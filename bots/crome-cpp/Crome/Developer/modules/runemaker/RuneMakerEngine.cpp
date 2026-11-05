#include "modules/runemaker/RuneMakerEngine.hpp"
#include <chrono>
namespace crome { void RuneMakerEngine::tick(const GameState& s,CromeApi& api,ActionScheduler& q,CooldownManager& cd){if(!cfg_.enabled||!s.player.valid||cfg_.spell.empty())return; if(s.player.manaPercent()<cfg_.minManaPercent||s.player.soul<cfg_.minSoul)return; if(!cd.ready("runemaker",std::chrono::milliseconds(1200)))return; auto spell=cfg_.spell;q.submit({40,"runemaker",[&api,&cd,spell]{api.actions().say(spell);cd.mark("runemaker");}}); } }
