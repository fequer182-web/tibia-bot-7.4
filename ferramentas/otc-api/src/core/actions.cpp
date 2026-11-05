#include "otc_api/actions.h"

#include "otc_api/container_reader.h"
#include "otc_api/creature_reader.h"
#include "otc_api/logger.h"
#include "otc_api/memory.h"
#include "otc_api/pipe_server.h"
#include "otc_api/player_reader.h"
#include "otc_api/profile.h"

#include <nlohmann/json.hpp>

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

namespace otc {
namespace {

using nlohmann::json;

std::mutex g_mtx;

constexpr int kStepHoldMs = 80;
constexpr int kFocusMs = 80;
constexpr std::size_t kMaxSay = 255;
constexpr UINT kWmTalk = WM_APP + 51;
constexpr UINT kWmWalk = WM_APP + 52;
constexpr UINT kWmUse = WM_APP + 53;
constexpr UINT kWmUseWith = WM_APP + 54;
constexpr UINT kWmAttack = WM_APP + 55;
constexpr UINT kWmMove = WM_APP + 56;
constexpr UINT kWmFollow = WM_APP + 57;
constexpr UINT kWmLook = WM_APP + 58;
constexpr UINT kWmClick = WM_APP + 59;
constexpr UINT kWmLight = WM_APP + 60;
constexpr UINT kWmLogout = WM_APP + 61;
constexpr int kMessageSay = 1;
constexpr int kMessageWhisper = 2;
constexpr int kMessageYell = 3;

// This OTClient Position is int32 x,y + 16-bit z in a 12-byte slot (matches
// profile positionLayout=int32). autoWalk takes it by value, plus bool retry.
struct OtcPos {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
};

#if defined(_M_IX86)
using TalkChannelFn = void(__thiscall*)(void* self, int mode, int channelId,
                                        const std::string& msg);
using AutoWalkFn = bool(__thiscall*)(void* self, OtcPos dest, bool retry);
using UseFn = void(__thiscall*)(void* self, const void* thing);
using UseWithFn = void(__thiscall*)(void* self, const void* item, const void* toThing, int subType);
using AttackFn = void(__thiscall*)(void* self, std::uintptr_t ptr, std::uintptr_t rep);
using AttackRawFn = void(__thiscall*)(void* self, std::uintptr_t ptr);
using AttackRawBoolFn = void(__thiscall*)(void* self, std::uintptr_t ptr, bool flag);
using ChaseFn = void(__thiscall*)(void* self, int mode);
using MoveFn = void(__thiscall*)(void* self, const void* thing, const OtcPos* toPos, int count);
using GetTileFn = void*(__thiscall*)(void* self, const OtcPos* pos);
using TopUseFn = void(__thiscall*)(void* self, void* ret);
using LookFn = void(__thiscall*)(void* self, const void* thing, int isBattleList);
using SafeLogoutFn = void(__thiscall*)(void* self);
#else
using TalkChannelFn = void (*)(void* self, int mode, int channelId, const std::string& msg);
using AutoWalkFn = bool (*)(void* self, OtcPos dest, bool retry);
using UseFn = void (*)(void* self, const void* thing);
using UseWithFn = void (*)(void* self, const void* item, const void* toThing, int subType);
using AttackFn = void (*)(void* self, const void* creature, bool localOnly);
using FollowFn = void (*)(void* self, const void* creature);
using AttackRawFn = void (*)(void* self, std::uintptr_t ptr);
using AttackRawBoolFn = void (*)(void* self, std::uintptr_t ptr, bool flag);
using ChaseFn = void (*)(void* self, int mode);
using MoveFn = void (*)(void* self, const void* thing, const OtcPos* toPos, int count);
using GetTileFn = void* (*)(void* self, const OtcPos* pos);
using TopUseFn = void (*)(void* self, void* ret);
using LookFn = void (*)(void* self, const void* thing, int isBattleList);
using SafeLogoutFn = void (*)(void* self);
#endif

struct TalkJob {
    std::string text;
    int mode = kMessageSay;
    bool ok = false;
    HANDLE done = nullptr;
};

struct LogoutJob {
    bool ok = false;
    std::atomic<int> phase{0}; // 0 queued, 1 executing, 2 canceled
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~LogoutJob() { if (done) CloseHandle(done); }
};

struct WalkJob {
    void* self = nullptr;
    OtcPos dest{};
    bool walked = false;
    bool sehOk = false;
    HANDLE done = nullptr;
};

struct UseJob {
    std::uintptr_t ptr = 0;
    std::uintptr_t rep = 0;
    bool ok = false;
    HANDLE done = nullptr;
};

struct Sp {
    std::uintptr_t ptr = 0;
    std::uintptr_t rep = 0;
};

struct UseWithJob {
    Sp from;
    Sp to;
    Sp fallback;
    bool rawFrom = false;
    bool rawTo = false;
    bool rawFallback = false;
    bool needTile = false;
    OtcPos tile{};
    bool ok = false;
    bool missingTile = false;
    HANDLE done = nullptr;
};

struct AttackJob {
    Sp creature;
    bool rawCreature = false;
    bool cancel = false;
    int chase = -1; // -1 leave, 0 DontChase, 1 ChaseOpponent
    bool ok = false;
    HANDLE done = nullptr;
};

struct MoveJob {
    Sp item;
    bool rawItem = false;
    OtcPos dest{};
    int count = 1;
    bool ok = false;
    HANDLE done = nullptr;
};

struct LookJob {
    Sp thing;
    bool rawThing = false;
    bool needTile = false;
    bool missingTile = false;
    OtcPos tile{};
    bool ok = false;
    HANDLE done = nullptr;
};

struct ClickJob {
    Sp thing;
    bool rawThing = false;
    OtcPos tile{};
    bool missingTile = false;
    bool ok = false;
    HANDLE done = nullptr;
    int expectedItemId = 0;
    int itemIdOffset = 0;
    ~ClickJob() { if (done) CloseHandle(done); }
};

struct LightJob {
    void* uiMap = nullptr;
    float intensity = 0.0f;
    bool ok = false;
    std::atomic<int> phase{0};
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~LightJob() { if (done) CloseHandle(done); }
};

HWND g_hookedHwnd = nullptr;
WNDPROC g_prevProc = nullptr;
ActionLayout g_talk;
bool g_talkTried = false;
bool g_logoutTried = false;
bool g_walkTried = false;
bool g_useTried = false;
bool g_useWithTried = false;
bool g_attackTried = false;
bool g_followTried = false;
bool g_chaseTried = false;
bool g_moveTried = false;
bool g_mapTried = false;
bool g_lookTried = false;
bool g_lightTried = false;
std::uintptr_t g_uiMap = 0;
std::int32_t g_uiMapViewOffset = 0;
float g_savedAmbientLight = 0.0f;
bool g_savedAmbientLightValid = false;

std::mutex g_lookMtx;
LookInfo g_lastLook;
std::mutex g_lookPatchMtx;
std::uintptr_t g_lookFn = 0;
bool g_lookHooked = false;
bool g_lookPatchOn = false;
volatile LONG g_inLookHook = 0;
#if defined(_M_X64)
constexpr int kLookPatchLen = 12;
#else
constexpr int kLookPatchLen = 5;
#endif
unsigned char g_lookOrig[16]{};
unsigned char g_lookJmp[16]{};

struct LookLayout {
    int itemIdOff = 48;
    int itemCountOff = 52;
    int creatureIdOff = 0;
    int nameOff = 0;
};
LookLayout g_lookLay;

void refreshLookLayout(const Profile& prof) {
    if (prof.inventory.present && prof.inventory.itemIdOffset)
        g_lookLay.itemIdOff = prof.inventory.itemIdOffset;
    else if (prof.containers.present && prof.containers.itemIdOffset)
        g_lookLay.itemIdOff = prof.containers.itemIdOffset;
    else if (prof.map.present && prof.map.itemIdOffset)
        g_lookLay.itemIdOff = prof.map.itemIdOffset;
    if (prof.inventory.present && prof.inventory.itemCountOffset)
        g_lookLay.itemCountOff = prof.inventory.itemCountOffset;
    else if (prof.containers.present && prof.containers.itemCountOffset)
        g_lookLay.itemCountOff = prof.containers.itemCountOffset;
    else if (prof.map.present && prof.map.itemCountOffset)
        g_lookLay.itemCountOff = prof.map.itemCountOffset;
    if (prof.creatures.present) {
        if (prof.creatures.idOffset)
            g_lookLay.creatureIdOff = prof.creatures.idOffset;
        else if (prof.creatures.nameOffset)
            g_lookLay.creatureIdOff = prof.creatures.nameOffset - 4;
        g_lookLay.nameOff = prof.creatures.nameOffset;
    }
}

struct EnumState {
    HWND hwnd = nullptr;
    int area = 0;
    bool visible = false;
};

BOOL CALLBACK enumWindowsProc(HWND hwnd, LPARAM lp) {
    auto* st = reinterpret_cast<EnumState*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId())
        return TRUE;
    if (GetWindow(hwnd, GW_OWNER))
        return TRUE;
    RECT r{};
    if (!GetClientRect(hwnd, &r))
        return TRUE;
    const int w = r.right - r.left;
    const int h = r.bottom - r.top;
    if (w < 320 || h < 240)
        return TRUE;
    const int area = w * h;
    const bool visible = IsWindowVisible(hwnd) != FALSE;
    // Preserve the established visible-window selection. Hidden top-level windows
    // are only an additive fallback for clients that hide their main HWND when
    // minimized to the taskbar/tray.
    if (!st->hwnd || (visible && !st->visible) || (visible == st->visible && area > st->area)) {
        st->area = area;
        st->hwnd = hwnd;
        st->visible = visible;
    }
    return TRUE;
}

HWND findGameWindow() {
    EnumState st;
    EnumWindows(enumWindowsProc, reinterpret_cast<LPARAM>(&st));
    return st.hwnd;
}

bool isExtended(WORD vk) {
    switch (vk) {
    case VK_UP:
    case VK_DOWN:
    case VK_LEFT:
    case VK_RIGHT:
    case VK_INSERT:
    case VK_DELETE:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
        return true;
    default:
        return false;
    }
}

bool sendVk(WORD vk, bool down) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    if (isExtended(vk))
        in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    if (!down)
        in.ki.dwFlags |= KEYEVENTF_KEYUP;
    return SendInput(1, &in, sizeof(INPUT)) == 1;
}

void focusWindow(HWND hwnd) {
    if (!hwnd)
        return;
    if (IsIconic(hwnd))
        ShowWindow(hwnd, SW_RESTORE);

    const DWORD gameTid = GetWindowThreadProcessId(hwnd, nullptr);
    const DWORD ourTid = GetCurrentThreadId();
    const bool attached = gameTid != ourTid && AttachThreadInput(ourTid, gameTid, TRUE);

    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);

    if (attached)
        AttachThreadInput(ourTid, gameTid, FALSE);

    Sleep(kFocusMs);
    if (GetForegroundWindow() != hwnd)
        LOG("action: window not foreground hwnd=%p", hwnd);
}

std::string lowerCopy(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

struct DirKeys {
    WORD a = 0;
    WORD b = 0;
    bool ok = false;
};

DirKeys parseDirection(const std::string& raw) {
    const std::string d = lowerCopy(raw);
    DirKeys k;
    k.ok = true;
    if (d == "north" || d == "n" || d == "up" || d == "0")
        k.a = VK_UP;
    else if (d == "east" || d == "e" || d == "right" || d == "1")
        k.a = VK_RIGHT;
    else if (d == "south" || d == "s" || d == "down" || d == "2")
        k.a = VK_DOWN;
    else if (d == "west" || d == "w" || d == "left" || d == "3")
        k.a = VK_LEFT;
    else if (d == "northeast" || d == "ne" || d == "4") {
        k.a = VK_UP;
        k.b = VK_RIGHT;
    } else if (d == "southeast" || d == "se" || d == "5") {
        k.a = VK_DOWN;
        k.b = VK_RIGHT;
    } else if (d == "southwest" || d == "sw" || d == "6") {
        k.a = VK_DOWN;
        k.b = VK_LEFT;
    } else if (d == "northwest" || d == "nw" || d == "7") {
        k.a = VK_UP;
        k.b = VK_LEFT;
    } else {
        k.ok = false;
    }
    return k;
}

int parseSayMode(const std::string& mode) {
    const std::string m = lowerCopy(mode);
    if (m == "whisper" || m == "2")
        return kMessageWhisper;
    if (m == "yell" || m == "3")
        return kMessageYell;
    return kMessageSay;
}

ActionResult fail(const char* err) {
    return ActionResult{false, err, {}};
}

bool inImage(std::uintptr_t p) {
    const std::uintptr_t base = moduleBase();
    const std::size_t img = moduleImageSize();
    return base && img && p >= base && p < base + img;
}

bool isExec(std::uintptr_t p) {
    if (!inImage(p))
        return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(p), &mbi, sizeof(mbi)))
        return false;
    if (memPageGuarded(mbi.Protect))
        return false;
    const DWORD prot = mbi.Protect & 0xFF;
    return prot == PAGE_EXECUTE || prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE;
}

bool isWritable(std::uintptr_t p) {
    if (!inImage(p))
        return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(p), &mbi, sizeof(mbi)))
        return false;
    if (mbi.State != MEM_COMMIT)
        return false;
    if (memPageGuarded(mbi.Protect))
        return false;
    const DWORD prot = mbi.Protect & 0xFF;
    return prot == PAGE_READWRITE || prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READWRITE;
}

bool looksLikeVtable(std::uintptr_t obj) {
    std::uintptr_t vptr = 0;
    if (!readPtr(obj, vptr) || !inImage(vptr))
        return false;
    std::uintptr_t fn = 0;
    if (!readPtr(vptr, fn) || !isExec(fn))
        return false;
    return true;
}

bool plausibleMapPos(int x, int y, int z) {
    return x >= 0 && x <= 65535 && y >= 0 && y <= 65535 && z >= 0 && z <= 15;
}

std::string imageRttiName(std::uintptr_t obj) {
    std::uintptr_t vtbl = 0;
    if (!readPtr(obj, vtbl) || !inImage(vtbl))
        return {};
    std::uintptr_t col = 0;
    if (!readPtr(vtbl - sizeof(void*), col) || !inImage(col))
        return {};
    std::uintptr_t td = 0;
#if defined(_WIN64)
    std::uint32_t tdRva = 0;
    if (!memReadT(col + 12, tdRva))
        return {};
    td = moduleBase() + static_cast<std::uintptr_t>(tdRva);
#else
    if (!readPtr(col + 12, td) || !inImage(td))
        return {};
#endif
    if (!inImage(td))
        return {};
    char name[96]{};
    if (!memRead(reinterpret_cast<const void*>(td + sizeof(void*) * 2), name,
                 sizeof(name) - 1))
        return {};
    name[sizeof(name) - 1] = 0;
    return name;
}

bool isLocalPlayerObject(std::uintptr_t p) {
    const std::string n = imageRttiName(p);
    return n.find("LocalPlayer") != std::string::npos && n.find("_Ref_count") == std::string::npos;
}

bool isTileObject(std::uintptr_t p) {
    const std::string n = imageRttiName(p);
    return n.find("Tile") != std::string::npos && n.find("_Ref_count") == std::string::npos;
}

void* tileFromGetTileSlot(void* slotRet) {
    const auto slot = reinterpret_cast<std::uintptr_t>(slotRet);
    if (!looksLikeHeapPtr(slot))
        return nullptr;
    if (isTileObject(slot)) {
        LOG("getTile eax is Tile object %p rtti=%s", slotRet, imageRttiName(slot).c_str());
        return slotRet;
    }

    std::uintptr_t ptr = 0;
    std::uintptr_t rep = 0;
    std::uintptr_t d2 = 0;
    std::uintptr_t d3 = 0;
    readPtr(slot, ptr);
    readPtr(slot + sizeof(void*), rep);
    readPtr(slot + sizeof(void*) * 2, d2);
    readPtr(slot + sizeof(void*) * 3, d3);
    LOG("getTile slot=%p [0]=%p [ptr]=%p [2ptr]=%p [3ptr]=%p rtti0=%s rttiT=%s",
        slotRet,
        reinterpret_cast<void*>(ptr),
        reinterpret_cast<void*>(rep),
        reinterpret_cast<void*>(d2),
        reinterpret_cast<void*>(d3),
        imageRttiName(slot).c_str(),
        imageRttiName(ptr).c_str());

    if (isTileObject(ptr))
        return reinterpret_cast<void*>(ptr);

    if (completeSharedPtr(ptr, rep) && isTileObject(ptr)) {
        LOG("getTile TilePtr unwrapped tile=%p rtti=%s",
            reinterpret_cast<void*>(ptr),
            imageRttiName(ptr).c_str());
        return reinterpret_cast<void*>(ptr);
    }
    return nullptr;
}

bool validThingHandle(const Sp& h, bool raw) {
    if (!h.ptr)
        return false;
    if (raw)
        return h.rep == 0 && looksLikeHeapPtr(h.ptr);
    return looksLikeSharedRep(h.rep);
}

bool normalizeThingHandle(Sp& h, int width, bool& raw) {
    raw = false;
    if (!h.ptr)
        return false;
    if (width >= static_cast<int>(sizeof(void*) * 2))
        return completeSharedPtr(h.ptr, h.rep) && looksLikeSharedRep(h.rep);
    if (width == static_cast<int>(sizeof(void*))) {
        h.rep = 0;
        raw = true;
        return looksLikeHeapPtr(h.ptr);
    }
    return false;
}

int mapThingWidth(const Profile& p) {
    if (p.map.present) {
        if (p.map.itemPtrSize > 0)
            return p.map.itemPtrSize;
        if (p.map.tilePtrSize > 0)
            return p.map.tilePtrSize;
    }
    if (p.inventory.present && p.inventory.ptrSize > 0)
        return p.inventory.ptrSize;
    if (p.containers.present && p.containers.itemPtrSize > 0)
        return p.containers.itemPtrSize;
    return 0;
}

int creatureThingWidth(const Profile& p) {
    if (!p.creatures.present)
        return 0;
    if (p.creatures.creaturePtrSize > 0)
        return p.creatures.creaturePtrSize;
    return p.creatures.ptrSize;
}

bool prepareMapThingHandle(Sp& h, bool& raw) {
    Profile p;
    copyLiveProfile(p);
    return normalizeThingHandle(h, mapThingWidth(p), raw);
}

bool prepareCreatureThingHandle(Sp& h, bool& raw) {
    Profile p;
    copyLiveProfile(p);
    return normalizeThingHandle(h, creatureThingWidth(p), raw);
}

bool rawCreatureHandles() {
    Profile p;
    copyLiveProfile(p);
    return creatureThingWidth(p) == static_cast<int>(sizeof(void*));
}

bool thingFromTileVectorAt(void* tile, int off, int width, bool forLook, Sp* out, bool* rawOut) {
    out->ptr = 0;
    out->rep = 0;
    if (rawOut)
        *rawOut = false;
    if (!tile || off <= 0 || width < static_cast<int>(sizeof(void*)))
        return false;

    const auto t = reinterpret_cast<std::uintptr_t>(tile);
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
    if (!readPtr(t + static_cast<std::uintptr_t>(off), begin) ||
        !readPtr(t + static_cast<std::uintptr_t>(off + sizeof(void*)), end))
        return false;
    if (end < begin || ((end - begin) % static_cast<std::uintptr_t>(width)) != 0 ||
        end - begin > static_cast<std::uintptr_t>(width * 64))
        return false;

    auto skipFx = [](const std::string& n) {
        return n.find("Effect") != std::string::npos ||
               n.find("Missile") != std::string::npos ||
               n.find("AnimatedText") != std::string::npos;
    };
    auto skipUse = [&](const std::string& n) {
        return skipFx(n) || n.find("Creature") != std::string::npos ||
               n.find("LocalPlayer") != std::string::npos ||
               n.find("Player") != std::string::npos ||
               n.find("Monster") != std::string::npos || n.find("Npc") != std::string::npos;
    };

    Sp lastItem{};
    Sp lastAny{};
    bool lastItemRaw = false;
    bool lastAnyRaw = false;
    for (std::uintptr_t p = end; p >= begin + static_cast<std::uintptr_t>(width);
         p -= static_cast<std::uintptr_t>(width)) {
        const std::uintptr_t slot = p - static_cast<std::uintptr_t>(width);
        Sp candidate{};
        if (!readPtr(slot, candidate.ptr) || !candidate.ptr)
            continue;
        if (width >= static_cast<int>(sizeof(void*) * 2))
            readPtr(slot + sizeof(void*), candidate.rep);
        bool raw = false;
        if (!normalizeThingHandle(candidate, width, raw))
            continue;
        const std::string n = imageRttiName(candidate.ptr);
        if (forLook ? skipFx(n) : skipUse(n))
            continue;
        if (forLook) {
            *out = candidate;
            if (rawOut)
                *rawOut = raw;
            LOG("tile look thing off=%d width=%d raw=%d ptr=%p rep=%p rtti=%s", off, width,
                raw ? 1 : 0, reinterpret_cast<void*>(candidate.ptr),
                reinterpret_cast<void*>(candidate.rep), n.c_str());
            return true;
        }
        if (!lastAny.ptr) {
            lastAny = candidate;
            lastAnyRaw = raw;
        }
        if (!lastItem.ptr && n.find("Item") != std::string::npos) {
            lastItem = candidate;
            lastItemRaw = raw;
            break;
        }
    }

    if (lastItem.ptr) {
        *out = lastItem;
        if (rawOut)
            *rawOut = lastItemRaw;
    } else if (lastAny.ptr) {
        *out = lastAny;
        if (rawOut)
            *rawOut = lastAnyRaw;
    } else {
        return false;
    }
    LOG("tile use thing off=%d width=%d raw=%d ptr=%p rep=%p rtti=%s", off, width,
        rawOut && *rawOut ? 1 : 0, reinterpret_cast<void*>(out->ptr),
        reinterpret_cast<void*>(out->rep), imageRttiName(out->ptr).c_str());
    return true;
}

