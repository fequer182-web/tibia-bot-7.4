#include "../Source/OTCLab_ABIBuilder_V1_3_X64_AUTOCALIBRATOR_CALLER_EVIDENCE.cpp"
#include "../../../../Crome/Developer/native_runtime/profiles/MiniJson.hpp"

static void Check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
struct FakeItem{uintptr_t vtable;uint32_t reserved,id;};
static FakeItem itemA{0x100030,0,100},itemB{0x100030,0,200};
static int mode=0;static std::vector<uintptr_t> attempted;
static bool CaptureFixture(uintptr_t rva,const std::string&,const std::string&,std::vector<Capture>& a,std::vector<Capture>& b,int){
    attempted.push_back(rva);
    if(mode==3||(mode==1&&rva==0x1000))return false;
    Capture first{};first.tid=123;first.rdx=reinterpret_cast<uintptr_t>(&itemA);a.push_back(first);
    if(mode==2)return false;
    first.rdx=reinterpret_cast<uintptr_t>(&itemB);b.push_back(first);return true;
}
static Profile Fixture(){
    Profile p;p.timestamp=0x1234;p.sizeOfImage=0x20000;p.textHash=0x5678;
    p.fnGameLook=0x1000;p.fnGameDetailedLook=0x2000;p.itemIdOffset=offsetof(FakeItem,id);p.itemVtableRva=0x30;
    p.raw=R"({"pipeDiscovery":{"schema":"crome-pipe-discovery-x64-v1","architecture":"x64","fingerprint":{"timestamp":"0x1234","sizeOfImage":"0x20000","textHashFNV1A64":"0x5678"},"functions":{"gameLook":{"rva":"0x1000","status":"unique_bind_candidate","candidateCount":1},"gameDetailedLook":{"rva":"0x2000","status":"unique_bind_candidate","candidateCount":1}}}})";
    return p;
}
int main(){
    try{
        g_base=0x100000;auto p=Fixture();AbiResult result;result.tested=true;
        Check(EstablishLookOracle(p,&result,CaptureFixture),"normal Look not calibrated");
        Check(attempted==std::vector<uintptr_t>{0x1000},"detailedLook preferred over normal Look");
        Check(result.key=="gameLook"&&result.rva==0x1000&&g_lookOracle.rva==0x1000,"normal Look mislabeled or cached wrong RVA");
        Fingerprint fp{};fp.timestamp=p.timestamp;fp.sizeOfImage=p.sizeOfImage;fp.textHash=p.textHash;
        const auto json=crome::json::parse(BuildAbiCatalog(p,fp,{{result.key,result}}));
        Check(json.get("functions")->get("gameLook")->get("status")->string()=="confirmed_runtime_primary_argument","Look evidence saved under wrong key");
        Check(json.get("functions")->get("gameDetailedLook")->get("status")->string()=="abi_validation_required","Look evidence borrowed by detailedLook");
        Check(!json.get("functions")->get("gameLook")->get("callable")->boolean(),"passive capture enabled Look");
        g_lookOracle={};attempted.clear();mode=1;result={};
        Check(EstablishLookOracle(p,&result,CaptureFixture),"separate fallback failed");
        Check(attempted==std::vector<uintptr_t>({0x1000,0x2000})&&result.key=="gameDetailedLook"&&g_lookOracle.rva==0x2000,"fallback mislabeled");
        g_lookOracle={};attempted.clear();mode=2;result={};
        Check(!EstablishLookOracle(p,&result,CaptureFixture)&&!g_lookOracle.ready,"partial capture accepted");
        Check(attempted.size()==1&&result.a.size()==1&&result.b.empty(),"partial samples mixed across functions");
        g_lookOracle={};attempted.clear();mode=3;result={};
        Check(!EstablishLookOracle(p,&result,CaptureFixture)&&!g_lookOracle.ready,"unobserved Look promoted");
        attempted.clear();p.textHash++;
        Check(LookEntries(p).empty()&&!EstablishLookOracle(p,nullptr,CaptureFixture)&&attempted.empty(),"stale discovery captured");
        std::cout<<"PASS: normal Look, separate fallback, correct catalog/cache, partial/stale rejection and passive policy\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
