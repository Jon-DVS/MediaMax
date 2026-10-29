# MediaMax example shows

Self-contained example shows demonstrating the engine's features. Each folder
contains its own `show.json` and a `media/` directory, and the media paths in
the show file are **relative to the example folder** — run the playback
engine with the working directory set to the example folder (or pass an
absolute path to `--show`; the media paths still resolve against the working
directory).

Build the project first (see the top-level README):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

## Running an example

```bat
cd examples\01_basics
..\..\build\playback\Release\mediamax_playback.exe --show show.json
```

A window titled `MediaMax - Display 1` opens and the show starts playing
automatically (each startup show auto-plays). The engine opens a 1920x1080
window by default - the HD output space all the example shows are laid out
in - and runs them at their show frame rate (60 fps); `--width`/`--height`
change the window, `--display WxH@X+Y` adds off-default displays (see
`06_multi_display`). The control interface listens on port 47600; in a
second terminal:

```bat
..\..\build\control\Release\mediamax_control.exe --system 127.0.0.1:47600
```

Each REPL line is a protocol method with optional JSON params. Useful
commands while an example runs: `status` (state, timecode, scene, media,
displays), `show.info`, `scene.list`, `playback.play`, `playback.pause`,
`playback.stop`, `playback.seek {"time":5.0}`,
`scene.set {"index":0}` (0-based), `layer.set` / `layer.clear`,
`media.list`, `display.list`, `quit`.

## The examples

| Folder | Demonstrates |
| --- | --- |
| `01_basics` | Scenes and cuts (`playScene` cues), color layers, video + still-image layers, `zIndex` stacking, `loop` (a 4 s clip in a 6 s scene), `inPoint`, `rate` (0.75 slow motion) |
| `02_keyframes` | Keyframe tracks: `x`/`y` linear tracks (a diamond orbit), `rotation` (one turn in 12 s), `scale` (pulse), `step` interpolation (blinking opacity), and a linear fade-in |
| `03_blends` | Blend modes over SMPTE bars: `add` (static) cycled to `screen`, `multiply`, `subtract` by `setLayerBlend` cues. `normal` is the default mode |
| `04_cues` | The cue timeline: an opacity fade, visibility on/off, a position jump, a **`seek` cue that skips an entire scene** (watch the timecode jump 9.0 → 16.5 — the red scene is never shown), and a `pause` cue that holds the last frame (resume with `playback.play`) |
| `05_video_audio` | A/V sync: a 5 s segment (inPoint 1.0, outPoint 6.0) of an 8 s clip **with audio**, looping inside a 10 s show; layer opacity also drives the audio gain (you hear the fade) |
| `06_multi_display` | A two-display wall (3840x1080 shared wall space, two HD outputs): per-display positioning, a layer spanning both displays, and a scene swap |

### 04_cues: watching the seek cue

Run the example and watch `status` around 9 s — the timecode jumps from
`00:00:09:00` straight to `00:00:16:30` (at 60 fps the show frame at 16.5 s
is frame 30 of the 17th second, and that frame is scene 3, so the red
"Skipped" scene between 10–16 s is never rendered). At 22 s the show
pauses and holds; `playback.play` resumes it to the end.

### 06_multi_display: the two-window wall

```bat
cd examples\06_multi_display
..\..\build\playback\Release\mediamax_playback.exe --show show.json
```

The show file carries the wall itself (`displays` section: two 1920x1080
panels side by side), so no `--display` flags are needed — the engine
starts with one default window and loads the show's wall on top of it
(Display 1 updated in place, Display 2 added). Two HD windows appear side
by side. Content is laid out in the shared wall space (3840x1080, origin
top-left): the video is centered on the left display (x=960), the logo on
the right (x=2880), and the 3840 px plasma strip crosses **both** windows.
At 8 s the panels swap.

While it runs, try a projection warp from the control console (display 1,
a 2x2 grid of display-local UV points — a mild trapezoid):

```
display.warp.set {"display":1,"points":[[0,0],[1,0],[0.05,0.95],[0.95,0.95]]}
```

and reset it with the identity grid
`{"display":1,"points":[[0,0],[1,0],[0,1],[1,1]]}`.

## Regenerating the media

The example media are generated test patterns (FFmpeg lavfi sources:
`testsrc2`, `gradients`, `smptebars`, `sine`) at full HD (1920x1080) and
60 fps with a fixed 12 Mbit/s video bitrate, matching the 60 fps HD output
space the shows run in. To regenerate everything:

```bat
powershell -ExecutionPolicy Bypass -File examples\generate_media.ps1
```

The script prefers the FFmpeg binaries CMake fetched
(`build\_deps\mediamax-ffmpeg\bin\ffmpeg.exe`) and falls back to a
`ffmpeg` on PATH.
