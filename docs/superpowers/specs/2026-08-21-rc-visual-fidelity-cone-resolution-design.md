# RC Visual Fidelity: Sky, Player Cone, and High-Tier Resolution

Status: Approved

## Goal

Bring the production Radiance Cascades image materially closer to the original game's readability and color while preserving the RC-only lighting path whenever RC is enabled.

## Confirmed design

- The authored procedural sky reuses the engine's irradiance SH projection in the RC resolve, multiplied by RC-derived open-sky transmittance. This restores the full hemispherical sky contribution while keeping buildings between a receiver and the sky effective as blockers.
- A spot light remains a compact physical emitter. Its world-space direction and inner/outer cutoff cosines are rasterized into a 64-bin directional source atlas; trace and merge sample the matching angular bins. The illuminated cone is therefore a transport result, not a world-space emissive triangle, so intervening occluders terminate it.
- Local spot illumination also has a new-system direct layer: every spot instance rasterizes its bounded cone additively into a field-resolution irradiance target, and each covered fragment sphere-traces the same 2.5D occlusion SDF back to its emitter before contributing. This replaces the disabled legacy direct-light path, gives the player cone a stable source-centric shape, and keeps light-count scaling on instanced rasterization instead of a receiver-side loop over lights. RC continues to transport emissive/indirect radiance around that direct layer.
- The player's existing gameplay `.spot` light remains the single source of truth. No separate fake player glow and no legacy direct-light contribution are introduced in RC mode.
- High tier uses one world-field cell between base probes, a full-resolution indirect resolve, a 2x field-resolution direct target, and `64→128→256→512→1024` directions. Low tier retains the existing coarser profile for performance and remains the automatic fallback.
- The user intensity control scales transported local source energy only. The occluded direct cone and baseline sky environment retain their authored energy, so the control cannot regress into a global brightness slider.
- Engine direct-light energy and RC injection energy are stored separately on each emitter. This permits a readable player cone without forcing the indirect bounce into saturation.
- Spatial interpolation remains directional radiance reconstruction; post blur or exposure is not accepted as a substitute for transport resolution.
- Deterministic visual A/B captures use a quiet profile scenario that selects the requested frame and settles without applying the Growth 5 weapon/growth stress sequence. The existing stress scenario remains unchanged for performance coverage.

## Acceptance criteria

- In `finalIndirect`, the player contribution has a visible forward cone with a feathered outer edge and no symmetric white disk around the player.
- A building intersecting that cone blocks the direct field through SDF visibility; pixels behind it do not receive the unoccluded spot contribution.
- Rotating the player light rotates the RC cone because the directional-source shader consumes `direction`, `innerCutoff`, and `outerCutoff`, while trace/merge select the receiver-to-source angular bin.
- The high-tier contract reports full drawable resolution and base probe spacing of one field cell; low tier remains quarter-resolution with two-cell base spacing.
- Authored sky hue survives the RC SH path, its energy follows the environment ambient authoring exactly once, and occluders still block the result through cascade transmittance.
- The debug menu exposes the directional source atlas (and the direct field once present), so a missing source can be distinguished from transport or composition failures without changing shaders.
- RC composition remains mutually exclusive with legacy lighting.
- Renderer unit tests, shader-contract tests, a real GPU probe, game build/tests, and deterministic legacy/RC/debug captures pass.
- Quiet-profile captures contain only the player's baseline weapon loadout, so sky, cone shape, occlusion, and probe-grid artifacts can be judged without projectile VFX covering the scene.

## Affected components

- `shaders/gpu_spike/production_rc_source.vert.hlsl`
- `shaders/gpu_spike/production_rc_source.frag.hlsl`
- `shaders/gpu_spike/production_rc_directional_source.vert.hlsl`
- `shaders/gpu_spike/production_rc_directional_source.frag.hlsl`
- `shaders/gpu_spike/production_rc_direct_light.vert.hlsl`
- `shaders/gpu_spike/production_rc_direct_light.frag.hlsl`
- `shaders/gpu_spike/production_rc_trace.comp.hlsl`
- `shaders/gpu_spike/production_rc_merge.comp.hlsl`
- `shaders/gpu_spike/production_rc_resolve.comp.hlsl`
- `Sources/BTBHRenderer/RenderResources.swift`
- `Sources/BTBHRendererGPU/GPUBackendsPending.swift`
- `src/gpu/btbh_gpu_radiance_cascades.c`
- `Tests/BTBHRendererTests/RadianceCascadesTests.swift`
- `tests/btbh_gpu_radiance_cascades_test.c`
- Vendored renderer copies in `Be-The-Bullet-Hell/City Generator (r3d)/Vendor/BTBHRenderer`
- Game profile harness files used for deterministic quiet visual captures

## Risks and controls

- Full-resolution resolve and denser high-tier probes increase GPU cost. The low tier is unchanged, telemetry is captured, and visual quality is evaluated separately from frame time.
- The directional atlas adds 32 MiB at a 256x256 field (64 RGBA16F bins arranged 8x8). It preserves additive rasterization and does not introduce a per-ray light loop, so overlapping and numerous lights retain the source-field scaling model.
- The SDF-occluded direct field adds one 2x field-resolution RGBA16F target on High (base field resolution on Low). Fragment cost scales with covered cone area and SDF steps; it does not scale every screen receiver by the total light count.
- Cone cutoff inputs are cosines. Invalid or omni sentinel cutoffs must continue to select the non-cone path.
