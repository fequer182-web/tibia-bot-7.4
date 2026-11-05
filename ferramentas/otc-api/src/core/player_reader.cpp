#include "otc_api/player_reader.h"

#include "otc_api/actions.h"
#include "otc_api/build_id.h"
#include "otc_api/container_reader.h"
#include "otc_api/creature_reader.h"
#include "otc_api/logger.h"
#include "otc_api/memory.h"
#include "otc_api/pipe_server.h"
#include "otc_api/profile.h"
#include "otc_api/profile_store.h"
#include "otc_api/player_recovery.h"

#include <nlohmann/json.hpp>

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace otc {
namespace {

using nlohmann::json;

constexpr DWORD kPollMs = 250;
constexpr DWORD kScanBudgetMs = 20000;
constexpr DWORD kChainlessExtraMs = 20000;
constexpr std::size_t kMaxScanBytes = 512ull * 1024 * 1024;

std::mutex g_mtx;
BuildId g_build;
Profile g_profile;
std::uintptr_t g_statsBase = 0;
PlayerSnapshot g_snap;
CalibHints g_hints;
std::string g_phase = "needCalibration";
std::string g_phaseMsg = "Waiting for profile or hints";
HANDLE g_pollThread = nullptr;
bool g_pollRun = false;
bool g_profileFound = false;
bool g_readyEmitted = false;
std::uint32_t g_lastEmittedTarget = 0;
bool g_targetEmitted = false;
bool g_livePosKnown = false;
std::int32_t g_livePosOff = 0;
bool g_livePosPacked = false;
std::uintptr_t g_posDiscoverBase = 0;
DWORD g_posDiscoverSince = 0;
int g_posLogLeft = 4;

void applyLivePosition(PlayerSnapshot& s, std::uintptr_t statsBase, const Profile& p);
bool looksLikeHeapObject(std::uintptr_t p);
bool looksLikeVtable(std::uintptr_t obj);
bool readLayoutPosition(std::uintptr_t statsBase, const Profile& layout, int& x, int& y, int& z);
bool plausibleCalibratedPos(int x, int y, int z);
std::uintptr_t playerObjectFromStats(std::uintptr_t statsBase, const Profile& p);

double readStat(std::uintptr_t base, std::int32_t off, StatType t) {
    const std::uintptr_t addr = base + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(off));
    if (t == StatType::Int32) {
        std::int32_t v = 0;
        if (!memReadT(addr, v))
            return 0;
        return static_cast<double>(v);
    }
    if (t == StatType::Float) {
        float v = 0;
        if (!memReadT(addr, v))
            return 0;
        return static_cast<double>(v);
    }
    double v = 0;
    if (!memReadT(addr, v))
        return 0;
    return v;
}

int scaleCap(double raw, int scale, bool truncate = false) {
    double scaled = raw;
    if (scale == 100)
        scaled = raw * 100.0;
    else if (scale == -100)
        scaled = raw / 100.0;
    return truncate ? static_cast<int>(scaled) : static_cast<int>(std::lround(scaled));
}

bool nearEq(double a, double b, double tol = 0.51) {
    return std::fabs(a - b) <= tol;
}

bool plausible(const PlayerSnapshot& s) {
    if (!s.valid)
        return false;
    if (s.health < 0 || s.maxHealth < 50 || s.health > s.maxHealth + 5)
        return false;
    if (s.maxHealth > 20000)
        return false;
    if (s.level < 1 || s.level > 2000)
        return false;
    // Some valid characters/build states legitimately expose no mana as 0/0
    // (for example, a vocation without mana or while the LocalPlayer is being
    // rebuilt during login). Do not make the build profile character-specific
    // by requiring maxMana to be positive. Negative values and mana above the
    // reported maximum remain invalid.
    if (s.mana < 0 || s.maxMana < 0 || s.mana > s.maxMana + 5)
        return false;
    return true;
}

std::uintptr_t resolveChain(const Profile& p) {
    if (p.chain.empty())
        return 0;
    std::uintptr_t cur = moduleBase();
    for (const auto& step : p.chain) {
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

PlayerSnapshot readSnapshot(std::uintptr_t base, const Profile& p) {
    PlayerSnapshot s;
    if (!base)
        return s;

    const std::uintptr_t player = playerObjectFromStats(base, p);
    if (player && p.creatures.present) {
        if (p.creatures.nameOffset >= 0)
            readCreatureNameAt(player + static_cast<std::uintptr_t>(p.creatures.nameOffset),
                               p.creatures, s.name);
        if (p.creatures.directionOffset >= 0) {
            std::uint32_t value = 0;
            const auto addr = player + static_cast<std::uintptr_t>(p.creatures.directionOffset);
            if (p.creatures.directionSize == 1) {
                std::uint8_t v = 0;
                if (memReadT(addr, v)) value = v;
            } else if (p.creatures.directionSize == 2) {
                std::uint16_t v = 0;
                if (memReadT(addr, v)) value = v;
            } else {
                memReadT(addr, value);
            }
            static const char* dirs[] = {"north", "east", "south", "west"};
            if (value < 4) s.direction = dirs[value];
        }
    }

    s.health = static_cast<int>(std::lround(readStat(base, p.offHealth, p.statType)));
    s.maxHealth = static_cast<int>(std::lround(readStat(base, p.offMaxHealth, p.statType)));
    s.mana = static_cast<int>(std::lround(readStat(base, p.offMana, p.statType)));
    s.maxMana = static_cast<int>(std::lround(readStat(base, p.offMaxMana, p.statType)));
    s.capacity = scaleCap(readStat(base, p.offFreeCap, p.statType), p.capacityScale,
                          p.capacityTruncate);
    if (p.hasTotalCapacity)
        s.totalCapacity = scaleCap(readStat(base, p.offTotalCap, p.statType), p.capacityScale,
                                   p.capacityTruncate);

    if (p.statType == StatType::Int32) {
        std::uint16_t lv = 0;
        std::uint16_t lp = 0;
        std::uint16_t ml = 0;
        std::uint8_t soul = 0;
        memReadT(base + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(p.offLevel)), lv);
        memReadT(base + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(p.offLevelPct)), lp);
        memReadT(base + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(p.offMagicLevel)), ml);
        memReadT(base + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(p.offSoul)), soul);
        s.level = lv;
        s.levelPercent = lp;
        if (p.hasMagicLevel)
            s.magicLevel = ml;
        if (p.hasSoul)
            s.soul = soul;
    } else {
        s.level = static_cast<int>(std::lround(readStat(base, p.offLevel, p.statType)));
        s.levelPercent = static_cast<int>(std::lround(readStat(base, p.offLevelPct, p.statType)));
        if (p.hasMagicLevel)
            s.magicLevel =
                static_cast<int>(std::lround(readStat(base, p.offMagicLevel, p.statType)));
        if (p.hasSoul)
            s.soul = static_cast<int>(std::lround(readStat(base, p.offSoul, p.statType)));
    }

    if (p.statType == StatType::Int32 || p.experienceUInt64) {
        std::uint32_t expLo = 0;
        std::uint32_t expHi = 0;
        memReadT(base + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(p.offExp)), expLo);
        memReadT(base + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(p.offExp)) + 4,
                 expHi);
        s.experience = (static_cast<std::uint64_t>(expHi) << 32) | expLo;
    } else {
        s.experience =
            static_cast<std::uint64_t>(std::llround(readStat(base, p.offExp, p.statType)));
    }

    if (p.hasStates) {
        const std::uintptr_t addr =
            base + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(p.statesOffset));
        memReadT(addr, s.stateFlags);
    }

    if (p.actions.gameRva != 0 && p.actions.attackingCreatureOffset >= 0 &&
        p.creatures.present && p.creatures.idSize == 4) {
        const std::uintptr_t game =
            moduleBase() + static_cast<std::uintptr_t>(
                               static_cast<std::intptr_t>(p.actions.gameRva));
        std::uintptr_t target = 0;
        if (readPtr(game + static_cast<std::uintptr_t>(p.actions.attackingCreatureOffset),
                    target) && target != 0) {
            std::uint32_t id = 0;
            const auto idAddr = target + static_cast<std::uintptr_t>(
                                             static_cast<std::intptr_t>(p.creatures.idOffset));
            if (memReadT(idAddr, id) && id >= 0x10000000u)
                s.targetCreatureId = id;
        }
    }

    applyLivePosition(s, base, p);

    if (p.inventory.present) {
        const std::uintptr_t array =
            base + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(p.inventory.arrayOffset));
        const int stride =
            p.inventory.ptrSize > 0 ? p.inventory.ptrSize : static_cast<int>(sizeof(void*));
        std::uintptr_t ptrs[10]{};
        std::uint16_t ids[10]{};
        int filled = 0;
        for (int i = 0; i < 10; ++i) {
            s.inventory[i].slot = i + 1;
            const std::uintptr_t slotAddr = array + static_cast<std::uintptr_t>(i * stride);
            std::uintptr_t item = slotAddr;
            if (p.inventory.slotsArePointers) {
                item = 0;
                if (!readPtr(slotAddr, item) || !item)
                    continue;
            }
            std::uint16_t id = 0;
            if (!memReadT(item + static_cast<std::uintptr_t>(p.inventory.itemIdOffset), id))
                continue;
            if (id < 100)
                continue;
            std::uint8_t count8 = 0;
            memReadT(item + static_cast<std::uintptr_t>(p.inventory.itemCountOffset), count8);
            s.inventory[i].id = id;
            s.inventory[i].count = count8 > 0 ? static_cast<int>(count8) : 1;
            ptrs[i] = item;
            ids[i] = id;
            ++filled;
        }
        int uniqueIds = 0;
        int uniquePtrs = 0;
        for (int i = 0; i < 10; ++i) {
            if (!ids[i])
                continue;
            bool idSeen = false;
            bool ptrSeen = false;
            for (int j = 0; j < i; ++j) {
                if (ids[j] && ids[j] == ids[i])
                    idSeen = true;
                if (ptrs[j] && ptrs[j] == ptrs[i])
                    ptrSeen = true;
            }
            if (!idSeen)
                ++uniqueIds;
            if (!ptrSeen)
                ++uniquePtrs;
        }
        if (filled >= 2 && (uniqueIds < 2 || uniquePtrs < filled)) {
            for (int i = 0; i < 10; ++i)
                s.inventory[i] = ItemSlot{i + 1, 0, 0};
            s.hasInventory = false;
        } else {
            s.hasInventory = filled > 0;
        }
    }

    s.valid = true;
    if (!plausible(s)) {
        s.valid = false;
    }
    return s;
}

bool loadProfile() {
    const auto dir = thisDllDir();
    CalibrationSource source = CalibrationSource::None;
    const bool ok = loadCalibration(g_profile, dir, g_build.id, &source);
    const char* sourceName = source == CalibrationSource::External ? "external" :
                             source == CalibrationSource::Embedded ? "embedded" : "none";
    LOG("profile %s source=%s path=%ls", ok ? "loaded" : "missing or invalid", sourceName,
        calibrationPath(dir, g_build.id).c_str());
    return ok;
}

// Called only after the saved chain failed. No heap scan or client action occurs.
bool recoverPlayerFromGame() {
    static DWORD lastAttempt = 0;
    static bool attempted = false;
    static DWORD lastDiscovery = 0;
    static std::vector<std::int32_t> anchors;
    static PlayerRecoveryConfirmation confirmation;
    const DWORD now = GetTickCount();
    if (attempted && now - lastAttempt < 1000)
        return false;
    attempted = true;
    lastAttempt = now;
    if (gamePlayerRecoveryChain(g_profile, 1).empty())
        return false;
    if (anchors.empty() && (!lastDiscovery || now - lastDiscovery >= 10000)) {
        lastDiscovery = now;
        resolveCalibrationGameAnchors(anchors);
    }

    Profile selected;
    PlayerSnapshot selectedSnap;
    std::uintptr_t selectedBase = 0;
    std::int32_t selectedRva = 0;
    std::int32_t selectedMember = 0;
    auto considerCandidate = [&](Profile candidate, std::int32_t rva, std::int32_t member) {
        const auto base = resolveChain(candidate);
        int x = 0, y = 0, z = 0;
        if (!base || !readLayoutPosition(base, candidate, x, y, z) ||
            !plausibleCalibratedPos(x, y, z))
            return true;
        // Validate every equipped pointer, not just plausible numbers in a copied HP block.
        const auto array = base + static_cast<std::intptr_t>(candidate.inventory.arrayOffset);
        const int stride = candidate.inventory.ptrSize;
        if (stride != 4 && stride != 8 && stride != 16)
            return true;
        for (int slot = 0; slot < 10; ++slot) {
            std::uintptr_t item = 0;
            if (!readPtr(array + slot * stride, item) || (item && !looksLikeVtable(item)))
                return true;
        }
        const auto snap = readSnapshot(base, candidate);
        if (!snap.valid || !snap.hasInventory || !snap.hasPosition ||
            snap.position.x != x || snap.position.y != y || snap.position.z != z)
            return true;
        if (selectedBase && selectedBase != base) {
            confirmation.observe(0, 0);
            LOG("P1 recovery rejected ambiguous verified anchors");
            return false;
        }
        if (!selectedBase) {
            selected = std::move(candidate);
            selectedSnap = snap;
            selectedBase = base;
            selectedRva = rva;
            selectedMember = member;
        }
        return true;
    };
    for (const auto rva : anchors) {
      for (int member = 0; member <= 64; member += static_cast<int>(sizeof(void*))) {
        Profile candidate = g_profile;
        candidate.chain = gamePlayerRecoveryChain(candidate, rva, member);
        if (candidate.chain.empty())
            continue;
        std::uintptr_t player = 0;
        if (!readPtr(moduleBase() + static_cast<std::uintptr_t>(rva) + member, player) ||
            !looksLikeHeapObject(player) || !looksLikeVtable(player))
            continue;
        if (!considerCandidate(std::move(candidate), rva, member))
            return false;
      }
    }

    // A calibrated P2 module collection is a strong Game-local anchor even
    // when x64 bind decoding cannot recover g_game. Search a small module-only
    // neighborhood and accept only the fully validated LocalPlayer snapshot.
    if (!selectedBase && g_profile.containers.present &&
        !g_profile.containers.chain.empty()) {
        Profile containerAnchor;
        containerAnchor.chain = g_profile.containers.chain;
        const auto containerBase = resolveChain(containerAnchor);
        const auto mod = moduleBase();
        const auto imageEnd = mod + moduleImageSize();
        if (containerBase >= mod && containerBase < imageEnd) {
            constexpr std::uintptr_t kGameNeighborhood = 8192;
            const auto lo = containerBase > mod + kGameNeighborhood
                ? containerBase - kGameNeighborhood : mod;
            const auto hi = (std::min)(containerBase + kGameNeighborhood, imageEnd);
            for (auto slot = lo; slot + sizeof(void*) <= hi; slot += sizeof(void*)) {
                std::uintptr_t player = 0;
                if (!readPtr(slot, player) || !looksLikeHeapObject(player) ||
                    !looksLikeVtable(player))
                    continue;
                Profile candidate = g_profile;
                candidate.chain = {
                    {static_cast<std::int32_t>(slot - mod), true}, g_profile.chain.back()};
                if (!considerCandidate(std::move(candidate),
                                       static_cast<std::int32_t>(slot - mod), 0))
                    return false;
            }
            LOG("P1 recovery near P2 collection=%p selected=%p",
                reinterpret_cast<void*>(containerBase), reinterpret_cast<void*>(selectedBase));
        }
    }

    // Some builds expose no usable Game bind metadata. If the module prefix of
    // the saved chain still resolves, search only the two stale object members.
    // The calibrated position, all equipment slots and full stats remain the
    // acceptance gate, and the legacy/Game paths above keep priority.
    if (!selectedBase && g_profile.chain.size() == 4 && g_profile.chain[0].deref &&
        g_profile.chain[1].deref && g_profile.chain[2].deref &&
        !g_profile.chain[3].deref) {
        const auto mod = moduleBase();
        const auto firstSlot = mod + static_cast<std::uintptr_t>(
            static_cast<std::intptr_t>(g_profile.chain[0].offset));
        std::uintptr_t root = 0;
        if (readPtr(firstSlot, root) && looksLikeHeapObject(root)) {
            constexpr int kRootSpan = 4096;
            constexpr int kChildSpan = 512;
            for (int off1 = 0; off1 <= kRootSpan; off1 += static_cast<int>(sizeof(void*))) {
                std::uintptr_t child = 0;
                if (!readPtr(root + off1, child) || !looksLikeHeapObject(child))
                    continue;
                for (int off2 = 0; off2 <= kChildSpan; off2 += static_cast<int>(sizeof(void*))) {
                    std::uintptr_t player = 0;
                    if (!readPtr(child + off2, player) || !looksLikeHeapObject(player) ||
                        !looksLikeVtable(player))
                        continue;
                    Profile candidate = g_profile;
                    candidate.chain = {g_profile.chain[0], {off1, true}, {off2, true},
                                       g_profile.chain[3]};
                    if (!considerCandidate(std::move(candidate),
                                           g_profile.chain[0].offset, off1 ^ (off2 << 16)))
                        return false;
                }
            }
        }
        LOG("P1 recovery stable-prefix root=%p selected=%p",
            reinterpret_cast<void*>(root), reinterpret_cast<void*>(selectedBase));
    }
    if (!confirmation.observe(selectedBase, selectedRva + selectedMember))
        return false;
    g_profile.chain = selected.chain; // Preserve all calibrated layouts, P2-P5 included.
    g_statsBase = selectedBase;
    g_snap = selectedSnap;
    g_phase = "ready";
    const bool saved = saveCalibration(g_profile, thisDllDir());
    g_phaseMsg = saved ? "LocalPlayer recovered; profile chain repaired" :
        "LocalPlayer recovered; profile save failed (check DLL folder permissions)";
    LOG("P1 recovery via Game rva=0x%X member=%d statsBase=%p steps=%zu saved=%d",
        static_cast<unsigned>(selectedRva), selectedMember, reinterpret_cast<void*>(selectedBase),
        g_profile.chain.size(), saved ? 1 : 0);
    return true;
}

bool tryApplyProfile() {
    if (g_profile.chain.empty())
        return false;
    const std::uintptr_t base = resolveChain(g_profile);
    if (!base) {
        LOG("pointer-chain failed (%zu steps)", g_profile.chain.size());
        return recoverPlayerFromGame();
    }
    const PlayerSnapshot snap = readSnapshot(base, g_profile);
    if (!snap.valid) {
        LOG("pointer-chain landed but stats implausible hp=%d/%d mana=%d/%d level=%d",
            snap.health, snap.maxHealth, snap.mana, snap.maxMana, snap.level);
        return recoverPlayerFromGame();
    }
    g_statsBase = base;
    g_snap = snap;
    g_phase = "ready";
    g_phaseMsg = "LocalPlayer live";
    if (g_profile.inventory.present && !snap.hasInventory) {
        LOG("dropping implausible inventory layout");
        g_profile.inventory = InventoryLayout{};
    }
    LOG("pointer-chain resolved at 0x%p (%zu steps)", reinterpret_cast<void*>(base),
        g_profile.chain.size());
    return true;
}

bool hintsMatchSnap(const PlayerSnapshot& s, const CalibHints& hints) {
    if (!s.valid)
        return false;
    if (hints.hasHealth && (!nearEq(s.health, hints.health) ||
                            !nearEq(s.maxHealth, hints.maxHealth)))
        return false;
    if (hints.hasMana && (!nearEq(s.mana, hints.mana) ||
                          !nearEq(s.maxMana, hints.maxMana)))
        return false;
    if (hints.hasLevel && s.level != hints.level)
        return false;
    if (hints.hasCapacity && s.capacity != hints.capacity)
        return false;
    if (hints.hasExp && s.experience != hints.experience)
        return false;
    return true;
}

void pickCapacityEncoding(std::uintptr_t base, Profile& layout, const CalibHints& hints) {
    if (!hints.hasCapacity)
        return;
    const double raw = readStat(base, layout.offFreeCap, layout.statType);
    const int scales[] = {1, 100, -100};
    for (int sc : scales) {
        for (bool truncate : {false, true}) {
            if (scaleCap(raw, sc, truncate) != hints.capacity)
                continue;
            layout.capacityScale = sc;
            layout.capacityTruncate = truncate;
            return;
        }
    }
}

bool matchHints(std::uintptr_t base, const Profile& layout, const CalibHints& hints) {
    return hintsMatchSnap(readSnapshot(base, layout), hints);
}

bool regionWritable(const MEMORY_BASIC_INFORMATION& mbi) {
    if (mbi.State != MEM_COMMIT)
        return false;
    if (mbi.Type != MEM_PRIVATE && mbi.Type != MEM_MAPPED)
        return false;
    if (memPageGuarded(mbi.Protect))
        return false;
    const DWORD prot = mbi.Protect & 0xFF;
    return prot == PAGE_READWRITE || prot == PAGE_EXECUTE_READWRITE ||
           prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_WRITECOPY;
}

