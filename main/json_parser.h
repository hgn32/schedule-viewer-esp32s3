#pragma once
#include <cstdint>
#include <string>

#include "schedule.h"

// サーバから受け取ったJSONをScheduleStoreへ流し込む。
//
// 実スキーマは判明済み(READMEの「通信プロトコル(サーバ → デバイス)」章に実例がある)。
// 候補表(kEventsKeys / kTitleKeysなど)はAPIの仕様変更に対する保険として残してあり、
// 1つに削っても動作は変わらない。
//
// 表示対象から外す予定:
//   isCancelled: true         中止された会議
//   showAs:      free         空き(OutlookのBusyStatus 0)
// showAs: tentative(BusyStatus 1、仮の予定)は表示するが、明滅させず枠線を破線にする
// (Event::is_tentative、main/display.cpp)。
// isAllDay: true(終日予定、00:00〜翌00:00の24時間枠)は除外せず取り込む。
// 12時間タイムラインには載せず、ヘッダー直下の帯に出す
// (Event::is_all_day、main/display.cpp、Display::drawAllDayBand())。
//
// 受け付ける全体構造:
//   [ {...}, {...} ]                     ルートが配列
//   { "events":  [...] }                 以下はキー名の候補(kEventsKeys)
//   { "value":   [...] }                 Microsoft GraphのcalendarView形式
//   { "items":   [...] } / "data" / "schedule" / "schedules" / "appointments"
//   { "data": { "events": [...] } }      1段だけ入れ子も追う
//
// 1件あたりのフィールド:
//   件名     subject / title / name / summary
//   開始     start / startTime / start_time / startDateTime / begin
//   終了     end / endTime / end_time / endDateTime / finish
//   場所     location / place / room / locationName
//   本文     bodyPreview(無ければbody)。行末の空白除去・連続空行の畳み込みを
//            行い、kBodyMaxBytesで切ってからEvent::bodyへ格納する(詳細ダイアログ用)。
//            定型文の除去はサーバ側で行うため、デバイス側では削らない
//   主催者   organizerName / organizer
//   状態等   responseStatus / importance / sensitivity / categories /
//            isOrganizer / isRecurring / isDraft / lastModifiedAt
//
// 時刻の値は次のいずれでもよい:
//   "2026-08-31T09:00:00Z" / "2026-08-31T09:00:00+09:00" / "2026-08-31 09:00:00"
//   1756598400            (epoch秒。1e11を超える場合はミリ秒とみなす)
//   { "dateTime": "2026-08-31T00:00:00.0000000", "timeZone": "UTC" }

// bodyを解析してstoreへ追加する(成功時は先にstore->clear()する)。
// 1件も取り出せなかった場合はfalseを返す(storeは変更しない)。
bool parseScheduleJson(const std::string& body, ScheduleStore* store);
