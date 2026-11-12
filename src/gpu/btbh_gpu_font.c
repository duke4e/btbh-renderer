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

/* A kerning pair of a font from btbh_gpu_window_font_load_ex, in pixels at its size: the public
   pair, so btbh_gpu_font_kerning_pairs copies the font's list as it is. */
typedef BTBHGPUFontKerningPair BTBHGPUFontKern;

#define BTBH_GPU_FONT_KNOWN_FLAGS \
    ((unsigned int)BTBH_GPU_FONT_FLAG_R8_ATLAS | (unsigned int)BTBH_GPU_FONT_FLAG_SNAP_X_HEIGHT | \
        (unsigned int)BTBH_GPU_FONT_FLAG_SNAP_X_HEIGHT_DOWN)

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
    float x_height;
    float cap_height;
    /* btbh_gpu_window_font_load_ex only (NULL and 0 from btbh_gpu_window_font_load): each
       codepoint's glyph as btbh_gpu_font_glyph returns it, the kerning pairs sorted by (left,
       right), and the size asked for. */
    BTBHGPUFontGlyph *baked;
    BTBHGPUFontKern *kerning;
    uint32_t kerning_count;
    float pixel_size;
};

/* The top of codepoint's outline above the baseline in pixels at scale, or 0 if the font lacks the
   glyph or it has no outline. */
static float btbh_gpu_font_outline_top(
    const stbtt_fontinfo *info,
    int codepoint,
    float scale
) {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    if (
        stbtt_FindGlyphIndex(info, codepoint) <= 0 ||
        stbtt_GetCodepointBox(info, codepoint, &x0, &y0, &x1, &y1) == 0
    ) {
        return 0.0f;
    }
    return (float)y1 * scale;
}

/* btbh_gpu_window_font_load_ex's atlas: empty texels between two glyphs (and at the atlas edges),
   and how far each glyph's quad reaches past its glyph, in texels, so linear filtering at a
   fractional position isn't cut off at the quad's edge. The margin must stay below the padding,
   or a quad's filter would reach its neighbour. */
#define BTBH_GPU_FONT_EX_PADDING 2
#define BTBH_GPU_FONT_EX_MARGIN 1
#define BTBH_GPU_FONT_EX_MAX_ATLAS 8192u
#define BTBH_GPU_FONT_EX_MIN_ATLAS 64u
#define BTBH_GPU_FONT_EX_MAX_OVERSAMPLE 8u

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
    /* Read now: info points into font_bytes, which is freed before the font is filled in. */
    const float x_height = btbh_gpu_font_outline_top(&info, 'x', metric_scale);
    const float cap_height = btbh_gpu_font_outline_top(&info, 'H', metric_scale);

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
    font->x_height = x_height;
    font->cap_height = cap_height;

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

static void btbh_gpu_font_invalid(
    BTBHGPUFontReport *report,
    const char *message
) {
    report->result = BTBH_GPU_INVALID_ARGUMENT;
    (void)snprintf(report->message, sizeof(report->message), "%s", message);
}

/* Why desc can't be baked, or NULL if it can. */
static const char *btbh_gpu_font_desc_problem(const BTBHGPUFontDesc *desc) {
    if (desc == NULL) {
        return "BTBHGPUFontDesc is NULL";
    }
    if (
        !isfinite(desc->pixel_size) ||
        desc->pixel_size < 1.0f ||
        desc->pixel_size > 1024.0f
    ) {
        return "BTBHGPUFontDesc.pixel_size must be 1 to 1024";
    }
    if (
        desc->oversample_x > BTBH_GPU_FONT_EX_MAX_OVERSAMPLE ||
        desc->oversample_y > BTBH_GPU_FONT_EX_MAX_OVERSAMPLE
    ) {
        return "BTBHGPUFontDesc.oversample_x and oversample_y must be 0 to 8";
    }
    if (desc->snap_advances > 1u) {
        return "BTBHGPUFontDesc.snap_advances must be 0 or 1";
    }
    if (((unsigned int)desc->flags & ~BTBH_GPU_FONT_KNOWN_FLAGS) != 0u) {
        return "BTBHGPUFontDesc.flags has bits other than BTBH_GPU_FONT_FLAG_R8_ATLAS, "
            "BTBH_GPU_FONT_FLAG_SNAP_X_HEIGHT and BTBH_GPU_FONT_FLAG_SNAP_X_HEIGHT_DOWN";
    }
    if (
        (desc->flags & BTBH_GPU_FONT_FLAG_SNAP_X_HEIGHT) != 0u &&
        (desc->flags & BTBH_GPU_FONT_FLAG_SNAP_X_HEIGHT_DOWN) != 0u
    ) {
        return "BTBHGPUFontDesc.flags has both BTBH_GPU_FONT_FLAG_SNAP_X_HEIGHT and "
            "BTBH_GPU_FONT_FLAG_SNAP_X_HEIGHT_DOWN; set one";
    }
    if (
        desc->coverage_gamma != 0.0f &&
        (
            !isfinite(desc->coverage_gamma) ||
            desc->coverage_gamma < 0.25f ||
            desc->coverage_gamma > 4.0f
        )
    ) {
        return "BTBHGPUFontDesc.coverage_gamma must be 0 or 0.25 to 4";
    }
    return NULL;
}

