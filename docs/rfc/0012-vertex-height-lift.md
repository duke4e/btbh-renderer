# RFC 0012: A vertex-stage height lift from a heightfield

- **Status:** Accepted on 2026-11-23 (the owner left the decision to the coordinator). Released in v0.11.0:
  - phase 1 and the extra (queued custom-mesh updates): see "Implementation notes (phase 1 and the extra)";
  - queued instance buffer uploads and partial mesh updates (v2's night perf round): see "Queued instance uploads and
    partial updates".

  Phase 2 only if asked. See "v2's answers"
- **Requested by:** Be The Bullet Hell v2 (its session, 2026-11-22, after the owner's sixth play-test)
- **Affects:** Be The Bullet Hell v2 (adopts it). Be The Bullet Hell (v1, Swift), CHIMERA and Tyrian3D: nothing, it is
  opt-in and their batches keep today's vertex shaders. They rebuild the shaders with the C core, as every release
- **Issue:** none. v2 answered all seven questions through its session on 2026-11-23 (see "v2's answers")
- **Depends on:** nothing. On 0.10.0's layout (RFC 0011 phase 1 changed no struct) the batch is 280 bytes with
  `world_detail_table` at 272; this RFC appends `height_lift` at 280, so the batch is 288 bytes. RFC 0011's phase 2
  `draw_list`, if it comes, goes after it at 288. It follows 0.8.0's pattern of a separate static vertex shader
  (`production_model_detail_table.vert`) for the batches that need more

## Need

v2's request, verbatim:

> "v2's 'Height terrain' experiment lifts kit pieces per cell, so street tiles step in terraces on slopes. A
> vertex-stage lift from a heightfield texture would let them follow the slope smoothly."

The experiment, from v2's code (paths under `Be-The-Bullet-Hell/v2/crates/`, BTBH `dec1a3b7`; the commit that made it
is `361be68d`):

- **Off by default** (the Experiments tab). Five cells of relief plus a two-cell tilt, the river and the moat in
  valleys. Each building stands on its lot's mean ground with foundations; units, shots, props and the camera stand
  on the ground. The simulation stays flat: it is visual only.
- **One heightfield for everything on the ground.** `v2_core::Heightfield` (`v2-core/src/height.rs`) holds samples on
  a lattice at `(x0 + i·step, y0 + j·step)`, row by row along x, read bilinearly and clamped at the edges
  (`Heightfield::at`). The terrain's meshes, the outskirts' blocks and, under the experiment, the city read it.
- **Kit pieces are lifted whole.** Each piece's instance takes its cell's height, so a street steps once a cell.
- **The cost** (`361be68d`): Height terrain on, the Deck preset at the map's middle, 6.5 → 7.9–8.5 ms a frame.

The 0.8.0 constraint, from its notes: one more vertex attribute read in the static vertex shader cost about 2 % of
a frame of many small draws, table or not, until it moved to a vertex shader of its own. A lift must not put cost on
batches that don't use it.

## v2's answers

Relayed by the v2 session (from its terrain agent) on 2026-11-23. Since the request v2 also conforms ground marks on
the CPU (small marks tilted onto the local plane, large ones laid as meshes split to edges of at most 0.6 cells), so
a GPU lift would mainly help the street tiles and remove those uploads.

1. **Which batches.** Ground pieces only: the kit files under `road/`, `plaza/`, `water/`, `bridge/`, `rubble/` and
   `shared/marking` (`KitMesh::ground`), the baked water chunks and the outskirts ring's ground. Parks are 1-storey
   building lots in v2's kit and are lifted rigidly per lot with buildings, props and units. Overlays should take the
   same lift: night wash 0.0585, scars 0.057, telegraphs 0.07, hazards 0.085, rings 0.09, weapons 0.095, aims 0.1 over
   the ground. v2 would subdivide overlays first (a telegraph can be one quad over about 10 cells).
2. **The field.** One field for the map, the ring and the hills, step 1 cell, samples on cell corners from -170 to
   map + 170: 469 × 469 at map size 128, 565 × 565 for the 224-cell one-big-level map, so 4096 a side is ample.
   Heights run from -0.34 to 15.4 over the three cities. Half floats are fine: ground and marks read the same texel at
   the same xz.
3. **Geometry.** Pieces have vertices at cell and quarter corners; shared edges are axis-aligned, so they stay linear
   under bilinear filtering (45° kerbs aren't). Overlays aren't triangulated like the ground under them. Measured twist
   per cell, (h00 + h11 − h01 − h10) / 4: median 0.0007, p99 0.007–0.009, max 0.07–0.11 at crests. Steepest step: 0.59
   per cell.
4. **KEEP_NORMALS.** Useful for the overlays, all unlit, alpha or additive. The street tiles want the tilt.
5. **Shadows.** Ground pieces, the terrain mesh and overlays cast none (`casts_shadow` false, which v2 maps to
   `BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW`, or transparent). No lifted shadow shader is needed.
6. **Changes during a stage.** None: the field is built once a stage, scars are decals, rubble keeps its building's
   pad. v2 bakes the lot pads into the field it hands over.
7. **Occluder tops.** Already handled: `CityLayer::top_at` returns floors × storey over the local ground, with no lift.

Extra request (small, optional): let `btbh_gpu_custom_mesh_update` queue into the frame instead of submitting per call,
or offer a transient vertex stream; each upload costs about 0.17 ms whatever its size, which is what makes moving
conformed marks expensive. See "Extra: custom-mesh updates queued into the frame".

### What the answers change

- **Batches.** Static ground pieces and overlays. The overlays are transparent and unlit (alpha or additive) with
  `KEEP_NORMALS`; so the lift must work in every pass a batch can be drawn in, not only the opaque one. It does: every
  geometry pass (opaque, transparent, unlit, additive, soft particles, refraction) draws a static batch with
  `production_model.vert` (the fast unlit pipeline changes only the fragment shader), so the lifted vertex shaders
  cover them all through pipelines of their own (see "The core"). Only the radiance cascades' material sources draw
  with another vertex shader (`production_rc_material_source.vert`): they project straight down, so they take the
  lifted batch unlifted, at the same xz.
- **Field.** RGBA16F as drafted. In 8..16 units a half steps by 1/128, so v2's heights land within 1/256 of their
  value on the GPU; its units and props read the same field on the CPU with `Heightfield::at` and agree within that.
- **Shadows.** Dropped from phase 1 (decision below): no `production_shadow_lift.vert`.
- **Changes.** No update call (phase 2 stays "only if asked").
- **Occluder tops.** Nothing to do for the radiance cascades.

### The shadow decision

`production_shadow_lift.vert` would be one define, but it isn't cheap to carry: lifted shadow pipelines (per cull mode,
with and without discard), the heightfield's binding and block in the directional, spot and six omni passes, their
own runs, prewarm, and a test per map. v2's lifted batches cast none. So in phase 1 **a lifted batch casts no shadow**:
a batch whose lift has a heightfield must be transparent (`transparency_enabled`), or set
`BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW` (or have its `directional_shadow_visibility` byte 0) and leave
`spot_shadow_visible` 0. Otherwise the present fails with `BTBH_GPU_INVALID_ARGUMENT`, naming the batch: "GPU scene
batch N: height_lift: a lifted batch casts no shadow until phase 2: set BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW and leave
spot_shadow_visible 0, or draw it transparent". The rule reads the batch's fields, not whether the present draws shadow
maps, so a batch is refused the same way with the shadows on and off. Lifted batches still take shadows: their
fragments see the lifted world position.

## What the code allows

### Where a vertex is placed

`production_model.vert.hlsl` (static) and `production_animated_model.vert.hlsl` (skinned):

1. The surface variant moves the vertex (dissolve, shatter) in object space.
2. The instance's matrix takes it to world space; the wind sways it.
3. The normal and tangent go through the matrix's cofactor.
4. `world_position` (flat) goes to the fragment shader, which lights, details and digs with it.
5. `btbh_world_bend` bends it for the screen (`world_curvature`); the previous frame's position is worked out the
   same way for FSR 3's motion.

A lift belongs between 2 and 3's normalisation: after the instance and the wind, before the normal's frame, the flat
world position, the bend and the motion.

### Shadows

Three vertex shaders draw the shadow maps (directional, spot, omni):

| Shader | For | Instances read |
| --- | --- | --- |
| `production_shadow_fast.vert` | static batches without vertex motion, billboards or a dig mask | as a vertex stream, position only |
| `production_shadow.vert` | the other static batches | the storage buffer, with the surface variants and billboards |
| `production_animated_shadow.vert` | skinned batches | the storage buffer and the bones |

A lifted caster would need a lifted shadow vertex shader and couldn't take the fast path. Phase 1 has none: lifted
batches cast no shadow (see "The shadow decision").

### Textures in the vertex stage

- **The model vertex shaders took no sampler before.** SDL allows samplers in every stage (16 per stage); the shader
  is created with `num_samplers` and the pass binds them with `SDL_BindGPUVertexSamplers`.
- **Registers.** In SDL's convention a vertex shader's sampled textures come first in space 0, then its storage
  buffers. In a lifted shader the heightfield is t0 / s0 and the instances and motions move to t1 and t2. The pass
  binds storage buffers by slot, so the core's binding code doesn't change for them.
- **Uniforms.** The vertex stage uses one uniform buffer (b0); the lift's 32 bytes go in a second (b1).
- **Sampling.** `SampleLevel(..., 0)`: no derivatives in the vertex stage. Bilinear filtering with clamped edges is
  `Heightfield::at`: samples on texel centres, clamped past the edges. GPU filter weights have about 8 bits, so the
  GPU's height matches the CPU's within 1/256 of a texel's height change.
- **Formats.** RGBA16F filters on every device the games target. The renderer creates RGBA8, R8 (fonts) and RGBA8
  arrays; a heightfield is a new kind.

### What else reads a vertex's place

| Path | With a lift |
| --- | --- |
| Lighting, the world detail (RFC 0008/0009), the procedural detail's height terms, the fog | Read `world_position`, so they see the lifted one. The world detail projects level faces (within 18°) from above; a slope's stretch stays under 5 % |
| SSAO, HBAO, SSR | Read the depth buffer: nothing to do |
| FSR 3 motion | The previous position is lifted at its own xz. A static instance on a fixed field moves 0 |
| World curvature | Bends the lifted position |
| The opaque sort | One key per batch from its bounds' centre through each instance; the lift raises each centre (CPU heights) |
| Culling | The renderer doesn't cull. The host's bounds must span the lift |
| Radiance-cascade material sources | Projected straight down (`production_rc_material_source.vert`): the xz is the same, so the field is right without a lift. Only RFC 0010's source tops would read the height |
| The SDF terrain's dig mask | Compares the fragment's height with the terrain's patch, which knows nothing of the lift: a lifted batch can't be dug |

## Proposal

A heightfield texture, a per-batch lift that points at it, and two new vertex shader files that only lifted batches
draw with.

### API

```c
/* A heightfield for BTBHGPUSceneHeightLift: width x height samples, row by row along u, rows along v, sample (i, j) at
 * the texel centre ((i + 0.5) / width, (j + 0.5) / height). Kept as RGBA16F: the height and its slope along u and v
 * (central differences in heights per sample, one-sided at the edges), filtered bilinearly, clamped at the edges, one
 * mip; the renderer also keeps the heights on the CPU, for the opaque sort. Only height_lift takes it: every other
 * texture slot refuses it, and btbh_gpu_texture_update_rgba8 refuses it. btbh_gpu_texture_destroy frees it.
 * width and height: 2..BTBH_GPU_HEIGHTFIELD_MAX_SIZE (4096). A height that isn't finite, or past +-32768 (halves hold
 * the heights and their differences), fails with BTBH_GPU_INVALID_ARGUMENT naming the sample; a device that can't
 * sample RGBA16F with BTBH_GPU_MISSING_CAPABILITY. */
BTBHGPUTexture *btbh_gpu_window_heightfield_create(
    BTBHGPUWindow *window,
    const float *heights,
    uint32_t width,
    uint32_t height,
    BTBHGPUTextureReport *report
);

/* A static batch drawn on a heightfield: after its instance's matrix and the wind, every vertex rises by
 * height_scale * h(u, v) + height_offset, with u = x * world_to_uv[0] + world_to_uv[2] and
 * v = z * world_to_uv[1] + world_to_uv[3] at its flat world x and z. Its normal and tangent tilt with the
 * heightfield's slope there, unless BTBH_GPU_SCENE_HEIGHT_LIFT_KEEP_NORMALS. The host owns it; the renderer reads it
 * during the present call only. A lift with heightfield NULL draws as no lift. */
typedef struct BTBHGPUSceneHeightLift {
    BTBHGPUTexture *heightfield;     /* btbh_gpu_window_heightfield_create */
    float world_to_uv[4];            /* u scale, v scale, u offset, v offset; finite, the scales not 0 */
    float height_scale;              /* world units per unit of h; finite. 0 means 1 */
    float height_offset;             /* world units added; finite */
    uint32_t flags;                  /* BTBH_GPU_SCENE_HEIGHT_LIFT_* bits */
    uint32_t reserved;               /* 0 */
} BTBHGPUSceneHeightLift;

enum {
    /* The normals and tangents stay as the instance's matrix gives them (a lifted overlay that keeps its shading). */
    BTBH_GPU_SCENE_HEIGHT_LIFT_KEEP_NORMALS = 1u << 0
};
#define BTBH_GPU_HEIGHTFIELD_MAX_SIZE 4096u

/* In BTBHGPUSceneGeometryBatch, appended after world_detail_table: */
    /* NULL: drawn as before. Otherwise the batch's vertices are lifted by a heightfield, in every pass that draws it.
     * Static batches that cast no shadow only: with a heightfield set, a batch that is animated, a billboard, dug, or
     * would cast a shadow fails the present with BTBH_GPU_INVALID_ARGUMENT, naming the batch. A lifted batch draws
     * with production_model_lift.vert (production_model_detail_table_lift.vert with a world-detail table), loaded
     * beside the host's production_model.vert; without it the present fails, naming the file. Several batches may
     * point at one. */
    const BTBHGPUSceneHeightLift *height_lift;

/* Defined when btbh_gpu_window_heightfield_create, BTBHGPUSceneHeightLift and BTBHGPUSceneGeometryBatch.height_lift
   exist. */
#define BTBH_GPU_SCENE_HAS_HEIGHT_LIFT 1
```

`BTBHGPUSceneHeightLift` on 64-bit, 40 bytes:

| Offset | Field |
| --- | --- |
| 0 | `heightfield` |
| 8 | `world_to_uv[4]` |
| 24 | `height_scale` |
| 28 | `height_offset` |
| 32 | `flags` |
| 36 | `reserved` |

The batch grows from 280 to 288 bytes; `height_lift` sits at 280 (checked on this branch: `world_detail_table` at 272,
the batch 280 bytes before).

**Zero values.** `height_lift` NULL draws as before, byte for byte, with today's vertex shaders. So does a lift whose
`heightfield` is NULL (then only its `flags` and `reserved` are checked). Inside the lift, `height_scale` 0 means 1, so
a zeroed lift with a heightfield and its mapping set adds the heights as they are.

**Refusals** (`BTBH_GPU_INVALID_ARGUMENT`, "GPU scene batch N: height_lift: ...", as RFC 0008's checks do):
- `world_to_uv` not finite or a zero scale; `height_scale` or `height_offset` not finite;
- an unknown flag, `reserved` not 0;
- `heightfield` not a heightfield, or of another window (or destroyed); a heightfield in any other slot (a batch's four
  textures, a world detail's three, a table's `map_layers`, the post LUT, the canvas's texture and target, the render
  target, the capture destination, `btbh_gpu_texture_update_rgba8`);
- with a heightfield set: an animated batch (`animation_library` or `animation_pose` set), `billboard_mode` not 0,
  `dug_by` set, or a batch that would cast a shadow (see "The shadow decision").

A refracting batch may be lifted (a refraction strip on a lifted street).

### The lift in the vertex shader

A new include, `production_height_lift.hlsli`, with a C copy for the native tests (`src/gpu/btbh_gpu_height_lift.h`,
as 0.6.0 did for the world detail):

```hlsl
Texture2D<float4> height_lift_field : register(t0, space0);   // instances t1, motions t2 in a lifted shader
SamplerState height_lift_sampler : register(s0, space0);

cbuffer HeightLift : register(b1, space1)
{
    float4 height_lift_uv;      // u scale, v scale, u offset, v offset
    float4 height_lift_scale;   // x height_scale, y height_offset, zw a sample's slope to dh/dx, dh/dz (0: KEEP_NORMALS)
};

// x the lift, yz its slope (d/dx, d/dz) in world units.
float3 btbh_height_lift(float2 world_xz)
{
    const float2 uv = world_xz * height_lift_uv.xy + height_lift_uv.zw;
    const float3 h = height_lift_field.SampleLevel(height_lift_sampler, uv, 0.0f).xyz;
    return float3(h.x * height_lift_scale.x + height_lift_scale.y, h.yz * height_lift_scale.zw);
}

// In production_model.vert, under BTBH_HEIGHT_LIFT, after the instance's matrix and the wind:
const float3 height_lift_flat_world = world_position.xyz;   // the motion's previous position starts from here
const float3 height_lift = btbh_height_lift(height_lift_flat_world.xz);
world_position.y += height_lift.x;
// The lift maps (x, y, z) to (x, y + h(x, z), z): tangents go through its Jacobian, normals through its inverse
// transpose. Vertical faces keep their normals; level ones tilt with the slope.
raw_world_normal = float3(raw_world_normal.x - height_lift.y * raw_world_normal.y,
                          raw_world_normal.y,
                          raw_world_normal.z - height_lift.z * raw_world_normal.y);
raw_world_tangent.y += height_lift.y * raw_world_tangent.x + height_lift.z * raw_world_tangent.z;
```

- **One fetch per vertex.** The slope is stored beside the height, worked out once on the CPU when the heightfield is
  made, and filtered with it: the normals are smooth across texels. It is stored in heights per sample (bounded by the
  heights' range, so it never overflows a half) and the block turns it into world units:
  `zw = height_scale * world_to_uv.xy * (width, height)`. `KEEP_NORMALS` sets them to 0, so the tilt adds nothing (no
  branch, no second shader).
- **The previous position** is the flat one minus the instance's motion, lifted at its own xz (a second fetch, only
  for an instance with motion under FSR 3). A static instance on a fixed field gets no motion from the lift.
- **The world-detail table** (0.8.0) and a lift together: `production_model_detail_table_lift.vert` defines both.

### Files

| File | What |
| --- | --- |
| `shaders/gpu/production_height_lift.hlsli` | New, included: the bindings, the uniform block, `btbh_height_lift` |
| `shaders/gpu/production_model_lift.vert.hlsl` | New: `#define BTBH_HEIGHT_LIFT 1` and `production_model.vert.hlsl` |
| `shaders/gpu/production_model_detail_table_lift.vert.hlsl` | New: both defines |
| `production_model.vert` | The lift under `#if BTBH_HEIGHT_LIFT` (registers, the lift and tilt, the previous position). Without it, it and every shader built from it (`production_model_detail_table.vert`, `production_rc_material_source.vert`) compile to 0.10.0's SPIR-V, MSL and reflection byte for byte (tested) |
| `production_rc_material_source.vert` | No: it includes `production_model.vert` without the define; lifted batches draw there unlifted |
| `production_animated_model.vert` | No: skinned batches can't be lifted |
| `production_shadow.vert`, `production_shadow_fast.vert`, `production_animated_shadow.vert` | No: lifted batches cast no shadow in phase 1 (no `production_shadow_lift.vert`) |
| Fragment shaders (all six scene variants, the fast unlit one) | No: they already read the flat world position and the interpolated normal |
| `src/gpu/btbh_gpu_height_lift.h` | New: the C copy (halves, slopes, the bilinear read, the lift and tilt), the checks, the block's packing |

The two new files join `CMakeLists.txt`'s list after their bases, as 0.8.0's table shader did (73 shaders). The core
loads them from the directory of the host's static vertex shader, for lifted batches only. CHIMERA's list of 14 and
v2's named shaders don't need them unless they lift.

### The core

- **Pipelines.** Lifted batches get their own static model pipelines, `[table or not][the sun's and the omni shadow,
  the sun's only, unshadowed]` sets indexed as `model_pipelines` (colour targets, blend, depth, transparency, cull,
  stencil, fast unlit), made on first use, as 0.8.0's `table_model_pipelines`. The sets themselves (6 × 9,216
  pointers) are allocated by the first lifted batch, so a host that lifts nothing holds none.
  `btbh_gpu_window_scene_prewarm` (and its render-to-target form) takes batches, so it makes them too.
- **Bindings per lifted batch:** the heightfield as the one vertex sampler and the 32-byte block in vertex uniform slot
  1, pushed once per run of lifted batches sharing a lift (as the table's block). Unlifted batches bind as before.
  `height_lift` is part of RFC 0011's run key.
- **The opaque sort** lifts a lifted batch's key: each instance's bounds centre is raised by the heightfield at its xz,
  read bilinearly from the CPU copy.
- **The name check:** a lifted batch needs the lifted files. They are new files, so a missing one fails the present
  with its path ("A batch has a height_lift, and its vertex shader is missing (build the shaders from 0.11.0 or
  later): <path>"); a SPIR-V file of that name that doesn't name `height_lift_field` is refused the same way ("doesn't
  read a heightfield"). Read once per lifted pipeline made.

## Minimal example

```c
/* v2's Heightfield: samples at (x0 + i * step, z0 + j * step), nx by nz. Texel centres on its samples: */
BTBHGPUTextureReport tr;
BTBHGPUTexture *ground = btbh_gpu_window_heightfield_create(window, field.heights, field.nx, field.nz, &tr);

BTBHGPUSceneHeightLift lift = {0};
lift.heightfield = ground;
lift.world_to_uv[0] = 1.0f / ((float)field.nx * field.step);
lift.world_to_uv[1] = 1.0f / ((float)field.nz * field.step);
lift.world_to_uv[2] = 0.5f / (float)field.nx - field.x0 * lift.world_to_uv[0];
lift.world_to_uv[3] = 0.5f / (float)field.nz - field.z0 * lift.world_to_uv[1];

/* The street pieces: their instances stand at height 0; the renderer lifts every vertex. They cast no shadow. */
street_batch.height_lift = &lift;
street_batch.flags |= BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW;

/* An overlay 0.07 over the ground, keeping its shading: */
BTBHGPUSceneHeightLift overlay_lift = lift;
overlay_lift.height_offset = 0.07f;
overlay_lift.flags = BTBH_GPU_SCENE_HEIGHT_LIFT_KEEP_NORMALS;
telegraph_batch.height_lift = &overlay_lift;   /* transparent: casts no shadow anyway */

/* Buildings, props and units: lifted by the host as today (rigid), height_lift NULL. */
```

In v2's Rust: one `HeightLift` per stage beside the kit library (boxed, so it doesn't move while a present runs),
`height_lift: lift.map_or(null, ...)` in `batch_of` for the ground pieces' and overlays' draws, and the pieces'
instances at height 0 instead of their cell's lift.

## Implementation notes for the host

- **No cracks on lattice lines.** With texel centres on the lattice, bilinear filtering is linear along every
  lattice line. Two pieces whose shared edge lies on a lattice line meet without a crack, T-junctions included,
  because every vertex on that line takes the line's linear height. Put the lattice on the kit's cell corners (v2's
  axis-aligned edges do; its 45° kerbs don't lie on lattice lines, and their edges bend as the triangles do).
- **Between vertices the triangle is flat.** A cell of two triangles bends along its diagonal, not as the bilinear
  surface: they differ by up to a quarter of the cell's twist (h00 + h11 − h01 − h10) / 4 at its middle. An overlay
  triangulated another way over the same cell can dip by that much. v2's twist (median 0.0007, p99 0.007–0.009) is
  far under its overlays' 0.057–0.1; at crests (0.07–0.11) a subdivided overlay at quarter corners keeps it under a
  sixteenth of that.
- **Shadows.** A lifted batch casts none: give it `BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW` and `spot_shadow_visible` 0,
  or draw it transparent. It still takes them.
- **Culling.** The host's bounds must span the lift: v2's chunks carry their lowest lift (`base`, since `361be68d`);
  with the renderer's lift they need the field's range over the chunk.
- **Hosts that lift on the CPU** (units, props, the camera) read the same heights with `Heightfield::at`: they agree
  with the GPU within the halves' rounding (1/256 at v2's heights) and the filter weights' precision (1/256 of a
  texel's height change).
- **Radiance cascades.** The trace counts a street receiver at height 0 whatever the ground (RFC 0010, "How a ray
  meets them"). On Height terrain, occluder tops given over the ground beneath them (v2 does) shade the street round a
  hill's houses as on flat ground. The cascades' material sources take a lifted batch unlifted (straight down, same xz).

## Extra: custom-mesh updates queued into the frame

v2's extra request: each `btbh_gpu_custom_mesh_update` costs about 0.17 ms whatever its size, because
`btbh_gpu_custom_mesh_upload_buffers` creates two transfer buffers and acquires and submits a command buffer of its own
on every call. Moving many small conformed marks pays that per mark.

### Design

An opt-in flag on a new `_ex` update; the plain update is unchanged (it is now `_ex(..., 0, ...)`).

```c
enum { BTBH_GPU_CUSTOM_MESH_UPDATE_QUEUED = 1u << 0 };
#define BTBH_GPU_CUSTOM_MESH_QUEUE_MAX_BYTES (256u * 1024u * 1024u)
BTBHGPUResult btbh_gpu_custom_mesh_update_ex(BTBHGPUCustomMesh *mesh, const void *vertices,
    uint32_t vertex_byte_count, const uint32_t *indices, uint32_t index_count, uint32_t flags,
    BTBHGPUCustomMeshReport *report);
#define BTBH_GPU_HAS_QUEUED_MESH_UPDATES 1
```

- **At the call.** The data is validated as without the flag and copied into a staging area of the window (a byte
  array that grows by doubling; each range at a multiple of 16). The mesh's counts and bounds change at once (the
  opaque sort and the draws read them, and nothing draws before the copy). A mesh with buffers of its own that needs
  bigger buffers gets them now (the old ones are released; SDL frees them when no frame uses them).
- **At the next scene.** `btbh_gpu_window_scene_present_instances(_with_visibility)` and
  `btbh_gpu_window_scene_render_to_target` add the queued bytes to the transfer they already map for the instances,
  bones and motions, and record the copies in the copy pass they already begin, at the start of their own command
  buffer, before any pass draws. So N updates cost one map and one copy pass (and no submission of their own) instead
  of N submissions. A mesh of its own is cycled (a frame in flight keeps its data); a mesh in the shared buffers is
  written in place.
- **When it shows:** in the next scene the window draws, whichever of the two comes first, and in every one after it.
  A scene that fails before it is kept leaves the queue as it was for the next one.

### Rules

- **Several updates of one mesh** before a scene: only the last is copied (a later queued or plain update replaces a
  queued one the scene hasn't copied; the replaced bytes stay in the staging area until the next scene empties it).
- **A plain update after a queued one always lands after it.** The case that needs care: a scene presented but not yet
  submitted by the canvas has copied a queued update of a mesh in the shared buffers, and the host then updates the
  mesh plainly before the canvas. A submission of its own would run before that frame, and the frame's copy would then
  overwrite it. So that plain update is recorded into the frame's command buffer, after its draws: the frame shows the
  queued data, the next scene the plain one. A mesh of its own needs nothing (SDL cycles its buffer, as since 0.1.0).
  A host that never queues never takes this path.
- **Destroy:** `btbh_gpu_custom_mesh_destroy` drops the mesh's queued update; one a frame already copied still runs
  (SDL frees the buffers after it). A window closed with updates queued drops them.
- **Shared buffers** (`BTBH_GPU_CUSTOM_MESH_SHARED_BUFFERS`, RFC 0011): as without the flag, the update must fit the
  mesh's ranges (a bigger one fails with `BTBH_GPU_INVALID_ARGUMENT`, nothing queued, the mesh as it was).
- **Limits:** one update's limits are the plain update's. The staging area holds at most
  `BTBH_GPU_CUSTOM_MESH_QUEUE_MAX_BYTES` (256 MiB) at a time, replaced updates included; past that the call fails with
  `BTBH_GPU_INVALID_ARGUMENT` and queues nothing (present a scene, or update plainly).
- An unknown flag fails with `BTBH_GPU_INVALID_ARGUMENT`. A window that queues nothing draws as before, byte for byte.

A transient vertex stream (the other option v2 named) would need a new geometry kind and buffer per frame; the queue
gives v2's marks the same saving with the meshes it already has.

### Queued instance uploads and partial updates (0.11.0, v2's night perf round)

v2's next two requests, from its night perf round on the Deck preset: **(2)** a partial upload that keeps the rest of
the buffer (v2 re-uploads whole city and kit chunks only because `btbh_gpu_instance_buffer_upload` cycles the buffer,
so everything outside the range is undefined after it), and **(3)** one copy pass a frame (each chunk upload is a
submission of its own). Both opt-in; the plain calls are unchanged.

```c
enum {
    BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED = 1u << 0,    /* into the next scene's copy pass, in place */
    BTBH_GPU_INSTANCE_BUFFER_UPLOAD_NO_CYCLE = 1u << 1   /* submitted now, in place */
};
BTBHGPUResult btbh_gpu_instance_buffer_upload_ex(BTBHGPUInstanceBuffer *buffer, uint32_t byte_offset,
    const void *bytes, uint32_t byte_count, uint32_t flags, BTBHGPUInstanceBufferReport *report);
#define BTBH_GPU_HAS_QUEUED_INSTANCE_UPLOADS 1

BTBHGPUResult btbh_gpu_custom_mesh_update_range(BTBHGPUCustomMesh *mesh, uint32_t first_vertex,
    const void *vertices, uint32_t vertex_byte_count, uint32_t first_index, const uint32_t *indices,
    uint32_t index_count, uint32_t flags /* 0 or BTBH_GPU_CUSTOM_MESH_UPDATE_QUEUED */,
    BTBHGPUCustomMeshReport *report);
#define BTBH_GPU_HAS_PARTIAL_MESH_UPDATES 1
```

- **One queue a window.** The custom-mesh queue became the window's upload queue: an entry per range (a mesh's
  vertices, its indices, or an instance buffer, whose two buffers both take the bytes), chained per target. A whole mesh
  update is two entries (vertices, indices) and drops the mesh's earlier ones, as before; it is the only entry that
  cycles (a mesh of its own). Every other entry, a queued instance upload and a partial update, is written in place.
  The scene copies the live entries in its one transfer and copy pass, after its instances, bones and motions. The cap
  (`BTBH_GPU_CUSTOM_MESH_QUEUE_MAX_BYTES`) counts both kinds together.
- **Later wins, independent of copy order.** A later write to a target, queued or not, patches the bytes of the earlier
  queued ranges it overlaps and drops those it covers; one that an earlier range holds entirely goes into it instead of
  a new entry. So ranges that overlap carry the same bytes in the overlap, and the order in which the GPU runs their
  copies doesn't matter (SDL's Vulkan and D3D12 backends put barriers around every copy; its Metal backend adds none
  within a copy pass, and nothing here depends on it).
