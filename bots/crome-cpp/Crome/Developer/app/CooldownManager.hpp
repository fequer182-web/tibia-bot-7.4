#pragma once
#include <chrono>
#include <string>
#include <unordered_map>
namespace crome { class CooldownManager { using Clock=std::chrono::steady_clock; std::unordered_map<std::string,Clock::time_point> last_; public: bool ready(const std::string&,std::chrono::milliseconds)const; void mark(const std::string&); }; }
