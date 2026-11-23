# RFC 0013: Lit particles

- **Status:** Phase 1 accepted on 2026-11-23 for 0.11.0 (the owner left the decision to the coordinator); phase 2 after
  v2's phase 1 captures. See "Decisions"
- **Requested by:** Be The Bullet Hell v2 (its session, 2026-11-23, after it shipped fake-lit smoke)
- **Affects:** Be The Bullet Hell v2 (adopts it). Be The Bullet Hell (v1, Swift), CHIMERA and Tyrian3D: nothing, it is
  opt-in. They rebuild the scene shaders with the C core, as every release. `SOFT_PARTICLE`, which CHIMERA and v2 draw,
  doesn't change. `production_model.vert` and the four files built from it gain the UV rect's test: their SPIR-V
  changes, their pictures don't
- **Issue:** none. v2 answered all 13 questions through its session on 2026-11-23 (see "Decisions")
- **Target:** 0.11.0 for phase 1; phase 2 after phase 1's captures
- **Depends on:** nothing. Drafted against 0.10.0's work in progress (`c5390f0b`) and re-checked against
  `release/0.11.0` (`fc2784ea`: 0.9.0, RFC 0011 phase 1, 0.10.0's fixes, RFC 0010 and RFC 0012); every line reference
  below is `fc2784ea`'s. On 64-bit the batch is 288 bytes (RFC 0012's `height_lift` at 280) with `flags` at 253,
  `BTBHGPUScenePost` is 432 bytes with `flags` at 427, and `BTBHGPUSceneLighting` is 3,340 bytes with
  `directional_shadow_reserved[0]` at 3,337 and 0.10.0's `spot_occluder_flags` at 3,338
  (`Tests/btbh_gpu_scene_order_test.c:25`, `:33-34`, `:122`, `:125`, `:149-152`). No struct grows. Phase 1 takes an enum
  value and a free bit of the batch's flags; phase 2 a second bit and the lighting's reserved byte

## Need

v2 has shipped smoke that only looks lit. From its request:

- It bakes lit puffs at five light tilts and turns each sprite toward the sun or a nearby fire, picking the tilt.
- It draws them with the renderer's soft particles (`SOFT_PARTICLE`) and unlit mode 3, so the sun's shadow darkens them.
- It needs 9–11 draws for its smoke (13 at most; the request said 6–20, answer 4 corrected it).
- Lamps, point lights and the radiance cascades don't light the puffs. The puffs shadow neither each other nor the
  street.

It asks for five things to replace this:

1. **A lit particle surface,** e.g. `BTBH_GPU_SCENE_SURFACE_LIT_PARTICLE` (11): a transparent billboard lit per pixel.
   - Its light: the sun with its shadow map (the plain 4-tap filter is enough), sky ambient, fill, point and spot
     lights, lamps, the radiance cascades when on, and fog.
   - Its inputs: a `normal_texture` with a tangent-space normal in RG and thickness in B, on the albedo's UV. The
     tangent frame comes from the instance matrix's x and y.
   - Diffuse only: wrapped Lambert (`surface_parameters0.w`) plus backlight transmission
     `exp(-thickness·p1.z)·HG(p1.y)·p1.x`.
2. **A soft fade with a normal map.** The scene's distance gets its own binding (today soft particles take the normal
   slot), plus a near-camera fade (`p0.y`, `p0.z`).
3. **A UV rect per instance.** A batch flag that reads custom floats 0–3 as a UV scale and offset, so one draw covers
   every atlas cell.
4. **Self-shadowing.** Batches flagged `PARTICLE_SHADOW` write a half-resolution transmittance map in sun space. Lit
   particles read it, and optionally the ground too, so a smoke column's lee side darkens and its shadow falls on the
   street.
5. **Sorting.** Particle batches depth-sorted against each other, or the instances within one batch.

## What the code does today

### Surfaces and the two particle paths

- **The surface variants** are an enum, `NONE` (0) to `REFRACT` (10), with `COUNT` 11 (`include/btbh_gpu/btbh_gpu.h:548-581`).
  `SOFT_PARTICLE` (6) has no comment in the header. The present refuses a variant at or past `COUNT`
  (`src/gpu/btbh_gpu_scene.c:4817-4820`) and a batch flag it doesn't know (`:4844-4849`).
- **Soft particles** (`shaders/gpu/production_model.frag.hlsl:498-513`):
  - The normal slot carries the opaque scene's distance from the camera. The billboard's alpha fades over
    `surface_parameters0.x` render units where it meets the scene.
  - The core swaps the texture into slot 2 (`btbh_gpu_scene.c:9739-9746`). Outside the transparent pass, or without the
    distance, it draws the batch as `NONE` (`:9575-9580`).
  - The distance is an R32F target at the render size (`:2473-2516`), written by `production_scene_distance.frag`
    between the opaque and transparent passes (`:8953-8987`). Its pipeline is made on first use (`:2435-2471`), and the
    pass runs only on frames that draw a soft particle or a refracting batch (`:2566-2581`, `:7706-7720`). The
    background's distance is 10⁶ (`production_scene_distance.frag.hlsl:24-27`).
  - A soft particle still runs the full shader: it samples the albedo, emission, "normal" and ORM slots
    (`production_model.frag.hlsl:2315-2338`), then loads the distance again.
- **Unlit modes 2 and 3** (`btbh_gpu.h:1036-1038`, `production_model.frag.hlsl:2406-2425`):
  - The albedo is multiplied by `lerp(0.45, 1, visibility)` of the sun's shadow.
  - Mode 3 asks for the "plain" lookup: four comparison taps with the receiver plane's gradient (`:1111-1125`). That
    lookup belongs to the soft filter only. Under the tent filter, the default (`btbh_gpu.h:1178-1187`), mode 3 takes
    the tent's 16 gathers (`production_model.frag.hlsl:1244-1321`, `:973-1004`).
  - Point lights brighten unlit surfaces too (`:2488-2491`).

### The transparent pass

- **Which batches.** `transparency_enabled`, or a blend other than `ALPHA`, puts a batch in the transparent pass
  (`btbh_gpu_scene.c:1213-1219`). Its pipelines write no depth (`:1527-1531`). It writes the colour, plus the reactive
  mask under FSR 3 (`:1561-1563`, `:1591-1603`, `:9012-9034`).
- **What runs before it, in order:**
  - the opaque pass;
  - the radiance cascades, whose compose adds their light to the opaque pixels (`:9948-10063`;
    `btbh_gpu_radiance_cascades.c:3604-3852`);
  - the HBAO (`btbh_gpu_scene.c:8752-8847`);
  - the SSAO and SSR resolve (`:8848-8952`);
  - the distance pass (`:8953-8987`).
- **What it draws over.** The transparent pass draws on the result of all that (`radiance_cascades_post_input`,
  `:9014`). Refracting batches follow in a pass of their own, over a copy that includes the transparent pass
  (`:8988-9011`).
- **The order.** The batches draw in the order given. The opaque sort walks its own order, but only in the opaque pass
  (`:9438-9449`; `btbh_gpu.h:1376`). Its key is the view depth of the batch's mean instance centre
  (`btbh_gpu_scene.c:5228-5282`), sorted in runs between pinned batches (`:5346-5408`).
- **Shadows.** Transparent batches cast none.
- **The cascades' light.** No transparent batch takes it. The compose adds it to the opaque pixels before the
  transparent pass draws. In the transparent pass, even the cascades' fragment shader (`production_model_rc*.frag`)
  writes only its lit colour.

### Instances and texture coordinates

- **The instance** is 24 floats (`btbh_gpu_scene.c:22-24`): `float4x4 model`, `float4 color`, `float4 custom`
  (`production_model.vert.hlsl:27-32`). The packer writes the matrix by columns: the x axis times its scale in floats
  0–2, the y axis in 4–6, the translation in 12–14. The colour goes in 16–19 and the custom floats in 20–23
  (`src/gpu/btbh_gpu_instance_buffer.c`).
- **Who reads the custom floats:**
  - `DISSOLVE` reads x, y and z; `SHATTER` reads y, z and w; `GLITTER` reads x (`btbh_gpu.h:899-900`;
    `production_surface.hlsli`).
  - A world detail's seed reads the one its `seed_component` names (`production_world_detail.hlsli`).
  - Nothing else does. The vertex shader passes them on (`production_model.vert.hlsl:316`).
- **The texture coordinate** is the mesh's, times the batch's `uv_scale` plus its `uv_offset`
  (`production_model.vert.hlsl:311-312`). Both are in the same-material run key (`btbh_gpu_scene.c:5091-5094`). So
  batches that differ only in their atlas cell don't run together, which matches v2's 9–11 draws.
