#include "../modules/explore/VisualTiles.hpp"
#include <iostream>
#include <stdexcept>
using namespace crome::visual;
static void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
static Image pattern(int seed){Image image{32,32,{}};for(int y=0;y<32;++y)for(int x=0;x<32;++x)
    image.pixels.push_back({static_cast<std::uint8_t>((x*13+y*7+seed)%256),static_cast<std::uint8_t>((x*3+y*17+seed*2)%256),static_cast<std::uint8_t>((x*23+y*5+seed*3)%256),255});return image;}
int main(){try{
    Image black{32,32,std::vector<Pixel>(1024,{0,0,0,255})};check(!hasDetail(black),"black capture accepted");
    auto a=pattern(0),b=pattern(99);std::vector<Reference> bank{{"floor","test","passage",870,a},{"fire","test","hazard",0,b}};
    auto m=match(a,bank);check(m.accepted&&m.index==0&&m.score==1,"exact floor not identified");
    bank.push_back(bank[0]);check(match(a,bank).accepted,"duplicate evidence caused false ambiguity");
    bank.push_back({"wall","test","blocked",123,a});check(!match(a,bank).accepted,"identical different labels silently chosen");
    check(!match(black,bank).accepted,"uniform tile identified");check(!match(a,{}).accepted,"empty bank identified");
    Image transparent=a;for(auto& pixel:transparent.pixels)pixel.a=0;
    check(difference(a,transparent)==1,"empty alpha mask accepted");
    Image grid{47,35,{}};grid.pixels.resize(47*35);for(int y=0;y<35;++y)for(int x=0;x<47;++x)grid.pixels[y*47+x]={std::uint8_t(x),std::uint8_t(y),1,255};
    unsigned covered=0;for(int row=0;row<11;++row)for(int col=0;col<15;++col){auto part=cell(grid,15,11,col,row);check(part.valid(),"nondivisible grid has empty cell");covered+=part.pixels.size();}
    check(covered==47*35,"grid dropped or duplicated pixels");check(!cell(grid,15,11,15,0).valid(),"out of bounds cell read");
    check(!crop(grid,-1,0,4,4).valid(),"negative crop allowed");
    check(worldCell({100,200,7},0,0,7,5)==Location{93,195,7},"world/grid origin incorrect");
    Observation before{{100,200,7},100,0,1000,false},after{{101,200,7},80,0,1500,false};
    check(movementEvidence(before,after)==Event::DamageCandidate,"damage observation lost");
    after.combat=true;check(movementEvidence(before,after)==Event::Passage,"combat damage classified as tile damage");
    after.combat=false;after.position=before.position;check(movementEvidence(before,after)==Event::DamageCandidate,"delayed field tick lost");
    after.health=100;after.states=1;check(movementEvidence(before,after)==Event::StateChangeCandidate,"new condition bits lost");
    after.position.z=8;check(movementEvidence(before,after)==Event::Transition,"floor transition lost");
    after.time=6000;check(movementEvidence(before,after)==Event::None,"stale position linked to transition");
    after.time=1500;after.position={120,200,7};check(movementEvidence(before,after)==Event::None,"teleport linked to adjacent movement");
    std::cout<<"PASS: image ambiguity/duplicates/alpha, empty captures, complete grid coverage, world alignment and conservative movement evidence\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
