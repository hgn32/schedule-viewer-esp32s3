#pragma once
#include <cstdint>
#include <string>
#include "esp_err.h"

// 予定取得APIへのHTTPS GET。esp_http_client + esp_crt_bundleを使う。
//
// 証明書はESP-IDF同梱の公的CAバンドル(esp_crt_bundle)で検証する。
// サーバ証明書は*.asahi-kasei.co.jp ← GlobalSign RSA OV SSL CA 2018 ←
// GlobalSign Root CA - R3で、いずれもバンドルに含まれるため埋め込みは不要。

struct HttpResponse {
    int         status = 0;   // HTTPステータス。0は接続そのものに失敗した場合
    std::string body;         // レスポンス本文(JSONを想定)
    uint32_t    date_utc = 0; // Dateヘッダから得たサーバ時刻のUTC epoch。取れなければ0
    std::string location;     // 3xxのときのLocationヘッダ(認証リダイレクトの切り分け用)
};

// urlへGETしてoutに結果を入れる。
// リダイレクトは追わない(302はEntra IDのログインへ飛ばされるだけで、
// 追うと巨大なHTMLを掴まされる。呼び出し側にstatusとLocationで判断させる)。
// url / outがnullptrならESP_ERR_INVALID_ARG。
// max_body_bytesを超えた分は捨てる(0を渡すと既定の64KB)。
esp_err_t httpGetJson(const char* url, HttpResponse* out, uint32_t timeout_ms,
                      size_t max_body_bytes = 0);
