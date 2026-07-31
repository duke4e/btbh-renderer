#include "btbh_gpu/btbh_gpu_resources.h"

#include <stdlib.h>

typedef enum BTBHGPUFrameSlotState {
    BTBH_GPU_FRAME_SLOT_AVAILABLE = 0,
    BTBH_GPU_FRAME_SLOT_ACTIVE = 1,
    BTBH_GPU_FRAME_SLOT_IN_FLIGHT = 2
} BTBHGPUFrameSlotState;

typedef struct BTBHGPUFrameSlot {
    uint8_t *upload_bytes;
    uint8_t *readback_bytes;
    size_t upload_offset;
    size_t readback_offset;
    uint64_t frame_serial;
    uint64_t retire_serial;
    uint32_t generation;
    BTBHGPUFrameSlotState state;
    uint8_t exhausted;
} BTBHGPUFrameSlot;

struct BTBHGPUFramePool {
    BTBHGPUFrameSlot *slots;
    void *upload_allocation;
    void *readback_allocation;
    uint32_t slot_count;
    size_t upload_capacity;
    size_t readback_capacity;
    size_t arena_stride_upload;
    size_t arena_stride_readback;
    BTBHGPUFramePoolStats stats;
};

static int btbh_gpu_is_power_of_two(size_t value)
{
    return value != 0u && (value & (value - 1u)) == 0u;
}

static uintptr_t btbh_gpu_align_address(uintptr_t value, size_t alignment)
{
    const uintptr_t mask = (uintptr_t)(alignment - 1u);
    return (value + mask) & ~mask;
}

static BTBHGPUFrameToken btbh_gpu_make_frame_token(
    uint32_t index,
    uint32_t generation
) {
    return ((uint64_t)generation << 32u) | (uint64_t)(index + 1u);
}

static BTBHGPUResourceResult btbh_gpu_validate_frame_token(
    BTBHGPUFramePool *pool,
    BTBHGPUFrameToken token,
    BTBHGPUFrameSlot **slot
) {
    uint32_t encoded_index;
    uint32_t generation;
    BTBHGPUFrameSlot *candidate;

    if (pool == NULL || slot == NULL ||
        token == BTBH_GPU_INVALID_FRAME_TOKEN) {
        return BTBH_GPU_RESOURCE_INVALID_ARGUMENT;
    }

    encoded_index = (uint32_t)token;
    generation = (uint32_t)(token >> 32u);
    if (encoded_index == 0u || encoded_index > pool->slot_count ||
        generation == 0u) {
        return BTBH_GPU_RESOURCE_STALE_HANDLE;
    }

    candidate = &pool->slots[encoded_index - 1u];
    if (candidate->state != BTBH_GPU_FRAME_SLOT_ACTIVE ||
        candidate->generation != generation) {
        return BTBH_GPU_RESOURCE_STALE_HANDLE;
    }
    *slot = candidate;
    return BTBH_GPU_RESOURCE_OK;
}

static int btbh_gpu_arena_allocation_size(
    uint32_t slot_count,
    size_t capacity,
    size_t *stride,
    size_t *allocation_size
) {
    if (capacity > SIZE_MAX - (BTBH_GPU_FRAME_MAX_ALIGNMENT - 1u)) {
        return 0;
    }
    *stride = capacity + (BTBH_GPU_FRAME_MAX_ALIGNMENT - 1u);
    if ((size_t)slot_count > SIZE_MAX / *stride) {
        return 0;
    }
    *allocation_size = (size_t)slot_count * *stride;
    return 1;
}

