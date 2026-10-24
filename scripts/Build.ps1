[CmdletBinding()]
# -Release drops debug info and symbols: public binaries must not embed local
# paths (user name, project folder) from DWARF/PDB data.
param([string]$ZigPath, [string]$BuildDirectory, [switch]$Release, [switch]$TestsOnly)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $ZigPath) {
    $zigCommand = Get-Command zig.exe -ErrorAction SilentlyContinue
    if ($zigCommand) { $ZigPath = $zigCommand.Source }
}
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $projectRoot 'out\build' }
if (-not $ZigPath) { throw 'Provide -ZigPath or put a trusted Zig 0.13.0 zig.exe on PATH.' }
if (-not (Test-Path -LiteralPath $ZigPath -PathType Leaf)) { throw 'Provide -ZigPath pointing to a trusted Zig 0.13.0 installation.' }
$ZigPath = (Resolve-Path -LiteralPath $ZigPath).Path
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
New-Item -ItemType Directory -Path $BuildDirectory -Force | Out-Null
$oldGlobal = $env:ZIG_GLOBAL_CACHE_DIR
$oldLocal = $env:ZIG_LOCAL_CACHE_DIR
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $BuildDirectory '.cache\global'
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $BuildDirectory '.cache\local'
function Invoke-Compiler([string[]]$CompilerArguments) {
    & $ZigPath @CompilerArguments
    if ($LASTEXITCODE -ne 0) { throw "Compiler exited with $LASTEXITCODE" }
}
Push-Location -LiteralPath $projectRoot
try {
    $version = & $ZigPath version
    if ($version.Trim() -ne '0.13.0') { throw "Expected Zig 0.13.0; got $version" }
    $common = @('-target','x86_64-windows-gnu','-O2')
    if ($Release) { $common += @('-g0','-s') }
    $cIncludes = @('-Ithird_party/minhook/include','-Ithird_party/minhook/src')
    $objects = @()
    foreach ($source in @('buffer','hook','trampoline','hde/hde64')) {
        $object = Join-Path $BuildDirectory (($source -replace '/','_') + '.o')
        Invoke-Compiler (@('cc') + $common + $cIncludes + @('-c',"third_party/minhook/src/$source.c",'-o',$object))
        $objects += $object
    }
    $cpp = @('c++') + $common + @('-std=c++17','-fms-extensions','-Wall','-Wextra','-Iinclude')
    if (-not $TestsOnly) {
        Invoke-Compiler ($cpp + $cIncludes + @('-shared','-static','src/proxy.cpp','src/graphics.cpp','src/core.cpp','src/native_runtime.cpp','src/render_capture.cpp','src/resource_observer.cpp','src/input_observer.cpp','src/live_controller.cpp','src/marker_blink.cpp','src/marker_identity.cpp','src/d3d11.def') + $objects + @('-lbcrypt','-luser32','-lgdi32','-lole32','-o',(Join-Path $BuildDirectory 'd3d11.dll')))
    }
    Invoke-Compiler ($cpp + @('-static','src/core.cpp','tests/core_tests.cpp','-o',(Join-Path $BuildDirectory 'core_tests.exe')))
    # Runs real game code read at run time from a local analysis copy (argument).
    Invoke-Compiler ($cpp + $cIncludes + @('-static','src/core.cpp','src/input_observer.cpp','tests/steam_pad_tests.cpp') + $objects + @('-o',(Join-Path $BuildDirectory 'steam_pad_tests.exe')))
    if (-not $TestsOnly) {
        Invoke-Compiler ($cpp + @('-static','-municode','tools/attach_observer.cpp','-o',(Join-Path $BuildDirectory 'attach_observer.exe')))
    }
    if (Test-Path -LiteralPath 'tests/d3d11_tests.cpp') {
        Invoke-Compiler ($cpp + @('-static','src/core.cpp','src/graphics.cpp','src/marker_blink.cpp','src/marker_identity.cpp','tests/d3d11_tests.cpp','-lbcrypt','-luser32','-lgdi32','-lole32','-o',(Join-Path $BuildDirectory 'dominus_d3d11_tests.exe')))
    }
    Copy-Item -LiteralPath 'dominus-single-screen.ini' -Destination (Join-Path $BuildDirectory 'dominus-single-screen.ini') -Force
    Write-Output "Build complete: $BuildDirectory"
} finally {
    Pop-Location
    $env:ZIG_GLOBAL_CACHE_DIR = $oldGlobal
    $env:ZIG_LOCAL_CACHE_DIR = $oldLocal
}
