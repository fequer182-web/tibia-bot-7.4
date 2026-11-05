#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "IActions.hpp"
#include "OldersAttackOwnership.hpp"
#include "PendingItemPayload.hpp"
#include "../profiles/ProfileConfig.hpp"
#include "../sdk/Snapshots.hpp"
#include "../bot/ChangeGold.hpp"
#include "../bot/OldersLoot.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <fstream>
#include <sstream>
#include <optional>
#include <string>
#include <vector>

namespace crome {

// In-process action bridge for the validated Olders x64 profile.
// All build-specific addresses/layout offsets come from profiles/builds.json.
// V15 keeps the confirmed walkTo/use bridges and extends Game::useWith
// to non-creature ground Things resolved through Map::getTile on the game thread.
// This is the path required by rope/shovel/fishing style cavebot actions.
class OldersInternalActions final : public IActions {
public:
    OldersInternalActions(std::uintptr_t moduleBase,
                          const profiles::RuntimeProfile& profile)
        : base_(moduleBase), profile_(profile) {}

    ~OldersInternalActions() override { uninstallLookMonitor(); uninstallDispatcher(); }

    void setInspectResultPath(std::string path) { inspectResultPath_ = std::move(path); }
    void setLookResultPath(std::string path) { lookResultPath_ = std::move(path); }

    OldersInternalActions(const OldersInternalActions&) = delete;
    OldersInternalActions& operator=(const OldersInternalActions&) = delete;

    [[nodiscard]] std::uintptr_t gameObject() const {
        if (profile_.runtime.gameObject)
            return base_ + *profile_.runtime.gameObject;
        // Compatibility with V7-V12 profiles: the confirmed global at
        // 0x169A680 is the Game object and its first pointer is LocalPlayer.
        if (profile_.runtime.localPlayerPtr)
            return base_ + *profile_.runtime.localPlayerPtr;
        return 0;
    }

    [[nodiscard]] std::uintptr_t localPlayer() const {
        if (!profile_.runtime.localPlayerPtr) return 0;
        return readPtr(base_ + *profile_.runtime.localPlayerPtr);
    }

    [[nodiscard]] Position currentPosition() const {
        Position out{};
        const auto player = localPlayer();
        if (!player) return out;
        out.x = static_cast<std::int32_t>(readU32(player + profile_.creature.x));
        out.y = static_cast<std::int32_t>(readU32(player + profile_.creature.y));
        out.z = static_cast<std::int32_t>(readU8(player + profile_.creature.z));
        return out;
    }

    [[nodiscard]] std::optional<LocalPlayerSnapshot> localPlayerSnapshot() const {
        const auto player = localPlayer();
        if (!player) return std::nullopt;

        LocalPlayerSnapshot s{};
        s.address = player;
        const auto& o = profile_.localPlayer;

        switch (profile_.localPlayerStates) {
            case SmallFieldStorage::UInt8: {
                std::uint8_t v{}; if (!safeRead(player + o.states, v)) return std::nullopt; s.states = v; break;
            }
            case SmallFieldStorage::UInt16: {
                std::uint16_t v{}; if (!safeRead(player + o.states, v)) return std::nullopt; s.states = v; break;
            }
            case SmallFieldStorage::UInt32: {
                std::uint32_t v{}; if (!safeRead(player + o.states, v)) return std::nullopt; s.states = v; break;
            }
        }

        if (profile_.localPlayerStats == StatStorage::Double) {
            if (!safeRead(player + o.health, s.health) ||
                !safeRead(player + o.maxHealth, s.maxHealth) ||
                !safeRead(player + o.capacity, s.capacity) ||
                !safeRead(player + o.experience, s.experience) ||
                !safeRead(player + o.level, s.level) ||
                !safeRead(player + o.mana, s.mana) ||
                !safeRead(player + o.maxMana, s.maxMana) ||
                !safeRead(player + o.magicLevel, s.magicLevel))
                return std::nullopt;
        } else {
            std::uint64_t hp{}, maxHp{}, cap{}, exp{}, lvl{}, mana{}, maxMana{}, ml{};
            if (!safeRead(player + o.health, hp) ||
                !safeRead(player + o.maxHealth, maxHp) ||
                !safeRead(player + o.capacity, cap) ||
                !safeRead(player + o.experience, exp) ||
                !safeRead(player + o.level, lvl) ||
                !safeRead(player + o.mana, mana) ||
                !safeRead(player + o.maxMana, maxMana) ||
                !safeRead(player + o.magicLevel, ml))
                return std::nullopt;
            s.health = static_cast<double>(hp);
            s.maxHealth = static_cast<double>(maxHp);
            s.capacity = static_cast<double>(cap);
            s.experience = static_cast<double>(exp);
            s.level = static_cast<double>(lvl);
            s.mana = static_cast<double>(mana);
            s.maxMana = static_cast<double>(maxMana);
            s.magicLevel = static_cast<double>(ml);
        }
        return s;
    }

    [[nodiscard]] bool isWorldReady() const {
        const auto player = localPlayer();
        if (!player) return false;
        const auto p = currentPosition();
        return p.x > 0 && p.y > 0 && p.x != 0xFFFF && p.y != 0xFFFF &&
               p.z >= 0 && p.z != 0xFF;
    }

    bool validateRuntime(bool verbose = false) const {
        const auto& r = profile_.runtime;
        if (!r.localPlayerPtr || !r.localPlayerVtable ||
            !r.localPlayerSlotTarget || !profile_.actions.walk) {
            if (verbose) std::cout << "[FAIL] Profile sem RVAs necessarios para walk.\n";
            return false;
        }

        const auto player = localPlayer();
        const auto vtable = player ? readPtr(player) : 0;
        const auto slot = vtable && r.localPlayerSlotOffset
            ? readPtr(vtable + r.localPlayerSlotOffset)
            : 0;

        const bool playerOk = player != 0;
        const bool vtableOk = vtable == base_ + *r.localPlayerVtable;
        const bool slotOk = slot == base_ + *r.localPlayerSlotTarget;
        const bool worldOk = isWorldReady();

        if (verbose) {
            const auto p = currentPosition();
            std::cout << "\n============================================================\n"
                      << "              CROME WALK RUNTIME VALIDATION\n"
                      << "============================================================\n"
                      << std::hex << std::uppercase
                      << "Game object          : 0x" << gameObject() << "\n"
                      << "LocalPlayer          : 0x" << player << "\n"
                      << "VTable               : 0x" << vtable
                      << "  expected 0x" << (base_ + *r.localPlayerVtable) << "\n"
                      << "Slot +0x" << r.localPlayerSlotOffset << "          : 0x" << slot
                      << "  expected 0x" << (base_ + *r.localPlayerSlotTarget) << "\n"
                      << "Walk wrapper         : RVA 0x" << *profile_.actions.walk << "\n";
            if (r.gameAutoWalk)
                std::cout << "Game::autoWalk       : RVA 0x" << *r.gameAutoWalk << "\n";
            if (profile_.actions.attack)
                std::cout << "Game::attack         : RVA 0x" << *profile_.actions.attack << "\n";
            if (profile_.actions.use)
                std::cout << "Game::use            : RVA 0x" << *profile_.actions.use << "\n";
            if (profile_.actions.useWith)
                std::cout << "Game::useWith        : RVA 0x" << *profile_.actions.useWith << "\n";
            if (profile_.actions.sayDefault)
                std::cout << "Game::talk           : RVA 0x" << *profile_.actions.sayDefault << "\n";
            if (r.mapGetTile)
                std::cout << "Map::getTile         : RVA 0x" << *r.mapGetTile << "\n";
            std::cout << std::dec
                      << "Position             : " << p.x << ", " << p.y << ", " << p.z << "\n"
                      << "Player pointer       : " << (playerOk ? "OK" : "FAIL") << "\n"
                      << "VTable match         : " << (vtableOk ? "OK" : "FAIL") << "\n"
                      << "Slot match           : " << (slotOk ? "OK" : "FAIL") << "\n"
                      << "World ready          : " << (worldOk ? "OK" : "FAIL") << "\n";
        }
        return playerOk && vtableOk && slotOk && worldOk;
    }

    bool installDispatcher() {
        if (originalWndProc_) return true;
        if (active_ && active_ != this) return false;

        gameWindow_ = findCurrentProcessWindow();
        if (!gameWindow_) {
            std::cout << "[FAIL] Janela do Olders nao encontrada.\n";
            return false;
        }

        active_ = this;
        SetLastError(0);
        const LONG_PTR previous = SetWindowLongPtr(
            gameWindow_, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(&OldersInternalActions::hookedWndProc));
        if (!previous) {
            const auto error = GetLastError();
            active_ = nullptr;
            gameWindow_ = nullptr;
            std::cout << "[FAIL] WndProc dispatcher. Win32 error=" << error << "\n";
            return false;
        }

        originalWndProc_ = reinterpret_cast<WNDPROC>(previous);
        std::cout << "[OK] Game-thread dispatcher instalado.\n";
        if (!installLookMonitor())
            std::cout << "[WARN] Look monitor nao instalado; runtime principal continua ativo.\n";
        return true;
    }

    void uninstallDispatcher() {
        if (gameWindow_ && originalWndProc_ && followBlockInstalled_.load(std::memory_order_relaxed)) {
            desiredFollowBlocked_.store(false, std::memory_order_relaxed);
            SendMessage(gameWindow_, kFollowBlockMessage, 0, 0);
        }
        if (gameWindow_ && originalWndProc_) {
            SetWindowLongPtr(gameWindow_, GWLP_WNDPROC,
                             reinterpret_cast<LONG_PTR>(originalWndProc_));
            std::cout << "[OK] Game-thread dispatcher removido.\n";
        }
        originalWndProc_ = nullptr;
        gameWindow_ = nullptr;
        InterlockedExchange(&walkPending_, 0);
        InterlockedExchange(&itemActionPending_, 0);
        InterlockedExchange(&talkPending_, 0);
        InterlockedExchange(&attackPending_, 0);
        pendingAttackCreature_ = 0;
        desiredFollowBlocked_.store(false, std::memory_order_relaxed);
        followBlockInstalled_.store(false, std::memory_order_relaxed);
        pendingItemAction_ = PendingItemAction::None;
        if (active_ == this) active_ = nullptr;
    }

