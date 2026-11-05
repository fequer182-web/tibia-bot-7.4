#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

static_assert(sizeof(void*)==8,"Use x64");
std::atomic<bool> interrupted{false};
BOOL WINAPI ConsoleHandler(DWORD){interrupted=true;return TRUE;}
std::string Hex(uint64_t value){std::ostringstream out;out<<"0x"<<std::hex<<value;return out.str();}
std::runtime_error Error(const char* api){return std::runtime_error(std::string(api)+" falhou. WinError="+std::to_string(GetLastError()));}
struct Handle {
    HANDLE h=nullptr;
    explicit Handle(HANDLE value=nullptr):h(value){}
    ~Handle(){if(h && h!=INVALID_HANDLE_VALUE)CloseHandle(h);}
    Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
};
bool Read(HANDLE process,uintptr_t address,void* buffer,size_t size){SIZE_T got=0;return ReadProcessMemory(process,reinterpret_cast<void*>(address),buffer,size,&got)&&got==size;}
template<class T> T ReadValue(HANDLE process,uintptr_t address){T value{};if(!Read(process,address,&value,sizeof(value)))throw Error("ReadProcessMemory");return value;}
uintptr_t CheckBuild(HANDLE process,DWORD pid){
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid));
    if(snapshot.h==INVALID_HANDLE_VALUE)throw Error("CreateToolhelp32Snapshot");
    MODULEENTRY32W module{};module.dwSize=sizeof(module);
    if(!Module32FirstW(snapshot.h,&module))throw Error("Module32FirstW");
    if(_wcsicmp(module.szModule,L"Antica Retro opengl.exe"))throw std::runtime_error("PID nao e o Antica Retro OpenGL.");
    uintptr_t base=reinterpret_cast<uintptr_t>(module.modBaseAddr);
    auto dos=ReadValue<IMAGE_DOS_HEADER>(process,base);
    if(dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<64 || dos.e_lfanew>0x100000)throw std::runtime_error("DOS header invalido.");
    auto nt=ReadValue<IMAGE_NT_HEADERS64>(process,base+dos.e_lfanew);
    if(nt.Signature!=IMAGE_NT_SIGNATURE || nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64 ||
       nt.FileHeader.TimeDateStamp!=0x6AD91836 || nt.OptionalHeader.SizeOfImage!=0xEDE000 ||
       nt.OptionalHeader.AddressOfEntryPoint!=0x536BD0 || nt.FileHeader.NumberOfSections>96)
        throw std::runtime_error("Build diferente. Nenhum depurador foi anexado.");
    uintptr_t sections=base+dos.e_lfanew+FIELD_OFFSET(IMAGE_NT_HEADERS64,OptionalHeader)+nt.FileHeader.SizeOfOptionalHeader;
    bool found=false;
    for(unsigned i=0;i<nt.FileHeader.NumberOfSections;++i){
        auto section=ReadValue<IMAGE_SECTION_HEADER>(process,sections+i*sizeof(IMAGE_SECTION_HEADER));
        if(std::string(reinterpret_cast<char*>(section.Name),5)!=".text")continue;
        size_t size=std::max(section.Misc.VirtualSize,section.SizeOfRawData);
        if(section.VirtualAddress!=0x1000 || size!=0x95B400)throw std::runtime_error("Layout .text diferente.");
        uint64_t hash=0xCBF29CE484222325ULL;std::array<uint8_t,4096> bytes{};
        for(size_t offset=0;offset<size;){size_t amount=std::min(bytes.size(),size-offset);
            if(!Read(process,base+section.VirtualAddress+offset,bytes.data(),amount))throw Error("ReadProcessMemory .text");
            for(size_t n=0;n<amount;++n){hash^=bytes[n];hash*=0x100000001B3ULL;}offset+=amount;
        }
        if(hash!=0xBF68517610291E3AULL)throw std::runtime_error("Hash .text diferente. Nenhum depurador foi anexado.");
        found=true;break;
    }
    if(!found)throw std::runtime_error(".text ausente");
    return base;
}
