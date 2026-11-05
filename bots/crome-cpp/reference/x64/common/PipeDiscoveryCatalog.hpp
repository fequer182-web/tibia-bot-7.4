#pragma once
#include <array>
#include <cstdint>
#include <set>

namespace pipe_discovery {
enum class Owner { Game, Map, LocalPlayer, Instance };
struct Method { const char* key; const char* semantic; const char* token; Owner owner; bool ownedReturn; };
inline constexpr std::array<Method,29> methods{{
    {"gameAutoWalk","Game::autoWalk","autoWalk",Owner::Game,false},
    {"localPlayerAutoWalk","LocalPlayer::autoWalk","autoWalk",Owner::LocalPlayer,false},
    {"gameAttack","Game::attack","attack",Owner::Game,false},
    {"gameFollow","Game::follow","follow",Owner::Game,false},
    {"gameLook","Game::look","look",Owner::Game,false},
    {"gameDetailedLook","Game::detailedLook","detailedLook",Owner::Game,false},
    {"gameMove","Game::move","move",Owner::Game,false},
    {"gameUse","Game::use","use",Owner::Game,false},
    {"gameUseWith","Game::useWith","useWith",Owner::Game,false},
    {"gameTalk","Game::talk","talk",Owner::Game,false},
    {"gameTalkChannel","Game::talkChannel","talkChannel",Owner::Game,false},
    {"mapGetTile","Map::getTile","getTile",Owner::Map,true},
    {"mapGetCreatureById","Map::getCreatureById","getCreatureById",Owner::Map,true},
    {"mapGetCreatureByName","Map::getCreatureByName","getCreatureByName",Owner::Map,true},
    {"tileGetThings","Tile::getThings","getThings",Owner::Instance,false},
    {"tileGetThingCount","Tile::getThingCount","getThingCount",Owner::Instance,false},
    {"tileGetTopUseThing","Tile::getTopUseThing","getTopUseThing",Owner::Instance,true},
    {"tileGetTopMoveThing","Tile::getTopMoveThing","getTopMoveThing",Owner::Instance,true},
    {"tileGetGround","Tile::getGround","getGround",Owner::Instance,true},
    {"tileIsWalkable","Tile::isWalkable","isWalkable",Owner::Instance,false},
    {"tileIsPathable","Tile::isPathable","isPathable",Owner::Instance,false},
    {"localPlayerGetInventoryItem","LocalPlayer::getInventoryItem","getInventoryItem",Owner::Instance,true},
    {"itemGetId","Item::getId","getId",Owner::Instance,false},
    {"itemGetCount","Item::getCount","getCount",Owner::Instance,false},
    {"containerGetItem","Container::getItem","getItem",Owner::Instance,true},
    {"containerGetItems","Container::getItems","getItems",Owner::Instance,false},
    {"gameGetContainers","Game::getContainers","getContainers",Owner::Game,false},
    {"thingGetPosition","Thing::getPosition","getPosition",Owner::Instance,false},
    {"containerGetCapacity","Container::getCapacity","getCapacity",Owner::Instance,false}
}};
inline constexpr const char* schema="crome-pipe-discovery-x64-v1";
// Semantic alternatives must be selected by the requested natural action,
// never by a reference RVA or by borrowing another method's ABI.
struct ActionFamily { const char* key;std::array<const char*,2> functionKeys; };
inline constexpr std::array<ActionFamily,2> actionFamilies{{
    {"look",{"gameLook","gameDetailedLook"}},
    {"talk",{"gameTalkChannel","gameTalk"}}
}};
inline constexpr const char* actionSelectionJson=R"({"schema":"crome-action-selection-v1","policy":"current_build_discovery_then_two_natural_samples_per_method","look":{"candidates":["gameLook","gameDetailedLook"],"status":"runtime_selection_required"},"talk":{"candidates":["gameTalkChannel","gameTalk"],"status":"runtime_selection_required"},"mixSamplesAcrossMethods":false,"borrowAbiAcrossMethods":false,"callable":false})";
inline std::uintptr_t uniqueCandidate(const std::set<std::uintptr_t>& candidates) {
    return candidates.size()==1 ? *candidates.begin() : 0;
}
inline bool exactFingerprint(std::uint64_t timestamp,std::uint64_t imageSize,std::uint64_t hash,
                             std::uint64_t expectedTimestamp,std::uint64_t expectedSize,std::uint64_t expectedHash) {
    return timestamp && imageSize && hash &&
        timestamp==expectedTimestamp && imageSize==expectedSize && hash==expectedHash;
}
}
