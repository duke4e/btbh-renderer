# Phase 5 Radiance Cascades and CameraProfile Implementation Plan

Status: implementation complete; Windows verified; native Metal acceptance pending

Date: 2026-08-20

## Goal and acceptance

Deliver the approved narrow Radiance Cascades module in the production SDL_GPU
renderer, expose it through the Swift seam, integrate deterministic
CameraProfile transitions in the game, and close Phase 5 with Vulkan/Metal
visual and performance evidence. RC must always retain a typed, non-black
fallback to the existing IBL + ambient + SSAO result.

## Ordered implementation

1. **Lock renderer contracts and deterministic policy.** Add the tier, debug,
   fallback, descriptor, and status types to
   `include/btbh_gpu/btbh_gpu.h`; add pure validation/Auto-state helpers in
   `src/gpu/btbh_gpu_radiance_cascades.{h,c}`; extend the scene report; and add
   focused C tests in `tests/btbh_gpu_radiance_cascades_test.c`. Verify unknown
   values, `Off`, first-use `Auto`, hysteresis, adjacent changes, transition
   gating, and sticky fallback.

2. **Add cross-backend shader artifacts.** Add seed, trace/merge, resolve, and
   compose/debug HLSL under `shaders/gpu_spike`; register SPIR-V, MSL,
   `metallib`, and JSON outputs in `CMakeLists.txt`; and extend
   `tests/btbh_gpu_phase2_shader_test.c` with exact sampler/storage/uniform and
   8x8 threadgroup assertions. The shader contract must be identical between
   Vulkan and Metal.

3. **Integrate lazy RC resources and command encoding.** Add per-window state to
   `BTBHGPUSceneRuntime`; create/release lazy tier-sized textures and pipelines;
   make auxiliary geometry targets active for SSAO or RC; encode seed,
   far-to-near cascades, resolve, and compose between geometry and SSAO/post;
   and preserve the original scene input on every local RC failure. Extend
   teardown, resize, debug labels, typed status, and CPU encode telemetry.

4. **Expose RC through the Swift renderer seam.** Add public descriptor enums
   and defaults in `Sources/BTBHRenderer/RenderResources.swift`; project them
   through `GPUBackendsPending.swift`, `BTBHRendererGPU.swift`, and
   `GPUPlatform.swift`; surface status in scene telemetry; and add Swift unit
   and source-contract tests. Existing environment construction must remain
   source compatible and default to `off`.

5. **Integrate CameraProfile in the game repository.** Add deterministic
   profile content/selection and bounds tests; replace continuous
   `cameraGlitch`; make level-up apply profile, pending Auto tier, invalidation,
   and one hidden warm-up frame atomically; expose debug/tier controls without
   changing gameplay cue authority. Exact game paths are selected from the
   current camera and level-up ownership during implementation.

6. **Verify and close Phase 5.** Run renderer C/Swift tests, shader builds, real
   Vulkan smoke/readback tests, game tests, and all five Windows modes. Run the
   equivalent native Metal suite and captures on the Mac worker. Record
   three-run development and ten-run milestone A/B data, visual/cue comparisons,
   fallback evidence, and RC pass timings. Request one independent code review
   before declaring Phase 5 complete.

## Invariants and migration constraints

- `Off` is allocation/dispatch free and remains the default until milestone
  acceptance.
- An RC error cannot replace the primary scene error/result or abort otherwise
  valid scene submission.
- Auto applies only adjacent tiers and only at a transition boundary.
- All persistent state is per window and is released by existing scene teardown.
- The one-target geometry fast path remains unchanged when SSAO and RC are off.
- Existing five-mode goldens and thresholds are not weakened to accept RC.
- Phase 6 removal does not begin within this plan until Phase 5 is accepted on
  both platforms.

## Risks and unresolved evidence

The approved MRT-reuse approach derives receiver diffuse response from existing
ambient/material outputs. Its adequacy is a milestone evidence question, not an
open architecture decision. If controlled scenes show a material-color error
that confidence cannot mask, a dedicated material target requires a separate
design amendment and fresh MRT performance evidence.

The Windows machine can implement and validate the shared HLSL/SPIR-V path and
all platform-neutral contracts. Native Metal execution and final cross-platform
performance evidence require the Mac environment; lack of that external worker
blocks only Phase 5 closeout, not implementation progress.

## Verification record

Windows implementation verification completed on 2026-08-20: native renderer
build and 19 C tests, 73 Swift XCTest cases plus one Swift Testing test, the
targeted real-Vulkan RC Off/Low/resize/fallback test, GPU game build/bootstrap,
and the full game suite of 1,612 XCTest cases plus three Swift Testing tests all
passed. An independent review identified and closed missing RC shader staging in
the Mac build script and non-finite environment budget validation. Phase 5 stays
open only for native Metal execution, visual captures, and cross-platform A/B
performance evidence; RC therefore remains default-off and Phase 6 removal is
not yet authorized.
