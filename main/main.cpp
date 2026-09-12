#include <stdlib.h>   // setenv (POSIX)

#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "display.h"
#include "dummy_schedule.h"
#include "http_client.h"
#include "io_ext.h"
#include "json_parser.h"
#include "lcd_panel.h"
#include "protocol.h"
#include "schedule.h"
#include "secrets.h"
#include "serial_link.h"
#include "sntp_time.h"
#include "time_util.h"
#include "wifi_link.h"

static const char* TAG = "main";

// secrets.hでUSE_DUMMY_SCHEDULEを1にすると、HTTP取得を行わずダミー予定を表示する。
// サーバへ到達できない環境での暫定手段。到達性が確認できたら撤去する。
#if !defined(USE_DUMMY_SCHEDULE)
#define USE_DUMMY_SCHEDULE 0
#endif

// Wi-Fi接続を待つ上限。接続確立後、切断からの再接続を待つ側で使う。
// 社内APは認証に時間がかかることがあるので長めに取る。
static const uint32_t WIFI_TIMEOUT_MS = 20000;
// 起動時に候補を1つ試すときの上限。候補が複数あるので短めにして次候補へ早く移る。
static const uint32_t WIFI_CANDIDATE_TIMEOUT_MS = 15000;
// SNTPの同期待ち。10秒では実機でタイムアウトすることがあったため長めに取る
// (名前解決とNTPの往復を含む。失敗しても表示は続けるので待つ側に倒す)。
static const uint32_t SNTP_TIMEOUT_MS = 25000;
// HTTPS 1回あたりの上限。TLSハンドシェイクを含む。
static const uint32_t HTTP_TIMEOUT_MS = 15000;
// 取得に失敗したときの再試行間隔。POLL_INTERVAL_SEC(既定300)より短くする。
static const uint32_t RETRY_INTERVAL_SEC = 60;
// メインループの1ティック(シリアル受信待ちのタイムアウト)。
static const uint32_t LOOP_TICK_MS = 100;
// 起動後、表示ができてから点灯するバックライトの輝度(%)。
static const uint8_t BACKLIGHT_PERCENT = 80;
// タイムライン全体の再描画は重いので、分の変わり目(秒=0)を避けてこの秒へずらす。
// 00秒には時計の部分更新だけを行い、時計が止まって見えないようにする。
static const int TIMELINE_REDRAW_SEC = 5;

#if USE_DUMMY_SCHEDULE
// ダミーモードのときだけ使う。最初のタイムライン描画からこの時間後に
// スクリーンショットをログへ出し、以後もこの間隔で繰り返す(一時的なデバッグ機能。
// サーバ到達性が確認できたら撤去する)。繰り返すのは、監視を繋いでいない間に
// 起きた再描画の計測値を画面のオーバーレイ経由で読み取れるようにするため。
static const uint32_t SCREENSHOT_INTERVAL_MS = 20000;
#endif

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

#if USE_DUMMY_SCHEDULE
    // サーバへ到達できない環境向けの暫定経路。HTTPは一切呼ばない。
    // 時刻はSNTPが入れたシステム時刻をそのまま使うので、ここではsetSystemTime()を呼ばない。
    ESP_LOGW(TAG, "ダミーモード: サーバへは接続していない");
    return parseScheduleJson(dummyScheduleJson(nowUtc()), store, nullptr);
#else
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
#endif
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

    // 一時的な調査用。タッチICのI2Cアドレスを確定させたら、この呼び出しと
    // ioExtScanBus()そのものを撤去する。
    ioExtScanBus();

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

#if USE_DUMMY_SCHEDULE
    display.setPerfOverlay(true);
