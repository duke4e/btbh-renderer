#ifndef BTBH_GPU_DISTORTION_H
#define BTBH_GPU_DISTORTION_H

/* Screen-space distortion on the CPU (RFC 0018, since 0.18.0): the reference the shaders (production_distortion.hlsli,
   production_distortion.vert and .frag, production_hdr_composite.frag's BTBH_DISTORTION blocks) follow, used by the
   renderer (a setter's checks, each view's culling, the clamps, the records the source pass draws) and by the tests
   (the field and the composite against the GPU's). Header only, no SDL. */

#include "btbh_gpu/btbh_gpu.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* strength is in pixels of a frame this many lines high. */
#define BTBH_DISTORTION_REFERENCE_LINES 1080.0f
/* The sources' summed displacement is clamped to this many pixels of a 1080-line frame. */
#define BTBH_DISTORTION_SUM_LIMIT 64.0f
/* A PUSH source's fold clamp keeps the Jacobian of p -> p - k d(p) positive over the whole shape for every colour
   (k = 1 + BTBH_DISTORTION_CHROMA_SPLIT * chroma, red's), at this share of the limit. With the shape's projection
   locally A (singular values a >= b, render pixels per world unit) and d = m(r) along r's gradient on screen, the
   Jacobian's determinant is det(A) (1 - k m'(r) / l)(1 - k (m(r) / r) l^3 / (a b)^2) for l = a b / |A^-T r^| in [b, a]:
   one-to-one while k m' < b and k (m / r) < b^2 / a everywhere (a pull: k (-m') < b). btbh_distortion_pack takes a, b
   over the shape (perspective: its thinnest place) and m's steepest slopes (btbh_distortion_profile_slopes). */
#define BTBH_DISTORTION_FOLD_MARGIN 0.9f
/* 1 / max(u (1 - u^2)^2), at u = 1 / sqrt(5): the disc's profile peaks at 1. */
#define BTBH_DISTORTION_DISC_PEAK 3.4938562f
/* Chroma: red displaced by 1 + this * chroma, blue by 1 - this * chroma (REFRACT's dispersion). */
#define BTBH_DISTORTION_CHROMA_SPLIT 0.35f
/* The reactive mask: fully reactive at this many render pixels of displacement (REFRACT's rule). */
#define BTBH_DISTORTION_REACTIVE_PIXELS 6.0f
/* The haze's lattice repeats every this many cells (its hash takes the low 8 bits), so the drift wraps there. */
#define BTBH_DISTORTION_HAZE_PERIOD 256.0
/* Scene views: the displacement fades to nothing within this many render pixels of a view's edge. */
#define BTBH_DISTORTION_VIEW_FADE_PIXELS 8.0f
/* The sources one draw of the source pass takes (its vertex block: 128 bytes and 64 a source, under SDL's 4 KiB). */
#define BTBH_DISTORTION_SOURCES_PER_DRAW 60u

/* One source as the source pass draws it in one view: production_distortion.vert's four float4. 64 bytes. */
typedef struct BTBHDistortionRecord {
    float origin[3];       /* world: position */
    float radius;          /* world units */
    float axis_u[3];       /* the shape's plane: unit vectors in the world (a strip: along its segment, then across) */
    float span;            /* RING: its width; STRIP: its segment's length; DISC: 0 */
    float axis_v[3];
    float strength;        /* render pixels: PUSH signed and clamped at the fold, HAZE its size */
    float chroma;
    float haze_scale;      /* cells per world unit (HAZE; 0 for PUSH) */
    float haze_drift;      /* cells, wrapped to the lattice's period (HAZE; 0 for PUSH) */
    float kind;            /* shape + 3 * mode */
} BTBHDistortionRecord;

