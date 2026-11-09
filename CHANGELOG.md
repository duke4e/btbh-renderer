# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.1.1] — 2026-11-09

0.1.0 plus three opt-in additions requested by CHIMERA ([RFC 0006](docs/rfc/0006-scene-render-to-target.md)).
C code only: no shader, uniform or G-buffer changed. A host that doesn't call the new function or set the
new field renders exactly as with 0.1.0. Additive, so a patch release under the pre-1.0 rule in
`COORDINATION.md`.

### Added
- **A scene into a render target, apart from the window's frame** (`btbh_gpu_window_scene_render_to_target`,
  marker `BTBH_GPU_SCENE_HAS_RENDER_TO_TARGET`). It is meant for a picture inside the frame, such as a unit
  portrait. It takes the arguments of `btbh_gpu_window_scene_present_instances_with_visibility`, plus a render
  target of the window (`btbh_gpu_window_render_target_create`, any size).
  - The scene renders at its own render size and is upscaled to the target's size: FSR 1 or Lanczos, else
    bilinear. Rendered at twice the target's size, it is halved exactly.
  - It runs on a second scene runtime the window keeps for these scenes. That runtime has its own targets,
    shadow maps and pipelines, so the frame's targets keep their size.
  - It is never temporal: FSR 3 runs as FSR 1 and `post`'s `temporal_*` fields are ignored. The window's
    FSR 3 history, previous view-projection, bone palettes, retained shadow maps and radiance-cascade history
    are neither read nor written. The next frame reprojects as if the portrait had not been drawn.
  - Call it before the frame's scene present or after its canvas present. While a scene waits for its
    canvas, it is refused and the waiting frame is kept.
- **Per-batch reactive mask** (`BTBHGPUSceneGeometryBatch.reactive_mode`). `BTBH_GPU_SCENE_REACTIVE_NONE`
  keeps a transparent batch out of FSR 3's reactive mask, so it is accumulated like the opaque scene. This
  suits ground detail layers drawn over their base, which made the whole ground "reactive". The default
  (`BTBH_GPU_SCENE_REACTIVE_SCALED`, 0) is as before: alpha times `temporal_reactive_scale`.
- **What upscaling ran** (`BTBHGPUSceneReport.upscale_mode`). It is `post->upscale_mode`, or
  `BTBH_GPU_SCENE_UPSCALE_FSR1` where FSR 3 was asked for and is unavailable (the device, a failed encode, or a
  scene rendered to a target). Before, a fallback was only a log line.

### Notes
- The first scene rendered to a target creates its runtime: pipelines (a one-time hitch) and, with
  shadows, its own shadow maps (about 21 MB with the directional and spot maps).
- Known, unchanged: with `temporal_reactive_scale` 0, transparent batches leave the shading's ambient term in
  the reactive mask. The shader writes the mask only for a positive scale. `REACTIVE_NONE` does not have this
  problem.
- Not in this release: motion vectors for meshes updated in place (cloth, decals). The vertex shader would
  need each vertex's previous position, which is a vertex-layout change (RFC 0006, open questions).
- `release/v0.2.0` (the v0.2.0 candidate) must merge this before it is tagged.

## [0.1.0] — 2026-11-01 (first tagged release)

The first version under semver. The public C header `include/btbh_gpu/btbh_gpu.h` is the contract:
anything it declares is covered by semver from here on; the Swift package (`BTBHRenderer`,
`BTBHRendererGPU`) wraps it. This is what the SDL3 GPU (SDL_GPU) renderer offers today.

### Device, window, input and gamepad
- Device creation with capability validation (`btbh_gpu_device_create`, `btbh_gpu_validate_capabilities`, `BTBH_GPU_REQUIRED_FEATURES`).
- Windows with VSync/immediate/mailbox present modes, resize, minimize/restore; resizable windows open at a share of the display, centred, and keep running while minimized.
- Frame loop: `btbh_gpu_window_begin_frame`, `btbh_gpu_window_poll_events`, `btbh_gpu_window_present_clear`.
- Keyboard and mouse queries (`btbh_gpu_window_key_down/_pressed`, `btbh_gpu_window_mouse_button_*`), cursor visibility and warping, clipboard text.
- Gamepad snapshots and rumble (`btbh_gpu_window_gamepad_snapshot`, `btbh_gpu_window_gamepad_rumble` via `SDL_RumbleGamepad`).
- Native open/save file dialogs (`btbh_gpu_window_show_file_dialog`).
- Asynchronous GPU frame telemetry and per-pass CPU timings in `BTBHGPUSceneReport`; opt-in GPU cost log (GPU time and triangles per pass, per model).