bool thingFromTile(void* tile, bool forLook, Sp* out, bool* rawOut) {
    Profile p;
    copyLiveProfile(p);
    const int width = mapThingWidth(p);
    if (p.map.present && p.map.thingsOffset > 0 && width > 0 &&
        thingFromTileVectorAt(tile, p.map.thingsOffset, width, forLook, out, rawOut))
        return true;

    // Preserve the established MSVC shared_ptr fallback for profiles that do
    // not yet carry a calibrated map layout.
    return thingFromTileVectorAt(tile, 0x28, static_cast<int>(sizeof(void*) * 2), forLook, out,
                                 rawOut);
}

bool thingFromTileVector(void* tile, Sp* out, bool* rawOut = nullptr) {
    return thingFromTile(tile, false, out, rawOut);
}

bool thingFromTileLook(void* tile, Sp* out, bool* rawOut = nullptr) {
    return thingFromTile(tile, true, out, rawOut);
}

bool hasPlausiblePos(std::uintptr_t p) {
    const int offs[] = {32, 16, 20, 24, 36, 40};
    for (int off : offs) {
        std::int32_t x = 0;
        std::int32_t y = 0;
        std::int32_t z = 0;
        if (!memReadT(p + static_cast<std::uintptr_t>(off), x) ||
            !memReadT(p + static_cast<std::uintptr_t>(off) + 4, y) ||
            !memReadT(p + static_cast<std::uintptr_t>(off) + 8, z))
            continue;
        if (plausibleMapPos(x, y, z))
            return true;
    }
    return false;
}

void logWalkCand(const char* tag, std::uintptr_t p) {
    if (!p) {
        LOG("walk %s null", tag);
        return;
    }
    std::uintptr_t vtbl = 0;
    std::uint32_t uses = 0;
    std::uint32_t weaks = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
    readPtr(p, vtbl);
    memReadT(p + 4, uses);
    memReadT(p + 8, weaks);
    memReadT(p + 32, x);
    memReadT(p + 36, y);
    memReadT(p + 40, z);
    const std::string rtti = imageRttiName(p);
    LOG("walk %s p=%p vtbl=%p rtti=%s uses=%u weaks=%u pos32=%d,%d,%d", tag,
        reinterpret_cast<void*>(p), reinterpret_cast<void*>(vtbl),
        rtti.empty() ? "?" : rtti.c_str(), uses, weaks, x, y, z);
}

std::uintptr_t unwrapLocalPlayer(std::uintptr_t p) {
    if (!p)
        return 0;
    if (isLocalPlayerObject(p))
        return p;
    for (int off = 4; off <= 64; off += 4) {
        const std::uintptr_t t = p + static_cast<std::uintptr_t>(off);
        if (isLocalPlayerObject(t))
            return t;
    }
    if (looksLikeVtable(p) && hasPlausiblePos(p) &&
        imageRttiName(p).find("_Ref_count") == std::string::npos)
        return p;
    for (int off = 8; off <= 64; off += 4) {
        const std::uintptr_t t = p + static_cast<std::uintptr_t>(off);
        if (looksLikeVtable(t) && hasPlausiblePos(t) &&
            imageRttiName(t).find("_Ref_count") == std::string::npos)
            return t;
    }
    return 0;
}

std::uintptr_t resolveLocalPlayerThis() {
    std::uintptr_t ptr = 0;
    std::uintptr_t rep = 0;
    if (g_talk.gameRva) {
        const std::uintptr_t game =
            moduleBase() +
            static_cast<std::uintptr_t>(static_cast<std::intptr_t>(g_talk.gameRva));
        readPtr(game + sizeof(void*), ptr);
        readPtr(game + 2 * sizeof(void*), rep);
    }
    const std::uintptr_t chain = currentPlayerObject();
    logWalkCand("ptr", ptr);
    logWalkCand("rep", rep);
    logWalkCand("chain", chain);

    if (const auto t = unwrapLocalPlayer(ptr)) {
        logWalkCand("this", t);
        return t;
    }
    if (const auto t = unwrapLocalPlayer(rep)) {
        logWalkCand("this", t);
        return t;
    }
    if (const auto t = unwrapLocalPlayer(chain)) {
        logWalkCand("this", t);
        return t;
    }
    LOG("walk local player not unwrapped");
    return 0;
}

std::uintptr_t fieldAddr(const std::vector<ChainStep>& chain) {
    if (chain.empty())
        return 0;
    return moduleBase() +
           static_cast<std::uintptr_t>(static_cast<std::intptr_t>(chain[0].offset));
}

struct ImageRange {
    std::uintptr_t start = 0;
    std::uintptr_t end = 0;
};

void collectImageRanges(std::vector<ImageRange>& readable, std::vector<ImageRange>& exec) {
    auto* base = reinterpret_cast<std::uint8_t*>(hostModule());
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return;
    auto* sec = IMAGE_FIRST_SECTION(nt);
    const std::uintptr_t mod = moduleBase();
    for (UINT i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const std::uintptr_t start = mod + sec[i].VirtualAddress;
        std::size_t sz = sec[i].SizeOfRawData;
        if (sec[i].Misc.VirtualSize && (sz == 0 || sec[i].Misc.VirtualSize < sz))
            sz = sec[i].Misc.VirtualSize;
        if (!sz)
            continue;
        const DWORD ch = sec[i].Characteristics;
        ImageRange r{start, start + sz};
        if (ch & IMAGE_SCN_MEM_EXECUTE)
            exec.push_back(r);
        else if ((ch & IMAGE_SCN_MEM_READ) && !(ch & IMAGE_SCN_MEM_DISCARDABLE))
            readable.push_back(r);
    }
}

void findBytes(const std::vector<ImageRange>& ranges, const void* needle, std::size_t n,
               std::vector<std::uintptr_t>& out, std::size_t cap) {
    if (!needle || n == 0)
        return;
    for (const auto& r : ranges) {
        if (out.size() >= cap)
            return;
        if (r.end <= r.start || r.end - r.start < n)
            continue;
        const std::size_t len = static_cast<std::size_t>(r.end - r.start);
        if (!memReadable(reinterpret_cast<const void*>(r.start), len))
            continue;
        const auto* p = reinterpret_cast<const std::uint8_t*>(r.start);
        for (std::size_t i = 0; i + n <= len; ++i) {
            if (std::memcmp(p + i, needle, n) == 0) {
                out.push_back(r.start + i);
                if (out.size() >= cap)
                    return;
            }
        }
    }
}

struct ImmPtr {
    std::uintptr_t at = 0;
    std::uintptr_t val = 0;
};

void collectImmPtrs(std::uintptr_t window, std::vector<ImmPtr>& out) {
    constexpr std::size_t kBack = 16;
    constexpr std::size_t kFwd = 96;
    const std::uintptr_t from = window > kBack ? window - kBack : window;
    const std::uintptr_t to = window + kFwd;
    for (std::uintptr_t p = from; p + 5 <= to; ++p) {
        std::uint8_t op = 0;
        if (!memReadT(p, op))
            continue;
        std::uint32_t imm = 0;
        bool take = false;
        if (op == 0x68)
            take = memReadT(p + 1, imm);
        else if (op >= 0xB8 && op <= 0xBF)
            take = memReadT(p + 1, imm);
        if (!take || !inImage(imm))
            continue;
        out.push_back(ImmPtr{p + 1, imm});
    }
}

#if defined(_M_X64)
void findRipRelativeRefs(const std::vector<ImageRange>& ranges, std::uintptr_t target,
                         std::vector<std::uintptr_t>& out, std::size_t cap) {
    for (const auto& r : ranges) {
        const auto len = static_cast<std::size_t>(r.end - r.start);
        if (len < 7 || !memReadable(reinterpret_cast<const void*>(r.start), len))
            continue;
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(r.start);
        for (std::size_t i = 0; i + 7 <= len && out.size() < cap; ++i) {
            const auto rex = bytes[i];
            const auto op = bytes[i + 1];
            const auto modrm = bytes[i + 2];
            std::int32_t disp = 0;
            if ((rex != 0x48 && rex != 0x4C) || op != 0x8D ||
                (modrm & 0xC7) != 0x05)
                continue;
            std::memcpy(&disp, bytes + i + 3, sizeof(disp));
            const auto p = r.start + i;
            if (p + 7 + static_cast<std::intptr_t>(disp) == target)
                out.push_back(p);
        }
        if (out.size() >= cap)
            return;
    }
}

void collectRipRelativePtrs(std::uintptr_t window, std::vector<ImmPtr>& out) {
    constexpr std::size_t kBack = 16;
    constexpr std::size_t kFwd = 96;
    const auto from = window > kBack ? window - kBack : window;
    const auto to = window + kFwd;
    for (auto p = from; p + 7 <= to; ++p) {
        std::uint8_t rex = 0, op = 0, modrm = 0;
        std::int32_t disp = 0;
        if (!memReadT(p, rex) || (rex != 0x48 && rex != 0x4C) ||
            !memReadT(p + 1, op) || op != 0x8D || !memReadT(p + 2, modrm) ||
            (modrm & 0xC7) != 0x05 || !memReadT(p + 3, disp))
            continue;
        const auto target = p + 7 + static_cast<std::intptr_t>(disp);
        if (inImage(target))
            out.push_back(ImmPtr{p + 3, target});
    }
}
#endif

std::uintptr_t findGameFromMembers(const Profile& prof) {
    std::vector<std::uintptr_t> members;
    if (const auto a = fieldAddr(prof.chain))
        members.push_back(a);
    if (const auto a = fieldAddr(prof.containers.chain))
        members.push_back(a);
    if (const auto a = fieldAddr(prof.creatures.chain))
        members.push_back(a);
    if (members.size() < 2)
        return 0;

    std::uintptr_t earliest = members[0];
    std::uintptr_t latest = members[0];
    for (auto m : members) {
        if (m < earliest)
            earliest = m;
        if (m > latest)
            latest = m;
    }
    if (latest - earliest > 0x4000)
        return 0;

    std::uintptr_t best = 0;
    for (int off = 0; off <= 4096; off += static_cast<int>(sizeof(void*))) {
        if (earliest < static_cast<std::uintptr_t>(off))
            break;
        const std::uintptr_t cand = earliest - static_cast<std::uintptr_t>(off);
        if (!isWritable(cand) || !looksLikeVtable(cand))
            continue;
        bool ok = true;
        for (auto m : members) {
            if (m < cand || m >= cand + 0x4000) {
                ok = false;
                break;
            }
        }
        if (!ok)
            continue;
        best = cand;
        break;
    }
#if defined(_M_X64)
    // Some x64 OTClient builds embed Game in writable module storage and its
    // first field is LocalPlayerPtr rather than a vtable. The independently
    // calibrated P1/P2/P3 fields above still bound and identify the object.
    if (!best && isWritable(earliest)) {
        std::uintptr_t localPlayer = 0;
        if (readPtr(earliest, localPlayer) && looksLikeHeapPtr(localPlayer))
            best = earliest;
    }
#endif
    return best;
}

bool pickTalkBind(const Profile& prof, std::uintptr_t& gameOut, std::uintptr_t& fnOut,
                  std::vector<std::uintptr_t>* gameCandidates = nullptr) {
    const DWORD t0 = GetTickCount();
    gameOut = findGameFromMembers(prof);
    fnOut = 0;
    if (!gameOut)
        LOG("talk scan: g_game not found from P1-P3 members; trying Lua bind metadata");

    std::vector<ImageRange> readable;
    std::vector<ImageRange> exec;
    collectImageRanges(readable, exec);

    std::vector<std::uintptr_t> strs;
    findBytes(readable, "talkChannel", 12, strs, 12);
    if (strs.empty()) {
        LOG("talk scan: string talkChannel not found (%lums)", GetTickCount() - t0);
        return false;
    }

    std::uintptr_t bestFn = 0;
    int bestScore = -1;
    int xrefCount = 0;

    for (auto str : strs) {
        std::vector<std::uintptr_t> xrefs;
        const auto nle = static_cast<std::uint32_t>(str);
        findBytes(exec, &nle, 4, xrefs, 32);
#if defined(_M_X64)
        findRipRelativeRefs(exec, str, xrefs, 32);
#endif
        xrefCount += static_cast<int>(xrefs.size());
        for (auto xref : xrefs) {
            std::vector<ImmPtr> imms;
            collectImmPtrs(xref, imms);
#if defined(_M_X64)
            collectRipRelativePtrs(xref, imms);
#endif
            std::uintptr_t fn = 0;
            std::uintptr_t gameBind = 0;
            std::uintptr_t gameNearLp = 0;
            std::size_t fnDist = static_cast<std::size_t>(-1);
            const std::uintptr_t lp = fieldAddr(prof.chain);
            for (const auto& imm : imms) {
                if (imm.at < xref)
                    continue;
                const std::size_t d = imm.at - xref;
                if (isExec(imm.val) && imm.val != str && d < fnDist) {
                    fnDist = d;
                    fn = imm.val;
                }
                if (!isWritable(imm.val) || isExec(imm.val))
                    continue;
                if (gameCandidates &&
                    std::find(gameCandidates->begin(), gameCandidates->end(), imm.val) ==
                        gameCandidates->end())
                    gameCandidates->push_back(imm.val);
                if (!gameBind)
                    gameBind = imm.val;
                if (lp && lp >= imm.val && lp - imm.val <= 16)
                    gameNearLp = imm.val;
            }
            if (gameNearLp)
                gameBind = gameNearLp;
            if (!fn)
                continue;
            int score = 4;
            if (fnDist < 80)
                score += 4;
            if (gameBind)
                score += 6;
            if (gameNearLp)
                score += 4;
            if (score > bestScore) {
                bestScore = score;
                bestFn = fn;
                if (gameBind)
                    gameOut = gameBind;
            }
        }
    }

    if (!bestFn || !gameOut) {
        LOG("talk scan: bind site not resolved game=%p strs=%zu xrefs=%d (%lums)",
            reinterpret_cast<void*>(gameOut), strs.size(), xrefCount, GetTickCount() - t0);
        return false;
    }

    fnOut = bestFn;
    const std::uintptr_t lp = fieldAddr(prof.chain);
    const int lpDelta = (lp && gameOut && lp >= gameOut)
                            ? static_cast<int>(lp - gameOut)
                            : -1;
    LOG("talk scan: g_game=%p talkChannel=%p lpDelta=%d score=%d strs=%zu (%lums)",
        reinterpret_cast<void*>(gameOut), reinterpret_cast<void*>(fnOut), lpDelta, bestScore,
        strs.size(), GetTickCount() - t0);
    return lpDelta < 0 || lpDelta <= 64;
}

std::uintptr_t gameAddress(const Profile& prof) {
    const std::uintptr_t base = moduleBase();
    auto fromRva = [&](std::int32_t rva) -> std::uintptr_t {
        if (!rva)
            return 0;
        return base + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(rva));
    };
    if (const auto g = fromRva(g_talk.gameRva))
        return g;
    if (const auto g = fromRva(prof.actions.gameRva)) {
        const auto lp = fieldAddr(prof.chain);
        if (!lp || (lp >= g && lp - g <= 64))
            return g;
    }
    return findGameFromMembers(prof);
}

bool pickAutoWalk(std::uintptr_t game, std::uintptr_t& localFnOut, std::uintptr_t& gameFnOut) {
    const DWORD t0 = GetTickCount();
    localFnOut = 0;
    gameFnOut = 0;
    if (!game) {
        LOG("walk scan: g_game is null");
        return false;
    }

    std::vector<ImageRange> readable;
    std::vector<ImageRange> exec;
    collectImageRanges(readable, exec);

    std::vector<std::uintptr_t> strs;
    findBytes(readable, "autoWalk", 9, strs, 12);
    if (strs.empty()) {
        LOG("walk scan: string autoWalk not found (%lums)", GetTickCount() - t0);
        return false;
    }

    std::uintptr_t bestLocal = 0;
    std::size_t bestLocalDist = static_cast<std::size_t>(-1);
    std::uintptr_t bestGameFn = 0;
    int xrefCount = 0;
    int skippedGame = 0;

    for (auto str : strs) {
        std::vector<std::uintptr_t> xrefs;
        const auto nle = static_cast<std::uint32_t>(str);
        findBytes(exec, &nle, 4, xrefs, 32);
#if defined(_M_X64)
        findRipRelativeRefs(exec, str, xrefs, 32);
#endif
        xrefCount += static_cast<int>(xrefs.size());
        for (auto xref : xrefs) {
            std::vector<ImmPtr> imms;
            collectImmPtrs(xref, imms);
#if defined(_M_X64)
            collectRipRelativePtrs(xref, imms);
#endif
            std::uintptr_t fn = 0;
            std::size_t fnDist = static_cast<std::size_t>(-1);
            bool sawGame = false;
            for (const auto& imm : imms) {
                if (imm.val == game)
                    sawGame = true;
                if (imm.at < xref)
                    continue;
                const std::size_t d = imm.at - xref;
                if (isExec(imm.val) && imm.val != str && d < fnDist) {
                    fnDist = d;
                    fn = imm.val;
                }
            }
            if (!fn)
                continue;
            if (sawGame) {
                ++skippedGame;
                if (!bestGameFn)
                    bestGameFn = fn;
                continue;
            }
            if (fnDist < bestLocalDist) {
                bestLocalDist = fnDist;
                bestLocal = fn;
            }
        }
    }

    if (!bestLocal) {
        LOG("walk scan: LocalPlayer::autoWalk not resolved skipGame=%p xrefs=%d skipped=%d "
            "strs=%zu (%lums)",
            reinterpret_cast<void*>(bestGameFn), xrefCount, skippedGame, strs.size(),
            GetTickCount() - t0);
        return false;
    }
    if (bestGameFn && bestLocal == bestGameFn) {
        LOG("walk scan: refused Game::autoWalk %p", reinterpret_cast<void*>(bestLocal));
        return false;
    }

    localFnOut = bestLocal;
    gameFnOut = bestGameFn;
    LOG("walk scan: LocalPlayer::autoWalk=%p (skip Game::autoWalk=%p) dist=%zu strs=%zu (%lums)",
        reinterpret_cast<void*>(localFnOut), reinterpret_cast<void*>(gameFnOut), bestLocalDist,
        strs.size(), GetTickCount() - t0);
    return true;
}

struct BindHit {
    std::uintptr_t fn = 0;
    std::uintptr_t game = 0;
    std::uintptr_t xref = 0;
    std::size_t fnDist = static_cast<std::size_t>(-1);
    bool sawGame = false;
};

void collectBinds(const char* name, std::size_t n, std::uintptr_t gameHint, std::vector<BindHit>& out) {
    out.clear();
    if (!name || n == 0)
        return;

    std::vector<ImageRange> readable;
    std::vector<ImageRange> exec;
    collectImageRanges(readable, exec);

    std::vector<std::uintptr_t> strs;
    findBytes(readable, name, n, strs, 16);
    for (auto str : strs) {
        std::vector<std::uintptr_t> xrefs;
        const auto nle = static_cast<std::uint32_t>(str);
        findBytes(exec, &nle, 4, xrefs, 32);
#if defined(_M_X64)
        findRipRelativeRefs(exec, str, xrefs, 32);
#endif
        for (auto xref : xrefs) {
            std::vector<ImmPtr> imms;
            collectImmPtrs(xref, imms);
#if defined(_M_X64)
            collectRipRelativePtrs(xref, imms);
#endif
            BindHit hit;
            hit.xref = xref;
            for (const auto& imm : imms) {
                if (gameHint && imm.val == gameHint)
                    hit.sawGame = true;
                if (imm.at < xref)
                    continue;
                const std::size_t d = imm.at - xref;
                if (isExec(imm.val) && imm.val != str && d < hit.fnDist) {
                    hit.fnDist = d;
                    hit.fn = imm.val;
                }
                if (!isWritable(imm.val) || isExec(imm.val))
                    continue;
                if (!hit.game)
                    hit.game = imm.val;
            }
            if (hit.fn)
                out.push_back(hit);
        }
    }
}

