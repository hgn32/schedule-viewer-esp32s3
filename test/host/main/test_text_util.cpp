// text_util.cppのテスト。件名・場所・本文の正規化はここだけで完結するロジックなので、
// 実機を使わずに検証できる。
#include <string>

#include "unity.h"

#include "text_util.h"

namespace {

void testNormalizeFullWidthSpace() {
    TEST_ASSERT_EQUAL_STRING("A B", normalizeText("A\xE3\x80\x80" "B").c_str());
}

void testNormalizeFullWidthAlnum() {
    // "ＡＢＣ１２３" -> "ABC123"
    TEST_ASSERT_EQUAL_STRING(
        "ABC123",
        normalizeText("\xEF\xBC\xA1\xEF\xBC\xA2\xEF\xBC\xA3"
                      "\xEF\xBC\x91\xEF\xBC\x92\xEF\xBC\x93").c_str());
}

void testNormalizeHalfWidthKana() {
    // "ｶﾞ" (半角カ+濁点) -> "ガ"(全角、濁点を合成する)
    TEST_ASSERT_EQUAL_STRING("\xE3\x82\xAC",
                             normalizeText("\xEF\xBD\xB6\xEF\xBE\x9E").c_str());
    // "ﾊﾟ" (半角ハ+半濁点) -> "パ"
    TEST_ASSERT_EQUAL_STRING("\xE3\x83\x91",
                             normalizeText("\xEF\xBE\x8A\xEF\xBE\x9F").c_str());
}

void testNormalizeTrimsEnds() {
    TEST_ASSERT_EQUAL_STRING("abc", normalizeText("  abc  ").c_str());
    // 全角スペースも半角化したうえで落とす。
    TEST_ASSERT_EQUAL_STRING("abc", normalizeText("\xE3\x80\x80" "abc\xE3\x80\x80").c_str());
}

void testNormalizeEmpty() {
    TEST_ASSERT_EQUAL_STRING("", normalizeText("").c_str());
    TEST_ASSERT_EQUAL_STRING("", normalizeText("   ").c_str());
}

void testUtf8NextCharAscii() {
    uint32_t cp = 0;
    TEST_ASSERT_EQUAL_UINT32(1, utf8NextChar("A", 0, &cp));
    TEST_ASSERT_EQUAL_UINT32(0x41, cp);
}

void testUtf8NextCharMultiByte() {
    uint32_t cp = 0;
    // "あ" = U+3042 = E3 81 82
    TEST_ASSERT_EQUAL_UINT32(3, utf8NextChar("\xE3\x81\x82", 0, &cp));
    TEST_ASSERT_EQUAL_UINT32(0x3042, cp);
}

void testUtf8NextCharInvalidByte() {
    uint32_t cp = 0;
    // 不正な並びは1バイト進めてU+FFFDにする(無限ループにしないため)。
    TEST_ASSERT_EQUAL_UINT32(1, utf8NextChar("\xFF", 0, &cp));
    TEST_ASSERT_EQUAL_UINT32(0xFFFD, cp);
}

void testUtf8NextCharGuards() {
    uint32_t cp = 0;
    TEST_ASSERT_EQUAL_UINT32(0, utf8NextChar("A", 1, &cp));   // 範囲外
    TEST_ASSERT_EQUAL_UINT32(0, utf8NextChar("A", 0, nullptr)); // NULL
}

}  // namespace

void runTextUtilTests(void) {
    RUN_TEST(testNormalizeFullWidthSpace);
    RUN_TEST(testNormalizeFullWidthAlnum);
    RUN_TEST(testNormalizeHalfWidthKana);
    RUN_TEST(testNormalizeTrimsEnds);
    RUN_TEST(testNormalizeEmpty);
    RUN_TEST(testUtf8NextCharAscii);
    RUN_TEST(testUtf8NextCharMultiByte);
    RUN_TEST(testUtf8NextCharInvalidByte);
    RUN_TEST(testUtf8NextCharGuards);
}
