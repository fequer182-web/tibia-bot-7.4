#pragma once
#include "../profiles/MiniJson.hpp"
#include "../sdk/Position.hpp"
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
namespace crome {
// Game-thread scope: nested dispatches cannot reuse snapshots from an action
// that has not returned. Release the reservation even on an exception.
class LootExecutionGuard {
    bool& flag_; bool owns_;
public:
    explicit LootExecutionGuard(bool& flag):flag_(flag),owns_(!flag){if(owns_)flag_=true;}
    ~LootExecutionGuard(){if(owns_)flag_=false;}
    LootExecutionGuard(const LootExecutionGuard&)=delete;
    LootExecutionGuard& operator=(const LootExecutionGuard&)=delete;
    explicit operator bool()const{return owns_;}
};
struct OldersLootConfig {
    bool enabled{};bool onlyList{true};int radius{3};unsigned destination{};
    std::vector<std::uint16_t> items,corpses,nested;
};
inline bool lootHas(const std::vector<std::uint16_t>& ids,unsigned id){return std::find(ids.begin(),ids.end(),id)!=ids.end();}
inline OldersLootConfig parseOldersLoot(const json::Value& root){
    OldersLootConfig out;if(!root.isObject())return out;
    if(const auto* v=root.get("enabled");v&&v->isBool())out.enabled=v->boolean();
    if(const auto* v=root.get("only_list");v&&v->isBool())out.onlyList=v->boolean();
    if(const auto* v=root.get("radius");v&&v->isNumber()&&std::isfinite(v->number()))out.radius=static_cast<int>(std::clamp(v->number(),1.0,7.0));
    if(const auto* v=root.get("target_container_id");v){
        if(!v->isNumber()||!std::isfinite(v->number())||v->number()<0||v->number()>63||v->number()!=static_cast<int>(v->number())){out.enabled=false;return out;}
        out.destination=static_cast<unsigned>(v->number());
    }
    auto ids=[&](const char* key,std::vector<std::uint16_t>& dst){
        if(const auto* arr=root.get(key);arr&&arr->isArray())for(const auto& v:arr->array()){
            if(!v.isNumber()||!std::isfinite(v.number())||v.number()<1||v.number()>65535||v.number()!=static_cast<int>(v.number()))continue;
            auto id=static_cast<std::uint16_t>(v.number());if(!lootHas(dst,id))dst.push_back(id);
            if(dst.size()>=256)break;
        }
    };
    ids("item_ids",out.items);ids("corpse_item_ids",out.corpses);ids("learned_corpse_ids",out.corpses);ids("nested_container_ids",out.nested);
    if(out.corpses.empty()||(out.onlyList&&out.items.empty()))out.enabled=false;
    return out;
}
// A native request is not confirmation; stop retrying unchanged snapshots.
struct LootRetryBudget {
    std::uintptr_t pointer{};unsigned count{},source{},slot{},destination{},attempts{};
    bool allow(std::uintptr_t p,unsigned n,unsigned s,unsigned i,unsigned d){
        if(p!=pointer||n!=count||s!=source||i!=slot||d!=destination){pointer=p;count=n;source=s;slot=i;destination=d;attempts=0;}
        return ++attempts<=3;
    }
};
struct OldersLootStep {bool active{};bool walk{};Position position{};std::string status;};
inline Position lootContainerDestination(unsigned container,unsigned slot){return {65535,static_cast<int>(0x40|container),static_cast<int>(slot)};}
}
