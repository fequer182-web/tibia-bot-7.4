#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <vector>
#include <algorithm>
#include <cmath>
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
constexpr std::size_t SCAN_SIZE = 0x1200;

std::uintptr_t g_base=0;
std::uintptr_t g_lp=0;
std::vector<std::uint8_t> g_baseSnap, g_luxSnap, g_granSnap, g_finalSnap;
bool g_haveBase=false,g_haveLux=false,g_haveGran=false,g_haveFinal=false;

bool readable(std::uintptr_t p, std::size_t n=1){
    if(!p||!n) return false; MEMORY_BASIC_INFORMATION mbi{};
    if(!VirtualQuery(reinterpret_cast<void*>(p),&mbi,sizeof(mbi))) return false;
    if(mbi.State!=MEM_COMMIT || (mbi.Protect&(PAGE_NOACCESS|PAGE_GUARD))) return false;
    auto e=p+n, re=reinterpret_cast<std::uintptr_t>(mbi.BaseAddress)+mbi.RegionSize;
    return e>=p && e<=re;
}
template<class T> bool readVal(std::uintptr_t p,T& out){ if(!readable(p,sizeof(T))) return false; std::memcpy(&out,reinterpret_cast<void*>(p),sizeof(T)); return true; }
std::uintptr_t readPtr(std::uintptr_t p){ std::uintptr_t v=0; readVal(p,v); return v; }

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
    if(!p||!readable(p,0x100)) return false;
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
            g_lp=p;
            std::uint32_t x=0,y=0,dir=0; std::uint8_t z=0;
            readVal(p+OFF_X,x); readVal(p+OFF_Y,y); readVal(p+OFF_Z,z); readVal(p+OFF_DIRECTION,dir);
            std::cout<<"[PLAYER][OK] via "<<name<<" LocalPlayer=0x"<<std::hex<<std::uppercase<<p<<std::dec
                     <<" Pos="<<x<<","<<y<<","<<unsigned(z)<<" Dir="<<dir<<"\n";
            return true;
        }
    }
    std::cout<<"[PLAYER][MISS] Nao consegui resolver LocalPlayer pelo profile.\n";
    return false;
}

bool snapshot(std::vector<std::uint8_t>& out,const char* tag){
    if(!validLocalPlayer(g_lp) && !resolveLocalPlayer()) return false;
    out.resize(SCAN_SIZE);
    SIZE_T got=0;
    if(!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(g_lp),out.data(),out.size(),&got)||got!=out.size()){
        std::cout<<"["<<tag<<"][FAIL] snapshot incompleto: "<<got<<" bytes\n"; return false;
    }
    std::uint32_t dir=0; readVal(g_lp+OFF_DIRECTION,dir);
    std::cout<<"["<<tag<<"][OK] "<<out.size()<<" bytes de LocalPlayer, Dir="<<dir<<"\n";
    return true;
}

struct Cand { std::size_t off; int b,l,g,f; int score; bool monotonic; bool returned; };
void analyze(){
    if(!g_haveBase||!g_haveLux||!g_haveGran){
        std::cout<<"[ANALYZE][FAIL] Precisa BASE + LUX + GRAN LUX.\n"; return;
    }
    std::vector<Cand> cs;
    for(std::size_t i=0;i<SCAN_SIZE;i++){
        const int b=g_baseSnap[i], l=g_luxSnap[i], g=g_granSnap[i];
        const int f=g_haveFinal?g_finalSnap[i]:-1;
        if(b==l && l==g) continue;
        const bool monoUp=(b<l && l<g), monoDown=(b>l && l>g);
        const bool mono=monoUp||monoDown;
        const bool returned=g_haveFinal && f==b;
        int score=0;
        if(mono) score+=100;
        if(returned) score+=80;
        if(b!=l) score+=20;
        if(l!=g) score+=20;
        if(std::abs(g-b)>=2) score+=10;
        // Light intensity costuma ser byte pequeno/medio; nao e regra, apenas ranking.
        if(b<=255&&l<=255&&g<=255) score+=5;
        cs.push_back({i,b,l,g,f,score,mono,returned});
    }
    std::sort(cs.begin(),cs.end(),[](const Cand&a,const Cand&b){return a.score>b.score;});
    std::cout<<"\n[ANALYZE] candidatos u8 em LocalPlayer +0x000..+0x"<<std::hex<<std::uppercase<<SCAN_SIZE-1<<std::dec<<"\n";
    std::cout<<"  Regra forte: BASE -> LUX -> GRAN LUX, preferindo monotonia e retorno ao BASE.\n";
    int shown=0;
    for(const auto& c:cs){
        if(c.score<40) break;
        std::cout<<"  #"<<std::setw(2)<<std::setfill('0')<<(shown+1)<<std::setfill(' ')
                 <<" off=+0x"<<std::hex<<std::uppercase<<c.off<<std::dec
                 <<"  "<<c.b<<" -> "<<c.l<<" -> "<<c.g;
        if(g_haveFinal) std::cout<<" -> "<<c.f;
        std::cout<<"  score="<<c.score;
        if(c.monotonic) std::cout<<" MONOTONIC";
        if(c.returned) std::cout<<" RETURNED";
        std::cout<<"\n";
        if(++shown>=40) break;
    }
    if(!shown) std::cout<<"  Nenhum candidato forte. Repita com estados mais distintos.\n";

    // Mostra pares de bytes vizinhos que mudaram juntos: possivel intensity/color.
    std::cout<<"\n[PAIRS] pares vizinhos que mudaram BASE/LUX/GRAN:\n";
    int pairs=0;
    for(std::size_t i=0;i+1<SCAN_SIZE && pairs<25;i++){
        bool a=(g_baseSnap[i]!=g_luxSnap[i]||g_luxSnap[i]!=g_granSnap[i]);
        bool b=(g_baseSnap[i+1]!=g_luxSnap[i+1]||g_luxSnap[i+1]!=g_granSnap[i+1]);
        if(!a||!b) continue;
        std::cout<<"  +0x"<<std::hex<<std::uppercase<<i<<std::dec
                 <<": ["<<int(g_baseSnap[i])<<","<<int(g_baseSnap[i+1])<<"] -> ["
                 <<int(g_luxSnap[i])<<","<<int(g_luxSnap[i+1])<<"] -> ["
                 <<int(g_granSnap[i])<<","<<int(g_granSnap[i+1])<<"]\n";
        pairs++;
    }
}

