# Native storage-buffer bind cache design

Status: rejected after gameplay measurement

## Evidence

The retained pipeline cache reduces native encode to approximately
`0.96 ms`, but both shadow and geometry loops still call
`SDL_BindGPUVertexStorageBuffers` for every drawn geometry. Most gameplay
draws read instances from the same shared transient buffer; animated draws add
the same frame bone buffer in slot one.

SDL defines vertex storage-buffer bindings for subsequent draw calls inside a
render pass. Rebinding the exact same slot count and pointer set without an
intervening different binding is redundant.

## Design

Track the currently bound vertex storage-buffer count and pointers separately
inside every shadow and geometry render pass. Bind again only when:

- slot-zero instance-buffer identity changes;
- the draw changes between static and animated storage layouts; or
- the animated bone-buffer identity changes.

Reset all cached state at `SDL_BeginGPURenderPass`. Pipeline, uniform, sampler,
vertex/index, stencil, and draw behavior otherwise remain unchanged.

Expose actual and skipped storage-bind counts only while frame telemetry is
enabled.

## Validation

1. The real Vulkan material-slot presentation test reports skipped shadow and
   geometry storage binds while preserving draw counts.
2. Existing shadow, animation, material, and window tests remain green.
3. Gameplay-only A/B must reduce encode/submission time with non-zero skips.
4. Retain only with canonical entities, clean shutdown, exact resolution, and
   unchanged visual gates.

## Outcome

Rejected and reverted. The cache engaged in the canonical gameplay capture:
shadow recorded `110` binds and `636` skips, while geometry recorded `57`
binds and `383` skips. Despite removing `1,019` API calls per measured frame,
the warmed W294 run regressed native encode from the W291/W292 mean of
approximately `0.966 ms` to `1.015 ms` and frame median from `18.088 ms` to
`19.611 ms`. W293 was additionally contaminated by transient system load, so
the rejection is based on the stable warmed W294 result.
