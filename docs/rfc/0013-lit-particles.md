# RFC 0013: Lit particles

- **Status:** Draft (2026-11-23), waiting for v2's answers
- **Requested by:** Be The Bullet Hell v2 (its session, 2026-11-23, after it shipped fake-lit smoke)
- **Affects:** Be The Bullet Hell v2 (adopts it). Be The Bullet Hell (v1, Swift), CHIMERA and Tyrian3D: nothing, it is
  opt-in. They rebuild the scene shaders with the C core, as every release. `SOFT_PARTICLE`, which CHIMERA and v2 draw,
  doesn't change
- **Issue:** none yet
- **Target:** 0.11.0 for phase 1, or later; phase 2 after phase 1's captures
- **Depends on:** nothing. Written against 0.10.0's work in progress (`c5390f0b`). On 64-bit the batch is 280 bytes
  with `flags` at 253, `BTBHGPUScenePost` is 432 bytes with `flags` at 427, and `BTBHGPUSceneLighting` is 3,340 bytes
  with `directional_shadow_reserved` at 3,337 (`tests/btbh_gpu_scene_order_test.c:24`, `:31`, `:112-115`; the last
  offset worked out from the header). No struct grows. The new values go in an enum, in free bits of the two flag
  bytes and, in phase 2, in one reserved byte of the lighting

## Need

v2 has shipped smoke that only looks lit. From its request:

- It bakes lit puffs at five light tilts and turns each sprite toward the sun or a nearby fire, picking the tilt.
- It draws them with the renderer's soft particles (`SOFT_PARTICLE`) and unlit mode 3, so the sun's shadow darkens them.
- It needs 6–20 draws for its smoke.
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
  (`src/gpu/btbh_gpu_scene.c:4676-4679`) and a batch flag it doesn't know (`:4701-4705`).
- **Soft particles** (`shaders/gpu/production_model.frag.hlsl:496-511`):
  - The normal slot carries the opaque scene's distance from the camera. The billboard's alpha fades over
    `surface_parameters0.x` render units where it meets the scene.
  - The core swaps the texture into slot 2 (`btbh_gpu_scene.c:9417-9424`). Outside the transparent pass, or without the
    distance, it draws the batch as `NONE` (`:9275-9280`).
  - The distance is an R32F target at the render size (`:2447-2490`), written by `production_scene_distance.frag`
    between the opaque and transparent passes (`:8664-8698`). Its pipeline is made on first use (`:2409-2445`), and the
    pass runs only on frames that draw a soft particle or a refracting batch (`:2540-2555`, `:7462-7476`). The
    background's distance is 10⁶ (`production_scene_distance.frag.hlsl:24-27`).
  - A soft particle still runs the full shader: it samples the albedo, emission, "normal" and ORM slots
    (`production_model.frag.hlsl:2295-2318`), then loads the distance again.
- **Unlit modes 2 and 3** (`btbh_gpu.h:961-963`, `production_model.frag.hlsl:2386-2404`):
  - The albedo is multiplied by `lerp(0.45, 1, visibility)` of the sun's shadow.
  - Mode 3 asks for the "plain" lookup: four comparison taps with the receiver plane's gradient (`:1109-1123`). That
    lookup belongs to the soft filter only. Under the tent filter, the default (`btbh_gpu.h:1079-1087`), mode 3 takes
    the tent's 16 gathers (`production_model.frag.hlsl:1253-1318`).
  - Point lights brighten unlit surfaces too (`:2468-2471`).

### The transparent pass

- **Which batches.** `transparency_enabled`, or a blend other than `ALPHA`, puts a batch in the transparent pass
  (`btbh_gpu_scene.c:1189-1195`). Its pipelines write no depth (`:1500-1505`). It writes the colour, plus the reactive
  mask under FSR 3 (`:1532-1537`, `:1565-1577`, `:8723-8745`).
- **What runs before it, in order:**
  - the opaque pass;
  - the radiance cascades, whose compose adds their light to the opaque pixels (`:9624-9660`;
    `btbh_gpu_radiance_cascades.c:3604-3640`);
  - the HBAO (`btbh_gpu_scene.c:8463-8558`);
  - the SSAO and SSR resolve (`:8559-8663`);
  - the distance pass.
- **What it draws over.** The transparent pass draws on the result of all that (`radiance_cascades_post_input`,
  `:8725`). Refracting batches follow in a pass of their own, over a copy that includes the transparent pass
  (`:8699-8722`).
- **The order.** The batches draw in the order given. The opaque sort walks its own order, but only in the opaque pass
  (`:9142-9150`; `btbh_gpu.h:1233-1234`). Its key is the view depth of the batch's mean instance centre
  (`btbh_gpu_scene.c:5040-5087`), sorted in runs between pinned batches (`:5153-5214`).
- **Shadows.** Transparent batches cast none (`:6277-6285`).
- **The cascades' light.** No transparent batch takes it. The compose adds it to the opaque pixels before the
  transparent pass draws. In the transparent pass, even the cascades' fragment shader (`production_model_rc*.frag`)
  writes only its lit colour.

### Instances and texture coordinates

- **The instance** is 24 floats (`btbh_gpu_scene.c:21-23`): `float4x4 model`, `float4 color`, `float4 custom`
  (`production_model.vert.hlsl:18-23`). The packer writes the matrix by columns: the x axis times its scale in floats
  0–2, the y axis in 4–6, the translation in 12–14. The colour goes in 16–19 and the custom floats in 20–23
  (`src/gpu/btbh_gpu_instance_buffer.c:346-385`).
- **Who reads the custom floats:**
  - `DISSOLVE` reads x, y and z; `SHATTER` reads y, z and w; `GLITTER` reads x (`btbh_gpu.h:852-853`;
    `production_surface.hlsli:231-290`).
  - A world detail's seed reads the one its `seed_component` names (`production_world_detail.hlsli:110`).
  - Nothing else does. The vertex shader passes them on (`production_model.vert.hlsl:279`).
- **The texture coordinate** is the mesh's, times the batch's `uv_scale` plus its `uv_offset`
  (`production_model.vert.hlsl:274-275`). Both are in the same-material run key (`btbh_gpu_scene.c:4904-4907`). So
  batches that differ only in their atlas cell don't run together, which matches v2's 6–20 draws.
