#include "io_ext.h"

#include "driver/i2c_master.h"
#include "esp_log.h"

static const char* TAG = "io_ext";

// Waveshare ESP32-S3-Touch-LCD-7Bの基板配線。
static const gpio_num_t IO_EXT_SDA      = GPIO_NUM_8;
static const gpio_num_t IO_EXT_SCL      = GPIO_NUM_9;
static const uint16_t   IO_EXT_ADDR     = 0x24;
static const uint32_t   IO_EXT_FREQ_HZ  = 400000;
static const int        IO_EXT_TIMEOUT_MS = 100;

// レジスタアドレス。
static const uint8_t REG_MODE   = 0x02; // 0xFFで全ピン出力
static const uint8_t REG_OUTPUT = 0x03; // 出力値(8bitシャドウ)
static const uint8_t REG_PWM    = 0x05; // バックライトのPWM値

static i2c_master_bus_handle_t s_bus = nullptr;
static i2c_master_dev_handle_t s_dev = nullptr;
static uint8_t s_output_shadow = 0xFF;

// レジスタへ2バイト{reg, value}を書く。
static esp_err_t writeReg(uint8_t reg, uint8_t value) {
    if (s_dev == nullptr) return ESP_ERR_INVALID_STATE;

    uint8_t buf[2] = {reg, value};
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
        ESP_LOGE(TAG, "I2Cバスの初期化に失敗: %s", esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address  = IO_EXT_ADDR;
    dev_cfg.scl_speed_hz    = IO_EXT_FREQ_HZ;

    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "IOエキスパンダのデバイス登録に失敗: %s", esp_err_to_name(err));
        i2c_del_master_bus(s_bus);
        s_bus = nullptr;
        return err;
    }

    // 全ピン出力(0xFF)、初期出力値は全High。
    s_output_shadow = 0xFF;
    err = writeReg(REG_MODE, 0xFF);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "モードレジスタの書き込みに失敗: %s", esp_err_to_name(err));
        return err;
    }
    err = writeReg(REG_OUTPUT, s_output_shadow);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "出力レジスタの初期化に失敗: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "IOエキスパンダを初期化した(addr=0x%02X)", IO_EXT_ADDR);
    return ESP_OK;
}

esp_err_t ioExtSetOutput(uint8_t pin, bool level) {
    if (s_dev == nullptr) return ESP_ERR_INVALID_STATE;
    if (pin > 7) return ESP_ERR_INVALID_ARG;

    uint8_t mask = (uint8_t)(1u << pin);
    if (level) {
        s_output_shadow |= mask;
    } else {
        s_output_shadow &= (uint8_t)~mask;
    }
    return writeReg(REG_OUTPUT, s_output_shadow);
}

esp_err_t ioExtSetBacklight(uint8_t percent) {
    if (s_dev == nullptr) return ESP_ERR_INVALID_STATE;

    uint8_t clamped = (percent > 97) ? 97 : percent;
    uint8_t pwm     = (uint8_t)((uint32_t)clamped * 255u / 100u);
    return writeReg(REG_PWM, pwm);
}