void collectWideGameBinds(const char* name, std::size_t n, std::uintptr_t gameHint,
                          std::vector<BindHit>& out, std::size_t& stringCount,
                          std::size_t& xrefCount) {
    out.clear();
    stringCount = 0;
    xrefCount = 0;
    if (!name || n == 0 || !gameHint)
        return;

    std::vector<ImageRange> readable;
    std::vector<ImageRange> exec;
    collectImageRanges(readable, exec);

    // Short Lua names such as "use" can occur many times. This fallback runs only
    // after the established path misses, retains its forward registration order,
    // and scans all candidate string references in one executable-image pass.
    std::vector<std::uintptr_t> strs;
    findBytes(readable, name, n, strs, 64);
    stringCount = strs.size();
    if (strs.empty())
        return;
    std::sort(strs.begin(), strs.end());

    constexpr std::size_t kXrefCap = 256;
    for (const auto& r : exec) {
        if (xrefCount >= kXrefCap)
            break;
        if (r.end <= r.start || r.end - r.start < sizeof(std::uint32_t))
            continue;
        const std::size_t len = static_cast<std::size_t>(r.end - r.start);
        if (!memReadable(reinterpret_cast<const void*>(r.start), len))
            continue;

        const auto* bytes = reinterpret_cast<const std::uint8_t*>(r.start);
        for (std::size_t i = 0; i + sizeof(std::uint32_t) <= len; ++i) {
            std::uint32_t value = 0;
            std::memcpy(&value, bytes + i, sizeof(value));
            if (!std::binary_search(strs.begin(), strs.end(),
                                    static_cast<std::uintptr_t>(value)))
                continue;
            if (++xrefCount > kXrefCap)
                return;

            const std::uintptr_t xref = r.start + i;
            const std::uintptr_t str = value;
            std::vector<ImmPtr> imms;
            collectImmPtrs(xref, imms);
            BindHit hit;
            hit.xref = xref;
            for (const auto& imm : imms) {
                if (imm.val == gameHint)
                    hit.sawGame = true;
                if (imm.at < xref)
                    continue;
                const std::size_t distance = imm.at - xref;
                if (isExec(imm.val) && imm.val != str && distance < hit.fnDist) {
                    hit.fnDist = distance;
                    hit.fn = imm.val;
                }
            }
            if (hit.sawGame && hit.fn) {
                hit.game = gameHint;
                out.push_back(hit);
            }
        }
    }
}

bool pickUseFn(std::uintptr_t game, std::uintptr_t& fnOut, std::uintptr_t& withOut) {
    const DWORD t0 = GetTickCount();
    fnOut = 0;
    withOut = 0;
    if (!game) {
        LOG("use scan: g_game is null");
        return false;
    }

    std::vector<BindHit> withHits;
    collectBinds("useWith", 8, game, withHits);
    std::uintptr_t withFn = 0;
    std::uintptr_t withXref = 0;
    for (const auto& h : withHits) {
        if (h.sawGame || h.game == game) {
            withFn = h.fn;
            withXref = h.xref;
            break;
        }
    }
    if (!withFn && !withHits.empty()) {
        withFn = withHits[0].fn;
        withXref = withHits[0].xref;
    }
    withOut = withFn;

    std::vector<BindHit> useHits;
    collectBinds("use", 4, game, useHits);

    std::uintptr_t bestFn = 0;
    int bestScore = -1;
    int skippedWith = 0;
    for (const auto& h : useHits) {
        if (!h.fn)
            continue;
        if (withFn && h.fn == withFn) {
            ++skippedWith;
            continue;
        }
        int score = 2;
        if (h.fnDist < 80)
            score += 4;
        if (h.sawGame || h.game == game)
            score += 8;
        if (withXref) {
            const std::uintptr_t a = h.xref > withXref ? h.xref - withXref : withXref - h.xref;
            if (a < 64)
                score += 20;
            else if (a < 256)
                score += 12;
            else if (a < 1024)
                score += 4;
        }
        if (score > bestScore) {
            bestScore = score;
            bestFn = h.fn;
        }
    }

    bool usedWideFallback = false;
    std::size_t wideStrings = 0;
    std::size_t wideXrefs = 0;
    std::size_t wideHits = 0;
    if (!bestFn) {
        std::vector<BindHit> hits;
        collectWideGameBinds("use", 4, game, hits, wideStrings, wideXrefs);
        wideHits = hits.size();
        for (const auto& h : hits) {
            if (!h.fn || (withFn && h.fn == withFn)) {
                if (h.fn)
                    ++skippedWith;
                continue;
            }
            int score = 10; // The fallback already requires the exact g_game reference.
            if (h.fnDist < 80)
                score += 4;
            if (withXref) {
                const std::uintptr_t a = h.xref > withXref ? h.xref - withXref
                                                           : withXref - h.xref;
                if (a < 64)
                    score += 20;
                else if (a < 256)
                    score += 12;
                else if (a < 1024)
                    score += 4;
            }
            if (score > bestScore) {
                bestScore = score;
                bestFn = h.fn;
                usedWideFallback = true;
            }
        }
    }

    if (!bestFn) {
        LOG("use scan: Game::use not resolved useWith=%p hits=%zu wide=%zu/%zu/%zu "
            "skipWith=%d (%lums)",
            reinterpret_cast<void*>(withFn), useHits.size(), wideHits, wideXrefs, wideStrings,
            skippedWith, GetTickCount() - t0);
        return false;
    }
    if (withFn && bestFn == withFn) {
        LOG("use scan: refused Game::useWith %p", reinterpret_cast<void*>(bestFn));
        return false;
    }

    fnOut = bestFn;
    LOG("use scan: Game::use=%p (skip useWith=%p) score=%d hits=%zu wide=%d/%zu/%zu/%zu "
        "(%lums)",
        reinterpret_cast<void*>(fnOut), reinterpret_cast<void*>(withFn), bestScore, useHits.size(),
        usedWideFallback ? 1 : 0, wideHits, wideXrefs, wideStrings, GetTickCount() - t0);
    return true;
}

bool pickAttackFn(std::uintptr_t game, std::uintptr_t& fnOut) {
    const DWORD t0 = GetTickCount();
    fnOut = 0;
    if (!game) {
        LOG("attack scan: g_game is null");
        return false;
    }

    std::vector<BindHit> cancelHits;
    collectBinds("cancelAttack", 13, game, cancelHits);
    std::uintptr_t cancelFn = 0;
    if (!cancelHits.empty())
        cancelFn = cancelHits[0].fn;

    std::vector<BindHit> hits;
    collectBinds("attack", 7, game, hits);
    std::uintptr_t bestFn = 0;
    int bestScore = -1;
    int skipped = 0;
    for (const auto& h : hits) {
        if (!h.fn)
            continue;
        if (cancelFn && h.fn == cancelFn) {
            ++skipped;
            continue;
        }
        int score = 2;
        if (h.fnDist < 80)
            score += 4;
        if (h.sawGame || h.game == game)
            score += 8;
        if (score > bestScore) {
            bestScore = score;
            bestFn = h.fn;
        }
    }
    if (!bestFn) {
        LOG("attack scan: Game::attack not resolved hits=%zu skipCancel=%d (%lums)", hits.size(),
            skipped, GetTickCount() - t0);
        return false;
    }
    fnOut = bestFn;
    LOG("attack scan: Game::attack=%p (skip cancelAttack=%p) score=%d hits=%zu (%lums)",
        reinterpret_cast<void*>(fnOut), reinterpret_cast<void*>(cancelFn), bestScore, hits.size(),
        GetTickCount() - t0);
    return true;
}

bool pickFollowFn(std::uintptr_t game, std::uintptr_t& fnOut) {
    const DWORD t0 = GetTickCount();
    fnOut = 0;
    if (!game) {
        LOG("follow scan: g_game is null");
        return false;
    }

    std::vector<BindHit> cancelHits;
    collectBinds("cancelFollow", 13, game, cancelHits);
    std::uintptr_t cancelFn = 0;
    if (!cancelHits.empty())
        cancelFn = cancelHits[0].fn;

    std::vector<BindHit> hits;
    collectBinds("follow", 7, game, hits);
    std::uintptr_t bestFn = 0;
    int bestScore = -1;
    int skipped = 0;
    for (const auto& h : hits) {
        if (!h.fn)
            continue;
        if (cancelFn && h.fn == cancelFn) {
            ++skipped;
            continue;
        }
        int score = 2;
        if (h.fnDist < 80)
            score += 4;
        if (h.sawGame || h.game == game)
            score += 8;
        if (score > bestScore) {
            bestScore = score;
            bestFn = h.fn;
        }
    }
    if (!bestFn) {
        LOG("follow scan: Game::follow not resolved hits=%zu skipCancel=%d (%lums)", hits.size(),
            skipped, GetTickCount() - t0);
        return false;
    }
    fnOut = bestFn;
    LOG("follow scan: Game::follow=%p (skip cancelFollow=%p) score=%d hits=%zu (%lums)",
        reinterpret_cast<void*>(fnOut), reinterpret_cast<void*>(cancelFn), bestScore, hits.size(),
        GetTickCount() - t0);
    return true;
}

bool pickChaseFn(std::uintptr_t game, std::uintptr_t& fnOut) {
    const DWORD t0 = GetTickCount();
    fnOut = 0;
    if (!game) {
        LOG("chase scan: g_game is null");
        return false;
    }

    std::vector<BindHit> hits;
    collectBinds("setChaseMode", 13, game, hits);
    std::uintptr_t bestFn = 0;
    int bestScore = -1;
    for (const auto& h : hits) {
        if (!h.fn)
            continue;
        int score = 2;
        if (h.fnDist < 80)
            score += 4;
        if (h.sawGame || h.game == game)
            score += 8;
        if (score > bestScore) {
            bestScore = score;
            bestFn = h.fn;
        }
    }
    if (!bestFn) {
        LOG("chase scan: Game::setChaseMode not resolved hits=%zu (%lums)", hits.size(),
            GetTickCount() - t0);
        return false;
    }
    fnOut = bestFn;
    LOG("chase scan: Game::setChaseMode=%p score=%d hits=%zu (%lums)",
        reinterpret_cast<void*>(fnOut), bestScore, hits.size(), GetTickCount() - t0);
    return true;
}

bool pickSafeLogoutFn(std::uintptr_t game, std::uintptr_t& fnOut) {
    const DWORD t0 = GetTickCount();
    fnOut = 0;
    if (!game) {
        LOG("logout scan: g_game is null");
        return false;
    }

    std::vector<BindHit> hits;
    collectBinds("safeLogout", 11, game, hits);
    std::uintptr_t bestFn = 0;
    int bestScore = -1;
    for (const auto& h : hits) {
        if (!h.fn || (!h.sawGame && h.game != game))
            continue;
        int score = 8;
        if (h.fnDist < 80)
            score += 4;
        if (h.sawGame)
            score += 4;
        if (score > bestScore) {
            bestScore = score;
            bestFn = h.fn;
        }
    }
    if (!bestFn) {
        LOG("logout scan: Game::safeLogout not resolved hits=%zu (%lums)", hits.size(),
            GetTickCount() - t0);
        return false;
    }
    fnOut = bestFn;
    LOG("logout scan: Game::safeLogout=%p score=%d hits=%zu (%lums)",
        reinterpret_cast<void*>(fnOut), bestScore, hits.size(), GetTickCount() - t0);
    return true;
}

bool pickLookFn(std::uintptr_t game, std::uintptr_t& fnOut) {
    const DWORD t0 = GetTickCount();
    fnOut = 0;
    if (!game) {
        LOG("look scan: g_game is null");
        return false;
    }

    std::vector<BindHit> hits;
    collectBinds("look", 5, game, hits);
    std::uintptr_t bestFn = 0;
    int bestScore = -1;
    for (const auto& h : hits) {
        if (!h.fn)
            continue;
        int score = 2;
        if (h.fnDist < 80)
            score += 4;
        if (h.sawGame || h.game == game)
            score += 8;
        if (score > bestScore) {
            bestScore = score;
            bestFn = h.fn;
        }
    }
    if (!bestFn) {
        LOG("look scan: Game::look not resolved hits=%zu (%lums)", hits.size(),
            GetTickCount() - t0);
        return false;
    }
    fnOut = bestFn;
    LOG("look scan: Game::look=%p score=%d hits=%zu (%lums)", reinterpret_cast<void*>(fnOut),
        bestScore, hits.size(), GetTickCount() - t0);
    return true;
}

bool pickMoveFn(std::uintptr_t game, std::uintptr_t& fnOut) {
    const DWORD t0 = GetTickCount();
    fnOut = 0;
    if (!game) {
        LOG("move scan: g_game is null");
        return false;
    }

    std::uintptr_t skipRaw = 0;
    std::uintptr_t skipParent = 0;
    std::vector<BindHit> rawHits;
    collectBinds("moveRaw", 8, game, rawHits);
    if (!rawHits.empty())
        skipRaw = rawHits[0].fn;
    std::vector<BindHit> parentHits;
    collectBinds("moveToParentContainer", 22, game, parentHits);
    if (!parentHits.empty())
        skipParent = parentHits[0].fn;

    std::vector<BindHit> hits;
    collectBinds("move", 5, game, hits);
    std::uintptr_t bestFn = 0;
    int bestScore = -1;
    int skipped = 0;
    for (const auto& h : hits) {
        if (!h.fn)
            continue;
        if ((skipRaw && h.fn == skipRaw) || (skipParent && h.fn == skipParent)) {
            ++skipped;
            continue;
        }
        int score = 2;
        if (h.fnDist < 80)
            score += 4;
        if (h.sawGame || h.game == game)
            score += 8;
        if (score > bestScore) {
            bestScore = score;
            bestFn = h.fn;
        }
    }
    if (!bestFn) {
        LOG("move scan: Game::move not resolved hits=%zu skip=%d (%lums)", hits.size(), skipped,
            GetTickCount() - t0);
        return false;
    }
    fnOut = bestFn;
    LOG("move scan: Game::move=%p (skip raw=%p parent=%p) score=%d hits=%zu (%lums)",
        reinterpret_cast<void*>(fnOut), reinterpret_cast<void*>(skipRaw),
        reinterpret_cast<void*>(skipParent), bestScore, hits.size(), GetTickCount() - t0);
    return true;
}

bool pickMapFns(std::uintptr_t& mapOut, std::uintptr_t& getTileOut, std::uintptr_t& topUseOut) {
    const DWORD t0 = GetTickCount();
    mapOut = 0;
    getTileOut = 0;
    topUseOut = 0;

    std::vector<BindHit> tileHits;
    collectBinds("getTile", 8, 0, tileHits);
    int bestScore = -1;
    for (const auto& h : tileHits) {
        if (!h.fn)
            continue;
        int score = 1;
        if (h.fnDist < 80)
            score += 4;
        if (h.game && isWritable(h.game) && !isExec(h.game))
            score += 8;
        if (score > bestScore) {
            bestScore = score;
            getTileOut = h.fn;
            if (h.game && isWritable(h.game) && !isExec(h.game))
                mapOut = h.game;
        }
    }

    std::vector<BindHit> topHits;
    collectBinds("getTopUseThing", 15, 0, topHits);
    std::size_t bestDist = static_cast<std::size_t>(-1);
    for (const auto& h : topHits) {
        if (!h.fn)
            continue;
        if (h.fnDist < bestDist) {
            bestDist = h.fnDist;
            topUseOut = h.fn;
        }
    }

    if (!getTileOut || !topUseOut) {
        LOG("map scan: getTile=%p topUse=%p map=%p hitsTile=%zu hitsTop=%zu (%lums)",
            reinterpret_cast<void*>(getTileOut), reinterpret_cast<void*>(topUseOut),
            reinterpret_cast<void*>(mapOut), tileHits.size(), topHits.size(), GetTickCount() - t0);
        return false;
    }
    LOG("map scan: g_map=%p getTile=%p getTopUseThing=%p (%lums)", reinterpret_cast<void*>(mapOut),
        reinterpret_cast<void*>(getTileOut), reinterpret_cast<void*>(topUseOut),
        GetTickCount() - t0);
    return mapOut != 0;
}

bool sehTalkChannel(void* self, std::uintptr_t fn, int mode, const std::string* msg) {
    __try {
        reinterpret_cast<TalkChannelFn>(fn)(self, mode, 0, *msg);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool sehUse(void* self, std::uintptr_t fn, const void* thing) {
    DWORD code = 0;
    __try {
        reinterpret_cast<UseFn>(fn)(self, thing);
        return true;
    } __except ((code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)) {
        LOG("use exception code=%lu this=%p fn=%p thing=%p", code, self,
            reinterpret_cast<void*>(fn), thing);
        return false;
    }
}

bool sehUseWith(void* self, std::uintptr_t fn, const void* item, const void* toThing) {
    DWORD code = 0;
    __try {
        reinterpret_cast<UseWithFn>(fn)(self, item, toThing, 0);
        return true;
    } __except ((code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)) {
        LOG("useWith exception code=%lu this=%p fn=%p item=%p to=%p", code, self,
            reinterpret_cast<void*>(fn), item, toThing);
        return false;
    }
}

void bumpSharedUses(std::uintptr_t rep) {
    if (!rep)
        return;
    // MSVC _Ref_count_base begins with a native-width vptr, followed by the
    // 32-bit strong and weak counters. Using +4 on x64 corrupts the vptr and
    // lets the callee destroy an unbalanced temporary reference.
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(rep + sizeof(void*)));
}

bool sehSafeLogout(void* self, std::uintptr_t fn) {
    DWORD code = 0;
    __try {
        reinterpret_cast<SafeLogoutFn>(fn)(self);
        return true;
    } __except ((code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)) {
        LOG("logout exception code=%lu this=%p fn=%p", code, self,
            reinterpret_cast<void*>(fn));
        return false;
    }
}

int rawCreatureActionStackBytes(std::uintptr_t fn) {
#if defined(_M_IX86)
    // x86 __thiscall encodes the size of its explicit arguments in `ret N`.
    // Raw CreaturePtr builds exist with either CreaturePtr alone (ret 4) or
    // CreaturePtr plus a boolean option (ret 8). Inspect the selected method
    // instead of assigning either ABI to a client/build.
    constexpr std::size_t kProbeBytes = 1024;
    for (std::size_t i = 0; i + 2 < kProbeBytes; ++i) {
        std::uint8_t opcode = 0;
        if (!memReadT(fn + i, opcode))
            break;
        if (opcode != 0xC2)
            continue;

        std::uint16_t stackBytes = 0;
        if (!memReadT(fn + i + 1, stackBytes))
            break;
        if (stackBytes == sizeof(void*) || stackBytes == 2 * sizeof(void*))
            return static_cast<int>(stackBytes);
    }
#else
    (void)fn;
#endif
    return 0;
}

bool sehCreatureAction(const char* action, void* self, std::uintptr_t fn, std::uintptr_t ptr,
                       std::uintptr_t rep, bool raw) {
    const int stackBytes = raw ? rawCreatureActionStackBytes(fn) : 0;
    if (raw && stackBytes != static_cast<int>(sizeof(void*)) &&
        stackBytes != static_cast<int>(2 * sizeof(void*))) {
        LOG("%s refuse unknown raw CreaturePtr ABI fn=%p stack=%d", action,
            reinterpret_cast<void*>(fn), stackBytes);
        return false;
    }

    // Intrusive CreaturePtr builds dispatch window input into the OTClient event
    // dispatcher before invoking Game actions. Calling a non-null target directly
    // from WndProc re-enters Lua callbacks on the window thread and can deadlock.
    // Keep the established shared_ptr path untouched and allow the harmless null
    // cancel call, but refuse a raw target until a dispatcher-backed route has
    // been resolved and validated for that build.
    // A non-null intrusive attack is dispatched by callRawAttack below.
    if (raw && ptr)
        return false;

    DWORD code = 0;
    __try {
        if (raw) {
            // Passing an intrusive pointer by value performs one caller-side
            // add_ref; the callee releases that temporary on return.
            if (ptr)
                InterlockedIncrement(reinterpret_cast<volatile LONG*>(ptr + sizeof(void*)));
            if (stackBytes == static_cast<int>(2 * sizeof(void*)))
                reinterpret_cast<AttackRawBoolFn>(fn)(self, ptr, false);
            else
                reinterpret_cast<AttackRawFn>(fn)(self, ptr);
        } else {
            bumpSharedUses(rep);
#if defined(_M_IX86)
            reinterpret_cast<AttackFn>(fn)(self, ptr, rep);
#else
            // MSVC x64 passes the non-trivial 16-byte shared_ptr value through
            // a pointer to caller-created temporary storage. The callee releases
            // that temporary, balanced by bumpSharedUses above. Game::attack has
            // an additional local-only flag in R8B. In this PE a true value jumps
            // over the protocol send. The empty CreaturePtr already represents
            // cancellation, so API attack and cancel both pass false.
            const Sp creature{ptr, rep};
            if (std::strcmp(action, "attack") == 0)
                reinterpret_cast<AttackFn>(fn)(self, &creature, false);
            else
                reinterpret_cast<FollowFn>(fn)(self, &creature);
#endif
        }
        LOG("%s call ABI raw=%d stack=%d ptr=%p", action, raw ? 1 : 0,
            raw ? stackBytes : static_cast<int>(2 * sizeof(void*)),
            reinterpret_cast<void*>(ptr));
        return true;
    } __except ((code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)) {
        LOG("%s exception code=%lu this=%p fn=%p ptr=%p rep=%p raw=%d stack=%d", action, code,
            self, reinterpret_cast<void*>(fn), reinterpret_cast<void*>(ptr),
            reinterpret_cast<void*>(rep), raw ? 1 : 0, stackBytes);
        return false;
    }
}

#if defined(_M_IX86)
using AddEventFn = void*(__thiscall*)(void* dispatcher, void* result,
                                     const std::string& name,
                                     const std::function<void()>& callback, bool front);

struct IntrusiveEventResult {
    std::uintptr_t ptr = 0;
};

void releaseIntrusiveEvent(std::uintptr_t ptr) {
    if (!ptr)
        return;
    const LONG before = InterlockedDecrement(reinterpret_cast<volatile LONG*>(ptr + 4));
    if (before != 0)
        return;
    std::uintptr_t vtable = 0;
    std::uintptr_t destroy = 0;
    if (readPtr(ptr, vtable) && readPtr(vtable, destroy) && isExec(destroy)) {
        using DestroyFn = void(__thiscall*)(void*, bool);
        reinterpret_cast<DestroyFn>(destroy)(reinterpret_cast<void*>(ptr), true);
    }
}

bool resolveDispatcherAddEvent(std::uintptr_t& dispatcher, std::uintptr_t& addEvent) {
    dispatcher = 0;
    addEvent = 0;
    std::vector<ImageRange> readable;
    std::vector<ImageRange> exec;
    collectImageRanges(readable, exec);
    std::vector<std::uintptr_t> strings;
    findBytes(readable, "addEvent", 9, strings, 8);
    for (const auto str : strings) {
        const auto needle = static_cast<std::uint32_t>(str);
        std::vector<std::uintptr_t> refs;
        findBytes(exec, &needle, sizeof(needle), refs, 16);
        for (const auto ref : refs) {
            std::vector<ImmPtr> imms;
            collectImmPtrs(ref, imms);
            std::uintptr_t fn = 0;
            std::uintptr_t object = 0;
            for (std::size_t i = 1; i < imms.size(); ++i) {
                const auto& prev = imms[i - 1];
                const auto& imm = imms[i];
                std::uint8_t op1 = 0, op2 = 0;
                if (prev.at > ref && imm.at == prev.at + 5 &&
                    memReadT(prev.at - 1, op1) && op1 == 0x68 &&
                    memReadT(imm.at - 1, op2) && op2 == 0x68 &&
                    isWritable(prev.val) && !isExec(prev.val) && isExec(imm.val)) {
                    object = prev.val;
                    fn = imm.val;
                    break;
                }
            }
            if (!fn || !object)
                continue;
            // addEvent has a hidden EventPtr result and three explicit arguments.
            // Requiring ret 0x10 prevents selecting similarly named overloads.
            bool ret16 = false;
            for (std::size_t i = 0; i + 2 < 768; ++i) {
                std::uint8_t op = 0;
                std::uint16_t bytes = 0;
                if (!memReadT(fn + i, op))
                    break;
                if (op == 0xC2 && memReadT(fn + i + 1, bytes) && bytes == 0x10) {
                    ret16 = true;
                    break;
                }
            }
            if (ret16) {
                dispatcher = object;
                addEvent = fn;
                LOG("dispatcher addEvent resolved object=%p fn=%p", reinterpret_cast<void*>(object),
                    reinterpret_cast<void*>(fn));
                return true;
            }
        }
    }
    LOG("dispatcher addEvent not resolved");
    return false;
}

bool sehAddEvent(std::uintptr_t dispatcher, std::uintptr_t addEvent,
                 const std::string& name, const std::function<void()>& callback) {
    IntrusiveEventResult event;
    DWORD code = 0;
    __try {
        reinterpret_cast<AddEventFn>(addEvent)(reinterpret_cast<void*>(dispatcher), &event,
                                              name, callback, false);
        releaseIntrusiveEvent(event.ptr);
    } __except ((code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)) {
        LOG("addEvent exception code=%lu", code);
        return false;
    }
    return event.ptr != 0;
}

bool sehRawAttack(void* self, std::uintptr_t fn, std::uintptr_t ptr, int stackBytes) {
    DWORD code = 0;
    __try {
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(ptr + sizeof(void*)));
        if (stackBytes == 8)
            reinterpret_cast<AttackRawBoolFn>(fn)(self, ptr, false);
        else
            reinterpret_cast<AttackRawFn>(fn)(self, ptr);
        return true;
    } __except ((code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)) {
        LOG("attack dispatcher exception code=%lu", code);
        return false;
    }
}

