#define main ExternalBuilderEntry
#include "ExternalBuilder.cpp"
#undef main
int main() {
    try {
        if(LoadReferenceProfiles().size()!=3)throw std::runtime_error("Reference profiles were not loaded.");
        BuildInfo build{};build.timestamp=0x123;build.textHash=0x456;
        g_moduleBase=0x100000;g_processPath=u8"C:\\Área de Trabalho\\Antica.exe";
        LocalCandidate lc{};lc.base=0x200000;lc.vtable=0x300000;
        guided::Evidence evidence;evidence.candidates={{0x6C0,"float64"}};evidence.observations={150,130,109};evidence.transitions=2;evidence.reason="two_observed_changes";g_guided["health"]=evidence;
        lc.fieldOffsets["health"]={0x6C0};
        std::string path=SaveProfile(build,lc,PosFields{},PointerPath{},GameDiscovery{},MapCenterDiscovery{}, {}, {});
        std::ifstream input(std::filesystem::u8path(path));std::string text((std::istreambuf_iterator<char>(input)),{});
        if(text.find("confirmed_read_transition")==std::string::npos || text.find("float64")==std::string::npos)throw std::runtime_error("Guided evidence not saved.");
        std::cout<<path<<"\n";return 0;
    }catch(const std::exception& ex){std::cerr<<ex.what()<<"\n";return 1;}
}
