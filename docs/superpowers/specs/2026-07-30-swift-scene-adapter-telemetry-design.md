# Swift scene-adapter telemetry design

Status: approved

Outcome: complete. W284 measured adapter prepare at approximately `0.595 ms`
and isolated combined lighting/clear/post work at `0.521 ms`. W285 then split
that result into `0.0038 ms` scene lighting, `0.0011 ms` clear, and
`0.5171 ms` screen-post resolution, selecting the immutable screen-shader kind
cache rather than the neutral light-snapshot experiment.

## Goal

Split the remaining `GPUSceneAdapter` prepare median before choosing another
gameplay optimization.

## Design

When `BTBH_RENDER_PERF_LOG=1`, report these subphases:

- camera basis calculation;
- lighting, clear-color, and post-state resolution;
- spot-shadow batch culling;
- opaque/material batch ordering;
- adjacent persistent-range consolidation and screen-clear claim.

All added clock reads remain behind the existing frame-telemetry flag. The
aggregate adapter `prepare_ms` remains unchanged and the subphases do not alter
the batch or scene data.

## Validation

Build the production GPU game, capture one warmed gameplay workload, and
select only the largest measured subphase for the next isolated experiment.
