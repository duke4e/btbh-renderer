#include "btbh_gpu_internal.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BTBH_GPU_FONT_GLYPH_PADDING 4u

#define STB_TRUETYPE_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#endif
#include "stb_truetype.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

typedef struct BTBHGPUFontGlyphData {
    int32_t advance;
    int32_t offset_x;
    int32_t offset_y;
    uint32_t width;
    uint32_t height;
    uint32_t atlas_x;
    uint32_t atlas_y;
} BTBHGPUFontGlyphData;

typedef struct BTBHGPUFontRasterGlyph {
    uint8_t *pixels;
    uint32_t width;
    uint32_t height;
} BTBHGPUFontRasterGlyph;

struct BTBHGPUFont {
    BTBHGPUWindow *owner;
    BTBHGPUFont *previous;
    BTBHGPUFont *next;
    BTBHGPUTexture *atlas;
    uint32_t base_size;
    uint32_t glyph_count;
    uint32_t atlas_width;
    uint32_t atlas_height;
    uint32_t *codepoints;
    BTBHGPUFontGlyphData *glyphs;
    float ascent;
    float descent;
    float line_gap;
};

static void btbh_gpu_font_error(
    BTBHGPUFontReport *report,
    const char *message
) {
    report->result = BTBH_GPU_DEVICE_FAILURE;
    (void)snprintf(
        report->message,
        sizeof(report->message),
        "%s",
        message != NULL ? message : ""
    );
}

static void btbh_gpu_font_unlink(BTBHGPUFont *font) {
    BTBHGPUWindow *owner = font->owner;
    if (owner == NULL) {
        return;
    }
    if (font->previous != NULL) {
        font->previous->next = font->next;
    } else if (owner->font_head == font) {
        owner->font_head = font->next;
    }
    if (font->next != NULL) {
        font->next->previous = font->previous;
    }
    font->owner = NULL;
    font->previous = NULL;
    font->next = NULL;
}

static void btbh_gpu_font_free_rasters(
    BTBHGPUFontRasterGlyph *rasters,
    uint32_t glyph_count
) {
    if (rasters == NULL) {
        return;
    }
    for (uint32_t index = 0u; index < glyph_count; index += 1u) {
        stbtt_FreeBitmap(rasters[index].pixels, NULL);
    }
    free(rasters);
}

static bool btbh_gpu_font_atlas_size(
    uint32_t base_size,
    const BTBHGPUFontRasterGlyph *rasters,
    uint32_t glyph_count,
    uint32_t *atlas_width,
    uint32_t *atlas_height
) {
    uint64_t total_width = 0u;
    for (uint32_t index = 0u; index < glyph_count; index += 1u) {
        total_width +=
            (uint64_t)rasters[index].width +
            2u * BTBH_GPU_FONT_GLYPH_PADDING;
    }
    const float padded_font_size = (float)(
        base_size + 2u * BTBH_GPU_FONT_GLYPH_PADDING
    );
    const float total_area =
        (float)total_width * padded_font_size * 1.2f;
    if (!(total_area > 0.0f) || !isfinite(total_area)) {
        return false;
    }
    const float minimum_size = sqrtf(total_area);
    const float size_power = ceilf(logf(minimum_size) / logf(2.0f));
    const float size_value = powf(2.0f, size_power);
    if (
        !isfinite(size_value) ||
        size_value < 2.0f ||
        size_value > (float)UINT32_MAX
    ) {
        return false;
    }
    const uint32_t size = (uint32_t)size_value;
    *atlas_width = size;
    *atlas_height =
        total_area < (float)(((uint64_t)size * (uint64_t)size) / 2u)
        ? size / 2u
        : size;
    return *atlas_height > 0u;
}

