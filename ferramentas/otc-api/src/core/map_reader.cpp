#include "otc_api/map_reader.h"

#include "otc_api/logger.h"
#include "otc_api/memory.h"
#include "otc_api/player_reader.h"
#include "otc_api/profile.h"

#include <nlohmann/json.hpp>

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace otc {
namespace {

using nlohmann::json;

constexpr DWORD kScanBudgetMs = 20000;
constexpr int kMaxTiles = 512;
constexpr int kMaxThings = 64;
constexpr int kMaxBlocks = 512;
constexpr int kMaxMapScan = 4096;
constexpr int kDefaultRadius = 8;

void findPointersTo(std::uintptr_t target, std::vector<std::uintptr_t>& hits, DWORD t0);

struct OtcPos {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
};

#if defined(_M_IX86)
using GetTileFn = void*(__thiscall*)(void* self, const OtcPos* pos);
#else
using GetTileFn = void* (*)(void* self, const OtcPos* pos);
#endif

std::mutex g_mtx;
MapLayout g_layout;
MapCalibHints g_hints;
std::string g_msg = "map not calibrated";

int ptrSizeOf(const MapLayout& l) {
    return l.ptrSize > 0 ? l.ptrSize : static_cast<int>(sizeof(void*));
}

int tileStride(const MapLayout& l) {
    if (l.tilePtrSize > 0)
        return l.tilePtrSize;
    return ptrSizeOf(l);
}

int itemStride(const MapLayout& l) {
    if (l.itemPtrSize > 0)
        return l.itemPtrSize;
    return tileStride(l);
}

int iabs(int v) {
    return v < 0 ? -v : v;
}

int chebyshev(int ax, int ay, int bx, int by) {
    const int dx = iabs(ax - bx);
    const int dy = iabs(ay - by);
    return dx > dy ? dx : dy;
}

bool inModule(std::uintptr_t p) {
    const std::uintptr_t mod = moduleBase();
    const std::size_t img = moduleImageSize();
    return img && p >= mod && p < mod + img;
}

bool regionWritable(const MEMORY_BASIC_INFORMATION& mbi) {
    if (mbi.State != MEM_COMMIT)
        return false;
    if (memPageGuarded(mbi.Protect))
        return false;
    const DWORD prot = mbi.Protect & 0xFF;
    return prot == PAGE_READWRITE || prot == PAGE_EXECUTE_READWRITE || prot == PAGE_WRITECOPY ||
           prot == PAGE_EXECUTE_WRITECOPY;
}

bool looksLikeVtable(std::uintptr_t obj) {
    const std::uintptr_t mod = moduleBase();
    const std::size_t img = moduleImageSize();
    std::uintptr_t vptr = 0;
    if (!readPtr(obj, vptr) || vptr < mod || vptr >= mod + img)
        return false;
    std::uintptr_t fn = 0;
    if (!readPtr(vptr, fn) || fn < mod || fn >= mod + img)
        return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(fn), &mbi, sizeof(mbi)))
        return false;
    if (memPageGuarded(mbi.Protect))
        return false;
    const DWORD prot = mbi.Protect & 0xFF;
    return prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE;
}

bool looksLikeHeapObject(std::uintptr_t p) {
    return looksLikeHeapPtr(p);
}

bool isExec(std::uintptr_t p) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(p), &mbi, sizeof(mbi)))
        return false;
    if (memPageGuarded(mbi.Protect))
        return false;
    const DWORD prot = mbi.Protect & 0xFF;
    return prot == PAGE_EXECUTE || prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE ||
           prot == PAGE_EXECUTE_WRITECOPY;
}

std::string imageRttiName(std::uintptr_t obj) {
    std::uintptr_t vtbl = 0;
    if (!readPtr(obj, vtbl) || !inModule(vtbl))
        return {};
    std::uintptr_t col = 0;
    if (!readPtr(vtbl - sizeof(void*), col) || !inModule(col))
        return {};
    std::uintptr_t td = 0;
#if defined(_WIN64)
    std::uint32_t tdRva = 0;
    if (!memReadT(col + 12, tdRva))
        return {};
    td = moduleBase() + static_cast<std::uintptr_t>(tdRva);
#else
    if (!readPtr(col + 12, td) || !inModule(td))
        return {};
#endif
    if (!inModule(td))
        return {};
    char name[96]{};
    if (!memRead(reinterpret_cast<const void*>(td + sizeof(void*) * 2), name,
                 sizeof(name) - 1))
        return {};
    name[sizeof(name) - 1] = 0;
    return name;
}

bool isTileObject(std::uintptr_t p) {
    if (!p || !looksLikeHeapObject(p) || !looksLikeVtable(p))
        return false;
    const std::string n = imageRttiName(p);
    return n.find("Tile") != std::string::npos && n.find("_Ref_count") == std::string::npos;
}

bool skipThingRtti(const std::string& n) {
    return n.find("Creature") != std::string::npos || n.find("LocalPlayer") != std::string::npos ||
           n.find("Player") != std::string::npos || n.find("Monster") != std::string::npos ||
           n.find("Npc") != std::string::npos || n.find("Effect") != std::string::npos ||
           n.find("Missile") != std::string::npos || n.find("AnimatedText") != std::string::npos;
}

bool creatureThingRtti(const std::string& n) {
    return n.find("Creature") != std::string::npos || n.find("LocalPlayer") != std::string::npos ||
           n.find("Player") != std::string::npos || n.find("Monster") != std::string::npos ||
           n.find("Npc") != std::string::npos;
}

bool looksLikeItemId(std::uint16_t id) {
    return id >= 100 && id < 60000;
}

std::uintptr_t nodePtr(std::uintptr_t node, std::int32_t off) {
    std::uintptr_t p = 0;
    if (!readPtr(node + static_cast<std::uintptr_t>(off), p))
        return 0;
    return p;
}

std::uintptr_t mapObject(const MapLayout& l) {
    const std::uintptr_t base = moduleBase();
    if (l.mapRva) {
        const std::uintptr_t addr =
            base + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(l.mapRva));
        if (addr && (inModule(addr) || looksLikeHeapObject(addr)))
            return addr;
    }
    if (l.chain.empty())
        return 0;
    std::uintptr_t p = base;
    for (const auto& step : l.chain) {
        p += static_cast<std::uintptr_t>(static_cast<std::intptr_t>(step.offset));
        if (!step.deref)
            continue;
        std::uintptr_t next = 0;
        if (!readPtr(p, next) || !next)
            return 0;
        p = next;
    }
    return p;
}

std::uintptr_t unwrapTile(std::uintptr_t slotOrObj) {
    if (!slotOrObj)
        return 0;
    if (isTileObject(slotOrObj))
        return slotOrObj;
    if (!looksLikeHeapPtr(slotOrObj) && !inModule(slotOrObj))
        return 0;
    std::uintptr_t ptr = 0;
    std::uintptr_t rep = 0;
    if (!readPtr(slotOrObj, ptr))
        return 0;
    readPtr(slotOrObj + sizeof(void*), rep);
    if (isTileObject(ptr))
        return ptr;
    if (completeSharedPtr(ptr, rep) && isTileObject(ptr))
        return ptr;
    return 0;
}

bool readSharedSlot(std::uintptr_t addr, int stride, std::uintptr_t& ptr, std::uintptr_t& rep) {
    ptr = 0;
    rep = 0;
    if (!readPtr(addr, ptr) || !ptr)
        return false;
    if (stride >= static_cast<int>(sizeof(void*) * 2)) {
        readPtr(addr + sizeof(void*), rep);
        return completeSharedPtr(ptr, rep);
    }
    // Some calibrated clients use an intrusive one-pointer ThingPtr. Keep the
    // established shared_ptr path above strict and accept this ABI only when the
    // profile's slot stride explicitly says that the handle is one pointer wide.
    if (stride == static_cast<int>(sizeof(void*)))
        return looksLikeHeapPtr(ptr);
    return false;
}