#endif

    display.showBootMessage("Wi-Fi接続中...");

    // 表示ができてから点灯する(初期化中の乱れた画面を見せないため)。
    if (ioExtSetBacklight(BACKLIGHT_PERCENT) != ESP_OK) {
        ESP_LOGW(TAG, "[IO] バックライトの点灯に失敗した");
    }

    // secrets.hでWIFI_STATIC_IP(候補2・候補3は_2/_3付き)を定義したときだけ、
    // その候補は固定IPで接続する。未定義ならDHCPのまま(nullptrを渡す)。
#ifdef WIFI_STATIC_IP
    const WifiStaticIp static_ip_1 = {
        WIFI_STATIC_IP,
        WIFI_STATIC_GATEWAY,
        WIFI_STATIC_NETMASK,
        WIFI_STATIC_DNS1,
        WIFI_STATIC_DNS2,
    };
    const WifiStaticIp* static_ip_1_ptr = &static_ip_1;
#else
    const WifiStaticIp* static_ip_1_ptr = nullptr;
#endif
    // 候補2・候補3のIP設定は、そのSSID自体が定義されているときだけ意味を持つ。
    // SSID未定義のときにポインタ変数だけ作ると未使用変数の警告になるため、
    // WIFI_SSID_2/3のifdefの中で完結させる。
#ifdef WIFI_SSID_2
#ifdef WIFI_STATIC_IP_2
    const WifiStaticIp static_ip_2 = {
        WIFI_STATIC_IP_2,
        WIFI_STATIC_GATEWAY_2,
        WIFI_STATIC_NETMASK_2,
        WIFI_STATIC_DNS1_2,
        WIFI_STATIC_DNS2_2,
    };
    const WifiStaticIp* static_ip_2_ptr = &static_ip_2;
#else
    const WifiStaticIp* static_ip_2_ptr = nullptr;
#endif
#endif
#ifdef WIFI_SSID_3
#ifdef WIFI_STATIC_IP_3
    const WifiStaticIp static_ip_3 = {
        WIFI_STATIC_IP_3,
        WIFI_STATIC_GATEWAY_3,
        WIFI_STATIC_NETMASK_3,
        WIFI_STATIC_DNS1_3,
        WIFI_STATIC_DNS2_3,
    };
    const WifiStaticIp* static_ip_3_ptr = &static_ip_3;
#else
    const WifiStaticIp* static_ip_3_ptr = nullptr;
#endif
#endif

    WifiCandidate candidates[3];
    size_t        candidate_count = 0;
    candidates[candidate_count++] = {WIFI_SSID, WIFI_PASSWORD, static_ip_1_ptr};
#ifdef WIFI_SSID_2
    candidates[candidate_count++] = {WIFI_SSID_2, WIFI_PASSWORD_2, static_ip_2_ptr};
#endif
#ifdef WIFI_SSID_3
    candidates[candidate_count++] = {WIFI_SSID_3, WIFI_PASSWORD_3, static_ip_3_ptr};
#endif

    esp_err_t werr = wifiLinkBegin(candidates, candidate_count, WIFI_CANDIDATE_TIMEOUT_MS);
    if (werr != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi接続に失敗した: %s", esp_err_to_name(werr));
        display.showBootMessage("Wi-Fi接続失敗\nシリアル待機中");
    } else {
        char ip[16];
        char ssid[33];
        wifiLinkGetIp(ip, sizeof(ip));
        wifiLinkGetSsid(ssid, sizeof(ssid));
        ESP_LOGI(TAG, "Wi-Fi接続完了 SSID=%s IP=%s", ssid, ip);

#if USE_DUMMY_SCHEDULE
        // ダミーモードにはRTCどころかサーバ時刻すら無いので、Wi-Fi接続直後にSNTPで
        // 実時刻を取る。失敗しても表示自体は続ける(時刻がずれるだけ)。
        esp_err_t serr = sntpSyncTime(SNTP_TIMEOUT_MS);
        if (serr != ESP_OK) {
            ESP_LOGW(TAG, "SNTP同期に失敗した(続行する): %s", esp_err_to_name(serr));
        }
        display.showBootMessage("ダミーデータ表示中");
#else
        display.showBootMessage("スケジュール取得中...");
#endif
    }

    // 初回は接続の成否にかかわらず1度試す(失敗ならシリアル待機に落ちる)。
    bool     rendered       = false;
    uint32_t next_fetch_utc = 0;
    int      last_minute    = -1;
    // 取得やシリアル受信でタイムラインの再描画が必要になったことを示すフラグ。
    // 00秒に重い全画面再描画が集中しないよう、実際の再描画はTIMELINE_REDRAW_SECまで遅らせる。
    bool     timeline_dirty = false;
