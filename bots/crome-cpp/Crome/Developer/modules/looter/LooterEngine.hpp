#pragma once
#include "app/ActionScheduler.hpp"
#include "api/CromeApi.hpp"
#include "modules/ModuleConfig.hpp"
namespace crome { class LooterEngine { LooterConfig cfg_; public: LooterConfig& config(){return cfg_;} void tick(const GameState&,CromeApi&,ActionScheduler&); }; }
