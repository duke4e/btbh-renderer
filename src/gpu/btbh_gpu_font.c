#include "btbh_gpu_internal.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    stbtt_packedchar *glyphs;
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

static bool btbh_gpu_font_pack(
    const uint8_t *font_bytes,
    uint32_t base_size,
    const int *codepoints,
    uint32_t glyph_count,
    uint32_t atlas_size,
    uint8_t *bitmap,
    stbtt_packedchar *glyphs
) {
    stbtt_pack_context context;
    if (!stbtt_PackBegin(
            &context,
            bitmap,
            (int)atlas_size,
            (int)atlas_size,
            0,
            1,
            NULL
        )) {
        return false;
    }
    stbtt_PackSetOversampling(&context, 1u, 1u);
    stbtt_pack_range range;
    (void)memset(&range, 0, sizeof(range));
    range.font_size = (float)base_size;
    range.array_of_unicode_codepoints = (int *)codepoints;
    range.num_chars = (int)glyph_count;
    range.chardata_for_range = glyphs;
    const bool packed = stbtt_PackFontRanges(
        &context,
        font_bytes,
        0,
        &range,
        1
    ) != 0;
    stbtt_PackEnd(&context);
    return packed;
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

    int *packed_codepoints = malloc(
        (size_t)glyph_count * sizeof(*packed_codepoints)
    );
    uint32_t *stored_codepoints = malloc(
        (size_t)glyph_count * sizeof(*stored_codepoints)
    );
    stbtt_packedchar *packed_glyphs = calloc(
        glyph_count,
        sizeof(*packed_glyphs)
    );
    if (
        packed_codepoints == NULL ||
        stored_codepoints == NULL ||
        packed_glyphs == NULL
    ) {
        free(packed_glyphs);
        free(stored_codepoints);
        free(packed_codepoints);
        SDL_free(font_bytes);
        btbh_gpu_font_error(report, "Could not allocate font glyph storage");
        return NULL;
    }
    bool valid = true;
    for (uint32_t index = 0u; index < glyph_count; index += 1u) {
        if (glyphs[index] > (uint32_t)INT_MAX) {
            valid = false;
            break;
        }
        packed_codepoints[index] = (int)glyphs[index];
        stored_codepoints[index] = glyphs[index];
    }

    uint8_t *bitmap = NULL;
    uint32_t atlas_size = 512u;
    bool packed = false;
    while (valid && atlas_size <= 4096u && !packed) {
        const size_t atlas_bytes =
            (size_t)atlas_size * (size_t)atlas_size;
        bitmap = calloc(atlas_bytes, 1u);
        if (bitmap == NULL) {
            valid = false;
            break;
        }
        (void)memset(
            packed_glyphs,
            0,
            (size_t)glyph_count * sizeof(*packed_glyphs)
        );
        packed = btbh_gpu_font_pack(
            font_bytes,
            base_size,
            packed_codepoints,
            glyph_count,
            atlas_size,
            bitmap,
            packed_glyphs
        );
        if (!packed) {
            free(bitmap);
            bitmap = NULL;
            atlas_size *= 2u;
        }
    }
    if (!valid || !packed || bitmap == NULL) {
        free(bitmap);
        free(packed_glyphs);
        free(stored_codepoints);
        free(packed_codepoints);
        SDL_free(font_bytes);
        btbh_gpu_font_error(report, "Could not pack font glyph atlas");
        return NULL;
    }

    const size_t pixel_count =
        (size_t)atlas_size * (size_t)atlas_size;
    uint8_t *rgba = malloc(pixel_count * 4u);
    if (rgba == NULL) {
        free(bitmap);
        free(packed_glyphs);
        free(stored_codepoints);
        free(packed_codepoints);
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
        atlas_size,
        atlas_size,
        BTBH_GPU_TEXTURE_FILTER_LINEAR,
        BTBH_GPU_TEXTURE_WRAP_CLAMP,
        &texture_report
    );
    free(rgba);
    if (atlas == NULL) {
        free(packed_glyphs);
        free(stored_codepoints);
        free(packed_codepoints);
        SDL_free(font_bytes);
        btbh_gpu_font_error(report, texture_report.message);
        return NULL;
    }

    stbtt_fontinfo info;
    int ascent = 0;
    int descent = 0;
    int line_gap = 0;
    if (
        stbtt_InitFont(
            &info,
            font_bytes,
            stbtt_GetFontOffsetForIndex(font_bytes, 0)
        ) == 0
    ) {
        btbh_gpu_texture_destroy(atlas);
        free(packed_glyphs);
        free(stored_codepoints);
        free(packed_codepoints);
        SDL_free(font_bytes);
        btbh_gpu_font_error(report, "Could not initialize TTF font");
        return NULL;
    }
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &line_gap);
    const float metric_scale = stbtt_ScaleForPixelHeight(
        &info,
        (float)base_size
    );
    SDL_free(font_bytes);
    free(packed_codepoints);

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
    font->atlas_width = atlas_size;
    font->atlas_height = atlas_size;
    font->codepoints = stored_codepoints;
    font->glyphs = packed_glyphs;
    font->ascent = (float)ascent * metric_scale;
    font->descent = (float)descent * metric_scale;
    font->line_gap = (float)line_gap * metric_scale;

    report->result = BTBH_GPU_OK;
    report->base_size = base_size;
    report->glyph_count = glyph_count;
    report->atlas_width = atlas_size;
    report->atlas_height = atlas_size;
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
    const stbtt_packedchar *packed = &font->glyphs[glyph_index];
    (void)memset(glyph, 0, sizeof(*glyph));
    glyph->codepoint = font->codepoints[glyph_index];
    glyph->advance = packed->xadvance;
    glyph->offset_x = packed->xoff;
    glyph->offset_y = packed->yoff;
    glyph->width = packed->xoff2 - packed->xoff;
    glyph->height = packed->yoff2 - packed->yoff;
    glyph->u0 = (float)packed->x0 / (float)font->atlas_width;
    glyph->v0 = (float)packed->y0 / (float)font->atlas_height;
    glyph->u1 = (float)packed->x1 / (float)font->atlas_width;
    glyph->v1 = (float)packed->y1 / (float)font->atlas_height;
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
