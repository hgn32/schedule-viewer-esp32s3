#pragma once
#include <LovyanGFX.hpp>

#include <string>
#include <vector>

#include "schedule.h"

// レイアウト計算済みの1件の予定。同じ時間帯に重なる予定は列に分けて並べる。
struct LayoutEvent {
    Event event;
    int col;
    int total_cols;
};

// Waveshare ESP32-S3-Touch-LCD-7B(1024x600 RGB LCD)へのタイムライン描画。
// 画面は縦置き(論理600x1024)で使う。SDカードもE-Inkも無いので、
// 描画はPSRAM上のスプライトへ行い、完成した領域だけをLCDのフレームバッファへ
// pushSprite()で転送する(部分更新はsetClipRect()で範囲を絞る)。
class Display {
public:
    // lcdPanelGfx()がnullptrなら失敗。描画用スプライト(600x1024 RGB565、PSRAM)の
    // 確保、TTFの読み込み、時計用グリフキャッシュの作成まで行う。
    // どれか1つでも失敗したらfalseを返す(内蔵フォントへの退避はしない)。
    bool begin();

    // 致命的な失敗の告知。スプライトを介さずLCDへ内蔵フォント(efontJA)で直接描く。
    // begin()が失敗した後でも使えるように、TTFもスプライトも使わない。
    void showFatalMessage(const std::string& msg);

    // 全画面メッセージ(起動段階の表示に使う)。次回のrenderTimeline()を
    // 「内容が同じ」で飛ばさせないよう、署名・点滅・強調履歴の状態を捨てる。
    void showBootMessage(const std::string& msg);

    // 6時間タイムラインを描く。表示窓は[now_utc - NOW_OFFSET_SEC, +DISP_HOURS時間]。
    // 現在時刻線は常に画面上の同じY座標(HEADER_H + NOW_OFFSET_SEC分)に来る。
    // 前回と画面内容が完全に一致する場合は何も描かずfalseを返す。描いたらtrue。
    bool renderTimeline(ScheduleStore& store, uint32_t now_utc);

    // ヘッダ右の時計(HH:MM:SS)だけを部分更新する。前回と同じ文字列なら何もしない。
    void renderClock(uint32_t now_utc);

    // 強調表示に入った予定枠の点滅を1ティック分進める。BLINK_HALF_PERIOD_MSごとに
    // フェーズを反転して該当矩形だけ再描画する。BLINK_DURATION_MSを過ぎたら
    // 通常表示(点滅なしの強調色)に戻して点滅対象から外す。
    void tickBlink(uint32_t now_utc, uint32_t now_ms);

private:
    struct BoxRect {
        int x = 0, y = 0, w = 0, h = 0;
    };

    struct EmphasisRecord {
        uint32_t key;
        int      level;
    };

    struct BlinkEntry {
        uint32_t    key;
        LayoutEvent layout;
        int         level;
        uint32_t    expire_ms;
        uint32_t    next_toggle_ms;
        bool        phase;
    };

    // ─ 描画の内訳 ─
    void drawHeader(const struct tm& jst_now, uint32_t now_utc);
    void drawClock(const std::string& time_str);
    void drawHourGrid(uint32_t display_start_utc, uint32_t display_end_utc);
    void drawEventBox(const LayoutEvent& le, int level, bool blink_phase,
                      uint32_t now_utc, uint32_t display_start_utc);
    void drawNowLineFull();
    void redrawBoxAndNowLine(const LayoutEvent& le, int level, bool blink_phase,
                             uint32_t now_utc);

    std::vector<LayoutEvent> layoutEvents(std::vector<Event> events);
    BoxRect eventBoxRect(const LayoutEvent& le, uint32_t display_start_utc,
                        uint32_t display_end_utc) const;

    // 強調段階(0〜3)。0は非強調。開始が過去なら0。
    static int emphasisLevel(uint32_t event_start_utc, uint32_t now_utc);

    // 予定を識別するキー(開始時刻+件名のFNV-1a)。
    static uint32_t eventKey(const Event& e);

    int  findHistoryLevel(uint32_t key) const;
    void addOrExtendBlink(uint32_t key, const LayoutEvent& le, int level, uint32_t now_ms);

    // 実際に描かれる要素だけからハッシュを作る(日付文字列、表示開始分、窓内の全予定)。
    uint32_t contentSignature(const std::vector<Event>& events,
                              const std::string& date_str, uint32_t start_min) const;

    // スプライトの指定矩形だけをLCDへ転送する。
    void pushRect(int x, int y, int w, int h);

    static int   nowLineY();
    static float pxPerSec() { return (float)TIMELINE_H / (DISP_HOURS * 3600); }
    static int   clockRectW();
    static int   clockRectH() { return HEADER_H - 8; }
    static int   clockRectX();
    static int   clockRectY() { return (HEADER_H - clockRectH()) / 2; }

    // ─ 画面レイアウト(論理600x1024、縦置き) ─
    static const int SCR_W      = 600;
    static const int SCR_H      = 1024;
    static const int HEADER_H   = 60;
    static const int TIMELINE_H = SCR_H - HEADER_H;
    static const int LABEL_W    = 70;
    static const int CONTENT_X  = LABEL_W;
    static const int CONTENT_W  = SCR_W - LABEL_W;
    static const int DISP_HOURS = 6;
    // 現在時刻線は表示窓の先頭からこの秒数だけ下に固定する
    // (窓の先頭 = now - NOW_OFFSET_SEC なので、現在時刻線のY座標は常に一定になる)。
    static const uint32_t NOW_OFFSET_SEC = 3600;

    // 各書体の実効ピクセル高さ(レイアウト計算用)。
    static const int FS_BOOT   = 46;
    static const int FS_HEADER = 36;
    static const int FS_CLOCK  = 40;
    static const int FS_EVENT  = 22;

    // 点滅の総時間と半周期。半周期ごとにフェーズを反転する。
    static const uint32_t BLINK_DURATION_MS    = 5000;
    static const uint32_t BLINK_HALF_PERIOD_MS = 250;

    // 強調段階のしきい値(開始までの残り秒数)。値が小さいほど強い強調。
    static const uint32_t EMPH_L1_SEC = 600;
    static const uint32_t EMPH_L2_SEC = 300;
    static const uint32_t EMPH_L3_SEC = 120;

    LGFX_Device* _gfx = nullptr; // begin()前はnullptr
    LGFX_Sprite  _canvas;        // 600x1024 RGB565、PSRAM

    bool        _has_rendered   = false;
    uint32_t    _last_signature = 0;
    std::string _last_clock_str;

    uint32_t _last_display_start_utc = 0;
    uint32_t _last_display_end_utc   = 0;

    std::vector<EmphasisRecord> _emphasis_history;
    std::vector<BlinkEntry>     _blinks;
};
