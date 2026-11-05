#pragma once
#include "Item.hpp"
#include "Position.hpp"
#include "Snapshots.hpp"
#include <vector>
namespace crome {
struct TileSnapshot {
    Position position{};
    std::vector<Item> items;
    std::vector<CreatureSnapshot> creatures;
};
struct MapSnapshot {
    std::vector<TileSnapshot> tiles;
};
}
