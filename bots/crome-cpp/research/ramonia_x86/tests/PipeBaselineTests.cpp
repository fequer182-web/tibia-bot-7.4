#include "../src/RamoniaResearchProbe.cpp"
#include <stdexcept>

void check(bool condition,const char* message) {
    if(!condition) throw std::runtime_error(message);
}

int main(int argc,char** argv) {
    try {
        check(argc==2,"provide canonical profile path");
        Report r;
        r.module.timestamp=0x6AB03F94;
        r.fp.ok=true;
        r.fp.textHash=0xE281E9CA5BA0AF2ULL;
        auto cache=ParseProfileFile(argv[1],r);
        check(cache.exactBuild,"exact fingerprint must resume");
        check(cache.actionAbiRaw.count("getTopMoveThing"),"resume lost TopMove ABI");
        r.actionAbiRaw=cache.actionAbiRaw;
        const auto original=r.actionAbiRaw.at("getTopMoveThing");
        check(TopMoveContractMatches(original,0x197AD0),"confirmed return contract rejected");
        check(!TopMoveContractMatches(original,0x197AD1),"different bind inherited confirmation");
        auto badOwnership=original;
        const auto ownershipAt=badOwnership.find("increment_and_release_runtime_confirmed");
        check(ownershipAt!=std::string::npos,"missing ownership evidence fixture");
        badOwnership.replace(ownershipAt,std::string("increment_and_release_runtime_confirmed").size(),"pending");
        check(!TopMoveContractMatches(badOwnership,0x197AD0),"unconfirmed ownership promoted");
        auto badStorage=original;
        const auto storageAt=badStorage.find("[ebp+8]");
        check(storageAt!=std::string::npos,"missing hidden storage fixture");
        badStorage.replace(storageAt,7,"[ebp+C]");
        check(!TopMoveContractMatches(badStorage,0x197AD0),"different hidden return ABI promoted");
        const auto& bindJson=FindJsonObject(FindJsonObject(ReadWholeFile(argv[1]),"semanticBinds"),"getTopMoveThing");
        check(HexU32Value(JsonStringValue(bindJson,"methodRva"))==0x197AD0,"evidence/bind mismatch");
        auto& bind=r.binds["getTopMoveThing"];
        bind.selectedOk=true;
        bind.selected.methodRva=0x197AD0;
        RunTopMoveAbiBaseline(r);
        check(r.actionAbiRaw.at("getTopMoveThing")==original,"confirmed contract changed on resume");
        const auto saved=SaveJson(r);
        check(!saved.empty(),"save failed");
        const auto resumed=ParseProfileFile(saved,r);
        check(resumed.actionAbiRaw.at("getTopMoveThing")==original,"save/load lost return/refcount");
        check(resumed.actionAbiRaw.count("move") && resumed.actionAbiRaw.count("useWith"),"existing actions lost");
        r.module.timestamp++;
        check(!ParseProfileFile(argv[1],r).exactBuild,"another timestamp reused profile");
        r.module.timestamp--;
        r.fp.textHash++;
        check(!ParseProfileFile(argv[1],r).exactBuild,"another text hash reused profile");
        r.fp.textHash--;
        bind.selected.methodRva=0x197AD1;
        RunTopMoveAbiBaseline(r);
        check(JsonStringValue(r.actionAbiRaw.at("getTopMoveThing"),"status")=="abi_validation_required","changed bind did not reset ABI");
        check(!TopMoveContractMatches(r.actionAbiRaw.at("getTopMoveThing"),0x197AD1),"pending return promoted");
        bind.selectedOk=false;
        RunTopMoveAbiBaseline(r);
        check(JsonStringValue(r.actionAbiRaw.at("getTopMoveThing"),"status")=="discovery_required","missing bind promoted");
        check(JsonStringValue(FindJsonObject(CurrentPipeBaselineJson(r),"binds"),"getTopMoveThing")=="pending","baseline marked missing bind found");
        std::cout << "Pipe baseline: fingerprint, return ownership, bind invalidation and roundtrip PASS\n";
        return 0;
    } catch(const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
