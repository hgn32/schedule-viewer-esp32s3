// json_parser.cppのテスト。サーバの実スキーマ(2026-09-15時点)をそのまま当てる。
// 氏名だけは架空のものへ差し替えてある(リポジトリに個人情報を残さないため)。
#include <cstdlib>
#include <ctime>
#include <string>

#include "unity.h"

#include "json_parser.h"
#include "schedule.h"
#include "time_util.h"

namespace {

// 2026-09-15T11:00:00+09:00 / 12:00:00+09:00
const uint32_t kStart1100Jst = 1789437600u;
const uint32_t kEnd1200Jst   = 1789441200u;

void fixTzToUtc() {
    setenv("TZ", "UTC0", 1);
    tzset();
}

// サーバが実際に返す形。showAs=freeの「週報」は除外される想定。
const char* const kRealResponse = R"json({
  "userId": "a0000000",
  "userName": "山田 太郎",
  "date": "2026-09-15",
  "count": 2,
  "events": [
    {
      "subject": "【JMIS】MODインフラチーム定例",
      "start": "2026-09-15T11:00:00+09:00",
      "end": "2026-09-15T12:00:00+09:00",
      "isAllDay": false,
      "bodyPreview": "インフラT\r\n関係各位\r\n\r\n\r\nお疲れ様です。   \r\n",
      "location": "Teams",
      "organizerName": "鈴木　一郎(Suzuki, Ichiro)",
      "showAs": "busy",
      "sensitivity": "normal",
      "isOrganizer": false,
      "responseStatus": "accepted",
      "importance": "normal",
      "isRecurring": true,
      "lastModifiedAt": "2026-09-15T12:10:26+09:00"
    },
    {
      "subject": "週報",
      "start": "2026-09-15T12:50:00+09:00",
      "end": "2026-09-15T12:50:00+09:00",
      "isAllDay": false,
      "bodyPreview": "",
      "location": "",
      "organizerName": "山田　太郎(Yamada, Taro)",
      "showAs": "free",
      "sensitivity": "normal",
      "isOrganizer": true,
      "responseStatus": "none",
      "importance": "normal",
      "isRecurring": true,
      "lastModifiedAt": "2026-09-15T12:51:49+09:00"
    }
  ]
})json";

void testRealSchema() {
    fixTzToUtc();
    ScheduleStore store;
    TEST_ASSERT_TRUE(parseScheduleJson(kRealResponse, &store));

    // showAs=freeの「週報」は除外されるので1件だけ残る。
    TEST_ASSERT_EQUAL_INT(1, store.count());

    auto got = store.getInRange(0, 0xFFFFFFFFu);
    TEST_ASSERT_EQUAL_INT(1, (int)got.size());
    const Event& e = got[0];

    TEST_ASSERT_EQUAL_UINT32(kStart1100Jst, e.start_utc);
    TEST_ASSERT_EQUAL_UINT32(kEnd1200Jst, e.end_utc);
    TEST_ASSERT_EQUAL_STRING("Teams", e.location.c_str());
    TEST_ASSERT_EQUAL_STRING("busy", e.show_as.c_str());
    TEST_ASSERT_EQUAL_STRING("accepted", e.response_status.c_str());
    TEST_ASSERT_EQUAL_STRING("normal", e.sensitivity.c_str());
    TEST_ASSERT_TRUE(e.is_recurring);
    TEST_ASSERT_FALSE(e.is_organizer);
    TEST_ASSERT_FALSE(e.is_tentative);
    TEST_ASSERT_FALSE(e.is_all_day);
    TEST_ASSERT_NOT_EQUAL(0u, e.last_modified_utc);
    // 開催者は全角スペースを半角へ正規化して取り込む。
    TEST_ASSERT_EQUAL_STRING("鈴木 一郎(Suzuki, Ichiro)", e.organizer.c_str());
}

void testBodyPreviewIsTidied() {
    fixTzToUtc();
    ScheduleStore store;
    TEST_ASSERT_TRUE(parseScheduleJson(kRealResponse, &store));
    auto got = store.getInRange(0, 0xFFFFFFFFu);
    TEST_ASSERT_EQUAL_INT(1, (int)got.size());

    // 連続する空行は1行に畳み、行末の空白と末尾の空行は落とす。改行は残す。
    TEST_ASSERT_EQUAL_STRING("インフラT\n関係各位\n\nお疲れ様です。",
                             got[0].body.c_str());
}

// 終日予定はOutlookの既定でshowAs=freeになる。空きの除外より前に判定しないと
// 1件も残らない(実際にこの順序で消えていたため、退行の検出用に残す)。
void testAllDayIsKeptEvenWhenFree() {
    fixTzToUtc();
    const char* const json = R"json({"events":[
      {"subject":"健康診断","start":"2026-09-15T00:00:00+09:00",
       "end":"2026-09-16T00:00:00+09:00","isAllDay":true,"showAs":"free"}
    ]})json";

    ScheduleStore store;
    TEST_ASSERT_TRUE(parseScheduleJson(json, &store));
    TEST_ASSERT_EQUAL_INT(1, store.count());

    auto got = store.getInRange(0, 0xFFFFFFFFu);
    TEST_ASSERT_TRUE(got[0].is_all_day);
    TEST_ASSERT_EQUAL_STRING("健康診断", got[0].title.c_str());
}

