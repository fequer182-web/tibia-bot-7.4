#pragma once

#include <Windows.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace otc {

inline constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\OTCApi";

class PipeServer {
public:
    static PipeServer& instance();

    using CommandHandler = std::function<void(const std::string& jsonLine)>;

    void setCommandHandler(CommandHandler handler);
    bool start();
    void stop();
    bool isRunning() const { return running_.load(); }

    bool emit(const char* event, const char* dataObjectJson);
    bool emit(const char* event, const std::string& dataObjectJson);
    bool tryEmit(const char* event, const char* dataObjectJson);
    bool tryEmit(const char* event, const std::string& dataObjectJson);

    using ConnectHandler = std::function<void()>;
    void setConnectHandler(ConnectHandler handler);

private:
    PipeServer() = default;
    ~PipeServer();
    PipeServer(const PipeServer&) = delete;
    PipeServer& operator=(const PipeServer&) = delete;

    void acceptLoop();
    void clientLoop(HANDLE pipe);
    bool writeLine(const std::string& line);
    std::wstring pipeName() const;

    CommandHandler commandHandler_;
    ConnectHandler connectHandler_;
    std::thread acceptThread_;
    std::mutex writeMtx_;
    std::mutex handlerMtx_;
    std::atomic<bool> running_{false};
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    std::wstring pipeName_;
};

} // namespace otc

