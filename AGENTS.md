# AGENTS.md — MediaMax development guidelines

Guidelines for AI agents (and humans) working in this repository. Read this
file before making changes; it encodes the project's conventions and the
hard-won gotchas.

## Ground rules

1. **Work autonomously.** Ask only when a decision is genuinely ambiguous
   and material.
2. **Do not pull code from outside this working directory** into the
   implementation. Reading public specs/docs on the web is fine; copy-pasting
   external projects is not.
3. **Production grade, not a hack.** Every behavior you add needs tests where
   the logic is testable; public APIs get doc comments; error paths return
   clean errors (no crashes, no `assert` on user input).
4. **Keep the core headless.** `mediamax` (the core static library) must never
   touch the GPU, create windows or open sockets in its *engine* types. The
   protocol types are data-only; network I/O lives in `net/` and is driven by
   the apps. This is what makes the core unit-testable on any machine.
5. **Code review before commit.** Build + run all tests, review the diff for
   style/quality, then commit and push.

## Repository layout

```
core/      headless engine (static lib `mediamax`) + tests
render/    Vulkan/NVRHI renderer (static lib `mediamax_render`) + GPU tests
playback/  `mediamax_playback` executable
control/   `mediamax_control` executable (CLI/REPL + gui/ Dear ImGui app)
examples/  self-contained example shows + generated media (see examples/README.md)
third_party/ vendored third-party sources (ImGuiFileDialog, MIT - used only
             by the control GUI, compiled as its own lib at default /W3)
cmake/     build helpers; docs/ for the reference docs; skills/ for playbooks
```

Key entry points:

- `core/include/mediamax/time/Time.h` — exact rational time
- `core/include/mediamax/playback/PlaybackEngine.h` — the pure frame function
- `core/include/mediamax/show/` — Show / Scene / Layer / Cue / KeyframeTrack
- `core/include/mediamax/protocol/` — wire protocol + CommandServer
- `core/include/mediamax/media/` — IMediaSource, MediaLibrary, FFmpeg source
- `render/include/mediamax/render/` — VulkanContext, Swapchain, SceneRenderer
- `core/src/blend/Blend.cpp` + `render/shaders/layer.frag` — the two blend
  implementations that MUST stay in sync (see "Critical invariants")

## Build and test (the loop you must run)

Configure (once):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```

Build everything (or a single target):

```sh
cmake --build build --config Release -j
cmake --build build --config Release --target mediamax_render_tests
```

Collect the final output into a single folder — `build/dist/` with both
executables, their runtime DLLs (FFmpeg, the Vulkan loader when the SDK
ships one) and the example shows, ready to copy to another machine
(`cmake/Deploy.cmake`):

```sh
cmake --build build --config Release --target mediamax_dist
```

Run the full suite:

```sh
ctest --test-dir build -C Release --output-on-failure
```

- 180+ core cases and 16 GPU cases. The GPU cases self-skip (with a message)
  when no usable Vulkan device exists; a green ctest on a GPU machine is the
  bar before committing render changes.
- doctest note: the `--test-case` filter in `mediamax_render_tests` is
  unreliable — run the whole binary and grep its output instead.

Smoke-test the app:

```sh
build/playback/Release/mediamax_playback.exe --self-test --self-test-seconds 3
# must end with: self-test: OK - N frames rendered, ...
build/control/Release/mediamax_control.exe --help
build/control/Release/mediamax_control.exe --gui-self-test 90
# hidden window, 90 full UI frames; exit 0 = OK. With
# --gui-self-test-system <host:port> it also drives a running playback
# engine through the GUI's command path (replaces that engine's show).
```

MSVC workflow notes:

- Chase the **first** compiler error; MSVC cascades are enormous.
- Build large logs to a file and read the tail, e.g.
  `cmake --build build --config Release 2>&1 | Tee-Object build.log | Select-Object -Last 20`.
- The third-party doctest/miniaudio C5285/C4244 warnings are known-accepted;
  do not "fix" warnings inside `build/_deps` or `third_party/` (the
  vendored code compiles at the default warning level on purpose).

## Coding conventions

- C++20, no C++23. `#pragma once` on headers.
- Format: the repo's `.clang-format` (LLVM base, 120 cols, 4-space indent).
  Match the surrounding code exactly when in doubt.