BTBHGPUFramePool *btbh_gpu_frame_pool_create(
    uint32_t frame_slot_count,
    size_t upload_capacity,
    size_t readback_capacity
) {
    BTBHGPUFramePool *pool;
    size_t upload_size;
    size_t readback_size;

    if (frame_slot_count == 0u ||
        upload_capacity == 0u ||
        readback_capacity == 0u ||
        (size_t)frame_slot_count > SIZE_MAX / sizeof(BTBHGPUFrameSlot)) {
        return NULL;
    }

    pool = (BTBHGPUFramePool *)calloc(1u, sizeof(*pool));
    if (pool == NULL) {
        return NULL;
    }
    if (!btbh_gpu_arena_allocation_size(
            frame_slot_count,
            upload_capacity,
            &pool->arena_stride_upload,
            &upload_size
        ) ||
        !btbh_gpu_arena_allocation_size(
            frame_slot_count,
            readback_capacity,
            &pool->arena_stride_readback,
            &readback_size
        )) {
        free(pool);
        return NULL;
    }

    pool->slots = (BTBHGPUFrameSlot *)calloc(
        frame_slot_count,
        sizeof(*pool->slots)
    );
    pool->upload_allocation = malloc(upload_size);
    pool->readback_allocation = malloc(readback_size);
    if (pool->slots == NULL ||
        pool->upload_allocation == NULL ||
        pool->readback_allocation == NULL) {
        free(pool->readback_allocation);
        free(pool->upload_allocation);
        free(pool->slots);
        free(pool);
        return NULL;
    }

    pool->slot_count = frame_slot_count;
    pool->upload_capacity = upload_capacity;
    pool->readback_capacity = readback_capacity;
    pool->stats.heap_allocation_count = 4u;

    for (uint32_t index = 0u; index < frame_slot_count; ++index) {
        uint8_t *upload_region =
            (uint8_t *)pool->upload_allocation +
            ((size_t)index * pool->arena_stride_upload);
        uint8_t *readback_region =
            (uint8_t *)pool->readback_allocation +
            ((size_t)index * pool->arena_stride_readback);
        pool->slots[index].upload_bytes = (uint8_t *)btbh_gpu_align_address(
            (uintptr_t)upload_region,
            BTBH_GPU_FRAME_MAX_ALIGNMENT
        );
        pool->slots[index].readback_bytes = (uint8_t *)btbh_gpu_align_address(
            (uintptr_t)readback_region,
            BTBH_GPU_FRAME_MAX_ALIGNMENT
        );
    }
    return pool;
}

void btbh_gpu_frame_pool_destroy(BTBHGPUFramePool *pool)
{
    if (pool == NULL) {
        return;
    }
    free(pool->readback_allocation);
    free(pool->upload_allocation);
    free(pool->slots);
    free(pool);
}

void btbh_gpu_frame_pool_stats(
    const BTBHGPUFramePool *pool,
    BTBHGPUFramePoolStats *stats
) {
    if (pool != NULL && stats != NULL) {
        *stats = pool->stats;
    }
}

BTBHGPUResourceResult btbh_gpu_frame_begin(
    BTBHGPUFramePool *pool,
    uint64_t frame_serial,
    uint64_t completed_serial,
    BTBHGPUFrameToken *token
) {
    if (pool == NULL || token == NULL) {
        return BTBH_GPU_RESOURCE_INVALID_ARGUMENT;
    }

    for (uint32_t index = 0u; index < pool->slot_count; ++index) {
        BTBHGPUFrameSlot *slot = &pool->slots[index];
        const int reusable =
            slot->state == BTBH_GPU_FRAME_SLOT_AVAILABLE ||
            (slot->state == BTBH_GPU_FRAME_SLOT_IN_FLIGHT &&
             slot->retire_serial <= completed_serial);

        if (reusable && slot->exhausted == 0u) {
            if (slot->generation == UINT32_MAX) {
                slot->exhausted = 1u;
                continue;
            }
            slot->generation += 1u;
            slot->state = BTBH_GPU_FRAME_SLOT_ACTIVE;
            slot->frame_serial = frame_serial;
            slot->retire_serial = 0u;
            slot->upload_offset = 0u;
            slot->readback_offset = 0u;
            *token = btbh_gpu_make_frame_token(index, slot->generation);
            return BTBH_GPU_RESOURCE_OK;
        }
    }
    return BTBH_GPU_RESOURCE_FRAME_BUSY;
}

