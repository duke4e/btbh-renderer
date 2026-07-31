# Staged Post-Processing Design

Status: rejected after benchmark

Date: 2026-07-31

## Problem

The Vulkan production post shader evaluates LUT grading and CAS sharpening for
every tilt-shift tap. A single output pixel can therefore repeat the expensive
grading and sharpening work thirteen times. The legacy renderer instead applies
LUT, CAS, and tilt shift as ordered screen passes.

The measured warm RenderDoc capture attributes 1.73 ms at 1920x1080 to the fused
production post pass. This is a material part of the Vulkan deficit and an
avoidable compatibility cost rather than useful image work.

## Design

Preserve the existing public post-processing API and the visual operation order,
but execute the operations in two native passes:

1. A new HDR pre-post pass applies LUT grading and CAS sharpening once per pixel.
2. The existing production post pass samples that result for tilt shift, then
   applies tone mapping, linear-to-sRGB conversion, and dithering.
3. FXAA or SMAA remains a separate final anti-aliasing stage.

The pre-post pass runs only when LUT or CAS is enabled. Its target uses the scene
HDR format and is recreated with the other size-dependent scene textures. The
production post shader no longer contains LUT or CAS evaluation, preventing a
future accidental multiplication inside blur taps.

## Compatibility

- Keep the current post uniform layout and public renderer/game API.
- Preserve the semantic order LUT -> CAS -> tilt shift -> output transform -> AA.
- Do not change visual goldens or comparison thresholds.
- Keep both SPIR-V and Metal shader build paths in CMake.
- Release the new texture and pipeline with the existing scene runtime resources.

The staged result can differ slightly from the fused shader around sub-pixel
sampling because the preprocessed HDR field is bilinearly sampled by tilt shift.
That matches the legacy renderer's staged-pass model and must be evaluated by
capture comparison rather than hidden by threshold changes.

## Acceptance

- A source-contract regression test proves LUT/CAS live only in the pre-post
  shader and that the native runtime encodes the new pass.
- The real GPU readback scene test and renderer test suite remain green.
- The game GPU release build succeeds and shuts down cleanly.
- Gameplay-only uncapped telemetry shows no frame-time regression.
- A fresh GPU capture confirms LUT/CAS is no longer multiplied by tilt-shift
  taps and records the new per-pass timing.

## Benchmark Result

The implementation passed its source-contract and real Vulkan readback tests,
but failed the gameplay performance gate. On the same seeded, uncapped
gameplay-only profile, warmed 60-frame windows increased from the prior
approximately 11.3-11.8 ms range to 13.8-16.8 ms. The full-screen HDR prepass
cost more than it removed because much of the existing fused shader exits
before the expensive blur taps. The implementation was reverted; this document
remains as evidence against repeating the experiment without a more selective
execution strategy.
