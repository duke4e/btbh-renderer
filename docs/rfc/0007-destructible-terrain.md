# RFC 0007: Destructible terrain from SDF edits

- **Status:** Implemented in v0.5.0: phases 1 to 3, and a Swift layer (`GPUSdfTerrain`). Accepted on
  2026-11-11. See "Implementation notes".
- **Requested by:** Be The Bullet Hell, CHIMERA (proposed by the coordinator)
- **Affects:** Be The Bullet Hell and CHIMERA (each adopts it on its own). Tyrian3D: nothing, it is opt-in
- **Issue:** [#3](https://github.com/duke4e/btbh-renderer/issues/3). Both games answered on 2026-11-11; their
  answers are folded in below
- **Depends on:** nothing to start. [RFC 0001](0001-layered-terrain.md) (layered terrain) only for CHIMERA's
  layered material look (phase 4, optional)

## Need

Both games destroy things on the ground, but the ground itself never changes.

- **Be The Bullet Hell.** A kaiju tears through a city. Buildings shatter and vanish, and the ground stays
  untouched. That ground is about 259k flat instanced tiles (roads, lawns, paving), static after generation.
  Wanted:
  - craters where the kaiju stomps or a nuke lands (first);
  - rubble mounds of tilted boxes where a building fell (second);
  - ruts and trenches where it drags or digs;
  - torn asphalt with soil showing in the walls.

  Only big events make edits: building collapses, the Corruption nuke wave, boss slams and the kaiju's stomp
  abilities. Footsteps stay decals. The city is regenerated every level, so edits never pile up across
  levels. Gameplay may later treat a deep crater as a hole: the game already has holes in its nav grid
  (water).
- **CHIMERA.** The GDD asks for "rane u tlu; tragovi bitke ostaju" (wounds in the ground; battle traces
  stay). Today it has scorch decals (35 s), decorative "old battle" craters and ruins that slump only in the
  render. Wanted, for the look:
  - artillery and siege craters that stay for the whole battle;
  - collapsed rock and bridges.

  The sim keeps its own list of craters and its own passability (`Nav::set_walk`, as for flooding and ice).
  It never reads the terrain: it must run headless (gates, bot runs, replays, `chimera-tools --lockstep`),
  replays are checked by state hash, and the sim's Rust f32 math would not match a C core built by another
  compiler with its own float contraction.

Neither simulation needs tunnels or walkable overhangs: both sims are 2D or 2.5D. Overhangs may still
appear in the look: undercut crater lips, rubble piles, a collapsed slab.

The reference is Mike Turitzin's engine ([video](https://www.youtube.com/watch?v=il-TXbn5iMA), January
2026). In it, the world is an ordered list of SDF edits: a shape with a transform, a boolean operation and a
smoothing radius. Distances are cached sparsely in 8³ bricks. A spatial index finds the bricks an edit
touches, and only those are evaluated again. Edits stamp their material underground, so a dig shows dirt
in its walls. Edits are non-destructive: you can move or remove one and the ground comes back. He renders
by ray marching the bricks on the GPU and meshes them only for physics (marching cubes in chunks, on CPU
threads).

We take the edit model, the sparse bricks, the incremental rebuild and the material stamping. We leave out
the ray marching and the clipmaps (see Alternatives). Our cameras are top-down, our maps are bounded, and
every pass in the renderer (shadows, HBAO, radiance cascades, FSR 3 motion, render to target) already
works on meshes.

## Proposal

Three parts, all opt-in:

1. **`BTBHGPUSdfTerrain`.** A terrain volume in the C core. The host gives it the ground before any damage
   (the *base*) and adds, moves or removes edits. It keeps sparse distance bricks only where edits reach, and
   meshes the changed region into custom meshes the host draws as ordinary batches. It also answers height
   and distance queries from the edit list. The core needs no window; a window is attached for the GPU side.
2. **The dig mask.** A per-batch pointer. The host's own ground (BTBH's tiles, CHIMERA's chunk meshes) stays
   as it is, but its fragments that lie above the dug surface are discarded. The terrain only draws
   *patches* where the ground changed, and nothing needs to rebuild or reproduce the host's ground.
