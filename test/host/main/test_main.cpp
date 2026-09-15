// ホスト上の単体テストの入口。LCDもWi-Fiも使わないロジックだけを対象にする。
#include <cstdlib>

#include "unity.h"

// Unityが要求するフック。各テストで共有する状態が無いので空でよい。
void setUp(void) {}
void tearDown(void) {}

void runTextUtilTests(void);
void runTimeUtilTests(void);
void runScheduleTests(void);
void runJsonParserTests(void);

extern "C" void app_main(void) {
    UNITY_BEGIN();
    runTextUtilTests();
    runTimeUtilTests();
    runScheduleTests();
    runJsonParserTests();
    // 失敗があれば非0で終わる。CIやスクリプトから成否を見られるようにする。
    exit(UNITY_END());
}
