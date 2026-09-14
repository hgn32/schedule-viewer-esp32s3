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

// NVS/netif/イベント登録/STAモード/省電力OFF/esp_wifi_start()までの初期化だけを行う。
// 接続そのものはここでは行わない(呼び出し側がwifiLinkConnectRound()を繰り返す)。
// candidatesは内部へコピーして保持し、以後のラウンドすべてで使う
// (secrets.hの文字列リテラルを指すため、ポインタの寿命は問題ない)。
// candidatesがnullptr、countが0、初期化に失敗したらエラーを返す。
esp_err_t wifiLinkBegin(const WifiCandidate* candidates, size_t count, uint32_t timeout_ms_each);

// 記憶した候補で接続を1巡だけ試す。まずスキャンして実際に見えている候補を優先し、
// スキャンに出なかった候補(ステルスSSID)も後から試す。
// 1つでも接続できたらtrueを返す。全滅したらfalseを返すので、
// 呼び出し側はfalseが返る間、繰り返しこの関数を呼んで諦めずに再試行する。
bool wifiLinkConnectRound();

// 現在APに接続済みでIPv4アドレスを持っているか。
bool wifiLinkIsConnected();

// 運用中に切断された後、再接続(自動で無制限に試行される)が
// 完了するのを待つ。接続済みなら即trueを返す。timeout_ms内に
// 繋がらなければfalseを返す(再接続の試行自体は継続する)。
bool wifiLinkWaitConnected(uint32_t timeout_ms);

// 取得済みのIPv4アドレスを"10.0.0.2"形式で返す。未取得なら"0.0.0.0"。
// bufがnullptrまたはlenが16未満なら何もしない。
void wifiLinkGetIp(char* buf, size_t len);

// 接続中のSSIDを返す。未接続なら空文字。bufがnullptrかlenが0なら何もしない。
void wifiLinkGetSsid(char* buf, size_t len);

// 直近の接続試行の結果。原因の切り分けのため画面へ出す用途。
struct WifiAttemptInfo {
    char     ssid[33];   // 最後に試した候補のSSID。まだ試していなければ空文字
    bool     scanned;    // その候補が直前のスキャンで実際に見えていたか
    uint16_t ap_count;   // 直前のスキャンで見つかったAPの総数(0ならスキャン自体が空振り)
    uint8_t  reason;     // 直近のWIFI_EVENT_STA_DISCONNECTEDのreason。未発生なら0
};

void wifiLinkGetLastAttempt(WifiAttemptInfo* out);

// Wi-Fi(STA)のMACアドレスを"14:C1:9F:59:D4:34"形式で返す。
// 端末のMAC登録が要るネットワーク向けに、画面へ出して控えられるようにする。
// bufがnullptrかlenが18未満なら何もしない。読み出しに失敗したら"取得失敗"を返す。
void wifiLinkGetMac(char* buf, size_t len);
const char* wifiLinkDescribeReason(uint8_t reason);
