[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Verify-Game.ps1') -Library
$testRoot = Join-Path $script:DssAllowedWorkRoot ('installer-tests-' + [Guid]::NewGuid().ToString('N'))
Assert-DssNoReparse $testRoot
[void][IO.Directory]::CreateDirectory($testRoot)
$passed = [Collections.Generic.List[string]]::new()

function New-DssFixture([string]$Name) {
    $fixture = Join-Path $testRoot $Name
    $game = Join-Path $fixture 'game'
    $package = Join-Path $fixture 'package'
    [void][IO.Directory]::CreateDirectory((Join-Path $game 'windata'))
    [void][IO.Directory]::CreateDirectory($package)
    Write-DssNewFile (Join-Path $fixture '.dss-fixture') ([Text.Encoding]::ASCII.GetBytes($script:DssFixtureMarker))
    foreach ($entry in @(
        @((Join-Path $game 'game.exe'), 'FAKE-PE-DO-NOT-EXECUTE'),
        @((Join-Path $game 'dra02.dll'), 'ORIGINAL-GAME-DLL'),
        @((Join-Path $game 'windata\alldata.bin'), 'ORIGINAL-GAME-DATA'),
        @((Join-Path $package 'd3d11.dll'), 'SYNTHETIC-PROXY-DO-NOT-EXECUTE'),
        @((Join-Path $package 'dominus-single-screen.ini'), '[General] Enabled=false')
    )) { Write-DssNewFile $entry[0] ([Text.Encoding]::ASCII.GetBytes($entry[1])) }
    return [pscustomobject]@{ Root = $fixture; Game = $game; Package = $package }
}

function Assert-DssTest([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "TEST FAILED: $Message" }
}

function Assert-DssRejected([scriptblock]$Action, [string]$MessagePattern) {
    $rejected = $false
    try { & $Action | Out-Null } catch {
        $rejected = $true
        if ($_.Exception.Message -notmatch $MessagePattern) { throw "Wrong rejection: $($_.Exception.Message)" }
    }
    Assert-DssTest $rejected 'Unsafe operation was unexpectedly accepted.'
}

function Install-DssFixture($Fixture, [int]$Failure = 0) {
    & (Join-Path $PSScriptRoot 'Install-Mod.ps1') -GameDirectory $Fixture.Game -PackageDirectory $Fixture.Package -TestFixtureRoot $Fixture.Root -TestFailAfterCopy $Failure | Out-Null
}

function Uninstall-DssFixture($Fixture) {
    & (Join-Path $PSScriptRoot 'Uninstall-Mod.ps1') -GameDirectory $Fixture.Game -TestFixtureRoot $Fixture.Root | Out-Null
}

function Write-DssTestManifest($Fixture, $Manifest) {
    # Intentional corruption exists only in a marked synthetic fixture.
    [IO.File]::WriteAllText((Join-Path $Fixture.Game $script:DssManifestName), ($Manifest | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
}

$f = New-DssFixture 'roundtrip'
$before = @(Get-DssOriginalSnapshot $f.Game)
Install-DssFixture $f
& (Join-Path $PSScriptRoot 'Verify-Game.ps1') -GameDirectory $f.Game -TestFixtureRoot $f.Root | Out-Null
Assert-DssTest (Test-Path -LiteralPath (Join-Path $f.Game $script:DssManifestName)) 'Manifest absent after install.'
Assert-DssSnapshot $before @(Get-DssOriginalSnapshot $f.Game)
Uninstall-DssFixture $f
Assert-DssTest (-not (Test-Path -LiteralPath (Join-Path $f.Game 'd3d11.dll'))) 'Proxy survived successful uninstall.'
Assert-DssTest (-not (Test-Path -LiteralPath (Join-Path $f.Game $script:DssManifestName))) 'Manifest survived successful uninstall.'
Assert-DssSnapshot $before @(Get-DssOriginalSnapshot $f.Game)
$passed.Add('Install / verify / uninstall preserve all original hashes')

$f = New-DssFixture 'conflict'
Write-DssNewFile (Join-Path $f.Game 'dxgi.dll') ([byte[]]@(1, 2, 3))
$hash = Get-DssHash (Join-Path $f.Game 'dxgi.dll')
Assert-DssRejected { Install-DssFixture $f } 'Conflicting'
Assert-DssTest ((Get-DssHash (Join-Path $f.Game 'dxgi.dll')) -ceq $hash) 'Conflicting proxy changed.'
Assert-DssTest (-not (Test-Path -LiteralPath (Join-Path $f.Game 'd3d11.dll'))) 'Install wrote despite conflict.'
$passed.Add('Existing graphics wrapper is refused without overwrite')

$f = New-DssFixture 'existing-config'
Write-DssNewFile (Join-Path $f.Game 'dominus-single-screen.ini') ([byte[]]@(7, 8, 9))
Assert-DssRejected { Install-DssFixture $f } 'Conflicting'
Assert-DssTest ((Get-Item -LiteralPath (Join-Path $f.Game 'dominus-single-screen.ini')).Length -eq 3) 'Existing config changed.'
$passed.Add('Pre-existing mod config is never overwritten')

$f = New-DssFixture 'tampered-mod'
Install-DssFixture $f
[IO.File]::AppendAllText((Join-Path $f.Game 'd3d11.dll'), 'TAMPER')
Assert-DssRejected { Uninstall-DssFixture $f } 'Mod file changed'
Assert-DssTest (Test-Path -LiteralPath (Join-Path $f.Game 'dominus-single-screen.ini')) 'Uninstaller partially removed a known-tampered installation.'
Assert-DssTest (Test-Path -LiteralPath (Join-Path $f.Game $script:DssManifestName)) 'Tampered install manifest removed.'
$passed.Add('Tampered mod hash blocks all removal')

$f = New-DssFixture 'changed-original'
Install-DssFixture $f
[IO.File]::AppendAllText((Join-Path $f.Game 'windata\alldata.bin'), 'CHANGE')
Assert-DssRejected { Uninstall-DssFixture $f } 'integrity mismatch'
Assert-DssTest (Test-Path -LiteralPath (Join-Path $f.Game 'd3d11.dll')) 'Uninstaller removed files after original mismatch.'
$passed.Add('Original data modification is detected before removal')

$f = New-DssFixture 'traversal'
Install-DssFixture $f
$victim = Join-Path $f.Root 'outside.txt'
Write-DssNewFile $victim ([Text.Encoding]::ASCII.GetBytes('KEEP-ME'))
$manifest = Get-DssManifest $f.Game
$manifest.InstalledFiles[0].Name = '../outside.txt'
Write-DssTestManifest $f $manifest
Assert-DssRejected { Uninstall-DssFixture $f } 'Unsafe, duplicate or unknown'
Assert-DssTest ([IO.File]::ReadAllText($victim) -ceq 'KEEP-ME') 'Traversal touched a foreign file.'
Assert-DssTest (Test-Path -LiteralPath (Join-Path $f.Game 'dominus-single-screen.ini')) 'Traversal caused partial removal.'
$passed.Add('Traversal manifest is rejected before touching any file')

$f = New-DssFixture 'baseline-traversal'
Install-DssFixture $f
$manifest = Get-DssManifest $f.Game
$manifest.Originals[0].Path = '../outside.txt'
Write-DssTestManifest $f $manifest
Assert-DssRejected { Uninstall-DssFixture $f } 'Unsafe original manifest path'
$passed.Add('Traversal inside original baseline is rejected')

$f = New-DssFixture 'rollback'
$before = @(Get-DssOriginalSnapshot $f.Game)
Assert-DssRejected { Install-DssFixture $f 1 } 'Synthetic failure'
Assert-DssSnapshot $before @(Get-DssOriginalSnapshot $f.Game)
Assert-DssTest (-not (Test-Path -LiteralPath (Join-Path $f.Game 'd3d11.dll'))) 'Rollback left proxy.'
Assert-DssTest (@(Get-ChildItem -LiteralPath $f.Game -Force -Filter '.dominus-single-screen.*.tmp').Count -eq 0) 'Rollback left staging temp.'
Assert-DssTest (-not (Test-Path -LiteralPath (Join-Path $f.Game $script:DssManifestName))) 'Rollback left manifest.'
$passed.Add('Injected post-copy failure rolls back only new mod files')

$f = New-DssFixture 'fingerprint'
Assert-DssRejected { & (Join-Path $PSScriptRoot 'Install-Mod.ps1') -GameDirectory $f.Game -PackageDirectory $f.Package } 'Unsupported game.exe fingerprint'
Assert-DssRejected { & (Join-Path $PSScriptRoot 'Install-Mod.ps1') -GameDirectory $f.Game -PackageDirectory $f.Package -TestFixtureRoot $testRoot } 'marker is missing'
Assert-DssRejected { & (Join-Path $PSScriptRoot 'Install-Mod.ps1') -GameDirectory $f.Game -PackageDirectory $f.Package -TestFixtureRoot (Get-DssFullPath (Join-Path $PSScriptRoot '..')) } 'Synthetic fixtures must be inside'
Assert-DssRejected { & (Join-Path $PSScriptRoot 'Install-Mod.ps1') -GameDirectory $f.Game -PackageDirectory $f.Package -TestFailAfterCopy 1 } 'restricted to synthetic'
$passed.Add('Fixture fingerprint bypass and failure injection require explicit marker gate')

$f = New-DssFixture 'idempotence'
Install-DssFixture $f
$hash = Get-DssHash (Join-Path $f.Game $script:DssManifestName)
Assert-DssRejected { Install-DssFixture $f } 'Conflicting'
Assert-DssTest ((Get-DssHash (Join-Path $f.Game $script:DssManifestName)) -ceq $hash) 'Repeated installation overwrote manifest.'
Uninstall-DssFixture $f
$passed.Add('Repeated installation is rejected without changing existing installation')

$f = New-DssFixture 'unrelated-files'
Install-DssFixture $f
$foreign = Join-Path $f.Game 'unrelated-tool.log'
Write-DssNewFile $foreign ([Text.Encoding]::ASCII.GetBytes('NOT-OWNED-BY-MOD'))
Uninstall-DssFixture $f
Assert-DssTest ([IO.File]::ReadAllText($foreign) -ceq 'NOT-OWNED-BY-MOD') 'Uninstaller touched an unrelated file.'
$passed.Add('Unrelated files survive removal unchanged')

$f = New-DssFixture 'process-identity'
$target = Join-Path $f.Game 'game.exe'
$other = Join-Path $f.Root 'other-collection\game.exe'
[void][IO.Directory]::CreateDirectory((Split-Path -Parent $other))
Write-DssNewFile $other ([Text.Encoding]::ASCII.GetBytes('ANOTHER-INSTALLATION'))
$link = Join-Path $f.Root 'other-collection\linked-game.exe'
New-Item -ItemType HardLink -Path $link -Target $target | Out-Null
$targetId = Get-DssFileIdentity $target
Assert-DssTest ($targetId -and (Get-DssFileIdentity $other) -and $targetId -cne (Get-DssFileIdentity $other)) 'Distinct files must have distinct identities.'
Assert-DssTest ((Get-DssFileIdentity $link) -ceq $targetId) 'A hard link must share the identity of its file.'
Assert-DssTest (-not (Get-DssFileIdentity (Join-Path $f.Root 'missing.exe'))) 'A missing file has no identity.'
$passed.Add('File identity tells another installation from the same file under another path')

function New-DssRecord([int]$Id, [string]$Path) { [pscustomobject]@{ Id = $Id; Path = $Path; Identity = (Get-DssFileIdentity $Path) } }
$block = { param($records, $root, $id) @(Get-DssProcessBlockers $records $root $id) }
Assert-DssTest (@(& $block @() $f.Game $targetId).Count -eq 0) 'No process must not block.'
Assert-DssTest (@(& $block @((New-DssRecord 1 $other)) $f.Game $targetId).Count -eq 0) 'Another identified installation must be allowed.'
Assert-DssTest (@(& $block @((New-DssRecord 2 $target.ToUpperInvariant())) $f.Game $targetId)[0] -match 'this game is running') 'The target game must block, whatever the case.'
Assert-DssTest (@(& $block @([pscustomobject]@{ Id = 3; Path = $null; Identity = $null }) $f.Game $targetId)[0] -match 'unavailable or inconsistent') 'An unidentified process must block.'
Assert-DssTest (@(& $block @((New-DssRecord 4 $link)) $f.Game $targetId)[0] -match 'same file') 'The same file through a hard link must block.'
Assert-DssTest (@(& $block @([pscustomobject]@{ Id = 5; Path = $other; Identity = $null }) $f.Game $targetId)[0] -match 'identity unavailable') 'An unreadable identity must block.'
Assert-DssTest (@(& $block @((New-DssRecord 6 $other)) $f.Game $null)[0] -match 'identity unavailable') 'An unreadable target identity must block.'
Assert-DssTest (@(& $block @((New-DssRecord 7 (Join-Path $f.Game 'windata\game.exe'))) $f.Game $targetId)[0] -match 'this game directory') 'Any image inside the game directory must block.'
Assert-DssTest (@(& $block @((New-DssRecord 8 $other)) $null $null)[0] -match 'is running') 'Without a game directory any game.exe must block (v0.1.1 rule).'
$mixed = @((New-DssRecord 9 $other), [pscustomobject]@{ Id = 10; Path = $null; Identity = $null })
Assert-DssTest (@(& $block $mixed $f.Game $targetId).Count -eq 1) 'One unidentified process among allowed ones must still block.'
Assert-DssTest ((Merge-DssProcessPath $other $other.ToUpperInvariant()) -ieq $other) 'Agreeing reports must merge.'
Assert-DssTest (-not (Merge-DssProcessPath $other $target)) 'Disagreeing reports must be unidentified.'
Assert-DssTest (-not (Merge-DssProcessPath $null $null)) 'No report must be unidentified.'
Assert-DssTest ((Merge-DssProcessPath $null $other) -ieq $other) 'A single report is used.'
$passed.Add('Process check allows only an unambiguous other installation')

# The running system (any open Castlevania Advance Collection included) is
# only enumerated and judged against a synthetic target; nothing is touched.
$live = @(Get-DssGameProcessRecords $target)
$liveBlockers = @(Get-DssProcessBlockers $live $f.Game $targetId)
$liveSummary = @($live | ForEach-Object { "pid $($_.Id): $($_.Path)" })
$passed.Add('Live enumeration judged against a synthetic target')

# Tests retain their small synthetic fixtures for inspection; no recursive deletion is used.
[pscustomobject]@{ Result = 'PASS'; Tests = $passed.Count; Cases = $passed.ToArray(); FixtureDirectory = $testRoot; RealGameTouched = $false; LaunchedGame = $false; LiveGameProcesses = $liveSummary; LiveBlockersForFixture = $liveBlockers }
