#include "../native_runtime/bot/Targeting.hpp"
#include <iostream>
#include <filesystem>
using namespace crome;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int main(){try {
    const Position monster{100,100,7};
    check(!targetPositionDestination({100,101,7},monster,1),"None moved");
    check(!targetPositionDestination({100,101,6},monster,7),"floor changed");
    check(!targetPositionDestination({103,103,7},monster,7),"correct diagonal moved");
    for (int x=94;x<=106;++x) for(int y=94;y<=106;++y) {
        Position self{x,y,7}; auto dest=targetPositionDestination(self,monster,7);
        if(dest){check(std::abs(dest->x-100)==3&&std::abs(dest->y-100)==3,"diagonal 3 geometry");
            int cost=self.chebyshevDistance(*dest);
            for(int sx:{-3,3})for(int sy:{-3,3})check(cost<=self.chebyshevDistance({100+sx,100+sy,7}),"not closest diagonal");}
        for(int mode:{3,4,5}) {auto ring=targetPositionDestination(self,monster,mode);
            if(ring)check(ring->chebyshevDistance(monster)==targetModeDistance(mode),"incorrect range ring");}
    }
    auto retreat=targetPositionDestination({100,101,7},monster,4);
    check(retreat&&retreat->chebyshevDistance(monster)==3,"no retreat from too close");
    auto moved=targetPositionDestination({103,103,7},{101,100,7},7);
    check(moved&&std::abs(moved->x-101)==3&&std::abs(moved->y-100)==3,"moving monster not tracked");
    auto path=std::filesystem::temp_directory_path()/"crome_target_modes_test.json";
    auto load=[&](const char* text){std::ofstream(path)<<text;return loadTargetingConfig(path.string());};
    auto none=load(R"({"movement_mode":1,"chase_enabled":true})");check(!none.chaseEnabled,"none ignored");
    auto diag=load(R"({"movement_mode":7,"chase_enabled":false,"chase_distance":1})");
    check(diag.chaseEnabled&&diag.chaseDistance==3,"diagonal config ignored");
    auto legacy=load(R"({"chase_enabled":false,"chase_distance":2})");
    check(legacy.movementMode==-1&&!legacy.chaseEnabled&&legacy.chaseDistance==2,"legacy changed");
    auto invalid=load(R"({"movement_mode":99})");check(!invalid.chaseEnabled,"invalid mode moved");
    std::filesystem::remove(path);
    std::cout<<"PASS: None, distance/retreat rings, four diagonal quadrants, moving target, floor and config compatibility\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