bool sehChase(void* self, std::uintptr_t fn, int mode);

struct RawAttackState {
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<int> phase{0}; // queued, executing, cancelled
    bool ok = false;
    ~RawAttackState() { if (done) CloseHandle(done); }
};

bool callRawAttack(std::uint32_t creatureId, int chase) {
    static_assert(sizeof(std::function<void()>) == 40);
    static_assert(sizeof(std::string) == 24);
    std::uintptr_t dispatcher = 0;
    std::uintptr_t addEvent = 0;
    if (!resolveDispatcherAddEvent(dispatcher, addEvent))
        return false;
    const auto base = moduleBase();
    const auto fn = base + g_talk.attackRva;
    void* self = reinterpret_cast<void*>(base + g_talk.gameRva);
    const auto chaseFn = g_talk.chaseModeRva ? base + g_talk.chaseModeRva : 0;
    const int stackBytes = rawCreatureActionStackBytes(fn);
    if (stackBytes != 4 && stackBytes != 8)
        return false;
    const auto state = std::make_shared<RawAttackState>();
    if (!state->done)
        return false;
    std::function<void()> callback = [=]() {
        int queued = 0;
        if (!state->phase.compare_exchange_strong(queued, 1))
            return;
        Sp creature;
        bool raw = false;
        if (resolveCreature(creatureId, creature.ptr, creature.rep) &&
            prepareCreatureThingHandle(creature, raw) && raw) {
            const bool chaseOk = chase < 0 || (chaseFn && sehChase(self, chaseFn, chase));
            std::uintptr_t current = 0;
            readPtr(reinterpret_cast<std::uintptr_t>(self) + 8, current);
            state->ok = chaseOk && (current == creature.ptr ||
                sehRawAttack(self, fn, creature.ptr, stackBytes));
        }
        LOG("attack dispatcher completed id=%u ok=%d tid=%lu", creatureId,
            state->ok ? 1 : 0, GetCurrentThreadId());
        SetEvent(state->done);
    };
    if (!sehAddEvent(dispatcher, addEvent, "otc-api.attack", callback)) {
        int queued = 0;
        state->phase.compare_exchange_strong(queued, 2);
        return false;
    }
    if (WaitForSingleObject(state->done, 5000) != WAIT_OBJECT_0) {
        int queued = 0;
        state->phase.compare_exchange_strong(queued, 2);
        LOG("attack dispatcher timeout id=%u", creatureId);
        return false;
    }
    return state->ok;
}

struct LogoutDispatchState {
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<int> phase{0}; // queued, executing, canceled
    bool ok = false;
    ~LogoutDispatchState() { if (done) CloseHandle(done); }
};

bool callDispatcherLogout() {
    static_assert(sizeof(std::function<void()>) == 40);
    static_assert(sizeof(std::string) == 24);
    std::uintptr_t dispatcher = 0;
    std::uintptr_t addEvent = 0;
    if (!resolveDispatcherAddEvent(dispatcher, addEvent))
        return false;
    if (!g_talk.gameRva || !g_talk.safeLogoutRva)
        return false;

    const auto base = moduleBase();
    void* self = reinterpret_cast<void*>(base + g_talk.gameRva);
    const auto fn = base + g_talk.safeLogoutRva;
    const auto state = std::make_shared<LogoutDispatchState>();
    if (!state->done)
        return false;
    std::function<void()> callback = [=]() {
        int queued = 0;
        if (!state->phase.compare_exchange_strong(queued, 1))
            return;
        state->ok = sehSafeLogout(self, fn);
        LOG("logout dispatcher completed ok=%d tid=%lu", state->ok ? 1 : 0,
            GetCurrentThreadId());
        SetEvent(state->done);
    };
    if (!sehAddEvent(dispatcher, addEvent, "otc-api.logout", callback)) {
        int queued = 0;
        state->phase.compare_exchange_strong(queued, 2);
        return false;
    }
    if (WaitForSingleObject(state->done, 5000) != WAIT_OBJECT_0) {
        int queued = 0;
        state->phase.compare_exchange_strong(queued, 2);
        LOG("logout dispatcher timeout");
        return false;
    }
    return state->ok;
}
#endif

bool sehChase(void* self, std::uintptr_t fn, int mode) {
    DWORD code = 0;
    __try {
        reinterpret_cast<ChaseFn>(fn)(self, mode);
        return true;
    } __except ((code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)) {
        LOG("chase exception code=%lu this=%p fn=%p mode=%d", code, self,
            reinterpret_cast<void*>(fn), mode);
        return false;
    }
}

bool sehMove(void* self, std::uintptr_t fn, const void* thing, const OtcPos* toPos, int count) {
    DWORD code = 0;
    __try {
        reinterpret_cast<MoveFn>(fn)(self, thing, toPos, count);
        return true;
    } __except ((code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)) {
        LOG("move exception code=%lu this=%p fn=%p thing=%p dest=%d,%d,%d count=%d", code, self,
            reinterpret_cast<void*>(fn), thing, toPos ? toPos->x : 0, toPos ? toPos->y : 0,
            toPos ? toPos->z : 0, count);
        return false;
    }
}

void* sehGetTile(void* map, std::uintptr_t fn, const OtcPos* pos) {
    DWORD code = 0;
    void* ret = nullptr;
    __try {
        ret = reinterpret_cast<GetTileFn>(fn)(map, pos);
    } __except ((code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)) {
        LOG("getTile exception code=%lu map=%p fn=%p pos=%d,%d,%d", code, map,
            reinterpret_cast<void*>(fn), pos->x, pos->y, pos->z);
        return nullptr;
    }
    if (inImage(reinterpret_cast<std::uintptr_t>(ret)))
        return nullptr;
    return ret;
}

bool sehTopUse(void* tile, std::uintptr_t fn, Sp* out) {
    DWORD code = 0;
    out->ptr = 0;
    out->rep = 0;
    __try {
        reinterpret_cast<TopUseFn>(fn)(tile, out);
        return out->ptr != 0;
    } __except ((code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)) {
        LOG("getTopUseThing exception code=%lu tile=%p fn=%p", code, tile,
            reinterpret_cast<void*>(fn));
        return false;
    }
}

bool sehLook(void* self, std::uintptr_t fn, const void* thing, int isBattleList) {
    DWORD code = 0;
    __try {
        reinterpret_cast<LookFn>(fn)(self, thing, isBattleList);
        return true;
    } __except ((code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)) {
        LOG("look exception code=%lu this=%p fn=%p thing=%p", code, self,
            reinterpret_cast<void*>(fn), thing);
        return false;
    }
}

bool lookReadMsvcName(std::uintptr_t addr, std::string& out) {
    const int ps = static_cast<int>(sizeof(void*));
    std::size_t size = 0;
    std::size_t cap = 0;
    if (ps == 4) {
        std::uint32_t s = 0;
        std::uint32_t c = 0;
        if (!memReadT(addr + 16, s) || !memReadT(addr + 20, c))
            return false;
        size = s;
        cap = c;
    } else {
        if (!memReadT(addr + 16, size) || !memReadT(addr + 24, cap))
            return false;
    }
    if (size == 0 || size > 64 || cap > 4096 || size > cap)
        return false;
    char buf[65]{};
    if (cap < 16) {
        if (!memRead(reinterpret_cast<const void*>(addr), buf, size))
            return false;
    } else {
        std::uintptr_t p = 0;
        if (!readPtr(addr, p) || !p)
            return false;
        if (!memRead(reinterpret_cast<const void*>(p), buf, size))
            return false;
    }
    for (std::size_t i = 0; i < size; ++i) {
        const unsigned char ch = static_cast<unsigned char>(buf[i]);
        if (ch < 32 || ch > 126)
            return false;
    }
    out.assign(buf, size);
    return true;
}

std::string lookObjectJson(const LookInfo& info) {
    json j;
    j["kind"] = info.kind;
    j["id"] = info.id;
    if (info.kind == "item" && info.count > 0)
        j["count"] = info.count;
    if (info.hasPos) {
        j["x"] = info.x;
        j["y"] = info.y;
        j["z"] = info.z;
    }
    if (info.container >= 0)
        j["container"] = info.container;
    if (info.slot >= 0)
        j["slot"] = info.slot;
    if (!info.name.empty())
        j["name"] = info.name;
    return j.dump();
}

bool fillLookFromObj(std::uintptr_t obj, LookInfo& out) {
    out = {};
    if (!obj)
        return false;

    const std::string rtti = imageRttiName(obj);
    const bool isCreature = rtti.find("Creature") != std::string::npos ||
                            rtti.find("LocalPlayer") != std::string::npos ||
                            rtti.find("Monster") != std::string::npos ||
                            rtti.find("Npc") != std::string::npos ||
                            (rtti.find("Player") != std::string::npos &&
                             rtti.find("Item") == std::string::npos);
    const bool isItem = rtti.find("Item") != std::string::npos;

    const int itemIdOff = g_lookLay.itemIdOff;
    const int itemCountOff = g_lookLay.itemCountOff;
    const int creatureIdOff = g_lookLay.creatureIdOff;

    std::uint16_t itemId = 0;
    const bool haveItemId =
        memReadT(obj + static_cast<std::uintptr_t>(itemIdOff), itemId) && itemId >= 100 &&
        itemId < 60000;
    std::uint32_t creatureId = 0;
    const bool haveCreatureId =
        creatureIdOff != 0 &&
        memReadT(obj + static_cast<std::uintptr_t>(creatureIdOff), creatureId) && creatureId != 0;

    if (isCreature || (!isItem && haveCreatureId && creatureId >= 0x10000000u)) {
        out.kind = "creature";
        out.id = creatureId;
        if (!out.id)
            return false;
        if (g_lookLay.nameOff > 0)
            lookReadMsvcName(obj + static_cast<std::uintptr_t>(g_lookLay.nameOff), out.name);
    } else if (isItem || haveItemId) {
        out.kind = "item";
        out.id = itemId;
        if (!out.id)
            return false;
        std::int32_t n = 0;
        if (itemCountOff > 0 &&
            memReadT(obj + static_cast<std::uintptr_t>(itemCountOff), n) && n > 0)
            out.count = n;
        else
            out.count = 1;
    } else {
        return false;
    }

    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint16_t z = 0;
    if (memReadT(obj + 0x10, x) && memReadT(obj + 0x14, y) && memReadT(obj + 0x18, z)) {
        if (x == 0xFFFF) {
            if (y & 0x40) {
                out.container = y & ~0x40;
                out.slot = z;
            } else if (y >= 1 && y <= 10) {
                out.slot = y;
            }
        } else if (plausibleMapPos(x, y, z)) {
            out.hasPos = true;
            out.x = x;
            out.y = y;
            out.z = z;
        }
    }

    out.valid = true;
    return true;
}

void captureLook(const void* thingRef) {
    if (!thingRef)
        return;
    const auto addr = reinterpret_cast<std::uintptr_t>(thingRef);
    std::uintptr_t ptr = 0;
    std::uintptr_t rep = 0;
    if (!readPtr(addr, ptr) || !ptr)
        return;
    readPtr(addr + sizeof(void*), rep);
    completeSharedPtr(ptr, rep);

    LookInfo info;
    if (!fillLookFromObj(ptr, info))
        return;

    {
        std::lock_guard<std::mutex> lock(g_lookMtx);
        g_lastLook = info;
    }

    try {
        PipeServer::instance().tryEmit("look", lookObjectJson(info));
    } catch (...) {
    }
}

void lookApplyPatch(bool on) {
    if (!g_lookFn)
        return;
    auto* dst = reinterpret_cast<unsigned char*>(g_lookFn);
    const unsigned char* src = on ? g_lookJmp : g_lookOrig;
    memcpy(dst, src, static_cast<std::size_t>(kLookPatchLen));
    FlushInstructionCache(GetCurrentProcess(), dst, static_cast<std::size_t>(kLookPatchLen));
    g_lookPatchOn = on;
}

#if defined(_M_IX86)
void __fastcall lookHook(void* self, void* /*edx*/, const void* thing, int isBattleList) {
    if (InterlockedCompareExchange(&g_inLookHook, 1, 0) != 0)
        return;
    try {
        captureLook(thing);
    } catch (...) {
    }
    {
        std::lock_guard<std::mutex> lock(g_lookPatchMtx);
        lookApplyPatch(false);
        sehLook(self, g_lookFn, thing, isBattleList);
        lookApplyPatch(true);
    }
    InterlockedExchange(&g_inLookHook, 0);
}
#else
void lookHook(void* self, const void* thing, int isBattleList) {
    if (InterlockedCompareExchange(&g_inLookHook, 1, 0) != 0)
        return;
    try {
        captureLook(thing);
    } catch (...) {
    }
    {
        std::lock_guard<std::mutex> lock(g_lookPatchMtx);
        lookApplyPatch(false);
        sehLook(self, g_lookFn, thing, isBattleList);
        lookApplyPatch(true);
    }
    InterlockedExchange(&g_inLookHook, 0);
}
#endif

bool installLookHook() {
    std::lock_guard<std::mutex> lock(g_lookPatchMtx);
    if (g_lookHooked && g_lookPatchOn)
        return true;
    if (!g_lookFn || !isExec(g_lookFn))
        return false;

    memcpy(g_lookOrig, reinterpret_cast<const void*>(g_lookFn),
           static_cast<std::size_t>(kLookPatchLen));
    const auto hook = reinterpret_cast<std::uintptr_t>(&lookHook);
    memset(g_lookJmp, 0x90, sizeof(g_lookJmp));
#if defined(_M_X64)
    g_lookJmp[0] = 0x48;
    g_lookJmp[1] = 0xB8;
    memcpy(g_lookJmp + 2, &hook, 8);
    g_lookJmp[10] = 0xFF;
    g_lookJmp[11] = 0xE0;
#else
    g_lookJmp[0] = 0xE9;
    const auto rel = static_cast<std::int32_t>(hook - (g_lookFn + 5));
    memcpy(g_lookJmp + 1, &rel, 4);
#endif

    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(g_lookFn), static_cast<std::size_t>(kLookPatchLen),
                        PAGE_EXECUTE_READWRITE, &old)) {
        LOG("look hook VirtualProtect failed err=%lu", GetLastError());
        return false;
    }
    lookApplyPatch(true);
    g_lookHooked = true;
    LOG("look hooked fn=%p", reinterpret_cast<void*>(g_lookFn));
    return true;
}

#if defined(_M_IX86)
// Game autoWalk uses SSE and std::function; WndProc stack is often 4-byte aligned.
__declspec(naked) bool alignedAutoWalk(void* self, std::uintptr_t fn, int x, int y, int z) {
    __asm {
        push ebp
        mov ebp, esp
        push esi
        mov esi, esp
        and esp, 0xFFFFFFF0
        push 0
        push dword ptr[ebp + 24]
        push dword ptr[ebp + 20]
        push dword ptr[ebp + 16]
        mov ecx, dword ptr[ebp + 8]
        call dword ptr[ebp + 12]
        mov esp, esi
        pop esi
        pop ebp
        ret
    }
}
#endif

bool sehAutoWalk(void* self, std::uintptr_t fn, OtcPos dest, bool* walked) {
    DWORD code = 0;
    __try {
#if defined(_M_IX86)
        *walked = alignedAutoWalk(self, fn, dest.x, dest.y, dest.z);
#else
        *walked = reinterpret_cast<AutoWalkFn>(fn)(self, dest, false);
#endif
        return true;
    } __except ((code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)) {
        LOG("autoWalk exception code=%lu this=%p fn=%p dest=%d,%d,%d", code, self,
            reinterpret_cast<void*>(fn), dest.x, dest.y, dest.z);
        return false;
    }
}

bool sehAmbientSet(void* uiMap, std::uintptr_t fn, float value);

