#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "MiniJson.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <optional>
#include <sstream>
#include <set>
#include <string>
#include <vector>

namespace {

static_assert(sizeof(void*) == 4, "CromeRamoniaBridge must be built as x86");

HMODULE g_module{};
volatile LONG g_running = 1;

std::string g_status = "starting";
std::string g_profileError;
std::uintptr_t g_base{};
std::uint32_t g_imageTimestamp{};
std::uint32_t g_imageSize{};
bool g_buildMatched{false};

HWND g_gameWindow{};
DWORD g_gameThreadId{};
HHOOK g_gameThreadHook{};
HANDLE g_actionEvent{};
constexpr UINT kActionMessage = WM_APP + 0x620;

// V32 active-call gate. V40 deep static + natural callsite capture proved
// ECX=Game plus 16 callee-cleaned stack bytes:
//   arg1 Directions& (4 bytes) + arg2 Position value (12 bytes), RET 16.
constexpr bool kAutoWalkActiveCallV40Implemented = true;

enum PendingActionKind : LONG {
    PendingNone = 0,
    PendingAttack = 1,
    PendingUse = 2,
    PendingMove = 3,
    PendingTalk = 4,
    PendingUseWithSelf = 5,
    PendingAutoWalk = 6,
    PendingUseWithTile = 7,
    PendingMoveTileToInventory = 8,
    PendingMoveTileToGround = 9,
    PendingTopMoveRead = 10,
    PendingUncoverTile = 11,
    PendingPickupTopMove = 12,
    PendingUseWithExposedTile = 13
};

volatile LONG g_lastActionCode{};
volatile LONG g_actionTokenCounter{};
volatile LONG g_pendingActionToken{};
volatile LONG g_pendingActionKind{};
std::uint32_t g_lastActionTargetId{};
std::uintptr_t g_pendingAttackTarget{};
std::uint32_t g_pendingAttackRequestedId{};

std::uintptr_t g_pendingUseTile{};
std::uintptr_t g_pendingUseContainer{};
std::uint32_t g_pendingUseContainerId{};
std::uintptr_t g_pendingUseHolder{};
std::uintptr_t g_pendingUseItem{};
std::uint16_t g_pendingUseItemId{};

std::uintptr_t g_pendingMoveSourceContainer{};
std::uintptr_t g_pendingMoveHolder{};
std::uintptr_t g_pendingMoveItem{};
std::uint16_t g_pendingMoveItemId{};
std::uint32_t g_pendingMoveCount{};
std::uintptr_t g_pendingMoveTargetContainer{};
std::uint32_t g_pendingMoveTargetContainerId{};
std::uint8_t g_pendingMoveTargetSlot{};

std::uintptr_t g_pendingMoveSourceTile{};
std::uintptr_t g_pendingMoveTileExpectedItem{};
std::uint16_t g_pendingMoveTileItemId{};
std::uint8_t g_pendingMoveInventorySlot{};
std::uint32_t g_pendingMoveGroundX{};
std::uint32_t g_pendingMoveGroundY{};
std::uint8_t g_pendingMoveGroundZ{};
std::uint32_t g_pendingMoveGroundCount{};

std::uint32_t g_pendingTalkMode{1};
std::uint32_t g_pendingTalkChannel{};
char g_pendingTalkMessage[16]{};

std::uintptr_t g_pendingUseWithTargetTile{};
std::uintptr_t g_pendingUseWithTargetHolder{};
std::uintptr_t g_pendingUseWithTargetItem{};
std::uint16_t g_pendingUseWithTargetItemId{};

std::uint32_t g_pendingWalkX{};
std::uint32_t g_pendingWalkY{};
std::uint8_t g_pendingWalkZ{};
ULONGLONG g_lastAutoWalkDispatchTick{};

// One-shot Tile::getTopMoveThing runtime diagnostic. This is intentionally
// separate from pickup/uncover and never moves an item.
std::uintptr_t g_pendingTopMoveTile{};
std::uint32_t g_pendingTopMoveX{};
std::uint32_t g_pendingTopMoveY{};
std::uint8_t g_pendingTopMoveZ{};
std::uintptr_t g_topMoveThing{};
std::uintptr_t g_topMoveVtable{};
std::uint16_t g_topMoveItemId{};
std::uint16_t g_topMoveItemCount{};
std::uint32_t g_topMoveRefsAfterCall{};
std::uint32_t g_topMoveRefsAfterRelease{};
bool g_topMoveWasItem{};
bool g_topMoveRefReleased{};

struct Profile {
    std::string schema;
    int profileVersion{};
    std::string architecture;
    std::string timestamp;
    std::string sizeOfImage;
    std::string textHash;
    std::uint32_t expectedTimestamp{};
    std::uint32_t expectedImageSize{};
    std::uintptr_t gameRva{};
    std::size_t localPlayerOffset{};
    std::size_t attackTargetOffset{};
    std::size_t followTargetOffset{};
    std::size_t posX{}, posY{}, posZ{};
    std::size_t creatureIdOffset{};
    std::size_t creatureNameOffset{};
    std::size_t creatureHpPercentOffset{};
    std::uintptr_t monsterVtableRva{};
    std::uintptr_t localPlayerVtableRva{};
    std::uintptr_t itemVtableRva{};
    std::uintptr_t containerVtableRva{};
    std::size_t itemIdOffset{};
    std::size_t itemCountOffset{};
    bool itemContractReady{};

    std::size_t inventoryArrayOffset{};
    std::size_t inventoryStride{};
    bool inventoryContractReady{};

    std::size_t openContainersOffset{};
    std::size_t containerIdOffset{};
    std::size_t containerCapacityOffset{};
    std::size_t containerNameOffset{};
    std::size_t containerItemsOffset{};
    std::size_t containerItemStride{};
    bool containerContractReady{};

    std::uintptr_t useMethodRva{};
    bool useRuntimeConfirmed{};
    bool useItemSharedRefConfirmed{};

    std::uintptr_t lookMethodRva{};
    bool lookRuntimeConfirmed{};
    bool lookItemSharedRefConfirmed{};

    std::uintptr_t useWithMethodRva{};
    bool useWithRuntimeConfirmed{};
    bool useWithItemSharedRefConfirmed{};
    bool useWithThingSharedRefConfirmed{};

    std::uintptr_t moveMethodRva{};
    bool moveRuntimeConfirmed{};
    bool moveItemSharedRefConfirmed{};
    bool movePositionRefConfirmed{};
    bool moveCountConfirmed{};

    std::uintptr_t autoWalkMethodRva{};
    bool autoWalkRuntimeConfirmed{};
    bool autoWalkDirectionsVectorRefConfirmed{};
    bool autoWalkStartPositionValueConfirmed{};
    std::uint32_t autoWalkStackCleanupBytes{};

    std::uintptr_t tileVtableRva{};
    std::uintptr_t getTopMoveThingMethodRva{};
    std::size_t tileThingsOffset{};
    std::size_t tileThingsEndOffset{};
    std::size_t tileThingsCapOffset{};
    std::size_t tileThingsElemSize{};
    bool tileThingsRuntimeConfirmed{};

    std::uintptr_t uiMapVtableRva{};
    std::uintptr_t mapViewVtableRva{};
    std::size_t uiMapToMapViewOffset{};
    std::size_t mapViewDrawLightsOffset{};
    std::size_t mapViewMinimumAmbientOffset{};
    bool worldLightContractReady{};

    std::uintptr_t talkChannelMethodRva{};
    bool talkChannelRuntimeConfirmed{};
    bool talkModeArgConfirmed{};
    bool talkChannelArgConfirmed{};
    bool talkMessageMsvcStringRefConfirmed{};
    std::uint32_t talkSayModeValue{1};
    std::uint32_t talkDefaultChannel{};

    std::uintptr_t attackMethodRva{};
    bool attackRuntimeConfirmed{};
    bool attackCreatureSharedValueConfirmed{};
    std::size_t sharedObjectRefCountOffset{};
    bool attackCancelFalseConfirmed{};

    struct Field {
        std::size_t offset{};
        std::string type;
        bool valid{};
    };
    Field health, maxHealth, mana, maxMana, soul;
};

std::optional<Profile> g_profile;

struct WorldLightTargetState {
    std::uintptr_t uiMap{};
    std::uintptr_t mapView{};
    float originalAmbient{};
    bool originalSaved{};
};
std::vector<WorldLightTargetState> g_worldLightTargets;
bool g_worldLightEnabled{};
constexpr float kWorldLightHackAmbient = 1.0f;

// One-shot real Game::look capture. No breakpoint/VEH remains armed while idle.
SRWLOCK g_lookCaptureLock = SRWLOCK_INIT;
PVOID g_lookVehHandle{};
HANDLE g_lookCaptureEvent{};
volatile LONG g_lookCaptureWorkerActive{};
volatile LONG g_lookCaptureArmed{};
volatile LONG g_lookCaptureCaptured{};
std::uintptr_t g_lookCaptureAddress{};
std::uint8_t g_lookCaptureOriginalByte{};
std::uint32_t g_lookCapturedId{};
std::uintptr_t g_lookCapturedHolder{};
std::uintptr_t g_lookCapturedItem{};
ULONGLONG g_lookCapturedTick{};

SRWLOCK g_monsterCacheLock = SRWLOCK_INIT;
ULONGLONG g_monsterCacheTick{};
constexpr ULONGLONG kMonsterCacheTtlMs = 1000;

SRWLOCK g_tileCacheLock = SRWLOCK_INIT;
std::vector<std::uintptr_t> g_tileCache;

struct TileCoordCacheEntry {
    std::uintptr_t tile{};
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint8_t z{};
};
std::vector<TileCoordCacheEntry> g_tileCoordCache;

ULONGLONG g_tileCacheTick{};
std::uint32_t g_tileCacheOriginX{};
std::uint32_t g_tileCacheOriginY{};
std::uint8_t g_tileCacheOriginZ{};
std::uint64_t g_tileCacheGeneration{};
constexpr ULONGLONG kTileCacheMaxAgeMs = 60000;
constexpr ULONGLONG kTileCacheMinRefreshMs = 4000;
constexpr std::uint32_t kTileCacheMoveRefresh = 8;

std::string moduleDirectory();
std::string jsonEscape(const std::string& s);

std::filesystem::path bridgeLogPath() {
    return moduleDirectory() + "\\CromeRamoniaBridge.log";
}

void logLine(const std::string& line) {
    try {
        std::ofstream o(bridgeLogPath(), std::ios::app | std::ios::binary);
        if (o) o << line << "\r\n";
    } catch (...) {}
}

void writeStatusFile() {
    try {
        const auto path = moduleDirectory() + "\\CromeRamoniaBridge.status.txt";
        std::ofstream o(path, std::ios::trunc | std::ios::binary);
        if (!o) return;
        o << "status=" << g_status << "\r\n"
          << "pid=" << GetCurrentProcessId() << "\r\n"
          << "pipe=" << "\\\\.\\pipe\\Crome.Ramonia." << GetCurrentProcessId() << "\r\n"
          << "profileReady=" << (g_profile.has_value() ? "true" : "false") << "\r\n";
        if (!g_profileError.empty()) o << "profileError=" << g_profileError << "\r\n";
    } catch (...) {}
}

std::string readText(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open profile: " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string moduleDirectory() {
    char path[MAX_PATH]{};
    const DWORD len = GetModuleFileNameA(g_module, path, MAX_PATH);
    if (!len) return ".";
    std::string full(path, path + len);
    const auto pos = full.find_last_of("\\/");
    return pos == std::string::npos ? std::string(".") : full.substr(0, pos);
}

std::optional<std::uint64_t> integerValue(const crome::json::Value* v) {
    if (!v || v->isNull()) return std::nullopt;
    if (v->isNumber()) return static_cast<std::uint64_t>(v->number());
    if (!v->isString()) return std::nullopt;
    const auto& s = v->string();
    if (s.empty()) return std::nullopt;
    std::size_t used{};
    const auto n = std::stoull(s, &used, 0);
    if (used != s.size()) return std::nullopt;
    return n;
}

const crome::json::Value* child(const crome::json::Value* v, const char* key) {
    return v && v->isObject() ? v->get(key) : nullptr;
}

std::string stringValue(const crome::json::Value* v, const std::string& fallback = {}) {
    return v && v->isString() ? v->string() : fallback;
}

Profile::Field readField(const crome::json::Value* fields, const char* name) {
    Profile::Field out;
    const auto* f = child(fields, name);
    if (!f || !f->isObject()) return out;
    const auto off = integerValue(f->get("offset"));
    if (!off) return out;
    out.offset = static_cast<std::size_t>(*off);
    out.type = stringValue(f->get("type"));
    out.valid = !out.type.empty();
    return out;
}

Profile loadProfile(const std::string& path) {
    const auto root = crome::json::parse(readText(path));
    if (!root.isObject()) throw std::runtime_error("profile root must be object");

    Profile p;
    p.schema = stringValue(root.get("schema"));

    if (const auto* persistence = root.get("persistence"); persistence && persistence->isObject()) {
        if (const auto n = integerValue(persistence->get("profileVersion")))
            p.profileVersion = static_cast<int>(*n);
    }

    if (const auto* build = root.get("build"); build && build->isObject()) {
        p.architecture = stringValue(build->get("architecture"));
        p.timestamp = stringValue(build->get("timestamp"));
        p.sizeOfImage = stringValue(build->get("sizeOfImage"));
        p.textHash = stringValue(build->get("textHashFNV1A64"));
        if (const auto n = integerValue(build->get("timestamp")))
            p.expectedTimestamp = static_cast<std::uint32_t>(*n);
        if (const auto n = integerValue(build->get("sizeOfImage")))
            p.expectedImageSize = static_cast<std::uint32_t>(*n);
    }

    const auto* owners = root.get("owners");
    const auto game = integerValue(child(owners, "gameRva"));
    if (!game) throw std::runtime_error("owners.gameRva missing");
    p.gameRva = static_cast<std::uintptr_t>(*game);

    const auto* layout = root.get("gameLayout");
    const auto lp = integerValue(child(layout, "localPlayerOffset"));
    if (!lp) throw std::runtime_error("gameLayout.localPlayerOffset missing");
    p.localPlayerOffset = static_cast<std::size_t>(*lp);
    if (const auto n = integerValue(child(layout, "attackTargetOffset")))
        p.attackTargetOffset = static_cast<std::size_t>(*n);
    if (const auto n = integerValue(child(layout, "followTargetOffset")))
        p.followTargetOffset = static_cast<std::size_t>(*n);

    const auto* rtti = root.get("rtti");
    const auto* monster = child(rtti, "Monster");
    const auto* monsterVtables = child(monster, "vtables");
    if (monsterVtables && monsterVtables->isArray() && !monsterVtables->array().empty()) {
        const auto n = integerValue(&monsterVtables->array().front());
        if (n) p.monsterVtableRva = static_cast<std::uintptr_t>(*n);
    }

    const auto* localPlayer = child(rtti, "LocalPlayer");
    const auto* localPlayerVtables = child(localPlayer, "vtables");
    if (localPlayerVtables && localPlayerVtables->isArray() &&
        !localPlayerVtables->array().empty()) {
        const auto n=integerValue(&localPlayerVtables->array().front());
        if (n) p.localPlayerVtableRva=static_cast<std::uintptr_t>(*n);
    }

    const auto* item = child(rtti, "Item");
    const auto* itemVtables = child(item, "vtables");
    if (itemVtables && itemVtables->isArray() && !itemVtables->array().empty()) {
        const auto n = integerValue(&itemVtables->array().front());
        if (n) p.itemVtableRva = static_cast<std::uintptr_t>(*n);
    }

    const auto* container = child(rtti, "Container");
    const auto* containerVtables = child(container, "vtables");
    if (containerVtables && containerVtables->isArray() && !containerVtables->array().empty()) {
        const auto n = integerValue(&containerVtables->array().front());
        if (n) p.containerVtableRva = static_cast<std::uintptr_t>(*n);
    }

    const auto* uiMap = child(rtti, "UIMap");
    const auto* uiMapVtables = child(uiMap, "vtables");
    if (uiMapVtables && uiMapVtables->isArray() && !uiMapVtables->array().empty()) {
        const auto n = integerValue(&uiMapVtables->array().front());
        if (n) p.uiMapVtableRva = static_cast<std::uintptr_t>(*n);
    }

    const auto* mapView = child(rtti, "MapView");
    const auto* mapViewVtables = child(mapView, "vtables");
    if (mapViewVtables && mapViewVtables->isArray() && !mapViewVtables->array().empty()) {
        const auto n = integerValue(&mapViewVtables->array().front());
        if (n) p.mapViewVtableRva = static_cast<std::uintptr_t>(*n);
    }

    const auto* semanticBinds = root.get("semanticBinds");
    const auto* topMoveBind = child(semanticBinds,"getTopMoveThing");
    if(topMoveBind && topMoveBind->isObject()) {
        if(const auto n=integerValue(topMoveBind->get("methodRva")))
            p.getTopMoveThingMethodRva=static_cast<std::uintptr_t>(*n);
    }

    const auto* actions = root.get("actions");
    const auto* attack = child(actions, "attack");
    if (attack && attack->isObject()) {
        if (const auto n = integerValue(attack->get("methodRva")))
            p.attackMethodRva = static_cast<std::uintptr_t>(*n);
        p.attackRuntimeConfirmed = stringValue(attack->get("status")) == "runtime_confirmed";

        const auto* args = attack->get("args");
        if (args && args->isArray() && args->array().size() >= 2) {
            const auto& creature = args->array()[0];
            const auto& cancel = args->array()[1];

            p.attackCreatureSharedValueConfirmed =
                creature.isObject() &&
                stringValue(creature.get("semantic")) == "Creature" &&
                stringValue(creature.get("abiType")) == "shared_object_ptr_value" &&
                stringValue(creature.get("status")) == "runtime_confirmed";

            if (const auto n = integerValue(
                    creature.isObject() ? creature.get("refCountOffset") : nullptr))
                p.sharedObjectRefCountOffset = static_cast<std::size_t>(*n);

            const auto* required = cancel.isObject() ? cancel.get("observedRequiredValue") : nullptr;
            p.attackCancelFalseConfirmed =
                cancel.isObject() &&
                stringValue(cancel.get("semantic")) == "Cancel" &&
                stringValue(cancel.get("abiType")) == "bool" &&
                stringValue(cancel.get("status")) == "runtime_confirmed" &&
                required && required->isBool() && required->boolean() == false;
        }
    }


    const auto* use = child(actions, "use");
    if (use && use->isObject()) {
        if (const auto n = integerValue(use->get("methodRva")))
            p.useMethodRva = static_cast<std::uintptr_t>(*n);
        p.useRuntimeConfirmed = stringValue(use->get("status")) == "runtime_confirmed";
        const auto* args = use->get("args");
        if (args && args->isArray() && !args->array().empty()) {
            const auto& a0 = args->array()[0];
            p.useItemSharedRefConfirmed =
                a0.isObject() &&
                stringValue(a0.get("semantic")) == "Item" &&
                stringValue(a0.get("abiType")) == "shared_object_ptr_ref" &&
                stringValue(a0.get("status")) == "runtime_confirmed";
        }
    }

    const auto* look = child(actions, "look");
    if (look && look->isObject()) {
        if (const auto n=integerValue(look->get("methodRva")))
            p.lookMethodRva=static_cast<std::uintptr_t>(*n);
        p.lookRuntimeConfirmed=
            stringValue(look->get("status"))=="runtime_confirmed";

        const auto* args=look->get("args");
        if (args && args->isArray() && !args->array().empty()) {
            const auto& a0=args->array()[0];
            p.lookItemSharedRefConfirmed=
                a0.isObject() &&
                stringValue(a0.get("semantic"))=="Item" &&
                stringValue(a0.get("abiType"))=="shared_object_ptr_ref" &&
                stringValue(a0.get("status"))=="runtime_confirmed";
        }
    }

    const auto* useWith = child(actions, "useWith");
    if (useWith && useWith->isObject()) {
        if (const auto n=integerValue(useWith->get("methodRva")))
            p.useWithMethodRva=static_cast<std::uintptr_t>(*n);
        p.useWithRuntimeConfirmed=
            stringValue(useWith->get("status"))=="runtime_confirmed";

        const auto* args=useWith->get("args");
        if (args && args->isArray() && args->array().size()>=2) {
            const auto& a0=args->array()[0];
            const auto& a1=args->array()[1];
            p.useWithItemSharedRefConfirmed=
                a0.isObject() &&
                stringValue(a0.get("semantic"))=="Item" &&
                stringValue(a0.get("abiType"))=="shared_object_ptr_ref" &&
                stringValue(a0.get("status"))=="runtime_confirmed";
            p.useWithThingSharedRefConfirmed=
                a1.isObject() &&
                stringValue(a1.get("semantic"))=="Thing" &&
                stringValue(a1.get("abiType"))=="shared_object_ptr_ref" &&
                stringValue(a1.get("status"))=="runtime_confirmed";
        }
    }

    const auto* move = child(actions, "move");
    if (move && move->isObject()) {
        if (const auto n = integerValue(move->get("methodRva")))
            p.moveMethodRva = static_cast<std::uintptr_t>(*n);
        p.moveRuntimeConfirmed = stringValue(move->get("status")) == "runtime_confirmed";
        const auto* args = move->get("args");
        if (args && args->isArray() && args->array().size() >= 3) {
            const auto& a0 = args->array()[0];
            const auto& a1 = args->array()[1];
            const auto& a2 = args->array()[2];
            p.moveItemSharedRefConfirmed =
                a0.isObject() &&
                stringValue(a0.get("semantic")) == "Item" &&
                stringValue(a0.get("abiType")) == "shared_object_ptr_ref" &&
                stringValue(a0.get("status")) == "runtime_confirmed";
            p.movePositionRefConfirmed =
                a1.isObject() &&
                stringValue(a1.get("semantic")) == "Position" &&
                stringValue(a1.get("abiType")) == "position_u32_u32_u8_ref" &&
                stringValue(a1.get("status")) == "runtime_confirmed";
            p.moveCountConfirmed =
                a2.isObject() &&
                stringValue(a2.get("semantic")) == "Count" &&
                stringValue(a2.get("abiType")) == "u32" &&
                stringValue(a2.get("status")) == "runtime_confirmed";
        }
    }

    const auto* autoWalk = child(actions, "autoWalk");
    if (autoWalk && autoWalk->isObject()) {
        if (const auto n = integerValue(autoWalk->get("methodRva")))
            p.autoWalkMethodRva = static_cast<std::uintptr_t>(*n);
        p.autoWalkRuntimeConfirmed =
            stringValue(autoWalk->get("status")) == "runtime_confirmed";
        if (const auto n = integerValue(autoWalk->get("stackCleanupBytes")))
            p.autoWalkStackCleanupBytes = static_cast<std::uint32_t>(*n);

        const auto* args = autoWalk->get("args");
        if (args && args->isArray() && !args->array().empty()) {
            const auto& a0 = args->array()[0];
            p.autoWalkDirectionsVectorRefConfirmed =
                a0.isObject() &&
                stringValue(a0.get("semantic")) == "Directions" &&
                stringValue(a0.get("abiType")) == "vector_direction_u32_ref" &&
                stringValue(a0.get("status")) == "runtime_confirmed";

            if (args->array().size() >= 2) {
                const auto& a1 = args->array()[1];
                p.autoWalkStartPositionValueConfirmed =
                    a1.isObject() &&
                    stringValue(a1.get("semantic")) == "Position" &&
                    stringValue(a1.get("abiType")) == "position_u32_u32_u8_value" &&
                    stringValue(a1.get("status")) == "runtime_confirmed";
            }
        }
    }

    const auto* talkChannel = child(actions, "talkChannel");
    if (talkChannel && talkChannel->isObject()) {
        if (const auto n = integerValue(talkChannel->get("methodRva")))
            p.talkChannelMethodRva = static_cast<std::uintptr_t>(*n);
        p.talkChannelRuntimeConfirmed =
            stringValue(talkChannel->get("status")) == "runtime_confirmed";
        if (const auto n=integerValue(talkChannel->get("messageSayValue")))
            p.talkSayModeValue=static_cast<std::uint32_t>(*n);
        if (const auto n=integerValue(talkChannel->get("defaultChannel")))
            p.talkDefaultChannel=static_cast<std::uint32_t>(*n);

        const auto* args = talkChannel->get("args");
        if (args && args->isArray() && args->array().size() >= 3) {
            const auto& a0=args->array()[0];
            const auto& a1=args->array()[1];
            const auto& a2=args->array()[2];
            p.talkModeArgConfirmed =
                a0.isObject() &&
                stringValue(a0.get("semantic"))=="MessageMode" &&
                stringValue(a0.get("abiType"))=="u32" &&
                stringValue(a0.get("status"))=="runtime_confirmed";
            p.talkChannelArgConfirmed =
                a1.isObject() &&
                stringValue(a1.get("semantic"))=="Channel" &&
                stringValue(a1.get("abiType"))=="u32" &&
                stringValue(a1.get("status"))=="runtime_confirmed";
            p.talkMessageMsvcStringRefConfirmed =
                a2.isObject() &&
                stringValue(a2.get("semantic"))=="Message" &&
                stringValue(a2.get("abiType"))=="msvc_string_ref" &&
                stringValue(a2.get("status"))=="runtime_confirmed";
        }
    }

    const auto* runtime = root.get("runtime");

    const auto* worldLight = child(runtime, "worldLight");
    if (worldLight && worldLight->isObject()) {
        if (const auto n = integerValue(worldLight->get("uiMapToMapView")))
            p.uiMapToMapViewOffset = static_cast<std::size_t>(*n);

        const auto* drawLights = child(worldLight, "drawLights");
        if (const auto n = integerValue(child(drawLights, "offset")))
            p.mapViewDrawLightsOffset = static_cast<std::size_t>(*n);

        const auto* minimumAmbient = child(worldLight, "minimumAmbient");
        if (const auto n = integerValue(child(minimumAmbient, "offset")))
            p.mapViewMinimumAmbientOffset = static_cast<std::size_t>(*n);

        const auto contract = stringValue(worldLight->get("staticContractStatus"));
        p.worldLightContractReady =
            p.uiMapVtableRva &&
            p.mapViewVtableRva &&
            p.uiMapToMapViewOffset &&
            p.mapViewMinimumAmbientOffset &&
            stringValue(child(minimumAmbient, "type")) == "float" &&
            stringValue(child(drawLights, "type")) == "u8" &&
            contract == "semantic_uimap_mapview_setter_getter_contract";
    }

    const auto* position = child(runtime, "position");
    const auto x = integerValue(child(position, "x"));
    const auto y = integerValue(child(position, "y"));
    const auto z = integerValue(child(position, "z"));
    if (!x || !y || !z) throw std::runtime_error("runtime.position incomplete");
    p.posX = static_cast<std::size_t>(*x);
    p.posY = static_cast<std::size_t>(*y);
    p.posZ = static_cast<std::size_t>(*z);

    const auto* creatureLayout = child(runtime, "creatureLayout");
    const auto* creatureId = child(creatureLayout, "id");
    if (const auto n = integerValue(child(creatureId, "offset")))
        p.creatureIdOffset = static_cast<std::size_t>(*n);
    const auto* creatureName = child(creatureLayout, "name");
    if (const auto n = integerValue(child(creatureName, "offset")))
        p.creatureNameOffset = static_cast<std::size_t>(*n);
    const auto* creatureHp = child(creatureLayout, "hpPercent");
    if (const auto n = integerValue(child(creatureHp, "offset")))
        p.creatureHpPercentOffset = static_cast<std::size_t>(*n);

    const auto* fields = child(runtime, "selectedFields");
    p.health = readField(fields, "health");
    p.maxHealth = readField(fields, "maxHealth");
    p.mana = readField(fields, "mana");
    p.maxMana = readField(fields, "maxMana");
    p.soul = readField(fields, "soul");

    const auto itemId = readField(fields, "itemId");
    const auto itemCount = readField(fields, "itemCount");
    if (itemId.valid && itemCount.valid &&
        itemId.type == "u16" && itemCount.type == "u16" &&
        p.itemVtableRva) {
        p.itemIdOffset = itemId.offset;
        p.itemCountOffset = itemCount.offset;
        p.itemContractReady = true;
    }

    const auto inventoryArray = readField(fields, "inventoryArray");
    const auto inventoryStride = readField(fields, "inventoryStride");
    if (inventoryArray.valid && inventoryStride.valid &&
        inventoryArray.type == "ptr_array10" &&
        inventoryStride.type == "bytes" &&
        inventoryStride.offset == 4) {
        p.inventoryArrayOffset = inventoryArray.offset;
        p.inventoryStride = inventoryStride.offset;
        p.inventoryContractReady = true;
    }

    const auto openContainers = readField(fields, "openContainersMap");
    const auto containerId = readField(fields, "containerId");
    const auto containerCapacity = readField(fields, "containerCapacity");
    const auto containerName = readField(fields, "containerName");
    const auto containerItems = readField(fields, "containerItems");
    const auto containerItemStride = readField(fields, "containerItemStride");
    if (openContainers.valid && containerId.valid && containerCapacity.valid &&
        containerName.valid && containerItems.valid && containerItemStride.valid &&
        openContainers.type == "inline_registry" &&
        containerId.type == "u32" &&
        containerCapacity.type == "u32" &&
        containerName.type == "msvc_string" &&
        containerItems.type == "deque" &&
        containerItemStride.type == "bytes" &&
        containerItemStride.offset == 4 &&
        p.containerVtableRva) {
        p.openContainersOffset = openContainers.offset;
        p.containerIdOffset = containerId.offset;
        p.containerCapacityOffset = containerCapacity.offset;
        p.containerNameOffset = containerName.offset;
        p.containerItemsOffset = containerItems.offset;
        p.containerItemStride = containerItemStride.offset;
        p.containerContractReady = true;
    }

    const auto tileThings = readField(fields, "tileThings");
    const auto tileThingsEnd = readField(fields, "tileThingsEnd");
    const auto tileThingsCap = readField(fields, "tileThingsCap");
    const auto tileThingsElem = readField(fields, "tileThingsElem");
    const auto tileVtable = readField(fields, "tileVtableRva");
    if (tileThings.valid && tileThingsEnd.valid && tileThingsCap.valid &&
        tileThingsElem.valid && tileVtable.valid) {
        p.tileThingsOffset = tileThings.offset;
        p.tileThingsEndOffset = tileThingsEnd.offset;
        p.tileThingsCapOffset = tileThingsCap.offset;
        p.tileThingsElemSize = tileThingsElem.offset;
        p.tileVtableRva = tileVtable.offset;
        p.tileThingsRuntimeConfirmed =
            tileThings.type.find("vector") != std::string::npos &&
            tileThingsElem.type == "bytes" &&
            tileVtable.type == "rva" &&
            p.tileThingsElemSize == 4;
    }
    if (!p.health.valid || !p.maxHealth.valid || !p.mana.valid || !p.maxMana.valid)
        throw std::runtime_error("required player fields missing");
    if (!p.expectedTimestamp || !p.expectedImageSize)
        throw std::runtime_error("build fingerprint incomplete");
    if (!p.monsterVtableRva || !p.creatureIdOffset || !p.creatureNameOffset ||
        !p.creatureHpPercentOffset || !p.attackMethodRva ||
        !p.attackRuntimeConfirmed || !p.attackCreatureSharedValueConfirmed ||
        !p.sharedObjectRefCountOffset || !p.attackCancelFalseConfirmed)
        throw std::runtime_error("V39 target/attack ownership contract incomplete");

    return p;
}

std::string findProfilePath() {
    return moduleDirectory() + "\\profiles\\ramonia_x86\\6AB03F94_E281E9CA5BA0AF2.json";
}

template<class T>
bool safeRead(std::uintptr_t address, T& out) {
    SIZE_T got{};
    return ReadProcessMemory(GetCurrentProcess(),
        reinterpret_cast<LPCVOID>(address), &out, sizeof(out), &got) && got == sizeof(out);
}

bool readNumber(std::uintptr_t address, const std::string& type, double& out) {
    if (type == "double") { double v{}; if (!safeRead(address, v)) return false; out = v; return true; }
    if (type == "float") { float v{}; if (!safeRead(address, v)) return false; out = v; return true; }
    if (type == "u32") { std::uint32_t v{}; if (!safeRead(address, v)) return false; out = v; return true; }
    if (type == "i32") { std::int32_t v{}; if (!safeRead(address, v)) return false; out = v; return true; }
    if (type == "u16") { std::uint16_t v{}; if (!safeRead(address, v)) return false; out = v; return true; }
    if (type == "u8") { std::uint8_t v{}; if (!safeRead(address, v)) return false; out = v; return true; }
    return false;
}

bool isExecutableAddress(std::uintptr_t address) {
    if (!address) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) || (mbi.Protect & PAGE_NOACCESS))
        return false;
    const DWORD p = mbi.Protect & 0xFFu;
    return p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ ||
           p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}

