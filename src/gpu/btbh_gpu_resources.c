#include "btbh_gpu/btbh_gpu_resources.h"

#include <stdlib.h>

typedef struct BTBHGPUResourceSlot {
    uint32_t generation;
    BTBHGPUResourceKind kind;
    uintptr_t payload;
    uint8_t occupied;
    uint8_t exhausted;
} BTBHGPUResourceSlot;

typedef struct BTBHGPUDeferredResource {
    uint64_t retire_serial;
    BTBHGPUResourceKind kind;
    uintptr_t payload;
} BTBHGPUDeferredResource;

struct BTBHGPUResourceRegistry {
    BTBHGPUResourceSlot *slots;
    BTBHGPUDeferredResource *deferred;
    uint32_t capacity;
    uint32_t deferred_capacity;
    uint32_t deferred_count;
};

static BTBHGPUResourceHandle btbh_gpu_make_handle(
    uint32_t index,
    uint32_t generation
) {
    return ((uint64_t)generation << 32u) | (uint64_t)(index + 1u);
}

static BTBHGPUResourceResult btbh_gpu_validate_handle(
    const BTBHGPUResourceRegistry *registry,
    BTBHGPUResourceHandle handle,
    BTBHGPUResourceKind expected_kind,
    uint32_t *index
) {
    uint32_t encoded_index;
    uint32_t generation;
    const BTBHGPUResourceSlot *slot;

    if (registry == NULL || index == NULL ||
        handle == BTBH_GPU_INVALID_RESOURCE_HANDLE ||
        expected_kind == BTBH_GPU_RESOURCE_INVALID) {
        return BTBH_GPU_RESOURCE_INVALID_ARGUMENT;
    }

    encoded_index = (uint32_t)handle;
    generation = (uint32_t)(handle >> 32u);
    if (encoded_index == 0u || encoded_index > registry->capacity ||
        generation == 0u) {
        return BTBH_GPU_RESOURCE_STALE_HANDLE;
    }

    *index = encoded_index - 1u;
    slot = &registry->slots[*index];
    if (slot->occupied == 0u || slot->generation != generation) {
        return BTBH_GPU_RESOURCE_STALE_HANDLE;
    }
    if (slot->kind != expected_kind) {
        return BTBH_GPU_RESOURCE_TYPE_MISMATCH;
    }
    return BTBH_GPU_RESOURCE_OK;
}

BTBHGPUResourceRegistry *btbh_gpu_resource_registry_create(
    uint32_t capacity,
    uint32_t deferred_capacity
) {
    BTBHGPUResourceRegistry *registry;

    if (capacity == 0u || deferred_capacity == 0u ||
        (size_t)capacity > SIZE_MAX / sizeof(BTBHGPUResourceSlot) ||
        (size_t)deferred_capacity >
            SIZE_MAX / sizeof(BTBHGPUDeferredResource)) {
        return NULL;
    }

    registry = (BTBHGPUResourceRegistry *)calloc(1u, sizeof(*registry));
    if (registry == NULL) {
        return NULL;
    }
    registry->slots = (BTBHGPUResourceSlot *)calloc(
        capacity,
        sizeof(*registry->slots)
    );
    registry->deferred = (BTBHGPUDeferredResource *)calloc(
        deferred_capacity,
        sizeof(*registry->deferred)
    );
    if (registry->slots == NULL || registry->deferred == NULL) {
        free(registry->deferred);
        free(registry->slots);
        free(registry);
        return NULL;
    }

    registry->capacity = capacity;
    registry->deferred_capacity = deferred_capacity;
    for (uint32_t index = 0u; index < capacity; ++index) {
        registry->slots[index].generation = 1u;
    }
    return registry;
}

void btbh_gpu_resource_registry_destroy(
    BTBHGPUResourceRegistry *registry,
    BTBHGPUResourceReleaseFn release_fn,
    void *context
) {
    if (registry == NULL) {
        return;
    }

    if (release_fn != NULL) {
        for (uint32_t index = 0u; index < registry->capacity; ++index) {
            const BTBHGPUResourceSlot *slot = &registry->slots[index];
            if (slot->occupied != 0u) {
                release_fn(slot->kind, slot->payload, context);
            }
        }
        for (uint32_t index = 0u;
             index < registry->deferred_count;
             ++index) {
            const BTBHGPUDeferredResource *resource =
                &registry->deferred[index];
            release_fn(resource->kind, resource->payload, context);
        }
    }

    free(registry->deferred);
    free(registry->slots);
    free(registry);
}