- Naming: `PascalCase` types/functions-in-headers-are-mixed — follow the
  existing style per file: free functions `camelCase` in `mediamax` core
  (e.g. `blend::apply`), `PascalCase` for classes and methods
  (`PlaybackEngine::frameState`). Member variables `m_CamelCase`.
  Constants `kCamelCase`. Enum values `PascalCase`.
- Headers: include what you use; keep the public core headers free of
  implementation dependencies.
- No exceptions across public API boundaries: engine/protocol APIs return
  `bool`/`std::optional` + `std::string& error` (or, for the protocol,
  structured failure responses). Exceptions are fine *inside* private code
  where they are caught (see `CommandServer::handle`).
- Logging: `mediamax::Logger` (fmt-style `{}` placeholders). Never `printf`.
- `Id` is `uint32_t`; `0` (`kInvalidId`) means "none".
- Time: use `Time` (exact rational seconds) for anything internal. `double`
  seconds are only acceptable at the very edges (protocol JSON, file I/O) —
  `Time::fromSeconds` is the documented precision boundary.
- Threads: the render (main) thread, the media pump thread, the audio
  callback and the per-client control threads are separate. `MediaLibrary`
  is *not* internally synchronized: access to it is serialized by the
  app's state mutex (the pump is handed the same mutex). Cross-thread
  frames go through `SceneRenderer`'s upload queue and `MediaPump`'s
  targets/channels locks. Do not introduce cross-thread raw pointer
  sharing without a clear owner + refcount (NVRHI handles are refcounted;
  keep them in `RefCountPtr`/`Handle` types).

## Critical invariants (breaking these causes subtle bugs)

1. **Frame state is a pure function of time.**
   `PlaybackEngine::frameState(t)` must not depend on playback history.
   Cues are evaluated as "the last cue of each kind at or before t". If you
   change cue semantics, update `PlaybackEngine`, the show tests and
   `docs/SHOW_FORMAT.md` together.
2. **CPU and GPU blend must match exactly.**
   `core/src/blend/Blend.cpp` is the reference (premultiplied-space
   equations); `render/shaders/layer.frag` re-implements the same equations
   for the GPU. Change one, change the other, and extend
   `render/tests/test_Render.cpp` (it compares GPU pixels to
   `blend::apply` for every blend mode).
3. **Scene RTs are premultiplied alpha; media is straight alpha; the display
   pass unpremultiplies.** The clear value is `(0,0,0,1)`. `readBackScene`
   unpremultiplies on read. Don't "normalize" this without updating both
   shader and reference.
4. **Show files are the interchange format.** Any show-model change must keep
   `Show::toJson/fromJson` round-trippable (tested) and be reflected in
   `docs/SHOW_FORMAT.md`. The `app`/`version` fields are the compatibility
   gate — bump `version` for breaking changes.
5. **The protocol is versioned wire format** (`"v":1`). New commands are
   additive; changing a result shape is a breaking change. Document every
   command in `docs/PROTOCOL.md` and add `CommandServer` tests
   (`core/tests/test_CommandServer.cpp`).

## NVRHI/Vulkan gotchas (learned the hard way — do not relearn)

- **One vulkan.hpp for the whole build = the SDK's.**
  `cmake/ThirdParty.cmake` runs `find_package(Vulkan)` *before* NVRHI's
  FetchContent and sets `NVRHI_FETCH_VULKAN_HEADERS OFF` so NVRHI uses the
  SDK `Vulkan::Headers` target. Two different vulkan.hpp versions in one link
  give cryptic SIGSEGVs/dispatcher mismatches.
