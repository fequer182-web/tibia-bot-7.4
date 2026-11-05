#pragma once
#include <cstring>
#include <functional>
namespace otclab::extra {
template<class T> class TemporaryLightValue {
    std::function<bool(T&)> read_;std::function<bool(const T&)> write_;std::function<bool()> identity_;
    T original_{};bool loaded_{},attempted_{},restored_{};
public:
    TemporaryLightValue(std::function<bool(T&)> r,std::function<bool(const T&)> w,std::function<bool()> id):read_(r),write_(w),identity_(id){loaded_=identity_()&&read_(original_);}
    TemporaryLightValue(const TemporaryLightValue&)=delete;
    ~TemporaryLightValue(){if(attempted_&&!restored_)restore();}
    bool loaded()const{return loaded_;}T original()const{return original_;}
    bool apply(T value){if(!loaded_||!identity_())return false;attempted_=true;T actual{};return write_(value)&&identity_()&&read_(actual)&&std::memcmp(&value,&actual,sizeof(T))==0;}
    bool restore(){if(!loaded_||!identity_())return false;T actual{};restored_=write_(original_)&&identity_()&&read_(actual)&&std::memcmp(&original_,&actual,sizeof(T))==0;return restored_;}
};
}