int tileIndex(int x, int y, const MapLayout& l) {
    const int bs = l.blockSize > 0 ? l.blockSize : 32;
    const int lx = x % bs;
    const int ly = y % bs;
    if (l.indexOrder == "yx")
        return ly + lx * bs;
    return lx + ly * bs;
}

std::uint32_t blockId(int x, int y, const MapLayout& l) {
    const int bs = l.blockSize > 0 ? l.blockSize : 32;
    const std::uint32_t bx = static_cast<std::uint32_t>(x / bs);
    const std::uint32_t by = static_cast<std::uint32_t>(y / bs);
    if (l.keyOrder == "otc" && bs > 1 && (65536 % bs) == 0) {
        const auto mul = static_cast<std::uint32_t>(65536 / bs);
        return by * mul + bx;
    }
    if (l.keyOrder == "otc-xy" && bs > 1 && (65536 % bs) == 0) {
        const auto mul = static_cast<std::uint32_t>(65536 / bs);
        return bx * mul + by;
    }
    if (l.keyOrder == "y16x")
        return (by << 16) | bx;
    return (bx << 16) | by;
}

void coordsFromIndex(std::uint32_t key, int index, const MapLayout& l, int& x, int& y) {
    const int bs = l.blockSize > 0 ? l.blockSize : 32;
    std::uint32_t bx = key >> 16;
    std::uint32_t by = key & 0xFFFFu;
    if (l.keyOrder == "otc" && bs > 1 && (65536 % bs) == 0) {
        const auto mul = static_cast<std::uint32_t>(65536 / bs);
        by = mul ? key / mul : 0;
        bx = mul ? key % mul : key;
    } else if (l.keyOrder == "otc-xy" && bs > 1 && (65536 % bs) == 0) {
        const auto mul = static_cast<std::uint32_t>(65536 / bs);
        bx = mul ? key / mul : 0;
        by = mul ? key % mul : key;
    } else if (l.keyOrder == "y16x") {
        by = key >> 16;
        bx = key & 0xFFFFu;
    }
    int lx = 0;
    int ly = 0;
    if (l.indexOrder == "yx") {
        ly = index % bs;
        lx = index / bs;
    } else {
        lx = index % bs;
        ly = index / bs;
    }
    x = static_cast<int>(bx) * bs + lx;
    y = static_cast<int>(by) * bs + ly;
}

bool readItemFields(std::uintptr_t item, const MapLayout& l, int& id, int& count) {
    id = 0;
    count = 0;
    std::uint16_t wid = 0;
    if (!memReadT(item + static_cast<std::uintptr_t>(l.itemIdOffset), wid) || !looksLikeItemId(wid))
        return false;
    std::uint8_t c = 0;
    memReadT(item + static_cast<std::uintptr_t>(l.itemCountOffset), c);
    id = wid;
    count = c > 0 ? static_cast<int>(c) : 1;
    return true;
}

bool readTileItems(std::uintptr_t tile, const MapLayout& l, std::vector<MapItem>& items) {
    items.clear();
    if (!tile)
        return false;
    const int es = itemStride(l);
    if (es < 4)
        return false;
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
    const std::uintptr_t vec = tile + static_cast<std::uintptr_t>(l.thingsOffset);
    if (!readPtr(vec, begin) || !readPtr(vec + static_cast<std::uintptr_t>(ptrSizeOf(l)), end))
        return false;
    if (end < begin || ((end - begin) % static_cast<std::uintptr_t>(es)) != 0)
        return false;
    const std::size_t n = static_cast<std::size_t>(end - begin) / static_cast<std::size_t>(es);
    if (n == 0 || n > static_cast<std::size_t>(kMaxThings))
        return false;
    for (std::size_t i = 0; i < n; ++i) {
        std::uintptr_t ptr = 0;
        std::uintptr_t rep = 0;
        if (!readSharedSlot(begin + i * static_cast<std::uintptr_t>(es), es, ptr, rep))
            continue;
        const std::string rtti = imageRttiName(ptr);
        if (skipThingRtti(rtti))
            continue;
        int id = 0;
        int count = 0;
        if (!readItemFields(ptr, l, id, count))
            continue;
        if (!rtti.empty() && rtti.find("Item") == std::string::npos &&
            rtti.find("Thing") == std::string::npos)
            continue;
        items.push_back(MapItem{id, count});
    }
    return !items.empty();
}

bool plausiblePos(int x, int y, int z) {
    return z >= 0 && z <= 15 && x >= 0 && y >= 0 && x <= 65535 && y <= 65535;
}

bool readTilePos(std::uintptr_t tile, const MapLayout& l, int& x, int& y, int& z) {
    if (l.tilePositionOffset < 0)
        return false;
    const std::uintptr_t addr = tile + static_cast<std::uintptr_t>(l.tilePositionOffset);
    if (l.tilePositionPacked) {
        std::uint16_t px = 0;
        std::uint16_t py = 0;
        std::uint8_t pz = 0;
        if (!memReadT(addr, px) || !memReadT(addr + 2, py) || !memReadT(addr + 4, pz))
            return false;
        x = px;
        y = py;
        z = pz;
        return plausiblePos(x, y, z);
    }
    std::int32_t xyz[3]{};
    if (!memRead(reinterpret_cast<const void*>(addr), xyz, sizeof(xyz)))
        return false;
    x = xyz[0];
    y = xyz[1];
    z = xyz[2];
    return plausiblePos(x, y, z);
}

bool discoverTilePosition(std::uintptr_t tile, const Position& pos, MapLayout& l) {
    for (int off = 4; off <= 160; off += 4) {
        std::int32_t xyz[3]{};
        if (!memRead(reinterpret_cast<const void*>(tile + static_cast<std::uintptr_t>(off)), xyz,
                     sizeof(xyz)))
            continue;
        if (xyz[0] == pos.x && xyz[1] == pos.y && xyz[2] == pos.z) {
            l.tilePositionOffset = off;
            l.tilePositionPacked = false;
            return true;
        }
    }
    for (int off = 4; off <= 160; off += 2) {
        std::uint16_t px = 0;
        std::uint16_t py = 0;
        std::uint8_t pz = 0;
        if (!memReadT(tile + static_cast<std::uintptr_t>(off), px) ||
            !memReadT(tile + static_cast<std::uintptr_t>(off) + 2, py) ||
            !memReadT(tile + static_cast<std::uintptr_t>(off) + 4, pz))
            continue;
        if (px == pos.x && py == pos.y && pz == pos.z) {
            l.tilePositionOffset = off;
            l.tilePositionPacked = true;
            return true;
        }
    }
    return false;
}

