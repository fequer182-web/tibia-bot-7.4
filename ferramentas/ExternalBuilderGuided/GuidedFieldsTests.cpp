#include "GuidedFields.hpp"
#include <iostream>
#include <stdexcept>
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
template<class T> void set(std::vector<uint8_t>& b,size_t off,T v){std::memcpy(b.data()+off,&v,sizeof(v));}
int main() {
    try {
        check(guided::SeedAllowed(true,3,8),"regenerated HP and non-double XP must not block seed");
        check(!guided::SeedAllowed(false,7,8),"max HP anchor required");check(!guided::SeedAllowed(true,7,0),"position shape required");
        std::vector<uint8_t> a(256,0),b,c;
        // A zero-valued mana field is ambiguous. Unrelated zeros must be removed after changes.
        set<double>(a,64,0);guided::Evidence e;e.candidates=guided::Find(a,0);e.observations={0};check(e.candidates.size()>10,"initial zeros should be ambiguous");
        b=a;set<double>(b,64,20);guided::Narrow(e,b,20);check(e.candidates.size()==1 && e.candidates[0].offset==64 && e.status()=="strong_candidate","one transition is not confirmation");
        c=b;set<double>(c,64,35);guided::Narrow(e,c,35);check(e.status()=="confirmed_read_transition","two transitions confirm unique read candidate");
        guided::Narrow(e,c,35);check(e.transitions==2,"unchanged observation must not confirm");
        guided::Evidence duplicate;set<double>(a,64,150);set<double>(a,128,150);duplicate.candidates=guided::Find(a,150);duplicate.observations={150};b=a;set<double>(b,64,130);set<double>(b,128,130);guided::Narrow(duplicate,b,130);c=b;set<double>(c,64,109);set<double>(c,128,109);guided::Narrow(duplicate,c,109);check(duplicate.status()=="ambiguous","two equivalent locations remain ambiguous");
        guided::Evidence absent;absent.candidates=guided::Find(a,987654.5);absent.observations={987654.5};check(absent.status()=="pending","missing field pending");
        std::vector<uint8_t> f(24,0xFF);set<float>(f,4,286.25f);auto found=guided::Find(f,286.25);check(found.size()==1 && found[0].type=="float32","float storage detected");
        check(!guided::Match(f,{4,"float32"},300),"mismatch rejected");check(!guided::Match(f,{23,"float64"},0),"short buffer rejected");check(!guided::Match(f,{4,"float32"},NAN),"NaN rejected");
        std::cout<<"PASS: zeros, two changes, duplicates, missing fields, types and invalid data.\n";
        return 0;
    }catch(const std::exception& ex){std::cerr<<"FAIL: "<<ex.what()<<"\n";return 1;}
}
