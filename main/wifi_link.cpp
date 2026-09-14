#include "wifi_link.h"

#include <cstring>
#include <vector>

#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"

// ブロッキングスキャンで拾うAP数の上限。ヒープ確保を有限に保つための安全装置。
static const uint16_t MAX_SCAN_APS = 20;

static const int BIT_CONNECTED = BIT0;
static const int BIT_FAILED    = BIT1;

static EventGroupHandle_t s_events     = nullptr;
static bool               s_started    = false;
static bool               s_probing    = false;  // 候補を順に試している最中か
static esp_ip4_addr_t     s_ip         = {};
static esp_netif_t*       s_netif      = nullptr;
static char               s_ssid[33]   = {};      // 最後に接続確立したSSID

// wifiLinkBegin()で記憶した候補。以後のwifiLinkConnectRound()すべてで使う。
static std::vector<WifiCandidate> s_candidates;
static uint32_t                   s_timeout_ms_each = 0;

// 直近の接続試行の状況。画面へ出して原因を切り分けるためだけに保持する。
static char     s_try_ssid[33]   = {};  // 最後に試した候補のSSID
static bool     s_try_scanned    = false;
static uint16_t s_scan_count     = 0;   // 直前のスキャンで見つかったAP総数
static uint8_t  s_last_reason    = 0;   // 直近のWIFI_EVENT_STA_DISCONNECTEDのreason

// ─────────────────────────────────────────────────────────────────────────────

static void onWifiEvent(void* arg, esp_event_base_t base, int32_t id, void* data) {
    (void)arg;

    if (base != WIFI_EVENT) return;

    if (id == WIFI_EVENT_STA_START) {
        // ここではesp_wifi_connect()を呼ばない。ブロッキングスキャンと競合するため。
        // 接続はwifiLinkBegin()が候補を決めてから明示的に行う。
        return;
    }

    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        // 原因の切り分けのため理由コードを保存する。画面へ出す用途。
        if (data != nullptr) {
            s_last_reason = ((wifi_event_sta_disconnected_t*)data)->reason;
        }

        s_ip.addr = 0;
        xEventGroupClearBits(s_events, BIT_CONNECTED);

        if (s_probing) {
            // 候補を試している最中は再接続で粘らず、即座に次の候補へ切り替える。
            xEventGroupSetBits(s_events, BIT_FAILED);
            return;
        }

        // 運用中は上限を設けず、繋がるまで無制限に再接続する。
        esp_wifi_connect();
    }
}

static void onIpEvent(void* arg, esp_event_base_t base, int32_t id, void* data) {
    (void)arg;

    if (base != IP_EVENT || id != IP_EVENT_STA_GOT_IP) return;
    if (data == nullptr) return;

    auto* event = (ip_event_got_ip_t*)data;
    s_ip        = event->ip_info.ip;

    // 接続できた時点で古い切断理由を残さない。
    s_last_reason = 0;

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
        return err;
    }

    esp_netif_ip_info_t info = {};
    if (esp_netif_str_to_ip4(cfg->ip, &info.ip) != ESP_OK ||
        esp_netif_str_to_ip4(cfg->gateway, &info.gw) != ESP_OK ||
        esp_netif_str_to_ip4(cfg->netmask, &info.netmask) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    err = esp_netif_set_ip_info(netif, &info);
    if (err != ESP_OK) {
        return err;
    }

    err = setDnsServer(netif, ESP_NETIF_DNS_MAIN, cfg->dns1);
    if (err != ESP_OK) return err;
    err = setDnsServer(netif, ESP_NETIF_DNS_BACKUP, cfg->dns2);
    if (err != ESP_OK) return err;

    return ESP_OK;
}

// AP一覧の中に指定SSIDが見つかるか。ap.ssidは32byteの固定長で、ちょうど32文字の
// ときは末尾がNUL終端されないため、strnlenで実長を出してから比較する。
static bool ssidFoundIn(const std::vector<wifi_ap_record_t>& aps, const char* ssid) {
    size_t want_len = strlen(ssid);
    for (const auto& ap : aps) {
        size_t ap_len = strnlen((const char*)ap.ssid, sizeof(ap.ssid));
        if (ap_len == want_len && memcmp(ap.ssid, ssid, want_len) == 0) return true;
    }
    return false;
}

