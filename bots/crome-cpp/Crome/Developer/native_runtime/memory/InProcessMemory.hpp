#pragma once
#include "IMemory.hpp"
#include <cstring>

namespace crome {

// Intended for an injected DLL. On MSVC/Windows, SEH prevents an invalid game
// pointer from taking the bot down during a diagnostic read.
class InProcessMemory final : public IMemory {
public:
    bool readBytes(std::uintptr_t address, void* out, std::size_t size) const override {
        if (!address || !out || !size) return false;
#if defined(_MSC_VER) && defined(_WIN32)
        __try {
            std::memcpy(out, reinterpret_cast<const void*>(address), size);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
#else
        std::memcpy(out, reinterpret_cast<const void*>(address), size);
        return true;
#endif
    }
};

} // namespace crome
