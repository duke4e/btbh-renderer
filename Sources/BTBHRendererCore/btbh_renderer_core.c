#include "btbh_renderer_core.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef struct BTBHHandleSlot {
    uint32_t generation;
    uint32_t next_free;
    bool occupied;
} BTBHHandleSlot;

struct BTBHHandlePool {
    BTBHHandleSlot *slots;
    uint32_t capacity;
    uint32_t free_head;
};

static const uint32_t BTBH_NO_SLOT = UINT32_MAX;

static uint64_t btbh_pack_handle(uint32_t index, uint32_t generation) {
    return ((uint64_t)generation << 32u) | ((uint64_t)index + 1u);
}

static bool btbh_unpack_handle(uint64_t handle, uint32_t *index, uint32_t *generation) {
    const uint32_t encoded_index = (uint32_t)(handle & UINT64_C(0xffffffff));
    if (encoded_index == 0u) return false;
    *index = encoded_index - 1u;
    *generation = (uint32_t)(handle >> 32u);
    return *generation != 0u;
}

static bool btbh_handle_pool_grow(BTBHHandlePool *pool) {
    const uint32_t old_capacity = pool->capacity;
    const uint32_t new_capacity = old_capacity > 0u ? old_capacity * 2u : 8u;
    if (new_capacity < old_capacity) return false;
    BTBHHandleSlot *grown = realloc(pool->slots, sizeof(*grown) * new_capacity);
    if (!grown) return false;
    pool->slots = grown;
    memset(pool->slots + old_capacity, 0, sizeof(*grown) * (new_capacity - old_capacity));
    for (uint32_t index = old_capacity; index < new_capacity; ++index) {
        pool->slots[index].generation = 1u;
        pool->slots[index].next_free = index + 1u < new_capacity ? index + 1u : pool->free_head;
    }
    pool->free_head = old_capacity;
    pool->capacity = new_capacity;
    return true;
}

uint32_t btbh_renderer_abi_version(void) {
    return BTBH_RENDERER_ABI_VERSION;
}

BTBHHandlePool *btbh_handle_pool_create(uint32_t initial_capacity) {
    BTBHHandlePool *pool = calloc(1u, sizeof(*pool));
    if (!pool) return NULL;
    pool->free_head = BTBH_NO_SLOT;
    while (pool->capacity < initial_capacity) {
        if (!btbh_handle_pool_grow(pool)) {
            btbh_handle_pool_destroy(pool);
            return NULL;
        }
    }
    return pool;
}

void btbh_handle_pool_destroy(BTBHHandlePool *pool) {
    if (!pool) return;
    free(pool->slots);
    free(pool);
}

BTBHHandleResult btbh_handle_pool_acquire(BTBHHandlePool *pool, uint64_t *out_handle) {
    if (!pool || !out_handle) return BTBH_HANDLE_INVALID_ARGUMENT;
    if (pool->free_head == BTBH_NO_SLOT && !btbh_handle_pool_grow(pool)) return BTBH_HANDLE_OUT_OF_MEMORY;
    const uint32_t index = pool->free_head;
    BTBHHandleSlot *slot = &pool->slots[index];
    pool->free_head = slot->next_free;
    slot->next_free = BTBH_NO_SLOT;
    slot->occupied = true;
    *out_handle = btbh_pack_handle(index, slot->generation);
    return BTBH_HANDLE_OK;
}

int btbh_handle_pool_contains(const BTBHHandlePool *pool, uint64_t handle) {
    uint32_t index = 0u;
    uint32_t generation = 0u;
    if (!pool || !btbh_unpack_handle(handle, &index, &generation) || index >= pool->capacity) return 0;
    const BTBHHandleSlot *slot = &pool->slots[index];
    return slot->occupied && slot->generation == generation;
}

BTBHHandleResult btbh_handle_pool_release(BTBHHandlePool *pool, uint64_t handle) {
    uint32_t index = 0u;
    uint32_t generation = 0u;
    if (!pool || !btbh_unpack_handle(handle, &index, &generation) || index >= pool->capacity) return BTBH_HANDLE_INVALID_ARGUMENT;
    BTBHHandleSlot *slot = &pool->slots[index];
    if (!slot->occupied || slot->generation != generation) return BTBH_HANDLE_STALE;
    slot->occupied = false;
    slot->generation += 1u;
    if (slot->generation == 0u) slot->generation = 1u;
    slot->next_free = pool->free_head;
    pool->free_head = index;
    return BTBH_HANDLE_OK;
}
