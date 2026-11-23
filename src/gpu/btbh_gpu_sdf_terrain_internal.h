#ifndef BTBH_GPU_SDF_TERRAIN_INTERNAL_H
#define BTBH_GPU_SDF_TERRAIN_INTERNAL_H

#include "btbh_gpu_internal.h"

/* The dig mask of a terrain as the scene samples it (RFC 0007). */
typedef struct BTBHGPUSdfTerrainMaskBinding {
    SDL_GPUTexture *texture;
    SDL_GPUSampler *sampler;
    /* x, y: the lattice's min x and z; z: its floor (min y); w: the cell size (production_dig_mask.hlsli) */
    float origin[4];
    /* x, z: 1 / the mask's extent (world units) along x and z; y: 1 / the cell size; w: the threshold (cells
     * under the patch's top surface the host's ground must lie to still draw) */
    float scale[4];
} BTBHGPUSdfTerrainMaskBinding;

/* The mask of `terrain` when it is attached to `window` and has one; false otherwise (the batch then draws as
 * with dug_by NULL). */
bool btbh_gpu_sdf_terrain_mask_binding(
    const BTBHGPUSdfTerrain *terrain,
    const BTBHGPUWindow *window,
    BTBHGPUSdfTerrainMaskBinding *binding
);

/* btbh_gpu_window_destroy: detaches every terrain still attached to the window. */
void btbh_gpu_window_detach_sdf_terrains(BTBHGPUWindow *window);

/* For the native tests: the CPU mask (one texel per cell column, x-fastest): the texel at (x, z) in cells above
 * the terrain's floor, or 65504 where the host's ground stays; and how many texels are not 65504. */
float btbh_gpu_sdf_terrain_mask_texel(const BTBHGPUSdfTerrain *terrain, uint32_t x, uint32_t z);
uint64_t btbh_gpu_sdf_terrain_mask_texel_count(const BTBHGPUSdfTerrain *terrain);
/* For the native tests: a 64-bit hash of every chunk's current mesh (positions, normals, materials, indices). */
uint64_t btbh_gpu_sdf_terrain_mesh_hash(const BTBHGPUSdfTerrain *terrain);
/* For the native tests: a live sample (int8 distance and material) by its lattice index; false where no brick
 * holds it (the base there). */
bool btbh_gpu_sdf_terrain_sample(const BTBHGPUSdfTerrain *terrain, int32_t x, int32_t y, int32_t z, int8_t *distance,
                                 uint8_t *material);
/* For the native tests: the materials and normals of a chunk's current mesh (NULL when it has none). */
const uint8_t *btbh_gpu_sdf_terrain_chunk_materials(const BTBHGPUSdfTerrain *terrain, uint32_t index);
const float *btbh_gpu_sdf_terrain_chunk_normals(const BTBHGPUSdfTerrain *terrain, uint32_t index);

/* For the benches (since 0.13.0): where the last btbh_gpu_sdf_terrain_update's time went, in microseconds of the
 * calling thread (the brick and mesh jobs' wall time when run_jobs spreads them). */
typedef struct BTBHGPUSdfTerrainUpdateTimes {
    double total;
    double bricks;            /* evaluating stale bricks */
    double meshing;           /* meshing chunks */
    double switching;         /* switching the ready chunks, and their mask tiles */
    double gpu_vertices;      /* the switched chunks' vertices in the GPU layout */
    double mesh_uploads;      /* their custom meshes: updated, made or released */
    double mask_upload;       /* the dig mask: made, uploaded or released */
    uint32_t meshes_updated;
    uint32_t meshes_created;
    uint32_t meshes_queued;   /* of those updated or made, through the window's upload queue */
    uint32_t meshes_same_counts; /* of those updated, with the vertex and index counts they had */
    uint32_t mask_uploads;    /* 0 or 1 */
    uint64_t mesh_upload_bytes;
    uint64_t mask_upload_bytes;
} BTBHGPUSdfTerrainUpdateTimes;
/* Defined when btbh_gpu_sdf_terrain_update_times exists (since 0.13.0). */
#define BTBH_GPU_SDF_TERRAIN_HAS_UPDATE_TIMES 1
void btbh_gpu_sdf_terrain_update_times(const BTBHGPUSdfTerrain *terrain, BTBHGPUSdfTerrainUpdateTimes *times);

#endif
