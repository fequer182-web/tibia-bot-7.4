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
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace otc {
namespace {

using nlohmann::json;

constexpr DWORD kPollMs = 250;
constexpr DWORD kScanBudgetMs = 20000;
constexpr int kMaxCreatures = 256;
constexpr int kScreenRange = 12;
constexpr int kCalibRange = 24;
constexpr int kViewportRangeX = 8;
constexpr int kViewportRangeY = 6;
constexpr int kMaxNameBack = 768;
constexpr int kNearScanRange = 2048;

std::mutex g_mtx;
CreatureLayout g_layout;
CreatureCalibHints g_hints;
std::vector<CreatureSnapshot> g_live;
std::string g_msg = "creatures not calibrated";
HANDLE g_pollThread = nullptr;
bool g_pollRun = false;

int ptrSizeOf(const CreatureLayout& l) {
    return l.ptrSize > 0 ? l.ptrSize : static_cast<int>(sizeof(void*));
}

int creatureStride(const CreatureLayout& l) {
    if (l.creaturePtrSize > 0)
        return l.creaturePtrSize;
    return ptrSizeOf(l);
}

int nameBytes() {
    return static_cast<int>(sizeof(void*)) == 4 ? 24 : 32;
}

bool regionWritable(const MEMORY_BASIC_INFORMATION& mbi) {
    if (mbi.State != MEM_COMMIT)
        return false;
    if (memPageGuarded(mbi.Protect))
        return false;
    const DWORD prot = mbi.Protect & 0xFF;
    return prot == PAGE_READWRITE || prot == PAGE_EXECUTE_READWRITE ||
           prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_WRITECOPY;
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
    if (p < 0x10000)
        return false;
    const std::uintptr_t mod = moduleBase();
    const std::size_t img = moduleImageSize();
    if (img && p >= mod && p < mod + img)
        return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(p), &mbi, sizeof(mbi)))
        return false;
    if (mbi.State != MEM_COMMIT)
        return false;
    if (memPageGuarded(mbi.Protect))
        return false;
    return mbi.Type == MEM_PRIVATE || mbi.Type == MEM_MAPPED;
}

bool looksLikeCreatureObject(std::uintptr_t p) {
    return p != 0 && looksLikeHeapObject(p) && looksLikeVtable(p);
}

bool nameEq(const std::string& a, const std::string& b) {
    return a.size() == b.size() && _stricmp(a.c_str(), b.c_str()) == 0;
}

bool nameMatches(const std::string& got, const std::string& expected) {
    if (nameEq(got, expected))
        return true;
    if (expected.size() < 3 || got.size() < expected.size())
        return false;
    for (std::size_t i = 0; i + expected.size() <= got.size(); ++i) {
        bool ok = true;
        for (std::size_t j = 0; j < expected.size(); ++j) {
            const unsigned char ca = static_cast<unsigned char>(got[i + j]);
            const unsigned char cb = static_cast<unsigned char>(expected[j]);
            if (std::tolower(ca) != std::tolower(cb)) {
                ok = false;
                break;
            }
        }
        if (ok)
            return true;
    }
    return false;
}

bool printableName(const std::string& s) {
    if (s.size() < 2 || s.size() > 48)
        return false;
    for (unsigned char c : s) {
        if (c < 32 || c > 126)
            return false;
    }
    return true;
}

bool overlapsName(int off, int size, int nameOff) {
    if (nameOff < 0)
        return false;
    const int hi = nameOff + nameBytes();
    return off < hi && off + size > nameOff;
}

constexpr int iabs(int v) {
    return v < 0 ? -v : v;
}

// Internal calibration sentinel only; zero remains a valid world coordinate.
constexpr Position unknownPlayerPosition(int z = 0) {
    return Position{-1, -1, z};
}

constexpr bool hasPlayerXY(const Position& p) {
    return p.x >= 0 && p.y >= 0;
}

constexpr bool nearPlayer(int x, int y, int z, const Position& p, int range = kScreenRange,
                bool requireKnownZ = false) {
    if (!hasPlayerXY(p))
        return !requireKnownZ || z == p.z;
    if (z != p.z)
        return false;
    return iabs(x - p.x) <= range && iabs(y - p.y) <= range;
}

// Regression checks for --z, name-only fallback and legitimate zero coordinates.
static_assert(nearPlayer(32105, 32204, 7, unknownPlayerPosition(7), 12, true));
static_assert(!nearPlayer(32105, 32204, 8, unknownPlayerPosition(7), 12, true));
static_assert(nearPlayer(32105, 32204, 8, unknownPlayerPosition(), 12, false));
static_assert(nearPlayer(32106, 32204, 7, Position{32105, 32204, 7}, 12));
static_assert(!nearPlayer(32118, 32204, 7, Position{32105, 32204, 7}, 12));
static_assert(nearPlayer(0, 1, 7, Position{0, 0, 7}, 12));
static_assert(!nearPlayer(32105, 32204, 7, Position{0, 0, 7}, 12));
static_assert(!nearPlayer(0, 1, 8, Position{0, 0, 7}, 12));

bool plausiblePos(int x, int y, int z) {
    // 0xffff is OTClient's invalid/sentinel coordinate. Zero remains valid on
    // custom maps, so reject only the sentinel instead of imposing map bounds.
    return z >= 0 && z <= 15 && x >= 0 && y >= 0 && x < 65535 && y < 65535;
}

bool inModule(std::uintptr_t p) {
    const std::uintptr_t mod = moduleBase();
    const std::size_t img = moduleImageSize();
    return img && p >= mod && p < mod + img;
}

bool readMsvcString(std::uintptr_t addr, std::string& out) {
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
    if (size > 64 || cap > 4096 || size > cap)
        return false;
    char buf[65]{};
    if (cap < 16) {
        if (size > 0 && !memRead(reinterpret_cast<const void*>(addr), buf, size))
            return false;
    } else {
        std::uintptr_t p = 0;
        if (!readPtr(addr, p) || !p)
            return false;
        if (size > 0 && !memRead(reinterpret_cast<const void*>(p), buf, size))
            return false;
    }
    out.assign(buf, size);
    return printableName(out) || out.empty();
}

bool readGccString(std::uintptr_t addr, std::string& out) {
    const int ps = static_cast<int>(sizeof(void*));
    std::uintptr_t p = 0;
    std::size_t len = 0;
    if (!readPtr(addr, p) || !p)
        return false;
    if (ps == 4) {
        std::uint32_t n = 0;
        if (!memReadT(addr + 4, n))
            return false;
        len = n;
    } else {
        if (!memReadT(addr + 8, len))
            return false;
    }
    if (len > 64)
        return false;
    char buf[65]{};
    if (len > 0 && !memRead(reinterpret_cast<const void*>(p), buf, len))
        return false;
    out.assign(buf, len);
    return printableName(out) || out.empty();
}

bool readCString(std::uintptr_t addr, std::string& out) {
    char buf[49]{};
    if (!memRead(reinterpret_cast<const void*>(addr), buf, sizeof(buf) - 1))
        return false;
    buf[sizeof(buf) - 1] = 0;
    out = buf;
    return printableName(out);
}

bool readCreatureName(std::uintptr_t addr, const CreatureLayout& l, std::string& out) {
    if (l.nameKind == "gcc")
        return readGccString(addr, out);
    if (l.nameKind == "cstr")
        return readCString(addr, out);
    return readMsvcString(addr, out);
}

bool ciEqualBytes(const char* a, const std::string& b) {
    for (std::size_t i = 0; i < b.size(); ++i) {
        const unsigned char ca = static_cast<unsigned char>(a[i]);
        const unsigned char cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb))
            return false;
    }
    return true;
}

bool interpretNameHit(std::uintptr_t hit, const std::string& expected, std::uintptr_t& strObj,
                      std::string& kind) {
    std::string got;
    if (readMsvcString(hit, got) && nameMatches(got, expected)) {
        strObj = hit;
        kind = "msvc";
        return true;
    }
    const int ps = static_cast<int>(sizeof(void*));
    if (hit >= static_cast<std::uintptr_t>(ps * 2)) {
        const std::uintptr_t gccObj = hit - static_cast<std::uintptr_t>(ps * 2);
        std::uintptr_t p = 0;
        if (readPtr(gccObj, p) && p == hit && readGccString(gccObj, got) &&
            nameMatches(got, expected)) {
            strObj = gccObj;
            kind = "gcc";
            return true;
        }
    }
    for (int back = ps; back <= 48; back += ps) {
        if (hit < static_cast<std::uintptr_t>(back))
            break;
        const std::uintptr_t s = hit - static_cast<std::uintptr_t>(back);
        std::uintptr_t p = 0;
        if (!readPtr(s, p) || p != hit)
            continue;
        if (readMsvcString(s, got) && nameMatches(got, expected)) {
            strObj = s;
            kind = "msvc";
            return true;
        }
        if (readGccString(s, got) && nameMatches(got, expected)) {
            strObj = s;
            kind = "gcc";
            return true;
        }
    }
    char z = 1;
    if (memReadT(hit + expected.size(), z) && z == 0 && readCString(hit, got) &&
        nameMatches(got, expected)) {
        strObj = hit;
        kind = "cstr";
        return true;
    }
    return false;
}

std::uintptr_t resolveChain(const std::vector<ChainStep>& chain) {
    if (chain.empty())
        return 0;
    std::uintptr_t cur = moduleBase();
    for (const auto& step : chain) {
        cur += static_cast<std::uintptr_t>(static_cast<std::intptr_t>(step.offset));
        if (step.deref) {
            std::uintptr_t next = 0;
            if (!readPtr(cur, next) || next == 0)
                return 0;
            cur = next;
        }
    }
    return cur;
}

bool readPackedPos(std::uintptr_t addr, int& x, int& y, int& z) {
    std::uint16_t px = 0;
    std::uint16_t py = 0;
    std::uint8_t pz = 0;
    if (!memReadT(addr, px) || !memReadT(addr + 2, py) || !memReadT(addr + 4, pz))
        return false;
    x = px;
    y = py;
    z = pz;
    return true;
}

bool readCreaturePos(std::uintptr_t obj, const CreatureLayout& l, int& x, int& y, int& z) {
    const std::uintptr_t addr = obj + static_cast<std::uintptr_t>(l.positionOffset);
    if (l.positionPacked)
        return readPackedPos(addr, x, y, z);
    std::int32_t xyz[3]{};
    if (!memRead(reinterpret_cast<const void*>(addr), xyz, sizeof(xyz)))
        return false;
    x = xyz[0];
    y = xyz[1];
    z = xyz[2];
    return true;
}

bool readLivePlayerTile(Position& out) {
    const PlayerSnapshot p = currentPlayer();
    if (plausiblePos(p.position.x, p.position.y, p.position.z)) {
        out = p.position;
        return true;
    }
    Profile prof;
    copyLiveProfile(prof);
    const std::uintptr_t base = currentStatsBase();
    if (!base || !prof.hasPosition)
        return false;
    const std::uintptr_t addr =
        base + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(prof.positionOffset));
    if (prof.positionPacked) {
        if (!readPackedPos(addr, out.x, out.y, out.z))
            return false;
    } else {
        std::int32_t xyz[3]{};
        if (!memRead(reinterpret_cast<const void*>(addr), xyz, sizeof(xyz)))
            return false;
        out.x = xyz[0];
        out.y = xyz[1];
        out.z = xyz[2];
    }
    return plausiblePos(out.x, out.y, out.z);
}

int readHpperc(std::uintptr_t obj, const CreatureLayout& l) {
    const std::uintptr_t addr = obj + static_cast<std::uintptr_t>(l.hppercOffset);
    if (l.hppercSize == 4) {
        std::int32_t v = 0;
        if (!memReadT(addr, v))
            return -1;
        return v;
    }
    std::uint8_t v = 0;
    if (!memReadT(addr, v))
        return -1;
    return v;
}

std::uint32_t readId(std::uintptr_t obj, const CreatureLayout& l) {
    std::uint32_t v = 0;
    memReadT(obj + static_cast<std::uintptr_t>(l.idOffset), v);
    return v;
}

bool looksLikeHeapPointerId(std::uint32_t id) {
    return id >= 0x10000u && id < 0x40000000u &&
           looksLikeHeapObject(static_cast<std::uintptr_t>(id));
}

bool looksLikeCreatureId(std::uint32_t id) {
    if (id == 0)
        return false;
    if (looksLikeVtable(static_cast<std::uintptr_t>(id)))
        return false;
    if (looksLikeHeapPointerId(id))
        return false;
    return true;
}

const char* kindFromId(std::uint32_t id) {
    // OTClient/TFS ranges first — 0x10000000+ overlaps user-space heap on Win32.
    if (id >= 0x80000000u)
        return "npc";
    if (id >= 0x40000000u)
        return "monster";
    if (id >= 0x10000000u)
        return "player";
    if (looksLikeHeapPointerId(id))
        return "unknown";
    return "player";
}

const char* kindFromField(int k) {
    switch (k) {
    case 0:
        return "player";
    case 1:
        return "monster";
    case 2:
        return "npc";
    case 3:
    case 4:
        return "monster";
    default:
        return "unknown";
    }
}

bool parseVectorPtrs(std::uintptr_t addr, int ps, int elemSize, int minN, int maxN,
                     std::vector<std::uintptr_t>& out) {
    if (elemSize < 4)
        elemSize = ps;
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
    std::uintptr_t cap = 0;
    if (!readPtr(addr, begin) || !readPtr(addr + ps, end) || !readPtr(addr + ps * 2, cap))
        return false;
    if (end < begin || cap < end)
        return false;
    const std::size_t bytes = static_cast<std::size_t>(end - begin);
    if (bytes % static_cast<std::size_t>(elemSize) != 0)
        return false;
    const int n = static_cast<int>(bytes / static_cast<std::size_t>(elemSize));
    if (n < minN || n > maxN)
        return false;
    if (n == 0) {
        out.clear();
        return minN <= 0;
    }
    if (!looksLikeHeapObject(begin))
        return false;
    out.clear();
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        std::uintptr_t p = 0;
        if (!readPtr(begin + static_cast<std::uintptr_t>(i * elemSize), p))
            return false;
        out.push_back(p);
    }
    return true;
}

bool nodeIsNil(std::uintptr_t node, const CreatureLayout& l) {
    std::uint8_t v = 0;
    if (!memReadT(node + static_cast<std::uintptr_t>(l.mapNodeIsnilOffset), v))
        return true;
    return v != 0;
}

