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
struct MovePlan {BorrowedItem handle;NativePosition destination;};
MovePlan ResolveMove(HANDLE process,uintptr_t base,unsigned source,unsigned slot,unsigned destination,unsigned count){
    Require(source<=63&&destination<=63&&source!=destination&&count>=1&&count<=100,"Parametros de transferencia invalidos");
    auto from=ContainerNow(process,base,source);auto to=ContainerNow(process,base,destination);
    Require(from.bagItemId==3994,"Origem nao e o corpo dead rat 3994 validado neste teste");
    Require(to.bagItemId==2853,"Destino nao e a bag 2853 validada neste teste");
    // O containerItem recebido ao abrir um corpo pode ser um Item apenas descritivo,
    // com Position invalida (FFFF,FFFF,FF). Nao representa o objeto no mapa.
    Require(ReadPosition(process,base).valid,"Posicao do jogador invalida");
    Require(slot<from.items.size(),"Slot de origem indisponivel");
    auto item=from.items[slot];auto id=ItemField(process,base,item.object,0x40);auto amount=ItemField(process,base,item.object,0x44);
    Require(id==3031&&amount>=count&&amount<=100,"Slot nao contem moedas 3031 suficientes");
    auto position=ReadValue<NativePosition>(process,item.object+0x20);
    Require(position.x==65535&&position.y==(0x40|source)&&position.z==slot,"Posicao do item nao corresponde ao slot de origem");
    unsigned target=static_cast<unsigned>(to.count);
    for(unsigned i=0;i<to.items.size();++i){
        if(ItemField(process,base,to.items[i].object,0x40)==3031){
            auto existing=ItemField(process,base,to.items[i].object,0x44);
            if(existing>0&&existing+count<=100){target=i;break;}
        }
    }
    Require(target<to.capacity&&target<=255,"Bag sem espaco/stack compativel");
    return {item,{65535,0x40|destination,static_cast<uint16_t>(target),0}};
}

struct OneCall:Trace {
    NativePosition destination{};uintptr_t executedThis=0;
    unsigned sourceId=1,destinationId=0,sourceSlot=0,requestedCount=1;MovePlan move{};
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
        if(base){move=ResolveMove(process,base,sourceId,sourceSlot,destinationId,requestedCount);destination=move.destination;}
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
        memcpy(frame.data()+40,&move.handle,sizeof(move.handle));
        memcpy(frame.data()+56,&destination,sizeof(destination));
        if(!Write(process,temporaryRsp,frame.data(),frame.size()))throw Error("Preparar retorno e destino na pilha");
        writableStackPrepared=true;
        CONTEXT call=context;
        executedThis=liveThis;
        call.Rip=targetFunction;call.Rsp=temporaryRsp;call.Rcx=liveThis;call.Rdx=temporaryRsp+40;call.R8=temporaryRsp+56;call.R9=requestedCount;
        call.EFlags&=~0x100; // sem single-step por instrucao
        call.EFlags|=0x10000;
        call.Dr6=0;
        if(!SetThreadContext(thread,&call)){Restore(thread,false);throw Error("SetThreadContext chamada");}
        started=true;
        std::cout<<"[CHAMADA] Game::move; thread="<<targetThread<<" moeda=3031 quantidade="<<requestedCount<<" origem="<<sourceId<<":"<<sourceSlot<<" destino="<<destinationId<<":"<<destination.z<<". Uma chamada.\n"<<std::flush;
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
        f<<"{\n  \"mode\": \"native_coin_move_one_call_on_game_thread\",\n  \"pid\": "<<pid<<",\n  \"threadId\": "<<targetThread
         <<",\n  \"function\": \""<<Hex(targetFunction)<<"\",\n  \"game_instance\": \""<<Hex(instance)<<"\",\n  \"checkpoint\": \""<<Hex(address)
         <<"\",\n  \"executed_this\": \""<<Hex(executedThis)<<"\",\n  \"destination\": {\"x\": "<<destination.x<<", \"y\": "<<destination.y<<", \"z\": "<<destination.z<<"},\n  \"item_id\": 3031,\n  \"source_container\": "<<sourceId<<",\n  \"source_slot\": "<<sourceSlot<<",\n  \"destination_container\": "<<destinationId<<",\n  \"requested_count\": "<<requestedCount<<",\n  \"borrowed_item\": \""<<Hex(move.handle.object)<<"\",\n  \"borrowed_control\": \""<<Hex(move.handle.control)<<"\",\n  \"stack_bytes_preserved\": 96,\n  \"started\": "<<(started?"true":"false")
         <<",\n  \"returned\": "<<(completed?"true":"false")<<",\n  \"context_restored\": "<<(contextRestored?"true":"false")
         <<",\n  \"stack_slot_restored\": "<<(stackRestored?"true":"false")<<",\n  \"detached\": "<<(detached?"true":"false")
         <<",\n  \"raw_rax_at_return\": \""<<Hex(returnedRax)<<"\",\n  \"return_type_interpreted\": false,\n  \"position_before\": ";
        WritePosition(f,positionBefore);f<<",\n  \"positions_after\": [";
        for(size_t i=0;i<positionAfter.size();++i){if(i)f<<", ";WritePosition(f,positionAfter[i]);}
        f<<"],\n  \"diagnostics\": [";for(size_t i=0;i<diagnostics.size();++i){if(i)f<<", ";f<<"\""<<diagnostics[i]<<"\"";}f<<"]\n}\n";
    }
};

