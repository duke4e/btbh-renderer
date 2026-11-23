#include "btbh_gpu_internal.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    BTBH_GPU_CUSTOM_MESH_VERTEX_STRIDE =
        24u * (uint32_t) sizeof(float)
};

static void btbh_gpu_custom_mesh_error(
    BTBHGPUCustomMeshReport *report,
    BTBHGPUResult result,
    const char *message
) {
    if (report == NULL) {
        return;
    }
    report->result = result;
    (void) snprintf(
        report->message,
        sizeof(report->message),
        "%s",
        message != NULL ? message : ""
    );
}

/* Uploads the vertices and the indices in one submission, at the given byte offsets of their buffers (either may be
   empty: a partial update, since 0.11.0). `cycle`: a buffer of the mesh's own that a frame still uses gets new memory
   (SDL's cycling), so that frame keeps the old data; a shared mesh's buffers are the arena's and must not cycle (the
   rest of the block would be lost), nor must a partial update's (the rest of the mesh would be). `into`: NULL to
   acquire and submit a command buffer of its own (as since 0.1.0); otherwise the copy pass is recorded into that one
   (a frame waiting for the canvas, the upload queue's ordering rules) and nothing is submitted. The data is staged
   through the upload staging pool when the device has a budget and `into` is NULL (btbh_gpu_staging_acquire_into). */
static bool btbh_gpu_custom_mesh_upload_buffers(
    SDL_GPUDevice *device,
    SDL_GPUBuffer *vertex_buffer,
    uint32_t vertex_offset,
    const void *vertices,
    uint32_t vertex_byte_count,
    SDL_GPUBuffer *index_buffer,
    uint32_t index_offset,
    const uint32_t *indices,
    uint32_t index_byte_count,
    bool cycle,
    SDL_GPUCommandBuffer *into,
    BTBHGPUCustomMeshReport *report
) {
    SDL_GPUTransferBuffer *transfers[2] = { NULL, NULL };
    uint32_t staging_offsets[2] = { 0u, 0u };
    const void *const sources[2] = { vertices, indices };
    const uint32_t byte_counts[2] = { vertex_byte_count, index_byte_count };
    for (uint32_t part = 0u; part < 2u; ++part) {
        if (byte_counts[part] == 0u) {
            continue;
        }
        transfers[part] = btbh_gpu_staging_acquire_into(device, into, byte_counts[part], &staging_offsets[part]);
        void *mapped = transfers[part] != NULL ? SDL_MapGPUTransferBuffer(device, transfers[part], false) : NULL;
        if (mapped == NULL) {
            btbh_gpu_custom_mesh_error(
                report,
                BTBH_GPU_DEVICE_FAILURE,
                SDL_GetError()
            );
            btbh_gpu_staging_abandon(device, transfers[1]);
            btbh_gpu_staging_abandon(device, transfers[0]);
            return false;
        }
        memcpy((uint8_t *)mapped + staging_offsets[part], sources[part], byte_counts[part]);
        SDL_UnmapGPUTransferBuffer(device, transfers[part]);
    }

    SDL_GPUCommandBuffer *command = into != NULL ? into : SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass *copy = command != NULL
        ? SDL_BeginGPUCopyPass(command)
        : NULL;
    if (copy == NULL) {
        btbh_gpu_custom_mesh_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        if (command != NULL && into == NULL) {
            (void) SDL_CancelGPUCommandBuffer(command);
        }
        btbh_gpu_staging_abandon(device, transfers[1]);
        btbh_gpu_staging_abandon(device, transfers[0]);
        return false;
    }

    if (vertex_byte_count > 0u) {
        SDL_GPUTransferBufferLocation vertex_source = {0};
        vertex_source.transfer_buffer = transfers[0];
        vertex_source.offset = staging_offsets[0];
        SDL_GPUBufferRegion vertex_destination = {0};
        vertex_destination.buffer = vertex_buffer;
        vertex_destination.offset = vertex_offset;
        vertex_destination.size = vertex_byte_count;
        SDL_UploadToGPUBuffer(
            copy,
            &vertex_source,
            &vertex_destination,
            cycle
        );
    }
    if (index_byte_count > 0u) {
        SDL_GPUTransferBufferLocation index_source = {0};
        index_source.transfer_buffer = transfers[1];
        index_source.offset = staging_offsets[1];
        SDL_GPUBufferRegion index_destination = {0};
        index_destination.buffer = index_buffer;
        index_destination.offset = index_offset;
        index_destination.size = index_byte_count;
        SDL_UploadToGPUBuffer(
            copy,
            &index_source,
            &index_destination,
            cycle
        );
    }
    SDL_EndGPUCopyPass(copy);
    /* (Recorded into a frame, the transfer buffers are released now and freed by SDL once that frame has run.) */
    const bool submitted = btbh_gpu_staging_submit_into(device, command, into, transfers, 2u);
    if (!submitted) {
        btbh_gpu_custom_mesh_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return false;
    }
    return true;
}

static SDL_GPUBuffer *btbh_gpu_custom_mesh_create_buffer(
    SDL_GPUDevice *device,
    SDL_GPUBufferUsageFlags usage,
    uint32_t byte_count,
    BTBHGPUCustomMeshReport *report
) {
    SDL_GPUBufferCreateInfo info = {0};
    info.usage = usage;
    info.size = byte_count;
    SDL_GPUBuffer *buffer = SDL_CreateGPUBuffer(device, &info);
    if (buffer == NULL) {
        btbh_gpu_custom_mesh_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
    }
    return buffer;
}

static bool btbh_gpu_custom_mesh_validate_data(
    const void *vertices,
    uint32_t vertex_byte_count,
    uint32_t vertex_count,
    const uint32_t *indices,
    uint32_t index_count,
    BTBHGPUCustomMeshReport *report
) {
    if (
        vertices == NULL ||
        vertex_count == 0u ||
        vertex_count > UINT32_MAX / BTBH_GPU_CUSTOM_MESH_VERTEX_STRIDE ||
        vertex_byte_count !=
            vertex_count * BTBH_GPU_CUSTOM_MESH_VERTEX_STRIDE ||
        index_count > UINT32_MAX / (uint32_t) sizeof(uint32_t) ||
        (index_count > 0u && indices == NULL)
    ) {
        btbh_gpu_custom_mesh_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            "Invalid GPU custom mesh data"
        );
        return false;
    }
    for (uint32_t index = 0u; index < index_count; index += 1u) {
        if (indices[index] >= vertex_count) {
            btbh_gpu_custom_mesh_error(
                report,
                BTBH_GPU_INVALID_ARGUMENT,
                "GPU custom mesh index is outside the vertex buffer"
            );
            return false;
        }
    }
    return true;
}

