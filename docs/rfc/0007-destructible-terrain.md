# RFC 0007: Destructible terrain from SDF edits

- **Status:** Draft
- **Requested by:** Be The Bullet Hell, CHIMERA (proposed by the coordinator)
- **Affects:** Be The Bullet Hell and CHIMERA (each adopts it on its own). Tyrian3D: nothing, it is opt-in
- **Issue:** to be opened
- **Depends on:** nothing to start. [RFC 0001](0001-layered-terrain.md) (layered terrain) for the full
  material look (phase 4)

## Need

Both games destroy things on the ground, but the ground itself never changes.

- **Be The Bullet Hell.** A kaiju tears through a city. Buildings shatter and vanish, and the ground stays
  untouched. That ground is about 259k flat instanced tiles (roads, lawns, paving), static after generation.
  Wanted:
  - craters where the kaiju stomps or a nuke lands;
  - ruts and trenches where it drags or digs;
  - rubble mounds where a building fell;
  - torn asphalt with soil showing in the walls.

  Gameplay may later treat a deep crater as a hole: the game already has holes in its nav grid (water).
- **CHIMERA.** The GDD asks for "rane u tlu; tragovi bitke ostaju" (wounds in the ground; battle traces
  stay) and for dynamic passability. Today it has scorch decals (35 s), decorative "old battle" craters and
  ruins that slump only in the render. Wanted:
  - artillery and siege craters that stay for the whole battle;
  - collapsed rock and bridges;
  - craters that can feed `Nav::set_walk`, which already updates passability incrementally.

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
   and distance queries from the authoritative edit list.
2. **The dig mask.** A per-batch pointer. The host's own ground (BTBH's tiles, CHIMERA's chunk meshes) stays
   as it is, but its fragments that lie above the dug surface are discarded. The terrain only draws
   *patches* where the ground changed, and nothing needs to rebuild or reproduce the host's ground.
3. **Vertex layer weights and triplanar sampling** for RFC 0001 terrain batches. A crater wall shows
   subsurface layers (soil, rock) and its steep faces don't stretch the textures.

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

typedef struct BTBHGPUSdfEdit {
    BTBHGPUSdfShape shape;
    BTBHGPUSdfOperation operation;
    float position[3];
    float rotation[4];       /* quaternion x, y, z, w; all zero means identity */
    float size[3];
    float corner_radius;
    float smoothing;         /* smooth-min radius in world units; 0 = a sharp edge */
    /* Material written where this edit decides the surface: the layer index (0..7) of the terrain
     * material. Ignored by SUBTRACT unless material_band > 0. */
    uint8_t material;
    uint8_t reserved[3];
    /* SUBTRACT and PAINT: how far under the new surface `material` reaches (scorched walls, ash).
     * 0: SUBTRACT exposes whatever the base has at that depth (soil under asphalt). */
    float material_band;
} BTBHGPUSdfEdit;

/* The ground before any edit, in batches of points (a brick at a time, so a call across FFI is cheap):
 * the signed distance of each point to the base surface (negative inside the ground) and the material
 * at that point. Only called where an edit reaches. */
typedef void (*BTBHGPUSdfTerrainBaseFunction)(
    void *user,
    const float *points,     /* count x (x, y, z) */
    uint32_t count,
    float *distances,        /* count */
    uint8_t *materials       /* count */
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
    uint8_t reserved[2];
    float base_subsurface_depth;
    BTBHGPUSdfTerrainBaseFunction base_function;
    void *base_user;
} BTBHGPUSdfTerrainDesc;

typedef struct BTBHGPUSdfTerrainReport {
    BTBHGPUResult result;
    uint32_t bricks_evaluated;   /* by the last update */
    uint32_t chunks_meshed;
    uint32_t pending_bricks;     /* still waiting after the budget ran out */
    uint32_t brick_count;        /* allocated */
    uint64_t mesh_bytes;         /* all chunk meshes */
    char message[256];
} BTBHGPUSdfTerrainReport;

BTBHGPUSdfTerrain *btbh_gpu_window_sdf_terrain_create(
    BTBHGPUWindow *window,
    const BTBHGPUSdfTerrainDesc *desc,
    BTBHGPUSdfTerrainReport *report
);
void btbh_gpu_sdf_terrain_destroy(BTBHGPUSdfTerrain *terrain);

/* Edits apply in the order they were added. An id stays valid until it is removed. Returns 0 when the
 * edit is invalid or max_edits is reached. */