void status(){
    std::cout<<"\n[STATUS] LocalPlayer=0x"<<std::hex<<std::uppercase<<g_lp<<std::dec
             <<" BASE="<<g_haveBase<<" LUX="<<g_haveLux<<" GRAN="<<g_haveGran<<" FINAL="<<g_haveFinal<<"\n";
    if(validLocalPlayer(g_lp)){
        std::uint32_t x=0,y=0,d=0; std::uint8_t z=0;
        readVal(g_lp+OFF_X,x);readVal(g_lp+OFF_Y,y);readVal(g_lp+OFF_Z,z);readVal(g_lp+OFF_DIRECTION,d);
        std::cout<<"  Pos="<<x<<","<<y<<","<<unsigned(z)<<" Dir="<<d<<"\n";
    }
}

DWORD WINAPI MainThread(void*){
    AllocConsole(); FILE* f{}; freopen_s(&f,"CONOUT$","w",stdout); freopen_s(&f,"CONOUT$","w",stderr);
    SetConsoleTitleW(L"Crome Classic PlayerLightProbe v1.5");
    g_base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    std::cout<<"============================================================\n";
    std::cout<<" CROME CLASSIC PLAYER LIGHT PROBE v1.5 - LUX / GRAN LUX\n";
    std::cout<<"============================================================\n";
    std::cout<<"Profile              : 6AA3019A / 602E746BAE706D6C\n";
    std::cout<<"ModuleBase           : 0x"<<std::hex<<std::uppercase<<g_base<<std::dec<<"\n";
    std::cout<<"LocalPlayer direct   : RVA 0x13EF528\n";
    std::cout<<"LocalPlayer indirect : RVA 0x13EF550 owner +0x1F8\n";
    std::cout<<"LocalPlayer vtable   : RVA 0xFBFD18\n";
    std::cout<<"Direction            : +0x7C (0=N 1=E 2=S 3=W)\n";
    std::cout<<"Scan                 : LocalPlayer +0x000..+0x11FF (u8)\n\n";
    if(!validateBuild()){
        std::cout<<"[STOP] Fingerprint nao confere. Nenhum teste sera feito.\n"; Sleep(1500);
        FreeConsole(); FreeLibraryAndExitThread(GetModuleHandleW(L"CromeClassicPlayerLightProbe.dll"),0);
    }
    resolveLocalPlayer(); status();
    std::cout<<"\nORDEM DO TESTE:\n";
    std::cout<<"  1) F5  = BASE sem magia de luz\n";
    std::cout<<"  2) fale 'utevo lux' e aperte F6 enquanto a luz esta ativa\n";
    std::cout<<"  3) fale 'utevo gran lux' e aperte F7 enquanto a luz maior esta ativa\n";
    std::cout<<"  4) espere voltar ao estado inicial e aperte F8 (opcional, melhora o filtro)\n";
    std::cout<<"  5) F9  = analisar bytes que mudaram\n";
    std::cout<<"  F10 = status / resolver LocalPlayer novamente\n";
    std::cout<<"  END = descarregar\n\n";
    while(true){
        if(GetAsyncKeyState(VK_F5)&1){ g_haveBase=snapshot(g_baseSnap,"BASE"); }
        if(GetAsyncKeyState(VK_F6)&1){ g_haveLux=snapshot(g_luxSnap,"LUX"); }
        if(GetAsyncKeyState(VK_F7)&1){ g_haveGran=snapshot(g_granSnap,"GRAN"); }
        if(GetAsyncKeyState(VK_F8)&1){ g_haveFinal=snapshot(g_finalSnap,"FINAL"); }
        if(GetAsyncKeyState(VK_F9)&1){ analyze(); }
        if(GetAsyncKeyState(VK_F10)&1){ resolveLocalPlayer(); status(); }
        if(GetAsyncKeyState(VK_END)&1) break;
        Sleep(50);
    }
    std::cout<<"[END] Probe somente leitura encerrado.\n"; Sleep(250);
    FreeConsole(); FreeLibraryAndExitThread(GetModuleHandleW(L"CromeClassicPlayerLightProbe.dll"),0); return 0;
}
}

BOOL APIENTRY DllMain(HMODULE h,DWORD reason,LPVOID){ if(reason==DLL_PROCESS_ATTACH){ DisableThreadLibraryCalls(h); HANDLE t=CreateThread(nullptr,0,MainThread,nullptr,0,nullptr); if(t) CloseHandle(t);} return TRUE; }
