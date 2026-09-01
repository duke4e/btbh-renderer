# HRC Temporal Quadrant Reconstruction Design

Status: implemented and verified on Windows

Date: 2026-08-23

## Goal

Reduce the dominant HRC Finish Quadrants cost while preserving the existing
single-frame HRC result as the cold-start and invalidation baseline. Temporal
work must remain responsive: no long-running exponential average, no visible
light trail, and no history sample older than one completed frame.

## Research basis

- Holographic Radiance Cascades provides the single-shot directional transport
  solved by the current trace, merge-up, merge-down, and finish stages:
  https://arxiv.org/abs/2505.02041
- SVGF establishes the relevant temporal safety pattern: reproject history and
  reject it when depth, normal, or geometry identity is inconsistent:
  https://research.nvidia.com/publication/2017-07_spatiotemporal-variance-guided-filtering-real-time-reconstruction-path-traced

The temporal layer below amortizes independent HRC quadrant reconstruction. It
does not replace the paper solver, seed transport from screen color, or use a
long temporal denoiser to conceal a spatially invalid result.

## Design

### Bounded history

The runtime owns one fresh-work atlas and explicit filtered history A/B atlases.
Opposite rotation pairs `(0, 2)` and `(1, 3)` are refreshed on alternating
frames from the current merged radiance atlas. A fused reprojection and
opacity-aware reconstruction pass reads the fresh pair plus the prior history
pair and writes the opposite history atlas. The other pair is therefore exactly
one frame old at most; SDL resource cycling is not treated as history.

Cold start, resize, backend/tier generation change, explicit invalidation, or
an incompatible field transform refreshes all four rotations in one dispatch.
The resulting full-quality single-frame solve is always available without
temporal convergence.

### World-grid reprojection and rejection

HRC quadrants are anchored to the world-space occlusion field, so reprojection
uses the difference between previous and current field origins expressed in
HRC display pixels. Fractional or excessively large shifts invalidate history.
For valid integer shifts, every layer samples the matching prior display cell.

History is rejected when the source coordinate falls outside its layer or the
stored opacity disagrees with opacity derived from the current occlusion field.
Rejected texels are zeroed and naturally replaced when their pair is refreshed;
the rejection mask remains available in alpha for edge-aware reconstruction.

### Deterministic phasing

The rotation-pair phase advances only after a successful HRC encode. It resets
to pair zero whenever history is invalidated. This avoids random flicker and
ensures every direction is refreshed within two successful frames. No EMA or
unbounded sample count is used.

### GPU ordering

1. Refresh two selected layers into the fresh-work atlas, cycling once before
   the first partial write and preserving that backing for the second.
2. Reproject the non-fresh A/B history pair and reconstruct all four layers in
   one opacity-aware pass into the opposite history atlas.
3. Resolve from the newly written history atlas exactly as before.

Cold start refreshes all layers and sends all four through the same reconstruction
pass without sampling stale history. All work remains in the main scene command
buffer.

## Telemetry

Expose reprojection encode time/dispatch count plus history-valid, phase, and
fresh-layer count in the native and Swift pass telemetry. Named GPU groups make
the new cost independently measurable against the saved Finish work.

## Acceptance

- Cold-start output retains the existing four-layer single-frame path.
- Valid history encodes one reprojection dispatch and two half-height Finish
  dispatches; no layer can be older than one completed frame.
- Origin motion reprojects by the exact integer HRC-grid offset and rejects
  out-of-bounds or opacity-incompatible samples.
- Resize, tier/backend generation changes, and explicit invalidation perform a
  full refresh with no stale history.
- Dynamic lights respond within one refresh frame and leave no multi-frame EMA
  trail.
- Focused tests, Release build, live Vulkan telemetry, and deterministic moving
  and static captures pass before the roadmap item closes.

## Verification result

- Thirteen focused renderer tests pass, including bounded history, A/B storage,
  phase selection, opacity rejection, and cold-start contracts.
- SPIR-V compilation/reflection and the production Release game build pass; the
  Windows packaging allowlist includes the new shader artifact.
- Live telemetry shows cold frames as `history=0`, `fresh=4`, followed by a
  balanced phase 0/1 alternation with `history=1`, `fresh=2`. Every steady frame
  encodes one fused reconstruction dispatch, two half-height Finish dispatches,
  and zero standalone blur dispatches.
- Repeated 300-frame High/HRC profiles measure 4.270 ms average, 4.2 ms median,
  4.6 ms p95, and 4.831 ms worst versus the 4.30/4.20/4.60/4.82 pre-temporal
  reference. Forced occlusion revisions every 30 frames remain at 4.294 ms
  average, 4.2 ms median, 4.6 ms p95, and 4.875 ms worst.
- The 1920x1080 deterministic capture completes with the expected entity
  signature and no quadrant seams, black history, or visible stale-light trail.
