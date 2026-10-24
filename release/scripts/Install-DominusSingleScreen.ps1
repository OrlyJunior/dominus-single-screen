# End-user installer: finds the game through Steam, then runs the verified,
# non-overwriting Install-Mod.ps1 with this package. Never starts the game.
[CmdletBinding()]
param([string]$GameDirectory)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Find-DominusGame.ps1')
$package = Split-Path -Parent $PSScriptRoot
if (-not $GameDirectory) { $GameDirectory = Find-DominusGame }
if (-not $GameDirectory) {
    throw 'Castlevania Dominus Collection was not found in your Steam libraries. Run: Install.bat "C:\path\to\Castlevania Dominus Collection"'
}
Write-Host "Game folder: $GameDirectory"
Write-Host 'Checking game version, conflicts and original files (may take a minute)...'
& (Join-Path $PSScriptRoot 'Install-Mod.ps1') -GameDirectory $GameDirectory -PackageDirectory $package
Write-Host 'Installed. Start Portrait of Ruin from Steam; press View+Y to switch to single screen.'
