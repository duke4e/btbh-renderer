# RC Performance, Resolve Cache, and HRC Design

Status: approved

Date: 2026-08-22

Builds on:

- `2026-08-21-production-directional-radiance-cascades-design.md`
- `2026-08-21-rc-responsive-high-fidelity-design.md`

## Goal

Reduce the measured Radiance Cascades frame overhead while increasing the
spatial fidelity of transported light. Preserve the current production
directional solver as a reference and introduce a paper-conformant,
single-frame Holographic Radiance Cascades backend as an explicit experimental
option for the fixed-camera 2.5D world domain.

The work must improve quality by changing where radiance is represented and
evaluated. Increasing the existing classic RC field globally is explicitly not
the strategy: doubling both dimensions would multiply its largest atlases and
their bandwidth by approximately four.

## Baseline and confirmed bottlenecks

The deterministic 115-projectile capture measured:

- RC High median frame: 23.9 ms;
- RC Off median frame: 8.8 ms;
- measured RC overhead: approximately 15.1 ms.

RC High already resolves at native render resolution. Its visible block size
therefore comes from the world transport/occlusion domain, probe spacing, and
cascade interpolation rather than the final indirect target.

The production merge always enables the reprojected bilinear fix. In that
branch every lower-cascade output traces four corrected intervals and never
reads the raw lower-cascade interval texture. Nevertheless, the runtime first
dispatches raw directional traces for every lower cascade. Those dispatches
are dead production work. Merged cascade debug views also read the merged
textures, not the raw textures.

## Research basis

The HRC backend follows:

- Freeman, Sannikov, and Margel, *Holographic Radiance Cascades for 2D Global
  Illumination*, arXiv:2505.02041;
- the Amitabha HRC reference implementation at
  `https://github.com/entropylost/amitabha`;
- the radiance/transmittance over operator already required by the production
  directional RC design.

For one quadrant, HRC keeps full probe resolution perpendicular to the traced
direction and halves it only parallel to that direction. Cascade `n` therefore
has probes at `(x * 2^n, y)`, doubles its angular direction count, and preserves
constant endpoint spacing. Four rotated quadrant solves are summed at the end.

Long intervals are not raymarched conventionally. The acceleration pyramid
stores short radiance/transmittance intervals `T_n`. Even child directions are
formed by exact near-over-far composition; odd child directions average the
two neighboring composed approximations. The radiance pyramid `R_n` then
merges down from coarse to fine using the odd/even probe rules in equations
14-17 of the paper.

## Architecture decision

### Backends

Add an explicit RC backend selector:

- `classic`: the current directional cascade implementation and default;
- `holographic`: the new experimental HRC implementation.

Tier and backend are orthogonal. `off`, `low`, `high`, and `auto` continue to
describe quality/budget behavior. Existing callers that do not specify a
backend remain on `classic` with no behavior or ABI-value changes to the tier
enum.

HRC initially accepts a square, power-of-two primary occlusion domain. If its
requirements or resources are unavailable, the runtime falls back to the
equivalent classic RC tier for that frame and reports the fallback in status;
it must never silently disable all new lighting.

### Stage A: remove dead classic work

When the reprojected bilinear merge is active, only the highest classic
cascade receives a raw directional trace dispatch. Cascades below it are
created entirely by merge-down. Raw lower textures may remain allocated during
the first implementation to keep bindings and rollback simple, but they are
not populated.

GPU debug labels include the cascade index and operation (`Trace C4`,
`Merge C3 <- C4`, and so on) so RenderDoc/Nsight captures expose useful pass
boundaries. This is observability, not a claim of portable in-engine GPU
timestamps.

### Stage B: material-independent diffuse irradiance cache

Classic High retains all 64 C0 transport directions. Before full-screen
material resolve, a compute pass convolves those directions once per spatial
probe into a compact nine-bin diffuse irradiance atlas:

- one upward-facing normal bin;
- eight azimuthal tilted-normal bins around the XZ plane.

Each texel stores RGB irradiance and cosine-weighted mean transmittance. At
native-resolution material resolve, rough non-metallic pixels interpolate the
up bin and the two neighboring tilted bins, including normal direction and
spatial probe interpolation. This changes the dominant cost from many
directional atlas samples per screen pixel to a small fixed number of cache
samples. Glossy, metallic, downward-facing, and debug-sensitive pixels retain
the existing 16-direction BRDF path.

