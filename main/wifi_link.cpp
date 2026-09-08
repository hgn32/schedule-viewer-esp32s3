#include "wifi_link.h"

#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"

static const char* TAG = "wifi_link";

// APからの切断でどこまで粘るか。これを超えたら諦めてイベントグループへ失敗を立てる。
// (電源投入直後にAPが見えないだけのこともあるので即座には諦めない)
static const int MAX_RETRY = 8;

static const int BIT_CONNECTED = BIT0;
static const int BIT_FAILED    = BIT1;

static EventGroupHandle_t s_events     = nullptr;
static int                s_retry      = 0;
static bool               s_started    = false;
static esp_ip4_addr_t     s_ip         = {};
static esp_netif_t*       s_netif      = nullptr;

// ─────────────────────────────────────────────────────────────────────────────

static void onWifiEvent(void* arg, esp_event_base_t base, int32_t id, void* data) {
    (void)arg;
    (void)data;

    if (base != WIFI_EVENT) return;

    if (id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        s_ip.addr = 0;
        xEventGroupClearBits(s_events, BIT_CONNECTED);

        if (s_retry < MAX_RETRY) {
            s_retry++;
            ESP_LOGW(TAG, "APから切断された。再接続する(%d/%d)", s_retry, MAX_RETRY);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "再接続の上限(%d回)に達した", MAX_RETRY);
            xEventGroupSetBits(s_events, BIT_FAILED);
        }
    }
}

static void onIpEvent(void* arg, esp_event_base_t base, int32_t id, void* data) {
    (void)arg;

    if (base != IP_EVENT || id != IP_EVENT_STA_GOT_IP) return;
    if (data == nullptr) return;

    auto* event = (ip_event_got_ip_t*)data;
    s_ip        = event->ip_info.ip;
    s_retry     = 0;
    ESP_LOGI(TAG, "IPv4取得: " IPSTR, IP2STR(&s_ip));

    xEventGroupClearBits(s_events, BIT_FAILED);
    xEventGroupSetBits(s_events, BIT_CONNECTED);
}

// ─────────────────────────────────────────────────────────────────────────────

// DNSサーバを1つ設定する。addrがnullptrか空文字なら何もしない(未指定として扱う)。
static esp_err_t setDnsServer(esp_netif_t* netif, esp_netif_dns_type_t type, const char* addr) {
    if (netif == nullptr) return ESP_ERR_INVALID_ARG;
    if (addr == nullptr || addr[0] == '\0') return ESP_OK;

    esp_netif_dns_info_t dns = {};
    dns.ip.type              = ESP_IPADDR_TYPE_V4;
    if (esp_netif_str_to_ip4(addr, &dns.ip.u_addr.ip4) != ESP_OK) {
        ESP_LOGE(TAG, "DNSアドレスの書式が不正: %s", addr);
        return ESP_ERR_INVALID_ARG;
    }
    return esp_netif_set_dns_info(netif, type, &dns);
}

// DHCPクライアントを止めて固定IPを設定する。
// 呼ぶ順番に意味がある。esp_netif_set_ip_info()は内部でdns_clear_servers()を呼ぶので、
// DNSを先に入れると消える。必ずIP設定の後にDNSを入れる。
static esp_err_t applyStaticIp(esp_netif_t* netif, const WifiStaticIp* cfg) {
    if (netif == nullptr || cfg == nullptr) return ESP_ERR_INVALID_ARG;
    if (cfg->ip == nullptr || cfg->gateway == nullptr || cfg->netmask == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    // 起動直後のDHCPクライアントはINIT状態にある。停止済みならその旨が返るが
    // 目的は果たしているので通す。
    esp_err_t err = esp_netif_dhcpc_stop(netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGE(TAG, "DHCPクライアントを停止できない: %s", esp_err_to_name(err));
        return err;
    }

    esp_netif_ip_info_t info = {};
    if (esp_netif_str_to_ip4(cfg->ip, &info.ip) != ESP_OK ||
        esp_netif_str_to_ip4(cfg->gateway, &info.gw) != ESP_OK ||
        esp_netif_str_to_ip4(cfg->netmask, &info.netmask) != ESP_OK) {
        ESP_LOGE(TAG, "固定IPの書式が不正: ip=%s gw=%s mask=%s", cfg->ip, cfg->gateway,
                 cfg->netmask);
        return ESP_ERR_INVALID_ARG;
    }

    err = esp_netif_set_ip_info(netif, &info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "固定IPを設定できない: %s", esp_err_to_name(err));
        return err;
    }

    err = setDnsServer(netif, ESP_NETIF_DNS_MAIN, cfg->dns1);
    if (err != ESP_OK) return err;
    err = setDnsServer(netif, ESP_NETIF_DNS_BACKUP, cfg->dns2);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "固定IPを使う: ip=%s mask=%s gw=%s dns=%s", cfg->ip, cfg->netmask, cfg->gateway,
             (cfg->dns1 != nullptr && cfg->dns1[0] != '\0') ? cfg->dns1 : "(未設定)");
    return ESP_OK;
}

