#include "btbh_gpu_internal.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STBI_NO_STDIO
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#endif
#include "stb_image.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

static void btbh_gpu_texture_unlink(BTBHGPUTexture *texture) {
    BTBHGPUWindow *owner = texture->owner;
    if (owner == NULL) {
        return;
    }
    if (texture->previous != NULL) {
        texture->previous->next = texture->next;
    } else if (owner->texture_head == texture) {
        owner->texture_head = texture->next;
    }
    if (texture->next != NULL) {
        texture->next->previous = texture->previous;
    }
    texture->owner = NULL;
    texture->previous = NULL;
    texture->next = NULL;
}

static void btbh_gpu_texture_release_handles(
    BTBHGPUTexture *texture,
    SDL_GPUDevice *device
) {
    if (device != NULL) {
        if (texture->sampler != NULL) {
            SDL_ReleaseGPUSampler(device, texture->sampler);
        }
        if (texture->texture != NULL) {
            SDL_ReleaseGPUTexture(device, texture->texture);
        }
    }
    texture->sampler = NULL;
    texture->texture = NULL;
}

static void btbh_gpu_texture_error(
    BTBHGPUTextureReport *report,
    const char *message
) {
    report->result = BTBH_GPU_DEVICE_FAILURE;
    (void) snprintf(
        report->message,
        sizeof(report->message),
        "%s",
        message != NULL ? message : ""
    );
}

static uint32_t btbh_gpu_texture_mip_count(
    uint32_t width,
    uint32_t height
) {
    uint32_t extent = width > height ? width : height;
    uint32_t count = 1u;
    while (extent > 1u) {
        extent >>= 1u;
        count += 1u;
    }
    return count;
}

static bool btbh_gpu_read_file(
    const char *path,
    uint8_t **bytes,
    size_t *size
) {
    SDL_IOStream *stream = SDL_IOFromFile(path, "rb");
    if (stream == NULL) {
        return false;
    }
    const Sint64 length = SDL_GetIOSize(stream);
    if (length <= 0 || (uint64_t) length > (uint64_t) SIZE_MAX) {
        SDL_CloseIO(stream);
        return false;
    }
    uint8_t *data = malloc((size_t) length);
    if (data == NULL) {
        SDL_CloseIO(stream);
        return false;
    }
    const size_t read_count = SDL_ReadIO(
        stream,
        data,
        (size_t) length
    );
    SDL_CloseIO(stream);
    if (read_count != (size_t) length) {
        free(data);
        return false;
    }
    *bytes = data;
    *size = read_count;
    return true;
}

static SDL_GPUFilter btbh_gpu_sampler_filter(
    BTBHGPUTextureFilter filter
) {
    return filter == BTBH_GPU_TEXTURE_FILTER_NEAREST
        ? SDL_GPU_FILTER_NEAREST
        : SDL_GPU_FILTER_LINEAR;
}

static SDL_GPUSamplerAddressMode btbh_gpu_sampler_address(
    BTBHGPUTextureWrap wrap
) {
    switch (wrap) {
        case BTBH_GPU_TEXTURE_WRAP_REPEAT:
            return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        case BTBH_GPU_TEXTURE_WRAP_MIRROR:
            return SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT;
        case BTBH_GPU_TEXTURE_WRAP_CLAMP:
        default:
            return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    }
}

