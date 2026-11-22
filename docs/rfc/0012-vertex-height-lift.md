# RFC 0012: A vertex-stage height lift from a heightfield

- **Status:** Draft (2026-11-22), awaiting v2's answers
- **Requested by:** Be The Bullet Hell v2 (its session, 2026-11-22, after the owner's sixth play-test)
- **Affects:** Be The Bullet Hell v2 (adopts it). Be The Bullet Hell (v1, Swift), CHIMERA and Tyrian3D: nothing, it is
  opt-in and their batches keep today's vertex shaders. They rebuild the shaders with the C core, as every release
- **Issue:** none yet
- **Depends on:** nothing. Written against 0.8.0's layout (`feature/0.8.0-detail-table`, unreleased): the batch is
  280 bytes there and this RFC appends a pointer at 280 (at 288 if RFC 0011's `draw_list` is appended first). It
  follows 0.8.0's pattern of a separate static vertex shader (`production_model_detail_table.vert`) for the batches
  that need more

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

## What the code allows

### Where a vertex is placed

`production_model.vert.hlsl` (static) and `production_animated_model.vert.hlsl` (skinned):

1. The surface variant moves the vertex (dissolve, shatter) in object space.
2. The instance's matrix takes it to world space; the wind sways it.
3. The normal and tangent go through the matrix's cofactor.
4. `world_position` (flat) goes to the fragment shader, which lights, details and digs with it.
5. `btbh_world_bend` bends it for the screen (`world_curvature`); the previous frame's position is worked out the
   same way for FSR 3's motion.

A lift belongs between 2 and 3: after the instance and the wind, before the normal's frame, the flat world position,
the bend and the motion.

### Shadows

Three vertex shaders draw the shadow maps (directional, spot, omni):

| Shader | For | Instances read |
| --- | --- | --- |
| `production_shadow_fast.vert` | static batches without vertex motion, billboards or a dig mask (`btbh_gpu_scene.c:5869-5876`) | as a vertex stream, position only |
| `production_shadow.vert` | the other static batches | the storage buffer, with the surface variants and billboards |
| `production_animated_shadow.vert` | skinned batches | the storage buffer and the bones |

A lifted batch must cast where it is drawn, so its shadow vertex shader lifts too. It can't take the fast path,
which has no texture and no uniforms but the light's matrix.

### Textures in the vertex stage

- **The model vertex shaders take no sampler today.** SDL allows samplers in every stage (16 per stage); the shader
  is created with `num_samplers` and the pass binds them with `SDL_BindGPUVertexSamplers`.
- **Registers.** In SDL's convention a vertex shader's sampled textures come first in space 0, then its storage
  buffers. In a lifted shader the heightfield is t0 and the instances and motions move to t1 and t2. The pass binds
  storage buffers by slot, so the core's binding code doesn't change for them.
- **Uniforms.** The vertex stage uses one uniform buffer (b0); the lift's 32 bytes go in a second (b1).
- **Sampling.** `SampleLevel(..., 0)`: no derivatives in the vertex stage. Bilinear filtering with clamped edges is
  `Heightfield::at`: samples on texel centres, clamped past the edges. GPU filter weights have about 8 bits, so the
  GPU's height matches the CPU's within 1/256 of a texel's height change.
- **Formats.** RGBA16F filters on every device the games target. The renderer creates RGBA8, R8 (fonts) and, on
  0.8.0, RGBA8 arrays; a heightfield is a new kind.

### What else reads a vertex's place

| Path | With a lift |
| --- | --- |
| Lighting, the world detail (RFC 0008/0009), the dig mask (RFC 0007) | Read `world_position`, so they see the lifted one. The world detail projects level faces (within 18°) from above; a slope's stretch stays under 5 % |
| SSAO, HBAO, SSR, fog | Read the depth buffer: nothing to do |
| FSR 3 motion | The previous position is lifted at its own xz. A static instance on a fixed field moves 0 |
| World curvature | Bends the lifted position |
| The opaque sort | One key per batch from its bounds' centre; the lift raises it |
| Culling | The renderer doesn't cull. The host's bounds must span the lift |
| Radiance-cascade material sources | Projected straight down (`production_rc_material_source.vert`): the xz is the same, so the field is right without a lift. Only RFC 0010's source tops read the height |
| The SDF terrain's dig mask | Compares the fragment's height with the terrain's patch, which knows nothing of the lift |

## Proposal

A heightfield texture, a per-batch lift that points at it, and three new vertex shader files that only lifted batches
draw with.

### API

```c
/* A heightfield for BTBHGPUSceneHeightLift (RFC 0012): width x height samples, row by row along u, rows along v,
 * sample (i, j) at the texel centre ((i + 0.5) / width, (j + 0.5) / height). Kept as RGBA16F: the height and its
 * gradient along u and v (central differences, one-sided at the edges), filtered bilinearly, clamped at the edges,
 * one mip; the renderer also keeps the heights on the CPU, for the opaque sort. Heights are half floats: for
 * precision give values within a few hundred units, or normalise them and use height_scale. Only height_lift takes
 * it: every other texture slot refuses it, and btbh_gpu_texture_update_rgba8 refuses it. btbh_gpu_texture_destroy
 * frees it. width and height: 2..4096. A height that isn't finite fails with BTBH_GPU_INVALID_ARGUMENT. */
BTBHGPUTexture *btbh_gpu_window_heightfield_create(
    BTBHGPUWindow *window,
    const float *heights,
    uint32_t width,
    uint32_t height,
    BTBHGPUTextureReport *report
);

/* A static batch drawn on a heightfield (RFC 0012): after its instance's matrix and the wind, every vertex rises by
 * height_scale * h(u, v) + height_offset, with u = x * world_to_uv[0] + world_to_uv[2] and
 * v = z * world_to_uv[1] + world_to_uv[3] at its flat world x and z. Its normal and tangent tilt with the
 * heightfield's slope there, unless BTBH_GPU_SCENE_HEIGHT_LIFT_KEEP_NORMALS. Its shadows lift the same way. The
 * host owns it; the renderer reads it during the present call only. A lift with heightfield NULL draws as no lift. */
typedef struct BTBHGPUSceneHeightLift {
    BTBHGPUTexture *heightfield;     /* btbh_gpu_window_heightfield_create */
    float world_to_uv[4];            /* u scale, v scale, u offset, v offset; the scales finite and not 0 */
    float height_scale;              /* world units per unit of h; finite. 0 means 1 */
    float height_offset;             /* world units added; finite */
    uint32_t flags;                  /* BTBH_GPU_SCENE_HEIGHT_LIFT_* bits; 0 */
    uint32_t reserved;               /* 0 */
} BTBHGPUSceneHeightLift;

enum {
    /* The normals and tangents stay as the instance's matrix gives them (a lifted object that keeps its shading:
     * one vertex fetch, no tilt). */
    BTBH_GPU_SCENE_HEIGHT_LIFT_KEEP_NORMALS = 1u << 0
};

/* In BTBHGPUSceneGeometryBatch, appended after world_detail_table: */
    /* NULL: drawn as before. Otherwise the batch's vertices are lifted by a heightfield (BTBHGPUSceneHeightLift,
     * RFC 0012). Static batches only: a skinned batch, a billboard or a batch with dug_by and a lift whose
     * heightfield is set fails the present with BTBH_GPU_INVALID_ARGUMENT, naming the batch. A lifted batch
     * draws with production_model_lift.vert (or production_model_detail_table_lift.vert with a world-detail table)
     * and casts with production_shadow_lift.vert, loaded beside the host's production_model.vert and
     * production_shadow.vert; without them its present fails, naming the file. Several batches may point at one. */
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

The batch grows from 280 to 288 bytes; `height_lift` sits at 280.

**Zero values.** `height_lift` NULL draws as before, byte for byte, with today's vertex shaders. So does a lift whose
`heightfield` is NULL. Inside the lift, `height_scale` 0 means 1, so a zeroed lift with a heightfield and its mapping
set adds the heights as they are.

**Refusals** (`BTBH_GPU_INVALID_ARGUMENT`, naming the batch, as RFC 0008's checks do):
- `world_to_uv` not finite or a zero scale; `height_scale` or `height_offset` not finite;
- an unknown flag, `reserved` not 0;
- `heightfield` not a heightfield, of another window or destroyed; a heightfield in any other slot;
- with a heightfield set: a skinned batch, `billboard_mode` not 0, `dug_by` set.

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
    float4 height_lift_scale;   // x height_scale, y height_offset, z flags (bits), w 0
};

// x the lift, yz its slope (d/dx, d/dz) in world units.
float3 btbh_height_lift(float2 world_xz)
{
    const float2 uv = world_xz * height_lift_uv.xy + height_lift_uv.zw;
    const float3 h = height_lift_field.SampleLevel(height_lift_sampler, uv, 0.0f).xyz;
    return float3(h.x * height_lift_scale.x + height_lift_scale.y,
                  h.yz * height_lift_scale.x * height_lift_uv.xy);
}

// In production_model.vert, under BTBH_HEIGHT_LIFT, after the instance's matrix and the wind:
const float3 flat_world = world_position.xyz;           // the motion's previous position starts from here
const float3 lift = btbh_height_lift(flat_world.xz);
world_position.y += lift.x;
// The lift maps (x, y, z) to (x, y + h(x, z), z): tangents go through its Jacobian, normals through its inverse
// transpose. Vertical faces keep their normals; level ones tilt with the slope.
raw_world_normal = float3(raw_world_normal.x - lift.y * raw_world_normal.y,
                          raw_world_normal.y,
                          raw_world_normal.z - lift.z * raw_world_normal.y);
raw_world_tangent.y += lift.y * raw_world_tangent.x + lift.z * raw_world_tangent.z;
```

- **One fetch per vertex.** The gradient is stored beside the height, worked out once on the CPU when the heightfield
  is made, and filtered with it: the normals are smooth across texels. A moving instance under FSR 3 takes a second
  fetch for its previous position. `KEEP_NORMALS` skips the tilt.
- **The previous position** is the flat one minus the instance's motion, lifted at its own xz. A static instance on
  a fixed field gets no motion from the lift.
- **Shadows** (`production_shadow_lift.vert`): the same lift, without the normals.
- **The world-detail table** (0.8.0) and a lift together: `production_model_detail_table_lift.vert` defines both.

### Files

| File | What |
| --- | --- |
| `shaders/gpu/production_height_lift.hlsli` | New, included: the bindings, the uniform block, `btbh_height_lift` |
| `shaders/gpu/production_model_lift.vert.hlsl` | New: `#define BTBH_HEIGHT_LIFT 1` and `production_model.vert.hlsl` |
| `shaders/gpu/production_model_detail_table_lift.vert.hlsl` | New: both defines |
| `shaders/gpu/production_shadow_lift.vert.hlsl` | New: `#define BTBH_HEIGHT_LIFT 1` and `production_shadow.vert.hlsl` |
| `production_model.vert`, `production_model_detail_table.vert`, `production_shadow.vert` | The lift under `#if BTBH_HEIGHT_LIFT`. Without it they compile to 0.8.0's SPIR-V, tested |
| `production_rc_material_source.vert` | No: it includes `production_model.vert` without the define |
| `production_animated_model.vert`, `production_animated_shadow.vert`, `production_shadow_fast.vert` | No: skinned batches can't be lifted, and lifted ones never take the fast shadow |
| Fragment shaders | No: they already read the flat world position and the interpolated normal |

The three new files join `CMakeLists.txt`'s list after their bases, as 0.8.0's table shader did. The core loads them
from the directory of the host's static and shadow vertex shaders, for lifted batches only. CHIMERA's list of 14 and
v2's named shaders don't need them unless they lift.

### The core

- **Pipelines.** Lifted batches get their own static model pipelines (with and without the table, for each shadow
  set) and shadow pipelines (per cull mode, with and without discard), made on first use, as 0.8.0's
  `table_model_pipelines`. `btbh_gpu_window_scene_prewarm` takes batches, so it makes them too.
- **Bindings per lifted batch:** one vertex sampler (the heightfield), the 32-byte block in vertex uniform slot 1.
  Unlifted batches bind as before.
- **The opaque sort** lifts a lifted batch's key by the heightfield at its centre's xz, from the CPU copy.
- **The name check:** a lifted batch needs the lifted files (they are new files, so a missing one fails the present
  with its path, as 0.8.0 does for the table's vertex shader).

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

/* The street pieces: their instances stand at height 0; the renderer lifts every vertex. */
street_batch.height_lift = &lift;
/* Buildings, props and units: lifted by the host as today (rigid), height_lift NULL. */
```

In v2's Rust: one `HeightLift` per stage beside the kit library, `height_lift: lift.map_or(null, ...)` in `batch_of`
for the ground pieces' draws, and the pieces' instances at height 0 instead of their cell's lift.

## Implementation notes for the host

- **No cracks on lattice lines.** With texel centres on the lattice, bilinear filtering is linear along every
  lattice line. Two pieces whose shared edge lies on a lattice line meet without a crack, T-junctions included,
  because every vertex on that line takes the line's linear height. Put the lattice on the kit's cell corners.
- **Between vertices the triangle is flat.** A cell of two triangles bends along its diagonal, not as the bilinear
  surface: they differ by up to a quarter of the cell's twist (h00 + h11 − h01 − h10) / 4 at its middle. An overlay
  triangulated another way over the same cell (a pool at 0.0585 over the street, a scar, a decal) can dip by that
  much. Overlays need the same lift and the same triangulation, or a twist smaller than their height over the
  ground.
- **Culling.** The host's bounds must span the lift: v2's chunks carry their lowest lift (`base`, since `361be68d`);
  with the renderer's lift they need the field's range over the chunk.
- **Hosts that lift on the CPU** (units, props, the camera) read the same heights with `Heightfield::at`: they agree
  with the GPU within the filter weights' precision (1/256 of a texel's height change).
- **Radiance cascades.** The trace counts a street receiver at height 0 whatever the ground (RFC 0010, "How a ray
  meets them"). On Height terrain, occluder tops given over the ground beneath them shade the street round a hill's
  houses as on flat ground. With RFC 0010's tops, a lifted material source's top is its height before the lift,
  which agrees with that.

## Cost

Estimates, to measure with the frame bench (`btbh_gpu_frame_bench`) and a lifted variant of its kit pieces.

| Item | Estimate |
| --- | --- |
| Unlifted batches | Nothing: today's vertex shaders, byte for byte in SPIR-V |
| Lifted vertex | One bilinear RGBA16F fetch and about 25 ALU (the tilt); a second fetch for a moving instance under FSR 3 |
| Lifted shadow vertex | One fetch, a few ALU; and the full shadow shader instead of the fast one |
| 100,000 lifted vertices a frame | About 0.001 ms of fetches on the bench's RTX 3070 Ti (170 billion bilinear samples a second), 0.004–0.006 ms on a Deck (16–26 billion) |
| CPU per lifted batch | One sampler binding and a 32-byte push; the sort key's one bilinear read |
| Memory | 8 bytes a texel on the GPU and 4 on the CPU: a 512 × 512 field is 2 MiB and 1 MiB |

The fetches are cheap. What a smooth slope costs is the vertices a piece needs to follow it: a street tile of two
triangles a cell follows the slope cell by cell; a finer bend needs more vertices, and those cost what vertices cost.

**How to measure it.**
- The frame bench with its pieces on a heightfield: unlifted, lifted, lifted with `KEEP_NORMALS`, and lifted with
  shadows cast; against 0.8.0's shaders for the unlifted case.
- v2's Height terrain at the map's middle on the Deck preset: per-cell lift against the renderer's lift.

## Compatibility

- **Versioning:** minor. New function, struct, flag and macro; a pointer appended to the batch (280 → 288 bytes);
  three new shader files. Every game compiles the C core from the checkout whose header it builds against, so they
  agree on the size.
- **Shaders.** The changed sources compile to 0.8.0's SPIR-V without the define. 0.8.0's shaders under the new core
  draw everything but lifted batches as before; a lifted batch fails the present naming the missing lifted file.
- **Be The Bullet Hell v2 (Rust, bindgen):** nothing changes until it opts in. bindgen gives the new structs
  `Default`, and `batch_of` ends in `..Default::default()`, so `height_lift` is null. To adopt: bump the pin,
  build the three lifted shaders, make the heightfield, set the lift on the ground pieces' batches, and stand their
  instances at 0.
- **Be The Bullet Hell (v1, Swift):** nothing changes. Batches are built zeroed. A `MaterialDescriptor.heightLift`
  can follow when it asks; its SDF terrain (RFC 0007) can't be combined with a lift in phase 1.
- **CHIMERA (Rust, bindgen):** nothing changes; it regenerates the bindings for the bigger batch. Its list of 14
  shaders can stay.
- **Tyrian3D (C):** nothing changes. `init_batch` memsets the batch. Pinned at v0.2.1.

## Platforms and testing

- **Native:**
  - `btbh_gpu_scene_order_test`: `offsetof(height_lift) == 280`, the batch 288 bytes, `sizeof(BTBHGPUSceneHeightLift)
    == 40` and its offsets.
  - `btbh_gpu_height_lift_test` (or in the world detail test): the C copy of the lift and the tilt. A plane's lift
    and normal exactly; a vertical face's normal unchanged; tangent and normal orthogonal after the tilt; the
    gradient's central differences and edges; the mapping of a v2-style lattice onto texel centres; continuity along
    lattice lines.
  - The refusals.
- **GPU smoke (Windows/Vulkan):**
  - **Unlifted, byte for byte:** the galleries and suites as with 0.8.0's core and shaders, with `height_lift` NULL
    and with a lift whose heightfield is NULL; and `production_model.vert`, `production_model_detail_table.vert` and
    `production_shadow.vert` compiled to 0.8.0's SPIR-V.
  - **A flat field:** a lift of constant c draws as the batch moved up by c (within the filter's rounding).
  - **A slope:** a tessellated plane on a tilted field against the same plane tilted on the CPU: positions, normals
    (lit with a sun) and the shadow it casts and takes.
  - **Seams:** two street tiles and a T-junction on a lattice line show no crack (no background pixel between them).
  - **FSR 3:** a static lifted batch writes zero motion; a moving one writes the CPU's expected motion.
  - **With the world-detail table, and transparent:** both draw.
  - **Old shaders:** a lifted batch with a shader directory without the lifted files fails with the file's path.
- **Bench:** as above.
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
| **A shear by the instance** (the host gives each instance a slope, the vertex shader shears the piece) | A plane per piece: smooth inside a piece, a crease at every piece's edge where the slope changes. No texture, but not "follows the slope" |
| **Tessellation or mesh shaders** | SDL's GPU API has neither |
| **The SDF terrain's heightfield base** (RFC 0007) | It meshes its own ground; kit pieces (streets, kerbs, markings) placed on it would still need lifting |
| **Lifting skinned batches and billboards now** | Units and props stand on the ground rigidly (the host lifts their instance); bending a skinned body by the slope would look wrong. Phase 2 if a game asks |

## Open questions for v2

1. **Which batches.** The ground pieces only (streets, plazas, kerbs, markings, parks), with buildings, props and units
   lifted rigidly by you as today? Do your overlays (the night's pools at 0.0585, scars, decals) lift the same way?
2. **The field.** What lattice and extent will you give: one field for the map, the ring and the hills, at one cell?
   Is 4096 a side enough, and are heights within a few hundred units (half floats)?
3. **Geometry.** Do your ground pieces have vertices on every lattice line? Are the overlays triangulated like the
   ground under them, or is the twist small enough for their 0.0585?
4. **Normals.** The tilt by the slope is the default. Is `KEEP_NORMALS` useful for anything you lift?
5. **Shadows.** Do lifted ground pieces cast (most ground sets `NO_SUN_SHADOW`)? A lifted caster takes the full
   shadow shader, not the fast one.
6. **Changes.** Does the field change during a stage? A change shows no motion to FSR 3; phase 2 would add an update
   call that records it.
7. **Radiance cascades.** On Height terrain, will you give the occluders' tops over the ground beneath them, so the
   cascades see houses on a hill as on flat ground?

## Phases

1. **Phase 1, the lift for static batches (0.10.0).** `btbh_gpu_window_heightfield_create`, the struct, the pointer,
   the three vertex shader files and the include, the pipelines, the refusals, the C copy, the tests and the bench.
2. **Phase 2, only if asked.** A heightfield update (with the motion it causes), lifted skinned batches and billboards,
   the radiance-cascade material sources lifted (for RFC 0010's tops), and the SDF terrain with a lift.