- **NVRHI places sampler descriptors at binding `128 + slot` by default**
  (the DXC register-space convention). Our GLSL declares samplers explicitly
  (`layout(set=0, binding=2)`), so every `BindingLayoutDesc` must call
  `setBindingOffsets(nvrhi::VulkanBindingOffsets().setSamplerOffset(0))`.
  Without it, the shader samples from an unbound descriptor and **silently
  gets zeros** — the validation layer does *not* catch this.
- **NVRHI render passes always use `loadOp = LOAD`** (not configurable).
  Anything not covered by a draw keeps the target's previous content. The
  scene renderer therefore clears *both* ping-pong targets at the start of
  every frame. If you add a new render target that a pass writes to, clear
  (or otherwise define) it first.
- **Push constants must be set after `setGraphicsState`** in the same
  command list — NVRHI records `vkCmdPushConstants` immediately with the
  currently bound pipeline layout, which is null until the first
  `setGraphicsState` of that command list.
- **Tell NVRHI the invariant state of textures/buffers it will use across
  command lists** via `enableAutomaticStateTracking(...)` in the
  `TextureDesc`/`BufferDesc`, or call
  `beginTrackingTextureState(...)` (swapchain images: `Present`). Missing
  tracking yields "Unknown prior state" errors and, on some drivers, missing
  barriers.
- The app owns the vk-hpp default dispatcher: each final binary defines it
  exactly once — `render/src/VulkanContext.cpp` for everything that links
  `mediamax_render` (playback, render tests) and
  `control/src/gui/GuiVulkan.cpp` for the control GUI (which links no NVRHI,
  so it keeps a minimal raw-Vulkan context of its own). Both initialize it
  with the 4-arg form (`instance, vkGetInstanceProcAddr, device,
  vkGetDeviceProcAddr`) after device creation.
- **The SDK headers (1.4.363) have drifted from the classic spec — check
  `vulkan/vulkan_core.h` before touching any raw Vulkan struct.**
  (1) `VkImageSubresourceRange` carries a **5th field, `baseArrayLayer`,
  between `levelCount` and `layerCount`** — a 4-value brace-init silently
  becomes `baseArrayLayer=1, layerCount=0` → invalid image view → *GPU
  fault* (`nvlddmkm` Event 153 in the System log, then
  `VK_ERROR_DEVICE_LOST` from every later call; the validation layer reports
  it at `vkCreateImageView`). Initialize the fields individually.
  (2) `VkPresentInfoKHR` has **no `fence` member** — the present fence goes
  through a pNext `VkSwapchainPresentFenceInfoKHR`, which requires the
  *device* ext `VK_KHR_swapchain_maintenance1` + the `swapchainMaintenance1`
  feature (via `VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR` chained on
  `VkPhysicalDeviceFeatures2`); that device ext in turn requires the
  *instance* exts `VK_KHR_surface_maintenance1` ←
  `VK_KHR_get_surface_capabilities2`. (3) `VkSurfaceCreateInfoKHR` /
  `vkCreateSurfaceKHR` are absent from the C headers — create the window
  surface with `glfwCreateWindowSurface`. `control/src/gui/GuiVulkan.cpp`
  is the reference for all of this (plus per-swapchain-image
  `VkFramebuffer`s, which are required, not optional).
- GPU debugging tools: the SDK ships `VkLayer_khronos_validation` (set
  `VK_LAYER_PATH=C:\VulkanSDK\<ver>\Bin` and
  `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`) and
  `VK_LAYER_LUNARG_api_dump` (dumps every API call with parameters to
  stdout). See `skills/gpu-debugging.md`.

## FFmpeg notes

