# Skill: debugging the GPU render path

Use this playbook when `mediamax_render_tests` fail on pixels, or the
playback engine renders black/garbage. The core suite is CPU-only; everything
in this file is about the Vulkan/NVRHI path in `render/`.

## The test harness

`render/tests/test_Render.cpp`:

- Creates one shared `VulkanContext` + `SceneRenderer` (256x256 scene) via
  `gpu()`; each case either composes a `FrameInput` (layers and/or display
  targets) and calls `renderOneFrame()`, or builds its own context for
  isolation.
- `readBackScene()` copies the final ping-pong target back to the CPU,
  **unpremultiplies** (scene RTs are premultiplied alpha) and reports
  straight RGBA8. `readBackTexture()` is the same for arbitrary textures.
- Cases compare GPU pixels against `blend::apply` (the CPU reference) with a
  small tolerance.

## Step 1 — run under the validation layer

```powershell
cmd /c "set VK_LAYER_PATH=C:\VulkanSDK\1.4.363.0\Bin&& set VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation&& build\render\tests\Release\mediamax_render_tests.exe > val_out.txt 2> val_err.txt"
```

(Use your installed SDK version; the layer is *not* registry-registered on
this machine — `VK_LAYER_PATH` is mandatory.)

A clean stream means the command stream is spec-valid: no missing barriers,
no bad layouts, no descriptor *binding* errors. Note what it does NOT catch
(see "Known blind spot" below).

## Step 2 — dump the actual API calls

```powershell
cmd /c "set VK_LAYER_PATH=C:\VulkanSDK\1.4.363.0\Bin&& set VK_INSTANCE_LAYERS=VK_LAYER_LUNARG_api_dump&& build\render\tests\Release\mediamax_render_tests.exe > apidump.txt 2> api_err.txt"
```

The layer (name is `VK_LAYER_LUNARG_api_dump`, **not** khronos) prints every
Vulkan call with full parameters to stdout. Grep for the interesting region:

```powershell
Select-String -Path apidump.txt -Pattern "vkCmdPushConstants|vkCmdBeginRendering|vkCmdDraw|vkUpdateDescriptorSets|vkCreateDescriptorSetLayout|vkCreatePipelineLayout"
```

Read the calls *in order* around your draw: barriers (old/new layout),
`vkCmdBeginRendering` (attachment imageView + loadOp), descriptor set writes
(binding numbers! types!), `vkCmdBindDescriptorSets`, `vkCmdPushConstants`
(layout, stageFlags, offset, size), `vkCmdDraw`. The truth is always in this
stream — stop theorizing once you have it.

## Step 3 — reason about the pixel math

The compositing model (see `render/shaders/layer.frag` and
`core/src/blend/Blend.cpp`):

- Cleared scene = premultiplied `(0,0,0,1)`.
- Layer i: `s_pm = src.rgb * sa` (straight src, `sa = clamp(src.a * opacity)`);
  `out_pm = f(s_pm, d_pm, sa, mode)`; `a = sa + da * (1 - sa)`.
- Readback unpremultiplies by the final alpha. **An alpha error rescales the
  RGB you read back** — e.g. a correct premultiplied `(32,64,95, a=128)`
  reads back as `(64,128,190)`. When a color looks "doubled", suspect the
  alpha channel / destination alpha, not the RGB path.

### The fingerprint technique

To find out what alpha the GPU actually used for a given draw, render over an
**opaque black** previous scene (Normal blend): the readback RGB equals
`src.rgb * sa_gpu` (because `d_pm.rgb = 0` and final alpha = 1), so `sa_gpu`
is directly observable as `readback / src.rgb`. Vary one layer's alpha
(0.5 / 1.0 / 0.0) and the draw count (1-layer vs 2-layer frames) to
disambiguate which draw's push constants the GPU saw.

## Known blind spot (real incident — remember this one)

**The validation layer does not flag a shader descriptor binding that is
missing from the pipeline's descriptor set layout.** The bug: NVRHI's default
`VulkanBindingOffsets` puts samplers at binding `128 + slot`; the GLSL
declared the sampler at `binding = 2`; the set layout had bindings
`{0: image, 1: image, 130: sampler}`. The pipeline was created, the layer
stayed silent, and **every texture sample returned (0,0,0,0)** — which
looked, after the readback unpremultiply, exactly like "the wrong alpha in
the push constants".

Fix (already in the code): every `BindingLayoutDesc` in
`render/src/SceneRenderer.cpp` calls
`setBindingOffsets(nvrhi::VulkanBindingOffsets().setSamplerOffset(0))`.

General rule: **if all texture samples are zero but push constants and
vertex data are verifiably correct (check them in the api dump!), compare
the SPIR-V descriptor interface against the set layout in the dump.**
Disassemble the SPIR-V with the SDK's `spirv-dis`
(`build/render/shaders/*.spv` exist after a build):

```powershell
C:\VulkanSDK\1.4.363.0\Bin\spirv-dis.exe build\render\shaders\layer_frag.spv | Select-String "Decorate"
```

Other confirmed NVRHI behaviors that shape debugging:

- Render passes always `loadOp = LOAD` → stale pixels outside a quad are
  expected if the target wasn't cleared; `SceneRenderer::recordFrame` clears
  both ping-pong targets at the start of every frame for this reason.
- `setPushConstants` records immediately with the *current* pipeline layout
  (null before the first `setGraphicsState`) → push constants must come
  after `setGraphicsState`.
- Automatic state tracking inserts the barriers you see as
  `vkCmdPipelineBarrier2` in the dump; if a needed barrier is missing, the
  resource's desc is missing `enableAutomaticStateTracking(...)`.

## Checklist

1. Validation layer run — any violations?
2. Api dump: for the failing draw — is the descriptor set written to the
   right binding numbers/types? Is the sampler where the GLSL says?
3. Api dump: barriers — does the sampled image transition
   Transfer/RenderAttachment → ShaderReadOnly before the draw?
4. Api dump: `vkCmdBeginRendering` — is the attachment the image you expect?
   (loadOp is always LOAD.)
5. Push constants — right layout/offset/size, set after `setGraphicsState`?
6. Vertex data — `vkCmdUpdateBuffer` present with the right size? (Read back
   the vertex buffer to a CPU-accessible buffer + `mapBuffer` if in doubt.)
7. Re-derive the expected pixel from the premultiplied model, including the
   readback unpremultiply.
8. Minimal reproduction in `test_Render.cpp` (fresh context + renderer),
   then fix, then keep the regression test.
