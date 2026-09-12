#include "font_ttf.h"

#include <string.h>

#include <unordered_map>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_partition.h"

#include "ft2build.h"
#include FT_FREETYPE_H

static const char* TAG = "font";

// フラッシュ上のfontパーティション(生のTTF、partitions.csvを参照)のラベル。
static const char* FONT_PARTITION_LABEL = "font";
// esp_partition_subtype_t以外の独自サブタイプ(0x40)。partitions.csvと合わせる。
static const esp_partition_subtype_t FONT_PARTITION_SUBTYPE =
    (esp_partition_subtype_t)0x40;

static FT_Library s_library = nullptr;
static FT_Face    s_face    = nullptr;
static int        s_px      = 0;

static const void*                  s_mmap_ptr    = nullptr;
static esp_partition_mmap_handle_t  s_mmap_handle = 0;

// グリフ1文字ぶんを合成する一時バッファ。1画素ずつdrawPixel()を呼ぶと
// LovyanGFX側の固定費(クリップ判定・回転変換・書き込み窓の設定)が
// 画素数ぶん掛かるため、ここで合成してからpushImage()で一括転送する。
static uint16_t* s_glyph_buf      = nullptr;
static size_t    s_glyph_buf_px   = 0;

// ─────────────────────────────────────────────────────────────────────────────
// UTF-8を1文字読み進める。戻り値は進んだバイト数。
static size_t utf8Next(const std::string& s, size_t i, uint32_t* out) {
    const unsigned char c = (unsigned char)s[i];
    const size_t rest = s.size() - i;

    if (c < 0x80) {
        *out = c;
        return 1;
    }
    if ((c & 0xE0) == 0xC0 && rest >= 2) {
        *out = ((uint32_t)(c & 0x1F) << 6) | (uint32_t)(s[i + 1] & 0x3F);
        return 2;
    }
    if ((c & 0xF0) == 0xE0 && rest >= 3) {
        *out = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(s[i + 1] & 0x3F) << 6) |
               (uint32_t)(s[i + 2] & 0x3F);
        return 3;
    }
    if ((c & 0xF8) == 0xF0 && rest >= 4) {
        *out = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(s[i + 1] & 0x3F) << 12) |
               ((uint32_t)(s[i + 2] & 0x3F) << 6) | (uint32_t)(s[i + 3] & 0x3F);
        return 4;
    }
    *out = 0xFFFD;
    return 1;
}