LRESULT CALLBACK talkWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    (void)wParam;
    if (msg == kWmTalk) {
        auto* job = reinterpret_cast<TalkJob*>(lParam);
        if (!job)
            return 0;
        job->ok = false;
        if (g_talk.gameRva && g_talk.talkChannelRva) {
            const std::uintptr_t base = moduleBase();
            void* self = reinterpret_cast<void*>(
                base + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(g_talk.gameRva)));
            const std::uintptr_t fn = base + static_cast<std::uintptr_t>(
                                                 static_cast<std::intptr_t>(g_talk.talkChannelRva));
            job->ok = sehTalkChannel(self, fn, job->mode, &job->text);
        }
        if (job->done)
            SetEvent(job->done);
        return 1;
    }
    if (msg == kWmLogout) {
        std::unique_ptr<std::shared_ptr<LogoutJob>> holder(
            reinterpret_cast<std::shared_ptr<LogoutJob>*>(lParam));
        if (!holder || !*holder)
            return 0;
        const auto job = *holder;
        int queued = 0;
        if (!job->phase.compare_exchange_strong(queued, 1))
            return 1;
        job->ok = false;
        if (g_talk.gameRva && g_talk.safeLogoutRva) {
            const std::uintptr_t base = moduleBase();
            void* self = reinterpret_cast<void*>(
                base + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(g_talk.gameRva)));
            const std::uintptr_t fn = base + static_cast<std::uintptr_t>(
                                                 static_cast<std::intptr_t>(g_talk.safeLogoutRva));
            job->ok = sehSafeLogout(self, fn);
        }
        if (job->done)
            SetEvent(job->done);
        return 1;
    }
    if (msg == kWmWalk) {
        auto* job = reinterpret_cast<WalkJob*>(lParam);
        if (!job || !job->self || !g_talk.autoWalkRva)
            return 0;
        const std::uintptr_t base = moduleBase();
        const std::uintptr_t fn =
            base + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(g_talk.autoWalkRva));
        job->sehOk = sehAutoWalk(job->self, fn, job->dest, &job->walked);
        if (job->done)
            SetEvent(job->done);
        return 1;
    }
    if (msg == kWmUse) {
        auto* job = reinterpret_cast<UseJob*>(lParam);
        if (!job)
            return 0;
        job->ok = false;
        if (job->ptr && g_talk.gameRva && g_talk.useRva) {
            const std::uintptr_t base = moduleBase();
            void* self = reinterpret_cast<void*>(
                base + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(g_talk.gameRva)));
            const std::uintptr_t fn =
                base + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(g_talk.useRva));
            job->ok = sehUse(self, fn, job);
        }
        if (job->done)
            SetEvent(job->done);
        return 1;
    }
    if (msg == kWmUseWith) {
        auto* job = reinterpret_cast<UseWithJob*>(lParam);
        if (!job)
            return 0;
        job->ok = false;
        if (job->from.ptr && g_talk.gameRva && g_talk.useWithRva) {
            const std::uintptr_t base = moduleBase();
            if (job->needTile) {
                job->to = {};
                job->rawTo = false;
                if (g_talk.mapRva && g_talk.getTileRva && g_talk.topUseRva) {
                    void* map = reinterpret_cast<void*>(
                        base + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(g_talk.mapRva)));
                    const std::uintptr_t getTile =
                        base +
                        static_cast<std::uintptr_t>(static_cast<std::intptr_t>(g_talk.getTileRva));
                    const std::uintptr_t topUse =
                        base +
                        static_cast<std::uintptr_t>(static_cast<std::intptr_t>(g_talk.topUseRva));
                    void* slot = sehGetTile(map, getTile, &job->tile);
                    LOG("getTile %d,%d,%d -> slot %p", job->tile.x, job->tile.y, job->tile.z, slot);
                    void* tile = tileFromGetTileSlot(slot);
                    if (tile) {
                        LOG("getTile Tile* %p rtti=%s", tile,
                            imageRttiName(reinterpret_cast<std::uintptr_t>(tile)).c_str());
                        sehTopUse(tile, topUse, &job->to);
                        if (!prepareMapThingHandle(job->to, job->rawTo))
                            thingFromTileVector(tile, &job->to, &job->rawTo);
                        LOG("getTopUseThing ptr=%p rep=%p raw=%d rtti=%s",
                            reinterpret_cast<void*>(job->to.ptr),
                            reinterpret_cast<void*>(job->to.rep),
                            job->rawTo ? 1 : 0,
                            imageRttiName(job->to.ptr).c_str());
                    } else {
                        LOG("getTile did not yield a Tile object");
                    }
                }
                if (!validThingHandle(job->to, job->rawTo)) {
                    job->to = job->fallback;
                    job->rawTo = job->rawFallback;
                }
                if (!validThingHandle(job->to, job->rawTo))
                    job->missingTile = true;
            }
            if (!job->missingTile && validThingHandle(job->from, job->rawFrom) &&
                validThingHandle(job->to, job->rawTo)) {
                void* self = reinterpret_cast<void*>(
                    base + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(g_talk.gameRva)));
                const std::uintptr_t fn =
                    base + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(g_talk.useWithRva));
                job->ok = sehUseWith(self, fn, &job->from, &job->to);
            } else if (!job->missingTile) {
                LOG("useWith refuse bad handles from=%p/%p raw=%d to=%p/%p raw=%d",
                    reinterpret_cast<void*>(job->from.ptr), reinterpret_cast<void*>(job->from.rep),
                    job->rawFrom ? 1 : 0, reinterpret_cast<void*>(job->to.ptr),
                    reinterpret_cast<void*>(job->to.rep), job->rawTo ? 1 : 0);
            }
        }
        if (job->done)
            SetEvent(job->done);
        return 1;
    }
    if (msg == kWmAttack) {
        auto* job = reinterpret_cast<AttackJob*>(lParam);
        if (!job)
            return 0;
        job->ok = false;
        if (g_talk.gameRva && g_talk.attackRva) {
            const std::uintptr_t base = moduleBase();
            void* self = reinterpret_cast<void*>(
                base + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(g_talk.gameRva)));
            const std::uintptr_t fn =
                base + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(g_talk.attackRva));
            bool chaseOk = true;
            if (!job->cancel && job->chase >= 0) {
                chaseOk = false;
                if (g_talk.chaseModeRva) {
                    const std::uintptr_t chaseFn =
                        base +
                        static_cast<std::uintptr_t>(static_cast<std::intptr_t>(g_talk.chaseModeRva));
                    chaseOk = sehChase(self, chaseFn, job->chase);
                    LOG("attack chase mode=%d ok=%d", job->chase, chaseOk ? 1 : 0);
                } else {
                    LOG("attack chase missing setChaseMode");
                }
            }
            if (job->cancel) {
                job->ok = sehCreatureAction("attack", self, fn, 0, 0, job->rawCreature);
                if (job->ok && !job->rawCreature) {
                    std::uintptr_t current = 0;
                    const auto attackingOffset = g_talk.attackingCreatureOffset >= 0
                        ? g_talk.attackingCreatureOffset
                        : static_cast<std::int32_t>(sizeof(void*) * 2);
                    readPtr(reinterpret_cast<std::uintptr_t>(self) + attackingOffset, current);
                    job->ok = current == 0;
                    LOG("cancelAttack postcondition current=%p ok=%d",
                        reinterpret_cast<void*>(current), job->ok ? 1 : 0);
                }
            } else if (validThingHandle(job->creature, job->rawCreature)) {
                // m_attackingCreature is profile/native-width derived. Same target is a client toggle
                // (cancel). Keep attack sticky; cancelAttack sends an empty CreaturePtr.
                std::uintptr_t current = 0;
                const auto attackingOffset = g_talk.attackingCreatureOffset >= 0
                    ? g_talk.attackingCreatureOffset
                    : static_cast<std::int32_t>(sizeof(void*) * 2);
                readPtr(reinterpret_cast<std::uintptr_t>(self) + attackingOffset, current);
                bool attackOk = false;
                if (current && current == job->creature.ptr) {
                    LOG("attack already on ptr=%p", reinterpret_cast<void*>(current));
                    attackOk = true;
                } else {
                    attackOk = sehCreatureAction("attack", self, fn, job->creature.ptr,
                                                 job->creature.rep, job->rawCreature);
                }
                if (attackOk && !job->rawCreature) {
                    std::uintptr_t after = 0;
                    readPtr(reinterpret_cast<std::uintptr_t>(self) + attackingOffset, after);
                    attackOk = after == job->creature.ptr;
                    LOG("attack postcondition want=%p current=%p ok=%d",
                        reinterpret_cast<void*>(job->creature.ptr), reinterpret_cast<void*>(after),
                        attackOk ? 1 : 0);
                }
                job->ok = chaseOk && attackOk;
            } else {
                LOG("attack refuse bad handle ptr=%p rep=%p raw=%d",
                    reinterpret_cast<void*>(job->creature.ptr),
                    reinterpret_cast<void*>(job->creature.rep), job->rawCreature ? 1 : 0);
            }
        }
        if (job->done)
            SetEvent(job->done);
        return 1;
    }
    if (msg == kWmFollow) {
        auto* job = reinterpret_cast<AttackJob*>(lParam);
        if (!job)
            return 0;
        job->ok = false;
        if (g_talk.gameRva && g_talk.followRva) {
            const std::uintptr_t base = moduleBase();
            void* self = reinterpret_cast<void*>(
                base + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(g_talk.gameRva)));
            const std::uintptr_t fn =
                base + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(g_talk.followRva));
            if (job->cancel) {
                job->ok = sehCreatureAction("follow", self, fn, 0, 0, job->rawCreature);
            } else if (validThingHandle(job->creature, job->rawCreature)) {
                // Game+0x10 is m_followingCreature T*. Same target again toggles off.
                std::uintptr_t current = 0;
                readPtr(reinterpret_cast<std::uintptr_t>(self) + 0x10, current);
                if (current && current == job->creature.ptr) {
                    LOG("follow already on ptr=%p", reinterpret_cast<void*>(current));
                    job->ok = true;
                } else {
                    job->ok = sehCreatureAction("follow", self, fn, job->creature.ptr,
                                                job->creature.rep, job->rawCreature);
                }
            } else {
                LOG("follow refuse bad handle ptr=%p rep=%p raw=%d",
                    reinterpret_cast<void*>(job->creature.ptr),
                    reinterpret_cast<void*>(job->creature.rep), job->rawCreature ? 1 : 0);
            }
        }
        if (job->done)
            SetEvent(job->done);
        return 1;
    }
    if (msg == kWmMove) {
        auto* job = reinterpret_cast<MoveJob*>(lParam);
        if (!job)
            return 0;
        job->ok = false;
        const bool validItem =
            job->rawItem ? (!job->item.rep && looksLikeHeapPtr(job->item.ptr))
                         : looksLikeSharedRep(job->item.rep);
        if (job->item.ptr && validItem && g_talk.gameRva && g_talk.moveRva) {
            const std::uintptr_t base = moduleBase();
            void* self = reinterpret_cast<void*>(
                base + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(g_talk.gameRva)));
            const std::uintptr_t fn =
                base + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(g_talk.moveRva));
            job->ok = sehMove(self, fn, &job->item, &job->dest, job->count);
        } else {
            LOG("move refuse bad item handle raw=%d ptr=%p rep=%p", job->rawItem ? 1 : 0,
                reinterpret_cast<void*>(job->item.ptr), reinterpret_cast<void*>(job->item.rep));
        }
        if (job->done)
            SetEvent(job->done);
        return 1;
    }
    if (msg == kWmLook) {
        auto* job = reinterpret_cast<LookJob*>(lParam);
        if (!job)
            return 0;
        job->ok = false;
        const std::uintptr_t base = moduleBase();
        if (job->needTile) {
            job->thing = {};
            job->rawThing = false;
            if (g_talk.mapRva && g_talk.getTileRva) {
                void* map = reinterpret_cast<void*>(
                    base + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(g_talk.mapRva)));
                const std::uintptr_t getTile =
                    base +
                    static_cast<std::uintptr_t>(static_cast<std::intptr_t>(g_talk.getTileRva));
                void* slot = sehGetTile(map, getTile, &job->tile);
                void* tile = tileFromGetTileSlot(slot);
                if (tile) {
                    thingFromTileLook(tile, &job->thing, &job->rawThing);
                    if (!validThingHandle(job->thing, job->rawThing) && g_talk.topUseRva) {
                        const std::uintptr_t topUse =
                            base +
                            static_cast<std::uintptr_t>(static_cast<std::intptr_t>(g_talk.topUseRva));
                        job->thing = {};
                        job->rawThing = false;
                        sehTopUse(tile, topUse, &job->thing);
                        prepareMapThingHandle(job->thing, job->rawThing);
                    }
                }
            }
            if (!validThingHandle(job->thing, job->rawThing)) {
                job->missingTile = true;
                LOG("look tile not resolved %d,%d,%d", job->tile.x, job->tile.y, job->tile.z);
                if (job->done)
                    SetEvent(job->done);
                return 1;
            }
        }
        if (validThingHandle(job->thing, job->rawThing) && g_talk.gameRva && g_talk.lookRva) {
            void* self = reinterpret_cast<void*>(
                base + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(g_talk.gameRva)));
            const std::uintptr_t fn =
                base + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(g_talk.lookRva));
            job->ok = sehLook(self, fn, &job->thing, 0);
        } else {
            LOG("look refuse ptr=%p rep=%p raw=%d lookRva=%d",
                reinterpret_cast<void*>(job->thing.ptr), reinterpret_cast<void*>(job->thing.rep),
                job->rawThing ? 1 : 0, g_talk.lookRva);
        }
        if (job->done)
            SetEvent(job->done);
        return 1;
    }
    if (msg == kWmClick) {
        std::unique_ptr<std::shared_ptr<ClickJob>> holder(
            reinterpret_cast<std::shared_ptr<ClickJob>*>(lParam));
        if (!holder || !*holder)
            return 0;
        const auto job = *holder;
        job->ok = false;
        job->missingTile = false;
        job->thing = {};
        job->rawThing = false;
        const std::uintptr_t base = moduleBase();
        if (g_talk.mapRva && g_talk.getTileRva) {
            void* map = reinterpret_cast<void*>(
                base + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(g_talk.mapRva)));
            const std::uintptr_t getTile =
                base + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(g_talk.getTileRva));
            void* slot = sehGetTile(map, getTile, &job->tile);
            void* tile = tileFromGetTileSlot(slot);
            LOG("click getTile %d,%d,%d -> slot %p tile %p", job->tile.x, job->tile.y, job->tile.z,
                slot, tile);
            if (tile) {
                if (g_talk.topUseRva) {
                    const std::uintptr_t topUse =
                        base +
                        static_cast<std::uintptr_t>(static_cast<std::intptr_t>(g_talk.topUseRva));
                    sehTopUse(tile, topUse, &job->thing);
                    prepareMapThingHandle(job->thing, job->rawThing);
                    LOG("click getTopUseThing ptr=%p rep=%p raw=%d rtti=%s",
                        reinterpret_cast<void*>(job->thing.ptr),
                        reinterpret_cast<void*>(job->thing.rep),
                        job->rawThing ? 1 : 0,
                        imageRttiName(job->thing.ptr).c_str());
                }
                if (!validThingHandle(job->thing, job->rawThing))
                    thingFromTileVector(tile, &job->thing, &job->rawThing);
            }
        }
        if (!validThingHandle(job->thing, job->rawThing)) {
            job->missingTile = true;
            LOG("click tile not resolved %d,%d,%d", job->tile.x, job->tile.y, job->tile.z);
            if (job->done)
                SetEvent(job->done);
            return 1;
        }
        if (job->expectedItemId) {
            std::uint16_t id = 0;
            const auto player = currentPlayer();
            if (job->rawThing || !player.valid || !player.hasPosition ||
                player.position.z != job->tile.z ||
                std::max(std::abs(player.position.x - job->tile.x),
                         std::abs(player.position.y - job->tile.y)) > 1 ||
                !memReadT(job->thing.ptr + job->itemIdOffset, id) || id != job->expectedItemId) {
                job->missingTile = true;
                if (job->done) SetEvent(job->done);
                return 1;
            }
        }
        if (g_talk.gameRva && g_talk.useRva) {
            void* self = reinterpret_cast<void*>(
                base + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(g_talk.gameRva)));
            const std::uintptr_t fn =
                base + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(g_talk.useRva));
            job->ok = sehUse(self, fn, &job->thing);
        } else {
            LOG("click refuse useRva=%d ptr=%p", g_talk.useRva,
                reinterpret_cast<void*>(job->thing.ptr));
        }
        if (job->done)
            SetEvent(job->done);
        return 1;
    }
    if (msg == kWmLight) {
        std::unique_ptr<std::shared_ptr<LightJob>> holder(
            reinterpret_cast<std::shared_ptr<LightJob>*>(lParam));
        if (!holder || !*holder)
            return 0;
        const auto job = *holder;
        int queued = 0;
        if (!job->phase.compare_exchange_strong(queued, 1))
            return 1;
        job->ok = false;
        if (job->uiMap && g_talk.ambientLightRva) {
            const auto fn = moduleBase() + static_cast<std::uintptr_t>(
                static_cast<std::intptr_t>(g_talk.ambientLightRva));
            job->ok = sehAmbientSet(job->uiMap, fn, job->intensity);
        }
        if (job->done)
            SetEvent(job->done);
        return 1;
    }
    if (g_prevProc) {
        if (IsWindowUnicode(hwnd))
            return CallWindowProcW(g_prevProc, hwnd, msg, wParam, lParam);
        return CallWindowProcA(g_prevProc, hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool hookGameWindow(HWND hwnd) {
    if (!hwnd)
        return false;
    if (g_hookedHwnd == hwnd && g_prevProc)
        return true;
    if (g_hookedHwnd && g_prevProc) {
        if (IsWindowUnicode(g_hookedHwnd))
            SetWindowLongPtrW(g_hookedHwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_prevProc));
        else
            SetWindowLongPtrA(g_hookedHwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_prevProc));
        g_prevProc = nullptr;
        g_hookedHwnd = nullptr;
    }
    LONG_PTR prev = 0;
    if (IsWindowUnicode(hwnd))
        prev = SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(talkWndProc));
    else
        prev = SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(talkWndProc));
    if (!prev) {
        LOG("talk hook failed err=%lu", GetLastError());
        return false;
    }
    g_prevProc = reinterpret_cast<WNDPROC>(prev);
    g_hookedHwnd = hwnd;
    const DWORD windowTid = GetWindowThreadProcessId(hwnd, nullptr);
    LOG("talk hooked hwnd=%p prev=%p windowTid=%lu hookTid=%lu", hwnd,
        reinterpret_cast<void*>(g_prevProc), windowTid, GetCurrentThreadId());
    return true;
}

bool sehAmbientGet(void* uiMap, std::uintptr_t fn, float& value) {
    DWORD code = 0;
    __try {
#if defined(_M_IX86)
        using Fn = float(__thiscall*)(void*);
#else
        using Fn = float (*)(void*);
#endif
        value = reinterpret_cast<Fn>(fn)(uiMap);
        return value >= 0.0f && value <= 1.0f;
    } __except ((code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)) {
        LOG("ambient light get exception code=%lu", code);
        return false;
    }
}

bool sehAmbientSet(void* uiMap, std::uintptr_t fn, float value) {
    DWORD code = 0;
    __try {
#if defined(_M_IX86)
        using Fn = void(__thiscall*)(void*, float);
#else
        using Fn = void (*)(void*, float);
#endif
        reinterpret_cast<Fn>(fn)(uiMap, value);
        return true;
    } __except ((code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)) {
        LOG("ambient light set exception code=%lu", code);
        return false;
    }
}

void keepActionRvas(ActionLayout& lay, const ActionLayout& old) {
    if (!lay.gameRva)
        lay.gameRva = old.gameRva;
    if (lay.attackingCreatureOffset < 0)
        lay.attackingCreatureOffset = old.attackingCreatureOffset;
    if (!lay.talkChannelRva)
        lay.talkChannelRva = old.talkChannelRva;
    if (!lay.safeLogoutRva)
        lay.safeLogoutRva = old.safeLogoutRva;
    if (!lay.autoWalkRva)
        lay.autoWalkRva = old.autoWalkRva;
    if (!lay.useRva)
        lay.useRva = old.useRva;
    if (!lay.useWithRva)
        lay.useWithRva = old.useWithRva;
    if (!lay.attackRva)
        lay.attackRva = old.attackRva;
    if (!lay.followRva)
        lay.followRva = old.followRva;
    if (!lay.chaseModeRva)
        lay.chaseModeRva = old.chaseModeRva;
    if (!lay.moveRva)
        lay.moveRva = old.moveRva;
    if (!lay.mapRva)
        lay.mapRva = old.mapRva;
    if (!lay.getTileRva)
        lay.getTileRva = old.getTileRva;
    if (!lay.topUseRva)
        lay.topUseRva = old.topUseRva;
    if (!lay.lookRva)
        lay.lookRva = old.lookRva;
    if (!lay.ambientLightRva)
        lay.ambientLightRva = old.ambientLightRva;
    if (!lay.ambientLightGetRva)
        lay.ambientLightGetRva = old.ambientLightGetRva;
}

void commitActionLayout(ActionLayout lay) {
    keepActionRvas(lay, g_talk);
    Profile live;
    copyLiveProfile(live);
    keepActionRvas(lay, live.actions);
    if (lay.attackingCreatureOffset < 0 && lay.attackRva != 0 && live.creatures.present)
        lay.attackingCreatureOffset = static_cast<std::int32_t>(sizeof(void*) * 2);
    lay.present = lay.gameRva != 0 || lay.talkChannelRva != 0 || lay.safeLogoutRva != 0 ||
                  lay.autoWalkRva != 0 ||
                  lay.useRva != 0 || lay.useWithRva != 0 || lay.attackRva != 0 ||
                  lay.followRva != 0 || lay.moveRva != 0 || lay.ambientLightRva != 0;
    g_talk = lay;
    setActionLayout(lay);
}

bool ensureTalkResolved() {
    if (g_talk.gameRva && g_talk.talkChannelRva)
        return true;

    Profile prof;
    copyLiveProfile(prof);
    if (prof.actions.gameRva && prof.actions.talkChannelRva) {
        const std::uintptr_t game =
            moduleBase() +
            static_cast<std::uintptr_t>(static_cast<std::intptr_t>(prof.actions.gameRva));
        const std::uintptr_t lp = fieldAddr(prof.chain);
        const bool lpOk = !lp || (lp >= game && lp - game <= 64);
        if (lpOk) {
            if (!g_talk.autoWalkRva)
                g_talk.autoWalkRva = prof.actions.autoWalkRva;
            if (!g_talk.useRva)
                g_talk.useRva = prof.actions.useRva;
            g_talk.gameRva = prof.actions.gameRva;
            g_talk.talkChannelRva = prof.actions.talkChannelRva;
            g_talk.present = true;
            LOG("talk from profile gameRva=%d talkChannelRva=%d autoWalkRva=%d useRva=%d",
                g_talk.gameRva, g_talk.talkChannelRva, g_talk.autoWalkRva, g_talk.useRva);
            return true;
        }
        LOG("talk profile discarded (g_game not next to LocalPlayer field)");
    }

    if (prof.chain.empty() && prof.containers.chain.empty() && prof.creatures.chain.empty()) {
        LOG("talk scan: profile not ready yet");
        return false;
    }
    if (g_talkTried)
        return false;
    g_talkTried = true;

    std::uintptr_t game = 0;
    std::uintptr_t fn = 0;
    if (!pickTalkBind(prof, game, fn))
        return false;

    const std::uintptr_t base = moduleBase();
    ActionLayout lay = prof.actions;
    if (g_talk.autoWalkRva)
        lay.autoWalkRva = g_talk.autoWalkRva;
    if (g_talk.useRva)
        lay.useRva = g_talk.useRva;
    lay.gameRva = static_cast<std::int32_t>(game - base);
    lay.talkChannelRva = static_cast<std::int32_t>(fn - base);
    commitActionLayout(lay);
    if (saveCurrentProfile())
        LOG("talk saved to profile");
    return true;
}

