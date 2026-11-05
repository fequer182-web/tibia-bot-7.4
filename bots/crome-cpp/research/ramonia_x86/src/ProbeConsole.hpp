#pragma once
#include <streambuf>

namespace probe_console {
inline std::ostream* screen=nullptr;
inline std::ostream& ui(){return screen?*screen:std::cout;}
class DiagnosticBuffer:public std::streambuf {
    HANDLE file=INVALID_HANDLE_VALUE;
    std::streambuf* console=nullptr;
    std::string pending;
    void emit(bool partial=false) {
        if(pending.empty())return;
        if(file!=INVALID_HANDLE_VALUE){DWORD written=0;WriteFile(file,pending.data(),static_cast<DWORD>(pending.size()),&written,nullptr);}
        // Preserve actionable failures/instructions from the detailed legacy
        // validators. Normal address/score/snapshot lines stay in the log.
        const bool important=pending.find("[STOP]")==0||pending.find("[RETRY]")==0||pending.find("[ERRO]")==0||
            pending.find("[REJEITADO]")==0||pending.find("FAILED")!=std::string::npos||
            pending.find("falhou Win32=")!=std::string::npos||pending.find("Valor invalido")==0||
            pending.find("Gaste mana")==0||pending.find("Mova ")==0||pending.find("Altere ")==0||pending.find("O ideal ")==0||
            (pending.find("[CAPACITY V12]")==0&&pending.find("Altere SOMENTE")!=std::string::npos);
        if(console&&(important||file==INVALID_HANDLE_VALUE)){console->sputn(pending.data(),pending.size());console->pubsync();}
        pending.clear();(void)partial;
    }
protected:
    int_type overflow(int_type c) override {
        if(traits_type::eq_int_type(c,traits_type::eof()))return traits_type::not_eof(c);
        const char value=traits_type::to_char_type(c);pending.push_back(value);
        if(value=='\n'||pending.size()>=4096)emit();return c;
    }
    std::streamsize xsputn(const char* bytes,std::streamsize size) override {
        for(std::streamsize i=0;i<size;++i)overflow(traits_type::to_int_type(bytes[i]));return size;
    }
    int sync() override {emit(true);return 0;}
public:
    DiagnosticBuffer(HANDLE log,std::streambuf* output):file(log),console(output){}
    ~DiagnosticBuffer(){sync();}
};
class Session {
    std::streambuf* original=std::cout.rdbuf();
    std::ostream console{original};
    HANDLE file=INVALID_HANDLE_VALUE;
    DiagnosticBuffer* buffer=nullptr;
public:
    explicit Session(HMODULE module) {
        wchar_t path[32768]{};
        const DWORD size=GetModuleFileNameW(module,path,32768);
        if(size&&size<32768){std::wstring folder(path,size);const auto slash=folder.find_last_of(L"\\/");
            if(slash!=std::wstring::npos){folder.resize(slash+1);folder+=L"ResearchProbeDiagnostic.log";
                file=CreateFileW(folder.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);}}
        buffer=new DiagnosticBuffer(file,original);std::cout.rdbuf(buffer);screen=&console;
        if(file==INVALID_HANDLE_VALUE)ui()<<"Nao foi possivel abrir o log; os detalhes serao exibidos na tela.\n";
    }
    void close() {
        if(!buffer)return;
        buffer->pubsync();std::cout.rdbuf(original);screen=nullptr;delete buffer;buffer=nullptr;
        if(file!=INVALID_HANDLE_VALUE){CloseHandle(file);file=INVALID_HANDLE_VALUE;}
    }
    ~Session(){close();}
};
}