static bool btbh_gpu_font_pack(
    const BTBHGPUFontRasterGlyph *rasters,
    BTBHGPUFontGlyphData *glyphs,
    uint32_t glyph_count,
    uint32_t base_size,
    uint32_t atlas_width,
    uint32_t atlas_height,
    uint8_t *bitmap
) {
    uint32_t offset_x = BTBH_GPU_FONT_GLYPH_PADDING;
    uint32_t offset_y = BTBH_GPU_FONT_GLYPH_PADDING;
    for (uint32_t index = 0u; index < glyph_count; index += 1u) {
        const BTBHGPUFontRasterGlyph *raster = &rasters[index];
        if (
            (uint64_t)offset_x >=
            (uint64_t)atlas_width -
                (uint64_t)raster->width -
                2u * BTBH_GPU_FONT_GLYPH_PADDING
        ) {
            offset_x = BTBH_GPU_FONT_GLYPH_PADDING;
            offset_y +=
                base_size + 2u * BTBH_GPU_FONT_GLYPH_PADDING;
            if (
                (uint64_t)offset_y >
                (uint64_t)atlas_height -
                    (uint64_t)base_size -
                    BTBH_GPU_FONT_GLYPH_PADDING
            ) {
                return false;
            }
        }
        if (
            (uint64_t)offset_x + (uint64_t)raster->width >
                (uint64_t)atlas_width ||
            (uint64_t)offset_y + (uint64_t)raster->height >
                (uint64_t)atlas_height
        ) {
            return false;
        }
        for (uint32_t row = 0u; row < raster->height; row += 1u) {
            if (raster->width == 0u) {
                continue;
            }
            (void)memcpy(
                bitmap +
                    ((size_t)offset_y + (size_t)row) *
                        (size_t)atlas_width +
                    (size_t)offset_x,
                raster->pixels +
                    (size_t)row * (size_t)raster->width,
                raster->width
            );
        }
        glyphs[index].atlas_x = offset_x;
        glyphs[index].atlas_y = offset_y;
        glyphs[index].width = raster->width;
        glyphs[index].height = raster->height;
        offset_x +=
            raster->width + 2u * BTBH_GPU_FONT_GLYPH_PADDING;
    }
    return true;
}

