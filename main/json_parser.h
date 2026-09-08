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
//   isCancelled: true  中止された会議
//   isAllDay:    true  00:00〜翌00:00の24時間枠になり6時間タイムラインを潰すため
//
// 受け付ける全体構造:
//   [ {...}, {...} ]                     ルートが配列
//   { "events":  [...] }                 以下はキー名の候補(kEventsKeys)
//   { "value":   [...] }                 Microsoft GraphのcalendarView形式
//   { "items":   [...] } / "data" / "schedule" / "schedules" / "appointments"
//   { "data": { "events": [...] } }      1段だけ入れ子も追う
//
// 1件あたりのフィールド:
//   件名   subject / title / name / summary
//   開始   start / startTime / start_time / startDateTime / begin
//   終了   end / endTime / end_time / endDateTime / finish
//   場所   location / place / room / locationName
//
// 時刻の値は次のいずれでもよい:
//   "2026-08-31T09:00:00Z" / "2026-08-31T09:00:00+09:00" / "2026-08-31 09:00:00"
//   1756598400            (epoch秒。1e11を超える場合はミリ秒とみなす)
//   { "dateTime": "2026-08-31T00:00:00.0000000", "timeZone": "UTC" }

// bodyを解析してstoreへ追加する(成功時は先にstore->clear()する)。
// server_now_utcが非nullで、JSON側に現在時刻(now / serverTime / currentTime)が
// 入っていればそれを書き込む。無ければ触らない。
// 1件も取り出せなかった場合はfalseを返す(storeは変更しない)。
bool parseScheduleJson(const std::string& body, ScheduleStore* store,
                       uint32_t* server_now_utc);
