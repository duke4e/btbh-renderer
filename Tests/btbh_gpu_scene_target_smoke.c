/* A scene rendered into a render target (btbh_gpu_window_scene_render_to_target) beside a window's
 * FSR 3 frames: the window's temporal history must not notice it.
 *
 * Two hidden windows render the same world frames with FSR 3 (a lit cube, a moving camera, a
 * directional shadow updated every other frame). Window B also renders portraits into a small target
 * between two of them.
 * The world frame after it must be byte-identical in both windows: the portrait neither reset nor
 * fed the history. Then window B renders a scene aside the old way (a present with FSR 1 and a copy),
 * and the next world frame must differ (the history started again): the check can tell. The test
 * also reads the portrait back (a cube on the clear colour, at the target's size), and checks the
 * refusals (another window's texture, a texture that is no render target, a scene waiting for its
 * canvas, which must survive the refusal). A second portrait with another sun, rendered in B after the
 * first ones and in A as A's first, must be byte-identical: no shadow map is kept from one portrait to
 * the next, whatever the shadow update periods.
 *
 * It needs a GPU and the production shaders; with no device it is skipped (77). */
#undef NDEBUG

#include "btbh_gpu/btbh_gpu.h"
#include "btbh_gpu_internal.h"
#include "btbh_gpu_package_internal.h"

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_timer.h>

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SMOKE_SKIP 77
#define WORLD_WIDTH 320u
#define WORLD_HEIGHT 180u
#define WORLD_RENDER_WIDTH 160u
#define WORLD_RENDER_HEIGHT 90u
#define PORTRAIT_SIZE 64u

#if defined(__APPLE__)
#define SMOKE_SHADER_EXTENSION ".metallib"
#define SMOKE_SHADER_FORMAT BTBH_GPU_SHADER_FORMAT_METALLIB
#define SMOKE_BACKEND BTBH_GPU_BACKEND_METAL
#else
#define SMOKE_SHADER_EXTENSION ".spv"
#define SMOKE_SHADER_FORMAT BTBH_GPU_SHADER_FORMAT_SPIRV
#define SMOKE_BACKEND BTBH_GPU_BACKEND_VULKAN
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

typedef struct SmokeWindow {
    BTBHGPUWindow *window;
    BTBHGPUCustomMesh *cube;
    /* Where each world frame is copied (the frame's scene is finished by the copy, not a canvas). */
    BTBHGPUTexture *frame;
    BTBHGPUTexture *portrait;
} SmokeWindow;

/* Column-major 4x4 helpers in the renderer's convention (right-handed, depth 0..1). */
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

typedef struct SmokeCamera {
    float view_projection[16];
    float right[3];
    float up[3];
    float position[3];
    float near_plane;
    float far_plane;
    float fov_y;
} SmokeCamera;

static SmokeCamera look_at(
    const float eye[3],
    const float centre[3],
    float fov_y,
    float aspect,
    const float projection_override[16]
) {
    SmokeCamera camera;
    memset(&camera, 0, sizeof(camera));
    camera.near_plane = 0.1f;
    camera.far_plane = 50.0f;
    camera.fov_y = fov_y;
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
        const float h = 1.0f / tanf(0.5f * fov_y);
        const float r = camera.far_plane / (camera.near_plane - camera.far_plane);
        const float perspective[16] = {
            h / aspect, 0.0f, 0.0f, 0.0f,
            0.0f, h, 0.0f, 0.0f,
            0.0f, 0.0f, r, -1.0f,
            0.0f, 0.0f, r * camera.near_plane, 0.0f,
        };
        memcpy(projection, perspective, sizeof(projection));
    }
    multiply(projection, view, camera.view_projection);
    memcpy(camera.right, side, sizeof(side));
    memcpy(camera.up, up, sizeof(up));
    memcpy(camera.position, eye, sizeof(camera.position));
    return camera;
}

