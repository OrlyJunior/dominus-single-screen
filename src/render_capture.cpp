#include "dominus/render_capture.hpp"
#include "../third_party/renderdoc/renderdoc_app.h"
#include <bcrypt.h>
#include <array>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace dominus::capture {
namespace {
std::mutex mutex;
Log logger = nullptr;
RENDERDOC_API_1_6_0* api = nullptr;
bool attempted = false, capturing = false;
ID3D11Device* captureDevice = nullptr;
HWND captureWindow = nullptr;
std::wstring live, library;
ULONGLONG lastPoll = 0;
void log(const char* s) noexcept { if (logger) logger(s); }
bool file(const std::wstring& p) noexcept {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}
std::wstring parent(std::wstring p) {
    const auto slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"" : p.substr(0, slash);
}
std::string utf8(const std::wstring& s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::vector<char> out(static_cast<size_t>(n));
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, out.data(), n, nullptr, nullptr);
    return std::string(out.data());
}
// SHA-256 of the locally validated, Authenticode-signed RenderDoc 1.46 DLL.
// No downloads, alternate DLL search locations, or fallback DLLs are used.
bool verified(HANDLE input) noexcept {
    constexpr unsigned char expected[32] = {
        0x80,0x9d,0xa3,0x8e,0x38,0x67,0xd9,0xfd,0x09,0xcc,0x5c,0x30,0xdd,0x53,0x10,0x50,
        0x0d,0xee,0x75,0xe9,0x99,0x16,0x6d,0x7a,0x14,0xad,0x1e,0xf6,0xe9,0xec,0xa6,0x5d};
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::array<unsigned char,32> digest{};
    bool okay = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
    if (okay) okay = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    unsigned char data[65536]; DWORD count = 0;
    while (okay) {
        if (!ReadFile(input, data, sizeof(data), &count, nullptr)) { okay = false; break; }
        if (!count) break;
        okay = BCryptHashData(hash, data, count, 0) >= 0;
    }
    if (okay) okay = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    for (size_t i=0; okay && i<digest.size(); ++i) okay = digest[i] == expected[i];
    return okay;
}
void paths() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCWSTR>(&initialize), &self)) return;
    wchar_t path[32768]{};
    const DWORD n = GetModuleFileNameW(self, path, 32768);
    if (!n || n >= 32768) return;
    std::wstring build = parent(path);
    const std::wstring work = parent(build);
    const auto leaf = [](const std::wstring& s) { const auto p=s.find_last_of(L"\\/"); return s.substr(p+1); };
    // Deliberately unavailable from an installed game folder. This helper is
    // for the development DLL in work/build, not a redistributable capture tool.
    if (_wcsicmp(leaf(build).c_str(), L"build") || _wcsicmp(leaf(work).c_str(), L"work")) {
        log("renderdoc unavailable: diagnostic DLL is outside work/build"); return;
    }
    live = work + L"\\live";
    wchar_t overridePath[32768]{};
    const DWORD len = GetEnvironmentVariableW(L"DSS_LIVE_DIR", overridePath, 32768);
    // Only accept an absolute descendant of this workspace's work directory.
    wchar_t normalized[32768]{};
    const DWORD full = len && len < 32768 ? GetFullPathNameW(overridePath,32768,normalized,nullptr) : 0;
    if (full && full < 32768 && _wcsnicmp(normalized, (work + L"\\").c_str(), work.size()+1)==0)
        live = normalized;
    library = work + L"\\re-tools\\RenderDoc_1.46_64\\renderdoc.dll";
}
}

void initialize(Log output) noexcept {
    try { std::lock_guard<std::mutex> lock(mutex); logger=output; if (live.empty()) paths(); }
    catch (...) { log("renderdoc initialization failed"); }
}

