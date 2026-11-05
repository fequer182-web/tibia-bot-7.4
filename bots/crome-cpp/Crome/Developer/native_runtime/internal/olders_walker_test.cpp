#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "../actions/OldersInternalActions.hpp"
#include "../bot/RouteConfig.hpp"
#include "../bot/Healing.hpp"
#include "../bot/Targeting.hpp"
#include "../bot/Walker.hpp"
#include "../profiles/ProfileConfig.hpp"
#include "../sdk/Conditions.hpp"
#include "../runtime/OldersBaseline.hpp"
#include "../runtime/OldersPipe.hpp"
#include "../runtime/LegacyWindowsPath.hpp"
#include "../runtime/LockedLogBuffer.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <iterator>
#include <string>
#include <vector>

namespace {

HMODULE g_module{};
volatile bool g_running = true;
std::uintptr_t g_base{};

std::string moduleDirectory(HMODULE mod) {
    char path[MAX_PATH]{};
    GetModuleFileNameA(mod, path, MAX_PATH);
    const std::string value(path);const auto slash=value.find_last_of("\\/");
    return slash==std::string::npos?std::string{}:value.substr(0,slash);
}

std::string currentExecutableName() {
    char path[MAX_PATH]{};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    const std::string value(path);const auto slash=value.find_last_of("\\/");
    return slash==std::string::npos?value:value.substr(slash+1);
}

std::uint32_t currentImageSize() {
    const auto base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
    if (!base) return 0;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    return nt->OptionalHeader.SizeOfImage;
}

bool equalsIgnoreCase(std::string a, std::string b) {
    std::transform(a.begin(), a.end(), a.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    std::transform(b.begin(), b.end(), b.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return a == b;
}

const crome::profiles::RuntimeProfile* selectInternalProfile(const crome::profiles::ProfilesFile& file) {
    const auto exe = currentExecutableName();
    const auto imageSize = currentImageSize();

    auto matches = [&](const crome::profiles::RuntimeProfile& p) {
        if (!p.enabled) return false;
        bool exeOk = false;
        for (const auto& e : p.executables) {
            if (equalsIgnoreCase(e, exe)) { exeOk = true; break; }
        }
        if (!exeOk) return false;
        if (p.moduleSize && *p.moduleSize != imageSize) return false;
        return true;
    };

    if (!file.activeProfile.empty() && file.activeProfile != "auto") {
        if (const auto* p = crome::profiles::findProfileById(file, file.activeProfile); p && matches(*p)) return p;
        return nullptr;
    }
    for (const auto& p : file.profiles) if (matches(p)) return &p;
    return nullptr;
}



int clampAxis(int value, int limit) {
    if (value > limit) return limit;
    if (value < -limit) return -limit;
    return value;
}

crome::Position chaseDestination(const crome::Position& self,
                                 const crome::Position& target,
                                 int desiredDistance) {
    crome::Position out = target;
    const int d = std::max(1, desiredDistance);
    out.x += clampAxis(self.x - target.x, d);
    out.y += clampAxis(self.y - target.y, d);
    out.z = target.z;
    return out;
}

struct ActionTestConfig {
    std::uint32_t useItemId{3578};
    std::uint32_t useWithSelfItemId{2874};
};

ActionTestConfig loadActionTestConfig(const std::string& path) {
    const auto root = crome::json::parse(crome::readRouteText(path));
    if (!root.isObject()) throw std::runtime_error("action_test root must be object");
    ActionTestConfig out;
    out.useItemId = crome::jsonU32(root.get("use_item_id"), out.useItemId);
    out.useWithSelfItemId = crome::jsonU32(root.get("use_with_self_item_id"), out.useWithSelfItemId);
    return out;
}

struct SayTestConfig {
    std::string text{"exura"};
    std::string alternateText{"exani tera"};
};

SayTestConfig loadSayTestConfig(const std::string& path) {
    const auto root = crome::json::parse(crome::readRouteText(path));
    if (!root.isObject()) throw std::runtime_error("say_test root must be object");
    SayTestConfig out;
    if (const auto* v = root.get("text"); v && v->isString()) out.text = v->string();
    if (const auto* v = root.get("alternate_text"); v && v->isString()) out.alternateText = v->string();
    return out;
}

struct TileActionTestConfig {
    crome::Position shovelTarget{32865, 32034, 7};
    crome::Position ropeTarget{32865, 32034, 7};
    bool ropeUseCurrentZ{true};
    std::uint32_t shovelItemId{3457};
    std::uint32_t ropeItemId{3003};
    std::uint32_t fishingRodItemId{3483};
    std::vector<std::uint16_t> fishableWaterIds{4597,4598,4599,4600,4601,4602};
    int fishingRange{7};
    std::string magicRopeSpell{"exani tera"};
    bool magicRopeEnabled{false};
};

TileActionTestConfig loadTileActionTestConfig(const std::string& path) {
    const auto root = crome::json::parse(crome::readRouteText(path));
    if (!root.isObject()) throw std::runtime_error("tile_action_test root must be object");
    TileActionTestConfig out;
    auto loadTarget = [](const crome::json::Value* t, crome::Position& outPos, bool* currentZ) {
        if (!t || !t->isObject()) return;
        outPos.x = crome::jsonInt(t->get("x"), outPos.x);
        outPos.y = crome::jsonInt(t->get("y"), outPos.y);
        if (const auto* z = t->get("z")) {
            if (z->isNumber()) {
                outPos.z = static_cast<std::int32_t>(z->number());
                if (currentZ) *currentZ = false;
            } else if (currentZ && z->isString() && z->string() == "current") {
                *currentZ = true;
            }
        }
    };

    const auto* legacy = root.get("hole_target");
    if (!legacy) legacy = root.get("target");
    loadTarget(legacy, out.shovelTarget, nullptr);
    out.ropeTarget = out.shovelTarget;

    loadTarget(root.get("shovel_target"), out.shovelTarget, nullptr);
    loadTarget(root.get("rope_target"), out.ropeTarget, &out.ropeUseCurrentZ);
    out.shovelItemId = crome::jsonU32(root.get("shovel_item_id"), out.shovelItemId);
    out.ropeItemId = crome::jsonU32(root.get("rope_item_id"), out.ropeItemId);
    out.fishingRodItemId = crome::jsonU32(root.get("fishing_rod_item_id"), out.fishingRodItemId);
    out.fishingRange = crome::jsonInt(root.get("fishing_range"), out.fishingRange);
    if (const auto* ids = root.get("fishable_water_ids"); ids && ids->isArray()) {
        std::vector<std::uint16_t> parsed;
        for (const auto& v : ids->array()) {
            if (!v.isNumber()) continue;
            const auto id = static_cast<int>(v.number());
            if (id > 0 && id <= 65535) parsed.push_back(static_cast<std::uint16_t>(id));
        }
        if (!parsed.empty()) out.fishableWaterIds = std::move(parsed);
    }
    if (const auto* sp = root.get("magic_rope_spell"); sp && sp->isString())
        out.magicRopeSpell = sp->string();
    if (const auto* mr = root.get("magic_rope_enabled"); mr && mr->isBool())
        out.magicRopeEnabled = mr->boolean();
    return out;
}


struct FishingConfig {
    bool enabled{false};
    std::uint32_t rodId{3483};
    std::vector<std::uint16_t> waterIds{4597,4598,4599,4600,4601,4602};
    int range{7};
    std::uint32_t minDelayMs{850};
    std::uint32_t maxDelayMs{1450};
    bool pauseWhileWalker{true};
    bool pauseWhileTargeting{true};
};

FishingConfig loadFishingConfig(const std::string& path) {
    const auto root = crome::json::parse(crome::readRouteText(path));
    if (!root.isObject()) throw std::runtime_error("fishing root must be object");
    FishingConfig out;
    if (const auto* v=root.get("enabled"); v&&v->isBool()) out.enabled=v->boolean();
    out.rodId=crome::jsonU32(root.get("rod_id"),out.rodId);
    out.range=crome::jsonInt(root.get("range"),out.range);
    out.minDelayMs=crome::jsonU32(root.get("min_delay_ms"),out.minDelayMs);
    out.maxDelayMs=crome::jsonU32(root.get("max_delay_ms"),out.maxDelayMs);
    if(out.maxDelayMs<out.minDelayMs) std::swap(out.maxDelayMs,out.minDelayMs);
    if(const auto* v=root.get("pause_while_walker");v&&v->isBool())out.pauseWhileWalker=v->boolean();
    if(const auto* v=root.get("pause_while_targeting");v&&v->isBool())out.pauseWhileTargeting=v->boolean();
    if(const auto* ids=root.get("water_ids");ids&&ids->isArray()){
        std::vector<std::uint16_t> parsed; for(const auto& v:ids->array()){ if(!v.isNumber())continue; const int id=(int)v.number(); if(id>0&&id<=65535)parsed.push_back((std::uint16_t)id); }
        if(!parsed.empty()) out.waterIds=std::move(parsed);
    }
    return out;
}


struct ToolsConfig {
    crome::ChangeGoldConfig changeGold;
    bool foodEnabled{false}; std::vector<std::uint32_t> foodIds{}; std::uint32_t foodIntervalMs{60000};
    bool manaEnabled{false}; std::string manaSpell{"utamo vita"}; std::uint32_t manaIntervalMs{170000};
    bool paraEnabled{false}; std::string paraSpell{"exura"};
    bool hasteEnabled{false}; std::string hasteSpell{"utani hur"}; std::uint32_t hasteIntervalMs{30000};
};

ToolsConfig loadToolsConfig(const std::string& path) {
    ToolsConfig out;
    const auto root = crome::json::parse(crome::readRouteText(path));
    if (!root.isObject()) return out;
    out.changeGold=crome::parseChangeGoldConfig(root.get("change_gold"));
    auto parseIds=[&](const crome::json::Value* obj,const char* key,std::vector<std::uint32_t>& dst){ if(!obj||!obj->isObject())return; if(const auto* a=obj->get(key);a&&a->isArray()){ dst.clear(); for(const auto& v:a->array()) if(v.isNumber()){ auto id=(std::uint32_t)v.number(); if(id) dst.push_back(id);} } };
    if(const auto* o=root.get("food");o&&o->isObject()){ if(const auto* v=o->get("enabled");v&&v->isBool())out.foodEnabled=v->boolean(); out.foodIntervalMs=crome::jsonU32(o->get("interval_ms"),out.foodIntervalMs); parseIds(o,"item_ids",out.foodIds); }
    if(const auto* o=root.get("mana_shield");o&&o->isObject()){ if(const auto* v=o->get("enabled");v&&v->isBool())out.manaEnabled=v->boolean(); if(const auto* v=o->get("spell");v&&v->isString())out.manaSpell=v->string(); out.manaIntervalMs=crome::jsonU32(o->get("fallback_interval_ms"),out.manaIntervalMs); }
    if(const auto* o=root.get("anti_paralyze");o&&o->isObject()){ if(const auto* v=o->get("enabled");v&&v->isBool())out.paraEnabled=v->boolean(); if(const auto* v=o->get("spell");v&&v->isString())out.paraSpell=v->string(); }
    if(const auto* o=root.get("auto_haste");o&&o->isObject()){ if(const auto* v=o->get("enabled");v&&v->isBool())out.hasteEnabled=v->boolean(); if(const auto* v=o->get("spell");v&&v->isString())out.hasteSpell=v->string(); out.hasteIntervalMs=crome::jsonU32(o->get("fallback_interval_ms"),out.hasteIntervalMs); }
    return out;
}

std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct CaptureResult {
    bool ok = false;
    std::size_t insertedIndex = 0;
    bool insertedBeforeGoto = false;
};

CaptureResult captureWaypoint(const std::string& routePath, const crome::Position& p, crome::RouteConfig& route) {
    crome::RouteWaypoint w;
    w.type = crome::RouteWaypointType::Walk;
    w.position = p;

    // Route scripts commonly end with an unconditional GOTO back to a label.
    // A captured point appended after that GOTO would never be executed.
    // Insert immediately before the first GOTO instead.
    auto it = std::find_if(route.waypoints.begin(), route.waypoints.end(),
        [](const crome::RouteWaypoint& item) {
            return item.type == crome::RouteWaypointType::Goto;
        });

    const std::size_t index = static_cast<std::size_t>(std::distance(route.waypoints.begin(), it));
    const bool beforeGoto = it != route.waypoints.end();
    route.waypoints.insert(it, w);

    if (!crome::saveRouteConfig(routePath, route)) {
        route.waypoints.erase(route.waypoints.begin() + static_cast<std::ptrdiff_t>(index));
        return {};
    }

    return CaptureResult{true, index, beforeGoto};
}

void printRouteFlowWarnings(const crome::RouteConfig& route) {
    // Simple diagnostic for the common capture mistake from V11: an unconditional
    // GOTO followed by plain WALK/WAIT nodes without a LABEL entry point.
    for (std::size_t i = 0; i < route.waypoints.size(); ++i) {
        if (route.waypoints[i].type != crome::RouteWaypointType::Goto) continue;
        bool labelAfter = false;
        for (std::size_t j = i + 1; j < route.waypoints.size(); ++j) {
            if (route.waypoints[j].type == crome::RouteWaypointType::Label) {
                labelAfter = true;
                break;
            }
        }
        if (!labelAfter && i + 1 < route.waypoints.size()) {
            std::cout << "[WARN] Existem " << (route.waypoints.size() - i - 1)
                      << " node(s) depois do GOTO #" << (i + 1)
                      << ". Sem LABEL de entrada, eles nao serao executados.\n";
        }
        break;
    }
}

DWORD WINAPI worker(LPVOID) {
    wchar_t modulePath[32768]{};GetModuleFileNameW(g_module,modulePath,32768);
    const auto baseDirWide=std::filesystem::path(modulePath).parent_path();
    std::ofstream runtimeLog(baseDirWide/L"CromeRuntime.log",std::ios::app);
    crome::LockedLogBuffer lockedLog(runtimeLog.rdbuf());
    auto* priorOut=std::cout.rdbuf(); auto* priorError=std::cerr.rdbuf();
    if(runtimeLog){std::cout.rdbuf(&lockedLog);std::cerr.rdbuf(&lockedLog);}

    std::cout << "\n============================================================\n"
              << "       CROME SDK V21.6 - FOLLOW BLOCKER / SAFE CHASE / HEALING / WALKER\n"
              << "============================================================\n";

    g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));

    try {
        const auto baseDir = moduleDirectory(g_module);
        const auto profilesPath = baseDir + "\\profiles\\builds.json";
        const auto routePath = baseDir + "\\config\\waypoints.json";
        const auto actionTestPath = baseDir + "\\config\\action_test.json";
        const auto tileActionTestPath = baseDir + "\\config\\tile_action_test.json";
        const auto sayTestPath = baseDir + "\\config\\say_test.json";
        const auto healingPath = baseDir + "\\config\\healing.json";
        const auto targetingPath = baseDir + "\\config\\targeting.json";
        const auto fishingPath = baseDir + "\\config\\fishing.json";
        const auto lootPath = baseDir + "\\config\\loot.json";
        const auto toolsPath = baseDir + "\\config\\tools.json";
        const auto inspectRequestPath = baseDir + "\\config\\inspect_request.json";
        const auto inspectResultPath = baseDir + "\\config\\inspect_result.json";
        const auto lookResultPath = baseDir + "\\config\\look_result.json";

        wchar_t executable[32768]{};
        if(!GetModuleFileNameW(nullptr,executable,32768)||!crome::olders_baseline::matchesExecutable(executable)){
            std::cout<<"[STOP] Legacy Olders runtime requires exact timestamp/size/disk text hash.\n"; goto done;
        }
        auto profiles = crome::profiles::loadProfiles(profilesPath);
        const auto* profile = selectInternalProfile(profiles);
        if (!profile) {
            std::cout << "[-] Nenhum profile corresponde ao executavel/module_size atual.\n";
            goto done;
        }

        std::cout << "[+] Profile          : " << profile->id << "\n"
                  << "[+] Executable       : " << currentExecutableName() << "\n"
                  << std::hex << std::uppercase
                  << "[+] ModuleBase       : 0x" << g_base << "\n"
                  << "[+] ModuleSize       : 0x" << currentImageSize() << "\n";
        if (profile->actions.walk)
            std::cout << "[+] walkTo wrapper   : RVA 0x" << *profile->actions.walk << "\n";
        if (profile->actions.attack)
            std::cout << "[+] Game::attack     : RVA 0x" << *profile->actions.attack << "\n";
        if (profile->actions.use)
            std::cout << "[+] Game::use        : RVA 0x" << *profile->actions.use << "\n";
        if (profile->actions.useWith)
            std::cout << "[+] Game::useWith    : RVA 0x" << *profile->actions.useWith << "\n";
        if (profile->runtime.mapGetTile)
            std::cout << "[+] Map::getTile     : RVA 0x" << *profile->runtime.mapGetTile << "\n";
        if (profile->actions.sayDefault)
            std::cout << "[+] Game::talk       : RVA 0x" << *profile->actions.sayDefault << "\n";
        std::cout << std::dec
                  << "[+] Route JSON       : " << routePath << "\n"
                  << "[+] Action test JSON : " << actionTestPath << "\n"
                  << "[+] Tile action JSON : " << tileActionTestPath << "\n"
                  << "[+] Say test JSON    : " << sayTestPath << "\n"
                  << "[+] Healing JSON     : " << healingPath << "\n"
                  << "[+] Targeting JSON   : " << targetingPath << "\n"
                  << "[+] Fishing JSON     : " << fishingPath << "\n";

        crome::OldersInternalActions actions(g_base, *profile);
        actions.setInspectResultPath(inspectResultPath);
        actions.setLookResultPath(lookResultPath);
        crome::Walker walker(actions, [](const std::string& s){ std::cout << s << "\n"; });
        crome::HealingController healer(actions);
        crome::TargetingController targeter;

        std::cout << "\n[WAIT] Aguardando personagem/world ready...\n";
        while (g_running && !actions.isWorldReady()) {
            if (GetAsyncKeyState(VK_END) & 1) { g_running = false; break; }
            Sleep(250);
        }
        if (!g_running) goto done;

        while (g_running && !actions.validateRuntime(true)) {
            std::cout << "[WAIT] Runtime ainda nao estavel; nova tentativa em 500ms.\n";
            Sleep(500);
        }
        if (!g_running) goto done;
        while (g_running && !actions.installDispatcher()) {
            std::cout << "[WAIT] Dispatcher indisponivel; mantendo DLL carregada e tentando novamente em 700ms.\n";
            Sleep(700);
        }
        if (!g_running) goto done;

        crome::olders_pipe::Server pipeServer{baseDirWide};
        pipeServer.start(); pipeServer.setReady(true);
        std::cout<<"[PIPE READY] Olders legacy actions connected.\n";
        crome::RouteConfig route = crome::loadRouteConfig(routePath);
        walker.setRoute(route);
        std::cout << "[OK] Waypoints carregados: " << route.waypoints.size()
                  << " enabled=" << (route.enabled ? "true" : "false") << "\n";
        printRouteFlowWarnings(route);
        if (route.enabled && !route.waypoints.empty()) walker.start(nowMs(), actions.currentPosition());

        crome::HealingConfig healingConfig = crome::loadHealingConfig(healingPath);
        bool healingEnabled = healingConfig.enabled;
        std::uint64_t nextHealingPoll = 0;
        std::cout << "[OK] Healing config: rules=" << healingConfig.rules.size()
                  << " enabled=" << (healingEnabled ? "true" : "false")
                  << " poll=" << healingConfig.pollMs << "ms\n";

        crome::TargetingConfig targetingConfig = crome::loadTargetingConfig(targetingPath);
        bool targetingEnabled = targetingConfig.enabled;
        actions.setManualInteractionPauseMs(targetingConfig.manualInputPauseMs);
        std::uint64_t nextTargetingPoll = 0;
        crome::OldersLootConfig lootConfig{};
        std::uint64_t nextLootReloadMs=0,nextLootPollMs=0;
        bool walkerPausedByLoot=false,resumeWalkerAfterLoot=false;
        std::string lastLootStatus;
        bool walkerPausedByTarget = false;
        bool resumeWalkerAfterTarget = false;
        std::uint64_t targetMissingSince = 0;
        std::uint64_t lastAttackReassert = 0;
        std::uint64_t lastChaseMs = 0;
        std::uint64_t lastChaseProgressMs = 0;
        int lastChaseRemaining = 9999;
        crome::Position lastChaseDestination{};
        bool haveLastChaseDestination = false;
        bool manualInteractionWasActive = false;
        constexpr std::uint64_t kTargetMissingGraceMs = 1200;
        constexpr std::uint64_t kAttackReassertMs = 900;
        std::uint64_t lastInspectSeq = 0;
        std::uint64_t nextInspectPoll = 0;
        std::cout << "[OK] Targeting config: targets=" << targetingConfig.targets.size()
                  << " enabled=" << (targetingEnabled ? "true" : "false")
                  << " any=" << (targetingConfig.attackAnyMonster ? "true" : "false")
                  << " range=" << targetingConfig.maxRange
                  << " chase=" << (targetingConfig.chaseEnabled ? "true" : "false")
                  << " chaseDist=" << targetingConfig.chaseDistance
                  << " chaseScan=" << targetingConfig.chaseScanRange
                  << " repath=" << targetingConfig.chaseRepathMs << "ms"
                  << " manualPause=" << targetingConfig.manualInputPauseMs << "ms"
                  << " poll=" << targetingConfig.pollMs << "ms\n";

        FishingConfig fishingConfig{};
        try { fishingConfig = loadFishingConfig(fishingPath); }
        catch(...) { std::cout << "[WARN] fishing.json ausente/invalido; Auto Fishing OFF.\n"; }
        std::uint64_t nextFishingMs = 0;
        std::uint64_t nextFishingReloadMs = 0;
        ToolsConfig toolsConfig{}; try { toolsConfig=loadToolsConfig(toolsPath); } catch(...) {}
        std::uint64_t nextToolsReloadMs=0, nextFoodMs=0, nextManaMs=0, nextHasteMs=0, nextParaMs=0;
        std::uint64_t nextChangeGoldMs=0;
        std::cout << "[OK] Fishing config: enabled=" << (fishingConfig.enabled?"true":"false")
                  << " rod=" << fishingConfig.rodId << " waterIds=" << fishingConfig.waterIds.size()
                  << " range=" << fishingConfig.range << " delay=" << fishingConfig.minDelayMs << "-" << fishingConfig.maxDelayMs << "ms\n";

        if (targetingEnabled) {
            if (actions.currentFollowTargetAddress() != 0) {
                std::cout << "[TARGET] Follow ja estava ativo no cliente; Targeting inicia OFF por seguranca.\n";
                targetingEnabled = false;
            } else if (!actions.setFollowBlocked(true)) {
                std::cout << "[TARGET] Falha ao ativar Follow Blocker; Targeting inicia OFF.\n";
                targetingEnabled = false;
            }
        }

        std::cout << "\n============================================================\n"
                  << "                         READY\n"
                  << "============================================================\n"
                  << "F1  = SAY direto do config\\say_test.json (Shift+F1 = alternate_text)\n"
                  << "F2  = inspecionar shovel target (tile_action_test.json)\n"
                  << "F3  = SHOVEL 3457 no target tile\n"
                  << "F4  = ROPE 3003 no target tile\n"
                  << "F5  = FISHING ROD 3483 -> procura water tile proximo\n"
                  << "F6  = validar runtime\n"
                  << "F7  = mostrar LocalPlayer/Position\n"
                  << "F8  = iniciar/pausar Walker\n"
                  << "F9  = recarregar config\\waypoints.json\n"
                  << "F10 = capturar posicao atual no waypoints.json\n"
                  << "F11 = Game::use item de config\\action_test.json\n"
                  << "F12 = Game::useWith item -> LocalPlayer\n"
                  << "HOME     = Healing ON/OFF\n"
                  << "INSERT   = recarregar config\\healing.json\n"
                  << "DELETE   = Targeting ON/OFF\n"
                  << "PAGEDOWN = recarregar config\\targeting.json\n"
                  << "Route nodes: say / use / use_with_self / use_with_ground / shovel / rope / fish\n"
                  << "Healing: estado -> regra -> Game::talk (sem confirmacao pos-cast)\n"
                  << "Targeting: sticky attack -> safe chase via walkTo -> Game::follow blocker\n"
                  << "Direct talk: Game::talk -> talkChannel -> sendTalk opcode 0x96\n"
                  << "END = descarregar DLL\n"
                  << "============================================================\n";

        bool oldF1=false, oldF2=false, oldF3=false, oldF4=false, oldF5=false;
        bool oldF6=false, oldF7=false, oldF8=false, oldF9=false, oldF10=false;
        bool oldF11=false, oldF12=false, oldHome=false, oldInsert=false;
        bool oldDelete=false, oldPageDown=false, oldEnd=false;

        auto safeWriteTime = [](const std::string& path) {
            std::error_code ec;
            auto t = std::filesystem::last_write_time(crome::legacyWindowsPath(path), ec);
            return ec ? std::filesystem::file_time_type::min() : t;
        };
        auto routeMtime = safeWriteTime(routePath);
        auto healingMtime = safeWriteTime(healingPath);
        auto targetingMtime = safeWriteTime(targetingPath);
        std::uint64_t nextConfigPoll = 0;
        bool walkerPausedByManualFollow = false;
        bool resumeWalkerAfterManualFollow = false;

        while (g_running) {
            const bool f1 = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
            const bool f2 = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
            const bool f3 = (GetAsyncKeyState(VK_F3) & 0x8000) != 0;
            const bool f4 = (GetAsyncKeyState(VK_F4) & 0x8000) != 0;
            const bool f5 = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
            const bool f6 = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
            const bool f7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
            const bool f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
            const bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
            const bool f10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
            const bool f11 = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
            const bool f12 = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
            const bool home = (GetAsyncKeyState(VK_HOME) & 0x8000) != 0;
            const bool insertKey = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
            const bool deleteKey = (GetAsyncKeyState(VK_DELETE) & 0x8000) != 0;
            const bool pageDown = (GetAsyncKeyState(VK_NEXT) & 0x8000) != 0;
            const bool end = (GetAsyncKeyState(VK_END) & 0x8000) != 0;

            const auto p = actions.currentPosition();
            const auto t = nowMs();

            const bool manualFollowActive = !targetingEnabled && actions.currentFollowTargetAddress() != 0;
            if (manualFollowActive && !walkerPausedByManualFollow) {
                resumeWalkerAfterManualFollow = walker.running();
                if (resumeWalkerAfterManualFollow) walker.pause();
                walkerPausedByManualFollow = true;
                std::cout << "[FOLLOW] Manual Follow ativo: Walker pausado para evitar disputa de movimento.\n";
            } else if (!manualFollowActive && walkerPausedByManualFollow) {
                if (resumeWalkerAfterManualFollow && !walker.running()) walker.start(t, p);
                walkerPausedByManualFollow = false;
                resumeWalkerAfterManualFollow = false;
                std::cout << "[FOLLOW] Manual Follow terminou: Walker restaurado.\n";
            }

            if (t >= nextInspectPoll) {
                nextInspectPoll = t + 150;
                try {
                    if (std::filesystem::exists(crome::legacyWindowsPath(inspectRequestPath))) {
                        const auto rootReq = crome::json::parse(crome::readRouteText(inspectRequestPath));
                        if (rootReq.isObject()) {
                            const auto seq = static_cast<std::uint64_t>(crome::jsonU32(rootReq.get("seq"), 0));
                            if (seq && seq != lastInspectSeq) {
                                crome::Position ip{
                                    crome::jsonInt(rootReq.get("x"), p.x),
                                    crome::jsonInt(rootReq.get("y"), p.y),
                                    crome::jsonInt(rootReq.get("z"), p.z)
                                };
                                lastInspectSeq = seq;
                                std::cout << "[INSPECT] request seq=" << seq << " pos=" << ip.x << "," << ip.y << "," << ip.z << "\n";
                                actions.inspectTile(ip);
                            }
                        }
                    }
                } catch (...) {}
            }

            if (t >= nextConfigPoll) {
                nextConfigPoll = t + 250;
                const auto rm = safeWriteTime(routePath);
                if (rm != routeMtime) {
                    routeMtime = rm;
                    try {
                        route = crome::loadRouteConfig(routePath);
                        walker.setRoute(route);
                        if (route.enabled && !route.waypoints.empty()) {
                            if (!walker.running()) walker.start(t, p);
                        } else if (walker.running()) walker.pause();
                        std::cout << "[SYNC] Waypoints reload: enabled=" << (route.enabled?"true":"false")
                                  << " nodes=" << route.waypoints.size() << "\n";
                    } catch (const std::exception& e) { std::cout << "[SYNC] waypoints ERRO: " << e.what() << "\n"; }
                }
                const auto hm = safeWriteTime(healingPath);
                if (hm != healingMtime) {
                    healingMtime = hm;
                    try {
                        healingConfig = crome::loadHealingConfig(healingPath);
                        healingEnabled = healingConfig.enabled;
                        healer.resetCooldowns();
                        nextHealingPoll = 0;
                        std::cout << "[SYNC] Healing=" << (healingEnabled?"ON":"OFF")
                                  << " rules=" << healingConfig.rules.size() << "\n";
                    } catch (const std::exception& e) { std::cout << "[SYNC] healing ERRO: " << e.what() << "\n"; }
                }
                const auto tm = safeWriteTime(targetingPath);
                if (tm != targetingMtime) {
                    targetingMtime = tm;
                    try {
                        const bool was = targetingEnabled;
                        targetingConfig = crome::loadTargetingConfig(targetingPath);
                        actions.setManualInteractionPauseMs(targetingConfig.manualInputPauseMs);
                        targetingEnabled = false;
                        actions.setFollowBlocked(false);
                        if (targetingConfig.enabled && actions.currentFollowTargetAddress() == 0 && actions.setFollowBlocked(true))
                            targetingEnabled = true;
                        targeter.reset();
                        targetMissingSince = 0; lastAttackReassert = 0; lastChaseMs = 0; lastChaseProgressMs = 0;
                        lastChaseRemaining = 9999; haveLastChaseDestination = false; manualInteractionWasActive = false; nextTargetingPoll = 0;
                        std::cout << "[SYNC] Targeting=" << (targetingEnabled?"ON":"OFF")
                                  << " any=" << (targetingConfig.attackAnyMonster?"true":"false")
                                  << " targets=" << targetingConfig.targets.size() << "\n";
                        (void)was;
                    } catch (const std::exception& e) { std::cout << "[SYNC] targeting ERRO: " << e.what() << "\n"; }
                }
            }

            if (f1 && !oldF1) {
                walker.pause();
                try {
                    const auto cfg = loadSayTestConfig(sayTestPath);
                    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                    const auto& text = shift ? cfg.alternateText : cfg.text;
                    std::cout << "[F1] SAY direct text=\"" << text << "\""
                              << (shift ? " (alternate)" : "") << "\n";
                    const bool ok = actions.sayDefault(text);
                    std::cout << "[F1] request=" << (ok ? "accepted" : "failed") << "\n";
                } catch (const std::exception& e) {
                    std::cout << "[F1] ERRO: " << e.what() << "\n";
                }
            }

            if (f2 && !oldF2) {
                walker.pause();
                try {
                    const auto cfg = loadTileActionTestConfig(tileActionTestPath);
                    std::cout << "[F2] Inspect tile -> " << cfg.shovelTarget.x << "," << cfg.shovelTarget.y << "," << cfg.shovelTarget.z << "\n";
                    const bool ok = actions.inspectTile(cfg.shovelTarget);
                    std::cout << "[F2] request=" << (ok ? "accepted" : "failed") << "\n";
                } catch (const std::exception& e) { std::cout << "[F2] ERRO: " << e.what() << "\n"; }
            }

            if (f3 && !oldF3) {
                walker.pause();
                try {
                    const auto cfg = loadTileActionTestConfig(tileActionTestPath);
                    const crome::Item item{cfg.shovelItemId, 0, 1};
                    std::cout << "[F3] SHOVEL item=" << cfg.shovelItemId << " target="
                              << cfg.shovelTarget.x << "," << cfg.shovelTarget.y << "," << cfg.shovelTarget.z << "\n";
                    const bool ok = actions.useWithGround(item, cfg.shovelTarget);
                    std::cout << "[F3] request=" << (ok ? "accepted" : "failed") << "\n";
                } catch (const std::exception& e) { std::cout << "[F3] ERRO: " << e.what() << "\n"; }
            }

            if (f4 && !oldF4) {
                walker.pause();
                try {
                    const auto cfg = loadTileActionTestConfig(tileActionTestPath);
                    const crome::Item item{cfg.ropeItemId, 0, 1};
                    crome::Position target = cfg.ropeTarget;
                    if (cfg.ropeUseCurrentZ) target.z = p.z;
                    std::cout << "[F4] ROPE item=" << cfg.ropeItemId << " target="
                              << target.x << "," << target.y << "," << target.z
                              << (cfg.ropeUseCurrentZ ? " (z=current)" : "") << "\n";
                    const bool ok = actions.useWithGround(item, target);
                    std::cout << "[F4] request=" << (ok ? "accepted" : "failed") << "\n";
                } catch (const std::exception& e) { std::cout << "[F4] ERRO: " << e.what() << "\n"; }
            }

            if (f5 && !oldF5) {
                walker.pause();
                try {
                    const auto cfg = loadTileActionTestConfig(tileActionTestPath);
                    const crome::Item item{cfg.fishingRodItemId, 0, 1};
                    std::cout << "[F5] FISHING item=" << cfg.fishingRodItemId
                              << " auto-water range=" << cfg.fishingRange
                              << " ids=" << cfg.fishableWaterIds.size() << "\n";
                    const bool ok = actions.useWithNearestGroundIds(
                        item, cfg.fishableWaterIds, cfg.fishingRange);
                    std::cout << "[F5] request=" << (ok ? "accepted" : "failed") << "\n";
                } catch (const std::exception& e) { std::cout << "[F5] ERRO: " << e.what() << "\n"; }
            }

            if (f6 && !oldF6) actions.validateRuntime(true);

            if (f7 && !oldF7) {
                std::cout << "[F7] LocalPlayer=0x" << std::hex << std::uppercase << actions.localPlayer() << std::dec
                          << " Position=" << p.x << "," << p.y << "," << p.z
                          << " Walker=" << (walker.running() ? "RUNNING" : "PAUSED")
                          << " index=" << (walker.size() ? walker.index()+1 : 0) << "/" << walker.size()
                          << " Healing=" << (healingEnabled ? "ON" : "OFF")
                          << " Targeting=" << (targetingEnabled ? "ON" : "OFF");
                if (targeter.hasLock()) {
                    std::cout << " TargetLock=0x" << std::hex << std::uppercase
                              << targeter.lockedAddress() << std::dec;
                }
                if (const auto self = actions.localPlayerSnapshot()) {
                    std::cout << std::fixed << std::setprecision(1)
                              << " HP=" << self->health << "/" << self->maxHealth
                              << " (" << self->healthPercent() << "%)"
                              << " MP=" << self->mana << "/" << self->maxMana
                              << " states=0x" << std::hex << self->states << std::dec
                              << std::defaultfloat;
                }
                std::cout << "\n";
            }

            if (f8 && !oldF8) {
                if(walkerPausedByLoot){resumeWalkerAfterLoot=false;}
                walker.toggle(t, p);
                std::cout << "[F8] Walker=" << (walker.running() ? "RUNNING" : "PAUSED") << "\n";
            }

            if (f9 && !oldF9) {
                const bool wasRunning = walker.running();
                walker.pause();
                try {
                    route = crome::loadRouteConfig(routePath);
                    walker.setRoute(route);
                    std::cout << "[F9] Route recarregada. waypoints=" << route.waypoints.size() << "\n";
                    printRouteFlowWarnings(route);
                    if (wasRunning) walker.start(t, p);
                } catch (const std::exception& e) {
                    std::cout << "[F9] ERRO: " << e.what() << "\n";
                }
            }

            if (f10 && !oldF10) {
                walker.pause();
                const auto captured = captureWaypoint(routePath, p, route);
                if (captured.ok) {
                    walker.setRoute(route);
                    std::cout << "[F10] Waypoint capturado #" << (captured.insertedIndex + 1)
                              << ": " << p.x << "," << p.y << "," << p.z;
                    if (captured.insertedBeforeGoto) std::cout << " (inserido antes do GOTO)";
                    std::cout << "\n";
                } else {
                    std::cout << "[F10] Falha salvando waypoint.\n";
                }
            }

            if (f11 && !oldF11) {
                walker.pause();
                try {
                    const auto cfg = loadActionTestConfig(actionTestPath);
                    const crome::Item item{cfg.useItemId, 0, 1};
                    std::cout << "[F11] Request Game::use item_id=" << cfg.useItemId << "\n";
                    const bool ok = actions.use(item);
                    std::cout << "[F11] request=" << (ok ? "accepted" : "failed") << "\n";
                } catch (const std::exception& e) {
                    std::cout << "[F11] ERRO: " << e.what() << "\n";
                }
            }

            if (f12 && !oldF12) {
                walker.pause();
                try {
                    const auto cfg = loadActionTestConfig(actionTestPath);
                    const crome::Item item{cfg.useWithSelfItemId, 0, 1};
                    const auto selfPos = actions.currentPosition();
                    std::cout << "[F12] Request Game::useWith item_id="
                              << cfg.useWithSelfItemId << " -> LocalPlayer\n";
                    const bool ok = actions.useWith(item, selfPos);
                    std::cout << "[F12] request=" << (ok ? "accepted" : "failed") << "\n";
                } catch (const std::exception& e) {
                    std::cout << "[F12] ERRO: " << e.what() << "\n";
                }
            }

            if (home && !oldHome) {
                healingEnabled = !healingEnabled;
                if (healingEnabled) nextHealingPoll = 0;
                std::cout << "[HOME] Healing=" << (healingEnabled ? "ON" : "OFF") << "\n";
            }

            if (insertKey && !oldInsert) {
                try {
                    healingConfig = crome::loadHealingConfig(healingPath);
                    healingEnabled = healingConfig.enabled;
                    healer.resetCooldowns();
                    nextHealingPoll = 0;
                    std::cout << "[INSERT] Healing recarregado. rules=" << healingConfig.rules.size()
                              << " enabled=" << (healingEnabled ? "true" : "false")
                              << " poll=" << healingConfig.pollMs << "ms\n";
                } catch (const std::exception& e) {
                    std::cout << "[INSERT] ERRO: " << e.what() << "\n";
                }
            }

            if (deleteKey && !oldDelete) {
                if (!targetingEnabled) {
                    if (actions.currentFollowTargetAddress() != 0) {
                        std::cout << "[DELETE] Follow do jogo ja esta ativo. Pare o Follow primeiro; Targeting continua OFF.\n";
                    } else if (!actions.setFollowBlocked(true)) {
                        std::cout << "[DELETE] Falha ao ativar Follow Blocker; Targeting continua OFF.\n";
                    } else {
                        targetingEnabled = true;
                        std::cout << "[DELETE] Targeting=ON (Game::follow bloqueado)\n";
                    }
                } else {
                    targetingEnabled = false;
                    actions.setFollowBlocked(false);
                    targeter.reset();
                    targetMissingSince = 0;
                    lastAttackReassert = 0;
                    lastChaseMs = 0;
                    lastChaseProgressMs = 0;
                    lastChaseRemaining = 9999;
                    haveLastChaseDestination = false;
                    manualInteractionWasActive = false;
                    nextTargetingPoll = 0;
                    if (walkerPausedByTarget) {
                        if (resumeWalkerAfterTarget && !walker.running()) walker.start(t, p);
                        walkerPausedByTarget = false;
                        resumeWalkerAfterTarget = false;
                    }
                    std::cout << "[DELETE] Targeting=OFF (Game::follow restaurado)\n";
                }
            }

            if (pageDown && !oldPageDown) {
                try {
                    targetingConfig = crome::loadTargetingConfig(targetingPath);
                    const bool requestedTargeting = targetingConfig.enabled;
                    targetingEnabled = false;
                    actions.setManualInteractionPauseMs(targetingConfig.manualInputPauseMs);
                    actions.setFollowBlocked(false);
                    if (requestedTargeting && actions.currentFollowTargetAddress() == 0 && actions.setFollowBlocked(true))
                        targetingEnabled = true;
                    targeter.reset();
                    targetMissingSince = 0;
                    lastAttackReassert = 0;
                    lastChaseMs = 0;
                    lastChaseProgressMs = 0;
                    lastChaseRemaining = 9999;
                    haveLastChaseDestination = false;
                    manualInteractionWasActive = false;
                    nextTargetingPoll = 0;
                    if (!targetingEnabled && walkerPausedByTarget) {
                        if (resumeWalkerAfterTarget && !walker.running()) walker.start(t, p);
                        walkerPausedByTarget = false;
                        resumeWalkerAfterTarget = false;
                    }
                    std::cout << "[PAGEDOWN] Targeting recarregado. targets=" << targetingConfig.targets.size()
                              << " enabled=" << (targetingEnabled ? "true" : "false")
                              << " any=" << (targetingConfig.attackAnyMonster ? "true" : "false")
                              << " range=" << targetingConfig.maxRange
                              << " chase=" << (targetingConfig.chaseEnabled ? "true" : "false")
                              << " chaseDist=" << targetingConfig.chaseDistance
                              << " chaseScan=" << targetingConfig.chaseScanRange
                              << " repath=" << targetingConfig.chaseRepathMs << "ms"
                              << " manualPause=" << targetingConfig.manualInputPauseMs << "ms"
                              << " poll=" << targetingConfig.pollMs << "ms\n";
                } catch (const std::exception& e) {
                    std::cout << "[PAGEDOWN] ERRO: " << e.what() << "\n";
                }
            }

            if (t >= nextFishingReloadMs) {
                nextFishingReloadMs = t + 750;
                try {
                    const auto fresh = loadFishingConfig(fishingPath);
                    const bool changed = fresh.enabled != fishingConfig.enabled || fresh.rodId != fishingConfig.rodId || fresh.waterIds != fishingConfig.waterIds || fresh.range != fishingConfig.range || fresh.minDelayMs != fishingConfig.minDelayMs || fresh.maxDelayMs != fishingConfig.maxDelayMs;
                    fishingConfig = fresh;
                    if (changed) std::cout << "[FISH CFG] reloaded enabled=" << (fishingConfig.enabled?"true":"false") << " rod=" << fishingConfig.rodId << " waterIds=" << fishingConfig.waterIds.size() << " range=" << fishingConfig.range << "\n";
                } catch(...) {}
            }
            if (t >= nextToolsReloadMs) {
                nextToolsReloadMs = t + 1000;
                try { toolsConfig = loadToolsConfig(toolsPath); } catch(...) {}
            }
            if (const auto self = actions.localPlayerSnapshot()) {
                if (toolsConfig.paraEnabled && crome::hasCondition(self->states,crome::Condition::Paralyzed) && t >= nextParaMs) {
                    if (actions.sayDefault(toolsConfig.paraSpell)) { nextParaMs=t+1200; std::cout << "[TOOLS] Anti-Paralyze -> " << toolsConfig.paraSpell << "\n"; }
                }
                if (toolsConfig.manaEnabled && !crome::hasCondition(self->states,crome::Condition::ManaShield) && t >= nextManaMs) {
                    if (actions.sayDefault(toolsConfig.manaSpell)) { nextManaMs=t+toolsConfig.manaIntervalMs; std::cout << "[TOOLS] Mana Shield -> " << toolsConfig.manaSpell << "\n"; }
                }
                if (toolsConfig.hasteEnabled && !crome::hasCondition(self->states,crome::Condition::Haste) && !crome::hasCondition(self->states,crome::Condition::Paralyzed) && t >= nextHasteMs) {
                    if (actions.sayDefault(toolsConfig.hasteSpell)) { nextHasteMs=t+toolsConfig.hasteIntervalMs; std::cout << "[TOOLS] Auto Haste -> " << toolsConfig.hasteSpell << "\n"; }
                }
            }
            if (toolsConfig.foodEnabled && t >= nextFoodMs && !toolsConfig.foodIds.empty()) {
                bool used=false;
                for(const auto id: toolsConfig.foodIds) { if(actions.use(crome::Item{id,0,1})) { used=true; std::cout << "[TOOLS] Food -> item " << id << "\n"; break; } }
                nextFoodMs=t+(used?toolsConfig.foodIntervalMs:2500);
            }

            if (fishingConfig.enabled && t >= nextFishingMs) {
                const bool blockedByWalker = fishingConfig.pauseWhileWalker && walker.running();
                const bool blockedByTarget = fishingConfig.pauseWhileTargeting && (targetingEnabled || targeter.hasLock());
                if (!blockedByWalker && !blockedByTarget) {
                    const crome::Item rod{fishingConfig.rodId,0,1};
                    const bool ok = actions.useWithNearestGroundIds(rod, fishingConfig.waterIds, fishingConfig.range);
                    const auto span = fishingConfig.maxDelayMs > fishingConfig.minDelayMs ? fishingConfig.maxDelayMs - fishingConfig.minDelayMs : 0;
                    const auto jitter = span ? static_cast<std::uint32_t>(GetTickCount64() % (static_cast<std::uint64_t>(span)+1)) : 0;
                    nextFishingMs = t + fishingConfig.minDelayMs + jitter;
                    if(ok) std::cout << "[FISH] auto request accepted; next=" << (fishingConfig.minDelayMs+jitter) << "ms\n";
                } else nextFishingMs = t + 250;
            }

            if (toolsConfig.changeGold.enabled && t >= nextChangeGoldMs) {
                if (!actions.itemActionPending() && !actions.manualInteractionActive())
                    actions.changeGold(toolsConfig.changeGold.itemIds);
                nextChangeGoldMs=t+toolsConfig.changeGold.intervalMs;
            }

            if (healingEnabled && t >= nextHealingPoll) {
                nextHealingPoll = t + healingConfig.pollMs;
                if (const auto self = actions.localPlayerSnapshot()) {
                    if (const auto exec = healer.tick(*self, healingConfig.rules, t, p)) {
                        std::cout << std::fixed << std::setprecision(1)
                                  << "[HEAL] rule=\"" << exec->name << "\""
                                  << " HP=" << exec->healthPercent << "%"
                                  << " MP=" << exec->mana
                                  << " -> SAY \"" << exec->spell << "\" accepted\n"
                                  << std::defaultfloat;
                    }
                }
            }

            if (targetingEnabled && t >= nextTargetingPoll) {
                nextTargetingPoll = t + targetingConfig.pollMs;
                bool hasCombatTarget = false;

                // V21.6 MANUAL INTERACTION GUARD:
                // A mouse click may open a corpse/container or issue another native client
                // action. While that action is being processed, do not scan/reassert/chase.
                // This prevents our autoWalk from racing a manual loot/open operation.
                if (actions.manualInteractionActive()) {
                    hasCombatTarget = true; // keep route paused during the short manual window
                    if (!manualInteractionWasActive) {
                        std::cout << "[TARGET] Interacao manual detectada; targeting/chase pausado por "
                                  << actions.manualInteractionRemainingMs() << "ms.\n";
                    }
                    manualInteractionWasActive = true;
                    targetMissingSince = 0;
                    lastAttackReassert = t;
                } else {
                    if (manualInteractionWasActive) {
                        std::cout << "[TARGET] Janela de interacao manual encerrada; targeting/chase retomado.\n";
                    }
                    manualInteractionWasActive = false;

                    // V21.6 FOLLOW BLOCKER: Game::follow itself is neutralized while
                    // Targeting is ON. We do not cooperate with FollowTarget. This read
                    // is only a fail-safe: if FollowTarget somehow becomes active anyway,
                    // disable bot combat instead of racing the client.
                    if (actions.currentFollowTargetAddress() != 0) {
                        std::cout << "[TARGET] SAFETY: FollowTarget ativo apesar do blocker; desligando Targeting.\n";
                        targetingEnabled = false;
                        actions.setFollowBlocked(false);
                        targeter.reset();
                        targetMissingSince = 0;
                        lastAttackReassert = 0;
                        lastChaseMs = 0;
                        lastChaseProgressMs = 0;
                        lastChaseRemaining = 9999;
                        haveLastChaseDestination = false;
                    } else {
                    std::vector<crome::CreatureSnapshot> maintenance;
                    crome::Position selfNow = actions.currentPosition();

                    // V21.4 SAFE LOCK + CHASE:
                    // All target state comes from a fresh game-thread scan. We never
                    // dereference the old Creature* on the worker thread.
                    if (targeter.hasLock()) {
                        const auto locked = targeter.lockedAddress();
                        const auto lockedId = targeter.lockedId();
                        const auto clientAttack = actions.currentAttackTargetAddress();
                        maintenance = actions.visibleMonsters(targetingConfig.chaseScanRange);
                        selfNow = actions.currentPosition();

                        const auto it = std::find_if(maintenance.begin(), maintenance.end(),
                            [&](const crome::CreatureSnapshot& c) {
                                return c.address == locked && c.id == lockedId &&
                                       c.alive && c.hpPercent > 0 && c.position.z == selfNow.z;
                            });
                        const bool lockVisible = it != maintenance.end();

                        if (clientAttack != 0 && clientAttack != locked) {
                            // User/client selected another attack target. Release only our
                            // bookkeeping; do not touch/dereference the previous pointer.
                            std::cout << "[TARGET] Olders mudou AttackTarget; liberando lock antigo.\n";
                            targeter.reset();
                            targetMissingSince = 0;
                            lastAttackReassert = 0;
                            lastChaseMs = 0;
                            lastChaseProgressMs = 0;
                            lastChaseRemaining = 9999;
                            haveLastChaseDestination = false;
                        } else if (lockVisible) {
                            hasCombatTarget = true;
                            targetMissingSince = 0;

                            if (clientAttack == 0 && t - lastAttackReassert >= kAttackReassertMs) {
                                std::cout << "[TARGET] AttackTarget NULL mas alvo segue visivel; reafirmando Game::attack.\n";
                                if (actions.attackAddress(locked, lockedId)) lastAttackReassert = t;
                            }

                            if (targetingConfig.chaseEnabled) {
                                const int dist = selfNow.chebyshevDistance(it->position);
                                const auto modeDestination = targetPositionDestination(selfNow, it->position,
                                                                                       targetingConfig.movementMode);
                                const bool positioned = targetingConfig.movementMode >= 0
                                    ? !modeDestination.has_value() : dist <= targetingConfig.chaseDistance;
                                if (positioned) {
                                    haveLastChaseDestination = false;
                                    lastChaseRemaining = 9999;
                                    lastChaseProgressMs = t;
                                } else if (!actions.walkPending() && !actions.manualInteractionActive()) {
                                    const auto dest = modeDestination.value_or(chaseDestination(selfNow, it->position,
                                                                       targetingConfig.chaseDistance));
                                    if (dest.z == selfNow.z && dest.x > 0 && dest.y > 0) {
                                        const int remaining = haveLastChaseDestination
                                            ? selfNow.chebyshevDistance(lastChaseDestination) : 9999;
                                        const int destShift = haveLastChaseDestination
                                            ? lastChaseDestination.chebyshevDistance(dest) : 9999;

                                        if (haveLastChaseDestination && remaining < lastChaseRemaining) {
                                            lastChaseRemaining = remaining;
                                            lastChaseProgressMs = t;
                                        }

                                        const bool first = !haveLastChaseDestination;
                                        const bool reachedOld = haveLastChaseDestination && remaining <= 1;
                                        const bool targetMovedEnough = haveLastChaseDestination &&
                                            destShift >= (targetingConfig.movementMode >= 0 ? 1 : 2) && t - lastChaseMs >= targetingConfig.chaseRepathMs;
                                        const bool stalePath = haveLastChaseDestination &&
                                            t - lastChaseProgressMs >= targetingConfig.chaseForceRefreshMs;
                                        const bool intervalReady = t - lastChaseMs >= targetingConfig.chaseIntervalMs;

                                        if (intervalReady && (first || reachedOld || targetMovedEnough || stalePath)) {
                                            const bool accepted = actions.walkTo(dest);
                                            if (accepted) {
                                                lastChaseMs = t;
                                                lastChaseProgressMs = t;
                                                lastChaseDestination = dest;
                                                lastChaseRemaining = selfNow.chebyshevDistance(dest);
                                                haveLastChaseDestination = true;
                                                std::cout << "[TARGET/CHASE] name=\"" << it->name
                                                          << "\" dist=" << dist
                                                          << " -> " << dest.x << "," << dest.y << "," << dest.z
                                                          << " accepted"
                                                          << (stalePath ? " [refresh]" : "") << "\n";
                                            }
                                        }
                                    }
                                }
                            }
                        } else {
                            // Target not present in a fresh same-floor scan. Give the client
                            // a short grace window before releasing; never read the old ptr.
                            if (!targetMissingSince) {
                                targetMissingSince = t;
                                hasCombatTarget = true;
                                std::cout << "[TARGET] Alvo saiu do scan; aguardando grace period.\n";
                            } else if (t - targetMissingSince < kTargetMissingGraceMs) {
                                hasCombatTarget = true;
                            } else {
                                std::cout << "[TARGET] Alvo morreu/desapareceu; liberando lock.\n";
                                targeter.reset();
                                targetMissingSince = 0;
                                lastAttackReassert = 0;
                                lastChaseMs = 0;
                                lastChaseProgressMs = 0;
                                lastChaseRemaining = 9999;
                                haveLastChaseDestination = false;
                            }
                        }
                    }

                    // Acquire only when there is no lock. Acquisition remains bounded by
                    // max_range; chase may then keep a living target out to chase_scan_range.
                    if (!targeter.hasLock()) {
                        const auto monsters = actions.visibleMonsters(targetingConfig.maxRange);
                        const auto acquireSelf = actions.currentPosition();
                        auto update = targeter.tick(acquireSelf, monsters, targetingConfig);
                        if (update.hasTarget && update.acquired) {
                            std::cout << "[TARGET] LOCK name=\"" << update.target.name << "\""
                                      << " id=" << update.target.id
                                      << " hp=" << update.target.hpPercent << "%"
                                      << " pos=" << update.target.position.x << ","
                                      << update.target.position.y << "," << update.target.position.z
                                      << " dist=" << update.distance
                                      << " priority=" << update.priority << "\n";
                            const bool accepted = actions.attackAddress(update.target.address, update.target.id);
                            std::cout << "[TARGET] Game::attack request="
                                      << (accepted ? "accepted" : "failed") << "\n";
                            targetMissingSince = 0;
                            lastAttackReassert = t;
                            lastChaseMs = 0;
                            lastChaseProgressMs = 0;
                            lastChaseRemaining = 9999;
                            haveLastChaseDestination = false;
                            if (accepted) {
                                hasCombatTarget = true;
                            } else {
                                targeter.reset();
                            }
                        }
                    } else {
                        hasCombatTarget = true;
                    }
                }
                } // !manualInteractionActive

                if (hasCombatTarget && targetingConfig.pauseWalker && !walkerPausedByTarget) {
                    resumeWalkerAfterTarget = walker.running() || resumeWalkerAfterLoot;
                    walkerPausedByLoot=false;resumeWalkerAfterLoot=false;
                    if (walker.running()) walker.pause();
                    walkerPausedByTarget = true;
                    std::cout << "[TARGET] Walker pausado durante combate/chase.\n";
                }

                if (!hasCombatTarget && !targeter.hasLock() && walkerPausedByTarget) {
                    if (resumeWalkerAfterTarget && !walker.running()) {
                        walker.start(t, p);
                        std::cout << "[TARGET] Sem alvo. Walker retomado.\n";
                    }
                    walkerPausedByTarget = false;
                    resumeWalkerAfterTarget = false;
                }
            }

            if(t>=nextLootReloadMs){
                nextLootReloadMs=t+750;
                try {lootConfig=crome::parseOldersLoot(crome::json::parse(crome::readRouteText(lootPath)));}
                catch(...) {lootConfig={};}
            }
            if(t>=nextLootPollMs && !targeter.hasLock() && !walkerPausedByTarget &&
               !manualFollowActive && !actions.currentAttackTargetAddress() && !actions.manualInteractionActive() && !actions.walkPending()){
                nextLootPollMs=t+600;
                const auto loot=actions.lootStep(lootConfig);
                if(loot.status!=lastLootStatus){std::cout<<"[LOOT] "<<loot.status<<"\n"<<std::flush;lastLootStatus=loot.status;}
                if(loot.active){
                    if(!walkerPausedByLoot){resumeWalkerAfterLoot=walker.running();walkerPausedByLoot=true;}
                    if(walker.running())walker.pause();
                    if(loot.walk)actions.walkTo(loot.position);
                }else if(walkerPausedByLoot){
                    if(resumeWalkerAfterLoot && route.enabled && !walker.running())walker.start(t,p);
                    walkerPausedByLoot=false;resumeWalkerAfterLoot=false;
                }
            }
            if(walkerPausedByLoot && walker.running())walker.pause();
            walker.tick(t, p);

            if (end && !oldEnd) g_running = false;
            oldF1=f1; oldF2=f2; oldF3=f3; oldF4=f4; oldF5=f5;
            oldF6=f6; oldF7=f7; oldF8=f8; oldF9=f9; oldF10=f10;
            oldF11=f11; oldF12=f12; oldHome=home; oldInsert=insertKey;
            oldDelete=deleteKey; oldPageDown=pageDown; oldEnd=end;
            Sleep(20);
        }

        pipeServer.stop();
        actions.uninstallDispatcher();
    } catch (const std::exception& e) {
        std::cout << "[-] Exception: " << e.what() << "\n";
    }

done:
    std::cout << "\n[UNLOAD] Encerrando Crome SDK V21.6...\n";
    Sleep(150);
    std::cout.flush();std::cerr.flush();
    std::cout.rdbuf(priorOut);std::cerr.rdbuf(priorError);runtimeLog.close();
    FreeLibraryAndExitThread(g_module, 0);
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = hModule;
        DisableThreadLibraryCalls(hModule);
        const HANDLE h = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    }
    return TRUE;
}
