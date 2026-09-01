# Production Directional Radiance Cascades Design

Status: approved

Date: 2026-08-21

Supersedes:

- `2026-08-20-rc-dedicated-light-sources-design.md`
- the bounded-source and hybrid-lighting parts of
  `2026-08-20-rc-shared-lighting-visibility-design.md`

## Goal

Replace the current screen-space multi-scale gather with a paper-conformant
directional radiance-cascade solver that is the complete lighting path while
enabled. Scene emission, not a bounded analytic-light array or already-lit
scene color, is the source of radiance. The solve must retain angular
information until material resolve, produce stable soft penumbras in a single
frame, and scale independently of the number of emissive sources.

Legacy PBR lighting remains available only as the explicit RC-off renderer.
No production frame combines legacy direct/ambient lighting with an RC
indirect overlay.

## Research basis

The design follows these primary and reference sources:

- Osborne and Sannikov, *Radiance Cascades: A Novel High-Resolution Formal
  Solution for Multidimensional Non-LTE Radiative Transfer*:
  https://doi.org/10.1093/rasti/rzae062
- Sannikov, *Radiance Cascades: A Novel Approach to Calculating Global
  Illumination* draft and Path of Exile 2 implementation notes:
  https://github.com/Raikiri/RadianceCascadesPaper
- Freeman, Sannikov, and Margel, *Holographic Radiance Cascades for 2D Global
  Illumination*: https://arxiv.org/abs/2505.02041
- Freeman and Sannikov, *Split Radiance Cascades: Real-Time Global
  Illumination via Sparse Radiance Probes*:
  https://arxiv.org/abs/2607.20384
- Amitabha HRC reference implementation:
  https://github.com/entropylost/amitabha
- RadianceCascadesUnity reference merge and packing implementation:
  https://github.com/agrdino/RadianceCascadesUnity

The production solver initially targets the game's fixed-camera 2.5D city
domain: directional flatland probes live in world XZ and trace through the
existing signed-distance/top-height field. This is a complete RC
specialization for the scene topology, not a screen blur or a bounded-light
approximation. Public source and storage contracts remain suitable for a
future sparse 3D Split RC backend when gameplay requires unrestricted camera
height or volumetric GI.

## Non-negotiable invariants

1. Every cascade stores directional radiance intervals. Angular samples are
   not averaged before the far-to-near merge completes.
2. Cascade `i + 1` has lower spatial resolution, higher angular resolution,
   and a longer non-overlapping interval than cascade `i`.
3. Intervals store linear HDR radiance and transmittance. Merging uses the
   radiative-transfer over operator:

   `L = L_near + T_near * L_far`

   `T = T_near * T_far`

4. Upper-cascade probes are spatially interpolated and child directions are
   directionally filtered before merging into the lower cascade.
5. RC source count does not appear in the trace or resolve shader loop.
   Emissive geometry and analytic gameplay emitters rasterize additively into
   a world-space HDR source field before tracing.
6. RC-on geometry never evaluates legacy ambient, directional, spot, or omni
   PBR light functions. It writes material/G-buffer data and authored
   emission; the RC resolve owns diffuse local illumination.
7. RC-off uses the existing legacy PBR and shadow path unchanged.
8. The single-frame result is the quality baseline. Temporal reprojection is
   optional and cannot be required to hide spatial or angular aliasing.

## Cascade representation

### Probe-tile atlas

Each cascade uses one `RGBA16F` probe-tile atlas. Every spatial probe owns a
2D tile whose texels are its ordered angular samples:

`atlasX = probeX * directionTileWidth + directionX`

`atlasY = probeY * directionTileHeight + directionY`

This follows the practical Radiant 2D/reference packing and prevents 4x
angular branching from producing an over-wide direction-strip texture.
Spatial interpolation explicitly addresses the same direction in four probe
tiles, so filtering never crosses probe or direction boundaries.

For cascade `i`:

- probe spacing is `baseProbeSpacing * 2^i` world cells;
- direction count is `baseDirectionCount * angularBranching^i`;
- interval start/end follow contiguous exponential ranges;
- the 4x production profile grows both angular-tile dimensions by `2^i`,
  keeping atlas dimensions approximately constant as probe counts fall;
- the 2x performance profile alternates angular-tile growth between axes,
  so total storage falls with higher cascades as described by the flatland
  analysis.

The initial quality profiles are data-driven rather than shader constants:

- low: 4 cascades, 4 base directions, 2x angular/interval branching;
- high: 5 cascades, 16 base directions, Radiant-style 4x
  angular/interval branching, plus the four-ray bilinear/parallax fix.

The 2x profile is the paper's asymptotic flatland layout. The 4x profile is
the higher-quality scaling reported for Radiant 2D and used by the practical
reference implementation; it intentionally spends constant storage per
cascade for finer angular resolution. Profile validation enforces
power-of-two direction tiles, contiguous intervals, complete domain coverage,
and device texture limits.

### Interval tracing

