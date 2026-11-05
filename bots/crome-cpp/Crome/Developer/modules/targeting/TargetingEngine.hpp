#pragma once
#include "app/ActionScheduler.hpp"
#include "api/CromeApi.hpp"
#include "modules/ModuleConfig.hpp"
#include <optional>
namespace crome { class TargetingEngine { TargetingConfig cfg_; uint32_t current_{}; public: TargetingConfig& config(){return cfg_;} std::optional<CreatureSnapshot> select(const GameState&)const; void tick(const GameState&,CromeApi&,ActionScheduler&); }; }