3. **Materials on patches.** Vertex colours from a per-terrain palette, with smooth or faceted normals. That
   is a complete look (BTBH's). For RFC 0001's layered ground, vertex layer weights and triplanar sampling
   are an optional extension (CHIMERA's).

### 1. The terrain volume

A new header, `include/btbh_gpu/btbh_gpu_sdf_terrain.h`, included at the end of `btbh_gpu.h`, so CHIMERA's
bindgen and the Swift module pick it up. A new source, `src/gpu/btbh_gpu_sdf_terrain.c`, with a bridge file
`Sources/BTBHRendererGPUCore/btbh_gpu_sdf_terrain_bridge.c` for SwiftPM. The CHIMERA and Tyrian3D builds
already compile every `src/gpu/*.c` file.

```c
typedef struct BTBHGPUSdfTerrain BTBHGPUSdfTerrain;

typedef enum BTBHGPUSdfShape {
    BTBH_GPU_SDF_SHAPE_SPHERE = 0,      /* size.x radius */
    BTBH_GPU_SDF_SHAPE_ELLIPSOID = 1,   /* size = radii */
    BTBH_GPU_SDF_SHAPE_BOX = 2,         /* size = half extents; corner_radius rounds it */
    BTBH_GPU_SDF_SHAPE_CAPSULE = 3,     /* along local Y: size.x radius, size.y half length */
    BTBH_GPU_SDF_SHAPE_CYLINDER = 4,    /* along local Y: size.x radius, size.y half height; corner_radius */
    BTBH_GPU_SDF_SHAPE_TORUS = 5        /* in local XZ: size.x major radius, size.y minor radius (crater rims) */
} BTBHGPUSdfShape;

typedef enum BTBHGPUSdfOperation {
    BTBH_GPU_SDF_OPERATION_ADD = 0,        /* union: mounds, rubble, rims, walls */
    BTBH_GPU_SDF_OPERATION_SUBTRACT = 1,   /* craters, trenches, holes */
    BTBH_GPU_SDF_OPERATION_INTERSECT = 2,  /* keep only what lies inside the shape */
    BTBH_GPU_SDF_OPERATION_PAINT = 3       /* material only, the shape of the ground is untouched */
} BTBHGPUSdfOperation;

typedef enum BTBHGPUSdfNormals {
    BTBH_GPU_SDF_NORMALS_SMOOTH = 0,   /* shared vertices, normals from the analytic gradient */
    BTBH_GPU_SDF_NORMALS_FACETED = 1   /* every triangle has its own three vertices, its face normal and one
                                        * material (the majority of its corners): a low-poly, flat-coloured
                                        * look. About six times the vertices of SMOOTH */
} BTBHGPUSdfNormals;

typedef enum BTBHGPUSdfEditCap {
    BTBH_GPU_SDF_EDIT_CAP_FREEZE = 0,  /* at max_edits, the oldest edits fold into the base (see below);
                                        * add_edit never fails for a valid edit */
    BTBH_GPU_SDF_EDIT_CAP_REJECT = 1   /* at max_edits, add_edit returns 0 */
} BTBHGPUSdfEditCap;

typedef struct BTBHGPUSdfEdit {
    BTBHGPUSdfShape shape;
    BTBHGPUSdfOperation operation;
    float position[3];
    float rotation[4];       /* quaternion x, y, z, w; all zero means identity */
    float size[3];
    float corner_radius;
    float smoothing;         /* smooth-min radius in world units; 0 = a sharp edge */
    /* Material written where this edit decides the surface: an index (0..7) into material_colors, and the
     * layer of an RFC 0001 terrain. Ignored by SUBTRACT unless material_band > 0. */
    uint8_t material;
    uint8_t reserved[3];
    /* SUBTRACT and PAINT: how far under the new surface `material` reaches (scorched walls, ash).
     * 0: SUBTRACT exposes whatever the base has at that depth (soil under asphalt). */
    float material_band;
} BTBHGPUSdfEdit;

/* The ground before any edit, in batches of points (a brick at a time, so a call across FFI is cheap):
 * the signed distance of each point to the base surface (negative inside the ground) and the material
 * at that point. Only called where an edit reaches. With run_jobs it is called from the host's worker
 * threads, so it must be safe to call from several threads at once. */
typedef void (*BTBHGPUSdfTerrainBaseFunction)(
    void *user,
    const float *points,     /* count x (x, y, z) */
    uint32_t count,
    float *distances,        /* count */
    uint8_t *materials       /* count */
);

/* Runs job(job_user, i) for every i < count on the host's worker threads and returns once all of them are
 * done. The jobs only touch the terrain's own memory and the base function. */
typedef void (*BTBHGPUSdfTerrainJobFunction)(
    void *host_user,
    uint32_t count,
    void (*job)(void *job_user, uint32_t index),
    void *job_user
);

typedef struct BTBHGPUSdfTerrainDesc {
    float bounds_min[3];     /* the world box the terrain may change */
    float bounds_max[3];
    float cell_size;         /* sample spacing, 0.1..4 world units */
    uint32_t chunk_cells;    /* cells per mesh chunk edge, a multiple of 8; 0 = 64 */
    uint32_t max_edits;      /* 0 = 4096 */
    /* The base, either a heightfield (base_heights != NULL) or a function. Heightfield: (width + 1) x
     * (depth + 1) corner heights over bounds xz, base_cell apart. The surface material holds down to
     * base_subsurface_depth under the surface, and the subsurface material holds below it. */
    const float *base_heights;
    uint32_t base_width;
    uint32_t base_depth;
    float base_cell;
    uint8_t base_surface_material;
    uint8_t base_subsurface_material;
    uint8_t normals;         /* BTBHGPUSdfNormals; 0 = smooth */
    uint8_t edit_cap;        /* BTBHGPUSdfEditCap; 0 = freeze */
    float base_subsurface_depth;
    BTBHGPUSdfTerrainBaseFunction base_function;
    void *base_user;
    /* The vertex colour of each material, linear RGBA, multiplied into the batch's albedo. A material whose
     * four values are all zero is white. */
    float material_colors[8][4];
    /* NULL: all work on the thread that calls update. Otherwise bricks and chunk meshes are built through
     * it; uploads stay on the calling thread. The result is the same either way. */
    BTBHGPUSdfTerrainJobFunction run_jobs;
    void *jobs_user;
} BTBHGPUSdfTerrainDesc;

typedef struct BTBHGPUSdfTerrainReport {
    BTBHGPUResult result;
    uint32_t bricks_evaluated;   /* by the last update */
    uint32_t chunks_meshed;
    uint32_t pending_bricks;     /* still waiting after the budget ran out */
    uint32_t brick_count;        /* allocated */
    uint32_t edit_count;         /* live edits */
    uint32_t frozen_edits;       /* folded into the base so far (edit_cap = freeze) */
    uint64_t mesh_bytes;         /* all chunk meshes */
    uint64_t mask_bytes;         /* the dig mask; 0 until something digs */
    char message[256];
} BTBHGPUSdfTerrainReport;

/* The CPU core: edits, bricks, queries and chunk meshes on the CPU. It needs no window, so headless tests
 * and tools can run the edit list and the queries. */
BTBHGPUSdfTerrain *btbh_gpu_sdf_terrain_create(const BTBHGPUSdfTerrainDesc *desc,
                                               BTBHGPUSdfTerrainReport *report);
/* The GPU side on this window's device: the chunk custom meshes and the dig mask. Until it is attached,
 * chunks have no mesh and batches with dug_by draw as with NULL. A terrain is attached to one window at
 * a time; detach it (or destroy it) before destroying the window. */
BTBHGPUResult btbh_gpu_window_sdf_terrain_attach(BTBHGPUWindow *window, BTBHGPUSdfTerrain *terrain,
                                                 BTBHGPUSdfTerrainReport *report);
void btbh_gpu_sdf_terrain_detach(BTBHGPUSdfTerrain *terrain);
void btbh_gpu_sdf_terrain_destroy(BTBHGPUSdfTerrain *terrain);   /* detaches first */

/* Edits apply in the order they were added. Returns 0 when the edit is invalid, or at max_edits with
 * edit_cap = reject. An id stays valid until it is removed or frozen. */
uint32_t btbh_gpu_sdf_terrain_add_edit(BTBHGPUSdfTerrain *terrain, const BTBHGPUSdfEdit *edit);
/* Move, resize, deepen: non-destructive. BTBH_GPU_INVALID_ARGUMENT for an id that was removed or frozen;
 * the host then adds a new edit instead. */
BTBHGPUResult btbh_gpu_sdf_terrain_set_edit(BTBHGPUSdfTerrain *terrain, uint32_t id,
                                            const BTBHGPUSdfEdit *edit);
BTBHGPUResult btbh_gpu_sdf_terrain_remove_edit(BTBHGPUSdfTerrain *terrain, uint32_t id);

/* Evaluates the bricks that edits made stale, meshes the changed chunks and, when attached, uploads them,
 * until budget_microseconds of the calling thread's time is spent (0 = no limit). The rest waits for the
 * next call. */
BTBHGPUResult btbh_gpu_sdf_terrain_update(BTBHGPUSdfTerrain *terrain, uint32_t budget_microseconds,
                                          BTBHGPUSdfTerrainReport *report);

/* Queries read the edit list itself, not the bricks, so they are exact and immediate whatever the budget
 * left pending. In a frozen chunk they read its frozen base (within a hundredth of a cell). */
float btbh_gpu_sdf_terrain_distance(const BTBHGPUSdfTerrain *terrain, const float point[3]);
/* The first surface going down from (x, from_y, z); -INFINITY when there is none. A column no edit
 * reaches returns the base height at once. */
float btbh_gpu_sdf_terrain_height(const BTBHGPUSdfTerrain *terrain, float x, float z, float from_y);
uint8_t btbh_gpu_sdf_terrain_raycast(const BTBHGPUSdfTerrain *terrain, const float origin[3],
                                     const float direction[3], float max_distance, float hit[3]);
/* A height raster (top surface minus base surface; 0 where nothing changed), for the radiance cascades
 * occlusion field or a game's own nav rule. */
BTBHGPUResult btbh_gpu_sdf_terrain_sample_height_deltas(const BTBHGPUSdfTerrain *terrain,
                                                        float origin_x, float origin_z, float cell,
                                                        uint32_t width, uint32_t depth, float *out);
/* The XZ rectangles (min x, min z, max x, max z) whose look changed since the last call, so the host can
 * redo grass, scatter, decals, nav or occlusion only there. They are reported when the chunk's new mesh
 * is ready, not when the edit is added. Returns how many were written; the rest stay queued. */
uint32_t btbh_gpu_sdf_terrain_take_changes(BTBHGPUSdfTerrain *terrain, float (*rects)[4], uint32_t capacity);

/* Drawing: one ordinary batch per chunk that has a mesh. */
typedef struct BTBHGPUSdfTerrainChunk {
    BTBHGPUCustomMesh *mesh;     /* NULL: nothing changed in this chunk, or no window attached */
    float bounds_min[3];
    float bounds_max[3];
    uint64_t revision;           /* bumps when the mesh changes */
} BTBHGPUSdfTerrainChunk;
uint32_t btbh_gpu_sdf_terrain_chunk_count(const BTBHGPUSdfTerrain *terrain);
uint8_t btbh_gpu_sdf_terrain_chunk(const BTBHGPUSdfTerrain *terrain, uint32_t index,
                                   BTBHGPUSdfTerrainChunk *chunk);
/* The chunk's triangles on the CPU (world space), for a game's own collision or debris. Works without a
 * window. */
BTBHGPUResult btbh_gpu_sdf_terrain_chunk_geometry(const BTBHGPUSdfTerrain *terrain, uint32_t index,
                                                  const float **positions, uint32_t *vertex_count,
                                                  const uint32_t **indices, uint32_t *index_count);
```

**Patches, not the whole ground.** A chunk gets bricks and a mesh only once an edit reaches it. A mesh only
covers the cells where the surface or its material differs from the base, plus a one-cell apron. Where
nothing changed, the host's ground is what you see. A chunk whose edits are all removed frees its bricks and
its mesh, so the ground is back exactly as it was.

**At the edit cap.** With `edit_cap` = freeze (the default), an `add_edit` at `max_edits` first freezes the
chunks the oldest live edit reaches:

- their bricks become their base (the int8 distance and the material: this is the lossy step);
- every edit that reaches a frozen chunk is frozen as a whole: its id stops being valid, and it keeps
  applying, unchanged, in the chunks it reaches that are not frozen;
- this repeats until a slot is free.

A frozen chunk keeps its mesh and its mask; it just can't be undone. The report counts the frozen edits.
BTBH regenerates the city every level and never comes near the cap. CHIMERA merges nearby craters itself
through `set_edit`, and expects hundreds to low thousands of edits in a long battle: under the cap, with the
freeze as a safety net, so a crater the sim has is never missing from the look.

### 2. The dig mask

```c
/* In BTBHGPUSceneGeometryBatch, appended: */
    /* NULL: drawn as before. Otherwise the batch is ground that this terrain may dig through: its fragments
     * that lie more than a few centimetres above the terrain's dug surface at their world XZ are discarded,
     * in the colour, depth, shadow and radiance-cascade passes. Until the terrain has dug anything, the
     * batch draws exactly as with NULL. */
    const BTBHGPUSdfTerrain *dug_by;
```

- **One texture over the whole bounds**, R16F, one texel per cell. Each texel holds the height of the dug
  surface, or a sentinel where nothing was dug. A window that follows the action is not enough for BTBH:
  craters stay for the whole level, and the view (about 4× farther at high growth, the Top-Down and
  hyperbolic frames) and the shadow map reach far from the kaiju. Sparse pages come only if a measurement
  on the Steam Deck says the memory hurts.
- **Created lazily**, on the first edit that digs below the base, so a level without a crater pays nothing:
  no texture, and the dug batches keep their usual pipelines.
- **Flat world position.** A dug fragment compares its flat `world_position` (before `btbh_world_bend`, the
  position lighting and shadows already use) with the mask, so BTBH's world curvature doesn't move the hole
  away from its patch.
- **In step with the patches.** A chunk's new mesh and its rectangle of the mask switch in the same frame,
  once all of its stale bricks are done. A half-built crater is never shown, and the mask never opens a hole
  that the patch doesn't fill yet.

Mounds and rims need no mask, because they draw above the host's ground. Where a crater wall crosses the
base height, the two surfaces meet at the same height, so they join without a seam.

### 3. Materials on patches

**Vertex colours** are a complete look, not a stopgap. Each vertex takes `material_colors[material]`. A patch
batch draws with a plain white albedo (or its own textures), and the colours carry the materials: asphalt
grey on top, soil brown in the walls, scorch near the floor from a SUBTRACT's `material_band`. With
`normals` = faceted, each triangle has one colour and one normal, matching BTBH's low-poly city, where every
face samples one texel of a palette atlas. This is all BTBH needs.

**RFC 0001 extension** (phase 4, for CHIMERA's layered ground):

```c
/* In BTBHGPUSceneTerrain (RFC 0001), appended: */
    /* 0: layer weights from control_textures, as RFC 0001. 1: from the vertex: the eight floats a custom
     * mesh vertex carries at bytes 64..95 (the joint and weight slots, which static meshes don't use) are
     * the weights of layers 0..7. BTBHGPUSdfTerrain meshes fill them from the brick materials. */
    uint8_t vertex_weights;
    /* 0: layer UVs from world XZ, as RFC 0001. 1: triplanar: each layer is sampled on the three world
     * planes and blended by the normal, so crater walls and rubble faces don't stretch. */
    uint8_t triplanar;
```

## Minimal example

```c
/* CHIMERA (numbers confirmed on #3): the bake's corner heights as the base; 0.25 m cells; sand on top,
 * soil from 0.4 m down. */
BTBHGPUSdfTerrainDesc desc = {0};
desc.bounds_min[0] = 0.0f;  desc.bounds_min[1] = -24.0f; desc.bounds_min[2] = 0.0f;
desc.bounds_max[0] = 208.0f; desc.bounds_max[1] = 16.0f;  desc.bounds_max[2] = 208.0f;
desc.cell_size = 0.25f;
desc.chunk_cells = 96;                       /* 24 m, as CHIMERA's ground chunks */
desc.base_heights = corner_heights;          /* 417 x 417, 0.5 m apart */
desc.base_width = desc.base_depth = 416;
desc.base_cell = 0.5f;
desc.base_surface_material = 0;              /* sand layer */
desc.base_subsurface_material = 6;           /* earth layer */
desc.base_subsurface_depth = 0.4f;
BTBHGPUSdfTerrainReport report;
BTBHGPUSdfTerrain *terrain = btbh_gpu_sdf_terrain_create(&desc, &report);   /* headless tools stop here */
btbh_gpu_window_sdf_terrain_attach(window, terrain, &report);

/* A shell lands (the sim has already added it to its own crater list): a smooth bowl and a raised rim,
 * with scorched walls. */
BTBHGPUSdfEdit bowl = {0};
bowl.shape = BTBH_GPU_SDF_SHAPE_ELLIPSOID;
bowl.operation = BTBH_GPU_SDF_OPERATION_SUBTRACT;
bowl.position[0] = x; bowl.position[1] = ground_y + 0.6f; bowl.position[2] = z;
bowl.size[0] = bowl.size[2] = 2.6f; bowl.size[1] = 1.4f;
bowl.smoothing = 0.3f;
bowl.material = 5;                           /* scorched layer */
bowl.material_band = 0.15f;
BTBHGPUSdfEdit rim = {0};
rim.shape = BTBH_GPU_SDF_SHAPE_TORUS;
rim.operation = BTBH_GPU_SDF_OPERATION_ADD;
rim.position[0] = x; rim.position[1] = ground_y - 0.25f; rim.position[2] = z;
rim.size[0] = 2.7f; rim.size[1] = 0.5f;
rim.smoothing = 0.6f;
rim.material = 6;
uint32_t bowl_id = btbh_gpu_sdf_terrain_add_edit(terrain, &bowl);
btbh_gpu_sdf_terrain_add_edit(terrain, &rim);

/* A second shell next to it: the game widens the first crater instead of adding one. */
bowl.size[0] = bowl.size[2] = 3.4f;
if (btbh_gpu_sdf_terrain_set_edit(terrain, bowl_id, &bowl) != BTBH_GPU_OK) {
    btbh_gpu_sdf_terrain_add_edit(terrain, &bowl);   /* it was frozen at the cap */
}

/* Every frame: 1.5 ms of terrain work, then draw. */
btbh_gpu_sdf_terrain_update(terrain, 1500u, &report);
for (uint32_t i = 0; i < btbh_gpu_sdf_terrain_chunk_count(terrain); ++i) {
    BTBHGPUSdfTerrainChunk chunk;
    if (btbh_gpu_sdf_terrain_chunk(terrain, i, &chunk) && chunk.mesh != NULL && visible(&chunk)) {
        BTBHGPUSceneGeometryBatch batch = ground_material_batch;   /* identity instance */
        batch.kind = BTBH_GPU_SCENE_GEOMETRY_CUSTOM_MESH;
        batch.custom_mesh = chunk.mesh;
        push_batch(&batch);
    }
}
for (each host ground batch) { host_batch.dug_by = terrain; }
```

BTBH differs in a few fields: a base function (flat at the tile height, asphalt over soil), 0.5 m cells,
`normals` = faceted, `material_colors` from its palette, a 500–1000 µs budget, and `run_jobs` on its worker
threads. A rubble mound is a few ADD boxes with random rotations under the collapsed building.

## Implementation

Phases, each one testable on its own. The first prototype is phases 1 to 3 on a BTBH road crater; the rubble
mound is the second.

1. **Core, no GPU.**
   - **Creation** needs no window (`btbh_gpu_sdf_terrain_create`); the native tests use the core as it is.
   - **Edits** live in an id-addressed array that keeps their order. A uniform grid of per-chunk edit
     lists replaces the video's BVH: it is simpler, deterministic, and enough for edits that are small
     next to the map. An edit's bounds grow by its smoothing radius.
   - **Bricks** hold 8³ cells: an int8 distance clamped to ±(√3/2)·cell, which is the video's one byte, plus
     a uint8 material and a "differs from base" bit. That is about 1.1 KB per brick, allocated from a pool,
     with a dense per-chunk pointer grid. One material id per sample is enough for both games.
   - **Evaluating** a brick means calling the base once for its 729 corner points, then applying the
     chunk's edit list in order.
   - **Queries:** `height` first checks whether any edit's bounds cover the column (the per-chunk list); if
     none does, it returns the base height. CHIMERA's ~1000 unit-foot calls a frame mostly take that path.
   - - The mask's edge follows cell columns: where a patch's colour differs from the host's ground (a rim's foot in
  soil next to grey asphalt), its outline steps by a cell. That is the "seam between host ground and patch" left
  open below; the GPU test's reference image shows it (`Tests/Fixtures/sdf_terrain/crater.png`).
- The GPU test needed the host's ground in tiles: one 14 m quad can't follow the world curvature (only its
  corners bend), so a hole cut at flat positions no longer lies under the bent patch. BTBH's tiles bend per tile.

**The edit cap** freezes chunks as described above.
   - **Budget:** measured on the calling thread. Results don't depend on it.
   - **Determinism:** the same results for the same edit sequence on the same binary.
2. **Meshing.**
   - **Surface nets** with one vertex per cell. The vertex sits on the mass point, then takes one step
     along the analytic gradient. Only cells marked "differs", plus the apron, are meshed.
   - **Normals:** smooth from the analytic gradient, or faceted (unshared vertices per triangle, the face
     normal, the triangle's majority material).
   - **Seams** between chunks are watertight because each chunk meshes with a one-cell overlap of shared
     samples.
   - **Vertex layout:** the custom mesh's 96 bytes. The colour carries `material_colors`, and joints and
     weights carry the eight layer weights (phase 4).
   - **Jobs:** with `run_jobs`, stale bricks, then the chunks to mesh, go out as one job each. Uploads stay on
     the calling thread. Each brick and chunk is built by the same code either way, so the mesh hash is
     the same.
   - **Upload:** `btbh_gpu_custom_mesh_update` today reallocates its buffers whenever the size changes.
     It gains a grow-only capacity, internally or through a new `btbh_gpu_custom_mesh_reserve`.
3. **Dig mask.**
   - **Texture:** the R16F texture over the bounds, created on the first dig and updated in sub-rectangles
     when a chunk's new mesh switches in.
   - **Shaders:** the scene, depth, shadow and RC material-source shaders gain a uniform-guarded branch
     that samples it at the flat `world_position`.
   - **Shadow pipelines:** the renderer already has shadow pipelines with a fragment stage
     (`shadow_pipelines`, for surfaces that need a discard) beside the depth-only ones
     (`non_discard_shadow_pipelines`, `fast_shadow_pipelines`). A dug batch takes the one with a fragment
     stage once the mask exists. Otherwise the ground over a crater would shadow its floor.
   - **BTBH's ground in the shadow pass:** its floor tiles do cast into the directional shadow map today,
     kerbs included and with no difference by tile type. `productionMapMaterialPatch` leaves `castsShadow`
     at its default (true), and persistent ranges are shadow-visible, even outside the frustum. This was
     read from the source, not measured. Flat ground at one height shadows nothing above it, so the
     cheapest answer is for BTBH to stop its flat floor tiles casting (`castsShadow = false`). That also
     saves shadow-map time today, and inside a crater the patch walls cast the shadows. Kerbs and anything
     raised keep casting and take the fragment-stage pipeline. If BTBH keeps the floor casting, the
     prototype measures that pipeline over the 259k tiles.
   - **Slots:** the texture slot is chosen at implementation. t8 and t9 are taken since 0.2.0, and RFC
     0001 wants two more.
4. **RFC 0001 extension (optional):** vertex weights and triplanar sampling (one define in the terrain
   variant). CHIMERA's; BTBH doesn't need it.

**Cost (estimates, to measure in the prototype):**

| Item | Estimate |
| --- | --- |
| CPU, one 6 m crater at 0.25 m cells | about 15 bricks of 0.1 ms each (10 overlapping edits), plus about 0.5 ms meshing. About 2 ms on one thread, spread over frames by the budget |
| CPU, a 20 m BTBH stomp at 0.5 m cells | about 60 bricks, about 6 ms. At BTBH's 0.5–1 ms main-thread budget that is 6 to 12 frames (0.1–0.2 s at 60 fps); with `run_jobs` on four workers, 2 to 3 frames. Queries are exact meanwhile |
| CPU, height queries | a column no edit reaches: the base lookup, well under a microsecond. Inside an edit's bounds: a short march down the column, a few microseconds. 1000 calls a frame should stay under 0.3 ms |
| GPU | patches are small. A dug batch pays one texture fetch per fragment, and only once something was dug. In the shadow pass it loses the depth-only path; BTBH avoids that for its flat floor tiles by not casting them (see phase 3) |
| Memory, bricks | about 15–60 KB per crater |
| Memory, meshes | smooth: about 100 KB per 6 m crater at 0.25 m cells. Faceted: about 0.6 MB per 6 m crater at 0.25 m, about 1.3 MB per 20 m stomp at 0.5 m (96-byte vertices, three per triangle) |
| Memory, dig mask | 1.4 MB for CHIMERA (832², R16F). 17 MB for BTBH's whole 1440 m map at 0.5 m (2880²), from its first crater on |

No new device capability: R16F sampling and custom meshes are core on Vulkan and Metal.

## Implementation notes (phases 1 to 3, 2026-11-11)

Phases 1 to 3 are implemented in v0.5.0: `include/btbh_gpu/btbh_gpu_sdf_terrain.h`,
`src/gpu/btbh_gpu_sdf_terrain.c`, the dig mask in the scene, unlit, shadow and radiance-cascade material-source
shaders, `Tests/btbh_gpu_sdf_terrain_test.c` (native) and `Tests/btbh_gpu_sdf_terrain_smoke.c` (GPU). The API is
the one above. Where the prototype had to differ from the text above, or measured something the text estimated,
it is written down here.

**Bricks and reach**
- A brick stores and evaluates its own 8³ samples (512 base calls, not 729). The mesher reads its neighbours'
  samples across brick and chunk borders.
- An edit's bounds (its *reach*) are its shape's box grown by its smoothing and **five cells**. A sample can
  differ from the base only within a clamp distance of the shape's box and smoothing, and the mesh reads samples
  up to three cells from a differing one, so every sample a mesh reads lies in an evaluated brick.
- A brick whose interval bound says that every sample clamps to the base's side is kept as a tag, not
  evaluated: the bound comes from the base's samples (or the heightfield's corners), the shapes' Lipschitz
  bounds and the monotone smooth operations.
- **INTERSECT** can't be both local and continuous: kept as is, it would clear the whole map outside the shape.
  It acts within its reach only, so it leaves walls at the reach box wherever that box cuts the ground (a true
  discontinuity of the field). Use it to trim shapes that the box keeps clear of the ground (a slab cut out of a
  floating ball, a mound trimmed from above), or use SUBTRACT.

**Updates and switching**
- Stale bricks are evaluated oldest first; a chunk is meshed once no brick it reads is stale. Chunks that touch
  (26-neighbourhood) and are busy switch together, once none of them waits for a brick or a mesh: a crater over
  four chunks appears in one frame. A region edited faster than its budget rebuilds it switches when it
  settles.
- A work item (one brick, or one chunk's mesh) is never split: a call can overrun its budget by one chunk's
  mesh (about 1 to 3 ms at the sizes below).

**The mesh**
- Surface nets as specified: the mass point, one Newton step along the exact field's gradient (tetrahedral
  differences), clamped to the cell. Quads belong to the chunk of their first sample, and the vertices of the
  cells one under a chunk's low faces are computed identically on both sides, so seams are watertight.
- Surface nets bevel sharp edges and corners (one vertex per cell) by up to about half a cell. Heights agree
  with the mesh within half a cell where the surface is a plane at the cell's scale; at a box's edge the vertical
  gap can be larger. Mesh vertices lie within 0.18 of a cell of the surface in the tests.
- Apron vertices (cells with no differing corner) sink a twentieth of a cell, so the host's ground, which still
  draws there, wins the depth test instead of fighting the patch.
- Triangles are clockwise seen from outside. SDL flips Vulkan's viewport and the scene pipelines take clockwise
  as front, so under a camera built as the Swift layer builds it (right = forward × up, view z along forward)
  those are the front faces of `BTBH_GPU_SCENE_CULL_BACK`. The GPU test draws patches that way.
- Faceted meshes go to the GPU without an index buffer. Vertices carry UVs from world x and z, and the joint and
  weight slots already carry the one-hot layer weights phase 4 will read.

**The dig mask** (different from "Proposal, 2" in what it marks)
- One texel per *cell column*, point-sampled. Where the column's top surface cell differs from the base, the
  patch owns that column, and its texel holds the height (in cells above the terrain's floor) of the lowest point
  of the patch's top surface over the column: the lowest crossing on the column's four vertical edges, in that
  cell and in the cells under it that the surface still passes through and that differ from the base (and in the
  chunk below, where the surface goes on into it). Elsewhere it holds 65504.
- In 0.5.0 a texel took the top surface cell alone. Where a wall drops across a column (a bowl that cuts its rim),
  that is the column's high side, and a host's floor between it and the wall stayed and stuck out over the bowl,
  cut off at the column's edge: a sawtooth along the lip, which Be The Bullet Hell saw from its gameplay camera.
  The same columns are marked either way; only their heights went down.
- A dug batch's fragment is discarded unless it lies **more than a tenth of a cell under** that height. The text
  above discarded only fragments "more than a few centimetres above the dug surface", which keeps the host's
  ground wherever the patch lies at the base's height: at a mound's foot, in PAINT, at a crater's lip. Both
  surfaces then draw at the same depth and fight. Taking the whole patch footprint over removes that, and
  host geometry under a mound (more than a tenth of a cell down) still draws.
- So the mask is created on the first patch of any kind (a crater, a mound's foot, paint), not only on the
  first dig. Mounds and rims do need it at their feet.
- A column whose top surface is the base but which has a patch further down (a cave under unchanged ground)
  is not handled; neither game needs one.
- Slots: the scene's fragment shaders take the mask after each variant's own textures (t10, t9 or t5; t2 in the
  fast unlit and RC material-source shaders) and its placement in a second fragment uniform buffer (b1, space3).
  The shadow fragment shader takes them as t0 and b0. The material uniform layout is unchanged. RFC 0001's two
  textures can follow the mask in the scene shaders.
- The shadow vertex shaders now pass the flat world position to the shadow fragment shader. A dug batch takes
  the fragment-stage shadow pipeline only while its terrain has a mask; other batches choose as before.

**The edit cap**
- Freezing as specified. Near the surface (within half the clamp distance) the frozen terrain's samples equal an
  unfrozen terrain's within two int8 steps. Deeper, a smooth edit added later blends against the clamped frozen
  base, which is the lossy part. Queries in a frozen chunk read its samples trilinearly: where the surface curves
  within a cell (smoothing near the cell size) that moves it by up to about a tenth of a cell (0.12 measured),
  not a hundredth.

**Queries**
- `height` over a column no edit reaches returns the heightfield's height at once; a base function is marched
  (false position: exact at once for flat ground). Starting under a heightfield's surface, there is no surface
  going down: -INFINITY.
- `sample_height_deltas` samples at cell centres (documented in the header).

**Measured costs** (`btbh_gpu_sdf_terrain_test`; Windows, Release, a flat function base, an idle desktop CPU;
ranges over runs)

| Item | Estimate | Measured |
| --- | --- | --- |
| 6 m crater (bowl and rim), 0.25 m cells, smooth | about 2 ms | 2.9–3.4 ms on one thread: bricks 1.6, mesh 1.7 (169 bricks evaluated, 94 kept) |
| same, faceted | | 2.6–3.5 ms |
| 20 m stomp, 0.5 m cells, faceted | about 6 ms | 6.1–8.1 ms on one thread: bricks 2.4, meshes 3.8 (324 bricks, 4 chunks); 2.8–3.3 ms on the calling thread with 4 job threads |
| `height` in a crater / elsewhere | a few µs / under 1 µs | 0.4–0.7 µs / 0.06–0.12 µs |
| Bricks, 6 m crater at 0.25 m | 15–60 KB | 101 KiB (94 bricks) |
| Mesh, 6 m crater at 0.25 m | smooth about 100 KB, faceted about 0.6 MB | smooth 109 KiB CPU + 159 KiB GPU; faceted 267 KiB CPU + 733 KiB GPU |
| Mesh, 20 m stomp at 0.5 m, faceted | about 1.3 MB | 744 KiB CPU + 2.2 MiB GPU (23 448 vertices) |
| Dig mask | 2 bytes a cell column | as estimated for the texture; CPU tiles of chunk_cells² texels for each chunk column with a patch (18–32 KiB in these cases) |

At BTBH's 0.5–1 ms main-thread budget a 20 m stomp takes 7 to 16 frames on one thread and 3 to 7 with four job
threads. Under load (a build running beside it) the same work took about 1.5 times as long. The meshing is the
larger half; the next step if that is too slow is to mesh only the bricks near active ones instead of their
bounding box.

## Compatibility

- **Versioning:** minor (additive, opt-in). A new header and source, new functions, and a pointer appended
  to `BTBHGPUSceneGeometryBatch` (zero = `NULL` = as today). Two fields are appended to RFC 0001's struct.
  No shader is renamed.
- **Be The Bullet Hell:**
  - Nothing changes until it opts in. The batch grows by one pointer, and the Swift layer zero-initialises
    it.
  - To adopt it: a base function (flat at the tile height; asphalt over soil), `dug_by` on the ground tile
    batches, and patch batches with a white albedo, faceted normals and palette `material_colors`.
  - Flat floor tiles stop casting into the directional shadow map (`castsShadow = false`), so a dug batch
    costs nothing extra there (phase 3).
  - Each level: destroy the terrain with the old city and create one for the new city.
  - The world curvature applies to patches for free (`production_model.vert`), and the mask reads the flat
    position.
  - Phase 4 is optional; vertex colours are BTBH's real look.
  - The RC occlusion field can take mounds from `sample_height_deltas`. Craters don't occlude.
  - Later, gameplay: deep craters as nav holes, through `take_changes` and `sample_height_deltas`. The sim
    stays authoritative; for now it is look only.
- **CHIMERA:**
  - Nothing changes until it opts in. Bindgen picks up the new header.
  - To adopt it: corner heights as the base, `dug_by` on its ground chunks, and patch batches.
  - Data goes one way. A sim event (a shell lands, a bridge collapses) adds a crater to the sim's own list
    and applies its passability rule through `Nav::set_walk`. The view sends the same crater to the terrain
    as edits. The sim never reads the terrain, and doesn't need `sample_height_deltas`.
  - On the view side, `take_changes` drives grass, scatter, cover decals and ground detail.
  - Units stand at `btbh_gpu_sdf_terrain_height`, not at the static `ground_height` (look only).
  - Nearby craters merge through `set_edit`; a frozen id falls back to `add_edit`.
  - Headless tools and tests can run the core without a window.
- **Tyrian3D:** nothing.
- **FSR 3:** a changed chunk pops in a single frame. Setting `reactive_mode` on the chunk's batch doesn't
  help: only the transparent pass writes the reactive mask, and `reactive_mode` only affects transparent
  batches. (Corrected in 0.5.0.) For a few frames after a chunk's `revision` changes, the host can instead
  draw the chunk a second time in the transparent pass: unlit, black, additive, with `LESS_EQUAL`. That adds
  nothing to the colour and marks the mask. The Swift layer (`GPUSdfTerrain.reactiveFrameCount`) does this.

## Platforms and testing

- **Native tests (core, no GPU, run by `tools/check.ps1` and `check.sh`):**
  - The same edits give the same mesh hash, with and without `run_jobs`, and whatever the budget.
  - Removing every edit restores an empty mesh set and an empty dig mask.
  - Heights from `height` agree with the mesh within half a cell.
  - Chunk seams are watertight (no boundary edges inside the bounds).
  - At the cap, freeze keeps the surface within int8 precision, retires the oldest ids, and never fails a
    valid edit; reject returns 0.
  - A faceted mesh has one normal and one colour per triangle.
- **Contract test (GPU):** a crater patch and a dug ground plane render into a readback, compared with a
  reference, on Windows/Vulkan and macOS/Metal. A curved world (`world_curvature` > 0) keeps the hole
  under its patch.
- **Smoke scenes:**
  - BTBH, first: a crater on a road crossing (asphalt over soil, kerbs and pavement at the seam). BTBH adds
    a dev step `crater:<x>,<z>,<radius>`, like its `objective:` steps, so `BTBH_DEV_SCRIPT` captures
    reproduce it.
  - BTBH, second: a rubble mound of tilted boxes under a collapsed building.
  - CHIMERA, after the first prototype: a shell crater beside a cliff and a ramp.

## Alternatives

| Alternative | Why not (now) |
| --- | --- |
| **Ray march the SDF on the GPU** (the video's renderer) | Every pass would need an SDF path: depth, shadow maps, FSR 3 motion, RC material source, render to target. That is a subsystem the size of radiance cascades, for no gain at our camera distances. The clipmaps that make it scale exist for open worlds; our maps are 0.2–1.4 km and bounded |
| **Heightfield deformation only** (2.5D) | Cheaper: no bricks, a 2D grid of height edits with smooth min. It gets most of the crater look from above, but it can't make BTBH's rubble of tilted boxes (vertical faces, overhangs) or undercut lips, and it stretches on steep walls. CHIMERA is fine either way, so BTBH's rubble decides: 3D bricks from the start. It stays the fallback if the 3D prototype is too slow |
| **Prefab crater meshes plus the dig mask** | Cheap, but craters don't merge, overlap or erode into trenches, and there is no material stamping |
| **Rebuild the host's ground** (no dig mask) | The terrain would have to reproduce BTBH's 259k textured tiles and CHIMERA's seven layer copies exactly. Seams appear wherever it can't |
| **Stencil instead of the dig mask** | It works in the colour pass only. Shadows and RC don't see it |
| **A mask window that follows the kaiju** | BTBH's craters must stay for the whole level, and the camera and shadow map see far from the kaiju |
| **Derivative normals in the shader** for the faceted look | No extra vertices, but it needs a batch flag in every pass, and the vertex colour still blends across faces, so the flat-coloured look is lost. Kept in reserve if faceted mesh memory hurts |
| **Each game writes its own** | All three already compile the C core, and the meshing and mask are the same work twice |

## Open questions

Still open (each needs the prototype):

- **The seam between host ground and patch** (the apron overlapping the host ground, the material change at
  a rim's foot, kerbs and pavement at a road crossing). The first prototype answers it.
- **Dig mask memory on the Steam Deck:** 17 MB for BTBH's whole map. Sparse pages only if a measurement
  says it hurts.
- **Faceted mesh memory:** about six times smooth. If it hurts, derivative normals are the fallback (see
  Alternatives).
- **BTBH's floor tiles in the shadow pass:** they cast today (read from the source). BTBH decides
  whether they stop casting (recommended) or the prototype measures the fragment-stage shadow pipeline
  over them.

Resolved on #3 (2026-11-11):

- **Edit compaction:** BTBH needs none. CHIMERA wants a safety net, so the cap's behaviour is defined:
  freeze the oldest edits into the base by default, or reject.
- **Materials:** one id per sample (both games).
- **Threads:** an optional host job callback (`run_jobs`). BTBH wants it for its CPU-bound fights; CHIMERA
  is GPU-bound and uses the budget alone.
- **BTBH's mask:** one texture over the whole map, created on the first crater, not a window.
- **Cells:** 3D bricks from the start, because BTBH's rubble needs them. The first prototype is a road
  crater.
- **A window-free core:** creation and the GPU side are split (`create`, then `attach`).