bool looksLikeThingVector(std::uintptr_t tile, int off, int es, int ps, int expectId,
                          const MapLayout& itemLay, int& itemHits, int& score) {
    itemHits = 0;
    score = 0;
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
    std::uintptr_t capacity = 0;
    if (!readPtr(tile + static_cast<std::uintptr_t>(off), begin) ||
        !readPtr(tile + static_cast<std::uintptr_t>(off + ps), end) ||
        !readPtr(tile + static_cast<std::uintptr_t>(off + ps * 2), capacity))
        return false;
    if (end < begin || capacity < end ||
        ((end - begin) % static_cast<std::uintptr_t>(es)) != 0 ||
        ((capacity - begin) % static_cast<std::uintptr_t>(es)) != 0)
        return false;
    const std::size_t n = static_cast<std::size_t>(end - begin) / static_cast<std::size_t>(es);
    const std::size_t cap =
        static_cast<std::size_t>(capacity - begin) / static_cast<std::size_t>(es);
    if (n == 0 || n > static_cast<std::size_t>(kMaxThings) || cap < n ||
        cap > static_cast<std::size_t>(kMaxThings))
        return false;
    int items = 0;
    int typedItems = 0;
    int creatures = 0;
    bool hint = expectId == 0;
    for (std::size_t i = 0; i < n; ++i) {
        std::uintptr_t ptr = 0;
        std::uintptr_t rep = 0;
        if (!readSharedSlot(begin + i * static_cast<std::uintptr_t>(es), es, ptr, rep))
            continue;
        const std::string rtti = imageRttiName(ptr);
        if (creatureThingRtti(rtti)) {
            ++creatures;
            continue;
        }
        if (skipThingRtti(rtti))
            continue;
        int id = 0;
        int count = 0;
        if (!readItemFields(ptr, itemLay, id, count))
            continue;
        if (!rtti.empty() && rtti.find("Item") == std::string::npos &&
            rtti.find("Thing") == std::string::npos)
            continue;
        ++items;
        if (!rtti.empty())
            ++typedItems;
        if (expectId && id == expectId)
            hint = true;
    }
    itemHits = items;
    // The real Tile::m_things normally contains the local player/creatures as
    // well as Items. Prefer that semantic signal over incidental pointer pairs,
    // while retaining an Item-only fallback for clients that split creatures.
    score = items * 10 + typedItems * 5 + creatures * 100;
    if (expectId && hint)
        score += 1000;
    return items > 0 && hint;
}

bool discoverThings(std::uintptr_t tile, const MapCalibHints& hints, const Profile& profile,
                    MapLayout& l) {
    if (profile.inventory.present) {
        l.itemIdOffset = profile.inventory.itemIdOffset;
        l.itemCountOffset = profile.inventory.itemCountOffset;
        if (profile.inventory.ptrSize > 0)
            l.itemPtrSize = profile.inventory.ptrSize;
    } else if (profile.containers.present) {
        l.itemIdOffset = profile.containers.itemIdOffset;
        l.itemCountOffset = profile.containers.itemCountOffset;
        if (profile.containers.itemPtrSize > 0)
            l.itemPtrSize = profile.containers.itemPtrSize;
    }
    if (l.itemPtrSize <= 0)
        l.itemPtrSize = 8;
    if (l.tilePtrSize <= 0)
        l.tilePtrSize = l.itemPtrSize;
    if (l.itemIdOffset == 0) {
        const std::int32_t idTries[] = {48, 40, 36, 32, 44, 52};
        for (std::int32_t idOff : idTries) {
            l.itemIdOffset = idOff;
            l.itemCountOffset = idOff + 4;
            MapLayout tmp = l;
            int hits = 0;
            int score = 0;
            if (looksLikeThingVector(tile, 0x28, itemStride(l), ptrSizeOf(l),
                                     hints.hasItemId ? hints.itemId : 0, tmp, hits, score) &&
                hits > 0)
                break;
            l.itemIdOffset = idOff;
            l.itemCountOffset = idOff + 2;
        }
    }
    const int ps = ptrSizeOf(l);
    const int es = itemStride(l);
    const int expectId = hints.hasItemId ? hints.itemId : 0;
    int bestOff = -1;
    int bestScore = -1;
    for (int off = 8; off <= 160; off += 4) {
        int hits = 0;
        int score = 0;
        if (!looksLikeThingVector(tile, off, es, ps, expectId, l, hits, score))
            continue;
        LOG("map things candidate off=%d items=%d score=%d", off, hits, score);
        if (score > bestScore) {
            bestScore = score;
            bestOff = off;
        }
    }
    if (bestOff < 0)
        return false;
    l.thingsOffset = bestOff;
    l.thingsKind = "vector";
    std::vector<MapItem> items;
    if (!readTileItems(tile, l, items))
        return false;
    if (hints.hasItemId) {
        bool ok = false;
        for (const auto& it : items) {
            if (it.id == hints.itemId)
                ok = true;
        }
        if (!ok)
            return false;
    }
    return true;
}

void* sehGetTile(void* map, std::uintptr_t fn, const OtcPos* pos) {
    DWORD code = 0;
    void* ret = nullptr;
    __try {
        ret = reinterpret_cast<GetTileFn>(fn)(map, pos);
    } __except ((code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)) {
        LOG("map scan getTile exception code=%lu map=%p fn=%p pos=%d,%d,%d", code, map,
            reinterpret_cast<void*>(fn), pos->x, pos->y, pos->z);
        return nullptr;
    }
    if (inModule(reinterpret_cast<std::uintptr_t>(ret)))
        return nullptr;
    return ret;
}

bool tryGetTileNeedle(const Profile& profile, const Position& pos, std::uintptr_t& slotOut,
                      std::uintptr_t& tileOut) {
    slotOut = 0;
    tileOut = 0;
    if (!profile.actions.mapRva || !profile.actions.getTileRva)
        return false;
    const std::uintptr_t base = moduleBase();
    void* map = reinterpret_cast<void*>(
        base + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(profile.actions.mapRva)));
    const std::uintptr_t fn =
        base + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(profile.actions.getTileRva));
    OtcPos p;
    p.x = pos.x;
    p.y = pos.y;
    p.z = pos.z;
    void* slot = sehGetTile(map, fn, &p);
    if (!slot)
        return false;
    const auto slotAddr = reinterpret_cast<std::uintptr_t>(slot);
    const std::uintptr_t tile = unwrapTile(slotAddr);
    if (!tile)
        return false;
    slotOut = slotAddr;
    tileOut = tile;
    LOG("map scan getTile %d,%d,%d slot=%p tile=%p rtti=%s", pos.x, pos.y, pos.z,
        reinterpret_cast<void*>(slotAddr), reinterpret_cast<void*>(tile),
        imageRttiName(tile).c_str());
    return true;
}

bool plausibleLink(std::uintptr_t p) {
    return p && (looksLikeHeapObject(p) || inModule(p));
}

bool listLinkOk(std::uintptr_t node, int ps) {
    std::uintptr_t next = 0;
    std::uintptr_t prev = 0;
    if (!readPtr(node, next) || !readPtr(node + static_cast<std::uintptr_t>(ps), prev))
        return false;
    if (!plausibleLink(next) || !plausibleLink(prev))
        return false;
    std::uintptr_t nextPrev = 0;
    std::uintptr_t prevNext = 0;
    readPtr(next + static_cast<std::uintptr_t>(ps), nextPrev);
    readPtr(prev, prevNext);
    return nextPrev == node || prevNext == node;
}

bool nodeIsNil(std::uintptr_t node, const MapLayout& l) {
    std::uint8_t isnil = 0;
    if (!memReadT(node + static_cast<std::uintptr_t>(l.mapNodeIsnilOffset), isnil))
        return true;
    return isnil != 0;
}

bool treeParentOk(std::uintptr_t node, const MapLayout& l) {
    const std::uintptr_t parent = nodePtr(node, l.mapNodeParentOffset);
    if (!plausibleLink(parent))
        return false;
    if (nodeIsNil(parent, l))
        return true;
    const std::uintptr_t left = nodePtr(parent, l.mapNodeLeftOffset);
    const std::uintptr_t right = nodePtr(parent, l.mapNodeRightOffset);
    return left == node || right == node;
}

bool packedPosKey(std::uint32_t key, const Position& pos, MapLayout& trial) {
    const auto x = static_cast<std::uint32_t>(pos.x) & 0xFFFFu;
    const auto y = static_cast<std::uint32_t>(pos.y) & 0xFFFFu;
    if (key == ((x << 16) | y)) {
        trial.keyOrder = "x16y";
        return true;
    }
    if (key == ((y << 16) | x)) {
        trial.keyOrder = "y16x";
        return true;
    }
    return false;
}

bool matchPosKey(std::uint32_t key, const Position& pos, int bs, MapLayout& trial) {
    if (bs > 1) {
        trial.blockSize = bs;
        const char* orders[] = {"otc", "otc-xy", "x16y", "y16x"};
        for (const char* ko : orders) {
            trial.keyOrder = ko;
            if (blockId(pos.x, pos.y, trial) == key)
                return true;
        }
    }
    trial.blockSize = bs > 0 ? bs : 1;
    if (packedPosKey(key, pos, trial)) {
        trial.blockSize = 1;
        return true;
    }
    return false;
}

