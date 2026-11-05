#pragma once
#include <cstdint>

namespace crome {
enum class Condition : std::uint64_t {
    ManaShield = 0x10,
    Paralyzed  = 0x20,
    Haste      = 0x40,
    Unknown80  = 0x80,
};

inline bool hasCondition(std::uint64_t states, Condition c) noexcept {
    return (states & static_cast<std::uint64_t>(c)) != 0;
}
}
