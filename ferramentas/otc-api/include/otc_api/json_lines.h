#pragma once

#include <string>

namespace otc {

std::string jsonEscape(const std::string& s);
std::string makeEventLine(const char* event, const char* dataObjectJson);

} // namespace otc

