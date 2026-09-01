# HRC Pass Profiling Plan

Status: complete

Date: 2026-08-22

1. Extend the C and Swift RC status contracts with preparation and HRC phase
   CPU telemetry plus dispatch counts; add focused mapping regressions.
2. Instrument RC preparation and each HRC phase, and wrap every HRC GPU event
   in a balanced debug group while preserving the existing labels.
3. Extend `BTBH_GPU_SCENE_ENCODE_TELEMETRY` with the new unambiguous fields and
   verify classic/HRC behavior through renderer tests.
4. Mirror renderer source changes into the game's vendored renderer, build the
   production GPU Release, and run no-readback and visual HRC High profiles.
5. Capture a representative frame with RenderDoc, record the GPU pass
   distribution, and name the first evidence-backed shader/temporal follow-up.

Invariants: no fence waits or command-buffer splitting in gameplay, no visual
change, no RC quality-tier change, and no interpretation of fence latency as a
GPU pass duration.

## Completion evidence

- `swift test --filter RadianceCascadesTests --jobs 4`: 10 tests passed.
- Production GPU Release build: completed in 217.75 seconds.
- No-readback HRC High profile: 300 frames, 4.20 ms median, 4.60 ms p95,
  230.62 reported FPS; one 16.49 ms outlier was attributed to CPU RC resource
  preparation rather than HRC GPU execution.
- RenderDoc named-event total: 1.974 ms, led by `Finish Quadrants` at 0.782 ms.
- Visual baseline supervisor completed twice with identical entity and cue
  signatures and no visible instrumentation change.