// 直前と同じサイズなら再設定しない(FT_Set_Pixel_Sizesはグリフキャッシュを捨てる)。
static bool setPixelSize(int px) {
    if (s_face == nullptr || px <= 0) return false;
    if (s_px == px) return true;

    FT_Error err = FT_Set_Pixel_Sizes(s_face, 0, (FT_UInt)px);
    if (err != 0) {
        ESP_LOGW(TAG, "FT_Set_Pixel_Sizesが失敗: px=%d err=%d", px, (int)err);
        return false;
    }
    s_px = px;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// fontTtfDrawText()/fontTtfTextWidth()向けの汎用グリフキャッシュ。
//
// 実機計測でタイムライン全体の再描画604msのうち、文字の内訳はFreeTypeの
// ラスタライズが203ms、実際の転送は23msだった(2026-09-12)。ラスタライズ結果を
// (文字コード, ピクセルサイズ)単位でキャッシュし、同じ文字を再描画のたびに
// FreeTypeで作り直さないようにする。
//
// 既存の時計用キャッシュ(fontTtfCacheGlyphs()/fontTtfDrawCachedText())とは
// 別の仕組みとして共存させる(呼び出しが明示的な文字集合の事前キャッシュなのに対し、
// こちらは任意の文字列を描くたびに遅延して埋まっていく)。

// カバレッジ配列の置き場所。小さな確保を大量に行うとヒープが断片化するため、
// PSRAM上に一括のアリーナを確保し、そこから先頭詰めで切り出す。
static const size_t GLYPH_CACHE_ARENA_SIZE = 512 * 1024;

static uint8_t* s_glyph_cache_arena        = nullptr;
static size_t   s_glyph_cache_arena_used   = 0;
static bool     s_glyph_cache_arena_failed = false;

struct GenericGlyphEntry {
    int      width;
    int      rows;
    int      left;     // bitmap_left
    int      top;      // bitmap_top
    int      advance;
    uint32_t cov_offset;  // s_glyph_cache_arena内のオフセット、width*rowsバイト(行優先、詰め)
};

static std::unordered_map<uint64_t, GenericGlyphEntry> s_glyph_cache;

static inline uint64_t glyphCacheKey(uint32_t code, int px) {
    return ((uint64_t)code << 32) | (uint32_t)px;
}

// キャッシュに無ければnullptr。
static const GenericGlyphEntry* findGenericGlyph(uint32_t code, int px) {
    auto it = s_glyph_cache.find(glyphCacheKey(code, px));
    if (it == s_glyph_cache.end()) return nullptr;
    return &it->second;
}

// アリーナを遅延確保する。確保に失敗したら以後も再試行せずnullptrを返し続ける
// (呼び出し側はキャッシュを使わず、従来どおり毎回ラスタライズする経路へ落ちる)。
static uint8_t* ensureGlyphCacheArena() {
    if (s_glyph_cache_arena != nullptr) return s_glyph_cache_arena;
    if (s_glyph_cache_arena_failed) return nullptr;

    s_glyph_cache_arena = (uint8_t*)heap_caps_malloc(
        GLYPH_CACHE_ARENA_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_glyph_cache_arena == nullptr) {
        ESP_LOGW(TAG, "グリフキャッシュ用アリーナ(%u bytes)の確保に失敗、"
                      "キャッシュ無しで動作する",
                 (unsigned)GLYPH_CACHE_ARENA_SIZE);
        s_glyph_cache_arena_failed = true;
        return nullptr;
    }
    return s_glyph_cache_arena;
}

// FT_Load_Char(FT_LOAD_RENDER)直後のslotからキャッシュへ登録する。
// アリーナが無い(確保失敗)、または1文字がアリーナより大きい場合は何もしない
// (呼び出し側はFT側のビットマップから直接描くので、機能上は問題ない)。
static void cacheGenericGlyph(uint32_t code, int px, const FT_GlyphSlot slot) {
    uint8_t* arena = ensureGlyphCacheArena();
    if (arena == nullptr) return;

    const FT_Bitmap& bmp = slot->bitmap;
    const size_t w = (size_t)bmp.width;
    const size_t h = (size_t)bmp.rows;
    const size_t need = w * h;
    if (need == 0 || need > GLYPH_CACHE_ARENA_SIZE) return;

    if (s_glyph_cache_arena_used + need > GLYPH_CACHE_ARENA_SIZE) {
        // 満杯。凝ったLRUは入れず、キャッシュ全体を捨てて先頭から作り直す。
        ESP_LOGW(TAG, "グリフキャッシュのアリーナが満杯(%u件を破棄して作り直す)",
                 (unsigned)s_glyph_cache.size());
        s_glyph_cache.clear();
        s_glyph_cache_arena_used = 0;
    }

    uint8_t* dst = arena + s_glyph_cache_arena_used;
    for (size_t row = 0; row < h; row++) {
        const unsigned char* src = bmp.buffer + row * bmp.pitch;
        memcpy(dst + row * w, src, w);
    }

    GenericGlyphEntry entry;
    entry.width      = (int)w;
    entry.rows       = (int)h;
    entry.left       = slot->bitmap_left;
    entry.top        = slot->bitmap_top;
    entry.advance    = (int)(slot->advance.x >> 6);
    entry.cov_offset = (uint32_t)s_glyph_cache_arena_used;

    s_glyph_cache_arena_used += need;
    s_glyph_cache[glyphCacheKey(code, px)] = entry;
}

esp_err_t fontTtfInit() {
    if (s_face != nullptr) return ESP_OK;

    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, FONT_PARTITION_SUBTYPE, FONT_PARTITION_LABEL);
    if (part == nullptr) {
        ESP_LOGE(TAG, "fontパーティションが見つからない");
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t merr = esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA,
                                        &s_mmap_ptr, &s_mmap_handle);
    if (merr != ESP_OK) {
        ESP_LOGE(TAG, "fontパーティションのmmapに失敗: %s", esp_err_to_name(merr));
        return ESP_FAIL;
    }

    FT_Error err = FT_Init_FreeType(&s_library);
    if (err != 0) {
        ESP_LOGE(TAG, "FreeTypeの初期化に失敗: err=%d", (int)err);
        s_library = nullptr;
        return ESP_FAIL;
    }

    err = FT_New_Memory_Face(s_library, (const FT_Byte*)s_mmap_ptr, (FT_Long)part->size,
                             0, &s_face);
    if (err != 0) {
        ESP_LOGE(TAG, "TTFを開けない(fontパーティション、%lu bytes、err=%d)",
                 (unsigned long)part->size, (int)err);
        FT_Done_FreeType(s_library);
        s_library = nullptr;
        s_face    = nullptr;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "TTFを読み込んだ(fontパーティション、family=%s style=%s glyphs=%ld)",
             s_face->family_name ? s_face->family_name : "?",
             s_face->style_name ? s_face->style_name : "?",
             (long)s_face->num_glyphs);
    return ESP_OK;
}

