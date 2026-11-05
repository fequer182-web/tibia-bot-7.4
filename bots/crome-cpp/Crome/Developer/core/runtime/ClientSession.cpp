#include "core/runtime/ClientSession.hpp"
#ifdef _WIN32
#include <tlhelp32.h>
#include <psapi.h>
#include <algorithm>
#include <cwchar>
#endif
#include <chrono>

namespace crome {
static uint64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

ClientSession::~ClientSession(){ closeProcess(); }
void ClientSession::closeProcess(){
#ifdef _WIN32
    if(process_){ CloseHandle(process_); process_=nullptr; }
#endif
    memory_.set(nullptr);
    runtime_ = {};
}

bool ClientSession::loadProfiles(const std::filesystem::path& dir){
    registry_.loadDirectory(dir);
    return !registry_.all().empty();
}

#ifdef _WIN32
static std::string narrow(const std::wstring& w){
    if(w.empty()) return {};
    int n=WideCharToMultiByte(CP_UTF8,0,w.c_str(),(int)w.size(),nullptr,0,nullptr,nullptr);
    std::string s(n,'\0'); WideCharToMultiByte(CP_UTF8,0,w.c_str(),(int)w.size(),s.data(),n,nullptr,nullptr); return s;
}
static std::string baseName(const std::string& p){
    auto i=p.find_last_of("/\\"); return i==std::string::npos?p:p.substr(i+1);
}
#endif

bool ClientSession::refresh(bool force){
    const auto n=nowMs(); if(!force && n-lastRefreshMs_<750) return status_.localPlayerResolved; lastRefreshMs_=n;
#ifdef _WIN32
    struct Match { DWORD pid{}; uintptr_t base{}; const BuildProfile* profile{}; std::string exe; } found;
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snap==INVALID_HANDLE_VALUE){ status_.message="CreateToolhelp32Snapshot falhou"; return false; }
    PROCESSENTRY32W pe{}; pe.dwSize=sizeof(pe);
    if(Process32FirstW(snap,&pe)) do {
        std::wstring wexe=pe.szExeFile; std::string exe=narrow(wexe);
        for(const auto& p: registry_.all()){
            if(_stricmp(baseName(p.build.exe).c_str(),exe.c_str())!=0) continue;
            HANDLE ms=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pe.th32ProcessID);
            if(ms==INVALID_HANDLE_VALUE) continue;
            MODULEENTRY32W me{}; me.dwSize=sizeof(me);
            if(Module32FirstW(ms,&me)){
                // Match by exe first; runtime PE validation below rejects incompatible image.
                found={pe.th32ProcessID,reinterpret_cast<uintptr_t>(me.modBaseAddr),&p,exe};
                CloseHandle(ms); break;
            }
            CloseHandle(ms);
        }
        if(found.pid) break;
    } while(Process32NextW(snap,&pe));
    CloseHandle(snap);

    if(!found.pid){
        closeProcess(); status_={}; status_.message="Nenhum cliente com profile encontrado"; return false;
    }
    if(!process_ || status_.pid!=found.pid){
        closeProcess();
        process_=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ|PROCESS_VM_WRITE|PROCESS_VM_OPERATION,FALSE,found.pid);
        if(!process_){ status_={}; status_.processFound=true; status_.pid=found.pid; status_.exe=found.exe; status_.message="OpenProcess falhou"; return false; }
        memory_.set(process_);
    }
    runtime_.moduleBase=found.base; runtime_.profile=found.profile;
    status_.processFound=true; status_.memoryOpen=true; status_.pid=found.pid; status_.moduleBase=found.base; status_.exe=found.exe;
    status_.profileMatched=true; status_.profileName=found.profile->build.profileName;

    // Validate PE timestamp + image size in memory when possible.
    IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{};
    if(memory_.readT(found.base,dos) && dos.e_magic==IMAGE_DOS_SIGNATURE &&
       memory_.read(found.base+dos.e_lfanew,&nt,sizeof(nt)) && nt.Signature==IMAGE_NT_SIGNATURE){
        if(found.profile->build.timestamp && nt.FileHeader.TimeDateStamp!=found.profile->build.timestamp){
            status_.message="Processo encontrado, mas timestamp nao confere com o profile"; runtime_.profile=nullptr; return false;
        }
        if(found.profile->build.sizeOfImage && nt.OptionalHeader.SizeOfImage!=found.profile->build.sizeOfImage){
            status_.message="Processo encontrado, mas SizeOfImage nao confere com o profile"; runtime_.profile=nullptr; return false;
        }
    }
    return resolveRuntime();
#else
    status_.message="ClientSession requer Windows"; return false;
#endif
}

bool ClientSession::resolveRuntime(){
    if(!runtime_.profile||!runtime_.moduleBase) return false;
    const auto& p=*runtime_.profile;
    uintptr_t lp=0;
    const uintptr_t expectedVt = p.localPlayer.vtableRva ? runtime_.moduleBase + p.localPlayer.vtableRva : 0;
    auto looksLikeLocalPlayer = [&](uintptr_t obj)->bool {
        if (!obj) return false;
        if (!expectedVt) return obj > 0x10000;
        uintptr_t vt=0;
        return memory_.readT(obj,vt) && vt==expectedVt;
    };
    if(p.localPlayer.directGlobalRva){
        const uintptr_t globalAddr=runtime_.moduleBase+p.localPlayer.directGlobalRva;
        uintptr_t candidate=0;
        if(memory_.readT(globalAddr,candidate) && looksLikeLocalPlayer(candidate)) lp=candidate;
        // Some profiles can point directly to an object rather than to a pointer/shared_ptr slot.
        if(!lp && looksLikeLocalPlayer(globalAddr)) lp=globalAddr;
    }
    if(!lp && p.localPlayer.indirectGlobalRva){
        uintptr_t owner=0, candidate=0;
        if(memory_.readT(runtime_.moduleBase+p.localPlayer.indirectGlobalRva,owner) && owner &&
           memory_.readT(owner+p.localPlayer.indirectOwnerOffset,candidate) && looksLikeLocalPlayer(candidate)) {
            lp=candidate;
        }
    }
    runtime_.localPlayer=lp;
    status_.localPlayerResolved=lp!=0;
    status_.localPlayerVtableOk=false;
    if(lp && p.localPlayer.vtableRva){
        uintptr_t vt=0;
        if(memory_.readT(lp,vt)) status_.localPlayerVtableOk=(vt==runtime_.moduleBase+p.localPlayer.vtableRva);
    }
    auto resolveGlobal=[&](uint64_t rva)->uintptr_t{ uintptr_t v=0; if(rva) memory_.readT(runtime_.moduleBase+rva,v); return v; };
    runtime_.game=resolveGlobal(p.game.rva);
    runtime_.map=resolveGlobal(p.map.rva);
    status_.message=lp?"Runtime conectado":"Profile carregado; LocalPlayer ainda nao resolvido";
    return lp!=0;
}
}
