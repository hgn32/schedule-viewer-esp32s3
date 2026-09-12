#include "lcd_panel.h"

#include <cstring>

#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"

#include <LovyanGFX.hpp>
#include <lgfx/v1/Bus.hpp>
#include <lgfx/v1/panel/Panel_FrameBufferBase.hpp>

static const char* TAG = "lcd_panel";

// ─────────────────────────────────────────────────────────────────────────────
// esp_lcdのRGBパネル(フレームバッファ)をLovyanGFXのPanel_FrameBufferBase派生で
// 包む層。バス通信を持たないフレームバッファ直書きのパネルなので、
// Panel_Device::init()を呼ばず、init(bool)を丸ごと上書きする
// (「LovyanGFXの実装上の注意」を参照)。

namespace {

class PanelLcd7b : public lgfx::Panel_FrameBufferBase {
public:
    PanelLcd7b() {
        // esp_lcdのRGBパネルはフレームバッファを16bitのネイティブ順で読む。
        // LovyanGFXのrgb565_2Byteはバイトスワップ済みの形式(SPIパネル向け)なので、
        // それを渡すと上位・下位バイトが入れ替わり、赤と青が混ざって紫がかる。
        // スワップしない側のrgb565_nonswappedを使う(enum.hppの定義を参照)。
        _write_depth = lgfx::color_depth_t::rgb565_nonswapped;
        _read_depth  = lgfx::color_depth_t::rgb565_nonswapped;
    }

    // esp_lcdから取得したフレームバッファの先頭アドレスを渡す。
    // init()より前に呼ぶこと。
    void setFrameBuffer(uint8_t* fb) { _fb = fb; }

    lgfx::color_depth_t setColorDepth(lgfx::color_depth_t) override {
        // このパネルはRGB565固定。呼ばれても深度は変えない。
        return _write_depth;
    }

    bool init(bool) override {
        if (_fb == nullptr) {
            ESP_LOGE(TAG, "フレームバッファが未設定");
            return false;
        }

        _range_mod.top    = INT16_MAX;
        _range_mod.left   = INT16_MAX;
        _range_mod.right  = 0;
        _range_mod.bottom = 0;

        setInvert(_invert);
        setRotation(_rotation);

        // フレームバッファのメモリレイアウトは物理解像度(1024x600)で固定。
        // 論理座標の回転はPanel_FrameBufferBase側のx/y入れ替えが担うので、
        // ここでは常に物理値を使う。
        const int w = config().panel_width;
        const int h = config().panel_height;

        _lines_buffer = (uint8_t**)heap_caps_malloc((size_t)h * sizeof(uint8_t*),
                                                     MALLOC_CAP_DEFAULT);
        if (_lines_buffer == nullptr) {
            ESP_LOGE(TAG, "行ポインタ配列を確保できない(%d行)", h);
            return false;
        }
        for (int y = 0; y < h; y++) {
            _lines_buffer[y] = _fb + (size_t)y * w * 2;
        }
        return true;
    }

private:
    uint8_t* _fb = nullptr;
};

// LGFX_Device派生。esp_lcdのRGBパネルをそのままLGFXの描画先として扱う。
class Lgfx7b : public lgfx::LGFX_Device {
public:
    Lgfx7b() {
        auto cfg          = _panel.config();
        cfg.memory_width  = LCD_PHYS_W;
        cfg.memory_height = LCD_PHYS_H;
        cfg.panel_width   = LCD_PHYS_W;
        cfg.panel_height  = LCD_PHYS_H;
        _panel.config(cfg);

        // Panel_Device::init()は呼ばないため必須ではないが、他経路からの
        // null参照を防ぐために念のためバスを持たせておく。
        _panel.setBus(&_bus);

        setPanel(&_panel);
    }

