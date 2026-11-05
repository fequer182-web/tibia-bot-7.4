#include "../Source/OTCLab_ABIBuilder_V1_3_X64_AUTOCALIBRATOR_CALLER_EVIDENCE.cpp"
#include <cstddef>

static void Check(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);}
struct FixtureThing {uintptr_t vtable;volatile LONG references;uint32_t id;uint8_t padding[64]{};};
struct FixtureTile {uintptr_t vtable;uintptr_t begin,end,capacity;};
static uintptr_t fixtureTag=0x1234;
static volatile uintptr_t fixtureSink=0;
__attribute__((noinline,noclone)) static uintptr_t* FixtureGetTop(FixtureTile* tile,uintptr_t* storage) {
    auto* object=*reinterpret_cast<FixtureThing**>(tile->begin);
    InterlockedIncrement(&object->references);
    storage[0]=reinterpret_cast<uintptr_t>(object);return storage;
}
__attribute__((noinline,noclone)) static void FixtureCaller(FixtureTile* tile) {
    uintptr_t storage[8]{};
    auto* returned=FixtureGetTop(tile,storage);
    auto* object=reinterpret_cast<FixtureThing*>(returned[0]);
    InterlockedDecrement(&object->references);
    fixtureSink=returned[0];
}

int main() {
    try {
        Fingerprint fingerprint;Check(GetFingerprint(fingerprint),"fixture fingerprint unavailable");
        FixtureThing object{reinterpret_cast<uintptr_t>(&fixtureTag),3,2389};
        uintptr_t things[]{reinterpret_cast<uintptr_t>(&object)};
        FixtureTile tile{reinterpret_cast<uintptr_t>(&fixtureTag),reinterpret_cast<uintptr_t>(things),reinterpret_cast<uintptr_t>(things+1),reinterpret_cast<uintptr_t>(things+1)};
        completion_probe::Layout config;config.tileVtable=tile.vtable;config.itemVtable=object.vtable;
        config.beginOffset=offsetof(FixtureTile,begin);config.endOffset=offsetof(FixtureTile,end);config.stride=8;config.rawArray=true;
        const auto function=reinterpret_cast<uintptr_t>(&FixtureGetTop);
        Check(InstallBP(function),"entry calibration installation failed");
        FixtureCaller(&tile);
        const auto captures=Snapshot();UninstallBP();
        Check(captures.size()==1&&captures[0].ret&&captures[0].callSite,"natural caller calibration failed");
        const uintptr_t rva=function-g_base,returnRva=captures[0].ret-g_base;
        Check(completion_probe::start(rva,returnRva,config),"paired probe installation failed");
        FixtureCaller(&tile);
        Check(completion_probe::stop(),"paired probe did not stop");
        const auto returned=completion_probe::state.pair;
        Check(completion_probe::loweredTileReturn(returned),"hidden return or same-tile membership not measured");
        Check(returned.object==reinterpret_cast<uintptr_t>(&object)&&returned.counterCount==1&&returned.counterOffsets[0]==offsetof(FixtureThing,references),"refcount offset was guessed or misidentified");
        Check(object.references==3,"passive probe leaked a reference");
        Check(completion_probe::start(rva,returnRva,config,returned.object,{returned.counterOffsets[0]}),"ownership watch installation failed");
        FixtureCaller(&tile);
        Check(completion_probe::stop(),"ownership probe did not stop");
        const auto watched=completion_probe::state.pair;
        Check(completion_probe::ownershipComplete(watched),"hardware writer increment/release pair not measured");
        Check(watched.incrementIp[0]!=0&&watched.decrementIp[0]!=0&&object.references==3,"ownership writes not captured or reference leaked");
        Check(completion_probe::start(rva,returnRva,config,returned.object,{returned.counterOffsets[0]},1),"active probe installation failed");
        FixtureCaller(&tile);
        Check(completion_probe::stop(),"active probe did not stop");
        const auto active=completion_probe::state.pair;
        Check(completion_probe::activeOwnershipBalanced(active),"active call did not use original caller release");
        Check(object.references==3,"active call duplicated invocation or leaked a reference");
        Check(!completion_probe::ownershipComplete(active),"active post-call snapshot falsely claimed a hardware increment writer");
        completion_probe::Pair directions{};directions.vectorSize=12;uint32_t sequence[]{1,2,3};memcpy(directions.vectorBytes,sequence,sizeof(sequence));
        uint32_t width=0;Check(completion_probe::vectorLayout(directions,3,width)&&width==4,"direction vector width not measured");
        directions.vectorBytes[0]=0xFF;Check(!completion_probe::vectorLayout(directions,3,width),"invalid direction vector accepted");
        completion_probe::Outcome incomplete;incomplete.typed=true;incomplete.returned=true;incomplete.active=true;incomplete.thread=true;incomplete.effect=true;
        Check(!incomplete.complete(),"missing ownership enabled a function");
        incomplete.ownership=true;incomplete.replayOnly=true;Check(!incomplete.complete(),"live argument replay enabled independent calls");
        const auto json=crome::json::parse(completion_probe::pairJson(watched));
        Check(json.get("ownershipWrites")->array()[0].get("offset")->number()==offsetof(FixtureThing,references),"saved ownership offset missing");
        Check(!completion_probe::state.installed,"probe or debug state left installed");
        Check(completion_probe::stop(),"repeated stop wrote to a restored read-only page");
        EXCEPTION_RECORD exception{};CONTEXT context{};EXCEPTION_POINTERS pointers{&exception,&context};
        exception.ExceptionCode=EXCEPTION_BREAKPOINT;exception.ExceptionAddress=reinterpret_cast<void*>(function);
        context.Rip=function+1;
        Check(completion_probe::handler(&pointers)==EXCEPTION_CONTINUE_EXECUTION&&context.Rip==function,"delayed owned INT3 was not handled after restoration");
        exception.ExceptionAddress=reinterpret_cast<void*>(function+2);
        Check(completion_probe::handler(&pointers)==EXCEPTION_CONTINUE_SEARCH,"unrelated breakpoint was swallowed");
        Check(completion_probe::start(rva,returnRva,config)&&completion_probe::stop(),"no-hit cancellation failed");
        Check(completion_probe::start(rva,returnRva,config,returned.object,{returned.counterOffsets[0]},1,0,0,0,GetCurrentThreadId()+1),"thread rejection probe failed");
        FixtureCaller(&tile);Check(completion_probe::stop(),"thread rejection did not stop");
        Check(!completion_probe::state.pair.active&&object.references==3&&std::string(completion_probe::state.pair.stop)=="different_natural_thread","active call ran on a different thread");
        const auto exported=crome::json::parse(completion_probe::outcomeJson(incomplete));
        Check(!exported.get("callable")->boolean(),"completion evidence enabled a missing runtime adapter");
        auto* leaf=static_cast<uint8_t*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
        Check(leaf!=nullptr,"scalar fixture allocation failed");
        const uint8_t leafBytes[]{0x0F,0xB7,0x41,static_cast<uint8_t>(offsetof(FixtureThing,id)),0xC3};
        memcpy(leaf,leafBytes,sizeof(leafBytes));DWORD oldProtect=0;
        Check(VirtualProtect(leaf,4096,PAGE_EXECUTE_READ,&oldProtect)!=FALSE,"scalar fixture protection failed");
        const auto leafAddress=reinterpret_cast<uintptr_t>(leaf);
        Check(completion_probe::scalarLeaf(leafAddress,offsetof(FixtureThing,id),2),"exact scalar leaf rejected");
        Check(!completion_probe::scalarLeaf(leafAddress,offsetof(FixtureThing,id)+4,2)&&!completion_probe::scalarLeaf(leafAddress,offsetof(FixtureThing,id),4),"scalar leaf accepted wrong field/width");
        Check(reinterpret_cast<uintptr_t(*)(uintptr_t)>(leafAddress)(reinterpret_cast<uintptr_t>(&object))==2389,"typed native scalar call mismatch");
        VirtualFree(leaf,0,MEM_RELEASE);
        std::cout << "PASS: real x64 entry/return, hardware refcount writes, active game-thread relay and balanced caller release\n";
        return 0;
    } catch(const std::exception& error) {
        completion_probe::stop();UninstallBP();std::cerr << error.what() << '\n';return 1;
    }
}
