#pragma once
#include <cstdint>

namespace dominus::input {
using Log = void (*)(const char*) noexcept;
struct Snapshot {
    std::uint32_t controller=0;
    bool connected=false;
    std::uint16_t buttons=0;
    std::uint32_t packet=0;
    std::uint32_t error=0;
    std::uint64_t sequence=0;
    std::uint64_t tick=0;
    std::uint64_t gameCalls=0;
    bool reservedRequested=false;
};
// Receives raw state. The returned mask may suppress View (0x20), Y (0x8000),
// LB (0x100) or RB (0x200), and may replay ONLY View when the chord recognizer
// delays its native action. Y, LB and RB can never be injected. Every other
// button/analog field remains unchanged.
// fromGame=false is the observer's own poll; its returned mask is discarded.
// Callback must be noexcept, nonblocking, and safe on the game's input thread.
using Filter = std::uint16_t (*)(const Snapshot&,bool fromGame) noexcept;
void set_filter(Filter filter) noexcept;
// Hooks only an already-loaded System32 xinput1_4.dll. Caller gates the host.
// Default null filter forwards the complete original XINPUT_STATE unchanged.
bool initialize(Log log) noexcept;
// Snapshot::controller of the Steam Input pad (XInput uses 0..3).
inline constexpr std::uint32_t SteamController=4;
// Exact game.exe build only: the caller MUST first gate its SHA-256. Verifies
// and hooks the game's SteamInput006 pad builder (RVA 0x22A920), whose output
// uses XInput button bits. The tracked Steam controller goes through the same
// Filter; other fields and other controller handles stay unchanged. Calls no
// Steam API. See docs/STEAM-INPUT-VIEW.md.
bool initialize_steam(void* host,Log log) noexcept;
// Polls all four slots through the trampoline, never through the hook.
void on_present() noexcept;
// UINT32_MAX selects the first currently connected XInput controller;
// otherwise 0..3 or SteamController.
Snapshot snapshot(std::uint32_t controller=UINT32_MAX) noexcept;
// Records reservation intent only. Actual suppression is defined by set_filter;
// this flag is not required to enable that callback's policy.
void set_mod_reserved(bool requested) noexcept;
// Only after no game thread is calling the hook; no global MinHook shutdown.
void shutdown() noexcept;
}