uint32_t btbh_gpu_sdf_terrain_add_edit(BTBHGPUSdfTerrain *terrain, const BTBHGPUSdfEdit *edit);
BTBHGPUResult btbh_gpu_sdf_terrain_set_edit(BTBHGPUSdfTerrain *terrain, uint32_t id,
                                            const BTBHGPUSdfEdit *edit);   /* move, resize: non-destructive */
BTBHGPUResult btbh_gpu_sdf_terrain_remove_edit(BTBHGPUSdfTerrain *terrain, uint32_t id);

/* Evaluates the bricks that edits made stale, meshes the changed chunks and uploads them, until
 * budget_microseconds is spent (0 = no limit). The rest waits for the next call. */
BTBHGPUResult btbh_gpu_sdf_terrain_update(BTBHGPUSdfTerrain *terrain, uint32_t budget_microseconds,
                                          BTBHGPUSdfTerrainReport *report);

/* Queries read the edit list itself, not the bricks, so they are exact and immediate whatever the budget
 * left pending. */
float btbh_gpu_sdf_terrain_distance(const BTBHGPUSdfTerrain *terrain, const float point[3]);
/* The first surface going down from (x, from_y, z); -INFINITY when there is none. */
float btbh_gpu_sdf_terrain_height(const BTBHGPUSdfTerrain *terrain, float x, float z, float from_y);
uint8_t btbh_gpu_sdf_terrain_raycast(const BTBHGPUSdfTerrain *terrain, const float origin[3],
                                     const float direction[3], float max_distance, float hit[3]);
/* A height raster (top surface minus base surface; 0 where nothing changed) for nav, passability or the
 * radiance cascades occlusion field. */
BTBHGPUResult btbh_gpu_sdf_terrain_sample_height_deltas(const BTBHGPUSdfTerrain *terrain,
                                                        float origin_x, float origin_z, float cell,
                                                        uint32_t width, uint32_t depth, float *out);
/* The XZ rectangles (min x, min z, max x, max z) changed since the last call, so the host can redo nav or
 * occlusion only there. Returns how many were written; the rest stay queued. */
uint32_t btbh_gpu_sdf_terrain_take_changes(BTBHGPUSdfTerrain *terrain, float (*rects)[4], uint32_t capacity);

/* Drawing: one ordinary batch per chunk that has a mesh. */
typedef struct BTBHGPUSdfTerrainChunk {
    BTBHGPUCustomMesh *mesh;     /* NULL: nothing changed in this chunk */
    float bounds_min[3];
    float bounds_max[3];
    uint64_t revision;           /* bumps when the mesh changes */
} BTBHGPUSdfTerrainChunk;
uint32_t btbh_gpu_sdf_terrain_chunk_count(const BTBHGPUSdfTerrain *terrain);
uint8_t btbh_gpu_sdf_terrain_chunk(const BTBHGPUSdfTerrain *terrain, uint32_t index,
                                   BTBHGPUSdfTerrainChunk *chunk);
/* The chunk's triangles on the CPU (world space), for a game's own collision or debris. */
BTBHGPUResult btbh_gpu_sdf_terrain_chunk_geometry(const BTBHGPUSdfTerrain *terrain, uint32_t index,
                                                  const float **positions, uint32_t *vertex_count,
                                                  const uint32_t **indices, uint32_t *index_count);
```

**Patches, not the whole ground.** A chunk gets bricks and a mesh only once an edit reaches it. A mesh only
covers the cells where the surface or its material differs from the base, plus a one-cell apron. Where
nothing changed, the host's ground is what you see. A chunk whose edits are all removed frees its bricks and
its mesh, so the ground is back exactly as it was.

### 2. The dig mask

```c
/* In BTBHGPUSceneGeometryBatch, appended: */
    /* NULL: drawn as before. Otherwise the batch is ground that this terrain may dig through: its fragments
     * that lie more than a few centimetres above the terrain's dug surface at their world XZ are discarded,
     * in the colour, depth, shadow and radiance-cascade passes. Where the terrain is untouched, nothing
     * is discarded. */
    const BTBHGPUSdfTerrain *dug_by;
