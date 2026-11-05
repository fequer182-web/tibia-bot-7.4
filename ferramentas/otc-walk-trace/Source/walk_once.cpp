#define main TraceUtilitiesEntry
#include "trace_walk.cpp"
#undef main

void CheckShadowStack(HANDLE process){
    PROCESS_MITIGATION_USER_SHADOW_STACK_POLICY policy{};
    if(!GetProcessMitigationPolicy(process,ProcessUserShadowStackPolicy,&policy,sizeof(policy)))throw Error("GetProcessMitigationPolicy shadow stack");
    if(policy.EnableUserShadowStack)throw std::runtime_error("Shadow stack ativo. Este mecanismo nao e compativel; nao alterou a politica nem anexou depurador.");
}
bool GameThreadMatches(HANDLE process,DWORD pid,DWORD thread,uintptr_t idAddress){
    DWORD live=ReadValue<DWORD>(process,idAddress);
    std::cout<<"[THREAD] capturada="<<thread<<" ciclo_do_jogo="<<live<<"\n";
    if(live!=thread)return false;
    Handle handle(OpenThread(THREAD_QUERY_LIMITED_INFORMATION,FALSE,thread));
    return handle.h && GetProcessIdOfThread(handle.h)==pid;
}
volatile DWORD fixtureThreadId=0;
bool Write(HANDLE process,uintptr_t address,const void* data,size_t size){SIZE_T done=0;return WriteProcessMemory(process,reinterpret_cast<void*>(address),data,size,&done)&&done==size;}

