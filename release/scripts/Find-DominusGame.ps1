# Locates the Castlevania Dominus Collection install (Steam AppID 2369900) in
# any Steam library. Read-only: registry and Steam's own .vdf/.acf files.
Set-StrictMode -Version Latest

function Find-DominusGame {
    $appId = '2369900'
    $libraries = [Collections.Generic.List[string]]::new()
    foreach ($key in 'HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam', 'HKLM:\SOFTWARE\Valve\Steam') {
        $props = Get-ItemProperty -LiteralPath $key -ErrorAction SilentlyContinue
        if ($null -eq $props) { continue }
        foreach ($name in 'SteamPath', 'InstallPath') {
            $value = $props.PSObject.Properties[$name]
            if ($value -and $value.Value) { $libraries.Add([IO.Path]::GetFullPath([string]$value.Value)) }
        }
    }
    # Every library listed by each Steam root ("path" entries in libraryfolders.vdf).
    foreach ($steam in @($libraries)) {
        $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
        if (-not (Test-Path -LiteralPath $vdf -PathType Leaf)) { continue }
        foreach ($match in [regex]::Matches([IO.File]::ReadAllText($vdf), '"path"\s+"([^"]+)"')) {
            $libraries.Add([IO.Path]::GetFullPath($match.Groups[1].Value.Replace('\\', '\')))
        }
    }
    foreach ($library in ($libraries | Select-Object -Unique)) {
        $acf = Join-Path $library "steamapps\appmanifest_$appId.acf"
        if (-not (Test-Path -LiteralPath $acf -PathType Leaf)) { continue }
        $install = [regex]::Match([IO.File]::ReadAllText($acf), '"installdir"\s+"([^"]+)"')
        if (-not $install.Success) { continue }
        $directory = Join-Path $library ('steamapps\common\' + $install.Groups[1].Value)
        if (Test-Path -LiteralPath (Join-Path $directory 'game.exe') -PathType Leaf) { return $directory }
    }
    return $null
}
