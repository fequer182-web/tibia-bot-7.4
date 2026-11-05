#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <string>
#include <cstring>
constexpr uint32_t UiMagic=0x49555243,UiVersion=1;
constexpr uint32_t Attack=1,Loot=2,Cave=4,Heal=8,Loop=16;
struct Waypoint {uint32_t x,y,z;};
struct UiState {
    uint32_t magic,version,bytes,uiReady,uiClosed,heartbeat;
    uint32_t flags,hpThreshold,healCooldownMs,destination;
    wchar_t targetName[32],healText[64],status[256],detail[256];
    uint32_t waypointCount,waypointIndex,addSerial,clearSerial,removeSerial,removeIndex;
    Waypoint waypoints[64];
};
static_assert(sizeof(wchar_t)==2&&sizeof(UiState)==2048,"IPC x64 Windows");
inline std::wstring MappingName(){return L"Local\\CrominUI_"+std::to_wstring(GetCurrentProcessId());}
inline std::wstring MutexName(){return MappingName()+L"_Mutex";}
template<size_t N> void Text(wchar_t(&dest)[N],const std::wstring& value){
    memset(dest,0,sizeof(dest));value.copy(dest,N-1);
}
struct UiBridge {
    HANDLE map=nullptr,mutex=nullptr;UiState* state=nullptr;
    bool open(){
        mutex=CreateMutexW(nullptr,FALSE,MutexName().c_str());
        map=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,MappingName().c_str());
        if(!map||!mutex)return false;
        state=static_cast<UiState*>(MapViewOfFile(map,FILE_MAP_ALL_ACCESS,0,0,sizeof(UiState)));
        return state&&state->magic==UiMagic&&state->version==UiVersion&&state->bytes==sizeof(UiState);
    }
    template<class F> bool access(F fn){
        if(!state)return false;
        DWORD wait=WaitForSingleObject(mutex,200);
        if(wait!=WAIT_OBJECT_0&&wait!=WAIT_ABANDONED)return false;
        fn(*state);ReleaseMutex(mutex);return true;
    }
    ~UiBridge(){if(state)UnmapViewOfFile(state);if(map)CloseHandle(map);if(mutex)CloseHandle(mutex);}
};
