#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <algorithm>
#include <atomic>
#include <limits>
#include <cmath>

namespace {
constexpr std::uintptr_t RVA_GAME_LOOK = 0x164090;
constexpr std::uintptr_t RVA_LOCALPLAYER_GLOBAL = 0x13EF528;
constexpr std::ptrdiff_t ITEM_ID_OFF = 0x40;
constexpr std::ptrdiff_t POS_X_OFF = 0x30;
constexpr std::ptrdiff_t POS_Y_OFF = 0x34;
constexpr std::ptrdiff_t POS_Z_OFF = 0x38;
constexpr std::size_t LOOK_PATCH_LEN = 18;

std::uintptr_t g_base = 0;
std::size_t g_imageSize = 0;
std::uintptr_t g_lookTarget = 0;
void* g_lookTrampoline = nullptr;
std::uint8_t g_lookSaved[LOOK_PATCH_LEN]{};
bool g_lookHooked = false;
std::atomic<unsigned long long> g_lookCalls{0};

struct LightAccessor {
    std::uintptr_t getter = 0;
    std::uintptr_t setter = 0;
    std::int32_t viewOffset = 0;
    std::int32_t lightOffset = 0;
};
struct LightBinding {
    LightAccessor accessor{};
    std::uintptr_t uiMap = 0;
    std::uintptr_t mapView = 0;
    float* lightPtr = nullptr;
    float saved = 0.0f;
    bool savedValid = false;
    bool valid = false;
};
LightBinding g_light{};

using GenericLookFn = std::uintptr_t(*)(void*, void*, void*, void*);
GenericLookFn g_originalLook = nullptr;

bool readable(std::uintptr_t p, std::size_t n=1) {
    if (!p || !n) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS|PAGE_GUARD)) return false;
    const auto end = p+n;
    const auto regionEnd = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress)+mbi.RegionSize;
    return end>=p && end<=regionEnd;
}

bool writable(std::uintptr_t p, std::size_t n=1) {
    if (!readable(p,n)) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi));
    const DWORD prot = mbi.Protect & 0xFF;
    return prot==PAGE_READWRITE || prot==PAGE_WRITECOPY || prot==PAGE_EXECUTE_READWRITE || prot==PAGE_EXECUTE_WRITECOPY;
}

bool executable(std::uintptr_t p) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS|PAGE_GUARD))) return false;
    const DWORD x = mbi.Protect & 0xFF;
    return x==PAGE_EXECUTE || x==PAGE_EXECUTE_READ || x==PAGE_EXECUTE_READWRITE || x==PAGE_EXECUTE_WRITECOPY;
}

template<class T> bool readVal(std::uintptr_t p, T& out) {
    if(!readable(p,sizeof(T))) return false;
    std::memcpy(&out,reinterpret_cast<void*>(p),sizeof(T)); return true;
}
std::uintptr_t readPtr(std::uintptr_t p) { std::uintptr_t v=0; readVal(p,v); return v; }

void dumpBytes(const char* name, std::uintptr_t p, std::size_t n=32) {
    std::cout << name << " @ 0x" << std::hex << std::uppercase << p << std::dec << "\n  ";
    if (!readable(p,n)) { std::cout << "<unreadable>\n"; return; }
    const auto* b=reinterpret_cast<const std::uint8_t*>(p);
    for (std::size_t i=0;i<n;i++) std::cout << std::hex << std::setw(2) << std::setfill('0') << (unsigned)b[i] << ' ';
    std::cout << std::dec << "\n";
}

bool getImageInfo() {
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_base);
    if(!readable(g_base,sizeof(*dos)) || dos->e_magic!=IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
    if(!readable(reinterpret_cast<std::uintptr_t>(nt),sizeof(*nt)) || nt->Signature!=IMAGE_NT_SIGNATURE) return false;
    g_imageSize = nt->OptionalHeader.SizeOfImage;
    return true;
}

std::vector<std::pair<std::uintptr_t,std::size_t>> executableSections() {
    std::vector<std::pair<std::uintptr_t,std::size_t>> out;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;i++) {
        if(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) {
            const auto size = std::max<std::uint32_t>(sec[i].Misc.VirtualSize,sec[i].SizeOfRawData);
            out.push_back({g_base+sec[i].VirtualAddress,size});
        }
    }
    return out;
}

void makeAbsJump(std::uint8_t* out, std::uintptr_t target) {
    out[0]=0x48; out[1]=0xB8;
    std::memcpy(out+2,&target,sizeof(target));
    out[10]=0xFF; out[11]=0xE0;
}

