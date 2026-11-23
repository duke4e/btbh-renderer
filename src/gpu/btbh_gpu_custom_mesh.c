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

/* Uploads the vertices and the indices in one submission, at the given byte offsets of their buffers. `cycle`: a
   buffer of the mesh's own that a frame still uses gets new memory (SDL's cycling), so that frame keeps the old data;
   a shared mesh's buffers are the arena's and must not cycle (the rest of the block would be lost). `into`: NULL to
   acquire and submit a command buffer of its own (as since 0.1.0); otherwise the copy pass is recorded into that one
   (a frame waiting for the canvas, BTBH_GPU_CUSTOM_MESH_UPDATE_QUEUED's ordering rule) and nothing is submitted. */
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
    SDL_GPUTransferBufferCreateInfo vertex_transfer_info = {0};
    vertex_transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    vertex_transfer_info.size = vertex_byte_count;
    SDL_GPUTransferBuffer *vertex_transfer = SDL_CreateGPUTransferBuffer(
        device,
        &vertex_transfer_info
    );
    if (vertex_transfer == NULL) {
        btbh_gpu_custom_mesh_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return false;
    }

    void *mapped = SDL_MapGPUTransferBuffer(
        device,
        vertex_transfer,
        false
    );
    if (mapped == NULL) {
        btbh_gpu_custom_mesh_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        SDL_ReleaseGPUTransferBuffer(device, vertex_transfer);
        return false;
    }
    memcpy(mapped, vertices, vertex_byte_count);
    SDL_UnmapGPUTransferBuffer(device, vertex_transfer);

    SDL_GPUTransferBuffer *index_transfer = NULL;
    if (index_byte_count > 0u) {
        SDL_GPUTransferBufferCreateInfo index_transfer_info = {0};
        index_transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        index_transfer_info.size = index_byte_count;
        index_transfer = SDL_CreateGPUTransferBuffer(
            device,
            &index_transfer_info
        );
        if (index_transfer == NULL) {
            btbh_gpu_custom_mesh_error(
                report,
                BTBH_GPU_DEVICE_FAILURE,
                SDL_GetError()
            );
            SDL_ReleaseGPUTransferBuffer(device, vertex_transfer);
            return false;
        }
        mapped = SDL_MapGPUTransferBuffer(
            device,
            index_transfer,
            false
        );
        if (mapped == NULL) {
            btbh_gpu_custom_mesh_error(
                report,
                BTBH_GPU_DEVICE_FAILURE,
                SDL_GetError()
            );
            SDL_ReleaseGPUTransferBuffer(device, index_transfer);
            SDL_ReleaseGPUTransferBuffer(device, vertex_transfer);
            return false;
        }
        memcpy(mapped, indices, index_byte_count);
        SDL_UnmapGPUTransferBuffer(device, index_transfer);
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
        if (index_transfer != NULL) {
            SDL_ReleaseGPUTransferBuffer(device, index_transfer);
        }
        SDL_ReleaseGPUTransferBuffer(device, vertex_transfer);
        return false;
    }

    SDL_GPUTransferBufferLocation vertex_source = {0};
    vertex_source.transfer_buffer = vertex_transfer;
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
    if (index_byte_count > 0u) {
        SDL_GPUTransferBufferLocation index_source = {0};
        index_source.transfer_buffer = index_transfer;
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
    const bool submitted = into != NULL || SDL_SubmitGPUCommandBuffer(command);
    if (index_transfer != NULL) {
        SDL_ReleaseGPUTransferBuffer(device, index_transfer);
    }
    SDL_ReleaseGPUTransferBuffer(device, vertex_transfer);
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

/* ---- BTBH_GPU_CUSTOM_MESH_UPDATE_QUEUED (since 0.11.0): updates queued into the window's next scene ---- */

/* Each queued range starts at a multiple of this in the queue's bytes and in the scene's transfer buffer (Metal's
   blit encoder wants offsets in multiples of 4; 16 keeps every float vector aligned). */
#define BTBH_GPU_CUSTOM_MESH_QUEUE_ALIGNMENT 16u

static uint32_t btbh_gpu_custom_mesh_queue_align(uint32_t value) {
    return (value + (BTBH_GPU_CUSTOM_MESH_QUEUE_ALIGNMENT - 1u)) & ~(BTBH_GPU_CUSTOM_MESH_QUEUE_ALIGNMENT - 1u);
}

/* Takes the mesh's waiting update out of the queue (a later update replaces it, or the mesh goes): its entry and
   bytes stay as a hole until the next scene empties the queue. */
static void btbh_gpu_custom_mesh_queue_drop(BTBHGPUCustomMesh *mesh) {
    if (mesh->queue_entry == 0u || mesh->owner == NULL) {
        mesh->queue_entry = 0u;
        return;
    }
    BTBHGPUCustomMeshQueue *queue = &mesh->owner->mesh_queue;
    BTBHGPUCustomMeshQueueEntry *entry = &queue->entries[mesh->queue_entry - 1u];
    queue->live_count -= 1u;
    queue->live_bytes -= btbh_gpu_custom_mesh_queue_align(entry->vertex_byte_count) +
        btbh_gpu_custom_mesh_queue_align(entry->index_byte_count);
    entry->mesh = NULL;
    mesh->queue_entry = 0u;
}

static void btbh_gpu_custom_mesh_queue_clear(BTBHGPUCustomMeshQueue *queue) {
    for (uint32_t index = 0u; index < queue->entry_count; ++index) {
        if (queue->entries[index].mesh != NULL) {
            queue->entries[index].mesh->queue_entry = 0u;
            queue->entries[index].mesh = NULL;
        }
    }
    queue->entry_count = 0u;
    queue->live_count = 0u;
    queue->byte_count = 0u;
    queue->live_bytes = 0u;
    queue->recorded = false;
}

/* Room for one more entry and `byte_count` more bytes; false when memory runs out (nothing changed). */
static bool btbh_gpu_custom_mesh_queue_reserve(BTBHGPUCustomMeshQueue *queue, uint32_t byte_count) {
    if (queue->entry_count == queue->entry_capacity) {
        const uint32_t capacity = queue->entry_capacity == 0u ? 64u : queue->entry_capacity * 2u;
        BTBHGPUCustomMeshQueueEntry *entries =
            (BTBHGPUCustomMeshQueueEntry *) realloc(queue->entries, sizeof(*entries) * capacity);
        if (entries == NULL) {
            return false;
        }
        queue->entries = entries;
        queue->entry_capacity = capacity;
    }
    const uint64_t needed = (uint64_t) queue->byte_count + byte_count;
    if (needed > queue->byte_capacity) {
        uint64_t capacity = queue->byte_capacity == 0u ? 64u * 1024u : queue->byte_capacity;
        while (capacity < needed) {
            capacity *= 2u;
        }
        if (capacity > BTBH_GPU_CUSTOM_MESH_QUEUE_MAX_BYTES) {
            capacity = BTBH_GPU_CUSTOM_MESH_QUEUE_MAX_BYTES;
        }
        uint8_t *bytes = (uint8_t *) realloc(queue->bytes, (size_t) capacity);
        if (bytes == NULL) {
            return false;
        }
        queue->bytes = bytes;
        queue->byte_capacity = (uint32_t) capacity;
    }
    return true;
}

uint32_t btbh_gpu_custom_mesh_queue_size(const BTBHGPUWindow *window) {
    return window != NULL ? window->mesh_queue.live_bytes : 0u;
}

void btbh_gpu_custom_mesh_queue_write(const BTBHGPUWindow *window, uint8_t *destination) {
    const BTBHGPUCustomMeshQueue *queue = &window->mesh_queue;
    uint32_t offset = 0u;
    for (uint32_t index = 0u; index < queue->entry_count; ++index) {
        const BTBHGPUCustomMeshQueueEntry *entry = &queue->entries[index];
        if (entry->mesh == NULL) {
            continue;
        }
        memcpy(destination + offset, queue->bytes + entry->vertex_offset, entry->vertex_byte_count);
        offset += btbh_gpu_custom_mesh_queue_align(entry->vertex_byte_count);
        if (entry->index_byte_count > 0u) {
            memcpy(destination + offset, queue->bytes + entry->index_offset, entry->index_byte_count);
            offset += btbh_gpu_custom_mesh_queue_align(entry->index_byte_count);
        }
    }
}

uint32_t btbh_gpu_custom_mesh_queue_record(
    BTBHGPUWindow *window,
    SDL_GPUCopyPass *copy,
    SDL_GPUTransferBuffer *transfer,
    uint32_t offset
) {
    BTBHGPUCustomMeshQueue *queue = &window->mesh_queue;
    uint32_t uploads = 0u;
    for (uint32_t index = 0u; index < queue->entry_count; ++index) {
        const BTBHGPUCustomMeshQueueEntry *entry = &queue->entries[index];
        const BTBHGPUCustomMesh *mesh = entry->mesh;
        if (mesh == NULL) {
            continue;
        }
        /* Where the data goes is read now: a mesh with buffers of its own may have grown them at the call. A mesh of
           its own cycles (a frame still drawing it keeps the old data); a shared one is written in place. */
        const bool cycle = !btbh_gpu_custom_mesh_shared(mesh);
        SDL_GPUTransferBufferLocation source = {0};
        source.transfer_buffer = transfer;
        source.offset = offset;
        SDL_GPUBufferRegion destination = {0};
        destination.buffer = mesh->vertex_buffer;
        destination.offset = (uint32_t) mesh->vertex_base * BTBH_GPU_CUSTOM_MESH_VERTEX_STRIDE;
        destination.size = entry->vertex_byte_count;
        SDL_UploadToGPUBuffer(copy, &source, &destination, cycle);
        uploads += 1u;
        offset += btbh_gpu_custom_mesh_queue_align(entry->vertex_byte_count);
        if (entry->index_byte_count > 0u) {
            source.offset = offset;
            destination.buffer = mesh->index_buffer;
            destination.offset = mesh->first_index * (uint32_t) sizeof(uint32_t);
            destination.size = entry->index_byte_count;
            SDL_UploadToGPUBuffer(copy, &source, &destination, cycle);
            uploads += 1u;
            offset += btbh_gpu_custom_mesh_queue_align(entry->index_byte_count);
        }
    }
    queue->recorded = queue->live_count > 0u;
    return uploads;
}

void btbh_gpu_custom_mesh_queue_commit(BTBHGPUWindow *window, bool pending) {
    if (window == NULL) {
        return;
    }
    BTBHGPUCustomMeshQueue *queue = &window->mesh_queue;
    if (!queue->recorded) {
        queue->pending_serial = 0u;
        return;
    }
    queue->copied_serial += 1u;
    for (uint32_t index = 0u; index < queue->entry_count; ++index) {
        if (queue->entries[index].mesh != NULL) {
            queue->entries[index].mesh->queue_copied_serial = queue->copied_serial;
        }
    }
    queue->pending_serial = pending ? queue->copied_serial : 0u;
    btbh_gpu_custom_mesh_queue_clear(queue);
}

void btbh_gpu_window_release_custom_mesh_queue(BTBHGPUWindow *window) {
    if (window == NULL) {
        return;
    }
    BTBHGPUCustomMeshQueue *queue = &window->mesh_queue;
    btbh_gpu_custom_mesh_queue_clear(queue);
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
    BTBHGPUCustomMeshQueue *queue = &window->mesh_queue;
    SDL_GPUDevice *device = window->device->gpu;
    const uint32_t index_byte_count = index_count * (uint32_t) sizeof(uint32_t);
    const uint64_t queued_bytes = (uint64_t) btbh_gpu_custom_mesh_queue_align(vertex_byte_count) +
        (uint64_t) btbh_gpu_custom_mesh_queue_align(index_byte_count);
    if ((uint64_t) queue->byte_count + queued_bytes > BTBH_GPU_CUSTOM_MESH_QUEUE_MAX_BYTES) {
        char message[256];
        (void) snprintf(
            message,
            sizeof(message),
            "GPU custom mesh queued update: the window's queue holds %u bytes and this one adds %llu, past "
            "BTBH_GPU_CUSTOM_MESH_QUEUE_MAX_BYTES (%u); present a scene, or update without the flag",
            queue->byte_count,
            (unsigned long long) queued_bytes,
            (uint32_t) BTBH_GPU_CUSTOM_MESH_QUEUE_MAX_BYTES
        );
        btbh_gpu_custom_mesh_error(report, BTBH_GPU_INVALID_ARGUMENT, message);
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
    if (!btbh_gpu_custom_mesh_queue_reserve(queue, (uint32_t) queued_bytes)) {
        if (grow_indices) {
            SDL_ReleaseGPUBuffer(device, index_buffer);
        }
        if (grow_vertices) {
            SDL_ReleaseGPUBuffer(device, vertex_buffer);
        }
        btbh_gpu_custom_mesh_error(
            report,
            BTBH_GPU_PLATFORM_FAILURE,
            "Could not allocate room in the window's queue of GPU custom mesh updates"
        );
        return report->result;
    }
    /* A waiting update of this mesh is replaced: only the last one is copied. */
    btbh_gpu_custom_mesh_queue_drop(mesh);
    BTBHGPUCustomMeshQueueEntry *entry = &queue->entries[queue->entry_count];
    entry->mesh = mesh;
    entry->vertex_offset = queue->byte_count;
    entry->vertex_byte_count = vertex_byte_count;
    memcpy(queue->bytes + entry->vertex_offset, vertices, vertex_byte_count);
    queue->byte_count += btbh_gpu_custom_mesh_queue_align(vertex_byte_count);
    entry->index_offset = queue->byte_count;
    entry->index_byte_count = index_byte_count;
    if (index_byte_count > 0u) {
        memcpy(queue->bytes + entry->index_offset, indices, index_byte_count);
        queue->byte_count += btbh_gpu_custom_mesh_queue_align(index_byte_count);
    }
    queue->entry_count += 1u;
    queue->live_count += 1u;
    queue->live_bytes += (uint32_t) queued_bytes;
    mesh->queue_entry = queue->entry_count;
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
        SDL_GPUCommandBuffer *into =
            owner->pending_command != NULL &&
            mesh->queue_copied_serial != 0u &&
            mesh->queue_copied_serial == owner->mesh_queue.pending_serial
                ? owner->pending_command
                : NULL;
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
        /* A queued update not yet copied is older: this one replaces it. */
        btbh_gpu_custom_mesh_queue_drop(mesh);
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
    /* A queued update not yet copied is older: this one replaces it (a copied one cycled the buffers or was recorded
       before this submission's frame, so it lands first either way). */
    btbh_gpu_custom_mesh_queue_drop(mesh);
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
    /* Its queued update goes with it (one a frame already recorded still runs: SDL frees the buffers after it). */
    btbh_gpu_custom_mesh_queue_drop(mesh);
    btbh_gpu_custom_mesh_release_handles(mesh, device);
    btbh_gpu_custom_mesh_unlink(mesh);
    free(mesh);
}

void btbh_gpu_window_release_custom_meshes(BTBHGPUWindow *window) {
    if (window == NULL) {
        return;
    }
    btbh_gpu_window_release_custom_mesh_queue(window);
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