void seedTreeFields(MapLayout& trial, int ps) {
    trial.mapNodeLeftOffset = 0;
    trial.mapNodeParentOffset = ps;
    trial.mapNodeRightOffset = ps * 2;
    trial.mapNodeIsnilOffset = ps * 3 + 1;
}

void logProbeSlot(std::uintptr_t slot, std::uintptr_t tile, const Position& pos, int ps, int es) {
    const int idxXy = (pos.x % 32) + (pos.y % 32) * 32;
    const int idxYx = (pos.y % 32) + (pos.x % 32) * 32;
    const std::uint32_t otc = static_cast<std::uint32_t>(pos.y / 32) * 2048u +
                              static_cast<std::uint32_t>(pos.x / 32);
    const std::uint32_t packed = (static_cast<std::uint32_t>(pos.x) << 16) |
                                 (static_cast<std::uint32_t>(pos.y) & 0xFFFFu);
    LOG("map probe slot=%p tile=%p slotMod8=%d idxXy=%d idxYx=%d otcKey=%u packed=%u",
        reinterpret_cast<void*>(slot), reinterpret_cast<void*>(tile), static_cast<int>(slot & 7),
        idxXy, idxYx, otc, packed);
    const int idxs[] = {idxYx, idxXy};
    const char* names[] = {"yx", "xy"};
    const int offs[] = {16, 20, 24, 28, 32};
    for (int n = 0; n < 2; ++n) {
        const std::uintptr_t array =
            slot - static_cast<std::uintptr_t>(idxs[n]) * static_cast<std::uintptr_t>(es);
        LOG("map probe order=%s array=%p", names[n], reinterpret_cast<void*>(array));
        for (int arrayOff : offs) {
            const std::uintptr_t node = array - static_cast<std::uintptr_t>(arrayOff);
            std::uint32_t w[8]{};
            memRead(reinterpret_cast<const void*>(node), w, sizeof(w));
            std::uint8_t isnil = 0;
            memReadT(node + 13, isnil);
            LOG("map probe %s arrayOff=%d node=%p isnil=%u w0=%08X w1=%08X w2=%08X w3=%08X w4=%08X "
                "w5=%08X",
                names[n], arrayOff, reinterpret_cast<void*>(node), isnil, w[0], w[1], w[2], w[3],
                w[4], w[5]);
        }
    }
    (void)ps;
    (void)tile;
}

bool finishBlockLayout(MapLayout& trial, MapLayout& l, const char* kind, int bs, const char* order,
                       int keyOff, int arrayOff, std::uint32_t key) {
    trial.blockSize = bs;
    trial.indexOrder = order;
    trial.tileArrayOffset = arrayOff;
    l = trial;
    LOG("map %s layout bs=%d order=%s key=%s keyOff=%d arrayOff=%d vec=%d key=%u (0x%X)", kind, bs,
        order, trial.keyOrder.c_str(), keyOff, arrayOff, trial.tilesVector ? 1 : 0, key, key);
    return true;
}

bool acceptNodeKey(std::uintptr_t node, int arrayOff, const Position& pos, int bs, const char* order,
                   MapLayout trial, MapLayout& l, const char* kind, bool vec) {
    const int ps = ptrSizeOf(trial);
    if (node % static_cast<std::uintptr_t>(ps) != 0)
        return false;
    const bool blockTree = std::strcmp(kind, "map-blocks") == 0;
    const int keyTry[] = {16, 12, 20, 24, 8, arrayOff - 4, arrayOff - 8};
    for (int keyOff : keyTry) {
        if (keyOff < 8 || keyOff >= arrayOff)
            continue;
        if (blockTree && (keyOff < 16 || arrayOff < 20))
            continue;
        std::uint32_t key = 0;
        if (!memReadT(node + static_cast<std::uintptr_t>(keyOff), key) || !key)
            continue;
        if (!matchPosKey(key, pos, bs, trial))
            continue;
        if (blockTree) {
            if (nodeIsNil(node, trial))
                continue;
            if (!treeParentOk(node, trial)) {
                const std::uintptr_t parent = nodePtr(node, trial.mapNodeParentOffset);
                const std::uintptr_t left = nodePtr(node, trial.mapNodeLeftOffset);
                const std::uintptr_t right = nodePtr(node, trial.mapNodeRightOffset);
                if (!plausibleLink(parent))
                    continue;
                if (!plausibleLink(left) && !plausibleLink(right))
                    continue;
            }
        }
        trial.collection = std::string(kind);
        trial.mapNodeKeyOffset = keyOff;
        trial.listKeyOffset = keyOff;
        trial.listNextOffset = 0;
        trial.listPrevOffset = ps;
        trial.tilesVector = vec;
        const int useBs = trial.blockSize > 0 ? trial.blockSize : bs;
        LOG("map node=%p kind=%s keyOff=%d arrayOff=%d", reinterpret_cast<void*>(node), kind, keyOff,
            arrayOff);
        return finishBlockLayout(trial, l, kind, useBs, order, keyOff, arrayOff, key);
    }
    return false;
}

bool deriveEmbeddedNode(std::uintptr_t array, const Position& pos, int bs, const char* order,
                        MapLayout& l) {
    const int ps = ptrSizeOf(l);
    MapLayout trial = l;
    trial.blockSize = bs;
    trial.indexOrder = order;
    seedTreeFields(trial, ps);
    for (int arrayOff = 20; arrayOff <= 64; arrayOff += 4) {
        const std::intptr_t nodeI =
            static_cast<std::intptr_t>(array) - static_cast<std::intptr_t>(arrayOff);
        if (nodeI <= 0x10000)
            continue;
        if (acceptNodeKey(static_cast<std::uintptr_t>(nodeI), arrayOff, pos, bs, order, trial, l,
                          "map-blocks", false))
            return true;
    }
    return false;
}

bool deriveVectorNode(std::uintptr_t array, const Position& pos, int bs, const char* order,
                      MapLayout& l, DWORD t0) {
    std::vector<std::uintptr_t> hits;
    findPointersTo(array, hits, t0);
    const int ps = ptrSizeOf(l);
    MapLayout trial = l;
    trial.blockSize = bs;
    trial.indexOrder = order;
    seedTreeFields(trial, ps);
    for (auto hit : hits) {
        const int vecOffs[] = {16, 20, 24, 28, 32, 12, 36, 40};
        for (int vecOff : vecOffs) {
            if (vecOff < ps * 2)
                continue;
            const std::intptr_t nodeI =
                static_cast<std::intptr_t>(hit) - static_cast<std::intptr_t>(vecOff);
            if (nodeI <= 0x10000)
                continue;
            std::uintptr_t stored = 0;
            if (!readPtr(static_cast<std::uintptr_t>(nodeI) + static_cast<std::uintptr_t>(vecOff),
                         stored) ||
                stored != array)
                continue;
            if (acceptNodeKey(static_cast<std::uintptr_t>(nodeI), vecOff, pos, bs, order, trial, l,
                              "map-blocks", true))
                return true;
        }
    }
    return false;
}

bool deriveTreeBlockLayout(std::uintptr_t slot, std::uintptr_t tile, const Position& pos,
                           MapLayout& l) {
    const int es = tileStride(l);
    if (es < 4)
        return false;
    const int sizes[] = {32, 16, 8, 64};
    const char* orders[] = {"xy", "yx"};
    for (int bs : sizes) {
        for (const char* order : orders) {
            MapLayout trial = l;
            trial.blockSize = bs;
            trial.indexOrder = order;
            const int idx = tileIndex(pos.x, pos.y, trial);
            if (idx < 0 || idx >= bs * bs)
                continue;
            const std::uintptr_t array =
                slot - static_cast<std::uintptr_t>(idx) * static_cast<std::uintptr_t>(es);
            if (unwrapTile(array + static_cast<std::uintptr_t>(idx * es)) != tile)
                continue;
            if (deriveEmbeddedNode(array, pos, bs, order, l))
                return true;
        }
    }
    return false;
}

