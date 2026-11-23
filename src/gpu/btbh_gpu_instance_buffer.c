#include "btbh_gpu_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void btbh_gpu_instance_buffer_error(
    BTBHGPUInstanceBufferReport *report,
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

static void btbh_gpu_instance_buffer_unlink(
    BTBHGPUInstanceBuffer *buffer
) {
    BTBHGPUWindow *owner = buffer->owner;
    if (owner == NULL) {
        return;
    }
    if (buffer->previous != NULL) {
        buffer->previous->next = buffer->next;
    } else if (owner->instance_buffer_head == buffer) {
        owner->instance_buffer_head = buffer->next;
    }
    if (buffer->next != NULL) {
        buffer->next->previous = buffer->previous;
    }
    buffer->owner = NULL;
    buffer->previous = NULL;
    buffer->next = NULL;
}

static void btbh_gpu_instance_buffer_release_handle(
    BTBHGPUInstanceBuffer *buffer,
    SDL_GPUDevice *device
) {
    if (device != NULL && buffer->buffer != NULL) {
        SDL_ReleaseGPUBuffer(device, buffer->buffer);
    }
    if (device != NULL && buffer->vertex_buffer != NULL) {
        SDL_ReleaseGPUBuffer(device, buffer->vertex_buffer);
    }
    buffer->buffer = NULL;
    buffer->vertex_buffer = NULL;
}

BTBHGPUInstanceBuffer *btbh_gpu_window_instance_buffer_create(
    BTBHGPUWindow *window,
    uint32_t capacity,
    uint32_t stride,
    BTBHGPUInstanceBufferReport *report
) {
    if (report == NULL) {
        return NULL;
    }
    memset(report, 0, sizeof(*report));
    if (
        window == NULL ||
        window->device == NULL ||
        window->device->gpu == NULL ||
        window->device->shutdown ||
        !window->claimed ||
        capacity == 0u ||
        stride == 0u
    ) {
        btbh_gpu_instance_buffer_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            "Invalid GPU instance buffer descriptor"
        );
        return NULL;
    }
    const uint64_t byte_capacity =
        (uint64_t) capacity * (uint64_t) stride;
    if (byte_capacity == 0u || byte_capacity > UINT32_MAX) {
        btbh_gpu_instance_buffer_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            "GPU instance buffer exceeds SDL_GPU limits"
        );
        return NULL;
    }

    BTBHGPUInstanceBuffer *resource = calloc(1, sizeof(*resource));
    if (resource == NULL) {
        btbh_gpu_instance_buffer_error(
            report,
            BTBH_GPU_PLATFORM_FAILURE,
            "Could not allocate GPU instance buffer resource"
        );
        return NULL;
    }
    SDL_GPUBufferCreateInfo info = {0};
    info.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    info.size = (uint32_t) byte_capacity;
    resource->buffer = SDL_CreateGPUBuffer(
        window->device->gpu,
        &info
    );
    if (resource->buffer == NULL) {
        btbh_gpu_instance_buffer_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        free(resource);
        return NULL;
    }
    info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    resource->vertex_buffer = SDL_CreateGPUBuffer(
        window->device->gpu,
        &info
    );
    if (resource->vertex_buffer == NULL) {
        btbh_gpu_instance_buffer_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        SDL_ReleaseGPUBuffer(window->device->gpu, resource->buffer);
        resource->buffer = NULL;
        free(resource);
        return NULL;
    }

    resource->owner = window;
    resource->next = window->instance_buffer_head;
    if (window->instance_buffer_head != NULL) {
        window->instance_buffer_head->previous = resource;
    }
    window->instance_buffer_head = resource;
    resource->capacity = capacity;
    resource->stride = stride;
    resource->byte_capacity = (uint32_t) byte_capacity;

    report->result = BTBH_GPU_OK;
    report->capacity = capacity;
    report->stride = stride;
    report->byte_capacity = resource->byte_capacity;
    return resource;
}

