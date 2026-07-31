# Scene-material linear-color cache design

Status: rejected after warmed gameplay A/B

## Evidence

The Swift-to-native scene projection rebuilds approximately `400` native
geometry batches per canonical gameplay frame. For every batch it calls
`GPUColorSpace.linearized` for both albedo and emission, even though model and
standalone material probes are already resource-cached. This performs up to
eight sRGB transfer functions per batch in a per-frame hot path.

The current pipeline-only W297 control attributes approximately `2.60 ms` to
the platform/native presentation wrapper while the measured native C encode is
approximately `1.01 ms`. Native batch projection is therefore a larger
remaining renderer-side CPU target than individual redundant SDL state calls.

## Design

Add an immutable linear-color projection to `GPUSceneMaterialProbe`. Compute
it once from the semantic albedo and emission colors whenever a material probe
is created. Native scene-batch projection copies these cached colors instead
of re-running the sRGB transfer function.

The original semantic colors remain unchanged for equality, ordering, tests,
and resource behavior. Nil materials preserve the current white defaults.

## Validation

1. A unit test proves the cached albedo and emission values use the canonical
   `GPUColorSpace.linearized` conversion.
2. Existing material, model, and real Vulkan presentation tests remain green.
3. Gameplay-only A/B must reduce adapter/native presentation time without
   changing batch, instance, or entity counts.
4. Retain only with canonical entities, clean shutdown, exact resolution, and
   unchanged visual gates.

## Outcome

Rejected and reverted. The warmed W300/W301 and W302/W303 control/candidate
pairs showed that enlarging every cached material probe cost more in copying
and packing than the removed color conversions saved:

- frame median: `+1.62%`;
- frame p95: `+2.51%`;
- adapter pack mean: `+5.31%`;
- platform/native presentation mean: `+2.44%`.

Future native-batch work should reduce the amount of per-frame projection
rather than add more cached fields to every Swift material value.
