#include "otc_api/profile.h"

#include <nlohmann/json.hpp>

namespace otc {
namespace {

using nlohmann::json;

const char* statTypeName(StatType t) {
    switch (t) {
    case StatType::Int32: return "int32";
    case StatType::Float: return "float";
    default: return "double";
    }
}

StatType parseStatType(const std::string& s) {
    if (s == "int32")
        return StatType::Int32;
    if (s == "float")
        return StatType::Float;
    return StatType::Double;
}

} // namespace

std::string profileToJson(const Profile& p) {
    json j;
    j["version"] = p.version;
    j["buildId"] = p.buildId;
    j["statType"] = statTypeName(p.statType);
    j["capacityScale"] = p.capacityScale;
    if (p.experienceUInt64)
        j["experienceType"] = "uint64";
    if (p.capacityTruncate)
        j["capacityRounding"] = "truncate";

    json chain = json::array();
    for (const auto& s : p.chain) {
        chain.push_back({{"offset", s.offset}, {"deref", s.deref}});
    }
    j["chain"] = chain;

    json fields = {
        {"health", p.offHealth},
        {"maxHealth", p.offMaxHealth},
        {"freeCapacity", p.offFreeCap},
        {"experience", p.offExp},
        {"level", p.offLevel},
        {"levelPercent", p.offLevelPct},
        {"mana", p.offMana},
        {"maxMana", p.offMaxMana},
    };
    if (p.hasTotalCapacity)
        fields["totalCapacity"] = p.offTotalCap;
    if (p.hasMagicLevel)
        fields["magicLevel"] = p.offMagicLevel;
    if (p.hasSoul)
        fields["soul"] = p.offSoul;
    j["fields"] = std::move(fields);

    if (p.hasPosition) {
        j["positionOffset"] = p.positionOffset;
        j["positionLayout"] = p.positionPacked ? "packed16" : "int32";
        if (p.positionIndirect) {
            j["positionIndirect"] = true;
            j["positionInnerOffset"] = p.positionInnerOffset;
        }
    }
    if (p.hasStates)
        j["statesOffset"] = p.statesOffset;

    if (p.inventory.present) {
        j["inventory"] = {
            {"arrayOffset", p.inventory.arrayOffset},
            {"itemIdOffset", p.inventory.itemIdOffset},
            {"itemCountOffset", p.inventory.itemCountOffset},
            {"ptrSize", p.inventory.ptrSize},
            {"slotsArePointers", p.inventory.slotsArePointers},
        };
    }

    if (p.containers.present) {
        json cchain = json::array();
        for (const auto& s : p.containers.chain)
            cchain.push_back({{"offset", s.offset}, {"deref", s.deref}});
        j["containers"] = {
            {"chain", cchain},
            {"collection", p.containers.collection},
            {"maxCount", p.containers.maxCount},
            {"ptrSize", p.containers.ptrSize},
            {"idOffset", p.containers.idOffset},
            {"idSize", p.containers.idSize},
            {"capacityOffset", p.containers.capacityOffset},
            {"capacitySize", p.containers.capacitySize},
            {"nameOffset", p.containers.nameOffset},
            {"nameKind", p.containers.nameKind},
            {"itemsOffset", p.containers.itemsOffset},
            {"itemsKind", p.containers.itemsKind},
            {"dequeBlockSize", p.containers.dequeBlockSize},
            {"itemPtrSize", p.containers.itemPtrSize},
            {"itemIdOffset", p.containers.itemIdOffset},
            {"itemCountOffset", p.containers.itemCountOffset},
            {"mapHeadOffset", p.containers.mapHeadOffset},
            {"mapSizeOffset", p.containers.mapSizeOffset},
            {"mapNodeLeftOffset", p.containers.mapNodeLeftOffset},
            {"mapNodeParentOffset", p.containers.mapNodeParentOffset},
            {"mapNodeRightOffset", p.containers.mapNodeRightOffset},
            {"mapNodeIsnilOffset", p.containers.mapNodeIsnilOffset},
            {"mapNodeKeyOffset", p.containers.mapNodeKeyOffset},
            {"mapNodePtrOffset", p.containers.mapNodePtrOffset},
            {"umapHeadOffset", p.containers.umapHeadOffset},
            {"umapSizeOffset", p.containers.umapSizeOffset},
            {"listNextOffset", p.containers.listNextOffset},
            {"listPrevOffset", p.containers.listPrevOffset},
            {"listKeyOffset", p.containers.listKeyOffset},
            {"listPtrOffset", p.containers.listPtrOffset},
        };
    }

    if (p.creatures.present) {
        json cchain = json::array();
        for (const auto& s : p.creatures.chain)
            cchain.push_back({{"offset", s.offset}, {"deref", s.deref}});
        json cr = {
            {"chain", cchain},
            {"collection", p.creatures.collection},
            {"maxCount", p.creatures.maxCount},
            {"ptrSize", p.creatures.ptrSize},
            {"creaturePtrSize", p.creatures.creaturePtrSize},
            {"idOffset", p.creatures.idOffset},
            {"idSize", p.creatures.idSize},
            {"nameOffset", p.creatures.nameOffset},
            {"nameKind", p.creatures.nameKind},
            {"directionOffset", p.creatures.directionOffset},
            {"directionSize", p.creatures.directionSize},
            {"hppercOffset", p.creatures.hppercOffset},
            {"hppercSize", p.creatures.hppercSize},
            {"positionOffset", p.creatures.positionOffset},
            {"positionLayout", p.creatures.positionPacked ? "packed16" : "int32"},
            {"kindMode", p.creatures.kindMode},
            {"mapHeadOffset", p.creatures.mapHeadOffset},
            {"mapSizeOffset", p.creatures.mapSizeOffset},
            {"mapNodeLeftOffset", p.creatures.mapNodeLeftOffset},
            {"mapNodeParentOffset", p.creatures.mapNodeParentOffset},
            {"mapNodeRightOffset", p.creatures.mapNodeRightOffset},
            {"mapNodeIsnilOffset", p.creatures.mapNodeIsnilOffset},
            {"mapNodeKeyOffset", p.creatures.mapNodeKeyOffset},
            {"mapNodePtrOffset", p.creatures.mapNodePtrOffset},
            {"umapHeadOffset", p.creatures.umapHeadOffset},
            {"umapSizeOffset", p.creatures.umapSizeOffset},
            {"listNextOffset", p.creatures.listNextOffset},
            {"listPrevOffset", p.creatures.listPrevOffset},
            {"listKeyOffset", p.creatures.listKeyOffset},
            {"listPtrOffset", p.creatures.listPtrOffset},
        };
        if (p.creatures.skullOffset >= 0)
            cr["skullOffset"] = p.creatures.skullOffset;
        if (p.creatures.kindOffset >= 0)
            cr["kindOffset"] = p.creatures.kindOffset;
        if (p.creatures.typePtrOffset >= 0)
            cr["typePtrOffset"] = p.creatures.typePtrOffset;
        if (p.creatures.typeNameOffset >= 0)
            cr["typeNameOffset"] = p.creatures.typeNameOffset;
        j["creatures"] = cr;
    }

    if (p.actions.present) {
        j["actions"] = {
            {"gameRva", p.actions.gameRva},
            {"attackingCreatureOffset", p.actions.attackingCreatureOffset},
            {"talkChannelRva", p.actions.talkChannelRva},
            {"safeLogoutRva", p.actions.safeLogoutRva},
            {"autoWalkRva", p.actions.autoWalkRva},
            {"useRva", p.actions.useRva},
            {"useWithRva", p.actions.useWithRva},
            {"attackRva", p.actions.attackRva},
            {"followRva", p.actions.followRva},
            {"chaseModeRva", p.actions.chaseModeRva},
            {"moveRva", p.actions.moveRva},
            {"mapRva", p.actions.mapRva},
            {"getTileRva", p.actions.getTileRva},
            {"topUseRva", p.actions.topUseRva},
            {"lookRva", p.actions.lookRva},
            {"ambientLightRva", p.actions.ambientLightRva},
            {"ambientLightGetRva", p.actions.ambientLightGetRva},
        };
    }

    if (p.map.present) {
        json mchain = json::array();
        for (const auto& s : p.map.chain)
            mchain.push_back({{"offset", s.offset}, {"deref", s.deref}});
        j["map"] = {
            {"chain", mchain},
            {"mapRva", p.map.mapRva},
            {"tileBlocksOffset", p.map.tileBlocksOffset},
            {"umapStride", p.map.umapStride},
            {"floors", p.map.floors},
            {"maxZ", p.map.maxZ},
            {"collection", p.map.collection},
            {"blockSize", p.map.blockSize},
            {"tilePtrSize", p.map.tilePtrSize},
            {"indexOrder", p.map.indexOrder},
            {"keyOrder", p.map.keyOrder},
            {"ptrSize", p.map.ptrSize},
            {"umapHeadOffset", p.map.umapHeadOffset},
            {"umapSizeOffset", p.map.umapSizeOffset},
            {"listNextOffset", p.map.listNextOffset},
            {"listPrevOffset", p.map.listPrevOffset},
            {"listKeyOffset", p.map.listKeyOffset},
            {"tileArrayOffset", p.map.tileArrayOffset},
            {"tilesVector", p.map.tilesVector},
            {"mapHeadOffset", p.map.mapHeadOffset},
            {"mapSizeOffset", p.map.mapSizeOffset},
            {"mapNodeLeftOffset", p.map.mapNodeLeftOffset},
            {"mapNodeParentOffset", p.map.mapNodeParentOffset},
            {"mapNodeRightOffset", p.map.mapNodeRightOffset},
            {"mapNodeIsnilOffset", p.map.mapNodeIsnilOffset},
            {"mapNodeKeyOffset", p.map.mapNodeKeyOffset},
            {"thingsOffset", p.map.thingsOffset},
            {"thingsKind", p.map.thingsKind},
            {"itemPtrSize", p.map.itemPtrSize},
            {"itemIdOffset", p.map.itemIdOffset},
            {"itemCountOffset", p.map.itemCountOffset},
            {"radius", p.map.radius},
        };
        if (p.map.tilePositionOffset >= 0) {
            j["map"]["tilePositionOffset"] = p.map.tilePositionOffset;
            j["map"]["tilePositionLayout"] = p.map.tilePositionPacked ? "packed16" : "int32";
        }
    }

    return j.dump(2);
}

bool profileFromJson(Profile& p, const std::string& text) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return false;

