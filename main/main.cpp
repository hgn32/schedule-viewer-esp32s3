#include <stdlib.h>   // setenv (POSIX)

#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "display.h"
#include "http_client.h"
#include "io_ext.h"
#include "json_parser.h"
#include "lcd_panel.h"
#include "protocol.h"
#include "schedule.h"
#include "secrets.h"
#include "serial_link.h"
#include "time_util.h"
#include "wifi_link.h"

static const char* TAG = "main";

// Wi-Fi接続を待つ上限。社内APは認証に時間がかかることがあるので長めに取る。
static const uint32_t WIFI_TIMEOUT_MS = 20000;
// HTTPS 1回あたりの上限。TLSハンドシェイクを含む。
static const uint32_t HTTP_TIMEOUT_MS = 15000;
// 取得に失敗したときの再試行間隔。POLL_INTERVAL_SEC(既定300)より短くする。
static const uint32_t RETRY_INTERVAL_SEC = 60;
// メインループの1ティック(シリアル受信待ちのタイムアウト)。
static const uint32_t LOOP_TICK_MS = 100;
// 起動後、表示ができてから点灯するバックライトの輝度(%)。
static const uint8_t BACKLIGHT_PERCENT = 80;

// ─────────────────────────────────────────────────────────────────────────────

// 次の取得時刻を壁時計の境界に合わせる。POLL_INTERVAL_SEC=300なら
// X:00 / X:05 / X:10…になる。旧版ではPC側(scheduler_sender.py)が
// 同じ計算をして送信していたので、その挙動をデバイス側へ移したもの。
// JSTのオフセットは時間単位なので、UTCで割っても境界は一致する。
static uint32_t nextAlignedUtc(uint32_t now, uint32_t step_sec) {
    if (step_sec == 0) return now;
    return (now / step_sec + 1) * step_sec;
}

