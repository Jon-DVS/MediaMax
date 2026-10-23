# Skill: adding a control-protocol command end-to-end

Use this playbook when a feature needs a new command in the control protocol
(e.g. a new `layer.set` field, a new `warp.get`, an `audio.setVolume`).

The protocol is a **versioned wire format** (`"v":1`, see
`core/include/mediamax/protocol/Protocol.h`). New commands are additive and
fine; changing an existing command's parameter or result shape is a breaking
change — do not do it silently.

## The layers (change all of them)

```
mediamax_control (control/src/main.cpp)          — no change needed for new
                                                    methods (it is generic),
                                                    unless you add a CLI flag
core/.../protocol/CommandServer.{h,cpp}          — command implementation
core/.../protocol/CommandServer.h                — add handler decl + method
                                                    to supportedMethods()
core/tests/test_CommandServer.cpp                — tests (required)
docs/PROTOCOL.md                                 — command reference entry
PlaybackEngine / MediaLibrary / DisplayManager   — only if the command needs
                                                    a new engine capability
```

## Step-by-step

1. **Add the capability in the right engine type** if the command needs one
   (e.g. a new `PlaybackEngine` method). Keep the engine headless and make the
   method a pure state transition where possible (see the existing
   `setLayerOpacity`/`setSceneNow` pattern: returns `bool`, validated).

2. **Declare the handler** in `CommandServer.h`:
   ```cpp
   nlohmann::json handleFoo(const nlohmann::json& params) const;
   ```
   Stateful handlers may mutate via `mutable` members (see
   `m_SystemName`), mirroring how `system.set` works.

3. **Implement in `CommandServer.cpp`:**
   - Add `"foo"` (dot-notation like the others: `area.verb`) to
     `supportedMethods()` — the control app and tests use it.
   - Add the dispatch `else if (cmd.method == "foo")` in `handle()`.
   - In the handler, extract parameters with the existing helpers:
     `requireString`, `requireNumber`, `requireInt`, `optionalString`.
     They throw `ParameterError` with a message that becomes the response
     `error` field — never throw raw exceptions from a handler, and never
     return an ad-hoc error shape: all failures flow through
     `Response::failure`.
   - Return the result JSON object. Keep result shapes stable and
     self-describing (reuse the existing `mediaToJson`/`displayToJson`
     helpers when returning assets/displays).

4. **Test in `core/tests/test_CommandServer.cpp`.** This file builds a real
   `CommandServer` over an in-memory `PlaybackEngine` + `MediaLibrary` +
   `DisplayManager` — no network, no GPU. Cover at least:
   - happy path (result shape asserted field-by-field),
   - missing/invalid parameter (expect `ok=false` and a useful `error`),
   - unknown id / out-of-range index (expect `ok=false`),
   - and the interaction with existing commands (e.g. a new override that a
     cue or a later command observes).
   Also keep `supportedMethods()` covered — there is a test asserting the
   full method list.

5. **Document in `docs/PROTOCOL.md`:** a row/section with the method name,
   each parameter (name, type, required/optional, meaning) and the result
   shape, plus a one-line example. If you added a CLI convenience for it in
   the control app, mention it there too.

6. **Build + test + smoke:**
   ```powershell
   cmake --build build --config Release -j
   ctest --test-dir build -C Release --output-on-failure
   build\control\Release\mediamax_control.exe --help
   ```

## Wire-format reminders

- One JSON object per line, `\n`-terminated, UTF-8.
- Command: `{"v":1,"id":<int>,"method":"<name>","params":{...}}`
- Response: `{"v":1,"id":<int>,"ok":true,"result":{...}}` or
  `{"v":1,"id":<int>,"ok":false,"error":"<message>"}`
- `id` is echoed 1:1; requests and responses are strictly paired (no
  server-push today — if you add events, version-bump and design a distinct
  envelope).
- Time values on the wire are **seconds as JSON numbers**; the core converts
  through `Time::fromSeconds` (the documented precision boundary).
- `Id` values are JSON unsigned integers; `0` means "none".
