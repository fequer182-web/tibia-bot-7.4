#include "../src/RamoniaResearchProbe.cpp"
#include "../../../Crome/Developer/native_runtime/profiles/MiniJson.hpp"
#include <stdexcept>

void check(bool condition,const char* message) {
    if(!condition) throw std::runtime_error(message);
}

int main() {
    try {
        const std::vector<std::string> names{"getId","Id","getTopUseThing","absent"};
        auto* memory=static_cast<uint8_t*>(VirtualAlloc(nullptr,16384,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
        check(memory!=nullptr,"synthetic image allocation failed");
        std::memset(memory,0xF0,16384);
        std::memcpy(memory+4093,"getId",6); // A token crosses the scanner's page boundary.
        std::memcpy(memory+8190,"getTopUseThing",15);
        std::memcpy(memory+12000,"prefixgetId",12); // Original search also accepts a suffix.
        ModuleView image;
        image.base=reinterpret_cast<uintptr_t>(memory); image.sizeOfImage=16384;
        SectionInfo resource; resource.name=".rsrc"; resource.start=image.base;
        resource.end=image.base+16384; resource.characteristics=IMAGE_SCN_MEM_READ;
        image.sections.push_back(resource);
        std::map<std::string,std::vector<uintptr_t>> expected;
        for(const auto& name:names) expected[name]=FindStringAddresses(image,name);
        BuildSemanticStringIndex(image,names);
        for(const auto& name:names) check(FindStringAddresses(image,name)==expected[name],"batch search changed candidates or excluded resources");
        check(image.semanticStringIndex.at("getId").size()==2,"overlap/suffix token missing");
        check(image.semanticStringIndex.count("absent") && image.semanticStringIndex.at("absent").empty(),"missing token not cached");
        VirtualFree(memory,0,MEM_RELEASE);
        guided_discovery::TokenIndex gapIndex({"getId"});
        size_t matches=0;
        gapIndex.feed(reinterpret_cast<const uint8_t*>("get"),3,100,[&](size_t,uintptr_t){++matches;});
        gapIndex.gap();
        gapIndex.feed(reinterpret_cast<const uint8_t*>("Id"),3,200,[&](size_t,uintptr_t){++matches;});
        check(matches==0,"unreadable gap fabricated token");
        guided_discovery::AsciiHints hints("light");
        const std::string a="WorldLi",b="ght\0other",c="LightGap";
        hints.feed(reinterpret_cast<const uint8_t*>(a.data()),a.size());
        const std::array<uint8_t,6> tail{'g','h','t',0,'x',0};
        hints.feed(tail.data(),tail.size());
        check(hints.result()==std::vector<std::string>{"WorldLight"},"chunk boundary lost string");
        hints.feed(reinterpret_cast<const uint8_t*>(c.data()),c.size());
        hints.gap(); hints.finish();
        check(hints.result().size()==1,"unreadable gap fabricated match");

        Report r;
        r.module.timestamp=0x12345678;
        r.module.sizeOfImage=0x10000;
        r.fp.ok=true; r.fp.textHash=0xDEADBEAF;
        auto& bind=r.binds["move"];
        bind.selectedOk=true; bind.selected.methodRva=0x2000;
        bind.selected.ownerRva=0x6000;
        BindCandidate other; other.methodRva=0x3000; other.ownerRva=0x6000;
        bind.candidates={bind.selected,other,other};
        check(guided_discovery::candidates(bind)==std::vector<uint32_t>({0x2000,0x3000}),"alternative lost or duplicated");
        const auto signature=GuidedCandidateSignature(bind);
        bind.candidates[1].score=999;
        check(signature==GuidedCandidateSignature(bind),"score change invalidated identical addresses");
        bind.candidates[1].ownerRva++;
        check(signature!=GuidedCandidateSignature(bind),"owner change retained evidence");
        bind.candidates[1].ownerRva--;

        V33Capture capture{};
        capture.methodRva=0x2000; capture.threadId=42;
        capture.ecx=0xDEADC000; // Historical pointer must never be dereferenced.
        capture.argSnap[0].address=0xDEADB000;
        capture.argSnap[0].valid=48;
        for(size_t i=0;i<48;++i) capture.argSnap[0].bytes[i]=static_cast<uint8_t>(i);
        const auto frozen=GuidedCaptureJson(r,capture);
        auto value=crome::json::Parser(frozen).parse();
        check(value.get("savedPointersAreHistorical")->boolean(),"live pointer claim");
        check(value.get("arguments")->array()[0].get("direct")->get("bytesHex")->string().size()==96,"snapshot bytes truncated");
        std::vector<std::string> snapshots{frozen,"{\"methodRva\":\"0x3000\",\"status\":\"no_hit\"}"};
        const auto evidence=GuidedMethodJson(signature,snapshots,"entry_observations_pending_contract");
        value=crome::json::Parser(evidence).parse();
        check(!value.get("callable")->boolean() && !value.get("contractComplete")->boolean(),"entry enabled ABI");
        check(GuidedSavedObservations(evidence)==snapshots,"nested snapshots lost on resume");
        check(GuidedSavedObservations("{\"observations\":[{\"nested\":{}").empty(),"truncated JSON accepted");
        r.guidedEvidence["move"]=evidence;
        r.guidedEvidence["setOutfit"]=GuidedMethodJson("0x4000:0x0/;",{},"not_tested_pending_contract");
        const auto saved=SaveJson(r);
        check(!saved.empty(),"profile save failed");
        value=crome::json::Parser(ReadWholeFile(saved)).parse();
        check(value.get("semanticBinds")->get("move")->get("candidates")->array().size()==3,"saved candidates lost");
        const auto resumed=ParseProfileFile(saved,r);
        check(resumed.exactBuild && resumed.guidedEvidence.at("move")==evidence,"resume lost guided observations");
        check(resumed.guidedEvidence.count("setOutfit"),"non-baseline method lost on resume");
        std::ostringstream printed;
        auto* previousOutput=std::cout.rdbuf(printed.rdbuf());
        PrintAllBindCandidates(bind);
        std::cout.rdbuf(previousOutput);
        check(printed.str().find("#3")!=std::string::npos && printed.str().find("[DUVIDA]")!=std::string::npos,"candidate display hides alternatives/ambiguity");
        r.fp.textHash++;
        check(!ParseProfileFile(saved,r).exactBuild,"different build inherited captures");
        r.fp.textHash--;
        // Declining the guide preserves an identical current-build observation.
        std::istringstream answers("n\nn\n");
        auto* previous=std::cin.rdbuf(answers.rdbuf());
        RunGuidedDiscoveryValidation(r);
        check(r.guidedEvidence.at("move")==evidence,"skip discarded observations");
        bind.selected.methodRva++;
        RunGuidedDiscoveryValidation(r);
        std::cin.rdbuf(previous);
        check(GuidedSavedObservations(r.guidedEvidence.at("move")).empty(),"changed address retained old observations");
        check(r.localPlayerPtr==0 && r.actionAbi.empty(),"guide requires LP or promotes legacy ABI");
        // Walk the complete missing-method sequence without a LocalPlayer or
        // breakpoints: offer help and preserve explicit discovery prerequisites.
        r.binds.clear(); r.guidedEvidence.clear();
        std::string input="s\n\n";
        for(size_t i=0;i<guided_discovery::methods.size();++i) input+="n\n";
        std::istringstream missingAnswers(input);
        previous=std::cin.rdbuf(missingAnswers.rdbuf());
        RunGuidedDiscoveryValidation(r);
        std::cin.rdbuf(previous);
        check(r.guidedEvidence.size()==guided_discovery::methods.size(),"missing methods disappeared from guide");
        for(const auto& entry:r.guidedEvidence)
            check(JsonStringValue(entry.second,"status")=="discovery_required","missing method promoted");
        std::cout << "Guided discovery: chunk boundaries, candidates, frozen evidence, exact resume and no promotion PASS\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
