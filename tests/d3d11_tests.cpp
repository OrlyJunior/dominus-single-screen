// Independent synthetic WARP host. Never opens a game, Steam, saves or input.
// Optional proxy coverage: dominus_d3d11_tests.exe --proxy <absolute d3d11.dll>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include "dominus/graphics.hpp"
#include "dominus/core.hpp"
#include "dominus/runtime_api.h"
#include "dominus/marker_blink.hpp"
#include "dominus/title_identity.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

extern "C" __declspec(dllexport) unsigned __cdecl DominusSyntheticHostProof() { return 0x44535331u; }

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
void checked(HRESULT hr, const char* operation) {
    if (FAILED(hr)) throw std::runtime_error(std::string(operation) + " HRESULT=" + std::to_string(static_cast<unsigned long>(hr)));
}
template<class T> void release(T*& value) { if (value) { value->Release(); value = nullptr; } }
template<class T> class Com {
    T* value_ = nullptr;
public:
    Com() = default;
    ~Com() { release(value_); }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    Com(Com&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    Com& operator=(Com&& other) noexcept { if (this != &other) { release(value_); value_ = std::exchange(other.value_, nullptr); } return *this; }
    T* get() const { return value_; }
    T* operator->() const { return value_; }
    T** put() { release(value_); return &value_; }
};
class Library {
    HMODULE value_ = nullptr;
public:
    explicit Library(const std::wstring& path) {
        value_ = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        require(value_ != nullptr, "LoadLibraryExW failed (absolute DLL path)");
    }
    ~Library() { if (value_) FreeLibrary(value_); }
    Library(const Library&) = delete;
    template<class Fn> Fn proc(const char* name) const {
        auto result = reinterpret_cast<Fn>(GetProcAddress(value_, name));
        require(result != nullptr, std::string("Missing export: ") + name);
        return result;
    }
};
std::wstring system_dll(const wchar_t* filename) {
    wchar_t path[MAX_PATH + 64]{};
    const UINT count = GetSystemDirectoryW(path, MAX_PATH);
    require(count && count < MAX_PATH, "GetSystemDirectoryW failed");
    return std::wstring(path) + L"\\" + filename;
}
using CreateDeviceFn = decltype(&D3D11CreateDevice);
using CreateBothFn = decltype(&D3D11CreateDeviceAndSwapChain);
using CompileFn = decltype(&D3DCompile);

struct Device {
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context;
};
Device create_warp(CreateDeviceFn create) {
    Device result;
    D3D_FEATURE_LEVEL selected{};
    checked(create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                   nullptr, 0, D3D11_SDK_VERSION, result.device.put(), &selected, result.context.put()), "Create WARP device");
    require(selected >= D3D_FEATURE_LEVEL_10_0, "WARP feature level below shader model 4");
    return result;
}
Com<ID3D11Texture2D> texture(Device& device, unsigned width, unsigned height, DXGI_FORMAT format,
                            const std::vector<std::uint8_t>* initial = nullptr, UINT bindings = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width=width; desc.Height=height; desc.MipLevels=1; desc.ArraySize=1; desc.Format=format;
    desc.SampleDesc.Count=1; desc.Usage=D3D11_USAGE_DEFAULT; desc.BindFlags=bindings;
    D3D11_SUBRESOURCE_DATA data{};
    if (initial) { require(initial->size() == width*height*4u, "initial texture byte count"); data.pSysMem=initial->data(); data.SysMemPitch=width*4; }
    Com<ID3D11Texture2D> result;
    checked(device.device->CreateTexture2D(&desc, initial ? &data : nullptr, result.put()), "CreateTexture2D");
    return result;
}
std::vector<std::uint8_t> readback(Device& device, ID3D11Texture2D* image) {
    D3D11_TEXTURE2D_DESC desc{}; image->GetDesc(&desc);
    desc.Usage=D3D11_USAGE_STAGING; desc.BindFlags=0; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ; desc.MiscFlags=0;
    Com<ID3D11Texture2D> staging;
    checked(device.device->CreateTexture2D(&desc,nullptr,staging.put()), "Create staging texture");
    device.context->CopyResource(staging.get(), image);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    checked(device.context->Map(staging.get(),0,D3D11_MAP_READ,0,&mapped), "Map staging texture");
    std::vector<std::uint8_t> bytes(desc.Width*desc.Height*4u);
    for (UINT row=0; row<desc.Height; ++row)
        std::memcpy(bytes.data()+row*desc.Width*4, static_cast<const std::uint8_t*>(mapped.pData)+row*mapped.RowPitch, desc.Width*4);
    device.context->Unmap(staging.get(),0);
    return bytes;
}
enum class Color { Red, Green, Blue, Black };
void encode(std::uint8_t* pixel, Color color, DXGI_FORMAT format) {
    unsigned r=0,g=0,b=0;
    if (color==Color::Red) r=255;
    if (color==Color::Green) g=255;
    if (color==Color::Blue) b=255;
    const bool bgra=format==DXGI_FORMAT_B8G8R8A8_UNORM;
    pixel[0]=static_cast<std::uint8_t>(bgra?b:r); pixel[1]=static_cast<std::uint8_t>(g);
    pixel[2]=static_cast<std::uint8_t>(bgra?r:b); pixel[3]=255;
}
std::vector<std::uint8_t> panels(unsigned width, unsigned height, DXGI_FORMAT format) {
    std::vector<std::uint8_t> bytes(width*height*4u);
    for(unsigned y=0;y<height;++y) for(unsigned x=0;x<width;++x) {
        Color color=Color::Blue;
        if(x<256&&y<192) color=(x&1)?Color::Green:Color::Red;
        if(x>=width-256&&y>=height-192) color=((y-(height-192))&1)?Color::Red:Color::Green;
        encode(&bytes[(y*width+x)*4],color,format);
    }
    return bytes;
}
dominus::gfx::Request request(unsigned width,unsigned height,bool map=false,bool integerScale=false) {
    const float x=map?float(width-256)/float(width):0;
    const float y=map?float(height-192)/float(height):0;
    return {map?dominus::gfx::Mode::Map:dominus::gfx::Mode::Gameplay,
            {x,y,256.0f/float(width),192.0f/float(height)},integerScale};
}
void verify_panels(const std::vector<std::uint8_t>& actual,unsigned width,unsigned height,DXGI_FORMAT format,bool map,bool integerScale) {
    const auto fit=dominus::fit_4_3(static_cast<int>(width),static_cast<int>(height),integerScale?dominus::ScaleMode::Integer:dominus::ScaleMode::Fit);
    require(fit.valid,"fit invalid");
    for(unsigned y=0;y<height;++y) for(unsigned x=0;x<width;++x) {
        Color expected=Color::Black;
        bool nearestTie=false;
        if(int(x)>=fit.left&&int(x)<fit.left+fit.width&&int(y)>=fit.top&&int(y)<fit.top+fit.height) {
            const double sampleX=(double(int(x)-fit.left)+0.5)*256.0/double(fit.width);
            const double sampleY=(double(int(y)-fit.top)+0.5)*192.0/double(fit.height);
            const unsigned sx=static_cast<unsigned>(sampleX);
            const unsigned sy=static_cast<unsigned>(sampleY);
            expected=map?((sy&1)?Color::Red:Color::Green):((sx&1)?Color::Green:Color::Red);
            const double axis=map?sampleY:sampleX;
            // At an exact texel boundary, finite shader interpolation can land
            // on either adjacent nearest texel. Both remain point-sampled;
            // blended colors, wrong ROIs and all non-boundary errors still fail.
            nearestTie=axis>=1&&std::abs(axis-std::round(axis))<0.0001;
        }
        std::array<std::uint8_t,4> pixel{}; encode(pixel.data(),expected,format);
        const auto* observed=&actual[(y*width+x)*4];
        bool matches=std::memcmp(pixel.data(),observed,4)==0;
        if(!matches&&nearestTie) {
            encode(pixel.data(),expected==Color::Red?Color::Green:Color::Red,format);
            matches=std::memcmp(pixel.data(),observed,4)==0;
        }
        if(!matches)
            throw std::runtime_error("nearest/ROI/black pixel mismatch at "+std::to_string(x)+","+std::to_string(y)+" expected "+std::to_string(pixel[0])+","+std::to_string(pixel[1])+","+std::to_string(pixel[2])+" actual "+std::to_string(observed[0])+","+std::to_string(observed[1])+","+std::to_string(observed[2]));
    }
}
void pixel_tests(Device& device,dominus::gfx::Presenter& presenter,unsigned width,unsigned height,DXGI_FORMAT format) {
    auto initial=panels(width,height,format);
    auto image=texture(device,width,height,format,&initial);
    dominus::gfx::Request original{};
    require(presenter.compose(device.device.get(),device.context.get(),image.get(),original),"Original mode rejected");
    require(readback(device,image.get())==initial,"Original mode changed pixels");
    auto invalid=request(width,height); invalid.source.x=-0.01f;
    require(!presenter.compose(device.device.get(),device.context.get(),image.get(),invalid),"invalid rectangle accepted");
    require(readback(device,image.get())==initial,"invalid rectangle changed pixels");
    require(presenter.compose(device.device.get(),device.context.get(),image.get(),request(width,height)),"Gameplay composition failed");
    verify_panels(readback(device,image.get()),width,height,format,false,false);
    device.context->UpdateSubresource(image.get(),0,nullptr,initial.data(),width*4,0);
    require(presenter.compose(device.device.get(),device.context.get(),image.get(),request(width,height,true)),"Map composition failed");
    verify_panels(readback(device,image.get()),width,height,format,true,false);
    device.context->UpdateSubresource(image.get(),0,nullptr,initial.data(),width*4,0);
    require(presenter.compose(device.device.get(),device.context.get(),image.get(),request(width,height,false,true)),"Integer composition failed");
    verify_panels(readback(device,image.get()),width,height,format,false,true);
    Com<ID3D11DeviceContext> deferred;
    checked(device.device->CreateDeferredContext(0,deferred.put()),"CreateDeferredContext");
    device.context->UpdateSubresource(image.get(),0,nullptr,initial.data(),width*4,0);
    require(!presenter.compose(device.device.get(),deferred.get(),image.get(),request(width,height)),"deferred context accepted");
    require(readback(device,image.get())==initial,"deferred-context rejection changed pixels");
}

