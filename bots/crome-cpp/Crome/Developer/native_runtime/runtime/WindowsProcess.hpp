#pragma once

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <cstdint>
#include <cwchar>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace crome::win {

struct ProcessInfo {
    DWORD pid{};
    std::wstring executable;
    std::uintptr_t moduleBase{};
    std::size_t moduleSize{};
};

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE h) noexcept : handle_(h) {}
    ~UniqueHandle() { reset(); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.release()) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }
    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept { return handle_ && handle_ != INVALID_HANDLE_VALUE; }
    HANDLE release() noexcept { HANDLE h = handle_; handle_ = nullptr; return h; }
    void reset(HANDLE h = nullptr) noexcept {
        if (handle_ && handle_ != INVALID_HANDLE_VALUE) ::CloseHandle(handle_);
        handle_ = h;
    }
private:
    HANDLE handle_{};
};

inline std::wstring widenAscii(std::string_view s) {
    return std::wstring(s.begin(), s.end());
}

inline std::optional<DWORD> findProcessId(std::wstring_view executable) {
    UniqueHandle snapshot(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) return std::nullopt;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!::Process32FirstW(snapshot.get(), &entry)) return std::nullopt;
    const std::wstring target(executable);
    do {
        if (_wcsicmp(entry.szExeFile, target.c_str()) == 0) return entry.th32ProcessID;
    } while (::Process32NextW(snapshot.get(), &entry));
    return std::nullopt;
}

inline std::optional<ProcessInfo> resolveMainModule(DWORD pid, std::wstring_view executable) {
    UniqueHandle snapshot(::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
    if (!snapshot) return std::nullopt;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!::Module32FirstW(snapshot.get(), &entry)) return std::nullopt;
    const std::wstring target(executable);
    do {
        if (_wcsicmp(entry.szModule, target.c_str()) == 0) {
            return ProcessInfo{
                .pid = pid,
                .executable = entry.szModule,
                .moduleBase = reinterpret_cast<std::uintptr_t>(entry.modBaseAddr),
                .moduleSize = static_cast<std::size_t>(entry.modBaseSize),
            };
        }
    } while (::Module32NextW(snapshot.get(), &entry));
    return std::nullopt;
}

inline std::optional<ProcessInfo> findProcess(const std::vector<std::string>& executableNames) {
    for (const auto& name : executableNames) {
        const auto wide = widenAscii(name);
        if (auto pid = findProcessId(wide)) {
            if (auto info = resolveMainModule(*pid, wide)) return info;
        }
    }
    return std::nullopt;
}

inline std::optional<ProcessInfo> findOlders() {
    static const std::vector<std::string> candidates = {
        "Olders_OpenGL.exe", "olders_opengl.exe", "Olders_DX9.exe", "olders_dx9.exe"
    };
    return findProcess(candidates);
}

inline UniqueHandle openReadOnly(DWORD pid) {
    return UniqueHandle(::OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid));
}

} // namespace crome::win
#endif
