#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <wincrypt.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

static_assert(sizeof(void*)==4,"Compile este observador como x86");
volatile LONG stopped=0;
BOOL WINAPI stopHandler(DWORD){InterlockedExchange(&stopped,1);return TRUE;}
std::runtime_error fail(const char* name){return std::runtime_error(std::string(name)+" WinError "+std::to_string(GetLastError()));}
std::string hx(uint32_t n){std::ostringstream s;s<<"0x"<<std::hex<<std::uppercase<<n;return s.str();}
std::string quote(const std::string& text){std::ostringstream s;s<<'"';for(unsigned char c:text){
    if(c=='"'||c=='\\')s<<'\\'<<char(c);else if(c<32)s<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<unsigned(c);else s<<char(c);
}s<<'"';return s.str();}
struct Handle{HANDLE h=nullptr;explicit Handle(HANDLE p=nullptr):h(p){}~Handle(){if(h&&h!=INVALID_HANDLE_VALUE)CloseHandle(h);}Handle(const Handle&)=delete;};
bool read(HANDLE p,uint32_t a,void* b,size_t n){SIZE_T got=0;return a>=0x10000&&uint64_t(a)+n<=0x100000000ULL&&ReadProcessMemory(p,reinterpret_cast<void*>(a),b,n,&got)&&got==n;}
template<class T>bool value(HANDLE p,uint32_t a,T& out){return read(p,a,&out,sizeof(out));}
std::string hashFile(const std::wstring& path){
    HCRYPTPROV prov=0;HCRYPTHASH hash=0;
    if(!CryptAcquireContextW(&prov,nullptr,nullptr,PROV_RSA_AES,CRYPT_VERIFYCONTEXT))throw fail("CryptAcquireContext");
    if(!CryptCreateHash(prov,CALG_SHA_256,0,0,&hash)){CryptReleaseContext(prov,0);throw fail("CryptCreateHash");}
    try{
        std::ifstream f(std::filesystem::path(path),std::ios::binary);if(!f)throw std::runtime_error("Nao abriu arquivo para SHA256");
        std::array<char,65536> b{};while(f){f.read(b.data(),b.size());if(f.gcount()&&!CryptHashData(hash,reinterpret_cast<BYTE*>(b.data()),DWORD(f.gcount()),0))throw fail("CryptHashData");}
        if(!f.eof())throw std::runtime_error("Falha lendo arquivo para SHA256");
        BYTE digest[32];DWORD n=32;if(!CryptGetHashParam(hash,HP_HASHVAL,digest,&n,0))throw fail("CryptGetHashParam");
        std::ostringstream out;for(BYTE c:digest)out<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(c);
        CryptDestroyHash(hash);CryptReleaseContext(prov,0);return out.str();
    }catch(...){CryptDestroyHash(hash);CryptReleaseContext(prov,0);throw;}
}
struct Config{DWORD pid=0,windowThread=0;uint32_t base=0,game=0,dispatcherOwner=0;std::array<uint32_t,3> functions{};unsigned seconds=60,stopReturns=0,watchMask=7;bool fixture=false,observeFixtureState=false,dispatcherMode=false;};
struct WindowQuery{DWORD pid,thread;};
BOOL CALLBACK findWindow(HWND hwnd,LPARAM arg){auto* q=reinterpret_cast<WindowQuery*>(arg);DWORD pid=0;DWORD tid=GetWindowThreadProcessId(hwnd,&pid);
    if(pid==q->pid&&IsWindowVisible(hwnd)&&!GetWindow(hwnd,GW_OWNER)){q->thread=tid;return FALSE;}return TRUE;}