struct Snapshot {
    ID3D11DeviceContext* context;
    std::array<ID3D11RenderTargetView*,8> targets{}; ID3D11DepthStencilView* depthView=nullptr;
    ID3D11DepthStencilState* depthState=nullptr; UINT stencil=0;
    ID3D11BlendState* blend=nullptr; FLOAT blendFactor[4]{}; UINT mask=0;
    ID3D11RasterizerState* raster=nullptr;
    std::array<D3D11_VIEWPORT,16> viewports{}; UINT viewportCount=16;
    std::array<D3D11_RECT,16> scissors{}; UINT scissorCount=16;
    ID3D11InputLayout* layout=nullptr; D3D11_PRIMITIVE_TOPOLOGY topology{};
    ID3D11VertexShader* vs=nullptr; ID3D11PixelShader* ps=nullptr; ID3D11GeometryShader* gs=nullptr;
    ID3D11HullShader* hs=nullptr; ID3D11DomainShader* ds=nullptr;
    ID3D11Buffer* cb=nullptr; ID3D11ShaderResourceView* srv=nullptr; ID3D11SamplerState* sampler=nullptr;
    ID3D11Predicate* predicate=nullptr; BOOL predicateValue=FALSE;
    ID3D11Buffer* vertex=nullptr; UINT stride=0,vertexOffset=0;
    ID3D11Buffer* index=nullptr; DXGI_FORMAT indexFormat{}; UINT indexOffset=0;
    explicit Snapshot(ID3D11DeviceContext* c):context(c) {
        c->OMGetRenderTargets(8,targets.data(),&depthView);c->OMGetDepthStencilState(&depthState,&stencil);
        c->OMGetBlendState(&blend,blendFactor,&mask);c->RSGetState(&raster);c->RSGetViewports(&viewportCount,viewports.data());
        c->RSGetScissorRects(&scissorCount,scissors.data());c->IAGetInputLayout(&layout);c->IAGetPrimitiveTopology(&topology);
        c->VSGetShader(&vs,nullptr,nullptr);c->PSGetShader(&ps,nullptr,nullptr);c->GSGetShader(&gs,nullptr,nullptr);
        c->HSGetShader(&hs,nullptr,nullptr);c->DSGetShader(&ds,nullptr,nullptr);
        c->VSGetConstantBuffers(0,1,&cb);c->PSGetShaderResources(0,1,&srv);c->PSGetSamplers(0,1,&sampler);
        c->GetPredication(&predicate,&predicateValue);c->IAGetVertexBuffers(0,1,&vertex,&stride,&vertexOffset);
        c->IAGetIndexBuffer(&index,&indexFormat,&indexOffset);
    }
    ~Snapshot() {
        for(auto*& target:targets)release(target);release(depthView);release(depthState);release(blend);release(raster);
        release(layout);release(vs);release(ps);release(gs);release(hs);release(ds);release(cb);release(srv);release(sampler);
        release(predicate);release(vertex);release(index);
    }
    void compare(const Snapshot& other) const {
        require(targets==other.targets&&depthView==other.depthView,"render target/depth view not restored");
        require(depthState==other.depthState&&stencil==other.stencil,"depth state/stencil not restored");
        require(blend==other.blend&&mask==other.mask&&std::memcmp(blendFactor,other.blendFactor,sizeof(blendFactor))==0,"blend state not restored");
        require(raster==other.raster,"rasterizer not restored");
        require(viewportCount==other.viewportCount&&std::memcmp(viewports.data(),other.viewports.data(),viewportCount*sizeof(D3D11_VIEWPORT))==0,"viewports not restored");
        require(scissorCount==other.scissorCount&&std::memcmp(scissors.data(),other.scissors.data(),scissorCount*sizeof(D3D11_RECT))==0,"scissor rectangles changed");
        require(layout==other.layout&&topology==other.topology,"input assembly state not restored");
        require(vs==other.vs&&ps==other.ps&&gs==other.gs&&hs==other.hs&&ds==other.ds,"shader stages not restored");
        require(cb==other.cb&&srv==other.srv&&sampler==other.sampler,"slot-zero resources not restored");
        require(predicate==other.predicate&&predicateValue==other.predicateValue,"predication not restored");
        require(vertex==other.vertex&&stride==other.stride&&vertexOffset==other.vertexOffset,"vertex buffer changed");
        require(index==other.index&&indexFormat==other.indexFormat&&indexOffset==other.indexOffset,"index buffer changed");
    }
};
Com<ID3DBlob> compile(CompileFn fn,const char* source,const char* entry,const char* profile) {
    Com<ID3DBlob> result,errors;
    HRESULT hr=fn(source,std::strlen(source),"synthetic-state",nullptr,nullptr,entry,profile,D3DCOMPILE_ENABLE_STRICTNESS,0,result.put(),errors.put());
    if(FAILED(hr)&&errors.get())std::cerr<<static_cast<const char*>(errors->GetBufferPointer())<<"\n";
    checked(hr,"Compile state sentinel shader");return result;
}
void state_test(Device& device,CompileFn compiler) {
    auto* d=device.device.get();auto* c=device.context.get();
    auto initial=panels(640,360,DXGI_FORMAT_R8G8B8A8_UNORM);
    auto image=texture(device,640,360,DXGI_FORMAT_R8G8B8A8_UNORM,&initial);
    auto other=texture(device,16,16,DXGI_FORMAT_R8G8B8A8_UNORM);
    Com<ID3D11RenderTargetView> target;checked(d->CreateRenderTargetView(other.get(),nullptr,target.put()),"sentinel RTV");
    D3D11_TEXTURE2D_DESC td{};td.Width=16;td.Height=16;td.MipLevels=1;td.ArraySize=1;td.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;td.SampleDesc.Count=1;td.BindFlags=D3D11_BIND_DEPTH_STENCIL;
    Com<ID3D11Texture2D> depth;checked(d->CreateTexture2D(&td,nullptr,depth.put()),"sentinel depth texture");
    Com<ID3D11DepthStencilView> depthView;checked(d->CreateDepthStencilView(depth.get(),nullptr,depthView.put()),"sentinel DSV");
    D3D11_DEPTH_STENCIL_DESC dd{};dd.DepthEnable=TRUE;dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;dd.DepthFunc=D3D11_COMPARISON_GREATER;
    dd.StencilEnable=TRUE;dd.StencilReadMask=3;dd.StencilWriteMask=7;
    dd.FrontFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_REPLACE,D3D11_STENCIL_OP_INCR,D3D11_COMPARISON_ALWAYS};dd.BackFace=dd.FrontFace;
    Com<ID3D11DepthStencilState> depthState;checked(d->CreateDepthStencilState(&dd,depthState.put()),"sentinel depth state");
    D3D11_BLEND_DESC bd{};bd.RenderTarget[0].BlendEnable=TRUE;bd.RenderTarget[0].SrcBlend=D3D11_BLEND_ONE;bd.RenderTarget[0].DestBlend=D3D11_BLEND_ONE;
    bd.RenderTarget[0].BlendOp=D3D11_BLEND_OP_ADD;bd.RenderTarget[0].SrcBlendAlpha=D3D11_BLEND_ONE;bd.RenderTarget[0].DestBlendAlpha=D3D11_BLEND_ZERO;bd.RenderTarget[0].BlendOpAlpha=D3D11_BLEND_OP_ADD;bd.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
    Com<ID3D11BlendState> blend;checked(d->CreateBlendState(&bd,blend.put()),"sentinel blend");
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_WIREFRAME;rd.CullMode=D3D11_CULL_FRONT;rd.DepthClipEnable=TRUE;rd.ScissorEnable=TRUE;
    Com<ID3D11RasterizerState> raster;checked(d->CreateRasterizerState(&rd,raster.put()),"sentinel rasterizer");
    const char code[]=R"(
struct V {float4 p:SV_Position;float2 uv:TEXCOORD0;};
V vs(float3 p:POSITION){V o;o.p=float4(p,1);o.uv=p.xy;return o;}
float4 ps(V v):SV_Target{return float4(v.uv,0,1);}
[maxvertexcount(1)] void gs(point V v[1],inout PointStream<V> result){result.Append(v[0]);}
)";
    auto vb=compile(compiler,code,"vs","vs_4_0");auto pb=compile(compiler,code,"ps","ps_4_0");auto gb=compile(compiler,code,"gs","gs_4_0");
    Com<ID3D11VertexShader> vs;Com<ID3D11PixelShader> ps;Com<ID3D11GeometryShader> gs;
    checked(d->CreateVertexShader(vb->GetBufferPointer(),vb->GetBufferSize(),nullptr,vs.put()),"sentinel VS");
    checked(d->CreatePixelShader(pb->GetBufferPointer(),pb->GetBufferSize(),nullptr,ps.put()),"sentinel PS");
    checked(d->CreateGeometryShader(gb->GetBufferPointer(),gb->GetBufferSize(),nullptr,gs.put()),"sentinel GS");
    D3D11_INPUT_ELEMENT_DESC element{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0};
    Com<ID3D11InputLayout> layout;checked(d->CreateInputLayout(&element,1,vb->GetBufferPointer(),vb->GetBufferSize(),layout.put()),"sentinel input layout");
    D3D11_BUFFER_DESC buffer{};buffer.ByteWidth=64;buffer.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    Com<ID3D11Buffer> cb;checked(d->CreateBuffer(&buffer,nullptr,cb.put()),"sentinel CB");
    buffer.BindFlags=D3D11_BIND_VERTEX_BUFFER;Com<ID3D11Buffer> vertex;checked(d->CreateBuffer(&buffer,nullptr,vertex.put()),"sentinel VB");
    buffer.BindFlags=D3D11_BIND_INDEX_BUFFER;Com<ID3D11Buffer> index;checked(d->CreateBuffer(&buffer,nullptr,index.put()),"sentinel IB");
    auto srvImage=texture(device,8,8,DXGI_FORMAT_R8G8B8A8_UNORM);
    Com<ID3D11ShaderResourceView> srv;checked(d->CreateShaderResourceView(srvImage.get(),nullptr,srv.put()),"sentinel SRV");
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_MIRROR;sd.MaxLOD=D3D11_FLOAT32_MAX;sd.ComparisonFunc=D3D11_COMPARISON_NEVER;
    Com<ID3D11SamplerState> sampler;checked(d->CreateSamplerState(&sd,sampler.put()),"sentinel sampler");
    D3D11_QUERY_DESC qd{D3D11_QUERY_OCCLUSION_PREDICATE,0};Com<ID3D11Predicate> predicate;checked(d->CreatePredicate(&qd,predicate.put()),"sentinel predicate");
    auto* rtv=target.get();c->OMSetRenderTargets(1,&rtv,depthView.get());c->OMSetDepthStencilState(depthState.get(),0x3A);
    const FLOAT factor[]={.1f,.2f,.3f,.4f};c->OMSetBlendState(blend.get(),factor,0x00FF00FF);c->RSSetState(raster.get());
    D3D11_VIEWPORT viewport[]={{3,4,16,9,.2f,.8f},{1,2,12,6,.1f,.9f}};c->RSSetViewports(2,viewport);
    D3D11_RECT scissor[]={{1,2,5,6},{3,4,7,8}};c->RSSetScissorRects(2,scissor);
    c->IASetInputLayout(layout.get());c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP);
    c->VSSetShader(vs.get(),nullptr,0);c->PSSetShader(ps.get(),nullptr,0);c->GSSetShader(gs.get(),nullptr,0);
    auto* cbp=cb.get();auto* srvp=srv.get();auto* sp=sampler.get();c->VSSetConstantBuffers(0,1,&cbp);c->PSSetShaderResources(0,1,&srvp);c->PSSetSamplers(0,1,&sp);
    auto* vbp=vertex.get();const UINT stride=12,offset=4;c->IASetVertexBuffers(0,1,&vbp,&stride,&offset);c->IASetIndexBuffer(index.get(),DXGI_FORMAT_R16_UINT,2);
    c->SetPredication(predicate.get(),TRUE);
    {
        Snapshot before(c);dominus::gfx::Presenter presenter;
        require(presenter.compose(d,c,image.get(),request(640,360)),"composition with sentinel state failed");
        Snapshot after(c);before.compare(after);
    }
    // Remove the unresolved sentinel predicate before readback; the compositor
    // must have disabled it during its own draw, then restored it faithfully.
    c->ClearState();verify_panels(readback(device,image.get()),640,360,DXGI_FORMAT_R8G8B8A8_UNORM,false,false);
}

