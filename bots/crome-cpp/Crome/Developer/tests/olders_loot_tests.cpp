#include "../native_runtime/bot/OldersLoot.hpp"
#include <iostream>
#include <stdexcept>
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
int main(){try {
    bool executing=false;
    try {
        crome::LootExecutionGuard first(executing);
        check(bool(first)&&executing,"loot action not reserved");
        {crome::LootExecutionGuard nested(executing);check(!nested,"reentrant loot admitted");}
        check(executing,"nested rejection released outer reservation");
        throw std::runtime_error("simulated native action failure");
    } catch(const std::runtime_error& error) {
        if(std::string(error.what())!="simulated native action failure")throw;
    }
    check(!executing,"exception stranded loot reservation");
    {crome::LootExecutionGuard next(executing);check(bool(next),"later loot blocked after unwind");}
    auto config=crome::parseOldersLoot(crome::json::parse(R"({"enabled":true,"item_ids":[3031,3031,0,-1,65536,1.5],"corpse_item_ids":[2813],"learned_corpse_ids":[2813,3000],"nested_container_ids":[2854],"radius":999999999999999999999,"target_container_id":63})"));
    check(config.enabled&&config.radius==7&&config.destination==63,"valid destination/radius rejected");
    check(config.items==std::vector<std::uint16_t>{3031}&&config.corpses==std::vector<std::uint16_t>({2813,3000}),"ID filter or corpse merge");
    for(const auto* input:{R"({"enabled":true,"item_ids":[1]})",R"({"enabled":true,"corpse_item_ids":[1]})",R"({"enabled":true,"item_ids":[1],"corpse_item_ids":[1],"target_container_id":64})",R"({"enabled":true,"item_ids":[1],"corpse_item_ids":[1],"target_container_id":0.5})"})
        check(!crome::parseOldersLoot(crome::json::parse(input)).enabled,"unsafe configuration enabled");
    config=crome::parseOldersLoot(crome::json::parse(R"({"enabled":true,"only_list":false,"corpse_item_ids":[1]})"));
    check(config.enabled&&!config.onlyList,"explicit all-items mode rejected");
    auto destination=crome::lootContainerDestination(0,5);check(destination.x==65535&&destination.y==64&&destination.z==5,"bag encoding wrong");
    destination=crome::lootContainerDestination(63,254);check(destination.y==127&&destination.z==254,"last container encoding wrong");
    crome::LootRetryBudget retries;
    check(retries.allow(100,3,2,4,0)&&retries.allow(100,3,2,4,0)&&retries.allow(100,3,2,4,0)&&!retries.allow(100,3,2,4,0),"unchanged item retried forever");
    check(retries.allow(100,2,2,4,0),"stack progress did not reset retries");
    check(retries.allow(100,2,2,4,1),"destination change did not reset retries");
    check(retries.allow(101,2,2,4,1),"new item did not reset retries");
    std::cout<<"PASS: loot configuration, corpse IDs, destination encoding, unchanged-request limit and observed progress\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
