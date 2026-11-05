#pragma once
#include "../memory/IMemory.hpp"
#include "../profiles/BuildProfile.hpp"
#include "Snapshots.hpp"
#include <optional>

namespace crome {

inline std::optional<std::uint32_t> readSmallUnsigned(const IMemory& mem, std::uintptr_t address, SmallFieldStorage type) {
    switch (type) {
        case SmallFieldStorage::UInt8:  if (auto v = mem.read<std::uint8_t>(address)) return *v; break;
        case SmallFieldStorage::UInt16: if (auto v = mem.read<std::uint16_t>(address)) return *v; break;
        case SmallFieldStorage::UInt32: if (auto v = mem.read<std::uint32_t>(address)) return *v; break;
    }
    return std::nullopt;
}

class CreatureReader {
public:
    CreatureReader(const IMemory& mem, const BuildProfile& profile) : mem_(mem), profile_(profile) {}

    std::optional<CreatureSnapshot> read(std::uintptr_t base) const {
        if (!base) return std::nullopt;
        const auto& o = profile_.creature;
        CreatureSnapshot s{};
        s.address = base;
        auto x = mem_.read<std::int32_t>(base + o.x);
        auto y = mem_.read<std::int32_t>(base + o.y);
        auto z = mem_.read<std::int32_t>(base + o.z);
        auto id = mem_.read<std::uint32_t>(base + o.id);
        auto hp = readSmallUnsigned(mem_, base + o.hpPercent, profile_.creatureHpPercent);
        auto dir = readSmallUnsigned(mem_, base + o.direction, profile_.creatureDirection);
        if (!x || !y || !z || !id || !hp || !dir) return std::nullopt;
        s.position = {*x, *y, *z};
        s.id = *id;
        s.hpPercent = *hp;
        s.direction = *dir;
        if (o.aliveRemoved) if (auto v = mem_.read<std::uint8_t>(base + *o.aliveRemoved)) s.alive = (*v == 0);
        if (o.skull) if (auto v = mem_.read<std::uint8_t>(base + *o.skull)) s.skull = *v;
        if (o.party) if (auto v = mem_.read<std::uint8_t>(base + *o.party)) s.party = *v;
        return s;
    }
private:
    const IMemory& mem_;
    const BuildProfile& profile_;
};

class LocalPlayerReader {
public:
    LocalPlayerReader(const IMemory& mem, const BuildProfile& profile) : mem_(mem), profile_(profile) {}

    std::optional<LocalPlayerSnapshot> read(std::uintptr_t base) const {
        if (!base) return std::nullopt;
        const auto& o = profile_.localPlayer;
        LocalPlayerSnapshot s{};
        s.address = base;
        auto states = readSmallUnsigned(mem_, base + o.states, profile_.localPlayerStates);
        if (!states) return std::nullopt;
        s.states = *states;

        if (profile_.localPlayerStats == StatStorage::Double) {
            auto hp = mem_.read<double>(base + o.health);
            auto maxHp = mem_.read<double>(base + o.maxHealth);
            auto cap = mem_.read<double>(base + o.capacity);
            auto exp = mem_.read<double>(base + o.experience);
            auto lvl = mem_.read<double>(base + o.level);
            auto mana = mem_.read<double>(base + o.mana);
            auto maxMana = mem_.read<double>(base + o.maxMana);
            auto ml = mem_.read<double>(base + o.magicLevel);
            if (!hp || !maxHp || !cap || !exp || !lvl || !mana || !maxMana || !ml) return std::nullopt;
            s.health=*hp; s.maxHealth=*maxHp; s.capacity=*cap; s.experience=*exp;
            s.level=*lvl; s.mana=*mana; s.maxMana=*maxMana; s.magicLevel=*ml;
        } else {
            auto hp = mem_.read<std::uint64_t>(base + o.health);
            auto maxHp = mem_.read<std::uint64_t>(base + o.maxHealth);
            auto cap = mem_.read<std::uint64_t>(base + o.capacity);
            auto exp = mem_.read<std::uint64_t>(base + o.experience);
            auto lvl = mem_.read<std::uint64_t>(base + o.level);
            auto mana = mem_.read<std::uint64_t>(base + o.mana);
            auto maxMana = mem_.read<std::uint64_t>(base + o.maxMana);
            auto ml = mem_.read<std::uint64_t>(base + o.magicLevel);
            if (!hp || !maxHp || !cap || !exp || !lvl || !mana || !maxMana || !ml) return std::nullopt;
            s.health=static_cast<double>(*hp); s.maxHealth=static_cast<double>(*maxHp);
            s.capacity=static_cast<double>(*cap); s.experience=static_cast<double>(*exp);
            s.level=static_cast<double>(*lvl); s.mana=static_cast<double>(*mana);
            s.maxMana=static_cast<double>(*maxMana); s.magicLevel=static_cast<double>(*ml);
        }
        return s;
    }
private:
    const IMemory& mem_;
    const BuildProfile& profile_;
};

} // namespace crome
