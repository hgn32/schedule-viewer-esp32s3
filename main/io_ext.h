#pragma once
#include <cstdint>

#include "driver/i2c_master.h"
#include "esp_err.h"

// Waveshare ESP32-S3-Touch-LCD-7BのIO拡張チップ(I2C 0x24)制御。
// I2C(SDA=GPIO8 / SCL=GPIO9 / 400kHz)経由でタッチリセット・バックライト・
// LCDリセット・SDカードCS・USB/CAN切替の5ピンを出力制御する。
// **チップはCH32V003(MCU)。CH422Gではない**(7Bで置き換わった)。単一アドレス0x24へ
// {レジスタ番号, 値}の2バイトを書く形式で、レジスタは0x02=モード、0x03=IO出力、
// 0x04=IO入力、0x05=バックライトPWM、0x06=ADC。

static const uint8_t IO_EXT_PIN_TP_RST     = 1;
static const uint8_t IO_EXT_PIN_BACKLIGHT  = 2;
static const uint8_t IO_EXT_PIN_LCD_RST    = 3;
static const uint8_t IO_EXT_PIN_SD_CS      = 4;
static const uint8_t IO_EXT_PIN_USB_SEL    = 5;

// I2Cバスの初期化とデバイス登録、全ピン出力への設定、出力値の初期化まで行う。
// 出力の初期値は0x1E(バックライトON、USB_SELはLow)。
esp_err_t ioExtBegin();

// 指定ピンの出力レベルを変える。シャドウ値を更新してからレジスタ0x03へまとめて書く。
// USB_SEL(bit5)をHighにする要求は無視してESP_ERR_INVALID_ARGを返す
// (HighにするとネイティブUSBが切り離され、書き込みもログ取得もできなくなる)。
// begin()未実行ならESP_ERR_INVALID_STATE。
esp_err_t ioExtSetOutput(uint8_t pin, bool level);

// バックライトのイネーブル(DISP=IO2)。レジスタ0x03のbit2だけを操作する。
// Waveshare公式サンプル(wavesahre_rgb_lcd_bl_on()/bl_off())と同じ構造で、
// PWM(0x05)には一切触らない。begin()未実行ならESP_ERR_INVALID_STATE。
esp_err_t ioExtBacklightEnable(bool on);

// バックライトの輝度(0〜100)。レジスタ0x05へPWM値を書く。
// **PWMは反転しており、書く値が大きいほど暗い**(0が最大輝度、255で消灯)。
// IO2(DISP)のON/OFFには関与しないので、ここではレジスタ0x03を触らない。
// begin()未実行ならESP_ERR_INVALID_STATE。
esp_err_t ioExtSetBacklightLevel(uint8_t percent);

// タッチIC(GT911)は同じI2Cバス(GPIO8/GPIO9)にぶら下がっているので、
// バスを作り直さずここで確保したものを共有する。begin()未実行ならnullptrを返す。
i2c_master_bus_handle_t ioExtGetBus();
