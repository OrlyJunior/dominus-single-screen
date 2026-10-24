#pragma once
#include <windows.h>
#include "dominus/graphics.hpp"
#include "dominus/marker_blink.hpp"
namespace dominus::live {
using Log=void (*)(const char*) noexcept;
// Explicit experimental Portrait preset, observed at 1920x1080. No other title
// or Konami layout is inferred to be supported by this adapter.
void initialize(const wchar_t* directory, bool initiallyEnabled, Log log) noexcept;
gfx::Request frame(HWND window) noexcept;
// Snapshot of pause ownership and voluntary map-close state synchronized with
// the most recent frame() evaluation on the render thread.
marker::PauseContext pause_context() noexcept;
// Compositor hand-off when View opens the map: if true, sample both right-hand
// panels of the original frame (before composing) with gfx::sample_grid on a
// PanelColumns x PanelRows grid and pass them on (nullptr on failure).
bool panels_wanted(gfx::SourceRect& first,gfx::SourceRect& second) noexcept;
void panels_sampled(const unsigned* first,const unsigned* second,unsigned count) noexcept;
// The two right-hand panels of the preset, either of which may show the map.
void panel_rects(gfx::SourceRect& upper,gfx::SourceRect& lower) noexcept;
}
