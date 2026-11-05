#include "ui_shared.hpp"
#include <filesystem>
#include <iostream>
template<class F> bool PumpUntil(F predicate,unsigned milliseconds){
    DWORD end=GetTickCount()+milliseconds;
    while(GetTickCount()<end){MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
        if(predicate())return true;
        Sleep(10);}return false;
}
int main(){
    HANDLE mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(UiState),MappingName().c_str());
    if(!mapping)return 1;
    auto state=static_cast<UiState*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(UiState)));
    if(!state)return 2;
    *state={};state->magic=UiMagic;state->version=UiVersion;state->bytes=sizeof(UiState);state->heartbeat=GetTickCount();
    state->hpThreshold=80;state->healCooldownMs=2000;Text(state->status,L"Teste controlado da DLL");
    HWND host=CreateWindowExW(WS_EX_NOACTIVATE,L"STATIC",L"Cromin: host de teste",WS_OVERLAPPEDWINDOW,
        0,0,900,680,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    ShowWindow(host,SW_SHOWNOACTIVATE);
    wchar_t exe[32768]{};GetModuleFileNameW(nullptr,exe,32768);
    auto dll=std::filesystem::path(exe).parent_path()/L"Cromin_UI.dll";
    HMODULE module=LoadLibraryW(dll.c_str());
    bool ready=module&&PumpUntil([&](){return state->uiReady==1;},5000);
    HWND panel=ready?FindWindowExW(host,nullptr,L"CrominEmbeddedPanelV1",nullptr):nullptr;
    if(panel){
        PumpUntil([](){return false;},300);
        SendMessageW(GetDlgItem(panel,110),BM_CLICK,0,0);
        ready=ready&&(state->flags&Attack);
        SendMessageW(GetDlgItem(panel,101),BM_CLICK,0,0);
        ready=ready&&!(state->flags&15);
        SetWindowPos(host,nullptr,0,0,900,400,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        PumpUntil([](){return false;},300);
        SendMessageW(panel,WM_VSCROLL,SB_PAGEDOWN,0);
        ready=ready&&GetScrollPos(panel,SB_VERT)>0;
        SendMessageW(panel,WM_CLOSE,0,0);
    }
    bool closed=PumpUntil([&](){return state->uiClosed&&GetModuleHandleW(L"Cromin_UI.dll")==nullptr;},5000);
    bool ok=ready&&panel&&closed;
    std::cout<<(ok?"PASS":"FAIL")<<": DLL real, painel filho, IPC, rolagem e descarregamento ao fechar\n";
    DestroyWindow(host);UnmapViewOfFile(state);CloseHandle(mapping);return ok?0:1;
}