BTBHGPUFont *btbh_gpu_window_font_load(
    BTBHGPUWindow *window,
    const char *path,
    uint32_t base_size,
    const uint32_t *glyphs,
    uint32_t glyph_count,
    BTBHGPUFontReport *report
) {
    if (
        window == NULL ||
        window->device == NULL ||
        window->device->gpu == NULL ||
        window->device->shutdown ||
        !window->claimed ||
        path == NULL ||
        base_size == 0u ||
        glyphs == NULL ||
        glyph_count == 0u ||
        glyph_count > (uint32_t)INT_MAX ||
        report == NULL
    ) {
        return NULL;
    }
    (void)memset(report, 0, sizeof(*report));
    size_t font_byte_count = 0u;
    uint8_t *font_bytes = SDL_LoadFile(path, &font_byte_count);
    if (font_bytes == NULL || font_byte_count == 0u) {
        SDL_free(font_bytes);
        btbh_gpu_font_error(report, SDL_GetError());
        return NULL;
    }

    uint32_t *stored_codepoints = malloc(
        (size_t)glyph_count * sizeof(*stored_codepoints)
    );
    BTBHGPUFontGlyphData *packed_glyphs = calloc(
        glyph_count,
        sizeof(*packed_glyphs)
    );
    BTBHGPUFontRasterGlyph *raster_glyphs = calloc(
        glyph_count,
        sizeof(*raster_glyphs)
    );
    if (
        stored_codepoints == NULL ||
        packed_glyphs == NULL ||
        raster_glyphs == NULL
    ) {
        btbh_gpu_font_free_rasters(raster_glyphs, glyph_count);
        free(packed_glyphs);
        free(stored_codepoints);
        SDL_free(font_bytes);
        btbh_gpu_font_error(report, "Could not allocate font glyph storage");
        return NULL;
    }

    stbtt_fontinfo info;
    if (
        stbtt_InitFont(
            &info,
            font_bytes,
            stbtt_GetFontOffsetForIndex(font_bytes, 0)
        ) == 0
    ) {
        btbh_gpu_font_free_rasters(raster_glyphs, glyph_count);
        free(packed_glyphs);
        free(stored_codepoints);
        SDL_free(font_bytes);
        btbh_gpu_font_error(report, "Could not initialize TTF font");
        return NULL;
    }
    int ascent = 0;
    int descent = 0;
    int line_gap = 0;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &line_gap);
    const float metric_scale = stbtt_ScaleForPixelHeight(
        &info,
        (float)base_size
    );

    bool valid = true;
    for (uint32_t index = 0u; index < glyph_count; index += 1u) {
        if (glyphs[index] > (uint32_t)INT_MAX) {
            valid = false;
            break;
        }
        stored_codepoints[index] = glyphs[index];
        const int codepoint = (int)glyphs[index];
        if (stbtt_FindGlyphIndex(&info, codepoint) <= 0) {
            continue;
        }
        int width = 0;
        int height = 0;
        int offset_x = 0;
        int offset_y = 0;
        uint8_t *pixels = stbtt_GetCodepointBitmap(
            &info,
            metric_scale,
            metric_scale,
            codepoint,
            &width,
            &height,
            &offset_x,
            &offset_y
        );
        int advance = 0;
        stbtt_GetCodepointHMetrics(&info, codepoint, &advance, NULL);
        advance = (int)((float)advance * metric_scale);
        packed_glyphs[index].advance = advance;
        packed_glyphs[index].offset_x = offset_x;
        packed_glyphs[index].offset_y = offset_y;
        if (codepoint == (int)' ') {
            stbtt_FreeBitmap(pixels, NULL);
            pixels = NULL;
            width = advance;
            height = (int)base_size;
            if (width > 0 && height > 0) {
                pixels = calloc(
                    (size_t)width * (size_t)height,
                    1u
                );
            }
        }
        if (
            width < 0 ||
            height < 0 ||
            (width > 0 && height > 0 && pixels == NULL)
        ) {
            stbtt_FreeBitmap(pixels, NULL);
            valid = false;
            break;
        }
        raster_glyphs[index].pixels = pixels;
        raster_glyphs[index].width = (uint32_t)width;
        raster_glyphs[index].height = (uint32_t)height;
    }

    uint32_t atlas_width = 0u;
    uint32_t atlas_height = 0u;
    if (
        !valid ||
        !btbh_gpu_font_atlas_size(
            base_size,
            raster_glyphs,
            glyph_count,
            &atlas_width,
            &atlas_height
        ) ||
        atlas_width > SIZE_MAX / atlas_height
    ) {
        btbh_gpu_font_free_rasters(raster_glyphs, glyph_count);
        free(packed_glyphs);
        free(stored_codepoints);
        SDL_free(font_bytes);
        btbh_gpu_font_error(report, "Could not size font glyph atlas");
        return NULL;
    }

    const size_t pixel_count =
        (size_t)atlas_width * (size_t)atlas_height;
    uint8_t *bitmap = calloc(pixel_count, 1u);
    if (
        bitmap == NULL ||
        !btbh_gpu_font_pack(
            raster_glyphs,
            packed_glyphs,
            glyph_count,
            base_size,
            atlas_width,
            atlas_height,
            bitmap
        )
    ) {
        free(bitmap);
        btbh_gpu_font_free_rasters(raster_glyphs, glyph_count);
        free(packed_glyphs);
        free(stored_codepoints);
        SDL_free(font_bytes);
        btbh_gpu_font_error(report, "Could not pack font glyph atlas");
        return NULL;
    }
    btbh_gpu_font_free_rasters(raster_glyphs, glyph_count);

    uint8_t *rgba = malloc(pixel_count * 4u);
    if (rgba == NULL) {
        free(bitmap);
        free(packed_glyphs);
        free(stored_codepoints);
        SDL_free(font_bytes);
        btbh_gpu_font_error(report, "Could not allocate font atlas pixels");
        return NULL;
    }
    for (size_t index = 0u; index < pixel_count; index += 1u) {
        rgba[index * 4u] = 255u;
        rgba[index * 4u + 1u] = 255u;
        rgba[index * 4u + 2u] = 255u;
        rgba[index * 4u + 3u] = bitmap[index];
    }
    free(bitmap);

    BTBHGPUTextureReport texture_report;
    BTBHGPUTexture *atlas = btbh_gpu_window_texture_create_rgba8(
        window,
        rgba,
        atlas_width,
        atlas_height,
        BTBH_GPU_TEXTURE_FILTER_LINEAR,  /* text drawn at any size */
        BTBH_GPU_TEXTURE_WRAP_CLAMP,
        &texture_report
    );
    free(rgba);
    if (atlas == NULL) {
        free(packed_glyphs);
        free(stored_codepoints);
        SDL_free(font_bytes);
        btbh_gpu_font_error(report, texture_report.message);
        return NULL;
    }

    SDL_free(font_bytes);

    BTBHGPUFont *font = calloc(1, sizeof(*font));
    if (font == NULL) {
        btbh_gpu_texture_destroy(atlas);
        free(packed_glyphs);
        free(stored_codepoints);
        btbh_gpu_font_error(report, "Could not allocate font resource");
        return NULL;
    }
    font->owner = window;
    font->next = window->font_head;
    if (window->font_head != NULL) {
        window->font_head->previous = font;
    }
    window->font_head = font;
    font->atlas = atlas;
    font->base_size = base_size;
    font->glyph_count = glyph_count;
    font->atlas_width = atlas_width;
    font->atlas_height = atlas_height;
    font->codepoints = stored_codepoints;
    font->glyphs = packed_glyphs;
    font->ascent = (float)((int)((float)ascent * metric_scale));
    font->descent = (float)descent * metric_scale;
    font->line_gap = 2.0f;

    report->result = BTBH_GPU_OK;
    report->base_size = base_size;
    report->glyph_count = glyph_count;
    report->atlas_width = atlas_width;
    report->atlas_height = atlas_height;
    report->ascent = font->ascent;
    report->descent = font->descent;
    report->line_gap = font->line_gap;
    return font;
}

