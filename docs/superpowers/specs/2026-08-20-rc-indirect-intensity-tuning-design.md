# Radiance Cascades Indirect Intensity Tuning Design

Status: superseded by `2026-08-20-rc-cinematic-presence-design.md`

Date: 2026-08-20

## Problem and goal

The first Phase 5 calibration ties indirect-light energy to the resolution tier:
Low resolves at `0.25` strength and High at `0.35`. Even the
`final-indirect` debug view is therefore too subtle for useful visual tuning.
Quality controls must select resolution and sampling cost, not change authored
light energy.

The goal is to expose one validated indirect intensity across the native and
Swift renderer seams, use it identically for Low and High, and make the three
approved visual presets directly selectable from the game's interactive CMD
launcher.

## Approved contract

- Add `indirect_intensity` to `BTBHGPUSceneRadianceCascades` and
  `indirectIntensity` to `RadianceCascadesDescriptor`.
- The renderer accepts finite values in the closed range `0.0...2.0` and rejects
  invalid configuration through the existing typed fallback path.
- The default is Balanced, `0.60`. RC remains default-off, so existing scenes
  that do not opt in remain allocation-, dispatch-, and appearance-identical.
- Low, High, and Auto use the configured value unchanged. Tier transitions may
  change resolution and sample count but not indirect-light energy.
- Resolve continues to apply material response and compose continues to apply
  confidence. Intensity does not bypass either safety mask and is applied only
  once.

## Game tuning surface

The game reads `BTBH_RADIANCE_CASCADES_INTENSITY`. Missing, non-finite,
negative, or greater-than-two values fall back to `0.60`.

The interactive `run-radiance-cascades.cmd` menu adds a third selection:

- Subtle: `0.35`
- Balanced: `0.60`
- Strong: `0.90`

The command-line-compatible form accepts the preset name as its third optional
argument. The launcher translates preset names to numeric environment values;
the game and renderer still validate the final value independently.

## Acceptance

- Descriptor projection preserves intensity from game to native C.
- Native validation covers valid bounds, negative, over-limit, and non-finite
  values.
- `final-indirect` responds to the preset while Low/High energy remains equal
  for the same intensity.
- Existing RC fallback, Auto transition, and default-off behavior remain intact.
- Canonical and vendored renderer sources remain synchronized.
