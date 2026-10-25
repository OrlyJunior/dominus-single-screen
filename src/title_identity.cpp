#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include "dominus/title_identity.hpp"
#include <array>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <limits>
#include <new>
#include <string>

namespace dominus::title {
namespace {
// game.exe E6FA885F...6B64: the constructor 0x140026cc0 publishes the task at
// 0x140599fd0 (0x140026dd8) with the derived vtable 0x140512850; the module
// fields are written by loadEmuModuleNum 0x1400299d0 (see core.hpp).
constexpr std::uintptr_t TaskGlobalRva = 0x599fd0, TaskVtableRva = 0x512850;
constexpr std::uintptr_t ReadyOffset = 0x184, ModuleOffset = 0x528, IndexOffset = 0x53c;
// The same dra02.dll that marker_identity verifies before reading positions.
constexpr std::uint64_t PortraitFileSize = 5056512;
// dra02.dll RVAs of the scene fields documented with PortraitStateFields.
constexpr std::uintptr_t ModeRva = 0x98c224, GameStateRva = 0x9b1e3c, GameTypeRva = 0x9b2661, PageRva = 0x9bb6f8;
static_assert(sizeof(PortraitStateFields) == 5, "byte fields compared with memcmp");
constexpr std::array<unsigned char, 32> PortraitHash = {
    0xff,0xef,0xa1,0x10,0x9a,0x43,0xfa,0x7a,0x01,0x9a,0x80,0xee,0x16,0xc6,0xf2,0x87,
    0x88,0x43,0x02,0xc8,0xcb,0xb9,0x21,0xd3,0x61,0xf5,0x95,0xe7,0x30,0xc3,0x87,0x44
};

bool read_memory(std::uintptr_t address, void* target, std::size_t length) noexcept {
    if (!address || length > std::numeric_limits<std::uintptr_t>::max() - address) return false;
    SIZE_T received = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
                             target, length, &received) && received == length;
}
template<class T> bool read_value(std::uintptr_t address, T& value) noexcept {
    return read_memory(address, &value, sizeof(value));
}

struct File {
    explicit File(const wchar_t* path, DWORD access) noexcept
        : handle(CreateFileW(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr)) {}
    ~File() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    HANDLE handle;
};

TitleFileKey key_of(HANDLE file) noexcept {
    BY_HANDLE_FILE_INFORMATION info{};
    TitleFileKey key{};
    if (!GetFileInformationByHandle(file, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return key;
    key.volume = info.dwVolumeSerialNumber;
    key.file = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    key.size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    key.written = (static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) |
                  info.ftLastWriteTime.dwLowDateTime;
    key.valid = true;
    return key;
}
bool same_key(const TitleFileKey& a, const TitleFileKey& b) noexcept {
    return a.valid && b.valid && a.volume == b.volume && a.file == b.file && a.size == b.size &&
           a.written == b.written;
}

class WindowsPlatform final : public ITitlePlatform {
public:
    WindowsPlatform() noexcept {
        // LoadLibraryA("dra02.dll") resolves from the application directory.
        try {
            std::wstring host(32768, L'\0');
            const DWORD length = GetModuleFileNameW(nullptr, host.data(), static_cast<DWORD>(host.size()));
            if (!length || length >= host.size()) return;
            host.resize(length);
            const auto slash = host.find_last_of(L"\\/");
            if (slash == std::wstring::npos) return;
            path_ = host.substr(0, slash + 1) + L"dra02.dll";
        } catch (...) { path_.clear(); }
    }
    bool read_task(std::uintptr_t& task) noexcept override {
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        std::uintptr_t value = 0, table = 0;
        if (!base || !read_value(base + TaskGlobalRva, value) || !value ||
            !read_value(value, table) || table != base + TaskVtableRva) return false;
        task = value;
        return true;
    }
    bool read_fields(std::uintptr_t task, EmuTaskTitleFields& fields) noexcept override {
        // +0x528 module, +0x530 static-link module, +0x538 dword, +0x53c index.
        std::array<unsigned char, 0x18> tail{};
        if (task > std::numeric_limits<std::uintptr_t>::max() - IndexOffset - sizeof(std::int32_t) ||
            !read_value(task, fields.vtable) || !read_value(task + ReadyOffset, fields.ready) ||
            !read_memory(task + ModuleOffset, tail.data(), tail.size())) return false;
        std::memcpy(&fields.module, tail.data(), sizeof(fields.module));
        std::memcpy(&fields.index, tail.data() + (IndexOffset - ModuleOffset), sizeof(fields.index));
        return true;
    }
    std::uintptr_t portrait_module() noexcept override {
        // Unchanged refcount: neither pins nor unloads the game DLL.
        return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"dra02.dll"));
    }
    TitleModuleImage module_image(std::uintptr_t handle) noexcept override {
        TitleModuleImage image{};
        image.handle = handle;
        if (!handle || path_.empty()) return image;
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<const void*>(handle), &region, sizeof(region)) ||
            region.AllocationBase != reinterpret_cast<void*>(handle) || region.Type != MEM_IMAGE) return image;
        IMAGE_DOS_HEADER dos{};
        IMAGE_NT_HEADERS64 nt{};
        if (!read_value(handle, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
            dos.e_lfanew < 0x40 || dos.e_lfanew > 0x100000 ||
            !read_value(handle + static_cast<std::uintptr_t>(dos.e_lfanew), nt) ||
            nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            !(nt.FileHeader.Characteristics & IMAGE_FILE_DLL) ||
            nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return image;
        wchar_t path[32768]{};
        const DWORD length = GetModuleFileNameW(reinterpret_cast<HMODULE>(handle), path,
                                                static_cast<DWORD>(std::size(path)));
        if (!length || length >= std::size(path) ||
            CompareStringOrdinal(path, static_cast<int>(length), path_.c_str(),
                                 static_cast<int>(path_.size()), TRUE) != CSTR_EQUAL) return image;
        image.imageSize = nt.OptionalHeader.SizeOfImage;
        image.timestamp = nt.FileHeader.TimeDateStamp;
        image.valid = true;
        return image;
    }
    TitleFileKey file_key(const TitleModuleImage& image) noexcept override {
        if (!image.valid || path_.empty()) return {};
        File file(path_.c_str(), FILE_READ_ATTRIBUTES);
        return file.handle == INVALID_HANDLE_VALUE ? TitleFileKey{} : key_of(file.handle);
    }
    bool verify_file(const TitleModuleImage& image, const TitleFileKey& key) noexcept override {
        if (!image.valid || !key.valid || key.size != PortraitFileSize || path_.empty()) return false;
        File file(path_.c_str(), GENERIC_READ);
        if (file.handle == INVALID_HANDLE_VALUE || !same_key(key_of(file.handle), key)) return false;
        // The file's PE identity must match the mapped image; reading the file
        // never maps or executes it.
        IMAGE_DOS_HEADER dos{};
        IMAGE_NT_HEADERS64 nt{};
        DWORD count = 0;
        bool ok = ReadFile(file.handle, &dos, sizeof(dos), &count, nullptr) && count == sizeof(dos) &&
                  dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew >= 0x40 && dos.e_lfanew <= 0x100000;
        if (ok) {
            LARGE_INTEGER offset{};
            offset.QuadPart = dos.e_lfanew;
            ok = SetFilePointerEx(file.handle, offset, nullptr, FILE_BEGIN) &&
                 ReadFile(file.handle, &nt, sizeof(nt), &count, nullptr) && count == sizeof(nt) &&
                 nt.Signature == IMAGE_NT_SIGNATURE && nt.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
                 nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
                 nt.OptionalHeader.SizeOfImage == image.imageSize &&
                 nt.FileHeader.TimeDateStamp == image.timestamp;
        }
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        std::array<unsigned char, 32> digest{};
        std::array<unsigned char, 65536> bytes{};
        if (ok) {
            LARGE_INTEGER beginning{};
            ok = SetFilePointerEx(file.handle, beginning, nullptr, FILE_BEGIN) &&
                 BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
        }
        if (ok) ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
        std::uint64_t total = 0;
        while (ok) {
            if (!ReadFile(file.handle, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr)) { ok = false; break; }
            if (!count) break;
            if (count > PortraitFileSize - total) { ok = false; break; }
            total += count;
            ok = BCryptHashData(hash, bytes.data(), count, 0) >= 0;
        }
        if (ok) ok = total == PortraitFileSize &&
                     BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0 &&
                     digest == PortraitHash && same_key(key_of(file.handle), key);
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        return ok;
    }

private:
    std::wstring path_;
};

struct Lock {
    explicit Lock(SRWLOCK& lock) noexcept : value(lock) { AcquireSRWLockExclusive(&value); }
    ~Lock() { ReleaseSRWLockExclusive(&value); }
    SRWLOCK& value;
};
SRWLOCK lock = SRWLOCK_INIT;
} // namespace

