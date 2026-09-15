#include "touch.h"

#include <cstdlib>

#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_timer.h"

#include "io_ext.h"

// パネルの生の向き(1024x600)での最大値。esp_lcd_touch_config_tの初期化に使うほか、
// rawToLogical()の反転計算にも使う。
static const uint16_t TOUCH_X_MAX = 1024;
static const uint16_t TOUCH_Y_MAX = 600;

// GT911が返す座標はパネルの生の向き(1024x600)。表示は論理600x1024なので直して返す。
// display.cppのpushRect()の逆変換にあたる(論理y = 生xの反転、論理x = 生y)。
// GT911の座標系が実機で想定と食い違った場合は、この3つの定数だけを直すこと。
static const bool TOUCH_SWAP_XY  = true;
static const bool TOUCH_INVERT_X = true;  // 生x(0..TOUCH_X_MAX-1)を反転する
static const bool TOUCH_INVERT_Y = false; // 生y(0..TOUCH_Y_MAX-1)を反転する

static void rawToLogical(int raw_x, int raw_y, int* out_x, int* out_y) {
    int x = raw_x;
    int y = raw_y;
    if (TOUCH_INVERT_X) x = (int)TOUCH_X_MAX - 1 - x;
    if (TOUCH_INVERT_Y) y = (int)TOUCH_Y_MAX - 1 - y;
    if (TOUCH_SWAP_XY) { *out_x = y; *out_y = x; }
    else               { *out_x = x; *out_y = y; }
}

// 1回のタッチ(押してから離すまで)の状態。
// WaitingLongTap: 押下中で、Pressは返却済みだがLongTapはまだ判定していない。
// LongTapSent: LongTapを返却済み。離れるまで何も返さない。
enum class TouchState {
    Idle,
    WaitingLongTap,
    LongTapSent,
};

static esp_lcd_touch_handle_t s_touch          = nullptr;
static TouchState             s_state          = TouchState::Idle;
static int64_t                s_press_start_us = 0;
static int                    s_press_x        = 0; // 押し始めの論理座標
static int                    s_press_y        = 0;
static int                    s_last_x         = 0; // 押下中の最後の論理座標
static int                    s_last_y         = 0;
static bool                   s_moved          = false; // スライド超過フラグ

bool touchBegin() {
    if (s_touch != nullptr) return true;

    i2c_master_bus_handle_t bus = ioExtGetBus();
    if (bus == nullptr) {
        // ioExtBegin()より先に呼ばれた場合はI2Cバスが未初期化。
        return false;
    }

    // ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG()マクロは指定順の指示付き初期化子リストで
    // esp_lcd_panel_io_i2c_config_tを組み立てるが、このマクロを提供する
    // esp_lcd_touch_gt911(1.2.1)が想定する構造体のメンバ順とESP-IDF v5.5の実際の
    // 宣言順が一致せず、C++では指示付き初期化子の順序違反としてビルドエラーになる。
    // そのため、マクロは使わずメンバへ個別に代入する。
    esp_lcd_panel_io_i2c_config_t io_config = {};
    io_config.dev_addr             = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS;
    io_config.control_phase_bytes  = 1;
    io_config.dc_bit_offset        = 0;
    io_config.lcd_cmd_bits         = 16;
    io_config.flags.disable_control_phase = 1;
    io_config.scl_speed_hz         = 100000; // GT911推奨のI2Cクロック(100kHz)
    esp_lcd_panel_io_handle_t     io_handle = nullptr;
    esp_err_t err = esp_lcd_new_panel_io_i2c(bus, &io_config, &io_handle);
    if (err != ESP_OK) {
        return false;
    }

    esp_lcd_touch_config_t touch_config = {};
    touch_config.x_max        = TOUCH_X_MAX;
    touch_config.y_max        = TOUCH_Y_MAX;
    touch_config.rst_gpio_num = GPIO_NUM_NC; // IO拡張(IO1)経由で既にリセット解除済み
    touch_config.int_gpio_num = GPIO_NUM_NC; // ポーリングで読むため使わない

    err = esp_lcd_touch_new_i2c_gt911(io_handle, &touch_config, &s_touch);
    if (err != ESP_OK) {
        s_touch = nullptr;
        return false;
    }

    return true;
}

TouchEvent touchPoll(int* out_x, int* out_y) {
    if (s_touch == nullptr) return TouchEvent::None;

    // 読み出し失敗時は「触れていない」として扱う。タッチが使えなくても
    // 表示自体は続けたいので、ここでエラーログを出し続けることはしない。
    // 押下中に読み出しが失敗した場合でも、離したときと同じ経路は通さず
    // 既存どおり即Noneを返す(状態は維持する。Tapの誤発火を避けるため)。
    if (esp_lcd_touch_read_data(s_touch) != ESP_OK) {
        return TouchEvent::None;
    }

    esp_lcd_touch_point_data_t points[1] = {};
    uint8_t                    point_num = 0;
    esp_err_t data_err = esp_lcd_touch_get_data(s_touch, points, &point_num, 1);
    bool touched = (data_err == ESP_OK) && point_num > 0;

    int64_t now_us = esp_timer_get_time();

    switch (s_state) {
    case TouchState::Idle:
        if (!touched) return TouchEvent::None;
        rawToLogical(points[0].x, points[0].y, &s_press_x, &s_press_y);
        s_last_x         = s_press_x;
        s_last_y         = s_press_y;
        s_moved          = false;
        s_state          = TouchState::WaitingLongTap;
        s_press_start_us = now_us;
        if (out_x != nullptr) *out_x = s_press_x;
        if (out_y != nullptr) *out_y = s_press_y;
        return TouchEvent::Press;

    case TouchState::WaitingLongTap:
        if (!touched) {
            s_state = TouchState::Idle;
            if (!s_moved) {
                if (out_x != nullptr) *out_x = s_last_x;
                if (out_y != nullptr) *out_y = s_last_y;
                return TouchEvent::Tap;
            }
            return TouchEvent::None;
        }
        rawToLogical(points[0].x, points[0].y, &s_last_x, &s_last_y);
        if (abs(s_last_x - s_press_x) > TOUCH_TAP_MOVE_MAX_PX ||
            abs(s_last_y - s_press_y) > TOUCH_TAP_MOVE_MAX_PX) {
            s_moved = true;
        }
        if ((now_us - s_press_start_us) >= (int64_t)TOUCH_LONG_TAP_MS * 1000) {
            s_state = TouchState::LongTapSent;
            return TouchEvent::LongTap;
        }
        return TouchEvent::None;

    case TouchState::LongTapSent:
        if (!touched) {
            s_state = TouchState::Idle;
        }
        return TouchEvent::None;
    }

    return TouchEvent::None;
}
