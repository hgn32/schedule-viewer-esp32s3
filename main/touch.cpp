#include "touch.h"

#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_timer.h"

#include "io_ext.h"

// パネルの生の向き(1024x600)での最大値。座標そのものは使わないが、
// esp_lcd_touch_config_tの初期化には必要。
static const uint16_t TOUCH_X_MAX = 1024;
static const uint16_t TOUCH_Y_MAX = 600;

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

TouchEvent touchPoll() {
    if (s_touch == nullptr) return TouchEvent::None;

    // 読み出し失敗時は「触れていない」として扱う。タッチが使えなくても
    // 表示自体は続けたいので、ここでエラーログを出し続けることはしない。
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
        s_state          = TouchState::WaitingLongTap;
        s_press_start_us = now_us;
        return TouchEvent::Press;

    case TouchState::WaitingLongTap:
        if (!touched) {
            s_state = TouchState::Idle;
            return TouchEvent::None;
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
