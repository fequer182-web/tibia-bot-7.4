#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <dwmapi.h>
#include <commdlg.h>
#include <gl/GL.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include <filesystem>
#include <memory>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <cctype>
#include <cwctype>
#include <optional>
#include <thread>
#include <mutex>
#include <atomic>
#include <nlohmann/json.hpp>

#include "core/runtime/ClientSession.hpp"
#include "core/readers/ProfileBackedPlayerReader.hpp"
#include "core/profile/Capabilities.hpp"
#include "modules/support/RamoniaSpearPickup.hpp"
#include "modules/cavebot/RamoniaToolTileCycle.hpp"
#include "native_runtime/runtime/OldersBaseline.hpp"
#include "native_runtime/runtime/OldersPipe.hpp"

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_opengl3.h"

#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "dwmapi.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static HWND g_hWnd = nullptr;
static HDC g_hDC = nullptr;
static HGLRC g_hRC = nullptr;
static bool g_running = true;
static bool g_dark = false;
enum class CompactPanel { None, Healing, Cavebot, Targeting, Loot, Hotkeys, Tools };
static CompactPanel g_compactPanel = CompactPanel::None;
static bool g_alwaysOnTop = true;
static bool g_widePanel = false;
static bool g_previewMode = false;
static std::string g_previewName;
static bool g_previewIds = false;
static bool g_largePopupOpen = false;
static float g_cardCanvasWidth = 0;
static int g_healingTab = 0;
static int g_toolsTab = 0;
static bool g_targetOnly = false;
static ImFont* g_font = nullptr;
static ImFont* g_fontBold = nullptr;
static ImFont* g_fontTitle = nullptr;

static std::unique_ptr<crome::ClientSession> g_session;
static std::unique_ptr<crome::ProfileBackedPlayerReader> g_playerReader;
static crome::PlayerSnapshot g_livePlayer;
static uint64_t g_lastPlayerRead = 0;
static std::filesystem::path g_baseDir;
static DWORD FindProcessByName(const wchar_t* exeName);

static bool g_nativeRuntimeLoaded = false;
static std::string g_nativeRuntimeStatus = "waiting";
static uint64_t g_lastNativeInjectAttempt = 0;
static DWORD g_nativeAttemptPid = 0;
static bool g_nativeAttemptedForPid = false;
static std::unordered_map<std::string,std::string> g_lastWrittenJson;

struct RamoniaMonsterUi {
    std::uint32_t id{};
    std::string name;
    std::uint32_t hpPercent{};
    std::uint32_t distance{};
};

struct RamoniaTargetConfigUi {
    bool enabled{};
    bool attackAny{};
    int hpMin{0};
    int hpMax{100};
    int maxRange{7};
    std::vector<std::string> names;
};

struct RamoniaFoodConfigUi {
    bool enabled{};
    std::vector<std::uint32_t> itemIds;
    std::uint32_t intervalMs{60000};
    std::uint32_t retryMs{1000};
};

struct RamoniaSpearConfigUi {
    bool enabled{};
    std::vector<std::uint32_t> itemIds{2389};
    std::uint32_t retryMs{250};
};

struct RamoniaLightConfigUi {
    bool enabled{};
};

struct RamoniaHealingSpellRuleUi {
    bool enabled{};
    int hpPercent{};
    double minMana{};
    std::string spell;
    std::uint32_t cooldownMs{850};
    int priority{};
};

struct RamoniaHealingItemRuleUi {
    bool enabled{};
    bool manaTrigger{};
    int triggerPercent{};
    std::uint32_t itemId{};
    std::uint32_t cooldownMs{650};
    int priority{};
    std::string name;
};

struct RamoniaHealingConfigUi {
    bool enabled{};
    std::vector<RamoniaHealingSpellRuleUi> spells;
    std::vector<RamoniaHealingItemRuleUi> items;
};

struct RamoniaRuneConfigUi {
    bool enabled{};
    int manaPercent{70};
    int soulMin{5};
    std::string spell{"exura"};
    std::uint32_t cooldownMs{1000};
};

struct RamoniaCaveNodeUi {
    std::uint32_t targetItemId{};
    std::string type;
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint32_t z{};
    std::uint32_t itemId{};
    std::uint32_t waitMs{};
    std::string text;
};

struct RamoniaCaveConfigUi {
    bool enabled{};
    bool loop{true};
    std::uint32_t stuckMs{3500};
    std::uint32_t retryDelayMs{650};
    int maxRetries{2};
    std::vector<RamoniaCaveNodeUi> nodes;
};

struct RamoniaVitalsUi {
    bool valid{};
    double health{};
    double maxHealth{};
    double mana{};
    double maxMana{};
    bool soulAvailable{};
    double soul{};

    double healthPercent() const {
        return maxHealth>0.0 ? (health*100.0/maxHealth) : 0.0;
    }

    double manaPercent() const {
        return maxMana>0.0 ? (mana*100.0/maxMana) : 0.0;
    }
};

struct RamoniaLootConfigUi {
    bool enabled{};
    bool onlyList{true};
    std::uint32_t corpseItemId{2813};
    std::uint32_t targetContainerId{0};
    std::vector<std::uint32_t> itemIds;
    std::vector<std::uint32_t> nestedContainerIds;
    std::vector<std::uint32_t> learnedCorpseIds;
};

struct RamoniaContainerItemUi {
    std::uint32_t slot{};
    std::uint32_t id{};
    std::uint32_t count{};
};

struct RamoniaContainerUi {
    std::uint32_t id{};
    std::string name;
    std::vector<RamoniaContainerItemUi> items;
};

struct RamoniaPositionUi {
    bool valid{};
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint32_t z{};
};

static std::atomic<bool> g_ramoniaWorkerRun{true};
static std::atomic<bool> g_ramoniaBridgeReady{false};
static std::atomic<bool> g_ramoniaWalkReady{false};
static std::atomic<bool> g_ramoniaSpearReady{false};
static std::atomic<bool> g_ramoniaToolTileReady{false};
static std::atomic<int> g_ramoniaBridgeVersion{0};
static std::atomic<DWORD> g_ramoniaPid{0};
static std::mutex g_ramoniaMutex;
static RamoniaTargetConfigUi g_ramoniaTargetConfig;
static RamoniaFoodConfigUi g_ramoniaFoodConfig;
static RamoniaSpearConfigUi g_ramoniaSpearConfig;
static RamoniaLightConfigUi g_ramoniaLightConfig;
static RamoniaHealingConfigUi g_ramoniaHealingConfig;
static RamoniaRuneConfigUi g_ramoniaRuneConfig;
static RamoniaCaveConfigUi g_ramoniaCaveConfig;
static RamoniaLootConfigUi g_ramoniaLootConfig;
static std::string g_ramoniaTargetStatus = "Ramonia bridge aguardando";
static std::string g_ramoniaFoodStatus = "Auto Food aguardando";
static std::string g_ramoniaSpearStatus = "Pick Up Spear aguardando";
static std::string g_ramoniaLightStatus = "Light aguardando";
static std::string g_ramoniaHealingStatus = "Healing aguardando";
static std::string g_ramoniaRuneStatus = "Trainer aguardando";
static std::string g_ramoniaCaveStatus = "Cavebot aguardando";
static std::string g_ramoniaLootStatus = "Loot aguardando";
static RamoniaPositionUi g_ramoniaPlayerPosition;
static std::string g_ramoniaCurrentTarget;
static std::uint32_t g_ramoniaCurrentTargetId{};
static std::uint32_t g_ramoniaCurrentTargetHp{};
static std::thread g_ramoniaWorker;

static std::string LowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return value;
}

