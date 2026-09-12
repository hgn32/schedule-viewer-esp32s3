#include "io_ext.h"

#include "driver/i2c_master.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "io_ext";

// Waveshare ESP32-S3-Touch-LCD-7Bの基板配線。
static const gpio_num_t IO_EXT_SDA      = GPIO_NUM_8;
static const gpio_num_t IO_EXT_SCL      = GPIO_NUM_9;
static const uint32_t   IO_EXT_FREQ_HZ  = 400000;
static const int        IO_EXT_TIMEOUT_MS = 100;

// IO拡張チップはCH422G。用途ごとにI2Cアドレスが違い、各アドレスへ1バイトだけ書く
// (「アドレス+レジスタ番号」形式のチップではない)。
static const uint16_t CH422G_ADDR_MODE = 0x24; // モード設定(WR_SET)
static const uint16_t CH422G_ADDR_OUT  = 0x38; // IO0〜IO7の出力(WR_IO)

// モード。bit0を立てるとIO0〜IO7が出力になる。
static const uint8_t CH422G_MODE_IO_OUTPUT = 0x01;

// 出力値はWaveshare公式デモ(ESP32-S3-Touch-LCD-7-Demo/ESP-IDF/08_lvgl_Porting/
// main/waveshare_rgb_lcd_port.cのwavesahre_rgb_lcd_bl_on/off)の実測値をそのまま使う。
// 0x1E = 0b0001_1110。bit1〜bit4だけHighで、bit0・bit5・bit6・bit7はLow。
// bit2がバックライト(DISP)で、消すときは0x1A(bit2だけ落とす)。
// bit5はUSB_SEL(HighでCAN、LowでネイティブUSB)。公式もLowで固定しており、
// Highにするとアプリ起動と同時にCOMポートが消えて書き込みもログ取得もできなくなる。
//
// 独自に0xFFや0xDF(bit0/6/7も立てた値)を書くと画面が真っ黒になることを実機で確認済み
// (2026-09-12)。公式の値から外れた値を書かないこと。
static const uint8_t OUTPUT_BL_ON  = 0x1E;
static const uint8_t OUTPUT_BL_OFF = 0x1A;

static const uint8_t EXIO_LCD_BL  = 2; // bit2。バックライト(DISP)
static const uint8_t EXIO_USB_SEL = 5; // bit5。LowでネイティブUSB、HighでCAN

static i2c_master_bus_handle_t s_bus      = nullptr;
static i2c_master_dev_handle_t s_dev_mode = nullptr;
static i2c_master_dev_handle_t s_dev_out  = nullptr;
static uint8_t                 s_output_shadow = OUTPUT_BL_ON;

// 指定アドレスのデバイスへ1バイト書く。
// 実機ログでNACKが1回だけ出て以降の書き込みが失敗する事例があったため、
// 失敗時は2msだけ待って同じ値をもう1回だけ送り直す(2回目も失敗したらそのまま返す)。
// これで直るかは未検証。
static esp_err_t writeByte(i2c_master_dev_handle_t dev, uint8_t value) {
    if (dev == nullptr) return ESP_ERR_INVALID_STATE;

    esp_err_t err = i2c_master_transmit(dev, &value, 1, IO_EXT_TIMEOUT_MS);
    if (err == ESP_OK) return err;

    ESP_LOGW(TAG, "I2C書き込みに失敗した(%s)。2ms待って1回だけ再送する",
            esp_err_to_name(err));
    vTaskDelay(pdMS_TO_TICKS(2));
    return i2c_master_transmit(dev, &value, 1, IO_EXT_TIMEOUT_MS);
}

