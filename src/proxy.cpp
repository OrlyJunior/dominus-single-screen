#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <shlobj.h>
#include "MinHook.h"
#include "dominus/core.hpp"
#include "dominus/graphics.hpp"
#include "dominus/runtime_api.h"
#include "dominus/native_runtime.hpp"
#include "dominus/render_capture.hpp"
#include "dominus/resource_observer.hpp"
#include "dominus/input_observer.hpp"
#include "dominus/live_controller.hpp"
#include "dominus/marker_blink.hpp"
#include "dominus/title_identity.hpp"
#include "dominus/pointer_remap.hpp"
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <cstdio>
#include <cstdlib>

namespace {
using CreateDeviceFn=decltype(&D3D11CreateDevice);
using CreateBothFn=decltype(&D3D11CreateDeviceAndSwapChain);
using CreateSwapFn=HRESULT (STDMETHODCALLTYPE*)(IDXGIFactory*,IUnknown*,DXGI_SWAP_CHAIN_DESC*,IDXGISwapChain**);
using CreateHwndFn=HRESULT (STDMETHODCALLTYPE*)(IDXGIFactory2*,IUnknown*,HWND,const DXGI_SWAP_CHAIN_DESC1*,const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,IDXGIOutput*,IDXGISwapChain1**);
using PresentFn=HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,UINT);
using Present1Fn=HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain1*,UINT,UINT,const DXGI_PRESENT_PARAMETERS*);
using ResizeFn=HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,UINT,UINT,DXGI_FORMAT,UINT);
using CreateOn12Fn=HRESULT (WINAPI*)(IUnknown*,UINT,const D3D_FEATURE_LEVEL*,UINT,IUnknown* const*,UINT,UINT,ID3D11Device**,ID3D11DeviceContext**,D3D_FEATURE_LEVEL*);
using KnownFolderFn=HRESULT (WINAPI*)(REFKNOWNFOLDERID,DWORD,HANDLE,PWSTR*);

struct Chain { std::mutex mutex; dominus::gfx::Presenter presenter; };
struct Runtime {
    std::once_flag load;
    HMODULE system=nullptr;
    CreateDeviceFn create=nullptr;CreateBothFn createBoth=nullptr;CreateOn12Fn createOn12=nullptr;
    bool supported=false,synthetic=false,observer=false;
    std::atomic<bool> stopped{false};
    std::atomic<std::uint64_t> presents{0},compositions{0},fallbacks{0},resizes{0};
    std::mutex installMutex,chainsMutex,requestMutex;
    std::unordered_map<IDXGISwapChain*,std::shared_ptr<Chain>> chains;
    dominus::gfx::Request request{};
    CreateSwapFn createSwap=nullptr;CreateHwndFn createHwnd=nullptr;
    PresentFn present=nullptr;Present1Fn present1=nullptr;ResizeFn resize=nullptr;
    bool minhook=false;
    bool nativeObserver=false;
    bool liveControls=false,nativePause=false,startEnabled=false;
    // Experimental Dawn of Sorrow profile; off keeps Dawn inert as in v0.1.1
    // and installs no pointer hook.
    bool dawn=false,pointer=false;
    // Diagnostic only: logs D3D11 calls inside DrawGpuScreen, which no session
    // has reached (draw=0). Off unless the INI asks for it.
    bool resourceObserver=false;
    // Map marker blink during the mod's pause (marker_blink.hpp).
    bool markerTrace=false,markerReplay=false;
    // Proxy mode waits for the EmuTask before hooking (see start_native).
    bool deferNative=true;
    std::atomic<bool> nativeStarted{false};
    HANDLE logFile=INVALID_HANDLE_VALUE;
    std::mutex logMutex;
    std::wstring liveDirectory;
    DWORD lastRequestRead=0;
};
// Process-lifetime storage avoids COM destruction and loader operations inside
// DLL_PROCESS_DETACH. The owned synthetic host explicitly shuts it down.
Runtime& rt(){static auto* value=new Runtime;return *value;}
void diagnostic(const char* line) noexcept {
    auto& r=rt();if(r.logFile==INVALID_HANDLE_VALUE)return;
    std::lock_guard<std::mutex> lock(r.logMutex);char prefix[64]{};
    const int n=std::snprintf(prefix,sizeof(prefix),"%llu,%lu,",static_cast<unsigned long long>(GetTickCount64()),static_cast<unsigned long>(GetCurrentThreadId()));
    DWORD written=0;WriteFile(r.logFile,prefix,static_cast<DWORD>(n),&written,nullptr);WriteFile(r.logFile,line,static_cast<DWORD>(std::strlen(line)),&written,nullptr);WriteFile(r.logFile,"\r\n",2,&written,nullptr);
}