bool btbh_gpu_custom_mesh_bounds(
    const void *vertices,
    uint32_t vertex_count,
    float minimum[3],
    float maximum[3]
) {
    if (vertices == NULL || vertex_count == 0u) {
        return false;
    }
    const uint8_t *vertex = (const uint8_t *) vertices;
    float position[3];
    memcpy(position, vertex, sizeof(position));
    memcpy(minimum, position, sizeof(position));
    memcpy(maximum, position, sizeof(position));
    for (uint32_t index = 0u; index < vertex_count; index += 1u) {
        /* (The host's bytes need not be aligned for floats.) */
        memcpy(position, vertex, sizeof(position));
        vertex += BTBH_GPU_CUSTOM_MESH_VERTEX_STRIDE;
        if (!isfinite(position[0]) ||
            !isfinite(position[1]) ||
            !isfinite(position[2])) {
            return false;
        }
        for (uint32_t axis = 0u; axis < 3u; axis += 1u) {
            minimum[axis] = position[axis] < minimum[axis] ? position[axis] : minimum[axis];
            maximum[axis] = position[axis] > maximum[axis] ? position[axis] : maximum[axis];
        }
    }
    return true;
}

static void btbh_gpu_custom_mesh_measure(
    BTBHGPUCustomMesh *mesh,
    const void *vertices,
    uint32_t vertex_count
) {
    mesh->bounds_valid = btbh_gpu_custom_mesh_bounds(
        vertices,
        vertex_count,
        mesh->minimum,
        mesh->maximum
    );
}

static void btbh_gpu_custom_mesh_unlink(BTBHGPUCustomMesh *mesh) {
    BTBHGPUWindow *owner = mesh->owner;
    if (owner == NULL) {
        return;
    }
    if (mesh->previous != NULL) {
        mesh->previous->next = mesh->next;
    } else if (owner->custom_mesh_head == mesh) {
        owner->custom_mesh_head = mesh->next;
    }
    if (mesh->next != NULL) {
        mesh->next->previous = mesh->previous;
    }
    mesh->owner = NULL;
    mesh->previous = NULL;
    mesh->next = NULL;
}

static bool btbh_gpu_custom_mesh_shared(const BTBHGPUCustomMesh *mesh) {
    return mesh->vertex_block != NULL;
}

/* A mesh's buffers go; a shared mesh's ranges are given back to their blocks, whose buffers go with their last range
   (the range stays a hole until then). */
static void btbh_gpu_custom_mesh_release_handles(
    BTBHGPUCustomMesh *mesh,
    SDL_GPUDevice *device
) {
    if (btbh_gpu_custom_mesh_shared(mesh)) {
        btbh_gpu_geometry_block_release(mesh->index_block, device);
        btbh_gpu_geometry_block_release(mesh->vertex_block, device);
        mesh->index_block = NULL;
        mesh->vertex_block = NULL;
    } else {
        if (device != NULL && mesh->index_buffer != NULL) {
            SDL_ReleaseGPUBuffer(device, mesh->index_buffer);
        }
        if (device != NULL && mesh->vertex_buffer != NULL) {
            SDL_ReleaseGPUBuffer(device, mesh->vertex_buffer);
        }
    }
    mesh->index_buffer = NULL;
    mesh->vertex_buffer = NULL;
}

/* BTBH_GPU_CUSTOM_MESH_SHARED_BUFFERS: the vertices and indices placed in the window's geometry arena, as a model's
   primitive's are (btbh_gpu_model.c). */
static bool btbh_gpu_custom_mesh_place_shared(
    BTBHGPUWindow *window,
    BTBHGPUCustomMesh *mesh,
    const void *vertices,
    uint32_t vertex_byte_count,
    const uint32_t *indices,
    uint32_t index_count,
    BTBHGPUCustomMeshReport *report
) {
    SDL_GPUDevice *device = window->device->gpu;
    char message[256];
    uint32_t vertex_offset = 0u;
    BTBHGPUGeometryBlock *vertex_block = btbh_gpu_geometry_arena_upload(
        device,
        &window->vertex_arena,
        SDL_GPU_BUFFERUSAGE_VERTEX,
        BTBH_GPU_GEOMETRY_VERTEX_BLOCK_BYTES,
        BTBH_GPU_CUSTOM_MESH_VERTEX_STRIDE,
        vertices,
        vertex_byte_count,
        &vertex_offset
    );
    if (vertex_block == NULL) {
        (void) snprintf(
            message,
            sizeof(message),
            "Could not place the GPU custom mesh's vertices in the shared geometry buffers: %s",
            SDL_GetError()
        );
        btbh_gpu_custom_mesh_error(report, BTBH_GPU_DEVICE_FAILURE, message);
        return false;
    }
    BTBHGPUGeometryBlock *index_block = NULL;
    uint32_t index_offset = 0u;
    if (index_count > 0u) {
        index_block = btbh_gpu_geometry_arena_upload(
            device,
            &window->index_arena,
            SDL_GPU_BUFFERUSAGE_INDEX,
            BTBH_GPU_GEOMETRY_INDEX_BLOCK_BYTES,
            (uint32_t) sizeof(uint32_t),
            indices,
            index_count * (uint32_t) sizeof(uint32_t),
            &index_offset
        );
        if (index_block == NULL) {
            (void) snprintf(
                message,
                sizeof(message),
                "Could not place the GPU custom mesh's indices in the shared geometry buffers: %s",
                SDL_GetError()
            );
            btbh_gpu_custom_mesh_error(report, BTBH_GPU_DEVICE_FAILURE, message);
            btbh_gpu_geometry_block_release(vertex_block, device);
            return false;
        }
    }
    mesh->vertex_block = vertex_block;
    mesh->index_block = index_block;
    mesh->vertex_buffer = vertex_block->buffer;
    mesh->index_buffer = index_block != NULL ? index_block->buffer : NULL;
    mesh->vertex_base = (int32_t) (vertex_offset / BTBH_GPU_CUSTOM_MESH_VERTEX_STRIDE);
    mesh->first_index = index_offset / (uint32_t) sizeof(uint32_t);
    return true;
}

/* Without BTBH_GPU_CUSTOM_MESH_SHARED_BUFFERS: a vertex buffer and (with indices) an index buffer of the mesh's own,
   sized to the data, as since 0.1.0. False, with the report set and nothing left behind, when the GPU failed. */
static bool btbh_gpu_custom_mesh_create_own_buffers(
    SDL_GPUDevice *device,
    BTBHGPUCustomMesh *mesh,
    const void *vertices,
    uint32_t vertex_byte_count,
    const uint32_t *indices,
    uint32_t index_count,
    BTBHGPUCustomMeshReport *report
) {
    mesh->vertex_buffer = btbh_gpu_custom_mesh_create_buffer(
        device,
        SDL_GPU_BUFFERUSAGE_VERTEX,
        vertex_byte_count,
        report
    );
    if (mesh->vertex_buffer == NULL) {
        return false;
    }

    if (index_count > 0u) {
        mesh->index_buffer = btbh_gpu_custom_mesh_create_buffer(
            device,
            SDL_GPU_BUFFERUSAGE_INDEX,
            index_count * (uint32_t) sizeof(uint32_t),
            report
        );
        if (mesh->index_buffer == NULL) {
            btbh_gpu_custom_mesh_release_handles(mesh, device);
            return false;
        }
    }

    if (!btbh_gpu_custom_mesh_upload_buffers(
            device,
            mesh->vertex_buffer,
            0u,
            vertices,
            vertex_byte_count,
            mesh->index_buffer,
            0u,
            indices,
            index_count * (uint32_t) sizeof(uint32_t),
            true,
            NULL,
            report
        )
    ) {
        btbh_gpu_custom_mesh_release_handles(mesh, device);
        return false;
    }
    return true;
}

