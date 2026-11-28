/* HDR output (RFC 0017, since 0.17.0), offscreen: this PC's display is SDR, so the HDR path runs with
 * BTBH_GPU_HDR_SIMULATE, which keeps the swapchain SDR and encodes into a simulated one the test reads back.
 * - A plain window: SDR state and formats, the describe line, every refusal of btbh_gpu_window_set_hdr, AUTO and FORCE
 *   finding no HDR here (or, on an HDR display, taking it).
 * - Simulated HDR10 and scRGB windows (shown: the canvas needs a swapchain image), one scene with lit boxes and two
 *   emissive ones above the knee:
 *   - peak = paper white (P = 1): the HDR frame, read back as RGBA8, is the SDR frame within 1 code (SDR dithers);
 *   - the encode: every pixel of the simulated swapchain against the CPU reference applied to the RGBA16F frame (1 code
 *     of 10 bits for HDR10 with the dither off, 2e-3 relative for scRGB's halves), at P = 1 and P = 1000/203;
 *   - the highlight gain: with BTBH_GPU_HDR_DEBUG=heat the frame's alpha holds the scene's heat, and the P = 4.93
 *     frame over the P = 1 frame, in display light, is the CPU gain of that heat; a heat past the largest half (the
 *     LUT's alpha, exposure 1e6) gives P, no NaN through SMAA;
 *   - a change of nits while a frame is pending reaches the next frame (the pending one encodes with its own); the
 *     serial and hdr_changed for it; AUTO's re-check of a change SDL sends no event for;
 *   - an encode that can't be made: SDR at the next frame (SWITCH_FAILED), not retried until a switch;
 *   - the toggle: SDR and HDR ten times between frames with a prewarm after each switch: nothing made after the first
 *     round, every SDR frame the first one's to the byte;
 *   - a set_hdr that switches while a frame is pending is refused, a change of nits is taken;
 *   - Auto following the OS: the simulated display turns SDR and back, an SDL HDR event is pushed: the poll reports
 *     the change, the state reads pending, the next frame's start switches (the SDR frame the SDR one to the byte);
 *   - captures and copies: the frame-format copy exact, the copy into an SDR target the CPU fold within 1 code, the
 *     capture in RGBA16F; render_target_create_ex's flags;
 *   - the HDR tail with FXAA, SMAA, FSR 1, FSR 3, tilt-shift, the LUT and CAS: finite, under the tail's peak.
 * It needs a GPU and the production shaders; with no device it is skipped (77). */
#undef NDEBUG

#include "btbh_gpu/btbh_gpu.h"
#include "btbh_gpu_internal.h"
#include "btbh_gpu_hdr.h"

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_hints.h>

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SMOKE_SKIP 77
#define INSTANCE_FLOATS 24u
#define INSTANCE_COUNT 7u
#define WINDOW_WIDTH 160u
#define WINDOW_HEIGHT 96u

#if defined(__APPLE__)
#define SMOKE_SHADER_EXTENSION ".metallib"
#define SMOKE_SHADER_FORMAT BTBH_GPU_SHADER_FORMAT_METALLIB
#define SMOKE_BACKEND BTBH_GPU_BACKEND_METAL
#else
#define SMOKE_SHADER_EXTENSION ".spv"
#define SMOKE_SHADER_FORMAT BTBH_GPU_SHADER_FORMAT_SPIRV
#define SMOKE_BACKEND BTBH_GPU_BACKEND_VULKAN
#endif

static const char *const shader_names[14] = {
    "production_model.vert",
    "production_animated_model.vert",
    "production_shadow_fast.vert",
    "production_shadow_fast.frag",
    "production_shadow.vert",
    "production_animated_shadow.vert",
    "production_shadow.frag",
    "production_model.frag",
    "production_model_unshadowed.frag",
    "production_post.vert",
    "production_post.frag",
    "production_canvas.vert",
    "production_canvas.frag",
    "production_canvas_textured.frag",
};
static char shader_paths[14][1024];

typedef struct SmokeWindow {
    BTBHGPUWindow *window;
    BTBHGPUCustomMesh *cube;
    uint32_t width;
    uint32_t height;
} SmokeWindow;

static void multiply(const float a[16], const float b[16], float out[16]) {
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a[k * 4 + row] * b[column * 4 + k];
            }
            out[column * 4 + row] = sum;
        }
    }
}

static void normalize3(float v[3]) {
    const float length = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    v[0] /= length;
    v[1] /= length;
    v[2] /= length;
}

