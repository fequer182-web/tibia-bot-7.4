#pragma once
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <cstring>

namespace crome::olders_baseline {
inline constexpr unsigned long timestamp=0x6AA9FA57;
inline constexpr unsigned long imageSize=0x17BF000;
inline constexpr unsigned long long textHash=0x89D3D35243890EA2ULL;
inline bool matches(unsigned long stamp,unsigned long size,unsigned long long hash) {
    return stamp==timestamp&&size==imageSize&&hash==textHash;
}
// Hash disk .text: runtime hooks/relocations must not change build identity.
inline bool matchesExecutable(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary);if(!file)return false;
    IMAGE_DOS_HEADER dos{};file.read(reinterpret_cast<char*>(&dos),sizeof(dos));
    if(!file||dos.e_magic!=IMAGE_DOS_SIGNATURE||dos.e_lfanew<0)return false;
    file.seekg(dos.e_lfanew);IMAGE_NT_HEADERS64 nt{};file.read(reinterpret_cast<char*>(&nt),sizeof(nt));
    if(!file||nt.Signature!=IMAGE_NT_SIGNATURE||nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64||nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC||nt.FileHeader.NumberOfSections>96)return false;
    if(nt.FileHeader.TimeDateStamp!=timestamp||nt.OptionalHeader.SizeOfImage!=imageSize)return false;
    file.seekg(dos.e_lfanew+sizeof(DWORD)+sizeof(IMAGE_FILE_HEADER)+nt.FileHeader.SizeOfOptionalHeader);
    for(unsigned i=0;i<nt.FileHeader.NumberOfSections;++i){IMAGE_SECTION_HEADER section{};file.read(reinterpret_cast<char*>(&section),sizeof(section));if(!file)return false;
        if(std::memcmp(section.Name,".text",5))continue;
        if(!section.SizeOfRawData||section.SizeOfRawData>64*1024*1024)return false;
        std::vector<unsigned char> bytes(section.SizeOfRawData);file.seekg(section.PointerToRawData);file.read(reinterpret_cast<char*>(bytes.data()),bytes.size());if(!file)return false;
        unsigned long long hash=14695981039346656037ULL;for(auto value:bytes){hash^=value;hash*=1099511628211ULL;}return matches(nt.FileHeader.TimeDateStamp,nt.OptionalHeader.SizeOfImage,hash);
    }return false;
}
inline bool processMatches(DWORD pid) {
    HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);if(!process)return false;
    wchar_t path[32768]{};DWORD size=32768;const bool found=QueryFullProcessImageNameW(process,0,path,&size)!=FALSE;CloseHandle(process);
    return found&&matchesExecutable(std::filesystem::path(std::wstring(path,size)));
}
}
