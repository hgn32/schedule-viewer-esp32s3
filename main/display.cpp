#include "display.h"

#include <algorithm>
#include <cstdio>

#include "esp_timer.h"

#include "font_ttf.h"
#include "lcd_panel.h"
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

// ─────────────────────────────────────────────────────────────────────────────

int Display::nowLineY() {
    return HEADER_H + (int)(NOW_OFFSET_SEC * pxPerSec());
}

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
    _canvas.drawFastVLine(LABEL_W - 1, HEADER_H, SCR_H - HEADER_H, COLOR_LABEL_DIVIDER);

    // 12時間表示では30分刻みの補助線は間隔が狭すぎて密になるため、1時間線のみ引く。
    uint32_t t = (display_start_utc / 3600u) * 3600u;
    if (t < display_start_utc) t += 3600u;

    for (; t <= display_end_utc; t += 3600u) {
        int y = HEADER_H + (int)((t - display_start_utc) * pxPerSec());
        if (y < HEADER_H || y >= SCR_H) continue;

        _canvas.drawFastHLine(CONTENT_X, y, SCR_W - CONTENT_X, COLOR_HOUR_LINE);

        time_t jst_t = (time_t)(t + JST_OFFSET);
        struct tm ht;
        gmtime_r(&jst_t, &ht);
        fontTtfDrawText(&_canvas, formatHour(ht.tm_hour), LABEL_W / 2, y + 8,
                        FS_TICK, COLOR_MUTED_TEXT, COLOR_BG,
                        lgfx::textdatum_t::top_center);
    }
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

    int y_top = HEADER_H + (int)((clipped_start - display_start_utc) * px_sec);
    int y_bot = HEADER_H + (int)((clipped_end - display_start_utc) * px_sec);
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
        dashRoundRectEdges(r, border_w, fill_color);
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
    _canvas.clearClipRect();
}

void Display::dashRoundRectEdges(const BoxRect& r, int border_w, uint32_t gap_color) {
    const int radius = 6; // drawEventBox()のfillRoundRect()/drawRoundRect()と同じ値
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

    auto layout = layoutEvents(events);

    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    std::vector<EmphasisRecord> new_history;
    new_history.reserve(layout.size());

    _gfx->startWrite();
    _canvas.fillScreen(COLOR_BG);

    drawHeader(jst_now, now_utc);
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

        redrawBoxAndNowLine(b.layout, b.level, phase, now_utc);

        if (expired) {
            _blinks.erase(_blinks.begin() + (long)i);
        } else {
            i++;
        }
    }
}