bool regionPrivateWritable(const MEMORY_BASIC_INFORMATION& mbi) {
    return regionWritable(mbi) && mbi.Type == MEM_PRIVATE;
}

int readLevelAt(std::uintptr_t base, std::int32_t off, StatType type) {
    const std::uintptr_t addr = base + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(off));
    if (type == StatType::Int32) {
        std::uint16_t v = 0;
        if (!memReadT(addr, v))
            return 0;
        return v;
    }
    return static_cast<int>(std::lround(readStat(base, off, type)));
}

std::uint64_t readExpAt(std::uintptr_t base, std::int32_t off, StatType type,
                        bool forceUInt64 = false) {
    const std::uintptr_t addr = base + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(off));
    if (type == StatType::Int32 || forceUInt64) {
        std::uint32_t lo = 0;
        std::uint32_t hi = 0;
        if (!memReadT(addr, lo))
            return 0;
        memReadT(addr + 4, hi);
        if (hi == 0)
            return lo;
        return (static_cast<std::uint64_t>(hi) << 32) | lo;
    }
    const double v = readStat(base, off, type);
    if (v < 0)
        return 0;
    return static_cast<std::uint64_t>(std::llround(v));
}

bool offsetTaken(const Profile& p, std::int32_t off) {
    return off == p.offHealth || off == p.offMaxHealth || off == p.offMana || off == p.offMaxMana ||
           off == p.offLevel;
}

bool discoverFields(std::uintptr_t healthAddr, StatType type, int stride, const CalibHints& hints,
                    Profile& layout, int lo, int hi) {
    layout = Profile{};
    layout.statType = type;
    layout.offHealth = 0;
    layout.offMaxHealth = stride;
    layout.offFreeCap = 0;
    layout.offTotalCap = 0;
    layout.offExp = 0;
    layout.offLevel = 0;
    layout.offLevelPct = 0;
    layout.offMana = 0;
    layout.offMaxMana = 0;
    layout.offMagicLevel = 0;
    layout.offSoul = 0;
    layout.experienceUInt64 = false;
    layout.capacityTruncate = false;
    layout.hasTotalCapacity = false;
    layout.hasMagicLevel = false;
    layout.hasSoul = false;
    if (!nearEq(readStat(healthAddr, 0, type), hints.health) ||
        !nearEq(readStat(healthAddr, stride, type), hints.maxHealth))
        return false;

    const int align = (type == StatType::Double) ? 8 : (type == StatType::Float) ? 4 : 4;

    if (hints.hasMana) {
        bool found = false;
        for (int off = lo; off <= hi; off += align) {
            if (off == 0 || off == stride || off + stride == 0)
                continue;
            if (nearEq(readStat(healthAddr, off, type), hints.mana) &&
                nearEq(readStat(healthAddr, off + stride, type), hints.maxMana)) {
                layout.offMana = off;
                layout.offMaxMana = off + stride;
                found = true;
                break;
            }
        }
        if (!found) {
            LOG("discoverFields mana miss health=0x%p window=[%d,%d]",
                reinterpret_cast<void*>(healthAddr), lo, hi);
            return false;
        }
    }

    if (hints.hasLevel) {
        const int step = (type == StatType::Int32) ? 2 : align;
        int bestOff = 0;
        int bestScore = 0x7fffffff;
        bool found = false;
        for (int off = lo; off <= hi; off += step) {
            if (offsetTaken(layout, off))
                continue;
            if (readLevelAt(healthAddr, off, type) != hints.level)
                continue;
            int score = std::abs(off);
            if (hints.hasMana) {
                if (off < layout.offMana)
                    score = layout.offMana - off;
                else
                    score = 1000 + (off - layout.offMana);
            }
            if (score < bestScore) {
                bestScore = score;
                bestOff = off;
                found = true;
            }
        }
        if (!found) {
            LOG("discoverFields level miss health=0x%p window=[%d,%d]",
                reinterpret_cast<void*>(healthAddr), lo, hi);
            return false;
        }
        layout.offLevel = bestOff;
        layout.offLevelPct = (type == StatType::Int32) ? bestOff + 2 : bestOff + align;
        if (type == StatType::Int32) {
            const int tryPct[] = {bestOff + 2, bestOff + 4, bestOff - 2, bestOff + 6, bestOff + 1};
            bool pctOk = false;
            for (int po : tryPct) {
                if (po == bestOff || offsetTaken(layout, po))
                    continue;
                const int pct = readLevelAt(healthAddr, po, type);
                if (pct >= 0 && pct <= 100) {
                    layout.offLevelPct = po;
                    pctOk = true;
                    break;
                }
            }
            if (!pctOk)
                LOG("int32 levelPercent not 0-100 near levelOff=%d (not vetoing HP block)", bestOff);
        }
    }

    if (hints.hasExp) {
        const int step = (type == StatType::Double) ? 8 : 4;
        bool found = false;
        for (int off = lo; off <= hi; off += step) {
            if (offsetTaken(layout, off))
                continue;
            if (readExpAt(healthAddr, off, type) == hints.experience) {
                layout.offExp = off;
                found = true;
                break;
            }
        }
        if (!found && type != StatType::Int32) {
            for (int off = lo; off <= hi; off += 4) {
                if (offsetTaken(layout, off))
                    continue;
                if (readExpAt(healthAddr, off, type, true) != hints.experience)
                    continue;
                layout.offExp = off;
                layout.experienceUInt64 = true;
                found = true;
                break;
            }
        }
        if (!found) {
            LOG("discoverFields exp miss health=0x%p window=[%d,%d]",
                reinterpret_cast<void*>(healthAddr), lo, hi);
            return false;
        }
    }

    if (hints.hasCapacity) {
        const int step = align;
        bool found = false;
        for (int off = lo; off <= hi; off += step) {
            if (offsetTaken(layout, off) || off == layout.offExp)
                continue;
            const double raw = readStat(healthAddr, off, type);
            const int scales[] = {1, 100, -100};
            for (int sc : scales) {
                for (bool truncate : {false, true}) {
                    if (scaleCap(raw, sc, truncate) != hints.capacity)
                        continue;
                    layout.offFreeCap = off;
                    layout.capacityScale = sc;
                    layout.capacityTruncate = truncate;
                    found = true;
                    break;
                }
                if (found)
                    break;
            }
            if (found)
                break;
        }
        if (!found) {
            LOG("discoverFields cap miss health=0x%p window=[%d,%d]",
                reinterpret_cast<void*>(healthAddr), lo, hi);
            return false;
        }
    }
    return true;
}

bool tryLayouts(std::uintptr_t healthAddr, StatType type, int stride, const CalibHints& hints,
                Profile& layout, PlayerSnapshot& snap) {
    if (type == StatType::Double && stride == 8) {
        Profile classic;
        classic.statType = StatType::Double;
        const int scales[] = {1, 100, -100};
        for (int sc : scales) {
            if (!hints.hasCapacity && sc != 1)
                continue;
            for (bool truncate : {false, true}) {
                classic.capacityScale = sc;
                classic.capacityTruncate = truncate;
                if (!matchHints(healthAddr, classic, hints))
                    continue;
                layout = classic;
                pickCapacityEncoding(healthAddr, layout, hints);
                snap = readSnapshot(healthAddr, layout);
                return snap.valid;
            }
        }
    }

    if (type == StatType::Int32 && stride == 4) {
        Profile packed;
        packed.statType = StatType::Int32;
        packed.offHealth = 0;
        packed.offMaxHealth = 4;
        packed.offFreeCap = -12;
        packed.offTotalCap = -8;
        packed.offExp = 8;
        packed.offLevel = 16;
        packed.offLevelPct = 18;
        packed.offMana = 20;
        packed.offMaxMana = 24;
        packed.offMagicLevel = 28;
        packed.offSoul = 34;
        if (matchHints(healthAddr, packed, hints)) {
            pickCapacityEncoding(healthAddr, packed, hints);
            snap = readSnapshot(healthAddr, packed);
            if (snap.valid && snap.levelPercent >= 0 && snap.levelPercent <= 100) {
                layout = packed;
                return true;
            }
        }
    }

    if (!discoverFields(healthAddr, type, stride, hints, layout, -128, 320)) {
        if (type != StatType::Int32)
            return false;
        if (discoverFields(healthAddr, type, stride, hints, layout, -1024, 1536)) {
            LOG("discoverFields int32 wide window health=0x%p manaOff=%d levelOff=%d expOff=%d "
                "capOff=%d",
                reinterpret_cast<void*>(healthAddr), layout.offMana, layout.offLevel, layout.offExp,
                layout.offFreeCap);
        } else if (discoverFields(healthAddr, type, stride, hints, layout, -4096, 4096)) {
            LOG("discoverFields int32 object window health=0x%p manaOff=%d levelOff=%d expOff=%d "
                "capOff=%d",
                reinterpret_cast<void*>(healthAddr), layout.offMana, layout.offLevel, layout.offExp,
                layout.offFreeCap);
        } else {
            return false;
        }
    }
    pickCapacityEncoding(healthAddr, layout, hints);
    snap = readSnapshot(healthAddr, layout);
    if (!hintsMatchSnap(snap, hints)) {
        LOG("tryLayouts snap mismatch health=0x%p hp=%d/%d mana=%d/%d lv=%d exp=%llu cap=%d",
            reinterpret_cast<void*>(healthAddr), snap.health, snap.maxHealth, snap.mana, snap.maxMana,
            snap.level, static_cast<unsigned long long>(snap.experience), snap.capacity);
        return false;
    }
    return true;
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

bool moduleReadableRegion(const MEMORY_BASIC_INFORMATION& mbi) {
    if (mbi.State != MEM_COMMIT)
        return false;
    if (memPageGuarded(mbi.Protect))
        return false;
    return true;
}

struct FindChainCache {
    std::uintptr_t mod = 0;
    std::size_t img = 0;
    std::uintptr_t walkAddr = 0;
    std::uintptr_t dataLo = 0;
    std::uintptr_t dataHi = 0;
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> heapRanges;
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> heapPtrs;
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> heapMids;
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> dataPtrs;
    bool filled = false;
    bool midsReady = false;
    bool dataFilled = false;
    bool gameAnchorTried = false;
    std::vector<std::int32_t> gameRvas;
};

bool ptrInRanges(std::uintptr_t v, const std::vector<std::pair<std::uintptr_t, std::uintptr_t>>& r) {
    if (r.empty() || v < r.front().first)
        return false;
    auto it = std::upper_bound(r.begin(), r.end(), v,
                               [](std::uintptr_t x, const std::pair<std::uintptr_t, std::uintptr_t>& e) {
                                   return x < e.first;
                               });
    if (it == r.begin())
        return false;
    --it;
    return v < it->second;
}

void fillHeapRanges(FindChainCache* cache) {
    if (!cache->heapRanges.empty())
        return;
    const std::uintptr_t mod = cache->mod;
    const std::uintptr_t modEnd = mod + cache->img;
    MEMORY_BASIC_INFORMATION mbi{};
    std::uintptr_t addr = 0;
    while (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi))) {
        const std::uintptr_t next =
            reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && (mbi.Type == MEM_PRIVATE || mbi.Type == MEM_MAPPED)) {
            if (!memPageGuarded(mbi.Protect)) {
                const auto start = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
                const auto end = start + mbi.RegionSize;
                if (!(end > mod && start < modEnd)) {
                    if (!cache->heapRanges.empty() && cache->heapRanges.back().second == start)
                        cache->heapRanges.back().second = end;
                    else
                        cache->heapRanges.push_back({start, end});
                }
            }
        }
        if (next <= addr)
            break;
        addr = next;
    }
}

bool moduleWritableDataRegion(const MEMORY_BASIC_INFORMATION& mbi) {
    if (!moduleReadableRegion(mbi))
        return false;
    const DWORD prot = mbi.Protect & 0xFF;
    return prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
           prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
}

bool heapPrivatePtr(std::uintptr_t p) {
    if (p < 0x10000)
        return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(p), &mbi, sizeof(mbi)))
        return false;
    if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE)
        return false;
    return !memPageGuarded(mbi.Protect);
}

bool sharedSlotAt(std::uintptr_t slot) {
    std::uintptr_t rep = 0;
    return readPtr(slot + sizeof(void*), rep) && looksLikeSharedRep(rep);
}

void initChainCache(FindChainCache* cache) {
    if (cache->mod)
        return;
    cache->mod = moduleBase();
    cache->img = moduleImageSize();
    cache->walkAddr = cache->mod;
    cache->dataLo = 0;
    cache->dataHi = 0;
    cache->heapPtrs.clear();
    cache->heapMids.clear();
    cache->dataPtrs.clear();
    cache->heapRanges.clear();
    cache->heapPtrs.reserve(16384);
    cache->dataPtrs.reserve(4096);
    cache->filled = false;
    cache->midsReady = false;
    cache->dataFilled = false;
    cache->gameAnchorTried = false;
    cache->gameRvas.clear();
    fillHeapRanges(cache);
}

std::size_t readablePrefix(std::uintptr_t p, std::size_t cap) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(p), &mbi, sizeof(mbi)))
        return 0;
    if (mbi.State != MEM_COMMIT)
        return 0;
    if (memPageGuarded(mbi.Protect))
        return 0;
    const std::uintptr_t end = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    if (end <= p)
        return 0;
    const std::size_t avail = static_cast<std::size_t>(end - p);
    return avail < cap ? avail : cap;
}

int gameObjectScore(std::uintptr_t obj) {
    if (!obj)
        return -1;
    const std::size_t n = readablePrefix(obj, 1024);
    if (n < 16)
        return -1;
    int shared = 0;
    int heap = 0;
    for (std::size_t off = 0; off + 2 * sizeof(void*) <= n; off += sizeof(void*)) {
        std::uintptr_t p = 0;
        if (!readPtr(obj + off, p) || !p)
            continue;
        if (!looksLikeHeapPtr(p))
            continue;
        ++heap;
        if (sharedSlotAt(obj + off))
            ++shared;
    }
    if (shared < 1 && heap < 6)
        return -1;
    return shared * 1000 + heap;
}

int cheapHeapMemberCount(std::uintptr_t obj, std::size_t cap = 256) {
    if (!obj)
        return -1;
    const std::size_t n = readablePrefix(obj, cap);
    if (n < 16)
        return -1;
    int heap = 0;
    for (std::size_t off = 0; off + sizeof(void*) <= n; off += sizeof(void*)) {
        std::uintptr_t p = 0;
        if (!readPtr(obj + off, p) || !p)
            continue;
        if (looksLikeHeapPtr(p))
            ++heap;
    }
    return heap;
}

bool peSectionRange(const char* name, std::uintptr_t& lo, std::uintptr_t& hi) {
    lo = 0;
    hi = 0;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(hostModule());
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
        reinterpret_cast<std::uint8_t*>(dos) + dos->e_lfanew);
    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE)
        return false;
    auto* sec = IMAGE_FIRST_SECTION(nt);
    const std::uintptr_t mod = moduleBase();
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        char n[9]{};
        std::memcpy(n, sec[i].Name, 8);
        if (std::strcmp(n, name) != 0)
            continue;
        lo = mod + sec[i].VirtualAddress;
        const std::size_t sz =
            sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
        hi = lo + sz;
        return hi > lo;
    }
    return false;
}

void logPeSections() {
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(hostModule());
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
        reinterpret_cast<std::uint8_t*>(dos) + dos->e_lfanew);
    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE)
        return;
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        char n[9]{};
        std::memcpy(n, sec[i].Name, 8);
        LOG("pe section %s rva=0x%X sz=%u", n, sec[i].VirtualAddress, sec[i].Misc.VirtualSize);
    }
}

void fillDataPtrs(FindChainCache* cache) {
    initChainCache(cache);
    if (cache->dataFilled)
        return;
    const std::uintptr_t mod = cache->mod;
    const std::uintptr_t modEnd = mod + cache->img;
    const bool filterHeap = !cache->heapRanges.empty();
    constexpr std::size_t kMaxVt = 512;
    constexpr std::size_t kMaxStatic = 0;
    constexpr DWORD kBudgetMs = 2500;
    const DWORD t0 = GetTickCount();
    std::size_t heapSeen = 0;
    std::size_t nShared = 0;
    std::size_t nVt = 0;
    std::size_t nStatic = 0;
    std::size_t vtKept = 0;
    LOG("findChain fillDataPtrs start ranges=%zu img=%zu", cache->heapRanges.size(), cache->img);
    logPeSections();

    auto inHeap = [&](std::uintptr_t v) -> bool {
        if (v < 0x10000)
            return false;
        if (v >= mod && v < modEnd)
            return false;
        if (!filterHeap)
            return true;
        return ptrInRanges(v, cache->heapRanges);
    };

    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> writable;
    MEMORY_BASIC_INFORMATION mbi{};
    std::uintptr_t addr = mod;
    while (addr < modEnd && VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi))) {
        const std::uintptr_t next =
            reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        const std::uintptr_t regionEnd = next < modEnd ? next : modEnd;
        const std::uintptr_t start = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const std::uintptr_t walk = start > addr ? start : addr;
        if (moduleWritableDataRegion(mbi) && regionEnd > walk)
            writable.push_back({walk, regionEnd});
        if (next <= addr)
            break;
        addr = next;
    }

    std::uintptr_t dataLo = 0;
    std::uintptr_t dataHi = 0;
    std::uintptr_t bssLo = 0;
    std::uintptr_t bssHi = 0;
    const bool hasData = peSectionRange(".data", dataLo, dataHi);
    const bool hasBss = peSectionRange(".bss", bssLo, bssHi);
    cache->dataLo = dataLo;
    cache->dataHi = dataHi;

    auto overlaps = [](std::uintptr_t a0, std::uintptr_t a1, std::uintptr_t b0, std::uintptr_t b1) {
        return a0 < b1 && a1 > b0;
    };
    auto appendOverlap = [&](std::uintptr_t slo, std::uintptr_t shi,
                             std::vector<std::pair<std::uintptr_t, std::uintptr_t>>& out) {
        if (shi <= slo)
            return;
        for (const auto& w : writable) {
            const std::uintptr_t lo = w.first > slo ? w.first : slo;
            const std::uintptr_t hi = w.second < shi ? w.second : shi;
            if (hi > lo + 2 * sizeof(void*))
                out.push_back({lo, hi});
        }
    };

    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> ordered;
    if (hasData)
        appendOverlap(dataLo, dataHi, ordered);
    if (hasBss)
        appendOverlap(bssLo, bssHi, ordered);

    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> rawHeap;
    rawHeap.reserve(8192);

    auto ingestSlot = [&](std::uintptr_t a, std::uintptr_t v) {
        if (v >= mod && v < modEnd) {
            if (nStatic < kMaxStatic && looksLikeVtable(a)) {
                ++nStatic;
                cache->dataPtrs.push_back({a, a});
            } else if (nStatic < kMaxStatic && looksLikeVtable(v)) {
                ++nStatic;
                cache->dataPtrs.push_back({a, v});
            }
            return;
        }
        if (!inHeap(v))
            return;
        ++heapSeen;
        rawHeap.push_back({a, v});
        std::uintptr_t rep = 0;
        const bool shared = memCopy(reinterpret_cast<const void*>(a + sizeof(void*)), &rep,
                                    sizeof(rep)) &&
                            looksLikeHeapPtr(rep) && looksLikeSharedRep(rep);
        if (shared) {
            ++nShared;
            if (looksLikeVtable(v))
                ++nVt;
            cache->dataPtrs.push_back({a, v});
            cache->heapPtrs.push_back({a, v});
        }
    };

    auto walkRangeBulk = [&](std::uintptr_t lo, std::uintptr_t hi) {
        if (hi < lo + 2 * sizeof(void*))
            return;
        constexpr std::size_t kChunk = 65536;
        std::vector<std::uint8_t> buf(kChunk);
        for (std::uintptr_t base = lo; base + sizeof(void*) <= hi;) {
            if (GetTickCount() - t0 > kBudgetMs)
                break;
            const std::size_t want = hi - base;
            const std::size_t n = want < kChunk ? want : kChunk;
            if (!memCopy(reinterpret_cast<const void*>(base), buf.data(), n))
                break;
            for (std::size_t off = 0; off + sizeof(void*) <= n; off += sizeof(void*)) {
                std::uintptr_t v = 0;
                std::memcpy(&v, buf.data() + off, sizeof(v));
                if (v)
                    ingestSlot(base + off, v);
            }
            if (base + n >= hi)
                break;
            // kChunk is pointer-aligned. Overlapping by sizeof(void*) - 1 shifted every
            // following Win32 chunk by one byte, turning .data pointers into unaligned
            // garbage and hiding module-owned shared_ptr anchors. Pointer slots are
            // aligned and cannot straddle these chunk boundaries, so advance by the
            // complete chunk and preserve the original address alignment.
            base += n;
        }
    };

    auto walkRange = [&](std::uintptr_t lo, std::uintptr_t hi, bool lowToHigh) {
        if (hi < lo + 2 * sizeof(void*))
            return;
        const std::uintptr_t span = hi - lo;
        for (std::uintptr_t off = 0; off + 2 * sizeof(void*) <= span; off += sizeof(void*)) {
            if (GetTickCount() - t0 > kBudgetMs)
                break;
            const std::uintptr_t a = lowToHigh ? lo + off : hi - sizeof(void*) - off;
            std::uintptr_t v = 0;
            if (!memCopy(reinterpret_cast<const void*>(a), &v, sizeof(v)) || !v)
                continue;
            ingestSlot(a, v);
        }
    };

    std::size_t nWalked = 0;
    std::unordered_set<std::uintptr_t> haveTarget;
    for (const auto& w : ordered) {
        if (GetTickCount() - t0 > kBudgetMs)
            break;
        ++nWalked;
        walkRangeBulk(w.first, w.second);
    }
    for (const auto& p : cache->dataPtrs)
        haveTarget.insert(p.second);
    if (hasData) {
        for (std::uintptr_t a = dataLo; a + 2 * sizeof(void*) <= dataHi;
             a += sizeof(void*)) {
            if (GetTickCount() - t0 > kBudgetMs)
                break;
            if (!sharedSlotAt(a))
                continue;
            std::uintptr_t v = 0;
            if (!readPtr(a, v) || !v || !looksLikeHeapPtr(v))
                continue;
            if (haveTarget.count(v))
                continue;
            ++nShared;
            if (looksLikeVtable(v))
                ++nVt;
            cache->dataPtrs.push_back({a, v});
            cache->heapPtrs.push_back({a, v});
            haveTarget.insert(v);
        }
    }
    for (auto it = writable.rbegin(); it != writable.rend(); ++it) {
        if (GetTickCount() - t0 > kBudgetMs)
            break;
        if ((hasData && overlaps(it->first, it->second, dataLo, dataHi)) ||
            (hasBss && overlaps(it->first, it->second, bssLo, bssHi)))
            continue;
        ++nWalked;
        walkRange(it->first, it->second, false);
    }

    for (const auto& p : cache->dataPtrs)
        haveTarget.insert(p.second);
    auto inDataSlot = [&](std::uintptr_t slot) {
        return hasData && slot >= dataLo && slot < dataHi;
    };
    std::unordered_set<std::uintptr_t> dataUniq;
    std::size_t nDataVt = 0;
    for (const auto& p : rawHeap) {
        if (!inDataSlot(p.first))
            continue;
        if (!dataUniq.insert(p.second).second)
            continue;
        if (haveTarget.count(p.second))
            continue;
        if (!looksLikeVtable(p.second) || !looksLikeHeapPtr(p.second) || vtKept >= kMaxVt)
            continue;
        ++nVt;
        ++vtKept;
        ++nDataVt;
        cache->dataPtrs.push_back(p);
        cache->heapPtrs.push_back(p);
        haveTarget.insert(p.second);
    }

    constexpr std::size_t kLogSlots = 32;
    std::size_t nLogged = 0;
    for (const auto& hp : cache->dataPtrs) {
        if (nLogged >= kLogSlots)
            break;
        if (!inDataSlot(hp.first))
            continue;
        const bool sh = sharedSlotAt(hp.first);
        const bool vt = looksLikeVtable(hp.second);
        const char* kind = sh ? "shared" : (vt ? "vtable" : "other");
        LOG("data slot rva=0x%X kind=%s obj=0x%p", static_cast<unsigned>(hp.first - mod), kind,
            reinterpret_cast<void*>(hp.second));
        ++nLogged;
    }

    cache->dataFilled = true;
    LOG("findChain data ptrs=%zu shared=%zu vtable=%zu dataVt=%zu staticObj=%zu heapSeen=%zu "
        "regions=%zu walked=%zu data=%d bss=%d ms=%u",
        cache->dataPtrs.size(), nShared, nVt, nDataVt, nStatic, heapSeen, writable.size(), nWalked,
        hasData ? 1 : 0, hasBss ? 1 : 0, GetTickCount() - t0);
}

