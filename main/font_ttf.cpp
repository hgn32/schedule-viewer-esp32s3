#include "font_ttf.h"

#include <string.h>

#include <vector>

#include "esp_log.h"
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

        if (FT_Load_Char(s_face, code, FT_LOAD_RENDER) != 0) continue;

        const FT_GlyphSlot slot = s_face->glyph;
        const FT_Bitmap&   bmp  = slot->bitmap;
        const int gx = pen_x + slot->bitmap_left;
        const int gy = baseline - slot->bitmap_top;

        for (unsigned int row = 0; row < bmp.rows; row++) {
            const unsigned char* src = bmp.buffer + (size_t)row * bmp.pitch;
            // 1px右にもう1回描いて太らせる。2度塗りするとアンチエイリアスの
            // 縁が濁るので、左隣のカバレッジとの最大値を取って1回で塗る。
            // glyphの右端に1px分はみ出すため幅を1つ広げる。
            for (unsigned int col = 0; col < bmp.width + 1; col++) {
                const int cov_here = (col < bmp.width) ? src[col] : 0;
                const int cov_left = (col > 0) ? src[col - 1] : 0;
                const int cov = (cov_here > cov_left) ? cov_here : cov_left;
                blendPixel(gfx, gx + (int)col, gy + (int)row, cov, fore_rgb888,
                          fore_r, fore_g, fore_b, back_r, back_g, back_b);
            }
        }

        pen_x += (int)(slot->advance.x >> 6);
    }
    gfx->endWrite();
}

// ─────────────────────────────────────────────────────────────────────────────
// 時計表示用のグリフキャッシュ。

struct CachedGlyph {
    uint32_t             code;
    int                  width;   // 太らせ後の幅
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
        g.width   = (int)bmp.width + 1; // 太らせ分の1pxを含む
        g.rows    = (int)bmp.rows;
        g.left    = slot->bitmap_left;
        g.top     = slot->bitmap_top;
        g.advance = (int)(slot->advance.x >> 6);
        g.cov.resize((size_t)g.width * g.rows, 0);

        for (int row = 0; row < g.rows; row++) {
            const unsigned char* src = bmp.buffer + (size_t)row * bmp.pitch;
            for (int col = 0; col < g.width; col++) {
                const int cov_here = (col < (int)bmp.width) ? src[col] : 0;
                const int cov_left = (col > 0) ? src[col - 1] : 0;
                g.cov[(size_t)row * g.width + col] =
                    (uint8_t)((cov_here > cov_left) ? cov_here : cov_left);
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

        for (int row = 0; row < g->rows; row++) {
            for (int col = 0; col < g->width; col++) {
                const int cov = g->cov[(size_t)row * g->width + col];
                blendPixel(gfx, gx + col, gy + row, cov, fore_rgb888,
                          fore_r, fore_g, fore_b, back_r, back_g, back_b);
            }
        }

        pen_x += g->advance;
    }
    gfx->endWrite();
}