void before_create_device() noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex);
        if (attempted || live.empty() || !file(live + L"\\capture.enable")) return;
        attempted=true;
        HANDLE input = CreateFileW(library.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (input == INVALID_HANDLE_VALUE) { log("renderdoc unavailable: missing DLL"); return; }
        const bool trusted = verified(input);
        HMODULE module = trusted ? LoadLibraryExW(library.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32) : nullptr;
        CloseHandle(input);
        if (!module) { log("renderdoc unavailable: DLL hash or load failed"); return; }
        auto get = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(module,"RENDERDOC_GetAPI"));
        if (!get || get(eRENDERDOC_API_Version_1_6_0,reinterpret_cast<void**>(&api)) != 1 || !api) {
            log("renderdoc unavailable: API 1.6.0 missing"); return;
        }
        const std::wstring captures = live + L"\\captures";
        const DWORD liveAttributes=GetFileAttributesW(live.c_str());
        const DWORD captureAttributes=GetFileAttributesW(captures.c_str());
        if (liveAttributes==INVALID_FILE_ATTRIBUTES || !(liveAttributes&FILE_ATTRIBUTE_DIRECTORY) ||
            (liveAttributes&FILE_ATTRIBUTE_REPARSE_POINT) ||
            (captureAttributes!=INVALID_FILE_ATTRIBUTES &&
             (!(captureAttributes&FILE_ATTRIBUTE_DIRECTORY) || (captureAttributes&FILE_ATTRIBUTE_REPARSE_POINT)))) {
            api=nullptr; log("renderdoc unavailable: unsafe capture directory"); return;
        }
        if (!CreateDirectoryW(captures.c_str(), nullptr) && GetLastError()!=ERROR_ALREADY_EXISTS) {
            api=nullptr; log("renderdoc unavailable: capture directory creation failed"); return;
        }
        const std::string target = utf8(captures + L"\\dominus");
        api->SetCaptureFilePathTemplate(target.c_str());
        api->SetCaptureOptionU32(eRENDERDOC_Option_CaptureCallstacks,1);
        api->SetCaptureOptionU32(eRENDERDOC_Option_AllowFullscreen,1);
        api->SetCaptureKeys(nullptr,0);
        api->SetFocusToggleKeys(nullptr,0);
        api->MaskOverlayBits(eRENDERDOC_Overlay_None,eRENDERDOC_Overlay_None);
        log("renderdoc loaded: capture.request arms one frame; existing devices unsupported");
    } catch (...) { log("renderdoc load failed with exception"); }
}

void on_present(ID3D11Device* device, HWND window) noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex);
        if (!api || !device) return;
        if (capturing) {
            if (device != captureDevice || window != captureWindow) return;
            const uint32_t okay=api->EndFrameCapture(captureDevice,captureWindow);
            captureDevice->Release(); captureDevice=nullptr; captureWindow=nullptr; capturing=false;
            char line[2048]{}; char path[1800]{}; uint32_t size=sizeof(path); uint64_t stamp=0;
            const uint32_t count=api->GetNumCaptures();
            if (okay && count && api->GetCapture(count-1,path,&size,&stamp))
                std::snprintf(line,sizeof(line),"renderdoc capture complete: %s",path);
            else std::snprintf(line,sizeof(line),"renderdoc capture failed: EndFrameCapture=%u",okay);
            log(line); return;
        }
        const ULONGLONG now=GetTickCount64();
        if (now-lastPoll < 250) return;
        lastPoll=now;
        const std::wstring request=live+L"\\capture.request";
        if (!file(request) || !DeleteFileW(request.c_str())) return;
        if (api->IsFrameCapturing()) { log("renderdoc request ignored: capture already active"); return; }
        api->StartFrameCapture(device,window);
        if (!api->IsFrameCapturing()) { log("renderdoc could not start: device must be created after RenderDoc load"); return; }
        captureDevice=device; device->AddRef(); captureWindow=window; capturing=true;
        log("renderdoc one-frame capture started");
    } catch (...) { log("renderdoc present handler failed with exception"); }
}
}