- **The vertex uniforms** use only `surface_controls.x`, the surface time (`production_model.vert.hlsl:49`;
  `btbh_gpu_scene.c:9227`, and `:6751` in the cascades' source pass). Its y, z and w are free, and so are the block's
  three `temporal_reserved` words (`production_temporal.hlsli:13-17`; `btbh_gpu_scene.c:91-92`).
- **A billboard's frame.** The instance's linear part takes the vertex's offset, normal and tangent, which are then
  laid out along the camera's right, up and `cross(right, up)` (`production_model.vert.hlsl:153-198`). The outputs are
  normalized world normal, tangent and bitangent, the bitangent being `cross(normal, tangent)` times the tangent's w
  (`:200-219`).
- **What the shadow vertex shader passes on:** no texture coordinate (`production_shadow.vert.hlsl:31-37`). Shadow
  passes never sample a texture.

### What a lit pixel's light is made of

| Light | Function (`production_model.frag.hlsl`) | Per pixel |
| --- | --- | --- |
| Sky ambient | SH irradiance at the normal with IBL, else the flat ambient (`:2586-2614`, `:1554-1577`) | 9 SH terms |
| Sun | `evaluate_directional_light` (`:664-738`): Disney diffuse and GGX, times 1/π; the shadow through `primary_directional_visibility` (`:1242-1319`): the tent's 16 gathers, or the soft filter's search and 4–32 taps (`:1067-1236`); the clouds (`:847-865`) | 16+ fetches |
| Fill (secondary) | the same BRDF; its shadow a 3×3 tent of gathers at 512 (`:1321-1366`) | 9 gathers |
| Spot | `evaluate_spot_light` (`:782-841`); its shadow a 3×3 tent at 1024 (`:1443-1494`), or 9 cube samples for the omni light (`:1382-1440`) | 9 fetches |
| Street lamps | `evaluate_lamps` (`:870-910`): up to 40, each through the full BRDF, 1/π included | ALU |
| Point lights | `evaluate_point_lights` (`:742-763`): up to 32, diffuse, facing `n·l × 0.7 + 0.3`, no 1/π; eased under a ceiling (`:769-780`) | ALU |

The frame's lighting is one fragment uniform block (`MaterialPBR`, `:131-191`). Since RFC 0011 the core fills it once a
pass and each batch writes only its material fields over it.

### The radiance cascades

- **Sources.** A batch is a source when it is unlit and no receiver, or when it has an emission
  (`btbh_gpu_scene.c:6688-6695`). The source pass's fragment shader writes its emission, plus its albedo when it is
  unlit, times alpha (`production_rc_material_source.frag.hlsl:35-64`). The source pass's vertex shader is
  `production_model.vert` with `BTBH_RC_SOURCE_PROJECTION` (`production_model.vert.hlsl:222-233`).
- **The light at a place** is a four-quadrant atlas: the light arriving at each field texel from four directions,
  each `(±1, 0.65, 0)` or `(0, 0.65, ±1)`.
  - The reproject pass writes it to `hrc_quadrant_textures[history_write_index]` (`btbh_gpu_radiance_cascades.c:3443-3460`).
  - The compose reads it (`:3632`), then makes that index the history's read index, with the field's origin and cell
    size (`:3660-3663`; `src/gpu/btbh_gpu_radiance_cascades_runtime.h:44`, `:76-82`).
  - So during the transparent pass, `hrc_quadrant_textures[hrc_history_read_index]` holds this frame's light.
- **The resolve's unlit receivers** read it at the pixel's xz (`production_hrc_resolve.comp.hlsl:70-84`, `:233-234`).
  So do all surfaces under additive cascades. Their bounce light is
  `albedo · Σ quadrant · saturate(n·l) · 2 · indirect_strength` over the four directions (`:282-296`).

### Fog

- **The HDR composite fogs every pixel at the depth of the opaque surface behind it**
  (`production_hdr_composite.frag.hlsl:40-71`, `:79-82`). The fog's opacity is `1 - exp(-((view depth - start) ·
  density)²)`, capped, plus the edge fog at the world's rim.
- **Transparent pixels write no depth,** so a puff over far ground is fogged as the far ground is.
- **Over the background** the composite adds no fog. The scene pass clears the background to the fog's colour instead,
  so that transparent pixels over it keep their colour (`btbh_gpu_scene.c:8313-8326`). A puff over the sky is not
  fogged at all.
- **Bloom** reads the image before the fog (`:9753-9790`).

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

- **Fifteen of SDL's 16 sampler slots** are taken. The shader says so (`production_model.frag.hlsl:88-90`), and the core
  makes the pipeline with 15, 14 or 10 samplers (`btbh_gpu_scene.c:3225-3230`).
- **The comparison binding (t8)** is bound for every batch (`:9451-9455`).
- **The fast unlit shader** (`production_model_unlit.frag`) is a separate file, loaded beside the host's fragment
  shader. It has its own sampler and uniform counts and its own index in the pipeline arrays (`:1197-1211`,
  `:2942-3006`, `:3158-3184`).

## Proposal

**Phase 1** brings items 1–3, and item 5 between batches:
- a new surface, `LIT_PARTICLE`, drawn by fragment shader files of its own;
- a batch flag for the UV rect, read in the static vertex shader;
- a post flag that draws the transparent batches far to near.

**Phase 2** brings item 4: a batch flag and a lighting byte for a transmittance map in sun space.

**Within one batch,** the instances draw in the order given. v2 sorts them on the CPU.

### Where this differs from the request

- **Its own shader, not a branch of `production_model.frag`.** The full scene shader has one sampler slot left
  ("The scene fragment shader's slots"). The particle needs three textures that shader doesn't bind: the distance in a
  slot of its own beside the normal, the cascades' atlas and, in phase 2, the transmittance map. A branch would also
  grow the shader every batch runs. Its own files keep both at today's cost and leave the last slot for phase 2's ground.
- **The distance's own binding comes with the new shader.** `SOFT_PARTICLE` keeps its slot swap, and it doesn't get
  the near fade. Its `p0.y` and `p0.z` were never read, and CHIMERA (`CONSUMERS.md:37`) and v2 draw it, so giving them
  a meaning there could change a look nobody asked to change.
- **Each light keeps the scale lit surfaces give it.** The sun, fill, spot and lamps go through
  `evaluate_directional_light`, which divides by π. The point lights don't (`evaluate_point_lights`). A puff then sits
  in the same light as the ground beside it. The phase is Henyey-Greenstein times 4π, which is 1 at every angle for
  g = 0, and it is tinted by the albedo (question 2).
- **The sun's shadow is always the four plain taps,** whatever `directional_shadow_filter` says, and with a constant
  bias instead of the receiver plane. A billboard's plane isn't the smoke's surface, and seen edge-on from the sun its
  gradient would push the bias anywhere. The fill and the spot take one gather each, the omni light one cube sample.
- **Fog.** The particle fogs itself at its own depth. It also writes the colour that comes out right after the
  composite fogs the pixel again at the depth behind it (below). Where that second fog is total, nothing in the
  transparent pass can show (question 6).
- **The transmittance map holds a slab per texel in one RGBA16F texture,** so that a receiver reads it with one
  sampler slot and one fetch. Puffs stacked along a sun ray are treated as one column (question 11).
- **Sorting.** Batches sort far to near in phase 1: the opaque sort's code does it, at the opaque sort's cost.
  Instances are left to v2 ("Sorting" below).

### API (phase 1)

```c
typedef enum BTBHGPUSceneSurfaceVariant {
    /* ... NONE (0) to REFRACT (10) as before ... */
    /* A see-through surface lit per pixel, for smoke, dust and steam (RFC 0013, since 0.11.0). Drawn in the transparent
     * pass with production_particle_lit.frag (or its _2d_shadows or _unshadowed sibling, by the frame's shadows), found
     * beside the host's fragment shader; a present without it fails, naming the file. Diffuse only, no specular.
     * Its light: the sky's ambient; the sun, its shadow through four plain comparison taps, and the clouds; the fill
     * and its shadow; the spot or omni light and its shadow; the street lamps; the point lights; with the radiance
     * cascades on, their light at its world xz; its emission (as a lit batch's). Then fog at its own view depth.
     * The normal: normal_texture, linear, on the albedo's texture coordinate. Its rg is a tangent-space normal (OpenGL
     * +Y, z rebuilt) whose tilt is scaled by normal_scale (0 flat); its b is the thickness, 0..1. The frame is the
     * vertex's: a billboard's is its mesh's tangent and normal through the instance's matrix, laid in the camera's
     * plane, so a quad in local x and y with normal +z and tangent +x (w +1) takes the instance's x and y axes. The
     * normal is turned toward the side seen. Without normal_texture the surface is flat and its thickness is 1.
     * surface_parameters0: x the soft fade (render units over which it fades where it meets the opaque scene behind
     *   it; 0 none); y, z the near fade (gone nearer the camera than y, whole from y + z on; z 0 none); w the wrap of
     *   its diffuse light (0 Lambert, 1 lit all the way round to the back).
     * surface_parameters1: x the transmission (light from behind, through it; 0 none); y the Henyey-Greenstein
     *   asymmetry, -1..1 (above 0 the light goes on forward); z the optical depth at thickness 1; w 0 (phase 2: the
     *   density it shadows with).
     * Needs transparency_enabled 1 with BTBH_GPU_SCENE_BLEND_ALPHA or _PREMULTIPLIED_ALPHA, unlit_enabled 0, a static
     * geometry, dug_by NULL, no world detail or table texture and detail_parameters 0; otherwise the present fails
     * with BTBH_GPU_INVALID_ARGUMENT, naming the batch. It casts no shadow and writes no motion. */
    BTBH_GPU_SCENE_SURFACE_LIT_PARTICLE = 11,
    BTBH_GPU_SCENE_SURFACE_COUNT = 12
} BTBHGPUSceneSurfaceVariant;

