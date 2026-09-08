#pragma once
#include <cstdint>

#include "esp_err.h"

// Waveshare ESP32-S3-Touch-LCD-7BのIOエキスパンダ(I2C 0x24)制御。
// I2C(SDA=GPIO8 / SCL=GPIO9 / 400kHz)経由でタッチリセット・バックライト・
// LCDリセット・SDカードCS・USB/CAN切替の5ピンを出力制御する。
// レジスタ0x02がモード(0xFFで全ピン出力)、0x03が出力値(8bitシャドウ)、
// 0x05がバックライトのPWM値。

static const uint8_t IO_EXT_PIN_TP_RST     = 1;
static const uint8_t IO_EXT_PIN_BACKLIGHT  = 2;
static const uint8_t IO_EXT_PIN_LCD_RST    = 3;
static const uint8_t IO_EXT_PIN_SD_CS      = 4;
static const uint8_t IO_EXT_PIN_USB_SEL    = 5;

// I2Cバスの初期化とデバイス登録、モードレジスタを全出力(0xFF)に設定する。
// 出力値のシャドウは0xFFで初期化する(全ピンHigh)。
esp_err_t ioExtBegin();

// 指定ピンの出力レベルを変える。シャドウ値を更新してから0x03へまとめて書く。
// begin()未実行ならESP_ERR_INVALID_STATE。
esp_err_t ioExtSetOutput(uint8_t pin, bool level);

// バックライトの輝度(0〜100%)。レジスタ0x05へPWM値を書く。
// 公式サンプルと同じく97%を上限にする(値=min(percent,97)*255/100)。
// begin()未実行ならESP_ERR_INVALID_STATE。
esp_err_t ioExtSetBacklight(uint8_t percent);
