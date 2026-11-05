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
struct OwnedCreature {uintptr_t object=0,control=0;};
struct RatInfo {uint32_t id=0;uintptr_t object=0;NativePosition position{};uint8_t hp=0;};
void RequireRat(bool value,const char* message){if(!value)throw std::runtime_error(message);}
std::vector<std::pair<uint32_t,OwnedCreature>> CreatureEntries(HANDLE process,uintptr_t base){
    uintptr_t map=base+0xE52E30+0x100;
    auto header=ReadValue<uintptr_t>(process,map);auto size=ReadValue<uint64_t>(process,map+8);
    RequireRat(header&&size<=4096&&ReadValue<uint8_t>(process,header+0x19)==1,"Mapa de criaturas fora do formato esperado");
    std::vector<std::pair<uintptr_t,uintptr_t>> pending{{ReadValue<uintptr_t>(process,header+8),header}};
    std::vector<uintptr_t> seen;std::vector<std::pair<uint32_t,OwnedCreature>> result;
    while(!pending.empty()){
        auto entry=pending.back();pending.pop_back();auto node=entry.first;if(node==header)continue;
        RequireRat(node&&seen.size()<size&&std::find(seen.begin(),seen.end(),node)==seen.end(),"Mapa de criaturas ciclico ou alterado");seen.push_back(node);
        RequireRat(ReadValue<uint8_t>(process,node+0x19)==0&&ReadValue<uintptr_t>(process,node+8)==entry.second,"No de criatura alterado");
        auto id=ReadValue<uint32_t>(process,node+0x20);auto pair=ReadValue<OwnedCreature>(process,node+0x28);
        RequireRat(pair.object||!pair.control,"Criatura nula com controle nao nulo");
        if(pair.object)result.push_back({id,pair});
        pending.push_back({ReadValue<uintptr_t>(process,node),node});pending.push_back({ReadValue<uintptr_t>(process,node+0x10),node});
    }
    RequireRat(seen.size()==size&&ReadValue<uintptr_t>(process,map)==header&&ReadValue<uint64_t>(process,map+8)==size,"Mapa de criaturas mudou durante leitura");
    return result;
}
RatInfo ReadRat(HANDLE process,uintptr_t base,uint32_t id,uintptr_t object){
    std::array<uint8_t,0x70> raw{};RequireRat(Read(process,object,raw.data(),raw.size()),"Nao conseguiu ler o rato");
    uintptr_t vt=0;uint32_t actual=0;uint64_t length=0,capacity=0;
    memcpy(&vt,raw.data(),8);memcpy(&actual,raw.data()+0x40,4);memcpy(&length,raw.data()+0x58,8);memcpy(&capacity,raw.data()+0x60,8);
    RequireRat(vt==base+0xAF61D8&&actual==id&&length==3&&capacity==15&&memcmp(raw.data()+0x48,"Rat\0",4)==0,"Objeto nao corresponde ao Rat observado nesta build");
    RatInfo result{id,object,{},{}};memcpy(&result.position,raw.data()+0x20,sizeof(NativePosition));result.hp=raw[0x69];
    RequireRat(result.position.x>0&&result.position.x<65535&&result.position.y>0&&result.position.y<65535&&result.position.z<=15&&result.hp>0&&result.hp<=100,"Rato morto ou posicao/vida invalida");
    return result;
}
RatInfo CurrentRat(HANDLE process,uintptr_t base,uint32_t id){
    for(const auto& entry:CreatureEntries(process,base))if(entry.first==id)return ReadRat(process,base,id,entry.second.object);
    throw std::runtime_error("Rato nao esta mais no mapa de criaturas");
}
void CheckRatNear(HANDLE process,uintptr_t base,const RatInfo& rat){
    auto player=ReadPosition(process,base);
    RequireRat(player.valid&&player.z==rat.position.z,"Rato precisa ter posicao valida no mesmo andar do jogador");
    RequireRat(!ReadValue<uintptr_t>(process,base+0xE52A50)&&!ReadValue<uintptr_t>(process,base+0xE52A60),"Ja existe ataque/follow ativo. Nao alterna nem troca o alvo");
}
RatInfo ChooseRat(HANDLE process,uintptr_t base){
    std::vector<RatInfo> candidates;
    for(const auto& entry:CreatureEntries(process,base)){
        try{auto rat=ReadRat(process,base,entry.first,entry.second.object);CheckRatNear(process,base,rat);candidates.push_back(rat);}catch(const std::exception&){}
    }
    RequireRat(!candidates.empty(),"Nenhum Rat vivo no mesmo andar sem ataque/follow ativo");
    auto player=ReadPosition(process,base);
    auto distance=[&](const RatInfo& rat){return std::max(abs(int(player.x)-int(rat.position.x)),abs(int(player.y)-int(rat.position.y)));};
    std::sort(candidates.begin(),candidates.end(),[&](auto a,auto b){auto da=distance(a),db=distance(b);return da==db?a.id<b.id:da<db;});return candidates.front();
}

