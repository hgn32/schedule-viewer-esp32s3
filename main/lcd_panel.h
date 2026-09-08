#pragma once
#include "esp_err.h"

// LGFX_Deviceの前方宣言。実体はlovyangfxのLGFXBase.hppにある。
// このヘッダを読む側(display.cpp等)は<LovyanGFX.hpp>を直接includeしている前提。
namespace lgfx {
inline namespace v1 {
class LGFX_Device;
}
}  // namespace lgfx
using LGFX_Device = lgfx::LGFX_Device;

// Waveshare ESP32-S3-Touch-LCD-7Bの物理パネル(1024x600 RGB565パラレル)を
// esp_lcdで駆動し、LovyanGFXのLGFX_Deviceとして包む層。
// 論理座標はsetRotation(LCD_ROTATION)で600x1024(縦長)にする。

static const int LCD_PHYS_W = 1024;
static const int LCD_PHYS_H = 600;
// 縦置き。天地が逆に出る場合は3にする(実機未確認)。
static const int LCD_ROTATION = 1;

// esp_lcdのRGBパネル初期化 → フレームバッファ取得 → LovyanGFXデバイス初期化
// → setRotation(LCD_ROTATION)まで行う。
esp_err_t lcdPanelBegin();

// 初期化済みのLGFX_Deviceを返す。begin()が成功するまではnullptr。
LGFX_Device* lcdPanelGfx();
