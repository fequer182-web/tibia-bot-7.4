#pragma once
#include "app/ActionScheduler.hpp"
#include "api/CromeApi.hpp"
#include "modules/ModuleConfig.hpp"
#include <cstddef>
namespace crome { class CavebotEngine { CavebotConfig cfg_; size_t index_{}; public: CavebotConfig& config(){return cfg_;} size_t index()const{return index_;} void reset(){index_=0;} void tick(const GameState&,CromeApi&,ActionScheduler&); }; }
