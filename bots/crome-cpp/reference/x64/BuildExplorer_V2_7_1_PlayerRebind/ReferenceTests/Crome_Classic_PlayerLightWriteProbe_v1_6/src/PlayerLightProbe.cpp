#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <vector>
#include <string>

namespace {
constexpr std::uint32_t EXPECTED_TIMESTAMP = 0x6AA3019A;
constexpr std::uint32_t EXPECTED_IMAGE_SIZE = 0x14C1000;
constexpr std::uintptr_t RVA_LOCALPLAYER_DIRECT   = 0x13EF528;
constexpr std::uintptr_t RVA_LOCALPLAYER_INDIRECT = 0x13EF550;
constexpr std::ptrdiff_t  OFF_INDIRECT_OWNER      = 0x1F8;
constexpr std::uintptr_t RVA_LOCALPLAYER_VTABLE   = 0xFBFD18;
constexpr std::ptrdiff_t OFF_X         = 0x30;
constexpr std::ptrdiff_t OFF_Y         = 0x34;
constexpr std::ptrdiff_t OFF_Z         = 0x38;
constexpr std::ptrdiff_t OFF_DIRECTION = 0x7C;
constexpr std::ptrdiff_t OFF_LIGHT_INTENSITY = 0xF1; // runtime: 0 -> 6 -> 8 -> 0

std::uintptr_t g_base=0;
std::uintptr_t g_lp=0;
std::uint8_t g_originalLight=0;
bool g_originalSaved=false;
int g_forceValue=-1; // -1 = nao forcar

bool readable(std::uintptr_t p, std::size_t n=1){
    if(!p||!n) return false; MEMORY_BASIC_INFORMATION mbi{};
    if(!VirtualQuery(reinterpret_cast<void*>(p),&mbi,sizeof(mbi))) return false;
    if(mbi.State!=MEM_COMMIT || (mbi.Protect&(PAGE_NOACCESS|PAGE_GUARD))) return false;
    auto e=p+n, re=reinterpret_cast<std::uintptr_t>(mbi.BaseAddress)+mbi.RegionSize;
    return e>=p && e<=re;
}
template<class T> bool readVal(std::uintptr_t p,T& out){ if(!readable(p,sizeof(T))) return false; std::memcpy(&out,reinterpret_cast<void*>(p),sizeof(T)); return true; }
std::uintptr_t readPtr(std::uintptr_t p){ std::uintptr_t v=0; readVal(p,v); return v; }

bool writeByte(std::uintptr_t p,std::uint8_t v){
    if(!readable(p,1)) return false;
    DWORD old=0;
    if(!VirtualProtect(reinterpret_cast<void*>(p),1,PAGE_READWRITE,&old)) return false;
    *reinterpret_cast<volatile std::uint8_t*>(p)=v;
    DWORD tmp=0; VirtualProtect(reinterpret_cast<void*>(p),1,old,&tmp);
    return true;
}

bool validateBuild(){
    auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(g_base);
    if(!readable(g_base,sizeof(*dos))||dos->e_magic!=IMAGE_DOS_SIGNATURE) return false;
    auto* nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(g_base+dos->e_lfanew);
    if(!readable(reinterpret_cast<std::uintptr_t>(nt),sizeof(*nt))||nt->Signature!=IMAGE_NT_SIGNATURE) return false;
    std::cout<<"PE Timestamp       : 0x"<<std::hex<<std::uppercase<<nt->FileHeader.TimeDateStamp<<"\n";
    std::cout<<"PE ImageSize       : 0x"<<nt->OptionalHeader.SizeOfImage<<std::dec<<"\n";
    return nt->FileHeader.TimeDateStamp==EXPECTED_TIMESTAMP && nt->OptionalHeader.SizeOfImage==EXPECTED_IMAGE_SIZE;
}

bool validLocalPlayer(std::uintptr_t p){
    if(!p||!readable(p,0x200)) return false;
    std::uintptr_t vt=0; std::uint32_t x=0,y=0,dir=99; std::uint8_t z=255;
    if(!readVal(p,vt)||!readVal(p+OFF_X,x)||!readVal(p+OFF_Y,y)||!readVal(p+OFF_Z,z)||!readVal(p+OFF_DIRECTION,dir)) return false;
    if(vt!=g_base+RVA_LOCALPLAYER_VTABLE) return false;
    if(x<1000||y<1000||z>15||dir>3) return false;
    return true;
}

bool resolveLocalPlayer(){
    std::vector<std::pair<std::string,std::uintptr_t>> c;
    const auto d=readPtr(g_base+RVA_LOCALPLAYER_DIRECT);
    c.push_back({"direct",d});
    const auto i=readPtr(g_base+RVA_LOCALPLAYER_INDIRECT);
    c.push_back({"indirect",i});
    if(i) c.push_back({"indirect+owner",readPtr(i+OFF_INDIRECT_OWNER)});
    for(auto& [name,p]:c){
        if(validLocalPlayer(p)){
            const bool changed=(g_lp!=p);
            g_lp=p;
            std::uint32_t x=0,y=0,dir=0; std::uint8_t z=0,light=0;
            readVal(p+OFF_X,x); readVal(p+OFF_Y,y); readVal(p+OFF_Z,z); readVal(p+OFF_DIRECTION,dir); readVal(p+OFF_LIGHT_INTENSITY,light);
            if(changed || !g_originalSaved){ g_originalLight=light; g_originalSaved=true; }
            std::cout<<"[PLAYER][OK] via "<<name<<" LocalPlayer=0x"<<std::hex<<std::uppercase<<p<<std::dec
                     <<" Pos="<<x<<","<<y<<","<<unsigned(z)<<" Dir="<<dir
                     <<" Light(+0xF1)="<<unsigned(light)<<" Original="<<unsigned(g_originalLight)<<"\n";
            return true;
        }
    }
    std::cout<<"[PLAYER][MISS] Nao consegui resolver LocalPlayer pelo profile.\n";
    return false;
}

void status(){
    if(!validLocalPlayer(g_lp)) resolveLocalPlayer();
    std::cout<<"\n[STATUS] LocalPlayer=0x"<<std::hex<<std::uppercase<<g_lp<<std::dec;
    if(!validLocalPlayer(g_lp)){ std::cout<<" INVALID\n"; return; }
    std::uint32_t x=0,y=0,d=0; std::uint8_t z=0,light=0;
    readVal(g_lp+OFF_X,x);readVal(g_lp+OFF_Y,y);readVal(g_lp+OFF_Z,z);readVal(g_lp+OFF_DIRECTION,d);readVal(g_lp+OFF_LIGHT_INTENSITY,light);
    std::cout<<" Pos="<<x<<","<<y<<","<<unsigned(z)<<" Dir="<<d
             <<" Light(+0xF1)="<<unsigned(light)<<" Force=";
    if(g_forceValue<0) std::cout<<"OFF"; else std::cout<<g_forceValue;
    std::cout<<" Original="<<unsigned(g_originalLight)<<"\n";
}

void setForce(int v,const char* tag){
    if(!validLocalPlayer(g_lp) && !resolveLocalPlayer()) return;
    if(!g_originalSaved){ readVal(g_lp+OFF_LIGHT_INTENSITY,g_originalLight); g_originalSaved=true; }
    g_forceValue=v;
    if(writeByte(g_lp+OFF_LIGHT_INTENSITY,static_cast<std::uint8_t>(v)))
        std::cout<<"[LIGHT]["<<tag<<"] Forcando LocalPlayer+0xF1 = "<<v<<" continuamente.\n";
    else
        std::cout<<"[LIGHT]["<<tag<<"][FAIL] escrita em +0xF1 falhou.\n";
}

void restore(){
    g_forceValue=-1;
    if(validLocalPlayer(g_lp) && g_originalSaved && writeByte(g_lp+OFF_LIGHT_INTENSITY,g_originalLight))
        std::cout<<"[LIGHT][RESTORE] +0xF1 restaurado para "<<unsigned(g_originalLight)<<".\n";
    else
        std::cout<<"[LIGHT][RESTORE] nada para restaurar ou LocalPlayer invalido.\n";
}

DWORD WINAPI MainThread(void*){
    AllocConsole(); FILE* f{}; freopen_s(&f,"CONOUT$","w",stdout); freopen_s(&f,"CONOUT$","w",stderr);
    SetConsoleTitleW(L"Crome Classic PlayerLight WriteProbe v1.6");
    g_base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    std::cout<<"============================================================\n";
    std::cout<<" CROME CLASSIC PLAYER LIGHT WRITE PROBE v1.6\n";
    std::cout<<"============================================================\n";
    std::cout<<"Profile              : 6AA3019A / 602E746BAE706D6C\n";
    std::cout<<"ModuleBase           : 0x"<<std::hex<<std::uppercase<<g_base<<std::dec<<"\n";
    std::cout<<"LocalPlayer direct   : RVA 0x13EF528\n";
    std::cout<<"LocalPlayer indirect : RVA 0x13EF550 owner +0x1F8\n";
    std::cout<<"LocalPlayer vtable   : RVA 0xFBFD18\n";
    std::cout<<"Direction            : +0x7C\n";
    std::cout<<"Light candidate      : +0xF1 (u8)\n";
    std::cout<<"Observed runtime     : BASE 0 -> LUX 6 -> GRAN LUX 8 -> BASE 0\n\n";
    if(!validateBuild()){
        std::cout<<"[STOP] Fingerprint nao confere. Nenhuma escrita sera feita.\n"; Sleep(1500);
        FreeConsole(); FreeLibraryAndExitThread(GetModuleHandleW(L"CromeClassicPlayerLightProbe.dll"),0);
    }
    resolveLocalPlayer(); status();
    std::cout<<"\nTECLAS:\n";
    std::cout<<"  F5  = status / resolver LocalPlayer\n";
    std::cout<<"  F6  = forcar Light 6  (valor observado: utevo lux)\n";
    std::cout<<"  F7  = forcar Light 8  (valor observado: utevo gran lux)\n";
    std::cout<<"  F8  = forcar Light 15 (teste acima do gran lux)\n";
    std::cout<<"  F9  = restaurar valor original e parar force\n";
    std::cout<<"  END = restaurar e descarregar\n\n";

    while(true){
        if(GetAsyncKeyState(VK_F5)&1){ resolveLocalPlayer(); status(); }
        if(GetAsyncKeyState(VK_F6)&1){ setForce(6,"LUX6"); }
        if(GetAsyncKeyState(VK_F7)&1){ setForce(8,"GRAN8"); }
        if(GetAsyncKeyState(VK_F8)&1){ setForce(15,"TEST15"); }
        if(GetAsyncKeyState(VK_F9)&1){ restore(); status(); }
        if(GetAsyncKeyState(VK_END)&1) break;
        if(g_forceValue>=0){
            if(!validLocalPlayer(g_lp)) resolveLocalPlayer();
            if(validLocalPlayer(g_lp)) writeByte(g_lp+OFF_LIGHT_INTENSITY,static_cast<std::uint8_t>(g_forceValue));
        }
        Sleep(50);
    }
    restore();
    std::cout<<"[END] Valor restaurado; probe encerrado.\n"; Sleep(250);
    FreeConsole(); FreeLibraryAndExitThread(GetModuleHandleW(L"CromeClassicPlayerLightProbe.dll"),0); return 0;
}
}

BOOL APIENTRY DllMain(HMODULE h,DWORD reason,LPVOID){ if(reason==DLL_PROCESS_ATTACH){ DisableThreadLibraryCalls(h); HANDLE t=CreateThread(nullptr,0,MainThread,nullptr,0,nullptr); if(t) CloseHandle(t);} return TRUE; }
