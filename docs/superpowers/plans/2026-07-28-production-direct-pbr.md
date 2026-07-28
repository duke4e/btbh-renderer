# Production Direct-PBR Implementation Plan

**Goal:** Replace production vertex-Lambert lighting with bounded,
view-dependent direct PBR that consumes normal and ORM material channels.

**Approved design:**
`docs/superpowers/specs/2026-07-28-production-direct-pbr-design.md`

## Invariants

- Work only on `codex/renderer-phase-0-1`.
- Keep legacy goldens and comparator thresholds unchanged.
- Preserve unlit, emission, animation, billboard, depth, and clean-shutdown
  behavior.
- Do not claim AO/IBL, shadows, post-processing, or Metal verification.
- Commit and push renderer changes before updating the game gitlink.

## Deliverable 1: RED shader and ABI contracts

**Files:**

- Modify `tests/btbh_gpu_phase2_shader_test.c`
- Modify `CMakeLists.txt`
- Modify `tests/BTBHRendererGPUTests/GPUWindowProbeTests.swift`
- Modify `tests/BTBHRendererGPUTests/GPURenderServicesTests.swift`

Add reflection assertions for four production fragment samplers and tangent
location 6. Add camera-position validation expectations. Add a Windows Vulkan
render test whose normal-map and roughness/metalness variants must produce
measurably different center-region pixels. Run the focused tests and preserve
the failing evidence before implementation.

## Deliverable 2: Production shader and native runtime

**Files:**

- Modify `shaders/gpu_spike/production_model.vert.hlsl`
- Modify `shaders/gpu_spike/production_animated_model.vert.hlsl`
- Modify `shaders/gpu_spike/production_model.frag.hlsl`
- Modify `include/btbh_gpu/btbh_gpu.h`
- Modify `src/gpu/btbh_gpu_scene.c`

Thread camera position into a fragment uniform block, bind tangent input at
location 6, skin tangent data, emit the world-space TBN basis, bind all four
material textures, and create/release a neutral normal fallback. Implement
the bounded Burley/GGX/Schlick direct-light equations and stable degenerate
basis handling.

## Deliverable 3: Swift camera threading and focused green tests

**Files:**

- Modify `Sources/BTBHRendererGPU/BTBHRendererGPU.swift`
- Modify `Sources/BTBHRendererGPU/GPUPlatform.swift`
- Modify `Sources/BTBHRendererGPU/GPUBackendsPending.swift`
- Update affected tests under `tests/BTBHRendererGPUTests`

Add reusable camera-position storage to the probe, validate finite values,
forward it through `GPUPlatform`, and source it from the current frame camera.
Rebuild generated shaders and run the focused native reflection/contract and
Swift render tests until green.

## Deliverable 4: Renderer and game verification

Run the native `/W4 /WX` build, native CTest, full base renderer Swift tests,
full GPU renderer Swift tests, and `git diff --check`. After renderer review,
commit and push the renderer branch. Pin that exact renderer commit in the
game repository, run full game tests, release GPU build/bootstrap, render API
audit, and `git diff --check`.

## Deliverable 5: Ten-launch Windows A/B closeout

Run two independently warmed capture series for all five modes with the GPU
feature flag and Vulkan runtime environment. Verify 1920x1080 output,
identical entity signatures, and clean shutdown. Compare imagery and cues
against unchanged legacy authority, then compare median and p95 against
Windows Run 3/4. Diagnose any regression above 5%. Update the Windows Vulkan
report with exact commits, artifacts, results, and the remaining AO/IBL,
shadow, post, and Mac work. Commit and push the game gitlink/report only
after all gates are green.

