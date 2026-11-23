#include "btbh_gpu_internal.h"
#include "btbh_gpu_height_lift.h"

#include <limits.h>
#include <math.h>
#include <stdarg.h>
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

/* generate_mipmaps: 0 none, 1 the full chain, n > 1 at most n levels (for atlases whose cells
 * would bleed into each other further down). */
static uint32_t btbh_gpu_texture_levels(
    uint8_t generate_mipmaps,
    uint32_t full
) {
    if (generate_mipmaps == 0u) {
        return 1u;
    }
    if (generate_mipmaps == 1u || generate_mipmaps > full) {
        return full;
    }
    return generate_mipmaps;
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
    resource->color_target =
        (usage & SDL_GPU_TEXTUREUSAGE_COLOR_TARGET) != 0u;

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

static BTBHGPUTexture *btbh_gpu_window_texture_create_rgba8_format(
    BTBHGPUWindow *window,
    const uint8_t *pixels,
    uint32_t width,
    uint32_t height,
    uint8_t generate_mipmaps,
    BTBHGPUTextureFilter filter,
    BTBHGPUTextureWrap wrap,
    SDL_GPUTextureFormat format,
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
    /* 4 for the RGBA8 formats, 1 for a one-channel font atlas (R8). */
    const uint64_t byte_count =
        (uint64_t)width * (uint64_t)height *
        (uint64_t)SDL_GPUTextureFormatTexelBlockSize(format);
    if (byte_count == 0u) {
        btbh_gpu_texture_error(report, "Texture format has no texel size");
        return NULL;
    }
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
    resource->mip_count = btbh_gpu_texture_levels(
        generate_mipmaps,
        btbh_gpu_texture_mip_count(width, height)
    );
    resource->format = format;

    SDL_GPUTextureCreateInfo texture_info = {0};
    texture_info.type = SDL_GPU_TEXTURETYPE_2D;
    texture_info.format = format;
    texture_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    if (resource->mip_count > 1u) {
        texture_info.usage |= SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    }
    texture_info.width = width;
    texture_info.height = height;
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
    sampler_info.max_lod = (float)(resource->mip_count - 1u);
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
    report->width = width;
    report->height = height;
    report->mip_count = resource->mip_count;
    return resource;
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
    return btbh_gpu_window_texture_create_rgba8_format(
        window,
        pixels,
        width,
        height,
        0u,
        filter,
        wrap,
        SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        report
    );
}

BTBHGPUTexture *btbh_gpu_window_texture_create_r8(
    BTBHGPUWindow *window,
    const uint8_t *pixels,
    uint32_t width,
    uint32_t height,
    BTBHGPUTextureFilter filter,
    BTBHGPUTextureWrap wrap,
    BTBHGPUTextureReport *report
) {
    return btbh_gpu_window_texture_create_rgba8_format(
        window,
        pixels,
        width,
        height,
        0u,
        filter,
        wrap,
        SDL_GPU_TEXTUREFORMAT_R8_UNORM,
        report
    );
}

BTBHGPUTexture *btbh_gpu_window_texture_create_rgba8_ex(
    BTBHGPUWindow *window,
    const uint8_t *pixels,
    uint32_t width,
    uint32_t height,
    uint8_t generate_mipmaps,
    BTBHGPUTextureFilter filter,
    BTBHGPUTextureWrap wrap,
    BTBHGPUTextureColorSpace color_space,
    BTBHGPUTextureReport *report
) {
    return btbh_gpu_window_texture_create_rgba8_format(
        window,
        pixels,
        width,
        height,
        generate_mipmaps,
        filter,
        wrap,
        color_space == BTBH_GPU_TEXTURE_COLOR_SPACE_SRGB
            ? SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB
            : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        report
    );
}

BTBHGPUResult btbh_gpu_texture_update_rgba8(
    BTBHGPUTexture *texture,
    const uint8_t *pixels,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height
) {
    if (texture == NULL || texture->owner == NULL ||
        texture->owner->device == NULL || texture->owner->device->shutdown ||
        texture->format == SDL_GPU_TEXTUREFORMAT_R8_UNORM ||
        btbh_gpu_texture_is_array(texture) ||
        btbh_gpu_texture_is_heightfield(texture) ||
        pixels == NULL || width == 0u || height == 0u ||
        x > texture->width || width > texture->width - x ||
        y > texture->height || height > texture->height - y) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }

    SDL_GPUDevice *device = texture->owner->device->gpu;
    const uint32_t byte_count = width * height * 4u;
    SDL_GPUTransferBufferCreateInfo transfer_info = {0};
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size = byte_count;
    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(device, &transfer_info);
    if (transfer == NULL) {
        return BTBH_GPU_DEVICE_FAILURE;
    }
    void *mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
    if (mapped == NULL) {
        SDL_ReleaseGPUTransferBuffer(device, transfer);
        return BTBH_GPU_DEVICE_FAILURE;
    }
    memcpy(mapped, pixels, byte_count);
    SDL_UnmapGPUTransferBuffer(device, transfer);

    SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(device);
    if (command == NULL) {
        SDL_ReleaseGPUTransferBuffer(device, transfer);
        return BTBH_GPU_DEVICE_FAILURE;
    }
    SDL_GPUCopyPass *copy_pass = SDL_BeginGPUCopyPass(command);
    SDL_GPUTextureTransferInfo source = {0};
    source.transfer_buffer = transfer;
    source.pixels_per_row = width;
    source.rows_per_layer = height;
    SDL_GPUTextureRegion destination = {0};
    destination.texture = texture->texture;
    destination.x = x;
    destination.y = y;
    destination.w = width;
    destination.h = height;
    destination.d = 1;
    const bool full_overwrite =
        x == 0u && y == 0u && width == texture->width && height == texture->height;
    SDL_UploadToGPUTexture(copy_pass, &source, &destination, full_overwrite);
    SDL_EndGPUCopyPass(copy_pass);
    if (texture->mip_count > 1u) {
        SDL_GenerateMipmapsForGPUTexture(command, texture->texture);
    }
    const bool submitted = SDL_SubmitGPUCommandBuffer(command);
    SDL_ReleaseGPUTransferBuffer(device, transfer);
    return submitted ? BTBH_GPU_OK : BTBH_GPU_DEVICE_FAILURE;
}