std::uintptr_t nodePtr(std::uintptr_t node, std::int32_t off) {
    std::uintptr_t p = 0;
    readPtr(node + static_cast<std::uintptr_t>(off), p);
    return p;
}

bool probeMapNode(std::uintptr_t valueAddr, std::uintptr_t creature, std::uint32_t expectId,
                  CreatureLayout& l) {
    const int ps = static_cast<int>(sizeof(void*));
    const int ptrOffs[] = {ps * 5, ps * 4 + 4, ps * 5 + 4, 20, 24, 16, 32, 40, 48, 28, 36};
    const int isnilOffs[] = {ps * 3 + 1, ps * 3, ps * 4 + 1, 13, 25};
    for (int ptrOff : ptrOffs) {
        if (ptrOff < ps * 3)
            continue;
        const std::uintptr_t node = valueAddr - static_cast<std::uintptr_t>(ptrOff);
        if ((node % static_cast<std::uintptr_t>(ps)) != 0)
            continue;
        std::uintptr_t stored = 0;
        if (!readPtr(node + static_cast<std::uintptr_t>(ptrOff), stored) || stored != creature)
            continue;
        const std::uintptr_t left = nodePtr(node, 0);
        const std::uintptr_t parent = nodePtr(node, ps);
        const std::uintptr_t right = nodePtr(node, ps * 2);
        if (parent == 0)
            continue;
        if (left && !looksLikeHeapObject(left))
            continue;
        if (!looksLikeHeapObject(parent))
            continue;
        if (right && !looksLikeHeapObject(right))
            continue;
        for (int isnilOff : isnilOffs) {
            std::uint8_t isnil = 1;
            if (!memReadT(node + static_cast<std::uintptr_t>(isnilOff), isnil) || isnil != 0)
                continue;
            const int keyTry[] = {ptrOff - 8, ptrOff - 4, ptrOff - ps};
            int bestKey = -1;
            int bestKeyScore = -1;
            for (int keyOff : keyTry) {
                if (keyOff < ps * 3)
                    continue;
                std::uint32_t key = 0;
                if (!memReadT(node + static_cast<std::uintptr_t>(keyOff), key) || key == 0)
                    continue;
                if (expectId && key != expectId)
                    continue;
                if (looksLikeHeapPointerId(key))
                    continue;
                int ks = 10;
                if (looksLikeCreatureId(key))
                    ks += 50;
                if (key >= 0x40000000u && key < 0x80000000u)
                    ks += 40;
                if (expectId && key == expectId)
                    ks += 200;
                if (ks > bestKeyScore) {
                    bestKeyScore = ks;
                    bestKey = keyOff;
                }
            }
            if (bestKey < 0)
                continue;
            l.mapNodeLeftOffset = 0;
            l.mapNodeParentOffset = ps;
            l.mapNodeRightOffset = ps * 2;
            l.mapNodeIsnilOffset = isnilOff;
            l.mapNodePtrOffset = ptrOff;
            l.mapNodeKeyOffset = bestKey;
            return true;
        }
    }
    return false;
}

std::uintptr_t walkToSentinel(std::uintptr_t node, const CreatureLayout& l) {
    for (int i = 0; i < 64; ++i) {
        if (!node)
            return 0;
        if (nodeIsNil(node, l))
            return node;
        const std::uintptr_t parent = nodePtr(node, l.mapNodeParentOffset);
        if (!parent || parent == node)
            return 0;
        node = parent;
    }
    return 0;
}

void walkMapNodes(std::uintptr_t node, const CreatureLayout& l, std::set<std::uintptr_t>& seen,
                  std::vector<std::pair<std::uint32_t, std::uintptr_t>>& out) {
    if (!node || seen.size() > static_cast<std::size_t>(kMaxCreatures) + 8 || seen.count(node))
        return;
    seen.insert(node);
    if (nodeIsNil(node, l))
        return;
    walkMapNodes(nodePtr(node, l.mapNodeLeftOffset), l, seen, out);
    std::uint32_t key = 0;
    memReadT(node + static_cast<std::uintptr_t>(l.mapNodeKeyOffset), key);
    std::uintptr_t p = 0;
    readPtr(node + static_cast<std::uintptr_t>(l.mapNodePtrOffset), p);
    if (p && key)
        out.push_back({key, p});
    walkMapNodes(nodePtr(node, l.mapNodeRightOffset), l, seen, out);
}

bool walkMap(std::uintptr_t mapObj, const CreatureLayout& l,
             std::vector<std::pair<std::uint32_t, std::uintptr_t>>& out) {
    std::uintptr_t head = 0;
    if (!readPtr(mapObj + static_cast<std::uintptr_t>(l.mapHeadOffset), head) || !head)
        return false;
    const std::uintptr_t root = nodePtr(head, l.mapNodeParentOffset);
    std::set<std::uintptr_t> seen;
    out.clear();
    walkMapNodes(root, l, seen, out);
    return true;
}

bool probeUMapNode(std::uintptr_t valueAddr, std::uintptr_t creature, std::uint32_t expectId,
                   CreatureLayout& l) {
    const int ps = static_cast<int>(sizeof(void*));
    const int ptrOffs[] = {ps * 2, ps * 2 + 4, ps * 3, ps * 4, 12, 16, 20, 24, 32};
    for (int ptrOff : ptrOffs) {
        if (ptrOff < ps * 2)
            continue;
        const std::uintptr_t node = valueAddr - static_cast<std::uintptr_t>(ptrOff);
        if ((node % static_cast<std::uintptr_t>(ps)) != 0)
            continue;
        std::uintptr_t stored = 0;
        if (!readPtr(node + static_cast<std::uintptr_t>(ptrOff), stored) || stored != creature)
            continue;
        std::uintptr_t next = 0;
        std::uintptr_t prev = 0;
        if (!readPtr(node, next) || !readPtr(node + static_cast<std::uintptr_t>(ps), prev))
            continue;
        if (!next || !prev || !looksLikeHeapObject(next) || !looksLikeHeapObject(prev))
            continue;
        std::uintptr_t nextPrev = 0;
        std::uintptr_t prevNext = 0;
        readPtr(next + static_cast<std::uintptr_t>(ps), nextPrev);
        readPtr(prev, prevNext);
        if (nextPrev != node && prevNext != node)
            continue;
        const int keyTry[] = {ptrOff - 4, ptrOff - 8, ps * 2};
        for (int keyOff : keyTry) {
            if (keyOff < ps * 2)
                continue;
            std::uint32_t key = 0;
            if (!memReadT(node + static_cast<std::uintptr_t>(keyOff), key) || key == 0)
                continue;
            if (expectId && key != expectId)
                continue;
            l.listNextOffset = 0;
            l.listPrevOffset = ps;
            l.listPtrOffset = ptrOff;
            l.listKeyOffset = keyOff;
            return true;
        }
    }
    return false;
}

bool walkUMapList(std::uintptr_t startNode, const CreatureLayout& l,
                  std::vector<std::pair<std::uint32_t, std::uintptr_t>>& out,
                  std::uintptr_t& sentinelOut) {
    out.clear();
    sentinelOut = 0;
    if (!startNode)
        return false;
    std::set<std::uintptr_t> seen;
    std::uintptr_t node = startNode;
    std::vector<std::uintptr_t> ring;
    for (int i = 0; i < kMaxCreatures + 4; ++i) {
        if (!node || seen.count(node))
            break;
        seen.insert(node);
        ring.push_back(node);
        node = nodePtr(node, l.listNextOffset);
    }
    if (ring.size() < 2)
        return false;
    std::vector<std::pair<std::uint32_t, std::uintptr_t>> data;
    std::uintptr_t sentinel = 0;
    for (auto n : ring) {
        std::uintptr_t p = 0;
        readPtr(n + static_cast<std::uintptr_t>(l.listPtrOffset), p);
        std::uint32_t key = 0;
        memReadT(n + static_cast<std::uintptr_t>(l.listKeyOffset), key);
        if (p && looksLikeCreatureObject(p) && key) {
            data.push_back({key, p});
        } else if (!sentinel) {
            sentinel = n;
        }
    }
    if (data.empty())
        return false;
    out = std::move(data);
    sentinelOut = sentinel ? sentinel : startNode;
    return true;
}

bool walkUMap(std::uintptr_t mapObj, const CreatureLayout& l,
              std::vector<std::pair<std::uint32_t, std::uintptr_t>>& out) {
    std::uintptr_t head = 0;
    if (!readPtr(mapObj + static_cast<std::uintptr_t>(l.umapHeadOffset), head) || !head)
        return false;
    const std::uintptr_t first = nodePtr(head, l.listNextOffset);
    std::uintptr_t sentinel = 0;
    if (!walkUMapList(first && first != head ? first : head, l, out, sentinel))
        return false;
    return true;
}

bool collectCreaturePtrs(std::uintptr_t base, const CreatureLayout& l,
                         std::vector<std::uintptr_t>& objs) {
    objs.clear();
    if (!base)
        return false;
    const int ps = ptrSizeOf(l);
    const int stride = creatureStride(l);
    if (l.collection == "vector") {
        std::vector<std::uintptr_t> ptrs;
        if (!parseVectorPtrs(base, ps, stride, 0, kMaxCreatures, ptrs))
            return false;
        for (auto p : ptrs) {
            if (p && looksLikeCreatureObject(p))
                objs.push_back(p);
        }
        return !objs.empty();
    }
    std::vector<std::pair<std::uint32_t, std::uintptr_t>> nodes;
    const bool walked = l.collection == "umap" ? walkUMap(base, l, nodes) : walkMap(base, l, nodes);
    if (!walked)
        return false;
    for (const auto& n : nodes) {
        if (n.second && looksLikeCreatureObject(n.second))
            objs.push_back(n.second);
    }
    return !objs.empty();
}

bool readSharedSlot(std::uintptr_t slot, int elemSize, std::uintptr_t& ptr, std::uintptr_t& rep) {
    ptr = 0;
    rep = 0;
    if (!slot || !readPtr(slot, ptr) || !ptr)
        return false;
    if (elemSize >= static_cast<int>(sizeof(void*) * 2))
        readPtr(slot + sizeof(void*), rep);
    return true;
}

struct CreatureSp {
    std::uint32_t id = 0;
    std::uintptr_t ptr = 0;
    std::uintptr_t rep = 0;
};

void walkMapSp(std::uintptr_t node, const CreatureLayout& l, std::set<std::uintptr_t>& seen,
               std::vector<CreatureSp>& out) {
    if (!node || seen.size() > static_cast<std::size_t>(kMaxCreatures) + 8 || seen.count(node))
        return;
    seen.insert(node);
    if (nodeIsNil(node, l))
        return;
    walkMapSp(nodePtr(node, l.mapNodeLeftOffset), l, seen, out);
    CreatureSp h;
    memReadT(node + static_cast<std::uintptr_t>(l.mapNodeKeyOffset), h.id);
    if (readSharedSlot(node + static_cast<std::uintptr_t>(l.mapNodePtrOffset), creatureStride(l),
                       h.ptr, h.rep) &&
        h.ptr && h.id)
        out.push_back(h);
    walkMapSp(nodePtr(node, l.mapNodeRightOffset), l, seen, out);
}

bool collectCreatureSp(std::uintptr_t base, const CreatureLayout& l, std::vector<CreatureSp>& out) {
    out.clear();
    if (!base)
        return false;
    const int ps = ptrSizeOf(l);
    const int es = creatureStride(l);
    if (l.collection == "vector") {
        std::uintptr_t begin = 0;
        std::uintptr_t end = 0;
        if (!readPtr(base, begin) || !readPtr(base + ps, end) || end < begin || es < 4)
            return false;
        const std::size_t bytes = static_cast<std::size_t>(end - begin);
        if (bytes % static_cast<std::size_t>(es) != 0)
            return false;
        const int n = static_cast<int>(bytes / static_cast<std::size_t>(es));
        if (n < 0 || n > kMaxCreatures)
            return false;
        for (int i = 0; i < n; ++i) {
            CreatureSp h;
            if (!readSharedSlot(begin + static_cast<std::uintptr_t>(i * es), es, h.ptr, h.rep))
                continue;
            h.id = readId(h.ptr, l);
            if (!h.id)
                continue;
            out.push_back(h);
        }
        return !out.empty();
    }
    if (l.collection == "umap") {
        std::uintptr_t head = 0;
        if (!readPtr(base + static_cast<std::uintptr_t>(l.umapHeadOffset), head) || !head)
            return false;
        const std::uintptr_t first = nodePtr(head, l.listNextOffset);
        std::uintptr_t node = first && first != head ? first : head;
        std::set<std::uintptr_t> seen;
        for (int i = 0; i < kMaxCreatures + 4; ++i) {
            if (!node || seen.count(node))
                break;
            seen.insert(node);
            CreatureSp h;
            memReadT(node + static_cast<std::uintptr_t>(l.listKeyOffset), h.id);
            if (readSharedSlot(node + static_cast<std::uintptr_t>(l.listPtrOffset), es, h.ptr,
                               h.rep) &&
                h.ptr && h.id)
                out.push_back(h);
            node = nodePtr(node, l.listNextOffset);
        }
        return !out.empty();
    }
    std::uintptr_t head = 0;
    if (!readPtr(base + static_cast<std::uintptr_t>(l.mapHeadOffset), head) || !head)
        return false;
    const std::uintptr_t root = nodePtr(head, l.mapNodeParentOffset);
    std::set<std::uintptr_t> seen;
    walkMapSp(root, l, seen, out);
    return !out.empty();
}

bool matchCreatureSp(const CreatureLayout& l, const CreatureSp& h, std::uint32_t id, int x, int y,
                     int z, bool byId, bool byPos) {
    if (!h.ptr)
        return false;
    if (byId) {
        if (h.id == id)
            return true;
        return readId(h.ptr, l) == id;
    }
    if (!byPos)
        return false;
    int cx = 0, cy = 0, cz = 0;
    if (!readCreaturePos(h.ptr, l, cx, cy, cz))
        return false;
    return cx == x && cy == y && cz == z;
}

