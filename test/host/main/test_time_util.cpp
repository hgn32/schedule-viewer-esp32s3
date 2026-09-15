// time_util.hのテスト。libcのTZがUTCである前提で書かれたヘッダなので、
// テスト側でもTZ=UTCを固定してから呼ぶ(app_main()がTZをUTC0にするのと同じ条件)。
#include <cstdlib>
#include <ctime>

#include "unity.h"

#include "time_util.h"

namespace {

// 2026-09-15T11:00:00+09:00 = 2026-09-15T02:00:00Z
const uint32_t kSep15_0200Z = 1789437600u;
// 2026-09-15T00:00:00+09:00 = 2026-09-14T15:00:00Z(終日予定の開始に相当)
const uint32_t kSep14_1500Z = 1789398000u;

void fixTzToUtc() {
    setenv("TZ", "UTC0", 1);
    tzset();
}

void testParseIso8601WithJstOffset() {
    fixTzToUtc();
    uint32_t out = 0;
    TEST_ASSERT_TRUE(parseIso8601("2026-09-15T11:00:00+09:00", &out));
    TEST_ASSERT_EQUAL_UINT32(kSep15_0200Z, out);
}

void testParseIso8601OffsetWithoutColon() {
    fixTzToUtc();
    uint32_t out = 0;
    TEST_ASSERT_TRUE(parseIso8601("2026-09-15T11:00:00+0900", &out));
    TEST_ASSERT_EQUAL_UINT32(kSep15_0200Z, out);
}

void testParseIso8601Zulu() {
    fixTzToUtc();
    uint32_t out = 0;
    TEST_ASSERT_TRUE(parseIso8601("2026-09-15T02:00:00Z", &out));
    TEST_ASSERT_EQUAL_UINT32(kSep15_0200Z, out);
}

void testParseIso8601AllDayStart() {
    fixTzToUtc();
    uint32_t out = 0;
    TEST_ASSERT_TRUE(parseIso8601("2026-09-15T00:00:00+09:00", &out));
    TEST_ASSERT_EQUAL_UINT32(kSep14_1500Z, out);
}

void testParseIso8601FractionAndSpace() {
    fixTzToUtc();
    uint32_t a = 0, b = 0;
    // 小数秒は読み飛ばす。区切りの空白も受ける。オフセット無しはUTC扱い。
    TEST_ASSERT_TRUE(parseIso8601("2026-09-15T02:00:00.1234567", &a));
    TEST_ASSERT_TRUE(parseIso8601("2026-09-15 02:00:00", &b));
    TEST_ASSERT_EQUAL_UINT32(kSep15_0200Z, a);
    TEST_ASSERT_EQUAL_UINT32(kSep15_0200Z, b);
}

void testParseIso8601FallbackOffset() {
    fixTzToUtc();
    uint32_t out = 0;
    // オフセットが書かれていないときだけfallbackを引く(Graphの{dateTime,timeZone}用)。
    TEST_ASSERT_TRUE(parseIso8601("2026-09-15T11:00:00", &out, JST_OFFSET));
    TEST_ASSERT_EQUAL_UINT32(kSep15_0200Z, out);

    // 明示のZがある場合はfallbackを無視する。
    TEST_ASSERT_TRUE(parseIso8601("2026-09-15T02:00:00Z", &out, JST_OFFSET));
    TEST_ASSERT_EQUAL_UINT32(kSep15_0200Z, out);
}

void testParseIso8601Rejects() {
    fixTzToUtc();
    uint32_t out = 12345u;
    TEST_ASSERT_FALSE(parseIso8601(nullptr, &out));
    TEST_ASSERT_FALSE(parseIso8601("2026-09-15", &out));
    TEST_ASSERT_FALSE(parseIso8601("not a date", &out));
    TEST_ASSERT_FALSE(parseIso8601("2026-13-15T00:00:00Z", &out)); // 月が範囲外
    TEST_ASSERT_FALSE(parseIso8601("2026-09-15X02:00:00Z", &out)); // 区切りが不正
    // 失敗時は*outを触らない。
    TEST_ASSERT_EQUAL_UINT32(12345u, out);
}

void testFormatDate() {
    fixTzToUtc();
    // 2026-09-15はJSTで火曜日。月日はゼロ埋めしない。
    time_t t = (time_t)(kSep15_0200Z + JST_OFFSET);
    struct tm jst;
    gmtime_r(&t, &jst);
    TEST_ASSERT_EQUAL_STRING("9\xE6\x9C\x88" "15\xE6\x97\xA5(\xE7\x81\xAB)",
                             formatDate(jst).c_str());
}

void testFormatTimeAndHour() {
    fixTzToUtc();
    time_t t = (time_t)(kSep15_0200Z + JST_OFFSET);
    struct tm jst;
    gmtime_r(&t, &jst);
    TEST_ASSERT_EQUAL_STRING("11:00", formatTime(jst).c_str());
    TEST_ASSERT_EQUAL_STRING("09", formatHour(9).c_str());
    TEST_ASSERT_EQUAL_STRING("23", formatHour(23).c_str());
    TEST_ASSERT_EQUAL_STRING("00", formatHour(24).c_str()); // 24時は00時に丸める
}

}  // namespace

void runTimeUtilTests(void) {
    RUN_TEST(testParseIso8601WithJstOffset);
    RUN_TEST(testParseIso8601OffsetWithoutColon);
    RUN_TEST(testParseIso8601Zulu);
    RUN_TEST(testParseIso8601AllDayStart);
    RUN_TEST(testParseIso8601FractionAndSpace);
    RUN_TEST(testParseIso8601FallbackOffset);
    RUN_TEST(testParseIso8601Rejects);
    RUN_TEST(testFormatDate);
    RUN_TEST(testFormatTimeAndHour);
}
