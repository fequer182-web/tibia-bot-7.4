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
struct GroundHandle {uintptr_t object=0,control=0;};
static_assert(sizeof(GroundHandle)==16);
struct CorpseInfo {GroundHandle handle{};NativePosition position{};uintptr_t tile=0;unsigned slot=0;};
void RequireCorpse(bool value,const char* message){if(!value)throw std::runtime_error(message);}
uintptr_t TileSlotRead(HANDLE process,uintptr_t base,const NativePosition& position){
    RequireCorpse(position.x>0&&position.x<65535&&position.y>0&&position.y<65535&&position.z<=15,"Posicao de tile invalida");
    auto tree=base+0xE52E30+position.z*16;
    auto header=ReadValue<uintptr_t>(process,tree);
    RequireCorpse(header&&ReadValue<uint8_t>(process,header+0x19)==1,"Arvore de tiles fora do formato esperado");
    auto node=ReadValue<uintptr_t>(process,header+8);uint32_t key=(position.y>>5)*2048+(position.x>>5);
    std::vector<uintptr_t> seen;
    while(node!=header){
        RequireCorpse(node&&seen.size()<64&&std::find(seen.begin(),seen.end(),node)==seen.end(),"Arvore de tiles ciclica/invalida");seen.push_back(node);
        RequireCorpse(ReadValue<uint8_t>(process,node+0x19)==0,"No de tiles inesperado");
        auto actual=ReadValue<uint32_t>(process,node+0x20);
        if(actual==key)return node+0x28+(((position.y&31)*32+(position.x&31))*16);
        node=ReadValue<uintptr_t>(process,node+(key<actual?0:0x10));
    }
    return 0;
}
std::vector<CorpseInfo> CorpsesAt(HANDLE process,uintptr_t base,const NativePosition& position){
    std::vector<CorpseInfo> result;auto slot=TileSlotRead(process,base,position);if(!slot)return result;
    auto tilePair=ReadValue<GroundHandle>(process,slot);if(!tilePair.object){RequireCorpse(!tilePair.control,"Tile nulo com controle nao nulo");return result;}
    auto tile=tilePair.object;RequireCorpse(ReadValue<uintptr_t>(process,tile)==base+0xAF6848,"VTable de Tile diferente");
    auto begin=ReadValue<uintptr_t>(process,tile+0x50),end=ReadValue<uintptr_t>(process,tile+0x58),capacity=ReadValue<uintptr_t>(process,tile+0x60);
    RequireCorpse(end>=begin&&capacity>=end&&(end-begin)%16==0&&(end-begin)<=256*16&&capacity-begin<=65536,"Vetor de Things invalido");
    for(unsigned i=0;i<(end-begin)/16;++i){
        auto item=ReadValue<GroundHandle>(process,begin+i*16);if(!item.object)continue;
        auto vt=ReadValue<uintptr_t>(process,item.object);if(vt!=base+0xAE9BC0)continue;
        if(ReadValue<uint16_t>(process,item.object+0x40)!=3994)continue;
        auto actual=ReadValue<NativePosition>(process,item.object+0x20);
        RequireCorpse(item.control&&actual.x==position.x&&actual.y==position.y&&actual.z==position.z,"Corpo nao corresponde ao tile");
        result.push_back({item,actual,tile,i});
    }
    auto after=ReadValue<GroundHandle>(process,slot);
    RequireCorpse(after.object==tilePair.object&&after.control==tilePair.control&&ReadValue<uintptr_t>(process,tile+0x50)==begin&&ReadValue<uintptr_t>(process,tile+0x58)==end&&ReadValue<uintptr_t>(process,tile+0x60)==capacity,"Tile mudou durante a leitura");
    return result;
}
CorpseInfo SelectCorpse(HANDLE process,uintptr_t base,const NativePosition& position,uintptr_t expected){
    auto player=ReadPosition(process,base);
    RequireCorpse(player.valid&&player.z==position.z&&abs(int(player.x)-int(position.x))<=1&&abs(int(player.y)-int(position.y))<=1,"Corpo precisa estar adjacente e no mesmo andar");
    auto candidates=CorpsesAt(process,base,position);
    if(expected){for(const auto& candidate:candidates)if(candidate.handle.object==expected)return candidate;throw std::runtime_error("Corpo selecionado nao esta mais nesse tile");}
    RequireCorpse(candidates.size()==1,"Tile sem corpo de rato unico; informe um objeto atual validado pelo leitor");return candidates.front();
}

