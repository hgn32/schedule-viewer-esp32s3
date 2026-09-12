#pragma once
#include <cstdint>

#include "esp_err.h"

// 一時的なダミー予定表示モード用(main/secrets.hのUSE_DUMMY_SCHEDULEが1のときだけ使う)。
// 通常モードはHTTP DateヘッダとJSON本文の時刻で足りるためSNTPは呼ばない
// (社内ネットワークからNTPへ出られるかは未確認)。
// サーバへの到達性が確認できたら、main/dummy_schedule.cpp/.hと合わせて撤去してよい。

// SNTPで実時刻を取得しシステムクロックへ反映する。
// Wi-Fi接続済みであることが前提だが、未接続でもタイムアウトするだけで落ちない。
// timeout_ms以内に同期できればESP_OK、できなければESP_ERR_TIMEOUTを返す。
esp_err_t sntpSyncTime(uint32_t timeout_ms);
