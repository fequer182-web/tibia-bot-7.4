#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <nlohmann/json.hpp>

namespace crome {
enum class FieldStatus { Missing, Pending, Candidate, Confirmed };
inline FieldStatus parseStatus(std::string s) {
    if (s == "confirmed") return FieldStatus::Confirmed;
    if (s == "candidate") return FieldStatus::Candidate;
    if (s == "pending") return FieldStatus::Pending;
    return FieldStatus::Missing;
}
struct FieldSpec { uint64_t offset{}; std::string type; FieldStatus status{FieldStatus::Missing}; double confidence{}; std::string method; };
struct ActionSpec { uint64_t rva{}; std::string signature; FieldStatus status{FieldStatus::Missing}; bool available() const { return rva != 0; } };
struct BuildInfo {
    std::string profileName, profileStatus, exe, architecture, rendererHint;
    uint32_t pointerSize{}; uint64_t timestamp{}, sizeOfImage{}, entryPointRva{}, textRva{}, textSize{}; std::string textHash;
};
struct LocalPlayerLayout { uint64_t vtableRva{}, directGlobalRva{}, indirectGlobalRva{}, indirectOwnerOffset{}; std::map<std::string,FieldSpec> fields; };
struct CreatureLayout { std::map<std::string,FieldSpec> fields; uint64_t monsterVtableRva{}, playerVtableRva{}, npcVtableRva{}; };
struct MapLayout {
    uint64_t rva{}, centerX{}, centerY{}, centerZ{}, north{}, east{}, south{}, west{};
    uint64_t tileThingsBegin{}, tileThingsEnd{}, tileThingsCap{}, tileVtableRva{}, thingStorageStride{};
    std::string thingStorageMode;
    uint64_t tilePathDepth{}, tilePathMapOffset{}, tilePathChildOffset{}, tilePathHeaderOffset{}, tileStorageStride{};
    std::string tileStorageMode;
};
struct InventoryLayout { uint64_t baseOffset{}, slotStride{}, itemIdOffset{}, itemCountOffset{}, itemCountWidth{}; std::string storageMode; };
struct ContainerLayout {
    uint64_t gameOffset{}, mapHeaderOffset{}, mapSizeOffset{}, nodeLeftOffset{}, nodeRightOffset{}, nodeNilOffset{}, nodeKeyOffset{}, nodeValueOffset{};
    uint64_t idOffset{}, capacityOffset{}, itemOffset{}, nameOffset{}, hasParentOffset{}, closedOffset{}, unlockedOffset{}, pagesOffset{}, sizeOffset{}, firstIndexOffset{};
    uint64_t dequeMapOffset{}, dequeMapSizeOffset{}, dequeOffsetOffset{}, itemCountOffset{}, itemElementStride{}; std::string itemStorageMode;
};
struct GameLayout { uint64_t rva{}, localPlayer{}, attackTarget{}, followTarget{}; };
struct ActionLayout { std::map<std::string,ActionSpec> actions; };

struct LookFeature {
    FieldStatus status{FieldStatus::Missing};
    std::string architecture, hookFunction, hookArgument, itemIdType;
    uint64_t hookRva{}, thingIndirection{}, itemIdOffset{};
    bool positionConfirmed{}, contextConfirmed{}, supportsGroundAndContainer{};
    std::string positionMode;
    uint64_t positionXOffset{}, positionYOffset{}, positionZOffset{}, inventoryXSentinel{};
    bool ready() const { return status==FieldStatus::Confirmed && hookRva!=0 && itemIdOffset!=0; }
};
struct HackLightFeature {
    FieldStatus status{FieldStatus::Missing};
    std::string architecture, storage, type, restorePolicy;
    uint64_t intensityOffset{};
    int noneValue{}, utevoLuxValue{}, utevoGranLuxValue{}, hackValue{};
    bool visualWriteConfirmed{}, restoreConfirmed{};
    bool ready() const { return status==FieldStatus::Confirmed && intensityOffset!=0 && type=="u8"; }
};
struct FeatureLayout { LookFeature look; HackLightFeature hackLight; };

struct BuildProfile {
    int schemaVersion{}; std::string schema; bool readOnlyCoreReady{}, strictConsumerReady{}, nativeActionsReady{};
    BuildInfo build; LocalPlayerLayout localPlayer; CreatureLayout creature; GameLayout game; MapLayout map; InventoryLayout inventory; ContainerLayout containers; ActionLayout action; FeatureLayout features;
    nlohmann::json raw;
};
}