    p = Profile{};
    p.version = j.value("version", 1);
    p.buildId = j.value("buildId", "");
    p.statType = parseStatType(j.value("statType", "double"));
    p.capacityScale = j.value("capacityScale", 1);
    p.experienceUInt64 = j.value("experienceType", "stat") == "uint64";
    p.capacityTruncate = j.value("capacityRounding", "nearest") == "truncate";

    if (j.contains("chain") && j["chain"].is_array()) {
        for (const auto& s : j["chain"]) {
            ChainStep step;
            step.offset = s.value("offset", 0);
            step.deref = s.value("deref", true);
            p.chain.push_back(step);
        }
    }

    if (j.contains("fields") && j["fields"].is_object()) {
        const auto& f = j["fields"];
        p.offHealth = f.value("health", 0);
        p.offMaxHealth = f.value("maxHealth", 8);
        p.offFreeCap = f.value("freeCapacity", 16);
        p.hasTotalCapacity = f.contains("totalCapacity");
        p.offTotalCap = f.value("totalCapacity", 0);
        p.offExp = f.value("experience", 32);
        p.offLevel = f.value("level", 40);
        p.offLevelPct = f.value("levelPercent", 48);
        p.offMana = f.value("mana", 56);
        p.offMaxMana = f.value("maxMana", 64);
        p.hasMagicLevel = f.contains("magicLevel");
        p.offMagicLevel = f.value("magicLevel", 0);
        p.hasSoul = f.contains("soul");
        p.offSoul = f.value("soul", 0);
    }