esp_err_t wifiLinkBegin(const char* ssid, const char* password, uint32_t timeout_ms,
                        const WifiStaticIp* static_ip) {
    if (ssid == nullptr || password == nullptr) return ESP_ERR_INVALID_ARG;
    if (s_started) return wifiLinkWaitConnected(timeout_ms) ? ESP_OK : ESP_ERR_TIMEOUT;

    // esp_wifiはキャリブレーションデータの保存にNVSを使う。
    // パーティションが古い/壊れている場合は消して作り直す。
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    s_events = xEventGroupCreate();
    if (s_events == nullptr) return ESP_ERR_NO_MEM;

    err = esp_netif_init();
    if (err != ESP_OK) return err;

    // 既定のイベントループが既に作られていればESP_ERR_INVALID_STATEが返る
    // (default_event_loop.cで確認済み)。他所が先に作っていても続行してよい。
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    s_netif = esp_netif_create_default_wifi_sta();
    if (s_netif == nullptr) return ESP_FAIL;

    if (static_ip != nullptr) {
        err = applyStaticIp(s_netif, static_ip);
        if (err != ESP_OK) return err;
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) return err;

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              &onWifiEvent, nullptr, nullptr);
    if (err != ESP_OK) return err;
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              &onIpEvent, nullptr, nullptr);
    if (err != ESP_OK) return err;

    wifi_config_t wifi_cfg = {};
    // strlcpyでSSID(32byte)/パスワード(64byte)の固定長配列を溢れさせない。
    strlcpy((char*)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char*)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_cfg.sta.pmf_cfg.capable    = true;
    wifi_cfg.sta.pmf_cfg.required   = false;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (err != ESP_OK) return err;

    // 省電力を切る。既定のWIFI_PS_MIN_MODEmは待ち受けの応答が鈍り、
    // 10分に1回のポーリングでは節電の効果より接続の不安定さのほうが目立つ。
    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) return err;

    err = esp_wifi_start();
    if (err != ESP_OK) return err;

    s_started = true;
    ESP_LOGI(TAG, "SSID '%s' へ接続中...", ssid);

    return wifiLinkWaitConnected(timeout_ms) ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool wifiLinkIsConnected() {
    if (s_events == nullptr) return false;
    return (xEventGroupGetBits(s_events) & BIT_CONNECTED) != 0;
}

bool wifiLinkWaitConnected(uint32_t timeout_ms) {
    if (s_events == nullptr) return false;

    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_CONNECTED | BIT_FAILED,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    if (bits & BIT_CONNECTED) return true;

    if (bits & BIT_FAILED) {
        // 上限まで再接続して駄目だった状態。次の呼び出しでまた試せるよう戻す。
        s_retry = 0;
        xEventGroupClearBits(s_events, BIT_FAILED);
        esp_wifi_connect();
    }
    return false;
}

void wifiLinkGetIp(char* buf, size_t len) {
    if (buf == nullptr || len < 16) return;
    snprintf(buf, len, IPSTR, IP2STR(&s_ip));
}