void extended_binding_test(Device& device) {
    auto* d=device.device.get();auto* c=device.context.get();c->ClearState();
    auto initial=panels(640,360,DXGI_FORMAT_R8G8B8A8_UNORM);
    auto image=texture(device,640,360,DXGI_FORMAT_R8G8B8A8_UNORM,&initial);
    Com<ID3D11ShaderResourceView> view;checked(d->CreateShaderResourceView(image.get(),nullptr,view.put()),"alias SRV");
    auto* srv=view.get();c->PSSetShaderResources(7,1,&srv);c->VSSetShaderResources(3,1,&srv);
    c->CSSetShaderResources(11,1,&srv);c->GSSetShaderResources(4,1,&srv);c->HSSetShaderResources(5,1,&srv);c->DSSetShaderResources(6,1,&srv);
    Com<ID3D11DeviceContext1> context1;
    checked(c->QueryInterface(__uuidof(ID3D11DeviceContext1),reinterpret_cast<void**>(context1.put())),"Context1 for CB subrange");
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=1024;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    Com<ID3D11Buffer> cb;checked(d->CreateBuffer(&bd,nullptr,cb.put()),"subrange constant buffer");
    auto* cbp=cb.get();const UINT first=16,count=16;context1->VSSetConstantBuffers1(0,1,&cbp,&first,&count);
    UINT firstBefore=0,countBefore=0;Com<ID3D11Buffer> before;
    context1->VSGetConstantBuffers1(0,1,before.put(),&firstBefore,&countBefore);
    require(before.get()==cb.get()&&firstBefore==first&&countBefore==count,"CB1 subrange setup failed");
    dominus::gfx::Presenter presenter;require(presenter.compose(d,c,image.get(),request(640,360)),"alias SRV composition failed");
    Com<ID3D11ShaderResourceView> ps,vs,cs,gs,hs,ds;
    c->PSGetShaderResources(7,1,ps.put());c->VSGetShaderResources(3,1,vs.put());c->CSGetShaderResources(11,1,cs.put());
    c->GSGetShaderResources(4,1,gs.put());c->HSGetShaderResources(5,1,hs.put());c->DSGetShaderResources(6,1,ds.put());
    require(ps.get()==srv&&vs.get()==srv&&cs.get()==srv&&gs.get()==srv&&hs.get()==srv&&ds.get()==srv,"aliased high-slot SRVs not restored");
    UINT firstAfter=0,countAfter=0;Com<ID3D11Buffer> after;context1->VSGetConstantBuffers1(0,1,after.put(),&firstAfter,&countAfter);
    require(after.get()==before.get()&&firstBefore==firstAfter&&countBefore==countAfter,"CB1 subrange not restored");
    c->ClearState();verify_panels(readback(device,image.get()),640,360,DXGI_FORMAT_R8G8B8A8_UNORM,false,false);
}
void uav_rejection_test(Device& device) {
    auto* d=device.device.get();auto* c=device.context.get();c->ClearState();
    auto initial=panels(640,360,DXGI_FORMAT_R8G8B8A8_UNORM);
    auto image=texture(device,640,360,DXGI_FORMAT_R8G8B8A8_UNORM,&initial);
    auto writable=texture(device,8,8,DXGI_FORMAT_R8G8B8A8_UNORM,nullptr,D3D11_BIND_UNORDERED_ACCESS);
    Com<ID3D11UnorderedAccessView> view;checked(d->CreateUnorderedAccessView(writable.get(),nullptr,view.put()),"synthetic UAV");
    auto* uav=view.get();c->CSSetUnorderedAccessViews(2,1,&uav,nullptr);
    dominus::gfx::Presenter presenter;require(!presenter.compose(d,c,image.get(),request(640,360)),"CS UAV state accepted");
    Com<ID3D11UnorderedAccessView> observed;c->CSGetUnorderedAccessViews(2,1,observed.put());require(observed.get()==uav,"CS UAV binding changed on rejection");
    require(readback(device,image.get())==initial,"CS UAV rejection changed pixels");
    c->ClearState();c->OMSetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,1,&uav,nullptr);
    require(!presenter.compose(d,c,image.get(),request(640,360)),"OM UAV state accepted");
    c->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,1,observed.put());require(observed.get()==uav,"OM UAV binding changed on rejection");
    require(readback(device,image.get())==initial,"OM UAV rejection changed pixels");c->ClearState();
}

