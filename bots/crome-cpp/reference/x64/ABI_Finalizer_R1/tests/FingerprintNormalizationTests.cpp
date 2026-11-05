#include "../Source/FingerprintNormalization.hpp"
#include <iostream>
int main(){
    std::vector<std::uint8_t> before(32,0x90),rebased=before;
    std::uint64_t address=0x140123456;std::memcpy(before.data()+8,&address,8);
    address+=0x7FF600000000ULL-0x140000000ULL;std::memcpy(rebased.data()+8,&address,8);
    if(!focused_abi::projectRelocation(rebased,0x1000,0x1008,10,0x7FF600000000ULL,0x140000000ULL)||rebased!=before)return 1;
    rebased[0]^=1;if(rebased==before)return 2;
    if(focused_abi::projectRelocation(rebased,0x1000,0x1008,3,0,0))return 3;
    if(focused_abi::projectRelocation(rebased,0x1000,0x101D,10,0,0))return 4;
    if(!focused_abi::projectRelocation(rebased,0x1000,0x2000,10,0,0))return 5;
    std::cout<<"PASS: ASLR projection matches original bytes; real code changes remain different; unsupported types and boundaries rejected\n";
}