/* BTBHGPUSceneGeometryBatch.flags, after BTBH_GPU_SCENE_BATCH_NO_RC_SOURCE: */
    /* The instance's four custom floats are a rectangle of the texture (RFC 0013, since 0.11.0): the texture
     * coordinate, after uv_scale and uv_offset, is multiplied by custom x and y and offset by z and w, so one batch draws
     * every cell of an atlas, a cell per instance. The radiance cascades' source pass takes the rectangle too; shadows
     * sample no texture and don't change. Static batches only, and not with the surface variants that read custom
     * floats (DISSOLVE, SHATTER, GLITTER) or a world detail or table seeded by one: the present fails with
     * BTBH_GPU_INVALID_ARGUMENT, naming the batch. */
    BTBH_GPU_SCENE_BATCH_INSTANCE_UV_RECT = 1u << 3

/* BTBHGPUScenePost.flags, after BTBH_GPU_SCENE_POST_SSR_SURFACE_REFLECTIVITY: */
    /* The transparent pass draws its batches far to near (RFC 0013, since 0.11.0), by the key
     * BTBH_GPU_SCENE_OPAQUE_SORT_FRONT_TO_BACK uses (the view depth of the batch's mean instance centre). The sort runs
     * within each run between batches that keep their place: BTBH_GPU_SCENE_BATCH_KEEP_ORDER, persistent instance
     * buffers, bounds not known, a stencil in use. Equal depths keep the order given. Refracting batches keep their
     * own pass, in the order given. A batch's instances draw in their own order: sort them before the present. */
    BTBH_GPU_SCENE_POST_SORT_TRANSPARENT = 1u << 3

/* Defined when BTBH_GPU_SCENE_SURFACE_LIT_PARTICLE exists (since 0.11.0). */
#define BTBH_GPU_SCENE_HAS_LIT_PARTICLE 1
/* Defined when BTBH_GPU_SCENE_BATCH_INSTANCE_UV_RECT exists (since 0.11.0). */
#define BTBH_GPU_SCENE_HAS_INSTANCE_UV_RECT 1
/* Defined when BTBH_GPU_SCENE_POST_SORT_TRANSPARENT exists (since 0.11.0). */
#define BTBH_GPU_SCENE_HAS_TRANSPARENT_SORT 1
```

| Where | What | Offset (64-bit) | Was |
| --- | --- | --- | --- |
| `BTBHGPUSceneSurfaceVariant` | `LIT_PARTICLE` 11, `COUNT` 12 | – | `COUNT` 11 |
| `BTBHGPUSceneGeometryBatch.flags` (280 bytes) | bit 3, and bit 4 in phase 2 | 253 | free bits |
| `BTBHGPUScenePost.flags` (432 bytes) | bit 3 | 427 | a free bit (0.10.0 takes bit 2 for `SSR_FACING_HITS`) |
| `BTBHGPUSceneLighting` (3,340 bytes), phase 2 | `particle_shadow` | 3,337 | `directional_shadow_reserved[0]` (0.10.0 keeps it free; `spot_occluder_flags` is at 3,338) |

**Zero values.** A batch that doesn't use variant 11 and leaves bits 3 and 4 clear draws as before. So does a post
without bit 3, and (phase 2) a lighting whose `particle_shadow` is 0.

**Refusals** (`BTBH_GPU_INVALID_ARGUMENT`, naming the batch, as the other batch checks):
- `LIT_PARTICLE` with `transparency_enabled` 0, or a blend other than `ALPHA` and `PREMULTIPLIED_ALPHA`;
- `LIT_PARTICLE` with `unlit_enabled` set, a skinned geometry, `dug_by`, a world detail with a texture, a table with
  `map_layers`, or non-zero `detail_parameters`;
- `LIT_PARTICLE` with `surface_parameters1[3]` not 0 (phase 1 only);
- `INSTANCE_UV_RECT` on a skinned geometry, on `DISSOLVE`, `SHATTER` or `GLITTER`, or with a world detail or table
  whose `seed_component` isn't 0.

### The lit particle shader

`production_particle_lit.frag.hlsl` comes in three variants, as `production_model.frag` does: the full one (the sun's
map and the spot's, or the omni cube), `_2d_shadows` (no cube) and `_unshadowed` (no maps).

The vertex stage is the host's static vertex shader, with the UV rect below.

| Slot (full variant) | Texture | Sampler |
| --- | --- | --- |
| t0 | albedo | the texture's |
| t1 | normal (rg) and thickness (b) | the texture's |
| t2 | the scene's distance (R32F, loaded per pixel) | the scene's colour sampler |
| t3 | emission | the texture's |
| t4 | the sun's map | the comparison sampler |
| t5 | the fill's map | the shadow sampler |
| t6 | the spot's map | the shadow sampler |
| t7 | the omni light's cube | the shadow sampler |
| t8 | the cascades' quadrant atlas (a fallback never sampled when they are off) | linear, clamped |
| t9 | phase 2: the transmittance map (a fallback never sampled when off) | linear, clamped |

That is 9 samplers (10 in phase 2) of 16. The 2D-shadow variant drops the cube, the unshadowed one the four maps.

**Uniforms.**
- **b0:** `MaterialPBR`, declared exactly as in `production_model.frag`. The core pushes the same
  `BTBHGPUSceneFragmentUniforms`, filled once a pass.
- **b1:** a 112-byte `ParticleFrame`, the same for every particle batch in a pass, pushed once:
  - the cascades' field origin, cell size and texel count;
  - whether the cascades are on and additive, and their `indirect_intensity`;
  - the composite's four fog vectors (`BTBHGPUSceneCompositeUniforms`' `fog_color_density`, `fog_shape`,
    `fog_edge_bounds` and `fog_edge`).

  Phase 2 adds 16 bytes: whether to read the map, and its texel size. The map reuses the sun's matrix in b0.

**Alpha and fades.** The alpha is the albedo's, times the vertex's, the instance's and the material's.

```hlsl
const float d = length(input.world_position - camera_position.xyz);
if (surface_parameters0.x > 0.0f)   // the soft fade, as SOFT_PARTICLE's
    alpha *= saturate((scene_distance.Load(int3(input.pixel_position.xy, 0)) - d) / surface_parameters0.x);
