#include "dominus/native_runtime.hpp"
#include "MinHook.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace dominus::native {
namespace {
constexpr std::uint32_t DrawRva=0x27010,EndInputRva=0x18e30,GetPauseRva=0x16e40,SetPauseRva=0x16d40;
constexpr std::uint32_t OnRva=0x19390,PressedRva=0x193c0,TotalOnRva=0x19420,TotalPressedRva=0x19440;
constexpr std::uint32_t CtorRva=0x26cc0,DtorRva=0x26ef0,ReinitRva=0x174b0;
constexpr std::uint32_t TaskVtableRva=0x512850,BaseVtableRva=0x511fa0,ScreenVtableRva=0x512cf0;

using DrawFn=void (*)(void*,std::uint32_t);
using EndInputFn=void (*)(void*);
using GetPauseFn=std::uint8_t (*)(void*);
using SetPauseFn=void (*)(void*,std::uint8_t);
using QueryFn=std::uint32_t (*)(void*,std::uint32_t);
using CtorFn=void* (*)(void*);
using DtorFn=void* (*)(void*,std::uint32_t);
using ReinitFn=std::uint8_t (*)(void*,std::uint32_t,std::uintptr_t,std::uintptr_t,std::uintptr_t,std::uint32_t,std::uint32_t,std::uint32_t);

struct State {
    std::mutex mutex;
    // Serialize a lease comparison and its native setter with every observed
    // external setter/lifetime mutation, including calls from other threads.
    std::recursive_mutex pauseExecutionMutex;
    Status status{};
    std::uintptr_t base=0,input=0,backend=0;
    std::size_t imageSize=0;
    LogCallback log=nullptr;
    std::array<void*,11> targets{};
    std::atomic<bool> active{false};
    DrawFn draw=nullptr;EndInputFn endInput=nullptr;
    GetPauseFn getPause=nullptr;SetPauseFn setPause=nullptr;
    QueryFn on=nullptr,pressed=nullptr,totalOn=nullptr,totalPressed=nullptr;
    std::array<std::uint32_t,4> lastQueryArgument{},lastQueryResult{};
    std::array<std::uint64_t,4> queryCounts{};
    std::uint64_t queryLogWindow=0;
    std::uint32_t queryLogsInWindow=0;
    CtorFn ctor=nullptr;DtorFn dtor=nullptr;ReinitFn reinit=nullptr;
    PauseRequest pending{};
    PauseReply reply{};
    bool hasPending=false,pauseInFlight=false;
    // The current pause value was set by our Acquire and not cleared since.
    bool modPause=false;
    std::uint64_t nextToken=0,ownedEpoch=0,ownedRevision=0;
    // Distinct callers of the pause methods, logged once each (diagnostic).
    std::array<std::uintptr_t,16> getCallers{},setCallers{};
};
State& state(){static auto* p=new State;return *p;}
thread_local DrawScope currentScope{};
thread_local bool insideLog=false;

bool readable(std::uintptr_t address,std::size_t bytes,bool executable=false) noexcept {
    if(!address||!bytes||address>UINTPTR_MAX-bytes)return false;
    const auto end=address+bytes;
    while(address<end){
        MEMORY_BASIC_INFORMATION info{};
        if(!VirtualQuery(reinterpret_cast<void*>(address),&info,sizeof(info))||info.State!=MEM_COMMIT)return false;
        const DWORD p=info.Protect&0xff;
        if((info.Protect&(PAGE_GUARD|PAGE_NOACCESS))||p==PAGE_NOACCESS)return false;
        const bool canRead=p==PAGE_READONLY||p==PAGE_READWRITE||p==PAGE_WRITECOPY||p==PAGE_EXECUTE_READ||p==PAGE_EXECUTE_READWRITE||p==PAGE_EXECUTE_WRITECOPY;
        const bool canExecute=p==PAGE_EXECUTE||p==PAGE_EXECUTE_READ||p==PAGE_EXECUTE_READWRITE||p==PAGE_EXECUTE_WRITECOPY;
        if(!canRead||(executable&&!canExecute))return false;
        const auto next=reinterpret_cast<std::uintptr_t>(info.BaseAddress)+info.RegionSize;
        if(next<=address)return false;address=next;
    }
    return true;
}
template<class T> bool read(std::uintptr_t address,T& output) noexcept {
    if(!readable(address,sizeof(T)))return false;
    std::memcpy(&output,reinterpret_cast<const void*>(address),sizeof(T));return true;
}
void emit(const char* name,std::uintptr_t task,std::uint64_t epoch,std::uint32_t a=0,std::uint32_t b=0) noexcept {
    auto cb=state().log;if(!cb||insideLog)return;
    insideLog=true;
    char line[240]{};
    std::snprintf(line,sizeof(line),"native,%s,tid=%lu,task=0x%llx,epoch=%llu,a=0x%x,b=0x%x",name,static_cast<unsigned long>(GetCurrentThreadId()),static_cast<unsigned long long>(task),static_cast<unsigned long long>(epoch),a,b);
    cb(line);insideLog=false;
}
bool task_valid(void* object,bool derivedOnly=false) noexcept {
    std::uintptr_t table=0;auto& s=state();
    if(!read(reinterpret_cast<std::uintptr_t>(object),table))return false;
    return table==s.base+TaskVtableRva||(!derivedOnly&&table==s.base+BaseVtableRva);
}
struct Observed {std::uint64_t epoch=0;bool valid=false;};
void clear_owner_locked(State& s) noexcept {
    s.status.ownedPauseToken=0;s.ownedEpoch=0;s.ownedRevision=0;
}
Observed observe(void* object) noexcept {
    auto& s=state();const auto task=reinterpret_cast<std::uintptr_t>(object);
    if(!s.active||!task_valid(object)){
        std::lock_guard<std::mutex> lock(s.mutex);++s.status.invalidObjects;return {};
    }
    std::uintptr_t input=0,backend=0;std::uint8_t pause=0;
    const bool fields=read(task+0x170,input)&&read(task+0x238,backend)&&read(task+0x185,pause);
    if(!fields){std::lock_guard<std::mutex> lock(s.mutex);++s.status.invalidObjects;return {};}
    bool changed=false,pauseChanged=false;std::uint64_t epoch=0;
    {
        std::lock_guard<std::mutex> lock(s.mutex);auto& st=s.status;
        changed=!st.taskObserved||st.task!=task||s.input!=input||s.backend!=backend;
        if(changed){++st.taskEpoch;++st.pauseRevision;clear_owner_locked(s);st.task=task;st.taskObserved=true;s.input=input;s.backend=backend;}
        pauseChanged=st.pauseKnown&&st.paused!=(pause!=0);
        if(pauseChanged){++st.pauseRevision;clear_owner_locked(s);}
        // Any value change or new task ends "the pause is ours"; our own
        // Acquire re-marks it after observing its transition.
        if(changed||pauseChanged)s.modPause=false;
        st.pauseKnown=true;st.paused=pause!=0;epoch=st.taskEpoch;
    }
    if(changed)emit("task",task,epoch,pause,static_cast<std::uint32_t>(backend!=0));
    if(pauseChanged)emit("pause-field-change",task,epoch,pause);
    return {epoch,true};
}
void invalidate_object(void* object,const char* why) noexcept {
    auto& s=state();std::uint64_t epoch=0;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if(s.status.task==reinterpret_cast<std::uintptr_t>(object)){
            ++s.status.taskEpoch;++s.status.pauseRevision;clear_owner_locked(s);
            s.status.taskObserved=false;s.status.pauseKnown=false;s.input=0;s.backend=0;s.modPause=false;
        }
        epoch=s.status.taskEpoch;
    }
    emit(why,reinterpret_cast<std::uintptr_t>(object),epoch);
}
void service_pause_request(void* object) noexcept {
    auto& s=state();PauseRequest request{};std::uint64_t revision=0,epoch=0;bool targetPause=false;
    std::lock_guard<std::recursive_mutex> execution(s.pauseExecutionMutex);
    {
        std::lock_guard<std::mutex> lock(s.mutex);auto& st=s.status;
        if(!s.hasPending||!st.pauseCommandsEnabled||st.pauseThread!=GetCurrentThreadId())return;
        request=s.pending;s.hasPending=false;
        s.reply={request.requestId,PauseReplyCode::Rejected,0,st.taskEpoch,st.pauseRevision};
        if(!st.taskObserved||st.task!=reinterpret_cast<std::uintptr_t>(object)||!st.pauseKnown)return;
        if(request.expectedTaskEpoch!=st.taskEpoch||request.expectedRevision!=st.pauseRevision){s.reply.code=PauseReplyCode::Superseded;return;}
        if(request.kind==PauseCommandKind::Acquire){
            if(st.paused){s.reply.code=PauseReplyCode::AlreadyPaused;return;}
            if(st.ownedPauseToken)return;
            targetPause=true;
        }else if(request.kind==PauseCommandKind::ReleaseStale){
            // Only a pause we created that nobody owns or has unpaused since.
            if(!s.modPause||!st.paused||st.ownedPauseToken){s.reply.code=PauseReplyCode::Superseded;return;}
        }else{
            if(!request.leaseToken||request.leaseToken!=st.ownedPauseToken||s.ownedEpoch!=st.taskEpoch||s.ownedRevision!=st.pauseRevision||!st.paused){s.reply.code=PauseReplyCode::Superseded;return;}
            clear_owner_locked(s);
        }
        epoch=st.taskEpoch;revision=++st.pauseRevision;s.pauseInFlight=true;
    }
    // The trampoline bypasses our external-request hook. All calls that enter
    // onSetPause still invalidate ownership, including same-value requests.
    s.setPause(object,targetPause?1:0);
    const auto actual=s.getPause(object);observe(object);
    {
        std::lock_guard<std::mutex> lock(s.mutex);auto& st=s.status;s.pauseInFlight=false;
        s.reply={request.requestId,PauseReplyCode::Failed,0,st.taskEpoch,st.pauseRevision};
        // A successful value transition adds exactly one observation revision.
        // Any intervening external setter or lifetime boundary invalidates it.
        if(st.taskEpoch!=epoch||st.pauseRevision!=revision+1){s.reply.code=PauseReplyCode::Superseded;return;}
        if((actual!=0)!=targetPause)return;
        s.reply.code=targetPause?PauseReplyCode::Acquired:PauseReplyCode::Released;
        s.modPause=targetPause;
        if(targetPause){
            if(++s.nextToken==0)++s.nextToken;
            st.ownedPauseToken=s.nextToken;s.ownedEpoch=st.taskEpoch;s.ownedRevision=st.pauseRevision;
            s.reply.leaseToken=st.ownedPauseToken;
        }
    }
    emit(targetPause?"mod-pause-acquired":request.kind==PauseCommandKind::ReleaseStale?"mod-stale-pause-released":"mod-pause-released",reinterpret_cast<std::uintptr_t>(object),epoch,static_cast<std::uint32_t>(request.requestId));
}
// Logs each distinct caller of a pause method once, as an RVA when inside the
// image, to map which collection code pauses/unpauses (diagnostic only).
void note_caller(std::array<std::uintptr_t,16>& seen,const char* name,void* object,const void* caller) noexcept {
    auto& s=state();const auto address=reinterpret_cast<std::uintptr_t>(caller);bool fresh=false;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        for(auto& slot:seen){if(slot==address)break;if(!slot){slot=address;fresh=true;break;}}
    }
    if(!fresh)return;
    const bool inside=address>=s.base&&address<s.base+s.imageSize;
    emit(name,reinterpret_cast<std::uintptr_t>(object),0,static_cast<std::uint32_t>(inside?address-s.base:0),inside?1:0);
}
void onDraw(void* object,std::uint32_t index){
    auto& s=state();const auto observed=observe(object);const auto previous=currentScope;
    currentScope={};std::uintptr_t screen=0,vtable=0;
    // The real method indexes this array without a bounds check. Only inspect
    // an index supplied by the live caller; +0x4a8 is NOT a proven array count.
    // The 16-item ceiling is our diagnostic bound, not a claim about the game.
    if(observed.valid&&task_valid(object,true)&&index<16&&read(reinterpret_cast<std::uintptr_t>(object)+0x4b0+sizeof(void*)*index,screen)&&read(screen,vtable)&&vtable==s.base+ScreenVtableRva){
        currentScope={reinterpret_cast<std::uintptr_t>(object),screen,observed.epoch,index,GetCurrentThreadId(),true};
    }
    std::uint64_t calls=0;
    {std::lock_guard<std::mutex> lock(s.mutex);calls=++s.status.drawCalls;s.status.drawThread=GetCurrentThreadId();if(currentScope.valid&&s.status.screenCount<index+1)s.status.screenCount=index+1;}
    if(calls<=24||calls%600==0)emit(currentScope.valid?"draw":"draw-unclassified",reinterpret_cast<std::uintptr_t>(object),observed.epoch,index,currentScope.valid?1:0);
    s.draw(object,index);currentScope=previous;
}
void onEndInput(void* object){
    auto& s=state();s.endInput(object);const auto observed=observe(object);std::uint64_t calls=0;
    {std::lock_guard<std::mutex> lock(s.mutex);calls=++s.status.inputUpdates;s.status.inputThread=GetCurrentThreadId();}
    if(calls<=8||calls%600==0)emit("end-input",reinterpret_cast<std::uintptr_t>(object),observed.epoch,static_cast<std::uint32_t>(calls));
}
std::uint8_t onGetPause(void* object){
    auto& s=state();note_caller(s.getCallers,"get-pause-caller",object,__builtin_return_address(0));
    auto result=s.getPause(object);const auto observed=observe(object);std::uint64_t calls=0;
    {std::lock_guard<std::mutex> lock(s.mutex);calls=++s.status.pauseReads;if(observed.valid&&!s.status.pauseThread)s.status.pauseThread=GetCurrentThreadId();}
    if(observed.valid){service_pause_request(object);result=s.getPause(object);}
    if(calls<=8||calls%1200==0)emit("get-pause",reinterpret_cast<std::uintptr_t>(object),observed.epoch,result);
    return result;
}
void onSetPause(void* object,std::uint8_t value){
    auto& s=state();note_caller(s.setCallers,"set-pause-caller",object,__builtin_return_address(0));
    std::lock_guard<std::recursive_mutex> execution(s.pauseExecutionMutex);const auto observed=observe(object);
    {std::lock_guard<std::mutex> lock(s.mutex);++s.status.pauseWrites;++s.status.pauseRevision;clear_owner_locked(s);}
    emit("set-pause-before",reinterpret_cast<std::uintptr_t>(object),observed.epoch,value);
    s.setPause(object,value);
    const auto after=observe(object);emit("set-pause-after",reinterpret_cast<std::uintptr_t>(object),after.epoch,value);
}
void* onCtor(void* object){auto& s=state();std::lock_guard<std::recursive_mutex> execution(s.pauseExecutionMutex);invalidate_object(object,"construct");auto* result=s.ctor(object);observe(result);return result;}
void* onDtor(void* object,std::uint32_t flags){auto& s=state();std::lock_guard<std::recursive_mutex> execution(s.pauseExecutionMutex);invalidate_object(object,"destroy");return s.dtor(object,flags);}
std::uint8_t onReinit(void* object,std::uint32_t mode,std::uintptr_t a,
                      std::uintptr_t b,std::uintptr_t c,std::uint32_t flags,
                      std::uint32_t d,std::uint32_t e){
    auto& s=state();std::lock_guard<std::recursive_mutex> execution(s.pauseExecutionMutex);invalidate_object(object,"reinitialize");
    const auto result=s.reinit(object,mode,a,b,c,flags,d,e);observe(object);return result;
}
// REA/Ghidra + native assembly corroborate these Win64 ABIs. On/Pressed use
// the second argument as a pad/group index, while Total* use it as a mask.
// Every returned bit is forwarded unchanged; observing a bit is not proof that
// it means View/Y, or that all other input consumers traverse these getters.
std::uint32_t query(void* object,std::uint32_t argument,QueryFn original,
                    std::size_t channel,const char* label){
    const auto result=original(object,argument);const auto observed=observe(object);
    auto& s=state();bool shouldLog=false;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        ++s.status.normalizedQueries;
        const auto calls=++s.queryCounts[channel];
        const bool changed=calls==1||s.lastQueryArgument[channel]!=argument||s.lastQueryResult[channel]!=result;
        s.lastQueryArgument[channel]=argument;s.lastQueryResult[channel]=result;
        switch(channel){
            case 0:s.status.lastNormalizedOn=result;break;
            case 1:s.status.lastNormalizedPressed=result;break;
            case 2:s.status.lastTotalOn=result;break;
            case 3:s.status.lastTotalPressed=result;break;
        }
        const auto window=GetTickCount64()/1000;
        if(window!=s.queryLogWindow){s.queryLogWindow=window;s.queryLogsInWindow=0;}
        const bool candidate=calls<=8||changed||calls%1200==0;
        if(candidate&&s.queryLogsInWindow<128){++s.queryLogsInWindow;shouldLog=true;}
        else if(candidate)++s.status.queryLogDrops;
    }
    if(shouldLog)emit(label,reinterpret_cast<std::uintptr_t>(object),observed.epoch,argument,result);
    return result;
}
std::uint32_t onOn(void* o,std::uint32_t a){return query(o,a,state().on,0,"query-on-pad");}
std::uint32_t onPressed(void* o,std::uint32_t a){return query(o,a,state().pressed,1,"query-pressed-pad");}
std::uint32_t onTotalOn(void* o,std::uint32_t a){return query(o,a,state().totalOn,2,"query-total-on-mask");}
std::uint32_t onTotalPressed(void* o,std::uint32_t a){return query(o,a,state().totalPressed,3,"query-total-pressed-mask");}

