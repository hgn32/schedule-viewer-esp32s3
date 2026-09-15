#include "display.h"

#include <algorithm>
#include <cstdio>

#include "esp_timer.h"

#include "font_ttf.h"
#include "lcd_panel.h"
#include "text_util.h"
#include "time_util.h"

// ─────────────────────────────────────────────────────────────────────────────
// 色(RGB888のuint32_t)。LovyanGFXはuint32_tをRGB888として解釈するため、
// 描画呼び出しではuint16_tを使わずここにまとめた値だけを使う。
static const uint32_t COLOR_BG               = 0x121212;
static const uint32_t COLOR_TEXT             = 0xE8E8E8;
static const uint32_t COLOR_MUTED_TEXT       = 0x9E9E9E;
static const uint32_t COLOR_HOUR_LINE        = 0x3A3A3A;
static const uint32_t COLOR_LABEL_DIVIDER    = 0x505050;
static const uint32_t COLOR_NOW_LINE         = 0xFF5252;
static const uint32_t COLOR_EVENT_FILL       = 0x263B52;
static const uint32_t COLOR_EVENT_BORDER     = 0x5B8DB8;
static const uint32_t COLOR_EVENT_TEXT       = 0xFFFFFF;
static const uint32_t COLOR_IN_PROGRESS_BAND = 0x4CAF50;
static const uint32_t COLOR_EMPH_L1          = 0xFFD54F;
static const uint32_t COLOR_EMPH_L2          = 0xFF9800;
static const uint32_t COLOR_EMPH_L3          = 0xF44336;
static const uint32_t COLOR_BLINK_TEXT       = 0x000000;

// 時計用グリフキャッシュに含める文字集合。
static const char* CLOCK_CHARS = "0123456789:";

