#pragma once
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include "core/runtime/IMemory.hpp"
#include <cstdint>

namespace crome {
class WindowsProcessMemory final : public IMemory {
#ifdef _WIN32
    HANDLE process_{};
#else
    void* process_{};
#endif
public:
    WindowsProcessMemory() = default;
#ifdef _WIN32
    explicit WindowsProcessMemory(HANDLE h) : process_(h) {}
    void set(HANDLE h) { process_ = h; }
    HANDLE handle() const { return process_; }
#else
    explicit WindowsProcessMemory(void* h) : process_(h) {}
    void set(void* h) { process_ = h; }
#endif
    bool read(uintptr_t address, void* out, size_t size) const override;
    bool write(uintptr_t address, const void* data, size_t size) override;
};
}