const char* methodNames[]={"Game::attack","Game::walk","Game::autoWalk"};
Config realConfig(DWORD pid,HANDLE process,unsigned seconds){
    Handle snap(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid));
    if(snap.h==INVALID_HANDLE_VALUE)throw fail("CreateToolhelp32Snapshot");
    MODULEENTRY32W m{};m.dwSize=sizeof(m);if(!Module32FirstW(snap.h,&m))throw fail("Module32FirstW");
    if(_wcsicmp(m.szModule,L"AncientiaDX.exe"))throw std::runtime_error("PID nao pertence ao AncientiaDX.exe");
    if(hashFile(m.szExePath)!="e6c7daedcab8c324697ce55c564a7300997507fd44454854109707c344208be2")throw std::runtime_error("SHA256 desconhecido; nao anexou depurador");
    Config c;c.pid=pid;c.base=reinterpret_cast<uint32_t>(m.modBaseAddr);c.game=c.base+0xD43090;c.seconds=seconds;
    IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS32 nt{};
    if(!value(process,c.base,dos)||dos.e_magic!=IMAGE_DOS_SIGNATURE||dos.e_lfanew<64||dos.e_lfanew>0x100000||
       !value(process,c.base+dos.e_lfanew,nt)||nt.Signature!=IMAGE_NT_SIGNATURE||nt.FileHeader.Machine!=0x14c||
       nt.FileHeader.TimeDateStamp!=0x6AEBB27C||nt.OptionalHeader.SizeOfImage!=0xDDF000||nt.OptionalHeader.Magic!=0x10b)
        throw std::runtime_error("Cabecalho em memoria diverge; nao anexou depurador");
    c.functions={c.base+0x80030,c.base+0x7D0F0,c.base+0x7C9D0};
    for(auto fn:c.functions){BYTE code[3];if(!read(process,fn,code,3)||memcmp(code,"\x55\x8b\xec",3))throw std::runtime_error("Entrada de funcao diferente; nao anexou depurador");}
    BYTE getter[4];if(!read(process,c.base+0x5B2E0,getter,4)||memcmp(getter,"\x8b\x41\x40\xc3",4))throw std::runtime_error("Getter de Creature::id diverge; nao anexou depurador");
    WindowQuery q{pid,0};EnumWindows(findWindow,reinterpret_cast<LPARAM>(&q));c.windowThread=q.thread;
    return c;
}
std::string objectSnapshot(HANDLE process,uint32_t a,uint32_t base){
    std::array<BYTE,0x80> b{};if(!a||!read(process,a,b.data(),b.size()))return "null";
    auto u32=[&](unsigned off){uint32_t v;memcpy(&v,b.data()+off,4);return v;};
    std::ostringstream s;s<<"{\"address\":"<<quote(hx(a))<<",\"vtable\":"<<quote(hx(u32(0)))
        <<",\"refCountAt4\":"<<u32(4)<<",\"candidateIdAt40\":"<<u32(0x40)
        <<",\"monsterVtableMatch\":"<<(u32(0)==base+0xABA974?"true":"false")
        <<",\"position\":["<<u32(0xc)<<','<<u32(0x10)<<','<<u32(0x14)<<"],\"hpPercentAt5C\":"<<unsigned(b[0x5c]);
    uint32_t len=u32(0x54),cap=u32(0x58);std::string name;
    if(len&&len<=128&&cap>=len&&cap<=4096){
        if(cap==15&&len<=15)name.assign(reinterpret_cast<char*>(b.data()+0x44),len);
        else if(cap>=16){name.resize(len);if(!read(process,u32(0x44),name.data(),len))name.clear();}
    }
    s<<",\"nameCandidate\":"<<quote(name)<<'}';return s.str();
}
struct Pending{int method;uint32_t ret,esp,arg0;};
struct Thread{HANDLE handle=nullptr;CONTEXT original{};std::vector<Pending> pending;};
struct Trace{
    Config config;HANDLE process;std::map<DWORD,Thread> threads;
    std::vector<std::string> records,errors,hardwareChecks,targetStates;std::vector<DWORD> armedThreadIds;bool attached=false,pendingEvent=false,exited=false,detached=false;
    bool targetInitialized=false,targetChangedNoEntry=false;uint32_t initialTarget=0,lastTarget=0;ULONGLONG targetChangedAt=0;
    DEBUG_EVENT event{};DWORD continuation=DBG_CONTINUE;unsigned entries=0,returns=0,restored=0,armed=0;
    HANDLE fixtureGo=nullptr;
    Trace(Config c,HANDLE p):config(c),process(p){}
    ~Trace(){cleanup();}
    const char* label(int method){return config.dispatcherMode?"Dispatcher::poll":methodNames[method];}
    bool observeTarget(){
        if(config.dispatcherMode||(config.fixture&&!config.observeFixtureState)||config.watchMask!=1)return false;
        uint32_t target=0;if(!value(process,config.game+0x34,target))return false;
        if(!targetInitialized||target!=lastTarget){
            if(!targetInitialized){initialTarget=target;targetInitialized=true;}
            std::ostringstream s;s<<"{\"tick\":"<<GetTickCount64()<<",\"address\":"<<quote(hx(target))
                <<",\"creature\":"<<objectSnapshot(process,target,config.base)<<'}';targetStates.push_back(s.str());lastTarget=target;
        }
        if(entries==0&&initialTarget==0&&target!=0){
            if(!targetChangedAt)targetChangedAt=GetTickCount64();
            if(GetTickCount64()-targetChangedAt>=2500){
                targetChangedNoEntry=true;
                std::cout<<"[DIAGNOSTICO] O alvo mudou na memoria, mas a entrada da funcao nao foi observada. Encerrando para analisar, sem chamar a funcao.\n"<<std::flush;
                return true;
            }
        }else targetChangedAt=0;
        return false;
    }
    bool hardwareMatches(const CONTEXT& c){
        const DWORD addresses[]={c.Dr0,c.Dr1,c.Dr2};
        for(unsigned i=0;i<3;++i)if(config.watchMask&(1u<<i)){
            if(addresses[i]!=config.functions[i]||!(c.Dr7&(1u<<(i*2)))||(c.Dr7&(0xfu<<(16+i*4))))return false;
        }
        return true;
    }
    void verifyHardware(){
        if(threads.empty())return;
        auto it=threads.find(config.windowThread);if(it==threads.end())it=threads.begin();
        bool paused=SuspendThread(it->second.handle)!=DWORD(-1);if(!paused)throw fail("SuspendThread verificacao");
        CONTEXT c{};c.ContextFlags=CONTEXT_DEBUG_REGISTERS;BOOL got=GetThreadContext(it->second.handle,&c);DWORD error=GetLastError();
        BOOL resumed=ResumeThread(it->second.handle)!=DWORD(-1);
        if(!got){SetLastError(error);throw fail("GetThreadContext verificacao");}
        if(!resumed)throw fail("ResumeThread verificacao");
        bool ok=hardwareMatches(c);std::ostringstream s;
        s<<"{\"threadId\":"<<it->first<<",\"matches\":"<<(ok?"true":"false")<<",\"dr0\":"<<quote(hx(c.Dr0))<<",\"dr1\":"<<quote(hx(c.Dr1))<<",\"dr2\":"<<quote(hx(c.Dr2))<<",\"dr7\":"<<quote(hx(c.Dr7))<<'}';
        hardwareChecks.push_back(s.str());
        if(!ok)throw std::runtime_error("Breakpoints diferem da configuracao aplicada; observacao interrompida sem tentar reaplica-los");
    }
    void arm(DWORD tid,HANDLE eventHandle){
        Thread t;if(!DuplicateHandle(GetCurrentProcess(),eventHandle,GetCurrentProcess(),&t.handle,0,FALSE,DUPLICATE_SAME_ACCESS))throw fail("DuplicateHandle");
        t.original.ContextFlags=CONTEXT_DEBUG_REGISTERS;
        if(!GetThreadContext(t.handle,&t.original)){CloseHandle(t.handle);throw fail("GetThreadContext inicial");}
        if(t.original.Dr7&0xff){CloseHandle(t.handle);throw std::runtime_error("Thread ja possui breakpoints de hardware; nao os substituiu");}
        CONTEXT c=t.original;c.Dr0=config.functions[0];c.Dr1=config.functions[1];c.Dr2=config.functions[2];c.Dr3=0;c.Dr6=0;c.Dr7=0;
        for(unsigned i=0;i<3;++i)if(config.watchMask&(1u<<i))c.Dr7|=1u<<(i*2);
        if(!SetThreadContext(t.handle,&c)){CloseHandle(t.handle);throw fail("SetThreadContext inicial");}
        threads.emplace(tid,t);
        ++armed;armedThreadIds.push_back(tid);
        CONTEXT verify{};verify.ContextFlags=CONTEXT_DEBUG_REGISTERS;
        if(!GetThreadContext(t.handle,&verify)||!hardwareMatches(verify))throw std::runtime_error("Breakpoints nao conferiram apos configurar a thread");
    }
    void continueEvent(){if(pendingEvent){if(!ContinueDebugEvent(event.dwProcessId,event.dwThreadId,continuation))throw fail("ContinueDebugEvent");pendingEvent=false;}}
    void cleanup() noexcept{
        if(!attached)return;
        for(auto& [tid,t]:threads){bool paused=false;
            if(!exited&&!pendingEvent){paused=SuspendThread(t.handle)!=DWORD(-1);if(!paused)errors.push_back("SuspendThread restore "+std::to_string(tid));}
            if(!exited&&(pendingEvent||paused)){
                CONTEXT c{};c.ContextFlags=CONTEXT_DEBUG_REGISTERS;
                if(GetThreadContext(t.handle,&c)){
                    c.Dr0=t.original.Dr0;c.Dr1=t.original.Dr1;c.Dr2=t.original.Dr2;c.Dr3=t.original.Dr3;c.Dr6=t.original.Dr6;c.Dr7=t.original.Dr7;
                    if(SetThreadContext(t.handle,&c)){
                        CONTEXT verify{};verify.ContextFlags=CONTEXT_DEBUG_REGISTERS;
                        if(GetThreadContext(t.handle,&verify)&&verify.Dr0==t.original.Dr0&&verify.Dr1==t.original.Dr1&&
                           verify.Dr2==t.original.Dr2&&verify.Dr3==t.original.Dr3&&verify.Dr6==t.original.Dr6&&verify.Dr7==t.original.Dr7)++restored;
                        else errors.push_back("Debug registers restore readback mismatch "+std::to_string(tid));
                    }else errors.push_back("SetThreadContext restore "+std::to_string(tid));
                }else errors.push_back("GetThreadContext restore "+std::to_string(tid));
            }
            if(paused&&ResumeThread(t.handle)==DWORD(-1))errors.push_back("ResumeThread "+std::to_string(tid));
            CloseHandle(t.handle);
        }
        threads.clear();
        if(pendingEvent){if(!ContinueDebugEvent(event.dwProcessId,event.dwThreadId,continuation))errors.push_back("ContinueDebugEvent cleanup");pendingEvent=false;}
        detached=exited||!!DebugActiveProcessStop(config.pid);
        if(!detached)errors.push_back("DebugActiveProcessStop WinError "+std::to_string(GetLastError()));
        attached=false;
    }
    void entry(Thread& t,CONTEXT& c,int method){
        if(config.dispatcherMode&&c.Ecx!=config.dispatcherOwner)return;
        std::array<uint32_t,9> stack{};bool readable=read(process,c.Esp,stack.data(),sizeof(stack));
        std::ostringstream s;s<<"{\"phase\":\"entry\",\"threadId\":"<<event.dwThreadId<<",\"method\":"<<quote(label(method))
            <<",\"eip\":"<<quote(hx(c.Eip))<<",\"ecx\":"<<quote(hx(c.Ecx))<<",\"ecxMatchesGame\":"<<(c.Ecx==config.game?"true":"false")
            <<",\"ecxMatchesExpectedOwner\":"<<(c.Ecx==(config.dispatcherMode?config.dispatcherOwner:config.game)?"true":"false")
            <<",\"esp\":"<<quote(hx(c.Esp))<<",\"stackRead\":"<<(readable?"true":"false")<<",\"wordsAfterReturn\":[";
        for(unsigned i=1;i<stack.size();++i){if(i>1)s<<',';s<<quote(hx(stack[i]));}s<<']';
        if(readable){s<<",\"returnAddress\":"<<quote(hx(stack[0]));
            if(method==0&&!config.dispatcherMode)s<<",\"creatureArgument\":"<<objectSnapshot(process,stack[1],config.base)<<",\"cancelLowByte\":"<<(stack[2]&255);
            if(method==2){std::array<uint32_t,6> words{};bool ok=read(process,stack[1],words.data(),sizeof(words));
                s<<",\"arg0MemoryRead\":"<<(ok?"true":"false")<<",\"arg0MemoryWords\":[";
                for(size_t i=0;i<words.size();++i){if(i)s<<',';s<<quote(hx(words[i]));}s<<']';}
            if(stack[0]>=0x10000&&t.pending.size()<8)t.pending.push_back({method,stack[0],c.Esp,stack[1]});
        }
        uint32_t target=0;value(process,config.game+0x34,target);s<<",\"attackTargetBefore\":"<<quote(hx(target))<<'}';
        records.push_back(s.str());++entries;
        std::cout<<"[ENTRADA] "<<label(method)<<" thread="<<event.dwThreadId<<" ECX="<<hx(c.Ecx)<<"\n"<<std::flush;
    }
    void returned(Thread& t,CONTEXT& c){
        Pending call=t.pending.back();t.pending.pop_back();uint32_t target=0;value(process,config.game+0x34,target);
        std::ostringstream s;s<<"{\"phase\":\"return\",\"threadId\":"<<event.dwThreadId<<",\"method\":"<<quote(label(call.method))
          <<",\"espDelta\":"<<int64_t(c.Esp)-call.esp<<",\"eax\":"<<quote(hx(c.Eax))
          <<",\"attackTargetAfter\":"<<quote(hx(target));
        if(call.method==0&&!config.dispatcherMode)s<<",\"creatureAfter\":"<<objectSnapshot(process,call.arg0,config.base);
        s<<'}';records.push_back(s.str());++returns;
    }
    void run(){
        if(!DebugActiveProcess(config.pid))throw fail("DebugActiveProcess");
        attached=true;
        if(!DebugSetProcessKillOnExit(FALSE))throw fail("DebugSetProcessKillOnExit");
        bool initial=true;ULONGLONG until=GetTickCount64()+config.seconds*1000ULL,lastCheck=0;
        while(!stopped&&GetTickCount64()<until&&entries<128){
            if(!WaitForDebugEvent(&event,100)){DWORD error=GetLastError();if(error==ERROR_SEM_TIMEOUT||error==0){
                if(!initial){if(GetTickCount64()-lastCheck>=2000){verifyHardware();lastCheck=GetTickCount64();}
                    if(observeTarget())break;}continue;}throw fail("WaitForDebugEvent");}
            pendingEvent=true;continuation=DBG_CONTINUE;
            switch(event.dwDebugEventCode){
                case CREATE_PROCESS_DEBUG_EVENT:
                    if(event.u.CreateProcessInfo.hFile)CloseHandle(event.u.CreateProcessInfo.hFile);
                    arm(event.dwThreadId,event.u.CreateProcessInfo.hThread);break;
                case CREATE_THREAD_DEBUG_EVENT:arm(event.dwThreadId,event.u.CreateThread.hThread);break;
                case LOAD_DLL_DEBUG_EVENT:if(event.u.LoadDll.hFile)CloseHandle(event.u.LoadDll.hFile);break;
                case EXIT_THREAD_DEBUG_EVENT:{auto it=threads.find(event.dwThreadId);if(it!=threads.end()){CloseHandle(it->second.handle);threads.erase(it);}break;}
                case EXIT_PROCESS_DEBUG_EVENT:exited=true;break;
                case EXCEPTION_DEBUG_EVENT:{
                    DWORD code=event.u.Exception.ExceptionRecord.ExceptionCode;continuation=DBG_EXCEPTION_NOT_HANDLED;
                    if(initial&&code==EXCEPTION_BREAKPOINT){initial=false;continuation=DBG_CONTINUE;
                        until=GetTickCount64()+config.seconds*1000ULL;
                        observeTarget();
                        const char* instruction=config.dispatcherMode?"Somente deixe o jogo aberto. Observando tres ciclos do dispatcher.":config.watchMask==1?"Ataque UM monstro manualmente.":config.watchMask==2?"Caminhe UMA vez com uma seta.":config.watchMask==4?"Clique num destino proximo no mapa/minimapa.":"Ataque, caminhe com seta e clique no mapa/minimapa.";
                        std::cout<<"[PRONTO] "<<instruction<<"\n"<<std::flush;
                        if(fixtureGo)SetEvent(fixtureGo);
                    }else if(code==EXCEPTION_SINGLE_STEP){
                        auto it=threads.find(event.dwThreadId);if(it==threads.end())break;
                        CONTEXT c{};c.ContextFlags=CONTEXT_CONTROL|CONTEXT_INTEGER|CONTEXT_DEBUG_REGISTERS;
                        if(!GetThreadContext(it->second.handle,&c))throw fail("GetThreadContext hit");
                        Thread& t=it->second;bool ours=false;
                        if(!t.pending.empty()&&(c.Dr6&8)&&c.Eip==t.pending.back().ret){ours=true;returned(t,c);}
                        else for(int i=0;i<3;++i)if((c.Dr6&(1u<<i))&&c.Eip==config.functions[i]){ours=true;entry(t,c,i);break;}
                        if(ours){
                            continuation=DBG_CONTINUE;c.Dr6=0;c.EFlags|=0x10000;
                            c.Dr3=t.pending.empty()?0:t.pending.back().ret;
                            c.Dr7=(c.Dr7&~(0xfu<<28))&~0xC0u;if(c.Dr3)c.Dr7|=0x40;
                            if(!SetThreadContext(t.handle,&c))throw fail("SetThreadContext resume");
                        }
                    }
                    break;
                }
            }
            if(exited||(config.stopReturns&&returns>=config.stopReturns))break;
            continueEvent();
        }
        cleanup();
    }
    void save(const std::filesystem::path& path){
        std::ofstream f(path);if(!f)throw std::runtime_error("Nao abriu arquivo de relatorio");
        f<<"{\n\"schema\":\"ancientia-native-observation-v2\",\n\"pid\":"<<config.pid
          <<",\n\"fixture\":"<<(config.fixture?"true":"false")<<",\n\"base\":"<<quote(hx(config.base))
          <<",\n\"game\":"<<quote(hx(config.game))<<",\n\"dispatcherMode\":"<<(config.dispatcherMode?"true":"false")<<",\n\"dispatcherOwner\":"<<quote(hx(config.dispatcherOwner))<<",\n\"nativeCallsRequested\":false,\n\"gameCodePatched\":false,\n\"dllInjected\":false,\n"
          <<"\"threadDebugRegistersTemporarilyChanged\":"<<(armed?"true":"false")<<",\n\"windowThreadId\":"<<config.windowThread<<",\n\"entries\":"<<entries<<",\n\"returns\":"<<returns
          <<",\n\"restoredThreads\":"<<restored<<",\n\"detached\":"<<(detached?"true":"false")<<",\n\"watchMask\":"<<config.watchMask<<",\n\"armedThreadIds\":[";
        for(size_t i=0;i<armedThreadIds.size();++i){if(i)f<<',';f<<armedThreadIds[i];}f<<"],\n\"hardwareChecks\":[";
        for(size_t i=0;i<hardwareChecks.size();++i){if(i)f<<',';f<<hardwareChecks[i];}f<<"],\n\"targetChangedWithoutFunctionEntry\":"<<(targetChangedNoEntry?"true":"false")<<",\n\"targetStates\":[";
        for(size_t i=0;i<targetStates.size();++i){if(i)f<<',';f<<targetStates[i];}f<<"],\n\"records\":[";
        for(size_t i=0;i<records.size();++i){if(i)f<<',';f<<'\n'<<records[i];}f<<"\n],\n\"diagnostics\":[";
        for(size_t i=0;i<errors.size();++i){if(i)f<<',';f<<quote(errors[i]);}f<<"]\n}\n";
        if(!f)throw std::runtime_error("Falha ao gravar relatorio");
    }
};

