#pragma once
#include <cstdint>
#include <d3d11.h>
#include "dominus/graphics.hpp"

// Replays an observed native multi-state marker cycle during the collection
// pause. Partial or changed sequences keep the frozen frame. Captures are
// asynchronous; replay changes only a patch of the compositor's private copy.
// Presentation only: no game state is written, no simulation is advanced.
namespace dominus::marker {
using Log = void (*)(const char*) noexcept;
struct Settings {
    bool trace = false;   // log tracker events and plans, dump a few images
    bool replay = false;  // replay complete native cycles during the pause
    bool nativeIdentity = false; // production Portrait: read-only position veto
};
struct PauseContext {
    bool supplied = false;
    bool paused = false;
    bool pauseOwned = false;
    bool voluntaryClose = false;
    bool releasePending = false;
    bool releaseConfirmed = false;
    std::uint64_t taskEpoch = 0;
    std::uint64_t leaseToken = 0;
    std::uint64_t connectionEpoch = 0;
};
void configure(const Settings& settings, const wchar_t* directory, Log log) noexcept;
bool active() noexcept;
struct Patch { ID3D11Texture2D* texture = nullptr; unsigned left = 0, top = 0; };
// Render thread, once per Present, before composition. mapPanel: index into
// panels shown full screen under the mod's pause, or -1. tracking: the mod is
// on. Disabling it invalidates all captures. The patch, if any, is valid until the
// next call and goes into gfx::Request::patch.
Patch on_present(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* backbuffer,
                 const gfx::SourceRect (&panels)[2], int mapPanel, bool tracking,
                 const PauseContext& pause = {}) noexcept;
}
