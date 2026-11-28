# Consumers

The games that build on btbh-renderer, how each one gets it and what it uses. The coordinator keeps
this current; a game's session asks (an issue) when something here changes. Rules: [`COORDINATION.md`](COORDINATION.md).

| | Be The Bullet Hell | CHIMERA | Tyrian3D | Be The Bullet Hell v2 |
| --- | --- | --- | --- | --- |
| Language | Swift (SwiftPM) | Rust | C (OpenTyrian fork) | Rust (a rewrite in `v2/`) |
| Platforms | Windows, macOS 12+, Linux (Steam Deck) | Windows (Vulkan) | macOS (Metal), Windows (Vulkan) | Windows (Vulkan); Linux and Steam Deck planned (`tools/build-linux.sh`), not yet run on them |
| Repository | `Be-The-Bullet-Hell`, branch `main` | `RTS`, branch `main` | `Tyrian3D`, branch `tyrian3d/m1-tools` | `Be-The-Bullet-Hell`, folder `v2/`, branch `main` |
| Machine / session | Windows PC, BTBH session | Windows PC, CHIMERA session in `RTS/` | Mac and Windows PC, Tyrian3D sessions | Windows PC, BTBH v2 session |
| Consumes through | git submodule `City Generator (r3d)/Vendor/BTBHRenderer`, SwiftPM `.package(path:)`; products `BTBHRenderer`, `BTBHRendererGPU` | `crates/btbh-gpu-sys/build.rs` compiles `src/gpu/*.c` from `BTBH_RENDERER_DIR` (default `../btbh-renderer`), bindgen over `btbh_gpu.h` | git submodule `external/btbh-renderer`; its CMake globs `src/gpu/*.c` into a static `btbh_gpu` | `v2/crates/btbh-gpu-sys/build.rs` compiles `src/gpu/*.c` from the checkout `v2/renderer.pin` names (`BTBH_RENDERER_DIR` overrides it), bindgen over `btbh_gpu.h` |
| Native deps | renderer's CMake (`build-gpu-game.ps1/.sh`, `build-gpu-game-linux.sh`): SDL3 static, Assimp, shadercross | `BTBH_RENDERER_PREBUILT` (SDL3-static, Assimp DLL; `tools/build_prebuilt.ps1`) | system SDL3, Assimp, zlib | `BTBH_RENDERER_PREBUILT` (SDL3-static, Assimp DLL; `tools/stage_prebuilt.py`) |
| Shaders | renderer's CMake → SPIR-V / metallib staged into `GPUShaders/` | `tools/build_shaders.py` (shadercross → SPIR-V in `assets-cache/shaders`), list read from the renderer's `CMakeLists.txt` | its CMake: shadercross → SPIR-V → MSL → `xcrun metal` metallib, list read from the renderer's `CMakeLists.txt` | `v2/tools/build_shaders.py` |
| Pinned version | submodule at tag `v0.5.1` (BTBH `ca6744ac`, 2026-11-12) | `renderer.pin` → `../btbh-renderer-v0.14.2` (tag `v0.14.2`; CHIMERA `7e7c905c`, 2026-11-25) | submodule at tag `v0.2.1` (Tyrian3D `60c1b596`, 2026-11-11) | `v2/renderer.pin` → `../../btbh-renderer-v0.16.1` (tag `v0.16.1`; BTBH `55921df8c`, 2026-11-28) |

## Features used

**Be The Bullet Hell** (only through the Swift layer, `import BTBHRenderer` / `BTBHRendererGPU`; no direct
`btbh_gpu_*` calls): window and fullscreen, gamepad (incl. rumble), canvas/2D and fonts, model loading
with materials and patches, surface details, persistent instanced ranges with LOD, custom meshes,
skinning and animation trees, lights (directional, spot, omni, street lamps), procedural sky, fog and
fog edge, clouds, wetness/rain, shadows, radiance cascades with emitters, post chain (SSAO/SSR, bloom,
vignette, tonemap, AA), upscaling (`fsr1`, `fsr3`), frame readback for render baselines, file dialog.
The most demanding consumer: it defines most of the scene API. From v0.4.0: V-Sync from its Graphics menu
(`GPUPlatform.setPresentMode`, `.vsync` or `.automatic`, off by default) and `.shareExternalTextures` on its army
bodies. From v0.5.1: craters (a torus rim and an ellipsoid bowl) and collapse rubble (rounded boxes) through
`GPUSdfTerrain`, one per city. It has a heightfield base at its dug floor tiles' tops, 0.1-unit (0.5 m) cells, 32-cell
chunks, smooth normals, concurrent jobs and a 1 ms update budget. It is drawn through `GPUPlatform.setSdfTerrain`;
`dugByTerrain` is set on the floor tiles' `ModelMaterialPatch` and the kerb's `MaterialDescriptor`.

