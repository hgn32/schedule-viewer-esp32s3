#include <stdlib.h>   // setenv (POSIX)
#include <cstring>    // strcmp

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_timer.h"

#include "display.h"
#include "http_client.h"
#include "io_ext.h"
#include "json_parser.h"
#include "lcd_panel.h"
#include "schedule.h"
#include "secrets.h"
#include "time_sync.h"
#include "time_util.h"
#include "touch.h"
#include "wifi_link.h"

// Wi-Fi接続を待つ上限。接続確立後、切断からの再接続を待つ側で使う。
// 社内APは認証に時間がかかることがあるので長めに取る。
static const uint32_t WIFI_TIMEOUT_MS = 20000;
// 起動時に候補を1つ試すときの上限。候補が複数あるので短めにして次候補へ早く移る。
static const uint32_t WIFI_CANDIDATE_TIMEOUT_MS = 15000;
// HTTPS 1回あたりの上限。TLSハンドシェイクを含む。
static const uint32_t HTTP_TIMEOUT_MS = 15000;
// 取得に失敗したときの再試行間隔。POLL_INTERVAL_SEC(既定300)より短くする。
static const uint32_t RETRY_INTERVAL_SEC = 60;
// メインループの1ティック(タッチ・時計・再描画をポーリングする周期)。
static const uint32_t LOOP_TICK_MS = 100;
// Wi-Fi候補が全滅したときの再スキャンまでの待ち時間。
static const uint32_t WIFI_RESCAN_WAIT_MS = 5000;
// SNTP同期待ちのポーリング間隔。
static const uint32_t TIME_SYNC_POLL_MS = 500;
// 起動後、表示ができてから点灯するバックライトの輝度(%)。
static const uint8_t BACKLIGHT_PERCENT = 80;
// タイムライン全体の再描画は重いので、分の変わり目(秒=0)を避けてこの秒へずらす。
// 00秒には時計の部分更新だけを行い、時計が止まって見えないようにする。
static const int TIMELINE_REDRAW_SEC = 5;

// ─────────────────────────────────────────────────────────────────────────────

// Wi-Fi未接続時のブートメッセージを組み立てる。PCを繋がずに原因を切り分けられるよう、
// 試したSSID・スキャンでの検出有無・直近の切断理由・自分のMACを画面へ出す。
// MACを出すのは、APが端末のMAC登録を要求するネットワークで、画面を見るだけで
// 登録申請に必要な値が分かるようにするため。
// 1行目だけが大きく描かれ、2行目以降は小さく描かれる(Display::showBootMessage())。
static void buildWifiStatusMessage(const char* head, char* buf, size_t len) {
    if (head == nullptr || buf == nullptr || len == 0) return;

    WifiAttemptInfo info = {};
    wifiLinkGetLastAttempt(&info);

    char mac[24] = {};
    wifiLinkGetMac(mac, sizeof(mac));

    if (info.ssid[0] == '\0') {
        // まだ1度も試していない(起動直後の最初のスキャン中など)。
        snprintf(buf, len, "%s\nMAC %s", head, mac);
        return;
    }

    if (info.reason == 0) {
        // 切断イベントがまだ無い=接続応答そのものが無い状態。
        snprintf(buf, len, "%s\n%s\n%s AP%u件\n理由:なし\nMAC %s", head, info.ssid,
                 info.scanned ? "スキャン:検出" : "スキャン:未検出", (unsigned)info.ap_count,
                 mac);
        return;
    }

    snprintf(buf, len, "%s\n%s\n%s AP%u件\n理由%u\n%s\nMAC %s", head, info.ssid,
             info.scanned ? "スキャン:検出" : "スキャン:未検出", (unsigned)info.ap_count,
             (unsigned)info.reason, wifiLinkDescribeReason(info.reason), mac);
}

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
        return false;
    }

    HttpResponse res;
    esp_err_t err = httpGetJson(SCHEDULE_URL, &res, HTTP_TIMEOUT_MS);
    if (err != ESP_OK) return false;

    if (res.status >= 300 && res.status < 400) {
        // Entra IDのログインへ飛ばされる場合はここに来る。
        // デバイス側では対話的なOAuth2を通せないので、サーバ側の設定変更が要る。
        return false;
    }
    if (res.status != 200) {
        return false;
    }

    return parseScheduleJson(res.body, store);
}

// ─────────────────────────────────────────────────────────────────────────────