static inline float btbh_distortion_dot3(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static inline void btbh_distortion_cross3(const float a[3], const float b[3], float out[3]) {
    const float x = a[1] * b[2] - a[2] * b[1];
    const float y = a[2] * b[0] - a[0] * b[2];
    const float z = a[0] * b[1] - a[1] * b[0];
    out[0] = x;
    out[1] = y;
    out[2] = z;
}

/* Normalises v in place; false (v left as it was) when it is shorter than `shortest`. */
static inline bool btbh_distortion_normalize3(float v[3], float shortest) {
    const float length = sqrtf(btbh_distortion_dot3(v, v));
    if (!(length > shortest)) {
        return false;
    }
    v[0] /= length;
    v[1] /= length;
    v[2] /= length;
    return true;
}

/* A setter's checks of one source (btbh_gpu_window_scene_set_distortion): true, or false with `message` naming the
   source and the field. */
static inline bool btbh_distortion_source_check(
    const BTBHGPUSceneDistortionSource *source,
    uint32_t index,
    char *message,
    size_t capacity
) {
    const struct {
        const char *name;
        float value;
    } floats[] = {
        {"position[0]", source->position[0]}, {"position[1]", source->position[1]},
        {"position[2]", source->position[2]}, {"radius", source->radius}, {"end[0]", source->end[0]},
        {"end[1]", source->end[1]}, {"end[2]", source->end[2]}, {"width", source->width},
        {"strength", source->strength}, {"chroma", source->chroma}, {"haze_scale", source->haze_scale},
        {"haze_speed", source->haze_speed},
    };
    for (size_t field = 0u; field < sizeof(floats) / sizeof(floats[0]); ++field) {
        if (!isfinite(floats[field].value)) {
            (void)snprintf(message, capacity, "Distortion source %u: %s is not finite", index, floats[field].name);
            return false;
        }
    }
    const char *problem = NULL;
    if (source->shape >= (uint32_t)BTBH_GPU_SCENE_DISTORTION_SHAPE_COUNT) {
        problem = "shape is unknown";
    } else if (source->mode >= (uint32_t)BTBH_GPU_SCENE_DISTORTION_MODE_COUNT) {
        problem = "mode is unknown";
    } else if ((source->flags & ~(uint32_t)BTBH_GPU_SCENE_DISTORTION_FLAT) != 0u) {
        problem = "flags has an unknown bit";
    } else if (source->reserved != 0u) {
        problem = "reserved is not 0";
    } else if (source->radius < 0.0f) {
        problem = "radius is below 0";
    } else if (source->width < 0.0f) {
        problem = "width is below 0";
    } else if (source->haze_scale < 0.0f) {
        problem = "haze_scale is below 0";
    } else if (source->chroma < 0.0f || source->chroma > 1.0f) {
        problem = "chroma is outside 0..1";
    } else if (source->shape != (uint32_t)BTBH_GPU_SCENE_DISTORTION_STRIP &&
               (source->end[0] != 0.0f || source->end[1] != 0.0f || source->end[2] != 0.0f)) {
        problem = "end is not 0 (a ring or a disc has none)";
    } else if (source->shape != (uint32_t)BTBH_GPU_SCENE_DISTORTION_RING && source->width != 0.0f) {
        problem = "width is not 0 (a disc or a strip has none)";
    } else if (source->mode == (uint32_t)BTBH_GPU_SCENE_DISTORTION_PUSH &&
               (source->haze_scale != 0.0f || source->haze_speed != 0.0f)) {
        problem = "haze_scale or haze_speed is not 0 (PUSH has no haze)";
    }
    if (problem != NULL) {
        (void)snprintf(message, capacity, "Distortion source %u: %s", index, problem);
        return false;
    }
    return true;
}

/* A source that draws nothing: its radius or strength 0, or a ring's width 0 (not drawn, not counted). */
static inline bool btbh_distortion_source_skipped(const BTBHGPUSceneDistortionSource *source) {
    return source->radius == 0.0f ||
        source->strength == 0.0f ||
        (source->shape == (uint32_t)BTBH_GPU_SCENE_DISTORTION_RING && source->width == 0.0f);
}

/* The shape's plane in one view (right and up: the view's camera): its origin (position), its unit axes, and span. */
static inline void btbh_distortion_axes(
    const BTBHGPUSceneDistortionSource *source,
    const float right[3],
    const float up[3],
    float axis_u[3],
    float axis_v[3],
    float *span
) {
    const bool flat = (source->flags & (uint32_t)BTBH_GPU_SCENE_DISTORTION_FLAT) != 0u;
    const float world_up[3] = {0.0f, 1.0f, 0.0f};
    float frame_u[3];
    float frame_v[3];
    if (flat) {
        /* u: the camera's right laid flat; v: across it, up the screen (the side the camera's up, or else its forward,
           points to on the ground). */
        frame_u[0] = right[0];
        frame_u[1] = 0.0f;
        frame_u[2] = right[2];
        if (!btbh_distortion_normalize3(frame_u, 1.0e-6f)) {
            frame_u[0] = 1.0f;
            frame_u[1] = 0.0f;
            frame_u[2] = 0.0f;
        }
        btbh_distortion_cross3(world_up, frame_u, frame_v);
        float toward[3] = {up[0], 0.0f, up[2]};
        if (btbh_distortion_dot3(toward, toward) < 1.0e-8f) {
            float forward[3];
            btbh_distortion_cross3(up, right, forward);
            toward[0] = forward[0];
            toward[2] = forward[2];
        }
        if (btbh_distortion_dot3(frame_v, toward) < 0.0f) {
            frame_v[0] = -frame_v[0];
            frame_v[2] = -frame_v[2];
        }
    } else {
        frame_u[0] = right[0];
        frame_u[1] = right[1];
        frame_u[2] = right[2];
        if (!btbh_distortion_normalize3(frame_u, 1.0e-12f)) {
            frame_u[0] = 1.0f;
            frame_u[1] = 0.0f;
            frame_u[2] = 0.0f;
        }
        const float along = btbh_distortion_dot3(up, frame_u);
        frame_v[0] = up[0] - along * frame_u[0];
        frame_v[1] = up[1] - along * frame_u[1];
        frame_v[2] = up[2] - along * frame_u[2];
        if (!btbh_distortion_normalize3(frame_v, 1.0e-12f)) {
            btbh_distortion_cross3(frame_u, world_up, frame_v);
            (void)btbh_distortion_normalize3(frame_v, 0.0f);
        }
    }
    memcpy(axis_u, frame_u, sizeof(frame_u));
    memcpy(axis_v, frame_v, sizeof(frame_v));
    *span = 0.0f;
    if (source->shape == (uint32_t)BTBH_GPU_SCENE_DISTORTION_RING) {
        *span = source->width;
    } else if (source->shape == (uint32_t)BTBH_GPU_SCENE_DISTORTION_STRIP) {
        float segment[3] = {
            source->end[0] - source->position[0],
            source->end[1] - source->position[1],
            source->end[2] - source->position[2],
        };
        const float length = sqrtf(btbh_distortion_dot3(segment, segment));
        if (!(length > 1.0e-6f)) {
            return;   /* a point: the frame's axes, a disc-like capsule */
        }
        segment[0] /= length;
        segment[1] /= length;
        segment[2] /= length;
        float across[3];
        if (flat) {
            btbh_distortion_cross3(world_up, segment, across);
        } else {
            float forward[3];
            btbh_distortion_cross3(up, right, forward);
            btbh_distortion_cross3(forward, segment, across);
        }
        if (!btbh_distortion_normalize3(across, 1.0e-6f)) {
            /* Along the view (or upright, flat): across in the frame's plane. */
            const float along = btbh_distortion_dot3(frame_v, segment);
            across[0] = frame_v[0] - along * segment[0];
            across[1] = frame_v[1] - along * segment[1];
            across[2] = frame_v[2] - along * segment[2];
            if (!btbh_distortion_normalize3(across, 1.0e-6f)) {
                memcpy(across, frame_u, sizeof(across));
            }
        }
        memcpy(axis_u, segment, sizeof(segment));
        memcpy(axis_v, across, sizeof(across));
        *span = length;
    }
}

/* The source's bounding sphere (world). */
static inline void btbh_distortion_bounds(const BTBHGPUSceneDistortionSource *source, float centre[3], float *radius) {
    centre[0] = source->position[0];
    centre[1] = source->position[1];
    centre[2] = source->position[2];
    if (source->shape == (uint32_t)BTBH_GPU_SCENE_DISTORTION_RING) {
        *radius = source->radius + 0.5f * source->width;
    } else if (source->shape == (uint32_t)BTBH_GPU_SCENE_DISTORTION_STRIP) {
        const float segment[3] = {
            source->end[0] - source->position[0],
            source->end[1] - source->position[1],
            source->end[2] - source->position[2],
        };
        centre[0] += 0.5f * segment[0];
        centre[1] += 0.5f * segment[1];
        centre[2] += 0.5f * segment[2];
        *radius = 0.5f * sqrtf(btbh_distortion_dot3(segment, segment)) + source->radius;
    } else {
        *radius = source->radius;
    }
}

/* Whether a sphere lies at least partly on the drawn side of a view's half (RFC 0014): clip_plane is its clip-space
   plane (btbh_gpu_scene_view_planes: a point is drawn where a x + b y + c w >= 0), a world plane through the camera;
   NULL or zero: the whole frame. */
static inline bool btbh_distortion_in_half(const float m[16], const float clip_plane[4], const float centre[3],
    float radius) {
    if (clip_plane == NULL || (clip_plane[0] == 0.0f && clip_plane[1] == 0.0f && clip_plane[2] == 0.0f)) {
        return true;
    }
    float plane[4];
    for (int column = 0; column < 4; ++column) {
        plane[column] = clip_plane[0] * m[column * 4 + 0] + clip_plane[1] * m[column * 4 + 1] +
            clip_plane[2] * m[column * 4 + 3];
    }
    const float length = sqrtf(plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2]);
    const float distance = plane[0] * centre[0] + plane[1] * centre[1] + plane[2] * centre[2] + plane[3];
    return distance >= -radius * length;
}

