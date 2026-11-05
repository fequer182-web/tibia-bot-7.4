#define main ExternalBuilderEntry
#include "ExternalBuilder.cpp"
#undef main
int main() {
    try {
        if(LoadReferenceProfiles().size()!=3)throw std::runtime_error("Reference profiles were not loaded.");
        auto raw=ReadFileText(ExeDirectory()+"/profiles/exact/Antica.profile.json");
        auto value=[&](const char* key){return std::stoull(ExtractJsonString(raw,key),nullptr,0);};
        BuildInfo exact{};exact.timestamp=static_cast<DWORD>(value("timestamp"));exact.sizeOfImage=static_cast<DWORD>(value("sizeOfImage"));exact.textHash=value("textHashFNV1A64");
        std::vector<uint8_t> module(exact.sizeOfImage),playerMemory(0x1200);g_moduleBase=reinterpret_cast<uintptr_t>(module.data());g_process=GetCurrentProcess();
        uintptr_t local=reinterpret_cast<uintptr_t>(playerMemory.data()),vtable=g_moduleBase+value("vtableRva");std::memcpy(module.data()+value("directGlobalRva"),&local,8);std::memcpy(playerMemory.data(),&vtable,8);
        uint32_t x=32081,y=32183;uint8_t z=7;std::memcpy(playerMemory.data()+value("x"),&x,4);std::memcpy(playerMemory.data()+value("y"),&y,4);std::memcpy(playerMemory.data()+value("z"),&z,1);
        StatsInput seedInput{};seedInput.health=146;seedInput.maxHealth=150;
        if(SeedFromExactProfile(raw,exact,seedInput).size()!=1)throw std::runtime_error("Exact profile did not recover the live pointer.");
        auto wrong=exact;wrong.textHash++;if(!SeedFromExactProfile(raw,wrong,seedInput).empty())throw std::runtime_error("Wrong fingerprint accepted.");
        uintptr_t invalid=0;std::memcpy(playerMemory.data(),&invalid,8);if(!SeedFromExactProfile(raw,exact,seedInput).empty())throw std::runtime_error("Wrong vtable accepted.");g_process=nullptr;
        BuildInfo build{};build.timestamp=0x123;build.textHash=0x456;
        g_moduleBase=0x100000;g_processPath=u8"C:\\Área de Trabalho\\Antica.exe";
        LocalCandidate lc{};lc.base=0x200000;lc.vtable=0x300000;
        guided::Evidence evidence;evidence.candidates={{0x6C0,"float64"}};evidence.observations={150,130,109};evidence.transitions=2;evidence.reason="two_observed_changes";g_guided["health"]=evidence;
        lc.fieldOffsets["health"]={0x6C0};
        g_levelRecords={std::array<double,7>{150,150,0,0,307,0,1},std::array<double,7>{155,155,5,5,317,100,2},std::array<double,7>{160,160,10,10,327,247,3}};
        auto cap=evidence;cap.candidates={{0x700,"float64",100}};cap.observations={307,317,327};g_guided["capacity"]=cap;
        std::string path=SaveProfile(build,lc,PosFields{},PointerPath{},GameDiscovery{},MapCenterDiscovery{}, {}, {});
        std::ifstream input(std::filesystem::u8path(path));std::string text((std::istreambuf_iterator<char>(input)),{});
        if(text.find("confirmed_read_transition")==std::string::npos || text.find("float64")==std::string::npos)throw std::runtime_error("Guided evidence not saved.");
        std::cout<<path<<"\n";return 0;
    }catch(const std::exception& ex){std::cerr<<ex.what()<<"\n";return 1;}
}