class HiddenWindow {
    HWND value_=nullptr;
    HINSTANCE instance_=GetModuleHandleW(nullptr);
    bool ownsClass_=false;
public:
    HiddenWindow() {
        WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=instance_;wc.lpszClassName=L"DominusSyntheticHiddenHost";
        ownsClass_=RegisterClassW(&wc)!=0;
        require(ownsClass_||GetLastError()==ERROR_CLASS_ALREADY_EXISTS,"RegisterClassW failed");
        value_=CreateWindowExW(WS_EX_NOACTIVATE,wc.lpszClassName,L"Synthetic D3D11 test",WS_POPUP,0,0,64,64,nullptr,nullptr,instance_,nullptr);
        require(value_!=nullptr,"Create hidden synthetic HWND failed");
        require(!IsWindowVisible(value_),"synthetic window unexpectedly visible");
    }
    ~HiddenWindow(){if(value_)DestroyWindow(value_);if(ownsClass_)UnregisterClassW(L"DominusSyntheticHiddenHost",instance_);}
    HWND get() const{return value_;}
};
void proxy_tests(const std::wstring& absolutePath) {
    require(absolutePath.size()>3&&absolutePath[1]==L':'&&(absolutePath[2]==L'\\'||absolutePath[2]==L'/'),"proxy requires an absolute drive path");
    Library proxy(absolutePath);
    auto create=proxy.proc<CreateBothFn>("D3D11CreateDeviceAndSwapChain");
    auto status=proxy.proc<DominusGetStatusFn>("DominusGetStatus");
    auto configure=proxy.proc<DominusSyntheticConfigureFn>("DominusSyntheticConfigure");
    auto shutdown=proxy.proc<DominusSyntheticShutdownFn>("DominusSyntheticShutdown");
    struct ShutdownGuard { DominusSyntheticShutdownFn call; bool done=false; ~ShutdownGuard(){if(!done)call();} } shutdownGuard{shutdown};
    auto get=[&](){DominusStatus value{};value.size=sizeof(value);require(status(&value)==1,"status rejected");return value;};
    auto first=get();
    require(first.abi_version==1&&first.synthetic_host==1&&first.supported_build==0,"synthetic host detection failed");
    require(first.observer_enabled==1,"synthetic observer not enabled");
    require(!first.native_pause_validated&&!first.input_path_validated&&!first.context_validated,"runtime claims unvalidated game hooks");
    require(status(nullptr)==0,"null status accepted");
    DominusStatus shortStatus{};shortStatus.size=4;require(status(&shortStatus)==0,"short status accepted");
    require(configure(3,0,0,1,1,0)==0,"invalid mode accepted");
    require(configure(1,-1,0,1,1,0)==0,"invalid source rect accepted");
    require(configure(0,0,0,1,1,0)==1,"Original configure failed");
    HiddenWindow window;
    Device device;Com<IDXGISwapChain> chain;
    DXGI_SWAP_CHAIN_DESC desc{};desc.BufferDesc.Width=640;desc.BufferDesc.Height=360;desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=1;desc.OutputWindow=window.get();desc.Windowed=TRUE;desc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
    checked(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&desc,chain.put(),device.device.put(),nullptr,device.context.put()),"proxy WARP CreateDeviceAndSwapChain");
    const auto before=get();
    checked(chain->Present(0,0),"proxy Original Present");auto original=get();
    require(original.presents==before.presents+1&&original.compositions==before.compositions,"Original Present counters wrong");
    auto fill=[&](unsigned width,unsigned height){Com<ID3D11Texture2D> back;checked(chain->GetBuffer(0,__uuidof(ID3D11Texture2D),reinterpret_cast<void**>(back.put())),"Get synthetic backbuffer");auto bytes=panels(width,height,DXGI_FORMAT_R8G8B8A8_UNORM);device.context->UpdateSubresource(back.get(),0,nullptr,bytes.data(),width*4,0);};
    fill(640,360);require(configure(1,0,0,256.0f/640,192.0f/360,0)==1,"Gameplay configure failed");
    checked(chain->Present(0,0),"proxy Gameplay Present");auto gameplay=get();
    require(gameplay.presents==original.presents+1&&gameplay.compositions==original.compositions+1,"Gameplay composition counter wrong");
    checked(chain->Present(0,DXGI_PRESENT_TEST),"proxy test Present");auto test=get();
    require(test.presents==gameplay.presents+1&&test.compositions==gameplay.compositions,"DXGI_PRESENT_TEST composed pixels");
    device.context->ClearState();device.context->Flush();
    checked(chain->ResizeBuffers(1,800,600,DXGI_FORMAT_R8G8B8A8_UNORM,0),"proxy ResizeBuffers");auto resized=get();
    require(resized.resizes==test.resizes+1,"Resize counter wrong");
    fill(800,600);require(configure(2,544.0f/800,408.0f/600,256.0f/800,192.0f/600,1)==1,"Map configure after resize failed");
    checked(chain->Present(0,0),"proxy Map Present after resize");auto map=get();
    require(map.compositions==resized.compositions+1&&map.presents==resized.presents+1,"Map/resize composition counter wrong");
    require(map.fallbacks==before.fallbacks,"unexpected synthetic composition fallback");
    require(configure(0,0,0,1,1,0)==1,"disable configure failed");
    checked(chain->Present(0,0),"proxy disabled Present");auto disabled=get();
    require(disabled.compositions==map.compositions,"Original toggle still composed");
    // The real executable imports CreateDevice, then obtains its factory. Test
    // that separate path too, rather than relying only on CreateDeviceAndSwapChain.
    {
        HiddenWindow secondWindow;auto secondDevice=create_warp(proxy.proc<CreateDeviceFn>("D3D11CreateDevice"));
        Com<IDXGIDevice> dxgi;Com<IDXGIAdapter> adapter;Com<IDXGIFactory> factory;Com<IDXGISwapChain> secondChain;
        checked(secondDevice.device->QueryInterface(__uuidof(IDXGIDevice),reinterpret_cast<void**>(dxgi.put())),"separate DXGI device");
        checked(dxgi->GetAdapter(adapter.put()),"separate adapter");
        checked(adapter->GetParent(__uuidof(IDXGIFactory),reinterpret_cast<void**>(factory.put())),"separate factory");
        auto secondDesc=desc;secondDesc.OutputWindow=secondWindow.get();
        checked(factory->CreateSwapChain(secondDevice.device.get(),&secondDesc,secondChain.put()),"intercepted factory CreateSwapChain");
        Com<ID3D11Texture2D> back;checked(secondChain->GetBuffer(0,__uuidof(ID3D11Texture2D),reinterpret_cast<void**>(back.put())),"separate backbuffer");
        auto bytes=panels(640,360,DXGI_FORMAT_R8G8B8A8_UNORM);secondDevice.context->UpdateSubresource(back.get(),0,nullptr,bytes.data(),640*4,0);
        require(configure(1,0,0,256.0f/640,192.0f/360,0)==1,"separate chain configure");
        const auto prior=get();checked(secondChain->Present(0,0),"separate factory chain Present");const auto next=get();
        require(next.presents==prior.presents+1&&next.compositions==prior.compositions+1&&next.fallbacks==prior.fallbacks,"separate factory path not composed");
        back=Com<ID3D11Texture2D>{};secondDevice.context->ClearState();secondDevice.context->Flush();
        checked(secondChain->ResizeBuffers(1,800,600,DXGI_FORMAT_R8G8B8A8_UNORM,0),"factory-created chain ResizeBuffers");
        auto afterFactoryResize=get();require(afterFactoryResize.resizes==next.resizes+1,"factory route resize not intercepted");
        checked(secondChain->GetBuffer(0,__uuidof(ID3D11Texture2D),reinterpret_cast<void**>(back.put())),"resized separate backbuffer");
        bytes=panels(800,600,DXGI_FORMAT_R8G8B8A8_UNORM);secondDevice.context->UpdateSubresource(back.get(),0,nullptr,bytes.data(),800*4,0);
        require(configure(1,0,0,256.0f/800,192.0f/600,0)==1,"factory route configure after resize");
        checked(secondChain->Present(0,0),"factory route resized Present");auto secondResized=get();
        require(secondResized.compositions==afterFactoryResize.compositions+1,"factory route resized composition failed");
        {
            HiddenWindow thirdWindow;Com<IDXGIFactory2> factory2;Com<IDXGISwapChain1> modern;
            checked(factory->QueryInterface(__uuidof(IDXGIFactory2),reinterpret_cast<void**>(factory2.put())),"Factory2 availability");
            DXGI_SWAP_CHAIN_DESC1 modernDesc{};modernDesc.Width=640;modernDesc.Height=360;modernDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
            modernDesc.SampleDesc.Count=1;modernDesc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;modernDesc.BufferCount=2;modernDesc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            modernDesc.Scaling=DXGI_SCALING_STRETCH;modernDesc.AlphaMode=DXGI_ALPHA_MODE_UNSPECIFIED;
            checked(factory2->CreateSwapChainForHwnd(secondDevice.device.get(),thirdWindow.get(),&modernDesc,nullptr,nullptr,modern.put()),"factory2 CreateSwapChainForHwnd");
            {
                Com<ID3D11Texture2D> modernBack;checked(modern->GetBuffer(0,__uuidof(ID3D11Texture2D),reinterpret_cast<void**>(modernBack.put())),"modern backbuffer");
                auto modernBytes=panels(640,360,DXGI_FORMAT_R8G8B8A8_UNORM);secondDevice.context->UpdateSubresource(modernBack.get(),0,nullptr,modernBytes.data(),640*4,0);
            }
            require(configure(1,0,0,256.0f/640,192.0f/360,0)==1,"modern route configure");
            DXGI_PRESENT_PARAMETERS full{};auto beforeModern=get();checked(modern->Present1(0,0,&full),"modern full Present1");auto fullStatus=get();
            require(fullStatus.presents==beforeModern.presents+1&&fullStatus.compositions==beforeModern.compositions+1,"factory2/Present1 full frame not composed exactly once");
            RECT dirtyRect{0,0,320,180};DXGI_PRESENT_PARAMETERS dirty{};dirty.DirtyRectsCount=1;dirty.pDirtyRects=&dirtyRect;
            const auto originalDirty=dirty;const auto originalRect=dirtyRect;
            checked(modern->Present1(0,0,&dirty),"modern dirty Present1");auto dirtyStatus=get();
            require(dirtyStatus.compositions==fullStatus.compositions&&dirtyStatus.fallbacks==fullStatus.fallbacks+1,"dirty Present1 did not preserve bypass");
            require(std::memcmp(&dirty,&originalDirty,sizeof(dirty))==0&&std::memcmp(&dirtyRect,&originalRect,sizeof(dirtyRect))==0,"dirty Present1 parameters changed");
            secondDevice.context->ClearState();secondDevice.context->Flush();
            checked(modern->ResizeBuffers(2,800,600,DXGI_FORMAT_R8G8B8A8_UNORM,0),"modern chain resize");auto modernResized=get();
            require(modernResized.resizes==dirtyStatus.resizes+1,"modern resize not intercepted");
            {
                Com<ID3D11Texture2D> modernBack;checked(modern->GetBuffer(0,__uuidof(ID3D11Texture2D),reinterpret_cast<void**>(modernBack.put())),"modern resized backbuffer");
                auto modernBytes=panels(800,600,DXGI_FORMAT_R8G8B8A8_UNORM);secondDevice.context->UpdateSubresource(modernBack.get(),0,nullptr,modernBytes.data(),800*4,0);
            }
            require(configure(2,544.0f/800,408.0f/600,256.0f/800,192.0f/600,0)==1,"modern resized map configure");
            checked(modern->Present1(0,0,&full),"modern resized Present1");auto modernFinal=get();
            require(modernFinal.compositions==modernResized.compositions+1&&modernFinal.presents==modernResized.presents+1,"modern resized composition failed");
        }
        secondDevice.context->ClearState();secondDevice.context->Flush();
    }
    require(shutdown()==1,"synthetic shutdown rejected");shutdownGuard.done=true;auto stopped=get();
    require(!stopped.observer_enabled,"observer active after shutdown");
    require(configure(1,0,0,1,1,0)==0,"configure accepted after shutdown");
    checked(chain->Present(0,0),"post-shutdown Present");auto post=get();
    require(post.presents==stopped.presents&&post.compositions==stopped.compositions,"hooks remained after shutdown");
    device.context->ClearState();device.context->Flush();
    std::cout<<"Proxy counters: presents="<<post.presents<<" compositions="<<post.compositions<<" resizes="<<post.resizes<<" fallbacks="<<post.fallbacks<<"\n";
}
void guard_test(const std::wstring& absolutePath) {
    require(absolutePath.size()>3&&absolutePath[1]==L':'&&(absolutePath[2]==L'\\'||absolutePath[2]==L'/'),"guard requires absolute DLL path");
    Library proxy(absolutePath);
    auto create=proxy.proc<CreateBothFn>("D3D11CreateDeviceAndSwapChain");
    auto status=proxy.proc<DominusGetStatusFn>("DominusGetStatus");
    auto configure=proxy.proc<DominusSyntheticConfigureFn>("DominusSyntheticConfigure");
    auto shutdown=proxy.proc<DominusSyntheticShutdownFn>("DominusSyntheticShutdown");
    DominusStatus first{};first.size=sizeof(first);require(status(&first)==1,"guard status failed");
    require(!first.synthetic_host&&!first.supported_build&&!first.observer_enabled,"unknown host gained observer access");
    require(!first.native_pause_validated&&!first.input_path_validated&&!first.context_validated,"unknown host enabled game adapters");
    require(configure(1,0,0,1,1,0)==0&&shutdown()==0,"unknown host accepted test controls");
    HiddenWindow window;Device device;Com<IDXGISwapChain> chain;
    DXGI_SWAP_CHAIN_DESC desc{};desc.BufferDesc.Width=640;desc.BufferDesc.Height=360;desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=1;desc.OutputWindow=window.get();desc.Windowed=TRUE;desc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
    checked(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&desc,chain.put(),device.device.put(),nullptr,device.context.put()),"guard system forwarding CreateDeviceAndSwapChain");
    checked(chain->Present(0,0),"guard forwarding Present");
    DominusStatus after{};after.size=sizeof(after);require(status(&after)==1,"guard final status failed");
    require(after.presents==first.presents&&after.compositions==first.compositions&&after.resizes==first.resizes,"unknown host installed hooks");
    device.context->ClearState();device.context->Flush();
}
std::wstring widen(const char* value) {
    const int length=MultiByteToWideChar(CP_ACP,MB_ERR_INVALID_CHARS,value,-1,nullptr,0);
    require(length>0,"invalid proxy argument encoding");std::wstring result(length,L'\0');
    require(MultiByteToWideChar(CP_ACP,MB_ERR_INVALID_CHARS,value,-1,result.data(),length)==length,"proxy path conversion failed");
    result.resize(length-1);return result;
}
// Portrait layout: gameplay left, map and Status stacked on the right, order
// swappable. Colours from user screenshots (teal map background, brown enemy
// panel). sample_grid must return them packed as 0x00RRGGBB in both formats,
// leave the source untouched, and let choose_map_panel find the map either way.
void panel_sampling_test(Device& device,DXGI_FORMAT format,bool mapOnTop) {
    constexpr unsigned width=960,height=540;
    const unsigned panelTop=height/18,panelHeight=height*4/9,left=width*2/3;
    std::vector<std::uint8_t> bytes(width*height*4u);
    const bool bgra=format==DXGI_FORMAT_B8G8R8A8_UNORM;
    auto put=[&](unsigned x,unsigned y,std::uint32_t rgb){auto* p=&bytes[(y*width+x)*4];p[0]=std::uint8_t(bgra?rgb:rgb>>16);p[1]=std::uint8_t(rgb>>8);p[2]=std::uint8_t(bgra?rgb>>16:rgb);p[3]=255;};
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){
        std::uint32_t c=0x303040;
        if(x>=left&&y>=panelTop&&y<panelTop+2*panelHeight){
            const bool upper=y<panelTop+panelHeight;const unsigned py=(y-panelTop)%panelHeight;
            const bool map=upper==mapOnTop;
            if(map)c=((x+py)/7)%5?0x2a6a7a:0xa04090;
            else c=py<panelHeight*7/12?0xa21869:((x-left)<panelHeight/3?0x000000:0x95735a);
        }
        put(x,y,c);
    }
    auto image=texture(device,width,height,format,&bytes);
    std::array<unsigned,dominus::PanelColumns*dominus::PanelRows> first{},second{};
    const dominus::gfx::SourceRect upper{2.f/3.f,1.f/18.f,1.f/3.f,4.f/9.f},lower{2.f/3.f,0.5f,1.f/3.f,4.f/9.f};
    require(dominus::gfx::sample_grid(device.device.get(),device.context.get(),image.get(),upper,dominus::PanelColumns,dominus::PanelRows,first.data()),"sample upper panel");
    require(dominus::gfx::sample_grid(device.device.get(),device.context.get(),image.get(),lower,dominus::PanelColumns,dominus::PanelRows,second.data()),"sample lower panel");
    require(readback(device,image.get())==bytes,"sampling changed the source");
    const unsigned topRow=0,bottomRow=dominus::PanelRows-1;
    const auto& statusGrid=mapOnTop?second:first;
    require(statusGrid[topRow*dominus::PanelColumns+8]==0xa21869,"Status pink packed as 0x00RRGGBB");
    require(statusGrid[bottomRow*dominus::PanelColumns+12]==0x95735a,"Status brown packed as 0x00RRGGBB");
    const auto choice=dominus::choose_map_panel(reinterpret_cast<const std::uint32_t*>(first.data()),reinterpret_cast<const std::uint32_t*>(second.data()),dominus::PanelColumns,dominus::PanelRows);
    require(choice==(mapOnTop?dominus::MapPanel::First:dominus::MapPanel::Second),"map panel chosen from pixels");
    const dominus::gfx::SourceRect invalid{0.9f,0.9f,0.5f,0.5f};
    require(!dominus::gfx::sample_grid(device.device.get(),device.context.get(),image.get(),invalid,4,4,first.data()),"invalid rect rejected");
    require(!dominus::gfx::sample_grid(device.device.get(),device.context.get(),image.get(),upper,0,4,first.data()),"empty grid rejected");
}

// Map panel in the preset's upper-right rectangle (teal, with a 2x2 DS-pixel
// marker at DS 100,80 when on) and a lower panel filled with `lower`. At
// 960x540 each panel is 320x240 pixels, 1.25 per DS pixel.
std::vector<std::uint8_t> map_frame(DXGI_FORMAT format,bool markerOn,std::uint32_t lower) {
    constexpr unsigned width=960,height=540,left=640,top=30,mid=270,bottom=510;
    std::vector<std::uint8_t> bytes(width*height*4u);
    const bool bgra=format==DXGI_FORMAT_B8G8R8A8_UNORM;
    auto put=[&](unsigned x,unsigned y,std::uint32_t rgb){auto* p=&bytes[(y*width+x)*4];p[0]=std::uint8_t(bgra?rgb:rgb>>16);p[1]=std::uint8_t(rgb>>8);p[2]=std::uint8_t(bgra?rgb>>16:rgb);p[3]=255;};
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){
        std::uint32_t c=0x303040;
        if(x>=left&&y>=top&&y<mid)c=0x2a6a7a;
        if(x>=left&&y>=mid&&y<bottom)c=lower;
        put(x,y,c);
    }
    if(markerOn)for(unsigned y=top+100;y<top+103;++y)for(unsigned x=left+125;x<left+128;++x)put(x,y,0xf8f8f8);
    return bytes;
}
bool collect_wait(Device& device,dominus::gfx::PanelProbe& probe,std::vector<unsigned>& image,unsigned long long& time) {
    for(int i=0;i<4000;++i){if(probe.collect(device.context.get(),image.data(),time))return true;device.context->Flush();Sleep(1);}
    return false;
}
const dominus::gfx::SourceRect UpperPanel{2.f/3.f,1.f/18.f,1.f/3.f,4.f/9.f},LowerPanel{2.f/3.f,0.5f,1.f/3.f,4.f/9.f};