BTBHGPUResourceResult btbh_gpu_frame_allocate(
    BTBHGPUFramePool *pool,
    BTBHGPUFrameToken token,
    BTBHGPUFrameArena arena,
    size_t size,
    size_t alignment,
    BTBHGPUFrameSlice *slice
) {
    BTBHGPUResourceResult result;
    BTBHGPUFrameSlot *slot = NULL;
    uint8_t *bytes;
    size_t *offset;
    size_t capacity;
    size_t aligned_offset;

    if (slice == NULL || size == 0u ||
        !btbh_gpu_is_power_of_two(alignment) ||
        alignment > BTBH_GPU_FRAME_MAX_ALIGNMENT) {
        return BTBH_GPU_RESOURCE_INVALID_ARGUMENT;
    }

    result = btbh_gpu_validate_frame_token(pool, token, &slot);
    if (result != BTBH_GPU_RESOURCE_OK) {
        return result;
    }

    if (arena == BTBH_GPU_FRAME_ARENA_UPLOAD) {
        bytes = slot->upload_bytes;
        offset = &slot->upload_offset;
        capacity = pool->upload_capacity;
    } else if (arena == BTBH_GPU_FRAME_ARENA_READBACK) {
        bytes = slot->readback_bytes;
        offset = &slot->readback_offset;
        capacity = pool->readback_capacity;
    } else {
        return BTBH_GPU_RESOURCE_INVALID_ARGUMENT;
    }

    if (*offset > SIZE_MAX - (alignment - 1u)) {
        return BTBH_GPU_RESOURCE_OVERFLOW;
    }
    aligned_offset = (*offset + (alignment - 1u)) & ~(alignment - 1u);
    if (aligned_offset > capacity || size > capacity - aligned_offset) {
        return BTBH_GPU_RESOURCE_ARENA_EXHAUSTED;
    }

    slice->bytes = bytes + aligned_offset;
    slice->offset = aligned_offset;
    slice->size = size;
    *offset = aligned_offset + size;
    if (arena == BTBH_GPU_FRAME_ARENA_UPLOAD) {
        if (*offset > pool->stats.peak_upload_bytes) {
            pool->stats.peak_upload_bytes = *offset;
        }
    } else if (*offset > pool->stats.peak_readback_bytes) {
        pool->stats.peak_readback_bytes = *offset;
    }
    return BTBH_GPU_RESOURCE_OK;
}

BTBHGPUResourceResult btbh_gpu_frame_end(
    BTBHGPUFramePool *pool,
    BTBHGPUFrameToken token,
    uint64_t retire_serial
) {
    BTBHGPUFrameSlot *slot = NULL;
    const BTBHGPUResourceResult result =
        btbh_gpu_validate_frame_token(pool, token, &slot);

    if (result != BTBH_GPU_RESOURCE_OK) {
        return result;
    }
    if (retire_serial < slot->frame_serial) {
        return BTBH_GPU_RESOURCE_INVALID_ARGUMENT;
    }
    slot->retire_serial = retire_serial;
    slot->state = BTBH_GPU_FRAME_SLOT_IN_FLIGHT;
    return BTBH_GPU_RESOURCE_OK;
}

BTBHGPUResourceResult btbh_gpu_aligned_row_pitch(
    uint32_t width,
    uint32_t bytes_per_pixel,
    uint32_t alignment,
    uint32_t *pitch
) {
    uint64_t row_size;
    uint64_t aligned;

    if (width == 0u || bytes_per_pixel == 0u || pitch == NULL ||
        !btbh_gpu_is_power_of_two((size_t)alignment)) {
        return BTBH_GPU_RESOURCE_INVALID_ARGUMENT;
    }

    row_size = (uint64_t)width * (uint64_t)bytes_per_pixel;
    aligned = (row_size + ((uint64_t)alignment - 1u)) &
        ~((uint64_t)alignment - 1u);
    if (aligned > UINT32_MAX || aligned < row_size) {
        return BTBH_GPU_RESOURCE_OVERFLOW;
    }
    *pitch = (uint32_t)aligned;
    return BTBH_GPU_RESOURCE_OK;
}
