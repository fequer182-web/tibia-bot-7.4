#pragma once
#include "app/ActionScheduler.hpp"
#include "app/CooldownManager.hpp"
#include "api/CromeApi.hpp"
#include "modules/ModuleConfig.hpp"
namespace crome { class SupportEngine { SupportConfig cfg_; public: SupportConfig& config(){return cfg_;} void tick(const GameState&,CromeApi&,ActionScheduler&,CooldownManager&); }; }
