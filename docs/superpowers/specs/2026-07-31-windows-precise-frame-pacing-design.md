# Windows precise frame pacing design

Status: approved, implemented, and Windows-verified

## Problem

Normal gameplay uses `FrameRatePolicy.displayRefresh`. On a 239/240 Hz Windows
display, the Vulkan backend currently waits with Foundation `Thread.sleep`.
The remaining budget after rendering is often below one millisecond, but the
sleep overshoots by several milliseconds. A deterministic 120-frame gameplay
probe measured 7.996 ms Vulkan cadence (125.1 FPS) versus 4.490 ms legacy
cadence (222.7 FPS), while the identical uncapped workload measured 2.659 ms
Vulkan cadence (376.1 FPS) versus 4.849 ms legacy cadence (206.2 FPS).

The SDL GPU swapchain uses immediate presentation, so the regression is in the
manual Swift wait rather than present or rendering throughput.

## Decision

- Preserve `displayRefresh`, `fixed`, and `uncapped` policy semantics.
- Preserve the immediate swapchain selection.
- Add a package-internal C function that delegates the coarse portion of a
  nanosecond wait to SDL's high-resolution `SDL_DelayNS` and reserves a 0.5 ms
  spin tail. This avoids stacking our spin tail on top of
  `SDL_DelayPrecise`'s own final spin, while still avoiding the observed
  Windows scheduler overshoot. It
  mirrors Raylib's partial-busy-wait strategy.
- Call that function from `GPUPlatform` instead of `Thread.sleep`.
- Keep the function out of the public `btbh_gpu.h` ABI.
- Preserve first-frame behavior, close/reopen reset, and start-to-start pacing.
- Expose an explicit opt-in uncapped choice in `run-btbh.cmd` for fair visible
  throughput comparisons; normal gameplay remains display-refresh capped.

## Verification

- Unit/source-contract coverage proves positive waits use the internal precise
  delay and uncapped/late frames do not wait.
- Public-header ABI stays byte-for-byte compatible with `HEAD`.
- The same fixed-frame gameplay signature must match across both backends.
- A display-refresh gate must bring Vulkan cadence to the 239/240 Hz cap rather
  than the previous ~8 ms cadence.
- A separate uncapped A/B remains the throughput authority and must show Vulkan
  ahead of legacy on the matched gameplay workload.
- Full renderer/game tests, release build, Vulkan bootstrap, API audit, and
  `git diff --check` must pass before handoff.

## Verified result

The final ABI-clean release executable has SHA-256
`F96BAE414FE8562E9389D3218D95EA8FF5B4656F06E7FE9FA41EA583DEA052BF`.
In the balanced ten-pair exact-300-frame display-refresh series, Vulkan
measured 4.227 ms median cadence and 4.973 ms median-run p95 versus legacy
OpenGL at 4.919/5.420 ms. The cadence-derived rates are 236.57 versus 203.29
FPS. In the separate uncapped series Vulkan measured 2.520/2.928 ms versus
4.705/5.272 ms, or 396.83 versus 212.54 FPS from the median cadence.

Vulkan won 10/10 per-pair medians in both policies, 8/10 display p95
comparisons, and 10/10 uncapped p95 comparisons. Both display p95 losses were
below the 5% diagnostic threshold. Every scored run used the same exact entity
signature and exited cleanly. The retained artifacts are
`.build/perf/final-fixed-frame-display-10x-20260731-v3-final-exe` and
`.build/perf/final-fixed-frame-uncapped-10x-20260731-v3-final-exe` in the game
repository.
