#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/time.h>

// システムのTZはapp_main()で"UTC0"に固定している。
// mktime / gmtime_rはどちらもUTCとして働く前提でこのファイルを書いている。
static constexpr int JST_OFFSET = 32400; // UTC+9 in seconds

static const char* const WEEKDAYS_JA[] = {"日","月","火","水","木","金","土"};

// Set ESP32 system clock from UTC epoch
inline void setSystemTime(uint32_t utc_epoch) {
    struct timeval tv = {(time_t)utc_epoch, 0};
    settimeofday(&tv, nullptr);
}

// Current UTC epoch from system clock
inline uint32_t nowUtc() {
    return (uint32_t)time(nullptr);
}

// Current JST time as struct tm
inline struct tm nowJst() {
    time_t jst_t = (time_t)(nowUtc() + JST_OFFSET);
    struct tm t;
    gmtime_r(&jst_t, &t);
    return t;
}

// UTC epoch of the current hour boundary (floor to JST hour, return as UTC)
inline uint32_t floorHourUtc() {
    uint32_t jst_now = nowUtc() + JST_OFFSET;
    uint32_t jst_floored = (jst_now / 3600) * 3600;
    return jst_floored - JST_OFFSET;
}

// "9月12日(金)"(月日はゼロ埋めしない)
inline std::string formatDate(const struct tm& t) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d月%d日(%s)",
             t.tm_mon + 1, t.tm_mday,
             WEEKDAYS_JA[t.tm_wday]);
    return std::string(buf);
}

// "14:00"
inline std::string formatTime(const struct tm& t) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
    return std::string(buf);
}

// "14" (hour only, zero-padded)
inline std::string formatHour(int hour) {
    char buf[4];
    snprintf(buf, sizeof(buf), "%02d", hour % 24);
    return std::string(buf);
}

// ─────────────────────────────────────────────────────────────────────────────
// サーバのJSON / HTTPヘッダから受け取る時刻表現のパース。
// TZ=UTC0前提なのでmktimeがそのままUTC epochを返す(このファイル冒頭の注記を参照)。

// 分解された年月日時分秒(UTC)をepochへ。年が1970未満なら0を返す。
inline uint32_t epochFromUtcParts(int year, int mon, int mday,
                                  int hour, int min, int sec) {
    if (year < 1970) return 0;
    struct tm t = {};
    t.tm_year   = year - 1900;
    t.tm_mon    = mon - 1;
    t.tm_mday   = mday;
    t.tm_hour   = hour;
    t.tm_min    = min;
    t.tm_sec    = sec;
    t.tm_isdst  = 0;
    time_t e    = mktime(&t);
    return e < 0 ? 0 : (uint32_t)e;
}

// ISO8601 / RFC3339をUTC epochへ。受け付ける形は次のとおり。
//   2026-08-31T09:00:00Z
//   2026-08-31T09:00:00+09:00   (+0900 / -09:00 も可)
//   2026-08-31T09:00:00.1234567 (小数秒は読み飛ばす)
//   2026-08-31 09:00:00         (区切りの空白も可)
// オフセットが書かれていない場合はfallback_offset_secを引いてUTCにする
// (Microsoft Graphはオフセット無しでUTCを返すので既定は0)。
// 解析できなければfalseを返し、*outは触らない。
inline bool parseIso8601(const char* s, uint32_t* out, int fallback_offset_sec = 0) {
    if (s == nullptr || out == nullptr) return false;

    int  year = 0, mon = 0, mday = 0, hour = 0, min = 0, sec = 0;
    int  consumed = 0;
    char sep      = 0;
    if (sscanf(s, "%4d-%2d-%2d%c%2d:%2d:%2d%n",
               &year, &mon, &mday, &sep, &hour, &min, &sec, &consumed) != 7) {
        return false;
    }
    if (sep != 'T' && sep != 't' && sep != ' ') return false;
    if (mon < 1 || mon > 12 || mday < 1 || mday > 31) return false;
    if (hour > 23 || min > 59 || sec > 60) return false;

    const char* p = s + consumed;

    // 小数秒(".1234567")は精度を使わないので読み飛ばす。
    if (*p == '.' || *p == ',') {
        p++;
        while (*p >= '0' && *p <= '9') p++;
    }

    int offset_sec = fallback_offset_sec;
    if (*p == 'Z' || *p == 'z') {
        offset_sec = 0;
    } else if (*p == '+' || *p == '-') {
        int sign = (*p == '-') ? -1 : 1;
        p++;
        int oh = 0, om = 0;
        // "+09:00"と"+0900"の両方を受ける。
        if (sscanf(p, "%2d:%2d", &oh, &om) == 2) {
            // 何もしない(値は取れている)
        } else if (sscanf(p, "%2d%2d", &oh, &om) == 2) {
            // 何もしない
        } else if (sscanf(p, "%2d", &oh) == 1) {
            om = 0;
        } else {
            return false;
        }
        offset_sec = sign * (oh * 3600 + om * 60);
    }

    uint32_t epoch_local = epochFromUtcParts(year, mon, mday, hour, min, sec);
    if (epoch_local == 0) return false;

    int64_t utc = (int64_t)epoch_local - offset_sec;
    if (utc < 0) return false;
    *out = (uint32_t)utc;
    return true;
}

// RFC7231のIMF-fixdate("Sun, 06 Nov 1994 08:49:37 GMT")をUTC epochへ。
// HTTPのDateヘッダはこの形しか返さない決まりなので、旧2形式は扱わない。
inline bool parseHttpDate(const char* s, uint32_t* out) {
    if (s == nullptr || out == nullptr) return false;

    // "Sun, " の5文字を飛ばす。曜日名は使わない。
    const char* p = strchr(s, ',');
    p             = (p != nullptr) ? p + 1 : s;
    while (*p == ' ') p++;

    int  mday = 0, year = 0, hour = 0, min = 0, sec = 0;
    char mon_name[4] = {};
    if (sscanf(p, "%2d %3s %4d %2d:%2d:%2d", &mday, mon_name, &year,
               &hour, &min, &sec) != 6) {
        return false;
    }

    static const char* const MONTHS[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                         "Jul","Aug","Sep","Oct","Nov","Dec"};
    int mon = 0;
    for (int i = 0; i < 12; i++) {
        if (strncmp(mon_name, MONTHS[i], 3) == 0) {
            mon = i + 1;
            break;
        }
    }
    if (mon == 0) return false;

    uint32_t epoch = epochFromUtcParts(year, mon, mday, hour, min, sec);
    if (epoch == 0) return false;
    *out = epoch;
    return true;
}
