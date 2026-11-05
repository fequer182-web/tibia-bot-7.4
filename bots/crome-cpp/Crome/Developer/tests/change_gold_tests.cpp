#include "../native_runtime/bot/ChangeGold.hpp"
#include <iostream>
#include <stdexcept>
void check(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
int main(){try{
    auto root=crome::json::parse(R"({"enabled":true,"item_ids":[3031,3035,3031,0,-1,65536,1.5,"bad"],"interval_ms":1200})");
    auto cfg=crome::parseChangeGoldConfig(&root);
    check(cfg.enabled&&cfg.itemIds==std::vector<std::uint16_t>({3031,3035})&&cfg.intervalMs==1200,"invalid coin config");
    root=crome::json::parse(R"({"enabled":true,"item_ids":[],"interval_ms":0})");cfg=crome::parseChangeGoldConfig(&root);
    check(!cfg.enabled&&cfg.intervalMs==1000,"empty ids enabled conversion");
    check(!crome::coinStackMatches(99,100)&&crome::coinStackMatches(100,100)&&!crome::coinStackMatches(101,100),"partial stack converted");
    check(crome::coinStackMatches(1,0),"legacy item use changed");
    std::cout<<"PASS: coin ids, invalid config, interval, full stacks and legacy lookup\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