BTBHGPUCustomMesh *btbh_gpu_window_custom_mesh_create(
    BTBHGPUWindow *window,
    const void *vertices,
    uint32_t vertex_byte_count,
    uint32_t vertex_count,
    const uint32_t *indices,
    uint32_t index_count,
    BTBHGPUCustomMeshReport *report
) {
    return btbh_gpu_window_custom_mesh_create_ex(
        window,
        vertices,
        vertex_byte_count,
        vertex_count,
        indices,
        index_count,
        0u,
        report
    );
}

BTBHGPUCustomMesh *btbh_gpu_window_custom_mesh_create_ex(
    BTBHGPUWindow *window,
    const void *vertices,
    uint32_t vertex_byte_count,
    uint32_t vertex_count,
    const uint32_t *indices,
    uint32_t index_count,
    uint32_t flags,
    BTBHGPUCustomMeshReport *report
) {
    if (report == NULL) {
        return NULL;
    }
    memset(report, 0, sizeof(*report));
    if ((flags & ~(uint32_t) BTBH_GPU_CUSTOM_MESH_SHARED_BUFFERS) != 0u) {
        btbh_gpu_custom_mesh_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            "Unknown GPU custom mesh flags (BTBH_GPU_CUSTOM_MESH_*)"
        );
        return NULL;
    }
    if (
        window == NULL ||
        window->device == NULL ||
        window->device->gpu == NULL ||
        window->device->shutdown ||
        !window->claimed ||
        !btbh_gpu_custom_mesh_validate_data(
            vertices,
            vertex_byte_count,
            vertex_count,
            indices,
            index_count,
            report
        )
    ) {
        if (report->result == BTBH_GPU_OK) {
            btbh_gpu_custom_mesh_error(
                report,
                BTBH_GPU_INVALID_ARGUMENT,
                "Invalid GPU custom mesh descriptor"
            );
        }
        return NULL;
    }

    BTBHGPUCustomMesh *mesh = calloc(1, sizeof(*mesh));
    if (mesh == NULL) {
        btbh_gpu_custom_mesh_error(
            report,
            BTBH_GPU_PLATFORM_FAILURE,
            "Could not allocate GPU custom mesh resource"
        );
        return NULL;
    }

    if ((flags & BTBH_GPU_CUSTOM_MESH_SHARED_BUFFERS) != 0u) {
        if (!btbh_gpu_custom_mesh_place_shared(
                window,
                mesh,
                vertices,
                vertex_byte_count,
                indices,
                index_count,
                report
            )) {
            free(mesh);
            return NULL;
        }
    } else if (!btbh_gpu_custom_mesh_create_own_buffers(
            window->device->gpu,
            mesh,
            vertices,
            vertex_byte_count,
            indices,
            index_count,
            report
        )) {
        free(mesh);
        return NULL;
    }

    mesh->owner = window;
    mesh->next = window->custom_mesh_head;
    if (window->custom_mesh_head != NULL) {
        window->custom_mesh_head->previous = mesh;
    }
    window->custom_mesh_head = mesh;
    mesh->vertex_count = vertex_count;
    mesh->index_count = index_count;
    mesh->vertex_byte_count = vertex_byte_count;
    mesh->vertex_buffer_capacity = vertex_byte_count;
    mesh->index_buffer_capacity = index_count * (uint32_t) sizeof(uint32_t);
    btbh_gpu_custom_mesh_measure(mesh, vertices, vertex_count);

    report->result = BTBH_GPU_OK;
    report->vertex_count = vertex_count;
    report->index_count = index_count;
    report->vertex_byte_count = vertex_byte_count;
    return mesh;
}

/* ---- The window's upload queue (since 0.11.0): BTBH_GPU_CUSTOM_MESH_UPDATE_QUEUED's updates, whole and partial, and
   BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED's uploads, copied by the window's next scene ---- */

/* Each queued range starts at a multiple of this in the queue's bytes and in the scene's transfer buffer (Metal's
   blit encoder wants offsets in multiples of 4; 16 keeps every float vector aligned). */
#define BTBH_GPU_UPLOAD_QUEUE_ALIGNMENT 16u

static uint32_t btbh_gpu_upload_queue_align(uint32_t value) {
    return (value + (BTBH_GPU_UPLOAD_QUEUE_ALIGNMENT - 1u)) & ~(BTBH_GPU_UPLOAD_QUEUE_ALIGNMENT - 1u);
}

static uint32_t *btbh_gpu_upload_queue_head(BTBHGPUCustomMesh *mesh, BTBHGPUInstanceBuffer *instance_buffer) {
    return mesh != NULL ? &mesh->queue_head : &instance_buffer->queue_head;
}

/* The range is no longer copied (a later write replaced it, or its target went); its bytes stay as a hole until the
   next scene empties the queue. */
static void btbh_gpu_upload_queue_kill(BTBHGPUUploadQueue *queue, BTBHGPUUploadQueueEntry *entry) {
    if (!entry->live) {
        return;
    }
    entry->live = false;
    queue->live_count -= 1u;
    queue->live_bytes -= btbh_gpu_upload_queue_align(entry->byte_count);
}

static void btbh_gpu_upload_queue_clear(BTBHGPUUploadQueue *queue) {
    for (uint32_t index = 0u; index < queue->entry_count; ++index) {
        BTBHGPUUploadQueueEntry *entry = &queue->entries[index];
        if (entry->mesh != NULL) {
            entry->mesh->queue_head = 0u;
        }
        if (entry->instance_buffer != NULL) {
            entry->instance_buffer->queue_head = 0u;
        }
        entry->mesh = NULL;
        entry->instance_buffer = NULL;
        entry->live = false;
    }
    queue->entry_count = 0u;
    queue->live_count = 0u;
    queue->byte_count = 0u;
    queue->live_bytes = 0u;
    queue->recorded = false;
}

/* A write of `byte_count` bytes at `byte_offset` of the target's `part`, later than its queued ranges: those it covers
   are dropped, those it overlaps take its bytes there. True when one of them holds the write entirely (it then
   carries all of it). */