- **Partial mesh updates.** In place within the mesh's counts, which don't change; indices must be below the vertex
  count. Mesh of its own or shared alike. **The bounds grow** to cover the new vertices (the union with the old ones) and
  never shrink until a whole update measures afresh: the core keeps no CPU copy of the vertices, and asking the caller
  for bounds would be one more thing to get wrong. Loose bounds only shift the opaque sort's key.

#### Ordering against frames in flight (SDL's semantics)

- **A buffer an in-flight frame reads, written without cycling:** safe on the queue's order alone. SDL has one queue a
  device; `SDL_UploadToGPUBuffer` with `cycle = false` puts a barrier from the buffer's default use (vertex or storage
  read) to copy-destination before the copy on Vulkan and D3D12, and those barriers order every command submitted
  before them, earlier command buffers included; on Metal, SDL's buffers are hazard-tracked (the default), and Metal
  orders the copy after the earlier command buffers' reads on its queue. The
  earlier frames draw the old data, and the copy may wait for their reads. What SDL's documentation warns against
  ("overwriting a section of data that has already been referenced") is the other case: a buffer referenced by a
  command buffer that is recorded but not yet submitted, written by one submitted before it. Here that is the frame a
  presented scene leaves waiting for the canvas (`window->pending_command`). So a write that isn't queued and doesn't
  cycle (`NO_CYCLE`, a plain partial mesh update) goes, while such a frame waits, into that frame after its draws
  instead of a submission of its own: the frame draws the old data, and a queued copy it holds lands first. The older
  plain whole update of a shared mesh keeps its 0.10.0 behaviour (submitted on its own, so a waiting frame shows it),
  except under the queued-update rule above.
