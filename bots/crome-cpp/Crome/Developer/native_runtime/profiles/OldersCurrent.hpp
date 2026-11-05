#pragma once
#include "BuildProfile.hpp"

namespace crome::profiles {

// Compile-time fallback profile. The tools in v4 prefer profiles/builds.json.
// Runtime scan on 25/09/2026 validated LocalPlayer stat storage as double.
inline constexpr BuildProfile OldersCurrent{
    .name = "Olders current profile",
    .executable = "Olders_OpenGL.exe",
    .x64 = true,
    .localPlayerStats = StatStorage::Double,
    .localPlayerStates = SmallFieldStorage::UInt8,
    .creatureHpPercent = SmallFieldStorage::UInt8,
    .creatureDirection = SmallFieldStorage::UInt8,
    .creature = {
        .x = 0x18,
        .y = 0x1C,
        .z = 0x20,
        .id = 0x38,
        .name = 0x40,
        .hpPercent = 0x60,
        .direction = 0x64,
        .aliveRemoved = 0x1A6,
        .skull = 0xFC,
        .party = 0xFD,
    },
    .localPlayer = {
        .states = 0x648,
        .health = 0x658,
        .maxHealth = 0x660,
        .capacity = 0x668,
        .experience = 0x678,
        .level = 0x680,
        .mana = 0x690,
        .maxMana = 0x698,
        .magicLevel = 0x6A0,
    },
    .map = {
        .tileThingsBegin = 0x48,
        .tileThingsEnd = 0x50,
        .tileThingsCapacity = 0x58,
    },
    .runtime = {
        .localPlayerPtr = 0x0169A680,
        .localPlayerVtable = 0x011DA490,
        .localPlayerSlotOffset = 0xA8,
        .localPlayerSlotTarget = 0x00181C40,
        .gameAutoWalk = 0x00162200,
        .mapObject = 0x0169A860,
        .mapEmptySlot = 0x0169A850,
        .mapGetTile = 0x0022D9A0,
        .monsterVtable = 0x011E6460,
    },
    .actions = {
        .attack = 0x00165410,
        .follow = 0x001656E0,
        .walk = 0x001805C0,
        .look = 0x00163790,
        .use = 0x00164570,
        .useWith = 0x00164A50,
        .move = 0x00163E20,
        .castSpell = 0x00165D40,
        .sayDefault = 0x00165D40,
    }
};

} // namespace crome::profiles