bool autoWalkExecutable(std::int32_t rva) {
    if (!rva)
        return false;
    const std::uintptr_t fn =
        moduleBase() + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(rva));
    return isExec(fn);
}

bool ensureWalkResolved() {
    if (autoWalkExecutable(g_talk.autoWalkRva))
        return true;

    Profile prof;
    copyLiveProfile(prof);
    if (autoWalkExecutable(prof.actions.autoWalkRva)) {
        if (!g_talk.gameRva)
            g_talk.gameRva = prof.actions.gameRva;
        if (!g_talk.talkChannelRva)
            g_talk.talkChannelRva = prof.actions.talkChannelRva;
        if (!g_talk.useRva)
            g_talk.useRva = prof.actions.useRva;
        g_talk.autoWalkRva = prof.actions.autoWalkRva;
        g_talk.present = true;
        LOG("walk from profile autoWalkRva=%d", g_talk.autoWalkRva);
        return true;
    }

    if (g_walkTried)
        return false;
    g_walkTried = true;

    std::uintptr_t game = gameAddress(prof);
    if (!game) {
        std::uintptr_t talkFn = 0;
        if (pickTalkBind(prof, game, talkFn) && game && talkFn) {
            const std::uintptr_t base = moduleBase();
            ActionLayout lay = prof.actions;
            if (g_talk.autoWalkRva)
                lay.autoWalkRva = g_talk.autoWalkRva;
            if (g_talk.useRva)
                lay.useRva = g_talk.useRva;
            lay.gameRva = static_cast<std::int32_t>(game - base);
            lay.talkChannelRva = static_cast<std::int32_t>(talkFn - base);
            commitActionLayout(lay);
        }
    }
    if (!game) {
        LOG("walk scan: g_game unknown, cannot skip Game::autoWalk");
        return false;
    }

    std::uintptr_t localFn = 0;
    std::uintptr_t gameFn = 0;
    if (!pickAutoWalk(game, localFn, gameFn))
        return false;

    const std::uintptr_t base = moduleBase();
    ActionLayout lay =
        (g_talk.gameRva || g_talk.talkChannelRva || g_talk.autoWalkRva) ? g_talk : prof.actions;
    if (!lay.gameRva)
        lay.gameRva = static_cast<std::int32_t>(game - base);
    lay.autoWalkRva = static_cast<std::int32_t>(localFn - base);
    commitActionLayout(lay);
    if (saveCurrentProfile())
        LOG("walk saved to profile autoWalkRva=%d (not Game::autoWalk rva=%d)", lay.autoWalkRva,
            gameFn ? static_cast<int>(gameFn - base) : 0);
    return true;
}

bool callTalkOnUiThread(const std::string& text, int mode) {
    if (!ensureTalkResolved())
        return false;
    const HWND hwnd = findGameWindow();
    if (!hwnd || !hookGameWindow(hwnd))
        return false;

    TalkJob job;
    job.text = text;
    job.mode = mode;
    DWORD_PTR result = 0;
    SetLastError(ERROR_SUCCESS);
    const DWORD sendStarted = GetTickCount();
    const LRESULT sent = SendMessageTimeoutW(hwnd, kWmTalk, 0, reinterpret_cast<LPARAM>(&job),
                                             SMTO_ABORTIFHUNG, 2000, &result);
    if (sent)
        return job.ok;

    const DWORD sendError = GetLastError();
    const DWORD sendElapsed = GetTickCount() - sendStarted;
    LOG("talk SendMessageTimeout err=%lu elapsed=%lums", sendError, sendElapsed);

    // Preserve the established synchronous path. Some client windows reject the
    // cross-thread Send immediately; only that case gets the same asynchronous
    // UI-thread dispatch used by the dispatcher-backed actions below. Do not post
    // after a real timeout because the original stack-backed message may still run.
    if (sendError == ERROR_TIMEOUT || sendElapsed >= 1900)
        return false;

    job.done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!job.done) {
        LOG("talk fallback CreateEvent failed err=%lu", GetLastError());
        return false;
    }

    if (!PostMessageW(hwnd, kWmTalk, 0, reinterpret_cast<LPARAM>(&job))) {
        LOG("talk fallback PostMessage err=%lu", GetLastError());
        CloseHandle(job.done);
        job.done = nullptr;
        return false;
    }

    const DWORD wait = WaitForSingleObject(job.done, 5000);
    CloseHandle(job.done);
    job.done = nullptr;
    if (wait != WAIT_OBJECT_0) {
        LOG("talk fallback wait %lu err=%lu", wait, GetLastError());
        return false;
    }
    LOG("talk PostMessage fallback completed ok=%d", job.ok ? 1 : 0);
    return job.ok;
}

bool ensureLogoutResolved() {
    auto executable = [](std::int32_t rva) {
        if (!rva)
            return false;
        return isExec(moduleBase() +
                      static_cast<std::uintptr_t>(static_cast<std::intptr_t>(rva)));
    };
    if (g_talk.gameRva && executable(g_talk.safeLogoutRva))
        return true;

    Profile prof;
    copyLiveProfile(prof);
    if (prof.actions.gameRva && executable(prof.actions.safeLogoutRva)) {
        keepActionRvas(g_talk, prof.actions);
        g_talk.present = true;
        LOG("logout from profile safeLogoutRva=%d", g_talk.safeLogoutRva);
        return true;
    }
    if (g_logoutTried)
        return false;
    g_logoutTried = true;

    std::uintptr_t game = gameAddress(prof);
    if (!game && ensureTalkResolved()) {
        copyLiveProfile(prof);
        game = gameAddress(prof);
    }
    std::uintptr_t fn = 0;
    if (!game || !pickSafeLogoutFn(game, fn))
        return false;

    const std::uintptr_t base = moduleBase();
    ActionLayout lay = prof.actions;
    if (!lay.gameRva)
        lay.gameRva = static_cast<std::int32_t>(game - base);
    lay.safeLogoutRva = static_cast<std::int32_t>(fn - base);
    commitActionLayout(lay);
    if (saveCurrentProfile())
        LOG("logout saved to profile safeLogoutRva=%d", lay.safeLogoutRva);
    return true;
}

bool callLogoutOnUiThread() {
    if (!ensureLogoutResolved())
        return false;
    const HWND hwnd = findGameWindow();
    if (!hwnd) {
        LOG("logout game window not found");
#if defined(_M_IX86)
        return callDispatcherLogout();
#else
        return false;
#endif
    }
    if (!hookGameWindow(hwnd)) {
        LOG("logout game window hook failed hwnd=%p visible=%d iconic=%d", hwnd,
            IsWindowVisible(hwnd) ? 1 : 0, IsIconic(hwnd) ? 1 : 0);
#if defined(_M_IX86)
        return callDispatcherLogout();
#else
        return false;
#endif
    }
    LOG("logout dispatch hwnd=%p visible=%d iconic=%d", hwnd,
        IsWindowVisible(hwnd) ? 1 : 0, IsIconic(hwnd) ? 1 : 0);

    const auto job = std::make_shared<LogoutJob>();
    if (!job->done) {
        LOG("logout CreateEvent failed err=%lu", GetLastError());
        return false;
    }
    auto* holder = new (std::nothrow) std::shared_ptr<LogoutJob>(job);
    if (!holder)
        return false;
    if (!PostMessageW(hwnd, kWmLogout, 0, reinterpret_cast<LPARAM>(holder))) {
        LOG("logout PostMessage err=%lu", GetLastError());
        delete holder;
        return false;
    }

    const DWORD wait = WaitForSingleObject(job->done, 5000);
    if (wait == WAIT_OBJECT_0)
        return job->ok;

    int queued = 0;
    const bool canceled = job->phase.compare_exchange_strong(queued, 2);
    LOG("logout wait %lu err=%lu canceled=%d", wait, GetLastError(), canceled ? 1 : 0);
    return false;
}

bool callWalkOnUiThread(void* self, const OtcPos& dest, bool& walked) {
    walked = false;
    if (!ensureWalkResolved())
        return false;
    const HWND hwnd = findGameWindow();
    if (!hwnd || !hookGameWindow(hwnd))
        return false;

    WalkJob job;
    job.self = self;
    job.dest = dest;
    job.done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!job.done) {
        LOG("walk CreateEvent failed err=%lu", GetLastError());
        return false;
    }

    // Post, don't Send: autoWalk uses the dispatcher/pathfinder and AVs if nested
    // inside another window procedure (talkChannel does not).
    if (!PostMessageW(hwnd, kWmWalk, 0, reinterpret_cast<LPARAM>(&job))) {
        LOG("walk PostMessage err=%lu", GetLastError());
        CloseHandle(job.done);
        return false;
    }
    const DWORD wait = WaitForSingleObject(job.done, 5000);
    CloseHandle(job.done);
    if (wait != WAIT_OBJECT_0) {
        LOG("walk wait %lu err=%lu", wait, GetLastError());
        return false;
    }
    walked = job.walked;
    return job.sehOk;
}

bool rvaExecutable(std::int32_t rva) {
    return autoWalkExecutable(rva);
}

bool rvaWritable(std::int32_t rva) {
    if (!rva)
        return false;
    const std::uintptr_t p =
        moduleBase() + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(rva));
    return isWritable(p);
}

bool ensureUseResolved() {
    if (rvaExecutable(g_talk.useRva) && g_talk.gameRva)
        return true;

    Profile prof;
    copyLiveProfile(prof);
    if (rvaExecutable(prof.actions.useRva) && prof.actions.gameRva) {
        if (!g_talk.gameRva)
            g_talk.gameRva = prof.actions.gameRva;
        if (!g_talk.talkChannelRva)
            g_talk.talkChannelRva = prof.actions.talkChannelRva;
        if (!g_talk.autoWalkRva)
            g_talk.autoWalkRva = prof.actions.autoWalkRva;
        if (!g_talk.useWithRva)
            g_talk.useWithRva = prof.actions.useWithRva;
        if (!g_talk.mapRva)
            g_talk.mapRva = prof.actions.mapRva;
        if (!g_talk.getTileRva)
            g_talk.getTileRva = prof.actions.getTileRva;
        if (!g_talk.topUseRva)
            g_talk.topUseRva = prof.actions.topUseRva;
        g_talk.useRva = prof.actions.useRva;
        g_talk.present = true;
        LOG("use from profile useRva=%d", g_talk.useRva);
        return true;
    }

    if (g_useTried)
        return false;
    g_useTried = true;

    std::uintptr_t game = gameAddress(prof);
    if (!game) {
        std::uintptr_t talkFn = 0;
        if (pickTalkBind(prof, game, talkFn) && game && talkFn) {
            const std::uintptr_t base = moduleBase();
            ActionLayout lay = prof.actions;
            if (g_talk.autoWalkRva)
                lay.autoWalkRva = g_talk.autoWalkRva;
            if (g_talk.useRva)
                lay.useRva = g_talk.useRva;
            lay.gameRva = static_cast<std::int32_t>(game - base);
            lay.talkChannelRva = static_cast<std::int32_t>(talkFn - base);
            commitActionLayout(lay);
        }
    }
    if (!game)
        game = gameAddress(prof);
    if (!game) {
        LOG("use scan: g_game unknown");
        return false;
    }

    std::uintptr_t useFn = 0;
    std::uintptr_t withFn = 0;
    if (!pickUseFn(game, useFn, withFn))
        return false;

    const std::uintptr_t base = moduleBase();
    ActionLayout lay = (g_talk.gameRva || g_talk.talkChannelRva || g_talk.autoWalkRva ||
                        g_talk.useRva || g_talk.useWithRva)
                           ? g_talk
                           : prof.actions;
    if (!lay.gameRva)
        lay.gameRva = static_cast<std::int32_t>(game - base);
    lay.useRva = static_cast<std::int32_t>(useFn - base);
    if (withFn)
        lay.useWithRva = static_cast<std::int32_t>(withFn - base);
    commitActionLayout(lay);
    if (saveCurrentProfile())
        LOG("use saved to profile useRva=%d useWithRva=%d", lay.useRva, lay.useWithRva);
    return true;
}

bool callUseOnUiThread(std::uintptr_t ptr, std::uintptr_t rep) {
    if (!ptr || !ensureUseResolved())
        return false;
    const HWND hwnd = findGameWindow();
    if (!hwnd || !hookGameWindow(hwnd))
        return false;

    UseJob job;
    job.ptr = ptr;
    job.rep = rep;

    // Keep the established shared_ptr path unchanged. A calibrated one-pointer
    // handle is a separate ABI and uses the asynchronous window dispatch required
    // by dispatcher-backed OTClient actions.
    if (!rep) {
        job.done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!job.done) {
            LOG("use raw CreateEvent failed err=%lu", GetLastError());
            return false;
        }
        if (!PostMessageW(hwnd, kWmUse, 0, reinterpret_cast<LPARAM>(&job))) {
            LOG("use raw PostMessage err=%lu", GetLastError());
            CloseHandle(job.done);
            job.done = nullptr;
            return false;
        }
        const DWORD wait = WaitForSingleObject(job.done, 5000);
        CloseHandle(job.done);
        job.done = nullptr;
        if (wait != WAIT_OBJECT_0) {
            LOG("use raw wait %lu err=%lu", wait, GetLastError());
            return false;
        }
        LOG("use raw PostMessage completed ok=%d", job.ok ? 1 : 0);
        return job.ok;
    }

    DWORD_PTR result = 0;
    const LRESULT sent = SendMessageTimeoutW(hwnd, kWmUse, 0, reinterpret_cast<LPARAM>(&job),
                                             SMTO_ABORTIFHUNG, 2000, &result);
    if (!sent) {
        LOG("use SendMessageTimeout err=%lu", GetLastError());
        return false;
    }
    return job.ok;
}

bool ensureMapResolved() {
    if (rvaWritable(g_talk.mapRva) && rvaExecutable(g_talk.getTileRva) &&
        rvaExecutable(g_talk.topUseRva))
        return true;

    Profile prof;
    copyLiveProfile(prof);
    if (rvaWritable(prof.actions.mapRva) && rvaExecutable(prof.actions.getTileRva) &&
        rvaExecutable(prof.actions.topUseRva)) {
        g_talk.mapRva = prof.actions.mapRva;
        g_talk.getTileRva = prof.actions.getTileRva;
        g_talk.topUseRva = prof.actions.topUseRva;
        g_talk.present = true;
        LOG("map from profile mapRva=%d getTileRva=%d topUseRva=%d", g_talk.mapRva,
            g_talk.getTileRva, g_talk.topUseRva);
        return true;
    }
    if (g_mapTried)
        return false;
    g_mapTried = true;

    std::uintptr_t map = 0;
    std::uintptr_t getTile = 0;
    std::uintptr_t topUse = 0;
    if (!pickMapFns(map, getTile, topUse))
        return false;

    const std::uintptr_t base = moduleBase();
    ActionLayout lay = g_talk.present ? g_talk : prof.actions;
    lay.mapRva = static_cast<std::int32_t>(map - base);
    lay.getTileRva = static_cast<std::int32_t>(getTile - base);
    lay.topUseRva = static_cast<std::int32_t>(topUse - base);
    commitActionLayout(lay);
    if (saveCurrentProfile())
        LOG("map saved to profile mapRva=%d getTileRva=%d topUseRva=%d", lay.mapRva, lay.getTileRva,
            lay.topUseRva);
    return true;
}

bool ensureUseWithResolved() {
    if (rvaExecutable(g_talk.useWithRva) && g_talk.gameRva)
        return true;

    Profile prof;
    copyLiveProfile(prof);
    if (rvaExecutable(prof.actions.useWithRva) && prof.actions.gameRva) {
        if (!g_talk.gameRva)
            g_talk.gameRva = prof.actions.gameRva;
        if (!g_talk.useRva)
            g_talk.useRva = prof.actions.useRva;
        g_talk.useWithRva = prof.actions.useWithRva;
        g_talk.present = true;
        LOG("useWith from profile useWithRva=%d", g_talk.useWithRva);
        return true;
    }

    if (g_useWithTried)
        return false;
    g_useWithTried = true;

    std::uintptr_t game = gameAddress(prof);
    if (!game)
        ensureUseResolved();
    game = gameAddress(prof);
    if (!game && g_talk.gameRva)
        game = moduleBase() +
               static_cast<std::uintptr_t>(static_cast<std::intptr_t>(g_talk.gameRva));
    if (!game) {
        LOG("useWith scan: g_game unknown");
        return false;
    }

    std::uintptr_t useFn = 0;
    std::uintptr_t withFn = 0;
    if (!pickUseFn(game, useFn, withFn) || !withFn) {
        LOG("useWith scan: Game::useWith not resolved");
        return false;
    }

    const std::uintptr_t base = moduleBase();
    ActionLayout lay = g_talk.present ? g_talk : prof.actions;
    if (!lay.gameRva)
        lay.gameRva = static_cast<std::int32_t>(game - base);
    if (useFn && !lay.useRva)
        lay.useRva = static_cast<std::int32_t>(useFn - base);
    lay.useWithRva = static_cast<std::int32_t>(withFn - base);
    commitActionLayout(lay);
    if (saveCurrentProfile())
        LOG("useWith saved to profile useWithRva=%d", lay.useWithRva);
    return true;
}

bool callUseWithOnUiThread(UseWithJob& job) {
    if (!job.from.ptr || !ensureUseWithResolved())
        return false;
    if (job.needTile && !ensureMapResolved()) {
        job.missingTile = true;
        LOG("useWith tile: map/getTile not resolved");
        return false;
    }
    const HWND hwnd = findGameWindow();
    if (!hwnd || !hookGameWindow(hwnd))
        return false;

    job.done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!job.done) {
        LOG("useWith CreateEvent failed err=%lu", GetLastError());
        return false;
    }

    // Post, don't Send: useWith talks to the protocol/dispatcher and hangs
    // inside a nested window procedure (same as autoWalk).
    if (!PostMessageW(hwnd, kWmUseWith, 0, reinterpret_cast<LPARAM>(&job))) {
        LOG("useWith PostMessage err=%lu", GetLastError());
        CloseHandle(job.done);
        job.done = nullptr;
        return false;
    }
    const DWORD wait = WaitForSingleObject(job.done, 5000);
    CloseHandle(job.done);
    job.done = nullptr;
    if (wait != WAIT_OBJECT_0) {
        LOG("useWith wait %lu err=%lu", wait, GetLastError());
        return false;
    }
    return job.ok;
}

bool ensureAttackResolved() {
    if (rvaExecutable(g_talk.attackRva) && g_talk.gameRva)
        return true;

    Profile prof;
    copyLiveProfile(prof);
    if (rvaExecutable(prof.actions.attackRva) && prof.actions.gameRva) {
        if (!g_talk.gameRva)
            g_talk.gameRva = prof.actions.gameRva;
        if (!g_talk.useWithRva)
            g_talk.useWithRva = prof.actions.useWithRva;
        g_talk.attackRva = prof.actions.attackRva;
        if (rvaExecutable(prof.actions.chaseModeRva))
            g_talk.chaseModeRva = prof.actions.chaseModeRva;
        g_talk.present = true;
        LOG("attack from profile attackRva=%d", g_talk.attackRva);
        return true;
    }

    if (g_attackTried)
        return false;
    g_attackTried = true;

    std::uintptr_t game = gameAddress(prof);
    if (!game && g_talk.gameRva)
        game = moduleBase() +
               static_cast<std::uintptr_t>(static_cast<std::intptr_t>(g_talk.gameRva));
    if (!game) {
        LOG("attack scan: g_game unknown");
        return false;
    }

    std::uintptr_t fn = 0;
    if (!pickAttackFn(game, fn) || !fn) {
        LOG("attack scan: Game::attack not resolved");
        return false;
    }

    const std::uintptr_t base = moduleBase();
    ActionLayout lay = g_talk.present ? g_talk : prof.actions;
    if (!lay.gameRva)
        lay.gameRva = static_cast<std::int32_t>(game - base);
    lay.attackRva = static_cast<std::int32_t>(fn - base);
    commitActionLayout(lay);
    if (saveCurrentProfile())
        LOG("attack saved to profile attackRva=%d", lay.attackRva);
    return true;
}

bool callAttackOnUiThread(AttackJob& job) {
    if (!ensureAttackResolved())
        return false;
    if (!job.cancel && !job.creature.ptr)
        return false;
    const HWND hwnd = findGameWindow();
    if (!hwnd || !hookGameWindow(hwnd))
        return false;

    job.done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!job.done) {
        LOG("attack CreateEvent failed err=%lu", GetLastError());
        return false;
    }

    // Post, don't Send: attack talks to the protocol/dispatcher (same as useWith).
    if (!PostMessageW(hwnd, kWmAttack, 0, reinterpret_cast<LPARAM>(&job))) {
        LOG("attack PostMessage err=%lu", GetLastError());
        CloseHandle(job.done);
        job.done = nullptr;
        return false;
    }
    const DWORD wait = WaitForSingleObject(job.done, 5000);
    CloseHandle(job.done);
    job.done = nullptr;
    if (wait != WAIT_OBJECT_0) {
        LOG("attack wait %lu err=%lu", wait, GetLastError());
        return false;
    }
    return job.ok;
}