- FFmpeg 9: wrap all FFmpeg headers in `extern "C"` in C++ translation units.
- **FFmpeg 9 renamed/moved APIs the classic docs remember.** `AVCodecParameters`
  uses `color_space` (not `colorspace`), `format` is a plain `int` (cast to
  `AVPixelFormat` for `av_pix_fmt_desc_get`), the field-order enum values are
  `AV_FIELD_*` (not `AVFIELD_*`, see `libavcodec/defs.h`), and `AVStream` no
  longer carries `bit_rate` (it moved to `AVCodecParameters::bit_rate`; the
  container's overall rate is `AVFormatContext::bit_rate`). `av_color_space_name`
  lives in `libavutil/pixdesc.h`. Check the fetched headers
  (`build/_deps/mediamax-ffmpeg/include`) before using any FFmpeg struct field.
- MSVC link step chokes on `%3B`-encoded multi-lib dependency lists — the
  `cmake/FFmpeg.cmake` IMPORTED targets are one-per-library + an INTERFACE
  umbrella; keep it that way.
- **Image classification:** standalone image files are read by per-format
  `*_pipe` demuxers (`bmp_pipe`, `png_pipe`, ...) which report neither frame
  count nor duration. `isSingleFrameImageStream()` in
  `core/src/media/FFmpegSource.cpp` is the single place that classifies
  image vs video — it is used by *both* `open()` and `probeFile()`; keep it
  that way.
- **One demuxer, two pumps, a parking lot.** `pumpVideo`/`pumpAudio` pull
  alternately from the same `AVFormatContext`. A pump must *park* the other
  stream's packets (`m_ParkedPackets`, drained first, cleared in
  `flushDecoders` and the destructor) — dropping them punches holes in the
  decoded audio and A/V drifts until the next seek. Never "simplify" the
  lot away; the regression test is
  `FFmpeg: alternating video/audio pulls keep the audio continuous`.
- The tests use the FFmpeg binaries fetched into
  `build/_deps/mediamax-ffmpeg/bin` (ffprobe/ffmpeg) to generate test media.

## Playback app gotchas (windowed mode — not covered by the self-test)

- **miniaudio structs must outlive the worker thread.** `AudioSink::start()`
  heap-allocates the `ma_context`/`ma_device` (owned by the unique_ptr
  members; `stop()` uninits then deletes). Never make them stack-locals:
  the worker thread runs on `&device` forever, so stack scope death is a
  use-after-scope (the state field gets clobbered by main-thread stack
  reuse → the worker breaks out of its loop into the stop path and AVs),
  and the unique_ptr would `delete` a stack address in `stop()`.
- **`SceneRenderer` has a null device until `SceneRenderer::create()` is
  called.** `PlaybackApp::initVulkan()` does this (phase 4, before the
  frame loop); the self-test builds its own renderer. `setSceneSize()` on
  an uncreated renderer is a null dereference (only `recordFrame()` is
  null-guarded).
- **Silent crash diagnosis (no console, no debugger attached):** the
  Windows Application event log records every crash —
  `Get-WinEvent -LogName Application` (provider "Application Error") gives
  the faulting module + offset (RVA). Symbolize with a debug build: add
  `/Zi` + `/DEBUG:FULL` (globally, so the static libraries carry symbols
  too — an exe-only PDB shows big "holes" in .text), rebuild, and run the
  app under `cdb -g -c ".ecxr; ~*kv; q"` for a symbolicated stack of all
  threads on the exception.

## Definition of done

A change is done when:

1. `cmake --build build --config Release` is clean (no new warnings).
2. `ctest --test-dir build -C Release --output-on-failure` passes (on a GPU
   machine, including the 12 render cases).
3. Where the change is in the app layer, the relevant smoke command works
   (`--self-test` for playback, `--help`/`--list` for control, and
   `--gui-self-test 90` for control GUI changes).
4. Docs are updated when behavior/protocol/format changed.
5. The diff was self-reviewed against the conventions above.
6. `git add` (explicit paths), `git commit` (conventional message),
   `git push origin main`.

## Skills (task playbooks)

- `skills/build-and-test.md` — configure/build/test matrix, target names,
  log handling.
- `skills/gpu-debugging.md` — pixel-level debugging of the render path
  (validation layer, api dump, readback patterns, the fingerprint technique).
- `skills/extending-protocol.md` — adding a control-protocol command
  end-to-end (CommandServer + tests + control app + docs).
