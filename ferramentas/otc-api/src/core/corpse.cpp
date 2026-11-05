#include "otc_api/corpse.h"
#include "otc_api/corpse_tracker.h"
#include "otc_api/monster_end.h"
#include "otc_api/actions.h"
#include "otc_api/creature_reader.h"
#include "otc_api/map_reader.h"
#include "otc_api/player_reader.h"
#include "otc_api/pipe_server.h"
#include <Windows.h>
#include <mutex>
#include <limits>

namespace otc {
namespace {
using nlohmann::json;
std::mutex mutex;
CorpseTracker tracker;
MonsterEndTracker monsterTracker;
json lastMonsterEnd = json::object();
std::uint64_t sequence = 0;
json state() {
    if (tracker.status.empty()) return json::object();
    json j = {{"status", tracker.status}, {"corpseId", tracker.token},
        {"creatureId", tracker.creature.id}, {"name", tracker.creature.name},
        {"identification", tracker.ids.empty() ? "automatic" : "filtered"},
        {"x", tracker.creature.x}, {"y", tracker.creature.y}, {"z", tracker.creature.z}};
    if (tracker.itemId) { j["itemId"] = tracker.itemId; j["evidence"] = tracker.evidence; }
    if (tracker.status == "expired") {
        j["reason"] = tracker.reason;
        j["diagnostic"] = tracker.diagnostic;
        j["baselineItems"] = json::array();
        j["observedItems"] = json::array();
        for (const auto& i : tracker.baseline) j["baselineItems"].push_back({{"id", i.id}, {"count", i.count}});
        for (const auto& i : tracker.observedItems) j["observedItems"].push_back({{"id", i.id}, {"count", i.count}});
    }
    return j;
}
json error(const char* message) { return {{"ok", false}, {"error", message}}; }
void emit() { PipeServer::instance().emit("corpse", state().dump()); }
bool positive(const json& j, std::uint64_t max) {
    if (!j.is_number_integer()) return false;
    if (j.is_number_unsigned()) return j.get<std::uint64_t>() > 0 && j.get<std::uint64_t>() <= max;
    const auto n = j.get<std::int64_t>();
    return n > 0 && static_cast<std::uint64_t>(n) <= max;
}
DWORD WINAPI poll(LPVOID) {
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            // Read map only for the legacy explicit corpse watcher.
            const auto tiles = tracker.status == "watching" ? currentMapTiles() : std::vector<TileSnapshot>{};
            const auto player = currentPlayer();
            std::vector<CreatureSnapshot> creatures;
            std::vector<std::uint32_t> removedZeroHp;
            const bool valid = player.valid && player.hasPosition && readFreshCreatures(creatures, &removedZeroHp);
            if (!valid || !monsterTracker.initialized || monsterTracker.floor != player.position.z)
                lastMonsterEnd = json::object();
            for (const auto& end : monsterTracker.observe(player, creatures, valid, removedZeroHp)) {
                const auto& c = end.creature;
                lastMonsterEnd = {{"creatureId", c.id}, {"name", c.name},
                    {"x", c.x}, {"y", c.y}, {"z", c.z}, {"hpperc", c.hpperc},
                    {"deathObserved", end.deathObserved},
                    {"positionSource", end.lastSeenPosition ? "last-seen" : "current"},
                    {"reason", end.deathObserved ? "zero-hp" : "disappeared"}};
                PipeServer::instance().emit("monsterEnd", lastMonsterEnd.dump());
            }
            if (!tracker.status.empty() && tracker.status != "expired") {
                const auto old = tracker.status;
                // Map first, then uncached creatures: never label a post-death
                // map sample as pre-death using the independently polled cache.
                tracker.observe(GetTickCount64(), player, creatures, tiles, valid);
                if (old != tracker.status) emit();
            }
        }
        Sleep(250);
    }
}
}
void startCorpseReader() {
    const HANDLE thread = CreateThread(nullptr, 0, poll, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
}
json corpseState() { std::lock_guard<std::mutex> lock(mutex); return state(); }
json monsterEndState() { std::lock_guard<std::mutex> lock(mutex); return lastMonsterEnd; }
json watchCorpse(const json& request) {
    if (request.contains("creatureId") && !positive(request["creatureId"], UINT32_MAX))
        return error("bad creatureId");
    if (request.contains("corpseItemIds") && (!request["corpseItemIds"].is_array() ||
        request["corpseItemIds"].empty() || request["corpseItemIds"].size() > 32))
        return error("bad corpseItemIds");
    std::vector<int> ids;
    if (request.contains("corpseItemIds")) for (const auto& id : request["corpseItemIds"]) {
        if (!positive(id, 65535)) return error("bad corpseItemIds");
        const int value = id.get<int>();
        if (std::find(ids.begin(), ids.end(), value) == ids.end()) ids.push_back(value);
    }
    std::lock_guard<std::mutex> lock(mutex);
    const auto player = currentPlayer();
    const auto tiles = currentMapTiles();
    std::vector<CreatureSnapshot> creatures;
    if (!readFreshCreatures(creatures)) return error("creatures unavailable");
    const auto id = request.contains("creatureId") ? request["creatureId"].get<std::uint32_t>() :
        player.targetCreatureId;
    const auto it = std::find_if(creatures.begin(), creatures.end(),
        [id](const auto& c) { return c.id == id; });
    if (!player.valid || !player.hasPosition || player.targetCreatureId != id ||
        it == creatures.end() || it->kind != "monster" || it->hpperc <= 0 ||
        it->z != player.position.z) return error("target not attackable");
    CorpseTracker next;
    next.creature = *it;
    const auto* tile = next.tile(tiles);
    if (!tile) return error("map tile unavailable");
    next.baseline = tile->items;
    next.hasBaseline = true;
    next.ids = std::move(ids);
    next.token = ++sequence;
    next.deadline = GetTickCount64() + 60000;
    next.status = "watching";
    tracker = std::move(next);
    emit();
    auto result = state(); result["ok"] = true; return result;
}
json openCorpse(const json& request) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!request.contains("corpseId") || !positive(request["corpseId"], UINT64_MAX) ||
        request["corpseId"].get<std::uint64_t>() != tracker.token ||
        GetTickCount64() >= tracker.deadline || tracker.status == "watching" ||
        tracker.status.empty() || tracker.status == "expired") return error("corpse unavailable");
    const auto player = currentPlayer();
    if (!player.valid || !player.hasPosition || player.position.z != tracker.creature.z)
        return error("player unavailable");
    if (tracker.status == "useRequested") { auto j = state(); j["ok"] = true; return j; }
    if (!tracker.present(currentMapTiles())) return error("corpse changed");
    const auto& c = tracker.creature;
    const int distance = std::max(std::abs(player.position.x - c.x), std::abs(player.position.y - c.y));
    ActionResult result{true, {}, "client"};
    if (distance > 1) {
        if (tracker.status != "walking") result = actionWalkTo(c.x, c.y, c.z);
        if (result.ok) tracker.status = "walking";
    } else {
        result = actionUseCorpse(c.x, c.y, c.z, tracker.itemId);
        if (result.ok) tracker.status = "useRequested";
    }
    if (!result.ok) return {{"ok", false}, {"error", result.error}};
    emit();
    auto j = state(); j["ok"] = true; j["via"] = result.via; return j;
}
} // namespace otc