- **Two queued uploads overlapping in one frame:** the later wins, by the patch above, whatever the copy order.
- **A plain upload after a queued one:** a queued range not yet copied takes the plain upload's bytes where they
  overlap (and goes if covered), so the plain one wins. A queued range already copied into the waiting frame is
  before it: `NO_CYCLE` goes into that frame after it; a cycling plain upload lands in a new buffer, which later frames
  draw (only its own range defined, as always for a cycling upload).
- **A render-to-target first:** it is a scene: it copies the whole queue at the start of its command buffer and submits
  it. The buffer isn't cycled, so the next present draws the data too.
- **Destroy with uploads pending:** `btbh_gpu_instance_buffer_destroy` drops the buffer's queued ranges and the entries
  forget it; one already copied into a frame not yet submitted still runs (SDL frees the buffers after that frame). A
  window closed with uploads queued drops them.
- **A frame dropped after it copied** (the canvas refuses its submission, or the next scene is presented without one):
  `btbh_gpu_window_finish_pending_frame` now submits instead of cancelling a frame that holds queued copies or a write
  recorded after its draws, so they land; the queue was already emptied for it. This also fixes 0.11.0's queued mesh
  updates, which such a frame lost. A host that never queues drops such frames as before.

#### Measured

Windows, RTX 3070 Ti, Vulkan, no debug device, the machine's perf lock held (06:49–06:50 on 2026-11-23; two other
sessions' `v2-game` processes ran). `BTBH_SMOKE_BENCH=upload-queue`: 60 chunks (instance buffers of 4 to 64 KiB, 42 to
682 instances, 2,037 KiB in all), each drawn by a batch, all uploaded every frame; five modes in interleaved rounds,
medians of 120 frames a mode, three runs:

