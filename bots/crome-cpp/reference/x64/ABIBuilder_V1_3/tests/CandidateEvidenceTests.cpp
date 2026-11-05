#include "../Source/OTCLab_ABIBuilder_V1_3_X64_AUTOCALIBRATOR_CALLER_EVIDENCE.cpp"
#include "../../../../Crome/Developer/native_runtime/profiles/MiniJson.hpp"

static void Check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int main(){
    try{
        AbiResult r;r.key="gameMove";r.name="Game::move";r.rva=0x1000;r.tested=true;r.status="captured_runtime_ambiguous";
        r.candidates={{"RDX",100,{"sample A/B matched"}},{"*(R8)",110,{"sample A/B matched"}},{"STACK+0x8",20,{"caller alias"}}};
        Capture c{};c.tid=42;c.rdxMem.validBytes=64;memset(c.rdxMem.bytes,0xAB,64);r.a.assign(9,c);r.b.push_back(c);
        const auto result=crome::json::parse(ResultJson(r));
        const auto* review=result.get("candidateReview");
        Check(review->get("needsDisambiguation")->boolean(),"near candidates marked resolved");
        Check(review->get("ranked")->array().size()==3,"weak alternatives discarded");
        Check(review->get("ranked")->array()[0].get("path")->string()=="*(R8)","ranking not deterministic");
        Check(review->get("bestRunnerUpGap")->number()==10,"candidate gap incorrect");
        const auto* snap=result.get("capturesA")->array()[0].get("argSnapshots")->get("RDX");
        Check(snap->get("bytesHex")->string().size()==128,"full argument bytes lost");
        Check(result.get("captureStorage")->get("truncated")->boolean(),"capture truncation hidden");
        Capture historical{};historical.rdx=0xDEAD0000;historical.rdxMem.validBytes=8;
        const uintptr_t historicalValue=0x1234;memcpy(historical.rdxMem.bytes,&historicalValue,8);
        uintptr_t replayed=0;
        Check(FrozenPointerPath(historical,"*(RDX+0x0)",replayed)&&replayed==historicalValue,"frozen wrapper replay failed");
        Check(!FrozenPointerPath(historical,"*(RDX+0x8)",replayed),"missing saved bytes dereferenced live memory");
        Check(!result.get("frozenCandidateReplay")->get("activeValidation")->boolean(),"offline replay treated as active validation");
        Check(!result.get("callable")->boolean()&&!result.get("contractComplete")->boolean(),"ranking enabled function");
        const auto* progress=result.get("validationProgress");
        Check(!progress->get("activeCallValidated")->boolean()&&progress->get("refreshObjectsBeforeActiveTest")->boolean(),"old pointers authorized for active call");
        Profile p;p.timestamp=0x1234;p.sizeOfImage=0x20000;p.textHash=0x5678;
        p.raw=R"({"pipeDiscovery":{"schema":"crome-pipe-discovery-x64-v1","architecture":"x64","fingerprint":{"timestamp":"0x1234","sizeOfImage":"0x20000","textHashFNV1A64":"0x5678"},"functions":{"gameMove":{"rva":"0x1000","status":"unique_bind_candidate","candidateCount":1}}}})";
        const auto prior=std::string(R"({"fingerprint":{"timestamp":"0x1234","sizeOfImage":"0x20000","textHashFNV1A64":"0x5678"},"functions":{"gameMove":{"rva":"0x1000","status":"confirmed_runtime_arguments","callable":false}},"lastEvidenceSession":{"gameMove":)")+ResultJson(r)+"}}";
        ReplaceOrInsertRootObject(p.raw,"abiCatalog",prior);
        Fingerprint fp{};fp.timestamp=p.timestamp;fp.sizeOfImage=p.sizeOfImage;fp.textHash=p.textHash;
        const auto resumed=crome::json::parse(BuildAbiCatalog(p,fp,{}));
        Check(resumed.get("functions")->get("gameMove")->get("status")->string()=="confirmed_runtime_arguments","best proof downgraded");
        Check(resumed.get("lastEvidenceSession")->get("gameMove")->get("evidenceCandidates")->array().size()==3,"skipped test erased inconclusive alternatives");
        p.textHash++;fp.textHash++;
        const auto changed=crome::json::parse(BuildAbiCatalog(p,fp,{}));
        Check(!changed.get("lastEvidenceSession")->get("gameMove"),"foreign-build observations retained");
        std::cout<<"PASS: close candidates, full bytes, explicit truncation, retained inconclusive evidence and build isolation\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
