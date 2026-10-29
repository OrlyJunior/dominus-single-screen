#pragma once
#include <cstdint>
#include "dominus/core.hpp"

// Read-only player position identity for the titles whose marker cycle was
// established (Portrait; Dawn experimental). This never calls a game function,
// installs hooks, changes pause/input, or supplies a rendered marker.
// A coherent sample is not an atomic association with the current backbuffer;
// the caller must retain its panel/context and asynchronous-capture guards.
namespace dominus::marker::identity {
struct Sample {
    bool available = false;
    std::uint64_t epoch = 0; // Always zero when unavailable; never a position.
};

// title is the verified title whose DLL is read (any other title is never
// available); panel is the caller's native panel index (0 or 1). Position is
// global to the active player; the caller, not this provider, establishes
// which panel is map. An observed loss of availability forces a new epoch on
// the next good sample.
Sample sample(Title title, unsigned panel) noexcept;
}
