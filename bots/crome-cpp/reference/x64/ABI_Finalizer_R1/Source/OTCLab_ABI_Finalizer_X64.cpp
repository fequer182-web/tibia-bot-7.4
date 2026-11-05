// Reuse the tested capture engine; this DLL has its own session and entry point.
#define CROME_ABI_FINALIZER_PRESERVE_RETURN 1
#define CROME_ABI_FINALIZER_CURRENT_CALLER 1
#define DllMain LegacyAbiBuilderDllMain
#include "../../ABIBuilder_V1_3/Source/OTCLab_ABIBuilder_V1_3_X64_AUTOCALIBRATOR_CALLER_EVIDENCE.cpp"
#undef DllMain
#include "FocusedAbiPolicy.hpp"
#include "FingerprintNormalization.hpp"

static uintptr_t g_focusedFingerprintReferenceBase=0;
static bool FocusedFingerprintMatches(const Profile& profile,const Fingerprint& live){
    if(profile.timestamp!=live.timestamp||profile.sizeOfImage!=live.sizeOfImage)return false;
    if(profile.textHash==live.textHash){g_focusedFingerprintReferenceBase=g_base;return true;}
    std::set<uintptr_t> bases;
    const auto priorBase=ParseNum(FlatJsonString(ExtractNamedObject(profile.raw,"abiFinalizer"),"fingerprintReferenceBase"));if(priorBase)bases.insert(priorBase);
    try{const auto functions=crome::json::parse(ExtractNamedObject(ExtractNamedObject(profile.raw,"abiCatalog"),"functions"));
        for(const auto& method:pipe_discovery::methods){const auto* entry=functions.get(method.key);const auto* scope=entry?entry->get("captureScope"):nullptr;const auto* base=scope?scope->get("moduleBase"):nullptr;if(base&&base->isString())bases.insert(ParseNum(base->string()));}
    }catch(...){}
    if(bases.empty()||!live.textSize||live.textSize>g_imageSize||live.textRva>g_imageSize-live.textSize)return false;
    std::vector<uint8_t> original(live.textSize);SIZE_T readBytes{};
    if(!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(g_base+live.textRva),original.data(),original.size(),&readBytes)||readBytes!=original.size())return false;
    IMAGE_DOS_HEADER dos{};if(!SafeRead(g_base,dos)||dos.e_lfanew<=0||static_cast<size_t>(dos.e_lfanew)>g_imageSize-sizeof(IMAGE_NT_HEADERS64))return false;
    IMAGE_NT_HEADERS64 nt{};if(!SafeRead(g_base+dos.e_lfanew,nt))return false;
    const auto reloc=nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if(!reloc.VirtualAddress||reloc.Size<8||reloc.VirtualAddress>g_imageSize||reloc.Size>g_imageSize-reloc.VirtualAddress)return false;
    for(auto reference:bases){if(reference<0x10000)continue;auto bytes=original;size_t cursor=0;bool valid=true;unsigned projected=0;
        while(cursor<reloc.Size){IMAGE_BASE_RELOCATION block{};
            if(reloc.Size-cursor<sizeof(block)||!SafeRead(g_base+reloc.VirtualAddress+cursor,block)||block.SizeOfBlock<8||block.SizeOfBlock>reloc.Size-cursor||(block.SizeOfBlock-8)%2){valid=false;break;}
            for(size_t i=8;i<block.SizeOfBlock;i+=2){uint16_t value{};if(!SafeRead(g_base+reloc.VirtualAddress+cursor+i,value)){valid=false;break;}
                const uint64_t at=uint64_t(block.VirtualAddress)+(value&0xFFF);if(at>UINT32_MAX){valid=false;break;}
                if(!focused_abi::projectRelocation(bytes,live.textRva,static_cast<uint32_t>(at),value>>12,g_base,reference)){valid=false;break;}
                if(value>>12==10&&at>=live.textRva&&at+8<=uint64_t(live.textRva)+bytes.size())++projected;
            }
            if(!valid)break;cursor+=block.SizeOfBlock;
        }
        if(valid&&projected&&Fnv1a64(bytes.data(),bytes.size())==profile.textHash){g_focusedFingerprintReferenceBase=reference;std::cout<<"[FINGERPRINT] Mesmo codigo apos normalizar "<<projected<<" relocacao(oes) ASLR; base de referencia="<<Hex(reference)<<".\n";return true;}
    }
    return false;
}