BTBHGPUTexture *btbh_gpu_window_texture_create_rgba8_mipmapped(
    BTBHGPUWindow *window,
    const uint8_t *pixels,
    uint32_t width,
    uint32_t height,
    BTBHGPUTextureFilter filter,
    BTBHGPUTextureWrap wrap,
    BTBHGPUTextureReport *report
) {
    return btbh_gpu_window_texture_create_rgba8_format(
        window,
        pixels,
        width,
        height,
        1u,
        filter,
        wrap,
        SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        report
    );
}

bool btbh_gpu_texture_decode_memory_rgba8(
    const uint8_t *encoded,
    size_t encoded_size,
    uint8_t **pixels,
    uint32_t *width,
    uint32_t *height
) {
    if (pixels == NULL || width == NULL || height == NULL) {
        return false;
    }
    *pixels = NULL;
    *width = 0u;
    *height = 0u;
    if (
        encoded == NULL ||
        encoded_size == 0u ||
        encoded_size > (size_t)INT_MAX
    ) {
        return false;
    }
    int decoded_width = 0;
    int decoded_height = 0;
    int channels = 0;
    stbi_uc *decoded = stbi_load_from_memory(
        encoded,
        (int)encoded_size,
        &decoded_width,
        &decoded_height,
        &channels,
        4
    );
    if (
        decoded == NULL ||
        decoded_width <= 0 ||
        decoded_height <= 0
    ) {
        stbi_image_free(decoded);
        return false;
    }
    const uint64_t byte_count =
        (uint64_t)(uint32_t)decoded_width *
        (uint64_t)(uint32_t)decoded_height *
        UINT64_C(4);
    if (byte_count > SIZE_MAX) {
        stbi_image_free(decoded);
        return false;
    }
    uint8_t *copy = malloc((size_t)byte_count);
    if (copy == NULL) {
        stbi_image_free(decoded);
        return false;
    }
    (void)memcpy(copy, decoded, (size_t)byte_count);
    stbi_image_free(decoded);
    *pixels = copy;
    *width = (uint32_t)decoded_width;
    *height = (uint32_t)decoded_height;
    return true;
}