static int btbh_gpu_font_kern_compare(const void *a, const void *b) {
    const BTBHGPUFontKern *left = a;
    const BTBHGPUFontKern *right = b;
    if (left->left != right->left) {
        return left->left < right->left ? -1 : 1;
    }
    if (left->right != right->right) {
        return left->right < right->right ? -1 : 1;
    }
    return 0;
}

/* The font's kerning between every two of the n codepoints it has, in pixels (whole ones when
   snap), sorted. stb_truetype reads GPOS pair adjustments when the font has them, else its kern
   table. False only when out of memory; a font with no kerning gives none. */
static bool btbh_gpu_font_ex_kerning(
    const stbtt_fontinfo *info,
    const int *codepoints,
    const int *glyph_indices,
    uint32_t count,
    float scale,
    bool snap,
    BTBHGPUFontKern **pairs,
    uint32_t *pair_count
) {
    *pairs = NULL;
    *pair_count = 0u;
    if (info->gpos == 0 && info->kern == 0) {
        return true;
    }
    BTBHGPUFontKern *list = NULL;
    uint32_t capacity = 0u;
    uint32_t used = 0u;
    for (uint32_t a = 0u; a < count; a += 1u) {
        for (uint32_t b = 0u; b < count; b += 1u) {
            const int units = stbtt_GetGlyphKernAdvance(
                info,
                glyph_indices[a],
                glyph_indices[b]
            );
            if (units == 0) {
                continue;
            }
            float advance = (float)units * scale;
            if (snap) {
                advance = roundf(advance);
            }
            if (advance == 0.0f) {
                continue;
            }
            if (used == capacity) {
                const uint32_t grown = capacity == 0u ? 256u : capacity * 2u;
                if (grown < capacity || (size_t)grown > SIZE_MAX / sizeof(*list)) {
                    free(list);
                    return false;
                }
                BTBHGPUFontKern *larger = realloc(list, (size_t)grown * sizeof(*list));
                if (larger == NULL) {
                    free(list);
                    return false;
                }
                list = larger;
                capacity = grown;
            }
            list[used].left = (uint32_t)codepoints[a];
            list[used].right = (uint32_t)codepoints[b];
            list[used].advance = advance;
            used += 1u;
        }
    }
    if (used > 1u) {
        qsort(list, used, sizeof(*list), btbh_gpu_font_kern_compare);
    }
    *pairs = list;
    *pair_count = used;
    return true;
}

/* What btbh_gpu_window_font_load_ex bakes, and its scratch: btbh_gpu_font_ex_bake fills it, the
   loader frees whatever is left in it. */
typedef struct BTBHGPUFontExBake {
    /* Kept by the font. rgba is the RGBA8 atlas, NULL with BTBH_GPU_FONT_FLAG_R8_ATLAS, whose
       atlas is bitmap itself. */
    uint32_t *codepoints;
    BTBHGPUFontGlyph *baked;
    BTBHGPUFontKern *kerning;
    uint32_t kerning_count;
    uint8_t *rgba;
    uint32_t atlas_width;
    uint32_t atlas_height;
    /* Scratch: the codepoints the font has (present), their glyph indices, where each is in
       glyphs, stb_truetype's packed glyphs and its one-channel atlas. */
    int *present;
    int *present_glyphs;
    uint32_t *present_index;
    stbtt_packedchar *packed;
    uint8_t *bitmap;
} BTBHGPUFontExBake;

