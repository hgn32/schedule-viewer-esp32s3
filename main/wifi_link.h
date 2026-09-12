#pragma once
#include <cstddef>
#include <cstdint>
#include "esp_err.h"

// Wi-Fi(STA / WPA2-PSK)接続。予定データをサーバから取得するためだけに使う。
//
// このプロジェクトは元々USBシリアルのみで通信していたが、
// サーバのREST APIから予定を取得する方式へ変更したためSTAを有効にした。
// AP(親機)にはならず、Bluetoothも使わない。

// 固定IPで接続するときの設定。値はすべて"10.0.0.2"形式の文字列。
// dns2が不要ならnullptrか空文字にする(dns1は名前解決に必須)。
struct WifiStaticIp {
    const char* ip;
    const char* gateway;
    const char* netmask;
    const char* dns1;
    const char* dns2;
};

// 接続候補1つ分。static_ipがnullptrならDHCPでアドレスを取得する。
struct WifiCandidate {
    const char* ssid;
    const char* password;
    const WifiStaticIp* static_ip;
};

// 候補を順に試してAPへ接続する。先にスキャンして実際に見えている候補を優先し、
// スキャンに出なかった候補(ステルスSSID)も後から試す。
// 候補1つあたりtimeout_ms_each待つ。全候補が失敗したらESP_ERR_TIMEOUTを返す。
// candidatesがnullptrかcountが0ならESP_ERR_INVALID_ARG。
esp_err_t wifiLinkBegin(const WifiCandidate* candidates, size_t count, uint32_t timeout_ms_each);

// 現在APに接続済みでIPv4アドレスを持っているか。
bool wifiLinkIsConnected();

// 切断された後に再接続を待つ。接続済みなら即trueを返す。
bool wifiLinkWaitConnected(uint32_t timeout_ms);

// 取得済みのIPv4アドレスを"10.0.0.2"形式で返す。未取得なら"0.0.0.0"。
// bufがnullptrまたはlenが16未満なら何もしない。
void wifiLinkGetIp(char* buf, size_t len);

// 接続中のSSIDを返す。未接続なら空文字。bufがnullptrかlenが0なら何もしない。
void wifiLinkGetSsid(char* buf, size_t len);
