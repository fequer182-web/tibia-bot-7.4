// Container item actions: reuse the proven serial checkpoint/restore framework.
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
bool enteredNativeRun=false;
bool Write(HANDLE process,uintptr_t address,const void* data,size_t size){SIZE_T done=0;return WriteProcessMemory(process,reinterpret_cast<void*>(address),data,size,&done)&&done==size;}

struct NativePosition {uint32_t x=0,y=0;uint16_t z=0,padding=0;};
static_assert(sizeof(NativePosition)==12);
struct BorrowedItem {uintptr_t object=0,control=0;};
static_assert(sizeof(BorrowedItem)==16);
struct ContainerState {uintptr_t object=0,bagObject=0;uint32_t id=0,capacity=0;uint16_t bagItemId=0;uint64_t count=0;std::vector<BorrowedItem> items;};
void Require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
uint16_t ItemField(HANDLE process,uintptr_t base,uintptr_t object,uintptr_t offset){
    Require(object && ReadValue<uintptr_t>(process,object)==base+0xAE9BC0,"Item/VTable diferente");
    return ReadValue<uint16_t>(process,object+offset);
}
ContainerState ContainerNow(HANDLE process,uintptr_t base,unsigned wanted){
    uintptr_t map=base+0xE52A40+0x40;
    auto header=ReadValue<uintptr_t>(process,map);auto size=ReadValue<uint64_t>(process,map+8);
    Require(header&&size>0&&size<=256,"Mapa de containers vazio ou invalido");
    Require(ReadValue<uint8_t>(process,header+0x19)==1,"Sentinela de containers diferente");
    std::vector<std::pair<uintptr_t,uintptr_t>> pending{{ReadValue<uintptr_t>(process,header+8),header}};
    std::vector<uintptr_t> seen;ContainerState result{};
    while(!pending.empty()){
        auto entry=pending.back();pending.pop_back();uintptr_t node=entry.first;
        if(node==header)continue;
        Require(node&&seen.size()<size&&std::find(seen.begin(),seen.end(),node)==seen.end(),"Arvore ciclica ou divergente");seen.push_back(node);
        Require(ReadValue<uint8_t>(process,node+0x19)==0&&ReadValue<uintptr_t>(process,node+8)==entry.second,"No de container mudou");
        auto id=ReadValue<uint32_t>(process,node+0x20);
        if(id==wanted){Require(!result.object,"Container ID duplicado");result.object=ReadValue<uintptr_t>(process,node+0x28);result.id=id;}
        pending.push_back({ReadValue<uintptr_t>(process,node),node});pending.push_back({ReadValue<uintptr_t>(process,node+0x10),node});
    }
    Require(seen.size()==size&&ReadValue<uintptr_t>(process,map)==header&&ReadValue<uint64_t>(process,map+8)==size,"Mapa mudou durante leitura");
    Require(result.object&&ReadValue<uintptr_t>(process,result.object)==base+0xAE9230,"Container nao encontrado ou VTable diferente");
    auto object=result.object;
    Require(ReadValue<uint32_t>(process,object+0x20)==wanted,"Identidade do container divergiu");
    result.capacity=ReadValue<uint32_t>(process,object+0x24);
    result.bagObject=ReadValue<uintptr_t>(process,object+0x28);
    result.bagItemId=ItemField(process,base,result.bagObject,0x40);
    auto table=ReadValue<uintptr_t>(process,object+0x70);auto tableSize=ReadValue<uint64_t>(process,object+0x78);
    auto offset=ReadValue<uint64_t>(process,object+0x80);result.count=ReadValue<uint64_t>(process,object+0x88);
    Require(result.capacity>0&&result.capacity<=10000&&result.count<=result.capacity&&result.count<=512,"Limites de container divergentes");
    if(result.count)Require(table&&tableSize&&!(tableSize&(tableSize-1))&&tableSize<=65536&&result.count<=tableSize,"Deque invalida");
    for(uint64_t i=0;i<result.count;++i){
        auto block=ReadValue<uintptr_t>(process,table+((offset+i)&(tableSize-1))*8);
        BorrowedItem item=ReadValue<BorrowedItem>(process,block);
        Require(item.object&&item.control,"shared_ptr de item vazio");
        ItemField(process,base,item.object,0x40);result.items.push_back(item);
    }
    Require(ReadValue<uintptr_t>(process,object+0x70)==table&&ReadValue<uint64_t>(process,object+0x78)==tableSize&&ReadValue<uint64_t>(process,object+0x80)==offset&&ReadValue<uint64_t>(process,object+0x88)==result.count,"Deque mudou durante leitura");
    return result;
}

