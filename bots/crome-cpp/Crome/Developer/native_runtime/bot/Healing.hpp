#pragma once
#include "../actions/IActions.hpp"
#include "../profiles/MiniJson.hpp"
#include "../sdk/Conditions.hpp"
#include "../sdk/Snapshots.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace crome {

enum class HealingTrigger {
    HealthPercent,
    ManaPercent,
    MissingManaShield,
    Paralyzed,
    MissingHaste,
};

enum class HealingActionKind { Spell, ItemOnSelf };

struct HealingRule {
    std::string name;
    bool enabled{true};
    HealingTrigger trigger{HealingTrigger::HealthPercent};
    double belowOrEqual{100.0};
    std::uint64_t minMana{};
    HealingActionKind action{HealingActionKind::Spell};
    std::string spell;
    Item item{};
    std::uint32_t cooldownMs{900};
    int priority{};
};

struct HealingConfig {
    bool enabled{false};
    std::uint32_t pollMs{100};
    std::vector<HealingRule> rules;
};

struct HealingExecution {
    std::size_t ruleIndex{};
    std::string name;
    std::string spell;
    double healthPercent{};
    double mana{};
};

inline std::string readHealingText(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open healing config: " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

inline double healingJsonNumber(const json::Value* v, double fallback) {
    return v && v->isNumber() ? v->number() : fallback;
}

inline std::uint32_t healingJsonU32(const json::Value* v, std::uint32_t fallback) {
    if (!v || !v->isNumber() || v->number() < 0) return fallback;
    return static_cast<std::uint32_t>(v->number());
}

inline int healingJsonInt(const json::Value* v, int fallback) {
    return v && v->isNumber() ? static_cast<int>(v->number()) : fallback;
}

inline HealingTrigger parseHealingTrigger(const std::string& s) {
    if (s == "health_percent") return HealingTrigger::HealthPercent;
    if (s == "mana_percent") return HealingTrigger::ManaPercent;
    if (s == "missing_mana_shield") return HealingTrigger::MissingManaShield;
    if (s == "paralyzed") return HealingTrigger::Paralyzed;
    if (s == "missing_haste") return HealingTrigger::MissingHaste;
    throw std::runtime_error("unsupported healing trigger: " + s);
}

inline HealingConfig loadHealingConfig(const std::string& path) {
    const auto root = json::parse(readHealingText(path));
    if (!root.isObject()) throw std::runtime_error("healing root must be object");

    HealingConfig out;
    if (const auto* v = root.get("enabled"); v && v->isBool()) out.enabled = v->boolean();
    out.pollMs = healingJsonU32(root.get("poll_ms"), out.pollMs);
    if (out.pollMs < 20) out.pollMs = 20;

    const auto* rules = root.get("rules");
    if (!rules || !rules->isArray()) throw std::runtime_error("healing needs rules array");

    for (const auto& value : rules->array()) {
        if (!value.isObject()) continue;
        HealingRule r;
        if (const auto* v = value.get("name"); v && v->isString()) r.name = v->string();
        if (const auto* v = value.get("enabled"); v && v->isBool()) r.enabled = v->boolean();
        if (const auto* v = value.get("trigger"); v && v->isString())
            r.trigger = parseHealingTrigger(v->string());
        r.belowOrEqual = healingJsonNumber(value.get("below_or_equal"), r.belowOrEqual);
        r.minMana = healingJsonU32(value.get("min_mana"), static_cast<std::uint32_t>(r.minMana));
        r.cooldownMs = healingJsonU32(value.get("cooldown_ms"), r.cooldownMs);
        r.priority = healingJsonInt(value.get("priority"), r.priority);
        if (r.cooldownMs < 50) r.cooldownMs = 50;

        std::string action = "spell";
        if (const auto* v = value.get("action"); v && v->isString()) action = v->string();
        if (action == "spell") {
            r.action = HealingActionKind::Spell;
            if (const auto* v = value.get("spell"); v && v->isString()) r.spell = v->string();
            if (r.spell.empty()) continue;
        } else if (action == "item_on_self") {
            r.action = HealingActionKind::ItemOnSelf;
            const auto id = healingJsonU32(value.get("item_id"), 0);
            if (!id) continue;
            r.item = Item{id, 0, 1};
        } else {
            continue;
        }

        if (r.name.empty()) r.name = r.spell;
        out.rules.push_back(std::move(r));
    }
    return out;
}

class HealingController {
public:
    explicit HealingController(IActions& actions) : actions_(actions) {}

    void resetCooldowns() { nextAllowedMs_.clear(); }

    std::optional<HealingExecution> tick(const LocalPlayerSnapshot& self,
                                         const std::vector<HealingRule>& rules,
                                         std::uint64_t nowMs,
                                         Position selfPosition = {}) {
        if (nextAllowedMs_.size() != rules.size())
            nextAllowedMs_.assign(rules.size(), 0);

        std::optional<std::size_t> bestIndex;
        for (std::size_t i = 0; i < rules.size(); ++i) {
            const auto& r = rules[i];
            if (!r.enabled || !matches(self, r)) continue;
            if (!bestIndex || r.priority > rules[*bestIndex].priority)
                bestIndex = i;
        }
        if (!bestIndex) return std::nullopt;

        const auto i = *bestIndex;
        const auto& best = rules[i];
        if (nowMs < nextAllowedMs_[i]) return std::nullopt;

        bool accepted = false;
        if (best.action == HealingActionKind::Spell) {
            accepted = !best.spell.empty() && actions_.castSpell(best.spell);
        } else if (best.action == HealingActionKind::ItemOnSelf) {
            accepted = best.item.id != 0 && actions_.useWith(best.item, selfPosition);
        }

        if (!accepted) return std::nullopt;
        nextAllowedMs_[i] = nowMs + best.cooldownMs;
        return HealingExecution{
            .ruleIndex = i,
            .name = best.name,
            .spell = best.spell,
            .healthPercent = self.healthPercent(),
            .mana = self.mana,
        };
    }

    // Compatibility helper for older unit tests/callers.
    bool tick(const LocalPlayerSnapshot& self, const std::vector<HealingRule>& rules) {
        return tick(self, rules, 0).has_value();
    }

private:
    bool matches(const LocalPlayerSnapshot& s, const HealingRule& r) const {
        if (s.mana < static_cast<double>(r.minMana)) return false;
        switch (r.trigger) {
            case HealingTrigger::HealthPercent:
                return s.healthPercent() <= r.belowOrEqual;
            case HealingTrigger::ManaPercent:
                return s.manaPercent() <= r.belowOrEqual;
            case HealingTrigger::MissingManaShield:
                return !hasCondition(s.states, Condition::ManaShield);
            case HealingTrigger::Paralyzed:
                return hasCondition(s.states, Condition::Paralyzed);
            case HealingTrigger::MissingHaste:
                return !hasCondition(s.states, Condition::Haste);
        }
        return false;
    }

    IActions& actions_;
    std::vector<std::uint64_t> nextAllowedMs_;
};

} // namespace crome