struct HookDefinition {std::uint32_t rva;const unsigned char* bytes;std::size_t length;void* replacement;void** original;};
constexpr unsigned char drawBytes[]={0x8b,0xc2,0x48,0x8b,0x8c,0xc1,0xb0,0x04,0,0,0x48,0x8b,0x01,0x48,0xff,0xa0,0x98,0,0,0};
constexpr unsigned char endBytes[]={0x48,0x8b,0x89,0x70,0x01,0,0,0xc6,0x41,0x2b,0x01,0xe9,0x40,0x51,0xff,0xff};
constexpr unsigned char getBytes[]={0x0f,0xb6,0x81,0x85,0x01,0,0,0xc3};
constexpr unsigned char setBytes[]={0x48,0x89,0x5c,0x24,0x10,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9,0x0f,0xb6,0xfa,0x0f,0xb6,0x89,0x85,0x01,0,0,0x3a,0xca,0x0f,0x84,0xd2,0,0,0};
constexpr unsigned char onBytes[]={0x48,0x83,0xec,0x38,0x48,0x8b,0x89,0x70,0x01,0,0,0x45,0x33,0xc9,0xc7,0x44,0x24,0x28,0xff,0xff,0xff,0x1f,0x45,0x33,0xc0};
constexpr unsigned char pressedBytes[]={0x48,0x83,0xec,0x38,0x48,0x8b,0x89,0x70,0x01,0,0,0x45,0x33,0xc9,0xc7,0x44,0x24,0x28,0xff,0xff,0xff,0x1f,0x48,0xc7,0x44,0x24,0x20,0,0,0,0,0x45,0x8d,0x41,0x01};
constexpr unsigned char totalOnBytes[]={0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0x89,0x70,0x01,0,0,0x8b,0xda,0x33,0xd2,0xe8,0xca,0x4f,0xff,0xff};
constexpr unsigned char totalPressedBytes[]={0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0x89,0x70,0x01,0,0,0x8b,0xda,0xba,0x01,0,0,0,0xe8,0xa7,0x4f,0xff,0xff};
constexpr unsigned char ctorBytes[]={0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x4c,0x24,0x08,0x57,0x48,0x83,0xec,0x30,0x48,0x8b,0xd9,0xe8,0x09,0xa0,0xfe,0xff};
constexpr unsigned char dtorBytes[]={0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xec,0x20,0x48,0x8d,0x05,0x4a,0xb9,0x4e,0};
constexpr unsigned char reinitBytes[]={0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x6c,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x57,0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x20,0x49,0x8b,0xe9,0x4d,0x8b,0xf0,0x8b,0xda};

