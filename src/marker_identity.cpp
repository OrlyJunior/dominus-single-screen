#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include "dominus/marker_identity.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <limits>

namespace dominus::marker::identity {
namespace {
// dra02.dll, SHA-256 FFEFA1109A43FA7A019A80EE16C6F287884302C8CBB921D361F595E730C38744.
// Static Ghidra 12.1.4: FUN_18002c280 decodes the handle; FUN_18002e9b0
// reads int32 player +0x30/+0x34 and room low bytes, then writes OAM directly.
// No persistent marker x/y field is assumed. See MARKER-POSITION-IDENTITY.md.
constexpr std::size_t ExpectedFileSize = 5056512;
constexpr std::uintptr_t HandleRva = 0x9b26a8;
constexpr std::uintptr_t AreaRva = 0x9b1e89;
constexpr std::uintptr_t RoomRva = 0x9b1e7c;
constexpr std::uintptr_t PoolOneRva = 0x994198, PoolTwoRva = 0x9b2e78;
constexpr std::size_t PoolOneCount = 0xf6, PoolOneStride = 0x1f0;
constexpr std::size_t PoolTwoCount = 0x80, PoolTwoStride = 0x100;
constexpr std::size_t PositionOffset = 0x30, PositionBytes = 8;
constexpr std::size_t RequiredImageSize = PoolTwoRva + PoolTwoCount * PoolTwoStride;
constexpr std::array<unsigned char, 32> ExpectedHash = {
    0xff,0xef,0xa1,0x10,0x9a,0x43,0xfa,0x7a,0x01,0x9a,0x80,0xee,0x16,0xc6,0xf2,0x87,
    0x88,0x43,0x02,0xc8,0xcb,0xb9,0x21,0xd3,0x61,0xf5,0x95,0xe7,0x30,0xc3,0x87,0x44
};

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
    SRWLOCK lock = SRWLOCK_INIT;
    HMODULE module = nullptr; // Identity only; never directly dereferenced.
    wchar_t path[32768]{};
    DWORD imageSize = 0, timestamp = 0;
    bool attempted = false, valid = false;
    Panel panels[2]{};
};
State state;
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

bool image_headers(HMODULE module, DWORD& imageSize, DWORD& timestamp) noexcept {
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
        nt.OptionalHeader.SizeOfImage < RequiredImageSize || nt.OptionalHeader.SizeOfImage > 0x4000000 ||
        !in_image(PoolOneRva, PoolOneCount * PoolOneStride, nt.OptionalHeader.SizeOfImage) ||
        !in_image(HandleRva, sizeof(std::uint64_t), nt.OptionalHeader.SizeOfImage) ||
        !in_image(AreaRva, sizeof(std::uint8_t), nt.OptionalHeader.SizeOfImage) ||
        !in_image(RoomRva, sizeof(Room), nt.OptionalHeader.SizeOfImage)) return false;
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
bool supported_file(const wchar_t* path, DWORD imageSize, DWORD timestamp) noexcept {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION before{}, after{};
    bool ok = GetFileInformationByHandle(file, &before) && before.nFileSizeHigh == 0 &&
              before.nFileSizeLow == ExpectedFileSize && !(before.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
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
        if (count > ExpectedFileSize - total) { ok = false; break; }
        total += count;
        ok = BCryptHashData(hash, bytes.data(), count, 0) >= 0;
    }
    if (ok) ok = total == ExpectedFileSize &&
                 BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0 &&
                 digest == ExpectedHash && GetFileInformationByHandle(file, &after) && same_file(before, after);
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    return ok;
}

void invalidate_all() noexcept { for (auto& panel : state.panels) panel.ready = false; }
void forget_module() noexcept {
    state.module = nullptr;
    state.path[0] = L'\0';
    state.attempted = false;
    state.valid = false;
    invalidate_all();
}
bool module_ready() noexcept {
    // Unchanged refcount: this provider neither pins nor unloads the game DLL.
    // A concurrent unload is handled by ReadProcessMemory failure and the final
    // module identity check; no raw pointer dereference crosses that race.
    HMODULE module = GetModuleHandleW(L"dra02.dll");
    if (!module) { forget_module(); return false; }
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
    if (!length || length >= std::size(path)) { forget_module(); return false; }
    if (module != state.module || std::wcscmp(path, state.path) != 0) {
        invalidate_all();
        state.module = module;
        std::wmemcpy(state.path, path, length + 1);
        state.attempted = false;
        state.valid = false;
    }
    DWORD imageSize = 0, timestamp = 0;
    if (!image_headers(module, imageSize, timestamp)) { state.valid = false; invalidate_all(); return false; }
    if (!state.attempted) {
        state.attempted = true;
        state.imageSize = imageSize;
        state.timestamp = timestamp;
        state.valid = supported_file(path, imageSize, timestamp);
    }
    if (!state.valid || imageSize != state.imageSize || timestamp != state.timestamp) {
        state.valid = false; invalidate_all(); return false;
    }
    return true;
}

bool entity_rva(std::uint64_t handle, std::size_t& positionRva) noexcept {
    // Mirrors FUN_18002c280, with stricter fail-closed slot-envelope checks.
    // The original can add low24 even after an invalid pool/index; never do so.
    if (((handle >> 48) & 0xff) != 3) return false;
    const unsigned type = static_cast<unsigned>((handle >> 24) & 0xff);
    const std::size_t index = static_cast<std::size_t>((handle >> 32) & 0xffff);
    const std::size_t offset = static_cast<std::size_t>(handle & 0xffffff);
    std::size_t pool = 0, stride = 0, count = 0;
    if (type == 1) { pool = PoolOneRva; stride = PoolOneStride; count = PoolOneCount; }
    else if (type == 2) { pool = PoolTwoRva; stride = PoolTwoStride; count = PoolTwoCount; }
    else return false;
    if (index >= count || offset > stride - PositionOffset - PositionBytes) return false;
    // All terms are bounded above before arithmetic, including low24.
    const std::size_t slot = pool + index * stride;
    positionRva = slot + offset + PositionOffset;
    return in_image(slot, stride, state.imageSize) && in_image(positionRva, PositionBytes, state.imageSize);
}
bool read_key(Key& key) noexcept {
    Room room{};
    Position position{};
    std::size_t positionRva = 0;
    if (!read_rva(state.module, state.imageSize, HandleRva, key.handle) ||
        !read_rva(state.module, state.imageSize, AreaRva, key.area) || key.area >= 14 ||
        !read_rva(state.module, state.imageSize, RoomRva, room) ||
        !entity_rva(key.handle, positionRva) ||
        !read_rva(state.module, state.imageSize, positionRva, position)) return false;
    key.roomX = room.x;
    key.roomY = room.y;
    // Native signed division truncates toward zero. These quantized terms move
    // the marker by one DS pixel; phase +0x5e is deliberately not part of a key.
    key.quantX = position.x / 0x40000;
    key.quantY = position.y / 0x30000;
    return true;
}
} // namespace

Sample sample(unsigned panel) noexcept {
    if (panel > 1) return {};
    Lock lock(state.lock);
    if (!module_ready()) return {};
    Key first{}, second{};
    if (!read_key(first) || !read_key(second) || !equal(first, second) ||
        GetModuleHandleW(L"dra02.dll") != state.module) {
        invalidate_all();
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
