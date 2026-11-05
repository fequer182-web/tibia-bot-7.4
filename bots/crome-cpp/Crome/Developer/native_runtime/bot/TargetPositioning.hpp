#pragma once
#include "../sdk/Position.hpp"
#include <algorithm>
#include <optional>

namespace crome {
// Indices match the persisted UI menu. None never dispatches combat movement.
inline int targetModeDistance(int mode) {
    if (mode == 3) return 2;
    if (mode == 4 || mode == 7) return 3;
    if (mode == 5) return 4;
    return 1;
}
inline std::optional<Position> targetPositionDestination(
    const Position& self, const Position& target, int mode) {
    if (mode < 0 || mode > 7 || mode == 1 || self.z != target.z) return std::nullopt;
    const int distance = targetModeDistance(mode);
    const bool diagonal = mode == 6 || mode == 7;
    const int dx = std::abs(self.x-target.x), dy = std::abs(self.y-target.y);
    if (diagonal ? dx == distance && dy == distance
                 : (mode == 0 ? std::max(dx,dy) <= distance : std::max(dx,dy) == distance))
        return std::nullopt;
    std::optional<Position> best;
    int bestCost = 1000000;
    for (int x=-distance;x<=distance;++x) for (int y=-distance;y<=distance;++y) {
        if (diagonal ? std::abs(x)!=distance || std::abs(y)!=distance
                     : std::max(std::abs(x),std::abs(y))!=distance) continue;
        Position candidate{target.x+x,target.y+y,target.z};
        if (candidate.x<=0 || candidate.x>=65535 || candidate.y<=0 || candidate.y>=65535) continue;
        const int cost=self.chebyshevDistance(candidate)*16+
            std::abs(self.x-candidate.x)+std::abs(self.y-candidate.y);
        if (cost<bestCost) {best=candidate;bestCost=cost;}
    }
    return best;
}
}
