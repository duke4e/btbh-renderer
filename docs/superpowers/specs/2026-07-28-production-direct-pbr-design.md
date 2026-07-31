# Production Direct-PBR Design

**Status:** Approved automatically on 2026-07-28 under the repository
`AGENTS.md` instruction.

## Goal

Use the normal and packed ORM material channels already preserved by the
production SDL_GPU path to render view-dependent direct lighting on Vulkan,
while keeping the same source ABI viable for the later Metal closeout.

## Scope

The production model and animated-model vertex shaders will emit world
position, world normal, and world tangent data. The production fragment
shader will sample albedo, emission, normal, and ORM textures and evaluate
the two existing directional lights with:

- tangent-space normal mapping and the material normal scale;
- Burley diffuse;
- GGX distribution and Smith geometry;
- Schlick Fresnel;
- roughness and metalness from `ORM.g` and `ORM.b`, multiplied by the
  corresponding material factors;
- a fixed dielectric specular control of `0.5`, matching the legacy default
  until the public material ABI carries this channel.

The caller will pass camera position through the neutral C ABI so the
fragment shader can compute view-dependent highlights. The runtime will bind
four fragment samplers and create a neutral `(0.5, 0.5, 1.0)` normal-map
fallback in addition to the existing white fallback.

The existing simple ambient color remains in this slice. It contributes to
the diffuse lobe and preserves the current zero-light visibility behavior.
The packed occlusion channel and factor remain transported, but AO is not
applied until the ambient/IBL slice can define correct behavior for glTF
files whose occlusion texture is separate from their metallic-roughness
texture.

## Shader and ABI contract

- Shadercross compacts the static shader's `TEXCOORD6` tangent semantic to
  reflected location 4; the animated shader retains tangent at location 6
  because joints and weights occupy 4 and 5. Static pipelines therefore bind
  locations 0 through 4, while animated pipelines bind all seven locations.
  Both read tangent bytes from the same vertex offset, so the model memory
  ABI does not change.
- The fragment shader exposes exactly four samplers and one uniform buffer.
- The scene presentation C ABI adds a required three-float camera-position
  argument after camera-up.
- Swift validates and forwards camera position from the active
  `FrameDescriptor`.
- Unlit materials bypass direct and ambient PBR and preserve the current
  albedo-only behavior; the legacy-compatible unlit path suppresses emission.
- Zero-length normals or tangents fail soft to a stable geometric basis
  instead of producing NaNs.

## Acceptance criteria

1. Generated reflection reports four fragment samplers, the expected
   varyings, and tangent input locations 4 static / 6 animated.
2. Unit and native contract tests reject a missing or non-finite camera
   position.
3. A real Windows Vulkan render test proves that normal-map direction changes
   the lit result and that roughness/metalness change the specular result.
4. Existing unlit, emission, animation, billboard, depth, and material tests
   remain green.
5. The native `/W4 /WX` build, native CTest, full base Swift suite, and full
   GPU Swift suite pass.
6. The game package passes full tests and a release GPU build/bootstrap.
7. Two warmed capture series for each of the five game modes (ten launches
   total) retain deterministic entity signatures, clean shutdown, and
   1920x1080 output. No golden or threshold authority changes are allowed.
8. Median and p95 regressions greater than 5% against the retained Windows
   Run 3/4 authority are diagnosed before closeout.

## Explicitly out of scope

- image-based lighting, reflection probes, and BRDF LUT integration;
- applying the occlusion channel to ambient/IBL;
- shadow maps;
- point and spot lights;
- tone-map, bloom, fog, SSAO, SSR, DoF, or other post passes;
- changing goldens, visual thresholds, or the documented cross-platform
  text/antialiasing SSIM limitation;
- Mac or Metal execution in this Windows phase.

## Risks and mitigations

- **Material brightness changes:** direct PBR is intentionally different from
  vertex Lambert lighting. Existing semantic tests and the unchanged visual
  comparator expose unintended changes.
- **Tangent ABI mismatch:** reflection assertions and real static/animated
  render coverage guard the fixed locations.
- **NaNs from degenerate input:** shader basis construction clamps lengths
  and uses deterministic fallback axes.
- **False parity claims:** closeout documentation names the omitted AO/IBL,
  shadow, and post work explicitly.