bool deriveListBlockLayout(std::uintptr_t slot, std::uintptr_t tile, const Position& pos,
                           MapLayout& l) {
    const int ps = ptrSizeOf(l);
    const int es = tileStride(l);
    if (es < 4)
        return false;
    const int sizes[] = {32, 16, 8, 64};
    const char* orders[] = {"yx", "xy"};
    for (int bs : sizes) {
        for (const char* order : orders) {
            MapLayout trial = l;
            trial.blockSize = bs;
            trial.indexOrder = order;
            const int idx = tileIndex(pos.x, pos.y, trial);
            if (idx < 0 || idx >= bs * bs)
                continue;
            const std::uintptr_t array =
                slot - static_cast<std::uintptr_t>(idx) * static_cast<std::uintptr_t>(es);
            if (unwrapTile(array + static_cast<std::uintptr_t>(idx * es)) != tile)
                continue;
            for (int arrayOff = ps * 2; arrayOff <= 64; arrayOff += 4) {
                const std::intptr_t nodeI =
                    static_cast<std::intptr_t>(array) - static_cast<std::intptr_t>(arrayOff);
                if (nodeI <= 0x10000)
                    continue;
                const auto node = static_cast<std::uintptr_t>(nodeI);
                if (!listLinkOk(node, ps))
                    continue;
                if (acceptNodeKey(node, arrayOff, pos, bs, order, trial, l, "umap-blocks", false))
                    return true;
            }
        }
    }
    return false;
}

bool deriveSingleTileLayout(std::uintptr_t slot, std::uintptr_t tile, const Position& pos,
                            MapLayout& l) {
    const int ps = ptrSizeOf(l);
    const int ptrOffs[] = {16, 20, 24, 28, 32, 12, 8, 36, 40};
    MapLayout trial = l;
    seedTreeFields(trial, ps);
    trial.blockSize = 1;
    trial.indexOrder = "xy";
    for (int ptrOff : ptrOffs) {
        if (ptrOff < 8)
            continue;
        const std::intptr_t nodeI =
            static_cast<std::intptr_t>(slot) - static_cast<std::intptr_t>(ptrOff);
        if (nodeI <= 0x10000)
            continue;
        const auto node = static_cast<std::uintptr_t>(nodeI);
        if (unwrapTile(node + static_cast<std::uintptr_t>(ptrOff)) != tile)
            continue;
        if (acceptNodeKey(node, ptrOff, pos, 1, "xy", trial, l, "map-tiles", false))
            return true;
        if (listLinkOk(node, ps) &&
            acceptNodeKey(node, ptrOff, pos, 1, "xy", trial, l, "umap-tiles", false))
            return true;
    }
    return false;
}

bool deriveVectorLayouts(std::uintptr_t slot, std::uintptr_t tile, const Position& pos, MapLayout& l,
                         DWORD t0) {
    const int es = tileStride(l);
    const int sizes[] = {32, 16, 8, 64};
    const char* orders[] = {"yx", "xy"};
    std::set<std::uintptr_t> arrays;
    for (int bs : sizes) {
        for (const char* order : orders) {
            MapLayout trial = l;
            trial.blockSize = bs;
            trial.indexOrder = order;
            const int idx = tileIndex(pos.x, pos.y, trial);
            if (idx < 0 || idx >= bs * bs)
                continue;
            const std::uintptr_t array =
                slot - static_cast<std::uintptr_t>(idx) * static_cast<std::uintptr_t>(es);
            if (unwrapTile(array + static_cast<std::uintptr_t>(idx * es)) != tile)
                continue;
            if (arrays.count(array))
                continue;
            arrays.insert(array);
            if (deriveVectorNode(array, pos, bs, order, l, t0))
                return true;
        }
    }
    return false;
}

bool deriveBlockLayout(std::uintptr_t slot, std::uintptr_t tile, const Position& pos, MapLayout& l) {
    if (deriveTreeBlockLayout(slot, tile, pos, l))
        return true;
    if (deriveListBlockLayout(slot, tile, pos, l))
        return true;
    if (deriveSingleTileLayout(slot, tile, pos, l))
        return true;
    LOG("map derive miss slot=%p tile=%p slotMod8=%d", reinterpret_cast<void*>(slot),
        reinterpret_cast<void*>(tile), static_cast<int>(slot & 7));
    return false;
}

std::uintptr_t tileArrayOf(std::uintptr_t node, const MapLayout& l) {
    if (l.tilesVector) {
        std::uintptr_t array = 0;
        if (!readPtr(node + static_cast<std::uintptr_t>(l.tileArrayOffset), array))
            return 0;
        return array;
    }
    return node + static_cast<std::uintptr_t>(l.tileArrayOffset);
}

std::uintptr_t nodeFromSlot(std::uintptr_t slot, const Position& pos, const MapLayout& l, DWORD t0) {
    const int es = tileStride(l);
    const int idx = l.blockSize <= 1 ? 0 : tileIndex(pos.x, pos.y, l);
    if (idx < 0)
        return 0;
    const std::uintptr_t array =
        slot - static_cast<std::uintptr_t>(idx) * static_cast<std::uintptr_t>(es);
    if (!l.tilesVector)
        return array - static_cast<std::uintptr_t>(l.tileArrayOffset);
    std::vector<std::uintptr_t> hits;
    findPointersTo(array, hits, t0);
    for (auto hit : hits) {
        const std::intptr_t nodeI =
            static_cast<std::intptr_t>(hit) - static_cast<std::intptr_t>(l.tileArrayOffset);
        if (nodeI <= 0x10000)
            continue;
        std::uintptr_t stored = 0;
        if (!readPtr(static_cast<std::uintptr_t>(nodeI) +
                         static_cast<std::uintptr_t>(l.tileArrayOffset),
                     stored) ||
            stored != array)
            continue;
        return static_cast<std::uintptr_t>(nodeI);
    }
    return 0;
}

bool slotLooksLikeCopy(std::uintptr_t hit, std::uintptr_t tile, std::uintptr_t realSlot) {
    if (hit >= tile && hit < tile + 512)
        return true;
    if (realSlot && (hit & 7) != (realSlot & 7))
        return true;
    return false;
}

struct UMapWalk {
    std::uintptr_t sentinel = 0;
    std::vector<std::uintptr_t> nodes;
};

bool walkUMapNodes(std::uintptr_t start, const MapLayout& l, UMapWalk& out) {
    out = {};
    if (!start)
        return false;
    std::set<std::uintptr_t> seen;
    std::uintptr_t node = start;
    std::vector<std::uintptr_t> ring;
    for (int i = 0; i < kMaxBlocks + 4; ++i) {
        if (!node || seen.count(node))
            break;
        seen.insert(node);
        ring.push_back(node);
        node = nodePtr(node, l.listNextOffset);
    }
    if (ring.size() < 2)
        return false;
    std::uintptr_t sentinel = 0;
    for (auto n : ring) {
        std::uint32_t key = 0;
        memReadT(n + static_cast<std::uintptr_t>(l.listKeyOffset), key);
        std::uintptr_t nxt = nodePtr(n, l.listNextOffset);
        std::uintptr_t prv = nodePtr(n, l.listPrevOffset);
        if (!key || !nxt || !prv) {
            if (!sentinel)
                sentinel = n;
            continue;
        }
        out.nodes.push_back(n);
    }
    if (!sentinel) {
        for (auto n : ring) {
            std::uintptr_t v = 0;
            readPtr(n + static_cast<std::uintptr_t>(l.tileArrayOffset), v);
            if (!v) {
                sentinel = n;
                break;
            }
        }
    }
    if (!sentinel && !ring.empty())
        sentinel = ring[0];
    out.sentinel = sentinel;
    return !out.nodes.empty() || sentinel != 0;
}