    if (j.contains("positionOffset") && j["positionOffset"].is_number_integer()) {
        p.hasPosition = true;
        p.positionOffset = j["positionOffset"].get<int>();
        p.positionPacked = j.value("positionLayout", "packed16") != "int32";
        p.positionIndirect = j.value("positionIndirect", false);
        p.positionInnerOffset = j.value("positionInnerOffset", 0);
    }
    if (j.contains("statesOffset") && j["statesOffset"].is_number_integer()) {
        p.hasStates = true;
        p.statesOffset = j["statesOffset"].get<int>();
    }
    if (j.contains("inventory") && j["inventory"].is_object()) {
        const auto& inv = j["inventory"];
        p.inventory.present = true;
        p.inventory.arrayOffset = inv.value("arrayOffset", 0);
        p.inventory.itemIdOffset = inv.value("itemIdOffset", 0);
        p.inventory.itemCountOffset = inv.value("itemCountOffset", 4);
        p.inventory.ptrSize = inv.value("ptrSize", static_cast<int>(sizeof(void*)));
        p.inventory.slotsArePointers = inv.value("slotsArePointers", true);
    }
    if (j.contains("containers") && j["containers"].is_object()) {
        const auto& c = j["containers"];
        p.containers.present = true;
        p.containers.collection = c.value("collection", "map");
        p.containers.maxCount = c.value("maxCount", 16);
        p.containers.ptrSize = c.value("ptrSize", static_cast<int>(sizeof(void*)));
        p.containers.idOffset = c.value("idOffset", 0);
        p.containers.idSize = c.value("idSize", 4);
        p.containers.capacityOffset = c.value("capacityOffset", 0);
        p.containers.capacitySize = c.value("capacitySize", 4);
        p.containers.nameOffset = c.value("nameOffset", 0);
        p.containers.nameKind = c.value("nameKind", "msvc");
        p.containers.itemsOffset = c.value("itemsOffset", 0);
        p.containers.itemsKind = c.value("itemsKind", "deque");
        p.containers.dequeBlockSize = c.value("dequeBlockSize", 0);
        p.containers.itemPtrSize = c.value("itemPtrSize", 0);
        p.containers.itemIdOffset = c.value("itemIdOffset", 0);
        p.containers.itemCountOffset = c.value("itemCountOffset", 4);
        p.containers.mapHeadOffset = c.value("mapHeadOffset", 0);
        p.containers.mapSizeOffset =
            c.value("mapSizeOffset", static_cast<int>(sizeof(void*)));
        p.containers.mapNodeLeftOffset = c.value("mapNodeLeftOffset", 0);
        p.containers.mapNodeParentOffset =
            c.value("mapNodeParentOffset", static_cast<int>(sizeof(void*)));
        p.containers.mapNodeRightOffset =
            c.value("mapNodeRightOffset", static_cast<int>(sizeof(void*) * 2));
        p.containers.mapNodeIsnilOffset =
            c.value("mapNodeIsnilOffset", static_cast<int>(sizeof(void*) * 3 + 1));
        p.containers.mapNodeKeyOffset = c.value("mapNodeKeyOffset", 0);
        p.containers.mapNodePtrOffset = c.value("mapNodePtrOffset", 0);
        p.containers.umapHeadOffset = c.value("umapHeadOffset", 0);
        p.containers.umapSizeOffset =
            c.value("umapSizeOffset", static_cast<int>(sizeof(void*)));
        p.containers.listNextOffset = c.value("listNextOffset", 0);
        p.containers.listPrevOffset =
            c.value("listPrevOffset", static_cast<int>(sizeof(void*)));
        p.containers.listKeyOffset = c.value("listKeyOffset", 0);
        p.containers.listPtrOffset = c.value("listPtrOffset", 0);
        if (c.contains("chain") && c["chain"].is_array()) {
            for (const auto& s : c["chain"]) {
                ChainStep step;
                step.offset = s.value("offset", 0);
                step.deref = s.value("deref", true);
                p.containers.chain.push_back(step);
            }
        }
    }
    if (j.contains("creatures") && j["creatures"].is_object()) {
        const auto& c = j["creatures"];
        p.creatures.present = true;
        p.creatures.collection = c.value("collection", "map");
        p.creatures.maxCount = c.value("maxCount", 256);
        p.creatures.ptrSize = c.value("ptrSize", static_cast<int>(sizeof(void*)));
        p.creatures.creaturePtrSize = c.value("creaturePtrSize", 0);
        p.creatures.idOffset = c.value("idOffset", 0);
        p.creatures.idSize = c.value("idSize", 4);
    p.creatures.nameOffset = c.value("nameOffset", 0);
    p.creatures.nameKind = c.value("nameKind", "msvc");
    p.creatures.directionOffset = c.value("directionOffset", -1);
    p.creatures.directionSize = c.value("directionSize", 1);
        p.creatures.hppercOffset = c.value("hppercOffset", 0);
        p.creatures.hppercSize = c.value("hppercSize", 1);
        p.creatures.positionOffset = c.value("positionOffset", 0);
        p.creatures.positionPacked = c.value("positionLayout", "packed16") != "int32";
        p.creatures.skullOffset = c.value("skullOffset", -1);
        p.creatures.kindOffset = c.value("kindOffset", -1);
        p.creatures.kindMode = c.value("kindMode", "idRange");
        p.creatures.mapHeadOffset = c.value("mapHeadOffset", 0);
        p.creatures.mapSizeOffset =
            c.value("mapSizeOffset", static_cast<int>(sizeof(void*)));
        p.creatures.mapNodeLeftOffset = c.value("mapNodeLeftOffset", 0);
        p.creatures.mapNodeParentOffset =
            c.value("mapNodeParentOffset", static_cast<int>(sizeof(void*)));
        p.creatures.mapNodeRightOffset =
            c.value("mapNodeRightOffset", static_cast<int>(sizeof(void*) * 2));
        p.creatures.mapNodeIsnilOffset =
            c.value("mapNodeIsnilOffset", static_cast<int>(sizeof(void*) * 3 + 1));
        p.creatures.mapNodeKeyOffset = c.value("mapNodeKeyOffset", 0);
        p.creatures.mapNodePtrOffset = c.value("mapNodePtrOffset", 0);
        p.creatures.umapHeadOffset = c.value("umapHeadOffset", 0);
        p.creatures.umapSizeOffset =
            c.value("umapSizeOffset", static_cast<int>(sizeof(void*)));
        p.creatures.listNextOffset = c.value("listNextOffset", 0);
        p.creatures.listPrevOffset =
            c.value("listPrevOffset", static_cast<int>(sizeof(void*)));
        p.creatures.listKeyOffset = c.value("listKeyOffset", 0);
        p.creatures.listPtrOffset = c.value("listPtrOffset", 0);
        p.creatures.typePtrOffset = c.value("typePtrOffset", -1);
        p.creatures.typeNameOffset = c.value("typeNameOffset", -1);
        if (c.contains("chain") && c["chain"].is_array()) {
            for (const auto& s : c["chain"]) {
                ChainStep step;
                step.offset = s.value("offset", 0);
                step.deref = s.value("deref", true);
                p.creatures.chain.push_back(step);
            }
        }
    }
    if (j.contains("actions") && j["actions"].is_object()) {
        const auto& a = j["actions"];
        p.actions.gameRva = a.value("gameRva", 0);
        // Profiles created before target exposure already used the documented
        // Game layout for attack/follow. Migrate only those proven action
        // profiles; read-only or unknown Game layouts remain fail-closed.
        const bool legacyCreatureActions = a.value("attackRva", 0) != 0 ||
                                           a.value("followRva", 0) != 0;
        p.actions.attackingCreatureOffset =
            a.value("attackingCreatureOffset",
                    legacyCreatureActions ? static_cast<int>(sizeof(void*) * 2) : -1);
        p.actions.talkChannelRva = a.value("talkChannelRva", 0);
        p.actions.safeLogoutRva = a.value("safeLogoutRva", 0);
        p.actions.autoWalkRva = a.value("autoWalkRva", 0);
        p.actions.useRva = a.value("useRva", 0);
        p.actions.useWithRva = a.value("useWithRva", 0);
        p.actions.attackRva = a.value("attackRva", 0);
        p.actions.followRva = a.value("followRva", 0);
        p.actions.chaseModeRva = a.value("chaseModeRva", 0);
        p.actions.moveRva = a.value("moveRva", 0);
        p.actions.mapRva = a.value("mapRva", 0);
        p.actions.getTileRva = a.value("getTileRva", 0);
        p.actions.topUseRva = a.value("topUseRva", 0);
        p.actions.lookRva = a.value("lookRva", 0);
        p.actions.ambientLightRva = a.value("ambientLightRva", 0);
        p.actions.ambientLightGetRva = a.value("ambientLightGetRva", 0);
        p.actions.present = p.actions.gameRva != 0 &&
                            (p.actions.talkChannelRva != 0 || p.actions.safeLogoutRva != 0 ||
                             p.actions.autoWalkRva != 0 ||
                             p.actions.useRva != 0 || p.actions.useWithRva != 0 ||
                             p.actions.attackRva != 0 || p.actions.followRva != 0 ||
                             p.actions.moveRva != 0 || p.actions.ambientLightRva != 0);
    }
    if (j.contains("map") && j["map"].is_object()) {
        const auto& m = j["map"];
        p.map.present = true;
        p.map.mapRva = m.value("mapRva", 0);
        p.map.tileBlocksOffset = m.value("tileBlocksOffset", 0);
        p.map.umapStride = m.value("umapStride", 0);
        p.map.floors = m.value("floors", 16);
        p.map.maxZ = m.value("maxZ", 15);
        p.map.collection = m.value("collection", "umap-blocks");
        p.map.blockSize = m.value("blockSize", 32);
        p.map.tilePtrSize = m.value("tilePtrSize", 8);
        p.map.indexOrder = m.value("indexOrder", "xy");
        p.map.keyOrder = m.value("keyOrder", "x16y");
        p.map.ptrSize = m.value("ptrSize", static_cast<int>(sizeof(void*)));
        p.map.umapHeadOffset = m.value("umapHeadOffset", 0);
        p.map.umapSizeOffset =
            m.value("umapSizeOffset", static_cast<int>(sizeof(void*)));
        p.map.listNextOffset = m.value("listNextOffset", 0);
        p.map.listPrevOffset =
            m.value("listPrevOffset", static_cast<int>(sizeof(void*)));
        p.map.listKeyOffset = m.value("listKeyOffset", 8);
        p.map.tileArrayOffset = m.value("tileArrayOffset", 12);
        p.map.tilesVector = m.value("tilesVector", false);
        p.map.mapHeadOffset = m.value("mapHeadOffset", 0);
        p.map.mapSizeOffset =
            m.value("mapSizeOffset", static_cast<int>(sizeof(void*)));
        p.map.mapNodeLeftOffset = m.value("mapNodeLeftOffset", 0);
        p.map.mapNodeParentOffset =
            m.value("mapNodeParentOffset", static_cast<int>(sizeof(void*)));
        p.map.mapNodeRightOffset =
            m.value("mapNodeRightOffset", static_cast<int>(sizeof(void*) * 2));
        p.map.mapNodeIsnilOffset =
            m.value("mapNodeIsnilOffset", static_cast<int>(sizeof(void*) * 3 + 1));
        p.map.mapNodeKeyOffset = m.value("mapNodeKeyOffset", 0);
        p.map.thingsOffset = m.value("thingsOffset", 0);
        p.map.thingsKind = m.value("thingsKind", "vector");
        p.map.itemPtrSize = m.value("itemPtrSize", 0);
        p.map.itemIdOffset = m.value("itemIdOffset", 0);
        p.map.itemCountOffset = m.value("itemCountOffset", 4);
        p.map.radius = m.value("radius", 8);
        if (m.contains("tilePositionOffset") && m["tilePositionOffset"].is_number_integer()) {
            p.map.tilePositionOffset = m["tilePositionOffset"].get<int>();
            p.map.tilePositionPacked = m.value("tilePositionLayout", "int32") != "int32";
        }
        if (m.contains("chain") && m["chain"].is_array()) {
            for (const auto& s : m["chain"]) {
                ChainStep step;
                step.offset = s.value("offset", 0);
                step.deref = s.value("deref", true);
                p.map.chain.push_back(step);
            }
        }
    }
    return true;
}

