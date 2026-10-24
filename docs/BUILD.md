# Build and test

## Prerequisites

Windows x64; trusted Zig **0.13.0** available locally; PowerShell 5.1 or newer.
Python 3.10 or newer is needed for the PE-parser tests. Scripts do not download or
install dependencies, change global PATH, or start Steam/the game.

From the repository root, with zig.exe on PATH:

```powershell
& ./scripts/Build.ps1 -Release
```

Alternatively pass `-ZigPath $zigExecutable` where that variable names your local
compiler, and optionally `-BuildDirectory $outputDirectory`. Defaults are
`out/build` for artifacts and caches inside that build directory. The root INI
uses the functional release settings, with StartEnabled=0 and marker traces off.
A new build is not installed automatically. Optimized builds use static runtimes;
paths and timestamps may prevent byte-identical DLL hashes between output folders.

For source validation without producing a new DLL:

```powershell
& ./scripts/Build.ps1 -TestsOnly -Release
& ./out/build/core_tests.exe
python -B -m unittest discover -s tests -p 'test_*.py' -v
& ./scripts/Test-Install.ps1
```

Synthetic installer tests create retained fixtures under `out/installer-tests`,
never in a Steam installation. They may refuse if a process named game.exe is
running; that is a safety refusal, not permission to close someone else's game.

The D3D11 WARP test uses only its own hidden windows, textures and synthetic
process. Run against a separately built DLL:

```powershell
$dll = (Resolve-Path ./out/build/d3d11.dll).Path
& ./out/build/dominus_d3d11_tests.exe --proxy $dll
```

For the unknown-process guard, copy only this synthetic test executable to
`out/build/dominus_guard_tests.exe` and run it with `--guard $dll`.

The optional Steam-pad ABI test requires a **user-owned local analysis copy** of
the supported game executable, passed explicitly:

```powershell
& ./out/build/steam_pad_tests.exe $analysisExecutable
```

It validates the fingerprint and executes a copied function in the synthetic
host with fake Steam responses. No game executable is bundled. This is an opt-in
local test, not a default CTest test and not a reason to fetch game files.

## CMake

The source tree also includes CMake 3.20+ targets for the core, graphics, marker,
proxy, developer attach utility and test executables. Use a Windows x64 toolchain
with the Windows SDK. For example, in an x64 developer environment:

```powershell
cmake -S . -B out/cmake
cmake --build out/cmake --config Release
ctest --test-dir out/cmake -C Release --output-on-failure
```

Zig/Build.ps1 is the historical validated build route. The publication corrects
missing marker/provider/Steam-pad target wiring in the previous CMake file;
CMake/MSVC builds have not received the same in-game validation. CTest registers
only synthetic tests; it neither installs nor starts a game.

## Optional local package

Only after building, a private package can be prepared with
`scripts/Package-Release.ps1 -BuildDirectory $buildDirectory -OutputDirectory $packageOutput -Version 0.1.0`.
It creates files only at the requested output and refuses an existing package.
Do not commit the resulting binary/archive. Package creation is not publishing a
GitHub Release and does not install it. Preserve all included license notices.
