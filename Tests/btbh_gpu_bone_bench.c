/* A measurement, not a test (0.16.0): the CPU the scene's bone palettes take (btbh_gpu_scene_prepare_bones) for an
 * army of skinned bodies, drawn as Be The Bullet Hell v2 draws its enemies.
 *
 * argv[1] is a skinned glTF (default Tests/Fixtures/skinned-quad.gltf), loaded as a model and as its clips. The army:
 * BTBH_BONE_BENCH_INSTANCES bodies (default 300) in buckets of (clip, rate, phase), as v2's bodies.rs groups them: the
 * first BTBH_BONE_BENCH_CLIPS clips (default 3), 8 playback rates (0.5 to 2.52) and 3 phases, so up to 72 buckets, one
 * batch (one pose) each, its bodies its instances. A bucket's time is (now * rate + phase / 3 * length) mod length.
 *
 * Modes (argv[2], default all three), each on a window of its own, BTBH_BONE_BENCH_FRAMES frames (default 300) after 30
 * to warm up:
 * - moving: now advances 1/60 s a frame, so every pose is new every frame;
 * - held: now stays (a paused game; v2's frozen bodies, corpses on its death grid): every pose comes back;
 * - split: moving, each frame's scene rendered into a target first (a second view), then presented: the present
 *   samples what the target's render sampled.
 * Output per mode: the median, 10th and 90th percentiles over the frames of the present's
 * cpu_prepare_bone_sample_milliseconds (the palettes) and cpu_prepare_bones_milliseconds (the bones step as the scene
 * telemetry times it, its shader checks included), and the samples, bone matrices and (0.16.0 on) palettes copied from
 * the kept ones a frame. It builds against 0.15.0's headers too, for a before and after. With no device it is skipped
 * (77). */
#include "btbh_gpu/btbh_gpu.h"
#include "btbh_gpu_internal.h"
#include "btbh_gpu_package_internal.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_timer.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BENCH_SKIP 77
#define INSTANCE_FLOATS 24u
#define RATE_COUNT 8u
#define PHASE_COUNT 3u
#define WARM_FRAMES 30u
#define RENDER_WIDTH 480u
#define RENDER_HEIGHT 270u

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

/* v2's bodies.rs RATE_STEPS. */
static const float rates[RATE_COUNT] = {0.5f, 0.63f, 0.79f, 1.0f, 1.26f, 1.59f, 2.0f, 2.52f};

typedef struct Bench {
    BTBHGPUWindow *window;
    BTBHGPUModel *model;
    BTBHGPUAnimationLibrary *animation;
    BTBHGPUTexture *frame;
    BTBHGPUTexture *view;
    float bounds_min[3];
    float bounds_max[3];
    uint32_t clip_count;
    float clip_lengths[16];
} Bench;

typedef struct Samples {
    double *sample_milliseconds;
    double *bones_milliseconds;
    double *count_milliseconds;
    double *allocate_milliseconds;
    uint64_t animation_samples;
    uint64_t bone_matrices;
    uint64_t palette_hits;
    uint32_t count;
} Samples;

static uint32_t env_count(const char *name, uint32_t fallback) {
    const char *value = SDL_getenv(name);
    if (value == NULL || value[0] == '\0') {
        return fallback;
    }
    const long parsed = strtol(value, NULL, 10);
    return parsed > 0 ? (uint32_t)parsed : fallback;
}

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

typedef struct Camera {
    float view_projection[16];
    float right[3];
    float up[3];
    float position[3];
} Camera;

static Camera look_at(const float eye[3], const float centre[3], float far_plane) {
    Camera camera;
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
    const float near_plane = 0.1f;
    const float h = 1.0f / tanf(0.5f * 0.9f);
    const float aspect = (float)RENDER_WIDTH / (float)RENDER_HEIGHT;
    const float r = far_plane / (near_plane - far_plane);
    const float projection[16] = {
        h / aspect, 0.0f, 0.0f, 0.0f,
        0.0f, h, 0.0f, 0.0f,
        0.0f, 0.0f, r, -1.0f,
        0.0f, 0.0f, r * near_plane, 0.0f,
    };
    multiply(projection, view, camera.view_projection);
    memcpy(camera.right, side, sizeof(side));
    memcpy(camera.up, up, sizeof(up));
    memcpy(camera.position, eye, sizeof(camera.position));
    return camera;
}