/* A sun from above and to the side (from `sun`), its shadow covering the cube and the ground. */
static BTBHGPUSceneLighting smoke_lighting_from(const float sun[3]) {
    BTBHGPUSceneLighting lighting;
    memset(&lighting, 0, sizeof(lighting));
    lighting.ambient_red = 1.0f;
    lighting.ambient_green = 1.0f;
    lighting.ambient_blue = 1.0f;
    lighting.ambient_energy = 0.25f;
    lighting.directional_energy = 2.0f;
    lighting.directional_red = 1.0f;
    lighting.directional_green = 0.95f;
    lighting.directional_blue = 0.9f;
    lighting.spot_range = 8.0f;
    lighting.spot_falloff = 1.0f;
    lighting.spot_inner_cutoff = 1.0f;
    lighting.spot_outer_cutoff = 0.0f;
    lighting.directional_x = -sun[0];
    lighting.directional_y = -sun[1];
    lighting.directional_z = -sun[2];
    const float centre[3] = {0.0f, 0.0f, 0.0f};
    const float rcp = 1.0f / 8.0f;
    const float range = 1.0f / (1.0f - 30.0f);
    const float orthographic[16] = {
        rcp, 0.0f, 0.0f, 0.0f,
        0.0f, rcp, 0.0f, 0.0f,
        0.0f, 0.0f, range, 0.0f,
        0.0f, 0.0f, range * 1.0f, 1.0f,
    };
    const SmokeCamera light = look_at(sun, centre, 1.0f, 1.0f, orthographic);
    memcpy(
        lighting.directional_shadow_view_projection,
        light.view_projection,
        sizeof(lighting.directional_shadow_view_projection)
    );
    lighting.directional_shadow_enabled = 1u;
    lighting.directional_shadow_softness = 1.0f;
    return lighting;
}

static BTBHGPUSceneLighting smoke_lighting(void) {
    const float sun[3] = {4.0f, 10.0f, 3.0f};
    return smoke_lighting_from(sun);
}

static BTBHGPUScenePost smoke_post(const SmokeCamera *camera, uint8_t reset) {
    BTBHGPUScenePost post;
    memset(&post, 0, sizeof(post));
    post.tonemap_white = 1.0f;
    post.exposure = 1.0f;
    post.contrast = 1.0f;
    post.saturation = 1.0f;
    post.blur_curve = 1.0f;
    post.tonemap_mode = BTBH_GPU_SCENE_TONEMAP_ACES;
    post.upscale_mode = BTBH_GPU_SCENE_UPSCALE_FSR3;
    post.temporal_camera_near = camera->near_plane;
    post.temporal_camera_far = camera->far_plane;
    post.temporal_vertical_fov = camera->fov_y;
    post.temporal_frame_seconds = 1.0f / 60.0f;
    post.temporal_meters_per_unit = 1.0f;
    post.temporal_reactive_scale = 0.3f;
    post.temporal_reset = reset;
    return post;
}

/* Instances: 16 floats of transform (column-major), colour, custom. */
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

static BTBHGPUSceneGeometryBatch cube_batch(BTBHGPUCustomMesh *cube, uint32_t first, uint32_t count) {
    BTBHGPUSceneGeometryBatch batch;
    memset(&batch, 0, sizeof(batch));
    batch.kind = BTBH_GPU_SCENE_GEOMETRY_CUSTOM_MESH;
    batch.custom_mesh = cube;
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
    batch.cull_mode = BTBH_GPU_SCENE_CULL_NONE;
    /* No stencil: compare always, replace with reference 0. */
    batch.stencil_pass = BTBH_GPU_SCENE_STENCIL_REPLACE;
    batch.spot_shadow_visible = 1u;
    return batch;
}