- **The vertex uniforms** use only `surface_controls.x`, the surface time (`btbh_gpu_scene.c:9523`, and `:6993` in the
  cascades' source pass). Its y, z and w are free, and so are the block's three `temporal_reserved` words
  (`production_temporal.hlsli:13-17`; `btbh_gpu_scene.c:95`).
- **A billboard's frame.** The instance's linear part takes the vertex's offset, normal and tangent, which are then
  laid out along the camera's right, up and `cross(right, up)` (`production_model.vert.hlsl:150-217`). The outputs are
  normalized world normal, tangent and bitangent, the bitangent being `cross(normal, tangent)` times the tangent's w.
- **What the shadow vertex shader passes on:** no texture coordinate (`production_shadow.vert.hlsl:31`). Shadow passes
  never sample a texture.

### What a lit pixel's light is made of

| Light | Function (`production_model.frag.hlsl`) | Per pixel |
| --- | --- | --- |
| Sky ambient | SH irradiance at the normal with IBL, else the flat ambient (`:2602-2647`, `:1574-1597`) | 9 SH terms |
| Sun | `evaluate_directional_light` (`:666-740`): Disney diffuse and GGX, times 1/π; the shadow through `primary_directional_visibility` (`:1244-1321`): the tent's 16 gathers, or the soft filter's search and 4–32 taps (`:1069-1238`); the clouds (`:849-867`) | 16+ fetches |
| Fill (secondary) | the same BRDF; its shadow a 3×3 tent of gathers at 512 (`:1323-1368`) | 9 gathers |
| Spot | `evaluate_spot_light` (`:784-843`); its shadow a 3×3 tent at 1024 (`:1452-1507`), or 9 cube samples for the omni light (`:1384-1442`) | 9 fetches |
| Street lamps | `evaluate_lamps` (`:872-912`): up to 40, each through the full BRDF, 1/π included | ALU |
| Point lights | `evaluate_point_lights` (`:744-765`): up to 32, diffuse, facing `n·l × 0.7 + 0.3`, no 1/π; eased under a ceiling (`:771-782`) | ALU |

The frame's lighting is one fragment uniform block (`MaterialPBR`, `:131-193`). Since RFC 0011 the core fills it once a
pass and each batch writes only its material fields over it.

### The radiance cascades

- **Sources.** A batch is a source when it is unlit and no receiver, or when it has an emission
  (`btbh_gpu_scene.c:6930-6937`). The source pass's fragment shader writes its emission, plus its albedo when it is
  unlit, times alpha (`production_rc_material_source.frag.hlsl:42`). The source pass's vertex shader is
  `production_model.vert` with `BTBH_RC_SOURCE_PROJECTION` (`production_model.vert.hlsl:252`).
- **The light at a place** is a four-quadrant atlas: the light arriving at each field texel from four directions,
  each `(±1, 0.65, 0)` or `(0, 0.65, ±1)`.
  - The reproject pass writes it to `hrc_quadrant_textures[history_write_index]` (`btbh_gpu_radiance_cascades.c:3631`).
  - The resolve and compose read it (`:3708`), then the compose makes that index the history's read index, with the
    field's origin and cell size (`:3845-3852`; `src/gpu/btbh_gpu_radiance_cascades_runtime.h:84-87`).
  - So during the transparent pass, `hrc_quadrant_textures[hrc_history_read_index]` holds this frame's light.
- **The resolve's unlit receivers** read it at the pixel's xz (`production_hrc_resolve.comp.hlsl:72-84`). So do all
  surfaces under additive cascades. Their bounce light is `albedo · Σ quadrant · saturate(n·l) · 2 · indirect_strength`
  over the four directions (`:298-312`).

### Fog

- **The HDR composite fogs every pixel at the depth of the opaque surface behind it**
  (`production_hdr_composite.frag.hlsl:40-77`, `:81`). The fog's opacity is `1 - exp(-((view depth - start) ·
  density)²)`, capped, plus the edge fog at the world's rim.
- **Transparent pixels write no depth,** so a puff over far ground is fogged as the far ground is.
- **Over the background** the composite adds no fog (`:45-48`). The scene pass clears the background to the fog's colour
  instead, so that transparent pixels over it keep their colour (`btbh_gpu_scene.c:8594-8610`). A puff over the sky is
  not fogged at all.
- **Bloom** reads the image before the fog (`:10081-10153`).

### The scene fragment shader's slots

| Slot (full variant) | Texture |
| --- | --- |
| t0, t1, t2, t3 | albedo, emission, normal (a soft particle's distance, a refracting batch's distance), ORM |
| t4 | the BRDF table |
| t5, t6, t7, t8 | the sun's map, the fill's, the spot's, the sun's again through the comparison sampler |
| t9 | the omni light's cube |
| t10 | an SDF terrain's dig mask (RFC 0007) |
| t11, t12, t13 | the world detail's albedo, normal, ORM (RFC 0008) |
| t14 | the world-detail table's array (RFC 0009) |

- **Fifteen of SDL's 16 sampler slots** are taken (`production_model.frag.hlsl:88-100`), and the core makes the pipeline
  with 15, 14 or 10 samplers (`btbh_gpu_scene.c:3364-3371`).
- **The comparison binding (t8)** is bound for every batch (`:9773-9777`).
- **The fast unlit shader** (`production_model_unlit.frag`) is a separate file, loaded beside the host's fragment
  shader. It has its own sampler and uniform counts and its own index in the pipeline arrays (`:32`, `:259-362`,
  `:2983-3062`, `:3287-3313`).

## Proposal

**Phase 1** brings items 1–3:
- a new surface, `LIT_PARTICLE`, drawn by fragment shader files of its own, with the scene's distance in a slot of its
  own and the soft and near fades;
- a batch flag for the UV rect, read in every static vertex shader.

Item 5 is v2's: it sorts its puffs far to near on the CPU every frame, and the renderer draws a batch's instances in
their order (decision 8). The draft's batch sort (1d, `BTBH_GPU_SCENE_POST_SORT_TRANSPARENT` and its macro) is dropped.

**Phase 2** brings item 4: a batch flag and a lighting byte for a transmittance map in sun space.

### Where this differs from the request

- **Its own shader, not a branch of `production_model.frag`.** The full scene shader has one sampler slot left
  ("The scene fragment shader's slots"). The particle needs textures that shader doesn't bind: the distance in a slot
  of its own beside the normal, the cascades' atlas and, in phase 2, the transmittance map. A branch would also grow
  the shader every batch runs. Its own files keep both at today's cost and leave the last slot for phase 2's ground.
- **The distance's own binding comes with the new shader.** `SOFT_PARTICLE` keeps its slot swap, and it doesn't get
  the near fade. Its `p0.y` and `p0.z` were never read, and CHIMERA (`CONSUMERS.md:37`) and v2 draw it, so giving them
  a meaning there could change a look nobody asked to change.
- **Each light keeps the scale lit surfaces give it** (decision 2). The sun, fill, spot and lamps divide by π, as
  `evaluate_directional_light` does. The point lights don't (`evaluate_point_lights`). A puff then sits in the same
  light as the ground beside it. The phase is Henyey-Greenstein times 4π, which is 1 at every angle for g = 0, and the
  transmission is tinted by the albedo.
- **The transmission for the sun and the point lights only** (decision 3). Fire glowing through smoke is v2's night
  look; lamps on poles below the smoke light its undersides, so the fill, the spot and the lamps take the wrap alone.
- **The sun's shadow is always the four plain taps,** whatever `directional_shadow_filter` says, and with a constant
  bias instead of the receiver plane. A billboard's plane isn't the smoke's surface, and seen edge-on from the sun its
  gradient would push the bias anywhere. The fill and the spot take one gather each, the omni light one cube sample.
- **Lights out of reach exit early** (decision 4): a lamp or point light beyond its reach costs its distance test. No
  per-vertex fallback.
- **Straight alpha only** (decision 5): `BTBH_GPU_SCENE_BLEND_ALPHA`. `PREMULTIPLIED_ALPHA` is refused, and can be
  added later.
- **Fog.** The particle fogs itself at its own depth. It also writes the colour that comes out right after the
  composite fogs the pixel again at the depth behind it (below). Where that second fog is total, nothing in the
  transparent pass can show; v2's fog never is (decision 6), so the make-up is exact for it.
- **Never a source of the cascades' light** (decision 7). Lit particles stay out of the cascades' source field, with an
  emission or without, as if they had `NO_RC_SOURCE`: the cleaner of the two options, since the fades need the
  camera's distance that the source pass doesn't have. v2's embers and flames stay additive unlit sprites.
- **The transmittance map holds a slab per texel in one RGBA16F texture,** so that a receiver reads it with one
  sampler slot and one fetch. Puffs stacked along a sun ray are treated as one column (decision 11).

### API (phase 1, as built)

```c
typedef enum BTBHGPUSceneSurfaceVariant {
    /* ... NONE (0) to REFRACT (10) as before; SOFT_PARTICLE (6) gains its comment ... */
    /* A see-through surface lit per pixel, for smoke, dust and steam (RFC 0013, since 0.11.0). Drawn in the
     * transparent pass with production_particle_lit.frag (its _2d_shadows or _unshadowed sibling by the frame's
     * shadows), found beside the host's fragment shader: a present without it fails, naming the file. Diffuse only.
     * Its light, each at the scale a lit surface takes it: the sky's ambient (the SH irradiance at its normal with
     * IBL, else the flat ambient); the sun, its shadow through four plain comparison taps whatever
     * directional_shadow_filter says, and the clouds; the fill and its shadow; the spot (or omni) light and its
     * shadow; the street lamps and the point lights, each skipped beyond its reach; with the radiance cascades on
     * (additive or not), their light at its world x and z from four directions; its emission, as a lit batch's.
     * A light's share is albedo * (wrap + transmission) * light, where wrap = saturate((n.l + w) / (1 + w)) and
     * the transmission (light from behind, through it) = x * exp(-thickness * z) * 4 pi * HG(y, the angle the
     * light turns to reach the eye), HG being Henyey-Greenstein's phase; only the sun and the point lights give a
     * transmission, the fill, the spot, the lamps and the cascades the wrap alone. Then fog at its own depth:
     * written so that the HDR composite's fog at the opaque depth behind it leaves the puff fogged at its own depth
     * over the background fogged at its own (exact while the fog's opacity there stays below 1).
     * Its alpha: the albedo's times the vertex's, the instance's and the material's, times the fades.
     * Its normal: normal_texture, linear, on the albedo's texture coordinate: rg a tangent-space normal (OpenGL +Y,
     * z rebuilt) whose tilt normal_scale scales (0 flat), b the thickness 0..1. The frame is the vertex's: a
     * billboard's lies in the camera's plane, so a quad in local x and y with normal +z and tangent +x (w +1) takes
     * the instance's x and y axes. It is turned toward the side seen. Without normal_texture it is flat, 1 thick.
     * surface_parameters0: x the soft fade (render units over which it fades where it meets the opaque scene behind
     *   it, as SOFT_PARTICLE's; 0 none); y, z the near fade (gone nearer the camera than y render units, whole from
     *   y + z on; z 0 none); w the wrap (0 Lambert, 1 lit all the way round to the back).
     * surface_parameters1: x the transmission (0 none); y the asymmetry, clamped to -0.99..0.99 (above 0 the light
     *   goes on forward, so a puff glows looking toward the light); z the optical depth at thickness 1 (taken as 0
     *   when below); w reserved, 0.
     * Needs transparency_enabled 1 with BTBH_GPU_SCENE_BLEND_ALPHA (straight alpha), unlit_enabled 0, a static
     * geometry (animation_library and animation_pose NULL), dug_by NULL, no world_detail with a texture, no
     * world_detail_table with map_layers, no height_lift with a heightfield, detail_parameters 0 and
     * surface_parameters1[3] 0: otherwise the present fails with BTBH_GPU_INVALID_ARGUMENT, naming the batch. It
     * casts no shadow, writes no motion and is never a source of the radiance cascades' light, with an emission or
     * without (as if it had BTBH_GPU_SCENE_BATCH_NO_RC_SOURCE). */
    BTBH_GPU_SCENE_SURFACE_LIT_PARTICLE = 11,
    BTBH_GPU_SCENE_SURFACE_COUNT = 12
} BTBHGPUSceneSurfaceVariant;

