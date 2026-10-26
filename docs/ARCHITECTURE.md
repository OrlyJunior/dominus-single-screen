# Runtime architecture

## Version gate and presentation

The x64 D3D11 proxy forwards system exports and only enables the supported game
fingerprint. Native hooks are deferred until the collection's task is ready;
initializing them earlier caused a startup regression in internal experiments.
The validated preset selects gameplay and either right-hand map panel from the
collection's original frame. Presenter copies the source, optionally patches its
private copy, draws point-filtered 4:3 gameplay/map and clears unused space black.
It restores the relevant D3D11 state and refuses unsupported states/resources.
This is not a general layout detector or a replacement for every D3D11 wrapper.

## Title isolation and gameplay scene

The common game.exe hosts every collection title, so the version gate alone
does not identify Portrait. title_identity reads the EmuTask's module index,
loaded HMODULE and ready flag twice per Present and accepts Portrait only for
the exact dra02.dll loaded from the game directory (hash cached per image and
re-checked on new task epochs). portrait_scene then reads Portrait's own mode,
gameplay state, game type and sub-screen pages. PortraitGate enables the mod's
presentation, toggling, pause and marker tracking after two verified frames and
disables them at once; View is the map command only in normal gameplay and the
map opens only while the sub-screen shows it. The input filter also stays out of
pauses the mod does not own; outside these contexts buttons pass unchanged.
See [TITLE-ISOLATION.md](TITLE-ISOLATION.md).

## Input and native pause

The tested controller reaches the game through Steam Input, so input_observer
filters the collection's normalized pad output, preserving its ABI. View is
resolved on release to distinguish View+Y; disabled View is replayed long enough
to act as Select. Other controllers and already-consumed Y presses have limits.

Native pause commands are queued to the safe native thread. Task epochs,
revision and lease ownership prevent releasing a pause owned by another actor.
The disconnect path releases the mod-owned pause before the collection handles
its own controller-loss menu. live::pause_context exposes a synchronized snapshot
of voluntary close, pending/confirmed release, task, lease and connection epochs.

## Native marker replay

MarkerCycleTracker keeps up to 16 real images and ordered occurrences, with
separate holds for repeated images. It can recognize two matching general cycles.
The Portrait-specific optimization recognizes a measured39-tick lap, or the first
return to the initial state with the initial hold complemented from the validated
39-tick native period, under minimum state-count, difference and hold bounds.
This inference is specific to the observed Portrait profile, not proof of another
animation's period. Sampling gaps, timing changes, new images and identities reset
confidence. An incomplete or uncertain sequence stays frozen.

PanelProbe asynchronously captures native pixels plus timestamps/position epochs,
retains GPU images, then prepares small patches of the changed union plus a guard
pixel. It does not draw a substitute marker. Captures during pause verify the
frozen source and do not train a new loop. Playback uses rational 60 Hz timing;
39 ticks correspond to 650 ms without accumulating per-hold millisecond rounding.

Cycle retention requires a voluntary close, the same task/lease/controller and
position, followed by a confirmed asynchronous unpause. Captures while Release
is pending do not train timings. Invalid identity/context/resource events discard
retention. This avoids both stale positions and the extra paused frame corrupting
a measured run after map closure.

The read-only Portrait position provider validates dra02.dll's SHA/PE and reads
bounded player-handle pools, area/room and quantized coordinates twice. Position
changes invalidate even visually identical collapsed-marker frames. A coherent
read is not an atomic association with the GPU/GameThread; freshness, matching
frozen pixels and cycle gates remain necessary. No game function is called and
no game state is written by this provider.

## Title profiles (development branch)

One decision point per title, the rest shared. title_identity verifies a title
from an enabled set (Portrait by default; Dawn only with the experimental INI
switch) and reads that title's own scene fields; core classifies them
(classify_portrait_scene, classify_dawn_scene) and TitleGate gates them. The
differences live in core's AdaptationProfile data: layout rectangles, how the
map panel is found (Portrait: pixels; Dawn: native page), whether the main
screen is shown alone in every verified scene (Portrait) or only where the
scene allows it (Dawn, original layout elsewhere), LB/RB page navigation
(PageButtonFilter), marker replay (Portrait only) and pointer remapping
(pointer_remap, Dawn only). A title without a profile stays inert. See
[DAWN-OF-SORROW.md](DAWN-OF-SORROW.md).

## Ownership and extension boundaries

Runtime code is split among core, graphics, input_observer, native_runtime,
live_controller, marker_blink, marker_identity, title_identity and pointer_remap.
render_capture and
resource_observer are optional diagnostics, not requirements for normal playback.
The RenderDoc adapter has its own opt-in/version/path checks and ships no tool DLL.

To add a title or update a fingerprint, validate its ABI, task lifecycle, native
pause, input consumption, surfaces, position identity and touchscreen behavior.
Do not interpret the common collection executable as validation of every title.
The source includes geometry/touch helpers but does not establish universal
touchscreen compatibility across all layouts or games.
