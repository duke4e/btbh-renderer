/* Screen-space distortion on the CPU (RFC 0018, since 0.18.0): btbh_gpu_distortion.h, the reference the shaders
 * follow, and the public struct's layout. No GPU.
 * - BTBHGPUSceneDistortionSource: 64 bytes with RFC 0018's offsets; BTBHGPUSceneReport.distortion_sources_drawn in the
 *   byte after upscale_mode (the report keeps its size); the post flag's value.
 * - Each check of btbh_gpu_window_scene_set_distortion, with its message naming the source and the field; a NULL
 *   window refused.
 * - The profiles: 0 at their edges and C1 there, their peaks where RFC 0018 says, a ring's ramp by its centre.
 * - The fold clamp: at the clamped strength the Jacobian of p -> p - k d(p) stays positive at every pixel for every
 *   colour (chroma 0, 0.6 and 1), each shape facing and flat under a 45 degree camera, pushed and pulled, large flat
 *   shapes too; past it a flat disc folds.
 * - The haze: its noise within -1..1, repeating every 256 cells (the drift's wrap), a source's drift wrapped.
 * - Skipping and culling: zero radius, strength or ring width skipped; a source off to the side, behind the camera, or
 *   (scene views) on the other side of a view's half culled, one across the line kept; the bounds.
 * - The view rules: no fade with one view, the fade's ends, a read moved back to the margin. */
#undef NDEBUG

#include "btbh_gpu/btbh_gpu.h"
#include "btbh_gpu_distortion.h"

#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* The core's view planes (RFC 0014; btbh_gpu_internal.h), for the half-plane culling. */
void btbh_gpu_scene_view_planes(const float clip[3], uint32_t output_width, uint32_t output_height,
    uint32_t render_width, uint32_t render_height, float clip_plane[4], float uv_plane[4]);

_Static_assert(sizeof(BTBHGPUSceneDistortionSource) == 64u, "a distortion source is 64 bytes");
_Static_assert(offsetof(BTBHGPUSceneDistortionSource, position) == 0u, "position");
_Static_assert(offsetof(BTBHGPUSceneDistortionSource, radius) == 12u, "radius");
_Static_assert(offsetof(BTBHGPUSceneDistortionSource, end) == 16u, "end");
_Static_assert(offsetof(BTBHGPUSceneDistortionSource, width) == 28u, "width");
_Static_assert(offsetof(BTBHGPUSceneDistortionSource, strength) == 32u, "strength");
_Static_assert(offsetof(BTBHGPUSceneDistortionSource, chroma) == 36u, "chroma");
_Static_assert(offsetof(BTBHGPUSceneDistortionSource, haze_scale) == 40u, "haze_scale");
_Static_assert(offsetof(BTBHGPUSceneDistortionSource, haze_speed) == 44u, "haze_speed");
_Static_assert(offsetof(BTBHGPUSceneDistortionSource, shape) == 48u, "shape");
_Static_assert(offsetof(BTBHGPUSceneDistortionSource, mode) == 52u, "mode");
_Static_assert(offsetof(BTBHGPUSceneDistortionSource, flags) == 56u, "flags");
_Static_assert(offsetof(BTBHGPUSceneDistortionSource, reserved) == 60u, "reserved");
_Static_assert(offsetof(BTBHGPUSceneReport, distortion_sources_drawn) == offsetof(BTBHGPUSceneReport, upscale_mode) + 1u,
    "distortion_sources_drawn takes upscale_reserved[0]");
_Static_assert(offsetof(BTBHGPUSceneReport, upscale_reserved) == offsetof(BTBHGPUSceneReport, upscale_mode) + 2u,
    "two reserved bytes stay");
_Static_assert(sizeof(((BTBHGPUSceneReport *)0)->upscale_reserved) == 2u, "upscale_reserved[2]");
_Static_assert(BTBH_GPU_SCENE_POST_DISTORTION == 8u, "the post flag is bit 3");
_Static_assert(BTBH_GPU_SCENE_MAX_DISTORTION_SOURCES == 4u * BTBH_DISTORTION_SOURCES_PER_DRAW, "four draws a view");
_Static_assert(sizeof(BTBHDistortionRecord) == 64u, "a record is four float4");

/* The report's size as 0.17.0 had it (its message, the cascades' status and the upscale bytes unchanged). */
typedef struct OldReport {
    BTBHGPUResult result;
    uint32_t drawable_width;
    uint32_t drawable_height;
    uint32_t batch_count;
    uint32_t primitive_draw_count;
    uint32_t prepared_geometry_count;
    uint32_t instance_count;
    uint32_t transient_instance_count;
    uint32_t persistent_batch_count;
    double cpu_prepare_milliseconds;
    double cpu_upload_milliseconds;
    double cpu_shadow_encode_milliseconds;
    double cpu_swapchain_acquire_milliseconds;
    double cpu_target_ensure_milliseconds;
    double cpu_geometry_encode_milliseconds;
    double cpu_post_encode_milliseconds;
    BTBHGPUSceneRadianceCascadesStatus radiance_cascades;
    char message[256];
    uint8_t upscale_mode;
    uint8_t upscale_reserved[3];
} OldReport;
_Static_assert(sizeof(OldReport) == sizeof(BTBHGPUSceneReport), "the report keeps its size");

