#include "dominus/resource_observer.hpp"
#include "dominus/native_runtime.hpp"
#include "MinHook.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <mutex>

namespace dominus::resources {
namespace {
using UpdateFn=void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,UINT,const D3D11_BOX*,const void*,UINT,UINT);
using MapFn=HRESULT (STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,UINT,D3D11_MAP,UINT,D3D11_MAPPED_SUBRESOURCE*);
using UnmapFn=void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,UINT);
using CopyFn=void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,ID3D11Resource*);
using SrvFn=void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,ID3D11ShaderResourceView* const*);
using DrawFn=void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT);
using DrawIndexedFn=void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,INT);
struct State {
    std::mutex mutex;
    Log log=nullptr;
    bool initialized=false;
    std::array<void*,7> targets{};
    UpdateFn update=nullptr; MapFn map=nullptr; UnmapFn unmap=nullptr; CopyFn copy=nullptr;
    SrvFn srv=nullptr; DrawFn draw=nullptr; DrawIndexedFn drawIndexed=nullptr;
    std::atomic<unsigned> budget{0};
    std::atomic<unsigned long long> frame{0};
};
State& state(){static auto* s=new State;return *s;}
thread_local bool inside=false;
struct Key {std::uintptr_t resource=0;std::uint64_t epoch=0;std::uint32_t index=0;const char* op=nullptr;};
thread_local std::array<Key,64> recent{};
thread_local std::uint64_t recentFrame=~std::uint64_t{};
thread_local unsigned recentCount=0;

bool eligible(const char* op,ID3D11Resource* resource,native::DrawScope& scope) noexcept {
    if(inside || !state().log) return false;
    scope=native::draw_scope(); if(!scope.valid)return false;
    const auto frame=state().frame.load(std::memory_order_relaxed);
    if(recentFrame!=frame){recentFrame=frame;recentCount=0;}
    for(unsigned i=0;i<recentCount;++i){const auto& k=recent[i];if(k.resource==reinterpret_cast<std::uintptr_t>(resource)&&k.epoch==scope.epoch&&k.index==scope.index&&k.op==op)return false;}
    if(state().budget.fetch_add(1,std::memory_order_relaxed)>=64)return false;
    if(recentCount<recent.size())recent[recentCount++]={reinterpret_cast<std::uintptr_t>(resource),scope.epoch,scope.index,op};
    return true;
}
void emit(const char* op,ID3D11Resource* resource,unsigned a=0,unsigned b=0,const void* related=nullptr) noexcept {
    native::DrawScope scope{};
    if(!eligible(op,resource,scope))return;
    inside=true;
    char desc[240]="none";
    if(resource){
        D3D11_RESOURCE_DIMENSION type=D3D11_RESOURCE_DIMENSION_UNKNOWN;resource->GetType(&type);
        if(type==D3D11_RESOURCE_DIMENSION_TEXTURE2D){
            ID3D11Texture2D* texture=nullptr;
            if(SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D),reinterpret_cast<void**>(&texture)))){
                D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);texture->Release();
                std::snprintf(desc,sizeof(desc),"tex2d=%ux%u,fmt=%u,mips=%u,array=%u,samples=%u,usage=%u,bind=0x%x,cpu=0x%x,misc=0x%x",d.Width,d.Height,unsigned(d.Format),d.MipLevels,d.ArraySize,d.SampleDesc.Count,unsigned(d.Usage),d.BindFlags,d.CPUAccessFlags,d.MiscFlags);
            }
        } else if(type==D3D11_RESOURCE_DIMENSION_BUFFER){
            ID3D11Buffer* buffer=nullptr;
            if(SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Buffer),reinterpret_cast<void**>(&buffer)))){
                D3D11_BUFFER_DESC d{};buffer->GetDesc(&d);buffer->Release();
                std::snprintf(desc,sizeof(desc),"buffer=%u,usage=%u,bind=0x%x,cpu=0x%x,misc=0x%x,stride=%u",d.ByteWidth,unsigned(d.Usage),d.BindFlags,d.CPUAccessFlags,d.MiscFlags,d.StructureByteStride);
            }
        } else std::snprintf(desc,sizeof(desc),"type=%u",unsigned(type));
    }
    char line[640]{};
    std::snprintf(line,sizeof(line),"resource,%s,frame=%llu,tid=%lu,task=%p,screen=%p,epoch=%llu,index=%u,res=%p,related=%p,a=%u,b=%u,%s",op,state().frame.load(std::memory_order_relaxed),static_cast<unsigned long>(GetCurrentThreadId()),reinterpret_cast<void*>(scope.task),reinterpret_cast<void*>(scope.screen),static_cast<unsigned long long>(scope.epoch),scope.index,resource,related,a,b,desc);
    state().log(line);inside=false;
}
void STDMETHODCALLTYPE onUpdate(ID3D11DeviceContext* c,ID3D11Resource* r,UINT sub,const D3D11_BOX* box,const void* data,UINT row,UINT depth){
    emit("update",r,sub,row,data);state().update(c,r,sub,box,data,row,depth);
}
HRESULT STDMETHODCALLTYPE onMap(ID3D11DeviceContext* c,ID3D11Resource* r,UINT sub,D3D11_MAP type,UINT flags,D3D11_MAPPED_SUBRESOURCE* output){
    const HRESULT result=state().map(c,r,sub,type,flags,output);
    emit("map",r,sub,unsigned(type),SUCCEEDED(result)&&output?output->pData:nullptr);return result;
}
void STDMETHODCALLTYPE onUnmap(ID3D11DeviceContext* c,ID3D11Resource* r,UINT sub){emit("unmap",r,sub);state().unmap(c,r,sub);}
void STDMETHODCALLTYPE onCopy(ID3D11DeviceContext* c,ID3D11Resource* dst,ID3D11Resource* src){emit("copy-dst",dst,0,0,src);emit("copy-src",src,0,0,dst);state().copy(c,dst,src);}
void STDMETHODCALLTYPE onSrv(ID3D11DeviceContext* c,UINT first,UINT count,ID3D11ShaderResourceView* const* views){
    if(native::draw_scope().valid&&views){
        for(UINT i=0;i<count&&i<16;++i)if(views[i]){
            ID3D11Resource* resource=nullptr;views[i]->GetResource(&resource);
            emit("ps-srv",resource,first+i,count,views[i]);if(resource)resource->Release();
        }
    }
    state().srv(c,first,count,views);
}
void STDMETHODCALLTYPE onDraw(ID3D11DeviceContext* c,UINT count,UINT start){emit("draw",nullptr,count,start);state().draw(c,count,start);}
void STDMETHODCALLTYPE onDrawIndexed(ID3D11DeviceContext* c,UINT count,UINT start,INT base){emit("draw-indexed",nullptr,count,start);state().drawIndexed(c,count,start,base);}
struct Hook {unsigned slot;void* replacement;void** original;};
}

