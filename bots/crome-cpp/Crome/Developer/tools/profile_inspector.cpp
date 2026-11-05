#include "core/profile/ProfileLoader.hpp"
#include "core/profile/ProfileValidator.hpp"
#include "core/profile/Capabilities.hpp"
#include "ui/DiagnosticsModel.hpp"
#include <iostream>
int main(int argc,char** argv){ if(argc<2){std::cerr<<"uso: profile_inspector <profile.json>\n";return 2;} try{auto p=crome::ProfileLoader::load(argv[1]);auto v=crome::ProfileValidator::validate(p);std::cout<<crome::diagnosticsText(p)<<"\n";for(auto&s:v.warnings)std::cout<<"WARN: "<<s<<"\n";for(auto&s:v.errors)std::cout<<"ERROR: "<<s<<"\n";return v.usable?0:1;}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;} }
