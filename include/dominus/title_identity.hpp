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

}