static BTBHGPUSceneDistortionSource ring(float radius, float width, float strength) {
    BTBHGPUSceneDistortionSource source;
    memset(&source, 0, sizeof(source));
    source.radius = radius;
    source.width = width;
    source.strength = strength;
    source.shape = BTBH_GPU_SCENE_DISTORTION_RING;
    return source;
}

static void expect_refused(const BTBHGPUSceneDistortionSource *source, const char *field) {
    char message[256];
    message[0] = '\0';
    const bool accepted = btbh_distortion_source_check(source, 7u, message, sizeof(message));
    if (accepted || strstr(message, "Distortion source 7") == NULL || strstr(message, field) == NULL) {
        fprintf(stderr, "expected a refusal naming %s, got %d '%s'\n", field, accepted ? 1 : 0, message);
    }
    assert(!accepted);
    assert(strstr(message, "Distortion source 7") != NULL);
    assert(strstr(message, field) != NULL);
}

static void check_refusals(void) {
    char message[256];
    BTBHGPUSceneDistortionSource source = ring(2.0f, 1.0f, 10.0f);
    assert(btbh_distortion_source_check(&source, 0u, message, sizeof(message)));
    const float bad[3] = {NAN, INFINITY, -INFINITY};
    for (int which = 0; which < 3; ++which) {
        source = ring(2.0f, 1.0f, 10.0f);
        source.position[1] = bad[which];
        expect_refused(&source, "position[1]");
        source = ring(2.0f, 1.0f, 10.0f);
        source.strength = bad[which];
        expect_refused(&source, "strength");
        source = ring(2.0f, 1.0f, 10.0f);
        source.haze_speed = bad[which];
        expect_refused(&source, "haze_speed");
    }
    source = ring(-1.0f, 1.0f, 10.0f);
    expect_refused(&source, "radius");
    source = ring(2.0f, -0.5f, 10.0f);
    expect_refused(&source, "width");
    source = ring(2.0f, 1.0f, 10.0f);
    source.mode = BTBH_GPU_SCENE_DISTORTION_HAZE;
    source.haze_scale = -1.0f;
    expect_refused(&source, "haze_scale");
    source = ring(2.0f, 1.0f, 10.0f);
    source.chroma = 1.5f;
    expect_refused(&source, "chroma");
    source.chroma = -0.1f;
    expect_refused(&source, "chroma");
    source = ring(2.0f, 1.0f, 10.0f);
    source.shape = 3u;
    expect_refused(&source, "shape");
    source = ring(2.0f, 1.0f, 10.0f);
    source.mode = 2u;
    expect_refused(&source, "mode");
    source = ring(2.0f, 1.0f, 10.0f);
    source.flags = 2u;
    expect_refused(&source, "flags");
    source = ring(2.0f, 1.0f, 10.0f);
    source.reserved = 1u;
    expect_refused(&source, "reserved");
    /* Fields a shape or a mode doesn't use. */
    source = ring(2.0f, 1.0f, 10.0f);
    source.end[2] = 1.0f;
    expect_refused(&source, "end");
    source.shape = BTBH_GPU_SCENE_DISTORTION_DISC;
    source.width = 0.0f;
    expect_refused(&source, "end");
    source = ring(2.0f, 1.0f, 10.0f);
    source.shape = BTBH_GPU_SCENE_DISTORTION_DISC;
    expect_refused(&source, "width");
    source.shape = BTBH_GPU_SCENE_DISTORTION_STRIP;
    expect_refused(&source, "width");
    source = ring(2.0f, 1.0f, 10.0f);
    source.haze_scale = 1.0f;
    expect_refused(&source, "haze_scale");
    source = ring(2.0f, 1.0f, 10.0f);
    source.haze_speed = 1.0f;
    expect_refused(&source, "haze_speed");
    /* Accepted: a strip with ends, a haze disc, a flat ring with chroma 1, zeroed sources. */
    memset(&source, 0, sizeof(source));
    assert(btbh_distortion_source_check(&source, 0u, message, sizeof(message)));
    assert(btbh_distortion_source_skipped(&source));
    source.shape = BTBH_GPU_SCENE_DISTORTION_STRIP;
    source.radius = 0.5f;
    source.end[0] = 3.0f;
    source.strength = -4.0f;
    assert(btbh_distortion_source_check(&source, 0u, message, sizeof(message)));
    memset(&source, 0, sizeof(source));
    source.shape = BTBH_GPU_SCENE_DISTORTION_DISC;
    source.mode = BTBH_GPU_SCENE_DISTORTION_HAZE;
    source.radius = 2.0f;
    source.strength = 2.0f;
    source.haze_scale = 1.5f;
    source.haze_speed = -0.8f;
    assert(btbh_distortion_source_check(&source, 0u, message, sizeof(message)));
    source = ring(2.0f, 1.0f, 10.0f);
    source.flags = BTBH_GPU_SCENE_DISTORTION_FLAT;
    source.chroma = 1.0f;
    assert(btbh_distortion_source_check(&source, 0u, message, sizeof(message)));
    /* A NULL window is refused before anything is read. */
    BTBHGPUSceneReport report;
    memset(&report, 0xA5, sizeof(report));
    assert(btbh_gpu_window_scene_set_distortion(NULL, &source, 1u, 0.0f, &report) == BTBH_GPU_INVALID_ARGUMENT);
    assert(report.result == BTBH_GPU_INVALID_ARGUMENT);
    assert(strstr(report.message, "window") != NULL);
    assert(btbh_gpu_window_scene_set_distortion(NULL, NULL, 0u, 0.0f, NULL) == BTBH_GPU_INVALID_ARGUMENT);
    /* Only result and message are written. */
    assert(report.upscale_mode == 0xA5u && report.drawable_width == 0xA5A5A5A5u);
}

