# RC Environment Boundary Radiance Design

Status: approved

Date: 2026-08-21

Extends:
`docs/superpowers/specs/2026-08-21-production-directional-radiance-cascades-design.md`

## Problem

The production RC trace transports material and analytic emission correctly,
but rays that leave the source/occlusion field return zero radiance. Because RC
is the exclusive lighting path while enabled, non-emissive geometry becomes
nearly black even though the scene supplies ambient and directional lighting.

## Design

Treat the existing scene environment and directional lights as incoming
boundary radiance. Inject that radiance only into unobstructed rays in the
farthest cascade. Far-to-near RC composition then transports it through the
same radiance/transmittance representation as every other source.

- A procedural environment supplies its authored horizon/top radiance and
  ambient energy; scenes without one use ambient RGB as a constant boundary.
- Primary and secondary directional lights supply normalized cosine-power
  lobes centered on the direction toward each light.
- A source or occluder encountered by the farthest trace drives
  transmittance to zero and therefore blocks the boundary contribution.
- The solve domain must cover the camera-visible play space; pixels outside
  that domain cannot receive RC lighting and are treated as a configuration
  error, not an artistic shadow.
- For an occupied 2.5D probe, the obstacle top is the receiver plane. Free
  probes retain the ground receiver plane. Trace height rises with horizontal
  distance using the same elevation slope as material resolve, so equal-height
  roofs do not self-occlude, nearby tall geometry casts a horizon shadow, and
  distant footprints do not incorrectly block the whole sky hemisphere.
- Local lights and material emission remain source-field inputs.
- No legacy direct, ambient, or already-lit scene color is blended into RC.

This is the environment/sun boundary-radiance path required by the approved
production RC design, not a hybrid fallback.

## Initial tuning

Use a cosine-power exponent of 4 with analytic normalization over the 2D
direction domain. Keep the scene-authored HDR colors and energies unclamped;
make further artistic adjustments only after deterministic legacy/RC captures.

## Acceptance criteria

- RC-high makes non-emissive world geometry readable without legacy lighting
  and preserves authored procedural-sky color in the transported result.
- Environment light is absent behind opaque RC obstacles.
- Trace cost remains independent of material/analytic emitter count.
- Shader reflection, C layout assertions, renderer tests, live GPU validation,
  game bootstrap, and deterministic screenshot capture pass.