TitleObservation observe(std::uintptr_t observedTask, std::uint64_t taskEpoch) noexcept {
    Lock guard(lock);
    // Process-lifetime storage, like the other runtime singletons: nothing is
    // destroyed inside DLL_PROCESS_DETACH.
    static auto* platform = new (std::nothrow) WindowsPlatform;
    static auto* detector = new (std::nothrow) TitleDetector;
    if (!platform || !detector) return {};
    return detector->update(*platform, observedTask, taskEpoch, GetTickCount64());
}

PortraitSceneSample portrait_scene(const TitleObservation& title) noexcept {
    const auto module = title.fields.module;
    if (!title.portrait || !module) return {};
    const auto read = [module](PortraitStateFields& fields) noexcept {
        std::array<std::uint8_t, 2> pages{};
        if (!read_value(module + ModeRva, fields.mode) || !read_value(module + GameStateRva, fields.gameState) ||
            !read_value(module + GameTypeRva, fields.gameType) || !read_memory(module + PageRva, pages.data(), pages.size()))
            return false;
        fields.page = pages[0];
        fields.requestedPage = pages[1];
        return true;
    };
    PortraitSceneSample sample{};
    PortraitStateFields again{};
    if (!read(sample.fields) || !read(again) || std::memcmp(&sample.fields, &again, sizeof(again)) != 0 ||
        reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"dra02.dll")) != module) return {};
    sample.available = true;
    return sample;
}

} // namespace dominus::title
