# RC HDR Tone Mapping Design

Status: approved

Date: 2026-08-22

## Goal

Preserve the color and structure of strong Radiance Cascades lighting while
mapping its HDR output safely to the SDR swapchain. The gameplay reference
energy of 30 remains unchanged.

## Proven failure

- The RC source, cascade, indirect, composed, and scene-color textures are
  `RGBA16F`; HDR precision is retained through RC composition.
- The production game enables the color LUT and configures extended Reinhard
  with `tonemapWhite = 0.5`.
- The LUT currently saturates its HDR input to `[0, 1]` before color grading.
  The following output pass then maps each RGB component independently; with a
  white point of 0.5, a component of 0.5 maps to 1.0 and larger components map
  above 1.0 before the SDR target clamps them.
- In the deterministic HRC High reference capture, 33.47% of pixels contain an
  exactly-255 channel and 10.17% are pure white. The RC-Off reference is 13.52%
  and 1.37%, respectively.

The first destructive boundary is therefore the HDR-unaware LUT, followed by
the final Reinhard output. Increasing cascade or scene-buffer precision cannot
recover values after either clamp.

## Approved behavior

- Promote `TonemapMode` from an ignored descriptor field to an explicit GPU
  contract with two modes: legacy extended Reinhard and Khronos PBR Neutral.
- Keep extended Reinhard and its current ordering unchanged for RC-Off so the
  legacy lighting path retains its established look.
- Select PBR Neutral whenever gameplay RC is enabled. Its reference algorithm
  accepts non-negative linear Rec.709, guarantees linear Rec.709 output in
  `[0, 1]`, avoids hue-angle shifts, and compresses/desaturates only the HDR
  highlight region.
- Apply PBR Neutral exactly once at the first SDR-constrained boundary:
  inside the LUT pass when the LUT is enabled, otherwise in the final output
  pass. Final output must not tone-map an already-mapped LUT result again.
- Use one shared HLSL implementation from the LUT, standard output, and fused
  tilt-shift output paths so the equations cannot drift.
- `tonemapWhite` remains the legacy Reinhard white point and is intentionally
  ignored by PBR Neutral. RC light authoring values and `indirectIntensity`
  remain unchanged.

## Acceptance

- Focused math tests prove PBR Neutral is continuous around its compression
  boundary, produces finite values in `[0, 1]` for representative HDR colors,
  and preserves the expected color ordering.
- Source/reflection tests prove the mode crosses Swift, the C ABI, the post
  uniform, and all three relevant shaders; LUT-enabled PBR output is mapped
  once rather than twice.
- Renderer shaders and tests pass, renderer changes are synchronized to the
  game vendor checkout, and the game production release build succeeds.
- A deterministic HRC High A/B capture shows materially fewer clipped-white
  pixels without reducing the 30-point gameplay light reference, with frame
  time remaining within measurement noise of the current post chain.

## Sources

- Khronos PBR Neutral specification and reference implementation:
  https://github.com/KhronosGroup/ToneMapping/tree/main/PBR_Neutral
- Khronos release rationale and adoption notes:
  https://www.khronos.org/news/press/khronos-pbr-neutral-tone-mapper-released-for-true-to-life-color-rendering-of-3d-products