struct OneCall:Trace {
    RatInfo target{};uintptr_t executedThis=0;
    uintptr_t lookupFunction=0,releaseFunction=0;
    enum class Stage {Lookup,Attack,Release};Stage stage=Stage::Lookup;
    bool attackRequested=false,attackReturned=false,ownedReferenceHandled=false;
    OwnedCreature acquired{};
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
        if(base){target=CurrentRat(process,base,target.id);CheckRatNear(process,base,target);}
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
        OwnedCreature empty{};memcpy(frame.data()+64,&empty,sizeof(empty));
        if(!Write(process,temporaryRsp,frame.data(),frame.size()))throw Error("Preparar retorno e destino na pilha");
        writableStackPrepared=true;
        CONTEXT call=context;
        executedThis=liveThis;
        call.Rip=lookupFunction;call.Rsp=temporaryRsp;call.Rcx=base?base+0xE52E30:0x4321;call.Rdx=temporaryRsp+64;call.R8=target.id;call.R9=0;
        call.EFlags&=~0x100; // sem single-step por instrucao
        call.EFlags|=0x10000;
        call.Dr6=0;
        if(!SetThreadContext(thread,&call)){Restore(thread,false);throw Error("SetThreadContext chamada");}
        started=true;
        std::cout<<"[REFERENCIA] Map::getCreatureById; id="<<target.id<<". Obtendo copia propria antes do ataque.\n"<<std::flush;
    }
    bool Advance(HANDLE thread){
        acquired=ReadValue<OwnedCreature>(process,temporaryRsp+64);
        if(!acquired.control){diagnostics.push_back("Lookup sem referencia propria; nenhum ataque executado");return false;}
        bool valid=acquired.object==target.object;
        if(base&&valid){try{auto rat=ReadRat(process,base,target.id,acquired.object);CheckRatNear(process,base,rat);}catch(const std::exception& error){diagnostics.push_back(error.what());valid=false;}}
        if(!valid&&diagnostics.empty())diagnostics.push_back("Lookup retornou objeto diferente; libera copia sem atacar");
        stage=valid?Stage::Attack:Stage::Release;
        CONTEXT call=saved;call.ContextFlags=CONTEXT_ALL;call.Rip=valid?targetFunction:releaseFunction;call.Rsp=temporaryRsp;
        call.Rcx=valid?instance:acquired.control;call.Rdx=valid?temporaryRsp+64:0;call.R8=0;call.R9=0;
        call.Dr6=0;call.EFlags&=~0x100;call.EFlags|=0x10000;
        if(!SetThreadContext(thread,&call))throw Error("SetThreadContext ataque/liberacao");
        attackRequested=valid;
        std::cout<<(valid?"[ATAQUE] Game::attack uma vez, com copia propria.\n":"[LIBERAR] Sem ataque; devolvendo a referencia obtida.\n")<<std::flush;
        return true;
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
                                if(stage==Stage::Lookup&&Advance(it->second.handle))break;
                                if(stage!=Stage::Lookup){ownedReferenceHandled=true;attackReturned=stage==Stage::Attack;}
                                if(!Restore(it->second.handle,true))throw std::runtime_error("Nao foi possivel restaurar o contexto apos o retorno");
                                std::cout<<"[RETORNO] Sequencia retornou; contexto e pilha restaurados.\n"<<std::flush;
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
        if(completed && detached && base)positionAfter.push_back(ReadPosition(process,base));
    }
    void SaveResult(const std::filesystem::path& output){
        std::ofstream f(output);if(!f)throw std::runtime_error("Nao conseguiu salvar o relatorio");
        f<<"{\n  \"mode\": \"native_rat_attack_owned_reference_on_game_thread\",\n  \"pid\": "<<pid<<",\n  \"threadId\": "<<targetThread
         <<",\n  \"function\": \""<<Hex(targetFunction)<<"\",\n  \"game_instance\": \""<<Hex(instance)<<"\",\n  \"checkpoint\": \""<<Hex(address)
         <<"\",\n  \"executed_this\": \""<<Hex(executedThis)<<"\",\n  \"rat_id\": "<<target.id<<",\n  \"rat_position_at_request\": {\"x\": "<<target.position.x<<", \"y\": "<<target.position.y<<", \"z\": "<<target.position.z<<"},\n  \"rat_object\": \""<<Hex(target.object)<<"\",\n  \"attack_requested\": "<<(attackRequested?"true":"false")<<",\n  \"attack_returned\": "<<(attackReturned?"true":"false")<<",\n  \"owned_reference_handled\": "<<(ownedReferenceHandled?"true":"false")<<",\n  \"stack_bytes_preserved\": 96,\n  \"started\": "<<(started?"true":"false")
         <<",\n  \"returned\": "<<(completed?"true":"false")<<",\n  \"context_restored\": "<<(contextRestored?"true":"false")
         <<",\n  \"stack_slot_restored\": "<<(stackRestored?"true":"false")<<",\n  \"detached\": "<<(detached?"true":"false")
         <<",\n  \"raw_rax_at_return\": \""<<Hex(returnedRax)<<"\",\n  \"return_type_interpreted\": false,\n  \"position_before\": ";
        WritePosition(f,positionBefore);f<<",\n  \"positions_after\": [";
        for(size_t i=0;i<positionAfter.size();++i){if(i)f<<", ";WritePosition(f,positionAfter[i]);}
        f<<"],\n  \"diagnostics\": [";for(size_t i=0;i<diagnostics.size();++i){if(i)f<<", ";f<<"\""<<diagnostics[i]<<"\"";}f<<"]\n}\n";
    }
};

