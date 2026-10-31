#pragma once
#include <windows.h>
#include "dominus/graphics.hpp"
#include "dominus/marker_blink.hpp"
namespace dominus::live {
using Log=void (*)(const char*) noexcept;
// Explicit experimental presets per verified title (core AdaptationProfile).
// No other title or Konami layout is inferred to be supported by this adapter.
void initialize(const wchar_t* directory, bool initiallyEnabled, Log log) noexcept;
gfx::Request frame(HWND window) noexcept;
// Snapshot of pause ownership and voluntary map-close state synchronized with
// the most recent frame() evaluation on the render thread.
marker::PauseContext pause_context() noexcept;
// What the native marker follows after the most recent frame(): tracking only
// for a title whose replay was established (title: the verified one) and a
// non-original presentation; mapPanel is the panel holding the map under the
// mod's pause (0 upper, 1 lower), also while the view shows the other page,
// else -1.
struct MarkerTarget { bool tracking=false; int mapPanel=-1; Title title=Title::Unknown; };
MarkerTarget marker_target() noexcept;
// Pointer handling for touch-remap titles (Dawn) after the most recent
// frame(): remap while it returned single-screen gameplay, hide while the
// enlarged view reserves input (requested, shown or releasing its pause).
struct PointerPolicy { bool remap=false; bool hide=false; };
PointerPolicy pointer_policy() noexcept;
// Compositor hand-off when View opens the map: if true, sample both right-hand
// panels of the original frame (before composing) with gfx::sample_grid on a
// PanelColumns x PanelRows grid and pass them on (nullptr on failure).
bool panels_wanted(gfx::SourceRect& first,gfx::SourceRect& second) noexcept;
void panels_sampled(const unsigned* first,const unsigned* second,unsigned count) noexcept;
// The two right-hand panels of the active preset, either of which may show the map.
void panel_rects(gfx::SourceRect& upper,gfx::SourceRect& lower) noexcept;
}