/* The record of a source facing a camera looking down -z (right +x, up +y): local coordinates are world x and y. */
static BTBHDistortionRecord record_of(const BTBHGPUSceneDistortionSource *source, float strength_pixels) {
    BTBHDistortionRecord record;
    memset(&record, 0, sizeof(record));
    const float right[3] = {1.0f, 0.0f, 0.0f};
    const float up[3] = {0.0f, 1.0f, 0.0f};
    btbh_distortion_axes(source, right, up, record.axis_u, record.axis_v, &record.span);
    memcpy(record.origin, source->position, sizeof(record.origin));
    record.radius = source->radius;
    record.strength = strength_pixels;
    record.chroma = source->chroma;
    record.haze_scale = source->haze_scale;
    record.kind = (float)(source->shape + 3u * source->mode);
    return record;
}

/* The displacement's x at (x, 0) for a record whose local frame is the screen's at one pixel a unit (y down). */
static float push_at(const BTBHDistortionRecord *record, float x, float y) {
    const float local[2] = {x, y};
    const float dl_dx[2] = {1.0f, 0.0f};
    const float dl_dy[2] = {0.0f, -1.0f};
    float out[4];
    btbh_distortion_displacement(record, local, dl_dx, dl_dy, out);
    return out[0];
}

static void check_profiles(void) {
    /* A ring of radius 20, width 8, 10 pixels: nothing outside the band, the peak at its middle, outward. */
    BTBHGPUSceneDistortionSource source = ring(20.0f, 8.0f, 10.0f);
    BTBHDistortionRecord record = record_of(&source, 10.0f);
    assert(push_at(&record, 16.01f, 0.0f) > 0.0f);
    assert(push_at(&record, 15.99f, 0.0f) == 0.0f);
    assert(push_at(&record, 16.0f, 0.0f) == 0.0f);
    assert(push_at(&record, 24.0f, 0.0f) == 0.0f);
    assert(push_at(&record, 30.0f, 0.0f) == 0.0f);
    assert(fabsf(push_at(&record, 20.0f, 0.0f) - 10.0f) < 1.0e-4f);
    assert(fabsf(push_at(&record, -20.0f, 0.0f) + 10.0f) < 1.0e-4f);   /* outward on the other side */
    /* C1 at the edges: the slope a hair inside is ~0. */
    assert(fabsf(push_at(&record, 23.999f, 0.0f)) < 1.0e-3f);
    /* A newborn ring (radius under half its width) ramps to nothing at its centre. */
    source = ring(1.0f, 8.0f, 10.0f);
    record = record_of(&source, 10.0f);
    assert(fabsf(push_at(&record, 0.01f, 0.0f)) < 0.05f);
    /* A disc: 0 at the centre and the edge, its peak 1 at u = 0.447, negative pulls in. */
    memset(&source, 0, sizeof(source));
    source.shape = BTBH_GPU_SCENE_DISTORTION_DISC;
    source.radius = 10.0f;
    source.strength = 6.0f;
    record = record_of(&source, 6.0f);
    assert(fabsf(push_at(&record, 0.0f, 0.0f)) == 0.0f);
    assert(push_at(&record, 10.0f, 0.0f) == 0.0f);
    assert(fabsf(push_at(&record, 10.0f / sqrtf(5.0f), 0.0f) - 6.0f) < 1.0e-3f);
    float peak = 0.0f;
    for (int step = 1; step < 1000; ++step) {
        const float value = push_at(&record, (float)step * 0.01f, 0.0f);
        peak = value > peak ? value : peak;
    }
    assert(peak <= 6.0f + 1.0e-3f && peak > 5.99f);
    assert(fabsf(push_at(&record, 9.999f, 0.0f)) < 1.0e-3f);
    record.strength = -6.0f;
    assert(push_at(&record, 4.0f, 0.0f) < 0.0f);
    /* A strip from (0,0) to (30,0), half-width 5: across it, nothing past its caps. */
    memset(&source, 0, sizeof(source));
    source.shape = BTBH_GPU_SCENE_DISTORTION_STRIP;
    source.radius = 5.0f;
    source.end[0] = 30.0f;
    source.strength = 4.0f;
    record = record_of(&source, 4.0f);
    assert(fabsf(record.span - 30.0f) < 1.0e-5f);
    float local[2] = {15.0f, 2.236f};
    const float dl_dx[2] = {1.0f, 0.0f};
    const float dl_dy[2] = {0.0f, -1.0f};
    float out[4];
    btbh_distortion_displacement(&record, local, dl_dx, dl_dy, out);
    assert(fabsf(out[0]) < 1.0e-5f && fabsf(out[1] + 4.0f) < 1.0e-2f);   /* up the screen (+v), y down: -4 */
    local[0] = 36.0f;
    local[1] = 0.0f;
    btbh_distortion_displacement(&record, local, dl_dx, dl_dy, out);
    assert(out[3] == 0.0f);
    local[0] = 32.0f;
    btbh_distortion_displacement(&record, local, dl_dx, dl_dy, out);
    assert(out[0] > 0.0f && fabsf(out[1]) < 1.0e-5f);   /* past the cap, out along the segment */
    /* Chroma: the field's b is chroma times the length. */
    record.chroma = 0.5f;
    local[0] = 10.0f;
    local[1] = -2.0f;
    btbh_distortion_displacement(&record, local, dl_dx, dl_dy, out);
    assert(fabsf(out[2] - 0.5f * out[3]) < 1.0e-6f && out[3] > 0.0f);
}