/* Whether a sphere meets the frustum of a view-projection (column-major; clip x, y in -w..w, z in 0..w). */
static inline bool btbh_distortion_in_frustum(const float m[16], const float centre[3], float radius) {
    float rows[4][4];
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            rows[row][column] = m[column * 4 + row];
        }
    }
    const float planes[6][4] = {
        {rows[3][0] + rows[0][0], rows[3][1] + rows[0][1], rows[3][2] + rows[0][2], rows[3][3] + rows[0][3]},
        {rows[3][0] - rows[0][0], rows[3][1] - rows[0][1], rows[3][2] - rows[0][2], rows[3][3] - rows[0][3]},
        {rows[3][0] + rows[1][0], rows[3][1] + rows[1][1], rows[3][2] + rows[1][2], rows[3][3] + rows[1][3]},
        {rows[3][0] - rows[1][0], rows[3][1] - rows[1][1], rows[3][2] - rows[1][2], rows[3][3] - rows[1][3]},
        {rows[2][0], rows[2][1], rows[2][2], rows[2][3]},
        {rows[3][0] - rows[2][0], rows[3][1] - rows[2][1], rows[3][2] - rows[2][2], rows[3][3] - rows[2][3]},
    };
    for (int plane = 0; plane < 6; ++plane) {
        const float length = sqrtf(
            planes[plane][0] * planes[plane][0] + planes[plane][1] * planes[plane][1] +
            planes[plane][2] * planes[plane][2]
        );
        const float distance = planes[plane][0] * centre[0] + planes[plane][1] * centre[1] +
            planes[plane][2] * centre[2] + planes[plane][3];
        if (distance < -radius * length) {
            return false;
        }
    }
    return true;
}

