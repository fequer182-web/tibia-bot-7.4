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
DWORD64& DebugAddress(CONTEXT& context,int slot){
    switch(slot){case 0:return context.Dr0;case 1:return context.Dr1;case 2:return context.Dr2;default:return context.Dr3;}
}
struct ThreadState {HANDLE handle=nullptr;CONTEXT original{};int slot=-1;};
struct Position {bool valid=false;uint32_t x=0,y=0;uint8_t z=0;};
Position ReadPosition(HANDLE process,uintptr_t base){
    Position p{};uintptr_t player=0,vt=0;
    if(!base || !Read(process,base+0xE52A48,&player,8) || !player || !Read(process,player,&vt,8) || vt!=base+0xAE99E0)return p;
    if(!Read(process,player+0x30,&p.x,4)||!Read(process,player+0x34,&p.y,4)||!Read(process,player+0x38,&p.z,1))return p;
    p.valid=p.x>0&&p.x<65535&&p.y>0&&p.y<65535&&p.z<=15;return p;
}
void WritePosition(std::ostream& out,const Position& p){out<<"{\"valid\": "<<(p.valid?"true":"false")<<", \"x\": "<<p.x<<", \"y\": "<<p.y<<", \"z\": "<<unsigned(p.z)<<"}";}
struct Hit {DWORD thread=0;uintptr_t rip=0,rcx=0,rdx=0,r8=0,r9=0,rsp=0,returnAddress=0;bool returnRead=false,redirected=false;uint32_t effectiveEdx=0;Position before;};
struct Trace {
    DWORD pid;HANDLE process;uintptr_t address,base;unsigned seconds,maxHits;
    std::map<DWORD,ThreadState> threads;
    std::vector<Hit> hits;
    std::vector<std::string> diagnostics;
    DEBUG_EVENT event{};DWORD continuation=DBG_CONTINUE;
    bool attached=false,pending=false,exited=false,detached=false;
    HANDLE startFixture=nullptr;
    int redirectDirection=-1;uintptr_t expectedInstance=0;
    std::vector<Position> positionsAfter;
    Trace(DWORD p,HANDLE h,uintptr_t a,uintptr_t b,unsigned duration,unsigned count):pid(p),process(h),address(a),base(b),seconds(duration),maxHits(count){}
    ~Trace(){Cleanup();}
    void Arm(DWORD tid,HANDLE eventHandle){
        // Os handles dos eventos sao gerenciados pelo mecanismo de depuracao.
        // Mantemos uma duplicata propria; fechar o original quebra o detach.
        HANDLE handle=nullptr;
        if(!DuplicateHandle(GetCurrentProcess(),eventHandle,GetCurrentProcess(),&handle,0,FALSE,DUPLICATE_SAME_ACCESS))throw Error("DuplicateHandle thread");
        ThreadState state{};state.handle=handle;state.original.ContextFlags=CONTEXT_DEBUG_REGISTERS;
        if(!GetThreadContext(handle,&state.original)){CloseHandle(handle);throw Error("GetThreadContext");}
        for(int slot=0;slot<4;++slot)if(!(state.original.Dr7&(3ULL<<(slot*2)))){state.slot=slot;break;}
        if(state.slot<0){CloseHandle(handle);throw std::runtime_error("Thread sem registrador de breakpoint livre. Nao substitui breakpoint existente.");}
        auto context=state.original;
        DebugAddress(context,state.slot)=address;
        context.Dr7&=~(0xFULL<<(16+state.slot*4)); // execucao, tamanho 1 byte
        context.Dr7|=1ULL<<(state.slot*2); // local enable
        if(!SetThreadContext(handle,&context)){CloseHandle(handle);throw Error("SetThreadContext");}
        threads.emplace(tid,state);
    }
    void Continue(){if(pending){if(!ContinueDebugEvent(event.dwProcessId,event.dwThreadId,continuation))throw Error("ContinueDebugEvent");pending=false;}}
    void Cleanup() noexcept {
        if(!attached)return;
        for(auto& entry:threads){
            auto& state=entry.second;bool suspended=false;
            if(!exited && !pending){
                if(SuspendThread(state.handle)!=DWORD(-1))suspended=true;
                else diagnostics.push_back("Nao foi possivel pausar/restaurar a thread "+std::to_string(entry.first));
            }
            if(!exited && (pending||suspended)){
                CONTEXT context{};context.ContextFlags=CONTEXT_DEBUG_REGISTERS;
                if(GetThreadContext(state.handle,&context)){
                    // Restaura apenas o slot que este rastreador reservou.
                    DebugAddress(context,state.slot)=DebugAddress(state.original,state.slot);
                    uint64_t mask=(3ULL<<(state.slot*2))|(0xFULL<<(16+state.slot*4));
                    context.Dr7=(context.Dr7&~mask)|(state.original.Dr7&mask);
                    context.Dr6&=~(1ULL<<state.slot);
                    if(!SetThreadContext(state.handle,&context))diagnostics.push_back("Falha restaurando registradores da thread "+std::to_string(entry.first));
                }else diagnostics.push_back("Thread indisponivel ao restaurar: "+std::to_string(entry.first));
            }
            if(suspended)ResumeThread(state.handle);
            CloseHandle(state.handle);
        }
        threads.clear();
        if(pending){if(!ContinueDebugEvent(event.dwProcessId,event.dwThreadId,continuation))diagnostics.push_back("ContinueDebugEvent falhou no encerramento");pending=false;}
        if(exited)detached=true;
        else {detached=!!DebugActiveProcessStop(pid);if(!detached)diagnostics.push_back("DebugActiveProcessStop falhou: "+std::to_string(GetLastError()));}
        attached=false;
    }
    void Run(){
        if(!DebugActiveProcess(pid))throw Error("DebugActiveProcess");
        attached=true;
        if(!DebugSetProcessKillOnExit(FALSE)){Cleanup();throw Error("DebugSetProcessKillOnExit");}
        auto deadline=GetTickCount64()+seconds*1000ULL;
        bool initialBreakpoint=true;
        while(!interrupted && GetTickCount64()<deadline && hits.size()<maxHits){
            if(!WaitForDebugEvent(&event,100)){
                DWORD error=GetLastError();if(error==ERROR_SEM_TIMEOUT || error==0)continue;throw Error("WaitForDebugEvent");
            }
            pending=true;continuation=DBG_CONTINUE;
            switch(event.dwDebugEventCode){
                case CREATE_PROCESS_DEBUG_EVENT:
                    if(event.u.CreateProcessInfo.hFile)CloseHandle(event.u.CreateProcessInfo.hFile);
                    Arm(event.dwThreadId,event.u.CreateProcessInfo.hThread);break;
                case CREATE_THREAD_DEBUG_EVENT:Arm(event.dwThreadId,event.u.CreateThread.hThread);break;
                case LOAD_DLL_DEBUG_EVENT:if(event.u.LoadDll.hFile)CloseHandle(event.u.LoadDll.hFile);break;
                case EXIT_THREAD_DEBUG_EVENT:{auto it=threads.find(event.dwThreadId);if(it!=threads.end()){CloseHandle(it->second.handle);threads.erase(it);}break;}
                case EXIT_PROCESS_DEBUG_EVENT:exited=true;break;
                case EXCEPTION_DEBUG_EVENT:{
                    auto code=event.u.Exception.ExceptionRecord.ExceptionCode;
                    continuation=DBG_EXCEPTION_NOT_HANDLED;
                    if(initialBreakpoint && code==EXCEPTION_BREAKPOINT){
                        initialBreakpoint=false;continuation=DBG_CONTINUE;
                        std::cout<<"[PRONTO] Ande MANUALMENTE uma vez no jogo. Captura por ate "<<seconds<<" segundos.\n"<<std::flush;
                        if(startFixture)SetEvent(startFixture);
                    }else if(code==EXCEPTION_SINGLE_STEP){
                        auto it=threads.find(event.dwThreadId);
                        if(it!=threads.end()){
                            CONTEXT context{};context.ContextFlags=CONTEXT_CONTROL|CONTEXT_INTEGER|CONTEXT_DEBUG_REGISTERS;
                            if(!GetThreadContext(it->second.handle,&context))throw Error("GetThreadContext hit");
                            int slot=it->second.slot;
                            if(context.Rip==address && (context.Dr6&(1ULL<<slot))){
                                continuation=DBG_CONTINUE; // breakpoint proprio, inclusive se a verificacao seguinte falhar
                                Hit hit{};hit.thread=event.dwThreadId;hit.rip=context.Rip;hit.rcx=context.Rcx;hit.rdx=context.Rdx;
                                hit.r8=context.R8;hit.r9=context.R9;hit.rsp=context.Rsp;
                                hit.returnRead=Read(process,context.Rsp,&hit.returnAddress,8);
                                hit.effectiveEdx=uint32_t(context.Rdx);
                                if(redirectDirection>=0){
                                    if(context.Rcx!=expectedInstance || uint32_t(context.Rdx)>7 || (context.R8&255)>1)
                                        throw std::runtime_error("Instancia/argumentos diferentes do esperado. Nao alterou a direcao.");
                                    if(uint32_t(context.Rdx)!=0)throw std::runtime_error("A primeira chamada nao era para cima (EDX=0). Nao alterou a direcao; repita usando apenas a seta para cima.");
                                    hit.before=ReadPosition(process,base);
                                    if(base && !hit.before.valid)throw std::runtime_error("Posicao/VTable nao conferem. Nao alterou a direcao.");
                                    context.Rdx=uint32_t(redirectDirection);
                                    hit.effectiveEdx=uint32_t(redirectDirection);
                                }
                                context.Dr6&=~(1ULL<<slot);context.EFlags|=0x10000; // Resume Flag
                                if(!SetThreadContext(it->second.handle,&context))throw Error("SetThreadContext resume");
                                hit.redirected=redirectDirection>=0;
                                hits.push_back(hit);
                                continuation=DBG_CONTINUE;
                                std::cout<<"[HIT] thread="<<hit.thread<<" RCX="<<Hex(hit.rcx)<<" EDX="<<uint32_t(hit.rdx)
                                         <<" R8B="<<unsigned(hit.r8&255)<<" retorno="<<Hex(hit.returnAddress)<<"\n"<<std::flush;
                                if(hit.redirected)std::cout<<"[DIRECAO] EDX "<<uint32_t(hit.rdx)<<" -> "<<hit.effectiveEdx<<"; RCX e R8B preservados.\n"<<std::flush;
                            }
                        }
                    }
                    break;
                }
            }
            if(exited || hits.size()>=maxHits)break; // restaura com o evento ainda pausado
            Continue();
        }
        Cleanup();
    }
    void Save(const std::filesystem::path& output){
        std::ofstream f(output);if(!f)throw std::runtime_error("Falha ao salvar relatorio");
        f<<"{\n  \"mode\": \"external_hardware_breakpoint_trace\",\n  \"native_call_requested\": false,\n  \"pid\": "<<pid
         <<",\n  \"argument_redirect_requested\": "<<(redirectDirection>=0?"true":"false")
         <<",\n  \"function_address\": \""<<Hex(address)<<"\",\n  \"base\": \""<<Hex(base)<<"\",\n  \"detached\": "<<(detached?"true":"false")
         <<",\n  \"hits\": [\n";
        for(size_t i=0;i<hits.size();++i){auto& h=hits[i];if(i)f<<",\n";
            f<<"    {\"threadId\": "<<h.thread<<", \"rip\": \""<<Hex(h.rip)<<"\", \"rcx\": \""<<Hex(h.rcx)
             <<"\", \"edx\": "<<uint32_t(h.rdx)<<", \"r8b\": "<<(h.r8&255)<<", \"r9\": \""<<Hex(h.r9)
             <<"\", \"rsp\": \""<<Hex(h.rsp)<<"\", \"returnRead\": "<<(h.returnRead?"true":"false")
             <<", \"returnAddress\": \""<<Hex(h.returnAddress)<<"\"}";
        }
        f<<"\n  ],\n  \"direction_test\": {\"requested_direction\": "<<redirectDirection<<", \"changes\": [";
        for(size_t i=0;i<hits.size();++i){if(i)f<<", ";auto& h=hits[i];f<<"{\"applied\": "<<(h.redirected?"true":"false")<<", \"original_edx\": "<<uint32_t(h.rdx)<<", \"effective_edx\": "<<h.effectiveEdx<<", \"position_before\": ";WritePosition(f,h.before);f<<"}";}
        f<<"], \"positions_after\": [";
        for(size_t i=0;i<positionsAfter.size();++i){if(i)f<<", ";WritePosition(f,positionsAfter[i]);}
        f<<"]},\n  \"diagnostics\": [";
        for(size_t i=0;i<diagnostics.size();++i){if(i)f<<", ";f<<"\""<<diagnostics[i]<<"\"";}
        f<<"]\n}\n";
    }
};