BTBHGPUTexture *btbh_gpu_window_texture_create_empty(
    BTBHGPUWindow *window,
    uint32_t width,
    uint32_t height,
    SDL_GPUTextureFormat format,
    SDL_GPUTextureUsageFlags usage,
    BTBHGPUTextureFilter filter,
    BTBHGPUTextureWrap wrap,
    BTBHGPUTextureReport *report
) {
    if (
        window == NULL ||
        window->device == NULL ||
        window->device->gpu == NULL ||
        window->device->shutdown ||
        !window->claimed ||
        width == 0u ||
        height == 0u ||
        format == SDL_GPU_TEXTUREFORMAT_INVALID ||
        usage == 0u ||
        report == NULL
    ) {
        return NULL;
    }
    (void)memset(report, 0, sizeof(*report));
    BTBHGPUTexture *resource = calloc(1, sizeof(*resource));
    if (resource == NULL) {
        btbh_gpu_texture_error(
            report,
            "Could not allocate texture resource"
        );
        return NULL;
    }
    resource->owner = window;
    resource->next = window->texture_head;
    if (window->texture_head != NULL) {
        window->texture_head->previous = resource;
    }
    window->texture_head = resource;
    resource->width = width;
    resource->height = height;
    resource->mip_count = 1u;
    resource->format = format;

    SDL_GPUTextureCreateInfo texture_info;
    SDL_zero(texture_info);
    texture_info.type = SDL_GPU_TEXTURETYPE_2D;
    texture_info.format = format;
    texture_info.usage = usage;
    texture_info.width = width;
    texture_info.height = height;
    texture_info.layer_count_or_depth = 1u;
    texture_info.num_levels = 1u;
    texture_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    resource->texture = SDL_CreateGPUTexture(
        window->device->gpu,
        &texture_info
    );

    SDL_GPUSamplerCreateInfo sampler_info;
    SDL_zero(sampler_info);
    sampler_info.min_filter = btbh_gpu_sampler_filter(filter);
    sampler_info.mag_filter = btbh_gpu_sampler_filter(filter);
    sampler_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler_info.address_mode_u = btbh_gpu_sampler_address(wrap);
    sampler_info.address_mode_v = btbh_gpu_sampler_address(wrap);
    sampler_info.address_mode_w = btbh_gpu_sampler_address(wrap);
    resource->sampler = SDL_CreateGPUSampler(
        window->device->gpu,
        &sampler_info
    );
    if (resource->texture == NULL || resource->sampler == NULL) {
        btbh_gpu_texture_error(report, SDL_GetError());
        btbh_gpu_texture_unlink(resource);
        btbh_gpu_texture_release_handles(
            resource,
            window->device->gpu
        );
        free(resource);
        return NULL;
    }
    report->result = BTBH_GPU_OK;
    report->width = width;
    report->height = height;
    report->mip_count = 1u;
    report->message[0] = '\0';
    return resource;
}

BTBHGPUTexture *btbh_gpu_window_render_target_create(
    BTBHGPUWindow *window,
    uint32_t width,
    uint32_t height,
    BTBHGPUTextureFilter filter,
    BTBHGPUTextureReport *report
) {
    if (
        window == NULL ||
        window->window == NULL ||
        window->device == NULL ||
        window->device->gpu == NULL ||
        report == NULL
    ) {
        return NULL;
    }
    const SDL_GPUTextureFormat format =
        SDL_GetGPUSwapchainTextureFormat(
            window->device->gpu,
            window->window
        );
    return btbh_gpu_window_texture_create_empty(
        window,
        width,
        height,
        format,
        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
            SDL_GPU_TEXTUREUSAGE_SAMPLER,
        filter,
        BTBH_GPU_TEXTURE_WRAP_CLAMP,
        report
    );
}

