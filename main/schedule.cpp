#include "schedule.h"
#include <algorithm>

void ScheduleStore::clear() {
    _events.clear();
}

void ScheduleStore::add(const Event& e) {
    _events.push_back(e);
}

std::vector<Event> ScheduleStore::getInRange(uint32_t from_utc, uint32_t to_utc) const {
    std::vector<Event> result;
    for (const auto& e : _events) {
        if (e.start_utc < to_utc && e.end_utc > from_utc) {
            result.push_back(e);
        }
    }
    std::sort(result.begin(), result.end(), [](const Event& a, const Event& b) {
        return a.start_utc < b.start_utc;
    });
    return result;
}