static esp_err_t addDevice(uint16_t addr, i2c_master_dev_handle_t* out) {
    if (out == nullptr) return ESP_ERR_INVALID_ARG;

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address  = addr;
    dev_cfg.scl_speed_hz    = IO_EXT_FREQ_HZ;
    // CH422GはACKを返さない。ACK検査を有効にしたままだと書き込みが成立していても
    // i2c_master_transmit()がESP_ERR_INVALID_STATEを返し、ログもエラーで埋まる。
    // Waveshare公式デモも同じ書き込みの戻り値を一切見ておらず、それで
    // バックライトもタッチリセットも動いている(2026-09-12に実機のログで確認)。
    dev_cfg.flags.disable_ack_check = 1;

    return i2c_master_bus_add_device(s_bus, &dev_cfg, out);
}

esp_err_t ioExtBegin() {
    if (s_dev_out != nullptr) return ESP_OK;

    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port          = I2C_NUM_0;
    bus_cfg.sda_io_num        = IO_EXT_SDA;
    bus_cfg.scl_io_num        = IO_EXT_SCL;
    bus_cfg.clk_source        = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2Cバスの初期化に失敗: %s", esp_err_to_name(err));
        return err;
    }

    err = addDevice(CH422G_ADDR_MODE, &s_dev_mode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "モード用デバイスの登録に失敗: %s", esp_err_to_name(err));
        i2c_del_master_bus(s_bus);
        s_bus = nullptr;
        return err;
    }

    err = addDevice(CH422G_ADDR_OUT, &s_dev_out);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "出力用デバイスの登録に失敗: %s", esp_err_to_name(err));
        i2c_master_bus_rm_device(s_dev_mode);
        s_dev_mode = nullptr;
        i2c_del_master_bus(s_bus);
        s_bus = nullptr;
        return err;
    }

    // 順番に意味がある。出力モードにしてから出力値を書く。
    err = writeByte(s_dev_mode, CH422G_MODE_IO_OUTPUT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "出力モードの設定に失敗: %s", esp_err_to_name(err));
        return err;
    }

    // モード切替直後に立て続けに書くとNACKされている可能性があるため、
    // 出力値の書き込みまで少し待つ(これで直るかは未検証)。
    vTaskDelay(pdMS_TO_TICKS(2));

    s_output_shadow = OUTPUT_BL_ON;
    err = writeByte(s_dev_out, s_output_shadow);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "出力値の初期化に失敗: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "CH422Gを初期化した(出力=0x%02X、USB_SELはLow)", s_output_shadow);
    return ESP_OK;
}

esp_err_t ioExtSetOutput(uint8_t pin, bool level) {
    if (s_dev_out == nullptr) return ESP_ERR_INVALID_STATE;
    if (pin > 7) return ESP_ERR_INVALID_ARG;

    // USB_SELをHighにするとネイティブUSBがCAN側へ切り替わり、書き込み経路も
    // ログ取得も失われる。このプロジェクトはCANを使わないので上げさせない。
    if (pin == EXIO_USB_SEL && level) {
        ESP_LOGW(TAG, "USB_SEL(bit%u)はLow固定。Highにする要求を無視した", pin);
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t mask = (uint8_t)(1u << pin);
    if (level) {
        s_output_shadow |= mask;
    } else {
        s_output_shadow &= (uint8_t)~mask;
    }

    // 出力(0x38)の前に毎回モード(0x24)を書く。Waveshare公式デモの
    // wavesahre_rgb_lcd_bl_on()/off()も呼び出しのたびに0x24→0x38の順で書いており、
    // モードを書かずに0x38だけ書くと実機でNACKされることを確認している(2026-09-12)。
    esp_err_t err = writeByte(s_dev_mode, CH422G_MODE_IO_OUTPUT);
    if (err != ESP_OK) return err;

    return writeByte(s_dev_out, s_output_shadow);
}

esp_err_t ioExtSetBacklight(uint8_t percent) {
    if (s_dev_out == nullptr) return ESP_ERR_INVALID_STATE;

    // CH422Gに調光の機能は無い。bit2のON/OFFだけで、percentは0か非0かしか見ない。
    return ioExtSetOutput(EXIO_LCD_BL, percent != 0);
}

i2c_master_bus_handle_t ioExtGetBus() {
    return s_bus;
}