bool quickInt32Hp(std::uintptr_t obj, const CalibHints& hints, std::size_t cap = 4096) {
    if (!hints.hasHealth)
        return false;
    const std::size_t n = readablePrefix(obj, cap);
    if (n < 8)
        return false;
    alignas(std::int32_t) std::uint8_t buf[4096]{};
    const std::size_t nread = n < sizeof(buf) ? n : sizeof(buf);
    if (!memCopy(reinterpret_cast<const void*>(obj), buf, nread))
        return false;
    for (std::size_t off = 0; off + 8 <= nread; off += 4) {
        std::int32_t a = 0;
        std::int32_t b = 0;
        std::memcpy(&a, buf + off, 4);
        std::memcpy(&b, buf + off + 4, 4);
        if (a == hints.health && b == hints.maxHealth)
            return true;
    }
    return false;
}

bool probeObjectStats(std::uintptr_t obj, const CalibHints& hints, std::uintptr_t& healthAddr,
                      Profile& layout, PlayerSnapshot& snap, std::size_t cap) {
    healthAddr = 0;
    const std::size_t n = readablePrefix(obj, cap);
    if (n < 16)
        return false;
    std::vector<std::uint8_t> buf(n);
    if (!memCopy(reinterpret_cast<const void*>(obj), buf.data(), n))
        return false;
    auto tryAt = [&](std::size_t off, StatType type, int stride) -> bool {
        Profile cand;
        PlayerSnapshot s;
        if (!tryLayouts(obj + off, type, stride, hints, cand, s))
            return false;
        healthAddr = obj + off;
        layout = std::move(cand);
        snap = s;
        return true;
    };
    for (std::size_t off = 0; off + 16 <= n; off += 8) {
        double a = 0;
        double b = 0;
        std::memcpy(&a, buf.data() + off, 8);
        std::memcpy(&b, buf.data() + off + 8, 8);
        if (nearEq(a, hints.health) && nearEq(b, hints.maxHealth) &&
            tryAt(off, StatType::Double, 8))
            return true;
        if (off + 24 <= n) {
            double c = 0;
            std::memcpy(&c, buf.data() + off + 16, 8);
            if (nearEq(a, hints.health) && nearEq(c, hints.maxHealth) &&
                tryAt(off, StatType::Double, 16))
                return true;
        }
    }
    for (std::size_t off = 0; off + 8 <= n; off += 4) {
        std::int32_t a = 0;
        std::int32_t b = 0;
        std::memcpy(&a, buf.data() + off, 4);
        std::memcpy(&b, buf.data() + off + 4, 4);
        if (a == hints.health && b == hints.maxHealth && a >= 0 && b > 0 &&
            tryAt(off, StatType::Int32, 4))
            return true;
    }
    return false;
}

bool locatePlayerFromDataPtrs(const CalibHints& hints, FindChainCache* cache, std::uintptr_t& hit,
                              Profile& layout, PlayerSnapshot& snap, std::vector<ChainStep>& chain) {
    fillDataPtrs(cache);
    const std::uintptr_t mod = cache->mod;
    if (!mod || cache->dataPtrs.empty())
        return false;

    struct Cand {
        std::uintptr_t health = 0;
        Profile layout;
        PlayerSnapshot snap;
        std::vector<ChainStep> steps;
        int healthOff = 0;
        bool vtable = false;
        bool shared = false;
        int score = -1;
    } best;

    auto consider = [&](std::uintptr_t obj, std::vector<ChainStep> steps, bool shared,
                        std::size_t cap = 8192) {
        std::uintptr_t health = 0;
        Profile cand;
        PlayerSnapshot s;
        if (!probeObjectStats(obj, hints, health, cand, s, cap) || !health)
            return;
        if (health < obj || health - obj > cap)
            return;
        const int healthOff = static_cast<int>(health - obj);
        if (healthOff % static_cast<int>(sizeof(void*)) != 0)
            return;
        if (healthOff != 0)
            steps.push_back({healthOff, false});
        const bool vt = looksLikeVtable(obj);
        const int score = (vt ? 10000 : 0) + (static_cast<int>(steps.size()) >= 3 ? 5000 : 0) +
                          (shared ? 2000 : 0) + healthOff;
        if (score > best.score) {
            best.health = health;
            best.layout = std::move(cand);
            best.snap = s;
            best.steps = std::move(steps);
            best.healthOff = healthOff;
            best.vtable = vt;
            best.shared = shared;
            best.score = score;
        }
    };

    const DWORD t0 = GetTickCount();
    constexpr DWORD kLocateBudgetMs = 16000;
    constexpr DWORD kGameProbeMs = 6000;
    constexpr DWORD kSharedTwoHopMs = 1500;
    constexpr DWORD kSharedTripleMs = 2000;
    constexpr DWORD kSharedQuadMs = 2000;
    constexpr int kGameWaves[] = {64, 256, 512, 1024, 2048, 4096};
    auto locateTimedOut = [&] { return GetTickCount() - t0 > kLocateBudgetMs; };
    auto gameTimedOut = [&](DWORD tGame) {
        return locateTimedOut() || GetTickCount() - tGame > kGameProbeMs;
    };
    auto phaseTimedOut = [&](DWORD tPhase, DWORD budgetMs) {
        return locateTimedOut() || GetTickCount() - tPhase > budgetMs;
    };
    auto chainStrong = [&](const std::vector<ChainStep>& steps, bool vt) {
        if (vt)
            return true;
        if (steps.size() >= 3)
            return true;
        if (steps.size() >= 2 && !steps.empty() && steps[0].deref &&
            sharedSlotAt(mod + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(steps[0].offset))))
            return true;
        return false;
    };

    struct PartialHop {
        int depth = 0;
        int off1 = 0;
        int off2 = 0;
        int off3 = 0;
        std::int32_t rva = 0;
        std::uintptr_t inner = 0;
        bool hpOnly = false;
        bool fullStats = false;
    };
    PartialHop bestPartial;
    int bestPartialScore = -1;
    auto notePartial = [&](int depth, std::int32_t rva, int o1, int o2, int o3, std::uintptr_t inner) {
        if (!inner || inner < 0x01000000 || !looksLikeHeapPtr(inner))
            return;
        const bool hp = quickInt32Hp(inner, hints);
        bool full = false;
        if (hp) {
            std::uintptr_t health = 0;
            Profile cand;
            PlayerSnapshot s;
            full = probeObjectStats(inner, hints, health, cand, s, 8192);
        }
        if (!hp && !full)
            return;
        const int score = (full ? 100000 : 0) + (hp ? 10000 : 0) + depth * 100 +
                          (o1 > 0 ? o1 / static_cast<int>(sizeof(void*)) : 0);
        if (score > bestPartialScore) {
            bestPartialScore = score;
            bestPartial = {depth, o1, o2, o3, rva, inner, hp && !full, full};
        }
    };

    auto followInner = [&](std::uintptr_t mid, std::int32_t rva, int off) {
        std::uintptr_t inner = 0;
        if (!readPtr(mid + static_cast<std::uintptr_t>(off), inner) || !inner)
            return;
        if (inner >= mod && inner < mod + cache->img)
            return;
        if (!looksLikeHeapPtr(inner))
            return;
        if (!quickInt32Hp(inner, hints))
            return;
        if (sharedSlotAt(mod + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(rva))))
            notePartial(2, rva, off, 0, 0, inner);
        consider(inner,
                 {{rva, mid != mod + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(rva))},
                  {off, true}},
                 sharedSlotAt(mid + static_cast<std::uintptr_t>(off)));
    };

    auto followTripleFromGame = [&](std::uintptr_t game, std::int32_t rva, int prevWave, int wave,
                                    DWORD tPhase, DWORD budgetMs) {
        const std::size_t nGame = readablePrefix(game, static_cast<std::size_t>(wave));
        for (int off1 = prevWave; off1 + static_cast<int>(sizeof(void*)) <= static_cast<int>(nGame);
             off1 += static_cast<int>(sizeof(void*))) {
            if (phaseTimedOut(tPhase, budgetMs))
                break;
            std::uintptr_t hop = 0;
            if (!readPtr(game + static_cast<std::uintptr_t>(off1), hop) || !hop)
                continue;
            if (hop >= mod && hop < mod + cache->img)
                continue;
            if (!looksLikeHeapPtr(hop))
                continue;
            const std::size_t nHop = readablePrefix(hop, 4096);
            for (int off2 = 0; off2 + static_cast<int>(sizeof(void*)) <= static_cast<int>(nHop);
                 off2 += static_cast<int>(sizeof(void*))) {
                if (phaseTimedOut(tPhase, budgetMs))
                    break;
                std::uintptr_t inner = 0;
                if (!readPtr(hop + static_cast<std::uintptr_t>(off2), inner) || !inner)
                    continue;
                if (inner >= mod && inner < mod + cache->img)
                    continue;
                if (!looksLikeHeapPtr(inner))
                    continue;
                notePartial(3, rva, off1, off2, 0, inner);
                consider(inner, {{rva, true}, {off1, true}, {off2, true}}, true);
                if (best.score >= 0 && chainStrong(best.steps, best.vtable))
                    return;
            }
        }
    };

    auto followQuadFromGame = [&](std::uintptr_t game, std::int32_t rva, int prevWave, int wave,
                                  DWORD tPhase, DWORD budgetMs) {
        constexpr int kHopMax = 1024;
        const std::size_t nGame = readablePrefix(game, static_cast<std::size_t>(wave));
        const int lim1 = static_cast<int>(nGame < 2048 ? nGame : 2048) - static_cast<int>(sizeof(void*));
        for (int off1 = prevWave; off1 <= lim1; off1 += static_cast<int>(sizeof(void*))) {
            if (phaseTimedOut(tPhase, budgetMs))
                break;
            std::uintptr_t hop1 = 0;
            if (!readPtr(game + static_cast<std::uintptr_t>(off1), hop1) || !hop1)
                continue;
            if (hop1 >= mod && hop1 < mod + cache->img)
                continue;
            if (!looksLikeHeapPtr(hop1))
                continue;
            const std::size_t nHop1 = readablePrefix(hop1, static_cast<std::size_t>(kHopMax));
            const int lim2 = static_cast<int>(nHop1) - static_cast<int>(sizeof(void*));
            for (int off2 = 0; off2 <= lim2; off2 += static_cast<int>(sizeof(void*))) {
                if (phaseTimedOut(tPhase, budgetMs))
                    break;
                std::uintptr_t hop2 = 0;
                if (!readPtr(hop1 + static_cast<std::uintptr_t>(off2), hop2) || !hop2)
                    continue;
                if (hop2 >= mod && hop2 < mod + cache->img)
                    continue;
                if (!looksLikeHeapPtr(hop2))
                    continue;
                const std::size_t nHop2 = readablePrefix(hop2, static_cast<std::size_t>(kHopMax));
                const int lim3 = static_cast<int>(nHop2) - static_cast<int>(sizeof(void*));
                for (int off3 = 0; off3 <= lim3; off3 += static_cast<int>(sizeof(void*))) {
                    if (phaseTimedOut(tPhase, budgetMs))
                        break;
                    std::uintptr_t inner = 0;
                    if (!readPtr(hop2 + static_cast<std::uintptr_t>(off3), inner) || !inner)
                        continue;
                    if (inner >= mod && inner < mod + cache->img)
                        continue;
                    if (!looksLikeHeapPtr(inner))
                        continue;
                    notePartial(4, rva, off1, off2, off3, inner);
                    consider(inner, {{rva, true}, {off1, true}, {off2, true}, {off3, true}}, true);
                    if (best.score >= 0 && chainStrong(best.steps, best.vtable))
                        return;
                }
            }
        }
    };

    struct GameCand {
        std::uintptr_t obj;
        std::int32_t rva;
        std::uintptr_t slot;
    };
    std::vector<GameCand> games;
    const auto inDataSlot = [&](std::uintptr_t slot) {
        return cache->dataLo < cache->dataHi && slot >= cache->dataLo && slot < cache->dataHi;
    };
    for (const auto& hp : cache->dataPtrs) {
        if (!inDataSlot(hp.first))
            continue;
        const bool sh = sharedSlotAt(hp.first);
        if (!looksLikeVtable(hp.second) && !sh)
            continue;
        if (!looksLikeHeapPtr(hp.second))
            continue;
        games.push_back({hp.second, static_cast<std::int32_t>(hp.first - mod), hp.first});
    }
    std::sort(games.begin(), games.end(), [&](const GameCand& a, const GameCand& b) {
        const bool shA = sharedSlotAt(a.slot);
        const bool shB = sharedSlotAt(b.slot);
        if (shA != shB)
            return shA > shB;
        return looksLikeVtable(a.obj) > looksLikeVtable(b.obj);
    });
    constexpr std::size_t kMaxGames = 48;
    if (games.size() > kMaxGames)
        games.resize(kMaxGames);

    int nSharedProbe = 0;
    int nGameProbe = static_cast<int>(games.size());
    int nGameInner = 0;
    int gameWave = 0;

    // The semantic Game anchor often starts with the LocalPlayer handle. Probe that
    // bounded member before walking unrelated shared objects deeply. Full P1 hints
    // still have to match and the pointed object must provide the strong vtable
    // evidence, so this cannot turn a loose HP copy into an accepted chain.
    if (!chainStrong(best.steps, best.vtable)) {
        if (!cache->gameAnchorTried) {
            cache->gameAnchorTried = true;
            resolveCalibrationGameAnchors(cache->gameRvas);
        }
        for (const auto semanticRva : cache->gameRvas) {
            const std::uintptr_t slot =
                mod + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(semanticRva));
            std::uintptr_t child = 0;
            if (!readPtr(slot, child) || !child || !looksLikeHeapPtr(child))
                continue;
            const bool childShared = sharedSlotAt(slot);
            if (!childShared && !looksLikeVtable(child))
                continue;
            consider(child, {{semanticRva, false}, {0, true}}, childShared, 8192);
            LOG("scan semantic-slot rva=0x%X obj=0x%p score=%d steps=%zu healthOff=%d strong=%d",
                static_cast<unsigned>(semanticRva), reinterpret_cast<void*>(child), best.score,
                best.steps.size(), best.healthOff,
                (best.score >= 0 && chainStrong(best.steps, best.vtable)) ? 1 : 0);
            if (best.score >= 0 && chainStrong(best.steps, best.vtable))
                break;
        }
    }

    if (!chainStrong(best.steps, best.vtable)) {
        LOG("scan Game candidates dataSlots=%zu (vtable+shared)", games.size());
        const DWORD tGame = GetTickCount();
        int prev = 0;
        for (int wave : kGameWaves) {
            if (gameTimedOut(tGame))
                break;
            gameWave = wave;
            for (const auto& g : games) {
                if (gameTimedOut(tGame))
                    break;
                const std::size_t n = readablePrefix(g.obj, static_cast<std::size_t>(wave));
                for (int off = prev; off + static_cast<int>(sizeof(void*)) <= static_cast<int>(n);
                     off += static_cast<int>(sizeof(void*))) {
                    if (gameTimedOut(tGame))
                        break;
                    ++nGameInner;
                    followInner(g.obj, g.rva, off);
                    if (best.score >= 0 && chainStrong(best.steps, best.vtable))
                        break;
                }
                if (best.score >= 0 && chainStrong(best.steps, best.vtable))
                    break;
            }
            if (best.score >= 0 && chainStrong(best.steps, best.vtable))
                break;
            prev = wave;
        }
    }

    for (const auto& g : games) {
        if (!sharedSlotAt(g.slot))
            continue;
        if (locateTimedOut())
            break;
        if (best.score >= 0 && chainStrong(best.steps, best.vtable))
            break;
        ++nSharedProbe;
        LOG("shared probe rva=0x%X obj=0x%p prefix=%zu vt=%d",
            static_cast<unsigned>(g.slot - mod), reinterpret_cast<void*>(g.obj),
            readablePrefix(g.obj, 8192), looksLikeVtable(g.obj) ? 1 : 0);
        consider(g.obj, {{g.rva, g.slot != g.obj}}, true, 8192);
        if (best.score >= 0 && chainStrong(best.steps, best.vtable))
            continue;
        const DWORD tTwoHop = GetTickCount();
        int prevShared = 0;
        for (int wave : kGameWaves) {
            if (phaseTimedOut(tTwoHop, kSharedTwoHopMs))
                break;
            const std::size_t n = readablePrefix(g.obj, static_cast<std::size_t>(wave));
            for (int off = prevShared; off + static_cast<int>(sizeof(void*)) <= static_cast<int>(n);
                 off += static_cast<int>(sizeof(void*))) {
                if (phaseTimedOut(tTwoHop, kSharedTwoHopMs))
                    break;
                followInner(g.obj, g.rva, off);
                if (best.score >= 0 && chainStrong(best.steps, best.vtable))
                    break;
            }
            if (best.score >= 0 && chainStrong(best.steps, best.vtable))
                break;
            prevShared = wave;
        }
        if (!(best.score >= 0 && chainStrong(best.steps, best.vtable))) {
            const DWORD tTriple = GetTickCount();
            int prevTriple = 0;
            for (int wave : kGameWaves) {
                if (phaseTimedOut(tTriple, kSharedTripleMs))
                    break;
                followTripleFromGame(g.obj, g.rva, prevTriple, wave, tTriple, kSharedTripleMs);
                if (best.score >= 0 && chainStrong(best.steps, best.vtable))
                    break;
                prevTriple = wave;
            }
        }
        if (!(best.score >= 0 && chainStrong(best.steps, best.vtable))) {
            const DWORD tQuad = GetTickCount();
            int prevQuad = 0;
            for (int wave : kGameWaves) {
                if (phaseTimedOut(tQuad, kSharedQuadMs))
                    break;
                followQuadFromGame(g.obj, g.rva, prevQuad, wave, tQuad, kSharedQuadMs);
                if (best.score >= 0 && chainStrong(best.steps, best.vtable))
                    break;
                prevQuad = wave;
            }
        }
    }

    int semanticFields = 0;
    if (!(best.score >= 0 && chainStrong(best.steps, best.vtable))) {
        if (!cache->gameAnchorTried) {
            cache->gameAnchorTried = true;
            resolveCalibrationGameAnchors(cache->gameRvas);
        }
        for (const auto semanticRva : cache->gameRvas) {
            const std::uintptr_t game =
                mod + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(semanticRva));
            constexpr std::size_t kSemanticBytes = 4096;
            alignas(std::uintptr_t) std::uint8_t gameBuf[kSemanticBytes]{};
            alignas(std::uintptr_t) std::uint8_t holderBuf[kSemanticBytes]{};
            const std::size_t nGame = readablePrefix(game, sizeof(gameBuf));
            if (nGame >= sizeof(void*) &&
                memCopy(reinterpret_cast<const void*>(game), gameBuf, nGame)) {
                for (std::size_t off = 0; off + sizeof(void*) <= nGame; off += sizeof(void*)) {
                    std::uintptr_t child = 0;
                    std::memcpy(&child, gameBuf + off, sizeof(child));
                    if (!child || !looksLikeHeapPtr(child))
                        continue;
                    const bool childShared = sharedSlotAt(game + off);
                    const bool childVtable = looksLikeVtable(child);
                    const bool childHasHp = quickInt32Hp(child, hints, 4096);
                    if (!childShared && !childVtable && !childHasHp)
                        continue;
                    ++semanticFields;
                    const std::vector<ChainStep> direct{
                        {semanticRva, false}, {static_cast<std::int32_t>(off), true}};
                    // The semantic Game member is already a bounded candidate.
                    // Probe all shared/vtable members fully: some clients keep
                    // LocalPlayer stats as doubles, so an Int32-only HP prefilter
                    // would skip the real Game+0 LocalPlayer and accept a copy.
                    consider(child, direct, childShared, 8192);
                    if (best.score >= 0 && chainStrong(best.steps, best.vtable))
                        break;

                    const std::size_t nHolder = readablePrefix(child, sizeof(holderBuf));
                    if (nHolder < sizeof(void*) ||
                        !memCopy(reinterpret_cast<const void*>(child), holderBuf, nHolder))
                        continue;
                    for (std::size_t innerOff = 0;
                         innerOff + sizeof(void*) <= nHolder; innerOff += sizeof(void*)) {
                        std::uintptr_t inner = 0;
                        std::memcpy(&inner, holderBuf + innerOff, sizeof(inner));
                        if (!inner || !looksLikeHeapPtr(inner))
                            continue;
                        const bool innerShared = sharedSlotAt(child + innerOff);
                        if (!innerShared && !looksLikeVtable(inner))
                            continue;
                        std::vector<ChainStep> nested = direct;
                        nested.push_back({static_cast<std::int32_t>(innerOff), true});
                        consider(inner, std::move(nested), innerShared, 8192);
                        if (best.score >= 0 && chainStrong(best.steps, best.vtable))
                            break;
                    }
                    if (best.score >= 0 && chainStrong(best.steps, best.vtable))
                        break;
                }
            }
            LOG("scan semantic-game rva=%d fields=%d score=%d steps=%zu healthOff=%d strong=%d",
                semanticRva, semanticFields, best.score, best.steps.size(), best.healthOff,
                (best.score >= 0 && chainStrong(best.steps, best.vtable)) ? 1 : 0);
            if (best.score >= 0 && chainStrong(best.steps, best.vtable))
                break;
        }
    }

    const bool strong = best.score >= 0 && chainStrong(best.steps, best.vtable);
    if (!strong && bestPartial.inner && (bestPartial.hpOnly || bestPartial.fullStats)) {
        std::vector<ChainStep> steps;
        steps.push_back({bestPartial.rva, true});
        if (bestPartial.depth >= 3) {
            steps.push_back({bestPartial.off1, true});
            steps.push_back({bestPartial.off2, true});
        }
        if (bestPartial.depth >= 4)
            steps.push_back({bestPartial.off3, true});
        consider(bestPartial.inner, std::move(steps), true, 8192);
    }
    const bool strongFinal = best.score >= 0 && chainStrong(best.steps, best.vtable);
    if (!strongFinal && bestPartial.inner) {
        LOG("shared chain partial rva=0x%X depth=%d off1=%d off2=%d off3=%d inner=0x%p hpOnly=%d "
            "full=%d",
            static_cast<unsigned>(bestPartial.rva), bestPartial.depth, bestPartial.off1,
            bestPartial.off2, bestPartial.off3, reinterpret_cast<void*>(bestPartial.inner),
            bestPartial.hpOnly ? 1 : 0, bestPartial.fullStats ? 1 : 0);
    }
    LOG("scan via module-data ptrs=%zu sharedProbe=%d gameProbe=%d gameInner=%d gameWave=%d "
        "score=%d vtable=%d shared=%d steps=%zu healthOff=%d strong=%d ms=%u",
        cache->dataPtrs.size(), nSharedProbe, nGameProbe, nGameInner, gameWave, best.score,
        best.vtable ? 1 : 0, best.shared ? 1 : 0, best.steps.size(), best.healthOff,
        strongFinal ? 1 : 0, GetTickCount() - t0);
    if (!strongFinal)
        return false;
    hit = best.health;
    layout = std::move(best.layout);
    snap = best.snap;
    chain = std::move(best.steps);
    return true;
}

