#pragma once

// All addresses are fresh semantic candidates, not callable ABI declarations.
// One shot per address per sampling round avoids TF/rearming inside the VEH.
namespace grouped_observation {
constexpr LONG capacity=512;
struct Site {
    uintptr_t address=0;uint32_t rva=0;uint8_t original=0;
    volatile LONG armed=0,ready=0;
    V33Capture capture{};
};
struct Page {uintptr_t address=0;DWORD protection=0;};
inline Site sites[capacity]{};
inline volatile LONG count=0,handlers=0,accepting=0;
inline PVOID veh=nullptr;
inline std::vector<Page> pages;
inline SRWLOCK mutation=SRWLOCK_INIT;
struct Lock {Lock(){AcquireSRWLockExclusive(&mutation);}~Lock(){ReleaseSRWLockExclusive(&mutation);}};
struct Scope {Scope(){InterlockedIncrement(&handlers);}~Scope(){InterlockedDecrement(&handlers);}};
inline void byte(Site& site,uint8_t value) {
    *reinterpret_cast<volatile uint8_t*>(site.address)=value;
    FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<void*>(site.address),1);
}
inline void snapshot(const CONTEXT& c,Site& site) {
    auto& out=site.capture;
    out.methodAddress=static_cast<uint32_t>(site.address);out.methodRva=site.rva;out.threadId=GetCurrentThreadId();
    out.eax=c.Eax;out.ebx=c.Ebx;out.ecx=c.Ecx;out.edx=c.Edx;out.esi=c.Esi;out.edi=c.Edi;
    out.ebp=c.Ebp;out.esp=c.Esp;out.eflags=c.EFlags;
    for(size_t i=0;i<8;++i)V33ReadProcessSafe(c.Esp+i*4,&out.stack[i],4);
    V33SnapMemory(c.Ecx,out.ecxSnap);
    for(size_t i=0;i<6;++i) {
        V33SnapMemory(out.stack[i+1],out.argSnap[i]);
        V33SnapMemory(V33SnapDword(out.argSnap[i],0),out.argNested0[i]);
        V33SnapMemory(V33SnapDword(out.argSnap[i],4),out.argNested1[i]);
    }
    out.hit=1;
}
inline LONG CALLBACK handler(EXCEPTION_POINTERS* ep) {
    Scope scope;
    if(!ep||!ep->ExceptionRecord||!ep->ContextRecord||ep->ExceptionRecord->ExceptionCode!=EXCEPTION_BREAKPOINT)
        return EXCEPTION_CONTINUE_SEARCH;
    Lock lock;
    const auto at=reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
    const LONG size=InterlockedCompareExchange(&count,0,0);
    for(LONG i=0;i<size;++i)if(sites[i].address==at) {
        auto& site=sites[i];auto& c=*ep->ContextRecord;
        const LONG claimed=InterlockedCompareExchange(&site.armed,2,1);
        if(claimed==1) {
            byte(site,site.original);c.Eip=static_cast<DWORD>(at);
            if(InterlockedCompareExchange(&accepting,0,0)) {snapshot(c,site);InterlockedExchange(&site.ready,1);}
            InterlockedExchange(&site.armed,0);return EXCEPTION_CONTINUE_EXECUTION;
        }
        uint8_t current=0;
        // Recognize an already restored, delayed owned INT3, never somebody
        // else's breakpoint or an unrelated single-step exception.
        if(V33ReadProcessSafe(at,&current,1)&&current==site.original) {c.Eip=static_cast<DWORD>(at);return EXCEPTION_CONTINUE_EXECUTION;}
        return EXCEPTION_CONTINUE_SEARCH;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
inline bool writable(uintptr_t address) {
    SYSTEM_INFO system{};GetSystemInfo(&system);
    const uintptr_t page=address&~(uintptr_t(system.dwPageSize)-1);
    for(const auto& held:pages)if(held.address==page)return true;
    DWORD original=0;
    if(!VirtualProtect(reinterpret_cast<void*>(page),system.dwPageSize,PAGE_EXECUTE_READWRITE,&original))return false;
    pages.push_back({page,original});return true;
}
inline bool stop() {
    {Lock lock;
        InterlockedExchange(&accepting,0);
        for(LONG i=0;i<count;++i)if(sites[i].armed==1){byte(sites[i],sites[i].original);sites[i].armed=0;}
    }
    for(int i=0;i<100&&handlers;++i)Sleep(10);
    if(handlers)return false;
    for(LONG i=0;i<count;++i)sites[i].armed=0;
    SYSTEM_INFO system{};GetSystemInfo(&system);
    for(const auto& page:pages){DWORD ignored=0;VirtualProtect(reinterpret_cast<void*>(page.address),system.dwPageSize,page.protection,&ignored);}
    pages.clear();return true;
}
inline Site* add(const ModuleView& module,uint32_t rva) {
    Lock lock;
    if(!GuidedExecutableRva(module,rva))return nullptr;
    const auto address=module.base+rva;
    for(LONG i=0;i<count;++i)if(sites[i].address==address) {
        uint8_t current=0;
        return V33ReadProcessSafe(address,&current,1)&&current==sites[i].original&&writable(address)?&sites[i]:nullptr;
    }
    if(count==capacity)return nullptr;
    uint8_t original=0;
    if(!V33ReadProcessSafe(address,&original,1)||original==0xCC||!writable(address))return nullptr;
    auto& site=sites[count];site.address=address;site.rva=rva;site.original=original;
    InterlockedIncrement(&count);return &site;
}
inline bool round(const std::vector<Site*>& active) {
    Lock lock;
    if(!veh)veh=AddVectoredExceptionHandler(1,handler);
    if(!veh)return false;
    InterlockedExchange(&accepting,1);
    for(auto* site:active) {
        site->capture={};site->ready=0;
        InterlockedExchange(&site->armed,1);byte(*site,0xCC);
    }
    return true;
}
inline bool pause(const std::vector<Site*>& active) {
    {Lock lock;
        InterlockedExchange(&accepting,0);
        for(auto* site:active)if(site->armed==1){byte(*site,site->original);site->armed=0;}
    }
    for(int i=0;i<100&&handlers;++i)Sleep(1);
    if(handlers)return false;
    for(auto* site:active)site->armed=0;
    return true;
}
struct Evidence {uint32_t hits[3]{},stored[3]{};std::vector<std::string> captures;};
inline std::string reference(const Report& r) {
    std::ostringstream out;out << "{\"localPlayerAvailable\":" << (r.localPlayerPtr&&r.localPlayerVtableMatch?"true":"false") << ",\"fields\":{";
    size_t i=0;
    if(r.localPlayerPtr&&r.localPlayerVtableMatch)for(const auto& kv:r.selectedFields) {
        double value=0;
        if(kv.second.offset<0||!ReadTypedValue(r.localPlayerPtr,kv.second.type,kv.second.offset,value))continue;
        if(i++)out << ",";
        out << "\"" << JsonEscape(kv.first) << "\":{\"offset\":" << kv.second.offset << ",\"type\":\"" << kv.second.type << "\",\"value\":" << std::setprecision(16) << value << "}";
    }
    out << "},\"position\":{";
    uint32_t x=0,y=0;uint8_t z=0;
    const bool position=r.localPlayerPtr&&r.localPlayerVtableMatch&&QuickCheckPosition(r.localPlayerPtr,r.positionRuntime,&x,&y,&z);
    out << "\"available\":" << (position?"true":"false") << ",\"x\":" << x << ",\"y\":" << y << ",\"z\":" << unsigned(z) << "}}";return out.str();
}
inline std::string observation(const Report& r,const V33Capture& capture,const std::string& activity,int phase) {
    auto json=GuidedCaptureJson(r,capture);json.pop_back();
    json += ",\"activity\":\""+activity+"\",\"phase\":"+std::to_string(phase)+",\"thisMatchesLocalPlayer\":";
    json += r.localPlayerVtableMatch&&capture.ecx==r.localPlayerPtr?"true}":"false}";
    return json;
}
inline bool phase(Report& r,const guided_discovery::Activity& activity,int phaseIndex,unsigned duration,
                  const std::vector<Site*>& active,std::map<uint32_t,Evidence>& evidence,std::string& references) {
    std::cout << "[OBSERVANDO] " << activity.label << " " << (phaseIndex==0?"CONTROLE PARADO":phaseIndex==1?"ACAO A":"ACAO B")
              << " por " << duration << " segundos. Pode ficar no jogo; nao precisa voltar para ENTER.\n" << std::flush;
    probe_console::ui()<<"Observando por "<<duration<<" segundos. Fique no jogo durante esta etapa.\n";
    const auto before=reference(r);const auto end=GetTickCount64()+duration*1000ull;
    unsigned rounds=0;ULONGLONG progress=0;
    while(GetTickCount64()<end) {
        if(!round(active))return false;
        Sleep(200);
        if(!pause(active))return false;
        ++rounds;
        for(auto* site:active)if(site->ready) {
            auto& result=evidence[site->rva];++result.hits[phaseIndex];
            if(result.stored[phaseIndex]++<2)result.captures.push_back(observation(r,site->capture,activity.key,phaseIndex));
            if(phaseIndex&&result.hits[phaseIndex]==1)
                std::cout << "[DETECTADO] " << HexU32(site->rva) << " this=" << V33ClassFromSnap(r,site->capture.ecxSnap) << "\n" << std::flush;
        }
        if(GetTickCount64()>=progress) {
            std::cout << "[PROGRESSO] " << (end-std::min(end,GetTickCount64())+999)/1000 << "s restantes; " << active.size() << " enderecos acompanhados.\n" << std::flush;
            progress=GetTickCount64()+3000;
        }
        probe_console::ui()<<"\r  Tempo restante: "<<(end-std::min(end,GetTickCount64())+999)/1000<<"s       "<<std::flush;
    }
    std::ostringstream refs;refs << "{\"phase\":" << phaseIndex << ",\"rounds\":" << rounds << ",\"before\":" << before << ",\"after\":" << reference(r) << "}";
    references=refs.str();std::cout << "[FASE CONCLUIDA] " << activity.label << "\n";
    probe_console::ui()<<"\r  Etapa concluida.                  \n";return true;
}
inline std::string resultJson(const BindResult& bind,const std::vector<std::string>& previous,
                             const std::map<uint32_t,Evidence>& evidence,const std::string& activity,const std::vector<std::string>& refs) {
    auto observations=previous;bool hit=false;
    for(auto rva:guided_discovery::candidates(bind)) {
        const auto it=evidence.find(rva);
        if(it!=evidence.end()) {observations.insert(observations.end(),it->second.captures.begin(),it->second.captures.end());hit|=it->second.hits[1]||it->second.hits[2];}
    }
    auto json=GuidedMethodJson(GuidedCandidateSignature(bind),observations,hit?"grouped_observations_pending_contract":"grouped_no_hit_pending");json.pop_back();
    std::ostringstream extra;extra << ",\"activity\":\"" << activity << "\",\"samplingIntervalMs\":200,\"maxSnapshotsPerCandidatePerPhase\":2,\"referenceSamples\":[";
    for(size_t i=0;i<refs.size();++i){if(i)extra << ",";extra << refs[i];}
    extra << "],\"candidateComparison\":[";size_t index=0;
    for(auto rva:guided_discovery::candidates(bind)) {
        if(index++)extra << ",";const auto found=evidence.find(rva);
        const Evidence empty{};const auto& row=found==evidence.end()?empty:found->second;
        extra << "{\"rva\":\"" << HexU32(rva) << "\",\"samplingRoundsWithHit\":[" << row.hits[0] << "," << row.hits[1] << "," << row.hits[2]
              << "],\"observedInControl\":" << (row.hits[0]?"true":"false")
              << ",\"status\":\"" << (found==evidence.end()?"breakpoint_unavailable":row.hits[1]&&row.hits[2]?"observed_both_actions":row.hits[1]||row.hits[2]?"observed_one_action":"not_observed_not_rejected") << "\",\"receiverClasses\":[";
        std::set<std::string> classes;
        for(const auto& capture:row.captures){const auto cls=JsonStringValue(capture,"thisClassObserved");if(!cls.empty())classes.insert(cls);}
        size_t classIndex=0;for(const auto& cls:classes){if(classIndex++)extra << ",";extra << "\"" << JsonEscape(cls) << "\"";}
        extra << "]}";
    }
    extra << "]}";return json+extra.str();
}
}

#include "ObservationSummary.hpp"

void RunGuidedDiscoveryValidation(Report& r) {
    observation_summary::sessionRows.clear();observation_summary::skipped.clear();
    observation_summary::basics(r);
    probe_console::ui()<<"\nTestes por atividade: todos os candidatos sao acompanhados juntos.\n"
        <<"CANDIDATO indica uma pista consistente; nao significa ABI liberada.\n";
    std::cout << "\n[SESSOES POR ATIVIDADE R3]\nCalibracao basica primeiro; todos os candidatos do grupo acompanhados juntos.\n"
              << "Cada atividade: 2s parado, 10s acao A, 10s acao B. Nenhuma chamada interna e executada.\n";
    for(auto it=r.guidedEvidence.begin();it!=r.guidedEvidence.end();) {
        const auto bind=r.binds.find(it->first);
        if(bind==r.binds.end()||JsonStringValue(it->second,"candidateSignature")!=GuidedCandidateSignature(bind->second))it=r.guidedEvidence.erase(it);else ++it;
    }
    std::vector<std::string> queue(guided_discovery::methods.begin(),guided_discovery::methods.end());
    for(const auto& bind:r.binds)if(std::find(queue.begin(),queue.end(),bind.first)==queue.end())queue.push_back(bind.first);
    for(const auto& method:queue)if(!r.guidedEvidence.count(method)) {
        const auto bind=r.binds.find(method);
        const bool found=bind!=r.binds.end()&&!guided_discovery::candidates(bind->second).empty();
        r.guidedEvidence[method]=GuidedMethodJson(found?GuidedCandidateSignature(bind->second):"",{},found?"not_tested_pending_contract":"discovery_required");
    }
    SaveCheckpoint(r,"grouped_ready_after_basics");
    if(AskYesNo("Iniciar observacao por atividades?")!='s')return;
    std::vector<std::string> missing;
    for(const auto& method:queue){const auto bind=r.binds.find(method);if(bind==r.binds.end()||guided_discovery::candidates(bind->second).empty())missing.push_back(method);}
    if(!missing.empty()) {
        std::cout << "[SEM CANDIDATO] ";for(const auto& method:missing)std::cout << method << " ";std::cout << "\n";
        probe_console::ui()<<missing.size()<<" funcoes ainda sem candidato; registradas como pendentes.\n";
        if(AskYesNo("Adicionar manualmente uma entrada encontrada no debugger? (opcional)")=='s') {
            probe_console::ui() << "Informe METODO RVA (ex.: getTile 0x1234); linha vazia termina:\n";
            for(std::string line;std::getline(std::cin,line)&&!line.empty();) {
                std::istringstream input(line);std::string method,address,extra;input >> method >> address >> extra;
                uint32_t rva=0;size_t consumed=0;
                try{const auto value=std::stoul(address,&consumed,0);if(consumed==address.size()&&value<=0xFFFFFFFFul)rva=static_cast<uint32_t>(value);}catch(...){}
                if(!extra.empty()||std::find(missing.begin(),missing.end(),method)==missing.end()||!GuidedExecutableRva(r.module,rva)) {std::cout << "[REJEITADO] Metodo/RVA invalido.\n";continue;}
                auto& bind=r.binds[method];bind.name=method;BindCandidate candidate;candidate.methodRva=rva;candidate.ownerSection="user_supplied_unvalidated";bind.candidates.push_back(candidate);
                r.guidedEvidence[method]=GuidedMethodJson(GuidedCandidateSignature(bind),{},"user_supplied_pending_observation");
                std::cout << "[ADICIONADO] " << method << " sera observado junto com o grupo; ABI permanece pendente.\n";
            }
            SaveCheckpoint(r,"grouped_user_candidates");
        }
    }
    std::set<std::string> covered;
    for(const auto& activity:guided_discovery::activities()) {
        std::set<uint32_t> addresses;std::vector<std::string> methods;
        for(const auto& method:activity.methods) {
            const auto bind=r.binds.find(method);if(bind==r.binds.end())continue;
            const auto candidates=guided_discovery::candidates(bind->second);if(candidates.empty())continue;
            methods.push_back(method);covered.insert(method);addresses.insert(candidates.begin(),candidates.end());
        }
        if(addresses.empty())continue;
        std::cout << "\n[ATIVIDADE] " << activity.label << " metodos=" << methods.size() << " candidatos unicos=" << addresses.size() << "\n";
        probe_console::ui()<<"\nTeste: "<<activity.label<<"\n";
        if(AskYesNo("Fazer este teste agora?")!='s'){probe_console::ui()<<"  [NAO TESTADO] "<<activity.label<<"\n";observation_summary::skipped.push_back(activity.label);continue;}
        V33Disarm();std::vector<grouped_observation::Site*> active;std::map<uint32_t,grouped_observation::Evidence> evidence;
        for(auto rva:addresses) {
            auto* site=grouped_observation::add(r.module,rva);
            if(site){active.push_back(site);evidence[rva];}else std::cout << "[PENDENTE] " << HexU32(rva) << " breakpoint indisponivel.\n";
        }
        if(active.empty()){grouped_observation::stop();probe_console::ui()<<"  [NAO TESTADO] Observacao indisponivel neste grupo.\n";continue;}
        std::vector<std::string> refs;bool success=true;
        for(int phase=0;phase<3;++phase) {
            probe_console::ui() << "\n" << (phase==0?"Fique parado, sem abrir/mover itens.":phase==1?activity.a:activity.b) << "\n";
            WaitEnter("ENTER quando estiver pronto; depois acompanhe pelo jogo... ");
            std::string reference;
            success=grouped_observation::phase(r,activity,phase,phase==0?2:10,active,evidence,reference);
            if(!success)break;refs.push_back(reference);
        }
        const bool stopped=grouped_observation::stop();
        for(const auto& method:methods) {
            const auto old=r.guidedEvidence.find(method);
            const auto previous=old==r.guidedEvidence.end()?std::vector<std::string>{}:GuidedSavedObservations(old->second);
            r.guidedEvidence[method]=grouped_observation::resultJson(r.binds.at(method),previous,evidence,activity.key,refs);
        }
        SaveCheckpoint(r,"grouped_"+activity.key);
        std::cout << "[ATIVIDADE CONCLUIDA] " << activity.label << "; comparacao e capturas salvas.\n";
        observation_summary::activity(r,activity,methods,evidence,success&&stopped);
        if(!success||!stopped){std::cout << "[STOP] Observacao interrompida; DLL mantida carregada.\n";return;}
    }
    std::cout << "[PENDENCIAS DE BUSCA/ATIVIDADE]\n";
    for(const auto& method:queue) {
        const auto bind=r.binds.find(method);
        if(bind==r.binds.end()||guided_discovery::candidates(bind->second).empty())std::cout << "  " << method << ": sem candidato\n";
        else if(!covered.count(method))std::cout << "  " << method << ": atividade especifica ainda nao mapeada\n";
    }
    SaveCheckpoint(r,"grouped_sessions_finished");
    std::cout << "[SESSOES CONCLUIDAS] Sem hit nao significa candidato errado. Retorno, ownership e chamada ativa continuam etapas proprias.\n";
}
