# HRC Edge-Aware Reconstruction Plan

Status: complete

Date: 2026-08-23

Design:
`docs/superpowers/specs/2026-08-23-hrc-edge-aware-reconstruction-design.md`

1. [x] Add shader source/reflection contract tests for depth, normal, and RC
   confidence-guided reconstruction.
2. [x] Implement the bounded joint-bilateral reconstruction compute shader and
   add it to shader compilation/reflection and game packaging.
3. [x] Add the render-resolution reconstruction texture, pipeline lifetime,
   dispatch, compose input, and pass telemetry.
4. [x] Mirror shared renderer changes into the game Vendor copy and pass focused
   Swift/C reflection tests.
5. [x] Build the production Release game, capture deterministic High and Low
   comparisons, and record no-readback Vulkan performance evidence.
6. [x] Update the parent roadmap and mark item 3 complete only after visual and
   runtime acceptance.