void prepareHeapMids(FindChainCache* cache, std::uintptr_t statsBase) {
    if (cache->midsReady)
        return;
    const DWORD t0 = GetTickCount();
    constexpr std::uintptr_t kNear = 256ull * 1024 * 1024;
    constexpr std::size_t kMaxMids = 4096;
    constexpr DWORD kBudgetMs = 4000;
    std::unordered_set<std::uintptr_t> seen;
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> nearPtrs;
    nearPtrs.reserve(8192);
    LOG("findChain prepare start ptrs=%zu ranges=%zu stats=0x%p", cache->heapPtrs.size(),
        cache->heapRanges.size(), reinterpret_cast<void*>(statsBase));
    for (const auto& hp : cache->heapPtrs) {
        const std::uintptr_t dist =
            hp.second > statsBase ? hp.second - statsBase : statsBase - hp.second;
        if (dist > kNear)
            continue;
        if (!seen.insert(hp.second).second)
            continue;
        nearPtrs.push_back(hp);
    }
    std::sort(nearPtrs.begin(), nearPtrs.end(),
              [statsBase](const std::pair<std::uintptr_t, std::uintptr_t>& a,
                          const std::pair<std::uintptr_t, std::uintptr_t>& b) {
                  const auto da = a.second > statsBase ? a.second - statsBase : statsBase - a.second;
                  const auto db = b.second > statsBase ? b.second - statsBase : statsBase - b.second;
                  return da < db;
              });
    cache->heapMids.clear();
    for (const auto& hp : nearPtrs) {
        if (GetTickCount() - t0 > kBudgetMs)
            break;
        if (looksLikeVtable(hp.second))
            cache->heapMids.push_back(hp);
        if (cache->heapMids.size() >= kMaxMids)
            break;
    }
    if (cache->heapMids.empty()) {
        seen.clear();
        for (const auto& hp : cache->heapPtrs) {
            if (GetTickCount() - t0 > kBudgetMs)
                break;
            if (!seen.insert(hp.second).second)
                continue;
            if (looksLikeVtable(hp.second))
                cache->heapMids.push_back(hp);
            if (cache->heapMids.size() >= kMaxMids)
                break;
        }
    }
    cache->midsReady = true;
    LOG("findChain prepare near=%zu mids=%zu ms=%u", nearPtrs.size(), cache->heapMids.size(),
        GetTickCount() - t0);
}

std::vector<ChainStep> findChain(std::uintptr_t statsBase, FindChainCache* cache) {
    std::vector<ChainStep> found;
    FindChainCache local;
    if (!cache)
        cache = &local;
    initChainCache(cache);
    fillDataPtrs(cache);
    const std::uintptr_t mod = cache->mod;
    if (!mod || !cache->img || !statsBase)
        return found;

    constexpr std::uintptr_t kMaxHealthOff = 4096;
    DWORD deadline = GetTickCount() + 4000;
    auto timedOut = [&] { return GetTickCount() > deadline; };

    struct Cand {
        std::vector<ChainStep> steps;
        int healthOff = 0;
        bool vtable = false;
    };
    Cand best;
    int bestScore = -1;

    auto consider = [&](std::vector<ChainStep> steps, std::uintptr_t object, bool shared) {
        if (object > statsBase || statsBase - object > kMaxHealthOff)
            return;
        const int healthOff = static_cast<int>(statsBase - object);
        if (healthOff % static_cast<int>(sizeof(void*)) != 0)
            return;
        if (healthOff != 0)
            steps.push_back({healthOff, false});
        const bool vt = looksLikeVtable(object);
        int score = (vt ? 10000 : 0) + (static_cast<int>(steps.size()) >= 3 ? 5000 : 0) +
                    (shared ? 2000 : 0) + healthOff;
        if (score > bestScore) {
            bestScore = score;
            best = {std::move(steps), healthOff, vt};
        }
    };

    auto twoStep = [&](const std::vector<std::pair<std::uintptr_t, std::uintptr_t>>& mids,
                       int innerMax) {
        alignas(std::uintptr_t) std::uint8_t innerBuf[4096 + sizeof(void*)]{};
        for (const auto& hp : mids) {
            if (timedOut())
                break;
            const std::uintptr_t mid = hp.second;
            const std::size_t nread = readablePrefix(mid, sizeof(innerBuf));
            if (nread < sizeof(void*))
                continue;
            if (!memCopy(reinterpret_cast<const void*>(mid), innerBuf, nread))
                continue;
            const int offMax = static_cast<int>(nread - sizeof(void*));
            const int cap = innerMax < offMax ? innerMax : offMax;
            for (int off = 0; off <= cap; off += static_cast<int>(sizeof(void*))) {
                std::uintptr_t inner = 0;
                std::memcpy(&inner, innerBuf + off, sizeof(inner));
                if (inner >= statsBase - kMaxHealthOff && inner <= statsBase) {
                    consider({{static_cast<std::int32_t>(hp.first - mod), true}, {off, true}}, inner,
                             sharedSlotAt(mid + static_cast<std::uintptr_t>(off)));
                }
            }
        }
    };

    auto threeStep = [&](const std::vector<std::pair<std::uintptr_t, std::uintptr_t>>& mids,
                         int innerMax) {
        alignas(std::uintptr_t) std::uint8_t midBuf[4096 + sizeof(void*)]{};
        alignas(std::uintptr_t) std::uint8_t hopBuf[4096 + sizeof(void*)]{};
        for (const auto& hp : mids) {
            if (timedOut())
                break;
            const std::uintptr_t mid = hp.second;
            const std::size_t nMid = readablePrefix(mid, sizeof(midBuf));
            if (nMid < sizeof(void*))
                continue;
            if (!memCopy(reinterpret_cast<const void*>(mid), midBuf, nMid))
                continue;
            const int cap1 = static_cast<int>(nMid - sizeof(void*));
            const int lim1 = innerMax < cap1 ? innerMax : cap1;
            for (int off1 = 0; off1 <= lim1; off1 += static_cast<int>(sizeof(void*))) {
                if (timedOut())
                    break;
                std::uintptr_t hop = 0;
                std::memcpy(&hop, midBuf + off1, sizeof(hop));
                if (!hop || (hop >= mod && hop < mod + cache->img))
                    continue;
                if (!looksLikeHeapPtr(hop))
                    continue;
                const std::size_t nHop = readablePrefix(hop, sizeof(hopBuf));
                if (nHop < sizeof(void*))
                    continue;
                if (!memCopy(reinterpret_cast<const void*>(hop), hopBuf, nHop))
                    continue;
                const int cap2 = static_cast<int>(nHop - sizeof(void*));
                const int lim2 = innerMax < cap2 ? innerMax : cap2;
                for (int off2 = 0; off2 <= lim2; off2 += static_cast<int>(sizeof(void*))) {
                    std::uintptr_t inner = 0;
                    std::memcpy(&inner, hopBuf + off2, sizeof(inner));
                    if (inner >= statsBase - kMaxHealthOff && inner <= statsBase) {
                        consider({{static_cast<std::int32_t>(hp.first - mod), true}, {off1, true},
                                  {off2, true}},
                                 inner, sharedSlotAt(hp.first));
                    }
                }
            }
        }
    };

    auto fourStep = [&](const std::vector<std::pair<std::uintptr_t, std::uintptr_t>>& mids,
                        int gameMax, int hopMax) {
        alignas(std::uintptr_t) std::uint8_t b0[4096 + sizeof(void*)]{};
        alignas(std::uintptr_t) std::uint8_t b1[1024 + sizeof(void*)]{};
        alignas(std::uintptr_t) std::uint8_t b2[1024 + sizeof(void*)]{};
        for (const auto& hp : mids) {
            if (timedOut())
                break;
            if (!sharedSlotAt(hp.first))
                continue;
            const std::uintptr_t mid = hp.second;
            const std::size_t n0 = readablePrefix(mid, sizeof(b0));
            if (n0 < sizeof(void*))
                continue;
            if (!memCopy(reinterpret_cast<const void*>(mid), b0, n0))
                continue;
            const int cap0 = static_cast<int>(n0 - sizeof(void*));
            const int lim0 = gameMax < cap0 ? gameMax : cap0;
            for (int off1 = 0; off1 <= lim0; off1 += static_cast<int>(sizeof(void*))) {
                if (timedOut())
                    break;
                std::uintptr_t hop1 = 0;
                std::memcpy(&hop1, b0 + off1, sizeof(hop1));
                if (!hop1 || (hop1 >= mod && hop1 < mod + cache->img))
                    continue;
                if (!looksLikeHeapPtr(hop1))
                    continue;
                const std::size_t n1 = readablePrefix(hop1, sizeof(b1));
                if (n1 < sizeof(void*))
                    continue;
                if (!memCopy(reinterpret_cast<const void*>(hop1), b1, n1))
                    continue;
                const int cap1 = static_cast<int>(n1 - sizeof(void*));
                const int lim1 = hopMax < cap1 ? hopMax : cap1;
                for (int off2 = 0; off2 <= lim1; off2 += static_cast<int>(sizeof(void*))) {
                    if (timedOut())
                        break;
                    std::uintptr_t hop2 = 0;
                    std::memcpy(&hop2, b1 + off2, sizeof(hop2));
                    if (!hop2 || (hop2 >= mod && hop2 < mod + cache->img))
                        continue;
                    if (!looksLikeHeapPtr(hop2))
                        continue;
                    const std::size_t n2 = readablePrefix(hop2, sizeof(b2));
                    if (n2 < sizeof(void*))
                        continue;
                    if (!memCopy(reinterpret_cast<const void*>(hop2), b2, n2))
                        continue;
                    const int cap2 = static_cast<int>(n2 - sizeof(void*));
                    const int lim2 = hopMax < cap2 ? hopMax : cap2;
                    for (int off3 = 0; off3 <= lim2; off3 += static_cast<int>(sizeof(void*))) {
                        std::uintptr_t inner = 0;
                        std::memcpy(&inner, b2 + off3, sizeof(inner));
                        if (inner >= statsBase - kMaxHealthOff && inner <= statsBase) {
                            consider({{static_cast<std::int32_t>(hp.first - mod), true}, {off1, true},
                                      {off2, true}, {off3, true}},
                                     inner, true);
                        }
                    }
                }
            }
        }
    };

    for (const auto& hp : cache->dataPtrs) {
        if (hp.second >= statsBase - kMaxHealthOff && hp.second <= statsBase)
            consider({{static_cast<std::int32_t>(hp.first - mod), true}}, hp.second,
                     sharedSlotAt(hp.first));
    }
    for (const auto& hp : cache->heapPtrs) {
        if (hp.second >= statsBase - kMaxHealthOff && hp.second <= statsBase)
            consider({{static_cast<std::int32_t>(hp.first - mod), true}}, hp.second,
                     sharedSlotAt(hp.first));
    }
    if (best.vtable && best.healthOff >= 32) {
        LOG("pointer-chain candidate healthOff=%d vtable=%d steps=%zu via=1-deref", best.healthOff,
            best.vtable ? 1 : 0, best.steps.size());
        return best.steps;
    }
    if (best.steps.size() >= 2 && best.steps[0].deref &&
        sharedSlotAt(mod + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(best.steps[0].offset)))) {
        LOG("pointer-chain candidate healthOff=%d vtable=%d steps=%zu via=shared", best.healthOff,
            best.vtable ? 1 : 0, best.steps.size());
        return best.steps;
    }

    deadline = GetTickCount() + 8000;
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> dataGamePtrs;
    dataGamePtrs.reserve(cache->dataPtrs.size());
    std::size_t nDataShared = 0;
    for (const auto& hp : cache->dataPtrs) {
        if (cache->dataLo >= cache->dataHi || hp.first < cache->dataLo || hp.first >= cache->dataHi)
            continue;
        const bool sh = sharedSlotAt(hp.first);
        const bool vt = looksLikeVtable(hp.second);
        if (!vt && !sh)
            continue;
        if (!looksLikeHeapPtr(hp.second))
            continue;
        if (sh)
            ++nDataShared;
        dataGamePtrs.push_back(hp);
    }
    std::sort(dataGamePtrs.begin(), dataGamePtrs.end(),
              [&](const std::pair<std::uintptr_t, std::uintptr_t>& a,
                  const std::pair<std::uintptr_t, std::uintptr_t>& b) {
                  const bool shA = sharedSlotAt(a.first);
                  const bool shB = sharedSlotAt(b.first);
                  if (shA != shB)
                      return shA > shB;
                  return looksLikeVtable(a.second) > looksLikeVtable(b.second);
              });
    LOG("findChain dataGame=%zu shared=%zu stats=0x%p", dataGamePtrs.size(), nDataShared,
        reinterpret_cast<void*>(statsBase));
    twoStep(dataGamePtrs, 4096);
    if (best.vtable || static_cast<int>(best.steps.size()) >= 3) {
        LOG("pointer-chain candidate healthOff=%d vtable=%d steps=%zu via=dataGame", best.healthOff,
            best.vtable ? 1 : 0, best.steps.size());
        return best.steps;
    }
    deadline = GetTickCount() + 8000;
    threeStep(dataGamePtrs, 4096);
    if (best.vtable || static_cast<int>(best.steps.size()) >= 3) {
        LOG("pointer-chain candidate healthOff=%d vtable=%d steps=%zu via=dataGame3", best.healthOff,
            best.vtable ? 1 : 0, best.steps.size());
        return best.steps;
    }
    deadline = GetTickCount() + 8000;
    fourStep(dataGamePtrs, 2048, 512);
    if (best.vtable || static_cast<int>(best.steps.size()) >= 3) {
        LOG("pointer-chain candidate healthOff=%d vtable=%d steps=%zu via=dataGame4", best.healthOff,
            best.vtable ? 1 : 0, best.steps.size());
        return best.steps;
    }
    deadline = GetTickCount() + 4000;
    twoStep(cache->dataPtrs, 4096);
    if (best.vtable || static_cast<int>(best.steps.size()) >= 3) {
        LOG("pointer-chain candidate healthOff=%d vtable=%d steps=%zu via=data", best.healthOff,
            best.vtable ? 1 : 0, best.steps.size());
        return best.steps;
    }
    deadline = GetTickCount() + 4000;
    threeStep(cache->dataPtrs, 4096);
    if (best.vtable || static_cast<int>(best.steps.size()) >= 3) {
        LOG("pointer-chain candidate healthOff=%d vtable=%d steps=%zu via=data3", best.healthOff,
            best.vtable ? 1 : 0, best.steps.size());
        return best.steps;
    }

    if (!cache->midsReady)
        prepareHeapMids(cache, statsBase);
    deadline = GetTickCount() + 4000;
    twoStep(cache->heapMids, 4096);
    if (best.vtable || static_cast<int>(best.steps.size()) >= 3) {
        LOG("pointer-chain candidate healthOff=%d vtable=%d steps=%zu via=mids", best.healthOff,
            best.vtable ? 1 : 0, best.steps.size());
        return best.steps;
    }
    deadline = GetTickCount() + 4000;
    threeStep(cache->heapMids, 4096);
    if (best.vtable || static_cast<int>(best.steps.size()) >= 3) {
        LOG("pointer-chain candidate healthOff=%d vtable=%d steps=%zu via=mids3", best.healthOff,
            best.vtable ? 1 : 0, best.steps.size());
        return best.steps;
    }

    if (bestScore >= 0) {
        LOG("pointer-chain candidate healthOff=%d vtable=%d steps=%zu via=weak", best.healthOff,
            best.vtable ? 1 : 0, best.steps.size());
        return best.steps;
    }
    LOG("pointer-chain miss for statsBase=0x%p dataPtrs=%zu", reinterpret_cast<void*>(statsBase),
        cache->dataPtrs.size());
    return found;
}

