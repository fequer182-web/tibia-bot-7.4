#pragma once
#include "app/ActionScheduler.hpp"
#include "app/CooldownManager.hpp"
#include "api/CromeApi.hpp"
#include "modules/ModuleConfig.hpp"
namespace crome { class HealingEngine { HealingConfig cfg_; public: HealingConfig& config(){return cfg_;} void tick(const GameState&,CromeApi&,ActionScheduler&,CooldownManager&); }; }