    [[nodiscard]] bool walkPending() const noexcept {
        return InterlockedCompareExchange(
                   const_cast<volatile LONG*>(&walkPending_), 0, 0) != 0;
    }

    [[nodiscard]] bool itemActionPending() const noexcept {
        return InterlockedCompareExchange(
                   const_cast<volatile LONG*>(&itemActionPending_), 0, 0) != 0;
    }

    // V21.5: manual interaction guard. Mouse input in the Olders window (for
    // example opening a corpse/container) temporarily suppresses bot walk
    // requests so native autoWalk is not re-issued while the client processes
    // the user's own click/use action.
    void setManualInteractionPauseMs(std::uint32_t ms) noexcept {
        manualInteractionPauseMs_ = std::clamp<std::uint32_t>(ms, 250u, 5000u);
    }

    [[nodiscard]] bool manualInteractionActive() const noexcept {
        const bool held = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ||
                          (GetAsyncKeyState(VK_RBUTTON) & 0x8000) ||
                          (GetAsyncKeyState(VK_MBUTTON) & 0x8000);
        return held || GetTickCount64() < manualInteractionUntil_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t manualInteractionRemainingMs() const noexcept {
        const auto now = static_cast<std::uint64_t>(GetTickCount64());
        const auto until = manualInteractionUntil_.load(std::memory_order_relaxed);
        return until > now ? until - now : 0;
    }

    // V21.6: while bot Targeting owns combat/chase, neutralize the client's
    // native Game::follow entry point. The patch is applied/restored on the
    // Olders window/game thread and touches only the first byte: RET (0xC3).
    // This prevents a manual Follow click from cancelling AttackTarget and
    // racing the bot's walkTo chase. When Targeting is OFF the original byte
    // is restored, so normal client Follow works again.
    bool setFollowBlocked(bool blocked) {
        if (!profile_.actions.follow || !dispatcherReady()) return false;
        desiredFollowBlocked_.store(blocked, std::memory_order_relaxed);
        // Synchronous on purpose: when DELETE enables Targeting, Game::follow
        // is already neutralized before the user can issue the next UI command.
        SendMessage(gameWindow_, kFollowBlockMessage, 0, 0);
        return followBlockInstalled_.load(std::memory_order_relaxed) == blocked;
    }

    [[nodiscard]] bool followBlocked() const noexcept {
        return followBlockInstalled_.load(std::memory_order_relaxed);
    }

    // V21 targeting bridge. Creature enumeration executes synchronously on the
    // game/window thread because it calls the validated Map::getTile method.
    // Only Monster-vtable Things are returned.
    [[nodiscard]] std::uintptr_t currentAttackTargetAddress() const {
        const auto game = gameObject();
        if (!game || !isReadableRange(game + 0x08, sizeof(std::uintptr_t))) return 0;
        return readPtr(game + 0x08);
    }

    [[nodiscard]] std::uintptr_t currentFollowTargetAddress() const {
        const auto game = gameObject();
        if (!game || !isReadableRange(game + 0x10, sizeof(std::uintptr_t))) return 0;
        return readPtr(game + 0x10);
    }

    [[nodiscard]] std::optional<CreatureSnapshot> monsterSnapshot(std::uintptr_t thing) const {
        if (!thing || !profile_.runtime.monsterVtable ||
            !isReadableRange(thing, 0x70) ||
            readPtr(thing) != base_ + *profile_.runtime.monsterVtable) {
            return std::nullopt;
        }

        CreatureSnapshot c{};
        c.address = thing;
        c.id = readU32(thing + profile_.creature.id);
        c.position.x = static_cast<std::int32_t>(readU32(thing + profile_.creature.x));
        c.position.y = static_cast<std::int32_t>(readU32(thing + profile_.creature.y));
        c.position.z = static_cast<std::int32_t>(readU8(thing + profile_.creature.z));
        c.hpPercent = readCreatureHp(thing);
        c.direction = readCreatureDirection(thing);
        c.alive = c.hpPercent > 0 && c.hpPercent <= 100;
        if (profile_.creature.skull) c.skull = readU8(thing + *profile_.creature.skull);
        if (profile_.creature.party) c.party = readU8(thing + *profile_.creature.party);
        readNativeMsvcString(thing + profile_.creature.name, c.name);
        return c;
    }

    [[nodiscard]] std::vector<CreatureSnapshot> visibleMonsters(int maxRange) {
        creatureScanResult_.clear();
        if (!validateRuntime(false) || !dispatcherReady() || !mapRuntimeReady() ||
            !profile_.runtime.monsterVtable) {
            return creatureScanResult_;
        }
        pendingCreatureScanRange_ = std::clamp(maxRange, 0, 15);
        SendMessage(gameWindow_, kCreatureScanMessage, 0, 0);
        return creatureScanResult_;
    }

    bool attackAddress(std::uintptr_t creature, std::uint32_t expectedId) {
        if (!validateRuntime(false) || !profile_.actions.attack ||
            !profile_.runtime.monsterVtable || !dispatcherReady()) {
            std::cout << "\n[ATTACK] Runtime/RVA Game::attack nao disponivel.\n";
            return false;
        }
        // V21.3: do not dereference Creature* from the worker thread.
        // The creature may be removed between polls. The actual pointer/vtable
        // validation is performed by executePendingAttack() on the game thread.
        if (!creature || !expectedId) {
            std::cout << "\n[ATTACK] Creature pointer nulo.\n";
            return false;
        }
        if (InterlockedCompareExchange(&attackPending_, 1, 0) != 0) {
            std::cout << "\n[ATTACK] Ja existe uma requisicao pendente.\n";
            return false;
        }
        pendingAttackCreature_ = creature;
        pendingAttackId_ = expectedId;
        if (!PostMessage(gameWindow_, kAttackMessage, 0, 0)) {
            pendingAttackCreature_ = 0;
            InterlockedExchange(&attackPending_, 0);
            std::cout << "\n[ATTACK] PostMessage falhou. error=" << GetLastError() << "\n";
            return false;
        }
        return true;
    }

    // IActions -------------------------------------------------
    bool attack(std::uint32_t creatureId) override {
        if (!creatureId) return false;
        const auto monsters = visibleMonsters(15);
        for (const auto& c : monsters) {
            if (c.id == creatureId) return attackAddress(c.address, c.id);
        }
        return false;
    }
    // Automatic follow is intentionally not bound into V21 targeting: the
    // validated OTClient-family behavior can cancel an active attack.
    bool follow(std::uint32_t) override { return false; }

    bool walkTo(Position p) override {
        if (!validateRuntime(false)) {
            std::cout << "\n[WALK] Runtime nao esta valido.\n";
            return false;
        }
        if (!dispatcherReady()) {
            std::cout << "\n[WALK] Dispatcher nao instalado.\n";
            return false;
        }
        if (p.x <= 0 || p.y <= 0 || p.z < 0 || p.z > 255) {
            std::cout << "\n[WALK] Destino invalido.\n";
            return false;
        }
        if (manualInteractionActive()) {
            return false;
        }
        if (InterlockedCompareExchange(&walkPending_, 1, 0) != 0) {
            std::cout << "\n[WALK] Ja existe uma requisicao pendente.\n";
            return false;
        }

        pendingDestination_.x = static_cast<std::uint32_t>(p.x);
        pendingDestination_.y = static_cast<std::uint32_t>(p.y);
        pendingDestination_.z = static_cast<std::uint8_t>(p.z);
        pendingDestination_.pad[0] = pendingDestination_.pad[1] = pendingDestination_.pad[2] = 0;

        if (!PostMessage(gameWindow_, kWalkMessage, 0, 0)) {
            InterlockedExchange(&walkPending_, 0);
            std::cout << "\n[WALK] PostMessage falhou. error=" << GetLastError() << "\n";
            return false;
        }
        return true;
    }

    bool look(Position) override { return false; }

    bool use(const Item& item) override {
        if (!validateRuntime(false) || !profile_.actions.use) {
            std::cout << "\n[USE] Runtime/RVA Game::use nao disponivel.\n";
            return false;
        }
        if (!dispatcherReady()) {
            std::cout << "\n[USE] Dispatcher nao instalado.\n";
            return false;
        }

        FoundItem found{};
        if (!findUsableItemById(item.id, found)) {
            std::cout << "\n[USE] Item ID " << item.id
                      << " nao encontrado em equipment nem containers abertos.\n";
            return false;
        }
        return requestItemAction(PendingItemAction::Use, item.id, found);
    }

    bool changeGold(const std::vector<std::uint16_t>& ids) {
        if(ids.empty() || !dispatcherReady() || !validateRuntime(false) || !profile_.actions.use)return false;
        return requestItemAction(PendingItemAction::ChangeGold,0,FoundItem{},nullptr,ids);
    }

    // Creature/self path validated in V13.
    bool useWith(const Item& item, Position target) override {
        if (!validateRuntime(false) || !profile_.actions.useWith) {
            std::cout << "\n[USEWITH] Runtime/RVA Game::useWith nao disponivel.\n";
            return false;
        }
        if (!dispatcherReady()) {
            std::cout << "\n[USEWITH] Dispatcher nao instalado.\n";
            return false;
        }
        const auto self = currentPosition();
        if (target.x != self.x || target.y != self.y || target.z != self.z) {
            std::cout << "\n[USEWITH] Este metodo e reservado ao target LocalPlayer; use useWithGround para tile.\n";
            return false;
        }
        FoundItem found{};
        if (!findUsableItemById(item.id, found)) {
            std::cout << "\n[USEWITH] Item ID " << item.id
                      << " nao encontrado em equipment nem containers abertos.\n";
            return false;
        }
        return requestItemAction(PendingItemAction::UseWithSelf, item.id, found, &target);
    }

    // Ground/item path. Upstream OTC Game::useWith dispatches to the item/ground
    // protocol branch when toThing is not a Creature. We resolve Thing[0] (ground)
    // from the requested tile on the game thread.
    bool useWithGround(const Item& item, Position target) override {
        if (!validateRuntime(false) || !profile_.actions.useWith || !mapRuntimeReady()) {
            std::cout << "\n[USEWITH-GROUND] Runtime/RVA/map nao disponivel.\n";
            return false;
        }
        if (!dispatcherReady()) return false;
        FoundItem found{};
        if (!findUsableItemById(item.id, found)) {
            std::cout << "\n[USEWITH-GROUND] Item ID " << item.id
                      << " nao encontrado em equipment nem containers abertos.\n";
            return false;
        }
        return requestItemAction(PendingItemAction::UseWithGround, item.id, found, &target);
    }

    // Diagnostic/helper for fishing: search the current floor for the nearest
    // ground Thing whose ID is in groundIds, then execute the same generic
    // Game::useWith path. The Map::getTile scan and the native call both run on
    // the game/window thread.
    bool useWithNearestGroundIds(const Item& item,
                                 const std::vector<std::uint16_t>& groundIds,
                                 int maxRange) {
        if (!validateRuntime(false) || !profile_.actions.useWith || !mapRuntimeReady()) {
            std::cout << "\n[AUTO-GROUND] Runtime/RVA/map nao disponivel.\n";
            return false;
        }
        if (!dispatcherReady() || groundIds.empty() || maxRange < 0) return false;
        FoundItem found{};
        if (!findUsableItemById(item.id, found)) {
            std::cout << "\n[AUTO-GROUND] Item ID " << item.id
                      << " nao encontrado em equipment nem containers abertos.\n";
            return false;
        }
        return requestItemAction(PendingItemAction::UseWithNearestGround,
                                 item.id, found, nullptr, groundIds, maxRange);
    }

    // Diagnostic used by V15 hotkey F2. Map::getTile itself executes only on
    // the game/window thread through the same dispatcher as item actions.
    bool inspectTile(Position target) {
        if (!validateRuntime(false) || !mapRuntimeReady()) {
            std::cout << "\n[TILE] Map runtime nao esta configurado/valido.\n";
            return false;
        }
        if (!dispatcherReady()) return false;
        if (InterlockedCompareExchange(&itemActionPending_, 1, 0) != 0) {
            std::cout << "\n[TILE] Ja existe uma requisicao pendente.\n";
            return false;
        }
        pendingItemAction_ = PendingItemAction::InspectTile;
        pendingRequestedItemId_ = 0;
        pendingFoundItem_ = {};
        setPendingTarget(target);
        if (!PostMessage(gameWindow_, kItemActionMessage, 0, 0)) {
            pendingItemAction_ = PendingItemAction::None;
            InterlockedExchange(&itemActionPending_, 0);
            return false;
        }
        return true;
    }

    bool useGroundObject(std::uint32_t itemId, Position target) override {
        if (!itemId || !validateRuntime(false) || !mapRuntimeReady() || !profile_.actions.use) return false;
        if (!dispatcherReady()) return false;
        if (InterlockedCompareExchange(&itemActionPending_, 1, 0) != 0) return false;
        pendingItemAction_ = PendingItemAction::UseGroundObject;
        pendingRequestedItemId_ = itemId;
        pendingFoundItem_ = {};
        setPendingTarget(target);
        if (!PostMessage(gameWindow_, kItemActionMessage, 0, 0)) {
            pendingItemAction_ = PendingItemAction::None;
            InterlockedExchange(&itemActionPending_, 0);
            return false;
        }
        return true;
    }

    OldersLootStep lootStep(const OldersLootConfig& config) {
        if(!dispatcherReady())return {false,false,{},"dispatcher_unavailable"};
        lootConfig_=config;SendMessage(gameWindow_,kLootMessage,0,0);return lootResult_;
    }
    bool move(const Item&, Position, Position, std::uint32_t) override { return false; }
    bool open(Position) override { return false; }
    bool close(std::size_t) override { return false; }

    bool castSpell(std::string_view spell) override { return sayDefault(spell); }

    bool sayDefault(std::string_view text) override {
        if (!validateRuntime(false) || !profile_.actions.sayDefault) {
            std::cout << "\n[SAY] Runtime/RVA Game::talk nao disponivel.\n";
            return false;
        }
        if (!dispatcherReady()) {
            std::cout << "\n[SAY] Dispatcher nao instalado.\n";
            return false;
        }
        if (text.empty()) {
            std::cout << "\n[SAY] Texto vazio.\n";
            return false;
        }
        // V18 uses only the recovered native SSO form. The Olders string
        // object is 32 bytes with size at +0x10 and capacity at +0x18.
        // Capacity 15 selects the inline 16-byte storage at +0x00.
        if (text.size() > 15) {
            std::cout << "\n[SAY] V18 suporta ate 15 bytes (SSO nativo). Texto tem "
                      << text.size() << ".\n";
            return false;
        }
        if (InterlockedCompareExchange(&talkPending_, 1, 0) != 0) {
            std::cout << "\n[SAY] Ja existe uma requisicao pendente.\n";
            return false;
        }
        std::memset(pendingTalkText_, 0, sizeof(pendingTalkText_));
        std::memcpy(pendingTalkText_, text.data(), text.size());
        pendingTalkLength_ = static_cast<std::uint8_t>(text.size());
        if (!PostMessage(gameWindow_, kTalkMessage, 0, 0)) {
            InterlockedExchange(&talkPending_, 0);
            std::cout << "\n[SAY] PostMessage falhou. error=" << GetLastError() << "\n";
            return false;
        }
        return true;
    }

private:
#pragma pack(push, 1)
    struct NativePosition {
        std::uint32_t x{};
        std::uint32_t y{};
        std::uint8_t z{};
        std::uint8_t pad[3]{};
    };
#pragma pack(pop)
    static_assert(sizeof(NativePosition) == 12);

    // Native string layout recovered in the earlier V13 string-producer scans.
    // This matches the Olders short-string representation without crossing the
    // MinGW std::string ABI boundary.
    struct NativeString32 {
        char storage[16]{};
        std::uint64_t size{};
        std::uint64_t capacity{15};
    };
    static_assert(sizeof(NativeString32) == 32);
    static_assert(offsetof(NativeString32, size) == 0x10);
    static_assert(offsetof(NativeString32, capacity) == 0x18);

    struct ThingHandle {
        std::uintptr_t thing{};
        std::uintptr_t representation{};
    };

    struct FoundItem {
        std::uintptr_t ptr{};
        std::uint32_t containerId{};
        std::uint32_t index{};
        std::uint16_t subtype{};
        bool equipped{};
    };

    enum class PendingItemAction : std::uint8_t {
        None,
        Use,
        UseWithSelf,
        UseWithGround,
        UseWithNearestGround,
        InspectTile,
        UseGroundObject,
        ChangeGold,
    };

    using LocalPlayerAutoWalkFn = bool(__fastcall*)(void*, NativePosition*, bool);
    using GameUseFn = void(__fastcall*)(void*, ThingHandle*);
    using GameUseWithFn = void(__fastcall*)(void*, ThingHandle*, ThingHandle*, int);
    using MapGetTileFn = std::uintptr_t(__fastcall*)(void*, NativePosition*);
    using GameTalkFn = void(__fastcall*)(void*, const NativeString32*);
    using GameAttackFn = void(__fastcall*)(void*, ThingHandle*, bool);
    using GameLookFn = std::uintptr_t(__fastcall*)(void*, void*, void*, void*);

    static constexpr UINT kWalkMessage = WM_APP + 0x514;
    static constexpr UINT kItemActionMessage = WM_APP + 0x515;
    static constexpr UINT kTalkMessage = WM_APP + 0x516;
    static constexpr UINT kCreatureScanMessage = WM_APP + 0x517;
    static constexpr UINT kAttackMessage = WM_APP + 0x518;
    static constexpr UINT kFollowBlockMessage = WM_APP + 0x519;
    static constexpr UINT kLootMessage = WM_APP + 0x51A;
    inline static OldersInternalActions* active_ = nullptr;

    std::uintptr_t base_{};
    const profiles::RuntimeProfile& profile_;
    HWND gameWindow_{};
    WNDPROC originalWndProc_{};

    volatile LONG walkPending_{0};
    NativePosition pendingDestination_{};

    volatile LONG itemActionPending_{0};
    PendingItemAction pendingItemAction_{PendingItemAction::None};
    std::uint32_t pendingRequestedItemId_{};
    FoundItem pendingFoundItem_{};
    NativePosition pendingTargetPosition_{};
    std::vector<std::uint16_t> pendingGroundIds_{};
    int pendingGroundRange_{7};
    mutable std::size_t lastGroundPick_{static_cast<std::size_t>(-1)};
    mutable std::uint64_t groundRngState_{0x9E3779B97F4A7C15ull};

    volatile LONG talkPending_{0};
    char pendingTalkText_[16]{};
    std::uint8_t pendingTalkLength_{};

    int pendingCreatureScanRange_{7};
    std::vector<CreatureSnapshot> creatureScanResult_{};

    volatile LONG attackPending_{0};
    std::uintptr_t pendingAttackCreature_{};
    std::uint32_t pendingAttackId_{};
    std::atomic<std::uint64_t> lootManualUntil_{0};

    std::atomic<bool> desiredFollowBlocked_{false};
    std::atomic<bool> followBlockInstalled_{false};
    std::uint8_t followOriginalByte_{0};
    std::size_t followPatchOffset_{0};
    bool followOriginalByteSaved_{false};

    std::atomic<std::uint64_t> manualInteractionUntil_{0};
    std::uint32_t manualInteractionPauseMs_{1500};
    std::string inspectResultPath_{};
    std::string lookResultPath_{};
    GameLookFn gameLookTrampoline_{};
    void* gameLookTrampolineMem_{};
    std::uint8_t gameLookOriginal_[15]{};
    bool gameLookHookInstalled_{false};

    bool findThingPositionGameThread(std::uintptr_t wanted, NativePosition& out) const {
        if (!wanted || !mapRuntimeReady()) return false;
        const auto self = currentPosition();
        for (int dy=-7; dy<=7; ++dy) {
            for (int dx=-10; dx<=10; ++dx) {
                const auto tx = static_cast<std::int64_t>(self.x) + dx;
                const auto ty = static_cast<std::int64_t>(self.y) + dy;
                if (tx <= 0 || ty <= 0) continue;
                NativePosition p{}; p.x=static_cast<std::uint32_t>(tx); p.y=static_cast<std::uint32_t>(ty); p.z=static_cast<std::uint8_t>(self.z);
                std::uintptr_t slot{}, tile{}, ground{}; std::size_t count{};
                if (!resolveTileGroundGameThread(p, slot, tile, ground, count) || !tile) continue;
                const auto begin = readPtr(tile + profile_.map.tileThingsBegin);
                if (!begin) continue;
                for (std::size_t i=0; i<count && i<64; ++i) {
                    const auto thing = readPtr(begin + i*sizeof(std::uintptr_t));
                    if (thing == wanted) { out=p; return true; }
                }
            }
        }
        return false;
    }

    void writeLookResult(std::uintptr_t thing) {
        if (!thing || !isReadableRange(thing, profile_.container.itemId + sizeof(std::uint32_t))) return;
        std::uint32_t id{};
        if (!safeRead(thing + profile_.container.itemId, id)) return;
        if (!id || lookResultPath_.empty()) return;
        NativePosition pos{}; const bool onGround = findThingPositionGameThread(thing, pos);
        std::ofstream out(lookResultPath_, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out << "{\n  \"valid\": true,\n  \"id\": " << id
            << ",\n  \"onGround\": " << (onGround ? "true" : "false");
        if (onGround) out << ",\n  \"x\": " << pos.x << ",\n  \"y\": " << pos.y << ",\n  \"z\": " << static_cast<unsigned>(pos.z);
        out << "\n}\n";
        std::cout << "[LOOK] id=" << id;
        if (onGround) std::cout << " pos=" << pos.x << "," << pos.y << "," << static_cast<unsigned>(pos.z);
        else std::cout << " container/inventory";
        std::cout << "\n";
    }

    static std::uintptr_t __fastcall hookedGameLook(void* a1, void* a2, void* a3, void* a4) {
        auto* self = active_;
        if (!self || !self->gameLookTrampoline_) return 0;
        std::uintptr_t thing{};
        const auto rdx = reinterpret_cast<std::uintptr_t>(a2);
        if (rdx && self->isReadableRange(rdx, sizeof(std::uintptr_t)))
            std::memcpy(&thing, reinterpret_cast<const void*>(rdx), sizeof(thing));
        if (thing && self->isReadableRange(thing, self->profile_.container.itemId + sizeof(std::uint32_t)))
            self->writeLookResult(thing);
        return self->gameLookTrampoline_(a1, a2, a3, a4);
    }

    bool installLookMonitor() {
        if (gameLookHookInstalled_) return true;
        if (!profile_.actions.look) return false;
        const auto target = base_ + *profile_.actions.look;
        if (!isReadableRange(target, 15)) return false;
        // Olders confirmed runtime path is Game::detailedLook @ 0x163B10.
        // Probe v1.5 confirmed Thing* = *(RDX), item ID at Thing+0x38.
        const std::uint8_t expected[15] = {
            0x48,0x89,0x5C,0x24,0x18,
            0x55,0x56,0x57,
            0x48,0x81,0xEC,0xB0,0x00,0x00,0x00
        };
        if (std::memcmp(reinterpret_cast<const void*>(target), expected, sizeof(expected)) != 0) {
            std::cout << "[LOOK] Game::detailedLook signature mismatch; monitor desativado.\n";
            return false;
        }
        std::memcpy(gameLookOriginal_, reinterpret_cast<const void*>(target), 15);
        auto* tramp = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!tramp) return false;
        std::memcpy(tramp, gameLookOriginal_, 15);
        std::uint8_t tail[12] = {0x48,0xB8};
        const auto back = target + 15;
        std::memcpy(tail+2, &back, sizeof(back)); tail[10]=0xFF; tail[11]=0xE0;
        std::memcpy(tramp+15, tail, sizeof(tail));
        gameLookTrampolineMem_ = tramp;
        gameLookTrampoline_ = reinterpret_cast<GameLookFn>(tramp);

        DWORD oldProtect{};
        if (!VirtualProtect(reinterpret_cast<void*>(target), 15, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            VirtualFree(tramp,0,MEM_RELEASE); gameLookTrampolineMem_=nullptr; gameLookTrampoline_=nullptr; return false;
        }
        std::uint8_t patch[15] = {0x48,0xB8};
        const auto hook = reinterpret_cast<std::uintptr_t>(&OldersInternalActions::hookedGameLook);
        std::memcpy(patch+2, &hook, sizeof(hook)); patch[10]=0xFF; patch[11]=0xE0; patch[12]=patch[13]=patch[14]=0x90;
        std::memcpy(reinterpret_cast<void*>(target), patch, sizeof(patch));
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(target), sizeof(patch));
        DWORD tmp{}; VirtualProtect(reinterpret_cast<void*>(target), 15, oldProtect, &tmp);
        gameLookHookInstalled_=true;
        std::cout << "[OK] Look ID monitor instalado em Game::detailedLook.\n";
        return true;
    }

    void uninstallLookMonitor() {
        if (gameLookHookInstalled_ && profile_.actions.look) {
            const auto target = base_ + *profile_.actions.look;
            DWORD oldProtect{};
            if (VirtualProtect(reinterpret_cast<void*>(target), 15, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                std::memcpy(reinterpret_cast<void*>(target), gameLookOriginal_, 15);
                FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(target), 15);
                DWORD tmp{}; VirtualProtect(reinterpret_cast<void*>(target), 15, oldProtect, &tmp);
            }
        }
        gameLookHookInstalled_=false;
        gameLookTrampoline_=nullptr;
        if (gameLookTrampolineMem_) { VirtualFree(gameLookTrampolineMem_,0,MEM_RELEASE); gameLookTrampolineMem_=nullptr; }
    }

    void noteManualInteraction(UINT message) noexcept {
        const auto until = static_cast<std::uint64_t>(GetTickCount64()) + manualInteractionPauseMs_;
        manualInteractionUntil_.store(until, std::memory_order_relaxed);
        lootManualUntil_.store(GetTickCount64() + 2500, std::memory_order_relaxed);
        // If a bot walk request is queued but has not executed yet, its game-thread
        // handler will observe this guard and abort. We intentionally do not call
        // any unknown native cancel-walk function.
        (void)message;
    }

    static bool isManualMouseMessage(UINT message) noexcept {
        return message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN ||
               message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN ||
               message == WM_LBUTTONDBLCLK || message == WM_RBUTTONDBLCLK ||
               message == WM_MBUTTONDBLCLK || message == WM_XBUTTONDBLCLK ||
               message == WM_LBUTTONUP || message == WM_RBUTTONUP ||
               message == WM_MBUTTONUP || message == WM_XBUTTONUP ||
               message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL;
    }

    bool applyFollowBlockOnGameThread() {
        if (!profile_.actions.follow) return false;
        const auto functionAddress = base_ + *profile_.actions.follow;
        if (!isReadableRange(functionAddress, 8)) return false;

        const bool wantBlocked = desiredFollowBlocked_.load(std::memory_order_relaxed);
        const bool installed = followBlockInstalled_.load(std::memory_order_relaxed);
        if (wantBlocked == installed) return true;

        if (wantBlocked && !followOriginalByteSaved_) {
            std::uint8_t first4[4]{};
            std::memcpy(first4, reinterpret_cast<const void*>(functionAddress), sizeof(first4));
            // Preserve Intel CET/IBT ENDBR64 (F3 0F 1E FA) when present.
            followPatchOffset_ = (first4[0] == 0xF3 && first4[1] == 0x0F &&
                                  first4[2] == 0x1E && first4[3] == 0xFA) ? 4u : 0u;
            const auto patchAddress = functionAddress + followPatchOffset_;
            if (!isReadableRange(patchAddress, 1)) return false;
            std::memcpy(&followOriginalByte_, reinterpret_cast<const void*>(patchAddress), 1);
            followOriginalByteSaved_ = true;
        }

        const auto patchAddress = functionAddress + followPatchOffset_;
        DWORD oldProtect{};
        if (!VirtualProtect(reinterpret_cast<void*>(patchAddress), 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            std::cout << "[FOLLOW-BLOCK] VirtualProtect falhou error=" << GetLastError() << "\n";
            return false;
        }

        bool ok = true;
        if (wantBlocked) {
            const std::uint8_t retOpcode = 0xC3;
            std::memcpy(reinterpret_cast<void*>(patchAddress), &retOpcode, 1);
            followBlockInstalled_.store(true, std::memory_order_relaxed);
            std::cout << "[FOLLOW-BLOCK] ON Game::follow RVA 0x" << std::hex << std::uppercase
                      << *profile_.actions.follow << " +0x" << followPatchOffset_
                      << " original=0x" << static_cast<unsigned>(followOriginalByte_)
                      << std::dec << "\n";
        } else if (followOriginalByteSaved_) {
            std::memcpy(reinterpret_cast<void*>(patchAddress), &followOriginalByte_, 1);
            followBlockInstalled_.store(false, std::memory_order_relaxed);
            std::cout << "[FOLLOW-BLOCK] OFF Game::follow restaurado.\n";
        } else {
            ok = false;
        }

        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<const void*>(patchAddress), 1);
        DWORD ignored{};
        VirtualProtect(reinterpret_cast<void*>(patchAddress), 1, oldProtect, &ignored);
        return ok;
    }

    [[nodiscard]] bool dispatcherReady() const noexcept {
        return gameWindow_ && originalWndProc_;
    }

    template <class T>
    static bool safeRead(std::uintptr_t address, T& out) {
        if (!address) return false;
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) ||
            (mbi.Protect & PAGE_NOACCESS)) return false;
        const auto regionEnd = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (address + sizeof(T) > regionEnd) return false;
        std::memcpy(&out, reinterpret_cast<const void*>(address), sizeof(T));
        return true;
    }

