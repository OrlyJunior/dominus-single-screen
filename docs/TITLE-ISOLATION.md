# Title isolation (v0.1.1 candidate)

Status: **implemented and covered by synthetic tests; not yet validated in game.**
No game was opened, attached to or instrumented for this change, and no
installation, save or configuration was modified.

## Problem in v0.1.0

v0.1.0 never identified the active title. Its live controls start once per
process, so in Dawn of Sorrow, Order of Ecclesia, Haunted Castle and the
collection menus:

- with the mod off, View was held until release and then replayed as an 80 ms
  Select pulse (holding Select was impossible);
- View+Y toggled the mod and consumed Y;
- with the mod on, the Portrait crop/map preset and pause commands could apply
  to a different game's layout.

No save risk was found. Until v0.1.1 is validated, uninstalling the mod is the
way to play the other titles with original Select.

## Static evidence

Ghidra 12.1.4 through REA on `game.exe` SHA-256
`E6FA885F560C8A47640FF8DF4DE3CE4A938C8053EB11A90A7B65F5EBF1AB6B64` (addresses are
virtual addresses at the default image base 0x140000000):

| Fact | Address | Evidence |
| --- | --- | --- |
| `EmuTask::loadEmuModuleNum` (name from its error strings): `FreeLibrary(+0x528)`, `+0x184 = 0`, `LoadLibraryA(table[index])` stored in `+0x528`; table 0x14027def8 = dra01, dra02, dra03, ac_dracula | 0x1400299fc, 0x140029a14, 0x140029a23, 0x140029a29 | E1, E2 |
| then `GetProcAddress(+0x528, "m2epi_moduledata__draNN")`, the descriptor's init, `SetMSoundExportTable`, `+0x53c = index`, `+0x184 = 1` | 0x140029afb, 0x140029bc6, 0x140029bd1 | E3 |
| `setEmuModuleNum` writes only `+0x53c`; `getEmuModuleNum` reads it; both are script bindings registered by 0x140027030 | 0x140029bf0, 0x140029c00 | E4, E5 |
| reinitialize callback: index < 0 becomes 0; index 4 sets `+0x184` without a DLL; otherwise loads | 0x140029fb0 | E6 |
| derived constructor: `+0x528 = 0`, `+0x53c = -1`; base constructor: `+0x184 = 0`, `+0x185 = 1` (paused) | 0x140026d83, 0x140026d97, 0x140010da1 | E7, E8 |
| all disp32 writes to `+0x528`, `+0x53c` and, in 0x140010000–0x14002c000, `+0x184` | byte searches | E9, E10, E11 |
| save file names use `index + 1` (`dra0N_data_000_0000.bin`) | 0x140026900 | E12 |
| the script constructor builds the EmuTask (`sqobject` class) | 0x14002de60 | E13 |

Import slots 0x14026e1a8/b0/b8 resolve to KERNEL32 `LoadLibraryA`,
`GetProcAddress` and `FreeLibrary` (PE import table of the same file).

REA evidence IDs: E1 `ev_7f341a229dd7b4a77b621e96ffe4e7a3090c54cd0dc3d9e5024f590037c225f1`,
E2 `ev_7eec93116c91ef1f09b28a64606d3c5d30bd8aec570631d07e40a5470ed38a70`,
E3 `ev_ed037aab10ba4bf74ebd2c250459ba4bbf7bf72965a3971ef4b6e1fd88a53037`,
E4 `ev_57d6864e000fe38f655bbd4419846b529c64e4297d45c30a39fc5ededb338045`,
E5 `ev_78501753080f3ea775599dfa6ed049609ccd1a421ab2369a55640aac7f4eca5c`,
E6 `ev_7585e555f8e43ce1080bf73a2cbdaae7a56ae15f3813f352b2e5227df423c20e`,
E7 `ev_c9315b9682cc2bb93b2d05b73c8c53f25fa8e50e56f6d0dc1dd99213dd3a89f3`,
E8 `ev_772bb92e284f23f4dfab6dd3caba217b916cc5fc974ff10ece847d075210bc91`,
E9 `ev_6e9c4372ad8213a77ae2156625abcd2f48475c0df3a70c26e0f3eb4afb935619`,
E10 `ev_2784ef648ecb1072793cce9571e9d63509c7cbe3a5aa9245380fa427d90e34fa`,
E11 `ev_b8a1068cd4096b78439c7aeaa1149bf92a7a3550e423392b7751a90d7f3c44a9`,
E12 `ev_168f611674da8f29efbde684e527744c52ff6420a2f4576a967fcbcbdc1a42ad`,
E13 `ev_0923697283a37f2a4154a84b09bc601dd0db374a88e0a8ce9bd748cee041cf04`.

So the index alone is never proof (the setter writes it before the reload),
`+0x184` is clear for the whole load, and the module handle is the loader's.
The EmuTask is a script class (constructor called from 0x14002de60).

Lifecycle seen in the private local logs of 15 earlier Portrait sessions
(not distributed): the EmuTask is constructed during start-up and destroyed only
at process exit. It is first observed in the collection menu with `+0x185 = 1`
(paused). Starting a game pauses through the caller at RVA 0x22E41, runs two
reinitializations and unpauses through RVA 0x22E26. The start-up reinitialization
therefore loads dra01.dll behind the menu (index -1 becomes 0).

