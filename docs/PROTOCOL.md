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
| `scene` | active scene id |
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
skipped, not fatal.

- Params: `path` (string, required) — path readable by the engine process.
- Result: `{"name","scenes":<n>,"media":<n>}`

#### `show.loadJson`

Like `show.load`, but the show document is given inline as a JSON object
instead of a file path. This lets an authoring tool (e.g. the control GUI's
show editor) push an edited show to a system without a shared filesystem.

- Params: `show` (object, required) — the complete show document (see
  [SHOW_FORMAT.md](SHOW_FORMAT.md)), as `Show::toJson` produces it.
- Result: `{"name","scenes":<n>,"media":<n>}` (same as `show.load`).
- Errors: missing/non-object `show` parameter, an invalid document, or a
  malformed file are reported the same way as for `show.load`. Media
  loading (and skipping of missing media files) is identical.

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
 "width":1920, "height":1080, "duration":12.34, "frameRate":"30/1"}
```

`type` is one of `video`, `image`, `audio`, `unknown`.

#### `media.load`

- Params: `path` (string, required).
- Result: the media object.

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

#### `scene.set`

Force a scene immediately (operator override; takes effect over the
timeline until the timeline's next `playScene` cue or another `scene.set`).

- Params: `index` (int, required) — 0-based scene index in the show.
- Result: `{"scene":<id>,"name":"<name>"}`

#### `scene.clear`

Release the operator's scene override (from `scene.set`): scene selection
returns to the timeline. Idempotent — clearing when there is no override
succeeds and reports `cleared: 0`.

- Params: none.
- Result: `{"cleared":<0|1>,"scene":<active scene id after the clear>}`

#### `scene.list`

- Params: none.
- Result:

```json
{"scenes":[
  {"id":3,"name":"Intro","active":true,
   "layers":[{"id":11,"name":"BG"},{"id":12,"name":"Cam1"}]}
]}
```

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

### Displays

Display object shape (used by `display.list`, `display.set`, `status`):

```json
{"id":1, "name":"Wall A", "x":0, "y":0,
 "width":1920, "height":1080, "brightness":1.0, "enabled":true}
```

Geometry is in the shared output space (pixels, origin top-left). The union
of enabled display rectangles defines the scene size.

#### `display.list`

- Params: none.
- Result: `{"displays":[ <display object>, ... ]}`

#### `display.set`

Update any subset of a display's properties.

- Params: `display` (int id, required); any of `x`, `y`, `width`, `height`
  (ints, applied as one geometry update), `brightness` (number, default 1.0),
  `enabled` (bool).
- Result: the updated display object.

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
