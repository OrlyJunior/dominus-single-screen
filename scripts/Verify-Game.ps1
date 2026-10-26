[CmdletBinding(DefaultParameterSetName = 'Verify')]
param(
    [Parameter(Mandatory = $true, ParameterSetName = 'Verify')][Alias('GameDirectory')][string]$VerifyGameDirectory,
    [Parameter(Mandatory = $true, ParameterSetName = 'Library')][switch]$Library,
    [Alias('TestFixtureRoot')][string]$VerifyTestFixtureRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$script:DssExpectedExeHash = 'E6FA885F560C8A47640FF8DF4DE3CE4A938C8053EB11A90A7B65F5EBF1AB6B64'
$script:DssManifestName = 'dominus-single-screen.manifest.json'
$script:DssPayloadNames = @('d3d11.dll', 'dominus-single-screen.ini')
$script:DssFixtureMarker = 'DSS-SYNTHETIC-FIXTURE-v1-NOT-A-GAME'
$script:DssAllowedWorkRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\out\installer-tests'))

function Get-DssFullPath([string]$Path) {
    return [IO.Path]::GetFullPath($Path).TrimEnd([char[]]@('\', '/'))
}

function Test-DssWithin([string]$Path, [string]$Root) {
    $full = Get-DssFullPath $Path
    $base = Get-DssFullPath $Root
    return $full.StartsWith($base + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)
}

function Assert-DssNoReparse([string]$Path) {
    $itemPath = [IO.Path]::GetFullPath($Path)
    while ($itemPath) {
        if (Test-Path -LiteralPath $itemPath) {
            $item = Get-Item -LiteralPath $itemPath -Force
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Reparse points/symlinks are not accepted: $itemPath"
            }
        }
        $parent = [IO.Path]::GetDirectoryName($itemPath)
        if ($parent -eq $itemPath) { break }
        $itemPath = $parent
    }
}

if (-not ('DssFileIdentity' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class DssFileIdentity {
    [StructLayout(LayoutKind.Sequential)]
    struct Info {
        public uint Attributes; public long Created, Accessed, Written;
        public uint Volume, SizeHigh, SizeLow, Links, IndexHigh, IndexLow;
    }
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetFileInformationByHandle(SafeFileHandle file, out Info info);
    // Volume serial and file index: equal for every path (hard link included) of one file.
    public static string Of(string path) {
        using (var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete)) {
            Info info;
            if (!GetFileInformationByHandle(stream.SafeFileHandle, out info)) return null;
            return string.Format("{0:X8}-{1:X8}{2:X8}", info.Volume, info.IndexHigh, info.IndexLow);
        }
    }
}
'@
}

# Identity of an existing regular file reached without reparse points; $null otherwise.
function Get-DssFileIdentity([string]$Path) {
    try {
        if (-not $Path) { return $null }
        Assert-DssNoReparse $Path
        if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
        return [DssFileIdentity]::Of([IO.Path]::GetFullPath($Path))
    } catch { return $null }
}

# Two independent reports of a process image must agree; anything else is unidentified.
function Merge-DssProcessPath([string]$ProcessPath, [string]$CimPath) {
    $paths = @(@($ProcessPath, $CimPath) | Where-Object { $_ } | ForEach-Object {
        try { [IO.Path]::GetFullPath($_) } catch { '?' }
    })
    if ($paths.Count -eq 0 -or $paths -contains '?') { return $null }
    if ($paths.Count -eq 2 -and $paths[0] -ine $paths[1]) { return $null }
    return $paths[0]
}

# Every process named game.exe (Dominus and Advance Collection both use that name)
# plus any process whose image is the target, with its image path and file identity.
# Only process names and image paths are read: never memory, modules, handles or windows.
function Get-DssGameProcessRecords([string]$TargetExe) {
    $byId = @{}
    foreach ($process in @(Get-Process -Name 'game' -ErrorAction SilentlyContinue)) {
        $path = $null
        try { $path = $process.Path } catch { $path = $null }
        $byId[[int]$process.Id] = [pscustomobject]@{ Id = [int]$process.Id; ProcessPath = $path; CimPath = $null }
    }
    $cim = @(Get-CimInstance -ClassName Win32_Process -ErrorAction Stop)
    foreach ($entry in $cim) {
        $id = [int]$entry.ProcessId
        $image = [string]$entry.ExecutablePath
        $named = [string]$entry.Name -ieq 'game.exe'
        $target = $TargetExe -and $image -and ($image -ieq $TargetExe)
        if (-not ($named -or $target -or $byId.ContainsKey($id))) { continue }
        if (-not $byId.ContainsKey($id)) { $byId[$id] = [pscustomobject]@{ Id = $id; ProcessPath = $null; CimPath = $null } }
        $byId[$id].CimPath = $image
    }
    foreach ($record in $byId.Values) {
        $path = Merge-DssProcessPath $record.ProcessPath $record.CimPath
        [pscustomobject]@{ Id = $record.Id; Path = $path; Identity = (Get-DssFileIdentity $path) }
    }
}

# Pure decision: the reasons any record blocks installation/removal into GameRoot.
# A process is allowed only when its image is known, readable, outside GameRoot
# and provably a different file from GameRoot's game.exe.
function Get-DssProcessBlockers([object[]]$Records, [string]$GameRoot, [string]$TargetIdentity) {
    $reasons = [Collections.Generic.List[string]]::new()
    foreach ($record in @($Records)) {
        if (-not $record) { continue }
        if (-not $GameRoot) { $reasons.Add("pid $($record.Id): game.exe is running"); continue }
        $root = Get-DssFullPath $GameRoot
        $target = Join-Path $root 'game.exe'
        if (-not $record.Path) { $reasons.Add("pid $($record.Id): image path unavailable or inconsistent"); continue }
        $path = Get-DssFullPath $record.Path
        if ($path -ieq $target) { $reasons.Add("pid $($record.Id): this game is running"); continue }
        if (Test-DssWithin $path $root) { $reasons.Add("pid $($record.Id): runs from this game directory"); continue }
        if (-not $TargetIdentity -or -not $record.Identity) { $reasons.Add("pid $($record.Id): file identity unavailable"); continue }
        if ($record.Identity -ceq $TargetIdentity) { $reasons.Add("pid $($record.Id): same file as this game.exe"); continue }
    }
    return $reasons.ToArray()
}

# Without GameRoot any game.exe blocks (the v0.1.1 rule). With it, another,
# unambiguously identified installation (e.g. Castlevania Advance Collection)
# may stay open; this game, or any process that cannot be identified, blocks.
function Assert-DssNoGameProcess([string]$GameRoot) {
    $target = if ($GameRoot) { Join-Path (Get-DssFullPath $GameRoot) 'game.exe' } else { $null }
    try { $records = @(Get-DssGameProcessRecords $target) } catch {
        throw 'Process enumeration failed. Installation/removal is refused; try again later.'
    }
    $identity = if ($target) { Get-DssFileIdentity $target } else { $null }
    $blockers = @(Get-DssProcessBlockers $records $GameRoot $identity)
    if ($blockers.Count -ne 0) {
        throw ('A game.exe process is active (' + ($blockers -join '; ') + '). Installation/removal is refused; close it yourself later.')
    }
}

function Get-DssHash([string]$Path) {
    Assert-DssNoReparse $Path
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToUpperInvariant()
}

function Get-DssFixtureHash([string]$GameRoot, [string]$FixtureRoot) {
    if (-not $FixtureRoot) { return $null }
    $fixture = Get-DssFullPath $FixtureRoot
    Assert-DssNoReparse $fixture
    if (-not (Test-DssWithin $fixture $script:DssAllowedWorkRoot)) {
        throw "Synthetic fixtures must be inside $script:DssAllowedWorkRoot"
    }
    if (-not (Test-DssWithin $GameRoot $fixture)) { throw 'Fixture GameDirectory must be a descendant of TestFixtureRoot.' }
    $marker = Join-Path $fixture '.dss-fixture'
    Assert-DssNoReparse $marker
    if (-not (Test-Path -LiteralPath $marker -PathType Leaf)) { throw 'Synthetic fixture marker is missing.' }
    if ([IO.File]::ReadAllText($marker) -cne $script:DssFixtureMarker) { throw 'Synthetic fixture marker is invalid.' }
    # A real Steam installation cannot accidentally become a fixture.
    if ($GameRoot -match '(?i)[\\/]steamapps[\\/]' -or (Test-Path -LiteralPath (Join-Path $GameRoot 'steam_api64.dll'))) {
        throw 'Steam paths and steam_api64.dll are forbidden in synthetic fixture mode.'
    }
    return (Get-DssHash (Join-Path $GameRoot 'game.exe'))
}

function Assert-DssGame([string]$Directory, [string]$FixtureRoot) {
    $root = Get-DssFullPath $Directory
    Assert-DssNoReparse $root
    if (-not (Test-Path -LiteralPath $root -PathType Container)) { throw "GameDirectory does not exist: $root" }
    $exe = Join-Path $root 'game.exe'
    if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { throw 'game.exe is missing.' }
    $data = Join-Path $root 'windata'
    if (-not (Test-Path -LiteralPath $data -PathType Container)) { throw 'windata directory is missing.' }
    Assert-DssNoReparse $data
    $fixtureHash = Get-DssFixtureHash $root $FixtureRoot
    $expected = $script:DssExpectedExeHash
    if ($fixtureHash) { $expected = $fixtureHash }
    $actual = Get-DssHash $exe
    if ($actual -cne $expected) { throw "Unsupported game.exe fingerprint: $actual" }
    return $root
}

function Assert-DssNoConflicts([string]$Root) {
    $proxies = @('d3d11.dll', 'dxgi.dll', 'dinput8.dll', 'd3d9.dll', 'opengl32.dll', 'version.dll', 'winmm.dll', 'dsound.dll', 'xinput1_3.dll', 'xinput1_4.dll', 'xinput9_1_0.dll', 'winhttp.dll', 'ReShade.ini', 'ReShadePreset.ini', 'reshade-shaders', 'dxvk.conf', 'd3dx.ini', 'd3d11_log.txt', 'MGSM2Fix.ini', 'MGSM2Fix.asi', 'dominus-single-screen.ini', $script:DssManifestName)
    foreach ($name in $proxies) {
        if (Test-Path -LiteralPath (Join-Path $Root $name)) { throw "Conflicting mod/proxy or existing installation: $name" }
    }
    $asi = @(Get-ChildItem -LiteralPath $Root -File -Force | Where-Object { $_.Extension -ieq '.asi' })
    if ($asi.Count -ne 0) { throw 'An ASI mod is present; compatibility has not been established.' }
}

function Get-DssOriginalSnapshot([string]$Root) {
    $entries = [Collections.Generic.List[object]]::new()
    $files = @(Get-ChildItem -LiteralPath $Root -File -Force | Where-Object {
        ($_.Extension -ieq '.exe' -or $_.Extension -ieq '.dll') -and $_.Name -ine 'd3d11.dll'
    })
    # Walk explicitly so junctions are rejected before any descent.
    $pending = [Collections.Generic.Stack[string]]::new()
    $pending.Push((Join-Path $Root 'windata'))
    while ($pending.Count -gt 0) {
        $dir = $pending.Pop()
        Assert-DssNoReparse $dir
        foreach ($entry in Get-ChildItem -LiteralPath $dir -Force) {
            Assert-DssNoReparse $entry.FullName
            if ($entry.PSIsContainer) { $pending.Push($entry.FullName) } else { $files += $entry }
        }
    }
    foreach ($file in ($files | Sort-Object FullName)) {
        Assert-DssNoReparse $file.FullName
        $relative = $file.FullName.Substring($Root.Length + 1).Replace('\', '/')
        $entries.Add([pscustomobject]@{ Path = $relative; Length = [long]$file.Length; SHA256 = Get-DssHash $file.FullName })
    }
    return $entries.ToArray()
}

function Assert-DssSnapshot([object[]]$Expected, [object[]]$Actual) {
    $before = @($Expected | Sort-Object Path)
    $after = @($Actual | Sort-Object Path)
    if ($before.Count -eq 0 -or $before.Count -ne $after.Count) { throw 'Original file inventory changed or is invalid.' }
    for ($i = 0; $i -lt $before.Count; $i++) {
        $a = $before[$i]; $b = $after[$i]
        if ($a.Path -cne $b.Path -or [long]$a.Length -ne [long]$b.Length -or $a.SHA256 -cne $b.SHA256) {
            throw "Original file integrity mismatch: $($a.Path)"
        }
    }
}

function Write-DssNewFile([string]$Path, [byte[]]$Bytes) {
    Assert-DssNoReparse $Path
    $stream = [IO.File]::Open($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try { $stream.Write($Bytes, 0, $Bytes.Length); $stream.Flush($true) } finally { $stream.Dispose() }
}

function Get-DssManifest([string]$Root) {
    $path = Join-Path $Root $script:DssManifestName
    Assert-DssNoReparse $path
    $item = Get-Item -LiteralPath $path -Force
    if ($item.Length -gt 16MB) { throw 'Manifest is unreasonably large.' }
    $manifest = [IO.File]::ReadAllText($path) | ConvertFrom-Json
    if ($manifest.Schema -ne 1 -or $manifest.Mod -cne 'dominus-single-screen') { throw 'Unknown manifest format.' }
    if ((Get-DssFullPath $manifest.GameDirectory) -ine $Root) { throw 'Manifest belongs to a different game directory.' }
    $installed = @($manifest.InstalledFiles)
    if ($installed.Count -ne 2) { throw 'Manifest must contain exactly the two known mod files.' }
    $seen = @{}
    foreach ($file in $installed) {
        if ($script:DssPayloadNames -cnotcontains $file.Name -or $seen.ContainsKey($file.Name)) { throw 'Unsafe, duplicate or unknown manifest file name.' }
        if ($file.SHA256 -cnotmatch '^[0-9A-F]{64}$' -or [long]$file.Length -le 0) { throw 'Invalid installed-file hash/length.' }
        $seen[$file.Name] = $true
    }
    $baseline = @($manifest.Originals)
    $seen.Clear()
    foreach ($file in $baseline) {
        $relative = [string]$file.Path
        if ($relative -match '(^|/)\.\.?(/|$)' -or $relative -match '[:\\]' -or $relative.StartsWith('/') -or $seen.ContainsKey($relative)) { throw 'Unsafe original manifest path.' }
        if ($relative -cnotmatch '^(windata/[^\r\n]+|[^/]+\.(exe|dll))$' -or $relative -ieq 'd3d11.dll') { throw 'Manifest contains a non-original path.' }
        if ($file.SHA256 -cnotmatch '^[0-9A-F]{64}$' -or [long]$file.Length -lt 0) { throw 'Invalid original-file hash/length.' }
        $seen[$relative] = $true
    }
    if (-not $seen.ContainsKey('game.exe')) { throw 'Manifest lacks game.exe baseline.' }
    return $manifest
}

if ($Library) { return }
$root = Assert-DssGame $VerifyGameDirectory $VerifyTestFixtureRoot
$snapshot = @(Get-DssOriginalSnapshot $root)
$manifestPath = Join-Path $root $script:DssManifestName
$installed = Test-Path -LiteralPath $manifestPath
if ($installed) {
    $manifest = Get-DssManifest $root
    if ([bool]$manifest.SyntheticFixture -ne [bool]$VerifyTestFixtureRoot) { throw 'Manifest fixture mode does not match this verification.' }
    Assert-DssSnapshot @($manifest.Originals) $snapshot
    foreach ($file in $manifest.InstalledFiles) {
        $path = Join-Path $root $file.Name
        if ((Get-DssHash $path) -cne $file.SHA256 -or (Get-Item -LiteralPath $path).Length -ne $file.Length) { throw "Installed mod file changed: $($file.Name)" }
    }
}
[pscustomobject]@{ GameDirectory = $root; SupportedExe = $true; OriginalFilesHashed = $snapshot.Count; InstalledManifest = $installed; IntegrityVerifiedAgainstManifest = $installed; SHA256 = Get-DssHash (Join-Path $root 'game.exe') }