// ブロッキングスキャンを行い、見つかったAPを返す。スキャン自体が失敗したら
// 空のvectorを返す(呼び出し側は定義順の総当たりへフォールバックする)。
static std::vector<wifi_ap_record_t> scanAccessPoints() {
    std::vector<wifi_ap_record_t> result;

    esp_err_t err = esp_wifi_scan_start(nullptr, true);
    if (err != ESP_OK) {
        // スキャン自体が失敗した場合は定義順の総当たりへフォールバックする。
        s_scan_count = 0;
        return result;
    }

    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    if (ap_num == 0) {
        s_scan_count = 0;
        return result;
    }
    // 画面へ出すAP総数はバッファ上限で切り詰める前の値を使う。
    s_scan_count = ap_num;
    if (ap_num > MAX_SCAN_APS) ap_num = MAX_SCAN_APS;

    result.resize(ap_num);
    uint16_t actual = ap_num;
    err             = esp_wifi_scan_get_ap_records(&actual, result.data());
    if (err != ESP_OK) {
        // スキャン結果の取得に失敗した場合も定義順の総当たりへフォールバックする。
        result.clear();
        s_scan_count = 0;
        return result;
    }
    result.resize(actual);
    return result;
}

// 候補の試行順を決める。スキャンで見つかった候補を定義順のまま前に、
// 見つからなかった候補を定義順のまま後ろに置く(安定な並べ替え)。
static std::vector<size_t> orderCandidates(const WifiCandidate* candidates, size_t count,
                                           const std::vector<wifi_ap_record_t>& scanned) {
    std::vector<size_t> order;
    order.reserve(count);
    for (size_t i = 0; i < count; i++) {
        if (candidates[i].ssid == nullptr) continue;
        if (ssidFoundIn(scanned, candidates[i].ssid)) order.push_back(i);
    }
    for (size_t i = 0; i < count; i++) {
        if (candidates[i].ssid == nullptr) continue;
        if (!ssidFoundIn(scanned, candidates[i].ssid)) order.push_back(i);
    }
    return order;
}

// 1つの候補への接続を試す。timeout_ms待って接続できたらtrue。
// scannedは直前のスキャンでこの候補が実際に見えていたか(画面表示用)。
static bool tryCandidate(const WifiCandidate& cand, uint32_t timeout_ms, bool scanned) {
    strlcpy(s_try_ssid, cand.ssid, sizeof(s_try_ssid));
    s_try_scanned = scanned;

    // 切断イベントが「運用中の無制限再接続」の分岐へ落ちてこの試行と競合しないよう、
    // esp_wifi_disconnect()より前にs_probingを立てる。
    s_probing = true;

    // 前の候補が残したBIT_FAILEDを先に落とす。これをやらないと、この直後の
    // 「切断イベントを吸う待ち」が古いビットで即座に戻ってしまい、吸う意味が無くなる。
    xEventGroupClearBits(s_events, BIT_CONNECTED | BIT_FAILED);

    // 未接続でも失敗にしない。前の候補が繋がりかけていた状態を確実に落とすため。
    esp_wifi_disconnect();

    // ここで出る切断イベントを吸っておく。吸わずに先へ進むと、遅れて届いた
    // 切断イベントが下のクリアの後にBIT_FAILEDを立ててしまい、この後の接続試行が
    // まったく待たずに即失敗する。切断イベントが出ないこともあるので有限待ちにする。
    xEventGroupWaitBits(s_events, BIT_FAILED, pdFALSE, pdFALSE, pdMS_TO_TICKS(500));

    esp_err_t net_err;
    if (cand.static_ip != nullptr) {
        net_err = applyStaticIp(s_netif, cand.static_ip);
    } else {
        net_err = esp_netif_dhcpc_start(s_netif);
        if (net_err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) net_err = ESP_OK;
    }
    if (net_err != ESP_OK) {
        s_probing = false;
        return false;
    }

    wifi_config_t wifi_cfg = {};
    // strlcpyでSSID(32byte)/パスワード(64byte)の固定長配列を溢れさせない。
    strlcpy((char*)wifi_cfg.sta.ssid, cand.ssid, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char*)wifi_cfg.sta.password, cand.password, sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_cfg.sta.pmf_cfg.capable    = true;
    wifi_cfg.sta.pmf_cfg.required   = false;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (err != ESP_OK) {
        s_probing = false;
        return false;
    }

    xEventGroupClearBits(s_events, BIT_CONNECTED | BIT_FAILED);

    err = esp_wifi_connect();
    if (err != ESP_OK) {
        s_probing = false;
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_CONNECTED | BIT_FAILED, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    s_probing = false;
    if (bits & BIT_CONNECTED) {
        strlcpy(s_ssid, cand.ssid, sizeof(s_ssid));
        return true;
    }

    return false;
}

esp_err_t wifiLinkBegin(const WifiCandidate* candidates, size_t count, uint32_t timeout_ms_each) {
    if (candidates == nullptr || count == 0) return ESP_ERR_INVALID_ARG;
    if (s_started) return ESP_OK;  // 初期化は一度だけ。候補の記憶も済んでいる。

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

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err                         = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) return err;

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &onWifiEvent, nullptr,
                                              nullptr);
    if (err != ESP_OK) return err;
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &onIpEvent, nullptr,
                                              nullptr);
    if (err != ESP_OK) return err;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) return err;

    // 省電力を切る。既定のWIFI_PS_MIN_MODEmは待ち受けの応答が鈍り、
    // 10分に1回のポーリングでは節電の効果より接続の不安定さのほうが目立つ。
    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) return err;

    err = esp_wifi_start();
    if (err != ESP_OK) return err;
    s_started = true;

    // 候補を記憶する。secrets.hの文字列リテラルを指すだけなので、
    // 呼び出し元の配列(スタック上)が後で消えても問題ない。
    s_candidates.assign(candidates, candidates + count);
    s_timeout_ms_each = timeout_ms_each;

    return ESP_OK;
}