bool ensureFollowResolved() {
    if (rvaExecutable(g_talk.followRva) && g_talk.gameRva)
        return true;

    Profile prof;
    copyLiveProfile(prof);
    if (rvaExecutable(prof.actions.followRva) && prof.actions.gameRva) {
        if (!g_talk.gameRva)
            g_talk.gameRva = prof.actions.gameRva;
        if (!g_talk.attackRva)
            g_talk.attackRva = prof.actions.attackRva;
        g_talk.followRva = prof.actions.followRva;
        g_talk.present = true;
        LOG("follow from profile followRva=%d", g_talk.followRva);
        return true;
    }

    if (g_followTried)
        return false;
    g_followTried = true;

    std::uintptr_t game = gameAddress(prof);
    if (!game && g_talk.gameRva)
        game = moduleBase() +
               static_cast<std::uintptr_t>(static_cast<std::intptr_t>(g_talk.gameRva));
    if (!game) {
        LOG("follow scan: g_game unknown");
        return false;
    }

    std::uintptr_t fn = 0;
    if (!pickFollowFn(game, fn) || !fn) {
        LOG("follow scan: Game::follow not resolved");
        return false;
    }

    const std::uintptr_t base = moduleBase();
    ActionLayout lay = g_talk.present ? g_talk : prof.actions;
    if (!lay.gameRva)
        lay.gameRva = static_cast<std::int32_t>(game - base);
    lay.followRva = static_cast<std::int32_t>(fn - base);
    commitActionLayout(lay);
    if (saveCurrentProfile())
        LOG("follow saved to profile followRva=%d", lay.followRva);
    return true;
}

bool callFollowOnUiThread(AttackJob& job) {
    if (!ensureFollowResolved())
        return false;
    if (!job.cancel && !job.creature.ptr)
        return false;
    const HWND hwnd = findGameWindow();
    if (!hwnd || !hookGameWindow(hwnd))
        return false;

    job.done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!job.done) {
        LOG("follow CreateEvent failed err=%lu", GetLastError());
        return false;
    }

    if (!PostMessageW(hwnd, kWmFollow, 0, reinterpret_cast<LPARAM>(&job))) {
        LOG("follow PostMessage err=%lu", GetLastError());
        CloseHandle(job.done);
        job.done = nullptr;
        return false;
    }
    const DWORD wait = WaitForSingleObject(job.done, 5000);
    CloseHandle(job.done);
    job.done = nullptr;
    if (wait != WAIT_OBJECT_0) {
        LOG("follow wait %lu err=%lu", wait, GetLastError());
        return false;
    }
    return job.ok;
}

bool ensureChaseResolved() {
    if (rvaExecutable(g_talk.chaseModeRva) && g_talk.gameRva)
        return true;

    Profile prof;
    copyLiveProfile(prof);
    if (rvaExecutable(prof.actions.chaseModeRva) && prof.actions.gameRva) {
        if (!g_talk.gameRva)
            g_talk.gameRva = prof.actions.gameRva;
        if (!g_talk.attackRva)
            g_talk.attackRva = prof.actions.attackRva;
        g_talk.chaseModeRva = prof.actions.chaseModeRva;
        g_talk.present = true;
        LOG("chase from profile chaseModeRva=%d", g_talk.chaseModeRva);
        return true;
    }

    if (g_chaseTried)
        return false;
    g_chaseTried = true;

    std::uintptr_t game = gameAddress(prof);
    if (!game && g_talk.gameRva)
        game = moduleBase() +
               static_cast<std::uintptr_t>(static_cast<std::intptr_t>(g_talk.gameRva));
    if (!game) {
        LOG("chase scan: g_game unknown");
        return false;
    }

    std::uintptr_t fn = 0;
    if (!pickChaseFn(game, fn) || !fn) {
        LOG("chase scan: Game::setChaseMode not resolved");
        return false;
    }

    const std::uintptr_t base = moduleBase();
    ActionLayout lay = g_talk.present ? g_talk : prof.actions;
    if (!lay.gameRva)
        lay.gameRva = static_cast<std::int32_t>(game - base);
    lay.chaseModeRva = static_cast<std::int32_t>(fn - base);
    commitActionLayout(lay);
    if (saveCurrentProfile())
        LOG("chase saved to profile chaseModeRva=%d", lay.chaseModeRva);
    return true;
}

bool targetActive(const UseTarget* to) {
    if (!to)
        return false;
    return to->self || to->hasCreature || to->hasPos || to->slot >= 0;
}

bool posFromThing(std::uintptr_t obj, OtcPos& out) {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint16_t z = 0;
    if (!obj || !memReadT(obj + 0x10, x) || !memReadT(obj + 0x14, y) || !memReadT(obj + 0x18, z))
        return false;
    if (x == 0xFFFF) {
        out.x = x;
        out.y = y;
        out.z = z;
        return true;
    }
    if (!plausibleMapPos(x, y, z))
        return false;
    out.x = x;
    out.y = y;
    out.z = z;
    return true;
}

int itemCountOf(std::uintptr_t obj, bool fromContainer) {
    Profile prof;
    copyLiveProfile(prof);
    const int off = fromContainer ? prof.containers.itemCountOffset : prof.inventory.itemCountOffset;
    std::int32_t n = 0;
    if (off > 0 && memReadT(obj + static_cast<std::uintptr_t>(off), n) && n > 0)
        return n;
    return 1;
}

bool prepareItemHandle(std::uintptr_t& ptr, std::uintptr_t& rep, bool fromContainer) {
    if (!ptr)
        return false;

    Profile prof;
    copyLiveProfile(prof);
    int width = 0;
    if (fromContainer && prof.containers.present && prof.containers.itemPtrSize > 0)
        width = prof.containers.itemPtrSize;
    else if (prof.inventory.present && prof.inventory.ptrSize > 0)
        width = prof.inventory.ptrSize;

    // Keep the established shared_ptr ABI strict. A calibrated one-pointer
    // handle has no control block slot to recover; its object and id were already
    // validated by the reader that resolved the inventory/container entry.
    if (width >= static_cast<int>(sizeof(void*) * 2))
        return completeSharedPtr(ptr, rep) && looksLikeSharedRep(rep);
    if (width == static_cast<int>(sizeof(void*))) {
        rep = 0;
        const bool ok = looksLikeHeapPtr(ptr);
        LOG("item handle width=%d raw=%d ptr=%p", width, ok ? 1 : 0,
            reinterpret_cast<void*>(ptr));
        return ok;
    }
    return false;
}

bool destPosForMove(const UseTarget& to, OtcPos& out) {
    if (to.hasPos) {
        out.x = to.x;
        out.y = to.y;
        out.z = to.z;
        return true;
    }
    if (to.slot < 0)
        return false;

    std::uintptr_t ptr = 0;
    std::uintptr_t rep = 0;
    if (to.container >= 0) {
        if (resolveContainerItem(to.container, to.slot, to.id, ptr, rep) &&
            prepareItemHandle(ptr, rep, true) && posFromThing(ptr, out))
            return true;
        out.x = 0xFFFF;
        out.y = to.container | 0x40;
        out.z = to.slot;
        return true;
    }
    if (to.slot < 1 || to.slot > 10)
        return false;
    if (resolveInventoryItem(to.slot, to.id, ptr, rep) &&
        prepareItemHandle(ptr, rep, false) && posFromThing(ptr, out))
        return true;
    out.x = 0xFFFF;
    out.y = to.slot;
    out.z = 0;
    return true;
}

bool ensureMoveResolved() {
    if (rvaExecutable(g_talk.moveRva) && g_talk.gameRva)
        return true;

    Profile prof;
    copyLiveProfile(prof);
    if (rvaExecutable(prof.actions.moveRva) && prof.actions.gameRva) {
        if (!g_talk.gameRva)
            g_talk.gameRva = prof.actions.gameRva;
        g_talk.moveRva = prof.actions.moveRva;
        g_talk.present = true;
        LOG("move from profile moveRva=%d", g_talk.moveRva);
        return true;
    }

    if (g_moveTried)
        return false;
    g_moveTried = true;

    std::uintptr_t game = gameAddress(prof);
    if (!game && g_talk.gameRva)
        game = moduleBase() +
               static_cast<std::uintptr_t>(static_cast<std::intptr_t>(g_talk.gameRva));
    if (!game) {
        LOG("move scan: g_game unknown");
        return false;
    }

    std::uintptr_t fn = 0;
    if (!pickMoveFn(game, fn) || !fn) {
        LOG("move scan: Game::move not resolved");
        return false;
    }

    const std::uintptr_t base = moduleBase();
    ActionLayout lay = g_talk.present ? g_talk : prof.actions;
    if (!lay.gameRva)
        lay.gameRva = static_cast<std::int32_t>(game - base);
    lay.moveRva = static_cast<std::int32_t>(fn - base);
    commitActionLayout(lay);
    if (saveCurrentProfile())
        LOG("move saved to profile moveRva=%d", lay.moveRva);
    return true;
}

bool callMoveOnUiThread(MoveJob& job) {
    if (!job.item.ptr || !ensureMoveResolved())
        return false;
    const HWND hwnd = findGameWindow();
    if (!hwnd || !hookGameWindow(hwnd))
        return false;

    job.done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!job.done) {
        LOG("move CreateEvent failed err=%lu", GetLastError());
        return false;
    }

    if (!PostMessageW(hwnd, kWmMove, 0, reinterpret_cast<LPARAM>(&job))) {
        LOG("move PostMessage err=%lu", GetLastError());
        CloseHandle(job.done);
        job.done = nullptr;
        return false;
    }
    const DWORD wait = WaitForSingleObject(job.done, 5000);
    CloseHandle(job.done);
    job.done = nullptr;
    if (wait != WAIT_OBJECT_0) {
        LOG("move wait %lu err=%lu", wait, GetLastError());
        return false;
    }
    return job.ok;
}

bool ensureLookResolved() {
    if (rvaExecutable(g_talk.lookRva) && g_talk.gameRva) {
        if (!g_lookFn)
            g_lookFn = moduleBase() +
                       static_cast<std::uintptr_t>(static_cast<std::intptr_t>(g_talk.lookRva));
        Profile live;
        copyLiveProfile(live);
        refreshLookLayout(live);
        return true;
    }

    Profile prof;
    copyLiveProfile(prof);
    if (rvaExecutable(prof.actions.lookRva) && prof.actions.gameRva) {
        if (!g_talk.gameRva)
            g_talk.gameRva = prof.actions.gameRva;
        g_talk.lookRva = prof.actions.lookRva;
        g_talk.present = true;
        g_lookFn = moduleBase() +
                   static_cast<std::uintptr_t>(static_cast<std::intptr_t>(g_talk.lookRva));
        refreshLookLayout(prof);
        LOG("look from profile lookRva=%d", g_talk.lookRva);
        return true;
    }

    if (g_lookTried)
        return false;

    std::uintptr_t game = gameAddress(prof);
    if (!game && g_talk.gameRva)
        game = moduleBase() +
               static_cast<std::uintptr_t>(static_cast<std::intptr_t>(g_talk.gameRva));
    if (!game) {
        LOG("look scan: g_game unknown");
        return false;
    }

    g_lookTried = true;
    std::uintptr_t fn = 0;
    if (!pickLookFn(game, fn) || !fn) {
        LOG("look scan: Game::look not resolved");
        return false;
    }

    const std::uintptr_t base = moduleBase();
    ActionLayout lay = g_talk.present ? g_talk : prof.actions;
    if (!lay.gameRva)
        lay.gameRva = static_cast<std::int32_t>(game - base);
    lay.lookRva = static_cast<std::int32_t>(fn - base);
    commitActionLayout(lay);
    g_lookFn = fn;
    refreshLookLayout(prof);
    if (saveCurrentProfile())
        LOG("look saved to profile lookRva=%d", lay.lookRva);
    return true;
}

bool callLookOnUiThread(LookJob& job) {
    if (!ensureLookResolved())
        return false;
    installLookHook();
    if (job.needTile && !ensureMapResolved())
        return false;
    const HWND hwnd = findGameWindow();
    if (!hwnd || !hookGameWindow(hwnd))
        return false;

    job.done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!job.done) {
        LOG("look CreateEvent failed err=%lu", GetLastError());
        return false;
    }

    if (!PostMessageW(hwnd, kWmLook, 0, reinterpret_cast<LPARAM>(&job))) {
        LOG("look PostMessage err=%lu", GetLastError());
        CloseHandle(job.done);
        job.done = nullptr;
        return false;
    }
    const DWORD wait = WaitForSingleObject(job.done, 5000);
    CloseHandle(job.done);
    job.done = nullptr;
    if (wait != WAIT_OBJECT_0) {
        LOG("look wait %lu err=%lu", wait, GetLastError());
        return false;
    }
    return job.ok;
}

bool callClickOnUiThread(ClickJob& job) {
    if (!ensureUseResolved())
        return false;
    if (!ensureMapResolved())
        return false;
    const HWND hwnd = findGameWindow();
    if (!hwnd || !hookGameWindow(hwnd))
        return false;

    auto pending = std::make_shared<ClickJob>();
    pending->tile = job.tile;
    pending->expectedItemId = job.expectedItemId;
    pending->itemIdOffset = job.itemIdOffset;
    pending->done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!pending->done) {
        LOG("click CreateEvent failed err=%lu", GetLastError());
        return false;
    }

    auto* payload = new std::shared_ptr<ClickJob>(pending);
    if (!PostMessageW(hwnd, kWmClick, 0, reinterpret_cast<LPARAM>(payload))) {
        LOG("click PostMessage err=%lu", GetLastError());
        delete payload;
        return false;
    }
    const DWORD wait = WaitForSingleObject(pending->done, 5000);
    if (wait != WAIT_OBJECT_0) {
        LOG("click wait %lu err=%lu", wait, GetLastError());
        return false;
    }
    job.thing = pending->thing;
    job.missingTile = pending->missingTile;
    job.ok = pending->ok;
    return job.ok;
}

} // namespace

bool resolveCalibrationGameAnchors(std::vector<std::int32_t>& gameRvas) {
    gameRvas.clear();
    Profile empty;
    std::uintptr_t selected = 0;
    std::uintptr_t talkFn = 0;
    std::vector<std::uintptr_t> candidates;
    pickTalkBind(empty, selected, talkFn, &candidates);
    if (selected &&
        std::find(candidates.begin(), candidates.end(), selected) == candidates.end())
        candidates.insert(candidates.begin(), selected);
    const std::uintptr_t base = moduleBase();
    for (const auto game : candidates) {
        if (!base || !game || !isWritable(game) || game < base ||
            game - base > static_cast<std::uintptr_t>(INT32_MAX))
            continue;
        const auto rva = static_cast<std::int32_t>(game - base);
        if (std::find(gameRvas.begin(), gameRvas.end(), rva) == gameRvas.end())
            gameRvas.push_back(rva);
    }
    LOG("calibration anchors: candidates=%zu selected=%p talk=%p", gameRvas.size(),
        reinterpret_cast<void*>(selected), reinterpret_cast<void*>(talkFn));
    return !gameRvas.empty();
}

ActionResult actionSay(const std::string& text, const std::string& mode) {
    std::lock_guard<std::mutex> lock(g_mtx);
    LOG("say request len=%zu", text.size());
    if (text.empty())
        return fail("empty text");

    std::string clipped = text;
    if (clipped.size() > kMaxSay)
        clipped.resize(kMaxSay);

    if (!ensureTalkResolved())
        return fail("talk not resolved");

    const int sayMode = parseSayMode(mode);
    if (!callTalkOnUiThread(clipped, sayMode))
        return fail("talk call failed");

    LOG("say via client len=%zu mode=%d", clipped.size(), sayMode);
    return ActionResult{true, {}, "client"};
}

ActionResult actionStep(const std::string& direction) {
    std::lock_guard<std::mutex> lock(g_mtx);
    const DirKeys keys = parseDirection(direction);
    if (!keys.ok)
        return fail("bad direction");

    const HWND hwnd = findGameWindow();
    if (!hwnd)
        return fail("no game window");
    focusWindow(hwnd);

    LOG("step dir=%s hwnd=%p", direction.c_str(), hwnd);

    sendVk(keys.a, true);
    if (keys.b)
        sendVk(keys.b, true);
    Sleep(kStepHoldMs);
    if (keys.b)
        sendVk(keys.b, false);
    sendVk(keys.a, false);
    return ActionResult{true, {}, "keys"};
}

ActionResult actionStop() {
    std::lock_guard<std::mutex> lock(g_mtx);
    const HWND hwnd = findGameWindow();
    if (hwnd)
        focusWindow(hwnd);

    const WORD vks[] = {
        VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT,
        VK_NUMPAD1, VK_NUMPAD2, VK_NUMPAD3, VK_NUMPAD4,
        VK_NUMPAD6, VK_NUMPAD7, VK_NUMPAD8, VK_NUMPAD9,
    };
    for (WORD vk : vks)
        sendVk(vk, false);
    LOG("stop keys released");
    return ActionResult{true, {}, "keys"};
}

ActionResult actionLogout() {
    std::lock_guard<std::mutex> lock(g_mtx);
    LOG("logout request");
    if (!ensureLogoutResolved())
        return fail("logout not resolved");
    if (!callLogoutOnUiThread())
        return fail("logout call failed");
    LOG("logout via client");
    return ActionResult{true, {}, "client"};
}

ActionResult actionWalkTo(int x, int y, int z) {
    std::lock_guard<std::mutex> lock(g_mtx);
    LOG("walkTo request %d,%d,%d", x, y, z);
    if (x < 0 || y < 0 || z < 0 || x > 65535 || y > 65535 || z > 255)
        return fail("bad coordinates");

    if (!ensureWalkResolved())
        return fail("autoWalk not resolved");

    const std::uintptr_t player = resolveLocalPlayerThis();
    if (!player)
        return fail("no local player");

    std::uintptr_t vtbl = 0;
    std::uintptr_t ev = 0;
    std::int32_t px = 0;
    std::int32_t py = 0;
    std::int32_t pz = 0;
    readPtr(player, vtbl);
    readPtr(player + 0x31C, ev);
    memReadT(player + 32, px);
    memReadT(player + 36, py);
    memReadT(player + 40, pz);
    LOG("walkTo this=%p vtbl=%p pos=%d,%d,%d ev=%p dest=%d,%d,%d",
        reinterpret_cast<void*>(player), reinterpret_cast<void*>(vtbl), px, py, pz,
        reinterpret_cast<void*>(ev), x, y, z);

    OtcPos dest;
    dest.x = x;
    dest.y = y;
    dest.z = z;

    bool walked = false;
    if (!callWalkOnUiThread(reinterpret_cast<void*>(player), dest, walked))
        return fail("autoWalk call failed");
    if (!walked) {
        LOG("walkTo no path %d,%d,%d via client", x, y, z);
        return ActionResult{false, "no path", "client"};
    }

    LOG("walkTo via client %d,%d,%d", x, y, z);
    return ActionResult{true, {}, "client"};
}

ActionResult actionUseItem(int containerIndex, int slot, int id, const UseTarget* to) {
    std::lock_guard<std::mutex> lock(g_mtx);
    const bool with = targetActive(to);
    LOG("useItem request container=%d slot=%d id=%d with=%d", containerIndex, slot, id, with ? 1 : 0);

    std::uintptr_t ptr = 0;
    std::uintptr_t rep = 0;
    if (containerIndex >= 0) {
        if (slot < 0)
            return fail("bad slot");
        if (!resolveContainerItem(containerIndex, slot, id, ptr, rep))
            return fail("item not found");
    } else {
        if (slot < 1 || slot > 10)
            return fail("bad slot");
        if (!resolveInventoryItem(slot, id, ptr, rep))
            return fail("item not found");
    }
    if (!prepareItemHandle(ptr, rep, containerIndex >= 0))
        return fail("item not found");

    if (!with) {
        if (!ensureUseResolved())
            return fail("use not resolved");
        if (!callUseOnUiThread(ptr, rep))
            return fail("use call failed");
        LOG("useItem via client container=%d slot=%d id=%d ptr=%p", containerIndex, slot, id,
            reinterpret_cast<void*>(ptr));
        return ActionResult{true, {}, "client"};
    }

    UseWithJob job;
    job.from.ptr = ptr;
    job.from.rep = rep;
    job.rawFrom = rep == 0;
    if (to->hasCreature) {
        if (!resolveCreature(to->creatureId, job.to.ptr, job.to.rep))
            return fail("creature not found");
        if (!prepareCreatureThingHandle(job.to, job.rawTo))
            return fail("creature not found");
    } else if (to->self) {
        const std::uintptr_t obj = currentPlayerObject();
        if (obj)
            resolveCreatureByObject(obj, job.to.ptr, job.to.rep);
        job.rawTo = job.to.ptr && job.to.rep == 0;
        if (!validThingHandle(job.to, job.rawTo)) {
            resolveLocalPlayerShared(job.to.ptr, job.to.rep);
            job.rawTo = job.to.ptr && job.to.rep == 0;
        }
        if (!validThingHandle(job.to, job.rawTo))
            return fail("no local player");
    } else if (to->slot >= 0) {
        if (to->container >= 0) {
            if (!resolveContainerItem(to->container, to->slot, to->id, job.to.ptr, job.to.rep))
                return fail("target item not found");
        } else if (!resolveInventoryItem(to->slot, to->id, job.to.ptr, job.to.rep)) {
            return fail("target item not found");
        }
        if (!prepareItemHandle(job.to.ptr, job.to.rep, to->container >= 0))
            return fail("target item not found");
        job.rawTo = job.to.rep == 0;
    } else if (to->hasPos) {
        job.needTile = true;
        job.tile.x = to->x;
        job.tile.y = to->y;
        job.tile.z = to->z;
        if (resolveCreatureAt(to->x, to->y, to->z, job.fallback.ptr, job.fallback.rep))
            job.rawFallback = job.fallback.rep == 0;
        LOG("useWith to tile %d,%d,%d", to->x, to->y, to->z);
    } else {
        return fail("bad target");
    }

    if (!ensureUseWithResolved())
        return fail("useWith not resolved");
    if (!callUseWithOnUiThread(job))
        return fail(job.missingTile ? "tile not resolved" : "useWith call failed");

    LOG("useItem with via client from=%p to=%p tile=%d", reinterpret_cast<void*>(job.from.ptr),
        reinterpret_cast<void*>(job.to.ptr), job.needTile ? 1 : 0);
    return ActionResult{true, {}, "client"};
}

