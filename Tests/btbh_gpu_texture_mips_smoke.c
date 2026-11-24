/* Every mip level of a texture whose sides differ is written (since 0.14.1). SDL 3.4's Vulkan GenerateMipmaps leaves
 * the levels past the short side's last texel unwritten (zero-extent blits); btbh_gpu_texture.c blits them again.
 *
 * The checks: one-colour textures of 2048x1024 (the case v2 met: its 1x1 level), 64x256, 256x16 and a 2-layer
 * 128x32 array (each layer its own colour) are made with the full chain; every level from the one where a side runs
 * out to the last is read back, and every texel there must be the colour (a mip of one colour is that colour).
 * A 256x256 texture's last levels are read the same way (square ones SDL fills by itself).
 *
 * It needs a GPU (a claimed window); with no device it is skipped (77). */
#undef NDEBUG

#include "btbh_gpu/btbh_gpu.h"
#include "btbh_gpu_internal.h"

#include <SDL3/SDL_hints.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SMOKE_SKIP 77

#if defined(__APPLE__)
#define SMOKE_BACKEND BTBH_GPU_BACKEND_METAL
#else
#define SMOKE_BACKEND BTBH_GPU_BACKEND_VULKAN
#endif

static int failures;

static BTBHGPUWindow *open_window(BTBHGPUDevice *device) {
    const BTBHGPUWindowDescriptor descriptor = {
        .title = "BTBH texture mips smoke",
        .width = 64,
        .height = 64,
        .hidden = 1,
    };
    BTBHGPUWindowReport report;
    memset(&report, 0, sizeof(report));
    BTBHGPUWindow *window = btbh_gpu_window_create(&descriptor, &report);
    assert(window != NULL);
    assert(btbh_gpu_window_claim(device, window, &report) == BTBH_GPU_OK);
    return window;
}

static uint32_t level_extent(uint32_t size, uint32_t level) {
    const uint32_t extent = size >> level;
    return extent > 0u ? extent : 1u;
}

/* Reads level `level` of layer `layer` back and checks that every texel is `rgba`. */
static void check_level(
    BTBHGPUWindow *window,
    const BTBHGPUTexture *texture,
    uint32_t layer,
    uint32_t level,
    const uint8_t rgba[4],
    const char *what
) {
    SDL_GPUDevice *device = window->device->gpu;
    const uint32_t w = level_extent(texture->width, level);
    const uint32_t h = level_extent(texture->height, level);
    const uint32_t bytes = w * h * 4u;
    SDL_GPUTransferBufferCreateInfo info;
    SDL_zero(info);
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    info.size = bytes;
    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(device, &info);
    assert(transfer != NULL);
    SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(device);
    assert(command != NULL);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command);
    SDL_GPUTextureRegion source;
    SDL_zero(source);
    source.texture = texture->texture;
    source.mip_level = level;
    source.layer = layer;
    source.w = w;
    source.h = h;
    source.d = 1u;
    SDL_GPUTextureTransferInfo destination;
    SDL_zero(destination);
    destination.transfer_buffer = transfer;
    destination.pixels_per_row = w;
    destination.rows_per_layer = h;
    SDL_DownloadFromGPUTexture(copy, &source, &destination);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
    assert(fence != NULL);
    assert(SDL_WaitForGPUFences(device, true, &fence, 1u));
    SDL_ReleaseGPUFence(device, fence);
    const uint8_t *texels = (const uint8_t *)SDL_MapGPUTransferBuffer(device, transfer, false);
    assert(texels != NULL);
    uint32_t wrong = 0u;
    uint32_t first_wrong = 0u;
    for (uint32_t texel = 0u; texel < w * h; ++texel) {
        if (memcmp(texels + texel * 4u, rgba, 4u) != 0) {
            if (wrong == 0u) {
                first_wrong = texel;
            }
            wrong += 1u;
        }
    }
    if (wrong != 0u) {
        const uint8_t *t = texels + first_wrong * 4u;
        fprintf(stderr, "%s %ux%u layer %u level %u (%ux%u): %u of %u texels wrong, the first (%u,%u,%u,%u), expected "
            "(%u,%u,%u,%u)\n", what, texture->width, texture->height, layer, level, w, h, wrong, w * h, t[0], t[1],
            t[2], t[3], rgba[0], rgba[1], rgba[2], rgba[3]);
        failures += 1;
    }
    SDL_UnmapGPUTransferBuffer(device, transfer);
    SDL_ReleaseGPUTransferBuffer(device, transfer);
}

/* The levels to read: from the first where a side ran out (or the last two of a square texture) to the last. */
static uint32_t first_checked_level(uint32_t width, uint32_t height, uint32_t mip_count) {
    for (uint32_t level = 1u; level < mip_count; ++level) {
        if ((width >> level) == 0u || (height >> level) == 0u) {
            return level - 1u;
        }
    }
    return mip_count > 2u ? mip_count - 2u : 0u;
}