static bool FocusedGameBinding(const Profile& original,Profile& actionProfile,std::string& report){
    const auto catalog=ExtractNamedObject(original.raw,"abiCatalog");
    if(!CompletionEvidenceMatches(original,catalog))return false;
    std::set<uintptr_t> hints;
    try{
        const auto functions=crome::json::parse(ExtractNamedObject(catalog,"functions"));
        for(const char* key:{"gameAttack","gameFollow","gameLook","gameMove","gameUse","gameUseWith","gameAutoWalk"}){
            const auto* method=functions.get(key);if(!method||!method->isObject())continue;
            const auto* rva=method->get("rva");if(!rva||!rva->isString()||ParseNum(rva->string())!=DiscoveredRva(original,key))continue;
            const auto* scope=method->get("captureScope");const auto* base=scope?scope->get("moduleBase"):nullptr;
            if(!base||!base->isString())continue;const auto oldBase=ParseNum(base->string());
            for(const char* samples:{"capturesA","capturesB"}){const auto* values=method->get(samples);if(!values||!values->isArray())continue;
                for(const auto& capture:values->array()){const auto* rcx=capture.get("RCX");if(!rcx||!rcx->isString())continue;
                    const auto old=ParseNum(rcx->string());if(old>=oldBase&&old-oldBase<g_imageSize)hints.insert(old-oldBase);
                }
            }
        }
    }catch(...){return false;}
    const auto player=ResolveLocalPlayer(original);if(!player)return false;
    std::vector<focused_abi::Binding> good;
    for(auto hint:hints){auto binding=focused_abi::bind(g_imageSize,hint,original.gameRva,original.gameAttackTarget,original.gameFollowTarget);
        if(!binding.valid)continue;unsigned matches=0;
        for(unsigned off=0;off<0x100;off+=8){uintptr_t pointer{};if(SafeRead(g_base+hint+off,pointer)&&pointer==player)++matches;}
        if(matches==1)good.push_back(binding);
    }
    if(good.size()!=1){std::cout<<"[GAME RECEIVER] Dono atual ambiguo/ausente; nao sera ajustado por suposicao.\n";return false;}
    actionProfile=original;actionProfile.gameRva=good[0].receiver;actionProfile.gameAttackTarget=good[0].attack;actionProfile.gameFollowTarget=good[0].follow;
    std::ostringstream out;out<<"{\"status\":\"current_player_membership_checked_capture_hint\",\"stateAnchorRva\":\""<<Hex(original.gameRva)
        <<"\",\"receiverRva\":\""<<Hex(good[0].receiver)<<"\",\"attackOffset\":\""<<Hex(good[0].attack)<<"\",\"followOffset\":\""<<Hex(good[0].follow)<<"\",\"canonicalStateUnchanged\":true}";
    report=out.str();std::cout<<"[GAME RECEIVER] Estado="<<Hex(original.gameRva)<<" dono da chamada="<<Hex(actionProfile.gameRva)<<"; alvos projetados. Novas capturas vao revalidar RCX.\n";
    return true;
}
#include "IndependentAttack.hpp"

