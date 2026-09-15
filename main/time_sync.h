#pragma once
#include "esp_err.h"

// SNTPによる時刻同期。RTCが無いため、起動のたびにここで得た時刻を
// システムクロックへ反映する(esp_netif_sntpが内部でsettimeofday()する)。
// 時刻源はここだけ。HTTPSレスポンスのDateヘッダやJSON本文からは取らない。

// serverへの同期を開始する(非同期。完了はtimeSyncIsSynced()で確認する)。
// 2回目以降の呼び出しは何もせずESP_OKを返す(多重初期化しない)。
// serverがnullptrならESP_ERR_INVALID_ARG。
esp_err_t timeSyncBegin(const char* server);

// 一度でも同期が完了していればtrue。
bool timeSyncIsSynced();
