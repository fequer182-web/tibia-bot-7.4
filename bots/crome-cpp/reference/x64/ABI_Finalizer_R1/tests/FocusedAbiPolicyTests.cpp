#include "../Source/FocusedAbiPolicy.hpp"
#include <iostream>
int main(){
    auto b=focused_abi::bind(0xFA3000,0xEFDC30,0xEFDC40,0x10,0x20);
    if(!b.valid||b.attack!=0x20||b.follow!=0x30)return 1;
    auto same=focused_abi::bind(0x10000,0x2000,0x2000,8,16);if(!same.valid||same.attack!=8)return 2;
    if(focused_abi::bind(0x10000,0x3000,0x1000,8,16).valid)return 3;
    if(focused_abi::bind(0x10000,0x10001,0x1000,8,16).valid)return 4;
    if(focused_abi::bind(0x10000,0x1010,0x1000,8,16).valid)return 5;
    std::cout<<"PASS: receiver/state separation, offset projection, bounds and underflow\n";
}
