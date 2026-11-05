#pragma once
#include "core/profile/BuildProfile.hpp"
#include <string>
#include <vector>
namespace crome {
struct Capabilities {
    bool playerRead{}, creatureRead{}, inventoryRead{}, containerRead{}, mapRead{};
    bool walk{}, attack{}, follow{}, look{}, move{}, use{}, useWith{}, say{}, containerGetItem{};
    bool lookCapture{}, hackLight{};
    std::vector<std::string> missing;
    static Capabilities from(const BuildProfile& p) {
        Capabilities c;
        auto confirmed=[&](const auto& fields, const char* k){ auto i=fields.find(k); return i!=fields.end() && i->second.status==FieldStatus::Confirmed && i->second.offset!=0; };
        c.playerRead = p.localPlayer.directGlobalRva!=0 && confirmed(p.localPlayer.fields,"health") && confirmed(p.localPlayer.fields,"mana") && confirmed(p.localPlayer.fields,"x");
        c.creatureRead = confirmed(p.creature.fields,"x") && confirmed(p.creature.fields,"y") && confirmed(p.creature.fields,"direction");
        c.inventoryRead = p.inventory.baseOffset!=0 && p.inventory.slotStride!=0 && p.inventory.itemIdOffset!=0;
        c.containerRead = p.containers.gameOffset!=0 && p.containers.itemElementStride!=0;
        c.mapRead = p.map.rva!=0 && p.map.centerX!=0 && p.map.tileThingsBegin!=0;
        auto has=[&](const char* n){ auto i=p.action.actions.find(n); return i!=p.action.actions.end() && i->second.available(); };
        c.walk=has("gameAutoWalk")||has("localPlayerAutoWalk"); c.attack=has("gameAttack"); c.follow=has("gameFollow"); c.look=has("gameLook")||has("gameDetailedLook"); c.move=has("gameMove"); c.use=has("gameUse"); c.useWith=has("gameUseWith"); c.say=has("gameTalk"); c.containerGetItem=has("containerGetItem");
        c.lookCapture=p.features.look.ready(); c.hackLight=p.features.hackLight.ready();
        for (auto [n,v] : std::vector<std::pair<std::string,bool>>{{"player",c.playerRead},{"creatures",c.creatureRead},{"inventory",c.inventoryRead},{"containers",c.containerRead},{"map",c.mapRead},{"walk",c.walk},{"attack",c.attack},{"follow",c.follow},{"say",c.say},{"use",c.use},{"useWith",c.useWith}}) if(!v)c.missing.push_back(n);
        return c;
    }
};
}