static bool btbh_gpu_upload_queue_patch(
    BTBHGPUUploadQueue *queue,
    uint32_t head,
    BTBHGPUUploadPart part,
    uint32_t byte_offset,
    const void *bytes,
    uint32_t byte_count
) {
    const uint64_t start = byte_offset;
    const uint64_t end = start + byte_count;
    bool held = false;
    for (uint32_t link = head; link != 0u; link = queue->entries[link - 1u].next) {
        BTBHGPUUploadQueueEntry *entry = &queue->entries[link - 1u];
        if (!entry->live || entry->part != (uint8_t) part) {
            continue;
        }
        const uint64_t entry_start = entry->byte_offset;
        const uint64_t entry_end = entry_start + entry->byte_count;
        if (entry_end <= start || end <= entry_start) {
            continue;
        }
        if (start <= entry_start && entry_end <= end) {
            btbh_gpu_upload_queue_kill(queue, entry);
            continue;
        }
        const uint64_t from = start > entry_start ? start : entry_start;
        const uint64_t to = end < entry_end ? end : entry_end;
        memcpy(
            queue->bytes + entry->data_offset + (size_t) (from - entry_start),
            (const uint8_t *) bytes + (size_t) (from - start),
            (size_t) (to - from)
        );
        held = held || (entry_start <= start && end <= entry_end);
    }
    return held;
}

bool btbh_gpu_upload_queue_prepare(
    BTBHGPUWindow *window,
    uint32_t first_byte_count,
    uint32_t second_byte_count,
    const char *what,
    char *message,
    size_t message_size,
    bool *out_of_memory
) {
    BTBHGPUUploadQueue *queue = &window->upload_queue;
    const uint64_t byte_count = (uint64_t) btbh_gpu_upload_queue_align(first_byte_count) +
        (uint64_t) btbh_gpu_upload_queue_align(second_byte_count);
    *out_of_memory = false;
    if ((uint64_t) queue->byte_count + byte_count > BTBH_GPU_CUSTOM_MESH_QUEUE_MAX_BYTES) {
        (void) snprintf(
            message,
            message_size,
            "%s: the window's queue holds %u bytes and this one adds %llu, past "
            "BTBH_GPU_CUSTOM_MESH_QUEUE_MAX_BYTES (%u); present a scene, or go without the flag",
            what,
            queue->byte_count,
            (unsigned long long) byte_count,
            (uint32_t) BTBH_GPU_CUSTOM_MESH_QUEUE_MAX_BYTES
        );
        return false;
    }
    if (queue->entry_count + 2u > queue->entry_capacity) {
        uint32_t capacity = queue->entry_capacity == 0u ? 64u : queue->entry_capacity * 2u;
        while (capacity < queue->entry_count + 2u) {
            capacity *= 2u;
        }
        BTBHGPUUploadQueueEntry *entries =
            (BTBHGPUUploadQueueEntry *) realloc(queue->entries, sizeof(*entries) * capacity);
        if (entries == NULL) {
            *out_of_memory = true;
        } else {
            queue->entries = entries;
            queue->entry_capacity = capacity;
        }
    }
    const uint64_t needed = (uint64_t) queue->byte_count + byte_count;
    if (!*out_of_memory && needed > queue->byte_capacity) {
        uint64_t capacity = queue->byte_capacity == 0u ? 64u * 1024u : queue->byte_capacity;
        while (capacity < needed) {
            capacity *= 2u;
        }
        if (capacity > BTBH_GPU_CUSTOM_MESH_QUEUE_MAX_BYTES) {
            capacity = BTBH_GPU_CUSTOM_MESH_QUEUE_MAX_BYTES;
        }
        uint8_t *grown = (uint8_t *) realloc(queue->bytes, (size_t) capacity);
        if (grown == NULL) {
            *out_of_memory = true;
        } else {
            queue->bytes = grown;
            queue->byte_capacity = (uint32_t) capacity;
        }
    }
    if (*out_of_memory) {
        (void) snprintf(message, message_size, "%s: could not allocate room in the window's upload queue", what);
        return false;
    }
    return true;
}

void btbh_gpu_upload_queue_push(
    BTBHGPUWindow *window,
    BTBHGPUCustomMesh *mesh,
    BTBHGPUInstanceBuffer *instance_buffer,
    BTBHGPUUploadPart part,
    uint32_t byte_offset,
    const void *bytes,
    uint32_t byte_count,
    bool cycle
) {
    BTBHGPUUploadQueue *queue = &window->upload_queue;
    uint32_t *head = btbh_gpu_upload_queue_head(mesh, instance_buffer);
    if (btbh_gpu_upload_queue_patch(queue, *head, part, byte_offset, bytes, byte_count)) {
        return;
    }
    BTBHGPUUploadQueueEntry *entry = &queue->entries[queue->entry_count];
    entry->mesh = mesh;
    entry->instance_buffer = instance_buffer;
    entry->next = *head;
    entry->data_offset = queue->byte_count;
    entry->byte_offset = byte_offset;
    entry->byte_count = byte_count;
    entry->part = (uint8_t) part;
    entry->cycle = cycle;
    entry->live = true;
    memcpy(queue->bytes + entry->data_offset, bytes, byte_count);
    queue->byte_count += btbh_gpu_upload_queue_align(byte_count);
    queue->entry_count += 1u;
    queue->live_count += 1u;
    queue->live_bytes += btbh_gpu_upload_queue_align(byte_count);
    *head = queue->entry_count;
}

void btbh_gpu_upload_queue_overwrite(
    BTBHGPUWindow *window,
    BTBHGPUCustomMesh *mesh,
    BTBHGPUInstanceBuffer *instance_buffer,
    BTBHGPUUploadPart part,
    uint32_t byte_offset,
    const void *bytes,
    uint32_t byte_count
) {
    const uint32_t head = *btbh_gpu_upload_queue_head(mesh, instance_buffer);
    if (window == NULL || head == 0u) {
        return;
    }
    (void) btbh_gpu_upload_queue_patch(&window->upload_queue, head, part, byte_offset, bytes, byte_count);
}

void btbh_gpu_upload_queue_drop(
    BTBHGPUWindow *window,
    BTBHGPUCustomMesh *mesh,
    BTBHGPUInstanceBuffer *instance_buffer
) {
    uint32_t *head = btbh_gpu_upload_queue_head(mesh, instance_buffer);
    if (window == NULL) {
        *head = 0u;
        return;
    }
    BTBHGPUUploadQueue *queue = &window->upload_queue;
    for (uint32_t link = *head; link != 0u; link = queue->entries[link - 1u].next) {
        BTBHGPUUploadQueueEntry *entry = &queue->entries[link - 1u];
        /* Forgotten too: the chain starts afresh, and nothing may reach the target through an entry it no longer
           links (it may be destroyed before the queue empties). */
        btbh_gpu_upload_queue_kill(queue, entry);
        entry->mesh = NULL;
        entry->instance_buffer = NULL;
    }
    *head = 0u;
}

SDL_GPUCommandBuffer *btbh_gpu_upload_queue_pending_command(
    const BTBHGPUWindow *window,
    uint64_t copied_serial
) {
    return window->pending_command != NULL &&
            copied_serial != 0u &&
            copied_serial == window->upload_queue.pending_serial
        ? window->pending_command
        : NULL;
}

