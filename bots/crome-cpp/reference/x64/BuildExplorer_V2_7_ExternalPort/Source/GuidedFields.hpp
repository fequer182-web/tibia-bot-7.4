#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace guided {
inline bool SeedAllowed(bool maxHealthFound,int fields,int positionShape) {
    return maxHealthFound && fields>=2 && positionShape>0;
}
struct Candidate {size_t offset; std::string type;};
struct Evidence {
    std::vector<Candidate> candidates;
    std::vector<double> observations;
    unsigned transitions=0;
    std::string reason="not_tested";
    std::string status() const {
        if(candidates.empty()) return "pending";
        if(candidates.size()>1) return "ambiguous";
        return transitions>=2?"confirmed_read_transition":"strong_candidate";
    }
};
inline size_t Width(const std::string& type) {return type=="float32" || type=="uint32" ? 4:8;}
inline bool Match(const std::vector<uint8_t>& data,const Candidate& c,double expected) {
    size_t n=Width(c.type); if(c.offset+n>data.size() || !std::isfinite(expected) || expected<0) return false;
    double value=0;
    if(c.type=="float64") {double v;std::memcpy(&v,data.data()+c.offset,8);value=v;}
    else if(c.type=="float32") {float v;std::memcpy(&v,data.data()+c.offset,4);value=v;}
    else if(c.type=="uint32") {uint32_t v;std::memcpy(&v,data.data()+c.offset,4);value=v;}
    else if(c.type=="uint64") {uint64_t v;std::memcpy(&v,data.data()+c.offset,8);if(v>9007199254740991ULL)return false;value=static_cast<double>(v);}
    else return false;
    // Allow display rounding to 2 decimals, but not integer rounding or a guessed scale.
    return std::isfinite(value) && std::abs(value-expected)<=0.0051;
}
inline std::vector<Candidate> Find(const std::vector<uint8_t>& data,double value) {
    std::vector<Candidate> result;
    for(const char* type:{"float64","float32","uint32","uint64"}) {
        size_t n=Width(type);
        for(size_t off=0;off+n<=data.size();off+=n) {Candidate c{off,type};if(Match(data,c,value))result.push_back(c);}
    }
    return result;
}
inline void Narrow(Evidence& e,const std::vector<uint8_t>& data,double value) {
    auto previous=e.observations.back();
    e.candidates.erase(std::remove_if(e.candidates.begin(),e.candidates.end(),[&](const Candidate& c){return !Match(data,c,value);}),e.candidates.end());
    if(std::abs(value-previous)>0.0051) ++e.transitions;
    e.observations.push_back(value);
    e.reason=e.candidates.empty()?"no_matching_candidate":e.transitions>=2 && e.candidates.size()==1?"two_observed_changes":"needs_more_evidence";
}
}
