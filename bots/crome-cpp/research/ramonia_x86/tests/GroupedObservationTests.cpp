#include "../src/RamoniaResearchProbe.cpp"
#include <stdexcept>
static void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
using Fixture=uint32_t(__attribute__((thiscall)) *)(void*);
static Fixture calls[16]{};
static uint32_t receiver=2389;
static volatile LONG running=0;
static DWORD WINAPI FixtureThread(void*) {
    while(InterlockedCompareExchange(&running,0,0))for(auto function:calls)function(&receiver);
    return 0;
}
int main() {
    uint8_t* code=nullptr;HANDLE thread=nullptr;
    try {
        code=static_cast<uint8_t*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
        Check(code!=nullptr,"executable fixture allocation failed");
        ModuleView module;module.base=reinterpret_cast<uintptr_t>(code);module.sizeOfImage=4096;
        SectionInfo text;text.start=module.base;text.end=module.base+4096;text.characteristics=IMAGE_SCN_MEM_EXECUTE|IMAGE_SCN_MEM_READ;module.sections.push_back(text);
        BindResult bind;bind.name="getItems";
        for(unsigned i=0;i<16;++i) {
            const uint32_t rva=32+i*32;code[rva]=0x8B;code[rva+1]=0x01;code[rva+2]=0xC3;
            calls[i]=reinterpret_cast<Fixture>(code+rva);BindCandidate candidate;candidate.methodRva=rva;bind.candidates.push_back(candidate);
        }
        DWORD previous=0;Check(VirtualProtect(code,4096,PAGE_EXECUTE_READ,&previous)!=FALSE,"fixture RX protection failed");
        std::vector<grouped_observation::Site*> active;
        for(auto rva:guided_discovery::candidates(bind)){auto* site=grouped_observation::add(module,rva);Check(site!=nullptr,"candidate rejected");active.push_back(site);}
        Check(active.size()==16&&grouped_observation::pages.size()==1,"candidates truncated or page protection duplicated");
        std::map<uint32_t,grouped_observation::Evidence> evidence;
        Report report;report.module=module;
        for(int phase=0;phase<3;++phase)for(int round=0;round<2;++round) {
            Check(grouped_observation::round(active),"simultaneous candidate arm failed");
            if(phase)for(unsigned i=0;i<16;i+=2)Check(calls[i](&receiver)==2389,"INT3 changed native result or this/stack");
            Check(grouped_observation::pause(active),"observation pause failed");
            for(auto* site:active){auto& row=evidence[site->rva];if(site->ready){++row.hits[phase];row.captures.push_back(grouped_observation::observation(report,site->capture,"bags",phase));}}
        }
        auto json=crome::json::Parser(grouped_observation::resultJson(bind,{},evidence,"bags",{})).parse();
        Check(!json.get("callable")->boolean()&&!json.get("contractComplete")->boolean(),"group hit enabled ABI");
        const auto& rows=json.get("candidateComparison")->array();Check(rows.size()==16,"candidate comparison hid alternatives");
        for(unsigned i=0;i<16;++i) {
            Check(rows[i].get("samplingRoundsWithHit")->array()[0].number()==0,"control phase included an action");
            Check(rows[i].get("status")->string()==(i%2?"not_observed_not_rejected":"observed_both_actions"),"missing hit rejected or promoted candidate");
        }
        // Race the worker's repeated byte restoration/arming against a live
        // natural caller. No TF, manual invocation by the observer, or leak.
        running=1;thread=CreateThread(nullptr,0,FixtureThread,nullptr,0,nullptr);Check(thread!=nullptr,"fixture thread failed");
        for(int round=0;round<40;++round){Check(grouped_observation::round(active),"live round arm failed");Sleep(1);Check(grouped_observation::pause(active),"live round pause failed");}
        InterlockedExchange(&running,0);Check(WaitForSingleObject(thread,3000)==WAIT_OBJECT_0,"fixture caller stalled");CloseHandle(thread);thread=nullptr;
        Check(grouped_observation::stop(),"stop failed");
        MEMORY_BASIC_INFORMATION protection{};VirtualQuery(code,&protection,sizeof(protection));Check(protection.Protect==PAGE_EXECUTE_READ,"shared page original protection not restored");
        Check(grouped_observation::stop(),"repeated stop failed");
        EXCEPTION_RECORD event{};CONTEXT context{};EXCEPTION_POINTERS pointers{&event,&context};
        event.ExceptionCode=EXCEPTION_BREAKPOINT;event.ExceptionAddress=reinterpret_cast<void*>(active[0]->address);context.Eip=active[0]->address+1;
        Check(grouped_observation::handler(&pointers)==EXCEPTION_CONTINUE_EXECUTION&&context.Eip==active[0]->address,"delayed owned trap not handled");
        event.ExceptionAddress=code+8;Check(grouped_observation::handler(&pointers)==EXCEPTION_CONTINUE_SEARCH,"foreign trap swallowed");
        const auto groups=guided_discovery::activities();Check(groups.front().key=="status","basic status not first");
        Check(std::any_of(groups.begin(),groups.end(),[](const auto& group){return group.key=="bags"&&std::find(group.methods.begin(),group.methods.end(),"getItems")!=group.methods.end();}),"container methods not grouped");
        VirtualFree(code,0,MEM_RELEASE);code=nullptr;
        std::cout << "PASS: all 16 candidates together, control/A/B comparison, live rearm race, native this/stack, exact protection restore and no promotion\n";return 0;
    }catch(const std::exception& error){InterlockedExchange(&running,0);if(thread){WaitForSingleObject(thread,3000);CloseHandle(thread);}grouped_observation::stop();std::cerr << error.what() << '\n';return 1;}
}