struct OneCall:Trace {
    DWORD targetThread;uintptr_t targetFunction,instance;uint32_t direction;
    bool started=false,completed=false,contextRestored=false,stackRestored=false;
    bool writableStackPrepared=false;
    CONTEXT saved{};
    uintptr_t temporaryRsp=0;std::array<uint8_t,8> previousStack{};
    DWORD64 returnedRax=0;unsigned ignoredNested=0;
    Position positionBefore;std::vector<Position> positionAfter;
    OneCall(DWORD pid,HANDLE process,uintptr_t checkpoint,uintptr_t base,DWORD thread,uintptr_t function,uintptr_t object,uint32_t dir,unsigned timeout)
        :Trace(pid,process,checkpoint,base,timeout,1),targetThread(thread),targetFunction(function),instance(object),direction(dir){}
    bool Restore(HANDLE thread,bool afterReturn){
        if(writableStackPrepared){
            std::array<uint8_t,8> check{};
            stackRestored=Write(process,temporaryRsp,previousStack.data(),8)&&Read(process,temporaryRsp,check.data(),8)&&check==previousStack;
            if(!stackRestored)diagnostics.push_back("Falha restaurando os 8 bytes temporarios de pilha");
            writableStackPrepared=false;
        }
        CONTEXT original=saved;
        original.ContextFlags=CONTEXT_ALL;
        original.EFlags|=0x10000; // evita retrap na instrucao original
        contextRestored=!!SetThreadContext(thread,&original);
        if(!contextRestored)diagnostics.push_back("Falha restaurando contexto completo");
        if(afterReturn)completed=contextRestored;
        return contextRestored;
    }
    void Begin(HANDLE thread,CONTEXT& context){
        positionBefore=ReadPosition(process,base);
        if(base){
            if(!positionBefore.valid)throw std::runtime_error("Posicao/VTable nao conferem. Nao fez chamada.");
            std::array<uint8_t,0x80> game{};
            if(!Read(process,instance,game.data(),game.size()))throw Error("Leitura instancia Game");
            uintptr_t pointer0=0,protocol=0;memcpy(&pointer0,game.data(),8);memcpy(&protocol,game.data()+0x30,8);
            uint8_t state=0;
            if(!game[0x60]||game[0x62]||!pointer0||!protocol||!Read(process,pointer0+0x69,&state,1)||!state)
                throw std::runtime_error("Precondicoes observadas de Game::walk nao passaram. Nao fez chamada.");
        }
        saved=context;
        temporaryRsp=((context.Rsp-0x80)&~uintptr_t(15))-8; // entrada x64: RSP % 16 == 8
        MEMORY_BASIC_INFORMATION stack{},current{};
        if(!VirtualQueryEx(process,reinterpret_cast<void*>(temporaryRsp),&stack,sizeof(stack))||
           !VirtualQueryEx(process,reinterpret_cast<void*>(context.Rsp),&current,sizeof(current))||
           stack.AllocationBase!=current.AllocationBase || stack.State!=MEM_COMMIT ||
           (stack.Protect&PAGE_GUARD)||((stack.Protect&255)!=PAGE_READWRITE && (stack.Protect&255)!=PAGE_WRITECOPY)||
           temporaryRsp+40>reinterpret_cast<uintptr_t>(stack.BaseAddress)+stack.RegionSize)
            throw std::runtime_error("Area temporaria de pilha nao passou na verificacao. Nao fez chamada.");
        if(!Read(process,temporaryRsp,previousStack.data(),8))throw Error("Leitura backup pilha");
        if(!Write(process,temporaryRsp,&address,8))throw Error("Preparar retorno na pilha");
        writableStackPrepared=true;
        CONTEXT call=context;
        call.Rip=targetFunction;call.Rsp=temporaryRsp;call.Rcx=instance;call.Rdx=direction;call.R8=1;
        call.EFlags&=~0x100; // sem single-step por instrucao
        call.EFlags|=0x10000;
        call.Dr6=0;
        if(!SetThreadContext(thread,&call)){Restore(thread,false);throw Error("SetThreadContext chamada");}
        started=true;
        std::cout<<"[CHAMADA] Game::walk; thread="<<targetThread<<" direcao="<<direction<<" R8B=1. Nenhuma tecla enviada.\n"<<std::flush;
    }
    void RunOnce(){
        if(!DebugActiveProcess(pid))throw Error("DebugActiveProcess");
        attached=true;
        if(!DebugSetProcessKillOnExit(FALSE)){Cleanup();throw Error("DebugSetProcessKillOnExit");}
        ULONGLONG deadline=GetTickCount64()+seconds*1000ULL;bool initial=true,waitingNotice=false;
        while(!exited && !completed && (started || (!interrupted && GetTickCount64()<deadline))){
            if(started && !waitingNotice && GetTickCount64()>deadline){
                std::cout<<"[AGUARDANDO] A chamada ainda nao retornou. Nao interrompe no meio nem repete a chamada.\n"<<std::flush;waitingNotice=true;
            }
            if(!WaitForDebugEvent(&event,100)){DWORD code=GetLastError();if(code==ERROR_SEM_TIMEOUT||code==0)continue;throw Error("WaitForDebugEvent");}
            pending=true;continuation=DBG_CONTINUE;
            switch(event.dwDebugEventCode){
                case CREATE_PROCESS_DEBUG_EVENT:
                    if(event.u.CreateProcessInfo.hFile)CloseHandle(event.u.CreateProcessInfo.hFile);
                    if(event.dwThreadId==targetThread)Arm(event.dwThreadId,event.u.CreateProcessInfo.hThread);
                    break;
                case CREATE_THREAD_DEBUG_EVENT:
                    if(event.dwThreadId==targetThread)Arm(event.dwThreadId,event.u.CreateThread.hThread);
                    break;
                case LOAD_DLL_DEBUG_EVENT:if(event.u.LoadDll.hFile)CloseHandle(event.u.LoadDll.hFile);break;
                case EXIT_THREAD_DEBUG_EVENT:
                    if(event.dwThreadId==targetThread){auto it=threads.find(targetThread);if(it!=threads.end()){CloseHandle(it->second.handle);threads.erase(it);}throw std::runtime_error("A thread alvo encerrou");}break;
                case EXIT_PROCESS_DEBUG_EVENT:exited=true;diagnostics.push_back("O processo encerrou durante o teste");break;
                case EXCEPTION_DEBUG_EVENT:{
                    continuation=DBG_EXCEPTION_NOT_HANDLED;auto code=event.u.Exception.ExceptionRecord.ExceptionCode;
                    if(initial && code==EXCEPTION_BREAKPOINT){initial=false;continuation=DBG_CONTINUE;
                        std::cout<<"[ARMADO] Aguarde a chamada unica no ciclo de eventos. Nao pressione setas nem clique no mapa.\n"<<std::flush;
                        if(startFixture)SetEvent(startFixture);
                    }else if(code==EXCEPTION_SINGLE_STEP && event.dwThreadId==targetThread){
                        auto it=threads.find(targetThread);if(it==threads.end())break;
                        CONTEXT context{};context.ContextFlags=CONTEXT_ALL;
                        if(!GetThreadContext(it->second.handle,&context))throw Error("GetThreadContext checkpoint");
                        if(context.Rip==address && (context.Dr6&(1ULL<<it->second.slot))){
                            continuation=DBG_CONTINUE;
                            if(!started)Begin(it->second.handle,context);
                            else if(context.Rsp==temporaryRsp+8){
                                returnedRax=context.Rax;
                                if(!Restore(it->second.handle,true))throw std::runtime_error("Nao foi possivel restaurar o contexto apos o retorno");
                                std::cout<<"[RETORNO] Chamada retornou; contexto e pilha restaurados.\n"<<std::flush;
                            }else{
                                ++ignoredNested;context.Dr6&=~(1ULL<<it->second.slot);context.EFlags|=0x10000;
                                if(!SetThreadContext(it->second.handle,&context))throw Error("Resume checkpoint aninhado");
                            }
                        }
                    }
                    break;
                }
            }
            if(completed||exited)break;
            Continue();
        }
        Cleanup();
        if(completed && detached && base){for(int i=0;i<40;++i){Sleep(50);positionAfter.push_back(ReadPosition(process,base));}}
    }
    void SaveResult(const std::filesystem::path& output){
        std::ofstream f(output);if(!f)throw std::runtime_error("Nao conseguiu salvar o relatorio");
        f<<"{\n  \"mode\": \"native_one_call_on_event_thread\",\n  \"pid\": "<<pid<<",\n  \"threadId\": "<<targetThread
         <<",\n  \"function\": \""<<Hex(targetFunction)<<"\",\n  \"instance\": \""<<Hex(instance)<<"\",\n  \"checkpoint\": \""<<Hex(address)
         <<"\",\n  \"direction\": "<<direction<<",\n  \"third_argument\": 1,\n  \"started\": "<<(started?"true":"false")
         <<",\n  \"returned\": "<<(completed?"true":"false")<<",\n  \"context_restored\": "<<(contextRestored?"true":"false")
         <<",\n  \"stack_slot_restored\": "<<(stackRestored?"true":"false")<<",\n  \"detached\": "<<(detached?"true":"false")
         <<",\n  \"raw_rax_at_return\": \""<<Hex(returnedRax)<<"\",\n  \"return_type_interpreted\": false,\n  \"position_before\": ";
        WritePosition(f,positionBefore);f<<",\n  \"positions_after\": [";
        for(size_t i=0;i<positionAfter.size();++i){if(i)f<<", ";WritePosition(f,positionAfter[i]);}
        f<<"],\n  \"diagnostics\": [";for(size_t i=0;i<diagnostics.size();++i){if(i)f<<", ";f<<"\""<<diagnostics[i]<<"\"";}f<<"]\n}\n";
    }
};