/* A perspective camera looking down at 45 degrees on the origin from (0, 10, 10). */
static void camera(float view_projection[16], float inverse[16], float right[3], float up[3], uint32_t width,
    uint32_t height) {
    const float eye[3] = {0.0f, 10.0f, 10.0f};
    float forward[3] = {0.0f, -10.0f, -10.0f};
    (void)btbh_distortion_normalize3(forward, 0.0f);
    const float world_up[3] = {0.0f, 1.0f, 0.0f};
    float side[3];
    btbh_distortion_cross3(forward, world_up, side);
    (void)btbh_distortion_normalize3(side, 0.0f);
    float camera_up[3];
    btbh_distortion_cross3(side, forward, camera_up);
    const float view[16] = {
        side[0], camera_up[0], -forward[0], 0.0f,
        side[1], camera_up[1], -forward[1], 0.0f,
        side[2], camera_up[2], -forward[2], 0.0f,
        -btbh_distortion_dot3(eye, side), -btbh_distortion_dot3(eye, camera_up), btbh_distortion_dot3(eye, forward), 1.0f,
    };
    const float near_plane = 0.1f;
    const float far_plane = 100.0f;
    const float h = 1.0f / tanf(0.4f);
    const float aspect = (float)width / (float)height;
    const float r = far_plane / (near_plane - far_plane);
    const float projection[16] = {
        h / aspect, 0.0f, 0.0f, 0.0f,
        0.0f, h, 0.0f, 0.0f,
        0.0f, 0.0f, r, -1.0f,
        0.0f, 0.0f, r * near_plane, 0.0f,
    };
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += projection[k * 4 + row] * view[column * 4 + k];
            }
            view_projection[column * 4 + row] = sum;
        }
    }
    /* Its inverse: (P V)^-1 = V^-1 P^-1, by Gauss-Jordan on the product. */
    double a[4][8];
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            a[row][column] = view_projection[column * 4 + row];
            a[row][column + 4] = row == column ? 1.0 : 0.0;
        }
    }
    for (int pivot = 0; pivot < 4; ++pivot) {
        int best = pivot;
        for (int row = pivot + 1; row < 4; ++row) {
            if (fabs(a[row][pivot]) > fabs(a[best][pivot])) {
                best = row;
            }
        }
        for (int column = 0; column < 8; ++column) {
            const double swap = a[pivot][column];
            a[pivot][column] = a[best][column];
            a[best][column] = swap;
        }
        const double scale = a[pivot][pivot];
        for (int column = 0; column < 8; ++column) {
            a[pivot][column] /= scale;
        }
        for (int row = 0; row < 4; ++row) {
            if (row != pivot) {
                const double factor = a[row][pivot];
                for (int column = 0; column < 8; ++column) {
                    a[row][column] -= factor * a[pivot][column];
                }
            }
        }
    }
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            inverse[column * 4 + row] = (float)a[row][column + 4];
        }
    }
    memcpy(right, side, sizeof(side));
    memcpy(up, camera_up, sizeof(camera_up));
}

