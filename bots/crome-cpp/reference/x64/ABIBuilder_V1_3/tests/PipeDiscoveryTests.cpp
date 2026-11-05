#include "../Source/OTCLab_ABIBuilder_V1_3_X64_AUTOCALIBRATOR_CALLER_EVIDENCE.cpp"
#include "../../../../Crome/Developer/native_runtime/profiles/MiniJson.hpp"
#include <stdexcept>

static void Check(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
static Profile Fixture(const char* status="unique_bind_candidate",const char* count="1") {
    Profile p; p.timestamp=0x1234; p.sizeOfImage=0x20000; p.textHash=0x5678;
    p.raw=std::string(R"({"pipeDiscovery":{"schema":"crome-pipe-discovery-x64-v1","architecture":"x64","fingerprint":{"timestamp":"0x1234","sizeOfImage":"0x20000","textHashFNV1A64":"0x5678"},"functions":{"gameMove":{"rva":"0x1000","status":")")
        +status+R"(","candidateCount":)"+count+R"(}}}})";
    return p;
}
int main() {
    try {
        Check(pipe_discovery::uniqueCandidate({})==0,"empty discovery promoted");
        Check(pipe_discovery::uniqueCandidate({0x100,0x200})==0,"ambiguous discovery promoted");
        Check(pipe_discovery::uniqueCandidate({0x100})==0x100,"unique candidate lost");
        auto p=Fixture();
        Check(DiscoveredRva(p,"gameMove")==0x1000,"current discovery rejected");
        Check(DiscoveredRva(p,"gameUse")==0,"missing method borrowed another RVA");
        Check(DiscoveredRva(Fixture("ambiguous_bind"),"gameMove")==0,"ambiguous bind promoted");
        Check(DiscoveredRva(Fixture("unique_bind_candidate","2"),"gameMove")==0,"nonunique count promoted");
        Check(DiscoveredRva(Fixture("unique_bind_candidate","1.5"),"gameMove")==0,"invalid count promoted");
        p.textHash++;
        Check(!DiscoveryFingerprintMatches(p),"different text hash inherited discovery");
        p=Fixture(); p.timestamp++;
        Check(!DiscoveryFingerprintMatches(p),"different timestamp inherited discovery");
        p=Fixture(); p.sizeOfImage++;
        Check(!DiscoveryFingerprintMatches(p),"different image size inherited discovery");
        p=Fixture();
        Fingerprint fp{}; fp.timestamp=p.timestamp; fp.sizeOfImage=p.sizeOfImage; fp.textHash=p.textHash;
        const auto catalog=crome::json::parse(BuildAbiCatalog(p,fp,{}));
        const auto* functions=catalog.get("functions");
        Check(functions && functions->object().size()==pipe_discovery::methods.size(),"missing default methods");
        for(const auto& [name,obj]:functions->object()) {
            Check(obj.get("callable") && !obj.get("callable")->boolean(),"passive catalog enabled method");
            Check(obj.get("contractComplete") && !obj.get("contractComplete")->boolean(),"partial contract marked complete");
        }
        Check(functions->get("gameMove")->get("status")->string()=="abi_validation_required","discovery treated as ABI proof");
        auto old=Fixture();
        ReplaceOrInsertRootObject(old.raw,"abiCatalog",R"({"fingerprint":{"timestamp":"0x1234","sizeOfImage":"0x20000","textHashFNV1A64":"0x5678"},"functions":{"gameMove":{"rva":"0x1000","status":"confirmed_runtime_arguments","callable":true,"activeCallValidated":true}}})");
        const auto merged=crome::json::parse(BuildAbiCatalog(old,fp,{}));
        const auto* move=merged.get("functions")->get("gameMove");
        Check(move->get("status")->string()=="confirmed_runtime_arguments","same-build evidence lost");
        Check(!move->get("callable")->boolean() && !move->get("activeCallValidated")->boolean(),"old enablement inherited");
        old.raw.replace(old.raw.find("\"rva\":\"0x1000\""),std::string("\"rva\":\"0x1000\"").size(),"\"rva\":\"0x1100\"");
        const auto changed=crome::json::parse(BuildAbiCatalog(old,fp,{}));
        Check(changed.get("functions")->get("gameMove")->get("status")->string()=="abi_validation_required","changed RVA inherited proof");
        const auto legacyPath=fs::temp_directory_path()/("crome_legacy_"+std::to_string(GetCurrentProcessId())+".json");
        WriteText(legacyPath,R"({"migratorState":{"timestamp":"0x1234","sizeOfImage":"0x20000","textHashFNV1A64":"0x5678","fnGameMove":"0x1000"}})");
        const auto legacy=ParseProfile(legacyPath);
        fs::remove(legacyPath);
        Check(!legacy.fnGameMove,"legacy hardcoded RVA accepted without new search");
        auto tileProfile=Fixture();
        auto tileDiscovery=ExtractNamedObject(tileProfile.raw,"pipeDiscovery");
        auto tileFunctions=ExtractNamedObject(tileDiscovery,"functions");
        ReplaceOrInsertRootObject(tileFunctions,"tileGetTopMoveThing",R"({"rva":"0x2000","status":"unique_bind_candidate","candidateCount":1})");
        ReplaceOrInsertRootObject(tileDiscovery,"functions",tileFunctions);
        ReplaceOrInsertRootObject(tileProfile.raw,"pipeDiscovery",tileDiscovery);
        ReplaceOrInsertRootObject(tileProfile.raw,"abiCatalog",R"({"fingerprint":{"timestamp":"0x1234","sizeOfImage":"0x20000","textHashFNV1A64":"0x5678"},"functions":{"tileGetTopMoveThing":{"rva":"0x2000","status":"rva_not_observed_runtime","callable":false}}})");
        AbiResult entry;entry.key="tileGetTopMoveThing";entry.name="Tile::getTopMoveThing";entry.rva=0x2000;entry.tested=true;entry.runtimeHit=true;entry.status="captured_entry_return_pending";
        const auto promoted=crome::json::parse(BuildAbiCatalog(tileProfile,fp,{{entry.key,entry}}));
        const auto* topMove=promoted.get("functions")->get(entry.key);
        Check(topMove->get("status")->string()==entry.status,"observed entry lost to old no-hit status");
        Check(!topMove->get("callable")->boolean()&&!topMove->get("contractComplete")->boolean(),"entry-only proof completed return contract");
        const auto prior=tileProfile.raw.find("rva_not_observed_runtime");
        tileProfile.raw.replace(prior,std::string("rva_not_observed_runtime").size(),"confirmed_runtime_return_contract");
        const auto retained=crome::json::parse(BuildAbiCatalog(tileProfile,fp,{{entry.key,entry}}));
        Check(retained.get("functions")->get(entry.key)->get("status")->string()=="confirmed_runtime_return_contract","entry capture downgraded full proof");
        std::cout<<"PASS: unique current-build discovery, exact fingerprint, passive catalog and stale-proof rejection\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
