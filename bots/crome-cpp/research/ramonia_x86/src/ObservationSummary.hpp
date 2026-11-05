#pragma once

namespace observation_summary {
inline std::string label(const std::string& key) {
    static const std::map<std::string,std::string> labels{
        {"autoWalk","Andar automatico"},{"getPosition","Posicao"},{"getTile","Consulta do mapa"},
        {"isWalkable","Tile onde pode andar"},{"isPathable","Tile para caminho"},
        {"getContainer","Consulta de bag"},{"getContainers","Bags abertas"},{"getItems","Itens da bag"},
        {"getItem","Item da bag"},{"getCapacity","Capacidade da bag"},{"getName","Nome do objeto"},
        {"move","Mover item"},{"getId","ID do objeto"},{"getCount","Quantidade do item"},
        {"getInventoryItem","Item do inventario"},{"getGround","Chao do tile"},{"getThings","Objetos do tile"},
        {"getThing","Objeto do tile"},{"getThingCount","Quantidade de objetos"},{"getTopMoveThing","Bloqueador para mover"},
        {"getTopUseThing","Objeto para usar"},{"getTopLookThing","Objeto para olhar"},
        {"look","Olhar objeto"},{"use","Usar objeto"},{"useWith","Usar ferramenta no alvo"},
        {"getHealthPercent","Vida percentual"},{"getSpeed","Velocidade"},{"getBaseSpeed","Velocidade base"},
        {"getSkull","Skull"},{"getShield","Shield"},{"getDirection","Direcao"},{"setDirection","Virar personagem"},
        {"attack","Ataque"},{"follow","Seguir"},{"talk","Falar"},{"talkChannel","Falar no canal"}
    };
    const auto found=labels.find(key);return found==labels.end()?key:found->second;
}
struct Row {std::string status,message;size_t consistent=0,observed=0;};
inline std::map<std::string,std::vector<std::pair<std::string,Row>>> sessionRows;
inline std::vector<std::string> skipped;
inline bool receiver(const std::string& method,const std::string& activity,const std::string& json) {
    if(method=="autoWalk"||method=="look"||method=="move"||method=="use"||method=="useWith"||method=="attack"||method=="follow"||method=="talk"||method=="talkChannel"||method=="getContainer"||method=="getContainers")
        return json.find("\"thisMatchesGame\":true")!=std::string::npos;
    const auto cls=JsonStringValue(json,"thisClassObserved");
    if(method=="getItems"||method=="getItem"||method=="getCapacity"||(method=="getName"&&activity=="bags"))return cls=="Container";
    if(method=="getCount"||(method=="getId"&&activity=="items"))return cls=="Item";
    if(method=="getPosition"||method=="getDirection"||method=="setDirection")return cls=="LocalPlayer";
    if(method=="getGround"||method=="getThings"||method=="getThing"||method=="getThingCount"||method=="getTopMoveThing"||method=="getTopUseThing"||method=="getTopLookThing"||method=="isWalkable"||method=="isPathable")return cls=="Tile";
    return cls=="LocalPlayer"||cls=="Player"||cls=="Npc"||cls=="Monster";
}
inline Row evaluate(const std::string& method,const std::string& activity,const BindResult& bind,
                    const std::map<uint32_t,grouped_observation::Evidence>& evidence,bool complete) {
    Row row;size_t available=0;
    for(auto rva:guided_discovery::candidates(bind)) {
        const auto found=evidence.find(rva);if(found==evidence.end())continue;++available;
        const auto& data=found->second;
        if(data.hits[1]||data.hits[2])++row.observed;
        if(!data.hits[1]||!data.hits[2])continue;
        bool phaseA=false,phaseB=false;std::set<std::string> threads;
        for(const auto& json:data.captures)if(receiver(method,activity,json)) {
            const auto parsed=crome::json::Parser(json).parse();const auto* phase=parsed.get("phase");const auto* tid=parsed.get("threadId");
            if(phase&&tid&&phase->isNumber()&&tid->isNumber()&&tid->number()>0){phaseA|=phase->number()==1;phaseB|=phase->number()==2;
                if(phase->number()==1||phase->number()==2)threads.insert(std::to_string(tid->number()));}
        }
        if(phaseA&&phaseB&&threads.size()==1)++row.consistent;
    }
    if(!complete){row.status="INCONCLUSIVO";row.message="sessao interrompida";}
    else if(!available){row.status="NAO TESTADO";row.message="observacao indisponivel";}
    else if(available<guided_discovery::candidates(bind).size()){row.status="INCONCLUSIVO";row.message="parte dos candidatos nao pode ser observada; comparacao incompleta";}
    else if(row.consistent==1){row.status="CANDIDATO";row.message="uma funcao consistente nas duas acoes; ABI pendente";}
    else if(row.consistent>1){row.status="INCONCLUSIVO";row.message=std::to_string(row.consistent)+" alternativas consistentes; falta separar";}
    else if(row.observed){row.status="INCONCLUSIVO";row.message="houve chamadas; falta confirmar objeto ou repetir a acao";}
    else{row.status="NAO OBSERVADO";row.message="nenhuma chamada durante as acoes; continua pendente";}
    return row;
}
inline void activity(const Report& r,const guided_discovery::Activity& group,const std::vector<std::string>& methods,
                     const std::map<uint32_t,grouped_observation::Evidence>& evidence,bool complete) {
    probe_console::ui()<<"\nResultado: "<<group.label<<"\n";
    auto& rows=sessionRows[group.label];rows.clear();
    for(const auto& method:methods){const auto row=evaluate(method,group.key,r.binds.at(method),evidence,complete);
        rows.emplace_back(label(method),row);
        probe_console::ui()<<"  ["<<row.status<<"] "<<label(method)<<": "<<row.message<<".\n";}
}
inline void finish() {
    auto& out=probe_console::ui();out<<"\nResumo das atividades\n";
    if(sessionRows.empty())out<<"  Nenhuma atividade foi observada nesta sessao.\n";
    for(const auto& group:sessionRows){size_t pending=0;std::vector<std::string> consistent;
        for(const auto& row:group.second){if(row.second.status=="CANDIDATO")consistent.push_back(row.first);else ++pending;}
        out<<"  "<<group.first<<": ";
        if(consistent.empty())out<<"sem candidato consistente";
        else{out<<"candidatos - ";for(size_t i=0;i<consistent.size();++i){if(i)out<<", ";out<<consistent[i];}}
        if(pending)out<<"; "<<pending<<" pendencia(s)";out<<".\n";
    }
    if(!skipped.empty()){out<<"  Nao testados: ";for(size_t i=0;i<skipped.size();++i){if(i)out<<", ";out<<skipped[i];}out<<".\n";}
}
inline void basics(const Report& r) {
    auto& out=probe_console::ui();out << "\nResultado das memorias basicas\n";
    out << "  [" << (r.localPlayerVtableMatch?"CONFIRMADO":"PENDENTE") << "] Personagem local\n";
    for(const auto& field:std::vector<std::pair<std::string,std::string>>{{"health","Vida atual"},{"maxHealth","Vida maxima"},{"mana","Mana atual"},{"maxMana","Mana maxima"},{"experience","Experiencia"},{"level","Level"},{"capacity","Capacity"}}) {
        double current=0;
        const auto found=r.selectedFields.find(field.first);const bool known=r.localPlayerVtableMatch&&r.localPlayerPtr&&found!=r.selectedFields.end()&&found->second.offset>=0&&!found->second.type.empty()&&found->second.status!="pending"&&ReadTypedValue(r.localPlayerPtr,found->second.type,found->second.offset,current)&&std::isfinite(current);
        const bool confirmed=known&&found->second.status.find("runtime")!=std::string::npos;
        out<<"  ["<<(confirmed?"CONFIRMADO":known?"IDENTIFICADO":"PENDENTE")<<"] "<<field.second<<(known&&!confirmed?" - corresponde ao valor informado":"")<<"\n";
    }
    const bool position=r.localPlayerVtableMatch&&r.localPlayerPtr&&r.positionRuntime.x>=0&&r.positionRuntime.y>=0&&r.positionRuntime.z>=0&&EvidenceScore(r.positionSource)>=90&&QuickCheckPosition(r.localPlayerPtr,r.positionRuntime);
    out<<"  ["<<(position?"CONFIRMADO":"PENDENTE")<<"] Posicao X/Y/Z\n";
    out<<"  ["<<(r.mapCenterOffset>=0&&r.mapCenterStatus.find("runtime")!=std::string::npos?"CONFIRMADO":"PENDENTE")<<"] Centro do mapa\n";
}
}
