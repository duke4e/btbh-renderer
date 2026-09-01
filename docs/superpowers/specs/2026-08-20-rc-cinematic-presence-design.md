# Radiance Cascades Cinematic Presence Design

Status: approved

Date: 2026-08-20

## Goal

Make Radiance Cascades visibly shape the gameplay image through readable color
bleed and local bounce while retaining depth/normal rejection, confidence
gating, bounded energy, and raw diagnostic views.

The deterministic Windows capture establishes the baseline:

- High with intensity `0.00` is pixel-identical to RC Off in the measured world
  region.
- High at `0.90` changes 75.72% of world pixels by more than two code values,
  but averages only `3.66 / 255`, so the contribution is technically active yet
  visually subdued.
- The confidence view is broadly stable around the mid/high range. The current
  linear confidence multiplication therefore attenuates reliable bounce before
  the main post tonemap compresses it again.

## Approved approach

Keep the seed, trace, resolve, public intensity range, tier behavior, and raw
debug outputs unchanged. Improve only the final non-debug compose:

1. Remap confidence with a smooth threshold so uncertain samples stay
   suppressed while reliable mid/high-confidence samples approach full weight.
2. Apply a bounded, luminance-driven soft-knee presence gain to low and mid
   indirect energy. The gain preserves hue and trends to `1.0` for bright
   indirect values so highlights are not multiplied aggressively.
3. Continue applying authored intensity exactly once in resolve. The compose
   curve is a fixed presentation policy, not another user intensity control.

This is preferred over a global multiplier because it targets the actual loss
point and does not indiscriminately amplify low-confidence leakage. A seed or
trace rewrite is deferred until temporal capture proves a concrete propagation
or stability defect.

## Presets and defaults

- Subtle: `0.45`
- Balanced: `0.90`
- Strong: `1.50`

Balanced `0.90` becomes the renderer and game fallback default. RC remains
default-off. Low, High, and Auto continue to use identical energy for the same
configured intensity.

## Acceptance

- Raw cascade, hit-distance, confidence, and final-indirect debug views remain
  unchanged by the compose presentation curve.
- A deterministic High/Strong capture must average at least `7 / 255` absolute
  RGB delta from High/Zero in the measured world region and at least 30% of
  world pixels must differ by more than ten code values.
- Fewer than 0.5% of world pixels may darken by more than two code values.
- The result must show no obvious grid seams, hard cascade rings, NaNs, fallback
  colors, or broad highlight clipping in the approved gameplay capture.
- Native validation, local fallback, same-tier recovery, Auto transitions,
  canonical/vendor parity, and the existing Windows suites must remain green.
- Native Metal visual and temporal acceptance remains part of the Phase 5 Mac
  gate.

## Windows calibration evidence

The first conservative confidence window (`0.08...0.80`) was rejected after
the real Vulkan capture produced only `4.47 / 255` Strong MAE and 22.29% of
world pixels above a ten-code-value delta. The final measured window is
`0.03...0.60`; it still smoothly rejects very low confidence without adding a
confidence floor.

On the fixed `courser_frame`, seed `424242`, High/Strong (`1.50`) produces:

- `10.01 / 255` mean absolute RGB delta from High/Zero,
- 71.51% of world pixels above a ten-code-value delta,
- 0.17% of world pixels darkened by more than two code values, and
- only 0.029% newly clipped pixels whose zero-intensity source was below 192.

The raw confidence capture is pixel-identical before and after the presentation
change. Final-indirect remains spatially coherent with the authored red,
orange, purple, green, and yellow emitters, with no visible grid seam, hard
cascade ring, fallback color, or non-finite artifact in the fixed frame.
