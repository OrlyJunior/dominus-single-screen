# Validation and known limits

## Historical rc5 evidence (basis for v0.1.0)

Internal validation reported 38 core groups, 8 Steam-pad groups, 18 D3D11 groups,
19 Python cases and 11 synthetic installer fixtures passing. The in-game session
reported 21 map openings: 15 complete native 39-tick replays, 10 consecutive stationary
retentions, 15 confirmed pause releases, and 6 safe incomplete-capture fallbacks.
The user visually confirmed the behavior; the game exited normally. Original
files and progress were checked locally. Those private logs, save hashes,
process IDs, dumps and snapshots are intentionally not distributed.

Reference validated rc5 DLL SHA-256 (identifier only; no binary included):
`810F55B8FCD3182F696A2EEFF73B36A8144AD9CA783C5AE162E6F76D39E27A73`.

The v0.1.0 publication preserves runtime/header/test source bytes. Build scripts,
CMake and documentation are made portable; licensing and source-only hygiene are
added separately. See [PUBLICATION.md](PUBLICATION.md) and [BUILD.md](BUILD.md).

## Scope of synthetic tests

Core tests cover geometry, input gestures, pause ownership, marker order/holds,
39-tick closure, partial fallback, sampling gaps, identity changes and retention.
D3D11 tests use their own WARP resources and cover pixels/state preservation,
both panels, union patches, 16 slots, resize, asynchronous release and invalidation.
The Steam-pad test executes a copied function from a locally supplied supported
executable with fake Steam input. Installer fixtures never touch Steam/game data.
Python tests validate the static PE parser and fail-closed matching behavior.

Synthetic coverage does not establish full compatibility with every menu,
touchscreen interaction, controller, graphics state, collection layout or game.
CMake/MSVC is not equivalent to the historically exercised Zig build route.

## Remaining limitations

Opening before a full marker capture at the current position can freeze the
marker, including a nearly invisible phase. Moving invalidates the retained cycle.
Only Portrait, the documented layout, and the tested 16:9 resolutions are supported.
Other titles, keyboard shortcuts, second-controller filtering and other wrappers
are not validated. Diagnostic logs can accumulate. Collection-menu paths beyond
the tested disconnect behavior need further testing. New game fingerprints must
stay inactive until separately investigated and validated.

No gameplay was launched and no installed mod or save was changed to prepare
this source publication. Publication checks are recorded separately from the
historical in-game evidence; do not infer new gameplay validation from a push.

## Checks rerun for this source publication

The portable TestsOnly route compiled the copied sources with Zig 0.13.0,
without producing a new mod DLL. The rebuilt core passed 38groups; the rebuilt
WARP/proxy host passed 18groups against the separately preserved validated rc5 DLL.
Python passed 19cases and the portable installer passed 11fixtures with
RealGameTouched=false. These are synthetic/source-publication checks, not a new
in-game session. The optional Steam-pad executable compiled; its 8-group gameplay-
function test is retained as opt-in and was not rerun during publication.

A fresh Zig runtime cache emitted existing libunwind dllexport-redeclaration
warnings, but compilation completed successfully. CMake/MSVC configuration and
in-game tests were not rerun. The installed rc5 is preserved unchanged.

## v0.1.1 (validated as internal rc7, 2026-10-25)

Reference v0.1.1 DLL SHA-256 (Zig 0.13.0 `-Release`; identifier only):
`B9438FB55D39FCF531C0679F1A56EC17A810197166AC85402D5D58AC87B729E2`.
The runtime sources match [SRC-SHA256.json](SRC-SHA256.json).

Synthetic: core 50 groups (identification, races, caching, scenes, map commands,
gate transitions), Steam pad 9 (all 65,536 Steam action combinations forwarded
unchanged through the real pad-builder code when the gate is inactive), WARP with
`--proxy` 19 (the real title platform fails closed in a non-game host), guard 1,
Python 19, installer 11 fixtures with RealGameTouched=false.

In game, installed with the repository scripts over a verified private backup
(private logs, not distributed):

