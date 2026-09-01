# Radiance Cascades Dedicated Light Sources Design

Status: approved and implemented

Date: 2026-08-20

## Problem

The current RC seed subtracts ambient diffuse/specular from the already-lit
scene color. This makes RC inherit every screen-space brightness change and
causes indirect intensity to behave like a stylized brightness control rather
than the strength of propagated light.

## Chosen design

`RadianceCascadesDescriptor` owns a bounded list of dedicated point-light
sources. A source contains world position, authored `RenderColor`, energy, and
range; the GPU bridge converts that color to linear space. It does not
participate in the ordinary direct-light pass.

The native bridge copies at most 16 sources into the RC configuration. The seed
compute pass receives those sources plus the inverse view-projection matrix,
reconstructs each visible surface's world position from depth, and evaluates
distance attenuation and Lambert response. Its output is sourced only from
these authored emitters; scene color and ambient subtraction are removed from
the seed path.

The first game integration authors one moving RC source above the player. This
is intentionally independent of the existing player spotlight and establishes
the runtime contract for later explosion, projectile, neon, and encounter
sources.

## Boundaries

- Version one supports point emitters only and no emitter shadows.
- At most 16 active, finite, positive-energy/range sources reach the GPU.
- Source ordering is stable and excess sources are ignored.
- An empty source list produces black RC indirect rather than falling back to
  scene brightness.
- Existing tier, debug-view, auto-budget, invalidation, and intensity behavior
  remains unchanged.
- Legacy rendering ignores the RC descriptor as before.

## Acceptance

- `finalIndirect` is black with no dedicated sources, regardless of scene
  brightness.
- A colored source produces localized, range-limited seed radiance on nearby
  facing surfaces.
- Moving the game-authored player source changes the indirect-light location.
- Public Swift and native validation reject non-finite or invalid sources.
- Shader reflection, focused RC tests, the full renderer/game suites, and a GPU
  runtime smoke pass.
