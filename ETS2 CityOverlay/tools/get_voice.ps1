# Downloads the Piper speech engine and a voice into <Target>\voice (the plugin's data folder).
# usage: pwsh -File tools\get_voice.ps1 -Target "<...>\plugins\ets2_city_overlay" [-Voice en_GB-jenny_dioco-medium]
#
# Piper (MIT licence): https://github.com/rhasspy/piper
# Voices (see each voice's model card for its licence): https://huggingface.co/rhasspy/piper-voices
param(
    [Parameter(Mandatory)] [string]$Target,
    [string]$Voice = 'en_GB-jenny_dioco-medium'
)
$ErrorActionPreference = 'Stop'
$voiceDir = Join-Path $Target 'voice'
New-Item -ItemType Directory -Force $voiceDir | Out-Null

$exe = Join-Path $voiceDir 'piper\piper.exe'
if (-not (Test-Path $exe)) {
    $zip = Join-Path $env:TEMP 'piper_windows_amd64.zip'
    Write-Host 'Downloading Piper...'
    Invoke-WebRequest 'https://github.com/rhasspy/piper/releases/download/2023.11.14-2/piper_windows_amd64.zip' -OutFile $zip
    Expand-Archive $zip -DestinationPath $voiceDir -Force   # creates voice\piper\...
    Remove-Item $zip
}

# Voice names look like <lang>_<REGION>-<name>-<quality>, e.g. en_GB-jenny_dioco-medium.
if ($Voice -notmatch '^(([a-z]{2})_[A-Z]{2})-(.+)-(x_low|low|medium|high)$') { throw "Unexpected voice name '$Voice'" }
$base = "https://huggingface.co/rhasspy/piper-voices/resolve/main/$($Matches[2])/$($Matches[1])/$($Matches[3])/$($Matches[4])/$Voice"
foreach ($ext in '.onnx', '.onnx.json') {
    $file = Join-Path $voiceDir "$Voice$ext"
    if (-not (Test-Path $file)) {
        Write-Host "Downloading $Voice$ext..."
        Invoke-WebRequest "$base$ext" -OutFile $file
    }
}
Write-Host "Voice ready in $voiceDir"
