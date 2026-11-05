#include "otc_api/container_reader.h"

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
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace otc {
namespace {

using nlohmann::json;

constexpr DWORD kPollMs = 250;
constexpr DWORD kScanBudgetMs = 20000;
constexpr int kMaxContainers = 16;
constexpr int kMaxItems = 60;
constexpr int kNearScanRange = 2048;

std::mutex g_mtx;
ContainerLayout g_layout;
ContainerCalibHints g_hints;
std::vector<ContainerSnapshot> g_open;
std::string g_msg = "containers not calibrated";
HANDLE g_pollThread = nullptr;
bool g_pollRun = false;

int ptrSizeOf(const ContainerLayout& l) {
    return l.ptrSize > 0 ? l.ptrSize : static_cast<int>(sizeof(void*));
}

int itemStride(const ContainerLayout& l) {
    if (l.itemPtrSize > 0)
        return l.itemPtrSize;
    return ptrSizeOf(l);
}

int dequeBlocks(const ContainerLayout& l) {
    if (l.dequeBlockSize > 0)
        return l.dequeBlockSize;
    const int es = itemStride(l);
    if (es <= 1)
        return 16;
    if (es <= 2)
        return 8;
    if (es <= 4)
        return 4;
    if (es <= 8)
        return 2;
    return 1;
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

bool inModule(std::uintptr_t p) {
    const std::uintptr_t mod = moduleBase();
    const std::size_t img = moduleImageSize();
    return img && p >= mod && p < mod + img;
}

bool looksLikeHeapObject(std::uintptr_t p) {
    if (p < 0x10000)
        return false;
    if (inModule(p))
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

bool looksLikeContainerObject(std::uintptr_t p) {
    return p && looksLikeHeapObject(p) && looksLikeVtable(p);
}

bool looksLikeItemId(std::uint16_t id) {
    return id >= 100 && id < 60000;
}

bool looksLikeItemObject(std::uintptr_t item) {
    return item != 0 && looksLikeHeapObject(item) && looksLikeVtable(item);
}

bool nameEq(const std::string& a, const std::string& b) {
    return a.size() == b.size() && _stricmp(a.c_str(), b.c_str()) == 0;
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

int readSized(std::uintptr_t addr, int size) {
    if (size == 1) {
        std::uint8_t v = 0;
        return memReadT(addr, v) ? v : 0;
    }
    if (size == 2) {
        std::uint16_t v = 0;
        return memReadT(addr, v) ? v : 0;
    }
    std::int32_t v = 0;
    return memReadT(addr, v) ? v : 0;
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

bool readContainerName(std::uintptr_t addr, const ContainerLayout& l, std::string& out) {
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
    if (readMsvcString(hit, got) && nameEq(got, expected)) {
        strObj = hit;
        kind = "msvc";
        return true;
    }
    const int ps = static_cast<int>(sizeof(void*));
    if (hit >= static_cast<std::uintptr_t>(ps * 2)) {
        const std::uintptr_t gccObj = hit - static_cast<std::uintptr_t>(ps * 2);
        std::uintptr_t p = 0;
        if (readPtr(gccObj, p) && p == hit && readGccString(gccObj, got) && nameEq(got, expected)) {
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
        if (readMsvcString(s, got) && nameEq(got, expected)) {
            strObj = s;
            kind = "msvc";
            return true;
        }
        if (readGccString(s, got) && nameEq(got, expected)) {
            strObj = s;
            kind = "gcc";
            return true;
        }
    }
    char z = 1;
    if (memReadT(hit + expected.size(), z) && z == 0 && readCString(hit, got) &&
        nameEq(got, expected)) {
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

bool readItem(std::uintptr_t item, const ContainerLayout& l, int& id, int& count) {
    id = 0;
    count = 1;
    if (!item)
        return false;
    std::uint16_t wid = 0;
    if (!memReadT(item + static_cast<std::uintptr_t>(l.itemIdOffset), wid) || !looksLikeItemId(wid))
        return false;
    std::uint8_t c = 0;
    memReadT(item + static_cast<std::uintptr_t>(l.itemCountOffset), c);
    id = wid;
    count = c > 0 ? static_cast<int>(c) : 1;
    return true;
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

bool parseDequePtrs(std::uintptr_t addr, int ps, int elemSize, int block, int minN, int maxN,
                    std::vector<std::uintptr_t>& out) {
    if (elemSize < 4)
        elemSize = ps;
    std::uintptr_t map = 0;
    std::size_t mapsize = 0;
    std::size_t myoff = 0;
    std::size_t mysize = 0;
    if (!readPtr(addr, map))
        return false;
    if (ps == 4) {
        std::uint32_t ms = 0, off = 0, sz = 0;
        if (!memReadT(addr + 4, ms) || !memReadT(addr + 8, off) || !memReadT(addr + 12, sz))
            return false;
        mapsize = ms;
        myoff = off;
        mysize = sz;
    } else {
        if (!memReadT(addr + 8, mapsize) || !memReadT(addr + 16, myoff) || !memReadT(addr + 24, mysize))
            return false;
    }
    if (mysize < static_cast<std::size_t>(minN) || mysize > static_cast<std::size_t>(maxN))
        return false;
    if (block < 1 || block > 16)
        return false;
    if (mysize == 0) {
        out.clear();
        return minN <= 0;
    }
    if (!map || !looksLikeHeapObject(map) || mapsize == 0 || mapsize > 256)
        return false;
    out.clear();
    out.reserve(mysize);
    for (std::size_t i = 0; i < mysize; ++i) {
        const std::size_t off = myoff + i;
        std::size_t b = off / static_cast<std::size_t>(block);
        if ((mapsize & (mapsize - 1)) == 0)
            b &= (mapsize - 1);
        else
            b %= mapsize;
        const std::size_t inner = off % static_cast<std::size_t>(block);
        std::uintptr_t blockPtr = 0;
        if (!readPtr(map + b * static_cast<std::size_t>(ps), blockPtr) || !blockPtr)
            return false;
        std::uintptr_t p = 0;
        if (!readPtr(blockPtr + inner * static_cast<std::size_t>(elemSize), p))
            return false;
        out.push_back(p);
    }
    return true;
}

bool readSharedAt(std::uintptr_t slotAddr, int elemSize, std::uintptr_t& ptr, std::uintptr_t& rep) {
    ptr = 0;
    rep = 0;
    if (!slotAddr || !readPtr(slotAddr, ptr) || !ptr)
        return false;
    if (elemSize >= static_cast<int>(sizeof(void*) * 2))
        readPtr(slotAddr + sizeof(void*), rep);
    return true;
}

bool dequeSharedAt(std::uintptr_t addr, int ps, int elemSize, int block, int slot, std::uintptr_t& ptr,
                   std::uintptr_t& rep) {
    if (elemSize < 4)
        elemSize = ps;
    std::uintptr_t map = 0;
    std::size_t mapsize = 0;
    std::size_t myoff = 0;
    std::size_t mysize = 0;
    if (!readPtr(addr, map))
        return false;
    if (ps == 4) {
        std::uint32_t ms = 0, off = 0, sz = 0;
        if (!memReadT(addr + 4, ms) || !memReadT(addr + 8, off) || !memReadT(addr + 12, sz))
            return false;
        mapsize = ms;
        myoff = off;
        mysize = sz;
    } else {
        if (!memReadT(addr + 8, mapsize) || !memReadT(addr + 16, myoff) || !memReadT(addr + 24, mysize))
            return false;
    }
    if (slot < 0 || static_cast<std::size_t>(slot) >= mysize)
        return false;
    if (block < 1 || block > 16 || !map || mapsize == 0 || mapsize > 256)
        return false;
    const std::size_t off = myoff + static_cast<std::size_t>(slot);
    std::size_t b = off / static_cast<std::size_t>(block);
    if ((mapsize & (mapsize - 1)) == 0)
        b &= (mapsize - 1);
    else
        b %= mapsize;
    const std::size_t inner = off % static_cast<std::size_t>(block);
    std::uintptr_t blockPtr = 0;
    if (!readPtr(map + b * static_cast<std::size_t>(ps), blockPtr) || !blockPtr)
        return false;
    return readSharedAt(blockPtr + inner * static_cast<std::size_t>(elemSize), elemSize, ptr, rep);
}

bool vectorSharedAt(std::uintptr_t addr, int ps, int elemSize, int slot, std::uintptr_t& ptr,
                    std::uintptr_t& rep) {
    if (elemSize < 4)
        elemSize = ps;
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
    if (!readPtr(addr, begin) || !readPtr(addr + ps, end) || end < begin)
        return false;
    const std::size_t bytes = static_cast<std::size_t>(end - begin);
    if (elemSize <= 0 || bytes % static_cast<std::size_t>(elemSize) != 0)
        return false;
    const int n = static_cast<int>(bytes / static_cast<std::size_t>(elemSize));
    if (slot < 0 || slot >= n)
        return false;
    return readSharedAt(begin + static_cast<std::uintptr_t>(slot * elemSize), elemSize, ptr, rep);
}

bool itemSharedAt(std::uintptr_t obj, const ContainerLayout& l, int slot, std::uintptr_t& ptr,
                  std::uintptr_t& rep) {
    const int ps = ptrSizeOf(l);
    const int es = itemStride(l);
    const std::uintptr_t addr = obj + static_cast<std::uintptr_t>(l.itemsOffset);
    if (l.itemsKind == "vector")
        return vectorSharedAt(addr, ps, es, slot, ptr, rep);
    if (dequeSharedAt(addr, ps, es, dequeBlocks(l), slot, ptr, rep))
        return true;
    return vectorSharedAt(addr, ps, es, slot, ptr, rep);
}

bool itemsLookValid(const std::vector<std::uintptr_t>& items, int minN) {
    int n = 0;
    for (auto p : items) {
        if (!p)
            continue;
        if (!looksLikeItemObject(p))
            return false;
        ++n;
    }
    return n >= minN;
}

bool pickItemFields(const std::vector<std::uintptr_t>& items, int& idOff, int& countOff) {
    int nThing = 0;
    for (auto p : items) {
        if (p)
            ++nThing;
    }
    if (nThing < 1)
        return false;
    int bestScore = -1;
    int bestId = 0;
    int bestCount = 4;
    for (int off = 8; off <= 96; off += 2) {
        int ok = 0;
        int unique = 0;
        std::uint16_t ids[64]{};
        bool bad = false;
        for (std::size_t i = 0; i < items.size() && i < 64; ++i) {
            if (!items[i])
                continue;
            std::uint16_t id = 0;
            if (!memReadT(items[i] + static_cast<std::uintptr_t>(off), id) || !looksLikeItemId(id)) {
                bad = true;
                break;
            }
            ids[i] = id;
            ++ok;
            bool seen = false;
            for (std::size_t j = 0; j < i; ++j) {
                if (ids[j] && ids[j] == id)
                    seen = true;
            }
            if (!seen)
                ++unique;
        }
        if (bad || ok != nThing)
            continue;
        if (nThing >= 2 && unique < 2)
            continue;
        const int tryCount[] = {off + 4, off + 2, off + 6, off + 8};
        int chosenCount = -1;
        int bestCs = -1;
        for (int t : tryCount) {
            int ones = 0;
            bool allSame = true;
            int first = -1;
            bool cok = true;
            for (auto p : items) {
                if (!p)
                    continue;
                std::uint8_t c = 0;
                if (!memReadT(p + static_cast<std::uintptr_t>(t), c) || c > 100) {
                    cok = false;
                    break;
                }
                const int n = c == 0 ? 1 : c;
                if (n == 1)
                    ++ones;
                if (first < 0)
                    first = n;
                else if (n != first)
                    allSame = false;
            }
            if (!cok)
                continue;
            if (nThing >= 3 && allSame && first > 1)
                continue;
            const int cs = ones * 20 + (allSame && first == 1 ? 15 : 0);
            if (cs > bestCs) {
                bestCs = cs;
                chosenCount = t;
            }
        }
        if (chosenCount < 0)
            continue;
        const int score = unique * 50 + bestCs + off / 4;
        if (score > bestScore) {
            bestScore = score;
            bestId = off;
            bestCount = chosenCount;
        }
    }
    if (bestScore < 0)
        return false;
    idOff = bestId;
    countOff = bestCount;
    return true;
}

bool itemHintOk(const std::vector<std::uintptr_t>& items, const ContainerLayout& l,
                const ContainerCalibHints& hints) {
    if (!hints.hasItemId)
        return true;
    for (auto p : items) {
        int id = 0;
        int count = 0;
        if (readItem(p, l, id, count) && id == hints.itemId)
            return true;
    }
    return false;
}

bool parseItemsAt(std::uintptr_t addr, ContainerLayout& l, int minN, std::vector<std::uintptr_t>& items) {
    const int ps = ptrSizeOf(l);
    const int es = itemStride(l);
    const int defBlock = dequeBlocks(l);
    const int tries[] = {l.dequeBlockSize > 0 ? l.dequeBlockSize : 0, defBlock, 4, 2, 8, 1};
    if (l.itemsKind != "vector") {
        for (int b : tries) {
            if (b <= 0)
                continue;
            if (parseDequePtrs(addr, ps, es, b, minN, kMaxItems, items) &&
                itemsLookValid(items, minN)) {
                l.itemsKind = "deque";
                l.dequeBlockSize = b;
                return true;
            }
        }
    }
    if (parseVectorPtrs(addr, ps, es, minN, kMaxItems, items) && itemsLookValid(items, minN)) {
        l.itemsKind = "vector";
        return true;
    }
    return false;
}

std::vector<std::uintptr_t> objectStartsFromName(std::uintptr_t nameAddr) {
    std::vector<std::uintptr_t> found;
    const int ps = static_cast<int>(sizeof(void*));
    for (int back = ps; back <= 384; back += ps) {
        if (nameAddr < static_cast<std::uintptr_t>(back))
            break;
        const std::uintptr_t a = nameAddr - static_cast<std::uintptr_t>(back);
        if (looksLikeVtable(a) && looksLikeHeapObject(a))
            found.push_back(a);
    }
    return found;
}

bool discoverFields(std::uintptr_t obj, std::uintptr_t nameAddr, const ContainerCalibHints& hints,
                    const Profile& profile, ContainerLayout& l, std::vector<std::uintptr_t>& items) {
    l.ptrSize = static_cast<int>(sizeof(void*));
    const int ps = l.ptrSize;
    const int nameOff = static_cast<int>(nameAddr - obj);
    if (nameOff < 8 || nameOff > 384)
        return false;
    l.nameOffset = nameOff;
    if (profile.inventory.present && profile.inventory.ptrSize > 0)
        l.itemPtrSize = profile.inventory.ptrSize;
    if (profile.inventory.present) {
        l.itemIdOffset = profile.inventory.itemIdOffset;
        l.itemCountOffset = profile.inventory.itemCountOffset;
    }

    int bestCapOff = -1;
    int bestCapSize = 4;
    int bestScore = -1;
    for (int back = 1; back <= 64; ++back) {
        const int off = nameOff - back;
        if (off < 8)
            break;
        if (hints.capacity > 0 && hints.capacity <= 255) {
            std::uint8_t u8 = 0;
            if (memReadT(obj + static_cast<std::uintptr_t>(off), u8) &&
                static_cast<int>(u8) == hints.capacity) {
                int score = 200 - back;
                std::uintptr_t item = 0;
                if (readPtr(obj + static_cast<std::uintptr_t>(nameOff - ps), item) &&
                    (item == 0 || looksLikeItemObject(item)))
                    score += 80;
                if (score > bestScore) {
                    bestScore = score;
                    bestCapOff = off;
                    bestCapSize = 1;
                }
            }
        }
        if (back >= 4 && (off % 4) == 0) {
            std::int32_t v = 0;
            if (memReadT(obj + static_cast<std::uintptr_t>(off), v) && v == hints.capacity) {
                int score = 240 - back;
                std::uintptr_t item = 0;
                if (readPtr(obj + static_cast<std::uintptr_t>(nameOff - ps), item) &&
                    (item == 0 || looksLikeItemObject(item)))
                    score += 80;
                if (off == nameOff - ps - 4)
                    score += 40;
                if (score > bestScore) {
                    bestScore = score;
                    bestCapOff = off;
                    bestCapSize = 4;
                }
            }
        }
    }
    if (bestCapOff < 0)
        return false;
    l.capacityOffset = bestCapOff;
    l.capacitySize = bestCapSize;

    if (bestCapSize == 4 && bestCapOff >= 12) {
        const int idOff = bestCapOff - 4;
        const int id = readSized(obj + static_cast<std::uintptr_t>(idOff), 4);
        if (id >= 0 && id < kMaxContainers) {
            l.idOffset = idOff;
            l.idSize = 4;
        }
    } else if (bestCapSize == 1 && bestCapOff >= 9) {
        const int idOff = bestCapOff - 1;
        const int id = readSized(obj + static_cast<std::uintptr_t>(idOff), 1);
        if (id >= 0 && id < kMaxContainers) {
            l.idOffset = idOff;
            l.idSize = 1;
        }
    }

#if defined(_WIN64)
    const int tryElem[] = {l.itemPtrSize > 0 ? l.itemPtrSize : 0, 16, 8, 4};
#else
    const int tryElem[] = {l.itemPtrSize > 0 ? l.itemPtrSize : 0, 8, 4};
#endif
    bool itemsOk = false;
    for (int es : tryElem) {
        if (es <= 0)
            continue;
        l.itemPtrSize = es;
        for (int off = 8; off <= 400; off += 4) {
            if (off == nameOff)
                continue;
            ContainerLayout trial = l;
            std::vector<std::uintptr_t> cand;
            if (!parseItemsAt(obj + static_cast<std::uintptr_t>(off), trial, 1, cand))
                continue;
            l.itemsOffset = off;
            l.itemsKind = trial.itemsKind;
            l.dequeBlockSize = trial.dequeBlockSize;
            l.itemPtrSize = es;
            items = std::move(cand);
            itemsOk = true;
            break;
        }
        if (itemsOk)
            break;
    }
    if (!itemsOk)
        return false;

    if (profile.inventory.present) {
        l.itemIdOffset = profile.inventory.itemIdOffset;
        l.itemCountOffset = profile.inventory.itemCountOffset;
        int id = 0, count = 0;
        bool ok = false;
        for (auto p : items) {
            if (readItem(p, l, id, count)) {
                ok = true;
                break;
            }
        }
        if (!ok && !pickItemFields(items, l.itemIdOffset, l.itemCountOffset))
            return false;
    } else if (!pickItemFields(items, l.itemIdOffset, l.itemCountOffset)) {
        return false;
    }
    return itemHintOk(items, l, hints);
}

struct Needle {
    std::uintptr_t obj = 0;
    ContainerLayout layout;
};

struct ScanStats {
    int byteHits = 0;
    int strOk = 0;
    int objOk = 0;
    int capFail = 0;
    int itemsFail = 0;
    int fieldOk = 0;
};

std::string formatScanStats(const ScanStats& st) {
    std::string s = "bytes=";
    s += std::to_string(st.byteHits);
    s += " str=";
    s += std::to_string(st.strOk);
    s += " obj=";
    s += std::to_string(st.objOk);
    s += " items=";
    s += std::to_string(st.fieldOk);
    if (st.capFail)
        s += " capFail=" + std::to_string(st.capFail);
    if (st.itemsFail)
        s += " itemsFail=" + std::to_string(st.itemsFail);
    return s;
}

std::vector<Needle> findNeedles(const ContainerCalibHints& hints, const Profile& profile, DWORD t0,
                                ScanStats& st) {
    std::vector<Needle> found;
    if (hints.name.empty() || hints.capacity < 1)
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
                const std::size_t take = n - off > kChunk + name.size() ? kChunk + name.size() : n - off;
                std::vector<char> buf(take);
                if (!memCopy(begin + off, buf.data(), take)) {
                    off += kChunk;
                    continue;
                }
                for (std::size_t i = 0; i + name.size() <= take; ++i) {
                    if (!ciEqualBytes(buf.data() + i, name))
                        continue;
                    ++st.byteHits;
                    const std::uintptr_t hit = reinterpret_cast<std::uintptr_t>(begin + off + i);
                    std::uintptr_t strObj = 0;
                    std::string kind;
                    if (!interpretNameHit(hit, name, strObj, kind))
                        continue;
                    ++st.strOk;
                    const auto starts = objectStartsFromName(strObj);
                    bool added = false;
                    for (const auto obj : starts) {
                        if (!obj)
                            continue;
                        ++st.objOk;
                        Needle nd;
                        nd.obj = obj;
                        nd.layout.nameKind = kind;
                        std::vector<std::uintptr_t> items;
                        if (!discoverFields(obj, strObj, hints, profile, nd.layout, items)) {
                            const int nameOff = static_cast<int>(strObj - obj);
                            if (nameOff >= 8 && nameOff <= 384)
                                ++st.itemsFail;
                            else
                                ++st.capFail;
                            continue;
                        }
                        nd.layout.nameKind = kind;
                        ++st.fieldOk;
                        found.push_back(nd);
                        added = true;
                        break;
                    }
                    if (added && found.size() >= 8)
                        break;
                }
                if (found.size() >= 8)
                    break;
                if (take <= name.size())
                    break;
                off += take - (name.size() - 1);
            }
        }
        if (found.size() >= 8)
            break;
        if (next <= addr)
            break;
        addr = next;
    }
    if (found.size() > 1) {
        std::sort(found.begin(), found.end(), [](const Needle& a, const Needle& b) {
            return a.layout.nameOffset < b.layout.nameOffset;
        });
    }
    return found;
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
            for (std::size_t off = 0; off + static_cast<std::size_t>(ps) <= n; off += static_cast<std::size_t>(ps)) {
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

bool nodeIsNil(std::uintptr_t node, const ContainerLayout& l) {
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

bool probeMapNode(std::uintptr_t valueAddr, std::uintptr_t container, ContainerLayout& l) {
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
        if (!readPtr(node + static_cast<std::uintptr_t>(ptrOff), stored) || stored != container)
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
            const int keyTry[] = {ptrOff - 4, ptrOff - 8, ptrOff - ps};
            for (int keyOff : keyTry) {
                if (keyOff < ps * 3)
                    continue;
                const int key = readSized(node + static_cast<std::uintptr_t>(keyOff), 4);
                if (key < 0 || key >= kMaxContainers)
                    continue;
                l.mapNodeLeftOffset = 0;
                l.mapNodeParentOffset = ps;
                l.mapNodeRightOffset = ps * 2;
                l.mapNodeIsnilOffset = isnilOff;
                l.mapNodePtrOffset = ptrOff;
                l.mapNodeKeyOffset = keyOff;
                return true;
            }
        }
    }
    return false;
}

std::uintptr_t walkToSentinel(std::uintptr_t node, const ContainerLayout& l) {
    for (int i = 0; i < 32; ++i) {
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

void walkMapNodes(std::uintptr_t node, const ContainerLayout& l, std::set<std::uintptr_t>& seen,
                  std::vector<std::pair<int, std::uintptr_t>>& out) {
    if (!node || seen.size() > 32 || seen.count(node))
        return;
    seen.insert(node);
    if (nodeIsNil(node, l))
        return;
    walkMapNodes(nodePtr(node, l.mapNodeLeftOffset), l, seen, out);
    const int key = readSized(node + static_cast<std::uintptr_t>(l.mapNodeKeyOffset), 4);
    std::uintptr_t p = 0;
    readPtr(node + static_cast<std::uintptr_t>(l.mapNodePtrOffset), p);
    if (p && key >= 0 && key < kMaxContainers)
        out.push_back({key, p});
    walkMapNodes(nodePtr(node, l.mapNodeRightOffset), l, seen, out);
}

bool walkMap(std::uintptr_t mapObj, const ContainerLayout& l,
             std::vector<std::pair<int, std::uintptr_t>>& out) {
    std::uintptr_t head = 0;
    if (!readPtr(mapObj + static_cast<std::uintptr_t>(l.mapHeadOffset), head) || !head)
        return false;
    const std::uintptr_t root = nodePtr(head, l.mapNodeParentOffset);
    std::set<std::uintptr_t> seen;
    out.clear();
    walkMapNodes(root, l, seen, out);
    return true;
}

bool mapContains(std::uintptr_t mapObj, const ContainerLayout& l, std::uintptr_t container) {
    std::vector<std::pair<int, std::uintptr_t>> nodes;
    if (!walkMap(mapObj, l, nodes))
        return false;
    for (const auto& n : nodes) {
        if (n.second == container)
            return true;
    }
    return false;
}

std::uintptr_t findMapObject(std::uintptr_t sentinel, const ContainerLayout& l,
                             const std::vector<Needle>& needles, DWORD t0) {
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
        if (sz < 1 || sz > static_cast<std::size_t>(kMaxContainers))
            continue;
        ContainerLayout trial = l;
        trial.mapHeadOffset = 0;
        trial.mapSizeOffset = ps;
        if (!mapContains(addr, trial, needles[0].obj))
            continue;
        int score = static_cast<int>(sz) * 10;
        if (inModule(addr))
            score += 1000;
        std::vector<std::pair<int, std::uintptr_t>> nodes;
        walkMap(addr, trial, nodes);
        for (const auto& nd : needles) {
            for (const auto& n : nodes) {
                if (n.second == nd.obj)
                    score += 200;
            }
        }
        if (score > bestScore) {
            bestScore = score;
            best = addr;
        }
    }
    return best;
}

bool probeUMapNode(std::uintptr_t valueAddr, std::uintptr_t container, ContainerLayout& l) {
    const int ps = static_cast<int>(sizeof(void*));
    const int ptrOffs[] = {ps * 2, ps * 2 + 4, ps * 3, ps * 4, 12, 16, 20, 24, 32};
    for (int ptrOff : ptrOffs) {
        if (ptrOff < ps * 2)
            continue;
        const std::uintptr_t node = valueAddr - static_cast<std::uintptr_t>(ptrOff);
        if ((node % static_cast<std::uintptr_t>(ps)) != 0)
            continue;
        std::uintptr_t stored = 0;
        if (!readPtr(node + static_cast<std::uintptr_t>(ptrOff), stored) || stored != container)
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
            const int key = readSized(node + static_cast<std::uintptr_t>(keyOff), 4);
            if (key < 0 || key >= kMaxContainers)
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

bool walkUMapList(std::uintptr_t startNode, const ContainerLayout& l,
                  std::vector<std::pair<int, std::uintptr_t>>& out, std::uintptr_t& sentinelOut) {
    out.clear();
    sentinelOut = 0;
    if (!startNode)
        return false;
    std::set<std::uintptr_t> seen;
    std::uintptr_t node = startNode;
    std::vector<std::uintptr_t> ring;
    for (int i = 0; i < kMaxContainers + 4; ++i) {
        if (!node || seen.count(node))
            break;
        seen.insert(node);
        ring.push_back(node);
        node = nodePtr(node, l.listNextOffset);
    }
    if (ring.size() < 2)
        return false;
    std::vector<std::pair<int, std::uintptr_t>> data;
    std::uintptr_t sentinel = 0;
    for (auto n : ring) {
        std::uintptr_t p = 0;
        readPtr(n + static_cast<std::uintptr_t>(l.listPtrOffset), p);
        const int key = readSized(n + static_cast<std::uintptr_t>(l.listKeyOffset), 4);
        if (p && looksLikeContainerObject(p) && key >= 0 && key < kMaxContainers)
            data.push_back({key, p});
        else if (!sentinel)
            sentinel = n;
    }
    if (data.empty())
        return false;
    out = std::move(data);
    sentinelOut = sentinel ? sentinel : startNode;
    return true;
}

bool walkUMap(std::uintptr_t mapObj, const ContainerLayout& l,
              std::vector<std::pair<int, std::uintptr_t>>& out) {
    std::uintptr_t head = 0;
    if (!readPtr(mapObj + static_cast<std::uintptr_t>(l.umapHeadOffset), head) || !head)
        return false;
    const std::uintptr_t first = nodePtr(head, l.listNextOffset);
    std::uintptr_t sentinel = 0;
    return walkUMapList(first && first != head ? first : head, l, out, sentinel);
}

std::uintptr_t findUMapObject(std::uintptr_t sentinel, const ContainerLayout& l,
                              const std::vector<Needle>& needles, DWORD t0) {
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
        if (sz < 1 || sz > static_cast<std::size_t>(kMaxContainers))
            continue;
        ContainerLayout trial = l;
        trial.umapHeadOffset = 0;
        trial.umapSizeOffset = ps;
        std::vector<std::pair<int, std::uintptr_t>> nodes;
        if (!walkUMap(addr, trial, nodes))
            continue;
        bool hasNeedle = false;
        for (const auto& nd : needles) {
            for (const auto& n : nodes) {
                if (n.second == nd.obj)
                    hasNeedle = true;
            }
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

std::vector<ChainStep> findChainTo(std::uintptr_t target, DWORD t0) {
    std::vector<ChainStep> found;
    const std::uintptr_t mod = moduleBase();
    const std::size_t img = moduleImageSize();
    if (!mod || !img || !target)
        return found;
    if (target >= mod && target < mod + img) {
        LOG("container collection in module rva=0x%X", static_cast<unsigned>(target - mod));
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
                        LOG("container collection ptr in module rva=0x%X",
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
            if (!memCopy(reinterpret_cast<const void*>(mid + static_cast<std::uintptr_t>(off)), &inner,
                         sizeof(inner)))
                continue;
            if (inner == target) {
                // mid+off holds a pointer to the collection, not the collection itself.
                const std::vector<ChainStep> candidate = {
                    {static_cast<std::int32_t>(hp.first - mod), true}, {off, true}};
                if (resolveChain(candidate) != target)
                    continue;
                LOG("container collection via heap rva=0x%X off=%d",
                    static_cast<unsigned>(hp.first - mod), off);
                return candidate;
            }
        }
    }
    LOG("container-chain miss for 0x%p", reinterpret_cast<void*>(target));
    return found;
}

bool readItems(std::uintptr_t obj, const ContainerLayout& l, std::vector<ItemSlot>& slots) {
    slots.clear();
    const int ps = ptrSizeOf(l);
    const int es = itemStride(l);
    std::vector<std::uintptr_t> ptrs;
    const std::uintptr_t addr = obj + static_cast<std::uintptr_t>(l.itemsOffset);
    bool ok = false;
    if (l.itemsKind == "vector")
        ok = parseVectorPtrs(addr, ps, es, 0, kMaxItems, ptrs);
    else
        ok = parseDequePtrs(addr, ps, es, dequeBlocks(l), 0, kMaxItems, ptrs);
    if (!ok) {
        if (l.itemsKind != "vector" && parseVectorPtrs(addr, ps, es, 0, kMaxItems, ptrs))
            ok = true;
    }
    if (!ok)
        return true;
    int slot = 0;
    for (auto p : ptrs) {
        ItemSlot it;
        it.slot = slot++;
        if (!p || !readItem(p, l, it.id, it.count) || !it.id)
            continue;
        slots.push_back(it);
    }
    return true;
}

bool readOneContainer(std::uintptr_t obj, int index, const ContainerLayout& l, ContainerSnapshot& out) {
    if (!obj || !looksLikeHeapObject(obj) || !looksLikeVtable(obj))
        return false;
    out = ContainerSnapshot{};
    out.index = index;
    const int id = readSized(obj + static_cast<std::uintptr_t>(l.idOffset), l.idSize);
    if (id >= 0 && id < kMaxContainers)
        out.index = id;
    out.capacity = readSized(obj + static_cast<std::uintptr_t>(l.capacityOffset), l.capacitySize);
    if (out.capacity < 1 || out.capacity > 256)
        return false;
    std::string name;
    if (readContainerName(obj + static_cast<std::uintptr_t>(l.nameOffset), l, name))
        out.name = name;
    readItems(obj, l, out.items);
    return true;
}

bool readCollection(std::uintptr_t base, const ContainerLayout& l, std::vector<ContainerSnapshot>& out) {
    out.clear();
    if (!base)
        return false;
    const int ps = ptrSizeOf(l);
    const int cstride = itemStride(l);
    if (l.collection == "array") {
        for (int i = 0; i < l.maxCount && i < kMaxContainers; ++i) {
            std::uintptr_t p = 0;
            if (!readPtr(base + static_cast<std::uintptr_t>(i * cstride), p) || !p)
                continue;
            ContainerSnapshot snap;
            if (readOneContainer(p, i, l, snap))
                out.push_back(std::move(snap));
        }
        return true;
    }
    if (l.collection == "vector") {
        std::vector<std::uintptr_t> ptrs;
        if (!parseVectorPtrs(base, ps, cstride, 0, kMaxContainers, ptrs))
            return false;
        for (int i = 0; i < static_cast<int>(ptrs.size()); ++i) {
            if (!ptrs[i])
                continue;
            ContainerSnapshot snap;
            if (readOneContainer(ptrs[i], i, l, snap))
                out.push_back(std::move(snap));
        }
        return true;
    }
    std::vector<std::pair<int, std::uintptr_t>> nodes;
    const bool walked = l.collection == "umap" ? walkUMap(base, l, nodes) : walkMap(base, l, nodes);
    if (!walked)
        return false;
    for (const auto& n : nodes) {
        ContainerSnapshot snap;
        if (readOneContainer(n.second, n.first, l, snap)) {
            snap.index = n.first;
            out.push_back(std::move(snap));
        }
    }
    return true;
}

bool findContainerObj(std::uintptr_t base, const ContainerLayout& l, int index, std::uintptr_t& objOut) {
    objOut = 0;
    if (!base)
        return false;
    const int ps = ptrSizeOf(l);
    const int cstride = itemStride(l);
    auto take = [&](std::uintptr_t p, int fallbackIndex) {
        if (!p)
            return false;
        ContainerSnapshot snap;
        if (!readOneContainer(p, fallbackIndex, l, snap))
            return false;
        if (snap.index != index)
            return false;
        objOut = p;
        return true;
    };
    if (l.collection == "array") {
        for (int i = 0; i < l.maxCount && i < kMaxContainers; ++i) {
            std::uintptr_t p = 0;
            if (!readPtr(base + static_cast<std::uintptr_t>(i * cstride), p) || !p)
                continue;
            if (take(p, i))
                return true;
        }
        return false;
    }
    if (l.collection == "vector") {
        std::vector<std::uintptr_t> ptrs;
        if (!parseVectorPtrs(base, ps, cstride, 0, kMaxContainers, ptrs))
            return false;
        for (int i = 0; i < static_cast<int>(ptrs.size()); ++i) {
            if (take(ptrs[i], i))
                return true;
        }
        return false;
    }
    std::vector<std::pair<int, std::uintptr_t>> nodes;
    const bool walked = l.collection == "umap" ? walkUMap(base, l, nodes) : walkMap(base, l, nodes);
    if (!walked)
        return false;
    for (const auto& n : nodes) {
        if (n.first == index && n.second) {
            objOut = n.second;
            return true;
        }
    }
    return false;
}

json containerObject(const ContainerSnapshot& c) {
    json j;
    j["index"] = c.index;
    j["name"] = c.name;
    j["capacity"] = c.capacity;
    json items = json::array();
    for (const auto& it : c.items) {
        items.push_back({{"slot", it.slot}, {"id", it.id}, {"count", it.count}});
    }
    j["items"] = items;
    return j;
}

bool sameContainer(const ContainerSnapshot& a, const ContainerSnapshot& b) {
    if (a.index != b.index || a.name != b.name || a.capacity != b.capacity ||
        a.items.size() != b.items.size())
        return false;
    for (std::size_t i = 0; i < a.items.size(); ++i) {
        if (a.items[i].id != b.items[i].id || a.items[i].count != b.items[i].count ||
            a.items[i].slot != b.items[i].slot)
            return false;
    }
    return true;
}

void emitDiff(const std::vector<ContainerSnapshot>& prev, const std::vector<ContainerSnapshot>& next) {
    auto findIdx = [](const std::vector<ContainerSnapshot>& v, int index) -> const ContainerSnapshot* {
        for (const auto& c : v) {
            if (c.index == index)
                return &c;
        }
        return nullptr;
    };
    for (const auto& c : next) {
        const auto* old = findIdx(prev, c.index);
        if (!old)
            PipeServer::instance().tryEmit("containerOpen", containerObject(c).dump());
        else if (!sameContainer(*old, c))
            PipeServer::instance().tryEmit("containerUpdate", containerObject(c).dump());
    }
    for (const auto& c : prev) {
        if (!findIdx(next, c.index)) {
            json j;
            j["index"] = c.index;
            j["name"] = c.name;
            j["capacity"] = c.capacity;
            j["items"] = json::array();
            PipeServer::instance().tryEmit("containerClose", j.dump());
        }
    }
}

bool tryArrayAround(std::uintptr_t slotAddr, ContainerLayout& l, std::uintptr_t& baseOut,
                    int stride) {
    if (stride < 4)
        stride = static_cast<int>(sizeof(void*));
    int bestN = 0;
    std::uintptr_t bestBase = 0;
    for (int back = 0; back < kMaxContainers; ++back) {
        const std::uintptr_t start = slotAddr - static_cast<std::uintptr_t>(back * stride);
        int n = 0;
        int hits = 0;
        for (int i = 0; i < kMaxContainers; ++i) {
            std::uintptr_t p = 0;
            if (!readPtr(start + static_cast<std::uintptr_t>(i * stride), p))
                break;
            if (!p) {
                ++n;
                continue;
            }
            if (!looksLikeContainerObject(p))
                break;
            ++n;
            ++hits;
        }
        if (hits >= 1 && n >= 1 && n > bestN) {
            bestN = n;
            bestBase = start;
        }
    }
    if (bestN < 1)
        return false;
    l.collection = "array";
    l.itemPtrSize = stride;
    l.maxCount = kMaxContainers;
    baseOut = bestBase;
    return true;
}

bool collectionHasObj(std::uintptr_t base, const ContainerLayout& l, std::uintptr_t obj) {
    if (!base || !obj)
        return false;
    const int ps = ptrSizeOf(l);
    const int stride = itemStride(l);
    if (l.collection == "array") {
        for (int i = 0; i < l.maxCount && i < kMaxContainers; ++i) {
            std::uintptr_t p = 0;
            if (readPtr(base + static_cast<std::uintptr_t>(i * stride), p) && p == obj)
                return true;
        }
        return false;
    }
    if (l.collection == "vector") {
        std::vector<std::uintptr_t> ptrs;
        if (!parseVectorPtrs(base, ps, stride, 0, kMaxContainers, ptrs))
            return false;
        for (auto p : ptrs) {
            if (p == obj)
                return true;
        }
        return false;
    }
    std::vector<std::pair<int, std::uintptr_t>> nodes;
    if (l.collection == "umap") {
        if (!walkUMap(base, l, nodes))
            return false;
    } else if (!walkMap(base, l, nodes)) {
        return false;
    }
    for (const auto& n : nodes) {
        if (n.second == obj)
            return true;
    }
    return false;
}

int scoreContainerRead(std::uintptr_t base, const ContainerLayout& l,
                       const std::vector<Needle>& needles, const std::string& expectName) {
    std::vector<ContainerSnapshot> cur;
    if (!readCollection(base, l, cur) || cur.empty())
        return -1;
    bool hasNeedle = false;
    for (const auto& nd : needles) {
        if (collectionHasObj(base, l, nd.obj))
            hasNeedle = true;
    }
    bool hasName = false;
    for (const auto& c : cur) {
        if (nameEq(c.name, expectName))
            hasName = true;
    }
    if (!hasNeedle && !hasName)
        return -1;
    int score = static_cast<int>(cur.size()) * 10;
    if (hasNeedle)
        score += 500;
    if (hasName)
        score += 200;
    if (inModule(base))
        score += 1000;
    return score;
}

struct FoundCol {
    std::uintptr_t base = 0;
    ContainerLayout layout;
    int score = -1;
};

void considerCol(FoundCol& best, std::uintptr_t base, const ContainerLayout& l, int score) {
    if (score > best.score) {
        best.score = score;
        best.base = base;
        best.layout = l;
    }
}

void seedCollectionLayout(ContainerLayout& l, const Profile& profile) {
    const int ps = ptrSizeOf(l);
    l.mapHeadOffset = 0;
    l.mapSizeOffset = ps;
    l.mapNodeLeftOffset = 0;
    l.mapNodeParentOffset = ps;
    l.mapNodeRightOffset = ps * 2;
    l.mapNodeIsnilOffset = ps * 3 + 1;
    l.mapNodeKeyOffset = ps * 4;
    l.mapNodePtrOffset = ps * 5;
    l.umapHeadOffset = 0;
    l.umapSizeOffset = ps;
    l.listNextOffset = 0;
    l.listPrevOffset = ps;
    l.listPtrOffset = ps * 3;
    l.listKeyOffset = ps * 2;
    if (profile.creatures.present) {
        const auto& c = profile.creatures;
        if (c.mapNodeKeyOffset)
            l.mapNodeKeyOffset = c.mapNodeKeyOffset;
        if (c.mapNodePtrOffset)
            l.mapNodePtrOffset = c.mapNodePtrOffset;
        l.mapNodeLeftOffset = c.mapNodeLeftOffset;
        l.mapNodeParentOffset = c.mapNodeParentOffset;
        l.mapNodeRightOffset = c.mapNodeRightOffset;
        l.mapNodeIsnilOffset = c.mapNodeIsnilOffset;
        if (c.listPtrOffset)
            l.listPtrOffset = c.listPtrOffset;
        if (c.listKeyOffset)
            l.listKeyOffset = c.listKeyOffset;
        l.listNextOffset = c.listNextOffset;
        l.listPrevOffset = c.listPrevOffset;
        if (c.creaturePtrSize > 0 && l.itemPtrSize <= 0)
            l.itemPtrSize = c.creaturePtrSize;
        if (c.ptrSize > 0)
            l.ptrSize = c.ptrSize;
    } else if (profile.inventory.present && profile.inventory.ptrSize > 0 && l.itemPtrSize <= 0) {
        l.itemPtrSize = profile.inventory.ptrSize;
    }
}

// Used only after the established collection/chain discovery fails. An old
// Container can remain alive through Lua references after its window closes.
// A writable module map must prove membership, not merely contain a matching
// name or an incidental pointer array on the heap.
bool probeModuleContainerMap(std::uintptr_t base, const ContainerLayout& l,
                             const std::vector<Needle>& needles,
                             const ContainerCalibHints& hints) {
    if (l.collection != "map" || ptrSizeOf(l) != sizeof(void*) || !inModule(base) ||
        l.mapHeadOffset < 0 || l.mapSizeOffset < 0)
        return false;
    const auto headSlot = base + static_cast<std::uintptr_t>(l.mapHeadOffset);
    const auto sizeSlot = base + static_cast<std::uintptr_t>(l.mapSizeOffset);
    if (!inModule(headSlot + sizeof(void*) - 1) ||
        !inModule(sizeSlot + sizeof(void*) - 1))
        return false;
    MEMORY_BASIC_INFORMATION page{};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(base), &page, sizeof(page)) ||
        !regionWritable(page))
        return false;
    std::uintptr_t head = 0;
    std::size_t size = 0;
    if (!readPtr(headSlot, head) || !memReadT(sizeSlot, size) ||
        size < 1 || size > kMaxContainers || !looksLikeHeapObject(head))
        return false;
    std::uint8_t nil = 0;
    if (!memReadT(head + l.mapNodeIsnilOffset, nil) || nil != 1)
        return false;
    const auto root = nodePtr(head, l.mapNodeParentOffset);
    if (!root || root == head)
        return false;

    std::set<std::uintptr_t> seen;
    std::uintptr_t first = 0, last = 0;
    int firstKey = kMaxContainers, lastKey = -1;
    bool matched = false;
    std::function<bool(std::uintptr_t, std::uintptr_t, int, int)> visit =
        [&](std::uintptr_t node, std::uintptr_t parent, int low, int high) {
            if (node == head)
                return true;
            if (!node || seen.size() >= size || !seen.insert(node).second ||
                !looksLikeHeapObject(node))
                return false;
            std::uint8_t nodeNil = 1;
            int key = -1;
            std::uintptr_t obj = 0, storedParent = 0, left = 0, right = 0;
            if (!memReadT(node + l.mapNodeIsnilOffset, nodeNil) || nodeNil != 0 ||
                !memReadT(node + l.mapNodeKeyOffset, key) || key <= low || key >= high ||
                !readPtr(node + l.mapNodeParentOffset, storedParent) || storedParent != parent ||
                !readPtr(node + l.mapNodeLeftOffset, left) ||
                !readPtr(node + l.mapNodeRightOffset, right) ||
                !readPtr(node + l.mapNodePtrOffset, obj))
                return false;
            if (key < firstKey) { firstKey = key; first = node; }
            if (key > lastKey) { lastKey = key; last = node; }
            // Some clients retain null mapped entries after closing a window.
            if (obj) {
                ContainerSnapshot snap;
                int containerId = 0;
                if ((l.idSize != 1 && l.idSize != 2 && l.idSize != 4) ||
                    !memCopy(reinterpret_cast<const void*>(obj + l.idOffset),
                             &containerId, static_cast<std::size_t>(l.idSize)) || containerId != key ||
                    !readOneContainer(obj, key, l, snap) || snap.index != key ||
                    !printableName(snap.name))
                    return false;
                std::vector<std::uintptr_t> items;
                const auto itemsAddr = obj + static_cast<std::uintptr_t>(l.itemsOffset);
                const bool parsed = l.itemsKind == "vector"
                    ? parseVectorPtrs(itemsAddr, ptrSizeOf(l), itemStride(l), 0, kMaxItems, items)
                    : parseDequePtrs(itemsAddr, ptrSizeOf(l), itemStride(l), dequeBlocks(l),
                                     0, kMaxItems, items);
                if (!parsed || !itemsLookValid(items, 0))
                    return false;
                for (auto item : items) {
                    int id = 0, count = 0;
                    if (!item || !readItem(item, l, id, count))
                        return false;
                }
                const bool isNeedle = std::any_of(needles.begin(), needles.end(),
                    [&](const Needle& needle) { return needle.obj == obj; });
                if (isNeedle && nameEq(snap.name, hints.name) &&
                    snap.capacity == hints.capacity && !items.empty() && itemHintOk(items, l, hints))
                    matched = true;
            }
            return visit(left, node, low, key) && visit(right, node, key, high);
        };
    if (!visit(root, head, -1, kMaxContainers) || seen.size() != size || !matched ||
        nodePtr(head, l.mapNodeLeftOffset) != first ||
        nodePtr(head, l.mapNodeRightOffset) != last)
        return false;
    std::uintptr_t finalHead = 0;
    std::size_t finalSize = 0;
    return readPtr(headSlot, finalHead) && finalHead == head &&
           memReadT(sizeSlot, finalSize) && finalSize == size &&
           nodePtr(head, l.mapNodeParentOffset) == root;
}

bool findModuleContainerMap(const Profile& profile, const std::vector<Needle>& needles,
                             const ContainerCalibHints& hints, FoundCol& out) {
    const auto mod = moduleBase();
    const auto end = mod + moduleImageSize();
    const DWORD started = GetTickCount();
    constexpr DWORD budgetMs = 4000;
    constexpr std::size_t chunkSize = 65536;
    std::vector<std::uintptr_t> words(chunkSize / sizeof(void*));
    for (std::uintptr_t addr = mod; addr < end;) {
        if (GetTickCount() - started > budgetMs)
            break;
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)))
            break;
        const auto next = (std::min<std::uintptr_t>)(end,
            reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize);
        if (next <= addr)
            break;
        if (regionWritable(mbi)) {
            for (auto from = addr; from < next;) {
                const auto bytes = (std::min)(chunkSize, static_cast<std::size_t>(next - from));
                if (GetTickCount() - started > budgetMs)
                    return false;
                if (memCopy(reinterpret_cast<const void*>(from), words.data(), bytes)) {
                    const auto count = bytes / sizeof(void*);
                    for (std::size_t i = 0; i + 1 < count; ++i) {
                        if ((i & 255) == 0 && GetTickCount() - started > budgetMs)
                            return false;
                        if (words[i] < 0x10000 || words[i + 1] < 1 ||
                            words[i + 1] > kMaxContainers || inModule(words[i]))
                            continue;
                        const auto candidate = from + i * sizeof(void*);
                        for (const auto& needle : needles) {
                            ContainerLayout trial = needle.layout;
                            seedCollectionLayout(trial, profile);
                            trial.collection = "map";
                            trial.present = true;
                            trial.maxCount = kMaxContainers;
                            if (!probeModuleContainerMap(candidate, trial, needles, hints))
                                continue;
                            if (candidate - mod > 0x7FFFFFFF)
                                continue;
                            trial.chain = {{static_cast<std::int32_t>(candidate - mod), false}};
                            if (resolveChain(trial.chain) != candidate)
                                continue;
                            out = {candidate, trial, 0};
                            LOG("container module-map fallback rva=0x%X needles=%zu keyOff=%d ptrOff=%d",
                                static_cast<unsigned>(candidate - mod), needles.size(),
                                trial.mapNodeKeyOffset, trial.mapNodePtrOffset);
                            return true;
                        }
                    }
                }
                if (bytes <= sizeof(void*))
                    break;
                from += bytes - sizeof(void*); // header may cross a chunk boundary
            }
        }
        addr = next;
    }
    return false;
}

void tryVectorAt(std::uintptr_t addr, ContainerLayout seeded, const std::vector<Needle>& needles,
                 const std::string& name, FoundCol& best) {
    const int ps = ptrSizeOf(seeded);
#if defined(_WIN64)
    const int tries[] = {seeded.itemPtrSize > 0 ? seeded.itemPtrSize : 0, 16, 8, 4};
#else
    const int tries[] = {seeded.itemPtrSize > 0 ? seeded.itemPtrSize : 0, 8, 4};
#endif
    for (int es : tries) {
        if (es <= 0)
            continue;
        ContainerLayout l = seeded;
        l.itemPtrSize = es;
        std::vector<std::uintptr_t> ptrs;
        if (!parseVectorPtrs(addr, ps, es, 1, kMaxContainers, ptrs))
            continue;
        bool has = false;
        for (auto p : ptrs) {
            if (p && looksLikeContainerObject(p)) {
                for (const auto& nd : needles) {
                    if (p == nd.obj)
                        has = true;
                }
            }
        }
        if (!has)
            continue;
        l.collection = "vector";
        const int score = scoreContainerRead(addr, l, needles, name);
        if (score >= 0)
            considerCol(best, addr, l, score);
    }
}

void tryLayoutsAt(std::uintptr_t addr, ContainerLayout seeded, const std::vector<Needle>& needles,
                  const std::string& name, FoundCol& best) {
    std::uintptr_t head = 0;
    if (!readPtr(addr, head) || !head)
        return;
    if (!looksLikeHeapObject(head) && !inModule(head))
        return;

    ContainerLayout mapLay = seeded;
    mapLay.collection = "map";
    int score = scoreContainerRead(addr, mapLay, needles, name);
    if (score >= 0)
        considerCol(best, addr, mapLay, score);
    if (seeded.itemPtrSize == 8 && seeded.mapNodePtrOffset == seeded.mapNodeKeyOffset + 4) {
        ContainerLayout aligned = mapLay;
        aligned.mapNodePtrOffset = seeded.mapNodeKeyOffset + 8;
        score = scoreContainerRead(addr, aligned, needles, name);
        if (score >= 0)
            considerCol(best, addr, aligned, score);
    }

    const int ps = ptrSizeOf(seeded);
    const int umapPtr[] = {ps * 3, ps * 4, ps * 2 + 4, 12, 16, 20};
    for (int ptrOff : umapPtr) {
        if (ptrOff < ps * 2 + 4)
            continue;
        ContainerLayout u = seeded;
        u.collection = "umap";
        u.umapHeadOffset = 0;
        u.umapSizeOffset = ps;
        u.listNextOffset = 0;
        u.listPrevOffset = ps;
        u.listPtrOffset = ptrOff;
        u.listKeyOffset = ptrOff - 4;
        score = scoreContainerRead(addr, u, needles, name);
        if (score >= 0)
            considerCol(best, addr, u, score);
    }
    tryVectorAt(addr, seeded, needles, name, best);
}

void scanAround(std::uintptr_t center, int range, const ContainerLayout& seeded,
                const std::vector<Needle>& needles, const std::string& name, FoundCol& best) {
    if (!center)
        return;
    const int ps = ptrSizeOf(seeded);
    for (int off = -range; off <= range; off += ps) {
        const std::intptr_t a = static_cast<std::intptr_t>(center) + off;
        if (a <= 0x10000)
            continue;
        tryLayoutsAt(static_cast<std::uintptr_t>(a), seeded, needles, name, best);
    }
}

DWORD WINAPI pollThread(LPVOID) {
    while (g_pollRun) {
        ContainerLayout layout;
        {
            std::lock_guard<std::mutex> lock(g_mtx);
            layout = g_layout;
        }
        if (!layout.present || layout.chain.empty()) {
            Sleep(kPollMs);
            continue;
        }
        const std::uintptr_t base = resolveChain(layout.chain);
        std::vector<ContainerSnapshot> next;
        if (base)
            readCollection(base, layout, next);
        std::vector<ContainerSnapshot> prev;
        {
            std::lock_guard<std::mutex> lock(g_mtx);
            prev = g_open;
            g_open = next;
        }
        emitDiff(prev, next);
        Sleep(kPollMs);
    }
    return 0;
}

} // namespace

void startContainerReader() {
    Profile p;
    copyLiveProfile(p);
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_layout = p.containers;
        if (g_layout.present)
            g_msg = "container layout loaded";
        else
            g_msg = "open a backpack and run p2-calibrate";
    }
    if (g_layout.present && !g_layout.chain.empty()) {
        const std::uintptr_t base = resolveChain(g_layout.chain);
        if (base) {
            std::vector<ContainerSnapshot> cur;
            readCollection(base, g_layout, cur);
            std::lock_guard<std::mutex> lock(g_mtx);
            g_open = std::move(cur);
            g_msg = "container-chain resolved";
            LOG("container-chain resolved at 0x%p collection=%s open=%zu",
                reinterpret_cast<void*>(base), g_layout.collection.c_str(), g_open.size());
        } else {
            LOG("container-chain failed (%zu steps)", g_layout.chain.size());
        }
    }

    g_pollRun = true;
    g_pollThread = CreateThread(nullptr, 0, pollThread, nullptr, 0, nullptr);
    if (g_pollThread)
        CloseHandle(g_pollThread);
}

void setContainerCalibHints(const ContainerCalibHints& hints) {
    std::lock_guard<std::mutex> lock(g_mtx);
    g_hints = hints;
    LOG("container hints name=%s capacity=%d itemId=%d", hints.name.c_str(), hints.capacity,
        hints.itemId);
}

std::string containerCalibMessage() {
    std::lock_guard<std::mutex> lock(g_mtx);
    return g_msg;
}

bool runContainerScan() {
    ContainerCalibHints hints;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        hints = g_hints;
        g_msg = "scanning for open containers";
    }
    if (hints.name.empty() || hints.capacity < 1) {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_msg = "hints need containerName and containerCapacity";
        return false;
    }

    Profile profile;
    copyLiveProfile(profile);

    const DWORD t0 = GetTickCount();
    ScanStats st;
    auto needles = findNeedles(hints, profile, t0, st);
    if (needles.empty()) {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_msg = "no container object named '" + hints.name + "' with capacity " +
                std::to_string(hints.capacity) + " (" + formatScanStats(st) + ")";
        LOG("container scan miss name=%s cap=%d %s", hints.name.c_str(), hints.capacity,
            formatScanStats(st).c_str());
        return false;
    }
    LOG("container needles=%zu obj=0x%p nameOff=%d capOff=%d itemsOff=%d", needles.size(),
        reinterpret_cast<void*>(needles[0].obj), needles[0].layout.nameOffset,
        needles[0].layout.capacityOffset, needles[0].layout.itemsOffset);

    ContainerLayout layout = needles[0].layout;
    layout.present = true;
    layout.maxCount = kMaxContainers;
    seedCollectionLayout(layout, profile);

    FoundCol best;
    if (!profile.chain.empty()) {
        const std::uintptr_t lpField =
            moduleBase() + static_cast<std::uintptr_t>(
                               static_cast<std::intptr_t>(profile.chain[0].offset));
        LOG("container near-scan localPlayerField=0x%p", reinterpret_cast<void*>(lpField));
        scanAround(lpField, kNearScanRange, layout, needles, hints.name, best);
    }
    if (profile.creatures.present && !profile.creatures.chain.empty()) {
        const std::uintptr_t cbase = resolveChain(profile.creatures.chain);
        LOG("container near-scan creatures=0x%p score=%d", reinterpret_cast<void*>(cbase),
            best.score);
        scanAround(cbase, kNearScanRange, layout, needles, hints.name, best);
    }

    const DWORD tPtr = GetTickCount();
    std::vector<std::uintptr_t> ptrHits;
    if (best.score < 1000)
        findPointersTo(needles[0].obj, ptrHits, tPtr);

    int mapProbes = 0;
    int umapProbes = 0;
    for (auto hit : ptrHits) {
        if (GetTickCount() - tPtr > kScanBudgetMs)
            break;
        ContainerLayout nodeLay = layout;
        if (probeMapNode(hit, needles[0].obj, nodeLay)) {
            ++mapProbes;
            const std::uintptr_t sentinel = walkToSentinel(
                hit - static_cast<std::uintptr_t>(nodeLay.mapNodePtrOffset), nodeLay);
            if (sentinel) {
                const std::uintptr_t mapObj = findMapObject(sentinel, nodeLay, needles, tPtr);
                if (mapObj) {
                    nodeLay.collection = "map";
                    const int score = scoreContainerRead(mapObj, nodeLay, needles, hints.name);
                    if (score >= 0)
                        considerCol(best, mapObj, nodeLay, score);
                }
            }
        }
        ContainerLayout umapLay = layout;
        if (probeUMapNode(hit, needles[0].obj, umapLay)) {
            ++umapProbes;
            std::vector<std::pair<int, std::uintptr_t>> nodes;
            std::uintptr_t sentinel = 0;
            const std::uintptr_t start =
                hit - static_cast<std::uintptr_t>(umapLay.listPtrOffset);
            if (walkUMapList(start, umapLay, nodes, sentinel) && sentinel) {
                const std::uintptr_t mapObj = findUMapObject(sentinel, umapLay, needles, tPtr);
                if (mapObj) {
                    umapLay.collection = "umap";
                    const int score = scoreContainerRead(mapObj, umapLay, needles, hints.name);
                    if (score >= 0)
                        considerCol(best, mapObj, umapLay, score);
                }
            }
        }
        if (best.score < 0) {
#if defined(_WIN64)
            const int strides[] = {layout.itemPtrSize > 0 ? layout.itemPtrSize : 0, 16, 8, 4};
#else
            const int strides[] = {layout.itemPtrSize > 0 ? layout.itemPtrSize : 0, 8, 4};
#endif
            for (int stride : strides) {
                if (stride <= 0)
                    continue;
                ContainerLayout arrLay = layout;
                std::uintptr_t arr = 0;
                if (!tryArrayAround(hit, arrLay, arr, stride))
                    continue;
                const int score = scoreContainerRead(arr, arrLay, needles, hints.name);
                if (score >= 0)
                    considerCol(best, arr, arrLay, score);
            }
        }
    }

    std::vector<ChainStep> collectionChain;
    if (best.score >= 0 && best.base)
        collectionChain = findChainTo(best.base, GetTickCount());
    if (collectionChain.empty()) {
        FoundCol fallback;
        if (findModuleContainerMap(profile, needles, hints, fallback)) {
            best = std::move(fallback);
            collectionChain = best.layout.chain;
        }
    }

    if (best.score < 0 || !best.base) {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_msg = "found container object but not the open-container list (ptrHits=" +
                std::to_string(ptrHits.size()) + " mapProbe=" + std::to_string(mapProbes) +
                " umapProbe=" + std::to_string(umapProbes) + ")";
        LOG("container collection miss ptrHits=%zu mapProbe=%d umapProbe=%d", ptrHits.size(),
            mapProbes, umapProbes);
        return false;
    }

    layout = best.layout;
    layout.present = true;
    layout.maxCount = kMaxContainers;
    layout.chain = std::move(collectionChain);
    if (layout.chain.empty()) {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_msg = "no pointer-chain to container collection";
        return false;
    }

    const std::uintptr_t resolved = resolveChain(layout.chain);
    if (resolved != best.base) {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_msg = "container-chain did not resolve to the discovered collection";
        LOG("container-chain target mismatch expected=0x%p resolved=0x%p",
            reinterpret_cast<void*>(best.base), reinterpret_cast<void*>(resolved));
        return false;
    }
    std::vector<ContainerSnapshot> cur;
    if (!resolved || !readCollection(resolved, layout, cur) || cur.empty()) {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_msg = "container-chain landed but read failed";
        return false;
    }

    setContainerLayout(layout);
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_layout = layout;
        g_open = cur;
        g_msg = "container-chain resolved";
    }
    LOG("container-chain resolved at 0x%p collection=%s open=%zu steps=%zu",
        reinterpret_cast<void*>(resolved), layout.collection.c_str(), cur.size(),
        layout.chain.size());
    replayContainersToPipe();
    return true;
}

std::string containersJson() {
    std::lock_guard<std::mutex> lock(g_mtx);
    json arr = json::array();
    for (const auto& c : g_open)
        arr.push_back(containerObject(c));
    return arr.dump();
}

std::vector<ContainerSnapshot> currentContainers() {
    std::lock_guard<std::mutex> lock(g_mtx);
    return g_open;
}

void replayContainersToPipe() {
    std::vector<ContainerSnapshot> open;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        open = g_open;
    }
    for (const auto& c : open)
        PipeServer::instance().emit("containerOpen", containerObject(c).dump());
}

bool resolveContainerItem(int index, int slot, int expectId, std::uintptr_t& ptr, std::uintptr_t& rep) {
    ptr = 0;
    rep = 0;
    if (index < 0 || index >= kMaxContainers || slot < 0 || slot >= kMaxItems)
        return false;

    ContainerLayout layout;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        layout = g_layout;
    }
    if (!layout.present || layout.chain.empty())
        return false;

    const std::uintptr_t base = resolveChain(layout.chain);
    std::uintptr_t obj = 0;
    if (!base || !findContainerObj(base, layout, index, obj) || !obj)
        return false;

    if (!itemSharedAt(obj, layout, slot, ptr, rep) || !ptr)
        return false;

    int id = 0;
    int count = 0;
    if (!readItem(ptr, layout, id, count) || !id)
        return false;
    if (expectId && id != expectId) {
        LOG("use item id mismatch container=%d slot=%d have=%d want=%d", index, slot, id, expectId);
        ptr = 0;
        rep = 0;
        return false;
    }
    LOG("use item container=%d slot=%d id=%d ptr=%p rep=%p", index, slot, id,
        reinterpret_cast<void*>(ptr), reinterpret_cast<void*>(rep));
    return true;
}

} // namespace otc

