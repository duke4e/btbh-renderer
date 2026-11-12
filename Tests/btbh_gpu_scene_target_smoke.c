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
 * Then, rendered to the target and read back (0.5.0): a cube's shadow on a slab, gone when the cube has
 * BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW while the cube is still drawn, and still there when the slab has the flag
 * (it receives); the opaque pass sorted front to back (BTBHGPUScenePost.opaque_sort), which flips a depth tie
 * between two copies of a slab (so it does reorder) but not when a batch is pinned (KEEP_ORDER), writes the
 * stencil, or uses LESS_EQUAL or ALWAYS, and leaves a layered scene with see-through cubes byte for byte the
 * same; and the refusal of an unknown sort or flag. BTBH_SMOKE_BENCH=1 also times heavy-overdraw scenes as
 * given and sorted, and the order's CPU cost (prints only).
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
    /* The cube wound the other way: lit on the outside (the 0.5.0 checks and the bench). */
    BTBHGPUCustomMesh *lit_cube;
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

/* A unit cube (-0.5..0.5), 24 floats a vertex: position, normal, uv, tangent, colour, joints, weights.
   Its faces are wound so that the renderer takes their outsides for back faces (and turns their normals in);
   front_facing winds them the other way, for the checks that need the sun to light the faces it faces. */
