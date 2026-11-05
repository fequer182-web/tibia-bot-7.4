#include "otc_api/pipe_server.h"

#include "otc_api/json_lines.h"
#include "otc_api/logger.h"

#include <cwchar>

namespace otc {
namespace {

constexpr DWORD kBufSize = 64 * 1024;

void disconnectPipe(HANDLE& pipe) {
    if (pipe == INVALID_HANDLE_VALUE)
        return;
    FlushFileBuffers(pipe);
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
    pipe = INVALID_HANDLE_VALUE;
}

} // namespace

PipeServer& PipeServer::instance() {
    static PipeServer inst;
    return inst;
}

PipeServer::~PipeServer() {
    running_ = false;
}

void PipeServer::setCommandHandler(CommandHandler handler) {
    std::lock_guard<std::mutex> lock(handlerMtx_);
    commandHandler_ = std::move(handler);
}

void PipeServer::setConnectHandler(ConnectHandler handler) {
    std::lock_guard<std::mutex> lock(handlerMtx_);
        connectHandler_ = std::move(handler);
}

bool PipeServer::emit(const char* event, const std::string& dataObjectJson) {
    return emit(event, dataObjectJson.c_str());
}

bool PipeServer::start() {
    if (running_.exchange(true))
        return true;

    pipeName_ = pipeName();
    wchar_t readyName[128]{};
    _snwprintf_s(readyName, _countof(readyName), _TRUNCATE,
                 L"Local\\OTCApiPipeReady_%lu", GetCurrentProcessId());
    if (HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, readyName)) {
        SetEvent(ready);
        CloseHandle(ready);
    }
    acceptThread_ = std::thread([this] { acceptLoop(); });
    LOG("pipe listening on %ls", pipeName_.c_str());
    return true;
}

void PipeServer::stop() {
    if (!running_.exchange(false))
        return;

    HANDLE current = INVALID_HANDLE_VALUE;
    {
        std::lock_guard<std::mutex> lock(writeMtx_);
        current = pipe_;
        pipe_ = INVALID_HANDLE_VALUE;
    }
    if (current != INVALID_HANDLE_VALUE) {
        CancelIoEx(current, nullptr);
        disconnectPipe(current);
    }

    // Unblock ConnectNamedPipe by opening the pipe from this process.
    HANDLE wake = CreateFileW(pipeName_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, 0, nullptr);
    if (wake != INVALID_HANDLE_VALUE)
        CloseHandle(wake);

    if (acceptThread_.joinable())
        acceptThread_.join();
}

std::wstring PipeServer::pipeName() const {
    wchar_t configured[256]{};
    const DWORD length = GetEnvironmentVariableW(L"OTC_API_PIPE_NAME", configured,
                                                  static_cast<DWORD>(std::size(configured)));
    if (length > 0 && length < std::size(configured)) {
        std::wstring value(configured, length);
        if (value.rfind(L"\\\\.\\pipe\\", 0) == 0)
            return value;
        return L"\\\\.\\pipe\\" + value;
    }

    wchar_t mappingName[128]{};
    _snwprintf_s(mappingName, _countof(mappingName), _TRUNCATE,
                 L"Local\\OTCApiPipe_%lu", GetCurrentProcessId());
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, mappingName);
    if (mapping) {
        const wchar_t* mapped = static_cast<const wchar_t*>(
            MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 512));
        if (mapped && mapped[0]) {
            std::wstring value(mapped);
            UnmapViewOfFile(mapped);
            CloseHandle(mapping);
            if (value.rfind(L"\\\\.\\pipe\\", 0) == 0)
                return value;
            return L"\\\\.\\pipe\\" + value;
        }
        if (mapped)
            UnmapViewOfFile(mapped);
        CloseHandle(mapping);
    }

    // Compatibility default: existing consumers continue to use \\.\pipe\OTCApi.
    // Set OTC_API_PIPE_NAME=OTCApi_<game-pid> before injecting to isolate a client.
    return kPipeName;
}

bool PipeServer::emit(const char* event, const char* dataObjectJson) {
    return writeLine(makeEventLine(event, dataObjectJson));
}

bool PipeServer::tryEmit(const char* event, const char* dataObjectJson) {
    return emit(event, dataObjectJson);
}

bool PipeServer::tryEmit(const char* event, const std::string& dataObjectJson) {
    return emit(event, dataObjectJson);
}

bool PipeServer::writeLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(writeMtx_);
    if (pipe_ == INVALID_HANDLE_VALUE)
        return false;

    DWORD written = 0;
    const BOOL ok = WriteFile(pipe_, line.data(), static_cast<DWORD>(line.size()),
                              &written, nullptr);
    if (!ok || written != line.size()) {
        LOG("pipe write failed err=%lu", GetLastError());
        return false;
    }
    return true;
}

void PipeServer::acceptLoop() {
    while (running_.load()) {
        HANDLE pipe = CreateNamedPipeW(
            pipeName_.c_str(),
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1,
            kBufSize,
            kBufSize,
            0,
            nullptr);

        if (pipe == INVALID_HANDLE_VALUE) {
            LOG("CreateNamedPipe failed err=%lu", GetLastError());
            Sleep(200);
            continue;
        }

        const BOOL connected = ConnectNamedPipe(pipe, nullptr)
            ? TRUE
            : (GetLastError() == ERROR_PIPE_CONNECTED);

        if (!running_.load()) {
            disconnectPipe(pipe);
            break;
        }

        if (!connected) {
            LOG("ConnectNamedPipe failed err=%lu", GetLastError());
            CloseHandle(pipe);
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(writeMtx_);
            pipe_ = pipe;
        }

        LOG("pipe client connected");
        emit("dllStatus",
             "{\"phase\":\"connected\",\"message\":\"Pipe client connected\"}");
        ConnectHandler onConnect;
        {
            std::lock_guard<std::mutex> lock(handlerMtx_);
            onConnect = connectHandler_;
        }
        if (onConnect)
            onConnect();
        clientLoop(pipe);

        {
            std::lock_guard<std::mutex> lock(writeMtx_);
            if (pipe_ == pipe)
                pipe_ = INVALID_HANDLE_VALUE;
        }
        disconnectPipe(pipe);
        LOG("pipe client disconnected");
    }
}

void PipeServer::clientLoop(HANDLE pipe) {
    std::string acc;
    char chunk[4096];

    while (running_.load()) {
        DWORD avail = 0;
        DWORD bytesLeft = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, &bytesLeft)) {
            break;
        }

        if (avail == 0) {
            Sleep(15);
            continue;
        }

        DWORD got = 0;
        const DWORD toRead = avail > sizeof(chunk) ? sizeof(chunk) : avail;
        if (!ReadFile(pipe, chunk, toRead, &got, nullptr) || got == 0)
            break;

        acc.append(chunk, chunk + got);

        std::size_t nl;
        while ((nl = acc.find('\n')) != std::string::npos) {
            std::string line = acc.substr(0, nl);
            acc.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty())
                continue;

            CommandHandler handler;
            {
                std::lock_guard<std::mutex> lock(handlerMtx_);
                handler = commandHandler_;
            }
            if (handler)
                handler(line);
        }
    }
}

} // namespace otc

