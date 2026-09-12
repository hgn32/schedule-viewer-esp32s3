#pragma once
#include <cstdint>
#include <string>

// 一時的なダミー予定表示モード用(main/secrets.hのUSE_DUMMY_SCHEDULEが1のときだけ使う)。
// サーバ(SCHEDULE_URL)へ到達できない環境で画面を確認するための暫定手段。
// serial_link/protocol/pc_pythonと同じ扱いのフォールバックで、到達性が確認できたら
// main/sntp_time.cpp/.hと合わせて撤去してよい(CLAUDE.md参照)。
//
// M5にもIDFにも依存させない。time_util.hだけ使う。

// now_utcを基準に、相対時刻で予定を組み立てたJSON文字列を返す。
// main/json_parser.cppが受け付ける候補表のうち最優先の形
// (events / subject / start / end / location / isCancelled / isAllDay)に合わせている。
// 一部の予定は仮の予定・空き予定の確認用に"showAs"(tentative/free)も含む。
// 固定の絶対日時は使わない(翌日には表示範囲外になるため)。
// 件名の先頭には必ず"[ダミー]"を付け、ダミー表示中であることが画面上で分かるようにする。
// JSONには現在時刻を表すフィールド(now/serverTime/currentTime等)を入れない
// (呼び出し側でSNTPが入れたシステム時刻を上書きしないため)。
std::string dummyScheduleJson(uint32_t now_utc);
