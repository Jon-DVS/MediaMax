# MediaMax architecture

## Overview

MediaMax is split into two static libraries and two executables with a strict
one-directional dependency flow:

```
                    +----------------------------------------+
                    |           mediamax (core)              |
                    |  headless: no GPU, no windows, no net  |
                    |  (engine types are data + logic only)  |
                    +---+----------------+------------------+
                        |                |
        +---------------+--+   +---------+------------------+
        | mediamax_render    |   |   (data types shared)     |
        | NVRHI/Vulkan       |   +---------------------------+
        +---------------+----+
                        |
   +--------------------+----------------------+
   |                    |
+---v-----------------+  +v-----------------------------+
| mediamax_playback   |  | mediamax_control             |
| (links core+render) |  | (links core only)            |
+---------------------+  +------------------------------+
```

- **`mediamax` (core)** — everything that must be testable on any machine:
  time math, show model, blend reference, media abstraction + FFmpeg source,
  the playback engine (pure frame function), the control protocol and its
  command handlers, and TCP transport.
- **`mediamax_render`** — the GPU side: Vulkan context bootstrap, GLFW
  swapchain management, and the scene renderer (compositor + display pass).
- **`mediamax_playback`** — the app that wires core + render + OS
  (windows, audio, the media pump thread, the control server).
- **`mediamax_control`** — the CLI/operator tool that talks to one or many
  playback systems.

## Core modules (`core/include/mediamax`)

### `time/` — exact time

`Time` is an exact reduced rational number of seconds (`value/scale`,
int64). Media servers need frame-exact timing: 10000 frames at 29.97 fps is
exactly 10000/29.97 s; doubles or fixed-point accumulate error that breaks
cue triggering, timecode and lip-sync over long shows. `FrameRate` is a
reduced rational too. `Time::fromSeconds` (rounds to the nearest
microsecond) is the single documented precision boundary for external
values; all internal math stays exact.

### `math/` — geometry

`Rect2i/Rect2f`, `Size2i`, `Transform2D` (x/y position of the anchor,
rotation in radians clockwise — y grows down —, per-axis scale, anchor in
media UV). `Transform2D::toOutputSpace(u, v, mediaSize)` is the single map
from media UV to output-space pixels; both the CPU engine and the GPU vertex
shaders derive from it.

### `blend/` — compositing reference

`blend::apply(dst, src, mode)` is the CPU reference for the five blend modes
(`Normal`, `Add`, `Subtract`, `Multiply`, `Screen`). Conventions: inputs are
straight alpha in 0..1; the equations are written in **premultiplied**
space; the GPU shader (`render/shaders/layer.frag`) implements the *same*
equations. The two must stay in sync — the render test suite compares GPU
pixels to this reference for every mode.

### `show/` — the show model

- `Show` — name, frame rate, duration (zero = unlimited), media references,
  scenes, and the cue timeline (kept sorted by time).
- `Scene` — a named set of `Layer`s.
- `Layer` — one media element: media id (`0` = solid-color layer),
  in/out points (out `0` = to end of media), loop, rate, transform, opacity,
  blend mode, visibility, zIndex (higher = on top), plus optional
  keyframe tracks for opacity/x/y/rotation/scale.
- `Cue` — `{time, name, action}` where action is a variant: `PlayScene`,
  `Stop`, `Pause`, `SeekTo`, `SetLayerOpacity`, `SetLayerVisible`,
  `SetLayerBlend`, `SetLayerPosition`.
- `KeyframeTrack` — a sorted list of `{time, value, interpolation}`
  (linear/step); `valueAt(t)` is a pure function of time (hold before first
  and after last keyframe).
- JSON (de)serialization lives in the same module; the file format is
  documented in [SHOW_FORMAT.md](SHOW_FORMAT.md).

### `media/` — media abstraction

- `IMediaSource` — a decoding source: `nextVideoFrame` / `nextAudioFrame`
  (pulled with a timeout), metadata (type, size, frame rate, duration,
  audio layout).
- `MediaLibrary` — the process-wide asset store: load (via a pluggable
  source factory), unload, lookup. **Not internally synchronized**: the app
  serializes access with its state mutex (the media pump is handed the same
  mutex), which is the synchronization point between the pump thread and
  everything else.
