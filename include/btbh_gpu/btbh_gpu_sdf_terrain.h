#ifndef BTBH_GPU_SDF_TERRAIN_H
#define BTBH_GPU_SDF_TERRAIN_H

/* Destructible terrain from SDF edits (RFC 0007, since 0.4.0): an ordered list of shapes that add to, cut from,
 * intersect or paint the ground before any damage (the base). The terrain keeps sparse distance bricks only where
 * edits reach and meshes the changed region into patches the host draws as ordinary custom-mesh batches. The
 * host's own ground stays as it is: a batch with BTBHGPUSceneGeometryBatch.dug_by set loses its fragments where
 * a patch has taken over (the dig mask). Everything here is opt-in: a host that never creates a terrain renders as
 * before.
 *
 * Units are world units; "a cell" is BTBHGPUSdfTerrainDesc.cell_size. Positions are flat world positions (before
 * BTBHGPUScenePost.world_curvature bends them for drawing). */

#include "btbh_gpu.h"

#if defined(__cplusplus)
extern "C" {
#endif

/* Defined when this header's API exists (since 0.4.0). */
#define BTBH_GPU_HAS_SDF_TERRAIN 1

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
    /* Keep only what lies inside the shape, within the edit's bounds (the shape's box grown by its smoothing and
     * four cells): the ground in that box but outside the shape goes, and nothing outside the box changes. */
    BTBH_GPU_SDF_OPERATION_INTERSECT = 2,
    BTBH_GPU_SDF_OPERATION_PAINT = 3       /* material only, the shape of the ground is untouched */
} BTBHGPUSdfOperation;

typedef enum BTBHGPUSdfNormals {
    BTBH_GPU_SDF_NORMALS_SMOOTH = 0,   /* shared vertices, normals from the field's gradient */
    BTBH_GPU_SDF_NORMALS_FACETED = 1   /* every triangle has its own three vertices, its face normal and one
                                        * material (the majority of its corners; the lowest of three different
                                        * ones): a low-poly, flat-coloured look. About six times the vertices of
                                        * SMOOTH */
} BTBHGPUSdfNormals;

typedef enum BTBHGPUSdfEditCap {
    BTBH_GPU_SDF_EDIT_CAP_FREEZE = 0,  /* at max_edits, the oldest edits fold into the base (see add_edit);
                                        * add_edit never fails for a valid edit */
    BTBH_GPU_SDF_EDIT_CAP_REJECT = 1   /* at max_edits, add_edit returns 0 */
} BTBHGPUSdfEditCap;

typedef struct BTBHGPUSdfEdit {
    BTBHGPUSdfShape shape;
    BTBHGPUSdfOperation operation;
    float position[3];
    float rotation[4];       /* quaternion x, y, z, w (normalised here); all zero means identity */
    float size[3];
    float corner_radius;     /* BOX and CYLINDER; at most the smallest half extent */
    float smoothing;         /* smooth-min radius in world units; 0 = a sharp edge */
    /* Material written where this edit decides the surface: an index (0..7) into material_colors, and the
     * layer of an RFC 0001 terrain. Ignored by SUBTRACT unless material_band > 0. */
    uint8_t material;
    uint8_t reserved[3];
    /* SUBTRACT and PAINT: how far under the new surface `material` reaches (scorched walls, ash).
     * 0: SUBTRACT exposes whatever the base has at that depth (soil under asphalt); PAINT paints all of the
     * ground inside the shape. */
    float material_band;
} BTBHGPUSdfEdit;

/* The ground before any edit, in batches of points (a brick at a time, so a call across FFI is cheap):
 * the signed distance of each point to the base surface (negative inside the ground; a lower bound of the true
 * distance is fine) and the material at that point. Only called where an edit reaches. With run_jobs it is
 * called from the host's worker threads, so it must be safe to call from several threads at once. */
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
    uint32_t chunk_cells;    /* cells per mesh chunk edge, a multiple of 8 (8..256); 0 = 64 */
    uint32_t max_edits;      /* 0 = 4096 */
    /* The base, either a heightfield (base_heights != NULL) or a function. Heightfield: (width + 1) x
     * (depth + 1) corner heights over bounds xz, row by row along x, base_cell apart (copied at create). The
     * surface material holds down to base_subsurface_depth under the surface, and the subsurface material
     * holds below it. */
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
    uint64_t mesh_bytes;         /* all chunk meshes: their CPU copies, and their GPU buffers when attached */
    uint64_t mask_bytes;         /* the dig mask (its CPU tiles, and its texture when attached); 0 until a
                                  * patch takes over from the host's ground */
    char message[256];
} BTBHGPUSdfTerrainReport;

/* The CPU core: edits, bricks, queries and chunk meshes on the CPU. It needs no window, so headless tests
 * and tools can run the edit list and the queries. NULL (and report->result) for an invalid desc. */
BTBHGPUSdfTerrain *btbh_gpu_sdf_terrain_create(const BTBHGPUSdfTerrainDesc *desc,
                                               BTBHGPUSdfTerrainReport *report);
/* The GPU side on this window's device: the chunk custom meshes and the dig mask. Until it is attached,
 * chunks have no mesh and batches with dug_by draw as with NULL. A terrain is attached to one window at
 * a time; detach it (or destroy it) before destroying the window (destroying the window detaches it too). */
BTBHGPUResult btbh_gpu_window_sdf_terrain_attach(BTBHGPUWindow *window, BTBHGPUSdfTerrain *terrain,
                                                 BTBHGPUSdfTerrainReport *report);
void btbh_gpu_sdf_terrain_detach(BTBHGPUSdfTerrain *terrain);
void btbh_gpu_sdf_terrain_destroy(BTBHGPUSdfTerrain *terrain);   /* detaches first */