static void cross3(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static float dot3(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* A right-handed look-at with a perspective (ortho 0) or orthographic (ortho: half height) projection, depth 0..1. */
static void look_at(const float eye[3], const float centre[3], float ortho, float out[16], float right[3],
    float up_out[3]) {
    float forward[3] = { centre[0] - eye[0], centre[1] - eye[1], centre[2] - eye[2] };
    normalize3(forward);
    const float world_up[3] = { 0.0f, 1.0f, 0.0f };
    float side[3];
    cross3(forward, world_up, side);
    normalize3(side);
    float up[3];
    cross3(side, forward, up);
    float view[16] = {
        side[0], up[0], -forward[0], 0.0f,
        side[1], up[1], -forward[1], 0.0f,
        side[2], up[2], -forward[2], 0.0f,
        -dot3(side, eye), -dot3(up, eye), dot3(forward, eye), 1.0f,
    };
    float projection[16];
    memset(projection, 0, sizeof(projection));
    const float near_plane = 0.1f;
    const float far_plane = 60.0f;
    if (ortho > 0.0f) {
        projection[0] = 1.0f / ortho;
        projection[5] = 1.0f / ortho;
        projection[10] = -1.0f / (far_plane - near_plane);
        projection[14] = -near_plane / (far_plane - near_plane);
        projection[15] = 1.0f;
    } else {
        const float aspect = (float)WINDOW_WIDTH / (float)WINDOW_HEIGHT;
        const float f = 1.0f / tanf(0.5f);
        projection[0] = f / aspect;
        projection[5] = f;
        projection[10] = far_plane / (near_plane - far_plane);
        projection[11] = -1.0f;
        projection[14] = near_plane * far_plane / (near_plane - far_plane);
    }
    multiply(projection, view, out);
    if (right != NULL) {
        memcpy(right, side, sizeof(side));
        memcpy(up_out, up, sizeof(up));
    }
}

static BTBHGPUSceneLighting smoke_lighting(void) {
    BTBHGPUSceneLighting lighting;
    memset(&lighting, 0, sizeof(lighting));
    lighting.ambient_red = 1.0f;
    lighting.ambient_green = 1.0f;
    lighting.ambient_blue = 1.0f;
    lighting.ambient_energy = 0.3f;
    lighting.directional_energy = 2.5f;
    lighting.directional_red = 1.0f;
    lighting.directional_green = 0.95f;
    lighting.directional_blue = 0.9f;
    lighting.spot_range = 8.0f;
    lighting.spot_falloff = 1.0f;
    lighting.spot_inner_cutoff = 1.0f;
    lighting.spot_outer_cutoff = 0.0f;
    const float sun[3] = { 4.0f, 10.0f, 3.0f };
    const float centre[3] = { 0.0f, 0.0f, 0.0f };
    lighting.directional_x = -sun[0];
    lighting.directional_y = -sun[1];
    lighting.directional_z = -sun[2];
    look_at(sun, centre, 8.0f, lighting.directional_shadow_view_projection, NULL, NULL);
    lighting.directional_shadow_enabled = 1u;
    lighting.directional_shadow_softness = 1.0f;
    return lighting;
}

/* ACES, no anti-aliasing and no upscale (the render is the drawable size), no bloom: the frame is the output pass's. */
static BTBHGPUScenePost smoke_post(void) {
    BTBHGPUScenePost post;
    memset(&post, 0, sizeof(post));
    post.tonemap_white = 1.0f;
    post.exposure = 1.0f;
    post.contrast = 1.0f;
    post.saturation = 1.0f;
    post.blur_curve = 1.0f;
    post.tonemap_mode = BTBH_GPU_SCENE_TONEMAP_ACES;
    post.upscale_mode = BTBH_GPU_SCENE_UPSCALE_LINEAR;
    return post;
}

static BTBHGPUSceneGeometryBatch cube_batch(const SmokeWindow *smoke, uint32_t first, uint32_t count) {
    BTBHGPUSceneGeometryBatch batch;
    memset(&batch, 0, sizeof(batch));
    batch.kind = BTBH_GPU_SCENE_GEOMETRY_CUSTOM_MESH;
    batch.custom_mesh = smoke->cube;
    batch.material_slot = -1;
    batch.first_instance = first;
    batch.instance_count = count;
    batch.albedo_red = 255u;
    batch.albedo_green = 255u;
    batch.albedo_blue = 255u;
    batch.albedo_alpha = 255u;
    batch.uv_scale_x = 1.0f;
    batch.uv_scale_y = 1.0f;
    batch.roughness = 0.6f;
    batch.occlusion = 1.0f;
    batch.depth_compare = BTBH_GPU_SCENE_DEPTH_LESS;
    batch.cull_mode = BTBH_GPU_SCENE_CULL_BACK;
    batch.stencil_pass = BTBH_GPU_SCENE_STENCIL_REPLACE;
    batch.spot_shadow_visible = 1u;
    return batch;
}

/* The ground and four lit boxes; two unlit emissive boxes (an orange one at about 3 and a white one at 12). */
static uint32_t smoke_batches(const SmokeWindow *smoke, BTBHGPUSceneGeometryBatch out[2]) {
    out[0] = cube_batch(smoke, 0u, 5u);
    out[1] = cube_batch(smoke, 5u, 2u);
    out[1].unlit_enabled = 1u;
    return 2u;
}

static void put_instance(float out[INSTANCE_FLOATS], float x, float y, float z, float sx, float sy, float sz,
    float red, float green, float blue) {
    memset(out, 0, INSTANCE_FLOATS * sizeof(float));
    out[0] = sx;
    out[5] = sy;
    out[10] = sz;
    out[12] = x;
    out[13] = y;
    out[14] = z;
    out[15] = 1.0f;
    out[16] = red;
    out[17] = green;
    out[18] = blue;
    out[19] = 1.0f;
}

static void smoke_instances(float out[INSTANCE_COUNT][INSTANCE_FLOATS]) {
    put_instance(out[0], 0.0f, -0.05f, 0.0f, 10.0f, 0.1f, 10.0f, 0.55f, 0.6f, 0.5f);
    put_instance(out[1], -2.0f, 0.75f, -1.5f, 1.2f, 1.5f, 1.2f, 0.7f, 0.68f, 0.62f);
    put_instance(out[2], 2.2f, 1.0f, -2.0f, 1.0f, 2.0f, 1.4f, 0.9f, 0.88f, 0.85f);
    put_instance(out[3], -1.8f, 0.5f, 2.0f, 1.5f, 1.0f, 1.0f, 0.9f, 0.3f, 0.2f);
    put_instance(out[4], 1.4f, 0.4f, 1.6f, 0.8f, 0.8f, 0.8f, 0.2f, 0.5f, 0.9f);
    put_instance(out[5], 0.2f, 0.6f, 0.4f, 0.9f, 0.9f, 0.9f, 3.0f, 2.2f, 1.0f);
    put_instance(out[6], -0.6f, 0.5f, 1.0f, 0.6f, 0.8f, 0.6f, 12.0f, 12.0f, 12.0f);
}

static BTBHGPUCustomMesh *create_cube(BTBHGPUWindow *window) {
    static const float faces[6][3] = {
        { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 },
    };
    float vertices[24][24];
    uint32_t indices[36];
    memset(vertices, 0, sizeof(vertices));
    for (uint32_t face = 0u; face < 6u; ++face) {
        const float *n = faces[face];
        const float u[3] = { n[1], n[2], n[0] };
        float v[3];
        cross3(n, u, v);
        for (uint32_t corner = 0u; corner < 4u; ++corner) {
            const float su = (corner == 1u || corner == 2u) ? 0.5f : -0.5f;
            const float sv = corner >= 2u ? 0.5f : -0.5f;
            float *vertex = vertices[face * 4u + corner];
            for (int axis = 0; axis < 3; ++axis) {
                vertex[axis] = 0.5f * n[axis] + su * u[axis] + sv * v[axis];
                vertex[3 + axis] = n[axis];
                vertex[8 + axis] = u[axis];
            }
            vertex[6] = su + 0.5f;
            vertex[7] = sv + 0.5f;
            vertex[11] = 1.0f;
            for (int channel = 12; channel < 16; ++channel) {
                vertex[channel] = 1.0f;
            }
            vertex[20] = 1.0f;
        }
        const uint32_t base = face * 4u;
        const uint32_t wound[6] = { base, base + 2u, base + 1u, base, base + 3u, base + 2u };
        memcpy(&indices[face * 6u], wound, sizeof(wound));
    }
    BTBHGPUCustomMeshReport report;
    memset(&report, 0, sizeof(report));
    BTBHGPUCustomMesh *mesh = btbh_gpu_window_custom_mesh_create_ex(window, vertices, (uint32_t)sizeof(vertices), 24u,
        indices, 36u, 0u, &report);
    if (mesh == NULL) {
        fprintf(stderr, "cube: %s\n", report.message);
    }
    assert(mesh != NULL);
    return mesh;
}

static void set_env(const char *name, const char *value) {
    if (value == NULL) {
        (void)SDL_unsetenv_unsafe(name);
    } else {
        (void)SDL_setenv_unsafe(name, value, 1);
    }
}

/* A window (the HDR debug switches are read at its claim: set them before). */
static SmokeWindow open_window(BTBHGPUDevice *device, const char *title, bool hidden) {
    SmokeWindow smoke;
    memset(&smoke, 0, sizeof(smoke));
    const BTBHGPUWindowDescriptor descriptor = {
        .title = title,
        .width = WINDOW_WIDTH,
        .height = WINDOW_HEIGHT,
        .hidden = hidden ? 1u : 0u,
        .high_pixel_density = 0,
    };
    BTBHGPUWindowReport report;
    memset(&report, 0, sizeof(report));
    smoke.window = btbh_gpu_window_create(&descriptor, &report);
    assert(smoke.window != NULL);
    assert(btbh_gpu_window_claim(device, smoke.window, &report) == BTBH_GPU_OK);
    smoke.width = report.drawable_width;
    smoke.height = report.drawable_height;
    smoke.cube = create_cube(smoke.window);
    return smoke;
}

static BTBHGPUResult present_scene(SmokeWindow *smoke, const BTBHGPUScenePost *post, BTBHGPUSceneReport *report) {
    float view_projection[16];
    float right[3];
    float up[3];
    const float eye[3] = { 6.0f * sinf(0.6f), 3.5f, 6.0f * cosf(0.6f) };
    const float centre[3] = { 0.0f, 0.4f, 0.0f };
    look_at(eye, centre, 0.0f, view_projection, right, up);
    const BTBHGPUSceneLighting lighting = smoke_lighting();
    float instances[INSTANCE_COUNT][INSTANCE_FLOATS];
    smoke_instances(instances);
    BTBHGPUSceneGeometryBatch batches[2];
    const uint32_t batch_count = smoke_batches(smoke, batches);
    /* No upscale: the drawable size; FSR 1 from half of it; FSR 3 from two thirds (it needs 64 pixels or more). */
    uint32_t render_width = smoke->width;
    uint32_t render_height = smoke->height;
    if (post->upscale_mode == BTBH_GPU_SCENE_UPSCALE_FSR3) {
        render_width = smoke->width * 2u / 3u;
        render_height = smoke->height * 2u / 3u;
    } else if (post->upscale_mode != BTBH_GPU_SCENE_UPSCALE_LINEAR) {
        render_width = smoke->width / 2u;
        render_height = smoke->height / 2u;
    }
    return btbh_gpu_window_scene_present_instances(smoke->window, shader_paths[0], shader_paths[1], shader_paths[2],
        shader_paths[3], shader_paths[4], shader_paths[5], shader_paths[6], shader_paths[7], shader_paths[8],
        shader_paths[9], shader_paths[10], SMOKE_SHADER_FORMAT, render_width, render_height, view_projection, right, up,
        eye, &lighting, post, &instances[0][0], INSTANCE_COUNT, INSTANCE_COUNT, batches, batch_count, 30u, 50u, 80u,
        255u, report);
}

static BTBHGPUResult prewarm(SmokeWindow *smoke, const BTBHGPUScenePost *post) {
    const BTBHGPUSceneLighting lighting = smoke_lighting();
    BTBHGPUSceneGeometryBatch batches[2];
    const uint32_t batch_count = smoke_batches(smoke, batches);
    BTBHGPUSceneReport report;
    const BTBHGPUResult result = btbh_gpu_window_scene_prewarm(smoke->window, shader_paths[0], shader_paths[1],
        shader_paths[2], shader_paths[3], shader_paths[4], shader_paths[5], shader_paths[6], shader_paths[7],
        shader_paths[8], shader_paths[9], shader_paths[10], SMOKE_SHADER_FORMAT, smoke->width, smoke->height,
        &lighting, post, batches, batch_count, &report);
    if (result != BTBH_GPU_OK) {
        fprintf(stderr, "prewarm: %s\n", report.message);
    }
    return result;
}

static void canvas(SmokeWindow *smoke) {
    BTBHGPUCanvasPass pass;
    memset(&pass, 0, sizeof(pass));
    BTBHGPUCanvasReport report;
    const BTBHGPUResult result = btbh_gpu_window_canvas_present(smoke->window, shader_paths[11], shader_paths[12],
        shader_paths[13], SMOKE_SHADER_FORMAT, NULL, 0u, NULL, 0u, &pass, 1u, &report);
    if (result != BTBH_GPU_OK) {
        fprintf(stderr, "canvas: %s\n", report.message);
    }
    assert(result == BTBH_GPU_OK);
}

/* The upscaling the last frame ran (BTBHGPUSceneReport.upscale_mode). */
static uint8_t last_upscale_mode;

/* A frame: the scene, then the canvas with the presented frame kept for the read-backs. */
static void frame(SmokeWindow *smoke, const BTBHGPUScenePost *post) {
    BTBHGPUSceneReport report;
    const BTBHGPUResult result = present_scene(smoke, post, &report);
    last_upscale_mode = report.upscale_mode;
    if (result != BTBH_GPU_OK) {
        fprintf(stderr, "scene: %s\n", report.message);
    }
    assert(result == BTBH_GPU_OK);
    btbh_gpu_window_request_presented_frame_readback(smoke->window);
    canvas(smoke);
}

static uint8_t *read_rgba8(SmokeWindow *smoke) {
    const uint32_t size = smoke->width * smoke->height * 4u;
    uint8_t *pixels = malloc(size);
    assert(pixels != NULL);
    BTBHGPUCanvasReport report;
    assert(btbh_gpu_window_readback_presented_frame(smoke->window, pixels, size, &report) == BTBH_GPU_OK);
    assert(report.drawable_width == smoke->width && report.drawable_height == smoke->height);
    return pixels;
}

static float *read_frame(SmokeWindow *smoke) {
    const uint32_t count = smoke->width * smoke->height * 4u;
    uint16_t *halves = malloc(count * sizeof(uint16_t));
    float *values = malloc(count * sizeof(float));
    assert(halves != NULL && values != NULL);
    BTBHGPUCanvasReport report;
    assert(btbh_gpu_window_readback_presented_frame_rgba16f(
        smoke->window, halves, count * (uint32_t)sizeof(uint16_t), &report) == BTBH_GPU_OK);
    for (uint32_t index = 0u; index < count; ++index) {
        values[index] = btbh_hdr_half_to_float(halves[index]);
    }
    free(halves);
    return values;
}

/* The simulated swapchain as floats (RGB10A2 words unpacked, or halves). */
static float *read_output(SmokeWindow *smoke, uint32_t *bytes_per_pixel) {
    const uint32_t capacity = smoke->width * smoke->height * 8u;
    uint8_t *raw = malloc(capacity);
    float *values = malloc(smoke->width * smoke->height * 4u * sizeof(float));
    assert(raw != NULL && values != NULL);
    uint32_t width = 0u;
    uint32_t height = 0u;
    assert(btbh_gpu_window_readback_hdr_output(smoke->window, raw, capacity, &width, &height, bytes_per_pixel) ==
        BTBH_GPU_OK);
    assert(width == smoke->width && height == smoke->height);
    for (uint32_t pixel = 0u; pixel < width * height; ++pixel) {
        if (*bytes_per_pixel == 4u) {
            uint32_t word;
            memcpy(&word, raw + pixel * 4u, sizeof(word));
            values[pixel * 4u] = (float)(word & 0x3ffu) / 1023.0f;
            values[pixel * 4u + 1u] = (float)((word >> 10) & 0x3ffu) / 1023.0f;
            values[pixel * 4u + 2u] = (float)((word >> 20) & 0x3ffu) / 1023.0f;
            values[pixel * 4u + 3u] = (float)(word >> 30) / 3.0f;
        } else {
            uint16_t halves[4];
            memcpy(halves, raw + pixel * 8u, sizeof(halves));
            for (uint32_t channel = 0u; channel < 4u; ++channel) {
                values[pixel * 4u + channel] = btbh_hdr_half_to_float(halves[channel]);
            }
        }
    }
    free(raw);
    return values;
}

static BTBHGPUWindowHDRState set_hdr(SmokeWindow *smoke, uint8_t mode, float paper, float peak) {
    BTBHGPUWindowHDRSettings settings;
    memset(&settings, 0, sizeof(settings));
    settings.mode = mode;
    settings.paper_white_nits = paper;
    settings.peak_nits = peak;
    BTBHGPUWindowHDRState state;
    const BTBHGPUResult result = btbh_gpu_window_set_hdr(smoke->window, &settings, &state);
    if (result != BTBH_GPU_OK) {
        fprintf(stderr, "set_hdr: %s\n", state.message);
    }
    assert(result == BTBH_GPU_OK);
    return state;
}

/* The encode of every pixel against the CPU reference applied to the frame: HDR10 within 1 code of 10 bits (dither
   off), scRGB within 2e-3 relative (halves). */
static void check_encode(SmokeWindow *smoke, const BTBHGPUWindowHDRState *state, const char *what) {
    float *frame_values = read_frame(smoke);
    uint32_t bytes_per_pixel = 0u;
    float *output = read_output(smoke, &bytes_per_pixel);
    const uint32_t composition = state->composition;
    assert(bytes_per_pixel == (composition == BTBH_GPU_SWAPCHAIN_HDR10_ST2084 ? 4u : 8u));
    float largest_error = 0.0f;
    uint32_t above_peak = 0u;
    for (uint32_t pixel = 0u; pixel < smoke->width * smoke->height; ++pixel) {
        float expected[3];
        btbh_hdr_encode_pixel(&frame_values[pixel * 4u], composition, state->paper_white_nits,
            state->highlight_ratio, state->paper_white_nits / BTBH_HDR_SCRGB_NITS, expected);
        for (uint32_t channel = 0u; channel < 3u; ++channel) {
            const float got = output[pixel * 4u + channel];
            float error = fabsf(got - expected[channel]);
            if (composition == BTBH_GPU_SWAPCHAIN_HDR10_ST2084) {
                error *= 1023.0f;
            } else {
                error /= fmaxf(fabsf(expected[channel]), 1.0e-2f);
            }
            largest_error = fmaxf(largest_error, error);
        }
        const float largest_frame = fmaxf(frame_values[pixel * 4u], fmaxf(frame_values[pixel * 4u + 1u],
            frame_values[pixel * 4u + 2u]));
        if (largest_frame > powf(state->highlight_ratio, 1.0f / 2.2f) * 1.002f) {
            above_peak += 1u;
        }
    }
    printf("%s: encode error %.4f (%s), frame pixels above the peak %u\n", what, (double)largest_error,
        composition == BTBH_GPU_SWAPCHAIN_HDR10_ST2084 ? "10-bit codes" : "relative", above_peak);
    if (composition == BTBH_GPU_SWAPCHAIN_HDR10_ST2084) {
        assert(largest_error <= 1.0f);
    } else {
        assert(largest_error <= 2.0e-3f);
    }
    assert(above_peak == 0u);
    free(frame_values);
    free(output);
}

static void check_plain_window(BTBHGPUDevice *device) {
    SmokeWindow smoke = open_window(device, "HDR smoke: plain", true);
    BTBHGPUWindowHDRState state;
    /* The HDR probing (SDL's window properties and compositions, the display's name; the claim's too) keeps the host's
       SDL error. */
    (void)SDL_SetError("%s", "the host's own error");
    assert(btbh_gpu_window_hdr_state(smoke.window, &state) == BTBH_GPU_OK);
    char probe_line[BTBH_GPU_HDR_DESCRIBE_CAPACITY];
    (void)btbh_gpu_window_hdr_describe(smoke.window, probe_line, sizeof(probe_line));
    assert(strcmp(SDL_GetError(), "the host's own error") == 0);
    SDL_ClearError();
    assert(btbh_gpu_window_hdr_state(smoke.window, &state) == BTBH_GPU_OK);
    assert(state.serial >= 1u);
    assert(state.mode == BTBH_GPU_HDR_MODE_OFF && state.active == 0u);
    assert(state.composition == BTBH_GPU_SWAPCHAIN_SDR);
    assert(state.frame_format == state.swapchain_format && state.target_format == state.swapchain_format);
    assert(state.supported[0] == 1u);
    assert(state.highlight_ratio == 1.0f);
    assert(state.paper_white_nits == BTBH_GPU_HDR_DEFAULT_PAPER_WHITE_NITS);
    assert(state.fallback == BTBH_GPU_HDR_FALLBACK_NONE);
    char line[BTBH_GPU_HDR_DESCRIBE_CAPACITY];
    const uint32_t length = btbh_gpu_window_hdr_describe(smoke.window, line, sizeof(line));
    assert(length > 0u && length < sizeof(line) && strncmp(line, "BTBH_GPU_HDR event=claim", 24) == 0);
    assert(strstr(line, "metadata=0 (SDL 3.4.2 has no call)") != NULL);
    printf("plain: %s\n", line);
    char tiny[8];
    assert(btbh_gpu_window_hdr_describe(smoke.window, tiny, sizeof(tiny)) == 7u && strlen(tiny) == 7u);
    assert(btbh_gpu_window_hdr_describe(NULL, line, sizeof(line)) == 0u);
    assert(btbh_gpu_window_hdr_state(NULL, &state) == BTBH_GPU_INVALID_ARGUMENT);
    assert(btbh_gpu_window_hdr_state(smoke.window, NULL) == BTBH_GPU_INVALID_ARGUMENT);

    /* Refusals. */
    BTBHGPUWindowHDRSettings settings;
    memset(&settings, 0, sizeof(settings));
    assert(btbh_gpu_window_set_hdr(NULL, &settings, &state) == BTBH_GPU_INVALID_ARGUMENT);
    assert(btbh_gpu_window_set_hdr(smoke.window, NULL, &state) == BTBH_GPU_INVALID_ARGUMENT);
    assert(btbh_gpu_window_set_hdr(smoke.window, &settings, NULL) == BTBH_GPU_INVALID_ARGUMENT);
    settings.mode = 3u;
    assert(btbh_gpu_window_set_hdr(smoke.window, &settings, &state) == BTBH_GPU_INVALID_ARGUMENT);
    assert(strstr(state.message, "mode") != NULL);
    settings.mode = BTBH_GPU_HDR_MODE_AUTO;
    settings.composition = 1u;
    assert(btbh_gpu_window_set_hdr(smoke.window, &settings, &state) == BTBH_GPU_INVALID_ARGUMENT);
    settings.composition = 0u;
    settings.flags = 1u;
    assert(btbh_gpu_window_set_hdr(smoke.window, &settings, &state) == BTBH_GPU_INVALID_ARGUMENT);
    settings.flags = 0u;
    settings.reserved[4] = 1u;
    assert(btbh_gpu_window_set_hdr(smoke.window, &settings, &state) == BTBH_GPU_INVALID_ARGUMENT);
    settings.reserved[4] = 0u;
    settings.paper_white_nits = NAN;
    assert(btbh_gpu_window_set_hdr(smoke.window, &settings, &state) == BTBH_GPU_INVALID_ARGUMENT);
    settings.paper_white_nits = -1.0f;
    assert(btbh_gpu_window_set_hdr(smoke.window, &settings, &state) == BTBH_GPU_INVALID_ARGUMENT);
    settings.paper_white_nits = 0.0f;
    settings.peak_nits = INFINITY;
    assert(btbh_gpu_window_set_hdr(smoke.window, &settings, &state) == BTBH_GPU_INVALID_ARGUMENT);
    assert(btbh_gpu_window_hdr_state(smoke.window, &state) == BTBH_GPU_OK && state.mode == BTBH_GPU_HDR_MODE_OFF);

    /* AUTO and FORCE: HDR only where this machine's swapchain offers it. */
    const bool offered = state.supported[2] != 0u || state.supported[3] != 0u;
    state = set_hdr(&smoke, BTBH_GPU_HDR_MODE_AUTO, 0.0f, 0.0f);
    printf("plain AUTO: active %u available %u fallback %u (%s)\n", state.active, state.available, state.fallback,
        state.message);
    if (!offered) {
        assert(state.active == 0u && state.available == 0u);
        assert(state.fallback == BTBH_GPU_HDR_FALLBACK_UNSUPPORTED);
        state = set_hdr(&smoke, BTBH_GPU_HDR_MODE_FORCE, 0.0f, 0.0f);
        assert(state.active == 0u && state.fallback == BTBH_GPU_HDR_FALLBACK_UNSUPPORTED);
    }
    state = set_hdr(&smoke, BTBH_GPU_HDR_MODE_OFF, 0.0f, 0.0f);
    assert(state.active == 0u && state.fallback == BTBH_GPU_HDR_FALLBACK_NONE);
    /* Nits: defaults and clamps. */
    state = set_hdr(&smoke, BTBH_GPU_HDR_MODE_OFF, 20.0f, 50000.0f);
    assert(state.paper_white_nits == 80.0f && state.peak_nits == 10000.0f);
    state = set_hdr(&smoke, BTBH_GPU_HDR_MODE_OFF, 300.0f, 100.0f);
    assert(state.paper_white_nits == 300.0f && state.peak_nits == 300.0f);
    /* The snapshot reports the change once. */
    BTBHGPUPlatformSnapshot snapshot;
    assert(btbh_gpu_window_poll_events(smoke.window, &snapshot) == BTBH_GPU_OK && snapshot.hdr_changed == 1u);
    assert(btbh_gpu_window_poll_events(smoke.window, &snapshot) == BTBH_GPU_OK && snapshot.hdr_changed == 0u);
    /* Render targets: the flag's refusal; frame format in SDR is the target format. */
    BTBHGPUTextureReport texture_report;
    assert(btbh_gpu_window_render_target_create_ex(smoke.window, 8u, 8u, BTBH_GPU_TEXTURE_FILTER_LINEAR, 2u,
        &texture_report) == NULL && texture_report.result == BTBH_GPU_INVALID_ARGUMENT);
    BTBHGPUTexture *target = btbh_gpu_window_render_target_create_ex(smoke.window, 8u, 8u,
        BTBH_GPU_TEXTURE_FILTER_LINEAR, BTBH_GPU_RENDER_TARGET_FRAME_FORMAT, &texture_report);
    assert(target != NULL && target->format == smoke.window->sdr_format);
    btbh_gpu_texture_destroy(target);
    btbh_gpu_window_destroy(smoke.window);
}

/* P = 1 gives the SDR picture; the encode at P = 1 and 4.93; the gain against the CPU from the heat in alpha. */
static void check_simulated(BTBHGPUDevice *device, const char *simulate) {
    set_env("BTBH_GPU_HDR_SIMULATE", simulate);
    set_env("BTBH_GPU_HDR_DITHER", "0");
    set_env("BTBH_GPU_HDR_DEBUG", "heat");
    char title[64];
    (void)snprintf(title, sizeof(title), "HDR smoke: %s", simulate);
    SmokeWindow smoke = open_window(device, title, false);
    set_env("BTBH_GPU_HDR_SIMULATE", NULL);
    set_env("BTBH_GPU_HDR_DITHER", NULL);
    set_env("BTBH_GPU_HDR_DEBUG", NULL);
    const BTBHGPUScenePost post = smoke_post();

    frame(&smoke, &post);
    uint8_t *sdr = read_rgba8(&smoke);

    BTBHGPUWindowHDRState state = set_hdr(&smoke, BTBH_GPU_HDR_MODE_AUTO, 203.0f, 203.0f);
    assert(state.active == 1u && state.available == 1u);
    const bool hdr10 = strcmp(simulate, "hdr10") == 0;
    assert(state.composition == (hdr10 ? BTBH_GPU_SWAPCHAIN_HDR10_ST2084 : BTBH_GPU_SWAPCHAIN_HDR_EXTENDED_LINEAR));
    assert(state.swapchain_format ==
        (hdr10 ? BTBH_GPU_TEXTURE_FORMAT_RGB10A2_UNORM : BTBH_GPU_TEXTURE_FORMAT_RGBA16_FLOAT));
    assert(state.frame_format == BTBH_GPU_TEXTURE_FORMAT_RGBA16_FLOAT);
    assert(state.target_format != BTBH_GPU_TEXTURE_FORMAT_RGBA16_FLOAT);
    assert(state.highlight_ratio == 1.0f);
    char line[BTBH_GPU_HDR_DESCRIBE_CAPACITY];
    (void)btbh_gpu_window_hdr_describe(smoke.window, line, sizeof(line));
    printf("%s: %s\n", simulate, line);

    frame(&smoke, &post);
    uint8_t *flat = read_rgba8(&smoke);
    uint32_t differing = 0u;
    int largest = 0;
    for (uint32_t index = 0u; index < smoke.width * smoke.height * 4u; ++index) {
        if (index % 4u == 3u) {
            continue;
        }
        const int difference = abs((int)sdr[index] - (int)flat[index]);
        largest = difference > largest ? difference : largest;
        differing += difference != 0 ? 1u : 0u;
    }
    printf("%s: P = 1 against SDR: %u channels differ, largest %d\n", simulate, differing, largest);
    assert(largest <= 1);
    check_encode(&smoke, &state, hdr10 ? "HDR10, P = 1" : "scRGB, P = 1");
    float *at_one = read_frame(&smoke);

    state = set_hdr(&smoke, BTBH_GPU_HDR_MODE_AUTO, 203.0f, 1000.0f);
    assert(state.active == 1u && fabsf(state.highlight_ratio - 1000.0f / 203.0f) < 1.0e-4f);
    frame(&smoke, &post);
    check_encode(&smoke, &state, hdr10 ? "HDR10, P = 4.93" : "scRGB, P = 4.93");
    float *lifted = read_frame(&smoke);
    const float knee = btbh_hdr_knee_input(BTBH_HDR_TONEMAP_ACES, 1.0f, BTBH_HDR_KNEE_OUTPUT);
    float largest_gain_error = 0.0f;
    uint32_t lifted_pixels = 0u;
    uint32_t below_knee_changed = 0u;
    for (uint32_t pixel = 0u; pixel < smoke.width * smoke.height; ++pixel) {
        const float heat = at_one[pixel * 4u + 3u];
        assert(fabsf(heat - lifted[pixel * 4u + 3u]) <= 1.0e-3f * fmaxf(heat, 1.0f));
        const float expected = btbh_hdr_gain(heat, state.highlight_ratio, knee, BTBH_HDR_SHOULDER_SCALE * knee);
        for (uint32_t channel = 0u; channel < 3u; ++channel) {
            const float base = at_one[pixel * 4u + channel];
            if (base < 0.05f) {
                continue;
            }
            const float measured = powf(lifted[pixel * 4u + channel] / base, 2.2f);
            largest_gain_error = fmaxf(largest_gain_error, fabsf(measured - expected) / expected);
            if (heat <= knee && lifted[pixel * 4u + channel] != base) {
                below_knee_changed += 1u;
            }
        }
        lifted_pixels += expected > 1.01f ? 1u : 0u;
    }
    printf("%s: gain against the CPU: largest relative error %.4f over %u lifted pixels; %u values at or below the "
        "knee changed\n", simulate, (double)largest_gain_error, lifted_pixels, below_knee_changed);
    assert(lifted_pixels > 50u);
    assert(largest_gain_error <= 0.01f);
    assert(below_knee_changed == 0u);
    free(at_one);
    free(lifted);
    free(sdr);
    free(flat);
    btbh_gpu_window_destroy(smoke.window);
}

/* SDR and HDR ten times between frames, each switch followed by a prewarm: nothing made after the first round. */
static void check_toggle(BTBHGPUDevice *device) {
    set_env("BTBH_GPU_HDR_SIMULATE", "hdr10");
    SmokeWindow smoke = open_window(device, "HDR smoke: toggle", false);
    set_env("BTBH_GPU_HDR_SIMULATE", NULL);
    const BTBHGPUScenePost post = smoke_post();
    uint8_t *first_sdr = NULL;
    uint64_t made_after_round = 0u;
    uint32_t first_switches = 0u;
    for (uint32_t round = 0u; round < 5u; ++round) {
        for (uint32_t hdr = 0u; hdr < 2u; ++hdr) {
            const BTBHGPUWindowHDRState state =
                set_hdr(&smoke, hdr != 0u ? BTBH_GPU_HDR_MODE_FORCE : BTBH_GPU_HDR_MODE_OFF, 0.0f, 0.0f);
            assert(state.active == hdr);
            if (round == 0u && hdr == 0u) {
                first_switches = state.switch_count;
            }
            BTBHGPUCreationStats before;
            btbh_gpu_creation_stats(&before);
            assert(prewarm(&smoke, &post) == BTBH_GPU_OK);
            frame(&smoke, &post);
            BTBHGPUCreationStats after;
            btbh_gpu_creation_stats(&after);
            if (round > 0u) {
                made_after_round += after.graphics_pipelines - before.graphics_pipelines;
            }
            if (hdr == 0u) {
                uint8_t *pixels = read_rgba8(&smoke);
                if (first_sdr == NULL) {
                    first_sdr = pixels;
                } else {
                    assert(memcmp(first_sdr, pixels, smoke.width * smoke.height * 4u) == 0);
                    free(pixels);
                }
            }
        }
    }
    BTBHGPUWindowHDRState state;
    assert(btbh_gpu_window_hdr_state(smoke.window, &state) == BTBH_GPU_OK);
    printf("toggle: %u switches, %llu pipelines made after the first round, every SDR frame the first one's\n",
        state.switch_count - first_switches, (unsigned long long)made_after_round);
    assert(state.switch_count - first_switches == 9u);
    assert(made_after_round == 0u);

    /* Pending: a switch is refused, nits are taken. */
    BTBHGPUSceneReport report;
    assert(present_scene(&smoke, &post, &report) == BTBH_GPU_OK);
    BTBHGPUWindowHDRSettings settings;
    memset(&settings, 0, sizeof(settings));
    settings.mode = BTBH_GPU_HDR_MODE_OFF;
    assert(btbh_gpu_window_set_hdr(smoke.window, &settings, &state) == BTBH_GPU_INVALID_ARGUMENT);
    assert(strstr(state.message, "pending") != NULL && state.active == 1u);
    settings.mode = BTBH_GPU_HDR_MODE_FORCE;
    settings.peak_nits = 600.0f;
    assert(btbh_gpu_window_set_hdr(smoke.window, &settings, &state) == BTBH_GPU_OK);
    assert(state.peak_nits == 600.0f && state.active == 1u);
    canvas(&smoke);
    free(first_sdr);
    btbh_gpu_window_destroy(smoke.window);
}

/* A change of nits while a frame is pending reaches the next frame: the pending one encodes (and lifts) with the
   numbers it started with. A change of nits takes a new serial and raises no hdr_changed; hdr_state with nothing
   changed keeps the serial. */
static void check_frame_nits(BTBHGPUDevice *device) {
    set_env("BTBH_GPU_HDR_SIMULATE", "hdr10");
    set_env("BTBH_GPU_HDR_DITHER", "0");
    SmokeWindow smoke = open_window(device, "HDR smoke: frame nits", false);
    set_env("BTBH_GPU_HDR_SIMULATE", NULL);
    set_env("BTBH_GPU_HDR_DITHER", NULL);
    const BTBHGPUScenePost post = smoke_post();
    const BTBHGPUWindowHDRState before = set_hdr(&smoke, BTBH_GPU_HDR_MODE_AUTO, 203.0f, 1000.0f);
    assert(before.active == 1u);
    frame(&smoke, &post);
    BTBHGPUPlatformSnapshot snapshot;
    assert(btbh_gpu_window_poll_events(smoke.window, &snapshot) == BTBH_GPU_OK && snapshot.hdr_changed == 1u);

    BTBHGPUSceneReport report;
    assert(present_scene(&smoke, &post, &report) == BTBH_GPU_OK);
    const BTBHGPUWindowHDRState after = set_hdr(&smoke, BTBH_GPU_HDR_MODE_AUTO, 300.0f, 600.0f);
    assert(after.paper_white_nits == 300.0f && after.peak_nits == 600.0f && after.serial != before.serial);
    btbh_gpu_window_request_presented_frame_readback(smoke.window);
    canvas(&smoke);
    check_encode(&smoke, &before, "nits changed while the frame was pending: the frame's own");
    float *values = read_frame(&smoke);
    const float new_peak = powf(after.highlight_ratio, 1.0f / 2.2f);
    uint32_t above_new_peak = 0u;
    for (uint32_t index = 0u; index < smoke.width * smoke.height * 4u; ++index) {
        above_new_peak += index % 4u != 3u && values[index] > new_peak * 1.01f ? 1u : 0u;
    }
    free(values);
    printf("frame nits: %u values above the new peak in the pending frame (its gain kept P = %.2f)\n",
        above_new_peak, (double)before.highlight_ratio);
    assert(above_new_peak > 20u);
    frame(&smoke, &post);
    check_encode(&smoke, &after, "the next frame: the new nits");

    assert(btbh_gpu_window_poll_events(smoke.window, &snapshot) == BTBH_GPU_OK && snapshot.hdr_changed == 0u);
    BTBHGPUWindowHDRState first;
    BTBHGPUWindowHDRState second;
    assert(btbh_gpu_window_hdr_state(smoke.window, &first) == BTBH_GPU_OK);
    assert(btbh_gpu_window_hdr_state(smoke.window, &second) == BTBH_GPU_OK);
    assert(first.serial == second.serial && memcmp(&first, &second, sizeof(first)) == 0);
    printf("frame nits: serial %u kept by two states, no hdr_changed for a change of nits\n", (unsigned)first.serial);
    btbh_gpu_window_destroy(smoke.window);
}

/* The encode pass can't be made (its shader is missing beside the others): the frame that found it out still shows,
   the window falls back to SDR at the next frame's start (SWITCH_FAILED, "encode", logged), nothing is tried again
   until a set_hdr switch, which tries once. */
static void check_encode_unavailable(BTBHGPUDevice *device) {
    const char *temp = SDL_getenv("TEMP");
    temp = temp != NULL ? temp : SDL_getenv("TMPDIR");
    temp = temp != NULL ? temp : "/tmp";
    char directory[1024];
    (void)snprintf(directory, sizeof(directory), "%s/btbh_hdr_smoke_no_encode", temp);
    (void)SDL_CreateDirectory(directory);
    SDL_PathInfo directory_info;
    assert(SDL_GetPathInfo(directory, &directory_info) && directory_info.type == SDL_PATHTYPE_DIRECTORY);
    int count = 0;
    /* (Every file beside the shaders, which the scene reads too: its BRDF table.) */
    char **names = SDL_GlobDirectory(BTBH_GPU_SMOKE_SHADER_DIR, "*", 0, &count);
    assert(names != NULL && count > 14);
    int copied = 0;
    for (int index = 0; index < count; ++index) {
        char source[1024];
        char target[1024];
        (void)snprintf(source, sizeof(source), "%s/%s", BTBH_GPU_SMOKE_SHADER_DIR, names[index]);
        SDL_PathInfo source_info;
        if (strcmp(names[index], "production_hdr_encode.frag" SMOKE_SHADER_EXTENSION) == 0 ||
            !SDL_GetPathInfo(source, &source_info) || source_info.type != SDL_PATHTYPE_FILE) {
            continue;
        }
        (void)snprintf(target, sizeof(target), "%s/%s", directory, names[index]);
        size_t size = 0u;
        void *bytes = SDL_LoadFile(source, &size);
        assert(bytes != NULL && SDL_SaveFile(target, bytes, size));
        SDL_free(bytes);
        copied += 1;
    }
    char saved_paths[14][1024];
    memcpy(saved_paths, shader_paths, sizeof(saved_paths));
    for (int index = 0; index < 14; ++index) {
        (void)snprintf(shader_paths[index], sizeof(shader_paths[index]), "%s/%s%s", directory, shader_names[index],
            SMOKE_SHADER_EXTENSION);
    }

    set_env("BTBH_GPU_HDR_SIMULATE", "hdr10");
    SmokeWindow smoke = open_window(device, "HDR smoke: no encode", false);
    set_env("BTBH_GPU_HDR_SIMULATE", NULL);
    const BTBHGPUScenePost post = smoke_post();
    BTBHGPUWindowHDRState state = set_hdr(&smoke, BTBH_GPU_HDR_MODE_AUTO, 203.0f, 1000.0f);
    assert(state.active == 1u);
    frame(&smoke, &post);
    assert(smoke.window->hdr_encode_failed);
    BTBHGPUPlatformSnapshot snapshot;
    assert(btbh_gpu_window_poll_events(smoke.window, &snapshot) == BTBH_GPU_OK);
    frame(&smoke, &post);
    assert(btbh_gpu_window_hdr_state(smoke.window, &state) == BTBH_GPU_OK);
    char line[BTBH_GPU_HDR_DESCRIBE_CAPACITY];
    (void)btbh_gpu_window_hdr_describe(smoke.window, line, sizeof(line));
    printf("no encode: %s\n", line);
    assert(state.active == 0u && state.fallback == BTBH_GPU_HDR_FALLBACK_SWITCH_FAILED);
    assert(strstr(state.message, "encode is unavailable") != NULL);
    assert(strstr(line, "event=encode") != NULL);
    assert(btbh_gpu_window_poll_events(smoke.window, &snapshot) == BTBH_GPU_OK && snapshot.hdr_changed == 1u);
    BTBHGPUCreationStats before;
    btbh_gpu_creation_stats(&before);
    for (uint32_t repeat = 0u; repeat < 3u; ++repeat) {
        frame(&smoke, &post);
    }
    BTBHGPUCreationStats after;
    btbh_gpu_creation_stats(&after);
    assert(btbh_gpu_window_hdr_state(smoke.window, &state) == BTBH_GPU_OK && state.active == 0u);
    assert(after.shaders == before.shaders && after.graphics_pipelines == before.graphics_pipelines);
    /* A set_hdr switch tries once more: the prewarm can't make it either, so SDR at once. */
    (void)set_hdr(&smoke, BTBH_GPU_HDR_MODE_OFF, 203.0f, 1000.0f);
    state = set_hdr(&smoke, BTBH_GPU_HDR_MODE_AUTO, 203.0f, 1000.0f);
    (void)btbh_gpu_window_hdr_describe(smoke.window, line, sizeof(line));
    assert(state.active == 0u && state.fallback == BTBH_GPU_HDR_FALLBACK_SWITCH_FAILED);
    assert(strstr(line, "event=encode") != NULL);
    frame(&smoke, &post);
    printf("no encode: %d files copied without the encode; SDR at the next frame, not retried, once more on a "
        "switch\n", copied);
    btbh_gpu_window_destroy(smoke.window);

    memcpy(shader_paths, saved_paths, sizeof(saved_paths));
    for (int index = 0; index < count; ++index) {
        char target[1024];
        (void)snprintf(target, sizeof(target), "%s/%s", directory, names[index]);
        (void)SDL_RemovePath(target);
    }
    (void)SDL_RemovePath(directory);
    SDL_free(names);
}

/* Auto following the OS: the simulated display reports SDR, then HDR again, each time with an SDL HDR event. */
static void push_hdr_event(SmokeWindow *smoke) {
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_EVENT_WINDOW_HDR_STATE_CHANGED;
    event.window.windowID = SDL_GetWindowID(smoke->window->window);
    assert(SDL_PushEvent(&event));
}

static void check_auto(BTBHGPUDevice *device) {
    set_env("BTBH_GPU_HDR_SIMULATE", "hdr10");
    SmokeWindow smoke = open_window(device, "HDR smoke: auto", false);
    set_env("BTBH_GPU_HDR_SIMULATE", NULL);
    const BTBHGPUScenePost post = smoke_post();
    frame(&smoke, &post);
    uint8_t *sdr = read_rgba8(&smoke);
    BTBHGPUWindowHDRState state = set_hdr(&smoke, BTBH_GPU_HDR_MODE_AUTO, 0.0f, 0.0f);
    assert(state.active == 1u && state.peak_nits == 1000.0f && state.paper_white_nits == 203.0f);
    frame(&smoke, &post);
    BTBHGPUPlatformSnapshot snapshot;
    assert(btbh_gpu_window_poll_events(smoke.window, &snapshot) == BTBH_GPU_OK);

    smoke.window->hdr_simulate_os_sdr = true;
    push_hdr_event(&smoke);
    assert(btbh_gpu_window_poll_events(smoke.window, &snapshot) == BTBH_GPU_OK && snapshot.hdr_changed == 1u);
    assert(btbh_gpu_window_hdr_state(smoke.window, &state) == BTBH_GPU_OK);
    assert(state.active == 1u && state.available == 0u && state.fallback == BTBH_GPU_HDR_FALLBACK_PENDING);
    frame(&smoke, &post);
    assert(btbh_gpu_window_hdr_state(smoke.window, &state) == BTBH_GPU_OK);
    assert(state.active == 0u && state.fallback == BTBH_GPU_HDR_FALLBACK_OS_SDR);
    assert(state.mode == BTBH_GPU_HDR_MODE_AUTO);
    uint8_t *after = read_rgba8(&smoke);
    assert(memcmp(sdr, after, smoke.width * smoke.height * 4u) == 0);
    assert(btbh_gpu_window_poll_events(smoke.window, &snapshot) == BTBH_GPU_OK && snapshot.hdr_changed == 1u);
    char line[BTBH_GPU_HDR_DESCRIBE_CAPACITY];
    (void)btbh_gpu_window_hdr_describe(smoke.window, line, sizeof(line));
    assert(strstr(line, "event=switch") != NULL && strstr(line, "fallback=os_sdr") != NULL);
    printf("auto, OS to SDR: %s\n", line);

    smoke.window->hdr_simulate_os_sdr = false;
    push_hdr_event(&smoke);
    assert(btbh_gpu_window_poll_events(smoke.window, &snapshot) == BTBH_GPU_OK && snapshot.hdr_changed == 1u);
    frame(&smoke, &post);
    assert(btbh_gpu_window_hdr_state(smoke.window, &state) == BTBH_GPU_OK);
    assert(state.active == 1u && state.fallback == BTBH_GPU_HDR_FALLBACK_NONE);
    (void)btbh_gpu_window_hdr_describe(smoke.window, line, sizeof(line));
    printf("auto, OS back to HDR: %s\n", line);

    /* No event (SDL's answer changes silently, as Wine's lists may): hdr_state sees it (a new serial, PENDING) and the
       next frame switches; with no call at all, the frame start's re-check a second later. */
    assert(btbh_gpu_window_poll_events(smoke.window, &snapshot) == BTBH_GPU_OK);
    BTBHGPUWindowHDRState quiet;
    assert(btbh_gpu_window_hdr_state(smoke.window, &quiet) == BTBH_GPU_OK);
    const uint32_t quiet_serial = quiet.serial;
    smoke.window->hdr_simulate_os_sdr = true;
    assert(btbh_gpu_window_hdr_state(smoke.window, &quiet) == BTBH_GPU_OK);
    assert(quiet.serial != quiet_serial && quiet.active == 1u && quiet.fallback == BTBH_GPU_HDR_FALLBACK_PENDING);
    frame(&smoke, &post);
    assert(btbh_gpu_window_hdr_state(smoke.window, &quiet) == BTBH_GPU_OK);
    assert(quiet.active == 0u && quiet.fallback == BTBH_GPU_HDR_FALLBACK_OS_SDR);
    assert(btbh_gpu_window_poll_events(smoke.window, &snapshot) == BTBH_GPU_OK && snapshot.hdr_changed == 1u);
    smoke.window->hdr_simulate_os_sdr = false;
    frame(&smoke, &post);
    SDL_Delay(1100u);
    frame(&smoke, &post);
    assert(btbh_gpu_window_hdr_state(smoke.window, &quiet) == BTBH_GPU_OK && quiet.active == 1u);
    assert(btbh_gpu_window_poll_events(smoke.window, &snapshot) == BTBH_GPU_OK && snapshot.hdr_changed == 1u);
    (void)btbh_gpu_window_hdr_describe(smoke.window, line, sizeof(line));
    printf("auto, no event: PENDING seen by hdr_state, SDR at the next frame, HDR again by the re-check: %s\n", line);
    free(sdr);
    free(after);
    btbh_gpu_window_destroy(smoke.window);
}

/* Captures and copies in HDR (P = 4.93), and render targets in the frame format. */
static void check_copies(BTBHGPUDevice *device) {
    set_env("BTBH_GPU_HDR_SIMULATE", "hdr10");
    SmokeWindow smoke = open_window(device, "HDR smoke: copies", true);
    set_env("BTBH_GPU_HDR_SIMULATE", NULL);
    const BTBHGPUScenePost post = smoke_post();
    const BTBHGPUWindowHDRState state = set_hdr(&smoke, BTBH_GPU_HDR_MODE_AUTO, 203.0f, 1000.0f);
    assert(state.active == 1u);
    BTBHGPUTextureReport texture_report;
    BTBHGPUTexture *frame_target = btbh_gpu_window_render_target_create_ex(smoke.window, smoke.width, smoke.height,
        BTBH_GPU_TEXTURE_FILTER_LINEAR, BTBH_GPU_RENDER_TARGET_FRAME_FORMAT, &texture_report);
    BTBHGPUTexture *sdr_target = btbh_gpu_window_render_target_create(smoke.window, smoke.width, smoke.height,
        BTBH_GPU_TEXTURE_FILTER_LINEAR, &texture_report);
    assert(frame_target != NULL && frame_target->format == SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT);
    assert(sdr_target != NULL && sdr_target->format == smoke.window->sdr_format);

    BTBHGPUSceneReport report;
    assert(present_scene(&smoke, &post, &report) == BTBH_GPU_OK);
    BTBHGPUTexture *capture = btbh_gpu_window_capture_pending_scene_texture(smoke.window, &texture_report);
    assert(capture != NULL && capture->format == SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT);
    assert(btbh_gpu_window_copy_pending_scene_texture(smoke.window, frame_target, &texture_report) == BTBH_GPU_OK);
    assert(present_scene(&smoke, &post, &report) == BTBH_GPU_OK);
    assert(btbh_gpu_window_copy_pending_scene_texture(smoke.window, sdr_target, &texture_report) == BTBH_GPU_OK);

    /* Read both targets back through a canvas pass? The textures themselves, through SDL. */
    SDL_GPUDevice *gpu = smoke.window->device->gpu;
    const uint32_t pixels = smoke.width * smoke.height;
    const uint32_t sizes[2] = { pixels * 8u, pixels * 4u };
    BTBHGPUTexture *const textures[2] = { frame_target, sdr_target };
    uint8_t *data[2];
    for (uint32_t index = 0u; index < 2u; ++index) {
        SDL_GPUTransferBufferCreateInfo info;
        SDL_zero(info);
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        info.size = sizes[index];
        SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(gpu, &info);
        SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(gpu);
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command);
        SDL_GPUTextureRegion region;
        SDL_zero(region);
        region.texture = textures[index]->texture;
        region.w = smoke.width;
        region.h = smoke.height;
        region.d = 1u;
        SDL_GPUTextureTransferInfo destination;
        SDL_zero(destination);
        destination.transfer_buffer = transfer;
        SDL_DownloadFromGPUTexture(copy, &region, &destination);
        SDL_EndGPUCopyPass(copy);
        SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
        assert(fence != NULL && SDL_WaitForGPUFences(gpu, true, &fence, 1u));
        SDL_ReleaseGPUFence(gpu, fence);
        data[index] = malloc(sizes[index]);
        assert(data[index] != NULL);
        memcpy(data[index], SDL_MapGPUTransferBuffer(gpu, transfer, false), sizes[index]);
        SDL_UnmapGPUTransferBuffer(gpu, transfer);
        SDL_ReleaseGPUTransferBuffer(gpu, transfer);
    }
    const bool bgra = sdr_target->format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    int largest = 0;
    uint32_t folded = 0u;
    for (uint32_t pixel = 0u; pixel < pixels; ++pixel) {
        uint16_t halves[4];
        memcpy(halves, data[0] + pixel * 8u, sizeof(halves));
        const float value[3] = {
            btbh_hdr_half_to_float(halves[0]), btbh_hdr_half_to_float(halves[1]), btbh_hdr_half_to_float(halves[2])
        };
        float expected[3];
        btbh_hdr_fold_pixel(value, state.highlight_ratio, expected);
        folded += fmaxf(value[0], fmaxf(value[1], value[2])) > 1.0f ? 1u : 0u;
        for (uint32_t channel = 0u; channel < 3u; ++channel) {
            const uint32_t byte = bgra ? 2u - channel : channel;
            const int got = data[1][pixel * 4u + byte];
            const int want = (int)(btbh_hdr_saturate(expected[channel]) * 255.0f + 0.5f);
            largest = abs(got - want) > largest ? abs(got - want) : largest;
        }
    }
    printf("copies: the fold into SDR against the CPU: largest difference %d codes; %u pixels above paper white\n",
        largest, folded);
    assert(largest <= 1);
    assert(folded > 50u);
    free(data[0]);
    free(data[1]);

    /* In SDR: a copy into the FRAME_FORMAT target (RGBA16F, a colour target) is a blit; an RGBA16F texture that isn't a
       colour target is refused, by a copy and by a canvas pass, as 0.16.1 refused every RGBA16F texture. */
    (void)set_hdr(&smoke, BTBH_GPU_HDR_MODE_OFF, 203.0f, 1000.0f);
    BTBHGPUTexture *sampled = btbh_gpu_window_texture_create_empty(smoke.window, smoke.width, smoke.height,
        SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, SDL_GPU_TEXTUREUSAGE_SAMPLER, BTBH_GPU_TEXTURE_FILTER_LINEAR,
        BTBH_GPU_TEXTURE_WRAP_CLAMP, &texture_report);
    assert(sampled != NULL && !sampled->color_target);
    assert(present_scene(&smoke, &post, &report) == BTBH_GPU_OK);
    assert(btbh_gpu_window_copy_pending_scene_texture(smoke.window, sampled, &texture_report) ==
        BTBH_GPU_INVALID_ARGUMENT);
    assert(btbh_gpu_window_copy_pending_scene_texture(smoke.window, frame_target, &texture_report) == BTBH_GPU_OK);
    BTBHGPUCanvasPass pass;
    memset(&pass, 0, sizeof(pass));
    pass.target = sampled;
    BTBHGPUCanvasReport canvas_report;
    assert(btbh_gpu_window_canvas_present(smoke.window, shader_paths[11], shader_paths[12], shader_paths[13],
        SMOKE_SHADER_FORMAT, NULL, 0u, NULL, 0u, &pass, 1u, &canvas_report) == BTBH_GPU_INVALID_ARGUMENT);
    pass.target = frame_target;
    assert(btbh_gpu_window_canvas_present(smoke.window, shader_paths[11], shader_paths[12], shader_paths[13],
        SMOKE_SHADER_FORMAT, NULL, 0u, NULL, 0u, &pass, 1u, &canvas_report) == BTBH_GPU_OK);
    printf("copies, SDR: into the frame-format target taken, into a sampled RGBA16F texture refused (copy, canvas)\n");
    btbh_gpu_texture_destroy(sampled);
    btbh_gpu_texture_destroy(capture);
    btbh_gpu_texture_destroy(frame_target);
    btbh_gpu_texture_destroy(sdr_target);
    btbh_gpu_window_destroy(smoke.window);
}

/* The HDR tail's other passes: FXAA, SMAA, FSR 1, FSR 3, tilt-shift, the LUT, CAS (with bloom). */
static void check_tail(BTBHGPUDevice *device) {
    set_env("BTBH_GPU_HDR_SIMULATE", "hdr10");
    SmokeWindow smoke = open_window(device, "HDR smoke: tail", false);
    set_env("BTBH_GPU_HDR_SIMULATE", NULL);
    const BTBHGPUWindowHDRState state = set_hdr(&smoke, BTBH_GPU_HDR_MODE_AUTO, 203.0f, 1000.0f);
    assert(state.active == 1u);
    uint8_t *lut_pixels = malloc(512u * 512u * 4u);
    assert(lut_pixels != NULL);
    for (uint32_t y = 0u; y < 512u; ++y) {
        for (uint32_t x = 0u; x < 512u; ++x) {
            const uint32_t blue = (y / 64u) * 8u + x / 64u;
            uint8_t *texel = &lut_pixels[(y * 512u + x) * 4u];
            texel[0] = (uint8_t)((x % 64u) * 255u / 63u);
            texel[1] = (uint8_t)((y % 64u) * 255u / 63u);
            texel[2] = (uint8_t)(blue * 255u / 63u);
            texel[3] = 255u;
        }
    }
    BTBHGPUTextureReport texture_report;
    BTBHGPUTexture *lut = btbh_gpu_window_texture_create_rgba8_ex(smoke.window, lut_pixels, 512u, 512u, 0u,
        BTBH_GPU_TEXTURE_FILTER_LINEAR, BTBH_GPU_TEXTURE_WRAP_CLAMP, BTBH_GPU_TEXTURE_COLOR_SPACE_LINEAR,
        &texture_report);
    free(lut_pixels);
    assert(lut != NULL);
    const char *names[8] = {
        "FXAA", "SMAA", "FSR 1", "FSR 3", "tilt-shift", "LUT and CAS", "LUT, CAS, tilt-shift", "LUT, SMAA, exposure 1e6"
    };
    const float peak = powf(state.highlight_ratio, 1.0f / 2.2f);
    for (uint32_t variant = 0u; variant < 8u; ++variant) {
        BTBHGPUScenePost post = smoke_post();
        post.bloom_enabled = 1u;
        post.bloom_intensity = 0.3f;
        post.bloom_threshold = 1.0f;
        post.bloom_soft_knee = 0.5f;
        post.bloom_scatter = 0.7f;
        switch (variant) {
            case 0u: post.anti_aliasing = 3u; break; /* FXAA */
            case 1u: post.anti_aliasing = 2u; break;
            case 2u: post.upscale_mode = BTBH_GPU_SCENE_UPSCALE_FSR1; break;
            case 3u:
                post.upscale_mode = BTBH_GPU_SCENE_UPSCALE_FSR3;
                post.temporal_camera_near = 0.1f;
                post.temporal_camera_far = 60.0f;
                post.temporal_vertical_fov = 1.0f;
                post.temporal_frame_seconds = 1.0f / 60.0f;
                post.temporal_meters_per_unit = 1.0f;
                break;
            case 4u:
                post.tilt_shift_enabled = 1u;
                post.focus_point_x = 0.5f;
                post.focus_point_y = 0.5f;
                post.focus_direction_x = 1.0f;
                post.focus_half_width = 0.1f;
                post.maximum_radius_pixels = 6.0f;
                post.highlight_threshold = 0.8f;
                post.highlight_gain = 1.0f;
                break;
            case 5u:
                post.lut_enabled = 1u;
                post.lut_texture = lut;
                post.cas_enabled = 1u;
                post.cas_sharpness = 0.5f;
                break;
            case 6u:
                post.lut_enabled = 1u;
                post.lut_texture = lut;
                post.cas_enabled = 1u;
                post.cas_sharpness = 0.5f;
                post.tilt_shift_enabled = 1u;
                post.focus_point_x = 0.5f;
                post.focus_point_y = 0.5f;
                post.focus_direction_x = 1.0f;
                post.focus_half_width = 0.1f;
                post.maximum_radius_pixels = 6.0f;
                break;
            default:
                /* The heat (exposure times the scene) far past the largest half: the LUT pass's alpha must stay
                   finite and the gain P, or NaN spreads through SMAA. */
                post.lut_enabled = 1u;
                post.lut_texture = lut;
                post.anti_aliasing = 2u;
                post.exposure = 1.0e6f;
                break;
        }
        for (uint32_t repeat = 0u; repeat < 3u; ++repeat) {
            frame(&smoke, &post);
        }
        float *values = read_frame(&smoke);
        uint32_t bad = 0u;
        uint32_t lifted = 0u;
        for (uint32_t index = 0u; index < smoke.width * smoke.height * 4u; ++index) {
            if (index % 4u == 3u) {
                continue;
            }
            if (!(values[index] >= 0.0f) || values[index] > peak * 1.002f) {
                bad += 1u;
            }
            lifted += values[index] > 1.0f ? 1u : 0u;
        }
        printf("tail %s: upscale %u, %u values out of [0, peak], %u above paper white\n", names[variant],
            (unsigned)last_upscale_mode, bad, lifted);
        if (variant == 3u) {
            assert(last_upscale_mode == BTBH_GPU_SCENE_UPSCALE_FSR3);
        }
        assert(bad == 0u);
        assert(lifted > 20u);
        free(values);
    }
    btbh_gpu_texture_destroy(lut);
    btbh_gpu_window_destroy(smoke.window);
}

int main(void) {
    (void)setvbuf(stdout, NULL, _IONBF, 0);
    (void)SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    for (int index = 0; index < 14; ++index) {
        (void)snprintf(shader_paths[index], sizeof(shader_paths[index]), "%s/%s%s", BTBH_GPU_SMOKE_SHADER_DIR,
            shader_names[index], SMOKE_SHADER_EXTENSION);
        FILE *file = fopen(shader_paths[index], "rb");
        if (file == NULL) {
            fprintf(stderr, "skipped: no shader %s\n", shader_paths[index]);
            return SMOKE_SKIP;
        }
        fclose(file);
    }
    const BTBHGPUDeviceDescriptor device_descriptor = {
        .backend = SMOKE_BACKEND,
        .required_features = BTBH_GPU_REQUIRED_FEATURES,
        .debug_mode = 0u,
    };
    BTBHGPUDeviceReport device_report;
    memset(&device_report, 0, sizeof(device_report));
    BTBHGPUDevice *device = btbh_gpu_device_create(&device_descriptor, &device_report);
    if (device == NULL) {
        fprintf(stderr, "skipped: no device (%s)\n", device_report.message);
        return SMOKE_SKIP;
    }
    check_plain_window(device);
    check_simulated(device, "hdr10");
    check_simulated(device, "scrgb");
    check_toggle(device);
    check_frame_nits(device);
    check_auto(device);
    check_encode_unavailable(device);
    check_copies(device);
    check_tail(device);
    btbh_gpu_device_shutdown(device);
    btbh_gpu_device_destroy(device);
    printf("hdr smoke passed\n");
    return 0;
}