// Fixture isolada: a mesma ferramenta inicia uma copia propria, nunca o jogo.
struct FixtureInfo{DWORD magic,pid,game,attack,walk,autoWalk,poll;};
volatile uint32_t sink=0;
struct FixtureGame{
    BYTE padding[0x34]{};volatile uint32_t attackPointer=0;
    __attribute__((noinline)) void attack(uint32_t target,bool cancel){sink=target+unsigned(cancel);}
    __attribute__((noinline)) void walk(uint32_t direction,bool pre){sink=direction+unsigned(pre);}
    struct Position{uint32_t x,y,z;};
    __attribute__((noinline)) bool autoWalk(uint32_t directions,Position p){sink=directions+p.x+p.y+p.z;return true;}
    __attribute__((noinline)) void poll(){++attackPointer;sink=attackPointer;}
};
static_assert(offsetof(FixtureGame,attackPointer)==0x34);
template<class T>uint32_t methodAddress(T fn){uint32_t result=0;memcpy(&result,&fn,4);return result;}
int fixtureChild(const std::wstring& token,bool stateOnly=false,bool dispatcherOnly=false){
    Handle mapping(OpenFileMappingW(FILE_MAP_WRITE,FALSE,(token+L".map").c_str()));
    Handle ready(OpenEventW(EVENT_MODIFY_STATE,FALSE,(token+L".ready").c_str()));
    Handle go(OpenEventW(SYNCHRONIZE,FALSE,(token+L".go").c_str()));
    Handle quit(OpenEventW(SYNCHRONIZE,FALSE,(token+L".quit").c_str()));
    if(!mapping.h||!ready.h||!go.h||!quit.h)return 20;
    auto* info=static_cast<FixtureInfo*>(MapViewOfFile(mapping.h,FILE_MAP_WRITE,0,0,sizeof(FixtureInfo)));if(!info)return 21;
    FixtureGame game;
    *info={0x414E4354,GetCurrentProcessId(),reinterpret_cast<uint32_t>(&game),
        methodAddress(&FixtureGame::attack),methodAddress(&FixtureGame::walk),methodAddress(&FixtureGame::autoWalk),methodAddress(&FixtureGame::poll)};
    SetEvent(ready.h);UnmapViewOfFile(info);
    if(WaitForSingleObject(go.h,15000)!=WAIT_OBJECT_0)return 22;
    Sleep(2300);
    if(dispatcherOnly){game.poll();game.poll();game.poll();}
    else if(stateOnly)game.attackPointer=0x12345678;
    else {game.attack(0x12345678,false);game.walk(0,true);game.autoWalk(0x12345678,{100,101,7});game.attack(0,true);game.walk(1,false);}
    if(WaitForSingleObject(quit.h,15000)!=WAIT_OBJECT_0)return 23;
    // Parent report validates restore and detach; child verifies normal completion.
    return (dispatcherOnly?sink==3:stateOnly?game.attackPointer==0x12345678:sink==1)?0:24;
}
int selfTest(const std::filesystem::path& output,unsigned mask=7,bool stateOnly=false,bool dispatcherOnly=false){
    std::wstring token=L"Local\\AncientiaTraceTest."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(GetTickCount64());
    Handle mapping(CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(FixtureInfo),(token+L".map").c_str()));
    Handle ready(CreateEventW(nullptr,TRUE,FALSE,(token+L".ready").c_str()));
    Handle go(CreateEventW(nullptr,TRUE,FALSE,(token+L".go").c_str()));
    Handle quit(CreateEventW(nullptr,TRUE,FALSE,(token+L".quit").c_str()));
    if(!mapping.h||!ready.h||!go.h||!quit.h)throw fail("Fixture events");
    wchar_t exe[32768];if(!GetModuleFileNameW(nullptr,exe,32768))throw fail("GetModuleFileName");
    std::wstring command=L"\""+std::wstring(exe)+L"\" --fixture-child \""+token+L"\"";
    if(stateOnly)command+=L" state-only";
    if(dispatcherOnly)command+=L" dispatcher-only";
    STARTUPINFOW si{};si.cb=sizeof(si);PROCESS_INFORMATION pi{};
    if(!CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi))throw fail("CreateProcess fixture");
    Handle process(pi.hProcess),thread(pi.hThread);
    auto cleanup=[&](){SetEvent(go.h);SetEvent(quit.h);if(WaitForSingleObject(process.h,5000)!=WAIT_OBJECT_0)TerminateProcess(process.h,99);};
    try{
        if(WaitForSingleObject(ready.h,5000)!=WAIT_OBJECT_0)throw std::runtime_error("Fixture nao ficou pronta");
        auto* view=static_cast<FixtureInfo*>(MapViewOfFile(mapping.h,FILE_MAP_READ,0,0,sizeof(FixtureInfo)));if(!view)throw fail("MapViewOfFile");
        FixtureInfo info=*view;UnmapViewOfFile(view);if(info.magic!=0x414E4354||info.pid!=pi.dwProcessId)throw std::runtime_error("Fixture identity");
        unsigned expected=dispatcherOnly?3:stateOnly?0:mask==7?5:mask==4?1:2;
        Config c;c.pid=info.pid;c.game=info.game;c.functions={info.attack,info.walk,info.autoWalk};c.seconds=8;c.stopReturns=expected;c.fixture=true;c.watchMask=mask;
        c.observeFixtureState=stateOnly;
        if(dispatcherOnly){c.dispatcherMode=true;c.dispatcherOwner=info.game;c.functions[0]=info.poll;}
        Trace tracer(c,process.h);tracer.fixtureGo=go.h;
        try{tracer.run();}catch(...){tracer.cleanup();tracer.save(output);throw;}
        tracer.save(output);SetEvent(quit.h);
        if(WaitForSingleObject(process.h,5000)!=WAIT_OBJECT_0)throw std::runtime_error("Fixture nao encerrou");
        DWORD code=99;GetExitCodeProcess(process.h,&code);
        bool ok=tracer.entries==expected&&tracer.returns==expected&&tracer.detached&&tracer.errors.empty()&&tracer.restored>0&&code==0&&!tracer.hardwareChecks.empty();
        if(stateOnly)ok=ok&&tracer.targetChangedNoEntry;
        std::cout<<(ok?"PASS":"FAIL")<<": fixture propria, entradas/retornos, restauracao e detach.\n";
        return ok?0:2;
    }catch(...){cleanup();throw;}
}
int wmain(int argc,wchar_t** argv){
    SetConsoleCtrlHandler(stopHandler,TRUE);
    try{
        if(argc>=3&&!wcscmp(argv[1],L"--fixture-child"))return fixtureChild(argv[2],argc>=4&&!wcscmp(argv[3],L"state-only"),argc>=4&&!wcscmp(argv[3],L"dispatcher-only"));
        if(argc>=2&&!wcscmp(argv[1],L"--self-test")){
            unsigned mask=7;bool stateOnly=false,dispatcherOnly=false;if(argc>=4){std::wstring m=argv[3];if(m==L"attack")mask=1;else if(m==L"walk")mask=2;else if(m==L"autowalk")mask=4;else if(m==L"state"){mask=1;stateOnly=true;}else if(m==L"dispatcher"){mask=1;dispatcherOnly=true;}else if(m!=L"all")throw std::runtime_error("Metodo self-test invalido");}
            return selfTest(argc>=3?argv[2]:L"Ancientia_native_selftest.json",mask,stateOnly,dispatcherOnly);
        }
        if(argc<2||!wcscmp(argv[1],L"--help")){
            std::cout<<"AncientiaNativeTrace_x86.exe PID [segundos 5..120] [relatorio.json]\nObserva ataques/passos MANUAIS; nao chama funcoes do jogo.\n--self-test [relatorio.json] usa somente uma fixture propria.\n";return 0;}
        size_t used=0;auto numeric=std::stoul(argv[1],&used);if(!numeric||numeric>0xffffffffu||used!=wcslen(argv[1]))throw std::runtime_error("PID invalido");
        DWORD pid=DWORD(numeric);unsigned seconds=argc>=3?std::stoul(argv[2]):60;if(seconds<5||seconds>120)throw std::runtime_error("Duracao deve ficar entre 5 e 120 segundos");
        std::filesystem::path output=argc>=4?argv[3]:L"Ancientia_native_trace.json";
        if(std::filesystem::exists(output))throw std::runtime_error("Relatorio ja existe; use outro nome para preservar a coleta");
        Handle process(OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ|SYNCHRONIZE,FALSE,pid));if(!process.h)throw fail("OpenProcess");
        Config config=realConfig(pid,process.h,seconds);
        if(argc>=5){std::wstring method=argv[4];
            if(method==L"attack")config.watchMask=1;else if(method==L"walk")config.watchMask=2;else if(method==L"autowalk")config.watchMask=4;
            else if(method==L"dispatcher"){
                config.watchMask=1;config.dispatcherMode=true;config.dispatcherOwner=config.base+0xD43880;config.functions[0]=config.base+0x1E4990;
                BYTE entry[6];if(!read(process.h,config.functions[0],entry,6)||memcmp(entry,"\x53\x8b\xdc\x83\xec\x08",6))throw std::runtime_error("Entrada do dispatcher diverge; nao anexou depurador");
                BYTE cmp[6];if(!read(process.h,config.base+0x1E4A56,cmp,6)||cmp[0]!=0x81||cmp[1]!=0xFF)throw std::runtime_error("Ancora do dispatcher diverge");
                uint32_t owner=0;memcpy(&owner,cmp+2,4);if(owner!=config.dispatcherOwner)throw std::runtime_error("Instancia do dispatcher diverge");
            }else if(method!=L"all")throw std::runtime_error("Metodo invalido: attack, walk, autowalk, dispatcher ou all");
            if(config.watchMask!=7)config.stopReturns=config.dispatcherMode?3:1;
        }
        Trace tracer(config,process.h);
        try{tracer.run();}catch(const std::exception& e){tracer.errors.push_back(e.what());tracer.cleanup();tracer.save(output);throw;}
        tracer.save(output);std::wcout<<L"Relatorio: "<<output.wstring()<<L"\n";
        return tracer.detached&&tracer.errors.empty()?0:2;
    }catch(const std::exception& e){std::cerr<<"[ERRO] "<<e.what()<<"\n";return 1;}
}
