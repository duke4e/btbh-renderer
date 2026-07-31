# Fixed-frame gameplay A/B benchmark design

Status: approved; implementation in progress

## Problem

The final default-gameplay A/B harness currently filters active 60-frame telemetry
windows and selects the last ten. Because an uncapped backend completes a different
number of frames in the same wall-clock duration, those windows can represent
different gameplay states. A clean exit and an `active` classification do not prove
that the Vulkan and legacy renderers processed the same workload.

## Design

Add an opt-in fixed-frame standalone gameplay profile used only by the diagnostic
harness:

- `BTBH_START_PROFILE_FRAMES` supplies an exact positive frame count.
- While that profile is active, gameplay simulation advances by exactly `1 / 60`
  second per rendered frame; rendering remains uncapped.
- The game emits one `BTBH_START_PROFILE_FINISHED` record containing the seed,
  selected frame, completed frame count, simulated duration, and final entity
  counts.
- The A/B harness runs both backends with the same executable, seed, player frame,
  resolution, projectile-shadow setting, fixed step, and frame count.
- It rejects a run unless the exact expected number of 60-frame telemetry windows
  is present, every scored window represents an active world render, shutdown is
  clean, and the finish record is present exactly once.
- It rejects the complete series unless every scored run has an identical workload
  signature.
- One initial telemetry window is warmup; all remaining fixed-frame windows are
  scored. Backend order remains balanced and each backend receives a prewarm run.

The existing wall-clock profile remains available for interactive diagnostics, but
it is no longer authoritative evidence for a backend performance claim.

## Verification

- Unit tests prove fixed-frame profiles use a `1 / 60` simulation step, terminate
  exactly at the requested frame count, and leave ordinary gameplay on measured
  delta time.
- A short one-pair gate proves exact telemetry-window count, identical workload
  signatures, backend selection, clean shutdown, and artifact generation.
- A fresh balanced ten-pair run is the final early-game performance authority.
- The existing deterministic Growth-5 1920x1080 captures remain the late-game
  cross-check because they already use fixed simulation delta and exact entity
  signatures.

## Acceptance

Vulkan must beat legacy on aggregate median and p95 in the fresh ten-pair fixed-frame
run without changing visual goldens, comparison thresholds, or production quality
settings.
