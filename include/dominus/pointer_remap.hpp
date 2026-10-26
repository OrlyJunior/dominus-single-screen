#pragma once
#include "dominus/core.hpp"

// Experimental (Dawn of Sorrow only): keeps mouse and PC-touchscreen positions
// aligned with the mod's presentation. The collection polls the cursor once
// per frame in game.exe 0x1401a2250 (GetCursorPos + ScreenToClient on its
// window 0x14059e9c8) and stores client pixels at 0x14059a1fc/0x14059a200,
// -10000 when outside; 0x14000f580 and 0x1400071d0 then turn them into touches
// through the original layout. After that poll, this adapter moves a point
// aimed at the enlarged gameplay to the same spot of the original gameplay
// panel, reports black bars as outside, and hides the pointer while the
// enlarged view is open and until the mouse buttons are released after it.
// Nothing else is written; game.exe E6FA885F...6B64 only (the caller gates
// the hash) and the poll's bytes are verified before hooking.
namespace dominus::pointer {
using Log = void (*)(const char*) noexcept;
struct View {
    bool hide = false;        // the enlarged view reserves input
    bool remap = false;       // single-screen gameplay of a touch-remap title
    Geometry shown{};         // where the source is drawn, back-buffer pixels
    int backWidth = 0, backHeight = 0;
    NormalizedRect source{};  // the original gameplay panel
};
bool initialize(void* host, Log log) noexcept;
// Render thread, once per Present, with what was actually composed.
void set_view(const View& view) noexcept;
// Only after no game thread is calling the hook.
void shutdown() noexcept;
}
