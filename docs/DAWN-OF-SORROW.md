# Dawn of Sorrow (experimental, development branch)

Status: **experimental, behind a switch.** The profile is off by default
(`EnableExperimentalDawn=0` in the INI): Dawn then stays exactly as inert as in
v0.1.1. Portrait of Ruin keeps its validated v0.1.1 behavior with the switch on
or off. Two in-game sessions (28/10/2026, below) confirmed the single screen in
gameplay; the menus, the enlarged view, the warp room and the marker replay
described here are not validated in game yet.

The text avoids story, area, enemy and item details; mechanics are named only
generically.

## Intended experience

- **Gameplay:** the main screen alone, 4:3 with point sampling and black bars.
  In Dawn the main screen is also the DS touch screen.
- **Menus:** the title screen, the title menu and the file/mode select (while
  the top screen shows only its decorative page) and the pause menu opened with
  Start show their main screen alone: their options are on the touch screen.
  The start/load step between a menu and gameplay keeps the current layout, so
  nothing flashes.
- **View:** in gameplay with a live map it opens the enlarged map under the
  collection's native pause, from whichever panel holds the map (it follows the
  native page, not a fixed position), and **LB** or **RB** switch to the
  **Status** page (with the enemy information); **View** closes and resumes.
  Before the game gives the map, in the warp rooms (their own top screen) and
  in every menu, View stays native Select and nothing opens.
- **View + Y:** toggles the whole mod, as in Portrait, in Dawn's menus and in
  gameplay. The collection's own menu (which pauses the emulator) keeps its
  original input and layout.
- **Warp rooms:** the destination choice is played on the main screen (the
  castle map, the d-pad, A/B, a tap on a destination), so it stays single
  screen, with touches remapped like in gameplay; entering, cancelling and
  warping never flash the original layout. The top screen meanwhile shows only
  a picture of the highlighted destination's area name; it is hidden like any
  top screen (View+Y shows the collection layout).
- **Automatic original layout:** boot screens, sequences, game over, the other
  game modes and every unknown state keep the collection's own layout and
  original controls (View is native Select). The switch follows native game
  state, never pixels or timers.
- **Touch:** the touch screen stays visible and enlarged. Mouse and PC
  touchscreen positions are moved from the enlarged image back to the original
  gameplay panel, so a touch lands where it is aimed; the black bars are no
  touch. While the enlarged view is open, and after it until the mouse buttons
  are released, no touch reaches the paused game.
- **Map marker:** while the map is enlarged it keeps pulsing with the game's
  own frames, captured just before the pause and replayed as in Portrait.
  Nothing is drawn over the map: when no complete cycle was captured at the
  current position (the view opened right after moving), it stays frozen.

## LB/RB: why it is safe

- [FACT] game.exe reads the `l1_button`/`r1_button` Steam actions only in the
  pad builder 0x14022a920, which the mod already hooks (references from
  0x140229de0/0x14022a570 are writes of the handles). The collection turns them
  into DS L/R. dra01's game code uses them as gameplay buttons through its own
  configurable action table (0x1802942c0).
- [FACT] Under the collection pause the DS does not run (EmuTask frame loop
  0x14002aef0), so nothing is triggered while the view is shown.
- [FACT] dra01 detects presses as `trigger = (previous ^ current) & current`
  (0x18001fb90): a button still held when emulation resumes would be a new press.
- Rule (`ViewInputOwner`, `PageButtonFilter` with `ViewReservableButtons`):
  the view owns the controller from the View release that requests it until
  the View release (or View+Y) that closes it; not while its pause is being
  released. A LB/RB press that begins in the view is a page command; that
  press, and any other action that begins then (A, B, X, Y, Start, ...), is
  consumed **until the button is released, also after the view closed**.
  Directions are never withheld: they are held movement, so one held across
  the close keeps moving, as after the game's own pause menu. A button already
  held when the view opened passes unchanged, so resuming sees no new edge for
  it. LB and RB pressed together cancel out. View stays the recognizer's. The
  filter can only withhold presses: no button can be added to the game's input
  except the recognizer's View replay (`input_observer` forward()).
- [FACT, second session 28/10/2026] The first rule kept the view's ownership
  until the release of its pause was confirmed (16-32 ms after the View
  release) and withheld directions too, so a direction pressed while or right
  after closing was ignored until pressed again (logged d-pad bits withheld).
- Limit: analog sticks and triggers are never filtered; a stick held across the
  close acts as soon as the game resumes, as it would natively.

## Identification

