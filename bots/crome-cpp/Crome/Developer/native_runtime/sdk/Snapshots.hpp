#pragma once
#include "Position.hpp"
#include <cstdint>
#include <string>

namespace crome {
struct CreatureSnapshot {
    std::uintptr_t address{};
    std::uint32_t id{};
    std::string name;
    Position position{};
    std::uint32_t hpPercent{};
    std::uint32_t direction{};
    bool alive{true};
    std::uint8_t skull{};
    std::uint8_t party{};
};

struct LocalPlayerSnapshot {
    std::uintptr_t address{};
    std::uint64_t states{};
    double health{};
    double maxHealth{};
    double capacity{};
    double experience{};
    double level{};
    double mana{};
    double maxMana{};
    double magicLevel{};

    [[nodiscard]] double healthPercent() const noexcept {
        return maxHealth != 0.0 ? 100.0 * health / maxHealth : 0.0;
    }
    [[nodiscard]] double manaPercent() const noexcept {
        return maxMana != 0.0 ? 100.0 * mana / maxMana : 0.0;
    }
};
}