bool walkUMap(std::uintptr_t umap, const MapLayout& l, UMapWalk& out) {
    std::uintptr_t head = 0;
    if (!readPtr(umap + static_cast<std::uintptr_t>(l.umapHeadOffset), head) || !head)
        return false;
    const std::uintptr_t first = nodePtr(head, l.listNextOffset);
    return walkUMapNodes(first && first != head ? first : head, l, out);
}

void walkMapBlockNodes(std::uintptr_t node, const MapLayout& l, std::set<std::uintptr_t>& seen,
                       std::vector<std::uintptr_t>& out) {
    if (!node || seen.size() > static_cast<std::size_t>(kMaxBlocks) + 8 || seen.count(node))
        return;
    seen.insert(node);
    if (nodeIsNil(node, l))
        return;
    walkMapBlockNodes(nodePtr(node, l.mapNodeLeftOffset), l, seen, out);
    out.push_back(node);
    walkMapBlockNodes(nodePtr(node, l.mapNodeRightOffset), l, seen, out);
}

bool walkTreeMap(std::uintptr_t mapObj, const MapLayout& l, UMapWalk& out) {
    out = {};
    std::uintptr_t head = 0;
    if (!readPtr(mapObj + static_cast<std::uintptr_t>(l.mapHeadOffset), head) || !head)
        return false;
    out.sentinel = head;
    const std::uintptr_t root = nodePtr(head, l.mapNodeParentOffset);
    std::set<std::uintptr_t> seen;
    walkMapBlockNodes(root, l, seen, out.nodes);
    return !out.nodes.empty() || head != 0;
}

std::uintptr_t walkToSentinel(std::uintptr_t node, const MapLayout& l) {
    const std::uintptr_t start = node;
    for (int i = 0; i < 64; ++i) {
        if (!node)
            break;
        if (nodeIsNil(node, l))
            return node;
        const std::uintptr_t parent = nodePtr(node, l.mapNodeParentOffset);
        if (!parent || parent == node)
            break;
        node = parent;
    }
    std::uint8_t isnil = 0;
    memReadT(start + static_cast<std::uintptr_t>(l.mapNodeIsnilOffset), isnil);
    LOG("map sentinel miss node=%p parent=%p isnil=%u keyOff=%d", reinterpret_cast<void*>(start),
        reinterpret_cast<void*>(nodePtr(start, l.mapNodeParentOffset)), isnil, l.mapNodeKeyOffset);
    return 0;
}

void findPointersTo(std::uintptr_t target, std::vector<std::uintptr_t>& hits, DWORD t0) {
    hits.clear();
    MEMORY_BASIC_INFORMATION mbi{};
    std::uintptr_t addr = 0;
    const int ps = static_cast<int>(sizeof(void*));
    while (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi))) {
        if (GetTickCount() - t0 > kScanBudgetMs)
            break;
        const std::uintptr_t next =
            reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionWritable(mbi) && mbi.RegionSize >= static_cast<std::size_t>(ps)) {
            const auto base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
            const std::size_t n = mbi.RegionSize;
            for (std::size_t off = 0; off + static_cast<std::size_t>(ps) <= n;
                 off += static_cast<std::size_t>(ps)) {
                std::uintptr_t v = 0;
                if (!memCopy(reinterpret_cast<const void*>(base + off), &v, sizeof(v)))
                    continue;
                if (v == target)
                    hits.push_back(base + off);
                if (hits.size() >= 4096)
                    return;
            }
        }
        if (next <= addr)
            break;
        addr = next;
    }
}

std::uintptr_t findUMapObject(std::uintptr_t sentinel, const MapLayout& l, DWORD t0) {
    std::vector<std::uintptr_t> hits;
    findPointersTo(sentinel, hits, t0);
    const int ps = ptrSizeOf(l);
    std::uintptr_t best = 0;
    int bestScore = -1;
    for (auto addr : hits) {
        std::uint32_t sz = 0;
        if (!memReadT(addr + static_cast<std::uintptr_t>(ps), sz))
            continue;
        if (sz < 1 || sz > static_cast<std::uint32_t>(kMaxBlocks))
            continue;
        MapLayout trial = l;
        trial.umapHeadOffset = 0;
        trial.umapSizeOffset = ps;
        UMapWalk w;
        if (!walkUMap(addr, trial, w) || w.sentinel != sentinel)
            continue;
        int score = static_cast<int>(sz) * 10;
        if (inModule(addr))
            score += 1000;
        if (score > bestScore) {
            bestScore = score;
            best = addr;
        }
    }
    return best;
}

std::uintptr_t findTreeMapObject(std::uintptr_t sentinel, const MapLayout& l,
                                 std::uintptr_t needleNode, DWORD t0) {
    std::vector<std::uintptr_t> hits;
    findPointersTo(sentinel, hits, t0);
    const int ps = ptrSizeOf(l);
    std::uintptr_t best = 0;
    int bestScore = -1;
    for (auto addr : hits) {
        std::uint32_t sz = 0;
        if (!memReadT(addr + static_cast<std::uintptr_t>(ps), sz))
            continue;
        if (sz > static_cast<std::uint32_t>(kMaxBlocks))
            continue;
        MapLayout trial = l;
        trial.mapHeadOffset = 0;
        trial.mapSizeOffset = ps;
        UMapWalk w;
        if (!walkTreeMap(addr, trial, w) || w.sentinel != sentinel)
            continue;
        bool hasNeedle = needleNode == 0;
        for (auto n : w.nodes) {
            if (n == needleNode)
                hasNeedle = true;
        }
        if (!hasNeedle)
            continue;
        int score = static_cast<int>(sz) * 10 + static_cast<int>(w.nodes.size());
        if (inModule(addr))
            score += 1000;
        if (score > bestScore) {
            bestScore = score;
            best = addr;
        }
    }
    return best;
}

bool looksLikeUmap(std::uintptr_t addr, const MapLayout& l, std::uint32_t& sizeOut) {
    sizeOut = 0;
    std::uintptr_t head = 0;
    if (!readPtr(addr + static_cast<std::uintptr_t>(l.umapHeadOffset), head) || !head)
        return false;
    if (!plausibleLink(head))
        return false;
    std::uint32_t sz = 0;
    if (!memReadT(addr + static_cast<std::uintptr_t>(l.umapSizeOffset), sz) || sz > kMaxBlocks)
        return false;
    sizeOut = sz;
    return true;
}

bool looksLikeTreeMap(std::uintptr_t addr, const MapLayout& l, std::uint32_t& sizeOut) {
    sizeOut = 0;
    std::uintptr_t head = 0;
    if (!readPtr(addr + static_cast<std::uintptr_t>(l.mapHeadOffset), head) || !head)
        return false;
    if (!plausibleLink(head))
        return false;
    if (!nodeIsNil(head, l))
        return false;
    std::uint32_t sz = 0;
    if (!memReadT(addr + static_cast<std::uintptr_t>(l.mapSizeOffset), sz) || sz > kMaxBlocks)
        return false;
    sizeOut = sz;
    return true;
}

