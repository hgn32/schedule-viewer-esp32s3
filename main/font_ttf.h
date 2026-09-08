#pragma once

#include <LovyanGFX.hpp>

#include <string>

#include "esp_err.h"

// フラッシュのfontパーティション(生のTTF)をFreeTypeで描くための層。
//
// SDカードが無い基板なので、日本語TTF(fonts/MPLUS1-ExtraBold.ttf)はビルド時に
// fontパーティションへ書き込み、esp_partition_mmapで直接メモリへマップして
// FT_New_Memory_Faceへ渡す(ファイルシステムを介さない)。

// fontパーティションを探してマップし、FreeTypeでフェイスを開く。
// パーティションが無ければESP_ERR_NOT_FOUND、マップ/フェイス生成の失敗は
// ESP_FAILを返す。
esp_err_t fontTtfInit();

// TTFが使える状態か。falseなら呼び出し側は内蔵フォントへ退避する。
bool fontTtfReady();

// pxピクセル高で描いたときの文字列の幅。使えないときは0。
int fontTtfTextWidth(const std::string& str, int px);

// pxピクセル高で描いたときの行の高さ(ascender+descender)。使えないときは0。
int fontTtfLineHeight(int px);

// gfxへ描画する。色はRGB888。backは半透明合成の下地色として使う。
void fontTtfDrawText(LovyanGFX* gfx, const std::string& str, int x, int y, int px,
                     uint32_t fore_rgb888, uint32_t back_rgb888,
                     lgfx::textdatum_t datum);

// ─────────────────────────────────────────────────────────────────────────────
// 時計表示用のグリフキャッシュ。毎秒FreeTypeのラスタライズを走らせないよう、
// 使う文字だけを先に描いてカバレッジをRAM(PSRAM可)に保持しておく。

// charsに含まれる各文字をpxピクセル高で1回だけ描き、カバレッジをキャッシュする。
// 呼び直すと同じpxで再キャッシュする(キャッシュは1px太らせ後の幅で保持する)。
esp_err_t fontTtfCacheGlyphs(const char* chars, int px);

// キャッシュ済み文字だけで構成された文字列の幅。キャッシュに無い文字は幅0として無視する。
int fontTtfCachedTextWidth(const std::string& str);

// キャッシュ済みグリフを貼り付けるだけの描画(FreeTypeを呼ばない)。
// 合成ロジックはfontTtfDrawText()と同じ(1px太らせ+下地色との線形合成)。
void fontTtfDrawCachedText(LovyanGFX* gfx, const std::string& str, int x, int y,
                           uint32_t fore_rgb888, uint32_t back_rgb888,
                           lgfx::textdatum_t datum);
