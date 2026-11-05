#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace crome::visual {
struct Pixel {std::uint8_t r{},g{},b{},a{255};};
struct Image {
    int width{},height{};std::vector<Pixel> pixels;
    bool valid()const{return width>0&&height>0&&pixels.size()==static_cast<std::size_t>(width)*height;}
};
inline Image crop(const Image& src,int x,int y,int w,int h){
    Image out;if(!src.valid()||x<0||y<0||w<=0||h<=0||x+w>src.width||y+h>src.height)return out;
    out.width=w;out.height=h;out.pixels.reserve(static_cast<std::size_t>(w)*h);
    for(int row=y;row<y+h;++row)out.pixels.insert(out.pixels.end(),src.pixels.begin()+row*src.width+x,src.pixels.begin()+row*src.width+x+w);
    return out;
}
inline Image cell(const Image& src,int columns,int rows,int column,int row){
    if(columns<1||rows<1||columns>31||rows>31||column<0||row<0||column>=columns||row>=rows)return {};
    int x=column*src.width/columns,y=row*src.height/rows;
    return crop(src,x,y,(column+1)*src.width/columns-x,(row+1)*src.height/rows-y);
}
inline Image normalized(const Image& src){
    if(!src.valid())return {};
    Image out{32,32,{}};out.pixels.resize(1024);
    for(int y=0;y<32;++y)for(int x=0;x<32;++x)
        out.pixels[y*32+x]=src.pixels[(y*src.height/32)*src.width+x*src.width/32];
    return out;
}
inline bool hasDetail(const Image& src){
    if(!src.valid())return false;
    double sum=0,squared=0;
    for(auto p:src.pixels){double v=(p.r+p.g+p.b)/3.0;sum+=v;squared+=v*v;}
    const double mean=sum/src.pixels.size();
    return squared/src.pixels.size()-mean*mean>8.0;
}
inline double difference(const Image& sample,const Image& reference){
    if(!sample.valid()||!reference.valid()||sample.width!=32||sample.height!=32||reference.width!=32||reference.height!=32)return 1;
    double error=0;unsigned used=0;
    for(unsigned i=0;i<1024;++i){auto a=sample.pixels[i],b=reference.pixels[i];if(b.a<128)continue;
        error+=std::abs(int(a.r)-b.r)+std::abs(int(a.g)-b.g)+std::abs(int(a.b)-b.b);++used;}
    return used<128?1:error/(used*765.0);
}
struct Reference {std::string name,origin,category;unsigned clientId{};Image image;};
struct Match {int index{-1};double score{};bool accepted{};};
inline Match match(const Image& tile,const std::vector<Reference>& bank){
    const auto sample=normalized(tile);if(!hasDetail(sample))return {};
    double best=1,second=1;int index=-1;
    std::vector<double> differences;differences.reserve(bank.size());
    for(int i=0;i<static_cast<int>(bank.size());++i){double d=difference(sample,bank[i].image);differences.push_back(d);
        if(d<best){best=d;index=i;}}
    if(index>=0)for(int i=0;i<static_cast<int>(bank.size());++i){
        if(bank[i].name==bank[index].name&&bank[i].clientId==bank[index].clientId&&bank[i].category==bank[index].category)continue;
        second=std::min(second,differences[i]);
    }
    return {index,1-best,index>=0&&best<=0.10&&second-best>=0.025};
}
struct Location {int x{},y{},z{};bool operator==(const Location&)const=default;};
inline Location worldCell(Location player,int col,int row,int anchorCol,int anchorRow){return {player.x+col-anchorCol,player.y+row-anchorRow,player.z};}
struct Observation {Location position;double health{};std::uint64_t states{},time{};bool combat{};};
enum class Event { None, Passage, Transition, DamageCandidate, StateChangeCandidate };
inline Event movementEvidence(const Observation& before,const Observation& after){
    if(after.time<=before.time||after.time-before.time>3000||!std::isfinite(before.health)||!std::isfinite(after.health))return Event::None;
    if(before.position.z!=after.position.z)return Event::Transition;
    const int distance=std::max(std::abs(before.position.x-after.position.x),std::abs(before.position.y-after.position.y));
    if(distance>1)return Event::None;
    if(before.combat||after.combat)return distance?Event::Passage:Event::None;
    if(before.health-after.health>=20)return Event::DamageCandidate;
    if((after.states&~before.states)!=0)return Event::StateChangeCandidate;
    return distance?Event::Passage:Event::None;
}
}