bool wifiLinkConnectRound() {
    if (s_candidates.empty()) return false;

    // 候補のタイムアウト直後に接続が成立していることがある。ここで見ずに
    // 次のesp_wifi_disconnect()へ進むと、繋がった接続を自分から叩き落として
    // 永久に繋がらなくなる。
    if (wifiLinkIsConnected()) return true;

    // 見えている候補を優先する。
    std::vector<wifi_ap_record_t> scanned = scanAccessPoints();
    std::vector<size_t>           order =
        orderCandidates(s_candidates.data(), s_candidates.size(), scanned);

    for (size_t oi = 0; oi < order.size(); oi++) {
        const WifiCandidate& cand = s_candidates[order[oi]];
        if (cand.ssid == nullptr || cand.password == nullptr) continue;
        bool was_scanned = ssidFoundIn(scanned, cand.ssid);
        if (tryCandidate(cand, s_timeout_ms_each, was_scanned)) return true;
    }

    s_probing = false;
    return false;
}

bool wifiLinkIsConnected() {
    if (s_events == nullptr) return false;
    return (xEventGroupGetBits(s_events) & BIT_CONNECTED) != 0;
}

bool wifiLinkWaitConnected(uint32_t timeout_ms) {
    if (s_events == nullptr) return false;
    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_CONNECTED, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    return (bits & BIT_CONNECTED) != 0;
}

void wifiLinkGetIp(char* buf, size_t len) {
    if (buf == nullptr || len < 16) return;
    snprintf(buf, len, IPSTR, IP2STR(&s_ip));
}

void wifiLinkGetSsid(char* buf, size_t len) {
    if (buf == nullptr || len == 0) return;
    snprintf(buf, len, "%s", s_ssid);
}

// esp_read_mac()はeFuseから読むので、esp_wifi_init()の前後どちらでも使える
// (esp_wifi_get_mac()と違いWi-Fiの起動状態に依存しない)。
void wifiLinkGetMac(char* buf, size_t len) {
    if (buf == nullptr || len < 18) return;

    uint8_t mac[6] = {};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        snprintf(buf, len, "取得失敗");
        return;
    }
    snprintf(buf, len, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4],
             mac[5]);
}

void wifiLinkGetLastAttempt(WifiAttemptInfo* out) {
    if (out == nullptr) return;
    snprintf(out->ssid, sizeof(out->ssid), "%s", s_try_ssid);
    out->scanned  = s_try_scanned;
    out->ap_count = s_scan_count;
    out->reason   = s_last_reason;
}

// Wi-Fiの切断理由コード(wifi_err_reason_t)を短い日本語へ変換する。
// 画面の1行(全角10文字以内)に収まる範囲で、原因の切り分けに要る情報だけを返す。
const char* wifiLinkDescribeReason(uint8_t reason) {
    switch (reason) {
        case 1:   return "不明な理由";
        case 2:   return "認証の期限切れ";
        case 3:   return "AP側から切断";
        case 4:   return "無通信で切断";
        case 8:   return "AP側から切断";
        case 14:  return "MIC不一致";
        case 15:  return "4wayタイムアウト";
        case 16:  return "鍵更新の失敗";
        case 23:  return "802.1X認証失敗";
        case 200: return "ビーコン喪失";
        case 201: return "APが見つからない";
        case 202: return "認証失敗";
        case 203: return "接続要求の失敗";
        case 204: return "ハンドシェイク失敗";
        case 205: return "接続失敗";
        case 206: return "AP再起動";
        case 210: return "暗号方式が不一致";
        case 211: return "認証方式が下限未満";
        case 212: return "電波が弱い";
        default:  return "不明";
    }
}
