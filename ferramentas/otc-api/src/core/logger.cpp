#include "otc_api/logger.h"

#include <Windows.h>
#include <share.h>

#include <cstdarg>
#include <string>

namespace otc {

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

Logger::~Logger() {
    std::lock_guard<std::mutex> lock(mtx_);
    if (file_) {
        fclose(file_);
        file_ = nullptr;
    }
}

void Logger::init() {
    std::lock_guard<std::mutex> lock(mtx_);
    if (file_)
        return;

    char exePath[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);

    std::string path(exePath);
    const auto slash = path.find_last_of("\\/");
    if (slash != std::string::npos)
        path.resize(slash + 1);
    path += "otc-api.log";

    FILE* f = _fsopen(path.c_str(), "w", _SH_DENYWR);
    if (!f)
        return;
    file_ = f;

    logPath_ = path;
    fprintf(file_, "[otc-api] log started host=%s\n", exePath);
    fflush(file_);
}

void Logger::log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!file_)
        return;

    SYSTEMTIME st{};
    GetLocalTime(&st);
    fprintf(file_, "[%02u:%02u:%02u.%03u] ",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    va_list args;
    va_start(args, fmt);
    vfprintf(file_, fmt, args);
    va_end(args);
    fprintf(file_, "\n");
    fflush(file_);

    char dbg[1024];
    va_start(args, fmt);
    vsnprintf(dbg, sizeof(dbg), fmt, args);
    va_end(args);
    OutputDebugStringA(dbg);
    OutputDebugStringA("\n");
}

} // namespace otc