## Identification (`TitleDetector`, `title_identity.cpp`)

Once per Present, on the render thread, before the live controller's lock:

1. Read the task pointer at `game.exe+0x599FD0` and require the derived vtable
   0x140512850; require it to be the task the native observers track (its
   lifecycle epoch then bounds every cache).
2. Read vtable, `+0x184`, `+0x528` and `+0x53c` (bounded `ReadProcessMemory`).
3. Portrait requires index 1, `+0x184` set, and `+0x528` equal to
   `GetModuleHandleW("dra02.dll")` (no load, no pin).
4. The module must be a `MEM_IMAGE` x64 DLL mapped at that handle whose loader
   path is `dra02.dll` in the `game.exe` directory, and the file must be the
   validated dra02.dll (SHA-256 `FFEFA110…8744`, size, PE identity matching the
   mapped image, unchanged file identity before/after hashing).
5. Read the fields again and query the module again: any difference means a
   load, unload or destruction raced the checks.

Anything else is a named non-Portrait reason (`no-task`, `task-mismatch`,
`read-failed`, `no-module`, `other-title`, `static-module`, `loading`,
`module-mismatch`, `module-image`, `module-file`, `inconsistent`). Indices 0/2
are reported as Dawn/Ecclesia for diagnostics only.

Costs: while nothing changes a frame does two field reads and two module
lookups. The image/path check runs again only when the fields, handle or task
epoch change. The file is hashed once per loaded image (about 5 MB); a new task
epoch re-checks only its cheap file identity. A failed verification is retried at
most three times per epoch, five seconds apart.

## Gating (`PortraitGate`, `live_controller.cpp`)

- **Identity** turns on after two consecutive verified frames of the same module
  and task epoch, and turns off at the first unverified frame.
- **Input** additionally requires a known pause state that is clear, or owned by
  the mod (lease, request in flight, or a stale mod pause only the mod releases).
  The collection's menus pause the EmuTask, so they receive original input.

When identity is off: presentation is Original, the marker stops tracking
(captures discarded), map selection and pause requests stop, an owned map pause
is released through the existing lease/epoch-checked path (never a blind
unpause), recognized or `control.ini` commands are dropped and any replay is
cut. When input is off the filter returns the exact raw word: no View/Y
reservation, delay or chord; only an 80 ms pulse that Portrait itself started may
finish while Portrait is still verified. Every gate change resets the gesture
recognizer, so a View held into a new context is drained, never acted on. The
mod's on/off state is kept, so returning to Portrait restores the previous mode.

Unchanged in Portrait: View map with native pause, View+Y, the 39-tick marker
cycle and retention, the disconnect path that releases the mod pause first, the
stale-pause recovery and all rc5 fallbacks. Native hooks remain pass-through
observers in every title.

## Guarantees and limits

- Fail-closed: no Portrait-specific presentation, map, marker or pause command
  runs, and no button is changed, unless the verdict is a verified Portrait.
- Other DS titles, Haunted Castle, the static module, start-up before any load,
  loads in progress and unloaded or altered dra02.dll are never Portrait.
- `EnableNativeObserver=0` leaves the mod fully inert (no observed task).
- **Unknown (RE-1b):** whether returning from Portrait to the collection's title
  menu reloads or resets the module fields. No native path clears them; scripts
  were not analyzed. If they persist, the menu still reads as Portrait: then the
  input gate relies on the menu keeping the EmuTask paused (observed at start-up,
  not yet after leaving a game) and presentation already stays Original while
  another actor pauses. Confirm in the real test below.
- A paused-by-collection state inside Portrait (its own menus) now passes input
  unchanged; View+Y no longer toggles the mod there. Intentional, not yet tested.
- Hashing runs once per Portrait load on the render thread (one longer frame).

## Real-test plan (requires explicit authorization)

Before: verify the installed files' SHA-256, back up saves/configuration, keep
v0.1.0/rc5 as the rollback, use `DiagnosticDirectory` and keep marker traces
off. Never save manually, create save states or restore progress automatically.

1. Start-up menu: expect `title,…reason=other-title,index=0` (dra01 behind the
   menu); View/Y unchanged; View+Y does nothing.
2. Load Portrait: `portrait-verified` then `input-gate,active`; View+Y, map,
   39-tick marker, retention, disconnect and stale-pause flows as in rc5.
3. Open the collection menu in Portrait (`input-gate,passthrough`): its own
   View/Y behavior original; close it: input active again.
4. Return to the title menu: record the `title,…` line and `paused` state (RE-1b);
   View/Y must be original; screen must not be cropped.
5. Dawn of Sorrow, Order of Ecclesia and Haunted Castle, with the mod enabled
   beforehand in Portrait: no crop, Select held/tapped natively, View+Y inert.
6. Back to Portrait: single-screen state restored, map and marker working.
7. Afterwards: compare installed and original file hashes, and saves against the
   backup; report differences without restoring anything automatically.