- `FFmpegSource` (+ `FFmpegSupport`) — the file-backed source: demux, decode
  video to RGBA8 and audio to interleaved float32, with a shared packet
  parking lot so alternating video/audio pulls never drop the other
  stream's packets. (In/out points, loop and rate are the *engine's* job —
  the source just produces frames.) Classifies standalone image files
  (per-format `*_pipe` demuxers report no frame count) as
  `MediaType::Image` in one shared helper used by both `open()` and
  `probeFile()`.
- `TestPatternSource` — synthetic sources (`mediamax:testpattern:colorbars`,
  `checkerboard`) used by the self-test and tests so the pipeline works
  without any media files.

### `playback/` — the engine (the heart)

`PlaybackEngine` owns the loaded `Show` and the playback clock and produces,
for any show time `t`, a fully resolved `FrameState`:

```
FrameState {
    showTime, activeSceneId, playing,
    layers: [ LayerFrame { resolved transform (tracks applied), opacity,
                           blend, visible, zIndex, mediaId, mediaTime,
                           mediaAtEnd, color } ]   // sorted zIndex ascending
}
```

**The defining invariant: `frameState(t)` is a pure function of
`(show, t, runtime overrides)`.** Cue evaluation is "the last cue of each
kind at or before t"; overrides (scene set from the control surface, layer
overrides) are just part of the input. Consequences: seeking in either
direction is trivially correct; pausing/resuming never desyncs; the engine
is stateless-with-respect-to-frame-order and therefore fully unit-testable
(no GPU, no clock — tests feed it explicit times).

Transport: `play/pause/stop/seek` manipulate the clock + state
(`Stopped/Playing/Paused`); `tick(dt)` moves the clock and fires transport
cues (`stop`, `pause`, `seek`), and releases the scene override at a
`playScene` cue. Media time for a layer = `(showTime - layerStart) * rate`
clamped to its in/out points, with loop/hold-at-end semantics.

### `protocol/` + `net/` — the control surface

- `Protocol.h` — the wire envelope (JSON lines, `"v":1`, 1:1 request/response
  with an echoed `id`) and encode/decode.
- `CommandServer` — maps the 20 protocol commands onto
  `PlaybackEngine` + `MediaLibrary` + `DisplayManager`. Deliberately
  engine-agnostic (no GPU, no network): given a decoded `Command` it
  produces a `Response`. All failures become `ok:false` responses
  (`ParameterError` is the internal mechanism). This makes the *entire*
  control surface unit-testable and lets clients validate commands offline.
- `net/Tcp` + `ControlServer`/`ControlClient` — the transport: blocking
  line-based TCP, the server runs the command dispatcher on a thread; the
  client is used by the control executable.

### `display/` — display wall model

`DisplayManager` owns the display list: each display is a rectangle
(`x, y, width, height`) in the shared **output space** (pixels, origin
top-left) plus brightness and enabled flag. The union of display rectangles
is the scene size the renderer targets. `WarpMap` converts a user grid of
points into the dense RG8 warp texture the GPU display pass samples (see
below).

## Render library (`render/`)

### `VulkanContext`

Headless-capable Vulkan + NVRHI bootstrap: instance/device/queue creation,
the NVRHI device, and — critically — the **single vk-hpp default dispatcher
storage** for the process (defined here, initialized with the 4-arg form
`instance, vkGetInstanceProcAddr, device, vkGetDeviceProcAddr`). The build
uses exactly one vulkan.hpp (the SDK's; NVRHI's own header fetch is
disabled) so all code dispatches through the same loader state.

### `Swapchain`

GLFW window + VK_KHR_swapchain management per display: surface creation,
image acquisition, present, and the required
`beginTrackingTextureState(..., ResourceStates::Present)` bookkeeping around
each image's lifetime. The renderer is handed the acquired images as plain
textures.

### `SceneRenderer` — compositing

The renderer is a **pure recorder**: `recordFrame(cmd, frame)` records one
full frame into a caller-owned command list (it never submits; the host
owns the queue). Per frame:

1. **Flush queued texture uploads** — the texture pool (media + warp
   textures) is updated by the media pump thread through a lock-guarded
   queue; the render thread applies the pending uploads at the start of
   recording.
