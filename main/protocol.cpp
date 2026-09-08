#include "protocol.h"
#include <cstdlib>
#include <cstring>

namespace {

bool startsWith(const std::string& s, const char* prefix) {
    return s.compare(0, strlen(prefix), prefix) == 0;
}

// Arduino StringのtoInt()相当。数字以外で止まり、変換できなければ0を返す。
uint32_t toUint32(const std::string& s) {
    return (uint32_t)strtoul(s.c_str(), nullptr, 10);
}

// Arduino StringのtrimI()相当。前後の空白と制御文字を落とす。
void trim(std::string& s) {
    const char* ws = " \t\r\n\v\f";
    size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos) { s.clear(); return; }
    size_t e = s.find_last_not_of(ws);
    s = s.substr(b, e - b + 1);
}

} // namespace

void Protocol::processLine(const std::string& line) {
    if (startsWith(line, "NOW:")) {
        _received_time = toUint32(line.substr(4));

    } else if (line == "EVENT:CLEAR") {
        _store->clear();
        _complete = false;

    } else if (startsWith(line, "EVENT:ADD\t")) {
        // EVENT:ADD\t{start}\t{end}\t{title}\t{location}
        std::string payload = line.substr(10);

        size_t t1 = payload.find('\t');
        if (t1 == std::string::npos) return;
        size_t t2 = payload.find('\t', t1 + 1);
        if (t2 == std::string::npos) return;
        size_t t3 = payload.find('\t', t2 + 1);

        Event e;
        e.start_utc = toUint32(payload.substr(0, t1));
        e.end_utc   = toUint32(payload.substr(t1 + 1, t2 - t1 - 1));
        if (t3 != std::string::npos) {
            e.title    = payload.substr(t2 + 1, t3 - t2 - 1);
            e.location = payload.substr(t3 + 1);
        } else {
            e.title    = payload.substr(t2 + 1);
            e.location = "";
        }
        trim(e.title);
        trim(e.location);
        _store->add(e);

    } else if (line == "EVENT:FINISH") {
        _complete = true;
    }
}
