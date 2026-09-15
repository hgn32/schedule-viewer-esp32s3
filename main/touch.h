#pragma once

#include <cstdint>

// GT911(タッチIC)からの読み出しと、タップ/ロングタップの判定。
// 画面全体を1つの受付領域として押下の有無を見るだけでなく、短タップ確定時には
// 論理座標(600x1024)も返す(予定枠のタップ判定に使う)。

enum class TouchEvent {
    None,
    Press,   // 押し始めた瞬間
    Tap,     // 指を離した瞬間に確定する短いタップ。LongTapを返した後の解放では返さない
    LongTap, // 押したままTOUCH_LONG_TAP_MSに達した瞬間
};

// ロングタップと判定するまでの押下継続時間(ms)。
static const uint32_t TOUCH_LONG_TAP_MS = 1500;

// タップと認めるスライド量の上限(論理座標のpx)。これを超えて動いたらTapを返さない。
static const int TOUCH_TAP_MOVE_MAX_PX = 30;

// GT911を初期化する。I2Cバスはio_ext.hのioExtGetBus()から借りるので、
// ioExtBegin()より後に呼ぶこと。失敗してもtouchPoll()は常にTouchEvent::Noneを
// 返すので、呼び出し側は表示を止めなくてよい。
bool touchBegin();

// 現在のタッチ状態を読み、状態遷移からイベントを返す。メインループから
// 定期的に呼ぶこと。1回のタッチ(押してから離すまで)で返るのは、押し始めの
// Press(1回)と、押し続けた場合のLongTap(1回)、動かさずに離した場合のTap(1回)の
// いずれかだけで、それ以外は常にNoneを返す。
// Pressの直後に同じタッチのLongTapが続くことがあるので、呼び出し側は
// 「Pressで行った操作を、同じタッチのLongTapで取り消してしまわないか」を
// 必ず確認すること(main.cppのsuppress_longtapがその抑制にあたる)。
//
// 座標は論理座標(600x1024)。PressとTapのときに書き込む。不要ならnullptrを渡してよい。
TouchEvent touchPoll(int* out_x, int* out_y);
