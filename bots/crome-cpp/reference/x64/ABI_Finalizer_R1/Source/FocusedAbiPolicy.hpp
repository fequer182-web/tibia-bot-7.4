#pragma once
#include <cstdint>
namespace focused_abi {
struct Binding {std::uintptr_t receiver{},attack{},follow{};bool valid{};};
inline Binding bind(std::uintptr_t imageSize,std::uintptr_t receiver,std::uintptr_t state,std::uintptr_t attack,std::uintptr_t follow){
    if(!receiver||!state||receiver>=imageSize||state>=imageSize)return {};
    const auto delta=static_cast<std::int64_t>(state)-static_cast<std::int64_t>(receiver);
    const auto a=static_cast<std::int64_t>(attack)+delta,f=static_cast<std::int64_t>(follow)+delta;
    if(delta < -0x1000||delta>0x1000||a<0||f<0||a>0x4000||f>0x4000)return {};
    return {receiver,static_cast<std::uintptr_t>(a),static_cast<std::uintptr_t>(f),true};
}
inline bool isAction(const char* key){return key&&key[0]=='g'&&key[1]=='a'&&key[2]=='m'&&key[3]=='e';}
}
