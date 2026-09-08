#pragma once
#include <cstdint>
#include <string>

// PC(pc_python/scheduler_sender.py)とのUSBシリアル接続。
// Arduinoの`Serial`を置き換えるだけの薄いラッパで、プロトコルの解釈はProtocolが持つ。
//
// UART0(コンソールと同じ)を115200bpsで使う。ESP_LOG*の出力も同じ線に乗るが、
// PC側は完全一致で"REQ:ALL"だけを拾うため実害は無い。

void serialLinkBegin();

// 改行までを1行として取り出す。末尾のCR/空白は落とす。
// timeout_ms待って行が揃わなければfalse。
bool serialLinkReadLine(std::string& out, uint32_t timeout_ms);

// 1行送信(末尾に'\n'を付ける)。
void serialLinkWriteLine(const char* line);
