#include <windows.h>
#include <tlhelp32.h>
#include <iostream>
#include <string>
#include <filesystem>

static DWORD findPidExact(const wchar_t* exe) {
    PROCESSENTRY32W pe{}; pe.dwSize=sizeof(pe);
    HANDLE s=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0); if(s==INVALID_HANDLE_VALUE) return 0;
    DWORD pid=0; if(Process32FirstW(s,&pe)) do { if(_wcsicmp(pe.szExeFile,exe)==0){pid=pe.th32ProcessID;break;} } while(Process32NextW(s,&pe));
    CloseHandle(s); return pid;
}
static DWORD findPidPrefix(const wchar_t* prefix) {
    PROCESSENTRY32W pe{}; pe.dwSize=sizeof(pe);
    HANDLE s=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0); if(s==INVALID_HANDLE_VALUE) return 0;
    DWORD pid=0; const std::wstring pfx(prefix);
    if(Process32FirstW(s,&pe)) do { std::wstring n(pe.szExeFile); if(n.rfind(pfx,0)==0 && n.size()>=4 && _wcsicmp(n.c_str()+n.size()-4,L".exe")==0){pid=pe.th32ProcessID;break;} } while(Process32NextW(s,&pe));
    CloseHandle(s); return pid;
}
int main() {
    DWORD pid=findPidExact(L"Classico-1790708814.exe");
    if(!pid) pid=findPidPrefix(L"Classico-");
    if(!pid){ std::wcerr<<L"[FAIL] Processo Classico-*.exe nao encontrado.\n"; system("pause"); return 1; }
    auto dll=std::filesystem::absolute(L"CromeClassicFeatureProbe.dll").wstring();
    if(!std::filesystem::exists(dll)){ std::wcerr<<L"[FAIL] DLL nao encontrada: "<<dll<<L"\n"; system("pause"); return 2; }
    HANDLE p=OpenProcess(PROCESS_CREATE_THREAD|PROCESS_QUERY_INFORMATION|PROCESS_VM_OPERATION|PROCESS_VM_WRITE|PROCESS_VM_READ,FALSE,pid);
    if(!p){ std::cerr<<"[FAIL] OpenProcess error="<<GetLastError()<<"\n"; system("pause"); return 3; }
    const SIZE_T bytes=(dll.size()+1)*sizeof(wchar_t);
    void* remote=VirtualAllocEx(p,nullptr,bytes,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    if(!remote){ std::cerr<<"[FAIL] VirtualAllocEx error="<<GetLastError()<<"\n"; CloseHandle(p); system("pause"); return 4; }
    if(!WriteProcessMemory(p,remote,dll.c_str(),bytes,nullptr)){ std::cerr<<"[FAIL] WriteProcessMemory error="<<GetLastError()<<"\n"; VirtualFreeEx(p,remote,0,MEM_RELEASE); CloseHandle(p); system("pause"); return 5; }
    auto load=GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"LoadLibraryW");
    HANDLE th=CreateRemoteThread(p,nullptr,0,reinterpret_cast<LPTHREAD_START_ROUTINE>(load),remote,0,nullptr);
    if(!th){ std::cerr<<"[FAIL] CreateRemoteThread error="<<GetLastError()<<"\n"; VirtualFreeEx(p,remote,0,MEM_RELEASE); CloseHandle(p); system("pause"); return 6; }
    WaitForSingleObject(th,10000); DWORD code=0; GetExitCodeThread(th,&code);
    CloseHandle(th); VirtualFreeEx(p,remote,0,MEM_RELEASE); CloseHandle(p);
    std::wcout<<L"[OK] FeatureProbe injetado no PID "<<pid<<L". Module result=0x"<<std::hex<<code<<L"\n";
    std::wcout<<L"Veja a janela 'Crome Classic FeatureProbe v1 - Look + Lux'.\n";
    system("pause"); return code?0:7;
}
