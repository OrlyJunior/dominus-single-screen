# End-user uninstaller: finds the game through Steam, then runs Uninstall-Mod.ps1,
# which removes only the files listed in the install manifest after verifying them.
[CmdletBinding()]
param([string]$GameDirectory)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Find-DominusGame.ps1')
if (-not $GameDirectory) { $GameDirectory = Find-DominusGame }
if (-not $GameDirectory) {
    throw 'Castlevania Dominus Collection was not found in your Steam libraries. Run: Uninstall.bat "C:\path\to\Castlevania Dominus Collection"'
}
Write-Host "Game folder: $GameDirectory"
& (Join-Path $PSScriptRoot 'Uninstall-Mod.ps1') -GameDirectory $GameDirectory
Write-Host 'Uninstalled. Optional: delete %LOCALAPPDATA%\DominusSingleScreen (diagnostic logs).'