| Mode | At the call, per upload | In the present, per upload | Frame CPU (uploads, present, canvas) |
| --- | --- | --- | --- |
| No uploads | | | 0.094–0.102 ms |
| Whole chunks, plain (today) | 84–90 µs | 5.4–6.5 µs (+114–126 µs in the canvas) | 12.3–13.4 ms |
| Whole chunks, queued | 0.74–0.95 µs | 3.2–3.5 µs | **0.35–0.39 ms** |
| A quarter of each, queued (507 KiB) | 0.21–0.23 µs | 1.8–2.1 µs | **0.23–0.25 ms** |
| A quarter of each, `NO_CYCLE` | 82–92 µs | 4.9–5.9 µs (+115–132 µs in the canvas) | 12.3–14.1 ms |

The plain upload's cost is its transfer buffer, command buffer and submission; the canvas after 60 of them waits about
120 µs more each. Queued, an upload is a copy into the window's queue at the call and, in the present, a share of the one
map and SDL's two copies (the instance buffer's storage and vertex buffers). The GPU side wasn't measured: the copies
are 2 MiB at most here, but a non-cycled copy waits for the earlier frame's reads of that buffer.

The queued mesh updates after the queue was generalised (`BTBH_SMOKE_BENCH=mesh-queue`, release/0.11.0's build and
this branch's alternating, two runs each, same lock): 0.27–0.28 µs at the call for both; 2.35–2.48 µs (before) and
2.36–2.41 µs (after) an update in all; in the shared buffers 1.72–1.81 and 1.73–1.74 µs.

#### Was `NO_CYCLE` worth offering?

Yes: it is the partial upload that keeps the rest without waiting for a scene (a load, a render-to-target before the
first present), and it is the plain path with `cycle = false` plus the waiting-frame rule, nothing new to keep in sync.
It still costs a submission a call (unless a frame waits), so per-frame streaming should queue.

## Cost

Estimates from the draft, to compare with "Implementation notes (phase 1)".

| Item | Estimate |
| --- | --- |
| Unlifted batches | Nothing: today's vertex shaders, byte for byte in SPIR-V |
| Lifted vertex | One bilinear RGBA16F fetch and about 25 ALU (the tilt); a second fetch for a moving instance under FSR 3 |
| 100,000 lifted vertices a frame | About 0.001 ms of fetches on the bench's RTX 3070 Ti (170 billion bilinear samples a second), 0.004–0.006 ms on a Deck (16–26 billion) |
| CPU per lifted batch | One sampler binding and a 32-byte push (once per run); the sort key's one bilinear read per instance |
| Memory | 8 bytes a texel on the GPU and 4 on the CPU: a 565 × 565 field is 2.4 MiB and 1.2 MiB |

The fetches are cheap. What a smooth slope costs is the vertices a piece needs to follow it: a street tile of two
triangles a cell follows the slope cell by cell; a finer bend needs more vertices, and those cost what vertices cost.

## Compatibility

- **Versioning:** minor. New functions, structs, flags and macros; a pointer appended to the batch (280 → 288 bytes);
  two new shader files. Every game compiles the C core from the checkout whose header it builds against, so they
  agree on the size.
- **Shaders.** The changed source compiles to 0.10.0's SPIR-V without the define. 0.10.0's shaders under the new core
  draw everything but lifted batches as before; a lifted batch fails the present naming the missing lifted file.
- **Be The Bullet Hell v2 (Rust, bindgen):** nothing changes until it opts in. bindgen gives the new structs
  `Default`, and `batch_of` ends in `..Default::default()`, so `height_lift` is null. To adopt: bump the pin, build
  the two lifted shaders, make the heightfield, set the lift on the ground pieces' and overlays' batches (with
  `NO_SUN_SHADOW`), and stand their instances at 0. For the marks: `btbh_gpu_custom_mesh_update_ex(...,
  BTBH_GPU_CUSTOM_MESH_UPDATE_QUEUED, ...)`.
- **Be The Bullet Hell (v1, Swift):** nothing changes. Batches are built zeroed. A `MaterialDescriptor.heightLift`
  can follow when it asks; its SDF terrain (RFC 0007) can't be combined with a lift in phase 1.
- **CHIMERA (Rust, bindgen):** nothing changes; it regenerates the bindings for the bigger batch. Its list of 14
  shaders can stay.
- **Tyrian3D (C):** nothing changes. `init_batch` memsets the batch. Pinned at v0.2.1.

## Platforms and testing

- **Native:**
  - `btbh_gpu_scene_order_test`: `offsetof(height_lift) == 280`, the batch 288 bytes, `sizeof(BTBHGPUSceneHeightLift)
    == 40` and its offsets; the run key covers `height_lift`; a lifted batch sorts by its lifted centre.
  - `btbh_gpu_height_lift_test`: the C copy of the lift and the tilt. Halves (ties, subnormals, every half both ways);
    a plane's lift and normal exactly; a vertical face's normal unchanged; tangent and normal orthogonal after the
    tilt; the slopes' central differences and edges; the mapping of a v2-style lattice onto texel centres; linearity
    along lattice lines; the refusals' reasons; the block.
  - `btbh_gpu_phase2_shaders`: the lifted shaders' resources (1 sampler, 2 storage buffers, 2 uniform buffers) and
    their bases' inputs and outputs.
- **GPU smoke (Windows/Vulkan):** `btbh_gpu_height_lift_smoke`:
  - **Unlifted, byte for byte:** every smoke's images as with 0.10.0's core and shaders; with a lift whose heightfield
    is NULL; and the changed sources compiled to 0.10.0's SPIR-V.
  - **Lifted against CPU-lifted:** a lit terrain grid with a normal map and a box on it (the sun's shadow, the sun's and
    the omni shadow, none, the front-to-back sort, the radiance cascades), the terrain transparent, unlit alpha,
    additive and soft-particle overlays with `KEEP_NORMALS`, a refracting overlay, and the terrain with a world-detail
    table, each against the same meshes lifted on the CPU by the C copy and drawn unlifted.
  - **Refusals,** each through the present (or the call that takes the texture).
  - **Prewarm** makes the lifted pipelines (none made by the renders after it).
  - **Old shaders:** a lifted batch with a shader directory without the lifted file, or with one that doesn't lift,
    fails with the file's path.
  - `btbh_gpu_mesh_queue_smoke`: the queued updates in the next render-to-target and present, the rules above.
- **Not tested in phase 1:** FSR 3's motion of a lifted batch (the code path is the draft's; a static instance's
  previous position is the lifted one, so its motion is 0 by construction), seams between separate pieces (the
  lattice-line linearity is tested natively).