static BTBHGPUCustomMesh *create_cube_wound(BTBHGPUWindow *window, bool front_facing) {
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
        const uint32_t reversed[6] = {base, base + 2u, base + 1u, base, base + 3u, base + 2u};
        memcpy(&indices[face * 6u], front_facing ? reversed : quad, sizeof(quad));
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

static BTBHGPUCustomMesh *create_cube(BTBHGPUWindow *window) {
    return create_cube_wound(window, false);
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

/* ---- BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW and BTBHGPUScenePost.opaque_sort (since 0.5.0) ---- */

static int texel_luminance(const uint8_t *pixels, uint32_t index) {
    const uint8_t *texel = &pixels[index * 4u];
    return (int)texel[0] + (int)texel[1] + (int)texel[2];
}

static int texel_channel_distance(const uint8_t *a, const uint8_t *b, uint32_t index) {
    int distance = 0;
    for (uint32_t channel = 0u; channel < 3u; ++channel) {
        const int difference = abs((int)a[index * 4u + channel] - (int)b[index * 4u + channel]);
        distance = difference > distance ? difference : distance;
    }
    return distance;
}

/* Renders `batches` into the window's portrait target (twice its size, halved) and reads it back. */
static uint8_t *render_aside(
    SmokeWindow *smoke,
    const SmokeCamera *camera,
    const BTBHGPUSceneLighting *lighting,
    const BTBHGPUScenePost *post,
    const float *instances,
    uint32_t instance_count,
    const BTBHGPUSceneGeometryBatch *batches,
    uint32_t batch_count,
    BTBHGPUSceneReport *report
) {
    const BTBHGPUResult result = btbh_gpu_window_scene_render_to_target(
        smoke->window,
        smoke->portrait,
        shader_paths[0], shader_paths[1], shader_paths[2], shader_paths[3],
        shader_paths[4], shader_paths[5], shader_paths[6], shader_paths[7],
        shader_paths[8], shader_paths[9], shader_paths[10],
        SMOKE_SHADER_FORMAT,
        2u * PORTRAIT_SIZE,
        2u * PORTRAIT_SIZE,
        camera->view_projection,
        camera->right,
        camera->up,
        camera->position,
        lighting,
        post,
        instances,
        instance_count,
        instance_count,
        batches,
        NULL,
        NULL,
        batch_count,
        12u, 14u, 20u, 255u,
        report
    );
    if (result != BTBH_GPU_OK) {
        fprintf(stderr, "scene aside: %s\n", report->message);
    }
    assert(result == BTBH_GPU_OK);
    return read_texture(smoke->window, smoke->portrait);
}

/* A cube floating over a slab under a sun from (8, 6, 3): its shadow lies on the slab, off towards -x and -z,
   in view of a camera from +z. Two batches (cube, slab) with their flags; the cube left out when !with_cube, and
   the sun's shadow map turned off when !shadowed. (The cube wound to be lit on the outside: with the other
   winding the sun would light no face the camera sees, and no shadow would show.) */
static uint8_t *render_shadow_scene(
    SmokeWindow *smoke,
    uint8_t cube_flags,
    uint8_t slab_flags,
    bool with_cube,
    bool shadowed
) {
    const float eye[3] = {0.0f, 9.0f, 6.0f};
    const float centre[3] = {-1.0f, 0.0f, -0.5f};
    const SmokeCamera camera = look_at(eye, centre, 0.9f, 1.0f, NULL);
    const float sun[3] = {8.0f, 6.0f, 3.0f};
    BTBHGPUSceneLighting lighting = smoke_lighting_from(sun);
    lighting.directional_shadow_enabled = shadowed ? 1u : 0u;
    const BTBHGPUScenePost post = smoke_post(&camera, 0u);
    const float gold[4] = {1.0f, 0.8f, 0.3f, 1.0f};
    const float slate[4] = {0.5f, 0.55f, 0.6f, 1.0f};
    float instances[2][24];
    instance(instances[0], 0.0f, 2.1f, 0.0f, 1.4f, 1.4f, 1.4f, gold);
    instance(instances[1], 0.0f, -0.05f, 0.0f, 8.0f, 0.1f, 8.0f, slate);
    BTBHGPUSceneGeometryBatch batches[2] = {
        cube_batch(smoke->lit_cube, 0u, 1u),
        cube_batch(smoke->lit_cube, 1u, 1u),
    };
    batches[0].flags = cube_flags;
    batches[1].flags = slab_flags;
    BTBHGPUSceneReport report;
    return render_aside(
        smoke,
        &camera,
        &lighting,
        &post,
        &instances[0][0],
        2u,
        with_cube ? &batches[0] : &batches[1],
        with_cube ? 2u : 1u,
        &report
    );
}

/* The cube casts its shadow on the slab; with NO_SUN_SHADOW it is drawn but casts none, and a slab with the
   flag still receives the cube's shadow. */
static void check_no_sun_shadow(SmokeWindow *smoke) {
    uint8_t *casting = render_shadow_scene(smoke, 0u, 0u, true, true);
    uint8_t *flagged = render_shadow_scene(smoke, BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW, 0u, true, true);
    uint8_t *receiving = render_shadow_scene(smoke, 0u, BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW, true, true);
    uint8_t *slab_only = render_shadow_scene(smoke, 0u, 0u, false, true);
    uint8_t *unshadowed = render_shadow_scene(smoke, 0u, 0u, true, false);
    const uint32_t texels = PORTRAIT_SIZE * PORTRAIT_SIZE;
    uint32_t shadow_texels = 0u;
    uint32_t darker_texels = 0u;
    uint32_t received_texels = 0u;
    uint32_t cube_texels = 0u;
    int deepest_shadow = 0;
    int farthest_from_unshadowed = 0;
    for (uint32_t index = 0u; index < texels; ++index) {
        const int lit = texel_luminance(flagged, index);
        const int shaded = texel_luminance(casting, index);
        if (lit > shaded + 24) {
            /* The cube's shadow: brighter once the cube casts none. */
            shadow_texels += 1u;
            deepest_shadow = lit - shaded > deepest_shadow ? lit - shaded : deepest_shadow;
            if (texel_channel_distance(receiving, casting, index) <= 2) {
                received_texels += 1u;
            }
            const int unshadowed_distance = texel_channel_distance(flagged, unshadowed, index);
            farthest_from_unshadowed =
                unshadowed_distance > farthest_from_unshadowed ? unshadowed_distance : farthest_from_unshadowed;
        } else if (shaded > lit + 6) {
            /* Taking a caster out never darkens anything. */
            darker_texels += 1u;
        }
        if (texel_channel_distance(flagged, slab_only, index) > 30) {
            cube_texels += 1u;
        }
    }
    printf(
        "no sun shadow: %u shadow texels (deepest %d) gone with the cube's flag (there within %d of no shadows at "
        "all), %u of them kept with the slab's; the flagged cube still covers %u texels\n",
        shadow_texels,
        deepest_shadow,
        farthest_from_unshadowed,
        received_texels,
        cube_texels
    );
    (void)fflush(stdout);
    if (getenv("BTBH_SMOKE_DUMP") != NULL) {
        char path[1024];
        (void)snprintf(path, sizeof(path), "%s/shadow-cast.png", getenv("BTBH_SMOKE_DUMP"));
        (void)btbh_gpu_write_png_rgba8(path, PORTRAIT_SIZE, PORTRAIT_SIZE, casting);
        (void)snprintf(path, sizeof(path), "%s/shadow-flagged.png", getenv("BTBH_SMOKE_DUMP"));
        (void)btbh_gpu_write_png_rgba8(path, PORTRAIT_SIZE, PORTRAIT_SIZE, flagged);
        (void)snprintf(path, sizeof(path), "%s/shadow-received.png", getenv("BTBH_SMOKE_DUMP"));
        (void)btbh_gpu_write_png_rgba8(path, PORTRAIT_SIZE, PORTRAIT_SIZE, receiving);
        (void)snprintf(path, sizeof(path), "%s/shadow-off.png", getenv("BTBH_SMOKE_DUMP"));
        (void)btbh_gpu_write_png_rgba8(path, PORTRAIT_SIZE, PORTRAIT_SIZE, unshadowed);
    }
    assert(shadow_texels >= 40u);
    assert(deepest_shadow >= 60);
    assert(darker_texels == 0u);
    assert(farthest_from_unshadowed <= 3);
    assert(received_texels == shadow_texels);
    assert(cube_texels >= 40u);
    free(casting);
    free(flagged);
    free(receiving);
    free(slab_only);
    free(unshadowed);
}

/* Two copies of one slab facing the camera at exactly the same depth: batch A (light), and batch B (dark),
   whose second instance far off to the side and close to the camera makes its mean depth the nearer one. */
static uint8_t *render_tie_scene(
    SmokeWindow *smoke,
    uint8_t opaque_sort,
    const BTBHGPUSceneGeometryBatch *a_state,
    const BTBHGPUSceneGeometryBatch *b_state,
    BTBHGPUSceneReport *report
) {
    const float eye[3] = {0.0f, 0.0f, 5.0f};
    const float centre[3] = {0.0f, 0.0f, 0.0f};
    const SmokeCamera camera = look_at(eye, centre, 0.8f, 1.0f, NULL);
    BTBHGPUSceneLighting lighting = smoke_lighting();
    lighting.directional_shadow_enabled = 0u;
    BTBHGPUScenePost post = smoke_post(&camera, 0u);
    post.opaque_sort = opaque_sort;
    const float light[4] = {0.9f, 0.9f, 0.9f, 1.0f};
    const float dark[4] = {0.08f, 0.08f, 0.08f, 1.0f};
    float instances[3][24];
    instance(instances[0], 0.0f, 0.0f, 0.0f, 3.0f, 3.0f, 0.1f, light);
    instance(instances[1], 0.0f, 0.0f, 0.0f, 3.0f, 3.0f, 0.1f, dark);
    instance(instances[2], 40.0f, 0.0f, 3.0f, 1.0f, 1.0f, 1.0f, dark);
    BTBHGPUSceneGeometryBatch batches[2] = {*a_state, *b_state};
    batches[0].first_instance = 0u;
    batches[0].instance_count = 1u;
    batches[1].first_instance = 1u;
    batches[1].instance_count = 2u;
    return render_aside(smoke, &camera, &lighting, &post, &instances[0][0], 3u, batches, 2u, report);
}

/* The brightness (r + g + b) in the middle of the target, where both copies lie. */
static int centre_luminance(const uint8_t *pixels) {
    return texel_luminance(pixels, (PORTRAIT_SIZE / 2u) * PORTRAIT_SIZE + PORTRAIT_SIZE / 2u);
}

/* Overlapping cubes at different depths given far to near, a slab behind them and two overlapping
   see-through cubes: sorted, the picture must stay byte for byte the same (no two opaque surfaces tie). */
static uint8_t *render_layered_scene(SmokeWindow *smoke, uint8_t opaque_sort, BTBHGPUSceneReport *report) {
    const float eye[3] = {1.5f, 2.0f, 7.0f};
    const float centre[3] = {0.0f, 0.3f, 0.0f};
    const SmokeCamera camera = look_at(eye, centre, 0.8f, 1.0f, NULL);
    const BTBHGPUSceneLighting lighting = smoke_lighting();
    BTBHGPUScenePost post = smoke_post(&camera, 0u);
    post.opaque_sort = opaque_sort;
    const float colours[6][4] = {
        {0.5f, 0.55f, 0.6f, 1.0f},
        {0.9f, 0.3f, 0.2f, 1.0f},
        {0.2f, 0.8f, 0.3f, 1.0f},
        {0.3f, 0.4f, 0.9f, 1.0f},
        {0.9f, 0.9f, 0.2f, 0.5f},
        {0.2f, 0.9f, 0.9f, 0.5f},
    };
    float instances[6][24];
    instance(instances[0], 0.0f, -0.6f, -2.0f, 9.0f, 0.1f, 9.0f, colours[0]);
    instance(instances[1], -0.4f, 0.2f, -1.7f, 1.3f, 1.3f, 1.3f, colours[1]);
    instance(instances[2], 0.1f, 0.3f, -0.15f, 1.1f, 1.1f, 1.1f, colours[2]);
    instance(instances[3], 0.5f, 0.1f, 1.35f, 0.9f, 0.9f, 0.9f, colours[3]);
    instance(instances[4], -0.3f, 0.4f, 2.1f, 1.2f, 1.2f, 0.2f, colours[4]);
    instance(instances[5], 0.2f, 0.2f, 2.9f, 1.2f, 1.2f, 0.2f, colours[5]);
    BTBHGPUSceneGeometryBatch batches[6];
    for (uint32_t index = 0u; index < 6u; ++index) {
        batches[index] = cube_batch(smoke->lit_cube, index, 1u);
        batches[index].transparency_enabled = index >= 4u ? 1u : 0u;
    }
    return render_aside(smoke, &camera, &lighting, &post, &instances[0][0], 6u, batches, 6u, report);
}

/* The opaque pass's front-to-back order: it reorders (the tie shows who drew first), keeps the picture
   otherwise, never moves a pinned batch or one whose picture depends on the order, and refuses unknown values. */
static void check_opaque_sort(SmokeWindow *smoke) {
    const size_t bytes = (size_t)PORTRAIT_SIZE * PORTRAIT_SIZE * 4u;
    BTBHGPUSceneReport report;
    const BTBHGPUSceneGeometryBatch plain = cube_batch(smoke->lit_cube, 0u, 1u);

    /* The tie: as given A draws first and keeps the pixels (depth LESS); sorted, B (nearer on average) does. */
    uint8_t *submitted = render_tie_scene(smoke, 0u, &plain, &plain, &report);
    uint8_t *sorted = render_tie_scene(smoke, BTBH_GPU_SCENE_OPAQUE_SORT_FRONT_TO_BACK, &plain, &plain, &report);
    const int light_luminance = centre_luminance(submitted);
    const int dark_luminance = centre_luminance(sorted);
    printf("opaque sort: the tied copies' brightness %d as given (A first), %d sorted (B first)\n",
        light_luminance, dark_luminance);
    (void)fflush(stdout);
    assert(light_luminance > dark_luminance + 150);
    free(sorted);

    /* B pinned with KEEP_ORDER, or with a stencil write: nothing moves, the same bytes as given. */
    BTBHGPUSceneGeometryBatch pinned = plain;
    pinned.flags = BTBH_GPU_SCENE_BATCH_KEEP_ORDER;
    sorted = render_tie_scene(smoke, BTBH_GPU_SCENE_OPAQUE_SORT_FRONT_TO_BACK, &plain, &pinned, &report);
    assert(memcmp(sorted, submitted, bytes) == 0);
    free(sorted);
    BTBHGPUSceneGeometryBatch stencil = plain;
    stencil.stencil_reference = 1u;
    stencil.stencil_mask = 0xffu;
    uint8_t *stencil_submitted = render_tie_scene(smoke, 0u, &plain, &stencil, &report);
    sorted = render_tie_scene(smoke, BTBH_GPU_SCENE_OPAQUE_SORT_FRONT_TO_BACK, &plain, &stencil, &report);
    assert(memcmp(sorted, stencil_submitted, bytes) == 0);
    assert(centre_luminance(sorted) == light_luminance);
    free(sorted);
    free(stencil_submitted);
    free(submitted);

    /* Both LESS_EQUAL (the later one shows), or B drawn with depth ALWAYS: kept in order, the same bytes. */
    BTBHGPUSceneGeometryBatch less_equal = plain;
    less_equal.depth_compare = BTBH_GPU_SCENE_DEPTH_LESS_EQUAL;
    submitted = render_tie_scene(smoke, 0u, &less_equal, &less_equal, &report);
    sorted = render_tie_scene(smoke, BTBH_GPU_SCENE_OPAQUE_SORT_FRONT_TO_BACK, &less_equal, &less_equal, &report);
    assert(centre_luminance(submitted) == dark_luminance);
    assert(memcmp(sorted, submitted, bytes) == 0);
    free(sorted);
    free(submitted);
    BTBHGPUSceneGeometryBatch always = plain;
    always.depth_compare = BTBH_GPU_SCENE_DEPTH_ALWAYS;
    submitted = render_tie_scene(smoke, 0u, &plain, &always, &report);
    sorted = render_tie_scene(smoke, BTBH_GPU_SCENE_OPAQUE_SORT_FRONT_TO_BACK, &plain, &always, &report);
    assert(memcmp(sorted, submitted, bytes) == 0);
    free(sorted);
    free(submitted);
    printf("opaque sort: reorders (a depth tie flips), keeps KEEP_ORDER, stencil, LESS_EQUAL and ALWAYS batches in place\n");

    /* No tie: the same picture, the same draws, sorted or not (the see-through cubes keep their order). */
    submitted = render_layered_scene(smoke, 0u, &report);
    const uint32_t submitted_draws = report.primitive_draw_count;
    sorted = render_layered_scene(smoke, BTBH_GPU_SCENE_OPAQUE_SORT_FRONT_TO_BACK, &report);
    assert(report.primitive_draw_count == submitted_draws);
    if (getenv("BTBH_SMOKE_DUMP") != NULL) {
        char path[1024];
        (void)snprintf(path, sizeof(path), "%s/layered.png", getenv("BTBH_SMOKE_DUMP"));
        (void)btbh_gpu_write_png_rgba8(path, PORTRAIT_SIZE, PORTRAIT_SIZE, submitted);
    }
    assert(memcmp(sorted, submitted, bytes) == 0);
    printf("opaque sort: a layered scene with see-through cubes, byte for byte the same sorted\n");
    free(sorted);
    free(submitted);

    /* Unknown values fail the present. */
    {
        const SmokeCamera camera = world_camera(7u);
        const BTBHGPUSceneLighting lighting = smoke_lighting();
        BTBHGPUScenePost post = smoke_post(&camera, 0u);
        const float grey[4] = {0.6f, 0.6f, 0.6f, 1.0f};
        float instances[1][24];
        instance(instances[0], 0.0f, 0.5f, 0.0f, 1.0f, 1.0f, 1.0f, grey);
        BTBHGPUSceneGeometryBatch batch = cube_batch(smoke->lit_cube, 0u, 1u);
        for (uint32_t attempt = 0u; attempt < 4u; ++attempt) {
            post.opaque_sort = attempt == 1u ? (uint8_t)BTBH_GPU_SCENE_OPAQUE_SORT_COUNT : 0u;
            batch.flags = attempt == 2u
                ? (uint8_t)0x80u
                : (attempt == 3u
                    ? (uint8_t)(BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW | BTBH_GPU_SCENE_BATCH_KEEP_ORDER)
                    : 0u);
            const BTBHGPUResult result = btbh_gpu_window_scene_render_to_target(
                smoke->window,
                smoke->portrait,
                shader_paths[0], shader_paths[1], shader_paths[2], shader_paths[3],
                shader_paths[4], shader_paths[5], shader_paths[6], shader_paths[7],
                shader_paths[8], shader_paths[9], shader_paths[10],
                SMOKE_SHADER_FORMAT,
                PORTRAIT_SIZE,
                PORTRAIT_SIZE,
                camera.view_projection, camera.right, camera.up, camera.position,
                &lighting, &post,
                &instances[0][0], 1u, 1u,
                &batch, NULL, NULL, 1u,
                12u, 14u, 20u, 255u,
                &report
            );
            assert(result == (attempt == 1u || attempt == 2u ? BTBH_GPU_INVALID_ARGUMENT : BTBH_GPU_OK));
        }
    }
}

/* ---- BTBH_SMOKE_BENCH=1: what the front-to-back order buys and costs (prints only, asserts nothing) ---- */

#define BENCH_WIDTH 1920u
#define BENCH_HEIGHT 1080u
/* The configurations take turns for BENCH_ROUNDS rounds (so clocks and caches drift alike). A turn renders
   BENCH_WARM_FRAMES frames, waits for the GPU, then renders BENCH_FRAMES frames back to back and waits once:
   its sample is that time over BENCH_FRAMES, the GPU's throughput kept busy (a wait after every frame lets
   its clocks drop). */
#define BENCH_ROUNDS 8
#define BENCH_WARM_FRAMES 3
#define BENCH_FRAMES 20
#define BENCH_SAMPLES BENCH_ROUNDS

typedef struct BenchConfig {
    const char *name;
    uint8_t opaque_sort;
    const BTBHGPUSceneGeometryBatch *batches;
    uint32_t batch_count;
    double times[BENCH_SAMPLES];
    uint32_t sample_count;
    double cpu_prepare_milliseconds;
    double cpu_geometry_milliseconds;
    uint32_t cpu_frames;
} BenchConfig;

static int compare_doubles(const void *left, const void *right) {
    const double a = *(const double *)left;
    const double b = *(const double *)right;
    return a < b ? -1 : (a > b ? 1 : 0);
}

/* A heavy pixel: the sun's soft shadow, 40 lamps and 32 point lights over every lit surface. */
static BTBHGPUSceneLighting bench_lighting(void) {
    const float sun[3] = {4.0f, 10.0f, 3.0f};
    BTBHGPUSceneLighting lighting = smoke_lighting_from(sun);
    lighting.directional_shadow_filter = BTBH_GPU_SCENE_SHADOW_FILTER_SOFT;
    lighting.lamp_count = BTBH_GPU_SCENE_MAX_LAMPS;
    lighting.lamp_core_radius = 0.5f;
    for (uint32_t lamp = 0u; lamp < BTBH_GPU_SCENE_MAX_LAMPS; ++lamp) {
        lighting.lamp_position_reach[lamp][0] = -12.0f + 0.6f * (float)lamp;
        lighting.lamp_position_reach[lamp][1] = 1.5f;
        lighting.lamp_position_reach[lamp][2] = -6.0f + 0.3f * (float)(lamp % 7u);
        lighting.lamp_position_reach[lamp][3] = 30.0f;
        lighting.lamp_color_energy[lamp][0] = 1.0f;
        lighting.lamp_color_energy[lamp][1] = 0.8f;
        lighting.lamp_color_energy[lamp][2] = 0.6f;
        lighting.lamp_color_energy[lamp][3] = 0.05f;
    }
    lighting.point_light_count = BTBH_GPU_SCENE_POINT_LIGHTS;
    for (uint32_t light = 0u; light < BTBH_GPU_SCENE_POINT_LIGHTS; ++light) {
        float *point = lighting.point_lights[light];
        point[0] = -10.0f + 0.65f * (float)light;
        point[1] = 2.0f;
        point[2] = -4.0f + 0.25f * (float)(light % 5u);
        point[3] = 40.0f;
        point[4] = 0.6f;
        point[5] = 0.7f;
        point[6] = 1.0f;
        point[7] = 0.05f;
    }
    return lighting;
}

/* One turn of a configuration (BENCH_ROUNDS says how a turn is timed). */
static void bench_turn(
    SmokeWindow *smoke,
    BTBHGPUTexture *target,
    const SmokeCamera *camera,
    const BTBHGPUSceneLighting *lighting,
    const float *instances,
    uint32_t instance_count,
    BenchConfig *config
) {
    BTBHGPUScenePost post = smoke_post(camera, 0u);
    post.upscale_mode = BTBH_GPU_SCENE_UPSCALE_LINEAR;
    post.opaque_sort = config->opaque_sort;
    SDL_GPUDevice *device = smoke->window->device->gpu;
    uint64_t started = 0u;
    for (int frame = 0; frame < BENCH_WARM_FRAMES + BENCH_FRAMES; ++frame) {
        if (frame == BENCH_WARM_FRAMES) {
            (void)SDL_WaitForGPUIdle(device);
            started = SDL_GetTicksNS();
        }
        BTBHGPUSceneReport report;
        const BTBHGPUResult rendered = btbh_gpu_window_scene_render_to_target(
            smoke->window,
            target,
            shader_paths[0], shader_paths[1], shader_paths[2], shader_paths[3],
            shader_paths[4], shader_paths[5], shader_paths[6], shader_paths[7],
            shader_paths[8], shader_paths[9], shader_paths[10],
            SMOKE_SHADER_FORMAT,
            BENCH_WIDTH,
            BENCH_HEIGHT,
            camera->view_projection, camera->right, camera->up, camera->position,
            lighting, &post,
            instances, instance_count, instance_count,
            config->batches, NULL, NULL, config->batch_count,
            12u, 14u, 20u, 255u,
            &report
        );
        if (rendered != BTBH_GPU_OK) {
            fprintf(stderr, "bench: %s\n", report.message);
        }
        assert(rendered == BTBH_GPU_OK);
        if (frame >= BENCH_WARM_FRAMES) {
            config->cpu_prepare_milliseconds += report.cpu_prepare_milliseconds;
            config->cpu_geometry_milliseconds += report.cpu_geometry_encode_milliseconds;
            config->cpu_frames += 1u;
        }
    }
    (void)SDL_WaitForGPUIdle(device);
    if (config->sample_count < BENCH_SAMPLES) {
        config->times[config->sample_count++] =
            (double)(SDL_GetTicksNS() - started) / 1.0e6 / BENCH_FRAMES;
    }
}

static void bench_configs(
    SmokeWindow *smoke,
    BTBHGPUTexture *target,
    const SmokeCamera *camera,
    const BTBHGPUSceneLighting *lighting,
    const float *instances,
    uint32_t instance_count,
    BenchConfig *configs,
    uint32_t config_count
) {
    for (int round = 0; round < BENCH_ROUNDS; ++round) {
        for (uint32_t index = 0u; index < config_count; ++index) {
            bench_turn(smoke, target, camera, lighting, instances, instance_count, &configs[index]);
        }
    }
    for (uint32_t index = 0u; index < config_count; ++index) {
        BenchConfig *config = &configs[index];
        qsort(config->times, config->sample_count, sizeof(config->times[0]), compare_doubles);
        printf(
            "  %-40s %7.2f ms a frame (median of %u turns; fastest %7.2f, slowest %7.2f); CPU prepare %.3f ms, "
            "geometry encode %.3f ms\n",
            config->name,
            config->times[config->sample_count / 2u],
            config->sample_count,
            config->times[0],
            config->times[config->sample_count - 1u],
            config->cpu_prepare_milliseconds / config->cpu_frames,
            config->cpu_geometry_milliseconds / config->cpu_frames
        );
    }
}

static void bench_opaque_sort(SmokeWindow *smoke) {
    BTBHGPUTextureReport texture_report;
    BTBHGPUTexture *target = btbh_gpu_window_render_target_create(
        smoke->window, BENCH_WIDTH, BENCH_HEIGHT, BTBH_GPU_TEXTURE_FILTER_LINEAR, &texture_report);
    assert(target != NULL);
    const BTBHGPUSceneLighting lighting = bench_lighting();
    const float aspect = (float)BENCH_WIDTH / (float)BENCH_HEIGHT;
    const char *device_name = SDL_GetStringProperty(
        SDL_GetGPUDeviceProperties(smoke->window->device->gpu), SDL_PROP_GPU_DEVICE_NAME_STRING, "unknown");
    printf(
        "bench on %s, %ux%u renders to a target; the configurations take %d turns each of %d frames back to back "
        "(GPU throughput, CPU included):\n",
        device_name, BENCH_WIDTH, BENCH_HEIGHT, BENCH_ROUNDS, BENCH_FRAMES);

    /* Worst case: 48 screen-filling slabs given far to near, one batch each. */
    {
        enum { LAYERS = 48 };
        static float instances[LAYERS][24];
        static BTBHGPUSceneGeometryBatch batches[LAYERS];
        const float grey[4] = {0.6f, 0.6f, 0.62f, 1.0f};
        for (uint32_t layer = 0u; layer < LAYERS; ++layer) {
            const uint32_t depth_rank = LAYERS - 1u - layer; /* the first batch is the farthest */
            instance(instances[layer], 0.0f, 0.0f, -0.35f * (float)depth_rank, 60.0f, 34.0f, 0.02f, grey);
            batches[layer] = cube_batch(smoke->lit_cube, layer, 1u);
        }
        const float eye[3] = {0.0f, 0.0f, 8.0f};
        const float centre[3] = {0.0f, 0.0f, 0.0f};
        const SmokeCamera camera = look_at(eye, centre, 0.9f, aspect, NULL);
        printf(" 48 screen-filling slabs, given far to near:\n");
        static BenchConfig configs[3];
        memset(configs, 0, sizeof(configs));
        configs[0] = (BenchConfig){.name = "as given", .batches = batches, .batch_count = LAYERS};
        configs[1] = (BenchConfig){
            .name = "sorted front to back",
            .opaque_sort = BTBH_GPU_SCENE_OPAQUE_SORT_FRONT_TO_BACK,
            .batches = batches,
            .batch_count = LAYERS};
        configs[2] = (BenchConfig){.name = "the nearest slab alone", .batches = &batches[LAYERS - 1], .batch_count = 1u};
        bench_configs(smoke, target, &camera, &lighting, &instances[0][0], LAYERS, configs, 3u);
    }

    /* RTS-like: a ground slab and 64 batches of 16 units standing on it, seen from above at 45 degrees. */
    {
        enum { UNIT_BATCHES = 64, UNITS_PER_BATCH = 16, UNITS = UNIT_BATCHES * UNITS_PER_BATCH };
        static float instances[UNITS + 1][24];
        static BTBHGPUSceneGeometryBatch ground_first[UNIT_BATCHES + 1];
        static BTBHGPUSceneGeometryBatch ground_last[UNIT_BATCHES + 1];
        static BTBHGPUSceneGeometryBatch ground_pinned[UNIT_BATCHES + 1];
        const float ground[4] = {0.35f, 0.45f, 0.3f, 1.0f};
        const float unit[4] = {0.7f, 0.3f, 0.25f, 1.0f};
        instance(instances[0], 0.0f, -0.1f, 0.0f, 80.0f, 0.2f, 80.0f, ground);
        uint32_t seed = 12345u;
        for (uint32_t index = 0u; index < UNITS; ++index) {
            seed = seed * 1664525u + 1013904223u;
            const float x = -14.0f + 28.0f * (float)(seed >> 8) / 16777216.0f;
            seed = seed * 1664525u + 1013904223u;
            const float z = -12.0f + 22.0f * (float)(seed >> 8) / 16777216.0f;
            instance(instances[1u + index], x, 0.36f, z, 0.7f, 0.7f, 0.7f, unit);
        }
        ground_first[0] = cube_batch(smoke->lit_cube, 0u, 1u);
        for (uint32_t index = 0u; index < UNIT_BATCHES; ++index) {
            ground_first[1u + index] =
                cube_batch(smoke->lit_cube, 1u + index * UNITS_PER_BATCH, UNITS_PER_BATCH);
            ground_last[index] = ground_first[1u + index];
            ground_pinned[index] = ground_first[1u + index];
        }
        ground_last[UNIT_BATCHES] = ground_first[0];
        ground_pinned[UNIT_BATCHES] = ground_first[0];
        ground_pinned[UNIT_BATCHES].flags = BTBH_GPU_SCENE_BATCH_KEEP_ORDER;
        const float eye[3] = {0.0f, 14.0f, 14.0f};
        const float centre[3] = {0.0f, 0.0f, -1.0f};
        const SmokeCamera camera = look_at(eye, centre, 0.9f, aspect, NULL);
        printf(" a ground slab and %d units in %d batches (seen from 45 degrees up):\n", UNITS, UNIT_BATCHES);
        static BenchConfig configs[6];
        memset(configs, 0, sizeof(configs));
        configs[0] = (BenchConfig){
            .name = "ground first, as given", .batches = ground_first, .batch_count = UNIT_BATCHES + 1u};
        configs[1] = (BenchConfig){
            .name = "ground first, sorted",
            .opaque_sort = BTBH_GPU_SCENE_OPAQUE_SORT_FRONT_TO_BACK,
            .batches = ground_first,
            .batch_count = UNIT_BATCHES + 1u};
        configs[2] = (BenchConfig){
            .name = "ground last, as given", .batches = ground_last, .batch_count = UNIT_BATCHES + 1u};
        configs[3] = (BenchConfig){
            .name = "ground last (KEEP_ORDER), units sorted",
            .opaque_sort = BTBH_GPU_SCENE_OPAQUE_SORT_FRONT_TO_BACK,
            .batches = ground_pinned,
            .batch_count = UNIT_BATCHES + 1u};
        configs[4] = (BenchConfig){.name = "the ground alone", .batches = ground_first, .batch_count = 1u};
        configs[5] = (BenchConfig){
            .name = "the units alone", .batches = ground_last, .batch_count = UNIT_BATCHES};
        bench_configs(smoke, target, &camera, &lighting, &instances[0][0], UNITS + 1u, configs, 6u);
    }
    btbh_gpu_texture_destroy(target);

    /* The order's CPU cost on its own: btbh_gpu_scene_opaque_order over transient instances. */
    {
        static const uint32_t shapes[3][2] = {{256u, 16u}, {2048u, 1u}, {1024u, 64u}};
        printf(" CPU cost of the order (btbh_gpu_scene_opaque_order):\n");
        const float eye[3] = {0.0f, 14.0f, 14.0f};
        const float centre[3] = {0.0f, 0.0f, -1.0f};
        const SmokeCamera camera = look_at(eye, centre, 0.9f, 16.0f / 9.0f, NULL);
        for (uint32_t shape = 0u; shape < 3u; ++shape) {
            const uint32_t batch_count = shapes[shape][0];
            const uint32_t per_batch = shapes[shape][1];
            const uint32_t total = batch_count * per_batch;
            float *instances = malloc(sizeof(float) * 24u * total);
            BTBHGPUSceneGeometryBatch *batches = malloc(sizeof(*batches) * batch_count);
            uint32_t *order = malloc(sizeof(uint32_t) * batch_count);
            uint64_t *scratch = malloc(sizeof(uint64_t) * 2u * batch_count);
            assert(instances != NULL && batches != NULL && order != NULL && scratch != NULL);
            const float colour[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            uint32_t seed = 777u;
            for (uint32_t index = 0u; index < total; ++index) {
                seed = seed * 1664525u + 1013904223u;
                const float x = -20.0f + 40.0f * (float)(seed >> 8) / 16777216.0f;
                seed = seed * 1664525u + 1013904223u;
                const float z = -20.0f + 40.0f * (float)(seed >> 8) / 16777216.0f;
                instance(&instances[(size_t)index * 24u], x, 0.5f, z, 1.0f, 1.0f, 1.0f, colour);
            }
            for (uint32_t index = 0u; index < batch_count; ++index) {
                batches[index] = cube_batch(smoke->lit_cube, index * per_batch, per_batch);
            }
            const int repeats = 200;
            uint32_t count = 0u;
            const uint64_t started = SDL_GetTicksNS();
            for (int repeat = 0; repeat < repeats; ++repeat) {
                count = btbh_gpu_scene_opaque_order(
                    batches, NULL, batch_count, instances, camera.view_projection, order, scratch);
            }
            const double microseconds = (double)(SDL_GetTicksNS() - started) / 1.0e3 / repeats;
            assert(count == batch_count);
            printf("  %5u batches x %2u instances: %8.1f us a frame\n", batch_count, per_batch, microseconds);
            free(instances);
            free(batches);
            free(order);
            free(scratch);
        }
    }

    /* What the custom mesh's bounds add to an update (they're measured on every create and update). */
    {
        enum { VERTICES = 100000 };
        float *vertices = calloc((size_t)VERTICES * 24u, sizeof(float));
        assert(vertices != NULL);
        for (uint32_t index = 0u; index < VERTICES; ++index) {
            vertices[(size_t)index * 24u + 0u] = (float)(index % 317u);
            vertices[(size_t)index * 24u + 1u] = (float)(index % 13u);
            vertices[(size_t)index * 24u + 2u] = (float)(index / 317u);
        }
        BTBHGPUCustomMeshReport mesh_report;
        BTBHGPUCustomMesh *mesh = btbh_gpu_window_custom_mesh_create(
            smoke->window, vertices, (uint32_t)(sizeof(float) * 24u * VERTICES), VERTICES, NULL, 0u, &mesh_report);
        assert(mesh != NULL);
        const int repeats = 20;
        uint64_t started = SDL_GetTicksNS();
        for (int repeat = 0; repeat < repeats; ++repeat) {
            assert(btbh_gpu_custom_mesh_update(
                mesh, vertices, (uint32_t)(sizeof(float) * 24u * VERTICES), NULL, 0u, &mesh_report) == BTBH_GPU_OK);
        }
        const double update_milliseconds = (double)(SDL_GetTicksNS() - started) / 1.0e6 / repeats;
        float minimum[3];
        float maximum[3];
        started = SDL_GetTicksNS();
        bool valid = false;
        for (int repeat = 0; repeat < repeats; ++repeat) {
            valid = btbh_gpu_custom_mesh_bounds(vertices, VERTICES, minimum, maximum);
        }
        const double bounds_milliseconds = (double)(SDL_GetTicksNS() - started) / 1.0e6 / repeats;
        assert(valid);
        printf(
            " custom mesh update of %d vertices: %.3f ms, of which its bounds %.3f ms\n",
            VERTICES,
            update_milliseconds,
            bounds_milliseconds
        );
        btbh_gpu_custom_mesh_destroy(mesh);
        free(vertices);
    }
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
    smoke.lit_cube = create_cube_wound(smoke.window, true);
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

    /* 0.5.0: batches that cast no sun shadow, and the opaque pass sorted front to back. */
    check_no_sun_shadow(&a);
    check_opaque_sort(&a);
    if (getenv("BTBH_SMOKE_BENCH") != NULL) {
        bench_opaque_sort(&a);
    }

    btbh_gpu_texture_destroy(a.frame);
    btbh_gpu_texture_destroy(a.portrait);
    btbh_gpu_texture_destroy(b.frame);
    btbh_gpu_texture_destroy(b.portrait);
    btbh_gpu_custom_mesh_destroy(a.cube);
    btbh_gpu_custom_mesh_destroy(b.cube);
    btbh_gpu_custom_mesh_destroy(a.lit_cube);
    btbh_gpu_custom_mesh_destroy(b.lit_cube);
    btbh_gpu_window_destroy(a.window);
    btbh_gpu_window_destroy(b.window);
    btbh_gpu_device_shutdown(device);
    btbh_gpu_device_destroy(device);
    printf("render-to-target smoke passed\n");
    return 0;
}