/* The displacement a record writes at a render pixel's position (x, y continuous; the field is half the size). */
static void render_displacement(const BTBHDistortionRecord *record, const float inverse[16], uint32_t field_width,
    uint32_t field_height, float x, float y, float out[2]) {
    float field[4];
    btbh_distortion_field_at(record, inverse, field_width, field_height, 0.5f * x, 0.5f * y, field);
    out[0] = field[0];
    out[1] = field[1];
}

typedef struct FoldCase {
    const char *name;
    uint32_t shape;
    float radius;
    float width;
    float length;
} FoldCase;

/* At the clamped strength, for every colour's scale k (red's 1 + 0.35 chroma, green's 1, blue's 1 - 0.35 chroma), the
   Jacobian of p -> p - k d(p) has a positive determinant at every render pixel of the shape: a 2D check by central
   differences across half a pixel, under a camera looking down at 45 degrees, for each shape facing and flat, pushed and
   pulled, with chroma 0, 0.6 (v2's largest, an orb's) and 1; large flat shapes too (their far side thinner). Past the
   clamp (2.5 times its strength, no chroma) a flat disc's tangential term would fold: the check would find it. */
static void check_fold_clamp(void) {
    const uint32_t width = 480u;
    const uint32_t height = 270u;
    float view_projection[16];
    float inverse[16];
    float right[3];
    float up[3];
    camera(view_projection, inverse, right, up, width, height);
    const uint32_t field_width = width / 2u;
    const uint32_t field_height = height / 2u;
    const FoldCase cases[7] = {
        {"ring", BTBH_GPU_SCENE_DISTORTION_RING, 2.0f, 1.0f, 0.0f},
        {"newborn ring", BTBH_GPU_SCENE_DISTORTION_RING, 0.3f, 1.4f, 0.0f},
        {"large ring", BTBH_GPU_SCENE_DISTORTION_RING, 6.0f, 2.0f, 0.0f},
        {"disc", BTBH_GPU_SCENE_DISTORTION_DISC, 1.5f, 0.0f, 0.0f},
        {"large disc", BTBH_GPU_SCENE_DISTORTION_DISC, 5.0f, 0.0f, 0.0f},
        {"strip", BTBH_GPU_SCENE_DISTORTION_STRIP, 0.8f, 0.0f, 3.0f},
        {"long strip", BTBH_GPU_SCENE_DISTORTION_STRIP, 1.2f, 0.0f, 9.0f},
    };
    const float chromas[3] = {0.0f, 0.6f, 1.0f};
    float lowest = 1.0e30f;
    uint64_t checked = 0u;
    for (uint32_t index = 0u; index < 7u; ++index) {
        for (uint32_t flat = 0u; flat < 2u; ++flat) {
            for (int sign = -1; sign <= 1; sign += 2) {
                for (uint32_t which = 0u; which < 3u; ++which) {
                    BTBHGPUSceneDistortionSource source;
                    memset(&source, 0, sizeof(source));
                    source.shape = cases[index].shape;
                    source.flags = flat != 0u ? BTBH_GPU_SCENE_DISTORTION_FLAT : 0u;
                    source.radius = cases[index].radius;
                    source.width = cases[index].width;
                    if (source.shape == BTBH_GPU_SCENE_DISTORTION_STRIP) {
                        source.position[0] = -0.5f * cases[index].length;
                        source.end[0] = 0.5f * cases[index].length;
                        source.end[2] = 0.3f * cases[index].length;
                    }
                    source.strength = (float)sign * 5000.0f;   /* far past the clamp */
                    source.chroma = chromas[which];
                    BTBHDistortionRecord record;
                    assert(btbh_distortion_pack(&source, view_projection, NULL, right, up, width, height, 0.0,
                        &record));
                    assert(record.strength * (float)sign > 0.0f);
                    /* The shape's pixels: a coarse scan for where it displaces, then every pixel of that box. */
                    int box[4] = {(int)width, (int)height, -1, -1};
                    for (uint32_t y = 0u; y < height; y += 2u) {
                        for (uint32_t x = 0u; x < width; x += 2u) {
                            float d[2];
                            render_displacement(&record, inverse, field_width, field_height, (float)x + 0.5f,
                                (float)y + 0.5f, d);
                            if (d[0] != 0.0f || d[1] != 0.0f) {
                                box[0] = (int)x < box[0] ? (int)x : box[0];
                                box[1] = (int)y < box[1] ? (int)y : box[1];
                                box[2] = (int)x > box[2] ? (int)x : box[2];
                                box[3] = (int)y > box[3] ? (int)y : box[3];
                            }
                        }
                    }
                    assert(box[2] >= box[0]);
                    const float scales[3] = {
                        1.0f + BTBH_DISTORTION_CHROMA_SPLIT * source.chroma, 1.0f,
                        1.0f - BTBH_DISTORTION_CHROMA_SPLIT * source.chroma,
                    };
                    float case_lowest = 1.0e30f;
                    for (int y = box[1] - 3; y <= box[3] + 3; ++y) {
                        for (int x = box[0] - 3; x <= box[2] + 3; ++x) {
                            const float px = (float)x + 0.5f;
                            const float py = (float)y + 0.5f;
                            float left[2];
                            float right_d[2];
                            float above[2];
                            float below[2];
                            render_displacement(&record, inverse, field_width, field_height, px - 0.5f, py, left);
                            render_displacement(&record, inverse, field_width, field_height, px + 0.5f, py, right_d);
                            render_displacement(&record, inverse, field_width, field_height, px, py - 0.5f, above);
                            render_displacement(&record, inverse, field_width, field_height, px, py + 0.5f, below);
                            const float dxx = right_d[0] - left[0];
                            const float dyx = right_d[1] - left[1];
                            const float dxy = below[0] - above[0];
                            const float dyy = below[1] - above[1];
                            for (int channel = 0; channel < 3; ++channel) {
                                const float k = scales[channel];
                                const float det = (1.0f - k * dxx) * (1.0f - k * dyy) - k * k * dxy * dyx;
                                case_lowest = det < case_lowest ? det : case_lowest;
                            }
                            checked += 1u;
                        }
                    }
                    if (case_lowest <= 0.0f) {
                        fprintf(stderr, "fold: %s %s %s chroma %.1f: determinant %.4f at strength %.3f px\n",
                            cases[index].name, flat != 0u ? "flat" : "facing", sign > 0 ? "push" : "pull",
                            (double)source.chroma, (double)case_lowest, (double)record.strength);
                    }
                    assert(case_lowest > 0.0f);
                    lowest = case_lowest < lowest ? case_lowest : lowest;
                }
            }
        }
    }
    /* 2.5 times the clamped strength on a flat disc folds: the check sees it. */
    BTBHGPUSceneDistortionSource disc;
    memset(&disc, 0, sizeof(disc));
    disc.shape = BTBH_GPU_SCENE_DISTORTION_DISC;
    disc.flags = BTBH_GPU_SCENE_DISTORTION_FLAT;
    disc.radius = 1.5f;
    disc.strength = 5000.0f;
    BTBHDistortionRecord record;
    assert(btbh_distortion_pack(&disc, view_projection, NULL, right, up, width, height, 0.0, &record));
    record.strength *= 2.5f;
    float folded = 1.0e30f;
    for (uint32_t y = 0u; y < height; ++y) {
        for (uint32_t x = 0u; x < width; ++x) {
            const float px = (float)x + 0.5f;
            const float py = (float)y + 0.5f;
            float left[2];
            float right_d[2];
            float above[2];
            float below[2];
            render_displacement(&record, inverse, field_width, field_height, px - 0.5f, py, left);
            render_displacement(&record, inverse, field_width, field_height, px + 0.5f, py, right_d);
            render_displacement(&record, inverse, field_width, field_height, px, py - 0.5f, above);
            render_displacement(&record, inverse, field_width, field_height, px, py + 0.5f, below);
            const float det = (1.0f - (right_d[0] - left[0])) * (1.0f - (below[1] - above[1])) -
                (below[0] - above[0]) * (right_d[1] - left[1]);
            folded = det < folded ? det : folded;
        }
    }
    printf("fold clamp: %llu pixel checks, lowest determinant %.3f; 2.5 times the clamp on a flat disc: %.3f\n",
        (unsigned long long)checked, (double)lowest, (double)folded);
    assert(folded < 0.0f);
}