/* The plane's projection at a point: the most (a) and the fewest (b) render pixels a world unit of it spans on screen
   (A's singular values, A the derivative of the screen position by the plane's coordinates). False behind the camera. */
static inline bool btbh_distortion_plane_scales(
    const float m[16],
    const float point[3],
    const float axis_u[3],
    const float axis_v[3],
    uint32_t render_width,
    uint32_t render_height,
    float *a,
    float *b
) {
    float clip[4];
    float du[4];
    float dv[4];
    for (int row = 0; row < 4; ++row) {
        clip[row] = m[0 * 4 + row] * point[0] + m[1 * 4 + row] * point[1] + m[2 * 4 + row] * point[2] +
            m[3 * 4 + row];
        du[row] = m[0 * 4 + row] * axis_u[0] + m[1 * 4 + row] * axis_u[1] + m[2 * 4 + row] * axis_u[2];
        dv[row] = m[0 * 4 + row] * axis_v[0] + m[1 * 4 + row] * axis_v[1] + m[2 * 4 + row] * axis_v[2];
    }
    if (!(clip[3] > 1.0e-6f)) {
        return false;
    }
    const float w2 = clip[3] * clip[3];
    const float sx = 0.5f * (float)render_width / w2;
    const float sy = 0.5f * (float)render_height / w2;
    const float ux = (du[0] * clip[3] - clip[0] * du[3]) * sx;
    const float uy = (du[1] * clip[3] - clip[1] * du[3]) * sy;
    const float vx = (dv[0] * clip[3] - clip[0] * dv[3]) * sx;
    const float vy = (dv[1] * clip[3] - clip[1] * dv[3]) * sy;
    const float p = ux * ux + uy * uy;
    const float q = vx * vx + vy * vy;
    const float c = ux * vx + uy * vy;
    const float spread = sqrtf((p - q) * (p - q) + 4.0f * c * c);
    *a = sqrtf(0.5f * (p + q + spread));
    *b = *a > 0.0f ? fabsf(ux * vy - uy * vx) / *a : 0.0f;
    return *b > 0.0f;
}

/* A PUSH profile's steepest slopes, per world unit of r: the most m'(r) / strength rises (rise) and falls (fall), and the
   largest m(r) / (r strength) (ratio). A disc's and a strip's are 3.4939 / radius, 2.7951 / radius and 3.4939 / radius
   (at the centre and at u = 0.775); a ring's are sampled over its band (1.5396 / (width / 2) each way for a ring whose
   radius is past its width, more by its centre for a newborn one). */
