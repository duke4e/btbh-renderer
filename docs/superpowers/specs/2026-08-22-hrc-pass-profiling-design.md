# HRC Pass Profiling Design

Status: implemented

Date: 2026-08-22

## Goal

Make HRC performance attributable to individual transport and reconstruction
passes without adding synchronization or frame latency to normal gameplay, and
produce the evidence needed to select the first temporal-reconstruction
optimization.

## Evidence and constraints

- The production renderer vendors SDL 3.4.2. Its GPU API, and the current SDL
  GPU API, expose fences and debug markers but not timestamp query pools.
- Existing `fence_completion_ms` measures submit-to-observation queue latency;
  it is not an individual-pass GPU duration.
- In the screenshot capture, frame 167 had 25.29 ms fence latency while total
  CPU scene encode was 0.38 ms and RC encode was 0.08 ms. The following Canvas
  present waited 22.54 ms.
- A no-readback control run, after the existing two-second settle, completed
  300 HRC High frames at 4.20 ms median, 4.60 ms p95, and 4.68 ms worst. The
  apparent runtime hitch was therefore capture/readback interference. First-use
  pipeline work remains confined to startup/loading frames.

## Design

1. Preserve every existing per-dispatch label and wrap each HRC dispatch/draw
   in a balanced SDL GPU debug group. RenderDoc, PIX, and Xcode GPU captures can
   then report exact GPU duration for trace, merge-up, merge-down, finish,
   blur, resolve, and compose events without a shipping synchronization cost.
2. Add an HRC pass telemetry payload to the public RC status. It reports CPU
   command-encoding time and dispatch count per phase, plus RC prepare,
   pipeline-prepare, and resource-prepare CPU time. Field names must say `cpu`
   so fence latency is never mislabeled as GPU execution time.
3. Emit the new fields on the existing `BTBH_GPU_SCENE_ENCODE_TELEMETRY` line
   when `BTBH_RENDER_PERF_LOG=1`. Normal runs retain only timestamp reads and
   debug markers; no per-frame logging is added.
4. Keep first-use work lazy and inside the existing loading/settle window.
   Do not split HRC into synchronized command buffers or wait for a fence per
   pass: that would measure submission overhead and destroy normal throughput.
5. Capture a representative HRC High frame in RenderDoc and use GPU event
   durations, not CPU encode time or fence latency, to select the next shader
   optimization and temporal-history design.

## Acceptance criteria

- Classic RC reports zero HRC dispatch counts.
- HRC reports nonzero trace/merge counts and exactly one finish, blur, resolve,
  and compose operation on a successfully encoded frame.
- Every HRC debug group is balanced on success and dispatch failure.
- Perf logs expose prepare and per-phase CPU encode fields with unambiguous
  names.
- Existing RC output, tier selection, one-frame-in-flight policy, and normal
  frame pacing do not change.
- Renderer tests, production GPU Release build, no-readback profile, and a
  visual regression capture pass.

## Risks

- CPU encode timings do not estimate shader cost; they exist only to separate
  command-building stalls from GPU/queue stalls.
- GPU debug groups have small driver overhead in debug tooling. They do not
  force submission or synchronization.
- Exact GPU timestamps remain profiler-owned until SDL lands a portable query
  API; vendoring an unfinished SDL query proposal is intentionally out of
  scope.

## Result

- The production Release build completed and the no-readback HRC High profile
  retained a 4.20 ms median and 4.60 ms p95 over 300 measured frames.
- The public telemetry reports 3 trace, 6 merge-up, 8 merge-down, and one each
  of finish, blur, resolve, and compose for the production HRC layout.
- A RenderDoc GPU-duration capture measured 1.974 ms across the named HRC
  events. `Finish Quadrants` was the dominant event at 0.782 ms (39.6%),
  followed by trace at 0.477 ms (24.2%) and merge-down at 0.427 ms (21.7%).
- The first temporal follow-up is therefore alternating two of the four finish
  quadrants per frame with validated double-buffered history. This directly
  targets the dominant pass while retaining all four orientations over two
  frames; its upper-bound saving is approximately 0.39 ms before history and
  validation overhead.
- Two repeated 1920x1080 deterministic captures retained identical entity and
  cue signatures. Their small image differences matched the existing FPS text
  and time-driven light variation rather than a renderer-output regression.
