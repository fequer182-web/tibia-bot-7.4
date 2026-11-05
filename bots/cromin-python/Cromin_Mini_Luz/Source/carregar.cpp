#include "build_guard.hpp"
#include <shellapi.h>
struct RemoteBuffer {
    HANDLE process;void* pointer;
    RemoteBuffer(HANDLE p,size_t size):process(p),pointer(VirtualAllocEx(p,nullptr,size,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE)){}
    ~RemoteBuffer(){if(pointer)VirtualFreeEx(process,pointer,0,MEM_RELEASE);}
};
uintptr_t RemoteModule(DWORD pid,const wchar_t* name){
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid));
    if(snapshot.h==INVALID_HANDLE_VALUE)throw Error("Module snapshot");
    MODULEENTRY32W module{};module.dwSize=sizeof(module);
    if(Module32FirstW(snapshot.h,&module))do{
        if(!_wcsicmp(module.szModule,name))return reinterpret_cast<uintptr_t>(module.modBaseAddr);
    }while(Module32NextW(snapshot.h,&module));return 0;
}
int main(int argc,char** argv)try{
    if(argc!=4||std::string(argv[1])!="--pid")throw std::runtime_error("Uso: Mini_Luz_Load.exe --pid PID caminho_da_DLL");
    DWORD pid=std::stoul(argv[2]);if(!pid||pid==GetCurrentProcessId())throw std::runtime_error("PID invalido");
    int wideCount=0;LPWSTR* wideArgs=CommandLineToArgvW(GetCommandLineW(),&wideCount);
    if(!wideArgs||wideCount!=4){if(wideArgs)LocalFree(wideArgs);throw std::runtime_error("Argumentos do carregador invalidos");}
    auto path=std::filesystem::absolute(std::filesystem::path(wideArgs[3]));LocalFree(wideArgs);
    if(!std::filesystem::is_regular_file(path)||path.filename()!=L"Cromin_Luz.dll")throw std::runtime_error("Cromin_Luz.dll nao encontrada");
    Handle process(OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ|PROCESS_VM_WRITE|PROCESS_VM_OPERATION|PROCESS_CREATE_THREAD,FALSE,pid));
    if(!process.h)throw Error("OpenProcess");
    CheckBuild(process.h,pid);
    BOOL debugger=FALSE;if(!CheckRemoteDebuggerPresent(process.h,&debugger))throw Error("CheckRemoteDebuggerPresent");
    if(debugger)throw std::runtime_error("Encerre o outro depurador antes de carregar a interface");
    if(RemoteModule(pid,L"Cromin_Luz.dll")){std::cout<<"[LUZ] DLL ja carregada; nao injeta novamente.\n";return 0;}
    auto local=GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"LoadLibraryW");HMODULE owner=nullptr;
    if(!local||!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  reinterpret_cast<LPCWSTR>(local),&owner))throw Error("LoadLibraryW owner");
    wchar_t modulePath[32768]{};if(!GetModuleFileNameW(owner,modulePath,32768))throw Error("Owner path");
    auto moduleBase=RemoteModule(pid,std::filesystem::path(modulePath).filename().c_str());
    if(!moduleBase)throw std::runtime_error("Modulo do carregador nao localizado no alvo");
    auto remote=moduleBase+(reinterpret_cast<uintptr_t>(local)-reinterpret_cast<uintptr_t>(owner));
    std::array<uint8_t,16> actual{};
    if(!Read(process.h,remote,actual.data(),actual.size())||memcmp(actual.data(),reinterpret_cast<void*>(local),actual.size()))
        throw std::runtime_error("Carregador remoto diferente; nao executou");
    auto text=path.wstring();size_t bytes=(text.size()+1)*sizeof(wchar_t);RemoteBuffer argument(process.h,bytes);
    if(!argument.pointer)throw Error("VirtualAllocEx caminho");
    SIZE_T written=0;
    if(!WriteProcessMemory(process.h,argument.pointer,text.c_str(),bytes,&written)||written!=bytes)throw Error("WriteProcessMemory caminho");
    Handle thread(CreateRemoteThread(process.h,nullptr,0,reinterpret_cast<LPTHREAD_START_ROUTINE>(remote),argument.pointer,0,nullptr));
    if(!thread.h)throw Error("CreateRemoteThread LoadLibraryW");
    // O caminho permanece alocado ate LoadLibraryW retornar. Nao encerra a thread por timeout.
    if(WaitForSingleObject(thread.h,INFINITE)!=WAIT_OBJECT_0)throw Error("WaitForSingleObject carregador");
    if(!RemoteModule(pid,L"Cromin_Luz.dll"))throw std::runtime_error("DLL nao permaneceu carregada; confira inicializacao do painel");
    std::cout<<"[LUZ] Cromin_Luz.dll carregada. Luz inicia desligada; F6 ativa, F7/F8 ajustam e F9 descarrega.\n";return 0;
}catch(const std::exception& e){std::cerr<<"[ERRO] "<<e.what()<<"\n";return 1;}
