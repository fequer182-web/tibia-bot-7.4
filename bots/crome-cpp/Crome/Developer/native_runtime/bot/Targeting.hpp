#pragma once
#include "../profiles/MiniJson.hpp"
#include "../sdk/Snapshots.hpp"
#include "TargetPositioning.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace crome {

// Compatibility rule retained for existing callers/tests.
struct TargetRule {
    std::vector<std::string> names;
    bool anyMonster{false};
    std::uint32_t hpStart{0};
    std::uint32_t hpEnd{100};
    int maxRange{7};
    bool sameFloor{true};
};

class TargetSelector {
public:
    std::optional<CreatureSnapshot> select(const Position& self,
                                           const std::vector<CreatureSnapshot>& creatures,
                                           const TargetRule& rule) const {
        const CreatureSnapshot* best = nullptr;
        int bestDistance = 9999;
        int bestPriority = 9999;
        for (const auto& c : creatures) {
            if (!c.alive || c.hpPercent < rule.hpStart || c.hpPercent > rule.hpEnd) continue;
            if (rule.sameFloor && c.position.z != self.z) continue;
            const int d = self.chebyshevDistance(c.position);
            if (d > rule.maxRange) continue;
            int p = namePriority(c.name, rule);
            if (p < 0) continue;
            if (!best || p < bestPriority || (p == bestPriority && d < bestDistance)) {
                best = &c; bestPriority = p; bestDistance = d;
            }
        }
        if (!best) return std::nullopt;
        return *best;
    }

private:
    static std::string lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch){ return static_cast<char>(std::tolower(ch)); });
        return s;
    }
    static int namePriority(const std::string& name, const TargetRule& rule) {
        if (rule.anyMonster) return 1000;
        const auto n = lower(name);
        for (std::size_t i = 0; i < rule.names.size(); ++i) {
            auto wanted = lower(rule.names[i]);
            if (!wanted.empty() && wanted.back() == '~') {
                wanted.pop_back();
                if (n.rfind(wanted, 0) == 0) return static_cast<int>(i);
            } else if (n == wanted) return static_cast<int>(i);
        }
        return -1;
    }
};

struct TargetingEntry {
    std::string name;
    int priority{100};
};

struct TargetingConfig {
    bool enabled{false};
    std::uint32_t pollMs{300};
    bool attackAnyMonster{true};
    bool pauseWalker{true};
    bool chaseEnabled{true};
    int movementMode{-1}; // Missing key preserves legacy chase settings.
    int chaseDistance{1};
    int chaseScanRange{15};
    std::uint32_t chaseIntervalMs{350};
    std::uint32_t chaseRepathMs{900};
    std::uint32_t chaseForceRefreshMs{1800};
    std::uint32_t manualInputPauseMs{1500};
    std::uint32_t hpMin{1};
    std::uint32_t hpMax{100};
    int maxRange{7};
    std::vector<TargetingEntry> targets;
};

struct TargetingUpdate {
    bool hasTarget{false};
    bool acquired{false};
    bool lost{false};
    CreatureSnapshot target{};
    int priority{0};
    int distance{0};
};