static DWORD FindRamoniaProcess() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            std::wstring exe = pe.szExeFile;
            std::transform(exe.begin(), exe.end(), exe.begin(),
                [](wchar_t c){ return static_cast<wchar_t>(towlower(c)); });
            if (exe.rfind(L"ramonia_gl-", 0) == 0 &&
                exe.size() >= 4 && exe.substr(exe.size()-4) == L".exe") {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

static void SetRamoniaStatus(const std::string& status,
                             const std::string& target = {},
                             std::uint32_t id = 0,
                             std::uint32_t hp = 0) {
    std::lock_guard<std::mutex> lock(g_ramoniaMutex);
    g_ramoniaTargetStatus = status;
    g_ramoniaCurrentTarget = target;
    g_ramoniaCurrentTargetId = id;
    g_ramoniaCurrentTargetHp = hp;
}

static std::string RamoniaStatusText() {
    std::lock_guard<std::mutex> lock(g_ramoniaMutex);
    return g_ramoniaTargetStatus;
}

static void SetRamoniaTargetConfig(const RamoniaTargetConfigUi& cfg) {
    std::lock_guard<std::mutex> lock(g_ramoniaMutex);
    g_ramoniaTargetConfig = cfg;
}

static RamoniaTargetConfigUi GetRamoniaTargetConfig() {
    std::lock_guard<std::mutex> lock(g_ramoniaMutex);
    return g_ramoniaTargetConfig;
}

static void SetRamoniaFoodConfig(const RamoniaFoodConfigUi& cfg) {
    std::lock_guard<std::mutex> lock(g_ramoniaMutex);
    g_ramoniaFoodConfig = cfg;
}

static RamoniaFoodConfigUi GetRamoniaFoodConfig() {
    std::lock_guard<std::mutex> lock(g_ramoniaMutex);
    return g_ramoniaFoodConfig;
}

static void SetRamoniaFoodStatus(const std::string& status) {
    std::lock_guard<std::mutex> lock(g_ramoniaMutex);
    g_ramoniaFoodStatus = status;
}

static std::string RamoniaFoodStatusText() {
    std::lock_guard<std::mutex> lock(g_ramoniaMutex);
    return g_ramoniaFoodStatus;
}

static void SetRamoniaSpearConfig(const RamoniaSpearConfigUi& cfg) {
    std::lock_guard<std::mutex> lock(g_ramoniaMutex);
    g_ramoniaSpearConfig=cfg;
}

static RamoniaSpearConfigUi GetRamoniaSpearConfig() {
    std::lock_guard<std::mutex> lock(g_ramoniaMutex);
    return g_ramoniaSpearConfig;
}

static void SetRamoniaSpearStatus(const std::string& status) {
    std::lock_guard<std::mutex> lock(g_ramoniaMutex);
    g_ramoniaSpearStatus=status;
}

static std::string RamoniaSpearStatusText() {
    std::lock_guard<std::mutex> lock(g_ramoniaMutex);
    return g_ramoniaSpearStatus;
}

static void SetRamoniaLightConfig(const RamoniaLightConfigUi& cfg) {
    std::lock_guard<std::mutex> lock(g_ramoniaMutex);
    g_ramoniaLightConfig=cfg;
}

static RamoniaLightConfigUi GetRamoniaLightConfig() {
    std::lock_guard<std::mutex> lock(g_ramoniaMutex);
    return g_ramoniaLightConfig;
}

static void SetRamoniaLightStatus(const std::string& status) {
    std::lock_guard<std::mutex> lock(g_ramoniaMutex);
    g_ramoniaLightStatus=status;
}

static std::string RamoniaLightStatusText() {
    std::lock_guard<std::mutex> lock(g_ramoniaMutex);
    return g_ramoniaLightStatus;
}

static void SetRamoniaHealingConfig(const RamoniaHealingConfigUi& cfg) {
    std::lock_guard<std::mutex> lock(g_ramoniaMutex);
    g_ramoniaHealingConfig=cfg;
}

static RamoniaHealingConfigUi GetRamoniaHealingConfig() {
    std::lock_guard<std::mutex> lock(g_ramoniaMutex);
    return g_ramoniaHealingConfig;
}

static void SetRamoniaHealingStatus(const std::string& status) {
    std::lock_guard<std::mutex> lock(g_ramoniaMutex);
    g_ramoniaHealingStatus=status;
}

static std::string RamoniaHealingStatusText() {
    std::lock_guard<std::mutex> lock(g_ramoniaMutex);
    return g_ramoniaHealingStatus;
}

static void SetRamoniaRuneConfig(const RamoniaRuneConfigUi& cfg) {
    std::lock_guard<std::mutex> lock(g_ramoniaMutex);
    g_ramoniaRuneConfig=cfg;
}

static RamoniaRuneConfigUi GetRamoniaRuneConfig() {
    std::lock_guard<std::mutex> lock(g_ramoniaMutex);
    return g_ramoniaRuneConfig;
}

static void SetRamoniaRuneStatus(const std::string& status) {
    std::lock_guard<std::mutex> lock(g_ramoniaMutex);
    g_ramoniaRuneStatus=status;
}

static std::string RamoniaRuneStatusText() {
    std::lock_guard<std::mutex> lock(g_ramoniaMutex);
    return g_ramoniaRuneStatus;
}

static void SetRamoniaCaveConfig(const RamoniaCaveConfigUi& cfg) {
    std::lock_guard<std::mutex> lock(g_ramoniaMutex);
    g_ramoniaCaveConfig=cfg;
}

static RamoniaCaveConfigUi GetRamoniaCaveConfig() {
    std::lock_guard<std::mutex> lock(g_ramoniaMutex);
    return g_ramoniaCaveConfig;
}

static void SetRamoniaCaveStatus(const std::string& status) {
    std::lock_guard<std::mutex> lock(g_ramoniaMutex);
    g_ramoniaCaveStatus=status;
}

static std::string RamoniaCaveStatusText() {
    std::lock_guard<std::mutex> lock(g_ramoniaMutex);
    return g_ramoniaCaveStatus;
}

static RamoniaVitalsUi ParseRamoniaVitals(const nlohmann::json& j) {
    RamoniaVitalsUi out;
    if (!j.is_object() || !j.value("ok",false) ||
        !j.contains("player") || !j["player"].is_object())
        return out;

    const auto& p=j["player"];
    out.health=p.value("health",0.0);
    out.maxHealth=p.value("maxHealth",0.0);
    out.mana=p.value("mana",0.0);
    out.maxMana=p.value("maxMana",0.0);
    out.soulAvailable=p.value("soulAvailable",false);
    out.soul=out.soulAvailable ? p.value("soul",0.0) : 0.0;
    out.valid=out.maxHealth>0.0 && out.maxMana>=0.0 &&
              out.health>=0.0 && out.mana>=0.0;
    return out;
}

static void SetRamoniaLootConfig(const RamoniaLootConfigUi& cfg) {
    std::lock_guard<std::mutex> lock(g_ramoniaMutex);
    auto merged=cfg;
    for(const auto id:g_ramoniaLootConfig.learnedCorpseIds) {
        if(id && std::find(merged.learnedCorpseIds.begin(),
                           merged.learnedCorpseIds.end(),id)==merged.learnedCorpseIds.end())
            merged.learnedCorpseIds.push_back(id);
    }
    g_ramoniaLootConfig=std::move(merged);
}

static RamoniaLootConfigUi GetRamoniaLootConfig() {
    std::lock_guard<std::mutex> lock(g_ramoniaMutex);
    return g_ramoniaLootConfig;
}

static void LearnRamoniaCorpseId(std::uint32_t id) {
    if(!id) return;
    std::lock_guard<std::mutex> lock(g_ramoniaMutex);
    auto& ids=g_ramoniaLootConfig.learnedCorpseIds;
    if(std::find(ids.begin(),ids.end(),id)==ids.end())
        ids.push_back(id);
}

static void SetRamoniaLootStatus(const std::string& status) {
    std::lock_guard<std::mutex> lock(g_ramoniaMutex);
    g_ramoniaLootStatus = status;
}

static std::string RamoniaLootStatusText() {
    std::lock_guard<std::mutex> lock(g_ramoniaMutex);
    return g_ramoniaLootStatus;
}

static void SetRamoniaPlayerPosition(const RamoniaPositionUi& pos) {
    std::lock_guard<std::mutex> lock(g_ramoniaMutex);
    g_ramoniaPlayerPosition=pos;
}

static RamoniaPositionUi GetRamoniaPlayerPosition() {
    std::lock_guard<std::mutex> lock(g_ramoniaMutex);
    return g_ramoniaPlayerPosition;
}

static bool RamoniaPipeRequest(DWORD pid, const nlohmann::json& request, nlohmann::json& response) {
    const std::string pipeName = "\\\\.\\pipe\\Crome.Ramonia." + std::to_string(pid);
    HANDLE pipe = INVALID_HANDLE_VALUE;
    DWORD lastError = ERROR_SUCCESS;

    for (int attempt = 0; attempt < 20; ++attempt) {
        pipe = CreateFileA(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE,
                           0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;
        lastError = GetLastError();
        if (lastError != ERROR_PIPE_BUSY && lastError != ERROR_FILE_NOT_FOUND) break;
        if (lastError == ERROR_PIPE_BUSY) WaitNamedPipeA(pipeName.c_str(), 100);
        Sleep(25);
    }
    if (pipe == INVALID_HANDLE_VALUE) return false;

    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

    const std::string payload = request.dump();
    DWORD written{};
    if (!WriteFile(pipe, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr)) {
        CloseHandle(pipe);
        return false;
    }

    char buffer[16384]{};
    DWORD read{};
    const BOOL ok = ReadFile(pipe, buffer, sizeof(buffer)-1, &read, nullptr);
    if (!ok && GetLastError() != ERROR_MORE_DATA) {
        CloseHandle(pipe);
        return false;
    }
    CloseHandle(pipe);

    try {
        response = nlohmann::json::parse(std::string(buffer, buffer + read));
        return true;
    } catch (...) {
        return false;
    }
}

static std::vector<RamoniaMonsterUi> ParseRamoniaMonsters(const nlohmann::json& j) {
    std::vector<RamoniaMonsterUi> out;
    if (!j.value("ok", false) || !j.contains("monsters") || !j["monsters"].is_array()) return out;
    for (const auto& v : j["monsters"]) {
        if (!v.is_object()) continue;
        RamoniaMonsterUi m;
        m.id = v.value("id", 0u);
        m.name = v.value("name", std::string());
        m.hpPercent = v.value("hpPercent", 0u);
        m.distance = v.value("distance", 999u);
        if (m.id && !m.name.empty() && m.hpPercent <= 100u) out.push_back(std::move(m));
    }
    return out;
}

static std::uint32_t ParseRamoniaAttackId(const nlohmann::json& j) {
    if (!j.value("ok", false) || !j.contains("attack") || !j["attack"].is_object()) return 0;
    return j["attack"].value("id", 0u);
}

static bool RamoniaNameMatches(const RamoniaMonsterUi& m, const RamoniaTargetConfigUi& cfg, int& priority) {
    priority = 1000000;
    if (cfg.attackAny) { priority = 0; return true; }
    const auto monsterName = LowerAscii(m.name);
    for (std::size_t i=0; i<cfg.names.size(); ++i) {
        const auto rule = LowerAscii(cfg.names[i]);
        if (rule.empty()) continue;
        if (rule == "*" || rule == monsterName) {
            priority = static_cast<int>(i);
            return true;
        }
    }
    return false;
}

static bool HasRamoniaTargetPolicy(const RamoniaTargetConfigUi& cfg) {
    if (cfg.attackAny) return true;
    for (const auto& raw : cfg.names) {
        const auto name = LowerAscii(raw);
        if (!name.empty()) return true;
    }
    return false;
}

static std::optional<RamoniaMonsterUi> ChooseRamoniaTarget(
    const std::vector<RamoniaMonsterUi>& monsters,
    const RamoniaTargetConfigUi& cfg,
    std::uint32_t currentAttackId) {

    // Sticky target rule: once the client has a real AttackTarget, never
    // abandon it merely because one Tile::things poll missed the creature or
    // because HP/range changed. Let the game clear AttackTarget on death,
    // cancel or true disappearance before acquiring another monster.
    if (currentAttackId) {
        for (const auto& m : monsters) {
            if (m.id == currentAttackId)
                return m;
        }
        return std::nullopt;
    }

    std::optional<RamoniaMonsterUi> best;
    int bestPriority = 1000000;
    for (const auto& m : monsters) {
        int pr{};
        if (m.hpPercent == 0) continue;
        if (!RamoniaNameMatches(m, cfg, pr)) continue;
        if (static_cast<int>(m.hpPercent) < cfg.hpMin || static_cast<int>(m.hpPercent) > cfg.hpMax) continue;
        if (static_cast<int>(m.distance) > cfg.maxRange) continue;
        if (!best || pr < bestPriority || (pr == bestPriority && m.distance < best->distance)) {
            best = m;
            bestPriority = pr;
        }
    }
    return best;
}


static RamoniaPositionUi ParseRamoniaPlayerPosition(const nlohmann::json& j) {
    RamoniaPositionUi out;
    if (!j.is_object() || !j.value("ok",false) ||
        !j.contains("player") || !j["player"].is_object())
        return out;

    const auto& player=j["player"];
    if (!player.contains("position") || !player["position"].is_object())
        return out;

    const auto& p=player["position"];
    if (!p.contains("x") || !p.contains("y") || !p.contains("z"))
        return out;

    out.x=p.value("x",0u);
    out.y=p.value("y",0u);
    out.z=p.value("z",0u);
    out.valid=out.x!=0u && out.y!=0u;
    return out;
}

static std::vector<RamoniaContainerUi> ParseRamoniaContainers(const nlohmann::json& j) {
    std::vector<RamoniaContainerUi> out;
    if (!j.is_object() || !j.value("ok",false) ||
        !j.contains("containers") || !j["containers"].is_array())
        return out;

    for (const auto& cv : j["containers"]) {
        if (!cv.is_object()) continue;
        RamoniaContainerUi c;
        c.id = cv.value("id",0u);
        c.name = cv.value("name",std::string());
        if (cv.contains("items") && cv["items"].is_array()) {
            for (const auto& iv : cv["items"]) {
                if (!iv.is_object()) continue;
                RamoniaContainerItemUi item;
                item.slot = iv.value("slot",0u);
                item.id = iv.value("id",0u);
                item.count = iv.value("count",1u);
                if (item.id) c.items.push_back(item);
            }
        }
        out.push_back(std::move(c));
    }
    return out;
}

struct RamoniaLookCaptureUi {
    bool armed{};
    bool busy{};
    bool captured{};
    std::uint32_t id{};
    std::uint64_t nextPollMs{};
    std::uint64_t nextArmMs{};
};
static RamoniaLookCaptureUi g_ramoniaLookCapture;
static std::atomic<std::uint32_t> g_ramoniaPassiveLookId{0};

static void UpdateRamoniaPassiveLookId() {
    const DWORD pid=g_ramoniaPid.load();
    const int bridgeVersion=g_ramoniaBridgeVersion.load();
    if (!pid || !g_ramoniaBridgeReady.load() || bridgeVersion<25)
        return;

    const auto now=static_cast<std::uint64_t>(GetTickCount64());

    // Poll the current one-shot state at a low rate.
    if (!g_ramoniaLookCapture.nextPollMs ||
        now>=g_ramoniaLookCapture.nextPollMs) {

        nlohmann::json response;
        if (RamoniaPipeRequest(pid,{{"cmd","GET_LOOK_ID"}},response) &&
            response.value("ok",false)) {

            g_ramoniaLookCapture.armed=response.value("armed",false);
            g_ramoniaLookCapture.busy=response.value("busy",false);
            g_ramoniaLookCapture.captured=response.value("captured",false);

            if (g_ramoniaLookCapture.captured) {
                const auto id=response.value("id",0u);
                if (id) {
                    g_ramoniaLookCapture.id=id;

                    g_ramoniaPassiveLookId.store(id);
                }
            }

            // When the previous capture finished or timed out, re-arm shortly
            // afterward. No UI action is required from the user.
            if (!g_ramoniaLookCapture.armed &&
                !g_ramoniaLookCapture.busy &&
                now>=g_ramoniaLookCapture.nextArmMs) {

                nlohmann::json arm;
                if (RamoniaPipeRequest(pid,{{"cmd","ARM_LOOK_ID"}},arm) &&
                    arm.value("ok",false)) {
                    g_ramoniaLookCapture.armed=true;
                    g_ramoniaLookCapture.busy=true;
                    g_ramoniaLookCapture.nextArmMs=now+500;
                } else {
                    g_ramoniaLookCapture.nextArmMs=now+500;
                }
            }
        }

        g_ramoniaLookCapture.nextPollMs=now+150;
    }
}


static bool RamoniaContainerIdExists(const std::vector<RamoniaContainerUi>& containers,
                                     std::uint32_t id) {
    for (const auto& c : containers) if (c.id == id) return true;
    return false;
}

static const RamoniaContainerUi* FindRamoniaContainer(
    const std::vector<RamoniaContainerUi>& containers,
    std::uint32_t id) {
    for (const auto& c : containers) if (c.id == id) return &c;
    return nullptr;
}

static bool RamoniaLootMarkTracksTarget(const nlohmann::json& mark,
                                        std::uint32_t targetId) {
    if (!mark.is_object() || !mark.value("ok",false) ||
        !mark.contains("trackedMonsters") || !mark["trackedMonsters"].is_array())
        return false;
    for (const auto& m : mark["trackedMonsters"])
        if (m.is_object() && m.value("id",0u) == targetId) return true;
    return false;
}

static std::vector<std::uint32_t> RamoniaLootCorpseCandidates(
    const nlohmann::json& diff,
    std::uint32_t targetId,
    const std::vector<std::uint32_t>& preferredCorpseIds) {

    std::vector<std::uint32_t> out;
    if (!diff.is_object() || !diff.value("ok",false)) return out;
    if (diff.value("trackedTargetId",0u) != targetId) return out;

    bool targetRemoved=false;
    if (diff.contains("removedMonsters") && diff["removedMonsters"].is_array()) {
        for (const auto& m:diff["removedMonsters"]) {
            if (m.is_object() && m.value("id",0u)==targetId) {
                targetRemoved=true;
                break;
            }
        }
    }
    if (!targetRemoved) return out;

    auto pushUnique=[](std::vector<std::uint32_t>& dst,std::uint32_t id){
        if (!id) return;
        if (std::find(dst.begin(),dst.end(),id)==dst.end()) dst.push_back(id);
    };

    std::vector<std::uint32_t> strictIds;
    if (diff.contains("addedItems") && diff["addedItems"].is_array()) {
        for (const auto& item:diff["addedItems"]) {
            if (!item.is_object()) continue;
            const auto id=item.value("id",0u);
            if (id) pushUnique(strictIds,id);
        }
    }

    for (const auto knownId:preferredCorpseIds) {
        if (std::find(strictIds.begin(),strictIds.end(),knownId)!=strictIds.end())
            pushUnique(out,knownId);
    }
    for (const auto id:strictIds) pushUnique(out,id);
    return out;
}

static bool SameRamoniaContainerSnapshot(
    const RamoniaContainerUi& a,
    const RamoniaContainerUi& b) {

    if (a.name!=b.name || a.items.size()!=b.items.size()) return false;
    for (std::size_t i=0;i<a.items.size();++i) {
        if (a.items[i].id!=b.items[i].id ||
            a.items[i].count!=b.items[i].count)
            return false;
    }
    return true;
}

static bool RamoniaIsNestedContainerId(
    const RamoniaLootConfigUi& cfg,
    std::uint32_t id) {
    return id &&
           std::find(
               cfg.nestedContainerIds.begin(),
               cfg.nestedContainerIds.end(),
               id) != cfg.nestedContainerIds.end();
}

static std::uint32_t RamoniaMoveLootFromContainer(
    DWORD pid,
    const RamoniaLootConfigUi& cfg,
    std::uint32_t sourceId) {

    nlohmann::json initialJson;
    if (!RamoniaPipeRequest(pid,{{"cmd","GET_OPEN_CONTAINERS"}},initialJson))
        return 0;

    auto containers=ParseRamoniaContainers(initialJson);
    const auto* source=FindRamoniaContainer(containers,sourceId);
    if (!source) return 0;

    std::vector<std::uint32_t> wanted;
    if (cfg.onlyList) {
        for (const auto id:cfg.itemIds) {
            if (!id || RamoniaIsNestedContainerId(cfg,id)) continue;
            if (std::find(wanted.begin(),wanted.end(),id)==wanted.end())
                wanted.push_back(id);
        }
    } else {
        for (const auto& item:source->items) {
            if (!item.id || RamoniaIsNestedContainerId(cfg,item.id)) continue;
            if (std::find(wanted.begin(),wanted.end(),item.id)==wanted.end())
                wanted.push_back(item.id);
        }
    }

    std::uint32_t movedActions=0;
    for (const auto itemId:wanted) {
        for (int repeat=0;repeat<32;++repeat) {
            nlohmann::json currentJson;
            if (!RamoniaPipeRequest(pid,{{"cmd","GET_OPEN_CONTAINERS"}},currentJson))
                break;

            containers=ParseRamoniaContainers(currentJson);
            const auto* currentSource=FindRamoniaContainer(containers,sourceId);
            if (!currentSource) break;

            bool found=false;
            for (const auto& item:currentSource->items)
                if (item.id==itemId) {
                    found=true;
                    break;
                }
            if (!found) break;

            nlohmann::json moveResponse;
            if (!RamoniaPipeRequest(
                    pid,
                    {{"cmd","MOVE_CONTAINER_ITEM"},
                     {"sourceContainerId",sourceId},
                     {"targetContainerId",cfg.targetContainerId},
                     {"itemId",itemId}},
                    moveResponse) ||
                !moveResponse.value("ok",false)) {
                const auto error=moveResponse.is_object()
                    ? moveResponse.value("error",std::string("move_failed"))
                    : std::string("move_failed");
                SetRamoniaLootStatus("Loot: move falhou - "+error);
                break;
            }

            ++movedActions;
            Sleep(80);
        }
    }

    return movedActions;
}

static bool RamoniaOpenNestedContainerItem(
    DWORD pid,
    const RamoniaLootConfigUi& cfg,
    std::uint32_t sourceContainerId,
    std::uint32_t itemId,
    std::uint32_t& openedContainerId) {

    openedContainerId=0;

    nlohmann::json beforeJson;
    if (!RamoniaPipeRequest(pid,{{"cmd","GET_OPEN_CONTAINERS"}},beforeJson))
        return false;
    const auto before=ParseRamoniaContainers(beforeJson);

    const auto* source=FindRamoniaContainer(before,sourceContainerId);
    if (!source) return false;

    bool itemPresent=false;
    for (const auto& item:source->items)
        if (item.id==itemId) {
            itemPresent=true;
            break;
        }
    if (!itemPresent) return false;

    nlohmann::json useResponse;
    if (!RamoniaPipeRequest(
            pid,
            {{"cmd","USE_CONTAINER_ITEM"},
             {"sourceContainerId",sourceContainerId},
             {"itemId",itemId}},
            useResponse) ||
        !useResponse.value("ok",false))
        return false;

    const ULONGLONG deadline=GetTickCount64()+900;
    while(GetTickCount64()<deadline) {
        Sleep(90);

        nlohmann::json afterJson;
        if (!RamoniaPipeRequest(pid,{{"cmd","GET_OPEN_CONTAINERS"}},afterJson))
            continue;
        const auto after=ParseRamoniaContainers(afterJson);

        // Prefer a brand-new container window.
        for (const auto& c:after) {
            if (c.id==cfg.targetContainerId) continue;
            if (!RamoniaContainerIdExists(before,c.id)) {
                openedContainerId=c.id;
                return true;
            }
        }

        // Some clients reuse an existing window/container id.
        for (const auto& c:after) {
            if (c.id==cfg.targetContainerId) continue;
            const auto* old=FindRamoniaContainer(before,c.id);
            if (old && !SameRamoniaContainerSnapshot(*old,c)) {
                openedContainerId=c.id;
                return true;
            }
        }
    }

    return false;
}

static bool RamoniaLootContainerRecursive(
    DWORD pid,
    const RamoniaLootConfigUi& cfg,
    std::uint32_t sourceId,
    int depth,
    std::uint32_t& totalMoved,
    std::uint32_t& nestedOpened) {

    if (sourceId==cfg.targetContainerId || depth>3)
        return true;

    totalMoved += RamoniaMoveLootFromContainer(pid,cfg,sourceId);

    if (depth==3 || cfg.nestedContainerIds.empty())
        return true;

    nlohmann::json currentJson;
    if (!RamoniaPipeRequest(pid,{{"cmd","GET_OPEN_CONTAINERS"}},currentJson))
        return false;
    const auto containers=ParseRamoniaContainers(currentJson);
    const auto* source=FindRamoniaContainer(containers,sourceId);
    if (!source) return false;

    std::vector<std::uint32_t> nestedToOpen;
    for (const auto& item:source->items) {
        if (!RamoniaIsNestedContainerId(cfg,item.id)) continue;
        if (std::find(nestedToOpen.begin(),nestedToOpen.end(),item.id)
            == nestedToOpen.end())
            nestedToOpen.push_back(item.id);
    }

    for (const auto nestedItemId:nestedToOpen) {
        std::uint32_t childId{};
        SetRamoniaLootStatus(
            "Loot: abrindo bag interna ID "+std::to_string(nestedItemId));

        if (!RamoniaOpenNestedContainerItem(
                pid,cfg,sourceId,nestedItemId,childId) ||
            !childId)
            continue;

        ++nestedOpened;
        SetRamoniaLootStatus(
            "Loot: bag interna "+std::to_string(nestedItemId)+
            " -> container "+std::to_string(childId));

        RamoniaLootContainerRecursive(
            pid,cfg,childId,depth+1,totalMoved,nestedOpened);
    }

    return true;
}

static bool RamoniaLootOpenAndMove(
    DWORD pid,
    const RamoniaLootConfigUi& cfg,
    std::uint32_t corpseItemId,
    std::uint32_t openWaitMs) {

    nlohmann::json beforeJson;
    if (!RamoniaPipeRequest(pid,{{"cmd","GET_OPEN_CONTAINERS"}},beforeJson))
        return false;
    const auto before=ParseRamoniaContainers(beforeJson);

    if (!RamoniaContainerIdExists(before,cfg.targetContainerId)) {
        SetRamoniaLootStatus(
            "Loot: abra a bag destino ID "+
            std::to_string(cfg.targetContainerId));
        return false;
    }

    nlohmann::json useResponse;
    if (!RamoniaPipeRequest(
            pid,
            {{"cmd","USE_LOOT_ITEM"},{"itemId",corpseItemId}},
            useResponse) ||
        !useResponse.value("ok",false)) {
        SetRamoniaLootStatus("Loot: falha ao abrir corpse");
        return false;
    }

    std::vector<RamoniaContainerUi> containers;
    std::optional<std::uint32_t> sourceIdOpt;

    const std::uint32_t budget=std::max<std::uint32_t>(180u,openWaitMs);
    const ULONGLONG openDeadline=GetTickCount64()+budget;
    int attempt=0;

    while(!sourceIdOpt && GetTickCount64()<openDeadline) {
        Sleep(attempt++==0 ? std::min<std::uint32_t>(180u,budget) : 70u);

        nlohmann::json afterJson;
        if (!RamoniaPipeRequest(pid,{{"cmd","GET_OPEN_CONTAINERS"}},afterJson))
            continue;
        containers=ParseRamoniaContainers(afterJson);

        for (const auto& c:containers) {
            if (c.id==cfg.targetContainerId) continue;
            if (!RamoniaContainerIdExists(before,c.id)) {
                sourceIdOpt=c.id;
                break;
            }
        }
        if (sourceIdOpt) break;

        for (const auto& c:containers) {
            if (c.id==cfg.targetContainerId) continue;
            const auto* old=FindRamoniaContainer(before,c.id);
            if (old && !SameRamoniaContainerSnapshot(*old,c)) {
                sourceIdOpt=c.id;
                break;
            }
        }
    }

    if (!sourceIdOpt) {
        SetRamoniaLootStatus(
            "Loot: corpse "+std::to_string(corpseItemId)+
            " ainda nao abriu");
        return false;
    }

    std::uint32_t totalMoved{};
    std::uint32_t nestedOpened{};
    RamoniaLootContainerRecursive(
        pid,cfg,*sourceIdOpt,0,totalMoved,nestedOpened);

    if (nestedOpened) {
        SetRamoniaLootStatus(
            "Loot: "+std::to_string(totalMoved)+
            " move(s), "+std::to_string(nestedOpened)+
            " bag(s) interna(s)");
    } else if (totalMoved) {
        SetRamoniaLootStatus(
            "Loot: movido para bag "+
            std::to_string(cfg.targetContainerId));
    } else {
        SetRamoniaLootStatus("Loot: corpse sem itens da lista");
    }

    // Opening the corpse itself was successful even if no configured item was
    // present. This keeps corpse learning correct.
    return true;
}

static std::vector<int> CatalogIds(const char* key);

struct RamoniaCaveWorkerState {
    std::size_t index{};
    std::uint64_t waitUntilMs{};
    std::uint64_t nextActionMs{};
    std::uint64_t lastProgressMs{};
    RamoniaPositionUi lastPosition{};
    bool walkIssued{};
    std::uint32_t issuedWalkX{};
    std::uint32_t issuedWalkY{};
    std::uint32_t issuedWalkZ{};
    bool transitionPending{};
    std::uint32_t transitionFromZ{};
    std::uint64_t transitionDeadlineMs{};
    int retries{};
    bool halted{};
};

static void ResetRamoniaCaveWorkerState(RamoniaCaveWorkerState& state) {
    state={};
}

static void AdvanceRamoniaCaveNode(
    const RamoniaCaveConfigUi& cfg,
    RamoniaCaveWorkerState& state) {

    ++state.index;
    if (state.index>=cfg.nodes.size() && cfg.loop)
        state.index=0;

    state.waitUntilMs=0;
    state.nextActionMs=0;
    state.lastProgressMs=0;
    state.lastPosition={};
    state.walkIssued=false;
    state.issuedWalkX=0;
    state.issuedWalkY=0;
    state.issuedWalkZ=0;
    state.transitionPending=false;
    state.transitionFromZ=0;
    state.transitionDeadlineMs=0;
    state.retries=0;
    state.halted=false;
}

static void RunRamoniaCaveCycle(
    DWORD pid,
    const RamoniaCaveConfigUi& cfg,
    RamoniaCaveWorkerState& state,
    std::uint64_t nowMs,
    bool pausedForCombat) {

    if (!cfg.enabled) {
        ResetRamoniaCaveWorkerState(state);
        SetRamoniaCaveStatus("Cavebot: desligado");
        return;
    }

    if (cfg.nodes.empty()) {
        ResetRamoniaCaveWorkerState(state);
        SetRamoniaCaveStatus("Cavebot: nenhum waypoint");
        return;
    }

    if (state.index>=cfg.nodes.size()) {
        if (cfg.loop)
            state.index=0;
        else {
            SetRamoniaCaveStatus("Cavebot: rota concluida");
            return;
        }
    }

    if (pausedForCombat) {
        SetRamoniaCaveStatus("Cavebot: pausado por Target/Loot");
        return;
    }

    if (state.halted) {
        SetRamoniaCaveStatus(
            "Cavebot: parado por excesso de retries #" +
            std::to_string(state.index));
        return;
    }

    const auto& node=cfg.nodes[state.index];

    if (node.type=="wait") {
        if (!state.waitUntilMs)
            state.waitUntilMs=nowMs+std::max<std::uint32_t>(1u,node.waitMs);

        if (nowMs>=state.waitUntilMs) {
            SetRamoniaCaveStatus(
                "Cavebot: wait concluido #" + std::to_string(state.index));
            AdvanceRamoniaCaveNode(cfg,state);
        } else {
            SetRamoniaCaveStatus(
                "Cavebot: wait #" + std::to_string(state.index));
        }
        return;
    }

    if (node.type=="say") {
        if (node.text.empty() || node.text.size()>15) {
            SetRamoniaCaveStatus(
                "Cavebot: Say invalido #" + std::to_string(state.index));
            return;
        }
        if (state.nextActionMs && nowMs<state.nextActionMs)
            return;

        nlohmann::json response;
        const bool ok=
            RamoniaPipeRequest(
                pid,
                {{"cmd","SAY"},{"message",node.text}},
                response) &&
            response.value("ok",false);

        if (ok) {
            SetRamoniaCaveStatus(
                "Cavebot: Say " + node.text);
            AdvanceRamoniaCaveNode(cfg,state);
        } else {
            SetRamoniaCaveStatus("Cavebot: Say pendente");
            state.nextActionMs=nowMs+500;
        }
        return;
    }

    const bool isWalkNode =
        node.type=="walk" || node.type=="stand";
    const bool isDirectUseNode =
        node.type=="use" ||
        node.type=="ladder" ||
        node.type=="use_with_ground";
    const bool isToolUseNode =
        node.type=="rope" ||
        node.type=="shovel";

    if (!isWalkNode && !isDirectUseNode && !isToolUseNode) {
        SetRamoniaCaveStatus(
            "Cavebot: tipo nao suportado " + node.type);
        return;
    }

    nlohmann::json playerJson;
    if (!RamoniaPipeRequest(pid,{{"cmd","GET_PLAYER"}},playerJson)) {
        SetRamoniaCaveStatus("Cavebot: aguardando player");
        return;
    }

    const auto pos=ParseRamoniaPlayerPosition(playerJson);
    if (!pos.valid) {
        SetRamoniaCaveStatus("Cavebot: posicao indisponivel");
        return;
    }

    // Structural map use can change floors asynchronously. Keep the current
    // node until GET_PLAYER proves the Z transition really happened.
    if (state.transitionPending) {
        if (pos.z!=state.transitionFromZ) {
            SetRamoniaCaveStatus(
                "Cavebot: transicao OK z=" +
                std::to_string(state.transitionFromZ) + "->" +
                std::to_string(pos.z) +
                " #" + std::to_string(state.index));
            AdvanceRamoniaCaveNode(cfg,state);
            return;
        }

        if (state.transitionDeadlineMs && nowMs>=state.transitionDeadlineMs) {
            state.transitionPending=false;
            state.transitionFromZ=0;
            state.transitionDeadlineMs=0;
            state.nextActionMs=nowMs+650;
            SetRamoniaCaveStatus(
                "Cavebot: transicao nao ocorreu; tentando use novamente #" +
                std::to_string(state.index));
        } else {
            SetRamoniaCaveStatus(
                "Cavebot: aguardando mudanca de andar #" +
                std::to_string(state.index));
            return;
        }
    }

    if (pos.z!=node.z) {
        SetRamoniaCaveStatus(
            "Cavebot: waypoint em outro andar #" +
            std::to_string(state.index));
        return;
    }

    const auto dx =
        pos.x>node.x ? pos.x-node.x : node.x-pos.x;
    const auto dy =
        pos.y>node.y ? pos.y-node.y : node.y-pos.y;
    const auto distance=std::max(dx,dy);

    if (isWalkNode &&
        pos.x==node.x && pos.y==node.y) {
        SetRamoniaCaveStatus(
            "Cavebot: chegou #" + std::to_string(state.index));
        AdvanceRamoniaCaveNode(cfg,state);
        return;
    }

    if (!state.lastPosition.valid ||
        pos.x!=state.lastPosition.x ||
        pos.y!=state.lastPosition.y ||
        pos.z!=state.lastPosition.z) {

        state.lastPosition=pos;
        state.lastProgressMs=nowMs;
        // Any real movement proves the current auto-walk is still alive.
        // Keep it in-flight and do NOT send another native Game::autoWalk.
        state.retries=0;
    } else if (state.walkIssued &&
               state.lastProgressMs &&
               nowMs-state.lastProgressMs>=
                   std::max<std::uint32_t>(500u,cfg.stuckMs)) {

        ++state.retries;
        state.walkIssued=false;
        state.issuedWalkX=0;
        state.issuedWalkY=0;
        state.issuedWalkZ=0;
        state.lastProgressMs=nowMs;
        state.nextActionMs=
            nowMs+std::max<std::uint32_t>(250u,cfg.retryDelayMs);

        if (state.retries>std::max(0,cfg.maxRetries)) {
            state.halted=true;
            SetRamoniaCaveStatus(
                "Cavebot: STOP retry limit waypoint #" +
                std::to_string(state.index));
            return;
        }

        SetRamoniaCaveStatus(
            "Cavebot: retry " + std::to_string(state.retries) +
            "/" + std::to_string(std::max(0,cfg.maxRetries)) +
            " waypoint #" + std::to_string(state.index));
    }

    if (state.nextActionMs && nowMs<state.nextActionMs)
        return;

    // Use/Rope/Shovel/Ladder/Hole are tile actions. Get adjacent first,
    // never auto-walk onto the target tile just to perform the action.
    if (!isWalkNode && distance>1u) {
        // One auto-walk at a time. While the previously accepted native walk
        // is making progress, only poll GET_PLAYER.
        if (state.walkIssued)
            return;

        std::uint32_t approachX=node.x;
        std::uint32_t approachY=node.y;

        if (pos.x<node.x) approachX=node.x-1u;
        else if (pos.x>node.x) approachX=node.x+1u;

        if (pos.y<node.y) approachY=node.y-1u;
        else if (pos.y>node.y) approachY=node.y+1u;

        nlohmann::json approachResponse;
        const bool approached=
            RamoniaPipeRequest(
                pid,
                {{"cmd","AUTO_WALK_TO"},
                 {"x",approachX},
                 {"y",approachY},
                 {"z",node.z}},
                approachResponse) &&
            approachResponse.value("ok",false);

        if (approached) {
            SetRamoniaCaveStatus(
                "Cavebot: aproximando #" + std::to_string(state.index));
            state.walkIssued=true;
            state.issuedWalkX=approachX;
            state.issuedWalkY=approachY;
            state.issuedWalkZ=node.z;
            state.nextActionMs=0;
            if (!state.lastProgressMs)
                state.lastProgressMs=nowMs;
        } else {
            const auto error=
                approachResponse.is_object()
                    ? approachResponse.value(
                        "error",
                        approachResponse.value(
                            "result",
                            std::string("approach_failed")))
                    : std::string("pipe_failed");
            SetRamoniaCaveStatus("Cavebot: aproximacao " + error);
            state.walkIssued=false;
            state.nextActionMs=
                nowMs+std::max<std::uint32_t>(250u,cfg.retryDelayMs);
        }
        return;
    }

    // Once adjacent, the approach auto-walk is finished from the Cavebot
    // point of view and the tile action may run.
    if (!isWalkNode && distance<=1u) {
        state.walkIssued=false;
        state.issuedWalkX=0;
        state.issuedWalkY=0;
        state.issuedWalkZ=0;
    }

    if (isDirectUseNode) {
        std::vector<std::uint32_t> candidateIds;
        auto pushCandidate=[&](int id){
            if(id<=0 || id>0xFFFF) return;
            const auto u=static_cast<std::uint32_t>(id);
            if(std::find(candidateIds.begin(),candidateIds.end(),u)==candidateIds.end())
                candidateIds.push_back(u);
        };

        // Structural map actions are best expressed like a normal client
        // "use top thing on this tile": itemId=0 lets the Bridge resolve the
        // current top Item from fresh Tile::things, avoiding build-specific IDs.
        // Keep explicit/catalog IDs only as fallback and diagnostics.
        if(node.type=="ladder" || node.type=="use_with_ground")
            candidateIds.push_back(0);

        pushCandidate(static_cast<int>(node.itemId));
        if(node.type=="ladder") {
            for(int id:CatalogIds("ladder")) pushCandidate(id);
        } else if(node.type=="use_with_ground") {
            for(int id:CatalogIds("openHoles")) pushCandidate(id);
        }

        // Generic Use with no explicit ID uses the same top-item semantics.
        if(candidateIds.empty() && node.type=="use")
            candidateIds.push_back(0);

        nlohmann::json actionResponse;
        bool used=false;
        std::uint32_t usedId=0;

        for(const auto candidateId:candidateIds) {
            actionResponse=nlohmann::json::object();
            const bool callOk=
                RamoniaPipeRequest(
                    pid,
                    {{"cmd","USE_TILE_ITEM_AT"},
                     {"x",node.x},
                     {"y",node.y},
                     {"z",node.z},
                     {"itemId",candidateId}},
                    actionResponse);
            if(callOk && actionResponse.value("ok",false)) {
                used=true;
                usedId=actionResponse.value("itemId",candidateId);
                break;
            }

            // Only an item-resolution miss should try the next known ID.
            const auto error=
                actionResponse.is_object()
                    ? actionResponse.value("error",std::string())
                    : std::string();
            if(error!="tile_item_not_found")
                break;
        }

        if (used) {
            const bool expectsFloorChange =
                node.type=="ladder" || node.type=="use_with_ground";

            if (expectsFloorChange) {
                state.transitionPending=true;
                state.transitionFromZ=pos.z;
                state.transitionDeadlineMs=nowMs+3000;
                state.nextActionMs=0;
                SetRamoniaCaveStatus(
                    "Cavebot: " + node.type +
                    " use OK id=" + std::to_string(usedId) +
                    " | aguardando Z #" + std::to_string(state.index));
            } else {
                SetRamoniaCaveStatus(
                    "Cavebot: " + node.type +
                    " OK id=" + std::to_string(usedId) +
                    " #" + std::to_string(state.index));
                AdvanceRamoniaCaveNode(cfg,state);
            }
        } else {
            const auto error=
                actionResponse.is_object()
                    ? actionResponse.value(
                        "error",
                        actionResponse.value(
                            "result",
                            std::string("use_failed")))
                    : std::string("pipe_failed");
            SetRamoniaCaveStatus(
                "Cavebot: " + node.type + " " + error);
            state.nextActionMs=nowMs+650;
        }
        return;
    }

    if (isToolUseNode) {
        if (!g_ramoniaToolTileReady.load()) {
            SetRamoniaCaveStatus("Cavebot: rope/shovel aguardando bridge atualizada");
            return;
        }
        const auto result=crome::RunRamoniaToolTileCycle(
            node.itemId,node.targetItemId,node.x,node.y,node.z,
            [&](const nlohmann::json& command,nlohmann::json& response) {
                return RamoniaPipeRequest(pid,command,response);
            });
        if (result.moved) {
            SetRamoniaCaveStatus(
                "Cavebot: " + node.type + " retirou bloqueador " +
                std::to_string(result.blockerId) + " #" +
                std::to_string(state.index));
            state.nextActionMs=static_cast<std::uint64_t>(GetTickCount64())+
                150u;
        } else if (result.used) {
            SetRamoniaCaveStatus(
                "Cavebot: " + node.type + " OK #" +
                std::to_string(state.index));
            AdvanceRamoniaCaveNode(cfg,state);
        } else {
            SetRamoniaCaveStatus(
                result.error=="target_item_id_required"
                    ? "Cavebot: configure Alvo no chao no waypoint " + node.type
                    : "Cavebot: " + node.type + " " + result.error);
            state.nextActionMs=static_cast<std::uint64_t>(GetTickCount64())+
                std::max<std::uint32_t>(650u,cfg.retryDelayMs);
        }
        return;
    }

    // Walk/Stand: never re-enter Game::autoWalk while the accepted request is
    // still progressing toward this waypoint.
    if (state.walkIssued)
        return;

    nlohmann::json walkResponse;
    const bool walked=
        RamoniaPipeRequest(
            pid,
            {{"cmd","AUTO_WALK_TO"},
             {"x",node.x},
             {"y",node.y},
             {"z",node.z}},
            walkResponse) &&
        walkResponse.value("ok",false);

    if (walked) {
        SetRamoniaCaveStatus(
            "Cavebot: indo #" + std::to_string(state.index) +
            " -> " + std::to_string(node.x) + "," +
            std::to_string(node.y) + "," +
            std::to_string(node.z));
        state.walkIssued=true;
        state.issuedWalkX=node.x;
        state.issuedWalkY=node.y;
        state.issuedWalkZ=node.z;
        state.nextActionMs=0;
        if (!state.lastProgressMs)
            state.lastProgressMs=nowMs;
    } else {
        const auto error=
            walkResponse.is_object()
                ? walkResponse.value(
                    "error",
                    walkResponse.value(
                        "result",
                        std::string("walk_failed")))
                : std::string("pipe_failed");

        SetRamoniaCaveStatus("Cavebot: " + error);
        state.walkIssued=false;
        state.nextActionMs=
            nowMs+std::max<std::uint32_t>(250u,cfg.retryDelayMs);
    }
}

static void RamoniaTargetWorker() {
    std::uint64_t nextFoodAttemptMs = 0;
    std::uint64_t nextSpearAttemptMs = 0;
    std::uint64_t nextLightRefreshMs = 0;
    std::uint64_t nextHealAttemptMs = 0;
    std::uint64_t nextRuneAttemptMs = 0;
    std::uint64_t nextPlayerSnapshotMs = 0;
    bool lightApplied = false;
    std::uint32_t lootMarkedTargetId = 0;
    std::uint64_t lootLastTargetMarkMs = 0;
    std::uint64_t lootDiffDeadlineMs = 0;
    std::vector<std::uint32_t> lootKnownCorpseIds{2813};
    RamoniaCaveWorkerState caveState{};

    RamoniaPositionUi lastPosition{};
    std::uint64_t mapTransitionUntilMs = 0;

    while (g_ramoniaWorkerRun.load()) {
        const DWORD pid = FindRamoniaProcess();
        g_ramoniaPid.store(pid);
        if (!pid) {
            g_ramoniaBridgeReady.store(false);
            g_ramoniaWalkReady.store(false);
            g_ramoniaSpearReady.store(false);
            g_ramoniaToolTileReady.store(false);
            g_ramoniaBridgeVersion.store(0);
            nextFoodAttemptMs = 0;
            nextSpearAttemptMs = 0;
            nextLightRefreshMs = 0;
            nextHealAttemptMs = 0;
            nextRuneAttemptMs = 0;
            nextPlayerSnapshotMs = 0;
            lightApplied = false;
            lootMarkedTargetId = 0;
            lootLastTargetMarkMs = 0;
            lootDiffDeadlineMs = 0;
            ResetRamoniaCaveWorkerState(caveState);
            lastPosition = {};
            mapTransitionUntilMs = 0;
            SetRamoniaPlayerPosition({});
            SetRamoniaStatus("Ramonia nao esta aberto");
            SetRamoniaFoodStatus("Auto Food: Ramonia fechado");
            SetRamoniaSpearStatus("Pick Up Spear: Ramonia fechado");
            SetRamoniaHealingStatus("Healing: Ramonia fechado");
            SetRamoniaRuneStatus("Trainer: Ramonia fechado");
            SetRamoniaCaveStatus("Cavebot: Ramonia fechado");
            SetRamoniaLootStatus("Loot: Ramonia fechado");
            Sleep(700);
            continue;
        }

        nlohmann::json hello;
        if (!RamoniaPipeRequest(pid, {{"cmd","HELLO"}}, hello) ||
            !hello.value("ok",false) ||
            !hello.value("profileReady",false) ||
            !hello.value("buildMatched",false) ||
            !hello.value("dispatcherReady",false) ||
            hello.value("bridgeVersion",0) < 6) {
            g_ramoniaBridgeReady.store(false);
            g_ramoniaWalkReady.store(false);
            g_ramoniaSpearReady.store(false);
            g_ramoniaToolTileReady.store(false);
            g_ramoniaBridgeVersion.store(
                hello.is_object() ? hello.value("bridgeVersion",0) : 0);
            nextFoodAttemptMs = 0;
            nextSpearAttemptMs = 0;
            nextLightRefreshMs = 0;
            nextHealAttemptMs = 0;
            nextRuneAttemptMs = 0;
            nextPlayerSnapshotMs = 0;
            lightApplied = false;
            lootMarkedTargetId = 0;
            lootLastTargetMarkMs = 0;
            lootDiffDeadlineMs = 0;
            ResetRamoniaCaveWorkerState(caveState);
            lastPosition = {};
            mapTransitionUntilMs = 0;
            SetRamoniaPlayerPosition({});
            SetRamoniaStatus("Ramonia aberto - injete CromeRamoniaBridge_x86.dll");
            SetRamoniaFoodStatus("Auto Food: bridge aguardando");
            SetRamoniaSpearStatus("Pick Up Spear: bridge aguardando");
            SetRamoniaHealingStatus("Healing: bridge aguardando");
            SetRamoniaRuneStatus("Trainer: bridge aguardando");
            SetRamoniaCaveStatus("Cavebot: bridge aguardando");
            SetRamoniaLootStatus("Loot: bridge aguardando");
            Sleep(500);
            continue;
        }

        const int bridgeVersion = hello.value("bridgeVersion",0);
        g_ramoniaBridgeReady.store(true);
        g_ramoniaBridgeVersion.store(bridgeVersion);
        g_ramoniaSpearReady.store(hello.value("spearPickupReady",false));
        g_ramoniaToolTileReady.store(hello.value("toolTileUncoverReady",false));
        g_ramoniaWalkReady.store(
            bridgeVersion>=33 && hello.value("walkReady",false));

        const auto nowMs = static_cast<std::uint64_t>(GetTickCount64());

        if (!nextPlayerSnapshotMs || nowMs>=nextPlayerSnapshotMs) {
            nlohmann::json playerSnapshot;
            if (RamoniaPipeRequest(pid,{{"cmd","GET_PLAYER"}},playerSnapshot)) {
                const auto pos=ParseRamoniaPlayerPosition(playerSnapshot);
                if (pos.valid)
                    SetRamoniaPlayerPosition(pos);
            }
            nextPlayerSnapshotMs=nowMs+150;
        }

        // Light is independent from Target/Loot/Food and does not use the
        // game-thread dispatcher. Bridge V22 writes the profile-confirmed
        // MapView minimumAmbient field and restores the original on OFF.
        const auto lightCfg=GetRamoniaLightConfig();
        const bool bridgeReportsLight=
            hello.is_object() && hello.value("lightEnabled",false);

        if (!lightCfg.enabled) {
            nextLightRefreshMs=0;
            if (bridgeVersion>=22 && (lightApplied || bridgeReportsLight)) {
                nlohmann::json lightResponse;
                RamoniaPipeRequest(
                    pid,
                    {{"cmd","SET_LIGHT"},{"enabled",0}},
                    lightResponse);
            }
            lightApplied=false;
            SetRamoniaLightStatus("Light: desligado");
        } else if (bridgeVersion<22) {
            nextLightRefreshMs=0;
            lightApplied=false;
            SetRamoniaLightStatus("Light: requer Bridge V22+");
        } else if (!nextLightRefreshMs || nowMs>=nextLightRefreshMs) {
            nlohmann::json lightResponse;
            const bool ok=
                RamoniaPipeRequest(
                    pid,
                    {{"cmd","SET_LIGHT"},{"enabled",1}},
                    lightResponse) &&
                lightResponse.value("ok",false) &&
                lightResponse.value("contractReady",false);

            if (ok) {
                lightApplied=true;
                SetRamoniaLightStatus(
                    "Light: ambient=" +
                    std::to_string(lightResponse.value("ambient",1.0)));
            } else {
                lightApplied=false;
                SetRamoniaLightStatus("Light: MapView aguardando");
            }
            nextLightRefreshMs=nowMs+500;
        }

        bool healingActionThisCycle=false;

        // Ramonia Healing: spell rules use SAY/talkChannel; item rules use
        // USE_CONTAINER_ITEM_ON_SELF from Bridge V24+ (V25 inherits it).
        const auto healingCfg=GetRamoniaHealingConfig();
        if (!healingCfg.enabled) {
            nextHealAttemptMs=0;
            SetRamoniaHealingStatus("Healing: desligado");
        } else if (bridgeVersion<23) {
            nextHealAttemptMs=0;
            SetRamoniaHealingStatus("Healing: requer Bridge V23+");
        } else if (!nextHealAttemptMs || nowMs>=nextHealAttemptMs) {
            nlohmann::json playerJson;
            const bool playerOk=
                RamoniaPipeRequest(pid,{{"cmd","GET_PLAYER"}},playerJson);
            const auto vitals=playerOk ? ParseRamoniaVitals(playerJson) : RamoniaVitalsUi{};

            const RamoniaHealingSpellRuleUi* selectedSpell=nullptr;
            const RamoniaHealingItemRuleUi* selectedItem=nullptr;

            if (vitals.valid) {
                const double hpPct=vitals.healthPercent();
                const double mpPct=vitals.manaPercent();

                for (const auto& rule:healingCfg.spells) {
                    if (!rule.enabled || rule.spell.empty() || rule.spell.size()>15)
                        continue;
                    if (hpPct>static_cast<double>(rule.hpPercent) ||
                        vitals.mana<rule.minMana)
                        continue;
                    if (!selectedSpell || rule.priority>selectedSpell->priority)
                        selectedSpell=&rule;
                }

                if (bridgeVersion>=24) {
                    for (const auto& rule:healingCfg.items) {
                        if (!rule.enabled || !rule.itemId || rule.itemId>0xFFFFu)
                            continue;

                        const double current =
                            rule.manaTrigger ? mpPct : hpPct;
                        if (current>static_cast<double>(rule.triggerPercent))
                            continue;

                        if (!selectedItem || rule.priority>selectedItem->priority)
                            selectedItem=&rule;
                    }
                }
            }

            bool acted=false;
            bool itemUnavailable=false;

            const bool itemHasPriority=
                selectedItem &&
                (!selectedSpell || selectedItem->priority>selectedSpell->priority);

            if (itemHasPriority) {
                nlohmann::json useJson;
                const bool used=
                    RamoniaPipeRequest(
                        pid,
                        {{"cmd","USE_CONTAINER_ITEM_ON_SELF"},
                         {"itemId",selectedItem->itemId}},
                        useJson) &&
                    useJson.value("ok",false);

                if (used) {
                    acted=true;
                    healingActionThisCycle=true;
                    SetRamoniaHealingStatus(
                        "Healing: " + selectedItem->name +
                        " ID=" + std::to_string(selectedItem->itemId));
                    nextHealAttemptMs=
                        nowMs+std::max<std::uint32_t>(250u,selectedItem->cooldownMs);
                } else {
                    const auto error=
                        useJson.is_object()
                            ? useJson.value("error",std::string())
                            : std::string();

                    itemUnavailable=
                        error=="item_not_found_in_open_containers" ||
                        error=="source_container_not_open";

                    SetRamoniaHealingStatus(
                        itemUnavailable
                            ? "Healing: "+selectedItem->name+" nao encontrado na bag aberta"
                            : "Healing: "+selectedItem->name+" useWith pendente");
                }
            }

            // If a higher-priority item was configured but is unavailable,
            // fall back to the best matching spell instead of leaving the
            // player without healing.
            if (!acted && selectedSpell) {
                nlohmann::json sayJson;
                const bool cast=
                    RamoniaPipeRequest(
                        pid,
                        {{"cmd","SAY"},{"message",selectedSpell->spell}},
                        sayJson) &&
                    sayJson.value("ok",false);

                if (cast) {
                    acted=true;
                    healingActionThisCycle=true;
                    SetRamoniaHealingStatus(
                        "Healing: " + selectedSpell->spell +
                        " HP=" + std::to_string(
                            static_cast<int>(vitals.healthPercent())) + "%");
                    nextHealAttemptMs=
                        nowMs+std::max<std::uint32_t>(250u,selectedSpell->cooldownMs);
                } else {
                    SetRamoniaHealingStatus("Healing: SAY pendente");
                }
            }

            // Mana Fluid has lower priority than the spell rules by design.
            // If no spell fired, allow the matching item rule now.
            if (!acted && selectedItem && !itemHasPriority) {
                nlohmann::json useJson;
                const bool used=
                    RamoniaPipeRequest(
                        pid,
                        {{"cmd","USE_CONTAINER_ITEM_ON_SELF"},
                         {"itemId",selectedItem->itemId}},
                        useJson) &&
                    useJson.value("ok",false);

                if (used) {
                    acted=true;
                    healingActionThisCycle=true;
                    SetRamoniaHealingStatus(
                        "Healing: " + selectedItem->name +
                        " ID=" + std::to_string(selectedItem->itemId));
                    nextHealAttemptMs=
                        nowMs+std::max<std::uint32_t>(250u,selectedItem->cooldownMs);
                }
            }

            if (!acted) {
                if (!vitals.valid)
                    SetRamoniaHealingStatus("Healing: aguardando player");
                else if (!selectedSpell && !selectedItem)
                    SetRamoniaHealingStatus("Healing: monitorando");

                nextHealAttemptMs=nowMs+100;
            }
        }

        // Rune Maker / Mana Trainer. Mana is a >= threshold. Soul is
        // enforced only when the current build exposes a valid Soul field.
        const auto runeCfg=GetRamoniaRuneConfig();
        if (!runeCfg.enabled) {
            nextRuneAttemptMs=0;
            SetRamoniaRuneStatus("Trainer: desligado");
        } else if (bridgeVersion<26) {
            nextRuneAttemptMs=0;
            SetRamoniaRuneStatus("Trainer: requer Bridge V26+");
        } else if (!healingActionThisCycle &&
                   (!nextRuneAttemptMs || nowMs>=nextRuneAttemptMs)) {

            nlohmann::json playerJson;
            const bool playerOk=
                RamoniaPipeRequest(pid,{{"cmd","GET_PLAYER"}},playerJson);
            const auto vitals=playerOk ? ParseRamoniaVitals(playerJson) : RamoniaVitalsUi{};

            if (!vitals.valid) {
                SetRamoniaRuneStatus("Trainer: aguardando player");
                nextRuneAttemptMs=nowMs+150;
            } else {
                const int manaPct=
                    static_cast<int>(vitals.manaPercent());
                const bool manaReady=
                    manaPct>=std::clamp(runeCfg.manaPercent,0,100);
                const bool soulReady=
                    !vitals.soulAvailable ||
                    vitals.soul>=static_cast<double>(std::max(0,runeCfg.soulMin));

                std::ostringstream status;
                status << "Trainer: Mana " << manaPct << "%";
                if (vitals.soulAvailable)
                    status << " | Soul " << static_cast<int>(vitals.soul);
                else
                    status << " | Soul N/A";

                if (!manaReady) {
                    status << " | aguardando mana";
                    SetRamoniaRuneStatus(status.str());
                    nextRuneAttemptMs=nowMs+150;
                } else if (!soulReady) {
                    status << " | aguardando soul";
                    SetRamoniaRuneStatus(status.str());
                    nextRuneAttemptMs=nowMs+250;
                } else if (runeCfg.spell.empty() || runeCfg.spell.size()>15) {
                    SetRamoniaRuneStatus("Trainer: spell invalida (1-15 chars)");
                    nextRuneAttemptMs=nowMs+500;
                } else {
                    nlohmann::json sayJson;
                    const bool cast=
                        RamoniaPipeRequest(
                            pid,
                            {{"cmd","SAY"},{"message",runeCfg.spell}},
                            sayJson) &&
                        sayJson.value("ok",false);

                    if (cast) {
                        status << " | cast " << runeCfg.spell;
                        SetRamoniaRuneStatus(status.str());
                        nextRuneAttemptMs=
                            nowMs+std::max<std::uint32_t>(
                                300u,runeCfg.cooldownMs);
                    } else {
                        SetRamoniaRuneStatus("Trainer: SAY pendente");
                        nextRuneAttemptMs=nowMs+250;
                    }
                }
            }
        } else if (healingActionThisCycle) {
            SetRamoniaRuneStatus("Trainer: Healing teve prioridade");
        }

        // Auto Food stays independent from Target and Loot. Food IDs come
        // exclusively from the global walker catalog; try them in list order
        // until one is found in any currently open container.
        const auto foodCfg = GetRamoniaFoodConfig();
        if (!foodCfg.enabled) {
            nextFoodAttemptMs = 0;
            SetRamoniaFoodStatus("Auto Food: desligado");
        } else if (bridgeVersion < 16) {
            nextFoodAttemptMs = 0;
            SetRamoniaFoodStatus("Auto Food: requer Bridge V16+");
        } else if (foodCfg.itemIds.empty()) {
            nextFoodAttemptMs = 0;
            SetRamoniaFoodStatus("Auto Food: cadastre Food IDs");
        } else if (!nextFoodAttemptMs || nowMs >= nextFoodAttemptMs) {
            bool usedFood=false;
            bool transportFailed=false;
            nlohmann::json lastFoodResponse;

            for(const auto itemId:foodCfg.itemIds) {
                if(!itemId || itemId>0xFFFFu) continue;

                nlohmann::json foodResponse;
                const bool transportOk = RamoniaPipeRequest(
                    pid,
                    {{"cmd","USE_CONTAINER_ITEM"},{"itemId",itemId}},
                    foodResponse);

                if(!transportOk) {
                    transportFailed=true;
                    lastFoodResponse=nlohmann::json::object();
                    break;
                }

                lastFoodResponse=foodResponse;
                if(foodResponse.value("ok",false)) {
                    usedFood=true;
                    const auto containerId=foodResponse.value("sourceContainerId",0u);
                    const auto slot=foodResponse.value("slot",0u);
                    SetRamoniaFoodStatus(
                        "Auto Food: usou " + std::to_string(itemId) +
                        " bag=" + std::to_string(containerId) +
                        " slot=" + std::to_string(slot));
                    break;
                }

                const auto error=foodResponse.value("error",std::string());
                if(error=="item_not_found_in_open_containers")
                    continue;

                // Holder races can resolve on the next cycle. Do not continue
                // firing different food IDs after a real action-side failure.
                if(error=="container_item_not_visible" ||
                   error=="item_holder_changed")
                    break;

                break;
            }

            if(usedFood) {
                nextFoodAttemptMs =
                    nowMs + std::max<std::uint32_t>(1000u,foodCfg.intervalMs);
            } else {
                const std::string error =
                    lastFoodResponse.is_object()
                        ? lastFoodResponse.value("error",std::string())
                        : std::string();

                if(transportFailed)
                    SetRamoniaFoodStatus("Auto Food: pipe indisponivel");
                else if(error.empty() ||
                        error=="item_not_found_in_open_containers")
                    SetRamoniaFoodStatus("Auto Food: nenhum Food ID encontrado nas bags");
                else if(error=="container_item_not_visible" ||
                        error=="item_holder_changed")
                    SetRamoniaFoodStatus("Auto Food: item mudou - tentando novamente");
                else
                    SetRamoniaFoodStatus("Auto Food: "+error);

                nextFoodAttemptMs =
                    nowMs + std::max<std::uint32_t>(250u,foodCfg.retryMs);
            }
        }

        const auto targetCfg = GetRamoniaTargetConfig();
        const auto lootCfg = GetRamoniaLootConfig();
        const auto caveCfg = GetRamoniaCaveConfig();
        const auto spearCfg = GetRamoniaSpearConfig();
        for(const auto id:lootCfg.learnedCorpseIds)
            if(id && std::find(lootKnownCorpseIds.begin(),lootKnownCorpseIds.end(),id)
                      ==lootKnownCorpseIds.end())
                lootKnownCorpseIds.push_back(id);
        if (lootCfg.corpseItemId &&
            std::find(lootKnownCorpseIds.begin(),lootKnownCorpseIds.end(),
                      lootCfg.corpseItemId)==lootKnownCorpseIds.end())
            lootKnownCorpseIds.push_back(lootCfg.corpseItemId);
        const bool targetPolicyActive =
            targetCfg.enabled &&
            bridgeVersion >= 19 &&
            HasRamoniaTargetPolicy(targetCfg);
        const bool lootActive =
            lootCfg.enabled && bridgeVersion >= 21;
        const bool caveWalkReady =
            bridgeVersion>=33 && g_ramoniaWalkReady.load();
        const bool caveActive =
            caveCfg.enabled && caveWalkReady;
        const bool spearActive =
            spearCfg.enabled && g_ramoniaSpearReady.load() && !spearCfg.itemIds.empty();
        // Wake close to the next tile-action deadline instead of adding the
        // worker's ordinary 120/300 ms sleep to each blocker delay.
        auto workerCycleDelay=[&](DWORD normalDelay) {
            const auto tick=static_cast<std::uint64_t>(GetTickCount64());
            auto beforeDeadline=[&](std::uint64_t deadline) {
                const auto remaining=deadline>tick ? deadline-tick : 25u;
                return static_cast<DWORD>(
                    std::clamp<std::uint64_t>(remaining,25u,normalDelay));
            };
            DWORD delay=normalDelay;
            if (spearActive && nextSpearAttemptMs)
                delay=std::min(delay,beforeDeadline(nextSpearAttemptMs));
            if (caveActive && !caveState.halted &&
                caveState.index<caveCfg.nodes.size()) {
                const auto& current=caveCfg.nodes[caveState.index];
                if (current.type=="rope" || current.type=="shovel")
                    delay=std::min(delay,caveState.nextActionMs
                        ? beforeDeadline(caveState.nextActionMs) : 50u);
            }
            return delay;
        };


        if (!spearCfg.enabled) {
            nextSpearAttemptMs=0;
            SetRamoniaSpearStatus("Pick Up Spear: desligado");
        } else if (!g_ramoniaSpearReady.load()) {
            nextSpearAttemptMs=0;
            SetRamoniaSpearStatus("Pick Up Spear: aguardando bridge atualizada");
        } else if (spearCfg.itemIds.empty()) {
            nextSpearAttemptMs=0;
            SetRamoniaSpearStatus("Pick Up Spear: informe um Item ID");
        }

        if (!caveActive) {
            if (!caveCfg.enabled)
                SetRamoniaCaveStatus("Cavebot: desligado");
            else if (bridgeVersion<33)
                SetRamoniaCaveStatus("Cavebot: requer Bridge V33+");
            else
                SetRamoniaCaveStatus("Cavebot: AutoWalk V40 aguardando validacao");
        }

        if (!lootActive) {
            lootMarkedTargetId = 0;
            lootLastTargetMarkMs = 0;
            lootDiffDeadlineMs = 0;
            SetRamoniaLootStatus(lootCfg.enabled
                ? "Loot: requer Bridge V21+"
                : "Loot: desligado");
        }

        // Tile::things must not be queried while the client is changing
        // floor/map. A floor transition can destroy/rebuild Tile objects while
        // Target/Loot still hold a snapshot from the previous floor.
        if (targetPolicyActive || lootActive || caveActive || spearActive) {
            nlohmann::json playerJson;
            if (RamoniaPipeRequest(pid, {{"cmd","GET_PLAYER"}}, playerJson)) {
                const auto pos = ParseRamoniaPlayerPosition(playerJson);
                if (pos.valid) {
                    if (lastPosition.valid) {
                        const auto dx = pos.x > lastPosition.x ? pos.x-lastPosition.x : lastPosition.x-pos.x;
                        const auto dy = pos.y > lastPosition.y ? pos.y-lastPosition.y : lastPosition.y-pos.y;
                        const bool floorChanged = pos.z != lastPosition.z;
                        const bool largeJump = std::max(dx,dy) > 12u;

                        if (floorChanged || largeJump) {
                            mapTransitionUntilMs = nowMs + 1600;
                            lootMarkedTargetId = 0;
                            lootLastTargetMarkMs = 0;
                            lootDiffDeadlineMs = 0;
                            SetRamoniaStatus("Mapa mudou - aguardando estabilizar");
                            if (lootActive)
                                SetRamoniaLootStatus("Loot pausado durante troca de mapa");
                        }
                    }
                    lastPosition = pos;
                }
            }

            if (mapTransitionUntilMs && nowMs < mapTransitionUntilMs) {
                Sleep(120);
                continue;
            }
            if (mapTransitionUntilMs && nowMs >= mapTransitionUntilMs)
                mapTransitionUntilMs = 0;
        }

        // One pickup/uncover action per cycle, followed by a delay so the
        // server can update the tile before the next game-thread revalidation.
        if (spearActive &&
            (!nextSpearAttemptMs || nowMs>=nextSpearAttemptMs)) {
            crome::RamoniaSpearCycleResult result;
            std::uint32_t selectedItemId{};
            for (const auto itemId:spearCfg.itemIds) {
                if (!itemId || itemId>0xFFFFu) continue;
                selectedItemId=itemId;
                result=crome::RunRamoniaSpearPickupCycle(itemId,
                    [&](const nlohmann::json& command,nlohmann::json& response) {
                        return RamoniaPipeRequest(pid,command,response);
                    });
                if (result.error=="item_not_found_in_3x3") continue;
                break;
            }
            if (result.moved) {
                SetRamoniaSpearStatus(
                    "Pick Up Spear: enviou " + std::to_string(selectedItemId) +
                    " para a mao " + std::to_string(result.destinationSlot));
            } else if (result.uncovered) {
                SetRamoniaSpearStatus(
                    "Pick Up Spear: retirou bloqueador " +
                    std::to_string(result.blockerId));
            } else if (result.transportFailed) {
                SetRamoniaSpearStatus("Pick Up Spear: pipe indisponivel");
            } else if (result.error=="item_not_found_in_3x3") {
                SetRamoniaSpearStatus("Pick Up Spear: procurando ao redor");
            } else if (result.error=="both_hands_occupied_by_other_items" ||
                       result.error=="inventory_hand_occupied_by_other_item") {
                SetRamoniaSpearStatus("Pick Up Spear: maos ocupadas");
            } else if (result.error.empty()) {
                SetRamoniaSpearStatus("Pick Up Spear: alvo exposto - aguardando");
            } else {
                SetRamoniaSpearStatus("Pick Up Spear: "+result.error);
            }
            nextSpearAttemptMs=static_cast<std::uint64_t>(GetTickCount64())+
                (result.moved || result.uncovered || result.error.empty()
                    ? 150u : 500u);
        }

        if (!targetPolicyActive && !lootActive) {
            if (targetCfg.enabled && bridgeVersion < 19)
                SetRamoniaStatus("Target: requer Bridge V19+");
            else if (targetCfg.enabled)
                SetRamoniaStatus("Target ativo - informe um alvo ou marque Any");
            else
                SetRamoniaStatus("Ramonia Bridge pronta");

            if (caveActive)
                RunRamoniaCaveCycle(pid,caveCfg,caveState,nowMs,false);

            Sleep(workerCycleDelay(caveActive ? 120 : 300));
            continue;
        }

        nlohmann::json monstersJson, combatJson;
        if (!RamoniaPipeRequest(pid, {{"cmd","GET_VISIBLE_MONSTERS"}}, monstersJson) ||
            !RamoniaPipeRequest(pid, {{"cmd","GET_COMBAT_TARGETS"}}, combatJson)) {
            if (targetPolicyActive) SetRamoniaStatus("Target: aguardando bridge");
            if (lootActive) SetRamoniaLootStatus("Loot: aguardando bridge");
            Sleep(workerCycleDelay(300));
            continue;
        }

        const auto monsters = ParseRamoniaMonsters(monstersJson);
        auto currentAttackId = ParseRamoniaAttackId(combatJson);

        // Fast reacquire lane:
        // Loot can legitimately spend several seconds finishing the corpse of
        // the previous target. Do NOT let that block combat. When the game has
        // already cleared AttackTarget, acquire the next monster first, then
        // continue processing the previous death/loot state below.
        if (targetPolicyActive && currentAttackId == 0) {
            const auto fastTarget =
                ChooseRamoniaTarget(monsters,targetCfg,0);

            if (fastTarget) {
                nlohmann::json attackResponse;
                const bool transportOk =
                    RamoniaPipeRequest(
                        pid,
                        {{"cmd","ATTACK_ID"},{"creatureId",fastTarget->id}},
                        attackResponse);
                const bool actionOk =
                    transportOk &&
                    attackResponse.is_object() &&
                    attackResponse.value("ok",false);

                if (actionOk) {
                    currentAttackId=fastTarget->id;
                    SetRamoniaStatus(
                        "Target reacquired",
                        fastTarget->name,
                        fastTarget->id,
                        fastTarget->hpPercent);

                    // Do not overwrite a pending previous Loot baseline here.
                    // Once that death is resolved, the normal Loot marker will
                    // arm itself on this new AttackTarget.
                    if (!lootMarkedTargetId)
                        lootLastTargetMarkMs=0;
                } else {
                    const std::string error =
                        (transportOk && attackResponse.is_object())
                            ? attackResponse.value("error",std::string())
                            : std::string();
                    if (error == "creature_id_not_visible")
                        SetRamoniaStatus("Alvo sumiu - procurando proximo");
                    else
                        SetRamoniaStatus("ATTACK_ID pendente - nova tentativa");
                }
            }
        }

        // Loot baseline is armed against the actual AttackTarget. This removes
        // the manual diagnostic race where LOOT_MARK could run before a Rat was
        // present in the tracked tile snapshot.
        if (lootActive) {
            if (lootMarkedTargetId && currentAttackId != lootMarkedTargetId) {
                if (!lootDiffDeadlineMs) lootDiffDeadlineMs = nowMs + 5500;

                nlohmann::json diff;
                if (RamoniaPipeRequest(pid, {{"cmd","LOOT_DIFF"}}, diff)) {
                    const auto corpseCandidates=
                        RamoniaLootCorpseCandidates(
                            diff,lootMarkedTargetId,lootKnownCorpseIds);

                    if (!corpseCandidates.empty()) {
                        bool opened=false;
                        int unknownProbes=0;
                        for (const auto corpseId:corpseCandidates) {
                            const bool knownCorpse =
                                std::find(
                                    lootKnownCorpseIds.begin(),
                                    lootKnownCorpseIds.end(),
                                    corpseId) != lootKnownCorpseIds.end();

                            if (!knownCorpse && unknownProbes++ >= 6)
                                break;

                            SetRamoniaLootStatus(
                                std::string("Loot: ") +
                                (knownCorpse ? "corpse conhecido " : "probe corpse ") +
                                std::to_string(corpseId));

                            if (RamoniaLootOpenAndMove(
                                    pid,lootCfg,corpseId,
                                    knownCorpse ? 1250u : 320u)) {
                                opened=true;
                                if (!knownCorpse) {
                                    lootKnownCorpseIds.push_back(corpseId);
                                    LearnRamoniaCorpseId(corpseId);
                                    SetRamoniaLootStatus(
                                        "Loot: aprendeu e salvou corpse ID " +
                                        std::to_string(corpseId));
                                }
                                break;
                            }
                        }

                        if (opened) {
                            lootMarkedTargetId=0;
                            lootLastTargetMarkMs=0;
                            lootDiffDeadlineMs=0;
                            Sleep(120);
                            continue;
                        }
                    }
                }

                if (nowMs < lootDiffDeadlineMs) {
                    SetRamoniaLootStatus("Loot: aguardando/abrindo corpse...");
                    Sleep(120);
                    continue;
                }

                SetRamoniaLootStatus("Loot: morte sem corpse abrivel confirmado");
                lootMarkedTargetId=0;
                lootLastTargetMarkMs=0;
                lootDiffDeadlineMs=0;
            }

            if (currentAttackId) {
                const bool newTarget = lootMarkedTargetId != currentAttackId;
                const bool refreshDue =
                    !lootLastTargetMarkMs || nowMs - lootLastTargetMarkMs >= 250;

                if (newTarget || refreshDue) {
                    nlohmann::json mark;
                    const bool marked =
                        RamoniaPipeRequest(
                            pid,
                            {{"cmd","LOOT_MARK_TARGET"},{"creatureId",currentAttackId}},
                            mark) &&
                        mark.value("ok",false) &&
                        mark.value("targetId",0u)==currentAttackId;

                    if (marked) {
                        lootMarkedTargetId=currentAttackId;
                        lootLastTargetMarkMs=nowMs;
                        lootDiffDeadlineMs=0;
                        SetRamoniaLootStatus(
                            "Loot: acompanhando target " +
                            std::to_string(currentAttackId) +
                            " tile=" +
                            std::to_string(mark.value("targetTileSlot",0u)));
                    } else if (newTarget) {
                        SetRamoniaLootStatus(
                            "Loot: aguardando target entrar no tile snapshot");
                    }
                }
            } else if (!lootMarkedTargetId) {
                SetRamoniaLootStatus("Loot: aguardando AttackTarget");
            }
        }

        if (!targetPolicyActive) {
            SetRamoniaStatus("Ramonia Bridge pronta");
            if (caveActive)
                RunRamoniaCaveCycle(
                    pid,caveCfg,caveState,nowMs,currentAttackId!=0);
            Sleep(workerCycleDelay(caveActive ? 120 : 300));
            continue;
        }

        const auto currentAttackForTarget = currentAttackId;
        const auto target = ChooseRamoniaTarget(
            monsters,targetCfg,currentAttackForTarget);

        if (!target) {
            if (currentAttackForTarget) {
                SetRamoniaStatus(
                    "Target travado no ID " + std::to_string(currentAttackForTarget) +
                    " - aguardando confirmacao do jogo");
                if (caveActive)
                    RunRamoniaCaveCycle(pid,caveCfg,caveState,nowMs,true);
            } else {
                SetRamoniaStatus("Target ativo - aguardando alvo");
                if (caveActive)
                    RunRamoniaCaveCycle(pid,caveCfg,caveState,nowMs,false);
            }
            Sleep(workerCycleDelay(caveActive ? 120 : 300));
            continue;
        }

        if (target->id != currentAttackForTarget) {
            nlohmann::json attackResponse;
            const bool transportOk =
                RamoniaPipeRequest(pid, {{"cmd","ATTACK_ID"},{"creatureId",target->id}}, attackResponse);
            const bool actionOk =
                transportOk && attackResponse.is_object() && attackResponse.value("ok",false);
            if (!actionOk) {
                const std::string error =
                    (transportOk && attackResponse.is_object())
                        ? attackResponse.value("error",std::string())
                        : std::string();
                if (error == "creature_id_not_visible")
                    SetRamoniaStatus("Alvo sumiu - procurando proximo");
                else
                    SetRamoniaStatus("ATTACK_ID pendente - nova tentativa");
                Sleep(250);
                continue;
            }

            // Bridge V20 creates a target-specific baseline on the next worker
            // cycle. Never fall back to broad LOOT_MARK here.
            if (lootActive && lootMarkedTargetId != target->id)
                lootLastTargetMarkMs = 0;
        }

        SetRamoniaStatus("Target ativo",target->name,target->id,target->hpPercent);
        if (caveActive)
            RunRamoniaCaveCycle(pid,caveCfg,caveState,nowMs,true);
        Sleep(workerCycleDelay(300));
    }
}

static bool TargetRuntimeReady() {
    return g_nativeRuntimeLoaded ||
           (g_ramoniaBridgeReady.load() && g_ramoniaBridgeVersion.load() >= 19);
}

static bool FoodRuntimeReady() {
    return g_nativeRuntimeLoaded ||
           (g_ramoniaBridgeReady.load() && g_ramoniaBridgeVersion.load() >= 16);
}

static bool LightRuntimeReady() {
    return g_nativeRuntimeLoaded ||
           (g_ramoniaBridgeReady.load() && g_ramoniaBridgeVersion.load() >= 22);
}

static bool HealingRuntimeReady() {
    return g_nativeRuntimeLoaded ||
           (g_ramoniaBridgeReady.load() && g_ramoniaBridgeVersion.load() >= 23);
}

static bool RuneRuntimeReady() {
    return g_nativeRuntimeLoaded ||
           (g_ramoniaBridgeReady.load() && g_ramoniaBridgeVersion.load() >= 26);
}

static bool LootRuntimeReady() {
    return g_nativeRuntimeLoaded ||
           (g_ramoniaBridgeReady.load() && g_ramoniaBridgeVersion.load() >= 21);
}

static bool CaveRuntimeReady() {
    return g_nativeRuntimeLoaded ||
           (g_ramoniaBridgeReady.load() &&
            g_ramoniaBridgeVersion.load() >= 33 &&
            g_ramoniaWalkReady.load());
}

static bool IsModuleLoaded(DWORD pid, const wchar_t* moduleName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me{}; me.dwSize = sizeof(me);
    bool found = false;
    if (Module32FirstW(snap, &me)) {
        do { if (_wcsicmp(me.szModule, moduleName) == 0) { found = true; break; } }
        while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

static bool InjectRuntimeDll(DWORD pid, const std::filesystem::path& dllPath, std::string& error) {
    if (!std::filesystem::exists(dllPath)) { error = "CromeRuntime.dll missing"; return false; }
    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                 PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!process) { error = "OpenProcess=" + std::to_string(GetLastError()); return false; }
    const std::wstring wide = std::filesystem::absolute(dllPath).wstring();
    const SIZE_T bytes = (wide.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { error = "VirtualAllocEx=" + std::to_string(GetLastError()); CloseHandle(process); return false; }
    SIZE_T written{};
    if (!WriteProcessMemory(process, remote, wide.c_str(), bytes, &written) || written != bytes) {
        error = "WriteProcessMemory=" + std::to_string(GetLastError());
        VirtualFreeEx(process, remote, 0, MEM_RELEASE); CloseHandle(process); return false;
    }
    auto k32 = GetModuleHandleW(L"kernel32.dll");
    auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(k32, "LoadLibraryW"));
    HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remote, 0, nullptr);
    if (!thread) { error = "CreateRemoteThread=" + std::to_string(GetLastError()); VirtualFreeEx(process, remote, 0, MEM_RELEASE); CloseHandle(process); return false; }
    const DWORD wait=WaitForSingleObject(thread, 5000);
    if(wait!=WAIT_OBJECT_0){error="LoadLibraryW timeout; restart client before retry";CloseHandle(thread);CloseHandle(process);return false;}
    DWORD code = 0; GetExitCodeThread(thread, &code);
    CloseHandle(thread); VirtualFreeEx(process, remote, 0, MEM_RELEASE); CloseHandle(process);
    if (!code) { error = "LoadLibraryW returned 0"; return false; }
    return true;
}

static void RefreshNativeRuntime() {
    const DWORD pid = FindProcessByName(L"Olders_OpenGL.exe");
    if (!pid) {
        g_nativeRuntimeLoaded = false; g_nativeRuntimeStatus = "Olders not running";
        g_nativeAttemptPid = 0; g_nativeAttemptedForPid = false; return;
    }
    // Restore the historical wrappers only for their exact baseline. Discovery
    // candidates from other builds remain disabled; loaded DLL != ready runtime.
    const auto* profile=g_session ? g_session->profile() : nullptr;
    const auto* session=g_session ? &g_session->status() : nullptr;
    const bool legacyBaseline=profile&&crome::olders_baseline::matches(static_cast<DWORD>(profile->build.timestamp),static_cast<DWORD>(profile->build.sizeOfImage),std::strtoull(profile->build.textHash.c_str(),nullptr,16));
    if (!profile || (!profile->nativeActionsReady && !legacyBaseline) ||
        profile->build.architecture!="x64" || !session ||
        session->pid!=pid || !session->profileMatched ||
        !session->localPlayerResolved || !session->localPlayerVtableOk) {
        g_nativeRuntimeLoaded=false;
        g_nativeRuntimeStatus="Olders: somente leitura; runtime aguarda validacao";
        g_nativeAttemptPid=pid;
        g_nativeAttemptedForPid=false;
        return;
    }
    if (pid != g_nativeAttemptPid) { g_nativeAttemptPid = pid; g_nativeAttemptedForPid = false; }
    if (IsModuleLoaded(pid, L"CromeRuntime.dll")) {
        nlohmann::json reply;const bool connected=crome::olders_pipe::request(pid,{{"cmd","STATUS"}},reply);
        const bool ready=connected&&reply.value("ready",false)&&reply.value("protocol",0)==1&&reply.value("pid",0u)==pid&&reply.value("build",std::string{})=="6AA9FA57_89D3D35243890EA2";
        if(ready&&!g_nativeRuntimeLoaded)g_lastWrittenJson.clear();
        g_nativeRuntimeLoaded=ready;g_nativeRuntimeStatus=ready?"CONNECTED - Olders Pipe":"runtime sem Pipe pronto; aguarde ou reinicie com a DLL nova";return;
    }
    g_nativeRuntimeLoaded = false;
    if (g_nativeAttemptedForPid) { g_nativeRuntimeStatus = "runtime not loaded - use Retry"; return; }
    if(!crome::olders_baseline::processMatches(pid)){g_nativeRuntimeStatus="Olders: fingerprint diferente da base restaurada";return;}
    g_nativeAttemptedForPid = true;
    g_lastNativeInjectAttempt = GetTickCount64();
    std::string err;
    if (InjectRuntimeDll(pid, g_baseDir / "CromeRuntime.dll", err)) g_nativeRuntimeStatus = "injected, waiting runtime";
    else g_nativeRuntimeStatus = "inject failed: " + err;
}

static void RetryNativeRuntime() { g_nativeAttemptedForPid = false; g_nativeRuntimeStatus = "retry requested"; }

static void WriteJsonIfChanged(const std::string& fileName, const nlohmann::json& j) {
    if (g_baseDir.empty()) return;
    const std::string text = j.dump(2) + "\n";
    auto& old = g_lastWrittenJson[fileName];
    if (old == text) return;
    if(g_nativeRuntimeLoaded&&crome::olders_pipe::configName(fileName)){
        nlohmann::json reply;if(crome::olders_pipe::request(g_nativeAttemptPid,{{"cmd","SET_CONFIG"},{"file",fileName},{"value",j}},reply)){old=text;return;}
        g_nativeRuntimeLoaded=false;g_nativeRuntimeStatus="Olders Pipe: envio falhou; modulos aguardam reconexao";return;
    }
    const auto dir = g_baseDir / "config";
    std::error_code ec; std::filesystem::create_directories(dir, ec);
    const auto dst = dir / fileName;
    const auto tmp = dir / (fileName + ".tmp");
    { std::ofstream f(tmp, std::ios::binary | std::ios::trunc); if (!f) return; f << text; }
    std::filesystem::remove(dst, ec); ec.clear();
    std::filesystem::rename(tmp, dst, ec);
    if (!ec) old = text;
}


static bool ReadJsonFile(const std::filesystem::path& path, nlohmann::json& out) {
    try { std::ifstream f(path, std::ios::binary); if (!f) return false; f >> out; return true; } catch(...) { return false; }
}
static bool WriteJsonFileAtomic(const std::filesystem::path& path, const nlohmann::json& j) {
    std::error_code ec; std::filesystem::create_directories(path.parent_path(), ec);
    const auto tmp = path.string() + ".tmp";
    { std::ofstream f(tmp, std::ios::binary | std::ios::trunc); if(!f) return false; f << j.dump(2) << "\n"; }
    std::filesystem::remove(path, ec); ec.clear(); std::filesystem::rename(tmp, path, ec); return !ec;
}
static std::uint64_t g_profileLoadGeneration = 0;

static std::filesystem::path DefaultUserProfilesDir() {
    const auto dir = g_baseDir / "profiles" / "user";
    std::error_code ec; std::filesystem::create_directories(dir, ec);
    return dir;
}

static bool PickProfilePath(bool saveDialog, std::filesystem::path& outPath) {
    char fileBuf[MAX_PATH]{};
    const auto initial = DefaultUserProfilesDir().string();
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hWnd;
    ofn.lpstrFilter = "Crome Profile (*.json)\0*.json\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = initial.c_str();
    ofn.lpstrDefExt = "json";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | (saveDialog ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    const BOOL ok = saveDialog ? GetSaveFileNameA(&ofn) : GetOpenFileNameA(&ofn);
    if (!ok || !fileBuf[0]) return false;
    outPath = std::filesystem::path(fileBuf);
    if (saveDialog && outPath.extension().empty()) outPath += ".json";
    return true;
}

static bool SaveCromeProfileBundle(const std::filesystem::path& path) {
    nlohmann::json p={{"schema","crome-user-profile-v2"},{"version","7.12"},{"files",nlohmann::json::object()}};
    const char* files[]={"bot.json","healing.json","targeting.json","loot.json","waypoints.json","walker_ids.json","fishing.json","tools.json"};
    for(auto* fn:files){ nlohmann::json j; if(ReadJsonFile(g_baseDir/"config"/fn,j)) p["files"][fn]=j; }
    p["meta"]={{"includesCavebot",true},{"includesLoot",true},{"includesTools",true},{"includesFishing",true}};
    return WriteJsonFileAtomic(path,p);
}
static bool LoadCromeProfileBundle(const std::filesystem::path& path) {
    nlohmann::json p; if(!ReadJsonFile(path,p)) return false;
    if(!p.contains("files")||!p["files"].is_object()) return false;
    for(auto it=p["files"].begin();it!=p["files"].end();++it) if(it.value().is_object()||it.value().is_array()) WriteJsonFileAtomic(g_baseDir/"config"/it.key(),it.value());
    g_lastWrittenJson.clear(); ++g_profileLoadGeneration; return true;
}

struct InspectResultUi {
    bool valid=false; int x=0,y=0,z=0; std::vector<int> ids; uint64_t lastRead=0;
};
static InspectResultUi g_inspect;
static uint32_t g_inspectSeq=1;

struct LookResultUi {
    bool valid=false; bool onGround=false; int id=0,x=0,y=0,z=0; uint64_t lastRead=0;
};
static LookResultUi g_lastLook;
static void RefreshLookResult() {
    const auto now=GetTickCount64(); if(now-g_lastLook.lastRead<150) return; g_lastLook.lastRead=now;

    // Ramonia V25+ feeds g_lastLook directly from the real Game::look capture.
    if(g_ramoniaBridgeReady.load() && g_ramoniaBridgeVersion.load()>=25)
        return;

    try {
        std::ifstream f(g_baseDir / "config" / "look_result.json"); if(!f) return;
        nlohmann::json j; f>>j;
        g_lastLook.valid=j.value("valid",false); g_lastLook.id=j.value("id",0); g_lastLook.onGround=j.value("onGround",false);
        g_lastLook.x=j.value("x",0); g_lastLook.y=j.value("y",0); g_lastLook.z=j.value("z",0);
    } catch(...) {}
}

static void RefreshInspectResult() {
    const auto now=GetTickCount64(); if(now-g_inspect.lastRead<200) return; g_inspect.lastRead=now;
    try {
        std::ifstream f(g_baseDir / "config" / "inspect_result.json"); if(!f) return;
        nlohmann::json j; f>>j; g_inspect.valid=j.value("valid",false); g_inspect.x=j.value("x",0); g_inspect.y=j.value("y",0); g_inspect.z=j.value("z",0); g_inspect.ids.clear();
        if(j.contains("ids")&&j["ids"].is_array()) for(auto& v:j["ids"]) if(v.is_number_integer()) g_inspect.ids.push_back(v.get<int>());
    } catch(...) {}
}
static void RequestInspect(int x,int y,int z) {
    nlohmann::json j={{"seq",++g_inspectSeq},{"x",x},{"y",y},{"z",z}}; WriteJsonIfChanged("inspect_request.json",j);
}
struct WalkerIdCatalog {
    nlohmann::json data = nlohmann::json::object();
    bool loaded = false;
};
static WalkerIdCatalog g_walkerIds;

static std::filesystem::path WalkerIdsPath() { return g_baseDir / "config" / "walker_ids.json"; }
static void EnsureWalkerIdCatalog() {
    if (g_walkerIds.loaded) return;
    g_walkerIds.loaded = true;
    try { std::ifstream f(WalkerIdsPath()); if (f) f >> g_walkerIds.data; } catch(...) {}
    if (!g_walkerIds.data.is_object()) g_walkerIds.data = nlohmann::json::object();
    const char* keys[] = {"ladder","rope","shovel","pick","minePick","machete","pathable","nonPathable","openHoles","clearBeforeUse","counter","depot","food","rock","corpseBag","fishingRod","fishingWater","changeGold"};
    for (auto k: keys) if (!g_walkerIds.data.contains(k) || !g_walkerIds.data[k].is_array()) g_walkerIds.data[k] = nlohmann::json::array();
}
static void SaveWalkerIdCatalog() {
    EnsureWalkerIdCatalog();
    std::error_code ec; std::filesystem::create_directories(g_baseDir / "config", ec);
    std::ofstream o(WalkerIdsPath(), std::ios::trunc); if (o) o << g_walkerIds.data.dump(2) << "\n";
}
static bool CatalogHas(const char* key, int id) {
    EnsureWalkerIdCatalog(); if (id<=0 || !g_walkerIds.data.contains(key)) return false;
    for (auto& v: g_walkerIds.data[key]) if (v.is_number_integer() && v.get<int>()==id) return true;
    return false;
}
static int CatalogFirst(const char* key, int fallback=0) {
    EnsureWalkerIdCatalog(); if (!g_walkerIds.data.contains(key)) return fallback;
    for (auto& v: g_walkerIds.data[key]) if (v.is_number_integer()) return v.get<int>();
    return fallback;
}
static std::vector<int> CatalogIds(const char* key) {
    EnsureWalkerIdCatalog();
    std::vector<int> out;
    if (!g_walkerIds.data.contains(key) || !g_walkerIds.data[key].is_array())
        return out;
    for (auto& v : g_walkerIds.data[key]) {
        if (!v.is_number_integer()) continue;
        const int id=v.get<int>();
        if (id>0 && std::find(out.begin(),out.end(),id)==out.end())
            out.push_back(id);
    }
    return out;
}
static void SaveItemClass(const char* key,int id) {
    if(id<=0) return; EnsureWalkerIdCatalog();
    if(!g_walkerIds.data.contains(key)||!g_walkerIds.data[key].is_array()) g_walkerIds.data[key]=nlohmann::json::array();
    for(auto& v:g_walkerIds.data[key]) if(v.is_number_integer()&&v.get<int>()==id) return;
    g_walkerIds.data[key].push_back(id); SaveWalkerIdCatalog();
}
static std::string CatalogCsv(const char* key) {
    EnsureWalkerIdCatalog(); std::string out;
    if (!g_walkerIds.data.contains(key)) return out;
    for (auto& v: g_walkerIds.data[key]) if (v.is_number_integer()) { if(!out.empty()) out += ","; out += std::to_string(v.get<int>()); }
    return out;
}
static void SetCatalogCsv(const char* key, const char* text) {
    EnsureWalkerIdCatalog(); nlohmann::json a=nlohmann::json::array();
    std::istringstream ss(text?text:""); std::string p;
    while(std::getline(ss,p,',')) { try { int id=std::stoi(p); if(id>0 && std::find(a.begin(),a.end(),id)==a.end()) a.push_back(id); } catch(...) {} }
    g_walkerIds.data[key]=a; SaveWalkerIdCatalog();
}

static std::vector<std::string> SplitCsv(const char* text) {
    std::vector<std::string> out; std::string cur;
    std::istringstream ss(text ? text : "");
    while (std::getline(ss, cur, ',')) {
        auto b = cur.find_first_not_of(" \t\r\n"), e = cur.find_last_not_of(" \t\r\n");
        if (b != std::string::npos) out.push_back(cur.substr(b, e - b + 1));
    }
    return out;
}

static void RefreshRuntime() {
    if (!g_session) return;
    g_session->refresh();
    const auto now = GetTickCount64();
    if (g_session->runtime().localPlayer) {
        if (!g_playerReader) g_playerReader = std::make_unique<crome::ProfileBackedPlayerReader>(g_session->memory(), g_session->runtime());
        if (now - g_lastPlayerRead >= 100) {
            g_livePlayer = g_playerReader->read();
            g_lastPlayerRead = now;
        }
    } else {
        g_playerReader.reset();
        g_livePlayer = {};
    }
}

struct DetectedClient {
    DWORD pid = 0;
    std::string exe;
    std::string profile;
};

static DWORD FindProcessByName(const wchar_t* exeName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exeName) == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

static DetectedClient DetectKnownClient() {
    struct Known { const wchar_t* exeW; const char* exeA; const char* profile; };
    static const Known known[] = {
        {L"Olders_OpenGL.exe", "Olders_OpenGL.exe", "6AA9FA57_89D3D35243890EA2"},
        {L"TibiaScape.exe", "TibiaScape.exe", "6A82C258_15F49CEEE51C6AD"},
        {L"Classico-1790708814.exe", "Classico-1790708814.exe", "6AA3019A_602E746BAE706D6C"},
    };
    for (const auto& k : known) {
        DWORD pid = FindProcessByName(k.exeW);
        if (pid) return {pid, k.exeA, k.profile};
    }
    return {};
}

struct Theme {
    ImVec4 bg;
    ImVec4 panel;
    ImVec4 panel2;
    ImVec4 header;
    ImVec4 border;
    ImVec4 text;
    ImVec4 muted;
    ImVec4 input;
    ImVec4 blue;
    ImVec4 green;
};

static Theme DarkTheme() {
    return {
        {0.035f,0.055f,0.075f,1}, {0.055f,0.090f,0.120f,1}, {0.065f,0.105f,0.140f,1},
        {0.075f,0.115f,0.155f,1}, {0.16f,0.25f,0.32f,1}, {0.92f,0.95f,0.98f,1},
        {0.62f,0.70f,0.78f,1}, {0.035f,0.075f,0.105f,1}, {0.08f,0.50f,0.95f,1}, {0.18f,0.85f,0.38f,1}
    };
}

static Theme LightTheme() {
    return {
        {0.94f,0.96f,0.98f,1}, {1.0f,1.0f,1.0f,1}, {0.97f,0.98f,0.995f,1},
        {0.92f,0.95f,0.98f,1}, {0.76f,0.82f,0.88f,1}, {0.08f,0.12f,0.17f,1},
        {0.36f,0.43f,0.50f,1}, {0.93f,0.96f,0.985f,1}, {0.05f,0.44f,0.90f,1}, {0.10f,0.68f,0.28f,1}
    };
}

static ImU32 Col(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }

static void ApplyTheme(const Theme& t) {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowPadding = ImVec2(4, 4);
    s.FramePadding = ImVec2(3, 2);
    s.ItemSpacing = ImVec2(3, 3);
    s.ItemInnerSpacing = ImVec2(4, 2);
    s.WindowRounding = 0;
    s.ChildRounding = 3;
    s.FrameRounding = 2;
    s.PopupRounding = 5;
    s.ScrollbarRounding = 5;
    s.GrabRounding = 4;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 1;
    s.FrameBorderSize = 1;
    s.ScrollbarSize = 8;

    s.Colors[ImGuiCol_Text] = t.text;
    s.Colors[ImGuiCol_TextDisabled] = t.muted;
    s.Colors[ImGuiCol_WindowBg] = t.bg;
    s.Colors[ImGuiCol_ChildBg] = t.panel;
    s.Colors[ImGuiCol_PopupBg] = t.panel;
    s.Colors[ImGuiCol_TitleBg] = t.header;
    s.Colors[ImGuiCol_TitleBgActive] = t.header;
    s.Colors[ImGuiCol_TitleBgCollapsed] = t.panel2;
    s.Colors[ImGuiCol_Border] = t.border;
    s.Colors[ImGuiCol_FrameBg] = t.input;
    s.Colors[ImGuiCol_FrameBgHovered] = ImVec4(t.input.x+0.025f,t.input.y+0.025f,t.input.z+0.025f,1);
    s.Colors[ImGuiCol_FrameBgActive] = ImVec4(t.input.x+0.05f,t.input.y+0.05f,t.input.z+0.05f,1);
    s.Colors[ImGuiCol_CheckMark] = t.blue;
    s.Colors[ImGuiCol_Button] = t.blue;
    s.Colors[ImGuiCol_ButtonHovered] = ImVec4(std::min(1.f,t.blue.x+0.08f),std::min(1.f,t.blue.y+0.08f),std::min(1.f,t.blue.z+0.08f),1);
    s.Colors[ImGuiCol_ButtonActive] = ImVec4(std::max(0.f,t.blue.x-0.05f),std::max(0.f,t.blue.y-0.05f),std::max(0.f,t.blue.z-0.05f),1);
    s.Colors[ImGuiCol_Header] = t.header;
    s.Colors[ImGuiCol_HeaderHovered] = t.panel2;
    s.Colors[ImGuiCol_HeaderActive] = t.panel2;
    s.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0,0,0,0.08f);
    s.Colors[ImGuiCol_ScrollbarGrab] = t.border;
    s.Colors[ImGuiCol_TableHeaderBg] = t.header;
    s.Colors[ImGuiCol_TableBorderStrong] = t.border;
    s.Colors[ImGuiCol_TableBorderLight] = t.border;
    s.Colors[ImGuiCol_TableRowBg] = ImVec4(0,0,0,0);
    s.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(t.panel2.x,t.panel2.y,t.panel2.z,0.18f);
    s.Colors[ImGuiCol_Separator] = t.border;
    s.Colors[ImGuiCol_SeparatorHovered] = t.blue;
    s.Colors[ImGuiCol_SeparatorActive] = t.blue;
    s.Colors[ImGuiCol_SliderGrab] = t.blue;
    s.Colors[ImGuiCol_SliderGrabActive] = t.blue;
}

static bool Toggle(const char* id, bool* value, const Theme& t) {
    ImGui::PushID(id);
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = 18.0f;
    const float w = 34.0f;
    ImGui::InvisibleButton("##toggle", ImVec2(w,h));
    if (ImGui::IsItemClicked()) *value = !*value;
    ImDrawList* d = ImGui::GetWindowDrawList();
    ImU32 bg = *value ? Col(t.blue) : Col(ImVec4(t.muted.x,t.muted.y,t.muted.z,0.45f));
    d->AddRectFilled(p, ImVec2(p.x+w,p.y+h), bg, h*0.5f);
    float cx = *value ? p.x+w-h*0.5f : p.x+h*0.5f;
    d->AddCircleFilled(ImVec2(cx,p.y+h*0.5f), h*0.34f, IM_COL32(245,248,252,255));
    ImGui::PopID();
    return *value;
}

enum class CardIcon { Heart, Users, Rune, Pin, Gear, Coins, Bell, Keyboard, Info };

static void DrawCardIcon(ImDrawList* d, CardIcon icon, ImVec2 c, ImU32 color) {
    const float s = 7.0f;
    switch (icon) {
        case CardIcon::Heart: {
            d->AddCircleFilled(ImVec2(c.x-3.2f,c.y-2.0f),3.7f,color);
            d->AddCircleFilled(ImVec2(c.x+3.2f,c.y-2.0f),3.7f,color);
            ImVec2 pts[3]={{c.x-6.3f,c.y-0.7f},{c.x+6.3f,c.y-0.7f},{c.x,c.y+7.2f}};
            d->AddConvexPolyFilled(pts,3,color);
            break;
        }
        case CardIcon::Users: {
            d->AddCircleFilled(ImVec2(c.x-3.5f,c.y-3.0f),3.0f,color);
            d->AddCircleFilled(ImVec2(c.x+3.8f,c.y-2.2f),2.6f,color);
            d->AddRectFilled(ImVec2(c.x-8.0f,c.y+1.0f),ImVec2(c.x+1.0f,c.y+6.3f),color,3.0f);
            d->AddRectFilled(ImVec2(c.x+0.5f,c.y+1.3f),ImVec2(c.x+8.0f,c.y+5.8f),color,2.6f);
            break;
        }
        case CardIcon::Rune: {
            ImVec2 p[8];
            for(int i=0;i<8;i++) { float a=3.1415926f*0.25f*i; float r=(i%2)?3.2f:7.0f; p[i]=ImVec2(c.x+cosf(a)*r,c.y+sinf(a)*r); }
            d->AddConvexPolyFilled(p,8,color);
            d->AddCircleFilled(c,1.7f,IM_COL32(255,255,255,220));
            break;
        }
        case CardIcon::Pin: {
            d->AddCircleFilled(ImVec2(c.x,c.y-2.5f),6.0f,color);
            ImVec2 pts[3]={{c.x-4.4f,c.y+1.5f},{c.x+4.4f,c.y+1.5f},{c.x,c.y+8.0f}};
            d->AddConvexPolyFilled(pts,3,color);
            d->AddCircleFilled(ImVec2(c.x,c.y-2.5f),2.0f,IM_COL32(235,245,255,255));
            break;
        }
        case CardIcon::Gear: {
            d->AddCircle(c,5.0f,color,16,2.2f); d->AddCircleFilled(c,1.9f,color);
            for(int i=0;i<8;i++){ float a=3.1415926f*0.25f*i; ImVec2 a0(c.x+cosf(a)*5.3f,c.y+sinf(a)*5.3f), a1(c.x+cosf(a)*8.0f,c.y+sinf(a)*8.0f); d->AddLine(a0,a1,color,2.5f); }
            break;
        }
        case CardIcon::Coins: {
            d->AddEllipseFilled(ImVec2(c.x,c.y-4.0f),ImVec2(7.0f,3.0f),color);
            d->AddRectFilled(ImVec2(c.x-7.0f,c.y-4.0f),ImVec2(c.x+7.0f,c.y+4.0f),color);
            d->AddEllipseFilled(ImVec2(c.x,c.y+4.0f),ImVec2(7.0f,3.0f),color);
            d->AddLine(ImVec2(c.x-6.5f,c.y),ImVec2(c.x+6.5f,c.y),IM_COL32(255,255,255,120),1.0f);
            break;
        }
        case CardIcon::Bell: {
            d->AddCircleFilled(ImVec2(c.x,c.y+5.5f),1.8f,color);
            d->AddRectFilled(ImVec2(c.x-5.0f,c.y-1.0f),ImVec2(c.x+5.0f,c.y+4.3f),color,2.0f);
            d->AddCircleFilled(ImVec2(c.x,c.y-1.0f),5.0f,color);
            d->AddRectFilled(ImVec2(c.x-6.5f,c.y+3.0f),ImVec2(c.x+6.5f,c.y+5.0f),color,1.0f);
            break;
        }
        case CardIcon::Keyboard: {
            d->AddRect(ImVec2(c.x-8.0f,c.y-5.0f),ImVec2(c.x+8.0f,c.y+5.0f),color,2.0f,0,1.7f);
            for(int y=-2;y<=2;y+=4) for(int x=-5;x<=5;x+=4) d->AddRectFilled(ImVec2(c.x+x-1,c.y+y-1),ImVec2(c.x+x+1,c.y+y+1),color,0.5f);
            break;
        }
        case CardIcon::Info: {
            d->AddCircle(c,7.0f,color,18,2.0f);
            d->AddCircleFilled(ImVec2(c.x,c.y-3.7f),1.2f,color);
            d->AddRectFilled(ImVec2(c.x-1.0f,c.y-0.5f),ImVec2(c.x+1.0f,c.y+4.8f),color,1.0f);
            break;
        }
    }
}

static void CardBegin(const char* id, const char* title, CardIcon icon, ImVec4 accent, float height, const Theme& t) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, t.panel);
    ImGui::PushStyleColor(ImGuiCol_Border, t.border);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 7.0f);
    if (g_cardCanvasWidth > 0)
        ImGui::SetNextWindowContentSize(ImVec2(g_cardCanvasWidth, 0));
    ImGui::BeginChild(id, ImVec2(0,height), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    ImDrawList* d = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    ImVec2 ws = ImGui::GetWindowSize();
    float hh = 23.0f;
    ImVec4 hb = t.header;
    hb.x = std::min(1.0f, hb.x + accent.x*0.035f);
    hb.y = std::min(1.0f, hb.y + accent.y*0.035f);
    hb.z = std::min(1.0f, hb.z + accent.z*0.035f);
    d->AddRectFilled(wp, ImVec2(wp.x+ws.x,wp.y+hh), Col(hb), 8.0f, ImDrawFlags_RoundCornersTop);
    DrawCardIcon(d, icon, ImVec2(wp.x+15, wp.y+hh*0.5f), Col(accent));
    ImGui::SetCursorPos(ImVec2(28, 3));
    ImGui::PushFont(g_fontBold ? g_fontBold : g_font);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    ImGui::SetCursorPos(ImVec2(ws.x-20, 3));
    ImGui::TextColored(t.muted, "?");
    ImGui::SetCursorPos(ImVec2(8, hh+4));
}

