# Production Procedural-IBL and Material-AO Implementation Plan

**Goal:** Replace placeholder ambient shading with bounded procedural-sky IBL
and apply packed material AO on the production Vulkan path.

**Approved design:**
`docs/superpowers/specs/2026-07-28-production-procedural-ibl-design.md`

## Invariants

- Work only on `codex/renderer-phase-0-1`.
- Keep legacy goldens and comparator thresholds unchanged.
- Preserve direct PBR, unlit, emission, animation, billboard, and depth.
- Do not claim arbitrary-cubemap IBL, SSAO, exact legacy prefilter/LUT parity,
  shadows, post-processing, or Metal verification.
- Commit and push renderer changes before updating the game gitlink.

## Deliverable 1: RED lighting and render contracts

**Files:**

- Modify `Tests/BTBHRendererGPUTests/GPURenderServicesTests.swift`
- Modify native scene-contract tests as required by the ABI extension

Add focused tests for deterministic finite SH data and invalid-map fallback.
Add real Vulkan tests that isolate material AO under indirect diffuse light
and colored environment specular on a metallic surface. Run the focused tests
before implementation and retain the expected failing evidence.

## Deliverable 2: Procedural environment projection

**Files:**

- Modify `Sources/BTBHRendererGPU/GPUBackendsPending.swift`
- Modify `Sources/BTBHRendererGPU/BTBHRendererGPU.swift`

Implement the legacy-shaped procedural-sky evaluator and deterministic
solid-angle cubemap projection into nine real SH coefficients. Store the
projection with the ambient-map resource and resolve it into the current
scene-lighting probe without changing public resource handles.

## Deliverable 3: Native ABI and production shader

**Files:**

- Modify `include/btbh_gpu/btbh_gpu.h`
- Modify `src/gpu/btbh_gpu_scene.c`
- Modify `shaders/gpu_spike/production_model.frag.hlsl`
- Modify affected reflection and native contract tests

Extend the internal scene-lighting payload with SH coefficients, procedural
sky parameters, and an IBL-enabled flag. Upload the complete finite payload.
Evaluate SH diffuse irradiance, analytic procedural reflection radiance,
environment BRDF, material AO, and specular occlusion in the fragment shader.
Keep the four-sampler contract and all compatibility paths.

## Deliverable 4: Focused and full renderer verification

Rebuild generated shaders and run the focused RED tests until green. Then run
the native `/W4 /WX` build and CTest, full base Swift suite, full GPU Swift
suite, API checks, and `git diff --check`. Request one independent code review
and resolve any material findings before renderer closeout.

## Deliverable 5: Game integration and Windows A/B closeout

Commit and push the renderer branch, then pin that exact commit in the game.
Run the full game tests, release Vulkan build/bootstrap, render API audit, and
`git diff --check`. Run two independently warmed capture series across all five
modes, verify deterministic entities, clean shutdown, and 1920x1080 images,
then compare visuals, cues, median, and p95 without changing authority.
Document the exact commits, results, analytic IBL limitations, remaining
SSAO/shadow/post work, and the known cross-platform SSIM limitation. Push the
renderer documentation commit before the final game gitlink commit.