std::vector<std::uintptr_t> objectStartsFromName(std::uintptr_t nameAddr) {
    std::vector<std::uintptr_t> found;
    const int ps = static_cast<int>(sizeof(void*));
    for (int back = ps; back <= 400; back += ps) {
        if (nameAddr < static_cast<std::uintptr_t>(back))
            break;
        const std::uintptr_t a = nameAddr - static_cast<std::uintptr_t>(back);
        if (looksLikeVtable(a) && looksLikeHeapObject(a))
            found.push_back(a);
    }
    return found;
}

bool discoverPosition(std::uintptr_t obj, int nameOff, const Position& playerPos,
                      const Profile& profile, std::uintptr_t playerObj, std::uintptr_t statsBase,
                      CreatureLayout& l, bool requirePlayerMirror = false,
                      bool requireKnownZ = false) {
    (void)profile;
    (void)statsBase;
    for (int off = 4; off <= 160; off += 2) {
        if (overlapsName(off, 6, nameOff))
            continue;
        int x = 0, y = 0, z = 0;
        if (!readPackedPos(obj + static_cast<std::uintptr_t>(off), x, y, z))
            continue;
        if (!plausiblePos(x, y, z) ||
            !nearPlayer(x, y, z, playerPos, kCalibRange, requireKnownZ))
            continue;
        if (requirePlayerMirror && playerObj) {
            int px = 0, py = 0, pz = 0;
            if (!readPackedPos(playerObj + static_cast<std::uintptr_t>(off), px, py, pz) ||
                !plausiblePos(px, py, pz))
                continue;
            if (requireKnownZ) {
                if (pz != playerPos.z)
                    continue;
            } else if (hasPlayerXY(playerPos) &&
                       (px != playerPos.x || py != playerPos.y || pz != playerPos.z)) {
                continue;
            }
        }
        l.positionOffset = off;
        l.positionPacked = true;
        return true;
    }
    for (int off = 4; off <= 160; off += 4) {
        if (overlapsName(off, 12, nameOff))
            continue;
        std::int32_t xyz[3]{};
        if (!memRead(reinterpret_cast<const void*>(obj + static_cast<std::uintptr_t>(off)), xyz,
                     sizeof(xyz)))
            continue;
        if (!plausiblePos(xyz[0], xyz[1], xyz[2]) ||
            !nearPlayer(xyz[0], xyz[1], xyz[2], playerPos, kCalibRange, requireKnownZ))
            continue;
        if (requirePlayerMirror && playerObj) {
            std::int32_t pxyz[3]{};
            if (!memRead(reinterpret_cast<const void*>(playerObj +
                                                       static_cast<std::uintptr_t>(off)),
                         pxyz, sizeof(pxyz)) ||
                !plausiblePos(pxyz[0], pxyz[1], pxyz[2]))
                continue;
            if (requireKnownZ) {
                if (pxyz[2] != playerPos.z)
                    continue;
            } else if (hasPlayerXY(playerPos) &&
                       (pxyz[0] != playerPos.x || pxyz[1] != playerPos.y ||
                        pxyz[2] != playerPos.z)) {
                continue;
            }
        }
        l.positionOffset = off;
        l.positionPacked = false;
        return true;
    }
    return false;
}

bool discoverId(std::uintptr_t obj, int nameOff, const CreatureCalibHints& hints,
                std::uintptr_t playerObj, CreatureLayout& l) {
    int best = -1;
    int bestScore = -1;
    const int beforeName = nameOff - 4;
    int playerHpPercent = -1;
    if (playerObj && hints.hasHpperc) {
        const PlayerSnapshot p = currentPlayer();
        if (p.maxHealth > 0)
            playerHpPercent =
                static_cast<int>((static_cast<long long>(p.health) * 100) / p.maxHealth);
    }
    for (int off = 16; off <= 320; off += 4) {
        if (overlapsName(off, 4, nameOff))
            continue;
        if (off >= l.positionOffset && off < l.positionOffset + 12)
            continue;
        std::uint32_t mid = 0;
        if (!memReadT(obj + static_cast<std::uintptr_t>(off), mid) || !looksLikeCreatureId(mid))
            continue;
        if (hints.hasId && mid != hints.id)
            continue;
        if (!hints.hasId && hints.hasHpperc && mid == static_cast<std::uint32_t>(hints.hpperc) &&
            playerObj && playerHpPercent >= 0) {
            std::int32_t playerValue = -1;
            if (memReadT(playerObj + static_cast<std::uintptr_t>(off), playerValue) &&
                playerValue == playerHpPercent)
                continue;
        }
        int score = 10;
        if (hints.hasId && mid == hints.id)
            score += 200;
        if (off == beforeName)
            score += 180;
        if (mid >= 0x40000000u && mid < 0x80000000u)
            score += 40;
        if (playerObj) {
            std::uint32_t pid = 0;
            if (!memReadT(playerObj + static_cast<std::uintptr_t>(off), pid) || pid == 0 ||
                pid == mid)
                continue;
            if (looksLikeHeapPointerId(pid))
                continue;
            score += 80;
            if (pid >= 0x10000000u && pid < 0x40000000u)
                score += 40;
            else if (pid < 0x10000000u)
                score += 20;
        }
        if (score > bestScore) {
            bestScore = score;
            best = off;
        }
    }
    if (best < 0)
        return false;
    l.idOffset = best;
    l.idSize = 4;
    return true;
}

bool discoverHpperc(std::uintptr_t obj, int nameOff, const CreatureCalibHints& hints,
                    std::uintptr_t playerObj, CreatureLayout& l) {
    int bestOff = -1;
    int bestSize = 1;
    int bestScore = -1;
    int expectPlayer = -1;
    if (playerObj) {
        const PlayerSnapshot p = currentPlayer();
        if (p.maxHealth > 0)
            expectPlayer = static_cast<int>((static_cast<long long>(p.health) * 100) / p.maxHealth);
    }
    for (int off = 24; off <= 480; ++off) {
        if (overlapsName(off, 4, nameOff))
            continue;
        if (off >= l.positionOffset && off < l.positionOffset + 12)
            continue;
        if (off >= l.idOffset && off < l.idOffset + 4)
            continue;
        auto consider = [&](int size, int v, int extra) {
            if (v < 0 || v > 100)
                return;
            if (hints.hasHpperc && v != hints.hpperc)
                return;
            if (playerObj) {
                int pv = -1;
                if (size == 4) {
                    std::int32_t t = 0;
                    if (!memReadT(playerObj + static_cast<std::uintptr_t>(off), t))
                        return;
                    pv = t;
                } else {
                    std::uint8_t t = 0;
                    if (!memReadT(playerObj + static_cast<std::uintptr_t>(off), t))
                        return;
                    pv = t;
                }
                if (pv < 0 || pv > 100)
                    return;
                extra += 40;
                if (expectPlayer >= 0) {
                    if (pv == expectPlayer)
                        extra += 90;
                    else if (expectPlayer >= 95 && pv < 50)
                        return;
                }
            }
            int score = extra + (hints.hasHpperc ? 80 : 0) + (size == 4 ? 10 : 0);
            if (off >= nameOff + nameBytes())
                score += 50;
            const int dist = iabs(off - l.idOffset);
            if (dist < 64)
                score += 10;
            if (score > bestScore) {
                bestScore = score;
                bestOff = off;
                bestSize = size;
            }
        };
        if ((off % 4) == 0) {
            std::int32_t v = 0;
            if (memReadT(obj + static_cast<std::uintptr_t>(off), v))
                consider(4, v, 15);
        }
        std::uint8_t u = 0;
        if (memReadT(obj + static_cast<std::uintptr_t>(off), u))
            consider(1, u, 0);
    }
    if (bestOff < 0)
        return false;
    l.hppercOffset = bestOff;
    l.hppercSize = bestSize;
    return true;
}

void discoverKindSkull(std::uintptr_t obj, int nameOff, std::uintptr_t playerObj, CreatureLayout& l) {
    l.kindOffset = -1;
    l.skullOffset = -1;
    l.kindMode = "idRange";
    if (!playerObj)
        return;
    int bestKind = -1;
    int bestKindScore = -1;
    const int kindHi = nameOff >= 0 ? nameOff + 96 : 320;
    for (int off = 16; off <= kindHi && off <= 320; ++off) {
        if (overlapsName(off, 1, nameOff) || off == l.hppercOffset)
            continue;
        if (off >= l.idOffset && off < l.idOffset + l.idSize)
            continue;
        if (off >= l.positionOffset && off < l.positionOffset + 12)
            continue;
        std::uint8_t mk = 0xFF;
        std::uint8_t pk = 0xFF;
        if (!memReadT(obj + static_cast<std::uintptr_t>(off), mk) ||
            !memReadT(playerObj + static_cast<std::uintptr_t>(off), pk))
            continue;
        if (pk != 0)
            continue;
        if (mk != 1 && mk != 2 && mk != 3 && mk != 4)
            continue;
        int score = 20;
        if (iabs(off - l.hppercOffset) < 32)
            score += 30;
        if (iabs(off - l.idOffset) < 48)
            score += 10;
        if (score > bestKindScore) {
            bestKindScore = score;
            bestKind = off;
        }
    }
    if (bestKind >= 0) {
        l.kindOffset = bestKind;
        l.kindMode = "field";
    }
    int bestSkull = -1;
    int bestSkullScore = -1;
    for (int off = 16; off <= kindHi && off <= 320; ++off) {
        if (overlapsName(off, 1, nameOff) || off == l.hppercOffset)
            continue;
        if (off >= l.positionOffset && off < l.positionOffset + 12)
            continue;
        if (off >= l.idOffset && off < l.idOffset + 4)
            continue;
        std::uint8_t ms = 0xFF;
        std::uint8_t ps = 0xFF;
        if (!memReadT(obj + static_cast<std::uintptr_t>(off), ms) ||
            !memReadT(playerObj + static_cast<std::uintptr_t>(off), ps))
            continue;
        if (ms > 6 || ps > 6)
            continue;
        int score = 5;
        if (l.kindOffset >= 0 && iabs(off - l.kindOffset) <= 4)
            score += 40;
        if (ms != 0)
            score += 20;
        if (score > bestSkullScore) {
            bestSkullScore = score;
            bestSkull = off;
        }
    }
    if (bestSkull >= 0 && bestSkullScore >= 45 && bestSkull != l.hppercOffset)
        l.skullOffset = bestSkull;
}

bool discoverFields(std::uintptr_t obj, std::uintptr_t nameAddr, const CreatureCalibHints& hints,
                    const Position& playerPos, const Profile& profile, std::uintptr_t playerObj,
                    std::uintptr_t statsBase, CreatureLayout& l, int* stage = nullptr) {
    if (stage)
        *stage = 0;
    l.ptrSize = static_cast<int>(sizeof(void*));
    if (profile.inventory.present && profile.inventory.ptrSize > 0)
        l.creaturePtrSize = profile.inventory.ptrSize;
    else if (profile.containers.present && profile.containers.itemPtrSize > 0)
        l.creaturePtrSize = profile.containers.itemPtrSize;
    const int nameOff = static_cast<int>(nameAddr - obj);
    if (nameOff < 8 || nameOff > kMaxNameBack)
        return false;
    l.nameOffset = nameOff;
#if defined(_WIN64)
    const bool requirePlayerMirror = playerObj && hasPlayerXY(playerPos);
#else
    // Preserve the established Win32 discovery behavior. Some clients expose
    // LocalPlayer and Creature coordinates through different object layouts.
    const bool requirePlayerMirror = hints.hasZ;
#endif
    if (!discoverPosition(obj, nameOff, playerPos, profile, playerObj, statsBase, l,
                          requirePlayerMirror, hints.hasZ))
        return false;
    if (stage)
        *stage = 1;
    if (!discoverId(obj, nameOff, hints, playerObj, l))
        return false;
    if (stage)
        *stage = 2;
    if (!discoverHpperc(obj, nameOff, hints, playerObj, l)) {
        // Exact target HP remains sufficient evidence when LocalPlayer uses a
        // different layout. Keep the stronger mirrored check first.
        if (!hints.hasHpperc || !discoverHpperc(obj, nameOff, hints, 0, l))
            return false;
    }
    if (stage)
        *stage = 3;
    discoverKindSkull(obj, nameOff, playerObj, l);
    return true;
}

bool prepareCreatureHandle(const CreatureLayout& l, std::uintptr_t& ptr, std::uintptr_t& rep) {
    if (!ptr)
        return false;
    const int width = creatureStride(l);
    if (width >= static_cast<int>(sizeof(void*) * 2))
        return completeSharedPtr(ptr, rep);
    if (width == static_cast<int>(sizeof(void*))) {
        rep = 0;
        return looksLikeCreatureObject(ptr);
    }
    return false;
}

bool discoverFieldsFromType(std::uintptr_t obj, int typePtrOff, int typeNameOff,
                            const std::string& nameKind, const CreatureCalibHints& hints,
                            const Position& playerPos, const Profile& profile,
                            std::uintptr_t playerObj, std::uintptr_t statsBase,
                            CreatureLayout& l, int* stage = nullptr) {
    if (stage)
        *stage = 0;
    l.ptrSize = static_cast<int>(sizeof(void*));
    if (profile.inventory.present && profile.inventory.ptrSize > 0)
        l.creaturePtrSize = profile.inventory.ptrSize;
    else if (profile.containers.present && profile.containers.itemPtrSize > 0)
        l.creaturePtrSize = profile.containers.itemPtrSize;
    l.nameOffset = -1;
    l.nameKind = nameKind;
    l.typePtrOffset = typePtrOff;
    l.typeNameOffset = typeNameOff;
    if (!discoverPosition(obj, -1, playerPos, profile, playerObj, statsBase, l, true,
                          hints.hasZ))
        return false;
    if (stage)
        *stage = 1;
    if (!discoverId(obj, -1, hints, playerObj, l))
        return false;
    if (stage)
        *stage = 2;
    if (!discoverHpperc(obj, -1, hints, playerObj, l))
        return false;
    if (stage)
        *stage = 3;
    discoverKindSkull(obj, -1, playerObj, l);
    return true;
}

bool nameOnThingType(std::uintptr_t obj, const CreatureLayout& l, std::string& out, int& nameOffOut) {
    if (!obj)
        return false;
    if (!looksLikeVtable(obj))
        return false;
    const int tryOff[] = {4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 48, 52, 64, 72, 80, 96};
    for (int off : tryOff) {
        std::string n;
        if (!readCreatureName(obj + static_cast<std::uintptr_t>(off), l, n) || !printableName(n))
            continue;
        out = n;
        nameOffOut = off;
        return true;
    }
    return false;
}

