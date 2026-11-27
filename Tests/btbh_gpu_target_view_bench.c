/* A measurement, not a test (0.15.0): what a target view's rect and kept runtime save on Be The Bullet Hell v2's
 * second co-op view, a scene rendered every frame into a 4K target with btbh_gpu_window_scene_render_to_target_view.
 *
 * The scene: a city block of 48 x 48 lit boxes (heights 0.5 to 6) in 96 instanced batches on a ground, under a sun
 * with two shadow maps (primary and secondary directional, 2048^2), HBAO with contact AO, SSR, bloom, fog, vignette and
 * FXAA, ACES. It renders at the target's size (3840 x 2160 by default) without upscaling.
 *
 * Configurations, each timed in every round, in another order each round:
 * - full, half, diagonal: view 0 (afresh, both maps drawn every call) with no rect, the right half (a straight
 *   divider) and the bounding box of the half beyond a diagonal divider through the centre (BTBH_TARGET_BENCH_ANGLE
 *   degrees from vertical, default 30);
 * - kept-full, kept-half, kept-diagonal: the same with view 1, kept, under shadow update periods of
 *   BTBH_TARGET_BENCH_PERIOD (default 3) frames.
 * A sample is BTBH_TARGET_BENCH_BATCH (default 12) renders submitted back to back and a wait for the GPU: the GPU's
 * time a render is the sample's wall time over the batch (the CPU's encoding, ~2 ms a render here, overlaps the GPU's
 * work). BTBH_TARGET_BENCH_ROUNDS (default 25) rounds; the first 3 are dropped. BTBH_TARGET_BENCH_SIZE=WxH changes the
 * target (and render) size, BTBH_TARGET_BENCH_SUN_MAP=<size> the primary sun map's (default the renderer's 2048),
 * BTBH_TARGET_BENCH_DETAIL=<n> cuts each box's faces into n x n quads (default 1: 12 triangles a box; 8: 768, so 1.8
 * million triangles in the scene and in each sun map, about Be The Bullet Hell v2's city).
 * BTBH_TARGET_BENCH_BAND=1 first measures the band along the straight divider (a render of the whole target against
 * one with the half's rect, read back) with the whole chain, without bloom, and without bloom and SSR.
 *
 * Output: one line a configuration with the median, the 10th and 90th percentiles and min/max in ms a render, and the
 * share of pixels its rect covers. With no device it is skipped (77). */
#include "btbh_gpu/btbh_gpu.h"
#include "btbh_gpu_internal.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_timer.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BENCH_SKIP 77
#define GRID 48u
#define BATCHES 96u
#define CONFIGS 6u

#if defined(__APPLE__)
#define BENCH_SHADER_EXTENSION ".metallib"
#define BENCH_SHADER_FORMAT BTBH_GPU_SHADER_FORMAT_METALLIB
#define BENCH_BACKEND BTBH_GPU_BACKEND_METAL
#else
#define BENCH_SHADER_EXTENSION ".spv"
#define BENCH_SHADER_FORMAT BTBH_GPU_SHADER_FORMAT_SPIRV
#define BENCH_BACKEND BTBH_GPU_BACKEND_VULKAN
#endif

static const char *const shader_names[11] = {
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
};
static char shader_paths[11][1024];

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

typedef struct BenchCamera {
    float view_projection[16];
    float right[3];
    float up[3];
    float position[3];
} BenchCamera;

static BenchCamera look_at(const float eye[3], const float centre[3], float fov_y, float aspect,
    const float projection_override[16]) {
    BenchCamera camera;
    memset(&camera, 0, sizeof(camera));
    float forward[3] = {centre[0] - eye[0], centre[1] - eye[1], centre[2] - eye[2]};
    normalize3(forward);
    const float world_up[3] = {0.0f, 1.0f, 0.0f};
    float side[3];
    cross3(forward, world_up, side);
    normalize3(side);
    float up[3];
    cross3(side, forward, up);
    const float view[16] = {
        side[0], up[0], -forward[0], 0.0f,
        side[1], up[1], -forward[1], 0.0f,
        side[2], up[2], -forward[2], 0.0f,
        -dot3(eye, side), -dot3(eye, up), dot3(eye, forward), 1.0f,
    };
    float projection[16];
    if (projection_override != NULL) {
        memcpy(projection, projection_override, sizeof(projection));
    } else {
        const float near_plane = 0.2f;
        const float far_plane = 200.0f;
        const float h = 1.0f / tanf(0.5f * fov_y);
        const float r = far_plane / (near_plane - far_plane);
        const float perspective[16] = {
            h / aspect, 0.0f, 0.0f, 0.0f,
            0.0f, h, 0.0f, 0.0f,
            0.0f, 0.0f, r, -1.0f,
            0.0f, 0.0f, r * near_plane, 0.0f,
        };
        memcpy(projection, perspective, sizeof(projection));
    }
    multiply(projection, view, camera.view_projection);
    memcpy(camera.right, side, sizeof(side));
    memcpy(camera.up, up, sizeof(up));
    memcpy(camera.position, eye, sizeof(camera.position));
    return camera;
}

