#include "btbh_gpu/btbh_gpu_resources.h"

#include <assert.h>
#include <stdint.h>

typedef struct ReleaseLog {
    uint32_t count;
    BTBHGPUResourceKind last_kind;
    uintptr_t last_payload;
} ReleaseLog;

static void record_release(
    BTBHGPUResourceKind kind,
    uintptr_t payload,
    void *context
) {
    ReleaseLog *log = (ReleaseLog *)context;
    log->count += 1;
    log->last_kind = kind;
    log->last_payload = payload;
}

static void test_stale_handle_and_deferred_release(void)
{
    BTBHGPUResourceRegistry *registry =
        btbh_gpu_resource_registry_create(2, 4);
    BTBHGPUResourceHandle first = BTBH_GPU_INVALID_RESOURCE_HANDLE;
    BTBHGPUResourceHandle replacement = BTBH_GPU_INVALID_RESOURCE_HANDLE;
    uintptr_t payload = 0;
    ReleaseLog log = { 0 };

    assert(registry != NULL);
    assert(
        btbh_gpu_resource_insert(
            registry,
            BTBH_GPU_RESOURCE_TEXTURE,
            (uintptr_t)0x111u,
            &first
        ) == BTBH_GPU_RESOURCE_OK
    );
    assert(
        btbh_gpu_resource_lookup(
            registry,
            first,
            BTBH_GPU_RESOURCE_TEXTURE,
            &payload
        ) == BTBH_GPU_RESOURCE_OK
    );
    assert(payload == (uintptr_t)0x111u);
    assert(
        btbh_gpu_resource_retire(
            registry,
            first,
            BTBH_GPU_RESOURCE_TEXTURE,
            5
        ) == BTBH_GPU_RESOURCE_OK
    );
    assert(
        btbh_gpu_resource_lookup(
            registry,
            first,
            BTBH_GPU_RESOURCE_TEXTURE,
            &payload
        ) == BTBH_GPU_RESOURCE_STALE_HANDLE
    );
    assert(
        btbh_gpu_resource_retire(
            registry,
            first,
            BTBH_GPU_RESOURCE_TEXTURE,
            5
        ) == BTBH_GPU_RESOURCE_STALE_HANDLE
    );

    assert(
        btbh_gpu_resource_insert(
            registry,
            BTBH_GPU_RESOURCE_TEXTURE,
            (uintptr_t)0x222u,
            &replacement
        ) == BTBH_GPU_RESOURCE_OK
    );
    assert(replacement != first);
    assert(
        btbh_gpu_resource_collect(registry, 4, record_release, &log) == 0
    );
    assert(log.count == 0);
    assert(
        btbh_gpu_resource_collect(registry, 5, record_release, &log) == 1
    );
    assert(log.count == 1);
    assert(log.last_kind == BTBH_GPU_RESOURCE_TEXTURE);
    assert(log.last_payload == (uintptr_t)0x111u);

    btbh_gpu_resource_registry_destroy(registry, record_release, &log);
    assert(log.count == 2);
    assert(log.last_payload == (uintptr_t)0x222u);
}

static void test_type_mismatch_and_resize_recreation(void)
{
    BTBHGPUResourceRegistry *registry =
        btbh_gpu_resource_registry_create(2, 2);
    BTBHGPUResourceHandle old_texture = 0;
    BTBHGPUResourceHandle resized_texture = 0;
    uintptr_t payload = 0;
    ReleaseLog log = { 0 };

    assert(registry != NULL);
    assert(
        btbh_gpu_resource_insert(
            registry,
            BTBH_GPU_RESOURCE_TEXTURE,
            (uintptr_t)0x1000u,
            &old_texture
        ) == BTBH_GPU_RESOURCE_OK
    );
    assert(
        btbh_gpu_resource_lookup(
            registry,
            old_texture,
            BTBH_GPU_RESOURCE_BUFFER,
            &payload
        ) == BTBH_GPU_RESOURCE_TYPE_MISMATCH
    );
    assert(
        btbh_gpu_resource_retire(
            registry,
            old_texture,
            BTBH_GPU_RESOURCE_TEXTURE,
            12
        ) == BTBH_GPU_RESOURCE_OK
    );
    assert(
        btbh_gpu_resource_insert(
            registry,
            BTBH_GPU_RESOURCE_TEXTURE,
            (uintptr_t)0x2000u,
            &resized_texture
        ) == BTBH_GPU_RESOURCE_OK
    );
    assert(resized_texture != old_texture);
    assert(
        btbh_gpu_resource_lookup(
            registry,
            old_texture,
            BTBH_GPU_RESOURCE_TEXTURE,
            &payload
        ) == BTBH_GPU_RESOURCE_STALE_HANDLE
    );
    assert(
        btbh_gpu_resource_lookup(
            registry,
            resized_texture,
            BTBH_GPU_RESOURCE_TEXTURE,
            &payload
        ) == BTBH_GPU_RESOURCE_OK
    );
    assert(payload == (uintptr_t)0x2000u);
    assert(
        btbh_gpu_resource_collect(registry, 11, record_release, &log) == 0
    );
    assert(
        btbh_gpu_resource_collect(registry, 12, record_release, &log) == 1
    );

    btbh_gpu_resource_registry_destroy(registry, record_release, &log);
}