    static bool isReadableRange(std::uintptr_t address, std::size_t size) {
        if (!address || !size) return false;
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) ||
            (mbi.Protect & PAGE_NOACCESS)) return false;
        const auto regionEnd = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        return address + size <= regionEnd;
    }

    static std::uintptr_t readPtr(std::uintptr_t address) {
        std::uintptr_t value{};
        safeRead(address, value);
        return value;
    }

    static std::uint32_t readU32(std::uintptr_t address) {
        std::uint32_t value{};
        safeRead(address, value);
        return value;
    }

    static std::uint16_t readU16(std::uintptr_t address) {
        std::uint16_t value{};
        safeRead(address, value);
        return value;
    }

    static std::uint8_t readU8(std::uintptr_t address) {
        std::uint8_t value{};
        safeRead(address, value);
        return value;
    }

    static bool readNativeMsvcString(std::uintptr_t address, std::string& out) {
        out.clear();
        if (!address || !isReadableRange(address, 32)) return false;
        std::uint64_t size{}, capacity{};
        if (!safeRead(address + 0x10, size) || !safeRead(address + 0x18, capacity))
            return false;
        if (size > 255 || capacity > 0x100000 || size > capacity) return false;
        std::uintptr_t chars = address;
        if (capacity >= 16) {
            chars = readPtr(address);
            if (!chars) return false;
        }
        if (size && !isReadableRange(chars, static_cast<std::size_t>(size))) return false;
        out.assign(reinterpret_cast<const char*>(chars), static_cast<std::size_t>(size));
        return true;
    }

    [[nodiscard]] std::uint32_t readCreatureHp(std::uintptr_t creature) const {
        switch (profile_.creatureHpPercent) {
            case SmallFieldStorage::UInt8: return readU8(creature + profile_.creature.hpPercent);
            case SmallFieldStorage::UInt16: return readU16(creature + profile_.creature.hpPercent);
            case SmallFieldStorage::UInt32: return readU32(creature + profile_.creature.hpPercent);
        }
        return 0;
    }

    [[nodiscard]] std::uint32_t readCreatureDirection(std::uintptr_t creature) const {
        switch (profile_.creatureDirection) {
            case SmallFieldStorage::UInt8: return readU8(creature + profile_.creature.direction);
            case SmallFieldStorage::UInt16: return readU16(creature + profile_.creature.direction);
            case SmallFieldStorage::UInt32: return readU32(creature + profile_.creature.direction);
        }
        return 0;
    }

    static HWND findCurrentProcessWindow() {
        struct Data { DWORD pid; HWND hwnd; } data{GetCurrentProcessId(), nullptr};
        EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
            auto* d = reinterpret_cast<Data*>(lp);
            DWORD pid{};
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid != d->pid || !IsWindowVisible(hwnd) ||
                GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
            d->hwnd = hwnd;
            return FALSE;
        }, reinterpret_cast<LPARAM>(&data));
        return data.hwnd;
    }

    [[nodiscard]] bool mapRuntimeReady() const noexcept {
        return profile_.runtime.mapObject && profile_.runtime.mapEmptySlot &&
               profile_.runtime.mapGetTile;
    }

    void setPendingTarget(Position p) {
        pendingTargetPosition_.x = static_cast<std::uint32_t>(p.x);
        pendingTargetPosition_.y = static_cast<std::uint32_t>(p.y);
        pendingTargetPosition_.z = static_cast<std::uint8_t>(p.z);
        pendingTargetPosition_.pad[0] = pendingTargetPosition_.pad[1] =
            pendingTargetPosition_.pad[2] = 0;
    }