void ValidationSelfTest(){
    struct Allocation {void* p=VirtualAlloc(nullptr,0xEDE000,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);~Allocation(){if(p)VirtualFree(p,0,MEM_RELEASE);}} allocation;
    Require(allocation.p!=nullptr,"VirtualAlloc fixture");uintptr_t base=reinterpret_cast<uintptr_t>(allocation.p);
    std::array<uint8_t,0x38> head{},n0{},n1{},n2{};
    std::array<uint8_t,0x90> from{},to{};
    std::array<uint8_t,0x48> body{},bag{},coin{},existing{};
    std::array<uint8_t,0x80> player{};
    std::array<uintptr_t,8> sourceTable{},destinationTable{};
    BorrowedItem sourceBlock{reinterpret_cast<uintptr_t>(coin.data()),0x5555},destinationBlock{reinterpret_cast<uintptr_t>(existing.data()),0x6666};
    auto pointer=[](auto& data){return reinterpret_cast<uintptr_t>(data.data());};
    auto put=[](auto& data,size_t offset,auto value){memcpy(data.data()+offset,&value,sizeof(value));};
    auto h=pointer(head),a=pointer(n0),b=pointer(n1),empty=pointer(n2);
    put(head,0,a);put(head,8,a);put(head,16,empty);put(head,0x19,uint8_t(1));
    put(n0,0,h);put(n0,8,h);put(n0,16,b);put(n0,0x20,uint32_t(0));put(n0,0x28,pointer(to));
    put(n1,0,h);put(n1,8,a);put(n1,16,empty);put(n1,0x20,uint32_t(1));put(n1,0x28,pointer(from));
    put(n2,0,h);put(n2,8,b);put(n2,16,h);put(n2,0x20,uint32_t(2));
    memcpy(reinterpret_cast<void*>(base+0xE52A80),&h,8);uint64_t three=3;memcpy(reinterpret_cast<void*>(base+0xE52A88),&three,8);
    for(auto* value:{&body,&bag,&coin,&existing})put(*value,0,base+0xAE9BC0);
    put(body,0x40,uint16_t(3994));put(body,0x20,NativePosition{100,101,7,0});
    put(bag,0x40,uint16_t(2853));put(coin,0x40,uint16_t(3031));put(coin,0x44,uint16_t(3));put(coin,0x20,NativePosition{65535,65,0,0});
    put(existing,0x40,uint16_t(3031));put(existing,0x44,uint16_t(15));
    sourceTable[7]=reinterpret_cast<uintptr_t>(&sourceBlock);destinationTable[0]=reinterpret_cast<uintptr_t>(&destinationBlock);
    auto setup=[&](auto& c,unsigned id,uintptr_t item,auto& table,uint64_t offset){put(c,0,base+0xAE9230);put(c,0x20,uint32_t(id));put(c,0x24,uint32_t(8));put(c,0x28,item);put(c,0x70,pointer(table));put(c,0x78,uint64_t(8));put(c,0x80,offset);put(c,0x88,uint64_t(1));};
    setup(from,1,pointer(body),sourceTable,7);setup(to,0,pointer(bag),destinationTable,0);
    put(player,0,base+0xAE99E0);put(player,0x30,uint32_t(100));put(player,0x34,uint32_t(100));put(player,0x38,uint8_t(7));
    auto lp=pointer(player);memcpy(reinterpret_cast<void*>(base+0xE52A48),&lp,8);
    auto resolve=[&](unsigned count){return ResolveMove(GetCurrentProcess(),base,1,0,0,count);};
    auto refuses=[&](unsigned count){bool refused=false;try{resolve(count);}catch(...){refused=true;}Require(refused,"Validacao aceitou transferencia invalida");};
    auto result=resolve(3);Require(result.handle.object==sourceBlock.object&&result.handle.control==sourceBlock.control&&result.destination.x==65535&&result.destination.y==64&&result.destination.z==0,"Plano de merge incorreto");
    refuses(4);
    put(existing,0x44,uint16_t(98));Require(resolve(3).destination.z==1,"Nao escolheu slot livre");put(to,0x24,uint32_t(1));refuses(3);put(to,0x24,uint32_t(8));
    put(body,0x40,uint16_t(2853));refuses(1);put(body,0x40,uint16_t(3994));
    put(coin,0x40,uint16_t(3552));refuses(1);put(coin,0x40,uint16_t(3031));
    put(coin,0x20,NativePosition{65535,64,0,0});refuses(1);put(coin,0x20,NativePosition{65535,65,0,0});
    put(body,0x20,NativePosition{65535,65535,255,0});Require(resolve(1).handle.object==sourceBlock.object,"Rejeitou corpo aberto com posicao descritiva invalida");
    bool refusedEmpty=false;try{ContainerNow(GetCurrentProcess(),base,2);}catch(...){refusedEmpty=true;}Require(refusedEmpty,"Aceitou entrada vazia como origem/destino");
    std::cout<<"PASS: validacao nativa de origem, destino, moeda, quantidade, entrada vazia, posicao descritiva, merge e bag cheia.\n";
}
__declspec(noinline) void IdleFixture(){asm volatile("" ::: "memory");}
__declspec(noinline) void MoveFixture(uintptr_t object,const BorrowedItem* item,const NativePosition* destination,uint32_t count){
    fixtureSink=object+item->object+item->control+destination->x+destination->y+destination->z+count;asm volatile("" ::: "memory");
}
int MoveSelfTest(bool timeout){
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};Handle ready(CreateEventW(&sa,TRUE,FALSE,nullptr));if(!ready.h)throw Error("CreateEvent fixture");
    HANDLE input=nullptr,output=nullptr;if(!CreatePipe(&input,&output,&sa,0))throw Error("CreatePipe");
    Handle read(input),write(output);SetHandleInformation(read.h,HANDLE_FLAG_INHERIT,0);
    std::wstring cmd=L"\""+ExePath().wstring()+L"\" --move-fixture "+std::to_wstring(reinterpret_cast<uintptr_t>(ready.h));
    STARTUPINFOW si{};si.cb=sizeof(si);si.dwFlags=STARTF_USESTDHANDLES;si.hStdOutput=write.h;si.hStdError=write.h;si.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};if(!CreateProcessW(nullptr,cmd.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi))throw Error("CreateProcess fixture");
    Handle process(pi.hProcess),thread(pi.hThread);
    try{
        std::string line;char c=0;DWORD got=0;while(ReadFile(read.h,&c,1,&got,nullptr)&&got&&c!='\n')line+=c;
        std::istringstream values(line);std::string checkpoint,function,sink;values>>checkpoint>>function>>sink;
        CheckShadowStack(process.h);
        OneCall test(pi.dwProcessId,process.h,std::stoull(checkpoint,nullptr,0)+(timeout?1:0),0,pi.dwThreadId,std::stoull(function,nullptr,0),0x1234,0,timeout?2:5);
        test.requestedCount=3;test.move.handle={0x1111,0x2222};test.destination={65535,64,0,0};test.startFixture=ready.h;test.RunOnce();
        Sleep(100);DWORD code=0;bool ok=test.detached&&test.diagnostics.empty()&&GetExitCodeProcess(process.h,&code)&&code==STILL_ACTIVE;
        uint64_t value=ReadValue<uint64_t>(process.h,std::stoull(sink,nullptr,0));
        if(timeout)ok=ok&&!test.started&&!test.completed&&value==0;
        else ok=ok&&test.started&&test.completed&&test.contextRestored&&test.stackRestored&&value==0x1234+0x1111+0x2222+65535+64+3;
        auto report=std::filesystem::temp_directory_path()/(std::string("cromin_move_selftest_")+(timeout?"timeout_":"call_")+std::to_string(pi.dwProcessId)+".json");test.SaveResult(report);
        std::wcout<<L"[SELFTEST_JSON] "<<report.wstring()<<L"\n";
        std::cout<<(ok?"PASS":"FAIL")<<": "<<(timeout?"timeout sem chamada":"quatro argumentos, handle, posicao, quantidade, contexto, 96 bytes e detach")<<"\n";
        TerminateProcess(process.h,0);WaitForSingleObject(process.h,3000);return ok?0:1;
    }catch(...){TerminateProcess(process.h,1);WaitForSingleObject(process.h,3000);throw;}
}
int main(int argc,char** argv)try{
    if(argc==3&&std::string(argv[1])=="--move-fixture"){
        std::cout<<Hex(reinterpret_cast<uintptr_t>(&IdleFixture))<<" "<<Hex(reinterpret_cast<uintptr_t>(&MoveFixture))<<" "<<Hex(reinterpret_cast<uintptr_t>(&fixtureSink))<<"\n"<<std::flush;
        if(WaitForSingleObject(reinterpret_cast<HANDLE>(std::stoull(argv[2])),15000)!=WAIT_OBJECT_0)return 2;
        for(;;){IdleFixture();Sleep(10);}
    }
    if(argc==2&&std::string(argv[1])=="--self-test"){ValidationSelfTest();return MoveSelfTest(false);}
    if(argc==2&&std::string(argv[1])=="--self-test-timeout")return MoveSelfTest(true);
    std::cout<<"[VERSAO] Loot Once 2: entradas vazias de containers reconhecidas.\n";
    DWORD pid=0,tid=0;unsigned source=999,destination=999,slot=999,count=0;
    for(int i=1;i<argc;++i){std::string arg=argv[i];if(i+1>=argc)throw std::runtime_error("Falta valor do argumento");
        if(arg=="--pid")pid=std::stoul(argv[++i]);else if(arg=="--thread")tid=std::stoul(argv[++i]);else if(arg=="--source")source=std::stoul(argv[++i]);else if(arg=="--destination")destination=std::stoul(argv[++i]);else if(arg=="--slot")slot=std::stoul(argv[++i]);else if(arg=="--count")count=std::stoul(argv[++i]);else throw std::runtime_error("Argumento desconhecido");
    }
    Require(pid&&tid&&pid!=GetCurrentProcessId()&&source<=63&&destination<=63&&source!=destination&&slot<=511&&count>=1&&count<=100,"Use loot_once.py para preparar os parametros atuais");
    Handle process(OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ|PROCESS_VM_WRITE|PROCESS_VM_OPERATION,FALSE,pid));if(!process.h)throw Error("OpenProcess");
    BOOL debug=FALSE;if(!CheckRemoteDebuggerPresent(process.h,&debug))throw Error("CheckRemoteDebuggerPresent");Require(!debug,"Ja existe depurador no cliente");
    uintptr_t base=CheckBuild(process.h,pid);CheckShadowStack(process.h);Require(GameThreadMatches(process.h,pid,tid,base+0xE53E18),"Thread atual diferente da captura");
    std::array<uint8_t,16> expected{0xE8,0x39,0x85,0xFF,0xFF,0x48,0x8B,0x5E,0x08,0x48,0x8B,0xCB,0xE8,0x2D,0x9A,0x61},actual{};
    Require(Read(process.h,base+0x28E9B2,actual.data(),actual.size())&&actual==expected,"Checkpoint diferente");
    ResolveMove(process.h,base,source,slot,destination,count); // reconfere novamente no evento pausado em Begin
    SetConsoleCtrlHandler(ConsoleHandler,TRUE);
    OneCall call(pid,process.h,base+0x28E9B2,base,tid,base+0x141E50,base+0xE52A40,0,20);
    call.sourceId=source;call.sourceSlot=slot;call.destinationId=destination;call.requestedCount=count;
    auto report=ExePath().parent_path()/("loot_native_"+std::to_string(GetTickCount64())+".json");
    try{call.RunOnce();}catch(...){call.Cleanup();call.SaveResult(report);std::wcout<<L"[JSON] "<<report.wstring()<<L"\n";throw;}
    call.SaveResult(report);std::wcout<<L"[JSON] "<<report.wstring()<<L"\n";
    return call.completed&&call.detached&&call.contextRestored&&call.stackRestored&&call.diagnostics.empty()?0:2;
}catch(const std::exception& error){std::cerr<<"[ERRO] "<<error.what()<<"\n";return 1;}
