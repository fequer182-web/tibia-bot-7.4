#pragma once
#include "CompletionGuidance.hpp"
#include <array>
#include <limits>

// Runtime completion tests use only current-session objects. Profile pointers
// are historical; only current-build layouts and RVAs may be reused.
namespace completion_probe {
constexpr size_t maxThings=64, maxOffsets=14;
constexpr DWORD trapFlag=0x100;
struct Layout {
    uintptr_t tileVtable=0, itemVtable=0, game=0;
    uint32_t beginOffset=0,endOffset=0,stride=0;
    bool rawArray=false;
};
struct ObjectSample { uintptr_t pointer=0; uint32_t counters[maxOffsets]{}; uint32_t mask=0; };
struct Pair {
    Capture entry{};
    uintptr_t rax=0,returnRsp=0,holder=0,object=0,tile=0;
    MemSnap holders[4]{},returnedObject{};
    ObjectSample before[maxThings]{}; uint32_t things=0;
    uint8_t vectorBytes[256]{}; uint32_t vectorSize=0;
    uintptr_t vectorBegin=0,vectorEnd=0,vectorCapacity=0;
    uintptr_t scalarReceiver=0,scalarExpected=0,activeTarget=0;uint32_t scalarWidth=0;
    bool returned=false,membership=false,stackBalanced=false,active=false;
    uint32_t counterOffsets[4]{},counterCount=0;
    uint32_t watchBefore[4]{},watchIncrement[4]{},watchDecrement[4]{};
    uintptr_t watchedObject=0;uint32_t watchOffsets[4]{},watchCount=0;
    uintptr_t incrementIp[4]{},decrementIp[4]{};
    const char* stop="not_started";
};
struct Patch { uintptr_t address=0; uint8_t byte=0; DWORD protect=0; bool held=false; };
struct State {
    volatile LONG installed=0,done=0,inFlight=0,cancel=0;
    Patch entry{},ret{}; PVOID veh=nullptr;
    Layout layout{}; Pair pair{};
    uintptr_t seedObject=0; uint32_t seedOffsets[4]{},seedCount=0;
    DWORD tid=0,requiredTid=0; uint32_t steps=0; ULONGLONG started=0;
    DWORD64 savedDr[6]{};
    bool watching=false,afterReturn=false,activeRequested=false;
    int activeKind=0; // 1: Tile hidden return, 2: typed autoWalk, 3: live Game argument relay.
    uintptr_t scalarTarget=0;uint32_t scalarOffset=0,scalarWidth=0;
};
inline State state{};
using IndependentCallback=bool(*)(Pair&,void*);
inline IndependentCallback independentCallback=nullptr;
inline void* independentContext=nullptr;
// Keep this handler/module alive for delayed INT3 delivery after restoration.
// A restored site is handled only while its byte still matches our original.
inline PVOID retainedHandler=nullptr;
inline volatile LONG handlerCalls=0;
inline Patch retiredSites[256]{};
inline size_t retiredCount=0;
struct HandlerScope {
    HandlerScope(){InterlockedIncrement(&handlerCalls);}
    ~HandlerScope(){InterlockedDecrement(&handlerCalls);}
};
inline bool remember(const Patch& site) {
    for(size_t i=0;i<retiredCount;++i)if(retiredSites[i].address==site.address)return retiredSites[i].byte==site.byte;
    if(retiredCount==256)return false;
    retiredSites[retiredCount++]=site;return true;
}
inline Layout layout(const Profile&);
inline std::string pairJson(const Pair&);
inline bool take(uintptr_t,uintptr_t,const Layout&,const std::string&,Pair&,uintptr_t=0,const std::vector<uint32_t>& = {},int=0,DWORD=0);

inline bool read(uintptr_t address,void* out,size_t bytes) {
    SIZE_T got=0;
    return address && bytes && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<LPCVOID>(address),out,bytes,&got) && got==bytes;
}
template<class T> inline bool read(uintptr_t address,T& out) { return read(address,&out,sizeof(out)); }
inline MemSnap snap(uintptr_t pointer) {
    MemSnap result{}; result.base=pointer;
    for(uint32_t off=0;off<ARG_SNAP_BYTES;off+=8) {
        if(!read(pointer+off,result.bytes+off,8)) break;
        result.validBytes=off+8;
    }
    return result;
}
inline bool vtable(uintptr_t object,uintptr_t expected) {
    uintptr_t actual=0; return expected && read(object,actual) && actual==expected;
}
inline bool members(uintptr_t tile,const Layout& layout,ObjectSample* objects,uint32_t& count) {
    count=0; uintptr_t begin=0,end=0;
    if(!layout.rawArray || layout.stride!=8 || !vtable(tile,layout.tileVtable) ||
       !read(tile+layout.beginOffset,begin) || !read(tile+layout.endOffset,end) ||
       !begin || end<begin || (end-begin)%8 || (end-begin)/8>maxThings) return false;
    count=static_cast<uint32_t>((end-begin)/8);
    for(uint32_t i=0;i<count;++i) {
        auto& object=objects[i]; object={};
        if(!read(begin+i*8,object.pointer) || !object.pointer) return false;
        for(uint32_t j=0;j<maxOffsets;++j)
            if(read(object.pointer+8+j*4,object.counters[j])) object.mask|=1u<<j;
    }
    return true;
}
inline bool contains(const Pair& pair,uintptr_t object) {
    for(uint32_t i=0;i<pair.things;++i) if(pair.before[i].pointer==object) return true;
    return false;
}
inline bool prepare(Patch& patch,uintptr_t address,const Patch* prior=nullptr) {
    if(!IsExecutable(address) || !read(address,patch.byte) || patch.byte==0xCC) return false;
    if(!prior || (prior->address&~uintptr_t(4095))!=(address&~uintptr_t(4095))) {
        if(!VirtualProtect(reinterpret_cast<void*>(address),1,PAGE_EXECUTE_READWRITE,&patch.protect)) return false;
        patch.held=true;
    }
    patch.address=address;
    return true;
}
inline void patch(const Patch& site,uint8_t byte) {
    if(!site.address) return;
    *reinterpret_cast<volatile uint8_t*>(site.address)=byte;
    FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<void*>(site.address),1);
}
inline void restoreProtection(Patch& site) {
    if(site.held) { DWORD ignored=0; VirtualProtect(reinterpret_cast<void*>(site.address),1,site.protect,&ignored); site.held=false; }
}
inline void restoreDebug(CONTEXT& c) {
    if(state.watching) {
        c.Dr0=state.savedDr[0];c.Dr1=state.savedDr[1];c.Dr2=state.savedDr[2];c.Dr3=state.savedDr[3];
        c.Dr6=state.savedDr[4];c.Dr7=state.savedDr[5];state.watching=false;
    }
    c.EFlags&=~trapFlag;
}
inline void finish(CONTEXT& c,const char* reason) {
    restoreDebug(c);state.pair.stop=reason;
    InterlockedExchange(&state.inFlight,0);InterlockedExchange(&state.done,1);
}
inline void recordEntry(const CONTEXT& c) {
    auto& pair=state.pair;auto& entry=pair.entry;
    entry.tid=GetCurrentThreadId();entry.rcx=c.Rcx;entry.rdx=c.Rdx;entry.r8=c.R8;entry.r9=c.R9;entry.rsp=c.Rsp;
    read(c.Rsp,entry.ret);
    for(int i=0;i<STACK_QWORDS;++i) read(c.Rsp+8ull*(i+1),entry.stack[i]);
    entry.rcxMem=snap(c.Rcx);entry.rdxMem=snap(c.Rdx);entry.r8Mem=snap(c.R8);entry.r9Mem=snap(c.R9);
    ResolveNaturalCaller(state.entry.address,entry);
    if(vtable(entry.rcx,state.layout.tileVtable)) pair.tile=entry.rcx;
    else if(vtable(entry.rdx,state.layout.tileVtable)) pair.tile=entry.rdx;
    if(pair.tile) members(pair.tile,state.layout,pair.before,pair.things);
    uintptr_t begin=0,end=0,capacity=0;
    if(SnapRead(entry.rdxMem,0,begin)&&SnapRead(entry.rdxMem,8,end)&&SnapRead(entry.rdxMem,16,capacity)
       && begin && end>begin && end<=capacity && end-begin<=sizeof(pair.vectorBytes)
       && read(begin,pair.vectorBytes,static_cast<size_t>(end-begin))) {
        pair.vectorBegin=begin;pair.vectorEnd=end;pair.vectorCapacity=capacity;
        pair.vectorSize=static_cast<uint32_t>(end-begin);
    }
}
inline void armWatches(CONTEXT& c) {
    if(!state.seedCount || !contains(state.pair,state.seedObject) || (c.Dr7&0xFF)) return;
    state.savedDr[0]=c.Dr0;state.savedDr[1]=c.Dr1;state.savedDr[2]=c.Dr2;state.savedDr[3]=c.Dr3;
    state.savedDr[4]=c.Dr6;state.savedDr[5]=c.Dr7;
    uintptr_t addresses[4]{};
    for(uint32_t i=0;i<state.seedCount;++i) {
        addresses[i]=state.seedObject+state.seedOffsets[i];
        if(addresses[i]%4 || !read(addresses[i],state.pair.watchBefore[i]) || state.pair.watchBefore[i]==0) return;
    }
    c.Dr0=addresses[0];c.Dr1=addresses[1];c.Dr2=addresses[2];c.Dr3=addresses[3];c.Dr6=0;c.Dr7=0;
    for(uint32_t i=0;i<state.seedCount;++i) c.Dr7|=(1ull<<(i*2)) | (13ull<<(16+i*4)); // local, write, length=4
    state.watching=true;
    state.pair.watchedObject=state.seedObject;state.pair.watchCount=state.seedCount;
    for(uint32_t i=0;i<state.seedCount;++i) state.pair.watchOffsets[i]=state.seedOffsets[i];
}
inline bool ownershipComplete(const Pair& pair) {
    if(!pair.returned || !pair.membership || pair.object!=pair.watchedObject) return false;
    for(uint32_t i=0;i<pair.watchCount;++i)
        if(pair.incrementIp[i] && pair.decrementIp[i] && pair.watchIncrement[i]==pair.watchBefore[i]+1 &&
           pair.watchDecrement[i]==pair.watchBefore[i]) return true;
    return false;
}
inline bool activeOwnershipBalanced(const Pair& pair) {
    if(!pair.active || !pair.returned || !pair.membership || pair.object!=pair.watchedObject) return false;
    for(uint32_t i=0;i<pair.watchCount;++i) {
        uint32_t atReturn=0;
        if(pair.watchOffsets[i]<ARG_SNAP_BYTES && SnapRead(pair.returnedObject,pair.watchOffsets[i],atReturn) &&
           atReturn==pair.watchBefore[i]+1 && pair.decrementIp[i] && pair.watchDecrement[i]==pair.watchBefore[i]) return true;
    }
    return false;
}
inline bool callerDiscardsScalar(uintptr_t returnAddress) {
    uint8_t code[7]{};if(!read(returnAddress,code,sizeof(code))) return false;
    return code[0]==0xB8 || (code[0]==0x33&&code[1]==0xC0) || (code[0]==0x31&&code[1]==0xC0)
        || (code[0]==0x48&&code[1]==0xC7&&code[2]==0xC0);
}
inline bool relayPreservesScalarReturn(){
#ifdef CROME_ABI_FINALIZER_PRESERVE_RETURN
    return true;
#else
    return false;
#endif
}
inline bool callerUsesBool(uintptr_t returnAddress) {
    uint8_t code[2]{};return read(returnAddress,code,sizeof(code)) && code[0]==0x84 && code[1]==0xC0;
}
inline bool scalarLeaf(uintptr_t function,uint32_t expectedOffset,uint32_t expectedWidth) {
    uint8_t bytes[16]{};if(!read(function,bytes,sizeof(bytes)))return false;
    size_t i=0;uint32_t width=0;
    if(bytes[0]==0x0F&&(bytes[1]==0xB6||bytes[1]==0xB7)) {width=bytes[1]==0xB6?1:2;i=2;}
    else if(bytes[0]==0x8B){width=4;i=1;}
    else if(bytes[0]==0x48&&bytes[1]==0x8B){width=8;i=2;}
    else return false;
    if(width!=expectedWidth) return false;
    uint32_t offset=0;
    if(bytes[i]==0x01) ++i;
    else if(bytes[i]==0x41) {offset=bytes[i+1];i+=2;}
    else if(bytes[i]==0x81){memcpy(&offset,bytes+i+1,4);i+=5;}
    else return false;
    return offset==expectedOffset&&bytes[i]==0xC3;
}
inline void recordReturn(const CONTEXT& c) {
    auto& pair=state.pair;pair.returned=true;pair.rax=c.Rax;pair.returnRsp=c.Rsp;
    pair.stackBalanced=c.Rsp==pair.entry.rsp+8;
    const uintptr_t holders[]{pair.entry.rcx,pair.entry.rdx,pair.entry.r8,pair.entry.r9};
    size_t matches=0;
    for(size_t i=0;i<4;++i) {
        pair.holders[i]=snap(holders[i]);uintptr_t object=0;
        // A hidden return slot must be a current caller-stack address, returned
        // in RAX, with an object from the exact Tile::things entry snapshot.
        if(holders[i]==c.Rax && holders[i]>pair.entry.rsp && holders[i]-pair.entry.rsp<=8192 &&
           SnapRead(pair.holders[i],0,object) && contains(pair,object)) {
            pair.holder=holders[i];pair.object=object;++matches;
        }
    }
    if(matches!=1) { pair.holder=0;pair.object=0;return; }
    ObjectSample now[maxThings]{};uint32_t count=0;
    if(!members(pair.tile,state.layout,now,count)) return;
    for(uint32_t i=0;i<count;++i) if(now[i].pointer==pair.object) pair.membership=true;
    if(!pair.membership) return;
    pair.returnedObject=snap(pair.object);
    for(uint32_t i=0;i<pair.things;++i) if(pair.before[i].pointer==pair.object) {
        for(uint32_t j=0;j<maxOffsets;++j) {
            uint32_t after=0;
            if((pair.before[i].mask&(1u<<j)) && pair.before[i].counters[j]>0 && pair.before[i].counters[j]<0x100000 &&
               read(pair.object+8+j*4,after) && after==pair.before[i].counters[j]+1 && pair.counterCount<4)
                pair.counterOffsets[pair.counterCount++]=8+j*4;
        }
    }
}
inline LONG CALLBACK handler(PEXCEPTION_POINTERS info) {
    HandlerScope scope;
    if(!info || !info->ExceptionRecord || !info->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    auto& c=*info->ContextRecord;const DWORD code=info->ExceptionRecord->ExceptionCode;
    const auto at=reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress);
    const DWORD tid=GetCurrentThreadId();
    if(code==EXCEPTION_BREAKPOINT&&state.installed&&state.cancel&&(at==state.entry.address||at==state.ret.address)) {
        const auto& site=at==state.entry.address?state.entry:state.ret;
        patch(site,site.byte);c.Rip=at;
        if(state.inFlight&&tid==state.tid)finish(c,"cancelled");
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if(code==EXCEPTION_BREAKPOINT && (!state.installed || state.cancel || (at!=state.entry.address&&at!=state.ret.address))) {
        for(size_t i=0;i<retiredCount;++i)if(retiredSites[i].address==at) {
            uint8_t byte=0;
            if(read(at,byte)&&byte==retiredSites[i].byte) {c.Rip=at;return EXCEPTION_CONTINUE_EXECUTION;}
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if(!state.installed)return EXCEPTION_CONTINUE_SEARCH;
    if(code==EXCEPTION_BREAKPOINT && at==state.entry.address) {
        patch(state.entry,state.entry.byte);c.Rip=state.entry.address;
        if(state.done || state.inFlight || (c.EFlags&trapFlag) || (c.Dr7&0xFF)) { state.pair.stop="entry_debug_conflict";state.done=1;return EXCEPTION_CONTINUE_EXECUTION; }
        recordEntry(c);
#ifdef CROME_ABI_FINALIZER_CURRENT_CALLER
        if(state.activeKind==3||state.activeKind==6){
            const auto& entry=state.pair.entry;
            if(c.Rcx!=state.layout.game||!entry.callSite||!entry.callerFunction||entry.callKind==NaturalCallKind::Unknown||
               !InMainModule(entry.ret)||!IsExecutable(entry.ret)||!entry.callerFunctionEnd||
               entry.callSite<entry.callerFunction||entry.ret>entry.callerFunctionEnd||
               (state.requiredTid&&tid!=state.requiredTid)){
                state.pair.stop="current_caller_receiver_or_thread_invalid";state.done=1;return EXCEPTION_CONTINUE_EXECUTION;
            }
            if(entry.ret!=state.ret.address){
                restoreProtection(state.ret);state.ret={};
                if(!prepare(state.ret,entry.ret,&state.entry)||!remember(state.ret)){
                    state.pair.stop="current_return_install_failed";state.done=1;return EXCEPTION_CONTINUE_EXECUTION;
                }
            }
            patch(state.ret,0xCC);
        }
#endif
        if(state.pair.entry.ret!=state.ret.address) { state.pair.stop="different_natural_caller";state.done=1;return EXCEPTION_CONTINUE_EXECUTION; }
        if(state.requiredTid&&tid!=state.requiredTid) {state.pair.stop="different_natural_thread";state.done=1;return EXCEPTION_CONTINUE_EXECUTION;}
        state.tid=tid;state.started=GetTickCount64();state.inFlight=1;
        if(state.activeKind==5) {
            uintptr_t receiver=c.Rdx;
            if(!vtable(receiver,state.layout.itemVtable)) read(c.Rdx,receiver);
            uint64_t expected=0;
            if(vtable(receiver,state.layout.itemVtable)&&state.scalarWidth&&state.scalarWidth<=8&&
               scalarLeaf(state.scalarTarget,state.scalarOffset,state.scalarWidth)&&
               read(receiver+state.scalarOffset,&expected,state.scalarWidth)) {
                using Native=uintptr_t(*)(uintptr_t);
                state.pair.rax=reinterpret_cast<Native>(state.scalarTarget)(receiver);
                state.pair.scalarReceiver=receiver;state.pair.scalarExpected=expected;
                state.pair.scalarWidth=state.scalarWidth;state.pair.activeTarget=state.scalarTarget;
                state.pair.returnedObject=snap(receiver);state.pair.active=true;state.pair.returned=true;
                state.pair.stackBalanced=true;patch(state.ret,state.ret.byte);
                finish(c,state.pair.rax==expected?"active_scalar_leaf_verified":"active_scalar_value_mismatch");
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            patch(state.ret,state.ret.byte);finish(c,"item_receiver_or_scalar_leaf_required");
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        armWatches(c);c.EFlags|=trapFlag;
        // Execute on the intercepted natural thread, using this call's current
        // arguments. Replace the natural invocation instead of invoking twice.
        // Owned results go into the original caller's storage; that caller
        // performs the measured release. No saved holder or reference is reused.
        if(state.activeRequested && state.activeKind==1 && state.watching && state.seedCount==1 &&
           state.pair.tile==c.Rcx && c.Rdx>c.Rsp && c.Rdx-c.Rsp<=8192 && read(c.Rdx,state.pair.holders[1].bytes,16)) {
            using Native=uintptr_t(*)(uintptr_t,void*);
            c.Rax=reinterpret_cast<Native>(state.entry.address)(c.Rcx,reinterpret_cast<void*>(c.Rdx));
            state.pair.active=true;c.Rsp+=8;c.Rip=state.ret.address;
        } else if(state.activeRequested && state.activeKind==2 && c.Rcx==state.layout.game &&
                  state.pair.vectorSize && state.pair.entry.r8Mem.validBytes>=12) {
            using Native=bool(*)(uintptr_t,const void*,const void*);
            c.Rax=reinterpret_cast<Native>(state.entry.address)(c.Rcx,reinterpret_cast<const void*>(c.Rdx),reinterpret_cast<const void*>(c.R8))?1:0;
            state.pair.active=true;c.Rsp+=8;c.Rip=state.ret.address;
        } else if(state.activeRequested && state.activeKind==3 && c.Rcx==state.layout.game && (relayPreservesScalarReturn()||callerDiscardsScalar(state.ret.address))) {
            // Preserve the captured stack arguments too, after the four shadow
            // slots. This relay remains non-callable: it proves no typed builder.
            const auto& stack=state.pair.entry.stack;
#ifdef CROME_ABI_FINALIZER_PRESERVE_RETURN
            using Native=uintptr_t(*)(uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t);
            c.Rax=reinterpret_cast<Native>(state.entry.address)(c.Rcx,c.Rdx,c.R8,c.R9,stack[4],stack[5],stack[6],stack[7],stack[8],stack[9],stack[10],stack[11]);
#else
            using Native=void(*)(uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t);
            reinterpret_cast<Native>(state.entry.address)(c.Rcx,c.Rdx,c.R8,c.R9,stack[4],stack[5],stack[6],stack[7],stack[8],stack[9],stack[10],stack[11]);
            c.Rax=0;
#endif
            state.pair.active=true;c.Rsp+=8;c.Rip=state.ret.address;
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if(code==EXCEPTION_BREAKPOINT && at==state.ret.address) {
        patch(state.ret,state.ret.byte);c.Rip=state.ret.address;
        if(state.done) return EXCEPTION_CONTINUE_EXECUTION;
        if(!state.inFlight || tid!=state.tid || c.Rsp!=state.pair.entry.rsp+8) {
            state.cancel=1;state.pair.stop="return_not_paired";
            if(state.inFlight && tid==state.tid) finish(c,"return_not_paired");
            else if(!state.inFlight) state.done=1;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        recordReturn(c);state.afterReturn=true;
        if(state.activeKind==6){
            state.pair.active=independentCallback&&independentCallback(state.pair,independentContext);
            finish(c,state.pair.active?"independent_attack_returned":"independent_attack_validation_failed");
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if(!state.watching) finish(c,state.pair.membership?"return_and_membership_observed":"return_observed");
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if(code==EXCEPTION_SINGLE_STEP && state.inFlight && tid==state.tid) {
        // Only our trap flag or hardware data watch is handled; external debug
        // state is preserved and a conflicting trap flag prevents installation.
        if(state.watching) for(uint32_t i=0;i<state.seedCount;++i) if(c.Dr6&(1ull<<i)) {
            uint32_t value=0;read(state.seedObject+state.seedOffsets[i],value);
            if(!state.afterReturn && value==state.pair.watchBefore[i]+1) {
                state.pair.watchIncrement[i]=value;state.pair.incrementIp[i]=c.Rip;
            } else if(state.afterReturn && value==state.pair.watchBefore[i] && (state.pair.incrementIp[i] || state.pair.active)) {
                state.pair.watchDecrement[i]=value;state.pair.decrementIp[i]=c.Rip;
            }
        }
        if(state.watching) c.Dr6=0;
        else c.Dr6&=~0x4000ull;
        if(state.cancel) finish(c,"cancelled");
        else if(ownershipComplete(state.pair)) finish(c,"increment_and_caller_release_observed");
        else if(activeOwnershipBalanced(state.pair)) finish(c,"active_call_return_and_caller_release_observed");
        else if(++state.steps>=30000 || GetTickCount64()-state.started>=150)
            finish(c,state.afterReturn?"caller_release_not_observed_within_budget":"return_not_observed_within_budget");
        else c.EFlags|=trapFlag;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if(state.inFlight && tid==state.tid && code!=EXCEPTION_BREAKPOINT && code!=EXCEPTION_SINGLE_STEP)
        finish(c,"native_exception_during_trace");
    return EXCEPTION_CONTINUE_SEARCH;
}

struct Outcome {
    std::string key,name,reason="pending";uintptr_t rva=0;
    bool typed=false,returned=false,ownership=false,active=false,thread=false,effect=false,replayOnly=false;
    uint32_t refOffset=0,elementWidth=0;
    std::vector<Pair> observations;
    bool complete() const {return typed&&returned&&ownership&&active&&thread&&effect&&!replayOnly;}
};
inline std::vector<uint32_t> provenOffsets(const Pair& pair) {
    std::vector<uint32_t> offsets;
    if(!ownershipComplete(pair)) return offsets;
    for(uint32_t i=0;i<pair.watchCount;++i)
        if(pair.incrementIp[i]&&pair.decrementIp[i]&&pair.watchIncrement[i]==pair.watchBefore[i]+1&&pair.watchDecrement[i]==pair.watchBefore[i])
            offsets.push_back(pair.watchOffsets[i]);
    return offsets;
}
inline bool loweredTileReturn(const Pair& pair) {
    return pair.returned&&pair.stackBalanced&&pair.membership&&pair.tile==pair.entry.rcx&&pair.holder==pair.entry.rdx;
}
inline bool vectorLayout(const Pair& pair,uint32_t expectedSteps,uint32_t& width) {
    width=0;
    if(!expectedSteps || expectedSteps>128 || !pair.vectorSize || pair.vectorSize%expectedSteps) return false;
    width=pair.vectorSize/expectedSteps;
    if(width!=1 && width!=4) {width=0;return false;}
    for(uint32_t i=0;i<expectedSteps;++i) {
        uint32_t direction=0;memcpy(&direction,pair.vectorBytes+i*width,width);
        if(direction>7) {width=0;return false;}
    }
    return true;
}
inline bool startPosition(const Pair& pair,uint32_t x,uint32_t y,uint32_t z) {
    uint32_t px=0,py=0;uint8_t pz=0;
    return SnapRead(pair.entry.r8Mem,0,px)&&SnapRead(pair.entry.r8Mem,4,py)&&SnapRead(pair.entry.r8Mem,8,pz)&&px==x&&py==y&&pz==z;
}
inline std::string outcomeJson(const Outcome& result) {
    std::ostringstream json;
    json << "{\"rva\":\"" << Hex(result.rva) << "\",\"status\":\"" << (result.complete()?"complete_runtime_contract":result.reason)
         << "\",\"typedArgumentsComplete\":" << (result.typed?"true":"false") << ",\"returnValidated\":" << (result.returned?"true":"false")
         << ",\"ownershipValidated\":" << (result.ownership?"true":"false") << ",\"activeCallValidated\":" << (result.active?"true":"false")
         << ",\"gameThreadDispatchValidated\":" << (result.thread?"true":"false") << ",\"effectConfirmed\":" << (result.effect?"true":"false")
         << ",\"contractComplete\":" << (result.complete()?"true":"false") << ",\"callable\":false,\"runtimeAdapterReady\":false"
         << ",\"replayOnly\":" << (result.replayOnly?"true":"false") << ",\"refcountOffset\":" << result.refOffset
         << ",\"directionElementBytes\":" << result.elementWidth
         << ",\"dispatchRoute\":\"one_shot_current_natural_entry_game_thread\",\"savedPointersAreHistorical\":true,\"pending\":[";
    std::vector<std::string> pending;
    if(!result.typed) pending.emplace_back("typed_arguments");if(!result.returned)pending.emplace_back("paired_return_contract");
    if(!result.ownership)pending.emplace_back("ownership_increment_and_caller_release");if(!result.active)pending.emplace_back("active_call");
    if(!result.thread)pending.emplace_back("game_thread_dispatch");if(!result.effect)pending.emplace_back("functional_result");
    if(result.replayOnly)pending.emplace_back("independent_argument_construction_and_dispatch_adapter");
    for(size_t i=0;i<pending.size();++i){if(i)json << ",";json << "\"" << pending[i] << "\"";}
    json << "],\"observations\":[";
    for(size_t i=0;i<result.observations.size();++i){if(i)json << ",";json << pairJson(result.observations[i]);}
    json << "]}";return json.str();
}
inline Outcome tileTests(const Profile& profile,const AbiResult& entryEvidence) {
    Outcome result;result.key=entryEvidence.key;result.name=entryEvidence.name;result.rva=entryEvidence.rva;
    const auto config=layout(profile);
    if(!config.tileVtable || !config.rawArray || config.stride!=8 || !entryEvidence.naturalReturnRva) {result.reason="tile_layout_or_current_caller_required";return result;}
    std::vector<std::vector<uint32_t>> refProofs;
    uintptr_t lastObject=0;
    for(int stimulus=0;stimulus<2;++stimulus) {
        Pair returned{},watched{};
        const std::string action=entryEvidence.key=="tileGetTopUseThing"?"Use/selecionar um objeto do chao":"Selecione/comece arrastar um item do chao e cancele o arraste";
        if(!take(result.rva,entryEvidence.naturalReturnRva,config,action+" no tile "+(stimulus==0?"A":"B (outro tile/item)")+"; ENTER...",returned)) {
            result.observations.push_back(returned);result.reason=returned.stop;return result;
        }
        result.observations.push_back(returned);
        if(!loweredTileReturn(returned) || (stimulus && returned.object==lastObject)) {result.reason="distinct_tile_return_lowering_required";return result;}
        std::vector<uint32_t> offsets(returned.counterOffsets,returned.counterOffsets+returned.counterCount);
        if(offsets.empty()) {result.reason="refcount_increment_candidate_not_observed";return result;}
        lastObject=returned.object;
        take(result.rva,entryEvidence.naturalReturnRva,config,"Repita no MESMO tile com o MESMO item, sem alterar a pilha; ENTER...",watched,returned.object,offsets);
        result.observations.push_back(watched);
        const auto proven=provenOffsets(watched);
        if(!loweredTileReturn(watched) || proven.empty()) {result.reason="hardware_increment_and_caller_release_pending";return result;}
        refProofs.push_back(proven);
    }
    std::vector<uint32_t> common;
    for(uint32_t offset:refProofs[0]) if(std::find(refProofs[1].begin(),refProofs[1].end(),offset)!=refProofs[1].end()) common.push_back(offset);
    if(common.size()!=1) {result.reason="refcount_offset_ambiguous";return result;}
    for(const auto& observation:result.observations)if(observation.entry.tid!=result.observations[0].entry.tid) {
        result.reason="natural_thread_inconsistent";return result;
    }
    result.refOffset=common[0];result.typed=true;result.returned=true;result.ownership=true;
    result.reason="active_call_pending";
    if(!AskYes("Testar chamada ATIVA deste getter no game thread, usando os argumentos atuais do cliente?",true)) return result;
    Pair active{};
    take(result.rva,entryEvidence.naturalReturnRva,config,"Repita a acao no ULTIMO tile/item; o Builder executara a chamada uma vez no lugar da chamada natural. ENTER...",active,lastObject,common,1,result.observations[0].entry.tid);
    result.observations.push_back(active);
    result.active=activeOwnershipBalanced(active)&&loweredTileReturn(active);
    result.thread=result.active&&active.entry.tid==result.observations[0].entry.tid;
    result.effect=result.active;result.reason=result.complete()?"complete_runtime_contract":active.stop;
    return result;
}
inline Outcome autoWalkTests(const Profile& profile,const AbiResult& entryEvidence) {
    Outcome result;result.key=entryEvidence.key;result.name=entryEvidence.name;result.rva=entryEvidence.rva;result.ownership=true;
    const auto config=layout(profile);uint32_t widths[2]{};
    if(!config.game || !entryEvidence.naturalReturnRva) {result.reason="current_game_and_caller_required";return result;}
    for(int stimulus=0;stimulus<2;++stimulus) {
        uint32_t x=0,y=0,z=0;if(!ReadPosition(profile,x,y,z)) {result.reason="current_player_position_required";return result;}
        const uint32_t steps=AskU32("Escolha uma linha reta livre: quantos SQMs tera o clique de destino?",4);
        Pair pair{};take(result.rva,entryEvidence.naturalReturnRva,config,"Clique no destino escolhido, SEM setas; use outra direcao na amostra B. ENTER...",pair);
        result.observations.push_back(pair);
        if(!pair.returned || !pair.stackBalanced || pair.entry.rcx!=config.game || !vectorLayout(pair,steps,widths[stimulus]) ||
           !startPosition(pair,x,y,z)) {result.reason="direction_vector_or_start_position_pending";return result;}
        if(!callerUsesBool(pair.entry.ret) || (pair.rax&0xFF)>1) {result.reason="boolean_return_consumer_required";return result;}
    }
    if(widths[0]!=widths[1] || result.observations[0].entry.tid!=result.observations[1].entry.tid) {result.reason="vector_layout_or_thread_inconsistent";return result;}
    result.elementWidth=widths[0];result.typed=true;result.returned=true;result.reason="active_call_pending";
    if(!AskYes("Testar Game::autoWalk ATIVO com o proximo clique, no game thread?",true)) return result;
    Pair active{};take(result.rva,entryEvidence.naturalReturnRva,config,"Clique em outro destino livre. O Builder executara a chamada uma vez. ENTER...",active,0,{},2,result.observations[0].entry.tid);
    result.observations.push_back(active);result.active=active.active&&active.returned&&active.stackBalanced;
    result.thread=result.active&&active.entry.tid==result.observations[0].entry.tid;
    result.effect=result.active&&AskYes("O personagem caminhou corretamente ate o destino escolhido?",false);
    result.reason=result.complete()?"complete_runtime_contract":"active_walk_or_effect_pending";return result;
}
inline Outcome actionTests(const Profile& profile,const AbiResult& evidence) {
    Outcome result;result.key=evidence.key;result.name=evidence.name;result.rva=evidence.rva;result.ownership=true;result.replayOnly=true;
    if(!evidence.machine || !evidence.callerSiteValidated || !evidence.naturalReturnRva) {result.reason="typed_argument_or_current_caller_required";return result;}
    const auto config=layout(profile);
    if(!relayPreservesScalarReturn()&&!callerDiscardsScalar(g_base+evidence.naturalReturnRva)) {result.reason="caller_return_usage_required";return result;}
    Pair active{};
    if(!evidence.naturalThreadId){result.reason="current_natural_thread_required";return result;}
#ifdef CROME_ABI_FINALIZER_CURRENT_CALLER
    for(unsigned attempt=0;attempt<2;++attempt){
        Wait("PREPARACAO (captura ainda DESLIGADA): cancele ataque E follow no jogo. Volte ao console e ENTER...\n");
        take(result.rva,evidence.naturalReturnRva,config,"CAPTURA ATIVA: agora faca UMA NOVA acao: "+completion_guidance::forMethod(evidence.key).a+" Nao cancele durante a captura. Depois volte e ENTER...",active,0,{},3,evidence.naturalThreadId);
        if(active.active&&active.returned)break;
        if(active.stop!="not_started")break;
        std::cout<<"[SEM CHAMADA] Nenhuma entrada ocorreu. Repita selecionando uma criatura apos limpar o alvo.\n";
    }
#else
    take(result.rva,evidence.naturalReturnRva,config,completion_guidance::forMethod(evidence.key).a+" O Builder chamara a funcao uma vez no lugar da chamada natural. Depois volte e pressione ENTER...",active,0,{},3,evidence.naturalThreadId);
#endif
    result.observations.push_back(active);result.active=active.active&&active.returned&&active.stackBalanced;
    result.returned=result.active;result.thread=result.active&&active.entry.tid==evidence.naturalThreadId;
    result.effect=result.active&&AskYes("A acao aconteceu corretamente no cliente?",false);
    result.reason=result.active?"active_live_argument_relay_observed":"active_relay_not_observed";
    // Relaying an existing holder does not validate how an independent caller
    // constructs that holder or acquires its ownership. Never enable from this.
    return result;
}
inline bool stop() {
    state.cancel=1;
    if(state.entry.held||state.ret.held) {patch(state.entry,state.entry.byte);patch(state.ret,state.ret.byte);}
    for(int i=0;i<100 && (state.inFlight||handlerCalls);++i) Sleep(10);
    if(state.inFlight||handlerCalls) return false; // Keep VEH/DLL alive until this thread resumes.
    state.installed=0;
    restoreProtection(state.ret);restoreProtection(state.entry);state.veh=nullptr;
    return true;
}
inline bool start(uintptr_t rva,uintptr_t returnRva,const Layout& layout,uintptr_t seed=0,const std::vector<uint32_t>& offsets={},int activeKind=0,
                  uintptr_t scalarTarget=0,uint32_t scalarOffset=0,uint32_t scalarWidth=0,DWORD requiredTid=0) {
    if(state.installed && !stop()) return false;
    state={};state.layout=layout;state.seedObject=seed;state.requiredTid=requiredTid;
    state.activeRequested=activeKind!=0;state.activeKind=activeKind;
    state.scalarTarget=scalarTarget;state.scalarOffset=scalarOffset;state.scalarWidth=scalarWidth;
    state.seedCount=static_cast<uint32_t>(std::min<size_t>(offsets.size(),4));
    for(uint32_t i=0;i<state.seedCount;++i) {
        if(offsets[i]<8 || offsets[i]>=ARG_SNAP_BYTES || offsets[i]%4) return false;
        state.seedOffsets[i]=offsets[i];
    }
    if(!rva || !returnRva || !InMainModule(g_base+rva) || !InMainModule(g_base+returnRva) || rva==returnRva) return false;
    if(!prepare(state.entry,g_base+rva) || !prepare(state.ret,g_base+returnRva,&state.entry)) { stop();return false; }
    if(!remember(state.entry)||!remember(state.ret)) {stop();return false;}
    if(!retainedHandler)retainedHandler=AddVectoredExceptionHandler(1,handler);
    state.veh=retainedHandler;
    if(!state.veh) { stop();return false; }
    state.installed=1;
#ifdef CROME_ABI_FINALIZER_CURRENT_CALLER
    if(activeKind!=3&&activeKind!=6)patch(state.ret,0xCC);
#else
    patch(state.ret,0xCC);
#endif
    patch(state.entry,0xCC);return true;
}
inline Layout layout(const Profile& profile) {
    Layout result{};
    const auto values=ExtractNamedObject(profile.raw,"values");
    const auto tile=ExtractNamedObject(values,"tile"),things=ExtractNamedObject(tile,"things");
    result.tileVtable=g_base+ParseNum(FlatJsonString(tile,"vtableRva"));
    if(result.tileVtable==g_base) result.tileVtable=0;
    result.itemVtable=profile.itemVtableRva?g_base+profile.itemVtableRva:0;
    result.game=profile.gameRva?g_base+profile.gameRva:0;
    result.beginOffset=static_cast<uint32_t>(ParseNum(FlatJsonString(things,"begin")));
    result.endOffset=static_cast<uint32_t>(ParseNum(FlatJsonString(things,"end")));
    result.rawArray=FlatJsonString(things,"mode")=="raw_pointer_array";
    try {const auto parsed=crome::json::parse(things);const auto* stride=parsed.get("stride");if(stride&&stride->isNumber()) result.stride=static_cast<uint32_t>(stride->number());}catch(...){}
    return result;
}
inline std::string pairJson(const Pair& pair) {
    std::ostringstream json;
    json << "{\"entry\":" << CaptureJson({pair.entry}) << ",\"RAX\":\"" << Hex(pair.rax)
         << "\",\"returned\":" << (pair.returned?"true":"false") << ",\"stackBalanced\":" << (pair.stackBalanced?"true":"false")
         << ",\"returnRsp\":\"" << Hex(pair.returnRsp) << "\",\"tile\":\"" << Hex(pair.tile)
         << "\",\"hiddenStorage\":\"" << Hex(pair.holder) << "\",\"returnedObject\":\"" << Hex(pair.object)
         << "\",\"sameTileMembership\":" << (pair.membership?"true":"false") << ",\"objectSnapshot\":" << SnapJson(pair.returnedObject)
         << ",\"stopReason\":\"" << pair.stop << "\",\"returnStorageSnapshots\":[";
    for(size_t i=0;i<4;++i) {if(i)json << ",";json << SnapJson(pair.holders[i]);}
    json << "],\"refcountCandidates\":[";
    for(uint32_t i=0;i<pair.counterCount;++i) {if(i)json << ",";json << pair.counterOffsets[i];}
    json << "],\"ownershipWrites\":[";
    for(uint32_t i=0;i<pair.watchCount;++i) {if(i)json << ",";json << "{\"offset\":" << pair.watchOffsets[i] << ",\"before\":" << pair.watchBefore[i] << ",\"increment\":" << pair.watchIncrement[i]
        << ",\"release\":" << pair.watchDecrement[i] << ",\"incrementIp\":\"" << Hex(pair.incrementIp[i])
        << "\",\"releaseIp\":\"" << Hex(pair.decrementIp[i]) << "\"}";}
    json << "],\"vectorByteCount\":" << pair.vectorSize << ",\"vectorBytesHex\":\"";
    for(uint32_t i=0;i<pair.vectorSize;++i) json << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(pair.vectorBytes[i]);
    json << "\",\"activeCall\":" << (pair.active?"true":"false")
         << ",\"scalarReceiver\":\"" << Hex(pair.scalarReceiver) << "\",\"scalarExpected\":" << std::dec << pair.scalarExpected
         << ",\"scalarWidth\":" << pair.scalarWidth << ",\"activeTarget\":\"" << Hex(pair.activeTarget) << "\"}";
    return json.str();
}
inline bool take(uintptr_t rva,uintptr_t returnRva,const Layout& config,const std::string& prompt,Pair& result,
                 uintptr_t seed,const std::vector<uint32_t>& offsets,int activeKind,DWORD requiredTid) {
    if(!start(rva,returnRva,config,seed,offsets,activeKind,0,0,0,requiredTid)) {result.stop="probe_installation_failed";return false;}
    std::cout << "[RETURN TEST] Entrada + retorno pareados, limite 30000 instrucoes/150 ms.\n";
    Wait(prompt);
    if(!stop()) {std::cout << "[PENDENTE] Thread ainda dentro da captura; a DLL permanecera carregada.\n";return false;}
    result=state.pair;
    std::cout << "[RETURN RESULT] " << result.stop << " RAX=" << Hex(result.rax) << " membership=" << result.membership << "\n";
    return result.returned && result.stackBalanced;
}

inline Outcome scalarTests(const Profile& profile,const AbiResult& anchor,const std::string& key,uintptr_t candidate) {
    Outcome result;result.key=key;result.rva=candidate;result.ownership=true;
    const uint32_t offset=static_cast<uint32_t>(key=="itemGetId"?profile.itemIdOffset:profile.itemCountOffset);
    const uint32_t width=key=="itemGetId"?2:static_cast<uint32_t>(profile.itemCountWidth);
    if(!offset || !width || !anchor.naturalReturnRva || !scalarLeaf(g_base+candidate,offset,width)) {
        result.reason="current_field_and_exact_scalar_leaf_required";return result;
    }
    const auto config=layout(profile);
    for(int stimulus=0;stimulus<2;++stimulus) {
        Pair pair{};
        const DWORD requiredTid=stimulus?result.observations[0].entry.tid:anchor.naturalThreadId;
        if(!start(anchor.rva,anchor.naturalReturnRva,config,0,{},5,g_base+candidate,offset,width,requiredTid)) {result.reason="scalar_anchor_install_failed";return result;}
        Wait(completion_guidance::prompt(key,stimulus!=0));
        if(!stop()) {result.reason="scalar_capture_still_in_flight";return result;}
        pair=state.pair;result.observations.push_back(pair);
        if(!pair.active || pair.rax!=pair.scalarExpected || (stimulus && (pair.scalarReceiver==result.observations[0].scalarReceiver ||
           pair.scalarExpected==result.observations[0].scalarExpected))) {result.reason="two_distinct_item_values_required";return result;}
    }
    result.typed=true;result.returned=true;result.active=true;result.effect=true;
    result.thread=result.observations[0].entry.tid==result.observations[1].entry.tid;
    result.reason=result.complete()?"complete_runtime_contract":"natural_anchor_thread_inconsistent";
    return result;
}
}
