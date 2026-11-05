#pragma once
#include <array>

// Discovery names only: addresses and ABI must come from the current build.
namespace ramonia_baseline {
inline constexpr std::array<const char*,9> gameActions{
    "autoWalk","attack","follow","look","move","use","useWith","talk","talkChannel"
};
inline constexpr std::array<const char*,10> abiMethods{
    "autoWalk","attack","follow","look","move","use","useWith","talk","talkChannel",
    "getTopMoveThing"
};
inline constexpr std::array<const char*,24> requiredBinds{
    "autoWalk","attack","follow","look","move","use","useWith","talkChannel",
    "getTile","getCreatureById","getPosition","getThings","getThingCount",
    "getTopMoveThing","isWalkable","isPathable","getInventoryItem","getId",
    "getCount","getContainers","getItems","getContainer","getCapacity","getName"
};
inline constexpr const char* id="ramonia_pipe_current_v1";
}