void showThing(std::uintptr_t thing, const char* route) {
    if(!thing || !readable(thing+ITEM_ID_OFF,4)) return;
    std::uint32_t id=0,x=0,y=0; std::uint8_t z=0;
    readVal(thing+ITEM_ID_OFF,id);
    readVal(thing+POS_X_OFF,x); readVal(thing+POS_Y_OFF,y); readVal(thing+POS_Z_OFF,z);
    if(id==0 || id>1000000) return;
    std::cout << "    " << route << " Thing=0x" << std::hex << std::uppercase << thing << std::dec
              << " ID=" << id << " Pos=" << x << "," << y << "," << (unsigned)z;
    if(x==0xFFFFu) std::cout << " <CONTAINER/INVENTORY>";
    else if(x>=1000 && y>=1000 && z<=15) std::cout << " <GROUND?>";
    std::cout << "\n";
}

void inspectLookArg(const char* name, std::uintptr_t p) {
    std::cout << "  " << name << "=0x" << std::hex << std::uppercase << p << std::dec << "\n";
    showThing(p,name);
    const auto p1=readPtr(p);
    if(p1) { std::string s="["+std::string(name)+"]"; showThing(p1,s.c_str()); }
    const auto p2=readPtr(p1);
    if(p2) { std::string s="[["+std::string(name)+"]]"; showThing(p2,s.c_str()); }
}

std::uintptr_t hookLook(void* a1, void* a2, void* a3, void* a4) {
    const auto n=++g_lookCalls;
    const auto rcx=reinterpret_cast<std::uintptr_t>(a1);
    const auto rdx=reinterpret_cast<std::uintptr_t>(a2);
    const auto r8 =reinterpret_cast<std::uintptr_t>(a3);
    const auto r9 =reinterpret_cast<std::uintptr_t>(a4);
    std::cout << "\n------------------------------------------------------------\n";
    std::cout << "[LOOK #" << n << "] Game::look fired\n";
    inspectLookArg("RCX",rcx); inspectLookArg("RDX",rdx); inspectLookArg("R8",r8); inspectLookArg("R9",r9);
    std::cout.flush();
    return g_originalLook ? g_originalLook(a1,a2,a3,a4) : 0;
}

bool installLookHook() {
    static const std::uint8_t expected[LOOK_PATCH_LEN] = {
        0x48,0x89,0x5C,0x24,0x18,
        0x48,0x89,0x74,0x24,0x20,
        0x57,
        0x48,0x81,0xEC,0x90,0x00,0x00,0x00
    };
    if(!readable(g_lookTarget,LOOK_PATCH_LEN) || !executable(g_lookTarget)) return false;
    if(std::memcmp(reinterpret_cast<void*>(g_lookTarget),expected,LOOK_PATCH_LEN)!=0) {
        std::cout << "[LOOK] Signature mismatch. Nenhum patch foi aplicado.\n";
        dumpBytes("Game::look atual",g_lookTarget,32); return false;
    }
    std::memcpy(g_lookSaved,reinterpret_cast<void*>(g_lookTarget),LOOK_PATCH_LEN);
    auto* tramp=reinterpret_cast<std::uint8_t*>(VirtualAlloc(nullptr,LOOK_PATCH_LEN+16,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE));
    if(!tramp) return false;
    std::memcpy(tramp,g_lookSaved,LOOK_PATCH_LEN);
    makeAbsJump(tramp+LOOK_PATCH_LEN,g_lookTarget+LOOK_PATCH_LEN);
    for(std::size_t i=LOOK_PATCH_LEN+12;i<LOOK_PATCH_LEN+16;i++) tramp[i]=0x90;
    FlushInstructionCache(GetCurrentProcess(),tramp,LOOK_PATCH_LEN+16);
    g_lookTrampoline=tramp; g_originalLook=reinterpret_cast<GenericLookFn>(tramp);
    std::uint8_t patch[LOOK_PATCH_LEN]{}; std::memset(patch,0x90,sizeof(patch));
    makeAbsJump(patch,reinterpret_cast<std::uintptr_t>(&hookLook));
    DWORD old=0;
    if(!VirtualProtect(reinterpret_cast<void*>(g_lookTarget),LOOK_PATCH_LEN,PAGE_EXECUTE_READWRITE,&old)) return false;
    std::memcpy(reinterpret_cast<void*>(g_lookTarget),patch,LOOK_PATCH_LEN);
    FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<void*>(g_lookTarget),LOOK_PATCH_LEN);
    DWORD tmp=0; VirtualProtect(reinterpret_cast<void*>(g_lookTarget),LOOK_PATCH_LEN,old,&tmp);
    g_lookHooked=true; return true;
}