std::wstring module_path(HMODULE module){
    std::wstring path(32768,L'\0');const DWORD n=GetModuleFileNameW(module,path.data(),static_cast<DWORD>(path.size()));
    if(!n||n>=path.size())return {};path.resize(n);return path;
}
bool supported_hash(const std::wstring& path){
    HANDLE f=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
    if(f==INVALID_HANDLE_VALUE)return false;
    BCRYPT_ALG_HANDLE alg=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
    std::array<unsigned char,32> digest{};std::array<unsigned char,65536> chunk{};
    bool ok=BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0;
    if(ok)ok=BCryptCreateHash(alg,&hash,nullptr,0,nullptr,0,0)>=0;
    DWORD count=0;
    while(ok){if(!ReadFile(f,chunk.data(),static_cast<DWORD>(chunk.size()),&count,nullptr)){ok=false;break;}if(!count)break;ok=BCryptHashData(hash,chunk.data(),count,0)>=0;}
    if(ok)ok=BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0)>=0;
    if(hash)BCryptDestroyHash(hash);if(alg)BCryptCloseAlgorithmProvider(alg,0);CloseHandle(f);
    constexpr unsigned char expected[]={0xe6,0xfa,0x88,0x5f,0x56,0x0c,0x8a,0x47,0x64,0x0f,0xf8,0xdf,0x4d,0xe3,0xce,0x4a,0x93,0x8c,0x80,0x53,0xeb,0x11,0xa9,0x0a,0x7b,0x65,0xf5,0xeb,0xf1,0xab,0x6b,0x64};
    return ok&&std::memcmp(digest.data(),expected,sizeof(expected))==0;
}
void initialize(){
    auto& r=rt();std::call_once(r.load,[&]{
        wchar_t system[MAX_PATH+32]{};UINT n=GetSystemDirectoryW(system,MAX_PATH);
        if(n&&n<MAX_PATH){wcscat_s(system,L"\\d3d11.dll");r.system=LoadLibraryExW(system,nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);}
        if(r.system){r.create=reinterpret_cast<CreateDeviceFn>(GetProcAddress(r.system,"D3D11CreateDevice"));r.createBoth=reinterpret_cast<CreateBothFn>(GetProcAddress(r.system,"D3D11CreateDeviceAndSwapChain"));r.createOn12=reinterpret_cast<CreateOn12Fn>(GetProcAddress(r.system,"D3D11On12CreateDevice"));}
        const auto host=module_path(nullptr);const auto slash=host.find_last_of(L"\\/");
        const auto name=host.substr(slash==std::wstring::npos?0:slash+1);
        // This export is accepted ONLY in our exact, separately built test host.
        // There is deliberately no INI flag that enables native test controls.
        if(name==L"dominus_d3d11_tests.exe"){
            using Proof=unsigned (__cdecl*)();auto proof=reinterpret_cast<Proof>(GetProcAddress(GetModuleHandleW(nullptr),"DominusSyntheticHostProof"));
            r.synthetic=proof&&proof()==0x44535331u;
        }
        r.supported=name==L"game.exe"&&supported_hash(host);
        if(r.synthetic)r.observer=true;
        else if(r.supported){
            HMODULE self=nullptr;
            if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&initialize),&self)){
                auto ini=module_path(self);auto p=ini.find_last_of(L"\\/");
                if(p!=std::wstring::npos){
                    const auto directory=ini.substr(0,p);ini.resize(p+1);ini+=L"dominus-single-screen.ini";
                    r.observer=GetPrivateProfileIntW(L"runtime",L"EnableObserver",0,ini.c_str())==1;
                    r.nativeObserver=GetPrivateProfileIntW(L"runtime",L"EnableNativeObserver",0,ini.c_str())==1;
                    r.liveControls=GetPrivateProfileIntW(L"runtime",L"EnablePortraitTestControls",0,ini.c_str())==1;
                    r.nativePause=GetPrivateProfileIntW(L"runtime",L"EnableNativePause",0,ini.c_str())==1;
                    r.startEnabled=GetPrivateProfileIntW(L"runtime",L"StartEnabled",0,ini.c_str())==1;
                    r.resourceObserver=GetPrivateProfileIntW(L"runtime",L"EnableResourceObserver",0,ini.c_str())==1;
                    r.deferNative=GetPrivateProfileIntW(L"runtime",L"DeferNativeHooks",1,ini.c_str())!=0;
                    r.markerTrace=GetPrivateProfileIntW(L"runtime",L"EnableMarkerTrace",0,ini.c_str())==1;
                    r.markerReplay=GetPrivateProfileIntW(L"runtime",L"EnableMarkerBlink",0,ini.c_str())==1;
                    r.dawn=GetPrivateProfileIntW(L"runtime",L"EnableExperimentalDawn",0,ini.c_str())==1;
                    dominus::title::configure({true,r.dawn});
                    wchar_t live[32768]{};GetPrivateProfileStringW(L"runtime",L"DiagnosticDirectory",L"",live,32768,ini.c_str());
                    // Installed next to game.exe, logs must never land in the
                    // Steam library: default to the user's local app data, as
                    // the shell reports it (independent of the environment).
                    // Windows redirects it for packaged parents: Steam started
                    // from an MSIX app writes into that app's LocalCache.
                    if(*live)r.liveDirectory=live;
                    else if(HMODULE shell=LoadLibraryExW(L"shell32.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32)){
                        static constexpr KNOWNFOLDERID LocalAppData={0xf1b32785,0x6fba,0x4fcf,{0x9d,0x55,0x7b,0x8e,0x7f,0x15,0x70,0x91}};
                        PWSTR local=nullptr;auto known=reinterpret_cast<KnownFolderFn>(GetProcAddress(shell,"SHGetKnownFolderPath"));
                        if(known&&SUCCEEDED(known(LocalAppData,0,nullptr,&local))&&local)r.liveDirectory=std::wstring(local)+L"\\DominusSingleScreen";
                        CoTaskMemFree(local);
                    }
                    if(r.observer&&!r.liveDirectory.empty()){
                        CreateDirectoryW(r.liveDirectory.c_str(),nullptr);auto path=r.liveDirectory+L"\\runtime-"+std::to_wstring(GetCurrentProcessId())+L".log";
                        r.logFile=CreateFileW(path.c_str(),FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
                        diagnostic("observer-configured");
                        diagnostic(r.dawn?"title,configured,portrait=1,dawn=1-experimental":"title,configured,portrait=1,dawn=0");
                    }
                    if(r.liveControls)dominus::marker::configure({r.markerTrace,r.markerReplay,true},r.liveDirectory.c_str(),diagnostic);
                }
            }
        }
        OutputDebugStringW(r.synthetic?L"[Dominus] Synthetic host: D3D11 tests enabled.\n":L"[Dominus] Native integration unvalidated; original presentation and input retained.\n");
    });
}
template<class T> void* address(T* object,unsigned slot){return (*reinterpret_cast<void***>(object))[slot];}
bool hook(void* target,void* replacement,void** original){
    auto& r=rt();if(!r.minhook){auto s=MH_Initialize();if(s!=MH_OK&&s!=MH_ERROR_ALREADY_INITIALIZED)return false;r.minhook=true;}
    if(*original)return true;
    if(MH_CreateHook(target,replacement,original)!=MH_OK)return false;
    if(MH_EnableHook(target)!=MH_OK){MH_RemoveHook(target);*original=nullptr;return false;}return true;
}
// Reads the right-hand panels for the marker blink before composition
// overwrites the frame; a returned patch only affects the compositor's copy.
void follow_markers(IDXGISwapChain* chain,dominus::gfx::Request& request){
    if(!dominus::marker::active())return;
    ID3D11Device* device=nullptr;ID3D11DeviceContext* context=nullptr;ID3D11Texture2D* texture=nullptr;
    if(SUCCEEDED(chain->GetDevice(__uuidof(ID3D11Device),reinterpret_cast<void**>(&device)))){
        device->GetImmediateContext(&context);
        if(context&&SUCCEEDED(chain->GetBuffer(0,__uuidof(ID3D11Texture2D),reinterpret_cast<void**>(&texture)))){
            dominus::gfx::SourceRect panels[2];dominus::live::panel_rects(panels[0],panels[1]);
            // Only a title with validated replay is tracked (Portrait); the map
            // panel stays the map's even while the view shows the other page.
            const auto target=dominus::live::marker_target();
            const bool tracking=target.tracking&&request.mode!=dominus::gfx::Mode::Original;
            const int mapPanel=tracking&&request.mode==dominus::gfx::Mode::Map?target.mapPanel:-1;
            const auto patch=dominus::marker::on_present(device,context,texture,panels,mapPanel,tracking,dominus::live::pause_context());
            request.patch=patch.texture;request.patchLeft=patch.left;request.patchTop=patch.top;
        }
    }
    if(texture)texture->Release();if(context)context->Release();if(device)device->Release();
}
void compose(IDXGISwapChain* chain,UINT flags){
    auto& r=rt();++r.presents;
    if((flags&DXGI_PRESENT_TEST)||r.stopped)return;
    if(!r.synthetic&&!r.supported)return;
    if(r.supported){
        const auto count=r.presents.load();
        if(count<=3||count%600==0){DXGI_SWAP_CHAIN_DESC desc{};chain->GetDesc(&desc);auto status=dominus::native::status_snapshot();char line[320]{};
            std::snprintf(line,sizeof(line),"present,frames=%llu,chain=0x%llx,size=%ux%u,format=%u,native=%u,draw=%llu,input=%llu,paused=%u",static_cast<unsigned long long>(count),reinterpret_cast<unsigned long long>(chain),desc.BufferDesc.Width,desc.BufferDesc.Height,desc.BufferDesc.Format,status.hooksInstalled,static_cast<unsigned long long>(status.drawCalls),static_cast<unsigned long long>(status.inputUpdates),status.paused);diagnostic(line);
        }
        const DWORD now=GetTickCount();
        if(!r.liveControls&&now-r.lastRequestRead>=120&&!r.liveDirectory.empty()){
            r.lastRequestRead=now;const auto file=r.liveDirectory+L"\\presentation.ini";dominus::gfx::Request next{};
            const auto mode=GetPrivateProfileIntW(L"presentation",L"Mode",0,file.c_str());
            if(mode<=2){next.mode=static_cast<dominus::gfx::Mode>(mode);const wchar_t* keys[]={L"X",L"Y",L"Width",L"Height"};float* values[]={&next.source.x,&next.source.y,&next.source.width,&next.source.height};
                for(unsigned i=0;i<4;++i){wchar_t text[64]{};GetPrivateProfileStringW(L"presentation",keys[i],i<2?L"0":L"1",text,64,file.c_str());*values[i]=std::wcstof(text,nullptr);}
                next.integerScale=GetPrivateProfileIntW(L"presentation",L"IntegerScale",0,file.c_str())!=0;
                if(next.mode==dominus::gfx::Mode::Original||dominus::gfx::valid_rect(next.source)){std::lock_guard<std::mutex> lock(r.requestMutex);r.request=next;}
            }
        }
    }
    dominus::gfx::Request request;
    {std::lock_guard<std::mutex> lock(r.requestMutex);request=r.request;}
    if(r.supported&&r.liveControls){DXGI_SWAP_CHAIN_DESC desc{};chain->GetDesc(&desc);request=dominus::live::frame(desc.OutputWindow);follow_markers(chain,request);}
    // Touch-remap titles: hand the pointer adapter what this Present really
    // shows (a failed composition leaves the original layout: no remap).
    struct PointerScope{dominus::pointer::View view{};bool on=false;~PointerScope(){if(on)dominus::pointer::set_view(view);}} pointer;
    bool pointerRemap=false;
    if(r.pointer){const auto policy=dominus::live::pointer_policy();pointer.on=true;pointer.view.hide=policy.hide;pointerRemap=policy.remap;}
    if(request.mode==dominus::gfx::Mode::Original)return;
    std::shared_ptr<Chain> state;
    {std::lock_guard<std::mutex> lock(r.chainsMutex);auto i=r.chains.find(chain);if(i!=r.chains.end())state=i->second;else if(r.chains.size()<16){state=std::make_shared<Chain>();r.chains.emplace(chain,state);}}
    if(!state){++r.fallbacks;return;}
    std::lock_guard<std::mutex> lock(state->mutex);
    ID3D11Device* device=nullptr;ID3D11DeviceContext* context=nullptr;ID3D11Texture2D* texture=nullptr;
    if(SUCCEEDED(chain->GetDevice(__uuidof(ID3D11Device),reinterpret_cast<void**>(&device)))){
        device->GetImmediateContext(&context);
        if(SUCCEEDED(chain->GetBuffer(0,__uuidof(ID3D11Texture2D),reinterpret_cast<void**>(&texture)))){
            // Sample the original layout before our composition overwrites it.
            dominus::gfx::SourceRect first{},second{};
            if(r.supported&&r.liveControls&&dominus::live::panels_wanted(first,second)){
                std::array<unsigned,dominus::PanelColumns*dominus::PanelRows> a{},b{};
                const bool sampled=dominus::gfx::sample_grid(device,context,texture,first,dominus::PanelColumns,dominus::PanelRows,a.data())&&
                                   dominus::gfx::sample_grid(device,context,texture,second,dominus::PanelColumns,dominus::PanelRows,b.data());
                dominus::live::panels_sampled(sampled?a.data():nullptr,sampled?b.data():nullptr,static_cast<unsigned>(a.size()));
            }
            if(state->presenter.compose(device,context,texture,request)){
                ++r.compositions;
                if(pointerRemap&&request.mode==dominus::gfx::Mode::Gameplay){
                    D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
                    pointer.view.remap=true;pointer.view.backWidth=static_cast<int>(desc.Width);pointer.view.backHeight=static_cast<int>(desc.Height);
                    pointer.view.shown=dominus::fit_4_3(pointer.view.backWidth,pointer.view.backHeight,request.integerScale?dominus::ScaleMode::Integer:dominus::ScaleMode::Fit);
                    pointer.view.source={request.source.x,request.source.y,request.source.width,request.source.height};
                }
            }else {const auto failures=++r.fallbacks;if(failures<=3||failures%600==0){D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);char line[240]{};std::snprintf(line,sizeof(line),"compose-rejected,fl=0x%x,size=%ux%u,fmt=%u,samples=%u,failures=%llu",unsigned(device->GetFeatureLevel()),desc.Width,desc.Height,unsigned(desc.Format),desc.SampleDesc.Count,static_cast<unsigned long long>(failures));diagnostic(line);}}
        }else ++r.fallbacks;
    }else ++r.fallbacks;
    if(texture)texture->Release();if(context)context->Release();if(device)device->Release();
}
// Same live-control start-up for the late-attach entry and for the installed
// proxy (d3d11.dll next to game.exe), once per process, after native hooks.
void start_live_controls(){
    static std::once_flag once;auto& r=rt();
    if(!r.supported||!r.liveControls)return;
    // The collection receives the Xbox pad through Steam Input, not XInput.
    // The pointer adapter exists only with the experimental Dawn profile.
    std::call_once(once,[&]{dominus::native::enable_pause_commands(r.nativePause);dominus::input::initialize(diagnostic);dominus::input::initialize_steam(GetModuleHandleW(nullptr),diagnostic);if(r.dawn)r.pointer=dominus::pointer::initialize(GetModuleHandleW(nullptr),diagnostic);dominus::live::initialize(r.liveDirectory.c_str(),r.startEnabled,diagnostic);});
}
// Native hooks, then live controls. Every validated session (late attach)
// installed them after the game had built its EmuTask. As an installed proxy,
// hooking already at device creation left the collection on a black screen
// after the start-up warnings, 4 of 4 launches; then only the constructor and
// the first reinitialization had run hooked. So the proxy polls each Present
// (the thread that builds the task) and starts once the task is ready.
void start_native(bool immediate){
    auto& r=rt();
    if(!r.supported||r.nativeStarted)return;
    if(!immediate&&r.deferNative&&!dominus::native::task_ready(GetModuleHandleW(nullptr)))return;
    if(r.nativeStarted.exchange(true))return;
    if(r.nativeObserver){dominus::native::initialize(GetModuleHandleW(nullptr),diagnostic);if(r.deferNative&&!immediate)diagnostic("native,deferred-start,task-ready");}
    start_live_controls();
}
thread_local bool insidePresent=false;
struct PresentGuard{bool previous=insidePresent;PresentGuard(){insidePresent=true;}~PresentGuard(){insidePresent=previous;}};
void afterPresent(IDXGISwapChain* chain,UINT flags){
    if(!rt().supported||rt().stopped||(flags&DXGI_PRESENT_TEST))return;
    ID3D11Device* device=nullptr;if(SUCCEEDED(chain->GetDevice(__uuidof(ID3D11Device),reinterpret_cast<void**>(&device)))){
        DXGI_SWAP_CHAIN_DESC desc{};chain->GetDesc(&desc);start_native(false);
        if(rt().resourceObserver&&dominus::native::status_snapshot().hooksInstalled){dominus::resources::initialize(device,diagnostic);dominus::resources::on_present();}dominus::capture::on_present(device,desc.OutputWindow);device->Release();
    }
}
HRESULT STDMETHODCALLTYPE onPresent(IDXGISwapChain* chain,UINT sync,UINT flags){
    auto original=rt().present;if(!original)return E_FAIL;
    if(insidePresent)return original(chain,sync,flags);PresentGuard guard;
    try{compose(chain,flags);}catch(...){++rt().fallbacks;}
    const HRESULT hr=original(chain,sync,flags);try{afterPresent(chain,flags);}catch(...){}return hr;
}
HRESULT STDMETHODCALLTYPE onPresent1(IDXGISwapChain1* chain,UINT sync,UINT flags,const DXGI_PRESENT_PARAMETERS* parameters){
    auto original=rt().present1;if(!original)return E_FAIL;
    if(insidePresent)return original(chain,sync,flags,parameters);PresentGuard guard;
    // Partial dirty-rectangle presentations cannot safely describe our full
    // black clear. Preserve them verbatim rather than changing their contract.
    if(!parameters||(!parameters->DirtyRectsCount&&!parameters->pScrollRect&&!parameters->pScrollOffset)){
        try{compose(chain,flags);}catch(...){++rt().fallbacks;}
    }else ++rt().fallbacks;
    const HRESULT hr=original(chain,sync,flags,parameters);try{afterPresent(chain,flags);}catch(...){}return hr;
}
HRESULT STDMETHODCALLTYPE onResize(IDXGISwapChain* chain,UINT count,UINT width,UINT height,DXGI_FORMAT format,UINT flags){
    auto& r=rt();
    try{
        std::shared_ptr<Chain> state;
        {std::lock_guard<std::mutex> lock(r.chainsMutex);auto i=r.chains.find(chain);if(i!=r.chains.end())state=i->second;}
        if(state){std::lock_guard<std::mutex> lock(state->mutex);state->presenter.reset();}
    }catch(...){++r.fallbacks;}
    ++r.resizes;return r.resize?r.resize(chain,count,width,height,format,flags):E_FAIL;
}
void registerChain(IDXGISwapChain* chain){
    auto& r=rt();if(!chain||!r.observer||r.stopped)return;
    std::lock_guard<std::mutex> install(r.installMutex);
    const bool ok=hook(address(chain,8),reinterpret_cast<void*>(&onPresent),reinterpret_cast<void**>(&r.present));
    hook(address(chain,13),reinterpret_cast<void*>(&onResize),reinterpret_cast<void**>(&r.resize));
    IDXGISwapChain1* chain1=nullptr;
    if(SUCCEEDED(chain->QueryInterface(__uuidof(IDXGISwapChain1),reinterpret_cast<void**>(&chain1)))){
        hook(address(chain1,22),reinterpret_cast<void*>(&onPresent1),reinterpret_cast<void**>(&r.present1));chain1->Release();
    }
    if(ok){std::lock_guard<std::mutex> lock(r.chainsMutex);if(r.chains.size()<16)r.chains.emplace(chain,std::make_shared<Chain>());}
}
HRESULT STDMETHODCALLTYPE onCreateSwap(IDXGIFactory* factory,IUnknown* device,DXGI_SWAP_CHAIN_DESC* desc,IDXGISwapChain** output){
    HRESULT hr=rt().createSwap(factory,device,desc,output);if(SUCCEEDED(hr)&&output&&*output)try{registerChain(*output);}catch(...){}return hr;
}
HRESULT STDMETHODCALLTYPE onCreateHwnd(IDXGIFactory2* factory,IUnknown* device,HWND window,const DXGI_SWAP_CHAIN_DESC1* desc,const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* full,IDXGIOutput* restrictOutput,IDXGISwapChain1** output){
    HRESULT hr=rt().createHwnd(factory,device,window,desc,full,restrictOutput,output);if(SUCCEEDED(hr)&&output&&*output)try{registerChain(*output);}catch(...){}return hr;
}
void registerDevice(ID3D11Device* device){
    auto& r=rt();if(!device||!r.observer||r.stopped)return;
    IDXGIDevice* dxgi=nullptr;IDXGIAdapter* adapter=nullptr;IDXGIFactory* factory=nullptr;
    if(SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice),reinterpret_cast<void**>(&dxgi)))&&SUCCEEDED(dxgi->GetAdapter(&adapter))&&SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory),reinterpret_cast<void**>(&factory)))){
        std::lock_guard<std::mutex> install(r.installMutex);
        hook(address(factory,10),reinterpret_cast<void*>(&onCreateSwap),reinterpret_cast<void**>(&r.createSwap));
        IDXGIFactory2* factory2=nullptr;
        if(SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory2),reinterpret_cast<void**>(&factory2)))){
            hook(address(factory2,15),reinterpret_cast<void*>(&onCreateHwnd),reinterpret_cast<void**>(&r.createHwnd));factory2->Release();
        }
    }
    if(factory)factory->Release();if(adapter)adapter->Release();if(dxgi)dxgi->Release();
}
}