/* Edits apply in the order they were added. Returns 0 when the edit is invalid, or at max_edits with
 * edit_cap = reject. An id stays valid until it is removed or frozen.
 *
 * At max_edits with edit_cap = freeze, the chunks the oldest live edit reaches are frozen first: their bricks
 * become their base (an int8 distance and a material per sample: the lossy step), and every edit that reaches a
 * frozen chunk is frozen as a whole. A frozen edit's id stops being valid; it keeps applying, unchanged, in the
 * chunks it reaches that are not frozen. This repeats until a slot is free. A frozen chunk keeps its mesh and its
 * mask; it just can't be undone. */
uint32_t btbh_gpu_sdf_terrain_add_edit(BTBHGPUSdfTerrain *terrain, const BTBHGPUSdfEdit *edit);
/* Move, resize, deepen: non-destructive, and the edit keeps its place in the order. BTBH_GPU_INVALID_ARGUMENT
 * for an invalid edit or an id that was removed or frozen; the host then adds a new edit instead. */
BTBHGPUResult btbh_gpu_sdf_terrain_set_edit(BTBHGPUSdfTerrain *terrain, uint32_t id,
                                            const BTBHGPUSdfEdit *edit);
BTBHGPUResult btbh_gpu_sdf_terrain_remove_edit(BTBHGPUSdfTerrain *terrain, uint32_t id);

/* Evaluates the bricks that edits made stale, meshes the changed chunks and, when attached, uploads them,
 * until budget_microseconds of the calling thread's time is spent (0 = no limit). The rest waits for the
 * next call. Chunks that touch each other switch to their new meshes (and their rectangles of the dig mask) in
 * the same call, once none of them waits for a brick: a half-built crater is never shown. A region that keeps
 * changing faster than the budget rebuilds it switches once it settles. report may be NULL. */
BTBHGPUResult btbh_gpu_sdf_terrain_update(BTBHGPUSdfTerrain *terrain, uint32_t budget_microseconds,
                                          BTBHGPUSdfTerrainReport *report);

/* Queries read the edit list itself, not the bricks, so they are exact and immediate whatever the budget
 * left pending. In a frozen chunk they read its frozen base (within a hundredth of a cell near the surface).
 * The field is the base with every edit applied where the point lies within that edit's bounds (its shape's box
 * grown by its smoothing and four cells), and the base alone outside the terrain's bounds. */
float btbh_gpu_sdf_terrain_distance(const BTBHGPUSdfTerrain *terrain, const float point[3]);
/* The first surface going down from (x, from_y, z) (where the field goes from outside to inside); -INFINITY
 * when there is none. A column no edit reaches returns the base height at once. */
float btbh_gpu_sdf_terrain_height(const BTBHGPUSdfTerrain *terrain, float x, float z, float from_y);
/* The first surface along the ray within max_distance: 1 and its point in hit, else 0. */
uint8_t btbh_gpu_sdf_terrain_raycast(const BTBHGPUSdfTerrain *terrain, const float origin[3],
                                     const float direction[3], float max_distance, float hit[3]);
/* A height raster (top surface minus base surface; 0 where nothing changed), for the radiance cascades
 * occlusion field or a game's own nav rule. Sample (i, k) lies at the cell centre (origin_x + (i + 0.5) * cell,
 * origin_z + (k + 0.5) * cell) and is written to out[k * width + i]. Where no surface is left above the
 * terrain's floor, the floor (bounds_min y) minus the base. */
BTBHGPUResult btbh_gpu_sdf_terrain_sample_height_deltas(const BTBHGPUSdfTerrain *terrain,
                                                        float origin_x, float origin_z, float cell,
                                                        uint32_t width, uint32_t depth, float *out);
/* The XZ rectangles (min x, min z, max x, max z) whose look changed since the last call, so the host can
 * redo grass, scatter, decals, nav or occlusion only there. They are reported when the chunk's new mesh
 * is ready, not when the edit is added. Returns how many were written; the rest stay queued. */
uint32_t btbh_gpu_sdf_terrain_take_changes(BTBHGPUSdfTerrain *terrain, float (*rects)[4], uint32_t capacity);

/* Drawing: one ordinary batch per chunk that has a mesh (a BTBH_GPU_SCENE_GEOMETRY_CUSTOM_MESH batch with an
 * identity instance). The vertices carry world positions, normals, UVs from world x and z, the material's colour
 * and, in the joint and weight slots (bytes 64..95), a weight of 1 for the vertex's material among layers 0..7. */
typedef struct BTBHGPUSdfTerrainChunk {
    BTBHGPUCustomMesh *mesh;     /* NULL: nothing changed in this chunk, or no window attached */
    float bounds_min[3];
    float bounds_max[3];
    uint64_t revision;           /* bumps when the mesh changes */
} BTBHGPUSdfTerrainChunk;
uint32_t btbh_gpu_sdf_terrain_chunk_count(const BTBHGPUSdfTerrain *terrain);
/* index in x-fastest order (x, then y, then z); 0 for an index out of range. */
uint8_t btbh_gpu_sdf_terrain_chunk(const BTBHGPUSdfTerrain *terrain, uint32_t index,
                                   BTBHGPUSdfTerrainChunk *chunk);
/* The chunk's triangles on the CPU (world space), for a game's own collision or debris. Works without a
 * window. The pointers stay valid until the next update. A chunk without a mesh gives zero counts. */
BTBHGPUResult btbh_gpu_sdf_terrain_chunk_geometry(const BTBHGPUSdfTerrain *terrain, uint32_t index,
                                                  const float **positions, uint32_t *vertex_count,
                                                  const uint32_t **indices, uint32_t *index_count);

#if defined(__cplusplus)
}
#endif

#endif
