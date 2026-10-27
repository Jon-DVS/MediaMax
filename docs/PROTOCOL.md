# MediaMax control protocol

The wire protocol between the control interface (`mediamax_control`) and one
or more playback engines (`mediamax_playback`).

## Transport

- TCP, one listener per playback engine (default bind `0.0.0.0`, default
  port **47600**; set with `--host`/`--port`).
- Framing: **one JSON object per line**, `\n`-terminated, UTF-8. Lines that
  are not valid JSON are rejected.
- Connection lifetime: a client may send any number of commands over one
  connection; the server processes them in order and answers each in turn.
  Requests and responses are strictly **1:1** with the `id` echoed. Idle
  connections are **kept open** (the server never drops a quiet client):
  interactive command connections sit between operations for minutes. A
  client whose socket has gone stale (engine restart, network hiccup)
  reconnects transparently — `ControlClient` retries one request after
  re-establishing the connection.

## Envelope (protocol version 1)

Command (client -> engine):

```json
{"v":1, "id":42, "method":"<name>", "params":{ ... }}
```

Response (engine -> client):

```json
{"v":1, "id":42, "ok":true,  "result":{ ... }}
{"v":1, "id":42, "ok":false, "error":"<human readable message>"}
```

- `v` — protocol version (currently `1`).
- `id` — client-chosen integer, echoed back for correlation.
- `params` — object; `{}` when none.
- `result` — object (may be empty) when `ok` is true; `error` is a string
  when `ok` is false. Failures are always structured this way — the server
  never drops a connection because of a bad command.
- Unknown method → `ok:false`, `error:"unknown method: <name>"`.

## Conventions

- **Time** values on the wire are seconds as JSON numbers.
- **Ids** are unsigned integers; `0` means "none" / invalid.
- **Blend modes** are strings: `normal` (default), `add`, `subtract`,
  `multiply`, `screen`.
- **Playback states** are lowercase strings: `stopped`, `playing`, `paused`.

## Command reference

### `ping`

Liveness + identity probe.

- Params: none.
- Result: `{"server":"mediamax", "system":"<name>", "protocolVersion":1}`

### `status`

Full snapshot of the system.

- Params: none.
- Result:

| Field | Meaning |
| --- | --- |
| `system` | system name |
| `state` | `stopped` / `playing` / `paused` |
| `time` | current show time (s) |
| `timecode` | `HH:MM:SS:FF` at the show frame rate |
| `duration` | show duration (s), `-1` when unlimited |
| `frameRate` | broadcast string, e.g. `"30"` or `"29.97"` |
| `scene` | primary (bottom-most) active scene id (`0` when no scene is active) |
| `activeScenes` | all active scene ids, in show-list order (= compositing order) |
| `previewScene` | show's preview scene index (always `0` in v1; not settable via the protocol) |
| `layers` | number of layers in the current frame |
| `media` | array of media objects (below) |
| `displays` | array of display objects (below) |
| `scenes` | `[{"id","name","layers":<count>}]` |

### `system.set`

Rename the system (reflected in `status`/`ping`).

- Params: `name` (string, required).
- Result: `{"system":"<name>"}`

### Shows

#### `show.load`

Load a show file (see [SHOW_FORMAT.md](SHOW_FORMAT.md)) and make it active.
Referenced media files are loaded into the library automatically (file-local
media ids are remapped to library ids); missing media files are logged and
skipped, not fatal. Relative media paths resolve against the show file's
directory (a show is a self-contained folder), not the engine process's
working directory; absolute media paths are used as written.

- Params: `path` (string, required) — path readable by the engine process;
  `preserveState` (bool, optional, default `false`) — see `show.loadJson`.
- Result: `{"name","scenes":<n>,"media":<n>}`

#### `show.loadJson`

Like `show.load`, but the show document is given inline as a JSON object
instead of a file path. This lets an authoring tool (e.g. the control GUI's
show editor) push an edited show to a system without a shared filesystem.

