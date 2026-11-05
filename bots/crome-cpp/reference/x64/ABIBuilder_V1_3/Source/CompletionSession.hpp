#pragma once
#include "CompletionProbe.hpp"
#include "CompletionGuidance.hpp"

static std::vector<uintptr_t> CompletionCandidates(const Profile& profile,const std::string& key) {
    std::vector<uintptr_t> values;
    if(!DiscoveryFingerprintMatches(profile)) return values;
    const auto functions=ExtractNamedObject(ExtractNamedObject(profile.raw,"pipeDiscovery"),"functions");
    try {
        const auto function=crome::json::parse(ExtractNamedObject(functions,key));
        const auto* candidates=function.get("candidates");
        if(candidates&&candidates->isArray()) for(const auto& candidate:candidates->array())
            if(candidate.isString()) {
                const auto rva=ParseNum(candidate.string());
                if(rva&&rva<g_imageSize&&IsExecutable(g_base+rva)&&std::find(values.begin(),values.end(),rva)==values.end()) values.push_back(rva);
            }
        const auto* selected=function.get("rva");
        if(values.empty()&&selected&&selected->isString()) {
            const auto rva=ParseNum(selected->string());if(rva&&rva<g_imageSize&&IsExecutable(g_base+rva))values.push_back(rva);
        }
    } catch(...) {}
    return values;
}

static bool CompletionEvidenceMatches(const Profile& profile,const std::string& json) {
    const auto fingerprint=ExtractNamedObject(json,"fingerprint");
    return DiscoveryFingerprintMatches(profile)&&pipe_discovery::exactFingerprint(profile.timestamp,profile.sizeOfImage,profile.textHash,
        ParseNum(FlatJsonString(fingerprint,"timestamp")),ParseNum(FlatJsonString(fingerprint,"sizeOfImage")),ParseNum(FlatJsonString(fingerprint,"textHashFNV1A64")));
}

static AbiResult CompletionEntryEvidence(const Profile& profile,const std::map<std::string,AbiResult>& current,const std::string& key,uintptr_t candidate) {
    const auto found=current.find(key);
    if(found!=current.end()&&found->second.rva==candidate&&found->second.callerSiteValidated) return found->second;
    AbiResult result;result.key=key;result.name=key;result.rva=candidate;
    const auto existing=ExistingFunctionObject(profile.raw,key);
    const auto catalog=ExtractNamedObject(profile.raw,"abiCatalog");
    if(CompletionEvidenceMatches(profile,catalog)&&ParseNum(FlatJsonString(existing,"rva"))==candidate) {
        result.naturalReturnRva=ParseNum(FlatJsonString(existing,"naturalReturnRva"));
        result.naturalCallSiteRva=ParseNum(FlatJsonString(existing,"naturalCallSiteRva"));
        try {
            const auto json=crome::json::parse(existing);
            const auto* machine=json.get("machineAbiValidated");result.machine=machine&&machine->isBool()&&machine->boolean();
            const auto* caller=json.get("callerSiteValidated");result.callerSiteValidated=caller&&caller->isBool()&&caller->boolean();
        }catch(...){}
    }
    // Saved caller RVAs are only discovery hints. The paired probe requires
    // this current invocation to return to that site with exactly RSP+8.
    if(result.naturalReturnRva&&result.callerSiteValidated&&IsExecutable(g_base+result.naturalReturnRva))return result;
    std::cout << "[CALLER PENDENTE] " << key << " " << Hex(candidate) << ": duas acoes naturais antes do teste de retorno.\n";
    if(CaptureTwo(candidate,completion_guidance::prompt(key,false),completion_guidance::prompt(key,true),result.a,result.b,2)) {
        result.tested=true;result.runtimeHit=true;result.naturalThreadId=ConsistentThread(result.a,result.b);FinalizeCallerEvidence(result);
    }
    return result;
}

