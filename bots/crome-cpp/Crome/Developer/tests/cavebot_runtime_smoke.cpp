#include "native_runtime/bot/Walker.hpp"

#include <cassert>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace {

bool same(crome::Position a, crome::Position b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

struct MockActions final : crome::IActions {
    std::vector<crome::Position> walks;
    std::vector<std::pair<std::uint32_t, crome::Position>> groundUses;
    std::vector<std::pair<std::uint32_t, crome::Position>> toolUses;

    bool attack(std::uint32_t) override { return true; }
    bool follow(std::uint32_t) override { return true; }
    bool walkTo(crome::Position p) override { walks.push_back(p); return true; }
    bool look(crome::Position) override { return true; }
    bool use(const crome::Item&) override { return true; }
    bool useWith(const crome::Item&, crome::Position) override { return true; }
    bool useWithGround(const crome::Item& item, crome::Position p) override {
        toolUses.emplace_back(item.id, p);
        return true;
    }
    bool useGroundObject(std::uint32_t id, crome::Position p) override {
        groundUses.emplace_back(id, p);
        return true;
    }
    bool move(const crome::Item&, crome::Position, crome::Position, std::uint32_t) override { return true; }
    bool open(crome::Position) override { return true; }
    bool close(std::size_t) override { return true; }
    bool castSpell(std::string_view) override { return true; }
    bool sayDefault(std::string_view) override { return true; }
};

void testUseTargetRoundTrip() {
    crome::RouteConfig route;
    route.loop = false;

    crome::RouteWaypoint w;
    w.type = crome::RouteWaypointType::Use;
    w.itemId = 1948;
    w.position = {100, 100, 7};
    w.hasPosition = true;
    route.waypoints.push_back(w);

    const char* path = "cavebot_runtime_smoke_route.json";
    assert(crome::saveRouteConfig(path, route));
    const auto loaded = crome::loadRouteConfig(path);
    std::remove(path);

    assert(loaded.waypoints.size() == 1);
    assert(loaded.waypoints[0].type == crome::RouteWaypointType::Use);
    assert(loaded.waypoints[0].hasPosition);
    assert(loaded.waypoints[0].itemId == 1948);
    assert(same(loaded.waypoints[0].position, {100, 100, 7}));
}

void testUseApproachesAndUsesGroundObject() {
    MockActions actions;
    crome::Walker walker(actions);

    crome::RouteConfig route;
    route.loop = false;
    route.actionWaitMs = 50;

    crome::RouteWaypoint w;
    w.type = crome::RouteWaypointType::Use;
    w.itemId = 1948;
    w.position = {100, 100, 7};
    w.hasPosition = true;
    route.waypoints.push_back(w);

    walker.setRoute(route);
    assert(walker.start(0, {95, 100, 7}));

    walker.tick(0, {95, 100, 7});
    assert(actions.walks.size() == 1);
    assert(same(actions.walks.back(), {99, 100, 7}));
    assert(actions.groundUses.empty());

    walker.tick(100, {99, 100, 7});
    assert(actions.groundUses.empty());

    walker.tick(160, {99, 100, 7});
    assert(actions.groundUses.size() == 1);
    assert(actions.groundUses.back().first == 1948);
    assert(same(actions.groundUses.back().second, {100, 100, 7}));

    walker.tick(220, {99, 100, 7});
    assert(!walker.running());
}

void testLadderWaitsForFloorChange() {
    MockActions actions;
    crome::Walker walker(actions);

    crome::RouteConfig route;
    route.loop = false;

    crome::RouteWaypoint w;
    w.type = crome::RouteWaypointType::Ladder;
    w.itemId = 1948;
    w.position = {200, 200, 7};
    route.waypoints.push_back(w);

    walker.setRoute(route);
    assert(walker.start(0, {197, 200, 7}));

    walker.tick(0, {197, 200, 7});
    assert(actions.walks.size() == 1);
    assert(same(actions.walks.back(), {199, 200, 7}));

    walker.tick(100, {199, 200, 7});
    walker.tick(160, {199, 200, 7});
    assert(actions.groundUses.size() == 1);
    assert(walker.running());

    walker.tick(300, {199, 200, 6});
    assert(!walker.running());
}

void testRopeWaitsForFloorChange() {
    MockActions actions;
    crome::Walker walker(actions);

    crome::RouteConfig route;
    route.loop = false;

    crome::RouteWaypoint w;
    w.type = crome::RouteWaypointType::Rope;
    w.itemId = 3003;
    w.position = {300, 300, 7};
    route.waypoints.push_back(w);

    walker.setRoute(route);
    assert(walker.start(0, {299, 300, 7}));

    walker.tick(0, {299, 300, 7});
    assert(actions.toolUses.size() == 1);
    assert(actions.toolUses.back().first == 3003);
    assert(walker.running());

    walker.tick(200, {299, 300, 6});
    assert(!walker.running());
}

} // namespace

int main() {
    testUseTargetRoundTrip();
    testUseApproachesAndUsesGroundObject();
    testLadderWaitsForFloorChange();
    testRopeWaitsForFloorChange();
    return 0;
}