bool isWritableAddress(std::uintptr_t address) {
    if (!address) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) || (mbi.Protect & PAGE_NOACCESS))
        return false;
    const DWORD p = mbi.Protect & 0xFFu;
    return p == PAGE_READWRITE || p == PAGE_WRITECOPY ||
           p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}

template<class T>
bool safeWrite(std::uintptr_t address, const T& value) {
    if (!isWritableAddress(address)) return false;
    SIZE_T written{};
    return WriteProcessMemory(
               GetCurrentProcess(),
               reinterpret_cast<LPVOID>(address),
               &value,
               sizeof(value),
               &written) &&
           written == sizeof(value);
}

bool readCurrentImageFingerprint() {
    const auto* base = reinterpret_cast<const std::uint8_t*>(g_base);
    if (!base) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    g_imageTimestamp = nt->FileHeader.TimeDateStamp;
    g_imageSize = nt->OptionalHeader.SizeOfImage;
    return true;
}

struct WindowSearch {
    DWORD pid{};
    HWND hwnd{};
};

BOOL CALLBACK enumCurrentProcessWindow(HWND hwnd, LPARAM lp) {
    auto* d = reinterpret_cast<WindowSearch*>(lp);
    DWORD pid{};
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != d->pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr)
        return TRUE;
    d->hwnd = hwnd;
    return FALSE;
}

HWND findCurrentProcessWindow() {
    WindowSearch data{GetCurrentProcessId(), nullptr};
    EnumWindows(&enumCurrentProcessWindow, reinterpret_cast<LPARAM>(&data));
    return data.hwnd;
}

struct MovePosition {
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint8_t z{};
    std::uint8_t pad[3]{};
};
static_assert(sizeof(MovePosition) == 12, "MovePosition ABI must be 12 bytes");

#if defined(__GNUC__)
typedef void (__attribute__((thiscall)) *GameAttackFn)(void*, void*, bool);
typedef void (__attribute__((thiscall)) *GameUseFn)(void*, void*);
typedef void (__attribute__((thiscall)) *GameUseWithFn)(void*, void*, void*);
typedef void (__attribute__((thiscall)) *GameMoveFn)(void*, void*, const MovePosition*, std::uint32_t);
typedef void (__attribute__((thiscall)) *GameTalkChannelFn)(void*, std::uint32_t, std::uint32_t, const void*);
typedef void (__attribute__((thiscall)) *GameAutoWalkFn)(void*, const void*, MovePosition);
typedef void (__attribute__((thiscall)) *TileGetTopMoveThingFn)(void*, void*);
#else
typedef void (__thiscall *GameAttackFn)(void*, void*, bool);
typedef void (__thiscall *GameUseFn)(void*, void*);
typedef void (__thiscall *GameUseWithFn)(void*, void*, void*);
typedef void (__thiscall *GameMoveFn)(void*, void*, const MovePosition*, std::uint32_t);
typedef void (__thiscall *GameTalkChannelFn)(void*, std::uint32_t, std::uint32_t, const void*);
typedef void (__thiscall *GameAutoWalkFn)(void*, const void*, MovePosition);
typedef void (__thiscall *TileGetTopMoveThingFn)(void*, void*);
#endif

struct MsvcSmallString32 {
    char storage[16]{};
    std::uint32_t size{};
    std::uint32_t capacity{15};
};
static_assert(sizeof(MsvcSmallString32)==24,"MSVC x86 string SSO ABI must be 24 bytes");

struct MsvcVectorU32Ref32 {
    std::uint32_t* first{};
    std::uint32_t* last{};
    std::uint32_t* end{};
};
static_assert(sizeof(MsvcVectorU32Ref32)==12,"MSVC x86 vector ABI must be 12 bytes");

bool targetSnapshot(std::uintptr_t target, std::uint32_t& id) {
    id = 0;
    if (!g_profile || !target) return false;
    std::uintptr_t vtable{};
    if (!safeRead(target, vtable) || vtable != g_base + g_profile->monsterVtableRva)
        return false;
    return safeRead(target + g_profile->creatureIdOffset, id) && id != 0;
}

struct VisibleMonster {
    std::uintptr_t address{};
    std::uint32_t id{};
    std::string name;
    std::uint8_t hpPercent{};
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint8_t z{};
    std::uint32_t distance{};
};

std::vector<VisibleMonster> g_monsterCache;

std::vector<VisibleMonster> copyMonsterCache() {
    AcquireSRWLockShared(&g_monsterCacheLock);
    auto copy = g_monsterCache;
    ReleaseSRWLockShared(&g_monsterCacheLock);
    return copy;
}

void storeMonsterCache(std::vector<VisibleMonster> monsters) {
    AcquireSRWLockExclusive(&g_monsterCacheLock);
    g_monsterCache = std::move(monsters);
    g_monsterCacheTick = GetTickCount64();
    ReleaseSRWLockExclusive(&g_monsterCacheLock);
}

std::optional<std::uintptr_t> cachedMonsterAddressById(std::uint32_t id) {
    if (!id) return std::nullopt;
    AcquireSRWLockShared(&g_monsterCacheLock);
    std::optional<std::uintptr_t> result;
    for (const auto& m : g_monsterCache) {
        if (m.id == id) { result = m.address; break; }
    }
    ReleaseSRWLockShared(&g_monsterCacheLock);
    return result;
}

bool monsterCacheFresh() {
    AcquireSRWLockShared(&g_monsterCacheLock);
    const auto tick = g_monsterCacheTick;
    ReleaseSRWLockShared(&g_monsterCacheLock);
    return tick && (GetTickCount64() - tick) < kMonsterCacheTtlMs;
}

std::string safeReadInlineString(std::uintptr_t address, std::size_t maxLen = 64) {
    std::string out;
    out.reserve(maxLen);
    for (std::size_t i = 0; i < maxLen; ++i) {
        char c{};
        if (!safeRead(address + i, c)) return {};
        if (c == '\0') break;
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || uc > 0x7E) return {};
        out.push_back(c);
    }
    return out;
}

bool readableProtection(DWORD protect) {
    if (protect & PAGE_GUARD) return false;
    if ((protect & 0xFFu) == PAGE_NOACCESS) return false;
    const DWORD p = protect & 0xFFu;
    return p == PAGE_READONLY || p == PAGE_READWRITE || p == PAGE_WRITECOPY ||
           p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE ||
           p == PAGE_EXECUTE_WRITECOPY;
}

bool localPlayerPosition(std::uint32_t& x, std::uint32_t& y, std::uint8_t& z) {
    x = y = 0; z = 0;
    if (!g_profile) return false;
    const auto game = g_base + g_profile->gameRva;
    std::uintptr_t player{};
    if (!safeRead(game + g_profile->localPlayerOffset, player) || !player) return false;
    return safeRead(player + g_profile->posX, x) &&
           safeRead(player + g_profile->posY, y) &&
           safeRead(player + g_profile->posZ, z) &&
           x > 0 && y > 0 && z <= 15;
}

std::vector<VisibleMonster> scanVisibleMonsters(std::uint32_t maxRange = 15) {
    std::vector<VisibleMonster> out;
    if (!g_profile) return out;

    std::uint32_t px{}, py{};
    std::uint8_t pz{};
    if (!localPlayerPosition(px, py, pz)) return out;

    const std::uint32_t wantedVtable =
        static_cast<std::uint32_t>(g_base + g_profile->monsterVtableRva);

    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    auto address = reinterpret_cast<std::uintptr_t>(si.lpMinimumApplicationAddress);
    const auto maxAddress = reinterpret_cast<std::uintptr_t>(si.lpMaximumApplicationAddress);

    constexpr SIZE_T kChunk = 1u << 20;
    std::vector<std::uint8_t> buffer(kChunk);

    while (address < maxAddress) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi))) break;

        const auto regionBase = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const auto regionEnd = regionBase + mbi.RegionSize;
        if (regionEnd <= address) break;

        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
            readableProtection(mbi.Protect)) {
            for (auto chunkBase = regionBase; chunkBase < regionEnd; ) {
                const SIZE_T want = static_cast<SIZE_T>(
                    std::min<std::uintptr_t>(kChunk, regionEnd - chunkBase));
                SIZE_T got{};
                if (ReadProcessMemory(GetCurrentProcess(),
                                      reinterpret_cast<LPCVOID>(chunkBase),
                                      buffer.data(), want, &got) && got >= sizeof(std::uint32_t)) {
                    for (SIZE_T i = 0; i + sizeof(std::uint32_t) <= got; i += 4) {
                        std::uint32_t vt{};
                        std::memcpy(&vt, buffer.data() + i, sizeof(vt));
                        if (vt != wantedVtable) continue;

                        const auto candidate = chunkBase + i;
                        std::uint32_t id{}, x{}, y{};
                        std::uint8_t z{}, hpPercent{};
                        if (!safeRead(candidate + g_profile->creatureIdOffset, id) || !id ||
                            !safeRead(candidate + g_profile->posX, x) ||
                            !safeRead(candidate + g_profile->posY, y) ||
                            !safeRead(candidate + g_profile->posZ, z) ||
                            !safeRead(candidate + g_profile->creatureHpPercentOffset, hpPercent)) {
                            continue;
                        }

                        const auto name = safeReadInlineString(
                            candidate + g_profile->creatureNameOffset, 48);

                        if (!x || !y || z != pz || hpPercent > 100 || name.empty()) continue;
                        const auto dx = x > px ? x - px : px - x;
                        const auto dy = y > py ? y - py : py - y;
                        const auto dist = std::max(dx, dy);
                        if (dist > maxRange) continue;

                        bool duplicate = false;
                        for (const auto& m : out) {
                            if (m.address == candidate) { duplicate = true; break; }
                        }
                        if (!duplicate)
                            out.push_back({candidate, id, name, hpPercent, x, y, z, dist});
                    }
                }
                chunkBase += want ? want : 1;
            }
        }

        address = regionEnd;
    }

    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        if (a.distance != b.distance) return a.distance < b.distance;
        return a.id < b.id;
    });
    return out;
}


struct TileMonsterDiagnostic {
    VisibleMonster monster;
    std::uintptr_t tileAddress{};
    std::uint32_t tileThingCount{};
    bool tileContainsLocalPlayer{};
};

bool readTileVectorHeader(std::uintptr_t tile,
                          std::uint32_t& begin,
                          std::uint32_t& end,
                          std::uint32_t& cap,
                          std::uint32_t& count) {
    begin = end = cap = count = 0;
    if (!g_profile || !g_profile->tileThingsRuntimeConfirmed || !tile) return false;

    const auto& p = *g_profile;
    std::uintptr_t vt{};
    if (!safeRead(tile, vt) || vt != g_base + p.tileVtableRva) return false;

    if (!safeRead(tile + p.tileThingsOffset, begin) ||
        !safeRead(tile + p.tileThingsEndOffset, end) ||
        !safeRead(tile + p.tileThingsCapOffset, cap))
        return false;

    if (p.tileThingsElemSize != 4) return false;

    // A default-constructed empty std::vector is allowed to be all zeros.
    if (!begin && !end && !cap) {
        count = 0;
        return true;
    }

    if (!begin || end < begin || cap < end) return false;
    const std::uint32_t bytes = end - begin;
    const std::uint32_t capBytes = cap - begin;
    if (bytes % 4 || capBytes % 4) return false;

    count = bytes / 4;
    const std::uint32_t capacity = capBytes / 4;
    return count <= 80 && capacity >= count && capacity <= 160;
}

bool readStableTileThings(std::uintptr_t tile,
                          std::vector<std::uintptr_t>& things,
                          bool& containsLocalPlayer) {
    things.clear();
    containsLocalPlayer = false;
    if (!g_profile || !g_profile->tileThingsRuntimeConfirmed || !tile) return false;

    const auto& p = *g_profile;
    std::uint32_t begin1{}, end1{}, cap1{}, count1{};
    if (!readTileVectorHeader(tile, begin1, end1, cap1, count1)) return false;
    if (count1 == 0) return true;

    std::vector<std::uint32_t> raw(count1);
    SIZE_T got{};
    if (!ReadProcessMemory(GetCurrentProcess(),
                           reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(begin1)),
                           raw.data(), raw.size() * sizeof(std::uint32_t), &got) ||
        got != raw.size() * sizeof(std::uint32_t))
        return false;

    // Reject the snapshot if the vector changed while its pointer array was copied.
    std::uint32_t begin2{}, end2{}, cap2{}, count2{};
    if (!readTileVectorHeader(tile, begin2, end2, cap2, count2) ||
        begin1 != begin2 || end1 != end2 || cap1 != cap2 || count1 != count2)
        return false;

    std::uintptr_t localPlayer{};
    const auto game = g_base + p.gameRva;
    safeRead(game + p.localPlayerOffset, localPlayer);

    things.reserve(count1);
    for (const auto rawPtr : raw) {
        const auto thing = static_cast<std::uintptr_t>(rawPtr);
        if (!thing) return false;

        std::uintptr_t thingVtable{};
        if (!safeRead(thing, thingVtable) ||
            thingVtable < g_base || thingVtable >= g_base + g_imageSize)
            return false;

        if (localPlayer && thing == localPlayer) containsLocalPlayer = true;
        things.push_back(thing);
    }
    return true;
}

std::vector<std::uintptr_t> discoverTileAddresses() {
    std::vector<std::uintptr_t> tiles;
    if (!g_profile || !g_profile->tileThingsRuntimeConfirmed) return tiles;

    const std::uint32_t wantedTileVtable =
        static_cast<std::uint32_t>(g_base + g_profile->tileVtableRva);

    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    auto address = reinterpret_cast<std::uintptr_t>(si.lpMinimumApplicationAddress);
    const auto maxAddress = reinterpret_cast<std::uintptr_t>(si.lpMaximumApplicationAddress);

    constexpr SIZE_T kChunk = 1u << 20;
    std::vector<std::uint8_t> buffer(kChunk);

    while (address < maxAddress) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi))) break;

        const auto regionBase = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const auto regionEnd = regionBase + mbi.RegionSize;
        if (regionEnd <= address) break;

        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
            readableProtection(mbi.Protect)) {
            for (auto chunkBase = regionBase; chunkBase < regionEnd; ) {
                const SIZE_T want = static_cast<SIZE_T>(
                    std::min<std::uintptr_t>(kChunk, regionEnd - chunkBase));
                SIZE_T got{};
                if (ReadProcessMemory(GetCurrentProcess(),
                                      reinterpret_cast<LPCVOID>(chunkBase),
                                      buffer.data(), want, &got) &&
                    got >= sizeof(std::uint32_t)) {
                    for (SIZE_T i = 0; i + sizeof(std::uint32_t) <= got; i += 4) {
                        std::uint32_t vt{};
                        std::memcpy(&vt, buffer.data() + i, sizeof(vt));
                        if (vt != wantedTileVtable) continue;

                        const auto tile = chunkBase + i;
                        std::uint32_t begin{}, finish{}, cap{}, count{};
                        if (!readTileVectorHeader(tile, begin, finish, cap, count))
                            continue;
                        tiles.push_back(tile);
                    }
                }
                chunkBase += want ? want : 1;
            }
        }
        address = regionEnd;
    }

    std::sort(tiles.begin(), tiles.end());
    tiles.erase(std::unique(tiles.begin(), tiles.end()), tiles.end());
    return tiles;
}

std::vector<std::uintptr_t> copyTileCache() {
    AcquireSRWLockShared(&g_tileCacheLock);
    auto copy = g_tileCache;
    ReleaseSRWLockShared(&g_tileCacheLock);
    return copy;
}

struct TileCacheInfo {
    std::size_t count{};
    ULONGLONG ageMs{};
    std::uint32_t originX{};
    std::uint32_t originY{};
    std::uint8_t originZ{};
    std::uint64_t generation{};
};

TileCacheInfo tileCacheInfo() {
    TileCacheInfo info;
    AcquireSRWLockShared(&g_tileCacheLock);
    info.count = g_tileCache.size();
    info.originX = g_tileCacheOriginX;
    info.originY = g_tileCacheOriginY;
    info.originZ = g_tileCacheOriginZ;
    info.generation = g_tileCacheGeneration;
    const auto tick = g_tileCacheTick;
    ReleaseSRWLockShared(&g_tileCacheLock);
    info.ageMs = tick ? GetTickCount64() - tick : 0;
    return info;
}

std::vector<TileCoordCacheEntry> buildTileCoordCache(
    const std::vector<std::uintptr_t>& tiles) {

    std::vector<TileCoordCacheEntry> out;
    if (!g_profile || !g_profile->itemContractReady ||
        !g_profile->itemVtableRva)
        return out;

    const auto wantedItemVtable = g_base + g_profile->itemVtableRva;
    out.reserve(tiles.size());

    // Build the spatial index only when the broad Tile cache itself refreshes.
    // Normal pickup polling can then address only the player's 3x3 area.
    for (const auto tile : tiles) {
        std::vector<std::uintptr_t> things;
        bool containsLocalPlayer=false;
        if (!readStableTileThings(tile,things,containsLocalPlayer))
            continue;

        for (const auto thing:things) {
            std::uintptr_t vt{};
            if (!safeRead(thing,vt) || vt!=wantedItemVtable)
                continue;

            std::uint32_t x{},y{};
            std::uint8_t z{};
            if (!safeRead(thing+g_profile->posX,x) ||
                !safeRead(thing+g_profile->posY,y) ||
                !safeRead(thing+g_profile->posZ,z) ||
                !x || !y || z>15u)
                continue;

            out.push_back({tile,x,y,z});
            break;
        }
    }

    return out;
}

std::vector<std::uintptr_t> copyNearbyTileCache(
    std::uint32_t px,
    std::uint32_t py,
    std::uint8_t pz,
    std::uint32_t radius) {

    std::vector<std::uintptr_t> out;
    AcquireSRWLockShared(&g_tileCacheLock);
    for (const auto& entry:g_tileCoordCache) {
        if (entry.z!=pz) continue;
        const auto dx=entry.x>px ? entry.x-px : px-entry.x;
        const auto dy=entry.y>py ? entry.y-py : py-entry.y;
        if (dx<=radius && dy<=radius)
            out.push_back(entry.tile);
    }
    ReleaseSRWLockShared(&g_tileCacheLock);
    return out;
}

bool refreshTileCache(std::uint32_t px, std::uint32_t py, std::uint8_t pz) {
    const auto discovered = discoverTileAddresses();
    if (discovered.empty()) return false;
    const auto coordinates = buildTileCoordCache(discovered);

    AcquireSRWLockExclusive(&g_tileCacheLock);
    g_tileCache = discovered;
    g_tileCoordCache = coordinates;
    g_tileCacheTick = GetTickCount64();
    g_tileCacheOriginX = px;
    g_tileCacheOriginY = py;
    g_tileCacheOriginZ = pz;
    ++g_tileCacheGeneration;
    ReleaseSRWLockExclusive(&g_tileCacheLock);

    logLine("[TILE CACHE] refreshed tiles=" + std::to_string(discovered.size()) +
            " origin=" + std::to_string(px) + "," + std::to_string(py) + "," +
            std::to_string(static_cast<unsigned>(pz)));
    return true;
}

void invalidateTileCacheForFloorChange(
    std::uint32_t px,
    std::uint32_t py,
    std::uint8_t pz) {

    AcquireSRWLockExclusive(&g_tileCacheLock);
    g_tileCache.clear();
    g_tileCoordCache.clear();
    g_tileCacheTick = 0;
    g_tileCacheOriginX = px;
    g_tileCacheOriginY = py;
    g_tileCacheOriginZ = pz;
    ++g_tileCacheGeneration;
    ReleaseSRWLockExclusive(&g_tileCacheLock);

    logLine("[TILE CACHE] invalidated immediately on floor change -> z=" +
            std::to_string(static_cast<unsigned>(pz)));
}

bool ensureTileCache(std::uint32_t px, std::uint32_t py, std::uint8_t pz,
                     bool force = false) {
    const auto info = tileCacheInfo();
    const auto dx = px > info.originX ? px - info.originX : info.originX - px;
    const auto dy = py > info.originY ? py - info.originY : info.originY - py;
    const auto moved = std::max(dx, dy);

    // Floor changes are special: never reuse Tile* addresses from the
    // previous z, even briefly. Invalidate immediately and let the next poll
    // rebuild the cache after the client has had time to settle.
    if (info.count != 0 && info.originZ != pz) {
        invalidateTileCacheForFloorChange(px, py, pz);
        return false;
    }

    bool refresh = force || info.count == 0;
    if (!refresh && info.ageMs >= kTileCacheMaxAgeMs) refresh = true;
    if (!refresh && moved >= kTileCacheMoveRefresh &&
        info.ageMs >= kTileCacheMinRefreshMs)
        refresh = true;

    return !refresh || refreshTileCache(px, py, pz);
}

std::optional<std::uintptr_t> cachedTileAtPosition(
    std::uint32_t x,
    std::uint32_t y,
    std::uint8_t z,
    bool forceRefresh) {

    std::uint32_t px{},py{};
    std::uint8_t pz{};
    if (!localPlayerPosition(px,py,pz) ||
        !ensureTileCache(px,py,pz,forceRefresh))
        return std::nullopt;

    std::optional<std::uintptr_t> found;
    AcquireSRWLockShared(&g_tileCacheLock);
    for (const auto& entry:g_tileCoordCache) {
        if (entry.x==x && entry.y==y && entry.z==z) {
            found=entry.tile;
            break;
        }
    }
    ReleaseSRWLockShared(&g_tileCacheLock);
    return found;
}

bool snapshotMonsterFromThing(std::uintptr_t thing,
                              std::uint32_t px, std::uint32_t py, std::uint8_t pz,
                              std::uint32_t maxRange,
                              VisibleMonster& out) {
    out = {};
    if (!g_profile || !thing) return false;

    std::uintptr_t thingVtable{};
    if (!safeRead(thing, thingVtable) ||
        thingVtable != g_base + g_profile->monsterVtableRva)
        return false;

    std::uint32_t id{}, x{}, y{};
    std::uint8_t z{}, hpPercent{};
    if (!safeRead(thing + g_profile->creatureIdOffset, id) || !id ||
        !safeRead(thing + g_profile->posX, x) ||
        !safeRead(thing + g_profile->posY, y) ||
        !safeRead(thing + g_profile->posZ, z) ||
        !safeRead(thing + g_profile->creatureHpPercentOffset, hpPercent))
        return false;

    if (!x || !y || z != pz || hpPercent > 100) return false;
    const auto name = safeReadInlineString(
        thing + g_profile->creatureNameOffset, 48);
    if (name.empty()) return false;

    const auto dx = x > px ? x - px : px - x;
    const auto dy = y > py ? y - py : py - y;
    const auto dist = std::max(dx, dy);
    if (dist > maxRange) return false;

    out = {thing, id, name, hpPercent, x, y, z, dist};
    return true;
}

std::vector<TileMonsterDiagnostic> scanCachedTileMonsters(
    std::uint32_t maxRange = 15,
    bool forceTileRefresh = false) {
    std::vector<TileMonsterDiagnostic> out;
    if (!g_profile || !g_profile->tileThingsRuntimeConfirmed) return out;

    std::uint32_t px{}, py{};
    std::uint8_t pz{};
    if (!localPlayerPosition(px, py, pz)) return out;
    if (!ensureTileCache(px, py, pz, forceTileRefresh)) return out;

    const auto tiles = copyTileCache();
    for (const auto tile : tiles) {
        std::vector<std::uintptr_t> things;
        bool containsLocalPlayer = false;
        if (!readStableTileThings(tile, things, containsLocalPlayer))
            continue;

        for (const auto thing : things) {
            VisibleMonster monster;
            if (!snapshotMonsterFromThing(thing, px, py, pz, maxRange, monster))
                continue;

            bool duplicate = false;
            for (const auto& existing : out) {
                if (existing.monster.address == monster.address ||
                    existing.monster.id == monster.id) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;

            TileMonsterDiagnostic d;
            d.monster = std::move(monster);
            d.tileAddress = tile;
            d.tileThingCount = static_cast<std::uint32_t>(things.size());
            d.tileContainsLocalPlayer = containsLocalPlayer;
            out.push_back(std::move(d));
        }
    }

    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        if (a.monster.distance != b.monster.distance)
            return a.monster.distance < b.monster.distance;
        return a.monster.id < b.monster.id;
    });
    return out;
}

std::uintptr_t resolveTileMonsterById(std::uint32_t id) {
    if (!id || !g_profile || !g_profile->tileThingsRuntimeConfirmed) return 0;

    std::uint32_t px{}, py{};
    std::uint8_t pz{};
    if (!localPlayerPosition(px, py, pz)) return 0;

    // Never perform a broad discovery scan from the game thread.
    // ATTACK_ID can use only the current Tile cache and revalidates the Thing
    // membership immediately before Game::attack.
    const auto tiles = copyTileCache();
    for (const auto tile : tiles) {
        std::vector<std::uintptr_t> things;
        bool containsLocalPlayer = false;
        if (!readStableTileThings(tile, things, containsLocalPlayer))
            continue;

        for (const auto thing : things) {
            VisibleMonster monster;
            if (!snapshotMonsterFromThing(thing, px, py, pz, 15, monster))
                continue;
            if (monster.id == id) return thing;
        }
    }
    return 0;
}

std::vector<TileMonsterDiagnostic> scanTileMonstersReadOnly(std::uint32_t maxRange = 15) {
    return scanCachedTileMonsters(maxRange);
}

struct TileItemDiagnostic {
    std::uintptr_t address{};
    std::uintptr_t tileAddress{};
    std::uint16_t id{};
    std::uint16_t count{};
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint8_t z{};
    std::uint32_t tileSlot{};
    std::uint32_t stackIndex{};
    std::uint32_t tileThingCount{};
};

struct LootTargetTileFrame {
    std::uintptr_t tileAddress{};
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint8_t z{};
    ULONGLONG tick{};
    std::vector<TileItemDiagnostic> items;
};

struct LootBaselineState {
    bool valid{};
    std::uint64_t tileGeneration{};
    std::uint32_t trackedTargetId{};
    std::uintptr_t trackedTargetTile{};
    std::vector<LootTargetTileFrame> targetTileHistory;
    std::vector<std::uintptr_t> tiles;
    std::vector<TileItemDiagnostic> items;
    std::vector<TileMonsterDiagnostic> monsters;
};

SRWLOCK g_lootBaselineLock = SRWLOCK_INIT;
LootBaselineState g_lootBaseline;

SRWLOCK g_lootCandidateLock = SRWLOCK_INIT;
std::vector<TileItemDiagnostic> g_lastLootCandidates;

bool safeCopyBytes(std::uintptr_t address, void* out, std::size_t size) {
    if (!address || !out || !size) return false;
    SIZE_T got{};
    return ReadProcessMemory(GetCurrentProcess(),
                             reinterpret_cast<LPCVOID>(address),
                             out, size, &got) && got == size;
}

std::string hexBytes(const std::uint8_t* data,std::size_t size) {
    static const char* kHex="0123456789ABCDEF";
    std::string out;
    out.reserve(size*2);
    for(std::size_t i=0;i<size;++i) {
        const auto v=data[i];
        out.push_back(kHex[(v>>4)&0xF]);
        out.push_back(kHex[v&0xF]);
    }
    return out;
}

bool moduleTextRange(std::uintptr_t& start,std::size_t& size) {
    start=0;
    size=0;
    if(!g_base) return false;

    IMAGE_DOS_HEADER dos{};
    if(!safeRead(g_base,dos) || dos.e_magic!=IMAGE_DOS_SIGNATURE)
        return false;

    IMAGE_NT_HEADERS32 nt{};
    const auto ntAddress=g_base+static_cast<std::uintptr_t>(dos.e_lfanew);
    if(!safeCopyBytes(ntAddress,&nt,sizeof(nt)) ||
       nt.Signature!=IMAGE_NT_SIGNATURE)
        return false;

    const auto sectionBase=
        ntAddress+offsetof(IMAGE_NT_HEADERS32,OptionalHeader)+
        nt.FileHeader.SizeOfOptionalHeader;

    for(std::uint16_t i=0;i<nt.FileHeader.NumberOfSections;++i) {
        IMAGE_SECTION_HEADER sh{};
        if(!safeCopyBytes(
                sectionBase+static_cast<std::uintptr_t>(i)*sizeof(sh),
                &sh,sizeof(sh)))
            return false;

        char name[9]{};
        std::memcpy(name,sh.Name,8);
        if(std::string(name)!=".text")
            continue;

        start=g_base+sh.VirtualAddress;
        size=static_cast<std::size_t>(
            std::max(sh.Misc.VirtualSize,sh.SizeOfRawData));
        return start!=0 && size!=0;
    }
    return false;
}

bool looksHeapPointer(std::uintptr_t address) {
    if (address < 0x10000u || address >= 0x7FFF0000u) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)))
        return false;
    return mbi.State == MEM_COMMIT &&
           (mbi.Type == MEM_PRIVATE || mbi.Type == MEM_MAPPED) &&
           readableProtection(mbi.Protect);
}

bool exactObjectVtable(std::uintptr_t object, std::uintptr_t vtableRva) {
    if (!object || !vtableRva) return false;
    std::uintptr_t vt{};
    return safeRead(object, vt) && vt == g_base + vtableRva;
}