static bool SaveCompletionTests(Profile& profile,const Fingerprint& fp,const std::map<std::string,std::string>& methods) {
    std::ostringstream json;
    json << "{\"schema\":\"crome-abi-completion-x64-v1\",\"fingerprint\":{\"timestamp\":\"" << Hex(fp.timestamp)
         << "\",\"sizeOfImage\":\"" << Hex(fp.sizeOfImage) << "\",\"textHashFNV1A64\":\"" << Hex(fp.textHash)
         << "\"},\"functions\":{";
    size_t index=0;for(const auto& method:methods) {if(index++)json << ",";json << "\"" << method.first << "\":" << method.second;}
    json << "},\"policy\":\"current_objects_only_no_cross_build_or_historical_pointer_calls\"}";
    auto raw=ReadText(profile.path);if(raw.empty())return false;
    if(!ReplaceOrInsertRootObject(raw,"abiCompletion",json.str())||!WriteText(profile.path,raw))return false;
    profile.raw=raw;return true;
}

static bool CompletionGameRelay(const std::string& key) {
    return key=="gameAttack"||key=="gameFollow"||key=="gameLook"||key=="gameMove"||key=="gameUse"||key=="gameUseWith"||key=="gameTalk"||key=="gameTalkChannel";
}

static void RunCompletionTests(Profile& profile,const Fingerprint& fp,const std::map<std::string,AbiResult>& current) {
    std::cout << "\n[ABI COMPLETION X64 V1]\n"
              << "Novos testes: retorno pareado, refcount por escrita observada, vetor autoWalk e chamadas ativas no thread natural.\n"
              << "Cada etapa exige seus proprios dados; um replay de argumentos atuais nao prova a construcao de argumentos independentes.\n";
    if(!AskYes("Executar os testes de conclusao pendentes agora?",true))return;
    std::map<std::string,std::string> saved;
    const auto previous=ExtractNamedObject(profile.raw,"abiCompletion");
    if(CompletionEvidenceMatches(profile,previous)) {
        const auto functions=ExtractNamedObject(previous,"functions");
        for(const auto& method:pipe_discovery::methods) {
            const auto object=ExtractNamedObject(functions,method.key);
            // Results must belong to exactly the current candidate set too.
            std::ostringstream signature;
            for(auto rva:CompletionCandidates(profile,method.key))signature << Hex(rva) << ";";
            if(!object.empty()&&FlatJsonString(object,"candidateSignature")==signature.str()) saved[method.key]=object;
        }
    }
    std::vector<std::string> order{"tileGetTopMoveThing","tileGetTopUseThing","gameAutoWalk","itemGetId","itemGetCount"};
    for(const auto& method:pipe_discovery::methods)
        if(std::find(order.begin(),order.end(),method.key)==order.end()) order.emplace_back(method.key);
    for(const auto& key:order) {
        if(completion_probe::state.inFlight) {std::cout << "[STOP] Captura ainda em voo; nenhum outro teste sera armado.\n";break;}
        const auto candidates=CompletionCandidates(profile,key);
        std::cout << "\n[PENDENTE] " << key << " candidatos=" << candidates.size() << "\n";
        for(auto rva:candidates) std::cout << "  " << Hex(rva) << "\n";
        if(candidates.empty()) {std::cout << "[BUSCA NECESSARIA] Nenhum candidato atual.\n";continue;}
        const auto known=saved.find(key);
        if(known!=saved.end())try {
            const auto previousResult=crome::json::parse(known->second);
            const auto* finalized=previousResult.get("selectionFinalized");
            const auto* complete=previousResult.get("completeCandidates");
            if(finalized&&finalized->isBool()&&finalized->boolean()&&complete&&complete->isNumber()&&complete->number()==1) {
                std::cout << "[RETOMADA] Contrato concluido salvo para a mesma fingerprint e candidatos. Adaptador runtime ainda pendente.\n";
                if(!AskYes("Revalidar "+key+" nesta sessao?",false))continue;
            }
        }catch(...){}
        const bool priority=key=="tileGetTopMoveThing"||key=="tileGetTopUseThing"||key=="gameAutoWalk"||key=="itemGetId"||key=="itemGetCount";
        completion_guidance::explain(key,candidates.size());
        if(!AskYes("Fazer este teste de conclusao agora?",priority))continue;
        std::vector<completion_probe::Outcome> tests;
        for(uintptr_t candidate:candidates) {
            if(candidates.size()>1&&!AskYes("Testar candidato "+Hex(candidate)+"?",true))continue;
            completion_probe::Outcome result;result.key=key;result.rva=candidate;
            if(key=="itemGetId"||key=="itemGetCount") {
                const auto anchor=CompletionEntryEvidence(profile,current,"gameLook",DiscoveredRva(profile,"gameLook"));
                result=completion_probe::scalarTests(profile,anchor,key,candidate);
            } else {
                auto entry=CompletionEntryEvidence(profile,current,key,candidate);
                if(!entry.naturalReturnRva || !entry.callerSiteValidated) result.reason="current_natural_caller_required";
                else if(key=="tileGetTopMoveThing"||key=="tileGetTopUseThing")result=completion_probe::tileTests(profile,entry);
                else if(key=="gameAutoWalk") result=completion_probe::autoWalkTests(profile,entry);
                else if(CompletionGameRelay(key)) {
                    // Refresh the thread when reusing historical ABI/caller facts.
                    if(entry.a.empty()) {
                        CaptureTwo(candidate,completion_guidance::prompt(key,false),completion_guidance::prompt(key,true),entry.a,entry.b,2);
                        entry.naturalThreadId=ConsistentThread(entry.a,entry.b);FinalizeCallerEvidence(entry);
                    }
                    result=completion_probe::actionTests(profile,entry);
                } else {
                    const auto config=completion_probe::layout(profile);
                    for(int stimulus=0;stimulus<2;++stimulus) {
                        completion_probe::Pair pair{};
                        completion_probe::take(candidate,entry.naturalReturnRva,config,completion_guidance::prompt(key,stimulus!=0),pair);
                        result.observations.push_back(pair);
                    }
                    result.reason="paired_return_recorded_typed_receiver_and_ownership_pending";
                    // RAX alone does not identify the receiver, a borrowed
                    // vector, Position, scalar or owned pointer return contract.
                }
            }
            std::cout << "[CONCLUSAO] " << key << " " << Hex(candidate) << " " << result.reason
                      << " contractComplete=" << result.complete() << " callable=false\n";
            tests.push_back(result);
            std::ostringstream json;size_t complete=0,selected=0;
            for(size_t i=0;i<tests.size();++i)if(tests[i].complete()){++complete;selected=i;}
            std::ostringstream signature;for(auto rva:candidates)signature << Hex(rva) << ";";
            const bool finalized=tests.size()==candidates.size();
            json << "{\"candidateSignature\":\"" << signature.str() << "\",\"candidateCount\":" << candidates.size() << ",\"completeCandidates\":" << complete
                 << ",\"selectionFinalized\":" << (finalized?"true":"false")
                 << ",\"callable\":false,\"runtimeAdapterReady\":false,\"selectedRva\":\"" << Hex(finalized&&complete==1?tests[selected].rva:0)
                 << "\",\"tests\":[";
            for(size_t i=0;i<tests.size();++i){if(i)json << ",";json << completion_probe::outcomeJson(tests[i]);}
            json << "]}";saved[key]=json.str();
            if(!SaveCompletionTests(profile,fp,saved)) {std::cout << "[SAVE FAIL] Conclusao nao foi salva.\n";return;}
        }
    }
    std::cout << "\n[COMPLETION SAVE] Evidencias e pendencias gravadas em abiCompletion no mesmo profile.\n";
}