/* The upload was made (submitted, recorded or queued): the uploaded prefix grows over it, the report is filled. */
static BTBHGPUResult btbh_gpu_instance_buffer_uploaded(
    BTBHGPUInstanceBuffer *buffer,
    uint32_t byte_offset,
    uint32_t byte_count,
    BTBHGPUInstanceBufferReport *report
) {
    const uint32_t uploaded_end = byte_offset + byte_count;
    if (
        byte_offset <= buffer->uploaded_prefix_byte_count &&
        uploaded_end > buffer->uploaded_prefix_byte_count
    ) {
        buffer->uploaded_prefix_byte_count = uploaded_end;
    }
    report->result = BTBH_GPU_OK;
    report->capacity = buffer->capacity;
    report->stride = buffer->stride;
    report->byte_capacity = buffer->byte_capacity;
    return report->result;
}

BTBHGPUResult btbh_gpu_instance_buffer_upload(
    BTBHGPUInstanceBuffer *buffer,
    uint32_t byte_offset,
    const void *bytes,
    uint32_t byte_count,
    BTBHGPUInstanceBufferReport *report
) {
    return btbh_gpu_instance_buffer_upload_ex(buffer, byte_offset, bytes, byte_count, 0u, report);
}

BTBHGPUResult btbh_gpu_instance_buffer_upload_ex(
    BTBHGPUInstanceBuffer *buffer,
    uint32_t byte_offset,
    const void *bytes,
    uint32_t byte_count,
    uint32_t flags,
    BTBHGPUInstanceBufferReport *report
) {
    if (report == NULL) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    memset(report, 0, sizeof(*report));
    if ((flags & ~(uint32_t) (
            BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED | BTBH_GPU_INSTANCE_BUFFER_UPLOAD_NO_CYCLE
        )) != 0u) {
        btbh_gpu_instance_buffer_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            "Unknown GPU instance buffer upload flags (BTBH_GPU_INSTANCE_BUFFER_UPLOAD_*)"
        );
        return report->result;
    }
    if (
        buffer == NULL ||
        buffer->owner == NULL ||
        buffer->owner->device == NULL ||
        buffer->owner->device->gpu == NULL ||
        buffer->owner->device->shutdown ||
        buffer->buffer == NULL ||
        buffer->vertex_buffer == NULL ||
        bytes == NULL ||
        byte_count == 0u ||
        byte_offset > buffer->byte_capacity ||
        byte_count > buffer->byte_capacity - byte_offset
    ) {
        btbh_gpu_instance_buffer_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            "Invalid GPU instance buffer upload"
        );
        return report->result;
    }

    BTBHGPUWindow *owner = buffer->owner;
    if ((flags & BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED) != 0u) {
        /* Copied into the window's upload queue now; the next scene copies it in place, in its own copy pass. */
        char message[256];
        bool out_of_memory = false;
        if (!btbh_gpu_upload_queue_prepare(
                owner,
                byte_count,
                0u,
                "GPU instance buffer queued upload",
                message,
                sizeof(message),
                &out_of_memory
            )
        ) {
            btbh_gpu_instance_buffer_error(
                report,
                out_of_memory ? BTBH_GPU_PLATFORM_FAILURE : BTBH_GPU_INVALID_ARGUMENT,
                message
            );
            return report->result;
        }
        btbh_gpu_upload_queue_push(
            owner,
            NULL,
            buffer,
            BTBH_GPU_UPLOAD_INSTANCES,
            byte_offset,
            bytes,
            byte_count,
            false
        );
        return btbh_gpu_instance_buffer_uploaded(buffer, byte_offset, byte_count, report);
    }
    /* Without flags: a submission of its own that cycles the buffers (as since 0.1.0: a frame still drawing them keeps
       its data, and the rest of the buffer is undefined after it). BTBH_GPU_INSTANCE_BUFFER_UPLOAD_NO_CYCLE (since
       0.11.0): in place, so the rest keeps its data; while a scene waits for the canvas, recorded into that frame after
       its draws (it keeps the old data, and a queued upload it copied lands first). */
    const bool cycle = (flags & BTBH_GPU_INSTANCE_BUFFER_UPLOAD_NO_CYCLE) == 0u;
    SDL_GPUCommandBuffer *into = cycle ? NULL : owner->pending_command;

    SDL_GPUDevice *device = buffer->owner->device->gpu;
    /* (From the upload staging pool with a budget, unless recorded into the waiting frame.) */
    uint32_t staging_offset = 0u;
    SDL_GPUTransferBuffer *transfer = btbh_gpu_staging_acquire_into(device, into, byte_count, &staging_offset);
    if (transfer == NULL) {
        btbh_gpu_instance_buffer_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return report->result;
    }
    void *mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
    if (mapped == NULL) {
        btbh_gpu_instance_buffer_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        btbh_gpu_staging_abandon(device, transfer);
        return report->result;
    }
    memcpy((uint8_t *)mapped + staging_offset, bytes, byte_count);
    SDL_UnmapGPUTransferBuffer(device, transfer);

    SDL_GPUCommandBuffer *command = into != NULL ? into : SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass *copy = command != NULL
        ? SDL_BeginGPUCopyPass(command)
        : NULL;
    if (copy == NULL) {
        btbh_gpu_instance_buffer_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        if (command != NULL && into == NULL) {
            (void) SDL_CancelGPUCommandBuffer(command);
        }
        btbh_gpu_staging_abandon(device, transfer);
        return report->result;
    }
    SDL_GPUTransferBufferLocation source = {0};
    source.transfer_buffer = transfer;
    source.offset = staging_offset;
    SDL_GPUBufferRegion destination = {0};
    destination.buffer = buffer->buffer;
    destination.offset = byte_offset;
    destination.size = byte_count;
    SDL_UploadToGPUBuffer(copy, &source, &destination, cycle);
    destination.buffer = buffer->vertex_buffer;
    SDL_UploadToGPUBuffer(copy, &source, &destination, cycle);
    SDL_EndGPUCopyPass(copy);
    /* (Recorded into a frame, the transfer buffer is released now and freed by SDL once that frame has run.) */
    const bool submitted = btbh_gpu_staging_submit_into(device, command, into, &transfer, 1u);
    if (!submitted) {
        btbh_gpu_instance_buffer_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return report->result;
    }
    if (into != NULL) {
        btbh_gpu_upload_queue_note_pending_write(owner);
    }
    /* Queued uploads not yet copied are older: where they overlap this one they carry its bytes, and those it covers
       are dropped. (Nothing to do for a host that never queues.) */
    btbh_gpu_upload_queue_overwrite(owner, NULL, buffer, BTBH_GPU_UPLOAD_INSTANCES, byte_offset, bytes, byte_count);
    return btbh_gpu_instance_buffer_uploaded(buffer, byte_offset, byte_count, report);
}

