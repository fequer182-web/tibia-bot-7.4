#include "modules/looter/LooterEngine.hpp"
namespace crome { void LooterEngine::tick(const GameState& s,CromeApi&,ActionScheduler&){ if(!cfg_.enabled)return; if(cfg_.onlyWhenClear){for(const auto& c:s.creatures)if(c.valid&&c.alive)return;} /* CorpseTracker/MoveItem backend se conecta aqui. */ } }