```

The terrain keeps an R16F texture over its bounds' XZ, one texel per cell. Each texel holds the height of the
dug surface, or a sentinel where nothing was dug. A dug batch reads it once per fragment and discards above
it. Mounds and rims need no mask, because they draw above the host's ground. Where a crater wall crosses the
base height, the two surfaces meet at the same height, so they join without a seam.

### 3. Material on patches (RFC 0001 extension)

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

Until RFC 0001 lands, a patch draws with its batch's own albedo, normal and ORM textures. Its vertex colour
then carries the material: the base tint on the surface, darker soil in the walls. That is enough for the
first in-game tests.

## Minimal example

```c
/* CHIMERA: the bake's corner heights as the base; 0.25 m cells; sand on top, soil from 0.4 m down. */
BTBHGPUSdfTerrainDesc desc = {0};
desc.bounds_min[0] = 0.0f;  desc.bounds_min[1] = -24.0f; desc.bounds_min[2] = 0.0f;
desc.bounds_max[0] = 208.0f; desc.bounds_max[1] = 16.0f;  desc.bounds_max[2] = 208.0f;
desc.cell_size = 0.25f;
desc.chunk_cells = 96;                       /* 24 m, as CHIMERA's ground chunks */
desc.base_heights = corner_heights;          /* 417 x 417 */
desc.base_width = desc.base_depth = 416;
desc.base_cell = 0.5f;
desc.base_surface_material = 0;              /* sand layer */
desc.base_subsurface_material = 6;           /* earth layer */
desc.base_subsurface_depth = 0.4f;
BTBHGPUSdfTerrainReport report;
BTBHGPUSdfTerrain *terrain = btbh_gpu_window_sdf_terrain_create(window, &desc, &report);

/* A shell lands: a smooth bowl and a raised rim, with scorched walls. */
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
btbh_gpu_sdf_terrain_add_edit(terrain, &bowl);
btbh_gpu_sdf_terrain_add_edit(terrain, &rim);

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

## Implementation

Phases, each one testable on its own:

1. **Core, no GPU.**
   - **Edits** live in an id-addressed array that keeps their order. A uniform grid of per-chunk edit
     lists replaces the video's BVH: it is simpler, deterministic, and enough for edits that are small
     next to the map. An edit's bounds grow by its smoothing radius.
   - **Bricks** hold 8³ cells: an int8 distance clamped to ±(√3/2)·cell, which is the video's one byte, plus
     a uint8 material and a "differs from base" bit. That is about 1.1 KB per brick, allocated from a pool,
     with a dense per-chunk pointer grid.
   - **Evaluating** a brick means calling the base once for its 729 corner points, then applying the
     chunk's edit list in order.
   - **Determinism:** single-threaded, the same results for the same edit sequence on the same binary.
2. **Meshing.**
   - **Surface nets** with one vertex per cell. The vertex sits on the mass point, then takes one step
     along the analytic gradient. Normals come from the analytic gradient, not from the bricks. Only cells
     marked "differs", plus the apron, are meshed.
   - **Seams** between chunks are watertight because each chunk meshes with a one-cell overlap of shared
     samples.
   - **Vertex layout:** the custom mesh's 96 bytes. The colour carries the fallback tint, and joints and
     weights carry the eight layer weights.
   - **Upload:** `btbh_gpu_custom_mesh_update` today reallocates its buffers whenever the size changes.
     It gains a grow-only capacity, internally or through a new `btbh_gpu_custom_mesh_reserve`.
3. **Dig mask.**
   - **Texture:** the R16F texture, updated in sub-rectangles as bricks change.
   - **Shaders:** the scene, depth, shadow and RC material-source shaders gain a uniform-guarded branch
     that samples it.
   - **Pipelines:** a dug batch takes the shadow pipeline with a fragment stage, not the depth-only one.
     Otherwise the ground over a crater would shadow its floor.
   - **Slots:** the texture slot is chosen at implementation. t8 and t9 are taken since 0.2.0, and RFC
     0001 wants two more.
4. **RFC 0001 extension:** vertex weights and triplanar sampling (one define in the terrain variant).

**Cost (estimates, to measure in the prototype):**

| Item | Estimate |
| --- | --- |
| CPU, one 6 m crater at 0.25 m cells | about 15 bricks of 0.1 ms each (10 overlapping edits), plus about 0.5 ms meshing. About 2 ms on one thread, spread over frames by the budget |
| CPU, a 20 m BTBH stomp at 0.5 m cells | about 60 bricks, about 6 ms |
| GPU | patches are small. A dug batch pays one texture fetch per fragment and loses the depth-only shadow path |
| Memory | about 15–60 KB of bricks and about 100 KB of mesh per crater (96-byte vertices) |
| Memory, dig mask | 1.4 MB for CHIMERA (832², R16F) and 17 MB for BTBH's whole 1440 m map at 0.5 m (2880²). BTBH would rather use a window that follows the kaiju (open question) |