volatile uint64_t fixtureSink=0;
__declspec(noinline) void Fixture(uint64_t tag,int direction,bool flag){fixtureSink=tag+direction+flag;}
std::filesystem::path ExePath(){std::vector<wchar_t> path(32768);DWORD n=GetModuleFileNameW(nullptr,path.data(),DWORD(path.size()));if(!n||n>=path.size())throw Error("GetModuleFileNameW");return std::filesystem::path(std::wstring(path.data(),n));}
int SelfTest(bool timeout,bool redirectTest=false){
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};
    Handle ready(CreateEventW(&sa,TRUE,FALSE,nullptr));if(!ready.h)throw Error("CreateEventW");
    HANDLE pipeRead=nullptr,pipeWrite=nullptr;if(!CreatePipe(&pipeRead,&pipeWrite,&sa,0))throw Error("CreatePipe");
    Handle read(pipeRead),write(pipeWrite);SetHandleInformation(read.h,HANDLE_FLAG_INHERIT,0);
    std::wstring command=L"\""+ExePath().wstring()+L"\" "+(redirectTest?L"--fixture-one ":L"--fixture ")+std::to_wstring(reinterpret_cast<uintptr_t>(ready.h));
    STARTUPINFOW si{};si.cb=sizeof(si);si.dwFlags=STARTF_USESTDHANDLES;si.hStdOutput=write.h;si.hStdError=write.h;si.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};if(!CreateProcessW(nullptr,command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi))throw Error("CreateProcessW fixture");
    Handle process(pi.hProcess),thread(pi.hThread);
    try{
        std::string line;char c=0;DWORD got=0;while(ReadFile(read.h,&c,1,&got,nullptr)&&got&&c!='\n')line+=c;
        uintptr_t address=std::stoull(line,nullptr,0);
        std::istringstream fields(line);std::string functionText,sinkText;fields>>functionText>>sinkText;
        uintptr_t sink=std::stoull(sinkText,nullptr,0);
        Trace trace(pi.dwProcessId,process.h,address,0,timeout?2:8,(timeout||redirectTest)?1:3);
        if(redirectTest){trace.redirectDirection=1;trace.expectedInstance=0x1234;}
        if(!timeout)trace.startFixture=ready.h;
        trace.Run();
        bool valid=trace.detached && trace.diagnostics.empty();
        if(timeout)valid=valid && trace.hits.empty();
        else if(redirectTest){valid=valid && trace.hits.size()==1 && trace.hits[0].redirected && trace.hits[0].rdx==0 && trace.hits[0].effectiveEdx==1 && trace.hits[0].rcx==0x1234 && (trace.hits[0].r8&255)==1;}
        else {valid=valid && trace.hits.size()==3;for(size_t i=0;i<trace.hits.size();++i)valid=valid && trace.hits[i].rcx==0x1234 && uint32_t(trace.hits[i].rdx)==i && (trace.hits[i].r8&255)==(i==1);}
        // Apos o detach o fixture executa chamadas adicionais sem um depurador.
        // Se restou um breakpoint ativo, isso pode encerrar o fixture e falhar o teste.
        if(timeout)SetEvent(ready.h);
        Sleep(500);
        DWORD exitCode=0;valid=valid && GetExitCodeProcess(process.h,&exitCode)&&exitCode==STILL_ACTIVE;
        if(redirectTest)valid=valid && ReadValue<uint64_t>(process.h,sink)==0x1236;
        std::cout<<(valid?"PASS":"FAIL")<<": "<<(redirectTest?"direcao 0->1 executada pelo fixture, instancia/flag preservadas":timeout?"timeout, detach e processo de teste vivo":"captura RCX/EDX/R8B em processo de teste, restauracao e detach")<<"\n"<<std::flush;
        // Encerra somente o fixture criado por este proprio teste.
        TerminateProcess(process.h,0);WaitForSingleObject(process.h,3000);
        return valid?0:1;
    }catch(...){TerminateProcess(process.h,1);WaitForSingleObject(process.h,3000);throw;}
}

