# Dawn of Sorrow (experimental, development branch)

Status: **implemented behind a switch, not validated in game.** The profile is
off by default (`EnableExperimentalDawn=0` in the INI): Dawn then stays exactly
as inert as in v0.1.1. Portrait of Ruin keeps its validated v0.1.1 behavior
with the switch on or off. Nothing here was installed or run in the game.

The text avoids story, area, enemy and item details; mechanics are named only
generically.

## Intended experience

- **Gameplay:** the main screen alone, 4:3 with point sampling and black bars.
  In Dawn the main screen is also the DS touch screen.
- **View:** opens the enlarged top screen under the collection's native pause,
  always on the **map**. **LB** or **RB** switch between the map and the
  **Status** page (with the enemy information); **View** closes and resumes.
- **View + Y:** toggles the whole mod, as in Portrait.
- **Automatic original layout:** the collection's own layout is shown whenever
  the game needs more than its main screen: the pause menu (which uses the top
  screen for its own page), the title screen and start menus, sequences, game
  over, the other game modes, in-game events that put their own page on the top
  screen, and every unknown state. Controls are the original ones there (View is
  native Select). The switch follows native game state, never pixels or timers.
- **Touch:** the touch screen stays visible and enlarged. Mouse and PC
  touchscreen positions are moved from the enlarged image back to the original
  gameplay panel, so a touch lands where it is aimed; the black bars are no
  touch. While the enlarged view is open, and after it until the mouse buttons
  are released, no touch reaches the paused game.
- **Map marker:** shown frozen in the enlarged map (no replay yet; see below).

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
- Rule (`PageButtonFilter`): from the View release that requests the view until
  the release of its pause is confirmed, a LB/RB press that begins is a page
  command and that button is consumed **until it is released, also after the
  view closed**. A button already held when the view opened is not a command
  and passes unchanged, so resuming sees no new edge. Both pressed together
  cancel out. Other buttons are never touched; Y, LB and RB can never be added
  to the game's input (`input_observer` forward()).

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
| mode 2 gameplay; sub-state 0x180963ecc (0/1 entering, 2 running, 3 room change, 4 an event selection screen, 6 area load) | 0x18002b1b0 |
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
- Pages 2 and up replace the pair (2 during some in-game events, 3 the pause
  menu, others for menus and modes): original layout.

## Rules (fail-closed)

- **Single screen** needs: Dawn verified, mod on, mode 2, game type 0, a known
  sub-state (0-3, 6), shown and requested pages in {0, 1}, no overlay and no
  scripted page lock. Pages alone never decide.
- **Enlarged map (and its pause)** additionally needs: sub-state 2, shown ==
  requested, game not frozen, no fade, brightness 0, the game's own pause menu
  allowed, input not held by a foreign pause, and the usual lease checks. Under
  the mod's pause the DS is frozen, so a valid view stays valid; anything else
  closes through the lease checks (never a blind unpause).
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

## Not established yet

- The collection layout in Dawn (assumed identical to Portrait's: gameplay left,
  two panels right) and which bank goes to which panel.
- The exact purpose of page 2, sub-states 4/5, and every flag bit used above
  beyond the checks documented here; validation logs print all of them
  (`live,scene,title=dawn,...`).
- Marker replay: Dawn's marker animation period and a dra01 position identity
  (candidate starting points: sub-screen object lists 0x1809651f8/0x1809654a0,
  0x88-byte objects, map setup 0x18004c2c0/0x18004c5d0). Until then the marker
  is frozen; Portrait's 39-tick period and position provider are not used.
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
5. Mouse/touch: touches on the enlarged image land on the aimed spot; no touch
   from the black bars or from clicks made in the view.
6. View + Y with the view open; controller disconnection; the collection menu.
7. Normal exit; game files and progress verified.