**CHIMERA** (`crates/chimera-platform/src/gpu.rs`, bindgen): device and window, mouse buttons (keyboard
and audio via raw SDL), fonts (`window_font_load`, glyphs, atlas), textures (`window_texture_load_ex`:
sRGB/linear, trilinear, repeat), models (`window_model_load_ex`, ignore material textures), animation
library and poses, custom meshes (create, update), `window_scene_present_instances` (model and custom
mesh batches, directional + spot shadows, ACES tonemap, alpha/additive blend, `RIM` and
`SOFT_PARTICLE` surfaces, stencil replace), canvas, presented-frame readback and PNG. Forces Vulkan
and loads 14 named `.spv` shaders. Uses `REFRACT` (screen-space refraction, RFC 0005) for its Optical Fluid,
when `BTBH_GPU_SCENE_HAS_REFRACTION` is defined. FSR 3 (since 2026-11-09: instance motions, lens, cuts, reactive
scale 0.3). From v0.1.1 its live unit portrait can use `window_scene_render_to_target` and its ground detail
`reactive_mode` (RFC 0006). From v0.2.0: the soft sun shadow filter (`directional_shadow_filter` SOFT, on from
Medium up), `point_light_ceiling` 1.5, and the horizon-based occlusion as an opt-in "Ultra" AO in no preset. From
v0.3.0: its Graphics tab's V-Sync row (`btbh_gpu_window_set_present_mode`, gated on
`BTBH_GPU_WINDOW_HAS_PRESENT_MODE`). From v0.4.0: its HUD text through `window_font_load_ex` (RFC 0004 A:
oversampled 3×1 up to 48 px, fractional advances, `coverage_gamma` 1.3 under 22 px) and `font_metrics`. From
v0.5.1: all kerning pairs in one call (`font_kerning_pairs`), R8 font atlases, the x-height snap (rounded) for faces
under 22 px, `NO_SUN_SHADOW` on ornaments under 4 px on screen, and `KEEP_ORDER` on its ground with the scenery
drawn before it. `opaque_sort` and the floored snap are wired but off. `build.rs` gates them on
`BTBH_GPU_SCENE_HAS_OPAQUE_SORT` and `BTBH_GPU_HAS_FONT_FLAGS`. From v0.14.2 (straight from v0.5.1): no new opt-in, and the
32 MiB upload staging default applies. Its own shadow-only mark sits in the batch's last byte (`ssao_reserved[0]`,
byte 255; it was `ssao_reserved[1]` up to 0.11.0), which the renderer keeps reserved and in the run key. Giving that
byte a meaning needs CHIMERA to move its mark first. It presents through `_with_visibility` (`gpu.rs:2607`). Not
used yet: terrain. Requested (drafts in `docs/rfc/`): layered terrain (0001), height fog and water at RTS scale
(0002), light regions (0003); its lamps should move to `lamp_*` (in v0.1.0).