extern "C" void app_main(void) {
    // time_util.hのUTC⇔JST変換はlibcのTZがUTCである前提。明示的に固定する。
    setenv("TZ", "UTC0", 1);
    tzset();

    // IOエキスパンダ(バックライト/リセット)。無くても表示自体は続けられるので、
    // 失敗しても続行する(バックライトが点かないだけになる)。
    (void)ioExtBegin();

    // タッチ(GT911)。無くても表示自体は続けられるので、失敗しても続行する
    // (画面のON/OFF操作ができなくなるだけ)。IOエキスパンダと同じI2Cバスを
    // 共有するため、ioExtBegin()より後に呼ぶ必要がある。
    (void)touchBegin();

    // LCDパネルは表示の前提そのものなので、失敗したら止める。
    if (lcdPanelBegin() != ESP_OK) {
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    // app_main()のスタック上に置かず、関数ローカルstaticにしている
    // (巨大なスプライトや予定配列をこの後も生かし続けるため)。
    static ScheduleStore store;
    static Display       display;

    // フラッシュのfontパーティションが用意できないときは続行しない。
    // 内蔵フォントは輪郭が粗く予定を読み取れないため、動いているように
    // 見せるほうが害になる。
    if (!display.begin()) {
        display.showFatalMessage("フォント読込失敗\nfontパーティションに\nTTFを書き込んでください");
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    display.showBootMessage("Wi-Fi接続中...");

    // 表示ができてから点灯する(初期化中の乱れた画面を見せないため)。
    // 公式サンプルの初期化順序(bl_on()の後にPWM輝度を設定)と同じく、
    // イネーブルと輝度設定を分けて呼ぶ。
    (void)ioExtBacklightEnable(true);
    (void)ioExtSetBacklightLevel(BACKLIGHT_PERCENT);

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
        // ここは初期化そのものの失敗。再試行しても直らないので停止する。
        display.showFatalMessage("Wi-Fi初期化失敗");
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    // 繋がるまで諦めない。RTCが無く時刻はサーバからしか得られないため、
    // Wi-Fiが繋がらない限り表示できるものが無い。
    // showBootMessage()は全画面を描き直すので、前回と同じ文字列なら描き直さない。
    static char boot_wifi_msg[192]     = {};
    static char boot_wifi_msg_prev[192] = {};
    while (!wifiLinkConnectRound()) {
        buildWifiStatusMessage("Wi-Fi接続中...", boot_wifi_msg, sizeof(boot_wifi_msg));
        if (strcmp(boot_wifi_msg, boot_wifi_msg_prev) != 0) {
            display.showBootMessage(boot_wifi_msg);
            snprintf(boot_wifi_msg_prev, sizeof(boot_wifi_msg_prev), "%s", boot_wifi_msg);
        }
        vTaskDelay(pdMS_TO_TICKS(WIFI_RESCAN_WAIT_MS));
    }

    // 時刻源はSNTPのみ(JSON本文/Dateヘッダは見ない)。RTCが無いため、
    // 同期が済むまではタイムラインを描けるものが無い。
    if (timeSyncBegin(NTP_SERVER) != ESP_OK) {
        // 初期化そのものの失敗は再試行しても直らないので停止する。
        display.showFatalMessage("時刻同期の初期化失敗");
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    char time_sync_msg[128];
    snprintf(time_sync_msg, sizeof(time_sync_msg), "時刻同期中...\nNTP %s", NTP_SERVER);
    display.showBootMessage(time_sync_msg);
    while (!timeSyncIsSynced()) {
        vTaskDelay(pdMS_TO_TICKS(TIME_SYNC_POLL_MS));
    }

    display.showBootMessage("スケジュール取得中...");

    bool     rendered       = false;
    uint32_t next_fetch_utc = 0;
    int      last_minute    = -1;
    // 取得でタイムラインの再描画が必要になったことを示すフラグ。
    // 00秒に重い全画面再描画が集中しないよう、実際の再描画はTIMELINE_REDRAW_SECまで遅らせる。
    bool     timeline_dirty = false;
    // 画面ON/OFFの現在状態。バックライトの実際の状態と一致させる。
    bool     screen_on      = true;
    // Off中にPress(タップ)で画面を点けた場合、その同じタッチを離す前に
    // 1500ms経過してLongTapと判定され、そのまま即座に消灯してしまう
    // 誤動作を防ぐためのフラグ。新しいタッチが始まるたび(Press発生時)に
    // falseへ戻すので、次のタッチでは通常どおりLongTapが効く。
    bool     suppress_longtap = false;

    for (;;) {
        // タッチによる画面ON/OFF。取得・描画より前に処理してよい(軽い処理のため)。
        TouchEvent touch_ev = touchPoll();
        if (touch_ev == TouchEvent::Press) {
            // 新しいタッチの開始。前のタッチの抑制状態を引きずらない。
            suppress_longtap = false;
            if (!screen_on) {
                // 公式のbl_on()と同一にするため、ここではPWM(0x05)を書かない。
                // 消灯時にもPWMレジスタには触れていないので、起動時に設定した
                // 輝度がそのまま残っている想定。
                if (ioExtBacklightEnable(true) == ESP_OK) {
                    screen_on = true;
                }
                // このタッチ自身の長押しが、点灯直後にそのままロングタップと
                // 判定されて即座に消灯してしまわないよう抑制する。
                suppress_longtap = true;
            }
        } else if (touch_ev == TouchEvent::LongTap) {
            if (screen_on && !suppress_longtap) {
                // 消灯もDISP(IO2)のイネーブルだけを落とす。公式のbl_off()と同じ操作で、
                // PWM(0x05)には触らない。
                if (ioExtBacklightEnable(false) == ESP_OK) {
                    screen_on = false;
                }
            }
        }

        if (nowUtc() >= next_fetch_utc) {
            if (fetchSchedule(&store)) {
                // 取得の中でサーバ時刻に合わせ直すので、境界の計算はその後に行う。
                next_fetch_utc = nextAlignedUtc(nowUtc(), POLL_INTERVAL_SEC);
                if (!rendered) {
                    // 初回だけ即描く(起動直後に数秒待たせないため)。
                    display.renderTimeline(store, nowUtc());
                    last_minute = (int)(nowUtc() / 60);
                    rendered    = true;
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
                if (!rendered) {
                    // 原因が分かる表示にする。Wi-Fiが切れているのに「取得失敗」とだけ
                    // 出すと切り分けできない。
                    if (wifiLinkIsConnected()) {
                        display.showBootMessage("取得失敗\n再試行中");
                    } else {
                        char reconnect_msg[192];
                        buildWifiStatusMessage("Wi-Fi再接続中...", reconnect_msg,
                                                sizeof(reconnect_msg));
                        display.showBootMessage(reconnect_msg);
                    }
                }
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
        }

        // ループの唯一の待ち。これが無いとビジーループになりウォッチドッグが落ちる。
        vTaskDelay(pdMS_TO_TICKS(LOOP_TICK_MS));
    }
}