static void check_haze(void) {
    float minimum = 1.0f;
    float maximum = -1.0f;
    for (int y = -40; y < 40; ++y) {
        for (int x = -40; x < 40; ++x) {
            float noise[2];
            btbh_distortion_haze_noise((float)x * 0.37f, (float)y * 0.29f, 3.5f, false, noise);
            for (int axis = 0; axis < 2; ++axis) {
                assert(noise[axis] >= -1.0f && noise[axis] <= 1.0f);
                minimum = fminf(minimum, noise[axis]);
                maximum = fmaxf(maximum, noise[axis]);
            }
            /* The drift wraps at the lattice's period: both octaves repeat (the second drifts three cells a cell). */
            float wrapped[2];
            btbh_distortion_haze_noise((float)x * 0.37f, (float)y * 0.29f, 3.5f + 256.0f, false, wrapped);
            assert(fabsf(wrapped[0] - noise[0]) < 2.0e-3f && fabsf(wrapped[1] - noise[1]) < 2.0e-3f);
            btbh_distortion_haze_noise((float)x * 0.37f, (float)y * 0.29f, 3.5f + 256.0f, true, wrapped);
            float plain[2];
            btbh_distortion_haze_noise((float)x * 0.37f, (float)y * 0.29f, 3.5f, true, plain);
            assert(fabsf(wrapped[0] - plain[0]) < 2.0e-3f && fabsf(wrapped[1] - plain[1]) < 2.0e-3f);
        }
    }
    assert(minimum < -0.4f && maximum > 0.4f);   /* it does vary */
    /* A source's drift: time times speed, wrapped into 0..256. */
    float view_projection[16];
    float inverse[16];
    float right[3];
    float up[3];
    camera(view_projection, inverse, right, up, 640u, 360u);
    BTBHGPUSceneDistortionSource source;
    memset(&source, 0, sizeof(source));
    source.shape = BTBH_GPU_SCENE_DISTORTION_DISC;
    source.mode = BTBH_GPU_SCENE_DISTORTION_HAZE;
    source.radius = 2.0f;
    source.strength = -3.0f;
    source.haze_speed = 0.75f;
    BTBHDistortionRecord record;
    assert(btbh_distortion_pack(&source, view_projection, NULL, right, up, 640u, 360u, 1000.0, &record));
    assert(fabsf(record.haze_drift - (float)fmod(750.0, 256.0)) < 1.0e-4f);
    assert(record.haze_scale == 1.0f);                                  /* 0: one cell a unit */
    assert(fabsf(record.strength - 3.0f * 360.0f / 1080.0f) < 1.0e-5f); /* its size, in render pixels */
    source.haze_speed = -0.75f;
    assert(btbh_distortion_pack(&source, view_projection, NULL, right, up, 640u, 360u, 1000.0, &record));
    assert(record.haze_drift >= 0.0f && record.haze_drift < 256.0f);
    assert(fabsf(record.haze_drift - (float)(256.0 - fmod(750.0, 256.0))) < 1.0e-3f);
}