__declspec(noinline) void IdleFixture(){asm volatile("" ::: "memory");}
int CallSelfTest(bool timeout=false){
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};Handle ready(CreateEventW(&sa,TRUE,FALSE,nullptr));
    if(!ready.h)throw Error("CreateEvent fixture");
    HANDLE input=nullptr,output=nullptr;if(!CreatePipe(&input,&output,&sa,0))throw Error("CreatePipe");
    Handle read(input),write(output);SetHandleInformation(read.h,HANDLE_FLAG_INHERIT,0);
    std::wstring cmd=L"\""+ExePath().wstring()+L"\" --call-fixture "+std::to_wstring(reinterpret_cast<uintptr_t>(ready.h));
    STARTUPINFOW si{};si.cb=sizeof(si);si.dwFlags=STARTF_USESTDHANDLES;si.hStdOutput=write.h;si.hStdError=write.h;si.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};if(!CreateProcessW(nullptr,cmd.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi))throw Error("CreateProcess fixture");
    Handle process(pi.hProcess),thread(pi.hThread);
    try{
        std::string line;char c=0;DWORD got=0;while(ReadFile(read.h,&c,1,&got,nullptr)&&got&&c!='\n')line+=c;
        std::istringstream values(line);std::string checkpoint,function,sink,tidAddress;values>>checkpoint>>function>>sink>>tidAddress;
        if(!GameThreadMatches(process.h,pi.dwProcessId,pi.dwThreadId,std::stoull(tidAddress,nullptr,0)) ||
           GameThreadMatches(process.h,pi.dwProcessId,GetCurrentThreadId(),std::stoull(tidAddress,nullptr,0)) ||
           GameThreadMatches(process.h,GetCurrentProcessId(),pi.dwThreadId,std::stoull(tidAddress,nullptr,0)))
            throw std::runtime_error("Validacao de thread no fixture falhou");
        CheckShadowStack(process.h);
        OneCall test(pi.dwProcessId,process.h,std::stoull(checkpoint,nullptr,0)+(timeout?1:0),0,pi.dwThreadId,std::stoull(function,nullptr,0),0x1234,1,timeout?2:5);
        test.startFixture=ready.h;test.RunOnce();
        Sleep(300);DWORD code=0;bool ok=test.detached && test.diagnostics.empty() && GetExitCodeProcess(process.h,&code)&&code==STILL_ACTIVE;
        if(timeout)ok=ok && !test.started && !test.completed && ReadValue<uint64_t>(process.h,std::stoull(sink,nullptr,0))==0;
        else ok=ok && test.started && test.completed && test.contextRestored && test.stackRestored && ReadValue<uint64_t>(process.h,std::stoull(sink,nullptr,0))==0x1236;
        auto report=std::filesystem::temp_directory_path()/(std::string("cromin_walk_once_selftest_")+(timeout?"timeout_":"call_")+std::to_string(pi.dwProcessId)+".json");
        test.SaveResult(report);std::wcout<<L"[SELFTEST_JSON] "<<report.wstring()<<L"\n";
        std::cout<<(ok?"PASS":"FAIL")<<": "<<(timeout?"checkpoint nao alcancado: nenhuma chamada, timeout e detach":"chamada propria no fixture, resultado 0x1236, retorno, contexto, pilha e detach")<<".\n";
        TerminateProcess(process.h,0);WaitForSingleObject(process.h,3000);return ok?0:1;
    }catch(...){TerminateProcess(process.h,1);WaitForSingleObject(process.h,3000);throw;}
}

