#pragma once

#include <cstdint>

// GT911(タッチIC)からの読み出しと、タッチダウン/ロングタップの判定。
// 座標は使わない。画面全体を1つの受付領域として、押されているかどうかだけ見る。

enum class TouchEvent {
    None,
    Press,   // 押し始めた瞬間
    LongTap, // 押したままTOUCH_LONG_TAP_MSに達した瞬間
};

// ロングタップと判定するまでの押下継続時間(ms)。
static const uint32_t TOUCH_LONG_TAP_MS = 1500;

// GT911を初期化する。I2Cバスはio_ext.hのioExtGetBus()から借りるので、
// ioExtBegin()より後に呼ぶこと。失敗してもtouchPoll()は常にTouchEvent::Noneを
// 返すので、呼び出し側は表示を止めなくてよい。
bool touchBegin();

// 現在のタッチ状態を読み、状態遷移からイベントを返す。メインループから
// 定期的に呼ぶこと。1回のタッチ(押してから離すまで)で返るのは、押し始めの
// Press(1回)と、押し続けた場合のLongTap(1回)だけで、それ以外は常にNoneを返す。
// Pressの直後に同じタッチのLongTapが続くことがあるので、呼び出し側は
// 「Pressで行った操作を、同じタッチのLongTapで取り消してしまわないか」を
// 必ず確認すること(main.cppのsuppress_longtapがその抑制にあたる)。
TouchEvent touchPoll();
