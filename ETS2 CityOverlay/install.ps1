# Copies the built plugin into ETS2's bin\win_x64\plugins folder.
# usage:  pwsh -File install.ps1 [-GameDir "D:\SteamLibrary\steamapps\common\Euro Truck Simulator 2"]
param([string]$GameDir)

$ErrorActionPreference = 'Stop'
$build = Join-Path $PSScriptRoot 'build\Release'
if (-not (Test-Path "$build\ets2_city_overlay.dll")) { throw "Build first: cmake --build build --config Release" }

if (-not $GameDir) {
    # Find the game in any Steam library listed in libraryfolders.vdf
    $steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
    $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
    $libs = @($steam) + (Select-String -Path $vdf -Pattern '"path"\s+"([^"]+)"' |
            ForEach-Object { $_.Matches[0].Groups[1].Value -replace '\\\\', '\' })
    $GameDir = $libs | ForEach-Object { Join-Path $_ 'steamapps\common\Euro Truck Simulator 2' } |
               Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $GameDir) { throw "ETS2 not found; pass -GameDir" }
}

$plugins = Join-Path $GameDir 'bin\win_x64\plugins'
$data = Join-Path $plugins 'ets2_city_overlay'
New-Item -ItemType Directory -Force $data | Out-Null

Copy-Item "$build\ets2_city_overlay.dll" $plugins -Force
Copy-Item "$build\ets2_city_overlay\cities.csv", "$build\ets2_city_overlay\city_areas.csv", "$build\ets2_city_overlay\pois.csv", "$build\ets2_city_overlay\route_graph.bin", "$build\ets2_city_overlay\maneuvers.bin" $data -Force
# Keep the user's settings; learned.csv is never touched.
$template = "$build\ets2_city_overlay\ets2_city_overlay.ini"
$ini = "$data\ets2_city_overlay.ini"
if (-not (Test-Path $ini)) {
    Copy-Item $template $data
} else {
    # Rebuild from the new template (new options + comments), carrying over every value the user
    # has set for a key that still exists. Keys removed in newer versions are dropped.
    $mine = @{}
    $section = ''
    foreach ($line in Get-Content $ini) {
        if ($line -match '^\[(\w+)\]') { $section = $Matches[1] }
        elseif ($line -match '^\s*([A-Za-z_]+)\s*=(.*)$') { $mine["$section.$($Matches[1])"] = $Matches[2] }
    }
    $section = ''
    $out = foreach ($line in Get-Content $template) {
        if ($line -match '^\[(\w+)\]') { $section = $Matches[1]; $line }
        elseif ($line -match '^\s*([A-Za-z_]+)\s*=' -and $mine.ContainsKey("$section.$($Matches[1])")) {
            "$($Matches[1])=$($mine["$section.$($Matches[1])"])"
        } else { $line }
    }
    Copy-Item $ini "$ini.bak" -Force
    Set-Content $ini $out
    Write-Host "Updated settings (your values kept, backup: ets2_city_overlay.ini.bak)"
}

# Natural voice: Piper + the voice chosen in the settings (downloaded once, ~85 MB).
$model = (Select-String -Path $ini -Pattern '^\s*voice_model\s*=\s*(\S+)' | Select-Object -First 1).Matches.Groups[1].Value
if (-not $model) { $model = 'en_GB-jenny_dioco-medium' }
$built = Join-Path $build 'ets2_city_overlay\voice'
if ((Test-Path "$built\piper\piper.exe") -and -not (Test-Path "$data\voice\piper\piper.exe")) {
    Copy-Item $built "$data\voice" -Recurse -Force   # reuse what the build already downloaded
}
if (-not (Test-Path "$data\voice\piper\piper.exe") -or -not (Test-Path "$data\voice\$model.onnx")) {
    & (Join-Path $PSScriptRoot 'tools\get_voice.ps1') -Target $data -Voice $model
}

Write-Host "Installed to $plugins"