bool heapScan() {
    CalibHints hints;
    std::string buildId;
    Profile priorProfile;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        hints = g_hints;
        buildId = g_build.id;
        priorProfile = g_profile;
    }
    if (!hints.hasHealth || !hints.hasLevel) {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_phase = "needCalibration";
        g_phaseMsg = "hints need at least health/maxHealth and level";
        return false;
    }

    Profile layout;
    std::uintptr_t hit = 0;
    int hitScale = 1;
    std::vector<ChainStep> hitChain;
    PlayerSnapshot hitSnap;
    FindChainCache chainCache;

    if (locatePlayerFromDataPtrs(hints, &chainCache, hit, layout, hitSnap, hitChain)) {
        Profile stable = layout;
        stable.chain = hitChain;
        bool stableOk = true;
        for (int pass = 0; pass < 2; ++pass) {
            Sleep(125);
            const std::uintptr_t resolved = resolveChain(stable);
            if (!resolved || !matchHints(resolved, stable, hints)) {
                stableOk = false;
                break;
            }
        }
        if (stableOk) {
            hitScale = layout.capacityScale;
            LOG("scan hit via module-data statsBase=0x%p type=%d scale=%d chainSteps=%zu",
                reinterpret_cast<void*>(hit), static_cast<int>(layout.statType), hitScale,
                hitChain.size());
        } else {
            LOG("module-data chain changed during stability check — keep scanning");
            hit = 0;
            hitChain.clear();
            hitSnap = PlayerSnapshot{};
        }
    }

    const DWORD t0 = GetTickCount();
    std::size_t scanned = 0;
    std::size_t hpPairs = 0;
    bool timedOut = false;
    std::uintptr_t fallbackHit = 0;
    DWORD extraUntil = 0;
    bool sawInt32Pair = false;

    auto timeGone = [&] {
        const DWORD now = GetTickCount();
        if (extraUntil != 0 && now < extraUntil)
            return false;
        if (now - t0 > kScanBudgetMs) {
            timedOut = true;
            return true;
        }
        return false;
    };

    auto budgetGone = [&] {
        if (scanned > kMaxScanBytes)
            return true;
        return timeGone();
    };

    auto consider = [&](std::uintptr_t healthAddr, StatType type, int stride) -> bool {
        ++hpPairs;
        Profile cand;
        PlayerSnapshot snap;
        if (!tryLayouts(healthAddr, type, stride, hints, cand, snap)) {
            if (type == StatType::Int32) {
                sawInt32Pair = true;
                LOG("int32 hp-pair layouts miss at 0x%p", reinterpret_cast<void*>(healthAddr));
            }
            return false;
        }
        if (type == StatType::Int32)
            sawInt32Pair = true;
        const DWORD tChain0 = GetTickCount();
        auto chain = findChain(healthAddr, &chainCache);
        if (extraUntil != 0) {
            const DWORD spent = GetTickCount() - tChain0;
            extraUntil += spent > 8000 ? 8000 : spent;
        }
        if (!chain.empty()) {
            std::uintptr_t obj = healthAddr;
            int healthOff = 0;
            if (!chain.back().deref && chain.back().offset > 0) {
                healthOff = chain.back().offset;
                obj = healthAddr - static_cast<std::uintptr_t>(healthOff);
            }
            const bool sharedHead =
                !chain.empty() && chain[0].deref &&
                sharedSlotAt(moduleBase() +
                             static_cast<std::uintptr_t>(static_cast<std::intptr_t>(chain[0].offset)));
            const bool strong =
                looksLikeVtable(obj) || chain.size() >= 3 || (chain.size() >= 2 && sharedHead);
            if (!strong) {
                if (!fallbackHit)
                    fallbackHit = healthAddr;
                if (extraUntil == 0)
                    extraUntil = GetTickCount() + kChainlessExtraMs;
                LOG("hp-pair weak chain at 0x%p healthOff=%d vtable=0 steps=%zu — keep scanning",
                    reinterpret_cast<void*>(healthAddr), healthOff, chain.size());
                return false;
            }
            Profile stable = cand;
            stable.chain = chain;
            for (int pass = 0; pass < 2; ++pass) {
                Sleep(125);
                const std::uintptr_t resolved = resolveChain(stable);
                if (!resolved || !matchHints(resolved, stable, hints)) {
                    if (!fallbackHit)
                        fallbackHit = healthAddr;
                    LOG("hp-pair chain changed during stability check at 0x%p steps=%zu — keep scanning",
                        reinterpret_cast<void*>(healthAddr), chain.size());
                    return false;
                }
            }
            hit = healthAddr;
            layout = cand;
            hitScale = cand.capacityScale;
            hitSnap = snap;
            hitChain = std::move(chain);
            return true;
        }
        if (!fallbackHit) {
            fallbackHit = healthAddr;
            extraUntil = GetTickCount() + kChainlessExtraMs;
            LOG("hp-pair chainless at 0x%p type=%d — keep scanning (+%u ms)",
                reinterpret_cast<void*>(healthAddr), static_cast<int>(type), kChainlessExtraMs);
        } else {
            LOG("hp-pair chainless at 0x%p type=%d — keep scanning",
                reinterpret_cast<void*>(healthAddr), static_cast<int>(type));
        }
        return false;
    };

    auto scanRegion = [&](std::uintptr_t base, std::size_t n) {
        auto* begin = reinterpret_cast<std::uint8_t*>(base);
        const DWORD regionUntil = GetTickCount() + 2000;
        auto regionGone = [&] {
            return GetTickCount() >= regionUntil || timeGone() || scanned > kMaxScanBytes;
        };

        for (std::size_t off = 0; off + 16 <= n; off += 8) {
            if (regionGone())
                return;
            scanned += 8;
            double a = 0;
            double b = 0;
            if (!memCopy(begin + off, &a, 8) || !memCopy(begin + off + 8, &b, 8))
                continue;
            if (nearEq(a, hints.health) && nearEq(b, hints.maxHealth) &&
                consider(base + off, StatType::Double, 8))
                return;
            if (off + 24 <= n) {
                double c = 0;
                if (memCopy(begin + off + 16, &c, 8) && nearEq(a, hints.health) &&
                    nearEq(c, hints.maxHealth) && consider(base + off, StatType::Double, 16))
                    return;
            }
        }
        if (hit)
            return;

        for (std::size_t off = 0; off + 8 <= n; off += 4) {
            if (regionGone())
                return;
            scanned += 4;
            std::int32_t a = 0;
            std::int32_t b = 0;
            if (!memCopy(begin + off, &a, 4) || !memCopy(begin + off + 4, &b, 4))
                continue;
            if (a == hints.health && b == hints.maxHealth && a >= 0 && b > 0 &&
                consider(base + off, StatType::Int32, 4))
                return;
        }
        if (hit)
            return;

        if (!sawInt32Pair) {
            for (std::size_t off = 0; off + 8 <= n; off += 4) {
                if (regionGone())
                    return;
                scanned += 4;
                float a = 0;
                float b = 0;
                if (!memCopy(begin + off, &a, 4) || !memCopy(begin + off + 4, &b, 4))
                    continue;
                if (nearEq(a, hints.health) && nearEq(b, hints.maxHealth) &&
                    consider(base + off, StatType::Float, 4))
                    return;
            }
        }
    };

    std::vector<std::pair<std::uintptr_t, std::size_t>> priv;
    MEMORY_BASIC_INFORMATION mbi{};
    std::uintptr_t addr = 0;
    while (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi))) {
        const std::uintptr_t next =
            reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionPrivateWritable(mbi) && mbi.RegionSize >= 32)
            priv.push_back({reinterpret_cast<std::uintptr_t>(mbi.BaseAddress), mbi.RegionSize});
        if (next <= addr)
            break;
        addr = next;
    }
    LOG("heap scan private regions=%zu", priv.size());
    auto scanVaBand = [&](bool below2g) {
        for (const auto& r : priv) {
            if (hit || budgetGone())
                break;
            const bool low = r.first < 0x80000000u;
            if (low != below2g)
                continue;
            scanRegion(r.first, r.second);
        }
    };
    scanVaBand(true);
    if (!hit)
        scanVaBand(false);

    // The legacy mixed-type pass charges the same memory to `scanned` once per
    // interpretation and a large region can spend its whole per-region budget
    // on doubles before int32 is reached.  Preserve that path, then add one
    // physical-byte-counted int32 pass for clients whose live stats moved past
    // the effective first third of the 512 MB window.
    if (!hit) {
        constexpr DWORD kIntFallbackMs = 12000;
        constexpr std::size_t kChunk = 64 * 1024;
        const DWORD intStart = GetTickCount();
        std::size_t intWalked = 0;
        std::vector<std::uint8_t> chunk(kChunk + 4);
        auto scanIntBand = [&](bool below2g) {
            std::size_t bandWalked = 0;
            for (const auto& r : priv) {
                if (hit || bandWalked >= kMaxScanBytes ||
                    GetTickCount() - intStart >= kIntFallbackMs)
                    break;
                const bool low = r.first < 0x80000000u;
                if (low != below2g)
                    continue;
                for (std::size_t pos = 0; pos < r.second;) {
                    if (hit || bandWalked >= kMaxScanBytes ||
                        GetTickCount() - intStart >= kIntFallbackMs)
                        break;
                    const std::size_t left = r.second - pos;
                    const std::size_t n = left < kChunk ? left : kChunk;
                    if (n < 8)
                        break;
                    if (!memCopy(reinterpret_cast<const void*>(r.first + pos), chunk.data(), n)) {
                        pos += n;
                        intWalked += n;
                        bandWalked += n;
                        continue;
                    }
                    for (std::size_t off = 0; off + 8 <= n; off += 4) {
                        std::int32_t a = 0;
                        std::int32_t b = 0;
                        std::memcpy(&a, chunk.data() + off, 4);
                        std::memcpy(&b, chunk.data() + off + 4, 4);
                        if (a == hints.health && b == hints.maxHealth && a >= 0 && b > 0 &&
                            consider(r.first + pos + off, StatType::Int32, 4))
                            break;
                    }
                    pos += n;
                    intWalked += n;
                    bandWalked += n;
                }
            }
        };
        scanIntBand(true);
        if (!hit)
            scanIntBand(false);
        LOG("heap int32 fallback walked=%zu hpPairs=%zu hit=%d ms=%u", intWalked, hpPairs,
            hit ? 1 : 0, GetTickCount() - intStart);
    }

    if (!hit && fallbackHit) {
        LOG("scan no chained LocalPlayer — not locking chainless hp-pair 0x%p scanned=%zu timeout=%d",
            reinterpret_cast<void*>(fallbackHit), scanned, timedOut ? 1 : 0);
    }

    if (hit && !hitChain.empty()) {
        layout.chain = hitChain;
        if (hints.hasPosition) {
            for (int po = -2048; po <= 2048; po += 4) {
                std::int32_t xyz[3]{};
                const std::uintptr_t pa =
                    hit + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(po));
                if (!memRead(reinterpret_cast<const void*>(pa), xyz, sizeof(xyz)))
                    continue;
                if (xyz[0] == hints.x && xyz[1] == hints.y && xyz[2] == hints.z && xyz[2] >= 0 &&
                    xyz[2] <= 15) {
                    layout.hasPosition = true;
                    layout.positionOffset = po;
                    break;
                }
            }
        }
        hitSnap = readSnapshot(hit, layout);
    }

    std::lock_guard<std::mutex> lock(g_mtx);
    if (!hit || !hitSnap.valid) {
        g_phase = "needCalibration";
        std::ostringstream oss;
        oss << "scan found no chained LocalPlayer (" << (scanned / (1024 * 1024)) << " MB, "
            << hpPairs << " hp-pairs"
            << (fallbackHit ? ", HP copy ignored" : "") << (timedOut ? ", timeout" : "") << ")";
        g_phaseMsg = oss.str();
        LOG("heap scan miss scanned=%zu hpPairs=%zu timeout=%d", scanned, hpPairs,
            timedOut ? 1 : 0);
        if (hpPairs == 0)
            LOG("heap scan: 0 hp-pairs — log in on map and match calibrator hints (hp/level/cap)");
        return false;
    }

    layout.buildId = buildId;
    preserveCalibratedSurfaces(layout, priorProfile);
    g_profile = layout;
    g_profile.capacityScale = hitScale;
    g_profile.chain = std::move(hitChain);
    g_statsBase = hit;
    g_snap = hitSnap;
    g_phase = "ready";
    g_phaseMsg = g_profile.chain.empty()
                     ? "stats locked (no chain yet — save still works, restore may scan)"
                     : "pointer-chain resolved";
    LOG("scan hit statsBase=0x%p type=%d scale=%d chainSteps=%zu hpPairs=%zu scanned=%zu timeout=%d",
        reinterpret_cast<void*>(hit), static_cast<int>(layout.statType), hitScale,
        g_profile.chain.size(), hpPairs, scanned, timedOut ? 1 : 0);
    if (!g_profile.chain.empty())
        LOG("pointer-chain resolved at 0x%p (%zu steps)", reinterpret_cast<void*>(hit),
            g_profile.chain.size());
    return true;
}

bool heapRestoreFromProfile(const Profile& layout) {
    if (layout.statType != StatType::Double)
        return false;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        if (g_statsBase && g_snap.valid && g_snap.maxHealth >= 50 && g_snap.maxMana >= 1)
            return true;
    }

    const DWORD t0 = GetTickCount();
    std::uintptr_t hit = 0;
    PlayerSnapshot hitSnap;
    MEMORY_BASIC_INFORMATION mbi{};
    std::uintptr_t addr = 0;
    while (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi))) {
        if (GetTickCount() - t0 > kScanBudgetMs)
            break;
        const std::uintptr_t next =
            reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionWritable(mbi) && mbi.RegionSize >= 32) {
            auto* begin = static_cast<std::uint8_t*>(mbi.BaseAddress);
            const std::size_t n = mbi.RegionSize;
            const auto base = reinterpret_cast<std::uintptr_t>(begin);
            for (std::size_t off = 0; off + 16 <= n; off += 8) {
                double a = 0;
                double b = 0;
                if (!memCopy(begin + off, &a, 8) || !memCopy(begin + off + 8, &b, 8))
                    continue;
                if (a < 1 || b < 1 || a > b + 5 || b > 500000)
                    continue;
                const PlayerSnapshot snap = readSnapshot(base + off, layout);
                if (!snap.valid || snap.capacity <= 0 || snap.maxMana < 1)
                    continue;
                hit = base + off;
                hitSnap = snap;
                break;
            }
        }
        if (hit)
            break;
        if (next <= addr)
            break;
        addr = next;
    }
    if (!hit)
        return false;

    auto chain = findChain(hit, nullptr);
    if (chain.empty()) {
        LOG("heap restore rejected unchained statsBase=0x%p hp=%d/%d level=%d exp=%llu",
            reinterpret_cast<void*>(hit), hitSnap.health, hitSnap.maxHealth, hitSnap.level,
            static_cast<unsigned long long>(hitSnap.experience));
        return false;
    }

    std::lock_guard<std::mutex> lock(g_mtx);
    g_statsBase = hit;
    g_snap = hitSnap;
    g_profile.chain = std::move(chain);
    g_phase = "ready";
    g_phaseMsg = "LocalPlayer live";
    LOG("heap restore statsBase=0x%p hp=%d/%d chain=%zu", reinterpret_cast<void*>(hit),
        hitSnap.health, hitSnap.maxHealth, g_profile.chain.size());
    return true;
}

constexpr std::uint32_t kKnownStateBits = 1u | 2u | 4u | 8u | 16u | 32u | 64u | 128u | 256u |
                                          512u | 1024u | 2048u | 4096u | 8192u | 16384u | 32768u |
                                          65536u;

bool validPositionBounds(const CalibHints& hints) {
    return hints.hasZ && hints.z >= 0 && hints.z <= 15 && hints.minX >= 0 &&
           hints.minX <= hints.maxX && hints.minY >= 0 && hints.minY <= hints.maxY &&
           hints.maxX <= 65535 && hints.maxY <= 65535;
}

bool plausibleMapPos(int x, int y, int z, const CalibHints& hints) {
    if (z < 0 || z > 15)
        return false;
    if (hints.hasZ && z != hints.z)
        return false;
    if (hints.hasPosition) {
        if (x != hints.x || y != hints.y || z != hints.z)
            return false;
        return x >= 0 && y >= 0 && x <= 65535 && y <= 65535;
    }
    if (hints.hasPositionBounds) {
        if (!validPositionBounds(hints))
            return false;
        return x >= hints.minX && x <= hints.maxX && y >= hints.minY && y <= hints.maxY;
    }
    return x >= 0 && y >= 0 && x <= 65535 && y <= 65535;
}

std::uintptr_t objectBaseOf(std::uintptr_t statsBase, const Profile& p) {
    if (!p.chain.empty() && !p.chain.back().deref) {
        const int off = p.chain.back().offset;
        if (off > 0 && off < 8192)
            return statsBase - static_cast<std::uintptr_t>(off);
    }
    return statsBase > 2048 ? statsBase - 2048 : statsBase;
}

