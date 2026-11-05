#pragma once
#include "IMemory.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace crome {

// Read-only memory backend for a separate Windows process.
// This is intended for diagnostics before any native action is enabled.
class ExternalProcessMemory final : public IMemory {
public:
#if defined(_WIN32)
    explicit ExternalProcessMemory(HANDLE process) noexcept : process_(process) {}

    bool readBytes(std::uintptr_t address, void* out, std::size_t size) const override {
        if (!process_ || !address || !out || !size) return false;
        SIZE_T bytesRead = 0;
        return ::ReadProcessMemory(
                   process_,
                   reinterpret_cast<LPCVOID>(address),
                   out,
                   size,
                   &bytesRead) != FALSE && bytesRead == size;
    }

    [[nodiscard]] HANDLE nativeHandle() const noexcept { return process_; }
#else
    explicit ExternalProcessMemory(void*) noexcept {}
    bool readBytes(std::uintptr_t, void*, std::size_t) const override { return false; }
#endif

private:
#if defined(_WIN32)
    HANDLE process_{};
#endif
};

} // namespace crome
