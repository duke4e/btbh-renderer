/* The smokes' custom meshes. From 0.10.0, BTBH_SMOKE_SHARED_MESHES=1 makes them in the window's shared geometry
 * buffers (BTBH_GPU_CUSTOM_MESH_SHARED_BUFFERS, RFC 0011), where they must draw byte for byte as meshes with buffers
 * of their own: an identity run with it set compares its images with a build of 0.9.0's. Before 0.10.0 (no
 * BTBH_GPU_HAS_SHARED_MESH_BUFFERS) the variable does nothing. Only for meshes the smoke never updates. */
#ifndef BTBH_GPU_SMOKE_MESHES_H
#define BTBH_GPU_SMOKE_MESHES_H

#include "btbh_gpu/btbh_gpu.h"

#include <SDL3/SDL_stdinc.h>

static inline BTBHGPUCustomMesh *smoke_custom_mesh_create(
    BTBHGPUWindow *window,
    const void *vertices,
    uint32_t vertex_byte_count,
    uint32_t vertex_count,
    const uint32_t *indices,
    uint32_t index_count,
    BTBHGPUCustomMeshReport *report
) {
#if defined(BTBH_GPU_HAS_SHARED_MESH_BUFFERS)
    const char *shared = SDL_getenv("BTBH_SMOKE_SHARED_MESHES");
    if (shared != NULL && shared[0] == '1') {
        return btbh_gpu_window_custom_mesh_create_ex(window, vertices, vertex_byte_count, vertex_count, indices,
            index_count, BTBH_GPU_CUSTOM_MESH_SHARED_BUFFERS, report);
    }
#endif
    return btbh_gpu_window_custom_mesh_create(window, vertices, vertex_byte_count, vertex_count, indices, index_count,
        report);
}

#endif
