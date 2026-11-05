#include "app/CooldownManager.hpp"
namespace crome { bool CooldownManager::ready(const std::string& k,std::chrono::milliseconds d)const{auto i=last_.find(k);return i==last_.end()||Clock::now()-i->second>=d;} void CooldownManager::mark(const std::string& k){last_[k]=Clock::now();} }
