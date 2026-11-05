#include "../native_runtime/actions/PendingItemPayload.hpp"
#include "../native_runtime/runtime/LockedLogBuffer.hpp"
#include <atomic>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int main(){try {
    volatile LONG pending=0;std::vector<int> payload;std::atomic<bool> published=false;
    check(crome::tryPublishItemRequest(pending,[&]{payload={4597,4598,4599};return true;}),"initial request rejected");
    const auto original=payload;
    for(int i=0;i<10000;++i)check(!crome::tryPublishItemRequest(pending,[&]{payload.clear();return true;}),"pending request replaced");
    check(payload==original,"in-flight search corrupted by retry");InterlockedExchange(&pending,0);
    check(!crome::tryPublishItemRequest(pending,[&]{return false;})&&pending==0,"failed post locked queue");
    try {crome::tryPublishItemRequest(pending,[]{throw std::runtime_error("publish failed");return true;});}catch(const std::runtime_error&){}
    check(pending==0,"throw locked queue");
    std::atomic<bool> bad=false,done=false;
    std::thread consumer([&]{for(int i=0;i<10000;++i){while(!published.load(std::memory_order_acquire))std::this_thread::yield();
        if(payload.size()!=256)bad=true;else for(int value:payload)if(value!=i)bad=true;
        payload.clear();published.store(false,std::memory_order_release);InterlockedExchange(&pending,0);
    }done=true;});
    for(int i=0;i<10000;++i){while(!crome::tryPublishItemRequest(pending,[&]{payload.assign(256,i);published.store(true,std::memory_order_release);return true;}))std::this_thread::yield();}
    consumer.join();check(done&&!bad,"concurrent queue payload changed");
    std::stringbuf sink;crome::LockedLogBuffer buffer(&sink);std::vector<std::thread> writers;
    const std::string record(256,'x');
    for(int j=0;j<8;++j)writers.emplace_back([&]{std::ostream stream(&buffer);for(int i=0;i<2000;++i){stream<<record<<std::flush;stream.put('y');}});
    for(auto& thread:writers)thread.join();
    const auto text=sink.str();check(text.size()==8u*2000u*257u,"concurrent log lost/corrupted bytes");
    check(std::count(text.begin(),text.end(),'y')==16000,"single-character log writes lost");
    std::cout<<"PASS: pending retries, failed publish, 10000 concurrent payloads, shared logging writes and flushes\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
