# Frame-Cached Frustum State Design

Status: accepted and implemented

Date: 2026-07-31

## Problem

Fine static-city chunks reduce Vulkan GPU work, but every chunk query currently
recomputes the same camera normalization, basis cross-products, viewport
aspect, projection tangent, and a temporary scalar array. Gameplay calls this
query hundreds of times per frame, contributing several milliseconds to the
world-layer scene build.

## Design

Build an immutable `GPUSceneFrustumCullingState` once in `beginFrame` from the
current camera, viewport, and drawable size. It stores the normalized camera
basis and precomputed perspective tangents or orthographic half-extents.
Per-bound queries validate six scalars directly and perform only the AABB
projection tests.

Rebuild the state on every frame and drawable resize. Invalid camera state
retains the existing fail-open behavior; invalid bounds remain fail-closed.
Bounds are validated independently of cached-frame availability, so an invalid
query before `beginFrame` cannot accidentally pass through.

## Acceptance

- Focused tests preserve perspective inside/outside behavior.
- Existing scene frustum tests pass.
- The GPU-enabled release game exits cleanly.
- Chunk-160 gameplay lowers scene-build and total warmed frame time without
  changing entity signatures, goldens, or thresholds.

The implementation passed the focused perspective/orthographic and invalid
bounds tests and is part of the accepted current Vulkan result.