#ifndef CROMIN_WALK_LIB
int main(int argc,char** argv)try{
    if(argc==3&&std::string(argv[1])=="--call-fixture"){
        fixtureThreadId=GetCurrentThreadId();
        std::cout<<Hex(reinterpret_cast<uintptr_t>(&IdleFixture))<<" "<<Hex(reinterpret_cast<uintptr_t>(&Fixture))<<" "<<Hex(reinterpret_cast<uintptr_t>(&fixtureSink))<<" "<<Hex(reinterpret_cast<uintptr_t>(&fixtureThreadId))<<"\n"<<std::flush;
        if(WaitForSingleObject(reinterpret_cast<HANDLE>(std::stoull(argv[2])),15000)!=WAIT_OBJECT_0)return 2;
        for(;;){IdleFixture();Sleep(10);}
    }
    if(argc==2&&std::string(argv[1])=="--self-test")return CallSelfTest();
    if(argc==2&&std::string(argv[1])=="--self-test-timeout")return CallSelfTest(true);
    DWORD pid=0,thread=0;int direction=-1;unsigned seconds=20;
    for(int i=1;i<argc;++i){std::string arg=argv[i];
        if(arg=="--pid"&&i+1<argc)pid=std::stoul(argv[++i]);
        else if(arg=="--thread"&&i+1<argc)thread=std::stoul(argv[++i]);
        else if(arg=="--seconds"&&i+1<argc)seconds=std::stoul(argv[++i]);
        else if(arg=="--call-once"&&i+1<argc){std::string dir=argv[++i];direction=dir=="north"?0:dir=="east"?1:dir=="south"?2:dir=="west"?3:-1;if(direction<0)throw std::runtime_error("Direcao invalida");}
        else throw std::runtime_error("Argumento desconhecido");
    }
    if(!pid||!thread||direction<0){std::cout<<"Uso: OTC_Walk_Once.exe --pid NUMERO --thread THREAD_CAPTURADA --call-once east\n"
        <<"Faz uma chamada propria em Game::walk. Altera contexto e grava 8 bytes temporarios na pilha.\n";return 0;}
    if(seconds<5||seconds>60||pid==GetCurrentProcessId())throw std::runtime_error("Parametros invalidos");
    Handle process(OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ|PROCESS_VM_WRITE|PROCESS_VM_OPERATION,FALSE,pid));if(!process.h)throw Error("OpenProcess");
    BOOL existing=FALSE;if(!CheckRemoteDebuggerPresent(process.h,&existing))throw Error("CheckRemoteDebuggerPresent");if(existing)throw std::runtime_error("Ja existe depurador no cliente");
    uintptr_t base=CheckBuild(process.h,pid);CheckShadowStack(process.h);
    // Esta build guarda GetCurrentThreadId() no inicio do ciclo em RVA E53E18.
    if(!GameThreadMatches(process.h,pid,thread,base+0xE53E18))throw std::runtime_error("Thread capturada nao corresponde ao ciclo atual do jogo ou pertence a outro processo. Refaca o trace.");
    // Retorno observado na pilha real, depois do processamento de eventos/Lua.
    std::array<uint8_t,16> expected{0xE8,0x39,0x85,0xFF,0xFF,0x48,0x8B,0x5E,0x08,0x48,0x8B,0xCB,0xE8,0x2D,0x9A,0x61},actual{};
    if(!Read(process.h,base+0x28E9B2,actual.data(),actual.size())||actual!=expected)throw std::runtime_error("Checkpoint do ciclo de eventos diferente");
    SetConsoleCtrlHandler(ConsoleHandler,TRUE);
    std::cout<<"Build, checkpoint, politica e thread conferidos. Nao use setas nem clique no mapa.\n";
    OneCall call(pid,process.h,base+0x28E9B2,base,thread,base+0x140D60,base+0xE52A40,uint32_t(direction),seconds);
    auto path=ExePath().parent_path()/("walk_once_"+std::to_string(GetTickCount64())+".json");
    try{call.RunOnce();}catch(...){call.Cleanup();call.SaveResult(path);throw;}
    call.SaveResult(path);
    std::wcout<<L"[JSON] "<<path.wstring()<<L"\n";
    if(!call.positionAfter.empty()){auto& p=call.positionAfter.back();std::cout<<"Antes "<<call.positionBefore.x<<","<<call.positionBefore.y<<","<<unsigned(call.positionBefore.z)<<" | Depois "<<p.x<<","<<p.y<<","<<unsigned(p.z)<<"\n";}
    return call.completed&&call.detached&&call.diagnostics.empty()?0:2;
}catch(const std::exception& error){std::cerr<<"[ERRO] "<<error.what()<<"\n";return 1;}
#endif
