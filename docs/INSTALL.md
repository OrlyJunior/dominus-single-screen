# Installation and removal

Source checkout and compilation do not modify an installed mod. Build and review
a separate package first; see [BUILD.md](BUILD.md). No binary is supplied by this
repository. Back up progress independently before any later gameplay tests.
These instructions do not authorize replacing another local installation.

Close the game before installation/removal. Use normal user permissions; the
scripts do not request elevation, run Steam, restore saves, or kill processes.
Every `game.exe` is judged by its full image path and file identity: another
installation such as Castlevania Advance Collection may stay open, while this
game, a process whose path or identity cannot be read, or a failed process
enumeration refuses the operation.
Set `$gameDirectory` to your actual Steam game folder and `$packageDirectory` to
a separate directory containing your built `d3d11.dll` and the release INI.

```powershell
& ./scripts/Verify-Game.ps1 -GameDirectory $gameDirectory
& ./scripts/Install-Mod.ps1 -GameDirectory $gameDirectory -PackageDirectory $packageDirectory
& ./scripts/Verify-Game.ps1 -GameDirectory $gameDirectory
```

Installation adds only `d3d11.dll`, `dominus-single-screen.ini`, and
`dominus-single-screen.manifest.json`. It checks the executable fingerprint,
conflicting wrappers, reparse/nested paths and existing destination files;
hashes original game files before/after; copies without overwrite; and records
an integrity manifest. On failure it removes only unchanged files it created.
It does not alter original executable/DLL/data files or user progress.

Launch normally from Steam only when you choose to play. Use Portrait of Ruin,
the supported layout and View + Y to activate. Experimental Dawn of Sorrow
support needs `EnableExperimentalDawn=1` in the package INI before installing.
Other titles remain unsupported.

To remove a script-installed mod, with the game closed:

```powershell
& ./scripts/Uninstall-Mod.ps1 -GameDirectory $gameDirectory
```

Removal verifies payload/original hashes and removes only the files listed in
the manifest. It refuses mismatches instead of silently deleting changed files.
Do not bypass that refusal or overwrite a manifest when changing versions.
Uninstall the previous verified package, then install the new separate package.
The manifest is a local integrity baseline, not an authenticity signature.

Runtime logs live separately under `%LOCALAPPDATA%/DominusSingleScreen` (possibly
virtualized by the launching environment). No save or Steam Cloud recovery is
performed. Detailed marker dumps are disabled by default. If you change settings,
do so in the separate package before installing; editing installed files causes
the integrity checks to report a payload mismatch.

The optional generated end-user package contains Install.bat/Uninstall.bat and
Steam-library discovery helpers. Those launchers are source templates here;
they are usable only after packaging, with their expected companion scripts.
