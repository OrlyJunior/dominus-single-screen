#pragma once
#include <cstdint>
#include "dominus/core.hpp"

// Read-only identification of the title the collection's EmuTask has loaded,
// for game.exe E6FA885F...6B64 only (the caller gates that hash first).
// Portrait is reported only for the exact dra02.dll FFEFA110...8744 loaded from
// the game.exe directory. This never calls game code, installs hooks, loads or
// pins a module, or writes game memory.
namespace dominus::title {

// Render thread, once per Present (TitleDetector documents the costs).
// observedTask is the task the native observers validated, 0 if none; taskEpoch
// is their lifecycle epoch (construct, destroy and reinitialize advance it).
TitleObservation observe(std::uintptr_t observedTask, std::uint64_t taskEpoch) noexcept;

struct PortraitSceneSample {
    bool available = false;
    PortraitStateFields fields{};
};
// Render thread. Reads Portrait's mode, game state/type and sub-screen pages
// (core.hpp) from the dra02.dll that `title` verified, twice. Unavailable
// unless title.portrait, both reads agree and the module is still loaded at
// the same handle. Five bytes; no game code is called.
PortraitSceneSample portrait_scene(const TitleObservation& title) noexcept;

}
