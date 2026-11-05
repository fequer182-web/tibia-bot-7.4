#pragma once
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <set>
#include <queue>
#include <string>
#include <vector>

namespace guided_discovery {
struct Activity {
    std::string key,label,a,b;
    std::vector<std::string> methods;
};
inline std::vector<Activity> activities() {
    return {
        {"status","Status / campos basicos","Observe o status parado; abra as janelas de skills/status pelo cliente.","Se possivel, gaste mana ou altere capacity movendo um item; depois observe o status.",{"getHealthPercent","getSpeed","getBaseSpeed","getMagicLevel","getStates","getSoul","getStamina","getSkull","getShield"}},
        {"walk","Andar / mapa","Caminhe por cliques em destinos livres diferentes.","Caminhe em OUTRA direcao e vire o personagem.",{"autoWalk","getTile","getPosition","isWalkable","isPathable","getDirection","setDirection"}},
        {"bags","Bags / containers","Abra e feche uma bag varias vezes.","Abra OUTRA bag e mova itens entre as bags; arraste a janela da bag tambem.",{"getContainers","getContainer","getItems","getItem","getName","getCapacity"}},
        {"items","Itens / inventario / chao","Mova itens entre inventario, bag e chao; varie quantidades.","Use OUTRO item/tile e outra quantidade; de LOOK nos itens.",{"move","getId","getCount","getInventoryItem","getThings","getThing","getThingCount","getTopMoveThing","getTopLookThing","getGround","getName"}},
        {"look","Look / criaturas","De LOOK em itens e criaturas diferentes.","De LOOK em OUTROS objetos e criaturas.",{"look","getId","getName","getCreatureById","getCreatureByName","getHealthPercent"}},
        {"use","Use / ferramentas","Use objetos normalmente pelo cliente.","Use outra ferramenta COM um alvo diferente.",{"use","useWith","getTopUseThing"}},
        {"targets","Attack / follow","Ataque uma criatura e cancele; repita.","Use FOLLOW em OUTRA criatura e cancele; repita.",{"attack","follow","getTopCreature"}},
        {"chat","Chat","Fale ABI_TEST_ALPHA no canal Default.","Fale ABI_TEST_BETA no mesmo canal Default.",{"talkChannel","talk"}},
        {"light","Luz","Altere a opcao de luz pela interface do cliente.","Alterne para OUTRO estado de luz.",{"getAmbientLight","setAmbientLight","getMinimumAmbientLight","setMinimumAmbientLight","setDrawLights","isDrawingLights"}},
        {"outfit","Outfit","Altere o outfit pela interface normal.","Altere para OUTRO outfit.",{"getOutfit","setOutfit"}}
    };
}
// One pass for all exact token+NUL patterns, including overlapping suffixes.
// Completed transitions keep the work per input byte independent of token count.
class TokenIndex {
    struct Node {
        std::array<int,256> next;
        int failure=0;
        std::vector<size_t> outputs;
        Node() { next.fill(-1); }
    };
    std::vector<Node> nodes{1};
    int state=0;
public:
    std::vector<std::string> patterns;
    explicit TokenIndex(const std::vector<std::string>& tokens):patterns(tokens) {
        for(size_t id=0;id<patterns.size();++id) {
            int node=0;
            auto pattern=patterns[id]; pattern.push_back(0);
            for(unsigned char byte:pattern) {
                int next=nodes[node].next[byte];
                if(next<0) {
                    next=static_cast<int>(nodes.size());
                    nodes[node].next[byte]=next;
                    nodes.emplace_back();
                }
                node=next;
            }
            nodes[node].outputs.push_back(id);
        }
        std::queue<int> pending;
        for(int byte=0;byte<256;++byte) {
            const int next=nodes[0].next[byte];
            if(next<0) nodes[0].next[byte]=0;
            else pending.push(next);
        }
        while(!pending.empty()) {
            const int node=pending.front(); pending.pop();
            for(int byte=0;byte<256;++byte) {
                const int next=nodes[node].next[byte];
                const int fallback=nodes[nodes[node].failure].next[byte];
                if(next<0) nodes[node].next[byte]=fallback;
                else {
                    nodes[next].failure=fallback;
                    nodes[next].outputs.insert(nodes[next].outputs.end(),nodes[fallback].outputs.begin(),nodes[fallback].outputs.end());
                    pending.push(next);
                }
            }
        }
    }
    void gap() { state=0; }
    template<class Match> void feed(const uint8_t* bytes,size_t size,uintptr_t address,Match match) {
        for(size_t i=0;i<size;++i) {
            state=nodes[state].next[bytes[i]];
            for(size_t id:nodes[state].outputs) match(id,address+i-patterns[id].size());
        }
    }
};

inline constexpr std::array<const char*,26> methods{
    "look","talkChannel","talk","move","use","useWith","attack","follow","autoWalk",
    "getTile","getCreatureById","getPosition","getThings","getThingCount",
    "getTopMoveThing","getTopUseThing","isWalkable","isPathable","getInventoryItem",
    "getId","getCount","getContainers","getItems","getContainer","getName","getCapacity"
};

// Keep every distinct current-build address. A score is only an ordering hint.
template<class Bind> std::vector<uint32_t> candidates(const Bind& bind) {
    std::vector<uint32_t> result;
    if(bind.selectedOk && bind.selected.methodRva) result.push_back(bind.selected.methodRva);
    for(const auto& candidate:bind.candidates)
        if(candidate.methodRva && std::find(result.begin(),result.end(),candidate.methodRva)==result.end())
            result.push_back(candidate.methodRva);
    return result;
}

inline const char* instruction(const std::string& method) {
    if(method=="look" || method=="getId" || method=="getName")
        return "De LOOK em um item ou criatura. Na segunda amostra, use OUTRO objeto.";
    if(method=="talk" || method=="talkChannel")
        return "Fale ABI_TEST_ALPHA no Default; na segunda amostra, fale ABI_TEST_BETA.";
    if(method=="move" || method=="getTopMoveThing" || method=="getThings" || method=="getThingCount")
        return "Arraste um item do CHAO normalmente. Na segunda amostra, use OUTRO item/tile.";
    if(method=="use" || method=="getTopUseThing")
        return "Use um objeto do CHAO normalmente. Na segunda amostra, use OUTRO objeto/tile.";
    if(method=="useWith") return "Use uma ferramenta/runa COM um alvo; repita com OUTRO alvo na segunda amostra.";
    if(method=="attack") return "Ataque uma criatura e mantenha o target; na segunda amostra, ataque OUTRA.";
    if(method=="follow") return "Use FOLLOW em uma criatura; na segunda amostra, siga OUTRA.";
    if(method=="getContainer" || method=="getContainers" || method=="getItems")
        return "Abra um backpack/container; na segunda amostra, abra OUTRO container.";
    if(method=="getInventoryItem" || method=="getCount")
        return "Mova/equipe um item no inventario; na segunda amostra, use OUTRO item/quantidade.";
    if(method=="autoWalk" || method=="getTile" || method=="getPosition" || method=="isWalkable" || method=="isPathable")
        return "Ande/clique no mapa pelo cliente; na segunda amostra, use OUTRO destino.";
    if(method.find("Light")!=std::string::npos || method.find("Lights")!=std::string::npos)
        return "Altere a opcao de luz pela interface normal do cliente; repita com outro estado.";
    if(method.find("Outfit")!=std::string::npos)
        return "Altere o outfit pela interface normal do cliente; repita com outro outfit.";
    if(method.find("HealthPercent")!=std::string::npos)
        return "Observe uma criatura com HP cheio e depois ferida; faca isso pelo cliente normalmente.";
    if(method.find("Direction")!=std::string::npos)
        return "Vire o personagem normalmente; na segunda amostra, use outra direcao.";
    return "Use a interface normal que consulta/altera este campo. Se nao souber como aciona-lo, pule; o metodo permanece pendente.";
}

// Streaming ASCII hints preserve strings across chunks but never across an
// unreadable gap or section boundary. No per-byte system calls are needed.
struct AsciiHints {
    std::string needle, current;
    std::set<std::string> matches;
    explicit AsciiHints(std::string wanted):needle(std::move(wanted)) {
        for(char& c:needle) c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    void finish() {
        auto lower=current;
        for(char& c:lower) c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if(current.size()>=4 && lower.find(needle)!=std::string::npos) matches.insert(current);
        current.clear();
    }
    void gap() { current.clear(); }
    void feed(const uint8_t* bytes,size_t size) {
        for(size_t i=0;i<size;++i) {
            const auto c=bytes[i];
            if(c>=0x20 && c<=0x7E) { if(current.size()<256) current.push_back(static_cast<char>(c)); }
            else finish();
        }
    }
    std::vector<std::string> result() const {
        std::vector<std::string> out(matches.begin(),matches.end());
        if(out.size()>64) out.resize(64);
        return out;
    }
};
}