void RatSelectionSelfTest(){
    struct Allocation {void* p=VirtualAlloc(nullptr,0xEDE000,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);~Allocation(){if(p)VirtualFree(p,0,MEM_RELEASE);}} allocation;
    RequireRat(allocation.p!=nullptr,"VirtualAlloc fixture");auto base=reinterpret_cast<uintptr_t>(allocation.p);
    std::array<uint8_t,0x38> head{},node{};std::array<uint8_t,0x80> rat{},player{};
    auto ptr=[](auto& a){return reinterpret_cast<uintptr_t>(a.data());};
    auto put=[](auto& a,size_t off,auto value){memcpy(a.data()+off,&value,sizeof(value));};
    auto h=ptr(head),n=ptr(node),r=ptr(rat),p=ptr(player);uint32_t id=0x4000793E;
    put(head,0,n);put(head,8,n);put(head,16,n);put(head,0x19,uint8_t(1));
    put(node,0,h);put(node,8,h);put(node,16,h);put(node,0x20,id);put(node,0x28,r);put(node,0x30,uintptr_t(0x3333));
    memcpy(reinterpret_cast<void*>(base+0xE52F30),&h,8);uint64_t count=1;memcpy(reinterpret_cast<void*>(base+0xE52F38),&count,8);
    put(rat,0,base+0xAF61D8);put(rat,0x20,NativePosition{101,100,7,0});put(rat,0x40,id);memcpy(rat.data()+0x48,"Rat",4);put(rat,0x58,uint64_t(3));put(rat,0x60,uint64_t(15));put(rat,0x69,uint8_t(100));
    put(player,0,base+0xAE99E0);put(player,0x30,uint32_t(100));put(player,0x34,uint32_t(100));put(player,0x38,uint8_t(7));memcpy(reinterpret_cast<void*>(base+0xE52A48),&p,8);
    auto selected=ChooseRat(GetCurrentProcess(),base);RequireRat(selected.id==id&&selected.object==r&&selected.hp==100,"Selecao do Rat incorreta");
    auto refuse=[&](){bool rejected=false;try{ChooseRat(GetCurrentProcess(),base);}catch(...){rejected=true;}RequireRat(rejected,"Escolheu alvo fora do escopo");};
    put(rat,0x20,NativePosition{107,100,7,0});RequireRat(ChooseRat(GetCurrentProcess(),base).id==id,"Rejeitou Rat distante no mesmo andar");
    put(rat,0x20,NativePosition{101,100,8,0});refuse();put(rat,0x20,NativePosition{101,100,7,0});
    put(rat,0x69,uint8_t(0));refuse();put(rat,0x69,uint8_t(100));
    memcpy(rat.data()+0x48,"Bat",4);refuse();memcpy(rat.data()+0x48,"Rat",4);
    uintptr_t active=0x1111;memcpy(reinterpret_cast<void*>(base+0xE52A50),&active,8);refuse();
    std::cout<<"PASS: selecao fresca do Rat no mesmo andar; aceita distancia e recusa outro andar, vida, nome e alvo ja ativo.\n";
}
__declspec(noinline) void IdleFixture(){asm volatile("" ::: "memory");}
volatile uint64_t fixtureRefs=1;bool fixtureReject=false;
__declspec(noinline) OwnedCreature* LookupFixture(uintptr_t object,OwnedCreature* out,uint32_t id){
    if(object!=0x4321||id!=42){*out={};return out;}
    fixtureRefs=fixtureRefs+1;*out={fixtureReject?uintptr_t(0x3333):uintptr_t(0x1111),0x2222};return out;
}
__declspec(noinline) void AttackFixture(uintptr_t object,OwnedCreature* input){
    if(object==0x1234&&input->object==0x1111&&input->control==0x2222&&fixtureRefs==2)fixtureSink=0x7777;
    fixtureRefs=fixtureRefs-1;*input={};asm volatile("" ::: "memory");
}
__declspec(noinline) void ReleaseFixture(uintptr_t control){if(control==0x2222)fixtureRefs=fixtureRefs-1;asm volatile("" ::: "memory");}
int AttackSelfTest(bool timeout,bool reject){
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};Handle ready(CreateEventW(&sa,TRUE,FALSE,nullptr));if(!ready.h)throw Error("CreateEvent fixture");
    HANDLE input=nullptr,output=nullptr;if(!CreatePipe(&input,&output,&sa,0))throw Error("CreatePipe");
    Handle read(input),write(output);SetHandleInformation(read.h,HANDLE_FLAG_INHERIT,0);
    std::wstring cmd=L"\""+ExePath().wstring()+L"\" --attack-fixture "+std::to_wstring(reinterpret_cast<uintptr_t>(ready.h))+(reject?L" reject":L" accept");
    STARTUPINFOW si{};si.cb=sizeof(si);si.dwFlags=STARTF_USESTDHANDLES;si.hStdOutput=write.h;si.hStdError=write.h;si.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};if(!CreateProcessW(nullptr,cmd.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi))throw Error("CreateProcess fixture");
    Handle process(pi.hProcess),thread(pi.hThread);
    try{
        std::string line;char c=0;DWORD got=0;while(ReadFile(read.h,&c,1,&got,nullptr)&&got&&c!='\n')line+=c;
        std::istringstream values(line);std::string checkpoint,lookup,attack,release,sink,refs;values>>checkpoint>>lookup>>attack>>release>>sink>>refs;
        CheckShadowStack(process.h);
        OneCall test(pi.dwProcessId,process.h,std::stoull(checkpoint,nullptr,0)+(timeout?1:0),0,pi.dwThreadId,std::stoull(attack,nullptr,0),0x1234,0,timeout?2:5);
        test.target.id=42;test.target.object=0x1111;test.lookupFunction=std::stoull(lookup,nullptr,0);test.releaseFunction=std::stoull(release,nullptr,0);test.startFixture=ready.h;test.RunOnce();
        Sleep(100);DWORD code=0;bool ok=test.detached&&GetExitCodeProcess(process.h,&code)&&code==STILL_ACTIVE&&ReadValue<uint64_t>(process.h,std::stoull(refs,nullptr,0))==1;
        auto value=ReadValue<uint64_t>(process.h,std::stoull(sink,nullptr,0));
        if(timeout)ok=ok&&!test.started&&!test.completed&&value==0&&test.diagnostics.empty();
        else if(reject)ok=ok&&test.completed&&test.contextRestored&&test.stackRestored&&!test.attackRequested&&!test.attackReturned&&test.ownedReferenceHandled&&value==0&&!test.diagnostics.empty();
        else ok=ok&&test.completed&&test.contextRestored&&test.stackRestored&&test.attackRequested&&test.attackReturned&&test.ownedReferenceHandled&&value==0x7777&&test.diagnostics.empty();
        auto report=std::filesystem::temp_directory_path()/("cromin_rat_attack_selftest_"+std::to_string(pi.dwProcessId)+".json");test.SaveResult(report);std::wcout<<L"[SELFTEST_JSON] "<<report.wstring()<<L"\n";
        std::cout<<(ok?"PASS":"FAIL")<<": "<<(timeout?"timeout sem chamada":reject?"referencia obtida e liberada sem ataque":"lookup, ataque com consumo da copia, refcount preservado, contexto/pilha/detach")<<"\n";
        TerminateProcess(process.h,0);WaitForSingleObject(process.h,3000);return ok?0:1;
    }catch(...){TerminateProcess(process.h,1);WaitForSingleObject(process.h,3000);throw;}
}
int main(int argc,char** argv)try{
    if(argc==4&&std::string(argv[1])=="--attack-fixture"){
        fixtureReject=std::string(argv[3])=="reject";
        std::cout<<Hex(reinterpret_cast<uintptr_t>(&IdleFixture))<<" "<<Hex(reinterpret_cast<uintptr_t>(&LookupFixture))<<" "<<Hex(reinterpret_cast<uintptr_t>(&AttackFixture))<<" "<<Hex(reinterpret_cast<uintptr_t>(&ReleaseFixture))<<" "<<Hex(reinterpret_cast<uintptr_t>(&fixtureSink))<<" "<<Hex(reinterpret_cast<uintptr_t>(&fixtureRefs))<<"\n"<<std::flush;
        if(WaitForSingleObject(reinterpret_cast<HANDLE>(std::stoull(argv[2])),15000)!=WAIT_OBJECT_0)return 2;
        for(;;){IdleFixture();Sleep(10);}
    }
    if(argc==2&&std::string(argv[1])=="--self-test"){RatSelectionSelfTest();return AttackSelfTest(false,false);}
    if(argc==2&&std::string(argv[1])=="--self-test-timeout")return AttackSelfTest(true,false);
    if(argc==2&&std::string(argv[1])=="--self-test-reject")return AttackSelfTest(false,true);
    DWORD pid=0,tid=0;uint32_t requestedId=0;
    for(int i=1;i<argc;++i){std::string arg=argv[i];if(i+1>=argc)throw std::runtime_error("Falta valor do argumento");
        if(arg=="--pid")pid=std::stoul(argv[++i]);else if(arg=="--thread")tid=std::stoul(argv[++i]);else if(arg=="--id")requestedId=std::stoul(argv[++i],nullptr,0);else throw std::runtime_error("Argumento desconhecido");
    }
    RequireRat(pid&&tid&&pid!=GetCurrentProcessId(),"Uso: OTC_Rat_Attack_Once.exe --pid PID --thread THREAD");
    Handle process(OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ|PROCESS_VM_WRITE|PROCESS_VM_OPERATION,FALSE,pid));if(!process.h)throw Error("OpenProcess");
    BOOL debug=FALSE;if(!CheckRemoteDebuggerPresent(process.h,&debug))throw Error("CheckRemoteDebuggerPresent");RequireRat(!debug,"Ja existe depurador no cliente");
    auto base=CheckBuild(process.h,pid);CheckShadowStack(process.h);RequireRat(GameThreadMatches(process.h,pid,tid,base+0xE53E18),"Thread diferente da captura");
    std::array<uint8_t,16> expected{0xE8,0x39,0x85,0xFF,0xFF,0x48,0x8B,0x5E,0x08,0x48,0x8B,0xCB,0xE8,0x2D,0x9A,0x61},actual{};
    RequireRat(Read(process.h,base+0x28E9B2,actual.data(),actual.size())&&actual==expected,"Checkpoint diferente");
    auto rat=requestedId?CurrentRat(process.h,base,requestedId):ChooseRat(process.h,base);
    CheckRatNear(process.h,base,rat);
    std::cout<<"[RATO] id="<<Hex(rat.id)<<" posicao="<<rat.position.x<<","<<rat.position.y<<","<<rat.position.z<<" hp="<<unsigned(rat.hp)<<"\n";
    SetConsoleCtrlHandler(ConsoleHandler,TRUE);
    OneCall call(pid,process.h,base+0x28E9B2,base,tid,base+0x1434A0,base+0xE52A40,0,20);
    call.target=rat;call.lookupFunction=base+0x20F0D0;call.releaseFunction=base+0x110620;
    auto report=ExePath().parent_path()/("rat_attack_once_"+std::to_string(GetTickCount64())+".json");
    try{call.RunOnce();}catch(...){call.Cleanup();call.SaveResult(report);std::wcout<<L"[JSON] "<<report.wstring()<<L"\n";throw;}
    call.SaveResult(report);std::wcout<<L"[JSON] "<<report.wstring()<<L"\n";
    bool ok=call.attackReturned&&call.completed&&call.detached&&call.contextRestored&&call.stackRestored&&call.ownedReferenceHandled&&call.diagnostics.empty();
    std::cout<<(ok?"[RETORNOU] Um ataque solicitado. Confira no jogo; retorno nao confirma morte. Nao abre corpo nem coleta nesta etapa.\n":"[NAO CONFIRMADO] Consulte o JSON; nao repetiu ataque.\n");
    return ok?0:2;
}catch(const std::exception& error){std::cerr<<"[ERRO] "<<error.what()<<"\n";return 1;}
