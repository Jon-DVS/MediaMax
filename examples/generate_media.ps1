# Regenerates the example media (small FFmpeg lavfi test patterns).
#
# Usage: powershell -ExecutionPolicy Bypass -File examples\generate_media.ps1
#
# Prefers the FFmpeg binaries CMake fetched into build\_deps\mediamax-ffmpeg\bin
# and falls back to a `ffmpeg` on PATH.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$ffmpeg = Join-Path $root 'build\_deps\mediamax-ffmpeg\bin\ffmpeg.exe'
if (-not (Test-Path $ffmpeg)) {
    $found = Get-Command ffmpeg -ErrorAction SilentlyContinue
    if ($null -eq $found) {
        Write-Error "no ffmpeg found (build the project first, or put ffmpeg on PATH)"
    }
    $ffmpeg = $found.Source
}
Write-Host "Using ffmpeg: $ffmpeg"

# Common encoder options (kept after the inputs: they are output options).
$video = @('-hide_banner', '-loglevel', 'error', '-y',
           '-c:v', 'libx264', '-preset', 'ultrafast', '-pix_fmt', 'yuv420p', '-an')

function Encode-Video {
    param([string]$Filter, [double]$Seconds, [string]$Out)
    & $ffmpeg -f lavfi -i $Filter -t $Seconds @video $Out
    if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed: $Out" }
}

function Encode-Image {
    param([string]$Filter, [string]$Out)
    & $ffmpeg -hide_banner -loglevel error -y -frames:v 1 -f lavfi -i $Filter $Out
    if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed: $Out" }
}

# 01_basics: scenes and cuts
Encode-Video 'testsrc2=size=960x540:rate=30' 4  "$PSScriptRoot\01_basics\media\colorbars.mp4"
Encode-Video 'gradients=size=960x540:rate=30' 6 "$PSScriptRoot\01_basics\media\plasma.mp4"
Encode-Image 'testsrc2=size=480x270:rate=30'   "$PSScriptRoot\01_basics\media\logo.png"

# 02_keyframes: animation tracks (12s show)
Encode-Video 'testsrc2=size=960x540:rate=30' 4  "$PSScriptRoot\02_keyframes\media\colorbars.mp4"
Encode-Video 'gradients=size=960x540:rate=30' 12 "$PSScriptRoot\02_keyframes\media\plasma.mp4"
Encode-Image 'testsrc2=size=480x270:rate=30'   "$PSScriptRoot\02_keyframes\media\logo.png"

# 03_blends: blend modes (8s show; static bars + moving gradient)
Encode-Video 'smptebars=size=960x540:rate=30' 8  "$PSScriptRoot\03_blends\media\bars.mp4"
Encode-Video 'gradients=size=960x540:rate=30' 8 "$PSScriptRoot\03_blends\media\plasma.mp4"

# 04_cues: cue timeline (24s show)
Encode-Video 'testsrc2=size=960x540:rate=30' 4  "$PSScriptRoot\04_cues\media\colorbars.mp4"
Encode-Image 'testsrc2=size=480x270:rate=30'   "$PSScriptRoot\04_cues\media\logo.png"

# 05_video_audio: A/V sync (8s clip with a 440 Hz tone)
& $ffmpeg -hide_banner -loglevel error -y `
    -f lavfi -i 'testsrc2=size=960x540:rate=30' `
    -f lavfi -i 'sine=frequency=440:sample_rate=48000' `
    -t 8 -c:v libx264 -preset ultrafast -pix_fmt yuv420p -c:a aac `
    "$PSScriptRoot\05_video_audio\media\tone.mp4"
if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed: tone.mp4" }

# 06_multi_display: two 960x540 outputs (1920x540 wall space)
Encode-Video 'testsrc2=size=960x540:rate=30' 4  "$PSScriptRoot\06_multi_display\media\colorbars.mp4"
Encode-Image 'testsrc2=size=480x270:rate=30'   "$PSScriptRoot\06_multi_display\media\logo.png"
Encode-Video 'gradients=size=1920x90:rate=30' 16 "$PSScriptRoot\06_multi_display\media\plasma.mp4"

Write-Host "Example media generated."
