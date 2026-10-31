#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>
#include "dominus/input_observer.hpp"
#include "MinHook.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>

namespace dominus::input {
namespace {
using GetStateFn=DWORD (WINAPI*)(DWORD,XINPUT_STATE*);
// game.exe E6FA885F...B6B64 only. RVA 0x22A920 is int(int pad,void* out): it
// finds pad in the id table 0x59E680 (0..7) and returns that slot or -1. Only
// when handle[slot] (0x59E5A0) and enabled[pad] (0x59E668) are nonzero does it
// zero out+4 and OR SteamInput006 InGameControls actions into it with XInput
// bits (select_button 0x20, y_button 0x8000); otherwise out+4 keeps the
// caller's XInput-derived word. The signature spans both global reads.
// Assembly REA ev_6b8a8467d8cc22d11d1a25a8d480e3c98a49ba371886edad5f7b20306cc4eb19.
constexpr std::uint32_t SteamPadRva=0x22a920,SteamHandlesRva=0x59e5a0,SteamEnabledRva=0x59e668;
constexpr unsigned char steamPadBytes[]={0x40,0x55,0x53,0x57,0x48,0x8b,0xec,0x48,0x81,0xec,0x80,0,0,0,0x48,0x8b,0x05,0x13,0xe3,0x35,0,0x48,0x33,0xc4,0x48,0x89,0x45,0xc8,0x48,0x8b,0xda,0x48,0x8d,0x05,0x3a,0x3d,0x37,0,0x33,0xd2,0x8b,0xfa,0x66,0x0f,0x1f,0x44,0,0,0x39,0x08,0x74,0x15,0xff,0xc7,0x48,0x83,0xc0,0x04,0x83,0xff,0x08,0x72,0xf1,0xb8,0xff,0xff,0xff,0xff,0xe9,0x1b,0x05,0,0,0x85,0xff,0x0f,0x88,0x2a,0x05,0,0,0x48,0x89,0xb4,0x24,0xb0,0,0,0,0x48,0x63,0xf7,0x4c,0x89,0xb4,0x24,0xb8,0,0,0,0x4c,0x8d,0x35,0x75,0x56,0xdd,0xff,0x49,0x39,0x94,0xf6,0xa0,0xe5,0x59,0,0x0f,0x84,0xd9,0x04,0,0,0x48,0x63,0xc1,0x42,0x38,0x94,0x30,0x68,0xe6,0x59,0,0x0f,0x84,0xc8,0x04,0,0};
using SteamPadFn=int (*)(int,void*);
struct State {
    std::mutex initializeMutex,mutex;
    std::array<Snapshot,4> slots{};
    std::array<bool,4> seen{};
    std::atomic<Log> log{nullptr};
    std::atomic<Filter> filter{nullptr};
    std::atomic<bool> active{false},reserved{false},steamActive{false};
    std::atomic<ULONGLONG> lastPoll{0};
    std::atomic<unsigned long long> filteredCalls{0};
    GetStateFn original=nullptr;
    void* target=nullptr;
    SteamPadFn steamOriginal=nullptr;
    void* steamTarget=nullptr;
    std::uintptr_t steamBase=0;
    // One physical controller usually fills every slot; key on its handle.
    std::uint64_t steamHandle=0;
    Snapshot steam{};
    bool steamSeen=false,steamOtherLogged=false;
    std::array<unsigned long long,8> steamPadCalls{};
};
State& state(){static auto* s=new State;return *s;}
void log(const char* message) noexcept {const auto output=state().log.load();if(output)output(message);}
// Keeps a pressed button only if the mask keeps it; View alone may be
// replayed. No other button is ever added.
WORD forward(WORD raw,WORD mask,unsigned controller,const char* source) noexcept {
    const WORD forwarded=static_cast<WORD>((raw&mask&~0x0020)|(mask&0x0020));
    if(forwarded!=raw){
        const auto count=state().filteredCalls.fetch_add(1)+1;
        if(count<=16||count%600==0){char line[160]{};std::snprintf(line,sizeof(line),"%s,filtered,slot=%u,raw=0x%04x,forwarded=0x%04x,count=%llu",source,controller,unsigned(raw),unsigned(forwarded),count);log(line);}
    }
    return forwarded;
}
Snapshot record(DWORD controller,DWORD result,const XINPUT_STATE* sample,bool gameCall) noexcept {
    if(controller>=4){Snapshot missing{};missing.controller=controller;missing.error=result;return missing;}
    auto& s=state();Snapshot observed{};bool changed=false;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        auto& slot=s.slots[controller];
        const bool connected=result==ERROR_SUCCESS&&sample;
        const WORD buttons=connected?sample->Gamepad.wButtons:0;
        changed=!s.seen[controller]||slot.connected!=connected||slot.buttons!=buttons||slot.error!=result;
        s.seen[controller]=true;
        slot.controller=controller;slot.connected=connected;slot.buttons=buttons;
        slot.packet=connected?sample->dwPacketNumber:0;slot.error=result;
        ++slot.sequence;slot.tick=GetTickCount64();if(gameCall)++slot.gameCalls;
        slot.reservedRequested=s.reserved.load();observed=slot;
    }
    if(changed){
        char line[240]{};
        std::snprintf(line,sizeof(line),"xinput,slot=%u,connected=%u,buttons=0x%04x,packet=%u,error=%u,sequence=%llu,gameCalls=%llu,source=%s",observed.controller,unsigned(observed.connected),unsigned(observed.buttons),observed.packet,observed.error,static_cast<unsigned long long>(observed.sequence),static_cast<unsigned long long>(observed.gameCalls),gameCall?"consumer":"poll");
        log(line);
    }
    return observed;
}
DWORD WINAPI onGetState(DWORD controller,XINPUT_STATE* output){
    const DWORD result=state().original(controller,output);
    if(state().active.load()){
        const Snapshot observed=record(controller,result,output,true);
        const auto filter=state().filter.load();
        if(filter){
            const WORD mask=filter(observed,true);
            if(result==ERROR_SUCCESS&&output&&controller<4)output->Gamepad.wButtons=forward(output->Gamepad.wButtons,mask,controller,"xinput");
        }
    }
    return result;
}
Snapshot record_steam(bool connected,WORD buttons,int pad) noexcept {
    auto& s=state();Snapshot observed{};bool changed=false;
    {
        std::lock_guard<std::mutex> lock(s.mutex);auto& slot=s.steam;
        changed=!s.steamSeen||slot.connected!=connected||slot.buttons!=buttons;s.steamSeen=true;
        slot.controller=SteamController;slot.connected=connected;slot.buttons=connected?buttons:0;
        slot.error=connected?ERROR_SUCCESS:ERROR_DEVICE_NOT_CONNECTED;
        ++slot.sequence;slot.tick=GetTickCount64();++slot.gameCalls;
        slot.reservedRequested=s.reserved.load();observed=slot;
    }
    if(changed){char line[200]{};std::snprintf(line,sizeof(line),"steam-pad,pad=%d,connected=%u,buttons=0x%04x,sequence=%llu,gameCalls=%llu",pad,unsigned(observed.connected),unsigned(observed.buttons),static_cast<unsigned long long>(observed.sequence),static_cast<unsigned long long>(observed.gameCalls));log(line);}
    return observed;
}
int onSteamPad(int pad,void* output){
    auto& s=state();const int slot=s.steamOriginal(pad,output);
    // The id table is the identity 0..7 (RVA 0x52D2A0 + 0/4), so slot==pad.
    if(!s.steamActive.load()||slot<0||slot>=8||slot!=pad||!output)return slot;
    std::array<std::uint64_t,16> handles{};std::uint8_t enabled=0;
    std::memcpy(handles.data(),reinterpret_cast<const void*>(s.steamBase+SteamHandlesRva),sizeof(handles));
    std::memcpy(&enabled,reinterpret_cast<const void*>(s.steamBase+SteamEnabledRva+slot),sizeof(enabled));
    const auto handle=handles[slot];
    bool lost=false,tracked=false,logOther=false,firstCall=false;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        firstCall=++s.steamPadCalls[slot]==1;
        // RVA 0x229DE0 clears every slot of a vanished handle; one empty slot
        // alone is not a disconnection of the tracked controller.
        if(!handle&&s.steamHandle){lost=true;for(const auto other:handles)if(other==s.steamHandle)lost=false;if(lost)s.steamHandle=0;}
        // Without both conditions out+4 still holds the caller's XInput word.
        else if(handle&&enabled){
            if(!s.steamHandle)s.steamHandle=handle;
            tracked=handle==s.steamHandle;
            if(!tracked&&!s.steamOtherLogged)logOther=s.steamOtherLogged=true;
        }
    }
    if(firstCall){char line[160]{};std::snprintf(line,sizeof(line),"steam-pad,first-call,pad=%d,handle=%llu,enabled=%u,caller=%p",pad,static_cast<unsigned long long>(handle),unsigned(enabled),__builtin_return_address(0));log(line);}
    const auto filter=s.filter.load();
    // Disconnection carries no button evidence; its returned mask is unused.
    if(lost){const auto observed=record_steam(false,0,pad);if(filter)(void)filter(observed,true);return slot;}
    if(logOther)log("steam-pad,untracked-handle-forwarded-unchanged");
    if(!tracked)return slot;
    auto* const buttons=static_cast<unsigned char*>(output)+4;
    WORD raw=0;std::memcpy(&raw,buttons,sizeof(raw));
    const auto observed=record_steam(true,raw,pad);
    if(!filter)return slot;
    const WORD forwarded=forward(raw,filter(observed,true),SteamController,"steam-pad");
    if(forwarded!=raw)std::memcpy(buttons,&forwarded,sizeof(forwarded));
    return slot;
}
bool executable_bytes(std::uintptr_t address,const unsigned char* bytes,std::size_t length) noexcept {
    MEMORY_BASIC_INFORMATION info{};
    if(!VirtualQuery(reinterpret_cast<void*>(address),&info,sizeof(info))||info.State!=MEM_COMMIT||(info.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
    const DWORD p=info.Protect&0xff;if(p!=PAGE_EXECUTE_READ&&p!=PAGE_EXECUTE_READWRITE&&p!=PAGE_EXECUTE_WRITECOPY)return false;
    const auto end=reinterpret_cast<std::uintptr_t>(info.BaseAddress)+info.RegionSize;
    return address<end&&length<=end-address&&!std::memcmp(reinterpret_cast<const void*>(address),bytes,length);
}
bool readable_data(std::uintptr_t address,std::size_t length) noexcept {
    MEMORY_BASIC_INFORMATION info{};
    if(!VirtualQuery(reinterpret_cast<void*>(address),&info,sizeof(info))||info.State!=MEM_COMMIT||(info.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
    const DWORD p=info.Protect&0xff;if(p!=PAGE_READONLY&&p!=PAGE_READWRITE&&p!=PAGE_WRITECOPY)return false;
    const auto end=reinterpret_cast<std::uintptr_t>(info.BaseAddress)+info.RegionSize;
    return address<end&&length<=end-address;
}
}

bool initialize(Log output) noexcept {
    auto& s=state();
    try {
        std::lock_guard<std::mutex> lock(s.initializeMutex);
        if(s.active.load())return true;
        s.log=output;
        HMODULE module=GetModuleHandleW(L"xinput1_4.dll");
        if(!module){log("xinput,observer-unavailable,xinput1_4.dll-not-loaded");return false;}
        wchar_t system[32768]{},actual[32768]{};
        const UINT directoryLength=GetSystemDirectoryW(system,32768);
        const DWORD fileLength=GetModuleFileNameW(module,actual,32768);
        if(!directoryLength||directoryLength>=32768||!fileLength||fileLength>=32768){log("xinput,observer-unavailable,module-path-failed");return false;}
        const std::wstring expected=std::wstring(system)+L"\\xinput1_4.dll";
        if(_wcsicmp(expected.c_str(),actual)){log("xinput,observer-unavailable,non-system-module-rejected");return false;}
        void* target=reinterpret_cast<void*>(GetProcAddress(module,"XInputGetState"));
        if(!target){log("xinput,observer-unavailable,XInputGetState-missing");return false;}
        const auto init=MH_Initialize();
        if(init!=MH_OK&&init!=MH_ERROR_ALREADY_INITIALIZED){log("xinput,observer-unavailable,minhook-init-failed");return false;}
        const auto created=MH_CreateHook(target,reinterpret_cast<void*>(&onGetState),reinterpret_cast<void**>(&s.original));
        if(created!=MH_OK){char line[120]{};std::snprintf(line,sizeof(line),"xinput,hook-create-failed,status=%d",int(created));log(line);return false;}
        s.target=target;s.active=true;
        if(MH_EnableHook(target)!=MH_OK){s.active=false;MH_RemoveHook(target);s.target=nullptr;s.original=nullptr;log("xinput,hook-enable-failed");return false;}
        log("xinput,observation-enabled,module=system32/xinput1_4.dll,filtering=callback-opt-in");return true;
    } catch (...) {log("xinput,observer-initialization-failed,exception");return false;}
}
bool initialize_steam(void* host,Log output) noexcept {
    auto& s=state();
    try {
        std::lock_guard<std::mutex> lock(s.initializeMutex);
        if(s.steamActive.load())return true;
        if(output)s.log=output;
        const auto base=reinterpret_cast<std::uintptr_t>(host);
        if(!base||!readable_data(base,sizeof(IMAGE_DOS_HEADER))){log("steam-pad,unavailable,image-header");return false;}
        IMAGE_DOS_HEADER dos{};std::memcpy(&dos,host,sizeof(dos));
        IMAGE_NT_HEADERS64 nt{};
        if(dos.e_magic!=IMAGE_DOS_SIGNATURE||dos.e_lfanew<0x40||dos.e_lfanew>0x1000||!readable_data(base+dos.e_lfanew,sizeof(nt))){log("steam-pad,unavailable,image-header");return false;}
        std::memcpy(&nt,reinterpret_cast<const void*>(base+dos.e_lfanew),sizeof(nt));
        if(nt.Signature!=IMAGE_NT_SIGNATURE||nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64||nt.OptionalHeader.SizeOfImage<=SteamEnabledRva+8){log("steam-pad,unavailable,image-mismatch");return false;}
        if(!executable_bytes(base+SteamPadRva,steamPadBytes,sizeof(steamPadBytes))||!readable_data(base+SteamHandlesRva,16*sizeof(std::uint64_t))||!readable_data(base+SteamEnabledRva,8)){log("steam-pad,signature-rejected,rva=0x22a920");return false;}
        const auto init=MH_Initialize();
        if(init!=MH_OK&&init!=MH_ERROR_ALREADY_INITIALIZED){log("steam-pad,unavailable,minhook-init-failed");return false;}
        void* target=reinterpret_cast<void*>(base+SteamPadRva);s.steamBase=base;
        const auto created=MH_CreateHook(target,reinterpret_cast<void*>(&onSteamPad),reinterpret_cast<void**>(&s.steamOriginal));
        if(created!=MH_OK){char line[120]{};std::snprintf(line,sizeof(line),"steam-pad,hook-create-failed,status=%d",int(created));log(line);return false;}
        s.steamTarget=target;s.steamActive=true;
        if(MH_EnableHook(target)!=MH_OK){s.steamActive=false;MH_RemoveHook(target);s.steamTarget=nullptr;s.steamOriginal=nullptr;log("steam-pad,hook-enable-failed");return false;}
        log("steam-pad,observation-enabled,rva=0x22a920,signature=136+2,filtering=callback-opt-in,no-steam-api-calls");return true;
    } catch (...) {log("steam-pad,initialization-failed,exception");return false;}
}

void on_present() noexcept {
    auto& s=state();if(!s.active.load()||!s.original)return;
    const ULONGLONG now=GetTickCount64();auto previous=s.lastPoll.load();
    if(now-previous<12||!s.lastPoll.compare_exchange_strong(previous,now))return;
    for(DWORD controller=0;controller<4;++controller){
        XINPUT_STATE sample{};const DWORD result=s.original(controller,&sample);
        const Snapshot observed=record(controller,result,&sample,false);
        const auto filter=s.filter.load();if(filter)(void)filter(observed,false);
    }
}
Snapshot snapshot(std::uint32_t controller) noexcept {
    auto& s=state();std::lock_guard<std::mutex> lock(s.mutex);
    if(controller==UINT32_MAX){for(const auto& slot:s.slots)if(slot.connected)return slot;return s.slots[0];}
    if(controller<4)return s.slots[controller];
    if(controller==SteamController)return s.steam;
    Snapshot missing{};missing.controller=controller;missing.error=ERROR_BAD_ARGUMENTS;return missing;
}
void set_mod_reserved(bool requested) noexcept {state().reserved=requested;}
void set_filter(Filter filter) noexcept {state().filter=filter;}
void shutdown() noexcept {
    auto& s=state();std::lock_guard<std::mutex> lock(s.initializeMutex);s.active=false;
    if(s.target){MH_DisableHook(s.target);MH_RemoveHook(s.target);s.target=nullptr;}
    s.steamActive=false;
    if(s.steamTarget){MH_DisableHook(s.steamTarget);MH_RemoveHook(s.steamTarget);s.steamTarget=nullptr;}
}
}
