#include "json_parser.h"

#include <cstring>
#include <vector>

#include "cJSON.h"
#include "esp_log.h"

#include "text_util.h"
#include "time_util.h"

static const char* TAG = "json_parser";

namespace {

// 実スキーマが確定するまでの候補表。確定したら1つに削ってよい(json_parser.hの注記)。
const char* const kEventsKeys[] = {"events", "value", "items", "data",
                                   "schedule", "schedules", "appointments"};
const char* const kTitleKeys[]  = {"subject", "title", "name", "summary"};
const char* const kStartKeys[]  = {"start", "startTime", "start_time",
                                   "startDateTime", "begin"};
const char* const kEndKeys[]    = {"end", "endTime", "end_time",
                                   "endDateTime", "finish"};
const char* const kLocKeys[]    = {"location", "place", "room", "locationName"};
const char* const kNowKeys[]    = {"now", "serverTime", "currentTime", "timestamp"};
// 表示対象から外す予定の判定に使う。
// 中止済みの予定と終日予定は12時間タイムラインに載せない
// (終日予定は00:00〜翌00:00の24時間枠になり、画面を丸ごと潰してしまう)。
const char* const kCancelKeys[] = {"isCancelled", "isCanceled", "cancelled", "canceled"};
const char* const kAllDayKeys[] = {"isAllDay", "allDay", "all_day", "isAllday"};
// 空き時間(showAs=free、BusyStatus 0)だけを表示対象から外す。
// 仮の予定(tentative、BusyStatus 1)は表示するが、明滅させず枠線を破線にして
// 確定の予定と区別する(display.cpp、Event::is_tentative)。
// Graphの"showAs"は free / tentative / busy / oof / workingElsewhere を返す。
const char* const kBusyKeys[]   = {"showAs", "busyStatus", "busy", "status",
                                   "freeBusyStatus"};

// オブジェクトから候補キーを順に探す。見つからなければnullptr。
const cJSON* findByKeys(const cJSON* obj, const char* const* keys, size_t n) {
    if (obj == nullptr || !cJSON_IsObject(obj)) return nullptr;
    for (size_t i = 0; i < n; i++) {
        const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, keys[i]);
        if (v != nullptr && !cJSON_IsNull(v)) return v;
    }
    return nullptr;
}

// タイムゾーン名からUTCオフセット(秒)を決める。
// Graphは"UTC"か"Tokyo Standard Time"を返す。判別できないものはUTC扱い。
int offsetFromTimeZone(const char* tz) {
    if (tz == nullptr) return 0;
    if (strstr(tz, "Tokyo") != nullptr) return JST_OFFSET;
    if (strstr(tz, "Asia/Tokyo") != nullptr) return JST_OFFSET;
    if (strcmp(tz, "JST") == 0) return JST_OFFSET;
    return 0;
}

// showAs / busyStatus等の値から予定の状態を判定する。
// キーが無い、または判別できない値のときはBusyState::Other(表示対象)。
enum class BusyState { Free, Tentative, Other };
BusyState busyState(const cJSON* obj, const char* const* keys, size_t n);

// 候補キーのいずれかがtrue相当ならtrue。キーが無ければfalse。
// JSONの真偽値のほか、"true"文字列と非0の数値も真として扱う。
bool isFlagSet(const cJSON* obj, const char* const* keys, size_t n) {
    const cJSON* v = findByKeys(obj, keys, n);
    if (v == nullptr) return false;
    if (cJSON_IsBool(v))   return cJSON_IsTrue(v);
    if (cJSON_IsNumber(v)) return v->valuedouble != 0;
    if (cJSON_IsString(v) && v->valuestring != nullptr) {
        return strcasecmp(v->valuestring, "true") == 0;
    }
    return false;
}

