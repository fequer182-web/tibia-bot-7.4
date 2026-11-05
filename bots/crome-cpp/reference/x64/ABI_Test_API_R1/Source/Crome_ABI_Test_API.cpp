#define CROME_ABI_FINALIZER_NO_ENTRY 1
#include "../../ABI_Finalizer_R1/Source/OTCLab_ABI_Finalizer_X64.cpp"
#include <sddl.h>
#include <condition_variable>
#include <mutex>
#include <atomic>

namespace test_api {
std::mutex lock;
std::condition_variable inputReady;
std::string output, pendingInput, report="null", phase="idle";
bool waiting=false, started=false, finished=false, cancelled=false;
unsigned promptId=0;
constexpr size_t logLimit=262144;
std::atomic<DWORD> startupError{ERROR_IO_PENDING};

std::string quote(const std::string& text){
    std::string result="\"";const char* hex="0123456789abcdef";
    for(unsigned char c:text){if(c=='"'||c=='\\'){result+='\\';result+=c;}else if(c=='\n')result+="\\n";else if(c=='\r')result+="\\r";else if(c=='\t')result+="\\t";else if(c=='\b')result+="\\b";else if(c=='\f')result+="\\f";else if(c<32){result+="\\u00";result+=hex[c>>4];result+=hex[c&15];}else result+=c;}
    return result+'"';
}
void append(const char* data,size_t count){
    std::lock_guard<std::mutex> guard(lock);
    output.append(data,count);if(output.size()>logLimit)output.erase(0,output.size()-logLimit);
}
class Output:public std::streambuf{
    std::streamsize xsputn(const char* data,std::streamsize n)override{if(n>0)append(data,static_cast<size_t>(n));return n;}
    int_type overflow(int_type c)override{if(!traits_type::eq_int_type(c,traits_type::eof())){char v=traits_type::to_char_type(c);append(&v,1);}return traits_type::not_eof(c);}
};
class Input:public std::streambuf{
    std::string current;
    int_type underflow()override{
        if(gptr()&&gptr()<egptr())return traits_type::to_int_type(*gptr());
        std::unique_lock<std::mutex> guard(lock);waiting=true;++promptId;
        inputReady.wait(guard,[]{return !pendingInput.empty()||cancelled;});
        if(cancelled){waiting=false;throw std::runtime_error("test_cancelled");}
        current=std::move(pendingInput);pendingInput.clear();waiting=false;
        setg(current.data(),current.data(),current.data()+current.size());return traits_type::to_int_type(*gptr());
    }
};
struct Streams{
    Input input;Output out;std::streambuf *oldInput,*oldOutput,*oldError;std::ios::iostate oldExceptions;
    Streams():oldInput(std::cin.rdbuf(&input)),oldOutput(std::cout.rdbuf(&out)),oldError(std::cerr.rdbuf(&out)),oldExceptions(std::cin.exceptions()){std::cin.clear();std::cin.exceptions(std::ios::badbit);}
    ~Streams(){std::cin.exceptions(std::ios::goodbit);std::cin.rdbuf(oldInput);std::cout.rdbuf(oldOutput);std::cerr.rdbuf(oldError);std::cin.clear();std::cin.exceptions(oldExceptions);}
};

bool attackSession(){
    Fingerprint live{};if(!GetFingerprint(live)){std::cout<<"[STOP] Fingerprint indisponivel.\n";return false;}
    std::vector<Profile> accepted;
    // Only profiles next to this DLL, never arbitrary paths supplied by the API.
    for(const auto& path:FindProfilesInDirectories({DllDir()})){
        const auto name=path.filename().wstring();if(name.find(L"OTCLab_BuildProfile_")!=0||name.find(L".bak")!=std::wstring::npos)continue;
        auto p=ParseProfile(path);
        if(DiscoveryFingerprintMatches(p)&&CompletionEvidenceMatches(p,ExtractNamedObject(p.raw,"abiCatalog"))&&FocusedFingerprintMatches(p,live))accepted.push_back(std::move(p));
    }
    if(accepted.size()!=1){std::cout<<"[STOP] Deixe ao lado da DLL exatamente UM profile compativel COM abiCatalog; aceitos="<<accepted.size()<<".\n";return false;}
    auto profile=accepted.front();Profile actions;std::string receiver;
    if(!FocusedGameBinding(profile,actions,receiver)){std::cout<<"[STOP] Game receiver atual nao validou.\n";return false;}
    auto backup=profile.path;backup+=L".pre_api.bak.json";
    std::error_code ec;if(!fs::exists(backup,ec)&&!WriteText(backup,profile.raw)){std::cout<<"[STOP] Falha no backup do profile de teste.\n";return false;}
    live.textHash=profile.textHash;
    auto reportPath=profile.path;reportPath.replace_filename(L"Crome_ABI_Independent_Attack_Report.json");
    const auto previousWrite=fs::last_write_time(reportPath,ec);const bool existed=!ec;ec.clear();
    const bool passed=focused_attack::run(profile,actions,live);
    auto result=ReadText(reportPath);
    // Never expose an old report if this run stopped before saving.
    const auto currentWrite=fs::last_write_time(reportPath,ec);
    if(!ec&&(!existed||currentWrite!=previousWrite)&&!result.empty()&&ExtractNamedObject(profile.raw,"abiIndependentAttack")==result){std::lock_guard<std::mutex> guard(lock);report=result;}
    return passed;
}
DWORD WINAPI session(void*){
    bool passed=false;
    {Streams streams;try{std::cout<<"Crome ABI Test API R1: teste independente de ataque.\n";passed=attackSession();}
     catch(const std::exception& e){std::cout<<"[STOP] "<<e.what()<<"\n";}catch(...){std::cout<<"[STOP] Excecao na sessao.\n";}
     UninstallBP();const bool stopped=completion_probe::stop();
     if(!stopped)std::cout<<"[PENDING] Thread nativo ocupado. Reabra o cliente antes de outro teste.\n";
     if(!stopped)passed=false;
    }
    std::lock_guard<std::mutex> guard(lock);finished=true;waiting=false;phase=cancelled?"cancelled":passed?"passed":"pending";return 0;
}
std::string command(const std::string& request){
    try{
        const auto root=crome::json::parse(request);const auto* cmd=root.get("cmd");
        if(!cmd||!cmd->isString())return "{\"ok\":false,\"error\":\"cmd_required\"}";
        const auto name=cmd->string();
        if(name=="PING")return "{\"ok\":true,\"api\":\"crome-abi-test-v1\",\"pid\":"+std::to_string(GetCurrentProcessId())+"}";
        if(name=="STATUS"){
            std::lock_guard<std::mutex> guard(lock);
            return "{\"ok\":true,\"phase\":"+quote(phase)+",\"waiting\":"+(waiting?"true":"false")+",\"finished\":"+(finished?"true":"false")+",\"promptId\":"+std::to_string(promptId)+"}";
        }
        if(name=="OUTPUT"){
            std::lock_guard<std::mutex> guard(lock);const auto count=std::min<size_t>(output.size(),8192);
            auto chunk=output.substr(0,count);output.erase(0,count);return "{\"ok\":true,\"text\":"+quote(chunk)+"}";
        }
        if(name=="REPORT"){std::lock_guard<std::mutex> guard(lock);return "{\"ok\":true,\"report\":"+report+"}";}
        if(name=="CANCEL"){
            std::lock_guard<std::mutex> guard(lock);if(started&&!finished){cancelled=true;inputReady.notify_one();}
            return "{\"ok\":true,\"cancelRequested\":true}";
        }
        if(name=="RUN_ATTACK_TEST"){
            std::lock_guard<std::mutex> guard(lock);if(started)return "{\"ok\":false,\"error\":\"one_session_per_client_restart\"}";
            started=true;phase="running";auto thread=CreateThread(nullptr,0,session,nullptr,0,nullptr);
            if(!thread){started=false;phase="idle";return "{\"ok\":false,\"error\":\"thread_failed\"}";}CloseHandle(thread);
            return "{\"ok\":true,\"started\":true}";
        }
        if(name=="INPUT"){
            const auto* line=root.get("line");const auto* id=root.get("promptId");
            if(!line||!line->isString()||line->string().size()>128||line->string().find_first_of("\r\n\0",0,3)!=std::string::npos||!id||!id->isNumber())return "{\"ok\":false,\"error\":\"invalid_input\"}";
            std::lock_guard<std::mutex> guard(lock);
            if(!waiting||!pendingInput.empty()||id->number()!=promptId)return "{\"ok\":false,\"error\":\"stale_or_unexpected_prompt\"}";
            pendingInput=line->string()+"\n";waiting=false;inputReady.notify_one();return "{\"ok\":true}";
        }
        return "{\"ok\":false,\"error\":\"unknown_command\"}";
    }catch(...){return "{\"ok\":false,\"error\":\"invalid_json\"}";}
}
PSECURITY_DESCRIPTOR currentUserSecurity(){
    HANDLE token{};if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return nullptr;
    DWORD size{};GetTokenInformation(token,TokenUser,nullptr,0,&size);std::vector<uint8_t> buffer(size);
    if(!size||!GetTokenInformation(token,TokenUser,buffer.data(),size,&size)){CloseHandle(token);return nullptr;}CloseHandle(token);
    LPWSTR sid{};if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid,&sid))return nullptr;
    std::wstring sddl=L"D:P(A;;GA;;;SY)(A;;GA;;;";sddl+=sid;sddl+=L")";LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor{};if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&descriptor,nullptr))return nullptr;return descriptor;
}
DWORD WINAPI server(void*){
    auto descriptor=currentUserSecurity();if(!descriptor){startupError=GetLastError();return 0;}
    SECURITY_ATTRIBUTES security{sizeof(security),descriptor,FALSE};
    const auto name=L"\\\\.\\pipe\\Crome.AbiTest."+std::to_wstring(GetCurrentProcessId());
    // DLL intentionally remains loaded until client exits. No unsafe unload endpoint.
    auto pipe=CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,65536,4096,1000,&security);
    const auto error=GetLastError();LocalFree(descriptor);if(pipe==INVALID_HANDLE_VALUE){startupError=error;return 0;}startupError=0;
    for(;;){
        const bool connected=ConnectNamedPipe(pipe,nullptr)||GetLastError()==ERROR_PIPE_CONNECTED;
        if(connected){char bytes[4096];DWORD count{};
            if(ReadFile(pipe,bytes,sizeof(bytes),&count,nullptr)&&count){auto reply=command(std::string(bytes,count));DWORD sent{};if(WriteFile(pipe,reply.data(),static_cast<DWORD>(reply.size()),&sent,nullptr))FlushFileBuffers(pipe);}
        }
        DisconnectNamedPipe(pipe);
    }
}
}
extern "C" __declspec(dllexport) DWORD WINAPI CromeAbiApiStartupError(){return test_api::startupError.load();}
#ifndef CROME_ABI_API_NO_ENTRY
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,LPVOID){
    if(reason==DLL_PROCESS_ATTACH){g_self=module;DisableThreadLibraryCalls(module);
        auto thread=CreateThread(nullptr,0,test_api::server,nullptr,0,nullptr);if(thread)CloseHandle(thread);}
    return TRUE;
}
#endif