- **Bench:** the frame bench's CPU mode for the cost of a frame without lifted batches; the mesh queue smoke's bench
  (`BTBH_SMOKE_BENCH=mesh-queue`) for the queued updates.
- **Platforms:** Windows/Vulkan runs the smokes. The Mac session runs `check.sh` (vertex samplers go through
  shadercross to MSL as fragment samplers do; not run on Metal yet). Linux and the Deck through v2.
- **Games:** v2's Height terrain captures and Deck-preset timings.

## Alternatives

| Alternative | Why not (now) |
| --- | --- |
| **The lift in today's vertex shader behind a uniform branch** | Every static batch would carry the sampler binding and the longer shader; 0.8.0 measured about 2 % for one more attribute read in a frame of many small draws |
| **The host bakes the slope into its meshes** | The pieces are instanced (one mesh, many cells); a sloped copy per cell multiplies the meshes. v2 lifts per cell today for that reason |
| **The gradient by four more fetches** (an R16F height only) | Five fetches a vertex instead of one, to save 6 bytes a texel |
| **An R16 UNORM height** | More precision for a given range, but 16-bit UNORM filtering isn't required on every Vulkan device, and the gradient still needs a signed float |
| **The slope stored per uv unit** (the draft) | It grows with the field's size (a steep 4096-wide field overflows a half); per sample it stays within the heights' range, and the block scales it |
| **A shear by the instance** (the host gives each instance a slope, the vertex shader shears the piece) | A plane per piece: smooth inside a piece, a crease at every piece's edge where the slope changes. No texture, but not "follows the slope" |
| **Tessellation or mesh shaders** | SDL's GPU API has neither |
| **The SDF terrain's heightfield base** (RFC 0007) | It meshes its own ground; kit pieces (streets, kerbs, markings) placed on it would still need lifting |
| **Lifting skinned batches and billboards now** | Units and props stand on the ground rigidly (the host lifts their instance); bending a skinned body by the slope would look wrong. Phase 2 if a game asks |
| **A lifted shadow vertex shader now** | v2's lifted batches cast none; see "The shadow decision" |

