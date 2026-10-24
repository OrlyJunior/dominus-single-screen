# Dominus: Single Screen — 0.1.0

Portrait of Ruin on Castlevania Dominus Collection, Steam/Windows x64 only.
Based on the validated internal rc5 implementation. Other titles/layouts remain unsupported.

Close the game. Extract this locally built package outside the game folder and
run Install.bat, optionally passing your game folder as an argument. The script
checks the supported fingerprint and conflicts, refuses overwrites and records
a manifest. Uninstall.bat validates that manifest and removes only its own files.
Do not combine this d3d11.dll proxy with other graphics wrappers. Original game
files and saves are not replaced; keep independent progress backups.

The mod starts off: View+Y toggles single-screen mode, View opens/closes the map
with native pause. With the mod off, View is forwarded as Select on release.
Gameplay is 4:3 with point sampling and black bars. Native marker replay requires
a full capture at the current position (~650 ms); opening while moving or before
capture completes keeps the native marker frozen. Valid cycles persist across
validated stationary reopenings. Keep the mod off in unsupported games.

Validated layout: gameplay left, map/Status stacked right, 16:9 at 1920x1080 or
2560x1440. Keyboard shortcuts and filtering a second controller are unsupported.
Logs accumulate under %LOCALAPPDATA%/DominusSingleScreen. Marker traces are off.
Do not edit installed payload files without accounting for manifest hash checks.

MIT for project-owned material (LICENSE.txt); third-party terms in
THIRD-PARTY-NOTICES.md and licenses/. No Konami/M2 assets are redistributed.
This is an unofficial project, not affiliated with Konami or M2.
