# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- **Models share external textures** (asked for by Be The Bullet Hell, whose 37 army models each uploaded
  their own copy of the same `textures/mc_*.png`): `BTBH_GPU_MODEL_IMPORT_SHARE_EXTERNAL_TEXTURES` for
  `btbh_gpu_window_model_load_ex`. Models loaded with it share one texture per external image (a glTF image
  with a `uri`) and format, through a window-level cache keyed by the image's normalised resolved path: an sRGB
  albedo or emission texture never shares with a linear normal or ORM one. A composed ORM texture is shared by
  its pair of external images, so it is composed once. Embedded (buffer-view) images stay each model's own.
  The path key is lexical (`\` and `/` alike, `.` and `name/..` folded); spellings that differ otherwise
  (letter case, links) load twice, as without the flag. Without the flag nothing changes.
- Ownership, now in the header: a model's textures (`BTBHGPUModelMaterialInfo`) belong to the model. A shared
  one is counted per material slot that holds it; the last model destroyed frees it, the window's destroy frees
  what is left, and `btbh_gpu_texture_destroy` on it does nothing (so a host can't free it twice). Any other
  model texture is the model's alone, as before.
- `BTBH_GPU_MODEL_HAS_SHARED_EXTERNAL_TEXTURES`, defined when the flag exists, for hosts that build against
  several releases.
- Swift: `ModelImportFlags.shareExternalTextures`, and `ModelMaterialPatch.emissionTexture`, which replaces
  an imported material's emission texture as `albedoTexture`, `normalTexture` and `ormTexture` replace theirs.
  It wins over `clearEmissionTexture`. Load it with `colorSpace: .sRGB` to match the importer. Existing calls
  compile unchanged.
- **Readable small text** (RFC 0004, option A; prototype for the games to accept): `btbh_gpu_window_font_load_ex`
  bakes a font from a `BTBHGPUFontDesc` with stb_truetype's packer, oversampled (`oversample_x`, `oversample_y`,
  up to 8) and prefiltered. `BTBHGPUFontGlyph` then has fractional offsets and advances and UVs that undo the
  oversampling, so a glyph drawn on the 1/`oversample_x` pixel grid stays sharp and spacing stays even. Its atlas
  is always linear. `snap_advances` rounds advances and kerning to whole pixels. `coverage_gamma` (0.25 to 4,
  0 = 1) is applied once to the atlas while baking: 1.2-1.4 thickens light-on-dark text, and the canvas shader
  doesn't change. An invalid descriptor returns NULL with `BTBH_GPU_INVALID_ARGUMENT` and the reason in the
  report.
- `btbh_gpu_font_metrics`: ascent, descent, line gap and size in pixels (for a plain font, what its report says).
- `btbh_gpu_font_kerning(font, left, right)`: the font's kerning in pixels (GPOS pairs or its kern table),
  precomputed between the baked glyphs. 0 for fonts from `btbh_gpu_window_font_load`.
- `BTBH_GPU_HAS_FONT_LOAD_EX`, defined when these exist, for hosts that build against several releases.
- Native test `btbh_gpu_font_smoke` (GPU): `_ex`'s validation, its metrics against the plain loader (within a
  pixel), kerning, the coverage curve, and a line of text at 10, 12 and 16 px with and without `_ex`, read back
  into PNGs (`font-smoke/` in the build directory). Its face is Titillium Web SemiBold (OFL), added under
  `Tests/Fixtures` with its licence. `check.ps1 -SkipGpuTests` and `SKIP_GPU_TESTS=1 check.sh` skip it.

- **V-Sync through `GPUPlatform`** (asked for by Be The Bullet Hell, which opens its window through it):
  `GPUPlatform.setPresentMode(_:)` switches the present mode between frames without reopening the window
  (`.vsync` for on, `.automatic` for off), and `GPUPlatform.presentMode` says what the window took. While a
  chosen mode resolves to V-Sync, a `.displayRefresh` frame-rate policy stops throttling and leaves the pacing
  to the swapchain, so a whole-hertz throttle can't miss a vblank on a panel that is a little faster; `.fixed`
  caps still apply. Until a game calls it, pacing is as before.

### Upgrading
- Nothing changes unless a model is loaded with `BTBH_GPU_MODEL_IMPORT_SHARE_EXTERNAL_TEXTURES` or a patch sets
  `emissionTexture`.
- Nothing changes unless a game calls `btbh_gpu_window_font_load_ex`: `btbh_gpu_window_font_load` is byte for
  byte the same. No shader changed.
- Swift: no font wrapper yet (Be The Bullet Hell doesn't need one).

## [0.3.0] — 2026-11-11

0.2.1 plus a choosable present mode (V-Sync), asked for by CHIMERA's Graphics tab. New API, opt-in: a zeroed
`BTBHGPUWindowDescriptor` takes the same present mode as before, and nothing else changed. No shader changed.

Tested on Windows (Vulkan): `check.ps1` with the GPU smokes, the Swift build, and CHIMERA's build and on-screen
test at 60 Hz. In a running mission, V-Sync toggled on and off without recreating the window, and the report
followed. On capped the frame rate at the refresh. Off left 7 scenes with 0 differing pixels against 0.2.1, at
the same frame rate within noise. macOS (Metal) isn't verified, as for 0.2.0.

### Added
- **Choosable present mode**: `BTBHGPUWindowDescriptor.present_mode` (a `BTBHGPUPresentMode` in a byte that
  was `reserved`, so the struct keeps its size). `BTBH_GPU_PRESENT_MODE_INVALID` (0) takes the first of
  IMMEDIATE, MAILBOX and VSYNC that the window supports, as before. `BTBH_GPU_PRESENT_MODE_VSYNC`,
  `_IMMEDIATE` or `_MAILBOX` asks for that mode, and takes VSYNC, which every window supports, where the window
  lacks it. Any other value fails `btbh_gpu_window_create`. `BTBHGPUWindowReport.present_mode` says what was
  taken.
- `btbh_gpu_window_set_present_mode(window, mode, report)`: switches the mode at run time, without recreating
  the window, by the same rules (0 is the default choice again). The swapchain keeps its SDR composition.
  Call it between frames: with a frame pending (a scene presented, its canvas not yet) it changes nothing and
  returns `BTBH_GPU_INVALID_ARGUMENT`. Switching rebuilds the swapchain and may wait for the GPU to finish
  the frames in flight; a request that resolves to the mode in use does nothing. Before the claim it sets the
  mode the claim asks for.
- `BTBH_GPU_WINDOW_HAS_PRESENT_MODE`, defined when both exist, for hosts that build against several releases.
- Swift: `GPUPresentMode` (`.automatic`, `.vsync`, `.immediate`, `.mailbox`), `GPUWindowProbe`'s
  `presentMode:` parameter (defaults to `.automatic`) and `GPUWindowProbe.setPresentMode(_:)`. Existing calls
  compile unchanged. `GPUPlatform` and `WindowDescriptor` don't take a present mode yet.

### Upgrading
- Nothing to set: a zeroed descriptor, `= {0}` or `..Default::default()`, presents as in 0.2.1.
- For a V-Sync option, V-Sync on is `BTBH_GPU_PRESENT_MODE_VSYNC`, and V-Sync off is 0 (the fastest mode the
  window has). Asking for IMMEDIATE instead gives VSYNC on a window that has only MAILBOX besides it.

## [0.2.1] — 2026-11-11

0.2.0 plus a skinning fix found by CHIMERA. No API changed, but the skinned vertex shaders and the model
loader did: rebuild the shaders together with the C core.

Tested on Windows (Vulkan): `check.ps1` with the GPU smokes, the Swift build, and CHIMERA's build and on-screen
A/B (its original infantry no longer shard at 175 units). macOS (Metal) isn't verified, as for 0.2.0.

### Fixed
- **Skinned vertices away from the world origin** were thrown towards or away from it. A vertex blended from
  several bones moved by (Σw − 1) times its instance's translation, along the line through the world origin.
  At 175 units from the origin that is about 1 unit, enough for long shards on CHIMERA's infantry. Present
  since 0.1.0. Two causes, both fixed:
  - The skinned vertex shaders took the skinned point's w from the blended bone matrix, which makes it the
    weights' sum rather than 1, and the instance matrix then scaled its translation by that sum. They now
    take the point with w = 1. This covers `production_animated_model.vert` (the scene draw, its previous
    pose for motion vectors, and the radiance-cascade source variant
    `production_rc_animated_material_source.vert`) and `production_animated_shadow.vert`.
  - Loading quantised each bone weight to a 255th on its own, so a multi-bone vertex's weights added up to
    254–257 255ths. They now add up to exactly 255: each share is rounded down, and the 255ths left over go to
    the largest remainders. Weights that already added up to 255 (every single-bone vertex among them) pack
    exactly as before.

### Known
- A rigid mesh on a node inside a skin's skeleton (a soldier's gun under its hand) is drawn where the file
  puts it at rest; it doesn't follow its node through the pose. Following it is a feature for a later release.
  Until then, bind such meshes into the skin offline (CHIMERA does so with `asset-tool bind-skin`).

### Upgrading
- All three games: bump to `v0.2.1` and rebuild the shaders. There is nothing to set.
- Skinned models with vertices blended from several bones change where they were wrong. Where such a vertex's
  weights didn't add up to 255, one or two of them change by a 255th, and the vertex comes back to where its
  bones put it: by up to 2/255 of its instance's distance from the world origin.
- Every other skinned vertex moves only by float rounding: its instance's translation is added in a different
  order. A/B captures show scattered single edge pixels on skinned meshes, more of them further from the
  origin, and nothing visible side by side. CHIMERA, whose skins all add up to 255 already, measured this at
  1920 × 1080: 28 pixels differ near the origin and 318 at 175 units out (8 mechs), with no shape change.
  Static meshes are unchanged.

## [0.2.0] — 2026-11-10

Tyrian3D's last four `tyrian3d` commits, ported to master as opt-in features: with every new field zeroed
a scene looks as it did in 0.1.0. After this release the `tyrian3d` branch is retired and all three games
build from tags on `master`.

It also contains 0.1.1 (a scene into a render target, per-batch reactive mask, `upscale_mode` in the
report; RFC 0006), merged from `master`.

Tested on Windows (Vulkan) with all three games:
- CHIMERA is byte-identical to 0.1.0.
- Be The Bullet Hell builds unchanged from `e95b392` and keeps its look.
- Tyrian3D matches `tyrian3d` @ `89266d2` with the settings below.

macOS (Metal), including the new HBAO metallibs, isn't verified yet. Its check is pending in issue #1 and
didn't hold up this release.

### Added
- **Soft sun shadows** (from `f7c3773`): `BTBHGPUSceneLighting.directional_shadow_filter`
  (`BTBHGPUSceneShadowFilter`). `BTBH_GPU_SCENE_SHADOW_FILTER_TENT` (0) is the filter as before;
  `BTBH_GPU_SCENE_SHADOW_FILTER_SOFT` (1) is percentage-closer soft shadows: a blocker search, a penumbra
  0.035 world units wide per unit of the blockers' height, times `directional_shadow_softness` (at least
  0.75 texel), and 4 to 32 comparison taps against the receiver's own plane. With the soft filter,
  `unlit_enabled` 3 is a plain four-tap lookup for see-through layers that are drawn many times. With the
  tent filter, 3 behaves like 2. As before, unlit batches in the transparent pass with no surface variant
  take the fast unlit pipeline, which has no shadows.
- `btbh_gpu_window_scene_set_directional_shadow_size`: sets the primary directional shadow map's size
  (256–8192 each way; 2048 × 2048 by default), for a light-space region that isn't square.
- **Horizon-based ambient occlusion** (from `beaf727`): `BTBHGPUScenePost.ssao_enabled` 2. It runs at half
  resolution and uses 4 directions of 4 steps. A depth-aware denoise and a depth-aware upsample follow, and
  the result is laid over the opaque scene before the transparent draws.
  - New post fields: `ssao_radius`, `ssao_intensity`, `ssao_power` and `ssao_bias`. A field left at 0 takes
    its default (1, 1, 1, 0.03).
  - `ssao_contact` darkens all the light but the emission.
  - `ssao_debug_view` shows the occlusion alone, in grey.
  - Per-batch `ssao_mode` (`BTBHGPUSceneSSAOMode`: full, receive only, cast only, none).
  - Procedural pixel water never takes it.
  - Its ambient mode subtracts light through the blend, so it needs the floating-point scene colour. On
    devices that fall back to 8-bit colour, only the contact mode and the radiance-cascade mode darken.
  - The SSAO of `ssao_enabled` 1 (with `ao_*` and the SSR in its resolve) is unchanged. With HBAO on,
    screen-space reflections still run through that resolve.
- **Point-light ceiling** (from `89266d2`): `BTBHGPUSceneLighting.point_light_ceiling` (luminance, finite;
  0 means none). The light that point lights add to a surface eases out towards the room the surface has left under
  the ceiling. Dark ground still flashes bright, and pale ground under a stream of shots doesn't burn white.

### Changed
- `PIXEL_WATER`'s mask is smooth (from `96486ab`): it is interpolated between its texels and cut at a half,
  with an edge about a screen pixel soft. Shores no longer run in the game's pixel steps. Only Tyrian3D uses
  the surface.
- The scene shader's samplers changed: the primary directional map is bound again at t8 through a
  comparison sampler, and the omni map moved to t9. Rebuild the shaders together with the C core.
- The tent filter reads the primary map's size from the texture, so it is unchanged at the default 2048.
- New appended fields (`point_light_ceiling`, `directional_shadow_filter`, `ssao_mode`, the `ssao_*` post
  fields) carry reserved padding. Zero-initialise the structs, as before.

### Upgrading
- **Be The Bullet Hell** (from `e95b392`):
  - Bump the submodule to `v0.2.0` and rebuild the shaders.
  - There is nothing to set. The look stays the same, because 0.1.0's fixes only change zeroed fields and the
    log contrast is already in `e95b392`.
  - Text: the font atlas filters linearly since 0.1.0.
    - Text drawn at whole pixels is unchanged.
    - A pixel font drawn at half a pixel (centred labels, outlines) gets a soft fringe.
    - Round text positions and outline offsets to whole pixels to keep it sharp.
  - `build-gpu-game` stages an explicit shader list. Add `production_hbao*.spv` to it before using
    `ssao_enabled` 2.
  - On macOS, FSR 3 falls back to FSR 1.
- **CHIMERA** (from `v0.1.0` or `v0.1.1`): point `renderer.pin` at a `v0.2.0` checkout and rebuild the shaders. There is
  nothing to set.
- **Tyrian3D** (from `tyrian3d` @ `89266d2`):
  - Bump the submodule to `v0.2.0`.
  - In its CMake, change `shaders/gpu_spike` to `shaders/gpu` (the source path and its `-I`).
  - To keep its look, set:
    - `lighting->directional_shadow_filter = BTBH_GPU_SCENE_SHADOW_FILTER_SOFT`;
    - `post->ssao_enabled = 2` instead of 1, keeping its `ssao_*` values and the per-batch `ssao_mode`.
  - `point_light_ceiling`, `set_directional_shadow_size` and `unlit_enabled` 3 keep their names.
  - Two looks from 0.1.0 differ from `tyrian3d` and are retuned in the game rather than kept as options:
    - Bloom: `bloom_knee` and `bloom_radius` are gone. Retune with master's `bloom_intensity`,
      `bloom_threshold` (in exposed units, so the old threshold times the exposure), `bloom_soft_knee` and
      `bloom_scatter`. Master blends its mip levels, where `tyrian3d` added all six, so the intensity is
      about 6.4 times the old one (Tyrian3D: 0.35 became 2.25, with knee 0.2 → soft knee 0.16, radius
      1.2 → scatter 0.75).
    - Radiance cascades (`1917761`): a source is as opaque as it is bright against the light behind it, so
      rays pass the soft edges of additive glows. The light around explosions is slightly brighter.

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
  - The scene renders at its own render size. When it is smaller than the target, it is enlarged with
    FSR 1 or Lanczos (as `upscale_mode` asks). At the target's size or bigger it is scaled bilinearly without
    sharpening, so a render twice the target's size is halved exactly.
  - It runs on a second scene runtime the window keeps for these scenes. That runtime has its own targets,
    shadow maps and pipelines, so the frame's targets keep their size.
  - It is never temporal: FSR 3 runs as FSR 1 and `post`'s `temporal_*` fields are ignored. The window's
    FSR 3 history, previous view-projection, bone palettes, retained shadow maps and radiance-cascade history
    are neither read nor written. The next frame reprojects as if the portrait had not been drawn. Each call
    also starts afresh itself: its shadow maps are redrawn whatever the shadow update periods, and it keeps no
    cascade history from the last portrait.
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
- The first scene rendered to a target creates its runtime: pipelines (a one-time hitch of about 10–20 ms
  CPU in the smoke) and, with shadows, its own shadow maps (about 21 MB with the directional and spot maps).
- `BTBH_GPU_COST_LOG`: a scene rendered to a target is no frame of its own. It counts in the frame it is
  drawn for, and it doesn't split the GPU timing.
- Known, unchanged: with `temporal_reactive_scale` 0, transparent batches leave the shading's ambient term in
  the reactive mask. The shader writes the mask only for a positive scale. `REACTIVE_NONE` does not have this
  problem.
- Not in this release: motion vectors for meshes updated in place (cloth, decals). The vertex shader would
  need each vertex's previous position, which is a vertex-layout change (RFC 0006, open questions).
- Merged into `release/v0.2.0` (the v0.2.0 candidate) on 2026-11-10.

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
