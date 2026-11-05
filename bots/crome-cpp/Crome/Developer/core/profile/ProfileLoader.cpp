#include "core/profile/ProfileLoader.hpp"
#include <fstream>
#include <stdexcept>
namespace crome {
static uint64_t hx(const nlohmann::json& j, const char* key) {
    if(!j.contains(key) || j[key].is_null()) return 0; if(j[key].is_number_unsigned()) return j[key].get<uint64_t>();
    if(j[key].is_number_integer()) return (uint64_t)j[key].get<long long>();
    std::string s=j[key].get<std::string>(); if(s.empty()) return 0; return std::stoull(s,nullptr,0);
}
static FieldSpec field(const nlohmann::json& j) { FieldSpec f; f.offset=hx(j,"offset"); f.type=j.value("type",""); f.status=parseStatus(j.value("status","missing")); f.confidence=j.value("confidence",0.0); f.method=j.value("method",""); return f; }
static FieldStatus featureStatus(const nlohmann::json& j) { auto s=j.value("status","missing"); if(s.rfind("confirmed",0)==0) return FieldStatus::Confirmed; if(s=="candidate") return FieldStatus::Candidate; if(s=="pending") return FieldStatus::Pending; return FieldStatus::Missing; }
BuildProfile ProfileLoader::load(const std::filesystem::path& path) {
    std::ifstream in(path); if(!in) throw std::runtime_error("profile nao encontrado: "+path.string());
    nlohmann::json j; in>>j; BuildProfile p; p.raw=j; p.schema=j.value("schema",""); p.schemaVersion=j.value("schemaVersion",0);
    p.build.profileName=j.value("profileName",""); p.build.profileStatus=j.value("profileStatus","");
    if(j.contains("capabilities")){ auto& c=j["capabilities"]; p.readOnlyCoreReady=c.value("readOnlyCoreReady",false); p.strictConsumerReady=c.value("strictConsumerReady",false); p.nativeActionsReady=c.value("nativeActionsReady",false); }
    auto b=j.at("build"); p.build.exe=b.value("exe",""); p.build.architecture=b.value("architecture",""); p.build.pointerSize=b.value("pointerSize",0); p.build.rendererHint=b.value("rendererHint","");
    if(b.contains("fingerprint")){ auto f=b["fingerprint"]; p.build.timestamp=hx(f,"timestamp"); p.build.sizeOfImage=hx(f,"sizeOfImage"); p.build.entryPointRva=hx(f,"entryPointRva"); p.build.textRva=hx(f,"textRva"); p.build.textSize=hx(f,"textSize"); p.build.textHash=f.value("textHashFNV1A64",""); }
    auto v=j.at("values");
    if(v.contains("localPlayer")){ auto x=v["localPlayer"]; p.localPlayer.vtableRva=hx(x,"vtableRva"); p.localPlayer.directGlobalRva=hx(x,"directGlobalRva"); p.localPlayer.indirectGlobalRva=hx(x,"indirectGlobalRva"); p.localPlayer.indirectOwnerOffset=hx(x,"indirectOwnerOffset"); if(x.contains("fields")) for(auto& [k,z]:x["fields"].items()) p.localPlayer.fields[k]=field(z); }
    if(v.contains("creature")){ auto x=v["creature"]; if(x.contains("fields")) for(auto& [k,z]:x["fields"].items()) p.creature.fields[k]=field(z); }
    // migratorState e values podem coexistir; migratorState e a ponte de compatibilidade para campos ja confirmados.
    auto m=j.value("migratorState",nlohmann::json::object());
    p.game.rva=hx(m,"gameRva"); p.game.localPlayer=hx(m,"gameLocalPlayer"); p.game.attackTarget=hx(m,"gameAttackTarget"); p.game.followTarget=hx(m,"gameFollowTarget");
    p.map.rva=hx(m,"mapRva"); p.map.centerX=hx(m,"mapCenterX"); p.map.centerY=hx(m,"mapCenterY"); p.map.centerZ=hx(m,"mapCenterZ"); p.map.north=hx(m,"mapNorth"); p.map.east=hx(m,"mapEast"); p.map.south=hx(m,"mapSouth"); p.map.west=hx(m,"mapWest");
    p.map.tileThingsBegin=hx(m,"tileThingsBegin"); p.map.tileThingsEnd=hx(m,"tileThingsEnd"); p.map.tileThingsCap=hx(m,"tileThingsCap"); p.map.tileVtableRva=hx(m,"tileVtableRva"); p.map.thingStorageStride=hx(m,"thingStorageStride"); p.map.thingStorageMode=m.value("thingStorageMode","");
    p.map.tilePathDepth=hx(m,"mapTilePathDepth"); p.map.tilePathMapOffset=hx(m,"mapTilePathMapOffset"); p.map.tilePathChildOffset=hx(m,"mapTilePathChildOffset"); p.map.tilePathHeaderOffset=hx(m,"mapTilePathHeaderOffset"); p.map.tileStorageStride=hx(m,"mapTileStorageStride"); p.map.tileStorageMode=m.value("mapTileStorageMode","");
    p.creature.monsterVtableRva=hx(m,"monsterVtableRva"); p.creature.playerVtableRva=hx(m,"playerVtableRva"); p.creature.npcVtableRva=hx(m,"npcVtableRva");
    p.inventory.baseOffset=hx(m,"inventoryBaseOffset"); p.inventory.slotStride=hx(m,"inventorySlotStride"); p.inventory.storageMode=m.value("inventoryStorageMode",""); p.inventory.itemIdOffset=hx(m,"itemIdOffset"); p.inventory.itemCountOffset=hx(m,"itemCountOffset"); p.inventory.itemCountWidth=hx(m,"itemCountWidth");
    p.containers.gameOffset=hx(m,"gameContainersOffset"); p.containers.mapHeaderOffset=hx(m,"containerMapHeaderOffset"); p.containers.mapSizeOffset=hx(m,"containerMapSizeOffset"); p.containers.nodeLeftOffset=hx(m,"containerNodeLeftOffset"); p.containers.nodeRightOffset=hx(m,"containerNodeRightOffset"); p.containers.nodeNilOffset=hx(m,"containerNodeNilOffset"); p.containers.nodeKeyOffset=hx(m,"containerNodeKeyOffset"); p.containers.nodeValueOffset=hx(m,"containerNodeValueOffset"); p.containers.idOffset=hx(m,"containerIdOffset"); p.containers.capacityOffset=hx(m,"containerCapacityOffset"); p.containers.itemOffset=hx(m,"containerItemOffset"); p.containers.nameOffset=hx(m,"containerNameOffset"); p.containers.hasParentOffset=hx(m,"containerHasParentOffset"); p.containers.closedOffset=hx(m,"containerClosedOffset"); p.containers.unlockedOffset=hx(m,"containerUnlockedOffset"); p.containers.pagesOffset=hx(m,"containerPagesOffset"); p.containers.sizeOffset=hx(m,"containerSizeOffset"); p.containers.firstIndexOffset=hx(m,"containerFirstIndexOffset"); p.containers.dequeMapOffset=hx(m,"containerDequeMapOffset"); p.containers.dequeMapSizeOffset=hx(m,"containerDequeMapSizeOffset"); p.containers.dequeOffsetOffset=hx(m,"containerDequeOffsetOffset"); p.containers.itemCountOffset=hx(m,"containerItemCountOffset"); p.containers.itemElementStride=hx(m,"containerItemElementStride"); p.containers.itemStorageMode=m.value("containerItemStorageMode","");
    if(j.contains("features") && j["features"].is_object()) {
        const auto& fs=j["features"];
        if(fs.contains("look") && fs["look"].is_object()) {
            const auto& x=fs["look"]; auto& f=p.features.look;
            f.status=featureStatus(x); f.architecture=x.value("architecture",""); f.hookFunction=x.value("hookFunction",""); f.hookRva=hx(x,"hookRva"); f.hookArgument=x.value("hookArgument",""); f.thingIndirection=hx(x,"thingIndirection"); f.itemIdOffset=hx(x,"itemIdOffset"); f.itemIdType=x.value("itemIdType","");
            if(x.contains("position") && x["position"].is_object()) { const auto& z=x["position"]; f.positionConfirmed=featureStatus(z)==FieldStatus::Confirmed; f.positionMode=z.value("mode",""); f.positionXOffset=hx(z,"xOffset"); f.positionYOffset=hx(z,"yOffset"); f.positionZOffset=hx(z,"zOffset"); }
            if(x.contains("context") && x["context"].is_object()) { const auto& z=x["context"]; f.contextConfirmed=featureStatus(z)==FieldStatus::Confirmed; f.supportsGroundAndContainer=z.value("supportsGroundAndContainer",false); f.inventoryXSentinel=hx(z,"inventoryXSentinel"); }
        }
        if(fs.contains("hackLight") && fs["hackLight"].is_object()) {
            const auto& x=fs["hackLight"]; auto& f=p.features.hackLight;
            f.status=featureStatus(x); f.architecture=x.value("architecture",""); f.storage=x.value("storage",""); f.intensityOffset=hx(x,"intensityOffset"); f.type=x.value("type",""); f.hackValue=x.value("hackValue",0); f.restorePolicy=x.value("restorePolicy","");
            if(x.contains("observedValues") && x["observedValues"].is_object()) { const auto& z=x["observedValues"]; f.noneValue=z.value("none",0); f.utevoLuxValue=z.value("utevoLux",0); f.utevoGranLuxValue=z.value("utevoGranLux",0); }
            if(x.contains("validation") && x["validation"].is_object()) { const auto& z=x["validation"]; f.visualWriteConfirmed=z.value("visualWriteConfirmed",false); f.restoreConfirmed=z.value("restoreConfirmed",false); }
        }
    }
    const std::pair<const char*,const char*> acts[]={{"gameAutoWalk","fnGameAutoWalk"},{"localPlayerAutoWalk","fnLocalPlayerAutoWalk"},{"gameAttack","fnGameAttack"},{"gameFollow","fnGameFollow"},{"gameLook","fnGameLook"},{"gameDetailedLook","fnGameDetailedLook"},{"gameMove","fnGameMove"},{"gameUse","fnGameUse"},{"gameUseWith","fnGameUseWith"},{"gameTalk","fnGameTalk"},{"mapGetTile","fnMapGetTile"},{"mapGetCreatureById","fnMapGetCreatureById"},{"mapGetCreatureByName","fnMapGetCreatureByName"},{"containerGetItem","fnContainerGetItem"}};
    for(auto [name,key]:acts){ ActionSpec a; a.rva=hx(m,key); a.status=a.rva?FieldStatus::Confirmed:FieldStatus::Missing; std::string sigKey="sig"+std::string(key+2); a.signature=m.value(sigKey,""); p.action.actions[name]=a; }
    return p;
}
}