static void check_skip_and_cull(void) {
    float view_projection[16];
    float inverse[16];
    float right[3];
    float up[3];
    camera(view_projection, inverse, right, up, 640u, 360u);
    BTBHDistortionRecord record;
    BTBHGPUSceneDistortionSource source = ring(2.0f, 1.0f, 10.0f);
    assert(btbh_distortion_pack(&source, view_projection, NULL, right, up, 640u, 360u, 0.0, &record));
    assert(fabsf(record.kind - 0.0f) < 1.0e-6f && record.span == 1.0f);
    source.width = 0.0f;
    assert(!btbh_distortion_pack(&source, view_projection, NULL, right, up, 640u, 360u, 0.0, &record));
    source = ring(0.0f, 1.0f, 10.0f);
    assert(!btbh_distortion_pack(&source, view_projection, NULL, right, up, 640u, 360u, 0.0, &record));
    source = ring(2.0f, 1.0f, 0.0f);
    assert(!btbh_distortion_pack(&source, view_projection, NULL, right, up, 640u, 360u, 0.0, &record));
    /* Off to the side, and behind the camera. */
    source = ring(2.0f, 1.0f, 10.0f);
    source.position[0] = 1.0e5f;
    assert(!btbh_distortion_pack(&source, view_projection, NULL, right, up, 640u, 360u, 0.0, &record));
    source.position[0] = 0.0f;
    source.position[1] = 30.0f;
    source.position[2] = 30.0f;
    assert(!btbh_distortion_pack(&source, view_projection, NULL, right, up, 640u, 360u, 0.0, &record));
    /* A PUSH source whose centre is behind the camera (its ring reaching in front) isn't drawn; a haze one is. */
    source = ring(30.0f, 2.0f, 10.0f);
    source.position[1] = 12.0f;
    source.position[2] = 12.0f;
    assert(!btbh_distortion_pack(&source, view_projection, NULL, right, up, 640u, 360u, 0.0, &record));
    source.mode = BTBH_GPU_SCENE_DISTORTION_HAZE;
    assert(btbh_distortion_pack(&source, view_projection, NULL, right, up, 640u, 360u, 0.0, &record));
    /* A view's half (RFC 0014): the right half, x >= 320 of 640 (clip (1, 0, -320), as btbh_gpu_scene_view_planes
       makes it). A disc to the left of the line is culled, one to the right kept, one across the line kept. */
    {
        float clip_plane[4];
        float uv_plane[4];
        const float clip[3] = {1.0f, 0.0f, -320.0f};
        btbh_gpu_scene_view_planes(clip, 640u, 360u, 640u, 360u, clip_plane, uv_plane);
        BTBHGPUSceneDistortionSource disc;
        memset(&disc, 0, sizeof(disc));
        disc.shape = BTBH_GPU_SCENE_DISTORTION_DISC;
        disc.radius = 0.5f;
        disc.strength = 10.0f;
        disc.position[0] = -1.5f;
        assert(btbh_distortion_pack(&disc, view_projection, NULL, right, up, 640u, 360u, 0.0, &record));
        assert(!btbh_distortion_pack(&disc, view_projection, clip_plane, right, up, 640u, 360u, 0.0, &record));
        disc.position[0] = 1.5f;
        assert(btbh_distortion_pack(&disc, view_projection, clip_plane, right, up, 640u, 360u, 0.0, &record));
        disc.position[0] = -0.3f;
        assert(btbh_distortion_pack(&disc, view_projection, clip_plane, right, up, 640u, 360u, 0.0, &record));
    }
    /* Just outside the left edge, but its band reaches in. */
    source = ring(4.0f, 2.0f, 10.0f);
    source.position[0] = -12.0f;
    assert(btbh_distortion_pack(&source, view_projection, NULL, right, up, 640u, 360u, 0.0, &record));
    float centre[3];
    float bound = 0.0f;
    btbh_distortion_bounds(&source, centre, &bound);
    assert(bound == 5.0f);
    memset(&source, 0, sizeof(source));
    source.shape = BTBH_GPU_SCENE_DISTORTION_STRIP;
    source.radius = 1.0f;
    source.position[0] = -3.0f;
    source.end[0] = 5.0f;
    btbh_distortion_bounds(&source, centre, &bound);
    assert(centre[0] == 1.0f && bound == 5.0f);
    /* A flat shape's axes lie in x-z, v up the screen (away from this camera: -z). */
    source = ring(2.0f, 1.0f, 10.0f);
    source.flags = BTBH_GPU_SCENE_DISTORTION_FLAT;
    float axis_u[3];
    float axis_v[3];
    float span = 0.0f;
    btbh_distortion_axes(&source, right, up, axis_u, axis_v, &span);
    assert(axis_u[1] == 0.0f && axis_v[1] == 0.0f);
    assert(fabsf(axis_u[0] - 1.0f) < 1.0e-6f && axis_v[2] < -0.999f);
}