void btbh_gpu_upload_queue_note_pending_write(BTBHGPUWindow *window) {
    window->upload_queue.pending_holds = true;
}

bool btbh_gpu_upload_queue_finish_pending(BTBHGPUWindow *window) {
    const bool holds = window->upload_queue.pending_holds;
    window->upload_queue.pending_holds = false;
    return holds;
}

uint32_t btbh_gpu_upload_queue_size(const BTBHGPUWindow *window) {
    return window != NULL ? window->upload_queue.live_bytes : 0u;
}

void btbh_gpu_upload_queue_write(const BTBHGPUWindow *window, uint8_t *destination) {
    const BTBHGPUUploadQueue *queue = &window->upload_queue;
    uint32_t offset = 0u;
    for (uint32_t index = 0u; index < queue->entry_count; ++index) {
        const BTBHGPUUploadQueueEntry *entry = &queue->entries[index];
        if (!entry->live) {
            continue;
        }
        memcpy(destination + offset, queue->bytes + entry->data_offset, entry->byte_count);
        offset += btbh_gpu_upload_queue_align(entry->byte_count);
    }
}

uint32_t btbh_gpu_upload_queue_record(
    BTBHGPUWindow *window,
    SDL_GPUCopyPass *copy,
    SDL_GPUTransferBuffer *transfer,
    uint32_t offset
) {
    BTBHGPUUploadQueue *queue = &window->upload_queue;
    uint32_t uploads = 0u;
    for (uint32_t index = 0u; index < queue->entry_count; ++index) {
        const BTBHGPUUploadQueueEntry *entry = &queue->entries[index];
        if (!entry->live) {
            continue;
        }
        SDL_GPUTransferBufferLocation source = {0};
        source.transfer_buffer = transfer;
        source.offset = offset;
        SDL_GPUBufferRegion destination = {0};
        destination.size = entry->byte_count;
        if (entry->part == (uint8_t) BTBH_GPU_UPLOAD_INSTANCES) {
            /* In place: the rest of the buffer keeps its data; earlier frames read it before this copy runs. */
            destination.buffer = entry->instance_buffer->buffer;
            destination.offset = entry->byte_offset;
            SDL_UploadToGPUBuffer(copy, &source, &destination, false);
            destination.buffer = entry->instance_buffer->vertex_buffer;
            SDL_UploadToGPUBuffer(copy, &source, &destination, false);
            uploads += 2u;
        } else {
            /* Where the data goes is read now: a mesh with buffers of its own may have grown them at the call. Its whole
               update cycles (a frame still drawing it keeps the old data); a shared mesh and a partial update are
               written in place. */
            const BTBHGPUCustomMesh *mesh = entry->mesh;
            if (entry->part == (uint8_t) BTBH_GPU_UPLOAD_MESH_VERTICES) {
                destination.buffer = mesh->vertex_buffer;
                destination.offset = (uint32_t) mesh->vertex_base * BTBH_GPU_CUSTOM_MESH_VERTEX_STRIDE +
                    entry->byte_offset;
            } else {
                destination.buffer = mesh->index_buffer;
                destination.offset = mesh->first_index * (uint32_t) sizeof(uint32_t) + entry->byte_offset;
            }
            SDL_UploadToGPUBuffer(copy, &source, &destination, entry->cycle);
            uploads += 1u;
        }
        offset += btbh_gpu_upload_queue_align(entry->byte_count);
    }
    queue->recorded = queue->live_count > 0u;
    return uploads;
}

void btbh_gpu_upload_queue_commit(BTBHGPUWindow *window, bool pending) {
    if (window == NULL) {
        return;
    }
    BTBHGPUUploadQueue *queue = &window->upload_queue;
    if (!queue->recorded) {
        queue->pending_serial = 0u;
        return;
    }
    queue->copied_serial += 1u;
    for (uint32_t index = 0u; index < queue->entry_count; ++index) {
        const BTBHGPUUploadQueueEntry *entry = &queue->entries[index];
        if (!entry->live) {
            continue;
        }
        if (entry->mesh != NULL) {
            entry->mesh->queue_copied_serial = queue->copied_serial;
        } else {
            entry->instance_buffer->queue_copied_serial = queue->copied_serial;
        }
    }
    queue->pending_serial = pending ? queue->copied_serial : 0u;
    queue->pending_holds = queue->pending_holds || pending;
    btbh_gpu_upload_queue_clear(queue);
}

void btbh_gpu_window_release_upload_queue(BTBHGPUWindow *window) {
    if (window == NULL) {
        return;
    }
    BTBHGPUUploadQueue *queue = &window->upload_queue;
    btbh_gpu_upload_queue_clear(queue);
    free(queue->entries);
    free(queue->bytes);
    memset(queue, 0, sizeof(*queue));
}

/* A queued update (BTBH_GPU_CUSTOM_MESH_UPDATE_QUEUED): the data copied into the window's queue, the mesh's counts,
   capacities and bounds changed now; the next scene copies it to the GPU. The data was validated. */