struct OneCall:Trace {
    NativePosition destination{};uintptr_t executedThis=0,expectedCorpse=0;CorpseInfo corpse{};
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
        if(base)corpse=SelectCorpse(process,base,destination,expectedCorpse);
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
        memcpy(frame.data()+40,&corpse.handle,sizeof(corpse.handle));
        GroundHandle empty{};memcpy(frame.data()+56,&empty,sizeof(empty));
        if(!Write(process,temporaryRsp,frame.data(),frame.size()))throw Error("Preparar retorno e destino na pilha");
        writableStackPrepared=true;
        CONTEXT call=context;
        executedThis=liveThis;
        call.Rip=targetFunction;call.Rsp=temporaryRsp;call.Rcx=liveThis;call.Rdx=temporaryRsp+40;call.R8=temporaryRsp+56;call.R9=0;
        call.EFlags&=~0x100; // sem single-step por instrucao
        call.EFlags|=0x10000;
        call.Dr6=0;
        if(!SetThreadContext(thread,&call)){Restore(thread,false);throw Error("SetThreadContext chamada");}
        started=true;
        std::cout<<"[CHAMADA] Game::open; thread="<<targetThread<<" destino="<<destination.x<<","<<destination.y<<","<<destination.z<<". Corpo 3994 no tile; janela nova solicitada.\n"<<std::flush;
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
        std::ofstream f(output);if(!f)throw std::runtime_error("Nao conseguiu salvar o relatorio");
        f<<"{\n  \"mode\": \"native_corpse_open_one_call_on_game_thread\",\n  \"pid\": "<<pid<<",\n  \"threadId\": "<<targetThread
         <<",\n  \"function\": \""<<Hex(targetFunction)<<"\",\n  \"game_instance\": \""<<Hex(instance)<<"\",\n  \"checkpoint\": \""<<Hex(address)
         <<"\",\n  \"executed_this\": \""<<Hex(executedThis)<<"\",\n  \"corpse_position\": {\"x\": "<<destination.x<<", \"y\": "<<destination.y<<", \"z\": "<<destination.z<<"},\n  \"corpse_item_id\": 3994,\n  \"corpse_object\": \""<<Hex(corpse.handle.object)<<"\",\n  \"corpse_control\": \""<<Hex(corpse.handle.control)<<"\",\n  \"tile\": \""<<Hex(corpse.tile)<<"\",\n  \"tile_slot\": "<<corpse.slot<<",\n  \"requested_new_container\": true,\n  \"stack_bytes_preserved\": 96,\n  \"started\": "<<(started?"true":"false")
         <<",\n  \"returned\": "<<(completed?"true":"false")<<",\n  \"context_restored\": "<<(contextRestored?"true":"false")
         <<",\n  \"stack_slot_restored\": "<<(stackRestored?"true":"false")<<",\n  \"detached\": "<<(detached?"true":"false")
         <<",\n  \"raw_rax_at_return\": \""<<Hex(returnedRax)<<"\",\n  \"return_type_interpreted\": true,\n  \"return_type\": \"int32_container_id_or_minus_one\",\n  \"position_before\": ";
        WritePosition(f,positionBefore);f<<",\n  \"positions_after\": [";
        for(size_t i=0;i<positionAfter.size();++i){if(i)f<<", ";WritePosition(f,positionAfter[i]);}
        f<<"],\n  \"diagnostics\": [";for(size_t i=0;i<diagnostics.size();++i){if(i)f<<", ";f<<"\""<<diagnostics[i]<<"\"";}f<<"]\n}\n";
    }
};