std::uintptr_t chainObjectBeforeLastDeref(const Profile& p) {
    int lastDeref = -1;
    for (int i = 0; i < static_cast<int>(p.chain.size()); ++i) {
        if (p.chain[static_cast<std::size_t>(i)].deref)
            lastDeref = i;
    }
    if (lastDeref <= 0)
        return 0;
    std::uintptr_t cur = moduleBase();
    if (!cur)
        return 0;
    for (int i = 0; i < lastDeref; ++i) {
        const auto& step = p.chain[static_cast<std::size_t>(i)];
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

bool readLayoutPosition(std::uintptr_t statsBase, const Profile& layout, int& x, int& y, int& z) {
    std::uintptr_t addr =
        statsBase + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(layout.positionOffset));
    if (layout.positionIndirect) {
        std::uintptr_t target = 0;
        if (!readPtr(addr, target) || !looksLikeHeapObject(target))
            return false;
        addr = target + static_cast<std::uintptr_t>(
                            static_cast<std::intptr_t>(layout.positionInnerOffset));
    }
    if (layout.positionPacked)
        return readPackedPos(addr, x, y, z);
    std::int32_t xyz[3]{};
    if (!memRead(reinterpret_cast<const void*>(addr), xyz, sizeof(xyz)))
        return false;
    x = xyz[0];
    y = xyz[1];
    z = xyz[2];
    return true;
}

void discoverPosition(std::uintptr_t statsBase, Profile& layout, const CalibHints& hints) {
    if (layout.hasPosition && !hints.forcePositionRediscovery) {
        int x = 0;
        int y = 0;
        int z = 0;
        if (readLayoutPosition(statsBase, layout, x, y, z) && plausibleMapPos(x, y, z, hints)) {
            LOG("position already live %d,%d,%d", x, y, z);
            return;
        }
    }
    const bool had = layout.hasPosition;
    const std::int32_t oldOff = layout.positionOffset;
    const bool oldPacked = layout.positionPacked;
    const bool oldIndirect = layout.positionIndirect;
    const std::int32_t oldInnerOff = layout.positionInnerOffset;
    layout.hasPosition = false;
    const std::uintptr_t obj = objectBaseOf(statsBase, layout);
    if (obj > statsBase) {
        if (had && !hints.forcePositionRediscovery) {
            layout.hasPosition = true;
            layout.positionOffset = oldOff;
            layout.positionPacked = oldPacked;
            layout.positionIndirect = oldIndirect;
            layout.positionInnerOffset = oldInnerOff;
        }
        return;
    }
    const std::size_t primarySpan = (statsBase - obj) + 64;
    const std::size_t skip = sizeof(void*);

    struct Cand {
        std::size_t i = 0;
        int x = 0;
        int y = 0;
        int z = 0;
        bool packed = true;
        bool indirect = false;
        std::int32_t pointerRel = 0;
        std::int32_t innerOff = 0;
    };
    std::vector<Cand> cands;
    auto scanRange = [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i + 5 < end; i += 2) {
            int x = 0;
            int y = 0;
            int z = 0;
            if (!readPackedPos(obj + i, x, y, z) || !plausibleMapPos(x, y, z, hints))
                continue;
            const std::int32_t rel = static_cast<std::int32_t>(
                static_cast<std::intptr_t>(obj + i) - static_cast<std::intptr_t>(statsBase));
            if (!hints.forcePositionRediscovery || !had || rel != oldOff)
                cands.push_back({i, x, y, z, true});
        }
        std::size_t intBegin = (begin + 3u) & ~std::size_t{3u};
        for (std::size_t i = intBegin; i + 12 < end; i += 4) {
            std::int32_t xyz[3]{};
            if (!memRead(reinterpret_cast<const void*>(obj + i), xyz, sizeof(xyz)))
                continue;
            if (!plausibleMapPos(xyz[0], xyz[1], xyz[2], hints))
                continue;
            const std::int32_t rel = static_cast<std::int32_t>(
                static_cast<std::intptr_t>(obj + i) - static_cast<std::intptr_t>(statsBase));
            if (!hints.forcePositionRediscovery || !had || rel != oldOff)
                cands.push_back({i, xyz[0], xyz[1], xyz[2], false});
        }
    };

    // Preserve the established LocalPlayer-before-stats search first. Some
    // clients keep auxiliary player fields after the stats block, so only on
    // a miss add a bounded continuation over the same owning object.
    scanRange(skip, primarySpan);
    if (cands.empty() || hints.requirePositionMovement) {
        constexpr std::size_t kAfterStatsFallback = 4096;
        scanRange(primarySpan, primarySpan + kAfterStatsFallback);
        LOG("position primary miss span=%zu; after-stats fallback candidates=%zu", primarySpan,
            cands.size());
    }
    if (cands.empty() || hints.requirePositionMovement) {
        constexpr std::size_t kPointerAfterStatsFallback = 4096;
        const std::uintptr_t ptrEnd = obj + primarySpan + kPointerAfterStatsFallback;
        std::unordered_set<std::uintptr_t> scannedTargets;
        for (std::uintptr_t slot = obj + sizeof(void*); slot + sizeof(void*) <= ptrEnd;
             slot += sizeof(void*)) {
            std::uintptr_t target = 0;
            if (!readPtr(slot, target) || !looksLikeHeapObject(target))
                continue;
            if (!scannedTargets.insert(target).second)
                continue;
            const std::size_t n = readablePrefix(target, 1036);
            if (n < 12)
                continue;
            std::vector<std::uint8_t> bytes(n);
            if (!memCopy(reinterpret_cast<const void*>(target), bytes.data(), n))
                continue;
            for (std::size_t inner = 0; inner + 5 < n && inner <= 1024; inner += 2) {
                std::uint16_t px = 0, py = 0;
                std::memcpy(&px, bytes.data() + inner, sizeof(px));
                std::memcpy(&py, bytes.data() + inner + 2, sizeof(py));
                const int x = px, y = py, z = bytes[inner + 4];
                if (!plausibleMapPos(x, y, z, hints))
                    continue;
                Cand c;
                c.x = x; c.y = y; c.z = z; c.packed = true; c.indirect = true;
                c.pointerRel = static_cast<std::int32_t>(
                    static_cast<std::intptr_t>(slot) - static_cast<std::intptr_t>(statsBase));
                c.innerOff = static_cast<std::int32_t>(inner);
                if (!hints.forcePositionRediscovery || !had || !oldIndirect ||
                    c.pointerRel != oldOff || c.innerOff != oldInnerOff)
                    cands.push_back(c);
            }
            for (std::size_t inner = 0; inner + 12 <= n && inner <= 1024; inner += 4) {
                std::int32_t xyz[3]{};
                std::memcpy(xyz, bytes.data() + inner, sizeof(xyz));
                if (!plausibleMapPos(xyz[0], xyz[1], xyz[2], hints))
                    continue;
                Cand c;
                c.x = xyz[0]; c.y = xyz[1]; c.z = xyz[2]; c.packed = false; c.indirect = true;
                c.pointerRel = static_cast<std::int32_t>(
                    static_cast<std::intptr_t>(slot) - static_cast<std::intptr_t>(statsBase));
                c.innerOff = static_cast<std::int32_t>(inner);
                if (!hints.forcePositionRediscovery || !had || !oldIndirect ||
                    c.pointerRel != oldOff || c.innerOff != oldInnerOff)
                    cands.push_back(c);
            }
        }
        LOG("position inline miss; pointer-follow candidates=%zu", cands.size());
    }
    if (cands.empty()) {
        if (had && !hints.forcePositionRediscovery) {
            layout.hasPosition = true;
            layout.positionOffset = oldOff;
            layout.positionPacked = oldPacked;
            layout.positionIndirect = oldIndirect;
            layout.positionInnerOffset = oldInnerOff;
            LOG("position rediscovery missed, keeping previous offset");
        }
        return;
    }

    std::vector<Cand> stable;
    auto reread = [&](const Cand& c, int& x, int& y, int& z) {
        x = 0;
        y = 0;
        z = 0;
        bool ok = false;
        std::uintptr_t addr = obj + c.i;
        if (c.indirect) {
            std::uintptr_t target = 0;
            const std::uintptr_t slot = statsBase + static_cast<std::uintptr_t>(
                static_cast<std::intptr_t>(c.pointerRel));
            ok = readPtr(slot, target) && looksLikeHeapObject(target);
            addr = target + static_cast<std::uintptr_t>(c.innerOff);
        } else
            ok = true;
        if (ok && c.packed)
            ok = readPackedPos(addr, x, y, z);
        else {
            std::int32_t xyz[3]{};
            ok = memRead(reinterpret_cast<const void*>(addr), xyz, sizeof(xyz));
            x = xyz[0];
            y = xyz[1];
            z = xyz[2];
        }
        return ok;
    };

    if (hints.requirePositionMovement) {
        // Give the operator time to return focus to the game after approving
        // the command, then compare against a well-separated second snapshot.
        Sleep(12000);
        for (const auto& c : cands) {
            int x = 0, y = 0, z = 0;
            if (!reread(c, x, y, z))
                continue;
            if (!plausibleMapPos(x, y, z, hints))
                continue;
            const int dx = std::abs(x - c.x);
            const int dy = std::abs(y - c.y);
            if (z == c.z && (dx != 0 || dy != 0) && dx <= 100 && dy <= 100) {
                Cand moved = c;
                moved.x = x;
                moved.y = y;
                moved.z = z;
                stable.push_back(moved);
            }
        }
    } else {
        Sleep(2000);
        for (const auto& c : cands) {
            int x = 0, y = 0, z = 0;
            if (reread(c, x, y, z) && x == c.x && y == c.y && z == c.z)
                stable.push_back(c);
        }
    }
    if (stable.empty()) {
        if (had && !hints.forcePositionRediscovery) {
            layout.hasPosition = true;
            layout.positionOffset = oldOff;
            layout.positionPacked = oldPacked;
            layout.positionIndirect = oldIndirect;
            layout.positionInnerOffset = oldInnerOff;
            LOG("position rediscovery missed, keeping previous offset");
        }
        return;
    }

    struct Key {
        int x, y, z;
        bool operator<(const Key& o) const {
            if (x != o.x)
                return x < o.x;
            if (y != o.y)
                return y < o.y;
            return z < o.z;
        }
    };
    std::map<Key, int> freq;
    for (const auto& c : stable)
        freq[{c.x, c.y, c.z}]++;

    Key bestKey{};
    int bestN = 0;
    for (const auto& kv : freq) {
        if (kv.second > bestN) {
            bestN = kv.second;
            bestKey = kv.first;
        }
    }

    const Cand* chosen = nullptr;
    for (const auto& c : stable) {
        if (c.x != bestKey.x || c.y != bestKey.y || c.z != bestKey.z)
            continue;
        if (!chosen || (c.indirect ? c.pointerRel : static_cast<int>(c.i)) <
                           (chosen->indirect ? chosen->pointerRel : static_cast<int>(chosen->i)))
            chosen = &c;
    }
    if (!chosen) {
        if (had && !hints.forcePositionRediscovery) {
            layout.hasPosition = true;
            layout.positionOffset = oldOff;
            layout.positionPacked = oldPacked;
        }
        return;
    }

    layout.hasPosition = true;
    layout.positionOffset = chosen->indirect
                                ? chosen->pointerRel
                                : static_cast<int>(static_cast<std::intptr_t>(obj + chosen->i) -
                                                   static_cast<std::intptr_t>(statsBase));
    layout.positionPacked = chosen->packed;
    layout.positionIndirect = chosen->indirect;
    layout.positionInnerOffset = chosen->innerOff;
    LOG("position offset=%d packed=%d indirect=%d inner=%d copies=%d xyz=%d,%d,%d",
        layout.positionOffset, chosen->packed ? 1 : 0, chosen->indirect ? 1 : 0,
        chosen->innerOff, bestN, chosen->x, chosen->y, chosen->z);
}

void discoverStates(std::uintptr_t statsBase, Profile& layout, const CalibHints& hints) {
    const std::uint32_t need = hints.expectStateBits;
    auto readFlags = [&](int off, std::uint32_t& v) -> bool {
        const std::uintptr_t addr =
            statsBase + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(off));
        return memReadT(addr, v);
    };
    auto pop = [](std::uint32_t v) {
        int n = 0;
        while (v) {
            n += static_cast<int>(v & 1u);
            v >>= 1;
        }
        return n;
    };

    if (layout.hasStates) {
        std::uint32_t cur = 0;
        if (readFlags(layout.statesOffset, cur) && (cur & ~kKnownStateBits) == 0 &&
            (need == 0 || (cur & need) == need)) {
            std::uint32_t at16 = 0;
            if (need == 0 && layout.statesOffset != -16 && readFlags(-16, at16) &&
                (at16 & ~kKnownStateBits) == 0 && pop(at16) < pop(cur)) {
                layout.statesOffset = -16;
                LOG("states prefer -16 flags=%u over %u", at16, cur);
                return;
            }
            if (pop(cur) <= 3) {
                LOG("states already live flags=%u", cur);
                return;
            }
        }
    }

    int bestOff = 0;
    int bestScore = -0x7fffffff;
    bool found = false;
    for (int off = -256; off <= 16; off += 4) {
        std::uint32_t v = 0;
        if (!readFlags(off, v))
            continue;
        if ((v & ~kKnownStateBits) != 0)
            continue;
        if (need != 0 && (v & need) != need)
            continue;
        const int bits = pop(v);
        if (need == 0 && bits > 4)
            continue;
        int score = 0;
        if (off == -16)
            score += 400;
        if (off < 0 && off >= -64)
            score += 200;
        if (need == 0)
            score += (4 - bits) * 80;
        score -= std::abs(off);
        if (score > bestScore) {
            bestScore = score;
            bestOff = off;
            found = true;
        }
    }
    if (!found)
        return;
    layout.hasStates = true;
    layout.statesOffset = bestOff;
    LOG("states offset=%d", bestOff);
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

bool plausibleLivePos(int x, int y, int z) {
    return z >= 0 && z <= 15 && x >= 0 && y >= 0 && x <= 65535 && y <= 65535;
}

bool plausibleCalibratedPos(int x, int y, int z) {
    return z >= 0 && z <= 15 && x >= 0 && y >= 0 && x <= 65535 && y <= 65535;
}

bool readXyz(std::uintptr_t addr, bool packed, int& x, int& y, int& z) {
    if (packed)
        return readPackedPos(addr, x, y, z);
    std::int32_t xyz[3]{};
    if (!memRead(reinterpret_cast<const void*>(addr), xyz, sizeof(xyz)))
        return false;
    x = xyz[0];
    y = xyz[1];
    z = xyz[2];
    return true;
}

std::uintptr_t playerObjectFromStats(std::uintptr_t statsBase, const Profile& p) {
    if (!statsBase)
        return 0;
    const std::uintptr_t fromChain = objectBaseOf(statsBase, p);
    // A final non-dereferenced chain step is the offset from the owning object
    // to the stats block. That relationship is stronger than a vtable guess:
    // LocalPlayer may have no vptr at offset zero or may contain polymorphic
    // subobjects that otherwise look like a nearer (but wrong) object base.
    if (!p.chain.empty() && !p.chain.back().deref && fromChain &&
        looksLikeHeapObject(fromChain))
        return fromChain;
    if (fromChain && looksLikeVtable(fromChain) && looksLikeHeapObject(fromChain)) {
        std::uintptr_t ptr = fromChain;
        std::uintptr_t r = 0;
        if (completeSharedPtr(ptr, r))
            return ptr;
        return fromChain;
    }
    const int ps = static_cast<int>(sizeof(void*));
    std::uintptr_t found = 0;
    for (int back = 0; back <= 4096; back += ps) {
        if (statsBase < static_cast<std::uintptr_t>(back))
            break;
        const std::uintptr_t a = statsBase - static_cast<std::uintptr_t>(back);
        if (looksLikeVtable(a) && looksLikeHeapObject(a))
            found = a;
    }
    if (found) {
        std::uintptr_t ptr = found;
        std::uintptr_t r = 0;
        if (completeSharedPtr(ptr, r))
            return ptr;
        return found;
    }
    return fromChain;
}

void rememberPos(std::uintptr_t statsBase, std::uintptr_t addr, bool packed) {
    g_livePosOff = static_cast<std::int32_t>(static_cast<std::intptr_t>(addr - statsBase));
    g_livePosPacked = packed;
    g_livePosKnown = true;
}

bool takePos(PlayerSnapshot& s, std::uintptr_t statsBase, std::uintptr_t addr, bool packed,
             bool calibrated = false) {
    int x = 0;
    int y = 0;
    int z = 0;
    if (!readXyz(addr, packed, x, y, z) ||
        !(calibrated ? plausibleCalibratedPos(x, y, z) : plausibleLivePos(x, y, z)))
        return false;
    s.position.x = x;
    s.position.y = y;
    s.position.z = z;
    s.hasPosition = true;
    rememberPos(statsBase, addr, packed);
    return true;
}

void applyLivePosition(PlayerSnapshot& s, std::uintptr_t statsBase, const Profile& p) {
    s.hasPosition = false;
    if (p.chain.empty())
        return;

    // A calibrated profile is authoritative. A heuristic cached during the
    // short login window may still look like a plausible map position while
    // pointing at unrelated data, so it must only be a fallback.
    if (p.hasPosition) {
        if (p.positionIndirect) {
            int x = 0, y = 0, z = 0;
            if (readLayoutPosition(statsBase, p, x, y, z) && plausibleCalibratedPos(x, y, z)) {
                s.position.x = x;
                s.position.y = y;
                s.position.z = z;
                s.hasPosition = true;
                return;
            }
        }
        const std::uintptr_t addr =
            statsBase + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(p.positionOffset));
        if (!p.positionIndirect &&
            (takePos(s, statsBase, addr, p.positionPacked, true) ||
             takePos(s, statsBase, addr, !p.positionPacked, true)))
            return;
    }

    if (g_livePosKnown) {
        const std::uintptr_t addr =
            statsBase + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(g_livePosOff));
        if (takePos(s, statsBase, addr, g_livePosPacked))
            return;
        g_livePosKnown = false;
    }

    // A LocalPlayer object can become visible a few polls before its position
    // fields are initialized. Delay heuristic discovery so an unrelated stable
    // packed tuple is not cached during that login window. Profiles with an
    // explicit position offset are unaffected.
    if (g_posDiscoverBase != statsBase) {
        g_posDiscoverBase = statsBase;
        g_posDiscoverSince = GetTickCount();
        return;
    }
    if (GetTickCount() - g_posDiscoverSince < 750)
        return;

    const std::uintptr_t obj = playerObjectFromStats(statsBase, p);
    if (obj) {
        if (p.creatures.present) {
            const std::uintptr_t addr =
                obj + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(p.creatures.positionOffset));
            if (takePos(s, statsBase, addr, p.creatures.positionPacked, true) ||
                takePos(s, statsBase, addr, !p.creatures.positionPacked, true))
                return;
        }
        if (takePos(s, statsBase, obj + 16, false) || takePos(s, statsBase, obj + 16, true) ||
            takePos(s, statsBase, obj + 32, false) || takePos(s, statsBase, obj + 32, true))
            return;
    }

    const std::uintptr_t from =
        (obj && obj < statsBase) ? obj : (statsBase > 2048 ? statsBase - 2048 : statsBase);
    const std::uintptr_t to = statsBase + 64;
    if (from >= to)
        return;
    for (std::uintptr_t a = from + sizeof(void*); a + 12 <= to; a += 4) {
        if (takePos(s, statsBase, a, false))
            return;
    }
    for (std::uintptr_t a = from + sizeof(void*); a + 5 <= to; a += 2) {
        if (takePos(s, statsBase, a, true))
            return;
    }
}

bool looksLikeItemId(std::uint16_t id) {
    return id >= 100 && id < 60000;
}

bool looksLikeItemObject(std::uintptr_t item) {
    return item != 0 && looksLikeHeapObject(item) && looksLikeVtable(item);
}

bool probeInventorySlots(std::uintptr_t start, int stride, std::uintptr_t items[10], int& nThing) {
    nThing = 0;
    for (int i = 0; i < 10; ++i) {
        items[i] = 0;
        std::uintptr_t p = 0;
        if (!readPtr(start + static_cast<std::uintptr_t>(i * stride), p))
            return false;
        items[i] = p;
        if (!p)
            continue;
        if (!looksLikeItemObject(p))
            return false;
        for (int j = 0; j < i; ++j) {
            if (items[j] && items[j] == p)
                return false;
        }
        ++nThing;
    }
    return nThing >= 1;
}

bool probeInventorySlotsSkipUnknown(std::uintptr_t start, int stride, std::uintptr_t items[10],
                                    int& nThing) {
    nThing = 0;
    for (int i = 0; i < 10; ++i) {
        items[i] = 0;
        std::uintptr_t p = 0;
        if (!readPtr(start + static_cast<std::uintptr_t>(i * stride), p))
            return false;
        if (!p)
            continue;
        if (!looksLikeItemObject(p))
            continue;
        for (int j = 0; j < i; ++j) {
            if (items[j] && items[j] == p)
                return false;
        }
        items[i] = p;
        ++nThing;
    }
    return nThing >= 1;
}

bool probeInventorySlotsHeap(std::uintptr_t start, int stride, std::uintptr_t items[10], int& nThing) {
    nThing = 0;
    for (int i = 0; i < 10; ++i) {
        items[i] = 0;
        std::uintptr_t p = 0;
        if (!readPtr(start + static_cast<std::uintptr_t>(i * stride), p))
            return false;
        if (!p)
            continue;
        if (!looksLikeHeapObject(p))
            return false;
        for (int j = 0; j < i; ++j) {
            if (items[j] && items[j] == p)
                return false;
        }
        items[i] = p;
        ++nThing;
    }
    return nThing >= 1;
}

bool itemHasClientId(std::uintptr_t item, int wantId) {
    if (!item || wantId <= 0)
        return false;
    for (int off = 4; off <= 96; off += 2) {
        std::uint16_t id = 0;
        if (memReadT(item + static_cast<std::uintptr_t>(off), id) && static_cast<int>(id) == wantId)
            return true;
    }
    return false;
}

bool idOffsetLooksLikePointerWord(std::uintptr_t item, int off) {
    if (off < 2 || (off % 4) != 2)
        return false;
    std::uintptr_t pv = 0;
    if (!readPtr(item + static_cast<std::uintptr_t>(off - 2), pv))
        return false;
    return looksLikeHeapObject(pv);
}

int scoreCountOffset(const std::uintptr_t items[10], int countOff) {
    int ones = 0;
    int first = -1;
    bool allSame = true;
    for (int i = 0; i < 10; ++i) {
        if (!items[i])
            continue;
        std::uint8_t c = 0;
        if (!memReadT(items[i] + static_cast<std::uintptr_t>(countOff), c) || c > 100)
            return -1;
        const int n = c == 0 ? 1 : static_cast<int>(c);
        if (n == 1)
            ++ones;
        if (first < 0)
            first = n;
        else if (n != first)
            allSame = false;
    }
    int nThing = 0;
    for (int i = 0; i < 10; ++i) {
        if (items[i])
            ++nThing;
    }
    if (nThing >= 3 && allSame && first > 1)
        return -1;
    return ones * 20 + (allSame && first == 1 ? 15 : 0);
}

