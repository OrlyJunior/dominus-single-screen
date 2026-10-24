#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>

namespace dominus::native {

using LogCallback = void (*)(const char* line) noexcept;

struct DrawScope {
    std::uintptr_t task = 0;
    std::uintptr_t screen = 0;
    std::uint64_t epoch = 0;
    std::uint32_t index = 0;
    std::uint32_t thread = 0;
    bool valid = false;
};

struct Status {
    bool initialized = false;
    bool signaturesMatched = false;
    bool hooksInstalled = false;
    bool taskObserved = false;
    bool pauseKnown = false;
    bool paused = false;
    bool observationOnly = true;
    std::uintptr_t task = 0;
    std::uint64_t taskEpoch = 0;
    std::uint64_t pauseRevision = 0;
    std::uint64_t drawCalls = 0;
    std::uint64_t inputUpdates = 0;
    std::uint64_t pauseReads = 0;
    std::uint64_t pauseWrites = 0;
    std::uint64_t normalizedQueries = 0;
    std::uint64_t invalidObjects = 0;
    std::uint32_t drawThread = 0;
    std::uint32_t inputThread = 0;
    // Highest classified caller-provided index + 1, not an array-size field.
    std::uint32_t screenCount = 0;
    std::uint32_t lastNormalizedOn = 0;
    std::uint32_t lastNormalizedPressed = 0;
    std::uint32_t lastTotalOn = 0;
    std::uint32_t lastTotalPressed = 0;
    std::uint64_t queryLogDrops = 0;
    bool pauseCommandsEnabled = false;
    bool lifecycleHooksInstalled = false;
    std::uint32_t pauseThread = 0;
    std::uint64_t ownedPauseToken = 0;
    // The pause now in effect was set by a mod Acquire and nothing has
    // unpaused since, yet no lease owns it (an external same-value setPause
    // superseded the lease). The collection's own menu leaves this behind: it
    // only unpauses on close if it found the game unpaused when it opened.
    bool stalePause = false;
};

// ReleaseStale unpauses only a stalePause (see Status); no lease token needed.
enum class PauseCommandKind { Acquire, Release, ReleaseStale };
enum class PauseReplyCode { None, Acquired, AlreadyPaused, Released, Superseded, Rejected, Failed };
struct PauseRequest {
    PauseCommandKind kind = PauseCommandKind::Acquire;
    std::uint64_t requestId = 0;
    std::uint64_t expectedTaskEpoch = 0;
    std::uint64_t expectedRevision = 0;
    std::uint64_t leaseToken = 0;
};
struct PauseReply {
    std::uint64_t requestId = 0;
    PauseReplyCode code = PauseReplyCode::None;
    std::uint64_t leaseToken = 0;
    std::uint64_t taskEpoch = 0;
    std::uint64_t revision = 0;
};
// Explicit opt-in only after the live getter thread has been validated. Native
// calls are deferred until the next getter on that same thread and live object.
// One outstanding request is allowed. A reply is authoritative only for its ID.
bool enable_pause_commands(bool enabled) noexcept;
bool queue_pause_request(const PauseRequest& request) noexcept;
// Pause thread only (the pad hook runs there): releases a still-valid mod
// lease at once, so code later in the same frame (the collection opening its
// menu on controller loss) finds the game unpaused and restores it on close.
// False, changing nothing, on any other thread or without a valid lease.
bool release_owned_now() noexcept;
PauseReply pause_reply_snapshot() noexcept;

// Caller must first gate the exact game.exe SHA-256. This function independently
// verifies image boundaries, code protection, exact signatures and RTTI-linked
// vtable entries. It installs pass-through observers for DrawGpuScreen,
// end_update_padinfo, getPause, setPause and four normalized-input getters.
// Constructor/destructor/reinitialization observers invalidate pause ownership.
// Native pause commands default to disabled and require a separate explicit
// enable_pause_commands call after live validation. The On/Pressed getter
// argument is a pad/group index;
// TotalOn/TotalPressed receive a filter mask. Raw XInput bits are not substituted.
bool initialize(HMODULE host, LogCallback log) noexcept;
// True once the EmuTask the game's constructor publishes (global RVA 0x599FD0)
// has the derived vtable and the backend (+0x238) its first reinitialization
// attaches. Usable before initialize. Exact game build only; the caller gates
// the SHA-256 first. Read-only.
bool task_ready(HMODULE host) noexcept;
Status status_snapshot() noexcept;
DrawScope draw_scope() noexcept;

// Only after every game thread has stopped calling the observed methods. This
// removes this module's hooks; it does not uninitialize shared MinHook globally.
void shutdown() noexcept;

} // namespace dominus::native
