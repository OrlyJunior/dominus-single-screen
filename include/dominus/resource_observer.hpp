#pragma once
#include <d3d11.h>

namespace dominus::resources {
using Log = void (*)(const char*) noexcept;
// Diagnostic pass-through observer. Caller must validate the host build.
// Captures no texture bytes and never changes D3D arguments or resources.
bool initialize(ID3D11Device* device, Log log) noexcept;
// Starts a fresh per-frame logging budget; call once per real Present.
void on_present() noexcept;
// Only call after all contexts stop executing these hooks.
void shutdown() noexcept;
}
