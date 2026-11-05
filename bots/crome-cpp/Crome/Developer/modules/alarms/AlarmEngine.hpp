#pragma once
#include "core/sdk/Snapshots.hpp"
namespace crome { struct AlarmConfig { bool lowHealth{}, noManaRegen{}, playerOnScreen{}, disconnect{}, walkerStuck{}; double lowHealthPercent{30}; }; class AlarmEngine { AlarmConfig cfg_; public: AlarmConfig& config(){return cfg_;} void tick(const GameState&){} }; }
