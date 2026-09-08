#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct Event {
    uint32_t start_utc;
    uint32_t end_utc;
    std::string title;
    std::string location;
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
