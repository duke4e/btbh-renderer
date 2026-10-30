# Handoff to the Mac: `tyrian3d` merged into `master`

From the renderer coordinator (Windows PC) to the Tyrian3D session (Mac). Please verify this merge on
macOS/Metal and report back on the GitHub issue that links this file. Nothing is merged into `master`
until you have.

## What this is

Branch `merge/tyrian3d-into-master` = GitHub `master` (`e95b392`, 2026-10-22) + `origin/tyrian3d`
(`92b3b35`, its first 12 commits), conflicts resolved, plus one compatibility fix. It becomes `v0.1.0`,
the first tagged release; from then on Tyrian3D pins a tag instead of following a branch (see
`COORDINATION.md`).

**Please stop committing to `tyrian3d` now.** Four commits landed there after this merge was prepared
(`96486ab` smooth pixel-water mask, `f7c3773` PCSS sun shadows, `beaf727` HBAO SSAO, `89266d2` point-light
ceiling). They overlap master's own SSAO and shadow work, so they are **not** in `v0.1.0`: the coordinator
ports them onto `master` for `v0.2.0` as opt-in features (HBAO as an SSAO mode, PCSS as a shadow-quality
option, the point-light ceiling, the water mask), so Be The Bullet Hell's look stays as it is. Until
`v0.2.0`, Tyrian3D keeps building on `tyrian3d` @ `89266d2`; verify `v0.1.0` anyway (below), since `v0.2.0`
builds on it. After `v0.2.0` the `tyrian3d` branch is retired and new requests go through issues.
Note for the port: `89266d2` sends the ceiling in `point_light_count.y`, which this merge already uses
for `unlit_darkness`; `v0.2.0` will give the ceiling its own slot (internal only, the C field name stays).

Everything the first 12 `tyrian3d` commits added is kept: raw RGBA textures and streaming
(`btbh_gpu_window_texture_create_rgba8_ex`, `btbh_gpu_texture_update_rgba8`, mip caps),
`btbh_gpu_window_sdl_window`, `btbh_gpu_window_scene_present_instances_with_visibility`, unlit surfaces
that take the directional shadow (`unlit_enabled = 2`), `GLINTS` and `PIXEL_WATER` surfaces, 32 point
lights (`point_light_count`, `point_lights`), relief for unlit surfaces, radiance cascades `additive`
and `rc_receiver`, dark scenes (`unlit_darkness`, `unlit_glow_threshold`), linear font atlases.

## What changes for Tyrian3D

1. **Shader folder.** `shaders/gpu_spike/` is now `shaders/gpu/` (master removed the old spike tree).
   In Tyrian3D's `CMakeLists.txt` (around lines 51 and 56) replace `shaders/gpu_spike` with
   `shaders/gpu`.
