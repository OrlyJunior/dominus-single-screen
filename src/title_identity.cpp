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
// dra02.dll RVAs of the scene fields documented with PortraitStateFields.
constexpr std::uintptr_t ModeRva = 0x98c224, GameStateRva = 0x9b1e3c, GameTypeRva = 0x9b2661, PageRva = 0x9bb6f8;
static_assert(sizeof(PortraitStateFields) == 5, "byte fields compared with memcmp");
// dra01.dll RVAs of the scene fields documented with DawnStateFields. The
// page bytes are 0x965748 shown, +1 requested and +3 the overlay flag; the
// sub-state byte is followed by its step (one word, stored at once); the
// fade is the int32 frame count at 0x8f5c2c and the level at +8.
constexpr std::uintptr_t DawnModeRva = 0x8eb248, DawnFrozenRva = 0x8eb250, DawnGameStateRva = 0x963ecc,
                         DawnEventFlagsRva = 0x963ed4, DawnGameTypeRva = 0x964346, DawnPageRva = 0x965748,
                         DawnFadeRva = 0x8f5c2c;

// The exact title DLLs accepted. dra02.dll is the same file marker_identity
// verifies before reading positions.
struct ModuleSpec {
    Title title;
    const wchar_t* name;
    std::uint64_t size;
    std::array<unsigned char, 32> hash;
};
constexpr ModuleSpec Modules[] = {
    {Title::PortraitOfRuin, L"dra02.dll", 5056512, {
        0xff,0xef,0xa1,0x10,0x9a,0x43,0xfa,0x7a,0x01,0x9a,0x80,0xee,0x16,0xc6,0xf2,0x87,
        0x88,0x43,0x02,0xc8,0xcb,0xb9,0x21,0xd3,0x61,0xf5,0x95,0xe7,0x30,0xc3,0x87,0x44}},
    {Title::DawnOfSorrow, L"dra01.dll", 3872768, {
        0x52,0xe2,0x09,0x14,0xe3,0x66,0x8e,0xd3,0xe8,0x8b,0xc8,0x58,0x54,0x02,0x92,0xd5,
        0x2f,0xcf,0x6e,0xd3,0xdf,0xf6,0xf4,0x61,0xac,0x49,0xb5,0x3f,0xa2,0xf9,0xcd,0x6e}},
};
int module_slot(Title title) noexcept {
    for (std::size_t i = 0; i < std::size(Modules); ++i)
        if (Modules[i].title == title) return static_cast<int>(i);
    return -1;
}

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
        // LoadLibraryA("dra0N.dll") resolves from the application directory.
        try {
            std::wstring host(32768, L'\0');
            const DWORD length = GetModuleFileNameW(nullptr, host.data(), static_cast<DWORD>(host.size()));
            if (!length || length >= host.size()) return;
            host.resize(length);
            const auto slash = host.find_last_of(L"\\/");
            if (slash == std::wstring::npos) return;
            for (std::size_t i = 0; i < std::size(Modules); ++i) paths_[i] = host.substr(0, slash + 1) + Modules[i].name;
        } catch (...) { for (auto& path : paths_) path.clear(); }
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
    std::uintptr_t title_module(Title title) noexcept override {
        const int slot = module_slot(title);
        // Unchanged refcount: neither pins nor unloads the game DLL.
        return slot < 0 ? 0 : reinterpret_cast<std::uintptr_t>(GetModuleHandleW(Modules[slot].name));
    }
    TitleModuleImage module_image(Title title, std::uintptr_t handle) noexcept override {
        TitleModuleImage image{};
        image.handle = handle;
        const int slot = module_slot(title);
        if (!handle || slot < 0 || paths_[slot].empty()) return image;
        const auto& expected = paths_[slot];
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
            CompareStringOrdinal(path, static_cast<int>(length), expected.c_str(),
                                 static_cast<int>(expected.size()), TRUE) != CSTR_EQUAL) return image;
        image.imageSize = nt.OptionalHeader.SizeOfImage;
        image.timestamp = nt.FileHeader.TimeDateStamp;
        image.valid = true;
        return image;
    }
    TitleFileKey file_key(Title title, const TitleModuleImage& image) noexcept override {
        const int slot = module_slot(title);
        if (!image.valid || slot < 0 || paths_[slot].empty()) return {};
        File file(paths_[slot].c_str(), FILE_READ_ATTRIBUTES);
        return file.handle == INVALID_HANDLE_VALUE ? TitleFileKey{} : key_of(file.handle);
    }
    bool verify_file(Title title, const TitleModuleImage& image, const TitleFileKey& key) noexcept override {
        const int slot = module_slot(title);
        if (slot < 0 || paths_[slot].empty()) return false;
        const auto& spec = Modules[slot];
        if (!image.valid || !key.valid || key.size != spec.size) return false;
        File file(paths_[slot].c_str(), GENERIC_READ);
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
            if (count > spec.size - total) { ok = false; break; }
            total += count;
            ok = BCryptHashData(hash, bytes.data(), count, 0) >= 0;
        }
        if (ok) ok = total == spec.size &&
                     BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0 &&
                     digest == spec.hash && same_key(key_of(file.handle), key);
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        return ok;
    }

