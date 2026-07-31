# Native scene-prepare telemetry design

Status: approved

Outcome: complete. W279b isolated bone preparation as approximately
`1.028 ms` of `1.048 ms` native prepare. W280 then attributed approximately
`1.009 ms` to repeated pose sampling/fill, which directly selected the
frame-local bone-sample deduplication candidate.

## Goal

Split the remaining Vulkan scene `cpu_prepare` time into actionable warm-frame
subphases before selecting another optimization.

## Design

Extend `BTBHGPUSceneReport` and `GPUScenePresentationProbeReport` with:

- setup: validation, matrix work, and warm resource checks;
- bones: geometry/bone counting, allocation, and pose sampling;
- pipelines: warm model-pipeline lookup and validation;
- buffers: byte sizing and warm upload-buffer capacity checks.

The native implementation records the extra timestamps only while
`BTBH_RENDER_PERF_LOG=1` has enabled window frame telemetry. With telemetry
disabled, the added report fields remain zero and no extra clock reads occur.
The existing aggregate `cpu_prepare` measurement remains unchanged.

When the bones phase is dominant, it is further split into count, allocation,
and sample/fill durations. The report also exposes animated-geometry and bone
matrix counts so repeated animation work can be distinguished from allocator
overhead.

The existing `BTBH_GPU_SCENE_ENCODE_TELEMETRY` line exposes all four fields so
the gameplay capture tooling can compute median and p95 values.

## Validation

1. GPU presentation test proves the detailed fields are populated when frame
   telemetry is enabled and fit within the aggregate prepare duration.
2. Renderer GPU tests and the production GPU game build pass.
3. A warmed gameplay-only capture identifies the dominant subphase.
4. No renderer optimization is retained without repeated gameplay A/B evidence.
