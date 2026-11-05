#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace otc {

struct Position {
    int x = 0;
    int y = 0;
    int z = 0;
};

struct ItemSlot {
    int slot = 0; // 1..10
    int id = 0;
    int count = 0;
};

struct PlayerSnapshot {
    bool valid = false;
    int health = 0;
    int maxHealth = 0;
    int mana = 0;
    int maxMana = 0;
    int level = 0;
    int levelPercent = 0;
    std::uint64_t experience = 0;
    int capacity = 0;
    int totalCapacity = 0;
    int magicLevel = 0;
    int soul = 0;
    std::uint32_t stateFlags = 0;
    std::uint32_t targetCreatureId = 0;
    Position position;
    bool hasPosition = false;
    std::string name;
    std::string direction;
    ItemSlot inventory[10]{};
    bool hasInventory = false;
};

struct ContainerSnapshot {
    int index = 0;
    std::string name;
    int capacity = 0;
    std::vector<ItemSlot> items;
};

struct CreatureSnapshot {
    std::uint32_t id = 0;
    std::string name;
    int x = 0;
    int y = 0;
    int z = 0;
    int hpperc = 0;
    std::string kind;
    int skull = 0;
    bool inViewport = false;
    int distance = -1;
};

struct MapItem {
    int id = 0;
    int count = 0;
};

struct TileSnapshot {
    int x = 0;
    int y = 0;
    int z = 0;
    std::vector<MapItem> items;
};

enum class StatType { Double, Int32, Float };

} // namespace otc

