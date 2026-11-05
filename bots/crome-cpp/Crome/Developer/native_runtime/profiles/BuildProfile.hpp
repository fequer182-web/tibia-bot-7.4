#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace crome {

enum class StatStorage {
    UInt64,
    Double,
};

enum class SmallFieldStorage {
    UInt8,
    UInt16,
    UInt32,
};

struct CreatureOffsets {
    std::size_t x{};
    std::size_t y{};
    std::size_t z{};
    std::size_t id{};
    std::size_t name{};
    std::size_t hpPercent{};
    std::size_t direction{};
    std::optional<std::size_t> aliveRemoved{};
    std::optional<std::size_t> skull{};
    std::optional<std::size_t> party{};
};

struct LocalPlayerOffsets {
    std::size_t states{};
    std::size_t health{};
    std::size_t maxHealth{};
    std::size_t capacity{};
    std::size_t experience{};
    std::size_t level{};
    std::size_t mana{};
    std::size_t maxMana{};
    std::size_t magicLevel{};
};

struct RuntimeRvas {
    std::optional<std::uintptr_t> gameObject;
    std::optional<std::uintptr_t> localPlayerPtr;
    std::optional<std::uintptr_t> localPlayerVtable;
    std::size_t localPlayerSlotOffset{};
    std::optional<std::uintptr_t> localPlayerSlotTarget;
    std::optional<std::uintptr_t> gameAutoWalk;
    // Validated V15 map primitives. These are RVAs because they move with the module.
    std::optional<std::uintptr_t> mapObject;
    std::optional<std::uintptr_t> mapEmptySlot;
    std::optional<std::uintptr_t> mapGetTile;
    std::optional<std::uintptr_t> monsterVtable;
};

struct MapOffsets {
    std::size_t tileThingsBegin{0x48};
    std::size_t tileThingsEnd{0x50};
    std::size_t tileThingsCapacity{0x58};
};

struct ContainerOffsets {
    std::size_t mapFromGame{};
    std::size_t mapHeader{};
    std::size_t nodeLeft{};
    std::size_t nodeParent{};
    std::size_t nodeRight{};
    std::size_t nodeIsNil{};
    std::size_t nodeContainer{};
    std::size_t containerId{};
    std::size_t dequeMap{};
    std::size_t dequeMapSize{};
    std::size_t dequeOffset{};
    std::size_t itemCount{};
    std::size_t itemId{};
    std::size_t itemSubtype{};
};

struct InventoryOffsets {
    std::size_t base{};
    std::size_t slotStride{sizeof(std::uintptr_t)};
    std::size_t slotCount{10};
};

struct ActionRvas {
    std::optional<std::uintptr_t> attack;
    std::optional<std::uintptr_t> follow;
    std::optional<std::uintptr_t> walk;
    std::optional<std::uintptr_t> look;
    std::optional<std::uintptr_t> use;
    std::optional<std::uintptr_t> useWith;
    std::optional<std::uintptr_t> move;
    std::optional<std::uintptr_t> open;
    std::optional<std::uintptr_t> close;
    std::optional<std::uintptr_t> castSpell;
    std::optional<std::uintptr_t> sayDefault;
};

struct BuildProfile {
    std::string_view name;
    std::string_view executable;
    bool x64{};
    StatStorage localPlayerStats{StatStorage::UInt64};
    SmallFieldStorage localPlayerStates{SmallFieldStorage::UInt8};
    SmallFieldStorage creatureHpPercent{SmallFieldStorage::UInt8};
    SmallFieldStorage creatureDirection{SmallFieldStorage::UInt8};
    CreatureOffsets creature;
    LocalPlayerOffsets localPlayer;
    ContainerOffsets container;
    InventoryOffsets inventory;
    MapOffsets map;
    RuntimeRvas runtime;
    ActionRvas actions;
};

} // namespace crome
