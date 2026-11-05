#pragma once

#include <cstdio>
#include <mutex>
#include <string>

namespace otc {

class Logger {
public:
    static Logger& instance();

    void init();
    void log(const char* fmt, ...);
    const std::string& logPath() const { return logPath_; }
    ~Logger();

private:
    Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    FILE* file_ = nullptr;
    std::mutex mtx_;
    std::string logPath_;
};

} // namespace otc

#define LOG(fmt, ...) otc::Logger::instance().log(fmt, ##__VA_ARGS__)