void uninstallLookHook() {
    if(g_lookHooked) {
        DWORD old=0;
        if(VirtualProtect(reinterpret_cast<void*>(g_lookTarget),LOOK_PATCH_LEN,PAGE_EXECUTE_READWRITE,&old)) {
            std::memcpy(reinterpret_cast<void*>(g_lookTarget),g_lookSaved,LOOK_PATCH_LEN);
            FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<void*>(g_lookTarget),LOOK_PATCH_LEN);
            DWORD tmp=0; VirtualProtect(reinterpret_cast<void*>(g_lookTarget),LOOK_PATCH_LEN,old,&tmp);
        }
        g_lookHooked=false;
    }
    if(g_lookTrampoline) { VirtualFree(g_lookTrampoline,0,MEM_RELEASE); g_lookTrampoline=nullptr; g_originalLook=nullptr; }
}

std::vector<LightAccessor> findLightAccessors() {
    struct G { std::uintptr_t fn; std::int32_t v,l; };
    struct S { std::uintptr_t fn; std::int32_t v,l; };
    std::vector<G> gs; std::vector<S> ss;
    for(auto [start,size]: executableSections()) {
        if(!readable(start,std::min<std::size_t>(size,32))) continue;
        const auto end=start+size;
        for(auto p=start; p+16<=end; ++p) {
            if(!readable(p,16)) continue;
            const auto* b=reinterpret_cast<const std::uint8_t*>(p);
            // getter: mov rax,[rcx+view]; movss xmm0,[rax+light]; ret
            if(b[0]==0x48&&b[1]==0x8B&&b[2]==0x81&&b[7]==0xF3&&b[8]==0x0F&&b[9]==0x10&&b[10]==0x80&&b[15]==0xC3) {
                std::int32_t v=0,l=0; std::memcpy(&v,b+3,4); std::memcpy(&l,b+11,4);
                if(v>0&&v<0x2000&&l>=0&&l<0x4000) gs.push_back({p,v,l});
            }
            // setter: mov rax,[rcx+view]; movss [rax+light],xmm1; ret
            if(b[0]==0x48&&b[1]==0x8B&&b[2]==0x81&&b[7]==0xF3&&b[8]==0x0F&&b[9]==0x11&&b[10]==0x88&&b[15]==0xC3) {
                std::int32_t v=0,l=0; std::memcpy(&v,b+3,4); std::memcpy(&l,b+11,4);
                if(v>0&&v<0x2000&&l>=0&&l<0x4000) ss.push_back({p,v,l});
            }
        }
    }
    std::vector<LightAccessor> out;
    for(const auto& g:gs) for(const auto& s:ss) if(g.v==s.v&&g.l==s.l) {
        out.push_back({g.fn,s.fn,g.v,g.l});
    }
    std::sort(out.begin(),out.end(),[](const auto&a,const auto&b){return a.getter<b.getter;});
    out.erase(std::unique(out.begin(),out.end(),[](const auto&a,const auto&b){return a.getter==b.getter&&a.setter==b.setter;}),out.end());
    return out;
}

std::vector<std::uintptr_t> findBytesInImage(const void* needle,std::size_t n,std::size_t limit=128) {
    std::vector<std::uintptr_t> out;
    const auto* nd=reinterpret_cast<const std::uint8_t*>(needle);
    for(std::uintptr_t p=g_base;p+n<=g_base+g_imageSize && out.size()<limit;p++) {
        if(!readable(p,n)) continue;
        if(std::memcmp(reinterpret_cast<void*>(p),nd,n)==0) out.push_back(p);
    }
    return out;
}

std::vector<std::uintptr_t> findUiMapVtables() {
    const char name[]=".?AVUIMap@@";
    auto names=findBytesInImage(name,sizeof(name)-1,8);
    std::vector<std::uintptr_t> vtables;
    for(auto str:names) {
        if(str<g_base+16) continue;
        const auto tdAddr=str-16;
        if(tdAddr<g_base || tdAddr-g_base>0xffffffffull) continue;
        const std::uint32_t tdRva=static_cast<std::uint32_t>(tdAddr-g_base);
        auto tdRefs=findBytesInImage(&tdRva,sizeof(tdRva),64);
        for(auto tdRef:tdRefs) {
            if(tdRef<g_base+12) continue;
            const auto colAddr=tdRef-12;
            auto colRefs=findBytesInImage(&colAddr,sizeof(colAddr),64);
            for(auto colRef:colRefs) vtables.push_back(colRef+sizeof(void*));
        }
    }
    std::sort(vtables.begin(),vtables.end()); vtables.erase(std::unique(vtables.begin(),vtables.end()),vtables.end());
    return vtables;
}