2. **Bloom.** Master and `tyrian3d` had each grown a bloom. Only master's stays (HDR composite before
   grading, one implementation for all games). In `BTBHGPUScenePost`:
   - removed: `bloom_knee`, `bloom_radius`, and the shaders `production_bloom_down/up/composite.frag`;
   - use: `bloom_enabled = 1`, `bloom_intensity`, `bloom_threshold` (in exposed units: scene colour
     times `exposure`; with `exposure = 1` as Tyrian3D sets it, the same scale as before),
     `bloom_soft_knee` (0..1, a fraction of the threshold), `bloom_scatter` (0..1, how far it spreads).
   - A starting point for `render_gpu.c` (`bloom_parameters` = intensity 0.5, threshold 1.0, knee 0.1,
     radius 1.2): `bloom_soft_knee = 0.1f; bloom_scatter = 0.7f;` then tune by eye. `TYR3D_BLOOM`
     parsing needs the same rename.
   - The HDR composite loads `production_hdr_composite.frag`, `production_bloom_downsample.frag` and
     `production_bloom_upsample.frag` next to the post fragment shader: make sure Tyrian3D's shader list
     (read from the renderer's `CMakeLists.txt`) compiles and stages them as metallib.
3. **Surface variant numbers.** Master already used 6 and 7 (`SOFT_PARTICLE`, `RIM`), so
   `BTBH_GPU_SCENE_SURFACE_GLINTS` is now 8 and `BTBH_GPU_SCENE_SURFACE_PIXEL_WATER` 9 (`COUNT` 10).
   Code that uses the names only needs a rebuild; check that nothing stores the numbers
   (`grep -n "surface_variant *= *[0-9]" src/`).
4. **Shadow maps.** `tyrian3d` raised every directional shadow map to 2048. Master had split them: the
   primary (sun) is 2048, the secondary directional 512. If Tyrian3D uses the secondary directional
   light's shadow, it is softer now; say so if it matters.
5. **New fields, all off when zero.** Master added fog, fog edge, vignette, SSR, AO controls, world
   curvature, FSR 1/3 upscaling, street lamps (`lamp_*`), clouds, wetness and rain. A zeroed struct keeps
   them off. (One exception was found and fixed in this merge: master required `ao_radius > 0` even with
   SSAO off; zero now means the old defaults.)
6. **Uniform layout.** The dark-scene controls moved from `procedural_controls.zw` to
   `point_light_count.yz` inside the scene shader (master needs `procedural_controls.zw` for FSR 3).
   Internal only, but it is exactly the kind of change Metal can show differently: please look at a dark
   scene.

## How to test on macOS / Metal

```sh
cd external/btbh-renderer
git fetch origin
git checkout merge/tyrian3d-into-master     # after the coordinator has pushed it
git submodule update --init --recursive

# 1. The renderer's release check: C core, shaders (SPIR-V, MSL, metallib), native tests
#    including the device and window smoke tests, the headers on their own
tools/check.sh

# 2. Tyrian3D against it, with the changes above applied in Tyrian3D (not committed until it passes)
```

Then play and capture (presented-frame readback PNGs, before on the old submodule and after on the
merge) at least:

- a level with water: `GLINTS` and `PIXEL_WATER`;
- a dark level: `unlit_darkness`, glow threshold, the spot light on unlit surfaces;
- explosions: point lights, relief on unlit surfaces;
- radiance cascades in additive mode with receivers;
- bloom on shots and items after re-tuning;
- the HUD and text (linear font atlas).

## What to report (on the GitHub issue)

- `tools/check.sh` output (the ctest summary, and output for anything that failed).
- Did Tyrian3D build with only the changes listed above? Anything else you had to change.
- Before/after PNGs for the scenes above, and anything that looks different that you did not expect.
- Frame time before/after on the same scene if you measure it (the scene shader grew: lamps, clouds,
  surface detail are compiled in even when off).
- Your verdict: OK to merge into `master` and tag `v0.1.0`, or what must change first. Please do not
  commit to the renderer; describe the fix (a diff in the issue is welcome) and the coordinator applies it.

## Verified on Windows by the coordinator (2026-10-30)

- Renderer CMake build (MSVC 2022, Ninja, Release, vendored SDL_shadercross/DXC): all 66 production
  shaders compile to SPIR-V and MSL; `ctest`: 14/14 passed, including `btbh_gpu_device_smoke` and
  `btbh_gpu_window_smoke` on Vulkan. (The tests now keep `assert()` in Release; before, NDEBUG removed
  every check.)
- `btbh_gpu.h` and `btbh_gpu_resources.h` compile on their own as C11 and C++17 with `/W4 /WX`.
- CHIMERA (`cargo build --release -p chimera-game`, bindgen over the header) builds against this branch.
- Not verified here: Metal, metallib compilation, and Tyrian3D itself. That is what this handoff asks for.
- Note: the commits on this branch sit on GitHub `master` as of `e95b392`; the coordinator could not fetch
  from GitHub on 2026-10-30 (expired credentials), so if `master` moved since, the coordinator rebases
  before tagging.
