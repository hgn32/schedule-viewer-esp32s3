#pragma once
#include <cstdint>
#include <string>
#include "schedule.h"

// Parses the text protocol from the PC scheduler sender.
// Transport-agnostic: feed one line at a time from any source (UART, WiFi, etc.)
//
// Protocol (PC → device):
//   NOW:{utc_epoch}
//   EVENT:CLEAR
//   EVENT:ADD\t{start_utc}\t{end_utc}\t{title}\t{location}
//   EVENT:FINISH
//
// Device → PC:
//   REQ:ALL
class Protocol {
public:
    explicit Protocol(ScheduleStore* store) : _store(store) {}

    void processLine(const std::string& line);

    bool isComplete() const   { return _complete; }
    void resetComplete()      { _complete = false; }

    // UTC epoch received from NOW: message (0 if not yet received)
    uint32_t getReceivedTime() const { return _received_time; }

private:
    ScheduleStore* _store;
    bool     _complete      = false;
    uint32_t _received_time = 0;
};
