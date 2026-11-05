#include "core/readers/ProfileBackedPlayerReader.hpp"
namespace crome {
bool ProfileBackedPlayerReader::readNumber(const IMemory& m, uintptr_t a, const std::string& t, double& out){
    if(t=="double"){double v{}; if(!m.readT(a,v))return false;out=v;return true;}
    if(t=="float"){float v{}; if(!m.readT(a,v))return false;out=v;return true;}
    if(t=="u32"){uint32_t v{}; if(!m.readT(a,v))return false;out=v;return true;}
    if(t=="i32"){int32_t v{}; if(!m.readT(a,v))return false;out=v;return true;}
    if(t=="u16"){uint16_t v{}; if(!m.readT(a,v))return false;out=v;return true;}
    if(t=="u8"){uint8_t v{}; if(!m.readT(a,v))return false;out=v;return true;}
    return false;
}
PlayerSnapshot ProfileBackedPlayerReader::read(){
    PlayerSnapshot s; if(!rt_.attached()||!rt_.localPlayer)return s; s.address=rt_.localPlayer;
    const auto& f=rt_.profile->localPlayer.fields;
    auto num=[&](const char* k,double& dst){auto i=f.find(k);return i!=f.end()&&i->second.status==FieldStatus::Confirmed&&readNumber(mem_,rt_.localPlayer+i->second.offset,i->second.type,dst);};
    double x{},y{},z{},dir{}; num("x",x);num("y",y);num("z",z);num("direction",dir); s.position={(int32_t)x,(int32_t)y,(int16_t)z};s.direction=(int)dir;
    num("health",s.health);num("maxHealth",s.maxHealth);num("mana",s.mana);num("maxMana",s.maxMana);num("capacity",s.capacity);num("experience",s.experience);num("level",s.level);num("magicLevel",s.magicLevel);num("soul",s.soul);
    double states{}; if(num("states",states))s.states=(uint64_t)states;
    s.valid=s.maxHealth>0 && s.health>=0 && s.position.x>0 && s.position.y>0; return s;
}
}
