#pragma once
#include "otc_api/types.h"
#include <algorithm>
#include <vector>

namespace otc {
struct MonsterEnd {
    CreatureSnapshot creature;
    bool deathObserved = false;
    bool lastSeenPosition = false;
};

// Independent of attack target and map items. Disappearance is not a kill.
struct MonsterEndTracker {
    bool initialized = false;
    int floor = -1;
    std::vector<CreatureSnapshot> previous;

    std::vector<MonsterEnd> observe(const PlayerSnapshot& player,
        const std::vector<CreatureSnapshot>& current, bool valid,
        const std::vector<std::uint32_t>& removedZeroHp = {}) {
        std::vector<MonsterEnd> events;
        if (!valid || !player.valid || !player.hasPosition) {
            initialized = false; previous.clear(); floor = -1;
            return events;
        }
        if (initialized && floor == player.position.z) {
            for (const auto& old : previous) {
                if (old.kind != "monster" || old.hpperc <= 0) continue;
                const auto it = std::find_if(current.begin(), current.end(),
                    [&](const auto& c) { return c.id == old.id; });
                if (it == current.end()) {
                    const bool zeroHp = std::find(removedZeroHp.begin(), removedZeroHp.end(), old.id) != removedZeroHp.end();
                    auto last = old;
                    if (zeroHp) last.hpperc = 0;
                    events.push_back({last, zeroHp, true});
                }
                else if (it->kind == "monster" && it->hpperc == 0)
                    events.push_back({*it, true});
            }
        }
        previous = current;
        floor = player.position.z;
        initialized = true;
        return events;
    }
};
} // namespace otc

