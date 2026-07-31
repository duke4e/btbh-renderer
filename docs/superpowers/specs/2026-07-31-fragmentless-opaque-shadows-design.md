# Fragmentless Opaque Shadows Design

Status: rejected after controlled Vulkan runtime test

Date: 2026-07-31

## Problem

The production shadow renderer binds a fragment shader even for opaque geometry
that cannot discard pixels. That shader has an empty body and the render pass has
no color targets, but the graphics pipeline still schedules a fragment stage.
The warm Vulkan capture attributes 3.72 ms to the spot shadow and 1.62 ms to the
primary directional shadow, making this repeated empty work a high-value target.

## Design

Keep the discard-capable shadow pipelines unchanged for procedural surfaces.
Create every non-discard shadow pipeline with a null fragment shader:

- static general vertex path;
- static fast vertex path;
- animated vertex path.

The depth target, depth compare/write state, culling, vertex transforms, instance
data, and shadow matrices remain unchanged. The existing public shader-path API
is retained for compatibility during this phase, although the fast empty
fragment artifact is no longer loaded by the native runtime.

## Acceptance

- A source-contract test proves non-discard pipelines are fragmentless while
  discard-capable pipelines still use the shadow fragment shader.
- The renderer compiles and the real Vulkan material/readback scene test passes.
- Gameplay-only uncapped telemetry is neutral or faster and shuts down cleanly.
- No golden or visual threshold changes are allowed.

## Runtime Result

The first gameplay attempt was invalid because the Windows Swift release build
was still compiling in a detached frontend while an older executable was
launched. A subsequent controlled A/B used the shadow-active
`testMaterialSlotBatchesPrepareOnlyDrawnModelPrimitives` test with the same
shader assets and environment. The fragmentless candidate hung for more than
90 seconds; after restoring the empty fragment shader, the control completed in
0.381 seconds. The candidate is therefore unsafe on the tested Vulkan driver
and was removed before gameplay benchmarking.