static void orthographic_sun(const float sun[3], float half_extent, float out[16]) {
    const float centre[3] = {0.0f, 0.0f, 0.0f};
    const float rcp = 1.0f / half_extent;
    const float range = 1.0f / (1.0f - 120.0f);
    const float orthographic[16] = {
        rcp, 0.0f, 0.0f, 0.0f,
        0.0f, rcp, 0.0f, 0.0f,
        0.0f, 0.0f, range, 0.0f,
        0.0f, 0.0f, range * 1.0f, 1.0f,
    };
    const BenchCamera light = look_at(sun, centre, 1.0f, 1.0f, orthographic);
    memcpy(out, light.view_projection, sizeof(float) * 16u);
}

static void instance(float out[24], float x, float y, float z, float sx, float sy, float sz, const float colour[4]) {
    memset(out, 0, 24 * sizeof(float));
    out[0] = sx;
    out[5] = sy;
    out[10] = sz;
    out[12] = x;
    out[13] = y;
    out[14] = z;
    out[15] = 1.0f;
    memcpy(&out[16], colour, 4 * sizeof(float));
}

/* A unit box (-0.5..0.5) wound so the renderer lights its outsides, each face cut into detail x detail quads (a
   city piece's triangle count: 12 triangles at 1, 768 at 8); 24 floats a vertex. */
