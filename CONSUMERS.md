# Consumers

The games that build on btbh-renderer, how each one gets it and what it uses. The coordinator keeps
this current; a game's session asks (an issue) when something here changes. Rules: [`COORDINATION.md`](COORDINATION.md).

| | Be The Bullet Hell | CHIMERA | Tyrian3D |
| --- | --- | --- | --- |
| Language | Swift (SwiftPM) | Rust | C (OpenTyrian fork) |
| Platforms | Windows, macOS 12+, Linux (Steam Deck) | Windows (Vulkan) | macOS (Metal), Windows (Vulkan) |
| Repository | `Be-The-Bullet-Hell`, branch `main` | `RTS`, branch `main` | `Tyrian3D`, branch `tyrian3d/m1-tools` |
| Machine / session | Windows PC, BTBH session | Windows PC, CHIMERA session in `RTS/` | Mac and Windows PC, Tyrian3D sessions |
| Consumes through | git submodule `City Generator (r3d)/Vendor/BTBHRenderer`, SwiftPM `.package(path:)`; products `BTBHRenderer`, `BTBHRendererGPU` | `crates/btbh-gpu-sys/build.rs` compiles `src/gpu/*.c` from `BTBH_RENDERER_DIR` (default `../btbh-renderer`), bindgen over `btbh_gpu.h` | git submodule `external/btbh-renderer`; its CMake globs `src/gpu/*.c` into a static `btbh_gpu` |
| Native deps | renderer's CMake (`build-gpu-game.ps1/.sh`, `build-gpu-game-linux.sh`): SDL3 static, Assimp, shadercross | `BTBH_RENDERER_PREBUILT` (SDL3-static, Assimp DLL; `tools/build_prebuilt.ps1`) | system SDL3, Assimp, zlib |
| Shaders | renderer's CMake → SPIR-V / metallib staged into `GPUShaders/` | `tools/build_shaders.py` (shadercross → SPIR-V in `assets-cache/shaders`), list read from the renderer's `CMakeLists.txt` | its CMake: shadercross → SPIR-V → MSL → `xcrun metal` metallib, list read from the renderer's `CMakeLists.txt` |
| Pinned version | submodule at tag `v0.2.1` (BTBH `d2689071`, 2026-11-11) | `renderer.pin` → `../btbh-renderer-v0.3.0` (tag `v0.3.0`; CHIMERA `099d45ec`, 2026-11-11) | submodule at tag `v0.2.1` (Tyrian3D `60c1b596`, 2026-11-11) |

## Features used

**Be The Bullet Hell** (only through the Swift layer, `import BTBHRenderer` / `BTBHRendererGPU`; no direct
`btbh_gpu_*` calls): window and fullscreen, gamepad (incl. rumble), canvas/2D and fonts, model loading
with materials and patches, surface details, persistent instanced ranges with LOD, custom meshes,
skinning and animation trees, lights (directional, spot, omni, street lamps), procedural sky, fog and
fog edge, clouds, wetness/rain, shadows, radiance cascades with emitters, post chain (SSAO/SSR, bloom,
vignette, tonemap, AA), upscaling (`fsr1`, `fsr3`), frame readback for render baselines, file dialog.
The most demanding consumer: it defines most of the scene API.

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
`BTBH_GPU_WINDOW_HAS_PRESENT_MODE`). Not used yet: terrain, `_with_visibility`. Requested (drafts in
`docs/rfc/`): layered terrain (0001), height fog and water at RTS scale (0002), light regions (0003); its
lamps should move to `lamp_*` (in v0.1.0).

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
game in its title (`[BTBH]`, `[CHIMERA]`, `[Tyrian3D]`). The coordinator answers there and links the
release that resolves it.
