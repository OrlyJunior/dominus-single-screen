#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3dcompiler.h>
#include <d3d11_1.h>
#include "dominus/graphics.hpp"
#include "dominus/core.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace dominus::gfx {
namespace {
template<class T> class Ref {
    T* p_=nullptr;
public:
    Ref()=default;
    ~Ref(){ reset(); }
    Ref(const Ref&)=delete;
    Ref& operator=(const Ref&)=delete;
    Ref(Ref&& r) noexcept : p_(std::exchange(r.p_,nullptr)) {}
    Ref& operator=(Ref&& r) noexcept { if(this!=&r){reset();p_=std::exchange(r.p_,nullptr);}return *this; }
    T* get() const {return p_;}
    T* operator->() const {return p_;}
    T** put(){reset();return &p_;}
    void reset(){if(p_){p_->Release();p_=nullptr;}}
    explicit operator bool()const{return p_!=nullptr;}
};
using CompileFn=HRESULT (WINAPI*)(LPCVOID,SIZE_T,LPCSTR,const D3D_SHADER_MACRO*,ID3DInclude*,LPCSTR,LPCSTR,UINT,UINT,ID3DBlob**,ID3DBlob**);
CompileFn compiler(){
    static CompileFn f=[](){
        wchar_t path[MAX_PATH+32]{};
        const UINT n=GetSystemDirectoryW(path,MAX_PATH);
        if(!n||n>=MAX_PATH)return static_cast<CompileFn>(nullptr);
        wcscat_s(path,L"\\d3dcompiler_47.dll");
        auto m=LoadLibraryExW(path,nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        return m?reinterpret_cast<CompileFn>(GetProcAddress(m,"D3DCompile")):nullptr;
    }();return f;
}
const char shader[]=R"(
cbuffer Source : register(b0) { float4 sourceRect; };
struct V { float4 position:SV_Position; float2 uv:TEXCOORD0; };
V vs(uint id:SV_VertexID) {
    float2 uv=float2((id<<1)&2,id&2);
    V o; o.position=float4(uv*float2(2,-2)+float2(-1,1),0,1);
    o.uv=sourceRect.xy+uv*sourceRect.zw; return o;
}
Texture2D image:register(t0); SamplerState pixel:register(s0);
float4 ps(V i):SV_Target { return image.Sample(pixel,i.uv); }
)";

// Own draws temporarily replace these states; the original game state is restored
// even if composition fails. No clear/draw occurs before every resource exists.
struct SavedState {
    ID3D11DeviceContext* c;
    std::array<ID3D11RenderTargetView*,8> rt{};
    ID3D11DepthStencilView* dsv=nullptr;
    ID3D11DepthStencilState* ds=nullptr; UINT stencil=0;
    ID3D11BlendState* blend=nullptr; FLOAT factor[4]{};UINT mask=0;
    ID3D11RasterizerState* raster=nullptr;
    std::array<D3D11_VIEWPORT,16> viewport{};UINT viewportCount=16;
    ID3D11InputLayout* layout=nullptr;D3D11_PRIMITIVE_TOPOLOGY topology{};
    ID3D11VertexShader* vs=nullptr;ID3D11PixelShader* ps=nullptr;
    ID3D11GeometryShader* gs=nullptr;ID3D11HullShader* hs=nullptr;ID3D11DomainShader* domain=nullptr;
    std::array<ID3D11ClassInstance*,256> vc{},pc{},gc{},hc{},dc{};
    UINT vn=256,pn=256,gn=256,hn=256,dn=256;
    ID3D11Buffer* cb=nullptr;ID3D11SamplerState* sampler=nullptr;
    Ref<ID3D11DeviceContext1> context1;UINT firstConstant=0,constantCount=0;
    std::array<ID3D11ShaderResourceView*,128> vsrv{},psrv{},gsrv{},hsrv{},dsrv{},csrv{};
    ID3D11Predicate* predicate=nullptr;BOOL predicateValue=FALSE;
    explicit SavedState(ID3D11DeviceContext* context):c(context){
        c->OMGetRenderTargets(8,rt.data(),&dsv);c->OMGetDepthStencilState(&ds,&stencil);
        c->OMGetBlendState(&blend,factor,&mask);c->RSGetState(&raster);c->RSGetViewports(&viewportCount,viewport.data());
        c->IAGetInputLayout(&layout);c->IAGetPrimitiveTopology(&topology);
        c->VSGetShader(&vs,vc.data(),&vn);c->PSGetShader(&ps,pc.data(),&pn);
        c->GSGetShader(&gs,gc.data(),&gn);c->HSGetShader(&hs,hc.data(),&hn);c->DSGetShader(&domain,dc.data(),&dn);
        if(SUCCEEDED(c->QueryInterface(__uuidof(ID3D11DeviceContext1),reinterpret_cast<void**>(context1.put()))))
            context1->VSGetConstantBuffers1(0,1,&cb,&firstConstant,&constantCount);
        else c->VSGetConstantBuffers(0,1,&cb);
        c->PSGetSamplers(0,1,&sampler);
        c->VSGetShaderResources(0,128,vsrv.data());c->PSGetShaderResources(0,128,psrv.data());
        c->GSGetShaderResources(0,128,gsrv.data());c->HSGetShaderResources(0,128,hsrv.data());
        c->DSGetShaderResources(0,128,dsrv.data());c->CSGetShaderResources(0,128,csrv.data());
        c->GetPredication(&predicate,&predicateValue);
    }
    ~SavedState(){
        ID3D11ShaderResourceView* empty=nullptr;c->PSSetShaderResources(0,1,&empty);
        c->OMSetRenderTargets(8,rt.data(),dsv);c->OMSetDepthStencilState(ds,stencil);
        c->OMSetBlendState(blend,factor,mask);c->RSSetState(raster);c->RSSetViewports(viewportCount,viewport.data());
        c->IASetInputLayout(layout);c->IASetPrimitiveTopology(topology);
        c->VSSetShader(vs,vc.data(),vn);c->PSSetShader(ps,pc.data(),pn);c->GSSetShader(gs,gc.data(),gn);
        c->HSSetShader(hs,hc.data(),hn);c->DSSetShader(domain,dc.data(),dn);
        if(context1)context1->VSSetConstantBuffers1(0,1,&cb,&firstConstant,&constantCount);
        else c->VSSetConstantBuffers(0,1,&cb);
        c->PSSetSamplers(0,1,&sampler);
        c->VSSetShaderResources(0,128,vsrv.data());c->PSSetShaderResources(0,128,psrv.data());
        c->GSSetShaderResources(0,128,gsrv.data());c->HSSetShaderResources(0,128,hsrv.data());
        c->DSSetShaderResources(0,128,dsrv.data());c->CSSetShaderResources(0,128,csrv.data());
        c->SetPredication(predicate,predicateValue);
        auto release=[](auto* p){if(p)p->Release();};
        for(auto* p:rt)release(p);release(dsv);release(ds);release(blend);release(raster);release(layout);
        release(vs);release(ps);release(gs);release(hs);release(domain);release(cb);release(sampler);release(predicate);
        for(auto* p:vsrv)release(p);for(auto* p:psrv)release(p);for(auto* p:gsrv)release(p);
        for(auto* p:hsrv)release(p);for(auto* p:dsrv)release(p);for(auto* p:csrv)release(p);
        for(UINT i=0;i<vn;++i)release(vc[i]);for(UINT i=0;i<pn;++i)release(pc[i]);for(UINT i=0;i<gn;++i)release(gc[i]);
        for(UINT i=0;i<hn;++i)release(hc[i]);for(UINT i=0;i<dn;++i)release(dc[i]);
    }
};
}

