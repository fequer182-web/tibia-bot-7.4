#define main BuilderEntry
#include "../../outputs/Cromin_Builder_Padrao/Builder/Source/ExternalBuilder.cpp"
#undef main
int main(int argc,char** argv) {
    try {
        if(argc!=2)throw std::runtime_error("Pass profile path");
        auto raw=ReadFileText(argv[1]);ImportEvidence(raw);
        if(g_guided["health"].status()!="confirmed_read_transition" || g_guided["level"].transitions!=2 || g_guided["experience"].candidates[0].offset!=0x6E0)throw std::runtime_error("Confirmed fields lost on resume");
        auto capacity=g_guided["capacity"];if(capacity.candidates.size()!=2 || capacity.transitions!=3 || capacity.candidates[0].displayScale!=100)throw std::runtime_error("Capacity ambiguity lost");
        std::vector<uint8_t> sample(0x1200);double v=3.36;for(const auto& c:capacity.candidates)std::memcpy(sample.data()+c.offset,&v,8);
        guided::Narrow(capacity,sample,336);if(capacity.transitions!=3 || capacity.observations.size()!=5 || capacity.candidates.size()!=2)throw std::runtime_error("Unchanged capture reset history");
        auto mana=g_guided["mana"];if(mana.status()!="ambiguous")throw std::runtime_error("Mana promoted without evidence");
        std::cout<<"PASS: shared evidence, pending fields and preserved comparison history.\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
