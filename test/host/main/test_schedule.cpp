// schedule.cppのテスト。ScheduleStoreは外部依存の無いデータ構造なので、
// 期間の重なり判定とソート順をホスト上で検証できる。
#include "unity.h"

#include "schedule.h"

namespace {

Event makeEvent(uint32_t start, uint32_t end, const char* title) {
    Event e   = {};
    e.start_utc = start;
    e.end_utc   = end;
    e.title     = title;
    return e;
}

void testGetInRangeOverlap() {
    ScheduleStore s;
    s.add(makeEvent(1000, 2000, "before"));   // 窓より前
    s.add(makeEvent(2500, 3500, "crossing")); // 窓の先頭をまたぐ
    s.add(makeEvent(3500, 4000, "inside"));   // 窓の中
    s.add(makeEvent(4500, 6000, "tail"));     // 窓の末尾をまたぐ
    s.add(makeEvent(6000, 7000, "after"));    // 窓より後

    // 窓は[3000, 5000)。
    auto got = s.getInRange(3000, 5000);
    TEST_ASSERT_EQUAL_INT(3, (int)got.size());
    TEST_ASSERT_EQUAL_STRING("crossing", got[0].title.c_str());
    TEST_ASSERT_EQUAL_STRING("inside", got[1].title.c_str());
    TEST_ASSERT_EQUAL_STRING("tail", got[2].title.c_str());
}

void testGetInRangeBoundaries() {
    ScheduleStore s;
    // 終了が窓の先頭ちょうど = 含まない。
    s.add(makeEvent(2000, 3000, "ends at from"));
    // 開始が窓の末尾ちょうど = 含まない。
    s.add(makeEvent(5000, 6000, "starts at to"));

    auto got = s.getInRange(3000, 5000);
    TEST_ASSERT_EQUAL_INT(0, (int)got.size());
}

void testGetInRangeSortsByStart() {
    ScheduleStore s;
    s.add(makeEvent(4000, 4500, "third"));
    s.add(makeEvent(3100, 3200, "first"));
    s.add(makeEvent(3500, 3600, "second"));

    auto got = s.getInRange(3000, 5000);
    TEST_ASSERT_EQUAL_INT(3, (int)got.size());
    TEST_ASSERT_EQUAL_STRING("first", got[0].title.c_str());
    TEST_ASSERT_EQUAL_STRING("second", got[1].title.c_str());
    TEST_ASSERT_EQUAL_STRING("third", got[2].title.c_str());
}

void testClearAndCount() {
    ScheduleStore s;
    s.add(makeEvent(1000, 2000, "a"));
    s.add(makeEvent(2000, 3000, "b"));
    TEST_ASSERT_EQUAL_INT(2, s.count());
    s.clear();
    TEST_ASSERT_EQUAL_INT(0, s.count());
    TEST_ASSERT_EQUAL_INT(0, (int)s.getInRange(0, 100000).size());
}

}  // namespace

void runScheduleTests(void) {
    RUN_TEST(testGetInRangeOverlap);
    RUN_TEST(testGetInRangeBoundaries);
    RUN_TEST(testGetInRangeSortsByStart);
    RUN_TEST(testClearAndCount);
}