bool validateWorldLightTarget(std::uintptr_t uiMap,
                              std::uintptr_t& mapViewOut,
                              float* ambientOut = nullptr) {
    mapViewOut = 0;
    if (!g_profile || !g_profile->worldLightContractReady ||
        !uiMap || !exactObjectVtable(uiMap,g_profile->uiMapVtableRva))
        return false;

    std::uintptr_t mapView{};
    if (!safeRead(uiMap + g_profile->uiMapToMapViewOffset,mapView) ||
        !mapView ||
        !exactObjectVtable(mapView,g_profile->mapViewVtableRva))
        return false;

    float ambient{};
    if (!safeRead(mapView + g_profile->mapViewMinimumAmbientOffset,ambient) ||
        !std::isfinite(ambient) || ambient < -0.01f || ambient > 2.0f)
        return false;

    if (g_profile->mapViewDrawLightsOffset) {
        std::uint8_t draw{};
        if (!safeRead(mapView + g_profile->mapViewDrawLightsOffset,draw) || draw > 1u)
            return false;
    }

    mapViewOut=mapView;
    if (ambientOut) *ambientOut=ambient;
    return true;
}

std::vector<WorldLightTargetState> discoverWorldLightTargets() {
    std::vector<WorldLightTargetState> found;
    if (!g_profile || !g_profile->worldLightContractReady)
        return found;

    const std::uint32_t wantedUiMapVtable =
        static_cast<std::uint32_t>(g_base + g_profile->uiMapVtableRva);

    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    auto address = reinterpret_cast<std::uintptr_t>(si.lpMinimumApplicationAddress);
    const auto maxAddress =
        reinterpret_cast<std::uintptr_t>(si.lpMaximumApplicationAddress);

    constexpr SIZE_T kChunk = 1u << 20;
    std::vector<std::uint8_t> buffer(kChunk);

    while (address < maxAddress) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(address),&mbi,sizeof(mbi)))
            break;

        const auto regionBase=
            reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const auto regionEnd=regionBase+mbi.RegionSize;
        if (regionEnd<=address) break;

        if (mbi.State==MEM_COMMIT &&
            mbi.Type==MEM_PRIVATE &&
            readableProtection(mbi.Protect)) {

            for (auto chunkBase=regionBase; chunkBase<regionEnd;) {
                const SIZE_T want=static_cast<SIZE_T>(
                    std::min<std::uintptr_t>(kChunk,regionEnd-chunkBase));
                SIZE_T got{};
                if (ReadProcessMemory(
                        GetCurrentProcess(),
                        reinterpret_cast<LPCVOID>(chunkBase),
                        buffer.data(),want,&got) &&
                    got>=sizeof(std::uint32_t)) {

                    for (SIZE_T i=0;i+sizeof(std::uint32_t)<=got;i+=4) {
                        std::uint32_t vt{};
                        std::memcpy(&vt,buffer.data()+i,sizeof(vt));
                        if (vt!=wantedUiMapVtable) continue;

                        const auto uiMap=chunkBase+i;
                        std::uintptr_t mapView{};
                        float ambient{};
                        if (!validateWorldLightTarget(uiMap,mapView,&ambient))
                            continue;

                        bool duplicate=false;
                        for (const auto& existing:found)
                            if (existing.mapView==mapView) {
                                duplicate=true;
                                break;
                            }
                        if (duplicate) continue;

                        WorldLightTargetState state;
                        state.uiMap=uiMap;
                        state.mapView=mapView;
                        state.originalAmbient=ambient;
                        state.originalSaved=false;
                        found.push_back(state);
                    }
                }
                chunkBase += want ? want : 1;
            }
        }
        address=regionEnd;
    }

    return found;
}

bool ensureWorldLightTargets() {
    if (!g_profile || !g_profile->worldLightContractReady)
        return false;

    bool valid=!g_worldLightTargets.empty();
    if (valid) {
        for (const auto& target:g_worldLightTargets) {
            std::uintptr_t currentMapView{};
            if (!validateWorldLightTarget(target.uiMap,currentMapView,nullptr) ||
                currentMapView!=target.mapView) {
                valid=false;
                break;
            }
        }
    }

    if (!valid)
        g_worldLightTargets=discoverWorldLightTargets();

    return !g_worldLightTargets.empty();
}

bool applyWorldLightHack() {
    if (!ensureWorldLightTargets())
        return false;

    bool wroteAny=false;
    for (auto& target:g_worldLightTargets) {
        std::uintptr_t currentMapView{};
        float currentAmbient{};
        if (!validateWorldLightTarget(
                target.uiMap,currentMapView,&currentAmbient) ||
            currentMapView!=target.mapView)
            continue;

        if (!target.originalSaved) {
            target.originalAmbient=currentAmbient;
            target.originalSaved=true;
        }

        if (safeWrite(
                target.mapView + g_profile->mapViewMinimumAmbientOffset,
                kWorldLightHackAmbient))
            wroteAny=true;
    }

    g_worldLightEnabled=wroteAny;
    return wroteAny;
}

bool restoreWorldLightHack() {
    bool restoredAny=false;
    for (auto& target:g_worldLightTargets) {
        if (!target.originalSaved) continue;

        std::uintptr_t currentMapView{};
        if (!validateWorldLightTarget(
                target.uiMap,currentMapView,nullptr) ||
            currentMapView!=target.mapView)
            continue;

        if (safeWrite(
                target.mapView + g_profile->mapViewMinimumAmbientOffset,
                target.originalAmbient))
            restoredAny=true;

        target.originalSaved=false;
    }

    g_worldLightEnabled=false;
    return restoredAny || g_worldLightTargets.empty();
}

std::string worldLightStatusResponse(const char* cmd) {
    std::ostringstream o;
    const bool resolved=ensureWorldLightTargets();
    o << "{\"ok\":" << (resolved ? "true" : "false")
      << ",\"cmd\":\"" << cmd << "\""
      << ",\"contractReady\":"
      << ((g_profile && g_profile->worldLightContractReady) ? "true" : "false")
      << ",\"enabled\":" << (g_worldLightEnabled ? "true" : "false")
      << ",\"targets\":" << g_worldLightTargets.size()
      << ",\"forcedAmbient\":" << kWorldLightHackAmbient;

    if (resolved && !g_worldLightTargets.empty()) {
        float ambient{};
        safeRead(
            g_worldLightTargets.front().mapView +
                g_profile->mapViewMinimumAmbientOffset,
            ambient);
        o << ",\"ambient\":" << ambient;
    }

    if (!resolved)
        o << ",\"error\":\"world_light_target_not_found\"";

    o << "}";
    return o.str();
}

std::string setWorldLightResponse(bool enabled) {
    if (!g_profile || !g_profile->worldLightContractReady)
        return R"({"ok":false,"cmd":"SET_LIGHT","error":"world_light_contract_not_ready"})";

    const bool ok=enabled ? applyWorldLightHack() : restoreWorldLightHack();
    logLine(std::string("[LIGHT] ") +
            (enabled ? "enable" : "restore") +
            " ok=" + (ok ? "true" : "false") +
            " targets=" + std::to_string(g_worldLightTargets.size()));
    return worldLightStatusResponse("SET_LIGHT");
}

bool writeCodeByte(std::uintptr_t address,std::uint8_t value) {
    if (!address) return false;
    DWORD oldProtect{};
    if (!VirtualProtect(
            reinterpret_cast<void*>(address),
            1,
            PAGE_EXECUTE_READWRITE,
            &oldProtect))
        return false;

    *reinterpret_cast<volatile std::uint8_t*>(address)=value;
    FlushInstructionCache(
        GetCurrentProcess(),
        reinterpret_cast<void*>(address),
        1);

    DWORD ignored{};
    VirtualProtect(
        reinterpret_cast<void*>(address),
        1,
        oldProtect,
        &ignored);
    return true;
}

void restoreLookCaptureBreakpointLocked() {
    if (!g_lookCaptureArmed || !g_lookCaptureAddress)
        return;
    writeCodeByte(g_lookCaptureAddress,g_lookCaptureOriginalByte);
    InterlockedExchange(&g_lookCaptureArmed,0);
}

LONG CALLBACK lookCaptureVeh(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord)
        return EXCEPTION_CONTINUE_SEARCH;
    if (ep->ExceptionRecord->ExceptionCode!=EXCEPTION_BREAKPOINT)
        return EXCEPTION_CONTINUE_SEARCH;

    AcquireSRWLockExclusive(&g_lookCaptureLock);

    if (!g_lookCaptureArmed ||
        ep->ExceptionRecord->ExceptionAddress !=
            reinterpret_cast<void*>(g_lookCaptureAddress)) {
        ReleaseSRWLockExclusive(&g_lookCaptureLock);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // One-shot: restore original byte immediately before resuming Game::look.
    restoreLookCaptureBreakpointLocked();

    std::uintptr_t holderRef{};
    std::uintptr_t item{};
    std::uint16_t itemId{};
    std::uintptr_t itemVtable{};

    const auto esp=static_cast<std::uintptr_t>(ep->ContextRecord->Esp);
    const bool ok=
        g_profile &&
        g_profile->lookRuntimeConfirmed &&
        g_profile->lookItemSharedRefConfirmed &&
        safeRead(esp+sizeof(std::uintptr_t),holderRef) &&
        holderRef &&
        safeRead(holderRef,item) &&
        item &&
        safeRead(item,itemVtable) &&
        itemVtable>=g_base &&
        itemVtable<g_base+g_imageSize &&
        safeRead(item+g_profile->itemIdOffset,itemId) &&
        itemId!=0;

    if (ok) {
        g_lookCapturedHolder=holderRef;
        g_lookCapturedItem=item;
        g_lookCapturedId=itemId;
        g_lookCapturedTick=GetTickCount64();
        InterlockedExchange(&g_lookCaptureCaptured,1);
        logLine("[LOOK] captured itemId="+
                std::to_string(static_cast<unsigned>(itemId)));
    } else {
        g_lookCapturedHolder=0;
        g_lookCapturedItem=0;
        g_lookCapturedId=0;
        g_lookCapturedTick=GetTickCount64();
        InterlockedExchange(&g_lookCaptureCaptured,0);
        logLine("[LOOK] Game::look fired but ItemPtr validation failed");
    }

    // INT3 advances EIP by one byte. Rewind so the restored original first
    // instruction executes normally.
    ep->ContextRecord->Eip=
        static_cast<DWORD>(g_lookCaptureAddress);

    if (g_lookCaptureEvent)
        SetEvent(g_lookCaptureEvent);

    ReleaseSRWLockExclusive(&g_lookCaptureLock);
    return EXCEPTION_CONTINUE_EXECUTION;
}

DWORD WINAPI lookCaptureTimeoutThread(void*) {
    HANDLE event{};
    AcquireSRWLockShared(&g_lookCaptureLock);
    event=g_lookCaptureEvent;
    ReleaseSRWLockShared(&g_lookCaptureLock);

    if (event)
        WaitForSingleObject(event,8000);

    // Let the VEH finish returning before unregistering it.
    Sleep(40);

    AcquireSRWLockExclusive(&g_lookCaptureLock);
    restoreLookCaptureBreakpointLocked();

    if (g_lookVehHandle) {
        RemoveVectoredExceptionHandler(g_lookVehHandle);
        g_lookVehHandle=nullptr;
    }

    if (g_lookCaptureEvent) {
        CloseHandle(g_lookCaptureEvent);
        g_lookCaptureEvent=nullptr;
    }

    InterlockedExchange(&g_lookCaptureWorkerActive,0);
    ReleaseSRWLockExclusive(&g_lookCaptureLock);
    return 0;
}

void cleanupLookCapture(bool clearResult=false) {
    // Avoid closing the wait handle underneath an active timeout worker.
    AcquireSRWLockExclusive(&g_lookCaptureLock);
    restoreLookCaptureBreakpointLocked();

    if (!g_lookCaptureWorkerActive) {
        if (g_lookVehHandle) {
            RemoveVectoredExceptionHandler(g_lookVehHandle);
            g_lookVehHandle=nullptr;
        }
        if (g_lookCaptureEvent) {
            CloseHandle(g_lookCaptureEvent);
            g_lookCaptureEvent=nullptr;
        }
    }

    if (clearResult) {
        InterlockedExchange(&g_lookCaptureCaptured,0);
        g_lookCapturedId=0;
        g_lookCapturedHolder=0;
        g_lookCapturedItem=0;
        g_lookCapturedTick=0;
    }
    ReleaseSRWLockExclusive(&g_lookCaptureLock);
}

std::string armLookCaptureResponse() {
    if (!g_profile || !g_buildMatched)
        return R"({"ok":false,"cmd":"ARM_LOOK_ID","error":"build_not_validated"})";
    if (!g_profile->lookRuntimeConfirmed ||
        !g_profile->lookItemSharedRefConfirmed ||
        !g_profile->lookMethodRva ||
        !g_profile->itemContractReady)
        return R"({"ok":false,"cmd":"ARM_LOOK_ID","error":"look_contract_not_ready"})";

    if (InterlockedCompareExchange(
            &g_lookCaptureWorkerActive,1,0)!=0)
        return R"({"ok":false,"cmd":"ARM_LOOK_ID","error":"capture_busy"})";

    const auto lookAddress=g_base+g_profile->lookMethodRva;
    if (!isExecutableAddress(lookAddress)) {
        InterlockedExchange(&g_lookCaptureWorkerActive,0);
        return R"({"ok":false,"cmd":"ARM_LOOK_ID","error":"look_function_not_executable"})";
    }

    std::uint8_t original{};
    if (!safeRead(lookAddress,original) || original==0xCC) {
        InterlockedExchange(&g_lookCaptureWorkerActive,0);
        return R"({"ok":false,"cmd":"ARM_LOOK_ID","error":"look_first_byte_unavailable"})";
    }

    AcquireSRWLockExclusive(&g_lookCaptureLock);

    g_lookCaptureAddress=lookAddress;
    g_lookCaptureOriginalByte=original;
    g_lookCapturedId=0;
    g_lookCapturedHolder=0;
    g_lookCapturedItem=0;
    g_lookCapturedTick=0;
    InterlockedExchange(&g_lookCaptureCaptured,0);

    g_lookCaptureEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if (!g_lookCaptureEvent) {
        InterlockedExchange(&g_lookCaptureWorkerActive,0);
        ReleaseSRWLockExclusive(&g_lookCaptureLock);
        return R"({"ok":false,"cmd":"ARM_LOOK_ID","error":"event_create_failed"})";
    }

    g_lookVehHandle=AddVectoredExceptionHandler(1,&lookCaptureVeh);
    if (!g_lookVehHandle) {
        CloseHandle(g_lookCaptureEvent);
        g_lookCaptureEvent=nullptr;
        InterlockedExchange(&g_lookCaptureWorkerActive,0);
        ReleaseSRWLockExclusive(&g_lookCaptureLock);
        return R"({"ok":false,"cmd":"ARM_LOOK_ID","error":"veh_install_failed"})";
    }

    if (!writeCodeByte(lookAddress,0xCC)) {
        RemoveVectoredExceptionHandler(g_lookVehHandle);
        g_lookVehHandle=nullptr;
        CloseHandle(g_lookCaptureEvent);
        g_lookCaptureEvent=nullptr;
        InterlockedExchange(&g_lookCaptureWorkerActive,0);
        ReleaseSRWLockExclusive(&g_lookCaptureLock);
        return R"({"ok":false,"cmd":"ARM_LOOK_ID","error":"breakpoint_install_failed"})";
    }

    InterlockedExchange(&g_lookCaptureArmed,1);
    ReleaseSRWLockExclusive(&g_lookCaptureLock);

    HANDLE thread=CreateThread(
        nullptr,0,&lookCaptureTimeoutThread,nullptr,0,nullptr);
    if (!thread) {
        AcquireSRWLockExclusive(&g_lookCaptureLock);
        restoreLookCaptureBreakpointLocked();
        if (g_lookVehHandle) {
            RemoveVectoredExceptionHandler(g_lookVehHandle);
            g_lookVehHandle=nullptr;
        }
        if (g_lookCaptureEvent) {
            CloseHandle(g_lookCaptureEvent);
            g_lookCaptureEvent=nullptr;
        }
        InterlockedExchange(&g_lookCaptureWorkerActive,0);
        ReleaseSRWLockExclusive(&g_lookCaptureLock);
        return R"({"ok":false,"cmd":"ARM_LOOK_ID","error":"timeout_thread_failed"})";
    }
    CloseHandle(thread);

    logLine("[LOOK] one-shot capture armed for 8s");
    return R"({"ok":true,"cmd":"ARM_LOOK_ID","armed":true,"timeoutMs":8000})";
}

void shutdownLookCapture() {
    HANDLE event{};
    AcquireSRWLockExclusive(&g_lookCaptureLock);
    restoreLookCaptureBreakpointLocked();
    event=g_lookCaptureEvent;
    if (event) SetEvent(event);
    ReleaseSRWLockExclusive(&g_lookCaptureLock);

    for (int i=0;i<100 &&
         InterlockedCompareExchange(&g_lookCaptureWorkerActive,0,0)!=0;
         ++i)
        Sleep(10);

    AcquireSRWLockExclusive(&g_lookCaptureLock);
    if (g_lookVehHandle) {
        RemoveVectoredExceptionHandler(g_lookVehHandle);
        g_lookVehHandle=nullptr;
    }
    if (g_lookCaptureEvent) {
        CloseHandle(g_lookCaptureEvent);
        g_lookCaptureEvent=nullptr;
    }
    InterlockedExchange(&g_lookCaptureWorkerActive,0);
    InterlockedExchange(&g_lookCaptureArmed,0);
    ReleaseSRWLockExclusive(&g_lookCaptureLock);
}

std::string getLookCaptureResponse() {
    const bool captured=
        InterlockedCompareExchange(&g_lookCaptureCaptured,0,0)!=0;
    const bool armed=
        InterlockedCompareExchange(&g_lookCaptureArmed,0,0)!=0;
    const bool busy=
        InterlockedCompareExchange(&g_lookCaptureWorkerActive,0,0)!=0;

    std::ostringstream o;
    o << "{\"ok\":true,\"cmd\":\"GET_LOOK_ID\""
      << ",\"armed\":" << (armed?"true":"false")
      << ",\"busy\":" << (busy?"true":"false")
      << ",\"captured\":" << (captured?"true":"false");

    if (captured) {
        o << ",\"id\":" << g_lookCapturedId
          << ",\"ageMs\":"
          << (GetTickCount64()>=g_lookCapturedTick
                ? GetTickCount64()-g_lookCapturedTick
                : 0);
    }

    o << "}";
    return o.str();
}

bool readMsvcString32(std::uintptr_t address, std::string& out) {
    out.clear();
    std::uint32_t len{}, cap{};
    if (!safeRead(address + 0x10, len) ||
        !safeRead(address + 0x14, cap) ||
        len > 96 || cap < len || cap > 0x10000)
        return false;

    if (len == 0) return true;

    char buf[97]{};
    if (cap < 16) {
        if (!safeCopyBytes(address, buf, len)) return false;
    } else {
        std::uint32_t ptr{};
        if (!safeRead(address, ptr) || !ptr ||
            !safeCopyBytes(static_cast<std::uintptr_t>(ptr), buf, len))
            return false;
    }

    for (std::uint32_t i = 0; i < len; ++i) {
        const unsigned char c = static_cast<unsigned char>(buf[i]);
        if (c < 0x20 || c > 0x7E) return false;
    }
    out.assign(buf, buf + len);
    return true;
}

struct ContainerItemSlot {
    std::uintptr_t holderAddress{};
    std::uintptr_t itemAddress{};
    std::uint16_t id{};
    std::uint16_t count{};
    std::uint32_t index{};
};

struct ContainerSnapshot {
    std::uintptr_t address{};
    std::uint32_t id{};
    std::uint32_t capacity{};
    std::string name;
    std::vector<ContainerItemSlot> items;
};

bool readContainerItems(std::uintptr_t container,
                        std::vector<ContainerItemSlot>& items) {
    items.clear();
    if (!g_profile || !g_profile->containerContractReady ||
        !exactObjectVtable(container, g_profile->containerVtableRva))
        return false;

    const auto base = container + g_profile->containerItemsOffset;
    std::uint32_t map{}, mapSize{}, myOff{}, mySize{};
    if (!safeRead(base + 0x0, map) ||
        !safeRead(base + 0x4, mapSize) ||
        !safeRead(base + 0x8, myOff) ||
        !safeRead(base + 0xC, mySize))
        return false;

    if (mySize == 0) return true;
    if (!map || !looksHeapPointer(map) ||
        mapSize == 0 || mapSize > 256 ||
        mySize > 100 ||
        g_profile->containerItemStride != 4)
        return false;

    constexpr std::uint32_t blockSize = 4; // MSVC x86 deque block for 4-byte holders.
    items.reserve(mySize);

    for (std::uint32_t i = 0; i < mySize; ++i) {
        const std::uint32_t logical = myOff + i;
        std::uint32_t blockIndex = logical / blockSize;
        if ((mapSize & (mapSize - 1)) == 0)
            blockIndex &= (mapSize - 1);
        else
            blockIndex %= mapSize;

        const std::uint32_t inner = logical % blockSize;
        std::uint32_t blockPtr{};
        if (!safeRead(static_cast<std::uintptr_t>(map) + blockIndex * 4u, blockPtr) ||
            !blockPtr || !looksHeapPointer(blockPtr))
            return false;

        const auto holder =
            static_cast<std::uintptr_t>(blockPtr) +
            inner * g_profile->containerItemStride;

        std::uintptr_t item{};
        if (!safeRead(holder, item) ||
            !item ||
            !exactObjectVtable(item, g_profile->itemVtableRva))
            return false;

        std::uint16_t id{}, count{};
        if (!safeRead(item + g_profile->itemIdOffset, id) || !id ||
            !safeRead(item + g_profile->itemCountOffset, count))
            return false;

        items.push_back({holder, item, id, count, i});
    }
    return true;
}

bool readContainerSnapshot(std::uintptr_t container, ContainerSnapshot& out) {
    out = {};
    if (!g_profile || !g_profile->containerContractReady ||
        !exactObjectVtable(container, g_profile->containerVtableRva))
        return false;

    std::uint32_t id{}, capacity{};
    if (!safeRead(container + g_profile->containerIdOffset, id) ||
        !safeRead(container + g_profile->containerCapacityOffset, capacity) ||
        id > 255 || capacity > 1000)
        return false;

    std::string name;
    readMsvcString32(container + g_profile->containerNameOffset, name);

    std::vector<ContainerItemSlot> items;
    if (!readContainerItems(container, items)) return false;
    if (capacity && items.size() > capacity) return false;

    out.address = container;
    out.id = id;
    out.capacity = capacity;
    out.name = std::move(name);
    out.items = std::move(items);
    return true;
}

std::vector<std::uintptr_t> discoverOpenContainerAddresses() {
    std::vector<std::uintptr_t> found;
    if (!g_profile || !g_profile->containerContractReady) return found;

    const auto game = g_base + g_profile->gameRva;
    const auto rootAddress = game + g_profile->openContainersOffset;

    struct Node { std::uintptr_t address{}; int depth{}; };
    std::vector<Node> queue;
    std::set<std::uintptr_t> seen;

    const auto scan = [&](auto&& self,
                          std::uintptr_t address,
                          std::size_t bytes,
                          int depth,
                          bool enqueue) -> void {
        for (std::size_t off = 0; off + 4 <= bytes; off += 4) {
            std::uint32_t value{};
            if (!safeRead(address + off, value) || !value) continue;
            const auto ptr = static_cast<std::uintptr_t>(value);

            if (exactObjectVtable(ptr, g_profile->containerVtableRva)) {
                found.push_back(ptr);
                continue;
            }

            if (enqueue && depth < 4 && queue.size() < 600 &&
                looksHeapPointer(ptr) && seen.insert(ptr).second) {
                queue.push_back({ptr, depth + 1});
            }
        }
    };

    // V25 profile evidence requires the first hop from Game+openContainers
    // to be exactly +0 of the inline registry root.
    scan(scan, rootAddress, 0x04, 0, true);
    for (std::size_t i = 0; i < queue.size() && i < 600; ++i)
        scan(scan, queue[i].address, 0xA0, queue[i].depth, true);

    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    return found;
}

std::vector<ContainerSnapshot> scanOpenContainers() {
    std::vector<ContainerSnapshot> out;
    for (const auto address : discoverOpenContainerAddresses()) {
        ContainerSnapshot c;
        if (readContainerSnapshot(address, c))
            out.push_back(std::move(c));
    }

    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        if (a.id != b.id) return a.id < b.id;
        return a.address < b.address;
    });
    return out;
}

bool resolveTileItemHolder(std::uintptr_t tile,
                           std::uint16_t itemId,
                           std::uintptr_t expectedItem,
                           std::uintptr_t& holderOut,
                           std::uintptr_t& itemOut) {
    holderOut = itemOut = 0;
    if (!g_profile || !g_profile->itemContractReady || !tile || !itemId)
        return false;

    std::uint32_t begin{}, end{}, cap{}, count{};
    if (!readTileVectorHeader(tile, begin, end, cap, count) || !count)
        return false;

    std::uintptr_t fallbackHolder{}, fallbackItem{};
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto holder = static_cast<std::uintptr_t>(begin) + i * 4u;
        std::uintptr_t item{};
        if (!safeRead(holder, item) ||
            !item ||
            !exactObjectVtable(item, g_profile->itemVtableRva))
            continue;

        std::uint16_t id{};
        if (!safeRead(item + g_profile->itemIdOffset, id) || id != itemId)
            continue;

        if (expectedItem && item == expectedItem) {
            holderOut = holder;
            itemOut = item;
            return true;
        }
        if (!fallbackHolder) {
            fallbackHolder = holder;
            fallbackItem = item;
        }
    }

    holderOut = fallbackHolder;
    itemOut = fallbackItem;
    return holderOut != 0;
}

std::uint32_t tileSlotOf(const std::vector<std::uintptr_t>& tiles,
                         std::uintptr_t tile) {
    for (std::size_t i = 0; i < tiles.size(); ++i)
        if (tiles[i] == tile) return static_cast<std::uint32_t>(i);
    return 0xFFFFFFFFu;
}

std::vector<TileItemDiagnostic> scanTileItemsFromSnapshot(
    const std::vector<std::uintptr_t>& tiles) {
    std::vector<TileItemDiagnostic> out;
    if (!g_profile || !g_profile->itemContractReady ||
        !g_profile->tileThingsRuntimeConfirmed)
        return out;

    const auto wantedItemVtable = g_base + g_profile->itemVtableRva;

    for (std::size_t tileIndex = 0; tileIndex < tiles.size(); ++tileIndex) {
        const auto tile = tiles[tileIndex];
        std::vector<std::uintptr_t> things;
        bool containsLocalPlayer = false;
        if (!readStableTileThings(tile, things, containsLocalPlayer))
            continue;

        for (std::size_t stackIndex = 0; stackIndex < things.size(); ++stackIndex) {
            const auto thing = things[stackIndex];
            std::uintptr_t vt{};
            if (!safeRead(thing, vt) || vt != wantedItemVtable)
                continue;

            std::uint16_t id{}, count{};
            std::uint32_t x{},y{};
            std::uint8_t z{};
            if (!safeRead(thing + g_profile->itemIdOffset, id) || !id ||
                !safeRead(thing + g_profile->itemCountOffset, count) ||
                !safeRead(thing + g_profile->posX, x) ||
                !safeRead(thing + g_profile->posY, y) ||
                !safeRead(thing + g_profile->posZ, z) ||
                !x || !y || z > 15u)
                continue;

            TileItemDiagnostic d;
            d.address = thing;
            d.tileAddress = tile;
            d.id = id;
            d.count = count;
            d.x = x;
            d.y = y;
            d.z = z;
            d.tileSlot = static_cast<std::uint32_t>(tileIndex);
            d.stackIndex = static_cast<std::uint32_t>(stackIndex);
            d.tileThingCount = static_cast<std::uint32_t>(things.size());
            out.push_back(d);
        }
    }
    return out;
}

std::vector<TileMonsterDiagnostic> scanTileMonstersFromSnapshot(
    const std::vector<std::uintptr_t>& tiles,
    std::uint32_t maxRange = 15) {
    std::vector<TileMonsterDiagnostic> out;
    if (!g_profile || !g_profile->tileThingsRuntimeConfirmed)
        return out;

    std::uint32_t px{}, py{};
    std::uint8_t pz{};
    if (!localPlayerPosition(px, py, pz))
        return out;

    for (const auto tile : tiles) {
        std::vector<std::uintptr_t> things;
        bool containsLocalPlayer = false;
        if (!readStableTileThings(tile, things, containsLocalPlayer))
            continue;

        for (const auto thing : things) {
            VisibleMonster monster;
            if (!snapshotMonsterFromThing(
                    thing, px, py, pz, maxRange, monster))
                continue;

            bool duplicate = false;
            for (const auto& existing : out) {
                if (existing.monster.address == monster.address ||
                    existing.monster.id == monster.id) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;

            TileMonsterDiagnostic d;
            d.monster = std::move(monster);
            d.tileAddress = tile;
            d.tileThingCount = static_cast<std::uint32_t>(things.size());
            d.tileContainsLocalPlayer = containsLocalPlayer;
            out.push_back(std::move(d));
        }
    }

    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        if (a.monster.distance != b.monster.distance)
            return a.monster.distance < b.monster.distance;
        return a.monster.id < b.monster.id;
    });
    return out;
}

std::vector<TileItemDiagnostic> scanCachedTileItems(
    bool forceTileRefresh = false) {
    if (!g_profile || !g_profile->itemContractReady ||
        !g_profile->tileThingsRuntimeConfirmed)
        return {};

    std::uint32_t px{}, py{};
    std::uint8_t pz{};
    if (!localPlayerPosition(px, py, pz)) return {};
    if (!ensureTileCache(px, py, pz, forceTileRefresh)) return {};

    return scanTileItemsFromSnapshot(copyTileCache());
}

std::optional<TileItemDiagnostic> findNearbyTileItemById(
    std::uint16_t itemId,
    std::uint32_t radius = 1) {

    if (!itemId || !g_profile || !g_profile->itemContractReady ||
        !g_profile->tileThingsRuntimeConfirmed)
        return std::nullopt;

    std::uint32_t px{},py{};
    std::uint8_t pz{};
    if (!localPlayerPosition(px,py,pz))
        return std::nullopt;
    if (!ensureTileCache(px,py,pz,false))
        return std::nullopt;

    const auto tiles=copyNearbyTileCache(px,py,pz,radius);
    const auto wantedItemVtable=g_base+g_profile->itemVtableRva;
    std::optional<TileItemDiagnostic> best;
    std::uint32_t bestDistance=0xFFFFFFFFu;

    // Important: only the selected nearby tiles are opened here. We do not
    // enumerate every cached Tile::things on each pickup poll.
    for (std::size_t tileIndex=0;tileIndex<tiles.size();++tileIndex) {
        const auto tile=tiles[tileIndex];
        std::vector<std::uintptr_t> things;
        bool containsLocalPlayer=false;
        if (!readStableTileThings(tile,things,containsLocalPlayer))
            continue;

        for (std::size_t stackIndex=0;stackIndex<things.size();++stackIndex) {
            const auto thing=things[stackIndex];
            std::uintptr_t vt{};
            if (!safeRead(thing,vt) || vt!=wantedItemVtable)
                continue;

            std::uint16_t id{};
            if (!safeRead(thing+g_profile->itemIdOffset,id) || id!=itemId)
                continue;

            std::uint16_t count{};
            std::uint32_t x{},y{};
            std::uint8_t z{};
            if (!safeRead(thing+g_profile->itemCountOffset,count) ||
                !safeRead(thing+g_profile->posX,x) ||
                !safeRead(thing+g_profile->posY,y) ||
                !safeRead(thing+g_profile->posZ,z) ||
                z!=pz)
                continue;

            const auto dx=x>px ? x-px : px-x;
            const auto dy=y>py ? y-py : py-y;
            const auto distance=std::max(dx,dy);
            if (distance>radius)
                continue;

            TileItemDiagnostic d;
            d.address=thing;
            d.tileAddress=tile;
            d.id=id;
            d.count=count;
            d.x=x;
            d.y=y;
            d.z=z;
            d.tileSlot=static_cast<std::uint32_t>(tileIndex);
            d.stackIndex=static_cast<std::uint32_t>(stackIndex);
            d.tileThingCount=static_cast<std::uint32_t>(things.size());

            if (!best || distance<bestDistance ||
                (distance==bestDistance && d.stackIndex>best->stackIndex)) {
                best=d;
                bestDistance=distance;
            }
        }
    }

    return best;
}

