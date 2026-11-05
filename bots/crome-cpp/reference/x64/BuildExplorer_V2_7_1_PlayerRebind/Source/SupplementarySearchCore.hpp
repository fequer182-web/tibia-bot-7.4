#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace otclab::extra {
enum class Storage { U8,U16,U32,U64,F32,F64 };
inline const char* storageName(Storage s){switch(s){case Storage::U8:return "u8";case Storage::U16:return "u16";case Storage::U32:return "u32";case Storage::U64:return "u64";case Storage::F32:return "f32";default:return "f64";}}
inline unsigned width(Storage s){return s==Storage::U8?1:s==Storage::U16?2:s==Storage::U32||s==Storage::F32?4:8;}
inline double value(const std::vector<std::uint8_t>& bytes,std::size_t offset,Storage s){
    if(offset>bytes.size()||width(s)>bytes.size()-offset)return NAN;
    auto* p=bytes.data()+offset;
    switch(s){
        case Storage::U8:return *p;
        case Storage::U16:{std::uint16_t v{};std::memcpy(&v,p,2);return v;}
        case Storage::U32:{std::uint32_t v{};std::memcpy(&v,p,4);return v;}
        case Storage::U64:{std::uint64_t v{};std::memcpy(&v,p,8);return v>9007199254740991ull?NAN:double(v);}
        case Storage::F32:{float v{};std::memcpy(&v,p,4);return v;}
        default:{double v{};std::memcpy(&v,p,8);return v;}
    }
}
struct NumericCandidate {std::size_t offset{};Storage storage{};double scale{},before{},after{};std::string displayMode{"exact"};};
inline bool valuesClose(double a,double b){return std::isfinite(a)&&std::isfinite(b)&&std::abs(a-b)<=std::max(0.005,std::abs(b)*0.000001);}
inline std::vector<NumericCandidate> transitions(const std::vector<std::uint8_t>& before,const std::vector<std::uint8_t>& after,double oldDisplayed,double newDisplayed){
    std::vector<NumericCandidate> found;
    if(!std::isfinite(oldDisplayed)||!std::isfinite(newDisplayed)||valuesClose(oldDisplayed,newDisplayed))return found;
    const auto size=std::min(before.size(),after.size());
    for(auto s:{Storage::U16,Storage::U32,Storage::U64,Storage::F32,Storage::F64})for(std::size_t off=0;off+width(s)<=size;off+=width(s)){
        double a=value(before,off,s),b=value(after,off,s);
        for(double scale:{1.0,0.01,100.0}){
            if(valuesClose(a*scale,oldDisplayed)&&valuesClose(b*scale,newDisplayed))found.push_back({off,s,scale,a,b,"exact"});
            else if(std::isfinite(a)&&std::isfinite(b)&&std::floor(oldDisplayed)==oldDisplayed&&std::floor(newDisplayed)==newDisplayed&&
                    std::floor(a*scale)==oldDisplayed&&std::floor(b*scale)==newDisplayed)found.push_back({off,s,scale,a,b,"floor_candidate"});
        }
    }
    return found;
}
inline bool matchesDisplay(double raw,double displayed,const NumericCandidate& c){
    if(!std::isfinite(raw)||!std::isfinite(displayed))return false;
    return c.displayMode=="exact"?valuesClose(raw*c.scale,displayed):std::floor(raw*c.scale)==displayed;
}
inline void filterObservation(std::vector<NumericCandidate>& candidates,const std::vector<std::uint8_t>& bytes,double displayed){
    std::erase_if(candidates,[&](const auto& c){return !matchesDisplay(value(bytes,c.offset,c.storage),displayed,c);});
}
// Only correlations: no intensity/color/radius labels and no write validation.
inline bool lightCycle(const std::vector<double>& v){
    if(v.size()<3||v.size()%2!=1)return false;
    for(double n:v)if(!std::isfinite(n)||n<0||n>65535)return false;
    bool changed=false;
    for(std::size_t i=1;i<v.size();i+=2){if(!valuesClose(v[0],v[i+1]))return false;changed|=!valuesClose(v[0],v[i]);}
    return changed;
}
struct Path {std::uintptr_t root{},target{},terminalOwner{};std::vector<std::size_t> offsets;};
using Reader=std::function<bool(std::uintptr_t,std::size_t,std::vector<std::uint8_t>&)>;
struct SearchResult {std::vector<Path> paths;unsigned nodes{};bool bounded{};};
// Supplemental hypotheses only: reachability is not an indexed Map lookup.
inline SearchResult pointerPaths(std::uintptr_t root,const std::set<std::uintptr_t>& targets,const Reader& reader,unsigned maxNodes=256,unsigned maxDepth=4,const std::function<bool()>& cancelled={}){
    SearchResult out;struct Node{std::uintptr_t address;std::vector<std::size_t> path;};
    std::vector<Node> pending{{root,{}}};std::set<std::uintptr_t> seen;
    for(std::size_t cursor=0;cursor<pending.size();++cursor){
        if(out.nodes>=maxNodes||(cancelled&&cancelled())){out.bounded=true;break;}
        auto node=pending[cursor];if(!node.address||!seen.insert(node.address).second)continue;
        ++out.nodes;std::vector<std::uint8_t> bytes;
        if(!reader(node.address,node.path.empty()?0x1000:0x4000,bytes))continue;
        unsigned children=0;
        for(std::size_t off=0;off+8<=bytes.size();off+=8){
            std::uintptr_t p{};std::memcpy(&p,bytes.data()+off,8);
            auto path=node.path;path.push_back(off);
            if(targets.count(p)){out.paths.push_back({root,p,node.address,std::move(path)});if(out.paths.size()>=32){out.bounded=true;return out;}}
            else if(p>=0x10000&&p<=0x7fffffffffffULL&&(p&7)==0&&path.size()<maxDepth&&children<128&&pending.size()<maxNodes*4&&!seen.count(p)){
                pending.push_back({p,std::move(path)});++children;
            }
        }
    }
    return out;
}
inline bool sameParentSignature(const Path& a,const Path& b){
    if(a.root!=b.root||a.target==b.target||a.offsets.size()!=b.offsets.size()||a.offsets.empty())return false;
    return std::equal(a.offsets.begin(),a.offsets.end()-1,b.offsets.begin());
}
}

