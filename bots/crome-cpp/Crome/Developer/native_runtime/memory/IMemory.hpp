#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <type_traits>

namespace crome {

class IMemory {
public:
    virtual ~IMemory() = default;
    virtual bool readBytes(std::uintptr_t address, void* out, std::size_t size) const = 0;

    template <typename T>
    std::optional<T> read(std::uintptr_t address) const {
        static_assert(std::is_trivially_copyable_v<T>);
        T value{};
        if (!readBytes(address, &value, sizeof(T))) return std::nullopt;
        return value;
    }

    std::optional<std::string> readCString(std::uintptr_t address, std::size_t maxLen = 64) const {
        std::string s;
        s.reserve(maxLen);
        for (std::size_t i = 0; i < maxLen; ++i) {
            auto ch = read<char>(address + i);
            if (!ch) return std::nullopt;
            if (*ch == '\0') return s;
            s.push_back(*ch);
        }
        return s;
    }
};

} // namespace crome
