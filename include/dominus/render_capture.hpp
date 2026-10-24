#pragma once

#include <windows.h>
#include <d3d11.h>

namespace dominus::capture {
using Log = void (*)(const char*) noexcept;
// Diagnostic-only integration. The caller must first validate the host build.
void initialize(Log log) noexcept;
// Must run before device creation; late loading cannot capture existing devices.
// Opt-in requires work/live/capture.enable; there are no global hotkeys.
void before_create_device() noexcept;
// Call after the original Present (except DXGI_PRESENT_TEST).
// A request starts the next complete frame, ending after the following Present.
void on_present(ID3D11Device* device, HWND window) noexcept;
}
