#include "json_parser.h"

#include <cstring>
#include <vector>

#include "cJSON.h"

#include "text_util.h"
#include "time_util.h"

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
// 表示対象から外す予定の判定に使う。中止済みの予定だけを除外する。
const char* const kCancelKeys[] = {"isCancelled", "isCanceled", "cancelled", "canceled"};
// 終日予定(00:00〜翌00:00の24時間枠)の判定に使う。除外はせず、Event::is_all_dayへ
// 取り込んでヘッダー直下の帯に出す(display.cpp、Display::drawAllDayBand())。
const char* const kAllDayKeys[] = {"isAllDay", "allDay", "all_day", "isAllday"};
// 空き時間(showAs=free、BusyStatus 0)だけを表示対象から外す。
// 仮の予定(tentative、BusyStatus 1)は表示するが、明滅させず枠線を破線にして
// 確定の予定と区別する(display.cpp、Event::is_tentative)。
// Graphの"showAs"は free / tentative / busy / oof / workingElsewhere を返す。
const char* const kBusyKeys[]   = {"showAs", "busyStatus", "busy", "status",
                                   "freeBusyStatus"};
// ダイアログ詳細表示用。実スキーマ(READMEの実例)のキー名をそのまま候補にする。
// 本文はbodyPreviewを優先する。プレーンテキストの要約で255文字程度に切れるが、
// ダイアログに出す分には十分なため。bodyは数KBあるので、bodyPreviewが空のときだけ
// 使う保険。findByKeys()は「無い/null」しか読み飛ばさないため、空文字列での
// フォールバックはparseScheduleJson()側で個別に判定する(kBodyKeysは使わない)。
const char* const kBodyPreviewKeys[] = {"bodyPreview"};
const char* const kBodyFullKeys[]    = {"body"};
const char* const kOrganizerKeys[]   = {"organizerName", "organizer"};
const char* const kResponseKeys[]    = {"responseStatus", "response_status"};
const char* const kImportanceKeys[]  = {"importance"};
const char* const kSensitivityKeys[] = {"sensitivity"};
const char* const kCategoriesKeys[]  = {"categories"};
const char* const kIsOrganizerKeys[] = {"isOrganizer"};
const char* const kIsRecurringKeys[] = {"isRecurring"};
const char* const kIsDraftKeys[]     = {"isDraft"};
const char* const kLastModKeys[]     = {"lastModifiedAt", "lastModifiedDateTime"};

