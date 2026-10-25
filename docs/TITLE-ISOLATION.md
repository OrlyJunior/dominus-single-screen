# Title isolation and the gameplay-only map (v0.1.1)

v0.1.0 never identified the active collection game. Its live controls ran in
every title: with the mod off, View was held until release and replayed as an
80 ms Select (holding Select was impossible) and View+Y toggled the mod; with
the mod on, the Portrait preset and pause commands could apply to another game.
v0.1.1 answers two separate questions before acting:

1. **Is Portrait of Ruin the verified active game?** Only then may the mod be
   enabled, toggled with View+Y and change the presentation.
2. **Is Portrait in gameplay with its map shown?** Only then is View the map
   command, and only then may the enlarged map open.

Everything below was established statically with Ghidra 12.1.4 on the exact
files the runtime accepts and then validated in game (VALIDATION.md). Addresses
are virtual addresses at each image's default base.

## Active title: the collection's EmuTask

`game.exe` SHA-256 `E6FA885F560C8A47640FF8DF4DE3CE4A938C8053EB11A90A7B65F5EBF1AB6B64`:

| Fact | Address |
| --- | --- |
| `EmuTask::loadEmuModuleNum` (named by its error strings): `FreeLibrary(+0x528)`, `+0x184 = 0`, `LoadLibraryA(table[index])` into `+0x528` (table 0x14027def8: dra01, dra02, dra03, ac_dracula) | 0x1400299fc, 0x140029a14, 0x140029a23, 0x140029a29 |
| then `GetProcAddress(+0x528, "m2epi_moduledata__draNN")`, the module's init, `+0x53c = index`, `+0x184 = 1` | 0x140029afb, 0x140029bc6, 0x140029bd1 |
| script setter `setEmuModuleNum` writes only `+0x53c`; getter reads it | 0x140029bf0, 0x140029c00 |
| reinitialize callback: index < 0 becomes 0, index 4 is the static module (no DLL) | 0x140029fb0 |
| constructors: `+0x528 = 0`, `+0x53c = -1`, `+0x184 = 0`, `+0x185 = 1` (paused); task published at 0x140599fd0 | 0x140026d83, 0x140026d97, 0x140010da1, 0x140026dd8 |
| save files are named from `index + 1` (`dra0N_data_…`) | 0x140026900 |

The index alone is never proof (the setter writes it before the reload) and
`+0x184` is clear during a load. The EmuTask lives for the whole process; the
start-up reinitialization loads dra01.dll behind the collection menu.

`title_identity` reports Portrait only when, in one Present: the task at
`game.exe+0x599FD0` has the derived vtable and is the task the native observers
track; index is 1 and `+0x184` is set; `+0x528` equals the loaded `dra02.dll`
(no load or pin), mapped as an x64 DLL from the `game.exe` directory, whose file
is SHA-256 `FFEFA1109A43FA7A019A80EE16C6F287884302C8CBB921D361F595E730C38744` with
a matching PE identity; and a second read of the fields plus the module handle
agree. The hash is computed once per loaded image; a new task epoch re-checks
only the file identity. Anything else is a named non-Portrait reason.

## Gameplay scene: Portrait's own state

`dra02.dll` SHA-256 `FFEFA110…8744` (five bytes, read twice, never pixels):

| Fact | Address |
| --- | --- |
| GameThread loop calls `modes[mode]()` each frame; the return selects the next mode | 0x1800149a0, table 0x1803695a0, mode 0x18098c224 |
| mode 2 gameplay (sub-state 0x1809b1e3c: 2 running; Start enters 7), 5 title, 0xb Konami logo, 0/1 start/load, 7/8/9 in-game menus, 0x12/0x13/0x16 start menus, 0xd–0x11/0x17 scripted sequences | 0x180037d80, 0x180139e10, 0x1801f7c30, 0x180038cb0, 0x1800386d0 |
| sub-screen page manager: shows 0x1809bb6f8, switches to request 0x1809bb6f9; page 0 is the map (Status alongside), 1 Status (map alongside), toggled by native Select; title images use 5/6, menus 4 | 0x180022ab0, 0x1802e6b10, 0x18002da20 |
| game type 0x1809b2661: 0 normal game; 1–5 extra modes draw another screen alongside | 0x180022ab0, 0x180139e10 |

- **gameplay** = mode 2 and game type 0: View is the map command. Elsewhere
  (title, menus, sequences, extra modes, unknown or unreadable state) View stays
  native Select, replayed on release, and View+Y still toggles the mod.
- **map ready** = gameplay, sub-state 2, page 0 or 1 with no switch pending, and
  input not held by a foreign pause. A View released in gameplay without a ready
  map is consumed and ignored: no map, no pause, no Select page flip.

The page alone would not do: it is 0 at start-up, before the Konami screen
requests page 6. A map whose scene stops being ready closes through the lease
checks (under the mod's pause the DS is frozen, so a valid map stays valid). A
View held across a scene change is drained: no map, no Select, no second
command. This classifier is Portrait's; another title without its own classifier
is never treated as gameplay.

## Gating

`PortraitGate` turns identity on after two consecutive verified frames of the
same module and task epoch, and off at the first unverified frame. Input also
needs a known pause state that is clear or the mod's own (lease, request in
flight or a stale mod pause), so the collection's menus, which pause the
EmuTask, receive their original input. Without identity the presentation is
original, the marker stops tracking, pause requests stop, an owned map pause is
released through the existing lease checks (never a blind unpause), queued
commands are dropped and the filter returns the exact raw buttons. Every gate
change resets the gesture recognizer. The mod's on/off state is kept.

Unchanged: map with native pause, View+Y, the 39-tick marker cycle, its tracking
and retention, the disconnect path and stale-pause recovery. Native hooks remain
pass-through observers in every title. `EnableNativeObserver=0` leaves the mod
inert.

## Limits

- After leaving Portrait the collection menu still names Portrait (index 1,
  dra02 loaded) but pauses the EmuTask, so its input and layout stay original;
  choosing another game writes its index first, which ends Portrait at once.
- Extra modes, Boss Rush, endings and every in-game event were not validated one
  by one; outside mode-2 normal gameplay View stays Select.
- Hashing dra02.dll costs one longer frame per Portrait load.
- A changed `game.exe` or `dra02.dll` leaves the mod inert until re-investigated.
