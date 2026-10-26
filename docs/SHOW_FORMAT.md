# MediaMax show file format

A show file is a human-readable JSON document describing a complete media
presentation: its frame rate, the media it uses, its scenes (layer
stacks) and its cue timeline. It is the interchange format between the
control interface, the playback engine and any external authoring tool.

Reference implementation: `Show::toJson` / `Show::fromJson`
(`core/src/show/Show.cpp`), which must stay round-trippable (covered by
`core/tests/test_Show.cpp`).

## Document envelope

```json
{
  "app": "mediamax-show",
  "version": 1,
  "name": "My Show",
  "frameRate": { "num": 30, "den": 1 },
  "duration": 30.0,
  "media": [ ... ],
  "scenes": [ ... ],
  "cues": [ ... ]
}
```

| Field | Type | Required | Meaning |
| --- | --- | --- | --- |
| `app` | string | yes | Must be exactly `"mediamax-show"` (compatibility gate) |
| `version` | int | yes | Format version; only `1` is currently accepted |
| `name` | string | no | Show name (default `"Untitled Show"`) |
| `frameRate` | object | no | `{"num": <int>, "den": <int>}` — reduced rational, both > 0. Default 30/1 |
| `duration` | number | no | Show duration in **seconds**. `0` (default) = unlimited: the show runs until a `stop` cue (or forever) |
| `media` | array | no | Media references (below) |
| `scenes` | array | yes* | Scenes (below) — a show without scenes is empty |
| `cues` | array | no | Timeline cues (below) |

\* "required" in the sense that an empty show is valid but useless; the
parser accepts all lists as empty.

**All times in the file are seconds (JSON numbers).** Internally the engine
converts them through `Time::fromSeconds` (nearest microsecond) — that is
the documented precision boundary; frame-exact math happens on exact
rationals afterwards.

## Media references

```json
"media": [
  { "id": 1, "path": "media/bg.mp4" },
  { "id": 2, "path": "media/logo.png" }
]
```

- `id` — unsigned int, unique within the file. **File-local**: when the
  engine loads the show it loads each file and remaps these ids to its own
  media-library ids (layers referring to the same id are remapped too).
- `path` — path relative to (or absolute for) the engine process's working
  directory, readable by the engine.

Supported media: video containers (mp4/mov/...), images (png/jpg/bmp/tiff/
webp/gif — still images, or GIF as animation), audio.

## Scenes

```json
"scenes": [
  { "id": 1, "name": "Main", "layers": [ <layer>, <layer> ] },
  { "id": 2, "name": "Fallback", "layers": [ <layer> ] }
]
```

- `id` — unsigned int, unique within the file.
- `name` — display name.
- `layers` — the layer stack. Layers are composited **bottom to top by
  `zIndex`** (higher `zIndex` drawn on top; equal indices keep file order —
  stable sort).

