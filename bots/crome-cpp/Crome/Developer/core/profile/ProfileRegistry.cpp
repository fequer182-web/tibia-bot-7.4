#include "core/profile/ProfileRegistry.hpp"
#include "core/profile/ProfileLoader.hpp"
namespace crome {
void ProfileRegistry::loadDirectory(const std::filesystem::path& dir){
    profiles_.clear();
    if(!std::filesystem::exists(dir)) return;
    for(const auto& e: std::filesystem::directory_iterator(dir)){
        if(!e.is_regular_file() || e.path().extension()!=".json") continue;
        try { profiles_.push_back(ProfileLoader::load(e.path())); } catch(...) {}
    }
}
const BuildProfile* ProfileRegistry::find(const BuildFingerprint& fp) const{
    for(const auto& p:profiles_){
        if(p.build.timestamp==fp.timestamp && p.build.sizeOfImage==fp.sizeOfImage && p.build.textHash==fp.textHash) return &p;
    }
    return nullptr;
}
}