static void check_view_rules(void) {
    const float none[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    assert(btbh_distortion_view_fade(none, 0.5f, 0.5f, 1.0f / 320.0f, 1.0f / 180.0f) == 1.0f);
    float uv[2] = {0.1f, 0.2f};
    btbh_distortion_view_clamp(none, 1.0f / 320.0f, 1.0f / 180.0f, uv);
    assert(uv[0] == 0.1f && uv[1] == 0.2f);
    /* A vertical line at x = 160 of 320 (the view's half: x >= 160), in UV units: 320 u - 160 >= 0. */
    const float plane[4] = {0.0f, 320.0f, 0.0f, -160.0f};
    const float inverse_width = 1.0f / 320.0f;
    const float inverse_height = 1.0f / 180.0f;
    assert(btbh_distortion_view_fade(plane, (160.0f + 2.5f) / 320.0f, 0.5f, inverse_width, inverse_height) == 0.0f);
    assert(btbh_distortion_view_fade(plane, (160.0f + 8.0f) / 320.0f, 0.5f, inverse_width, inverse_height) == 1.0f);
    const float middle = btbh_distortion_view_fade(plane, (160.0f + 5.25f) / 320.0f, 0.5f, inverse_width,
        inverse_height);
    assert(fabsf(middle - 0.5f) < 1.0e-4f);
    uv[0] = 150.0f / 320.0f;
    uv[1] = 0.5f;
    btbh_distortion_view_clamp(plane, inverse_width, inverse_height, uv);
    assert(fabsf(uv[0] * 320.0f - 161.5f) < 1.0e-3f && uv[1] == 0.5f);
    uv[0] = 170.0f / 320.0f;
    btbh_distortion_view_clamp(plane, inverse_width, inverse_height, uv);
    assert(fabsf(uv[0] * 320.0f - 170.0f) < 1.0e-4f);
}

int main(void) {
    check_refusals();
    check_profiles();
    check_fold_clamp();
    check_haze();
    check_skip_and_cull();
    check_view_rules();
    printf("btbh_gpu_distortion_test: ok\n");
    return 0;
}
