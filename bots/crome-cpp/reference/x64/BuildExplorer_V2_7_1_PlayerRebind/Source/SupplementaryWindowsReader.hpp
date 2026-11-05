#pragma once
#include <windows.h>
#include "SupplementarySearchCore.hpp"
namespace otclab::extra {
inline bool readable(DWORD protection){
    if(protection&(PAGE_GUARD|PAGE_NOACCESS))return false;
    switch(protection&0xff){case PAGE_READONLY:case PAGE_READWRITE:case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READ:case PAGE_EXECUTE_READWRITE:case PAGE_EXECUTE_WRITECOPY:return true;default:return false;}
}
inline bool snapshot(std::uintptr_t address,std::size_t requested,std::vector<std::uint8_t>& bytes){
    bytes.clear();MEMORY_BASIC_INFORMATION mbi{};
    if(!address||!VirtualQuery(reinterpret_cast<void*>(address),&mbi,sizeof(mbi))||mbi.State!=MEM_COMMIT||!readable(mbi.Protect))return false;
    const auto end=reinterpret_cast<std::uintptr_t>(mbi.BaseAddress)+mbi.RegionSize;
    if(end<=address)return false;bytes.resize(std::min(requested,static_cast<std::size_t>(end-address)));
    SIZE_T read{};ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address),bytes.data(),bytes.size(),&read);
    bytes.resize(read);return !bytes.empty();
}
}