### Textures and fonts
- Texture loading with nearest/linear/trilinear filtering, clamp/repeat/mirror wrap and sRGB/linear colour space (`btbh_gpu_window_texture_load_ex`).
- Off-screen render targets (`btbh_gpu_window_render_target_create`).
- TrueType font atlases rasterized on the GPU path, with glyph metrics (`btbh_gpu_window_font_load`, `btbh_gpu_font_glyph`, `btbh_gpu_font_atlas`).

### Models, materials, skinning and animation
- glTF model loading (cgltf) with relative material textures; Assimp import on Windows and Linux (`btbh_gpu_window_model_load_ex`).
- Model introspection: material slots, material info, mesh info, positions and indices (`btbh_gpu_model_material_info`, `btbh_gpu_model_mesh_copy_*`).
- PBR materials: albedo, normal, ORM and emission textures and factors; per-batch material overrides and patches.
- Per-batch blend (alpha/additive/multiplied/premultiplied), billboard, cull, depth-compare, stencil and alpha-cutoff state.
- Materials can stay out of the shadow maps; model patches can fade a model and pick their depth test.
- GPU skinning with animation libraries and poses (`btbh_gpu_animation_library_load`, `btbh_gpu_animation_pose_create`, `btbh_gpu_animation_pose_set_samples`).
- Geometry arena: model primitives share a few vertex and index buffers.

### Custom meshes and instancing
- Custom meshes, created and updated at runtime (`btbh_gpu_window_custom_mesh_create`, `btbh_gpu_custom_mesh_update`).
- Instance buffers and transform streams (`btbh_gpu_window_instance_buffer_create`, `btbh_gpu_pack_transform_stream_instances`).
- Persistent instance ranges, lazily uploaded, with adjacent persistent batches consolidated.
- LOD: persistent ranges small on screen draw a simplified model.
- Frustum and camera-ray queries; authored cluster culling for spot shadows.

### Scene lighting and shadows
- Direct PBR shading with ambient, procedural sky/ground/sun environment and SH irradiance (`BTBHGPUSceneLighting`).
- Primary and secondary directional lights with soft shadows; the primary shadow can cover a sphere around the camera target.
- Spot light with shadow, or an omni (cube) shadow (`spot_shadow_is_omni`); a near-occluder shadow share (`spot_occluder_shadow_strength/_reach`).
- Street lamps: up to `BTBH_GPU_SCENE_MAX_LAMPS` (40) per-pixel point lights per scene.
- Cloud shadows on the sun (`cloud_*`), surface wetness (`surface_wetness`), rain ripples (`rain`) and lit night windows (`window_glow`).
- Shadow-pass specialization and budgeting; scenes without shadows skip the shadow resources entirely.

### Surfaces
- Surface variants (`BTBHGPUSceneSurfaceVariant`): glitter (static and animated), water, dissolve, shatter, soft particle, rim light.
- Procedural surface detail (`detail_parameters`): ground, grass, facade, paving, glass and lamp-lens patterns with albedo variation and bump.
- Water with world-space waves; foam only on the highest crests.
- Render-only world curvature (`world_curvature`): small-planet and hyperbolic looks.
- Screen-space refraction (`BTBH_GPU_SCENE_SURFACE_REFRACT` = 10, `BTBH_GPU_SCENE_HAS_REFRACTION`): batches that show the scene
  behind them through a flowing displacement with dispersion, chromatic lines and a normal push for shockwaves, in a pass
  of their own after the transparent one; frames without one pay nothing (RFC 0005, requested by CHIMERA).

### Radiance cascades (global illumination)
- Holographic radiance cascades (`BTBHGPUSceneRadianceCascades`) with low/high/auto tiers, typed emitters (disk, rectangle, segment) and debug views.
- Status and per-pass telemetry, with a fallback reason when RC turns itself off (`BTBHGPUSceneRadianceCascadesStatus`).

### Post chain (`BTBHGPUScenePost`)
- SSAO (radius, intensity, contact strength, direct share) and screen-space reflections (`ssr_*`), resolved before effects.
- HDR composite: distance fog with world-edge fade, soft-threshold bloom, vignette.
- Tone mapping (Reinhard, PBR Neutral, ACES), exposure, log-space contrast, saturation, 3D LUT grading.
- Contrast-adaptive sharpening (CAS) and tilt-shift.
- Anti-aliasing: SMAA (medium/high) or FXAA.
- Upscaling: linear, Lanczos, FSR 1 (EASU + RCAS), FSR 3.1 temporal (`BTBH_GPU_SCENE_UPSCALE_FSR3`, with motion vectors, reactive mask and texture LOD bias; falls back to FSR 1).
- Horizontal mirror (`mirror_horizontal`).

