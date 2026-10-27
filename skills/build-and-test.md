# Skill: building and testing MediaMax

Use this playbook whenever you configure, build or test the project, or when
a build/test failure needs diagnosis.

## One-time configure

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```

- Windows: Visual Studio generator. `-DCMAKE_BUILD_TYPE` is honored but the
  configuration is normally selected per-build with `--config Release`.
- Linux: `cmake -S . -B build -G Ninja` works; needs `libvulkan-dev`.
- Configure fetches GLFW, glm, miniaudio, doctest, nlohmann/json and NVRHI
  (plus FFmpeg binaries when `MEDIAMAX_ENABLE_FFMPEG=ON`) via FetchContent.
  The first configure is slow (downloads); later ones are incremental.

## Build

```sh
# everything (all 6 targets)
cmake --build build --config Release -j

# a single target (faster iteration)
cmake --build build --config Release --target mediamax_core_tests
cmake --build build --config Release --target mediamax_render_tests
cmake --build build --config Release --target mediamax_playback
cmake --build build --config Release --target mediamax_control
```

Targets: `mediamax`, `mediamax_render`, `mediamax_playback`,
`mediamax_control`, `mediamax_core_tests`, `mediamax_render_tests`.

## Test

```sh
ctest --test-dir build -C Release --output-on-failure
```

Expected on a machine with a working GPU: **100% tests passed, 2/2** —
`mediamax_core_tests` (138 cases) and `mediamax_render_tests` (12 cases).

On a headless box the render cases print
`skipping: no usable GPU (...)` and still pass; that is correct behavior,
but it means GPU changes were NOT verified there.

## Log handling (Windows PowerShell)

Build output is huge; always funnel it through a log and read the tail:

```powershell
cmake --build build --config Release 2>&1 | Tee-Object build.log | Select-Object -Last 20
ctest --test-dir build -C Release --output-on-failure 2>&1 | Tee-Object ctest.log | Select-Object -Last 30
```

Gotchas:

- **MSVC:** chase the FIRST error only; the cascade is misleading.
- `Tee-Object` + `2>&1` can make ctest's exit code look wrong when it writes
  to stderr — trust the `x/2 tests passed` line in the log, not `$LASTEXITCODE`.
- doctest: `--list-test-cases` works in the render test binary;
  `--test-case <filter>` is broken there (skips everything) — run the full
  binary and grep.
- GPU test runs with a layer (validation/api dump) need the layer env vars
  set in the same process that launches the exe; use a `cmd /c "set ...&& exe"`
  batch form (see `skills/gpu-debugging.md`).

## App smoke tests

```powershell
# playback: must print "self-test: OK - N frames rendered, ..."
build\playback\Release\mediamax_playback.exe --self-test --self-test-seconds 3

# control: must print usage / the show file's systems
build\control\Release\mediamax_control.exe --help
build\control\Release\mediamax_control.exe --show examples\01_basics\show.json --list
```

`--self-test` writes `selftest_*.ppm` next to the exe's working directory
(override with `--self-test-out`). Delete stray `selftest_*.ppm` and
diagnostic `*.log` files from the repo root before committing.

## Typical failure triage

| Symptom | First thing to check |
| --- | --- |
| LNK2019/LNK1181 mentioning `vk*` or nvrhi symbols | The vulkan.hpp unification in `cmake/ThirdParty.cmake` (SDK `find_package(Vulkan)` must run before NVRHI FetchContent; `NVRHI_FETCH_VULKAN_HEADERS OFF`) |
| SIGSEGV right after device creation | Dispatcher: exactly one `vulkan.hpp::Dispatcher` storage (in `render/src/VulkanContext.cpp`) and the 4-arg `VULKAN_HPP_DEFAULT_DISPATCHER.init(...)` |
| NVRHI "Unknown prior state of ..." | The resource's `TextureDesc`/`BufferDesc` is missing `enableAutomaticStateTracking(...)` (or the swapchain image is missing `beginTrackingTextureState`) |
| Black/garbage pixels but no Vulkan errors | Read `skills/gpu-debugging.md` — start with the validation layer, then the api dump |
| Core test failure about image vs video media | `isSingleFrameImageStream()` in `core/src/media/FFmpegSource.cpp` (must be used by both `open()` and `probeFile()`) |