static BTBHGPUCustomMesh *create_cube(BTBHGPUWindow *window, uint32_t detail) {
    static const float faces[6][3] = {
        {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
    };
    const uint32_t side = detail + 1u;
    const uint32_t vertex_count = 6u * side * side;
    const uint32_t index_count = 6u * detail * detail * 6u;
    float *vertices = calloc((size_t)vertex_count * 24u, sizeof(float));
    uint32_t *indices = calloc(index_count, sizeof(uint32_t));
    if (vertices == NULL || indices == NULL) {
        free(vertices);
        free(indices);
        return NULL;
    }
    uint32_t next_index = 0u;
    for (uint32_t face = 0u; face < 6u; ++face) {
        const float *n = faces[face];
        float u[3] = {n[1], n[2], n[0]};
        float v[3];
        cross3(n, u, v);
        const uint32_t base = face * side * side;
        for (uint32_t j = 0u; j < side; ++j) {
            for (uint32_t i = 0u; i < side; ++i) {
                const float su = -0.5f + (float)i / (float)detail;
                const float sv = -0.5f + (float)j / (float)detail;
                float *vertex = &vertices[(size_t)(base + j * side + i) * 24u];
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
        }
        for (uint32_t j = 0u; j < detail; ++j) {
            for (uint32_t i = 0u; i < detail; ++i) {
                const uint32_t a = base + j * side + i;
                const uint32_t b = a + 1u;
                const uint32_t c = a + side + 1u;
                const uint32_t d = a + side;
                const uint32_t quad[6] = {a, c, b, a, d, c};
                memcpy(&indices[next_index], quad, sizeof(quad));
                next_index += 6u;
            }
        }
    }
    BTBHGPUCustomMeshReport report;
    memset(&report, 0, sizeof(report));
    BTBHGPUCustomMesh *mesh = btbh_gpu_window_custom_mesh_create(window, vertices,
        (uint32_t)(sizeof(float) * 24u * vertex_count), vertex_count, indices, index_count, &report);
    free(vertices);
    free(indices);
    return mesh;
}

static int compare_doubles(const void *a, const void *b) {
    const double x = *(const double *)a;
    const double y = *(const double *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

typedef struct BenchScene {
    BTBHGPUWindow *window;
    BTBHGPUTexture *target;
    float *instances;
    uint32_t instance_count;
    BTBHGPUSceneGeometryBatch batches[BATCHES + 1u];
    BTBHGPUSceneLighting lighting;
    BTBHGPUScenePost post;
    BenchCamera camera;
    uint32_t width;
    uint32_t height;
} BenchScene;

static BTBHGPUResult render(BenchScene *scene, const BTBHGPUSceneTargetView *view, BTBHGPUSceneReport *report) {
    return btbh_gpu_window_scene_render_to_target_view(
        scene->window, scene->target,
        shader_paths[0], shader_paths[1], shader_paths[2], shader_paths[3],
        shader_paths[4], shader_paths[5], shader_paths[6], shader_paths[7],
        shader_paths[8], shader_paths[9], shader_paths[10],
        BENCH_SHADER_FORMAT, scene->width, scene->height,
        scene->camera.view_projection, scene->camera.right, scene->camera.up, scene->camera.position,
        &scene->lighting, &scene->post, scene->instances, scene->instance_count, scene->instance_count,
        scene->batches, NULL, NULL, BATCHES + 1u,
        30u, 40u, 55u, 255u, view, report);
}

/* The target's bytes (4 a texel), read back. */
static uint8_t *read_texture(BTBHGPUWindow *window, BTBHGPUTexture *texture) {
    SDL_GPUDevice *device = window->device->gpu;
    const uint32_t size = texture->width * texture->height * 4u;
    SDL_GPUTransferBufferCreateInfo info;
    SDL_zero(info);
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    info.size = size;
    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(device, &info);
    SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(device);
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
    (void)SDL_WaitForGPUFences(device, true, &fence, 1u);
    SDL_ReleaseGPUFence(device, fence);
    uint8_t *pixels = malloc(size);
    const void *mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
    memcpy(pixels, mapped, size);
    SDL_UnmapGPUTransferBuffer(device, transfer);
    SDL_ReleaseGPUTransferBuffer(device, transfer);
    return pixels;
}

/* Waits until the GPU has run everything submitted. */
static void wait_idle(BTBHGPUWindow *window) {
    (void)SDL_WaitForGPUIdle(window->device->gpu);
}

int main(void) {
    for (int index = 0; index < 11; ++index) {
        (void)snprintf(shader_paths[index], sizeof(shader_paths[index]), "%s/%s%s", BTBH_GPU_SMOKE_SHADER_DIR,
            shader_names[index], BENCH_SHADER_EXTENSION);
    }
    uint32_t width = 3840u;
    uint32_t height = 2160u;
    const char *size = SDL_getenv("BTBH_TARGET_BENCH_SIZE");
    if (size != NULL) {
        char *end = NULL;
        const unsigned long w = strtoul(size, &end, 10);
        const unsigned long h = end != NULL && *end == 'x' ? strtoul(end + 1, NULL, 10) : 0ul;
        if (w >= 64ul && h >= 64ul && w <= 8192ul && h <= 8192ul) {
            width = (uint32_t)w;
            height = (uint32_t)h;
        }
    }
    const char *angle_text = SDL_getenv("BTBH_TARGET_BENCH_ANGLE");
    const double angle_degrees = angle_text != NULL ? atof(angle_text) : 30.0;
    const char *period_text = SDL_getenv("BTBH_TARGET_BENCH_PERIOD");
    const uint32_t period = period_text != NULL ? (uint32_t)atoi(period_text) : 3u;
    const char *batch_text = SDL_getenv("BTBH_TARGET_BENCH_BATCH");
    const uint32_t renders_per_sample =
        batch_text != NULL && atoi(batch_text) > 0 ? (uint32_t)atoi(batch_text) : 12u;
    const char *rounds_text = SDL_getenv("BTBH_TARGET_BENCH_ROUNDS");
    const uint32_t rounds = rounds_text != NULL && atoi(rounds_text) > 3 ? (uint32_t)atoi(rounds_text) : 25u;

    (void)SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    const BTBHGPUDeviceDescriptor device_descriptor = {
        .backend = BENCH_BACKEND,
        .required_features = BTBH_GPU_REQUIRED_FEATURES,
        .debug_mode = 0,
    };
    BTBHGPUDeviceReport device_report;
    memset(&device_report, 0, sizeof(device_report));
    BTBHGPUDevice *device = btbh_gpu_device_create(&device_descriptor, &device_report);
    if (device == NULL) {
        fprintf(stderr, "skipped: no device (%s)\n", device_report.message);
        return BENCH_SKIP;
    }
    const BTBHGPUWindowDescriptor descriptor = {
        .title = "BTBH target view bench",
        .width = 640,
        .height = 360,
        .hidden = 1,
        .high_pixel_density = 0,
    };
    BTBHGPUWindowReport window_report;
    memset(&window_report, 0, sizeof(window_report));
    BenchScene scene;
    memset(&scene, 0, sizeof(scene));
    scene.width = width;
    scene.height = height;
    scene.window = btbh_gpu_window_create(&descriptor, &window_report);
    if (scene.window == NULL || btbh_gpu_window_claim(device, scene.window, &window_report) != BTBH_GPU_OK) {
        fprintf(stderr, "window: %s\n", window_report.message);
        return 1;
    }
    const char *detail_text = SDL_getenv("BTBH_TARGET_BENCH_DETAIL");
    const uint32_t detail = detail_text != NULL && atoi(detail_text) > 0 && atoi(detail_text) <= 64
        ? (uint32_t)atoi(detail_text)
        : 1u;
    BTBHGPUCustomMesh *cube = create_cube(scene.window, detail);
    BTBHGPUTextureReport texture_report;
    scene.target = btbh_gpu_window_render_target_create(scene.window, width, height, BTBH_GPU_TEXTURE_FILTER_LINEAR,
        &texture_report);
    if (cube == NULL || scene.target == NULL) {
        fprintf(stderr, "resources: %s\n", texture_report.message);
        return 1;
    }

    /* The block: GRID x GRID boxes, BATCHES batches of the same mesh (a row band each), then the ground. */
    scene.instance_count = GRID * GRID + 1u;
    scene.instances = calloc((size_t)scene.instance_count * 24u, sizeof(float));
    uint32_t seed = 12345u;
    for (uint32_t z = 0u; z < GRID; ++z) {
        for (uint32_t x = 0u; x < GRID; ++x) {
            seed = seed * 1664525u + 1013904223u;
            const float h = 0.5f + 5.5f * (float)(seed >> 8) / 16777216.0f;
            seed = seed * 1664525u + 1013904223u;
            const float tint = 0.45f + 0.4f * (float)(seed >> 8) / 16777216.0f;
            const float colour[4] = {tint, tint * 0.95f, tint * 0.85f, 1.0f};
            const float px = -36.0f + 1.5f * (float)x;
            const float pz = -36.0f + 1.5f * (float)z;
            instance(&scene.instances[(size_t)(z * GRID + x) * 24u], px, 0.5f * h, pz, 1.1f, h, 1.1f, colour);
        }
    }
    const float ground[4] = {0.4f, 0.42f, 0.38f, 1.0f};
    instance(&scene.instances[(size_t)(GRID * GRID) * 24u], 0.0f, -0.05f, 0.0f, 90.0f, 0.1f, 90.0f, ground);
    const uint32_t per_batch = GRID * GRID / BATCHES;
    for (uint32_t index = 0u; index <= BATCHES; ++index) {
        BTBHGPUSceneGeometryBatch *batch = &scene.batches[index];
        memset(batch, 0, sizeof(*batch));
        batch->kind = BTBH_GPU_SCENE_GEOMETRY_CUSTOM_MESH;
        batch->custom_mesh = cube;
        batch->material_slot = -1;
        batch->first_instance = index < BATCHES ? index * per_batch : GRID * GRID;
        batch->instance_count = index < BATCHES ? per_batch : 1u;
        batch->albedo_red = 255u;
        batch->albedo_green = 255u;
        batch->albedo_blue = 255u;
        batch->albedo_alpha = 255u;
        batch->uv_scale_x = 1.0f;
        batch->uv_scale_y = 1.0f;
        batch->roughness = index < BATCHES ? 0.5f : 0.25f;
        batch->metalness = 0.05f;
        batch->occlusion = 1.0f;
        batch->depth_compare = BTBH_GPU_SCENE_DEPTH_LESS;
        batch->cull_mode = BTBH_GPU_SCENE_CULL_BACK;
        batch->stencil_pass = BTBH_GPU_SCENE_STENCIL_REPLACE;
        batch->spot_shadow_visible = 1u;
        if (index % 9u == 4u) {
            /* Some lit windows, for bloom. */
            batch->emission_red = 255u;
            batch->emission_green = 210u;
            batch->emission_blue = 150u;
            batch->emission_energy = 2.5f;
        }
    }

    BTBHGPUSceneLighting *lighting = &scene.lighting;
    lighting->ambient_red = 1.0f;
    lighting->ambient_green = 1.0f;
    lighting->ambient_blue = 1.0f;
    lighting->ambient_energy = 0.3f;
    lighting->directional_energy = 2.4f;
    lighting->directional_red = 1.0f;
    lighting->directional_green = 0.95f;
    lighting->directional_blue = 0.88f;
    const float sun[3] = {30.0f, 60.0f, 20.0f};
    lighting->directional_x = -sun[0];
    lighting->directional_y = -sun[1];
    lighting->directional_z = -sun[2];
    lighting->spot_range = 8.0f;
    lighting->spot_falloff = 1.0f;
    lighting->spot_inner_cutoff = 1.0f;
    orthographic_sun(sun, 20.0f, lighting->directional_shadow_view_projection);
    lighting->directional_shadow_enabled = 1u;
    lighting->directional_shadow_softness = 1.0f;
    lighting->directional_shadow_filter = BTBH_GPU_SCENE_SHADOW_FILTER_SOFT;
    lighting->secondary_directional_x = -sun[0];
    lighting->secondary_directional_y = -sun[1];
    lighting->secondary_directional_z = -sun[2];
    orthographic_sun(sun, 55.0f, lighting->secondary_directional_shadow_view_projection);
    lighting->secondary_directional_shadow_enabled = 1u;
    lighting->secondary_directional_shadow_softness = 1.0f;

    BTBHGPUScenePost *post = &scene.post;
    post->tonemap_white = 1.0f;
    post->exposure = 1.0f;
    post->contrast = 1.0f;
    post->saturation = 1.0f;
    post->blur_curve = 1.0f;
    post->tonemap_mode = BTBH_GPU_SCENE_TONEMAP_ACES;
    post->upscale_mode = BTBH_GPU_SCENE_UPSCALE_FSR1;
    post->ssao_enabled = 2u;
    post->ssao_radius = 1.0f;
    post->ssao_intensity = 1.0f;
    post->ssao_power = 1.5f;
    post->ssao_bias = 0.05f;
    post->ssao_contact = 1u;
    post->ssr_intensity = 0.5f;
    post->ssr_max_roughness = 0.7f;
    post->ssr_max_distance = 30.0f;
    post->ssr_thickness = 0.6f;
    post->bloom_enabled = 1u;
    post->bloom_intensity = 0.4f;
    post->bloom_threshold = 1.0f;
    post->bloom_soft_knee = 0.5f;
    post->bloom_scatter = 0.7f;
    post->fog_enabled = 1u;
    post->fog_color[0] = 0.6f;
    post->fog_color[1] = 0.65f;
    post->fog_color[2] = 0.7f;
    post->fog_density = 0.01f;
    post->fog_start_distance = 20.0f;
    post->fog_maximum_opacity = 0.6f;
    post->vignette_enabled = 1u;
    post->vignette_intensity = 0.3f;
    post->vignette_smoothness = 0.5f;
    post->anti_aliasing = 3u;

    const float eye[3] = {-20.0f, 22.0f, 34.0f};
    const float centre[3] = {2.0f, 0.0f, -2.0f};
    scene.camera = look_at(eye, centre, 0.9f, (float)width / (float)height, NULL);

    /* The rects: the right half; the bounding box of the half beyond a divider through the centre at the angle. */
    BTBHGPUSceneTargetView views[CONFIGS];
    memset(views, 0, sizeof(views));
    const uint32_t half_x = width / 2u;
    /* The divider: through the centre, angle from vertical; the half on its right. Its polygon's bounding box is the
       screen's corners on that side and the line's two crossings with the screen's edges. */
    const double radians = angle_degrees * 3.14159265358979 / 180.0;
    const double nx = cos(radians);
    const double ny = -sin(radians);
    const double cx = 0.5 * (double)width;
    const double cy = 0.5 * (double)height;
    double minimum_x = (double)width;
    double minimum_y = (double)height;
    double maximum_x = 0.0;
    double maximum_y = 0.0;
    const double corners[4][2] = {{0.0, 0.0}, {(double)width, 0.0}, {0.0, (double)height},
        {(double)width, (double)height}};
    for (uint32_t index = 0u; index < 4u; ++index) {
        const double side = (corners[index][0] - cx) * nx + (corners[index][1] - cy) * ny;
        if (side >= 0.0) {
            minimum_x = fmin(minimum_x, corners[index][0]);
            maximum_x = fmax(maximum_x, corners[index][0]);
            minimum_y = fmin(minimum_y, corners[index][1]);
            maximum_y = fmax(maximum_y, corners[index][1]);
        }
    }
    /* The line's points on the top and bottom edges (y = 0, height) and on the left and right ones. */
    for (uint32_t edge = 0u; edge < 4u; ++edge) {
        double px;
        double py;
        if (edge < 2u) {
            py = edge == 0u ? 0.0 : (double)height;
            if (fabs(nx) < 1.0e-9) {
                continue;
            }
            px = cx - (py - cy) * ny / nx;
        } else {
            px = edge == 2u ? 0.0 : (double)width;
            if (fabs(ny) < 1.0e-9) {
                continue;
            }
            py = cy - (px - cx) * nx / ny;
        }
        if (px < -0.5 || px > (double)width + 0.5 || py < -0.5 || py > (double)height + 0.5) {
            continue;
        }
        minimum_x = fmin(minimum_x, px);
        maximum_x = fmax(maximum_x, px);
        minimum_y = fmin(minimum_y, py);
        maximum_y = fmax(maximum_y, py);
    }
    const uint32_t box_x = (uint32_t)fmax(0.0, floor(minimum_x));
    const uint32_t box_y = (uint32_t)fmax(0.0, floor(minimum_y));
    const uint32_t box_right = (uint32_t)fmin((double)width, ceil(maximum_x));
    const uint32_t box_bottom = (uint32_t)fmin((double)height, ceil(maximum_y));
    for (uint32_t config = 0u; config < CONFIGS; ++config) {
        views[config].view_id = config >= 3u ? 1u : 0u;
        if (config % 3u == 1u) {
            views[config].scissor_x = half_x;
            views[config].scissor_width = width - half_x;
            views[config].scissor_height = height;
        } else if (config % 3u == 2u) {
            views[config].scissor_x = box_x;
            views[config].scissor_y = box_y;
            views[config].scissor_width = box_right - box_x;
            views[config].scissor_height = box_bottom - box_y;
        }
    }
    static const char *const names[CONFIGS] = {"full", "half", "diagonal", "kept-full", "kept-half", "kept-diagonal"};
    if (!btbh_gpu_window_scene_set_shadow_update_periods(scene.window, period, period, period)) {
        fprintf(stderr, "bad period %u\n", period);
        return 1;
    }
    const char *sun_map_text = SDL_getenv("BTBH_TARGET_BENCH_SUN_MAP");
    const uint32_t sun_map = sun_map_text != NULL ? (uint32_t)atoi(sun_map_text) : 2048u;
    if (!btbh_gpu_window_scene_set_directional_shadow_size(scene.window, sun_map, sun_map)) {
        fprintf(stderr, "bad sun map size %u\n", sun_map);
        return 1;
    }
    printf("BENCH target %ux%u, render %ux%u, %u boxes of %u triangles in %u batches, diagonal %.0f deg: box %u,%u"
        " %ux%u, shadow period %u, sun map %u, %u renders a sample, %u rounds\n", width, height, width, height,
        GRID * GRID, 12u * detail * detail, BATCHES, angle_degrees, box_x, box_y, box_right - box_x,
        box_bottom - box_y, period, sun_map, renders_per_sample, rounds);

    BTBHGPUSceneReport report;
    double cpu_milliseconds = 0.0;
    for (uint32_t config = 0u; config < CONFIGS; ++config) {
        for (uint32_t warm = 0u; warm < 6u; ++warm) {
            if (render(&scene, &views[config], &report) != BTBH_GPU_OK) {
                fprintf(stderr, "render %s: %s\n", names[config], report.message);
                return 1;
            }
            cpu_milliseconds = report.cpu_prepare_milliseconds + report.cpu_upload_milliseconds +
                report.cpu_shadow_encode_milliseconds + report.cpu_target_ensure_milliseconds +
                report.cpu_geometry_encode_milliseconds + report.cpu_post_encode_milliseconds;
        }
    }
    printf("BENCH CPU encode a render (last warm-up): %.3f ms\n", cpu_milliseconds);
    if (SDL_getenv("BTBH_TARGET_BENCH_BAND") != NULL) {
        /* The band along a straight divider: the right half rendered with its rect against the whole target, by
           distance from the divider (x = half_x), the widest distance where a pixel differs by more than 0, 2, 8 and
           32 (8-bit steps), for the whole chain, without bloom, and without bloom and SSR. */
        const BTBHGPUScenePost chain = scene.post;
        for (uint32_t variant = 0u; variant < 3u; ++variant) {
            scene.post = chain;
            if (variant >= 1u) {
                scene.post.bloom_enabled = 0u;
            }
            if (variant >= 2u) {
                scene.post.ssr_intensity = 0.0f;
            }
            if (render(&scene, &views[0], &report) != BTBH_GPU_OK) {
                return 1;
            }
            uint8_t *full = read_texture(scene.window, scene.target);
            if (render(&scene, &views[1], &report) != BTBH_GPU_OK) {
                return 1;
            }
            uint8_t *part = read_texture(scene.window, scene.target);
            uint32_t widest[4] = {0u, 0u, 0u, 0u};
            static const int thresholds[4] = {0, 2, 8, 32};
            int largest = 0;
            for (uint32_t y = 0u; y < height; ++y) {
                for (uint32_t x = half_x; x < width; ++x) {
                    const uint8_t *a = &full[((size_t)y * width + x) * 4u];
                    const uint8_t *b = &part[((size_t)y * width + x) * 4u];
                    int difference = 0;
                    for (uint32_t channel = 0u; channel < 3u; ++channel) {
                        const int d = abs((int)a[channel] - (int)b[channel]);
                        difference = d > difference ? d : difference;
                    }
                    largest = difference > largest ? difference : largest;
                    for (uint32_t index = 0u; index < 4u; ++index) {
                        if (difference > thresholds[index] && x - half_x + 1u > widest[index]) {
                            widest[index] = x - half_x + 1u;
                        }
                    }
                }
            }
            static const char *const variant_names[3] = {"whole chain", "no bloom", "no bloom, no SSR"};
            printf("BENCH band %-17s: differs (>0) to %4u px, >2 to %4u px, >8 to %4u px, >32 to %4u px; largest %3d\n",
                variant_names[variant], widest[0], widest[1], widest[2], widest[3], largest);
            free(full);
            free(part);
        }
        scene.post = chain;
    }
    double *samples = calloc((size_t)CONFIGS * rounds, sizeof(double));
    for (uint32_t round = 0u; round < rounds; ++round) {
        for (uint32_t step = 0u; step < CONFIGS; ++step) {
            const uint32_t config = (step + round) % CONFIGS;
            /* Idle first: the sample starts on an empty queue. */
            wait_idle(scene.window);
            const uint64_t started = SDL_GetTicksNS();
            for (uint32_t index = 0u; index < renders_per_sample; ++index) {
                if (render(&scene, &views[config], &report) != BTBH_GPU_OK) {
                    fprintf(stderr, "render %s: %s\n", names[config], report.message);
                    return 1;
                }
            }
            wait_idle(scene.window);
            samples[config * rounds + round] = (double)(SDL_GetTicksNS() - started) / 1.0e6 / (double)renders_per_sample;
        }
    }
    wait_idle(scene.window);
    const double pixels = (double)width * (double)height;
    for (uint32_t config = 0u; config < CONFIGS; ++config) {
        const uint32_t kept = rounds - 3u;
        double *values = &samples[config * rounds + 3u];
        qsort(values, kept, sizeof(double), compare_doubles);
        const double share = views[config].scissor_width != 0u
            ? (double)views[config].scissor_width * (double)views[config].scissor_height / pixels
            : 1.0;
        printf("BENCH %-14s pixels %5.1f %%  median %7.3f ms  p10 %7.3f  p90 %7.3f  min %7.3f  max %7.3f\n",
            names[config], 100.0 * share, values[kept / 2u], values[kept / 10u], values[(kept * 9u) / 10u],
            values[0], values[kept - 1u]);
    }
    free(samples);
    free(scene.instances);
    btbh_gpu_texture_destroy(scene.target);
    btbh_gpu_custom_mesh_destroy(cube);
    btbh_gpu_window_destroy(scene.window);
    btbh_gpu_device_shutdown(device);
    btbh_gpu_device_destroy(device);
    return 0;
}