struct ItemSpec {
    unsigned source=999,slot=999,id=3577,raw=0,bagId=0;
    uintptr_t container=0,bag=0,object=0;
    bool open=false;
} spec;
BorrowedItem ResolveContained(HANDLE process,uintptr_t base){
    Require(spec.source<=63&&spec.slot<=511&&spec.id>0&&spec.id<=65535,"Parametros de item invalidos");
    Require(!spec.open||spec.id==2853,"Abertura interna limitada a bag 2853");
    auto c=ContainerNow(process,base,spec.source);
    Require(c.object==spec.container&&c.bagObject==spec.bag&&c.bagItemId==spec.bagId,"Container de origem mudou");
    Require(spec.slot<c.items.size(),"Slot indisponivel");auto item=c.items[spec.slot];
    Require(item.object==spec.object&&ItemField(process,base,item.object,0x40)==spec.id&&ItemField(process,base,item.object,0x44)==spec.raw,"Item mudou desde a leitura");
    auto pos=ReadValue<NativePosition>(process,item.object+0x20);
    Require(pos.x==65535&&pos.y==(0x40|spec.source)&&pos.z==spec.slot,"Posicao do item divergiu do slot");
    return item;
}
struct OneCall:Trace {
    BorrowedItem item{};uintptr_t executedThis=0;
    DWORD targetThread;uintptr_t targetFunction,instance;uint32_t direction;
    bool started=false,completed=false,contextRestored=false,stackRestored=false;
    bool writableStackPrepared=false;
    CONTEXT saved{};
    uintptr_t temporaryRsp=0;std::array<uint8_t,96> previousStack{};
    DWORD64 returnedRax=0;unsigned ignoredNested=0;
    Position positionBefore;std::vector<Position> positionAfter;
    OneCall(DWORD pid,HANDLE process,uintptr_t checkpoint,uintptr_t base,DWORD thread,uintptr_t function,uintptr_t object,uint32_t dir,unsigned timeout)
        :Trace(pid,process,checkpoint,base,timeout,1),targetThread(thread),targetFunction(function),instance(object),direction(dir){}
    bool Restore(HANDLE thread,bool afterReturn){
        if(writableStackPrepared){
            std::array<uint8_t,96> check{};
            stackRestored=Write(process,temporaryRsp,previousStack.data(),previousStack.size())&&Read(process,temporaryRsp,check.data(),check.size())&&check==previousStack;
            if(!stackRestored)diagnostics.push_back("Falha restaurando os 96 bytes temporarios de pilha");
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
        uintptr_t liveThis=instance;
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
        if(base)item=ResolveContained(process,base);
        saved=context;
        temporaryRsp=((context.Rsp-0x100)&~uintptr_t(15))-8; // entrada x64: RSP % 16 == 8
        MEMORY_BASIC_INFORMATION stack{},current{};
        if(!VirtualQueryEx(process,reinterpret_cast<void*>(temporaryRsp),&stack,sizeof(stack))||
           !VirtualQueryEx(process,reinterpret_cast<void*>(context.Rsp),&current,sizeof(current))||
           stack.AllocationBase!=current.AllocationBase || stack.State!=MEM_COMMIT ||
           (stack.Protect&PAGE_GUARD)||((stack.Protect&255)!=PAGE_READWRITE && (stack.Protect&255)!=PAGE_WRITECOPY)||
           temporaryRsp+previousStack.size()>reinterpret_cast<uintptr_t>(stack.BaseAddress)+stack.RegionSize)
            throw std::runtime_error("Area temporaria de pilha nao passou na verificacao. Nao fez chamada.");
        if(!Read(process,temporaryRsp,previousStack.data(),previousStack.size()))throw Error("Leitura backup pilha");
        auto frame=previousStack;
        memcpy(frame.data(),&address,8);
        memcpy(frame.data()+40,&item,sizeof(item));
        BorrowedItem empty{};memcpy(frame.data()+56,&empty,sizeof(empty));
        writableStackPrepared=true;
        if(!Write(process,temporaryRsp,frame.data(),frame.size())){Restore(thread,false);throw Error("Preparar retorno e destino na pilha");}
        CONTEXT call=context;
        executedThis=liveThis;
        call.Rip=targetFunction;call.Rsp=temporaryRsp;call.Rcx=liveThis;call.Rdx=temporaryRsp+40;call.R8=spec.open?temporaryRsp+56:0;call.R9=0;
        call.EFlags&=~0x100; // sem single-step por instrucao
        call.EFlags|=0x10000;
        call.Dr6=0;
        if(!SetThreadContext(thread,&call)){Restore(thread,false);throw Error("SetThreadContext chamada");}
        started=true;
        std::cout<<"[CHAMADA] "<<(spec.open?"Game::open bag 2853":"Game::use food")<<"; ID="<<spec.id<<". Uma chamada.\n"<<std::flush;
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
        if(completed && detached && base){positionAfter.push_back(ReadPosition(process,base));}
    }
    void SaveResult(const std::filesystem::path& output){
        std::ofstream f(output);if(!f)throw std::runtime_error("Nao conseguiu salvar relatorio");
        f<<"{\n\"action\":\""<<(spec.open?"open_bag":"use_food")<<"\",\n\"item_id\":"<<spec.id
         <<",\n\"borrowed_item\":\""<<Hex(item.object)<<"\",\n\"source_container\":"<<spec.source<<",\n\"source_slot\":"<<spec.slot
         <<",\n\"started\":"<<(started?"true":"false")<<",\n\"returned\":"<<(completed?"true":"false")
         <<",\n\"context_restored\":"<<(contextRestored?"true":"false")<<",\n\"stack_slot_restored\":"<<(stackRestored?"true":"false")
         <<",\n\"detached\":"<<(detached?"true":"false")<<",\n\"preflight_skipped\":"<<((!started&&!writableStackPrepared&&detached&&diagnostics.empty())?"true":"false")
         <<",\n\"raw_rax_at_return\":\""<<Hex(returnedRax)<<"\",\n\"diagnostics\":[";
        for(size_t i=0;i<diagnostics.size();++i){if(i)f<<",";f<<"\""<<diagnostics[i]<<"\"";}f<<"]\n}\n";
    }
};

void SelectionSelfTest(){
    struct Allocation{void* p=VirtualAlloc(nullptr,0xEDE000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);~Allocation(){if(p)VirtualFree(p,0,MEM_RELEASE);}} allocation;
    Require(allocation.p,"Fixture allocation");auto base=reinterpret_cast<uintptr_t>(allocation.p);
    std::array<uint8_t,0x38> head{},node{};std::array<uint8_t,0x90> container{};std::array<uint8_t,0x48> bag{},food{};std::array<uintptr_t,8> table{};
    auto ptr=[](auto& x){return reinterpret_cast<uintptr_t>(x.data());};auto put=[](auto& a,size_t o,auto v){memcpy(a.data()+o,&v,sizeof(v));};
    BorrowedItem block{ptr(food),0x2222};auto h=ptr(head),n=ptr(node);table[0]=reinterpret_cast<uintptr_t>(&block);
    put(head,8,n);put(head,0x19,uint8_t(1));put(node,0,h);put(node,8,h);put(node,16,h);put(node,0x20,uint32_t(1));put(node,0x28,ptr(container));
    memcpy(reinterpret_cast<void*>(base+0xE52A80),&h,8);uint64_t one=1;memcpy(reinterpret_cast<void*>(base+0xE52A88),&one,8);
    put(container,0,base+0xAE9230);put(container,0x20,uint32_t(1));put(container,0x24,uint32_t(8));put(container,0x28,ptr(bag));
    put(container,0x70,ptr(table));put(container,0x78,uint64_t(8));put(container,0x80,uint64_t(0));put(container,0x88,one);
    put(bag,0,base+0xAE9BC0);put(bag,0x40,uint16_t(2853));put(food,0,base+0xAE9BC0);put(food,0x40,uint16_t(3577));put(food,0x44,uint16_t(2));put(food,0x20,NativePosition{65535,65,0,0});
    spec={1,0,3577,2,2853,ptr(container),ptr(bag),ptr(food),false};auto process=GetCurrentProcess();
    Require(ResolveContained(process,base).object==ptr(food),"Food valido rejeitado");
    auto refuse=[&](){bool refused=false;try{ResolveContained(process,base);}catch(...){refused=true;}Require(refused,"Aceitou item alterado");};
    spec.raw=3;refuse();spec.raw=2;spec.object+=8;refuse();spec.object-=8;spec.open=true;refuse();
    spec.id=2853;put(food,0x40,uint16_t(2853));Require(ResolveContained(process,base).object==ptr(food),"Bag interna rejeitada");
    spec.container+=8;refuse();spec=ItemSpec{};
    std::cout<<"PASS: selecao por ID, objeto, container, slot, subtipo e bag 2853.\n";
}
__declspec(noinline) void IdleFixture(){asm volatile("" ::: "memory");}
__declspec(noinline) uint64_t ItemFixture(uintptr_t instance,const BorrowedItem* item,const BorrowedItem* empty){
    if(instance==0x1234&&item->object==0x1111&&item->control==0x2222&&(!empty||(!empty->object&&!empty->control)))fixtureSink=empty?0x7778:0x7777;
    asm volatile("" ::: "memory");return 7;
}
int ItemActionSelfTest(bool open,bool timeout){
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};Handle ready(CreateEventW(&sa,TRUE,FALSE,nullptr));Require(ready.h,"CreateEvent");
    HANDLE input=nullptr,output=nullptr;Require(CreatePipe(&input,&output,&sa,0),"CreatePipe");Handle read(input),write(output);SetHandleInformation(read.h,HANDLE_FLAG_INHERIT,0);
    std::wstring command=L"\""+ExePath().wstring()+L"\" --fixture "+std::to_wstring(reinterpret_cast<uintptr_t>(ready.h));
    STARTUPINFOW si{};si.cb=sizeof(si);si.dwFlags=STARTF_USESTDHANDLES;si.hStdOutput=write.h;si.hStdError=write.h;si.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};Require(CreateProcessW(nullptr,command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi),"CreateProcess fixture");
    Handle process(pi.hProcess),thread(pi.hThread);
    try{
        std::string line;char c;DWORD got;while(ReadFile(read.h,&c,1,&got,nullptr)&&got&&c!='\n')line+=c;
        std::istringstream values(line);std::string checkpoint,function,sink;values>>checkpoint>>function>>sink;CheckShadowStack(process.h);
        spec.open=open;spec.id=open?2853:3577;
        OneCall test(pi.dwProcessId,process.h,std::stoull(checkpoint,nullptr,0)+(timeout?1:0),0,pi.dwThreadId,std::stoull(function,nullptr,0),0x1234,0,timeout?2:5);
        test.item={0x1111,0x2222};test.startFixture=ready.h;test.RunOnce();Sleep(50);
        auto value=ReadValue<uint64_t>(process.h,std::stoull(sink,nullptr,0));
        bool ok=test.detached&&test.diagnostics.empty();
        if(timeout)ok=ok&&!test.started&&value==0;
        else ok=ok&&test.completed&&test.contextRestored&&test.stackRestored&&value==(open?0x7778:0x7777)&&test.item.object==0x1111;
        auto path=std::filesystem::temp_directory_path()/("cromin_item_fixture_"+std::to_string(pi.dwProcessId)+".json");test.SaveResult(path);std::wcout<<L"[SELFTEST_JSON] "<<path.wstring()<<L"\n";
        TerminateProcess(process.h,0);WaitForSingleObject(process.h,3000);std::cout<<(ok?"PASS":"FAIL")<<": item action ABI, restore, detach.\n";return ok?0:1;
    }catch(...){TerminateProcess(process.h,1);WaitForSingleObject(process.h,3000);throw;}
}