bool fontTtfReady() {
    return s_face != nullptr;
}

int fontTtfTextWidth(const std::string& str, int px) {
    if (!setPixelSize(px)) return 0;

    int width = 0;
    size_t i = 0;
    while (i < str.size()) {
        uint32_t code = 0;
        i += utf8Next(str, i, &code);

        const GenericGlyphEntry* cached = findGenericGlyph(code, px);
        if (cached != nullptr) {
            width += cached->advance;
            continue;
        }

        if (FT_Load_Char(s_face, code, FT_LOAD_DEFAULT) != 0) continue;
        width += (int)(s_face->glyph->advance.x >> 6);
    }
    return width;
}

int fontTtfLineHeight(int px) {
    if (!setPixelSize(px)) return 0;
    const FT_Size_Metrics& m = s_face->size->metrics;
    return (int)((m.ascender - m.descender) >> 6);
}

// 前景色/背景色からアンチエイリアス合成した色を1px描く共通処理。
// 一括転送(pushImage())が使えないとき(グリフバッファの確保失敗時)だけの
// フォールバック経路から使う。
static inline void blendPixel(LovyanGFX* gfx, int x, int y, int cov,
                              uint32_t fore_rgb888, int fore_r, int fore_g, int fore_b,
                              int back_r, int back_g, int back_b) {
    if (cov == 0) return;
    if (cov == 255) {
        gfx->drawPixel(x, y, fore_rgb888);
        return;
    }
    const int r = (fore_r * cov + back_r * (255 - cov)) / 255;
    const int g = (fore_g * cov + back_g * (255 - cov)) / 255;
    const int b = (fore_b * cov + back_b * (255 - cov)) / 255;
    gfx->drawPixel(x, y, ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b);
}

