#pragma once
#include <windows.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <thread>
#include <nlohmann/json.hpp>

namespace crome::olders_pipe {
inline std::wstring name(DWORD pid){return L"\\\\.\\pipe\\Crome.Olders."+std::to_wstring(pid);}
inline bool configName(const std::string& value){
    for(const auto* allowed:{"bot.json","healing.json","targeting.json","loot.json","fishing.json","tools.json","waypoints.json","action_test.json","tile_action_test.json","say_test.json","items.json","inspect_request.json"})if(value==allowed)return true;
    return false;
}
inline bool request(DWORD pid,const nlohmann::json& command,nlohmann::json& response) {
    const auto text=command.dump();if(text.size()>256*1024)return false;
    char buffer[4096]{};DWORD read=0;
    bool received=false;
    for(unsigned retry=0;retry<10;++retry){
        if(CallNamedPipeW(name(pid).c_str(),const_cast<char*>(text.data()),static_cast<DWORD>(text.size()),buffer,sizeof(buffer),&read,1000)){received=true;break;}
        const auto error=GetLastError();if(error!=ERROR_FILE_NOT_FOUND&&error!=ERROR_PIPE_BUSY)return false;
        Sleep(20); // Server closes/recreates the instance between requests.
    }
    if(!received)return false;
    try{response=nlohmann::json::parse(buffer,buffer+read);return response.value("ok",false);}catch(...){return false;}
}
class Server {
    std::atomic<bool> running{false},ready{false};std::thread thread;std::filesystem::path directory;
    bool operation(HANDLE pipe,OVERLAPPED& event,BOOL immediate,DWORD& transferred,DWORD timeout=1500) {
        if(immediate)return true;
        if(GetLastError()!=ERROR_IO_PENDING)return false;
        const auto end=GetTickCount64()+timeout;
        while(running&&GetTickCount64()<end){if(WaitForSingleObject(event.hEvent,100)==WAIT_OBJECT_0)return GetOverlappedResult(pipe,&event,&transferred,FALSE)!=FALSE;}
        CancelIoEx(pipe,&event);GetOverlappedResult(pipe,&event,&transferred,TRUE);return false;
    }
    nlohmann::json dispatch(const nlohmann::json& request) {
        const auto cmd=request.value("cmd",std::string{});
        if(cmd=="STATUS")return {{"ok",true},{"ready",ready.load()},{"protocol",1},{"pid",GetCurrentProcessId()},{"build","6AA9FA57_89D3D35243890EA2"}};
        if(cmd!="SET_CONFIG"||!ready)return {{"ok",false},{"error","runtime_not_ready_or_unknown_command"}};
        const auto file=request.value("file",std::string{});if(!configName(file)||!request.contains("value")||!request["value"].is_object())return {{"ok",false},{"error","invalid_config"}};
        const auto folder=directory/"config";std::error_code ec;std::filesystem::create_directories(folder,ec);if(ec)return {{"ok",false},{"error","config_directory"},{"code",ec.value()}};
        const auto dst=folder/file;const auto tmp=folder/(file+".pipe.tmp");
        {std::ofstream out(tmp,std::ios::binary);if(!out)return {{"ok",false},{"error","config_open"},{"code",GetLastError()}};out<<request["value"].dump(2)<<'\n';out.flush();if(!out)return {{"ok",false},{"error","config_write"}};}
        if(!MoveFileExW(tmp.c_str(),dst.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))return {{"ok",false},{"error","config_replace"},{"code",GetLastError()}};
        return {{"ok",true}};
    }
    void serve() {
        while(running){HANDLE pipe=CreateNamedPipeW(name(GetCurrentProcessId()).c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED,PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,4096,256*1024,0,nullptr);
            if(pipe==INVALID_HANDLE_VALUE){Sleep(100);continue;}
            OVERLAPPED event{};event.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);DWORD count=0;
            const BOOL connected=ConnectNamedPipe(pipe,&event);const DWORD error=connected?0:GetLastError();
            bool valid=connected||error==ERROR_PIPE_CONNECTED;
            if(error==ERROR_IO_PENDING){SetLastError(error);valid=operation(pipe,event,FALSE,count,500);}
            if(valid&&running){ResetEvent(event.hEvent);std::vector<char> input(256*1024);
                const BOOL read=ReadFile(pipe,input.data(),static_cast<DWORD>(input.size()),&count,&event);
                if(operation(pipe,event,read,count)){std::string output;
                    try{output=dispatch(nlohmann::json::parse(input.data(),input.data()+count)).dump();}catch(...){output="{\"ok\":false,\"error\":\"invalid_request\"}";}
                    ResetEvent(event.hEvent);const BOOL wrote=WriteFile(pipe,output.data(),static_cast<DWORD>(output.size()),&count,&event);
                    if(operation(pipe,event,wrote,count)){
                        // CallNamedPipe closes after reading the response. Wait for that
                        // disconnect before discarding buffers, with the same bounded I/O.
                        ResetEvent(event.hEvent);char ack{};const BOOL closed=ReadFile(pipe,&ack,1,&count,&event);operation(pipe,event,closed,count);
                    }
                }
            }
            // Never FlushFileBuffers here: a disappearing client must not pin the DLL.
            DisconnectNamedPipe(pipe);CloseHandle(event.hEvent);CloseHandle(pipe);
        }
    }
public:
    explicit Server(std::filesystem::path folder):directory(std::move(folder)){}
    void start(){running=true;thread=std::thread([this]{serve();});}
    void setReady(bool value){ready=value;}
    void stop(){ready=false;running=false;if(thread.joinable())thread.join();}
    ~Server(){stop();}
};
}