// Explicit late attach for the user-authorized live test. It creates a hidden
// owned probe window/device to discover DXGI method addresses; original files,
// input and save data are never changed by this entry point.
extern "C" DWORD WINAPI DominusAttachObserver(void*){
    try{
        initialize();auto& r=rt();if(!r.supported||!r.observer)return 0;
        start_native(true);
        dominus::capture::initialize(diagnostic);
        HWND probe=CreateWindowExW(0,L"STATIC",L"Dominus diagnostic probe",WS_POPUP,0,0,8,8,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        if(!probe){diagnostic("attach-hidden-window-failed");return 0;}
        DXGI_SWAP_CHAIN_DESC desc{};desc.BufferDesc.Width=8;desc.BufferDesc.Height=8;desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=1;desc.OutputWindow=probe;desc.Windowed=TRUE;desc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
        ID3D11Device* device=nullptr;ID3D11DeviceContext* context=nullptr;IDXGISwapChain* chain=nullptr;
        HRESULT hr=r.createBoth(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&desc,&chain,&device,nullptr,&context);
        if(SUCCEEDED(hr)){registerDevice(device);registerChain(chain);diagnostic("late-attach-dxgi-enabled");}
        if(chain)chain->Release();if(context)context->Release();if(device)device->Release();DestroyWindow(probe);return SUCCEEDED(hr)?1:0;
    }catch(...){diagnostic("late-attach-exception");return 0;}
}

extern "C" HRESULT WINAPI ProxyD3D11CreateDevice(IDXGIAdapter* adapter,D3D_DRIVER_TYPE type,HMODULE software,UINT flags,const D3D_FEATURE_LEVEL* levels,UINT levelCount,UINT sdk,ID3D11Device** device,D3D_FEATURE_LEVEL* chosen,ID3D11DeviceContext** context){
    initialize();if(!rt().deferNative)start_native(true);if(rt().supported){dominus::capture::initialize(diagnostic);dominus::capture::before_create_device();}auto f=rt().create;if(!f)return E_FAIL;
    HRESULT hr=f(adapter,type,software,flags,levels,levelCount,sdk,device,chosen,context);
    if(SUCCEEDED(hr)&&device&&*device)try{registerDevice(*device);}catch(...){}return hr;
}
extern "C" HRESULT WINAPI ProxyD3D11CreateDeviceAndSwapChain(IDXGIAdapter* adapter,D3D_DRIVER_TYPE type,HMODULE software,UINT flags,const D3D_FEATURE_LEVEL* levels,UINT levelCount,UINT sdk,const DXGI_SWAP_CHAIN_DESC* desc,IDXGISwapChain** chain,ID3D11Device** device,D3D_FEATURE_LEVEL* chosen,ID3D11DeviceContext** context){
    initialize();if(!rt().deferNative)start_native(true);if(rt().supported){dominus::capture::initialize(diagnostic);dominus::capture::before_create_device();}auto f=rt().createBoth;if(!f)return E_FAIL;
    HRESULT hr=f(adapter,type,software,flags,levels,levelCount,sdk,desc,chain,device,chosen,context);
    if(SUCCEEDED(hr))try{if(device&&*device)registerDevice(*device);if(chain&&*chain)registerChain(*chain);}catch(...){}return hr;
}
// Pass-through only, so Media Foundation (which delay-imports it from
// "d3d11.dll" and so binds to this proxy) gets the system function.
extern "C" HRESULT WINAPI ProxyD3D11On12CreateDevice(IUnknown* device,UINT flags,const D3D_FEATURE_LEVEL* levels,UINT levelCount,IUnknown* const* queues,UINT queueCount,UINT nodeMask,ID3D11Device** output,ID3D11DeviceContext** context,D3D_FEATURE_LEVEL* chosen){
    initialize();auto f=rt().createOn12;return f?f(device,flags,levels,levelCount,queues,queueCount,nodeMask,output,context,chosen):E_NOTIMPL;
}
extern "C" int __cdecl DominusGetStatus(DominusStatus* output){
    if(!output||output->size<sizeof(DominusStatus))return 0;initialize();auto& r=rt();
    DominusStatus s{};s.size=sizeof(s);s.abi_version=1;s.supported_build=r.supported;s.synthetic_host=r.synthetic;s.observer_enabled=r.observer&&!r.stopped;
    s.presents=r.presents;s.compositions=r.compositions;s.fallbacks=r.fallbacks;s.resizes=r.resizes;*output=s;return 1;
}
extern "C" int __cdecl DominusSyntheticConfigure(unsigned mode,float x,float y,float width,float height,int integerScale){
    initialize();auto& r=rt();if(!r.synthetic||r.stopped||mode>2)return 0;
    dominus::gfx::Request request{static_cast<dominus::gfx::Mode>(mode),{x,y,width,height},integerScale!=0};
    if(mode&&!dominus::gfx::valid_rect(request.source))return 0;
    std::lock_guard<std::mutex> lock(r.requestMutex);r.request=request;return 1;
}
extern "C" int __cdecl DominusSyntheticShutdown(){
    initialize();auto& r=rt();if(!r.synthetic)return 0;
    r.stopped=true;
    // Test host calls only after all Present calls and worker threads stopped.
    std::lock_guard<std::mutex> install(r.installMutex);
    if(r.minhook){MH_DisableHook(MH_ALL_HOOKS);MH_Uninitialize();r.minhook=false;}
    std::lock_guard<std::mutex> lock(r.chainsMutex);r.chains.clear();return 1;
}
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID){if(reason==DLL_PROCESS_ATTACH)DisableThreadLibraryCalls(instance);return TRUE;}