#if defined(__GNUC__)
    __attribute__((noinline, noclone, noipa, optimize("O0")))
#endif
    std::uintptr_t getTileSlotGameThread(const NativePosition& input) const {
        if (!mapRuntimeReady()) return 0;
        NativePosition p = input;
        volatile std::uintptr_t target = base_ + *profile_.runtime.mapGetTile;
        const auto fn = reinterpret_cast<MapGetTileFn>(static_cast<std::uintptr_t>(target));
        return fn(reinterpret_cast<void*>(base_ + *profile_.runtime.mapObject), &p);
    }

    bool resolveTileGroundGameThread(const NativePosition& p,
                                     std::uintptr_t& slot,
                                     std::uintptr_t& tile,
                                     std::uintptr_t& ground,
                                     std::size_t& count) const {
        slot = tile = ground = 0;
        count = 0;
        if (!mapRuntimeReady()) return false;
        slot = getTileSlotGameThread(p);
        const auto empty = base_ + *profile_.runtime.mapEmptySlot;
        if (!slot || slot == empty || !isReadableRange(slot, sizeof(std::uintptr_t))) return false;
        tile = readPtr(slot);
        if (!tile || !isReadableRange(tile, profile_.map.tileThingsCapacity + sizeof(std::uintptr_t))) return false;
        const auto begin = readPtr(tile + profile_.map.tileThingsBegin);
        const auto end = readPtr(tile + profile_.map.tileThingsEnd);
        if (!begin || !end || end < begin || ((end - begin) % sizeof(std::uintptr_t)) != 0) return false;
        const auto bytes = static_cast<std::size_t>(end - begin);
        if (bytes > sizeof(std::uintptr_t) * 256 || !isReadableRange(begin, bytes ? bytes : sizeof(std::uintptr_t))) return false;
        count = bytes / sizeof(std::uintptr_t);
        if (!count) return false;
        ground = readPtr(begin); // validated snapshots show Thing[0] is the ground item.
        return ground && isReadableRange(ground, 0x40);
    }

    bool groundIdWanted(std::uint16_t id) const {
        return std::find(pendingGroundIds_.begin(), pendingGroundIds_.end(), id) !=
               pendingGroundIds_.end();
    }

    bool findNearestGroundByIdGameThread(NativePosition& foundPos,
                                         std::uintptr_t& foundTile,
                                         std::uintptr_t& foundThing,
                                         std::uint16_t& foundId) const {
        foundTile = foundThing = 0;
        foundId = 0;
        if (pendingGroundIds_.empty() || pendingGroundRange_ < 0) return false;

        const auto self = currentPosition();
        if (self.x <= 0 || self.y <= 0 || self.z < 0) return false;

        struct Candidate { NativePosition pos{}; std::uintptr_t tile{}; std::uintptr_t thing{}; std::uint16_t id{}; };
        std::vector<Candidate> candidates;
        for (int dy = -pendingGroundRange_; dy <= pendingGroundRange_; ++dy) {
            for (int dx = -pendingGroundRange_; dx <= pendingGroundRange_; ++dx) {
                if (std::max(std::abs(dx), std::abs(dy)) > pendingGroundRange_) continue;
                const auto tx = static_cast<std::int64_t>(self.x) + dx;
                const auto ty = static_cast<std::int64_t>(self.y) + dy;
                if (tx <= 0 || ty <= 0) continue;
                NativePosition p{}; p.x=static_cast<std::uint32_t>(tx); p.y=static_cast<std::uint32_t>(ty); p.z=static_cast<std::uint8_t>(self.z);
                std::uintptr_t slot{}, tile{}, ground{}; std::size_t count{};
                if (!resolveTileGroundGameThread(p, slot, tile, ground, count)) continue;
                const auto id = readU16(ground + profile_.container.itemId);
                if (!groundIdWanted(id)) continue;
                candidates.push_back({p,tile,ground,id});
            }
        }
        if (candidates.empty()) return false;
        groundRngState_ ^= groundRngState_ << 13;
        groundRngState_ ^= groundRngState_ >> 7;
        groundRngState_ ^= groundRngState_ << 17;
        groundRngState_ ^= static_cast<std::uint64_t>(GetTickCount64());
        auto pick = static_cast<std::size_t>(groundRngState_ % candidates.size());
        if (candidates.size() > 1 && pick == lastGroundPick_)
            pick = (pick + 1 + static_cast<std::size_t>((groundRngState_ >> 16) % (candidates.size()-1))) % candidates.size();
        lastGroundPick_ = pick;
        const auto& c = candidates[pick]; foundPos=c.pos; foundTile=c.tile; foundThing=c.thing; foundId=c.id; return true;
    }

    bool dumpTileGameThread(const NativePosition& p) const {
        std::uintptr_t slot{}, tile{}, ground{};
        std::size_t count{};
        const bool ok = resolveTileGroundGameThread(p, slot, tile, ground, count);
        std::cout << "\n============================================================\n"
                  << "                 CROME SDK - TILE INSPECT\n"
                  << "============================================================\n"
                  << "Target               : {" << p.x << "," << p.y << ","
                  << static_cast<unsigned>(p.z) << "}\n"
                  << std::hex << std::uppercase
                  << "Map object           : 0x" << (profile_.runtime.mapObject ? base_ + *profile_.runtime.mapObject : 0) << "\n"
                  << "Map::getTile         : RVA 0x" << (profile_.runtime.mapGetTile ? *profile_.runtime.mapGetTile : 0) << "\n"
                  << "Returned slot        : 0x" << slot << "\n"
                  << "Tile*                : 0x" << tile << "\n"
                  << std::dec;

        std::vector<std::uint32_t> ids;
        if (ok) {
            const auto begin = readPtr(tile + profile_.map.tileThingsBegin);
            std::cout << "Thing count          : " << count << "\n"
                      << "Ground Thing[0]      : 0x" << std::hex << std::uppercase << ground << std::dec << "\n";
            const std::size_t show = count > 16 ? 16 : count;
            for (std::size_t i = 0; i < show; ++i) {
                const auto thing = readPtr(begin + i * sizeof(std::uintptr_t));
                if (!thing || !isReadableRange(thing, 0x40)) continue;
                const auto vt = readPtr(thing);
                const auto id = static_cast<std::uint32_t>(readU16(thing + profile_.container.itemId));
                const auto raw3c = readU16(thing + profile_.container.itemSubtype);
                if (id) ids.push_back(id);
                std::cout << "Thing[" << i << "] ptr=0x" << std::hex << std::uppercase << thing
                          << " vt=0x" << vt << std::dec << " id=" << id
                          << " subtype=" << raw3c;
                if (i == 0) std::cout << " <GROUND>";
                std::cout << "\n";
            }
        } else {
            std::cout << "[RESULT] tile/ground nao resolvido (fora do aware range ou tile vazio).\n";
        }

        if (!inspectResultPath_.empty()) {
            std::ofstream out(inspectResultPath_, std::ios::binary | std::ios::trunc);
            if (out) {
                out << "{\n  \"valid\": " << (ok ? "true" : "false")
                    << ",\n  \"x\": " << p.x << ",\n  \"y\": " << p.y
                    << ",\n  \"z\": " << static_cast<unsigned>(p.z)
                    << ",\n  \"ids\": [";
                for (std::size_t i=0;i<ids.size();++i) { if (i) out << ", "; out << ids[i]; }
                out << "]\n}\n";
            }
        }
        return ok;
    }

    bool findInventoryItemById(std::uint32_t wantedId, FoundItem& out, std::uint16_t expectedCount=0) const {
        out = {};
        if (!wantedId || wantedId > 0xFFFFu || !profile_.inventory.base || !profile_.inventory.slotStride)
            return false;
        const auto player = localPlayer();
        if (!player) return false;
        const auto slots = profile_.inventory.slotCount ? profile_.inventory.slotCount : 10;
        for (std::size_t i=0; i<slots; ++i) {
            const auto slotAddr = player + profile_.inventory.base + i * profile_.inventory.slotStride;
            if (!isReadableRange(slotAddr, sizeof(std::uintptr_t))) continue;
            const auto itemPtr = readPtr(slotAddr);
            if (!itemPtr || !isReadableRange(itemPtr, profile_.container.itemSubtype + sizeof(std::uint16_t))) continue;
            if (readU16(itemPtr + profile_.container.itemId) != static_cast<std::uint16_t>(wantedId)) continue;
            if (!coinStackMatches(readU16(itemPtr + profile_.container.itemSubtype),expectedCount)) continue;
            out.ptr = itemPtr;
            out.index = static_cast<std::uint32_t>(i);
            out.subtype = readU16(itemPtr + profile_.container.itemSubtype);
            out.equipped = true;
            return true;
        }
        return false;
    }

    bool findUsableItemById(std::uint32_t wantedId, FoundItem& out, std::uint16_t expectedCount=0) const {
        if (findInventoryItemById(wantedId, out, expectedCount)) return true;
        return findOpenContainerItemById(wantedId, out, expectedCount);
    }

    bool findOpenContainerItemById(std::uint32_t wantedId, FoundItem& out, std::uint16_t expectedCount=0) const {
        out = {};
        if (!wantedId || wantedId > 0xFFFFu) return false;

        const auto game = gameObject();
        if (!game) return false;
        const auto& c = profile_.container;

        const auto mapObject = game + c.mapFromGame;
        const auto header = readPtr(mapObject + c.mapHeader);
        if (!header || !isReadableRange(header, 0x30)) return false;

        const auto root = readPtr(header + c.nodeParent);
        if (!root || root == header) return false;

        std::vector<std::uintptr_t> stack;
        std::vector<std::uintptr_t> seen;
        stack.push_back(root);

        while (!stack.empty() && seen.size() < 256) {
            const auto node = stack.back();
            stack.pop_back();
            if (!node || node == header || !isReadableRange(node, 0x30)) continue;

            bool duplicate = false;
            for (const auto value : seen) {
                if (value == node) { duplicate = true; break; }
            }
            if (duplicate) continue;
            seen.push_back(node);

            if (readU8(node + c.nodeIsNil) != 0) continue;

            const auto left = readPtr(node + c.nodeLeft);
            const auto right = readPtr(node + c.nodeRight);
            if (left && left != header) stack.push_back(left);
            if (right && right != header) stack.push_back(right);

            const auto container = readPtr(node + c.nodeContainer);
            if (!container || !isReadableRange(container, 0x80)) continue;

            const auto containerId = readU32(container + c.containerId);
            auto itemCount = readU32(container + c.itemCount);
            const auto dequeMap = readPtr(container + c.dequeMap);

            std::size_t dequeMapSize{};
            std::size_t dequeOffset{};
            if (!safeRead(container + c.dequeMapSize, dequeMapSize) ||
                !safeRead(container + c.dequeOffset, dequeOffset)) continue;

            if (!dequeMap || !dequeMapSize ||
                (dequeMapSize & (dequeMapSize - 1)) != 0 ||
                dequeMapSize > 0x10000) continue;

            if (itemCount > 100) itemCount = 100;
            for (std::uint32_t i = 0; i < itemCount; ++i) {
                const std::size_t logical = dequeOffset + i;
                const std::size_t block = (logical >> 1) & (dequeMapSize - 1);
                const std::size_t cell = logical & 1u;

                const auto blockPtrAddress = dequeMap + block * sizeof(std::uintptr_t);
                if (!isReadableRange(blockPtrAddress, sizeof(std::uintptr_t))) continue;
                const auto blockPtr = readPtr(blockPtrAddress);
                if (!blockPtr || !isReadableRange(blockPtr, 2 * sizeof(std::uintptr_t))) continue;

                const auto itemPtr = readPtr(blockPtr + cell * sizeof(std::uintptr_t));
                if (!itemPtr || !isReadableRange(itemPtr, 0x40)) continue;

                const auto id = readU16(itemPtr + c.itemId);
                if (id != static_cast<std::uint16_t>(wantedId)) continue;
                if (!coinStackMatches(readU16(itemPtr + c.itemSubtype),expectedCount)) continue;

                out.ptr = itemPtr;
                out.containerId = containerId;
                out.index = i;
                out.subtype = readU16(itemPtr + c.itemSubtype);
                return true;
            }
        }
        return false;
    }

    bool scanVisibleMonstersGameThread() {
        creatureScanResult_.clear();
        if (!validateRuntime(false) || !mapRuntimeReady() || !profile_.runtime.monsterVtable)
            return false;

        const auto player = localPlayer();
        if (!player) return false;
        const auto self = currentPosition();
        if (self.x <= 0 || self.y <= 0 || self.z < 0) return false;

        const auto monsterVtable = base_ + *profile_.runtime.monsterVtable;
        const auto emptySlot = base_ + *profile_.runtime.mapEmptySlot;
        const int range = std::clamp(pendingCreatureScanRange_, 0, 15);
        std::vector<std::uintptr_t> seen;
        seen.reserve(64);

        for (int dy = -range; dy <= range; ++dy) {
            for (int dx = -range; dx <= range; ++dx) {
                if (std::max(std::abs(dx), std::abs(dy)) > range) continue;
                const auto tx = static_cast<std::int64_t>(self.x) + dx;
                const auto ty = static_cast<std::int64_t>(self.y) + dy;
                if (tx <= 0 || ty <= 0) continue;

                NativePosition p{};
                p.x = static_cast<std::uint32_t>(tx);
                p.y = static_cast<std::uint32_t>(ty);
                p.z = static_cast<std::uint8_t>(self.z);

                const auto slot = getTileSlotGameThread(p);
                if (!slot || slot == emptySlot || !isReadableRange(slot, sizeof(std::uintptr_t))) continue;
                const auto tile = readPtr(slot);
                if (!tile || !isReadableRange(tile, profile_.map.tileThingsCapacity + sizeof(std::uintptr_t))) continue;
                const auto begin = readPtr(tile + profile_.map.tileThingsBegin);
                const auto end = readPtr(tile + profile_.map.tileThingsEnd);
                if (!begin || !end || end < begin || ((end - begin) % sizeof(std::uintptr_t)) != 0) continue;
                const auto bytes = static_cast<std::size_t>(end - begin);
                if (bytes > sizeof(std::uintptr_t) * 256) continue;
                if (bytes && !isReadableRange(begin, bytes)) continue;

                for (auto q = begin; q < end; q += sizeof(std::uintptr_t)) {
                    const auto thing = readPtr(q);
                    if (!thing || thing == player || !isReadableRange(thing, 0x70)) continue;
                    if (readPtr(thing) != monsterVtable) continue;
                    if (std::find(seen.begin(), seen.end(), thing) != seen.end()) continue;

                    CreatureSnapshot c{};
                    c.address = thing;
                    c.id = readU32(thing + profile_.creature.id);
                    c.position.x = static_cast<std::int32_t>(readU32(thing + profile_.creature.x));
                    c.position.y = static_cast<std::int32_t>(readU32(thing + profile_.creature.y));
                    c.position.z = static_cast<std::int32_t>(readU8(thing + profile_.creature.z));
                    c.hpPercent = readCreatureHp(thing);
                    c.direction = readCreatureDirection(thing);
                    c.alive = c.hpPercent > 0 && c.hpPercent <= 100;
                    if (profile_.creature.skull) c.skull = readU8(thing + *profile_.creature.skull);
                    if (profile_.creature.party) c.party = readU8(thing + *profile_.creature.party);
                    readNativeMsvcString(thing + profile_.creature.name, c.name);

                    if (!c.alive || c.position.z != self.z ||
                        self.chebyshevDistance(c.position) > range) continue;
                    seen.push_back(thing);
                    creatureScanResult_.push_back(std::move(c));
                }
            }
        }
        return true;
    }

    bool executePendingAttack() {
        if (!validateRuntime(false) || !profile_.actions.attack || !profile_.runtime.monsterVtable)
            return false;
        const auto game = gameObject();
        const auto target = pendingAttackCreature_;
        if (game && isReadableRange(game + 0x10, sizeof(std::uintptr_t)) && readPtr(game + 0x10) != 0) {
            std::cout << "\n[ATTACK] ABORT: FollowTarget manual ativo; nao disputar follow com Game::attack.\n";
            return false;
        }
        if (!game || !target || !isReadableRange(target, 0x70) ||
            readPtr(target) != base_ + *profile_.runtime.monsterVtable) {
            std::cout << "\n[ATTACK] ABORT: target invalido/removido.\n";
            return false;
        }

        ThingHandle handle{};
        handle.thing = target;
        handle.representation = reinterpret_cast<std::uintptr_t>(&handle.thing);
        std::string name;
        readNativeMsvcString(target + profile_.creature.name, name);
        const auto id = readU32(target + profile_.creature.id);
        const auto hp = readCreatureHp(target);
        const Position pos{
            static_cast<std::int32_t>(readU32(target + profile_.creature.x)),
            static_cast<std::int32_t>(readU32(target + profile_.creature.y)),
            static_cast<std::int32_t>(readU8(target + profile_.creature.z))
        };

        const auto attackTargetBefore = readPtr(game + 0x08);
        NativePosition livePosition{};
        const auto dispatch = olders_attack::dispatch(target, attackTargetBefore,
            pendingAttackId_, id, hp, findThingPositionGameThread(target, livePosition));
        if (dispatch == olders_attack::Dispatch::Reject) {
            std::cout << "[ATTACK] ABORT: creature identity/liveness/map membership changed before dispatch.\n" << std::flush;
            return false;
        }
        // Game::attack toggles off an already selected creature. A queued
        // request can become redundant after a manual click or config reload.
        if (dispatch == olders_attack::Dispatch::AlreadySelected) {
            std::cout << "[ATTACK] SKIP: target already selected; no native call/refcount change.\n" << std::flush;
            return true;
        }

        std::cout << "\n============================================================\n"
                  << "              CROME SDK - Game::attack(Monster)\n"
                  << "============================================================\n"
                  << std::hex << std::uppercase
                  << "Game object          : 0x" << game << "\n"
                  << "Function             : RVA 0x" << *profile_.actions.attack << "\n"
                  << "Monster*             : 0x" << target << "\n"
                  << "Monster ID           : 0x" << id << "\n"
                  << std::dec
                  << "Name                 : \"" << name << "\"\n"
                  << "Position             : " << pos.x << "," << pos.y << "," << pos.z << "\n"
                  << "HP                   : " << hp << "%\n"
                  << "ABI                  : RCX=Game*, RDX=ThingHandle*, R8B=false\n"
                  << "[CALL] game/window thread\n";

        const auto fn = reinterpret_cast<GameAttackFn>(base_ + *profile_.actions.attack);
        // The native caller acquires one intrusive reference for the argument.
        // Attack consumes it even when it returns early. Passing a borrowed
        // pointer here used to release the map's existing reference instead.
        auto* counter=reinterpret_cast<volatile LONG*>(target+olders_attack::counterOffset);
        if(!olders_attack::acquireConsumedReference(counter)){
            std::cout<<"[ATTACK] ABORT: reference counter invalid.\n"<<std::flush;
            return false;
        }
        std::cout<<"[ATTACK OWNERSHIP] acquired argument reference; native callee consumes it.\n"<<std::flush;
        fn(reinterpret_cast<void*>(game), &handle, false);
        // Do not decrement here: Game::attack already released its argument.
        const auto attackTargetAfter = readPtr(game + 0x08);
        std::cout << "[RETURN] Game::attack returned normally.\n"
                  << std::hex << std::uppercase
                  << "AttackTarget before  : 0x" << attackTargetBefore << "\n"
                  << "AttackTarget after   : 0x" << attackTargetAfter
                  << (attackTargetAfter == target ? "  <MATCH>\n" : "  <NO MATCH>\n")
                  << std::dec << std::flush;
        return true;
    }

    bool requestItemAction(PendingItemAction action,
                           std::uint32_t requestedItemId,
                           const FoundItem& found,
                           const Position* target = nullptr,
                           std::vector<std::uint16_t> groundIds = {},
                           int groundRange = 0) {
        return tryPublishItemRequest(itemActionPending_, [&] {
            pendingGroundIds_ = std::move(groundIds);
            pendingGroundRange_ = std::clamp(groundRange, 0, 15);
            pendingItemAction_ = action;
            pendingRequestedItemId_ = requestedItemId;
            pendingFoundItem_ = found;
            if (target) setPendingTarget(*target);

            if (!PostMessage(gameWindow_, kItemActionMessage, 0, 0)) {
                pendingItemAction_ = PendingItemAction::None;
                pendingGroundIds_.clear();
                std::cout << "\n[ITEM ACTION] PostMessage falhou. error=" << GetLastError() << "\n";
                return false;
            }
            return true;
        });
    }

    bool executePendingWalk() {
        if (manualInteractionActive()) {
            std::cout << "\n[WALK] ABORT: interacao manual recente; chase temporariamente suspenso.\n";
            return false;
        }
        if (!validateRuntime(false)) {
            std::cout << "\n[WALK] ABORT: runtime validation failed.\n";
            return false;
        }

        const auto player = localPlayer();
        const auto before = currentPosition();
        auto destination = pendingDestination_;
        const auto fn = reinterpret_cast<LocalPlayerAutoWalkFn>(
            base_ + *profile_.actions.walk);

        std::cout << "\n============================================================\n"
                  << "               CROME SDK - WALKTO(Position)\n"
                  << "============================================================\n"
                  << std::hex << std::uppercase
                  << "LocalPlayer          : 0x" << player << "\n"
                  << "Function             : RVA 0x" << *profile_.actions.walk << "\n"
                  << std::dec
                  << "Current              : " << before.x << ", " << before.y << ", " << before.z << "\n"
                  << "Destination          : " << destination.x << ", " << destination.y
                  << ", " << static_cast<unsigned>(destination.z) << "\n"
                  << "ABI                  : RCX=LocalPlayer, RDX=&Position(12), R8B=false\n"
                  << "[CALL] game/window thread\n";

        const bool result = fn(reinterpret_cast<void*>(player), &destination, false);
        std::cout << "[RETURN] bool=" << (result ? "true" : "false") << "\n";
        return result;
    }

    bool executePendingItemAction() {
        if (!validateRuntime(false)) {
            std::cout << "\n[ITEM ACTION] ABORT: runtime validation failed.\n";
            return false;
        }
        const auto game = gameObject();
        if (!game) {
            std::cout << "\n[ITEM ACTION] ABORT: Game pointer invalido.\n";
            return false;
        }

        if (pendingItemAction_ == PendingItemAction::InspectTile)
            return dumpTileGameThread(pendingTargetPosition_);

        if (pendingItemAction_ == PendingItemAction::ChangeGold) {
            if (!profile_.actions.use || manualInteractionActive()) return false;
            for (const auto id : pendingGroundIds_) {
                FoundItem coin{};
                if (!findUsableItemById(id,coin,100)) continue;
                ThingHandle handle{}; handle.thing=coin.ptr;
                handle.representation=reinterpret_cast<std::uintptr_t>(&handle.thing);
                std::cout << "[CHANGE GOLD] use id=" << id << " count=100\n" << std::flush;
                const auto fn=reinterpret_cast<GameUseFn>(base_+*profile_.actions.use);
                fn(reinterpret_cast<void*>(game),&handle);
                std::cout << "[CHANGE GOLD] Game::use returned; waiting for server update.\n" << std::flush;
                return true;
            }
            return false;
        }

        if (pendingItemAction_ == PendingItemAction::UseGroundObject) {
            if (!profile_.actions.use) return false;
            std::uintptr_t slot{}, tile{}, ground{};
            std::size_t count{};
            if (!resolveTileGroundGameThread(pendingTargetPosition_, slot, tile, ground, count)) {
                std::cout << "\n[LADDER/USE] target tile nao resolvido.\n";
                return false;
            }
            const auto begin = readPtr(tile + profile_.map.tileThingsBegin);
            std::uintptr_t targetThing{};
            for (std::size_t i=0;i<count && i<64;++i) {
                const auto thing = readPtr(begin + i*sizeof(std::uintptr_t));
                if (!thing || !isReadableRange(thing, 0x40)) continue;
                const auto id = static_cast<std::uint32_t>(readU16(thing + profile_.container.itemId));
                if (id == pendingRequestedItemId_) { targetThing = thing; break; }
            }
            if (!targetThing) {
                std::cout << "\n[LADDER/USE] item id=" << pendingRequestedItemId_ << " nao encontrado no tile.\n";
                return false;
            }
            ThingHandle handle{};
            handle.thing = targetThing;
            handle.representation = reinterpret_cast<std::uintptr_t>(&handle.thing);
            const auto game = gameObject();
            const auto fn = reinterpret_cast<GameUseFn>(base_ + *profile_.actions.use);
            std::cout << "\n[LADDER/USE] Game::use item=" << pendingRequestedItemId_
                      << " target={" << pendingTargetPosition_.x << "," << pendingTargetPosition_.y
                      << "," << static_cast<unsigned>(pendingTargetPosition_.z) << "}\n";
            fn(reinterpret_cast<void*>(game), &handle);
            std::cout << "[LADDER/USE] returned normally.\n";
            return true;
        }

        if (!pendingFoundItem_.ptr || !isReadableRange(pendingFoundItem_.ptr, 0x40)) {
            std::cout << "\n[ITEM ACTION] ABORT: Item pointer invalido.\n";
            return false;
        }

        ThingHandle source{};
        source.thing = pendingFoundItem_.ptr;
        source.representation = reinterpret_cast<std::uintptr_t>(&source.thing);

        if (pendingItemAction_ == PendingItemAction::Use) {
            if (!profile_.actions.use) return false;
            std::cout << "\n============================================================\n"
                      << "                  CROME SDK - Game::use\n"
                      << "============================================================\n"
                      << std::hex << std::uppercase
                      << "Game object          : 0x" << game << "\n"
                      << "Function             : RVA 0x" << *profile_.actions.use << "\n"
                      << "Item*                : 0x" << pendingFoundItem_.ptr << "\n"
                      << std::dec
                      << "Item ID              : " << pendingRequestedItemId_ << "\n"
                      << "Count/SubType        : " << pendingFoundItem_.subtype << "\n"
                      << "Source               : {65535,"
                      << (pendingFoundItem_.containerId | 0x40u) << ","
                      << pendingFoundItem_.index << "}\n"
                      << "ABI                  : RCX=Game*, RDX=ThingHandle*\n"
                      << "[CALL] game/window thread\n";

            const auto fn = reinterpret_cast<GameUseFn>(base_ + *profile_.actions.use);
            fn(reinterpret_cast<void*>(game), &source);
            std::cout << "[RETURN] Game::use returned normally.\n";
            return true;
        }

        if (pendingItemAction_ == PendingItemAction::UseWithSelf) {
            if (!profile_.actions.useWith) return false;
            const auto player = localPlayer();
            if (!player || !isReadableRange(player, 0x100)) {
                std::cout << "\n[USEWITH] LocalPlayer invalido.\n";
                return false;
            }

            ThingHandle target{};
            target.thing = player;
            target.representation = reinterpret_cast<std::uintptr_t>(&target.thing);
            const auto p = currentPosition();

            std::cout << "\n============================================================\n"
                      << "             CROME SDK - Game::useWith(Self)\n"
                      << "============================================================\n"
                      << std::hex << std::uppercase
                      << "Game object          : 0x" << game << "\n"
                      << "Function             : RVA 0x" << *profile_.actions.useWith << "\n"
                      << "Source Item*         : 0x" << pendingFoundItem_.ptr << "\n"
                      << "LocalPlayer*         : 0x" << player << "\n"
                      << std::dec
                      << "Source Item ID       : " << pendingRequestedItemId_ << "\n"
                      << "Count/SubType        : " << pendingFoundItem_.subtype << "\n"
                      << "Source               : {65535,"
                      << (pendingFoundItem_.containerId | 0x40u) << ","
                      << pendingFoundItem_.index << "}\n"
                      << "Target Position      : {" << p.x << "," << p.y << "," << p.z << "}\n"
                      << "Stack override       : 0\n"
                      << "ABI                  : RCX=Game*, RDX=source*, R8=target*, R9D=0\n"
                      << "[CALL] game/window thread\n";

            const auto fn = reinterpret_cast<GameUseWithFn>(base_ + *profile_.actions.useWith);
            fn(reinterpret_cast<void*>(game), &source, &target, 0);
            std::cout << "[RETURN] Game::useWith returned normally.\n";
            return true;
        }

        if (pendingItemAction_ == PendingItemAction::UseWithNearestGround) {
            if (!profile_.actions.useWith || !mapRuntimeReady()) return false;
            FoundItem refreshed{};
            if (!findUsableItemById(pendingRequestedItemId_, refreshed)) {
                std::cout << "[FISH] ABORT: rod no longer available.\n" << std::flush;
                return false;
            }
            pendingFoundItem_ = refreshed;
            source.thing = refreshed.ptr;
            NativePosition targetPos{};
            std::uintptr_t tile{}, groundThing{};
            std::uint16_t targetId{};
            if (!findNearestGroundByIdGameThread(targetPos, tile, groundThing, targetId)) {
                std::cout << "\n[AUTO-GROUND] Nenhum ground ID configurado encontrado em range="
                          << pendingGroundRange_ << " no andar atual.\n";
                return false;
            }

            ThingHandle target{};
            target.thing = groundThing;
            target.representation = reinterpret_cast<std::uintptr_t>(&target.thing);
            const auto targetStack = readU16(groundThing + profile_.container.itemSubtype);

            std::cout << "\n============================================================\n"
                      << "       CROME SDK - AUTO FIND + Game::useWith(Ground)\n"
                      << "============================================================\n"
                      << std::hex << std::uppercase
                      << "Game object          : 0x" << game << "\n"
                      << "Function             : RVA 0x" << *profile_.actions.useWith << "\n"
                      << "Source Item*         : 0x" << pendingFoundItem_.ptr << "\n"
                      << "Target Tile*         : 0x" << tile << "\n"
                      << "Target Ground Thing* : 0x" << groundThing << "\n"
                      << std::dec
                      << "Source Item ID       : " << pendingRequestedItemId_ << "\n"
                      << "Search range         : " << pendingGroundRange_ << "\n"
                      << "Matched Position     : {" << targetPos.x << "," << targetPos.y << ","
                      << static_cast<unsigned>(targetPos.z) << "}\n"
                      << "Matched ground ID    : " << targetId << "\n"
                      << "Target raw subtype   : " << targetStack << "\n"
                      << "ABI                  : RCX=Game*, RDX=source*, R8=ground Thing*, R9D=0\n"
                      << "[CALL] game/window thread\n";

            const auto fn = reinterpret_cast<GameUseWithFn>(base_ + *profile_.actions.useWith);
            fn(reinterpret_cast<void*>(game), &source, &target, 0);
            std::cout << "[RETURN] Game::useWith returned normally.\n";
            return true;
        }

        if (pendingItemAction_ == PendingItemAction::UseWithGround) {
            if (!profile_.actions.useWith || !mapRuntimeReady()) return false;
            std::uintptr_t slot{}, tile{}, groundThing{};
            std::size_t count{};
            if (!resolveTileGroundGameThread(pendingTargetPosition_, slot, tile, groundThing, count)) {
                std::cout << "\n[USEWITH-GROUND] Nao foi possivel resolver o ground Thing em {"
                          << pendingTargetPosition_.x << "," << pendingTargetPosition_.y << ","
                          << static_cast<unsigned>(pendingTargetPosition_.z) << "}.\n";
                return false;
            }

            ThingHandle target{};
            target.thing = groundThing;
            target.representation = reinterpret_cast<std::uintptr_t>(&target.thing);
            const auto targetId = readU16(groundThing + profile_.container.itemId);
            const auto targetStack = readU16(groundThing + profile_.container.itemSubtype);

            std::cout << "\n============================================================\n"
                      << "          CROME SDK - Game::useWith(Ground Thing)\n"
                      << "============================================================\n"
                      << std::hex << std::uppercase
                      << "Game object          : 0x" << game << "\n"
                      << "Function             : RVA 0x" << *profile_.actions.useWith << "\n"
                      << "Source Item*         : 0x" << pendingFoundItem_.ptr << "\n"
                      << "Target Tile*         : 0x" << tile << "\n"
                      << "Target Ground Thing* : 0x" << groundThing << "\n"
                      << std::dec
                      << "Source Item ID       : " << pendingRequestedItemId_ << "\n"
                      << "Count/SubType        : " << pendingFoundItem_.subtype << "\n"
                      << "Source               : {65535,"
                      << (pendingFoundItem_.containerId | 0x40u) << ","
                      << pendingFoundItem_.index << "}\n"
                      << "Target Position      : {" << pendingTargetPosition_.x << ","
                      << pendingTargetPosition_.y << ","
                      << static_cast<unsigned>(pendingTargetPosition_.z) << "}\n"
                      << "Target ground ID     : " << targetId << "\n"
                      << "Target raw subtype   : " << targetStack << "\n"
                      << "Thing count on tile  : " << count << "\n"
                      << "Stack override       : 0\n"
                      << "ABI                  : RCX=Game*, RDX=source*, R8=ground Thing*, R9D=0\n"
                      << "[CALL] game/window thread\n";

            const auto fn = reinterpret_cast<GameUseWithFn>(base_ + *profile_.actions.useWith);
            fn(reinterpret_cast<void*>(game), &source, &target, 0);
            std::cout << "[RETURN] Game::useWith returned normally.\n";
            return true;
        }

        return false;
    }

    bool executePendingTalk() {
        if (!validateRuntime(false) || !profile_.actions.sayDefault) {
            std::cout << "\n[SAY] ABORT: runtime/Game::talk invalido.\n";
            return false;
        }
        const auto game = gameObject();
        if (!game) return false;

        NativeString32 native{};
        native.capacity = 15;
        native.size = pendingTalkLength_;
        if (native.size > 15) return false;
        std::memcpy(native.storage, pendingTalkText_, native.size);
        native.storage[native.size] = '\0';

        const auto fn = reinterpret_cast<GameTalkFn>(
            base_ + *profile_.actions.sayDefault);

        std::cout << "\n============================================================\n"
                  << "               CROME SDK - Game::talk(Default)\n"
                  << "============================================================\n"
                  << std::hex << std::uppercase
                  << "Game object          : 0x" << game << "\n"
                  << "Function             : RVA 0x" << *profile_.actions.sayDefault << "\n"
                  << std::dec
                  << "Text                 : \"" << native.storage << "\"\n"
                  << "Native string size   : " << native.size << "\n"
                  << "Native string cap    : " << native.capacity << " (SSO)\n"
                  << "ABI                  : RCX=Game*, RDX=&NativeString32\n"
                  << "Flow                 : Game::talk -> talkChannel -> sendTalk -> opcode 0x96\n"
                  << "[CALL] game/window thread\n";

        fn(reinterpret_cast<void*>(game), &native);
        std::cout << "[RETURN] Game::talk returned normally.\n";
        return true;
    }

