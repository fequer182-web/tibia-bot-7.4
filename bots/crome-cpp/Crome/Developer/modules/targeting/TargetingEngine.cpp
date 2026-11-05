#include "modules/targeting/TargetingEngine.hpp"
#include <algorithm>
#include <cctype>
namespace crome {
static std::string lower(std::string s){std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){return (char)std::tolower(c);});return s;}
std::optional<CreatureSnapshot> TargetingEngine::select(const GameState& s)const{
    std::optional<CreatureSnapshot> best; int bestScore=-999999;
    for(const auto& c:s.creatures){
        if(!c.valid||!c.alive) continue;
        bool matched=cfg_.attackAny;
        int ruleOrder=9999; int minHp=0,maxHp=100,maxRange=7;
        if(!cfg_.attackAny){
            for(size_t i=0;i<cfg_.rules.size();++i){
                const auto& r=cfg_.rules[i];
                if(r.name=="*" || lower(r.name)==lower(c.name)){
                    matched=true; ruleOrder=(int)i; minHp=r.minHp; maxHp=r.maxHp; maxRange=r.maxRange; break;
                }
            }
        }
        if(!matched) continue;
        if(c.hpPercent<minHp||c.hpPercent>maxHp||c.distance>maxRange) continue;
        // Listed names have left-to-right priority. Distance breaks ties.
        int score=(cfg_.attackAny?0:(10000-ruleOrder*100))-c.distance;
        if(score>bestScore){best=c;bestScore=score;}
    }
    return best;
}
void TargetingEngine::tick(const GameState& s,CromeApi& api,ActionScheduler& q){
    if(!cfg_.enabled)return; auto t=select(s); if(!t)return; auto id=t->id; current_=id;
    q.submit({70,"targeting",[&api,id]{api.actions().attack(id);}});
    if(cfg_.keepFollow)q.submit({65,"targeting-follow",[&api,id]{api.actions().follow(id);}});
}
}
