# Native stencil-reference cache design

Status: rejected after contemporaneous gameplay A/B

## Evidence

The gameplay geometry render loop currently calls
`SDL_SetGPUStencilReference` before every drawn geometry, even when adjacent
draws use the same reference. The retained graphics-pipeline cache proves that
avoiding redundant render-pass state changes can materially reduce native
encode time.

SDL stencil reference is dynamic state used by subsequent draw calls inside
the active render pass. Repeating an unchanged reference has no rendering
effect.

## Design

Track whether a stencil reference has been set and its current value inside the
geometry render pass. Call `SDL_SetGPUStencilReference` only on the first draw
or when the value changes. Reset the cache for every new render pass.

Expose actual and skipped stencil-reference call counts only while frame
telemetry is enabled. Pipeline, storage-buffer, sampler, vertex/index, uniform,
and draw behavior remain unchanged.

## Validation

1. The real Vulkan material-slot presentation test records a skipped stencil
   reference while preserving draw counts.
2. Existing stencil and window tests remain green.
3. Gameplay-only A/B must reduce geometry/native encode time with non-zero
   skips.
4. Retain only with canonical entities, clean shutdown, exact resolution, and
   unchanged visual gates.

## Outcome

Rejected and reverted. The candidate reduced the canonical geometry pass from
`440` stencil calls to `7` calls plus `433` skips, but the back-to-back W297
pipeline-only control and W298b candidate comparison showed:

- geometry encode mean: `-0.29%`;
- total native encode mean: `+1.38%`;
- frame median: `+0.99%`;
- frame p95: `+2.16%`.

The tiny geometry-only change did not survive total-frame measurement, so the
extra cache state and telemetry were removed.
