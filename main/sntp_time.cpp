#include "sntp_time.h"

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"

#include "secrets.h"
#include "time_util.h"

static const char* TAG = "sntp_time";

// secrets.hでSNTP_SERVERが定義されていればそれを、無ければpool.ntp.orgを使う。
#if !defined(SNTP_SERVER)
#define SNTP_SERVER "pool.ntp.org"
#endif

esp_err_t sntpSyncTime(uint32_t timeout_ms) {
    static bool s_initialized = false;

    if (!s_initialized) {
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(SNTP_SERVER);
        esp_err_t err = esp_netif_sntp_init(&config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_netif_sntp_initに失敗した: %s", esp_err_to_name(err));
            return err;
        }
        s_initialized = true;
    } else {
        // 2回目以降は再初期化せず、サービスだけ再スタートする(二重初期化を避ける)。
        esp_netif_sntp_start();
    }

    esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP同期に失敗した: %s", esp_err_to_name(err));
        return err;
    }

    struct tm jst = nowJst();
    ESP_LOGI(TAG, "SNTP同期完了(JST): %s %s", formatDate(jst).c_str(), formatTime(jst).c_str());
    return ESP_OK;
}
