#include "../Source/OTCLab_ABIBuilder_V1_3_X64_AUTOCALIBRATOR_CALLER_EVIDENCE.cpp"
#include "../../../../Crome/Developer/native_runtime/profiles/MiniJson.hpp"

static void Check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
static int mode=0;static std::vector<uintptr_t> attempted;
static Capture Message(const char* text){
    Capture c{};c.tid=42;c.rcx=0x100400;c.r9=0xDEAD0000;
    c.r9Mem.base=c.r9;c.r9Mem.validBytes=ARG_SNAP_BYTES;
    memcpy(c.r9Mem.bytes,text,strlen(text)+1);return c;
}
static bool CaptureFixture(uintptr_t rva,const std::string&,const std::string&,std::vector<Capture>& a,std::vector<Capture>& b,int){
    attempted.push_back(rva);
    if(mode==3||(mode==1&&rva==0x2000))return false;
    a.push_back(Message("ABI_TEST_ALPHA"));
    if(mode==2)return true;
    auto second=Message("ABI_TEST_BETA");if(mode==4)second.tid=43;
    b.push_back(second);return true;
}
static Profile Fixture(){
    Profile p;p.timestamp=0x1234;p.sizeOfImage=0x20000;p.textHash=0x5678;p.gameRva=0x400;
    p.raw=R"({"pipeDiscovery":{"schema":"crome-pipe-discovery-x64-v1","architecture":"x64","fingerprint":{"timestamp":"0x1234","sizeOfImage":"0x20000","textHashFNV1A64":"0x5678"},"functions":{"gameTalk":{"rva":"0x1000","status":"unique_bind_candidate","candidateCount":1},"gameTalkChannel":{"rva":"0x2000","status":"unique_bind_candidate","candidateCount":1}}}})";
    return p;
}
int main(){
    try{
        const auto policy=crome::json::parse(pipe_discovery::actionSelectionJson);
        for(const auto& family:pipe_discovery::actionFamilies){
            const auto* keys=policy.get(family.key)->get("candidates");
            Check(keys&&keys->array().size()==family.functionKeys.size(),"Builder/ABI family policy diverged");
            for(size_t i=0;i<family.functionKeys.size();++i)Check(keys->array()[i].string()==family.functionKeys[i],"exported family order differs from ABI");
        }
        g_base=0x100000;auto p=Fixture();auto r=TestTalk(p,CaptureFixture);
        Check(attempted==std::vector<uintptr_t>{0x2000}&&r.key=="gameTalkChannel"&&r.rva==0x2000,"channel method lost or labeled talk");
        Check(r.machine&&r.status=="confirmed_runtime_text_argument","entry string snapshots not matched after temporary expired");
        Fingerprint fp{};fp.timestamp=p.timestamp;fp.sizeOfImage=p.sizeOfImage;fp.textHash=p.textHash;
        const auto catalog=crome::json::parse(BuildAbiCatalog(p,fp,{{r.key,r}}));
        Check(catalog.get("functions")->get("gameTalkChannel")->get("status")->string()==r.status,"channel proof saved under talk key");
        Check(catalog.get("functions")->get("gameTalk")->get("status")->string()=="abi_validation_required","proof borrowed across methods");
        Check(!catalog.get("functions")->get("gameTalkChannel")->get("callable")->boolean(),"channel enabled from partial ABI");
        mode=1;attempted.clear();r=TestTalk(p,CaptureFixture);
        Check(attempted==std::vector<uintptr_t>({0x2000,0x1000})&&r.key=="gameTalk"&&r.machine,"natural Talk fallback failed");
        mode=2;attempted.clear();r=TestTalk(p,CaptureFixture);
        Check(attempted.size()==1&&r.status=="captured_runtime_partial"&&!r.machine,"partial channel mixed with talk");
        mode=3;attempted.clear();r=TestTalk(p,CaptureFixture);
        Check(r.status=="rva_not_observed_runtime"&&!r.machine,"unobserved methods promoted");
        mode=4;attempted.clear();r=TestTalk(p,CaptureFixture);
        Check(!r.machine,"different threads promoted");
        attempted.clear();p.textHash++;r=TestTalk(p,CaptureFixture);
        Check(attempted.empty()&&r.status=="discovery_required","foreign fingerprint captured");
        p=Fixture();p.raw.replace(p.raw.find("unique_bind_candidate"),strlen("unique_bind_candidate"),"ambiguous_bind");
        const auto entries=ActionEntries(p,"talk");Check(entries.size()==1&&entries.front().key=="gameTalkChannel","ambiguous method entered runtime selection");
        std::cout<<"PASS: Talk/TalkChannel runtime selection, short-string snapshots, separate evidence and passive gates\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