Usually one scene is active at a time; cues (and the operator) switch
between them. Several scenes may also be active at once (the operator's
`scene.toggle` from the control interface, see
[PROTOCOL.md](PROTOCOL.md)): all active scenes are composited together,
**in the order they appear in the `scenes` list** (first active scene
bottom-most, then each scene's layers by `zIndex`). Reordering the list
therefore reorders the compositing. Cues reference scenes by `id`, so
reordering does not affect the timeline.

### Layer

```json
{
  "id": 11,
  "name": "Background",
  "mediaId": 1,
  "color": { "r": 0.1, "g": 0.1, "b": 0.1, "a": 1.0 },
  "inPoint": 0.0,
  "outPoint": 0.0,
  "loop": false,
  "rate": 1.0,
  "transform": {
    "x": 960.0, "y": 540.0,
    "rotation": 0.0,
    "scaleX": 1.0, "scaleY": 1.0,
    "anchorX": 0.5, "anchorY": 0.5
  },
  "opacity": 1.0,
  "blend": "normal",
  "visible": true,
  "zIndex": 0,
  "tracks": { ... }
}
```

| Field | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | uint | — (required) | Unique within the file |
| `name` | string | `""` | Display name |
| `mediaId` | uint | `0` | Media reference id. **`0` = solid-color layer** (uses `color`) |
| `color` | `{r,g,b,a}` 0..1 | `{0,0,0,1}` | Color for color layers (ignored when `mediaId` is set) |
| `inPoint` | seconds | `0` | Start position within the media's own timeline |
| `outPoint` | seconds | `0` | End position within the media. **`0` = "to end of media"**. Not validated against `inPoint`: an end bound only exists when `outPoint > inPoint` (a non-zero `outPoint <= inPoint` disables the bound, so `mediaTime` runs unbounded) |
| `loop` | bool | `false` | When the out point is reached, loop back to the in point. Without loop the layer **holds its last frame** |
| `rate` | number > 0 | `1.0` | Playback speed multiplier within the media |
| `transform` | object | identity | Position/rotation/scale (below) |
| `opacity` | number 0..1 | `1.0` | Layer opacity (straight-alpha scale) |
| `blend` | string | `"normal"` | `normal`, `add`, `subtract`, `multiply`, `screen` |
| `visible` | bool | `true` | Hide the layer entirely |
| `zIndex` | int | `0` | Stacking order, higher = on top |
| `tracks` | object | absent | Animation tracks (below) |

### Transform

```json
"transform": { "x":960, "y":540, "rotation":0.3, "scaleX":2, "scaleY":2,
               "anchorX":0.5, "anchorY":0.5 }
```

All fields optional. Semantics (`mediamax::Transform2D`):

- The media (W x H px) is scaled to `(scaleX*W, scaleY*H)`.
- It is rotated `rotation` radians **clockwise** (output space has y down)
  around the anchor point.
- `anchorX/anchorY` are in **media UV** (0..1; default center).
- The rotated media is placed so the anchor lands at `(x, y)` in output
  space (pixels, origin top-left — the shared wall space, not per-display).

### Tracks (keyframe animation)

Optional. When a track is non-empty it overrides the corresponding static
field for the whole show timeline; when absent/empty the static field is
used.

```json
"tracks": {
  "opacity":  [ {"time":0.0, "value":1.0, "interpolation":"linear"},
                {"time":2.0, "value":0.0, "interpolation":"linear"} ],
  "x":        [ {"time":0.0, "value":100.0, "interpolation":"step"},
                {"time":5.0, "value":200.0, "interpolation":"step"} ],
  "y":        [ ... ],
  "rotation": [ ... ],
  "scale":    [ ... ]
}
```

- Track names: `opacity`, `x`, `y`, `rotation`, `scale` (uniform scale for
  both axes). Any subset may be present.
- Each keyframe: `time` (seconds, show time), `value` (number — for
  `scale`, a uniform factor; for `rotation`, radians), `interpolation`:
  `"linear"` (default) or `"step"`.
- Evaluation is a pure function of show time: hold the first keyframe's
  value before it, hold the last after it, interpolate between.

## Cues (timeline)

```json
"cues": [
  { "time": 0.0,  "name": "start",  "action": { "type": "playScene", "scene": 1 } },
  { "time": 12.0, "name": "fade",   "action": { "type": "setLayerOpacity", "layer": 12, "value": 0.0 } },
  { "time": 30.0, "name": "end",    "action": { "type": "stop" } }
]
```

- `time` — seconds on the show timeline (required).
- `name` — label (optional, for humans/tools).
- `action` — object with a `type` discriminator (required).

**Evaluation semantics (the core of the timeline model):** for a show time
`t`, each cue kind contributes the **last cue of that kind at or before
`t`**. The frame at `t` is a pure function of `(show, t, overrides)` —
seeking backward or forward always lands on the same state. Cues in the
file do not need to be sorted (the parser sorts them); a cue added at an
existing time replaces it.

### Cue actions

| `type` | Fields | Meaning |
| --- | --- | --- |
| `playScene` | `scene` (uint id) | Switch the active scene |
| `stop` | — | Stop playback at this time (playhead stays at the cue time, state `Stopped`) |
| `pause` | — | Pause playback at this time |
| `seek` | `time` (seconds) | Jump the show clock to `time` |
| `setLayerOpacity` | `layer` (uint id), `value` (0..1) | Layer opacity override from this time on |
| `setLayerVisible` | `layer` (uint id), `value` (bool) | Layer visibility override |
| `setLayerBlend` | `layer` (uint id), `mode` (blend string) | Layer blend-mode override |
| `setLayerPosition` | `layer` (uint id), `x`, `y` (pixels) | Layer anchor position override |

A cue action overrides a layer's *static* value and *tracks* for all later
times until another cue of the same kind targets the same layer.

## Minimal example

A 30 fps show: a solid-blue background and a looping logo, two scenes, the
scene switch at 5 s, stop at 10 s.

```json
{
  "app": "mediamax-show",
  "version": 1,
  "name": "Minimal",
  "frameRate": { "num": 30, "den": 1 },
  "duration": 10.0,
  "media": [ { "id": 1, "path": "media/logo.png" } ],
  "scenes": [
    {
      "id": 1,
      "name": "Main",
      "layers": [
        {
          "id": 10,
          "name": "BG",
          "mediaId": 0,
          "color": { "r": 0.0, "g": 0.0, "b": 0.5, "a": 1.0 },
          "transform": { "x": 960.0, "y": 540.0, "scaleX": 1920.0, "scaleY": 1080.0 },
          "zIndex": 0
        },
        {
          "id": 11,
          "name": "Logo",
          "mediaId": 1,
          "loop": true,
          "transform": { "x": 960.0, "y": 540.0 },
          "blend": "screen",
          "zIndex": 1
        }
      ]
    },
    { "id": 2, "name": "Blank", "layers": [] }
  ],
  "cues": [
    { "time": 0.0,  "action": { "type": "playScene", "scene": 1 } },
    { "time": 5.0,  "action": { "type": "playScene", "scene": 2 } },
    { "time": 10.0, "action": { "type": "stop" } }
  ]
}
```

Notes on the example: the color layer uses `mediaId: 0` and scales its
(unit-square) color quad to 1920x1080; a media layer keeps its native pixel
size unless `scaleX/scaleY` say otherwise; the logo layer is centered on the
wall (960, 540) with anchor at its center.

## Compatibility policy

- `app` + `version` are the compatibility gate; unknown versions are
  rejected with an error.
- Adding **optional** fields with defaults is non-breaking (the parser
  ignores unknown fields).
- Renaming/removing fields or changing a default is breaking: bump
  `version`.