bool pickItemFields(const std::uintptr_t items[10], int nThing, int& idOff, int& countOff) {
    int bestScore = -1;
    int bestId = 0;
    int bestCount = 4;
    constexpr int kMinOff = 8;
    for (int off = kMinOff; off <= 96; off += 2) {
        std::uint16_t ids[10]{};
        int ok = 0;
        int unique = 0;
        bool ptrWord = false;
        for (int i = 0; i < 10; ++i) {
            if (!items[i])
                continue;
            if (idOffsetLooksLikePointerWord(items[i], off)) {
                ptrWord = true;
                break;
            }
            std::uint16_t id = 0;
            if (!memReadT(items[i] + static_cast<std::uintptr_t>(off), id) || !looksLikeItemId(id))
                break;
            ids[i] = id;
            ++ok;
            bool seen = false;
            for (int j = 0; j < i; ++j) {
                if (ids[j] && ids[j] == id) {
                    seen = true;
                    break;
                }
            }
            if (!seen)
                ++unique;
        }
        if (ptrWord || ok != nThing)
            continue;
        if (nThing >= 2 && unique < 2)
            continue;

        int chosenId = off;
        int chosenCount = -1;
        int bestCountScore = -1;
        bool haveB = true;
        bool haveC = true;
        for (int i = 0; i < 10; ++i) {
            if (!items[i])
                continue;
            std::uint16_t tb = 0;
            std::uint16_t tc = 0;
            if (!memReadT(items[i] + static_cast<std::uintptr_t>(off + 2), tb) || !looksLikeItemId(tb))
                haveB = false;
            if (!memReadT(items[i] + static_cast<std::uintptr_t>(off + 4), tc) || !looksLikeItemId(tc))
                haveC = false;
        }
        if (haveB && haveC) {
            chosenId = off + 2;
            unique = 0;
            ok = 0;
            for (int i = 0; i < 10; ++i) {
                if (!items[i])
                    continue;
                std::uint16_t id = 0;
                memReadT(items[i] + static_cast<std::uintptr_t>(chosenId), id);
                ids[i] = id;
                ++ok;
                bool seen = false;
                for (int j = 0; j < i; ++j) {
                    if (ids[j] && ids[j] == id)
                        seen = true;
                }
                if (!seen)
                    ++unique;
            }
            if (nThing >= 2 && unique < 2)
                continue;
        }
        const int tryCount[] = {chosenId + 4, chosenId + 6, chosenId + 2, chosenId + 8, chosenId + 5};
        for (int t : tryCount) {
            const int cs = scoreCountOffset(items, t);
            if (cs > bestCountScore) {
                bestCountScore = cs;
                chosenCount = t;
            }
        }
        if (chosenCount < 0 || bestCountScore < 0)
            continue;
        int score = unique * 50 + bestCountScore + off / 4;
        if (score > bestScore) {
            bestScore = score;
            bestId = chosenId;
            bestCount = chosenCount;
        }
    }
    if (bestScore < 0)
        return false;
    idOff = bestId;
    countOff = bestCount;
    return true;
}

bool inventoryMatchesHint(std::uintptr_t start, int stride, int idOff, const CalibHints& hints) {
    if (!hints.hasItemId)
        return true;
    const int from = hints.itemSlot >= 1 && hints.itemSlot <= 10 ? hints.itemSlot - 1 : 0;
    const int to = hints.itemSlot >= 1 && hints.itemSlot <= 10 ? hints.itemSlot : 10;
    for (int i = from; i < to; ++i) {
        std::uintptr_t item = 0;
        if (!readPtr(start + static_cast<std::uintptr_t>(i * stride), item) || !item)
            continue;
        std::uint16_t id = 0;
        if (!memReadT(item + static_cast<std::uintptr_t>(idOff), id))
            continue;
        if (static_cast<int>(id) == hints.itemId)
            return true;
    }
    return false;
}

void discoverInventory(std::uintptr_t statsBase, Profile& layout, const CalibHints& hints) {
    if (layout.inventory.present) {
        const PlayerSnapshot cur = readSnapshot(statsBase, layout);
        if (cur.hasInventory) {
            LOG("inventory already live");
            return;
        }
        LOG("dropping fake inventory layout");
        layout.inventory = InventoryLayout{};
    }

    const std::uintptr_t obj = objectBaseOf(statsBase, layout);
    if (!obj || obj >= statsBase) {
        LOG("inventory skip bad object obj=%p statsBase=%p", reinterpret_cast<void*>(obj),
            reinterpret_cast<void*>(statsBase));
        return;
    }

    const int objSpan = static_cast<int>(statsBase - obj);
    const bool objVt = looksLikeVtable(obj);
    const bool truncatedObj = hints.hasItemId && objSpan < 256 && !objVt;
    LOG("inventory object span=%d vtable=%d truncated=%d", objSpan, objVt ? 1 : 0,
        truncatedObj ? 1 : 0);

    struct Hit {
        std::uintptr_t start = 0;
        int stride = 4;
        int idOff = 0;
        int countOff = 4;
        int nThing = 0;
        int uniqueIds = 0;
        int score = 0;
        const char* via = "tight";
        bool inlineIds = false;
    } best;
    bool found = false;
    int probeOk = 0;
    int fieldsFail = 0;
    int hintMiss = 0;

    const int strides[] = {static_cast<int>(sizeof(void*)), static_cast<int>(sizeof(void*) * 2)};
    auto considerStart = [&](std::uintptr_t start, int stride, const char* via, int probeMode) {
        std::uintptr_t items[10]{};
        int nThing = 0;
        bool probed = false;
        if (probeMode == 2)
            probed = probeInventorySlotsHeap(start, stride, items, nThing);
        else if (probeMode == 1)
            probed = probeInventorySlotsSkipUnknown(start, stride, items, nThing);
        else
            probed = probeInventorySlots(start, stride, items, nThing);
        if (!probed)
            return;
        ++probeOk;
        int idOff = 0;
        int countOff = 4;
        if (!pickItemFields(items, nThing, idOff, countOff)) {
            ++fieldsFail;
            return;
        }
        if (!inventoryMatchesHint(start, stride, idOff, hints)) {
            ++hintMiss;
            return;
        }

        std::uint16_t ids[10]{};
        int uniqueIds = 0;
        for (int i = 0; i < 10; ++i) {
            if (!items[i])
                continue;
            memReadT(items[i] + static_cast<std::uintptr_t>(idOff), ids[i]);
            bool seen = false;
            for (int j = 0; j < i; ++j) {
                if (ids[j] && ids[j] == ids[i])
                    seen = true;
            }
            if (!seen)
                ++uniqueIds;
        }

        if (probeMode == 2) {
            const std::uintptr_t interiorHi = statsBase + 4096;
            for (int i = 0; i < 10; ++i) {
                if (items[i] && items[i] >= obj && items[i] < interiorHi)
                    return;
            }
        }

        int score = uniqueIds * 80 + nThing * 5;
        std::uintptr_t prev = 1;
        if (start >= obj + static_cast<std::uintptr_t>(stride) &&
            readPtr(start - static_cast<std::uintptr_t>(stride), prev) && prev == 0)
            score += 40;
        const int rel = static_cast<int>(static_cast<std::intptr_t>(start) -
                                         static_cast<std::intptr_t>(statsBase));
        if (rel < 0 && rel > -320)
            score += 80;
        else if (rel < 0)
            score += 20;
        if (probeMode == 1 || probeMode == 2)
            score -= 50;
        if (!found || score > best.score) {
            found = true;
            best = {start, stride, idOff, countOff, nThing, uniqueIds, score, via, false};
        }
    };

    auto scan = [&](std::uintptr_t lo, std::uintptr_t hi, std::uintptr_t skipLo, std::uintptr_t skipHi,
                    const char* via, int probeMode) {
        if (!lo || lo >= hi)
            return;
        for (int stride : strides) {
            if (stride < 4)
                continue;
            const std::uintptr_t row = static_cast<std::uintptr_t>(10 * stride);
            if (hi < lo + row)
                continue;
            for (std::uintptr_t start = lo; start + row <= hi; start += 4) {
                if (skipHi > skipLo && start >= skipLo && start < skipHi)
                    continue;
                considerStart(start, stride, via, probeMode);
            }
        }
    };

    auto searchHeapAndInline = [&](std::uintptr_t lo, std::uintptr_t hi, std::uintptr_t anchor,
                                   const char* tag) {
        if (found || !hints.hasItemId || !lo || !hi || lo >= hi)
            return;
        int nHeap = 0;
        int nIdHits = 0;
        int nThingHere = 0;
        for (std::uintptr_t a = lo; a + 2 <= hi; a += 2) {
            std::uint16_t w = 0;
            if (memReadT(a, w) && static_cast<int>(w) == hints.itemId)
                ++nIdHits;
        }
        const std::uintptr_t ptrLo = (lo == anchor) ? lo + sizeof(void*) : lo;
        for (std::uintptr_t a = ptrLo; a + sizeof(void*) <= hi; a += 4) {
            std::uintptr_t p = 0;
            if (!readPtr(a, p) || !p)
                continue;
            if (p >= lo && p < hi)
                continue;
            if (looksLikeItemObject(p)) {
                ++nThingHere;
                if (itemHasClientId(p, hints.itemId)) {
                    for (int stride : strides) {
                        if (stride < 4)
                            continue;
                        considerStart(p, stride, "heap-row", false);
                        considerStart(p + static_cast<std::uintptr_t>(stride), stride, "heap-row+1",
                                      false);
                        const int want = hints.itemSlot >= 1 && hints.itemSlot <= 10 ? hints.itemSlot - 1
                                                                                    : 2;
                        const std::uintptr_t back0 =
                            static_cast<std::uintptr_t>(want) * static_cast<std::uintptr_t>(stride);
                        const std::uintptr_t back1 =
                            static_cast<std::uintptr_t>(want + 1) * static_cast<std::uintptr_t>(stride);
                        if (a >= anchor + back0)
                            considerStart(a - back0, stride, "slot-index", false);
                        if (a >= anchor + back1)
                            considerStart(a - back1, stride, "slot-index-none", false);
                    }
                }
            } else if (looksLikeHeapObject(p)) {
                ++nHeap;
                const bool heapId = itemHasClientId(p, hints.itemId);
                if (heapId)
                    LOG("inventory heapNoVt item=%p fieldRel=%d via=%s", reinterpret_cast<void*>(p),
                        static_cast<int>(static_cast<std::intptr_t>(a) -
                                         static_cast<std::intptr_t>(statsBase)),
                        tag);
                for (int stride : strides) {
                    if (stride < 4)
                        continue;
                    considerStart(p, stride, "heap-row", heapId ? 2 : 0);
                    considerStart(p + static_cast<std::uintptr_t>(stride), stride, "heap-row+1",
                                  heapId ? 2 : 0);
                    if (heapId) {
                        const int want = hints.itemSlot >= 1 && hints.itemSlot <= 10
                                             ? hints.itemSlot - 1
                                             : 2;
                        const std::uintptr_t back =
                            static_cast<std::uintptr_t>(want) * static_cast<std::uintptr_t>(stride);
                        if (a >= anchor + back)
                            considerStart(a - back, stride, "heap-novt-slot", 2);
                    }
                }
            }
        }
        LOG("inventory follow via=%s itemObjs=%d heapPtrs=%d idHits=%d probe=%d found=%d", tag,
            nThingHere, nHeap, nIdHits, probeOk, found ? 1 : 0);

        if (!found && nThingHere > 0) {
            LOG("inventory strict miss — skip-unknown slots with --id via=%s", tag);
            scan(ptrLo, hi, 0, 0, "skip-unknown", true);
        }

        if (found || !hints.hasItemId)
            return;
        const int wantId = hints.itemId;
        const int tryStride[] = {4, 8, 12, 16, 20};
        for (std::uintptr_t a = lo; a + 2 <= hi; a += 2) {
            std::uint16_t w = 0;
            if (!memReadT(a, w) || static_cast<int>(w) != wantId)
                continue;
            const std::uintptr_t aligned = a & ~static_cast<std::uintptr_t>(sizeof(void*) - 1);
            std::uintptr_t pv = 0;
            const bool insidePtr = readPtr(aligned, pv) && looksLikeHeapObject(pv);
            std::uint16_t before[4]{};
            std::uint16_t after[4]{};
            for (int i = 0; i < 4; ++i) {
                memReadT(a - 8 + static_cast<std::uintptr_t>(i * 2), before[i]);
                memReadT(a + 2 + static_cast<std::uintptr_t>(i * 2), after[i]);
            }
            LOG("inventory idHit rel=%d insidePtr=%d p=%p via=%s neigh=%u %u %u %u [%u] %u %u %u %u",
                static_cast<int>(static_cast<std::intptr_t>(a) - static_cast<std::intptr_t>(statsBase)),
                insidePtr ? 1 : 0, reinterpret_cast<void*>(pv), tag, before[0], before[1], before[2],
                before[3], w, after[0], after[1], after[2], after[3]);
            if (insidePtr)
                continue;
            if ((after[1] == 12288 && (after[2] == 4096 || after[3] == 4096)) ||
                (after[0] == 12288 && after[2] == 4096))
                continue;
            for (int stride : tryStride) {
                int idxLo = 0;
                int idxHi = 9;
                if (hints.itemSlot >= 1 && hints.itemSlot <= 10) {
                    idxLo = hints.itemSlot - 1;
                    idxHi = idxLo;
                }
                for (int idx = idxLo; idx <= idxHi; ++idx) {
                    const std::uintptr_t back =
                        static_cast<std::uintptr_t>(idx) * static_cast<std::uintptr_t>(stride);
                    if (a < anchor + back)
                        continue;
                    const std::uintptr_t start = a - back;
                    const std::uintptr_t rowEnd =
                        start + static_cast<std::uintptr_t>(9 * stride) + 2;
                    if (rowEnd > hi)
                        continue;
                    int filled = 0;
                    int wantHits = 0;
                    bool ok = true;
                    for (int i = 0; i < 10; ++i) {
                        std::uint16_t id = 0;
                        if (!memReadT(start + static_cast<std::uintptr_t>(i * stride), id)) {
                            ok = false;
                            break;
                        }
                        if (static_cast<int>(id) == wantId)
                            ++wantHits;
                        if (id == 0 || !looksLikeItemId(id))
                            continue;
                        ++filled;
                    }
                    if (!ok || filled < 1 || wantHits != 1)
                        continue;
                    std::uint16_t at = 0;
                    memReadT(start + static_cast<std::uintptr_t>(idx * stride), at);
                    if (static_cast<int>(at) != wantId)
                        continue;
                    int countOff = 2;
                    const int tryCountOff[] = {8, 4, 2, 6};
                    for (int c : tryCountOff) {
                        std::uint8_t n = 0;
                        if (!memReadT(start + static_cast<std::uintptr_t>(idx * stride + c), n))
                            continue;
                        if (n == 1) {
                            countOff = c;
                            break;
                        }
                    }
                    int score = 120 - filled * 15;
                    if (filled == 1)
                        score += 80;
                    if (stride == 4)
                        score += 10;
                    if (!found || score > best.score) {
                        found = true;
                        best = {start, stride, 0, countOff, filled, filled, score, "inline-u16", true};
                    }
                }
            }
        }
        if (found && best.inlineIds)
            LOG("inventory inline-u16 arrayOff=%d stride=%d items=%d via=%s",
                static_cast<int>(static_cast<std::intptr_t>(best.start) -
                                 static_cast<std::intptr_t>(statsBase)),
                best.stride, best.nThing, tag);
    };

    const std::uintptr_t objLo = obj + sizeof(void*);
    const std::uintptr_t tightLo =
        (statsBase - obj > 400) ? statsBase - 400 : objLo;
    scan(tightLo, statsBase, 0, 0, "tight", false);
    if (!found) {
        LOG("inventory tight miss span=%d probe=%d fieldsFail=%d hintMiss=%d — rest of object",
            static_cast<int>(statsBase - obj), probeOk, fieldsFail, hintMiss);
        scan(objLo, statsBase, tightLo, statsBase, "before-stats", false);
    }
    if (!found && hints.hasItemId) {
        LOG("inventory before-stats miss probe=%d fieldsFail=%d hintMiss=%d — after stats", probeOk,
            fieldsFail, hintMiss);
        scan(statsBase, statsBase + 1024, 0, 0, "after-stats", false);
    }
    if (!found && hints.hasItemId) {
        LOG("inventory vtable miss — heap pointer slots probe=%d fieldsFail=%d hintMiss=%d", probeOk,
            fieldsFail, hintMiss);
        scan(tightLo, statsBase, 0, 0, "heap-slots", 2);
        if (!found)
            scan(objLo, statsBase, tightLo, statsBase, "heap-slots-before", 2);
        if (!found)
            scan(statsBase, statsBase + 1024, 0, 0, "heap-slots-after", 2);
    }

    searchHeapAndInline(obj, statsBase + 1024, obj, "object");
    if (!found && truncatedObj) {
        LOG("inventory truncated span=%d vtable=0 — widen after-stats to +4096", objSpan);
        scan(statsBase + 1024, statsBase + 4096, 0, 0, "after-stats-wide", false);
        searchHeapAndInline(statsBase + 1024, statsBase + 4096, obj, "object+4k");
    }
    if (!found && truncatedObj) {
        const std::uintptr_t parent = chainObjectBeforeLastDeref(layout);
        if (parent && parent != obj) {
            LOG("inventory chain-parent=%p vs obj=%p", reinterpret_cast<void*>(parent),
                reinterpret_cast<void*>(obj));
            scan(parent + sizeof(void*), parent + 4096, 0, 0, "chain-parent", false);
            searchHeapAndInline(parent, parent + 4096, parent, "chain-parent");
        }
    }

    if (!found) {
        LOG("inventory not found span=%d probe=%d fieldsFail=%d hintMiss=%d itemId=%d — equip an item "
            "and retry refine",
            static_cast<int>(statsBase - obj), probeOk, fieldsFail, hintMiss, hints.itemId);
        return;
    }

    layout.inventory.present = true;
    layout.inventory.arrayOffset =
        static_cast<int>(static_cast<std::intptr_t>(best.start) - static_cast<std::intptr_t>(statsBase));
    layout.inventory.itemIdOffset = best.idOff;
    layout.inventory.itemCountOffset = best.countOff;
    layout.inventory.ptrSize = best.stride;
    layout.inventory.slotsArePointers = !best.inlineIds;
    LOG("inventory arrayOff=%d idOff=%d countOff=%d stride=%d items=%d uniqueIds=%d via=%s ptrs=%d",
        layout.inventory.arrayOffset, layout.inventory.itemIdOffset, layout.inventory.itemCountOffset,
        layout.inventory.ptrSize, best.nThing, best.uniqueIds, best.via,
        layout.inventory.slotsArePointers ? 1 : 0);

    if (hints.hasItemId) {
        const PlayerSnapshot check = readSnapshot(statsBase, layout);
        bool liveOk = check.hasInventory;
        if (hints.itemSlot >= 1 && hints.itemSlot <= 10)
            liveOk = liveOk && check.inventory[hints.itemSlot - 1].id == hints.itemId;
        else {
            liveOk = false;
            for (int i = 0; i < 10; ++i) {
                if (check.inventory[i].id == hints.itemId)
                    liveOk = true;
            }
        }
        if (!liveOk) {
            LOG("inventory layout failed live check slot=%d want=%d have=%d", hints.itemSlot,
                hints.itemId,
                (hints.itemSlot >= 1 && hints.itemSlot <= 10)
                    ? check.inventory[hints.itemSlot - 1].id
                    : 0);
            layout.inventory = InventoryLayout{};
        }
    }
}

bool refineLockedPlayer(const CalibHints& hints) {
    if (hints.hasPositionBounds && !validPositionBounds(hints)) {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_phaseMsg = "invalid position bounds — provide minX/maxX/minY/maxY and z";
        return false;
    }
    std::uintptr_t base = 0;
    Profile layout;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        if (!g_statsBase && !tryApplyProfile()) {
            g_phaseMsg = "LocalPlayer not live — log in on the map, then retry --refine";
            return false;
        }
        base = g_statsBase;
        layout = g_profile;
    }

    const bool needsPosition = hints.hasZ || hints.hasPosition || hints.hasPositionBounds;
    bool positionMatches = !needsPosition;
    if (needsPosition) {
        discoverPosition(base, layout, hints);
        int x = 0;
        int y = 0;
        int z = 0;
        positionMatches = layout.hasPosition && readLayoutPosition(base, layout, x, y, z) &&
                          plausibleMapPos(x, y, z, hints);
    }
    if (hints.expectBattle || hints.expectStateBits)
        discoverStates(base, layout, hints);
    if (hints.needInventory || hints.hasItemId)
        discoverInventory(base, layout, hints);

    std::lock_guard<std::mutex> lock(g_mtx);
    if (positionMatches) {
        g_profile.hasPosition = layout.hasPosition;
        g_profile.positionOffset = layout.positionOffset;
        g_profile.positionPacked = layout.positionPacked;
        g_profile.positionIndirect = layout.positionIndirect;
        g_profile.positionInnerOffset = layout.positionInnerOffset;
    }
    g_profile.hasStates = layout.hasStates;
    g_profile.statesOffset = layout.statesOffset;
    g_profile.inventory = layout.inventory;
    g_snap = readSnapshot(g_statsBase, g_profile);
    if (!positionMatches) {
        g_phaseMsg = "no player position matching the calibration hint — stay on the map and retry";
        return false;
    }
    if (hints.expectBattle && !g_profile.hasStates) {
        g_phaseMsg = "no battle bit in LocalPlayer — attack a creature (swords) and retry";
        return false;
    }
    if (hints.needInventory && !g_profile.inventory.present) {
        g_phaseMsg = "no inventory slots — equip a backpack or weapon and retry --refine";
        return false;
    }
    g_phase = "ready";
    if (g_profile.inventory.present)
        g_phaseMsg = "LocalPlayer live";
    else
        g_phaseMsg = "LocalPlayer live; equip an item and --refine for slots";
    LOG("refine position=%d states=%d inventory=%d flags=%u", g_profile.hasPosition ? 1 : 0,
        g_profile.hasStates ? 1 : 0, g_profile.inventory.present ? 1 : 0, g_snap.stateFlags);
    return g_profile.hasPosition || g_profile.hasStates || g_profile.inventory.present;
}