    void setFrameBuffer(uint8_t* fb) { _panel.setFrameBuffer(fb); }

private:
    PanelLcd7b     _panel;
    lgfx::Bus_NULL _bus;
};

Lgfx7b*           s_gfx   = nullptr;
esp_lcd_panel_handle_t s_panel = nullptr;

// Waveshare公式ESP-IDFサンプルと同じRGBタイミング設定。
esp_err_t createEspLcdPanel(esp_lcd_panel_handle_t* out_panel, void** out_fb) {
    if (out_panel == nullptr || out_fb == nullptr) return ESP_ERR_INVALID_ARG;

    esp_lcd_rgb_panel_config_t cfg = {};
    cfg.clk_src   = LCD_CLK_SRC_DEFAULT;
    cfg.data_width       = 16;
    cfg.bits_per_pixel   = 16;
    cfg.num_fbs          = 1;
    // バウンスバッファ(SRAM)は必須。外すとEDMAがPSRAMを直接読む構成になるが、
    // 実機で試したところ画面が真っ黒になった(2026-09-12)。公式ドキュメントの
    // "a high pixel clock might cause LCD peripheral starvation, leading to
    // display corruption"に該当する。CPUコピーの負荷(pclk 30MHzで毎秒60MB相当)は
    // 承知のうえで、ここは1024x10pxのまま維持すること。
    cfg.bounce_buffer_size_px = 1024 * 10;

    cfg.timings.pclk_hz           = 30 * 1000 * 1000;
    cfg.timings.h_res             = LCD_PHYS_W;
    cfg.timings.v_res             = LCD_PHYS_H;
    cfg.timings.hsync_pulse_width = 162;
    cfg.timings.hsync_back_porch  = 152;
    cfg.timings.hsync_front_porch = 48;
    cfg.timings.vsync_pulse_width = 45;
    cfg.timings.vsync_back_porch  = 13;
    cfg.timings.vsync_front_porch = 3;
    cfg.timings.flags.pclk_active_neg = 1;

    cfg.hsync_gpio_num = 46;
    cfg.vsync_gpio_num = 3;
    cfg.de_gpio_num    = 5;
    cfg.pclk_gpio_num  = 7;
    cfg.disp_gpio_num  = -1;

    // data[0..15] = B3..B7, G2..G7, R3..R7の順(Waveshare公式サンプルと同じ配線)。
    const int data_pins[16] = {14, 38, 18, 17, 10, 39, 0, 45, 48, 47, 21, 1, 2, 42, 41, 40};
    for (int i = 0; i < 16; i++) cfg.data_gpio_nums[i] = data_pins[i];

    cfg.flags.fb_in_psram = 1;

    esp_err_t err = esp_lcd_new_rgb_panel(&cfg, out_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_rgb_panelに失敗: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_lcd_panel_init(*out_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_panel_initに失敗: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_lcd_rgb_panel_get_frame_buffer(*out_panel, 1, out_fb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "フレームバッファの取得に失敗: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

}  // namespace

esp_err_t lcdPanelBegin() {
    if (s_gfx != nullptr) return ESP_OK;

    void* fb = nullptr;
    esp_err_t err = createEspLcdPanel(&s_panel, &fb);
    if (err != ESP_OK) return err;

    // esp_lcdが0で埋める保証に頼らず、明示的に黒で初期化する。
    memset(fb, 0, (size_t)LCD_PHYS_W * LCD_PHYS_H * 2);

    static Lgfx7b gfx;
    gfx.setFrameBuffer((uint8_t*)fb);
    if (!gfx.init()) {
        ESP_LOGE(TAG, "LGFX_Deviceの初期化に失敗");
        return ESP_FAIL;
    }
    // 回転はここではかけない。LGFX_Deviceは生の向き(1024x600)のまま使い、
    // 回転はDisplay側の描画用スプライトに持たせる(lcd_panel.hのコメント参照)。

    s_gfx = &gfx;
    ESP_LOGI(TAG, "LCDパネルを初期化した(生%dx%d)", gfx.width(), gfx.height());
    return ESP_OK;
}

LGFX_Device* lcdPanelGfx() {
    return s_gfx;
}
