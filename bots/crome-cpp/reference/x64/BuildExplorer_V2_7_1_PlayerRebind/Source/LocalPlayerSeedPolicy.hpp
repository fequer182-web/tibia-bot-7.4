#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>

namespace otclab::player_seed {
struct Selection {
    bool found{};std::size_t index{};int score{std::numeric_limits<int>::min()};
    bool consider(std::size_t candidate,int total,std::uintptr_t base){
        if(!base|| (found&&total<=score))return false;
        found=true;index=candidate;score=total;return true;
    }
};
// An auxiliary small-valued stat can belong to a preceding allocation. Do
// not move the structural search away from the original seed's address.
inline std::uintptr_t anchor(std::uintptr_t seed,std::uintptr_t lowestEvidence){
    return seed?seed:lowestEvidence;
}
template<class TryStage> bool resolve(TryStage&& tryStage){
    for(unsigned stage=0;stage<3;++stage)if(tryStage(stage))return true;
    return false;
}
}
