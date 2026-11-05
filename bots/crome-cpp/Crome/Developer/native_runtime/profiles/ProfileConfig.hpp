#pragma once
#include "BuildProfile.hpp"
#include "MiniJson.hpp"

#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace crome::profiles {

struct RuntimeProfile {
    std::string id;
    std::string name;
    std::vector<std::string> executables;
    bool enabled{true};
    bool x64{true};
    std::optional<std::size_t> moduleSize;
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

    BuildProfile view() const {
        return BuildProfile{
            .name = name,
            .executable = executables.empty() ? std::string_view{} : std::string_view(executables.front()),
            .x64 = x64,
            .localPlayerStats = localPlayerStats,
            .localPlayerStates = localPlayerStates,
            .creatureHpPercent = creatureHpPercent,
            .creatureDirection = creatureDirection,
            .creature = creature,
            .localPlayer = localPlayer,
            .container = container,
            .inventory = inventory,
            .map = map,
            .runtime = runtime,
            .actions = actions,
        };
    }
};

struct ProfilesFile {
    std::string activeProfile{"auto"};
    std::vector<RuntimeProfile> profiles;
};

inline std::string readTextFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open profile JSON: " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

inline const json::Value& required(const json::Value& obj, const char* key) {
    const auto* v = obj.get(key);
    if (!v) throw std::runtime_error(std::string("missing JSON field: ") + key);
    return *v;
}

inline std::string getString(const json::Value& obj, const char* key, const std::string& fallback = {}) {
    const auto* v = obj.get(key);
    if (!v) return fallback;
    if (!v->isString()) throw std::runtime_error(std::string("JSON field must be string: ") + key);
    return v->string();
}

inline bool getBool(const json::Value& obj, const char* key, bool fallback) {
    const auto* v = obj.get(key);
    if (!v) return fallback;
    if (!v->isBool()) throw std::runtime_error(std::string("JSON field must be bool: ") + key);
    return v->boolean();
}

inline std::optional<std::uint64_t> parseFlexibleInteger(const json::Value* v) {
    if (!v || v->isNull()) return std::nullopt;
    if (v->isNumber()) return static_cast<std::uint64_t>(v->number());
    if (v->isString()) {
        const auto& s = v->string();
        if (s.empty()) return std::nullopt;
        std::size_t used = 0;
        const auto n = std::stoull(s, &used, 0);
        if (used != s.size()) throw std::runtime_error("invalid integer string: " + s);
        return static_cast<std::uint64_t>(n);
    }
    throw std::runtime_error("expected integer, hex string, or null");
}

inline std::size_t reqOffset(const json::Value& obj, const char* key) {
    const auto n = parseFlexibleInteger(obj.get(key));
    if (!n) throw std::runtime_error(std::string("missing offset: ") + key);
    return static_cast<std::size_t>(*n);
}

inline std::optional<std::size_t> optOffset(const json::Value& obj, const char* key) {
    auto n = parseFlexibleInteger(obj.get(key));
    if (!n) return std::nullopt;
    return static_cast<std::size_t>(*n);
}

inline StatStorage parseStatStorage(const std::string& s) {
    if (s == "double") return StatStorage::Double;
    if (s == "uint64") return StatStorage::UInt64;
    throw std::runtime_error("unsupported local_player_stats type: " + s);
}

inline SmallFieldStorage parseSmallFieldStorage(const std::string& s) {
    if (s == "uint8") return SmallFieldStorage::UInt8;
    if (s == "uint16") return SmallFieldStorage::UInt16;
    if (s == "uint32") return SmallFieldStorage::UInt32;
    throw std::runtime_error("unsupported small field type: " + s);
}

inline std::optional<std::uintptr_t> actionRva(const json::Value& actions, const char* key) {
    auto n = parseFlexibleInteger(actions.get(key));
    if (!n || *n == 0) return std::nullopt;
    return static_cast<std::uintptr_t>(*n);
}

