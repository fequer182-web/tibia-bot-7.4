#pragma once

#include "otc_api/types.h"

#include <string>
#include <vector>

namespace otc {

struct ChainStep {
    std::int32_t offset = 0;
    bool deref = true;
};

struct InventoryLayout {
    bool present = false;
    std::int32_t arrayOffset = 0;
    std::int32_t itemIdOffset = 0;
    std::int32_t itemCountOffset = 4;
    int ptrSize = static_cast<int>(sizeof(void*));
    bool slotsArePointers = true;
};

struct ContainerLayout {
    bool present = false;
    std::vector<ChainStep> chain;
    std::string collection = "map"; // map | umap | array | vector
    int maxCount = 16;
    int ptrSize = static_cast<int>(sizeof(void*));
    std::int32_t idOffset = 0;
    int idSize = 4;
    std::int32_t capacityOffset = 0;
    int capacitySize = 4;
    std::int32_t nameOffset = 0;
    std::string nameKind = "msvc"; // msvc | gcc | cstr
    std::int32_t itemsOffset = 0;
    std::string itemsKind = "deque"; // deque | vector
    int dequeBlockSize = 0;          // 0 = MSVC default from itemPtrSize
    int itemPtrSize = 0;             // 0 = ptrSize; 8 if ItemPtr is shared_ptr
    std::int32_t itemIdOffset = 0;
    std::int32_t itemCountOffset = 4;
    std::int32_t mapHeadOffset = 0;
    std::int32_t mapSizeOffset = static_cast<std::int32_t>(sizeof(void*));
    std::int32_t mapNodeLeftOffset = 0;
    std::int32_t mapNodeParentOffset = static_cast<std::int32_t>(sizeof(void*));
    std::int32_t mapNodeRightOffset = static_cast<std::int32_t>(sizeof(void*) * 2);
    std::int32_t mapNodeIsnilOffset = static_cast<std::int32_t>(sizeof(void*) * 3 + 1);
    std::int32_t mapNodeKeyOffset = 0;
    std::int32_t mapNodePtrOffset = 0;
    std::int32_t umapHeadOffset = 0;
    std::int32_t umapSizeOffset = static_cast<std::int32_t>(sizeof(void*));
    std::int32_t listNextOffset = 0;
    std::int32_t listPrevOffset = static_cast<std::int32_t>(sizeof(void*));
    std::int32_t listKeyOffset = 0;
    std::int32_t listPtrOffset = 0;
};

struct CreatureLayout {
    bool present = false;
    std::vector<ChainStep> chain;
    std::string collection = "map"; // map | umap | vector
    int maxCount = 256;
    int ptrSize = static_cast<int>(sizeof(void*));
    int creaturePtrSize = 0; // 0 = ptrSize; 8 if CreaturePtr is shared_ptr
    std::int32_t idOffset = 0;
    int idSize = 4;
    std::int32_t nameOffset = 0;
    std::string nameKind = "msvc"; // msvc | gcc | cstr
    std::int32_t directionOffset = -1;
    int directionSize = 1;
    std::int32_t hppercOffset = 0;
    int hppercSize = 1;
    std::int32_t positionOffset = 0;
    bool positionPacked = true;
    std::int32_t skullOffset = -1;
    std::int32_t kindOffset = -1;
    std::string kindMode = "idRange"; // field | idRange
    std::int32_t mapHeadOffset = 0;
    std::int32_t mapSizeOffset = static_cast<std::int32_t>(sizeof(void*));
    std::int32_t mapNodeLeftOffset = 0;
    std::int32_t mapNodeParentOffset = static_cast<std::int32_t>(sizeof(void*));
    std::int32_t mapNodeRightOffset = static_cast<std::int32_t>(sizeof(void*) * 2);
    std::int32_t mapNodeIsnilOffset = static_cast<std::int32_t>(sizeof(void*) * 3 + 1);
    std::int32_t mapNodeKeyOffset = 0;
    std::int32_t mapNodePtrOffset = 0;
    std::int32_t umapHeadOffset = 0;
    std::int32_t umapSizeOffset = static_cast<std::int32_t>(sizeof(void*));
    std::int32_t listNextOffset = 0;
    std::int32_t listPrevOffset = static_cast<std::int32_t>(sizeof(void*));
    std::int32_t listKeyOffset = 0;
    std::int32_t listPtrOffset = 0;
    std::int32_t typePtrOffset = -1;  // T* to ThingType when creature m_name is empty
    std::int32_t typeNameOffset = -1;
};

struct ActionLayout {
    bool present = false;
    std::int32_t gameRva = 0;         // RVA of g_game
    std::int32_t attackingCreatureOffset = -1; // Game::m_attackingCreature T*
    std::int32_t talkChannelRva = 0;  // RVA of Game::talkChannel
    std::int32_t safeLogoutRva = 0;   // RVA of Game::safeLogout
    std::int32_t autoWalkRva = 0;     // RVA of LocalPlayer::autoWalk (not Game::autoWalk)
    std::int32_t useRva = 0;          // RVA of Game::use (not useWith)
    std::int32_t useWithRva = 0;      // RVA of Game::useWith
    std::int32_t attackRva = 0;       // RVA of Game::attack
    std::int32_t followRva = 0;       // RVA of Game::follow
    std::int32_t chaseModeRva = 0;    // RVA of Game::setChaseMode
    std::int32_t moveRva = 0;         // RVA of Game::move
    std::int32_t mapRva = 0;          // RVA of g_map
    std::int32_t getTileRva = 0;      // RVA of Map::getTile
    std::int32_t topUseRva = 0;       // RVA of Tile::getTopUseThing
    std::int32_t lookRva = 0;         // RVA of Game::look
    std::int32_t ambientLightRva = 0; // RVA of UIMap::setMinimumAmbientLight
    std::int32_t ambientLightGetRva = 0; // RVA of UIMap::getMinimumAmbientLight
};

struct MapLayout {
    bool present = false;
    std::vector<ChainStep> chain; // fallback if mapRva is 0; typically {mapRva, deref:false}
    std::int32_t mapRva = 0;      // RVA of g_map (Map object in .data)
    std::int32_t tileBlocksOffset = 0;
    int umapStride = 0;           // bytes between m_tileBlocks[z] and [z+1]
    int floors = 16;
    int maxZ = 15;
    std::string collection = "umap-blocks"; // umap-blocks | map-blocks
    int blockSize = 32;
    int tilePtrSize = 8;
    std::string indexOrder = "xy"; // xy: (x%bs)+(y%bs)*bs; yx: (y%bs)*bs+(x%bs)
    std::string keyOrder = "x16y"; // otc: (by*(65536/bs))+bx; x16y/y16x; otc-xy
    int ptrSize = static_cast<int>(sizeof(void*));
    std::int32_t umapHeadOffset = 0;
    std::int32_t umapSizeOffset = static_cast<std::int32_t>(sizeof(void*));
    std::int32_t listNextOffset = 0;
    std::int32_t listPrevOffset = static_cast<std::int32_t>(sizeof(void*));
    std::int32_t listKeyOffset = 8;
    std::int32_t tileArrayOffset = 12;
    bool tilesVector = false; // if true, node+tileArrayOffset is a pointer to TilePtr[]
    std::int32_t mapHeadOffset = 0;
    std::int32_t mapSizeOffset = static_cast<std::int32_t>(sizeof(void*));
    std::int32_t mapNodeLeftOffset = 0;
    std::int32_t mapNodeParentOffset = static_cast<std::int32_t>(sizeof(void*));
    std::int32_t mapNodeRightOffset = static_cast<std::int32_t>(sizeof(void*) * 2);
    std::int32_t mapNodeIsnilOffset = static_cast<std::int32_t>(sizeof(void*) * 3 + 1);
    std::int32_t mapNodeKeyOffset = 0;
    std::int32_t thingsOffset = 0;
    std::string thingsKind = "vector";
    int itemPtrSize = 0; // 0 = tilePtrSize
    std::int32_t itemIdOffset = 0;
    std::int32_t itemCountOffset = 4;
    std::int32_t tilePositionOffset = -1;
    bool tilePositionPacked = false;
    int radius = 8;
};

struct Profile {
    int version = 1;
    std::string buildId;
    StatType statType = StatType::Double;
    int capacityScale = 1; // 1, 100, or -100
    // Optional per-field encoding differences. Older profiles omit these and
    // retain the original all-stat / nearest-rounding behavior.
    bool experienceUInt64 = false;
    bool capacityTruncate = false;
    bool hasTotalCapacity = true;
    bool hasMagicLevel = true;
    bool hasSoul = true;
    std::vector<ChainStep> chain;
    std::int32_t offHealth = 0;
    std::int32_t offMaxHealth = 8;
    std::int32_t offFreeCap = 16;
    std::int32_t offTotalCap = 24;
    std::int32_t offExp = 32;
    std::int32_t offLevel = 40;
    std::int32_t offLevelPct = 48;
    std::int32_t offMana = 56;
    std::int32_t offMaxMana = 64;
    std::int32_t offMagicLevel = 72;
    std::int32_t offSoul = 96;
    bool hasPosition = false;
    std::int32_t positionOffset = 0;
    bool positionPacked = false; // uint16 x, uint16 y, uint8 z
    bool positionIndirect = false;
    std::int32_t positionInnerOffset = 0;
    bool hasStates = false;
    std::int32_t statesOffset = 0;
    InventoryLayout inventory;
    ContainerLayout containers;
    CreatureLayout creatures;
    ActionLayout actions;
    MapLayout map;
};

bool profileToJsonFile(const Profile& p, const std::wstring& path);
bool profileFromJsonFile(Profile& p, const std::wstring& path);
std::string profileToJson(const Profile& p);
bool profileFromJson(Profile& p, const std::string& json);
void preserveCalibratedSurfaces(Profile& fresh, const Profile& prior);

} // namespace otc