- [FACT, runtime log v0.1.1] Choosing Dawn in the collection menu sets the
  EmuTask module index 0 with the boot module (`dra01.dll`) loaded; the loader
  table 0x14027def8 maps index 0 to `dra01.dll`.
- The detector verifies Dawn exactly like Portrait: index 0 with +0x184 set,
  +0x528 equal to the loaded `dra01.dll` from the game directory, MEM_IMAGE x64
  DLL, SHA-256 `52E20914E3668ED3E88BC858540292D52FCF6ED3DFF6F461AC49B53FA2F9CD6E`
  (3,872,768 bytes) with a matching PE identity, and two equal field reads.
  Each title's file is hashed once per loaded image. The title gate needs two
  verified frames of the same title, module and task epoch.

## dra01.dll scene state (Ghidra 12.1.4, analysis copy)

| Fact | Address |
| --- | --- |
| GameThread loop calls `modes[mode]()`; -2 advances, other values select the mode | 0x18000b700, table 0x18026c600, mode 0x1808eb248 |
| mode 2 gameplay; sub-state 0x180963ecc (0/1 entering, 2 running, 3 room change, 4 the warp room's destination choice, 6 area load), followed by its step | 0x18002b1b0 |
| the warp room device stores the word 0x104 there (sub-state 4, step 1) on Up at its centre, freezes the game and sets event flag 1; the only writer of 4 | 0x180077390 (0x180077f3e) |
| the choice: d-pad between unlocked destinations, A/B (swapped by the button setting), a tap on a destination; ends with the word 1 | 0x180078500, case 4 of 0x18002b1b0 |
| Start in running gameplay enters mode 0xa (pause menu) unless flags 0x200001 of 0x180963ed4 block it | 0x18002b1b0 |
| area load freezes the game (0x1808eb250 = 1) and sets sub-state 6 | 0x1800191f0 |
| brightness fade: frames left 0x1808f5c2c, level 0x1808f5c34 (0 normal) | 0x18002b1b0 (default case) |
| game type 0x180964346: 0 normal game, other values other modes | 0x18002bb20 |
| mode 3 title screen, 5 game over, 0x16 closing sequence, 0/1 start/load | mode table entries |

## Top-screen pages

- [FACT] The top-screen manager 0x180022100 (called by the loop after the mode)
  shows page 0x180965748 and switches to the requested 0x180965749. Page 0 is the
  map (0x18006b040 sets up the map), page 1 Status; native Select toggles them
  (0x180022010) unless a scripted event locks it (flag 0x10000000).
- [FACT] Each frame the shown page runs on the first sub-screen bank
  (selector 0x1808e6974 = 0) and, for pages 0/1 without the overlay flag
  0x18096574b, **the other page runs on the second bank** (selector 1): both the
  map and Status stay live. Same architecture as Portrait.
- [FACT, Portrait runtime log] The collection shows the first bank in the upper
  right panel: Portrait page 0 put the map upper, page 1 lower.
- [INFERENCE] Dawn's map panel is therefore upper for page 0 and lower for
  page 1; the profile reads it from the page instead of from pixels.
- Pages 2 and up replace the pair: 2 the warp rooms' own screen (below), 3 a
  decorative "paper" screen (`f_paper1/2`) used by the menus and the pause menu,
  others for menus and modes. Flag 0x10000000 disables native Select switching.
- [FACT, first in-game session 28/10/2026] In the opening of a new game the top
  screen stayed on page 3 for the whole gameplay logged, then on page 1 with the
  overlay set; the switching lock stayed set throughout. The map/Status pair was
  never shown, so the top-screen page cannot gate the single screen.

## Warp rooms

- [FACT] Page 2 is the warp rooms' top screen: a wall (`f_wall_chara`) with the
  localized picture of an area's name (`/sc/f_area*.dat`; 0x18006ccb0, object
  0x18006d440). It stays while the room is one of the 12 warp rooms
  (0x18006d330, table 0x180270000), where native Select never switches pages
  (0x180022010).
- [FACT] The destination choice runs on the main screen: the castle map with a
  marker per unlocked destination, a pulsing cursor, a dark backdrop over the
  frozen gameplay. Input: the d-pad, A to warp, B to cancel (swapped by the
  button setting), or a tap on a destination's 16x16 box, read from the same
  touch coordinates as in gameplay (the pointer remap applies). The top screen
  only changes its picture to the highlighted destination's area.
- [FACT] Only the warp room device writes sub-state 4, with step 1, in one
  store; the game freeze and event flag 1 follow, the flag after the map is
  drawn. The choice ends with sub-state 1 (back in the room) or a fade and an
  area load (warp). The step-0 path (0x18002ad80) has no producer.