void panel_probe_test(Device& device,DXGI_FORMAT format) {
    const auto onBytes=map_frame(format,true,0x95735a),offBytes=map_frame(format,false,0x95735a);
    auto on=texture(device,960,540,format,&onBytes),off=texture(device,960,540,format,&offBytes);
    dominus::gfx::PanelProbe probe;
    std::vector<unsigned> image(dominus::NativeWidth*dominus::NativeHeight);unsigned long long time=0;
    require(probe.capture(device.device.get(),device.context.get(),on.get(),UpperPanel,11),"capture on");
    require(collect_wait(device,probe,image,time)&&time==11,"collect on");
    require(image[80*dominus::NativeWidth+100]==0xf8f8f8&&image[81*dominus::NativeWidth+101]==0xf8f8f8,"marker sampled at DS pixel");
    require(image[80*dominus::NativeWidth+102]==0x2a6a7a&&image[10*dominus::NativeWidth+10]==0x2a6a7a,"background sampled at DS pixel");
    require(probe.keep(device.context.get(),0)&&probe.kept(0)&&!probe.kept(1),"keep on");
    require(probe.capture(device.device.get(),device.context.get(),off.get(),UpperPanel,12),"capture off");
    require(collect_wait(device,probe,image,time)&&time==12&&image[80*dominus::NativeWidth+100]==0x2a6a7a,"collect off");
    require(probe.keep(device.context.get(),1),"keep off");
    require(!probe.collect(device.context.get(),image.data(),time),"nothing left to collect");
    require(readback(device,on.get())==onBytes&&readback(device,off.get())==offBytes,"probe changed a source");
    unsigned left=0,top=0;auto* kept=probe.kept_image(0,left,top);
    require(kept&&left==640&&top==30,"kept image and its position");
    // The patch lands only in the compositor's copy: composing the "off"
    // frame with the kept "on" pixels equals composing the "on" frame.
    dominus::gfx::Presenter presenter;
    auto plain=texture(device,960,540,format,&onBytes),patched=texture(device,960,540,format,&offBytes);
    dominus::gfx::Request map{dominus::gfx::Mode::Map,UpperPanel,false};
    require(presenter.compose(device.device.get(),device.context.get(),plain.get(),map),"compose plain");
    auto patchedRequest=map;patchedRequest.patch=kept;patchedRequest.patchLeft=left;patchedRequest.patchTop=top;
    require(presenter.compose(device.device.get(),device.context.get(),patched.get(),patchedRequest),"compose patched");
    require(readback(device,plain.get())==readback(device,patched.get()),"patched composition equals the other phase");
    // A patch that does not fit is ignored.
    auto ignored=texture(device,960,540,format,&offBytes),reference=texture(device,960,540,format,&offBytes);
    auto bad=patchedRequest;bad.patch=on.get();
    require(presenter.compose(device.device.get(),device.context.get(),ignored.get(),bad),"compose bad patch");
    require(presenter.compose(device.device.get(),device.context.get(),reference.get(),map),"compose reference");
    require(readback(device,ignored.get())==readback(device,reference.get()),"oversized patch ignored");
    probe.reset();require(!probe.kept(0)&&!probe.kept_image(0,left,top),"reset drops kept images");
}

constexpr std::array<unsigned,13> MarkerHolds{{2,2,5,2,3,2,3,2,3,5,2,3,5}};
unsigned marker_state(unsigned tick) {
    tick%=39;
    for(unsigned i=0;i<MarkerHolds.size();++i){if(tick<MarkerHolds[i])return i;tick-=MarkerHolds[i];}
    return 0;
}
unsigned long long native_ms(unsigned tick) {return (static_cast<unsigned long long>(tick)*1000+59)/60;}

// Synthetic DS pixels with a shrinking translucent-looking ring followed by
// four almost-hidden states. The runtime must replay these exact captured
// pixels, including their background; it does not draw a replacement marker.
std::vector<std::uint32_t> native_marker(unsigned state,int offsetX=0,int offsetY=0,bool changedBackground=false) {
    std::vector<std::uint32_t> image(dominus::NativeWidth*dominus::NativeHeight,0x2a6a7a);
    if(changedBackground)for(unsigned y=10;y<18;++y)for(unsigned x=10;x<18;++x)image[y*dominus::NativeWidth+x]=0x16bd42;
    const int cx=100+offsetX,cy=80+offsetY;
    auto put=[&](int x,int y,std::uint32_t color){if(x>=0&&y>=0&&x<int(dominus::NativeWidth)&&y<int(dominus::NativeHeight))image[y*dominus::NativeWidth+x]=color;};
    if(state<9){
        const int rx=6-int(state/2),ry=5-int(state/2);
        const std::uint32_t color=(state&1)?0xe89f9f:0xffbfbf;
        for(int y=-ry;y<=ry;++y)for(int x=-rx;x<=rx;++x){
            const double d=double(x*x)/(rx*rx)+double(y*y)/(ry*ry);
            if(d>=0.45&&d<=1.15)put(cx+x,cy+y,color);
        }
    }else if(state<13){
        put(cx+int(state-9)%2,cy+int(state-9)/2,0xffbfbf);
        if(state==10)put(cx+1,cy+1,0xff0000);
    }else{
        // Three additional unique local images exercise the full 16-slot cap.
        put(cx,cy,0xa00000u+state*0x101u);
    }
    return image;
}
std::vector<std::uint8_t> native_map_frame(DXGI_FORMAT format,const std::vector<std::uint32_t>& native,unsigned panel=0) {
    auto bytes=map_frame(format,false,panel?0x2a6a7a:0x95735a);
    const bool bgra=format==DXGI_FORMAT_B8G8R8A8_UNORM;
    const unsigned top=panel?270:30;
    for(unsigned y=0;y<240;++y)for(unsigned x=0;x<320;++x){
        const unsigned nx=std::min(255u,((2*x+1)*256)/(2*320));
        const unsigned ny=std::min(191u,((2*y+1)*192)/(2*240));
        const auto color=native[ny*256+nx];auto* p=&bytes[((top+y)*960+640+x)*4];
        p[0]=std::uint8_t(bgra?color:color>>16);p[1]=std::uint8_t(color>>8);p[2]=std::uint8_t(bgra?color>>16:color);p[3]=255;
    }
    return bytes;
}
bool patch_matches(const std::vector<std::uint8_t>& patch,unsigned width,unsigned height,unsigned left,unsigned top,
                   const std::vector<std::uint8_t>& frame) {
    if(left+width>960||top+height>540||patch.size()!=width*height*4u)return false;
    for(unsigned y=0;y<height;++y)if(std::memcmp(&patch[y*width*4],&frame[((top+y)*960+left)*4],width*4))return false;
    return true;
}

void marker_cycle_pixels_test(Device& device,DXGI_FORMAT format) {
    dominus::gfx::PanelProbe probe;
    dominus::MarkerCycleTracker tracker;
    std::vector<unsigned> sampled(dominus::NativeWidth*dominus::NativeHeight);
    std::array<std::vector<std::uint8_t>,16> frames;
    std::array<Com<ID3D11Texture2D>,16> sources;
    for(unsigned i=0;i<16;++i){frames[i]=native_map_frame(format,native_marker(i));sources[i]=texture(device,960,540,format,&frames[i]);}
    auto observe=[&](ID3D11Texture2D* source,unsigned long long stamp,const std::vector<std::uint32_t>& expected){
        require(probe.capture(device.device.get(),device.context.get(),source,UpperPanel,stamp),"cycle capture");
        unsigned long long collected=0;require(collect_wait(device,probe,sampled,collected)&&collected==stamp,"cycle capture timestamp");
        require(std::equal(sampled.begin(),sampled.end(),expected.begin()),"capture preserves every native DS sample");
        const auto generation=tracker.generation();const auto event=tracker.observe(sampled.data(),collected);
        if(generation!=tracker.generation())probe.invalidate_kept();
        if(event!=dominus::MarkerCycleTracker::Event::Same)require(probe.keep(device.context.get(),tracker.latest()),"keep cycle image");
        return event;
    };
    // Three laps minus the final hidden hold: two ordered repetitions have
    // completed even though the first observed run may have started midway.
    for(unsigned tick=0;tick<=114;++tick){const unsigned state=marker_state(tick);observe(sources[state].get(),native_ms(tick),native_marker(state));}
    const auto plan=tracker.plan(native_ms(114));
    require(plan.valid&&plan.confidence==dominus::MarkerCycleConfidence::Complete,"13-state complete cycle captured on GPU");
    require(plan.count==13&&plan.periodTicks==39&&tracker.imageCount()==13,"13 images and 39 native ticks");
    require(plan.frozenSlot==12,"opening near the hidden phase retains the entire cycle");
    unsigned holdSum=0;
    for(unsigned i=0;i<plan.count;++i){require(plan.holdTicks[i]==MarkerHolds[plan.slots[i]],"native 2/3/5 tick durations survive readback");holdSum+=plan.holdTicks[i];}
    require(holdSum==39,"650 ms native period without per-phase rounding");
    require(probe.prepare_cycle(device.context.get(),plan.slots.data(),plan.count,plan.diff),"prepare small cycle patches");
    unsigned left=0,top=0;auto* patch=probe.cycle_image(0,left,top);require(patch,"first cycle patch");
    D3D11_TEXTURE2D_DESC desc{};patch->GetDesc(&desc);
    const unsigned expectedLeft=640+((plan.diff.left?plan.diff.left-1:0)*320)/256;
    const unsigned expectedTop=30+((plan.diff.top?plan.diff.top-1:0)*240)/192;
    const unsigned expectedRight=640+(std::min(256u,plan.diff.right+1)*320+255)/256;
    const unsigned expectedBottom=30+(std::min(192u,plan.diff.bottom+1)*240+191)/192;
    require(left==expectedLeft&&top==expectedTop&&desc.Width==expectedRight-expectedLeft&&desc.Height==expectedBottom-expectedTop,"patch is precisely union plus one DS pixel guard band");
    require(desc.Width<24&&desc.Height<24,"replay texture contains only the marker region");
    for(unsigned slot=0;slot<13;++slot){
        auto* image=probe.cycle_image(slot,left,top);require(image,"all captured cycle slots have patches");image->GetDesc(&desc);
        require(patch_matches(readback(device,image),desc.Width,desc.Height,left,top,frames[slot]),"cycle patch is byte-exact native captured content");
        require(readback(device,sources[slot].get())==frames[slot],"cycle capture leaves the source intact");
    }
    const auto first=dominus::marker_cycle_frame(plan,plan.phaseStartMs);
    const auto nextLap=dominus::marker_cycle_frame(plan,plan.phaseStartMs+650);
    require(first.slot==12&&nextLap.slot==first.slot&&nextLap.sequenceIndex==first.sequenceIndex,"hidden phase resumes at the same phase after exactly 650 ms");
    // Change scenery outside the patch after capture. Replaying an old marker
    // must preserve that newer background in the compositor's private copy.
    const auto frozenBytes=native_map_frame(format,native_marker(12,0,0,true));
    auto frozen=texture(device,960,540,format,&frozenBytes),patched=texture(device,960,540,format,&frozenBytes);
    patch=probe.cycle_image(0,left,top);patch->GetDesc(&desc);const auto patchBytes=readback(device,patch);
    auto expected=frozenBytes;
    for(unsigned y=0;y<desc.Height;++y)std::memcpy(&expected[((top+y)*960+left)*4],&patchBytes[y*desc.Width*4],desc.Width*4);
    auto reference=texture(device,960,540,format,&expected);
    dominus::gfx::Presenter presenter;dominus::gfx::Request request{dominus::gfx::Mode::Map,UpperPanel,false};
    auto patchedRequest=request;patchedRequest.patch=patch;patchedRequest.patchLeft=left;patchedRequest.patchTop=top;
    require(presenter.compose(device.device.get(),device.context.get(),patched.get(),patchedRequest)&&presenter.compose(device.device.get(),device.context.get(),reference.get(),request),"compose small native marker patch");
    require(readback(device,patched.get())==readback(device,reference.get()),"background outside union survives replay");
    require(readback(device,frozen.get())==frozenBytes,"frozen source never receives replay pixels");
    probe.release_cycle();require(!probe.cycle_image(0,left,top)&&probe.kept(0),"release patches retains captures");

    // The GPU keeper supports every legal slot and rejects an out-of-bounds
    // slot without indexing or aliasing slot zero.
    for(unsigned slot=13;slot<16;++slot){
        require(probe.capture(device.device.get(),device.context.get(),sources[slot].get(),UpperPanel,2000+slot),"capture capacity image");
        unsigned long long stamp=0;require(collect_wait(device,probe,sampled,stamp)&&probe.keep(device.context.get(),slot),"keep through slot 15");
    }
    std::array<unsigned,16> allSlots{};for(unsigned slot=0;slot<16;++slot){allSlots[slot]=slot;require(probe.kept(slot),"all 16 slots retained");}
    require(!probe.keep(device.context.get(),16)&&!probe.kept(16)&&!probe.kept_image(16,left,top),"slot 16 rejected safely");
    require(probe.prepare_cycle(device.context.get(),allSlots.data(),16,plan.diff),"prepare maximum 16 image cycle");
    require(probe.cycle_image(15,left,top)&&!probe.cycle_image(16,left,top),"maximum cycle slot bounds");
    probe.invalidate_kept();
    for(unsigned slot=0;slot<16;++slot)require(!probe.kept(slot)&&!probe.cycle_image(slot,left,top),"invalidation clears old-room captures and patches");

    // Small within-room translations may contribute to a partial union. A
    // larger move starts a fresh generation and cannot reuse old textures.
    tracker.reset();unsigned long long generation=0;
    for(unsigned i=0;i<4;++i){
        const auto native=native_marker(i,int(i),int(i));const auto bytes=native_map_frame(format,native);auto shifted=texture(device,960,540,format,&bytes);
        observe(shifted.get(),native_ms(150+2*i),native);if(!i)generation=tracker.generation();
        require(tracker.generation()==generation&&tracker.imageCount()==i+1,"one to three DS pixel move remains within the same partial region (step="+std::to_string(i)+", generation="+std::to_string(tracker.generation())+"/"+std::to_string(generation)+", images="+std::to_string(tracker.imageCount())+")");
        observe(shifted.get(),native_ms(151+2*i),native);
    }
    require(tracker.difference().right-tracker.difference().left<=20&&tracker.difference().bottom-tracker.difference().top<=20,"shifted union stays bounded");
    const auto roomNative=native_marker(0,35,0);const auto roomBytes=native_map_frame(format,roomNative);auto room=texture(device,960,540,format,&roomBytes);
    observe(room.get(),native_ms(158),roomNative);
    require(tracker.generation()!=generation&&tracker.imageCount()==1&&!probe.kept(1),"large room displacement resets both tracking and retained images");
    const auto resizedBytes=panels(800,600,format);auto resized=texture(device,800,600,format,&resizedBytes);
    require(probe.capture(device.device.get(),device.context.get(),resized.get(),UpperPanel,2600),"capture after resolution change");
    require(!probe.kept(0)&&!probe.cycle_image(0,left,top),"resolution change invalidates previous GPU images");
}