bool initialize(ID3D11Device* device,Log log) noexcept {
    if(!device)return false;
    try {
        auto& s=state();std::lock_guard<std::mutex> lock(s.mutex);
        if(s.initialized)return true;
        s.log=log;
        ID3D11DeviceContext* context=nullptr;device->GetImmediateContext(&context);
        if(!context)return false;
        void** table=*reinterpret_cast<void***>(context);
        const Hook hooks[]={
            {48,reinterpret_cast<void*>(&onUpdate),reinterpret_cast<void**>(&s.update)},
            {14,reinterpret_cast<void*>(&onMap),reinterpret_cast<void**>(&s.map)},
            {15,reinterpret_cast<void*>(&onUnmap),reinterpret_cast<void**>(&s.unmap)},
            {47,reinterpret_cast<void*>(&onCopy),reinterpret_cast<void**>(&s.copy)},
            {8,reinterpret_cast<void*>(&onSrv),reinterpret_cast<void**>(&s.srv)},
            {13,reinterpret_cast<void*>(&onDraw),reinterpret_cast<void**>(&s.draw)},
            {12,reinterpret_cast<void*>(&onDrawIndexed),reinterpret_cast<void**>(&s.drawIndexed)}
        };
        const auto initialized=MH_Initialize();
        if(initialized!=MH_OK&&initialized!=MH_ERROR_ALREADY_INITIALIZED){context->Release();return false;}
        bool okay=true;unsigned made=0;
        for(const auto& hook:hooks){
            void* target=table[hook.slot];
            const auto result=MH_CreateHook(target,hook.replacement,hook.original);
            if(result!=MH_OK){char line[120]{};std::snprintf(line,sizeof(line),"resource,hook-create-failed,slot=%u,status=%d",hook.slot,int(result));if(log)log(line);okay=false;break;}
            s.targets[made++]=target;
        }
        context->Release();
        if(okay){
            for(void* target:s.targets)if(MH_QueueEnableHook(target)!=MH_OK)okay=false;
            if(okay)okay=MH_ApplyQueued()==MH_OK;
        }
        if(!okay){for(void* target:s.targets)if(target){MH_DisableHook(target);MH_RemoveHook(target);}s.targets.fill(nullptr);return false;}
        s.initialized=true;if(log)log("resource,observation-enabled,hooks=7,scope=DrawGpuScreen,budget=64/frame");return true;
    } catch (...) {if(log)log("resource,initialization-failed,exception");return false;}
}
void on_present() noexcept {state().budget=0;state().frame.fetch_add(1,std::memory_order_relaxed);}
void shutdown() noexcept {
    auto& s=state();std::lock_guard<std::mutex> lock(s.mutex);
    for(void* target:s.targets)if(target)MH_QueueDisableHook(target);
    MH_ApplyQueued();for(void* target:s.targets)if(target)MH_RemoveHook(target);
    s.targets.fill(nullptr);s.initialized=false;
}
}
