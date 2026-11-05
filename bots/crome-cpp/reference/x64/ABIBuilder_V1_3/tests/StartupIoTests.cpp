#include "../Source/OTCLab_ABIBuilder_V1_3_X64_AUTOCALIBRATOR_CALLER_EVIDENCE.cpp"
#include <stdexcept>
#include <io.h>

static void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}

int main(){
    try{
        HANDLE inputRead=nullptr,inputWrite=nullptr,outputRead=nullptr,outputWrite=nullptr;
        Check(CreatePipe(&inputRead,&inputWrite,nullptr,0)!=FALSE,"input pipe failed");
        Check(CreatePipe(&outputRead,&outputWrite,nullptr,0)!=FALSE,"output pipe failed");
        DWORD written=0;
        Check(WriteFile(inputWrite,"s\r\n\r\n",5,&written,nullptr)!=FALSE,"input setup failed");
        CloseHandle(inputWrite);
        auto* oldIn=std::cin.rdbuf();auto* oldOut=std::cout.rdbuf();auto* oldErr=std::cerr.rdbuf();
        const HANDLE hostIn=GetStdHandle(STD_INPUT_HANDLE),hostOut=GetStdHandle(STD_OUTPUT_HANDLE),hostErr=GetStdHandle(STD_ERROR_HANDLE);
        FILE* hostStdin=stdin;FILE* hostStdout=stdout;FILE* hostStderr=stderr;
        const auto hostInputFd=_get_osfhandle(_fileno(stdin)),hostOutputFd=_get_osfhandle(_fileno(stdout)),hostErrorFd=_get_osfhandle(_fileno(stderr));
        {
            ScopedConsoleStreams streams(inputRead,outputWrite);
            Check(AskYes("Capture?"),"guided input failed");
            Check(AskYes("Default?",true),"Windows ENTER default lost");
            std::cerr<<"diagnostic\n";
            Check(GetStdHandle(STD_INPUT_HANDLE)==hostIn&&GetStdHandle(STD_OUTPUT_HANDLE)==hostOut&&GetStdHandle(STD_ERROR_HANDLE)==hostErr,"host Win32 handles changed");
            Check(stdin==hostStdin&&stdout==hostStdout&&stderr==hostStderr,"host CRT streams changed");
            Check(_get_osfhandle(_fileno(stdin))==hostInputFd&&_get_osfhandle(_fileno(stdout))==hostOutputFd&&_get_osfhandle(_fileno(stderr))==hostErrorFd,"host CRT descriptors changed");
            Check(!InterlockedCompareExchange(&g_bp.installed,0,0),"startup installed breakpoint");
        }
        Check(std::cin.rdbuf()==oldIn&&std::cout.rdbuf()==oldOut&&std::cerr.rdbuf()==oldErr,"stream buffers not restored");
        CloseHandle(outputWrite);
        char output[256]{};DWORD read=0;
        Check(ReadFile(outputRead,output,sizeof(output)-1,&read,nullptr)!=FALSE,"output read failed");
        Check(std::string(output,read)=="Capture? [S/N]: Default? [S/n]: diagnostic\n","console output lost");
        CloseHandle(inputRead);CloseHandle(outputRead);
        try{ScopedConsoleStreams streams(INVALID_HANDLE_VALUE,INVALID_HANDLE_VALUE);throw std::runtime_error("test");}catch(const std::exception&){}
        Check(std::cin.rdbuf()==oldIn&&std::cout.rdbuf()==oldOut&&std::cerr.rdbuf()==oldErr,"exception left dangling stream buffers");
        std::cout<<"PASS: isolated console IO, host streams preserved, buffers restored after exception, no startup breakpoint\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
