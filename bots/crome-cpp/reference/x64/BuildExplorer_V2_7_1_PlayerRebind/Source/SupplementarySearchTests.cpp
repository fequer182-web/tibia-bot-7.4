#include "SupplementarySearchCore.hpp"
#include <iostream>
#include <stdexcept>
using namespace otclab::extra;
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
template<class T>void put(std::vector<std::uint8_t>& b,std::size_t off,T value){std::memcpy(b.data()+off,&value,sizeof(value));}
int main(){try{
    std::vector<std::uint8_t> before(128),after(128);
    put<std::uint32_t>(before,32,27200);put<std::uint32_t>(after,32,29700);
    put<double>(before,64,2.72);put<double>(after,64,2.97);
    put<double>(before,80,272.75);put<double>(after,80,297.25);
    auto results=transitions(before,after,272,297);bool integer=false,scaled=false,truncated=false;
    for(auto c:results){
        integer|=c.offset==32&&c.storage==Storage::U32&&c.scale==0.01;
        scaled|=c.offset==64&&c.storage==Storage::F64&&c.scale==100;
        truncated|=c.offset==80&&c.storage==Storage::F64&&c.scale==1&&c.displayMode=="floor_candidate";
    }
    check(integer&&scaled&&truncated,"extra numeric storage/scale/display searches missing");
    std::vector<std::uint8_t> a(32),b(32),c(32);
    put<std::uint32_t>(a,0,283);put<std::uint32_t>(b,0,237);put<std::uint32_t>(c,0,250);
    put<double>(a,8,2.83);put<double>(b,8,2.37);put<double>(c,8,2.50);
    put<std::uint32_t>(a,24,283);put<std::uint32_t>(b,24,237);put<std::uint32_t>(c,24,999);
    auto hypotheses=transitions(a,b,283,237);filterObservation(hypotheses,c,250);
    bool direct=false,decimal=false;for(auto h:hypotheses){direct|=h.offset==0&&h.storage==Storage::U32&&h.scale==1;decimal|=h.offset==8&&h.storage==Storage::F64&&h.scale==100;check(h.offset!=24,"third observation retained coincidence");}
    check(direct&&decimal,"third validator lost direct integer or scaled decimal");
    check(lightCycle({0,7,0,6,0,9,0}),"independent torch/spell cycle rejected");
    check(lightCycle({0,7,0,7,0,7,0}),"same levels should remain ambiguous candidates");
    check(!lightCycle({0,7,0,6,1,9,0}),"baseline drift accepted");
    check(!lightCycle({0,0,0,0,0,0,0}),"constant field accepted as light correlation");
    check(!lightCycle({0,7,0,NAN,0,9,0}),"invalid light sample accepted");
    check(lightCycle({0,7,0}),"torch-only cycle rejected");
    check(!lightCycle({0,7}),"incomplete torch cycle accepted");
    std::vector<std::uint8_t> floatBytes(8);put<float>(floatBytes,0,7.0f);
    check(value(floatBytes,0,Storage::F32)==7&&value(floatBytes,3,Storage::U8)==64,"float7 byte overlap explanation incorrect");
    check(transitions(before,after,272,272).empty(),"unchanged displayed value promoted");
    check(transitions(before,after,NAN,297).empty(),"invalid user value accepted");
    check(std::isnan(value(before,128,Storage::U64)),"out of bounds numeric read");
    std::map<std::uintptr_t,std::vector<std::uint8_t>> memory;
    memory[0x10000].resize(128);memory[0x20000].resize(128);memory[0x30000].resize(128);memory[0x40000].resize(128);
    put<std::uintptr_t>(memory[0x20000],16,0x30000);put<std::uintptr_t>(memory[0x30000],32,0x40000);
    put<std::uintptr_t>(memory[0x40000],48,0x90000);put<std::uintptr_t>(memory[0x40000],56,0x30000); // cycle
    Reader reader=[&](auto p,auto,std::vector<std::uint8_t>& out){auto it=memory.find(p);if(it==memory.end())return false;out=it->second;return true;};
    auto missing=pointerPaths(0x10000,{0x90000},reader);check(missing.paths.empty(),"wrong root invented a path");
    auto valid=pointerPaths(0x20000,{0x90000},reader);check(valid.paths.size()==1&&valid.paths[0].offsets==std::vector<std::size_t>({16,32,48}),"alternate root/depth search failed");
    auto shallow=pointerPaths(0x20000,{0x90000},reader,256,2);check(shallow.paths.empty(),"depth bound ignored");
    auto bounded=pointerPaths(0x20000,{0x90000},reader,1);check(bounded.bounded&&bounded.paths.empty(),"node budget ignored");
    auto cancelled=pointerPaths(0x20000,{0x90000},reader,256,4,[]{return true;});check(cancelled.bounded&&cancelled.nodes==0,"cancellation ignored");
    auto pathB=valid.paths[0];pathB.target=0xA0000;pathB.offsets.back()=64;
    check(sameParentSignature(valid.paths[0],pathB),"different SQM slot lost common parent evidence");
    pathB.root=0x10000;check(!sameParentSignature(valid.paths[0],pathB),"different roots merged");
    std::cout<<"PASS: capacity storage/scales/rounding, invalid observations, additional root, deeper paths, cycles and budgets\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
