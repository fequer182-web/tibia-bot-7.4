#include "otc_api/corpse_tracker.h"
#include "otc_api/monster_end.h"
#include <cstdlib>
#include <iostream>
using namespace otc;
void check(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
int main() {
    PlayerSnapshot p; p.valid = p.hasPosition = true; p.position = {100, 100, 7};
    CreatureSnapshot c; c.id = 42; c.kind = "monster"; c.hpperc = 20;
    c.x = 101; c.y = 100; c.z = 7;
    TileSnapshot ground{101, 100, 7, {{100, 1}}};
    TileSnapshot body{101, 100, 7, {{100, 1}, {200, 1}}};
    auto fresh = [&]() {
        CorpseTracker t; t.creature = c; t.ids = {200, 201}; t.token = 1;
        t.status = "watching"; t.deadline = 60000;
        t.observe(100, p, {c}, {ground}); return t;
    };
    auto t = fresh();
    t.observe(350, p, {}, {ground});
    check(t.status == "watching", "disappearance alone is not a corpse");
    t.observe(600, p, {}, {body});
    check(t.status == "available" && t.itemId == 200 && t.creature.x == 101,
          "delayed new body should identify the last square");
    check(t.present({body}) && !t.present({ground}), "opening must revalidate the body");
    t.observe(60600, p, {}, {body});
    check(t.status == "expired", "body must expire");
    t = fresh(); t.observe(350, p, {c}, {body}); t.observe(600, p, {}, {body});
    check(t.status == "watching", "preexisting body must not be reused");
    t = fresh(); auto ambiguous = body; ambiguous.items.push_back({201, 1});
    t.observe(350, p, {}, {ambiguous});
    check(t.status == "watching", "multiple candidate IDs are ambiguous");
    t = fresh(); ambiguous = body; ambiguous.items.push_back({200, 1});
    t.observe(350, p, {}, {ambiguous});
    check(t.status == "watching", "multiple bodies of one ID are ambiguous");
    t = fresh(); auto dead = c; dead.hpperc = 0;
    t.observe(350, p, {dead}, {body});
    check(t.status == "available" && t.evidence == "target-zero-hp-new-item",
          "zero HP should be recorded as evidence");
    t = fresh(); dead.x++;
    t.observe(350, p, {dead}, {body});
    check(t.status == "watching", "new death square without baseline must fail closed");
    t = fresh(); t.observe(350, p, {}, {ground}); t.observe(2400, p, {}, {body});
    check(t.status == "expired", "late item must not resurrect an expired observation");
    t = fresh(); auto moved = c; moved.x = 102;
    t.observe(350, p, {moved}, {}); t.observe(600, p, {}, {body});
    check(t.status == "watching" && !t.hasBaseline, "missing map tile must not reuse baseline");
    t = fresh(); auto otherFloor = p; otherFloor.position.z = 8;
    t.observe(350, otherFloor, {}, {body});
    check(t.status == "expired", "floor change must invalidate tracking");
    t = fresh(); auto offline = p; offline.valid = false;
    t.observe(350, offline, {}, {body});
    check(t.status == "expired", "offline state must invalidate tracking");
    t = fresh(); t.observe(350, p, {}, {ground}); t.observe(600, p, {c}, {ground});
    check(t.missingSince == 0, "reappearing creature must cancel disappearance timer");
    auto automatic = [&]() { auto a = fresh(); a.ids.clear(); return a; };
    t = automatic(); t.observe(350, p, {}, {body});
    check(t.status == "watching", "automatic candidate needs a second observation");
    t.observe(600, p, {}, {body});
    check(t.status == "available" && t.itemId == 200, "automatic ID discovery");
    auto reject = [&](TileSnapshot tile, const char* message) {
        auto a = automatic(); a.observe(350, p, {}, {tile}); a.observe(600, p, {}, {tile});
        check(a.status == "watching", message);
    };
    reject(ground, "no new item");
    reject({101, 100, 7, {{999, 1}, {200, 1}}}, "ground replacement is ambiguous");
    reject({101, 100, 7, {{100, 2}, {200, 1}}}, "baseline count change is ambiguous");
    reject({101, 100, 7, {{100, 1}, {200, 1}, {201, 1}}}, "multiple new items are ambiguous");
    reject({101, 100, 7, {{100, 1}, {100, 1}}}, "duplicate existing ID must fail closed");
    reject({101, 100, 7, {{100, 1}, {200, 5}}}, "new stack must not be mistaken for corpse");
    t = automatic(); t.observe(350, p, {}, {body}); t.observe(600, p, {}, {ground});
    t.observe(850, p, {}, {body});
    check(t.status == "watching", "disappearing candidate resets persistence");
    t.observe(1100, p, {}, {body}); check(t.status == "available", "stable reappearance");
    t = automatic(); t.observe(350, p, {}, {body});
    auto otherBody = body; otherBody.items.back().id = 201;
    t.observe(600, p, {}, {otherBody});
    check(t.status == "watching", "changed candidate resets persistence");
    t.observe(850, p, {}, {otherBody});
    check(t.status == "available" && t.itemId == 201, "new candidate identity");
    t = automatic(); t.observe(350, p, {}, {body}); t.observe(600, p, {c}, {body});
    check(t.status == "watching", "live target cannot yield automatic corpse");
    t = automatic();
    t.observe(350, p, {}, {body}, false);
    check(!t.missingSince && t.baseline.size() == 1 && t.diagnostic == "creatures unavailable",
          "failed creature read must neither imply death nor replace baseline");
    t.observe(600, p, {}, {body}); t.observe(850, p, {}, {body});
    check(t.status == "available", "fresh post-map disappearance preserves pre-death baseline");
    t = automatic(); t.observe(350, p, {}, {ground}); t.observe(600, p, {}, {ground});
    t.observe(2400, p, {}, {ground});
    check(t.status == "expired" && t.reason == "detection timeout" && t.diagnostic == "no new item",
          "expiration explains unchanged tile");
    t = automatic(); t.observe(350, p, {}, {ambiguous}); t.observe(2400, p, {}, {ambiguous});
    check(t.reason == "detection timeout" && t.diagnostic == "ambiguous tile change",
          "expiration explains ambiguous tile");
    MonsterEndTracker ends;
    p.targetCreatureId = 0;
    check(ends.observe(p, {c}, true).empty(), "first sample is baseline without attack target");
    auto dying = c; dying.hpperc = 0; dying.x += 1;
    auto events = ends.observe(p, {dying}, true);
    check(events.size() == 1 && events[0].deathObserved && events[0].creature.x == dying.x,
          "zero HP event uses final observed coordinates");
    check(ends.observe(p, {}, true).empty(), "disappearance after zero HP must not duplicate event");
    ends.observe(p, {c}, true);
    events = ends.observe(p, {}, true);
    check(events.size() == 1 && !events[0].deathObserved && events[0].creature.x == c.x,
          "disappearance retains last coordinates without declaring death");
    ends.observe(p, {c}, true); check(ends.observe(p, {}, false).empty(), "read failure is not disappearance");
    check(ends.observe(p, {}, true).empty(), "recovery after read failure resets baseline");
    ends.observe(p, {c}, true); auto floorPlayer = p; floorPlayer.position.z++;
    check(ends.observe(floorPlayer, {}, true).empty(), "floor change is not disappearance");
    ends.observe(p, {c}, true); auto loggedOut = p; loggedOut.valid = false;
    check(ends.observe(loggedOut, {}, true).empty(), "logout is not disappearance");
    auto npc = c; npc.kind = "npc";
    ends.observe(p, {npc}, true); check(ends.observe(p, {}, true).empty(), "NPCs excluded");
    auto alreadyDead = c; alreadyDead.hpperc = 0;
    ends.observe(loggedOut, {}, true);
    check(ends.observe(p, {alreadyDead}, true).empty(), "initial zero HP is not a new death");
    MonsterEndTracker removed;
    auto sheep1 = c; sheep1.id = 1073765704; sheep1.x = 32061; sheep1.y = 32208;
    auto sheep2 = c; sheep2.id = 1073765705; sheep2.x = 32060; sheep2.y = 32207;
    removed.observe(p, {sheep1, sheep2}, true);
    events = removed.observe(p, {}, true, {sheep1.id, sheep2.id, 1073765702});
    check(events.size() == 2, "two removed zero-HP Sheep must both emit");
    check(events[0].deathObserved && events[0].lastSeenPosition && events[0].creature.hpperc == 0 &&
          events[0].creature.x == 32061 && events[0].creature.y == 32208,
          "removed death uses first Sheep last valid square");
    check(events[1].deathObserved && events[1].creature.x == 32060 && events[1].creature.y == 32207,
          "removed death uses second Sheep last valid square");
    check(removed.observe(p, {}, true, {sheep1.id, sheep2.id}).empty(),
          "retained dead objects must not repeat events");
    MonsterEndTracker initialRemoved;
    check(initialRemoved.observe(p, {}, true, {sheep1.id}).empty(),
          "old dead objects without a live baseline must not emit");
    removed.observe(p, {sheep1}, true);
    events = removed.observe(p, {}, true, {sheep2.id});
    check(events.size() == 1 && !events[0].deathObserved && events[0].lastSeenPosition,
          "another creature's zero HP must not confirm this disappearance");
    std::cout << "corpse tracker checks passed\n";
}

