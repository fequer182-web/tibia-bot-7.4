#define main TraceUtilitiesEntry
#include "trace_walk.cpp"
#undef main
#include <iomanip>

struct LookResult {std::string kind="unknown";uint32_t id=0;uintptr_t object=0,vtable=0;uint32_t x=0,y=0;uint16_t z=0;};
LookResult ReadLook(HANDLE process,uintptr_t base,uintptr_t pairAddress){
    LookResult result;std::array<uintptr_t,2> pair{},after{};
    if(pairAddress<0x10000||!Read(process,pairAddress,pair.data(),sizeof(pair))||!pair[0]||!pair[1])return result;
    std::array<uint8_t,0x48> raw{};if(!Read(process,pair[0],raw.data(),raw.size()))return result;
    if(!Read(process,pairAddress,after.data(),sizeof(after))||pair!=after)return result;
    result.object=pair[0];memcpy(&result.vtable,raw.data(),8);
    if(result.vtable==base+0xAE9BC0){uint16_t id=0;memcpy(&id,raw.data()+0x40,2);if(id){result.kind="item";result.id=id;}}
    else if(result.vtable==base+0xAF61D8){result.kind="monster";memcpy(&result.id,raw.data()+0x40,4);}
    memcpy(&result.x,raw.data()+0x20,4);memcpy(&result.y,raw.data()+0x24,4);memcpy(&result.z,raw.data()+0x28,2);
    return result;
}
void EmitLook(const LookResult& value,DWORD thread){
    std::cout<<"{\"event\":\"look\",\"kind\":\""<<value.kind<<"\",\"id\":"<<value.id
        <<",\"object\":\""<<Hex(value.object)<<"\",\"vtable\":\""<<Hex(value.vtable)<<"\",\"thread\":"<<thread
        <<",\"position\":{\"x\":"<<value.x<<",\"y\":"<<value.y<<",\"z\":"<<value.z<<"}}\n"<<std::flush;
}
struct LookWatch:Trace {
    uintptr_t instance;bool stopAfterHit=false;unsigned observed=0;LookResult last;
    LookWatch(DWORD pid,HANDLE process,uintptr_t address,uintptr_t base,uintptr_t instance):Trace(pid,process,address,base,0,0),instance(instance){}
    void RunWatch(HANDLE stopEvent,HANDLE parent){
        if(WaitForSingleObject(stopEvent,0)==WAIT_OBJECT_0){detached=true;return;}
        if(!DebugActiveProcess(pid))throw Error("DebugActiveProcess look");attached=true;
        if(!DebugSetProcessKillOnExit(FALSE))throw Error("DebugSetProcessKillOnExit look");
        bool initial=true;
        while(!interrupted&&WaitForSingleObject(stopEvent,0)==WAIT_TIMEOUT&&(!parent||WaitForSingleObject(parent,0)==WAIT_TIMEOUT)){
            if(!WaitForDebugEvent(&event,100)){DWORD code=GetLastError();if(code==ERROR_SEM_TIMEOUT||code==0)continue;throw Error("WaitForDebugEvent look");}
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
                    continuation=DBG_EXCEPTION_NOT_HANDLED;auto code=event.u.Exception.ExceptionRecord.ExceptionCode;
                    if(initial&&code==EXCEPTION_BREAKPOINT){initial=false;continuation=DBG_CONTINUE;
                        std::cout<<"{\"event\":\"ready\"}\n"<<std::flush;if(startFixture)SetEvent(startFixture);
                    }else if(code==EXCEPTION_SINGLE_STEP){
                        auto it=threads.find(event.dwThreadId);if(it==threads.end())break;
                        CONTEXT context{};context.ContextFlags=CONTEXT_CONTROL|CONTEXT_INTEGER|CONTEXT_DEBUG_REGISTERS;
                        if(!GetThreadContext(it->second.handle,&context))throw Error("GetThreadContext look");
                        int slot=it->second.slot;
                        if(context.Rip==address&&(context.Dr6&(1ULL<<slot))){
                            continuation=DBG_CONTINUE;
                            if(context.Rcx==instance){last=ReadLook(process,base,context.Rdx);++observed;EmitLook(last,event.dwThreadId);}
                            context.Dr6&=~(1ULL<<slot);context.EFlags|=0x10000;
                            if(!SetThreadContext(it->second.handle,&context))throw Error("Resume Look");
                        }
                    }
                    break;
                }
            }
            if(exited||(stopAfterHit&&observed))break;
            Continue();
        }
        Cleanup();
    }
    void Closed(){std::cout<<"{\"event\":\"closed\",\"detached\":"<<(detached?"true":"false")<<",\"clean\":"<<(detached&&diagnostics.empty()?"true":"false")<<",\"error\":"<<std::quoted(diagnostics.empty()?std::string():diagnostics.front())<<"}\n"<<std::flush;}
};

