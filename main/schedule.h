#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct Event {
    uint32_t start_utc;
    uint32_t end_utc;
    std::string title;
    std::string location;
    // showAs=tentative(BusyStatus 1)の仮の予定。表示はするが明滅させず、枠を破線にする。
    bool is_tentative = false;
    // 終日予定(isAllDay)。00:00〜翌00:00の24時間枠なので12時間タイムラインには
    // 載せず、ヘッダー直下の帯に出す(display.cpp、Display::drawAllDayBand())。
    bool is_all_day = false;

    // 以下はダイアログでの詳細表示用。Eventはコピーされる場面が多い(LayoutEvent、
    // HitBox、BlinkEntry)ので、本文の上限を超える巨大な文字列を持たせないこと
    // (json_parser.cppが取り込み時点で刈り込む)。
    std::string body;              // 定型文を刈り込んだ本文(空なら表示しない)
    std::string organizer;         // organizerName
    std::string show_as;           // busy / tentative / free / oof / workingElsewhere(生の値)
    std::string response_status;   // accepted / tentativelyAccepted / declined / notResponded / none
    std::string importance;        // normal / high / low
    std::string sensitivity;       // normal / personal / private / confidential
    std::string categories;        // 配列を", "で連結したもの
    bool        is_organizer  = false;
    bool        is_recurring  = false;
    bool        is_draft      = false;
    uint32_t    last_modified_utc = 0; // 0は不明
};

class ScheduleStore {
public:
    void clear();
    void add(const Event& e);
    // Returns events that overlap [from_utc, to_utc), sorted by start time
    std::vector<Event> getInRange(uint32_t from_utc, uint32_t to_utc) const;
    int count() const { return (int)_events.size(); }

private:
    std::vector<Event> _events;
};
