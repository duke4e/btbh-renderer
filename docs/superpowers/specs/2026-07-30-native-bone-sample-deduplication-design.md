# Native bone-sample deduplication design

Status: approved

Outcome: retained. W281 reduced 66 animated-geometry samples to two and 7,614
uploaded bone matrices to 162. Native prepare fell from approximately
`1.035 ms` in W280 to `0.097 ms`. Repeated W282/W283 gameplay captures retained
the canonical entity signature, clean `1920x1080` output, and a two-run
`18.963/24.151 ms` median/p95 mean.

## Evidence

W280 attributes `1.009 ms` of the `1.017 ms` median bones phase to pose
sampling/fill. The gameplay frame contains 66 animated geometries and uploads
7,614 bone matrices. The principal player GLB has 30 skinned primitives that
all reference one 123-joint skin, so the current per-primitive sampling repeats
identical pose work.

## Design

During one scene preparation, cache sampled bone ranges by exact animation
source and skin:

- animation-pose submissions use pose pointer identity plus skin index;
- direct library submissions use library pointer, animation index, exact time
  bits, and skin index.

The first geometry for a key samples matrices and records its bone-buffer
offset. Later geometries reuse that offset. The outer geometry-offset array
remains one entry per prepared geometry, so shadow and geometry encoding keep
their existing cursor contract. Only unique matrices count toward the upload.

The cache is frame-local and freed before scene encoding. It does not persist
mutable animation state across frames and does not alter shaders, materials,
draw ordering, or instance data.

## Validation

1. Report `animationSampleCount <= animatedGeometryCount` and the reduced
   unique bone-matrix count.
2. Focused GPU presentation and animation math tests pass.
3. Two warmed gameplay captures retain the canonical entity signature,
   1920x1080 output, clean shutdown, and visual equivalence.
4. Retain only if repeated gameplay median/p95 and native sampling telemetry
   show a measurable improvement.
