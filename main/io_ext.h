#pragma once
#include <cstdint>

#include "driver/i2c_master.h"
#include "esp_err.h"

// Waveshare ESP32-S3-Touch-LCD-7BのIOエキスパンダ(I2C 0x24)制御。
// I2C(SDA=GPIO8 / SCL=GPIO9 / 400kHz)経由でタッチリセット・バックライト・
// LCDリセット・SDカードCS・USB/CAN切替の5ピンを出力制御する。
// チップはCH422G。用途ごとにI2Cアドレスが違い、各アドレスへ1バイトだけ書く。
// アドレス0x24がモード設定(0x01で出力)、0x38がIO0〜IO7の出力値。

static const uint8_t IO_EXT_PIN_TP_RST     = 1;
static const uint8_t IO_EXT_PIN_BACKLIGHT  = 2;
static const uint8_t IO_EXT_PIN_LCD_RST    = 3;
static const uint8_t IO_EXT_PIN_SD_CS      = 4;
static const uint8_t IO_EXT_PIN_USB_SEL    = 5;

// I2Cバスの初期化とデバイス登録、出力モード設定、出力値の初期化まで行う。
// 出力の初期値はWaveshare公式デモと同じ0x1E(バックライトON、USB_SELはLow)。
esp_err_t ioExtBegin();

// 指定ピンの出力レベルを変える。シャドウ値を更新してから0x38へまとめて書く。
// USB_SEL(bit5)をHighにする要求は無視してESP_ERR_INVALID_ARGを返す
// (HighにするとネイティブUSBが切り離され、書き込みもログ取得もできなくなる)。
// begin()未実行ならESP_ERR_INVALID_STATE。
esp_err_t ioExtSetOutput(uint8_t pin, bool level);

// バックライトのON/OFF。**CH422Gに調光の機能は無い**ので、
// percentは0(消灯)か非0(点灯)かしか見ない。引数は互換のために残してある。
// begin()未実行ならESP_ERR_INVALID_STATE。
esp_err_t ioExtSetBacklight(uint8_t percent);

// タッチIC(GT911)は同じI2Cバス(GPIO8/GPIO9)にぶら下がっているので、
// バスを作り直さずここで確保したものを共有する。begin()未実行ならnullptrを返す。
i2c_master_bus_handle_t ioExtGetBus();