static void btbh_gpu_font_ex_bake_free(BTBHGPUFontExBake *bake) {
    free(bake->codepoints);
    free(bake->baked);
    free(bake->kerning);
    free(bake->rgba);
    free(bake->present);
    free(bake->present_glyphs);
    free(bake->present_index);
    free(bake->packed);
    free(bake->bitmap);
    (void)memset(bake, 0, sizeof(*bake));
}

/* stbtt_PackFontRanges over the count glyphs, with a vertical scale of its own
   (BTBH_GPU_FONT_FLAG_SNAP_X_HEIGHT): stb_truetype's gather, pack and render steps, which take one
   scale for both axes, with scale_x across and scale_y down. Across (x0, x1, xoff, xoff2, xadvance)
   it gives what stb_truetype gives at scale_x. 1 when every glyph fitted, 0 if not, -1 out of
   memory. */
static int btbh_gpu_font_ex_pack_scaled(
    stbtt_pack_context *context,
    const stbtt_fontinfo *info,
    const int *glyph_indices,
    uint32_t count,
    float scale_x,
    float scale_y,
    stbtt_packedchar *packed
) {
    stbrp_rect *rects = calloc(count, sizeof(*rects));
    if (rects == NULL) {
        return -1;
    }
    const int over_x = (int)context->h_oversample;
    const int over_y = (int)context->v_oversample;
    const float raster_x = scale_x * (float)over_x;
    const float raster_y = scale_y * (float)over_y;
    for (uint32_t k = 0u; k < count; k += 1u) {
        int x0 = 0;
        int y0 = 0;
        int x1 = 0;
        int y1 = 0;
        stbtt_GetGlyphBitmapBoxSubpixel(
            info, glyph_indices[k], raster_x, raster_y, 0.0f, 0.0f, &x0, &y0, &x1, &y1);
        rects[k].w = (stbrp_coord)(x1 - x0 + context->padding + over_x - 1);
        rects[k].h = (stbrp_coord)(y1 - y0 + context->padding + over_y - 1);
    }
    stbtt_PackFontRangesPackRects(context, rects, (int)count);
    const float recip_x = 1.0f / (float)over_x;
    const float recip_y = 1.0f / (float)over_y;
    const float shift_x = stbtt__oversample_shift(over_x);
    const float shift_y = stbtt__oversample_shift(over_y);
    int fitted = 1;
    for (uint32_t k = 0u; k < count; k += 1u) {
        stbrp_rect *rect = &rects[k];
        if (rect->was_packed == 0) {
            fitted = 0;
            continue;
        }
        const int glyph = glyph_indices[k];
        rect->x += context->padding;
        rect->y += context->padding;
        rect->w -= context->padding;
        rect->h -= context->padding;
        int advance = 0;
        int left_bearing = 0;
        stbtt_GetGlyphHMetrics(info, glyph, &advance, &left_bearing);
        int x0 = 0;
        int y0 = 0;
        int x1 = 0;
        int y1 = 0;
        stbtt_GetGlyphBitmapBox(info, glyph, raster_x, raster_y, &x0, &y0, &x1, &y1);
        unsigned char *pixels =
            context->pixels + rect->x + rect->y * context->stride_in_bytes;
        stbtt_MakeGlyphBitmapSubpixel(
            info,
            pixels,
            rect->w - over_x + 1,
            rect->h - over_y + 1,
            context->stride_in_bytes,
            raster_x,
            raster_y,
            0.0f,
            0.0f,
            glyph
        );
        if (over_x > 1) {
            stbtt__h_prefilter(
                pixels, rect->w, rect->h, context->stride_in_bytes, (unsigned int)over_x);
        }
        if (over_y > 1) {
            stbtt__v_prefilter(
                pixels, rect->w, rect->h, context->stride_in_bytes, (unsigned int)over_y);
        }
        stbtt_packedchar *out = &packed[k];
        out->x0 = (unsigned short)rect->x;
        out->y0 = (unsigned short)rect->y;
        out->x1 = (unsigned short)(rect->x + rect->w);
        out->y1 = (unsigned short)(rect->y + rect->h);
        out->xadvance = scale_x * (float)advance;
        out->xoff = (float)x0 * recip_x + shift_x;
        out->yoff = (float)y0 * recip_y + shift_y;
        out->xoff2 = (float)(x0 + rect->w) * recip_x + shift_x;
        out->yoff2 = (float)(y0 + rect->h) * recip_y + shift_y;
    }
    free(rects);
    return fitted;
}