void testCancelledIsExcludedEvenWhenAllDay() {
    fixTzToUtc();
    const char* const json = R"json({"events":[
      {"subject":"中止済み","start":"2026-09-15T00:00:00+09:00",
       "end":"2026-09-16T00:00:00+09:00","isAllDay":true,"isCancelled":true},
      {"subject":"残る","start":"2026-09-15T11:00:00+09:00",
       "end":"2026-09-15T12:00:00+09:00","showAs":"busy"}
    ]})json";

    ScheduleStore store;
    TEST_ASSERT_TRUE(parseScheduleJson(json, &store));
    TEST_ASSERT_EQUAL_INT(1, store.count());
    TEST_ASSERT_EQUAL_STRING("残る", store.getInRange(0, 0xFFFFFFFFu)[0].title.c_str());
}

void testTentativeIsKept() {
    fixTzToUtc();
    const char* const json = R"json({"events":[
      {"subject":"仮","start":"2026-09-15T11:00:00+09:00",
       "end":"2026-09-15T12:00:00+09:00","showAs":"tentative"}
    ]})json";

    ScheduleStore store;
    TEST_ASSERT_TRUE(parseScheduleJson(json, &store));
    TEST_ASSERT_EQUAL_INT(1, store.count());
    TEST_ASSERT_TRUE(store.getInRange(0, 0xFFFFFFFFu)[0].is_tentative);
}

void testEndMissingOrNotAfterStartBecomesOneHour() {
    fixTzToUtc();
    const char* const json = R"json({"events":[
      {"subject":"終了なし","start":"2026-09-15T11:00:00+09:00"},
      {"subject":"同時刻","start":"2026-09-15T11:00:00+09:00",
       "end":"2026-09-15T11:00:00+09:00"}
    ]})json";

    ScheduleStore store;
    TEST_ASSERT_TRUE(parseScheduleJson(json, &store));
    auto got = store.getInRange(0, 0xFFFFFFFFu);
    TEST_ASSERT_EQUAL_INT(2, (int)got.size());
    for (const auto& e : got) {
        TEST_ASSERT_EQUAL_UINT32(kStart1100Jst, e.start_utc);
        TEST_ASSERT_EQUAL_UINT32(kStart1100Jst + 3600u, e.end_utc);
    }
}

void testAcceptsRootArrayAndGraphKeys() {
    fixTzToUtc();
    // ルートが配列。時刻はGraphの{dateTime,timeZone}形式。
    const char* const json = R"json([
      {"subject":"Graph形式","startDateTime":{"dateTime":"2026-09-15T11:00:00.0000000",
       "timeZone":"Tokyo Standard Time"},
       "endDateTime":{"dateTime":"2026-09-15T12:00:00.0000000",
       "timeZone":"Tokyo Standard Time"}}
    ])json";

    ScheduleStore store;
    TEST_ASSERT_TRUE(parseScheduleJson(json, &store));
    TEST_ASSERT_EQUAL_INT(1, store.count());
    auto got = store.getInRange(0, 0xFFFFFFFFu);
    TEST_ASSERT_EQUAL_UINT32(kStart1100Jst, got[0].start_utc);
    TEST_ASSERT_EQUAL_UINT32(kEnd1200Jst, got[0].end_utc);
}

void testTitleFallbackWhenMissing() {
    fixTzToUtc();
    const char* const json = R"json({"events":[
      {"start":"2026-09-15T11:00:00+09:00","end":"2026-09-15T12:00:00+09:00"}
    ]})json";

    ScheduleStore store;
    TEST_ASSERT_TRUE(parseScheduleJson(json, &store));
    TEST_ASSERT_EQUAL_STRING("(件名なし)",
                             store.getInRange(0, 0xFFFFFFFFu)[0].title.c_str());
}

void testRejectsBrokenInput() {
    fixTzToUtc();
    ScheduleStore store;
    store.add(Event{});
    const int before = store.count();

    TEST_ASSERT_FALSE(parseScheduleJson("", &store));
    TEST_ASSERT_FALSE(parseScheduleJson("{ this is not json", &store));
    TEST_ASSERT_FALSE(parseScheduleJson(kRealResponse, nullptr));

    // 失敗時はstoreを触らない(前回の表示内容を残せるようにするため)。
    TEST_ASSERT_EQUAL_INT(before, store.count());
}

void testAllFilteredIsNotAFailure() {
    fixTzToUtc();
    // 全件が除外(showAs=free)。「予定0件」であって取得失敗ではない。
    const char* const json = R"json({"events":[
      {"subject":"空き","start":"2026-09-15T11:00:00+09:00",
       "end":"2026-09-15T12:00:00+09:00","showAs":"free"}
    ]})json";

    // 前回の内容が残っていても、0件の取得が成功したら消して空にする。
    ScheduleStore store;
    store.add(Event{});
    TEST_ASSERT_TRUE(parseScheduleJson(json, &store));
    TEST_ASSERT_EQUAL_INT(0, store.count());

    // 配列そのものが空の場合も同じく成功(0件)扱いで、失敗ではない。
    ScheduleStore empty_store;
    TEST_ASSERT_TRUE(parseScheduleJson(R"json({"events":[]})json", &empty_store));
    TEST_ASSERT_EQUAL_INT(0, empty_store.count());
}

}  // namespace

void runJsonParserTests(void) {
    RUN_TEST(testRealSchema);
    RUN_TEST(testBodyPreviewIsTidied);
    RUN_TEST(testAllDayIsKeptEvenWhenFree);
    RUN_TEST(testCancelledIsExcludedEvenWhenAllDay);
    RUN_TEST(testTentativeIsKept);
    RUN_TEST(testEndMissingOrNotAfterStartBecomesOneHour);
    RUN_TEST(testAcceptsRootArrayAndGraphKeys);
    RUN_TEST(testTitleFallbackWhenMissing);
    RUN_TEST(testRejectsBrokenInput);
    RUN_TEST(testAllFilteredIsNotAFailure);
}