ActionResult actionAttack(std::uint32_t creatureId, int chase) {
    std::lock_guard<std::mutex> lock(g_mtx);
    LOG("attack request creatureId=%u chase=%d", creatureId, chase);
    if (!creatureId)
        return fail("missing creatureId");

    AttackJob job;
    job.chase = chase;
    if (!resolveCreature(creatureId, job.creature.ptr, job.creature.rep))
        return fail("creature not found");
    if (!prepareCreatureThingHandle(job.creature, job.rawCreature))
        return fail("creature not found");

    if (chase >= 0 && !ensureChaseResolved())
        return fail("chase not resolved");
    if (!ensureAttackResolved())
        return fail("attack not resolved");
#if defined(_M_IX86)
    if (job.rawCreature) {
        if (!callRawAttack(creatureId, chase))
            return fail("attack dispatcher failed");
        return ActionResult{true, {}, "client"};
    }
#endif
    if (!callAttackOnUiThread(job))
        return fail("attack call failed");

    LOG("attack via client id=%u ptr=%p chase=%d", creatureId,
        reinterpret_cast<void*>(job.creature.ptr), chase);
    return ActionResult{true, {}, "client"};
}

ActionResult actionCancelAttack() {
    std::lock_guard<std::mutex> lock(g_mtx);
    LOG("cancelAttack request");
    AttackJob job;
    job.cancel = true;
    job.rawCreature = rawCreatureHandles();
    if (!ensureAttackResolved())
        return fail("attack not resolved");
    if (!callAttackOnUiThread(job))
        return fail("cancelAttack call failed");
    LOG("cancelAttack via client");
    return ActionResult{true, {}, "client"};
}

ActionResult actionFollow(std::uint32_t creatureId) {
    std::lock_guard<std::mutex> lock(g_mtx);
    LOG("follow request creatureId=%u", creatureId);
    if (!creatureId)
        return fail("missing creatureId");

    AttackJob job;
    if (!resolveCreature(creatureId, job.creature.ptr, job.creature.rep))
        return fail("creature not found");
    if (!prepareCreatureThingHandle(job.creature, job.rawCreature))
        return fail("creature not found");

    if (!ensureFollowResolved())
        return fail("follow not resolved");
    if (!callFollowOnUiThread(job))
        return fail("follow call failed");

    LOG("follow via client id=%u ptr=%p", creatureId, reinterpret_cast<void*>(job.creature.ptr));
    return ActionResult{true, {}, "client"};
}

ActionResult actionCancelFollow() {
    std::lock_guard<std::mutex> lock(g_mtx);
    LOG("cancelFollow request");
    AttackJob job;
    job.cancel = true;
    job.rawCreature = rawCreatureHandles();
    if (!ensureFollowResolved())
        return fail("follow not resolved");
    if (!callFollowOnUiThread(job))
        return fail("cancelFollow call failed");
    LOG("cancelFollow via client");
    return ActionResult{true, {}, "client"};
}

ActionResult actionMoveItem(int containerIndex, int slot, int id, const UseTarget& to, int count) {
    std::lock_guard<std::mutex> lock(g_mtx);
    LOG("moveItem request container=%d slot=%d id=%d count=%d toSlot=%d toCont=%d pos=%d",
        containerIndex, slot, id, count, to.slot, to.container, to.hasPos ? 1 : 0);

    std::uintptr_t ptr = 0;
    std::uintptr_t rep = 0;
    if (containerIndex >= 0) {
        if (slot < 0)
            return fail("bad slot");
        if (!resolveContainerItem(containerIndex, slot, id, ptr, rep))
            return fail("item not found");
    } else {
        if (slot < 1 || slot > 10)
            return fail("bad slot");
        if (!resolveInventoryItem(slot, id, ptr, rep))
            return fail("item not found");
    }
    if (!prepareItemHandle(ptr, rep, containerIndex >= 0))
        return fail("item not found");

    OtcPos dest{};
    if (!destPosForMove(to, dest))
        return fail("bad target");

    int n = count;
    if (n <= 0)
        n = itemCountOf(ptr, containerIndex >= 0);
    if (n < 1)
        n = 1;

    MoveJob job;
    job.item.ptr = ptr;
    job.item.rep = rep;
    job.rawItem = !rep;
    job.dest = dest;
    job.count = n;

    if (!ensureMoveResolved())
        return fail("move not resolved");
    if (!callMoveOnUiThread(job))
        return fail("move call failed");

    LOG("moveItem via client from=%p dest=%d,%d,%d count=%d", reinterpret_cast<void*>(ptr), dest.x,
        dest.y, dest.z, n);
    return ActionResult{true, {}, "client"};
}

ActionResult actionLook(const LookTarget& target) {
    std::lock_guard<std::mutex> lock(g_mtx);
    LookJob job;

    if (target.hasPos) {
        LOG("look request tile %d,%d,%d", target.x, target.y, target.z);
        if (!plausibleMapPos(target.x, target.y, target.z))
            return fail("bad coordinates");
        job.needTile = true;
        job.tile.x = target.x;
        job.tile.y = target.y;
        job.tile.z = target.z;
    } else if (target.hasCreature) {
        LOG("look request creatureId=%u", target.creatureId);
        if (!target.creatureId)
            return fail("missing creatureId");
        if (!resolveCreature(target.creatureId, job.thing.ptr, job.thing.rep))
            return fail("creature not found");
        if (!prepareCreatureThingHandle(job.thing, job.rawThing))
            return fail("creature not found");
    } else if (target.slot >= 0) {
        LOG("look request container=%d slot=%d id=%d", target.container, target.slot, target.id);
        if (target.container >= 0) {
            if (!resolveContainerItem(target.container, target.slot, target.id, job.thing.ptr,
                                      job.thing.rep))
                return fail("item not found");
        } else {
            if (target.slot < 1 || target.slot > 10)
                return fail("bad slot");
            if (!resolveInventoryItem(target.slot, target.id, job.thing.ptr, job.thing.rep))
                return fail("item not found");
        }
        if (!prepareItemHandle(job.thing.ptr, job.thing.rep, target.container >= 0))
            return fail("item not found");
        job.rawThing = job.thing.rep == 0;
    } else {
        return fail("missing look target");
    }

    if (!ensureLookResolved())
        return fail("look not resolved");
    if (!callLookOnUiThread(job))
        return fail(job.missingTile ? "tile not resolved" : "look call failed");

    if (!g_lookHooked && job.thing.ptr)
        captureLook(&job.thing);

    LOG("look via client ptr=%p", reinterpret_cast<void*>(job.thing.ptr));
    return ActionResult{true, {}, "client"};
}

LookInfo currentLook() {
    std::lock_guard<std::mutex> lock(g_lookMtx);
    return g_lastLook;
}

std::string lastLookJson() {
    LookInfo info;
    {
        std::lock_guard<std::mutex> lock(g_lookMtx);
        info = g_lastLook;
    }
    if (!info.valid)
        return "{}";
    try {
        return lookObjectJson(info);
    } catch (...) {
        return "{}";
    }
}

void pumpLookCapture() {
    std::lock_guard<std::mutex> lock(g_mtx);
    if (!ensureLookResolved())
        return;
    installLookHook();
}

ActionResult actionUseCorpse(int x, int y, int z, int expectedItemId) {
    std::lock_guard<std::mutex> lock(g_mtx);
    Profile profile;
    if (!plausibleMapPos(x, y, z) || expectedItemId <= 0 || expectedItemId > 65535)
        return fail("bad corpse item");
    if (!copyLiveProfile(profile) || !profile.map.present || profile.map.itemIdOffset <= 0 ||
        profile.map.itemPtrSize != 8)
        return fail("corpse use ABI not supported");
    ClickJob job;
    job.tile.x = x; job.tile.y = y; job.tile.z = z;
    job.expectedItemId = expectedItemId;
    job.itemIdOffset = profile.map.itemIdOffset;
    if (!callClickOnUiThread(job))
        return fail(job.missingTile ? "corpse changed or covered" : "corpse use failed");
    return ActionResult{true, {}, "client"};
}

ActionResult actionClick(int x, int y, int z) {
    std::lock_guard<std::mutex> lock(g_mtx);
    LOG("click request %d,%d,%d", x, y, z);
    if (!plausibleMapPos(x, y, z))
        return fail("bad coordinates");

    ClickJob job;
    job.tile.x = x;
    job.tile.y = y;
    job.tile.z = z;

    if (!ensureUseResolved())
        return fail("use not resolved");
    if (!callClickOnUiThread(job))
        return fail(job.missingTile ? "tile not resolved" : "use call failed");

    LOG("click via client %d,%d,%d ptr=%p", x, y, z, reinterpret_cast<void*>(job.thing.ptr));
    return ActionResult{true, {}, "client"};
}

bool pickNamedMethod(const char* name, std::size_t bytes, std::uintptr_t& fnOut) {
    fnOut = 0;
    std::vector<ImageRange> readable;
    std::vector<ImageRange> exec;
    collectImageRanges(readable, exec);
    std::vector<std::uintptr_t> strings;
    findBytes(readable, name, bytes, strings, 8);
    for (const auto str : strings) {
        const auto needle = static_cast<std::uint32_t>(str);
        std::vector<std::uintptr_t> refs;
        findBytes(exec, &needle, sizeof(needle), refs, 16);
#if defined(_M_X64)
        findRipRelativeRefs(exec, str, refs, 16);
#endif
        for (const auto ref : refs) {
            std::vector<ImmPtr> imms;
            collectImmPtrs(ref, imms);
#if defined(_M_X64)
            collectRipRelativePtrs(ref, imms);
#endif
            for (const auto& imm : imms) {
                if (imm.at > ref && isExec(imm.val)) {
                    fnOut = imm.val;
                    return true;
                }
            }
        }
    }
    return false;
}

bool ensureLightResolved() {
    if (g_uiMapViewOffset > 0 && rvaExecutable(g_talk.ambientLightRva) &&
        rvaExecutable(g_talk.ambientLightGetRva))
        return true;
    Profile prof;
    copyLiveProfile(prof);
    if (g_lightTried)
        return false;
    g_lightTried = true;
    std::uintptr_t setter = 0;
    std::uintptr_t getter = 0;
    if (rvaExecutable(prof.actions.ambientLightRva) &&
        rvaExecutable(prof.actions.ambientLightGetRva)) {
        setter = moduleBase() + prof.actions.ambientLightRva;
        getter = moduleBase() + prof.actions.ambientLightGetRva;
    } else if (!pickNamedMethod("setMinimumAmbientLight", 23, setter) ||
               !pickNamedMethod("getMinimumAmbientLight", 23, getter)) {
        LOG("ambient light methods not resolved");
        return false;
    }
    // Recognize only the tiny MSVC accessors and require both member offsets to
    // agree before invoking either function. The established x86 form remains
    // first; x64 is an additive native-ABI form.
    unsigned char getCode[32]{};
    unsigned char setCode[32]{};
    if (!memRead(reinterpret_cast<void*>(getter), getCode, sizeof(getCode)) ||
        !memRead(reinterpret_cast<void*>(setter), setCode, sizeof(setCode))) {
        LOG("ambient light accessor unreadable");
        return false;
    }
    bool abiOk = false;
#if defined(_M_IX86)
    abiOk = getCode[0] == 0x8B && getCode[1] == 0x81 && getCode[6] == 0xD9 &&
        getCode[7] == 0x80 && getCode[12] == 0xC3 &&
        setCode[0] == 0x55 && setCode[1] == 0x8B && setCode[2] == 0xEC &&
        setCode[3] == 0x8B && setCode[4] == 0x81 &&
        std::memcmp(getCode + 2, setCode + 5, 4) == 0 &&
        setCode[9] == 0xF3 && setCode[10] == 0x0F && setCode[11] == 0x10 &&
        setCode[12] == 0x45 && setCode[13] == 0x08 &&
        setCode[14] == 0xF3 && setCode[15] == 0x0F && setCode[16] == 0x11 &&
        setCode[17] == 0x80 && std::memcmp(getCode + 8, setCode + 18, 4) == 0 &&
        setCode[22] == 0x5D && setCode[23] == 0xC2 && setCode[24] == 4 &&
        setCode[25] == 0;
    if (abiOk)
        std::memcpy(&g_uiMapViewOffset, getCode + 2, 4);
#else
    // mov rax,[rcx+view]; movss xmm0,[rax+light]; ret
    // mov rax,[rcx+view]; movss [rax+light],xmm1; ret
    abiOk = getCode[0] == 0x48 && getCode[1] == 0x8B && getCode[2] == 0x81 &&
        getCode[7] == 0xF3 && getCode[8] == 0x0F && getCode[9] == 0x10 &&
        getCode[10] == 0x80 && getCode[15] == 0xC3 &&
        setCode[0] == 0x48 && setCode[1] == 0x8B && setCode[2] == 0x81 &&
        std::memcmp(getCode + 3, setCode + 3, 4) == 0 &&
        setCode[7] == 0xF3 && setCode[8] == 0x0F && setCode[9] == 0x11 &&
        setCode[10] == 0x88 && std::memcmp(getCode + 11, setCode + 11, 4) == 0 &&
        setCode[15] == 0xC3;
    if (abiOk)
        std::memcpy(&g_uiMapViewOffset, getCode + 3, 4);
#endif
    if (!abiOk || g_uiMapViewOffset <= 0) {
        LOG("ambient light accessor ABI unsupported");
        return false;
    }
    ActionLayout lay = prof.actions;
    const auto base = moduleBase();
    lay.ambientLightRva = static_cast<std::int32_t>(setter - base);
    lay.ambientLightGetRva = static_cast<std::int32_t>(getter - base);
    commitActionLayout(lay);
    if (saveCurrentProfile())
        LOG("ambient light saved setter=%d getter=%d", lay.ambientLightRva,
            lay.ambientLightGetRva);
    return true;
}

std::uintptr_t findUiMapObject() {
    if (g_uiMap && imageRttiName(g_uiMap).find("UIMap") != std::string::npos)
        return g_uiMap;
    if (g_uiMap) {
        g_uiMap = 0;
        g_savedAmbientLightValid = false;
    }
    std::vector<ImageRange> readable;
    std::vector<ImageRange> exec;
    collectImageRanges(readable, exec);
    std::vector<std::uintptr_t> names;
    findBytes(readable, ".?AVUIMap@@", 12, names, 4);
    std::vector<std::uintptr_t> vtables;
    for (const auto name : names) {
        const auto tdNameOffset = sizeof(void*) * 2;
        if (name < tdNameOffset)
            continue;
        const auto tdAddr = name - tdNameOffset;
        std::vector<std::uintptr_t> tdRefs;
#if defined(_WIN64)
        const auto base = moduleBase();
        if (tdAddr < base || tdAddr - base > 0xffffffffu)
            continue;
        const auto td = static_cast<std::uint32_t>(tdAddr - base);
#else
        const auto td = static_cast<std::uint32_t>(tdAddr);
#endif
        findBytes(readable, &td, sizeof(td), tdRefs, 16);
        for (const auto tdRef : tdRefs) {
            if (tdRef < 12)
                continue;
            const auto colAddr = tdRef - 12;
            std::vector<std::uintptr_t> colRefs;
#if defined(_WIN64)
            if (colAddr < base || colAddr - base > 0xffffffffu)
                continue;
            // COL stores its TypeDescriptor as an image RVA, but vftable[-1]
            // is the relocated native pointer to the COL.
            findBytes(readable, &colAddr, sizeof(colAddr), colRefs, 16);
#else
            const auto col = static_cast<std::uint32_t>(colAddr);
            findBytes(readable, &col, sizeof(col), colRefs, 16);
#endif
            for (const auto colRef : colRefs)
                vtables.push_back(colRef + sizeof(void*));
        }
    }
    if (vtables.empty())
        return 0;
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    auto p = reinterpret_cast<std::uintptr_t>(si.lpMinimumApplicationAddress);
    auto end = reinterpret_cast<std::uintptr_t>(si.lpMaximumApplicationAddress);
#if defined(_WIN64)
    // UIMap and the live game objects are allocated in the same process heap
    // neighborhood on x64. Starting there avoids spending the four-second
    // bounded scan on unrelated low-address regions after the UI is recreated.
    if (const auto anchor = currentPlayerObject()) {
        constexpr std::uintptr_t span = 0x40000000ull;
        const auto minAddress = reinterpret_cast<std::uintptr_t>(si.lpMinimumApplicationAddress);
        const auto maxAddress = reinterpret_cast<std::uintptr_t>(si.lpMaximumApplicationAddress);
        p = anchor > span ? anchor - span : minAddress;
        if (p < minAddress)
            p = minAddress;
        end = anchor <= maxAddress - span ? anchor + span : maxAddress;
    }
#endif
    std::vector<std::uintptr_t> block(16384);
    const DWORD start = GetTickCount();
    while (p < end && GetTickCount() - start < 4000) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi)))
            break;
        const auto next = p + mbi.RegionSize;
        const DWORD prot = mbi.Protect & 0xff;
        if (mbi.State == MEM_COMMIT && !memPageGuarded(mbi.Protect) &&
            (prot == PAGE_READWRITE || prot == PAGE_WRITECOPY)) {
            const auto first = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
            const auto last = first + mbi.RegionSize;
            for (auto chunk = first; chunk < last && GetTickCount() - start < 4000;
                 chunk += block.size() * sizeof(void*)) {
                const auto bytes = std::min<std::size_t>(last - chunk, block.size() * sizeof(void*));
                if (!memCopy(reinterpret_cast<void*>(chunk), block.data(), bytes))
                    continue;
                for (std::size_t i = 0; i < bytes / sizeof(void*); ++i) {
                    if (std::find(vtables.begin(), vtables.end(), block[i]) == vtables.end())
                        continue;
                    const auto cur = chunk + i * sizeof(void*);
                    std::uintptr_t view = 0;
                    if (!readPtr(cur + g_uiMapViewOffset, view) ||
                        imageRttiName(view).find("MapView") == std::string::npos)
                        continue;
                    g_uiMap = cur;
                    LOG("ambient light UIMap=%p", reinterpret_cast<void*>(cur));
                    return cur;
                }
            }
        }
        if (next <= p)
            break;
        p = next;
    }
    return 0;
}

bool callLightOnUiThread(const std::shared_ptr<LightJob>& job) {
    const HWND hwnd = findGameWindow();
    if (!hwnd || !hookGameWindow(hwnd))
        return false;
    if (!job->done)
        return false;
    auto holder = std::make_unique<std::shared_ptr<LightJob>>(job);
    if (!PostMessageW(hwnd, kWmLight, 0, reinterpret_cast<LPARAM>(holder.get())))
        return false;
    holder.release(); // window message owns this reference, including after timeout
    const DWORD wait = WaitForSingleObject(job->done, 5000);
    if (wait != WAIT_OBJECT_0) {
        int queued = 0;
        job->phase.compare_exchange_strong(queued, 2);
        return false;
    }
    return job->ok;
}

ActionResult actionSetLight(bool enabled, float intensity, float& appliedIntensity) {
    std::lock_guard<std::mutex> lock(g_mtx);
    if (!(intensity >= 0.0f && intensity <= 1.0f))
        return fail("bad intensity");
    if (!ensureLightResolved())
        return fail("light not resolved");
    const auto uiMap = findUiMapObject();
    if (!uiMap)
        return fail("light not active");
    const auto getter = moduleBase() + static_cast<std::uintptr_t>(
        static_cast<std::intptr_t>(g_talk.ambientLightGetRva));
    float current = 0.0f;
    if (!sehAmbientGet(reinterpret_cast<void*>(uiMap), getter, current))
        return fail("light call failed");
    if (enabled && !g_savedAmbientLightValid) {
        g_savedAmbientLight = current;
        g_savedAmbientLightValid = true;
    }
    if (!enabled && !g_savedAmbientLightValid)
        return fail("light not active");
    const auto job = std::make_shared<LightJob>();
    job->uiMap = reinterpret_cast<void*>(uiMap);
    job->intensity = enabled ? intensity : g_savedAmbientLight;
    if (!callLightOnUiThread(job))
        return fail("light call failed");
    if (!enabled)
        g_savedAmbientLightValid = false;
    appliedIntensity = job->intensity;
    LOG("ambient light enabled=%d intensity=%.3f previous=%.3f", enabled ? 1 : 0,
        appliedIntensity, current);
    return ActionResult{true, {}, "client"};
}

} // namespace otc