/* A unit cube (-0.5..0.5), 24 floats a vertex: position, normal, uv, tangent, colour, joints, weights. */
static BTBHGPUCustomMesh *create_cube(BTBHGPUWindow *window) {
    static const float faces[6][3] = {
        {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
    };
    float vertices[24][24];
    uint32_t indices[36];
    memset(vertices, 0, sizeof(vertices));
    for (uint32_t face = 0u; face < 6u; ++face) {
        const float *n = faces[face];
        float u[3] = {n[1], n[2], n[0]};
        float v[3];
        cross3(n, u, v);
        for (uint32_t corner = 0u; corner < 4u; ++corner) {
            const float su = (corner == 1u || corner == 2u) ? 0.5f : -0.5f;
            const float sv = corner >= 2u ? 0.5f : -0.5f;
            float *vertex = vertices[face * 4u + corner];
            /* Floats: position 0-2, normal 3-5, uv 6-7, tangent 8-11, colour 12-15, joints 16-19,
               weights 20-23 (the renderer's 96-byte model vertex). */
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
        const uint32_t quad[6] = {base, base + 1u, base + 2u, base, base + 2u, base + 3u};
        memcpy(&indices[face * 6u], quad, sizeof(quad));
    }
    BTBHGPUCustomMeshReport report;
    memset(&report, 0, sizeof(report));
    BTBHGPUCustomMesh *mesh = btbh_gpu_window_custom_mesh_create(
        window,
        vertices,
        (uint32_t)sizeof(vertices),
        24u,
        indices,
        36u,
        &report
    );
    if (mesh == NULL) {
        fprintf(stderr, "cube: %s\n", report.message);
    }
    assert(mesh != NULL);
    return mesh;
}

/* The texture's bytes (4 a texel), read back through SDL_GPU. */
static uint8_t *read_texture(BTBHGPUWindow *window, BTBHGPUTexture *texture) {
    SDL_GPUDevice *device = window->device->gpu;
    const uint32_t texel = SDL_GPUTextureFormatTexelBlockSize(texture->format);
    assert(texel == 4u);
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
    const void *mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
    assert(mapped != NULL);
    memcpy(pixels, mapped, size);
    SDL_UnmapGPUTransferBuffer(device, transfer);
    SDL_ReleaseGPUTransferBuffer(device, transfer);
    return pixels;
}

static int texel_distance(const uint8_t *pixels, uint32_t width, uint32_t ax, uint32_t ay, uint32_t bx, uint32_t by) {
    const uint8_t *a = &pixels[(ay * width + ax) * 4u];
    const uint8_t *b = &pixels[(by * width + bx) * 4u];
    int distance = 0;
    for (int channel = 0; channel < 3; ++channel) {
        distance += abs((int)a[channel] - (int)b[channel]);
    }
    return distance;
}

/* World frame `index`: the camera circles a little each frame; the world is the cube on a ground slab. */
static SmokeCamera world_camera(uint32_t index) {
    const float angle = 0.6f + 0.02f * (float)index;
    const float eye[3] = {6.0f * sinf(angle), 3.0f, 6.0f * cosf(angle)};
    const float centre[3] = {0.0f, 0.3f, 0.0f};
    return look_at(eye, centre, 0.8f, (float)WORLD_WIDTH / (float)WORLD_HEIGHT, NULL);
}

static BTBHGPUResult present_world(
    SmokeWindow *smoke,
    uint32_t index,
    uint8_t upscale_mode,
    BTBHGPUSceneReport *report
) {
    const SmokeCamera camera = world_camera(index);
    const BTBHGPUSceneLighting lighting = smoke_lighting();
    BTBHGPUScenePost post = smoke_post(&camera, index == 0u ? 1u : 0u);
    post.upscale_mode = upscale_mode;
    const float grey[4] = {0.55f, 0.6f, 0.5f, 1.0f};
    const float red[4] = {0.9f, 0.25f, 0.2f, 1.0f};
    float instances[2][24];
    instance(instances[0], 0.0f, -0.55f, 0.0f, 8.0f, 0.1f, 8.0f, grey);
    instance(instances[1], 0.0f, 0.5f, 0.0f, 1.0f, 1.0f, 1.0f, red);
    const BTBHGPUSceneGeometryBatch batch = cube_batch(smoke->cube, 0u, 2u);
    return btbh_gpu_window_scene_present_instances(
        smoke->window,
        shader_paths[0], shader_paths[1], shader_paths[2], shader_paths[3],
        shader_paths[4], shader_paths[5], shader_paths[6], shader_paths[7],
        shader_paths[8], shader_paths[9], shader_paths[10],
        SMOKE_SHADER_FORMAT,
        WORLD_RENDER_WIDTH,
        WORLD_RENDER_HEIGHT,
        camera.view_projection,
        camera.right,
        camera.up,
        camera.position,
        &lighting,
        &post,
        &instances[0][0],
        2u,
        2u,
        &batch,
        1u,
        40u, 60u, 90u, 255u,
        report
    );
}

/* A world frame with FSR 3, finished by a copy into the window's frame texture. */
static void world_frame(SmokeWindow *smoke, uint32_t index, uint8_t *expected_upscale) {
    BTBHGPUSceneReport report;
    const BTBHGPUResult result = present_world(smoke, index, BTBH_GPU_SCENE_UPSCALE_FSR3, &report);
    if (result != BTBH_GPU_OK) {
        fprintf(stderr, "world frame %u: %s\n", index, report.message);
    }
    assert(result == BTBH_GPU_OK);
    assert(report.drawable_width == WORLD_WIDTH && report.drawable_height == WORLD_HEIGHT);
    *expected_upscale = report.upscale_mode;
    BTBHGPUTextureReport copied;
    assert(btbh_gpu_window_copy_pending_scene_texture(smoke->window, smoke->frame, &copied) == BTBH_GPU_OK);
}

/* A portrait, on a dark clear colour, from another side; FSR 3 asked for (it must run as FSR 1, or
   bilinearly when not smaller than the target). Variant 0: the cube alone, close. Variant 1: the cube
   moved, on a slab, under another sun, so a shadow map kept from variant 0 would show. Variant 2:
   variant 1 without the shadow (to show that the shadow shows). */
static BTBHGPUResult render_portrait(
    SmokeWindow *smoke,
    BTBHGPUTexture *target,
    uint32_t render_size,
    uint32_t variant,
    BTBHGPUSceneReport *report
) {
    const float close_eye[3] = {-1.6f, 1.4f, -2.2f};
    const float far_eye[3] = {-4.0f, 4.0f, -5.0f};
    const float centre[3] = {0.0f, 0.5f, 0.0f};
    const SmokeCamera camera = look_at(variant == 0u ? close_eye : far_eye, centre, 0.6f, 1.0f, NULL);
    const float other_sun[3] = {-10.0f, 3.0f, 1.0f};
    BTBHGPUSceneLighting lighting =
        variant == 0u ? smoke_lighting() : smoke_lighting_from(other_sun);
    if (variant == 2u) {
        lighting.directional_shadow_enabled = 0u;
    }
    BTBHGPUScenePost post = smoke_post(&camera, 0u);
    post.temporal_reactive_scale = -1.0f; /* ignored: never temporal */
    const float gold[4] = {1.0f, 0.8f, 0.3f, 1.0f};
    const float slate[4] = {0.5f, 0.55f, 0.6f, 1.0f};
    float cube[2][24];
    /* (Variant 1 floats the cube over the slab: its shadow lies on the slab beside it.) */
    instance(cube[0], variant == 0u ? 0.0f : 0.3f, variant == 0u ? 0.5f : 1.4f, variant == 0u ? 0.0f : -0.2f, 1.0f, 1.0f, 1.0f, gold);
    instance(cube[1], 0.0f, -0.05f, 0.0f, 4.0f, 0.1f, 4.0f, slate);
    const BTBHGPUSceneGeometryBatch batch = cube_batch(smoke->cube, 0u, variant == 0u ? 1u : 2u);
    return btbh_gpu_window_scene_render_to_target(
        smoke->window,
        target,
        shader_paths[0], shader_paths[1], shader_paths[2], shader_paths[3],
        shader_paths[4], shader_paths[5], shader_paths[6], shader_paths[7],
        shader_paths[8], shader_paths[9], shader_paths[10],
        SMOKE_SHADER_FORMAT,
        render_size,
        render_size,
        camera.view_projection,
        camera.right,
        camera.up,
        camera.position,
        &lighting,
        &post,
        &cube[0][0],
        2u,
        2u,
        &batch,
        NULL,
        NULL,
        1u,
        12u, 14u, 20u, 255u,
        report
    );
}

static SmokeWindow open_window(BTBHGPUDevice *device, const char *title) {
    SmokeWindow smoke;
    memset(&smoke, 0, sizeof(smoke));
    const BTBHGPUWindowDescriptor descriptor = {
        .title = title,
        .width = WORLD_WIDTH,
        .height = WORLD_HEIGHT,
        .hidden = 1,
        .high_pixel_density = 0,
    };
    BTBHGPUWindowReport report;
    memset(&report, 0, sizeof(report));
    smoke.window = btbh_gpu_window_create(&descriptor, &report);
    assert(smoke.window != NULL);
    assert(btbh_gpu_window_claim(device, smoke.window, &report) == BTBH_GPU_OK);
    smoke.cube = create_cube(smoke.window);
    /* Shadows every other scene: a portrait must still draw its own. */
    assert(btbh_gpu_window_scene_set_shadow_update_periods(smoke.window, 2u, 2u, 2u) == 1u);
    BTBHGPUTextureReport texture_report;
    smoke.frame = btbh_gpu_window_render_target_create(
        smoke.window, WORLD_WIDTH, WORLD_HEIGHT, BTBH_GPU_TEXTURE_FILTER_LINEAR, &texture_report);
    assert(smoke.frame != NULL);
    smoke.portrait = btbh_gpu_window_render_target_create(
        smoke.window, PORTRAIT_SIZE, PORTRAIT_SIZE, BTBH_GPU_TEXTURE_FILTER_LINEAR, &texture_report);
    assert(smoke.portrait != NULL);
    return smoke;
}

int main(void) {
    for (int index = 0; index < 11; ++index) {
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
    /* Hidden windows never take the focus; nothing here presents to a swapchain. */
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
    SmokeWindow a = open_window(device, "BTBH render-to-target smoke A");
    SmokeWindow b = open_window(device, "BTBH render-to-target smoke B");

    /* The same world frames in both windows; B renders portraits between frames 1 and 2. */
    uint8_t upscale_a = 0u;
    uint8_t upscale_b = 0u;
    for (uint32_t frame = 0u; frame < 2u; ++frame) {
        world_frame(&a, frame, &upscale_a);
        world_frame(&b, frame, &upscale_b);
    }
    const bool fsr3 = upscale_a == BTBH_GPU_SCENE_UPSCALE_FSR3;
    printf("world frames upscaled with %s\n", fsr3 ? "FSR 3" : "FSR 1 (FSR 3 unavailable here)");
    assert(upscale_a == upscale_b);
#if !defined(__APPLE__)
    /* Vulkan runs FSR 3; without it the history checks below would prove nothing. */
    assert(fsr3);
#else
    if (!fsr3) {
        printf("FSR 3 falls back to FSR 1 on Metal: the history checks below hold trivially\n");
    }
#endif

    BTBHGPUSceneReport report;
    /* (The first call makes the window's second runtime: its pipelines and targets.) */
    const uint64_t first_started = SDL_GetTicksNS();
    BTBHGPUResult result = render_portrait(&b, b.portrait, 2u * PORTRAIT_SIZE, 0u, &report);
    const double first_milliseconds = (double)(SDL_GetTicksNS() - first_started) / 1.0e6;
    if (result != BTBH_GPU_OK) {
        fprintf(stderr, "portrait: %s\n", report.message);
    }
    assert(result == BTBH_GPU_OK);
    assert(report.drawable_width == PORTRAIT_SIZE && report.drawable_height == PORTRAIT_SIZE);
    /* Twice the target's size: halved bilinearly (FSR 1 only enlarges). */
    assert(report.upscale_mode == BTBH_GPU_SCENE_UPSCALE_LINEAR);
    uint8_t *portrait = read_texture(b.window, b.portrait);
    const uint32_t middle = PORTRAIT_SIZE / 2u;
    const uint32_t last = PORTRAIT_SIZE - 1u;
    assert(texel_distance(portrait, PORTRAIT_SIZE, 0u, 0u, last, last) <= 3);
    assert(texel_distance(portrait, PORTRAIT_SIZE, 0u, 0u, last, 0u) <= 3);
    assert(texel_distance(portrait, PORTRAIT_SIZE, 0u, 0u, middle, middle) > 60);
    /* Smaller than the target, enlarged by FSR 1; and again at the same size. */
    result = render_portrait(&b, b.portrait, PORTRAIT_SIZE / 2u, 0u, &report);
    assert(result == BTBH_GPU_OK);
    assert(report.upscale_mode == BTBH_GPU_SCENE_UPSCALE_FSR1);
    const uint64_t later_started = SDL_GetTicksNS();
    result = render_portrait(&b, b.portrait, PORTRAIT_SIZE, 0u, &report);
    assert(result == BTBH_GPU_OK);
    assert(report.upscale_mode == BTBH_GPU_SCENE_UPSCALE_LINEAR);
    printf(
        "portrait to a target: first call %.1f ms CPU (makes its runtime), a later one %.2f ms\n",
        first_milliseconds,
        (double)(SDL_GetTicksNS() - later_started) / 1.0e6
    );

    /* Frame 2 and 3: identical in both windows (the portraits left B's history alone). */
    for (uint32_t frame = 2u; frame < 4u; ++frame) {
        world_frame(&a, frame, &upscale_a);
        world_frame(&b, frame, &upscale_b);
    }
    uint8_t *frame_a = read_texture(a.window, a.frame);
    uint8_t *frame_b = read_texture(b.window, b.frame);
    const size_t frame_bytes = (size_t)WORLD_WIDTH * WORLD_HEIGHT * 4u;
    assert(memcmp(frame_a, frame_b, frame_bytes) == 0);
    printf("world frame after the portraits: identical in both windows\n");
    if (getenv("BTBH_SMOKE_DUMP") != NULL) {
        char path[1024];
        (void)snprintf(path, sizeof(path), "%s/world.png", getenv("BTBH_SMOKE_DUMP"));
        (void)btbh_gpu_write_png_rgba8(path, WORLD_WIDTH, WORLD_HEIGHT, frame_a);
    }
    free(frame_a);
    free(frame_b);

    /* Variant 1 in B (after three portraits of variant 0) and as A's very first portrait: the same
       bytes, so B kept no shadow map from before; and not variant 0's picture. */
    result = render_portrait(&b, b.portrait, 2u * PORTRAIT_SIZE, 1u, &report);
    assert(result == BTBH_GPU_OK);
    uint8_t *second_b = read_texture(b.window, b.portrait);
    result = render_portrait(&a, a.portrait, 2u * PORTRAIT_SIZE, 1u, &report);
    assert(result == BTBH_GPU_OK);
    uint8_t *second_a = read_texture(a.window, a.portrait);
    const size_t portrait_bytes = (size_t)PORTRAIT_SIZE * PORTRAIT_SIZE * 4u;
    const char *dump = getenv("BTBH_SMOKE_DUMP");
    if (dump != NULL) {
        char path[1024];
        (void)snprintf(path, sizeof(path), "%s/portrait-0.png", dump);
        (void)btbh_gpu_write_png_rgba8(path, PORTRAIT_SIZE, PORTRAIT_SIZE, portrait);
        (void)snprintf(path, sizeof(path), "%s/portrait-1-a.png", dump);
        (void)btbh_gpu_write_png_rgba8(path, PORTRAIT_SIZE, PORTRAIT_SIZE, second_a);
        (void)snprintf(path, sizeof(path), "%s/portrait-1-b.png", dump);
        (void)btbh_gpu_write_png_rgba8(path, PORTRAIT_SIZE, PORTRAIT_SIZE, second_b);
    }
    result = render_portrait(&a, a.portrait, 2u * PORTRAIT_SIZE, 2u, &report);
    assert(result == BTBH_GPU_OK);
    uint8_t *unshadowed = read_texture(a.window, a.portrait);
    assert(memcmp(unshadowed, second_a, portrait_bytes) != 0);
    free(unshadowed);
    assert(memcmp(portrait, second_b, portrait_bytes) != 0);
    assert(memcmp(second_a, second_b, portrait_bytes) == 0);
    printf("a second portrait: the same after earlier ones as on its own\n");
    free(portrait);
    free(second_a);
    free(second_b);

    /* The old way in B: a scene aside presented with FSR 1 and copied. B's next frame starts its
       history again, so it differs from A's (under FSR 3). */
    result = present_world(&b, 9u, BTBH_GPU_SCENE_UPSCALE_FSR1, &report);
    assert(result == BTBH_GPU_OK);
    BTBHGPUTextureReport copied;
    assert(btbh_gpu_window_copy_pending_scene_texture(b.window, b.frame, &copied) == BTBH_GPU_OK);
    world_frame(&a, 4u, &upscale_a);
    world_frame(&b, 4u, &upscale_b);
    frame_a = read_texture(a.window, a.frame);
    frame_b = read_texture(b.window, b.frame);
    if (fsr3) {
        assert(memcmp(frame_a, frame_b, frame_bytes) != 0);
        printf("world frame after a scene presented aside: differs (history restarted), as expected\n");
    }
    free(frame_a);
    free(frame_b);

    /* Refusals. Another window's render target; a texture that is no render target. */
    result = render_portrait(&b, a.portrait, PORTRAIT_SIZE, 0u, &report);
    assert(result == BTBH_GPU_INVALID_ARGUMENT);
    const uint8_t white[4] = {255u, 255u, 255u, 255u};
    BTBHGPUTextureReport texture_report;
    BTBHGPUTexture *plain = btbh_gpu_window_texture_create_rgba8_ex(
        b.window, white, 1u, 1u, 0u, BTBH_GPU_TEXTURE_FILTER_LINEAR,
        BTBH_GPU_TEXTURE_WRAP_CLAMP, BTBH_GPU_TEXTURE_COLOR_SPACE_SRGB, &texture_report);
    assert(plain != NULL);
    result = render_portrait(&b, plain, PORTRAIT_SIZE, 0u, &report);
    assert(result == BTBH_GPU_INVALID_ARGUMENT);
    btbh_gpu_texture_destroy(plain);
    /* While the frame's scene waits for its canvas: refused, and the frame survives. */
    result = present_world(&b, 5u, BTBH_GPU_SCENE_UPSCALE_FSR3, &report);
    assert(result == BTBH_GPU_OK);
    result = render_portrait(&b, b.portrait, PORTRAIT_SIZE, 0u, &report);
    assert(result == BTBH_GPU_INVALID_ARGUMENT);
    assert(btbh_gpu_window_copy_pending_scene_texture(b.window, b.frame, &copied) == BTBH_GPU_OK);

    /* A batch out of the reactive mask is valid; an unknown mode is not. */
    {
        const SmokeCamera camera = world_camera(6u);
        const BTBHGPUSceneLighting lighting = smoke_lighting();
        const BTBHGPUScenePost post = smoke_post(&camera, 0u);
        const float glass[4] = {0.4f, 0.7f, 1.0f, 0.5f};
        float instances[1][24];
        instance(instances[0], 0.0f, 0.5f, 0.0f, 1.2f, 1.2f, 1.2f, glass);
        BTBHGPUSceneGeometryBatch batch = cube_batch(a.cube, 0u, 1u);
        batch.transparency_enabled = 1u;
        batch.reactive_mode = BTBH_GPU_SCENE_REACTIVE_NONE;
        for (uint32_t mode = BTBH_GPU_SCENE_REACTIVE_NONE; mode <= BTBH_GPU_SCENE_REACTIVE_COUNT; ++mode) {
            batch.reactive_mode = (uint8_t)mode;
            result = btbh_gpu_window_scene_present_instances(
                a.window,
                shader_paths[0], shader_paths[1], shader_paths[2], shader_paths[3],
                shader_paths[4], shader_paths[5], shader_paths[6], shader_paths[7],
                shader_paths[8], shader_paths[9], shader_paths[10],
                SMOKE_SHADER_FORMAT,
                WORLD_RENDER_WIDTH,
                WORLD_RENDER_HEIGHT,
                camera.view_projection, camera.right, camera.up, camera.position,
                &lighting, &post,
                &instances[0][0], 1u, 1u,
                &batch, 1u,
                40u, 60u, 90u, 255u,
                &report
            );
            if (mode < BTBH_GPU_SCENE_REACTIVE_COUNT) {
                assert(result == BTBH_GPU_OK);
                assert(btbh_gpu_window_copy_pending_scene_texture(a.window, a.frame, &copied) == BTBH_GPU_OK);
            } else {
                assert(result == BTBH_GPU_INVALID_ARGUMENT);
            }
        }
    }

    btbh_gpu_texture_destroy(a.frame);
    btbh_gpu_texture_destroy(a.portrait);
    btbh_gpu_texture_destroy(b.frame);
    btbh_gpu_texture_destroy(b.portrait);
    btbh_gpu_custom_mesh_destroy(a.cube);
    btbh_gpu_custom_mesh_destroy(b.cube);
    btbh_gpu_window_destroy(a.window);
    btbh_gpu_window_destroy(b.window);
    btbh_gpu_device_shutdown(device);
    btbh_gpu_device_destroy(device);
    printf("render-to-target smoke passed\n");
    return 0;
}