/* Packs the glyphs of glyphs the font has with stb_truetype's packer (oversampled and
   prefiltered), applies the coverage curve and lays out each glyph's quad and UVs; NULL, or why it
   failed. scale_y is the vertical scale (BTBH_GPU_FONT_FLAG_SNAP_X_HEIGHT), otherwise the scale for
   desc's pixel_size, as across. */
static const char *btbh_gpu_font_ex_bake(
    const stbtt_fontinfo *info,
    const uint8_t *font_bytes,
    const BTBHGPUFontDesc *desc,
    float scale_y,
    const uint32_t *glyphs,
    uint32_t glyph_count,
    BTBHGPUFontExBake *bake
) {
    const int oversample_x = desc->oversample_x > 1u ? (int)desc->oversample_x : 1;
    const int oversample_y = desc->oversample_y > 1u ? (int)desc->oversample_y : 1;
    const bool snap = desc->snap_advances != 0u;
    const bool one_channel = (desc->flags & BTBH_GPU_FONT_FLAG_R8_ATLAS) != 0u;
    const float coverage_gamma =
        desc->coverage_gamma != 0.0f ? desc->coverage_gamma : 1.0f;
    const float scale = stbtt_ScaleForPixelHeight(info, desc->pixel_size);
    const bool scaled_apart = scale_y != scale;

    bake->codepoints = malloc((size_t)glyph_count * sizeof(*bake->codepoints));
    bake->baked = calloc(glyph_count, sizeof(*bake->baked));
    bake->present = malloc((size_t)glyph_count * sizeof(*bake->present));
    bake->present_glyphs = malloc((size_t)glyph_count * sizeof(*bake->present_glyphs));
    bake->present_index = malloc((size_t)glyph_count * sizeof(*bake->present_index));
    bake->packed = calloc(glyph_count, sizeof(*bake->packed));
    if (
        bake->codepoints == NULL ||
        bake->baked == NULL ||
        bake->present == NULL ||
        bake->present_glyphs == NULL ||
        bake->present_index == NULL ||
        bake->packed == NULL
    ) {
        return "Could not allocate font glyph storage";
    }

    /* Only the codepoints the font has are packed; the rest keep an empty glyph, no advance. */
    uint32_t present_count = 0u;
    uint64_t area = 0u;
    for (uint32_t index = 0u; index < glyph_count; index += 1u) {
        bake->codepoints[index] = glyphs[index];
        bake->baked[index].codepoint = glyphs[index];
        const int glyph = stbtt_FindGlyphIndex(info, (int)glyphs[index]);
        if (glyph <= 0) {
            continue;
        }
        int x0 = 0;
        int y0 = 0;
        int x1 = 0;
        int y1 = 0;
        stbtt_GetGlyphBitmapBoxSubpixel(
            info,
            glyph,
            scale * (float)oversample_x,
            scale_y * (float)oversample_y,
            0.0f,
            0.0f,
            &x0,
            &y0,
            &x1,
            &y1
        );
        area +=
            (uint64_t)(x1 - x0 + BTBH_GPU_FONT_EX_PADDING + oversample_x - 1) *
            (uint64_t)(y1 - y0 + BTBH_GPU_FONT_EX_PADDING + oversample_y - 1);
        bake->present[present_count] = (int)glyphs[index];
        bake->present_glyphs[present_count] = glyph;
        bake->present_index[present_count] = index;
        present_count += 1u;
    }

    /* The smallest power-of-two atlas the glyphs should fit (stb_truetype's own row packer, in
       order, wastes some), doubled until they do. */
    const uint64_t wanted = area + area / 3u + 1u;
    uint32_t atlas_width = BTBH_GPU_FONT_EX_MIN_ATLAS;
    while (
        atlas_width < BTBH_GPU_FONT_EX_MAX_ATLAS &&
        (uint64_t)atlas_width * (uint64_t)atlas_width < wanted
    ) {
        atlas_width *= 2u;
    }
    uint32_t atlas_height =
        (uint64_t)atlas_width * (uint64_t)(atlas_width / 2u) >= wanted
        ? atlas_width / 2u
        : atlas_width;
    for (;;) {
        bake->bitmap = malloc((size_t)atlas_width * (size_t)atlas_height);
        if (bake->bitmap == NULL) {
            return "Could not allocate font atlas pixels";
        }
        int fitted = 1;
        if (present_count > 0u) {
            stbtt_pack_context context;
            if (
                stbtt_PackBegin(
                    &context,
                    bake->bitmap,
                    (int)atlas_width,
                    (int)atlas_height,
                    0,
                    BTBH_GPU_FONT_EX_PADDING,
                    NULL
                ) == 0
            ) {
                return "Could not allocate font atlas packer";
            }
            stbtt_PackSetOversampling(
                &context,
                (unsigned int)oversample_x,
                (unsigned int)oversample_y
            );
            if (scaled_apart) {
                fitted = btbh_gpu_font_ex_pack_scaled(
                    &context,
                    info,
                    bake->present_glyphs,
                    present_count,
                    scale,
                    scale_y,
                    bake->packed
                );
            } else {
                stbtt_pack_range range;
                (void)memset(&range, 0, sizeof(range));
                range.font_size = desc->pixel_size;
                range.array_of_unicode_codepoints = bake->present;
                range.num_chars = (int)present_count;
                range.chardata_for_range = bake->packed;
                fitted = stbtt_PackFontRanges(&context, font_bytes, 0, &range, 1);
            }
            stbtt_PackEnd(&context);
            if (fitted < 0) {
                return "Could not allocate font atlas packer";
            }
        } else {
            (void)memset(bake->bitmap, 0, (size_t)atlas_width * (size_t)atlas_height);
        }
        if (fitted != 0) {
            break;
        }
        free(bake->bitmap);
        bake->bitmap = NULL;
        if (atlas_height < atlas_width) {
            atlas_height *= 2u;
        } else if (atlas_width < BTBH_GPU_FONT_EX_MAX_ATLAS) {
            atlas_width *= 2u;
        } else {
            return "Could not pack the glyphs into an 8192 x 8192 atlas";
        }
    }
    bake->atlas_width = atlas_width;
    bake->atlas_height = atlas_height;

    /* Coverage contrast, once, here: the canvas shader stays the same. */
    const size_t pixel_count = (size_t)atlas_width * (size_t)atlas_height;
    if (coverage_gamma != 1.0f) {
        uint8_t curve[256];
        for (uint32_t level = 0u; level < 256u; level += 1u) {
            const float coverage = powf((float)level / 255.0f, 1.0f / coverage_gamma);
            curve[level] = (uint8_t)lroundf(fminf(fmaxf(coverage, 0.0f), 1.0f) * 255.0f);
        }
        for (size_t index = 0u; index < pixel_count; index += 1u) {
            bake->bitmap[index] = curve[bake->bitmap[index]];
        }
    }
    /* BTBH_GPU_FONT_FLAG_R8_ATLAS uploads the coverage as it is. */
    if (!one_channel) {
        bake->rgba = malloc(pixel_count * 4u);
        if (bake->rgba == NULL) {
            return "Could not allocate font atlas pixels";
        }
        for (size_t index = 0u; index < pixel_count; index += 1u) {
            bake->rgba[index * 4u] = 255u;
            bake->rgba[index * 4u + 1u] = 255u;
            bake->rgba[index * 4u + 2u] = 255u;
            bake->rgba[index * 4u + 3u] = bake->bitmap[index];
        }
    }

    /* Each glyph's quad: its packed rect (the oversampled glyph and its prefilter's spread), and a
       margin round it, in output pixels; UVs over the same texels, so a quad drawn width x height
       pixels takes oversample texels a pixel. */
    const float margin = (float)BTBH_GPU_FONT_EX_MARGIN;
    const float margin_x = margin / (float)oversample_x;
    const float margin_y = margin / (float)oversample_y;
    for (uint32_t k = 0u; k < present_count; k += 1u) {
        const stbtt_packedchar *source = &bake->packed[k];
        BTBHGPUFontGlyph *glyph = &bake->baked[bake->present_index[k]];
        glyph->advance = snap ? roundf(source->xadvance) : source->xadvance;
        glyph->offset_x = source->xoff - margin_x;
        glyph->offset_y = source->yoff - margin_y;
        glyph->width = (source->xoff2 - source->xoff) + 2.0f * margin_x;
        glyph->height = (source->yoff2 - source->yoff) + 2.0f * margin_y;
        glyph->u0 = ((float)source->x0 - margin) / (float)atlas_width;
        glyph->v0 = ((float)source->y0 - margin) / (float)atlas_height;
        glyph->u1 = ((float)source->x1 + margin) / (float)atlas_width;
        glyph->v1 = ((float)source->y1 + margin) / (float)atlas_height;
    }

    if (
        !btbh_gpu_font_ex_kerning(
            info,
            bake->present,
            bake->present_glyphs,
            present_count,
            scale,
            snap,
            &bake->kerning,
            &bake->kerning_count
        )
    ) {
        return "Could not allocate font kerning";
    }
    return NULL;
}

