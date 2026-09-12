#include "dummy_schedule.h"

#include <cstdio>
#include <ctime>

#include "time_util.h"

// このファイルはM5にもIDFにも依存しない(time_util.hだけ使う)。
// ログを出す層ではないのでTAGは定義していない
// (呼び出し側のmain.cppが「ダミーモード」のログを出す)。

namespace {

// UTC epochを"YYYY-MM-DDTHH:MM:SSZ"へ(json_parser.cppのparseIso8601がそのまま読める形)。
std::string isoUtc(uint32_t epoch) {
    time_t t = (time_t)epoch;
    struct tm tmv = {};
    gmtime_r(&t, &tmv);
    char buf[48];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return std::string(buf);
}

// 予定1件分のJSONオブジェクトを組み立てる。
// json_parser.cppの候補表の先頭(subject/start/end/isAllDay/isCancelled/location)に合わせる。
// show_asがnullptrなら"showAs"フィールドを出さない(通常の予定は付けない)。
std::string eventJson(const char* title, const char* location,
                      uint32_t start_utc, uint32_t end_utc,
                      bool is_cancelled, bool is_all_day,
                      const char* show_as = nullptr) {
    char show_as_field[64] = "";
    if (show_as != nullptr) {
        snprintf(show_as_field, sizeof(show_as_field), ",\"showAs\":\"%s\"", show_as);
    }

    char buf[640];
    snprintf(buf, sizeof(buf),
             "{\"subject\":\"%s\",\"start\":\"%s\",\"end\":\"%s\","
             "\"isAllDay\":%s,\"isCancelled\":%s,\"location\":\"%s\"%s}",
             title,
             isoUtc(start_utc).c_str(),
             isoUtc(end_utc).c_str(),
             is_all_day ? "true" : "false",
             is_cancelled ? "true" : "false",
             location,
             show_as_field);
    return std::string(buf);
}

} // namespace

std::string dummyScheduleJson(uint32_t now_utc) {
    std::string json = "{\"events\":[";

    // 1. 今より前に始まって今も続いている予定(進行中)。
    // 全角スペース・全角英字・半角濁点カナを混ぜてtext_util.cppの正規化経路を踏ませる。
    json += eventJson("[ダミー]進行中会議　Ａ棟", "本社ﾋﾞﾙ3F会議室",
                       now_utc - 1800, now_utc + 1800, false, false);
    json += ",";

    // 2. L3(赤、開始2分前以下)の確認用。開始まで110秒。
    json += eventJson("[ダミー]まもなく開始(2分前)", "会議室２",
                       now_utc + 110, now_utc + 2600, false, false);
    json += ",";

    // 3. L2(橙、開始5分前以下)の確認用。開始まで280秒。
    json += eventJson("[ダミー]5分前の予定", "会議室３",
                       now_utc + 280, now_utc + 3000, false, false);
    json += ",";

    // 4. L1(黄、開始10分前以下)の確認用。開始まで580秒。
    // 2〜4は開始時刻が重なるので、layoutEvents()の列分割も同時に確認できる
    // (意図した重なりであり、避けない)。
    json += eventJson("[ダミー]10分前の予定", "会議室４",
                       now_utc + 580, now_utc + 3400, false, false);
    json += ",";

    // 5. 遠い時間帯の予定(12時間タイムラインの見た目確認用、6〜7時間後)。
    json += eventJson("[ダミー]夕会Ｃチーム", "オンライン",
                       now_utc + 21600, now_utc + 25200, false, false);
    json += ",";

    // 6. さらに遠い予定(場所が長い文字列、10〜11時間後)。
    json += eventJson("[ダミー]プロジェクト報告会",
                       "本社ビル３階　大会議室（南側）ﾌﾛｱ奥のｽﾍﾟｰｽ",
                       now_utc + 36000, now_utc + 39600, false, false);
    json += ",";

    // 7. 中止済みの予定(除外経路の確認用)。
    json += eventJson("[ダミー]中止済みイベント", "",
                       now_utc + 7200, now_utc + 9000, true, false);
    json += ",";

    // 8. 終日の予定(除外経路の確認用)。00:00〜翌00:00のUTC日境界にしておく。
    const uint32_t day_start = (now_utc / 86400u) * 86400u;
    json += eventJson("[ダミー]終日イベント", "",
                       day_start, day_start + 86400u, false, true);
    json += ",";

    // 9. 仮の予定(showAs=tentative)。開始まで200秒なのでL2(橙)の強調段階に入るが、
    // 明滅はせず、破線枠(実線8px/隙間5px)だけで仮の予定であることを示す。
    json += eventJson("[ダミー]仮の予定(明滅なし)", "会議室５",
                       now_utc + 200, now_utc + 3000, false, false, "tentative");
    json += ",";

    // 10. 空きの予定(showAs=free、除外経路の確認用)。
    json += eventJson("[ダミー]空き予定", "",
                       now_utc + 7200, now_utc + 9000, false, false, "free");

    json += "]}";
    return json;
}
