# Production Directional Shadows Design

Status: approved automatically on 2026-07-28 under the repository
`AGENTS.md` instruction.

## Goal

Make `LightDescriptor.castsShadow` and `shadowSoftness` affect the production
SDL GPU model path for directional lights on Vulkan, while keeping the shader
and resource contract suitable for the existing Metal build path.

The result must render real scene geometry into shadow maps before the main
scene pass. The phase-2 shadow spike is test scaffolding and is not an
implementation of this requirement.

## Scope

The production path supports the two directional slots already selected by
`GPUResourceBackend.sceneLighting`. Each active directional light with positive
energy, positive range, and `castsShadow == true` receives a 2048 by 2048
single-sample D32 shadow map. Static and skinned model geometry, custom meshes,
persistent instance buffers, transient instances, and supported billboard
transforms use the same world transform as the main pass.

Opaque geometry casts shadows. Alpha-cutout and transparent shadow casting are
deferred to the raster-state/transparency phase because the current production
material path does not yet preserve the complete alpha/cull contract.

Spot and omni shadows, cascades, contact shadows, receiver masks, per-mesh
shadow modes, and user-configurable map sizes/biases are out of scope.

## Light and camera projection

Swift resolves the two selected `LightDescriptor` values into a production
lighting projection. It carries direction, color, energy, range,
`castsShadow`, and softness together so sorting cannot separate a shadow
contract from its light.

For each enabled shadow, Swift computes a camera-centered orthographic light
view-projection using the legacy directional algorithm:

- shadow near distance is `range / 1000` and shadow far coverage is `range`;
- the view-frustum bounding sphere uses the camera vertical FOV and drawable
  aspect;
- the sphere center is placed halfway through that camera range;
- the light basis chooses the cardinal axis least aligned with the normalized
  light direction;
- the center is snapped to one shadow-map texel in light-space X/Y;
- the light eye is extended 100 world units behind the sphere;
- the final projection covers `[-radius, radius]` in X/Y and the entire
  extended depth.

Invalid or degenerate directions, non-positive ranges, disabled shadow flags,
or zero-energy lights produce an identity matrix with `shadow_enabled == 0`.

## Native resources and passes

The per-window scene runtime owns:

- two D32 textures with both depth-target and sampler usage;
- one clamped linear sampler;
- one static and one animated depth-only graphics pipeline.

The shadow shaders are production shaders, compiled to SPIR-V and Metal
artifacts by the existing shader build. Their uniform/storage layout mirrors
the corresponding production model vertex shader. They output only clip-space
position and use the empty fragment shader required by SDL GPU, with no color
target or fragment resources.

After transient instance/bone uploads and before the swapchain scene pass, the
native renderer begins one depth-only render pass for each enabled directional
shadow. It clears to depth 1 and draws every opaque batch with the matching
static or animated shadow pipeline. The same geometry selection, instance
range, bone offset, vertex/index buffers, and billboard camera basis are used
as in the main pass.

The main fragment shader binds the two maps after the four material samplers.
It transforms receiver world position with the corresponding light matrix,
rejects coordinates outside the shadow volume, applies the legacy directional
constant/slope depth bias (`0.001` and `0.0015`), and averages a deterministic
3 by 3 PCF kernel. `shadowSoftness` remains authored in texels, matching the
legacy adapter, and is converted to UV distance by dividing by 2048.

Only the direct contribution from the matching directional light is multiplied
by its visibility. Ambient/IBL, emission, and the other directional light are
unchanged.

## Failure and lifecycle behavior

Shader, pipeline, texture, or sampler creation failures fail the scene
submission closed with the existing device-failure reporting. A disabled
shadow never samples undefined data. Every added GPU resource is released by
`btbh_gpu_window_release_scene`, including partial-initialization paths.

The public backend-neutral renderer API does not change. The C GPU bridge is
internal to the package and may grow the lighting structure and shader-path
arguments.

## Verification

Tests must first prove the missing behavior, then cover:

- light projection preserves `castsShadow`, range, and softness per sorted
  directional slot;
- invalid/disabled shadow inputs fail closed;
- a real Vulkan receiver region darkens when a raised opaque caster and
  shadowed directional light are present, while the same light with
  `castsShadow == false` does not;
- the production shader reflection contract includes six fragment samplers and
  the expanded uniform buffer;
- static and animated shadow shaders compile and reflect with their expected
  storage/uniform counts;
- native `/W4 /WX`, CTest, renderer Swift tests, game Swift tests, release
  build, API audit, and `git diff --check` remain green.

Final Windows acceptance uses two warmed five-mode captures. It requires clean
shutdown, 1920 by 1080 output, identical entity signatures, unchanged goldens
and thresholds, visual cue inspection, and diagnosis of any stable same-backend
median or p95 regression above five percent.

## Benchmark-driven no-shadow specialization

The first warmed A/B/C captures showed that the shadow-capable fragment shader
added a stable GPU-fence regression to modes where both directional shadows
were disabled. CPU submission remained unchanged, isolating the cost to the
shared production fragment pipeline.

The production shader is therefore compiled into two variants from the same
source:

- the six-sampler shadow-capable variant used when either directional shadow
  slot is enabled;
- a four-sampler no-shadow variant that compile-time removes both PCF paths and
  shadow resources.

The scene runtime owns matching static and animated model pipeline sets and
selects one set once per scene submission. No-shadow draws bind only the four
material samplers. This specialization must preserve identical pixels and the
existing material/light uniform layout while removing disabled-shadow shader
occupancy and descriptor overhead.
