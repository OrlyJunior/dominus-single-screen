#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include "dominus/marker_identity.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <limits>

namespace dominus::marker::identity {
namespace {
// The fields a title's map marker is drawn from (static Ghidra 12.1.4). No
// persistent marker x/y field is assumed; see docs/ARCHITECTURE.md.
struct Pool { std::uintptr_t rva = 0; std::size_t count = 0, stride = 0; };
struct Spec {
    Title title;
    const wchar_t* name;
    std::size_t fileSize;
    std::array<unsigned char, 32> hash;
    std::uintptr_t handleRva, areaRva, roomRva;
    Pool pools[2];               // handle types 1 and 2; an empty pool refuses its type
    std::size_t positionOffset;  // int32 x, then int32 y
    std::uint8_t areas;          // valid area values are below this
};
constexpr Spec Specs[] = {
    // dra02.dll, SHA-256 FFEFA1109A43FA7A019A80EE16C6F287884302C8CBB921D361F595E730C38744.
    // FUN_18002c280 decodes the handle; FUN_18002e9b0 reads int32 player
    // +0x30/+0x34 and room low bytes, then writes OAM directly.
    {Title::PortraitOfRuin, L"dra02.dll", 5056512, {
        0xff,0xef,0xa1,0x10,0x9a,0x43,0xfa,0x7a,0x01,0x9a,0x80,0xee,0x16,0xc6,0xf2,0x87,
        0x88,0x43,0x02,0xc8,0xcb,0xb9,0x21,0xd3,0x61,0xf5,0x95,0xe7,0x30,0xc3,0x87,0x44},
     0x9b26a8, 0x9b1e89, 0x9b1e7c, {{0x994198, 0xf6, 0x1f0}, {0x9b2e78, 0x80, 0x100}}, 0x30, 14},
    // dra01.dll, SHA-256 52E20914E3668ED3E88BC858540292D52FCF6ED3DFF6F461AC49B53FA2F9CD6E.
    // FUN_1800166d0 decodes the handle (pool type 1 only); the marker,
    // FUN_18004d590, reads int32 player +0x2c/+0x30, the room low bytes and
    // the area (written 0-2 only) for its per-area offsets, then stages OAM.
    {Title::DawnOfSorrow, L"dra01.dll", 3872768, {
        0x52,0xe2,0x09,0x14,0xe3,0x66,0x8e,0xd3,0xe8,0x8b,0xc8,0x58,0x54,0x02,0x92,0xd5,
        0x2f,0xcf,0x6e,0xd3,0xdf,0xf6,0xf4,0x61,0xac,0x49,0xb5,0x3f,0xa2,0xf9,0xcd,0x6e},
     0x964440, 0x963f0d, 0x963f08, {{0x8f5c48, 400, 0x468}, {}}, 0x2c, 3},
};
constexpr std::size_t PositionBytes = 8;
constexpr std::size_t required_image_size(const Spec& spec) noexcept {
    std::size_t size = std::max({spec.handleRva + 8, spec.areaRva + 1, spec.roomRva + 4});
    for (const auto& pool : spec.pools) size = std::max(size, pool.rva + pool.count * pool.stride);
    return size;
}

struct Room { std::uint16_t x = 0, y = 0; };
struct Position { std::int32_t x = 0, y = 0; };
static_assert(sizeof(Room) == 4 && sizeof(Position) == PositionBytes, "Unexpected field layout");
struct Key {
    std::uint64_t handle = 0;
    std::uint16_t roomX = 0, roomY = 0;
    std::uint8_t area = 0;
    std::int32_t quantX = 0, quantY = 0;
};
bool equal(const Key& a, const Key& b) noexcept {
    return a.handle == b.handle && a.roomX == b.roomX && a.roomY == b.roomY &&
           a.area == b.area && a.quantX == b.quantX && a.quantY == b.quantY;
}
struct Panel {
    Key key{};
    std::uint64_t epoch = 0;
    bool ready = false;
};
struct State {
    HMODULE module = nullptr; // Identity only; never directly dereferenced.
    wchar_t path[32768]{};
    DWORD imageSize = 0, timestamp = 0;
    bool attempted = false, valid = false;
    Panel panels[2]{};
};
SRWLOCK stateLock = SRWLOCK_INIT;
State states[std::size(Specs)];
struct Lock {
    explicit Lock(SRWLOCK& lock) noexcept : value(lock) { AcquireSRWLockExclusive(&value); }
    ~Lock() { ReleaseSRWLockExclusive(&value); }
    SRWLOCK& value;
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
bool in_image(std::size_t rva, std::size_t length, DWORD imageSize) noexcept {
    return rva <= imageSize && length <= static_cast<std::size_t>(imageSize) - rva;
}
template<class T> bool read_rva(HMODULE module, DWORD imageSize, std::size_t rva, T& value) noexcept {
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    return in_image(rva, sizeof(value), imageSize) &&
           rva <= std::numeric_limits<std::uintptr_t>::max() - base && read_value(base + rva, value);
}

bool image_headers(const Spec& spec, HMODULE module, DWORD& imageSize, DWORD& timestamp) noexcept {
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    IMAGE_DOS_HEADER dos{};
    if (!read_value(base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew < 0x40 || dos.e_lfanew > 0x100000) return false;
    IMAGE_NT_HEADERS64 nt{};
    if (static_cast<std::uintptr_t>(dos.e_lfanew) > std::numeric_limits<std::uintptr_t>::max() - base ||
        !read_value(base + static_cast<std::uintptr_t>(dos.e_lfanew), nt) ||
        nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        !(nt.FileHeader.Characteristics & IMAGE_FILE_DLL) ||
        nt.FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64) ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.OptionalHeader.SizeOfImage < required_image_size(spec) || nt.OptionalHeader.SizeOfImage > 0x4000000 ||
        !in_image(spec.handleRva, sizeof(std::uint64_t), nt.OptionalHeader.SizeOfImage) ||
        !in_image(spec.areaRva, sizeof(std::uint8_t), nt.OptionalHeader.SizeOfImage) ||
        !in_image(spec.roomRva, sizeof(Room), nt.OptionalHeader.SizeOfImage)) return false;
    for (const auto& pool : spec.pools)
        if (!in_image(pool.rva, pool.count * pool.stride, nt.OptionalHeader.SizeOfImage)) return false;
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(reinterpret_cast<const void*>(base), &region, sizeof(region)) ||
        region.AllocationBase != reinterpret_cast<void*>(base) || region.Type != MEM_IMAGE) return false;
    imageSize = nt.OptionalHeader.SizeOfImage;
    timestamp = nt.FileHeader.TimeDateStamp;
    return true;
}

bool same_file(const BY_HANDLE_FILE_INFORMATION& a, const BY_HANDLE_FILE_INFORMATION& b) noexcept {
    return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber && a.nFileIndexHigh == b.nFileIndexHigh &&
           a.nFileIndexLow == b.nFileIndexLow && a.nFileSizeHigh == b.nFileSizeHigh &&
           a.nFileSizeLow == b.nFileSizeLow &&
           a.ftLastWriteTime.dwHighDateTime == b.ftLastWriteTime.dwHighDateTime &&
           a.ftLastWriteTime.dwLowDateTime == b.ftLastWriteTime.dwLowDateTime;
}
bool supported_file(const Spec& spec, const wchar_t* path, DWORD imageSize, DWORD timestamp) noexcept {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION before{}, after{};
    bool ok = GetFileInformationByHandle(file, &before) && before.nFileSizeHigh == 0 &&
              before.nFileSizeLow == spec.fileSize && !(before.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    // Compare the file's PE identity to the loaded image, in addition to hashing
    // all file bytes. File reads never use LoadLibrary or execute target code.
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    DWORD count = 0;
    if (ok) ok = ReadFile(file, &dos, sizeof(dos), &count, nullptr) && count == sizeof(dos) &&
                 dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew >= 0x40 && dos.e_lfanew <= 0x100000;
    if (ok) {
        LARGE_INTEGER offset{}; offset.QuadPart = dos.e_lfanew;
        ok = SetFilePointerEx(file, offset, nullptr, FILE_BEGIN) &&
             ReadFile(file, &nt, sizeof(nt), &count, nullptr) && count == sizeof(nt) &&
             nt.Signature == IMAGE_NT_SIGNATURE && nt.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
             nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
             nt.OptionalHeader.SizeOfImage == imageSize && nt.FileHeader.TimeDateStamp == timestamp;
    }
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::array<unsigned char, 32> digest{};
    std::array<unsigned char, 65536> bytes{};
    if (ok) {
        LARGE_INTEGER beginning{};
        ok = SetFilePointerEx(file, beginning, nullptr, FILE_BEGIN) &&
             BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
    }
    if (ok) ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    std::size_t total = 0;
    while (ok) {
        if (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr)) { ok = false; break; }
        if (!count) break;
        if (count > spec.fileSize - total) { ok = false; break; }
        total += count;
        ok = BCryptHashData(hash, bytes.data(), count, 0) >= 0;
    }
    if (ok) ok = total == spec.fileSize &&
                 BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0 &&
                 digest == spec.hash && GetFileInformationByHandle(file, &after) && same_file(before, after);
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    return ok;
}

void invalidate_all(State& state) noexcept { for (auto& panel : state.panels) panel.ready = false; }
void forget_module(State& state) noexcept {
    state.module = nullptr;
    state.path[0] = L'\0';
    state.attempted = false;
    state.valid = false;
    invalidate_all(state);
}
bool module_ready(const Spec& spec, State& state) noexcept {
    // Unchanged refcount: this provider neither pins nor unloads the game DLL.
    // A concurrent unload is handled by ReadProcessMemory failure and the final
    // module identity check; no raw pointer dereference crosses that race.
    HMODULE module = GetModuleHandleW(spec.name);
    if (!module) { forget_module(state); return false; }
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
    if (!length || length >= std::size(path)) { forget_module(state); return false; }
    if (module != state.module || std::wcscmp(path, state.path) != 0) {
        invalidate_all(state);
        state.module = module;
        std::wmemcpy(state.path, path, length + 1);
        state.attempted = false;
        state.valid = false;
    }
    DWORD imageSize = 0, timestamp = 0;
    if (!image_headers(spec, module, imageSize, timestamp)) { state.valid = false; invalidate_all(state); return false; }
    if (!state.attempted) {
        state.attempted = true;
        state.imageSize = imageSize;
        state.timestamp = timestamp;
        state.valid = supported_file(spec, path, imageSize, timestamp);
    }
    if (!state.valid || imageSize != state.imageSize || timestamp != state.timestamp) {
        state.valid = false; invalidate_all(state); return false;
    }
    return true;
}

bool entity_rva(const Spec& spec, const State& state, std::uint64_t handle, std::size_t& positionRva) noexcept {
    // Mirrors the title's handle decoder, with stricter fail-closed slot-envelope
    // checks. The original can add low24 even after an invalid pool/index (or a
    // type without a pool); never do so.
    if (((handle >> 48) & 0xff) != 3) return false;
    const unsigned type = static_cast<unsigned>((handle >> 24) & 0xff);
    const std::size_t index = static_cast<std::size_t>((handle >> 32) & 0xffff);
    const std::size_t offset = static_cast<std::size_t>(handle & 0xffffff);
    if (type < 1 || type > 2) return false;
    const Pool& pool = spec.pools[type - 1];
    if (!pool.count || index >= pool.count || offset > pool.stride - spec.positionOffset - PositionBytes) return false;
    // All terms are bounded above before arithmetic, including low24.
    const std::size_t slot = pool.rva + index * pool.stride;
    positionRva = slot + offset + spec.positionOffset;
    return in_image(slot, pool.stride, state.imageSize) && in_image(positionRva, PositionBytes, state.imageSize);
}
bool read_key(const Spec& spec, const State& state, Key& key) noexcept {
    Room room{};
    Position position{};
    std::size_t positionRva = 0;
    if (!read_rva(state.module, state.imageSize, spec.handleRva, key.handle) ||
        !read_rva(state.module, state.imageSize, spec.areaRva, key.area) || key.area >= spec.areas ||
        !read_rva(state.module, state.imageSize, spec.roomRva, room) ||
        !entity_rva(spec, state, key.handle, positionRva) ||
        !read_rva(state.module, state.imageSize, positionRva, position)) return false;
    key.roomX = room.x;
    key.roomY = room.y;
    // Native signed division truncates toward zero. In both titles these
    // quantized terms move the marker by one DS pixel; its phase is
    // deliberately not part of a key.
    key.quantX = position.x / 0x40000;
    key.quantY = position.y / 0x30000;
    return true;
}
int spec_slot(Title title) noexcept {
    for (std::size_t i = 0; i < std::size(Specs); ++i)
        if (Specs[i].title == title) return static_cast<int>(i);
    return -1;
}
} // namespace

Sample sample(Title title, unsigned panel) noexcept {
    const int slot = spec_slot(title);
    if (panel > 1 || slot < 0) return {};
    const Spec& spec = Specs[slot];
    Lock guard(stateLock);
    State& state = states[slot];
    if (!module_ready(spec, state)) return {};
    Key first{}, second{};
    if (!read_key(spec, state, first) || !read_key(spec, state, second) || !equal(first, second) ||
        GetModuleHandleW(spec.name) != state.module) {
        invalidate_all(state);
        return {};
    }
    auto& current = state.panels[panel];
    if (!current.ready || !equal(current.key, second)) {
        if (current.epoch == std::numeric_limits<std::uint64_t>::max()) {
            current.ready = false;
            return {}; // Never wrap an epoch and accidentally reuse old data.
        }
        ++current.epoch;
        current.key = second;
        current.ready = true;
    }
    return {true, current.epoch};
}
} // namespace dominus::marker::identity