### Canvas / 2D
- Canvas2D batches (`BTBHGPUCanvasPass`) with alpha/additive/multiplied/premultiplied blend modes, presented with the scene in one frame (`btbh_gpu_window_canvas_present`).

### Readback and capture
- Presented-frame readback (`btbh_gpu_window_request_presented_frame_readback`, `btbh_gpu_window_readback_presented_frame`); PNGs are written opaque (`btbh_gpu_write_png_rgba8`).
- Scenes render into sub-rectangles; a finished scene can be copied or captured aside (`btbh_gpu_window_copy_pending_scene_texture`, `btbh_gpu_window_capture_pending_scene_texture`).

### Platforms and build
- Windows and Linux on Vulkan (SPIR-V); macOS on Metal (metallib). There is no Direct3D backend.
- HLSL shaders in `shaders/gpu`, compiled by SDL_shadercross; pinned SDL3, SDL_shadercross and Assimp in `external/`.
- SwiftPM package (`Package.swift`, `Sources/`) and CMake; `BTBH_RENDERER_BUILD_ROOT` overrides the native build directory; native tests are opt-in (`BTBH_BUILD_GPU_NATIVE_TESTS`).

### From the tyrian3d branch (merged for v0.1.0)

The branch up to `92b3b35`. Its later commits (`96486ab`, `f7c3773`, `beaf727`, `89266d2`: smooth pixel-water
mask, PCSS sun shadows, HBAO SSAO, point-light ceiling) are planned for v0.2.0 as opt-in features.

- Raw RGBA8 textures from memory, region updates for streaming, and access to the SDL window (`btbh_gpu_window_texture_create_rgba8_ex`, `btbh_gpu_texture_update_rgba8`, `btbh_gpu_window_sdl_window`).
- Unlit surfaces that receive the primary directional shadow (`unlit_enabled = 2`); public per-batch shadow visibility (`btbh_gpu_window_scene_present_instances_with_visibility`).
- Glints surface for water drawn in textures (`BTBH_GPU_SCENE_SURFACE_GLINTS`).
- Small point lights without shadows for flashes, up to 32 (`point_light_count`, `point_lights`).
- Relief from albedo brightness for unlit surfaces under point lights (negative `normal_scale`).
- Procedural pixel water surface (`BTBH_GPU_SCENE_SURFACE_PIXEL_WATER`).
- Radiance cascades for pixel art: unlit receivers (`rc_receiver`) and an additive mode (`additive`).
- Dark scenes: unlit surfaces darkened, lit by the spot light, brightest pixels glowing (`unlit_darkness`, `unlit_glow_threshold`).
- Font atlases filtered linearly (text at any size).
- Mip caps: `generate_mipmaps` n > 1 limits a texture to at most n levels, for atlases whose cells would bleed.