struct TileTargetAccess {
    bool valid{};
    bool accessible{};
    bool nonItemAbove{};
    TileItemDiagnostic target;
    std::optional<TileItemDiagnostic> blocker;
};

TileTargetAccess inspectTileTargetAccess(const TileItemDiagnostic& wanted) {
    TileTargetAccess out;
    out.target=wanted;

    if (!g_profile || !g_profile->itemContractReady ||
        !g_profile->tileThingsRuntimeConfirmed ||
        !wanted.tileAddress || !wanted.address || !wanted.id)
        return out;

    std::vector<std::uintptr_t> things;
    bool containsLocalPlayer=false;
    if (!readStableTileThings(
            wanted.tileAddress,things,containsLocalPlayer) ||
        things.empty())
        return out;

    const auto wantedItemVtable=g_base+g_profile->itemVtableRva;
    std::size_t targetIndex=things.size();

    for(std::size_t i=0;i<things.size();++i) {
        const auto thing=things[i];
        if(thing!=wanted.address) continue;

        std::uintptr_t vt{};
        std::uint16_t id{};
        if(!safeRead(thing,vt) || vt!=wantedItemVtable ||
           !safeRead(thing+g_profile->itemIdOffset,id) ||
           id!=wanted.id)
            continue;

        targetIndex=i;
        break;
    }

    if(targetIndex>=things.size())
        return out;

    out.valid=true;
    out.target.stackIndex=static_cast<std::uint32_t>(targetIndex);
    out.target.tileThingCount=static_cast<std::uint32_t>(things.size());

    // Ramonia live evidence shows the visible/move blocker is not the
    // largest stackIndex. Example:
    //   103[0], 16845[1], 20159[2], spear 2389[3]
    // where 20159 is visually above the spear.
    //
    // OTC's classic getTopMoveThing semantics also walk Tile::things from
    // lower indices toward higher common items. Until ThingType flags are
    // mapped for this build, use the nearest Item immediately BEFORE the
    // exact target as the one-layer uncover candidate. This is deliberately
    // one layer per call and will be re-read after every move.
    if(targetIndex>0) {
        for(std::size_t i=targetIndex; i-- > 0;) {
            const auto thing=things[i];
            std::uintptr_t vt{};
            if(!safeRead(thing,vt))
                continue;

            if(vt!=wantedItemVtable) {
                out.nonItemAbove=true;
                continue;
            }

            std::uint16_t id{},count{};
            std::uint32_t x{},y{};
            std::uint8_t z{};
            if(!safeRead(thing+g_profile->itemIdOffset,id) || !id ||
               !safeRead(thing+g_profile->itemCountOffset,count) ||
               !safeRead(thing+g_profile->posX,x) ||
               !safeRead(thing+g_profile->posY,y) ||
               !safeRead(thing+g_profile->posZ,z))
                continue;

            TileItemDiagnostic blocker;
            blocker.address=thing;
            blocker.tileAddress=wanted.tileAddress;
            blocker.id=id;
            blocker.count=count;
            blocker.x=x;
            blocker.y=y;
            blocker.z=z;
            blocker.stackIndex=static_cast<std::uint32_t>(i);
            blocker.tileThingCount=static_cast<std::uint32_t>(things.size());
            out.blocker=blocker;
            break;
        }
    }

    out.accessible=!out.blocker.has_value();
    return out;
}

std::optional<MovePosition> chooseAdjacentGroundDestination(
    std::uint32_t sourceX,
    std::uint32_t sourceY,
    std::uint8_t sourceZ) {

    std::uint32_t px{},py{};
    std::uint8_t pz{};
    if(!localPlayerPosition(px,py,pz) || pz!=sourceZ)
        return std::nullopt;

    struct Delta { int dx; int dy; };
    static constexpr Delta kOrder[] = {
        {1,0},{-1,0},{0,1},{0,-1},
        {1,1},{1,-1},{-1,1},{-1,-1}
    };

    std::vector<TileCoordCacheEntry> coords;
    AcquireSRWLockShared(&g_tileCacheLock);
    coords=g_tileCoordCache;
    ReleaseSRWLockShared(&g_tileCacheLock);

    auto exists=[&](std::uint32_t x,std::uint32_t y){
        for(const auto& e:coords)
            if(e.z==sourceZ && e.x==x && e.y==y)
                return true;
        return false;
    };

    // Prefer not to dump a blocker underneath the player. If there is no
    // other cached neighbor, allow the player's tile as last resort.
    for(int pass=0;pass<2;++pass) {
        for(const auto d:kOrder) {
            const auto nx64=static_cast<std::int64_t>(sourceX)+d.dx;
            const auto ny64=static_cast<std::int64_t>(sourceY)+d.dy;
            if(nx64<=0 || ny64<=0)
                continue;
            const auto nx=static_cast<std::uint32_t>(nx64);
            const auto ny=static_cast<std::uint32_t>(ny64);
            const bool isPlayerTile=nx==px && ny==py;
            if((pass==0 && isPlayerTile) || (pass==1 && !isPlayerTile))
                continue;
            if(!exists(nx,ny))
                continue;

            MovePosition dest{};
            dest.x=nx;
            dest.y=ny;
            dest.z=sourceZ;
            return dest;
        }
    }

    return std::nullopt;
}

struct InventorySlotSnapshot {
    std::uint8_t slot{};
    std::uintptr_t item{};
    std::uint16_t id{};
    std::uint16_t count{};
    bool occupied{};
};

bool readInventorySlot(std::uint8_t slot, InventorySlotSnapshot& out) {
    out={};
    out.slot=slot;
    if (!g_profile || !g_profile->inventoryContractReady ||
        !g_profile->itemContractReady ||
        slot<1u || slot>10u)
        return false;

    const auto& p=*g_profile;
    const auto game=g_base+p.gameRva;
    std::uintptr_t localPlayer{};
    if (!safeRead(game+p.localPlayerOffset,localPlayer) ||
        !localPlayer ||
        !exactObjectVtable(localPlayer,p.localPlayerVtableRva))
        return false;

    std::uintptr_t item{};
    const auto holderAddress=
        localPlayer+p.inventoryArrayOffset+
        static_cast<std::size_t>(slot-1u)*p.inventoryStride;

    if (!safeRead(holderAddress,item))
        return false;

    if (!item) {
        out.occupied=false;
        return true;
    }

    if (!exactObjectVtable(item,p.itemVtableRva))
        return false;

    std::uint16_t id{},count{};
    if (!safeRead(item+p.itemIdOffset,id) || !id ||
        !safeRead(item+p.itemCountOffset,count))
        return false;

    out.item=item;
    out.id=id;
    out.count=count;
    out.occupied=true;
    return true;
}

std::optional<TileItemDiagnostic> findTileItemAt(
    std::uint32_t x,
    std::uint32_t y,
    std::uint8_t z,
    std::uint16_t preferredId = 0,
    bool forceTileRefresh = false) {

    const auto items=scanCachedTileItems(forceTileRefresh);
    std::optional<TileItemDiagnostic> best;

    for (const auto& item:items) {
        if (item.x!=x || item.y!=y || item.z!=z)
            continue;
        if (preferredId && item.id!=preferredId)
            continue;

        if (!best || item.stackIndex>best->stackIndex)
            best=item;
    }

    return best;
}

bool sameItemIdentity(const TileItemDiagnostic& a, const TileItemDiagnostic& b) {
    return a.address == b.address && a.id == b.id;
}

bool currentMonsterContainsId(const std::vector<TileMonsterDiagnostic>& current,
                              std::uint32_t id) {
    for (const auto& m : current)
        if (m.monster.id == id) return true;
    return false;
}

std::string lootMarkResponse() {
    if (!g_profile || !g_profile->itemContractReady)
        return R"({"ok":false,"cmd":"LOOT_MARK","error":"item_contract_not_ready"})";

    std::uint32_t px{}, py{};
    std::uint8_t pz{};
    if (!localPlayerPosition(px, py, pz))
        return R"({"ok":false,"cmd":"LOOT_MARK","error":"player_unavailable"})";

    if (!ensureTileCache(px, py, pz))
        return R"({"ok":false,"cmd":"LOOT_MARK","error":"tile_cache_unavailable"})";

    const auto cache = tileCacheInfo();
    const auto tiles = copyTileCache();
    const auto items = scanTileItemsFromSnapshot(tiles);
    const auto monsters = scanTileMonstersFromSnapshot(tiles, 15);

    AcquireSRWLockExclusive(&g_lootBaselineLock);
    g_lootBaseline.valid = true;
    g_lootBaseline.tileGeneration = cache.generation;
    g_lootBaseline.tiles = tiles;
    g_lootBaseline.items = items;
    g_lootBaseline.monsters = monsters;
    ReleaseSRWLockExclusive(&g_lootBaselineLock);

    AcquireSRWLockExclusive(&g_lootCandidateLock);
    g_lastLootCandidates.clear();
    ReleaseSRWLockExclusive(&g_lootCandidateLock);

    std::ostringstream o;
    o << "{\"ok\":true,\"cmd\":\"LOOT_MARK\",\"readOnly\":true,"
      << "\"tileGeneration\":" << cache.generation
      << ",\"baselineItems\":" << items.size()
      << ",\"trackedMonsters\":[";

    for (std::size_t i = 0; i < monsters.size() && i < 32; ++i) {
        if (i) o << ",";
        const auto& m = monsters[i];
        o << "{\"id\":" << m.monster.id
          << ",\"name\":\"" << jsonEscape(m.monster.name) << "\""
          << ",\"hpPercent\":" << static_cast<unsigned>(m.monster.hpPercent)
          << ",\"distance\":" << m.monster.distance
          << ",\"tileSlot\":" << tileSlotOf(tiles, m.tileAddress)
          << "}";
    }
    o << "]}";
    return o.str();
}

std::string lootMarkTargetResponse(std::uint32_t targetId) {
    if (!targetId)
        return R"({"ok":false,"cmd":"LOOT_MARK_TARGET","error":"invalid_target_id"})";
    if (!g_profile || !g_profile->itemContractReady)
        return R"({"ok":false,"cmd":"LOOT_MARK_TARGET","error":"item_contract_not_ready"})";

    std::uint32_t px{}, py{};
    std::uint8_t pz{};
    if (!localPlayerPosition(px, py, pz))
        return R"({"ok":false,"cmd":"LOOT_MARK_TARGET","error":"player_unavailable"})";

    if (!ensureTileCache(px, py, pz))
        return R"({"ok":false,"cmd":"LOOT_MARK_TARGET","error":"tile_cache_unavailable"})";

    const auto cache = tileCacheInfo();
    const auto tiles = copyTileCache();
    const auto items = scanTileItemsFromSnapshot(tiles);
    const auto monsters = scanTileMonstersFromSnapshot(tiles, 15);

    const TileMonsterDiagnostic* tracked = nullptr;
    for (const auto& m : monsters) {
        if (m.monster.id == targetId) {
            tracked = &m;
            break;
        }
    }

    // Critical safety rule: if the requested target is already gone, DO NOT
    // overwrite the previous good baseline. That preserved baseline is the
    // last confirmed tile where the creature was alive.
    if (!tracked || tracked->monster.hpPercent == 0) {
        std::ostringstream o;
        o << "{\"ok\":false,\"cmd\":\"LOOT_MARK_TARGET\","
          << "\"error\":\"target_not_alive\",\"targetId\":" << targetId << "}";
        return o.str();
    }

    std::vector<TileItemDiagnostic> targetTileItems;
    for (const auto& item : items) {
        if (item.tileAddress == tracked->tileAddress)
            targetTileItems.push_back(item);
    }

    AcquireSRWLockExclusive(&g_lootBaselineLock);

    // A different creature ID starts a fresh death-tile history. For the same
    // target, keep a short rolling history so a last-moment step cannot move
    // the corpse outside the single most recent sample.
    if (g_lootBaseline.trackedTargetId != targetId)
        g_lootBaseline.targetTileHistory.clear();

    LootTargetTileFrame frame;
    frame.tileAddress = tracked->tileAddress;
    frame.x = tracked->monster.x;
    frame.y = tracked->monster.y;
    frame.z = tracked->monster.z;
    frame.tick = GetTickCount64();
    frame.items = std::move(targetTileItems);

    if (!g_lootBaseline.targetTileHistory.empty() &&
        g_lootBaseline.targetTileHistory.back().tileAddress == frame.tileAddress) {
        g_lootBaseline.targetTileHistory.back() = std::move(frame);
    } else {
        g_lootBaseline.targetTileHistory.push_back(std::move(frame));
        constexpr std::size_t kLootTargetTileHistory = 6;
        if (g_lootBaseline.targetTileHistory.size() > kLootTargetTileHistory)
            g_lootBaseline.targetTileHistory.erase(
                g_lootBaseline.targetTileHistory.begin(),
                g_lootBaseline.targetTileHistory.begin() +
                (g_lootBaseline.targetTileHistory.size() - kLootTargetTileHistory));
    }

    g_lootBaseline.valid = true;
    g_lootBaseline.tileGeneration = cache.generation;
    g_lootBaseline.trackedTargetId = targetId;
    g_lootBaseline.trackedTargetTile = tracked->tileAddress;
    g_lootBaseline.tiles = tiles;
    g_lootBaseline.items = items;
    g_lootBaseline.monsters = monsters;
    const auto historyCount = g_lootBaseline.targetTileHistory.size();
    ReleaseSRWLockExclusive(&g_lootBaselineLock);

    AcquireSRWLockExclusive(&g_lootCandidateLock);
    g_lastLootCandidates.clear();
    ReleaseSRWLockExclusive(&g_lootCandidateLock);

    std::ostringstream o;
    o << "{\"ok\":true,\"cmd\":\"LOOT_MARK_TARGET\","
      << "\"targetId\":" << targetId
      << ",\"targetName\":\"" << jsonEscape(tracked->monster.name) << "\""
      << ",\"targetTileSlot\":" << tileSlotOf(tiles, tracked->tileAddress)
      << ",\"targetTileHistory\":" << historyCount
      << ",\"tileGeneration\":" << cache.generation
      << ",\"baselineItems\":" << items.size()
      << "}";
    return o.str();
}

std::string lootDiffResponse() {
    LootBaselineState baseline;
    AcquireSRWLockShared(&g_lootBaselineLock);
    baseline = g_lootBaseline;
    ReleaseSRWLockShared(&g_lootBaselineLock);

    if (!baseline.valid)
        return R"({"ok":false,"cmd":"LOOT_DIFF","error":"no_baseline","hint":"run_LOOT_MARK_first"})";

    if (baseline.tiles.empty())
        return R"({"ok":false,"cmd":"LOOT_DIFF","error":"baseline_tile_snapshot_empty"})";

    const auto items = scanTileItemsFromSnapshot(baseline.tiles);
    const auto monsters = scanTileMonstersFromSnapshot(baseline.tiles, 15);
    const auto cache = tileCacheInfo();
    const auto& tiles = baseline.tiles;

    std::vector<TileItemDiagnostic> added;
    for (const auto& now : items) {
        bool existed = false;
        for (const auto& old : baseline.items) {
            if (sameItemIdentity(now, old)) { existed = true; break; }
        }
        if (!existed) added.push_back(now);
    }

    std::vector<TileMonsterDiagnostic> removed;
    for (const auto& oldMonster : baseline.monsters) {
        if (!currentMonsterContainsId(monsters, oldMonster.monster.id))
            removed.push_back(oldMonster);
    }

    std::vector<TileItemDiagnostic> deathTileCandidates;
    if (baseline.trackedTargetId && baseline.trackedTargetTile) {
        bool trackedRemoved = false;
        for (const auto& oldMonster : removed) {
            if (oldMonster.monster.id == baseline.trackedTargetId) {
                trackedRemoved = true;
                break;
            }
        }

        if (trackedRemoved) {
            auto pushCandidateUnique=[](
                std::vector<TileItemDiagnostic>& dst,
                const TileItemDiagnostic& item) {
                for (const auto& existing : dst) {
                    if (sameItemIdentity(existing,item))
                        return;
                }
                dst.push_back(item);
            };

            // Compare each recently confirmed target tile against the exact
            // item identities that existed there while the target was alive.
            // Decorative corpses/objects were already present in frame.items,
            // therefore they are never emitted as "new".
            for (auto fit = baseline.targetTileHistory.rbegin();
                 fit != baseline.targetTileHistory.rend(); ++fit) {

                const auto nowOnTile =
                    scanTileItemsFromSnapshot({fit->tileAddress});

                for (const auto& nowItem : nowOnTile) {
                    bool existed = false;
                    for (const auto& oldItem : fit->items) {
                        if (sameItemIdentity(nowItem,oldItem)) {
                            existed = true;
                            break;
                        }
                    }
                    if (!existed)
                        pushCandidateUnique(deathTileCandidates,nowItem);
                }
            }
        }
    } else if (!removed.empty()) {
        // Legacy LOOT_MARK diagnostic behavior only.
        deathTileCandidates = added;
    }

    AcquireSRWLockExclusive(&g_lootCandidateLock);
    g_lastLootCandidates = deathTileCandidates;
    ReleaseSRWLockExclusive(&g_lootCandidateLock);

    const auto& emittedItems =
        baseline.trackedTargetId ? deathTileCandidates : added;

    std::ostringstream o;
    o << "{\"ok\":true,\"cmd\":\"LOOT_DIFF\",\"readOnly\":true,"
      << "\"baselineGeneration\":" << baseline.tileGeneration
      << ",\"currentGlobalGeneration\":" << cache.generation
      << ",\"snapshotTiles\":" << baseline.tiles.size()
      << ",\"trackedTargetId\":" << baseline.trackedTargetId
      << ",\"trackedTargetTileSlot\":"
      << (baseline.trackedTargetTile
            ? tileSlotOf(tiles, baseline.trackedTargetTile)
            : 0xFFFFFFFFu)
      << ",\"trackedTargetTileHistory\":" << baseline.targetTileHistory.size()
      << ",\"candidatePolicy\":\"rolling_tracked_target_death_tiles\","
      << "\"addedCount\":" << emittedItems.size()
      << ",\"addedItems\":[";

    for (std::size_t i = 0; i < emittedItems.size() && i < 64; ++i) {
        if (i) o << ",";
        const auto& item = emittedItems[i];

        o << "{\"id\":" << item.id
          << ",\"count\":" << item.count
          << ",\"tileSlot\":" << item.tileSlot
          << ",\"stackIndex\":" << item.stackIndex
          << ",\"tileThingCount\":" << item.tileThingCount
          << ",\"baselineHadMonster\":";

        bool hadMonster = false;
        for (const auto& oldMonster : baseline.monsters)
            if (oldMonster.tileAddress == item.tileAddress) {
                hadMonster = true;
                break;
            }
        o << (hadMonster ? "true" : "false")
          << ",\"baselineMonsters\":[";

        bool firstMonster = true;
        for (const auto& oldMonster : baseline.monsters) {
            if (oldMonster.tileAddress != item.tileAddress) continue;
            if (!firstMonster) o << ",";
            firstMonster = false;
            o << "{\"id\":" << oldMonster.monster.id
              << ",\"name\":\"" << jsonEscape(oldMonster.monster.name) << "\""
              << "}";
        }
        o << "]}";
    }

    o << "],\"removedMonsters\":[";
    bool firstRemoved = true;
    for (const auto& oldMonster : removed) {
        if (!firstRemoved) o << ",";
        firstRemoved = false;
        o << "{\"id\":" << oldMonster.monster.id
          << ",\"name\":\"" << jsonEscape(oldMonster.monster.name) << "\""
          << ",\"tileSlot\":" << tileSlotOf(tiles, oldMonster.tileAddress)
          << "}";
    }
    o << "]}";
    return o.str();
}

std::string tileMonstersResponse() {
    if (!g_profile || !g_profile->tileThingsRuntimeConfirmed)
        return R"({"ok":false,"cmd":"GET_TILE_MONSTERS","error":"tile_contract_not_ready"})";

    const auto monsters = scanCachedTileMonsters(15);
    const auto cache = tileCacheInfo();

    std::ostringstream o;
    o << "{\"ok\":true,\"cmd\":\"GET_TILE_MONSTERS\","
      << "\"source\":\"Tile::things-cache\",\"readOnly\":true,"
      << "\"cachedTiles\":" << cache.count
      << ",\"cacheAgeMs\":" << cache.ageMs
      << ",\"count\":" << monsters.size() << ",\"monsters\":[";

    for (std::size_t i = 0; i < monsters.size() && i < 64; ++i) {
        if (i) o << ",";
        const auto& d = monsters[i];
        const auto& m = d.monster;
        o << "{\"id\":" << m.id
          << ",\"name\":\"" << jsonEscape(m.name) << "\""
          << ",\"hpPercent\":" << static_cast<unsigned>(m.hpPercent)
          << ",\"x\":" << m.x
          << ",\"y\":" << m.y
          << ",\"z\":" << static_cast<unsigned>(m.z)
          << ",\"distance\":" << m.distance
          << ",\"tileThingCount\":" << d.tileThingCount
          << ",\"tileContainsLocalPlayer\":"
          << (d.tileContainsLocalPlayer ? "true" : "false")
          << "}";
    }
    o << "]}";
    return o.str();
}


std::string nearbyItemResponse(
    std::uint16_t itemId,
    std::uint32_t radius) {

    if (!g_profile || !g_profile->itemContractReady ||
        !g_profile->tileThingsRuntimeConfirmed)
        return R"({"ok":false,"cmd":"GET_NEARBY_ITEM","error":"tile_item_contract_not_ready"})";

    std::uint32_t px{},py{};
    std::uint8_t pz{};
    if (!localPlayerPosition(px,py,pz))
        return R"({"ok":false,"cmd":"GET_NEARBY_ITEM","error":"player_unavailable"})";

    if (!ensureTileCache(px,py,pz,false))
        return R"({"ok":false,"cmd":"GET_NEARBY_ITEM","error":"tile_cache_unavailable"})";

    const auto nearbyTiles=copyNearbyTileCache(px,py,pz,radius);
    const auto match=findNearbyTileItemById(itemId,radius);
    const auto cache=tileCacheInfo();

    std::ostringstream o;
    o << "{\"ok\":true,\"cmd\":\"GET_NEARBY_ITEM\""
      << ",\"itemId\":" << itemId
      << ",\"radius\":" << radius
      << ",\"center\":{\"x\":" << px
      << ",\"y\":" << py
      << ",\"z\":" << static_cast<unsigned>(pz) << "}"
      << ",\"scannedTiles\":" << nearbyTiles.size()
      << ",\"cacheGeneration\":" << cache.generation
      << ",\"found\":" << (match ? "true" : "false");

    if (match) {
        const auto access=inspectTileTargetAccess(*match);
        o << ",\"item\":{"
          << "\"id\":" << match->id
          << ",\"count\":" << match->count
          << ",\"x\":" << match->x
          << ",\"y\":" << match->y
          << ",\"z\":" << static_cast<unsigned>(match->z)
          << ",\"stackIndex\":" << match->stackIndex
          << ",\"tileThingCount\":" << match->tileThingCount
          << ",\"accessible\":" << (access.valid && access.accessible ? "true" : "false")
          << ",\"blockerRule\":\"nearest_item_before_target_experimental\"";
        if(access.blocker) {
            o << ",\"blocker\":{"
              << "\"id\":" << access.blocker->id
              << ",\"count\":" << access.blocker->count
              << ",\"stackIndex\":" << access.blocker->stackIndex
              << "}";
        }
        o << "}";
    }

    o << "}";
    return o.str();
}

std::string tileItemsAtResponse(
    std::uint32_t x,
    std::uint32_t y,
    std::uint8_t z) {

    if (!g_profile || !g_profile->itemContractReady ||
        !g_profile->tileThingsRuntimeConfirmed)
        return R"({"ok":false,"cmd":"GET_TILE_ITEMS_AT","error":"tile_item_contract_not_ready"})";

    // This diagnostic intentionally refreshes the cache. It is also the same
    // resolution path used by Cavebot tile actions after Bridge V33.
    const auto items = scanCachedTileItems(true);
    const auto cache = tileCacheInfo();

    std::vector<TileItemDiagnostic> matches;
    for (const auto& item : items)
        if (item.x==x && item.y==y && item.z==z)
            matches.push_back(item);

    std::sort(matches.begin(),matches.end(),[](const auto& a,const auto& b){
        return a.stackIndex < b.stackIndex;
    });

    std::ostringstream o;
    o << "{\"ok\":true,\"cmd\":\"GET_TILE_ITEMS_AT\","
      << "\"x\":" << x
      << ",\"y\":" << y
      << ",\"z\":" << static_cast<unsigned>(z)
      << ",\"cacheGeneration\":" << cache.generation
      << ",\"cacheAgeMs\":" << cache.ageMs
      << ",\"count\":" << matches.size()
      << ",\"items\":[";

    for (std::size_t i=0;i<matches.size();++i) {
        if (i) o << ",";
        const auto& item=matches[i];
        o << "{\"id\":" << item.id
          << ",\"count\":" << item.count
          << ",\"stackIndex\":" << item.stackIndex
          << ",\"tileThingCount\":" << item.tileThingCount
          << "}";
    }
    o << "]}";
    return o.str();
}

std::uintptr_t resolveVisibleMonsterById(std::uint32_t id) {
    return resolveTileMonsterById(id);
}

bool executeAttackSelectedOnGameThread() {
    InterlockedExchange(&g_lastActionCode, 0);
    g_lastActionTargetId = 0;

    if (!g_profile || !g_buildMatched) {
        InterlockedExchange(&g_lastActionCode, 5);
        return false;
    }

    const auto& p = *g_profile;
    const auto game = g_base + p.gameRva;

    std::uintptr_t target = 0;

    // ATTACK_ID carries only the semantic creature ID across threads.
    // Resolve Monster* from the CURRENT cached Tile::things membership on the
    // actual game/window thread immediately before validation + Game::attack.
    // No Monster pointer discovered by the old broad heap scan is used here.
    if (g_pendingAttackRequestedId) {
        target = resolveVisibleMonsterById(g_pendingAttackRequestedId);
        if (!target) {
            InterlockedExchange(&g_lastActionCode, 10);
            return false;
        }
    } else {
        target = g_pendingAttackTarget;
        if (!target) {
            std::uintptr_t followTarget{};
            std::uintptr_t attackTarget{};
            safeRead(game + p.followTargetOffset, followTarget);
            safeRead(game + p.attackTargetOffset, attackTarget);
            target = followTarget ? followTarget : attackTarget;
        }
    }

    if (!target) {
        InterlockedExchange(&g_lastActionCode, 2);
        return false;
    }

    std::uint32_t id{};
    if (!targetSnapshot(target, id)) {
        InterlockedExchange(&g_lastActionCode, 3);
        return false;
    }
    if (g_pendingAttackRequestedId && id != g_pendingAttackRequestedId) {
        InterlockedExchange(&g_lastActionCode, 9);
        return false;
    }

    const auto functionAddress = g_base + p.attackMethodRva;
    if (!isExecutableAddress(functionAddress)) {
        InterlockedExchange(&g_lastActionCode, 4);
        return false;
    }

    g_lastActionTargetId = id;
    const auto fn = reinterpret_cast<GameAttackFn>(functionAddress);

    // IMPORTANT: Game::attack receives CreaturePtr BY VALUE on this OTC family.
    // shared_object_ptr is 4 bytes, so the actual stack value looks exactly like
    // a raw Creature*. The semantic difference is ownership: the normal caller
    // copy-constructs CreaturePtr (add_ref), and the callee destroys that
    // by-value parameter (dec_ref) on return. Calling with raw pointer bits
    // without the matching add_ref under-counts references and leaves
    // m_attackingCreature with invalid ownership after creatures disappear.
    const auto refAddress = target + p.sharedObjectRefCountOffset;
    std::uint32_t refsBefore{};
    if (!p.attackCreatureSharedValueConfirmed ||
        !p.sharedObjectRefCountOffset ||
        !safeRead(refAddress, refsBefore) ||
        refsBefore == 0 || refsBefore > 1000000u ||
        !isWritableAddress(refAddress)) {
        InterlockedExchange(&g_lastActionCode, 22);
        logLine("[TARGET TRACE] refusing attack: invalid shared_object refcount");
        return false;
    }

    const LONG refsAfterAdd =
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(refAddress));

    logLine("[TARGET TRACE] before Game::attack id=" + std::to_string(id) +
            " refsBefore=" + std::to_string(refsBefore) +
            " refsAfterAdd=" + std::to_string(refsAfterAdd));

    // Stack ABI remains pointer-sized: the single px field of CreaturePtr is
    // passed by value. The explicit add_ref above supplies the ownership step
    // that the original C++ caller performs before entering Game::attack.
    fn(reinterpret_cast<void*>(game), reinterpret_cast<void*>(target), false);

    std::uint32_t refsAfter{};
    safeRead(refAddress, refsAfter);
    logLine("[TARGET TRACE] Game::attack returned id=" + std::to_string(id) +
            " refsAfter=" + std::to_string(refsAfter));

    std::uintptr_t after{};
    safeRead(game + p.attackTargetOffset, after);
    if (after == target) {
        InterlockedExchange(&g_lastActionCode, 1);
        logLine("[ACTION] ATTACK_SELECTED success id=" + std::to_string(id));
        return true;
    }

    InterlockedExchange(&g_lastActionCode, 6);
    logLine("[ACTION] ATTACK_SELECTED returned but AttackTarget did not match");
    return false;
}