static BTBHGPUResult btbh_gpu_custom_mesh_queue_update(
    BTBHGPUCustomMesh *mesh,
    const void *vertices,
    uint32_t vertex_byte_count,
    uint32_t vertex_count,
    const uint32_t *indices,
    uint32_t index_count,
    BTBHGPUCustomMeshReport *report
) {
    BTBHGPUWindow *window = mesh->owner;
    SDL_GPUDevice *device = window->device->gpu;
    const uint32_t index_byte_count = index_count * (uint32_t) sizeof(uint32_t);
    char message[256];
    bool out_of_memory = false;
    if (!btbh_gpu_upload_queue_prepare(
            window,
            vertex_byte_count,
            index_byte_count,
            "GPU custom mesh queued update",
            message,
            sizeof(message),
            &out_of_memory
        )
    ) {
        btbh_gpu_custom_mesh_error(
            report,
            out_of_memory ? BTBH_GPU_PLATFORM_FAILURE : BTBH_GPU_INVALID_ARGUMENT,
            message
        );
        return report->result;
    }
    SDL_GPUBuffer *vertex_buffer = mesh->vertex_buffer;
    SDL_GPUBuffer *index_buffer = mesh->index_buffer;
    uint32_t vertex_capacity = mesh->vertex_buffer_capacity;
    uint32_t index_capacity = mesh->index_buffer_capacity;
    bool grow_vertices = false;
    bool grow_indices = false;
    if (btbh_gpu_custom_mesh_shared(mesh)) {
        if (vertex_byte_count > mesh->vertex_buffer_capacity || index_byte_count > mesh->index_buffer_capacity) {
            (void) snprintf(
                message,
                sizeof(message),
                "GPU custom mesh in the shared geometry buffers: an update must fit the %u vertices and %u indices "
                "it was created with (got %u and %u)",
                mesh->vertex_buffer_capacity / BTBH_GPU_CUSTOM_MESH_VERTEX_STRIDE,
                mesh->index_buffer_capacity / (uint32_t) sizeof(uint32_t),
                vertex_count,
                index_count
            );
            btbh_gpu_custom_mesh_error(report, BTBH_GPU_INVALID_ARGUMENT, message);
            return report->result;
        }
    } else {
        /* Grown now, as the plain update grows them (a quarter more than needed); the old buffers are released,
           and SDL frees them once no frame uses them. Nothing draws the new ones before the next scene copies the
           queue into them. */
        grow_vertices = vertex_byte_count > vertex_capacity;
        grow_indices = index_byte_count > index_capacity;
        if (grow_vertices) {
            const uint64_t grown = (uint64_t) vertex_byte_count + (uint64_t) vertex_byte_count / 4u;
            vertex_capacity = grown > UINT32_MAX - BTBH_GPU_CUSTOM_MESH_VERTEX_STRIDE
                ? vertex_byte_count
                : (uint32_t) (grown - grown % BTBH_GPU_CUSTOM_MESH_VERTEX_STRIDE);
            vertex_buffer = btbh_gpu_custom_mesh_create_buffer(
                device,
                SDL_GPU_BUFFERUSAGE_VERTEX,
                vertex_capacity,
                report
            );
            if (vertex_buffer == NULL) {
                return report->result;
            }
        }
        if (grow_indices) {
            const uint64_t grown = (uint64_t) index_byte_count + (uint64_t) index_byte_count / 4u;
            index_capacity = grown > UINT32_MAX - 4u
                ? index_byte_count
                : (uint32_t) (grown - grown % 4u);
            index_buffer = btbh_gpu_custom_mesh_create_buffer(
                device,
                SDL_GPU_BUFFERUSAGE_INDEX,
                index_capacity,
                report
            );
            if (index_buffer == NULL) {
                if (grow_vertices) {
                    SDL_ReleaseGPUBuffer(device, vertex_buffer);
                }
                return report->result;
            }
        }
    }
    /* Every waiting update of this mesh is replaced, whole or partial: only this one is copied. A mesh of its own
       cycles (a frame still drawing it keeps the old data); a shared one is written in place. */
    btbh_gpu_upload_queue_drop(window, mesh, NULL);
    const bool cycle = !btbh_gpu_custom_mesh_shared(mesh);
    btbh_gpu_upload_queue_push(
        window,
        mesh,
        NULL,
        BTBH_GPU_UPLOAD_MESH_VERTICES,
        0u,
        vertices,
        vertex_byte_count,
        cycle
    );
    if (index_byte_count > 0u) {
        btbh_gpu_upload_queue_push(
            window,
            mesh,
            NULL,
            BTBH_GPU_UPLOAD_MESH_INDICES,
            0u,
            indices,
            index_byte_count,
            cycle
        );
    }
    if (grow_vertices) {
        SDL_ReleaseGPUBuffer(device, mesh->vertex_buffer);
        mesh->vertex_buffer = vertex_buffer;
        mesh->vertex_buffer_capacity = vertex_capacity;
    }
    if (grow_indices) {
        if (mesh->index_buffer != NULL) {
            SDL_ReleaseGPUBuffer(device, mesh->index_buffer);
        }
        mesh->index_buffer = index_buffer;
        mesh->index_buffer_capacity = index_capacity;
    }
    mesh->vertex_count = vertex_count;
    mesh->index_count = index_count;
    mesh->vertex_byte_count = vertex_byte_count;
    btbh_gpu_custom_mesh_measure(mesh, vertices, vertex_count);
    report->result = BTBH_GPU_OK;
    report->vertex_count = vertex_count;
    report->index_count = index_count;
    report->vertex_byte_count = vertex_byte_count;
    return report->result;
}

BTBHGPUResult btbh_gpu_custom_mesh_update(
    BTBHGPUCustomMesh *mesh,
    const void *vertices,
    uint32_t vertex_byte_count,
    const uint32_t *indices,
    uint32_t index_count,
    BTBHGPUCustomMeshReport *report
) {
    return btbh_gpu_custom_mesh_update_ex(mesh, vertices, vertex_byte_count, indices, index_count, 0u, report);
}