int main(int argc,char** argv)try{
    if(argc>=3 && (std::string(argv[1])=="--fixture" || std::string(argv[1])=="--fixture-one")){
        HANDLE start=reinterpret_cast<HANDLE>(std::stoull(argv[2]));
        std::cout<<Hex(reinterpret_cast<uintptr_t>(&Fixture))<<" "<<Hex(reinterpret_cast<uintptr_t>(&fixtureSink))<<"\n"<<std::flush;
        if(WaitForSingleObject(start,20000)==WAIT_OBJECT_0){
            if(std::string(argv[1])=="--fixture-one")Fixture(0x1234,0,true);
            else {Fixture(0x1234,0,false);Fixture(0x1234,1,true);Fixture(0x1234,2,false);Sleep(200);Fixture(0x9999,3,true);}
        }
        Sleep(15000);return 0;
    }
    if(argc==2 && std::string(argv[1])=="--self-test")return SelfTest(false);
    if(argc==2 && std::string(argv[1])=="--self-test-timeout")return SelfTest(true);
    if(argc==2 && std::string(argv[1])=="--self-test-redirect")return SelfTest(false,true);
    DWORD pid=0;unsigned seconds=30,count=1;bool traceRequested=false;int redirect=-1;
    for(int i=1;i<argc;++i){std::string arg=argv[i];
        if(arg=="--trace")traceRequested=true;
        else if(arg=="--pid"&&i+1<argc)pid=std::stoul(argv[++i]);
        else if(arg=="--seconds"&&i+1<argc)seconds=std::stoul(argv[++i]);
        else if(arg=="--hits"&&i+1<argc)count=std::stoul(argv[++i]);
        else if(arg=="--redirect"&&i+1<argc){std::string direction=argv[++i];if(direction=="north")redirect=0;else if(direction=="east")redirect=1;else if(direction=="south")redirect=2;else if(direction=="west")redirect=3;else throw std::runtime_error("Direcao invalida");}
        else throw std::runtime_error("Argumento desconhecido");
    }
    if(!pid||!traceRequested){std::cout<<"Uso: OTC_Walk_Trace.exe --pid NUMERO --trace [--seconds 30] [--hits 1] [--redirect east]\n"
        <<"Anexa depurador e breakpoint de hardware temporario. Nao chama walk; ande manualmente.\n";return 0;}
    if(seconds<5||seconds>60||count<1||count>12||pid==GetCurrentProcessId())throw std::runtime_error("Parametros invalidos");
    if(redirect>=0 && count!=1)throw std::runtime_error("O teste de direcao exige --hits 1");
    Handle process(OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ,FALSE,pid));if(!process.h)throw Error("OpenProcess");
    BOOL existing=FALSE;if(!CheckRemoteDebuggerPresent(process.h,&existing))throw Error("CheckRemoteDebuggerPresent");
    if(existing)throw std::runtime_error("O processo ja tem depurador. Teste cancelado.");
    uintptr_t base=CheckBuild(process.h,pid);
    std::cout<<"Build conferida. Observando Game::walk em "<<Hex(base+0x140D60)<<".\n";
    SetConsoleCtrlHandler(ConsoleHandler,TRUE);
    Trace trace(pid,process.h,base+0x140D60,base,seconds,count);
    trace.redirectDirection=redirect;trace.expectedInstance=base+0xE52A40;
    if(redirect>=0)std::cout<<"[TESTE DE ARGUMENTO] A proxima entrada real em walk PARA CIMA tera EDX="<<redirect<<" uma vez. Use apenas uma batida curta na seta para cima.\n";
    auto output=ExePath().parent_path()/("walk_call_trace_"+std::to_string(GetTickCount64())+".json");
    try{trace.Run();}catch(...){trace.Cleanup();trace.Save(output);throw;}
    if(redirect>=0 && trace.detached && !trace.hits.empty() && trace.hits[0].redirected){
        std::cout<<"[POSICAO] Observando por 2 segundos; solte as teclas.\n"<<std::flush;
        for(int i=0;i<40;++i){Sleep(50);trace.positionsAfter.push_back(ReadPosition(process.h,base));}
        auto& before=trace.hits[0].before;auto& after=trace.positionsAfter.back();
        if(before.valid && after.valid)std::cout<<"Antes "<<before.x<<","<<before.y<<","<<unsigned(before.z)<<" | Depois "<<after.x<<","<<after.y<<","<<unsigned(after.z)<<"\n";
    }
    trace.Save(output);
    std::wcout<<L"[JSON] "<<output.wstring()<<L"\n";
    std::cout<<"Capturas: "<<trace.hits.size()<<" | detached="<<trace.detached<<"\n";
    return trace.detached&&trace.diagnostics.empty()?0:2;
}catch(const std::exception& error){std::cerr<<"[ERRO] "<<error.what()<<"\n";return 1;}