- Rule: single screen in sub-state 4 step 1 with page 2 shown and requested.
  Neither the flag nor the freeze is required, so the frames between the store
  and the flag keep the single screen. Any other sub-state 4 keeps the original
  layout. View stays native Select (which the game ignores in warp rooms).
- [FACT, second session 28/10/2026] The four choices logged (sub-state 4, page
  2/2, frozen, flags 0x1000031) were shown in the original layout by 414645a.
  The room, the choice, its exit and a warp's area load and arrival logged then
  are regression tests (core `dawn_warp_choice_stays_on_the_main_screen`); the
  step was not logged yet and is assumed 1 there (its only writer).
- [INFERENCE] The area picture is all the single screen hides: the destinations
  carry no labels on the main screen, their place on the map identifies them.
  Showing it would need a new layout (the top panel beside the main screen),
  not done.

## Map marker

- [FACT] dra01 draws the map marker in 0x18004d590, object 3 of the
  sub-screen object list of the bank that shows the map (0x180965390 first
  bank, 0x180965638 second; set up by 0x18004c2c0). Its int16 counter +0x5e
  advances once per top-screen frame and wraps at 39; the affine scale
  ((40 - p) * 0x666 + 0x800) >> 12 steps from 16 down to 1 with holds 2, 2, 3,
  2, 3, ... (16 steps): Portrait's cycle and formula.
- [FACT] The sprite is 16x16, 4bpp, semi-transparent and affine in double-size
  mode; its visible part never exceeds 16x16. Its position comes from the
  player (handle 0x180964440: family 3, pool type 1, 400 slots of 0x468 at
  0x1808f5c48; int32 x/y at +0x2c/+0x30, divided by 0x40000/0x30000), the room
  0x180963f08/0x180963f0a and the area 0x180963f0d (written 0-2 only; per-area
  offsets).
- [FACT] The counter ignores the game's own freeze (0x1808eb250 is not read on
  that path); the collection pause under the enlarged view stops the whole DS,
  so the marker froze in one phase: that is why it did not pulse with replay
  off.
- Replay, as in Portrait: `marker_identity` reads those fields from the
  hash-verified dra01 (two equal reads, bounded pool slot) and starts a new
  epoch whenever a term of the drawn position changes; the tracker captures the
  map panel's own frames while the game runs and, under the pause, replays a
  complete cycle seen at the current position. Dawn's changed-pixel limit per
  transition is its sprite's 256 (Portrait: 192); every other limit is
  Portrait's (`marker_cycle_limits`). A partial or refused cycle stays frozen.
- [HYPOTHESIS] The captures, how many of the 16 steps look distinct (Portrait
  shows 13) and the bank-to-panel mapping are to be confirmed in game;
  `EnableMarkerTrace=1` logs the plans and dumps the phases.

## Rules (fail-closed)

- **Single screen** needs Dawn verified and the mod on, and then one of: mode 2,
  game type 0 and a known sub-state (0-3, 6), whatever the top screen shows, or
  the warp room's destination choice (above); modes 3 (title screen), 4 (title
  menu) or 9 (file/mode select) with the top screen on page 3, shown and
  requested; mode 0xa (pause menu) of the normal game. Modes 0/1 (start/load)
  hold the layout already shown.
- **View is the mod's command** only in that gameplay with a live map: shown
  and requested page in {0, 1}, no overlay, native switching allowed. Elsewhere
  it stays native Select.
- **Enlarged map (and its pause)** additionally needs: sub-state 2, shown ==
  requested (no page switch in progress), game not frozen, no fade, applied
  brightness 0 (level / 4096), the game's own pause menu allowed, input not
  held by a foreign pause, and the usual lease checks; no page authorizes it
  alone. The map's panel follows the page: page 0 shows it on the first bank
  (upper panel), page 1 on the second (lower panel); LB/RB switch to the other
  one. Under the mod's pause the DS is frozen, so a valid view stays valid;
  anything else closes through the lease checks (never a blind unpause).
- [FACT, second session 28/10/2026] A fade can leave a residual level (logged
  4, 8, 16) after its frames end; the hardware applies level / 4096, so such
  levels show nothing and must not refuse the view.
- Unreadable or inconsistent fields: unknown, original layout, native input.

## Touch and pointer (game.exe)

- [FACT] dra01's gameplay touch handler 0x18006fef0 adds the camera position to
  touch coordinates: the gameplay screen is the touch screen.