std::uintptr_t findUiMapObject(const std::vector<std::uintptr_t>& vtables, const LightAccessor& a) {
    if(vtables.empty()) return 0;
    SYSTEM_INFO si{}; GetSystemInfo(&si);
    auto minA=reinterpret_cast<std::uintptr_t>(si.lpMinimumApplicationAddress);
    auto maxA=reinterpret_cast<std::uintptr_t>(si.lpMaximumApplicationAddress);
    std::uintptr_t anchor=readPtr(g_base+RVA_LOCALPLAYER_GLOBAL);
    std::uintptr_t p=minA,end=maxA;
#ifdef _WIN64
    if(anchor) {
        constexpr std::uintptr_t span=0x40000000ull;
        p=anchor>span?anchor-span:minA; if(p<minA)p=minA;
        end=anchor<=maxA-span?anchor+span:maxA;
    }
#endif
    const DWORD started=GetTickCount();
    std::vector<std::uintptr_t> block(8192);
    while(p<end && GetTickCount()-started<5000) {
        MEMORY_BASIC_INFORMATION mbi{};
        if(!VirtualQuery(reinterpret_cast<void*>(p),&mbi,sizeof(mbi))) break;
        const auto next=p+mbi.RegionSize;
        const DWORD prot=mbi.Protect&0xFF;
        if(mbi.State==MEM_COMMIT && !(mbi.Protect&(PAGE_GUARD|PAGE_NOACCESS)) && (prot==PAGE_READWRITE||prot==PAGE_WRITECOPY)) {
            const auto first=reinterpret_cast<std::uintptr_t>(mbi.BaseAddress), last=first+mbi.RegionSize;
            for(auto chunk=first;chunk<last && GetTickCount()-started<5000;chunk+=block.size()*sizeof(void*)) {
                const auto bytes=std::min<std::size_t>(last-chunk,block.size()*sizeof(void*));
                if(!readable(chunk,bytes)) continue;
                std::memcpy(block.data(),reinterpret_cast<void*>(chunk),bytes);
                for(std::size_t i=0;i<bytes/sizeof(void*);++i) {
                    if(std::find(vtables.begin(),vtables.end(),block[i])==vtables.end()) continue;
                    const auto obj=chunk+i*sizeof(void*);
                    const auto view=readPtr(obj+a.viewOffset);
                    if(!view || !readable(view+a.lightOffset,sizeof(float))) continue;
                    float f=0; readVal(view+a.lightOffset,f);
                    if(std::isfinite(f) && f>=0.0f && f<=1.5f) return obj;
                }
            }
        }
        if(next<=p) break; p=next;
    }
    return 0;
}

bool discoverLight() {
    g_light={};
    // Confirmado offline no Classico.exe:
    // UIMap::setMinimumAmbientLight wrapper RVA 0x19D130
    // UIMap::getMinimumAmbientLight wrapper RVA 0x19D3C0
    // UIMap + 0x4E8 -> MapView*
    // MapView + 0x2F8 -> float minimumAmbientLight
    // MapView + 0x2F4 -> bool drawLights
    const LightAccessor a{g_base+0x19D3C0, g_base+0x19D130, 0x4E8, 0x2F8};
    std::cout << "\n[LIGHT] Layout offline confirmado para esta build.\n";
    std::cout << "[LIGHT] getter RVA=0x19D3C0 setter RVA=0x19D130 UIMap->MapView=+0x4E8 ambient=+0x2F8 drawLights=+0x2F4\n";
    auto vts=findUiMapVtables();
    std::cout << "[LIGHT] UIMap vtables candidatos: " << vts.size() << "\n";
    const auto ui=findUiMapObject(vts,a);
    if(!ui) {
        std::cout << "[LIGHT][FAIL] Layout confirmado, mas nao localizei a instancia UIMap ativa.\n";
        return false;
    }
    const auto view=readPtr(ui+a.viewOffset);
    float cur=0; if(!readVal(view+a.lightOffset,cur)) {
        std::cout << "[LIGHT][FAIL] MapView encontrado, ambient field ilegivel.\n";
        return false;
    }
    std::uint8_t draw=0; readVal(view+0x2F4,draw);
    g_light.accessor=a; g_light.uiMap=ui; g_light.mapView=view;
    g_light.lightPtr=reinterpret_cast<float*>(view+a.lightOffset);
    g_light.saved=cur; g_light.savedValid=true; g_light.valid=true;
    std::cout << "[LIGHT][OK] UIMap=0x" << std::hex << ui << " MapView=0x" << view
              << " ambientPtr=0x" << reinterpret_cast<std::uintptr_t>(g_light.lightPtr) << std::dec
              << " ambient=" << cur << " drawLights(byte)=" << (unsigned)draw << "\n";
    return true;
}