void rememberTypeOffsets(int ptrOff, int nameOff) {
    std::lock_guard<std::mutex> lock(g_mtx);
    if (g_layout.typePtrOffset >= 0)
        return;
    g_layout.typePtrOffset = ptrOff;
    g_layout.typeNameOffset = nameOff;
    LOG("creature type-name ptrOff=%d nameOff=%d", ptrOff, nameOff);
}

bool readTypeName(std::uintptr_t creature, const CreatureLayout& l, std::string& out,
                  const std::string* expect) {
    const int ps = static_cast<int>(sizeof(void*));
    int knownPtr = l.typePtrOffset;
    int knownName = l.typeNameOffset;
    if (knownPtr < 0 || knownName < 0) {
        std::lock_guard<std::mutex> lock(g_mtx);
        if (knownPtr < 0)
            knownPtr = g_layout.typePtrOffset;
        if (knownName < 0)
            knownName = g_layout.typeNameOffset;
    }
    auto take = [&](std::uintptr_t p, int ptrOff, bool discover) -> bool {
        if (!p || p == creature)
            return false;
        if (!looksLikeHeapObject(p) && !inModule(p))
            return false;
        int nameOff = knownName;
        std::string n;
        if (nameOff >= 0) {
            if (readCreatureName(p + static_cast<std::uintptr_t>(nameOff), l, n) &&
                printableName(n) && (!expect || nameMatches(n, *expect))) {
                out = std::move(n);
                return true;
            }
        }
        if (!discover)
            return false;
        if (!nameOnThingType(p, l, n, nameOff))
            return false;
        if (expect && !nameMatches(n, *expect))
            return false;
        out = n;
        rememberTypeOffsets(ptrOff, nameOff);
        return true;
    };
    if (knownPtr >= 0) {
        std::uintptr_t p = 0;
        if (readPtr(creature + static_cast<std::uintptr_t>(knownPtr), p) &&
            take(p, knownPtr, expect != nullptr))
            return true;
    }
    if (!expect)
        return false;
    for (int off = ps; off <= 512; off += ps) {
        std::uintptr_t p = 0;
        if (!readPtr(creature + static_cast<std::uintptr_t>(off), p))
            continue;
        if (take(p, off, true))
            return true;
    }
    return false;
}

bool fillCreatureName(std::uintptr_t obj, const CreatureLayout& l, std::string& out) {
    if (l.nameOffset >= 0 &&
        readCreatureName(obj + static_cast<std::uintptr_t>(l.nameOffset), l, out) &&
        printableName(out))
        return true;
    out.clear();
    return readTypeName(obj, l, out, nullptr);
}

bool readOneCreature(std::uintptr_t obj, const CreatureLayout& l, CreatureSnapshot& out) {
    if (!obj || !looksLikeCreatureObject(obj))
        return false;
    out = CreatureSnapshot{};
    out.id = readId(obj, l);
    if (!out.id)
        return false;
    std::string name;
    if (!fillCreatureName(obj, l, name))
        return false;
    out.name = name;
    if (!readCreaturePos(obj, l, out.x, out.y, out.z) || !plausiblePos(out.x, out.y, out.z))
        return false;
    out.hpperc = readHpperc(obj, l);
    if (out.hpperc < 0 || out.hpperc > 100)
        return false;
    // The OTClient monster range is unambiguous. A weakly calibrated auxiliary
    // byte must not override it (bytes near ids commonly vary between objects).
    if (out.id >= 0x40000000u && out.id < 0x80000000u) {
        out.kind = "monster";
    } else if (l.kindMode == "field" && l.kindOffset >= 0) {
        std::uint8_t k = 0;
        memReadT(obj + static_cast<std::uintptr_t>(l.kindOffset), k);
        out.kind = kindFromField(k);
    } else {
        out.kind = kindFromId(out.id);
    }
    if (l.skullOffset >= 0) {
        std::uint8_t s = 0;
        memReadT(obj + static_cast<std::uintptr_t>(l.skullOffset), s);
        if (s <= 6)
            out.skull = s;
    }
    return true;
}

bool playerAnchor(const CreatureLayout& l, Position& anchor, std::uint32_t& selfId) {
    selfId = 0;
    const std::uintptr_t obj = currentPlayerObject();
    if (obj)
        selfId = readId(obj, l);
    // P1 is calibrated independently and is the authoritative viewport anchor.
    // A provisional Creature position offset must not filter the collection
    // around the same bad value after a restart.
    if (readLivePlayerTile(anchor))
        return true;
    if (obj) {
        int x = 0, y = 0, z = 0;
        if (readCreaturePos(obj, l, x, y, z) && plausiblePos(x, y, z)) {
            anchor.x = x;
            anchor.y = y;
            anchor.z = z;
            return true;
        }
    }
    return false;
}

void filterVisible(const CreatureLayout& l, std::vector<CreatureSnapshot>& out,
                   std::vector<std::uintptr_t>* objs) {
    std::uint32_t selfId = 0;
    Position anchor{};
    if (!playerAnchor(l, anchor, selfId))
        return;

    std::vector<CreatureSnapshot> keep;
    std::vector<std::uintptr_t> keepObj;
    for (std::size_t i = 0; i < out.size(); ++i) {
        const auto& c = out[i];
        const bool visible =
            (selfId && c.id == selfId) ||
            (c.z == anchor.z && iabs(c.x - anchor.x) <= kScreenRange &&
             iabs(c.y - anchor.y) <= kScreenRange);
        if (!visible)
            continue;
        keep.push_back(c);
        if (objs && i < objs->size())
            keepObj.push_back((*objs)[i]);
    }
    out.swap(keep);
    if (objs)
        objs->swap(keepObj);
}

int chebyshev(int ax, int ay, int bx, int by) {
    const int dx = iabs(ax - bx);
    const int dy = iabs(ay - by);
    return dx > dy ? dx : dy;
}

void markViewport(const CreatureLayout& l, std::vector<CreatureSnapshot>& out) {
    std::uint32_t selfId = 0;
    Position anchor{};
    if (!playerAnchor(l, anchor, selfId)) {
        for (auto& c : out) {
            c.inViewport = false;
            c.distance = -1;
        }
        return;
    }
    for (auto& c : out) {
        c.distance = chebyshev(c.x, c.y, anchor.x, anchor.y);
        c.inViewport = (selfId && c.id == selfId) ||
                       (c.z == anchor.z && iabs(c.x - anchor.x) <= kViewportRangeX &&
                        iabs(c.y - anchor.y) <= kViewportRangeY);
    }
}

void stripLocalPlayer(const CreatureLayout& l, std::vector<CreatureSnapshot>& out,
                      std::vector<std::uintptr_t>* objs) {
    std::uint32_t selfId = 0;
    Position anchor{};
    playerAnchor(l, anchor, selfId);
    if (!selfId)
        return;
    std::size_t i = 0;
    while (i < out.size()) {
        if (out[i].id != selfId) {
            ++i;
            continue;
        }
        out.erase(out.begin() + static_cast<std::ptrdiff_t>(i));
        if (objs && i < objs->size())
            objs->erase(objs->begin() + static_cast<std::ptrdiff_t>(i));
    }
}

void finalizeCollection(const CreatureLayout& l, std::vector<CreatureSnapshot>& out,
                        std::vector<std::uintptr_t>* objs) {
    filterVisible(l, out, objs);
    markViewport(l, out);
    stripLocalPlayer(l, out, objs);
}

bool readCollectionRaw(std::uintptr_t base, const CreatureLayout& l,
                       std::vector<CreatureSnapshot>& out,
                       std::vector<std::uintptr_t>* objs) {
    out.clear();
    if (objs)
        objs->clear();
    if (!base)
        return false;
    const int ps = ptrSizeOf(l);
    const int stride = creatureStride(l);
    if (stride >= static_cast<int>(sizeof(void*) * 2)) {
        std::vector<CreatureSp> handles;
        if (!collectCreatureSp(base, l, handles))
            return false;
        std::set<std::uintptr_t> seenPtr;
        for (auto h : handles) {
            if (!h.ptr || seenPtr.count(h.ptr) || !completeSharedPtr(h.ptr, h.rep))
                continue;
            CreatureSnapshot snap;
            if (!readOneCreature(h.ptr, l, snap))
                continue;
            if (h.id && looksLikeCreatureId(h.id))
                snap.id = h.id;
            if (!snap.id)
                continue;
            seenPtr.insert(h.ptr);
            out.push_back(std::move(snap));
            if (objs)
                objs->push_back(h.ptr);
        }
        return !out.empty();
    }
    std::vector<std::pair<std::uint32_t, std::uintptr_t>> nodes;
    if (l.collection == "vector") {
        std::vector<std::uintptr_t> ptrs;
        if (parseVectorPtrs(base, ps, stride, 0, kMaxCreatures, ptrs)) {
            std::set<std::uintptr_t> seenPtr;
            for (auto p : ptrs) {
                if (!p || seenPtr.count(p))
                    continue;
                CreatureSnapshot snap;
                if (!readOneCreature(p, l, snap) || !snap.id)
                    continue;
                seenPtr.insert(p);
                out.push_back(std::move(snap));
                if (objs)
                    objs->push_back(p);
            }
        }
        return !out.empty();
    }
    bool walked = false;
    if (l.collection == "umap")
        walked = walkUMap(base, l, nodes);
    else
        walked = walkMap(base, l, nodes);
    if (walked) {
        std::set<std::uintptr_t> seenPtr;
        for (const auto& n : nodes) {
            if (!n.second || seenPtr.count(n.second))
                continue;
            CreatureSnapshot snap;
            if (!readOneCreature(n.second, l, snap))
                continue;
            if (n.first && looksLikeCreatureId(n.first))
                snap.id = n.first;
            else if (!snap.id)
                snap.id = n.first;
            if (!snap.id || seenPtr.count(n.second))
                continue;
            seenPtr.insert(n.second);
            out.push_back(std::move(snap));
            if (objs)
                objs->push_back(n.second);
        }
    }
    return !out.empty();
}

bool readCollection(std::uintptr_t base, const CreatureLayout& l,
                    std::vector<CreatureSnapshot>& out,
                    std::vector<std::uintptr_t>* objs) {
    readCollectionRaw(base, l, out, objs);
    finalizeCollection(l, out, objs);
    return !out.empty();
}

bool readCalibrationCollection(std::uintptr_t base, const CreatureLayout& l,
                               std::uintptr_t playerObj,
                               std::vector<CreatureSnapshot>& out) {
    std::vector<CreatureSnapshot> raw;
    std::vector<std::uintptr_t> objs;
    if (!readCollectionRaw(base, l, raw, &objs) || !playerObj)
        return false;

    int px = 0;
    int py = 0;
    int pz = 0;
    if (!readCreaturePos(playerObj, l, px, py, pz) || !plausiblePos(px, py, pz))
        return false;

    out.clear();
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const auto obj = i < objs.size() ? objs[i] : 0;
        auto c = raw[i];
        if (obj == playerObj)
            continue;
        if (c.z != pz || iabs(c.x - px) > kScreenRange ||
            iabs(c.y - py) > kScreenRange)
            continue;
        c.distance = chebyshev(c.x, c.y, px, py);
        c.inViewport = iabs(c.x - px) <= kViewportRangeX &&
                       iabs(c.y - py) <= kViewportRangeY;
        out.push_back(std::move(c));
    }
    return !out.empty();
}

struct Needle {
    std::uintptr_t obj = 0;
    CreatureLayout layout;
};

bool tryExistingLayout(const CreatureCalibHints& hints, const Profile& profile,
                       const Position& playerPos, Needle& out, std::string& nearbyNames) {
    nearbyNames.clear();
    if (!profile.creatures.present || profile.creatures.chain.empty())
        return false;
    CreatureLayout l = profile.creatures;
    const std::uintptr_t base = resolveChain(l.chain);
    std::vector<std::uintptr_t> objs;
    if (!collectCreaturePtrs(base, l, objs)) {
        LOG("P3 existing collection empty");
        return false;
    }
    int nearCount = 0;
    int named = 0;
    int empty = 0;
    std::vector<std::string> sample;
    for (auto obj : objs) {
        int x = 0, y = 0, z = 0;
        if (!readCreaturePos(obj, l, x, y, z) || !plausiblePos(x, y, z) ||
            !nearPlayer(x, y, z, playerPos, kCalibRange, hints.hasZ))
            continue;
        ++nearCount;
        std::string n;
        const bool own =
            readCreatureName(obj + static_cast<std::uintptr_t>(l.nameOffset), l, n) &&
            printableName(n);
        if (!own) {
            ++empty;
            if (!readTypeName(obj, l, n, &hints.name) || !printableName(n))
                continue;
        } else {
            ++named;
        }
        if (sample.size() < 8)
            sample.push_back(n);
        if (!nameMatches(n, hints.name))
            continue;
        if (hints.hasHpperc) {
            const int hpperc = readHpperc(obj, l);
            if (hpperc != hints.hpperc) {
                LOG("P3 existing reject hpperc=%d expected=%d obj=0x%p", hpperc,
                    hints.hpperc, reinterpret_cast<void*>(obj));
                continue;
            }
        }
        {
            std::lock_guard<std::mutex> lock(g_mtx);
            if (g_layout.typePtrOffset >= 0) {
                l.typePtrOffset = g_layout.typePtrOffset;
                l.typeNameOffset = g_layout.typeNameOffset;
            }
        }
        out.obj = obj;
        out.layout = l;
        out.layout.present = true;
        nearbyNames = n;
        LOG("P3 existing hit obj=0x%p name=%s ownName=%d typePtr=%d",
            reinterpret_cast<void*>(obj), n.c_str(), own ? 1 : 0, l.typePtrOffset);
        return true;
    }
    for (std::size_t i = 0; i < sample.size(); ++i) {
        if (i)
            nearbyNames += ", ";
        nearbyNames += sample[i];
    }
    LOG("P3 existing ptrs=%zu near=%d named=%d emptyName=%d sample=%s", objs.size(), nearCount,
        named, empty, nearbyNames.c_str());
    return false;
}

struct ScanStats {
    int byteHits = 0;
    int strOk = 0;
    int objOk = 0;
    int fieldOk = 0;
    int positionOk = 0;
    int idOk = 0;
    int hpOk = 0;
    int typeCandidates = 0;
    int typePointers = 0;
    int typeObjects = 0;
    int typeFields = 0;
    int typePositionOk = 0;
    int typeIdOk = 0;
    int typeHpOk = 0;
};