/* Defined when BTBH_GPU_SCENE_SURFACE_LIT_PARTICLE exists (RFC 0013, since 0.11.0). */
#define BTBH_GPU_SCENE_HAS_LIT_PARTICLE 1

/* BTBHGPUSceneGeometryBatch.flags, after BTBH_GPU_SCENE_BATCH_NO_RC_SOURCE: */
    /* The instance's four custom floats are a rectangle of its textures (RFC 0013, since 0.11.0, asked for by Be The
     * Bullet Hell v2): the texture coordinate, after uv_scale and uv_offset, is multiplied by custom x and y and
     * offset by z and w, so one batch draws every cell of an atlas, a cell per instance. Every static vertex shader
     * takes it (production_model.vert and the table's and the lift's), so it works in every pass that draws the
     * batch: opaque or transparent, lit, unlit (the fast unlit shader too), soft and lit particles, lifted, and the
     * radiance cascades' source pass. Shadows sample no texture and don't change. Static batches only, and not
     * with the surface variants that read the custom floats (DISSOLVE, SHATTER, GLITTER) nor a world detail or table
     * seeded by one (seed_component not 0): the present fails with BTBH_GPU_INVALID_ARGUMENT, naming the batch. It
     * needs this release's static vertex shaders: with SPIR-V ones that don't read it, the present fails with the
     * shader's path. (Bit 4 is kept for RFC 0013's phase 2.) */
    BTBH_GPU_SCENE_BATCH_INSTANCE_UV_RECT = 1u << 3

/* Defined when BTBH_GPU_SCENE_BATCH_INSTANCE_UV_RECT exists (RFC 0013, since 0.11.0). */
#define BTBH_GPU_SCENE_HAS_INSTANCE_UV_RECT 1
```

| Where | What | Offset (64-bit) | Was |
| --- | --- | --- | --- |
| `BTBHGPUSceneSurfaceVariant` | `LIT_PARTICLE` 11, `COUNT` 12 | – | `COUNT` 11 |
| `BTBHGPUSceneGeometryBatch.flags` (288 bytes) | bit 3; bit 4 in phase 2 | 253 | free bits (0.10.0 took bit 5) |
| `BTBHGPUSceneLighting` (3,340 bytes), phase 2 | `particle_shadow` | 3,337 | `directional_shadow_reserved[0]` (0.10.0's `spot_occluder_flags` is at 3,338) |

**Zero values.** A batch that doesn't use variant 11 and leaves bits 3 and 4 clear draws as before, to the byte
("Implementation notes"). So does (phase 2) a lighting whose `particle_shadow` is 0.

**Refusals** (`BTBH_GPU_INVALID_ARGUMENT`, "GPU scene batch N: surface_variant LIT_PARTICLE: …" or "… flags
INSTANCE_UV_RECT: …", beside the other batch checks):
- `LIT_PARTICLE` with `transparency_enabled` 0, or a blend other than `ALPHA`;
- `LIT_PARTICLE` with `unlit_enabled` set, an animated geometry, `dug_by`, a world detail with a texture, a table with
  `map_layers`, a height lift with a heightfield, or non-zero `detail_parameters`;
- `LIT_PARTICLE` with `surface_parameters1[3]` not 0 (phase 2's density);
- `INSTANCE_UV_RECT` on an animated geometry, on `DISSOLVE`, `SHATTER` or `GLITTER`, or with a world detail or table
  whose `seed_component` isn't 0.

### The lit particle shader

`production_particle_lit.frag.hlsl` comes in three variants, as `production_model.frag` does: the full one (the sun's,
the fill's and the spot's maps and the omni cube), `_2d_shadows` (no cube) and `_unshadowed` (no maps). The vertex
stage is the host's static vertex shader (`production_model.vert`), with the UV rect below.

| Slot (full variant) | Texture | Sampler |
| --- | --- | --- |
| t0 | albedo | the texture's |
| t1 | normal (rg) and thickness (b) | the texture's (the normal fallback, flat and 1 thick, without one) |
| t2 | the scene's distance (R32F), read at the pixel | nearest (a texture only loaded would be reflected as a storage texture) |
| t3 | emission | the texture's |
| t4 | the cascades' quadrant atlas (a fallback never sampled when they didn't light this frame) | the cascades' (linear, clamped) |
| t5 | the sun's map | the comparison sampler |
| t6 | the fill's map | the shadow sampler |
| t7 | the spot's map | the shadow sampler |
| t8 | the omni light's cube | the shadow sampler |

That is 9 samplers of 16 (8 in `_2d_shadows`, 5 unshadowed: each variant's are the next one's and some more). Phase 2's
map takes t9 in the full variant.

**Uniforms.**
- **b0:** `MaterialPBR`, declared exactly as in `production_model.frag`. The core pushes the same
  `BTBHGPUSceneFragmentUniforms`, filled once a pass; a lit particle without a `normal_texture` gets `normal_scale` 0
  (the fallback's rg is 127/255).
- **b1:** the 176-byte `ParticleFrame` (`BTBHGPUSceneParticleFrameUniforms`, `src/gpu/btbh_gpu_particle.h`), the same
  for every particle batch of a pass, pushed by the first and again after another batch's block took the slot:
  - the frame's inverse view-projection and render size;
  - whether the scene's distance is bound, and whether the HDR composite fogs this frame;
  - the composite's four fog vectors (`fog_color_density`, `fog_shape`, `fog_edge_bounds`, `fog_edge`);
  - the cascades' field origin, cell, size and source tops, and `2 · indirect_intensity` (0 when they didn't light this
    frame).

  Phase 2 adds whether to read the map and its texel size. The map reuses the sun's matrix in b0.

**Where the fragment lies.** From its pixel and its depth, through the frame's inverse view-projection: the point the
depth buffer would hold (the world bent by its curvature, as drawn), its view depth as the composite measures it
(`abs(1 / w)`), and its distance from the camera as the distance pass measures the scene's. The fades and the fog use
these; the light uses the flat world position.

**Alpha and fades.** The alpha is the albedo's, times the vertex's, the instance's and the material's.

```hlsl
if (surface_parameters0.x > 0.0f && has_distance)   // the soft fade, as SOFT_PARTICLE's
    alpha *= saturate((scene_distance - camera_distance) / surface_parameters0.x);