- Params: `show` (object, required) — the complete show document (see
  [SHOW_FORMAT.md](SHOW_FORMAT.md)), as `Show::toJson` produces it;
  `mediaDir` (string, optional) — the directory that relative media paths
  in the document resolve against. The control GUI sends the directory of
  the show file it has open, so a push behaves like a `show.load` of that
  file; when omitted, relative paths are used as written (engine working
  directory);
  `preserveState` (bool, optional, default `false`).
- Result: `{"name","scenes":<n>,"media":<n>}` (same as `show.load`).
- Errors: missing/non-object `show` parameter, an invalid document, or a
  malformed file are reported the same way as for `show.load`. Media
  loading (and skipping of missing media files) is identical.

Without `preserveState`, loading a show resets playback (stopped, time 0,
no scene/layer overrides). With `preserveState: true`, the live playback
state carries over to the new show wherever it still makes sense: the play
state (playing/paused/stopped) is kept, the playhead is kept (clamped to the
new duration), a manual scene override is kept when that scene still exists,
and layer overrides are kept for layers that still exist. This is what the
control GUI's auto-push of live edits uses, so editing a show does not
restart playback on the systems it is pushed to.

#### `show.save`

Write the current show back to a file in the canonical show format. Runtime
overrides (from `scene.set` / `layer.set`) are *not* part of the show model
and are not saved; the timeline and static layer values are.

- Params: `path` (string, required).
- Result: `{"path":"<path>"}`

#### `show.info`

- Params: none.
- Result: `{"name","frameRate","duration" (-1 unlimited),"scenes":[{id,name,layers}],"cues":<n>}`

### Media

Media object shape (used by `media.load`, `media.list`, `status`):

```json
{"id":7, "path":"media/clip.mp4", "type":"video",
 "width":1920, "height":1080, "duration":12.34, "frameRate":"30/1",
 "codec":"h264", "colorspace":"bt709", "bitDepth":8,
 "scanType":"progressive", "bitRate":8000000, "audioCodec":"aac"}
```

`type` is one of `video`, `image`, `audio`, `unknown`.

The codec-detail fields are probed from the container/codec and are
additive (absent or zero/empty means "not reported" — e.g. an image has no
bit rate, a stream without color metadata reports `colorspace` as
`"unknown"`, a source that does not probe (test patterns) leaves them
empty): `codec` / `audioCodec` are the video/audio codec names (e.g.
`h264`, `png`, `aac`), `colorspace` is the video color space (e.g. `bt709`),
`bitDepth` is the video bit depth in bits (e.g. `8`, `10`), `scanType` is
`progressive` / `interlaced`, and `bitRate` is the video bit rate in
bits/second.

#### `media.load`

- Params: `path` (string, required).
- Result: the media object.
- Media is deduplicated by file: loading a path that is already loaded
  returns the existing media object (same `id`, no re-decode) unless the
  file changed on disk (size or last-write time), in which case the source
  is reloaded in place and the same `id` is kept. Loading the same file
  through different spellings of the path (relative vs. absolute, case on
  Windows) still dedupes to one asset.

#### `media.list`

- Params: none.
- Result: `{"media":[ <media object>, ... ]}`

#### `media.unload`

- Params: `id` (int, required).
- Result: `{"id":<id>}`

### Playback

#### `playback.play`

Start or resume. If stopped at the end of a finite show, restarts from 0.

- Params: `at` (string seconds, optional) — seek here before playing.
- Result: `{"state":"playing"}`

#### `playback.pause`

- Params: none.
- Result: `{"state":"paused"}`

#### `playback.stop`

Stop and return to time 0.

- Params: none.
- Result: `{"state":"stopped"}`

#### `playback.seek`

- Params: `time` (number seconds, required).
- Result: `{"time":<actual new time>}`

### Scenes