std::string formatScanStats(const ScanStats& st) {
    std::string s = "bytes=";
    s += std::to_string(st.byteHits);
    s += " str=";
    s += std::to_string(st.strOk);
    s += " obj=";
    s += std::to_string(st.objOk);
    s += " fields=";
    s += std::to_string(st.fieldOk);
    s += " stage=";
    s += std::to_string(st.positionOk);
    s += "/";
    s += std::to_string(st.idOk);
    s += "/";
    s += std::to_string(st.hpOk);
    s += " type=";
    s += std::to_string(st.typeCandidates);
    s += "/";
    s += std::to_string(st.typePointers);
    s += "/";
    s += std::to_string(st.typeObjects);
    s += "/";
    s += std::to_string(st.typeFields);
    s += " typeStage=";
    s += std::to_string(st.typePositionOk);
    s += "/";
    s += std::to_string(st.typeIdOk);
    s += "/";
    s += std::to_string(st.typeHpOk);
    return s;
}

struct TypeNameCandidate {
    std::uintptr_t obj = 0;
    int nameOffset = -1;
    std::string nameKind;
    bool exact = false;
};

void rememberTypeCandidate(std::vector<TypeNameCandidate>& out, std::uintptr_t obj,
                           int nameOffset, const std::string& nameKind, bool exact) {
    if (!obj || nameOffset < 0 || nameOffset > 256)
        return;
    for (auto& c : out) {
        if (c.obj != obj)
            continue;
        c.exact = c.exact || exact;
        return;
    }
    if (out.size() >= 64)
        return;
    out.push_back({obj, nameOffset, nameKind, exact});
}

std::vector<Needle> findNeedles(const CreatureCalibHints& hints, const Position& playerPos,
                               const Profile& profile, std::uintptr_t playerObj,
                               std::uintptr_t statsBase, DWORD t0, ScanStats& st,
                               std::vector<TypeNameCandidate>& typeCandidates) {
    std::vector<Needle> found;
    if (hints.name.empty())
        return found;
    const std::string& name = hints.name;
    MEMORY_BASIC_INFORMATION mbi{};
    std::uintptr_t addr = 0;
    while (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi))) {
        if (GetTickCount() - t0 > kScanBudgetMs)
            break;
        const std::uintptr_t next =
            reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        const bool heapish = regionWritable(mbi) && mbi.Type == MEM_PRIVATE;
        if (heapish && mbi.RegionSize >= name.size() + 1) {
            auto* begin = static_cast<std::uint8_t*>(mbi.BaseAddress);
            const std::size_t n = mbi.RegionSize;
            constexpr std::size_t kChunk = 65536;
            std::size_t off = 0;
            while (off + name.size() <= n) {
                if (GetTickCount() - t0 > kScanBudgetMs)
                    break;
                const std::size_t take =
                    n - off > kChunk + name.size() ? kChunk + name.size() : n - off;
                std::vector<char> buf(take);
                if (!memCopy(begin + off, buf.data(), take)) {
                    off += kChunk;
                    continue;
                }
                const unsigned char firstLower = static_cast<unsigned char>(
                    std::tolower(static_cast<unsigned char>(name.front())));
                const unsigned char firstUpper = static_cast<unsigned char>(
                    std::toupper(static_cast<unsigned char>(name.front())));
                const std::size_t lastStart = take - name.size();
                std::size_t i = 0;
                while (i <= lastStart) {
                    const std::size_t remaining = lastStart - i + 1;
                    const void* lowerHit =
                        std::memchr(buf.data() + i, firstLower, remaining);
                    const void* upperHit = firstUpper == firstLower
                                               ? nullptr
                                               : std::memchr(buf.data() + i, firstUpper, remaining);
                    if (!lowerHit && !upperHit)
                        break;
                    const char* nextHit = nullptr;
                    if (!lowerHit)
                        nextHit = static_cast<const char*>(upperHit);
                    else if (!upperHit)
                        nextHit = static_cast<const char*>(lowerHit);
                    else
                        nextHit = std::min(static_cast<const char*>(lowerHit),
                                           static_cast<const char*>(upperHit));
                    i = static_cast<std::size_t>(nextHit - buf.data());
                    const std::size_t hitIndex = i++;
                    if (!ciEqualBytes(buf.data() + hitIndex, name)) {
                        continue;
                    }
                    ++st.byteHits;
                    const std::uintptr_t hit =
                        reinterpret_cast<std::uintptr_t>(begin + off + hitIndex);
                    std::uintptr_t strObj = 0;
                    std::string kind;
                    if (!interpretNameHit(hit, name, strObj, kind))
                        continue;
                    ++st.strOk;
                    CreatureLayout nameLayout;
                    nameLayout.nameKind = kind;
                    std::string actualName;
                    const bool haveActual = readCreatureName(strObj, nameLayout, actualName) &&
                                            printableName(actualName) &&
                                            nameMatches(actualName, name);
                    const auto starts = objectStartsFromName(strObj);
                    for (const auto obj : starts) {
                        if (!obj || obj == playerObj)
                            continue;
                        if (haveActual && strObj >= obj) {
                            const auto delta = strObj - obj;
                            if (delta <= 256)
                                rememberTypeCandidate(typeCandidates, obj,
                                                      static_cast<int>(delta), kind,
                                                      nameEq(actualName, name));
                        }
                        ++st.objOk;
                        Needle nd;
                        nd.obj = obj;
                        nd.layout.nameKind = kind;
                        int stage = 0;
                        if (!discoverFields(obj, strObj, hints, playerPos, profile, playerObj,
                                            statsBase, nd.layout, &stage)) {
                            if (stage >= 1)
                                ++st.positionOk;
                            if (stage >= 2)
                                ++st.idOk;
                            if (stage >= 3)
                                ++st.hpOk;
                            continue;
                        }
                        ++st.positionOk;
                        ++st.idOk;
                        ++st.hpOk;
                        nd.layout.nameKind = kind;
                        CreatureSnapshot snap;
                        if (!readOneCreature(obj, nd.layout, snap) || !nameMatches(snap.name, name))
                            continue;
                        if (hints.hasHpperc && snap.hpperc != hints.hpperc)
                            continue;
                        if (playerObj) {
                            std::string pname;
                            if (!readCreatureName(playerObj + static_cast<std::uintptr_t>(
                                                                  nd.layout.nameOffset),
                                                  nd.layout, pname) ||
                                !printableName(pname) || nameEq(pname, name))
                                continue;
                        }
                        ++st.fieldOk;
                        found.push_back(nd);
                        break;
                    }
                    if (found.size() >= 6)
                        return found;
                }
                if (take <= name.size())
                    break;
                off += take - (name.size() - 1);
            }
        }
        if (next <= addr)
            break;
        addr = next;
    }
    if (found.size() > 1) {
        std::sort(found.begin(), found.end(), [](const Needle& a, const Needle& b) {
            return a.layout.nameOffset < b.layout.nameOffset;
        });
        found.resize(1);
    }
    return found;
}

struct TypePointerHit {
    std::uintptr_t address = 0;
    std::size_t candidate = 0;
};

std::vector<TypePointerHit> findTypePointerHits(
    const std::vector<TypeNameCandidate>& candidates, DWORD t0) {
    std::vector<TypePointerHit> hits;
    std::vector<std::pair<std::uintptr_t, std::size_t>> targets;
    targets.reserve(candidates.size());
    for (std::size_t i = 0; i < candidates.size(); ++i)
        targets.push_back({candidates[i].obj, i});
    std::sort(targets.begin(), targets.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    MEMORY_BASIC_INFORMATION mbi{};
    std::uintptr_t addr = 0;
    constexpr std::size_t kChunk = 65536;
    const std::size_t ps = sizeof(void*);
    while (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi))) {
        if (GetTickCount() - t0 > kScanBudgetMs)
            break;
        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const std::uintptr_t next = base + mbi.RegionSize;
        const bool searchable = regionWritable(mbi) &&
                                (mbi.Type == MEM_PRIVATE || mbi.Type == MEM_MAPPED);
        if (searchable) {
            for (std::size_t off = 0; off + ps <= mbi.RegionSize; off += kChunk) {
                if (GetTickCount() - t0 > kScanBudgetMs)
                    break;
                const std::size_t take =
                    std::min<std::size_t>(kChunk, mbi.RegionSize - off);
                std::vector<std::uint8_t> buf(take);
                if (!memCopy(reinterpret_cast<const void*>(base + off), buf.data(), take))
                    continue;
                for (std::size_t i = 0; i + ps <= take; i += ps) {
                    std::uintptr_t value = 0;
                    std::memcpy(&value, buf.data() + i, ps);
                    const auto it = std::lower_bound(
                        targets.begin(), targets.end(), value,
                        [](const auto& entry, std::uintptr_t v) { return entry.first < v; });
                    if (it == targets.end() || it->first != value)
                        continue;
                    hits.push_back({base + off + i, it->second});
                    if (hits.size() >= 8192)
                        return hits;
                }
            }
        }
        if (next <= addr)
            break;
        addr = next;
    }
    return hits;
}

std::vector<Needle> findNeedlesFromType(
    const CreatureCalibHints& hints, const Position& playerPos, const Profile& profile,
    std::uintptr_t playerObj, std::uintptr_t statsBase,
    std::vector<TypeNameCandidate> candidates, DWORD t0, ScanStats& st) {
    std::vector<Needle> found;
    if (candidates.empty())
        return found;
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const TypeNameCandidate& a, const TypeNameCandidate& b) {
                         if (a.exact != b.exact)
                             return a.exact;
                         return a.nameOffset < b.nameOffset;
                     });
    if (candidates.size() > 32)
        candidates.resize(32);
    st.typeCandidates = static_cast<int>(candidates.size());
    const auto hits = findTypePointerHits(candidates, t0);
    st.typePointers = static_cast<int>(hits.size());
    std::set<std::uintptr_t> tested;
    const int ps = static_cast<int>(sizeof(void*));
    for (const auto& hit : hits) {
        if (GetTickCount() - t0 > kScanBudgetMs || hit.candidate >= candidates.size())
            break;
        const auto& type = candidates[hit.candidate];
        for (int back = ps; back <= 512; back += ps) {
            if (hit.address < static_cast<std::uintptr_t>(back))
                break;
            const std::uintptr_t obj = hit.address - static_cast<std::uintptr_t>(back);
            if (!obj || obj == playerObj || tested.count(obj) || !looksLikeCreatureObject(obj))
                continue;
            tested.insert(obj);
            ++st.typeObjects;
            CreatureLayout l;
            int stage = 0;
            if (!discoverFieldsFromType(obj, back, type.nameOffset, type.nameKind, hints,
                                        playerPos, profile, playerObj, statsBase, l, &stage)) {
                if (stage >= 1)
                    ++st.typePositionOk;
                if (stage >= 2)
                    ++st.typeIdOk;
                if (stage >= 3)
                    ++st.typeHpOk;
                continue;
            }
            ++st.typePositionOk;
            ++st.typeIdOk;
            ++st.typeHpOk;
            CreatureSnapshot snap;
            if (!readOneCreature(obj, l, snap) || !nameMatches(snap.name, hints.name))
                continue;
            if (hints.hasHpperc && snap.hpperc != hints.hpperc)
                continue;
            ++st.typeFields;
            found.push_back({obj, l});
            if (found.size() >= 6)
                return found;
        }
    }
    return found;
}

void findPointersTo(std::uintptr_t target, std::vector<std::uintptr_t>& hits, DWORD t0) {
    hits.clear();
    MEMORY_BASIC_INFORMATION mbi{};
    std::uintptr_t addr = 0;
    constexpr std::size_t kChunk = 65536;
    const std::size_t ps = sizeof(void*);
    while (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi))) {
        if (GetTickCount() - t0 > kScanBudgetMs)
            break;
        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const std::uintptr_t next = base + mbi.RegionSize;
        if (regionWritable(mbi) && mbi.RegionSize >= ps) {
            for (std::size_t off = 0; off + ps <= mbi.RegionSize; off += kChunk) {
                if (GetTickCount() - t0 > kScanBudgetMs)
                    break;
                const std::size_t take =
                    std::min<std::size_t>(kChunk, mbi.RegionSize - off);
                std::vector<std::uint8_t> buf(take);
                if (!memCopy(reinterpret_cast<const void*>(base + off), buf.data(), take))
                    continue;
                for (std::size_t i = 0; i + ps <= take; i += ps) {
                    std::uintptr_t value = 0;
                    std::memcpy(&value, buf.data() + i, ps);
                    if (value != target)
                        continue;
                    hits.push_back(base + off + i);
                    if (hits.size() >= 4096)
                        return;
                }
            }
        }
        if (next <= addr)
            break;
        addr = next;
    }
}

std::vector<ChainStep> findChainTo(std::uintptr_t target, DWORD t0) {
    std::vector<ChainStep> found;
    const std::uintptr_t mod = moduleBase();
    const std::size_t img = moduleImageSize();
    if (!mod || !img || !target)
        return found;
    if (target >= mod && target < mod + img) {
        LOG("creature collection in module rva=0x%X", static_cast<unsigned>(target - mod));
        return {{static_cast<std::int32_t>(target - mod), false}};
    }

    const int ps = static_cast<int>(sizeof(void*));
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> heapPtrs;
    MEMORY_BASIC_INFORMATION mbi{};
    std::uintptr_t addr = mod;
    const std::uintptr_t modEnd = mod + img;
    while (addr < modEnd && VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi))) {
        if (GetTickCount() - t0 > kScanBudgetMs)
            break;
        const std::uintptr_t next =
            reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        const std::uintptr_t regionEnd = next < modEnd ? next : modEnd;
        if (mbi.State == MEM_COMMIT && regionEnd > addr) {
            if (!memPageGuarded(mbi.Protect)) {
                for (std::uintptr_t a = addr; a + static_cast<std::uintptr_t>(ps) <= regionEnd;
                     a += static_cast<std::uintptr_t>(ps)) {
                    std::uintptr_t v = 0;
                    if (!memCopy(reinterpret_cast<const void*>(a), &v, sizeof(v)) || v == 0)
                        continue;
                    if (v == target) {
                        LOG("creature collection ptr in module rva=0x%X",
                            static_cast<unsigned>(a - mod));
                        return {{static_cast<std::int32_t>(a - mod), true}};
                    }
                    if (v < mod || v >= mod + img)
                        heapPtrs.push_back({a, v});
                }
            }
        }
        if (next <= addr)
            break;
        addr = next;
    }

    for (const auto& hp : heapPtrs) {
        if (GetTickCount() - t0 > kScanBudgetMs)
            break;
        const std::uintptr_t mid = hp.second;
        if (!looksLikeHeapObject(mid))
            continue;
        for (int off = 0; off <= 4096; off += ps) {
            std::uintptr_t inner = 0;
            if (!memCopy(reinterpret_cast<const void*>(mid + static_cast<std::uintptr_t>(off)),
                         &inner, sizeof(inner)))
                continue;
            if (inner == target) {
                LOG("creature collection via heap rva=0x%X off=%d",
                    static_cast<unsigned>(hp.first - mod), off);
                return {{static_cast<std::int32_t>(hp.first - mod), true}, {off, false}};
            }
        }
    }
    LOG("creature-chain miss for 0x%p", reinterpret_cast<void*>(target));
    return found;
}