bool executeUseOnGameThread() {
    InterlockedExchange(&g_lastActionCode, 0);

    if (!g_profile || !g_buildMatched) {
        InterlockedExchange(&g_lastActionCode, 5);
        return false;
    }

    const auto& p = *g_profile;
    if (!p.useRuntimeConfirmed || !p.useItemSharedRefConfirmed ||
        !p.useMethodRva || !p.itemContractReady) {
        InterlockedExchange(&g_lastActionCode, 14);
        return false;
    }

    std::uintptr_t holder{}, item{};

    if (g_pendingUseContainer) {
        ContainerSnapshot source;
        if (!readContainerSnapshot(g_pendingUseContainer, source) ||
            source.id != g_pendingUseContainerId) {
            InterlockedExchange(&g_lastActionCode, 21);
            return false;
        }

        const ContainerItemSlot* sourceSlot = nullptr;
        for (const auto& slot : source.items) {
            if (slot.holderAddress == g_pendingUseHolder &&
                slot.itemAddress == g_pendingUseItem &&
                slot.id == g_pendingUseItemId) {
                sourceSlot = &slot;
                break;
            }
        }
        if (!sourceSlot) {
            InterlockedExchange(&g_lastActionCode, 21);
            return false;
        }

        holder = sourceSlot->holderAddress;
        item = sourceSlot->itemAddress;
    } else {
        if (!resolveTileItemHolder(g_pendingUseTile,
                                   g_pendingUseItemId,
                                   g_pendingUseItem,
                                   holder,
                                   item) ||
            !holder || !item) {
            InterlockedExchange(&g_lastActionCode, 11);
            return false;
        }
    }

    if (holder != g_pendingUseHolder || item != g_pendingUseItem) {
        InterlockedExchange(&g_lastActionCode, 12);
        return false;
    }

    const auto functionAddress = g_base + p.useMethodRva;
    if (!isExecutableAddress(functionAddress)) {
        InterlockedExchange(&g_lastActionCode, 13);
        return false;
    }

    const auto game = g_base + p.gameRva;
    const auto fn = reinterpret_cast<GameUseFn>(functionAddress);

    logLine("[ACTION] USE_ITEM game-thread itemId=" +
            std::to_string(static_cast<unsigned>(g_pendingUseItemId)));

    // Runtime ABI capture says Game::use takes Item shared_object_ptr_ref.
    // The 4-byte Tile::things element is the actual holder; pass its address,
    // never the raw Item*.
    fn(reinterpret_cast<void*>(game),
       reinterpret_cast<void*>(holder));

    InterlockedExchange(&g_lastActionCode, 1);
    return true;
}

bool executeUseWithSelfOnGameThread() {
    InterlockedExchange(&g_lastActionCode,0);

    if (!g_profile || !g_buildMatched) {
        InterlockedExchange(&g_lastActionCode,5);
        return false;
    }

    const auto& p=*g_profile;
    if (!p.useWithRuntimeConfirmed ||
        !p.useWithItemSharedRefConfirmed ||
        !p.useWithThingSharedRefConfirmed ||
        !p.useWithMethodRva ||
        !p.containerContractReady ||
        !p.itemContractReady ||
        !p.localPlayerVtableRva) {
        InterlockedExchange(&g_lastActionCode,26);
        return false;
    }

    ContainerSnapshot source;
    if (!readContainerSnapshot(g_pendingUseContainer,source) ||
        source.id!=g_pendingUseContainerId) {
        InterlockedExchange(&g_lastActionCode,21);
        return false;
    }

    const ContainerItemSlot* sourceSlot=nullptr;
    for (const auto& slot:source.items) {
        if (slot.holderAddress==g_pendingUseHolder &&
            slot.itemAddress==g_pendingUseItem &&
            slot.id==g_pendingUseItemId) {
            sourceSlot=&slot;
            break;
        }
    }
    if (!sourceSlot) {
        InterlockedExchange(&g_lastActionCode,21);
        return false;
    }

    const auto game=g_base+p.gameRva;
    std::uintptr_t localPlayer{};
    if (!safeRead(game+p.localPlayerOffset,localPlayer) || !localPlayer ||
        !exactObjectVtable(localPlayer,p.localPlayerVtableRva)) {
        InterlockedExchange(&g_lastActionCode,27);
        return false;
    }

    const auto functionAddress=g_base+p.useWithMethodRva;
    if (!isExecutableAddress(functionAddress)) {
        InterlockedExchange(&g_lastActionCode,28);
        return false;
    }

    // Game::useWith(const ItemPtr&, const ThingPtr&).
    // arg0 is the real holder from Container::items. For self, construct the
    // pointer-sized ThingPtr view expected by the const-ref ABI and keep it
    // alive for the complete native call. The LocalPlayer object itself is
    // still owned by Game::m_localPlayer.
    std::uintptr_t selfThingHolder=localPlayer;

    const auto fn=reinterpret_cast<GameUseWithFn>(functionAddress);

    logLine("[ACTION] USE_WITH_SELF itemId=" +
            std::to_string(static_cast<unsigned>(g_pendingUseItemId)) +
            " sourceContainer=" +
            std::to_string(g_pendingUseContainerId));

    fn(reinterpret_cast<void*>(game),
       reinterpret_cast<void*>(sourceSlot->holderAddress),
       reinterpret_cast<void*>(&selfThingHolder));

    InterlockedExchange(&g_lastActionCode,1);
    return true;
}

bool executeUseWithTileOnGameThread() {
    InterlockedExchange(&g_lastActionCode,0);

    if (!g_profile || !g_buildMatched) {
        InterlockedExchange(&g_lastActionCode,5);
        return false;
    }

    const auto& p=*g_profile;
    if (!p.useWithRuntimeConfirmed ||
        !p.useWithItemSharedRefConfirmed ||
        !p.useWithThingSharedRefConfirmed ||
        !p.useWithMethodRva ||
        !p.containerContractReady ||
        !p.itemContractReady) {
        InterlockedExchange(&g_lastActionCode,36);
        return false;
    }

    ContainerSnapshot source;
    if (!readContainerSnapshot(g_pendingUseContainer,source) ||
        source.id!=g_pendingUseContainerId) {
        InterlockedExchange(&g_lastActionCode,21);
        return false;
    }

    const ContainerItemSlot* sourceSlot=nullptr;
    for (const auto& slot:source.items) {
        if (slot.holderAddress==g_pendingUseHolder &&
            slot.itemAddress==g_pendingUseItem &&
            slot.id==g_pendingUseItemId) {
            sourceSlot=&slot;
            break;
        }
    }
    if (!sourceSlot) {
        InterlockedExchange(&g_lastActionCode,21);
        return false;
    }

    std::uintptr_t targetHolder{},targetItem{};
    if (!resolveTileItemHolder(
            g_pendingUseWithTargetTile,
            g_pendingUseWithTargetItemId,
            g_pendingUseWithTargetItem,
            targetHolder,
            targetItem) ||
        !targetHolder || !targetItem) {
        InterlockedExchange(&g_lastActionCode,37);
        return false;
    }

    if (targetHolder!=g_pendingUseWithTargetHolder ||
        targetItem!=g_pendingUseWithTargetItem) {
        InterlockedExchange(&g_lastActionCode,38);
        return false;
    }

    const auto functionAddress=g_base+p.useWithMethodRva;
    if (!isExecutableAddress(functionAddress)) {
        InterlockedExchange(&g_lastActionCode,28);
        return false;
    }

    const auto game=g_base+p.gameRva;
    const auto fn=reinterpret_cast<GameUseWithFn>(functionAddress);

    logLine("[ACTION] USE_WITH_TILE toolId=" +
            std::to_string(static_cast<unsigned>(g_pendingUseItemId)) +
            " targetId=" +
            std::to_string(static_cast<unsigned>(g_pendingUseWithTargetItemId)));

    fn(reinterpret_cast<void*>(game),
       reinterpret_cast<void*>(sourceSlot->holderAddress),
       reinterpret_cast<void*>(targetHolder));

    InterlockedExchange(&g_lastActionCode,1);
    return true;
}

bool executeAutoWalkOnGameThread() {
    InterlockedExchange(&g_lastActionCode,0);

    if (!g_profile || !g_buildMatched) {
        InterlockedExchange(&g_lastActionCode,5);
        return false;
    }

    const auto& p=*g_profile;
    if (!kAutoWalkActiveCallV40Implemented ||
        !p.autoWalkRuntimeConfirmed ||
        !p.autoWalkDirectionsVectorRefConfirmed ||
        !p.autoWalkStartPositionValueConfirmed ||
        p.autoWalkStackCleanupBytes!=16 ||
        !p.autoWalkMethodRva ||
        !p.localPlayerVtableRva) {
        InterlockedExchange(&g_lastActionCode,29);
        return false;
    }

    const auto game=g_base+p.gameRva;
    std::uintptr_t localPlayer{};
    if (!safeRead(game+p.localPlayerOffset,localPlayer) ||
        !localPlayer ||
        !exactObjectVtable(localPlayer,p.localPlayerVtableRva)) {
        InterlockedExchange(&g_lastActionCode,30);
        return false;
    }

    std::uint32_t x{},y{};
    std::uint8_t z{};
    if (!safeRead(localPlayer+p.posX,x) ||
        !safeRead(localPlayer+p.posY,y) ||
        !safeRead(localPlayer+p.posZ,z)) {
        InterlockedExchange(&g_lastActionCode,31);
        return false;
    }

    if (z!=g_pendingWalkZ) {
        InterlockedExchange(&g_lastActionCode,32);
        return false;
    }

    if (x==g_pendingWalkX && y==g_pendingWalkY) {
        InterlockedExchange(&g_lastActionCode,33);
        return true;
    }

    // V40 natural callsite proves the 12-byte Position value is copied from
    // the walk task before Directions& is pushed. Preserve the real starting
    // position before x/y are advanced while building the route.
    MovePosition startPos{};
    startPos.x=x;
    startPos.y=y;
    startPos.z=z;

    std::uint32_t directions[127]{};
    std::size_t count{};

    auto pushDirection=[&](std::uint32_t dir)->bool{
        if (count>=127) return false;
        directions[count++]=dir;
        return true;
    };

    while (x!=g_pendingWalkX || y!=g_pendingWalkY) {
        std::uint32_t dir{};

        if (x<g_pendingWalkX && y<g_pendingWalkY) {
            dir=5; ++x; ++y; // SouthEast
        } else if (x<g_pendingWalkX && y>g_pendingWalkY) {
            dir=4; ++x; --y; // NorthEast
        } else if (x>g_pendingWalkX && y<g_pendingWalkY) {
            dir=6; --x; ++y; // SouthWest
        } else if (x>g_pendingWalkX && y>g_pendingWalkY) {
            dir=7; --x; --y; // NorthWest
        } else if (x<g_pendingWalkX) {
            dir=1; ++x; // East
        } else if (x>g_pendingWalkX) {
            dir=3; --x; // West
        } else if (y<g_pendingWalkY) {
            dir=2; ++y; // South
        } else {
            dir=0; --y; // North
        }

        if (!pushDirection(dir)) {
            InterlockedExchange(&g_lastActionCode,34);
            return false;
        }
    }

    if (!count) {
        InterlockedExchange(&g_lastActionCode,33);
        return true;
    }

    MsvcVectorU32Ref32 dirs;
    dirs.first=directions;
    dirs.last=directions+count;
    dirs.end=directions+count;

    const auto functionAddress=g_base+p.autoWalkMethodRva;
    if (!isExecutableAddress(functionAddress)) {
        InterlockedExchange(&g_lastActionCode,35);
        return false;
    }

    const auto fn=reinterpret_cast<GameAutoWalkFn>(functionAddress);

    // Hard bridge-side re-entry guard. Even callers other than Crome cannot
    // hammer the native method while the previous walk has just been issued.
    const ULONGLONG nowTick=GetTickCount64();
    if (g_lastAutoWalkDispatchTick &&
        nowTick-g_lastAutoWalkDispatchTick<500) {
        InterlockedExchange(&g_lastActionCode,39);
        return false;
    }

    logLine("[ACTION] AUTO_WALK_TO target=" +
            std::to_string(g_pendingWalkX) + "," +
            std::to_string(g_pendingWalkY) + "," +
            std::to_string(static_cast<unsigned>(g_pendingWalkZ)) +
            " directions=" + std::to_string(count));

    fn(reinterpret_cast<void*>(game),&dirs,startPos);
    g_lastAutoWalkDispatchTick=GetTickCount64();

    InterlockedExchange(&g_lastActionCode,1);
    return true;
}

bool executeMoveOnGameThread() {
    InterlockedExchange(&g_lastActionCode, 0);

    if (!g_profile || !g_buildMatched) {
        InterlockedExchange(&g_lastActionCode, 5);
        return false;
    }

    const auto& p = *g_profile;
    if (!p.moveRuntimeConfirmed || !p.moveItemSharedRefConfirmed ||
        !p.movePositionRefConfirmed || !p.moveCountConfirmed ||
        !p.moveMethodRva || !p.containerContractReady) {
        InterlockedExchange(&g_lastActionCode, 18);
        return false;
    }

    ContainerSnapshot source;
    if (!readContainerSnapshot(g_pendingMoveSourceContainer, source)) {
        InterlockedExchange(&g_lastActionCode, 15);
        return false;
    }

    const ContainerItemSlot* sourceSlot = nullptr;
    for (const auto& slot : source.items) {
        if (slot.holderAddress == g_pendingMoveHolder &&
            slot.itemAddress == g_pendingMoveItem &&
            slot.id == g_pendingMoveItemId) {
            sourceSlot = &slot;
            break;
        }
    }
    if (!sourceSlot) {
        InterlockedExchange(&g_lastActionCode, 15);
        return false;
    }

    ContainerSnapshot target;
    if (!readContainerSnapshot(g_pendingMoveTargetContainer, target) ||
        target.id != g_pendingMoveTargetContainerId) {
        InterlockedExchange(&g_lastActionCode, 16);
        return false;
    }

    if (target.capacity && g_pendingMoveTargetSlot >= target.capacity) {
        InterlockedExchange(&g_lastActionCode, 19);
        return false;
    }

    const auto functionAddress = g_base + p.moveMethodRva;
    if (!isExecutableAddress(functionAddress)) {
        InterlockedExchange(&g_lastActionCode, 17);
        return false;
    }

    // OTClient container position convention. Live V15 validation confirmed
    // that x=0xFFFF, y=0x40|containerId, z=slot moves the requested stack
    // into the open destination container on this Ramonia build.
    MovePosition dest{};
    dest.x = 0xFFFFu;
    dest.y = 0x40u | (g_pendingMoveTargetContainerId & 0xFFu);
    dest.z = g_pendingMoveTargetSlot;

    std::uint32_t moveCount = g_pendingMoveCount;
    if (!moveCount) moveCount = sourceSlot->count ? sourceSlot->count : 1u;

    const auto game = g_base + p.gameRva;
    const auto fn = reinterpret_cast<GameMoveFn>(functionAddress);

    logLine("[ACTION] MOVE_ITEM game-thread itemId=" +
            std::to_string(static_cast<unsigned>(g_pendingMoveItemId)) +
            " count=" + std::to_string(moveCount) +
            " targetContainer=" + std::to_string(g_pendingMoveTargetContainerId) +
            " targetSlot=" + std::to_string(static_cast<unsigned>(g_pendingMoveTargetSlot)));

    // Runtime ABI capture says arg1 is shared_object_ptr_ref. Container::items
    // is a deque of 4-byte holders, so the deque element address is the
    // reference object expected by Game::move.
    fn(reinterpret_cast<void*>(game),
       reinterpret_cast<void*>(sourceSlot->holderAddress),
       &dest,
       moveCount);

    InterlockedExchange(&g_lastActionCode, 1);
    return true;
}

bool executeMoveTileToInventoryOnGameThread() {
    InterlockedExchange(&g_lastActionCode,0);

    if (!g_profile || !g_buildMatched) {
        InterlockedExchange(&g_lastActionCode,5);
        return false;
    }

    const auto& p=*g_profile;
    if (!p.moveRuntimeConfirmed ||
        !p.moveItemSharedRefConfirmed ||
        !p.movePositionRefConfirmed ||
        !p.moveCountConfirmed ||
        !p.moveMethodRva ||
        !p.itemContractReady ||
        !p.tileThingsRuntimeConfirmed ||
        !p.inventoryContractReady) {
        InterlockedExchange(&g_lastActionCode,40);
        return false;
    }

    if (g_pendingMoveInventorySlot!=5u &&
        g_pendingMoveInventorySlot!=6u) {
        InterlockedExchange(&g_lastActionCode,41);
        return false;
    }

    InventorySlotSnapshot hand;
    if (!readInventorySlot(g_pendingMoveInventorySlot,hand)) {
        InterlockedExchange(&g_lastActionCode,41);
        return false;
    }

    // Never replace a shield/weapon/other hand item. Stacking onto the same
    // item ID or filling an empty hand is allowed.
    if (hand.occupied && hand.id!=g_pendingMoveTileItemId) {
        InterlockedExchange(&g_lastActionCode,42);
        return false;
    }

    std::uintptr_t holder{},item{};
    if (!resolveTileItemHolder(
            g_pendingMoveSourceTile,
            g_pendingMoveTileItemId,
            g_pendingMoveTileExpectedItem,
            holder,
            item) ||
        !holder || !item) {
        InterlockedExchange(&g_lastActionCode,43);
        return false;
    }

    // Revalidate pickup distance on the game thread immediately before move.
    std::uint32_t px{},py{},ix{},iy{};
    std::uint8_t pz{},iz{};
    if (!localPlayerPosition(px,py,pz) ||
        !safeRead(item+p.posX,ix) ||
        !safeRead(item+p.posY,iy) ||
        !safeRead(item+p.posZ,iz)) {
        InterlockedExchange(&g_lastActionCode,44);
        return false;
    }

    const auto dx=ix>px ? ix-px : px-ix;
    const auto dy=iy>py ? iy-py : py-iy;
    if (iz!=pz || std::max(dx,dy)>1u) {
        InterlockedExchange(&g_lastActionCode,45);
        return false;
    }

    const auto functionAddress=g_base+p.moveMethodRva;
    if (!isExecutableAddress(functionAddress)) {
        InterlockedExchange(&g_lastActionCode,17);
        return false;
    }

    // OTC inventory-position convention under live validation:
    // x=0xFFFF, y=inventory slot (1..10), z=0.
    MovePosition dest{};
    dest.x=0xFFFFu;
    dest.y=g_pendingMoveInventorySlot;
    dest.z=0u;

    const auto game=g_base+p.gameRva;
    const auto fn=reinterpret_cast<GameMoveFn>(functionAddress);

    logLine("[ACTION] MOVE_TILE_TO_INVENTORY itemId=" +
            std::to_string(static_cast<unsigned>(g_pendingMoveTileItemId)) +
            " slot=" +
            std::to_string(static_cast<unsigned>(g_pendingMoveInventorySlot)) +
            " count=1");

    fn(reinterpret_cast<void*>(game),
       reinterpret_cast<void*>(holder),
       &dest,
       1u);

    InterlockedExchange(&g_lastActionCode,1);
    return true;
}

bool executeMoveTileToGroundOnGameThread() {
    InterlockedExchange(&g_lastActionCode,0);

    if(!g_profile || !g_buildMatched) {
        InterlockedExchange(&g_lastActionCode,5);
        return false;
    }

    const auto& p=*g_profile;
    if(!p.moveRuntimeConfirmed ||
       !p.moveItemSharedRefConfirmed ||
       !p.movePositionRefConfirmed ||
       !p.moveCountConfirmed ||
       !p.moveMethodRva ||
       !p.itemContractReady ||
       !p.tileThingsRuntimeConfirmed) {
        InterlockedExchange(&g_lastActionCode,46);
        return false;
    }

    std::uintptr_t holder{},item{};
    if(!resolveTileItemHolder(
            g_pendingMoveSourceTile,
            g_pendingMoveTileItemId,
            g_pendingMoveTileExpectedItem,
            holder,
            item) ||
       !holder || !item) {
        InterlockedExchange(&g_lastActionCode,43);
        return false;
    }

    std::uint32_t px{},py{},ix{},iy{};
    std::uint8_t pz{},iz{};
    if(!localPlayerPosition(px,py,pz) ||
       !safeRead(item+p.posX,ix) ||
       !safeRead(item+p.posY,iy) ||
       !safeRead(item+p.posZ,iz)) {
        InterlockedExchange(&g_lastActionCode,44);
        return false;
    }

    const auto dx=ix>px ? ix-px : px-ix;
    const auto dy=iy>py ? iy-py : py-iy;
    if(iz!=pz || std::max(dx,dy)>1u) {
        InterlockedExchange(&g_lastActionCode,45);
        return false;
    }

    if(!g_pendingMoveGroundX || !g_pendingMoveGroundY ||
       g_pendingMoveGroundZ!=iz) {
        InterlockedExchange(&g_lastActionCode,47);
        return false;
    }

    const auto ddx=g_pendingMoveGroundX>ix
        ? g_pendingMoveGroundX-ix : ix-g_pendingMoveGroundX;
    const auto ddy=g_pendingMoveGroundY>iy
        ? g_pendingMoveGroundY-iy : iy-g_pendingMoveGroundY;
    if(std::max(ddx,ddy)!=1u) {
        InterlockedExchange(&g_lastActionCode,47);
        return false;
    }

    const auto functionAddress=g_base+p.moveMethodRva;
    if(!isExecutableAddress(functionAddress)) {
        InterlockedExchange(&g_lastActionCode,17);
        return false;
    }

    MovePosition dest{};
    dest.x=g_pendingMoveGroundX;
    dest.y=g_pendingMoveGroundY;
    dest.z=g_pendingMoveGroundZ;

    std::uint32_t count=g_pendingMoveGroundCount;
    if(!count) count=1u;

    const auto game=g_base+p.gameRva;
    const auto fn=reinterpret_cast<GameMoveFn>(functionAddress);

    logLine("[ACTION] MOVE_TILE_TO_GROUND itemId=" +
            std::to_string(static_cast<unsigned>(g_pendingMoveTileItemId)) +
            " count=" + std::to_string(count) +
            " dest=" + std::to_string(dest.x) + "," +
            std::to_string(dest.y) + "," +
            std::to_string(static_cast<unsigned>(dest.z)));

    // Ground destination uses the normal world Position directly.
    // This exact ground->ground use is V36 experimental until live-confirmed.
    fn(reinterpret_cast<void*>(game),
       reinterpret_cast<void*>(holder),
       &dest,
       count);

    InterlockedExchange(&g_lastActionCode,1);
    return true;
}

bool executeTalkOnGameThread() {
    InterlockedExchange(&g_lastActionCode,0);

    if (!g_profile || !g_buildMatched) {
        InterlockedExchange(&g_lastActionCode,5);
        return false;
    }

    const auto& p=*g_profile;
    if (!p.talkChannelRuntimeConfirmed ||
        !p.talkModeArgConfirmed ||
        !p.talkChannelArgConfirmed ||
        !p.talkMessageMsvcStringRefConfirmed ||
        !p.talkChannelMethodRva) {
        InterlockedExchange(&g_lastActionCode,23);
        return false;
    }

    const std::size_t len=std::strlen(g_pendingTalkMessage);
    if (!len || len>15) {
        InterlockedExchange(&g_lastActionCode,24);
        return false;
    }

    MsvcSmallString32 message;
    std::memcpy(message.storage,g_pendingTalkMessage,len);
    message.storage[len]='\0';
    message.size=static_cast<std::uint32_t>(len);
    message.capacity=15;

    const auto functionAddress=g_base+p.talkChannelMethodRva;
    if (!isExecutableAddress(functionAddress)) {
        InterlockedExchange(&g_lastActionCode,25);
        return false;
    }

    const auto game=g_base+p.gameRva;
    const auto fn=reinterpret_cast<GameTalkChannelFn>(functionAddress);

    logLine("[ACTION] TALK_CHANNEL mode=" +
            std::to_string(g_pendingTalkMode) +
            " channel=" + std::to_string(g_pendingTalkChannel) +
            " message=" + std::string(g_pendingTalkMessage));

    fn(reinterpret_cast<void*>(game),
       g_pendingTalkMode,
       g_pendingTalkChannel,
       &message);

    InterlockedExchange(&g_lastActionCode,1);
    return true;
}


bool executeTopMoveReadOnGameThread() {
    InterlockedExchange(&g_lastActionCode,0);
    g_topMoveThing=0;
    g_topMoveVtable=0;
    g_topMoveItemId=0;
    g_topMoveItemCount=0;
    g_topMoveRefsAfterCall=0;
    g_topMoveRefsAfterRelease=0;
    g_topMoveWasItem=false;
    g_topMoveRefReleased=false;

    if (!g_profile || !g_buildMatched) {
        InterlockedExchange(&g_lastActionCode,50);
        return false;
    }

    const auto& p=*g_profile;
    if (!p.getTopMoveThingMethodRva || !p.tileVtableRva ||
        !g_pendingTopMoveTile ||
        !exactObjectVtable(g_pendingTopMoveTile,p.tileVtableRva)) {
        InterlockedExchange(&g_lastActionCode,51);
        return false;
    }

    const auto functionAddress=g_base+p.getTopMoveThingMethodRva;
    if (!isExecutableAddress(functionAddress)) {
        InterlockedExchange(&g_lastActionCode,52);
        return false;
    }

    // V39 static ABI evidence:
    //   ECX = Tile*
    //   [EBP+8] = hidden shared_object_ptr<Thing> return storage
    //   RET 4 = callee cleans the hidden storage pointer.
    std::uintptr_t returnedThing{};
    const auto fn=reinterpret_cast<TileGetTopMoveThingFn>(functionAddress);
    fn(reinterpret_cast<void*>(g_pendingTopMoveTile),&returnedThing);

    g_topMoveThing=returnedThing;
    if (!returnedThing) {
        InterlockedExchange(&g_lastActionCode,1);
        return true;
    }

    if (!safeRead(returnedThing,g_topMoveVtable)) {
        InterlockedExchange(&g_lastActionCode,53);
        return false;
    }

    if (p.itemContractReady &&
        g_topMoveVtable==g_base+p.itemVtableRva) {
        g_topMoveWasItem=true;
        safeRead(returnedThing+p.itemIdOffset,g_topMoveItemId);
        safeRead(returnedThing+p.itemCountOffset,g_topMoveItemCount);
    }

    // getTopMoveThing add_refs the returned Thing at +0x4. Balance only when
    // the count is readable, sane and still at least two references.
    const auto refAddress=returnedThing+0x4u;
    std::uint32_t refs{};
    if (safeRead(refAddress,refs)) {
        g_topMoveRefsAfterCall=refs;
        if (refs>=2u && refs<=1000000u && isWritableAddress(refAddress)) {
            const LONG after=InterlockedDecrement(
                reinterpret_cast<volatile LONG*>(refAddress));
            g_topMoveRefsAfterRelease=
                after>0 ? static_cast<std::uint32_t>(after) : 0u;
            g_topMoveRefReleased=true;
        }
    }

    InterlockedExchange(&g_lastActionCode,1);
    return true;
}

struct UncoverTileRequest {
    std::uintptr_t tile{};
    std::uint32_t x{},y{};
    std::uint8_t z{};
    std::uint16_t targetId{};
};
UncoverTileRequest g_uncoverTileRequest;
std::string g_uncoverTileResult;

// Same +0x4 intrusive refcount contract used by topmoveat. Keep the returned
// ThingPtr alive through validation and Game::move; balance sane counts on exit.
struct UncoverThingRef {
    std::uintptr_t thing{};
    bool releasable() const {
        std::uint32_t refs{};
        return thing && safeRead(thing+0x4u,refs) &&
               refs>=2u && refs<=1000000u && isWritableAddress(thing+0x4u);
    }
    ~UncoverThingRef() {
        if (releasable())
            InterlockedDecrement(reinterpret_cast<volatile LONG*>(thing+0x4u));
    }
};

bool executeUncoverTileOnGameThread() {
    InterlockedExchange(&g_lastActionCode,0);
    const auto request=g_uncoverTileRequest;
    auto fail=[](const char* error) {
        g_uncoverTileResult=std::string(
            R"({"ok":false,"cmd":"UNCOVER_TILE_TARGET_AT","moved":false,"exposed":false,"error":")")+
            error+R"("})";
        return false;
    };
    g_uncoverTileResult.clear();
    if (!g_profile || !g_buildMatched)
        return fail("build_not_validated");
    const auto& p=*g_profile;
    if (!p.itemContractReady || !p.tileThingsRuntimeConfirmed ||
        p.getTopMoveThingMethodRva!=0x197AD0u ||
        !exactObjectVtable(request.tile,p.tileVtableRva))
        return fail("top_move_contract_not_ready");

    // Re-read the source on the game thread. No cached target/blocker pointer
    // or stack order is used to decide what can move.
    std::vector<std::uintptr_t> things;
    bool containsLocalPlayer=false;
    if (!readStableTileThings(request.tile,things,containsLocalPlayer))
        return fail("tile_things_unavailable");
    auto matchesPosition=[&](std::uintptr_t thing) {
        std::uint32_t x{},y{};
        std::uint8_t z{};
        return safeRead(thing+p.posX,x) && safeRead(thing+p.posY,y) &&
               safeRead(thing+p.posZ,z) &&
               x==request.x && y==request.y && z==request.z;
    };
    auto hasTarget=[&](const std::vector<std::uintptr_t>& entries) {
        for (const auto thing:entries) {
            std::uint16_t id{};
            if (exactObjectVtable(thing,p.itemVtableRva) &&
                safeRead(thing+p.itemIdOffset,id) &&
                id==request.targetId && matchesPosition(thing))
                return true;
        }
        return false;
    };
    if (!hasTarget(things))
        return fail("target_item_not_found");

    const auto topMoveAddress=g_base+p.getTopMoveThingMethodRva;
    if (!isExecutableAddress(topMoveAddress))
        return fail("top_move_function_not_executable");
    UncoverThingRef returned;
    const auto topMove=reinterpret_cast<TileGetTopMoveThingFn>(topMoveAddress);
    // Runtime-confirmed: ECX=Tile*, hidden return storage at [EBP+8], RET 4.
    topMove(reinterpret_cast<void*>(request.tile),&returned.thing);
    if (!returned.thing)
        return fail("top_move_empty");
    if (!returned.releasable())
        return fail("top_move_refcount_invalid");

    if (!readStableTileThings(request.tile,things,containsLocalPlayer) ||
        !hasTarget(things) ||
        std::find(things.begin(),things.end(),returned.thing)==things.end())
        return fail("top_move_not_in_target_tile");
    if (!exactObjectVtable(returned.thing,p.itemVtableRva))
        return fail("top_move_is_not_item");
    std::uint16_t id{},count{};
    if (!safeRead(returned.thing+p.itemIdOffset,id) || !id ||
        !safeRead(returned.thing+p.itemCountOffset,count) ||
        !matchesPosition(returned.thing))
        return fail("top_move_item_invalid");

    std::ostringstream response;
    response << R"({"ok":true,"cmd":"UNCOVER_TILE_TARGET_AT","target":{"id":)"
             << request.targetId << R"(,"x":)" << request.x
             << R"(,"y":)" << request.y << R"(,"z":)"
             << static_cast<unsigned>(request.z)
             << R"(},"topMoveItemId":)" << id;
    if (id==request.targetId) {
        response << R"(,"exposed":true,"moved":false})";
        g_uncoverTileResult=response.str();
        InterlockedExchange(&g_lastActionCode,1);
        return true;
    }

    if (!p.moveRuntimeConfirmed || !p.moveItemSharedRefConfirmed ||
        !p.movePositionRefConfirmed || !p.moveCountConfirmed ||
        !p.moveMethodRva)
        return fail("tile_ground_move_contract_not_ready");
    const auto moveAddress=g_base+p.moveMethodRva;
    if (!isExecutableAddress(moveAddress))
        return fail("move_function_not_executable");
    std::uint32_t px{},py{};
    std::uint8_t pz{};
    if (!localPlayerPosition(px,py,pz) || pz!=request.z ||
        std::max(px>request.x ? px-request.x : request.x-px,
                 py>request.y ? py-request.y : request.y-py)>1u)
        return fail("tile_item_out_of_range");

    const auto destination=chooseAdjacentGroundDestination(
        request.x,request.y,request.z);
    if (!destination)
        return fail("no_adjacent_ground_destination");
    const auto destinationTile=cachedTileAtPosition(
        destination->x,destination->y,destination->z,false);
    std::vector<std::uintptr_t> destinationThings;
    bool destinationContainsPlayer=false;
    if (!destinationTile || *destinationTile==request.tile ||
        !exactObjectVtable(*destinationTile,p.tileVtableRva) ||
        !readStableTileThings(*destinationTile,destinationThings,destinationContainsPlayer) ||
        destinationThings.empty())
        return fail("tile_ground_destination_invalid");

    // Validate the source once more immediately before the single move call.
    if (!readStableTileThings(request.tile,things,containsLocalPlayer) ||
        !hasTarget(things) ||
        std::find(things.begin(),things.end(),returned.thing)==things.end() ||
        !matchesPosition(returned.thing) || !returned.releasable())
        return fail("source_tile_changed");

    MovePosition dest=*destination;
    const std::uint32_t moveCount=count ? count : 1u;
    const auto move=reinterpret_cast<GameMoveFn>(moveAddress);
    // Pass the retained shared_object_ptr<Item> by reference, as confirmed for
    // Game::move. Exactly one blocker; a subsequent request starts from zero.
    move(reinterpret_cast<void*>(g_base+p.gameRva),&returned.thing,&dest,moveCount);
    response << R"(,"exposed":false,"moved":true,"blocker":{"id":)"
             << id << R"(,"count":)" << moveCount
             << R"(},"destination":{"x":)" << dest.x
             << R"(,"y":)" << dest.y << R"(,"z":)"
             << static_cast<unsigned>(dest.z)
             << R"(},"destinationEncoding":"world_position"})";
    g_uncoverTileResult=response.str();
    InterlockedExchange(&g_lastActionCode,1);
    return true;
}


