#include "time_sync.h"

#include "esp_netif_sntp.h"

namespace {

// sync_cbはlwIPのSNTPタスクから呼ばれるため、mainループ側と共有するフラグは
// volatileにしておく。
volatile bool g_synced = false;
bool          g_started = false;

void onTimeSync(struct timeval* /*tv*/) {
    g_synced = true;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────

esp_err_t timeSyncBegin(const char* server) {
    if (server == nullptr) return ESP_ERR_INVALID_ARG;
    if (g_started) return ESP_OK; // 多重初期化しない

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(server);
    cfg.sync_cb           = onTimeSync;

    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) return err;

    g_started = true;
    return ESP_OK;
}

bool timeSyncIsSynced() {
    return g_synced;
}