The cache is derived from the full 64-direction merged C0 atlas, not the
16-direction glossy prefilter, so diffuse angular integration gains fidelity
while reducing full-screen work. Direct cone lighting, authored emission,
environment lighting, confidence visualization, and the RC-only composition
contract remain unchanged.

### Stage C: HRC 2x transport domain

For a 256x256 primary world field, HRC High targets a 512x512 probe/output
domain mapped over the same world bounds. The scale is bounded so neither
dimension exceeds 1024 and the resource estimate remains inside a 128 MiB HRC
working-set budget. The source and occlusion textures are sampled in normalized
world coordinates, allowing the renderer to reuse the current emitter and SDF
contracts. The higher probe density improves transport interpolation even when
the source SDF originated from the lower-resolution gameplay field.

The GPU representation uses `RGBA16F` intervals (`RGB` linear fluence or
radiance plus scalar transmittance):

- one `T_n` 2D atlas per acceleration level;
- two ping-pong `R_n` atlases, because merge-down needs only adjacent levels;
- a four-quadrant field output;
- two textures for the opacity-aware cross blur.

HRC base levels 0-2 trace only short segments. Higher `T_n` levels are produced
by merge-up. `R_n` is produced coarse-to-fine by merge-down. The four quadrant
outputs are combined in a final compute pass and filtered with the paper's
1-pixel cross kernel, rejecting neighbors with materially different opacity.

The first HRC backend is intentionally a diffuse GI backend. The paper's final
output contains four quadrant fluence values and does not itself implement
glossy reconstruction. HRC material resolve therefore uses those quadrant
values for diffuse local lighting and retains authored direct light plus
environment specular. It must not claim parity with classic directional local
specular. A later directional HRC extension may query higher `R_n` levels for
glossy response without changing the public source/occlusion contracts.

### Single-frame invariant

All accepted quality and performance results are produced from the current
frame. Temporal reprojection is not used to hide spatial, angular, or
checkerboard artifacts. Temporal reuse remains an optional later optimization
for additional diffuse bounces only after the single-frame backend passes.

## Public and internal interfaces

The C and Swift RC descriptors gain a backend field with a default of
`classic`. RC status exposes the requested and active backend so captures can
prove which solver rendered a frame. Backend changes invalidate solver
resources and increment the RC generation.

Internal CPU layout helpers describe HRC grid sizes, direction counts, atlas
dimensions, and estimated bytes. These helpers are unit-tested independently
of GPU availability and reject overflows, non-power-of-two domains, invalid
levels, oversized textures, and working sets over budget.

## Acceptance criteria

### Correctness and compatibility

- Classic remains the default and preserves existing public tier values.
- With reprojected merging enabled, production classic output is bit-identical
  before and after dead lower trace dispatch removal.
- All current RC debug views continue to render meaningful data.
- Rough diffuse cache output remains within 0.015 tone-mapped RGB RMSE of the
  classic 16-direction reference on the deterministic capture; glossy and
  metallic materials continue to use the reference path.
- HRC uses only current-frame source and occlusion information and preserves
  the radiance/transmittance over operator.
- HRC shows no persistent checkerboard pattern after opacity-aware cross
  filtering and does not blur across solid/open field discontinuities.
- RC-off continues to use the untouched legacy renderer.

### Performance and resolution

- Dead lower trace dispatch removal is proven in a GPU capture or deterministic
  dispatch telemetry.
- Classic High improves over the 23.9 ms median baseline without reducing its
  C0 transport direction count or native output resolution.
- HRC High runs a 2x linear transport domain while keeping median RC overhead
  at or below 10 ms on the Windows comparison machine. If it misses that gate,
  HRC remains experimental and the capture records the failing pass budget.
- HRC resource allocation remains at or below 128 MiB for its supported
  working set.

## Risks and rollback

- The diffuse cache is an approximation for rough dielectrics. Its material
  gate keeps glossy/metallic response on the exact directional path, and the
  entire pass can be disabled independently.
- HRC may trade classic directional specular for higher diffuse spatial
  fidelity. It remains opt-in until screenshots and gameplay approve that
  tradeoff.
- HRC acceleration storage is memory-heavy. CPU layout validation prevents an
  allocation attempt outside the budget; classic High remains the rollback.
- The repository currently contains a large uncommitted RC implementation.
  This work edits only the documented RC/API/shader/test surfaces and must not
  discard or normalize unrelated user changes.
