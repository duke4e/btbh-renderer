# Production Procedural-IBL and Material-AO Design

**Status:** Approved automatically on 2026-07-28 under the repository
`AGENTS.md` instruction.

## Goal

Replace the production GPU path's placeholder ambient-color approximation
with environment-derived diffuse and specular lighting for the procedural-sky
ambient maps used by the game, and apply the packed material occlusion channel
to ambient light without changing the public renderer API.

## Scope

When an `EnvironmentDescriptor` references an ambient map created from a
procedural sky, the Swift GPU backend will deterministically project that sky
into nine real spherical-harmonic coefficients. The projection uses cubemap
texel-center directions, solid-angle weights, and the normalized cosine
convolution factors required for diffuse irradiance.

The production fragment shader will:

- evaluate the irradiance SH at the world-space normal for the diffuse lobe;
- sample the same procedural-sky equation analytically at the reflection
  direction for the specular lobe;
- broaden rough reflections toward the low-frequency SH result;
- use an analytic environment-BRDF approximation and the existing material
  Fresnel inputs;
- multiply diffuse ambient by `ORM.r * material.occlusion`;
- apply Lagarde-style specular occlusion;
- retain ambient-color diffuse fallback when no valid ambient map exists.

The neutral C ABI carries a bounded, version-local scene-lighting payload:
the nine RGB SH coefficients, procedural-sky parameters needed by the shader,
and an IBL-enabled flag. The existing public Swift descriptors and resource
handles do not change.

## Procedural-sky contract

Projection and shader sampling use the existing descriptor fields:

- ground-bottom and ground-horizon colors and energy;
- sky-horizon and sky-top colors and energy;
- their horizon curves;
- sun color, direction, energy, angular size, and fade curve.

The implementation follows the legacy procedural-sky shape closely enough to
preserve the current authored contract, while the specular convolution and
BRDF use bounded analytic approximations rather than creating native
prefiltered cube textures and a BRDF lookup texture.

## Compatibility rules

- Unlit materials remain independent of environment and AO.
- The zero-total-light compatibility path remains stable and finite.
- The explicit `.invalid` ambient-map handle uses the existing ambient-color
  fallback. A destroyed, stale, or otherwise nonzero missing handle retains
  the renderer's established fail-closed resource semantics and never reads
  stale sky data.
- AO affects indirect light only; it does not attenuate directional lights or
  emission.
- All SH and procedural parameters crossing the C ABI must be finite.
- No additional shader samplers are introduced in this slice.

## Acceptance criteria

1. Pure Swift tests prove deterministic, finite, chromatic SH generation and
   correct invalid-map fallback.
2. A real Windows Vulkan render test proves that packed material occlusion
   attenuates indirect diffuse light.
3. A real Windows Vulkan render test proves that a metallic surface receives
   colored, view-dependent environment specular from a valid procedural
   ambient map.
4. Existing direct-PBR, unlit, emission, animation, billboard, depth, and
   material-channel tests remain green.
5. The native `/W4 /WX` build, native CTest, full base Swift suite, and full
   GPU Swift suite pass.
6. The game package passes full tests and release Vulkan build/bootstrap.
7. Two warmed capture series for each of the five game modes retain identical
   entity signatures, clean shutdown, and 1920x1080 output. Goldens and
   comparator thresholds remain unchanged.
8. Median and p95 regressions above 5% are diagnosed against retained Windows
   Run 3/4 results and the preceding production-GPU series.

## Explicitly out of scope

- imported or arbitrary cubemap sources not expressible as a procedural sky;
- reflection probes and local parallax correction;
- native prefiltered environment textures and exact legacy BRDF-LUT parity;
- screen-space ambient occlusion despite transport of `ssaoEnabled`;
- shadows, point/spot lights, fog, bloom, SSR, DoF, and other post passes;
- golden or threshold changes;
- claiming the documented Windows-to-Apple text/antialiasing SSIM issue is
  resolved;
- Mac or Metal execution.

## Risks and mitigations

- **Approximation drift:** tests assert semantic response rather than exact
  legacy pixels, and closeout documentation names the analytic approximation.
- **C ABI layout mistakes:** native validation and Swift render tests cover
  the complete lighting payload.
- **SH convention errors:** a constant-sky invariant and directional/chromatic
  projection tests guard normalization and basis orientation.
- **Performance regression:** SH is generated only when creating an ambient
  map; frame work is a fixed uniform upload and shader evaluation. The full
  warmed benchmark gate remains mandatory.
- **False SSAO/IBL completeness claims:** the report distinguishes material AO,
  procedural IBL, arbitrary cubemaps, and screen-space AO.
