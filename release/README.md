# Dominus: Single Screen — 0.2.0

Portrait of Ruin on Castlevania Dominus Collection, Steam/Windows x64 only, with
experimental, opt-in Dawn of Sorrow support (below). Other titles/layouts remain
unsupported; the mod stays inert in them.

Close the game. Extract this locally built package outside the game folder and
run Install.bat, optionally passing your game folder as an argument. The script
checks the supported fingerprint and conflicts, refuses overwrites and records
a manifest. Uninstall.bat validates that manifest and removes only its own files.
Updating: run the installed version's Uninstall.bat (or this one), then
Install.bat. Settings belong in this package's INI before installing.
Do not combine this d3d11.dll proxy with other graphics wrappers. Original game
files and saves are not replaced; keep independent progress backups.

The mod starts off: View+Y toggles single-screen mode, View opens/closes the map
with native pause. With the mod off, View is forwarded as Select on release.
Gameplay is 4:3 with point sampling and black bars. Native marker replay requires
a full capture at the current position (~650 ms); opening while moving or before
capture completes keeps the native marker frozen. Valid cycles persist across
validated stationary reopenings.

Since 0.1.1 the mod acts only while Portrait of Ruin is the verified active game:
Dawn of Sorrow, Order of Ecclesia, Haunted Castle and the collection's menus keep
their original input and layout. Inside Portrait the enlarged map opens only in
gameplay; on the title screen and menus View stays native Select. Extra modes
(Sisters, Richter, ...) and Boss Rush keep View as Select.

Dawn of Sorrow (experimental): set `EnableExperimentalDawn=1` in this package's
dominus-single-screen.ini before running Install.bat. Then, with the mod on, the
main screen appears alone in gameplay, the title and pause menus, the warp rooms'
destination choice and the merchants' menus; View opens the enlarged map with
native pause where the map is live (LB/RB: Status) and stays Select elsewhere;
the marker pulses as in Portrait; mouse and touch positions follow the enlarged
image. The top screen is hidden meanwhile (View+Y shows the collection layout).
Sequences, game over and other modes keep the original layout. Validated by the
author only; report anything that looks wrong.

Validated layout: gameplay left, map/Status stacked right, 16:9 at 1920x1080 or
2560x1440. Keyboard shortcuts and filtering a second controller are unsupported.
Logs accumulate under %LOCALAPPDATA%/DominusSingleScreen. Marker traces are off.
Do not edit installed payload files without accounting for manifest hash checks.

MIT for project-owned material (LICENSE.txt); third-party terms in
THIRD-PARTY-NOTICES.md and licenses/. No Konami/M2 assets are redistributed.
This is an unofficial project, not affiliated with Konami or M2.