std::vector<std::string> markerLog;
void marker_log(const char* line) noexcept {try{markerLog.emplace_back(line);}catch(...){}}
bool logged(const std::string& text){for(const auto& l:markerLog)if(l.find(text)!=std::string::npos)return true;return false;}
std::string marker_plans(){std::string result;for(const auto& line:markerLog)if(line.find("marker,plan,")==0||line.find("marker,replay-stopped,")==0)result+=" ["+line+"]";return result;}
std::string marker_traces(){std::string result;for(const auto& line:markerLog)if(line.find("marker,trace,panel=0,")==0)result+=" ["+line+"]";return result;}

unsigned long long qpc_ms() {
    static const auto frequency=[](){LARGE_INTEGER value{};QueryPerformanceFrequency(&value);return value.QuadPart;}();
    LARGE_INTEGER value{};QueryPerformanceCounter(&value);
    return static_cast<unsigned long long>(value.QuadPart/frequency)*1000+static_cast<unsigned long long>((value.QuadPart%frequency)*1000/frequency);
}
void marker_multistate_replay_test(Device& device,DXGI_FORMAT format) {
    markerLog.clear();dominus::marker::configure({true,true},nullptr,marker_log);
    const dominus::gfx::SourceRect panels[2]={UpperPanel,LowerPanel};
    std::array<std::vector<std::uint8_t>,13> frames;
    std::array<Com<ID3D11Texture2D>,13> sources;
    for(unsigned state=0;state<13;++state){
        frames[state]=native_map_frame(format,native_marker(state));
        const auto lower=native_map_frame(format,native_marker(state,12,6),1);
        for(unsigned y=270;y<510;++y)std::memcpy(&frames[state][(y*960+640)*4],&lower[(y*960+640)*4],320*4);
        sources[state]=texture(device,960,540,format,&frames[state]);
    }
    auto reset=[&](){dominus::marker::on_present(device.device.get(),device.context.get(),sources[0].get(),panels,-1,false);markerLog.clear();};
    auto run=[&](unsigned duration,int panel,const std::function<unsigned(unsigned long long)>& stateAt,
                 const std::function<void(const dominus::marker::Patch&)>& inspect,bool nativePresentCadence=false){
        const auto begin=qpc_ms();auto next=begin;unsigned presentTick=0;
        while(qpc_ms()-begin<duration){
            const auto now=qpc_ms();if(now<next){if(nativePresentCadence)Sleep(1);else SwitchToThread();continue;}
            const auto state=stateAt(now-begin);
            const auto patch=dominus::marker::on_present(device.device.get(),device.context.get(),sources[state].get(),panels,panel,true);
            if(inspect)inspect(patch);
            // Observe twice per half native frame. Sampling every 8-9 ms can
            // round a 33 ms hold to three ticks when Windows wakes late.
            device.context->Flush();next=nativePresentCadence?begin+native_ms(++presentTick):now+4;
        }
    };
    // Both panels independently hold a complete 13-state cycle. Freeze near
    // the hidden phase and exercise each panel order through the real adapter.
    for(int panel=0;panel<2;++panel){
        reset();
        run(1910,-1,[](unsigned long long elapsed){return marker_state(unsigned(elapsed*60/1000));},{});
        std::array<bool,13> seen{};unsigned patches=0,unpatched=0;
        run(1050,panel,[](unsigned long long){return 12u;},[&](const dominus::marker::Patch& patch){
            if(!patch.texture){++unpatched;return;}++patches;
            require(patch.left>=640&&patch.left<960&&patch.top>=unsigned(panel?270:30)&&patch.top<unsigned(panel?510:270),"selected panel patch origin");
            D3D11_TEXTURE2D_DESC desc{};patch.texture->GetDesc(&desc);const auto bytes=readback(device,patch.texture);
            require(desc.Width<24&&desc.Height<24,"adapter replays a small marker patch");
            bool matched=false;
            for(unsigned state=0;state<13;++state)if(patch_matches(bytes,desc.Width,desc.Height,patch.left,patch.top,frames[state])){matched=true;seen[state]=true;break;}
            require(matched,"adapter replay consists entirely of the game's captured source pixels");
        });
        require(logged("marker,plan,panel="+std::to_string(panel)+",valid=1,reason=complete-observed-cycle,images=1,replay=1")&&logged("count=13"),"adapter accepts 13-state cycle after two ordered repetitions"+marker_plans()+marker_traces());
        require(patches>20&&unpatched>0&&std::count(seen.begin(),seen.end(),true)>=12,"all native phases replay, including a frozen near-hidden phase");
        for(unsigned state=0;state<13;++state)require(readback(device,sources[state].get())==frames[state],"adapter never modifies either source panel");
        unsigned after=0;
        run(40,-1,[](unsigned long long){return 12u;},[&](const dominus::marker::Patch& patch){if(patch.texture)++after;});
        require(after==0&&logged("marker,map-closed,panel="+std::to_string(panel)+",replayed=1"),"closing either panel immediately stops replay");
    }
    reset();
    // Only four late states, including the almost-hidden final state, have
    // been observed. A partial sequence remains frozen even after settling.
    run(230,-1,[](unsigned long long elapsed){return marker_state(24+unsigned(elapsed*60/1000));},{});
    unsigned partialPatches=0;
    run(750,0,[](unsigned long long){return 12u;},[&](const dominus::marker::Patch& patch){if(patch.texture)++partialPatches;});
    require(partialPatches==0&&logged("marker,plan,panel=0,valid=0"),"partial captured sequence keeps the native frozen image");
    // An open map must never train a later cycle, even if its source keeps
    // animating. These observations only validate/refuse the frozen image.
    run(1450,0,[](unsigned long long elapsed){return marker_state(unsigned(elapsed*60/1000));},{});
    run(20,-1,[](unsigned long long){return 12u;},{});
    markerLog.clear();unsigned learnedUnderPause=0;
    run(650,0,[](unsigned long long){return 12u;},[&](const dominus::marker::Patch& patch){if(patch.texture)++learnedUnderPause;});
    require(learnedUnderPause==0&&logged("marker,plan,panel=0,valid=0"),"captures under MapOpen cannot complete a partial cycle");
    reset();
    // Characterize actual 60 Hz Present scheduling separately from the exact
    // timestamped GPU test above. OS wake-up jitter may conservatively refuse
    // a cycle; it must never turn a partial sequence into an invented loop.
    run(1910,-1,[](unsigned long long elapsed){return marker_state(unsigned(elapsed*60/1000));},{},true);
    unsigned nativeCadencePatches=0;
    run(750,0,[](unsigned long long){return 12u;},[&](const dominus::marker::Patch& patch){if(patch.texture)++nativeCadencePatches;},true);
    if(nativeCadencePatches){
        require(logged("marker,plan,panel=0,valid=1,reason=complete-observed-cycle,images=1,replay=1")&&logged("count=13,periodTicks=39"),"real 60 Hz replay requires the full native cycle"+marker_plans());
        std::cout<<"INFO actual QPC 60 Hz capture: complete 13 states / 39 ticks\n";
    }else{
        require(logged("marker,plan,panel=0,valid=0")&&logged("replay=0"),"real 60 Hz timing uncertainty remains frozen"+marker_plans());
        std::cout<<"INFO actual QPC 60 Hz capture: conservatively frozen"<<marker_plans()<<"\n";
    }
    reset();
    require(!GetModuleHandleW(L"dra02.dll"),"synthetic host has no native game module");
    dominus::marker::configure({false,true,true},nullptr,marker_log);markerLog.clear();
    unsigned missingIdentityPatches=0;
    run(120,0,[](unsigned long long){return 12u;},[&](const dominus::marker::Patch& patch){if(patch.texture)++missingIdentityPatches;});
    require(missingIdentityPatches==0&&logged("reason=position-identity-unavailable"),"production identity mode stays frozen when dra02 is absent");
    // Dawn follows its own identity (dra01, absent here too) and limits.
    require(!GetModuleHandleW(L"dra01.dll"),"synthetic host has no Dawn module");
    markerLog.clear();unsigned dawnPatches=0;
    for(const auto begin=qpc_ms();qpc_ms()-begin<120;){
        const auto patch=dominus::marker::on_present(device.device.get(),device.context.get(),sources[12].get(),panels,0,true,{},dominus::Title::DawnOfSorrow);
        if(patch.texture)++dawnPatches;
        device.context->Flush();Sleep(4);
    }
    require(dawnPatches==0&&logged("marker,title=dawn,maxChanged=256")&&logged("reason=position-identity-unavailable"),"Dawn's identity mode stays frozen when dra01 is absent"+marker_plans());
    markerLog.clear();
    dominus::marker::on_present(device.device.get(),device.context.get(),sources[12].get(),panels,-1,true);
    require(logged("marker,title=portrait,maxChanged=192")&&logged("reason=title-changed"),"returning to Portrait drops Dawn's captures and restores its limits");
    dominus::marker::configure({false,true},nullptr,marker_log);
}