BTBHGPUFont *btbh_gpu_window_font_load_ex(
    BTBHGPUWindow *window,
    const char *path,
    const BTBHGPUFontDesc *desc,
    const uint32_t *glyphs,
    uint32_t glyph_count,
    BTBHGPUFontReport *report
) {
    if (report == NULL) {
        return NULL;
    }
    (void)memset(report, 0, sizeof(*report));
    if (
        window == NULL ||
        window->device == NULL ||
        window->device->gpu == NULL ||
        window->device->shutdown ||
        !window->claimed
    ) {
        btbh_gpu_font_invalid(report, "The window has no claimed GPU device");
        return NULL;
    }
    if (path == NULL) {
        btbh_gpu_font_invalid(report, "The font path is NULL");
        return NULL;
    }
    if (glyphs == NULL || glyph_count == 0u || glyph_count > (uint32_t)INT_MAX) {
        btbh_gpu_font_invalid(report, "No glyphs to bake");
        return NULL;
    }
    const char *problem = btbh_gpu_font_desc_problem(desc);
    if (problem != NULL) {
        btbh_gpu_font_invalid(report, problem);
        return NULL;
    }
    for (uint32_t index = 0u; index < glyph_count; index += 1u) {
        if (glyphs[index] > (uint32_t)INT_MAX) {
            btbh_gpu_font_invalid(report, "A glyph codepoint is out of range");
            return NULL;
        }
    }

    size_t font_byte_count = 0u;
    uint8_t *font_bytes = SDL_LoadFile(path, &font_byte_count);
    if (font_bytes == NULL || font_byte_count == 0u) {
        SDL_free(font_bytes);
        btbh_gpu_font_error(report, SDL_GetError());
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
        SDL_free(font_bytes);
        btbh_gpu_font_error(report, "Could not initialize TTF font");
        return NULL;
    }
    const float scale = stbtt_ScaleForPixelHeight(&info, desc->pixel_size);
    int ascent = 0;
    int descent = 0;
    int line_gap = 0;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &line_gap);
    /* Read now: info points into font_bytes, which is freed after the bake. */
    float x_height = btbh_gpu_font_outline_top(&info, 'x', scale);
    /* BTBH_GPU_FONT_FLAG_SNAP_X_HEIGHT: the vertical scale that puts the x-height on a whole pixel
       (at least one), and everything vertical at that scale. _DOWN rounds down, and leaves an x-height
       under a pixel as it is. */
    float scale_y = scale;
    const bool snap_down = (desc->flags & BTBH_GPU_FONT_FLAG_SNAP_X_HEIGHT_DOWN) != 0u;
    if (((desc->flags & BTBH_GPU_FONT_FLAG_SNAP_X_HEIGHT) != 0u || snap_down) && x_height > 0.0f) {
        const float snapped = snap_down ? floorf(x_height) : fmaxf(roundf(x_height), 1.0f);
        if (snapped >= 1.0f && snapped != x_height) {
            scale_y = scale * snapped / x_height;
            x_height = snapped;
        }
    }
    const float cap_height = btbh_gpu_font_outline_top(&info, 'H', scale_y);

    BTBHGPUFontExBake bake;
    (void)memset(&bake, 0, sizeof(bake));
    const char *failure = btbh_gpu_font_ex_bake(
        &info,
        font_bytes,
        desc,
        scale_y,
        glyphs,
        glyph_count,
        &bake
    );
    SDL_free(font_bytes);
    if (failure != NULL) {
        btbh_gpu_font_ex_bake_free(&bake);
        btbh_gpu_font_error(report, failure);
        return NULL;
    }

    BTBHGPUTextureReport texture_report;
    BTBHGPUTexture *atlas = bake.rgba != NULL
        ? btbh_gpu_window_texture_create_rgba8(
            window,
            bake.rgba,
            bake.atlas_width,
            bake.atlas_height,
            BTBH_GPU_TEXTURE_FILTER_LINEAR,  /* the prefilter assumes it */
            BTBH_GPU_TEXTURE_WRAP_CLAMP,
            &texture_report
        )
        : btbh_gpu_window_texture_create_r8(
            window,
            bake.bitmap,
            bake.atlas_width,
            bake.atlas_height,
            BTBH_GPU_TEXTURE_FILTER_LINEAR,
            BTBH_GPU_TEXTURE_WRAP_CLAMP,
            &texture_report
        );
    if (atlas == NULL) {
        btbh_gpu_font_ex_bake_free(&bake);
        btbh_gpu_font_error(report, texture_report.message);
        return NULL;
    }
    BTBHGPUFont *font = calloc(1, sizeof(*font));
    if (font == NULL) {
        btbh_gpu_texture_destroy(atlas);
        btbh_gpu_font_ex_bake_free(&bake);
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
    font->base_size = (uint32_t)lroundf(desc->pixel_size);
    font->glyph_count = glyph_count;
    font->atlas_width = bake.atlas_width;
    font->atlas_height = bake.atlas_height;
    font->codepoints = bake.codepoints;
    font->glyphs = NULL;
    font->ascent = (float)ascent * scale_y;
    font->descent = (float)descent * scale_y;
    font->line_gap = (float)line_gap * scale_y;
    font->x_height = x_height;
    font->cap_height = cap_height;
    font->baked = bake.baked;
    font->kerning = bake.kerning;
    font->kerning_count = bake.kerning_count;
    font->pixel_size = desc->pixel_size;
    bake.codepoints = NULL;
    bake.baked = NULL;
    bake.kerning = NULL;
    btbh_gpu_font_ex_bake_free(&bake);

    report->result = BTBH_GPU_OK;
    report->base_size = font->base_size;
    report->glyph_count = glyph_count;
    report->atlas_width = font->atlas_width;
    report->atlas_height = font->atlas_height;
    report->ascent = font->ascent;
    report->descent = font->descent;
    report->line_gap = font->line_gap;
    return font;
}

