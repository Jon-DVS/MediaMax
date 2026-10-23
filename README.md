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
| `mediamax_control` | executable | The control interface: manages one or many playback systems (one-shot commands or interactive REPL). |

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
        | mediamax_control (links the core only) |
        +----------------------------------------+
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

## Using the control interface

Systems are configured in a JSON file (default `./systems.json`):

```json
{
  "systems": [
    { "name": "Main Wall",  "host": "192.168.1.50", "port": 47600 },
    { "name": "Lobby",      "host": "192.168.1.51", "port": 47600 }
  ]
}
```

```
mediamax_control --list
mediamax_control --system <name|idx|host:port> --command "method" [json-params]
mediamax_control --system <name|idx|host:port>        (interactive REPL)
```

Examples:

```sh
# One-shot commands
mediamax_control --system "Main Wall" --command "status"
mediamax_control --system "Main Wall" --command "show.load" '{"path":"show.json"}'
mediamax_control --system "Main Wall" --command "playback.play" '{"at":"0"}'
mediamax_control --system "Main Wall" --command "display.warp.set" \
  '{"display":1,"points":[[0,0],[1,0],[0,1],[1,1]]}'

# Interactive REPL
mediamax_control --system 1
  > system "Main Wall"
  > status
  > scene.set {"index":0}
  > playback.play
  > quit
```

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
  src/                    Control interface executable
docs/                     Architecture, protocol and show format reference
skills/                   Development playbooks (see AGENTS.md)
```

## Documentation

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — modules, threading, invariants
- [docs/PROTOCOL.md](docs/PROTOCOL.md) — the control protocol, all commands
- [docs/SHOW_FORMAT.md](docs/SHOW_FORMAT.md) — the show file JSON format
- [AGENTS.md](AGENTS.md) — development guidelines and gotchas