BTBHGPUTexture *btbh_gpu_window_texture_load_memory_format(
    BTBHGPUWindow *window,
    const uint8_t *encoded,
    size_t encoded_size,
    uint8_t generate_mipmaps,
    BTBHGPUTextureFilter filter,
    BTBHGPUTextureWrap wrap,
    SDL_GPUTextureFormat format,
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
    BTBHGPUTexture *texture = btbh_gpu_window_texture_create_rgba8_format(
        window,
        pixels,
        (uint32_t)width,
        (uint32_t)height,
        generate_mipmaps,
        filter,
        wrap,
        format,
        report
    );
    stbi_image_free(pixels);
    return texture;
}

BTBHGPUTexture *btbh_gpu_window_texture_load_memory(
    BTBHGPUWindow *window,
    const uint8_t *encoded,
    size_t encoded_size,
    BTBHGPUTextureFilter filter,
    BTBHGPUTextureWrap wrap,
    BTBHGPUTextureReport *report
) {
    return btbh_gpu_window_texture_load_memory_format(
        window,
        encoded,
        encoded_size,
        0u,
        filter,
        wrap,
        SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        report
    );
}

BTBHGPUTexture *btbh_gpu_window_texture_load_format(
    BTBHGPUWindow *window,
    const char *path,
    uint8_t generate_mipmaps,
    BTBHGPUTextureFilter filter,
    BTBHGPUTextureWrap wrap,
    SDL_GPUTextureFormat format,
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
    resource->mip_count = btbh_gpu_texture_levels(
        generate_mipmaps,
        btbh_gpu_texture_mip_count(resource->width, resource->height)
    );
    resource->format = format;

    SDL_GPUTextureCreateInfo texture_info = {0};
    texture_info.type = SDL_GPU_TEXTURETYPE_2D;
    texture_info.format = format;
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

BTBHGPUTexture *btbh_gpu_window_texture_load(
    BTBHGPUWindow *window,
    const char *path,
    uint8_t generate_mipmaps,
    BTBHGPUTextureFilter filter,
    BTBHGPUTextureWrap wrap,
    BTBHGPUTextureReport *report
) {
    return btbh_gpu_window_texture_load_ex(
        window,
        path,
        generate_mipmaps,
        filter,
        wrap,
        BTBH_GPU_TEXTURE_COLOR_SPACE_LINEAR,
        report
    );
}

BTBHGPUTexture *btbh_gpu_window_texture_load_ex(
    BTBHGPUWindow *window,
    const char *path,
    uint8_t generate_mipmaps,
    BTBHGPUTextureFilter filter,
    BTBHGPUTextureWrap wrap,
    BTBHGPUTextureColorSpace color_space,
    BTBHGPUTextureReport *report
) {
    return btbh_gpu_window_texture_load_format(
        window,
        path,
        generate_mipmaps,
        filter,
        wrap,
        color_space == BTBH_GPU_TEXTURE_COLOR_SPACE_SRGB
            ? SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB
            : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        report
    );
}

/* ------------------------------------------------------------------- 2D array textures (RFC 0001, RFC 0009) */

static void btbh_gpu_texture_refuse(
    BTBHGPUTextureReport *report,
    BTBHGPUResult result,
    const char *format,
    ...
) {
    report->result = result;
    va_list arguments;
    va_start(arguments, format);
    (void)vsnprintf(report->message, sizeof(report->message), format, arguments);
    va_end(arguments);
}

BTBHGPUTexture *btbh_gpu_window_texture_array_create_rgba8_ex(
    BTBHGPUWindow *window,
    const uint8_t *const *layer_pixels,
    uint32_t width,
    uint32_t height,
    uint32_t layer_count,
    uint8_t generate_mipmaps,
    BTBHGPUTextureFilter filter,
    BTBHGPUTextureWrap wrap,
    BTBHGPUTextureColorSpace color_space,
    BTBHGPUTextureReport *report
) {
    if (report == NULL) {
        return NULL;
    }
    (void)memset(report, 0, sizeof(*report));
    if (window == NULL || window->device == NULL || window->device->gpu == NULL || window->device->shutdown ||
        !window->claimed) {
        btbh_gpu_texture_refuse(report, BTBH_GPU_INVALID_ARGUMENT,
            "Texture array: the window has no device (not claimed, or shut down)");
        return NULL;
    }
    if (layer_pixels == NULL) {
        btbh_gpu_texture_refuse(report, BTBH_GPU_INVALID_ARGUMENT, "Texture array: layer_pixels is NULL");
        return NULL;
    }
    if (width == 0u || height == 0u) {
        btbh_gpu_texture_refuse(report, BTBH_GPU_INVALID_ARGUMENT,
            "Texture array: width and height must be above 0 (%ux%u)", width, height);
        return NULL;
    }
    if (layer_count == 0u || layer_count > BTBH_GPU_TEXTURE_ARRAY_MAX_LAYERS) {
        btbh_gpu_texture_refuse(report, BTBH_GPU_INVALID_ARGUMENT,
            "Texture array: layer_count must be 1..%u (%u)", BTBH_GPU_TEXTURE_ARRAY_MAX_LAYERS, layer_count);
        return NULL;
    }
    if ((uint32_t)filter > (uint32_t)BTBH_GPU_TEXTURE_FILTER_TRILINEAR ||
        (uint32_t)wrap > (uint32_t)BTBH_GPU_TEXTURE_WRAP_MIRROR ||
        (uint32_t)color_space > (uint32_t)BTBH_GPU_TEXTURE_COLOR_SPACE_SRGB) {
        btbh_gpu_texture_refuse(report, BTBH_GPU_INVALID_ARGUMENT,
            "Texture array: unknown filter, wrap or colour space (%u, %u, %u)", (uint32_t)filter, (uint32_t)wrap,
            (uint32_t)color_space);
        return NULL;
    }
    for (uint32_t layer = 0u; layer < layer_count; ++layer) {
        if (layer_pixels[layer] == NULL) {
            btbh_gpu_texture_refuse(report, BTBH_GPU_INVALID_ARGUMENT, "Texture array: layer %u's pixels are NULL",
                layer);
            return NULL;
        }
    }
    const uint64_t layer_bytes = (uint64_t)width * (uint64_t)height * 4u;
    const uint64_t byte_count = layer_bytes * (uint64_t)layer_count;
    if (byte_count > UINT32_MAX) {
        btbh_gpu_texture_refuse(report, BTBH_GPU_INVALID_ARGUMENT,
            "Texture array: %ux%u x %u layers is more than SDL_GPU uploads at once (4 GiB)", width, height,
            layer_count);
        return NULL;
    }
    SDL_GPUDevice *device = window->device->gpu;
    const SDL_GPUTextureFormat format = color_space == BTBH_GPU_TEXTURE_COLOR_SPACE_SRGB
        ? SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB
        : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    const uint32_t mip_count = btbh_gpu_texture_levels(generate_mipmaps, btbh_gpu_texture_mip_count(width, height));
    /* COLOR_TARGET only for the mips' blits, as for the 2D textures: an array is never a render target. */
    SDL_GPUTextureUsageFlags usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    if (mip_count > 1u) {
        usage |= SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    }
    if (!SDL_GPUTextureSupportsFormat(device, format, SDL_GPU_TEXTURETYPE_2D_ARRAY, usage)) {
        btbh_gpu_texture_refuse(report, BTBH_GPU_MISSING_CAPABILITY,
            "Texture array: this device can't sample a 2D array of RGBA8 (%s)%s",
            color_space == BTBH_GPU_TEXTURE_COLOR_SPACE_SRGB ? "sRGB" : "linear",
            mip_count > 1u ? " with mips" : "");
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
    resource->mip_count = mip_count;
    resource->layer_count = layer_count;
    resource->format = format;

    SDL_GPUTextureCreateInfo texture_info = {0};
    texture_info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
    texture_info.format = format;
    texture_info.usage = usage;
    texture_info.width = width;
    texture_info.height = height;
    texture_info.layer_count_or_depth = layer_count;
    texture_info.num_levels = mip_count;
    texture_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    resource->texture = SDL_CreateGPUTexture(device, &texture_info);

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
    sampler_info.max_lod = (float)(mip_count - 1u);
    resource->sampler = SDL_CreateGPUSampler(device, &sampler_info);

    SDL_GPUTransferBufferCreateInfo transfer_info = {0};
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size = (uint32_t)byte_count;
    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(device, &transfer_info);
    if (resource->texture == NULL || resource->sampler == NULL || transfer == NULL) {
        btbh_gpu_texture_error(report, SDL_GetError());
        if (transfer != NULL) {
            SDL_ReleaseGPUTransferBuffer(device, transfer);
        }
        btbh_gpu_texture_destroy(resource);
        return NULL;
    }
    uint8_t *mapped = (uint8_t *)SDL_MapGPUTransferBuffer(device, transfer, false);
    if (mapped == NULL) {
        btbh_gpu_texture_error(report, SDL_GetError());
        SDL_ReleaseGPUTransferBuffer(device, transfer);
        btbh_gpu_texture_destroy(resource);
        return NULL;
    }
    for (uint32_t layer = 0u; layer < layer_count; ++layer) {
        (void)memcpy(mapped + (size_t)layer_bytes * layer, layer_pixels[layer], (size_t)layer_bytes);
    }
    SDL_UnmapGPUTransferBuffer(device, transfer);

    SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass *copy = command != NULL ? SDL_BeginGPUCopyPass(command) : NULL;
    if (copy == NULL) {
        btbh_gpu_texture_error(report, SDL_GetError());
        if (command != NULL) {
            (void)SDL_CancelGPUCommandBuffer(command);
        }
        SDL_ReleaseGPUTransferBuffer(device, transfer);
        btbh_gpu_texture_destroy(resource);
        return NULL;
    }
    /* One upload per layer, in one copy pass; then every layer's mips in one go (SDL blits each layer's chain). */
    for (uint32_t layer = 0u; layer < layer_count; ++layer) {
        SDL_GPUTextureTransferInfo source = {0};
        source.transfer_buffer = transfer;
        source.offset = (uint32_t)(layer_bytes * layer);
        source.pixels_per_row = width;
        source.rows_per_layer = height;
        SDL_GPUTextureRegion destination = {0};
        destination.texture = resource->texture;
        destination.layer = layer;
        destination.w = width;
        destination.h = height;
        destination.d = 1u;
        SDL_UploadToGPUTexture(copy, &source, &destination, false);
    }
    SDL_EndGPUCopyPass(copy);
    if (mip_count > 1u) {
        SDL_GenerateMipmapsForGPUTexture(command, resource->texture);
    }
    const bool submitted = SDL_SubmitGPUCommandBuffer(command);
    SDL_ReleaseGPUTransferBuffer(device, transfer);
    if (!submitted) {
        btbh_gpu_texture_error(report, SDL_GetError());
        btbh_gpu_texture_destroy(resource);
        return NULL;
    }
    report->result = BTBH_GPU_OK;
    report->width = width;
    report->height = height;
    report->mip_count = mip_count;
    return resource;
}

BTBHGPUTexture *btbh_gpu_window_texture_array_load_ex(
    BTBHGPUWindow *window,
    const char *const *layer_paths,
    uint32_t layer_count,
    uint8_t generate_mipmaps,
    BTBHGPUTextureFilter filter,
    BTBHGPUTextureWrap wrap,
    BTBHGPUTextureColorSpace color_space,
    BTBHGPUTextureReport *report
) {
    if (report == NULL) {
        return NULL;
    }
    (void)memset(report, 0, sizeof(*report));
    if (window == NULL || window->device == NULL || window->device->gpu == NULL || window->device->shutdown ||
        !window->claimed) {
        btbh_gpu_texture_refuse(report, BTBH_GPU_INVALID_ARGUMENT,
            "Texture array: the window has no device (not claimed, or shut down)");
        return NULL;
    }
    if (layer_paths == NULL) {
        btbh_gpu_texture_refuse(report, BTBH_GPU_INVALID_ARGUMENT, "Texture array: layer_paths is NULL");
        return NULL;
    }
    if (layer_count == 0u || layer_count > BTBH_GPU_TEXTURE_ARRAY_MAX_LAYERS) {
        btbh_gpu_texture_refuse(report, BTBH_GPU_INVALID_ARGUMENT,
            "Texture array: layer_count must be 1..%u (%u)", BTBH_GPU_TEXTURE_ARRAY_MAX_LAYERS, layer_count);
        return NULL;
    }
    for (uint32_t layer = 0u; layer < layer_count; ++layer) {
        if (layer_paths[layer] == NULL) {
            btbh_gpu_texture_refuse(report, BTBH_GPU_INVALID_ARGUMENT, "Texture array: layer %u's path is NULL",
                layer);
            return NULL;
        }
    }
    uint8_t **pixels = calloc(layer_count, sizeof(*pixels));
    if (pixels == NULL) {
        btbh_gpu_texture_error(report, "Could not allocate texture resource");
        return NULL;
    }
    uint32_t width = 0u;
    uint32_t height = 0u;
    bool decoded = true;
    for (uint32_t layer = 0u; layer < layer_count && decoded; ++layer) {
        const char *path = layer_paths[layer];
        uint8_t *encoded = NULL;
        size_t encoded_size = 0u;
        if (!btbh_gpu_read_file(path, &encoded, &encoded_size)) {
            btbh_gpu_texture_refuse(report, BTBH_GPU_DEVICE_FAILURE, "Texture array: layer %u, %s: %s", layer, path,
                SDL_GetError());
            decoded = false;
            break;
        }
        uint32_t layer_width = 0u;
        uint32_t layer_height = 0u;
        if (!btbh_gpu_texture_decode_memory_rgba8(encoded, encoded_size, &pixels[layer], &layer_width,
                &layer_height)) {
            btbh_gpu_texture_refuse(report, BTBH_GPU_DEVICE_FAILURE, "Texture array: layer %u, %s: %s", layer, path,
                stbi_failure_reason() != NULL ? stbi_failure_reason() : "can't be decoded");
            free(encoded);
            decoded = false;
            break;
        }
        free(encoded);
        if (layer == 0u) {
            width = layer_width;
            height = layer_height;
        } else if (layer_width != width || layer_height != height) {
            btbh_gpu_texture_refuse(report, BTBH_GPU_INVALID_ARGUMENT,
                "Texture array: layer %u, %s, is %ux%u; the first layer (%s) is %ux%u", layer, path, layer_width,
                layer_height, layer_paths[0], width, height);
            decoded = false;
        }
    }
    BTBHGPUTexture *texture = NULL;
    if (decoded) {
        texture = btbh_gpu_window_texture_array_create_rgba8_ex(window, (const uint8_t *const *)pixels, width,
            height, layer_count, generate_mipmaps, filter, wrap, color_space, report);
    }
    for (uint32_t layer = 0u; layer < layer_count; ++layer) {
        free(pixels[layer]);
    }
    free(pixels);
    return texture;
}

/* ------------------------------------------------------------------------------- heightfields (RFC 0012) */

BTBHGPUTexture *btbh_gpu_window_heightfield_create(
    BTBHGPUWindow *window,
    const float *heights,
    uint32_t width,
    uint32_t height,
    BTBHGPUTextureReport *report
) {
    if (report == NULL) {
        return NULL;
    }
    (void)memset(report, 0, sizeof(*report));
    if (window == NULL || window->device == NULL || window->device->gpu == NULL || window->device->shutdown ||
        !window->claimed) {
        btbh_gpu_texture_refuse(report, BTBH_GPU_INVALID_ARGUMENT,
            "Heightfield: the window has no device (not claimed, or shut down)");
        return NULL;
    }
    if (heights == NULL) {
        btbh_gpu_texture_refuse(report, BTBH_GPU_INVALID_ARGUMENT, "Heightfield: heights is NULL");
        return NULL;
    }
    if (width < 2u || height < 2u || width > BTBH_GPU_HEIGHTFIELD_MAX_SIZE || height > BTBH_GPU_HEIGHTFIELD_MAX_SIZE) {
        btbh_gpu_texture_refuse(report, BTBH_GPU_INVALID_ARGUMENT,
            "Heightfield: width and height must be 2..%u (%ux%u)", BTBH_GPU_HEIGHTFIELD_MAX_SIZE, width, height);
        return NULL;
    }
    const size_t sample_count = (size_t)width * (size_t)height;
    for (size_t index = 0u; index < sample_count; ++index) {
        if (!isfinite(heights[index]) || fabsf(heights[index]) > BTBH_GPU_HEIGHTFIELD_MAX_HEIGHT) {
            btbh_gpu_texture_refuse(report, BTBH_GPU_INVALID_ARGUMENT,
                "Heightfield: sample (%u, %u) is %g; heights must be finite and within +-%g (half floats)",
                (uint32_t)(index % width), (uint32_t)(index / width), (double)heights[index],
                (double)BTBH_GPU_HEIGHTFIELD_MAX_HEIGHT);
            return NULL;
        }
    }
    SDL_GPUDevice *device = window->device->gpu;
    const SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    if (!SDL_GPUTextureSupportsFormat(device, format, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_SAMPLER)) {
        btbh_gpu_texture_refuse(report, BTBH_GPU_MISSING_CAPABILITY,
            "Heightfield: this device can't sample RGBA16F textures");
        return NULL;
    }
    const size_t byte_count = sample_count * 4u * sizeof(uint16_t);
    uint16_t *texels = (uint16_t *)malloc(byte_count);
    float *copy = (float *)malloc(sample_count * sizeof(float));
    BTBHGPUTexture *resource = calloc(1, sizeof(*resource));
    if (texels == NULL || copy == NULL || resource == NULL) {
        free(texels);
        free(copy);
        free(resource);
        btbh_gpu_texture_error(report, "Could not allocate the heightfield");
        return NULL;
    }
    (void)memcpy(copy, heights, sample_count * sizeof(float));
    btbh_gpu_height_lift_texels(heights, width, height, texels);
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
    resource->heights = copy;

    SDL_GPUTextureCreateInfo texture_info = {0};
    texture_info.type = SDL_GPU_TEXTURETYPE_2D;
    texture_info.format = format;
    texture_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    texture_info.width = width;
    texture_info.height = height;
    texture_info.layer_count_or_depth = 1u;
    texture_info.num_levels = 1u;
    texture_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    resource->texture = SDL_CreateGPUTexture(device, &texture_info);

    /* Bilinear, clamped at the edges, one level: the shader reads it with SampleLevel(..., 0). */
    SDL_GPUSamplerCreateInfo sampler_info = {0};
    sampler_info.min_filter = SDL_GPU_FILTER_LINEAR;
    sampler_info.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampler_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.max_lod = 0.0f;
    resource->sampler = SDL_CreateGPUSampler(device, &sampler_info);

    SDL_GPUTransferBufferCreateInfo transfer_info = {0};
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size = (uint32_t)byte_count;
    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(device, &transfer_info);
    void *mapped = resource->texture != NULL && resource->sampler != NULL && transfer != NULL
        ? SDL_MapGPUTransferBuffer(device, transfer, false)
        : NULL;
    if (mapped == NULL) {
        btbh_gpu_texture_error(report, SDL_GetError());
        free(texels);
        if (transfer != NULL) {
            SDL_ReleaseGPUTransferBuffer(device, transfer);
        }
        btbh_gpu_texture_destroy(resource);
        return NULL;
    }
    (void)memcpy(mapped, texels, byte_count);
    free(texels);
    SDL_UnmapGPUTransferBuffer(device, transfer);
    SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass *copy_pass = command != NULL ? SDL_BeginGPUCopyPass(command) : NULL;
    if (copy_pass == NULL) {
        btbh_gpu_texture_error(report, SDL_GetError());
        if (command != NULL) {
            (void)SDL_CancelGPUCommandBuffer(command);
        }
        SDL_ReleaseGPUTransferBuffer(device, transfer);
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
    SDL_UploadToGPUTexture(copy_pass, &source, &destination, false);
    SDL_EndGPUCopyPass(copy_pass);
    const bool submitted = SDL_SubmitGPUCommandBuffer(command);
    SDL_ReleaseGPUTransferBuffer(device, transfer);
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

void btbh_gpu_texture_destroy(BTBHGPUTexture *texture) {
    /* A texture models share belongs to the share (btbh_gpu_model.c), which frees it when the last
       model lets go: a host's destroy of it, from BTBHGPUModelMaterialInfo, must not free it twice. */
    if (texture == NULL || texture->share != NULL) {
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
    free(texture->heights);
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
