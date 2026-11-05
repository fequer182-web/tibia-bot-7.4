#pragma once
#include "core/sdk/Snapshots.hpp"
#include <cstdint>
#include <string>
#include <vector>
namespace crome {
struct HealRule {
    bool enabled{};
    double hpPercent{};
    double minMana{};
    std::string spell;
    int priority{90};
};
struct HealingConfig {
    bool enabled{};
    std::vector<HealRule> rules;

    bool uhEnabled{};
    double uhHpPercent{50};
    uint32_t uhItemId{2273};

    bool manaFluidEnabled{};
    double manaFluidPercent{50};
    uint32_t manaFluidItemId{};

    bool lifeFluidEnabled{};
    double lifeFluidHpPercent{50};
    uint32_t lifeFluidItemId{};
};
struct SupportConfig { bool enabled{}; bool manaShield{}, antiParalyze{}, haste{}, antiIdle{}, food{}; std::string manaShieldSpell{"utamo vita"}, antiParaSpell{"exura"}, hasteSpell{"utani hur"}; uint32_t foodId{}; };
struct RuneMakerConfig { bool enabled{}; double minManaPercent{90}; double minSoul{}; std::string spell; };
struct TargetRule { std::string name{"*"}; int priority{}; int minHp{}, maxHp{100}; int maxRange{7}; };
struct TargetingConfig { bool enabled{}; bool attackAny{}; bool keepFollow{}; bool shouldAttackMe{}; std::vector<TargetRule> rules; };
enum class WaypointType { Walk, Stand, FloorChange, Use, UseWith, Rope, Shovel, Ladder, Hole, Say, Wait, Label, Goto, Script };
struct Waypoint { WaypointType type{WaypointType::Walk}; Position position{}; uint32_t itemId{}; std::string text; int delayMs{}; };
struct CavebotConfig { bool enabled{}; bool loop{true}; std::vector<Waypoint> waypoints; };
struct LootRule { uint32_t itemId{}; int destinationContainer{}; };
struct LooterConfig { bool enabled{}; bool onlyWhenClear{true}; int maxDistance{3}; std::vector<LootRule> items; };
}
