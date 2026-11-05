#pragma once

#include "otc_api/types.h"
#include <algorithm>
#include <cstdint>
#include <vector>

namespace otc {

// Pure snapshot correlation; never equates disappearance alone with a kill.
struct CorpseTracker {
    std::uint64_t token = 0, deadline = 0, missingSince = 0;
    CreatureSnapshot creature;
    std::vector<int> ids;
    std::vector<MapItem> baseline;
    std::vector<MapItem> observedItems;
    bool hasBaseline = false, zeroHp = false;
    int itemId = 0;
    int pendingItemId = 0;
    std::uint64_t pendingSince = 0;
    std::string status, evidence;
    std::string reason, diagnostic;

    const TileSnapshot* tile(const std::vector<TileSnapshot>& tiles) const {
        for (const auto& t : tiles)
            if (t.x == creature.x && t.y == creature.y && t.z == creature.z)
                return &t;
        return nullptr;
    }
    static int occurrences(const std::vector<MapItem>& items, int id) {
        return static_cast<int>(std::count_if(items.begin(), items.end(),
            [id](const auto& i) { return i.id == id; }));
    }
    bool present(const std::vector<TileSnapshot>& tiles) const {
        const auto* t = tile(tiles);
        return t && itemId && occurrences(t->items, itemId) == 1;
    }
    int automaticCandidate(const std::vector<MapItem>& items) const {
        if (baseline.empty() || items.size() != baseline.size() + 1) return 0;
        std::size_t old = 0;
        int candidate = 0;
        for (const auto& item : items) {
            if (old < baseline.size() && item.id == baseline[old].id &&
                item.count == baseline[old].count) {
                ++old;
                continue;
            }
            if (candidate || item.id <= 0 || item.id > 65535 || item.count < 0 ||
                item.count > 1 || occurrences(baseline, item.id) != 0) return 0;
            candidate = item.id;
        }
        return old == baseline.size() ? candidate : 0;
    }
    void observe(std::uint64_t now, const PlayerSnapshot& player,
                 const std::vector<CreatureSnapshot>& creatures,
                 const std::vector<TileSnapshot>& tiles, bool creaturesValid = true) {
        if (status.empty() || status == "expired") return;
        if (!player.valid || !player.hasPosition || player.position.z != creature.z ||
            now >= deadline) {
            reason = !player.valid || !player.hasPosition ? "player unavailable" :
                player.position.z != creature.z ? "floor changed" :
                status == "watching" ? "watch timeout" : "corpse timeout";
            status = "expired";
            return;
        }
        if (status != "watching") return;
        if (!creaturesValid) {
            diagnostic = "creatures unavailable";
            pendingItemId = 0;
            return;
        }
        const auto it = std::find_if(creatures.begin(), creatures.end(),
            [&](const auto& c) { return c.id == creature.id; });
        if (it != creatures.end() && it->hpperc > 0) {
            // Reappearance means this was not a completed death observation.
            missingSince = 0;
            zeroHp = false;
            pendingItemId = 0;
            creature = *it;
            const auto* t = tile(tiles);
            hasBaseline = t != nullptr;
            baseline = t ? t->items : std::vector<MapItem>{};
            observedItems = baseline;
            diagnostic = t ? "target alive" : "map tile unavailable";
            return;
        }
        if (!missingSince) {
            missingSince = now;
            zeroHp = it != creatures.end();
            if (zeroHp && (it->x != creature.x || it->y != creature.y || it->z != creature.z))
                hasBaseline = false;
        }
        if (now - missingSince > 2000) {
            reason = "detection timeout"; status = "expired"; return;
        }
        const auto* t = tile(tiles);
        observedItems = t ? t->items : std::vector<MapItem>{};
        if (!hasBaseline || !t) {
            diagnostic = !hasBaseline ? "baseline unavailable" : "map tile unavailable";
            pendingItemId = 0; return;
        }
        int candidate = 0, matches = 0;
        if (ids.empty()) {
            candidate = automaticCandidate(t->items);
            if (!candidate) {
                const bool same = baseline.size() == t->items.size() &&
                    std::equal(baseline.begin(), baseline.end(), t->items.begin(),
                        [](const auto& a, const auto& b) { return a.id == b.id && a.count == b.count; });
                diagnostic = same ? "no new item" : "ambiguous tile change";
                pendingItemId = 0; return;
            }
            if (pendingItemId != candidate) {
                pendingItemId = candidate;
                pendingSince = now;
                diagnostic = "candidate not stable";
                return;
            }
            if (now - pendingSince < 250) return;
            matches = 1;
        } else for (int id : ids) {
            if (occurrences(baseline, id) == 0 && occurrences(t->items, id) > 0) {
                matches += occurrences(t->items, id);
                candidate = id;
            }
        }
        if (matches != 1) { diagnostic = "no unique matching item"; return; }
        diagnostic.clear();
        itemId = candidate;
        status = "available";
        evidence = zeroHp ? "target-zero-hp-new-item" : "target-disappeared-new-item";
        deadline = now + 60000;
    }
};

} // namespace otc