if (surface_parameters0.z > 0.0f)   // the near fade
    alpha *= saturate((d - surface_parameters0.y) / surface_parameters0.z);
// PREMULTIPLIED_ALPHA: the fades scale the colour too.
```

**One light's share.** `l` points toward the light, `v` toward the camera, and `radiance` is the light's colour times
its energy, falloff and visibility.

```hlsl
float btbh_particle_wrap(float3 n, float3 l)                 // p0.w: 0 is Lambert
{
    return saturate((dot(n, l) + surface_parameters0.w) / (1.0f + surface_parameters0.w));
}

float btbh_particle_phase(float g, float cos_theta)          // Henyey-Greenstein times 4 pi: 1 everywhere at g = 0
{
    const float g2 = g * g;
    const float d = max(1.0f + g2 - 2.0f * g * cos_theta, 1.0e-4f);
    return (1.0f - g2) / (d * sqrt(d));
}

float3 btbh_particle_light(float3 albedo, float3 n, float3 v, float3 l, float thickness, float3 radiance)
{
    const float front = btbh_particle_wrap(n, l);
    // The light travels along -l; what reaches the eye has turned by acos(dot(-l, v)).
    const float back = surface_parameters1.x * exp(-thickness * surface_parameters1.z) *
        btbh_particle_phase(surface_parameters1.y, dot(-l, v));
    return albedo * (front + back) * radiance;
}
```

**The sum:**

| Term | How |
| --- | --- |
| Ambient | `albedo ·` the SH irradiance at `n` times the ambient energy with IBL, else the flat ambient (as `:2586-2614`) |
| Sun | `btbh_particle_light(...) / π`. The visibility is four `SampleCmpLevelZero` taps half a texel round the point, with a constant bias of a texel and a half, times `btbh_cloud_light` (skipped when the clouds are off) and, in phase 2, the transmittance |
| Fill | the same, its shadow one gather with bilinear weights |
| Spot | the same, its range and cone as `evaluate_spot_light`, its shadow one gather (2D) or one cube sample (omni) |
| Lamps | the same per lamp, each with `evaluate_lamps`' falloff (`fade² · core² / (d² + core²)`) |
| Point lights | `btbh_particle_light` without 1/π, each with `falloff²` (as `evaluate_point_lights`), then `ease_point_light` |
| Cascades | `albedo · 2 · indirect_strength · Σ quadrant(xz, k) · btbh_particle_wrap(n, l_k)` over the four directions `l_k` = `normalize(±1, 0.65, 0)`, `normalize(0, 0.65, ±1)`: the resolve's receiver term (`production_hrc_resolve.comp.hlsl:282-296`) with the wrap |
| Emission | as a lit batch's: texture times colour times energy |

**Fog.**
- **f_p** is the fog's opacity at the particle's own view depth and world xz, with the composite's formula, edge fog
  included.
- **f_b** is the composite's at the opaque surface behind it. Its view depth is the particle's times
  `scene distance / d`, because both lie on one pixel's ray, and its xz is `camera + (P - camera) · scene distance / d`.
  It is 0 over the background (distance 10⁶), where the composite adds none.
- **The colour written** is `c' = (c · (1 - f_p) + F · (f_p - f_b)) / (1 - f_b)`, with F the fog colour.
  Premultiplied, the F term is times alpha. The composite's `lerp(pixel, F, f_b)` then gives exactly the particle
  fogged at its own depth over the background fogged at its own.
- **Below zero.** `c'` can be below zero where a dark puff covers far, fogged ground. The target is RGBA16F, the
  composite brings the result back above zero, and bloom thresholds the intermediate image.
- **Where f_b is 1** (`fog_maximum_opacity` 1 over far ground, or the edge fog), the composite overwrites the pixel and
  nothing drawn in the transparent pass shows, as today. Fogging the opaque scene before the transparent pass would
  cure it, but that changes every transparent batch (Alternatives).
- With fog on, the distance pass runs whenever a lit particle draws, soft fade or not.

**Its outputs.** The colour with its alpha, and the reactive mask under FSR 3 as every transparent batch writes it
(`btbh_temporal_reactive`, `production_model.frag.hlsl:2929-2940`).

### The UV rect

In `production_model.vert.hlsl`, after line 275:

```hlsl
if (instance_uv_rect != 0u)   // BTBH_GPU_SCENE_BATCH_INSTANCE_UV_RECT (RFC 0013)
{
    output.texture_coordinate = output.texture_coordinate * instance.custom.xy + instance.custom.zw;
}
```

- **The uniform** is a named word, `instance_uv_rect`, the first of the vertex block's three `temporal_reserved` words
  (`BTBH_TEMPORAL_UNIFORMS`, `production_temporal.hlsli:13-17`; `BTBHGPUSceneUniforms`, `btbh_gpu_scene.c:91-92`). The
  block keeps its size. `surface_controls.y` is free too, but a lane has no name, and the old-shader check below needs
  one. The main pass (beside `btbh_gpu_scene.c:9227`) and the cascades' source pass (beside `:6751`) set it to 1 for a
  flagged batch.
