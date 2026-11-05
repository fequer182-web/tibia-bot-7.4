#pragma once
#include "../profiles/MiniJson.hpp"
#include <algorithm>
#include <cstdint>
#include <vector>
namespace crome {
struct ChangeGoldConfig {
    bool enabled{};
    std::uint32_t intervalMs{1000};
    std::vector<std::uint16_t> itemIds;
};
inline ChangeGoldConfig parseChangeGoldConfig(const json::Value* value) {
    ChangeGoldConfig out;
    if(!value || !value->isObject())return out;
    if(const auto* v=value->get("enabled");v&&v->isBool())out.enabled=v->boolean();
    if(const auto* v=value->get("interval_ms");v&&v->isNumber()&&v->number()>=500&&v->number()<=60000)
        out.intervalMs=static_cast<std::uint32_t>(v->number());
    if(const auto* ids=value->get("item_ids");ids&&ids->isArray()) for(const auto& v:ids->array()) {
        if(!v.isNumber() || v.number()<1 || v.number()>65535 || v.number()!=static_cast<int>(v.number()))continue;
        const auto id=static_cast<std::uint16_t>(v.number());
        if(std::find(out.itemIds.begin(),out.itemIds.end(),id)==out.itemIds.end())out.itemIds.push_back(id);
        if(out.itemIds.size()==16)break;
    }
    if(out.itemIds.empty())out.enabled=false;
    return out;
}
inline bool coinStackMatches(std::uint16_t count,std::uint16_t expected) {
    return expected==0 || count==expected;
}
}