Several scenes may be active at once (composited, in show-list order — the
first active scene is bottom-most). `scene.set` activates exactly one scene
(solo), `scene.toggle` adds/removes a scene, and `scene.clear` releases the
whole manual set back to the timeline. The operator may also have **no
scene active at any time**: when the manual set is empty (deliberately,
via `scene.toggle`), nothing is composited (black output) and `status`
reports `scene: 0` with an empty `activeScenes` array, until a scene is
toggled back on, `scene.clear` releases, or a `playScene` cue is crossed.

#### `scene.set`

Force exactly one scene immediately (operator solo; takes effect over the
timeline until the timeline's next `playScene` cue, another `scene.set`, or
`scene.clear`). Any other manually active scenes are released.

- Params: `index` (int, required) — 0-based scene index in the show.
- Result: `{"scene":<id>,"name":"<name>","activeScenes":[<id>,...]}`

#### `scene.toggle`

Toggle a scene in/out of the manual active set. Taken from a
timeline-driven state (no manual set yet) it operates on what the timeline
shows: toggling a *different* scene on composites it **with** the timeline
scene, while toggling the timeline scene itself off takes manual control
with an empty set — **no scene is active** (black output). Toggling the
last manually active scene off likewise leaves the manual set in effect
but empty. Each manually activated scene starts its media from its
activation time. The timeline's next `playScene` cue (crossed while
playing) releases the whole manual set, like `scene.clear`.

- Params: `index` (int, required) — 0-based scene index in the show.
- Result: `{"scene":<id>,"name":"<name>","active":<bool>,"activeScenes":[<id>,...]}`
  — `activeScenes` in show-list order (= compositing order, bottom to top);
  empty while the manual set has no active scene.

#### `scene.clear`

Release the operator's manual scene set (from `scene.set`/`scene.toggle`):
scene selection returns to the timeline — including a deliberate empty set
(no scene active), which this releases. Idempotent — clearing when there is
no manual set succeeds and reports `cleared: 0`.

- Params: none.
- Result: `{"cleared":<count>,"scene":<primary scene id after the clear>,"activeScenes":[<id>,...]}`

#### `scene.list`

- Params: none.
- Result:

```json
{"scenes":[
  {"id":3,"name":"Intro","active":true,
   "layers":[{"id":11,"name":"BG","x":0.0,"y":0.0,"override":true},
             {"id":12,"name":"Cam1","override":false}]}
]}
```

`active` is true for every scene in the active set (several may be true at
once). Per layer: `x`/`y` are the position the control surface should
present: the authored (static) transform with timeline cues and manual
overrides applied, but **without keyframe-track animation** — a track is
what the show plays out on screen, not a position the operator has set, so
the value is stable while an animation runs. They are only reported for
layers of *active* scenes that are currently visible (a hidden layer
reports none). `override` is true when any manual override
(opacity/visibility/blend/position) is set for the layer. The control GUI
uses `x`/`y` to keep its position fields in sync with the engine and
`override` to enable the per-layer Clear action.

### Layers

#### `layer.set`

Set one or more runtime overrides for a layer (applied on top of the show's
static values and its keyframe tracks until cleared with `layer.clear` or
the show is reloaded). The layer id must exist in at least one scene of the
loaded show; unknown ids are rejected.

- Params: `layer` (int id, required); any of:
  - `opacity` (number 0..1)
  - `visible` (bool)
  - `blend` (string blend mode)
  - `x` and `y` (numbers, must be given together) — anchor position in
    output-space pixels
- Result: `{"layer":<id>}`

#### `layer.clear`

Remove runtime overrides: one layer's (when `layer` is given) or all of
them (when it is absent).

- Params: `layer` (int id, optional).
- Result: `{"layer":<id>}` for a single layer (fails when it had no
  override), `{"cleared":<n>}` for all.

The control GUI's Layers block (playback pane) applies `layer.set` /
`layer.clear` to **every connected system** at once when an operator
changes a live override (per-system failures are logged in the GUI
console), so the adjustment reaches all players together; the values it
displays keep resyncing from the active system.

### Displays