private:
    std::wstring paths_[std::size(Modules)];
};

struct Lock {
    explicit Lock(SRWLOCK& lock) noexcept : value(lock) { AcquireSRWLockExclusive(&value); }
    ~Lock() { ReleaseSRWLockExclusive(&value); }
    SRWLOCK& value;
};
SRWLOCK lock = SRWLOCK_INIT;
TitleSet enabledTitles{};
// Process-lifetime storage, like the other runtime singletons: nothing is
// destroyed inside DLL_PROCESS_DETACH.
WindowsPlatform* platform = nullptr;
TitleDetector* detector = nullptr;
} // namespace

void configure(const TitleSet& enabled) noexcept {
    Lock guard(lock);
    enabledTitles = enabled;
    if (detector) *detector = TitleDetector{enabled};
}

TitleObservation observe(std::uintptr_t observedTask, std::uint64_t taskEpoch) noexcept {
    Lock guard(lock);
    if (!platform) platform = new (std::nothrow) WindowsPlatform;
    if (!detector) detector = new (std::nothrow) TitleDetector{enabledTitles};
    if (!platform || !detector) return {};
    return detector->update(*platform, observedTask, taskEpoch, GetTickCount64());
}

PortraitSceneSample portrait_scene(const TitleObservation& title) noexcept {
    const auto module = title.fields.module;
    if (!title.portrait || title.title != Title::PortraitOfRuin || !module) return {};
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

DawnSceneSample dawn_scene(const TitleObservation& title) noexcept {
    const auto module = title.fields.module;
    if (title.title != Title::DawnOfSorrow || !module) return {};
    const auto read = [module](DawnStateFields& fields) noexcept {
        std::array<std::uint8_t, 2> state{};  // sub-state, its step
        std::array<std::uint8_t, 4> pages{};
        std::array<std::int32_t, 3> fade{};  // frames, step, level
        if (!read_value(module + DawnModeRva, fields.mode) || !read_value(module + DawnFrozenRva, fields.frozen) ||
            !read_memory(module + DawnGameStateRva, state.data(), state.size()) ||
            !read_value(module + DawnEventFlagsRva, fields.eventFlags) ||
            !read_value(module + DawnGameTypeRva, fields.gameType) ||
            !read_memory(module + DawnPageRva, pages.data(), pages.size()) ||
            !read_memory(module + DawnFadeRva, fade.data(), sizeof(fade)))
            return false;
        fields.gameState = state[0];
        fields.subState = state[1];
        fields.page = pages[0];
        fields.requestedPage = pages[1];
        fields.pageOverlay = pages[3];
        fields.fadeFrames = fade[0];
        fields.brightness = fade[2];
        return true;
    };
    DawnSceneSample sample{};
    DawnStateFields again{};
    if (!read(sample.fields) || !read(again) || !same_dawn_fields(sample.fields, again) ||
        reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"dra01.dll")) != module) return {};
    sample.available = true;
    return sample;
}

SceneSample scene(const TitleObservation& title) noexcept {
    SceneSample sample{};
    sample.fields.title = title.title;
    if (title.title == Title::PortraitOfRuin) {
        const auto portrait = portrait_scene(title);
        sample.available = portrait.available;
        sample.fields.portrait = portrait.fields;
    } else if (title.title == Title::DawnOfSorrow) {
        const auto dawn = dawn_scene(title);
        sample.available = dawn.available;
        sample.fields.dawn = dawn.fields;
    }
    return sample;
}

} // namespace dominus::title