volatile uint64_t fixtureLookCalls=0;
__declspec(noinline) void LookFixture(uintptr_t instance,uintptr_t* pair,uint32_t flag){
    if(instance==0x1234&&pair&&pair[0]&&pair[1]==0x8888&&flag==1
        &&*reinterpret_cast<volatile uintptr_t*>(pair[0])==0x140AE9BC0ULL
        &&*reinterpret_cast<volatile uint16_t*>(pair[0]+0x40)==3994)fixtureLookCalls=fixtureLookCalls+1;
    asm volatile("" ::: "memory");
}
DWORD WINAPI StopLookFixture(LPVOID event){Sleep(200);SetEvent(reinterpret_cast<HANDLE>(event));return 0;}
int LookSelfTest(){
    constexpr uintptr_t base=0x140000000;std::array<uint8_t,0x48> item{};uintptr_t vt=base+0xAE9BC0;
    memcpy(item.data(),&vt,8);uint16_t id=3994;memcpy(item.data()+0x40,&id,2);
    std::array<uintptr_t,2> pair{reinterpret_cast<uintptr_t>(item.data()),0x8888};
    auto decoded=ReadLook(GetCurrentProcess(),base,reinterpret_cast<uintptr_t>(pair.data()));
    if(decoded.kind!="item"||decoded.id!=3994)throw std::runtime_error("Decoder ID");
    item[0]=0;decoded=ReadLook(GetCurrentProcess(),base,reinterpret_cast<uintptr_t>(pair.data()));
    if(decoded.kind!="unknown"||decoded.id)throw std::runtime_error("Unknown type accepted");
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};Handle ready(CreateEventW(&sa,TRUE,FALSE,nullptr)),stop(CreateEventW(nullptr,TRUE,FALSE,nullptr));
    HANDLE input=nullptr,output=nullptr;if(!CreatePipe(&input,&output,&sa,0))throw Error("fixture pipe");Handle read(input),write(output);
    SetHandleInformation(read.h,HANDLE_FLAG_INHERIT,0);
    std::wstring cmd=L"\""+ExePath().wstring()+L"\" --fixture "+std::to_wstring(reinterpret_cast<uintptr_t>(ready.h));
    STARTUPINFOW si{};si.cb=sizeof(si);si.dwFlags=STARTF_USESTDHANDLES;si.hStdOutput=write.h;si.hStdError=write.h;
    PROCESS_INFORMATION pi{};if(!CreateProcessW(nullptr,cmd.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi))throw Error("fixture process");
    Handle process(pi.hProcess),thread(pi.hThread);
    try{
        std::string line;char c;DWORD got;while(ReadFile(read.h,&c,1,&got,nullptr)&&got&&c!='\n')line+=c;
        std::istringstream values(line);std::string address,calls;values>>address>>calls;
        LookWatch watch(pi.dwProcessId,process.h,std::stoull(address,nullptr,0),base,0x1234);watch.startFixture=ready.h;watch.stopAfterHit=true;
        watch.RunWatch(stop.h,nullptr);watch.Closed();Sleep(100);
        bool ok=watch.detached&&watch.diagnostics.empty()&&watch.observed==1&&watch.last.id==3994&&ReadValue<uint64_t>(process.h,std::stoull(calls,nullptr,0))>0;
        struct Stopper {HANDLE h=nullptr;~Stopper(){if(h){WaitForSingleObject(h,2000);CloseHandle(h);}}} stopper;
        stopper.h=CreateThread(nullptr,0,StopLookFixture,stop.h,0,nullptr);if(!stopper.h)throw Error("fixture stop thread");
        LookWatch streaming(pi.dwProcessId,process.h,std::stoull(address,nullptr,0),base,0x1234);
        streaming.RunWatch(stop.h,nullptr);streaming.Closed();
        ok=ok&&streaming.detached&&streaming.diagnostics.empty();
        SetEvent(stop.h);LookWatch cancelled(pi.dwProcessId,process.h,std::stoull(address,nullptr,0),base,0x1234);cancelled.RunWatch(stop.h,nullptr);
        ok=ok&&cancelled.detached&&!cancelled.attached&&cancelled.observed==0;
        TerminateProcess(process.h,0);WaitForSingleObject(process.h,3000);
        std::cout<<(ok?"PASS":"FAIL")<<": Look real observado, ID 3994, argumentos preservados, detach e cancelamento antes do attach\n";return ok?0:1;
    }catch(...){TerminateProcess(process.h,1);WaitForSingleObject(process.h,3000);throw;}
}
int main(int argc,char** argv)try{
    if(argc==3&&std::string(argv[1])=="--fixture"){
        constexpr uintptr_t base=0x140000000;std::array<uint8_t,0x48> item{};uintptr_t vt=base+0xAE9BC0;memcpy(item.data(),&vt,8);uint16_t id=3994;memcpy(item.data()+0x40,&id,2);
        std::array<uintptr_t,2> pair{reinterpret_cast<uintptr_t>(item.data()),0x8888};
        std::cout<<Hex(reinterpret_cast<uintptr_t>(&LookFixture))<<" "<<Hex(reinterpret_cast<uintptr_t>(&fixtureLookCalls))<<"\n"<<std::flush;
        if(WaitForSingleObject(reinterpret_cast<HANDLE>(std::stoull(argv[2])),15000)!=WAIT_OBJECT_0)return 2;
        for(;;){LookFixture(0x1234,pair.data(),1);Sleep(20);}
    }
    if(argc==2&&std::string(argv[1])=="--self-test")return LookSelfTest();
    DWORD pid=0,parentId=0;std::string eventName;
    for(int i=1;i<argc;++i){std::string arg=argv[i];if(i+1>=argc)throw std::runtime_error("Falta argumento");
        if(arg=="--pid")pid=std::stoul(argv[++i]);else if(arg=="--parent")parentId=std::stoul(argv[++i]);else if(arg=="--stop-event")eventName=argv[++i];else throw std::runtime_error("Argumento desconhecido");}
    if(!pid||pid==GetCurrentProcessId()||!parentId||eventName.empty())throw std::runtime_error("PID, parent e stop-event necessarios");
    Handle stop(OpenEventA(SYNCHRONIZE,FALSE,eventName.c_str())),parent(OpenProcess(SYNCHRONIZE,FALSE,parentId));
    if(!stop.h||!parent.h)throw Error("OpenEvent/parent");
    Handle process(OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ,FALSE,pid));if(!process.h)throw Error("OpenProcess Look");
    BOOL debugger=FALSE;if(!CheckRemoteDebuggerPresent(process.h,&debugger))throw Error("CheckRemoteDebuggerPresent");
    if(debugger)throw std::runtime_error("Outro depurador ja esta ativo");
    auto base=CheckBuild(process.h,pid);
    std::array<uint8_t,16> expected{0x48,0x89,0x5c,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x57,0x48,0x81,0xec,0x90,0},actual{};
    if(!Read(process.h,base+0x141C60,actual.data(),actual.size())||actual!=expected)throw std::runtime_error("Game::look diferente");
    SetConsoleCtrlHandler(ConsoleHandler,TRUE);LookWatch watch(pid,process.h,base+0x141C60,base,base+0xE52A40);
    try{watch.RunWatch(stop.h,parent.h);}catch(const std::exception& error){watch.diagnostics.push_back(error.what());watch.Cleanup();}
    watch.Closed();return watch.detached&&watch.diagnostics.empty()?0:2;
}catch(const std::exception& error){std::cerr<<"[ERRO] "<<error.what()<<"\n";return 1;}
