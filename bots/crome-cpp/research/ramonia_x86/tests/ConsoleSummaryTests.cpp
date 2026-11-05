#include "../src/RamoniaResearchProbe.cpp"
#include <stdexcept>
static void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
static grouped_observation::Evidence Pair(const char* cls="Tile",int secondThread=123) {
    grouped_observation::Evidence e;e.hits[1]=2;e.hits[2]=3;
    for(int phase=1;phase<=2;++phase)e.captures.push_back("{\"thisClassObserved\":\""+std::string(cls)+"\",\"phase\":"+std::to_string(phase)+",\"threadId\":"+std::to_string(phase==1?123:secondThread)+"}");
    return e;
}
int main(){try{
    BindResult bind;BindCandidate a;a.methodRva=32;bind.candidates.push_back(a);
    std::map<uint32_t,grouped_observation::Evidence> evidence{{32,Pair()}};
    auto status=[&]{return observation_summary::evaluate("getTopMoveThing","items",bind,evidence,true).status;};
    Check(status()=="CANDIDATO","paired natural calls not shown as candidate");
    evidence[32]=Pair("Item");Check(status()=="INCONCLUSIVO","wrong receiver accepted");
    evidence[32]=Pair("Tile",456);Check(status()=="INCONCLUSIVO","different threads accepted");
    evidence[32]={};evidence[32].hits[0]=20;Check(status()=="NAO OBSERVADO","control-only calls accepted");
    evidence.clear();Check(status()=="NAO TESTADO","unavailable observation accepted");
    evidence[32]=Pair();BindCandidate b;b.methodRva=64;bind.candidates.push_back(b);
    Check(status()=="INCONCLUSIVO","partial coverage reported unique");
    evidence[64]=Pair();Check(status()=="INCONCLUSIVO","two candidates reported unique");
    Check(observation_summary::evaluate("getTopMoveThing","items",bind,evidence,false).status=="INCONCLUSIVO","interrupted session accepted");
    std::ostringstream visible;probe_console::screen=&visible;
    Report report;SelectedField field;field.offset=0;field.type="double";field.status="runtime_transition";report.selectedFields["mana"]=field;
    observation_summary::basics(report);Check(visible.str().find("[PENDENTE] Mana atual")!=std::string::npos,"saved field without live receiver confirmed");
    probe_console::screen=nullptr;
    wchar_t folder[MAX_PATH]{},path[MAX_PATH]{};GetTempPathW(MAX_PATH,folder);GetTempFileNameW(folder,L"r4",0,path);
    HANDLE log=CreateFileW(path,GENERIC_WRITE|GENERIC_READ,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_TEMPORARY,nullptr);
    Check(log!=INVALID_HANDLE_VALUE,"log fixture failed");
    std::ostringstream screen;
    {probe_console::DiagnosticBuffer buffer(log,screen.rdbuf());std::ostream out(&buffer);out<<"[DETECTADO] RVA=0x123\n[STOP] falha real\n";out.flush();}
    Check(screen.str()=="[STOP] falha real\n","technical noise visible or failure hidden");
    SetFilePointer(log,0,nullptr,FILE_BEGIN);char bytes[256]{};DWORD read=0;ReadFile(log,bytes,255,&read,nullptr);
    Check(std::string(bytes,read).find("RVA=0x123")!=std::string::npos,"technical evidence not logged");
    CloseHandle(log);DeleteFileW(path);
    std::cout<<"PASS: clear statuses, receiver/thread checks, incomplete coverage, live memory requirement, quiet console and diagnostic log\n";return 0;
}catch(const std::exception& error){probe_console::screen=nullptr;std::cerr<<error.what()<<'\n';return 1;}}
