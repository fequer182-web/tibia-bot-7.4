#pragma once
#include <cstdint>
#include <cstdlib>

namespace crome {
struct Position {
    std::int32_t x{};
    std::int32_t y{};
    std::int32_t z{};

    [[nodiscard]] int chebyshevDistance(const Position& other) const noexcept {
        const int dx = std::abs(x - other.x);
        const int dy = std::abs(y - other.y);
        return dx > dy ? dx : dy;
    }
};
}
