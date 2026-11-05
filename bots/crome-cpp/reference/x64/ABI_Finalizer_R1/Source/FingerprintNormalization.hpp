#pragma once
#include <vector>
#include <cstdint>
#include <cstring>
namespace focused_abi {
inline bool projectRelocation(std::vector<std::uint8_t>& text,std::uint32_t textRva,std::uint32_t at,unsigned type,std::uint64_t currentBase,std::uint64_t referenceBase){
    if(type==0)return true;
    if(std::uint64_t(at)+8<=textRva||std::uint64_t(at)>=std::uint64_t(textRva)+text.size())return true;
    if(type!=10||at<textRva||std::uint64_t(at)-textRva+8>text.size())return false;
    std::uint64_t value{};std::memcpy(&value,text.data()+at-textRva,8);value+=referenceBase-currentBase;
    std::memcpy(text.data()+at-textRva,&value,8);return true;
}
}