BTBHGPUResult btbh_gpu_custom_mesh_update_ex(
    BTBHGPUCustomMesh *mesh,
    const void *vertices,
    uint32_t vertex_byte_count,
    const uint32_t *indices,
    uint32_t index_count,
    uint32_t flags,
    BTBHGPUCustomMeshReport *report
) {
    if (report == NULL) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    memset(report, 0, sizeof(*report));
    if ((flags & ~(uint32_t) BTBH_GPU_CUSTOM_MESH_UPDATE_QUEUED) != 0u) {
        btbh_gpu_custom_mesh_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            "Unknown GPU custom mesh update flags (BTBH_GPU_CUSTOM_MESH_UPDATE_*)"
        );
        return report->result;
    }
    if (
        mesh == NULL ||
        mesh->owner == NULL ||
        mesh->owner->device == NULL ||
        mesh->owner->device->gpu == NULL ||
        mesh->owner->device->shutdown ||
        mesh->vertex_buffer == NULL ||
        vertex_byte_count % BTBH_GPU_CUSTOM_MESH_VERTEX_STRIDE != 0u
    ) {
        btbh_gpu_custom_mesh_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            "Invalid GPU custom mesh update"
        );
        return report->result;
    }

    const uint32_t vertex_count =
        vertex_byte_count / BTBH_GPU_CUSTOM_MESH_VERTEX_STRIDE;
    if (!btbh_gpu_custom_mesh_validate_data(
            vertices,
            vertex_byte_count,
            vertex_count,
            indices,
            index_count,
            report
        )
    ) {
        return report->result;
    }
    if ((flags & BTBH_GPU_CUSTOM_MESH_UPDATE_QUEUED) != 0u) {
        return btbh_gpu_custom_mesh_queue_update(
            mesh,
            vertices,
            vertex_byte_count,
            vertex_count,
            indices,
            index_count,
            report
        );
    }

    SDL_GPUDevice *device = mesh->owner->device->gpu;
    const uint32_t index_byte_count = index_count * (uint32_t) sizeof(uint32_t);
    if (btbh_gpu_custom_mesh_shared(mesh)) {
        /* BTBH_GPU_CUSTOM_MESH_SHARED_BUFFERS (RFC 0011): in place, within the ranges the mesh was created with
           (their neighbours belong to other meshes and models), without cycling the arena's buffers. */
        if (vertex_byte_count > mesh->vertex_buffer_capacity ||
            index_byte_count > mesh->index_buffer_capacity) {
            char message[256];
            (void) snprintf(
                message,
                sizeof(message),
                "GPU custom mesh in the shared geometry buffers: an update must fit the %u vertices and %u indices "
                "it was created with (got %u and %u)",
                mesh->vertex_buffer_capacity / BTBH_GPU_CUSTOM_MESH_VERTEX_STRIDE,
                mesh->index_buffer_capacity / (uint32_t) sizeof(uint32_t),
                vertex_count,
                index_count
            );
            btbh_gpu_custom_mesh_error(report, BTBH_GPU_INVALID_ARGUMENT, message);
            return report->result;
        }
        /* BTBH_GPU_CUSTOM_MESH_UPDATE_QUEUED's ordering: a queued update of this mesh that the frame waiting for the
           canvas copied would land after a submission of this one (in place, no cycling), so this one goes into that
           frame's command buffer, after its draws. A host that never queues never gets here. */
        BTBHGPUWindow *owner = mesh->owner;
        SDL_GPUCommandBuffer *into = btbh_gpu_upload_queue_pending_command(owner, mesh->queue_copied_serial);
        if (!btbh_gpu_custom_mesh_upload_buffers(
                device,
                mesh->vertex_buffer,
                (uint32_t) mesh->vertex_base * BTBH_GPU_CUSTOM_MESH_VERTEX_STRIDE,
                vertices,
                vertex_byte_count,
                mesh->index_buffer,
                mesh->first_index * (uint32_t) sizeof(uint32_t),
                indices,
                index_byte_count,
                false,
                into,
                report
            )
        ) {
            return report->result;
        }
        if (into != NULL) {
            btbh_gpu_upload_queue_note_pending_write(owner);
        }
        /* Queued updates not yet copied are older: this one replaces them. */
        btbh_gpu_upload_queue_drop(owner, mesh, NULL);
        mesh->vertex_count = vertex_count;
        mesh->index_count = index_count;
        mesh->vertex_byte_count = vertex_byte_count;
        btbh_gpu_custom_mesh_measure(mesh, vertices, vertex_count);
        report->result = BTBH_GPU_OK;
        report->vertex_count = vertex_count;
        report->index_count = index_count;
        report->vertex_byte_count = vertex_byte_count;
        return report->result;
    }

    /* The buffers only grow (since 0.5.0): an update that fits their capacity reuses them, and one that
       doesn't makes them a quarter bigger than it needs, so a mesh that keeps changing size (an SDF terrain
       chunk) stops reallocating. Draws use the counts, not the capacities. */
    SDL_GPUBuffer *vertex_buffer = mesh->vertex_buffer;
    SDL_GPUBuffer *index_buffer = mesh->index_buffer;
    uint32_t vertex_capacity = mesh->vertex_buffer_capacity;
    uint32_t index_capacity = mesh->index_buffer_capacity;
    const bool grow_vertices = vertex_byte_count > vertex_capacity;
    const bool grow_indices = index_byte_count > index_capacity;
    if (grow_vertices) {
        const uint64_t grown = (uint64_t) vertex_byte_count + (uint64_t) vertex_byte_count / 4u;
        vertex_capacity = grown > UINT32_MAX - BTBH_GPU_CUSTOM_MESH_VERTEX_STRIDE
            ? vertex_byte_count
            : (uint32_t) (grown - grown % BTBH_GPU_CUSTOM_MESH_VERTEX_STRIDE);
        vertex_buffer = btbh_gpu_custom_mesh_create_buffer(
            device,
            SDL_GPU_BUFFERUSAGE_VERTEX,
            vertex_capacity,
            report
        );
        if (vertex_buffer == NULL) {
            return report->result;
        }
    }
    if (grow_indices) {
        const uint64_t grown = (uint64_t) index_byte_count + (uint64_t) index_byte_count / 4u;
        index_capacity = grown > UINT32_MAX - 4u
            ? index_byte_count
            : (uint32_t) (grown - grown % 4u);
        index_buffer = btbh_gpu_custom_mesh_create_buffer(
            device,
            SDL_GPU_BUFFERUSAGE_INDEX,
            index_capacity,
            report
        );
        if (index_buffer == NULL) {
            if (grow_vertices) {
                SDL_ReleaseGPUBuffer(device, vertex_buffer);
            }
            return report->result;
        }
    }
    if (!btbh_gpu_custom_mesh_upload_buffers(
            device,
            vertex_buffer,
            0u,
            vertices,
            vertex_byte_count,
            index_buffer,
            0u,
            indices,
            index_byte_count,
            true,
            NULL,
            report
        )
    ) {
        if (grow_indices) {
            SDL_ReleaseGPUBuffer(device, index_buffer);
        }
        if (grow_vertices) {
            SDL_ReleaseGPUBuffer(device, vertex_buffer);
        }
        return report->result;
    }
    /* Queued updates not yet copied are older: this one replaces them (a copied one cycled the buffers or was recorded
       before this submission's frame, so it lands first either way). */
    btbh_gpu_upload_queue_drop(mesh->owner, mesh, NULL);
    if (grow_vertices) {
        SDL_ReleaseGPUBuffer(device, mesh->vertex_buffer);
        mesh->vertex_buffer = vertex_buffer;
        mesh->vertex_buffer_capacity = vertex_capacity;
    }
    if (grow_indices) {
        if (mesh->index_buffer != NULL) {
            SDL_ReleaseGPUBuffer(device, mesh->index_buffer);
        }
        mesh->index_buffer = index_buffer;
        mesh->index_buffer_capacity = index_capacity;
    }
    mesh->vertex_count = vertex_count;
    mesh->index_count = index_count;
    mesh->vertex_byte_count = vertex_byte_count;
    btbh_gpu_custom_mesh_measure(mesh, vertices, vertex_count);

    report->result = BTBH_GPU_OK;
    report->vertex_count = vertex_count;
    report->index_count = index_count;
    report->vertex_byte_count = vertex_byte_count;
    return report->result;
}

