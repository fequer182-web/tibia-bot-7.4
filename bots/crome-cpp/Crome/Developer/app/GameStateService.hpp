#pragma once
#include "api/CromeApi.hpp"
namespace crome { class GameStateService { CromeApi& api_; public: explicit GameStateService(CromeApi& a):api_(a){} GameState snapshot(){ GameState s; s.player=api_.player(); s.creatures=api_.creatures(); s.containers=api_.containers(); return s; } }; }