## Implementation notes (phase 1 and the extra, 2026-11-23)

Branch `feature/0.11.0-height-lift` on `c5390f0b` (0.9.0, RFC 0011 phase 1, the RFC 0010–0012 drafts). As drafted, with
these differences and decisions:

- **Offsets (checked):** `world_detail_table` at 272 and a 280-byte batch on this branch (RFC 0011 phase 1 changed no
  struct), so `height_lift` sits at 280 and the batch is 288 bytes. `BTBHGPUSceneHeightLift` 40 bytes as drafted.
- **No lifted shadow shader** (see "The shadow decision"): two new vertex shader files, not three. The rule reads the
  batch's fields (`transparency_enabled`, `NO_SUN_SHADOW`, the visibility byte, `spot_shadow_visible`), exactly as the
  shadow passes choose their casters, so no lifted batch reaches a shadow pass.
- **The slope per sample** (the draft stored it per uv unit): the block's `zw` scale it to world units, and
  `KEEP_NORMALS` is a zero factor, so the tilt adds exactly nothing (no branch). The draft's `z flags` field is gone.
- **Heights past ±32768 are refused** at creation (halves hold the heights and their differences); not finite too. The
  message names the sample.
- **"Static" means no animation fields:** a batch with `animation_library` or `animation_pose` set and a heightfield is
  refused, whether or not its model is skinned (the draft said "skinned").