static int compare_doubles(const void *a, const void *b) {
    const double x = *(const double *)a;
    const double y = *(const double *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

/* The value at `fraction` (0..1) of the sorted values. */
static double percentile(double *values, uint32_t count, double fraction) {
    qsort(values, count, sizeof(double), compare_doubles);
    const uint32_t index = (uint32_t)(fraction * (double)(count - 1u) + 0.5);
    return values[index];
}

static bool load_shader_paths(void) {
    for (int index = 0; index < 11; ++index) {
        (void)snprintf(shader_paths[index], sizeof(shader_paths[index]), "%s/%s%s", BTBH_GPU_SMOKE_SHADER_DIR,
            shader_names[index], BENCH_SHADER_EXTENSION);
        FILE *file = fopen(shader_paths[index], "rb");
        if (file == NULL) {
            fprintf(stderr, "skipped: no shader %s\n", shader_paths[index]);
            return false;
        }
        fclose(file);
    }
    return true;
}

static bool open_bench(BTBHGPUDevice *device, const char *path, Bench *bench) {
    memset(bench, 0, sizeof(*bench));
    const BTBHGPUWindowDescriptor descriptor = {
        .title = "BTBH bone bench",
        .width = RENDER_WIDTH,
        .height = RENDER_HEIGHT,
        .hidden = 1,
        .high_pixel_density = 0,
    };
    BTBHGPUWindowReport report;
    memset(&report, 0, sizeof(report));
    bench->window = btbh_gpu_window_create(&descriptor, &report);
    if (bench->window == NULL || btbh_gpu_window_claim(device, bench->window, &report) != BTBH_GPU_OK) {
        fprintf(stderr, "window: %s\n", report.message);
        return false;
    }
    BTBHGPUModelReport model_report;
    memset(&model_report, 0, sizeof(model_report));
    bench->model = btbh_gpu_window_model_load(bench->window, path, &model_report);
    if (bench->model == NULL) {
        fprintf(stderr, "%s: %s\n", path, model_report.message);
        return false;
    }
    bench->bounds_min[0] = model_report.minimum_x;
    bench->bounds_min[1] = model_report.minimum_y;
    bench->bounds_min[2] = model_report.minimum_z;
    bench->bounds_max[0] = model_report.maximum_x;
    bench->bounds_max[1] = model_report.maximum_y;
    bench->bounds_max[2] = model_report.maximum_z;
    BTBHGPUAnimationLibraryReport animation_report;
    memset(&animation_report, 0, sizeof(animation_report));
    bench->animation = btbh_gpu_animation_library_load(path, &animation_report);
    if (bench->animation == NULL) {
        fprintf(stderr, "%s clips: %s\n", path, animation_report.message);
        return false;
    }
    bench->clip_count = animation_report.animation_count;
    const uint32_t wanted = env_count("BTBH_BONE_BENCH_CLIPS", 3u);
    if (bench->clip_count > wanted) {
        bench->clip_count = wanted;
    }
    if (bench->clip_count > 16u) {
        bench->clip_count = 16u;
    }
    for (uint32_t clip = 0u; clip < bench->clip_count; ++clip) {
        const float length = btbh_gpu_animation_library_duration(bench->animation, clip);
        bench->clip_lengths[clip] = length > 0.0f ? length : 1.0f;
    }
    BTBHGPUTextureReport texture_report;
    memset(&texture_report, 0, sizeof(texture_report));
    bench->frame = btbh_gpu_window_render_target_create(bench->window, RENDER_WIDTH, RENDER_HEIGHT,
        BTBH_GPU_TEXTURE_FILTER_LINEAR, &texture_report);
    bench->view = btbh_gpu_window_render_target_create(bench->window, RENDER_WIDTH, RENDER_HEIGHT,
        BTBH_GPU_TEXTURE_FILTER_LINEAR, &texture_report);
    return bench->frame != NULL && bench->view != NULL;
}

static void close_bench(Bench *bench) {
    if (bench->animation != NULL) {
        btbh_gpu_animation_library_destroy(bench->animation);
    }
    if (bench->window != NULL) {
        btbh_gpu_window_destroy(bench->window);
    }
    memset(bench, 0, sizeof(*bench));
}

/* The army at `now`: one batch a bucket, its bodies on a grid. Returns the batch count. */
static uint32_t army(
    const Bench *bench,
    float now,
    uint32_t body_count,
    float *instances,
    BTBHGPUSceneGeometryBatch *batches
) {
    const uint32_t bucket_count = bench->clip_count * RATE_COUNT * PHASE_COUNT;
    const float size[3] = {
        bench->bounds_max[0] - bench->bounds_min[0],
        bench->bounds_max[1] - bench->bounds_min[1],
        bench->bounds_max[2] - bench->bounds_min[2],
    };
    const float spacing = 1.2f * fmaxf(fmaxf(size[0], size[2]), 0.01f);
    const uint32_t columns = (uint32_t)ceilf(sqrtf((float)body_count));
    for (uint32_t body = 0u; body < body_count; ++body) {
        float *record = &instances[(size_t)body * INSTANCE_FLOATS];
        memset(record, 0, INSTANCE_FLOATS * sizeof(float));
        record[0] = 1.0f;
        record[5] = 1.0f;
        record[10] = 1.0f;
        record[12] = ((float)(body % columns) - 0.5f * (float)columns) * spacing;
        record[14] = ((float)(body / columns) - 0.5f * (float)columns) * spacing;
        record[15] = 1.0f;
        record[16] = 0.8f;
        record[17] = 0.7f;
        record[18] = 0.6f;
        record[19] = 1.0f;
    }
    uint32_t batch_count = 0u;
    uint32_t first = 0u;
    for (uint32_t bucket = 0u; bucket < bucket_count && first < body_count; ++bucket) {
        const uint32_t clip = bucket / (RATE_COUNT * PHASE_COUNT);
        const uint32_t rate = (bucket / PHASE_COUNT) % RATE_COUNT;
        const uint32_t phase = bucket % PHASE_COUNT;
        /* The bodies spread over the buckets, the first ones taking the remainder. */
        const uint32_t count = body_count / bucket_count + (bucket < body_count % bucket_count ? 1u : 0u);
        if (count == 0u) {
            continue;
        }
        const float length = bench->clip_lengths[clip];
        const float time = fmodf(now * rates[rate] + (float)phase / (float)PHASE_COUNT * length, length);
        BTBHGPUSceneGeometryBatch *batch = &batches[batch_count++];
        memset(batch, 0, sizeof(*batch));
        batch->kind = BTBH_GPU_SCENE_GEOMETRY_MODEL;
        batch->model = bench->model;
        batch->animation_library = bench->animation;
        batch->animation_index = clip;
        batch->animation_time = time;
        batch->material_slot = -1;
        batch->first_instance = first;
        batch->instance_count = count;
        batch->albedo_red = 255u;
        batch->albedo_green = 255u;
        batch->albedo_blue = 255u;
        batch->albedo_alpha = 255u;
        batch->uv_scale_x = 1.0f;
        batch->uv_scale_y = 1.0f;
        batch->roughness = 0.6f;
        batch->occlusion = 1.0f;
        batch->depth_compare = BTBH_GPU_SCENE_DEPTH_LESS;
        batch->cull_mode = BTBH_GPU_SCENE_CULL_BACK;
        batch->stencil_pass = BTBH_GPU_SCENE_STENCIL_REPLACE;
        first += count;
    }
    return batch_count;
}

static BTBHGPUSceneLighting bench_lighting(void) {
    BTBHGPUSceneLighting lighting;
    memset(&lighting, 0, sizeof(lighting));
    lighting.ambient_red = 1.0f;
    lighting.ambient_green = 1.0f;
    lighting.ambient_blue = 1.0f;
    lighting.ambient_energy = 0.4f;
    lighting.directional_energy = 2.0f;
    lighting.directional_red = 1.0f;
    lighting.directional_green = 1.0f;
    lighting.directional_blue = 1.0f;
    lighting.directional_x = -0.4f;
    lighting.directional_y = -1.0f;
    lighting.directional_z = -0.3f;
    lighting.spot_range = 1.0f;
    lighting.spot_falloff = 1.0f;
    lighting.spot_inner_cutoff = 1.0f;
    return lighting;
}

static BTBHGPUScenePost bench_post(void) {
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

/* One frame: (split) the scene rendered into the view target, then presented and copied, the GPU waited for. The
   present's telemetry goes into `samples` when `measured`. */
static bool run_frame(
    Bench *bench,
    float now,
    uint32_t body_count,
    bool split,
    float *instances,
    BTBHGPUSceneGeometryBatch *batches,
    Samples *samples,
    bool measured
) {
    const uint32_t batch_count = army(bench, now, body_count, instances, batches);
    const float extent = sqrtf((float)body_count) *
        1.2f * fmaxf(fmaxf(bench->bounds_max[0] - bench->bounds_min[0], bench->bounds_max[2] - bench->bounds_min[2]),
            0.01f);
    const float eye[3] = {0.0f, 0.9f * extent + 1.0f, 0.9f * extent + 1.0f};
    const float centre[3] = {0.0f, 0.0f, 0.0f};
    const Camera camera = look_at(eye, centre, 4.0f * extent + 10.0f);
    const BTBHGPUSceneLighting lighting = bench_lighting();
    const BTBHGPUScenePost post = bench_post();
    BTBHGPUSceneReport report;
    memset(&report, 0, sizeof(report));
    if (split) {
        const BTBHGPUResult result = btbh_gpu_window_scene_render_to_target(
            bench->window, bench->view,
            shader_paths[0], shader_paths[1], shader_paths[2], shader_paths[3], shader_paths[4], shader_paths[5],
            shader_paths[6], shader_paths[7], shader_paths[8], shader_paths[9], shader_paths[10],
            BENCH_SHADER_FORMAT, RENDER_WIDTH, RENDER_HEIGHT, camera.view_projection, camera.right, camera.up,
            camera.position, &lighting, &post, instances, body_count, body_count, batches, NULL, NULL, batch_count,
            30u, 40u, 50u, 255u, &report);
        if (result != BTBH_GPU_OK) {
            fprintf(stderr, "render to target: %s\n", report.message);
            return false;
        }
    }
    const BTBHGPUResult result = btbh_gpu_window_scene_present_instances(
        bench->window,
        shader_paths[0], shader_paths[1], shader_paths[2], shader_paths[3], shader_paths[4], shader_paths[5],
        shader_paths[6], shader_paths[7], shader_paths[8], shader_paths[9], shader_paths[10],
        BENCH_SHADER_FORMAT, RENDER_WIDTH, RENDER_HEIGHT, camera.view_projection, camera.right, camera.up,
        camera.position, &lighting, &post, instances, body_count, body_count, batches, batch_count,
        30u, 40u, 50u, 255u, &report);
    if (result != BTBH_GPU_OK) {
        fprintf(stderr, "present: %s\n", report.message);
        return false;
    }
    BTBHGPUSceneTelemetryReport telemetry;
    memset(&telemetry, 0, sizeof(telemetry));
    (void)btbh_gpu_window_scene_read_telemetry(bench->window, &telemetry);
    BTBHGPUTextureReport copied;
    memset(&copied, 0, sizeof(copied));
    if (btbh_gpu_window_copy_pending_scene_texture(bench->window, bench->frame, &copied) != BTBH_GPU_OK) {
        fprintf(stderr, "copy: %s\n", copied.message);
        return false;
    }
    (void)SDL_WaitForGPUIdle(bench->window->device->gpu);
    if (measured) {
        const uint32_t index = samples->count++;
        samples->sample_milliseconds[index] = telemetry.cpu_prepare_bone_sample_milliseconds;
        samples->bones_milliseconds[index] = telemetry.cpu_prepare_bones_milliseconds;
        samples->count_milliseconds[index] = telemetry.cpu_prepare_bone_count_milliseconds;
        samples->allocate_milliseconds[index] = telemetry.cpu_prepare_bone_allocate_milliseconds;
        samples->animation_samples += telemetry.animation_sample_count;
        samples->bone_matrices += telemetry.bone_matrix_count;
#if defined(BTBH_GPU_HAS_BONE_PALETTE_CACHE)
        samples->palette_hits += telemetry.animation_palette_cache_hit_count;
#endif
    }
    return true;
}

static int run_mode(BTBHGPUDevice *device, const char *path, const char *mode, uint32_t body_count, uint32_t frames) {
    const bool held = strcmp(mode, "held") == 0;
    const bool split = strcmp(mode, "split") == 0;
    if (!held && !split && strcmp(mode, "moving") != 0) {
        fprintf(stderr, "mode: moving, held or split\n");
        return 2;
    }
    Bench bench;
    if (!open_bench(device, path, &bench)) {
        close_bench(&bench);
        return 1;
    }
    btbh_gpu_window_set_frame_telemetry_enabled(bench.window, 1u);
    float *instances = calloc((size_t)body_count * INSTANCE_FLOATS, sizeof(float));
    BTBHGPUSceneGeometryBatch *batches =
        calloc((size_t)bench.clip_count * RATE_COUNT * PHASE_COUNT, sizeof(*batches));
    Samples samples;
    memset(&samples, 0, sizeof(samples));
    samples.sample_milliseconds = calloc(frames, sizeof(double));
    samples.bones_milliseconds = calloc(frames, sizeof(double));
    samples.count_milliseconds = calloc(frames, sizeof(double));
    samples.allocate_milliseconds = calloc(frames, sizeof(double));
    if (instances == NULL || batches == NULL || samples.sample_milliseconds == NULL ||
        samples.bones_milliseconds == NULL || samples.count_milliseconds == NULL ||
        samples.allocate_milliseconds == NULL) {
        return 1;
    }
    bool ok = true;
    for (uint32_t frame = 0u; ok && frame < WARM_FRAMES + frames; ++frame) {
        const float now = held ? 1.0f : 1.0f + (float)frame / 60.0f;
        ok = run_frame(&bench, now, body_count, split, instances, batches, &samples, frame >= WARM_FRAMES);
    }
    if (ok) {
        const uint32_t count = samples.count;
        const double sample_p50 = percentile(samples.sample_milliseconds, count, 0.5);
        const double sample_p10 = percentile(samples.sample_milliseconds, count, 0.1);
        const double sample_p90 = percentile(samples.sample_milliseconds, count, 0.9);
        const double bones_p50 = percentile(samples.bones_milliseconds, count, 0.5);
        const double bones_p10 = percentile(samples.bones_milliseconds, count, 0.1);
        const double bones_p90 = percentile(samples.bones_milliseconds, count, 0.9);
        const double count_p50 = percentile(samples.count_milliseconds, count, 0.5);
        const double allocate_p50 = percentile(samples.allocate_milliseconds, count, 0.5);
        printf("%-7s %4u bodies %3u poses/frame: palettes %.4f ms (p10 %.4f, p90 %.4f) | bones step %.4f ms (p10 "
            "%.4f, p90 %.4f) | count %.4f, allocate %.4f | %.1f samples, %.0f matrices, %.1f kept a frame\n",
            mode, body_count, bench.clip_count * RATE_COUNT * PHASE_COUNT, sample_p50, sample_p10, sample_p90,
            bones_p50, bones_p10, bones_p90, count_p50, allocate_p50,
            (double)samples.animation_samples / (double)count, (double)samples.bone_matrices / (double)count,
            (double)samples.palette_hits / (double)count);
    }
    free(samples.allocate_milliseconds);
    free(samples.count_milliseconds);
    free(samples.bones_milliseconds);
    free(samples.sample_milliseconds);
    free(batches);
    free(instances);
    close_bench(&bench);
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : BTBH_GPU_SMOKE_FIXTURE_DIR "/skinned-quad.gltf";
    if (!load_shader_paths()) {
        return BENCH_SKIP;
    }
    const BTBHGPUDeviceDescriptor device_descriptor = {
        .backend = BENCH_BACKEND,
        .required_features = BTBH_GPU_REQUIRED_FEATURES,
        .debug_mode = 0u,
    };
    BTBHGPUDeviceReport device_report;
    memset(&device_report, 0, sizeof(device_report));
    BTBHGPUDevice *device = btbh_gpu_device_create(&device_descriptor, &device_report);
    if (device == NULL) {
        fprintf(stderr, "skipped: no device (%s)\n", device_report.message);
        return BENCH_SKIP;
    }
    const uint32_t body_count = env_count("BTBH_BONE_BENCH_INSTANCES", 300u);
    const uint32_t frames = env_count("BTBH_BONE_BENCH_FRAMES", 300u);
    printf("%s\n", path);
    int status = 0;
    if (argc > 2) {
        status = run_mode(device, path, argv[2], body_count, frames);
    } else {
        static const char *const modes[3] = {"moving", "held", "split"};
        for (int index = 0; status == 0 && index < 3; ++index) {
            status = run_mode(device, path, modes[index], body_count, frames);
        }
    }
    btbh_gpu_device_destroy(device);
    return status;
}
