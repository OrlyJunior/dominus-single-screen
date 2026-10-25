# Changelog

## 0.1.1 — 2026-10-25

- The mod acts only while Portrait of Ruin is the verified active game. Dawn of
  Sorrow, Order of Ecclesia, Haunted Castle and the collection's menus keep their
  original input and layout, with the mod on or off (0.1.0 delayed Select there).
- The enlarged map opens only during Portrait gameplay. On the title screen,
  start menus and the in-game menu View stays native Select; View+Y still
  toggles the mod. In gameplay without a map to show, View does nothing.
- The collection's own menus inside Portrait receive View/Y unchanged.
- Map, native pause, View+Y and the animated marker are otherwise unchanged.

## 0.1.0 — 2026-10-24

First source publication, based on the validated internal rc5 implementation.

- Proportional single-screen gameplay with black bars.
- Steam Input View map toggle with native pause and View+Y global toggle.
- Native marker capture/replay and safe stationary cycle retention.
- Portrait-specific 39-tick single-lap recognition to reduce capture warm-up.
- Fingerprint gates and non-overwriting manifest-based installation/removal.
- Portable build/test paths, corrected CMake target wiring, MIT project license
  and retained third-party notices.

Known limitation: map openings while moving or before a complete capture remain
safely frozen. Other titles and layouts are deferred. Internal rc3/rc4/rc5 builds
are development references, not reconstructed Git tags or public binary releases.