bool placeBlocksOnMap(std::uintptr_t map, std::uintptr_t col, int z, MapLayout& l) {
    const bool tree = l.collection.find("map-") == 0;
    const int strides[] = {8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 64};
    const int floors = l.floors > 0 ? l.floors : 16;
    for (int stride : strides) {
        const std::intptr_t startI =
            static_cast<std::intptr_t>(col) - static_cast<std::intptr_t>(z) * stride;
        if (startI <= 0)
            continue;
        const auto start = static_cast<std::uintptr_t>(startI);
        if (start < map || start > map + static_cast<std::uintptr_t>(kMaxMapScan))
            continue;
        int ok = 0;
        int nonempty = 0;
        for (int f = 0; f < floors; ++f) {
            const std::uintptr_t slot = start + static_cast<std::uintptr_t>(f * stride);
            std::uint32_t sz = 0;
            const bool hit = tree ? looksLikeTreeMap(slot, l, sz) : looksLikeUmap(slot, l, sz);
            if (!hit)
                continue;
            ++ok;
            if (sz)
                ++nonempty;
        }
        if (ok >= 8 && nonempty >= 1) {
            l.tileBlocksOffset = static_cast<std::int32_t>(start - map);
            l.umapStride = stride;
            l.floors = floors;
            l.maxZ = floors - 1;
            LOG("map tileBlocks off=%d stride=%d floors=%d hits=%d nonempty=%d tree=%d",
                l.tileBlocksOffset, stride, floors, ok, nonempty, tree ? 1 : 0);
            return true;
        }
    }
    if (col >= map && col < map + static_cast<std::uintptr_t>(kMaxMapScan)) {
        l.tileBlocksOffset = static_cast<std::int32_t>(col - map);
        l.umapStride = 0;
        LOG("map tileBlocks single collection off=%d", l.tileBlocksOffset);
        return true;
    }
    return false;
}

std::uintptr_t floorUmap(std::uintptr_t map, const MapLayout& l, int z) {
    if (!map || z < 0 || z > (l.maxZ > 0 ? l.maxZ : 15))
        return 0;
    std::uintptr_t addr = map + static_cast<std::uintptr_t>(l.tileBlocksOffset);
    if (l.umapStride > 0)
        addr += static_cast<std::uintptr_t>(z * l.umapStride);
    return addr;
}

bool readFloorTiles(std::uintptr_t col, const MapLayout& l, const Position& player, int radius,
                    std::vector<TileSnapshot>& out) {
    UMapWalk w;
    const bool tree = l.collection.find("map-") == 0;
    if (tree) {
        if (!walkTreeMap(col, l, w))
            return false;
    } else if (!walkUMap(col, l, w)) {
        return false;
    }
    const int es = tileStride(l);
    const int bs = l.blockSize > 0 ? l.blockSize : 32;
    const int nslot = bs * bs;
    const int keyOff = tree ? l.mapNodeKeyOffset : l.listKeyOffset;
    for (auto node : w.nodes) {
        std::uint32_t key = 0;
        memReadT(node + static_cast<std::uintptr_t>(keyOff), key);
        if (!key)
            continue;
        const std::uintptr_t array = tileArrayOf(node, l);
        if (!array)
            continue;
        for (int i = 0; i < nslot; ++i) {
            const std::uintptr_t tile = unwrapTile(array + static_cast<std::uintptr_t>(i * es));
            if (!tile)
                continue;
            int x = 0;
            int y = 0;
            coordsFromIndex(key, i, l, x, y);
            const int z = player.z;
            if (radius >= 0 && chebyshev(x, y, player.x, player.y) > radius)
                continue;
            TileSnapshot snap;
            snap.x = x;
            snap.y = y;
            snap.z = z;
            if (l.tilePositionOffset >= 0) {
                int tx = 0, ty = 0, tz = 0;
                if (readTilePos(tile, l, tx, ty, tz)) {
                    snap.x = tx;
                    snap.y = ty;
                    snap.z = tz;
                    if (tz != player.z)
                        continue;
                    if (radius >= 0 && chebyshev(tx, ty, player.x, player.y) > radius)
                        continue;
                }
            }
            if (!readTileItems(tile, l, snap.items))
                continue;
            out.push_back(std::move(snap));
            if (static_cast<int>(out.size()) >= kMaxTiles)
                return true;
        }
    }
    return !out.empty();
}

bool readMapTiles(const MapLayout& l, std::vector<TileSnapshot>& out) {
    out.clear();
    if (!l.present)
        return false;
    const PlayerSnapshot player = currentPlayer();
    if (!player.hasPosition || !plausiblePos(player.position.x, player.position.y, player.position.z))
        return false;
    const std::uintptr_t map = mapObject(l);
    if (!map)
        return false;
    const std::uintptr_t umap = floorUmap(map, l, player.position.z);
    if (!umap)
        return false;
    const int radius = l.radius > 0 ? l.radius : kDefaultRadius;
    return readFloorTiles(umap, l, player.position, radius, out);
}

json tileObject(const TileSnapshot& t) {
    json items = json::array();
    for (const auto& it : t.items)
        items.push_back({{"id", it.id}, {"count", it.count}});
    return {{"x", t.x}, {"y", t.y}, {"z", t.z}, {"items", items}};
}

bool heapFindTile(const Position& pos, DWORD t0, std::uintptr_t& tileOut) {
    tileOut = 0;
    MEMORY_BASIC_INFORMATION mbi{};
    std::uintptr_t addr = 0;
    const int ps = static_cast<int>(sizeof(void*));
    while (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi))) {
        if (GetTickCount() - t0 > kScanBudgetMs)
            break;
        const std::uintptr_t next =
            reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        const bool heapish = regionWritable(mbi) && mbi.Type == MEM_PRIVATE;
        if (heapish && mbi.RegionSize >= 64) {
            const auto base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
            const std::size_t n = mbi.RegionSize;
            for (std::size_t off = 0; off + 64 <= n; off += static_cast<std::size_t>(ps)) {
                if ((off & 0xFFF) == 0 && GetTickCount() - t0 > kScanBudgetMs)
                    break;
                const std::uintptr_t obj = base + off;
                if (!looksLikeVtable(obj) || !isTileObject(obj))
                    continue;
                MapLayout tmp;
                if (!discoverTilePosition(obj, pos, tmp))
                    continue;
                tileOut = obj;
                return true;
            }
        }
        if (next <= addr)
            break;
        addr = next;
    }
    return false;
}

bool verifyPlayerTile(const MapLayout& l, const Position& pos, const MapCalibHints& hints) {
    std::vector<TileSnapshot> tiles;
    MapLayout trial = l;
    trial.present = true;
    if (!readMapTiles(trial, tiles))
        return false;
    for (const auto& t : tiles) {
        if (t.x != pos.x || t.y != pos.y || t.z != pos.z)
            continue;
        if (t.items.empty() || t.items[0].id == 0)
            return false;
        if (hints.hasItemId) {
            bool ok = false;
            for (const auto& it : t.items) {
                if (it.id == hints.itemId)
                    ok = true;
            }
            if (!ok)
                return false;
        }
        return true;
    }
    return false;
}

void seedMapDefaults(MapLayout& l, const Profile& profile) {
    l.ptrSize = static_cast<int>(sizeof(void*));
    l.umapHeadOffset = 0;
    l.umapSizeOffset = l.ptrSize;
    l.listNextOffset = 0;
    l.listPrevOffset = l.ptrSize;
    l.mapHeadOffset = 0;
    l.mapSizeOffset = l.ptrSize;
    l.mapNodeLeftOffset = 0;
    l.mapNodeParentOffset = l.ptrSize;
    l.mapNodeRightOffset = l.ptrSize * 2;
    l.mapNodeIsnilOffset = l.ptrSize * 3 + 1;
    l.mapNodeKeyOffset = l.ptrSize * 4;
    l.floors = 16;
    l.maxZ = 15;
    l.radius = kDefaultRadius;
    l.thingsKind = "vector";
    if (profile.actions.mapRva)
        l.mapRva = profile.actions.mapRva;
    if (profile.containers.present) {
        const auto& c = profile.containers;
        l.mapHeadOffset = c.mapHeadOffset;
        l.mapSizeOffset = c.mapSizeOffset;
        l.mapNodeLeftOffset = c.mapNodeLeftOffset;
        l.mapNodeParentOffset = c.mapNodeParentOffset;
        l.mapNodeRightOffset = c.mapNodeRightOffset;
        l.mapNodeIsnilOffset = c.mapNodeIsnilOffset;
        if (c.mapNodeKeyOffset)
            l.mapNodeKeyOffset = c.mapNodeKeyOffset;
        if (c.ptrSize > 0)
            l.ptrSize = c.ptrSize;
    } else if (profile.creatures.present) {
        const auto& c = profile.creatures;
        l.mapHeadOffset = c.mapHeadOffset;
        l.mapSizeOffset = c.mapSizeOffset;
        l.mapNodeLeftOffset = c.mapNodeLeftOffset;
        l.mapNodeParentOffset = c.mapNodeParentOffset;
        l.mapNodeRightOffset = c.mapNodeRightOffset;
        l.mapNodeIsnilOffset = c.mapNodeIsnilOffset;
        if (c.mapNodeKeyOffset)
            l.mapNodeKeyOffset = c.mapNodeKeyOffset;
    }
    if (profile.inventory.present && profile.inventory.ptrSize > 0)
        l.tilePtrSize = profile.inventory.ptrSize;
    else if (profile.containers.present && profile.containers.itemPtrSize > 0)
        l.tilePtrSize = profile.containers.itemPtrSize;
    else
        l.tilePtrSize = 8;
    l.itemPtrSize = l.tilePtrSize;
}

} // namespace