std::uintptr_t findMapObject(std::uintptr_t sentinel, const CreatureLayout& l,
                             std::uintptr_t needle, DWORD t0) {
    std::vector<std::uintptr_t> hits;
    findPointersTo(sentinel, hits, t0);
    const int ps = static_cast<int>(sizeof(void*));
    std::uintptr_t best = 0;
    int bestScore = -1;
    for (auto addr : hits) {
        std::size_t sz = 0;
        if (ps == 4) {
            std::uint32_t s = 0;
            if (!memReadT(addr + 4, s))
                continue;
            sz = s;
        } else {
            if (!memReadT(addr + 8, sz))
                continue;
        }
        if (sz < 1 || sz > static_cast<std::size_t>(kMaxCreatures))
            continue;
        CreatureLayout trial = l;
        trial.mapHeadOffset = 0;
        trial.mapSizeOffset = ps;
        std::vector<std::pair<std::uint32_t, std::uintptr_t>> nodes;
        if (!walkMap(addr, trial, nodes))
            continue;
        bool hasNeedle = false;
        for (const auto& n : nodes) {
            if (n.second == needle)
                hasNeedle = true;
        }
        if (!hasNeedle)
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

std::uintptr_t findUMapObject(std::uintptr_t sentinel, const CreatureLayout& l,
                              std::uintptr_t needle, DWORD t0) {
    std::vector<std::uintptr_t> hits;
    findPointersTo(sentinel, hits, t0);
    const int ps = static_cast<int>(sizeof(void*));
    std::uintptr_t best = 0;
    int bestScore = -1;
    for (auto addr : hits) {
        std::size_t sz = 0;
        if (ps == 4) {
            std::uint32_t s = 0;
            if (!memReadT(addr + 4, s))
                continue;
            sz = s;
        } else {
            if (!memReadT(addr + 8, sz))
                continue;
        }
        if (sz < 1 || sz > static_cast<std::size_t>(kMaxCreatures))
            continue;
        CreatureLayout trial = l;
        trial.umapHeadOffset = 0;
        trial.umapSizeOffset = ps;
        std::vector<std::pair<std::uint32_t, std::uintptr_t>> nodes;
        if (!walkUMap(addr, trial, nodes))
            continue;
        bool hasNeedle = false;
        for (const auto& n : nodes) {
            if (n.second == needle)
                hasNeedle = true;
        }
        if (!hasNeedle)
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

int scoreRead(std::uintptr_t base, const CreatureLayout& l, std::uintptr_t needle,
              std::uintptr_t playerObj, const std::string& expectName) {
    // P1's X/Y can still be provisional during P3. Preserve the runtime visibility filter, but
    // allow structural scoring when the raw collection contains the known needle and the newly
    // discovered Creature layout independently proves it is near the LocalPlayer object.
    std::vector<std::uintptr_t> rawObjs;
    if (l.collection == "vector") {
        parseVectorPtrs(base, ptrSizeOf(l), creatureStride(l), 0, kMaxCreatures, rawObjs);
    } else {
        std::vector<std::pair<std::uint32_t, std::uintptr_t>> nodes;
        const bool walked = l.collection == "umap" ? walkUMap(base, l, nodes)
                                                   : walkMap(base, l, nodes);
        if (walked) {
            rawObjs.reserve(nodes.size());
            for (const auto& node : nodes)
                rawObjs.push_back(node.second);
        }
    }
    const bool rawHasNeedle =
        std::find(rawObjs.begin(), rawObjs.end(), needle) != rawObjs.end();
    const bool rawHasPlayer = playerObj &&
                              std::find(rawObjs.begin(), rawObjs.end(), playerObj) != rawObjs.end();
    bool rawNeedleNearPlayer = false;
    if (rawHasNeedle && playerObj) {
        int nx = 0, ny = 0, nz = 0;
        int px = 0, py = 0, pz = 0;
        if (readCreaturePos(needle, l, nx, ny, nz) && plausiblePos(nx, ny, nz) &&
            readCreaturePos(playerObj, l, px, py, pz) && plausiblePos(px, py, pz)) {
            rawNeedleNearPlayer =
                nearPlayer(nx, ny, nz, Position{px, py, pz}, kCalibRange, false);
        }
    }

    std::vector<CreatureSnapshot> cur;
    std::vector<std::uintptr_t> objs;
    if ((!readCollection(base, l, cur, &objs) || cur.empty()) && !rawNeedleNearPlayer)
        return -1;
    bool hasNeedle = rawNeedleNearPlayer;
    bool hasPlayer = rawHasPlayer;
    bool hasName = false;
    for (std::size_t i = 0; i < objs.size(); ++i) {
        if (objs[i] == needle)
            hasNeedle = true;
        if (playerObj && objs[i] == playerObj)
            hasPlayer = true;
    }
    for (const auto& c : cur) {
        if (nameMatches(c.name, expectName))
            hasName = true;
    }
    if (!hasNeedle && !hasName)
        return -1;
    const int n = static_cast<int>(cur.empty() ? rawObjs.size() : cur.size());
    if (n > kMaxCreatures)
        return -1;
    int score = n * 5 + 20;
    if (n >= 2 && n <= 48)
        score += 80;
    if (n > 80)
        score -= 50;
    if (hasPlayer)
        score += 200;
    if (inModule(base))
        score += 1000;
    return score;
}

struct FoundCol {
    std::uintptr_t base = 0;
    CreatureLayout layout;
    int score = -1;
};

struct CollectionProbe {
    std::uintptr_t sentinel = 0;
    CreatureLayout layout;
};

void considerCol(FoundCol& best, std::uintptr_t base, const CreatureLayout& l, int score) {
    if (score > best.score) {
        best.score = score;
        best.base = base;
        best.layout = l;
    }
}

void addCollectionProbe(std::vector<CollectionProbe>& probes, std::uintptr_t sentinel,
                        const CreatureLayout& layout) {
    if (!sentinel)
        return;
    for (const auto& p : probes) {
        const auto& a = p.layout;
        if (p.sentinel == sentinel && a.collection == layout.collection &&
            a.mapHeadOffset == layout.mapHeadOffset &&
            a.mapSizeOffset == layout.mapSizeOffset &&
            a.mapNodeLeftOffset == layout.mapNodeLeftOffset &&
            a.mapNodeParentOffset == layout.mapNodeParentOffset &&
            a.mapNodeRightOffset == layout.mapNodeRightOffset &&
            a.mapNodeIsnilOffset == layout.mapNodeIsnilOffset &&
            a.mapNodeKeyOffset == layout.mapNodeKeyOffset &&
            a.mapNodePtrOffset == layout.mapNodePtrOffset &&
            a.umapHeadOffset == layout.umapHeadOffset &&
            a.umapSizeOffset == layout.umapSizeOffset &&
            a.listNextOffset == layout.listNextOffset &&
            a.listPrevOffset == layout.listPrevOffset &&
            a.listKeyOffset == layout.listKeyOffset &&
            a.listPtrOffset == layout.listPtrOffset &&
            a.ptrSize == layout.ptrSize &&
            a.creaturePtrSize == layout.creaturePtrSize)
            return;
    }
    probes.push_back({sentinel, layout});
}

void scanModuleCollectionProbes(const std::vector<CollectionProbe>& probes,
                                std::uintptr_t needle, std::uintptr_t playerObj,
                                const std::string& name, FoundCol& best) {
    if (probes.empty())
        return;
    const std::uintptr_t mod = moduleBase();
    const std::size_t img = moduleImageSize();
    if (!mod || !img)
        return;

    std::unordered_multimap<std::uintptr_t, std::size_t> bySentinel;
    for (std::size_t i = 0; i < probes.size(); ++i)
        bySentinel.emplace(probes[i].sentinel, i);

    std::set<std::pair<std::uintptr_t, std::size_t>> tested;
    const std::uintptr_t modEnd = mod + img;
    const int ps = static_cast<int>(sizeof(void*));
    MEMORY_BASIC_INFORMATION mbi{};
    std::uintptr_t addr = mod;
    while (addr < modEnd && VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi))) {
        const std::uintptr_t regionBase = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const std::uintptr_t next = regionBase + mbi.RegionSize;
        const std::uintptr_t lo = addr > regionBase ? addr : regionBase;
        const std::uintptr_t hi = next < modEnd ? next : modEnd;
        if (regionWritable(mbi) && hi > lo) {
            for (std::uintptr_t a = lo; a + static_cast<std::uintptr_t>(ps) <= hi;
                 a += static_cast<std::uintptr_t>(ps)) {
                std::uintptr_t value = 0;
                if (!memCopy(reinterpret_cast<const void*>(a), &value, sizeof(value)))
                    continue;
                const auto range = bySentinel.equal_range(value);
                for (auto it = range.first; it != range.second; ++it) {
                    const CreatureLayout& l = probes[it->second].layout;
                    const std::int32_t headOff =
                        l.collection == "umap" ? l.umapHeadOffset : l.mapHeadOffset;
                    if (headOff < 0 || a < static_cast<std::uintptr_t>(headOff))
                        continue;
                    const std::uintptr_t base =
                        a - static_cast<std::uintptr_t>(static_cast<std::uint32_t>(headOff));
                    if (!tested.emplace(base, it->second).second)
                        continue;
                    const int score = scoreRead(base, l, needle, playerObj, name);
                    if (score < 0)
                        continue;
                    LOG("creature module collection rva=0x%X collection=%s score=%d",
                        static_cast<unsigned>(base - mod), l.collection.c_str(), score);
                    considerCol(best, base, l, score);
                }
            }
        }
        if (next <= addr)
            break;
        addr = next;
    }
}

bool collectionHeaderSize(std::uintptr_t base, const CreatureLayout& l,
                          std::size_t& sizeOut) {
    const std::int32_t sizeOff =
        l.collection == "umap" ? l.umapSizeOffset : l.mapSizeOffset;
    if (sizeOff < 0)
        return false;
    if (ptrSizeOf(l) == 4) {
        std::uint32_t size = 0;
        if (!memReadT(base + static_cast<std::uintptr_t>(sizeOff), size))
            return false;
        sizeOut = size;
    } else {
        if (!memReadT(base + static_cast<std::uintptr_t>(sizeOff), sizeOut))
            return false;
    }
    return sizeOut >= 1 && sizeOut <= static_cast<std::size_t>(kMaxCreatures);
}

void scanWritableCollectionProbes(const std::vector<CollectionProbe>& mapProbes,
                                  const std::vector<CollectionProbe>& umapProbes,
                                  std::uintptr_t needle, std::uintptr_t playerObj,
                                  const std::string& name, FoundCol& best) {
    std::vector<CollectionProbe> probes;
    probes.reserve(mapProbes.size() + umapProbes.size());
    probes.insert(probes.end(), mapProbes.begin(), mapProbes.end());
    probes.insert(probes.end(), umapProbes.begin(), umapProbes.end());
    if (probes.empty())
        return;

    std::unordered_multimap<std::uintptr_t, std::size_t> bySentinel;
    for (std::size_t i = 0; i < probes.size(); ++i)
        bySentinel.emplace(probes[i].sentinel, i);

    const DWORD t0 = GetTickCount();
    const int ps = static_cast<int>(sizeof(void*));
    constexpr std::size_t kChunk = 65536;
    std::set<std::pair<std::uintptr_t, std::size_t>> tested;
    int ownerHits = 0;
    int scoredMap = 0;
    int scoredUmap = 0;
    const DWORD regionTypes[] = {MEM_PRIVATE, MEM_MAPPED};
    for (DWORD wantedType : regionTypes) {
        MEMORY_BASIC_INFORMATION mbi{};
        std::uintptr_t addr = 0;
        while (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi))) {
            if (GetTickCount() - t0 > kScanBudgetMs)
                break;
            const std::uintptr_t regionBase =
                reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
            const std::uintptr_t next = regionBase + mbi.RegionSize;
            if (regionWritable(mbi) && mbi.Type == wantedType) {
                for (std::size_t off = 0;
                     off + static_cast<std::size_t>(ps) <= mbi.RegionSize; off += kChunk) {
                    if (GetTickCount() - t0 > kScanBudgetMs)
                        break;
                    const std::size_t take =
                        std::min<std::size_t>(kChunk, mbi.RegionSize - off);
                    std::vector<std::uint8_t> buf(take);
                    if (!memCopy(reinterpret_cast<const void*>(regionBase + off), buf.data(),
                                 take))
                        continue;
                    for (std::size_t i = 0; i + static_cast<std::size_t>(ps) <= take;
                         i += static_cast<std::size_t>(ps)) {
                        std::uintptr_t value = 0;
                        std::memcpy(&value, buf.data() + i, static_cast<std::size_t>(ps));
                        const auto range = bySentinel.equal_range(value);
                        for (auto it = range.first; it != range.second; ++it) {
                            const std::size_t probeIndex = it->second;
                            const auto& probe = probes[probeIndex];
                            const auto& l = probe.layout;
                            const std::int32_t headOff =
                                l.collection == "umap" ? l.umapHeadOffset : l.mapHeadOffset;
                            const std::uintptr_t field = regionBase + off + i;
                            if (headOff < 0 || field < static_cast<std::uintptr_t>(headOff))
                                continue;
                            const std::uintptr_t base =
                                field - static_cast<std::uintptr_t>(headOff);
                            if (!tested.emplace(base, probeIndex).second)
                                continue;
                            std::size_t size = 0;
                            if (!collectionHeaderSize(base, l, size))
                                continue;
                            ++ownerHits;
                            const int score = scoreRead(base, l, needle, playerObj, name);
                            if (score < 0)
                                continue;
                            if (l.collection == "umap")
                                ++scoredUmap;
                            else
                                ++scoredMap;
                            considerCol(best, base, l, score);
                        }
                    }
                }
            }
            if (next <= addr)
                break;
            addr = next;
        }
        if (GetTickCount() - t0 > kScanBudgetMs)
            break;
    }
    LOG("creature writable owners hits=%d scoredMap=%d scoredUmap=%d tested=%zu ms=%lu",
        ownerHits, scoredMap, scoredUmap, tested.size(),
        static_cast<unsigned long>(GetTickCount() - t0));
}