static inline void btbh_distortion_profile_slopes(
    const BTBHGPUSceneDistortionSource *source,
    float *rise,
    float *fall,
    float *ratio
) {
    if (source->shape != (uint32_t)BTBH_GPU_SCENE_DISTORTION_RING) {
        *rise = BTBH_DISTORTION_DISC_PEAK / source->radius;
        *fall = 0.8f * BTBH_DISTORTION_DISC_PEAK / source->radius;
        *ratio = BTBH_DISTORTION_DISC_PEAK / source->radius;
        return;
    }
    const float half = 0.5f * source->width;
    const float inner = source->radius > half ? source->radius - half : 0.0f;
    const float outer = source->radius + half;
    *rise = 0.0f;
    *fall = 0.0f;
    *ratio = 0.0f;
    for (int sample = 0; sample < 64; ++sample) {
        const float r = inner + ((float)sample + 0.5f) / 64.0f * (outer - inner);
        const float x = (r - source->radius) / half;
        const float band = (1.0f - x * x) * (1.0f - x * x);
        const float band_slope = -4.0f * x * (1.0f - x * x) / half;
        float slope;
        float per_r;
        if (r < half) {
            slope = band / half + (r / half) * band_slope;
            per_r = band / half;
        } else {
            slope = band_slope;
            per_r = band / r;
        }
        *rise = slope > *rise ? slope : *rise;
        *fall = -slope > *fall ? -slope : *fall;
        *ratio = per_r > *ratio ? per_r : *ratio;
    }
}

/* One source's record in one view, or false where it draws nothing there: skipped, outside the view's frustum or its
   half (clip_plane, NULL or zero for the whole frame), or a PUSH source whose centre is behind the camera.
   view_projection: the matrix the view's geometry is drawn with; right, up: its camera's; time_seconds: the setter's
   clock. */
static inline bool btbh_distortion_pack(
    const BTBHGPUSceneDistortionSource *source,
    const float view_projection[16],
    const float clip_plane[4],
    const float right[3],
    const float up[3],
    uint32_t render_width,
    uint32_t render_height,
    double time_seconds,
    BTBHDistortionRecord *record
) {
    memset(record, 0, sizeof(*record));
    if (btbh_distortion_source_skipped(source)) {
        return false;
    }
    float centre[3];
    float bound = 0.0f;
    btbh_distortion_bounds(source, centre, &bound);
    if (!btbh_distortion_in_frustum(view_projection, centre, bound) ||
        !btbh_distortion_in_half(view_projection, clip_plane, centre, bound)) {
        return false;
    }
    btbh_distortion_axes(source, right, up, record->axis_u, record->axis_v, &record->span);
    memcpy(record->origin, source->position, sizeof(record->origin));
    record->radius = source->radius;
    float strength = source->strength * (float)render_height / BTBH_DISTORTION_REFERENCE_LINES;
    if (source->mode == (uint32_t)BTBH_GPU_SCENE_DISTORTION_PUSH) {
        /* The fold clamp (BTBH_DISTORTION_FOLD_MARGIN): the projection's scales at the shape's centre and over its
           quad's corners and edges (perspective makes the far side thinner), behind the camera skipped. */
        float scale_a = 0.0f;
        float scale_b = 0.0f;
        if (!btbh_distortion_plane_scales(view_projection, centre, record->axis_u, record->axis_v, render_width,
                render_height, &scale_a, &scale_b)) {
            return false;   /* its centre behind the camera */
        }
        float thinnest = scale_b;
        float tangential = scale_b * scale_b / scale_a;
        const float extent = source->shape == (uint32_t)BTBH_GPU_SCENE_DISTORTION_RING
            ? source->radius + 0.5f * source->width
            : source->radius;
        const float low[2] = {-extent, -extent};
        const float high[2] = {
            (source->shape == (uint32_t)BTBH_GPU_SCENE_DISTORTION_STRIP ? record->span : 0.0f) + extent, extent,
        };
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                const float u = low[0] + 0.5f * (float)column * (high[0] - low[0]);
                const float v = low[1] + 0.5f * (float)row * (high[1] - low[1]);
                const float point[3] = {
                    source->position[0] + record->axis_u[0] * u + record->axis_v[0] * v,
                    source->position[1] + record->axis_u[1] * u + record->axis_v[1] * v,
                    source->position[2] + record->axis_u[2] * u + record->axis_v[2] * v,
                };
                float a = 0.0f;
                float b = 0.0f;
                if (!btbh_distortion_plane_scales(view_projection, point, record->axis_u, record->axis_v, render_width,
                        render_height, &a, &b)) {
                    continue;
                }
                thinnest = b < thinnest ? b : thinnest;
                tangential = b * b / a < tangential ? b * b / a : tangential;
            }
        }
        float rise = 0.0f;
        float fall = 0.0f;
        float ratio = 0.0f;
        btbh_distortion_profile_slopes(source, &rise, &fall, &ratio);
        const float k = 1.0f + BTBH_DISTORTION_CHROMA_SPLIT * source->chroma;
        if (strength > 0.0f) {
            const float by_rise = thinnest / (k * rise);
            const float by_ratio = tangential / (k * ratio);
            const float limit = BTBH_DISTORTION_FOLD_MARGIN * (by_rise < by_ratio ? by_rise : by_ratio);
            strength = strength > limit ? limit : strength;
        } else {
            const float limit = BTBH_DISTORTION_FOLD_MARGIN * thinnest / (k * fall);
            strength = strength < -limit ? -limit : strength;
        }
    } else {
        strength = fabsf(strength);
        record->haze_scale = source->haze_scale > 0.0f ? source->haze_scale : 1.0f;
        double drift = fmod(time_seconds * (double)source->haze_speed, BTBH_DISTORTION_HAZE_PERIOD);
        if (drift < 0.0) {
            drift += BTBH_DISTORTION_HAZE_PERIOD;
        }
        record->haze_drift = (float)drift;
    }
    record->strength = strength;
    record->chroma = source->chroma;
    record->kind = (float)(source->shape + 3u * source->mode);
    return true;
}