void startMapReader() {
    Profile p;
    copyLiveProfile(p);
    std::lock_guard<std::mutex> lock(g_mtx);
    g_layout = p.map;
    if (g_layout.present && !g_layout.mapRva && p.actions.mapRva)
        g_layout.mapRva = p.actions.mapRva;
    if (g_layout.present)
        g_msg = "map layout loaded";
    else
        g_msg = "stand on the map and run p5-calibrate";
    if (g_layout.present) {
        LOG("map layout loaded collection=%s blocksOff=%d thingsOff=%d radius=%d",
            g_layout.collection.c_str(), g_layout.tileBlocksOffset, g_layout.thingsOffset,
            g_layout.radius);
    }
}

void setMapCalibHints(const MapCalibHints& hints) {
    std::lock_guard<std::mutex> lock(g_mtx);
    g_hints = hints;
    LOG("map hints itemId=%d", hints.itemId);
}

std::string mapCalibMessage() {
    std::lock_guard<std::mutex> lock(g_mtx);
    return g_msg;
}

bool runMapScan() {
    MapCalibHints hints;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        hints = g_hints;
        g_msg = "scanning for map tiles";
    }
    if (!isPlayerReady()) {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_msg = "P1 LocalPlayer not ready — log in on the map first";
        return false;
    }

    PlayerSnapshot player{};
    for (int i = 0; i < 15; ++i) {
        player = currentPlayer();
        if (player.hasPosition)
            break;
        Sleep(200);
    }
    if (!player.hasPosition ||
        !plausiblePos(player.position.x, player.position.y, player.position.z)) {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_msg = "need player position once for this .exe (p1-calibrate --refine --z)";
        return false;
    }

    Profile profile;
    copyLiveProfile(profile);
    if (!profile.actions.mapRva) {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_msg = "need actions.mapRva (P4) — use an action once so the profile stores g_map";
        return false;
    }

    MapLayout layout;
    seedMapDefaults(layout, profile);

    const DWORD t0 = GetTickCount();
    std::uintptr_t slot = 0;
    std::uintptr_t tile = 0;
    if (!tryGetTileNeedle(profile, player.position, slot, tile)) {
        LOG("map scan getTile miss, heap-scanning Tile objects");
        if (!heapFindTile(player.position, t0, tile) || !tile) {
            std::lock_guard<std::mutex> lock(g_mtx);
            g_msg = "no Tile object at the player position";
            return false;
        }
    }

    discoverTilePosition(tile, player.position, layout);
    if (!discoverThings(tile, hints, profile, layout)) {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_msg = hints.hasItemId ? "Tile found but item id was not on that tile"
                                : "Tile found but no item vector (ground) on the object";
        return false;
    }
    LOG("map Tile* %p thingsOff=%d posOff=%d items ok", reinterpret_cast<void*>(tile),
        layout.thingsOffset, layout.tilePositionOffset);

    const std::uintptr_t getTileSlot = slot;
    if (slot)
        logProbeSlot(slot, tile, player.position, ptrSizeOf(layout), tileStride(layout));

    bool derived = false;
    if (slot)
        derived = deriveBlockLayout(slot, tile, player.position, layout);
    if (!derived && slot)
        derived = deriveVectorLayouts(slot, tile, player.position, layout, t0);
    if (!derived) {
        std::vector<std::uintptr_t> hits;
        findPointersTo(tile, hits, t0);
        for (auto hit : hits) {
            if (hit == getTileSlot)
                continue;
            if (slotLooksLikeCopy(hit, tile, getTileSlot))
                continue;
            if (deriveBlockLayout(hit, tile, player.position, layout)) {
                slot = hit;
                derived = true;
                break;
            }
        }
    }
    if (!derived || !slot) {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_msg = "found Tile but not the sector TilePtr array";
        return false;
    }

    const std::uintptr_t node = nodeFromSlot(slot, player.position, layout, GetTickCount());
    if (!node) {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_msg = "TilePtr array found but not the map node";
        return false;
    }
    const bool tree = layout.collection.find("map-") == 0;
    std::uintptr_t col = 0;
    if (tree) {
        const std::uintptr_t sentinel = walkToSentinel(node, layout);
        if (!sentinel) {
            std::lock_guard<std::mutex> lock(g_mtx);
            g_msg = "TilePtr array found but not the std::map sentinel";
            return false;
        }
        col = findTreeMapObject(sentinel, layout, node, GetTickCount());
        if (!col) {
            std::lock_guard<std::mutex> lock(g_mtx);
            g_msg = "no std::map object for this floor's tile blocks";
            return false;
        }
    } else {
        UMapWalk walk;
        if (!walkUMapNodes(node, layout, walk) || !walk.sentinel) {
            std::lock_guard<std::mutex> lock(g_mtx);
            g_msg = "TilePtr array found but not the floor unordered_map list";
            return false;
        }
        col = findUMapObject(walk.sentinel, layout, GetTickCount());
        if (!col) {
            std::lock_guard<std::mutex> lock(g_mtx);
            g_msg = "no unordered_map object for this floor's tile blocks";
            return false;
        }
    }

    const std::uintptr_t map = moduleBase() + static_cast<std::uintptr_t>(
                                                 static_cast<std::intptr_t>(layout.mapRva));
    if (!placeBlocksOnMap(map, col, player.position.z, layout)) {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_msg = "tile collection found but it is not a member of g_map";
        return false;
    }

    layout.chain = {{layout.mapRva, false}};
    layout.present = true;

    if (!verifyPlayerTile(layout, player.position, hints)) {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_msg = "map layout did not read the ground under the player";
        return false;
    }

    setMapLayout(layout);
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_layout = layout;
        g_msg = "map-chain resolved";
    }
    LOG("map-chain resolved mapRva=%d collection=%s blocksOff=%d stride=%d bs=%d thingsOff=%d",
        layout.mapRva, layout.collection.c_str(), layout.tileBlocksOffset, layout.umapStride,
        layout.blockSize, layout.thingsOffset);
    return true;
}

std::string mapJson() {
    MapLayout layout;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        layout = g_layout;
    }
    std::vector<TileSnapshot> tiles;
    if (layout.present)
        readMapTiles(layout, tiles);
    json arr = json::array();
    for (const auto& t : tiles)
        arr.push_back(tileObject(t));
    return arr.dump();
}

std::vector<TileSnapshot> currentMapTiles() {
    MapLayout layout;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        layout = g_layout;
    }
    std::vector<TileSnapshot> tiles;
    if (layout.present)
        readMapTiles(layout, tiles);
    return tiles;
}

} // namespace otc