BTBHGPUResourceResult btbh_gpu_resource_insert(
    BTBHGPUResourceRegistry *registry,
    BTBHGPUResourceKind kind,
    uintptr_t payload,
    BTBHGPUResourceHandle *handle
) {
    if (registry == NULL || handle == NULL ||
        kind == BTBH_GPU_RESOURCE_INVALID || payload == (uintptr_t)0) {
        return BTBH_GPU_RESOURCE_INVALID_ARGUMENT;
    }

    for (uint32_t index = 0u; index < registry->capacity; ++index) {
        BTBHGPUResourceSlot *slot = &registry->slots[index];
        if (slot->occupied == 0u && slot->exhausted == 0u) {
            slot->kind = kind;
            slot->payload = payload;
            slot->occupied = 1u;
            *handle = btbh_gpu_make_handle(index, slot->generation);
            return BTBH_GPU_RESOURCE_OK;
        }
    }
    return BTBH_GPU_RESOURCE_REGISTRY_FULL;
}

BTBHGPUResourceResult btbh_gpu_resource_lookup(
    const BTBHGPUResourceRegistry *registry,
    BTBHGPUResourceHandle handle,
    BTBHGPUResourceKind expected_kind,
    uintptr_t *payload
) {
    BTBHGPUResourceResult result;
    uint32_t index = 0u;

    if (payload == NULL) {
        return BTBH_GPU_RESOURCE_INVALID_ARGUMENT;
    }
    result = btbh_gpu_validate_handle(
        registry,
        handle,
        expected_kind,
        &index
    );
    if (result != BTBH_GPU_RESOURCE_OK) {
        return result;
    }
    *payload = registry->slots[index].payload;
    return BTBH_GPU_RESOURCE_OK;
}

BTBHGPUResourceResult btbh_gpu_resource_retire(
    BTBHGPUResourceRegistry *registry,
    BTBHGPUResourceHandle handle,
    BTBHGPUResourceKind expected_kind,
    uint64_t retire_serial
) {
    BTBHGPUResourceResult result;
    BTBHGPUResourceSlot *slot;
    BTBHGPUDeferredResource *deferred;
    uint32_t index = 0u;

    result = btbh_gpu_validate_handle(
        registry,
        handle,
        expected_kind,
        &index
    );
    if (result != BTBH_GPU_RESOURCE_OK) {
        return result;
    }
    if (registry->deferred_count == registry->deferred_capacity) {
        return BTBH_GPU_RESOURCE_DEFERRED_QUEUE_FULL;
    }

    slot = &registry->slots[index];
    deferred = &registry->deferred[registry->deferred_count++];
    deferred->retire_serial = retire_serial;
    deferred->kind = slot->kind;
    deferred->payload = slot->payload;

    slot->occupied = 0u;
    slot->kind = BTBH_GPU_RESOURCE_INVALID;
    slot->payload = (uintptr_t)0;
    if (slot->generation == UINT32_MAX) {
        slot->exhausted = 1u;
    } else {
        slot->generation += 1u;
    }
    return BTBH_GPU_RESOURCE_OK;
}

uint32_t btbh_gpu_resource_collect(
    BTBHGPUResourceRegistry *registry,
    uint64_t completed_serial,
    BTBHGPUResourceReleaseFn release_fn,
    void *context
) {
    uint32_t released = 0u;
    uint32_t retained = 0u;

    if (registry == NULL || release_fn == NULL) {
        return 0u;
    }

    for (uint32_t index = 0u;
         index < registry->deferred_count;
         ++index) {
        const BTBHGPUDeferredResource resource = registry->deferred[index];
        if (resource.retire_serial <= completed_serial) {
            release_fn(resource.kind, resource.payload, context);
            released += 1u;
        } else {
            registry->deferred[retained++] = resource;
        }
    }
    registry->deferred_count = retained;
    return released;
}