- **Every shader that includes the file takes it:** `production_rc_material_source.vert` and
  `production_model_detail_table.vert`. The skinned vertex shader doesn't need it, since the flag is refused on skinned
  geometry.
- **Unflagged batches** skip the branch, so their texture coordinate is bit for bit what it was.
- **A rect batch draws all its cells in one draw.** The cells no longer differ in `uv_offset`, which is in the run key.
- **Mip bleed** at a cell's edge is the atlas's to pad, as for any atlas.

### Sorting

- **Between batches** (the post flag), `btbh_gpu_scene_opaque_order` (`btbh_gpu_scene.c:5153-5214`) gains two
  parameters: which pass it orders, and the direction.
  - For the transparent pass it keeps the transparent, non-refracting batches and inverts the key's bits, so the
    farthest batch draws first.
  - The rules for a movable batch are the opaque pass's (`:5024-5035`), less the depth compare: a batch writes no depth
    in the transparent pass. KEEP_ORDER, persistent instances, unknown bounds and a stencil pin a batch.
  - The phase loop walks that order in the transparent pass as it does in the opaque one (`:9142-9153`). The geometry
    bases it needs come from the same walk that fills them for the opaque order (`:7227-7230`), which then runs
    whenever either order is used (today only with the opaque sort, `:7905`).
- **Within a batch, the renderer does nothing.** A draw's instances blend in instance order, which is the graphics
  APIs' primitive order. v2 builds its transient instances on the CPU each frame and knows its emitters, so it can sort
  the instances it writes. A sort in the renderer would cost about what v2's does, and would have to:
  - copy each sorted range, since batches may share a transient range;
  - permute `temporal_instance_motions`, which follow the transient instances' order (`btbh_gpu.h:1310-1316`);
  - leave persistent buffers alone.

  A GPU sort (bitonic, in compute) would also need a sorted index buffer read through `SV_InstanceID` by every vertex
  shader that draws the batch. None of that is worth it for particles the host already holds. Question 8 asks whether
  v2's own order is enough.
- **With the UV rect,** v2's smoke can be one batch, so the batch sort matters only between smoke, fire and sparks.
  Puffs of two different batches can't interleave in any case.

### Phase 2: the transmittance map

```c
/* BTBHGPUSceneGeometryBatch.flags, after BTBH_GPU_SCENE_BATCH_INSTANCE_UV_RECT: */
    /* The batch shadows the sun's light in the particle transmittance map (RFC 0013 phase 2), when
     * BTBHGPUSceneLighting.particle_shadow is set and the sun's shadow is on. Its fragments' coverage (alpha, times
     * surface_parameters1.w for LIT_PARTICLE, 0 meaning 1) becomes optical depth. Transparent batches of surface
     * variant NONE, SOFT_PARTICLE or LIT_PARTICLE only: the present fails with BTBH_GPU_INVALID_ARGUMENT, naming the
     * batch, on any other. */
    BTBH_GPU_SCENE_BATCH_PARTICLE_SHADOW = 1u << 4

/* BTBHGPUSceneLighting, in the byte that was directional_shadow_reserved[0]: */
    /* The particle transmittance map (RFC 0013 phase 2): 0 none, as before; 1 lit particles read it; 2 so do the
     * opaque surfaces the sun shadows (lit, and unlit modes 2 and 3). Drawn only on frames whose sun shadow is on and
     * that draw a batch with BTBH_GPU_SCENE_BATCH_PARTICLE_SHADOW. Other values fail the present. */
    uint8_t particle_shadow;
    uint8_t directional_shadow_reserved[2];

/* Defined when BTBH_GPU_SCENE_BATCH_PARTICLE_SHADOW and BTBHGPUSceneLighting.particle_shadow exist. */
#define BTBH_GPU_SCENE_HAS_PARTICLE_SHADOW 1
```

**The map.**
- **Format and size.** One RGBA16F texture at half the sun map's size each way: 1024 × 1024, 8 MiB, under the default
  2048 map (`btbh_gpu_scene.c:26`).
- **What it holds.** Cleared to (0, 0, 0, 1). r is the optical depth τ, summed; g is τ times the sun-space depth d,
  summed; a is the nearest d. b is unused.
- **Blending.** One blend state does it, because SDL blends colour and alpha with separate operations (the reactive
  mask already sets both, `:1565-1577`): colour `ADD` (one, one), alpha `MIN`. It is order-independent.
- **When it is drawn.** Every frame, after the shadow maps, with the matrix the receivers read: the retained one when
  the sun's map is kept over several frames (`:9053-9056`).
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
under the column takes all of it.

**Who reads it.**
- **Lit particles,** when `particle_shadow` ≥ 1, multiply the sun's share by T.
- **Opaque surfaces,** with mode 2 (phase 2b, question 10). The scene fragment shader's next free slot takes the map
  (t15 in the full variant, its 16th and last; t14 in `_2d_shadows`; a 1×1 fallback when off), and T multiplies `sun_visibility` in the lit path (`production_model.frag.hlsl:2642-2669`) and
  the visibility of unlit modes 2 and 3 (`:2391-2404`). That is every shadowed variant: `production_model.frag`,
  `_2d_shadows`, `_rc`, `_rc_2d_shadows`.

## Minimal example

```c
/* v2's smoke: every puff of every emitter in one batch, its instances sorted far to near on the CPU. */
BTBHGPUSceneGeometryBatch smoke = {0};
smoke.kind = BTBH_GPU_SCENE_GEOMETRY_CUSTOM_MESH;
smoke.custom_mesh = puff_quad;           /* local xy, normal +z, tangent +x (w +1), uv 0..1 */
smoke.material_slot = -1;
smoke.first_instance = first_puff;       /* each puff's custom = { cell width, cell height, cell u, cell v } */
smoke.instance_count = puff_count;
smoke.albedo_texture = smoke_atlas;      /* sRGB colour, alpha the coverage */
smoke.normal_texture = smoke_normals;    /* linear: rg the normal, b the thickness; the same cells */
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
smoke.surface_parameters1[2] = 3.0f;     /* optical depth at thickness 1 */
smoke.flags = BTBH_GPU_SCENE_BATCH_INSTANCE_UV_RECT;

/* Fire, sparks and the smoke batch sorted among themselves: */
post.flags |= BTBH_GPU_SCENE_POST_SORT_TRANSPARENT;

/* Phase 2: the smoke darkens itself and the street. */
smoke.flags |= BTBH_GPU_SCENE_BATCH_PARTICLE_SHADOW;
lighting.particle_shadow = 2;
```

