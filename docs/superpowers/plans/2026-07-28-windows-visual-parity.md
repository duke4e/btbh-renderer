# Windows Vulkan Visual Parity Implementation Plan

## 1. Freeze evidence and active feature inventory

- Retain the approved legacy manifest and the current
  `windows-gpu-canvas-probe-current-J` capture as the before pair.
- Record the active material, environment, screen-shader, and surface-shader
  descriptors used by the five deterministic modes.
- Add a source-level contract test that prevents an accepted descriptor field
  from being silently dropped at the Swift-to-C production boundary.

## 2. Implement raster and alpha contract test-first

- Extend `GPUSceneMaterialProbe`, `BTBHGPUSceneGeometryBatch`, validation, and
  shader uniforms with alpha-cutoff, cull, stencil, and depth-write
  semantics.
- Add native pipeline variants in `src/gpu/btbh_gpu_scene.c` for the active
  combinations while keeping unsupported combinations explicit.
- Add fragment alpha-cutoff and shadow alpha-cutout support to the production
  shader set and reflection/build assertions.
- Add focused Swift projection tests, native validation tests, and real
  Vulkan pixels that fail on the current always-blend/always-depth-write path.

## 3. Split opaque and transparent queues

- Compile opaque/alpha-cutout packets before transparent packets.
- Preserve grouped additive instances where ordering is safe; stable-sort
  alpha and premultiplied-alpha content back-to-front without mutating
  persistent caller storage.
- Disable transparent depth writes while retaining authored depth compare,
  cull, stencil, billboard, animation, and material texture behavior.
- Verify the original Game/Combat center-stack defect with a focused
  synthetic overlap probe and a five-mode capture.

## 4. Add the active HDR environment/post path

- Add renderer-owned linear HDR scene color and sampled depth resources that
  resize and release with the scene runtime.
- Apply SSAO only when `EnvironmentDescriptor.ssaoEnabled` is true, then
  Reinhard tonemap with `tonemapWhite`.
- Classify and project the bundled LUT, CAS, and tilt-shift resources and
  parameters; compile their offline Vulkan/Metal shader variants.
- Project `FrameDescriptor.antiAliasing` and add scene-only SMAA
  edge/weight/neighborhood passes using the retained area/search assets;
  keep medium and high as explicit variants and bypass all three passes for
  `.off`.
- Resolve post output to the swapchain before the existing canvas/UI pass.
- Add math/parameter tests plus real Vulkan image probes for each pass,
  including a particle ribbon control that distinguishes SMAA from `.off`.

## 5. Port exercised surface-shader semantics

- Classify the game’s known surface descriptors and reject unknown variants
  rather than silently ignoring them.
- Port deterministic-capture variants first, then water, glitter/highlight,
  dissolve, and shatter variants required by runtime gameplay.
- Preserve material PBR inputs, animation, instancing, shadows, and stencil
  behavior around each variant.

## 6. Close the Windows visual gate

- Run full renderer and game tests, native `/W4 /WX` build/CTest, release
  `.exe` build, API audit, and `git diff --check`.
- Capture two warmed five-mode Vulkan series and confirm exact entity
  signatures, clean shutdown, and 1920x1080 PNG/QOI output.
- Compare same-source repeatability and unchanged legacy goldens/cues. Record
  global/cue SSIM and manual defect classification without lowering a
  threshold or replacing authority.
- Compare median/p95 to the retained Windows controls and diagnose any stable
  regression above five percent.
- Request one final independent review, update parity documentation, commit
  and push the renderer first, then update and commit the game submodule
  gitlink.

## Risks and controls

- Pipeline variant growth: create only combinations authored by this game and
  test rejection of unsupported values.
- Transparent sorting cost: reuse scratch storage and preserve grouped
  additive paths; measure CPU encode telemetry already present locally.
- Post memory/bandwidth: allocate targets lazily, reuse them, and measure
  editor modes where sub-millisecond ratios are sensitive.
- Surface-shader scope: no runtime GLSL compiler and no filename-based silent
  fallback; known semantics are explicit and unknown semantics fail closed.
- Cross-platform drift: generate both SPIR-V and Metal shader artifacts, but
  defer native Metal visual claims until the later Mac gate.
