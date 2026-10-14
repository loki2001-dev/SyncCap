<#
.SYNOPSIS
  Downloads the FFmpeg + libx264 Windows build (GPL, shared) into 3rdparty/ffmpeg_x264/win64.

.DESCRIPTION
  The repository does not carry FFmpeg binaries (size, and GPL redistribution terms).
  By default this fetches one pinned BtbN build and checks its SHA-256.
  BtbN removes old builds after a while; if the pinned one is gone, pass another
  n8.1 "win64-gpl-shared" zip and -Sha256 "" to skip the check, for example
    scripts\fetch_ffmpeg.ps1 -Url https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-n8.1-latest-win64-gpl-shared-8.1.zip -Sha256 ""

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\fetch_ffmpeg.ps1
#>
param(
    [string]$Url = "https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-10-13-13-05/ffmpeg-n8.1.3-18-g46d899d7a0-win64-gpl-shared-8.1.zip",
    [string]$Sha256 = "b8421fee5f974528ee84dd5364922afb30baeddf2841c111dff08b131fa1a76b",
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$root = Split-Path -Parent $PSScriptRoot
$destination = Join-Path $root "3rdparty\ffmpeg_x264"
$target = Join-Path $destination "win64"

if ((Test-Path (Join-Path $target "lib\avcodec.lib")) -and -not $Force) {
    Write-Output "FFmpeg already installed at $target (use -Force to replace)"
    exit 0
}

New-Item -ItemType Directory -Force -Path $destination | Out-Null
$zip = Join-Path $destination "ffmpeg.zip"
Write-Output "downloading $Url"
Invoke-WebRequest -Uri $Url -OutFile $zip -UseBasicParsing

if ($Sha256 -ne "") {
    $actual = (Get-FileHash -Path $zip -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $Sha256.ToLowerInvariant()) {
        Remove-Item $zip -Force
        throw "SHA-256 mismatch: expected $Sha256, got $actual"
    }
    Write-Output "SHA-256 verified"
}

$extract = Join-Path $destination "extract"
if (Test-Path $extract) { Remove-Item -Recurse -Force $extract }
Write-Output "extracting"
Expand-Archive -Path $zip -DestinationPath $extract -Force
$inner = Get-ChildItem -Path $extract -Directory | Select-Object -First 1
if (-not $inner) { throw "the archive has no top-level directory" }
if (Test-Path $target) { Remove-Item -Recurse -Force $target }
Move-Item -Path $inner.FullName -Destination $target
Remove-Item -Recurse -Force $extract
Remove-Item -Force $zip

# the collector needs this exact encoder; refuse a build without it
$encoders = & (Join-Path $target "bin\ffmpeg.exe") -hide_banner -encoders 2>$null
if (-not ($encoders | Select-String -SimpleMatch "libx264")) {
    throw "this FFmpeg build has no libx264 encoder; use a *-gpl-* build"
}
Write-Output "installed $target (libx264 present)"