BTBHGPUResult btbh_gpu_font_glyph(
    const BTBHGPUFont *font,
    uint32_t codepoint,
    BTBHGPUFontGlyph *glyph
) {
    if (
        font == NULL ||
        font->atlas == NULL ||
        glyph == NULL
    ) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    uint32_t glyph_index = UINT32_MAX;
    uint32_t fallback_index = UINT32_MAX;
    for (uint32_t index = 0u; index < font->glyph_count; index += 1u) {
        if (font->codepoints[index] == codepoint) {
            glyph_index = index;
            break;
        }
        if (font->codepoints[index] == (uint32_t)'?') {
            fallback_index = index;
        }
    }
    if (glyph_index == UINT32_MAX) {
        glyph_index = fallback_index;
    }
    if (glyph_index == UINT32_MAX) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    const BTBHGPUFontGlyphData *packed = &font->glyphs[glyph_index];
    const float padding = (float)BTBH_GPU_FONT_GLYPH_PADDING;
    (void)memset(glyph, 0, sizeof(*glyph));
    glyph->codepoint = font->codepoints[glyph_index];
    glyph->advance = (float)packed->advance;
    glyph->offset_x = (float)packed->offset_x - padding;
    glyph->offset_y = (float)packed->offset_y - padding;
    glyph->width = (float)packed->width + 2.0f * padding;
    glyph->height = (float)packed->height + 2.0f * padding;
    glyph->u0 =
        ((float)packed->atlas_x - padding) /
        (float)font->atlas_width;
    glyph->v0 =
        ((float)packed->atlas_y - padding) /
        (float)font->atlas_height;
    glyph->u1 =
        ((float)packed->atlas_x + (float)packed->width + padding) /
        (float)font->atlas_width;
    glyph->v1 =
        ((float)packed->atlas_y + (float)packed->height + padding) /
        (float)font->atlas_height;
    return BTBH_GPU_OK;
}

BTBHGPUTexture *btbh_gpu_font_atlas(BTBHGPUFont *font) {
    return font != NULL ? font->atlas : NULL;
}

void btbh_gpu_font_destroy(BTBHGPUFont *font) {
    if (font == NULL) {
        return;
    }
    btbh_gpu_font_unlink(font);
    if (font->atlas != NULL) {
        btbh_gpu_texture_destroy(font->atlas);
    }
    free(font->glyphs);
    free(font->codepoints);
    free(font);
}

void btbh_gpu_window_release_fonts(BTBHGPUWindow *window) {
    if (window == NULL) {
        return;
    }
    BTBHGPUFont *font = window->font_head;
    window->font_head = NULL;
    while (font != NULL) {
        BTBHGPUFont *next = font->next;
        if (font->atlas != NULL) {
            btbh_gpu_texture_destroy(font->atlas);
            font->atlas = NULL;
        }
        font->owner = NULL;
        font->previous = NULL;
        font->next = NULL;
        font = next;
    }
}