**Be The Bullet Hell v2** (`v2/crates/v2-platform/src/gpu.rs`, bindgen; it took over CHIMERA's sys crate):
instanced model batches with vertex colours, one albedo/normal/ORM set per batch (kit detail maps since BTBH
`4ff0e1b3`), the built-in procedural surface variants, skinned kaiju (glTF), transparent and additive effects,
refraction strips, decals and ground scars, x-ray stencil masks, cascaded sun shadows, SSAO, SSR, glass, cloud
shadows, water normals, rain and wetness, bloom, vignette, LUT grading, FSR 1, and FSR 3.1 with motion vectors and
a reactive mask. Since 0.6.0: the world detail (RFC 0008 phase 1), a world-projected second texture set per batch
for its kit's coarse relief. Since 0.7.0: the scene prewarm and pattern 7 (RFC 0009 phase 1). Since 0.8.0:
the world-detail table chosen per vertex (RFC 0009 phase 2) on its merged distance models, full kit models and
outskirts ring. Since 0.9.0:
- `BTBH_GPU_SCENE_BATCH_NO_RC_SOURCE` on its panes, lamp heads, power lines, traffic, ambulance and boat lights, enemy
  and boss bodies, the cocoon, the kaiju's plates, hurt flash and rims, and its lit smoke;
- `SSR_SURFACE_REFLECTIVITY`, with `ssr_thickness` 0.1;
- `btbh_gpu_window_scene_prewarm_render_to_target` for its V1 cards' pictures.

Since 0.10.0:
- the cheaper draws, with `BTBH_GPU_CUSTOM_MESH_SHARED_BUFFERS` on its library meshes (RFC 0011 phase 1): the kit with
  landmarks and set pieces, the country looks, props, boats and roof gear;
- `SSR_FACING_HITS`, with `ssr_thickness` 0.6 (replacing 0.9.0's 0.1);
- the kaiju's spot shadow with `SPOT_OCCLUDER_SUN_ONLY | SPOT_OCCLUDER_ORTHOGRAPHIC`, its reach 2.5 times the map's
  depth, strength 0.8 by day and dusk and 0.53 at night;
- each emitter's `top_above` filled per class, with `source_tops` off until its lamps return as explicit emitters
  (RFC 0010).

Since 0.11.0:
- the upload staging budget (128 MiB);
- queued uploads: changed spans of its kit, prop and city chunks, and queued mesh updates;
- the height lift under its Height terrain experiment (RFC 0012), with lifted templates in its prewarm and lifted
  batches sorted first;
- the light factor for its blackouts, which replaced its dark batches;
- `KEEP_HBAO` with contact AO;
- lit smoke (RFC 0013 phase 1) behind a dev flag at an albedo gain of 0.38, waiting on its owner's choice.

Since 0.12.0:
- `KEEP_HBAO` with contact AO, including its ambient part;
- `KEEP_CASCADES_LOOK` (`flags2`) on its weapon beams' own unlit draws only, with flagged copies in its prewarm.

Since 0.13.0: nothing new used explicitly; its 128 MiB staging budget overrides the new 32 MiB default.

Since 0.14.0: the particles' transmittance map (RFC 0013 phase 2). `BTBH_GPU_SCENE_BATCH_PARTICLE_SHADOW` is set on
the lit smoke's batches, and `lighting.particle_shadow` is 2 at High and Medium and 5 (`1 | QUARTER`) on the Deck
preset and Low, with density 1 and the flagged batches in its prewarm.

Since 0.14.1: nothing new to use. The mip fix fills its kaiju's 2048x1024 maps' last levels, and its reference
captures moved with it.

Since 0.14.2: nothing new to use. Its shader set is rebuilt from the release (`shaders-142`), and its images and
reference hashes are unchanged on NVIDIA.

Since 0.15.0, for its local co-op split screen (shaders still `shaders-142`):
- the split's second view through `btbh_gpu_window_scene_render_to_target_view` as target view 1, rendered at 0.67
  with a rect of player 2's half grown 96 px past the divider, prewarmed with
  `btbh_gpu_window_scene_prewarm_render_to_target_view` at a two-player stage's load;
- the gamepad roster (`btbh_gpu_window_gamepad_*_at`, identity read once per connection), in place of opening player
  2's pad through SDL;
- `btbh_gpu_window_scene_set_shadow_update_periods` from `btbh_gpu.h` (its own declaration dropped): the sun at period
  2 while split, 1 otherwise.

Since 0.16.0 (shaders `shaders-160`, with `production_post_view.vert` in its list):
- the split in one present: `btbh_gpu_window_scene_present_views` with two views in place of the target view and its
  compose, prewarmed with `btbh_gpu_window_scene_prewarm_views` at a two-player stage's load. The old path stays
  behind `BTBH_SPLIT_VIEWS=0`. One sun map is shared while the two grounds are within 1.25 times each other; on the
  Deck preset the second view casts sun shadows too; split frames run SMAA medium;
- the cost log's pass frames (`BTBH_GPU_COST_PASSES=1`) in its perf runs, and SDL's debug info in its prebuilt
  libraries as an opt-in (`stage_prebuilt.py --sdl-debug-info`, the renderer's `BTBH_SDL_DEBUG_INFO`);
- the cheaper bone palettes, with nothing to change.

Since 0.16.1: nothing new to use. Its shader set is rebuilt from the release (`shaders-161`): the split's reflections
reach to its divider, and its frames with no split are unchanged.

Later: the frame thread (RFC 0015, accepted; phase 1a in 0.16.1), multi-draw indirect (RFC 0011 phase 2), a depth
prepass (RFC 0016), half-resolution SSR, FSR 3 over two views and bloom per view (RFC 0014 phases 2 and 3), BC texture
formats (RFC 0008 phase 2), and descriptor pools made before the first frames.

**Tyrian3D** (`src/render_gpu.c`, `src/background3d.c`, `src/extract.c`): device and window,
`window_sdl_window`, raw RGBA textures (`window_texture_create_rgba8_ex`, `texture_update_rgba8`,
mip caps), models (`window_model_load`, material info), custom meshes,
`window_scene_present_instances_with_visibility` with lighting (directional shadow, point lights,
dark scenes, unlit-with-shadow, `GLINTS` / `PIXEL_WATER` surfaces, radiance cascades additive and
receivers) and post (bloom), canvas, frame readback and PNG. From `v0.2.0`: the soft sun shadow filter
(`directional_shadow_filter`, `set_directional_shadow_size`, `unlit_enabled` 3), the horizon-based ambient
occlusion (`ssao_enabled` 2, `ssao_*`, per-batch `ssao_mode`) and `point_light_ceiling`. Its CMake reads
shaders from `shaders/gpu/`. The renderer's `tyrian3d` branch is retired; its last commits are kept under
the tag `archive/tyrian3d`.

## Contacts

There is no direct channel between the machines: open a GitHub issue on this repository and name the
game in its title (`[BTBH]`, `[BTBH v2]`, `[CHIMERA]`, `[Tyrian3D]`). The coordinator answers there and links the
release that resolves it.
