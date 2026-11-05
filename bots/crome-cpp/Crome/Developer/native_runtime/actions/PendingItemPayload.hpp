#pragma once
#include <windows.h>
#include <utility>
namespace crome {
// Claim before touching any shared payload. A rejected/retried request must
// leave the game thread's in-flight vector and parameters untouched.
template<class Publish> bool tryPublishItemRequest(volatile LONG& pending,Publish&& publish) {
    if(InterlockedCompareExchange(&pending,1,0)!=0)return false;
    try {
        if(std::forward<Publish>(publish)())return true;
    }catch(...){InterlockedExchange(&pending,0);throw;}
    InterlockedExchange(&pending,0);return false;
}
}