BTBHGPUTexture *btbh_gpu_window_texture_create_rgba8(
    BTBHGPUWindow *window,
    const uint8_t *pixels,
    uint32_t width,
    uint32_t height,
    BTBHGPUTextureFilter filter,
    BTBHGPUTextureWrap wrap,
    BTBHGPUTextureReport *report
) {
    if (
        window == NULL ||
        window->device == NULL ||
        window->device->gpu == NULL ||
        window->device->shutdown ||
        !window->claimed ||
        pixels == NULL ||
        width == 0u ||
        height == 0u ||
        report == NULL
    ) {
        return NULL;
    }
    (void)memset(report, 0, sizeof(*report));
    const uint64_t byte_count =
        (uint64_t)width * (uint64_t)height * UINT64_C(4);
    if (byte_count > UINT32_MAX) {
        btbh_gpu_texture_error(report, "Texture upload exceeds SDL_GPU limits");
        return NULL;
    }
    BTBHGPUTexture *resource = calloc(1, sizeof(*resource));
    if (resource == NULL) {
        btbh_gpu_texture_error(report, "Could not allocate texture resource");
        return NULL;
    }
    resource->owner = window;
    resource->next = window->texture_head;
    if (window->texture_head != NULL) {
        window->texture_head->previous = resource;
    }
    window->texture_head = resource;
    resource->width = width;
    resource->height = height;
    resource->mip_count = 1u;
    resource->format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

    SDL_GPUTextureCreateInfo texture_info = {0};
    texture_info.type = SDL_GPU_TEXTURETYPE_2D;
    texture_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    texture_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    texture_info.width = width;
    texture_info.height = height;
    texture_info.layer_count_or_depth = 1u;
    texture_info.num_levels = 1u;
    texture_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    resource->texture = SDL_CreateGPUTexture(
        window->device->gpu,
        &texture_info
    );

    SDL_GPUSamplerCreateInfo sampler_info = {0};
    sampler_info.min_filter = btbh_gpu_sampler_filter(filter);
    sampler_info.mag_filter = btbh_gpu_sampler_filter(filter);
    sampler_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler_info.address_mode_u = btbh_gpu_sampler_address(wrap);
    sampler_info.address_mode_v = btbh_gpu_sampler_address(wrap);
    sampler_info.address_mode_w = btbh_gpu_sampler_address(wrap);
    resource->sampler = SDL_CreateGPUSampler(
        window->device->gpu,
        &sampler_info
    );

    SDL_GPUTransferBufferCreateInfo transfer_info = {0};
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size = (uint32_t)byte_count;
    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(
        window->device->gpu,
        &transfer_info
    );
    if (
        resource->texture == NULL ||
        resource->sampler == NULL ||
        transfer == NULL
    ) {
        btbh_gpu_texture_error(report, SDL_GetError());
        if (transfer != NULL) {
            SDL_ReleaseGPUTransferBuffer(window->device->gpu, transfer);
        }
        btbh_gpu_texture_destroy(resource);
        return NULL;
    }
    void *mapped = SDL_MapGPUTransferBuffer(
        window->device->gpu,
        transfer,
        false
    );
    if (mapped == NULL) {
        btbh_gpu_texture_error(report, SDL_GetError());
        SDL_ReleaseGPUTransferBuffer(window->device->gpu, transfer);
        btbh_gpu_texture_destroy(resource);
        return NULL;
    }
    (void)memcpy(mapped, pixels, (size_t)byte_count);
    SDL_UnmapGPUTransferBuffer(window->device->gpu, transfer);

    SDL_GPUCommandBuffer *command =
        SDL_AcquireGPUCommandBuffer(window->device->gpu);
    SDL_GPUCopyPass *copy = command != NULL
        ? SDL_BeginGPUCopyPass(command)
        : NULL;
    if (copy == NULL) {
        btbh_gpu_texture_error(report, SDL_GetError());
        if (command != NULL) {
            (void)SDL_CancelGPUCommandBuffer(command);
        }
        SDL_ReleaseGPUTransferBuffer(window->device->gpu, transfer);
        btbh_gpu_texture_destroy(resource);
        return NULL;
    }
    SDL_GPUTextureTransferInfo source = {0};
    source.transfer_buffer = transfer;
    source.pixels_per_row = width;
    source.rows_per_layer = height;
    SDL_GPUTextureRegion destination = {0};
    destination.texture = resource->texture;
    destination.w = width;
    destination.h = height;
    destination.d = 1u;
    SDL_UploadToGPUTexture(copy, &source, &destination, false);
    SDL_EndGPUCopyPass(copy);
    const bool submitted = SDL_SubmitGPUCommandBuffer(command);
    SDL_ReleaseGPUTransferBuffer(window->device->gpu, transfer);
    if (!submitted) {
        btbh_gpu_texture_error(report, SDL_GetError());
        btbh_gpu_texture_destroy(resource);
        return NULL;
    }
    report->result = BTBH_GPU_OK;
    report->width = width;
    report->height = height;
    report->mip_count = 1u;
    return resource;
}

