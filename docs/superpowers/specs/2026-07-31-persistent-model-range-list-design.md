# Persistent Model Range List Design

Status: implemented and verified

Date: 2026-07-31

## Goal

Make the Vulkan gameplay path beat the tuned chunk-640 OpenGL baseline
(`4.190 ms` median, `4.862 ms` p95) without changing goldens or lowering
visual thresholds.

The intermediate chunk-160 Vulkan path spent roughly `4.8-5.1 ms` constructing
the static city scene in Swift before the renderer can submit it. It emits
roughly 414 persistent draws per frame. Fine ranges are still valuable on
Vulkan because they reduce camera and spot-shadow work, so reverting to coarse
chunks gives back the GPU win.

## Design

Add one renderer-neutral persistent model range-list submission. A list owns:

- one model and material-override set;
- one semantic/persistent instance buffer and revision;
- multiple source ranges, each with world-space cluster bounds.

The game groups the already packed static-city chunks by persistent storage and
submits one cached list per storage. The Vulkan backend queues the list in
constant work during scene construction, then:

1. resolves the persistent buffer, model geometry, and materials once;
2. validates the buffer revision once;
3. performs camera-frustum rejection over the compact range list;
4. emits native batches for surviving ranges while preserving the bounds for
   spot-shadow rejection.

An optional dedicated spot-shadow range extension was considered, then
removed after implementation evidence showed that production never supplied
it. Primary ranges remain authoritative for camera, directional-shadow, and
spot-shadow visibility; the public contract has no dead dedicated range list.

The default protocol implementation preserves compatibility by culling and
expanding the list to existing single-range draws. Legacy OpenGL keeps its
backend-optimal 640-unit city grouping, while Vulkan uses the measured
production optimum of 320 units.
This is an explicit backend capability choice, not a deliberately slowed A/B
control.

The API must not rebuild semantic instance arrays, compare their contents per
range, or allocate one draw object per range in the gameplay construction
phase. Destruction, objective highlighting, and persistent-buffer revision
updates retain their current behavior.

## Verification

- Core tests cover range validation and default expansion.
- GPU tests prove one resource resolution per list, per-range camera culling,
  and retained spot-shadow bounds.
- Legacy/game tests prove source compatibility and correct live/hidden ranges.
- Full renderer and game tests, release build, API audit, and diff checks pass.
- Repeated warmed gameplay captures retain the canonical entity signature,
  clean shutdown, and 1920x1080 output.
- Vulkan is compared against a fresh chunk-640 OpenGL control. The change is
  retained only if total median and p95 improve; it is not enough to move work
  between profiler phases.
- Goldens and thresholds stay unchanged. The documented Windows-to-Apple SSIM
  issue remains documented and is not declared fixed.

## Result

The final 10-pair late-active artifact measured Vulkan at `4.043/4.371 ms`
median/p95 and legacy at `5.644/5.996 ms`; Vulkan won median in 10/10 pairs.
Two 1920x1080 Growth 5 capture pairs averaged `16.106/21.942 ms` for Vulkan
and `19.803/25.623 ms` for legacy with exact matching entity signatures and
clean shutdown. No golden or threshold changed; strict visual SSIM remains a
separate open gate.