/* The haze's lattice: an integer hash of a cell (its coordinates' low 8 bits) and a seed (production_distortion.hlsli's
   distortion_hash). */
static inline uint32_t btbh_distortion_hash(int32_t x, int32_t y, uint32_t seed) {
    uint32_t h = ((uint32_t)x & 255u) * 0x8DA6B343u ^ ((uint32_t)y & 255u) * 0xD8163841u ^ seed * 0xCB1AB31Fu;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

static inline float btbh_distortion_lattice(int32_t x, int32_t y, uint32_t seed) {
    return (float)(btbh_distortion_hash(x, y, seed) & 0xFFFFu) * (2.0f / 65535.0f) - 1.0f;
}

/* Value noise in -1..1 with a smoothstep between the lattice's values. */
static inline float btbh_distortion_value_noise(float x, float y, uint32_t seed) {
    const float fx = floorf(x);
    const float fy = floorf(y);
    const int32_t ix = (int32_t)fx;
    const int32_t iy = (int32_t)fy;
    const float tx = x - fx;
    const float ty = y - fy;
    const float wx = tx * tx * (3.0f - 2.0f * tx);
    const float wy = ty * ty * (3.0f - 2.0f * ty);
    const float a = btbh_distortion_lattice(ix, iy, seed);
    const float b = btbh_distortion_lattice(ix + 1, iy, seed);
    const float c = btbh_distortion_lattice(ix, iy + 1, seed);
    const float d = btbh_distortion_lattice(ix + 1, iy + 1, seed);
    const float top = a + (b - a) * wx;
    const float bottom = c + (d - c) * wx;
    return top + (bottom - top) * wy;
}

/* The haze's two noises at a point of the shape's plane (cells): the first octave drifts `drift` cells along the
   shape's v (a strip: its u), the second, at twice the frequency and turned by half a radian, three cells for each of
   the first's (so both repeat when the drift wraps at the lattice's period). */
static inline void btbh_distortion_haze_noise(float hx, float hy, float drift, bool along_u, float out[2]) {
    const float c = 0.87758256f;
    const float s = 0.47942554f;
    const float x1 = along_u ? hx - drift : hx;
    const float y1 = along_u ? hy : hy - drift;
    const float rx = 2.0f * (c * hx - s * hy);
    const float ry = 2.0f * (s * hx + c * hy);
    const float x2 = along_u ? rx - 3.0f * drift : rx;
    const float y2 = along_u ? ry : ry - 3.0f * drift;
    out[0] = (btbh_distortion_value_noise(x1, y1, 0u) + 0.5f * btbh_distortion_value_noise(x2, y2, 1u)) / 1.5f;
    out[1] = (btbh_distortion_value_noise(x1, y1, 2u) + 0.5f * btbh_distortion_value_noise(x2, y2, 3u)) / 1.5f;
}

/* A source's displacement at a point of its plane (production_distortion.frag): local, the point (world units from
   origin along axis_u and axis_v); dl_dx and dl_dy, local's change across one field pixel right and down. out: the
   displacement in render pixels (x right, y down: the pixel shows the scene that far back), its length times chroma,
   its length. */
static inline void btbh_distortion_displacement(
    const BTBHDistortionRecord *record,
    const float local[2],
    const float dl_dx[2],
    const float dl_dy[2],
    float out[4]
) {
    out[0] = 0.0f;
    out[1] = 0.0f;
    out[2] = 0.0f;
    out[3] = 0.0f;
    const uint32_t kind = (uint32_t)(record->kind + 0.5f);
    const uint32_t shape = kind % 3u;
    const uint32_t mode = kind / 3u;
    float q[2] = {local[0], local[1]};
    if (shape == (uint32_t)BTBH_GPU_SCENE_DISTORTION_STRIP) {
        const float along = local[0] < 0.0f ? 0.0f : (local[0] > record->span ? record->span : local[0]);
        q[0] -= along;
    }
    const float r = sqrtf(q[0] * q[0] + q[1] * q[1]);
    float push = 0.0f;
    float envelope = 0.0f;
    if (shape == (uint32_t)BTBH_GPU_SCENE_DISTORTION_RING) {
        const float half = 0.5f * record->span;
        const float x = (r - record->radius) / half;
        if (!(fabsf(x) < 1.0f)) {
            return;
        }
        const float band = (1.0f - x * x) * (1.0f - x * x);
        const float ramp = r / half < 1.0f ? r / half : 1.0f;
        push = band * ramp;
        envelope = push;
    } else {
        const float u = r / record->radius;
        if (!(u < 1.0f)) {
            return;
        }
        const float edge = (1.0f - u * u) * (1.0f - u * u);
        push = BTBH_DISTORTION_DISC_PEAK * u * edge;
        envelope = edge;
    }
    float d[2];
    if (mode == (uint32_t)BTBH_GPU_SCENE_DISTORTION_PUSH) {
        if (!(r > 1.0e-12f)) {
            return;
        }
        const float gx = (q[0] * dl_dx[0] + q[1] * dl_dx[1]) / r;
        const float gy = (q[0] * dl_dy[0] + q[1] * dl_dy[1]) / r;
        const float length = sqrtf(gx * gx + gy * gy);
        if (!(length > 1.0e-20f)) {
            return;
        }
        const float magnitude = record->strength * push / length;
        d[0] = gx * magnitude;
        d[1] = gy * magnitude;
    } else {
        float noise[2];
        btbh_distortion_haze_noise(
            local[0] * record->haze_scale,
            local[1] * record->haze_scale,
            record->haze_drift,
            shape == (uint32_t)BTBH_GPU_SCENE_DISTORTION_STRIP,
            noise
        );
        const float magnitude = record->strength * envelope;
        d[0] = magnitude * noise[0];
        d[1] = -magnitude * noise[1];
    }
    const float length = sqrtf(d[0] * d[0] + d[1] * d[1]);
    out[0] = d[0];
    out[1] = d[1];
    out[2] = record->chroma * length;
    out[3] = length;
}

/* Where a pixel of the field (x, y: its continuous coordinates, a texel's centre at +0.5; y down) sees the record's
   plane, in its local coordinates; false when the ray misses it. inverse_view_projection: the view's. */
static inline bool btbh_distortion_local_at(
    const BTBHDistortionRecord *record,
    const float inverse_view_projection[16],
    uint32_t field_width,
    uint32_t field_height,
    float x,
    float y,
    float local[2]
) {
    const float ndc_x = 2.0f * x / (float)field_width - 1.0f;
    const float ndc_y = 1.0f - 2.0f * y / (float)field_height;
    float points[2][3];
    for (int which = 0; which < 2; ++which) {
        const float depth = which == 0 ? 0.0f : 0.5f;
        float h[4];
        for (int row = 0; row < 4; ++row) {
            h[row] = inverse_view_projection[0 * 4 + row] * ndc_x + inverse_view_projection[1 * 4 + row] * ndc_y +
                inverse_view_projection[2 * 4 + row] * depth + inverse_view_projection[3 * 4 + row];
        }
        if (h[3] == 0.0f) {
            return false;
        }
        points[which][0] = h[0] / h[3];
        points[which][1] = h[1] / h[3];
        points[which][2] = h[2] / h[3];
    }
    float normal[3];
    btbh_distortion_cross3(record->axis_u, record->axis_v, normal);
    const float direction[3] = {
        points[1][0] - points[0][0], points[1][1] - points[0][1], points[1][2] - points[0][2],
    };
    const float facing = btbh_distortion_dot3(normal, direction);
    if (fabsf(facing) < 1.0e-12f) {
        return false;
    }
    const float to_origin[3] = {
        record->origin[0] - points[0][0], record->origin[1] - points[0][1], record->origin[2] - points[0][2],
    };
    const float t = btbh_distortion_dot3(normal, to_origin) / facing;
    const float hit[3] = {
        points[0][0] + t * direction[0] - record->origin[0],
        points[0][1] + t * direction[1] - record->origin[1],
        points[0][2] + t * direction[2] - record->origin[2],
    };
    local[0] = btbh_distortion_dot3(hit, record->axis_u);
    local[1] = btbh_distortion_dot3(hit, record->axis_v);
    return true;
}

/* The field one record writes at a field pixel (as btbh_distortion_local_at), its derivatives by central differences
   across one pixel. */
static inline void btbh_distortion_field_at(
    const BTBHDistortionRecord *record,
    const float inverse_view_projection[16],
    uint32_t field_width,
    uint32_t field_height,
    float x,
    float y,
    float out[4]
) {
    float centre[2];
    float left[2];
    float right[2];
    float above[2];
    float below[2];
    out[0] = 0.0f;
    out[1] = 0.0f;
    out[2] = 0.0f;
    out[3] = 0.0f;
    if (!btbh_distortion_local_at(record, inverse_view_projection, field_width, field_height, x, y, centre) ||
        !btbh_distortion_local_at(record, inverse_view_projection, field_width, field_height, x - 0.5f, y, left) ||
        !btbh_distortion_local_at(record, inverse_view_projection, field_width, field_height, x + 0.5f, y, right) ||
        !btbh_distortion_local_at(record, inverse_view_projection, field_width, field_height, x, y - 0.5f, above) ||
        !btbh_distortion_local_at(record, inverse_view_projection, field_width, field_height, x, y + 0.5f, below)) {
        return;
    }
    const float dl_dx[2] = {right[0] - left[0], right[1] - left[1]};
    const float dl_dy[2] = {below[0] - above[0], below[1] - above[1]};
    btbh_distortion_displacement(record, centre, dl_dx, dl_dy, out);
}

/* The composite's view rules (scene views): uv_plane is the view's half in UV units (yzw, as
   btbh_gpu_scene_view_planes gives it; all zero with one view). The fade of the displacement at uv by the view's
   edge, and a displaced read moved back to the reflections' margin inside it. */
static inline float btbh_distortion_view_fade(const float uv_plane[4], float u, float v, float inverse_width,
    float inverse_height) {
    if (uv_plane[1] == 0.0f && uv_plane[2] == 0.0f) {
        return 1.0f;
    }
    const float nx = uv_plane[1] * inverse_width;
    const float ny = uv_plane[2] * inverse_height;
    float scale = sqrtf(nx * nx + ny * ny);
    scale = scale > 1.0e-30f ? scale : 1.0e-30f;
    const float line_pixels = (uv_plane[1] * u + uv_plane[2] * v + uv_plane[3]) / scale;
    const float margin = 0.5f + 2.0f * (fabsf(nx) + fabsf(ny)) / scale;
    const float fade = (line_pixels - margin) / (BTBH_DISTORTION_VIEW_FADE_PIXELS - margin);
    return fade < 0.0f ? 0.0f : (fade > 1.0f ? 1.0f : fade);
}

static inline void btbh_distortion_view_clamp(const float uv_plane[4], float inverse_width, float inverse_height,
    float uv[2]) {
    if (uv_plane[1] == 0.0f && uv_plane[2] == 0.0f) {
        return;
    }
    const float nx = uv_plane[1] * inverse_width;
    const float ny = uv_plane[2] * inverse_height;
    float scale = sqrtf(nx * nx + ny * ny);
    scale = scale > 1.0e-30f ? scale : 1.0e-30f;
    const float line_pixels = (uv_plane[1] * uv[0] + uv_plane[2] * uv[1] + uv_plane[3]) / scale;
    const float margin = 0.5f + (fabsf(nx) + fabsf(ny)) / scale;
    if (line_pixels < margin) {
        const float push = margin - line_pixels;
        uv[0] += push * (nx / scale) * inverse_width;
        uv[1] += push * (ny / scale) * inverse_height;
    }
}

#endif