In v2's Rust: `batch_of` ends in `..Default::default()`, so the flags and parameters it doesn't name stay 0. The
smoke's builder sets the variant, the two parameter arrays and the flag, and writes each puff's cell into its custom
floats.

## Implementation

### Files

| File | Phase | Change |
| --- | --- | --- |
| `include/btbh_gpu/btbh_gpu.h` | 1 | The variant and its comment (and one for `SOFT_PARTICLE`, which has none: `p0.x` the fade), the batch flag, the post flag, three macros |
| `include/btbh_gpu/btbh_gpu.h` | 2 | The second batch flag, `particle_shadow`, the macro |
| `shaders/gpu/production_particle_lit.frag.hlsl` | 1 | New: the lit particle (full variant) |
| `shaders/gpu/production_particle_lit_2d_shadows.frag.hlsl`, `..._unshadowed.frag.hlsl` | 1 | New: two-line wrappers with the shadow defines, as `production_model_2d_shadows.frag.hlsl` |
| `shaders/gpu/production_model.vert.hlsl` | 1 | The UV rect, behind `instance_uv_rect` |
| `shaders/gpu/production_temporal.hlsli` | 1 | `instance_uv_rect` named in `BTBH_TEMPORAL_UNIFORMS`, from the first `temporal_reserved` word (the skinned vertex shader shares the macro and doesn't read it) |
| `shaders/gpu/production_particle_transmittance.vert.hlsl`, `.frag.hlsl` | 2 | New: the map's pass |
| `shaders/gpu/production_particle_lit*.frag.hlsl` | 2 | Read the map (t9) |
| `shaders/gpu/production_model.frag.hlsl` | 2b | The map in the next free slot (t15 in the full variant, t14 in `_2d_shadows`) and T in the sun's visibility, behind `particle_shadow` 2. Its including files change with it; the unshadowed ones declare no map |
| `src/gpu/btbh_gpu_particle.h` | 1, 2 | New: C copies of the wrap, the phase, the fog's make-up and the map's reading, for the native tests (as `btbh_gpu_world_detail.h` was for 0.6.0) |
| `src/gpu/btbh_gpu_scene.c` | 1, 2 | Below |
| `CMakeLists.txt` | 1, 2 | The new files after their bases in `BTBH_GPU_SHADER_SPECS` |

Unchanged:
- `production_model.frag`, in phase 1. `SOFT_PARTICLE`, unlit mode 3 and every other surface draw as before.
- The shadow shaders, the cascades' and the post chain's shaders. `production_animated_model.vert` changes only by the
  renamed reserved word it shares through `BTBH_TEMPORAL_UNIFORMS`.

### The core

- **Checks.** The refusals above, beside the variant and flag checks (`btbh_gpu_scene.c:4676-4705`).
- **Pipelines.**
  - The pipeline arrays' last index (`BTBH_GPU_SCENE_UNLIT_VARIANT_COUNT`, today 0 full and 1 fast unlit,
    `btbh_gpu_scene.c:31`, `:253-339`) gains a third value: lit particle.
  - `btbh_gpu_scene_ensure_model_pipeline` picks the particle file by the frame's shadows, as it picks the fast unlit
    one (`:3158-3184`). It makes the pipeline with 9 samplers (10 in phase 2) and two uniform buffers.
  - They are made on first use, and `btbh_gpu_scene_ensure_model_pipelines` makes them for prewarm (`:7203-7291`).