void preserveCalibratedSurfaces(Profile& fresh, const Profile& prior) {
    if (!prior.buildId.empty() && !fresh.buildId.empty() && prior.buildId != fresh.buildId)
        return;
    if (!fresh.hasPosition && prior.hasPosition) {
        fresh.hasPosition = true;
        fresh.positionOffset = prior.positionOffset;
        fresh.positionPacked = prior.positionPacked;
        fresh.positionIndirect = prior.positionIndirect;
        fresh.positionInnerOffset = prior.positionInnerOffset;
    }
    if (!fresh.hasStates && prior.hasStates) {
        fresh.hasStates = true;
        fresh.statesOffset = prior.statesOffset;
    }
    if (!fresh.inventory.present && prior.inventory.present)
        fresh.inventory = prior.inventory;
    if (!fresh.containers.present && prior.containers.present)
        fresh.containers = prior.containers;
    if (!fresh.creatures.present && prior.creatures.present)
        fresh.creatures = prior.creatures;
    if (!fresh.actions.present && prior.actions.present)
        fresh.actions = prior.actions;
    if (!fresh.map.present && prior.map.present)
        fresh.map = prior.map;
}

bool profileToJsonFile(const Profile& p, const std::wstring& path) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f)
        return false;
    const std::string text = profileToJson(p);
    const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    fclose(f);
    return ok;
}

bool profileFromJsonFile(Profile& p, const std::wstring& path) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f)
        return false;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    const long sz = ftell(f);
    rewind(f);
    if (sz <= 0) {
        fclose(f);
        return false;
    }
    std::string text(static_cast<std::size_t>(sz), '\0');
    const bool ok = fread(text.data(), 1, text.size(), f) == text.size();
    fclose(f);
    return ok && profileFromJson(p, text);
}

} // namespace otc