UncoverTileRequest g_pickupTileRequest;
std::string g_pickupTileResult;
std::string actionCodeText(LONG code);

bool executePickupTopMoveOnGameThread() {
    InterlockedExchange(&g_lastActionCode,0);
    const auto request=g_pickupTileRequest;
    std::uint16_t topMoveId{};
    bool exposed=false;
    auto fail=[&](const char* error) {
        std::ostringstream response;
        response << R"({"ok":false,"cmd":"MOVE_NEARBY_ITEM_TO_HAND","moved":false,"exposed":)"
                 << (exposed ? "true" : "false")
                 << R"(,"itemId":)" << request.targetId
                 << R"(,"topMoveItemId":)" << topMoveId
                 << R"(,"source":{"x":)" << request.x
                 << R"(,"y":)" << request.y << R"(,"z":)"
                 << static_cast<unsigned>(request.z)
                 << R"(},"error":")" << error << R"("})";
        g_pickupTileResult=response.str();
        return false;
    };
    g_pickupTileResult.clear();
    if (!g_profile || !g_buildMatched)
        return fail("build_not_validated");
    const auto& p=*g_profile;
    if (!p.itemContractReady || !p.tileThingsRuntimeConfirmed ||
        !p.inventoryContractReady || p.getTopMoveThingMethodRva!=0x197AD0u ||
        !exactObjectVtable(request.tile,p.tileVtableRva))
        return fail("pickup_top_move_contract_not_ready");
    auto matchesPosition=[&](std::uintptr_t thing) {
        std::uint32_t x{},y{};
        std::uint8_t z{};
        return safeRead(thing+p.posX,x) && safeRead(thing+p.posY,y) &&
               safeRead(thing+p.posZ,z) &&
               x==request.x && y==request.y && z==request.z;
    };
    auto hasTarget=[&](const std::vector<std::uintptr_t>& entries) {
        for (const auto thing:entries) {
            std::uint16_t id{};
            if (exactObjectVtable(thing,p.itemVtableRva) &&
                safeRead(thing+p.itemIdOffset,id) &&
                id==request.targetId && matchesPosition(thing))
                return true;
        }
        return false;
    };
    std::vector<std::uintptr_t> things;
    bool containsLocalPlayer=false;
    if (!readStableTileThings(request.tile,things,containsLocalPlayer) ||
        !hasTarget(things))
        return fail("target_item_not_found");
    const auto address=g_base+p.getTopMoveThingMethodRva;
    if (!isExecutableAddress(address))
        return fail("top_move_function_not_executable");
    UncoverThingRef returned;
    const auto topMove=reinterpret_cast<TileGetTopMoveThingFn>(address);
    topMove(reinterpret_cast<void*>(request.tile),&returned.thing);
    if (!returned.thing || !returned.releasable())
        return fail("top_move_return_invalid");
    if (!readStableTileThings(request.tile,things,containsLocalPlayer) ||
        !hasTarget(things) ||
        std::find(things.begin(),things.end(),returned.thing)==things.end())
        return fail("top_move_not_in_target_tile");
    if (!exactObjectVtable(returned.thing,p.itemVtableRva) ||
        !safeRead(returned.thing+p.itemIdOffset,topMoveId) ||
        !topMoveId || !matchesPosition(returned.thing))
        return fail("top_move_is_not_valid_item");
    if (topMoveId!=request.targetId)
        return fail("item_covered");
    exposed=true;

    InventorySlotSnapshot right,left;
    if (!readInventorySlot(5u,right) || !readInventorySlot(6u,left))
        return fail("inventory_read_failed");
    std::uint8_t slot{};
    if (right.occupied && right.id==request.targetId) slot=5u;
    else if (left.occupied && left.id==request.targetId) slot=6u;
    else if (!right.occupied) slot=5u;
    else if (!left.occupied) slot=6u;
    else return fail("both_hands_occupied_by_other_items");

    // Reuse the V35 ground->hand backend unchanged. It revalidates the hand,
    // exact source holder, distance and confirmed Game::move contract here,
    // in this same game-thread action while the TopMove ThingPtr is retained.
    g_pendingMoveSourceTile=request.tile;
    g_pendingMoveTileExpectedItem=returned.thing;
    g_pendingMoveTileItemId=request.targetId;
    g_pendingMoveInventorySlot=slot;
    const bool moved=executeMoveTileToInventoryOnGameThread();
    const LONG code=InterlockedCompareExchange(&g_lastActionCode,0,0);
    g_pendingMoveSourceTile=0;
    g_pendingMoveTileExpectedItem=0;
    g_pendingMoveTileItemId=0;
    g_pendingMoveInventorySlot=0;
    if (!moved || code!=1)
        return fail(actionCodeText(code).c_str());

    std::ostringstream response;
    response << R"({"ok":true,"cmd":"MOVE_NEARBY_ITEM_TO_HAND","moved":true,"exposed":true,"itemId":)"
             << request.targetId << R"(,"topMoveItemId":)" << topMoveId
             << R"(,"count":1,"source":{"x":)" << request.x
             << R"(,"y":)" << request.y << R"(,"z":)"
             << static_cast<unsigned>(request.z)
             << R"(},"destinationSlot":)" << static_cast<unsigned>(slot)
             << R"(,"destinationEncoding":"otclient_inventory_position","rightBefore":)"
             << (right.occupied ? right.id : 0u)
             << R"(,"leftBefore":)" << (left.occupied ? left.id : 0u) << "}";
    g_pickupTileResult=response.str();
    return true;
}


std::uint32_t g_pendingExposedUseX{},g_pendingExposedUseY{};
std::uint8_t g_pendingExposedUseZ{};

bool executeUseWithExposedTileOnGameThread() {
    InterlockedExchange(&g_lastActionCode,0);
    auto fail=[](LONG code) {
        InterlockedExchange(&g_lastActionCode,code);
        return false;
    };
    if (!g_profile || !g_buildMatched) return fail(5);
    const auto& p=*g_profile;
    const auto tile=g_pendingUseWithTargetTile;
    const auto target=g_pendingUseWithTargetItem;
    if (!p.itemContractReady || !p.tileThingsRuntimeConfirmed ||
        p.getTopMoveThingMethodRva!=0x197AD0u ||
        !exactObjectVtable(tile,p.tileVtableRva))
        return fail(60);
    std::vector<std::uintptr_t> things;
    bool containsPlayer=false;
    if (!readStableTileThings(tile,things,containsPlayer) ||
        std::find(things.begin(),things.end(),target)==things.end() ||
        !exactObjectVtable(target,p.itemVtableRva))
        return fail(61);
    std::uint16_t targetId{};
    std::uint32_t ix{},iy{},px{},py{};
    std::uint8_t iz{},pz{};
    if (!safeRead(target+p.itemIdOffset,targetId) ||
        targetId!=g_pendingUseWithTargetItemId ||
        !safeRead(target+p.posX,ix) || !safeRead(target+p.posY,iy) ||
        !safeRead(target+p.posZ,iz) ||
        ix!=g_pendingExposedUseX || iy!=g_pendingExposedUseY ||
        iz!=g_pendingExposedUseZ)
        return fail(61);
    if (!localPlayerPosition(px,py,pz) || pz!=iz ||
        std::max(px>ix ? px-ix : ix-px,py>iy ? py-iy : iy-py)>1u)
        return fail(63);
    const auto address=g_base+p.getTopMoveThingMethodRva;
    if (!isExecutableAddress(address)) return fail(60);
    UncoverThingRef returned;
    const auto topMove=reinterpret_cast<TileGetTopMoveThingFn>(address);
    topMove(reinterpret_cast<void*>(tile),&returned.thing);
    if (!returned.thing || !returned.releasable() ||
        !readStableTileThings(tile,things,containsPlayer) ||
        std::find(things.begin(),things.end(),returned.thing)==things.end() ||
        !exactObjectVtable(returned.thing,p.itemVtableRva))
        return fail(61);
    if (returned.thing!=target) return fail(62);
    // Keep the returned reference alive during the unchanged useWith backend.
    return executeUseWithTileOnGameThread();
}

LRESULT CALLBACK gameThreadGetMessageHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code >= 0 && lParam) {
        auto* msg = reinterpret_cast<MSG*>(lParam);
        if (msg->message == kActionMessage) {
            const LONG token = static_cast<LONG>(msg->wParam);
            const LONG expected =
                InterlockedCompareExchange(&g_pendingActionToken, 0, 0);

            if (token != 0 && token == expected) {
                const LONG kind =
                    InterlockedCompareExchange(&g_pendingActionKind, 0, 0);

                if (kind == PendingAttack)
                    executeAttackSelectedOnGameThread();
                else if (kind == PendingUse)
                    executeUseOnGameThread();
                else if (kind == PendingMove)
                    executeMoveOnGameThread();
                else if (kind == PendingTalk)
                    executeTalkOnGameThread();
                else if (kind == PendingUseWithSelf)
                    executeUseWithSelfOnGameThread();
                else if (kind == PendingAutoWalk)
                    executeAutoWalkOnGameThread();
                else if (kind == PendingUseWithTile)
                    executeUseWithTileOnGameThread();
                else if (kind == PendingUseWithExposedTile)
                    executeUseWithExposedTileOnGameThread();
                else if (kind == PendingMoveTileToInventory)
                    executeMoveTileToInventoryOnGameThread();
                else if (kind == PendingMoveTileToGround)
                    executeMoveTileToGroundOnGameThread();
                else if (kind == PendingTopMoveRead)
                    executeTopMoveReadOnGameThread();
                else if (kind == PendingUncoverTile)
                    executeUncoverTileOnGameThread();
                else if (kind == PendingPickupTopMove)
                    executePickupTopMoveOnGameThread();
                else
                    InterlockedExchange(&g_lastActionCode, 7);

                // Keep the V12 safety property: preserve the hook chain and
                // remove this hook immediately after the one action.
                const HHOOK hook = g_gameThreadHook;
                const LRESULT next = CallNextHookEx(hook, code, wParam, lParam);
                if (hook) {
                    UnhookWindowsHookEx(hook);
                    if (g_gameThreadHook == hook) g_gameThreadHook = nullptr;
                }
                logLine("[TARGET TRACE] hook removed after action");
                if (g_actionEvent) SetEvent(g_actionEvent);
                return next;
            }
        }
    }
    return CallNextHookEx(g_gameThreadHook, code, wParam, lParam);
}
bool dispatcherReady() {
    return g_gameWindow && IsWindow(g_gameWindow) && g_gameThreadId != 0;
}

bool prepareGameThreadDispatcher() {
    // Re-resolve the current game window/thread for every on-demand action.
    // Do not carry a cached thread id across map/floor transitions.
    const HWND currentWindow = findCurrentProcessWindow();
    if (!currentWindow) {
        logLine("[DISPATCH] main game window not found");
        g_gameWindow = nullptr;
        g_gameThreadId = 0;
        return false;
    }

    const DWORD currentThreadId =
        GetWindowThreadProcessId(currentWindow, nullptr);
    if (!currentThreadId) {
        logLine("[DISPATCH] game window thread id unavailable");
        g_gameWindow = nullptr;
        g_gameThreadId = 0;
        return false;
    }

    const bool changed =
        currentWindow != g_gameWindow ||
        currentThreadId != g_gameThreadId;

    g_gameWindow = currentWindow;
    g_gameThreadId = currentThreadId;

    if (changed) {
        logLine("[DISPATCH] on-demand game-thread target refreshed tid=" +
                std::to_string(g_gameThreadId));
    }
    return true;
}

void cleanupOnDemandDispatcher() {
    if (g_gameThreadHook) {
        const HHOOK hook = g_gameThreadHook;
        g_gameThreadHook = nullptr;
        UnhookWindowsHookEx(hook);
    }
    if (g_actionEvent) {
        const HANDLE event = g_actionEvent;
        g_actionEvent = nullptr;
        CloseHandle(event);
    }
}

bool dispatchPendingActionToGameThread(DWORD& win32Error) {
    win32Error = ERROR_SUCCESS;

    if (!prepareGameThreadDispatcher()) {
        win32Error = ERROR_INVALID_WINDOW_HANDLE;
        InterlockedExchange(&g_pendingActionKind, PendingNone);
        return false;
    }

    // V12: no hook/event remains installed while the bridge is idle.
    cleanupOnDemandDispatcher();

    g_actionEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_actionEvent) {
        win32Error = GetLastError();
        InterlockedExchange(&g_pendingActionKind, PendingNone);
        return false;
    }

    SetLastError(0);
    g_gameThreadHook = SetWindowsHookExW(
        WH_GETMESSAGE,
        &gameThreadGetMessageHook,
        nullptr,
        g_gameThreadId);
    if (!g_gameThreadHook) {
        win32Error = GetLastError();
        InterlockedExchange(&g_pendingActionKind, PendingNone);
        cleanupOnDemandDispatcher();
        return false;
    }

    logLine("[TARGET TRACE] hook installed tid=" +
            std::to_string(g_gameThreadId));

    ResetEvent(g_actionEvent);
    InterlockedExchange(&g_lastActionCode, 0);

    LONG token = InterlockedIncrement(&g_actionTokenCounter);
    if (token == 0) token = InterlockedIncrement(&g_actionTokenCounter);
    InterlockedExchange(&g_pendingActionToken, token);

    SetLastError(0);
    if (!PostThreadMessageW(
            g_gameThreadId,
            kActionMessage,
            static_cast<WPARAM>(static_cast<ULONG_PTR>(token)),
            0)) {
        win32Error = GetLastError();
        InterlockedCompareExchange(&g_pendingActionToken, 0, token);
        InterlockedExchange(&g_pendingActionKind, PendingNone);
        cleanupOnDemandDispatcher();
        return false;
    }

    const DWORD wait = WaitForSingleObject(g_actionEvent, 3000);
    if (wait != WAIT_OBJECT_0) {
        win32Error = (wait == WAIT_TIMEOUT) ? ERROR_TIMEOUT : GetLastError();
        InterlockedCompareExchange(&g_pendingActionToken, 0, token);
        InterlockedExchange(&g_pendingActionKind, PendingNone);
        cleanupOnDemandDispatcher();
        return false;
    }

    InterlockedCompareExchange(&g_pendingActionToken, 0, token);

    // The hook normally removes itself in the callback. This is only a safety
    // cleanup for unexpected paths.
    cleanupOnDemandDispatcher();
    InterlockedExchange(&g_pendingActionKind, PendingNone);
    return true;
}

std::string actionCodeText(LONG code) {
    switch (code) {
        case 1: return "success";
        case 2: return "no_selected_target";
        case 3: return "selected_target_is_not_confirmed_monster";
        case 4: return "attack_function_not_executable";
        case 5: return "build_not_validated";
        case 6: return "attack_target_not_updated";
        case 7: return "dispatcher_not_ready";
        case 8: return "dispatcher_timeout";
        case 9: return "creature_id_mismatch";
        case 10: return "creature_id_not_visible";
        case 11: return "loot_item_not_visible";
        case 12: return "item_holder_changed";
        case 13: return "use_function_not_executable";
        case 14: return "use_contract_not_ready";
        case 15: return "move_source_invalid";
        case 16: return "move_target_invalid";
        case 17: return "move_function_not_executable";
        case 18: return "move_contract_not_ready";
        case 19: return "move_target_full";
        case 20: return "loot_candidate_missing";
        case 21: return "container_item_not_visible";
        case 22: return "attack_shared_object_refcount_invalid";
        case 23: return "talk_contract_not_ready";
        case 24: return "talk_message_invalid";
        case 25: return "talk_function_not_executable";
        case 26: return "usewith_self_contract_not_ready";
        case 27: return "local_player_holder_invalid";
        case 28: return "usewith_function_not_executable";
        case 29: return "autowalk_contract_not_ready";
        case 30: return "autowalk_local_player_invalid";
        case 31: return "autowalk_position_read_failed";
        case 32: return "autowalk_wrong_floor";
        case 33: return "autowalk_already_reached";
        case 34: return "autowalk_path_too_long";
        case 35: return "autowalk_function_not_executable";
        case 36: return "usewith_tile_contract_not_ready";
        case 37: return "usewith_tile_target_not_visible";
        case 38: return "usewith_tile_target_changed";
        case 39: return "autowalk_cooldown";
        case 40: return "tile_inventory_move_contract_not_ready";
        case 41: return "inventory_hand_slot_invalid";
        case 42: return "inventory_hand_occupied_by_other_item";
        case 43: return "tile_pickup_item_changed";
        case 44: return "tile_pickup_position_read_failed";
        case 45: return "tile_pickup_item_out_of_range";
        case 46: return "tile_ground_move_contract_not_ready";
        case 47: return "tile_ground_destination_invalid";
        case 60: return "usewith_exposure_contract_not_ready";
        case 61: return "usewith_exposure_target_invalid";
        case 62: return "item_covered";
        case 63: return "usewith_target_out_of_range";
        default: return "unknown_action_result";
    }
}

std::string jsonEscape(const std::string& s) {
    std::ostringstream o;
    for (unsigned char c : s) {
        switch (c) {
            case '\\': o << "\\\\"; break;
            case '"': o << "\\\""; break;
            case '\n': o << "\\n"; break;
            case '\r': o << "\\r"; break;
            case '\t': o << "\\t"; break;
            default:
                if (c < 0x20) o << '?';
                else o << static_cast<char>(c);
        }
    }
    return o.str();
}

