#include "LocalPlayerSeedPolicy.hpp"
#include <stdexcept>
#include <iostream>
#include <vector>
using namespace otclab::player_seed;
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
int main(){try{
    Selection s;
    check(!s.consider(0,1000,0)&&!s.found,"numeric-only candidate accepted");
    check(s.consider(1,50,100),"real base rejected");
    check(!s.consider(2,9999,0)&&s.index==1,"unresolved candidate displaced valid base");
    check(s.consider(3,51,200)&&s.index==3,"higher valid base ignored");
    check(anchor(0x2000,0x1000)==0x2000,"nearby small stat moved scan outside seed object");
    check(anchor(0,0x1000)==0x1000,"legacy no-anchor fallback lost");
    std::vector<unsigned> attempted;
    check(resolve([&](unsigned stage){attempted.push_back(stage);Selection selection;
        for(unsigned i=0;i<4;++i)selection.consider(i,100,stage==1?200:0);
        return selection.found;
    }),"four unconfirmed HP hits stopped fallback");
    check(attempted==std::vector<unsigned>({0,1}),"XP scanned after Mana succeeded");
    attempted.clear();check(!resolve([&](unsigned stage){attempted.push_back(stage);return false;}),"failed stages reported success");
    check(attempted==std::vector<unsigned>({0,1,2}),"fallback missed a seed stage");
    std::cout<<"PASS: invalid-base exclusion, seeded structural anchor, four false HP hits, Mana fallback and exhaustion\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
