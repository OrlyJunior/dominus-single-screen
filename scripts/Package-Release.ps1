# Builds the end-user ZIP from an already compiled d3d11.dll. Never builds,
# installs or touches the game. Contents: the mod files, installers, docs and
# required license texts only.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [Parameter(Mandatory = $true)][string]$Version
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
$name = "DominusSingleScreen-$Version"
$output = [IO.Path]::GetFullPath($OutputDirectory)
$staging = Join-Path $output $name
$zip = Join-Path $output "$name.zip"
if ((Test-Path -LiteralPath $staging) -or (Test-Path -LiteralPath $zip)) { throw "Release $name already exists in $output; choose a new version." }
New-Item -ItemType Directory -Path (Join-Path $staging 'scripts'), (Join-Path $staging 'licenses') -Force | Out-Null

function Copy-Exact([string]$From, [string]$To) {
    Copy-Item -LiteralPath $From -Destination $To
    if ((Get-FileHash -LiteralPath $From -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $To -Algorithm SHA256).Hash) { throw "Copy mismatch: $From" }
}
# Batch files need CRLF for cmd.exe.
function Copy-Crlf([string]$From, [string]$To) {
    $text = [IO.File]::ReadAllText($From) -replace "`r?`n", "`r`n"
    [IO.File]::WriteAllText($To, $text, [Text.Encoding]::ASCII)
}

Copy-Exact (Join-Path $BuildDirectory 'd3d11.dll') (Join-Path $staging 'd3d11.dll')
Copy-Exact (Join-Path $project 'release\dominus-single-screen.ini') (Join-Path $staging 'dominus-single-screen.ini')
foreach ($file in 'README.md', 'CHANGELOG.md', 'THIRD-PARTY-NOTICES.md') { Copy-Exact (Join-Path $project "release\$file") (Join-Path $staging $file) }
Copy-Exact (Join-Path $project 'LICENSE') (Join-Path $staging 'LICENSE.txt')
Copy-Crlf (Join-Path $project 'release\Install.bat') (Join-Path $staging 'Install.bat')
Copy-Crlf (Join-Path $project 'release\Uninstall.bat') (Join-Path $staging 'Uninstall.bat')
foreach ($file in 'Find-DominusGame.ps1', 'Install-DominusSingleScreen.ps1', 'Uninstall-DominusSingleScreen.ps1') { Copy-Exact (Join-Path $project "release\scripts\$file") (Join-Path $staging "scripts\$file") }
foreach ($file in 'Install-Mod.ps1', 'Uninstall-Mod.ps1', 'Verify-Game.ps1') { Copy-Exact (Join-Path $project "scripts\$file") (Join-Path $staging "scripts\$file") }
Copy-Exact (Join-Path $project 'third_party\minhook\LICENSE.txt') (Join-Path $staging 'licenses\MinHook-LICENSE.txt')
foreach ($file in Get-ChildItem -LiteralPath (Join-Path $project 'third_party\runtime-licenses') -File) { Copy-Exact $file.FullName (Join-Path $staging "licenses\$($file.Name)") }
# License blocks quoted verbatim from their sources.
$header = [IO.File]::ReadAllText((Join-Path $project 'third_party\renderdoc\renderdoc_app.h'))
$block = [regex]::Match($header, '(?s)^/\*{10,}.*?\*{10,}/')
if (-not $block.Success) { throw 'RenderDoc license block not found.' }
[IO.File]::WriteAllText((Join-Path $staging 'licenses\RenderDoc-LICENSE.txt'), $block.Value + "`r`n", [Text.Encoding]::ASCII)
$notices = [IO.File]::ReadAllText((Join-Path $project 'THIRD-PARTY-NOTICES.md'))
$mit = [regex]::Match($notices, '(?s)> MIT License.*?SOFTWARE\.')
if (-not $mit.Success) { throw 'MGSM2Fix notice not found.' }
[IO.File]::WriteAllText((Join-Path $staging 'licenses\MGSM2Fix-LICENSE.txt'), (($mit.Value -split "`r?`n" | ForEach-Object { $_ -replace '^>\s?', '' }) -join "`r`n") + "`r`n", [Text.Encoding]::ASCII)

$sums = Get-ChildItem -LiteralPath $staging -Recurse -File | Sort-Object FullName | ForEach-Object {
    '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash, $_.FullName.Substring($staging.Length + 1).Replace('\', '/')
}
[IO.File]::WriteAllLines((Join-Path $staging 'SHA256SUMS.txt'), [string[]]$sums, [Text.Encoding]::ASCII)
# Windows PowerShell's Compress-Archive writes '\' separators, which the ZIP
# format does not allow; write entries with '/' explicitly.
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($file in Get-ChildItem -LiteralPath $staging -Recurse -File | Sort-Object FullName) {
        $entry = $file.FullName.Substring($staging.Length + 1).Replace('\', '/')
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file.FullName, $entry, [IO.Compression.CompressionLevel]::Optimal)
    }
} finally { $archive.Dispose() }
[pscustomobject]@{ Zip = $zip; ZipSHA256 = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash; Files = @($sums).Count + 1 }