inline std::string readTargetingText(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open targeting config: " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

inline std::uint32_t targetingJsonU32(const json::Value* v, std::uint32_t fallback) {
    if (!v || !v->isNumber() || v->number() < 0) return fallback;
    return static_cast<std::uint32_t>(v->number());
}

inline int targetingJsonInt(const json::Value* v, int fallback) {
    return v && v->isNumber() ? static_cast<int>(v->number()) : fallback;
}

inline TargetingConfig loadTargetingConfig(const std::string& path) {
    const auto root = json::parse(readTargetingText(path));
    if (!root.isObject()) throw std::runtime_error("targeting root must be object");

    TargetingConfig out;
    if (const auto* v = root.get("enabled"); v && v->isBool()) out.enabled = v->boolean();
    if (const auto* v = root.get("attack_any_monster"); v && v->isBool()) out.attackAnyMonster = v->boolean();
    if (const auto* v = root.get("pause_walker"); v && v->isBool()) out.pauseWalker = v->boolean();
    if (const auto* v = root.get("chase_enabled"); v && v->isBool()) out.chaseEnabled = v->boolean();
    if (const auto* v = root.get("movement_mode"); v && v->isNumber()) {
        const auto mode = v->number();
        if (mode >= 0 && mode <= 7 && mode == static_cast<int>(mode)) {
            out.movementMode = static_cast<int>(mode);
            out.chaseEnabled = out.movementMode != 1;
        } else { out.movementMode = 1; out.chaseEnabled = false; }
    }
    out.chaseDistance = targetingJsonInt(root.get("chase_distance"), out.chaseDistance);
    out.chaseScanRange = targetingJsonInt(root.get("chase_scan_range"), out.chaseScanRange);
    out.chaseIntervalMs = targetingJsonU32(root.get("chase_interval_ms"), out.chaseIntervalMs);
    out.chaseRepathMs = targetingJsonU32(root.get("chase_repath_ms"), out.chaseRepathMs);
    out.chaseForceRefreshMs = targetingJsonU32(root.get("chase_force_refresh_ms"), out.chaseForceRefreshMs);
    out.manualInputPauseMs = targetingJsonU32(root.get("manual_input_pause_ms"), out.manualInputPauseMs);
    out.pollMs = targetingJsonU32(root.get("poll_ms"), out.pollMs);
    out.hpMin = targetingJsonU32(root.get("hp_min"), out.hpMin);
    out.hpMax = targetingJsonU32(root.get("hp_max"), out.hpMax);
    out.maxRange = targetingJsonInt(root.get("max_range"), out.maxRange);

    if (out.pollMs < 100) out.pollMs = 100;
    if (out.chaseIntervalMs < 150) out.chaseIntervalMs = 150;
    if (out.chaseRepathMs < 400) out.chaseRepathMs = 400;
    if (out.chaseForceRefreshMs < out.chaseRepathMs) out.chaseForceRefreshMs = out.chaseRepathMs;
    if (out.chaseForceRefreshMs > 5000) out.chaseForceRefreshMs = 5000;
    if (out.manualInputPauseMs < 250) out.manualInputPauseMs = 250;
    if (out.manualInputPauseMs > 5000) out.manualInputPauseMs = 5000;
    if (out.chaseDistance < 1) out.chaseDistance = 1;
    if (out.chaseDistance > 4) out.chaseDistance = 4;
    if (out.movementMode >= 0) out.chaseDistance = targetModeDistance(out.movementMode);
    if (out.chaseScanRange < out.maxRange) out.chaseScanRange = out.maxRange;
    if (out.chaseScanRange < 1) out.chaseScanRange = 1;
    if (out.chaseScanRange > 15) out.chaseScanRange = 15;
    if (out.maxRange < 0) out.maxRange = 0;
    if (out.maxRange > 15) out.maxRange = 15;
    if (out.hpMin > 100) out.hpMin = 100;
    if (out.hpMax > 100) out.hpMax = 100;
    if (out.hpMin > out.hpMax) std::swap(out.hpMin, out.hpMax);

    if (const auto* arr = root.get("targets"); arr && arr->isArray()) {
        for (const auto& value : arr->array()) {
            if (!value.isObject()) continue;
            TargetingEntry e;
            if (const auto* v = value.get("name"); v && v->isString()) e.name = v->string();
            e.priority = targetingJsonInt(value.get("priority"), e.priority);
            if (!e.name.empty()) out.targets.push_back(std::move(e));
        }
    }
    std::stable_sort(out.targets.begin(), out.targets.end(),
        [](const TargetingEntry& a, const TargetingEntry& b){ return a.priority < b.priority; });
    return out;
}

class TargetingController {
public:
    void reset() noexcept {
        lockedAddress_ = 0;
        lockedId_ = 0;
    }

    [[nodiscard]] bool hasLock() const noexcept { return lockedAddress_ != 0; }
    [[nodiscard]] std::uintptr_t lockedAddress() const noexcept { return lockedAddress_; }
    [[nodiscard]] std::uint32_t lockedId() const noexcept { return lockedId_; }

    TargetingUpdate tick(const Position& self,
                         const std::vector<CreatureSnapshot>& monsters,
                         const TargetingConfig& config) {
        TargetingUpdate update{};

        if (lockedAddress_) {
            const auto it = std::find_if(monsters.begin(), monsters.end(), [&](const CreatureSnapshot& c){
                return c.address == lockedAddress_ && c.id == lockedId_;
            });
            if (it != monsters.end()) {
                // Sticky combat lock: maxRange is an ACQUISITION filter, not a
                // reason to switch targets mid-fight. Keep the current monster
                // while it is alive, on our floor and still allowed by config.
                const auto prio = namePriorityForConfig(it->name, config);
                if (it->alive && it->hpPercent >= config.hpMin && it->hpPercent <= config.hpMax &&
                    it->position.z == self.z && prio.has_value()) {
                    update.hasTarget = true;
                    update.target = *it;
                    update.priority = *prio;
                    update.distance = self.chebyshevDistance(it->position);
                    return update;
                }
            }
            update.lost = true;
            reset();
        }

        const CreatureSnapshot* best = nullptr;
        int bestPriority = 999999;
        int bestDistance = 999999;
        for (const auto& c : monsters) {
            const auto p = matches(self, c, config);
            if (!p) continue;
            const int distance = self.chebyshevDistance(c.position);
            if (!best || *p < bestPriority ||
                (*p == bestPriority && distance < bestDistance) ||
                (*p == bestPriority && distance == bestDistance && c.hpPercent < best->hpPercent) ||
                (*p == bestPriority && distance == bestDistance && c.hpPercent == best->hpPercent && c.id < best->id)) {
                best = &c;
                bestPriority = *p;
                bestDistance = distance;
            }
        }

        if (!best) return update;
        lockedAddress_ = best->address;
        lockedId_ = best->id;
        update.hasTarget = true;
        update.acquired = true;
        update.target = *best;
        update.priority = bestPriority;
        update.distance = bestDistance;
        return update;
    }

private:
    static std::string lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch){ return static_cast<char>(std::tolower(ch)); });
        return s;
    }

    static bool nameMatches(const std::string& actual, const std::string& wanted) {
        if (wanted == "*") return true;
        auto a = lower(actual);
        auto w = lower(wanted);
        if (!w.empty() && w.back() == '~') {
            w.pop_back();
            return !w.empty() && a.rfind(w, 0) == 0;
        }
        return a == w;
    }

    static std::optional<int> namePriorityForConfig(const std::string& name,
                                                     const TargetingConfig& config) {
        for (const auto& e : config.targets) {
            if (nameMatches(name, e.name)) return e.priority;
        }
        if (config.attackAnyMonster) return 1000;
        return std::nullopt;
    }

    static std::optional<int> matches(const Position& self,
                                      const CreatureSnapshot& c,
                                      const TargetingConfig& config) {
        if (!c.alive || c.hpPercent < config.hpMin || c.hpPercent > config.hpMax) return std::nullopt;
        if (c.position.z != self.z || self.chebyshevDistance(c.position) > config.maxRange) return std::nullopt;
        return namePriorityForConfig(c.name, config);
    }

    std::uintptr_t lockedAddress_{};
    std::uint32_t lockedId_{};
};

} // namespace crome