static void test_fixed_registry_and_deferred_capacities(void)
{
    BTBHGPUResourceRegistry *registry =
        btbh_gpu_resource_registry_create(2, 1);
    BTBHGPUResourceHandle first = BTBH_GPU_INVALID_RESOURCE_HANDLE;
    BTBHGPUResourceHandle second = BTBH_GPU_INVALID_RESOURCE_HANDLE;
    BTBHGPUResourceHandle rejected = BTBH_GPU_INVALID_RESOURCE_HANDLE;
    uintptr_t payload = 0u;
    ReleaseLog log = { 0 };

    assert(registry != NULL);
    assert(
        btbh_gpu_resource_insert(
            registry,
            BTBH_GPU_RESOURCE_BUFFER,
            (uintptr_t)0x10u,
            &first
        ) == BTBH_GPU_RESOURCE_OK
    );
    assert(
        btbh_gpu_resource_insert(
            registry,
            BTBH_GPU_RESOURCE_BUFFER,
            (uintptr_t)0x20u,
            &second
        ) == BTBH_GPU_RESOURCE_OK
    );
    assert(
        btbh_gpu_resource_insert(
            registry,
            BTBH_GPU_RESOURCE_BUFFER,
            (uintptr_t)0x30u,
            &rejected
        ) == BTBH_GPU_RESOURCE_REGISTRY_FULL
    );
    assert(rejected == BTBH_GPU_INVALID_RESOURCE_HANDLE);

    assert(
        btbh_gpu_resource_retire(
            registry,
            first,
            BTBH_GPU_RESOURCE_BUFFER,
            3
        ) == BTBH_GPU_RESOURCE_OK
    );
    assert(
        btbh_gpu_resource_retire(
            registry,
            second,
            BTBH_GPU_RESOURCE_BUFFER,
            4
        ) == BTBH_GPU_RESOURCE_DEFERRED_QUEUE_FULL
    );
    assert(
        btbh_gpu_resource_lookup(
            registry,
            second,
            BTBH_GPU_RESOURCE_BUFFER,
            &payload
        ) == BTBH_GPU_RESOURCE_OK
    );
    assert(payload == (uintptr_t)0x20u);
    assert(btbh_gpu_resource_collect(registry, 3, NULL, NULL) == 0u);
    assert(
        btbh_gpu_resource_collect(registry, 3, record_release, &log) == 1u
    );
    assert(
        btbh_gpu_resource_retire(
            registry,
            second,
            BTBH_GPU_RESOURCE_BUFFER,
            4
        ) == BTBH_GPU_RESOURCE_OK
    );

    btbh_gpu_resource_registry_destroy(registry, record_release, &log);
    assert(log.count == 2u);
}