void marker_cycle_retention_test(Device& device,DXGI_FORMAT format) {
    markerLog.clear();dominus::marker::configure({true,true},nullptr,marker_log);
    const dominus::gfx::SourceRect panels[2]={UpperPanel,LowerPanel};
    std::array<std::vector<std::uint8_t>,13> frames;
    std::array<Com<ID3D11Texture2D>,13> sources;
    for(unsigned state=0;state<13;++state){
        frames[state]=native_map_frame(format,native_marker(state));
        const auto lower=native_map_frame(format,native_marker(state,12,6),1);
        for(unsigned y=270;y<510;++y)std::memcpy(&frames[state][(y*960+640)*4],&lower[(y*960+640)*4],320*4);
        sources[state]=texture(device,960,540,format,&frames[state]);
    }
    const auto movedBytes=native_map_frame(format,native_marker(0,8,0));
    auto movedSource=texture(device,960,540,format,&movedBytes);

    unsigned long long wallOrigin=0,pausedTotalMs=0;
    auto reset=[&](){
        dominus::marker::on_present(device.device.get(),device.context.get(),sources[0].get(),panels,-1,false);
        markerLog.clear();wallOrigin=qpc_ms();pausedTotalMs=0;
    };
    auto game_state=[&](unsigned long long now){
        const auto gameMs=now>=wallOrigin+pausedTotalMs?(now-wallOrigin-pausedTotalMs):0ull;
        return marker_state(unsigned(gameMs*60/1000));
    };
    auto run_unpaused=[&](unsigned durationMs,const dominus::marker::PauseContext& pause){
        const auto begin=qpc_ms();auto next=begin;
        while(qpc_ms()-begin<durationMs){
            const auto now=qpc_ms();if(now<next){SwitchToThread();continue;}
            const auto st=game_state(now);
            const auto patch=dominus::marker::on_present(device.device.get(),device.context.get(),sources[st].get(),panels,-1,true,pause);
            require(!patch.texture,"unpaused gameplay never emits a replay patch");
            device.context->Flush();next=now+4;
        }
    };
    auto run_paused=[&](unsigned durationMs,int panel,unsigned frozenState,const dominus::marker::PauseContext& pause){
        unsigned patches=0;const auto begin=qpc_ms();auto next=begin;
        while(qpc_ms()-begin<durationMs){
            const auto now=qpc_ms();if(now<next){SwitchToThread();continue;}
            const auto patch=dominus::marker::on_present(device.device.get(),device.context.get(),sources[frozenState].get(),panels,panel,true,pause);
            if(patch.texture)++patches;
            device.context->Flush();next=now+4;
        }
        return patches;
    };

    // 1. Capture one 2-lap cycle, then perform 10 static reopenings < 1.3 s apart
    // across 2/3/5-tick and hidden phases with 0, 1, and 2 frames of async pause release.
    reset();
    const dominus::marker::PauseContext initialGameplay{true,false,false,false,false,false,1,0,1};
    run_unpaused(1910,initialGameplay);
    const unsigned gapDurations[10]={140,180,95,210,130,165,85,240,115,190};
    std::string expectedGen;
    for(unsigned i=0;i<10;++i){
        markerLog.clear();
        const std::uint64_t lease=100+i;
        const auto openWall=qpc_ms();
        const unsigned frozenSt=game_state(openWall);
        const dominus::marker::PauseContext openPause{true,true,true,false,false,false,1,lease,1};
        const unsigned patches=run_paused(260,0,frozenSt,openPause);
        if(i==0){
            for(const auto& line:markerLog){
                const auto pos=line.find("generation=");
                if(line.find("marker,plan,panel=0,valid=1")==0&&pos!=std::string::npos){
                    const auto comma=line.find(',',pos);
                    expectedGen=line.substr(pos,comma-pos);
                    break;
                }
            }
            require(!expectedGen.empty(),"opening #0 records valid plan generation"+marker_plans());
        }
        require(patches>0&&logged("marker,plan,panel=0,valid=1,reason=complete-observed-cycle,images=1,replay=1")&&logged("count=13,periodTicks=39,"+expectedGen+","),
                "static reopening #"+std::to_string(i)+" retains complete 13-state "+expectedGen+" cycle"+marker_plans()+marker_traces());

        const unsigned asyncFrames=i%3; // 0, 1, or 2 frames of releasePending
        if(asyncFrames==0){
            const auto resumeWall=qpc_ms();
            pausedTotalMs+=(resumeWall-openWall);
            const dominus::marker::PauseContext released{true,false,false,true,false,true,1,lease,1};
            const auto st=game_state(resumeWall);
            dominus::marker::on_present(device.device.get(),device.context.get(),sources[st].get(),panels,-1,true,released);
            device.context->Flush();
            require(logged("marker,map-closed,panel=0,replayed=1")&&logged("retained=1,awaitingRelease=0")&&logged("marker,pause-release-confirmed,panel=0,"+expectedGen),
                    "0-frame release confirms immediately"+marker_plans());
            run_unpaused(gapDurations[i],released);
        }else{
            const dominus::marker::PauseContext pending{true,true,true,true,true,false,1,lease,1};
            for(unsigned f=0;f<asyncFrames;++f){
                // Pass movedSource while releasePending is true: must be ignored and never contaminate captures!
                const auto patch=dominus::marker::on_present(device.device.get(),device.context.get(),movedSource.get(),panels,-1,true,pending);
                require(!patch.texture,"no patch while awaiting release");
                device.context->Flush();Sleep(4);
            }
            require(logged("marker,map-closed,panel=0,replayed=1")&&logged("retained=1,awaitingRelease=1")&&!logged("marker,pause-release-confirmed"),
                    "async release waits for confirmation before excluding pause");
            const auto resumeWall=qpc_ms();
            pausedTotalMs+=(resumeWall-openWall);
            const dominus::marker::PauseContext released{true,false,false,true,false,true,1,lease,1};
            run_unpaused(gapDurations[i],released);
            require(logged("marker,pause-release-confirmed,panel=0,"+expectedGen),"async release confirmed on first unpaused frame");
        }
    }

    // 2. Movement / room change immediately after map close invalidates the retained cycle.
    {
        markerLog.clear();
        const auto begin=qpc_ms();auto next=begin;
        const dominus::marker::PauseContext released{true,false,false,true,false,true,1,109,1};
        while(qpc_ms()-begin<120){
            const auto now=qpc_ms();if(now<next){SwitchToThread();continue;}
            dominus::marker::on_present(device.device.get(),device.context.get(),movedSource.get(),panels,-1,true,released);
            device.context->Flush();next=now+4;
        }
        require(logged("marker,cycle-invalidated,panel=0")&&logged("marker,trace,panel=0,event=reset"),
                "post-close movement invalidates retained cycle immediately");
        markerLog.clear();
        const dominus::marker::PauseContext openAfterMove{true,true,true,false,false,false,1,200,1};
        const auto beginOpen=qpc_ms();next=beginOpen;unsigned movePatches=0;
        while(qpc_ms()-beginOpen<220){
            const auto now=qpc_ms();if(now<next){SwitchToThread();continue;}
            const auto patch=dominus::marker::on_present(device.device.get(),device.context.get(),movedSource.get(),panels,0,true,openAfterMove);
            if(patch.texture)++movePatches;
            device.context->Flush();next=now+4;
        }
        require(movePatches==0&&logged("marker,plan,panel=0,valid=0"),"reopening after movement stays frozen until a new 2-lap cycle completes");
    }

    // 3. Involuntary close (voluntaryClose=false), disconnect (connectionEpoch), taskEpoch change, and pause-ownership loss.
    for(int mode=0;mode<4;++mode){
        reset();
        run_unpaused(1910,initialGameplay);
        const auto openWall=qpc_ms();
        const unsigned frozenSt=game_state(openWall);
        const dominus::marker::PauseContext openPause{true,true,true,false,false,false,1,300+mode,1};
        require(run_paused(240,0,frozenSt,openPause)>0,"cycle replays before invalidation mode "+std::to_string(mode));
        markerLog.clear();
        if(mode==0){
            // Involuntary close (e.g. focus lost or ToggleMod)
            const dominus::marker::PauseContext involuntary{true,false,false,false,false,false,1,300+mode,1};
            run_unpaused(150,involuntary);
            require(logged("retained=0,awaitingRelease=0"),"involuntary close refuses retention");
        }else if(mode==1){
            // Controller disconnect during awaitingUnpause
            const dominus::marker::PauseContext pending{true,true,true,true,true,false,1,300+mode,1};
            dominus::marker::on_present(device.device.get(),device.context.get(),sources[frozenSt].get(),panels,-1,true,pending);
            const dominus::marker::PauseContext disconnected{true,false,false,true,false,true,1,300+mode,2};
            run_unpaused(150,disconnected);
            require(logged("marker,retention-dropped,panel=0")&&logged("reason=connection-epoch-changed"),"controller disconnect drops pending retention");
        }else if(mode==2){
            // Task epoch change during awaitingUnpause
            const dominus::marker::PauseContext pending{true,true,true,true,true,false,1,300+mode,1};
            dominus::marker::on_present(device.device.get(),device.context.get(),sources[frozenSt].get(),panels,-1,true,pending);
            const dominus::marker::PauseContext newEpoch{true,false,false,true,false,true,2,300+mode,1};
            run_unpaused(150,newEpoch);
            require(logged("marker,retention-dropped,panel=0")&&logged("reason=task-epoch-changed"),"taskEpoch change drops pending retention");
        }else{
            // Pause ownership lost while map is open, followed by voluntary close attempt
            const dominus::marker::PauseContext lostOwnership{true,true,false,false,false,false,1,300+mode,1};
            dominus::marker::on_present(device.device.get(),device.context.get(),sources[frozenSt].get(),panels,0,true,lostOwnership);
            require(logged("marker,replay-stopped,reason=pause-ownership-lost"),"losing pause ownership stops replay");
            const dominus::marker::PauseContext closeAfterLoss{true,false,false,true,false,true,1,300+mode,1};
            run_unpaused(150,closeAfterLoss);
            require(logged("retained=0,awaitingRelease=0"),"close after ownership loss refuses retention");
        }
        markerLog.clear();
        const unsigned afterOpenSt=game_state(qpc_ms());
        const dominus::marker::PauseContext reopenPause{true,true,true,false,false,false,mode==2?2u:1u,400+mode,mode==1?2u:1u};
        require(run_paused(220,0,afterOpenSt,reopenPause)==0&&logged("marker,plan,panel=0,valid=0"),
                "reopening after invalidation mode "+std::to_string(mode)+" stays frozen");
    }

    // 4. Panel switch (panel 0 retained clears panel 1; opening panel 1 after 150 ms stays frozen) and D3D11 resize.
    reset();
    run_unpaused(1910,initialGameplay);
    {
        const auto openWall=qpc_ms();
        const unsigned frozenSt=game_state(openWall);
        const dominus::marker::PauseContext openPause{true,true,true,false,false,false,1,500,1};
        require(run_paused(240,0,frozenSt,openPause)>0,"panel 0 replays before panel-switch test");
        const auto resumeWall=qpc_ms();
        pausedTotalMs+=(resumeWall-openWall);
        const dominus::marker::PauseContext released{true,false,false,true,false,true,1,500,1};
        run_unpaused(150,released);
        markerLog.clear();
        const unsigned nextSt=game_state(qpc_ms());
        const dominus::marker::PauseContext openPanel1{true,true,true,false,false,false,1,501,1};
        require(run_paused(220,1,nextSt,openPanel1)==0&&logged("marker,plan,panel=1,valid=0,reason=partial-cycle-frozen"),
                "retaining panel 0 does not retain unvalidated panel 1");
        // Resize invalidates all retained state
        auto resizedTex=texture(device,800,600,format);
        markerLog.clear();
        dominus::marker::on_present(device.device.get(),device.context.get(),resizedTex.get(),panels,-1,true,released);
        require(logged("reason=resources-changed"),"D3D11 resource change resets marker state");
    }
    dominus::marker::configure({false,true},nullptr,marker_log);
}

