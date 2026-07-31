# Transient instance-range consolidation design

Status: approved

Outcome: rejected after W289b. The final gameplay window retained exactly the
same `401/405` median/p95 batch count as W288, so no real workload pair passed
all merge guards. W289b measured `18.511/23.821 ms`, and the behavioral
candidate plus its expectation test were reverted.

## Evidence

W287/W288 leave approximately `3.6 ms` in renderer scene submission. Native
encoding accounts for approximately `0.47 ms` of shadow encoding and `0.42 ms`
of geometry encoding while the Swift adapter submits roughly 396 batches in
the final workload.

All transient scene instances are packed into one shared native instance
buffer. The current adjacent-range consolidator nevertheless requires a
non-null persistent instance-buffer pointer, so otherwise identical transient
ranges remain separate draw batches even when their `firstInstance` ranges are
contiguous.

## Design

Generalize adjacent instance-range consolidation to accept either:

- the same non-null persistent instance-buffer pointer; or
- two null pointers, which both select the one shared transient buffer.

All existing merge guards remain required: both batches are legacy-instanced,
geometry/material/material-slot state is equal, neither batch is animated,
spot-shadow visibility is equal, and the instance ranges are exactly
contiguous without integer overflow.

Consolidation runs after ordering and spot-shadow culling. It therefore keeps
the existing render-list order and does not broaden a batch across a
shadow-visibility boundary. Instance bytes, shaders, materials, and the native
buffer layout do not change.

## Validation

1. A focused test proves contiguous transient ranges merge and that mixed
   transient/persistent ranges do not.
2. Existing persistent-gap and spot-shadow visibility tests remain green.
3. A warmed gameplay-only capture must show a lower submitted batch count.
4. Retain only if repeated gameplay median/p95 improves without changing the
   canonical entity signature, clean shutdown, `1920x1080` output, or visual
   gates.