- [FACT] game.exe polls the cursor once per frame in 0x1401a2250 (DirectInput
  devices, then `GetCursorPos` + `ScreenToClient` on its window 0x14059e9c8) and
  stores client pixels at 0x14059a1fc/0x14059a200, -10000 when outside; the
  consumers 0x14000f580 and 0x1400071d0 turn them into touches through the
  original layout (the touch flag needs a position and a held button).
- The pointer adapter (`pointer_remap`, installed only with the experimental
  switch) runs after that poll: in single-screen gameplay it maps the point from
  the presented 4:3 image to the same spot of the original gameplay panel
  (`presented_to_original`), black bars become -10000, and during the view (and
  until release) the position is -10000. Its 47-byte prologue is verified first.
- [HYPOTHESIS] Controller-driven touch (a collection cursor) works unchanged:
  the collection computes it in its own layout and draws it inside the gameplay
  panel, which is what the mod enlarges.
- [HYPOTHESIS] Magic Seals and other touch interactions are drawn on the
  gameplay/touch screen and use the same input path. To validate in game.

## First in-game session (28/10/2026, build c341626)

Evidence from the runtime log of that session: Dawn was verified
(`title=dawn`, `dawn-verified`), the native, Steam pad and pointer hooks were
installed, and View+Y switched the mod on and off nine times in Dawn's menus
and in gameplay. Nothing changed on screen because the scene verdict never
allowed the single screen: the opening gameplay kept page 3, then page 1 with
the overlay, and the rule required the map/Status pair. No composition was
requested or rejected. The rule now follows gameplay; the logged fields are
regression tests (core `dawn_first_session_fields_reach_single_screen_in_gameplay`).

## Second in-game session (28/10/2026, build 414645a)

Gameplay showed the single screen and the pointer remapping ran. Remaining
issues, with their causes in that build: the title menus (modes 3, 4, 9, top
screen on page 3) and the pause menu (mode 0xa) kept the original layout
because only mode 2 allowed the single screen; and View would have opened the
shown top-screen page alone where no map was live. Evidence from the same log:
the map arrived on page 0 after an area load (sub-state 6), with the switching
lock cleared, and the pause menu kept page 0 on the top screen; statically, the
title menu (0x1800512a0) and the file select (0x1801e21c0) read the touch screen
and the pause menu runs its sub-menus 1-8 there (0x180059390). The logged fields
are regression tests (core `dawn_second_session_menus_and_map_fields`).

## Not established yet

- The collection layout in Dawn (assumed identical to Portrait's: gameplay left,
  two panels right) and which bank goes to which panel.
- Composition, touch remapping and the enlarged view in Dawn: never reached in
  the first session, still to validate.
- Which collection panel shows each sub-screen bank in Dawn (assumed as in
  Portrait: the first bank upper, the second lower); the map's panel follows it.
- Whether any pause sub-menu needs the top screen (stats, descriptions): it is
  hidden in the single screen; View+Y shows the collection layout.
- Sub-state 5 and every flag bit used above beyond the checks documented
  here; validation logs print all of them (`live,scene,title=dawn,...`, now
  with the sub-state's step).
- The marker replay in game: whether a cycle completes (one 39-tick lap or two
  laps) and matches the frozen frame.
- Mouse remap assumes the back buffer is stretched over the client area, as the
  collection's own conversion does.

## Validation plan (needs explicit authorization)

Pre-flight: fresh progress snapshot, verified rollback of v0.1.1, Steam Cloud
off, the experimental build kept apart from the validated install, a Dawn save
from the very beginning of the game only.

1. Switch off: Dawn behaves as v0.1.1 (`other-title`), Portrait unchanged.
2. Switch on: title screen and menus in the original layout; logs show
   `title=dawn` verified and scene fields.
3. Gameplay: single screen; View opens the map (screenshot of both panels to
   confirm the layout and the page/panel mapping); LB/RB switch pages; View
   closes. Repeat with LB/RB held while closing: no action on resume.
4. Pause menu, room changes, an event with its own top screen: original layout
   and native Select there; no map opens during fades or frozen states.
5. Warp room: enter, choose with the d-pad and with a tap, cancel, warp: single
   screen throughout, taps land on the destinations, no flash of the original
   layout; the log shows sub-state 4 with step 1.
6. Map marker: after standing still for two seconds, View: the marker keeps
   pulsing; right after moving: frozen at first, pulsing once reopened after
   standing still. With `EnableMarkerTrace=1`: `marker,title=dawn` and
   `marker,plan,...,valid=1` with `periodTicks=39`.
7. Mouse/touch: touches on the enlarged image land on the aimed spot; no touch
   from the black bars or from clicks made in the view.
8. View + Y with the view open; controller disconnection; the collection menu.
9. Normal exit; game files and progress verified.