No new device capability: R16F sampling and custom meshes are core on Vulkan and Metal.

## Compatibility

- **Versioning:** minor (additive, opt-in). A new header and source, new functions, and a pointer appended
  to `BTBHGPUSceneGeometryBatch` (zero = `NULL` = as today). Two fields are appended to RFC 0001's struct.
  No shader is renamed.
- **Be The Bullet Hell:**
  - Nothing changes until it opts in. The batch grows by one pointer, and the Swift layer zero-initialises
    it.
  - To adopt it: a base function (flat at the tile height; asphalt over soil), `dug_by` on the ground tile
    batches, and patch batches.
  - The world curvature applies to patches for free (`production_model.vert`).
  - The RC occlusion field can take mounds from `sample_height_deltas`. Craters don't occlude.
- **CHIMERA:**
  - Nothing changes until it opts in. Bindgen picks up the new header.
  - To adopt it: corner heights as the base, `dug_by` on its ground chunks, and patch batches.
  - Units stand at `btbh_gpu_sdf_terrain_height`, not at the static `ground_height` (look only).
  - The simulation stays authoritative. Craters come from sim events, and passability goes through
    `Nav::set_walk` with the game's own rule. A rule may read `sample_height_deltas` if the game accepts the
    same-binary determinism it already relies on.
- **Tyrian3D:** nothing.
- **FSR 3:** a changed chunk pops in a single frame. The host can set `reactive_mode` on a chunk whose
  `revision` changed in the last few frames.

## Platforms and testing

- **Native tests (core, no GPU, run by `tools/check.ps1` and `check.sh`):**
  - The same edits give the same mesh hash.
  - Removing every edit restores an empty mesh set and an empty dig mask.
  - Heights from `height` agree with the mesh within half a cell.
  - Chunk seams are watertight (no boundary edges inside the bounds).
  - The budget splits the work, and the final result is the same.
- **Contract test (GPU):** a crater patch and a dug ground plane render into a readback, compared with a
  reference, on Windows/Vulkan and macOS/Metal.
- **Smoke scenes:** BTBH, a stomp crater on a road crossing. CHIMERA, a shell crater beside a cliff and a
  ramp.

## Alternatives

| Alternative | Why not (now) |
| --- | --- |
| **Ray march the SDF on the GPU** (the video's renderer) | Every pass would need an SDF path: depth, shadow maps, FSR 3 motion, RC material source, render to target. That is a subsystem the size of radiance cascades, for no gain at our camera distances. The clipmaps that make it scale exist for open worlds; our maps are 0.2–1.4 km and bounded |
| **Heightfield deformation only** (2.5D) | Cheaper: no bricks, a 2D grid of height edits with smooth min. It gets most of the crater look from above, but it can't do undercut lips or BTBH's rubble piles of tilted boxes, and it stretches on steep walls. The API above doesn't depend on it, so it stays the fallback if the 3D prototype is too slow |
| **Prefab crater meshes plus the dig mask** | Cheap, but craters don't merge, overlap or erode into trenches, and there is no material stamping |
| **Rebuild the host's ground** (no dig mask) | The terrain would have to reproduce BTBH's 259k textured tiles and CHIMERA's seven layer copies exactly. Seams appear wherever it can't |
| **Stencil instead of the dig mask** | It works in the colour pass only. Shadows and RC don't see it |
| **Each game writes its own** | All three already compile the C core, and the meshing and mask are the same work twice |

## Open questions

- **The seam between host ground and patch** (the apron overlapping the host ground, and the material
  change at a rim's foot). Prototype this first, on a BTBH road.
- **Edit compaction.** A long CHIMERA battle may leave thousands of edits. Should a quiet chunk freeze its
  bricks into its base (lossy, int8), or is the edit cap enough?
- **Materials:** one id per sample (blended only across a cell) or two ids and a blend.
- **Threads:** a host job callback so bricks can evaluate on the game's worker threads.
- **BTBH's mask:** a window that follows the kaiju, or sparse pages.
- **Cells:** 3D bricks in the first prototype, or the heightfield fallback first to have something in
  hand within a week.