std::string statusObject() {
    json j;
    j["phase"] = g_phase;
    j["message"] = g_phaseMsg;
    j["buildId"] = g_build.id;
    j["architecture"] = sizeof(void*) == 8 ? "x64" : "x86";
    j["hasChain"] = !g_profile.chain.empty();
    j["statsBase"] = g_statsBase != 0;
    j["profileFound"] = g_profileFound;
    j["calibrationAvailable"] = g_profileFound;
    j["compatibility"] = g_profileFound
        ? (g_statsBase != 0 ? "compatible" : "waiting_for_player")
        : "not_calibrated";
    return j.dump();
}

json playerObject(const PlayerSnapshot& s) {
    json j;
    j["health"] = s.health;
    j["maxHealth"] = s.maxHealth;
    j["mana"] = s.mana;
    j["maxMana"] = s.maxMana;
    j["level"] = s.level;
    j["levelPercent"] = s.levelPercent;
    j["experience"] = s.experience;
    j["capacity"] = s.capacity;
    j["totalCapacity"] = s.totalCapacity;
    j["magicLevel"] = s.magicLevel;
    j["soul"] = s.soul;
    j["stateFlags"] = s.stateFlags;
    j["targetCreatureId"] = s.targetCreatureId;
    if (!s.name.empty()) j["name"] = s.name;
    if (!s.direction.empty()) j["direction"] = s.direction;
    json conditions = json::array();
    static const struct {
        std::uint32_t bit;
        const char* name;
    } kBits[] = {
        {1, "poison"},      {2, "fire"},        {4, "energy"},     {8, "drunk"},
        {16, "manaShield"}, {32, "paralyze"},   {64, "haste"},     {128, "battle"},
        {256, "drowning"},  {512, "freezing"},  {1024, "dazzled"}, {2048, "cursed"},
        {4096, "partyBuff"},{8192, "pzBlock"},  {16384, "pz"},     {32768, "bleeding"},
        {65536, "hungry"},
    };
    for (const auto& b : kBits) {
        if (s.stateFlags & b.bit)
            conditions.push_back(b.name);
    }
    j["conditions"] = conditions;
    if (s.hasPosition) {
        json pos = json::object();
        pos["x"] = s.position.x;
        pos["y"] = s.position.y;
        pos["z"] = s.position.z;
        j["position"] = pos;
    }
    return j;
}

json inventoryObject(const PlayerSnapshot& s) {
    json arr = json::array();
    if (!s.hasInventory)
        return arr;
    for (int i = 0; i < 10; ++i) {
        json it;
        it["slot"] = s.inventory[i].slot;
        it["id"] = s.inventory[i].id;
        it["count"] = s.inventory[i].count;
        arr.push_back(it);
    }
    return arr;
}

DWORD WINAPI pollThread(LPVOID) {
    while (g_pollRun) {
        std::string stats;
        std::string inv;
        std::string status;
        bool emitReady = false;
        bool emitInv = false;
        bool allowLook = false;
        bool emitTarget = false;
        bool emitRebound = false;
        std::uint32_t targetId = 0;
        {
            std::lock_guard<std::mutex> lock(g_mtx);
            if (g_statsBase) {
                // The LocalPlayer object is replaced when the user changes
                // character without restarting the client. Re-resolve the
                // calibrated build chain and adopt the new object only after
                // its complete snapshot is valid; otherwise keep the last
                // good snapshot while the client is between characters.
                const std::uintptr_t resolved = resolveChain(g_profile);
                if (resolved && resolved != g_statsBase) {
                    const PlayerSnapshot rebound = readSnapshot(resolved, g_profile);
                    if (rebound.valid) {
                        LOG("LocalPlayer rebound old=%p new=%p",
                            reinterpret_cast<void*>(g_statsBase),
                            reinterpret_cast<void*>(resolved));
                        g_statsBase = resolved;
                        g_snap = rebound;
                        g_phase = "ready";
                        g_phaseMsg = "LocalPlayer rebound after character change";
                        emitRebound = true;
                    }
                }
                const PlayerSnapshot snap = readSnapshot(g_statsBase, g_profile);
                if (snap.valid) {
                    g_snap = snap;
                    targetId = snap.targetCreatureId;
                    if (!g_targetEmitted || targetId != g_lastEmittedTarget) {
                        g_lastEmittedTarget = targetId;
                        g_targetEmitted = true;
                        emitTarget = true;
                    }
                    g_phase = "ready";
                    stats = playerObject(snap).dump();
                    if (g_posLogLeft > 0) {
                        --g_posLogLeft;
                        LOG("playerStats hasPosition=%d xyz=%d,%d,%d off=%d packed=%d json=%s",
                            snap.hasPosition ? 1 : 0, snap.position.x, snap.position.y,
                            snap.position.z, g_livePosOff, g_livePosPacked ? 1 : 0, stats.c_str());
                    }
                    if (snap.hasInventory) {
                        inv = inventoryObject(snap).dump();
                        emitInv = true;
                    }
                    if (!g_readyEmitted || emitRebound) {
                        g_readyEmitted = true;
                        emitReady = true;
                        status = statusObject();
                    }
                }
            } else if (g_phase != "scanning" && !g_profile.chain.empty() && tryApplyProfile()) {
                stats = playerObject(g_snap).dump();
                g_readyEmitted = true;
                emitReady = true;
                status = statusObject();
            }
            allowLook = !g_profile.chain.empty();
        }
        if (emitReady)
            PipeServer::instance().tryEmit("dllStatus", status);
        if (!stats.empty())
            PipeServer::instance().tryEmit("playerStats", stats);
        if (emitTarget) {
            json target;
            target["creatureId"] = targetId;
            PipeServer::instance().tryEmit("attackTarget", target.dump());
        }
        if (emitInv)
            PipeServer::instance().tryEmit("inventory", inv);
        if (!stats.empty() && allowLook)
            pumpLookCapture();
        Sleep(kPollMs);
    }
    return 0;
}

} // namespace

void startPlayerReader() {
    if (!computeHostBuildId(g_build)) {
        g_phase = "needCalibration";
        g_phaseMsg = "could not fingerprint host EXE on disk";
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_profileFound = loadProfile();
        if (g_profileFound && tryApplyProfile()) {
            LOG("profile loaded for %s", g_build.id.c_str());
        } else if (!g_profile.chain.empty()) {
            g_phase = "needCalibration";
            g_phaseMsg = "profile loaded, waiting for LocalPlayer (log in on the map)";
            LOG("waiting for LocalPlayer buildId=%s", g_build.id.c_str());
        } else {
            g_phase = "needCalibration";
            g_phaseMsg = "no profile — send calibration hints";
            LOG("needCalibration buildId=%s", g_build.id.c_str());
        }
    }

    PipeServer::instance().setConnectHandler([] { replayStatusToPipe(); });

    g_pollRun = true;
    g_pollThread = CreateThread(nullptr, 0, pollThread, nullptr, 0, nullptr);
    if (g_pollThread)
        CloseHandle(g_pollThread);
}

void replayStatusToPipe() {
    std::string status;
    std::string stats;
    std::string inv;
    bool valid = false;
    bool hasInv = false;
    std::uint32_t targetId = 0;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        status = statusObject();
        valid = g_snap.valid;
        if (valid)
            stats = playerObject(g_snap).dump();
        if (valid)
            targetId = g_snap.targetCreatureId;
        hasInv = g_snap.hasInventory;
        if (hasInv)
            inv = inventoryObject(g_snap).dump();
    }
    PipeServer::instance().emit("dllStatus", status);
    if (valid)
        PipeServer::instance().emit("playerStats", stats);
    if (valid) {
        json target;
        target["creatureId"] = targetId;
        PipeServer::instance().emit("attackTarget", target.dump());
    }
    if (hasInv)
        PipeServer::instance().emit("inventory", inv);
    replayContainersToPipe();
    replayCreaturesToPipe();
}

std::string dllStatusJson() {
    std::lock_guard<std::mutex> lock(g_mtx);
    return statusObject();
}

std::string playerStatsJson() {
    std::lock_guard<std::mutex> lock(g_mtx);
    if (!g_snap.valid)
        return "{}";
    return playerObject(g_snap).dump();
}

std::string inventoryJson() {
    std::lock_guard<std::mutex> lock(g_mtx);
    return inventoryObject(g_snap).dump();
}

std::string calibrationJson() {
    std::lock_guard<std::mutex> lock(g_mtx);
    json j = json::parse(statusObject(), nullptr, false);
    j["capacityScale"] = g_profile.capacityScale;
    j["chainSteps"] = g_profile.chain.size();
    j["hasPosition"] = g_profile.hasPosition;
    j["hasStates"] = g_profile.hasStates;
    j["hasInventory"] = g_profile.inventory.present;
    j["hasContainers"] = g_profile.containers.present;
    j["hasCreatures"] = g_profile.creatures.present;
    j["hasTalk"] = g_profile.actions.present;
    j["hasMap"] = g_profile.map.present;
    return j.dump();
}

PlayerSnapshot currentPlayer() {
    std::lock_guard<std::mutex> lock(g_mtx);
    return g_snap;
}

bool isPlayerReady() {
    std::lock_guard<std::mutex> lock(g_mtx);
    return g_phase == "ready" && g_snap.valid;
}

bool copyLiveProfile(Profile& out) {
    std::lock_guard<std::mutex> lock(g_mtx);
    out = g_profile;
    return true;
}

void setContainerLayout(const ContainerLayout& layout) {
    std::lock_guard<std::mutex> lock(g_mtx);
    g_profile.containers = layout;
}

void setCreatureLayout(const CreatureLayout& layout) {
    std::lock_guard<std::mutex> lock(g_mtx);
    g_profile.creatures = layout;
    if (!g_statsBase)
        return;

    const std::uintptr_t obj = playerObjectFromStats(g_statsBase, g_profile);
    if (!obj)
        return;
    const std::uintptr_t creaturePos =
        obj + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(layout.positionOffset));
    int cx = 0;
    int cy = 0;
    int cz = 0;
    if (!readXyz(creaturePos, layout.positionPacked, cx, cy, cz) ||
        !plausibleCalibratedPos(cx, cy, cz))
        return;

    int px = 0;
    int py = 0;
    int pz = 0;
    const bool profileAgrees =
        g_profile.hasPosition && readLayoutPosition(g_statsBase, g_profile, px, py, pz) &&
        px == cx && py == cy && pz == cz;
    if (profileAgrees)
        return;

    const std::intptr_t delta = static_cast<std::intptr_t>(creaturePos) -
                                static_cast<std::intptr_t>(g_statsBase);
    if (delta < INT32_MIN || delta > INT32_MAX)
        return;
    g_profile.hasPosition = true;
    g_profile.positionOffset = static_cast<std::int32_t>(delta);
    g_profile.positionPacked = layout.positionPacked;
    g_profile.positionIndirect = false;
    g_profile.positionInnerOffset = 0;
    rememberPos(g_statsBase, creaturePos, layout.positionPacked);
    LOG("position reconciled from P3 LocalPlayer offset=%d pos=%d,%d,%d",
        g_profile.positionOffset, cx, cy, cz);
}

void setActionLayout(const ActionLayout& layout) {
    std::lock_guard<std::mutex> lock(g_mtx);
    g_profile.actions = layout;
}

void setMapLayout(const MapLayout& layout) {
    std::lock_guard<std::mutex> lock(g_mtx);
    g_profile.map = layout;
}

std::uintptr_t currentStatsBase() {
    std::lock_guard<std::mutex> lock(g_mtx);
    return g_statsBase;
}

std::uintptr_t currentPlayerObject() {
    std::lock_guard<std::mutex> lock(g_mtx);
    return playerObjectFromStats(g_statsBase, g_profile);
}

bool resolveInventoryItem(int slot, int expectId, std::uintptr_t& ptr, std::uintptr_t& rep) {
    ptr = 0;
    rep = 0;
    if (slot < 1 || slot > 10)
        return false;

    Profile p;
    std::uintptr_t base = 0;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        p = g_profile;
        base = g_statsBase;
    }
    if (!p.inventory.present || !base)
        return false;

    const int stride =
        p.inventory.ptrSize > 0 ? p.inventory.ptrSize : static_cast<int>(sizeof(void*));
    const std::uintptr_t array =
        base + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(p.inventory.arrayOffset));
    const std::uintptr_t slotAddr = array + static_cast<std::uintptr_t>((slot - 1) * stride);

    std::uintptr_t item = slotAddr;
    if (p.inventory.slotsArePointers) {
        if (!readPtr(slotAddr, item) || !item)
            return false;
        ptr = item;
        if (stride >= static_cast<int>(sizeof(void*) * 2))
            readPtr(slotAddr + sizeof(void*), rep);
    } else {
        ptr = slotAddr;
    }

    std::uint16_t id = 0;
    if (!memReadT(item + static_cast<std::uintptr_t>(p.inventory.itemIdOffset), id) || id < 100)
        return false;
    if (expectId && static_cast<int>(id) != expectId) {
        LOG("use inventory id mismatch slot=%d have=%d want=%d", slot, static_cast<int>(id), expectId);
        ptr = 0;
        rep = 0;
        return false;
    }
    LOG("use inventory slot=%d id=%d ptr=%p rep=%p", slot, static_cast<int>(id),
        reinterpret_cast<void*>(ptr), reinterpret_cast<void*>(rep));
    return true;
}

bool resolveLocalPlayerShared(std::uintptr_t& ptr, std::uintptr_t& rep) {
    ptr = 0;
    rep = 0;
    Profile p;
    copyLiveProfile(p);
    if (p.chain.empty())
        return false;
    std::uintptr_t cur = moduleBase();
    std::uintptr_t lastSlot = 0;
    for (const auto& step : p.chain) {
        cur += static_cast<std::uintptr_t>(static_cast<std::intptr_t>(step.offset));
        if (step.deref) {
            lastSlot = cur;
            std::uintptr_t next = 0;
            if (!readPtr(cur, next) || !next)
                return false;
            cur = next;
        }
    }
    std::uintptr_t obj = 0;
    if (lastSlot)
        readPtr(lastSlot, obj);
    if (!obj)
        obj = cur;
    if (!obj)
        return false;

    auto logHeader = [&](int back) {
        if (obj < static_cast<std::uintptr_t>(back))
            return;
        const std::uintptr_t block = obj - static_cast<std::uintptr_t>(back);
        std::uintptr_t vt = 0;
        std::uint32_t uses = 0;
        std::uint32_t weaks = 0;
        readPtr(block, vt);
        memReadT(block + 4, uses);
        memReadT(block + 8, weaks);
        LOG("useWith self header-%d block=%p vt=%p uses=%u weaks=%u", back,
            reinterpret_cast<void*>(block), reinterpret_cast<void*>(vt), uses, weaks);
    };
    logHeader(16);
    logHeader(12);

    auto take = [&](std::uintptr_t pObj, std::uintptr_t r, const char* tag) {
        if (!pObj || !completeSharedPtr(pObj, r))
            return false;
        ptr = pObj;
        rep = r;
        LOG("useWith self %s ptr=%p rep=%p", tag, reinterpret_cast<void*>(ptr),
            reinterpret_cast<void*>(rep));
        return true;
    };

    if (take(obj, 0, "refcount"))
        return true;

    if (lastSlot) {
        std::uintptr_t slotPtr = 0;
        std::uintptr_t slotRep = 0;
        readPtr(lastSlot, slotPtr);
        readPtr(lastSlot + sizeof(void*), slotRep);
        if (take(slotPtr, slotRep, "chain-slot"))
            return true;
    }

    if (p.actions.gameRva) {
        const std::uintptr_t game =
            moduleBase() +
            static_cast<std::uintptr_t>(static_cast<std::intptr_t>(p.actions.gameRva));
        for (int off = 0; off <= 4096; off += static_cast<int>(sizeof(void*))) {
            std::uintptr_t fieldPtr = 0;
            std::uintptr_t fieldRep = 0;
            if (!readPtr(game + static_cast<std::uintptr_t>(off), fieldPtr) || fieldPtr != obj)
                continue;
            readPtr(game + static_cast<std::uintptr_t>(off) + sizeof(void*), fieldRep);
            if (take(fieldPtr, fieldRep, "g_game"))
                return true;
        }
    }

    for (int off = 4; off <= 2048; off += static_cast<int>(sizeof(void*))) {
        std::uintptr_t fieldPtr = 0;
        std::uintptr_t fieldRep = 0;
        if (!readPtr(obj + static_cast<std::uintptr_t>(off), fieldPtr) || fieldPtr != obj)
            continue;
        readPtr(obj + static_cast<std::uintptr_t>(off) + sizeof(void*), fieldRep);
        if (take(obj, fieldRep, "in-object"))
            return true;
    }

    if (take(obj, 0, "make_shared"))
        return true;

    const int creatureWidth =
        p.creatures.creaturePtrSize > 0 ? p.creatures.creaturePtrSize : p.creatures.ptrSize;
    if (p.creatures.present && creatureWidth == static_cast<int>(sizeof(void*)) &&
        looksLikeHeapObject(obj) && looksLikeVtable(obj)) {
        ptr = obj;
        rep = 0;
        LOG("useWith self intrusive ptr=%p width=%d", reinterpret_cast<void*>(ptr),
            creatureWidth);
        return true;
    }

    LOG("useWith self no control block obj=%p slot=%p", reinterpret_cast<void*>(obj),
        reinterpret_cast<void*>(lastSlot));
    return false;
}

void setCalibHints(const CalibHints& hints) {
    std::lock_guard<std::mutex> lock(g_mtx);
    g_hints = hints;
    LOG("calib hints hp=%d/%d mana=%d/%d level=%d exp=%llu cap=%d battleBits=%u pos=%d "
        "bounds=%d[%d..%d,%d..%d] itemId=%d",
        hints.health, hints.maxHealth, hints.mana, hints.maxMana, hints.level,
        static_cast<unsigned long long>(hints.experience), hints.capacity, hints.expectStateBits,
        hints.hasPosition ? 1 : 0, hints.hasPositionBounds ? 1 : 0, hints.minX, hints.maxX,
        hints.minY, hints.maxY, hints.itemId);
}

bool runCalibrationScan() {
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_phase = "scanning";
        g_phaseMsg = "heap scan for LocalPlayer stats block";
    }
    PipeServer::instance().emit("dllStatus", dllStatusJson());

    const bool ok = heapScan();
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_readyEmitted = false;
    }
    PipeServer::instance().emit("dllStatus", dllStatusJson());
    if (ok)
        replayStatusToPipe();
    return ok;
}

bool runRefineScan() {
    CalibHints hints;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        hints = g_hints;
        g_phaseMsg = "refine position/states/inventory on locked LocalPlayer";
    }
    PipeServer::instance().emit("dllStatus", dllStatusJson());

    bool ok = false;
    for (int i = 0; i < 15; ++i) {
        bool live = false;
        {
            std::lock_guard<std::mutex> lock(g_mtx);
            live = g_statsBase != 0 || tryApplyProfile();
        }
        if (live)
            break;
        Sleep(200);
    }
    {
        Profile layout;
        bool needScan = false;
        {
            std::lock_guard<std::mutex> lock(g_mtx);
            needScan = g_statsBase == 0;
            layout = g_profile;
        }
        if (needScan)
            heapRestoreFromProfile(layout);
    }
    const int refineAttempts = hints.requirePositionMovement ? 1 : 10;
    for (int i = 0; i < refineAttempts && !ok; ++i) {
        ok = refineLockedPlayer(hints);
        if (ok)
            break;
        Sleep(200);
    }

    {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_readyEmitted = false;
    }
    PipeServer::instance().emit("dllStatus", dllStatusJson());
    if (ok)
        replayStatusToPipe();
    return ok;
}

bool saveCurrentProfile() {
    std::lock_guard<std::mutex> lock(g_mtx);
    if (!g_profile.containers.present && (!g_statsBase || !g_snap.valid)) {
        LOG("save skipped statsBase=%d valid=%d hp=%d/%d level=%d mana=%d/%d",
            g_statsBase != 0 ? 1 : 0, g_snap.valid ? 1 : 0, g_snap.health, g_snap.maxHealth,
            g_snap.level, g_snap.mana, g_snap.maxMana);
        return false;
    }
    g_profile.buildId = g_build.id;
    g_profile.version = 1;

    const auto dir = thisDllDir();
    if (!saveCalibration(g_profile, dir)) {
        LOG("save profile failed path=%ls (check DLL directory/write permissions)",
            calibrationPath(dir, g_build.id).c_str());
        return false;
    }
    LOG("saved profile path=%ls", calibrationPath(dir, g_build.id).c_str());
    return true;
}

} // namespace otc