- **A lift without a heightfield** has only its `flags` and `reserved` checked (as a table without `map_layers`).
- **The lifted pipeline sets** are allocated by the first lifted batch (442 KiB of pointers otherwise held by every
  host); prewarm allocates and fills them as a present would. An unlifted batch's pipeline lookup is as before (the
  slot first, the lift's work only when a lifted batch finds no sets), and a batch without a lift leaves the lift's
  checks after one pointer test: a first version that looked at the lift before the slot cost a frame of 2,001
  batches about 7 µs of prepare (measured, then fixed).
- **The name check** reads the lifted file when its pipeline is made: missing ("is missing") or, in SPIR-V, without
  `height_lift_field` ("doesn't read a heightfield"), the present fails with the path.
- **The opaque sort** lifts each instance's bounds centre (the draft said "one key per batch from its centre": the key
  was already a mean over the instances, so each is lifted where it stands).
- **Not done:** a C model of the GPU's 8-bit filter weights (the CPU-lifted references differ from the GPU's by those
  weights: a few edge pixels, and specular sparkles under a world-detail normal map); FSR 3's motion of a lifted batch
  isn't tested on the GPU; Metal not run.
- **The extra** as in its section: the copies ride in the scene's own upload (one map, one copy pass, no extra
  submission). The ordering rule for shared meshes was found while designing it (without it, a plain update between a
  present and its canvas would be overwritten by the frame's queued copy; the smoke fails that way with the rule taken
  out).
- **MSL nondeterminism (unrelated):** three FSR 3 compute shaders' MSL (`production_fsr3_luma_pyramid`,
  `_prepare_inputs`, `_shading_change_pyramid`) differ between any two builds of the same source in the buffer index
  SPIRV-Cross gives an atomic image's fallback buffer; their SPIR-V is identical. Every other artefact is byte for byte.

### Tests (exact)

- `tools/check.ps1`: 28/28 (26 + `btbh_gpu_height_lift_smoke` and `btbh_gpu_mesh_queue_smoke`), 73 shaders with SPIR-V
  and MSL, public headers C11/C++17 /W4 /WX clean (on the tip, and before the last two scene commits).
