# RFC 0005: Screen-space refraction batches

- **Status:** Implemented in v0.1.0. Accepted by CHIMERA after testing on Windows/Vulkan (2026-10-31)
- **Requested by:** CHIMERA (2026-10-31: "Void Matter" and "Optical Fluid")
- **Affects:** CHIMERA (adopts it). Be The Bullet Hell and Tyrian3D: nothing, opt-in
- **Issue:** to be opened

## Need

CHIMERA has a resource that is almost invisible: an "Optical Fluid" shows only because the ground seen through it
bends. Stones and ground textures stretch, shift and double, some parts magnify or shrink, and thin chromatic
lines appear where the bend is strongest.

- **Flow:** it moves slowly (waves, small eddies, calm and unstable parts). It looks different when the camera
  angle changes, but holds still while the camera does.
- **Ribbons:** a building (the Optical Collector) pulls the fluid up as transparent ribbons of bent space with
  chromatic edges.
- **Shockwave:** breaking a "Void Matter" triangle sends a short ring of bent space through the fluid.

No batch can sample the scene behind it today (alpha and additive only tint), so none of this can be drawn.

## Proposal

One surface variant, appended, plus a compile-time marker:

```c
    BTBH_GPU_SCENE_SURFACE_REFRACT = 10,   /* after GLINTS = 8 and PIXEL_WATER = 9 */
    BTBH_GPU_SCENE_SURFACE_COUNT = 11

#define BTBH_GPU_SCENE_HAS_REFRACTION 1
```

(CHIMERA's request said 8. That number and 9 already belong to `GLINTS` and `PIXEL_WATER` from the `tyrian3d`
merge in `v0.1.0`.)

A refracting batch:

- **Pass:** draws after every other batch (opaque and transparent), depth-tested against the scene, without
  writing depth.
- **Required state:** `transparency_enabled = 1` and `BTBH_GPU_SCENE_BLEND_ALPHA`; anything else fails validation
  with `BTBH_GPU_INVALID_ARGUMENT`.
- **Shadows and GI:** casts no shadow and is no radiance-cascade source.
- **What it shows:** the scene as the transparent pass left it (a copy of the HDR target), sampled at the pixel
  moved by a flowing screen-space offset.

| Field | Meaning |
| --- | --- |
| `surface_parameters0.x` | strength: largest displacement in pixels at a 1080-pixel-high render target, scaled with the render height |
| `surface_parameters0.y` | dispersion 0..1: red and blue sampled at the offset times 1 ± 0.35·y, so colours part along the bend |
| `surface_parameters0.z` | flow scale: noise cells per world unit |
| `surface_parameters0.w` | flow speed: cells per second (times `surface_time`) |
| `surface_parameters1.x` | view coupling 0..1: 0 the pattern is fixed in the world; 1 it is sampled at the world position shifted along the view direction, so it changes as the camera moves and holds still with it |
| `surface_parameters1.y` | line strength: thin chromatic lines where the displacement changes fastest from pixel to pixel |
| `surface_parameters1.z` | normal push: the vertex normal's direction on screen times this many pixels (at 1080); a ring with outward-leaning normals makes a shockwave |
| `surface_parameters1.w` | reserved, must be 0 (the renderer uses it internally) |
| colour alpha (vertex x instance x material) | scales the whole displacement: 0 shows the scene undisplaced (fade edges, a depleted fluid, ribbon ends) |
| colour rgb | multiplies the refracted colour (1: unchanged) |

Where the displaced sample lies nearer the camera than the refracting surface (a unit standing in front of it),
the scene shows undisplaced instead: no unit is smeared into the fluid.

## Minimal example

```c
BTBHGPUSceneGeometryBatch fluid = {0};
fluid.kind = BTBH_GPU_SCENE_GEOMETRY_CUSTOM_MESH;
fluid.custom_mesh = fluid_mesh;              /* a sheet over the site; vertex alpha fades its edges */
fluid.transparency_enabled = 1u;
fluid.blend_mode = BTBH_GPU_SCENE_BLEND_ALPHA;
fluid.depth_compare = BTBH_GPU_SCENE_DEPTH_LESS_EQUAL;
fluid.surface_variant = BTBH_GPU_SCENE_SURFACE_REFRACT;
fluid.surface_time = seconds;
fluid.surface_parameters0[0] = 14.0f;        /* up to 14 px at 1080p */
fluid.surface_parameters0[1] = 0.4f;         /* dispersion */
fluid.surface_parameters0[2] = 0.6f;         /* cells per world unit */
fluid.surface_parameters0[3] = 0.15f;        /* slow */
fluid.surface_parameters1[0] = 0.5f;         /* view coupling */
fluid.surface_parameters1[1] = 0.6f;         /* chromatic lines */
/* albedo colour, instance colour: white, alpha 255 */
```

