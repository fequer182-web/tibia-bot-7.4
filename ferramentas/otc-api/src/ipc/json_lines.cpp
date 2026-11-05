#include "otc_api/json_lines.h"

#include <cstdio>
#include <string>

namespace otc {

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"':  out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
            break;
        }
    }
    return out;
}

std::string makeEventLine(const char* event, const char* dataObjectJson) {
    std::string line = "{\"type\":\"event\",\"event\":\"";
    line += jsonEscape(event ? event : "");
    line += "\",\"data\":";
    line += (dataObjectJson && dataObjectJson[0]) ? dataObjectJson : "{}";
    line += "}\n";
    return line;
}

} // namespace otc