// ─────────────────────────────────────────────────────────────────────────────
// FNV-1a(32bit)。衝突しても表示・点滅が1回ずれるだけなので暗号強度は要らない。
static uint32_t fnv1a(uint32_t h, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

// "HH:MM:SS"(JST)。
static std::string formatClockStr(uint32_t now_utc) {
    time_t jst_t = (time_t)(now_utc + JST_OFFSET);
    struct tm t;
    gmtime_r(&jst_t, &t);
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
    return std::string(buf);
}

static uint32_t emphasisColor(int level) {
    switch (level) {
        case 1: return COLOR_EMPH_L1;
        case 2: return COLOR_EMPH_L2;
        case 3: return COLOR_EMPH_L3;
        default: return COLOR_EVENT_BORDER;
    }
}

// sensitivityがnormal以外(personal / private / confidential)の予定は、
// タイムライン上では件名・場所を出さず鍵マークだけにする。壁掛けの画面を
// 他人に見られるため。内容はタップしたダイアログでだけ見せる。
static bool isSensitive(const Event& e) {
    return !e.sensitivity.empty() && e.sensitivity != "normal";
}

// ─────────────────────────────────────────────────────────────────────────────

int Display::clockRectW() {
    return fontTtfCachedTextWidth("00:00:00") + 8;
}

int Display::clockRectX() {
    return SCR_W - 20 - clockRectW();
}

int Display::emphasisLevel(uint32_t event_start_utc, uint32_t now_utc) {
    if (event_start_utc <= now_utc) return 0;
    uint32_t remain = event_start_utc - now_utc;
    if (remain <= EMPH_L3_SEC) return 3;
    if (remain <= EMPH_L2_SEC) return 2;
    if (remain <= EMPH_L1_SEC) return 1;
    return 0;
}

uint32_t Display::eventKey(const Event& e) {
    uint32_t h = 2166136261u;
    h = fnv1a(h, &e.start_utc, sizeof(e.start_utc));
    h = fnv1a(h, e.title.data(), e.title.size());
    return h;
}

int Display::findHistoryLevel(uint32_t key) const {
    for (const auto& r : _emphasis_history) {
        if (r.key == key) return r.level;
    }
    return -1;
}

void Display::addOrExtendBlink(uint32_t key, const LayoutEvent& le, int level,
                               uint32_t now_ms) {
    for (auto& b : _blinks) {
        if (b.key == key) {
            b.layout    = le;
            b.level     = level;
            b.expire_ms = now_ms + BLINK_DURATION_MS;
            return; // フェーズと次回トグル時刻は維持し、点滅を継続する
        }
    }

    const uint32_t start_ms = now_ms + BLINK_START_DELAY_MS;

    BlinkEntry b;
    b.key            = key;
    b.layout         = le;
    b.level          = level;
    b.expire_ms      = start_ms + BLINK_DURATION_MS;
    b.next_toggle_ms = start_ms;
    // 最初のトグルで強調側(phase=true)になるようfalseで登録する。
    // renderTimeline()が直前にphase=falseで描いているので、これで見た目が繋がる。
    b.phase          = false;
    _blinks.push_back(b);
}

uint32_t Display::contentSignature(const std::vector<Event>& events,
                                   const std::string& date_str,
                                   uint32_t start_min) const {
    uint32_t h = 2166136261u;
    h = fnv1a(h, date_str.data(), date_str.size());
    h = fnv1a(h, &start_min, sizeof(start_min));
    for (const auto& e : events) {
        h = fnv1a(h, &e.start_utc, sizeof(e.start_utc));
        h = fnv1a(h, &e.end_utc, sizeof(e.end_utc));
        h = fnv1a(h, e.title.data(), e.title.size());
        h = fnv1a(h, e.location.data(), e.location.size());
        const uint8_t tentative = e.is_tentative ? 1 : 0;
        h = fnv1a(h, &tentative, sizeof(tentative));
        h = fnv1a(h, e.sensitivity.data(), e.sensitivity.size());
    }
    return h;
}

// ─────────────────────────────────────────────────────────────────────────────

bool Display::begin() {
    _gfx = lcdPanelGfx();
    if (_gfx == nullptr) {
        return false;
    }

    _canvas.setPsram(true);
    // 転送先のフレームバッファと同じ並びにする。ここが食い違うとpushSprite()が
    // バイト入れ替えを伴う変換になり、色も速度も損なう(lcd_panel.cppのコメント参照)。
    _canvas.setColorDepth(lgfx::color_depth_t::rgb565_nonswapped);
    // パネルと同じ生の向き(1024x600)で確保し、回転はスプライト側に持たせる。
    // こうするとpushSprite()が同じ向きどうしの転送になり、行単位の連続コピーになる
    // (向きが食い違う回転を伴う転送は61万画素すべてがPSRAMのキャッシュラインを
    // またぐため極端に遅い。実測で全画面転送が832ms掛かっていた)。
    if (_canvas.createSprite(LCD_PHYS_W, LCD_PHYS_H) == nullptr) {
        return false;
    }
    // setRotation()はcreateSprite()の後に呼ぶこと。描画コードは論理座標
    // (SCR_W x SCR_H = 600x1024)のまま変更しない。
    _canvas.setRotation(LCD_ROTATION);
    _canvas.setTextWrap(false);

    esp_err_t err = fontTtfInit();
    if (err != ESP_OK) {
        return false;
    }

    err = fontTtfCacheGlyphs(CLOCK_CHARS, FS_CLOCK);
    if (err != ESP_OK) {
        return false;
    }

    return true;
}

void Display::showFatalMessage(const std::string& msg) {
    _has_rendered = false;

    if (_gfx == nullptr) {
        return;
    }

    // スプライトもTTFも当てにできない状況なので、LCDへ内蔵フォントで直接描く。
    // _gfxは回転なし(生の1024x600)なので、ここでだけ回転をかけて論理座標
    // (SCR_W x SCR_H = 600x1024)で描く。致命エラーの告知でそのまま停止する
    // 経路なので回転を戻す必要はない。
    _gfx->setRotation(LCD_ROTATION);
    _gfx->startWrite();
    _gfx->fillScreen(COLOR_BG);
    _gfx->setFont(&lgfx::fonts::efontJA_24_b);
    _gfx->setTextSize(1.5f);
    _gfx->setTextColor(COLOR_TEXT);
    _gfx->setTextDatum(lgfx::textdatum_t::middle_center);
    _gfx->drawString(msg.c_str(), SCR_W / 2, SCR_H / 2);
    _gfx->setTextDatum(lgfx::textdatum_t::top_left);
    _gfx->endWrite();
}

void Display::showBootMessage(const std::string& msg) {
    if (_gfx == nullptr) return;

    // 画面を丸ごと上書きするので、次のrenderTimeline()を「内容が同じ」で
    // 飛ばさせないように状態を捨てる。
    _has_rendered = false;
    _last_clock_str.clear();
    _emphasis_history.clear();
    _blinks.clear();
    _last_boxes.clear();
    _dialog_open = false;
    _band_h       = 0;

    // fontTtfDrawText()は1行ぶんしか描かない(改行を解釈せず、グリフとして
    // 描こうとして豆腐になる)。ここで行に分けてから1行ずつ中央へ置く。
    std::vector<std::string> lines;
    size_t start = 0;
    for (;;) {
        size_t nl = msg.find('\n', start);
        if (nl == std::string::npos) {
            lines.push_back(msg.substr(start));
            break;
        }
        lines.push_back(msg.substr(start, nl - start));
        start = nl + 1;
    }

    // 1行目は見出し(状態)なので大きく、2行目以降は補足情報なので小さく描く。
    int head_h = fontTtfLineHeight(FS_BOOT);
    if (head_h <= 0) head_h = FS_BOOT + 8;
    int sub_h = fontTtfLineHeight(FS_BOOT_SUB);
    if (sub_h <= 0) sub_h = FS_BOOT_SUB + 6;

    const int total_h = head_h + sub_h * ((int)lines.size() - 1);
    int y = SCR_H / 2 - total_h / 2 + head_h / 2;

    _gfx->startWrite();
    _canvas.fillScreen(COLOR_BG);
    for (size_t i = 0; i < lines.size(); i++) {
        if (!lines[i].empty()) {
            fontTtfDrawText(&_canvas, lines[i], SCR_W / 2, y, i == 0 ? FS_BOOT : FS_BOOT_SUB,
                            COLOR_TEXT, COLOR_BG, lgfx::textdatum_t::middle_center);
        }
        // 次の行の中心まで進む。1行目と2行目の間だけ行高が変わる。
        y += (i == 0) ? (head_h / 2 + sub_h / 2) : sub_h;
    }
    _canvas.pushSprite(_gfx, 0, 0);
    _gfx->endWrite();
}

// ─────────────────────────────────────────────────────────────────────────────

void Display::pushRect(int x, int y, int w, int h) {
    if (_gfx == nullptr) return;
    if (w <= 0 || h <= 0) return;

    // _gfxは回転なし(生の1024x600)なので、論理座標(600x1024)を生座標へ直してから
    // クリップを掛ける。変換式はLovyanGFXのPanel_FrameBufferBase::_rotate_pixelcopy()の
    // 回転1の扱いに合わせたもの(y反転 → x/yとw/hの入れ替え)。
    const int nx = SCR_H - (y + h);
    const int ny = x;
    const int nw = h;
    const int nh = w;

    _gfx->setClipRect(nx, ny, nw, nh);
    _canvas.pushSprite(_gfx, 0, 0);
    _gfx->clearClipRect();
}

void Display::drawClock(const std::string& time_str) {
    const int rw = clockRectW();
    const int rh = clockRectH();
    const int rx = clockRectX();
    const int ry = clockRectY();

    _canvas.fillRect(rx, ry, rw, rh, COLOR_BG);
    fontTtfDrawCachedText(&_canvas, time_str, SCR_W - 20, ry + rh / 2, COLOR_TEXT,
                          COLOR_BG, lgfx::textdatum_t::middle_right);
}

void Display::renderClock(uint32_t now_utc) {
    if (_gfx == nullptr) return;

    std::string time_str = formatClockStr(now_utc);
    if (time_str == _last_clock_str) return;

    _gfx->startWrite();
    drawClock(time_str);
    _gfx->endWrite();

    pushRect(clockRectX(), clockRectY(), clockRectW(), clockRectH());
    _last_clock_str = time_str;
}

void Display::drawHeader(const struct tm& jst_now, uint32_t now_utc) {
    // ヘッダーは本体と同じ背景色にし、下端の区切り線1本だけで本体と分ける
    // (面で背景色を分けるとヘッダーだけ浮いて見えるため)。
    _canvas.fillRect(0, 0, SCR_W, HEADER_H, COLOR_BG);

    // 日付・曜日は小さく左に、時計は大きく右に置いて主従をはっきりさせる。
    std::string date_str = formatDate(jst_now);
    fontTtfDrawText(&_canvas, date_str, 20, HEADER_H / 2, FS_HEADER, COLOR_TEXT,
                    COLOR_BG, lgfx::textdatum_t::middle_left);

    drawClock(formatClockStr(now_utc));

    _canvas.drawFastHLine(0, HEADER_H, SCR_W, COLOR_LABEL_DIVIDER);
}

void Display::drawHourGrid(uint32_t display_start_utc, uint32_t display_end_utc) {
    _canvas.drawFastVLine(LABEL_W - 1, timelineTop(), timelineH(), COLOR_LABEL_DIVIDER);

    // 12時間表示では30分刻みの補助線は間隔が狭すぎて密になるため、1時間線のみ引く。
    uint32_t t = (display_start_utc / 3600u) * 3600u;
    if (t < display_start_utc) t += 3600u;

    for (; t <= display_end_utc; t += 3600u) {
        int y = timelineTop() + (int)((t - display_start_utc) * pxPerSec());
        if (y < timelineTop() || y >= SCR_H) continue;

        _canvas.drawFastHLine(CONTENT_X, y, SCR_W - CONTENT_X, COLOR_HOUR_LINE);

        time_t jst_t = (time_t)(t + JST_OFFSET);
        struct tm ht;
        gmtime_r(&jst_t, &ht);
        fontTtfDrawText(&_canvas, formatHour(ht.tm_hour), LABEL_W / 2, y + 8,
                        FS_TICK, COLOR_MUTED_TEXT, COLOR_BG,
                        lgfx::textdatum_t::top_center);
    }
}

void Display::drawAllDayBand(const std::vector<Event>& all_day) {
    const int band_y = HEADER_H + 1;

    _canvas.fillRect(0, band_y, SCR_W, ALLDAY_BAND_H, COLOR_BG);

    // 左のラベル列。
    fontTtfDrawText(&_canvas, "終日", LABEL_W / 2, band_y + ALLDAY_BAND_H / 2, FS_ALLDAY,
                    COLOR_MUTED_TEXT, COLOR_BG, lgfx::textdatum_t::middle_center);

    // 右側を最大4件で等分する。5件以上あるときは最後の枠を"+N件"にする。
    const int max_slots = 4;
    const int n          = (int)all_day.size();
    const bool overflow  = n > max_slots;
    const int slots      = overflow ? max_slots : n;
    if (slots <= 0) return;

    const int area_w = SCR_W - CONTENT_X;
    const int slot_w = area_w / slots;

    for (int i = 0; i < slots; i++) {
        const int x0 = CONTENT_X + i * slot_w + 3;
        const int x1 = CONTENT_X + (i + 1) * slot_w - 3;
        const int y0 = band_y + 2;
        const int y1 = band_y + ALLDAY_BAND_H - 2;
        const int w  = x1 - x0;
        const int h  = y1 - y0;
        if (w < 4 || h < 4) continue;

        const bool is_overflow_slot = overflow && (i == slots - 1);

        _canvas.fillRoundRect(x0, y0, w, h, ALLDAY_RADIUS, COLOR_EVENT_FILL);
        _canvas.drawRoundRect(x0, y0, w, h, ALLDAY_RADIUS, COLOR_EVENT_BORDER);
        // 仮の予定はタイムラインの予定枠と同じく破線にして、確定の予定と見分ける。
        // 枠線の上へ描くので、この後のsetClipRect()より前に呼ぶこと
        // (クリップの内側だけになると破線が出ない)。
        if (!is_overflow_slot && all_day[i].is_tentative) {
            dashRoundRectEdges({x0, y0, w, h}, 1, COLOR_EVENT_FILL, ALLDAY_RADIUS);
        }

        _canvas.setClipRect(x0 + 1, y0 + 1, w - 2, h - 2);
        if (is_overflow_slot) {
            // 個別に描いた(slots - 1)件を除いた残り件数。
            char buf[16];
            snprintf(buf, sizeof(buf), "+%d件", n - (slots - 1));
            fontTtfDrawText(&_canvas, buf, x0 + w / 2, y0 + h / 2, FS_EVENT, COLOR_EVENT_TEXT,
                            COLOR_EVENT_FILL, lgfx::textdatum_t::middle_center);
            _canvas.clearClipRect();
            // 対応する予定が1件に決まらないため、タップ判定(_last_boxes)には積まない。
            continue;
        }

        const Event& e = all_day[i];
        if (isSensitive(e)) {
            // 壁掛けで内容を見せないため、件名の代わりに鍵マークを描く
            // (タイムラインの枠と同じ扱い)。
            if (h >= 15) {
                drawLockIcon(x0 + 4, y0 + (h - 11) / 2, COLOR_EVENT_TEXT);
            }
        } else {
            fontTtfDrawText(&_canvas, e.title, x0 + 4, y0 + h / 2, FS_EVENT, COLOR_EVENT_TEXT,
                            COLOR_EVENT_FILL, lgfx::textdatum_t::middle_left);
        }
        _canvas.clearClipRect();

        _last_boxes.push_back({{x0, y0, w, h}, e});
    }

    _canvas.drawFastHLine(0, band_y + ALLDAY_BAND_H, SCR_W, COLOR_LABEL_DIVIDER);
}

void Display::drawNowLineFull() {
    const int y_now = nowLineY();
    _canvas.fillRect(CONTENT_X, y_now, SCR_W - CONTENT_X, 2, COLOR_NOW_LINE);
    _canvas.fillCircle(CONTENT_X, y_now, 5, COLOR_NOW_LINE);
}

// ─────────────────────────────────────────────────────────────────────────────

Display::BoxRect Display::eventBoxRect(const LayoutEvent& le, uint32_t display_start_utc,
                                       uint32_t display_end_utc) const {
    BoxRect r;

    uint32_t clipped_start = std::max(le.event.start_utc, display_start_utc);
    uint32_t clipped_end   = std::min(le.event.end_utc, display_end_utc);
    if (clipped_start >= clipped_end) return r;

    const float px_sec = pxPerSec();
    const int   col_w  = CONTENT_W / le.total_cols;
    const int   pad    = 3; // 列(横)方向の隙間。左右3pxずつで隣接列との間隔は計6px

    int y_top = timelineTop() + (int)((clipped_start - display_start_utc) * px_sec);
    int y_bot = timelineTop() + (int)((clipped_end - display_start_utc) * px_sec);
    int x_left  = CONTENT_X + le.col * col_w + pad;
    int x_right = CONTENT_X + (le.col + 1) * col_w - pad;

    r.x = x_left;
    r.y = y_top;
    r.w = x_right - x_left;
    // 時間方向(縦)に隣接する予定が繋がって見えないよう、下端を2px削って隙間を作る。
    r.h = std::max(0, y_bot - y_top - 2);
    return r;
}

void Display::drawEventBox(const LayoutEvent& le, int level, bool blink_phase,
                           uint32_t now_utc, uint32_t display_start_utc) {
    BoxRect r = eventBoxRect(le, display_start_utc, display_start_utc + DISP_HOURS * 3600u);
    if (r.w < 4 || r.h < 4) return;

    const uint32_t fill_color   = blink_phase ? emphasisColor(level) : COLOR_EVENT_FILL;
    const uint32_t border_color = (level > 0) ? emphasisColor(level) : COLOR_EVENT_BORDER;
    const uint32_t text_color   = blink_phase ? COLOR_BLINK_TEXT : COLOR_EVENT_TEXT;
    const int      border_w    = (level > 0) ? 3 : 1;

    _canvas.fillRoundRect(r.x, r.y, r.w, r.h, 6, fill_color);
    for (int i = 0; i < border_w; i++) {
        int rad = 6 - i;
        if (rad < 0) rad = 0;
        _canvas.drawRoundRect(r.x + i, r.y + i, r.w - 2 * i, r.h - 2 * i, rad, border_color);
    }

    if (le.event.is_tentative) {
        dashRoundRectEdges(r, border_w, fill_color, 6); // drawEventBox()の角丸と同じ値
    }

    const bool in_progress = le.event.start_utc <= now_utc && now_utc < le.event.end_utc;
    if (in_progress) {
        _canvas.fillRect(r.x, r.y, 6, r.h, COLOR_IN_PROGRESS_BAND);
    }

    // 横方向は従来どおり(進行中は帯の幅6pxを加えて8px)。
    const int tx = r.x + (in_progress ? 6 : 0) + 8;

    // 縦方向はベースライン指定で置く。top_left指定だとhheaのascenderぶん
    // 下がりすぎて30分枠で2行目が切れるため、実インク上端・下端の実測値
    // (EVENT_INK_ASC/EVENT_INK_DESC、display.h参照)をもとにベースラインを直接決める。
    const int base_title = r.y + EVENT_PAD_TOP + EVENT_INK_ASC;
    const int base_loc   = base_title + EVENT_LINE_PITCH;
    const int clip_bottom = r.y + r.h - 2; // setClipRect()の下端と同じ

    _canvas.setClipRect(r.x + 1, r.y + 1, r.w - 2, r.h - 2);
    if (isSensitive(le.event)) {
        // 件名・場所は出さず、鍵マーク1つだけを描く(壁掛けの画面を他人に
        // 見られるため)。鍵は高さ11pxあるので、上下の余白が入らない枠には描かない。
        if (r.h >= 15) {
            drawLockIcon(tx, r.y + EVENT_PAD_TOP, text_color);
        }
    } else {
        // 件名: インク下端(base_title + EVENT_INK_DESC)が枠内に収まる高さがあれば描く。
        if (base_title + EVENT_INK_DESC <= clip_bottom) {
            fontTtfDrawText(&_canvas, le.event.title, tx, base_title, FS_EVENT, text_color,
                            fill_color, lgfx::textdatum_t::baseline_left);
        }
        // 場所: 同様にインク下端が収まる高さがあれば描く。
        if (base_loc + EVENT_INK_DESC <= clip_bottom && !le.event.location.empty()) {
            fontTtfDrawText(&_canvas, le.event.location, tx, base_loc, FS_EVENT, text_color,
                            fill_color, lgfx::textdatum_t::baseline_left);
        }
    }
    _canvas.clearClipRect();
}

void Display::drawLockIcon(int x, int y, uint32_t color) {
    _canvas.drawFastVLine(x + 2, y + 1, 4, color);
    _canvas.drawFastVLine(x + 7, y + 1, 4, color);
    _canvas.drawFastHLine(x + 3, y, 4, color);
    _canvas.fillRoundRect(x, y + 5, 10, 6, 1, color);
}

void Display::dashRoundRectEdges(const BoxRect& r, int border_w, uint32_t gap_color,
                                 int radius) {
    // radiusは呼び出し元のfillRoundRect()/drawRoundRect()と同じ値でなければならない。
    const int period = DASH_ON_PX + DASH_OFF_PX;

    const int x0 = r.x + radius;
    const int x1 = r.x + r.w - radius;
    for (int x = x0 + DASH_ON_PX; x < x1; x += period) {
        const int w = std::min(DASH_OFF_PX, x1 - x);
        _canvas.fillRect(x, r.y, w, border_w, gap_color);
        _canvas.fillRect(x, r.y + r.h - border_w, w, border_w, gap_color);
    }

    const int y0 = r.y + radius;
    const int y1 = r.y + r.h - radius;
    for (int y = y0 + DASH_ON_PX; y < y1; y += period) {
        const int h = std::min(DASH_OFF_PX, y1 - y);
        _canvas.fillRect(r.x, y, border_w, h, gap_color);
        _canvas.fillRect(r.x + r.w - border_w, y, border_w, h, gap_color);
    }
}

void Display::redrawBoxAndNowLine(const LayoutEvent& le, int level, bool blink_phase,
                                  uint32_t now_utc) {
    BoxRect r = eventBoxRect(le, _last_display_start_utc, _last_display_end_utc);
    if (r.w < 4 || r.h < 4) return;

    _gfx->startWrite();
    drawEventBox(le, level, blink_phase, now_utc, _last_display_start_utc);

    const int  y_now  = nowLineY();
    const bool crosses = (y_now + 1 >= r.y) && (y_now <= r.y + r.h - 1);

    int push_x = r.x;
    int push_w = r.w;
    if (crosses) {
        _canvas.fillRect(r.x, y_now, r.w, 2, COLOR_NOW_LINE);
        if (le.col == 0) {
            _canvas.fillCircle(CONTENT_X, y_now, 5, COLOR_NOW_LINE);
            int left  = std::min(r.x, CONTENT_X - 5);
            int right = std::max(r.x + r.w, CONTENT_X + 5);
            push_x = left;
            push_w = right - left;
        }
    }
    _gfx->endWrite();

    pushRect(push_x, r.y, push_w, r.h);
}

// ─────────────────────────────────────────────────────────────────────────────

std::vector<LayoutEvent> Display::layoutEvents(std::vector<Event> events) {
    std::vector<int>      cols(events.size(), 0);
    std::vector<uint32_t> col_ends;

    for (size_t i = 0; i < events.size(); i++) {
        int c = 0;
        while (c < (int)col_ends.size() && col_ends[c] > events[i].start_utc) {
            c++;
        }
        cols[i] = c;
        if (c < (int)col_ends.size()) {
            col_ends[c] = events[i].end_utc;
        } else {
            col_ends.push_back(events[i].end_utc);
        }
    }

    std::vector<int> totals(events.size(), 1);
    for (size_t i = 0; i < events.size(); i++) {
        for (size_t j = 0; j < events.size(); j++) {
            bool overlaps = events[j].start_utc < events[i].end_utc &&
                            events[j].end_utc   > events[i].start_utc;
            if (overlaps) {
                totals[i] = std::max(totals[i], cols[j] + 1);
            }
        }
    }

    std::vector<LayoutEvent> result;
    result.reserve(events.size());
    for (size_t i = 0; i < events.size(); i++) {
        result.push_back({events[i], cols[i], totals[i]});
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────

bool Display::renderTimeline(ScheduleStore& store, uint32_t now_utc) {
    if (_gfx == nullptr) return false;

    const uint32_t display_start_utc = now_utc - NOW_OFFSET_SEC;
    const uint32_t display_end_utc   = display_start_utc + (uint32_t)DISP_HOURS * 3600u;

    time_t jst_t = (time_t)(now_utc + JST_OFFSET);
    struct tm jst_now;
    gmtime_r(&jst_t, &jst_now);
    std::string date_str = formatDate(jst_now);

    auto events = store.getInRange(display_start_utc, display_end_utc);

    uint32_t sig = contentSignature(events, date_str, display_start_utc / 60u);
    if (_has_rendered && sig == _last_signature) {
        return false;
    }

    _last_display_start_utc = display_start_utc;
    _last_display_end_utc   = display_end_utc;

    // 終日予定は12時間タイムラインには載せず、ヘッダー直下の帯に出す。
    // 帯の高さはレイアウト計算(pxPerSec()等が_band_hに依存する)より前に決めること。
    std::vector<Event> all_day;
    std::vector<Event> timed;
    for (const auto& e : events) {
        if (e.is_all_day) {
            all_day.push_back(e);
        } else {
            timed.push_back(e);
        }
    }
    _band_h = all_day.empty() ? 0 : ALLDAY_BAND_H;

    // 列の割り当てと強調・明滅の登録は時刻ありの予定だけで行う
    // (終日予定は開始が00:00で強調段階に入らないうえ、帯は部分更新の対象外のため)。
    auto layout = layoutEvents(timed);

    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    std::vector<EmphasisRecord> new_history;
    new_history.reserve(layout.size());

    _gfx->startWrite();
    _canvas.fillScreen(COLOR_BG);

    drawHeader(jst_now, now_utc);

    _last_boxes.clear();
    if (!all_day.empty()) {
        drawAllDayBand(all_day);
    }
    drawHourGrid(display_start_utc, display_end_utc);

    for (const auto& le : layout) {
        uint32_t key       = eventKey(le.event);
        int      level     = emphasisLevel(le.event.start_utc, now_utc);
        int      old_level = findHistoryLevel(key);

        // 仮の予定は明滅させない(枠色と破線だけで示す)。
        if (level > 0 && !le.event.is_tentative && (old_level < 0 || old_level != level)) {
            addOrExtendBlink(key, le, level, now_ms);
        }
        new_history.push_back({key, level});

        // タップのヒット判定に使うため、実際に描画される枠だけを記録する
        // (drawEventBox()と同じ打ち切り条件)。
        BoxRect r = eventBoxRect(le, display_start_utc, display_end_utc);
        if (!(r.w < 4 || r.h < 4)) {
            _last_boxes.push_back({r, le.event});
        }

        drawEventBox(le, level, /*blink_phase=*/false, now_utc, display_start_utc);
    }
    _emphasis_history = std::move(new_history);

    drawNowLineFull();

    _canvas.pushSprite(_gfx, 0, 0);
    _gfx->endWrite();

    _has_rendered   = true;
    _last_signature = sig;
    _last_clock_str = formatClockStr(now_utc);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────

void Display::tickBlink(uint32_t now_utc, uint32_t now_ms) {
    if (_gfx == nullptr) return;

    for (size_t i = 0; i < _blinks.size();) {
        BlinkEntry& b = _blinks[i];

        bool expired = now_ms >= b.expire_ms;
        bool toggle  = now_ms >= b.next_toggle_ms;
        if (!expired && !toggle) {
            i++;
            continue;
        }

        bool phase = expired ? false : !b.phase;
        if (!expired) {
            b.phase          = phase;
            b.next_toggle_ms += BLINK_HALF_PERIOD_MS;
        }

        // ダイアログ表示中は描画だけを飛ばす。フェーズ・次回トグル時刻・期限切れの
        // 削除は進めておく(止めると、閉じた直後に一斉に明滅が再発するため)。
        if (!_dialog_open) {
            redrawBoxAndNowLine(b.layout, b.level, phase, now_utc);
        }

        if (expired) {
            _blinks.erase(_blinks.begin() + (long)i);
        } else {
            i++;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────

std::vector<std::string> Display::wrapText(const std::string& src, int px, int max_w,
                                           int max_lines) const {
    std::vector<std::string> lines;
    if (src.empty()) return lines;

    std::string cur;
    int cur_w = 0;
    size_t i = 0;
    while (i < src.size()) {
        uint32_t cp = 0;
        size_t adv = utf8NextChar(src, i, &cp);
        if (adv == 0) break; // srcが空でない限り起こらない安全策

        std::string ch = src.substr(i, adv);
        int ch_w = fontTtfTextWidth(ch, px);

        if (!cur.empty() && cur_w + ch_w > max_w) {
            lines.push_back(cur);
            if ((int)lines.size() >= max_lines) {
                // 残りは全部捨てる。末尾が伸びてmax_wを超えても構わない。
                lines.back() += "...";
                return lines;
            }
            cur.clear();
            cur_w = 0;
        }

        cur += ch;
        cur_w += ch_w;
        i += adv;
    }

    if (!cur.empty()) {
        lines.push_back(cur);
    }
    return lines;
}

namespace {

// ダイアログの1行分(文字列・フォントサイズ・色・行の高さ)。
// 行の組み立て(文字列作成と折り返し)と実際の描画を2段に分けるための中間表現。
struct DialogLine {
    std::string text;
    int         px;
    uint32_t    color;
    int         height;
};

// partsを" / "で連結する。空の要素は無い前提(呼び出し側で詰めてから渡す)。
std::string joinWithSlash(const std::vector<std::string>& parts) {
    std::string out;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i > 0) out += " / ";
        out += parts[i];
    }
    return out;
}

// showAs / responseStatusの日本語化。
// 表に無い値は生の値をそのまま返す(APIが増えても情報が消えないように)。
// showAsはラベルを付けず、値そのものが説明になるようにする("通常予定"等)。
std::string showAsLabel(const std::string& v) {
    if (v == "busy") return "通常予定";
    if (v == "tentative") return "仮予定";
    if (v == "free") return "空き予定";
    if (v == "oof") return "不在";
    if (v == "workingElsewhere") return "別の場所で作業";
    return v;
}
std::string responseStatusLabel(const std::string& v) {
    if (v == "accepted") return "承諾";
    if (v == "tentativelyAccepted") return "仮承諾";
    if (v == "declined") return "辞退";
    if (v == "notResponded") return "未回答";
    if (v == "organizer") return "主催";
    return v;
}

} // namespace

void Display::drawEventDialog(const Event& event, int level) {
    const int inner_w = DLG_W - DLG_PAD * 2;

    auto lineHeight = [](int px) {
        int h = fontTtfLineHeight(px);
        return h > 0 ? h : px + 6;
    };

    // ─ 行を積む(高さを先に確定させてから描く) ─
    std::vector<DialogLine> lines;

    auto addWrapped = [&](const std::string& text, int px, uint32_t color, int max_lines) {
        if (text.empty()) return;
        for (const auto& l : wrapText(text, px, inner_w, max_lines)) {
            lines.push_back({l, px, color, lineHeight(px)});
        }
    };

    // 1行目: "14:15 - 14:30 (1時間30分)"(JST)。所要時間が0分なら括弧ごと省く。
    time_t start_t = (time_t)(event.start_utc + JST_OFFSET);
    time_t end_t   = (time_t)(event.end_utc + JST_OFFSET);
    struct tm st = {}, et = {};
    gmtime_r(&start_t, &st);
    gmtime_r(&end_t, &et);
    const uint32_t dur_sec =
        (event.end_utc > event.start_utc) ? (event.end_utc - event.start_utc) : 0;
    const int dur_min = (int)(dur_sec / 60);
    char time_buf[64];
    if (dur_min > 0) {
        char dur_buf[24];
        if (dur_min % 60 == 0) {
            snprintf(dur_buf, sizeof(dur_buf), "%d時間", dur_min / 60);
        } else if (dur_min < 60) {
            snprintf(dur_buf, sizeof(dur_buf), "%d分", dur_min);
        } else {
            snprintf(dur_buf, sizeof(dur_buf), "%d時間%d分", dur_min / 60, dur_min % 60);
        }
        snprintf(time_buf, sizeof(time_buf), "%02d:%02d - %02d:%02d (%s)", st.tm_hour, st.tm_min,
                 et.tm_hour, et.tm_min, dur_buf);
    } else {
        snprintf(time_buf, sizeof(time_buf), "%02d:%02d - %02d:%02d", st.tm_hour, st.tm_min,
                 et.tm_hour, et.tm_min);
    }
    lines.push_back({time_buf, FS_DLG_TIME, COLOR_TEXT, lineHeight(FS_DLG_TIME)});

    // 2行目: 件名(最大2行)
    addWrapped(event.title, FS_DLG_TITLE, COLOR_EVENT_TEXT, DLG_TITLE_MAX_LINES);

    // 3行目: 場所(値だけ。ラベルは付けない。最大2行)
    if (!event.location.empty()) {
        addWrapped(event.location, FS_DLG_SUB, COLOR_MUTED_TEXT, DLG_LOC_MAX_LINES);
    }

    // 4行目: 開催者(最大1行)。自分が開催者なら"開催者：自分"にする。
    // コロンは全角(半角だと時刻表記と紛らわしいため)。
    if (!event.organizer.empty()) {
        const std::string text =
            event.is_organizer ? "開催者：自分" : ("開催者：" + event.organizer);
        addWrapped(text, FS_DLG_SUB, COLOR_MUTED_TEXT, 1);
    }

    // 5行目: 状態 / 出欠 / 繰り返し / 下書き / 分類を" / "で連結(最大2行)。
    // 状態(showAs)はラベルを付けない。出欠・分類はラベル+全角コロン。
    // response_status=noneと空は今までどおり項目ごと出さない。
    {
        std::vector<std::string> parts;
        if (!event.show_as.empty()) {
            parts.push_back(showAsLabel(event.show_as));
        }
        if (!event.response_status.empty() && event.response_status != "none") {
            parts.push_back("出欠：" + responseStatusLabel(event.response_status));
        }
        if (event.is_recurring) parts.push_back("繰り返し");
        if (event.is_draft) parts.push_back("下書き");
        if (!event.categories.empty()) parts.push_back("分類：" + event.categories);
        addWrapped(joinWithSlash(parts), FS_DLG_SUB, COLOR_MUTED_TEXT, 2);
    }

    int header_h = 0;
    for (const auto& l : lines) header_h += l.height;

    // ─ 6行目(区切り線+本文) ─
    // ダイアログの高さがDLG_MAX_Hを超えない範囲で本文を入るだけ入れる。
    // 入り切らなかったら最後に描いた行の末尾へ"..."を付ける。
    const int  divider_h = 6 + 1 + 6; // 上下の余白6px+線1px
    bool       show_divider = false;
    std::vector<DialogLine> body_lines;

    if (!event.body.empty()) {
        const int available = DLG_MAX_H - DLG_PAD * 2 - header_h - divider_h;
        if (available > 0) {
            const int body_line_h  = lineHeight(FS_DLG_BODY);
            const int empty_line_h = FS_DLG_BODY / 2;
            // 折り返し後の行数はここでは制限せず、高さで打ち切るための大きな値を渡す。
            const int kNoWrapLimit = 999;

            std::vector<std::string> raw_lines;
            {
                size_t pos = 0;
                while (true) {
                    const size_t nl = event.body.find('\n', pos);
                    if (nl == std::string::npos) {
                        raw_lines.push_back(event.body.substr(pos));
                        break;
                    }
                    raw_lines.push_back(event.body.substr(pos, nl - pos));
                    pos = nl + 1;
                }
            }

            int  used    = 0;
            bool stopped = false;
            for (size_t li = 0; li < raw_lines.size() && !stopped; li++) {
                const std::string& raw_line = raw_lines[li];
                if (raw_line.empty()) {
                    if (used + empty_line_h > available) break;
                    body_lines.push_back({"", FS_DLG_BODY, COLOR_EVENT_TEXT, empty_line_h});
                    used += empty_line_h;
                    continue;
                }
                for (const auto& wl : wrapText(raw_line, FS_DLG_BODY, inner_w, kNoWrapLimit)) {
                    if (used + body_line_h > available) {
                        stopped = true;
                        if (!body_lines.empty()) {
                            std::string& last = body_lines.back().text;
                            if (last.size() < 3 || last.substr(last.size() - 3) != "...") {
                                last += "...";
                            }
                        }
                        break;
                    }
                    body_lines.push_back({wl, FS_DLG_BODY, COLOR_EVENT_TEXT, body_line_h});
                    used += body_line_h;
                }
            }
            show_divider = !body_lines.empty();
        }
    }

    int body_h = 0;
    for (const auto& l : body_lines) body_h += l.height;

    const int total_h = header_h + (show_divider ? divider_h : 0) + body_h;
    const int h = total_h + DLG_PAD * 2;
    const int x = (SCR_W - DLG_W) / 2;
    // ヘッダー(y < HEADER_H)には絶対に掛けないこと。毎秒の時計の部分更新が
    // ダイアログを壊すため。
    int y = HEADER_H + (SCR_H - HEADER_H - h) / 2;
    if (y < HEADER_H + 8) y = HEADER_H + 8;

    const uint32_t border_color = emphasisColor(level);

    _gfx->startWrite();

    _canvas.fillRoundRect(x, y, DLG_W, h, DLG_RADIUS, COLOR_EVENT_FILL);
    for (int i = 0; i < DLG_BORDER_W; i++) {
        int rad = DLG_RADIUS - i;
        if (rad < 0) rad = 0;
        _canvas.drawRoundRect(x + i, y + i, DLG_W - 2 * i, h - 2 * i, rad, border_color);
    }

    int ty = y + DLG_PAD;
    for (const auto& l : lines) {
        if (!l.text.empty()) {
            fontTtfDrawText(&_canvas, l.text, x + DLG_PAD, ty, l.px, l.color, COLOR_EVENT_FILL,
                            lgfx::textdatum_t::top_left);
        }
        ty += l.height;
    }

    if (show_divider) {
        ty += 6;
        _canvas.drawFastHLine(x + DLG_PAD, ty, inner_w, COLOR_LABEL_DIVIDER);
        ty += 1 + 6;
    }

    for (const auto& l : body_lines) {
        if (!l.text.empty()) {
            fontTtfDrawText(&_canvas, l.text, x + DLG_PAD, ty, l.px, l.color, COLOR_EVENT_FILL,
                            lgfx::textdatum_t::top_left);
        }
        ty += l.height;
    }

    _gfx->endWrite();

    pushRect(x, y, DLG_W, h);
}

bool Display::openEventDialog(int x, int y, uint32_t now_utc) {
    if (_gfx == nullptr || !_has_rendered || _dialog_open) return false;

    // 15分の予定は枠が17pxしかなく指で狙いにくいため、Y方向はDLG_HIT_MARGINぶん
    // 上下に広げてヒット判定する。複数候補があればタップ位置に近い中心を選ぶ。
    const HitBox* best    = nullptr;
    int           best_dy = 0;
    for (const auto& hb : _last_boxes) {
        const BoxRect& r = hb.rect;
        if (x < r.x || x >= r.x + r.w) continue;
        if (y < r.y - DLG_HIT_MARGIN || y >= r.y + r.h + DLG_HIT_MARGIN) continue;

        const int center_y = r.y + r.h / 2;
        const int dy        = (y > center_y) ? (y - center_y) : (center_y - y);
        if (best == nullptr || dy < best_dy) {
            best    = &hb;
            best_dy = dy;
        }
    }
    if (best == nullptr) return false;

    const int level = emphasisLevel(best->event.start_utc, now_utc);
    drawEventDialog(best->event, level);
    _dialog_open = true;
    return true;
}

void Display::closeEventDialog() {
    _dialog_open = false;
    // 次のrenderTimeline()が「内容が同じ」でスキップするとダイアログが残るので
    // 状態を捨てる(_emphasis_history/_blinksは消さない。消すと閉じた直後に
    // 明滅が再発する)。
    _has_rendered = false;
    _last_clock_str.clear();
}