bool image_valid(HMODULE module) noexcept {
    auto& s=state();s.base=reinterpret_cast<std::uintptr_t>(module);
    IMAGE_DOS_HEADER dos{};if(!read(s.base,dos)||dos.e_magic!=IMAGE_DOS_SIGNATURE||dos.e_lfanew<0x40||dos.e_lfanew>0x100000)return false;
    IMAGE_NT_HEADERS64 nt{};if(!read(s.base+dos.e_lfanew,nt)||nt.Signature!=IMAGE_NT_SIGNATURE||nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64||nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC)return false;
    s.imageSize=nt.OptionalHeader.SizeOfImage;if(s.imageSize<0x590000||s.imageSize>0x8000000)return false;
    std::uintptr_t draw=0,set=0,child=0;
    return read(s.base+TaskVtableRva+0x58,draw)&&draw==s.base+DrawRva&&read(s.base+TaskVtableRva+0x10,set)&&set==s.base+SetPauseRva&&read(s.base+ScreenVtableRva+0x98,child)&&child==s.base+0x339e0;
}
} // namespace

bool initialize(HMODULE host,LogCallback log) noexcept {
    auto& s=state();std::lock_guard<std::mutex> lock(s.mutex);
    if(s.status.initialized)return s.status.hooksInstalled;
    s.status.initialized=true;s.log=log;
    if(!image_valid(host)){emit("image-rejected",0,0);return false;}
    const HookDefinition hooks[]={
        {DrawRva,drawBytes,sizeof(drawBytes),reinterpret_cast<void*>(&onDraw),reinterpret_cast<void**>(&s.draw)},
        {EndInputRva,endBytes,sizeof(endBytes),reinterpret_cast<void*>(&onEndInput),reinterpret_cast<void**>(&s.endInput)},
        {GetPauseRva,getBytes,sizeof(getBytes),reinterpret_cast<void*>(&onGetPause),reinterpret_cast<void**>(&s.getPause)},
        {SetPauseRva,setBytes,sizeof(setBytes),reinterpret_cast<void*>(&onSetPause),reinterpret_cast<void**>(&s.setPause)},
        {OnRva,onBytes,sizeof(onBytes),reinterpret_cast<void*>(&onOn),reinterpret_cast<void**>(&s.on)},
        {PressedRva,pressedBytes,sizeof(pressedBytes),reinterpret_cast<void*>(&onPressed),reinterpret_cast<void**>(&s.pressed)},
        {TotalOnRva,totalOnBytes,sizeof(totalOnBytes),reinterpret_cast<void*>(&onTotalOn),reinterpret_cast<void**>(&s.totalOn)},
        {TotalPressedRva,totalPressedBytes,sizeof(totalPressedBytes),reinterpret_cast<void*>(&onTotalPressed),reinterpret_cast<void**>(&s.totalPressed)},
        {CtorRva,ctorBytes,sizeof(ctorBytes),reinterpret_cast<void*>(&onCtor),reinterpret_cast<void**>(&s.ctor)},
        {DtorRva,dtorBytes,sizeof(dtorBytes),reinterpret_cast<void*>(&onDtor),reinterpret_cast<void**>(&s.dtor)},
        {ReinitRva,reinitBytes,sizeof(reinitBytes),reinterpret_cast<void*>(&onReinit),reinterpret_cast<void**>(&s.reinit)}
    };
    for(const auto& h:hooks){const auto target=s.base+h.rva;if(h.rva>=s.imageSize||h.length>s.imageSize-h.rva||!readable(target,h.length,true)||std::memcmp(reinterpret_cast<void*>(target),h.bytes,h.length)){emit("signature-rejected",target,0,h.rva);return false;}}
    s.status.signaturesMatched=true;
    const auto mh=MH_Initialize();if(mh!=MH_OK&&mh!=MH_ERROR_ALREADY_INITIALIZED){emit("minhook-init-failed",0,0,mh);return false;}
    std::size_t made=0;
    for(const auto& h:hooks){void* target=reinterpret_cast<void*>(s.base+h.rva);const auto result=MH_CreateHook(target,h.replacement,h.original);if(result!=MH_OK){emit("hook-create-failed",reinterpret_cast<std::uintptr_t>(target),0,result);break;}s.targets[made++]=target;}
    if(made==s.targets.size()){
        bool queued=true;for(auto target:s.targets)if(MH_QueueEnableHook(target)!=MH_OK)queued=false;
        // No callback may run while initialize holds the snapshot mutex.
        if(queued){s.active=true;const auto result=MH_ApplyQueued();if(result==MH_OK){s.status.hooksInstalled=true;s.status.lifecycleHooksInstalled=true;emit("observation-enabled",s.base,0,static_cast<std::uint32_t>(made));return true;}s.active=false;emit("hook-enable-failed",0,0,result);}
    }
    for(auto target:s.targets)if(target){MH_DisableHook(target);MH_RemoveHook(target);}
    s.targets.fill(nullptr);return false;
}
bool task_ready(HMODULE host) noexcept {
    // The constructor stores `this` here (0x140026dd8: mov [0x140599fd0],rbx),
    // the destructor overwrites it (0x140026fa9) and reinitialize stores the
    // new backend at +0x238 (0x140017652).
    constexpr std::uint32_t TaskGlobalRva=0x599fd0;
    const auto base=reinterpret_cast<std::uintptr_t>(host);std::uintptr_t task=0,table=0,backend=0;
    return host&&read(base+TaskGlobalRva,task)&&task&&read(task,table)&&table==base+TaskVtableRva&&read(task+0x238,backend)&&backend;
}
Status status_snapshot() noexcept {
    auto& s=state();std::lock_guard<std::mutex> lock(s.mutex);auto st=s.status;
    st.stalePause=s.modPause&&st.paused&&!st.ownedPauseToken;return st;
}
DrawScope draw_scope() noexcept {return currentScope;}
bool enable_pause_commands(bool enabled) noexcept {
    auto& s=state();std::lock_guard<std::mutex> lock(s.mutex);
    if(enabled&&(!s.status.hooksInstalled||!s.status.lifecycleHooksInstalled))return false;
    if(!enabled&&(s.hasPending||s.pauseInFlight||s.status.ownedPauseToken))return false;
    s.status.pauseCommandsEnabled=enabled;s.status.observationOnly=!enabled;return true;
}
bool queue_pause_request(const PauseRequest& request) noexcept {
    auto& s=state();std::lock_guard<std::mutex> lock(s.mutex);
    if(!s.status.pauseCommandsEnabled||!s.status.taskObserved||!s.status.pauseThread||s.hasPending||s.pauseInFlight||!request.requestId)return false;
    if(request.kind!=PauseCommandKind::Acquire&&request.kind!=PauseCommandKind::Release&&request.kind!=PauseCommandKind::ReleaseStale)return false;
    if(request.requestId==s.reply.requestId)return false;
    s.pending=request;s.hasPending=true;return true;
}
bool release_owned_now() noexcept {
    auto& s=state();std::uintptr_t task=0;std::uint64_t epoch=0;
    std::lock_guard<std::recursive_mutex> execution(s.pauseExecutionMutex);
    {
        std::lock_guard<std::mutex> lock(s.mutex);auto& st=s.status;
        if(!s.active||!st.pauseCommandsEnabled||s.pauseInFlight||!st.ownedPauseToken||!st.paused||!st.taskObserved)return false;
        if(st.pauseThread!=GetCurrentThreadId()||s.ownedEpoch!=st.taskEpoch||s.ownedRevision!=st.pauseRevision)return false;
        if(!task_valid(reinterpret_cast<void*>(st.task),true))return false;
        task=st.task;clear_owner_locked(s);epoch=st.taskEpoch;++st.pauseRevision;s.pauseInFlight=true;
    }
    auto* object=reinterpret_cast<void*>(task);
    s.setPause(object,0);
    const auto actual=s.getPause(object);observe(object);
    {std::lock_guard<std::mutex> lock(s.mutex);s.pauseInFlight=false;if(!actual)s.modPause=false;}
    emit(actual?"mod-pause-release-now-failed":"mod-pause-released-now",task,epoch,actual);
    return !actual;
}
PauseReply pause_reply_snapshot() noexcept {auto& s=state();std::lock_guard<std::mutex> lock(s.mutex);return s.reply;}
void shutdown() noexcept {
    auto& s=state();s.active=false;
    for(auto target:s.targets)if(target)MH_QueueDisableHook(target);
    MH_ApplyQueued();
    for(auto target:s.targets)if(target)MH_RemoveHook(target);
    std::lock_guard<std::mutex> lock(s.mutex);s.targets.fill(nullptr);s.status.hooksInstalled=false;s.status.pauseCommandsEnabled=false;s.status.lifecycleHooksInstalled=false;s.hasPending=false;
}
} // namespace dominus::native