static void test_frame_slots_and_fixed_arenas(void)
{
    BTBHGPUFramePool *pool = btbh_gpu_frame_pool_create(2, 128, 64);
    BTBHGPUFrameToken first = BTBH_GPU_INVALID_FRAME_TOKEN;
    BTBHGPUFrameToken second = BTBH_GPU_INVALID_FRAME_TOKEN;
    BTBHGPUFrameToken recycled = BTBH_GPU_INVALID_FRAME_TOKEN;
    BTBHGPUFrameSlice slice;
    BTBHGPUFramePoolStats before;
    BTBHGPUFramePoolStats after;

    assert(pool != NULL);
    btbh_gpu_frame_pool_stats(pool, &before);
    assert(before.heap_allocation_count == 4);

    assert(
        btbh_gpu_frame_begin(pool, 1, 0, &first) ==
        BTBH_GPU_RESOURCE_OK
    );
    assert(
        btbh_gpu_frame_allocate(
            pool,
            first,
            BTBH_GPU_FRAME_ARENA_UPLOAD,
            12,
            16,
            &slice
        ) == BTBH_GPU_RESOURCE_OK
    );
    assert(slice.offset == 0);
    assert(slice.size == 12);
    assert(((uintptr_t)slice.bytes & (uintptr_t)15u) == 0u);
    assert(
        btbh_gpu_frame_allocate(
            pool,
            first,
            BTBH_GPU_FRAME_ARENA_UPLOAD,
            8,
            16,
            &slice
        ) == BTBH_GPU_RESOURCE_OK
    );
    assert(slice.offset == 16);
    assert(
        btbh_gpu_frame_allocate(
            pool,
            first,
            BTBH_GPU_FRAME_ARENA_READBACK,
            65,
            1,
            &slice
        ) == BTBH_GPU_RESOURCE_ARENA_EXHAUSTED
    );
    assert(
        btbh_gpu_frame_allocate(
            pool,
            first,
            BTBH_GPU_FRAME_ARENA_UPLOAD,
            1,
            BTBH_GPU_FRAME_MAX_ALIGNMENT * 2u,
            &slice
        ) == BTBH_GPU_RESOURCE_INVALID_ARGUMENT
    );
    assert(
        btbh_gpu_frame_end(pool, first, 10) == BTBH_GPU_RESOURCE_OK
    );
    assert(
        btbh_gpu_frame_begin(pool, 2, 0, &second) ==
        BTBH_GPU_RESOURCE_OK
    );
    assert(
        btbh_gpu_frame_begin(pool, 3, 0, &recycled) ==
        BTBH_GPU_RESOURCE_FRAME_BUSY
    );
    assert(
        btbh_gpu_frame_end(pool, second, 11) == BTBH_GPU_RESOURCE_OK
    );
    assert(
        btbh_gpu_frame_begin(pool, 3, 10, &recycled) ==
        BTBH_GPU_RESOURCE_OK
    );
    assert(recycled != first);
    assert(
        btbh_gpu_frame_allocate(
            pool,
            first,
            BTBH_GPU_FRAME_ARENA_UPLOAD,
            4,
            4,
            &slice
        ) == BTBH_GPU_RESOURCE_STALE_HANDLE
    );
    assert(
        btbh_gpu_frame_end(pool, recycled, 12) ==
        BTBH_GPU_RESOURCE_OK
    );

    for (uint64_t serial = 13; serial < 113; ++serial) {
        BTBHGPUFrameToken token = 0;
        assert(
            btbh_gpu_frame_begin(pool, serial, serial - 1, &token) ==
            BTBH_GPU_RESOURCE_OK
        );
        assert(
            btbh_gpu_frame_allocate(
                pool,
                token,
                BTBH_GPU_FRAME_ARENA_UPLOAD,
                32,
                16,
                &slice
            ) == BTBH_GPU_RESOURCE_OK
        );
        assert(
            btbh_gpu_frame_end(pool, token, serial) ==
            BTBH_GPU_RESOURCE_OK
        );
    }

    btbh_gpu_frame_pool_stats(pool, &after);
    assert(after.heap_allocation_count == before.heap_allocation_count);
    assert(after.peak_upload_bytes >= 32);
    assert(after.peak_readback_bytes == 0);
    btbh_gpu_frame_pool_destroy(pool);
}

static void test_row_pitch_alignment_and_overflow(void)
{
    uint32_t pitch = 0;

    assert(
        btbh_gpu_aligned_row_pitch(13, 4, 256, &pitch) ==
        BTBH_GPU_RESOURCE_OK
    );
    assert(pitch == 256);
    assert(
        btbh_gpu_aligned_row_pitch(64, 4, 256, &pitch) ==
        BTBH_GPU_RESOURCE_OK
    );
    assert(pitch == 256);
    assert(
        btbh_gpu_aligned_row_pitch(
            UINT32_MAX,
            UINT32_MAX,
            256,
            &pitch
        ) == BTBH_GPU_RESOURCE_OVERFLOW
    );
    assert(
        btbh_gpu_aligned_row_pitch(1, 4, 3, &pitch) ==
        BTBH_GPU_RESOURCE_INVALID_ARGUMENT
    );
}

int main(void)
{
    test_stale_handle_and_deferred_release();
    test_type_mismatch_and_resize_recreation();
    test_fixed_registry_and_deferred_capacities();
    test_frame_slots_and_fixed_arenas();
    test_row_pitch_alignment_and_overflow();
    return 0;
}