2. **Clear both ping-pong scene targets** to premultiplied `(0,0,0,1)`.
   (NVRHI render passes always use `loadOp = LOAD`, so anything a layer quad
   does not cover keeps the previous content — both targets must be
   defined.)
3. **Vertex upload** — CPU-positioned quads (6 verts each) for every
   visible layer, then one per display; written with `writeBuffer`.
4. **Layer pass** — for each layer (zIndex ascending): render into the
   *next* ping-pong target, sampling the *previous* target plus the layer's
   media texture. No fixed-function blending: the fragment shader
   (`layer.frag`) implements the exact premultiplied-space equations of
   `core/src/blend/Blend.cpp` for all five modes, selected per draw via push
   constants (`LayerPush`: scene size, opacity, blend mode, color,
   useColor). Per-draw binding set: media SRV, scene SRV, linear sampler.
5. **Display pass** — one draw per display into the swapchain image (or an
   offscreen target for the self-test): samples the final scene at the
   display's output-space rectangle, applies the dense warp texture when
   present (`useWarp`), brightness, and **unpremultiplies** for the window
   (`display.frag`, push constants `DisplayPush`). Swapchain images are
   transitioned back to `Present` before submit.

Ping-pong invariant: after N layers, the final scene is in
`m_SceneRT[N % 2]` (`m_FinalSceneIndex`); `readBackScene` (offscreen
self-test/diagnostics) copies it back and unpremultiplies.

**NVRHI-specific requirements encoded in the code** (each was a real bug):
sampler binding offset forced to 0 (`VulkanBindingOffsets::setSamplerOffset(0)`)
because NVRHI's default places samplers at binding 128+slot; push constants
set *after* `setGraphicsState` (NVRHI records them immediately with the
currently bound pipeline layout, which is null before the first
`setGraphicsState`); automatic state tracking on all pooled textures and the
vertex buffer.

## Playback app (`playback/`)

Threading model:

```
render thread (main)   media pump thread      audio thread         control client threads
  |  parse args,          |  decode video/audio     |  miniaudio callback:  |  one per connection:
  |  build engine,        |  from the show's        |  snapshotTargets +    |  decode line,
  |  displays, audio      |  sources; push frames   |  mixAudio() -> device |  CommandServer::
  |  sink; per frame,     |  into the SceneRenderer |                     |  handle (under the
  |  under m_StateMutex:  |  texture pool (queued)  |                     |  handler mutex),
  |  tick(), frameState,  |                         |                     |  write response
  |  publish pump targets;|                         |                     |
  |  deliverVideo,        |                         |                     |
  |  record + present     |                         |                     |
```

- `PlaybackApp` — owns `PlaybackEngine`, `MediaLibrary`, `DisplayManager`,
  `VulkanContext`, swapchains, `SceneRenderer`, `AudioSink`,
  `ControlServer`. Each tick: advance the engine clock, resolve the
  `FrameState`, map media frames onto layers (via the texture pool), record
  and present.
- `MediaPump` — a dedicated thread that pulls `nextVideoFrame`/`nextAudioFrame`
  from every active source and hands frames to the renderer's texture pool /
  audio sink. Decoding never blocks the render loop.
- `AudioSink` — miniaudio output; mixes the show's audio (one sink per
  system; per-layer audio mixing is the current scope).
- `WindowSurface` — the GLFW window + cursor/resize plumbing for one display.
- `SelfTest` — the headless smoke test: a synthetic show (test patterns,
  two layers, a cue), a real render pipeline into an offscreen target,
  PPM frames written out, "self-test: OK" when content was rendered.

## Control app (`control/`)

Thin CLI + the operator GUI. With **no mode given it opens the windowed GUI**
(`--gui` is kept as the explicit form). The CLI reads `systems.json`
(`{"systems":[{"name","host","port"}]}`), resolves
`--system <name|idx|host:port>`, then either sends one
`--command "method" [json-params]` (one-shot) or enters a REPL
(`system <spec>`, `systems`, `<method> [json]`, `help`, `quit`). The CLI
modes link the core only — they can even validate command syntax offline
against `CommandServer`.

