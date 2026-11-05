#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace crome {
struct Position { int32_t x{}, y{}; int16_t z{}; bool operator==(const Position&) const = default; };
struct ItemSnapshot { uint32_t id{}; uint32_t count{}; uint32_t subtype{}; uintptr_t address{}; };
struct PlayerSnapshot {
    bool valid{}; uintptr_t address{}; uint32_t id{}; std::string name; Position position;
    double health{}, maxHealth{}, mana{}, maxMana{}, capacity{}, experience{}, level{}, magicLevel{}, soul{};
    uint64_t states{}; uint32_t targetId{}; int direction{};
    double healthPercent() const { return maxHealth > 0 ? health * 100.0 / maxHealth : 0.0; }
    double manaPercent() const { return maxMana > 0 ? mana * 100.0 / maxMana : 0.0; }
};
struct CreatureSnapshot {
    bool valid{}; uintptr_t address{}; uint32_t id{}; std::string name; Position position;
    int hpPercent{}; int direction{}; int skull{}; int party{}; bool alive{true}; bool attacking{};
    bool inViewport{}; int distance{};
};
struct ContainerSnapshot { int index{-1}; uint32_t id{}; std::string name; int capacity{}; std::vector<ItemSnapshot> items; };
enum class ThingKind { Unknown, Ground, Item, Creature, Monster, Player, Npc, LocalPlayer };
struct ThingSnapshot { ThingKind kind{ThingKind::Unknown}; uintptr_t address{}; uint32_t id{}; };
struct TileSnapshot { bool valid{}; bool walkable{}; Position position; std::vector<ThingSnapshot> things; };
struct GameState { PlayerSnapshot player; std::vector<CreatureSnapshot> creatures; std::vector<ContainerSnapshot> containers; };
}