// 本文の刈り込み後の上限バイト数。数KBある本文を丸ごと保持するとメモリを
// 食うため、表示に使う分だけをEventへ残す。
const size_t kBodyMaxBytes = 768;

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
        if (status == 0) {
            return BusyState::Free;
        }
        if (status == 1) {
            return BusyState::Tentative;
        }
        return BusyState::Other;
    }
    if (cJSON_IsString(v) && v->valuestring != nullptr) {
        if (strcasecmp(v->valuestring, "free") == 0) {
            return BusyState::Free;
        }
        if (strcasecmp(v->valuestring, "tentative") == 0) {
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

// organizerがGraph形式({emailAddress:{name,address}})で来た場合の保険。
// name→emailAddress.nameの順に文字列を探す。見つからなければ空。
std::string extractOrganizerText(const cJSON* node) {
    if (node == nullptr) return std::string();
    if (cJSON_IsString(node) && node->valuestring != nullptr) {
        return std::string(node->valuestring);
    }
    if (!cJSON_IsObject(node)) return std::string();

    static const char* const kNameKeys[] = {"name"};
    const cJSON* nameV = findByKeys(node, kNameKeys, 1);
    if (nameV != nullptr && cJSON_IsString(nameV) && nameV->valuestring != nullptr) {
        return std::string(nameV->valuestring);
    }

    static const char* const kEmailKeys[] = {"emailAddress"};
    const cJSON* email = findByKeys(node, kEmailKeys, 1);
    if (email != nullptr && cJSON_IsObject(email)) {
        const cJSON* n2 = findByKeys(email, kNameKeys, 1);
        if (n2 != nullptr && cJSON_IsString(n2) && n2->valuestring != nullptr) {
            return std::string(n2->valuestring);
        }
    }
    return std::string();
}

// categoriesは文字列の配列。", "で連結する。配列でなく文字列ならそのまま使う。
std::string toCategoriesText(const cJSON* node) {
    if (node == nullptr) return std::string();
    if (cJSON_IsString(node) && node->valuestring != nullptr) {
        return std::string(node->valuestring);
    }
    if (!cJSON_IsArray(node)) return std::string();

    std::string out;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, node) {
        if (!cJSON_IsString(item) || item->valuestring == nullptr) continue;
        if (!out.empty()) out += ", ";
        out += item->valuestring;
    }
    return out;
}

// \r\n / \n / \r のいずれでも行に分割する(\r\nは1行として扱う)。
std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> lines;
    std::string cur;
    for (size_t i = 0; i < s.size(); i++) {
        const char c = s[i];
        if (c == '\r') {
            lines.push_back(cur);
            cur.clear();
            if (i + 1 < s.size() && s[i + 1] == '\n') i++;
        } else if (c == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    lines.push_back(cur);
    return lines;
}

// 行末の半角スペース/タブだけを落とす。改行は既に行分割済みなので扱わない。
std::string rtrimSpaces(const std::string& s) {
    const size_t e = s.find_last_not_of(" \t");
    if (e == std::string::npos) return std::string();
    return s.substr(0, e + 1);
}

// UTF-8の文字境界を壊さずにmax_bytes以内へ切る。
std::string truncateUtf8(const std::string& s, size_t max_bytes) {
    if (s.size() <= max_bytes) return s;
    size_t i = 0, last_ok = 0;
    while (i < s.size()) {
        uint32_t cp = 0;
        const size_t adv = utf8NextChar(s, i, &cp);
        if (adv == 0 || i + adv > max_bytes) break;
        i += adv;
        last_ok = i;
    }
    return s.substr(0, last_ok);
}

// 本文の空行を畳んでから正規化する。定型文(区切り線や会議案内)の除去は
// サーバ側で行うため、デバイス側では削らない。
// 手順(順序が意味を持つのでこのまま守ること):
//   1. 行に分割する
//   2. 各行末尾の空白を落とす
//   3. 連続する空行を1行に畳む
//   4. 先頭・末尾の空行を落とす
//   5. kBodyMaxBytesを超えたらUTF-8境界を壊さずに切って"..."を付ける
//   6. normalizeText()を通す
std::string sanitizeBody(const std::string& raw) {
    const std::vector<std::string> lines = splitLines(raw);

    std::vector<std::string> kept;
    kept.reserve(lines.size());
    for (const auto& line : lines) {
        kept.push_back(rtrimSpaces(line));
    }

    std::vector<std::string> collapsed;
    collapsed.reserve(kept.size());
    for (const auto& line : kept) {
        if (line.empty() && !collapsed.empty() && collapsed.back().empty()) continue;
        collapsed.push_back(line);
    }
    while (!collapsed.empty() && collapsed.front().empty()) collapsed.erase(collapsed.begin());
    while (!collapsed.empty() && collapsed.back().empty()) collapsed.pop_back();

    std::string joined;
    for (size_t i = 0; i < collapsed.size(); i++) {
        if (i > 0) joined += '\n';
        joined += collapsed[i];
    }

    if (joined.size() > kBodyMaxBytes) {
        joined = truncateUtf8(joined, kBodyMaxBytes) + "...";
    }

    return normalizeText(joined);
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

bool parseScheduleJson(const std::string& body, ScheduleStore* store) {
    if (store == nullptr) return false;
    if (body.empty()) {
        return false;
    }

    cJSON* root = cJSON_ParseWithLength(body.c_str(), body.size());
    if (root == nullptr) {
        return false;
    }

    const cJSON* arr = findEventArray(root);
    if (arr == nullptr) {
        cJSON_Delete(root);
        return false;
    }

    // storeを壊す前に一旦ローカルへ組み立てる。
    // 途中で失敗しても前回の表示内容を残せるようにするため。
    std::vector<Event> parsed;
    parsed.reserve((size_t)cJSON_GetArraySize(arr));

    int skipped  = 0; // 構造が読めなかった件数。全件これだと解析失敗とみなす
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, arr) {
        if (!cJSON_IsObject(item)) {
            skipped++;
            continue;
        }

        if (isFlagSet(item, kCancelKeys, sizeof(kCancelKeys) / sizeof(kCancelKeys[0]))) {
            continue;
        }
        // 終日予定(isAllDay)は12時間タイムラインには載せず、ヘッダー直下の帯に出す
        // (display.cpp、Display::drawAllDayBand())。
        // **空きの除外より前に判定すること。** Outlookの終日予定は既定で
        // showAs=free(空き時間)になるため、freeの除外をそのまま効かせると
        // 帯へ出す前に1件残らず消える。
        const bool all_day =
            isFlagSet(item, kAllDayKeys, sizeof(kAllDayKeys) / sizeof(kAllDayKeys[0]));

        const BusyState busy =
            busyState(item, kBusyKeys, sizeof(kBusyKeys) / sizeof(kBusyKeys[0]));
        if (busy == BusyState::Free && !all_day) {
            continue;
        }

        Event e = {};
        e.is_tentative = (busy == BusyState::Tentative);
        e.is_all_day   = all_day;
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

        // ダイアログ詳細表示用。本文はbodyPreview(255文字程度で切れる要約)を
        // 優先し、空のときだけbody(数KBある全文)を使う。長さの上限と行の整形は
        // sanitizeBody()が行う(数KBを丸ごと保持しない)。
        {
            std::string raw_body = toText(findByKeys(
                item, kBodyPreviewKeys, sizeof(kBodyPreviewKeys) / sizeof(kBodyPreviewKeys[0])));
            if (raw_body.empty()) {
                raw_body = toText(findByKeys(
                    item, kBodyFullKeys, sizeof(kBodyFullKeys) / sizeof(kBodyFullKeys[0])));
            }
            e.body = sanitizeBody(raw_body);
        }

        e.organizer = normalizeText(extractOrganizerText(findByKeys(
            item, kOrganizerKeys, sizeof(kOrganizerKeys) / sizeof(kOrganizerKeys[0]))));

        {
            // show_asは既存のkBusyKeysの先頭で見つかった文字列をそのまま保持する
            // (busyStateはfree/tentative/その他の判定用で、生の値までは残さないため)。
            const cJSON* busyNode =
                findByKeys(item, kBusyKeys, sizeof(kBusyKeys) / sizeof(kBusyKeys[0]));
            if (busyNode != nullptr && cJSON_IsString(busyNode) &&
                busyNode->valuestring != nullptr) {
                e.show_as = busyNode->valuestring;
            }
        }

        // 識別子は表示側(display.cpp)で日本語化するため、ここでは生のまま保持する。
        e.response_status = toText(
            findByKeys(item, kResponseKeys, sizeof(kResponseKeys) / sizeof(kResponseKeys[0])));
        e.importance = toText(findByKeys(
            item, kImportanceKeys, sizeof(kImportanceKeys) / sizeof(kImportanceKeys[0])));
        e.sensitivity = toText(findByKeys(
            item, kSensitivityKeys, sizeof(kSensitivityKeys) / sizeof(kSensitivityKeys[0])));
        e.categories = normalizeText(toCategoriesText(findByKeys(
            item, kCategoriesKeys, sizeof(kCategoriesKeys) / sizeof(kCategoriesKeys[0]))));

        e.is_organizer = isFlagSet(
            item, kIsOrganizerKeys, sizeof(kIsOrganizerKeys) / sizeof(kIsOrganizerKeys[0]));
        e.is_recurring = isFlagSet(
            item, kIsRecurringKeys, sizeof(kIsRecurringKeys) / sizeof(kIsRecurringKeys[0]));
        e.is_draft = isFlagSet(
            item, kIsDraftKeys, sizeof(kIsDraftKeys) / sizeof(kIsDraftKeys[0]));

        {
            uint32_t lm = 0;
            const cJSON* lmNode = findByKeys(
                item, kLastModKeys, sizeof(kLastModKeys) / sizeof(kLastModKeys[0]));
            if (toEpoch(lmNode, &lm)) e.last_modified_utc = lm;
        }

        parsed.push_back(std::move(e));
    }

    cJSON_Delete(root);

    if (parsed.empty()) {
        // 予定0件とパース失敗を区別する。
        // 配列が空、または全件が中止や空き(showAs=free)で除外されただけなら成功扱いにする。
        if (skipped == 0) {
            store->clear();
            return true;
        }
        return false;
    }

    store->clear();
    for (const auto& e : parsed) store->add(e);

    return true;
}