struct Presenter::Impl {
    ID3D11Device* device=nullptr;UINT width=0,height=0;DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    Ref<ID3D11Texture2D> source;Ref<ID3D11ShaderResourceView> view;
    Ref<ID3D11VertexShader> vs;Ref<ID3D11PixelShader> ps;Ref<ID3D11Buffer> cb;
    Ref<ID3D11SamplerState> sampler;Ref<ID3D11RasterizerState> raster;Ref<ID3D11BlendState> blend;
    bool setup(ID3D11Device* d,const D3D11_TEXTURE2D_DESC& desc){
        if(device==d&&width==desc.Width&&height==desc.Height&&format==desc.Format&&source&&vs&&ps)return true;
        *this=Impl{};
        auto compile=compiler();if(!compile)return false;
        Ref<ID3DBlob> vb,pb,errors;
        if(FAILED(compile(shader,sizeof(shader)-1,"dominus-compositor",nullptr,nullptr,"vs","vs_4_0",D3DCOMPILE_ENABLE_STRICTNESS,0,vb.put(),errors.put())))return false;
        if(FAILED(compile(shader,sizeof(shader)-1,"dominus-compositor",nullptr,nullptr,"ps","ps_4_0",D3DCOMPILE_ENABLE_STRICTNESS,0,pb.put(),errors.put())))return false;
        if(FAILED(d->CreateVertexShader(vb->GetBufferPointer(),vb->GetBufferSize(),nullptr,vs.put())))return false;
        if(FAILED(d->CreatePixelShader(pb->GetBufferPointer(),pb->GetBufferSize(),nullptr,ps.put())))return false;
        D3D11_BUFFER_DESC bd{};bd.ByteWidth=16;bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        if(FAILED(d->CreateBuffer(&bd,nullptr,cb.put())))return false;
        D3D11_TEXTURE2D_DESC td=desc;td.MipLevels=1;td.ArraySize=1;td.Usage=D3D11_USAGE_DEFAULT;
        td.BindFlags=D3D11_BIND_SHADER_RESOURCE;td.CPUAccessFlags=0;td.MiscFlags=0;
        if(FAILED(d->CreateTexture2D(&td,nullptr,source.put())))return false;
        if(FAILED(d->CreateShaderResourceView(source.get(),nullptr,view.put())))return false;
        D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc=D3D11_COMPARISON_NEVER;sd.MaxLOD=D3D11_FLOAT32_MAX;
        if(FAILED(d->CreateSamplerState(&sd,sampler.put())))return false;
        D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
        if(FAILED(d->CreateRasterizerState(&rd,raster.put())))return false;
        D3D11_BLEND_DESC bl{};bl.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
        if(FAILED(d->CreateBlendState(&bl,blend.put())))return false;
        device=d;width=desc.Width;height=desc.Height;format=desc.Format;return true;
    }
};
bool valid_rect(const SourceRect& r) noexcept {
    return std::isfinite(r.x)&&std::isfinite(r.y)&&std::isfinite(r.width)&&std::isfinite(r.height)&&r.x>=0&&r.y>=0&&r.width>0&&r.height>0&&r.x+r.width<=1.000001f&&r.y+r.height<=1.000001f;
}
bool sample_grid(ID3D11Device* d,ID3D11DeviceContext* c,ID3D11Texture2D* source,const SourceRect& region,unsigned columns,unsigned rows,unsigned* output) noexcept {
    if(!d||!c||!source||!output||!columns||!rows||columns>64||rows>64||!valid_rect(region))return false;
    D3D11_TEXTURE2D_DESC desc{};source->GetDesc(&desc);
    if(!desc.Width||!desc.Height||desc.SampleDesc.Count!=1)return false;
    const bool bgra=desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM||desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    if(!bgra&&desc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM&&desc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)return false;
    const UINT left=std::min<UINT>(desc.Width-1,static_cast<UINT>(region.x*desc.Width)),top=std::min<UINT>(desc.Height-1,static_cast<UINT>(region.y*desc.Height));
    const UINT right=std::max<UINT>(left+1,std::min<UINT>(desc.Width,static_cast<UINT>(std::ceil((region.x+region.width)*desc.Width))));
    const UINT bottom=std::max<UINT>(top+1,std::min<UINT>(desc.Height,static_cast<UINT>(std::ceil((region.y+region.height)*desc.Height))));
    D3D11_TEXTURE2D_DESC sd{};sd.Width=right-left;sd.Height=bottom-top;sd.MipLevels=1;sd.ArraySize=1;sd.Format=desc.Format;
    sd.SampleDesc.Count=1;sd.Usage=D3D11_USAGE_STAGING;sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    Ref<ID3D11Texture2D> staging;if(FAILED(d->CreateTexture2D(&sd,nullptr,staging.put())))return false;
    const D3D11_BOX box{left,top,0,right,bottom,1};
    c->CopySubresourceRegion(staging.get(),0,0,0,0,source,0,&box);
    D3D11_MAPPED_SUBRESOURCE mapped{};if(FAILED(c->Map(staging.get(),0,D3D11_MAP_READ,0,&mapped)))return false;
    const auto* bytes=static_cast<const unsigned char*>(mapped.pData);
    for(unsigned row=0;row<rows;++row)for(unsigned column=0;column<columns;++column){
        const UINT x=std::min<UINT>(sd.Width-1,static_cast<UINT>((column+0.5)*sd.Width/columns));
        const UINT y=std::min<UINT>(sd.Height-1,static_cast<UINT>((row+0.5)*sd.Height/rows));
        const unsigned char* p=bytes+static_cast<std::size_t>(y)*mapped.RowPitch+x*4u;
        const unsigned r=bgra?p[2]:p[0],g=p[1],b=bgra?p[0]:p[2];
        output[row*columns+column]=(r<<16)|(g<<8)|b;
    }
    c->Unmap(staging.get(),0);return true;
}
struct PanelProbe::Impl {
    static constexpr unsigned Ring=4;
    struct Capture {Ref<ID3D11Texture2D> gpu,staging;unsigned long long time=0,identity=0;bool pending=false;};
    ID3D11Device* device=nullptr;DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;D3D11_BOX box{};
    std::array<Capture,Ring> ring{};
    std::array<Ref<ID3D11Texture2D>,MaxMarkerCycleStates> keepers{},patches{};
    std::array<bool,MaxMarkerCycleStates> keptValid{};
    UINT patchLeft=0,patchTop=0;
    unsigned head=0,tail=0,last=Ring;
    bool setup(ID3D11Device* d,const D3D11_TEXTURE2D_DESC& desc,const D3D11_BOX& b){
        if(device==d&&format==desc.Format&&box.left==b.left&&box.top==b.top&&box.right==b.right&&box.bottom==b.bottom&&ring[0].gpu)return true;
        *this=Impl{};
        D3D11_TEXTURE2D_DESC td{};td.Width=b.right-b.left;td.Height=b.bottom-b.top;td.MipLevels=1;td.ArraySize=1;td.Format=desc.Format;td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_DEFAULT;
        D3D11_TEXTURE2D_DESC sd=td;sd.Usage=D3D11_USAGE_STAGING;sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        for(auto& c:ring)if(FAILED(d->CreateTexture2D(&td,nullptr,c.gpu.put()))||FAILED(d->CreateTexture2D(&sd,nullptr,c.staging.put()))){*this=Impl{};return false;}
        for(auto& k:keepers)if(FAILED(d->CreateTexture2D(&td,nullptr,k.put()))){*this=Impl{};return false;}
        device=d;format=desc.Format;box=b;return true;
    }
};
PanelProbe::PanelProbe():impl_(std::make_unique<Impl>()){}
PanelProbe::~PanelProbe()=default;
void PanelProbe::reset() noexcept {*impl_=Impl{};}
bool PanelProbe::capture(ID3D11Device* d,ID3D11DeviceContext* c,ID3D11Texture2D* source,const SourceRect& region,unsigned long long timeMs,unsigned long long identityEpoch) noexcept {
    if(!d||!c||!source||!valid_rect(region))return false;
    D3D11_TEXTURE2D_DESC desc{};source->GetDesc(&desc);
    if(!desc.Width||!desc.Height||desc.SampleDesc.Count!=1)return false;
    if(desc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM&&desc.Format!=DXGI_FORMAT_B8G8R8A8_UNORM&&desc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB&&desc.Format!=DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)return false;
    // Same pixel bounds as sample_grid, so both read the same panel.
    const UINT left=std::min<UINT>(desc.Width-1,static_cast<UINT>(region.x*desc.Width)),top=std::min<UINT>(desc.Height-1,static_cast<UINT>(region.y*desc.Height));
    const UINT right=std::max<UINT>(left+1,std::min<UINT>(desc.Width,static_cast<UINT>(std::ceil((region.x+region.width)*desc.Width))));
    const UINT bottom=std::max<UINT>(top+1,std::min<UINT>(desc.Height,static_cast<UINT>(std::ceil((region.y+region.height)*desc.Height))));
    const D3D11_BOX box{left,top,0,right,bottom,1};
    auto& p=*impl_;
    if(!p.setup(d,desc,box))return false;
    auto& slot=p.ring[p.head];
    // The GPU is more than a ring behind: skip this frame rather than wait.
    if(slot.pending)return false;
    c->CopySubresourceRegion(slot.gpu.get(),0,0,0,0,source,0,&box);
    c->CopyResource(slot.staging.get(),slot.gpu.get());
    slot.time=timeMs;slot.identity=identityEpoch;slot.pending=true;p.head=(p.head+1)%Impl::Ring;return true;
}
bool PanelProbe::collect(ID3D11DeviceContext* c,unsigned* image,unsigned long long& timeMs,unsigned long long* identityEpoch) noexcept {
    auto& p=*impl_;if(!c||!image||!p.ring[0].gpu)return false;
    auto& slot=p.ring[p.tail];if(!slot.pending)return false;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if(FAILED(c->Map(slot.staging.get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped)))return false;
    const bool bgra=p.format==DXGI_FORMAT_B8G8R8A8_UNORM||p.format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    const UINT width=p.box.right-p.box.left,height=p.box.bottom-p.box.top;
    const auto* bytes=static_cast<const unsigned char*>(mapped.pData);
    for(unsigned row=0;row<NativeHeight;++row){
        const UINT y=std::min<UINT>(height-1,static_cast<UINT>((row+0.5)*height/NativeHeight));
        for(unsigned column=0;column<NativeWidth;++column){
            const UINT x=std::min<UINT>(width-1,static_cast<UINT>((column+0.5)*width/NativeWidth));
            const unsigned char* q=bytes+static_cast<std::size_t>(y)*mapped.RowPitch+x*4u;
            const unsigned r=bgra?q[2]:q[0],g=q[1],b=bgra?q[0]:q[2];
            image[row*NativeWidth+column]=(r<<16)|(g<<8)|b;
        }
    }
    c->Unmap(slot.staging.get(),0);
    timeMs=slot.time;if(identityEpoch)*identityEpoch=slot.identity;
    slot.pending=false;p.last=p.tail;p.tail=(p.tail+1)%Impl::Ring;return true;
}
bool PanelProbe::keep(ID3D11DeviceContext* c,unsigned which) noexcept {
    auto& p=*impl_;if(!c||which>=MaxMarkerCycleStates||p.last>=Impl::Ring||!p.keepers[which])return false;
    c->CopyResource(p.keepers[which].get(),p.ring[p.last].gpu.get());p.keptValid[which]=true;return true;
}
bool PanelProbe::kept(unsigned which) const noexcept {return which<MaxMarkerCycleStates&&impl_->keptValid[which];}
ID3D11Texture2D* PanelProbe::kept_image(unsigned which,unsigned& left,unsigned& top) const noexcept {
    if(!kept(which))return nullptr;
    left=impl_->box.left;top=impl_->box.top;return impl_->keepers[which].get();
}
void PanelProbe::release_cycle() noexcept {
    for(auto& patch:impl_->patches)patch.reset();
}
void PanelProbe::invalidate_kept() noexcept {
    impl_->keptValid.fill(false);release_cycle();
}
void PanelProbe::discard_pending() noexcept {
    auto& p=*impl_;for(auto& capture:p.ring)capture.pending=false;
    p.head=p.tail=0;p.last=Impl::Ring;
}
bool PanelProbe::prepare_cycle(ID3D11DeviceContext* c,const unsigned* slots,unsigned count,const ImageDiff& diff) noexcept {
    release_cycle();auto& p=*impl_;
    if(!c||!p.device||!slots||!count||count>MaxMarkerCycleStates||!diff.changed||
       diff.left>=diff.right||diff.top>=diff.bottom||diff.right>NativeWidth||diff.bottom>NativeHeight)return false;
    Ref<ID3D11Device> contextDevice;c->GetDevice(contextDevice.put());
    if(contextDevice.get()!=p.device||c->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return false;
    for(unsigned i=0;i<count;++i)if(!kept(slots[i]))return false;
    const UINT width=p.box.right-p.box.left,height=p.box.bottom-p.box.top;
    const UINT left=(diff.left?diff.left-1:0)*width/NativeWidth;
    const UINT top=(diff.top?diff.top-1:0)*height/NativeHeight;
    const UINT right=(std::min(NativeWidth,diff.right+1)*width+NativeWidth-1)/NativeWidth;
    const UINT bottom=(std::min(NativeHeight,diff.bottom+1)*height+NativeHeight-1)/NativeHeight;
    if(left>=right||top>=bottom||right>width||bottom>height)return false;
    const D3D11_BOX box{left,top,0,right,bottom,1};
    D3D11_TEXTURE2D_DESC td{};td.Width=right-left;td.Height=bottom-top;td.MipLevels=1;td.ArraySize=1;
    td.Format=p.format;td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_DEFAULT;
    for(unsigned i=0;i<count;++i){const unsigned slot=slots[i];if(p.patches[slot])continue;
        if(FAILED(p.device->CreateTexture2D(&td,nullptr,p.patches[slot].put()))){release_cycle();return false;}
        c->CopySubresourceRegion(p.patches[slot].get(),0,0,0,0,p.keepers[slot].get(),0,&box);
    }
    p.patchLeft=p.box.left+left;p.patchTop=p.box.top+top;return true;
}
ID3D11Texture2D* PanelProbe::cycle_image(unsigned slot,unsigned& left,unsigned& top) const noexcept {
    if(slot>=MaxMarkerCycleStates||!impl_->patches[slot])return nullptr;
    left=impl_->patchLeft;top=impl_->patchTop;return impl_->patches[slot].get();
}
Presenter::Presenter():impl_(std::make_unique<Impl>()){}
Presenter::~Presenter()=default;
void Presenter::reset(){impl_=std::make_unique<Impl>();}
bool Presenter::compose(ID3D11Device* d,ID3D11DeviceContext* c,ID3D11Texture2D* output,const Request& request){
    if(request.mode==Mode::Original)return true;
    if(!d||!c||!output||!valid_rect(request.source)||c->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return false;
    Ref<ID3D11Device> textureDevice,contextDevice;
    output->GetDevice(textureDevice.put());c->GetDevice(contextDevice.put());
    if(textureDevice.get()!=d||contextDevice.get()!=d)return false;
    // OMSetRenderTargets unbinds UAVs, and UAV counter state cannot be restored
    // by simply rebinding views. Refuse these unsupported caller states before
    // changing a resource or any pipeline binding.
    std::array<ID3D11UnorderedAccessView*,64> omUav{},csUav{};
    const UINT uavSlots=d->GetFeatureLevel()>=D3D_FEATURE_LEVEL_11_1?64:8;
    c->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,uavSlots,omUav.data());
    c->CSGetUnorderedAccessViews(0,uavSlots,csUav.data());
    bool hasUav=false;
    for(auto* p:omUav){if(p){hasUav=true;p->Release();}}
    for(auto* p:csUav){if(p){hasUav=true;p->Release();}}
    if(hasUav)return false;
    D3D11_TEXTURE2D_DESC desc{};output->GetDesc(&desc);
    if(!desc.Width||!desc.Height||desc.Width>32768||desc.Height>32768||desc.SampleDesc.Count!=1||desc.ArraySize!=1||desc.MipLevels!=1)return false;
    if(desc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM&&desc.Format!=DXGI_FORMAT_B8G8R8A8_UNORM&&desc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB&&desc.Format!=DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)return false;
    const auto geo=fit_4_3(static_cast<int>(desc.Width),static_cast<int>(desc.Height),request.integerScale?ScaleMode::Integer:ScaleMode::Fit);
    if(!geo.valid||!impl_->setup(d,desc))return false;
    Ref<ID3D11RenderTargetView> target;if(FAILED(d->CreateRenderTargetView(output,nullptr,target.put())))return false;
    SavedState saved(c);
    c->SetPredication(nullptr,FALSE);c->OMSetRenderTargets(0,nullptr,nullptr);
    c->CopyResource(impl_->source.get(),output);
    if(request.patch){
        D3D11_TEXTURE2D_DESC pd{};request.patch->GetDesc(&pd);Ref<ID3D11Device> owner;request.patch->GetDevice(owner.put());
        if(owner.get()==d&&pd.Format==desc.Format&&pd.SampleDesc.Count==1&&pd.MipLevels==1&&pd.ArraySize==1&&
           request.patchLeft<=desc.Width&&request.patchTop<=desc.Height&&pd.Width<=desc.Width-request.patchLeft&&pd.Height<=desc.Height-request.patchTop)
            c->CopySubresourceRegion(impl_->source.get(),0,request.patchLeft,request.patchTop,0,request.patch,0,nullptr);
    }
    const float source[]={request.source.x,request.source.y,request.source.width,request.source.height};
    c->UpdateSubresource(impl_->cb.get(),0,nullptr,source,0,0);
    auto* rt=target.get();c->OMSetRenderTargets(1,&rt,nullptr);const FLOAT black[]={0,0,0,1};c->ClearRenderTargetView(rt,black);
    c->OMSetDepthStencilState(nullptr,0);const FLOAT blendFactor[]={0,0,0,0};c->OMSetBlendState(impl_->blend.get(),blendFactor,0xffffffff);
    c->RSSetState(impl_->raster.get());D3D11_VIEWPORT vp{float(geo.left),float(geo.top),float(geo.width),float(geo.height),0,1};c->RSSetViewports(1,&vp);
    c->IASetInputLayout(nullptr);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(impl_->vs.get(),nullptr,0);c->PSSetShader(impl_->ps.get(),nullptr,0);
    c->GSSetShader(nullptr,nullptr,0);c->HSSetShader(nullptr,nullptr,0);c->DSSetShader(nullptr,nullptr,0);
    auto* cb=impl_->cb.get();auto* srv=impl_->view.get();auto* sampler=impl_->sampler.get();
    c->VSSetConstantBuffers(0,1,&cb);c->PSSetShaderResources(0,1,&srv);c->PSSetSamplers(0,1,&sampler);c->Draw(3,0);
    return true;
}
}