void tryVectorAt(std::uintptr_t addr, CreatureLayout l, std::uintptr_t needle,
                 std::uintptr_t playerObj, const std::string& name, FoundCol& best) {
    const int ps = ptrSizeOf(l);
#if defined(_WIN64)
    const int tries[] = {l.creaturePtrSize > 0 ? l.creaturePtrSize : 0, 16, 8, 4};
#else
    const int tries[] = {l.creaturePtrSize > 0 ? l.creaturePtrSize : 0, 8, 4};
#endif
    for (int es : tries) {
        if (es <= 0)
            continue;
        l.creaturePtrSize = es;
        std::vector<std::uintptr_t> ptrs;
        if (!parseVectorPtrs(addr, ps, es, 1, kMaxCreatures, ptrs))
            continue;
        bool has = false;
        int creatures = 0;
        for (auto p : ptrs) {
            if (!p)
                continue;
            if (p == needle)
                has = true;
            if (looksLikeCreatureObject(p))
                ++creatures;
        }
        if (!has || creatures < 1)
            continue;
        l.collection = "vector";
        const int score = scoreRead(addr, l, needle, playerObj, name);
        if (score >= 0)
            considerCol(best, addr, l, score);
    }
}

void seedMapLayout(CreatureLayout& l, const Profile& profile) {
    const int ps = ptrSizeOf(l);
    l.mapHeadOffset = 0;
    l.mapSizeOffset = ps;
    l.mapNodeLeftOffset = 0;
    l.mapNodeParentOffset = ps;
    l.mapNodeRightOffset = ps * 2;
    l.mapNodeIsnilOffset = ps * 3 + 1;
    l.mapNodeKeyOffset = ps * 4;
    l.mapNodePtrOffset = ps * 5;
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
        if (c.mapNodePtrOffset)
            l.mapNodePtrOffset = c.mapNodePtrOffset;
        if (c.itemPtrSize > 0)
            l.creaturePtrSize = c.itemPtrSize;
        if (c.ptrSize > 0)
            l.ptrSize = c.ptrSize;
    } else if (profile.inventory.present && profile.inventory.ptrSize > 0) {
        l.creaturePtrSize = profile.inventory.ptrSize;
    }
}

void tryLayoutsAt(std::uintptr_t addr, CreatureLayout seeded, std::uintptr_t needle,
                  std::uintptr_t playerObj, const std::string& name, FoundCol& best) {
    std::uintptr_t head = 0;
    if (!readPtr(addr, head) || !head)
        return;
    if (!looksLikeHeapObject(head) && !inModule(head))
        return;

    CreatureLayout mapLay = seeded;
    mapLay.collection = "map";
    int score = scoreRead(addr, mapLay, needle, playerObj, name);
    if (score >= 0)
        considerCol(best, addr, mapLay, score);
    if (seeded.creaturePtrSize == 8 &&
        seeded.mapNodePtrOffset == seeded.mapNodeKeyOffset + 4) {
        CreatureLayout aligned = mapLay;
        aligned.mapNodePtrOffset = seeded.mapNodeKeyOffset + 8;
        score = scoreRead(addr, aligned, needle, playerObj, name);
        if (score >= 0)
            considerCol(best, addr, aligned, score);
    }

    const int ps = ptrSizeOf(seeded);
    const int umapPtr[] = {ps * 3, ps * 4, ps * 2 + 4, 12, 16, 20};
    for (int ptrOff : umapPtr) {
        if (ptrOff < ps * 2 + 4)
            continue;
        CreatureLayout u = seeded;
        u.collection = "umap";
        u.umapHeadOffset = 0;
        u.umapSizeOffset = ps;
        u.listNextOffset = 0;
        u.listPrevOffset = ps;
        u.listPtrOffset = ptrOff;
        u.listKeyOffset = ptrOff - 4;
        score = scoreRead(addr, u, needle, playerObj, name);
        if (score >= 0)
            considerCol(best, addr, u, score);
    }
    tryVectorAt(addr, seeded, needle, playerObj, name, best);
}

void scanAround(std::uintptr_t center, int range, const CreatureLayout& seeded,
                std::uintptr_t needle, std::uintptr_t playerObj, const std::string& name,
                FoundCol& best) {
    if (!center)
        return;
    const int ps = ptrSizeOf(seeded);
    for (int off = -range; off <= range; off += ps) {
        const std::intptr_t a = static_cast<std::intptr_t>(center) + off;
        if (a <= 0x10000)
            continue;
        tryLayoutsAt(static_cast<std::uintptr_t>(a), seeded, needle, playerObj, name, best);
    }
}

bool fixIdFromCollection(std::uintptr_t collection, CreatureLayout& l, std::uintptr_t needle) {
    std::vector<CreatureSnapshot> cur;
    std::vector<std::uintptr_t> objs;
    if (!readCollectionRaw(collection, l, cur, &objs))
        return false;
    std::vector<std::pair<std::uint32_t, std::uintptr_t>> nodes;
    if (l.collection == "map")
        walkMap(collection, l, nodes);
    else if (l.collection == "umap")
        walkUMap(collection, l, nodes);
    std::uint32_t key = 0;
    for (const auto& n : nodes) {
        if (n.second == needle) {
            key = n.first;
            break;
        }
    }
    if (!key)
        return false;
    if (!looksLikeCreatureId(key)) {
        LOG("creature skip idOffset correction key=%u (not a server id)", key);
        return false;
    }
    if (readId(needle, l) == key)
        return false;
    for (int off = 4; off <= 320; off += 4) {
        std::uint32_t v = 0;
        if (!memReadT(needle + static_cast<std::uintptr_t>(off), v) || v != key)
            continue;
        l.idOffset = off;
        l.idSize = 4;
        LOG("creature idOffset corrected to %d key=%u", off, key);
        return true;
    }
    return false;
}

json creatureObject(const CreatureSnapshot& c) {
    json j;
    j["id"] = c.id;
    j["name"] = c.name;
    j["x"] = c.x;
    j["y"] = c.y;
    j["z"] = c.z;
    j["hpperc"] = c.hpperc;
    j["kind"] = c.kind;
    j["skull"] = c.skull;
    j["inViewport"] = c.inViewport;
    j["distance"] = c.distance;
    return j;
}

bool sameCreature(const CreatureSnapshot& a, const CreatureSnapshot& b) {
    return a.id == b.id && a.name == b.name && a.x == b.x && a.y == b.y && a.z == b.z &&
           a.hpperc == b.hpperc && a.kind == b.kind && a.skull == b.skull &&
           a.inViewport == b.inViewport && a.distance == b.distance;
}

void emitDiff(const std::vector<CreatureSnapshot>& prev, const std::vector<CreatureSnapshot>& next) {
    auto findId = [](const std::vector<CreatureSnapshot>& v,
                     std::uint32_t id) -> const CreatureSnapshot* {
        for (const auto& c : v) {
            if (c.id == id)
                return &c;
        }
        return nullptr;
    };
    for (const auto& c : next) {
        const auto* old = findId(prev, c.id);
        if (!old)
            PipeServer::instance().tryEmit("creatureAppear", creatureObject(c).dump());
        else if (!sameCreature(*old, c))
            PipeServer::instance().tryEmit("creatureUpdate", creatureObject(c).dump());
    }
    for (const auto& c : prev) {
        if (!findId(next, c.id)) {
            json j;
            j["id"] = c.id;
            j["name"] = c.name;
            PipeServer::instance().tryEmit("creatureDisappear", j.dump());
        }
    }
}

DWORD WINAPI pollThread(LPVOID) {
    while (g_pollRun) {
        CreatureLayout layout;
        {
            std::lock_guard<std::mutex> lock(g_mtx);
            layout = g_layout;
        }
        if (!layout.present || layout.chain.empty()) {
            Sleep(kPollMs);
            continue;
        }
        const std::uintptr_t base = resolveChain(layout.chain);
        std::vector<CreatureSnapshot> next;
        if (base)
            readCollection(base, layout, next, nullptr);
        std::vector<CreatureSnapshot> prev;
        {
            std::lock_guard<std::mutex> lock(g_mtx);
            prev = g_live;
            g_live = next;
        }
        emitDiff(prev, next);
        Sleep(kPollMs);
    }
    return 0;
}

} // namespace

void startCreatureReader() {
    Profile p;
    copyLiveProfile(p);
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_layout = p.creatures;
        if (g_layout.present)
            g_msg = "creature layout loaded";
        else
            g_msg = "put a monster on screen and run p3-calibrate";
    }
    if (g_layout.present && !g_layout.chain.empty()) {
        const std::uintptr_t base = resolveChain(g_layout.chain);
        if (base) {
            std::vector<CreatureSnapshot> cur;
            readCollection(base, g_layout, cur, nullptr);
            std::lock_guard<std::mutex> lock(g_mtx);
            g_live = std::move(cur);
            g_msg = "creature-chain resolved";
            LOG("creature-chain resolved at 0x%p collection=%s n=%zu",
                reinterpret_cast<void*>(base), g_layout.collection.c_str(), g_live.size());
        } else {
            LOG("creature-chain failed (%zu steps)", g_layout.chain.size());
        }
    }

    g_pollRun = true;
    g_pollThread = CreateThread(nullptr, 0, pollThread, nullptr, 0, nullptr);
    if (g_pollThread)
        CloseHandle(g_pollThread);
}

void setCreatureCalibHints(const CreatureCalibHints& hints) {
    std::lock_guard<std::mutex> lock(g_mtx);
    g_hints = hints;
    LOG("creature hints name=%s z=%d hasZ=%d hpperc=%d id=%u", hints.name.c_str(), hints.z,
        hints.hasZ ? 1 : 0, hints.hpperc, hints.id);
}

std::string creatureCalibMessage() {
    std::lock_guard<std::mutex> lock(g_mtx);
    return g_msg;
}