// Game running (marker blinking 200/200 ms) -> mod pause with the marker
// invisible -> the kept "on" image comes and goes -> map closed. Then two
// refusals: a panel that keeps changing under the "pause", and a pair that
// differs over a large area (like the Status panel).
void marker_replay_test(Device& device,DXGI_FORMAT format) {
    markerLog.clear();
    dominus::marker::configure({false,true},nullptr,marker_log);
    const auto onBytes=map_frame(format,true,0x95735a),offBytes=map_frame(format,false,0x95735a);
    const auto shadeBytes=map_frame(format,false,0x10508f);
    auto on=texture(device,960,540,format,&onBytes),off=texture(device,960,540,format,&offBytes),shade=texture(device,960,540,format,&shadeBytes);
    const dominus::gfx::SourceRect panels[2]={UpperPanel,LowerPanel};
    const auto start=qpc_ms();
    auto run=[&](ULONGLONG ms,int mapPanel,const std::function<ID3D11Texture2D*(ULONGLONG)>& frame,unsigned* with,unsigned* without,ID3D11Texture2D** lastPatch){
        const auto begin=qpc_ms();auto next=begin;
        while(qpc_ms()-begin<ms){
            const auto now=qpc_ms();if(now<next){SwitchToThread();continue;}
            const auto patch=dominus::marker::on_present(device.device.get(),device.context.get(),frame(qpc_ms()-start),panels,mapPanel,true);
            if(patch.texture){if(with)++*with;if(lastPatch)*lastPatch=patch.texture;require(patch.left>=640&&patch.left<960&&patch.top>=30&&patch.top<270,"patch position");}
            else if(without)++*without;
            device.context->Flush();next=now+4;
        }
    };
    auto blinking=[&](ULONGLONG t){return ((t/200)%2)?off.get():on.get();};
    run(1400,-1,blinking,nullptr,nullptr,nullptr);
    // Wait for an "off" phase, then freeze there.
    while(((qpc_ms()-start)/200)%2==0){dominus::marker::on_present(device.device.get(),device.context.get(),on.get(),panels,-1,true);device.context->Flush();Sleep(4);}
    // The pause freezes a phase already observed while running. MapOpen
    // captures only verify it and cannot introduce an unseen phase.
    run(24,-1,[&](ULONGLONG){return off.get();},nullptr,nullptr,nullptr);
    unsigned with=0,without=0;ID3D11Texture2D* lastPatch=nullptr;
    run(1600,0,[&](ULONGLONG){return off.get();},&with,&without,&lastPatch);
    require(logged("marker,plan,panel=0,valid=1,reason=complete-observed-cycle,images=1,replay=1"),"plan accepted while frozen off"+marker_plans());
    require(with>10&&without>10,"other image shown part of the time ("+std::to_string(with)+"/"+std::to_string(without)+")");
    require(lastPatch,"patch texture");
    const auto kept=readback(device,lastPatch);D3D11_TEXTURE2D_DESC patchDesc{};lastPatch->GetDesc(&patchDesc);
    // The two-state compatibility case now uses the same union-only patches.
    bool matchesOn=false;
    for(unsigned top=125;top<=131&&!matchesOn;++top)for(unsigned left=760;left<=767&&!matchesOn;++left)
        matchesOn=patch_matches(kept,patchDesc.Width,patchDesc.Height,left,top,onBytes);
    require(matchesOn&&patchDesc.Width<10&&patchDesc.Height<10,"replayed pixels are the game's own 'on' marker patch");
    unsigned after=0;
    run(100,-1,[&](ULONGLONG){return off.get();},&after,nullptr,nullptr);
    require(after==0&&logged("marker,map-closed,panel=0,replayed=1"),"closing the map stops the replay");
    // Still alternating under an open map: any replay stops at the change.
    run(1400,-1,blinking,nullptr,nullptr,nullptr);
    run(1200,0,blinking,nullptr,nullptr,nullptr);
    require(logged("marker,replay-stopped,reason=panel-changed-under-pause")||logged("reason=panel-not-settling"),"changing panel never keeps replaying");
    run(100,-1,blinking,nullptr,nullptr,nullptr);
    // Lower panel alternating over its whole area, then frozen: refused.
    dominus::marker::configure({true,true},nullptr,marker_log);markerLog.clear();
    auto status=[&](ULONGLONG t){return ((t/200)%2)?shade.get():off.get();};
    run(1400,-1,status,nullptr,nullptr,nullptr);
    unsigned refusedWith=0;
    run(800,1,[&](ULONGLONG){return off.get();},&refusedWith,nullptr,nullptr);
    require(refusedWith==0&&logged("marker,trace,panel=1,event=reset")&&logged("marker,plan,panel=1,valid=0")&&logged("images=0,replay=0,confidence=frozen,count=0"),"large alternation resets history and remains frozen"+marker_plans());
    run(50,-1,[&](ULONGLONG){return off.get();},nullptr,nullptr,nullptr);
}

// The real Windows title platform in a host that is not game.exe: no task at
// the game's global, so no title is ever reported, whatever task is claimed,
// with the default set (Portrait) and with experimental Dawn enabled.
void title_identity_fails_closed_outside_the_game() {
    for(const bool dawn:{false,true}){
        dominus::title::configure({true,dawn});
        for(const std::uintptr_t task:{std::uintptr_t{0},std::uintptr_t{0x10000},reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr))}){
            for(std::uint64_t epoch=0;epoch<3;++epoch){
                const auto observed=dominus::title::observe(task,epoch);
                require(!observed.portrait&&observed.title==dominus::Title::Unknown&&observed.reason==dominus::TitleReason::NoTask,"non-game host reports no EmuTask and no title");
            }
        }
    }
    dominus::title::configure({});
    dominus::TitleObservation claimed{};
    claimed.title=dominus::Title::PortraitOfRuin;claimed.portrait=true;claimed.fields.module=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    require(!dominus::title::portrait_scene(claimed).available,"scene fields are never read from a module that is not the loaded dra02.dll");
    require(!dominus::title::scene(claimed).available,"nor through the title dispatch");
    claimed.title=dominus::Title::DawnOfSorrow;claimed.portrait=false;
    require(!dominus::title::dawn_scene(claimed).available,"scene fields are never read from a module that is not the loaded dra01.dll");
    require(!dominus::title::scene(claimed).available,"nor through the title dispatch for Dawn");
    require(!dominus::title::portrait_scene(claimed).available,"Portrait fields are never read for another verified title");
    require(!dominus::title::portrait_scene(dominus::TitleObservation{}).available,"no scene without a verified title");
    require(!dominus::title::scene(dominus::TitleObservation{}).available,"no scene of an unknown title");
    require(!GetModuleHandleW(L"dra02.dll")&&!GetModuleHandleW(L"dra01.dll"),"the check loaded no game module");
}

struct Runner {
    unsigned passed=0,failed=0;
    void run(const std::string& name,const std::function<void()>& body) {
        try{body();++passed;std::cout<<"PASS "<<name<<"\n";}
        catch(const std::exception& error){++failed;std::cerr<<"FAIL "<<name<<": "<<error.what()<<"\n";}
    }
};
}

int main(int argc,char** argv) {
    try {
        require(argc==1||(argc==3&&(std::string(argv[1])=="--proxy"||std::string(argv[1])=="--proxy-only"||std::string(argv[1])=="--guard")),"usage: dominus_d3d11_tests.exe [--proxy absolute-dll-path | --proxy-only absolute-dll-path | --guard absolute-dll-path]");
        Runner tests;
        if(argc==3&&std::string(argv[1])=="--guard") {
            tests.run("unknown-process guard and system forwarding",[&]{guard_test(widen(argv[2]));});
            std::cout<<"Guard result: "<<tests.passed<<" passed, "<<tests.failed<<" failed. No game or Steam execution.\n";
            return tests.failed?1:0;
        }
        if(argc==3&&std::string(argv[1])=="--proxy-only") {
            tests.run("proxy CreateDevice+factory and CreateDeviceAndSwapChain, lifecycle",[&]{proxy_tests(widen(argv[2]));});
            std::cout<<"Proxy result: "<<tests.passed<<" passed, "<<tests.failed<<" failed. No game or Steam execution.\n";
            return tests.failed?1:0;
        }
        tests.run("title identity fails closed outside game.exe",[&]{title_identity_fails_closed_outside_the_game();});
        {
            Library d3d(system_dll(L"d3d11.dll"));
            Library compiler(system_dll(L"d3dcompiler_47.dll"));
            auto device=create_warp(d3d.proc<CreateDeviceFn>("D3D11CreateDevice"));
            dominus::gfx::Presenter presenter;
            tests.run("RGBA 640x360: original, invalid, nearest ROI, bars, map, integer, deferred rejection",[&]{pixel_tests(device,presenter,640,360,DXGI_FORMAT_R8G8B8A8_UNORM);});
            tests.run("RGBA resize 800x600: resource recreation and pixels",[&]{pixel_tests(device,presenter,800,600,DXGI_FORMAT_R8G8B8A8_UNORM);});
            tests.run("BGRA format transition 640x360 and pixels",[&]{pixel_tests(device,presenter,640,360,DXGI_FORMAT_B8G8R8A8_UNORM);});
            tests.run("BGRA resize 800x600 and pixels",[&]{pixel_tests(device,presenter,800,600,DXGI_FORMAT_B8G8R8A8_UNORM);});
            tests.run("graphics state restoration and predication-safe drawing",[&]{state_test(device,compiler.proc<CompileFn>("D3DCompile"));});
            tests.run("six-stage high-slot aliased SRV and Context1 CB subrange restoration",[&]{extended_binding_test(device);});
            tests.run("CS/OM UAV rejection preserves pixels and bindings",[&]{uav_rejection_test(device);});
            tests.run("panel sampling BGRA, map on top",[&]{panel_sampling_test(device,DXGI_FORMAT_B8G8R8A8_UNORM,true);});
            tests.run("panel sampling RGBA, map below",[&]{panel_sampling_test(device,DXGI_FORMAT_R8G8B8A8_UNORM,false);});
            tests.run("panel probe BGRA: async DS samples, keep, compositor-only patch",[&]{panel_probe_test(device,DXGI_FORMAT_B8G8R8A8_UNORM);});
            tests.run("panel probe RGBA: async DS samples, keep, compositor-only patch",[&]{panel_probe_test(device,DXGI_FORMAT_R8G8B8A8_UNORM);});
            tests.run("marker cycle BGRA: 13/39 native cadence, union patch, 16 slots, translation and resize",[&]{marker_cycle_pixels_test(device,DXGI_FORMAT_B8G8R8A8_UNORM);});
            tests.run("marker cycle RGBA: native pixels and background outside the union",[&]{marker_cycle_pixels_test(device,DXGI_FORMAT_R8G8B8A8_UNORM);});
            tests.run("marker cycle adapter: both panels, near-hidden opening, partial and MapOpen refusal",[&]{marker_multistate_replay_test(device,DXGI_FORMAT_B8G8R8A8_UNORM);});
            tests.run("marker cycle retention: 10 static reopenings, async release, movement/epoch/disconnect/panel/resize invalidation",[&]{marker_cycle_retention_test(device,DXGI_FORMAT_B8G8R8A8_UNORM);});
            tests.run("marker blink BGRA: replay under pause, stop on change, refuse large",[&]{marker_replay_test(device,DXGI_FORMAT_B8G8R8A8_UNORM);});
            presenter.reset();
            tests.run("reset and recomposition",[&]{pixel_tests(device,presenter,640,360,DXGI_FORMAT_R8G8B8A8_UNORM);});
            device.context->ClearState();device.context->Flush();
        }
        if(argc==3)tests.run("proxy exports, hidden swapchain, Present/Resize/toggle/Shutdown",[&]{proxy_tests(widen(argv[2]));});
        else std::cout<<"SKIP proxy DLL coverage (provide --proxy absolute-path once compiled)\n";
        std::cout<<"Synthetic WARP result: "<<tests.passed<<" passed, "<<tests.failed<<" failed. No game or Steam execution.\n";
        return tests.failed?1:0;
    } catch(const std::exception& error) { std::cerr<<"Synthetic host setup failed: "<<error.what()<<"\n";return 2; }
}
