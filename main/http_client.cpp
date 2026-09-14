#include "http_client.h"

#include <cstring>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"

#include "time_util.h"

// 本文の既定の上限。PSRAMがあるので余裕はあるが、
// 認証リダイレクト先のHTMLなど想定外の巨大レスポンスで詰まらないよう蓋をする。
static const size_t DEFAULT_MAX_BODY = 64 * 1024;

namespace {

struct FetchContext {
    HttpResponse* out;
    size_t        max_body;
};

esp_err_t onHttpEvent(esp_http_client_event_t* evt) {
    if (evt == nullptr || evt->user_data == nullptr) return ESP_OK;
    auto* ctx = (FetchContext*)evt->user_data;

    switch (evt->event_id) {
        case HTTP_EVENT_ON_HEADER:
            if (evt->header_key == nullptr || evt->header_value == nullptr) break;
            // Dateヘッダはサーバの現在時刻。RTCが飛んでいるときの時刻源に使う。
            if (strcasecmp(evt->header_key, "Date") == 0) {
                uint32_t epoch = 0;
                if (parseHttpDate(evt->header_value, &epoch)) {
                    ctx->out->date_utc = epoch;
                }
            } else if (strcasecmp(evt->header_key, "Location") == 0) {
                ctx->out->location = evt->header_value;
            }
            break;

        case HTTP_EVENT_ON_DATA: {
            if (evt->data == nullptr || evt->data_len <= 0) break;
            size_t room = ctx->max_body > ctx->out->body.size()
                              ? ctx->max_body - ctx->out->body.size()
                              : 0;
            if (room == 0) {
                break;
            }
            size_t take = (size_t)evt->data_len < room ? (size_t)evt->data_len : room;
            ctx->out->body.append((const char*)evt->data, take);
            break;
        }

        default:
            break;
    }
    return ESP_OK;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────

esp_err_t httpGetJson(const char* url, HttpResponse* out, uint32_t timeout_ms,
                      size_t max_body_bytes) {
    if (url == nullptr || out == nullptr) return ESP_ERR_INVALID_ARG;

    out->status   = 0;
    out->date_utc = 0;
    out->body.clear();
    out->location.clear();

    FetchContext ctx = {};
    ctx.out          = out;
    ctx.max_body     = max_body_bytes > 0 ? max_body_bytes : DEFAULT_MAX_BODY;

    // reserveしておくと本文追記のたびに再確保しないで済む。
    out->body.reserve(8 * 1024);

    esp_http_client_config_t cfg = {};
    cfg.url                   = url;
    cfg.method                = HTTP_METHOD_GET;
    cfg.timeout_ms            = (int)timeout_ms;
    cfg.event_handler         = onHttpEvent;
    cfg.user_data             = &ctx;
    cfg.crt_bundle_attach     = esp_crt_bundle_attach;
    cfg.disable_auto_redirect = true; // 302の中身ではなくLocationを見て判断したい
    cfg.buffer_size           = 4096;
    cfg.buffer_size_tx        = 1024;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Accept", "application/json");

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        out->status = esp_http_client_get_status_code(client);
    }

    esp_http_client_cleanup(client);
    return err;
}
