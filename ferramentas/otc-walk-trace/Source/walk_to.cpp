#define CROMIN_WALK_LIB
#include "walk_once.cpp"
#undef main

int NextStep(const Position& p,int x,int y,int z){
    if(!p.valid||p.z!=z)throw std::runtime_error("Posicao invalida ou andar diferente do destino");
    if(int(p.x)!=x)return int(p.x)>x?3:1;
    if(int(p.y)!=y)return int(p.y)>y?0:2;
    return -1;
}
bool ExpectedStep(const Position& a,const Position& b,int direction){
    if(!a.valid||!b.valid)return false;
    int dx=direction==1?1:direction==3?-1:0;
    int dy=direction==2?1:direction==0?-1:0;
    return int(b.x)==int(a.x)+dx && int(b.y)==int(a.y)+dy && b.z==a.z;
}
int main(int argc,char** argv)try{
    DWORD pid=0,tid=0;int x=-1,y=-1,z=-1;
    if(argc==2&&std::string(argv[1])=="--self-test"){
        Position a{};a.valid=true;a.x=32098;a.y=32211;a.z=7;
        if(NextStep(a,32092,32197,7)!=3)throw std::runtime_error("Direcao oeste incorreta");
        auto b=a;--b.x;if(!ExpectedStep(a,b,3)||ExpectedStep(a,a,3))throw std::runtime_error("Verificacao de movimento incorreta");
        a.x=32092;if(NextStep(a,32092,32197,7)!=0)throw std::runtime_error("Direcao norte incorreta");
        a.y=32197;if(NextStep(a,32092,32197,7)!=-1)throw std::runtime_error("Destino incorreto");
        bool refused=false;try{NextStep(a,32092,32197,8);}catch(...){refused=true;}
        if(!refused)throw std::runtime_error("Aceitou outro andar");
        a.x=32098;a.y=32211;
        int count=0;
        while(int d=NextStep(a,32092,32197,7)+1){
            --d;auto previous=a;
            if(d==0)--a.y;else if(d==1)++a.x;else if(d==2)++a.y;else --a.x;
            if(!ExpectedStep(previous,a,d)||++count>20)throw std::runtime_error("Rota simulada incorreta");
        }
        if(count!=20)throw std::runtime_error("Quantidade de passos incorreta");
        std::cout<<"PASS: direcoes, destino, bloqueio, desvio e andar.\n";return 0;
    }
    for(int i=1;i<argc;++i){std::string arg=argv[i];
        if(i+1>=argc)throw std::runtime_error("Falta valor do argumento");
        if(arg=="--pid")pid=std::stoul(argv[++i]);
        else if(arg=="--thread")tid=std::stoul(argv[++i]);
        else if(arg=="--x")x=std::stoi(argv[++i]);
        else if(arg=="--y")y=std::stoi(argv[++i]);
        else if(arg=="--z")z=std::stoi(argv[++i]);
        else throw std::runtime_error("Argumento desconhecido");
    }
    if(!pid||!tid||x<0||x>65535||y<0||y>65535||z<0||z>15)throw std::runtime_error("Uso: OTC_Walk_To.exe --pid PID --thread THREAD --x 32092 --y 32197 --z 7");
    Handle process(OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ|PROCESS_VM_WRITE|PROCESS_VM_OPERATION,FALSE,pid));
    if(!process.h)throw Error("OpenProcess");
    BOOL debug=FALSE;if(!CheckRemoteDebuggerPresent(process.h,&debug))throw Error("CheckRemoteDebuggerPresent");
    if(debug)throw std::runtime_error("Ja existe depurador no processo");
    uintptr_t base=CheckBuild(process.h,pid);CheckShadowStack(process.h);
    if(!GameThreadMatches(process.h,pid,tid,base+0xE53E18))throw std::runtime_error("Thread do jogo diferente");
    std::array<uint8_t,16> expected{0xE8,0x39,0x85,0xFF,0xFF,0x48,0x8B,0x5E,0x08,0x48,0x8B,0xCB,0xE8,0x2D,0x9A,0x61},actual{};
    if(!Read(process.h,base+0x28E9B2,actual.data(),actual.size())||actual!=expected)throw std::runtime_error("Checkpoint diferente");
    Position current=ReadPosition(process.h,base);
    NextStep(current,x,y,z);
    int distance=abs(int(current.x)-x)+abs(int(current.y)-y);
    if(distance>100)throw std::runtime_error("Destino excede o limite de 100 passos deste teste");
    SetConsoleCtrlHandler(ConsoleHandler,TRUE);
    std::cout<<"[DESTINO] "<<x<<","<<y<<","<<z<<". Rota direta: X e depois Y. Ctrl+C para parar entre chamadas.\n";
    for(int step=0;step<=distance;++step){
        int direction=NextStep(current,x,y,z);
        if(direction<0){std::cout<<"[CHEGOU] "<<current.x<<","<<current.y<<","<<unsigned(current.z)<<"\n";return 0;}
        if(interrupted)throw std::runtime_error("Parado pelo usuario");
        auto before=ReadPosition(process.h,base);
        if(!before.valid||before.x!=current.x||before.y!=current.y||before.z!=current.z)throw std::runtime_error("Posicao mudou entre passos. Parou.");
        OneCall call(pid,process.h,base+0x28E9B2,base,tid,base+0x140D60,base+0xE52A40,uint32_t(direction),20);
        auto path=ExePath().parent_path()/("walk_to_"+std::to_string(GetTickCount64())+"_step_"+std::to_string(step+1)+".json");
        try{call.RunOnce();}catch(...){call.Cleanup();call.SaveResult(path);throw;}
        call.SaveResult(path);
        if(!call.completed||!call.detached||!call.contextRestored||!call.stackRestored||!call.diagnostics.empty())throw std::runtime_error("Chamada incompleta; consulte o JSON");
        if(call.positionAfter.empty())throw std::runtime_error("Sem leitura apos chamada");
        auto after=call.positionAfter.back();
        if(!ExpectedStep(before,after,direction))throw std::runtime_error("Bloqueio ou movimento inesperado. Parou sem repetir; nao calcula desvio de obstaculos.");
        current=after;
        std::cout<<"[PASSO "<<step+1<<"] "<<current.x<<","<<current.y<<","<<unsigned(current.z)<<"\n";
    }
    throw std::runtime_error("Limite de passos atingido");
}catch(const std::exception& error){std::cerr<<"[ERRO] "<<error.what()<<"\n";return 1;}
