#pragma once
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

// NVS / esp_netif / esp_event / esp_wifiを初期化してAPへ接続する。
// timeout_ms待って接続できなければESP_ERR_TIMEOUTを返す(内部でリトライする)。
// ssid / passwordがnullptrならESP_ERR_INVALID_ARG。
//
// static_ipがnullptrならDHCPでアドレスを取得する。非nullptrならDHCPクライアントを
// 止めて固定IPを設定する。固定IPの場合もesp_netifがIP_EVENT_STA_GOT_IPを上げるため、
// 待ち合わせの流れはDHCPのときと変わらない。
esp_err_t wifiLinkBegin(const char* ssid, const char* password, uint32_t timeout_ms,
                        const WifiStaticIp* static_ip = nullptr);

// 現在APに接続済みでIPv4アドレスを持っているか。
bool wifiLinkIsConnected();

// 切断された後に再接続を待つ。接続済みなら即trueを返す。
bool wifiLinkWaitConnected(uint32_t timeout_ms);

// 取得済みのIPv4アドレスを"10.0.0.2"形式で返す。未取得なら"0.0.0.0"。
// bufがnullptrまたはlenが16未満なら何もしない。
void wifiLinkGetIp(char* buf, size_t len);
