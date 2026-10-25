# Changelog

## 0.1.1 (unreleased candidate)

- Title isolation: the mod acts only while Portrait of Ruin is the verified
  active title (EmuTask module index, loaded dra02.dll handle, path and hash,
  read twice). Dawn of Sorrow, Order of Ecclesia, Haunted Castle and loads in
  progress keep original presentation and input, with the mod on or off.
- Inside Portrait, the collection's own menus (which pause the game) now receive
  View/Y unchanged; the mod's map, View+Y and marker behavior is otherwise kept.
- Scene gate: Portrait's own game mode, gameplay state and sub-screen page decide
  whether View is the map command and whether the enlarged map may open. On the
  title screen, menus and sequences View stays native Select (View+Y still
  toggles the mod); in gameplay without a map to show, View does nothing.
- Synthetic tests for identification, races, caching, scenes and gate transitions.

Title isolation validated in game (rc6); the scene gate is not yet (rc7).
See docs/TITLE-ISOLATION.md and docs/VALIDATION.md.

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
