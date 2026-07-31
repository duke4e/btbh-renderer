# Renderer Phase 2 SDL_GPU vertical spike implementation plan

Status: automatski odobren za izvedbu

Design:
`docs/superpowers/specs/2026-07-26-sdl-gpu-vertical-spike-design.md`

## Goal and acceptance

Izgraditi samostalni, deterministički SDL_GPU vertical spike koji prolazi isti
graphics/compute/readback contract na Windows/Vulkan i macOS/Metal backendu,
bez promjene legacy game defaulta ili curenja backend tipova u Swift public
seam.

Phase 2 je gotov tek kada oba OS-a imaju capability izvještaj, dvije zagrijane
capture serije, isti scene signature, clean shutdown i ručno pregledan
cross-backend rezultat.

## Task 1 — Pin dependencies and establish isolated build targets

Affected files:

- `.gitmodules`
- `CMakeLists.txt`
- `CMakePresets.json`
- `external/SDL`
- `external/SDL_shadercross`
- `src/gpu/btbh_gpu*.c`
- `include/btbh_gpu/btbh_gpu.h`
- `tests/btbh_gpu_contract_test.c`

Work:

- Pin SDL 3.4.2 and a recorded SDL_shadercross revision compatible with it.
- Add opt-in `BTBH_BUILD_GPU_SPIKE` and targets `btbh_gpu`,
  `btbh_gpu_contract_test`, and `btbh_gpu_spike`.
- Keep legacy `r3d` build behavior unchanged when the option is off.
- Define backend request, shader format, capability, error-stage and
  diagnostics POD contracts without SDL types.
- Add compile-time/public-header scans preventing SDL/Vulkan/Metal types from
  entering `include/btbh_gpu`.

Verification:

- Existing Swift tests and legacy CMake target remain green.
- Contract test covers enum mapping, exact-driver validation, missing
  capability reporting and idempotent shutdown.
- Windows configure/build does not use the game repo as an implicit include or
  library source.

## Task 2 — Create explicit device, window and swapchain lifecycle

Affected files:

- `src/gpu/btbh_gpu_device.c`
- `src/gpu/btbh_gpu_device_internal.h`
- `src/gpu/btbh_gpu_window.c`
- `spikes/sdl_gpu/main.c`
- `tests/btbh_gpu_device_test.c`

Work:

- Create SDL video/window lifecycle for the spike.
- Create the device with properties, exact `vulkan`/`metal` driver request and
  platform-specific shader format flags.
- Query device/driver properties and all required G-buffer, depth, shadow,
  storage and transfer usages.
- Claim the window, select an available swapchain composition/present mode,
  and handle zero-sized, resize, minimize and restore states.
- Emit deterministic JSON diagnostics with SDL version, renderer commit,
  backend, adapter/driver strings, formats, logical/drawable sizes and failure
  stage.

Verification:

- Windows device reports exactly `vulkan`; any D3D12/software fallback fails.
- Hidden-window smoke, visible-window smoke, resize/high-DPI and repeated
  shutdown pass under SDL GPU validation.

## Task 3 — Add the offline shader and reflection pipeline

Affected files:

- `shaders/gpu_spike/*.hlsl`
- `tools/build-gpu-shaders.ps1`
- `tools/build-gpu-shaders.sh`
- `cmake/BTBHShaders.cmake`
- `generated/gpu_shaders/**`
- `tests/btbh_shader_manifest_test.c`

Work:

- Pin and build the offline shadercross CLI and its required compiler
  dependencies; runtime translation is disabled.
- Compile spike HLSL to SPIR-V and MSL; produce `metallib` on macOS.
- Emit one normalized reflection manifest per entry point plus a combined
  artifact hash.
- Generate embedded C byte arrays or deterministic packaged assets.
- Validate SDL binding order, vertex layouts, render target outputs, uniform
  sizes and storage usages against the C pipeline descriptions.

Verification:

- Clean rebuild produces byte-identical Windows artefacts and manifests.
- Deliberate layout/hash mismatch fails the build-time test.
- Runtime opens only precompiled SPIR-V or `metallib`.

