# Radiance Cascades Cinematic Presence Plan

Status: completed on Windows; native Metal and temporal acceptance remain in Phase 5

Date: 2026-08-20

## Goal and acceptance

Deliver the approved confidence-aware compose curve and cinematic presets,
then prove the effect with deterministic framebuffer captures rather than only
shader compilation or subjective inspection.

## Implementation

1. Add a source-contract test for the confidence remap, bounded low-energy
   presence curve, raw debug bypass, and single authored-intensity application.
2. Implement the compose curve in
   `shaders/gpu_spike/production_rc_compose.frag.hlsl` without changing the
   debug branches or native descriptor ABI.
3. Update Swift/game defaults, CMD presets, tests, and documentation to
   `0.45 / 0.90 / 1.50`, then synchronize the vendored renderer.
4. Rebuild the shader/runtime path and capture deterministic Off, Zero,
   Balanced, Strong, Final indirect, and Confidence frames. Measure the
   acceptance thresholds and inspect the images for leaks, seams, clipping,
   and spatial coherence.
5. Run targeted and full renderer/game tests, real Vulkan RC coverage,
   canonical/vendor parity, diff checks, and one independent final review.

## Invariants and risks

- Authored intensity remains finite `0.0...2.0`, is tier-independent, and is
  applied only in resolve.
- Confidence remains a safety signal; the curve may remap it but must not
  replace it with a floor or unconditional contribution.
- Presence gain is monotonic, hue-preserving, bounded, and returns to unity for
  bright indirect light.
- Screenshot thresholds are a regression floor for the fixed authority frame,
  not a universal artistic metric.
- Temporal stability and native Metal execution remain explicit follow-up
  acceptance gates.

## Completion evidence

- The deterministic High/Strong frame reached `10.01 / 255` mean absolute
  difference from Zero, with `71.51%` of world pixels changing by more than
  ten levels and only `0.17%` darkening by more than two levels.
- Newly clipped pixels whose Zero source was below 192 remained at `0.029%`,
  so the added presence did not produce broad highlight clipping.
- The renderer passed 19/19 native tests, 74/74 XCTest cases plus the Swift
  Testing lifecycle test, and the real Vulkan RC probe. The game passed
  1613/1613 XCTest cases plus three Swift Testing cases.
- Canonical/vendor parity and whitespace checks passed. Independent review
  found no remaining issues after adding launcher freshness validation and
  strengthening the shader source-contract test.