BusyState busyState(const cJSON* obj, const char* const* keys, size_t n) {
    const cJSON* v = findByKeys(obj, keys, n);
    if (v == nullptr) return BusyState::Other;

    if (cJSON_IsNumber(v)) {
        // OutlookのBusyStatus: 0=空き, 1=仮の予定, それ以外は表示対象。
        const int status = (int)v->valuedouble;
        const char* key = v->string ? v->string : "?";
        if (status == 0) {
            ESP_LOGI(TAG, "空きとして除外: %s=%d", key, status);
            return BusyState::Free;
        }
        if (status == 1) {
            ESP_LOGI(TAG, "仮の予定として取り込み(明滅なし): %s=%d", key, status);
            return BusyState::Tentative;
        }
        return BusyState::Other;
    }
    if (cJSON_IsString(v) && v->valuestring != nullptr) {
        const char* key = v->string ? v->string : "?";
        if (strcasecmp(v->valuestring, "free") == 0) {
            ESP_LOGI(TAG, "空きとして除外: %s=%s", key, v->valuestring);
            return BusyState::Free;
        }
        if (strcasecmp(v->valuestring, "tentative") == 0) {
            ESP_LOGI(TAG, "仮の予定として取り込み(明滅なし): %s=%s", key, v->valuestring);
            return BusyState::Tentative;
        }
        return BusyState::Other;
    }
    return BusyState::Other;
}

// 時刻を表すノードをUTC epochへ。文字列 / 数値 / {dateTime,timeZone}に対応する。
bool toEpoch(const cJSON* node, uint32_t* out) {
    if (node == nullptr || out == nullptr) return false;

    if (cJSON_IsNumber(node)) {
        double v = node->valuedouble;
        if (v <= 0) return false;
        // 1e11を超えていればミリ秒(2001年以降の秒はまだ1e10台)。
        if (v > 1e11) v /= 1000.0;
        *out = (uint32_t)v;
        return true;
    }

    if (cJSON_IsString(node) && node->valuestring != nullptr) {
        return parseIso8601(node->valuestring, out, 0);
    }

    if (cJSON_IsObject(node)) {
        static const char* const kDtKeys[] = {"dateTime", "date_time", "value", "datetime"};
        const cJSON* dt = findByKeys(node, kDtKeys, sizeof(kDtKeys) / sizeof(kDtKeys[0]));
        if (dt == nullptr) return false;

        static const char* const kTzKeys[] = {"timeZone", "time_zone", "tz"};
        const cJSON* tz = findByKeys(node, kTzKeys, sizeof(kTzKeys) / sizeof(kTzKeys[0]));
        int offset = (tz != nullptr && cJSON_IsString(tz)) ? offsetFromTimeZone(tz->valuestring) : 0;

        if (cJSON_IsNumber(dt)) return toEpoch(dt, out);
        if (!cJSON_IsString(dt) || dt->valuestring == nullptr) return false;
        return parseIso8601(dt->valuestring, out, offset);
    }

    return false;
}

// 文字列を表すノードを取り出す。{displayName:"..."}のような入れ子も剥がす。
std::string toText(const cJSON* node) {
    if (node == nullptr) return std::string();

    if (cJSON_IsString(node) && node->valuestring != nullptr) {
        return std::string(node->valuestring);
    }
    if (cJSON_IsObject(node)) {
        static const char* const kNameKeys[] = {"displayName", "display_name",
                                                "name", "value", "text"};
        const cJSON* v = findByKeys(node, kNameKeys, sizeof(kNameKeys) / sizeof(kNameKeys[0]));
        if (v != nullptr && cJSON_IsString(v) && v->valuestring != nullptr) {
            return std::string(v->valuestring);
        }
    }
    return std::string();
}