if (surface_parameters0.z > 0.0f)                    // the near fade
    alpha *= saturate((camera_distance - surface_parameters0.y) / surface_parameters0.z);
if (alpha <= 0.0f || alpha < alpha_cutoff) discard;  // a fully faded fragment lights nothing
```

**One light's share.** `l` points toward the light, `v` toward the camera.

```hlsl
float particle_wrap(float3 n, float3 l)            // p0.w: 0 is Lambert
{
    const float w = max(surface_parameters0.w, 0.0f);
    return saturate((dot(n, l) + w) / (1.0f + w));
}

float particle_phase(float cos_theta)              // Henyey-Greenstein times 4 pi: 1 everywhere at g = 0
{
    const float g = clamp(surface_parameters1.y, -0.99f, 0.99f);
    const float g2 = g * g;
    const float d = max(1.0f + g2 - 2.0f * g * cos_theta, 1.0e-4f);
    return (1.0f - g2) / (d * sqrt(d));
}

// once a pixel: the transmission's factor
const float transmission = max(surface_parameters1.x, 0.0f) * exp(-thickness * max(surface_parameters1.z, 0.0f));
// the sun and each point light: wrap + transmission * phase(dot(-l, v)); the others: wrap
```

**The sum:**

| Term | How |
| --- | --- |
| Ambient | `albedo ·` the SH irradiance at `n` times the ambient energy with IBL, else the flat ambient |
| Sun | `albedo · colour · energy · (wrap + transmission · phase) / π`, times the visibility: four `SampleCmpLevelZero` taps half a texel round the point, with a constant bias of a texel and a half, times the clouds and, in phase 2, the transmittance. Looked up only where the share isn't 0 |
| Fill | `albedo · colour · energy · wrap / π`, its shadow one gather with bilinear weights (bias 0.0025) |
| Spot | the same, its range and cone as `evaluate_spot_light`, its shadow one gather (2D, bias 0.0006) or one cube sample (omni, a world bias of 0.125), looked up only where the light reaches; none with `RECEIVES_NO_SPOT_SHADOW` |
| Lamps | the same per lamp, each with `evaluate_lamps`' falloff (`fade² · core² / (d² + core²)`); a lamp beyond its reach costs its distance test |
| Point lights | `albedo · colour · energy · falloff² · (wrap + transmission · phase)`, no 1/π (as `evaluate_point_lights`), then `ease_point_light`; a light beyond its reach costs its distance test |
| Cascades | `albedo · 2 · indirect_strength · Σ quadrant(xz, k) · wrap(n, l_k)` over the four directions `l_k` = `normalize(±1, 0.65, 0)`, `normalize(0, 0.65, ±1)`: the resolve's receiver term with the wrap, read where the resolve reads (half a texel back with source tops), none outside the field. Additive or not |
| Emission | as a lit batch's: texture times colour times energy, read only when the energy isn't 0 |

**Fog.**
- **own** is the fog's opacity at the particle's own view depth and drawn xz, with the composite's formula, edge fog
  included.
- **behind** is the composite's at the opaque surface behind it. Its view depth is the particle's times
  `scene distance / camera distance`, because both lie on one pixel's ray, and its xz is
  `camera + (P - camera) · scene distance / camera distance`. It is 0 over the background (distance 10⁶), where the
  composite adds none.
- **The colour written** is `c' = (c · (1 - own) + F · (own - behind)) / (1 - behind)`, with F the fog colour. The
  composite's `lerp(pixel, F, behind)` then gives exactly the particle fogged at its own depth over the background
  fogged at its own, for every layer of puffs over the pixel (the blend and the composite are affine; native test).
- **Below zero.** `c'` can be below zero where a dark puff covers far, fogged ground. The target is RGBA16F, the
  composite brings the result back above zero, and bloom thresholds the intermediate image.
- **Where behind is 1** (`fog_maximum_opacity` 1 over far ground, or the edge fog), the composite overwrites the pixel
  and nothing drawn in the transparent pass shows, as today. v2's fog never gets there (decision 6).
- **Without the distance** (its pass couldn't be made) the particle writes its colour unfogged and the composite fogs it
  as any transparent batch; the soft fade is skipped.
- The distance pass runs on every frame that draws a lit particle, as for soft particles.

**Its outputs.** The colour with its alpha, and the reactive mask under FSR 3 as every transparent batch writes it
(`btbh_temporal_reactive`).

### The UV rect

In `production_model.vert.hlsl`, after the batch's `uv_scale` and `uv_offset`:

```hlsl
if (instance_uv_rect != 0u)   // BTBH_GPU_SCENE_BATCH_INSTANCE_UV_RECT (RFC 0013)
{
    output.texture_coordinate = output.texture_coordinate * instance.custom.xy + instance.custom.zw;
}
```

- **The uniform** is a named word, `instance_uv_rect`, the first of the vertex block's three `temporal_reserved` words.
  The static vertex shaders declare their block with `BTBH_TEMPORAL_UNIFORMS_STATIC` (`production_temporal.hlsli`),
  which names it; the skinned ones keep `BTBH_TEMPORAL_UNIFORMS`, so their SPIR-V doesn't change. The block keeps its
  size and layout. The main pass and the cascades' source pass set it to 1 for a flagged batch, 0 for every other (as
  the reserved word was).
- **Every file built from `production_model.vert` takes it:** `production_model_detail_table.vert`,
  `production_model_lift.vert`, `production_model_detail_table_lift.vert` and `production_rc_material_source.vert`. So
  the rect works on every path that draws a static batch: the scene shader, the fast unlit shader, soft and lit
  particles, the lifted and the table variants, and the cascades' sources (decision 9 asked for the fast unlit path).
- **Unflagged batches** skip the branch, so their texture coordinate is bit for bit what it was.
- **A rect batch draws all its cells in one draw.** The cells no longer differ in `uv_offset`, which is in the run key.
- **Mip bleed** at a cell's edge is the atlas's to pad, as for any atlas.
- **Old shaders.** A rect batch drawn with an older vertex shader would show the whole atlas, so a present with the
  flag reads each static vertex shader a flagged batch draws with (the host's `production_model.vert`, its table or
  lifted sibling, and with the cascades on, for a flagged source, `production_rc_material_source.vert`) for the name
  `instance_uv_rect`, as the core reads the scene shader for the dig mask and the world detail, and fails with the
  path. Metal libraries aren't checked, as before.

### Sorting

Within a batch the renderer draws the instances in their order, which is the graphics APIs' primitive order; between
batches, in the order given. v2 builds its transient instances on the CPU each frame and sorts its lit puffs far to
near; its draw order is plain alpha particles, then the lit smoke, then the additive ones (decision 8). With the UV
rect its smoke is one batch, so a sort between batches isn't needed. The draft's 1d is dropped, with its post flag
(`SORT_TRANSPARENT`, post bit 3) and its macro; post bit 3 stays free.

### Phase 2: the transmittance map