- an intermediate build with only the title gate (internal rc6) confirmed the
  isolation: start-up menu `other-title` (dra01), Portrait verified after a
  `module-mismatch` while the new index preceded the reload, the collection menu
  passing input unchanged, Order of Ecclesia and Dawn of Sorrow `other-title` with
  no filtered input; it also showed the defect fixed in rc7 (View on Portrait's
  title screen opened the enlarged graveyard image);
- rc7 session: Konami logo, title (pages 6/3) and start menu (page 4) were
  classified as non-gameplay with the mod on, and no map opened there; gameplay
  reported `mapReady` only on stable map/Status pages (native Select switches
  were seen as not ready); 8 map openings with native pause, all 8 pauses
  released; the in-game menu (Talk) kept View native; brief gameplay sub-state
  changes were not ready; Order of Ecclesia stayed `other-title`;
- progress: only the collection's automatic `*_008_*`, `savecfg.txt` and
  `remotecache.vdf` changed; game saves unchanged, nothing removed; installed and
  original game files verified afterwards.

Not validated individually: extra modes, Boss Rush, endings, Haunted Castle
(not visited in the logged sessions), and every in-game dialogue or event.

## Development branch: experimental Dawn of Sorrow (synthetic only)

Built with Zig 0.13.0 `-Release` into `out/`; nothing installed, no game run.
Core 60 groups (the 50 of v0.1.1 plus profiles, Dawn detection on/off,
per-title hashing, title gate, Dawn scene rules over every mode/sub-state/page,
dispatch, LB/RB and view-owned press reservation over every button
combination, the Dawn view input sequences, pointer remapping and hiding), WARP with `--proxy` 19
(the real title platform fails closed for Portrait and Dawn in a non-game host),
guard 1, Steam pad 10 (adds: presses can be withheld through the real builder
but never injected), Python 19, installer fixtures 14 with RealGameTouched=false
(adds file identity, process decision and live enumeration; run while
Castlevania Advance Collection was open, correctly identified and allowed).

The Dawn profile is off by default; with it off the detector, gate and input
paths behave as v0.1.1 (covered by the unchanged v0.1.1 groups). In-game
validation of Dawn is partial (below); see the plan in DAWN-OF-SORROW.md.

First in-game session of the experimental Dawn build (28/10/2026, c341626):
identification, hooks and View+Y worked, but the single screen never appeared
because the scene rule required the map/Status pair, which the opening of a new
game does not show (see DAWN-OF-SORROW.md). Fixed in the next build; core then
63 groups, other suites unchanged and passing. Dawn remains unvalidated in game.

Second session (414645a): single screen and pointer remapping worked in
gameplay; the title menus and the pause menu still showed both screens. The
next change (menus in single screen, View only with a live map, start/load
hold) passed core 64 groups in an isolated light build (core and the live
controller only); the full suites and the DLL wait for a machine without the
game running.

Follow-ups of that session (29/10/2026, branch dev/dawn-menus-map-view, not
tested in game): directions held or pressed while closing the view reach the
game and residual fade levels no longer refuse it; the warp room's destination
choice stays single screen; Dawn's map marker is replayed from its own frames.
Full offline suites, no game running: core 68 groups (adds the logged warp room
sequences and Dawn's marker cycle against both titles' limits), WARP with
`--proxy` 19 (adds: Dawn's identity mode stays frozen without dra01, and a
title change drops captures and restores the limits), guard 1, Steam pad 10,
Python 19, installer fixtures 14 with RealGameTouched=false.

Third session (7769e06, installed 29/10/2026): menus, the view, the pulsing
marker and directions after the view confirmed by the user; the merchants'
menus still showed both screens. Their follow-up (branch dev/dawn-shops, not
tested in game): core 69 groups (adds the logged merchant sequences), WARP with
`--proxy` 19, guard 1, Steam pad 10, Python 19, all built while the game was
open (no game process touched). The installer fixtures were not run then, to
stay clear of the running game; the installer scripts are unchanged since
their last 14/14 run.