bool setLight(float v) {
    if(!g_light.valid && !discoverLight()) return false;
    const auto p=reinterpret_cast<std::uintptr_t>(g_light.lightPtr);
    if(!writable(p,sizeof(float))) { std::cout << "[LIGHT][FAIL] Campo nao e gravavel.\n"; return false; }
    if(!g_light.savedValid) { readVal(p,g_light.saved); g_light.savedValid=true; }
    std::memcpy(reinterpret_cast<void*>(p),&v,sizeof(v));
    float now=0; readVal(p,now);
    std::cout << "[LIGHT] aplicado=" << now << " (original salvo=" << g_light.saved << ")\n";
    return true;
}

void restoreLight() {
    if(!g_light.valid || !g_light.savedValid || !g_light.lightPtr) return;
    const auto p=reinterpret_cast<std::uintptr_t>(g_light.lightPtr);
    if(writable(p,sizeof(float))) {
        std::memcpy(reinterpret_cast<void*>(p),&g_light.saved,sizeof(g_light.saved));
        std::cout << "[LIGHT] restaurado para " << g_light.saved << "\n";
    }
    g_light.savedValid=false;
}

DWORD WINAPI MainThread(void*) {
    AllocConsole(); FILE* f{}; freopen_s(&f,"CONOUT$","w",stdout); freopen_s(&f,"CONOUT$","w",stderr);
    SetConsoleTitleW(L"Crome Classic FeatureProbe v1.2 - Look + Lux Offline");
    g_base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if(!getImageInfo()) { std::cout<<"[FAIL] PE image invalida.\n"; return 0; }
    g_lookTarget=g_base+RVA_GAME_LOOK;

    std::cout << "============================================================\n";
    std::cout << "      CROME CLASSIC FEATURE PROBE v1.2 - LOOK + LUX OFFLINE\n";
    std::cout << "============================================================\n";
    std::cout << "Build fingerprint : 6AA3019A / 602E746BAE706D6C\n";
    std::cout << "ModuleBase        : 0x" << std::hex << std::uppercase << g_base << std::dec << "\n";
    std::cout << "ImageSize         : 0x" << std::hex << g_imageSize << std::dec << "\n";
    std::cout << "Game::look RVA    : 0x164090\n";
    std::cout << "Item ID candidate : Thing + 0x40\n";
    std::cout << "Position candidate: +0x30/+0x34/+0x38\n\n";
    dumpBytes("Game::look BEFORE",g_lookTarget,32);
    if(installLookHook()) {
        std::cout << "[LOOK][OK] Hook instalado. De Look em item no chao e item na backpack.\n";
        dumpBytes("Game::look AFTER",g_lookTarget,24);
    } else std::cout << "[LOOK][FAIL] Hook nao instalado. Light ainda pode ser testado.\n";

    std::cout << "\nTECLAS:\n";
    std::cout << "  F6 = descobrir/vincular Lux (ambient light)\n";
    std::cout << "  F7 = Full Light ON (1.0)\n";
    std::cout << "  F8 = restaurar Light original\n";
    std::cout << "  F9 = dump Game::look atual\n";
    std::cout << "  END = restaurar tudo e descarregar\n\n";

    while(true) {
        if(GetAsyncKeyState(VK_F6)&1) discoverLight();
        if(GetAsyncKeyState(VK_F7)&1) { if(setLight(1.0f)) std::cout<<"[LIGHT] Veja se o mapa ficou claro.\n"; }
        if(GetAsyncKeyState(VK_F8)&1) restoreLight();
        if(GetAsyncKeyState(VK_F9)&1) dumpBytes("Game::look atual",g_lookTarget,32);
        if(GetAsyncKeyState(VK_END)&1) break;
        Sleep(30);
    }
    restoreLight(); uninstallLookHook();
    std::cout << "\n[UNLOAD] Look e Light restaurados.\n"; Sleep(600); FreeConsole();
    FreeLibraryAndExitThread(GetModuleHandleW(L"CromeClassicFeatureProbe.dll"),0);
    return 0;
}
}

BOOL WINAPI DllMain(HINSTANCE hinst,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(hinst); if(auto h=CreateThread(nullptr,0,MainThread,nullptr,0,nullptr)) CloseHandle(h); }
    return TRUE;
}