inline ProfilesFile loadProfiles(const std::string& path = "profiles/builds.json") {
    const auto root = json::parse(readTextFile(path));
    if (!root.isObject()) throw std::runtime_error("profile JSON root must be object");

    ProfilesFile out;
    out.activeProfile = getString(root, "active_profile", "auto");
    const auto& profiles = required(root, "profiles");
    if (!profiles.isArray()) throw std::runtime_error("profiles must be an array");

    for (const auto& item : profiles.array()) {
        if (!item.isObject()) throw std::runtime_error("profile entry must be an object");
        RuntimeProfile p;
        p.id = getString(item, "id");
        p.name = getString(item, "name", p.id);
        p.enabled = getBool(item, "enabled", true);
        p.x64 = getBool(item, "x64", true);
        if (p.id.empty()) throw std::runtime_error("profile id cannot be empty");

        const auto& exes = required(item, "executables");
        if (!exes.isArray()) throw std::runtime_error("executables must be an array");
        for (const auto& e : exes.array()) {
            if (!e.isString()) throw std::runtime_error("executable name must be string");
            p.executables.push_back(e.string());
        }
        if (p.executables.empty()) throw std::runtime_error("profile must contain at least one executable");

        if (auto n = parseFlexibleInteger(item.get("module_size"))) p.moduleSize = static_cast<std::size_t>(*n);

        const auto& types = required(item, "types");
        p.localPlayerStats = parseStatStorage(getString(types, "local_player_stats", "uint64"));
        p.localPlayerStates = parseSmallFieldStorage(getString(types, "local_player_states", "uint8"));
        p.creatureHpPercent = parseSmallFieldStorage(getString(types, "creature_hp_percent", "uint8"));
        p.creatureDirection = parseSmallFieldStorage(getString(types, "creature_direction", "uint8"));

        const auto& offsets = required(item, "offsets");
        const auto& c = required(offsets, "creature");
        const auto& l = required(offsets, "local_player");
        p.creature = CreatureOffsets{
            .x = reqOffset(c, "x"), .y = reqOffset(c, "y"), .z = reqOffset(c, "z"),
            .id = reqOffset(c, "id"), .name = reqOffset(c, "name"),
            .hpPercent = reqOffset(c, "hp_percent"), .direction = reqOffset(c, "direction"),
            .aliveRemoved = optOffset(c, "alive_removed"), .skull = optOffset(c, "skull"), .party = optOffset(c, "party"),
        };
        p.localPlayer = LocalPlayerOffsets{
            .states = reqOffset(l, "states"), .health = reqOffset(l, "health"),
            .maxHealth = reqOffset(l, "max_health"), .capacity = reqOffset(l, "capacity"),
            .experience = reqOffset(l, "experience"), .level = reqOffset(l, "level"),
            .mana = reqOffset(l, "mana"), .maxMana = reqOffset(l, "max_mana"),
            .magicLevel = reqOffset(l, "magic_level"),
        };

        if (const auto* co = offsets.get("container"); co && co->isObject()) {
            p.container = ContainerOffsets{
                .mapFromGame = reqOffset(*co, "map_from_game"),
                .mapHeader = reqOffset(*co, "map_header"),
                .nodeLeft = reqOffset(*co, "node_left"),
                .nodeParent = reqOffset(*co, "node_parent"),
                .nodeRight = reqOffset(*co, "node_right"),
                .nodeIsNil = reqOffset(*co, "node_is_nil"),
                .nodeContainer = reqOffset(*co, "node_container"),
                .containerId = reqOffset(*co, "container_id"),
                .dequeMap = reqOffset(*co, "deque_map"),
                .dequeMapSize = reqOffset(*co, "deque_map_size"),
                .dequeOffset = reqOffset(*co, "deque_offset"),
                .itemCount = reqOffset(*co, "item_count"),
                .itemId = reqOffset(*co, "item_id"),
                .itemSubtype = reqOffset(*co, "item_subtype"),
            };
        }

        if (const auto* io = offsets.get("inventory"); io && io->isObject()) {
            if (auto n = parseFlexibleInteger(io->get("base"))) p.inventory.base = static_cast<std::size_t>(*n);
            if (auto n = parseFlexibleInteger(io->get("slot_stride"))) p.inventory.slotStride = static_cast<std::size_t>(*n);
            if (auto n = parseFlexibleInteger(io->get("slot_count"))) p.inventory.slotCount = static_cast<std::size_t>(*n);
        }

        if (const auto* mo = offsets.get("map"); mo && mo->isObject()) {
            p.map = MapOffsets{
                .tileThingsBegin = reqOffset(*mo, "tile_things_begin"),
                .tileThingsEnd = reqOffset(*mo, "tile_things_end"),
                .tileThingsCapacity = reqOffset(*mo, "tile_things_capacity"),
            };
        }

        if (const auto* r = item.get("runtime"); r && r->isObject()) {
            p.runtime.gameObject = actionRva(*r, "game_object_rva");
            p.runtime.localPlayerPtr = actionRva(*r, "local_player_ptr_rva");
            p.runtime.localPlayerVtable = actionRva(*r, "local_player_vtable_rva");
            if (auto n = parseFlexibleInteger(r->get("local_player_slot_offset")))
                p.runtime.localPlayerSlotOffset = static_cast<std::size_t>(*n);
            p.runtime.localPlayerSlotTarget = actionRva(*r, "local_player_slot_target_rva");
            p.runtime.gameAutoWalk = actionRva(*r, "game_autowalk_rva");
            p.runtime.mapObject = actionRva(*r, "map_object_rva");
            p.runtime.mapEmptySlot = actionRva(*r, "map_empty_slot_rva");
            p.runtime.mapGetTile = actionRva(*r, "map_get_tile_rva");
            p.runtime.monsterVtable = actionRva(*r, "monster_vtable_rva");
        }

        if (const auto* a = item.get("actions"); a && a->isObject()) {
            p.actions.attack = actionRva(*a, "attack");
            p.actions.follow = actionRva(*a, "follow");
            p.actions.walk = actionRva(*a, "walk");
            p.actions.look = actionRva(*a, "look");
            p.actions.use = actionRva(*a, "use");
            p.actions.useWith = actionRva(*a, "use_with");
            p.actions.move = actionRva(*a, "move");
            p.actions.open = actionRva(*a, "open");
            p.actions.close = actionRva(*a, "close");
            p.actions.castSpell = actionRva(*a, "cast_spell");
            p.actions.sayDefault = actionRva(*a, "say_default");
        }
        out.profiles.push_back(std::move(p));
    }
    return out;
}

inline const RuntimeProfile* findProfileById(const ProfilesFile& file, const std::string& id) {
    for (const auto& p : file.profiles) if (p.id == id) return &p;
    return nullptr;
}

} // namespace crome::profiles