std::optional<std::uint32_t> uint32FieldFromRequest(
    const std::string& request, const char* field) {
    try {
        const auto root = crome::json::parse(request);
        if (!root.isObject()) return std::nullopt;
        const auto n = integerValue(root.get(field));
        if (!n || *n == 0 || *n > 0xFFFFFFFFull) return std::nullopt;
        return static_cast<std::uint32_t>(*n);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::uint32_t> uint32FieldAllowZeroFromRequest(
    const std::string& request, const char* field) {
    try {
        const auto root = crome::json::parse(request);
        if (!root.isObject()) return std::nullopt;
        const auto n = integerValue(root.get(field));
        if (!n || *n > 0xFFFFFFFFull) return std::nullopt;
        return static_cast<std::uint32_t>(*n);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::string> stringFieldFromRequest(
    const std::string& request,const char* field) {
    try {
        const auto root=crome::json::parse(request);
        if (!root.isObject()) return std::nullopt;
        const auto* v=root.get(field);
        if (!v || !v->isString()) return std::nullopt;
        return v->string();
    } catch (...) {
        return std::nullopt;
    }
}

std::string commandFromRequest(const std::string& request) {
    try {
        const auto root = crome::json::parse(request);
        if (root.isObject()) {
            if (const auto* v = root.get("cmd"); v && v->isString()) return v->string();
        }
    } catch (...) {}
    std::string s = request;
    s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c){ return std::isspace(c); }), s.end());
    return s;
}

std::string pipeName() {
    return "\\\\.\\pipe\\Crome.Ramonia." + std::to_string(GetCurrentProcessId());
}

std::string playerResponse(const Profile& p, std::uintptr_t base) {
    const auto game = base + p.gameRva;
    std::uintptr_t localPlayer{};
    if (!safeRead(game + p.localPlayerOffset, localPlayer) || !localPlayer)
        return R"({"ok":false,"cmd":"GET_PLAYER","error":"local_player_unavailable"})";

    std::uint32_t x{}, y{};
    std::uint8_t z{};
    double hp{}, maxHp{}, mana{}, maxMana{}, soul{};
    const bool ok =
        safeRead(localPlayer + p.posX, x) &&
        safeRead(localPlayer + p.posY, y) &&
        safeRead(localPlayer + p.posZ, z) &&
        readNumber(localPlayer + p.health.offset, p.health.type, hp) &&
        readNumber(localPlayer + p.maxHealth.offset, p.maxHealth.type, maxHp) &&
        readNumber(localPlayer + p.mana.offset, p.mana.type, mana) &&
        readNumber(localPlayer + p.maxMana.offset, p.maxMana.type, maxMana);

    if (!ok) return R"({"ok":false,"cmd":"GET_PLAYER","error":"snapshot_read_failed"})";

    const bool soulAvailable =
        p.soul.valid &&
        readNumber(localPlayer + p.soul.offset,p.soul.type,soul) &&
        std::isfinite(soul) &&
        soul >= 0.0;

    std::ostringstream o;
    o << std::fixed << std::setprecision(2)
      << "{\"ok\":true,\"cmd\":\"GET_PLAYER\",\"player\":{"
      << "\"health\":" << hp << ",\"maxHealth\":" << maxHp
      << ",\"mana\":" << mana << ",\"maxMana\":" << maxMana
      << ",\"soulAvailable\":" << (soulAvailable ? "true" : "false");

    if (soulAvailable)
        o << ",\"soul\":" << soul;

    o << ",\"position\":{\"x\":" << x << ",\"y\":" << y << ",\"z\":"
      << static_cast<unsigned>(z) << "}}}";
    return o.str();
}

std::string combatTargetsResponse(const Profile& p) {
    const auto game = g_base + p.gameRva;
    std::uintptr_t attackTarget{};
    std::uintptr_t followTarget{};
    safeRead(game + p.attackTargetOffset, attackTarget);
    safeRead(game + p.followTargetOffset, followTarget);

    std::uint32_t attackId{};
    std::uint32_t followId{};
    const bool attackMonster = targetSnapshot(attackTarget, attackId);
    const bool followMonster = targetSnapshot(followTarget, followId);

    std::ostringstream o;
    o << "{\"ok\":true,\"cmd\":\"GET_COMBAT_TARGETS\","
      << "\"attack\":{\"present\":" << (attackTarget ? "true" : "false")
      << ",\"monster\":" << (attackMonster ? "true" : "false")
      << ",\"id\":" << attackId << "},"
      << "\"follow\":{\"present\":" << (followTarget ? "true" : "false")
      << ",\"monster\":" << (followMonster ? "true" : "false")
      << ",\"id\":" << followId << "}}";
    return o.str();
}

std::string visibleMonstersResponse() {
    const auto tileMonsters = scanCachedTileMonsters(15);
    std::ostringstream o;
    o << "{\"ok\":true,\"cmd\":\"GET_VISIBLE_MONSTERS\","
      << "\"source\":\"Tile::things-cache\",\"count\":" << tileMonsters.size()
      << ",\"monsters\":[";
    for (std::size_t i = 0; i < tileMonsters.size() && i < 32; ++i) {
        if (i) o << ",";
        const auto& m = tileMonsters[i].monster;
        o << "{\"id\":" << m.id
          << ",\"name\":\"" << jsonEscape(m.name) << "\""
          << ",\"hpPercent\":" << static_cast<unsigned>(m.hpPercent)
          << ",\"x\":" << m.x
          << ",\"y\":" << m.y
          << ",\"z\":" << static_cast<unsigned>(m.z)
          << ",\"distance\":" << m.distance << "}";
    }
    o << "]}";
    return o.str();
}


std::string openContainersResponse() {
    if (!g_profile || !g_profile->containerContractReady)
        return R"({"ok":false,"cmd":"GET_OPEN_CONTAINERS","error":"container_contract_not_ready"})";

    const auto containers = scanOpenContainers();
    std::ostringstream o;
    o << "{\"ok\":true,\"cmd\":\"GET_OPEN_CONTAINERS\",\"count\":"
      << containers.size() << ",\"containers\":[";

    for (std::size_t i = 0; i < containers.size() && i < 32; ++i) {
        if (i) o << ",";
        const auto& c = containers[i];
        o << "{\"id\":" << c.id
          << ",\"name\":\"" << jsonEscape(c.name) << "\""
          << ",\"capacity\":" << c.capacity
          << ",\"itemCount\":" << c.items.size()
          << ",\"items\":[";

        for (std::size_t j = 0; j < c.items.size() && j < 100; ++j) {
            if (j) o << ",";
            const auto& item = c.items[j];
            o << "{\"slot\":" << item.index
              << ",\"id\":" << item.id
              << ",\"count\":" << item.count << "}";
        }
        o << "]}";
    }
    o << "]}";
    return o.str();
}

std::string useLootItemResponse(std::uint16_t itemId) {
    if (!g_profile || !g_buildMatched)
        return R"({"ok":false,"cmd":"USE_LOOT_ITEM","error":"build_not_validated"})";
    if (!g_profile->useRuntimeConfirmed ||
        !g_profile->useItemSharedRefConfirmed ||
        !g_profile->itemContractReady)
        return R"({"ok":false,"cmd":"USE_LOOT_ITEM","error":"use_contract_not_ready"})";
    if (!prepareGameThreadDispatcher())
        return R"({"ok":false,"cmd":"USE_LOOT_ITEM","error":"dispatcher_not_ready"})";

    std::vector<TileItemDiagnostic> candidates;
    AcquireSRWLockShared(&g_lootCandidateLock);
    candidates = g_lastLootCandidates;
    ReleaseSRWLockShared(&g_lootCandidateLock);

    std::optional<TileItemDiagnostic> chosen;
    for (auto it = candidates.rbegin(); it != candidates.rend(); ++it) {
        if (it->id == itemId) {
            chosen = *it;
            break;
        }
    }
    if (!chosen)
        return R"({"ok":false,"cmd":"USE_LOOT_ITEM","error":"loot_candidate_missing","hint":"run_LOOT_MARK_then_LOOT_DIFF"})";

    std::uintptr_t holder{}, item{};
    if (!resolveTileItemHolder(chosen->tileAddress,
                               itemId,
                               chosen->address,
                               holder,
                               item) ||
        !holder || !item)
        return R"({"ok":false,"cmd":"USE_LOOT_ITEM","error":"loot_item_not_visible"})";

    const auto before = scanOpenContainers();

    g_pendingUseTile = chosen->tileAddress;
    g_pendingUseContainer = 0;
    g_pendingUseContainerId = 0;
    g_pendingUseHolder = holder;
    g_pendingUseItem = item;
    g_pendingUseItemId = itemId;
    InterlockedExchange(&g_pendingActionKind, PendingUse);

    DWORD win32{};
    const bool dispatched = dispatchPendingActionToGameThread(win32);
    const LONG code = InterlockedCompareExchange(&g_lastActionCode, 0, 0);

    g_pendingUseTile = 0;
    g_pendingUseContainer = 0;
    g_pendingUseContainerId = 0;
    g_pendingUseHolder = 0;
    g_pendingUseItem = 0;
    g_pendingUseItemId = 0;

    if (!dispatched) {
        std::ostringstream o;
        o << "{\"ok\":false,\"cmd\":\"USE_LOOT_ITEM\","
          << "\"error\":\"dispatcher_timeout\",\"win32\":" << win32 << "}";
        return o.str();
    }

    Sleep(250);
    const auto after = scanOpenContainers();

    std::ostringstream o;
    o << "{\"ok\":" << (code == 1 ? "true" : "false")
      << ",\"cmd\":\"USE_LOOT_ITEM\""
      << ",\"result\":\"" << actionCodeText(code) << "\""
      << ",\"itemId\":" << itemId
      << ",\"tileSlot\":" << chosen->tileSlot
      << ",\"openContainersBefore\":" << before.size()
      << ",\"openContainersAfter\":" << after.size()
      << ",\"holderAbi\":\"shared_object_ptr_ref\"}";
    return o.str();
}

std::string useContainerItemResponse(
    const std::optional<std::uint32_t>& sourceContainerId,
    std::uint16_t itemId) {
    if (!g_profile || !g_buildMatched)
        return R"({"ok":false,"cmd":"USE_CONTAINER_ITEM","error":"build_not_validated"})";
    if (!g_profile->useRuntimeConfirmed ||
        !g_profile->useItemSharedRefConfirmed ||
        !g_profile->itemContractReady ||
        !g_profile->containerContractReady)
        return R"({"ok":false,"cmd":"USE_CONTAINER_ITEM","error":"use_contract_not_ready"})";
    if (!prepareGameThreadDispatcher())
        return R"({"ok":false,"cmd":"USE_CONTAINER_ITEM","error":"dispatcher_not_ready"})";

    const auto containers = scanOpenContainers();

    const ContainerSnapshot* source = nullptr;
    const ContainerItemSlot* sourceSlot = nullptr;
    bool requestedContainerOpen = !sourceContainerId.has_value();

    for (const auto& container : containers) {
        if (sourceContainerId && container.id != *sourceContainerId)
            continue;

        if (sourceContainerId && container.id == *sourceContainerId)
            requestedContainerOpen = true;

        for (const auto& slot : container.items) {
            if (slot.id == itemId) {
                source = &container;
                sourceSlot = &slot;
                break;
            }
        }
        if (sourceSlot) break;
    }

    if (!requestedContainerOpen)
        return R"({"ok":false,"cmd":"USE_CONTAINER_ITEM","error":"source_container_not_open"})";
    if (!source || !sourceSlot)
        return R"({"ok":false,"cmd":"USE_CONTAINER_ITEM","error":"item_not_found_in_open_containers"})";

    g_pendingUseTile = 0;
    g_pendingUseContainer = source->address;
    g_pendingUseContainerId = source->id;
    g_pendingUseHolder = sourceSlot->holderAddress;
    g_pendingUseItem = sourceSlot->itemAddress;
    g_pendingUseItemId = sourceSlot->id;
    InterlockedExchange(&g_pendingActionKind, PendingUse);

    DWORD win32{};
    const bool dispatched = dispatchPendingActionToGameThread(win32);
    const LONG code = InterlockedCompareExchange(&g_lastActionCode, 0, 0);

    const auto usedContainerId = source->id;
    const auto usedSlot = sourceSlot->index;
    const auto usedCount = sourceSlot->count;

    g_pendingUseTile = 0;
    g_pendingUseContainer = 0;
    g_pendingUseContainerId = 0;
    g_pendingUseHolder = 0;
    g_pendingUseItem = 0;
    g_pendingUseItemId = 0;

    if (!dispatched) {
        std::ostringstream o;
        o << "{\"ok\":false,\"cmd\":\"USE_CONTAINER_ITEM\","
          << "\"error\":\"dispatcher_timeout\",\"win32\":" << win32 << "}";
        return o.str();
    }

    std::ostringstream o;
    o << "{\"ok\":" << (code == 1 ? "true" : "false")
      << ",\"cmd\":\"USE_CONTAINER_ITEM\""
      << ",\"result\":\"" << actionCodeText(code) << "\""
      << ",\"sourceContainerId\":" << usedContainerId
      << ",\"slot\":" << usedSlot
      << ",\"itemId\":" << itemId
      << ",\"count\":" << usedCount
      << ",\"holderAbi\":\"shared_object_ptr_ref\""
      << ",\"source\":\"open_container\"}";
    return o.str();
}

std::string useContainerItemOnSelfResponse(
    const std::optional<std::uint32_t>& sourceContainerId,
    std::uint16_t itemId) {

    if (!g_profile || !g_buildMatched)
        return R"({"ok":false,"cmd":"USE_CONTAINER_ITEM_ON_SELF","error":"build_not_validated"})";
    if (!g_profile->useWithRuntimeConfirmed ||
        !g_profile->useWithItemSharedRefConfirmed ||
        !g_profile->useWithThingSharedRefConfirmed ||
        !g_profile->containerContractReady ||
        !g_profile->itemContractReady ||
        !g_profile->localPlayerVtableRva)
        return R"({"ok":false,"cmd":"USE_CONTAINER_ITEM_ON_SELF","error":"usewith_self_contract_not_ready"})";
    if (!prepareGameThreadDispatcher())
        return R"({"ok":false,"cmd":"USE_CONTAINER_ITEM_ON_SELF","error":"dispatcher_not_ready"})";

    const auto containers=scanOpenContainers();

    const ContainerSnapshot* source=nullptr;
    const ContainerItemSlot* sourceSlot=nullptr;
    bool requestedContainerOpen=!sourceContainerId.has_value();

    for (const auto& container:containers) {
        if (sourceContainerId && container.id!=*sourceContainerId)
            continue;

        if (sourceContainerId && container.id==*sourceContainerId)
            requestedContainerOpen=true;

        for (const auto& slot:container.items) {
            if (slot.id==itemId) {
                source=&container;
                sourceSlot=&slot;
                break;
            }
        }
        if (sourceSlot) break;
    }

    if (!requestedContainerOpen)
        return R"({"ok":false,"cmd":"USE_CONTAINER_ITEM_ON_SELF","error":"source_container_not_open"})";
    if (!source || !sourceSlot)
        return R"({"ok":false,"cmd":"USE_CONTAINER_ITEM_ON_SELF","error":"item_not_found_in_open_containers"})";

    g_pendingUseTile=0;
    g_pendingUseContainer=source->address;
    g_pendingUseContainerId=source->id;
    g_pendingUseHolder=sourceSlot->holderAddress;
    g_pendingUseItem=sourceSlot->itemAddress;
    g_pendingUseItemId=sourceSlot->id;
    InterlockedExchange(&g_pendingActionKind,PendingUseWithSelf);

    DWORD win32{};
    const bool dispatched=dispatchPendingActionToGameThread(win32);
    const LONG code=InterlockedCompareExchange(&g_lastActionCode,0,0);

    const auto usedContainerId=source->id;
    const auto usedSlot=sourceSlot->index;
    const auto usedCount=sourceSlot->count;

    g_pendingUseTile=0;
    g_pendingUseContainer=0;
    g_pendingUseContainerId=0;
    g_pendingUseHolder=0;
    g_pendingUseItem=0;
    g_pendingUseItemId=0;

    if (!dispatched) {
        std::ostringstream o;
        o << "{\"ok\":false,\"cmd\":\"USE_CONTAINER_ITEM_ON_SELF\","
          << "\"error\":\"dispatcher_timeout\",\"win32\":" << win32 << "}";
        return o.str();
    }

    std::ostringstream o;
    o << "{\"ok\":" << (code==1?"true":"false")
      << ",\"cmd\":\"USE_CONTAINER_ITEM_ON_SELF\""
      << ",\"result\":\"" << actionCodeText(code) << "\""
      << ",\"sourceContainerId\":" << usedContainerId
      << ",\"slot\":" << usedSlot
      << ",\"itemId\":" << itemId
      << ",\"count\":" << usedCount
      << ",\"itemHolderAbi\":\"shared_object_ptr_ref\""
      << ",\"targetHolderAbi\":\"shared_object_ptr_ref\""
      << ",\"target\":\"self\"}";
    return o.str();
}

std::string moveContainerItemResponse(std::uint32_t sourceContainerId,
                                      std::uint32_t targetContainerId,
                                      std::uint16_t itemId) {
    if (!g_profile || !g_buildMatched)
        return R"({"ok":false,"cmd":"MOVE_CONTAINER_ITEM","error":"build_not_validated"})";
    if (!g_profile->moveRuntimeConfirmed ||
        !g_profile->moveItemSharedRefConfirmed ||
        !g_profile->movePositionRefConfirmed ||
        !g_profile->moveCountConfirmed ||
        !g_profile->containerContractReady)
        return R"({"ok":false,"cmd":"MOVE_CONTAINER_ITEM","error":"move_contract_not_ready"})";
    if (!prepareGameThreadDispatcher())
        return R"({"ok":false,"cmd":"MOVE_CONTAINER_ITEM","error":"dispatcher_not_ready"})";

    const auto containers = scanOpenContainers();

    const ContainerSnapshot* source = nullptr;
    const ContainerSnapshot* target = nullptr;
    for (const auto& c : containers) {
        if (c.id == sourceContainerId && !source) source = &c;
        if (c.id == targetContainerId && !target) target = &c;
    }

    if (!source)
        return R"({"ok":false,"cmd":"MOVE_CONTAINER_ITEM","error":"source_container_not_open"})";
    if (!target)
        return R"({"ok":false,"cmd":"MOVE_CONTAINER_ITEM","error":"target_container_not_open"})";
    if (sourceContainerId == targetContainerId)
        return R"({"ok":false,"cmd":"MOVE_CONTAINER_ITEM","error":"source_equals_target"})";

    const ContainerItemSlot* sourceSlot = nullptr;
    for (const auto& item : source->items) {
        if (item.id == itemId) {
            sourceSlot = &item;
            break;
        }
    }
    if (!sourceSlot)
        return R"({"ok":false,"cmd":"MOVE_CONTAINER_ITEM","error":"item_not_found_in_source"})";

    const std::uint32_t targetSlot =
        static_cast<std::uint32_t>(target->items.size());
    if (target->capacity && targetSlot >= target->capacity)
        return R"({"ok":false,"cmd":"MOVE_CONTAINER_ITEM","error":"target_container_full"})";
    if (targetSlot > 255u)
        return R"({"ok":false,"cmd":"MOVE_CONTAINER_ITEM","error":"target_slot_out_of_range"})";

    g_pendingMoveSourceContainer = source->address;
    g_pendingMoveHolder = sourceSlot->holderAddress;
    g_pendingMoveItem = sourceSlot->itemAddress;
    g_pendingMoveItemId = sourceSlot->id;
    g_pendingMoveCount = sourceSlot->count ? sourceSlot->count : 1u;
    g_pendingMoveTargetContainer = target->address;
    g_pendingMoveTargetContainerId = target->id;
    g_pendingMoveTargetSlot = static_cast<std::uint8_t>(targetSlot);
    InterlockedExchange(&g_pendingActionKind, PendingMove);

    DWORD win32{};
    const bool dispatched = dispatchPendingActionToGameThread(win32);
    const LONG code = InterlockedCompareExchange(&g_lastActionCode, 0, 0);

    const auto movedCount = g_pendingMoveCount;
    const auto destinationSlot = g_pendingMoveTargetSlot;

    g_pendingMoveSourceContainer = 0;
    g_pendingMoveHolder = 0;
    g_pendingMoveItem = 0;
    g_pendingMoveItemId = 0;
    g_pendingMoveCount = 0;
    g_pendingMoveTargetContainer = 0;
    g_pendingMoveTargetContainerId = 0;
    g_pendingMoveTargetSlot = 0;

    if (!dispatched) {
        std::ostringstream o;
        o << "{\"ok\":false,\"cmd\":\"MOVE_CONTAINER_ITEM\","
          << "\"error\":\"dispatcher_timeout\",\"win32\":" << win32 << "}";
        return o.str();
    }

    std::ostringstream o;
    o << "{\"ok\":" << (code == 1 ? "true" : "false")
      << ",\"cmd\":\"MOVE_CONTAINER_ITEM\""
      << ",\"result\":\"" << actionCodeText(code) << "\""
      << ",\"sourceContainerId\":" << sourceContainerId
      << ",\"targetContainerId\":" << targetContainerId
      << ",\"itemId\":" << itemId
      << ",\"count\":" << movedCount
      << ",\"destinationSlot\":" << static_cast<unsigned>(destinationSlot)
      << ",\"holderAbi\":\"shared_object_ptr_ref\""
      << ",\"destinationEncoding\":\"otclient_container_position_runtime_confirmed\"}";
    return o.str();
}

std::string topMoveAbiResponse() {
    if(!g_profile || !g_buildMatched)
        return R"({"ok":false,"cmd":"GET_TOP_MOVE_ABI","error":"build_not_validated"})";
    if(!g_profile->getTopMoveThingMethodRva)
        return R"({"ok":false,"cmd":"GET_TOP_MOVE_ABI","error":"getTopMoveThing_rva_missing"})";

    const auto target=g_base+g_profile->getTopMoveThingMethodRva;
    if(!isExecutableAddress(target))
        return R"({"ok":false,"cmd":"GET_TOP_MOVE_ABI","error":"method_not_executable"})";

    constexpr std::size_t kProbeBytes=0x300;
    std::vector<std::uint8_t> code(kProbeBytes);
    if(!safeCopyBytes(target,code.data(),code.size()))
        return R"({"ok":false,"cmd":"GET_TOP_MOVE_ABI","error":"method_bytes_unreadable"})";

    struct RetSite {
        std::size_t offset{};
        unsigned cleanup{};
        std::string window;
    };
    std::vector<RetSite> returns;

    for(std::size_t i=0;i<code.size();++i) {
        unsigned cleanup{};
        std::size_t insnSize{};
        if(code[i]==0xC3) {
            cleanup=0;
            insnSize=1;
        } else if(code[i]==0xC2 && i+2<code.size()) {
            cleanup=
                static_cast<unsigned>(code[i+1]) |
                (static_cast<unsigned>(code[i+2])<<8);
            insnSize=3;
        } else {
            continue;
        }

        const std::size_t before=std::min<std::size_t>(32,i);
        const std::size_t after=std::min<std::size_t>(
            12,code.size()-(i+insnSize));
        const auto wstart=i-before;
        const auto wsize=before+insnSize+after;
        returns.push_back({
            i,
            cleanup,
            hexBytes(code.data()+wstart,wsize)
        });
    }

    auto containsPattern=[&](std::initializer_list<std::uint8_t> bytes) {
        if(bytes.size()==0 || bytes.size()>code.size()) return false;
        for(std::size_t i=0;i+bytes.size()<=code.size();++i) {
            std::size_t j=0;
            bool same=true;
            for(const auto b:bytes) {
                if(code[i+j]!=b) { same=false; break; }
                ++j;
            }
            if(same) return true;
        }
        return false;
    };

    const bool copiesThisFromEcx=
        containsPattern({0x8B,0xD1}) ||
        containsPattern({0x8B,0xF1}) ||
        containsPattern({0x8B,0xF9});
    const bool readsHiddenOutAtEbp8=
        containsPattern({0x8B,0x45,0x08}) ||
        containsPattern({0x8B,0x4D,0x08}) ||
        containsPattern({0x8B,0x55,0x08});
    const bool readsThingsBegin=
        containsPattern({0x8B,0x5A,0x24}) ||
        containsPattern({0x8B,0x72,0x24}) ||
        containsPattern({0x8B,0x7A,0x24});
    const bool readsThingsEnd=
        containsPattern({0x8B,0x4A,0x28}) ||
        containsPattern({0x8B,0x72,0x28}) ||
        containsPattern({0x8B,0x7A,0x28});

    std::ostringstream o;
    o << "{\"ok\":true,\"cmd\":\"GET_TOP_MOVE_ABI\""
      << ",\"readOnly\":true"
      << ",\"methodRva\":\"0x" << std::hex << std::uppercase
      << g_profile->getTopMoveThingMethodRva << std::dec << "\""
      << ",\"probeBytes\":" << kProbeBytes
      << ",\"evidence\":{"
      << "\"thisFromEcx\":" << (copiesThisFromEcx?"true":"false")
      << ",\"hiddenReturnStorageAtEbp8\":" << (readsHiddenOutAtEbp8?"true":"false")
      << ",\"readsTileThingsBegin24\":" << (readsThingsBegin?"true":"false")
      << ",\"readsTileThingsEnd28\":" << (readsThingsEnd?"true":"false")
      << "}"
      << ",\"returns\":[";

    for(std::size_t i=0;i<returns.size();++i) {
        if(i) o << ",";
        o << "{\"offset\":\"0x" << std::hex << std::uppercase
          << returns[i].offset << std::dec
          << "\",\"cleanup\":" << returns[i].cleanup
          << ",\"window\":\"" << returns[i].window << "\"}";
    }
    o << "]";

    // Include two bounded code windows: entry and the common return area
    // observed from the V38 forward jump to +0x1DD. No code is executed.
    const std::size_t entrySize=std::min<std::size_t>(0xB0,code.size());
    o << ",\"entryWindow\":\"" << hexBytes(code.data(),entrySize) << "\"";

    constexpr std::size_t commonReturn=0x1DD;
    if(commonReturn<code.size()) {
        const std::size_t before=std::min<std::size_t>(48,commonReturn);
        const std::size_t after=std::min<std::size_t>(
            128,code.size()-commonReturn);
        o << ",\"returnAreaOffset\":\"0x1DD\""
          << ",\"returnAreaWindow\":\""
          << hexBytes(
                 code.data()+commonReturn-before,
                 before+after)
          << "\"";
    }

    o << "}";
    return o.str();
}


std::string topMoveAtResponse(
    std::uint32_t x,
    std::uint32_t y,
    std::uint8_t z) {

    if (!g_profile || !g_buildMatched)
        return R"({"ok":false,"cmd":"GET_TOP_MOVE_AT","error":"build_not_validated"})";
    if (!g_profile->getTopMoveThingMethodRva)
        return R"({"ok":false,"cmd":"GET_TOP_MOVE_AT","error":"getTopMoveThing_rva_missing"})";

    const auto tile=cachedTileAtPosition(x,y,z,true);
    if (!tile)
        return R"({"ok":false,"cmd":"GET_TOP_MOVE_AT","error":"tile_not_found"})";
    if (!exactObjectVtable(*tile,g_profile->tileVtableRva))
        return R"({"ok":false,"cmd":"GET_TOP_MOVE_AT","error":"tile_vtable_mismatch"})";
    if (!prepareGameThreadDispatcher())
        return R"({"ok":false,"cmd":"GET_TOP_MOVE_AT","error":"dispatcher_not_ready"})";

    g_pendingTopMoveTile=*tile;
    g_pendingTopMoveX=x;
    g_pendingTopMoveY=y;
    g_pendingTopMoveZ=z;
    InterlockedExchange(&g_pendingActionKind,PendingTopMoveRead);

    DWORD win32{};
    const bool dispatched=dispatchPendingActionToGameThread(win32);
    const LONG code=InterlockedCompareExchange(&g_lastActionCode,0,0);

    std::vector<std::uintptr_t> things;
    bool containsLocalPlayer=false;
    bool matchedTileEntry=false;
    std::size_t stackIndex=0;
    if (dispatched && code==1 && g_topMoveThing &&
        readStableTileThings(*tile,things,containsLocalPlayer)) {
        for (std::size_t i=0;i<things.size();++i) {
            if (things[i]==g_topMoveThing) {
                matchedTileEntry=true;
                stackIndex=i;
                break;
            }
        }
    }

    g_pendingTopMoveTile=0;

    if (!dispatched) {
        std::ostringstream o;
        o << R"({"ok":false,"cmd":"GET_TOP_MOVE_AT","error":"dispatcher_timeout","win32":)"
          << win32 << "}";
        return o.str();
    }
    if (code!=1) {
        std::ostringstream o;
        o << R"({"ok":false,"cmd":"GET_TOP_MOVE_AT","error":"runtime_call_failed","actionCode":)"
          << code << "}";
        return o.str();
    }

    std::ostringstream o;
    o << R"({"ok":true,"cmd":"GET_TOP_MOVE_AT","diagnosticOnly":true,"movesItem":false,"x":)"
      << x << R"(,"y":)" << y
      << R"(,"z":)" << static_cast<unsigned>(z)
      << R"(,"tile":"0x)" << std::hex << std::uppercase << *tile << R"(")"
      << R"(,"thing":"0x)" << g_topMoveThing << R"(")"
      << R"(,"vtable":"0x)" << g_topMoveVtable << R"(")" << std::dec
      << R"(,"nullResult":)" << (g_topMoveThing?"false":"true")
      << R"(,"isItem":)" << (g_topMoveWasItem?"true":"false");

    if (g_topMoveWasItem) {
        o << R"(,"itemId":)" << g_topMoveItemId
          << R"(,"count":)" << g_topMoveItemCount;
    }

    o << R"(,"matchedTileEntry":)" << (matchedTileEntry?"true":"false");
    if (matchedTileEntry)
        o << R"(,"stackIndex":)" << stackIndex;
    o << R"(,"tileThingCount":)" << things.size()
      << R"(,"refCountAfterCall":)" << g_topMoveRefsAfterCall
      << R"(,"refReleased":)" << (g_topMoveRefReleased?"true":"false")
      << R"(,"refCountAfterRelease":)" << g_topMoveRefsAfterRelease
      << "}";

    return o.str();
}

std::string handsResponse() {
    if (!g_profile || !g_profile->inventoryContractReady ||
        !g_profile->itemContractReady)
        return R"({"ok":false,"cmd":"GET_HANDS","error":"inventory_contract_not_ready"})";

    InventorySlotSnapshot right,left;
    if (!readInventorySlot(5u,right) || !readInventorySlot(6u,left))
        return R"({"ok":false,"cmd":"GET_HANDS","error":"inventory_read_failed"})";

    std::ostringstream o;
    o << "{\"ok\":true,\"cmd\":\"GET_HANDS\""
      << ",\"right\":{\"slot\":5,\"occupied\":" << (right.occupied?"true":"false");
    if (right.occupied)
        o << ",\"id\":" << right.id << ",\"count\":" << right.count;
    o << "}"
      << ",\"left\":{\"slot\":6,\"occupied\":" << (left.occupied?"true":"false");
    if (left.occupied)
        o << ",\"id\":" << left.id << ",\"count\":" << left.count;
    o << "}}";
    return o.str();
}

std::string moveNearbyItemToHandResponse(std::uint16_t itemId) {
    if (!g_profile || !g_buildMatched)
        return R"({"ok":false,"cmd":"MOVE_NEARBY_ITEM_TO_HAND","moved":false,"error":"build_not_validated"})";
    if (!itemId)
        return R"({"ok":false,"cmd":"MOVE_NEARBY_ITEM_TO_HAND","moved":false,"error":"invalid_item_id"})";

    std::uint32_t px{},py{};
    std::uint8_t pz{};
    if (!localPlayerPosition(px,py,pz))
        return R"({"ok":false,"cmd":"MOVE_NEARBY_ITEM_TO_HAND","moved":false,"error":"player_unavailable"})";

    // Refresh discovery for every call. Distance selects the target tile;
    // only the runtime TopMove result can authorize picking up an Item.
    const auto items=scanCachedTileItems(true);
    std::optional<TileItemDiagnostic> target;
    std::uint32_t bestDistance=2u;
    for (const auto& item:items) {
        if (item.id!=itemId || item.z!=pz) continue;
        const auto distance=std::max(
            item.x>px ? item.x-px : px-item.x,
            item.y>py ? item.y-py : py-item.y);
        if (distance<=1u && (!target || distance<bestDistance)) {
            target=item;
            bestDistance=distance;
        }
    }
    if (!target)
        return R"({"ok":false,"cmd":"MOVE_NEARBY_ITEM_TO_HAND","moved":false,"error":"item_not_found_in_3x3"})";
    if (!prepareGameThreadDispatcher())
        return R"({"ok":false,"cmd":"MOVE_NEARBY_ITEM_TO_HAND","moved":false,"error":"dispatcher_not_ready"})";

    g_pickupTileRequest={target->tileAddress,target->x,target->y,target->z,itemId};
    g_pickupTileResult.clear();
    InterlockedExchange(&g_pendingActionKind,PendingPickupTopMove);
    DWORD win32{};
    const bool dispatched=dispatchPendingActionToGameThread(win32);
    g_pickupTileRequest={};
    if (!dispatched) {
        std::ostringstream response;
        response << R"({"ok":false,"cmd":"MOVE_NEARBY_ITEM_TO_HAND","moved":false,"error":"dispatcher_timeout","win32":)"
                 << win32 << "}";
        return response.str();
    }
    if (g_pickupTileResult.empty())
        return R"({"ok":false,"cmd":"MOVE_NEARBY_ITEM_TO_HAND","moved":false,"error":"game_thread_result_missing"})";
    return g_pickupTileResult;
}

std::string uncoverTileTargetResponse(
    const char* command,
    const TileItemDiagnostic& target) {

    if(!g_profile || !g_buildMatched)
        return std::string("{\"ok\":false,\"cmd\":\"") + command +
               "\",\"error\":\"build_not_validated\"}";

    // V39 safety gate. Do not move any candidate until the real
    // Tile::getTopMoveThing ABI/return convention is established.
    return std::string("{\"ok\":false,\"cmd\":\"") + command +
           "\",\"error\":\"top_move_abi_validation_required_v39\"}";

    /*
    if(!g_profile->moveRuntimeConfirmed ||
       !g_profile->moveItemSharedRefConfirmed ||
       !g_profile->movePositionRefConfirmed ||
       !g_profile->moveCountConfirmed ||
       !g_profile->itemContractReady ||
       !g_profile->tileThingsRuntimeConfirmed)
        return std::string("{\"ok\":false,\"cmd\":\"") + command +
               "\",\"error\":\"tile_ground_move_contract_not_ready\"}";

    const auto access=inspectTileTargetAccess(target);
    if(!access.valid)
        return std::string("{\"ok\":false,\"cmd\":\"") + command +
               "\",\"error\":\"target_stack_changed\"}";

    if(!access.blocker) {
        std::ostringstream o;
        o << "{\"ok\":true,\"cmd\":\"" << command << "\""
          << ",\"moved\":false"
          << ",\"exposed\":true"
          << ",\"target\":{\"id\":" << access.target.id
          << ",\"x\":" << access.target.x
          << ",\"y\":" << access.target.y
          << ",\"z\":" << static_cast<unsigned>(access.target.z)
          << ",\"stackIndex\":" << access.target.stackIndex
          << "}}";
        return o.str();
    }

    const auto destination=chooseAdjacentGroundDestination(
        access.target.x,access.target.y,access.target.z);
    if(!destination)
        return std::string("{\"ok\":false,\"cmd\":\"") + command +
               "\",\"error\":\"no_adjacent_ground_destination\"}";

    if(!prepareGameThreadDispatcher())
        return std::string("{\"ok\":false,\"cmd\":\"") + command +
               "\",\"error\":\"dispatcher_not_ready\"}";

    const auto blocker=*access.blocker;
    g_pendingMoveSourceTile=blocker.tileAddress;
    g_pendingMoveTileExpectedItem=blocker.address;
    g_pendingMoveTileItemId=blocker.id;
    g_pendingMoveInventorySlot=0;
    g_pendingMoveGroundX=destination->x;
    g_pendingMoveGroundY=destination->y;
    g_pendingMoveGroundZ=destination->z;
    g_pendingMoveGroundCount=blocker.count ? blocker.count : 1u;
    InterlockedExchange(&g_pendingActionKind,PendingMoveTileToGround);

    DWORD win32{};
    const bool dispatched=dispatchPendingActionToGameThread(win32);
    const LONG code=InterlockedCompareExchange(&g_lastActionCode,0,0);

    const auto moveCount=g_pendingMoveGroundCount;
    g_pendingMoveSourceTile=0;
    g_pendingMoveTileExpectedItem=0;
    g_pendingMoveTileItemId=0;
    g_pendingMoveInventorySlot=0;
    g_pendingMoveGroundX=0;
    g_pendingMoveGroundY=0;
    g_pendingMoveGroundZ=0;
    g_pendingMoveGroundCount=0;

    if(!dispatched) {
        std::ostringstream o;
        o << "{\"ok\":false,\"cmd\":\"" << command << "\","
          << "\"error\":\"dispatcher_timeout\",\"win32\":" << win32 << "}";
        return o.str();
    }

    std::ostringstream o;
    o << "{\"ok\":" << (code==1?"true":"false")
      << ",\"cmd\":\"" << command << "\""
      << ",\"result\":\"" << actionCodeText(code) << "\""
      << ",\"moved\":" << (code==1?"true":"false")
      << ",\"exposed\":false"
      << ",\"target\":{\"id\":" << access.target.id
      << ",\"x\":" << access.target.x
      << ",\"y\":" << access.target.y
      << ",\"z\":" << static_cast<unsigned>(access.target.z)
      << ",\"stackIndex\":" << access.target.stackIndex << "}"
      << ",\"blocker\":{\"id\":" << blocker.id
      << ",\"count\":" << moveCount
      << ",\"stackIndex\":" << blocker.stackIndex << "}"
      << ",\"destination\":{\"x\":" << destination->x
      << ",\"y\":" << destination->y
      << ",\"z\":" << static_cast<unsigned>(destination->z) << "}"
      << ",\"destinationEncoding\":\"world_position_experimental\""
      << "}";
    return o.str();
    */
}

std::string uncoverNearbyItemResponse(std::uint16_t itemId) {
    const auto target=findNearbyTileItemById(itemId,1u);
    if(!target)
        return R"({"ok":false,"cmd":"UNCOVER_NEARBY_ITEM","error":"item_not_found_in_3x3"})";
    return uncoverTileTargetResponse("UNCOVER_NEARBY_ITEM",*target);
}

std::string uncoverTileTargetAtResponse(
    std::uint32_t x,
    std::uint32_t y,
    std::uint8_t z,
    std::uint16_t itemId) {

    if (!g_profile || !g_buildMatched)
        return R"({"ok":false,"cmd":"UNCOVER_TILE_TARGET_AT","error":"build_not_validated","moved":false,"exposed":false})";
    if (!itemId)
        return R"({"ok":false,"cmd":"UNCOVER_TILE_TARGET_AT","error":"target_item_id_required","moved":false,"exposed":false})";

    const auto tile=cachedTileAtPosition(x,y,z,true);
    if (!tile)
        return R"({"ok":false,"cmd":"UNCOVER_TILE_TARGET_AT","error":"tile_not_found","moved":false,"exposed":false})";
    if (!prepareGameThreadDispatcher())
        return R"({"ok":false,"cmd":"UNCOVER_TILE_TARGET_AT","error":"dispatcher_not_ready","moved":false,"exposed":false})";

    g_uncoverTileRequest={*tile,x,y,z,itemId};
    g_uncoverTileResult.clear();
    InterlockedExchange(&g_pendingActionKind,PendingUncoverTile);
    DWORD win32{};
    const bool dispatched=dispatchPendingActionToGameThread(win32);
    g_uncoverTileRequest={};
    if (!dispatched) {
        std::ostringstream response;
        response << R"({"ok":false,"cmd":"UNCOVER_TILE_TARGET_AT","error":"dispatcher_timeout","moved":false,"exposed":false,"win32":)"
                 << win32 << "}";
        return response.str();
    }
    if (g_uncoverTileResult.empty())
        return R"({"ok":false,"cmd":"UNCOVER_TILE_TARGET_AT","error":"game_thread_result_missing","moved":false,"exposed":false})";
    return g_uncoverTileResult;
}

std::string useTileItemAtResponse(
    std::uint32_t x,
    std::uint32_t y,
    std::uint8_t z,
    std::uint16_t itemId) {

    if (!g_profile || !g_buildMatched)
        return R"({"ok":false,"cmd":"USE_TILE_ITEM_AT","error":"build_not_validated"})";
    if (!g_profile->useRuntimeConfirmed ||
        !g_profile->useItemSharedRefConfirmed ||
        !g_profile->itemContractReady ||
        !g_profile->tileThingsRuntimeConfirmed)
        return R"({"ok":false,"cmd":"USE_TILE_ITEM_AT","error":"use_tile_contract_not_ready"})";

    // Cavebot may have just walked into range. Force a fresh Tile::things
    // discovery here instead of trusting a cache that can legitimately be
    // several seconds old and miss the newly loaded target tile.
    const auto target=findTileItemAt(x,y,z,itemId,true);
    if (!target) {
        std::ostringstream o;
        o << "{\"ok\":false,\"cmd\":\"USE_TILE_ITEM_AT\","
          << "\"error\":\"tile_item_not_found\","
          << "\"requestedItemId\":" << itemId
          << ",\"x\":" << x << ",\"y\":" << y
          << ",\"z\":" << static_cast<unsigned>(z) << "}";
        return o.str();
    }

    std::uintptr_t holder{},item{};
    if (!resolveTileItemHolder(
            target->tileAddress,
            target->id,
            target->address,
            holder,
            item) ||
        !holder || !item)
        return R"({"ok":false,"cmd":"USE_TILE_ITEM_AT","error":"tile_item_holder_not_found"})";

    if (!prepareGameThreadDispatcher())
        return R"({"ok":false,"cmd":"USE_TILE_ITEM_AT","error":"dispatcher_not_ready"})";

    g_pendingUseTile=target->tileAddress;
    g_pendingUseContainer=0;
    g_pendingUseContainerId=0;
    g_pendingUseHolder=holder;
    g_pendingUseItem=item;
    g_pendingUseItemId=target->id;
    InterlockedExchange(&g_pendingActionKind,PendingUse);

    DWORD win32{};
    const bool dispatched=dispatchPendingActionToGameThread(win32);
    const LONG code=InterlockedCompareExchange(&g_lastActionCode,0,0);

    const auto usedId=target->id;
    g_pendingUseTile=0;
    g_pendingUseHolder=0;
    g_pendingUseItem=0;
    g_pendingUseItemId=0;

    if (!dispatched) {
        std::ostringstream o;
        o << "{\"ok\":false,\"cmd\":\"USE_TILE_ITEM_AT\","
          << "\"error\":\"dispatcher_timeout\",\"win32\":" << win32 << "}";
        return o.str();
    }

    std::ostringstream o;
    o << "{\"ok\":" << (code==1?"true":"false")
      << ",\"cmd\":\"USE_TILE_ITEM_AT\""
      << ",\"result\":\"" << actionCodeText(code) << "\""
      << ",\"itemId\":" << usedId
      << ",\"x\":" << x
      << ",\"y\":" << y
      << ",\"z\":" << static_cast<unsigned>(z)
      << "}";
    return o.str();
}

std::string useContainerItemWithTileResponse(
    std::uint16_t toolItemId,
    std::uint32_t x,
    std::uint32_t y,
    std::uint8_t z,
    std::uint16_t targetItemId,
    bool requireExposed=false) {

    if (!g_profile || !g_buildMatched)
        return R"({"ok":false,"cmd":"USE_WITH_TILE_AT","error":"build_not_validated"})";
    if (!g_profile->useWithRuntimeConfirmed ||
        !g_profile->useWithItemSharedRefConfirmed ||
        !g_profile->useWithThingSharedRefConfirmed ||
        !g_profile->containerContractReady ||
        !g_profile->itemContractReady ||
        !g_profile->tileThingsRuntimeConfirmed)
        return R"({"ok":false,"cmd":"USE_WITH_TILE_AT","error":"usewith_tile_contract_not_ready"})";

    if (requireExposed && !targetItemId)
        return R"({"ok":false,"cmd":"USE_WITH_TILE_AT","error":"target_item_id_required"})";

    // Tool actions have the same just-arrived problem as direct Use.
    // Always refresh once before resolving the ground/tile target.
    const auto target=findTileItemAt(x,y,z,targetItemId,true);
    if (!target) {
        std::ostringstream o;
        o << "{\"ok\":false,\"cmd\":\"USE_WITH_TILE_AT\","
          << "\"error\":\"tile_target_not_found\","
          << "\"requestedTargetItemId\":" << targetItemId
          << ",\"x\":" << x << ",\"y\":" << y
          << ",\"z\":" << static_cast<unsigned>(z) << "}";
        return o.str();
    }

    const auto containers=scanOpenContainers();
    const ContainerSnapshot* source=nullptr;
    const ContainerItemSlot* sourceSlot=nullptr;

    for (const auto& container:containers) {
        for (const auto& slot:container.items) {
            if (slot.id==toolItemId) {
                source=&container;
                sourceSlot=&slot;
                break;
            }
        }
        if (sourceSlot) break;
    }

    if (!source || !sourceSlot)
        return R"({"ok":false,"cmd":"USE_WITH_TILE_AT","error":"tool_not_found_in_open_containers"})";

    std::uintptr_t targetHolder{},targetItem{};
    if (!resolveTileItemHolder(
            target->tileAddress,
            target->id,
            target->address,
            targetHolder,
            targetItem) ||
        !targetHolder || !targetItem)
        return R"({"ok":false,"cmd":"USE_WITH_TILE_AT","error":"tile_target_holder_not_found"})";

    if (!prepareGameThreadDispatcher())
        return R"({"ok":false,"cmd":"USE_WITH_TILE_AT","error":"dispatcher_not_ready"})";

    g_pendingUseTile=0;
    g_pendingUseContainer=source->address;
    g_pendingUseContainerId=source->id;
    g_pendingUseHolder=sourceSlot->holderAddress;
    g_pendingUseItem=sourceSlot->itemAddress;
    g_pendingUseItemId=sourceSlot->id;

    g_pendingUseWithTargetTile=target->tileAddress;
    g_pendingUseWithTargetHolder=targetHolder;
    g_pendingUseWithTargetItem=targetItem;
    g_pendingUseWithTargetItemId=target->id;

    g_pendingExposedUseX=x;
    g_pendingExposedUseY=y;
    g_pendingExposedUseZ=z;
    InterlockedExchange(&g_pendingActionKind,
        requireExposed ? PendingUseWithExposedTile : PendingUseWithTile);

    DWORD win32{};
    const bool dispatched=dispatchPendingActionToGameThread(win32);
    const LONG code=InterlockedCompareExchange(&g_lastActionCode,0,0);

    const auto sourceContainerId=source->id;
    const auto actualTargetId=target->id;

    g_pendingUseContainer=0;
    g_pendingUseContainerId=0;
    g_pendingUseHolder=0;
    g_pendingUseItem=0;
    g_pendingUseItemId=0;
    g_pendingUseWithTargetTile=0;
    g_pendingUseWithTargetHolder=0;
    g_pendingUseWithTargetItem=0;
    g_pendingUseWithTargetItemId=0;
    g_pendingExposedUseX=0;
    g_pendingExposedUseY=0;
    g_pendingExposedUseZ=0;

    if (!dispatched) {
        std::ostringstream o;
        o << "{\"ok\":false,\"cmd\":\"USE_WITH_TILE_AT\","
          << "\"error\":\"dispatcher_timeout\",\"win32\":" << win32 << "}";
        return o.str();
    }

    std::ostringstream o;
    o << "{\"ok\":" << (code==1?"true":"false")
      << ",\"cmd\":\"USE_WITH_TILE_AT\""
      << ",\"exposureRequired\":" << (requireExposed ? "true" : "false")
      << ",\"result\":\"" << actionCodeText(code) << "\""
      << ",\"toolItemId\":" << toolItemId
      << ",\"sourceContainerId\":" << sourceContainerId
      << ",\"targetItemId\":" << actualTargetId
      << ",\"x\":" << x
      << ",\"y\":" << y
      << ",\"z\":" << static_cast<unsigned>(z)
      << "}";
    return o.str();
}

std::string autoWalkToResponse(
    std::uint32_t x,
    std::uint32_t y,
    std::uint8_t z) {

    if (!g_profile || !g_buildMatched)
        return R"({"ok":false,"cmd":"AUTO_WALK_TO","error":"build_not_validated"})";
    if (!g_profile->autoWalkRuntimeConfirmed ||
        !g_profile->autoWalkDirectionsVectorRefConfirmed ||
        !g_profile->autoWalkStartPositionValueConfirmed ||
        g_profile->autoWalkStackCleanupBytes!=16 ||
        !g_profile->autoWalkMethodRva)
        return R"({"ok":false,"cmd":"AUTO_WALK_TO","error":"autowalk_contract_not_ready_v40"})";
    if (!kAutoWalkActiveCallV40Implemented)
        return R"({"ok":false,"cmd":"AUTO_WALK_TO","error":"autowalk_active_call_locked_pending_v40"})";
    if (!prepareGameThreadDispatcher())
        return R"({"ok":false,"cmd":"AUTO_WALK_TO","error":"dispatcher_not_ready"})";

    g_pendingWalkX=x;
    g_pendingWalkY=y;
    g_pendingWalkZ=z;
    InterlockedExchange(&g_pendingActionKind,PendingAutoWalk);

    DWORD win32{};
    const bool dispatched=dispatchPendingActionToGameThread(win32);
    const LONG code=InterlockedCompareExchange(&g_lastActionCode,0,0);

    g_pendingWalkX=0;
    g_pendingWalkY=0;
    g_pendingWalkZ=0;

    if (!dispatched) {
        std::ostringstream o;
        o << "{\"ok\":false,\"cmd\":\"AUTO_WALK_TO\","
          << "\"error\":\"dispatcher_timeout\",\"win32\":" << win32 << "}";
        return o.str();
    }

    const bool ok=code==1 || code==33;

    std::ostringstream o;
    o << "{\"ok\":" << (ok?"true":"false")
      << ",\"cmd\":\"AUTO_WALK_TO\""
      << ",\"result\":\"" << actionCodeText(code) << "\""
      << ",\"x\":" << x
      << ",\"y\":" << y
      << ",\"z\":" << static_cast<unsigned>(z)
      << "}";
    return o.str();
}

std::string attackIdResponse(std::uint32_t creatureId) {
    logLine("[TARGET TRACE] ATTACK_ID request id=" + std::to_string(creatureId));
    if (!g_profile || !g_buildMatched)
        return R"({"ok":false,"cmd":"ATTACK_ID","error":"build_not_validated"})";
    if (!prepareGameThreadDispatcher())
        return R"({"ok":false,"cmd":"ATTACK_ID","error":"dispatcher_not_ready"})";

    const auto tileVisible = scanCachedTileMonsters(15);
    const bool idVisible = std::any_of(
        tileVisible.begin(), tileVisible.end(),
        [creatureId](const auto& d){ return d.monster.id == creatureId; });
    if (!idVisible)
        return R"({"ok":false,"cmd":"ATTACK_ID","error":"creature_id_not_visible"})";

    g_lastActionTargetId = 0;
    g_pendingAttackTarget = 0;
    g_pendingAttackRequestedId = creatureId;

    InterlockedExchange(&g_pendingActionKind, PendingAttack);
    DWORD win32{};
    const bool dispatched = dispatchPendingActionToGameThread(win32);

    g_pendingAttackTarget = 0;
    g_pendingAttackRequestedId = 0;

    if (!dispatched) {
        InterlockedExchange(&g_lastActionCode, 8);
        std::ostringstream o;
        o << "{\"ok\":false,\"cmd\":\"ATTACK_ID\",\"error\":\"dispatcher_timeout\","
          << "\"win32\":" << win32 << "}";
        return o.str();
    }

    const LONG code = InterlockedCompareExchange(&g_lastActionCode, 0, 0);
    std::ostringstream o;
    o << "{\"ok\":" << (code == 1 ? "true" : "false")
      << ",\"cmd\":\"ATTACK_ID\",\"result\":\"" << actionCodeText(code)
      << "\",\"requestedId\":" << creatureId
      << ",\"targetId\":" << g_lastActionTargetId
      << ",\"gameThreadResult\":" << (code == 1 ? 1 : 0) << "}";
    return o.str();
}
std::string attackSelectedResponse() {
    if (!g_profile || !g_buildMatched)
        return R"({"ok":false,"cmd":"ATTACK_SELECTED","error":"build_not_validated"})";
    if (!prepareGameThreadDispatcher())
        return R"({"ok":false,"cmd":"ATTACK_SELECTED","error":"dispatcher_not_ready"})";

    g_lastActionTargetId = 0;
    g_pendingAttackTarget = 0;
    g_pendingAttackRequestedId = 0;

    InterlockedExchange(&g_pendingActionKind, PendingAttack);
    DWORD win32{};
    const bool dispatched = dispatchPendingActionToGameThread(win32);
    if (!dispatched) {
        InterlockedExchange(&g_lastActionCode, 8);
        std::ostringstream o;
        o << "{\"ok\":false,\"cmd\":\"ATTACK_SELECTED\","
          << "\"error\":\"dispatcher_timeout\",\"win32\":" << win32 << "}";
        return o.str();
    }

    const LONG code = InterlockedCompareExchange(&g_lastActionCode, 0, 0);
    std::ostringstream o;
    o << "{\"ok\":" << (code == 1 ? "true" : "false")
      << ",\"cmd\":\"ATTACK_SELECTED\",\"result\":\"" << actionCodeText(code)
      << "\",\"targetId\":" << g_lastActionTargetId
      << ",\"gameThreadResult\":" << (code == 1 ? 1 : 0) << "}";
    return o.str();
}
std::string sayResponse(const std::string& message) {
    if (!g_profile || !g_buildMatched)
        return R"({"ok":false,"cmd":"SAY","error":"build_not_validated"})";
    if (message.empty() || message.size()>15)
        return R"({"ok":false,"cmd":"SAY","error":"message_must_be_1_to_15_ascii_chars"})";

    for (const unsigned char c:message) {
        if (c<0x20 || c>0x7E)
            return R"({"ok":false,"cmd":"SAY","error":"message_ascii_only"})";
    }

    if (!prepareGameThreadDispatcher())
        return R"({"ok":false,"cmd":"SAY","error":"dispatcher_not_ready"})";

    std::memset(g_pendingTalkMessage,0,sizeof(g_pendingTalkMessage));
    std::memcpy(g_pendingTalkMessage,message.data(),message.size());

    // OTC-family Game::talk is talkChannel(MessageSay,0,message).
    // MessageSay=1 is cross-checked against upstream OTClient/OTCv8 source;
    // the Ramonia talkChannel ABI itself is runtime-confirmed in this profile.
    g_pendingTalkMode=g_profile->talkSayModeValue;
    g_pendingTalkChannel=g_profile->talkDefaultChannel;
    InterlockedExchange(&g_pendingActionKind,PendingTalk);

    DWORD win32{};
    const bool dispatched=dispatchPendingActionToGameThread(win32);
    const LONG code=InterlockedCompareExchange(&g_lastActionCode,0,0);

    std::memset(g_pendingTalkMessage,0,sizeof(g_pendingTalkMessage));

    if (!dispatched) {
        std::ostringstream o;
        o << "{\"ok\":false,\"cmd\":\"SAY\","
          << "\"error\":\"dispatcher_timeout\",\"win32\":" << win32 << "}";
        return o.str();
    }

    std::ostringstream o;
    o << "{\"ok\":" << (code==1?"true":"false")
      << ",\"cmd\":\"SAY\",\"result\":\"" << actionCodeText(code)
      << "\",\"mode\":" << g_profile->talkSayModeValue
      << ",\"channel\":" << g_profile->talkDefaultChannel
      << ",\"message\":\"" << jsonEscape(message) << "\"}";
    return o.str();
}

std::string handleRequest(const std::string& request) {
    const auto cmd = commandFromRequest(request);
    if (cmd == "HELLO") {
        std::ostringstream o;
        o << "{\"ok\":true,\"cmd\":\"HELLO\",\"bridge\":\"CromeRamoniaBridge\","
          << "\"bridgeVersion\":39,\"protocol\":1,\"pid\":" << GetCurrentProcessId()
          << ",\"pipe\":\"" << jsonEscape(pipeName()) << "\","
          << "\"status\":\"" << jsonEscape(g_status) << "\","
          << "\"profileReady\":" << (g_profile.has_value() ? "true" : "false")
          << ",\"buildMatched\":" << (g_buildMatched ? "true" : "false")
          << ",\"toolTileUncoverReady\":"
          << ((g_buildMatched && g_profile &&
               g_profile->getTopMoveThingMethodRva==0x197AD0u &&
               g_profile->itemContractReady &&
               g_profile->tileThingsRuntimeConfirmed &&
               g_profile->moveRuntimeConfirmed &&
               g_profile->moveItemSharedRefConfirmed &&
               g_profile->movePositionRefConfirmed &&
               g_profile->moveCountConfirmed &&
               g_profile->moveMethodRva &&
               g_profile->containerContractReady &&
               g_profile->useWithRuntimeConfirmed &&
               g_profile->useWithItemSharedRefConfirmed &&
               g_profile->useWithThingSharedRefConfirmed &&
               g_profile->useWithMethodRva) ? "true" : "false")
          << ",\"spearPickupReady\":"
          << ((g_buildMatched && g_profile &&
               g_profile->getTopMoveThingMethodRva==0x197AD0u &&
               g_profile->itemContractReady &&
               g_profile->tileThingsRuntimeConfirmed &&
               g_profile->inventoryContractReady &&
               g_profile->moveMethodRva &&
               g_profile->moveRuntimeConfirmed &&
               g_profile->moveItemSharedRefConfirmed &&
               g_profile->movePositionRefConfirmed &&
               g_profile->moveCountConfirmed) ? "true" : "false")
          << ",\"lookReady\":"
          << ((g_profile && g_profile->lookRuntimeConfirmed &&
               g_profile->lookItemSharedRefConfirmed) ? "true" : "false")
          << ",\"walkReady\":"
          << ((kAutoWalkActiveCallV40Implemented &&
               g_profile && g_profile->autoWalkRuntimeConfirmed &&
               g_profile->autoWalkDirectionsVectorRefConfirmed &&
               g_profile->autoWalkStartPositionValueConfirmed &&
               g_profile->autoWalkStackCleanupBytes==16) ? "true" : "false")
          << ",\"lightReady\":"
          << ((g_profile && g_profile->worldLightContractReady) ? "true" : "false")
          << ",\"lightEnabled\":" << (g_worldLightEnabled ? "true" : "false")
          << ",\"dispatcherReady\":" << (dispatcherReady() ? "true" : "false")
          << ",\"dispatcherMode\":\"on_demand\""
          << ",\"dispatcherHookActive\":" << (g_gameThreadHook ? "true" : "false");
        if (!g_profileError.empty())
            o << ",\"profileError\":\"" << jsonEscape(g_profileError) << "\"";
        o << "}";
        return o.str();
    }

    if (!g_profile) {
        return "{\"ok\":false,\"cmd\":\"" + jsonEscape(cmd) +
               "\",\"error\":\"profile_not_ready\",\"detail\":\"" +
               jsonEscape(g_profileError) + "\"}";
    }

    const auto& p = *g_profile;
    if (cmd == "BUILD_INFO") {
        std::ostringstream o;
        o << "{\"ok\":true,\"cmd\":\"BUILD_INFO\",\"schema\":\"" << jsonEscape(p.schema)
          << "\",\"profileVersion\":" << p.profileVersion
          << ",\"architecture\":\"" << jsonEscape(p.architecture)
          << "\",\"timestamp\":\"" << jsonEscape(p.timestamp)
          << "\",\"sizeOfImage\":\"" << jsonEscape(p.sizeOfImage)
          << "\",\"textHash\":\"" << jsonEscape(p.textHash) << "\","
          << "\"runtimeTimestamp\":\"0x" << std::hex << std::uppercase << g_imageTimestamp
          << "\",\"runtimeSizeOfImage\":\"0x" << g_imageSize << std::dec
          << "\",\"matched\":" << (g_buildMatched ? "true" : "false") << "}";
        return o.str();
    }
    if (cmd == "GET_PLAYER") return playerResponse(p, g_base);
    if (cmd == "USE_TILE_ITEM_AT") {
        const auto x=uint32FieldFromRequest(request,"x");
        const auto y=uint32FieldFromRequest(request,"y");
        const auto z=uint32FieldAllowZeroFromRequest(request,"z");
        const auto itemId=uint32FieldAllowZeroFromRequest(request,"itemId");
        if (!x || !y || !z || !itemId || *z>0xFFu || *itemId>0xFFFFu)
            return R"({"ok":false,"cmd":"USE_TILE_ITEM_AT","error":"invalid_arguments"})";
        return useTileItemAtResponse(
            *x,*y,static_cast<std::uint8_t>(*z),
            static_cast<std::uint16_t>(*itemId));
    }

    if (cmd == "USE_WITH_TILE_AT") {
        const auto toolItemId=uint32FieldFromRequest(request,"toolItemId");
        const auto x=uint32FieldFromRequest(request,"x");
        const auto y=uint32FieldFromRequest(request,"y");
        const auto z=uint32FieldAllowZeroFromRequest(request,"z");
        const auto targetItemId=
            uint32FieldAllowZeroFromRequest(request,"targetItemId");

        const auto requireExposed=
            uint32FieldAllowZeroFromRequest(request,"requireExposed");

        if ((requireExposed && *requireExposed>1u) || !toolItemId || *toolItemId>0xFFFFu ||
            !x || !y || !z || *z>0xFFu ||
            (targetItemId && *targetItemId>0xFFFFu))
            return R"({"ok":false,"cmd":"USE_WITH_TILE_AT","error":"invalid_arguments"})";

        return useContainerItemWithTileResponse(
            static_cast<std::uint16_t>(*toolItemId),
            *x,*y,static_cast<std::uint8_t>(*z),
            static_cast<std::uint16_t>(targetItemId ? *targetItemId : 0u),
            requireExposed && *requireExposed==1u);
    }

    if (cmd == "AUTO_WALK_TO") {
        const auto x=uint32FieldFromRequest(request,"x");
        const auto y=uint32FieldFromRequest(request,"y");
        const auto z=uint32FieldAllowZeroFromRequest(request,"z");
        if (!x || !y || !z || *z>0xFFu)
            return R"({"ok":false,"cmd":"AUTO_WALK_TO","error":"invalid_position"})";
        return autoWalkToResponse(*x,*y,static_cast<std::uint8_t>(*z));
    }
    if (cmd == "ARM_LOOK_ID") return armLookCaptureResponse();
    if (cmd == "GET_LOOK_ID") return getLookCaptureResponse();
    if (cmd == "SAY") {
        const auto message=stringFieldFromRequest(request,"message");
        if (!message)
            return R"({"ok":false,"cmd":"SAY","error":"invalid_message"})";
        return sayResponse(*message);
    }
    if (cmd == "GET_LIGHT") return worldLightStatusResponse("GET_LIGHT");
    if (cmd == "SET_LIGHT") {
        const auto enabled=uint32FieldAllowZeroFromRequest(request,"enabled");
        if (!enabled || *enabled > 1u)
            return R"({"ok":false,"cmd":"SET_LIGHT","error":"invalid_enabled"})";
        return setWorldLightResponse(*enabled != 0u);
    }
    if (cmd == "GET_COMBAT_TARGETS") return combatTargetsResponse(p);
    if (cmd == "GET_VISIBLE_MONSTERS") return visibleMonstersResponse();
    if (cmd == "GET_HANDS") return handsResponse();
    if (cmd == "GET_TOP_MOVE_ABI") return topMoveAbiResponse();
    if (cmd == "GET_TOP_MOVE_AT") {
        const auto x=uint32FieldFromRequest(request,"x");
        const auto y=uint32FieldFromRequest(request,"y");
        const auto z=uint32FieldAllowZeroFromRequest(request,"z");
        if (!x || !y || !z || *z>0xFFu)
            return R"({"ok":false,"cmd":"GET_TOP_MOVE_AT","error":"invalid_position"})";
        return topMoveAtResponse(*x,*y,static_cast<std::uint8_t>(*z));
    }
    if (cmd == "UNCOVER_NEARBY_ITEM") {
        const auto itemId=uint32FieldFromRequest(request,"itemId");
        if(!itemId || !*itemId || *itemId>0xFFFFu)
            return R"({"ok":false,"cmd":"UNCOVER_NEARBY_ITEM","error":"invalid_item_id"})";
        return uncoverNearbyItemResponse(static_cast<std::uint16_t>(*itemId));
    }
    if (cmd == "UNCOVER_TILE_TARGET_AT") {
        const auto x=uint32FieldFromRequest(request,"x");
        const auto y=uint32FieldFromRequest(request,"y");
        const auto z=uint32FieldAllowZeroFromRequest(request,"z");
        const auto itemId=uint32FieldFromRequest(request,"itemId");
        if(!x || !y || !z || !itemId ||
           *z>0xFFu || *itemId>0xFFFFu)
            return R"({"ok":false,"cmd":"UNCOVER_TILE_TARGET_AT","error":"invalid_arguments"})";
        return uncoverTileTargetAtResponse(
            *x,*y,static_cast<std::uint8_t>(*z),
            static_cast<std::uint16_t>(*itemId));
    }
    if (cmd == "MOVE_NEARBY_ITEM_TO_HAND") {
        const auto itemId=uint32FieldFromRequest(request,"itemId");
        if (!itemId || !*itemId || *itemId>0xFFFFu)
            return R"({"ok":false,"cmd":"MOVE_NEARBY_ITEM_TO_HAND","error":"invalid_item_id"})";
        return moveNearbyItemToHandResponse(
            static_cast<std::uint16_t>(*itemId));
    }
    if (cmd == "GET_NEARBY_ITEM") {
        const auto itemId=uint32FieldFromRequest(request,"itemId");
        const auto requestedRadius=
            uint32FieldAllowZeroFromRequest(request,"radius");
        if (!itemId || *itemId>0xFFFFu)
            return R"({"ok":false,"cmd":"GET_NEARBY_ITEM","error":"invalid_item_id"})";
        const auto radius=std::min<std::uint32_t>(
            requestedRadius ? *requestedRadius : 1u, 7u);
        return nearbyItemResponse(
            static_cast<std::uint16_t>(*itemId),radius);
    }
    if (cmd == "GET_TILE_ITEMS_AT") {
        const auto x=uint32FieldFromRequest(request,"x");
        const auto y=uint32FieldFromRequest(request,"y");
        const auto z=uint32FieldAllowZeroFromRequest(request,"z");
        if (!x || !y || !z || *z>0xFFu)
            return R"({"ok":false,"cmd":"GET_TILE_ITEMS_AT","error":"invalid_position"})";
        return tileItemsAtResponse(*x,*y,static_cast<std::uint8_t>(*z));
    }
    if (cmd == "GET_TILE_MONSTERS") return tileMonstersResponse();
    if (cmd == "LOOT_MARK") return lootMarkResponse();
    if (cmd == "LOOT_MARK_TARGET") {
        const auto id = uint32FieldFromRequest(request, "creatureId");
        if (!id || !*id)
            return R"({"ok":false,"cmd":"LOOT_MARK_TARGET","error":"invalid_creature_id"})";
        return lootMarkTargetResponse(*id);
    }
    if (cmd == "LOOT_DIFF") return lootDiffResponse();
    if (cmd == "GET_OPEN_CONTAINERS") return openContainersResponse();
    if (cmd == "USE_LOOT_ITEM") {
        const auto id = uint32FieldFromRequest(request, "itemId");
        if (!id || *id > 0xFFFFu)
            return R"({"ok":false,"cmd":"USE_LOOT_ITEM","error":"invalid_item_id"})";
        return useLootItemResponse(static_cast<std::uint16_t>(*id));
    }
    if (cmd == "USE_CONTAINER_ITEM_ON_SELF") {
        const auto itemId=uint32FieldFromRequest(request,"itemId");
        if (!itemId || !*itemId || *itemId>0xFFFFu)
            return R"({"ok":false,"cmd":"USE_CONTAINER_ITEM_ON_SELF","error":"invalid_item_id"})";

        const auto sourceContainerId=
            uint32FieldAllowZeroFromRequest(request,"sourceContainerId");
        return useContainerItemOnSelfResponse(
            sourceContainerId,
            static_cast<std::uint16_t>(*itemId));
    }

    if (cmd == "USE_CONTAINER_ITEM") {
        const auto item = uint32FieldFromRequest(request, "itemId");
        if (!item || *item > 0xFFFFu)
            return R"({"ok":false,"cmd":"USE_CONTAINER_ITEM","error":"invalid_item_id"})";

        // sourceContainerId is optional. When omitted, search all currently
        // open containers and use the first matching item.
        const auto sourceId =
            uint32FieldAllowZeroFromRequest(request, "sourceContainerId");

        return useContainerItemResponse(
            sourceId, static_cast<std::uint16_t>(*item));
    }
    if (cmd == "MOVE_CONTAINER_ITEM") {
        const auto sourceId =
            uint32FieldAllowZeroFromRequest(request, "sourceContainerId");
        const auto targetId =
            uint32FieldAllowZeroFromRequest(request, "targetContainerId");
        const auto item =
            uint32FieldFromRequest(request, "itemId");
        if (!sourceId || !targetId || !item || *item > 0xFFFFu)
            return R"({"ok":false,"cmd":"MOVE_CONTAINER_ITEM","error":"invalid_arguments"})";
        return moveContainerItemResponse(
            *sourceId, *targetId, static_cast<std::uint16_t>(*item));
    }
    if (cmd == "ATTACK_SELECTED") return attackSelectedResponse();
    if (cmd == "ATTACK_ID") {
        const auto id = uint32FieldFromRequest(request, "creatureId");
        if (!id)
            return R"({"ok":false,"cmd":"ATTACK_ID","error":"invalid_creature_id"})";
        return attackIdResponse(*id);
    }
    return "{\"ok\":false,\"error\":\"unknown_command\",\"cmd\":\"" + jsonEscape(cmd) + "\"}";
}

void serveOne() {
    const auto name = pipeName();
    HANDLE pipe = CreateNamedPipeA(
        name.c_str(),
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        1, 8192, 8192, 1000, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        const auto err = GetLastError();
        g_status = "pipe_create_failed";
        logLine("[PIPE] CreateNamedPipe failed Win32=" + std::to_string(err) + " name=" + name);
        writeStatusFile();
        Sleep(500);
        return;
    }

    if (g_status != "listening") {
        g_status = "listening";
        logLine("[PIPE] listening name=" + name);
        writeStatusFile();
    }

    const BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE :
        (GetLastError() == ERROR_PIPE_CONNECTED ? TRUE : FALSE);

    if (connected) {
        char buffer[4096]{};
        DWORD read{};
        if (ReadFile(pipe, buffer, sizeof(buffer) - 1, &read, nullptr) && read) {
            std::string request(buffer, buffer + read);
            const auto response = handleRequest(request);
            DWORD written{};
            WriteFile(pipe, response.data(), static_cast<DWORD>(response.size()), &written, nullptr);
            FlushFileBuffers(pipe);
        }
    }

    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
}

DWORD WINAPI bridgeThread(LPVOID) {
    try {
        // Reset diagnostics on every fresh DLL load.
        try {
            std::ofstream clear(bridgeLogPath(), std::ios::trunc | std::ios::binary);
            if (clear) clear << "CromeRamoniaBridge V39 startup\r\n";
        } catch (...) {}

        g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        logLine("[START] pid=" + std::to_string(GetCurrentProcessId()));
        {
            std::ostringstream o;
            o << "[START] moduleBase=0x" << std::hex << std::uppercase << g_base;
            logLine(o.str());
        }

        try {
            const auto profilePath = findProfilePath();
            logLine("[PROFILE] path=" + profilePath);
            auto profile = loadProfile(profilePath);
            if (profile.architecture != "x86")
                throw std::runtime_error("profile architecture is not x86");
            logLine("[PROFILE] loaded schema=" + profile.schema +
                    " version=" + std::to_string(profile.profileVersion));
            g_profile = std::move(profile);
            g_profileError.clear();

            if (!readCurrentImageFingerprint())
                throw std::runtime_error("cannot read current image fingerprint");

            g_buildMatched =
                g_imageTimestamp == g_profile->expectedTimestamp &&
                g_imageSize == g_profile->expectedImageSize;

            {
                std::ostringstream fp;
                fp << "[BUILD] runtime timestamp=0x" << std::hex << std::uppercase
                   << g_imageTimestamp << " size=0x" << g_imageSize
                   << " matched=" << (g_buildMatched ? "true" : "false");
                logLine(fp.str());
            }

            if (!g_buildMatched)
                throw std::runtime_error("runtime build does not match canonical Ramonia profile");

            if (!prepareGameThreadDispatcher())
                throw std::runtime_error("game-thread target prepare failed");

            g_status = "runtime_ready";
        } catch (const std::exception& e) {
            g_buildMatched = false;
            g_profileError = e.what();
            g_status = "profile_error";
            logLine("[PROFILE ERROR] " + g_profileError);
        } catch (...) {
            g_buildMatched = false;
            g_profileError = "unknown profile initialization error";
            g_status = "profile_error";
            logLine("[PROFILE ERROR] unknown exception");
        }

        writeStatusFile();

        // Critical V2 change: always expose the pipe, even if profile loading failed.
        while (InterlockedCompareExchange(&g_running, 1, 1))
            serveOne();

    } catch (const std::exception& e) {
        g_status = "bridge_thread_error";
        g_profileError = e.what();
        logLine("[FATAL] " + g_profileError);
        writeStatusFile();
    } catch (...) {
        g_status = "bridge_thread_error";
        g_profileError = "unknown bridge thread error";
        logLine("[FATAL] unknown exception");
        writeStatusFile();
    }

    shutdownLookCapture();
    if (g_worldLightEnabled)
        restoreWorldLightHack();
    cleanupOnDemandDispatcher();
    g_gameThreadId = 0;
    g_gameWindow = nullptr;
    return 0;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
        InterlockedExchange(&g_running, 1);
        if (HANDLE h = CreateThread(nullptr, 0, bridgeThread, nullptr, 0, nullptr)) {
            CloseHandle(h);
        } else {
            g_status = "thread_create_failed";
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        InterlockedExchange(&g_running, 0);
    }
    return TRUE;
}
