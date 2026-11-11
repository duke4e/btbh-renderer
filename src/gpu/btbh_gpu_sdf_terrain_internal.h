#ifndef BTBH_GPU_SDF_TERRAIN_INTERNAL_H
#define BTBH_GPU_SDF_TERRAIN_INTERNAL_H

#include "btbh_gpu_internal.h"

/* The dig mask of a terrain as the scene samples it (RFC 0007). */
typedef struct BTBHGPUSdfTerrainMaskBinding {
    SDL_GPUTexture *texture;
    SDL_GPUSampler *sampler;
    /* x, z of the lattice's min corner; y its floor; w the cell size */
    float origin[4];
    /* x, z: 1 / the mask's extent (world units) along x and z; y: 1 / the cell size; w: the discard threshold
     * (cells under the patch's top surface that the host's ground may lie and still draw) */
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

#endif
