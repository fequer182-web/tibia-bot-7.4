#pragma once
#include <memory>
namespace focused_attack {
struct Holder{uintptr_t object{},control{};};
struct Context{
    Profile profile;uintptr_t release{},lastObject{},object{},control{};
    LONG before{},pinned{},argument{},afterCall{},afterRelease{};
    bool member{},constructed{},called{},selected{},balanced{};
    std::string reason="not_started";
};
inline uintptr_t number(const std::string& obj,const char* field){return ParseNum(FlatJsonString(obj,field));}
inline bool releaseShape(uintptr_t address){
    uint8_t bytes[80]{};if(!IsExecutable(address)||!completion_probe::read(address,bytes,sizeof(bytes)))return false;
    const uint8_t strong[]={0xF0,0x0F,0xC1,0x41,0x08},weak[]={0xF0,0x0F,0xC1,0x7B,0x0C};
    return std::search(bytes,bytes+80,std::begin(strong),std::end(strong))!=bytes+80&&std::search(bytes,bytes+80,std::begin(weak),std::end(weak))!=bytes+80;
}
inline uintptr_t releaseFromAttack(uintptr_t rva){
    DWORD64 image{};const auto* runtime=RtlLookupFunctionEntry(g_base+rva,&image,nullptr);
    if(!runtime||image!=g_base||runtime->EndAddress<=runtime->BeginAddress||runtime->EndAddress-runtime->BeginAddress>0x4000)return 0;
    std::vector<uint8_t> bytes(runtime->EndAddress-runtime->BeginAddress);
    if(!completion_probe::read(g_base+runtime->BeginAddress,bytes.data(),bytes.size()))return 0;
    std::set<uintptr_t> found;
    // Require the argument holder to be kept in RDI and released from +8.
    const uint8_t moveArg[]={0x48,0x8B,0xFA};if(std::search(bytes.begin(),bytes.begin()+std::min<size_t>(bytes.size(),96),std::begin(moveArg),std::end(moveArg))==bytes.begin()+std::min<size_t>(bytes.size(),96))return 0;
    for(size_t i=0;i+14<=bytes.size();++i){
        const uint8_t prefix[]={0x48,0x8B,0x4F,0x08,0x48,0x85,0xC9,0x74,0x05,0xE8};
        if(std::memcmp(bytes.data()+i,prefix,sizeof(prefix)))continue;
        int32_t relative{};std::memcpy(&relative,bytes.data()+i+10,4);auto target=g_base+runtime->BeginAddress+i+14+relative;
        if(InMainModule(target)&&releaseShape(target))found.insert(target);
    }
    return found.size()==1?*found.begin():0;
}
inline bool worldHolder(const Profile& p,uintptr_t target,Holder& result){
    uint32_t px{},py{},pz{};if(!ReadPosition(p,px,py,pz))return false;
    const auto state=ExtractNamedObject(p.raw,"migratorState");
    const auto resolver=ExtractNamedObject(ExtractNamedObject(p.raw,"resolvers"),"currentFloorTileStorage");
    const auto model=ExtractNamedObject(resolver,"linearModel");
    if(FlatJsonString(model,"status")!="confirmed_from_runtime_observations"||FlatJsonString(state,"mapTileStorageMode")!="shared_ptr_array"||number(state,"mapTileStorageStride")!=16||number(state,"mapTilePathDepth")!=2)return false;
    const auto base=number(model,"base"),stride=number(model,"stride");if(!base||!stride||stride>0x100||base+pz*stride>0x4000)return false;
    uintptr_t parent{},owner{};if(!SafeRead(g_base+number(state,"mapRva")+number(state,"mapTilePathMapOffset"),parent)||!parent||!SafeRead(parent+number(state,"mapTilePathChildOffset"),owner)||!owner)return false;
    uintptr_t begin{},end{};if(!SafeRead(owner+base+pz*stride,begin)||!SafeRead(owner+base+pz*stride+8,end)||!begin||end<begin||(end-begin)%16||(end-begin)/16>4096)return false;
    unsigned matches=0;
    for(auto at=begin;at<end;at+=16){uintptr_t tile{},vt{};if(!SafeRead(at,tile)||!tile||!SafeRead(tile,vt)||vt!=g_base+number(state,"tileVtableRva"))continue;
        uintptr_t first{},last{};if(!SafeRead(tile+number(state,"tileThingsBegin"),first)||!SafeRead(tile+number(state,"tileThingsEnd"),last)||!first||last<first||(last-first)%16||(last-first)/16>64)continue;
        for(auto item=first;item<last;item+=16){Holder h{};if(!SafeRead(item,h)||h.object!=target||!h.control)continue;
            if(!SafeRead(h.object,vt)||vt!=g_base+p.monsterVtableRva)continue;
            result=h;++matches;
        }
    }
    return matches==1;
}
inline bool acquire(uintptr_t control,LONG& after){
    if((control+8)%4)return false;MEMORY_BASIC_INFORMATION info{};
    if(!VirtualQuery(reinterpret_cast<void*>(control+8),&info,sizeof(info))||info.State!=MEM_COMMIT||(info.Protect&PAGE_GUARD))return false;
    auto protect=info.Protect&0xFF;if(protect!=PAGE_READWRITE&&protect!=PAGE_WRITECOPY)return false;
    LONG value{};if(!SafeRead(control+8,value))return false;
    for(unsigned attempt=0;attempt<8&&value>0&&value<1000000;++attempt){
        const auto previous=InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(control+8),value+1,value);
        if(previous==value){after=value+1;return true;}value=previous;
    }
    return false;
}
inline bool execute(completion_probe::Pair& pair,void* opaque){
    auto& c=*static_cast<Context*>(opaque);const auto& p=c.profile;
    uintptr_t target{},current{};
    if(!SnapRead(pair.entry.rdxMem,0,target)||!target||target==c.lastObject){c.reason="look_at_a_different_live_monster";return false;}
    Holder source{};if(!worldHolder(p,target,source)){c.reason="current_world_shared_holder_required";return false;}
    c.member=true;c.object=source.object;c.control=source.control;
    if(!SafeRead(g_base+p.gameRva+p.gameAttackTarget,current)||current){c.reason="cancel_attack_before_arming";return false;}
    if(!SafeRead(source.control+8,c.before)||!acquire(source.control,c.pinned)){c.reason="pin_acquire_failed";return false;}
    using Release=void(*)(uintptr_t);auto release=reinterpret_cast<Release>(c.release);
    struct Pin{Release release;uintptr_t control;~Pin(){if(control)release(control);}} pin{release,source.control};
    Holder fresh=source;
    if(!acquire(source.control,c.argument)){c.reason="argument_acquire_failed";return false;}
    c.constructed=true;
    using Attack=void(*)(uintptr_t,Holder*,bool);
    reinterpret_cast<Attack>(g_base+p.fnGameAttack)(g_base+p.gameRva,&fresh,false);c.called=true;
    Holder selected{};SafeRead(g_base+p.gameRva+p.gameAttackTarget,selected);
    c.selected=selected.object==source.object&&selected.control==source.control;
    const bool readAfter=SafeRead(source.control+8,c.afterCall);
    release(source.control);pin.control=0;
    const bool readFinal=SafeRead(source.control+8,c.afterRelease);
    c.balanced=readAfter&&readFinal&&c.pinned==c.before+1&&c.argument==c.pinned+1&&
        c.afterCall==c.pinned+(c.selected?1:0)&&c.afterRelease==c.afterCall-1;
    c.reason=!c.balanced?"reference_balance_not_confirmed":!c.selected?"native_call_did_not_select_target":"independent_attack_selected_and_balanced";
    return c.selected&&c.balanced;
}
inline std::string resultJson(const Context& c,const completion_probe::Pair& pair,bool visual){
    std::ostringstream out;out<<"{\"status\":\""<<c.reason<<"\",\"object\":\""<<Hex(c.object)<<"\",\"control\":\""<<Hex(c.control)
        <<"\",\"worldMembership\":"<<(c.member?"true":"false")<<",\"independentArgumentConstructed\":"<<(c.constructed?"true":"false")
        <<",\"nativeCallExecuted\":"<<(c.called?"true":"false")<<",\"targetMatched\":"<<(c.selected?"true":"false")<<",\"refcountBalanced\":"<<(c.balanced?"true":"false")
        <<",\"visualEffectConfirmed\":"<<(visual?"true":"false")<<",\"counts\":["<<c.before<<','<<c.pinned<<','<<c.argument<<','<<c.afterCall<<','<<c.afterRelease
        <<"],\"dispatchEvidence\":"<<completion_probe::pairJson(pair)<<"}";return out.str();
}
inline bool run(Profile& profile,const Profile& actions,const Fingerprint& fp){
    const auto attack=CompletionEntryEvidence(actions,{},"gameAttack",actions.fnGameAttack);
    if(!attack.machine){std::cout<<"[STOP] ABI primaria de Attack ainda nao validou.\n";return false;}
    const auto release=releaseFromAttack(actions.fnGameAttack);
    if(!release){std::cout<<"[STOP] Destrutor do argumento std::shared_ptr nao foi reconhecido nesta build. Nenhuma chamada sera feita.\n";return false;}
    const auto look=DiscoveredRva(actions,"gameLook");if(!look)return false;
    std::cout<<"\nATAQUE INDEPENDENTE: o alvo vem do MONSTRO no qual voce der LOOK. O programa relera a colecao do mapa e criara seu proprio argumento.\n";
    std::vector<Capture> a,b;
    if(!CaptureTwo(look,"De LOOK em um MONSTRO vivo A. Nao ataque. ENTER...","De LOOK em OUTRO MONSTRO vivo B. Nao ataque. ENTER...",a,b,2))return false;
    const auto thread=ConsistentThread(a,b);if(!thread){std::cout<<"[STOP] Thread atual nao ficou consistente.\n";return false;}
    AbiResult anchor;anchor.key="gameLook";anchor.rva=look;anchor.a=a;anchor.b=b;FinalizeCallerEvidence(anchor);if(!anchor.naturalReturnRva)return false;
    bool all=true;uintptr_t previous=0;std::ostringstream tests;
    for(int i=0;i<2;++i){
        Wait("PREPARACAO DESLIGADA: cancele ataque/follow. Mantenha um MONSTRO vivo e visivel. ENTER para armar...\n");
        auto owned=std::make_unique<Context>();auto& c=*owned;c.profile=actions;c.release=release;c.lastObject=previous;
        completion_probe::independentCallback=execute;completion_probe::independentContext=&c;
        completion_probe::Pair pair{};
        completion_probe::take(look,anchor.naturalReturnRva,completion_probe::layout(actions),i?"AGORA de LOOK em OUTRO MONSTRO. O ataque sera enviado pelo teste. Nao ataque manualmente. ENTER...":"AGORA de LOOK no MONSTRO A. O ataque sera enviado pelo teste. Nao ataque manualmente. ENTER...",pair,0,{},6,thread);
        if(completion_probe::state.inFlight||completion_probe::handlerCalls||completion_probe::state.installed){
            owned.release(); // Keep callback storage alive if a native handler has not finished.
            std::cout<<"[STOP] Thread ainda ocupado. DLL e contexto retidos; reabra o cliente antes de outro teste.\n";
            return false;
        }
        completion_probe::independentCallback=nullptr;completion_probe::independentContext=nullptr;
        const bool visual=pair.active&&c.selected&&c.balanced&&AskYes("O personagem atacou esse monstro apos SOMENTE dar Look?",false);
        if(i)tests<<',';tests<<resultJson(c,pair,visual);all=all&&visual;previous=c.object;
        std::cout<<"[ATAQUE INDEPENDENTE] "<<c.reason<<" refcount "<<c.before<<" -> "<<c.pinned<<" -> "<<c.argument<<" -> "<<c.afterCall<<" -> "<<c.afterRelease<<" efeito="<<visual<<"\n";
        if(!pair.active)break;
    }
    std::ostringstream result;result<<"{\"schema\":\"crome-independent-attack-x64-v1\",\"timestamp\":\""<<Hex(fp.timestamp)<<"\",\"textHash\":\""<<Hex(fp.textHash)
        <<"\",\"attackRva\":\""<<Hex(actions.fnGameAttack)<<"\",\"receiverRva\":\""<<Hex(actions.gameRva)<<"\",\"releaseRva\":\""<<Hex(release-g_base)
        <<"\",\"strongOffset\":8,\"holderBytes\":16,\"calleeConsumesArgument\":true,\"twoTargetsPassed\":"<<(all?"true":"false")
        <<",\"runtimeAdapterReady\":false,\"callable\":false,\"tests\":["<<tests.str()<<"]}";
    auto raw=ReadText(profile.path);if(ReplaceOrInsertRootObject(raw,"abiIndependentAttack",result.str())&&WriteText(profile.path,raw))profile.raw=raw;
    auto path=profile.path;path.replace_filename(L"Crome_ABI_Independent_Attack_Report.json");WriteText(path,result.str());
    std::cout<<(all?"[PASS] Construcao independente, selecao e referencias passaram em dois alvos.\n":"[PENDING] Veja a causa no relatorio.\n");return all;
}
}
