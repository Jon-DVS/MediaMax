# MediaMax

MediaMax is a production-grade media server for live projection and multi-wall
playback. It plays multi-layer shows (video, images, solid colors) onto
multiple physical displays with per-layer blending, per-display projection
warping and brightness, driven by a cue-based timeline. It is controlled
remotely over a small JSON-over-TCP protocol.

Feature inspiration: [Resolume](https://resolume.com/),
[WATCHOUT](https://www.dataton.com/watchout),
[PIXERA](https://pixera.one/en/).

## What it does

- **Multi-video playback** — many media layers per scene, each with its own
  in/out points, loop, playback rate, transform (position/rotation/scale),
  opacity and blend mode (`normal`, `add`, `subtract`, `multiply`, `screen`).
  Any container/codec the bundled FFmpeg decodes, including Vidvox
  **HAP** and **HAPQ** video (`.mov`, tags Hap1 / HapY).
- **Timelines** — shows are a frame rate, a set of scenes, and a cue
  timeline. The rendered frame at show time `t` is a *pure function* of
  `(show, t, overrides)`, so seeking forward and backward is always correct.
- **Multi-display output** — each display is a rectangle in a shared output
  space (a "wall"); the engine composites the scene once and maps it onto
  every display.
- **Warping and blending** — per-display projection correction via a dense
  warp texture (built from a user-specified grid of corner/edge points),
  per-display brightness, and per-layer blend modes for content blending.
- **Control interface** — a CLI that talks to one or many playback systems
  over a versioned JSON protocol (load shows, transport, scene/layer
  overrides, display geometry, warp grids).

## Architecture

| Component | Kind | Description |
| --- | --- | --- |
| `mediamax` | static library | Headless core: time math, shows/cues, blend reference, media library, playback engine, network protocol. No GPU, no windows — fully unit-testable. |
| `mediamax_render` | static library | Vulkan/NVRHI rendering: context bootstrap, swapchains, the ping-pong scene compositor and the per-display output pass. |
| `mediamax_playback` | executable | The playback engine: renders the show to one or more displays, decodes media, plays audio, and hosts the control server. |
| `mediamax_control` | executable | The control interface: manages one or many playback systems — windowed GUI (Dear ImGui), one-shot commands, or interactive REPL. |

The dependency flow is strictly one-directional:

```
            +-----------------+        +----------------------+
            | mediamax (core) | <----- | mediamax_render      |
            +--------+--------+        +-----------+----------+
                     ^                                ^
                     |                                |
        +------------+-----------+    +---------------+---------------+
        | mediamax_playback     |    | (playback also links the core
        | (links both)          |    |  for protocol + media)         |
        +-----------------------+    +-------------------------------+
        | mediamax_control (links the core only; the |
        | GUI adds ImGui/GLFW/Vulkan, never render)  |
        +--------------------------------------------+
```

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the module-by-module
walkthrough, threading model and design invariants.

## Tech stack

- C++20, CMake (>= 3.22)
- [Vulkan](https://vulkan.lunarg.com/) via [NVRHI](https://github.com/NVIDIA-RTX/NVRHI)
  (single static build) and [GLFW](https://github.com/glfw/glfw) for windows
- [glm](https://github.com/g-truc/glm) for math
- [miniaudio](https://github.com/mackron/miniaudio) for audio output
- [FFmpeg](https://ffmpeg.org/) for media decoding (optionally disabled;
  test-pattern sources still work without it)
- [Dear ImGui](https://github.com/ocornut/imgui) (docking branch) for the
  control interface GUI, on GLFW + a minimal raw-Vulkan surface
- [doctest](https://github.com/doctest/doctest) for unit tests
- [nlohmann/json](https://github.com/nlohmann/json) for the protocol and show
  files

All third-party dependencies are fetched automatically at CMake configure
time (FetchContent); no manual downloads are required beyond the platforms
listed below.

## Requirements

- **Windows:** Visual Studio 2022 or newer (MSVC), CMake >= 3.22, a
  [Vulkan SDK](https://vulkan.lunarg.com/sdk/home) with a supported GPU driver.
- **Linux (Ubuntu 24.04+):** `g++-12` or newer, CMake >= 3.22,
  `libvulkan-dev`, Mesa or vendor Vulkan drivers, `libglfw3-dev`
  (GLFW is otherwise fetched automatically).

## Building

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j
```

(On Windows with the Visual Studio generator, `-DCMAKE_BUILD_TYPE` is optional;
`--config Release` selects the configuration.)

Produced artifacts:

- `build/playback/Release/mediamax_playback(.exe)`
- `build/control/Release/mediamax_control(.exe)`
- `build/core/tests/Release/mediamax_core_tests(.exe)`
- `build/render/tests/Release/mediamax_render_tests(.exe)`

CMake options:

| Option | Default | Meaning |
| --- | --- | --- |
| `MEDIAMAX_BUILD_TESTS` | `ON` | Build the unit test suites |
| `MEDIAMAX_ENABLE_FFMPEG` | `ON` | FFmpeg media decoding (file playback). `OFF` builds a core with test-pattern sources only. |
| `MEDIAMAX_ENABLE_VALIDATION` | `ON` | Build NVRHI with its validation backend |

## Testing

```sh
ctest --test-dir build -C Release --output-on-failure
```

- `mediamax_core_tests` — the headless core: time math, show/cue
  evaluation, blend reference, transform/warp math, media library, FFmpeg
  decode round-trips, network client/server, protocol command handling.
- `mediamax_render_tests` — real GPU tests: they render frames offscreen
  through the full NVRHI pipeline, read the pixels back and compare against
  the CPU blend reference. On machines without a usable GPU they report
  "skipping" and pass.

The playback engine also has a built-in smoke test that runs headless,
renders a synthetic show offscreen and writes PPM frames:

```sh
mediamax_playback --self-test --self-test-seconds 3
# -> writes selftest_*.ppm and prints "self-test: OK - N frames rendered, ..."
```

## Running the playback engine

```
mediamax_playback [options]

  --host <ip>            Control server bind address (default 0.0.0.0)
  --port <n>             Control server port (default 47600)
  --show <file.json>     Show file to load at startup
  --system-name <name>   Name reported to the control interface
  --width <n>            Default display width (default 1920)
  --height <n>           Default display height (default 1080)
  --display WxH@X+Y      One physical output; repeat for a wall
  --self-test            Run headless, render offscreen, write a PPM
  --self-test-out <f>    Self-test output PPM (default selftest.ppm)
  --self-test-seconds <n> Self-test duration in seconds (default 3)
```

A two-panel wall, 1920x1080 each:

```sh
mediamax_playback --display 1920x1080@0+0 --display 1920x1080@1920+0 \
                  --system-name "Main Wall" --show show.json
```

## Example shows

`examples/` contains six self-contained example shows (each with its own
`media/`) that demonstrate the feature set: scenes and cuts, keyframe
animation tracks, blend modes, the cue timeline (opacity/visibility/position
overrides, a seek cue that skips a scene, pause), A/V-synced looping media,
and a two-display wall with per-display content and a spanning layer:

```bat
cd examples\01_basics
..\..\build\playback\Release\mediamax_playback.exe --show show.json
```

See [examples/README.md](examples/README.md) for the full list, the
per-example control-console tricks (including a projection-warp demo) and a
script to regenerate the example media.

## Using the control interface

The system list is stored in the show file (its `systems` field):

```json
"systems": [
  { "name": "Main Wall",  "host": "192.168.1.50", "port": 47600 },
  { "name": "Lobby",      "host": "192.168.1.51", "port": 47600 }
]
```

```
mediamax_control --show show.json --list
mediamax_control --show show.json --system <name|idx|host:port> --command "method" [json-params]
mediamax_control --show show.json --system <name|idx|host:port>        (interactive REPL)
mediamax_control --system <host:port>                                 (direct, no file)
```

Examples:

```sh
# One-shot commands
mediamax_control --show show.json --system "Main Wall" --command "status"
mediamax_control --show show.json --system "Main Wall" --command "show.load" '{"path":"show.json"}'
mediamax_control --show show.json --system "Main Wall" --command "playback.play" '{"at":"0"}'
mediamax_control --show show.json --system "Main Wall" --command "display.warp.set" \
  '{"display":1,"points":[[0,0],[1,0],[0,1],[1,1]]}'

# Interactive REPL
mediamax_control --show show.json --system 1
  > system "Main Wall"
  > status
  > scene.set {"index":0}
  > playback.play
  > quit
```

### The control GUI

```
mediamax_control --gui
```

Opens a windowed multi-system control interface (Dear ImGui, docking layout).
The show file drives it: File > Open loads a show *and its system list*
(entries connect automatically), and File > Save (or a push) writes the list
back in. A poll thread keeps each entry's status/scene snapshot fresh.

- **Systems** (left) — the system list with live connection state, connect/
  disconnect and removal, plus a form to add new systems (stored in the show
  file; written in by File > Save).
- **Playback** (center) — for the selected system: transport
  (play/pause/stop), a seek slider that follows the live time, scene
  selection and *scene.clear* (return to the timeline), live layer
  overrides (opacity/visible/blend/position via `layer.set`/`layer.clear`),
  display geometry + brightness (`display.set`) with a 2x2–4x4 warp-grid
  editor (`display.warp.set`), and media load/unload against the engine's
  library.
- **Show Editor** (right) — author show.json documents: show settings,
  media references (with a *Probe* that loads them on a system), scenes with
  full layer editing (media, color, in/out, transform, opacity/blend, and
  keyframe tracks for opacity/x/y/rotation/scale) and the cue timeline
  (all eight cue actions). *Save* writes the file, *Push to system* sends
  the document live via `show.loadJson`, *Pull from system* runs `show.save`
  on the system.
- **Console** (bottom) — any raw protocol method with JSON params, with the
  exchange logged.

Everything the CLI does is reachable in the GUI, and vice versa — the two
share the same protocol. The GUI's own smoke test:

```
mediamax_control --gui-self-test [frames]
mediamax_control --gui-self-test [frames] --gui-self-test-system <host:port>
```

The self-test renders full UI frames into a hidden window (no systems, so it
is side-effect free) and, with `--gui-self-test-system`, drives a running
playback engine through the same command path the UI uses (it replaces the
engine's loaded show with a deterministic one). The frame count defaults to
90.

The full command reference lives in [docs/PROTOCOL.md](docs/PROTOCOL.md), and
the show file format in [docs/SHOW_FORMAT.md](docs/SHOW_FORMAT.md).

## Project layout

```
CMakeLists.txt            Top-level build (options, third-party, sub-projects)
cmake/                    Build helpers (compiler settings, dependencies, shaders)
core/
  include/mediamax/       Public core headers (time, math, blend, show, media,
                          net, protocol, playback, display, util)
  src/                    Core implementation
  tests/                  doctest suite (headless)
render/
  include/mediamax/render Public render headers (VulkanContext, Swapchain,
                          SceneRenderer)
  src/                    NVRHI/Vulkan implementation
  shaders/                GLSL (compiled to SPIR-V at build time)
  tests/                  doctest GPU suite
playback/
  src/                    Playback engine executable (app, media pump, audio,
                          self-test, window surface)
control/
  src/                    Control interface executable (CLI/REPL + gui/: the
                           Dear ImGui app, systems, show editor, console)
examples/                 Self-contained example shows + generated media
docs/                     Architecture, protocol and show format reference
skills/                   Development playbooks (see AGENTS.md)
```

## Documentation

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — modules, threading, invariants
- [docs/PROTOCOL.md](docs/PROTOCOL.md) — the control protocol, all commands
- [docs/SHOW_FORMAT.md](docs/SHOW_FORMAT.md) — the show file JSON format
- [AGENTS.md](AGENTS.md) — development guidelines and gotchas