void CorpseSelectionSelfTest(){
    struct Allocation {void* p=VirtualAlloc(nullptr,0xEDE000,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);~Allocation(){if(p)VirtualFree(p,0,MEM_RELEASE);}} allocation;
    RequireCorpse(allocation.p!=nullptr,"VirtualAlloc fixture");auto base=reinterpret_cast<uintptr_t>(allocation.p);
    std::array<uint8_t,0x38> head{};std::array<uint8_t,0x4028> node{};
    std::array<uint8_t,0x70> tile{},player{};std::array<uint8_t,0x50> item{},second{};
    std::array<GroundHandle,2> things{};
    auto ptr=[](auto& a){return reinterpret_cast<uintptr_t>(a.data());};
    auto put=[](auto& a,size_t off,auto value){memcpy(a.data()+off,&value,sizeof(value));};
    auto h=ptr(head),n=ptr(node),t=ptr(tile),p=ptr(player),i=ptr(item),v=ptr(things);
    NativePosition pos{100,101,7,0};uint32_t key=(pos.y>>5)*2048+(pos.x>>5);
    put(head,8,n);put(head,0x19,uint8_t(1));put(node,0,h);put(node,8,h);put(node,16,h);put(node,0x20,key);
    size_t index=0x28+(((pos.y&31)*32+(pos.x&31))*16);put(node,index,GroundHandle{t,0x5555});
    memcpy(reinterpret_cast<void*>(base+0xE52E30+pos.z*16),&h,8);
    put(tile,0,base+0xAF6848);put(tile,0x50,v);put(tile,0x58,v+16);put(tile,0x60,v+32);
    put(item,0,base+0xAE9BC0);put(item,0x20,pos);put(item,0x40,uint16_t(3994));things[0]={i,0x2222};
    put(player,0,base+0xAE99E0);put(player,0x30,NativePosition{100,100,7,0});memcpy(reinterpret_cast<void*>(base+0xE52A48),&p,8);
    auto process=GetCurrentProcess();auto found=SelectCorpse(process,base,pos,0);
    RequireCorpse(found.handle.object==i&&found.slot==0,"Corpo incorreto");
    RequireCorpse(CorpsesAt(process,base,NativePosition{101,101,7,0}).empty(),"Tile vazio incorreto");
    auto refuse=[&](){bool rejected=false;try{SelectCorpse(process,base,pos,0);}catch(...){rejected=true;}RequireCorpse(rejected,"Aceitou corpo invalido/ambiguo");};
    put(item,0x20,NativePosition{99,101,7,0});refuse();put(item,0x20,pos);
    put(player,0x30,NativePosition{100,99,7,0});refuse();put(player,0x30,NativePosition{100,100,7,0});
    put(tile,0,uintptr_t(0));refuse();put(tile,0,base+0xAF6848);
    second=item;things[1]={ptr(second),0x3333};put(tile,0x58,v+32);refuse();
    RequireCorpse(SelectCorpse(process,base,pos,i).handle.object==i,"Objeto selecionado incorreto");
    put(tile,0x58,v+15);refuse();
    std::cout<<"PASS: mapa de tiles, corpo no chao, distancia, posicao, VTable, ambiguidade e vetor.\n";
}
__declspec(noinline) void IdleFixture(){asm volatile("" ::: "memory");}
__declspec(noinline) int OpenFixture(uintptr_t object,const GroundHandle* item,const GroundHandle* container){
    if(object==0x1234&&item->object==0x1111&&item->control==0x2222&&!container->object&&!container->control)fixtureSink=0x7777;
    asm volatile("" ::: "memory");return 7;
}
int OpenSelfTest(bool timeout){
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};Handle ready(CreateEventW(&sa,TRUE,FALSE,nullptr));if(!ready.h)throw Error("CreateEvent fixture");
    HANDLE input=nullptr,output=nullptr;if(!CreatePipe(&input,&output,&sa,0))throw Error("CreatePipe");
    Handle read(input),write(output);SetHandleInformation(read.h,HANDLE_FLAG_INHERIT,0);
    std::wstring cmd=L"\""+ExePath().wstring()+L"\" --open-fixture "+std::to_wstring(reinterpret_cast<uintptr_t>(ready.h));
    STARTUPINFOW si{};si.cb=sizeof(si);si.dwFlags=STARTF_USESTDHANDLES;si.hStdOutput=write.h;si.hStdError=write.h;si.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};if(!CreateProcessW(nullptr,cmd.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi))throw Error("CreateProcess fixture");
    Handle process(pi.hProcess),thread(pi.hThread);
    try{
        std::string line;char c=0;DWORD got=0;while(ReadFile(read.h,&c,1,&got,nullptr)&&got&&c!='\n')line+=c;
        std::istringstream values(line);std::string checkpoint,function,sink;values>>checkpoint>>function>>sink;CheckShadowStack(process.h);
        OneCall test(pi.dwProcessId,process.h,std::stoull(checkpoint,nullptr,0)+(timeout?1:0),0,pi.dwThreadId,std::stoull(function,nullptr,0),0x1234,0,timeout?2:5);
        test.corpse.handle={0x1111,0x2222};test.destination={100,101,7,0};test.startFixture=ready.h;test.RunOnce();
        Sleep(100);DWORD code=0;bool ok=test.detached&&test.diagnostics.empty()&&GetExitCodeProcess(process.h,&code)&&code==STILL_ACTIVE;
        auto value=ReadValue<uint64_t>(process.h,std::stoull(sink,nullptr,0));
        if(timeout)ok=ok&&!test.started&&!test.completed&&value==0;
        else ok=ok&&test.completed&&test.contextRestored&&test.stackRestored&&value==0x7777&&int32_t(test.returnedRax)==7;
        auto report=std::filesystem::temp_directory_path()/("cromin_corpse_open_selftest_"+std::to_string(pi.dwProcessId)+".json");test.SaveResult(report);std::wcout<<L"[SELFTEST_JSON] "<<report.wstring()<<L"\n";
        std::cout<<(ok?"PASS":"FAIL")<<": "<<(timeout?"timeout sem chamada":"Game*, ItemPtr emprestado, ContainerPtr vazio, retorno 7, contexto/pilha/detach")<<"\n";
        TerminateProcess(process.h,0);WaitForSingleObject(process.h,3000);return ok?0:1;
    }catch(...){TerminateProcess(process.h,1);WaitForSingleObject(process.h,3000);throw;}
}
int main(int argc,char** argv)try{
    if(argc==3&&std::string(argv[1])=="--open-fixture"){
        std::cout<<Hex(reinterpret_cast<uintptr_t>(&IdleFixture))<<" "<<Hex(reinterpret_cast<uintptr_t>(&OpenFixture))<<" "<<Hex(reinterpret_cast<uintptr_t>(&fixtureSink))<<"\n"<<std::flush;
        if(WaitForSingleObject(reinterpret_cast<HANDLE>(std::stoull(argv[2])),15000)!=WAIT_OBJECT_0)return 2;
        for(;;){IdleFixture();Sleep(10);}
    }
    if(argc==2&&std::string(argv[1])=="--self-test"){CorpseSelectionSelfTest();return OpenSelfTest(false);}
    if(argc==2&&std::string(argv[1])=="--self-test-timeout")return OpenSelfTest(true);
    DWORD pid=0,tid=0;int x=-1,y=-1,z=-1;uintptr_t expectedObject=0;
    for(int i=1;i<argc;++i){std::string arg=argv[i];if(i+1>=argc)throw std::runtime_error("Falta valor do argumento");
        if(arg=="--pid")pid=std::stoul(argv[++i]);else if(arg=="--thread")tid=std::stoul(argv[++i]);else if(arg=="--x")x=std::stoi(argv[++i]);else if(arg=="--y")y=std::stoi(argv[++i]);else if(arg=="--z")z=std::stoi(argv[++i]);else if(arg=="--object")expectedObject=std::stoull(argv[++i],nullptr,0);else throw std::runtime_error("Argumento desconhecido");
    }
    RequireCorpse(pid&&tid&&pid!=GetCurrentProcessId()&&x>0&&x<65535&&y>0&&y<65535&&z>=0&&z<=15,"Use open_and_loot.py para selecionar um corpo atual");
    Handle process(OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ|PROCESS_VM_WRITE|PROCESS_VM_OPERATION,FALSE,pid));if(!process.h)throw Error("OpenProcess");
    BOOL debug=FALSE;if(!CheckRemoteDebuggerPresent(process.h,&debug))throw Error("CheckRemoteDebuggerPresent");RequireCorpse(!debug,"Ja existe depurador no cliente");
    auto base=CheckBuild(process.h,pid);CheckShadowStack(process.h);RequireCorpse(GameThreadMatches(process.h,pid,tid,base+0xE53E18),"Thread diferente da captura");
    std::array<uint8_t,16> expected{0xE8,0x39,0x85,0xFF,0xFF,0x48,0x8B,0x5E,0x08,0x48,0x8B,0xCB,0xE8,0x2D,0x9A,0x61},actual{};
    RequireCorpse(Read(process.h,base+0x28E9B2,actual.data(),actual.size())&&actual==expected,"Checkpoint diferente");
    NativePosition position{uint32_t(x),uint32_t(y),uint16_t(z),0};auto corpse=SelectCorpse(process.h,base,position,expectedObject);SetConsoleCtrlHandler(ConsoleHandler,TRUE);
    OneCall call(pid,process.h,base+0x28E9B2,base,tid,base+0x142EE0,base+0xE52A40,0,20);call.destination=position;call.expectedCorpse=corpse.handle.object;
    auto report=ExePath().parent_path()/("corpse_open_native_"+std::to_string(GetTickCount64())+".json");
    try{call.RunOnce();}catch(...){call.Cleanup();call.SaveResult(report);std::wcout<<L"[JSON] "<<report.wstring()<<L"\n";throw;}
    call.SaveResult(report);std::wcout<<L"[JSON] "<<report.wstring()<<L"\n";
    bool ok=call.completed&&call.detached&&call.contextRestored&&call.stackRestored&&call.diagnostics.empty()&&int32_t(call.returnedRax)>=0&&int32_t(call.returnedRax)<=63;
    std::cout<<"[RETORNO_OPEN] "<<int32_t(call.returnedRax)<<". Confirme o novo container por leitura.\n";return ok?0:2;
}catch(const std::exception& error){std::cerr<<"[ERRO] "<<error.what()<<"\n";return 1;}
