#ifndef BTBH_GPU_RESOURCES_H
#define BTBH_GPU_RESOURCES_H

#include <stddef.h>
#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

typedef enum BTBHGPUResourceResult {
    BTBH_GPU_RESOURCE_OK = 0,
    BTBH_GPU_RESOURCE_INVALID_ARGUMENT = 1,
    BTBH_GPU_RESOURCE_STALE_HANDLE = 2,
    BTBH_GPU_RESOURCE_TYPE_MISMATCH = 3,
    BTBH_GPU_RESOURCE_REGISTRY_FULL = 4,
    BTBH_GPU_RESOURCE_DEFERRED_QUEUE_FULL = 5,
    BTBH_GPU_RESOURCE_FRAME_BUSY = 6,
    BTBH_GPU_RESOURCE_ARENA_EXHAUSTED = 7,
    BTBH_GPU_RESOURCE_OVERFLOW = 8
} BTBHGPUResourceResult;

typedef enum BTBHGPUResourceKind {
    BTBH_GPU_RESOURCE_INVALID = 0,
    BTBH_GPU_RESOURCE_BUFFER = 1,
    BTBH_GPU_RESOURCE_TEXTURE = 2,
    BTBH_GPU_RESOURCE_SAMPLER = 3,
    BTBH_GPU_RESOURCE_SHADER = 4,
    BTBH_GPU_RESOURCE_GRAPHICS_PIPELINE = 5,
    BTBH_GPU_RESOURCE_COMPUTE_PIPELINE = 6,
    BTBH_GPU_RESOURCE_TRANSFER_BUFFER = 7
} BTBHGPUResourceKind;

typedef uint64_t BTBHGPUResourceHandle;

#define BTBH_GPU_INVALID_RESOURCE_HANDLE UINT64_C(0)

typedef struct BTBHGPUResourceRegistry BTBHGPUResourceRegistry;

typedef void (*BTBHGPUResourceReleaseFn)(
    BTBHGPUResourceKind kind,
    uintptr_t payload,
    void *context
);

BTBHGPUResourceRegistry *btbh_gpu_resource_registry_create(
    uint32_t capacity,
    uint32_t deferred_capacity
);

void btbh_gpu_resource_registry_destroy(
    BTBHGPUResourceRegistry *registry,
    BTBHGPUResourceReleaseFn release_fn,
    void *context
);

BTBHGPUResourceResult btbh_gpu_resource_insert(
    BTBHGPUResourceRegistry *registry,
    BTBHGPUResourceKind kind,
    uintptr_t payload,
    BTBHGPUResourceHandle *handle
);

BTBHGPUResourceResult btbh_gpu_resource_lookup(
    const BTBHGPUResourceRegistry *registry,
    BTBHGPUResourceHandle handle,
    BTBHGPUResourceKind expected_kind,
    uintptr_t *payload
);

BTBHGPUResourceResult btbh_gpu_resource_retire(
    BTBHGPUResourceRegistry *registry,
    BTBHGPUResourceHandle handle,
    BTBHGPUResourceKind expected_kind,
    uint64_t retire_serial
);

uint32_t btbh_gpu_resource_collect(
    BTBHGPUResourceRegistry *registry,
    uint64_t completed_serial,
    BTBHGPUResourceReleaseFn release_fn,
    void *context
);

typedef enum BTBHGPUFrameArena {
    BTBH_GPU_FRAME_ARENA_UPLOAD = 1,
    BTBH_GPU_FRAME_ARENA_READBACK = 2
} BTBHGPUFrameArena;

#define BTBH_GPU_FRAME_MAX_ALIGNMENT ((size_t)256u)

typedef uint64_t BTBHGPUFrameToken;

#define BTBH_GPU_INVALID_FRAME_TOKEN UINT64_C(0)

typedef struct BTBHGPUFrameSlice {
    void *bytes;
    size_t offset;
    size_t size;
} BTBHGPUFrameSlice;

typedef struct BTBHGPUFramePoolStats {
    uint32_t heap_allocation_count;
    size_t peak_upload_bytes;
    size_t peak_readback_bytes;
} BTBHGPUFramePoolStats;

typedef struct BTBHGPUFramePool BTBHGPUFramePool;

BTBHGPUFramePool *btbh_gpu_frame_pool_create(
    uint32_t frame_slot_count,
    size_t upload_capacity,
    size_t readback_capacity
);

void btbh_gpu_frame_pool_destroy(BTBHGPUFramePool *pool);

void btbh_gpu_frame_pool_stats(
    const BTBHGPUFramePool *pool,
    BTBHGPUFramePoolStats *stats
);

BTBHGPUResourceResult btbh_gpu_frame_begin(
    BTBHGPUFramePool *pool,
    uint64_t frame_serial,
    uint64_t completed_serial,
    BTBHGPUFrameToken *token
);

BTBHGPUResourceResult btbh_gpu_frame_allocate(
    BTBHGPUFramePool *pool,
    BTBHGPUFrameToken token,
    BTBHGPUFrameArena arena,
    size_t size,
    size_t alignment,
    BTBHGPUFrameSlice *slice
);

BTBHGPUResourceResult btbh_gpu_frame_end(
    BTBHGPUFramePool *pool,
    BTBHGPUFrameToken token,
    uint64_t retire_serial
);

BTBHGPUResourceResult btbh_gpu_aligned_row_pitch(
    uint32_t width,
    uint32_t bytes_per_pixel,
    uint32_t alignment,
    uint32_t *pitch
);

#if defined(__cplusplus)
}
#endif

#endif
