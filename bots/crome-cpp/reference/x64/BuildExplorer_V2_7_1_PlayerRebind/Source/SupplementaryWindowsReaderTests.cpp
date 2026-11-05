#define NOMINMAX
#include "SupplementaryWindowsReader.hpp"
#include <iostream>
#include <stdexcept>
using namespace otclab::extra;
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
int main(){void* allocation=nullptr;try{
    SYSTEM_INFO info{};GetSystemInfo(&info);auto page=info.dwPageSize;
    allocation=VirtualAlloc(nullptr,page*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    check(allocation,"test allocation failed");auto root=reinterpret_cast<std::uintptr_t>(allocation);
    std::uint64_t needle=0x123456789abcdef0ull;std::memcpy(reinterpret_cast<void*>(root+16),&needle,8);
    std::vector<std::uint8_t> bytes;
    check(snapshot(root,64,bytes)&&bytes.size()==64,"own read-only snapshot failed");
    std::uint64_t result{};std::memcpy(&result,bytes.data()+16,8);check(result==needle,"snapshot corrupted data");
    DWORD old{};check(VirtualProtect(reinterpret_cast<void*>(root+page),page,PAGE_NOACCESS,&old),"test protection failed");
    check(!snapshot(root+page,64,bytes)&&bytes.empty(),"inaccessible page accepted");
    check(snapshot(root+page-16,128,bytes)&&bytes.size()==16,"snapshot crossed unreadable region");
    check(!snapshot(0,64,bytes)&&bytes.empty(),"null pointer accepted");
    check(!readable(PAGE_GUARD|PAGE_READWRITE),"guard page accepted");
    VirtualFree(allocation,0,MEM_RELEASE);allocation=nullptr;
    std::cout<<"PASS: real own-process read, inaccessible/null/guard rejection and bounded region tail\n";
}catch(const std::exception& error){if(allocation)VirtualFree(allocation,0,MEM_RELEASE);std::cerr<<error.what()<<'\n';return 1;}}