// 前景色/背景色からアンチエイリアス合成した色をRGB565(rgb565_nonswapped、
// lgfx::rgb565_tと同じビット並び)で返す。cov==0は背景色そのもの
// (一括転送では矩形全体を書き込むため、透過扱いにはできない)。
static inline uint16_t blendPixel565(int cov, int fore_r, int fore_g, int fore_b,
                                     int back_r, int back_g, int back_b) {
    int r, g, b;
    if (cov == 0) {
        r = back_r; g = back_g; b = back_b;
    } else if (cov == 255) {
        r = fore_r; g = fore_g; b = fore_b;
    } else {
        r = (fore_r * cov + back_r * (255 - cov)) / 255;
        g = (fore_g * cov + back_g * (255 - cov)) / 255;
        b = (fore_b * cov + back_b * (255 - cov)) / 255;
    }
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// グリフ合成用バッファをpx画素ぶん確保する(足りなければ確保し直す)。
// 内部RAMを優先する(合成は1画素ずつの細かい書き込みの連続なので、
// PSRAMだとキャッシュ経由のアクセスが遅く効果が薄れるため)。
// 確保に失敗したらnullptrを返す。呼び出し側は従来のdrawPixel()経路へ
// フォールバックすること。
// 一時的な内訳計測。FreeTypeのラスタライズと転送のどちらが重いかを切り分ける。
// 切り分けが済んだら撤去する。
static uint64_t s_ft_us   = 0;
static uint64_t s_blit_us = 0;

void fontTtfProfileReset() {
    s_ft_us   = 0;
    s_blit_us = 0;
}

void fontTtfProfileGet(uint32_t* ft_us, uint32_t* blit_us) {
    if (ft_us != nullptr) *ft_us = (uint32_t)s_ft_us;
    if (blit_us != nullptr) *blit_us = (uint32_t)s_blit_us;
}

static uint16_t* ensureGlyphBuf(size_t px) {
    if (px == 0) return nullptr;
    if (s_glyph_buf != nullptr && s_glyph_buf_px >= px) return s_glyph_buf;

    if (s_glyph_buf != nullptr) {
        heap_caps_free(s_glyph_buf);
        s_glyph_buf    = nullptr;
        s_glyph_buf_px = 0;
    }

    uint16_t* buf = (uint16_t*)heap_caps_malloc(px * sizeof(uint16_t),
                                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf == nullptr) {
        buf = (uint16_t*)heap_caps_malloc(px * sizeof(uint16_t),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (buf == nullptr) {
        ESP_LOGW(TAG, "グリフバッファの確保に失敗(%u px)、drawPixel()経路へ切り替える",
                 (unsigned)px);
        return nullptr;
    }

    s_glyph_buf    = buf;
    s_glyph_buf_px = px;
    return s_glyph_buf;
}

void fontTtfDrawText(LovyanGFX* gfx, const std::string& str, int x, int y, int px,
                     uint32_t fore_rgb888, uint32_t back_rgb888,
                     lgfx::textdatum_t datum) {
    if (gfx == nullptr) return;
    if (!setPixelSize(px)) return;
    if (str.empty()) return;

    const FT_Size_Metrics& metrics = s_face->size->metrics;
    const int ascender  = (int)(metrics.ascender >> 6);
    const int descender = (int)(-metrics.descender >> 6);
    const int height    = ascender + descender;

    // datumは下位2bitが水平(0:左 1:中央 2:右)、それより上が
    // 垂直(0:上 4:中央 8:下 16:ベースライン)。
    const int d_h = (int)datum & 0x03;
    const int d_v = (int)datum & 0x1C;

    if (d_h != 0) {
        const int w = fontTtfTextWidth(str, px);
        x -= (d_h == 1) ? (w / 2) : w;
    }

    // ペンはベースラインに置く。yは指定された基準位置。
    int baseline = y + ascender;
    if (d_v == 4) {         // middle
        baseline = y + ascender - height / 2;
    } else if (d_v == 8) {  // bottom
        baseline = y - descender;
    } else if (d_v == 16) { // baseline
        baseline = y;
    }

    const int fore_r = (int)((fore_rgb888 >> 16) & 0xFF);
    const int fore_g = (int)((fore_rgb888 >> 8) & 0xFF);
    const int fore_b = (int)(fore_rgb888 & 0xFF);
    const int back_r = (int)((back_rgb888 >> 16) & 0xFF);
    const int back_g = (int)((back_rgb888 >> 8) & 0xFF);
    const int back_b = (int)(back_rgb888 & 0xFF);

    int pen_x = x;
    size_t i  = 0;

    gfx->startWrite();
    while (i < str.size()) {
        uint32_t code = 0;
        i += utf8Next(str, i, &code);

        // まずキャッシュを引く。当たればFT_Load_Charを呼ばずに済む
        // (実機計測でFreeTypeのラスタライズが203ms/604msを占めていたため)。
        int left = 0, top = 0, advance = 0, w = 0, h = 0;
        const unsigned char* cov_base  = nullptr;
        int                  cov_pitch = 0;

        const GenericGlyphEntry* cached = findGenericGlyph(code, px);
        if (cached != nullptr) {
            left      = cached->left;
            top       = cached->top;
            advance   = cached->advance;
            w         = cached->width;
            h         = cached->rows;
            cov_base  = s_glyph_cache_arena + cached->cov_offset;
            cov_pitch = w;
        } else {
            const int64_t ft0 = esp_timer_get_time();
            int ft_rc = FT_Load_Char(s_face, code, FT_LOAD_RENDER);
            s_ft_us += (uint64_t)(esp_timer_get_time() - ft0);
            if (ft_rc != 0) continue;

            const FT_GlyphSlot slot = s_face->glyph;
            const FT_Bitmap&   bmp  = slot->bitmap;
            left      = slot->bitmap_left;
            top       = slot->bitmap_top;
            advance   = (int)(slot->advance.x >> 6);
            w         = (int)bmp.width;
            h         = (int)bmp.rows;
            cov_base  = bmp.buffer;
            cov_pitch = bmp.pitch;

            // 描画はこのままslotのビットマップから行うので、登録の成否に
            // 関わらず今回の文字は問題なく描ける(外れが続いてもフォールバックする)。
            cacheGenericGlyph(code, px, slot);
        }

        const int gx = pen_x + left;
        const int gy = baseline - top;

        if (w <= 0 || h <= 0) {
            pen_x += advance;
            continue;
        }

        uint16_t* buf = ensureGlyphBuf((size_t)w * (size_t)h);
        if (buf == nullptr) {
            // バッファが確保できないときだけ、従来どおり1画素ずつ描く。
            for (int row = 0; row < h; row++) {
                const unsigned char* src = cov_base + (size_t)row * cov_pitch;
                for (int col = 0; col < w; col++) {
                    blendPixel(gfx, gx + col, gy + row, src[col], fore_rgb888,
                              fore_r, fore_g, fore_b, back_r, back_g, back_b);
                }
            }
        } else {
            const int64_t b0 = esp_timer_get_time();
            for (int row = 0; row < h; row++) {
                const unsigned char* src = cov_base + (size_t)row * cov_pitch;
                for (int col = 0; col < w; col++) {
                    buf[(size_t)row * w + col] =
                        blendPixel565(src[col], fore_r, fore_g, fore_b, back_r, back_g, back_b);
                }
            }
            gfx->pushImage(gx, gy, w, h, (const lgfx::rgb565_t*)buf);
            s_blit_us += (uint64_t)(esp_timer_get_time() - b0);
        }

        pen_x += advance;
    }
    gfx->endWrite();
}

// ─────────────────────────────────────────────────────────────────────────────
// 時計表示用のグリフキャッシュ。

struct CachedGlyph {
    uint32_t             code;
    int                  width;
    int                  rows;
    int                  left;    // bitmap_left
    int                  top;     // bitmap_top
    int                  advance;
    std::vector<uint8_t> cov;     // width*rows、行優先
};

static std::vector<CachedGlyph> s_cache;
static int s_cache_px        = 0;
static int s_cache_ascender  = 0;
static int s_cache_descender = 0;

static const CachedGlyph* findCachedGlyph(uint32_t code) {
    for (const auto& g : s_cache) {
        if (g.code == code) return &g;
    }
    return nullptr;
}

esp_err_t fontTtfCacheGlyphs(const char* chars, int px) {
    if (chars == nullptr) return ESP_ERR_INVALID_ARG;
    if (!fontTtfReady()) return ESP_ERR_INVALID_STATE;
    if (!setPixelSize(px)) return ESP_FAIL;

    s_cache.clear();
    s_cache_px        = px;
    const FT_Size_Metrics& metrics = s_face->size->metrics;
    s_cache_ascender  = (int)(metrics.ascender >> 6);
    s_cache_descender = (int)(-metrics.descender >> 6);

    std::string str(chars);
    size_t i = 0;
    while (i < str.size()) {
        uint32_t code = 0;
        i += utf8Next(str, i, &code);
        if (findCachedGlyph(code) != nullptr) continue;
        if (FT_Load_Char(s_face, code, FT_LOAD_RENDER) != 0) continue;

        const FT_GlyphSlot slot = s_face->glyph;
        const FT_Bitmap&   bmp  = slot->bitmap;

        CachedGlyph g;
        g.code    = code;
        g.width   = (int)bmp.width;
        g.rows    = (int)bmp.rows;
        g.left    = slot->bitmap_left;
        g.top     = slot->bitmap_top;
        g.advance = (int)(slot->advance.x >> 6);
        g.cov.resize((size_t)g.width * g.rows, 0);

        for (int row = 0; row < g.rows; row++) {
            const unsigned char* src = bmp.buffer + (size_t)row * bmp.pitch;
            for (int col = 0; col < g.width; col++) {
                g.cov[(size_t)row * g.width + col] = src[col];
            }
        }
        s_cache.push_back(std::move(g));
    }

    ESP_LOGI(TAG, "グリフキャッシュを作成した(%d文字、%dpx)", (int)s_cache.size(), px);
    return ESP_OK;
}

int fontTtfCachedTextWidth(const std::string& str) {
    int width = 0;
    size_t i  = 0;
    while (i < str.size()) {
        uint32_t code = 0;
        i += utf8Next(str, i, &code);
        const CachedGlyph* g = findCachedGlyph(code);
        if (g != nullptr) width += g->advance;
    }
    return width;
}

void fontTtfDrawCachedText(LovyanGFX* gfx, const std::string& str, int x, int y,
                           uint32_t fore_rgb888, uint32_t back_rgb888,
                           lgfx::textdatum_t datum) {
    if (gfx == nullptr) return;
    if (str.empty()) return;
    if (s_cache.empty()) return;

    const int ascender  = s_cache_ascender;
    const int descender = s_cache_descender;
    const int height    = ascender + descender;

    const int d_h = (int)datum & 0x03;
    const int d_v = (int)datum & 0x1C;

    if (d_h != 0) {
        const int w = fontTtfCachedTextWidth(str);
        x -= (d_h == 1) ? (w / 2) : w;
    }

    int baseline = y + ascender;
    if (d_v == 4) {
        baseline = y + ascender - height / 2;
    } else if (d_v == 8) {
        baseline = y - descender;
    } else if (d_v == 16) {
        baseline = y;
    }

    const int fore_r = (int)((fore_rgb888 >> 16) & 0xFF);
    const int fore_g = (int)((fore_rgb888 >> 8) & 0xFF);
    const int fore_b = (int)(fore_rgb888 & 0xFF);
    const int back_r = (int)((back_rgb888 >> 16) & 0xFF);
    const int back_g = (int)((back_rgb888 >> 8) & 0xFF);
    const int back_b = (int)(back_rgb888 & 0xFF);

    int pen_x = x;
    size_t i  = 0;

    gfx->startWrite();
    while (i < str.size()) {
        uint32_t code = 0;
        i += utf8Next(str, i, &code);

        const CachedGlyph* g = findCachedGlyph(code);
        if (g == nullptr) continue;

        const int gx = pen_x + g->left;
        const int gy = baseline - g->top;
        const int w  = g->width;
        const int h  = g->rows;

        if (w <= 0 || h <= 0) {
            pen_x += g->advance;
            continue;
        }

        uint16_t* buf = ensureGlyphBuf((size_t)w * (size_t)h);
        if (buf == nullptr) {
            // バッファが確保できないときだけ、従来どおり1画素ずつ描く。
            for (int row = 0; row < h; row++) {
                for (int col = 0; col < w; col++) {
                    const int cov = g->cov[(size_t)row * w + col];
                    blendPixel(gfx, gx + col, gy + row, cov, fore_rgb888,
                              fore_r, fore_g, fore_b, back_r, back_g, back_b);
                }
            }
        } else {
            for (int row = 0; row < h; row++) {
                for (int col = 0; col < w; col++) {
                    const int cov = g->cov[(size_t)row * w + col];
                    buf[(size_t)row * w + col] =
                        blendPixel565(cov, fore_r, fore_g, fore_b, back_r, back_g, back_b);
                }
            }
            gfx->pushImage(gx, gy, w, h, (const lgfx::rgb565_t*)buf);
        }

        pen_x += g->advance;
    }
    gfx->endWrite();
}
