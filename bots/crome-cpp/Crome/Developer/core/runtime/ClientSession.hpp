#pragma once
#include "core/profile/ProfileRegistry.hpp"
#include "core/runtime/RuntimeContext.hpp"
#include "core/runtime/WindowsProcessMemory.hpp"
#include <filesystem>
#include <memory>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace crome {
struct ClientStatus {
    bool processFound{};
    bool profileMatched{};
    bool memoryOpen{};
    bool localPlayerResolved{};
    bool localPlayerVtableOk{};
    uint32_t pid{};
    uintptr_t moduleBase{};
    std::string exe;
    std::string profileName;
    std::string message;
};

class ClientSession {
    ProfileRegistry registry_;
    RuntimeContext runtime_{};
    WindowsProcessMemory memory_{};
    ClientStatus status_{};
#ifdef _WIN32
    HANDLE process_{};
#endif
    uint64_t lastRefreshMs_{};

    void closeProcess();
    bool resolveRuntime();
public:
    ClientSession() = default;
    ~ClientSession();
    bool loadProfiles(const std::filesystem::path& dir);
    bool refresh(bool force=false);
    const ClientStatus& status() const { return status_; }
    RuntimeContext& runtime() { return runtime_; }
    const RuntimeContext& runtime() const { return runtime_; }
    WindowsProcessMemory& memory() { return memory_; }
    const BuildProfile* profile() const { return runtime_.profile; }
};
}