static void FocusedSession(){
    std::cout<<"\nOTCLab ABI Finalizer x64 R5 - ataque independente\n"
             <<"Usa o profile e capturas anteriores; objetos e thread sao obtidos novamente.\n"
             <<"Nao repete Status/Item/Look/Luz. Cada chamada ativa exige contrato atual.\n";
    Fingerprint fp{};if(!GetFingerprint(fp)){Wait("Fingerprint indisponivel. ENTER...");return;}
    std::cout<<"[BUILD ATUAL] timestamp="<<Hex(fp.timestamp)<<" size="<<Hex(fp.sizeOfImage)<<" hash mapeado="<<Hex(fp.textHash)<<" base="<<Hex(g_base)<<"\n";
    Fingerprint runtimeFingerprint=fp;
    std::vector<Profile> profiles;for(const auto& path:FindProfiles()){
        const auto filename=path.filename().wstring();if(filename.find(L"OTCLab_BuildProfile_")!=0||filename.find(L".bak")!=std::wstring::npos)continue;
        std::cout<<"[PROFILE CHECK] "<<DisplayPath(path)<<"\n";
        auto p=ParseProfile(path);
        if(!p.timestamp||!p.sizeOfImage){std::cout<<"  REJEITADO: migratorState ausente/invalido.\n";continue;}
        if(!DiscoveryFingerprintMatches(p)){std::cout<<"  REJEITADO: pipeDiscovery nao corresponde ao profile.\n";continue;}
        if(!CompletionEvidenceMatches(p,ExtractNamedObject(p.raw,"abiCatalog"))){std::cout<<"  REJEITADO: abiCatalog ausente ou de outra fingerprint.\n";continue;}
        if(!FocusedFingerprintMatches(p,runtimeFingerprint)){std::cout<<"  REJEITADO: fingerprint do codigo difere, mesmo apos verificar ASLR; esperado="<<Hex(p.textHash)<<".\n";continue;}
        profiles.push_back(p);
    }
    if(profiles.empty()){Wait("[STOP] Nenhum perfil aceito. Veja PROFILE CHECK acima para distinguir arquivo ausente, catalogo invalido ou codigo diferente. ENTER...");return;}
    size_t pick=0;if(profiles.size()>1){for(size_t i=0;i<profiles.size();++i)std::cout<<i<<" "<<DisplayPath(profiles[i].path)<<"\n";pick=AskU32("Escolha o profile atualizado",0);if(pick>=profiles.size())return;}
    auto profile=profiles[pick];if(!FocusedFingerprintMatches(profile,runtimeFingerprint))return;
    fp.textHash=profile.textHash; // Preserve the profile's reference hash across rebases.
    std::cout<<"[PROFILE] "<<DisplayPath(profile.path)<<"\n";ProfileCrossCheck(profile);
    fs::path backup=profile.path;backup+=L".pre_finalizer.bak.json";std::error_code backupError;
    if(!fs::exists(backup,backupError)&&!WriteText(backup,profile.raw)){Wait("Nao foi possivel criar backup. ENTER...");return;}
    Profile actionProfile=profile;std::string receiver="{\"status\":\"unresolved\"}";const bool bound=FocusedGameBinding(profile,actionProfile,receiver);
    std::cout<<"6 ATAQUE INDEPENDENTE (teste novo)\n1 Attack/Follow | 2 autoWalk | 3 Move/Use/UseWith/Talk | 4 TopMove/TopUse e consultas | 5 Todos | 0 Sair\n";
    const auto group=AskU32("Grupo especifico",6);if(!group||group>6)return;
    if(group==6){if(bound)focused_attack::run(profile,actionProfile,fp);else std::cout<<"[STOP] Game receiver nao validou.\n";Wait("ENTER para encerrar...\n");return;}
    std::map<std::string,AbiResult> results;std::map<std::string,std::string> completed,attempts;
    unsigned completeCount=0,partialCount=0;
    std::vector<std::string> keys;
    if(group==1||group==5){keys.push_back("gameAttack");keys.push_back("gameFollow");}
    if(group==2||group==5){keys.push_back("gameAutoWalk");keys.push_back("localPlayerAutoWalk");}
    if(group==3||group==5)for(const char* k:{"gameMove","gameUse","gameUseWith","gameTalkChannel","gameTalk"})keys.emplace_back(k);
    if(group==4||group==5)for(const char* k:{"tileGetTopMoveThing","tileGetTopUseThing","itemGetId","itemGetCount","mapGetTile","containerGetItem","gameGetContainers","containerGetCapacity"})keys.emplace_back(k);
    // Keep previous completion observations from untested methods.
    const auto previous=ExtractNamedObject(profile.raw,"abiCompletion");if(CompletionEvidenceMatches(profile,previous))
        for(const auto& method:pipe_discovery::methods){auto value=ExtractNamedObject(ExtractNamedObject(previous,"functions"),method.key);if(!value.empty())completed[method.key]=value;}
    for(const auto& key:keys){
        if(!AskYes("Concluir "+key+" agora?",true))continue;
        const bool game=key.rfind("game",0)==0&&key!="gameGetContainers";
        const auto& live=game&&bound?actionProfile:profile;
        const auto candidates=CompletionCandidates(live,key);if(candidates.empty()){std::cout<<"[BUSCA PENDENTE] "<<key<<": sem candidato atual.\n";continue;}
        AbiResult current;
        if(key=="gameAttack"||key=="gameFollow"){
            if(!bound){std::cout<<"[PENDENTE] Game receiver nao fechou.\n";continue;}
            current=TestAttackFollow(live,key=="gameAttack");FinalizeCallerEvidence(current);results[key]=current;
        }else if(key=="gameMove"){current=TestMove(live);FinalizeCallerEvidence(current);results[key]=current;}
        else if(key=="gameUse"){current=TestUse(live);FinalizeCallerEvidence(current);results[key]=current;}
        else if(key=="gameUseWith"){current=TestUseWith(live);FinalizeCallerEvidence(current);results[key]=current;}
        else if(key=="gameTalk"||key=="gameTalkChannel"){current=TestTalk(live);FinalizeCallerEvidence(current);if(current.key!=key){results[current.key]=current;std::cout<<"[TALK] Metodo observado="<<current.key<<"; nao valida outro metodo.\n";continue;}results[key]=current;}
        std::vector<completion_probe::Outcome> outcomes;
        for(auto candidate:candidates){
            completion_guidance::explain(key,candidates.size());
            completion_probe::Outcome outcome;outcome.key=key;outcome.rva=candidate;
            auto entry=key=="itemGetId"||key=="itemGetCount"?AbiResult{}:CompletionEntryEvidence(live,results,key,candidate);
            if(key=="itemGetId"||key=="itemGetCount"){
                auto look=CompletionEntryEvidence(live,results,"gameLook",DiscoveredRva(live,"gameLook"));outcome=completion_probe::scalarTests(live,look,key,candidate);
            }
            else if(!entry.naturalReturnRva||!entry.callerSiteValidated)outcome.reason="current_natural_caller_required";
            else if(key=="tileGetTopMoveThing"||key=="tileGetTopUseThing")outcome=completion_probe::tileTests(live,entry);
            else if(key=="gameAutoWalk")outcome=completion_probe::autoWalkTests(live,entry);
            else if(CompletionGameRelay(key)){
                if(entry.a.empty()){CaptureTwo(candidate,completion_guidance::prompt(key,false),completion_guidance::prompt(key,true),entry.a,entry.b,2);entry.naturalThreadId=ConsistentThread(entry.a,entry.b);FinalizeCallerEvidence(entry);}
                outcome=completion_probe::actionTests(live,entry);
            }else{
                for(int stimulus=0;stimulus<2;++stimulus){completion_probe::Pair pair{};completion_probe::take(candidate,entry.naturalReturnRva,completion_probe::layout(live),completion_guidance::prompt(key,stimulus!=0),pair);outcome.observations.push_back(pair);}
                outcome.reason="paired_return_requires_typed_contract";
            }
            if(outcome.complete())++completeCount;else ++partialCount;
            std::cout<<"[RESULTADO] "<<key<<" "<<Hex(candidate)<<" "<<(outcome.complete()?"CONTRATO CONCLUIDO":"PENDENTE")<<": "<<outcome.reason<<"\n";
            if(outcome.active)std::cout<<"[EXECUCAO ATIVA] "<<key<<" chamada=SIM retorno="<<(outcome.returned?"SIM":"NAO")<<" thread="<<(outcome.thread?"SIM":"NAO")<<" efeito="<<(outcome.effect?"SIM":"NAO")<<"; argumentos independentes="<<(outcome.replayOnly?"PENDENTE":"ver contrato")<<".\n";
            outcomes.push_back(std::move(outcome));
        }
        std::ostringstream json;size_t total=0,selected=0;for(size_t i=0;i<outcomes.size();++i)if(outcomes[i].complete()){++total;selected=i;}
        std::ostringstream signature;for(auto rva:candidates)signature<<Hex(rva)<<';';
        json<<"{\"candidateSignature\":\""<<signature.str()<<"\",\"candidateCount\":"<<candidates.size()<<",\"completeCandidates\":"<<total
            <<",\"selectionFinalized\":"<<(outcomes.size()==candidates.size()?"true":"false")<<",\"callable\":false,\"runtimeAdapterReady\":false,\"selectedRva\":\""
            <<Hex(total==1?outcomes[selected].rva:0)<<"\",\"tests\":[";
        for(size_t i=0;i<outcomes.size();++i){if(i)json<<',';json<<completion_probe::outcomeJson(outcomes[i]);}json<<"]}";
        attempts[key]=json.str();
        const auto strength=[](const std::string& text){unsigned score=0;try{const auto root=crome::json::parse(text);const auto* tests=root.get("tests");if(tests&&tests->isArray())for(const auto& test:tests->array())for(const char* field:{"typedArgumentsComplete","returnValidated","ownershipValidated","activeCallValidated","gameThreadDispatchValidated","effectConfirmed","contractComplete"}){const auto* value=test.get(field);if(value&&value->isBool()&&value->boolean())score+=std::string(field)=="contractComplete"?100:1;}}catch(...){}return score;};
        if(!completed.count(key)||strength(json.str())>=strength(completed[key]))completed[key]=json.str();
        else std::cout<<"[PRESERVADO] Evidencia anterior mais forte; nova tentativa fica no relatorio da sessao.\n";
        if(!results.empty())SaveProfile(profile,fp,results);
        if(!SaveCompletionTests(profile,fp,completed)){std::cout<<"[SAVE FAIL] Pare e preserve o backup.\n";return;}
    }
    std::ostringstream handoff;handoff<<"{\"schema\":\"crome-abi-finalizer-report-v1\",\"receiverBinding\":"<<receiver
        <<",\"fingerprintReferenceBase\":\""<<Hex(g_focusedFingerprintReferenceBase)<<"\",\"runtimeMappedTextHash\":\""<<Hex(runtimeFingerprint.textHash)<<"\""
        <<",\"completedInSession\":"<<completeCount<<",\"pendingInSession\":"<<partialCount<<",\"runtimeAdapterReady\":false,\"nativeActionsEnabled\":false,\"relayReturnPolicy\":\"preserve_actual_rax_integer_pointer_path\",\"policy\":\"a_relay_does_not_prove_independent_argument_construction\",\"sessionTests\":{";
    size_t reportIndex=0;for(const auto& attempt:attempts){if(reportIndex++)handoff<<',';handoff<<'"'<<attempt.first<<"\":"<<attempt.second;}handoff<<"}}";
    auto raw=ReadText(profile.path);if(ReplaceOrInsertRootObject(raw,"abiFinalizer",handoff.str()))WriteText(profile.path,raw);
    fs::path report=profile.path;report.replace_filename(L"Crome_ABI_Finalizer_Report.json");WriteText(report,handoff.str());
    std::cout<<"\n[SAVE] Mesmo profile preservado; ABI atualizada. Concluidos="<<completeCount<<" pendentes="<<partialCount<<".\n"
             <<"Chamadas independentes no Crome ainda exigem adaptador para o contrato concluido; replay nao habilita automaticamente.\n";
    Wait("ENTER para encerrar...\n");
}
static DWORD WINAPI FocusedThread(LPVOID){
    OpenStartupLog();bool owns=AllocConsole()!=FALSE;
    HANDLE input=CreateFileW(L"CONIN$",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
    HANDLE output=CreateFileW(L"CONOUT$",GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
    try{if(input!=INVALID_HANDLE_VALUE&&output!=INVALID_HANDLE_VALUE){ScopedConsoleStreams streams(input,output);if(owns){SetConsoleCP(CP_UTF8);SetConsoleOutputCP(CP_UTF8);SetConsoleTitleW(L"OTCLab ABI Finalizer x64 R5");}FocusedSession();}}
    catch(const std::exception& error){LogStage(error.what());}catch(...){LogStage("focused_session_exception");}
    UninstallBP();const bool stopped=completion_probe::stop();
    if(!stopped||g_bp.installed||completion_probe::state.installed)return 0;
    if(input!=INVALID_HANDLE_VALUE)CloseHandle(input);if(output!=INVALID_HANDLE_VALUE)CloseHandle(output);if(owns)FreeConsole();
    if(g_startupLog!=INVALID_HANDLE_VALUE){CloseHandle(g_startupLog);g_startupLog=INVALID_HANDLE_VALUE;}
    if(completion_probe::retainedHandler)return 0;
    FreeLibraryAndExitThread(g_self,0);return 0;
}
#ifndef CROME_ABI_FINALIZER_NO_ENTRY
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,LPVOID){if(reason==DLL_PROCESS_ATTACH){g_self=module;DisableThreadLibraryCalls(module);auto thread=CreateThread(nullptr,0,FocusedThread,nullptr,0,nullptr);if(thread)CloseHandle(thread);}return TRUE;}
#endif