static void CardEnd() {
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

static void SmallInput(const char* id, char* buf, size_t cap, float width) {
    ImGui::SetNextItemWidth(width);
    ImGui::InputText(id, buf, cap);
}

static void SmallInt(const char* id, int* v, float width) {
    ImGui::SetNextItemWidth(width);
    ImGui::InputInt(id, v, 0, 0);
}


static void DrawHealing(const Theme& t) {
    static bool healingMaster = false;
    static char spells[3][32] = {"exura", "exura gran", "exura vita"};
    static int hp[3] = {90, 70, 40};
    static int mana[3] = {25, 40, 80}; // absolute mana, like Rifbot
    static bool enabled[3] = {false, false, false};

    static bool uhEnabled = false;
    static int uhHp = 50;
    static int uhId = 2273;
    static bool manaFluidEnabled = false;
    static int manaFluidMp = 50;
    static int manaFluidId = 15552;
    static bool lifeFluidEnabled = false;
    static int lifeFluidHp = 50;
    static int lifeFluidId = 0;
    static std::uint64_t seenProfileLoadGeneration=~0ull;

    if(seenProfileLoadGeneration!=g_profileLoadGeneration){
        seenProfileLoadGeneration=g_profileLoadGeneration;
        nlohmann::json hj;
        if(ReadJsonFile(g_baseDir/"config"/"healing.json",hj)){
            healingMaster=hj.value("enabled",false);
            if(hj.contains("rules")&&hj["rules"].is_array()){
                for(auto& r:hj["rules"]){
                    if(!r.is_object()) continue;
                    const auto name=r.value("name",std::string());
                    if(name=="light_heal"||name=="mid_heal"||name=="high_heal"){
                        const int i=name=="light_heal"?0:(name=="mid_heal"?1:2);
                        enabled[i]=r.value("enabled",false);
                        hp[i]=r.value("below_or_equal",hp[i]);
                        mana[i]=r.value("min_mana",mana[i]);
                        const auto sp=r.value("spell",std::string(spells[i]));
                        std::snprintf(spells[i],sizeof(spells[i]),"%s",sp.c_str());
                    } else if(name=="uh"){
                        uhEnabled=r.value("enabled",false);
                        uhHp=r.value("below_or_equal",uhHp);
                        uhId=r.value("item_id",uhId);
                    } else if(name=="mana_fluid"){
                        manaFluidEnabled=r.value("enabled",false);
                        manaFluidMp=r.value("below_or_equal",manaFluidMp);
                        const int configuredManaFluidId=r.value("item_id",manaFluidId);
                        if(configuredManaFluidId>0)
                            manaFluidId=configuredManaFluidId;
                    } else if(name=="life_fluid"){
                        lifeFluidEnabled=r.value("enabled",false);
                        lifeFluidHp=r.value("below_or_equal",lifeFluidHp);
                        lifeFluidId=r.value("item_id",lifeFluidId);
                    }
                }
            }
        }
    }

    const bool healingRuntimeReady=HealingRuntimeReady();
    const bool ramoniaSpellReady=
        g_ramoniaBridgeReady.load() && g_ramoniaBridgeVersion.load()>=23;

    CardBegin("healing", "Healing", CardIcon::Heart,
              ImVec4(1.0f,0.20f,0.36f,1), 164, t);

    if(!healingRuntimeReady) ImGui::BeginDisabled();
    ImGui::Checkbox("##healing_master",&healingMaster);
    if(!healingRuntimeReady) ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::TextUnformatted("Healing");
    ImGui::SameLine(92);
    if(g_nativeRuntimeLoaded && g_livePlayer.valid) {
        ImGui::TextDisabled(
            "HP %.0f%%  Mana %.0f",
            g_livePlayer.healthPercent(),
            g_livePlayer.mana);
    } else if(ramoniaSpellReady) {
        ImGui::TextDisabled("%s",RamoniaHealingStatusText().c_str());
    } else {
        ImGui::TextDisabled("aguardando backend");
    }

    const char* names[3]={"Light","Mid","High"};
    if(!healingRuntimeReady) ImGui::BeginDisabled();
    if(ImGui::BeginTable("heal_compact",5,ImGuiTableFlags_SizingStretchProp)){
        ImGui::TableSetupColumn("##name",0,0.62f);
        ImGui::TableSetupColumn("##hp",0,0.62f);
        ImGui::TableSetupColumn("##mana",0,0.86f);
        ImGui::TableSetupColumn("##spell",0,2.10f);
        ImGui::TableSetupColumn("##on",0,0.35f);
        for(int i=0;i<3;++i){
            ImGui::TableNextRow();
            ImGui::PushID(i);
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(names[i]);
            ImGui::TableSetColumnIndex(1);
            SmallInt("##hpv",&hp[i],-1); ImGui::SameLine(0,2); ImGui::TextDisabled("%%");
            ImGui::TableSetColumnIndex(2);
            SmallInt("##manav",&mana[i],-1); ImGui::SameLine(0,2); ImGui::TextDisabled("Mana");
            ImGui::TableSetColumnIndex(3); SmallInput("##spellv",spells[i],32,-1);
            ImGui::TableSetColumnIndex(4); ImGui::Checkbox("##onv",&enabled[i]);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if(!healingRuntimeReady) ImGui::EndDisabled();

    ImGui::Separator();

    const bool ramoniaItemHealReady=
        g_ramoniaBridgeReady.load() && g_ramoniaBridgeVersion.load()>=24;
    const bool itemHealReady=g_nativeRuntimeLoaded || ramoniaItemHealReady;

    if(ImGui::BeginTable("heal_items",5,ImGuiTableFlags_SizingStretchProp)){
        ImGui::TableSetupColumn("##heal_item_name",0,0.95f);
        ImGui::TableSetupColumn("##heal_item_trigger",0,0.72f);
        ImGui::TableSetupColumn("##heal_item_kind",0,0.45f);
        ImGui::TableSetupColumn("##heal_item_id",0,1.15f);
        ImGui::TableSetupColumn("##heal_item_on",0,0.35f);

        auto itemRow=[&](const char* label,
                         bool* on,
                         int* trigger,
                         const char* triggerLabel,
                         int* itemId,
                         const char* idSuffix){
            ImGui::TableNextRow();
            ImGui::PushID(label);

            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(label);

            ImGui::TableSetColumnIndex(1);
            SmallInt("##trigger",trigger,-1);

            ImGui::TableSetColumnIndex(2);
            ImGui::TextDisabled("%s",triggerLabel);

            ImGui::TableSetColumnIndex(3);
            SmallInt(idSuffix,itemId,-1);

            ImGui::TableSetColumnIndex(4);
            if(!itemHealReady) ImGui::BeginDisabled();
            ImGui::Checkbox("##enabled",on);
            if(!itemHealReady) ImGui::EndDisabled();

            ImGui::PopID();
        };

        itemRow("UH",&uhEnabled,&uhHp,"HP%",&uhId,"##uh_item_id");
        itemRow("Mana Fluid",&manaFluidEnabled,&manaFluidMp,"MP%",&manaFluidId,"##mf_item_id");
        itemRow("Life Fluid",&lifeFluidEnabled,&lifeFluidHp,"HP%",&lifeFluidId,"##lf_item_id");

        ImGui::EndTable();
    }

    if(healingMaster && g_nativeRuntimeLoaded && g_livePlayer.valid){
        const double php=g_livePlayer.healthPercent();
        const double pmana=g_livePlayer.mana;
        const double pmp=g_livePlayer.manaPercent();
        const char* trigger="none";
        if(uhEnabled && uhId>0 && php<=uhHp) trigger="UH";
        else if(manaFluidEnabled && manaFluidId>0 && pmp<=manaFluidMp) trigger="MF";
        else if(lifeFluidEnabled && lifeFluidId>0 && php<=lifeFluidHp) trigger="LF";
        else {
            for(int i=2;i>=0;--i)
                if(enabled[i] && php<=hp[i] && pmana>=mana[i]) {
                    trigger=names[i];
                    break;
                }
        }
        ImGui::TextDisabled("Rule trigger: %s",trigger);
    }

    const int spellPriority[3]={600,700,800};
    const char* ruleNames[3]={"light_heal","mid_heal","high_heal"};

    RamoniaHealingConfigUi ramHeal;
    ramHeal.enabled=ramoniaSpellReady && healingMaster;
    for(int i=0;i<3;++i){
        RamoniaHealingSpellRuleUi rule;
        rule.enabled=enabled[i];
        rule.hpPercent=std::clamp(hp[i],0,100);
        rule.minMana=std::max(0,mana[i]);
        rule.spell=spells[i];
        rule.cooldownMs=850;
        rule.priority=spellPriority[i];
        ramHeal.spells.push_back(std::move(rule));
    }

    auto addItemRule=[&](const char* name,
                         bool on,
                         bool manaTrigger,
                         int trigger,
                         int itemId,
                         int priority){
        RamoniaHealingItemRuleUi rule;
        rule.enabled=on;
        rule.manaTrigger=manaTrigger;
        rule.triggerPercent=std::clamp(trigger,0,100);
        rule.itemId=itemId>0 ? static_cast<std::uint32_t>(itemId) : 0u;
        rule.cooldownMs=650;
        rule.priority=priority;
        rule.name=name;
        ramHeal.items.push_back(std::move(rule));
    };

    if(ramoniaItemHealReady){
        addItemRule("UH",uhEnabled,false,uhHp,uhId,1000);
        addItemRule("Life Fluid",lifeFluidEnabled,false,lifeFluidHp,lifeFluidId,900);
        addItemRule("Mana Fluid",manaFluidEnabled,true,manaFluidMp,manaFluidId,500);
    }

    SetRamoniaHealingConfig(ramHeal);

    nlohmann::json hcfg;
    hcfg["enabled"]=healingRuntimeReady && healingMaster;
    hcfg["poll_ms"]=100;
    hcfg["ramonia_spell_backend"]=ramoniaSpellReady ? "bridge_v23_talkChannel" : "unavailable";
    hcfg["ramonia_item_backend"]=ramoniaItemHealReady ? "bridge_v24_useWithSelf" : "unavailable";
    hcfg["rules"]=nlohmann::json::array();

    for(int i=0;i<3;++i) hcfg["rules"].push_back({
        {"name",ruleNames[i]},
        {"enabled",enabled[i]},
        {"trigger","health_percent"},
        {"below_or_equal",hp[i]},
        {"min_mana",mana[i]},
        {"action","spell"},
        {"spell",spells[i]},
        {"cooldown_ms",850},
        {"priority",spellPriority[i]}
    });

    hcfg["rules"].push_back({
        {"name","uh"},{"enabled",itemHealReady&&uhEnabled},
        {"trigger","health_percent"},{"below_or_equal",uhHp},{"min_mana",0},
        {"action","item_on_self"},{"item_id",uhId},{"cooldown_ms",650},{"priority",1000}
    });
    hcfg["rules"].push_back({
        {"name","life_fluid"},{"enabled",itemHealReady&&lifeFluidEnabled},
        {"trigger","health_percent"},{"below_or_equal",lifeFluidHp},{"min_mana",0},
        {"action","item_on_self"},{"item_id",lifeFluidId},{"cooldown_ms",650},{"priority",900}
    });
    hcfg["rules"].push_back({
        {"name","mana_fluid"},{"enabled",itemHealReady&&manaFluidEnabled},
        {"trigger","mana_percent"},{"below_or_equal",manaFluidMp},{"min_mana",0},
        {"action","item_on_self"},{"item_id",manaFluidId},{"cooldown_ms",650},{"priority",500}
    });

    WriteJsonIfChanged("healing.json",hcfg);
    CardEnd();
}

static void DrawFriends(const Theme& t) {
    static int friendHp=70, myHp=80;
    static bool follow=false, uh=false, party=false;
    static char list[128]="Friend1, Friend2, Friend3";
    static std::uint64_t seenProfileLoadGeneration=~0ull;

    if(seenProfileLoadGeneration!=g_profileLoadGeneration){
        seenProfileLoadGeneration=g_profileLoadGeneration;
        nlohmann::json fj;
        if(ReadJsonFile(g_baseDir/"config"/"friends.json",fj)){
            friendHp=fj.value("friend_hp",friendHp);
            myHp=fj.value("your_hp",myHp);
            follow=fj.value("follow_friends",follow);
            uh=fj.value("uh_friends",uh);
            party=fj.value("party_mode",party);
            const auto csv=fj.value("friends_csv",std::string(list));
            std::snprintf(list,sizeof(list),"%s",csv.c_str());
        }
    }

    CardBegin("friends","Friends / Party",CardIcon::Users,
              ImVec4(0.12f,0.62f,1.0f,1),100,t);

    ImGui::TextUnformatted("Friend HP:");
    ImGui::SameLine();
    SmallInt("##fhp",&friendHp,58);
    ImGui::SameLine();
    ImGui::TextDisabled("%%");

    ImGui::SameLine(190);
    ImGui::TextUnformatted("Your HP:");
    ImGui::SameLine();
    SmallInt("##mhp",&myHp,58);
    ImGui::SameLine();
    ImGui::TextDisabled("%%");

    ImGui::Checkbox("Follow Friends",&follow);
    ImGui::SameLine(145);
    ImGui::Checkbox("UH Friends",&uh);
    ImGui::SameLine(270);
    ImGui::Checkbox("Party Mode",&party);

    ImGui::TextDisabled("Friends:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##flist",list,sizeof(list));
    ImGui::TextDisabled("Prep: exura sio / UH friend / follow aguardando teste live");

    nlohmann::json fcfg={
        {"friend_hp",std::clamp(friendHp,0,100)},
        {"your_hp",std::clamp(myHp,0,100)},
        {"follow_friends",follow},
        {"uh_friends",uh},
        {"party_mode",party},
        {"friends_csv",std::string(list)},
        {"backend","prepared_only"},
        {"future_follow_action","follow_friend_by_name"},
        {"future_uh_action","useWith_uh_on_friend"},
        {"future_sio_action","exura sio \"{name}\""},
        {"party_semantic","green_shield_or_green_skull_pending_runtime_validation"}
    };
    WriteJsonIfChanged("friends.json",fcfg);

    CardEnd();
}

static void DrawRune(const Theme& t) {
    static bool en=false, relog=false, logout=false, noblank=false;
    static int mana=70,soul=5,relogMin=6;
    static char spell[32]="exura";
    static int mode=0;
    static int cooldownMs=1000;
    static std::uint64_t seenProfileLoadGeneration=~0ull;

    const char* modes[]={"Logout","Stay Online","Pause"};

    if(seenProfileLoadGeneration!=g_profileLoadGeneration){
        seenProfileLoadGeneration=g_profileLoadGeneration;
        nlohmann::json rj;
        if(ReadJsonFile(g_baseDir/"config"/"rune.json",rj)){
            en=rj.value("enabled",en);
            mana=rj.value("mana_percent",mana);
            soul=rj.value("soul_min",soul);
            cooldownMs=rj.value("cooldown_ms",cooldownMs);
            relog=rj.value("relog_after",relog);
            relogMin=rj.value("relog_minutes",relogMin);
            logout=rj.value("player_logout",logout);
            noblank=rj.value("no_blank_runes",noblank);
            mode=rj.value("mode",mode);
            const auto sp=rj.value("spell",std::string(spell));
            std::snprintf(spell,sizeof(spell),"%s",sp.c_str());
        }
    }

    const bool runeRuntimeReady=RuneRuntimeReady();
    const bool ramoniaRuneReady=
        g_ramoniaBridgeReady.load() && g_ramoniaBridgeVersion.load()>=26;

    const float runeCardHeight =
        ramoniaRuneReady ? 90.0f : 104.0f;
    CardBegin("rune","Rune Maker / Trainer",CardIcon::Rune,
              ImVec4(0.72f,0.35f,1.0f,1),runeCardHeight,t);

    if(!runeRuntimeReady) ImGui::BeginDisabled();
    ImGui::Checkbox("Enabled",&en);
    ImGui::SameLine(165);
    ImGui::TextUnformatted("Spell:");
    ImGui::SameLine();
    SmallInput("##rspell",spell,sizeof(spell),170);
    if(!runeRuntimeReady) ImGui::EndDisabled();

    if(!runeRuntimeReady) ImGui::BeginDisabled();
    ImGui::TextUnformatted("Mana:");
    ImGui::SameLine();
    SmallInt("##rmana",&mana,60);
    ImGui::SameLine();
    ImGui::TextDisabled("%%");

    ImGui::SameLine(190);
    ImGui::TextUnformatted("Soul Min:");
    ImGui::SameLine();
    SmallInt("##soul",&soul,60);
    if(!runeRuntimeReady) ImGui::EndDisabled();

    if(ramoniaRuneReady){
        ImGui::TextDisabled("%s",RamoniaRuneStatusText().c_str());
    } else {
        // Advanced legacy automation remains native-only for now.
        if(!g_nativeRuntimeLoaded) ImGui::BeginDisabled();
        ImGui::Checkbox("Re-Login After",&relog);
        ImGui::SameLine();
        SmallInt("##relogin",&relogMin,55);
        ImGui::SameLine();
        ImGui::TextDisabled("min");
        ImGui::Checkbox("Player Logout",&logout);
        ImGui::SameLine(135);
        ImGui::Checkbox("No Blank Runes",&noblank);
        ImGui::SameLine(270);
        ImGui::SetNextItemWidth(105);
        ImGui::Combo("##mode",&mode,modes,3);
        if(!g_nativeRuntimeLoaded) ImGui::EndDisabled();
    }

    RamoniaRuneConfigUi ramRune;
    ramRune.enabled=ramoniaRuneReady && en;
    ramRune.manaPercent=std::clamp(mana,0,100);
    ramRune.soulMin=std::max(0,soul);
    ramRune.spell=spell;
    ramRune.cooldownMs=static_cast<std::uint32_t>(
        std::clamp(cooldownMs,300,10000));
    SetRamoniaRuneConfig(ramRune);

    nlohmann::json rcfg={
        {"enabled",runeRuntimeReady&&en},
        {"spell",std::string(spell)},
        {"mana_percent",std::clamp(mana,0,100)},
        {"soul_min",std::max(0,soul)},
        {"soul_mode","auto_if_available"},
        {"cooldown_ms",std::clamp(cooldownMs,300,10000)},
        {"ramonia_backend",ramoniaRuneReady ? "bridge_v26_get_player_plus_say" : "unavailable"},
        {"relog_after",g_nativeRuntimeLoaded&&relog},
        {"relog_minutes",std::max(0,relogMin)},
        {"player_logout",g_nativeRuntimeLoaded&&logout},
        {"no_blank_runes",g_nativeRuntimeLoaded&&noblank},
        {"mode",mode}
    };
    WriteJsonIfChanged("rune.json",rcfg);

    CardEnd();
}

static bool ParseWaypointLine(const std::string& line, int& type, int& x, int& y, int& z, int& item, int& delay, std::string& text, int* targetId=nullptr) {
    x=y=z=item=0; delay=1000; text.clear();
    if (targetId) *targetId=0;
    int groundTarget{};
    if (std::sscanf(line.c_str(),"Rope %d @ %d, %d, %d target %d",&item,&x,&y,&z,&groundTarget)==5) { type=3; if(targetId) *targetId=groundTarget; return true; }
    if (std::sscanf(line.c_str(),"Shovel %d @ %d, %d, %d target %d",&item,&x,&y,&z,&groundTarget)==5) { type=4; if(targetId) *targetId=groundTarget; return true; }
    if (std::sscanf(line.c_str(),"Walk %d, %d, %d",&x,&y,&z)==3) { type=0; return true; }
    if (std::sscanf(line.c_str(),"Stand %d, %d, %d",&x,&y,&z)==3) { type=1; return true; }
    if (std::sscanf(line.c_str(),"Use %d @ %d, %d, %d",&item,&x,&y,&z)==4) { type=2; return true; }
    if (std::sscanf(line.c_str(),"Rope %d @ %d, %d, %d",&item,&x,&y,&z)==4) { type=3; return true; }
    if (std::sscanf(line.c_str(),"Shovel %d @ %d, %d, %d",&item,&x,&y,&z)==4) { type=4; return true; }
    if (std::sscanf(line.c_str(),"Ladder %d @ %d, %d, %d",&item,&x,&y,&z)==4) { type=5; return true; }
    if (std::sscanf(line.c_str(),"Hole %d @ %d, %d, %d",&item,&x,&y,&z)==4) { type=6; return true; }
    if (line.rfind("Say ",0)==0) { type=7; text=line.substr(4); return true; }
    if (std::sscanf(line.c_str(),"Wait %d ms",&delay)==1) { type=8; return true; }
    if (std::sscanf(line.c_str(),"FloorChange %d, %d, %d",&x,&y,&z)==3) { type=0; return true; }
    return false;
}

static std::string BuildWaypointLine(int type,int x,int y,int z,int item,int delay,const char* text,int targetId=0) {
    const char* names[]={"Walk","Stand","Use","Rope","Shovel","Ladder","Hole","Say","Wait"};
    char b[224]{};
    if(type==7) std::snprintf(b,sizeof(b),"Say %s",text?text:"");
    else if(type==8) std::snprintf(b,sizeof(b),"Wait %d ms",delay);
    else if((type==3 || type==4) && targetId>0) std::snprintf(b,sizeof(b),"%s %d @ %d, %d, %d target %d",names[type],item,x,y,z,targetId);
    else if(type>=2 && type<=6) std::snprintf(b,sizeof(b),"%s %d @ %d, %d, %d",names[type],item,x,y,z);
    else std::snprintf(b,sizeof(b),"%s %d, %d, %d",names[type],x,y,z);
    return b;
}

static void DrawWalkerIdCatalog() {
    if(g_previewMode && g_previewIds) {
        ImGui::OpenPopup("Walker IDs");
        g_previewIds=false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),ImGuiCond_Always,ImVec2(0.5f,0.5f));
    ImGui::SetNextWindowSize(ImVec2(560,450),ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(240,0),ImVec2(ImGui::GetIO().DisplaySize.x-12,ImGui::GetIO().DisplaySize.y-12));
    if(ImGui::BeginPopupModal("Walker IDs",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){
        g_largePopupOpen=true;
        EnsureWalkerIdCatalog();
        struct Row{const char* label;const char* key;} rows[]={
            {"Ladder IDs","ladder"},{"Rope IDs","rope"},{"Shovel IDs","shovel"},{"Pick IDs","pick"},{"Mine Pick IDs","minePick"},
            {"Machete IDs","machete"},{"Open Holes IDs","openHoles"},{"Pathable IDs","pathable"},{"Non Pathable IDs","nonPathable"},
            {"Clear Before Use","clearBeforeUse"},{"Counter IDs","counter"},{"Depot IDs","depot"},{"Food IDs","food"},{"Rock IDs","rock"},
            {"Corpse Bag IDs","corpseBag"},{"Fishing Rod IDs","fishingRod"},{"Fishing Water IDs","fishingWater"},
            {"Change Gold IDs","changeGold"}
        };
        static std::unordered_map<std::string,std::array<char,512>> buffers; static bool loaded=false;
        if(!loaded){for(auto&r:rows){auto csv=CatalogCsv(r.key);auto&b=buffers[r.key];std::snprintf(b.data(),b.size(),"%s",csv.c_str());}loaded=true;}
        ImGui::TextDisabled("Catalogo global. Edite manualmente os IDs separados por virgula.");
        ImGui::TextDisabled("Change Gold: IDs de gold e platinum; nao incluir crystal.");
        ImGui::Separator();
        if(ImGui::BeginTable("idtable",2,ImGuiTableFlags_SizingStretchProp|ImGuiTableFlags_RowBg)){
            ImGui::TableSetupColumn("Categoria",0,0.8f);ImGui::TableSetupColumn("IDs",0,2.2f);
            for(auto&r:rows){ImGui::TableNextRow();ImGui::TableSetColumnIndex(0);ImGui::TextUnformatted(r.label);ImGui::TableSetColumnIndex(1);ImGui::SetNextItemWidth(390);ImGui::PushID(r.key);ImGui::InputText("##ids",buffers[r.key].data(),buffers[r.key].size());ImGui::PopID();}
            ImGui::EndTable();
        }
        if(ImGui::Button("Salvar",ImVec2(100,22))){for(auto&r:rows)SetCatalogCsv(r.key,buffers[r.key].data());loaded=false;ImGui::CloseCurrentPopup();}
        ImGui::SameLine(); if(ImGui::Button("Recarregar",ImVec2(100,22))){g_walkerIds.loaded=false;EnsureWalkerIdCatalog();loaded=false;}
        ImGui::SameLine(); if(ImGui::Button("Fechar",ImVec2(80,22))){loaded=false;ImGui::CloseCurrentPopup();}
        ImGui::EndPopup();
    }

}

static void DrawCavebot(const Theme& t) {
    static bool enabled=false,targeting=false,keep=false,attackAny=false,shouldAttack=false;
    static bool recording=false,recordInitialized=false;
    static int hpMin=0,hpMax=100,range=7,mode=1; static int selected=-1;
    static std::vector<std::string> wps;
    static RamoniaPositionUi recordLast{};
    static char monsters[160]="";
    static int editTargetId=0,addTargetId=0;
    static int editType=0,editItemId=0,editDelay=1000,editX=0,editY=0,editZ=0;
    static char editText[96]="";
    static int addType=0,addItemId=0,addDelay=1000,addX=0,addY=0,addZ=0;
    static char addText[96]="";
    const char* addTypes[]={"Walk","Stand","Use","Rope","Shovel","Ladder","Hole","Say","Wait"};
    const char* modes[]={"follow","none","stand near","2sqm","3sqm","4sqm","diagonal","diagonal 3sqm"};
    static std::uint64_t seenProfileLoadGeneration=UINT64_MAX;
    if(seenProfileLoadGeneration!=g_profileLoadGeneration){
        seenProfileLoadGeneration=g_profileLoadGeneration;
        nlohmann::json wj;
        if(ReadJsonFile(g_baseDir/"config"/"waypoints.json",wj)){
            enabled=wj.value("enabled",false); wps.clear();
            if(wj.contains("waypoints")&&wj["waypoints"].is_array()){
                for(auto& n:wj["waypoints"]){
                    if(!n.is_object()) continue;
                    const auto type=n.value("type",std::string("walk"));
                    const int x=n.value("x",0), y=n.value("y",0), z=n.value("z",0), item=n.value("item_id",0), ms=n.value("ms",1000);
                    std::string line;
                    if(type=="walk") line=BuildWaypointLine(0,x,y,z,0,0,"");
                    else if(type=="stand") line=BuildWaypointLine(1,x,y,z,0,0,"");
                    else if(type=="use") line=BuildWaypointLine(2,x,y,z,item,0,"");
                    else if(type=="rope") line=BuildWaypointLine(3,x,y,z,item,0,"",n.value("target_item_id",0));
                    else if(type=="shovel") line=BuildWaypointLine(4,x,y,z,item,0,"",n.value("target_item_id",0));
                    else if(type=="ladder") line=BuildWaypointLine(5,x,y,z,item,0,"");
                    else if(type=="use_with_ground") line=BuildWaypointLine(6,x,y,z,item,0,"");
                    else if(type=="say") { const auto s=n.value("text",std::string()); line=BuildWaypointLine(7,0,0,0,0,0,s.c_str()); }
                    else if(type=="wait") line=BuildWaypointLine(8,0,0,0,0,ms,"");
                    if(!line.empty()) wps.push_back(std::move(line));
                }
            }
            selected=-1;
        }
        nlohmann::json tj;
        if(ReadJsonFile(g_baseDir/"config"/"targeting.json",tj)){
            targeting=tj.value("enabled",false); attackAny=tj.value("attack_any_monster",false);
            hpMin=tj.value("hp_min",0); hpMax=tj.value("hp_max",100); range=tj.value("max_range",7);
            keep=tj.value("chase_enabled",false);
            mode=std::clamp(tj.value("movement_mode",keep?0:1),0,7);
            std::string csv;
            if(tj.contains("targets")&&tj["targets"].is_array()) for(auto& v:tj["targets"]){
                if(!v.is_object()) continue; const auto nm=v.value("name",std::string()); if(nm.empty()) continue;
                if(!csv.empty()) csv += ", "; csv += nm;
            }
            std::snprintf(monsters,sizeof(monsters),"%s",csv.c_str());
        }
        g_walkerIds.loaded=false;
    }

    RefreshInspectResult();

    RamoniaPositionUi currentRecordPos{};
    if(g_livePlayer.valid) {
        currentRecordPos.valid=true;
        currentRecordPos.x=static_cast<std::uint32_t>(g_livePlayer.position.x);
        currentRecordPos.y=static_cast<std::uint32_t>(g_livePlayer.position.y);
        currentRecordPos.z=static_cast<std::uint32_t>(g_livePlayer.position.z);
    } else {
        currentRecordPos=GetRamoniaPlayerPosition();
    }

    if(recording && currentRecordPos.valid) {
        const auto p=currentRecordPos;
        if(!recordInitialized) {
            recordLast=p;
            recordInitialized=true;
        } else if(p.x!=recordLast.x || p.y!=recordLast.y || p.z!=recordLast.z) {
            char b[160];
            if(p.z!=recordLast.z) {
                int knownId=0;
                const char* knownType=nullptr;
                if(g_inspect.valid && !g_inspect.ids.empty() &&
                   std::abs(g_inspect.x-static_cast<int>(recordLast.x))<=1 &&
                   std::abs(g_inspect.y-static_cast<int>(recordLast.y))<=1) {
                    for(int id:g_inspect.ids){
                        if(CatalogHas("ladder",id)){knownId=id;knownType="Ladder";break;}
                        if(CatalogHas("rope",id)){knownId=id;knownType="Rope";break;}
                        if(CatalogHas("shovel",id)){knownId=id;knownType="Shovel";break;}
                        if(CatalogHas("openHoles",id)){knownId=id;knownType="Hole";break;}
                    }
                }
                if(knownType)
                    std::snprintf(
                        b,sizeof(b),"%s %d @ %u, %u, %u",
                        knownType,knownId,recordLast.x,recordLast.y,recordLast.z);
                else
                    std::snprintf(
                        b,sizeof(b),"FloorChange %u, %u, %u  [from z=%u]",
                        p.x,p.y,p.z,recordLast.z);
            } else {
                std::snprintf(
                    b,sizeof(b),"Walk %u, %u, %u",
                    p.x,p.y,p.z);
            }
            if(wps.empty()||wps.back()!=b)
                wps.push_back(b);
            recordLast=p;
        }
    } else if(!recording) {
        recordInitialized=false;
    }

    const bool targetRuntimeReady = TargetRuntimeReady();
    const bool caveRuntimeReady = CaveRuntimeReady();
    if(!g_nativeRuntimeLoaded) {
        if(!caveRuntimeReady) enabled=false;
        if(!targetRuntimeReady) targeting=false;
    }

    const float caveCardHeight =
        std::max(1.0f,ImGui::GetContentRegionAvail().y);
    CardBegin("cave",g_targetOnly?"Targeting":"Cavebot",CardIcon::Pin,
              ImVec4(0.12f,0.62f,1.0f,1),caveCardHeight,t);
    if (!g_targetOnly) {
    if(!caveRuntimeReady) ImGui::BeginDisabled();
    ImGui::Checkbox("Cavebot ON",&enabled);
    if(!caveRuntimeReady) ImGui::EndDisabled();
    ImGui::SameLine(); if(ImGui::SmallButton("IDs")) ImGui::OpenPopup("Walker IDs");
    ImGui::TextColored(t.blue,"Waypoints"); ImGui::SameLine(); ImGui::TextColored(recording?t.green:t.muted,recording?"REC ON":"REC OFF");
    if(!g_nativeRuntimeLoaded){
        if(!caveRuntimeReady) enabled=false;
        ImGui::SameLine();
        if(g_ramoniaBridgeReady.load()) ImGui::TextColored(t.green,"RAMONIA READY");
        else ImGui::TextColored(ImVec4(1.0f,0.55f,0.25f,1),"WAITING RUNTIME");
        if(!targetRuntimeReady) targeting=false;
    }

    const float waypointListHeight =
        g_nativeRuntimeLoaded ? 48.0f : 42.0f;
    ImVec2 listSize(
        ImGui::GetContentRegionAvail().x-100,
        waypointListHeight);
    ImGui::BeginChild("##wps",listSize,ImGuiChildFlags_Borders);
    if(wps.empty()) ImGui::TextDisabled("Nenhum waypoint carregado");
    for(int i=0;i<(int)wps.size();++i) if(ImGui::Selectable(wps[i].c_str(),selected==i)) selected=i;
    ImGui::EndChild(); ImGui::SameLine(); ImGui::BeginGroup();
    if(ImGui::Button("Add",ImVec2(40,18))){
        if(g_livePlayer.valid){
            addX=g_livePlayer.position.x;
            addY=g_livePlayer.position.y;
            addZ=(int)g_livePlayer.position.z;
        } else {
            const auto p=GetRamoniaPlayerPosition();
            if(p.valid){
                addX=static_cast<int>(p.x);
                addY=static_cast<int>(p.y);
                addZ=static_cast<int>(p.z);
            }
        }
        ImGui::OpenPopup("Add waypoint");
    }
    ImGui::SameLine(0,4);
    if(ImGui::Button("Edit",ImVec2(40,18)) &&
       selected>=0 && selected<(int)wps.size()){
        std::string txt;
        if(ParseWaypointLine(wps[selected],editType,editX,editY,editZ,editItemId,editDelay,txt,&editTargetId)){
            std::snprintf(editText,sizeof(editText),"%s",txt.c_str());
            ImGui::OpenPopup("Edit waypoint");
        }
    }
    if(ImGui::Button("Remove",ImVec2(40,18))&&selected>=0){
        wps.erase(wps.begin()+selected);
        selected=-1;
    }
    ImGui::SameLine(0,4);
    if(ImGui::Button(recording?"Stop":"Rec",ImVec2(40,18))){
        recording=!recording;
        recordInitialized=false;
    }
    ImGui::EndGroup();

    if(!g_nativeRuntimeLoaded){
        const auto caveStatus=RamoniaCaveStatusText();
        const auto cavePos=GetRamoniaPlayerPosition();
        if(cavePos.valid){
            ImGui::TextDisabled(
                "%s | Pos %u,%u,%u | Bridge V%d",
                caveStatus.c_str(),
                cavePos.x,cavePos.y,cavePos.z,
                g_ramoniaBridgeVersion.load());
        } else {
            ImGui::TextDisabled(
                "%s | Pos -- | Bridge V%d",
                caveStatus.c_str(),
                g_ramoniaBridgeVersion.load());
        }
    }

    DrawWalkerIdCatalog();

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),ImGuiCond_Always,ImVec2(0.5f,0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(240,0),ImVec2(ImGui::GetIO().DisplaySize.x-12,ImGui::GetIO().DisplaySize.y-12));
    if(ImGui::BeginPopupModal("Add waypoint",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){
        g_largePopupOpen=true;
        ImGui::SetNextItemWidth(150);ImGui::Combo("Type",&addType,addTypes,IM_ARRAYSIZE(addTypes));
        SmallInt("X",&addX,90);ImGui::SameLine();SmallInt("Y",&addY,90);ImGui::SameLine();SmallInt("Z",&addZ,60);
        if(addType>=2&&addType<=6){if(addItemId==0){if(addType==3)addItemId=CatalogFirst("rope",3003);else if(addType==4)addItemId=CatalogFirst("shovel",3457);else if(addType==5)addItemId=CatalogFirst("ladder",0);}SmallInt("Item ID",&addItemId,110);}
        if(addType==3 || addType==4) SmallInt("Alvo no chao",&addTargetId,110);
        if(addType==7){ImGui::SetNextItemWidth(260);ImGui::InputText("Text",addText,sizeof(addText));}
        if(addType==8)SmallInt("Delay ms",&addDelay,110);
        if(ImGui::Button("Add node",ImVec2(100,22))){wps.push_back(BuildWaypointLine(addType,addX,addY,addZ,addItemId,addDelay,addText,addTargetId));ImGui::CloseCurrentPopup();}
        ImGui::SameLine();if(ImGui::Button("Cancel",ImVec2(80,22)))ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),ImGuiCond_Always,ImVec2(0.5f,0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(240,0),ImVec2(ImGui::GetIO().DisplaySize.x-12,ImGui::GetIO().DisplaySize.y-12));
    if(ImGui::BeginPopupModal("Edit waypoint",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){
        g_largePopupOpen=true;
        ImGui::SetNextItemWidth(150);ImGui::Combo("Type",&editType,addTypes,IM_ARRAYSIZE(addTypes));
        SmallInt("X",&editX,90);ImGui::SameLine();SmallInt("Y",&editY,90);ImGui::SameLine();SmallInt("Z",&editZ,60);
        if(editType>=2&&editType<=6)SmallInt("Item ID",&editItemId,110);
        if(editType==3 || editType==4) SmallInt("Alvo no chao",&editTargetId,110);
        if(editType==7){ImGui::SetNextItemWidth(260);ImGui::InputText("Text",editText,sizeof(editText));}
        if(editType==8)SmallInt("Delay ms",&editDelay,110);
        if(ImGui::Button("Salvar",ImVec2(100,22))&&selected>=0&&selected<(int)wps.size()){wps[selected]=BuildWaypointLine(editType,editX,editY,editZ,editItemId,editDelay,editText,editTargetId);ImGui::CloseCurrentPopup();}
        ImGui::SameLine();if(ImGui::Button("Cancelar",ImVec2(80,22)))ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    } // waypoint editor
    if (g_targetOnly) {
    ImGui::Separator();
    if(!targetRuntimeReady) ImGui::BeginDisabled();
    if(ImGui::BeginTable("target_controls",2,ImGuiTableFlags_SizingStretchSame)){
        ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::Checkbox("Ativar Target",&targeting);
        ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted("HP:"); ImGui::SameLine(); SmallInt("##hpmin",&hpMin,44); ImGui::SameLine(); ImGui::TextUnformatted("-"); ImGui::SameLine(); SmallInt("##hpmax",&hpMax,44);
        ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::Checkbox("Attack Any (*)",&attackAny);
        ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted("Range:"); ImGui::SameLine(); SmallInt("##range",&range,44);
        ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::BeginDisabled(); ImGui::Checkbox("Should Attack Me",&shouldAttack); ImGui::EndDisabled();
        if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Pendente: descobrir o foco real do monstro.");
        ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted("Mode:"); ImGui::SameLine(); ImGui::SetNextItemWidth(-1); ImGui::Combo("##targetmode",&mode,modes,IM_ARRAYSIZE(modes));
        ImGui::EndTable();
    }
    ImGui::TextUnformatted("Alvos:");ImGui::SameLine();ImGui::SetNextItemWidth(-1);ImGui::InputText("##monsters",monsters,sizeof(monsters));
    if(g_nativeRuntimeLoaded) ImGui::TextDisabled(mode==1?"None: ataca sem seguir.":"Reposiciona durante combate; Cavebot fica pausado.");
    if(!targetRuntimeReady) ImGui::EndDisabled();
    }
    keep=mode!=1;
    const int modeDistance=mode==3?2:(mode==4||mode==7)?3:mode==5?4:1;

    nlohmann::json tcfg={{"enabled",targetRuntimeReady&&targeting},{"poll_ms",300},{"attack_any_monster",attackAny},{"pause_walker",true},{"movement_mode",mode},{"chase_enabled",keep},{"chase_distance",modeDistance},{"chase_scan_range",15},{"chase_interval_ms",350},{"hp_min",hpMin},{"hp_max",hpMax},{"max_range",range},{"chase_repath_ms",900},{"chase_force_refresh_ms",1800},{"manual_input_pause_ms",1500},{"targets",nlohmann::json::array()}};
    const auto targetNames=SplitCsv(monsters);
    int pr=0;for(const auto&name:targetNames)tcfg["targets"].push_back({{"name",name},{"priority",pr++}});WriteJsonIfChanged("targeting.json",tcfg);

    RamoniaTargetConfigUi ramCfg;
    ramCfg.enabled=targeting;
    ramCfg.attackAny=attackAny;
    ramCfg.hpMin=std::clamp(hpMin,0,100);
    ramCfg.hpMax=std::clamp(hpMax,0,100);
    ramCfg.maxRange=std::max(0,range);
    ramCfg.names=targetNames;
    SetRamoniaTargetConfig(ramCfg);

    nlohmann::json rcfg={
        {"enabled",caveRuntimeReady&&enabled},
        {"loop",true},
        {"reach_distance",0},
        {"timeout_ms",15000},
        {"stuck_ms",3500},
        {"retry_delay_ms",650},
        {"max_retries",2},
        {"action_wait_ms",450},
        {"tool_ids",{
            {"rope",CatalogFirst("rope",3003)},
            {"shovel",CatalogFirst("shovel",3457)},
            {"fishing_rod",CatalogFirst("fishingRod",3483)}
        }},
        {"waypoints",nlohmann::json::array()}
    };

    RamoniaCaveConfigUi ramCave;
    ramCave.enabled=caveRuntimeReady&&enabled;
    ramCave.loop=true;
    ramCave.stuckMs=3500;
    ramCave.retryDelayMs=650;
    ramCave.maxRetries=2;

    for(const auto& line:wps){
        int x=0,y=0,z=0,item=0,ms=0;
        RamoniaCaveNodeUi node;
        bool haveNode=false;

        if(std::sscanf(line.c_str(),"Walk %d, %d, %d",&x,&y,&z)==3){
            rcfg["waypoints"].push_back({
                {"type","walk"},{"x",x},{"y",y},{"z",z}});
            node.type="walk";
            node.x=static_cast<std::uint32_t>(std::max(0,x));
            node.y=static_cast<std::uint32_t>(std::max(0,y));
            node.z=static_cast<std::uint32_t>(std::max(0,z));
            haveNode=true;
        } else if(std::sscanf(line.c_str(),"Stand %d, %d, %d",&x,&y,&z)==3){
            rcfg["waypoints"].push_back({
                {"type","stand"},{"x",x},{"y",y},{"z",z}});
            node.type="stand";
            node.x=static_cast<std::uint32_t>(std::max(0,x));
            node.y=static_cast<std::uint32_t>(std::max(0,y));
            node.z=static_cast<std::uint32_t>(std::max(0,z));
            haveNode=true;
        } else if(std::sscanf(line.c_str(),"FloorChange %d, %d, %d",&x,&y,&z)==3){
            rcfg["waypoints"].push_back({
                {"type","walk"},{"x",x},{"y",y},{"z",z}});
            node.type="walk";
            node.x=static_cast<std::uint32_t>(std::max(0,x));
            node.y=static_cast<std::uint32_t>(std::max(0,y));
            node.z=static_cast<std::uint32_t>(std::max(0,z));
            haveNode=true;
        } else if(std::sscanf(line.c_str(),"Wait %d ms",&ms)==1){
            rcfg["waypoints"].push_back({
                {"type","wait"},{"ms",ms}});
            node.type="wait";
            node.waitMs=static_cast<std::uint32_t>(std::max(1,ms));
            haveNode=true;
        } else if(line.rfind("Say ",0)==0){
            const auto text=line.substr(4);
            rcfg["waypoints"].push_back({
                {"type","say"},{"text",text}});
            node.type="say";
            node.text=text;
            haveNode=true;
        } else if(std::sscanf(
                      line.c_str(),"Use %d @ %d, %d, %d",
                      &item,&x,&y,&z)==4){
            rcfg["waypoints"].push_back({
                {"type","use"},{"item_id",item},
                {"x",x},{"y",y},{"z",z}});
            node.type="use";
            node.itemId=static_cast<std::uint32_t>(std::max(0,item));
            node.x=static_cast<std::uint32_t>(std::max(0,x));
            node.y=static_cast<std::uint32_t>(std::max(0,y));
            node.z=static_cast<std::uint32_t>(std::max(0,z));
            haveNode=true;
        } else if(std::sscanf(
                      line.c_str(),"Rope %d @ %d, %d, %d",
                      &item,&x,&y,&z)==4){
            rcfg["waypoints"].push_back({
                {"type","rope"},{"item_id",item},
                {"x",x},{"y",y},{"z",z}});
            int targetItemId=0;
            std::sscanf(line.c_str(),"Rope %d @ %d, %d, %d target %d",
                        &item,&x,&y,&z,&targetItemId);
            node.targetItemId=static_cast<std::uint32_t>(std::max(0,targetItemId));
            rcfg["waypoints"].back()["target_item_id"]=node.targetItemId;
            node.type="rope";
            node.itemId=static_cast<std::uint32_t>(std::max(0,item));
            node.x=static_cast<std::uint32_t>(std::max(0,x));
            node.y=static_cast<std::uint32_t>(std::max(0,y));
            node.z=static_cast<std::uint32_t>(std::max(0,z));
            haveNode=true;
        } else if(std::sscanf(
                      line.c_str(),"Shovel %d @ %d, %d, %d",
                      &item,&x,&y,&z)==4){
            rcfg["waypoints"].push_back({
                {"type","shovel"},{"item_id",item},
                {"x",x},{"y",y},{"z",z}});
            int targetItemId=0;
            std::sscanf(line.c_str(),"Shovel %d @ %d, %d, %d target %d",
                        &item,&x,&y,&z,&targetItemId);
            node.targetItemId=static_cast<std::uint32_t>(std::max(0,targetItemId));
            rcfg["waypoints"].back()["target_item_id"]=node.targetItemId;
            node.type="shovel";
            node.itemId=static_cast<std::uint32_t>(std::max(0,item));
            node.x=static_cast<std::uint32_t>(std::max(0,x));
            node.y=static_cast<std::uint32_t>(std::max(0,y));
            node.z=static_cast<std::uint32_t>(std::max(0,z));
            haveNode=true;
        } else if(std::sscanf(
                      line.c_str(),"Ladder %d @ %d, %d, %d",
                      &item,&x,&y,&z)==4){
            rcfg["waypoints"].push_back({
                {"type","ladder"},{"item_id",item},
                {"x",x},{"y",y},{"z",z},{"after_ms",500}});
            node.type="ladder";
            node.itemId=static_cast<std::uint32_t>(std::max(0,item));
            node.x=static_cast<std::uint32_t>(std::max(0,x));
            node.y=static_cast<std::uint32_t>(std::max(0,y));
            node.z=static_cast<std::uint32_t>(std::max(0,z));
            haveNode=true;
        } else if(std::sscanf(
                      line.c_str(),"Hole %d @ %d, %d, %d",
                      &item,&x,&y,&z)==4){
            // Hole/bueiro is a direct tile use. Rope/Shovel are the tool
            // actions that use Game::useWith.
            rcfg["waypoints"].push_back({
                {"type","use_with_ground"},{"item_id",item},
                {"x",x},{"y",y},{"z",z}});
            node.type="use_with_ground";
            node.itemId=static_cast<std::uint32_t>(std::max(0,item));
            node.x=static_cast<std::uint32_t>(std::max(0,x));
            node.y=static_cast<std::uint32_t>(std::max(0,y));
            node.z=static_cast<std::uint32_t>(std::max(0,z));
            haveNode=true;
        }

        if(haveNode)
            ramCave.nodes.push_back(std::move(node));
    }

    SetRamoniaCaveConfig(ramCave);
    WriteJsonIfChanged("waypoints.json",rcfg);
    CardEnd();
}

static void DrawTools(const Theme& t) {
    static bool changeGold=false;
    static bool mana=false,light=false,antiIdle=false,fish=false,food=false;
    static bool para=false,haste=false,advert=false,spear=false,reAmmo=false;
    static char paraSpell[32]="exura",hasteSpell[32]="utani hur",manaSpell[32]="utamo vita",ad[64]="Set Message...",spears[64]="2389";
    static int ammo=3447, foodInterval=60000, manaInterval=170000, hasteInterval=30000, idleMin=45000, idleMax=90000;
    static int fishMinDelay=850, fishMaxDelay=1450, fishRange=7;
    static std::uint64_t seenProfileLoadGeneration=~0ull;
    if(seenProfileLoadGeneration!=g_profileLoadGeneration){
        seenProfileLoadGeneration=g_profileLoadGeneration;
        nlohmann::json fj;
        if(ReadJsonFile(g_baseDir/"config"/"fishing.json",fj)){
            fish=fj.value("enabled",false); fishRange=fj.value("range",7);
            fishMinDelay=fj.value("min_delay_ms",850); fishMaxDelay=fj.value("max_delay_ms",1450);
        }
        nlohmann::json tj;
        if(ReadJsonFile(g_baseDir/"config"/"tools.json",tj)){
            if(tj.contains("change_gold") && tj["change_gold"].is_object()) changeGold=tj["change_gold"].value("enabled",false);
            auto loadSpell=[&](const char* key,bool& en,char* buf,size_t cap,int& interval,const char* def){ if(tj.contains(key)&&tj[key].is_object()){ const auto& o=tj[key]; en=o.value("enabled",false); const auto sp=o.value("spell",std::string(def)); std::snprintf(buf,cap,"%s",sp.c_str()); interval=o.value("fallback_interval_ms",interval);} };
            loadSpell("mana_shield",mana,manaSpell,sizeof(manaSpell),manaInterval,"utamo vita");
            loadSpell("anti_paralyze",para,paraSpell,sizeof(paraSpell),hasteInterval,"exura");
            loadSpell("auto_haste",haste,hasteSpell,sizeof(hasteSpell),hasteInterval,"utani hur");
            if(tj.contains("food")){
                food=tj["food"].value("enabled",false);
                foodInterval=tj["food"].value("interval_ms",foodInterval);
            }
            if(tj.contains("anti_idle")){ antiIdle=tj["anti_idle"].value("enabled",false); idleMin=tj["anti_idle"].value("min_interval_ms",idleMin); idleMax=tj["anti_idle"].value("max_interval_ms",idleMax); }
            if(tj.contains("light")) light=tj["light"].value("enabled",false);
            if(tj.contains("spear_pickup") && tj["spear_pickup"].is_object()){
                spear=tj["spear_pickup"].value("enabled",false);
                std::string ids;
                const auto& sp=tj["spear_pickup"];
                if(sp.contains("item_ids") && sp["item_ids"].is_array()){
                    for(const auto& v:sp["item_ids"]){
                        if(!v.is_number_integer() || v.get<int>()<=0) continue;
                        if(!ids.empty()) ids += ",";
                        ids += std::to_string(v.get<int>());
                    }
                }
                if(ids.empty()) ids="2389";
                std::snprintf(spears,sizeof(spears),"%s",ids.c_str());
            }
            if(tj.contains("reammo")){ reAmmo=tj["reammo"].value("enabled",false); if(tj["reammo"].contains("item_ids")&&tj["reammo"]["item_ids"].is_array()&&!tj["reammo"]["item_ids"].empty()) ammo=tj["reammo"]["item_ids"][0].get<int>(); }
        }
    }
    const float toolsCardHeight =
        g_nativeRuntimeLoaded ? (fish ? 222.0f : 188.0f) :
        (g_ramoniaBridgeReady.load() ? 170.0f : 148.0f);
    CardBegin("tools","Tools",CardIcon::Gear,ImVec4(0.43f,0.48f,1.0f,1),toolsCardHeight+20,t);
    if(ImGui::SmallButton("IDs"))ImGui::OpenPopup("Walker IDs");
    ImGui::SameLine();ImGui::TextDisabled("Catalogo de itens e pisos");
    const bool foodRuntimeReady = FoodRuntimeReady();
    const bool lightRuntimeReady = LightRuntimeReady();
    const bool spearRuntimeReady =
        g_nativeRuntimeLoaded ||
        (g_ramoniaBridgeReady.load() && g_ramoniaSpearReady.load());
    if (ImGui::BeginTable("tools_grid",3,ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("left",0,1.05f); ImGui::TableSetupColumn("right",0,1.05f); ImGui::TableSetupColumn("value",0,1.25f);
        auto row=[&](const char* lid,bool* lv,const char* rid,bool* rv,const char* iid,char* buf,size_t cap){ ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::Checkbox(lid,lv); ImGui::TableSetColumnIndex(1); ImGui::Checkbox(rid,rv); ImGui::TableSetColumnIndex(2); SmallInput(iid,buf,cap,-1); };

        if(!g_nativeRuntimeLoaded) ImGui::BeginDisabled();
        row("Mana Shield",&mana,"Anti-Paralyze",&para,"##para",paraSpell,sizeof(paraSpell));
        if(!g_nativeRuntimeLoaded) ImGui::EndDisabled();

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if(!lightRuntimeReady) ImGui::BeginDisabled();
        ImGui::Checkbox("Light Hack",&light);
        if(!lightRuntimeReady) ImGui::EndDisabled();

        ImGui::TableSetColumnIndex(1);
        if(!g_nativeRuntimeLoaded) ImGui::BeginDisabled();
        ImGui::Checkbox("Auto-Haste",&haste);
        ImGui::TableSetColumnIndex(2);
        SmallInput("##haste",hasteSpell,sizeof(hasteSpell),-1);
        if(!g_nativeRuntimeLoaded) ImGui::EndDisabled();

        if(!g_nativeRuntimeLoaded) ImGui::BeginDisabled();
        row("Anti-Idle",&antiIdle,"Advertise",&advert,"##advert",ad,sizeof(ad));
        if(!g_nativeRuntimeLoaded) ImGui::EndDisabled();

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if(!g_nativeRuntimeLoaded) ImGui::BeginDisabled();
        ImGui::Checkbox("Auto Fishing",&fish);
        if(!g_nativeRuntimeLoaded) ImGui::EndDisabled();

        ImGui::TableSetColumnIndex(1);
        if(!spearRuntimeReady) ImGui::BeginDisabled();
        ImGui::Checkbox("Pick Up Spear",&spear);
        ImGui::TableSetColumnIndex(2);
        SmallInput("##spear",spears,sizeof(spears),-1);
        if(!spearRuntimeReady) ImGui::EndDisabled();

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if(!foodRuntimeReady) ImGui::BeginDisabled();
        ImGui::Checkbox("Food Eater",&food);
        if(!foodRuntimeReady) ImGui::EndDisabled();

        ImGui::TableSetColumnIndex(1);
        if(!g_nativeRuntimeLoaded) ImGui::BeginDisabled();
        ImGui::Checkbox("Re-Ammo",&reAmmo);
        if(!g_nativeRuntimeLoaded) ImGui::EndDisabled();

        ImGui::TableSetColumnIndex(2);
        if(!g_nativeRuntimeLoaded) ImGui::BeginDisabled();
        SmallInt("##ammo",&ammo,-1);
        if(!g_nativeRuntimeLoaded) ImGui::EndDisabled();
        ImGui::EndTable();
    }

    if(!g_nativeRuntimeLoaded) ImGui::BeginDisabled();
    ImGui::Checkbox("Change Gold",&changeGold);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Converte pilhas de 100 gold em platinum e 100 platinum em crystal.");
    ImGui::SameLine(); ImGui::TextDisabled("IDs: Cavebot > IDs");
    if(!g_nativeRuntimeLoaded) ImGui::EndDisabled();
    if(g_nativeRuntimeLoaded && fish){ ImGui::TextUnformatted("Pesca: Delay"); ImGui::SameLine(); SmallInt("##fmin",&fishMinDelay,60); ImGui::SameLine(); ImGui::TextUnformatted("-"); ImGui::SameLine(); SmallInt("##fmax",&fishMaxDelay,60); ImGui::SameLine(); ImGui::TextUnformatted("ms  Range:"); ImGui::SameLine(); SmallInt("##frange",&fishRange,45); }

    if(!foodRuntimeReady) ImGui::BeginDisabled();
    ImGui::TextDisabled("Food IDs: usa catalogo global"); ImGui::SameLine();
    ImGui::TextDisabled("Timer ms"); ImGui::SameLine(); SmallInt("##foodms",&foodInterval,70);
    if(!foodRuntimeReady) ImGui::EndDisabled();

    if(g_nativeRuntimeLoaded){
        ImGui::SameLine(); ImGui::TextDisabled("ManaShield"); ImGui::SameLine(); SmallInt("##manams",&manaInterval,70);
        ImGui::SameLine(); ImGui::TextDisabled("Haste"); ImGui::SameLine(); SmallInt("##hastems",&hasteInterval,70);
    } else if(g_ramoniaBridgeReady.load() && g_ramoniaBridgeVersion.load()>=22) {
        ImGui::TextDisabled("%s",RamoniaLightStatusText().c_str());
    }

    if (g_ramoniaBridgeReady.load())
        ImGui::TextDisabled("%s",RamoniaSpearStatusText().c_str());

    EnsureWalkerIdCatalog();
    nlohmann::json waters=nlohmann::json::array(); for(auto&v:g_walkerIds.data["fishingWater"]) if(v.is_number_integer()) waters.push_back(v);
    nlohmann::json foodIds=nlohmann::json::array();
    std::vector<std::uint32_t> ramoniaFoodIds;
    for(int id:CatalogIds("food")) {
        if(id<=0 || id>0xFFFF) continue;
        foodIds.push_back(id);
        ramoniaFoodIds.push_back(static_cast<std::uint32_t>(id));
    }
    nlohmann::json fcfg={{"enabled",g_nativeRuntimeLoaded&&fish},{"rod_id",CatalogFirst("fishingRod",3483)},{"water_ids",waters},{"range",fishRange},{"min_delay_ms",fishMinDelay},{"max_delay_ms",fishMaxDelay},{"random_target",true},{"pause_while_walker",true},{"pause_while_targeting",true}};
    WriteJsonIfChanged("fishing.json",fcfg);

    RamoniaFoodConfigUi ramFood;
    ramFood.enabled = g_ramoniaBridgeReady.load() && g_ramoniaBridgeVersion.load() >= 16 && food;
    ramFood.itemIds = std::move(ramoniaFoodIds);
    ramFood.intervalMs = static_cast<std::uint32_t>(std::max(1000,foodInterval));
    ramFood.retryMs = 1000;
    SetRamoniaFoodConfig(ramFood);

    RamoniaSpearConfigUi ramSpear;
    for(const auto& token:SplitCsv(spears)) {
        try {
            const auto id=std::stoul(token);
            if(id>0 && id<=0xFFFFu &&
               std::find(ramSpear.itemIds.begin(),ramSpear.itemIds.end(),
                         static_cast<std::uint32_t>(id))==ramSpear.itemIds.end())
                ramSpear.itemIds.push_back(static_cast<std::uint32_t>(id));
        } catch (...) {}
    }
    // The struct carries 2389 as a safe default. If the user typed a valid
    // list, keep only that list instead of silently adding 2389.
    const auto typedSpearIds=SplitCsv(spears);
    if(!typedSpearIds.empty()) {
        std::vector<std::uint32_t> exactIds;
        for(const auto& token:typedSpearIds) {
            try {
                const auto id=std::stoul(token);
                if(id>0 && id<=0xFFFFu &&
                   std::find(exactIds.begin(),exactIds.end(),
                             static_cast<std::uint32_t>(id))==exactIds.end())
                    exactIds.push_back(static_cast<std::uint32_t>(id));
            } catch (...) {}
        }
        if(!exactIds.empty()) ramSpear.itemIds=std::move(exactIds);
    }
    ramSpear.enabled =
        g_ramoniaBridgeReady.load() &&
        g_ramoniaSpearReady.load() &&
        spear;
    ramSpear.retryMs=250;
    SetRamoniaSpearConfig(ramSpear);

    RamoniaLightConfigUi ramLight;
    ramLight.enabled =
        g_ramoniaBridgeReady.load() &&
        g_ramoniaBridgeVersion.load() >= 22 &&
        light;
    SetRamoniaLightConfig(ramLight);

    nlohmann::json spearIds=nlohmann::json::array();
    for(const auto id:ramSpear.itemIds) if(id>0 && id<=0xFFFFu) spearIds.push_back(id);

    nlohmann::json tcfg={
      {"change_gold",{{"enabled",g_nativeRuntimeLoaded&&changeGold},{"interval_ms",1000},{"item_ids",CatalogIds("changeGold")}}},
      {"food",{{"enabled",foodRuntimeReady&&food},{"interval_ms",foodInterval},{"item_ids",foodIds}}},
      {"anti_idle",{{"enabled",g_nativeRuntimeLoaded&&antiIdle},{"min_interval_ms",idleMin},{"max_interval_ms",idleMax},{"mode","turn_only"},{"status","runtime_turn_action_pending"}}},
      {"mana_shield",{{"enabled",g_nativeRuntimeLoaded&&mana},{"spell",manaSpell},{"fallback_interval_ms",manaInterval},{"prefer_condition_state",true}}},
      {"anti_paralyze",{{"enabled",g_nativeRuntimeLoaded&&para},{"spell",paraSpell},{"prefer_condition_state",true},{"speed_baseline_mode","unhasted_base"}}},
      {"auto_haste",{{"enabled",g_nativeRuntimeLoaded&&haste},{"spell",hasteSpell},{"fallback_interval_ms",hasteInterval},{"prefer_condition_state",true}}},
      {"light",{{"enabled",lightRuntimeReady&&light},{"profile_gated",true},{"ambient",1.0},{"ramonia_bridge_min_version",22}}},
      {"spear_pickup",{
          {"enabled",spear},
          {"item_ids",spearIds},
          {"scan_radius",1},
          {"scan_shape","center_plus_8_neighbors"},
          {"scan_only_when_enabled",true},
          {"tile_match_policy","runtime_getTopMoveThing"},
          {"hand_policy","existing_spear_else_free_hand"},
          {"right_hand_slot",5},
          {"left_hand_slot",6},
          {"reserve_free_capacity_percent",20},
          {"capacity_guard_enabled",false},
          {"capacity_guard_status","pending_GET_PLAYER_capacity"},
          {"ramonia_bridge_min_version",39},
          {"status","ramonia_ground_to_hand_live_validated"}
      }},
      {"reammo",{{"enabled",g_nativeRuntimeLoaded&&reAmmo},{"item_ids",nlohmann::json::array({ammo})},{"min_count",1},{"status","requires_move_equip_backend"}}}
    };
    WriteJsonIfChanged("tools.json",tcfg);
    DrawWalkerIdCatalog();
    CardEnd();
}

static void DrawLoot(const Theme& t) {
    static bool items=false,onScreen=false,only=true,dropEnabled=false;
    static int radius=3,maxWeight=3155,corpseId=2813,targetBagId=0;
    static char lootIds[256]="2148";
    static char nestedIds[256]="";
    static char dropIds[256]="";
    static std::uint64_t seenProfileLoadGeneration=0;

    auto csvIds=[](const char*s){
        nlohmann::json a=nlohmann::json::array();
        std::istringstream ss(s?s:"");
        std::string p;
        while(std::getline(ss,p,',')){
            try{int id=std::stoi(p);if(id>0)a.push_back(id);}catch(...){}
        }
        return a;
    };

    if(seenProfileLoadGeneration!=g_profileLoadGeneration){
        seenProfileLoadGeneration=g_profileLoadGeneration;
        nlohmann::json lj;
        if(ReadJsonFile(g_baseDir/"config"/"loot.json",lj)){
            items=lj.value("enabled",false);
            onScreen=lj.value("loot_on_screen",false);
            only=lj.value("only_list",true);
            radius=lj.value("radius",3);
            maxWeight=lj.value("max_weight",3155);
            targetBagId=lj.value("target_container_id",0);
            if(lj.contains("corpse_item_ids") && lj["corpse_item_ids"].is_array() &&
               !lj["corpse_item_ids"].empty() && lj["corpse_item_ids"][0].is_number_integer())
                corpseId=lj["corpse_item_ids"][0].get<int>();

            if(lj.contains("learned_corpse_ids") && lj["learned_corpse_ids"].is_array()) {
                for(const auto& v:lj["learned_corpse_ids"])
                    if(v.is_number_integer() && v.get<int>()>0)
                        LearnRamoniaCorpseId(static_cast<std::uint32_t>(v.get<int>()));
            }

            auto toCsv=[](const nlohmann::json& a){
                std::string out;
                if(a.is_array()) for(auto&v:a) if(v.is_number_integer()){
                    if(!out.empty()) out += ",";
                    out += std::to_string(v.get<int>());
                }
                return out;
            };
            const auto li=toCsv(lj.value("item_ids",nlohmann::json::array()));
            std::snprintf(lootIds,sizeof(lootIds),"%s",li.c_str());

            auto ni=toCsv(lj.value("nested_container_ids",nlohmann::json::array()));
            if(ni.empty()){
                EnsureWalkerIdCatalog();
                ni=CatalogCsv("corpseBag");
            }
            std::snprintf(nestedIds,sizeof(nestedIds),"%s",ni.c_str());

            if(lj.contains("drop_item")&&lj["drop_item"].is_object()){
                dropEnabled=lj["drop_item"].value("enabled",false);
                const auto di=toCsv(lj["drop_item"].value("item_ids",nlohmann::json::array()));
                std::snprintf(dropIds,sizeof(dropIds),"%s",di.c_str());
            }
        }
    }

    const bool lootRuntimeReady=LootRuntimeReady();
    const bool ramoniaLootReady=
        g_ramoniaBridgeReady.load() && g_ramoniaBridgeVersion.load()>=21;

    const float lootCardHeight =
        g_nativeRuntimeLoaded ? 200.0f : 122.0f;
    CardBegin("loot","Loot / Looter",CardIcon::Coins,ImVec4(1.0f,0.76f,0.12f,1),lootCardHeight,t);

    // Top controls: keep Ramonia-visible values inside fixed columns so
    // Destination Bag ID never gets clipped at the right edge.
    if(ImGui::BeginTable("loot_top",4,ImGuiTableFlags_SizingStretchProp)){
        ImGui::TableSetupColumn("##loot_on",0,1.05f);
        ImGui::TableSetupColumn("##only",0,1.15f);
        ImGui::TableSetupColumn("##corpse",0,1.55f);
        ImGui::TableSetupColumn("##dest",0,1.55f);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        if(!lootRuntimeReady) ImGui::BeginDisabled();
        ImGui::Checkbox("Loot Items",&items);
        if(!lootRuntimeReady) ImGui::EndDisabled();

        ImGui::TableSetColumnIndex(1);
        if(!lootRuntimeReady) ImGui::BeginDisabled();
        ImGui::Checkbox("Only List",&only);
        if(!lootRuntimeReady) ImGui::EndDisabled();

        ImGui::TableSetColumnIndex(2);
        if(ramoniaLootReady){
            ImGui::TextDisabled("Corpse fallback");
            ImGui::SameLine();
            SmallInt("##corpseid",&corpseId,62);
        }else{
            ImGui::TextDisabled("Radius");
            ImGui::SameLine();
            SmallInt("##radius",&radius,52);
        }

        ImGui::TableSetColumnIndex(3);
        if(ramoniaLootReady){
            ImGui::TextDisabled("Destination Bag ID");
            ImGui::SameLine();
            SmallInt("##lootbag",&targetBagId,52);
        }else{
            ImGui::TextDisabled("Destination Bag ID");
            ImGui::SameLine();
            SmallInt("##lootbag_olders",&targetBagId,52);
        }
        ImGui::EndTable();
    }

    // Main Ramonia loot configuration uses two balanced columns.
    if(ImGui::BeginTable("loot_ids_grid",2,ImGuiTableFlags_SizingStretchSame)){
        ImGui::TableSetupColumn("##loot_ids_col");
        ImGui::TableSetupColumn("##corpse_bags_col");
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        ImGui::TextDisabled("Loot IDs");
        ImGui::SetNextItemWidth(-1);
        if(!lootRuntimeReady) ImGui::BeginDisabled();
        ImGui::InputText("##lootids",lootIds,sizeof(lootIds));
        if(!lootRuntimeReady) ImGui::EndDisabled();

        ImGui::TableSetColumnIndex(1);
        ImGui::TextDisabled("Corpse Bag IDs (abre bags dentro do corpse)");
        ImGui::SetNextItemWidth(-1);
        if(!lootRuntimeReady) ImGui::BeginDisabled();
        ImGui::InputText("##nestedbagids",nestedIds,sizeof(nestedIds));
        if(!lootRuntimeReady) ImGui::EndDisabled();

        ImGui::EndTable();
    }

    if(g_nativeRuntimeLoaded){
        ImGui::TextDisabled("Corpse Client ID");ImGui::SameLine();SmallInt("##corpse_olders",&corpseId,62);
        ImGui::TextWrapped("Use os Client IDs do Look. Deixe a bag de destino aberta e com espaco.");
    }
    // Ground Drop remains a native-only feature and is visually separated
    // from the Ramonia corpse/bag workflow.
    ImGui::Separator();
    ImGui::BeginDisabled();
    ImGui::Checkbox("Ground Drop",&dropEnabled);
    ImGui::SameLine();
    ImGui::Checkbox("Loot on Screen",&onScreen);
    ImGui::SameLine();
    ImGui::TextDisabled("Ground Drop IDs");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##dropids",dropIds,sizeof(dropIds));
    ImGui::EndDisabled();

    const auto lootJsonIds=csvIds(lootIds);
    const auto nestedJsonIds=csvIds(nestedIds);

    RamoniaLootConfigUi ramLoot;
    ramLoot.enabled=ramoniaLootReady&&items;
    ramLoot.onlyList=only;
    ramLoot.corpseItemId=static_cast<std::uint32_t>(std::max(1,corpseId));
    ramLoot.targetContainerId=static_cast<std::uint32_t>(std::max(0,targetBagId));
    if(lootJsonIds.is_array())
        for(const auto& v:lootJsonIds)
            if(v.is_number_integer() && v.get<int>()>0)
                ramLoot.itemIds.push_back(static_cast<std::uint32_t>(v.get<int>()));

    if(nestedJsonIds.is_array())
        for(const auto& v:nestedJsonIds)
            if(v.is_number_integer() && v.get<int>()>0)
                ramLoot.nestedContainerIds.push_back(
                    static_cast<std::uint32_t>(v.get<int>()));

    ramLoot.learnedCorpseIds=GetRamoniaLootConfig().learnedCorpseIds;
    if(std::find(ramLoot.learnedCorpseIds.begin(),ramLoot.learnedCorpseIds.end(),2813u)
       ==ramLoot.learnedCorpseIds.end())
        ramLoot.learnedCorpseIds.push_back(2813u);
    SetRamoniaLootConfig(ramLoot);

    const auto persistedLoot=GetRamoniaLootConfig();
    nlohmann::json learnedCorpseIds=nlohmann::json::array();
    for(const auto id:persistedLoot.learnedCorpseIds)
        if(id) learnedCorpseIds.push_back(id);

    nlohmann::json lcfg={
      {"enabled",lootRuntimeReady&&items},
      {"loot_on_screen",false},
      {"only_list",only},
      {"radius",radius},
      {"max_weight",maxWeight},
      {"target_container_id",targetBagId},
      {"auto_detect_corpse",true},
      {"corpse_item_ids",nlohmann::json::array({std::max(1,corpseId)})},
      {"learned_corpse_ids",learnedCorpseIds},
      {"nested_container_ids",nestedJsonIds},
      {"recursive_container_depth",3},
      {"item_ids",lootJsonIds},
      {"drop_item",{
          {"enabled",false},
          {"item_ids",csvIds(dropIds)},
          {"destination","ground_near_corpse"},
          {"status","pending_native_drop_validation"}
      }}
    };
    WriteJsonIfChanged("loot.json",lcfg);
    CardEnd();
}

static void DrawAlarms(const Theme& t) {
    static bool stuck=false,pm=false,blank=false,mana=false,screen=false,low=false,disc=false,attack=false,macro=false;
    static int lowhp=50; static char vip[96]="Vip1, Vip2";
    CardBegin("alarms","Alarms",CardIcon::Bell,ImVec4(1.0f,0.72f,0.05f,1),155,t);
    if(ImGui::BeginTable("alarm_controls",2,ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::Checkbox("Walker Stuck",&stuck);
        ImGui::TableSetColumnIndex(1); ImGui::Checkbox("Low Health",&low);
        ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::Checkbox("Private Message",&pm);
        ImGui::TableSetColumnIndex(1); ImGui::Checkbox("Disconnected",&disc);
        ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::Checkbox("Out of Blank Runes",&blank);
        ImGui::TableSetColumnIndex(1); ImGui::Checkbox("Player Attack",&attack);
        ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::Checkbox("No Mana Region",&mana);
        ImGui::TableSetColumnIndex(1); ImGui::Checkbox("Macro Test",&macro);
        ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::Checkbox("Player on Screen",&screen);
        ImGui::EndTable();
    }
    SmallInt("HP (%)",&lowhp,40);
    SmallInput("VIP",vip,sizeof(vip),155);
    CardEnd();
}

static void DrawHotkeys(const Theme& t) {
    static const char* leftK[]={"F1","F2","F3","F4","F5","F6","F7","F8"};
    static const char* leftA[]={"Mana Shield","Anti-Paralyze","Heal Friend","Haste","Logout","SSA","UH","Auto Loot"};
    static const char* rightK[]={"F9","F10","F11","F12","N+","N-","N*","N/"};
    static const char* rightA[]={"Food","Fishing","Re-Ammo","Rune Maker","Exevo","Exura Vita","Utani Hur","Exori"};
    static char leftV[8][32]={{"exiva"},{"eRing"},{"exura"},{"utani hur"},{"/logout"},{"exori san"},{"uh"},{"loot all"}};
    static char rightV[8][32]={{"use food"},{"fish"},{"reammo"},{"make rune"},{"exevo gran mas"},{"exura vita"},{"utani hur"},{"exori"}};
    CardBegin("hotkeys","Hotkeys",CardIcon::Keyboard,ImVec4(0.12f,0.62f,1.0f,1),290,t);
    if(ImGui::BeginTable("hk",3,ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Key",0,0.45f); ImGui::TableSetupColumn("Action",0,1.2f); ImGui::TableSetupColumn("Value",0,1.25f);
        for(int i=0;i<8;i++) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(leftK[i]);
            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(leftA[i]);
            ImGui::TableSetColumnIndex(2); ImGui::PushID(i); SmallInput("##lv",leftV[i],32,-1); ImGui::PopID();
        }
        for(int i=0;i<8;i++) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(rightK[i]);
            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(rightA[i]);
            ImGui::TableSetColumnIndex(2); ImGui::PushID(i+20); SmallInput("##rv",rightV[i],32,-1); ImGui::PopID();
        }
        ImGui::EndTable();
    }
    CardEnd();
}

static void RefreshDisplayedLook() {
    RefreshLookResult();
    UpdateRamoniaPassiveLookId();

    if(g_ramoniaBridgeReady.load() && g_ramoniaBridgeVersion.load()>=25){
        const auto id=g_ramoniaPassiveLookId.load();
        if(id){
            g_lastLook.valid=true;
            g_lastLook.id=static_cast<int>(id);
            g_lastLook.onGround=false;
            g_lastLook.x=0;
            g_lastLook.y=0;
            g_lastLook.z=0;
        }
    }
}

#include "VisualExplore.hpp"

static void DrawFooter(const Theme& t) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg,t.panel); ImGui::PushStyleColor(ImGuiCol_Border,t.border); ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding,7);
    if(g_cardCanvasWidth>0)ImGui::SetNextWindowContentSize(ImVec2(g_cardCanvasWidth,0));
    ImGui::BeginChild("footer",ImVec2(0,92),ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    ImDrawList* d=ImGui::GetWindowDrawList(); ImVec2 wp=ImGui::GetWindowPos(); DrawCardIcon(d,CardIcon::Info,ImVec2(wp.x+17,wp.y+15),Col(t.blue));
    ImGui::SetCursorPosX(30); ImGui::PushFont(g_fontBold?g_fontBold:g_font); ImGui::TextUnformatted("Runtime / Build"); ImGui::PopFont();
    const auto* st=g_session?&g_session->status():nullptr; const bool connected=st&&st->localPlayerResolved&&g_livePlayer.valid;
    ImGui::SameLine(150); ImGui::TextColored(connected?t.green:t.muted,connected?"● LIVE":"○ WAIT"); ImGui::SameLine(225); ImGui::Text("PID: %lu",st?(unsigned long)st->pid:0ul); ImGui::SameLine(325); ImGui::Text("Client: %s",(st&&!st->exe.empty())?st->exe.c_str():"-");
    ImGui::Separator();
    ImGui::Text("Profile: %s",(st&&st->profileMatched)?st->profileName.c_str():"-"); ImGui::SameLine(325);
    if(g_livePlayer.valid) ImGui::TextColored(t.green,"HP %.0f/%.0f (%.0f%%) Mana %.0f/%.0f (%.0f%%) Pos %d,%d,%d",g_livePlayer.health,g_livePlayer.maxHealth,g_livePlayer.healthPercent(),g_livePlayer.mana,g_livePlayer.maxMana,g_livePlayer.manaPercent(),g_livePlayer.position.x,g_livePlayer.position.y,(int)g_livePlayer.position.z);
    else ImGui::TextDisabled("Player snapshot aguardando runtime...");
    ImGui::Separator();
    const auto* p=g_session?g_session->profile():nullptr;
    if(p){ auto caps=crome::Capabilities::from(*p); ImGui::Text("Read: Player %s | Map %s | Containers %s",caps.playerRead?"READY":"MISS",caps.mapRead?"READY":"MISS",caps.containerRead?"READY":"MISS"); ImGui::SameLine(420); ImGui::TextColored(g_nativeRuntimeLoaded?t.green:ImVec4(1.0f,0.58f,0.25f,1.0f),"Native: %s",g_nativeRuntimeStatus.c_str()); }
    if(g_ramoniaPid.load()){
        ImGui::TextColored(g_ramoniaBridgeReady.load()?t.green:ImVec4(1.0f,0.58f,0.25f,1.0f),
                           "Ramonia PID %lu | %s",
                           (unsigned long)g_ramoniaPid.load(), RamoniaStatusText().c_str());
    }
    if(g_lastLook.valid && g_lastLook.id>0){
        if(g_lastLook.onGround) ImGui::Text("Look ID: %d   Pos: %d,%d,%d",g_lastLook.id,g_lastLook.x,g_lastLook.y,g_lastLook.z);
        else ImGui::Text("Look ID: %d",g_lastLook.id);
    } else ImGui::TextDisabled("Look ID: -");
    ImGui::EndChild(); ImGui::PopStyleVar(); ImGui::PopStyleColor(2);
}

static void DrawRuntimeGuard(const Theme& t) {
    if (g_ramoniaPid.load()) return;
    const auto* profile=g_session ? g_session->profile() : nullptr;
    if (!profile || (!profile->nativeActionsReady&&!crome::olders_baseline::matches(static_cast<DWORD>(profile->build.timestamp),static_cast<DWORD>(profile->build.sizeOfImage),std::strtoull(profile->build.textHash.c_str(),nullptr,16)))) return;
    const auto* sessionStatus = g_session ? &g_session->status() : nullptr;
    const bool nonOldersProfile = sessionStatus && sessionStatus->profileMatched && sessionStatus->exe.find("Olders") == std::string::npos && sessionStatus->exe.find("olders") == std::string::npos;
    if (nonOldersProfile) return; // permite validar builds como Classico em modo leitura sem exigir o runtime Olders.
    const DWORD pid = FindProcessByName(L"Olders_OpenGL.exe");
    if (g_nativeRuntimeLoaded) return;
    ImGui::OpenPopup("Native runtime required");
    ImGui::SetNextWindowSize(ImVec2(440,0), ImGuiCond_Always);
    if (ImGui::BeginPopupModal("Native runtime required", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::PushFont(g_fontBold?g_fontBold:g_font);
        ImGui::TextColored(ImVec4(1.0f,0.42f,0.32f,1.0f),"Native runtime nao foi carregado");
        ImGui::PopFont();
        ImGui::Spacing();
        if (!pid) {
            ImGui::TextWrapped("Abra o Olders_OpenGL.exe. O Crome libera os modulos somente depois que o runtime nativo estiver conectado.");
        } else {
            ImGui::TextWrapped("Healing, Cavebot, Targeting, Loot e Tools permanecem bloqueados ate o runtime conectar.");
            ImGui::TextDisabled("Status: %s", g_nativeRuntimeStatus.c_str());
        }
        ImGui::Spacing();
        if (pid) {
            if (ImGui::Button("Retry Runtime", ImVec2(150,28))) RetryNativeRuntime();
        } else {
            ImGui::BeginDisabled(); ImGui::Button("Retry Runtime", ImVec2(150,28)); ImGui::EndDisabled();
        }
        ImGui::SameLine(); ImGui::TextDisabled("Sem reinjecao automatica em loop.");
        ImGui::EndPopup();
    }
}

static void ResizeCompactWindow() {
    RECT rect{}, work{};
    GetWindowRect(g_hWnd, &rect);
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const bool popup = g_largePopupOpen;
    const bool expanded = g_compactPanel != CompactPanel::None;
    const int width = std::min(popup ? 620 : expanded && (g_widePanel || (g_compactPanel==CompactPanel::Tools && g_toolsTab==3)) ? 520 : 260,
                               static_cast<int>(work.right-work.left));
    int desiredHeight=100;
    if(expanded) {
        switch(g_compactPanel) {
            case CompactPanel::Healing: desiredHeight=g_healingTab==0?290:g_healingTab==1?230:240; break;
            case CompactPanel::Targeting: desiredHeight=240; break;
            case CompactPanel::Loot: desiredHeight=g_nativeRuntimeLoaded?310:240; break;
            case CompactPanel::Hotkeys: desiredHeight=400; break;
            case CompactPanel::Tools: desiredHeight=g_toolsTab==3?560:g_toolsTab==0?(g_nativeRuntimeLoaded?350:285):g_toolsTab==1?280:230; break;
            default: desiredHeight=275; break;
        }
    }
    const int height = std::min(popup?520:desiredHeight, static_cast<int>(work.bottom-work.top));
    const int x = std::clamp(static_cast<int>(rect.left), static_cast<int>(work.left),
                             static_cast<int>(work.right)-width);
    const int y = std::clamp(static_cast<int>(rect.top), static_cast<int>(work.top),
                             static_cast<int>(work.bottom)-height);
    if (rect.right-rect.left != width || rect.bottom-rect.top != height)
        SetWindowPos(g_hWnd, nullptr, x, y, width, height,
                     SWP_NOZORDER | SWP_NOACTIVATE);
}

static void DrawTopBar(const Theme& t) {
    const float width = ImGui::GetWindowSize().x;
    ImGui::SetCursorPos(ImVec2(6, 5));
    ImGui::PushFont(g_fontBold);
    ImGui::TextUnformatted("Crome");
    ImGui::PopFont();
    ImGui::SetCursorPos(ImVec2(0, 0));
    ImGui::InvisibleButton("##drag", ImVec2(width-155, 23));
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        ReleaseCapture();
        SendMessageW(g_hWnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    }
    ImGui::SetCursorPos(ImVec2(width-153, 3));
    ImGui::PushStyleColor(ImGuiCol_Button,t.panel2);
    auto tool = [](const char* label, const char* tip) {
        const bool clicked = ImGui::Button(label, ImVec2(18, 18));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        return clicked;
    };
    if (tool("O", "Carregar profile")) {
        std::filesystem::path path;
        if (PickProfilePath(false, path) && LoadCromeProfileBundle(path)) g_walkerIds.loaded=false;
    }
    ImGui::SameLine(0, 3);
    if (tool("S", "Salvar profile")) {
        std::filesystem::path path;
        if (PickProfilePath(true, path)) SaveCromeProfileBundle(path);
    }
    ImGui::SameLine(0, 3);
    ImGui::PushStyleColor(ImGuiCol_Button, g_alwaysOnTop ? t.blue : t.panel2);
    if (tool("P", "Fixar acima do jogo")) {
        g_alwaysOnTop = !g_alwaysOnTop;
        SetWindowPos(g_hWnd, g_alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
                     0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 3);
    if (tool("+", "Ampliar/reduzir painel")) g_widePanel = !g_widePanel;
    ImGui::SameLine(0, 3);
    if (tool("D", "Alternar tema claro/escuro")) g_dark = !g_dark;
    ImGui::SameLine(0, 3);
    if (tool("-", "Minimizar")) ShowWindow(g_hWnd, SW_MINIMIZE);
    ImGui::SameLine(0, 3);
    if (tool("X", "Fechar Crome")) g_running = false;
    ImGui::PopStyleColor();
    ImGui::SetCursorPos(ImVec2(4, 27));
}

static void DrawCompactStatus(const Theme& t) {
    const auto* status = g_session ? &g_session->status() : nullptr;
    const DWORD pid = g_ramoniaPid.load() ? g_ramoniaPid.load() : status ? status->pid : 0;
    const bool connected = g_ramoniaBridgeReady.load() || (status && status->localPlayerResolved && g_livePlayer.valid);
    ImGui::Separator();
    ImGui::TextColored(connected ? t.green : t.muted, "%s", connected ? "Conectado" : "Offline");
    ImGui::SameLine();
    ImGui::TextDisabled("PID: %lu", static_cast<unsigned long>(pid));
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text("Runtime: %s", g_nativeRuntimeStatus.c_str());
        if (status) ImGui::Text("Cliente: %s", status->exe.c_str());
        ImGui::EndTooltip();
    }
    ImGui::SameLine();
    if (g_lastLook.valid && g_lastLook.id > 0) {
        ImGui::TextDisabled("ID: %d", g_lastLook.id);
    } else ImGui::TextDisabled("ID: -");
}

static void DrawUI() {
    if (!g_previewMode) {
        RefreshRuntime();
        RefreshNativeRuntime();
    }
    RefreshDisplayedLook();
    try {visual_ui::tick();}
    catch(const std::exception& error){visual_ui::state.running=false;visual_ui::clearFrame();visual_ui::state.status=std::string("Visual pausado: ")+error.what();}
    const Theme t = g_dark ? DarkTheme() : LightTheme();
    ApplyTheme(t);
    ResizeCompactWindow();
    g_largePopupOpen=false;
    ImGui::SetNextWindowPos(ImVec2(0,0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("##root",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|
                 ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoScrollWithMouse|ImGuiWindowFlags_NoScrollbar|
                 (g_previewMode?ImGuiWindowFlags_NoInputs:0));
    DrawTopBar(t);
    const char* labels[] = {"Healing", "Cavebot", "Targeting", "Loot", "Hotkeys", "Tools"};
    const CompactPanel panels[] = {CompactPanel::Healing, CompactPanel::Cavebot, CompactPanel::Targeting,
                                   CompactPanel::Loot, CompactPanel::Hotkeys, CompactPanel::Tools};
    const float buttonWidth = (ImGui::GetContentRegionAvail().x-6)/3;
    for (int i=0; i<6; ++i) {
        if (i%3) ImGui::SameLine(0,3);
        ImGui::PushID(i);
        ImGui::PushStyleColor(ImGuiCol_Button, g_compactPanel==panels[i] ? t.blue : t.panel2);
        ImGui::PushStyleColor(ImGuiCol_Text, g_compactPanel==panels[i] ? ImVec4(1,1,1,1) : t.text);
        if (ImGui::Button(labels[i],ImVec2(buttonWidth,20)))
            g_compactPanel = g_compactPanel==panels[i] ? CompactPanel::None : panels[i];
        ImGui::PopStyleColor(2);
        ImGui::PopID();
    }

    int visible = -1; // Legacy drawing functions also own their configuration/tick state.
    g_targetOnly = g_compactPanel == CompactPanel::Targeting;
    if (g_compactPanel != CompactPanel::None) {
        ImGui::Separator();
        if (g_compactPanel == CompactPanel::Healing) {
            const char* tabs[]={"Healing","Party","Rune"};
            ImGui::PushID("healing-tabs");
            for(int i=0;i<3;++i) {
                if(i) ImGui::SameLine();
                if(ImGui::Selectable(tabs[i],g_healingTab==i,0,ImVec2(68,16)))g_healingTab=i;
            }
            ImGui::PopID();
            visible = g_healingTab;
        } else if(g_compactPanel == CompactPanel::Cavebot || g_compactPanel == CompactPanel::Targeting) visible=3;
        else if(g_compactPanel == CompactPanel::Loot) visible=5;
        else if(g_compactPanel == CompactPanel::Hotkeys) visible=7;
        else {
            const char* tabs[]={"Tools","Alarmes","Runtime","Visual 7.4"};
            ImGui::PushID("tools-tabs");
            const float tabWidth=(ImGui::GetContentRegionAvail().x-9)/4;
            for(int i=0;i<4;++i) {
                if(i) ImGui::SameLine();
                if(ImGui::Selectable(tabs[i],g_toolsTab==i,0,ImVec2(tabWidth,16)))g_toolsTab=i;
            }
            ImGui::PopID();
            visible = g_toolsTab==0 ? 4 : g_toolsTab==1 ? 6 : g_toolsTab==2 ? 8 : 9;
            if(g_toolsTab==2 && !g_nativeRuntimeLoaded && !g_previewMode)
                if(ImGui::SmallButton("Reconectar runtime")) RetryNativeRuntime();
        }
        const float canvas[] = {252,350,330,g_targetOnly?280.0f:390.0f,390,430,252,252,560,490};
        g_cardCanvasWidth=canvas[visible];
        const float panelHeight=std::max(60.0f,ImGui::GetContentRegionAvail().y-24);
        ImGui::BeginChild("##module-panel",ImVec2(0,panelHeight),false);
        switch(visible) {
            case 0: DrawHealing(t); break;
            case 1: DrawFriends(t); break;
            case 2: DrawRune(t); break;
            case 3: DrawCavebot(t); break;
            case 4: DrawTools(t); break;
            case 5: DrawLoot(t); break;
            case 6: DrawAlarms(t); break;
            case 7: DrawHotkeys(t); break;
            case 8: DrawFooter(t); break;
            case 9: visual_ui::draw(t); break;
        }
        ImGui::EndChild();
    }
    DrawCompactStatus(t);
    ImGui::End();

    // Continue the existing per-frame configuration sync and waypoint recording
    // even when their editor is closed. This window cannot receive input.
    ImGui::SetNextWindowPos(ImVec2(-10000,-10000));
    ImGui::SetNextWindowSize(ImVec2(560,900));
    ImGui::Begin("##background-models",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoInputs|
                 ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoBringToFrontOnFocus);
    g_cardCanvasWidth=520;
    if(visible!=0)DrawHealing(t);
    if(visible!=1)DrawFriends(t);
    if(visible!=2)DrawRune(t);
    if(visible!=3)DrawCavebot(t);
    if(visible!=4)DrawTools(t);
    if(visible!=5)DrawLoot(t);
    if(visible!=6)DrawAlarms(t);
    if(visible!=7)DrawHotkeys(t);
    if(visible!=8)DrawFooter(t);
    ImGui::End();
    g_cardCanvasWidth=0;
}

static void SaveUiPreview() {
    const int width=static_cast<int>(ImGui::GetIO().DisplaySize.x);
    const int height=static_cast<int>(ImGui::GetIO().DisplaySize.y);
    const int stride=(width*3+3)&~3;
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width)*height*3);
    glPixelStorei(GL_PACK_ALIGNMENT,1);
    glReadBuffer(GL_BACK);
    glReadPixels(0,0,width,height,GL_RGB,GL_UNSIGNED_BYTE,pixels.data());
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER info{};
    file.bfType=0x4D42; file.bfOffBits=sizeof(file)+sizeof(info);
    file.bfSize=file.bfOffBits+stride*height;
    info.biSize=sizeof(info); info.biWidth=width; info.biHeight=height;
    info.biPlanes=1; info.biBitCount=24; info.biSizeImage=stride*height;
    std::ofstream output(g_baseDir/("preview-"+g_previewName+".bmp"),std::ios::binary);
    output.write(reinterpret_cast<const char*>(&file),sizeof(file));
    output.write(reinterpret_cast<const char*>(&info),sizeof(info));
    std::vector<unsigned char> row(stride);
    for(int y=0;y<height;++y) {
        for(int x=0;x<width;++x) {
            const auto source=(static_cast<std::size_t>(y)*width+x)*3;
            row[x*3]=pixels[source+2]; row[x*3+1]=pixels[source+1]; row[x*3+2]=pixels[source];
        }
        output.write(reinterpret_cast<const char*>(row.data()),row.size());
    }
}


static bool CreateOpenGLContext(HWND hwnd) {
    g_hDC = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd); pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 32; pfd.cAlphaBits = 8; pfd.cDepthBits = 24; pfd.iLayerType = PFD_MAIN_PLANE;
    int pf = ChoosePixelFormat(g_hDC,&pfd); if(!pf) return false;
    if(!SetPixelFormat(g_hDC,pf,&pfd)) return false;
    g_hRC = wglCreateContext(g_hDC); if(!g_hRC) return false;
    return wglMakeCurrent(g_hDC,g_hRC)==TRUE;
}

static void DestroyOpenGLContext() {
    if(g_hRC){wglMakeCurrent(nullptr,nullptr);wglDeleteContext(g_hRC);g_hRC=nullptr;}
    if(g_hDC&&g_hWnd){ReleaseDC(g_hWnd,g_hDC);g_hDC=nullptr;}
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if(ImGui_ImplWin32_WndProcHandler(hWnd,msg,wParam,lParam)) return true;
    switch(msg) {
        case WM_CLOSE: g_running=false; return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_MOVING: {
            // Mantem a janela inteira dentro da area util (nao entra atras da taskbar).
            RECT work{};
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
            RECT* r = reinterpret_cast<RECT*>(lParam);
            const LONG w = r->right - r->left;
            const LONG h = r->bottom - r->top;
            if (r->left < work.left) { r->left = work.left; r->right = r->left + w; }
            if (r->top < work.top) { r->top = work.top; r->bottom = r->top + h; }
            if (r->right > work.right) { r->right = work.right; r->left = r->right - w; }
            if (r->bottom > work.bottom) { r->bottom = work.bottom; r->top = r->bottom - h; }
            return TRUE;
        }
    }
    return DefWindowProcW(hWnd,msg,wParam,lParam);
}

int WINAPI WinMain(HINSTANCE hInst,HINSTANCE, LPSTR commandLine,int) {
    const std::string args=commandLine?commandLine:"";
    const auto previewAt=args.find("--preview=");
    if(previewAt!=std::string::npos) {
        g_previewMode=true;
        std::istringstream input(args.substr(previewAt+10));
        std::string name; input>>name; g_previewName="compact";
        const char* names[]={"healing","cavebot","targeting","loot","hotkeys","tools","runtime"};
        const CompactPanel panels[]={CompactPanel::Healing,CompactPanel::Cavebot,CompactPanel::Targeting,
                                     CompactPanel::Loot,CompactPanel::Hotkeys,CompactPanel::Tools,CompactPanel::Tools};
        for(int i=0;i<7;++i)if(name==names[i]){g_previewName=name;g_compactPanel=panels[i];}
        if(name=="runtime")g_toolsTab=2;
        if(name=="visual"){g_compactPanel=CompactPanel::Tools;g_toolsTab=3;g_previewName="visual";}
        if(name=="party"||name=="rune"){g_compactPanel=CompactPanel::Healing;g_healingTab=name=="party"?1:2;g_previewName=name;}
        if(name=="alarms"){g_compactPanel=CompactPanel::Tools;g_toolsTab=1;g_previewName=name;}
        if(name=="ids"){g_compactPanel=CompactPanel::Tools;g_previewName="ids";g_previewIds=true;}
        if(args.find("--wide")!=std::string::npos){g_widePanel=true;g_previewName+="-wide";}
    }
    SetProcessDPIAware();
    WNDCLASSEXW wc{sizeof(wc),CS_OWNDC,WndProc,0,0,hInst,nullptr,LoadCursor(nullptr,IDC_ARROW),nullptr,nullptr,L"CromeImGuiClass",nullptr};
    RegisterClassExW(&wc);

    // Usa a area util do Windows (exclui taskbar) para que toda a UI fique sempre visivel.
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int workW = work.right - work.left;
    const int workH = work.bottom - work.top;

    const int W = std::min(260,workW);
    const int H = std::min(100,workH);
    int sx = work.right-W-12;
    int sy = work.top+24;
    g_hWnd=CreateWindowExW(WS_EX_APPWINDOW|WS_EX_TOPMOST,wc.lpszClassName,L"Crome",WS_POPUP,sx,sy,W,H,nullptr,nullptr,hInst,nullptr);
    if(!g_hWnd) return 1;
    int corner=2; DwmSetWindowAttribute(g_hWnd,33,&corner,sizeof(corner));
    if(!CreateOpenGLContext(g_hWnd)) return 2;
    if(!g_previewMode) ShowWindow(g_hWnd,SW_SHOWDEFAULT);
    UpdateWindow(g_hWnd);

    // Runtime/profile connection. The UI stays open even when no client is running.
    {
        wchar_t exePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr,exePath,MAX_PATH);
        g_baseDir=std::filesystem::path(exePath).parent_path();
        if(!g_previewMode) {
            g_session=std::make_unique<crome::ClientSession>();
            g_session->loadProfiles(g_baseDir / "profiles");
            g_session->refresh(true);
        }
    }

    if(!g_previewMode) {
        g_ramoniaWorkerRun.store(true);
        g_ramoniaWorker = std::thread(RamoniaTargetWorker);
    }

    IMGUI_CHECKVERSION(); ImGui::CreateContext();
    ImGuiIO& io=ImGui::GetIO(); io.IniFilename=nullptr; io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;
    if(GetFileAttributesA("C:\\Windows\\Fonts\\segoeui.ttf")!=INVALID_FILE_ATTRIBUTES) {
        g_font=io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf",10.0f);
        g_fontBold=io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisb.ttf",10.0f);
        g_fontTitle=io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisb.ttf",11.0f);
    } else { g_font=io.Fonts->AddFontDefault(); g_fontBold=g_font; g_fontTitle=g_font; }

    ImGui_ImplWin32_InitForOpenGL(g_hWnd);
    ImGui_ImplOpenGL3_Init("#version 130");

    MSG msg{};
    int previewFrames=0;
    while(g_running) {
        while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); if(msg.message==WM_QUIT) g_running=false; }
        if(!g_running) break;
        ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
        DrawUI();
        ImGui::Render();
        Theme t=g_dark?DarkTheme():LightTheme();
        glViewport(0,0,(GLsizei)io.DisplaySize.x,(GLsizei)io.DisplaySize.y);
        glClearColor(t.bg.x,t.bg.y,t.bg.z,1.0f); glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if(g_previewMode && ++previewFrames>=5) { SaveUiPreview(); g_running=false; }
        SwapBuffers(g_hDC);
        Sleep(8);
    }

    g_ramoniaWorkerRun.store(false);
    if(g_ramoniaWorker.joinable()) g_ramoniaWorker.join();

    if(!g_previewMode)visual_ui::save();
    if(visual_ui::state.texture)glDeleteTextures(1,&visual_ui::state.texture);

    ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    DestroyOpenGLContext(); DestroyWindow(g_hWnd); UnregisterClassW(wc.lpszClassName,hInst);
    return 0;
}
