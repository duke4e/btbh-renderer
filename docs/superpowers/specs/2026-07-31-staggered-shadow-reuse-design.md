# Staggered Shadow Reuse Design

Status: accepted and implemented

Date: 2026-07-31

## Problem

On the current chunk-160 source, Vulkan is within about 0.9 ms of legacy
OpenGL, while the two active shadow passes remain the dominant Vulkan GPU
work. Both maps are regenerated every frame even at uncapped frame rates.

## Design

Add `LightDescriptor.shadowUpdatePeriodFrames`, a typed per-light request with
a default of one and a valid range of 1...60 frames. The GPU backend honors
larger periods by:

- update active shadow lights in staggered phases;
- retain each shadow texture between updates;
- retain and shade with the exact view-projection matrix that produced that
  retained texture;
- force an update when a shadow becomes active.

The renderer retains each light's exact texture/matrix pair. Directional and
spot maps use different schedule phases so period-two updates do not land in
the same frame. Newly activated shadows update immediately.

Legacy backends may update more frequently than requested, so the OpenGL
backend remains continuously updated and API-compatible. The setting is not
controlled by an environment variable in production.

## Acceptance

- Default period one remains scheduling compatible.
- Descriptor projection and native shadow-active tests cover periods one,
  two, and three, plus invalid zero.
- Seeded period-two gameplay exits cleanly.
- Warmed total frame time beats the current same-source OpenGL result.
- Goldens and thresholds remain unchanged.

## Accepted Result

Two warmed Vulkan runs measured 9.032 ms and 8.875 ms median active frame
time. Their 8.953 ms mean is 20.0% lower than the 11.196 ms same-source
OpenGL result, with 111.7 versus 89.3 effective FPS. The retained-map
implementation therefore replaces the experimental environment override.
