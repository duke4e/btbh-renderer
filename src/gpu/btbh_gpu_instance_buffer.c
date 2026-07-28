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
    buffer->buffer = NULL;
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

BTBHGPUResult btbh_gpu_instance_buffer_upload(
    BTBHGPUInstanceBuffer *buffer,
    uint32_t byte_offset,
    const void *bytes,
    uint32_t byte_count,
    BTBHGPUInstanceBufferReport *report
) {
    if (report == NULL) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    memset(report, 0, sizeof(*report));
    if (
        buffer == NULL ||
        buffer->owner == NULL ||
        buffer->owner->device == NULL ||
        buffer->owner->device->gpu == NULL ||
        buffer->owner->device->shutdown ||
        buffer->buffer == NULL ||
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

    SDL_GPUDevice *device = buffer->owner->device->gpu;
    SDL_GPUTransferBufferCreateInfo transfer_info = {0};
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size = byte_count;
    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(
        device,
        &transfer_info
    );
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
        SDL_ReleaseGPUTransferBuffer(device, transfer);
        return report->result;
    }
    memcpy(mapped, bytes, byte_count);
    SDL_UnmapGPUTransferBuffer(device, transfer);

    SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass *copy = command != NULL
        ? SDL_BeginGPUCopyPass(command)
        : NULL;
    if (copy == NULL) {
        btbh_gpu_instance_buffer_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        if (command != NULL) {
            (void) SDL_CancelGPUCommandBuffer(command);
        }
        SDL_ReleaseGPUTransferBuffer(device, transfer);
        return report->result;
    }
    SDL_GPUTransferBufferLocation source = {0};
    source.transfer_buffer = transfer;
    SDL_GPUBufferRegion destination = {0};
    destination.buffer = buffer->buffer;
    destination.offset = byte_offset;
    destination.size = byte_count;
    SDL_UploadToGPUBuffer(copy, &source, &destination, true);
    SDL_EndGPUCopyPass(copy);
    const bool submitted = SDL_SubmitGPUCommandBuffer(command);
    SDL_ReleaseGPUTransferBuffer(device, transfer);
    if (!submitted) {
        btbh_gpu_instance_buffer_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return report->result;
    }

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
    btbh_gpu_instance_buffer_release_handle(buffer, device);
    btbh_gpu_instance_buffer_unlink(buffer);
    free(buffer);
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
