# Dominus: Single Screen

**v0.1.0 — source publication of the validated internal rc5 implementation.**
A native C++ mod for **Castlevania Dominus Collection** on Steam/Windows x64,
currently supporting **Castlevania: Portrait of Ruin** only.

## Features

- Gameplay alone, scaled proportionally to 4:3 with point sampling and black bars.
- **View / Back:** open or close the original map, enlarged, with native simulation pause.
- **View + Y:** enable or disable the whole mod and restore the original collection layout.
- Native map-marker animation replay from captured game pixels, including safe
  cycle retention across stationary map reopenings and asynchronous pause release.
- With the mod disabled, View is forwarded as native Select on release.
- Separate D3D11 proxy files with a version gate and manifest-based installation/removal;
  original game executables, DLLs and data packages are not replaced.

The mod starts **off**. Load Portrait of Ruin and press View + Y.

## Compatibility and limitations

| Area | Status |
| --- | --- |
| Portrait of Ruin | Validated in gameplay with an Xbox-compatible controller through Steam Input |
| Dawn of Sorrow / Order of Ecclesia / Haunted Castle | Unsupported; keep the mod off |
| Display | Validated at 1920x1080 and 2560x1440, 16:9 |
| Collection layout | Gameplay left; map and Status stacked on the right |
| Game executable | SHA-256 `E6FA885F560C8A47640FF8DF4DE3CE4A938C8053EB11A90A7B65F5EBF1AB6B64` |

The marker needs a complete capture at the current position. Opening the map
while moving or before approximately one 39-tick cycle (650 ms) is available
keeps the native marker frozen, sometimes nearly invisible. It does not invent
missing frames or replay the marker from an old position. A captured cycle is
retained across validated stationary reopenings.

Other layouts/aspect ratios, keyboard shortcuts and filtering a second controller
are not supported. Holding native Select is not passed through; View is replayed
as a short tap on release. Opening the collection menu over the map by routes
other than controller disconnection has limited validation. This proxy conflicts
with other local D3D11/DXGI wrappers such as ReShade, DXVK and 3Dmigoto; the installer
refuses known conflicts instead of replacing them. Diagnostic logs accumulate
under `%LOCALAPPDATA%/DominusSingleScreen`; detailed marker traces are off by default.

## Build and use

This repository contains **source only**. It does not provide game files,
precompiled mods, a GitHub Release, or a downloader.

See [building and tests](docs/BUILD.md), [installation/removal](docs/INSTALL.md),
[architecture](docs/ARCHITECTURE.md), and [validation](docs/VALIDATION.md).
Development in this checkout does not change an installed mod: installation is
a separate, explicit operation after building and reviewing a package.

## License

Project-owned code and documentation are licensed under the [MIT License](LICENSE).
Third-party components retain their own licenses; see
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) and `third_party/`.
No Castlevania game assets, saves, proprietary DLLs or Steam credentials are included.
This is an unofficial fan project, not affiliated with Konami or M2.