// サーバから予定を取得してstoreを更新する。表示は呼び出し側で行う。
static bool fetchSchedule(ScheduleStore* store) {
    if (store == nullptr) return false;

    if (!wifiLinkIsConnected() && !wifiLinkWaitConnected(WIFI_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "Wi-Fi未接続のため取得を見送る");
        return false;
    }

    HttpResponse res;
    esp_err_t err = httpGetJson(SCHEDULE_URL, &res, HTTP_TIMEOUT_MS);
    if (err != ESP_OK) return false;

    if (res.status >= 300 && res.status < 400) {
        // Entra IDのログインへ飛ばされる場合はここに来る。
        // デバイス側では対話的なOAuth2を通せないので、サーバ側の設定変更が要る。
        ESP_LOGE(TAG, "HTTP %d: 認証リダイレクトの可能性がある。Location=%s",
                 res.status, res.location.c_str());
        return false;
    }
    if (res.status != 200) {
        ESP_LOGE(TAG, "HTTP %d", res.status);
        return false;
    }

    uint32_t server_now = 0;
    if (!parseScheduleJson(res.body, store, &server_now)) return false;

    // 時刻の優先順位: JSON本文 > Dateヘッダ。RTCが無い基板なのでシステム時刻へ反映するだけ。
    uint32_t new_time = server_now != 0 ? server_now : res.date_utc;
    if (new_time != 0) {
        setSystemTime(new_time);
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────

extern "C" void app_main(void) {
    // time_util.hのUTC⇔JST変換はlibcのTZがUTCである前提。明示的に固定する。
    setenv("TZ", "UTC0", 1);
    tzset();

    // IOエキスパンダ(バックライト/リセット)。無くても表示自体は続けられるので
    // 失敗してもログだけ出して続行する(バックライトが点かないだけになる)。
    if (ioExtBegin() != ESP_OK) {
        ESP_LOGE(TAG, "[IO] IOエキスパンダの初期化に失敗した(続行する)");
    }

    // LCDパネルは表示の前提そのものなので、失敗したら止める。
    // オンチップデバッグが無い基板なので、ここで停止してログだけを頼りに切り分ける。
    if (lcdPanelBegin() != ESP_OK) {
        ESP_LOGE(TAG, "[LCD] LCDパネルの初期化に失敗したため停止する");
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    serialLinkBegin();

    // app_main()のスタック上に置かず、関数ローカルstaticにしている
    // (巨大なスプライトや予定配列をこの後も生かし続けるため)。
    static ScheduleStore store;
    static Protocol      proto(&store);
    static Display       display;

    // フラッシュのfontパーティションが用意できないときは続行しない。
    // 内蔵フォントは輪郭が粗く予定を読み取れないため、動いているように
    // 見せるほうが害になる。
    if (!display.begin()) {
        ESP_LOGE(TAG, "[FONT] フォントを用意できないため停止する");
        display.showFatalMessage("フォント読込失敗\nfontパーティションに\nTTFを書き込んでください");
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    display.showBootMessage("Wi-Fi接続中...");

    // 表示ができてから点灯する(初期化中の乱れた画面を見せないため)。
    if (ioExtSetBacklight(BACKLIGHT_PERCENT) != ESP_OK) {
        ESP_LOGW(TAG, "[IO] バックライトの点灯に失敗した");
    }

    // secrets.hでWIFI_STATIC_IPを定義したときだけ固定IPで接続する。
    // 未定義ならDHCPのまま(nullptrを渡す)。
#ifdef WIFI_STATIC_IP
    const WifiStaticIp static_ip = {
        WIFI_STATIC_IP,
        WIFI_STATIC_GATEWAY,
        WIFI_STATIC_NETMASK,
        WIFI_STATIC_DNS1,
        WIFI_STATIC_DNS2,
    };
    const WifiStaticIp* static_ip_ptr = &static_ip;
#else
    const WifiStaticIp* static_ip_ptr = nullptr;
#endif

    esp_err_t werr = wifiLinkBegin(WIFI_SSID, WIFI_PASSWORD, WIFI_TIMEOUT_MS, static_ip_ptr);
    if (werr != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi接続に失敗した: %s", esp_err_to_name(werr));
        display.showBootMessage("Wi-Fi接続失敗\nシリアル待機中");
    } else {
        char ip[16];
        wifiLinkGetIp(ip, sizeof(ip));
        ESP_LOGI(TAG, "Wi-Fi接続完了 IP=%s", ip);
        display.showBootMessage("スケジュール取得中...");
    }

    // 初回は接続の成否にかかわらず1度試す(失敗ならシリアル待機に落ちる)。
    bool     rendered       = false;
    uint32_t next_fetch_utc = 0;
    int      last_minute    = -1;

    // サーバ経路が使えないときの保険として、PC(scheduler_sender.py)からの
    // シリアル受信も残してある。到達性が確認できたら撤去してよい。
    serialLinkWriteLine("REQ:ALL");

    std::string line;
    for (;;) {
        if (nowUtc() >= next_fetch_utc) {
            if (fetchSchedule(&store)) {
                // 取得の中でサーバ時刻に合わせ直すので、境界の計算はその後に行う。
                next_fetch_utc = nextAlignedUtc(nowUtc(), POLL_INTERVAL_SEC);
                display.renderTimeline(store, nowUtc());
                rendered = true;
            } else {
                // 失敗したときにPOLL_INTERVAL_SEC待つと、起動直後の
                // 一時的なAP不在で画面が長時間止まる。短い間隔で作り直す。
                // ここは境界に合わせない(合わせると次の境界まで待つことになる)。
                next_fetch_utc = nowUtc() + RETRY_INTERVAL_SEC;
                if (!rendered) display.showBootMessage("取得失敗\n再試行中");
            }
        }

        if (rendered) {
            uint32_t now = nowUtc();
            int cur_minute = (int)(now / 60);
            if (cur_minute != last_minute) {
                display.renderTimeline(store, now);
                last_minute = cur_minute;
            }
            display.renderClock(now);
            display.tickBlink(now, (uint32_t)(esp_timer_get_time() / 1000));
        }

        // LOOP_TICK_MS待って行が来なければfalse。ここがループの唯一の待ち。
        if (!serialLinkReadLine(line, LOOP_TICK_MS)) continue;
        if (line.empty()) continue;

        proto.processLine(line);

        if (proto.isComplete()) {
            proto.resetComplete();

            uint32_t recv_time = proto.getReceivedTime();
            if (recv_time > 0) {
                setSystemTime(recv_time);
            }

            display.renderTimeline(store, nowUtc());
            rendered = true;
        }
    }
}
