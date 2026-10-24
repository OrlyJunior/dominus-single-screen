# Changelog

## 0.1.0 — 2026-10-24

First source publication, based on the validated internal rc5 implementation.

- Proportional single-screen gameplay with black bars.
- Steam Input View map toggle with native pause and View+Y global toggle.
- Native marker capture/replay and safe stationary cycle retention.
- Portrait-specific39-tick single-lap recognition to reduce capture warm-up.
- Fingerprint gates and non-overwriting manifest-based installation/removal.
- Portable build/test paths, corrected CMake target wiring, MIT project license
  and retained third-party notices.

Known limitation: map openings while moving or before a complete capture remain
safely frozen. Other titles and layouts are deferred. Internal rc3/rc4/rc5 builds
are development references, not reconstructed Git tags or public binary releases.