```c
/* BTBHGPUSceneGeometryBatch.flags, after BTBH_GPU_SCENE_BATCH_INSTANCE_UV_RECT: */
    /* The batch shadows the sun's light in the particle transmittance map (RFC 0013 phase 2), when
     * BTBHGPUSceneLighting.particle_shadow is set and the sun's shadow is on. Its fragments' coverage (alpha, times
     * surface_parameters1.w for LIT_PARTICLE, 0 meaning 1) becomes optical depth. Transparent batches of surface
     * variant NONE, SOFT_PARTICLE or LIT_PARTICLE only: the present fails with BTBH_GPU_INVALID_ARGUMENT, naming the
     * batch, on any other. */
    BTBH_GPU_SCENE_BATCH_PARTICLE_SHADOW = 1u << 4

/* BTBHGPUSceneLighting, in the byte that is directional_shadow_reserved[0]: */
    /* The particle transmittance map (RFC 0013 phase 2): 0 none, as before; 1 lit particles read it; 2 so do the
     * opaque surfaces the sun shadows (lit, and unlit modes 2 and 3). Drawn only on frames whose sun shadow is on and
     * that draw a batch with BTBH_GPU_SCENE_BATCH_PARTICLE_SHADOW. Other values fail the present. */
    uint8_t particle_shadow;

/* Defined when BTBH_GPU_SCENE_BATCH_PARTICLE_SHADOW and BTBHGPUSceneLighting.particle_shadow exist. */
#define BTBH_GPU_SCENE_HAS_PARTICLE_SHADOW 1
```

**The map.**
- **Format and size.** One RGBA16F texture at half the sun map's size each way: 1024 × 1024, 8 MiB, under the default
  2048 map. v2's maps are 2048, 1536 and 1024 (Deck), redrawn every frame; half is plenty, a quarter would do on the
  Deck (decision 12).
- **What it holds.** Cleared to (0, 0, 0, 1). r is the optical depth τ, summed; g is τ times the sun-space depth d,
  summed; a is the nearest d. b is unused.
- **Blending.** One blend state does it, because SDL blends colour and alpha with separate operations: colour `ADD`
  (one, one), alpha `MIN`. It is order-independent.
- **When it is drawn.** Every frame, after the shadow maps, with the matrix the receivers read: the retained one when
  the sun's map is kept over several frames.
- **The draw.** `production_particle_transmittance.vert` draws the flagged batches, with billboards facing the light,
  so that a puff shadows with its whole disc. The light's right and up are the matrix's first two rows, normalized.
  The fragment shader, `production_particle_transmittance.frag`, writes `τ = -ln(1 - min(α, 0.999))`, `τ · d`, 0 and
  `d`. No depth target: what hides a puff from the sun shadows its shadow as well.
- **The fades.** The soft and near fades are the camera's, not the sun's, so the map leaves them out.

**Reading it.** At a receiver's sun-space uv and depth d, one bilinear fetch:
- `front = a`, `mean = g / r` and `back = max(2 · mean - front, front)`: the column's depth span, taking its
  coverage as even between front and back.
- `T = 1` before the front.
- `T = exp(-r)` past the back.
- `T = exp(-r · (d - front) / (back - front))` between them.

So a puff on a column's sunny side keeps its light, and one on the lee side takes the column's shadow. The ground
under the column takes all of it. One slab per texel is enough for v2's columns (decision 11).

**Who reads it.**
- **Lit particles,** when `particle_shadow` ≥ 1, multiply the sun's share by T (t9 in the full variant).
- **Opaque surfaces,** with mode 2 (2b; v2 wants it on desktop and will measure it on the Deck, decision 10). The scene
  fragment shader's next free slot takes the map (t15 in the full variant, its 16th and last; t14 in `_2d_shadows`; a
  1×1 fallback when off), and T multiplies `sun_visibility` in the lit path and the visibility of unlit modes 2 and 3.
  That is every shadowed variant: `production_model.frag`, `_2d_shadows`, `_rc`, `_rc_2d_shadows`.

## Minimal example

```c
/* v2's smoke: every puff of every emitter in one batch, its instances sorted far to near on the CPU. */
BTBHGPUSceneGeometryBatch smoke = {0};
smoke.kind = BTBH_GPU_SCENE_GEOMETRY_CUSTOM_MESH;
smoke.custom_mesh = puff_quad;           /* local xy, normal +z, tangent +x (w +1), uv 0..1 */
smoke.material_slot = -1;
smoke.first_instance = first_puff;       /* each puff's custom = { cell width, cell height, cell u, cell v } */
smoke.instance_count = puff_count;
smoke.albedo_texture = smoke_atlas;      /* sRGB colour, straight alpha the coverage */
smoke.normal_texture = smoke_normals;    /* its own texture, linear: rg the normal, b the thickness; the same cells */
smoke.normal_scale = 1.0f;
smoke.albedo_red = smoke.albedo_green = smoke.albedo_blue = smoke.albedo_alpha = 255;
smoke.uv_scale_x = smoke.uv_scale_y = 1.0f;
smoke.transparency_enabled = 1;
smoke.blend_mode = BTBH_GPU_SCENE_BLEND_ALPHA;
smoke.billboard_mode = BTBH_GPU_SCENE_BILLBOARD_FRONT;
/* depth compare, cull and stencil as the soft particles' today */
smoke.surface_variant = BTBH_GPU_SCENE_SURFACE_LIT_PARTICLE;
smoke.surface_parameters0[0] = 0.4f;     /* fades over 0.4 units where it meets the scene */
smoke.surface_parameters0[1] = 0.5f;     /* gone nearer than 0.5 units ... */
smoke.surface_parameters0[2] = 1.0f;     /* ... whole from 1.5 on */
smoke.surface_parameters0[3] = 0.5f;     /* wrap */
smoke.surface_parameters1[0] = 0.8f;     /* transmission */
smoke.surface_parameters1[1] = 0.6f;     /* forward scattering */
smoke.surface_parameters1[2] = 9.0f;     /* optical depth at thickness 1 (v2 bakes b = tau / 9) */
smoke.flags = BTBH_GPU_SCENE_BATCH_INSTANCE_UV_RECT;

/* Phase 2: the smoke darkens itself and the street. */
smoke.flags |= BTBH_GPU_SCENE_BATCH_PARTICLE_SHADOW;
lighting.particle_shadow = 2;
```

In v2's Rust: `batch_of` ends in `..Default::default()`, so the flags and parameters it doesn't name stay 0. The
smoke's builder sets the variant, the two parameter arrays and the flag, and writes each puff's cell into its custom
floats.

## Implementation

### Files (phase 1, as built)

| File | Change |
| --- | --- |
| `include/btbh_gpu/btbh_gpu.h` | The variant and its comment (and one for `SOFT_PARTICLE`), the batch flag, two macros |
| `shaders/gpu/production_particle_lit.frag.hlsl` | New: the lit particle (full variant) |
| `shaders/gpu/production_particle_lit_2d_shadows.frag.hlsl`, `..._unshadowed.frag.hlsl` | New: wrappers with the shadow defines, as `production_model_2d_shadows.frag.hlsl` |
| `shaders/gpu/production_model.vert.hlsl` | The UV rect, behind `instance_uv_rect`; its block through `BTBH_TEMPORAL_UNIFORMS_STATIC` |
| `shaders/gpu/production_temporal.hlsli` | `BTBH_TEMPORAL_UNIFORMS_STATIC`: `BTBH_TEMPORAL_UNIFORMS` with the first reserved word named `instance_uv_rect` (the skinned shaders keep the old macro and their SPIR-V) |
| `src/gpu/btbh_gpu_particle.h` | New: the refusals, the frame block, C copies of the wrap, the phase, the transmission and the fog's amount and make-up |
| `src/gpu/btbh_gpu_scene.c` | Below |
| `CMakeLists.txt` | The three files after the scene shaders in `BTBH_GPU_SHADER_SPECS` (76 shaders), their dependencies; the native test and the smoke |
| `Sources/BTBHRendererGPU/*.swift` | `GPUSceneSurfaceVariant.litParticle` (11), classified from a `lit_particle` surface shader as `soft_particle` is |
| Phase 2: `production_particle_transmittance.vert/.frag`, the particle shader's t9, the scene shaders' last slot (2b) | Later |

Unchanged: `production_model.frag` and every other fragment shader, the skinned vertex shaders, the shadow shaders, the
cascades' and the post chain's shaders.

### The core

- **Checks.** The refusals above, in the batch checks every present and prewarm runs
  (`btbh_gpu_scene_validate_world_details`), and bit 3 among the known flags.
- **Pipelines.** A small array of their own, `lit_particle_pipelines[3 shadow sets][2 temporal][depth][cull][stencil]`:
  always transparent with straight alpha, so their colour targets are the transparent pass's alone (the auxiliary
  outputs don't index them). `btbh_gpu_scene_pipeline_slot` and `btbh_gpu_scene_ensure_model_pipeline` pick it for a lit
  particle; the fragment file is a sibling of the host's fragment shader, by the frame's shadows, made with 9, 8 or 5
  samplers and two uniform buffers. Made on first use, and by `btbh_gpu_scene_ensure_model_pipelines` for prewarm (the
  window's and the render target's).