`--gui` runs the operator interface (Dear ImGui docking on GLFW + a minimal
raw-Vulkan surface): one dockspace with Systems (left), Show Editor (right),
Playback (center) and Console (bottom). The initial layout is built once by
`DockBuilder` on the first frame; `DockSpace()` is then called with the
*dockspace root* ID every frame. (The builder's split calls replace the node
they split, so the "remaining" node ID must be tracked in a separate
variable — passing the root ID as a split out-parameter clobbers it, makes
`DockSpace()` target a child node, and that node keeps the `DockSpace` flag
plus a private, never-again-submitted host window; the pane renders into the
dead window and the process crashes once ImGui's 60 s memory compaction
frees that host's draw-list channels.)

The raw-Vulkan surface follows the official ImGui GLFW+Vulkan example: the
swapchain is recreated when the framebuffer size changes (polled per frame;
a minimized window's frames are skipped entirely) or on
`OUT_OF_DATE`/`SUBOPTIMAL` from acquire/present. The render pass is
format-bound and the format never changes, so it is created once and the
Dear ImGui pipeline compiled against it stays valid across recreates. On
Windows the window frame opts into dark mode (`DwmSetWindowAttribute`) so a
light Windows theme does not put a white title bar over the dark UI.

Modal dialogs (Open/Save As/Pull/About) are Begun inside a small invisible
`##modalhost` window that `ControlGuiApp::frame()` keeps open for the whole
frame, and every `OpenPopup()` call passes the explicit id
`ImHashStr(name, 0, ImHashStr("##modalhost"))`. This is required, not a
style choice: in this Dear ImGui release popup ids are computed as
`currentWindow->GetID(name)` — i.e. seeded by the *current* window's ID
stack — so the same modal name opened from the menu bar and Begun outside
any window yields different ids (the modal silently never opens and its
`bool*` is reset), and `BeginPopupModal()` with no current window is a null
dereference. A top-level window's id is a plain name hash, so the host's
popup id is computable from anywhere; the host must have no `PushID`s.
`OpenPopup()` itself also dereferences the current window, so it may only
run inside a frame (the self-test's modal opens are therefore deferred to
`frame()` via a pending flag).

Crash diagnostics (`src/gui/GuiCrash.cpp`): the GUI thread installs an
unhandled-exception filter that appends a crash report — registers plus a
full Dear ImGui state dump (every window and dock node with flags, host
links and draw-list splitter state) — to `mediamax_control_crash.log` next
to the executable, then shows a message box, so a failure is never silent.
Each frame, `ControlGuiApp::checkDockInvariants()` walks the dock node tree
before `DockSpace()` and appends a line to the same log (and the in-UI
console) whenever a node drifts (foreign/stale host window, broken parent
link, host draw list missing its docking channels), so a docking crash is
always preceded by a diagnostic. `--gui-self-test [frames]` renders the GUI
headless (hidden window) for a fixed number of frames, then opens and
closes every modal dialog (failing if one does not open or does not close);
with `--gui-self-test-system <host:port>` it additionally drives a running
playback engine through the GUI's command path.

## Data flow, one frame

```
clock tick (render thread, under m_StateMutex)
  -> PlaybackEngine.tick(dt)               [cues fire; scene override released at playScene]
  -> PlaybackEngine.frameState(t)          [pure: resolved layers]
  -> media pump has already pushed new     [thread-safe texture pool]
     media frames into SceneRenderer
  -> per display: swapchain.acquire()
  -> SceneRenderer.recordFrame(cmd, {layers, displays})
       uploads -> clear x2 -> vertices -> layer pass -> display pass
  -> submit + present (+ setTextureState Present)
  -> AudioSink.push(mixed audio)
```

Control commands arrive on per-client threads, go through
`CommandServer::handle` (serialized by the server's handler mutex, and
taking the app's state mutex around engine/media/display mutations), and
never touch the render pipeline directly.

## Design invariants (the ones that make the system correct)

1. **Frame state is a pure function of time** (see `playback/` above).
2. **One blend implementation in two places, kept byte-equivalent**
   (`Blend.cpp` reference, `layer.frag` GPU).
3. **Premultiplied scene, straight media, unpremultiply at the display
   edge.** Clear = `(0,0,0,1)`.
4. **Exact time internally; `double` seconds only at the wire/file edges.**
5. **The core never touches the GPU or the network from its engine types**;
   the apps compose the pieces.
6. **Show files and the protocol are versioned, documented formats** —
   changes must keep round-trip tests green and update `docs/`.