## Task 4 — Implement minimal GPU resource and frame seam

Affected files:

- `src/gpu/btbh_gpu_resources.c`
- `src/gpu/btbh_gpu_frame.c`
- `src/gpu/btbh_gpu_pipeline.c`
- `src/gpu/btbh_gpu_readback.c`
- `tests/btbh_gpu_resource_test.c`

Work:

- Implement buffers, textures, samplers, graphics/compute pipelines and
  transfer/readback through opaque internal records.
- Use generation-checked handles and deferred release behind completed fences.
- Allocate frame-local upload/readback data from reusable arenas after warm-up.
- Add debug labels for every pass/resource and structured SDL error capture.

Verification:

- Stale handles, double destroy, deferred release, resize recreation and
  upload/readback row-pitch cases have focused tests.
- Warm frame path performs no general heap allocation after reserved capacity.

## Task 5 — Render the deterministic vertical scene

Affected files:

- `spikes/sdl_gpu/spike_scene.c`
- `spikes/sdl_gpu/spike_assets.*`
- `src/gpu/btbh_gpu_spike_renderer.c`
- `tests/fixtures/gpu_spike/**`

Work:

- Create the 3-target G-buffer and sampleable depth.
- Add shadow, opaque/skinned/instanced, deferred lighting and transparent
  pipelines in the design-specified order.
- Store bone matrices in a storage buffer.
- Add compute write→sample and representative interval-trace kernels.
- Fix camera, seed, animation time, assets and scene signature.
- Read back the final RGBA8 image and emit metrics/entities/capabilities JSON.

Verification:

- Expected scene signature and attachment formats are asserted.
- Compute output has a known checksum before it contributes to the final image.
- Validation output contains no missing transition/binding/lifetime error.

## Task 6 — Windows Vulkan gate

Affected files:

- `tools/capture-gpu-spike.ps1`
- `docs/rendering/phase-2-sdl-gpu-spike-report.md`

Work:

- Run full renderer Swift/C tests and both legacy and GPU CMake targets.
- Run two warmed 1920x1080 Windows/Vulkan capture series.
- Capture resize/high-DPI/minimize/restore and clean shutdown evidence.
- Record hardware, OS, driver, commit, formats, scene signature,
  median/p95/worst, CPU submit and external GPU-capture observations.
- Diagnose any run-to-run or prior-contract regression above 5%; do not lower
  thresholds or replace Phase 0–1 goldens.

Verification:

- Both series are complete and stable.
- Backend is exactly Vulkan, output dimensions and signatures match, and
  screenshot/readback checks pass.

## Task 7 — Mac Metal gate and Phase 2 closeout

Affected files:

- `tools/capture-gpu-spike.sh`
- `docs/rendering/phase-2-sdl-gpu-spike-report.md`

Work:

- Build the same source with native Metal and `metallib`.
- Repeat the two-series, resize/high-DPI, readback and clean-shutdown gate.
- Compare Windows/Vulkan and macOS/Metal screenshots at SSIM 0.98 and inspect
  the first pair manually.
- Record real host class; do not label an M4 result as M1.
- If both backends pass, freeze the Phase 2 shader/capability/scene contract
  that Phase 3 will integrate behind `RenderServices`.

Verification:

- Exact Metal backend, identical semantic signature and clean diagnostics.
- Cross-backend visual and performance report is complete.
- Game branch still defaults to the legacy bundle, and Phase 0–1 evidence is
  unchanged.

## Risks and rollback

- SDL or shadercross revision mismatch: pin exact revisions and keep the spike
  opt-in so legacy builds remain independent.
- Missing local shader tools: build pinned tooling through the project rather
  than accepting environment-dependent artefacts.
- Unsupported target format: capability failure is explicit; no silent format
  or backend substitution.
- Cross-platform shader drift: reflection hashes, semantic scene signatures and
  screenshot gates distinguish source-layout errors from raster differences.
- Spike regressions cannot break the game default because integration into
  `RenderServices` is deferred until the Phase 2 closeout.
- Rollback is removal/revert of the opt-in GPU targets and dependency pins;
  legacy r3d and the game gitlink remain independently buildable throughout.
