/* RFC 0004 option A (readable small text): btbh_gpu_window_font_load_ex.
 *
 * Checks the descriptor's validation; that oversampled glyphs keep the plain loader's metrics within
 * a pixel (advances, glyph centres, ascent and descent) and that their UVs undo the oversampling;
 * snap_advances, kerning, a codepoint the font lacks, and the coverage curve (read back from the
 * atlases). It prints how long each bake takes.
 *
 * Then it draws one line of HUD text at 10, 12 and 16 px, light on dark, into a render target with
 * the canvas, reads it back and writes PNGs for comparison (into BTBH_GPU_FONT_SMOKE_OUTPUT_DIR, or
 * BTBH_SMOKE_DUMP when set):
 *   text-<size>px-plain.png     btbh_gpu_window_font_load, its whole-pixel advances
 *   text-<size>px-snapped.png   the plain loader laid out as CHIMERA does (0.3.0): fractional advances
 *                               scaled from a 60 px face, each glyph snapped to the nearest pixel
 *   text-<size>px-ex.png        _ex with oversample_x 3, fractional advances, kerning, glyphs on 1/3 px
 *   text-<size>px-ex-gamma.png  the same with coverage_gamma 1.3
 *   text-all.png, text-all-x4.png  every row in that order for each size, and the same 4x (nearest)
 *
 * After v0.4.0 (BTBHGPUFontDesc.flags, btbh_gpu_font_kerning_pairs): the pairs against
 * btbh_gpu_font_kerning; the R8 atlas against the RGBA one (a quarter of the bytes, the same coverage,
 * drawn within a level), the canvas refusing an R8 batch without its coverage shader; the snapped
 * x-height's metrics (whole pixels, the vertical ones scaled to match, nothing across changed) and the
 * ink at the top of flat lowercase without and with it. More PNGs, in the same directory:
 *   text-<size>px-ex-r8.png     the ex row from the R8 atlas
 *   text-<size>px-ex-snap.png   the ex row with BTBH_GPU_FONT_FLAG_SNAP_X_HEIGHT (compare -ex.png)
 *   probe-<size>px-ex-vs-snap.png  "xxx zzz uuu vvv nnn ooo eee" without (left) and with (right) it
 *   text-extras.png, text-extras-x4.png  ex, ex-r8, ex-snap and the probe for each size
 *
 * The face is Titillium Web SemiBold (SIL OFL 1.1, Tests/Fixtures/OFL-TitilliumWeb.txt), CHIMERA's UI
 * face. It needs a GPU and the canvas shaders; with neither it is skipped (77). */
#undef NDEBUG

#include "btbh_gpu/btbh_gpu.h"
#include "btbh_gpu_internal.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SMOKE_SKIP 77
#define CANVAS_WIDTH 640u
#define CANVAS_HEIGHT 288u
#define MAX_VERTICES 16384u
#define MAX_BATCHES 16u
#define SIZE_COUNT 3u
#define VARIANT_COUNT 4u

#if defined(__APPLE__)
#define SMOKE_SHADER_EXTENSION ".metallib"
#define SMOKE_SHADER_FORMAT BTBH_GPU_SHADER_FORMAT_METALLIB
#define SMOKE_BACKEND BTBH_GPU_BACKEND_METAL
#else
#define SMOKE_SHADER_EXTENSION ".spv"
#define SMOKE_SHADER_FORMAT BTBH_GPU_SHADER_FORMAT_SPIRV
#define SMOKE_BACKEND BTBH_GPU_BACKEND_VULKAN
#endif

static const char text_line[] =
    "Power Plant: 100 Void Matter, 10 s. AV Ta We Yo - right-click again: cancel";
static const uint32_t sizes[SIZE_COUNT] = {10u, 12u, 16u};
static const char *const variant_names[VARIANT_COUNT] = {"plain", "snapped", "ex", "ex-gamma"};
static const uint8_t background[3] = {22u, 24u, 30u};
static const float ink[4] = {236.0f / 255.0f, 232.0f / 255.0f, 220.0f / 255.0f, 1.0f};

static char shader_paths[4][1024]; /* the canvas shaders; [3] is the coverage one, beside [2] */
static uint32_t codepoints[128];
static uint32_t codepoint_count;

static BTBHGPUCanvasVertex vertices[MAX_VERTICES];
static uint32_t vertex_count;

/* CHIMERA's glyph set: printable ASCII, the Croatian letters and its punctuation. */
static void fill_codepoints(void) {
    static const uint32_t extra[] = {
        0x10Cu, 0x10Du, 0x106u, 0x107u, 0x110u, 0x111u, 0x160u, 0x161u, 0x17Du, 0x17Eu,
        0x2013u, 0x2014u, 0x2018u, 0x2019u, 0x201Cu, 0x201Du, 0x2026u, 0x2022u, 0xB7u, 0xD7u,
        0xB0u, 0x2713u,
    };
    codepoint_count = 0u;
    for (uint32_t c = 32u; c < 127u; c += 1u) {
        codepoints[codepoint_count++] = c;
    }
    for (size_t i = 0u; i < sizeof(extra) / sizeof(extra[0]); i += 1u) {
        codepoints[codepoint_count++] = extra[i];
    }
}

/* The texture's texels, tightly packed: 4 bytes each (RGBA order), or 1 for an R8 atlas. */
static uint8_t *read_texture(BTBHGPUWindow *window, BTBHGPUTexture *texture) {
    SDL_GPUDevice *device = window->device->gpu;
    const uint32_t texel = SDL_GPUTextureFormatTexelBlockSize(texture->format);
    assert(texel == 4u || (texel == 1u && texture->format == SDL_GPU_TEXTUREFORMAT_R8_UNORM));
    const uint32_t size = texture->width * texture->height * texel;
    SDL_GPUTransferBufferCreateInfo info;
    SDL_zero(info);
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    info.size = size;
    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(device, &info);
    assert(transfer != NULL);
    SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(device);
    assert(command != NULL);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command);
    SDL_GPUTextureRegion region;
    SDL_zero(region);
    region.texture = texture->texture;
    region.w = texture->width;
    region.h = texture->height;
    region.d = 1u;
    SDL_GPUTextureTransferInfo destination;
    SDL_zero(destination);
    destination.transfer_buffer = transfer;
    SDL_DownloadFromGPUTexture(copy, &region, &destination);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
    assert(fence != NULL);
    assert(SDL_WaitForGPUFences(device, true, &fence, 1u));
    SDL_ReleaseGPUFence(device, fence);
    uint8_t *pixels = malloc(size);
    assert(pixels != NULL);
    const uint8_t *mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
    assert(mapped != NULL);
    memcpy(pixels, mapped, size);
    SDL_UnmapGPUTransferBuffer(device, transfer);
    SDL_ReleaseGPUTransferBuffer(device, transfer);
    if (
        texture->format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM ||
        texture->format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB
    ) {
        for (uint32_t i = 0u; i < texture->width * texture->height; i += 1u) {
            const uint8_t blue = pixels[i * 4u];
            pixels[i * 4u] = pixels[i * 4u + 2u];
            pixels[i * 4u + 2u] = blue;
        }
    }
    return pixels;
}

static BTBHGPUFont *load_plain(BTBHGPUWindow *window, uint32_t size, BTBHGPUFontReport *report) {
    memset(report, 0, sizeof(*report));
    BTBHGPUFont *font = btbh_gpu_window_font_load(
        window, BTBH_GPU_FONT_SMOKE_FONT, size, codepoints, codepoint_count, report);
    assert(font != NULL && report->result == BTBH_GPU_OK);
    return font;
}

static BTBHGPUFont *load_ex_flags(
    BTBHGPUWindow *window,
    float size,
    uint8_t oversample_x,
    uint8_t snap,
    float gamma,
    uint8_t flags,
    BTBHGPUFontReport *report
) {
    BTBHGPUFontDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.pixel_size = size;
    desc.oversample_x = oversample_x;
    desc.oversample_y = 1u;
    desc.snap_advances = snap;
    desc.coverage_gamma = gamma;
    desc.flags = flags;
    BTBHGPUFont *font = btbh_gpu_window_font_load_ex(
        window, BTBH_GPU_FONT_SMOKE_FONT, &desc, codepoints, codepoint_count, report);
    if (font == NULL) {
        fprintf(stderr, "_ex failed: %s\n", report->message);
    }
    assert(font != NULL && report->result == BTBH_GPU_OK);
    return font;
}

static BTBHGPUFont *load_ex(
    BTBHGPUWindow *window,
    float size,
    uint8_t oversample_x,
    uint8_t snap,
    float gamma,
    BTBHGPUFontReport *report
) {
    return load_ex_flags(window, size, oversample_x, snap, gamma, 0u, report);
}

