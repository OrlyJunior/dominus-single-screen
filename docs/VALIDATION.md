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

## v0.1.1 candidate: title isolation (synthetic only)

Built with Zig 0.13.0 (development build, not `-Release`) into `out/`. Core
passed 46 groups (38 existing + 8 for identification, caching, races, transitions
and gating); the Steam-pad test passed 9 groups against a local analysis copy,
including every one of the 65,536 Steam action combinations forwarded unchanged
through the real pad-builder code when the gate is inactive. The WARP host passed
19 groups with `--proxy` against the new DLL (18 existing + the real Windows title
platform failing closed in a non-game host); the unknown-process guard passed;
Python passed 19 cases and the installer 11 fixtures with RealGameTouched=false.
No compiler warnings came from project sources.

Nothing was launched, attached to or installed: the title verdicts, the
collection-menu pause assumption and RE-1b in [TITLE-ISOLATION.md](TITLE-ISOLATION.md)
still need the real-test plan described there.