void btbh_gpu_instance_buffer_destroy(
    BTBHGPUInstanceBuffer *buffer
) {
    if (buffer == NULL) {
        return;
    }
    SDL_GPUDevice *device = NULL;
    if (
        buffer->owner != NULL &&
        buffer->owner->device != NULL &&
        !buffer->owner->device->shutdown
    ) {
        device = buffer->owner->device->gpu;
    }
    /* Its queued uploads go with it (one a frame already recorded still runs: SDL frees the buffers after it). */
    btbh_gpu_upload_queue_drop(buffer->owner, NULL, buffer);
    btbh_gpu_instance_buffer_release_handle(buffer, device);
    btbh_gpu_instance_buffer_unlink(buffer);
    free(buffer);
}

uint8_t btbh_gpu_pack_transform_stream_instances(
    float *destination,
    const void *positions,
    uint32_t position_stride,
    const void *rotations,
    uint32_t rotation_stride,
    const void *scales,
    uint32_t scale_stride,
    const void *colors,
    uint32_t color_stride,
    const void *customs,
    uint32_t custom_stride,
    uint32_t source_start,
    uint32_t count
) {
    if (
        destination == NULL ||
        positions == NULL ||
        position_stride < sizeof(float) * 3u ||
        rotations == NULL ||
        rotation_stride < sizeof(float) * 4u ||
        scales == NULL ||
        scale_stride < sizeof(float) * 3u ||
        (colors != NULL && color_stride < sizeof(uint8_t) * 4u) ||
        (customs != NULL && custom_stride < sizeof(float) * 4u)
    ) {
        return 0u;
    }

    const uint8_t *position_bytes = (const uint8_t *)positions;
    const uint8_t *rotation_bytes = (const uint8_t *)rotations;
    const uint8_t *scale_bytes = (const uint8_t *)scales;
    const uint8_t *color_bytes = (const uint8_t *)colors;
    const uint8_t *custom_bytes = (const uint8_t *)customs;
    for (uint32_t output_index = 0u;
         output_index < count;
         ++output_index) {
        const uint32_t source_index = source_start + output_index;
        const float *position = (const float *)(
            position_bytes + (size_t)source_index * position_stride
        );
        const float *rotation = (const float *)(
            rotation_bytes + (size_t)source_index * rotation_stride
        );
        const float *scale = (const float *)(
            scale_bytes + (size_t)source_index * scale_stride
        );
        const float x = rotation[0];
        const float y = rotation[1];
        const float z = rotation[2];
        const float w = rotation[3];
        const float xx = x * x;
        const float yy = y * y;
        const float zz = z * z;
        const float xy = x * y;
        const float xz = x * z;
        const float yz = y * z;
        const float wx = w * x;
        const float wy = w * y;
        const float wz = w * z;
        float *record = destination + (size_t)output_index * 24u;

        record[0] = (1.0f - 2.0f * (yy + zz)) * scale[0];
        record[1] = 2.0f * (xy + wz) * scale[0];
        record[2] = 2.0f * (xz - wy) * scale[0];
        record[3] = 0.0f;
        record[4] = 2.0f * (xy - wz) * scale[1];
        record[5] = (1.0f - 2.0f * (xx + zz)) * scale[1];
        record[6] = 2.0f * (yz + wx) * scale[1];
        record[7] = 0.0f;
        record[8] = 2.0f * (xz + wy) * scale[2];
        record[9] = 2.0f * (yz - wx) * scale[2];
        record[10] = (1.0f - 2.0f * (xx + yy)) * scale[2];
        record[11] = 0.0f;
        record[12] = position[0];
        record[13] = position[1];
        record[14] = position[2];
        record[15] = 1.0f;

        if (color_bytes != NULL) {
            const uint8_t *color =
                color_bytes + (size_t)source_index * color_stride;
            record[16] = (float)color[0] / 255.0f;
            record[17] = (float)color[1] / 255.0f;
            record[18] = (float)color[2] / 255.0f;
            record[19] = (float)color[3] / 255.0f;
        } else {
            record[16] = 1.0f;
            record[17] = 1.0f;
            record[18] = 1.0f;
            record[19] = 1.0f;
        }
        if (custom_bytes != NULL) {
            const float *custom = (const float *)(
                custom_bytes + (size_t)source_index * custom_stride
            );
            record[20] = custom[0];
            record[21] = custom[1];
            record[22] = custom[2];
            record[23] = custom[3];
        } else {
            record[20] = 0.0f;
            record[21] = 0.0f;
            record[22] = 0.0f;
            record[23] = 0.0f;
        }
    }
    return 1u;
}

void btbh_gpu_window_release_instance_buffers(BTBHGPUWindow *window) {
    if (window == NULL) {
        return;
    }
    SDL_GPUDevice *device =
        window->device != NULL && !window->device->shutdown
            ? window->device->gpu
            : NULL;
    BTBHGPUInstanceBuffer *buffer = window->instance_buffer_head;
    window->instance_buffer_head = NULL;
    while (buffer != NULL) {
        BTBHGPUInstanceBuffer *next = buffer->next;
        btbh_gpu_instance_buffer_release_handle(buffer, device);
        buffer->owner = NULL;
        buffer->previous = NULL;
        buffer->next = NULL;
        buffer = next;
    }
}