static void check_texture(BTBHGPUWindow *window, uint32_t width, uint32_t height, const uint8_t rgba[4]) {
    uint8_t *pixels = (uint8_t *)malloc((size_t)width * height * 4u);
    assert(pixels != NULL);
    for (size_t texel = 0u; texel < (size_t)width * height; ++texel) {
        memcpy(pixels + texel * 4u, rgba, 4u);
    }
    BTBHGPUTextureReport report;
    memset(&report, 0, sizeof(report));
    BTBHGPUTexture *texture = btbh_gpu_window_texture_create_rgba8_ex(window, pixels, width, height, 1u,
        BTBH_GPU_TEXTURE_FILTER_TRILINEAR, BTBH_GPU_TEXTURE_WRAP_CLAMP, BTBH_GPU_TEXTURE_COLOR_SPACE_LINEAR, &report);
    free(pixels);
    if (texture == NULL) {
        fprintf(stderr, "%ux%u: %s\n", width, height, report.message);
    }
    assert(texture != NULL);
    uint32_t longest = width > height ? width : height;
    uint32_t full = 1u;
    while (longest > 1u) {
        longest >>= 1u;
        full += 1u;
    }
    assert(texture->mip_count == full);
    for (uint32_t level = first_checked_level(width, height, texture->mip_count); level < texture->mip_count; ++level) {
        check_level(window, texture, 0u, level, rgba, "texture");
    }
    btbh_gpu_texture_destroy(texture);
}

static void check_array(BTBHGPUWindow *window) {
    const uint32_t width = 128u;
    const uint32_t height = 32u;
    static const uint8_t colours[2][4] = { { 30u, 200u, 90u, 255u }, { 250u, 10u, 140u, 128u } };
    uint8_t *layers[2];
    for (uint32_t layer = 0u; layer < 2u; ++layer) {
        layers[layer] = (uint8_t *)malloc((size_t)width * height * 4u);
        assert(layers[layer] != NULL);
        for (size_t texel = 0u; texel < (size_t)width * height; ++texel) {
            memcpy(layers[layer] + texel * 4u, colours[layer], 4u);
        }
    }
    BTBHGPUTextureReport report;
    memset(&report, 0, sizeof(report));
    BTBHGPUTexture *texture = btbh_gpu_window_texture_array_create_rgba8_ex(window, (const uint8_t *const *)layers,
        width, height, 2u, 1u, BTBH_GPU_TEXTURE_FILTER_TRILINEAR, BTBH_GPU_TEXTURE_WRAP_REPEAT,
        BTBH_GPU_TEXTURE_COLOR_SPACE_LINEAR, &report);
    free(layers[0]);
    free(layers[1]);
    if (texture == NULL) {
        fprintf(stderr, "array: %s\n", report.message);
    }
    assert(texture != NULL);
    assert(texture->mip_count == 8u);
    for (uint32_t layer = 0u; layer < 2u; ++layer) {
        for (uint32_t level = first_checked_level(width, height, texture->mip_count); level < texture->mip_count;
             ++level) {
            check_level(window, texture, layer, level, colours[layer], "array");
        }
    }
    btbh_gpu_texture_destroy(texture);
}

int main(void) {
    (void)SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    const BTBHGPUDeviceDescriptor device_descriptor = {
        .backend = SMOKE_BACKEND,
        .required_features = BTBH_GPU_REQUIRED_FEATURES,
        .debug_mode = 1,
    };
    BTBHGPUDeviceReport device_report;
    memset(&device_report, 0, sizeof(device_report));
    BTBHGPUDevice *device = btbh_gpu_device_create(&device_descriptor, &device_report);
    if (device == NULL) {
        fprintf(stderr, "skipped: no device (%s)\n", device_report.message);
        return SMOKE_SKIP;
    }
    BTBHGPUWindow *window = open_window(device);

    static const uint8_t orange[4] = { 200u, 120u, 40u, 255u };
    static const uint8_t blue[4] = { 20u, 60u, 230u, 200u };
    static const uint8_t grey[4] = { 128u, 128u, 128u, 64u };
    check_texture(window, 2048u, 1024u, orange);
    check_texture(window, 64u, 256u, blue);
    check_texture(window, 256u, 16u, grey);
    check_texture(window, 256u, 256u, orange);
    check_array(window);

    btbh_gpu_window_destroy(window);
    btbh_gpu_device_destroy(device);
    if (failures != 0) {
        fprintf(stderr, "texture mips smoke: %d levels wrong\n", failures);
        return 1;
    }
    printf("texture mips smoke: every level written\n");
    return 0;
}