#include "OldersLootActions.inl"

    static LRESULT CALLBACK hookedWndProc(HWND hwnd, UINT message,
                                          WPARAM wParam, LPARAM lParam) {
        auto* self = active_;
        if(self && message==kLootMessage){self->lootResult_=self->executeLootStep();return 0;}
        if (self && isManualMouseMessage(message)) {
            self->noteManualInteraction(message);
        }
        if (self && message == kWalkMessage) {
            self->executePendingWalk();
            InterlockedExchange(&self->walkPending_, 0);
            return 0;
        }
        if (self && message == kItemActionMessage) {
            self->executePendingItemAction();
            self->pendingItemAction_ = PendingItemAction::None;
            self->pendingGroundIds_.clear();
            InterlockedExchange(&self->itemActionPending_, 0);
            return 0;
        }
        if (self && message == kTalkMessage) {
            self->executePendingTalk();
            InterlockedExchange(&self->talkPending_, 0);
            return 0;
        }
        if (self && message == kCreatureScanMessage) {
            self->scanVisibleMonstersGameThread();
            return 0;
        }
        if (self && message == kAttackMessage) {
            self->executePendingAttack();
            self->pendingAttackCreature_ = 0;
            InterlockedExchange(&self->attackPending_, 0);
            return 0;
        }
        if (self && message == kFollowBlockMessage) {
            self->applyFollowBlockOnGameThread();
            return 0;
        }
        if (self && self->originalWndProc_)
            return CallWindowProc(self->originalWndProc_, hwnd, message, wParam, lParam);
        return DefWindowProc(hwnd, message, wParam, lParam);
    }
};

} // namespace crome