- Swift (`BTBH_ENABLE_GPU_TESTS=1`, shaders from the build): build clean; 301 tests, 2 skipped, 0 failures.
- Native: `btbh_gpu_height_lift_test` 66,135 checks; `btbh_gpu_scene_order_test` (288 bytes, the lift's offsets, 60
  run-key fields over 288 bytes: 243 bytes break a run, 45 keep it; a lifted batch's sort key); `btbh_gpu_phase2_shaders`
  (the lifted shaders: 1 sampler, 2 storage buffers, 2 uniform buffers; their bases' inputs and outputs).
- GPU, `btbh_gpu_height_lift_smoke`: 12 scenes, lifted against CPU-lifted: at most 256 of 36,864 pixels differ by more
  than 8 (the world-detail table's specular sparkles; 1–4 pixels in the lit, shadowed, sorted, cascaded, transparent
  and overlay scenes, 90 in the refraction), each scene ~20,000 pixels from its unlifted picture; a lift without a
  heightfield 0 pixels from none; 34 refusals; prewarm made 40 pipelines and 55 shaders, the renders after it none; the
  old-shader checks.
- GPU, `btbh_gpu_mesh_queue_smoke`: 17 byte-for-byte comparisons against meshes made with the data (render-to-target
  and present), the ordering rule for a shared and an own mesh, the refusals, a closed window.
- **Identity against `c5390f0b`** (its build wrote, this one compared): flags, prewarm (16 frames + target frames),
  world detail, runs, scene target, font and SDF terrain: **153 images, 10,577,792 pixels, 0 differ**, with nothing set
  and again with `BTBH_SMOKE_SHARED_MESHES=1` (152 + the SDF terrain's), and again on the tip (153). Every shader that
  existed: same SPIR-V, MSL and reflection (the three FSR 3 MSL files aside, above).

### Measured

Windows, RTX 3070 Ti (driver 621.14), Vulkan, without a debug device. Runs marked *locked* held the machine's shared perf
lock (`perf-lock`, blocks at 03:20–03:24 and 03:51–03:54 on 2026-11-23); one to five other sessions' `v2-game`
processes ran through every run anyway, so the CPU times are up to twice a quiet machine's (the frame bench's geometry
encode 0.6 ms against 0.33 ms in the quietest pairs). Logs: `%LOCALAPPDATA%\btbh-wt-0110-bench-locked`, `-locked2` (and
the unlocked `-bench`).

**Queued custom-mesh updates.** `BTBH_SMOKE_BENCH=mesh-queue`: 200 meshes of 64 vertices (186 indices), each a batch,
all updated every frame and presented, in rounds interleaving five modes (still, plain, queued, still in the shared
buffers, queued in the shared buffers); per update, medians of 120 frames a mode, three runs:

| Per update | At the call | In the present (its upload) | In all |
| --- | --- | --- | --- |
| Plain (*locked*) | 234–271 µs | 3.7–4.3 µs (0.0) | 238–275 µs |
| Plain (not locked, quieter) | 156–245 µs | 3.2–3.8 µs | 159–249 µs |
| Queued, buffers of its own (*locked*) | 0.43–0.51 µs | 3.9–4.8 µs (3.7–4.6) | **4.3–5.3 µs** |
| Queued, shared buffers (*locked*) | 0.45–0.47 µs | 3.0–3.2 µs (2.9–3.1) | **3.4–3.6 µs** |

The plain update's cost is the transfer buffers, the command buffer and its submission (v2 measured about 170 µs). The
queued one's is mostly SDL's two uploads a mesh in the present (a barrier pair each; cycling for a mesh of its own).
200 marks: 48–55 ms a frame plainly, about 1 ms queued.

**A frame without lifted batches** (the frame bench's CPU mode, mode none, 2,001 batches: `c5390f0b`'s core, shaders
and bench against this branch's, alternating processes, 12 rounds each, the scene report's CPU fields, frames over 1.5×
their configuration's 25th percentile dropped):

| Set | Pairs | Geometry encode | Prepare | Shadow encode |
| --- | --- | --- | --- | --- |
| Quietest (not locked, 1 `v2-game`) | 6 | +3.7 µs (p25 +2.4) | +7.0 µs | +1.7 µs |
| Not locked, after the validation change | 6 | +3.0 µs (p25 +0.6) | +7.6 µs | noisy |
| *Locked* block 1, after the pipeline-lookup fix | 8 | +15 µs | +0.7 µs | +16 µs |
| *Locked* block 2, paired medians | 16 | +13 µs (quartiles +6..+65) | +4.2 µs (+1.7..+8.2) | +26 µs (+11..+39) |

The GPU does the same work: the shaders are the same bytes and the pictures the same pixels. The prepare's +7 µs came
from a first version of the pipeline lookup that looked at the lift before the slot; fixed (`c9d280f2`). The rest
scatters with the other processes (the 16 locked pairs' geometry differences run from −0.23 to +0.17 ms): about 2 ns a
batch in the quietest pairs (the run key's one more field, the batch's 8 more bytes), and nothing under about 0.03 ms a
frame resolved beside other sessions' processes.

**Lifted batches** (the frame bench's `lift`, `lift-keep`, `lift-flat` modes in one process, five windows with the
`+noruns` variants, orders rotated between runs; `lift-flat` is the same draws without the lift, all casting no shadow):

- GPU (*locked*, three runs, 60 rounds, every round kept): the opaque pass +0.043 ms (90 % interval +0.033 to +0.051;
  the whole frame +0.035) for 2,001 lifted draws, 6,112 instances, 2.2 million triangles, 4.4 million vertices: about
  0.001 ms per 100,000 lifted vertices, the RFC's estimate. `KEEP_NORMALS` +0.041 ms: the fetch costs, the tilt doesn't.
  (Not locked, beside four `v2-game` processes: +0.006 ms, interval −0.046 to +0.066: unresolved.)
- CPU (*locked*, three runs): geometry encode +14 µs a frame in runs (7 ns a lifted batch; 3 ns in a quieter unlocked
  run), +12 µs with runs off (6 ns; 16 ns unlocked), prepare +4 µs (2 ns). `KEEP_NORMALS` the same.

## Phases

1. **Phase 1, the lift for static batches (0.11.0).** `btbh_gpu_window_heightfield_create`, the struct, the pointer,
   the two vertex shader files and the include, the pipelines, the refusals, the C copy, the tests and the bench. With
   it, the extra: queued custom-mesh updates.
2. **Phase 2, only if asked.** Lifted shadow casters (`production_shadow_lift.vert`), a heightfield update (with the
   motion it causes), lifted skinned batches and billboards, the radiance-cascade material sources lifted (for RFC
   0010's tops), and the SDF terrain with a lift.