Each atlas texel traces exactly one world-space interval in one direction.
The interval traversal samples the world source field and the signed-distance
plus height occlusion field. Opaque hits set transmittance to zero; emissive
hits contribute linear radiance before occlusion terminates the interval.
Empty intervals retain unit transmittance.

The initial implementation uses deterministic fixed-cell traversal for
reference correctness. SDF-assisted empty-space skipping may replace it only
after GPU readbacks prove bitwise-equivalent hit ordering for the conformance
scenes.

### Far-to-near merge

Cascades are traced independently, then merged from the farthest cascade to
cascade zero. For each lower interval, the corresponding child directions in
the next cascade are filtered, four neighboring upper probes are bilinearly
sampled, and each upper interval is composed behind the lower interval with
the over operator.

The research profile applies the published bilinear/parallax fix: lower
intervals are reprojected toward each of the four upper probe origins before
the four independently merged results are spatially combined. This is never
approximated by a constant `lerp` weight.

## Unlimited emitter field

The public API exposes frame-scoped `RadianceEmitter` submissions rather than
a fixed array. Emitters contain stable identity, world position and height,
shape/radius, linear radiance, and optional direction/cone metadata.

The backend grows its upload buffer geometrically and rasterizes all emitters
in one or more instanced additive draws into the active world-space source
field. Culling is spatial: emitters outside every active cascade domain are
ignored for that frame. There is no numeric light budget and no importance
ranking that silently drops in-domain emitters.

Authored material emission is captured into the same field. Gameplay lights
such as projectiles, beams, explosions, loot, hazards, bosses, signs, windows,
and fire are emitter geometry/data, so RC solve cost depends on cascade
resolution, not source count. Source injection cost remains proportional to
submitted emitter coverage and is reported separately.

## Exclusive lighting modes

The renderer owns an explicit lighting mode:

- `legacy`: existing forward PBR, analytic lights, and shadow maps;
- `radianceCascades`: material/G-buffer and emission capture followed by the
  directional RC solve and RC material resolve.

RC is not a post effect in the second mode. The scene-color input to RC
compose is replaced by albedo/material/emission inputs. Environment and sun
lighting enter as boundary radiance or emitter data. A requested RC mode that
cannot prepare its pipelines/resources returns a visible RC failure status;
it does not render a hybrid frame. A caller may explicitly switch the next
frame to `legacy`.

## Spatial and temporal reconstruction

The RC resolve reconstructs world-space receiver position and evaluates the
merged directional field against the receiver normal and diffuse BRDF.
Spatial reconstruction is depth-, normal-, and world-position-aware. It must
not mix across geometry discontinuities.

Temporal history is a later optional quality layer with true A/B history
textures, previous view-projection data, motion/reprojection, disocclusion
rejection, neighborhood clipping, and bounded history weight. Static
single-frame conformance remains required with temporal accumulation disabled.

## Resource flow

The RC runtime owns:

- world-space HDR emission/source field;
- world-space occlusion/height fields;
- one interval atlas per cascade;
- one merged atlas per cascade when in-place merge hazards require it;
- full-resolution RC material resolve output;
- optional temporal history A/B only when temporal mode is enabled.

Dedicated cascade textures are preferred to two generic ping-pong textures
because each cascade has distinct dimensions and the farther cascade remains
an immutable input while merging the next level. Resource cycling is not
temporal history.

## Diagnostics and conformance scenes

Debug views expose:

- source radiance and emitter coverage;
- probe grid and selected direction;
- raw interval radiance;
- interval transmittance;
- each unmerged and merged cascade;
- final diffuse irradiance;
- spatial reconstruction confidence;
- temporal acceptance/rejection when enabled;
- legacy/RC mode and fallback reason.

Reference CPU tests and GPU readbacks cover an empty domain, one area emitter,
one blocker and penumbra, two colored emitters, a thin occluder, a source
behind a wall, and 10,000 in-domain emitters. The GPU result is compared with
the CPU interval/merge reference at atlas-texel level before visual tuning.

## Acceptance criteria

- With RC enabled, zero legacy ambient/directional/local-light evaluations
  contribute to final scene color.
- With RC disabled, legacy output remains within existing regression bounds.
- Cascade atlases retain distinct directional samples through merge.
- A finite-size emitter produces a continuous penumbra without a final
  Gaussian blur or temporal history.
- A wall blocks direct and propagated radiance without leaking through its
  represented height range.
- 10,000 visible emitters are accepted without source truncation; RC trace
  dispatch dimensions and shader loop counts are unchanged from the one-
  emitter case.
- Static output is deterministic and stable with temporal accumulation off.
- Vulkan/SPIR-V and Metal/metallib shader reflection, native/Swift tests,
  GPU readbacks, renderer suites, vendored game suites, and Windows/macOS
  build packaging pass.

## Migration and rollback

The existing approximate RC shaders and 16-source ABI remain only until the
new conformance path reaches visual parity in isolated tests. Production RC
mode cannot select them. Once the new path passes conformance and game smoke,
the old shaders, fixed source arrays, hybrid compose, and obsolete tests are
deleted together.

Rollback is the explicit `legacy` lighting mode. No compatibility switch
retains the approximate RC implementation.