- **Bindings in the transparent pass.** For a lit particle: the slots above, the frame block in fragment slot 1 when
  the slot doesn't hold it; every other batch's block there marks it gone.
- **The distance pass** runs on frames with a lit particle (`lit_particles_active`, beside the soft particles' and the
  refraction's), and prewarm makes it.
- **The cascades.** Lit particles never feed the source field (`btbh_gpu_scene_batch_material_source`). The frame block
  carries the cascades' field when they lit this frame (their encode ran, not the fallback view), and the binding their
  atlas, `hrc_quadrant_textures[hrc_history_read_index]`.
- **The UV rect:** `instance_uv_rect` in both passes that fill the vertex uniforms, and the SPIR-V name check
  (`btbh_gpu_scene_check_uv_rect_shaders`, present and prewarm); the shader check cache grows from 8 to 16 paths.
- **Missing files.** A lit particle whose `production_particle_lit*.frag` is missing fails the present with the file's
  path ("A lit particle's fragment shader is missing (build the shaders from 0.11.0 or later): <path>"); a SPIR-V file of
  that name that doesn't name `particle_scene_distance` is refused too ("… doesn't draw lit particles …"), and so is a
  rect batch whose static vertex shader doesn't read the rect ("A batch's UV rect (INSTANCE_UV_RECT) isn't read by its
  vertex shader …"). Older shaders draw everything else as before.

### Cost

Estimates before it was built; measured in "Implementation notes".

| Item | Estimate |
| --- | --- |
| Off (no lit particle, bit 3 clear) | One uniform test per static vertex for the UV rect, branch not taken; on the CPU, a flag test and a variant test per batch |
| A soft particle in unlit mode 3 today | 5 texture reads (albedo, emission, the distance in the normal slot, ORM, the distance again) plus 4 comparison taps under the soft filter, or 16 gathers under the tent |
| A lit particle | 14 reads (13 without an emission, which is skipped when its energy is 0): albedo, normal, distance, emission, 4 comparison taps, the fill's gather, the spot's, 4 from the cascades' atlas. About 150 ALU, plus about 20 for each lamp or point light in reach and 6 for each out of reach |
| Memory, phase 1 | The distance target (R32F at the render size), made already when soft particles draw |
| CPU, phase 1 | Per lit particle batch: 9 samplers; the 176-byte block once a pass |
| Phase 2 pass | The flagged puffs again, at half the sun map's resolution, one read and about 10 ALU per fragment |
| Phase 2 reads | One bilinear RGBA16F read and about 10 ALU per lit particle fragment and, with mode 2, per sun-lit opaque pixel |
| Phase 2 memory | 8 MiB under a 2048 sun map |

## Interactions

| With | What happens |
| --- | --- |
| Radiance cascades, as a source | Never: a lit particle stays out of the source field, emission or not (decision 7) |
| Radiance cascades, as a receiver | It reads the field at its xz from four directions, as an unlit receiver does, additive or not. The field is at street level, so a puff high above a street takes the street's light. v2's are additive (RFC 0010) |
| RFC 0010's source tops | Its reading is half a texel back, as the resolve's |
| SSR | Computed in the SSAO resolve, before the transparent pass: wet streets don't reflect the puffs, as with every transparent batch |
| Refraction | Refracting batches draw after, over a copy that includes the puffs. A refracting batch behind a puff draws over it, showing it displaced, as for every transparent batch |
| HBAO, SSAO | Before the transparent pass: puffs are neither occluded nor occluding |
| FSR 3 | The reactive mask as every transparent batch, `reactive_mode` included; no motion vectors |
| Render to target | Its own runtime: its own distance target, its cascades and, in phase 2, its own map, made by `btbh_gpu_window_scene_prewarm_render_to_target`. Never temporal, so no reactive mask |
| Prewarm | The particle pipelines and the distance pass join through the prewarm's own steps |
| RFC 0011 runs | Lit particle batches run as any batch; the frame block is pushed once a run of them |
| RFC 0012 | A lit particle can't be lifted (refused). A lifted batch takes the UV rect (`production_model_lift.vert`) |
| World curvature | The vertex shader bends the puffs as everything else. The light and the cascades read the flat world position; the fades and the fog the drawn one, as the depth buffer and the composite see it |
| Opaque sort | Unchanged |

## Compatibility

- **Versioning:** minor (0.11.0 for phase 1). An enum value and a free bit of the batch's flags, as 0.9.0 and 0.10.0
  took bits of them; in phase 2 a second bit and the lighting's reserved byte. No struct grows.
  `BTBH_GPU_SCENE_SURFACE_COUNT` goes from 11 to 12, as it went from 10 to 11 with RFC 0005. A host that used it as a
  past-the-end value is refused at 12 as it was at 11.
- **Shaders.**
  - Phase 1 changes `production_model.vert` (the rect) and the four files built from it; it adds three fragment shader
    files (76 shaders). Every unflagged batch draws as before, to the byte. The skinned vertex shaders and every
    existing fragment shader compile to the same SPIR-V.
  - 0.10.0's shaders under the new core draw everything but lit particles and rect batches as before.
  - A lit particle fails naming its missing file; a rect batch fails naming the vertex shader that doesn't read it.
- **Be The Bullet Hell v2 (Rust, bindgen):** nothing changes until it opts in. bindgen gives the new enum value and
  flag; `batch_of` already ends in `..Default::default()`. To adopt:
  - bump the pin and build the three particle shaders (`CMakeLists.txt`'s list, which `build_shaders.py` reads);
  - bake the normal and thickness atlas;
  - replace the five tilts, the sprite turning, the emission's sky fill and the CPU light gathering with the variant;
  - merge its smoke draws with the rect (and its plain particles, flipbooks, weather and city-life sprites);
  - keep sorting its puffs.
- **Be The Bullet Hell (v1, Swift):** nothing changes. Its `softParticle` case keeps its value; a `litParticle` case
  (11) classifies a `lit_particle` surface shader, with `u_softFade`, `u_nearStart`, `u_nearFade`, `u_wrap`,
  `u_transmission`, `u_asymmetry` and `u_opticalDepth`. The Swift layer sets no batch flags, so the UV rect has no Swift
  API.
- **CHIMERA (Rust, bindgen):** nothing changes. `SOFT_PARTICLE` is untouched and its list of 14 shaders can stay.
- **Tyrian3D (C):** nothing changes. It zeroes its batches and is pinned at v0.2.1.

## Platforms and testing

- **Native:**
  - `btbh_gpu_scene_order_test`: `LIT_PARTICLE == 11`, `COUNT == 12`, the flag bit 8, the batch's size and flags'
    byte, phase 2's byte still reserved at 3,337, the macros.
  - `btbh_gpu_particle_test` (new, on `btbh_gpu_particle.h`): the phase is 1 at g = 0 and its mean over the sphere 1
    for any g; the wrap is Lambert at 0; the fog's make-up gives the reference for random puffs, backgrounds and fogs,
    stacks included; the fog's amount; every refusal; the frame block's layout.
  - `btbh_gpu_phase2_shader_test`: the three variants' samplers (9, 8, 5), uniform buffers and interfaces.
- **GPU smoke** (Windows/Vulkan, `btbh_gpu_lit_particle_smoke`, new): see "Implementation notes".
- **Off, byte for byte:** the image smokes against `release/0.11.0`'s, and the shader artefacts.
- **Bench:** the frame bench's smoke modes (`smoke-off`, `smoke-soft`, `smoke-lit`), and mode `none` against
  `fc2784ea`'s shaders and core (the off cost).
- **Platforms:** Windows/Vulkan runs the smokes. The Mac session runs `check.sh` (the new files through shadercross to
  MSL; they translate here too). Linux and the Deck through v2.
- **Games:** v2's smoke captures by day, at dusk and at night, with its fake lighting and with the variant; its frame
  times on the Deck preset.

## Alternatives

| Alternative | Why not (now) |
| --- | --- |
| **A branch of `production_model.frag`** (variant 11 in the shared shader) | One sampler slot left; the particle needs more textures. Every batch would carry the longer shader |
| **Keep the five baked tilts** | v2's current smoke; no lamps, point lights, cascades or self-shadow, and a turn per sprite |
| **Six-way lightmaps** (light from six directions baked from a simulation, two RGBA textures) | Richer for simulated smoke at the same cost class, but v2 bakes a normal and a thickness. A later variant if v2 bakes from simulations |
| **All light per vertex** | Cheaper, but loses the normal map's detail. Not needed: lights out of reach exit early (decision 4) |
| **Fog before the transparent pass** (a post flag: the composite's fog moves between the opaque and transparent passes, and transparent shaders fog themselves) | Correct where the composite's fog is total, but changes every transparent batch's look under that flag. v2's fog is never total (decision 6) |
| **Deep or Fourier opacity maps** (layers of opacity along the sun's ray) | They separate puffs stacked along a ray, but need two textures (or a second pass). One slab is enough for v2 (decision 11) |
| **Puffs in the sun's depth map** (alpha-tested) | A hard, binary shadow, and the map is retained over frames while puffs move |
| **A batch sort in the renderer** (the draft's 1d) | v2 doesn't need it: its smoke is one batch with the rect, and its draw order is fixed (decision 8) |
| **Sorting instances on the GPU** | A compute sort, a sorted index read by every vertex shader of the batch and a permuted motion buffer, to redo what v2 does on the CPU with the positions in hand |
| **Sorting instances on the CPU in the renderer** | The same work as v2's, plus a copy per batch (ranges may be shared) and a permutation of the motions |
| **Order-independent transparency** (weighted, blended) | It would remove sorting, but changes the transparent pass's targets for every batch |
| **Premultiplied alpha** | v2 draws straight alpha (decision 5); refused in phase 1, easy to add (the fades and the fog's F term times alpha) |
| **Lit particles as cascade sources** | The source pass has no camera distance for the fades; v2 keeps its embers additive and unlit (decision 7) |

## Open questions for v2

These were the draft's questions; "Decisions" holds the answers.

1. **The normal map.** Linear, OpenGL +Y (green up), xy from `rg · 2 - 1` and z rebuilt, the tilt scaled by
   `normal_scale` (0 flat)? Thickness in b, linear 0..1, made optical depth by `p1.z`: is that the range you bake, or do
   you need more than 1? Is it a texture of its own on the albedo's cells?
2. **The light model.** The wrap as `(n·l + w) / (1 + w)`? The transmission as
   `albedo / π · p1.x · exp(-b · p1.z) · 4π·HG(g, cos θ)`? Every light at the scale lit surfaces give it?
3. **The phase for which lights.** Every light both terms, or the sun only?
4. **Counts.** Puffs on screen, their size, layers, lamps and point lights in reach at night: does it need the
   per-vertex fallback?
5. **Blend.** Alpha or premultiplied for the lit smoke?
6. **Fog.** Your fog by stage; is the particle's make-up enough, or do you need the post flag that fogs before the
   transparent pass?
7. **The cascades.** Should puffs take the field's light at their xz? Should emissive puffs stay sources, or will you
   set `NO_RC_SOURCE`?
8. **Sorting.** Is your own instance order enough? Do you need the batch sort?
9. **The rect.** Which batches would use it? Do any of them need their custom floats for something else?
10. **The ground.** In phase 2, do you need the street to take the smoke's shadow (mode 2)?
11. **Stacked puffs.** One slab per texel, or layers?
12. **The sun's map.** Its size and update period; is half its resolution enough?
13. **Release.** Phase 1 in 0.11.0, phase 2 once your phase 1 captures are in?

## Decisions (v2, 2026-11-23)

v2 answered all 13 questions on 2026-11-23, through its session (relayed from the agent who built its fake-lit smoke).
The owner left the decision to the coordinator, who accepted phase 1 (1a, 1b, 1c) for 0.11.0, dropped 1d, and left
phase 2 (2a, 2b) for after v2's phase 1 captures.

**v2's finding first: the tent filter's cost.** Under `TENT`, unlit mode 3's sun-shadow test was about 60 % of a smoke
layer's GPU cost: per full screen of smoke, 0.028 ms with the sun's shadow in unlit mode 3, 0.011 ms for soft particles
without it, 0.004 ms for the fast unlit shader. v2 now shadows only smoke reaching below 2.5 cells, by day and at dusk
(columns and smoke over roofs use unlit 1, nothing is shadowed at night): the smoke's added transparent cost in a
burning fight went from +0.022 to +0.009 ms. Its lit smoke takes 9–11 draws a frame (13 at most).

1. **The normal map: as proposed.** Linear, OpenGL +Y, xy = rg·2−1, z rebuilt, the tilt scaled by `normal_scale`. A
   texture of its own on the albedo's cell rects; the atlas shrinks to 4 cells of 128 px once the five tilts go. The
   thickness is b, linear 0..1, b = τ / 9 with `p1.z` ≈ 9: the view optical depth peaks at 8.75 (p95 8.0), 7.7, 3.6 and
   3.3, and 8 bits give steps of 0.035 in τ. The normal comes from the density gradient where the view ray reaches half
   its coverage.
2. **The light model: yes to all three.** The wrap is `(n·l + w) / (1 + w)`; the transmission is tinted by the albedo,
   divided by π, with 4π·HG; every light at the scale lit surfaces take (v2's fake was calibrated to it: its key light
   about 0.9 of a Lambert surface).
3. **The phase for the sun and the point lights.** Fire glowing through smoke is the night look. The fill, the spot and
   the lamps take the front (wrap) term only, since lamps on poles below the smoke light its undersides.
4. **Counts** (a burning fight, 60 one-second samples, the same on the Deck and High 1080p):
   - lit puffs: median 107, p90 154, max 185; at most 300 particles in all, plus 4 columns on the Deck (7 on High) of
     6–12 puffs each;
   - puff size: a median coverage disc of 5.6 % of the screen's height (45 px at 800p); the largest reach 0.5–1.1
     screens near the camera;
   - shaded cards: median 0.38–0.40 screens, p90 0.85–0.94, max 1.3;
   - layers: ≥1 over 10 % of the screen (p90 25 %, max 38 %), ≥3 over 3 % (6.5 %, 12 %), ≥6 over 0.5 % (max 2.9 %); the
     deepest stacks are 10–19 layers;
   - lights at night: up to 8 lamps (16 on High), 6 fire point lights (10), the kaiju's glow, blast flashes and 8 neon
     signs; about 3 lights in reach of a puff on average (p90 6, max 27 in a fire cluster).

   No per-vertex fallback is needed if lights out of reach exit early: they do.
5. **Blend: straight alpha** (`BLEND_ALPHA`) with straight-alpha textures. Each sprite's alpha is capped at 0.5 and
   thinned where enemies are behind it. So phase 1 takes `BLEND_ALPHA` alone.
6. **Fog: the make-up is enough.** v2's fog by look (start, density, greatest opacity, colour): day 30, 0.03, 0.40;
   dusk 30, 0.03, 0.45; storm 28, 0.032, 0.55; night 30, 0.03, 0.50; storm night 30, 0.03, 0.55. The game divides the
   start by 1.2, multiplies the density by 1.2 and adds the camera's extra distance from the classic camera; edge fog
   is off. The fog never reaches full opacity, so the make-up is exact. No post flag.
7. **The cascades: puffs take their light** at their xz when glow lighting is on (v2's additive experiment). No lit puff
   emits: embers and flames stay additive unlit sprites, and v2 would set `NO_RC_SOURCE` on any emissive lit batch.
   (Today's fake puts the sky fill in the emission, which makes smoke a weak source; the variant removes that.) The
   coordinator took the cleaner option: lit particles never feed the source field.
8. **Sorting: v2's own order is enough.** It sorts its lit puffs far to near on the CPU every frame; its draw order is
   plain alpha particles, then the lit smoke, then the additive ones. The batch sort isn't needed: 1d is dropped.
9. **The rect** would merge the lit smoke (9–11 draws become 1), plain particles (up to 12 cells × blend become 2),
   effect flipbooks (one draw a book, not a book and frame), and weather and city-life sprites. None of them use the
   custom floats (v2 uses them only on bodies, the wreck dissolve, and on the kit's world-detail seed, never on
   billboards). v2 asked that the rect work on the fast unlit shader too: it works on every path that uses
   `production_model.vert`.
10. **The ground (2b): yes on desktop.** A column's shadow on the street is the landmark cue the owner asked for. The sun
    is low (28° by day, 16° at dusk), so a 12-cell column lays a long shadow. On the Deck, v2 starts with
    `particle_shadow` 1 and measures 2.
11. **Stacked puffs: one slab is fine.** One column per fire, at least 5 cells apart, 4–7 at most; the low sun crosses
    one column per ray. No layers.
12. **The sun's map:** High 2048, Medium 1536, Low and Deck 1024, redrawn every frame. It spans about 70–80 cells on the
    classic camera (about 0.035 cells a texel at 2048, 0.07 on the Deck). Columns are 1–7 cells wide, so half is plenty
    and a quarter would do on the Deck.
13. **Release: phase 1 in 0.11.0, phase 2 after v2's phase 1 captures.** On phase 1 v2 rewrites `smoke.rs` in one step:
    bake the normal and thickness, one sorted rect batch, and drop the five tilts, the per-sprite turn, the emission's
    sky fill and the CPU light gathering. It keeps the columns, the readability thinning and the coverage parity.

**What the answers change in the design** (applied above): the transmission for the sun and the point lights only;
lights out of reach exit early; straight alpha only; the fog's make-up with no post flag; lit particles never cascade
sources; no batch sort (1d and its post flag and macro gone); the rect on every static vertex shader's path; `p1.z`
about 9 in the examples.

## Implementation notes (phase 1, 2026-11-23)

Built on `feature/0.11.0-lit-particles`, on `release/0.11.0` (`fc2784ea`). Nothing pushed or merged.

**As built, against the draft.**
- **Dropped:** the batch sort (1d) and its post flag and macro (decision 8).
- **Narrowed by the decisions:** straight alpha only; the transmission for the sun and the point lights; lit particles
  never in the cascades' source field (`btbh_gpu_scene_batch_material_source`, which the source pass and the UV rect's
  shader check share).
- **The slots** put the shadow maps last (t5–t8), so each variant's slots are the next one's and some more (9, 8, 5
  samplers). The draft had the atlas at t8.
- **The scene's distance is sampled** through a nearest sampler (the fallback texture's) at the pixel's centre: a
  texture only loaded is reflected by shadercross as a storage texture, which the scene's pipelines don't declare.
- **The fades' and the fog's distances** come from the fragment's depth through the frame's inverse view-projection:
  the point the depth buffer would hold, the world bent as drawn, as the distance pass and the composite see it. The
  light and the cascades read the flat world position. So the block grew from the draft's 112 to 176 bytes (the
  inverse view-projection and the render size).
- **The UV rect's word** is named through a second macro (`BTBH_TEMPORAL_UNIFORMS_STATIC`), so the skinned vertex
  shaders keep their block's names and their SPIR-V.
- **The lit particle's pipelines** are an array of their own (`[3][2][depth][cull][stencil]`, 54 pointers), not a third
  value of every model pipeline array's last index, which would have grown each by half.
- **A lit particle can't be lifted** (refused): its pipelines take only `production_model.vert`. A lifted batch takes
  the UV rect.
- **Without a `normal_texture`** a lit particle's block gets `normal_scale` 0 (the fallback's rg is 127/255, a tilt of
  -0.004) and the fallback's b (1) as its thickness.
- **Every texture with implicit derivatives is read before the discard;** a fragment the fades leave at alpha 0 is
  discarded before its light. The omni cube is read at level 0.
- **Without the distance** (its pass couldn't be made) there is no soft fade and no make-up: the composite fogs the
  puff as any transparent batch.
- **The messages:** "A lit particle's fragment shader is missing (build the shaders from 0.11.0 or later): <path>",
  "… doesn't draw lit particles …", "A batch's UV rect (INSTANCE_UV_RECT) isn't read by its vertex shader …". The
  report's 256 characters can cut a long path short.

**Tests** (Windows, RTX 3070 Ti, Vulkan):
- `tools/check.ps1`: 31/31 (29 before, the particle's native test and the lit particle smoke); 76 shaders; the public headers clean under `/W4 /WX`.
- Swift (`BTBH_ENABLE_GPU_TESTS=1`): the build is clean; 301 tests, 2 skipped, 0 failures (the vertex uniforms' check updated for the named word).
- `btbh_gpu_particle_test`: 20,155 checks.
- `btbh_gpu_lit_particle_smoke`: 126 checks. The light against the CPU at 41 pixels (three channels each): 38 exact,
  3 one level off. The fog's make-up over the wall and over the sky: within 0 levels (the composite alone would put the
  puff 14 and 18 levels off). The soft fade within 0.0052 of the CPU's alpha over 200 fading pixels (and SOFT_PARTICLE's
  within 0.03), the near fade within 0.0065 over 1,662. The UV rect: 0 pixels apart from four batches with `uv_offset`
  on all eight paths. Prewarm: 43 pipelines and 61 shaders for 14 scenes into a target, then 0 made by the renders;
  the window's prewarm, with and without FSR 3, then 0 made by its presents. With
  `BTBH_LIT_PARTICLE_OLD_SHADER_DIR` at `fc2784ea`'s shaders: its `production_model.vert` draws unflagged batches as this
  one's, to the byte, on four paths, and its `production_rc_material_source.vert` fails a rect source.
- The flags smoke's unknown batch flag is now bit 4 (bit 3 is the rect).

**Identity against `fc2784ea`.** The merged 0.11.0 build's images (`btbh-rel-0110-outs*`) against this build's, the nine
image smokes (flags, font, lift, prewarm, runs, SDF terrain, scene target, RC tops, world detail): 278 images,
16,219,008 pixels, 0 differ, in each of the three modes (nothing set, `BTBH_SMOKE_SHARED_MESHES=1`,
`BTBH_GPU_SCENE_RUNS=0`). Shader artefacts: 206 of 219 files byte for byte; the 13 that differ are the five static
vertex shaders' SPIR-V and MSL (the rect; their reflection is unchanged) and three FSR 3 MSL files that differ between
any two builds. Every fragment shader and the skinned vertex shaders compile to the same bytes.

**Cost.** Windows, RTX 3070 Ti, Vulkan, the frame bench's Deck preset (960 x 600 to 1280 x 800, HBAO,
FXAA, the soft sun shadow, 2,001 kit draws), all under the machine's perf lock (06:35:51–06:37:30; one other session's
`v2-game` process ran through it).
- **Against v2's smoke today** (`smoke-soft`: unlit soft particles in 10 draws; `smoke-lit`: one lit particle batch with
  the rect, a normal and thickness atlas, the soft fade and the transmission; 170 puffs, discs 5.8 % of the height, ≥1
  layer over 11.2 % of the screen, ≥3 over 3.4 %, ≥6 over 1.2 %, the deepest stack 14, 3.4 lights in reach of a puff,
  under 16 lamps, 10 point lights and the sun's shadow): the transparent pass, distance pass included, 0.067 ms without
  puffs, 0.110 ms soft, 0.112 ms lit. Lit against soft +0.002 ms (90 % interval −0.003 to +0.005), the whole frame
  +0.005 ms (−0.012 to +0.021): not resolved. The lit shader shades only the discs (alpha 0 is discarded first), where
  the soft particles' full scene shader shades the whole card.
- **Off:** this branch's shaders against `fc2784ea`'s under this core, mode none: the opaque pass −0.002 ms (−0.004 to
  +0.001), the frame 0.000 ms (−0.003 to +0.004) for 4.4 million vertices: the rect's test isn't measurable. The CPU
  (`fc2784ea`'s bench, core and shaders against this branch's, 10 alternating processes each): prepare +5 µs, geometry
  encode −8 µs, shadow encode +1 µs, the present +4 µs a frame, all inside the pairs' spread: nothing resolved.
- The Deck isn't measured; v2's captures will say.

**Left for phase 2.** The transmittance map (2a) and the ground's read of it (2b); `surface_parameters1.w` (the density
it shadows with) and batch bit 4 and lighting byte 3,337 stay reserved. Not done here: premultiplied alpha; the Metal
build (`check.sh` on the Mac; the MSL is generated here); the Vulkan validation layers; the Deck; v2's own frames.

## Phases

1. **Phase 1 (0.11.0): items 1, 2 and 3** (Decisions).
   - 1a: `LIT_PARTICLE` and its three fragment shader files. The ambient, the sun with four plain taps and the clouds,
     the fill, the spot and omni light, lamps, point lights (the transmission for the sun and the point lights), the
     cascades, emission and fog with its make-up.
   - 1b: the soft and near fades, with the distance in its own slot. Part of 1a's shader; `SOFT_PARTICLE` unchanged.
   - 1c: `BTBH_GPU_SCENE_BATCH_INSTANCE_UV_RECT` in every static vertex shader, behind the named `instance_uv_rect` word.
   - ~~1d: `BTBH_GPU_SCENE_POST_SORT_TRANSPARENT`~~: dropped (decision 8). v2 sorts its instances; post bit 3 stays free.

   Plus the checks, the C copies, the tests, the bench's smoke modes, prewarm and render to target.
2. **Phase 2 (after v2's phase 1 captures): item 4.**
   - 2a: `BTBH_GPU_SCENE_BATCH_PARTICLE_SHADOW`, `particle_shadow` 1, the RGBA16F map at half the sun map's size, its two
     shader files, the read in the particle shader.
   - 2b: `particle_shadow` 2, the map in the scene shader's last free slot (decision 10: yes on desktop).
3. **Not planned:** sorting in the renderer, between batches or within one, on the GPU or the CPU (item 5 is v2's).
4. **Later, only if asked:**
   - premultiplied alpha;
   - fog before the transparent pass;
   - lamps and point lights per vertex;
   - layers in the map;
   - six-way lightmaps;
   - particles in SSR.
