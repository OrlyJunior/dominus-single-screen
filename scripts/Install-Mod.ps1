[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$GameDirectory,
    [Parameter(Mandatory = $true)][string]$PackageDirectory,
    [string]$TestFixtureRoot,
    [ValidateRange(0, 2)][int]$TestFailAfterCopy = 0
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Verify-Game.ps1') -Library
if ($TestFailAfterCopy -and -not $TestFixtureRoot) { throw 'Failure injection is restricted to synthetic fixtures.' }
Assert-DssNoGameProcess
$root = Assert-DssGame $GameDirectory $TestFixtureRoot
$package = Get-DssFullPath $PackageDirectory
Assert-DssNoReparse $package
if (-not (Test-Path -LiteralPath $package -PathType Container)) { throw 'PackageDirectory does not exist.' }
if ($package -ieq $root -or (Test-DssWithin $package $root) -or (Test-DssWithin $root $package)) { throw 'PackageDirectory and GameDirectory must be separate, non-nested directories.' }
if ($TestFixtureRoot -and -not (Test-DssWithin $package (Get-DssFullPath $TestFixtureRoot))) { throw 'Synthetic package must be inside TestFixtureRoot.' }
Assert-DssNoConflicts $root
$payload = @($script:DssPayloadNames | ForEach-Object {
    $path = Join-Path $package $_
    Assert-DssNoReparse $path
    $item = Get-Item -LiteralPath $path -Force
    if ($item.PSIsContainer -or $item.Length -le 0) { throw "Invalid payload: $_" }
    [pscustomobject]@{ Name = $_; Length = [long]$item.Length; SHA256 = Get-DssHash $path }
})
if (-not $TestFixtureRoot) {
    # Static validation only; never LoadLibrary or execute the payload.
    $dllBytes = [IO.File]::ReadAllBytes((Join-Path $package 'd3d11.dll'))
    if ($dllBytes.Length -lt 256 -or [BitConverter]::ToUInt16($dllBytes, 0) -ne 0x5A4D) { throw 'Payload is not a PE DLL.' }
    $pe = [BitConverter]::ToInt32($dllBytes, 0x3C)
    if ($pe -lt 64 -or $pe -gt ($dllBytes.Length - 26)) { throw 'Payload PE header is invalid.' }
    if ([BitConverter]::ToUInt32($dllBytes, $pe) -ne 0x4550 -or [BitConverter]::ToUInt16($dllBytes, $pe + 4) -ne 0x8664 -or [BitConverter]::ToUInt16($dllBytes, $pe + 24) -ne 0x20B -or ([BitConverter]::ToUInt16($dllBytes, $pe + 22) -band 0x2000) -eq 0) { throw 'Payload must be a valid x64 PE32+ DLL.' }
}
$before = @(Get-DssOriginalSnapshot $root)
Assert-DssNoGameProcess
$created = [Collections.Generic.List[object]]::new()
try {
    $copied = 0
    foreach ($file in $payload) {
        Assert-DssNoGameProcess
        $source = Join-Path $package $file.Name
        if ((Get-DssHash $source) -cne $file.SHA256) { throw 'Package changed while preparing installation.' }
        $temporary = Join-Path $root ('.dominus-single-screen.' + [Guid]::NewGuid().ToString('N') + '.tmp')
        $record = [pscustomobject]@{ Path = $temporary; SHA256 = $null }
        $sourceStream = [IO.File]::Open($source, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
        try {
            Assert-DssNoReparse $temporary
            $destinationStream = [IO.File]::Open($temporary, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
            $created.Add($record)
            try { $sourceStream.CopyTo($destinationStream); $destinationStream.Flush($true) } finally { $destinationStream.Dispose() }
        } finally {
            $sourceStream.Dispose()
            if (Test-Path -LiteralPath $temporary -PathType Leaf) { $record.SHA256 = Get-DssHash $temporary }
        }
        if ($record.SHA256 -cne $file.SHA256) { throw 'Staged copy did not match the package.' }
        $destination = Join-Path $root $file.Name
        Assert-DssNoReparse $destination
        # File.Move without an overwrite overload refuses any existing destination.
        [IO.File]::Move($temporary, $destination)
        $record.Path = $destination
        $copied++
        if ($TestFailAfterCopy -eq $copied) { throw 'Synthetic failure after payload copy.' }
    }
    Assert-DssSnapshot $before @(Get-DssOriginalSnapshot $root)
    Assert-DssNoGameProcess
    $manifest = [ordered]@{ Schema = 1; Mod = 'dominus-single-screen'; GameDirectory = $root; SyntheticFixture = [bool]$TestFixtureRoot; CreatedUtc = [DateTime]::UtcNow.ToString('o'); ExpectedGameExeSHA256 = Get-DssHash (Join-Path $root 'game.exe'); InstalledFiles = $payload; Originals = $before }
    $manifestPath = Join-Path $root $script:DssManifestName
    $json = $manifest | ConvertTo-Json -Depth 8
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($json)
    $temporary = Join-Path $root ('.dominus-single-screen.' + [Guid]::NewGuid().ToString('N') + '.tmp')
    $record = [pscustomobject]@{ Path = $temporary; SHA256 = $null }
    Assert-DssNoReparse $temporary
    $manifestStream = [IO.File]::Open($temporary, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    $created.Add($record)
    try { $manifestStream.Write($bytes, 0, $bytes.Length); $manifestStream.Flush($true) } finally {
        $manifestStream.Dispose()
        $record.SHA256 = Get-DssHash $temporary
    }
    Assert-DssNoReparse $manifestPath
    [IO.File]::Move($temporary, $manifestPath)
    $record.Path = $manifestPath
    Assert-DssSnapshot $before @(Get-DssOriginalSnapshot $root)
    foreach ($file in $payload) {
        if ((Get-DssHash (Join-Path $root $file.Name)) -cne $file.SHA256) { throw 'Installed payload changed during verification.' }
    }
    [pscustomobject]@{ Result = 'Installed'; GameDirectory = $root; OriginalFilesVerified = $before.Count; AddedFiles = @($payload.Name) + $script:DssManifestName; LaunchedGame = $false }
} catch {
    $reason = $_
    $remaining = [Collections.Generic.List[string]]::new()
    $rollbackAllowed = $true
    try { Assert-DssNoGameProcess } catch { $rollbackAllowed = $false }
    for ($i = $created.Count - 1; $i -ge 0; $i--) {
        $file = $created[$i]
        try {
            if (-not $rollbackAllowed) { throw 'Game started concurrently; no rollback writes are attempted.' }
            Assert-DssNoReparse $file.Path
            if (Test-Path -LiteralPath $file.Path -PathType Leaf) {
                if (-not $file.SHA256 -or (Get-DssHash $file.Path) -cne $file.SHA256) { throw 'File changed; preserved.' }
                Remove-Item -LiteralPath $file.Path -Force
            }
        } catch { $remaining.Add($file.Path) }
    }
    if ($remaining.Count -gt 0) { Write-Warning ('Rollback preserved files it could not prove unchanged: ' + ($remaining -join ', ')) }
    throw $reason
}
