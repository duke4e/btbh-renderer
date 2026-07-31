# Canvas Batch and State Coalescing Design

Status: implemented and verified

Date: 2026-07-31

## Goal

Reduce Vulkan gameplay Canvas CPU/driver overhead without changing draw order
or pixels. A representative gameplay capture records roughly 208 Canvas draws
for only 4,992 vertices and about 0.02 ms of GPU work, making command encoding
the meaningful cost.

## Design

The Swift Canvas accumulator merges only two adjacent batches when all of the
following are true:

- their vertex ranges are contiguous;
- they belong to the same Canvas pass;
- texture identity is exactly equal, including both being solid/untextured;
- blend mode is exactly equal;
- scissor presence and rectangle are exactly equal.

The merge only extends the previous triangle-list vertex count. It never
reorders vertices, crosses a clear or render-target pass boundary, combines
different textures, or weakens state comparison. Text and textured quads may
merge only when they reference the same texture resource.

Within each native Canvas render pass, cache the currently bound graphics
pipeline and scissor rectangle. Bind or set them only when their exact value
changes. A textured batch still binds its sampler every time so texture
changes on a shared textured pipeline cannot inherit stale descriptors. State
caches reset at every render-pass boundary. The first draw always establishes
every required state.

## Verification

- Pure tests cover merge success plus rejection for noncontiguous ranges,
  pass boundaries, texture, blend, and scissor differences.
- A source contract verifies native pipeline and scissor caches plus the
  unconditional textured sampler bind.
- Vulkan Canvas readback tests cover solid, textured/font, scissor, blend, and
  render-target paths without changing thresholds.
- A gameplay capture preserves entity signature, clean shutdown, output size,
  and visual parity.
- The change is retained only when warmed total-frame median and p95 improve
  or remain neutral; Canvas telemetry and draw count provide secondary proof.

## Result

The exact adjacent-batch merge and per-pass pipeline/scissor caches are in the
final source, with sampler rebinding preserved. Focused Canvas readback and
full renderer suites pass. The optimization is included in the final 10-pair
Vulkan result of `4.043/4.371 ms` versus legacy `5.644/5.996 ms`; no golden or
threshold changed.