bool runCreatureScan() {
    CreatureCalibHints hints;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        hints = g_hints;
        g_msg = "scanning for visible creatures";
    }
    if (hints.name.empty()) {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_msg = "hints need creatureName (monster on screen)";
        return false;
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
    Profile profile;
    copyLiveProfile(profile);
    if (hints.hasZ) {
        if (hints.z < 0 || hints.z > 15) {
            std::lock_guard<std::mutex> lock(g_mtx);
            g_msg = "creature z hint must be between 0 and 15";
            return false;
        }
        player.position = unknownPlayerPosition(hints.z);
        player.hasPosition = true;
        LOG("P3 scan using explicit floor z=%d; ignoring unproven player x/y", hints.z);
    } else if (!player.hasPosition) {
        if (!profile.hasPosition) {
            std::lock_guard<std::mutex> lock(g_mtx);
            g_msg = "need player position once for this .exe (p1-calibrate --refine --z)";
            return false;
        }
        LOG("P3 scan without live player tile; using name-only filter");
        player.position = unknownPlayerPosition();
    }

    const std::uintptr_t playerObj = currentPlayerObject();
    const std::uintptr_t statsBase = currentStatsBase();

    Needle existing;
    std::string nearby;
    const bool reuse = tryExistingLayout(hints, profile, player.position, existing, nearby);

    const DWORD t0 = GetTickCount();
    ScanStats st;
    std::vector<Needle> needles;
    std::vector<TypeNameCandidate> typeCandidates;
    if (reuse) {
        needles.push_back(existing);
        LOG("P3 reused creature layout nameOff=%d", existing.layout.nameOffset);
    } else {
        needles = findNeedles(hints, player.position, profile, playerObj, statsBase, t0, st,
                              typeCandidates);
        if (needles.empty() && !typeCandidates.empty()) {
            LOG("P3 direct-name miss; type-name fallback candidates=%zu", typeCandidates.size());
            needles = findNeedlesFromType(hints, player.position, profile, playerObj, statsBase,
                                          std::move(typeCandidates), GetTickCount(), st);
        }
    }
    if (needles.empty()) {
        std::string msg = "no creature named '" + hints.name + "' near the player (" +
                          formatScanStats(st) + ")";
        if (!nearby.empty())
            msg += "; nearby: " + nearby;
        std::lock_guard<std::mutex> lock(g_mtx);
        g_msg = msg;
        LOG("creature scan miss name=%s %s nearby=%s", hints.name.c_str(),
            formatScanStats(st).c_str(), nearby.c_str());
        return false;
    }
    LOG("creature needles=%zu obj=0x%p nameOff=%d idOff=%d hpOff=%d posOff=%d", needles.size(),
        reinterpret_cast<void*>(needles[0].obj), needles[0].layout.nameOffset,
        needles[0].layout.idOffset, needles[0].layout.hppercOffset,
        needles[0].layout.positionOffset);

    CreatureLayout layout = needles[0].layout;
    layout.present = true;
    layout.maxCount = kMaxCreatures;
    if (!reuse)
        seedMapLayout(layout, profile);
    const std::uintptr_t needle = needles[0].obj;

    FoundCol best;
    if (reuse && !layout.chain.empty()) {
        best.base = resolveChain(layout.chain);
        best.layout = layout;
        best.score = 2000;
    }

    if (best.score < 1000 && profile.containers.present && !profile.containers.chain.empty()) {
        const std::uintptr_t cbase = resolveChain(profile.containers.chain);
        LOG("creature near-scan containers=0x%p", reinterpret_cast<void*>(cbase));
        scanAround(cbase, kNearScanRange, layout, needle, playerObj, hints.name, best);
    }
    if (!profile.chain.empty()) {
        const std::uintptr_t lpField =
            moduleBase() + static_cast<std::uintptr_t>(
                               static_cast<std::intptr_t>(profile.chain[0].offset));
        LOG("creature near-scan localPlayerField=0x%p score=%d",
            reinterpret_cast<void*>(lpField), best.score);
        scanAround(lpField, kNearScanRange, layout, needle, playerObj, hints.name, best);
    }

    const DWORD tPtr = GetTickCount();
    std::vector<std::uintptr_t> ptrHits;
    if (best.score < 1000)
        findPointersTo(needle, ptrHits, tPtr);

    int mapProbes = 0;
    int umapProbes = 0;
    std::vector<CollectionProbe> mapCandidates;
    std::vector<CollectionProbe> umapCandidates;
    const DWORD tProbe = GetTickCount();
    for (auto hit : ptrHits) {
        if (GetTickCount() - tProbe > kScanBudgetMs)
            break;
        CreatureLayout nodeLay = layout;
        if (probeMapNode(hit, needle, 0, nodeLay)) {
            ++mapProbes;
            const std::uintptr_t sentinel = walkToSentinel(
                hit - static_cast<std::uintptr_t>(nodeLay.mapNodePtrOffset), nodeLay);
            if (sentinel) {
                nodeLay.collection = "map";
                addCollectionProbe(mapCandidates, sentinel, nodeLay);
            }
        }
        CreatureLayout umapLay = layout;
        if (probeUMapNode(hit, needle, 0, umapLay)) {
            ++umapProbes;
            std::vector<std::pair<std::uint32_t, std::uintptr_t>> nodes;
            std::uintptr_t sentinel = 0;
            const std::uintptr_t start =
                hit - static_cast<std::uintptr_t>(umapLay.listPtrOffset);
            if (walkUMapList(start, umapLay, nodes, sentinel) && sentinel) {
                umapLay.collection = "umap";
                addCollectionProbe(umapCandidates, sentinel, umapLay);
            }
        }
    }

    LOG("creature collection probes map=%zu umap=%zu ptrHits=%zu", mapCandidates.size(),
        umapCandidates.size(), ptrHits.size());
    scanModuleCollectionProbes(mapCandidates, needle, playerObj, hints.name, best);
    if (best.score < 1000)
        scanModuleCollectionProbes(umapCandidates, needle, playerObj, hints.name, best);
    if (best.score < 0)
        scanWritableCollectionProbes(mapCandidates, umapCandidates, needle, playerObj,
                                     hints.name, best);

    if (best.score < 0) {
        const DWORD tResolve = GetTickCount();
        for (const auto& candidate : mapCandidates) {
            if (GetTickCount() - tResolve > kScanBudgetMs)
                break;
            const std::uintptr_t mapObj =
                findMapObject(candidate.sentinel, candidate.layout, needle, tResolve);
            if (!mapObj)
                continue;
            const int score =
                scoreRead(mapObj, candidate.layout, needle, playerObj, hints.name);
            if (score >= 0)
                considerCol(best, mapObj, candidate.layout, score);
        }
        for (const auto& candidate : umapCandidates) {
            if (GetTickCount() - tResolve > kScanBudgetMs)
                break;
            const std::uintptr_t mapObj =
                findUMapObject(candidate.sentinel, candidate.layout, needle, tResolve);
            if (!mapObj)
                continue;
            const int score =
                scoreRead(mapObj, candidate.layout, needle, playerObj, hints.name);
            if (score >= 0)
                considerCol(best, mapObj, candidate.layout, score);
        }
    }

    if (best.score < 0 || !best.base) {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_msg = "found creature object but not the visible-creature list (ptrHits=" +
                std::to_string(ptrHits.size()) + " mapProbe=" + std::to_string(mapProbes) +
                " umapProbe=" + std::to_string(umapProbes) + " mapCand=" +
                std::to_string(mapCandidates.size()) + " umapCand=" +
                std::to_string(umapCandidates.size()) + ")";
        LOG("creature collection miss ptrHits=%zu mapProbe=%d umapProbe=%d mapCand=%zu "
            "umapCand=%zu",
            ptrHits.size(), mapProbes, umapProbes, mapCandidates.size(), umapCandidates.size());
        return false;
    }

    layout = best.layout;
    layout.present = true;
    const bool idCorrected = fixIdFromCollection(best.base, layout, needle);
    if (idCorrected) {
        if (!discoverHpperc(needle, layout.nameOffset, hints, playerObj, layout)) {
            if (hints.hasHpperc) {
                std::lock_guard<std::mutex> lock(g_mtx);
                g_msg = "creature id corrected but hpperc hint did not match";
                return false;
            }
            LOG("creature hpperc rediscovery miss after id correction; keeping provisional field");
        } else {
            discoverKindSkull(needle, layout.nameOffset, playerObj, layout);
            LOG("creature fields refined after id correction idOff=%d hpOff=%d hpSize=%d",
                layout.idOffset, layout.hppercOffset, layout.hppercSize);
        }
    }
    const DWORD tChain = GetTickCount();
    layout.chain = findChainTo(best.base, tChain);
    if (layout.chain.empty()) {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_msg = "no pointer-chain to creature collection";
        return false;
    }

    const std::uintptr_t resolved = resolveChain(layout.chain);
    std::vector<CreatureSnapshot> cur;
    bool readOk = resolved && readCollection(resolved, layout, cur, nullptr) && !cur.empty();
    bool named = false;
    for (const auto& c : cur) {
        if (nameMatches(c.name, hints.name))
            named = true;
    }
    if (resolved && hints.hasZ && (!readOk || !named)) {
        std::vector<CreatureSnapshot> calibrated;
        if (readCalibrationCollection(resolved, layout, playerObj, calibrated)) {
            cur = std::move(calibrated);
            readOk = !cur.empty();
            named = false;
            for (const auto& c : cur) {
                if (nameMatches(c.name, hints.name))
                    named = true;
            }
            LOG("creature final validation used P3 LocalPlayer anchor n=%zu named=%d",
                cur.size(), named ? 1 : 0);
        }
    }
    if (!readOk) {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_msg = "creature-chain landed but read failed";
        return false;
    }
    if (!named) {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_msg = "creature list did not contain '" + hints.name + "'";
        return false;
    }

    setCreatureLayout(layout);
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_layout = layout;
        g_live = cur;
        g_msg = "creature-chain resolved";
    }
    LOG("creature-chain resolved at 0x%p collection=%s n=%zu steps=%zu score=%d",
        reinterpret_cast<void*>(resolved), layout.collection.c_str(), cur.size(),
        layout.chain.size(), best.score);
    replayCreaturesToPipe();
    return true;
}

std::string creaturesJson() {
    std::lock_guard<std::mutex> lock(g_mtx);
    json arr = json::array();
    for (const auto& c : g_live)
        arr.push_back(creatureObject(c));
    return arr.dump();
}

std::vector<CreatureSnapshot> currentCreatures() {
    std::lock_guard<std::mutex> lock(g_mtx);
    return g_live;
}

bool readCreatureNameAt(std::uintptr_t addr, const CreatureLayout& layout, std::string& out) {
    return readCreatureName(addr, layout, out);
}

bool readFreshCreatures(std::vector<CreatureSnapshot>& out,
    std::vector<std::uint32_t>* removedZeroHp) {
    if (removedZeroHp) removedZeroHp->clear();
    CreatureLayout layout;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        layout = g_layout;
    }
    out.clear();
    if (!layout.present || layout.chain.empty()) return false;
    const auto base = resolveChain(layout.chain);
    if (!base) return false;
    std::vector<std::pair<std::uint32_t, std::uintptr_t>> nodes;
    if (layout.collection == "vector") {
        std::vector<std::uintptr_t> ptrs;
        if (!parseVectorPtrs(base, ptrSizeOf(layout), creatureStride(layout), 0, kMaxCreatures, ptrs))
            return false;
        for (auto ptr : ptrs) nodes.emplace_back(0, ptr);
    } else {
        const bool umap = layout.collection == "umap";
        if (!umap && layout.collection != "map") return false;
        std::uintptr_t size = 0, head = 0;
        if (!readPtr(base + (umap ? layout.umapSizeOffset : layout.mapSizeOffset), size) ||
            size > kMaxCreatures ||
            !readPtr(base + (umap ? layout.umapHeadOffset : layout.mapHeadOffset), head) || !head)
            return false;
        if (!size) {
            std::uintptr_t link = 0;
            // Validate empty sentinel explicitly: empty is a valid observation.
            return readPtr(head + (umap ? layout.listNextOffset : layout.mapNodeParentOffset), link) &&
                link == head;
        }
        if (!(umap ? walkUMap(base, layout, nodes) : walkMap(base, layout, nodes)) || nodes.size() != size)
            return false;
    }
    for (const auto& node : nodes) {
        CreatureSnapshot snapshot;
        if (!node.second || !readOneCreature(node.second, layout, snapshot)) {
            // knownCreatures retains removed objects. Their invalid Position
            // is not a failed collection read. Validate identity and fields
            // independently before excluding exactly that sentinel position.
            int x = 0, y = 0, z = 0;
            std::string name;
            const auto id = node.second ? readId(node.second, layout) : 0;
            const int hp = node.second ? readHpperc(node.second, layout) : -1;
            if (node.second && looksLikeCreatureObject(node.second) && id &&
                (!node.first || node.first == id) && hp >= 0 && hp <= 100 &&
                fillCreatureName(node.second, layout, name) &&
                readCreaturePos(node.second, layout, x, y, z) &&
                x == 65535 && y == 65535 && z == 255) {
                if (removedZeroHp && hp == 0) removedZeroHp->push_back(id);
                continue;
            }
            if (removedZeroHp) removedZeroHp->clear();
            out.clear(); return false;
        }
        if (node.first && looksLikeCreatureId(node.first)) snapshot.id = node.first;
        if (!snapshot.id) { out.clear(); return false; }
        out.push_back(std::move(snapshot));
    }
    finalizeCollection(layout, out, nullptr);
    return true;
}

void replayCreaturesToPipe() {
    std::vector<CreatureSnapshot> live;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        live = g_live;
    }
    for (const auto& c : live)
        PipeServer::instance().emit("creatureAppear", creatureObject(c).dump());
}

bool resolveCreature(std::uint32_t id, std::uintptr_t& ptr, std::uintptr_t& rep) {
    ptr = 0;
    rep = 0;
    if (!id)
        return false;
    CreatureLayout layout;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        layout = g_layout;
    }
    if (!layout.present || layout.chain.empty())
        return false;
    const std::uintptr_t base = resolveChain(layout.chain);
    std::vector<CreatureSp> hits;
    if (!base || !collectCreatureSp(base, layout, hits))
        return false;
    for (const auto& h : hits) {
        if (!matchCreatureSp(layout, h, id, 0, 0, 0, true, false))
            continue;
        ptr = h.ptr;
        rep = h.rep;
        if (!prepareCreatureHandle(layout, ptr, rep)) {
            LOG("creature handle id=%u bad width=%d rep=%p ptr=%p", id,
                creatureStride(layout), reinterpret_cast<void*>(h.rep),
                reinterpret_cast<void*>(ptr));
            ptr = 0;
            rep = 0;
            continue;
        }
        LOG("creature handle id=%u width=%d ptr=%p rep=%p mapRep=%p", id,
            creatureStride(layout), reinterpret_cast<void*>(ptr), reinterpret_cast<void*>(rep),
            reinterpret_cast<void*>(h.rep));
        return true;
    }
    return false;
}

bool resolveCreatureAt(int x, int y, int z, std::uintptr_t& ptr, std::uintptr_t& rep) {
    ptr = 0;
    rep = 0;
    CreatureLayout layout;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        layout = g_layout;
    }
    if (!layout.present || layout.chain.empty())
        return false;
    const std::uintptr_t base = resolveChain(layout.chain);
    std::vector<CreatureSp> hits;
    if (!base || !collectCreatureSp(base, layout, hits))
        return false;
    for (const auto& h : hits) {
        if (!matchCreatureSp(layout, h, 0, x, y, z, false, true))
            continue;
        ptr = h.ptr;
        rep = h.rep;
        if (!prepareCreatureHandle(layout, ptr, rep)) {
            LOG("creature handle at %d,%d,%d bad width=%d rep=%p ptr=%p", x, y, z,
                creatureStride(layout), reinterpret_cast<void*>(h.rep),
                reinterpret_cast<void*>(ptr));
            ptr = 0;
            rep = 0;
            continue;
        }
        LOG("creature handle at %d,%d,%d id=%u width=%d ptr=%p rep=%p", x, y, z, h.id,
            creatureStride(layout), reinterpret_cast<void*>(ptr), reinterpret_cast<void*>(rep));
        return true;
    }
    return false;
}

bool resolveCreatureByObject(std::uintptr_t obj, std::uintptr_t& ptr, std::uintptr_t& rep) {
    ptr = 0;
    rep = 0;
    if (!obj)
        return false;
    CreatureLayout layout;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        layout = g_layout;
    }
    if (!layout.present || layout.chain.empty())
        return false;
    const std::uintptr_t base = resolveChain(layout.chain);
    std::vector<CreatureSp> hits;
    if (!base || !collectCreatureSp(base, layout, hits))
        return false;
    const std::uint32_t selfId = readId(obj, layout);
    for (const auto& h : hits) {
        const bool sameObj = h.ptr == obj;
        const bool sameId = selfId && h.id == selfId;
        if (!sameObj && !sameId)
            continue;
        ptr = h.ptr;
        rep = h.rep;
        if (!prepareCreatureHandle(layout, ptr, rep)) {
            LOG("creature handle by object bad width=%d rep=%p ptr=%p id=%u",
                creatureStride(layout), reinterpret_cast<void*>(h.rep),
                reinterpret_cast<void*>(ptr), h.id);
            ptr = 0;
            rep = 0;
            continue;
        }
        LOG("creature handle by object width=%d ptr=%p rep=%p id=%u mapRep=%p selfObj=%p",
            creatureStride(layout), reinterpret_cast<void*>(ptr), reinterpret_cast<void*>(rep),
            h.id, reinterpret_cast<void*>(h.rep), reinterpret_cast<void*>(obj));
        return true;
    }
    LOG("useWith creature by object miss obj=%p selfId=%u n=%zu", reinterpret_cast<void*>(obj),
        selfId, hits.size());
    return false;
}

} // namespace otc