Display object shape (used by `display.list`, `display.set`, `status`):

```json
{"id":1, "name":"Wall A", "x":0, "y":0,
 "width":1920, "height":1080, "brightness":1.0, "enabled":true,
 "statusOverlay":false}
```

Geometry is in the shared output space (pixels, origin top-left). The union
of enabled display rectangles defines the scene size.

#### `display.add`

Add a display to the system. The playback engine creates a window for it
(titled `MediaMax - <name>`), builds its Vulkan surface and swapchain, and
starts rendering the scene's share of the output wall onto it.

- Params: all optional — `x`, `y` (ints, position in the output space),
  `width`, `height` (ints, default 1920x1080), `name` (string, default
  `"Display <id>"`).
- Defaults: a missing `x` tiles the panel to the right of the current
  output wall (`x` = the wall's right edge, `0` when the wall is empty);
  a missing `y` is `0`.
- Result: the new display object.
- If the engine cannot create the display's window or swapchain, the
  display record is removed again (the system logs the failure).

#### `display.list`

- Params: none.
- Result: `{"displays":[ <display object>, ... ]}`

#### `display.remove`

Remove a display from the system: the engine destroys its window, Vulkan
surface and swapchain, and the scene re-slices across the remaining
displays. Removing the last display leaves the system running without
outputs (audio and control keep working; `display.add` can bring an
output back).

- Params: `display` (int id, required).
- Result: `{"display":<id>}`

#### `display.set`

Update any subset of a display's properties.

- Params: `display` (int id, required); any of `x`, `y`, `width`, `height`
  (ints, applied as one geometry update), `brightness` (number, default 1.0),
  `enabled` (bool), `statusOverlay` (bool, default false).
- Result: the updated display object.

`statusOverlay` toggles the per-display status overlay: when true, the
playback engine draws a small screen-aligned text block over that output
(showing the system IP address, the display name and number, the measured
frame rate and resolution, and the per-frame CPU/GPU render time in ms).
The overlay is display UI — it is not part of the scene and is not affected
by the display's warp.

A geometry update reinitializes the playback engine's display for that
record: the engine's window is moved/resized to the new geometry, the
swapchain is recreated when the size changes, and the scene re-slices to
the new output bounds. Conversely, manually moving or resizing the engine
window writes the window's client geometry back into the record, so the
record stays the single source of truth in both directions.

#### `display.warp.set`

Set the projection warp for a display from a grid of points in
**display-local UV** (0..1). The grid must be square, >= 2x2; the points are
ordered row-major. MediaMax builds a dense RG8 warp texture from the grid;
the display pass remaps every output pixel through it.

- Params: `display` (int id, required), `points` (array of `[u, v]`,
  required).
- Result: `{"display":<id>,"rows":<n>,"cols":<n>}`

An identity warp is the regular grid `[[0,0],[1,0],[0,1],[1,1]]` (2x2,
row-major: top-left, top-right, bottom-left, bottom-right) or any full N x N
grid with every point on its ideal position.

## Examples

```sh
mediamax_control --system wall1 --command "ping"
# -> {"v":1,"id":1,"ok":true,"result":{"server":"mediamax","system":"wall1","protocolVersion":1}}

mediamax_control --system wall1 --command "media.load" '{"path":"media/bg.mp4"}'
# -> {"v":1,"id":2,"ok":true,"result":{"id":1,"path":"media/bg.mp4","type":"video",...}}

mediamax_control --system wall1 --command "layer.set" '{"layer":4,"opacity":0.5}'
# -> {"v":1,"id":3,"ok":true,"result":{"layer":4}}

mediamax_control --system wall1 --command "bogus"
# -> {"v":1,"id":4,"ok":false,"error":"unknown method: bogus"}
```

## Versioning policy

- `v` is the envelope version; changes to the envelope bump it.
- Adding new commands or new optional parameters is non-breaking.
- Changing a command's parameter names/types or result shape is breaking:
  bump the version and keep old behavior available behind the old version.
