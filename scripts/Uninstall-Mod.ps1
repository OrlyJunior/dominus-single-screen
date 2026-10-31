[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$GameDirectory,
    [string]$TestFixtureRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Verify-Game.ps1') -Library
$root = Assert-DssGame $GameDirectory $TestFixtureRoot
Assert-DssNoGameProcess $root
$manifestPath = Join-Path $root $script:DssManifestName
$manifestHash = Get-DssHash $manifestPath
$manifest = Get-DssManifest $root
if ([bool]$manifest.SyntheticFixture -ne [bool]$TestFixtureRoot) { throw 'Manifest fixture mode does not match this uninstallation.' }
if ($manifest.ExpectedGameExeSHA256 -cne (Get-DssHash (Join-Path $root 'game.exe'))) { throw 'game.exe changed after installation.' }
Assert-DssSnapshot @($manifest.Originals) @(Get-DssOriginalSnapshot $root)
# Validate the complete operation before removing even the first file.
foreach ($file in $manifest.InstalledFiles) {
    Assert-DssNoGameProcess $root
    $path = Join-Path $root $file.Name
    if ((Get-DssHash $path) -cne $file.SHA256 -or (Get-Item -LiteralPath $path).Length -ne $file.Length) { throw "Mod file changed; nothing removed: $($file.Name)" }
}
Assert-DssNoGameProcess $root
foreach ($file in $manifest.InstalledFiles) {
    $path = Join-Path $root $file.Name
    Assert-DssNoReparse $path
    if ((Get-DssHash $path) -cne $file.SHA256) { throw "Mod file changed during removal: $($file.Name)" }
    Remove-Item -LiteralPath $path -Force
}
Assert-DssSnapshot @($manifest.Originals) @(Get-DssOriginalSnapshot $root)
if ((Get-DssHash $manifestPath) -cne $manifestHash) { throw 'Manifest changed; it was preserved.' }
Assert-DssNoGameProcess $root
Remove-Item -LiteralPath $manifestPath -Force
[pscustomobject]@{ Result = 'Uninstalled'; GameDirectory = $root; OriginalFilesVerified = @($manifest.Originals).Count; LaunchedGame = $false }
