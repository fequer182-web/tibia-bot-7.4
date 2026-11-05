#include "../native_runtime/runtime/OldersBaseline.hpp"
#include "../native_runtime/runtime/OldersPipe.hpp"
#include "../native_runtime/runtime/LegacyWindowsPath.hpp"
#include <iostream>
#include <stdexcept>
static void check(bool good,const char* message){if(!good)throw std::runtime_error(message);}
int main(){try{
    using namespace crome;
    check(olders_baseline::matches(0x6AA9FA57,0x17BF000,0x89D3D35243890EA2ULL),"exact legacy fingerprint rejected");
    check(!olders_baseline::matches(0x6AA9FA57,0x17BF000,0x89D3D35243890EA3ULL),"different build accepted");
    check(!olders_baseline::matchesExecutable(L"missing-test-executable.exe"),"missing executable accepted");
    // Use a writable workspace fixture, including an accented Windows path.
    const auto path=std::filesystem::current_path()/"outputs"/(L"crome-pipe-teste-\u00e1-"+std::to_wstring(GetCurrentProcessId()));
    olders_pipe::Server server(path);server.start();nlohmann::json reply;
    for(unsigned retry=0;retry<20&&!olders_pipe::request(GetCurrentProcessId(),{{"cmd","STATUS"}},reply);++retry)Sleep(50);
    check(reply.value("ok",false)&&!reply.value("ready",true),"unready runtime enabled");
    check(!olders_pipe::request(GetCurrentProcessId(),{{"cmd","SET_CONFIG"},{"file","tools.json"},{"value",{{"enabled",false}}}},reply),"unready config accepted");
    server.setReady(true);
    for(unsigned round=0;round<10;++round){check(olders_pipe::request(GetCurrentProcessId(),{{"cmd","STATUS"}},reply)&&reply.value("ready",false),"status handshake lost");
        if(!olders_pipe::request(GetCurrentProcessId(),{{"cmd","SET_CONFIG"},{"file","tools.json"},{"value",{{"enabled",false},{"round",round}}}},reply))throw std::runtime_error("config delivery failed round="+std::to_string(round)+" Win32="+std::to_string(GetLastError())+" response="+reply.dump());}
    nlohmann::json saved;{std::ifstream file(path/"config"/"tools.json");file>>saved;}check(saved.value("round",0)==9,"last acknowledged config not persisted");
    const auto native=(path/"config"/"tools.json").native();
    const int count=WideCharToMultiByte(CP_ACP,0,native.data(),static_cast<int>(native.size()),nullptr,0,nullptr,nullptr);
    std::string ansi(count,'\0');WideCharToMultiByte(CP_ACP,0,native.data(),static_cast<int>(native.size()),ansi.data(),count,nullptr,nullptr);
    check(std::filesystem::exists(legacyWindowsPath(ansi)),"legacy ACP filename lost");
    std::error_code ec;std::filesystem::last_write_time(legacyWindowsPath(ansi),ec);check(!ec,"legacy accented config polling failed");
    check(!olders_pipe::request(GetCurrentProcessId(),{{"cmd","SET_CONFIG"},{"file","../escape.json"},{"value",{{"enabled",true}}}},reply),"path traversal accepted");
    check(!olders_pipe::request(GetCurrentProcessId(),{{"cmd","SET_CONFIG"},{"file","tools.json"},{"value",1}},reply),"invalid config accepted");
    const auto start=GetTickCount64();server.stop();check(GetTickCount64()-start<3000,"idle stop blocked");
    std::filesystem::remove(path/"config"/"tools.json");std::filesystem::remove(path/"config");std::filesystem::remove(path);
    std::cout<<"PASS: exact build gate, readiness, repeated real named-pipe config delivery, input rejection and bounded shutdown\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
