#include "io_ext.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Waveshare ESP32-S3-Touch-LCD-7Bの基板配線。
static const gpio_num_t IO_EXT_SDA      = GPIO_NUM_8;
static const gpio_num_t IO_EXT_SCL      = GPIO_NUM_9;
static const uint16_t   IO_EXT_ADDR     = 0x24;
static const uint32_t   IO_EXT_FREQ_HZ  = 400000;
static const int        IO_EXT_TIMEOUT_MS = 100;

// IO拡張チップはCH32V003(MCU)。CH422Gではない。
// 単一のI2Cアドレス0x24に対して{レジスタ番号, 値}の2バイトを書く形式で、
// 用途ごとにアドレスを変えるCH422Gとは互換性が無い。
static const uint8_t REG_MODE   = 0x02; // ピンの入出力方向。0xFFで全ピン出力
static const uint8_t REG_IO_OUT = 0x03; // IO0〜IO7の出力値(8bitシャドウ)
static const uint8_t REG_IO_IN  = 0x04; // IO0〜IO7の入力値
static const uint8_t REG_PWM    = 0x05; // バックライトのPWM値(0〜255)

static const uint8_t MODE_ALL_OUTPUT = 0xFF;

// 出力の初期値。bit1〜bit4だけHighで、bit0・bit5・bit6・bit7はLow。
// bit5はUSB_SEL(HighでCAN、LowでネイティブUSB)。Highにするとアプリ起動と同時に
// COMポートが消えて書き込みもログ取得もできなくなるため、必ずLowで保つ。
// USB_SEL(bit5)だけLowで、残りは全てHigh。IO0・IO6・IO7の用途は7Bの資料で
// 確認できていないが、電源投入直後の既定はプルアップでHighとみられる。
// 0x1E(IO0・IO6・IO7をLowにする値。末尾Bなしの7の資料由来)を書いたところ
// 画面が真っ暗になったため、Low側へ落とすのはUSB_SELだけにする。
static const uint8_t OUTPUT_DEFAULT = 0xDF;

static const uint8_t EXIO_LCD_BL  = 2; // bit2。バックライト(DISP)のイネーブル
static const uint8_t EXIO_USB_SEL = 5; // bit5。LowでネイティブUSB、HighでCAN

// バックライトPWMの上限。Waveshare公式デモとESPHomeのコンポーネントが
// どちらも247(=97%)を上限にしている。
static const uint8_t BACKLIGHT_MAX_PERCENT = 97;

static i2c_master_bus_handle_t s_bus = nullptr;
static i2c_master_dev_handle_t s_dev = nullptr;
static uint8_t                 s_output_shadow = OUTPUT_DEFAULT;

// {レジスタ番号, 値}の2バイトを書く。
// 失敗時は2msだけ待って同じ値をもう1回だけ送り直す(2回目も失敗したらそのまま返す)。
static esp_err_t writeReg(uint8_t reg, uint8_t value) {
    if (s_dev == nullptr) return ESP_ERR_INVALID_STATE;

    uint8_t buf[2] = {reg, value};

    esp_err_t err = i2c_master_transmit(s_dev, buf, sizeof(buf), IO_EXT_TIMEOUT_MS);
    if (err == ESP_OK) return err;

    vTaskDelay(pdMS_TO_TICKS(2));
    return i2c_master_transmit(s_dev, buf, sizeof(buf), IO_EXT_TIMEOUT_MS);
}

esp_err_t ioExtBegin() {
    if (s_dev != nullptr) return ESP_OK;

    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port          = I2C_NUM_0;
    bus_cfg.sda_io_num        = IO_EXT_SDA;
    bus_cfg.scl_io_num        = IO_EXT_SCL;
    bus_cfg.clk_source        = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        return err;
    }

    // CH32V003はACKを返す(I2Cスキャンで0x24が応答することを実機で確認済み)。
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address  = IO_EXT_ADDR;
    dev_cfg.scl_speed_hz    = IO_EXT_FREQ_HZ;

    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        i2c_del_master_bus(s_bus);
        s_bus = nullptr;
        return err;
    }

    // CH32V003はESP32とは別MCUで、ESP32のリセットでは設定が消えない。
    // 前回の起動で書いた値が残るので、毎回ここで明示的に既知の状態へ戻す。
    err = writeReg(REG_MODE, MODE_ALL_OUTPUT);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(2));

    s_output_shadow = OUTPUT_DEFAULT;
    err = writeReg(REG_IO_OUT, s_output_shadow);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    // LCD_RST(IO3)をパルスして液晶を初期化し直す。lcdPanelBegin()より前に行う。
    writeReg(REG_IO_OUT, (uint8_t)(s_output_shadow & ~(1u << 3)));
    vTaskDelay(pdMS_TO_TICKS(20));
    writeReg(REG_IO_OUT, s_output_shadow);
    vTaskDelay(pdMS_TO_TICKS(50));

    return ESP_OK;
}

esp_err_t ioExtSetOutput(uint8_t pin, bool level) {
    if (s_dev == nullptr) return ESP_ERR_INVALID_STATE;
    if (pin > 7) return ESP_ERR_INVALID_ARG;

    // USB_SELをHighにするとネイティブUSBがCAN側へ切り替わり、書き込み経路も
    // ログ取得も失われる。このプロジェクトはCANを使わないので上げさせない。
    if (pin == EXIO_USB_SEL && level) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t mask = (uint8_t)(1u << pin);
    if (level) {
        s_output_shadow |= mask;
    } else {
        s_output_shadow &= (uint8_t)~mask;
    }
    return writeReg(REG_IO_OUT, s_output_shadow);
}

esp_err_t ioExtBacklightEnable(bool on) {
    if (s_dev == nullptr) return ESP_ERR_INVALID_STATE;

    // Waveshare公式のwavesahre_rgb_lcd_bl_on()/bl_off()と同じく、
    // DISP(IO2)の1ビットだけを操作する。PWM(0x05)には触らない。
    return ioExtSetOutput(EXIO_LCD_BL, on);
}

esp_err_t ioExtSetBacklightLevel(uint8_t percent) {
    if (s_dev == nullptr) return ESP_ERR_INVALID_STATE;

    // PWMは反転しており、書く値が大きいほど暗い(0が最大輝度)。
    uint8_t clamped = (percent > 100) ? 100 : percent;
    uint8_t pwm     = (uint8_t)(255u - (uint32_t)clamped * 255u / 100u);

    return writeReg(REG_PWM, pwm);
}

i2c_master_bus_handle_t ioExtGetBus() {
    return s_bus;
}
