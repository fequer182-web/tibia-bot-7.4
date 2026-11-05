#include "TypedLightValidation.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
using otclab::extra::TemporaryLightValue;
void check(bool b,const char* m){if(!b)throw std::runtime_error(m);}
int main(){try{
    std::array<unsigned char,12> memory{};memory.fill(0xAB);float zero=0;std::memcpy(memory.data()+4,&zero,4);
    auto read=[&](float& v){std::memcpy(&v,memory.data()+4,4);return true;};
    auto write=[&](const float& v){std::memcpy(memory.data()+4,&v,4);return true;};
    {TemporaryLightValue<float> test(read,write,[]{return true;});check(test.loaded()&&test.apply(15.0f),"float write/readback failed");check(test.restore(),"float restore failed");}
    check(memory[3]==0xAB&&memory[8]==0xAB,"neighbor bytes changed");float restored{};read(restored);check(restored==0,"float original lost");
    {TemporaryLightValue<float> test(read,write,[]{return true;});check(test.apply(7.0f),"float7 apply failed");}read(restored);check(restored==0,"destructor failed to restore");
    bool first=true;auto partial=[&](const float& v){write(v);if(first){first=false;return false;}return true;};
    {TemporaryLightValue<float> test(read,partial,[]{return true;});check(!test.apply(15),"partial write counted success");}read(restored);check(restored==0,"partial failure not restored");
    bool identity=true;int writes=0;auto counted=[&](const float& v){++writes;return write(v);};
    {TemporaryLightValue<float> test(read,counted,[&]{return identity;});identity=false;check(!test.apply(15)&&!test.restore(),"stale object accepted");}check(writes==0,"stale address written");
    unsigned char byte=7;{TemporaryLightValue<unsigned char> test([&](auto& v){v=byte;return true;},[&](const auto& v){byte=v;return true;},[]{return true;});check(test.apply(15)&&test.restore()&&byte==7,"legacy u8 cycle changed");}
    std::cout<<"PASS: float full width, neighbors, readback, restore, early exit, partial failure, stale identity and u8\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