// ルートからイベント配列を探す。1段だけ入れ子も追う。
const cJSON* findEventArray(const cJSON* root) {
    if (root == nullptr) return nullptr;
    if (cJSON_IsArray(root)) return root;
    if (!cJSON_IsObject(root)) return nullptr;

    const size_t n = sizeof(kEventsKeys) / sizeof(kEventsKeys[0]);

    const cJSON* v = findByKeys(root, kEventsKeys, n);
    if (v != nullptr && cJSON_IsArray(v)) return v;

    // { "data": { "events": [...] } } のような1段の入れ子。
    if (v != nullptr && cJSON_IsObject(v)) {
        const cJSON* inner = findByKeys(v, kEventsKeys, n);
        if (inner != nullptr && cJSON_IsArray(inner)) return inner;
    }
    return nullptr;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────

bool parseScheduleJson(const std::string& body, ScheduleStore* store,
                       uint32_t* server_now_utc) {
    if (store == nullptr) return false;
    if (body.empty()) {
        ESP_LOGE(TAG, "本文が空");
        return false;
    }

    cJSON* root = cJSON_ParseWithLength(body.c_str(), body.size());
    if (root == nullptr) {
        // 実スキーマ調査のため、先頭だけログに出す(全文はUART0を埋めるので出さない)。
        ESP_LOGE(TAG, "JSONとして解析できない。本文の先頭: %.200s", body.c_str());
        return false;
    }

    const cJSON* arr = findEventArray(root);
    if (arr == nullptr) {
        ESP_LOGE(TAG, "イベント配列が見つからない。本文の先頭: %.200s", body.c_str());
        cJSON_Delete(root);
        return false;
    }

    // storeを壊す前に一旦ローカルへ組み立てる。
    // 途中で失敗しても前回の表示内容を残せるようにするため。
    std::vector<Event> parsed;
    parsed.reserve((size_t)cJSON_GetArraySize(arr));

    int skipped  = 0; // 構造が読めなかった件数。全件これだと解析失敗とみなす
    int filtered = 0; // 意図して表示対象から外した件数。失敗ではない
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, arr) {
        if (!cJSON_IsObject(item)) {
            skipped++;
            continue;
        }

        if (isFlagSet(item, kCancelKeys, sizeof(kCancelKeys) / sizeof(kCancelKeys[0]))) {
            filtered++;
            continue;
        }
        if (isFlagSet(item, kAllDayKeys, sizeof(kAllDayKeys) / sizeof(kAllDayKeys[0]))) {
            filtered++;
            continue;
        }
        const BusyState busy =
            busyState(item, kBusyKeys, sizeof(kBusyKeys) / sizeof(kBusyKeys[0]));
        if (busy == BusyState::Free) {
            filtered++;
            continue;
        }

        Event e = {};
        e.is_tentative = (busy == BusyState::Tentative);
        const cJSON* s = findByKeys(item, kStartKeys, sizeof(kStartKeys) / sizeof(kStartKeys[0]));
        const cJSON* n = findByKeys(item, kEndKeys, sizeof(kEndKeys) / sizeof(kEndKeys[0]));

        if (!toEpoch(s, &e.start_utc)) {
            skipped++;
            continue;
        }
        // 終了時刻が無い/読めない予定は1時間の枠として扱う。
        if (!toEpoch(n, &e.end_utc) || e.end_utc <= e.start_utc) {
            e.end_utc = e.start_utc + 3600u;
        }

        // 旧PC版のclean_text()と同じく、表示前に全角英数などを正規化する。
        e.title = normalizeText(
            toText(findByKeys(item, kTitleKeys, sizeof(kTitleKeys) / sizeof(kTitleKeys[0]))));
        if (e.title.empty()) e.title = "(件名なし)";
        e.location = normalizeText(
            toText(findByKeys(item, kLocKeys, sizeof(kLocKeys) / sizeof(kLocKeys[0]))));

        parsed.push_back(std::move(e));
    }

    if (server_now_utc != nullptr && cJSON_IsObject(root)) {
        const cJSON* now = findByKeys(root, kNowKeys, sizeof(kNowKeys) / sizeof(kNowKeys[0]));
        uint32_t epoch = 0;
        if (toEpoch(now, &epoch)) *server_now_utc = epoch;
    }

    cJSON_Delete(root);

    if (parsed.empty()) {
        // 予定0件とパース失敗を区別する。
        // 配列が空、または全件が中止/終日で除外されただけなら成功扱いにする。
        if (skipped == 0) {
            store->clear();
            ESP_LOGI(TAG, "表示対象の予定は0件(除外%d件)", filtered);
            return true;
        }
        ESP_LOGE(TAG, "%d件すべて解析できなかった", skipped);
        return false;
    }

    store->clear();
    for (const auto& e : parsed) store->add(e);

    ESP_LOGI(TAG, "%d件を取り込んだ(除外%d件 / 解析不能%d件)",
             (int)parsed.size(), filtered, skipped);
    return true;
}
