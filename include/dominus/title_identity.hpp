#pragma once
#include <cstdint>
#include "dominus/core.hpp"

// Read-only identification of the title the collection's EmuTask has loaded,
// for game.exe E6FA885F...6B64 only (the caller gates that hash first).
// Portrait is reported only for the exact dra02.dll FFEFA110...8744 and Dawn of
// Sorrow, when enabled, only for the exact dra01.dll 52E20914...CD6E, each
// loaded from the game.exe directory. This never calls game code, installs
// hooks, loads or pins a module, or writes game memory.
namespace dominus::title {

// Titles that may be verified; the default is Portrait alone (v0.1.1). Call
// before the first observe(); a later call restarts identification.
void configure(const TitleSet& enabled) noexcept;

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

struct DawnSceneSample {
    bool available = false;
    DawnStateFields fields{};
};
// The same for Dawn of Sorrow's fields (core.hpp) in the dra01.dll that
// `title` verified. Six bytes, read twice.
DawnSceneSample dawn_scene(const TitleObservation& title) noexcept;

struct SceneSample {
    bool available = false;
    TitleSceneFields fields{};
};
// The verified title's scene fields; unavailable for any other title.
SceneSample scene(const TitleObservation& title) noexcept;

}