### Changed for consumers of tyrian3d
- Surface variants renumbered: `GLINTS` 6 → 8, `PIXEL_WATER` 7 → 9 (master already had `SOFT_PARTICLE` = 6, `RIM` = 7); `BTBH_GPU_SCENE_SURFACE_COUNT` = 10.
- The tyrian3d bloom (`bloom_knee`, `bloom_radius` and its own bloom down/up/composite shaders) was dropped in favour of master's HDR-composite bloom: `bloom_enabled`, `bloom_intensity`, `bloom_threshold` (in exposed units), `bloom_soft_knee` (0..1), `bloom_scatter` (0..1).
- Only the primary directional shadow map is 2048; the secondary stays 512 (master's split), instead of tyrian3d's global 2048.
- Font atlases are now linear for all consumers.

### Fixed for zero-initialised hosts
- `BTBHGPUScenePost`'s vignette fields are only validated when `vignette_enabled` is set; before, a zeroed struct
  failed every scene present (`vignette_smoothness` had to be positive). Found by Tyrian3D on the Mac.
- `BTBHGPUScenePost.ao_radius` no longer has to be positive: zero (a host that never set the AO controls) keeps
  the previous fixed AO values (radius 1, intensity 0.4, contact 0.75, direct share 0.2). Before, every scene
  present from a zeroed `BTBHGPUScenePost` failed validation.

### Notes for every consumer
- **Changed look for hosts from before 2026-10-16** (added after the release): `contrast` scales in log space around
  mid grey 0.18 since `0722f5f`, so black stays black and darks no longer clip. With `contrast` above 1, darks and
  mid-tones come out lighter than with the old linear curve around 0.5 (CHIMERA's estuary at 1.12: shaded ground and
  the abyss). CHIMERA kept the new look. A switch back (`contrast_mode`) exists on branch `fix/contrast-mode` and
  ships only if a game asks for it.
- `BTBH_GPU_PHASE2_REQUIRED_FEATURES` is now `BTBH_GPU_REQUIRED_FEATURES`; the old name stays as a deprecated alias.
- FSR 3 is not available on Metal yet: six of its compute passes produce MSL whose bindings collide with SDL_GPU's
  Metal convention, so they are not built as metallibs and `BTBH_GPU_SCENE_UPSCALE_FSR3` falls back to FSR 1 on macOS
  (with a log line). Windows and Linux are unchanged. A proper binding fix is planned for v0.2.0.
- macOS builds no longer need `CMAKE_OSX_DEPLOYMENT_TARGET` to link SDL_shadercross, and `tools/check.sh` uses Unix
  Makefiles when Ninja is not installed.
- Hosts that zero-initialise the scene structs get every feature added since their last update switched off.
- The scene shader's uniform block grew (street lamps, clouds, point lights): rebuild the shaders together with
  the C core; mixing shader and C versions is not supported.

## History before v0.1.0

### 2025-02 – 2026-07: r3d upstream (Bigfoot71 and contributors)
- 2025-02: r3d starts as a raylib OpenGL deferred renderer: IBL, bloom, SSAO, soft shadows (dir/spot/omni), FXAA, instancing, forward pass, billboards, particles.
- 2025-03 – 04: PCSS shadows, AgX tonemapping, animated sprites, physically based bloom.
- 2025-07 – 08: GPU skinning and model animations, depth of field, fog, SSR, sprite shadows.
- 2025-11 – 12: decals, primitive instancing, complete SSR and skinning, draw-call grouping, SSIL, à-trous SSAO denoiser.
- 2026-01 – 03: procedural skyboxes, reflection probes, SMAA, custom instance buffers, animation state trees.
- 2026-04 – 07: custom instance attribute layouts, volumetric fog, handle pools, temp allocator.

### 2026-07: the fork and the SDL_GPU renderer
- 07-18: fork design for Be The Bullet Hell; package foundation, POD types and generational handles.
- 07-19 – 07-24: backend-neutral renderer seams: math, platform lifecycle, mesh/model streams, material descriptors, animation tree graphs, presentation viewport.
- 07-26: SDL_GPU vertical spike, GPU resource lifecycle, Phase 2 deterministic scene runtime with shadow maps (Windows gate closed).
- 07-27: production package: textures, glTF models and animations, Assimp on Windows, instancing, custom meshes, GPU skinning, font atlases, Canvas2D, frame capture/readback.
- 07-28: persistent instance buffers, PBR material channels, file dialogs, direct PBR, procedural IBL, directional shadows.
- 07-29 – 07-31: Windows Vulkan visual parity (SSAO, SMAA), FXAA, shadow specialization and budgets, pass profiling; Vulkan gameplay migration finalized.

### 2026-08 – 09: Metal and radiance cascades
- 08-02: production Metal renderer parity on macOS (adds Lanczos upscale, CAS, tilt-shift and LUT stages).
- 09-01: production radiance cascades renderer (classic and holographic).
- 09-03 – 09-05: lighting polish, omni shadows in the RC path, Metal RC stabilized, faster debug asset imports.

### 2026-10: one renderer, then features
- 10-01: r3d/OpenGL renderer, GPU spikes and classic radiance cascades removed; holographic RC kept, `shaders/gpu_spike` became `shaders/gpu` (~81k lines deleted).
- 10-02 – 10-03: horizontal mirror; HDR composite (fog, bloom, vignette); procedural surface detail; shadow-excluded materials.
- 10-05: softer shadows, window sizing, sub-rectangle scenes, soft particles, world-space water waves, continuous shatter collapse.
- 10-07: paving detail and the spot light's near-occluder shadow share.
- 10-10: Linux build and run (Vulkan, X11/Wayland); LOD for small persistent ranges.
- 10-13 – 10-14: FSR 1, rim-light surface variant, FSR 3.1 temporal upscaling with motion vectors.
- 10-15 – 10-18: world curvature, wetness, omni street lamps (32, then 40), GPU cost log, glass and lamp-lens detail, cloud shadows, SSR and tunable AO, geometry arena, rain ripples, gamepad rumble.
- 10-19 – 10-22: opaque presented-frame PNGs; water foam on the highest crests (HEAD `e95b392`).
- 10: tyrian3d branch (raw textures, pixel-art lighting, glints, pixel water) merged for v0.1.0.