BTBHGPUResult btbh_gpu_font_metrics(
    const BTBHGPUFont *font,
    BTBHGPUFontMetrics *metrics
) {
    if (font == NULL || metrics == NULL) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    metrics->ascent = font->ascent;
    metrics->descent = font->descent;
    metrics->line_gap = font->line_gap;
    metrics->pixel_size =
        font->baked != NULL ? font->pixel_size : (float)font->base_size;
    metrics->x_height = font->x_height;
    metrics->cap_height = font->cap_height;
    return BTBH_GPU_OK;
}

float btbh_gpu_font_kerning(
    const BTBHGPUFont *font,
    uint32_t left,
    uint32_t right
) {
    if (font == NULL || font->kerning == NULL) {
        return 0.0f;
    }
    uint32_t low = 0u;
    uint32_t high = font->kerning_count;
    while (low < high) {
        const uint32_t middle = low + (high - low) / 2u;
        const BTBHGPUFontKern *pair = &font->kerning[middle];
        if (pair->left < left || (pair->left == left && pair->right < right)) {
            low = middle + 1u;
        } else {
            high = middle;
        }
    }
    if (
        low < font->kerning_count &&
        font->kerning[low].left == left &&
        font->kerning[low].right == right
    ) {
        return font->kerning[low].advance;
    }
    return 0.0f;
}

uint32_t btbh_gpu_font_kerning_pairs(
    const BTBHGPUFont *font,
    BTBHGPUFontKerningPair *pairs,
    uint32_t capacity
) {
    if (font == NULL || font->kerning == NULL) {
        return 0u;
    }
    if (pairs != NULL && capacity > 0u) {
        const uint32_t copied =
            capacity < font->kerning_count ? capacity : font->kerning_count;
        (void)memcpy(pairs, font->kerning, (size_t)copied * sizeof(*pairs));
    }
    return font->kerning_count;
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
    if (font->baked != NULL) {
        *glyph = font->baked[glyph_index];
        return BTBH_GPU_OK;
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
    free(font->baked);
    free(font->kerning);
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