- **Bindings in the transparent pass** (`:9394-9520`). For a lit particle:
  - the samplers above;
  - `hrc_quadrant_textures[hrc_history_read_index]`, with the history's origin and cell size, when the cascades ran
    this frame;
  - the 112-byte block in slot 1, pushed once a pass (as the world-detail table's, `:8758-8761`).
- **The distance pass** runs when a visible lit particle has a soft fade, or fog is on (`btbh_gpu_scene_batches_have_soft_particles`,
  `:2540-2555`; `:7462-7476`). So does prewarm, which makes "the distance pass ... where a batch needs them"
  (`btbh_gpu.h:1940-1941`).
- **The UV rect:** `instance_uv_rect` in both passes that fill the vertex uniforms, and the SPIR-V name check.
- **The sort:** above. Phase 1 only.
- **Phase 2:**
  - the map (create, release, resize with the sun's map);
  - its pipeline (cull none, the RGBA16F target with the blend above);
  - its pass after the shadow maps;
  - the binding in the particle and, with mode 2, the scene shaders;
  - the same in `btbh_gpu_scene_prewarm_internal` (`:10525`).
- **Missing files.** A present with a lit particle (or, in phase 2, a flagged batch) and a shader directory without the
  new files fails with the file's path, as 0.8.0 does for the table's vertex shader (`:3185-3187`). Older shaders draw
  everything else as before.

### Cost

Estimates, to measure with a smoke mode in the frame bench (`btbh_gpu_frame_bench`): N puffs at overdraw K, lit
against unlit mode 3 with a soft fade.

| Item | Estimate |
| --- | --- |
| Off (no lit particle, bits clear) | One uniform test per static vertex for the UV rect, branch not taken. If the bench shows it, the rect moves to a vertex shader of its own (`production_model_uv_rect.vert`), as 0.8.0 moved the table's index |
| A soft particle in unlit mode 3 today | 5 texture reads (albedo, emission, the distance in the normal slot, ORM, the distance again) plus 4 comparison taps under the soft filter, or 16 gathers under the tent |
| A lit particle | 14 reads (13 without an emission, which is skipped when its energy is 0): albedo, normal, distance, emission, 4 comparison taps, the fill's gather, the spot's, 4 from the cascades' atlas. About 150 ALU, plus about 20 for each lamp or point light in reach and 6 for each out of reach |
| So | About as many reads as today's smoke (fewer under the tent filter); more ALU, most of it in the lamp loop at night |
| Example: 1280 × 800, a quarter of the screen 6 puffs deep (1.5 million fragments), 10 of 40 lamps in reach | About 20 million reads and 0.8 billion ALU operations: a few tenths of a millisecond more than today's smoke on a Deck, well under 0.1 ms on the bench's RTX 3070 Ti. Blending 6 layers of RGBA16F costs the same either way |
| Memory, phase 1 | The distance target (R32F at the render size), made already when soft particles draw |
| CPU, phase 1 | Per lit particle batch: 9 samplers; the 112-byte block once a pass. The sort: one pass over the transparent batches' instances and a sort, as the opaque sort's |
| Phase 2 pass | The flagged puffs again, at half the sun map's resolution, one read and about 10 ALU per fragment |
| Phase 2 reads | One bilinear RGBA16F read and about 10 ALU per lit particle fragment and, with mode 2, per sun-lit opaque pixel |
| Phase 2 memory | 8 MiB under a 2048 sun map |
| Phase 2 off | Nothing in the particle shader but a uniform test. With 2b built, every scene draw binds a 16th sampler (a 1×1 fallback) and each lit pixel tests a uniform |

If the lamp loop dominates on the Deck (question 4), the fallback is lighting the lamps and point lights per vertex in a
particle vertex shader of its own, with the sun, its shadow and the normal map kept per pixel.

## Interactions

| With | What happens |
| --- | --- |
| Radiance cascades, as a source | A lit particle is a source only through its emission, as any lit batch (`btbh_gpu_scene.c:6688-6695`). Without an emission it is none. With one (embers), the source pass draws its emission times alpha, without the fades (they need the camera's distance); `NO_RC_SOURCE` keeps it out |
| Radiance cascades, as a receiver | It reads the field at its xz from four directions, as an unlit receiver does. The field is at street level, so a puff high above a street takes the street's light. Under replacing (non-additive) cascades it keeps the scene's ambient and adds the bounce; v2's are additive (RFC 0010) |
| RFC 0010's source tops | An emissive lit particle writes its fragment's height as its top, as every material source does. A non-emissive one writes nothing. Its own reading is at its xz, not its height: with tops, a texel under a roof holds what pieces starting on the roof gather |
| SSR | Computed in the SSAO resolve, before the transparent pass (`:8559-8663`): wet streets don't reflect the puffs, as with every transparent batch |
| Refraction | Refracting batches draw after, over a copy that includes the puffs (`:8699-8722`). A refracting batch behind a puff draws over it, showing it displaced, as for every transparent batch |
| HBAO, SSAO | Before the transparent pass: puffs are neither occluded nor occluding |
| FSR 3 | The reactive mask as every transparent batch, `reactive_mode` included; no motion vectors |
| Render to target | It has its own runtime (`btbh_gpu.h:2005-2006`): its own distance target, its cascades and, in phase 2, its own map, made by `btbh_gpu_window_scene_prewarm_render_to_target`. Never temporal, so no reactive mask |
| Prewarm | The particle pipelines join through `btbh_gpu_scene_ensure_model_pipelines`; the distance pass and (phase 2) the map and its pipeline through the prewarm's own steps, when a batch needs them |
| RFC 0011 runs | Lit particle batches run as any batch. The sort can split runs that the given order kept together |
| RFC 0012 (draft) | A lift refuses billboards, so a lit particle billboard can't be lifted |
| World curvature | The vertex shader bends the puffs as everything else. The light, the fog's xz and the map read the flat world position |
| Opaque sort | Unchanged |

## Compatibility

- **Versioning:** minor (0.11.0 for phase 1). An enum value, free bits of two flag bytes and, in phase 2, a reserved
  byte, as 0.5.0 took `ssao_reserved[0]` for the batch's flags and 0.9.0 took bits of them. No struct grows.
  `BTBH_GPU_SCENE_SURFACE_COUNT` goes from 11 to 12, as it went from 10 to 11 with RFC 0005. A host that used it as a
  past-the-end value is refused at 12 as it was at 11.
- **Shaders.**
  - Phase 1 changes `production_model.vert` (the rect) and a name in `production_temporal.hlsli`; it adds three
    fragment shader files. Every unflagged batch draws as before, to the byte.
  - 0.10.0's shaders under the new core draw everything but lit particles and rect batches as before.
  - A lit particle fails naming its missing file. A rect batch drawn with 0.10.0's vertex shader would show the whole
    atlas, so a present with the flag checks the vertex shader's SPIR-V for the rect, as the core checks the scene
    shader's names for the dig mask and the world detail (`btbh_gpu_scene_check_scene_shader`,
    `btbh_gpu_scene.c:6007-6050`), and fails with its path. The name it looks for is `instance_uv_rect` ("The UV
    rect"). Metal libraries aren't checked, as before.
- **Be The Bullet Hell v2 (Rust, bindgen):** nothing changes until it opts in. bindgen gives the new enum value and
  flags; `batch_of` already ends in `..Default::default()`. To adopt:
  - bump the pin and build the three particle shaders;
  - bake the normal and thickness atlas;
  - replace the five tilts and the sprite turning with the variant;
  - merge its smoke draws with the rect;
  - sort its puffs.
- **Be The Bullet Hell (v1, Swift):** nothing changes. Its `softParticle` case (`Sources/BTBHRendererGPU/GPUResourceBackend.swift:38`,
  mapped at `BTBHRendererGPU.swift:3439-3440`) keeps its value; a `litParticle` case can follow when it asks.
- **CHIMERA (Rust, bindgen):** nothing changes. `SOFT_PARTICLE` is untouched and its list of 14 shaders can stay.
- **Tyrian3D (C):** nothing changes. It zeroes its batches and is pinned at v0.2.1.

## Platforms and testing

- **Native:**
  - `btbh_gpu_scene_order_test`:
    - `LIT_PARTICLE == 11`, `COUNT == 12`, the flag bits 8 and 16 and the post bit 4;
    - in phase 2, `offsetof(BTBHGPUSceneLighting, particle_shadow) == 3337` and the size 3,340;
    - the refusals.
  - `btbh_gpu_particle_test` (new, on `btbh_gpu_particle.h`):
    - the phase is 1 at g = 0 and integrates to 4π for any g;
    - the wrap is Lambert at 0;
    - the fog's make-up gives the reference for random `f_p ≤ f_b < 1` and the three blends;
    - the map's reading: 1 before the front, `exp(-τ)` past the back, continuous between.
- **GPU smoke** (Windows/Vulkan, `btbh_gpu_particle_smoke`, new):
  - **Off, byte for byte:** the galleries and suites with 0.10.0's core and shaders, no lit particle and the bits clear.
  - **A puff against a CPU reference** at chosen pixels: the sun alone, wrap 0 and 0.5, transmission at g = 0 and 0.6
    looking toward the sun.
  - **The sun's shadow:** a puff half in a wall's shadow, under both filters (the same four taps).
  - **Lamps, point lights, the spot:** a puff between two lamps at night.
  - **The cascades:** a glow under a puff lights its underside (additive).
  - **Fades:** the soft fade against `SOFT_PARTICLE`'s at the same `p0.x` (the same alpha), the near fade at the camera.
  - **Fog:** a puff over the fogged background, and over near and far ground, against the formula.
  - **The rect:** one batch of 16 atlas cells against 16 batches with `uv_offset`, byte for byte.
  - **The sort:** two overlapping batches given far-last come out far-first; KEEP_ORDER holds one.
  - **Prewarm:** after prewarm a lit particle's first frame makes no pipeline (`btbh_gpu_scene_prewarm_smoke`).
  - **Old shaders:** a lit particle with 0.10.0's shader directory fails with `production_particle_lit.frag`'s path; a
    rect batch fails with the vertex shader's.
  - **Phase 2:** a column's lee side darker than its sunny side; its shadow on the ground (mode 2); a receiver above the
    column untouched.
- **Bench:** the smoke mode above, lit against unlit mode 3 at Deck and desktop sizes; the kit pieces with 0.11.0's
  vertex shader against 0.10.0's (the rect's off cost).
- **Platforms:** Windows/Vulkan runs the smokes. The Mac session runs `check.sh` (the new files through shadercross to
  MSL). Linux and the Deck through v2.
- **Games:** v2's smoke captures by day, at dusk and at night, with its fake lighting and with the variant; its frame
  times on the Deck preset.

## Alternatives

| Alternative | Why not (now) |
| --- | --- |
| **A branch of `production_model.frag`** (variant 11 in the shared shader) | One sampler slot left; the particle needs three more textures. Every batch would carry the longer shader |
| **Keep the five baked tilts** | v2's current smoke; no lamps, point lights, cascades or self-shadow, and a turn per sprite |
| **Six-way lightmaps** (light from six directions baked from a simulation, two RGBA textures) | Richer for simulated smoke at the same cost class, but v2 bakes a normal and a thickness. A later variant if v2 bakes from simulations |
| **All light per vertex** | Cheaper, but loses the normal map's detail. Kept as the fallback for lamps and point lights only (Cost) |
| **Fog before the transparent pass** (a post flag: the composite's fog moves between the opaque and transparent passes, and transparent shaders fog themselves) | Correct where the composite's fog is total, but changes every transparent batch's look under that flag and needs every transparent shader to fog itself. Later, if v2's fog needs it (question 6) |
| **Deep or Fourier opacity maps** (layers of opacity along the sun's ray) | They separate puffs stacked along a ray, but need two textures (or a second pass). The scene shader has one slot for the ground. Phase 2's slab first (question 11) |
| **Puffs in the sun's depth map** (alpha-tested) | A hard, binary shadow, and the map is retained over frames while puffs move |
| **Sorting instances on the GPU** | A compute sort, a sorted index read by every vertex shader of the batch and a permuted motion buffer, to redo what v2 does on the CPU with the positions in hand |
| **Sorting instances on the CPU in the renderer** | The same work as v2's, plus a copy per batch (ranges may be shared) and a permutation of the motions |
| **Order-independent transparency** (weighted, blended) | It would remove sorting, but changes the transparent pass's targets for every batch |

## Open questions for v2

1. **The normal map.** Linear, OpenGL +Y (green up), xy from `rg · 2 - 1` and z rebuilt, the tilt scaled by
   `normal_scale` (0 flat)? Thickness in b, linear 0..1, made optical depth by `p1.z`: is that the range you bake, or do
   you need more than 1? Is it a texture of its own on the albedo's cells?
2. **The light model.**
   - The wrap as `(n·l + w) / (1 + w)`?
   - The transmission as `albedo / π · p1.x · exp(-b · p1.z) · 4π·HG(g, cos θ)`, so that it is tinted by the albedo
     and p1.x = 1 at g = 0 matches the front's peak?
   - Every light at the scale lit surfaces give it (1/π for the sun, fill, spot and lamps; none for point lights)?
3. **The phase for which lights.** The proposal gives every light both terms: the sun, the fill, the spot, the lamps
   and the point lights, so a fire glows through the smoke in front of it. Or the sun only? That saves about 10 ALU per
   lamp and point light in reach.
4. **Counts.**
   - How many puffs on screen, on the Deck preset and on desktop, and how large?
   - How many layers deep at the thickest, and over what share of the screen?
   - At night, how many lamps and point lights in reach of a column?

   This decides whether lamps and point lights need the per-vertex fallback.
5. **Blend.** Alpha or premultiplied for the lit smoke? With premultiplied, the fades scale the colour.
6. **Fog.**
   - Your fog's density, start, maximum opacity and edge fog by stage?
   - The particle's make-up is exact up to where the composite's fog is total. Is that enough, or do you need the
     post flag that fogs before the transparent pass, which changes all your transparent batches?
7. **The cascades.**
   - Should puffs take the field's light at their xz (street level, four directions), so that night glows light the
     smoke from below?
   - Should emissive puffs (embers) stay sources, or will you set `NO_RC_SOURCE`?
8. **Sorting.** Is your own instance order enough, the renderer drawing a batch's instances in that order? With all
   smoke in one rect batch, do you need the batch sort for smoke against fire and sparks?
9. **The rect.** Beyond the lit smoke, which batches would use it (fire, sparks, unlit dust)? Do any of them need their
   custom floats for something else (a dissolve, a world detail's seed), which the flag refuses?
10. **The ground.** In phase 2, do you need the street to take the smoke's shadow (mode 2)? That takes the scene
    shader's last sampler slot and a read per sun-lit pixel. Or is the lee-side darkening on the puffs enough?
11. **Stacked puffs.** Each map texel holds one column, from its nearest puff to its farthest along the sun's ray. Two
    puffs at different heights on one ray darken the gap between them. Acceptable for your columns, or do you need
    layers (a second texture and pass)?
12. **The sun's map.** What size and update period do your presets use? The transmittance map is half the sun map's
    size and drawn every frame. Is half enough for a column's shadow on the street?
13. **Release.** Phase 1 (the variant, the rect, the batch sort) in 0.11.0, and phase 2 once your phase 1 captures are
    in?

## Phases

1. **Phase 1 (0.11.0): items 1, 2 and 3, and item 5 between batches.**
   - 1a: `LIT_PARTICLE` and its three fragment shader files. The ambient, the sun with four plain taps and the clouds,
     the fill, the spot and omni light, lamps, point lights, the cascades, emission and fog with its make-up.
   - 1b: the soft and near fades, with the distance in its own slot. Part of 1a's shader; `SOFT_PARTICLE` unchanged.
   - 1c: `BTBH_GPU_SCENE_BATCH_INSTANCE_UV_RECT` in `production_model.vert`, behind the named `instance_uv_rect` word.
   - 1d: `BTBH_GPU_SCENE_POST_SORT_TRANSPARENT` (no shader), if v2 wants it (question 8).

   Plus the checks, the C copies, the tests, the bench's smoke mode, prewarm and render to target.
2. **Phase 2 (0.12.0 or later, after phase 1's captures): item 4.**
   - 2a: `BTBH_GPU_SCENE_BATCH_PARTICLE_SHADOW`, `particle_shadow` 1, the RGBA16F map, its two shader files, the read in
     the particle shader.
   - 2b, if v2 answers yes to question 10: `particle_shadow` 2, the map in the scene shader's last free slot.
3. **Not planned:** sorting instances in the renderer, on the GPU or the CPU (item 5 within a batch is v2's).
4. **Later, only if asked:**
   - fog before the transparent pass (question 6);
   - lamps and point lights per vertex (question 4);
   - layers in the map (question 11);
   - six-way lightmaps;
   - particles in SSR.