BTBHGPUResult btbh_gpu_custom_mesh_update_range(
    BTBHGPUCustomMesh *mesh,
    uint32_t first_vertex,
    const void *vertices,
    uint32_t vertex_byte_count,
    uint32_t first_index,
    const uint32_t *indices,
    uint32_t index_count,
    uint32_t flags,
    BTBHGPUCustomMeshReport *report
) {
    if (report == NULL) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    memset(report, 0, sizeof(*report));
    if ((flags & ~(uint32_t) BTBH_GPU_CUSTOM_MESH_UPDATE_QUEUED) != 0u) {
        btbh_gpu_custom_mesh_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            "Unknown GPU custom mesh update flags (BTBH_GPU_CUSTOM_MESH_UPDATE_*)"
        );
        return report->result;
    }
    if (
        mesh == NULL ||
        mesh->owner == NULL ||
        mesh->owner->device == NULL ||
        mesh->owner->device->gpu == NULL ||
        mesh->owner->device->shutdown ||
        mesh->vertex_buffer == NULL ||
        vertex_byte_count % BTBH_GPU_CUSTOM_MESH_VERTEX_STRIDE != 0u ||
        (vertex_byte_count > 0u && vertices == NULL) ||
        (index_count > 0u && indices == NULL) ||
        (vertex_byte_count == 0u && index_count == 0u)
    ) {
        btbh_gpu_custom_mesh_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            "Invalid GPU custom mesh range update"
        );
        return report->result;
    }
    const uint32_t vertex_count = vertex_byte_count / BTBH_GPU_CUSTOM_MESH_VERTEX_STRIDE;
    if ((uint64_t) first_vertex + vertex_count > mesh->vertex_count ||
        (uint64_t) first_index + index_count > mesh->index_count) {
        char message[256];
        (void) snprintf(
            message,
            sizeof(message),
            "GPU custom mesh range update: %u vertices from %u and %u indices from %u must lie within the mesh's %u "
            "vertices and %u indices",
            vertex_count,
            first_vertex,
            index_count,
            first_index,
            mesh->vertex_count,
            mesh->index_count
        );
        btbh_gpu_custom_mesh_error(report, BTBH_GPU_INVALID_ARGUMENT, message);
        return report->result;
    }
    for (uint32_t index = 0u; index < index_count; index += 1u) {
        if (indices[index] >= mesh->vertex_count) {
            btbh_gpu_custom_mesh_error(
                report,
                BTBH_GPU_INVALID_ARGUMENT,
                "GPU custom mesh index is outside the vertex buffer"
            );
            return report->result;
        }
    }

    BTBHGPUWindow *window = mesh->owner;
    const uint32_t vertex_offset = first_vertex * BTBH_GPU_CUSTOM_MESH_VERTEX_STRIDE;
    const uint32_t index_offset = first_index * (uint32_t) sizeof(uint32_t);
    const uint32_t index_byte_count = index_count * (uint32_t) sizeof(uint32_t);
    if ((flags & BTBH_GPU_CUSTOM_MESH_UPDATE_QUEUED) != 0u) {
        char message[256];
        bool out_of_memory = false;
        if (!btbh_gpu_upload_queue_prepare(
                window,
                vertex_byte_count,
                index_byte_count,
                "GPU custom mesh queued range update",
                message,
                sizeof(message),
                &out_of_memory
            )
        ) {
            btbh_gpu_custom_mesh_error(
                report,
                out_of_memory ? BTBH_GPU_PLATFORM_FAILURE : BTBH_GPU_INVALID_ARGUMENT,
                message
            );
            return report->result;
        }
        if (vertex_byte_count > 0u) {
            btbh_gpu_upload_queue_push(
                window,
                mesh,
                NULL,
                BTBH_GPU_UPLOAD_MESH_VERTICES,
                vertex_offset,
                vertices,
                vertex_byte_count,
                false
            );
        }
        if (index_byte_count > 0u) {
            btbh_gpu_upload_queue_push(
                window,
                mesh,
                NULL,
                BTBH_GPU_UPLOAD_MESH_INDICES,
                index_offset,
                indices,
                index_byte_count,
                false
            );
        }
    } else {
        /* In place, without cycling (the rest of the mesh keeps its data), the mesh's own buffers as the shared
           ones. While a scene waits for the canvas, into that frame after its draws: it keeps the old data, and a
           queued update it copied lands first. */
        SDL_GPUCommandBuffer *into = window->pending_command;
        if (!btbh_gpu_custom_mesh_upload_buffers(
                window->device->gpu,
                mesh->vertex_buffer,
                (uint32_t) mesh->vertex_base * BTBH_GPU_CUSTOM_MESH_VERTEX_STRIDE + vertex_offset,
                vertices,
                vertex_byte_count,
                mesh->index_buffer,
                mesh->first_index * (uint32_t) sizeof(uint32_t) + index_offset,
                indices,
                index_byte_count,
                false,
                into,
                report
            )
        ) {
            return report->result;
        }
        if (into != NULL) {
            btbh_gpu_upload_queue_note_pending_write(window);
        }
        /* Queued updates not yet copied are older: where they overlap this one, they carry its data. */
        if (vertex_byte_count > 0u) {
            btbh_gpu_upload_queue_overwrite(
                window,
                mesh,
                NULL,
                BTBH_GPU_UPLOAD_MESH_VERTICES,
                vertex_offset,
                vertices,
                vertex_byte_count
            );
        }
        if (index_byte_count > 0u) {
            btbh_gpu_upload_queue_overwrite(
                window,
                mesh,
                NULL,
                BTBH_GPU_UPLOAD_MESH_INDICES,
                index_offset,
                indices,
                index_byte_count
            );
        }
    }
    /* The bounds grow to cover the new vertices (the old ones' too: they aren't kept on the CPU to measure again). */
    if (vertex_count > 0u && mesh->bounds_valid) {
        float minimum[3];
        float maximum[3];
        if (btbh_gpu_custom_mesh_bounds(vertices, vertex_count, minimum, maximum)) {
            for (uint32_t axis = 0u; axis < 3u; axis += 1u) {
                mesh->minimum[axis] = minimum[axis] < mesh->minimum[axis] ? minimum[axis] : mesh->minimum[axis];
                mesh->maximum[axis] = maximum[axis] > mesh->maximum[axis] ? maximum[axis] : mesh->maximum[axis];
            }
        } else {
            mesh->bounds_valid = false;
        }
    }
    report->result = BTBH_GPU_OK;
    report->vertex_count = mesh->vertex_count;
    report->index_count = mesh->index_count;
    report->vertex_byte_count = mesh->vertex_byte_count;
    return report->result;
}

void btbh_gpu_custom_mesh_destroy(BTBHGPUCustomMesh *mesh) {
    if (mesh == NULL) {
        return;
    }
    SDL_GPUDevice *device = NULL;
    if (
        mesh->owner != NULL &&
        mesh->owner->device != NULL &&
        !mesh->owner->device->shutdown
    ) {
        device = mesh->owner->device->gpu;
    }
    /* Its queued updates go with it (one a frame already recorded still runs: SDL frees the buffers after it). */
    btbh_gpu_upload_queue_drop(mesh->owner, mesh, NULL);
    btbh_gpu_custom_mesh_release_handles(mesh, device);
    btbh_gpu_custom_mesh_unlink(mesh);
    free(mesh);
}

void btbh_gpu_window_release_custom_meshes(BTBHGPUWindow *window) {
    if (window == NULL) {
        return;
    }
    /* The upload queue goes first: it forgets the meshes and instance buffers it names. */
    btbh_gpu_window_release_upload_queue(window);
    SDL_GPUDevice *device =
        window->device != NULL && !window->device->shutdown
            ? window->device->gpu
            : NULL;
    BTBHGPUCustomMesh *mesh = window->custom_mesh_head;
    window->custom_mesh_head = NULL;
    while (mesh != NULL) {
        BTBHGPUCustomMesh *next = mesh->next;
        btbh_gpu_custom_mesh_release_handles(mesh, device);
        mesh->owner = NULL;
        mesh->previous = NULL;
        mesh->next = NULL;
        mesh = next;
    }
}