int main(int argc,char** argv)try{
    if(argc==3&&std::string(argv[1])=="--fixture"){
        std::cout<<Hex(reinterpret_cast<uintptr_t>(&IdleFixture))<<" "<<Hex(reinterpret_cast<uintptr_t>(&ItemFixture))<<" "<<Hex(reinterpret_cast<uintptr_t>(&fixtureSink))<<"\n"<<std::flush;
        Require(WaitForSingleObject(reinterpret_cast<HANDLE>(std::stoull(argv[2])),15000)==WAIT_OBJECT_0,"Fixture event");for(;;){IdleFixture();Sleep(10);}
    }
    if(argc==2&&std::string(argv[1])=="--self-test"){SelectionSelfTest();if(ItemActionSelfTest(false,false))return 1;return ItemActionSelfTest(true,false);}
    if(argc==2&&std::string(argv[1])=="--self-test-timeout")return ItemActionSelfTest(false,true);
    DWORD pid=0,tid=0;
    for(int i=1;i<argc;++i){std::string arg=argv[i];Require(i+1<argc,"Falta argumento");std::string value=argv[++i];
        if(arg=="--pid")pid=std::stoul(value);else if(arg=="--thread")tid=std::stoul(value);
        else if(arg=="--source")spec.source=std::stoul(value);else if(arg=="--slot")spec.slot=std::stoul(value);
        else if(arg=="--item-id")spec.id=std::stoul(value);else if(arg=="--raw")spec.raw=std::stoul(value);else if(arg=="--source-item-id")spec.bagId=std::stoul(value);
        else if(arg=="--source-object")spec.container=std::stoull(value,nullptr,0);else if(arg=="--source-bag-object")spec.bag=std::stoull(value,nullptr,0);else if(arg=="--item-object")spec.object=std::stoull(value,nullptr,0);
        else if(arg=="--action"){Require(value=="open"||value=="use","Acao invalida");spec.open=value=="open";}else throw std::runtime_error("Argumento desconhecido");
    }
    Require(pid&&tid&&pid!=GetCurrentProcessId(),"PID/thread invalidos");
    Handle process(OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ|PROCESS_VM_WRITE|PROCESS_VM_OPERATION,FALSE,pid));Require(process.h,"OpenProcess");
    BOOL debug=FALSE;Require(CheckRemoteDebuggerPresent(process.h,&debug)&&!debug,"Cliente ja possui depurador");
    auto base=CheckBuild(process.h,pid);CheckShadowStack(process.h);Require(GameThreadMatches(process.h,pid,tid,base+0xE53E18),"Thread diferente");
    std::array<uint8_t,16> checkpoint{0xE8,0x39,0x85,0xFF,0xFF,0x48,0x8B,0x5E,0x08,0x48,0x8B,0xCB,0xE8,0x2D,0x9A,0x61},actual{};
    Require(Read(process.h,base+0x28E9B2,actual.data(),actual.size())&&actual==checkpoint,"Checkpoint diferente");
    std::vector<uint8_t> expected=spec.open?std::vector<uint8_t>{0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x7c,0x24,0x18,0x41,0x56,0x48,0x83,0xec,0x40}:std::vector<uint8_t>{0x48,0x89,0x5c,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x55,0x57,0x41,0x54,0x41,0x56,0x41,0x57,0x48,0x8d,0x6c,0x24,0xc9,0x48,0x81,0xec,0xb0,0x0,0x0,0x0,0x4c,0x8b},observed(expected.size());
    auto function=base+(spec.open?0x142EE0:0x142260);Require(Read(process.h,function,observed.data(),observed.size())&&observed==expected,"Funcao de item diferente");
    ResolveContained(process.h,base);SetConsoleCtrlHandler(ConsoleHandler,TRUE);
    OneCall call(pid,process.h,base+0x28E9B2,base,tid,function,base+0xE52A40,0,20);
    auto report=ExePath().parent_path()/("item_action_"+std::to_string(GetTickCount64())+".json");enteredNativeRun=true;
    try{call.RunOnce();}catch(...){call.Cleanup();call.SaveResult(report);std::wcout<<L"[JSON] "<<report.wstring()<<L"\n";throw;}
    call.SaveResult(report);std::wcout<<L"[JSON] "<<report.wstring()<<L"\n";
    return call.completed&&call.contextRestored&&call.stackRestored&&call.detached&&call.diagnostics.empty()?0:2;
}catch(const std::exception& error){
    std::cerr<<"[ERRO] "<<error.what()<<"\n";
    if(!enteredNativeRun){auto path=ExePath().parent_path()/("item_preflight_"+std::to_string(GetTickCount64())+".json");std::ofstream f(path);f<<"{\"preflight_skipped\":true,\"started\":false,\"detached\":true,\"diagnostics\":[]}\n";f.close();std::wcout<<L"[JSON] "<<path.wstring()<<L"\n";}
    return 1;
}
