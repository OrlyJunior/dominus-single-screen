#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "dominus/pointer_remap.hpp"
#include "MinHook.h"
#include <atomic>
#include <cstdio>
#include <cstring>

namespace dominus::pointer {
namespace {
// game.exe E6FA885F...6B64: the DirectInput poll (keyboard, then the mouse
// buttons and cursor), called once per frame by 0x1401a2610.
constexpr std::uint32_t PollRva=0x1a2250,ButtonsRva=0x59a1f4,CursorXRva=0x59a1fc,CursorYRva=0x59a200,WindowRva=0x59e9c8;
constexpr std::int32_t Outside=-10000;
// Prologue through the security cookie and the "devices created" test.
constexpr unsigned char pollBytes[]={0x48,0x89,0x5c,0x24,0x10,0x55,0x48,0x8d,0xac,0x24,0x60,0xff,0xff,0xff,0x48,0x81,0xec,0xa0,0x01,0,0,0x48,0x8b,0x05,0xdc,0x69,0x3e,0,0x48,0x33,0xc4,0x48,0x89,0x85,0x90,0,0,0,0x33,0xdb,0x38,0x1d,0x2b,0x7f,0x3f,0,0x0f,0x84};
using PollFn=void (*)();
struct State {
    SRWLOCK lock=SRWLOCK_INIT;
    View view{};
    PointerReservation reservation;
    std::uintptr_t base=0;
    PollFn original=nullptr;
    void* target=nullptr;
    std::atomic<bool> active{false};
    Log log=nullptr;
    unsigned long long remapped=0,hidden=0;
};
State& state(){static auto* s=new State;return *s;}
void log(const char* line) noexcept {if(state().log)state().log(line);}
std::int32_t& at(std::uintptr_t rva) noexcept {return *reinterpret_cast<std::int32_t*>(state().base+rva);}
void onPoll(){
    auto& s=state();s.original();
    if(!s.active.load())return;
    View view{};
    AcquireSRWLockShared(&s.lock);view=s.view;ReleaseSRWLockShared(&s.lock);
    auto& x=at(CursorXRva);auto& y=at(CursorYRva);
    const bool held=(static_cast<std::uint32_t>(at(ButtonsRva))&0xff)!=0;
    if(s.reservation.update(view.hide,held)){
        if(x!=Outside||y!=Outside){x=y=Outside;if(++s.hidden<=8||s.hidden%600==0){char line[96]{};std::snprintf(line,sizeof(line),"pointer,hidden,count=%llu",s.hidden);log(line);}}
        return;
    }
    if(!view.remap||x==Outside||y==Outside)return;
    const auto window=*reinterpret_cast<HWND*>(s.base+WindowRva);RECT client{};
    if(!window||!GetClientRect(window,&client)){x=y=Outside;return;}
    const auto point=presented_to_original(view.shown,view.backWidth,view.backHeight,view.source,client.right-client.left,client.bottom-client.top,x,y);
    if(++s.remapped<=8||s.remapped%3600==0){char line[160]{};std::snprintf(line,sizeof(line),"pointer,remapped,from=%d/%d,to=%d/%d,inside=%u,count=%llu",x,y,point.x,point.y,unsigned(point.inside),s.remapped);log(line);}
    if(point.inside){x=point.x;y=point.y;}else x=y=Outside;
}
bool writable(std::uintptr_t address,std::size_t length) noexcept {
    MEMORY_BASIC_INFORMATION info{};
    if(!VirtualQuery(reinterpret_cast<void*>(address),&info,sizeof(info))||info.State!=MEM_COMMIT||(info.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
    const DWORD p=info.Protect&0xff;if(p!=PAGE_READWRITE&&p!=PAGE_WRITECOPY)return false;
    const auto end=reinterpret_cast<std::uintptr_t>(info.BaseAddress)+info.RegionSize;
    return address<end&&length<=end-address;
}
bool executable(std::uintptr_t address,const unsigned char* bytes,std::size_t length) noexcept {
    MEMORY_BASIC_INFORMATION info{};
    if(!VirtualQuery(reinterpret_cast<void*>(address),&info,sizeof(info))||info.State!=MEM_COMMIT||(info.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
    const DWORD p=info.Protect&0xff;if(p!=PAGE_EXECUTE_READ&&p!=PAGE_EXECUTE_READWRITE&&p!=PAGE_EXECUTE_WRITECOPY)return false;
    const auto end=reinterpret_cast<std::uintptr_t>(info.BaseAddress)+info.RegionSize;
    return address<end&&length<=end-address&&!std::memcmp(reinterpret_cast<const void*>(address),bytes,length);
}
}

bool initialize(void* host,Log output) noexcept {
    auto& s=state();
    if(s.active.load())return true;
    s.log=output;
    const auto base=reinterpret_cast<std::uintptr_t>(host);
    IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS64 nt{};
    if(!base)return false;
    std::memcpy(&dos,host,sizeof(dos));
    if(dos.e_magic!=IMAGE_DOS_SIGNATURE||dos.e_lfanew<0x40||dos.e_lfanew>0x1000){log("pointer,unavailable,image-header");return false;}
    std::memcpy(&nt,reinterpret_cast<const void*>(base+dos.e_lfanew),sizeof(nt));
    if(nt.Signature!=IMAGE_NT_SIGNATURE||nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64||nt.OptionalHeader.SizeOfImage<=WindowRva+8){log("pointer,unavailable,image-mismatch");return false;}
    if(!executable(base+PollRva,pollBytes,sizeof(pollBytes))||!writable(base+ButtonsRva,CursorYRva+4-ButtonsRva)){log("pointer,signature-rejected,rva=0x1a2250");return false;}
    const auto init=MH_Initialize();
    if(init!=MH_OK&&init!=MH_ERROR_ALREADY_INITIALIZED){log("pointer,unavailable,minhook-init-failed");return false;}
    void* target=reinterpret_cast<void*>(base+PollRva);s.base=base;
    if(MH_CreateHook(target,reinterpret_cast<void*>(&onPoll),reinterpret_cast<void**>(&s.original))!=MH_OK){log("pointer,hook-create-failed");return false;}
    s.target=target;s.active=true;
    if(MH_EnableHook(target)!=MH_OK){s.active=false;MH_RemoveHook(target);s.target=nullptr;s.original=nullptr;log("pointer,hook-enable-failed");return false;}
    log("pointer,observation-enabled,rva=0x1a2250,remap=presented-gameplay-to-original-panel");return true;
}
void set_view(const View& view) noexcept {
    auto& s=state();AcquireSRWLockExclusive(&s.lock);s.view=view;ReleaseSRWLockExclusive(&s.lock);
}
void shutdown() noexcept {
    auto& s=state();s.active=false;
    if(s.target){MH_DisableHook(s.target);MH_RemoveHook(s.target);s.target=nullptr;}
}
}
