# Persistent Model Range List Implementation Plan

Status: completed and verified

Date: 2026-07-31

1. [x] Add failing core tests for bounded persistent model range lists and their
   source-compatible default expansion.
2. [x] Add the renderer-neutral range-list value types and scene-backend contract.
3. [x] Add a Vulkan queued-list path that resolves shared state once and performs
   concrete-backend frustum culling during scene submission.
4. [x] Add focused GPU tests for revision validation, camera culling, material
   reuse, and spot-shadow bounds.
5. [x] Add a `SceneRenderer` range-list seam and group static-city chunks by packed
   storage without per-range scene calls.
6. [x] Select chunk 320 for Vulkan and chunk 640 for legacy OpenGL after the
   measured 160-unit intermediate experiment.
7. [x] Run focused tests and diagnostic gameplay captures. Keep or revise only
   from total-frame evidence.
8. [x] Accumulate further native scene/canvas wins until repeated warmed Vulkan
   median and p95 beat the tuned OpenGL control.
9. [x] Reject and remove the optional dedicated spot-shadow range path after
   proving production never supplied it; retain primary ranges for all passes.
10. [x] Run the full renderer/game verification and parity gates: 185/185
    canonical and vendored renderer tests, 18/18 CTests, 1533+3 game tests,
    release/bootstrap, API audit, and diff checks.
