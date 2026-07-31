# Remove Unused Dedicated Spot-Range Path

Status: Completed and verified

Date: 2026-07-31

## Goal

Remove the unused per-instance dedicated spot-shadow range path from the game
and renderer contract while preserving the production persistent range-list
path that produced the accepted Vulkan gameplay result.

## Evidence

The game previously built `spotShadowRanges` and per-instance render bounds,
but its only production persistent-city submission passed only the primary
`ranges` list.
Consequently, the renderer's optional `spotShadowRanges` API and backend branch
are dead in the measured game. They add allocation, mutation, API, test, and
maintenance cost without affecting current output.

## Design

- Keep `PersistentModelInstanceRange` and the primary `ranges` list.
- Remove `spotShadowRanges` from `PersistentModelRangeListDraw` and the game
  `SceneRenderer` adapter.
- Remove dedicated spot-range construction and per-instance render-bound
  storage from `CityRenderer`; retain each batch's padded `clusterBounds`.
- Make the GPU backend use each accepted primary range for camera,
  directional-shadow, and spot-shadow visibility exactly as the current game
  path already does.
- Apply the same renderer changes to the canonical and vendored copies.

## Non-goals

- Do not change light range, spot projection, primary frustum culling,
  persistent buffer packing, shadow cadence, visual goldens, or thresholds.
- Do not remove lower-level `spotShadowVisible` batch state; other scene draw
  paths still use it.

## Acceptance

- The dedicated spot-range identifiers are absent from game, canonical, and
  vendored production/test sources.
- Primary persistent range-list tests continue to pass.
- Canonical and vendored renderer sources/tests are synchronized.
- The game test suite and GPU-enabled release build pass.
- A fresh default gameplay smoke after the final build remains clean and keeps
  Vulkan ahead of the legacy backend.

## Result

Dedicated spot-range identifiers are absent from canonical, vendored, and game
production/test sources. Primary persistent ranges remain intact. The final
release and full suites pass, and the final 10-pair result keeps Vulkan at
`4.043/4.371 ms` versus legacy `5.644/5.996 ms` without a golden or threshold
change.