#if USE_DUMMY_SCHEDULE
    // 最初のタイムライン描画時刻(ms、esp_timer基準)とスクリーンショット出力済みか。
    uint32_t rendered_at_ms    = 0;
#endif

    // サーバ経路が使えないときの保険として、PC(scheduler_sender.py)からの
    // シリアル受信も残してある。到達性が確認できたら撤去してよい。
    serialLinkWriteLine("REQ:ALL");

    std::string line;
    for (;;) {
        if (nowUtc() >= next_fetch_utc) {
            if (fetchSchedule(&store)) {
                // 取得の中でサーバ時刻に合わせ直すので、境界の計算はその後に行う。
                next_fetch_utc = nextAlignedUtc(nowUtc(), POLL_INTERVAL_SEC);
                if (!rendered) {
                    // 初回だけ即描く(起動直後に数秒待たせないため)。
                    display.renderTimeline(store, nowUtc());
                    last_minute = (int)(nowUtc() / 60);
                    rendered    = true;
#if USE_DUMMY_SCHEDULE
                    rendered_at_ms = (uint32_t)(esp_timer_get_time() / 1000);
#endif
                } else {
                    // 2回目以降はTIMELINE_REDRAW_SECまで遅らせる。取得はX:00境界に
                    // 揃うため、ここで即描くと結局00秒に重い処理が重なってしまう。
                    timeline_dirty = true;
                }
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

            // 1. 時計を最優先で更新する。ここは部分更新なので軽い。
            display.renderClock(now);

            // 2. タイムライン全体の再描画。00秒を避けてTIMELINE_REDRAW_SECへずらす。
            int cur_minute = (int)(now / 60);
            int cur_sec    = (int)(now % 60);
            if (cur_sec >= TIMELINE_REDRAW_SEC && (cur_minute != last_minute || timeline_dirty)) {
                display.renderTimeline(store, now);
                last_minute    = cur_minute;
                timeline_dirty = false;
            }

            // 3. 明滅。開始はBLINK_START_DELAY_MSだけ遅れるので00秒には重ならない。
            display.tickBlink(now, (uint32_t)(esp_timer_get_time() / 1000));

#if USE_DUMMY_SCHEDULE
            // 最初のタイムライン描画からSCREENSHOT_INTERVAL_MSごとに繰り返し出す
            // (一時的なデバッグ機能)。
            {
                uint32_t elapsed_ms = (uint32_t)(esp_timer_get_time() / 1000) - rendered_at_ms;
                if (elapsed_ms >= SCREENSHOT_INTERVAL_MS) {
                    ESP_LOGI(TAG, "スクリーンショットを出力する");
                    display.dumpScreenshot();
                    rendered_at_ms = (uint32_t)(esp_timer_get_time() / 1000);
                }
            }
#endif
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

            if (!rendered) {
                // 起動メッセージからの初回描画だけは即時に行う(起動直後に
                // 数秒待たせないため)。
                display.renderTimeline(store, nowUtc());
                last_minute = (int)(nowUtc() / 60);
                rendered    = true;
#if USE_DUMMY_SCHEDULE
                rendered_at_ms = (uint32_t)(esp_timer_get_time() / 1000);
#endif
            } else {
                // 2回目以降はTIMELINE_REDRAW_SECまで遅らせ、00秒への集中を避ける。
                timeline_dirty = true;
            }
        }
    }
}