/* _ex with this desc must fail as invalid, with a reason. */
static void expect_invalid(BTBHGPUWindow *window, const BTBHGPUFontDesc *desc, const char *what) {
    BTBHGPUFontReport report;
    memset(&report, 0xAB, sizeof(report));
    BTBHGPUFont *font = btbh_gpu_window_font_load_ex(
        window, BTBH_GPU_FONT_SMOKE_FONT, desc, codepoints, codepoint_count, &report);
    if (font != NULL || report.result != BTBH_GPU_INVALID_ARGUMENT || report.message[0] == '\0') {
        fprintf(stderr, "not refused: %s\n", what);
    }
    assert(font == NULL);
    assert(report.result == BTBH_GPU_INVALID_ARGUMENT);
    assert(report.message[0] != '\0');
    assert(report.atlas_width == 0u && report.glyph_count == 0u);
}

static void check_validation(BTBHGPUDevice *device, BTBHGPUWindow *window) {
    BTBHGPUFontDesc good;
    memset(&good, 0, sizeof(good));
    good.pixel_size = 12.0f;
    good.oversample_x = 3u;

    BTBHGPUFontDesc desc = good;
    expect_invalid(window, NULL, "NULL desc");
    BTBHGPUFontDesc zero;
    memset(&zero, 0, sizeof(zero));
    expect_invalid(window, &zero, "zeroed desc");
    const float bad_sizes[] = {-12.0f, 0.5f, 1025.0f, NAN, INFINITY};
    for (size_t i = 0u; i < sizeof(bad_sizes) / sizeof(bad_sizes[0]); i += 1u) {
        desc = good;
        desc.pixel_size = bad_sizes[i];
        expect_invalid(window, &desc, "pixel_size");
    }
    desc = good;
    desc.oversample_x = 9u;
    expect_invalid(window, &desc, "oversample_x 9");
    desc = good;
    desc.oversample_y = 9u;
    expect_invalid(window, &desc, "oversample_y 9");
    desc = good;
    desc.snap_advances = 2u;
    expect_invalid(window, &desc, "snap_advances 2");
    /* Bits other than the three flags stay invalid, and so do both snaps at once. */
    const uint8_t bad_flags[] = {
        1u << 3,
        1u << 7,
        0xFFu,
        BTBH_GPU_FONT_FLAG_SNAP_X_HEIGHT | BTBH_GPU_FONT_FLAG_SNAP_X_HEIGHT_DOWN,
    };
    for (size_t i = 0u; i < sizeof(bad_flags) / sizeof(bad_flags[0]); i += 1u) {
        desc = good;
        desc.flags = bad_flags[i];
        expect_invalid(window, &desc, "flags");
    }
    const float bad_gammas[] = {0.2f, 4.5f, -1.0f, NAN};
    for (size_t i = 0u; i < sizeof(bad_gammas) / sizeof(bad_gammas[0]); i += 1u) {
        desc = good;
        desc.coverage_gamma = bad_gammas[i];
        expect_invalid(window, &desc, "coverage_gamma");
    }

    BTBHGPUFontReport report;
    /* No report: NULL, nothing to say it in. */
    assert(btbh_gpu_window_font_load_ex(
        window, BTBH_GPU_FONT_SMOKE_FONT, &good, codepoints, codepoint_count, NULL) == NULL);
    assert(btbh_gpu_window_font_load_ex(
        NULL, BTBH_GPU_FONT_SMOKE_FONT, &good, codepoints, codepoint_count, &report) == NULL);
    assert(report.result == BTBH_GPU_INVALID_ARGUMENT);
    assert(btbh_gpu_window_font_load_ex(
        window, NULL, &good, codepoints, codepoint_count, &report) == NULL);
    assert(report.result == BTBH_GPU_INVALID_ARGUMENT);
    assert(btbh_gpu_window_font_load_ex(
        window, BTBH_GPU_FONT_SMOKE_FONT, &good, NULL, codepoint_count, &report) == NULL);
    assert(report.result == BTBH_GPU_INVALID_ARGUMENT);
    assert(btbh_gpu_window_font_load_ex(
        window, BTBH_GPU_FONT_SMOKE_FONT, &good, codepoints, 0u, &report) == NULL);
    assert(report.result == BTBH_GPU_INVALID_ARGUMENT);
    const uint32_t out_of_range[2] = {65u, 0x80000000u};
    assert(btbh_gpu_window_font_load_ex(
        window, BTBH_GPU_FONT_SMOKE_FONT, &good, out_of_range, 2u, &report) == NULL);
    assert(report.result == BTBH_GPU_INVALID_ARGUMENT);
    /* A file that isn't there fails as the plain loader does. */
    assert(btbh_gpu_window_font_load_ex(
        window, "no-such-font.ttf", &good, codepoints, codepoint_count, &report) == NULL);
    assert(report.result == BTBH_GPU_DEVICE_FAILURE);

    /* A window nobody claimed. */
    const BTBHGPUWindowDescriptor descriptor = {
        .title = "BTBH font smoke (unclaimed)", .width = 64, .height = 64, .hidden = 1,
    };
    BTBHGPUWindowReport window_report;
    memset(&window_report, 0, sizeof(window_report));
    BTBHGPUWindow *unclaimed = btbh_gpu_window_create(&descriptor, &window_report);
    assert(unclaimed != NULL);
    assert(btbh_gpu_window_font_load_ex(
        unclaimed, BTBH_GPU_FONT_SMOKE_FONT, &good, codepoints, codepoint_count, &report) == NULL);
    assert(report.result == BTBH_GPU_INVALID_ARGUMENT);
    btbh_gpu_window_destroy(unclaimed);
    (void)device;

    /* The edges of each range bake. */
    BTBHGPUFontDesc edge = good;
    edge.pixel_size = 1.0f;
    edge.oversample_x = 8u;
    edge.oversample_y = 8u;
    edge.coverage_gamma = 4.0f;
    BTBHGPUFont *font = btbh_gpu_window_font_load_ex(
        window, BTBH_GPU_FONT_SMOKE_FONT, &edge, codepoints, codepoint_count, &report);
    assert(font != NULL && report.result == BTBH_GPU_OK && report.base_size == 1u);
    btbh_gpu_font_destroy(font);
    edge.pixel_size = 9.5f;
    edge.oversample_x = 0u;
    edge.oversample_y = 0u;
    edge.coverage_gamma = 0.25f;
    font = btbh_gpu_window_font_load_ex(
        window, BTBH_GPU_FONT_SMOKE_FONT, &edge, codepoints, codepoint_count, &report);
    assert(font != NULL && report.result == BTBH_GPU_OK && report.base_size == 10u);
    BTBHGPUFontMetrics metrics;
    assert(btbh_gpu_font_metrics(font, &metrics) == BTBH_GPU_OK);
    assert(metrics.pixel_size == 9.5f);
    btbh_gpu_font_destroy(font);

    /* Both flags at the edges too: at 1 px the x-height snaps up to one pixel. */
    edge.pixel_size = 1.0f;
    edge.oversample_x = 8u;
    edge.oversample_y = 8u;
    edge.flags = BTBH_GPU_FONT_FLAG_R8_ATLAS | BTBH_GPU_FONT_FLAG_SNAP_X_HEIGHT;
    font = btbh_gpu_window_font_load_ex(
        window, BTBH_GPU_FONT_SMOKE_FONT, &edge, codepoints, codepoint_count, &report);
    assert(font != NULL && report.result == BTBH_GPU_OK);
    assert(btbh_gpu_font_metrics(font, &metrics) == BTBH_GPU_OK);
    assert(metrics.x_height == 1.0f && metrics.pixel_size == 1.0f);
    assert(btbh_gpu_font_atlas(font)->format == SDL_GPU_TEXTUREFORMAT_R8_UNORM);
    btbh_gpu_font_destroy(font);

    /* No flags: the atlas is RGBA, as in 0.4.0. */
    const BTBHGPUFontDesc plain_desc = {.pixel_size = 12.0f, .oversample_x = 3u, .flags = 0u};
    font = btbh_gpu_window_font_load_ex(
        window, BTBH_GPU_FONT_SMOKE_FONT, &plain_desc, codepoints, codepoint_count, &report);
    assert(font != NULL && report.result == BTBH_GPU_OK);
    assert(btbh_gpu_font_atlas(font)->format == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
    btbh_gpu_font_destroy(font);
    printf("validation: refusals and range edges as documented\n");
}

/* 0.4.0's layouts, which a host built against 0.4.0 still uses. */
_Static_assert(sizeof(BTBHGPUFontDesc) == 12u, "BTBHGPUFontDesc keeps its size");
_Static_assert(offsetof(BTBHGPUFontDesc, snap_advances) == 6u, "BTBHGPUFontDesc keeps its layout");
_Static_assert(offsetof(BTBHGPUFontDesc, flags) == 7u, "BTBHGPUFontDesc keeps its layout");
_Static_assert(offsetof(BTBHGPUFontDesc, coverage_gamma) == 8u, "BTBHGPUFontDesc keeps its layout");
_Static_assert(sizeof(BTBHGPUFontMetrics) == 24u, "BTBHGPUFontMetrics keeps its size");
_Static_assert(sizeof(BTBHGPUFontKerningPair) == 12u, "three 32-bit fields");

/* btbh_gpu_font_kerning_pairs: the pairs btbh_gpu_font_kerning answers, all of them, sorted, with
   the same advances. */
static void check_kerning_pairs(BTBHGPUWindow *window) {
    BTBHGPUFontReport report;
    BTBHGPUFont *ex = load_ex(window, 16.0f, 3u, 0u, 0.0f, &report);
    const uint32_t count = btbh_gpu_font_kerning_pairs(ex, NULL, 0u);
    assert(count > 0u);
    assert(btbh_gpu_font_kerning_pairs(ex, NULL, 100000u) == count);
    BTBHGPUFontKerningPair *pairs = malloc(((size_t)count + 1u) * sizeof(*pairs));
    assert(pairs != NULL);
    memset(pairs, 0xAB, ((size_t)count + 1u) * sizeof(*pairs));
    const uint64_t one_start = SDL_GetTicksNS();
    assert(btbh_gpu_font_kerning_pairs(ex, pairs, count + 1u) == count);
    const uint64_t one_time = SDL_GetTicksNS() - one_start;
    const uint8_t untouched[sizeof(*pairs)] = {
        0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB,
    };
    assert(memcmp(&pairs[count], untouched, sizeof(*pairs)) == 0);
    for (uint32_t i = 0u; i < count; i += 1u) {
        assert(pairs[i].advance != 0.0f);
        assert(pairs[i].advance == btbh_gpu_font_kerning(ex, pairs[i].left, pairs[i].right));
        if (i > 0u) {
            assert(
                pairs[i - 1u].left < pairs[i].left ||
                (pairs[i - 1u].left == pairs[i].left && pairs[i - 1u].right < pairs[i].right)
            );
        }
    }
    /* Every nonzero btbh_gpu_font_kerning between the baked codepoints is in the list. */
    uint32_t nonzero = 0u;
    const uint64_t each_start = SDL_GetTicksNS();
    for (uint32_t a = 0u; a < codepoint_count; a += 1u) {
        for (uint32_t b = 0u; b < codepoint_count; b += 1u) {
            nonzero += btbh_gpu_font_kerning(ex, codepoints[a], codepoints[b]) != 0.0f ? 1u : 0u;
        }
    }
    const uint64_t each_time = SDL_GetTicksNS() - each_start;
    assert(nonzero == count);
    /* A short array takes the first pairs and leaves the rest alone. */
    BTBHGPUFontKerningPair few[6];
    memset(few, 0xAB, sizeof(few));
    assert(btbh_gpu_font_kerning_pairs(ex, few, 5u) == count);
    assert(memcmp(few, pairs, 5u * sizeof(*pairs)) == 0);
    assert(memcmp(&few[5], untouched, sizeof(*pairs)) == 0);
    printf(
        "kerning pairs at 16 px: %u nonzero of %u x %u; one call %.3f ms, %u btbh_gpu_font_kerning "
        "calls %.3f ms\n",
        count,
        codepoint_count,
        codepoint_count,
        (double)one_time / 1e6,
        codepoint_count * codepoint_count,
        (double)each_time / 1e6
    );

    /* The flags change nothing across: the same pairs. */
    BTBHGPUFont *flagged = load_ex_flags(
        window, 16.0f, 3u, 0u, 0.0f,
        BTBH_GPU_FONT_FLAG_R8_ATLAS | BTBH_GPU_FONT_FLAG_SNAP_X_HEIGHT, &report);
    BTBHGPUFontKerningPair *same = malloc((size_t)count * sizeof(*same));
    assert(same != NULL);
    assert(btbh_gpu_font_kerning_pairs(flagged, same, count) == count);
    assert(memcmp(same, pairs, (size_t)count * sizeof(*pairs)) == 0);
    free(same);
    btbh_gpu_font_destroy(flagged);

    /* Whole pixels with snap_advances. */
    BTBHGPUFont *snapped = load_ex(window, 12.0f, 3u, 1u, 0.0f, &report);
    const uint32_t snapped_count = btbh_gpu_font_kerning_pairs(snapped, pairs, count);
    assert(snapped_count > 0u && snapped_count <= count);
    for (uint32_t i = 0u; i < snapped_count; i += 1u) {
        assert(pairs[i].advance == roundf(pairs[i].advance) && pairs[i].advance != 0.0f);
    }
    btbh_gpu_font_destroy(snapped);

    /* None for the plain loader, a NULL font, or a font baked without any kerned pair. */
    BTBHGPUFont *plain = load_plain(window, 16u, &report);
    assert(btbh_gpu_font_kerning_pairs(plain, NULL, 0u) == 0u);
    memset(few, 0xAB, sizeof(few));
    assert(btbh_gpu_font_kerning_pairs(plain, few, 6u) == 0u);
    assert(memcmp(&few[0], untouched, sizeof(*pairs)) == 0);
    btbh_gpu_font_destroy(plain);
    assert(btbh_gpu_font_kerning_pairs(NULL, few, 6u) == 0u);
    const uint32_t lonely[1] = {' '};
    BTBHGPUFontDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.pixel_size = 16.0f;
    BTBHGPUFont *single = btbh_gpu_window_font_load_ex(
        window, BTBH_GPU_FONT_SMOKE_FONT, &desc, lonely, 1u, &report);
    assert(single != NULL && btbh_gpu_font_kerning_pairs(single, NULL, 0u) == 0u);
    btbh_gpu_font_destroy(single);
    free(pairs);
    btbh_gpu_font_destroy(ex);
}

/* BTBH_GPU_FONT_FLAG_R8_ATLAS: the same atlas, one channel, a quarter of the bytes; the same glyphs
   and metrics. */
static void check_r8_atlas(BTBHGPUWindow *window) {
    for (uint32_t s = 0u; s < SIZE_COUNT; s += 1u) {
        for (uint32_t g = 0u; g < 2u; g += 1u) {
            const float gamma = g == 0u ? 0.0f : 1.3f;
            BTBHGPUFontReport rgba_report;
            BTBHGPUFontReport r8_report;
            BTBHGPUFont *rgba = load_ex(window, (float)sizes[s], 3u, 0u, gamma, &rgba_report);
            BTBHGPUFont *r8 = load_ex_flags(
                window, (float)sizes[s], 3u, 0u, gamma, BTBH_GPU_FONT_FLAG_R8_ATLAS, &r8_report);
            assert(r8_report.atlas_width == rgba_report.atlas_width);
            assert(r8_report.atlas_height == rgba_report.atlas_height);
            assert(r8_report.ascent == rgba_report.ascent && r8_report.descent == rgba_report.descent);
            BTBHGPUTexture *rgba_atlas = btbh_gpu_font_atlas(rgba);
            BTBHGPUTexture *r8_atlas = btbh_gpu_font_atlas(r8);
            assert(rgba_atlas->format == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
            assert(r8_atlas->format == SDL_GPU_TEXTUREFORMAT_R8_UNORM);
            const uint32_t width = r8_report.atlas_width;
            const uint32_t height = r8_report.atlas_height;
            const uint32_t rgba_bytes = width * height * SDL_GPUTextureFormatTexelBlockSize(rgba_atlas->format);
            const uint32_t r8_bytes = width * height * SDL_GPUTextureFormatTexelBlockSize(r8_atlas->format);
            assert(r8_bytes * 4u == rgba_bytes);
            uint8_t *a = read_texture(window, rgba_atlas);
            uint8_t *b = read_texture(window, r8_atlas);
            for (uint32_t i = 0u; i < width * height; i += 1u) {
                assert(a[i * 4u] == 255u && a[i * 4u + 1u] == 255u && a[i * 4u + 2u] == 255u);
                assert(b[i] == a[i * 4u + 3u]);
            }
            free(a);
            free(b);
            for (uint32_t c = 0u; c < codepoint_count; c += 1u) {
                BTBHGPUFontGlyph x;
                BTBHGPUFontGlyph y;
                assert(btbh_gpu_font_glyph(rgba, codepoints[c], &x) == BTBH_GPU_OK);
                assert(btbh_gpu_font_glyph(r8, codepoints[c], &y) == BTBH_GPU_OK);
                assert(memcmp(&x, &y, sizeof(x)) == 0);
            }
            BTBHGPUFontMetrics x_metrics;
            BTBHGPUFontMetrics y_metrics;
            assert(btbh_gpu_font_metrics(rgba, &x_metrics) == BTBH_GPU_OK);
            assert(btbh_gpu_font_metrics(r8, &y_metrics) == BTBH_GPU_OK);
            assert(memcmp(&x_metrics, &y_metrics, sizeof(x_metrics)) == 0);
            /* Not an RGBA texture: refused. */
            const uint8_t texel[4] = {255u, 255u, 255u, 255u};
            assert(btbh_gpu_texture_update_rgba8(r8_atlas, texel, 0u, 0u, 1u, 1u) == BTBH_GPU_INVALID_ARGUMENT);
            if (g == 0u) {
                printf(
                    "atlas at %u px (oversample 3): %u x %u, RGBA8 %u bytes, R8 %u bytes\n",
                    sizes[s], width, height, rgba_bytes, r8_bytes);
            }
            btbh_gpu_font_destroy(rgba);
            btbh_gpu_font_destroy(r8);
        }
    }
    printf("R8 atlas: the RGBA atlas's alpha, texel for texel; same glyphs and metrics\n");
}

/* BTBH_GPU_FONT_FLAG_SNAP_X_HEIGHT's metrics: a whole-pixel x-height, everything vertical scaled to
   match, nothing across changed. */
static void check_snapped_metrics(BTBHGPUWindow *window) {
    const float check_sizes[] = {9.0f, 10.0f, 11.0f, 12.0f, 13.5f, 16.0f, 24.0f};
    for (size_t s = 0u; s < sizeof(check_sizes) / sizeof(check_sizes[0]); s += 1u) {
        BTBHGPUFontReport report;
        BTBHGPUFontReport snapped_report;
        BTBHGPUFont *ex = load_ex(window, check_sizes[s], 3u, 0u, 0.0f, &report);
        BTBHGPUFont *snapped = load_ex_flags(
            window, check_sizes[s], 3u, 0u, 0.0f, BTBH_GPU_FONT_FLAG_SNAP_X_HEIGHT, &snapped_report);
        BTBHGPUFontMetrics m;
        BTBHGPUFontMetrics n;
        assert(btbh_gpu_font_metrics(ex, &m) == BTBH_GPU_OK);
        assert(btbh_gpu_font_metrics(snapped, &n) == BTBH_GPU_OK);
        assert(n.x_height == roundf(m.x_height) && n.x_height == floorf(n.x_height));
        const float ratio = n.x_height / m.x_height;
        assert(fabsf(n.ascent - m.ascent * ratio) < 1e-3f);
        assert(fabsf(n.descent - m.descent * ratio) < 1e-3f);
        assert(fabsf(n.line_gap - m.line_gap * ratio) < 1e-3f);
        assert(fabsf(n.cap_height - m.cap_height * ratio) < 1e-3f);
        assert(n.pixel_size == m.pixel_size && snapped_report.base_size == report.base_size);
        assert(snapped_report.ascent == n.ascent && snapped_report.descent == n.descent);
        assert(snapped_report.line_gap == n.line_gap);
        for (uint32_t c = 0u; c < codepoint_count; c += 1u) {
            BTBHGPUFontGlyph a;
            BTBHGPUFontGlyph b;
            assert(btbh_gpu_font_glyph(ex, codepoints[c], &a) == BTBH_GPU_OK);
            assert(btbh_gpu_font_glyph(snapped, codepoints[c], &b) == BTBH_GPU_OK);
            assert(a.advance == b.advance && a.offset_x == b.offset_x && a.width == b.width);
            /* The UVs still take one texel a pixel down. */
            if (b.height > 0.0f) {
                assert(fabsf((b.v1 - b.v0) * (float)snapped_report.atlas_height - b.height) < 0.01f);
            }
        }
        printf(
            "snapped x-height at %.1f px: %.3f -> %.0f (vertical x%.3f); ascent %.3f -> %.3f, "
            "descent %.3f -> %.3f, cap height %.3f -> %.3f\n",
            (double)check_sizes[s],
            (double)m.x_height,
            (double)n.x_height,
            (double)ratio,
            (double)m.ascent,
            (double)n.ascent,
            (double)m.descent,
            (double)n.descent,
            (double)m.cap_height,
            (double)n.cap_height
        );
        /* _DOWN: floor, so the face never grows (and nothing across changes either). */
        BTBHGPUFontReport down_report;
        BTBHGPUFont *down = load_ex_flags(
            window, check_sizes[s], 3u, 0u, 0.0f, BTBH_GPU_FONT_FLAG_SNAP_X_HEIGHT_DOWN, &down_report);
        BTBHGPUFontMetrics d;
        assert(btbh_gpu_font_metrics(down, &d) == BTBH_GPU_OK);
        assert(d.x_height == floorf(m.x_height) && d.x_height <= m.x_height);
        const float down_ratio = d.x_height / m.x_height;
        assert(fabsf(d.ascent - m.ascent * down_ratio) < 1e-3f);
        assert(fabsf(d.descent - m.descent * down_ratio) < 1e-3f);
        assert(d.ascent - d.descent <= m.ascent - m.descent + 1e-3f);
        assert(d.cap_height <= m.cap_height + 1e-3f);
        for (uint32_t c = 0u; c < codepoint_count; c += 1u) {
            BTBHGPUFontGlyph a;
            BTBHGPUFontGlyph b;
            assert(btbh_gpu_font_glyph(ex, codepoints[c], &a) == BTBH_GPU_OK);
            assert(btbh_gpu_font_glyph(down, codepoints[c], &b) == BTBH_GPU_OK);
            assert(a.advance == b.advance && a.offset_x == b.offset_x && a.width == b.width);
            assert(b.height <= a.height + 1.0f);
        }
        printf(
            "snapped down at %.1f px: x-height %.3f -> %.0f, ascent - descent %.3f -> %.3f\n",
            (double)check_sizes[s],
            (double)m.x_height,
            (double)d.x_height,
            (double)(m.ascent - m.descent),
            (double)(d.ascent - d.descent)
        );
        btbh_gpu_font_destroy(down);
        btbh_gpu_font_destroy(ex);
        btbh_gpu_font_destroy(snapped);
    }
}

static void check_metrics(BTBHGPUWindow *window) {
    const uint32_t check_sizes[] = {10u, 12u, 16u, 24u};
    const uint8_t oversamples[] = {1u, 2u, 3u};
    float worst_advance = 0.0f;
    float worst_centre_x = 0.0f;
    float worst_centre_y = 0.0f;
    for (size_t s = 0u; s < sizeof(check_sizes) / sizeof(check_sizes[0]); s += 1u) {
        BTBHGPUFontReport plain_report;
        BTBHGPUFont *plain = load_plain(window, check_sizes[s], &plain_report);
        BTBHGPUFontMetrics plain_metrics;
        assert(btbh_gpu_font_metrics(plain, &plain_metrics) == BTBH_GPU_OK);
        assert(plain_metrics.ascent == plain_report.ascent);
        assert(plain_metrics.descent == plain_report.descent);
        assert(plain_metrics.line_gap == plain_report.line_gap);
        assert(plain_metrics.pixel_size == (float)check_sizes[s]);
        assert(plain_metrics.x_height > 0.0f);
        assert(plain_metrics.x_height < plain_metrics.cap_height);
        assert(plain_metrics.cap_height <= plain_metrics.ascent + 1.0f);
        printf("metrics: %u px, x-height %.3f, cap height %.3f, ascent %.0f\n",
            check_sizes[s], plain_metrics.x_height, plain_metrics.cap_height, plain_metrics.ascent);
        assert(btbh_gpu_font_kerning(plain, 'A', 'V') == 0.0f);
        for (size_t o = 0u; o < sizeof(oversamples) / sizeof(oversamples[0]); o += 1u) {
            const uint8_t oversample = oversamples[o];
            BTBHGPUFontReport report;
            BTBHGPUFont *ex = load_ex(window, (float)check_sizes[s], oversample, 0u, 0.0f, &report);
            assert(report.base_size == check_sizes[s]);
            assert(report.glyph_count == codepoint_count);
            BTBHGPUFontMetrics metrics;
            assert(btbh_gpu_font_metrics(ex, &metrics) == BTBH_GPU_OK);
            assert(metrics.ascent == report.ascent && metrics.descent == report.descent);
            assert(metrics.line_gap == report.line_gap);
            assert(fabsf(metrics.ascent - plain_report.ascent) <= 1.0f);
            assert(fabsf(metrics.descent - plain_report.descent) < 1e-3f);
            assert(metrics.descent < 0.0f && metrics.line_gap >= 0.0f);
            assert(metrics.pixel_size == (float)check_sizes[s]);
            /* Same outlines, same scale: the plain loader only rounds its ascent. */
            assert(fabsf(metrics.x_height - plain_metrics.x_height) < 1e-3f);
            assert(fabsf(metrics.cap_height - plain_metrics.cap_height) < 1e-3f);
            assert(metrics.x_height > 0.0f && metrics.x_height < metrics.cap_height);
            assert(metrics.cap_height < metrics.ascent);
            bool fractional = false;
            for (uint32_t c = 32u; c < 127u; c += 1u) {
                BTBHGPUFontGlyph p;
                BTBHGPUFontGlyph e;
                assert(btbh_gpu_font_glyph(plain, c, &p) == BTBH_GPU_OK);
                assert(btbh_gpu_font_glyph(ex, c, &e) == BTBH_GPU_OK);
                assert(e.codepoint == c);
                const float advance = fabsf(e.advance - p.advance);
                worst_advance = fmaxf(worst_advance, advance);
                assert(advance <= 1.0f);
                fractional = fractional || e.advance != floorf(e.advance);
                if (c == ' ') {
                    continue;
                }
                /* Each glyph's quad is centred on its ink in both loaders. */
                const float centre_x = fabsf(
                    (e.offset_x + 0.5f * e.width) - (p.offset_x + 0.5f * p.width));
                const float centre_y = fabsf(
                    (e.offset_y + 0.5f * e.height) - (p.offset_y + 0.5f * p.height));
                worst_centre_x = fmaxf(worst_centre_x, centre_x);
                worst_centre_y = fmaxf(worst_centre_y, centre_y);
                assert(centre_x <= 1.0f);
                assert(centre_y <= 1.0f);
                /* The UVs take oversample texels for each output pixel across, one down. */
                const float texels_x = (e.u1 - e.u0) * (float)report.atlas_width;
                const float texels_y = (e.v1 - e.v0) * (float)report.atlas_height;
                assert(fabsf(texels_x - e.width * (float)oversample) < 0.01f);
                assert(fabsf(texels_y - e.height) < 0.01f);
                assert(e.u0 >= 0.0f && e.v0 >= 0.0f && e.u1 <= 1.0f && e.v1 <= 1.0f);
            }
            assert(fractional);
            btbh_gpu_font_destroy(ex);
        }
        btbh_gpu_font_destroy(plain);
    }
    printf(
        "metrics against the plain loader (10-24 px, oversample 1-3): advances within %.3f px, "
        "glyph centres within %.3f px across and %.3f px down\n",
        (double)worst_advance,
        (double)worst_centre_x,
        (double)worst_centre_y
    );

    /* Whole-pixel advances and kerning with snap_advances. */
    BTBHGPUFontReport report;
    BTBHGPUFont *snapped = load_ex(window, 12.0f, 3u, 1u, 0.0f, &report);
    for (uint32_t c = 32u; c < 127u; c += 1u) {
        BTBHGPUFontGlyph g;
        assert(btbh_gpu_font_glyph(snapped, c, &g) == BTBH_GPU_OK);
        assert(g.advance == roundf(g.advance));
        for (uint32_t d = 32u; d < 127u; d += 1u) {
            const float kern = btbh_gpu_font_kerning(snapped, c, d);
            assert(kern == roundf(kern));
        }
    }
    btbh_gpu_font_destroy(snapped);

    /* Kerning (Titillium Web has GPOS pairs), and none for codepoints it wasn't baked with. */
    BTBHGPUFont *ex = load_ex(window, 16.0f, 3u, 0u, 0.0f, &report);
    static const char *const pairs[] = {"AV", "AT", "To", "Ty", "Wa", "Yo", "LT", "Te", "P.", "VA"};
    uint32_t kerned = 0u;
    printf("kerning at 16 px:");
    for (size_t i = 0u; i < sizeof(pairs) / sizeof(pairs[0]); i += 1u) {
        const float kern = btbh_gpu_font_kerning(ex, (uint32_t)pairs[i][0], (uint32_t)pairs[i][1]);
        printf(" %s %.2f", pairs[i], (double)kern);
        kerned += kern < 0.0f ? 1u : 0u;
        assert(fabsf(kern) < 4.0f);
    }
    printf("\n");
    assert(kerned >= 3u);
    assert(btbh_gpu_font_kerning(ex, 'A', 0x4E00u) == 0.0f);
    assert(btbh_gpu_font_kerning(ex, 0x4E00u, 'A') == 0.0f);
    assert(btbh_gpu_font_kerning(NULL, 'A', 'V') == 0.0f);
    BTBHGPUFontMetrics metrics;
    assert(btbh_gpu_font_metrics(NULL, &metrics) == BTBH_GPU_INVALID_ARGUMENT);
    assert(btbh_gpu_font_metrics(ex, NULL) == BTBH_GPU_INVALID_ARGUMENT);
    btbh_gpu_font_destroy(ex);

    /* A codepoint the font lacks keeps an empty glyph, as with the plain loader; an unknown one
       falls back to '?'. */
    const uint32_t with_missing[3] = {'?', 'A', 0x4E00u};
    BTBHGPUFontDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.pixel_size = 12.0f;
    desc.oversample_x = 3u;
    BTBHGPUFont *partial = btbh_gpu_window_font_load_ex(
        window, BTBH_GPU_FONT_SMOKE_FONT, &desc, with_missing, 3u, &report);
    assert(partial != NULL);
    BTBHGPUFontGlyph glyph;
    assert(btbh_gpu_font_glyph(partial, 0x4E00u, &glyph) == BTBH_GPU_OK);
    assert(glyph.codepoint == 0x4E00u && glyph.advance == 0.0f && glyph.width == 0.0f);
    assert(btbh_gpu_font_glyph(partial, 'Z', &glyph) == BTBH_GPU_OK);
    assert(glyph.codepoint == '?' && glyph.advance > 0.0f);
    /* (The plain loader can't pack as few glyphs as three: it gets the whole set and the missing one.) */
    uint32_t plain_set[129];
    memcpy(plain_set, codepoints, codepoint_count * sizeof(codepoints[0]));
    plain_set[codepoint_count] = 0x4E00u;
    BTBHGPUFontReport plain_report;
    BTBHGPUFont *plain_partial = btbh_gpu_window_font_load(
        window, BTBH_GPU_FONT_SMOKE_FONT, 12u, plain_set, codepoint_count + 1u, &plain_report);
    assert(plain_partial != NULL);
    assert(btbh_gpu_font_glyph(plain_partial, 0x4E00u, &glyph) == BTBH_GPU_OK);
    assert(glyph.advance == 0.0f);
    btbh_gpu_font_destroy(plain_partial);
    btbh_gpu_font_destroy(partial);
    /* None of them: an empty atlas, still a font. */
    const uint32_t none[1] = {0x4E00u};
    BTBHGPUFont *empty = btbh_gpu_window_font_load_ex(
        window, BTBH_GPU_FONT_SMOKE_FONT, &desc, none, 1u, &report);
    assert(empty != NULL && report.result == BTBH_GPU_OK);
    btbh_gpu_font_destroy(empty);
}

/* coverage_gamma 1.3 raises every partial coverage and leaves 0 and 255: the same atlas otherwise. */
static void check_coverage_curve(BTBHGPUWindow *window) {
    BTBHGPUFontReport linear_report;
    BTBHGPUFontReport curved_report;
    BTBHGPUFont *linear = load_ex(window, 12.0f, 3u, 0u, 0.0f, &linear_report);
    BTBHGPUFont *curved = load_ex(window, 12.0f, 3u, 0u, 1.3f, &curved_report);
    assert(linear_report.atlas_width == curved_report.atlas_width);
    assert(linear_report.atlas_height == curved_report.atlas_height);
    uint8_t *a = read_texture(window, btbh_gpu_font_atlas(linear));
    uint8_t *b = read_texture(window, btbh_gpu_font_atlas(curved));
    const uint32_t count = linear_report.atlas_width * linear_report.atlas_height;
    uint32_t raised = 0u;
    uint32_t partial = 0u;
    uint64_t sum_a = 0u;
    uint64_t sum_b = 0u;
    for (uint32_t i = 0u; i < count; i += 1u) {
        const uint8_t before = a[i * 4u + 3u];
        const uint8_t after = b[i * 4u + 3u];
        assert(a[i * 4u] == 255u && b[i * 4u] == 255u);
        assert(after >= before);
        if (before == 0u || before == 255u) {
            assert(after == before);
        } else {
            partial += 1u;
            raised += after > before ? 1u : 0u;
        }
        sum_a += before;
        sum_b += after;
    }
    assert(partial > 0u && raised * 2u > partial);
    printf(
        "coverage_gamma 1.3: %u of %u partial texels raised, total coverage x%.3f\n",
        raised,
        partial,
        (double)sum_b / (double)sum_a
    );
    free(a);
    free(b);
    btbh_gpu_font_destroy(linear);
    btbh_gpu_font_destroy(curved);
}

static void push_quad(
    float x0, float y0, float x1, float y1,
    float u0, float v0, float u1, float v1
) {
    assert(vertex_count + 6u <= MAX_VERTICES);
    const BTBHGPUCanvasVertex corners[4] = {
        {x0, y0, u0, v0, ink[0], ink[1], ink[2], ink[3]},
        {x1, y0, u1, v0, ink[0], ink[1], ink[2], ink[3]},
        {x1, y1, u1, v1, ink[0], ink[1], ink[2], ink[3]},
        {x0, y1, u0, v1, ink[0], ink[1], ink[2], ink[3]},
    };
    const int order[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; i += 1) {
        vertices[vertex_count++] = corners[order[i]];
    }
}

typedef struct LineStats {
    float width;        /* where the pen ended, from the run's start */
    float worst_offset; /* the furthest a glyph was placed from its own fractional pen */
} LineStats;

/* Lays out text at (x, baseline) as variant does; one batch of quads from font's atlas. */
static LineStats lay_out_text(
    const char *text,
    uint32_t variant,
    const BTBHGPUFont *font,
    const BTBHGPUFont *big,
    float size,
    float x,
    float baseline
) {
    LineStats stats = {0.0f, 0.0f};
    float pen = x;
    uint32_t previous = 0u;
    for (const char *c = text; *c != '\0'; c += 1) {
        const uint32_t codepoint = (uint32_t)(unsigned char)*c;
        BTBHGPUFontGlyph g;
        assert(btbh_gpu_font_glyph(font, codepoint, &g) == BTBH_GPU_OK);
        float placed = pen;
        float advance = g.advance;
        if (variant == 1u) {
            /* CHIMERA: the advance of its biggest face, scaled; each glyph on the nearest pixel. */
            BTBHGPUFontGlyph b;
            assert(btbh_gpu_font_glyph(big, codepoint, &b) == BTBH_GPU_OK);
            advance = b.advance * size / 60.0f;
            placed = roundf(pen);
        } else if (variant >= 2u) {
            if (previous != 0u) {
                pen += btbh_gpu_font_kerning(font, previous, codepoint);
            }
            placed = floorf(pen * 3.0f + 0.5f) / 3.0f;
        }
        stats.worst_offset = fmaxf(stats.worst_offset, fabsf(placed - pen));
        if (codepoint != ' ' && g.width > 0.0f) {
            const float x0 = placed + g.offset_x;
            const float y0 = baseline + g.offset_y;
            push_quad(x0, y0, x0 + g.width, y0 + g.height, g.u0, g.v0, g.u1, g.v1);
        }
        pen += advance;
        previous = codepoint;
    }
    stats.width = pen - x;
    return stats;
}

static LineStats lay_out(
    uint32_t variant,
    const BTBHGPUFont *font,
    const BTBHGPUFont *big,
    float size,
    float x,
    float baseline
) {
    return lay_out_text(text_line, variant, font, big, size, x, baseline);
}

static void write_png(const char *dir, const char *name, uint32_t width, uint32_t height, const uint8_t *rgba) {
    char path[1024];
    (void)snprintf(path, sizeof(path), "%s/%s", dir, name);
    assert(btbh_gpu_write_png_rgba8(path, width, height, rgba) == BTBH_GPU_OK);
}

static void check_readback(BTBHGPUWindow *window) {
    const char *dump = getenv("BTBH_SMOKE_DUMP");
    const char *dir = dump != NULL ? dump : BTBH_GPU_FONT_SMOKE_OUTPUT_DIR;
    (void)SDL_CreateDirectory(dir);

    BTBHGPUTextureReport texture_report;
    BTBHGPUTexture *target = btbh_gpu_window_render_target_create(
        window, CANVAS_WIDTH, CANVAS_HEIGHT, BTBH_GPU_TEXTURE_FILTER_NEAREST, &texture_report);
    assert(target != NULL);

    BTBHGPUFontReport report;
    BTBHGPUFont *big = load_plain(window, 60u, &report);
    BTBHGPUFont *fonts[SIZE_COUNT][VARIANT_COUNT];
    uint32_t row_top[SIZE_COUNT][VARIANT_COUNT];
    uint32_t row_height[SIZE_COUNT];
    LineStats stats[SIZE_COUNT][VARIANT_COUNT];
    BTBHGPUCanvasBatch batches[MAX_BATCHES];
    uint32_t batch_count = 0u;
    vertex_count = 0u;
    uint32_t y = 6u;
    for (uint32_t s = 0u; s < SIZE_COUNT; s += 1u) {
        const float size = (float)sizes[s];
        const uint64_t plain_start = SDL_GetTicksNS();
        fonts[s][0] = load_plain(window, sizes[s], &report);
        const uint64_t plain_time = SDL_GetTicksNS() - plain_start;
        fonts[s][1] = load_plain(window, sizes[s], &report);
        const uint64_t ex_start = SDL_GetTicksNS();
        fonts[s][2] = load_ex(window, size, 3u, 0u, 0.0f, &report);
        const uint64_t ex_time = SDL_GetTicksNS() - ex_start;
        printf(
            "%u px, %u glyphs: plain bake %.2f ms; _ex (oversample 3, kerning) %.2f ms, atlas %ux%u\n",
            sizes[s],
            codepoint_count,
            (double)plain_time / 1e6,
            (double)ex_time / 1e6,
            report.atlas_width,
            report.atlas_height
        );
        fonts[s][3] = load_ex(window, size, 3u, 0u, 1.3f, &report);
        BTBHGPUFontMetrics metrics;
        assert(btbh_gpu_font_metrics(fonts[s][2], &metrics) == BTBH_GPU_OK);
        row_height[s] = (uint32_t)ceilf(size * 1.5f) + 2u;
        for (uint32_t v = 0u; v < VARIANT_COUNT; v += 1u) {
            row_top[s][v] = y;
            const float baseline = (float)y + 1.0f + roundf(metrics.ascent);
            const uint32_t first = vertex_count;
            stats[s][v] = lay_out(v, fonts[s][v], big, size, 8.0f, baseline);
            assert(batch_count < MAX_BATCHES);
            BTBHGPUCanvasBatch *batch = &batches[batch_count++];
            memset(batch, 0, sizeof(*batch));
            batch->first_vertex = first;
            batch->vertex_count = vertex_count - first;
            batch->texture = btbh_gpu_font_atlas(fonts[s][v]);
            batch->blend_mode = BTBH_GPU_CANVAS_BLEND_ALPHA;
            y += row_height[s];
        }
        y += 4u;
    }
    assert(y <= CANVAS_HEIGHT);

    BTBHGPUCanvasPass pass;
    memset(&pass, 0, sizeof(pass));
    pass.first_batch = 0u;
    pass.batch_count = batch_count;
    pass.target = target;
    pass.clear_enabled = 1u;
    pass.clear_red = background[0];
    pass.clear_green = background[1];
    pass.clear_blue = background[2];
    pass.clear_alpha = 255u;
    BTBHGPUCanvasReport canvas_report;
    const BTBHGPUResult result = btbh_gpu_window_canvas_present(
        window,
        shader_paths[0],
        shader_paths[1],
        shader_paths[2],
        SMOKE_SHADER_FORMAT,
        vertices,
        vertex_count,
        batches,
        batch_count,
        &pass,
        1u,
        &canvas_report
    );
    if (result != BTBH_GPU_OK) {
        fprintf(stderr, "canvas: %s\n", canvas_report.message);
    }
    assert(result == BTBH_GPU_OK);
    uint8_t *pixels = read_texture(window, target);
    for (uint32_t i = 0u; i < CANVAS_WIDTH * CANVAS_HEIGHT; i += 1u) {
        pixels[i * 4u + 3u] = 255u;
    }

    /* Each row has text on it; _ex's coverage is the plain loader's within a third, and the
       curve's is more. */
    for (uint32_t s = 0u; s < SIZE_COUNT; s += 1u) {
        double coverage[VARIANT_COUNT];
        for (uint32_t v = 0u; v < VARIANT_COUNT; v += 1u) {
            double sum = 0.0;
            uint32_t lit = 0u;
            for (uint32_t row = 0u; row < row_height[s]; row += 1u) {
                const uint8_t *line = &pixels[(size_t)(row_top[s][v] + row) * CANVAS_WIDTH * 4u];
                for (uint32_t x = 0u; x < CANVAS_WIDTH; x += 1u) {
                    const int excess = (int)line[x * 4u + 1u] - (int)background[1];
                    if (excess > 0) {
                        sum += (double)excess;
                        lit += excess > 16 ? 1u : 0u;
                    }
                }
            }
            coverage[v] = sum / (232.0 - (double)background[1]);
            assert(lit > 100u);
            char name[64];
            (void)snprintf(name, sizeof(name), "text-%upx-%s.png", sizes[s], variant_names[v]);
            write_png(
                dir,
                name,
                CANVAS_WIDTH,
                row_height[s],
                &pixels[(size_t)row_top[s][v] * CANVAS_WIDTH * 4u]
            );
        }
        printf(
            "%u px: ink (pixels of full coverage) plain %.0f, snapped %.0f, ex %.0f, ex-gamma %.0f; "
            "line width plain %.2f, snapped %.2f, ex %.2f px; glyphs off their pen by up to "
            "snapped %.3f, ex %.3f px\n",
            sizes[s],
            coverage[0],
            coverage[1],
            coverage[2],
            coverage[3],
            (double)stats[s][0].width,
            (double)stats[s][1].width,
            (double)stats[s][2].width,
            (double)stats[s][1].worst_offset,
            (double)stats[s][2].worst_offset
        );
        assert(fabs(coverage[2] / coverage[0] - 1.0) < 0.34);
        assert(coverage[3] > coverage[2]);
        assert(stats[s][1].worst_offset <= 0.5f + 1e-4f);
        assert(stats[s][2].worst_offset <= 1.0f / 6.0f + 1e-4f);
    }
    write_png(dir, "text-all.png", CANVAS_WIDTH, CANVAS_HEIGHT, pixels);
    const uint32_t zoom = 4u;
    uint8_t *zoomed = malloc((size_t)CANVAS_WIDTH * CANVAS_HEIGHT * 4u * zoom * zoom);
    assert(zoomed != NULL);
    for (uint32_t zy = 0u; zy < CANVAS_HEIGHT * zoom; zy += 1u) {
        for (uint32_t zx = 0u; zx < CANVAS_WIDTH * zoom; zx += 1u) {
            memcpy(
                &zoomed[((size_t)zy * CANVAS_WIDTH * zoom + zx) * 4u],
                &pixels[((size_t)(zy / zoom) * CANVAS_WIDTH + zx / zoom) * 4u],
                4u
            );
        }
    }
    write_png(dir, "text-all-x4.png", CANVAS_WIDTH * zoom, CANVAS_HEIGHT * zoom, zoomed);
    printf("text PNGs in %s\n", dir);
    free(zoomed);
    free(pixels);
    for (uint32_t s = 0u; s < SIZE_COUNT; s += 1u) {
        for (uint32_t v = 0u; v < VARIANT_COUNT; v += 1u) {
            btbh_gpu_font_destroy(fonts[s][v]);
        }
    }
    btbh_gpu_font_destroy(big);
    btbh_gpu_texture_destroy(target);
}

/* How much ink a read-back pixel has, 0 to 1, from its green against the background's. */
static float pixel_coverage(const uint8_t *pixels, uint32_t x, uint32_t y) {
    const float ink_green = roundf(ink[1] * 255.0f);
    const float green = (float)pixels[((size_t)y * CANVAS_WIDTH + x) * 4u + 1u];
    return fminf(fmaxf((green - (float)background[1]) / (ink_green - (float)background[1]), 0.0f), 1.0f);
}

/* The most ink in row y between columns x0 and x1. */
static float row_peak(const uint8_t *pixels, uint32_t y, uint32_t x0, uint32_t x1) {
    float peak = 0.0f;
    for (uint32_t x = x0; x < x1 && x < CANVAS_WIDTH; x += 1u) {
        peak = fmaxf(peak, pixel_coverage(pixels, x, y));
    }
    return peak;
}

#define EXTRA_COUNT 3u
#define PROBE_LETTERS 7u

/* The three font flags drawn: the line at 10, 12 and 16 px from the RGBA atlas, the R8 atlas and with
   the x-height snapped, and a probe row of lowercase without and with the snap, read back:
   text-<size>px-ex-r8.png, text-<size>px-ex-snap.png (compare text-<size>px-ex.png), and
   text-extras.png / text-extras-x4.png with every row. */
static void check_extras_readback(BTBHGPUWindow *window) {
    static const char *const extra_names[EXTRA_COUNT] = {"ex", "ex-r8", "ex-snap"};
    static const uint8_t extra_flags[EXTRA_COUNT] = {
        0u, BTBH_GPU_FONT_FLAG_R8_ATLAS, BTBH_GPU_FONT_FLAG_SNAP_X_HEIGHT,
    };
    static const char probe_letters[PROBE_LETTERS + 1u] = "xzuvnoe";
    const char *dump = getenv("BTBH_SMOKE_DUMP");
    const char *dir = dump != NULL ? dump : BTBH_GPU_FONT_SMOKE_OUTPUT_DIR;
    (void)SDL_CreateDirectory(dir);

    BTBHGPUTextureReport texture_report;
    BTBHGPUTexture *target = btbh_gpu_window_render_target_create(
        window, CANVAS_WIDTH, CANVAS_HEIGHT, BTBH_GPU_TEXTURE_FILTER_NEAREST, &texture_report);
    assert(target != NULL);

    BTBHGPUFont *fonts[SIZE_COUNT][EXTRA_COUNT];
    BTBHGPUFontMetrics metrics[SIZE_COUNT][EXTRA_COUNT];
    uint32_t row_top[SIZE_COUNT][EXTRA_COUNT + 1u];
    uint32_t row_height[SIZE_COUNT];
    float probe_baseline[SIZE_COUNT];
    uint32_t probe_span[SIZE_COUNT][2][PROBE_LETTERS][2];
    BTBHGPUCanvasBatch batches[MAX_BATCHES];
    uint32_t batch_count = 0u;
    vertex_count = 0u;
    uint32_t y = 6u;
    for (uint32_t s = 0u; s < SIZE_COUNT; s += 1u) {
        const float size = (float)sizes[s];
        BTBHGPUFontReport report;
        for (uint32_t e = 0u; e < EXTRA_COUNT; e += 1u) {
            fonts[s][e] = load_ex_flags(window, size, 3u, 0u, 0.0f, extra_flags[e], &report);
            assert(btbh_gpu_font_metrics(fonts[s][e], &metrics[s][e]) == BTBH_GPU_OK);
        }
        row_height[s] = (uint32_t)ceilf(size * 1.5f) + 2u;
        for (uint32_t e = 0u; e <= EXTRA_COUNT; e += 1u) {
            row_top[s][e] = y;
            /* The baseline as check_readback puts it, from the unsnapped ascent, on a whole pixel. */
            const float baseline = (float)y + 1.0f + roundf(metrics[s][0].ascent);
            const uint32_t first = vertex_count;
            if (e < EXTRA_COUNT) {
                (void)lay_out(2u, fonts[s][e], NULL, size, 8.0f, baseline);
                assert(batch_count < MAX_BATCHES);
                BTBHGPUCanvasBatch *batch = &batches[batch_count++];
                memset(batch, 0, sizeof(*batch));
                batch->first_vertex = first;
                batch->vertex_count = vertex_count - first;
                batch->texture = btbh_gpu_font_atlas(fonts[s][e]);
                batch->blend_mode = BTBH_GPU_CANVAS_BLEND_ALPHA;
            } else {
                /* The probe: three of each letter, unsnapped from x = 8, snapped from x = 328. */
                probe_baseline[s] = baseline;
                for (uint32_t side = 0u; side < 2u; side += 1u) {
                    const BTBHGPUFont *font = fonts[s][side == 0u ? 0u : 2u];
                    const uint32_t side_first = vertex_count;
                    float x = side == 0u ? 8.0f : 328.0f;
                    for (uint32_t l = 0u; l < PROBE_LETTERS; l += 1u) {
                        const char text[4] = {probe_letters[l], probe_letters[l], probe_letters[l], '\0'};
                        const LineStats stats = lay_out_text(text, 2u, font, NULL, size, x, baseline);
                        probe_span[s][side][l][0] = (uint32_t)floorf(x - 1.0f);
                        probe_span[s][side][l][1] = (uint32_t)ceilf(x + stats.width + 1.0f);
                        x = ceilf(x + stats.width + size * 0.6f);
                    }
                    assert(x < (side == 0u ? 328.0f : (float)CANVAS_WIDTH));
                    assert(batch_count < MAX_BATCHES);
                    BTBHGPUCanvasBatch *batch = &batches[batch_count++];
                    memset(batch, 0, sizeof(*batch));
                    batch->first_vertex = side_first;
                    batch->vertex_count = vertex_count - side_first;
                    batch->texture = btbh_gpu_font_atlas(fonts[s][side == 0u ? 0u : 2u]);
                    batch->blend_mode = BTBH_GPU_CANVAS_BLEND_ALPHA;
                }
            }
            y += row_height[s];
        }
        y += 4u;
    }
    assert(y <= CANVAS_HEIGHT);

    BTBHGPUCanvasPass pass;
    memset(&pass, 0, sizeof(pass));
    pass.first_batch = 0u;
    pass.batch_count = batch_count;
    pass.target = target;
    pass.clear_enabled = 1u;
    pass.clear_red = background[0];
    pass.clear_green = background[1];
    pass.clear_blue = background[2];
    pass.clear_alpha = 255u;
    BTBHGPUCanvasReport canvas_report;

    /* Without the coverage shader next to the textured one, an R8 batch fails, and says why. */
    BTBHGPUResult result = btbh_gpu_window_canvas_present(
        window,
        shader_paths[0],
        shader_paths[1],
        "no-such-directory/production_canvas_textured.frag" SMOKE_SHADER_EXTENSION,
        SMOKE_SHADER_FORMAT,
        vertices,
        vertex_count,
        batches,
        batch_count,
        &pass,
        1u,
        &canvas_report
    );
    assert(result == BTBH_GPU_INVALID_ARGUMENT);
    assert(strstr(canvas_report.message, "production_canvas_coverage.frag") != NULL);
    printf("canvas without the coverage shader: %s\n", canvas_report.message);

    result = btbh_gpu_window_canvas_present(
        window,
        shader_paths[0],
        shader_paths[1],
        shader_paths[2],
        SMOKE_SHADER_FORMAT,
        vertices,
        vertex_count,
        batches,
        batch_count,
        &pass,
        1u,
        &canvas_report
    );
    if (result != BTBH_GPU_OK) {
        fprintf(stderr, "canvas: %s\n", canvas_report.message);
    }
    assert(result == BTBH_GPU_OK);
    uint8_t *pixels = read_texture(window, target);
    for (uint32_t i = 0u; i < CANVAS_WIDTH * CANVAS_HEIGHT; i += 1u) {
        pixels[i * 4u + 3u] = 255u;
    }

    for (uint32_t s = 0u; s < SIZE_COUNT; s += 1u) {
        /* The R8 row against the RGBA row, pixel for pixel. */
        int worst = 0;
        uint32_t differing = 0u;
        uint32_t lit = 0u;
        for (uint32_t row = 0u; row < row_height[s]; row += 1u) {
            const uint8_t *a = &pixels[(size_t)(row_top[s][0] + row) * CANVAS_WIDTH * 4u];
            const uint8_t *b = &pixels[(size_t)(row_top[s][1] + row) * CANVAS_WIDTH * 4u];
            for (uint32_t x = 0u; x < CANVAS_WIDTH; x += 1u) {
                int pixel_worst = 0;
                for (uint32_t c = 0u; c < 3u; c += 1u) {
                    const int difference = abs((int)a[x * 4u + c] - (int)b[x * 4u + c]);
                    pixel_worst = difference > pixel_worst ? difference : pixel_worst;
                }
                worst = pixel_worst > worst ? pixel_worst : worst;
                differing += pixel_worst != 0 ? 1u : 0u;
                lit += (int)a[x * 4u + 1u] > (int)background[1] + 16 ? 1u : 0u;
            }
        }
        printf(
            "%u px: R8 atlas against RGBA8, drawn: %u of %u pixels differ, by at most %d (%u lit)\n",
            sizes[s], differing, CANVAS_WIDTH * row_height[s], worst, lit);
        assert(lit > 100u);
        assert(worst <= 1);

        /* The probe, round the top of the x-height: the most ink in the row above it and in the row
           it ends in, and that row's fill, its ink over the most in any row below it down to the
           baseline (so a stem narrower than a pixel still fills its row: 1). */
        printf("%u px top of the x-height (peak ink in the row above / the x-height's top row, fill):\n",
            sizes[s]);
        float fill[2][PROBE_LETTERS];
        float above[2][PROBE_LETTERS];
        for (uint32_t side = 0u; side < 2u; side += 1u) {
            const float x_height = metrics[s][side == 0u ? 0u : 2u].x_height;
            const uint32_t baseline = (uint32_t)probe_baseline[s];
            const uint32_t top = baseline - (uint32_t)ceilf(x_height - 1e-3f);
            printf("  %-7s x-height %.3f:", side == 0u ? "ex" : "ex-snap", (double)x_height);
            for (uint32_t l = 0u; l < PROBE_LETTERS; l += 1u) {
                const uint32_t x0 = probe_span[s][side][l][0];
                const uint32_t x1 = probe_span[s][side][l][1];
                const float at = row_peak(pixels, top, x0, x1);
                float body = 0.0f;
                for (uint32_t row = top + 1u; row < baseline; row += 1u) {
                    body = fmaxf(body, row_peak(pixels, row, x0, x1));
                }
                above[side][l] = row_peak(pixels, top - 1u, x0, x1);
                fill[side][l] = body > 0.0f ? at / body : 0.0f;
                printf(" %c %.2f/%.2f %.2f", probe_letters[l], (double)above[side][l], (double)at,
                    (double)fill[side][l]);
            }
            printf("\n");
        }
        for (uint32_t l = 0u; l < PROBE_LETTERS; l += 1u) {
            const char letter = probe_letters[l];
            /* Flat tops ('x', 'z', 'u', 'v'), snapped: nothing above the x-height, its top row full
               (the bar of 'z' and the stems of 'u'), and never less full than unsnapped. */
            if (letter == 'x' || letter == 'z' || letter == 'u' || letter == 'v') {
                assert(above[1][l] <= 0.1f);
                assert(fill[1][l] + 0.02f >= fill[0][l]);
            }
            if (letter == 'z' || letter == 'u') {
                assert(fill[1][l] >= 0.9f);
            }
        }

        for (uint32_t e = 1u; e < EXTRA_COUNT; e += 1u) {
            char name[64];
            (void)snprintf(name, sizeof(name), "text-%upx-%s.png", sizes[s], extra_names[e]);
            write_png(dir, name, CANVAS_WIDTH, row_height[s],
                &pixels[(size_t)row_top[s][e] * CANVAS_WIDTH * 4u]);
        }
        char probe_name[64];
        (void)snprintf(probe_name, sizeof(probe_name), "probe-%upx-ex-vs-snap.png", sizes[s]);
        write_png(dir, probe_name, CANVAS_WIDTH, row_height[s],
            &pixels[(size_t)row_top[s][EXTRA_COUNT] * CANVAS_WIDTH * 4u]);
    }
    write_png(dir, "text-extras.png", CANVAS_WIDTH, CANVAS_HEIGHT, pixels);
    const uint32_t zoom = 4u;
    uint8_t *zoomed = malloc((size_t)CANVAS_WIDTH * CANVAS_HEIGHT * 4u * zoom * zoom);
    assert(zoomed != NULL);
    for (uint32_t zy = 0u; zy < CANVAS_HEIGHT * zoom; zy += 1u) {
        for (uint32_t zx = 0u; zx < CANVAS_WIDTH * zoom; zx += 1u) {
            memcpy(
                &zoomed[((size_t)zy * CANVAS_WIDTH * zoom + zx) * 4u],
                &pixels[((size_t)(zy / zoom) * CANVAS_WIDTH + zx / zoom) * 4u],
                4u
            );
        }
    }
    write_png(dir, "text-extras-x4.png", CANVAS_WIDTH * zoom, CANVAS_HEIGHT * zoom, zoomed);
    free(zoomed);
    free(pixels);
    for (uint32_t s = 0u; s < SIZE_COUNT; s += 1u) {
        for (uint32_t e = 0u; e < EXTRA_COUNT; e += 1u) {
            btbh_gpu_font_destroy(fonts[s][e]);
        }
    }
    btbh_gpu_texture_destroy(target);
}

int main(void) {
    static const char *const shader_names[4] = {
        "production_canvas.vert",
        "production_canvas.frag",
        "production_canvas_textured.frag",
        "production_canvas_coverage.frag",
    };
    for (int index = 0; index < 4; index += 1) {
        (void)snprintf(
            shader_paths[index],
            sizeof(shader_paths[index]),
            "%s/%s%s",
            BTBH_GPU_SMOKE_SHADER_DIR,
            shader_names[index],
            SMOKE_SHADER_EXTENSION
        );
        FILE *file = fopen(shader_paths[index], "rb");
        if (file == NULL) {
            fprintf(stderr, "skipped: no shader %s\n", shader_paths[index]);
            return SMOKE_SKIP;
        }
        fclose(file);
    }
    (void)setvbuf(stdout, NULL, _IONBF, 0);
    fill_codepoints();
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
    const BTBHGPUWindowDescriptor descriptor = {
        .title = "BTBH font smoke",
        .width = CANVAS_WIDTH,
        .height = CANVAS_HEIGHT,
        .hidden = 1,
        .high_pixel_density = 0,
    };
    BTBHGPUWindowReport window_report;
    memset(&window_report, 0, sizeof(window_report));
    BTBHGPUWindow *window = btbh_gpu_window_create(&descriptor, &window_report);
    assert(window != NULL);
    assert(btbh_gpu_window_claim(device, window, &window_report) == BTBH_GPU_OK);

    check_validation(device, window);
    check_metrics(window);
    check_kerning_pairs(window);
    check_r8_atlas(window);
    check_snapped_metrics(window);
    check_coverage_curve(window);
    check_readback(window);
    check_extras_readback(window);

    /* A font outlives its window: metrics and kerning still answer. */
    BTBHGPUFontReport report;
    BTBHGPUFont *kept = load_ex(window, 12.0f, 3u, 0u, 0.0f, &report);
    const float kern = btbh_gpu_font_kerning(kept, 'A', 'V');
    const uint32_t kept_pairs = btbh_gpu_font_kerning_pairs(kept, NULL, 0u);
    btbh_gpu_window_destroy(window);
    assert(btbh_gpu_font_atlas(kept) == NULL);
    BTBHGPUFontMetrics metrics;
    assert(btbh_gpu_font_metrics(kept, &metrics) == BTBH_GPU_OK && metrics.pixel_size == 12.0f);
    assert(btbh_gpu_font_kerning(kept, 'A', 'V') == kern);
    assert(kept_pairs > 0u && btbh_gpu_font_kerning_pairs(kept, NULL, 0u) == kept_pairs);
    btbh_gpu_font_destroy(kept);

    btbh_gpu_device_shutdown(device);
    btbh_gpu_device_destroy(device);
    printf("font smoke passed\n");
    return 0;
}