A shockwave is the same variant on a ring mesh whose normals lean outward, with `surface_parameters1.z` set and
its colour alpha animated from 1 to 0.

## Implementation (prototype)

- **Phases.** `btbh_gpu_window_scene_present_instances` gains a third geometry phase after opaque and
  transparent, only on frames with a visible refracting batch.
- **Copy.** Before that phase, `SDL_BlitGPUTexture` copies the scene target at the render size into a
  `R16G16B16A16_FLOAT` texture, made on the first such frame and remade when the render size changes.
- **Pass.** The pass then loads the same colour, reactive and depth targets as the transparent pass. Its
  reactive mask is loaded, not cleared, so it adds to the transparent pass's.
- **Shader.** `production_model.frag.hlsl`'s `main` takes `refract_fragment` for variant 10 instead of the
  material path. No new shader files and no new bindings:
  - the albedo slot carries the scene copy;
  - the normal slot carries the opaque scene's distance from the camera (the soft particles' distance pass,
    which now also runs on frames with refraction), used for the "in front of it" fallback.
- **Flow.** A value-noise height field warped by a slower noise (the eddies), with a coarse mask for calm and
  unstable stretches. Its gradient turns into a screen direction through the position's screen-space
  derivatives, so it follows the ground's orientation on screen. The normal push adds the vertex normal's
  screen direction.
- **FSR 3.** Refracted pixels write the reactive mask by how far they move (`length(offset) / 6 px`, times the
  frame's reactive scale), so the temporal upscaler keeps less history there.
- **Cost.**
  - Frames without a refracting batch: nothing; no copy, no pass, no texture.
  - Frames with one: one copy at the render size (expected ~0.05-0.1 ms at 1080p) and the batches' fill: three
    to four texture samples and about 20 noise lookups per pixel.

## Compatibility

- **Versioning:** minor. An enum value appended (`COUNT` 10 → 11), a macro; nothing changes for batches that
  don't use it.
- **Be The Bullet Hell / Tyrian3D:** nothing to change; their batches never use variant 10, so their frames take
  no extra pass. The model shader gains a uniform branch at the top of `main`.
- **CHIMERA:** set the variant and parameters above; build its shader list from the renderer as before (no new
  files).

## Platforms and testing

- **Done in the prototype:**
  - native test of the validation rules (`btbh_gpu_scene_refraction_valid`);
  - the model shaders compile to SPIR-V and MSL on Windows.
- **CHIMERA on Windows/Vulkan (2026-10-31):**
  - What it drew: a fluid sheet per field, collector ribbons and a shockwave ring per break.
  - Parameters `[28, 0.6, 0.45, 0.12] / [0.6, 0.45, 0, 0]` for the sheet, `[30, 0.7, 1.0, 0.6] / [0.3, 0.6, 0, 0]`
    for the ribbons, `[0, 0.5, 1.0, 0] / [0, 0.4, 18, 0]` for the ring.
  - Look: paving, grass and rocks bend visibly, and units standing in the fluid are not smeared.
  - Tuning: around 28 px with lines at 0.45 it reads as intended; 60 px with lines at 3.0 turns the lines
    into smears.
  - Cost: about +0.4 ms GPU at 1600x900 on the field view (RTX 3070 Ti).
  - Stability: no validation errors or crashes over several runs, including a 5-minute bot game.
- **Still to do:**
  - the Mac runs `tools/check.sh` (metallib);
  - a readback test that draws an opaque quad and a refracting quad and checks the frame succeeds and differs
    from the unrefracted one.

## Alternatives

- **Refraction inside the transparent pass** with a copy before it: transparent effects would then be missing
  from what the fluid shows, and every transparent frame would pay for the copy.
- **A dedicated shader file:** cleaner separation, but every host's shader list would change. The branch in the
  model shader keeps the lists as they are.
- **A real refraction (ray through a surface) or SSR-style marching:** far more expensive. The look asked for
  is stylised bending, which a displacement field gives.

## Open questions

Answered by CHIMERA: the default cell size reads at RTS distance; the lines are fine as they are; no frosted
variant is needed now. Left for later: a second, larger octave would help over uniform sand, where there is
little texture to bend.