BTBHGPUTexture *btbh_gpu_window_texture_load_memory(
    BTBHGPUWindow *window,
    const uint8_t *encoded,
    size_t encoded_size,
    BTBHGPUTextureFilter filter,
    BTBHGPUTextureWrap wrap,
    BTBHGPUTextureReport *report
) {
    if (
        encoded == NULL ||
        encoded_size == 0u ||
        encoded_size > (size_t)INT_MAX ||
        report == NULL
    ) {
        return NULL;
    }
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc *pixels = stbi_load_from_memory(
        encoded,
        (int)encoded_size,
        &width,
        &height,
        &channels,
        4
    );
    if (pixels == NULL || width <= 0 || height <= 0) {
        btbh_gpu_texture_error(report, stbi_failure_reason());
        stbi_image_free(pixels);
        return NULL;
    }
    BTBHGPUTexture *texture = btbh_gpu_window_texture_create_rgba8(
        window,
        pixels,
        (uint32_t)width,
        (uint32_t)height,
        filter,
        wrap,
        report
    );
    stbi_image_free(pixels);
    return texture;
}

BTBHGPUTexture *btbh_gpu_window_texture_load(
    BTBHGPUWindow *window,
    const char *path,
    uint8_t generate_mipmaps,
    BTBHGPUTextureFilter filter,
    BTBHGPUTextureWrap wrap,
    BTBHGPUTextureReport *report
) {
    if (
        window == NULL ||
        window->device == NULL ||
        window->device->gpu == NULL ||
        window->device->shutdown ||
        !window->claimed ||
        path == NULL ||
        report == NULL
    ) {
        return NULL;
    }
    memset(report, 0, sizeof(*report));

    uint8_t *encoded = NULL;
    size_t encoded_size = 0u;
    if (!btbh_gpu_read_file(path, &encoded, &encoded_size)) {
        btbh_gpu_texture_error(report, SDL_GetError());
        return NULL;
    }
    if (encoded_size > (size_t) INT_MAX) {
        free(encoded);
        btbh_gpu_texture_error(report, "Texture file is too large");
        return NULL;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc *pixels = stbi_load_from_memory(
        encoded,
        (int) encoded_size,
        &width,
        &height,
        &channels,
        4
    );
    free(encoded);
    if (pixels == NULL || width <= 0 || height <= 0) {
        btbh_gpu_texture_error(report, stbi_failure_reason());
        stbi_image_free(pixels);
        return NULL;
    }

    const uint64_t byte_count =
        (uint64_t) (uint32_t) width *
        (uint64_t) (uint32_t) height *
        UINT64_C(4);
    if (byte_count > UINT32_MAX) {
        stbi_image_free(pixels);
        btbh_gpu_texture_error(report, "Texture upload exceeds SDL_GPU limits");
        return NULL;
    }

    BTBHGPUTexture *resource = calloc(1, sizeof(*resource));
    if (resource == NULL) {
        stbi_image_free(pixels);
        btbh_gpu_texture_error(report, "Could not allocate texture resource");
        return NULL;
    }
    resource->owner = window;
    resource->next = window->texture_head;
    if (window->texture_head != NULL) {
        window->texture_head->previous = resource;
    }
    window->texture_head = resource;
    resource->width = (uint32_t) width;
    resource->height = (uint32_t) height;
    resource->mip_count = generate_mipmaps != 0
        ? btbh_gpu_texture_mip_count(resource->width, resource->height)
        : 1u;
    resource->format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

    SDL_GPUTextureCreateInfo texture_info = {0};
    texture_info.type = SDL_GPU_TEXTURETYPE_2D;
    texture_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    texture_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    if (resource->mip_count > 1u) {
        texture_info.usage |= SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    }
    texture_info.width = resource->width;
    texture_info.height = resource->height;
    texture_info.layer_count_or_depth = 1u;
    texture_info.num_levels = resource->mip_count;
    texture_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    resource->texture = SDL_CreateGPUTexture(
        window->device->gpu,
        &texture_info
    );

    SDL_GPUSamplerCreateInfo sampler_info = {0};
    sampler_info.min_filter = btbh_gpu_sampler_filter(filter);
    sampler_info.mag_filter = btbh_gpu_sampler_filter(filter);
    sampler_info.mipmap_mode =
        filter == BTBH_GPU_TEXTURE_FILTER_TRILINEAR
            ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR
            : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler_info.address_mode_u = btbh_gpu_sampler_address(wrap);
    sampler_info.address_mode_v = btbh_gpu_sampler_address(wrap);
    sampler_info.address_mode_w = btbh_gpu_sampler_address(wrap);
    sampler_info.max_lod = (float) (resource->mip_count - 1u);
    resource->sampler = SDL_CreateGPUSampler(
        window->device->gpu,
        &sampler_info
    );

    SDL_GPUTransferBufferCreateInfo transfer_info = {0};
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size = (uint32_t) byte_count;
    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(
        window->device->gpu,
        &transfer_info
    );
    if (
        resource->texture == NULL ||
        resource->sampler == NULL ||
        transfer == NULL
    ) {
        btbh_gpu_texture_error(report, SDL_GetError());
        stbi_image_free(pixels);
        if (transfer != NULL) {
            SDL_ReleaseGPUTransferBuffer(window->device->gpu, transfer);
        }
        btbh_gpu_texture_destroy(resource);
        return NULL;
    }

    void *mapped = SDL_MapGPUTransferBuffer(
        window->device->gpu,
        transfer,
        false
    );
    if (mapped == NULL) {
        btbh_gpu_texture_error(report, SDL_GetError());
        stbi_image_free(pixels);
        SDL_ReleaseGPUTransferBuffer(window->device->gpu, transfer);
        btbh_gpu_texture_destroy(resource);
        return NULL;
    }
    memcpy(mapped, pixels, (size_t) byte_count);
    SDL_UnmapGPUTransferBuffer(window->device->gpu, transfer);
    stbi_image_free(pixels);

    SDL_GPUCommandBuffer *command =
        SDL_AcquireGPUCommandBuffer(window->device->gpu);
    SDL_GPUCopyPass *copy = command != NULL
        ? SDL_BeginGPUCopyPass(command)
        : NULL;
    if (copy == NULL) {
        btbh_gpu_texture_error(report, SDL_GetError());
        if (command != NULL) {
            (void) SDL_CancelGPUCommandBuffer(command);
        }
        SDL_ReleaseGPUTransferBuffer(window->device->gpu, transfer);
        btbh_gpu_texture_destroy(resource);
        return NULL;
    }

    SDL_GPUTextureTransferInfo source = {0};
    source.transfer_buffer = transfer;
    source.pixels_per_row = resource->width;
    source.rows_per_layer = resource->height;
    SDL_GPUTextureRegion destination = {0};
    destination.texture = resource->texture;
    destination.w = resource->width;
    destination.h = resource->height;
    destination.d = 1u;
    SDL_UploadToGPUTexture(copy, &source, &destination, false);
    SDL_EndGPUCopyPass(copy);
    if (resource->mip_count > 1u) {
        SDL_GenerateMipmapsForGPUTexture(command, resource->texture);
    }
    const bool submitted = SDL_SubmitGPUCommandBuffer(command);
    SDL_ReleaseGPUTransferBuffer(window->device->gpu, transfer);
    if (!submitted) {
        btbh_gpu_texture_error(report, SDL_GetError());
        btbh_gpu_texture_destroy(resource);
        return NULL;
    }

    report->result = BTBH_GPU_OK;
    report->width = resource->width;
    report->height = resource->height;
    report->mip_count = resource->mip_count;
    return resource;
}

void btbh_gpu_texture_destroy(BTBHGPUTexture *texture) {
    if (texture == NULL) {
        return;
    }
    SDL_GPUDevice *device = NULL;
    if (
        texture->owner != NULL &&
        texture->owner->device != NULL &&
        !texture->owner->device->shutdown
    ) {
        device = texture->owner->device->gpu;
    }
    btbh_gpu_texture_release_handles(texture, device);
    btbh_gpu_texture_unlink(texture);
    free(texture);
}

void btbh_gpu_window_release_textures(BTBHGPUWindow *window) {
    if (window == NULL) {
        return;
    }
    SDL_GPUDevice *device =
        window->device != NULL && !window->device->shutdown
            ? window->device->gpu
            : NULL;
    BTBHGPUTexture *texture = window->texture_head;
    window->texture_head = NULL;
    while (texture != NULL) {
        BTBHGPUTexture *next = texture->next;
        btbh_gpu_texture_release_handles(texture, device);
        texture->owner = NULL;
        texture->previous = NULL;
        texture->next = NULL;
        texture = next;
    }
}
