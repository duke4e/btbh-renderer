/* The cover laid by facing and still ice on the GPU (RFC 0020 phase 1, since 0.18.0), against the CPU reference
 * (src/gpu/btbh_gpu_cover.h). Hidden windows, a 256 x 192 frame rendered at its size, read back pixel by pixel.
 * - Identity: a cover set and no batch with a bit, and a bit on every batch with every layer at amount 0, draw byte for
 *   byte as no cover at all.
 * - The patterns, unlit (the frame holds sRGB of the covered albedo): eight facets at 0 to 120 degrees from level; SNOW
 *   without noise (each facet one c), SNOW, SAND and MUD with noise, SNOW COARSE, the second layer through COVER_1, a
 *   normal map leaning the facing (normal_influence), noise finer than a pixel (the edge averaged over it),
 *   BTBH_DEBUG_COVER, and SNOW under the sun's shadow and under the sun's and an omni light's (the cover variants of
 *   each shadow set). Each pixel against the reference at the
 *   point its ray hits: within 3 levels in 99.5 % of the pixels, 8 everywhere.
 * - The targets the screen-space passes read (lit; the SSAO resolve's reflections and occlusion read the normal target
 *   and the specular target's alpha, the HBAO the normal target): MUD with puddles (wet) and SNOW with relief, the
 *   roughness, the occlusion and the normal against the reference; under the radiance cascades their albedo target
 *   holds the covered albedo.
 * - SSR: a glossy floor reflects a bright wall; covered by snow (rough), the reflection is gone.
 * - A see-through batch takes the cover opaque where it lies.
 * - Still ice, unlit and lit: the cracks' and the frost's lift against the reference, the roughness (its own, not the
 *   batch's factor) and the plates' and bevels' normal in the targets; the zero values draw.
 * - Runs: covered batches sharing a material draw in as many runs as uncovered ones, so do batches with a bit whose
 *   layer is off among ones without (and the same frame), and BTBH_GPU_SCENE_RUNS=verify finds no mismatch.
 * - Scene views (two halves equal the one-view frame) and a render into a target take the cover.
 * - Prewarm: a prewarm with the cover's bits and still ice, the layers off, covers a first frame with them on and one
 *   with them off (nothing made); a prewarm without them leaves the cover variants' pipelines to the first covered
 *   frame (made then, marked as a prewarm's).
 * - Refusals: the setter's (a refused call keeps the setting), the batches' (both bits, a lit particle, a refracting
 *   batch, the fast unlit shader's, ICE's parameters), and cover variants of the scene shader that don't name the
 *   cover's block or are missing (shaders from before 0.18.0). */
#undef NDEBUG

#include "btbh_gpu/btbh_gpu.h"
#include "btbh_gpu_cover.h"
#include "btbh_gpu_hdr.h"
#include "btbh_gpu_internal.h"
#include "btbh_gpu_package_internal.h"
#include "btbh_gpu_smoke_runs_verify.h"

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_hints.h>

#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SMOKE_SKIP 77
#define WIDTH 256u
#define HEIGHT 192u
/* The frame against the reference: the 8-bit frame is dithered by half a level, and where c changes fast across a pixel
   the GPU's interpolated position and the ray's differ by a hair. */
#define COLOUR_TOLERANCE 3
#define COLOUR_LIMIT 8
#define COLOUR_OUTLIERS 0.005
/* Still ice: its cracks are a few pixels wide and steep-edged, and the fine craze turns on by a pixel's footprint. */
#define ICE_OUTLIERS 0.02

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

typedef struct ShaderSet {
    char paths[11][1200];
} ShaderSet;

static ShaderSet shaders;

static void shader_set_in(ShaderSet *set, const char *directory) {
    for (int index = 0; index < 11; ++index) {
        (void)snprintf(set->paths[index], sizeof(set->paths[index]), "%s/%s%s", directory, shader_names[index],
            SMOKE_SHADER_EXTENSION);
    }
}

/* ---- Vectors and the camera ---- */

static float dot3(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void cross3(const float a[3], const float b[3], float out[3]) {
    const float x = a[1] * b[2] - a[2] * b[1];
    const float y = a[2] * b[0] - a[0] * b[2];
    const float z = a[0] * b[1] - a[1] * b[0];
    out[0] = x;
    out[1] = y;
    out[2] = z;
}

static void normalize3(float v[3]) {
    const float length = sqrtf(dot3(v, v));
    v[0] /= length;
    v[1] /= length;
    v[2] /= length;
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

static void invert(const float m[16], float out[16]) {
    double a[4][8];
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            a[row][column] = m[column * 4 + row];
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
        assert(fabs(scale) > 1.0e-12);
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
            out[column * 4 + row] = (float)a[row][column + 4];
        }
    }
}

typedef struct SmokeCamera {
    float view_projection[16];
    float inverse[16];
    float right[3];
    float up[3];
    float position[3];
} SmokeCamera;

#define CAMERA_NEAR 0.1f
#define CAMERA_FAR 80.0f
#define CAMERA_FOV 0.85f

static SmokeCamera look_at(const float eye[3], const float target[3]) {
    SmokeCamera camera;
    memset(&camera, 0, sizeof(camera));
    float forward[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
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
    const float h = 1.0f / tanf(0.5f * CAMERA_FOV);
    const float aspect = (float)WIDTH / (float)HEIGHT;
    const float r = CAMERA_FAR / (CAMERA_NEAR - CAMERA_FAR);
    const float projection[16] = {
        h / aspect, 0.0f, 0.0f, 0.0f,
        0.0f, h, 0.0f, 0.0f,
        0.0f, 0.0f, r, -1.0f,
        0.0f, 0.0f, r * CAMERA_NEAR, 0.0f,
    };
    multiply(projection, view, camera.view_projection);
    invert(camera.view_projection, camera.inverse);
    memcpy(camera.right, side, sizeof(side));
    memcpy(camera.up, up, sizeof(up));
    memcpy(camera.position, eye, sizeof(camera.position));
    return camera;
}

/* The ray through pixel (x, y) (its centre at +0.5): its origin on the near plane and its direction. */
static void pixel_ray(const SmokeCamera *camera, float x, float y, float origin[3], float direction[3]) {
    const float ndc_x = 2.0f * x / (float)WIDTH - 1.0f;
    const float ndc_y = 1.0f - 2.0f * y / (float)HEIGHT;
    float points[2][3];
    for (int which = 0; which < 2; ++which) {
        const float depth = which == 0 ? 0.0f : 0.5f;
        float h[4];
        for (int row = 0; row < 4; ++row) {
            h[row] = camera->inverse[0 * 4 + row] * ndc_x + camera->inverse[1 * 4 + row] * ndc_y +
                camera->inverse[2 * 4 + row] * depth + camera->inverse[3 * 4 + row];
        }
        points[which][0] = h[0] / h[3];
        points[which][1] = h[1] / h[3];
        points[which][2] = h[2] / h[3];
    }
    for (int axis = 0; axis < 3; ++axis) {
        origin[axis] = points[0][axis];
        direction[axis] = points[1][axis] - points[0][axis];
    }
}

/* ---- The geometry: flat rectangles ---- */

typedef struct Rect {
    float center[3];
    float u[3];       /* unit, along its width (the tangent) */
    float v[3];       /* unit, along its length (the bitangent side) */
    float normal[3];  /* unit, u x v's sign: the face seen */
    float half_u;
    float half_v;
} Rect;

/* A facet tilted `slope` degrees from level toward +z (its normal (0, cos, sin)), width along x. */
static Rect facet(float x, float y, float z, float slope, float width, float length) {
    Rect rect;
    const float angle = slope * BTBH_COVER_PI / 180.0f;
    rect.center[0] = x;
    rect.center[1] = y;
    rect.center[2] = z;
    rect.u[0] = 1.0f;
    rect.u[1] = 0.0f;
    rect.u[2] = 0.0f;
    rect.v[0] = 0.0f;
    rect.v[1] = sinf(angle);
    rect.v[2] = -cosf(angle);
    rect.normal[0] = 0.0f;
    rect.normal[1] = cosf(angle);
    rect.normal[2] = sinf(angle);
    rect.half_u = 0.5f * width;
    rect.half_v = 0.5f * length;
    return rect;
}

/* Where the ray meets the rect's plane (t along direction), and whether within it. */
static bool rect_hit(const Rect *rect, const float origin[3], const float direction[3], float *t, float point[3]) {
    const float denominator = dot3(rect->normal, direction);
    if (fabsf(denominator) < 1.0e-9f) {
        return false;
    }
    const float offset[3] = {rect->center[0] - origin[0], rect->center[1] - origin[1], rect->center[2] - origin[2]};
    *t = dot3(rect->normal, offset) / denominator;
    for (int axis = 0; axis < 3; ++axis) {
        point[axis] = origin[axis] + *t * direction[axis];
    }
    const float local[3] = {point[0] - rect->center[0], point[1] - rect->center[1], point[2] - rect->center[2]};
    return *t > 0.0f && fabsf(dot3(local, rect->u)) <= rect->half_u && fabsf(dot3(local, rect->v)) <= rect->half_v;
}

/* A rect's custom mesh (4 vertices, world coordinates, white, its tangent u). */
static BTBHGPUCustomMesh *rect_mesh(BTBHGPUWindow *window, const Rect *rect) {
    float vertices[4][24];
    memset(vertices, 0, sizeof(vertices));
    const float corners[4][2] = {{-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}};
    for (int corner = 0; corner < 4; ++corner) {
        float *vertex = vertices[corner];
        for (int axis = 0; axis < 3; ++axis) {
            vertex[axis] = rect->center[axis] + corners[corner][0] * rect->half_u * rect->u[axis] +
                corners[corner][1] * rect->half_v * rect->v[axis];
            vertex[3 + axis] = rect->normal[axis];
            vertex[8 + axis] = rect->u[axis];
        }
        vertex[6] = 0.5f + 0.5f * corners[corner][0];
        vertex[7] = 0.5f + 0.5f * corners[corner][1];
        vertex[11] = 1.0f;
        vertex[12] = 1.0f;
        vertex[13] = 1.0f;
        vertex[14] = 1.0f;
        vertex[15] = 1.0f;
        vertex[16] = 255.0f;
        vertex[20] = 1.0f;
    }
    /* (Front faces wind clockwise around their normal, as the scene's model pipelines take them.) */
    const uint32_t indices[6] = {0u, 2u, 1u, 0u, 3u, 2u};
    BTBHGPUCustomMeshReport report;
    memset(&report, 0, sizeof(report));
    BTBHGPUCustomMesh *mesh = btbh_gpu_window_custom_mesh_create(window, vertices, (uint32_t)sizeof(vertices), 4u,
        indices, 6u, &report);
    if (mesh == NULL) {
        fprintf(stderr, "rect mesh: %s\n", report.message);
    }
    assert(mesh != NULL);
    return mesh;
}

/* The facets: two rows of four, at 0 to 120 degrees from level. */
#define FACETS 8u
static const float facet_slopes[FACETS] = {0.0f, 15.0f, 30.0f, 45.0f, 60.0f, 75.0f, 90.0f, 120.0f};
static Rect facets[FACETS];
/* The ice: a level sheet. The SSR scene: a level floor and a bright wall behind it. */
static Rect ice_rect;
static Rect floor_rect;
static Rect wall_rect;

static void make_rects(void) {
    for (uint32_t index = 0u; index < FACETS; ++index) {
        const float x = -4.2f + 2.8f * (float)(index % 4u);
        const float z = index < 4u ? 2.2f : -1.8f;
        facets[index] = facet(x, 0.9f, z, facet_slopes[index], 2.4f, 2.0f);
    }
    ice_rect = facet(0.0f, 0.0f, -1.0f, 0.0f, 16.0f, 12.0f);
    floor_rect = facet(0.0f, 0.0f, 0.0f, 0.0f, 14.0f, 12.0f);
    wall_rect = facet(0.0f, 1.5f, -4.0f, 90.0f, 10.0f, 3.0f);
}

static const float facets_eye[3] = {0.0f, 9.5f, 11.0f};
static const float facets_target[3] = {0.0f, 0.5f, 0.0f};
static const float ice_eye[3] = {0.0f, 5.0f, 5.0f};
static const float ice_target[3] = {0.0f, 0.0f, -1.5f};
static const float floor_eye[3] = {0.0f, 2.6f, 8.0f};
static const float floor_target[3] = {0.0f, 0.4f, -2.0f};

/* ---- The window and the scene ---- */

typedef struct SmokeWindow {
    BTBHGPUWindow *window;
    BTBHGPUCustomMesh *facet_meshes[FACETS];
    BTBHGPUCustomMesh *ice;
    BTBHGPUCustomMesh *floor;
    BTBHGPUCustomMesh *wall;
    BTBHGPUTexture *frame;
    BTBHGPUTexture *target;
    BTBHGPUTexture *tilt_map;
} SmokeWindow;

static void instance(float out[24], const float colour[3]) {
    memset(out, 0, 24 * sizeof(float));
    out[0] = 1.0f;
    out[5] = 1.0f;
    out[10] = 1.0f;
    out[15] = 1.0f;
    out[16] = colour[0];
    out[17] = colour[1];
    out[18] = colour[2];
    out[19] = 1.0f;
}

static BTBHGPUSceneGeometryBatch batch_of(BTBHGPUCustomMesh *mesh, uint32_t first, bool unlit) {
    BTBHGPUSceneGeometryBatch batch;
    memset(&batch, 0, sizeof(batch));
    batch.kind = BTBH_GPU_SCENE_GEOMETRY_CUSTOM_MESH;
    batch.custom_mesh = mesh;
    batch.material_slot = -1;
    batch.first_instance = first;
    batch.instance_count = 1u;
    batch.albedo_red = 255u;
    batch.albedo_green = 255u;
    batch.albedo_blue = 255u;
    batch.albedo_alpha = 255u;
    batch.uv_scale_x = 1.0f;
    batch.uv_scale_y = 1.0f;
    batch.normal_scale = 1.0f;
    batch.roughness = 0.5f;
    batch.occlusion = 0.4f;
    batch.unlit_enabled = unlit ? 1u : 0u;
    batch.depth_compare = BTBH_GPU_SCENE_DEPTH_LESS;
    batch.cull_mode = BTBH_GPU_SCENE_CULL_NONE;
    batch.stencil_pass = BTBH_GPU_SCENE_STENCIL_REPLACE;
    batch.flags = BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW;
    return batch;
}

#define MAX_BATCHES 12u

typedef struct Scene {
    SmokeCamera camera;
    BTBHGPUSceneLighting lighting;
    BTBHGPUScenePost post;
    float instances[MAX_BATCHES][24];
    BTBHGPUSceneGeometryBatch batches[MAX_BATCHES];
    uint32_t batch_count;
    const ShaderSet *shaders;
    /* Two views, split at the frame's middle. */
    bool split;
    /* Into the window's target texture instead of its frame. */
    bool into_target;
} Scene;

static const float facet_albedo[3] = {0.18f, 0.32f, 0.12f};

static BTBHGPUSceneLighting lighting_plain(void) {
    BTBHGPUSceneLighting lighting;
    memset(&lighting, 0, sizeof(lighting));
    lighting.ambient_red = 1.0f;
    lighting.ambient_green = 1.0f;
    lighting.ambient_blue = 1.0f;
    lighting.ambient_energy = 0.4f;
    lighting.directional_energy = 1.5f;
    lighting.directional_red = 1.0f;
    lighting.directional_green = 1.0f;
    lighting.directional_blue = 1.0f;
    lighting.directional_x = -0.3f;
    lighting.directional_y = -1.0f;
    lighting.directional_z = -0.4f;
    lighting.spot_range = 8.0f;
    lighting.spot_falloff = 1.0f;
    lighting.spot_inner_cutoff = 1.0f;
    return lighting;
}

/* Reinhard with a white point of 1 is the identity: the frame holds sRGB(linear colour), dithered. */
static BTBHGPUScenePost post_plain(void) {
    BTBHGPUScenePost post;
    memset(&post, 0, sizeof(post));
    post.tonemap_white = 1.0f;
    post.exposure = 1.0f;
    post.contrast = 1.0f;
    post.saturation = 1.0f;
    post.blur_curve = 1.0f;
    post.tonemap_mode = BTBH_GPU_SCENE_TONEMAP_REINHARD;
    post.upscale_mode = BTBH_GPU_SCENE_UPSCALE_LINEAR;
    return post;
}

/* The reflections on (so the opaque pass writes the targets the screen-space passes read), weighing each surface by
   its own reflectivity. */
static void post_reflections(BTBHGPUScenePost *post) {
    post->ssr_intensity = 1.0f;
    post->ssr_max_roughness = 0.35f;
    post->ssr_max_distance = 20.0f;
    post->ssr_thickness = 0.6f;
    post->flags |= (uint8_t)BTBH_GPU_SCENE_POST_SSR_SURFACE_REFLECTIVITY;
}

/* The eight facets, unlit or lit, each in facet_albedo, with `flags2`. */
static Scene facets_scene(const SmokeWindow *smoke, bool unlit, uint8_t flags2) {
    Scene scene;
    memset(&scene, 0, sizeof(scene));
    scene.camera = look_at(facets_eye, facets_target);
    scene.lighting = lighting_plain();
    scene.post = post_plain();
    scene.shaders = &shaders;
    for (uint32_t index = 0u; index < FACETS; ++index) {
        instance(scene.instances[index], facet_albedo);
        scene.batches[index] = batch_of(smoke->facet_meshes[index], index, unlit);
        scene.batches[index].flags2 = flags2;
    }
    scene.batch_count = FACETS;
    return scene;
}

static BTBHGPUResult present(SmokeWindow *smoke, const Scene *scene, BTBHGPUSceneReport *report) {
    const ShaderSet *set = scene->shaders;
    const SmokeCamera *camera = &scene->camera;
    if (scene->into_target) {
        return btbh_gpu_window_scene_render_to_target(smoke->window, smoke->target, set->paths[0], set->paths[1],
            set->paths[2], set->paths[3], set->paths[4], set->paths[5], set->paths[6], set->paths[7], set->paths[8],
            set->paths[9], set->paths[10], SMOKE_SHADER_FORMAT, WIDTH, HEIGHT, camera->view_projection, camera->right,
            camera->up, camera->position, &scene->lighting, &scene->post, &scene->instances[0][0], scene->batch_count,
            scene->batch_count, scene->batches, NULL, NULL, scene->batch_count, 23u, 41u, 67u, 255u, report);
    }
    if (!scene->split) {
        return btbh_gpu_window_scene_present_instances(smoke->window, set->paths[0], set->paths[1], set->paths[2],
            set->paths[3], set->paths[4], set->paths[5], set->paths[6], set->paths[7], set->paths[8], set->paths[9],
            set->paths[10], SMOKE_SHADER_FORMAT, WIDTH, HEIGHT, camera->view_projection, camera->right, camera->up,
            camera->position, &scene->lighting, &scene->post, &scene->instances[0][0], scene->batch_count,
            scene->batch_count, scene->batches, scene->batch_count, 23u, 41u, 67u, 255u, report);
    }
    BTBHGPUSceneView views[2];
    memset(views, 0, sizeof(views));
    for (uint32_t index = 0u; index < 2u; ++index) {
        memcpy(views[index].view_projection, camera->view_projection, sizeof(views[index].view_projection));
        memcpy(views[index].right, camera->right, sizeof(views[index].right));
        memcpy(views[index].up, camera->up, sizeof(views[index].up));
        memcpy(views[index].position, camera->position, sizeof(views[index].position));
        const float sign = index == 0u ? -1.0f : 1.0f;
        views[index].clip[0] = sign;
        views[index].clip[1] = 0.0f;
        views[index].clip[2] = -sign * 0.5f * (float)WIDTH;
    }
    BTBHGPUSceneViewReport view_reports[2];
    return btbh_gpu_window_scene_present_views(smoke->window, set->paths[0], set->paths[1], set->paths[2],
        set->paths[3], set->paths[4], set->paths[5], set->paths[6], set->paths[7], set->paths[8], set->paths[9],
        set->paths[10], SMOKE_SHADER_FORMAT, WIDTH, HEIGHT, views, 2u, &scene->lighting, &scene->post,
        &scene->instances[0][0], scene->batch_count, scene->batch_count, scene->batches, NULL, NULL,
        scene->batch_count, 23u, 41u, 67u, 255u, view_reports, report);
}

/* ---- Read-backs ---- */

static uint8_t *read_gpu_texture(BTBHGPUWindow *window, SDL_GPUTexture *texture, uint32_t width, uint32_t height,
    uint32_t texel) {
    SDL_GPUDevice *device = window->device->gpu;
    const uint32_t size = width * height * texel;
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
    region.texture = texture;
    region.w = width;
    region.h = height;
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
    uint8_t *bytes = malloc(size);
    assert(bytes != NULL);
    const void *mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
    assert(mapped != NULL);
    memcpy(bytes, mapped, size);
    SDL_UnmapGPUTransferBuffer(device, transfer);
    SDL_ReleaseGPUTransferBuffer(device, transfer);
    return bytes;
}

static uint8_t *read_texture_rgba8(SmokeWindow *smoke, BTBHGPUTexture *texture) {
    assert(SDL_GPUTextureFormatTexelBlockSize(texture->format) == 4u);
    uint8_t *pixels = read_gpu_texture(smoke->window, texture->texture, texture->width, texture->height, 4u);
    if (texture->format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM ||
        texture->format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB) {
        for (uint32_t index = 0u; index < texture->width * texture->height * 4u; index += 4u) {
            const uint8_t blue = pixels[index];
            pixels[index] = pixels[index + 2u];
            pixels[index + 2u] = blue;
        }
    }
    return pixels;
}

static void dump(const char *name, const uint8_t *pixels) {
    const char *directory = SDL_getenv("BTBH_COVER_SMOKE_DUMP");
    if (directory == NULL) {
        return;
    }
    char path[1024];
    (void)snprintf(path, sizeof(path), "%s/%s.png", directory, name);
    (void)btbh_gpu_write_png_rgba8(path, WIDTH, HEIGHT, pixels);
}

/* Presents (which must succeed) and reads the frame back (or the target, for a render into it). */
static uint8_t *frame_of(SmokeWindow *smoke, const Scene *scene, BTBHGPUSceneReport *out_report) {
    BTBHGPUSceneReport report;
    memset(&report, 0, sizeof(report));
    const BTBHGPUResult result = present(smoke, scene, &report);
    if (result != BTBH_GPU_OK) {
        fprintf(stderr, "present: %s\n", report.message);
    }
    assert(result == BTBH_GPU_OK);
    if (out_report != NULL) {
        *out_report = report;
    }
    if (scene->into_target) {
        return read_texture_rgba8(smoke, smoke->target);
    }
    BTBHGPUTextureReport copied;
    const BTBHGPUResult copy_result = btbh_gpu_window_copy_pending_scene_texture(smoke->window, smoke->frame, &copied);
    if (copy_result != BTBH_GPU_OK) {
        fprintf(stderr, "copy: %s\n", copied.message);
    }
    assert(copy_result == BTBH_GPU_OK);
    return read_texture_rgba8(smoke, smoke->frame);
}

/* The opaque targets after a present, as floats (four a pixel): the ambient target, the specular target, the normal
   target. */
typedef struct Targets {
    float *ambient;
    float *specular;
    float *normal;
} Targets;

static float *decode_target(SmokeWindow *smoke, SDL_GPUTexture *texture, SDL_GPUTextureFormat format) {
    const uint32_t texel = SDL_GPUTextureFormatTexelBlockSize(format);
    uint8_t *bytes = read_gpu_texture(smoke->window, texture, WIDTH, HEIGHT, texel);
    float *values = malloc((size_t)WIDTH * HEIGHT * 4u * sizeof(float));
    assert(values != NULL);
    for (size_t index = 0u; index < (size_t)WIDTH * HEIGHT * 4u; ++index) {
        if (format == SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT) {
            uint16_t half = 0u;
            memcpy(&half, bytes + index * 2u, 2u);
            values[index] = btbh_hdr_half_to_float(half);
        } else if (format == SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT) {
            memcpy(&values[index], bytes + index * 4u, 4u);
        } else {
            assert(texel == 4u);
            values[index] = (float)bytes[index] / 255.0f;
        }
    }
    free(bytes);
    return values;
}

static Targets read_targets(SmokeWindow *smoke) {
    SDL_GPUTexture *ambient = NULL;
    SDL_GPUTexture *specular = NULL;
    SDL_GPUTexture *normal = NULL;
    SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_INVALID;
    uint32_t width = 0u;
    uint32_t height = 0u;
    btbh_gpu_window_scene_material_targets(smoke->window, &ambient, &specular, &normal, &format, &width, &height);
    assert(ambient != NULL && specular != NULL && normal != NULL && width == WIDTH && height == HEIGHT);
    Targets targets;
    targets.ambient = decode_target(smoke, ambient, format);
    targets.specular = decode_target(smoke, specular, format);
    targets.normal = decode_target(smoke, normal, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
    return targets;
}

static void free_targets(Targets *targets) {
    free(targets->ambient);
    free(targets->specular);
    free(targets->normal);
}

/* ---- The reference ---- */

static float srgb_of(float linear) {
    linear = linear < 0.0f ? 0.0f : (linear > 1.0f ? 1.0f : linear);
    return linear <= 0.0031308f ? 12.92f * linear : 1.055f * powf(linear, 1.0f / 2.4f) - 0.055f;
}

/* What a pixel's ray hits among `rects`: the nearest rect's index (-1: nothing), its point and the point's footprint
   (the larger x-z length of the fine screen derivatives within its 2 x 2 quad, on the rect's plane). */
typedef struct Hit {
    int rect;
    float point[3];
    float footprint;
} Hit;

static bool plane_point(const Rect *rect, const SmokeCamera *camera, float x, float y, float point[3]) {
    float origin[3];
    float direction[3];
    pixel_ray(camera, x, y, origin, direction);
    float t = 0.0f;
    (void)rect_hit(rect, origin, direction, &t, point);
    return t > 0.0f;
}

static Hit hit_at(const Rect *rects, uint32_t count, const SmokeCamera *camera, uint32_t px, uint32_t py) {
    Hit hit;
    hit.rect = -1;
    float origin[3];
    float direction[3];
    pixel_ray(camera, (float)px + 0.5f, (float)py + 0.5f, origin, direction);
    float nearest = INFINITY;
    for (uint32_t index = 0u; index < count; ++index) {
        float t = 0.0f;
        float point[3];
        if (rect_hit(&rects[index], origin, direction, &t, point) && t < nearest) {
            nearest = t;
            hit.rect = (int)index;
            memcpy(hit.point, point, sizeof(point));
        }
    }
    if (hit.rect < 0) {
        return hit;
    }
    const Rect *rect = &rects[hit.rect];
    const uint32_t x0 = px & ~1u;
    const uint32_t y0 = py & ~1u;
    float a[3];
    float b[3];
    float c[3];
    float d[3];
    (void)plane_point(rect, camera, (float)x0 + 0.5f, (float)py + 0.5f, a);
    (void)plane_point(rect, camera, (float)x0 + 1.5f, (float)py + 0.5f, b);
    (void)plane_point(rect, camera, (float)px + 0.5f, (float)y0 + 0.5f, c);
    (void)plane_point(rect, camera, (float)px + 0.5f, (float)y0 + 1.5f, d);
    const float dx = sqrtf((b[0] - a[0]) * (b[0] - a[0]) + (b[2] - a[2]) * (b[2] - a[2]));
    const float dy = sqrtf((d[0] - c[0]) * (d[0] - c[0]) + (d[2] - c[2]) * (d[2] - c[2]));
    hit.footprint = dx > dy ? dx : dy;
    return hit;
}

/* Whether a pixel lies well inside its rect (a pixel's margin from every edge in every direction), so its colour is the
   rect's alone, not an edge's coverage. */
static bool inside_margin(const Rect *rects, uint32_t count, const SmokeCamera *camera, uint32_t px, uint32_t py) {
    const Hit centre = hit_at(rects, count, camera, px, py);
    if (centre.rect < 0) {
        return false;
    }
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const int x = (int)px + dx;
            const int y = (int)py + dy;
            if (x < 0 || y < 0 || x >= (int)WIDTH || y >= (int)HEIGHT) {
                return false;
            }
            if (hit_at(rects, count, camera, (uint32_t)x, (uint32_t)y).rect != centre.rect) {
                return false;
            }
        }
    }
    return true;
}

typedef struct Compare {
    uint32_t compared;
    uint32_t over_tolerance;
    int largest;
} Compare;

static void compare_report(const char *name, const Compare *result, double outliers) {
    const double share = result->compared > 0u ? (double)result->over_tolerance / (double)result->compared : 1.0;
    printf("%s: %u pixels, %u (%.3f %%) over %d levels, largest %d\n", name, result->compared, result->over_tolerance,
        100.0 * share, COLOUR_TOLERANCE, result->largest);
    assert(result->compared > 2000u);
    assert(share <= outliers);
}

/* ---- The cover ---- */

static void set_cover(SmokeWindow *smoke, const BTBHGPUSceneCover *cover) {
    BTBHGPUSceneReport report;
    memset(&report, 0, sizeof(report));
    const BTBHGPUResult result = btbh_gpu_window_scene_set_cover(smoke->window, cover, &report);
    if (result != BTBH_GPU_OK) {
        fprintf(stderr, "set_cover: %s\n", report.message);
    }
    assert(result == BTBH_GPU_OK && report.result == BTBH_GPU_OK);
}

static BTBHGPUSceneCoverLayer snow_layer(void) {
    BTBHGPUSceneCoverLayer layer;
    memset(&layer, 0, sizeof(layer));
    layer.color[0] = 0.82f;
    layer.color[1] = 0.85f;
    layer.color[2] = 0.9f;
    layer.amount = 0.75f;
    layer.slope_degrees = 60.0f;
    layer.edge_degrees = 30.0f;
    layer.pattern = BTBH_GPU_SCENE_COVER_SNOW;
    return layer;
}

/* The frame's shadows in check_unlit_case (0 none, 1 the sun's, 2 the sun's and an omni light's): unlit facets draw
   the same under each, through the cover variant of the scene shader each picks (_unshadowed, _2d_shadows and the
   plain one). */
static int unlit_shadows;

static void identity4(float matrix[16]) {
    memset(matrix, 0, 16u * sizeof(float));
    matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
}

/* The covered unlit frame of the facets against the reference. layer_index: which layer the batches take. tilt: the
   facets' normal map leans their shading normal by asin(0.2) toward +x (tilt_map). debug: BTBH_DEBUG_COVER. */
static void check_unlit_case(SmokeWindow *smoke, const char *name, const BTBHGPUSceneCover *cover, uint32_t layer_index,
    bool tilt, bool debug) {
    set_cover(smoke, cover);
    Scene scene = facets_scene(smoke, true,
        layer_index == 0u ? (uint8_t)BTBH_GPU_SCENE_BATCH_FLAGS2_COVER_0
                          : (uint8_t)BTBH_GPU_SCENE_BATCH_FLAGS2_COVER_1);
    if (unlit_shadows >= 1) {
        identity4(scene.lighting.directional_shadow_view_projection);
        scene.lighting.directional_shadow_enabled = 1u;
        scene.lighting.directional_shadow_softness = 1.0f;
    }
    if (unlit_shadows >= 2) {
        scene.lighting.spot_position_y = 3.0f;
        scene.lighting.spot_direction_y = -1.0f;
        scene.lighting.spot_outer_cutoff = -1.0f;
        scene.lighting.spot_shadow_enabled = 1u;
        scene.lighting.spot_shadow_is_omni = 1u;
        scene.lighting.spot_shadow_near = 0.1f;
        scene.lighting.spot_shadow_softness = 1.0f;
        for (uint32_t face = 0u; face < 6u; ++face) {
            identity4(scene.lighting.omni_shadow_view_projection[face]);
        }
    }
    if (tilt) {
        for (uint32_t index = 0u; index < FACETS; ++index) {
            scene.batches[index].normal_texture = smoke->tilt_map;
        }
    }
    if (debug) {
        (void)SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "BTBH_DEBUG_COVER", "1", true);
    }
    uint8_t *frame = frame_of(smoke, &scene, NULL);
    if (debug) {
        (void)SDL_UnsetEnvironmentVariable(SDL_GetEnvironment(), "BTBH_DEBUG_COVER");
    }
    dump(name, frame);
    BTBHGPUCoverUniforms block;
    btbh_cover_pack(cover, debug, &block);
    Compare result;
    memset(&result, 0, sizeof(result));
    uint32_t covered = 0u;
    uint32_t bare = 0u;
    /* The tilt map's texel: 153/255 -> 0.2 along the tangent, 128/255 -> 1/255 along the bitangent (normal_scale 1),
       z rebuilt; the bitangent is cross(normal, tangent) (production_model.vert). */
    const float tilt_x = 153.0f / 255.0f * 2.0f - 1.0f;
    const float tilt_y = 128.0f / 255.0f * 2.0f - 1.0f;
    const float tilt_z = sqrtf(1.0f - (tilt_x * tilt_x + tilt_y * tilt_y));
    for (uint32_t py = 0u; py < HEIGHT; ++py) {
        for (uint32_t px = 0u; px < WIDTH; ++px) {
            if (!inside_margin(facets, FACETS, &scene.camera, px, py)) {
                continue;
            }
            const Hit hit = hit_at(facets, FACETS, &scene.camera, px, py);
            const Rect *rect = &facets[hit.rect];
            float shading[3];
            memcpy(shading, rect->normal, sizeof(shading));
            if (tilt) {
                float bitangent[3];
                cross3(rect->normal, rect->u, bitangent);
                for (int axis = 0; axis < 3; ++axis) {
                    shading[axis] = rect->u[axis] * tilt_x + bitangent[axis] * tilt_y + rect->normal[axis] * tilt_z;
                }
                normalize3(shading);
            }
            const BTBHCoverSample sample = btbh_cover_sample(&block, layer_index, hit.point, hit.footprint,
                rect->normal, shading, facet_albedo, 1.0f, 0.0f, 1.0f, 0.0f);
            float expected[3];
            if (debug) {
                expected[0] = layer_index == 0u ? sample.c : 0.0f;
                expected[1] = layer_index == 0u ? 0.0f : sample.c;
                expected[2] = 0.0f;
            } else {
                memcpy(expected, sample.albedo, sizeof(expected));
            }
            covered += sample.c > 0.99f ? 1u : 0u;
            bare += sample.c < 0.01f ? 1u : 0u;
            const uint8_t *pixel = &frame[((size_t)py * WIDTH + px) * 4u];
            int worst = 0;
            for (int channel = 0; channel < 3; ++channel) {
                const int want = (int)floorf(srgb_of(expected[channel]) * 255.0f + 0.5f);
                const int difference = abs((int)pixel[channel] - want);
                worst = difference > worst ? difference : worst;
            }
            result.compared += 1u;
            result.over_tolerance += worst > COLOUR_TOLERANCE ? 1u : 0u;
            result.largest = worst > result.largest ? worst : result.largest;
        }
    }
    printf("%s: %u pixels covered, %u bare\n", name, covered, bare);
    compare_report(name, &result, COLOUR_OUTLIERS);
    assert(result.largest <= COLOUR_LIMIT || result.over_tolerance * 1000u <= result.compared);
    assert(covered > 500u && bare > 500u);
    free(frame);
}

static void check_patterns(SmokeWindow *smoke) {
    BTBHGPUSceneCover cover;
    memset(&cover, 0, sizeof(cover));
    /* SNOW without noise: each facet one c (reach 45 degrees, fading over 30: 1 to 15, 0.5 at 30, 0 from 45). */
    cover.layers[0] = snow_layer();
    check_unlit_case(smoke, "snow-clean", &cover, 0u, false, false);
    /* With noise. */
    cover.layers[0].edge_degrees = 10.0f;
    cover.layers[0].edge_noise_degrees = 20.0f;
    cover.layers[0].noise_scale = 1.2f;
    check_unlit_case(smoke, "snow-noise", &cover, 0u, false, false);
    cover.layers[0].pattern = BTBH_GPU_SCENE_COVER_SAND;
    cover.layers[0].color[0] = 0.55f;
    cover.layers[0].color[1] = 0.42f;
    cover.layers[0].color[2] = 0.25f;
    check_unlit_case(smoke, "sand", &cover, 0u, false, false);
    cover.layers[0].pattern = BTBH_GPU_SCENE_COVER_MUD;
    cover.layers[0].color[0] = 0.12f;
    cover.layers[0].color[1] = 0.08f;
    cover.layers[0].color[2] = 0.05f;
    check_unlit_case(smoke, "mud", &cover, 0u, false, false);
    cover.layers[0] = snow_layer();
    cover.layers[0].edge_degrees = 10.0f;
    cover.layers[0].edge_noise_degrees = 20.0f;
    cover.layers[0].noise_scale = 1.2f;
    cover.layers[0].flags = BTBH_GPU_SCENE_COVER_LAYER_COARSE;
    check_unlit_case(smoke, "snow-coarse", &cover, 0u, false, false);
    /* Noise finer than a pixel (its octaves fading, then gone): the edge averaged over what they lost. */
    cover.layers[0].flags = 0u;
    cover.layers[0].noise_scale = 12.0f;
    check_unlit_case(smoke, "snow-fine-noise", &cover, 0u, false, false);
    cover.layers[0].noise_scale = 60.0f;
    check_unlit_case(smoke, "snow-finer-noise", &cover, 0u, false, false);
    cover.layers[0].pattern = BTBH_GPU_SCENE_COVER_SAND;
    check_unlit_case(smoke, "sand-finer-noise", &cover, 0u, false, false);
    cover.layers[0].pattern = BTBH_GPU_SCENE_COVER_SNOW;
    cover.layers[0].flags = BTBH_GPU_SCENE_COVER_LAYER_COARSE;
    cover.layers[0].noise_scale = 1.2f;
    /* The second layer through COVER_1 (the first, different, unused). */
    cover.layers[1] = cover.layers[0];
    cover.layers[1].flags = 0u;
    cover.layers[1].pattern = BTBH_GPU_SCENE_COVER_SAND;
    cover.layers[1].amount = 0.5f;
    cover.layers[1].color[0] = 0.6f;
    check_unlit_case(smoke, "second-layer", &cover, 1u, false, false);
    /* normal_influence: the facets' normal maps lean their facing. */
    memset(&cover, 0, sizeof(cover));
    cover.layers[0] = snow_layer();
    cover.layers[0].normal_influence = 1.0f;
    check_unlit_case(smoke, "normal-influence", &cover, 0u, true, false);
    /* BTBH_DEBUG_COVER: c in red. */
    cover.layers[0].normal_influence = 0.0f;
    cover.layers[0].edge_noise_degrees = 15.0f;
    check_unlit_case(smoke, "debug", &cover, 0u, false, true);
    /* The frame's shadow sets: the sun's (production_model_cover_2d_shadows.frag), the sun's and an omni light's
       (production_model_cover.frag). */
    cover.layers[0] = snow_layer();
    cover.layers[0].edge_degrees = 10.0f;
    cover.layers[0].edge_noise_degrees = 20.0f;
    cover.layers[0].noise_scale = 1.2f;
    unlit_shadows = 1;
    check_unlit_case(smoke, "snow-noise-sun-shadow", &cover, 0u, false, false);
    unlit_shadows = 2;
    check_unlit_case(smoke, "snow-noise-omni-shadow", &cover, 0u, false, false);
    unlit_shadows = 0;
}

/* Without a cover, with a cover and no bits, with bits on every layer at amount 0: the same bytes. */
static void check_identity(SmokeWindow *smoke) {
    set_cover(smoke, NULL);
    for (int lit = 0; lit < 2; ++lit) {
        Scene scene = facets_scene(smoke, lit == 0, 0u);
        if (lit != 0) {
            post_reflections(&scene.post);
        }
        uint8_t *plain = frame_of(smoke, &scene, NULL);
        BTBHGPUSceneCover cover;
        memset(&cover, 0, sizeof(cover));
        cover.layers[0] = snow_layer();
        cover.layers[1] = snow_layer();
        set_cover(smoke, &cover);
        uint8_t *no_bits = frame_of(smoke, &scene, NULL);
        cover.layers[0].amount = 0.0f;
        cover.layers[0].edge_noise_degrees = 30.0f;
        cover.layers[1].amount = 0.0f;
        set_cover(smoke, &cover);
        for (uint32_t index = 0u; index < scene.batch_count; ++index) {
            scene.batches[index].flags2 = index % 2u == 0u ? (uint8_t)BTBH_GPU_SCENE_BATCH_FLAGS2_COVER_0 :
                (uint8_t)BTBH_GPU_SCENE_BATCH_FLAGS2_COVER_1;
        }
        uint8_t *amount_zero = frame_of(smoke, &scene, NULL);
        cover.layers[0].amount = 0.75f;
        set_cover(smoke, &cover);
        uint8_t *covered = frame_of(smoke, &scene, NULL);
        const size_t bytes = (size_t)WIDTH * HEIGHT * 4u;
        assert(memcmp(plain, no_bits, bytes) == 0);
        assert(memcmp(plain, amount_zero, bytes) == 0);
        assert(memcmp(plain, covered, bytes) != 0);
        free(plain);
        free(no_bits);
        free(amount_zero);
        free(covered);
        set_cover(smoke, NULL);
    }
    printf("identity: a cover with no bit, and bits with every layer at amount 0, draw byte for byte as none (unlit "
           "and lit)\n");
}

/* The lit targets: MUD with puddles (wet) and SNOW with relief, against the reference. */
static void check_targets(SmokeWindow *smoke) {
    for (int which = 0; which < 2; ++which) {
        BTBHGPUSceneCover cover;
        memset(&cover, 0, sizeof(cover));
        cover.layers[0] = snow_layer();
        cover.layers[0].edge_degrees = 10.0f;
        cover.layers[0].edge_noise_degrees = 20.0f;
        cover.layers[0].noise_scale = 1.2f;
        if (which == 0) {
            cover.layers[0].pattern = BTBH_GPU_SCENE_COVER_MUD;
        } else {
            cover.layers[0].bump = 0.8f;
        }
        set_cover(smoke, &cover);
        Scene scene = facets_scene(smoke, false, (uint8_t)BTBH_GPU_SCENE_BATCH_FLAGS2_COVER_0);
        post_reflections(&scene.post);
        scene.lighting.surface_wetness = 0.5f;
        uint8_t *frame = frame_of(smoke, &scene, NULL);
        dump(which == 0 ? "targets-mud" : "targets-snow-relief", frame);
        free(frame);
        Targets targets = read_targets(smoke);
        BTBHGPUCoverUniforms block;
        btbh_cover_pack(&cover, false, &block);
        uint32_t compared = 0u;
        uint32_t roughness_off = 0u;
        uint32_t occlusion_off = 0u;
        uint32_t normal_off = 0u;
        float largest_roughness = 0.0f;
        float lowest_roughness = 1.0f;
        float largest_normal = 0.0f;
        for (uint32_t py = 0u; py < HEIGHT; ++py) {
            for (uint32_t px = 0u; px < WIDTH; ++px) {
                if (!inside_margin(facets, FACETS, &scene.camera, px, py)) {
                    continue;
                }
                const Hit hit = hit_at(facets, FACETS, &scene.camera, px, py);
                const Rect *rect = &facets[hit.rect];
                const BTBHCoverSample sample = btbh_cover_sample(&block, 0u, hit.point, hit.footprint, rect->normal,
                    rect->normal, facet_albedo, 0.5f, 0.0f, 0.4f, 0.5f);
                const size_t at = ((size_t)py * WIDTH + px) * 4u;
                const float roughness = targets.specular[at + 3u];
                const float occlusion = targets.normal[at + 3u];
                float normal_error = 0.0f;
                for (int axis = 0; axis < 3; ++axis) {
                    const float decoded = targets.normal[at + (size_t)axis] * 2.0f - 1.0f;
                    const float error = fabsf(decoded - sample.normal[axis]);
                    normal_error = error > normal_error ? error : normal_error;
                }
                compared += 1u;
                roughness_off += fabsf(roughness - sample.roughness) > 0.01f ? 1u : 0u;
                occlusion_off += fabsf(occlusion - sample.occlusion) > 2.5f / 255.0f ? 1u : 0u;
                normal_off += normal_error > 0.02f ? 1u : 0u;
                largest_roughness = fabsf(roughness - sample.roughness) > largest_roughness
                    ? fabsf(roughness - sample.roughness) : largest_roughness;
                largest_normal = normal_error > largest_normal ? normal_error : largest_normal;
                lowest_roughness = sample.c > 0.99f && roughness < lowest_roughness ? roughness : lowest_roughness;
            }
        }
        printf("targets (%s): %u pixels; roughness off by more than 0.01 at %u (largest %.4f; lowest covered %.3f), "
               "occlusion at %u, normal by more than 0.02 at %u (largest %.4f)\n", which == 0 ? "MUD, wet" :
               "SNOW, relief", compared, roughness_off, (double)largest_roughness, (double)lowest_roughness,
               occlusion_off, normal_off, (double)largest_normal);
        assert(compared > 2000u);
        assert(roughness_off * 200u <= compared && occlusion_off * 200u <= compared && normal_off * 200u <= compared);
        if (which == 0) {
            /* The puddles: covered pixels down near the puddles' roughness (wetness 0.5: 0.55 eased 75 % of the way to
               0.12 at the deepest). */
            assert(lowest_roughness < 0.3f);
        }
        free_targets(&targets);
    }
    printf("targets: the roughness (MUD's puddles), the occlusion and the normal (SNOW's relief) the screen-space "
           "passes read are the reference's\n");
}

/* Under the radiance cascades the scene shader's albedo target holds the covered albedo. */
static void check_cascades(SmokeWindow *smoke) {
    BTBHGPUSceneCover cover;
    memset(&cover, 0, sizeof(cover));
    cover.layers[0] = snow_layer();
    cover.layers[0].edge_degrees = 10.0f;
    cover.layers[0].edge_noise_degrees = 20.0f;
    cover.layers[0].noise_scale = 1.2f;
    set_cover(smoke, &cover);
    Scene scene = facets_scene(smoke, false, (uint8_t)BTBH_GPU_SCENE_BATCH_FLAGS2_COVER_0);
    scene.post.radiance_cascades.tier = BTBH_GPU_SCENE_RC_LOW;
    scene.post.radiance_cascades.auto_frame_budget_milliseconds = 16.0f;
    scene.post.radiance_cascades.indirect_intensity = 0.8f;
    scene.post.radiance_cascades.hero_omni_emitter_index = -1;
    scene.post.radiance_cascades.invalidate = 1u;
    BTBHGPUSceneReport report;
    uint8_t *frame = frame_of(smoke, &scene, &report);
    dump("cascades", frame);
    free(frame);
    if (report.radiance_cascades.active_tier == BTBH_GPU_SCENE_RC_OFF) {
        printf("cascades: not active on this device; skipped\n");
        return;
    }
    Targets targets = read_targets(smoke);
    BTBHGPUCoverUniforms block;
    btbh_cover_pack(&cover, false, &block);
    uint32_t compared = 0u;
    uint32_t off = 0u;
    float largest = 0.0f;
    for (uint32_t py = 0u; py < HEIGHT; ++py) {
        for (uint32_t px = 0u; px < WIDTH; ++px) {
            if (!inside_margin(facets, FACETS, &scene.camera, px, py)) {
                continue;
            }
            const Hit hit = hit_at(facets, FACETS, &scene.camera, px, py);
            const Rect *rect = &facets[hit.rect];
            const BTBHCoverSample sample = btbh_cover_sample(&block, 0u, hit.point, hit.footprint, rect->normal,
                rect->normal, facet_albedo, 0.5f, 0.0f, 0.4f, 0.0f);
            const size_t at = ((size_t)py * WIDTH + px) * 4u;
            float error = 0.0f;
            for (int channel = 0; channel < 3; ++channel) {
                const float difference = fabsf(targets.ambient[at + (size_t)channel] - sample.albedo[channel]);
                error = difference > error ? difference : error;
            }
            compared += 1u;
            off += error > 0.01f ? 1u : 0u;
            largest = error > largest ? error : largest;
        }
    }
    printf("cascades: the albedo target holds the covered albedo at %u pixels (%u off by more than 0.01, largest "
           "%.4f)\n", compared, off, (double)largest);
    assert(compared > 2000u && off * 200u <= compared);
    free_targets(&targets);
}

/* SSR: a glossy floor reflects a bright wall; covered by rough snow, it reflects nothing. */
static void check_reflections(SmokeWindow *smoke) {
    const float dark[3] = {0.05f, 0.05f, 0.06f};
    const float bright[3] = {1.0f, 0.75f, 0.2f};
    Scene scene;
    memset(&scene, 0, sizeof(scene));
    scene.camera = look_at(floor_eye, floor_target);
    scene.lighting = lighting_plain();
    scene.post = post_plain();
    scene.shaders = &shaders;
    instance(scene.instances[0], dark);
    instance(scene.instances[1], bright);
    scene.batches[0] = batch_of(smoke->floor, 0u, false);
    scene.batches[0].roughness = 0.05f;
    scene.batches[0].occlusion = 1.0f;
    scene.batches[1] = batch_of(smoke->wall, 1u, true);
    scene.batch_count = 2u;
    uint32_t differing[2] = {0u, 0u};
    for (int covered = 0; covered < 2; ++covered) {
        BTBHGPUSceneCover cover;
        memset(&cover, 0, sizeof(cover));
        cover.layers[0] = snow_layer();
        cover.layers[0].amount = 1.0f;
        memcpy(cover.layers[0].color, dark, sizeof(dark));
        set_cover(smoke, covered != 0 ? &cover : NULL);
        scene.batches[0].flags2 = covered != 0 ? (uint8_t)BTBH_GPU_SCENE_BATCH_FLAGS2_COVER_0 : 0u;
        scene.post = post_plain();
        post_reflections(&scene.post);
        uint8_t *with = frame_of(smoke, &scene, NULL);
        scene.post.ssr_intensity = 0.0f;
        uint8_t *without = frame_of(smoke, &scene, NULL);
        dump(covered != 0 ? "ssr-covered" : "ssr-glossy", with);
        for (uint32_t py = 0u; py < HEIGHT; ++py) {
            for (uint32_t px = 0u; px < WIDTH; ++px) {
                const Hit hit = hit_at(&floor_rect, 1u, &scene.camera, px, py);
                if (hit.rect < 0 || hit_at(&wall_rect, 1u, &scene.camera, px, py).rect >= 0) {
                    continue;
                }
                const uint8_t *a = &with[((size_t)py * WIDTH + px) * 4u];
                const uint8_t *b = &without[((size_t)py * WIDTH + px) * 4u];
                int worst = 0;
                for (int channel = 0; channel < 3; ++channel) {
                    const int difference = abs((int)a[channel] - (int)b[channel]);
                    worst = difference > worst ? difference : worst;
                }
                differing[covered] += worst > 2 ? 1u : 0u;
            }
        }
        free(with);
        free(without);
    }
    printf("reflections: the glossy floor's pixels changed by SSR %u, covered by rough snow %u\n", differing[0],
        differing[1]);
    assert(differing[0] > 300u && differing[1] == 0u);
    set_cover(smoke, NULL);
}

/* A see-through lit batch takes the cover opaque where it lies: its covered pixels equal the opaque batch's. */
static void check_see_through(SmokeWindow *smoke) {
    BTBHGPUSceneCover cover;
    memset(&cover, 0, sizeof(cover));
    cover.layers[0] = snow_layer();
    set_cover(smoke, &cover);
    Scene scene = facets_scene(smoke, false, (uint8_t)BTBH_GPU_SCENE_BATCH_FLAGS2_COVER_0);
    uint8_t *opaque = frame_of(smoke, &scene, NULL);
    for (uint32_t index = 0u; index < FACETS; ++index) {
        scene.batches[index].transparency_enabled = 1u;
        scene.batches[index].albedo_alpha = 96u;
    }
    uint8_t *see_through = frame_of(smoke, &scene, NULL);
    for (uint32_t index = 0u; index < FACETS; ++index) {
        scene.batches[index].flags2 = 0u;
    }
    set_cover(smoke, NULL);
    uint8_t *bare = frame_of(smoke, &scene, NULL);
    dump("see-through", see_through);
    BTBHGPUCoverUniforms block;
    btbh_cover_pack(&cover, false, &block);
    uint32_t full = 0u;
    uint32_t matching = 0u;
    uint32_t blended = 0u;
    for (uint32_t py = 0u; py < HEIGHT; ++py) {
        for (uint32_t px = 0u; px < WIDTH; ++px) {
            if (!inside_margin(facets, FACETS, &scene.camera, px, py)) {
                continue;
            }
            const Hit hit = hit_at(facets, FACETS, &scene.camera, px, py);
            if (facet_slopes[hit.rect] > 15.0f) {
                continue;
            }
            full += 1u;
            const size_t at = ((size_t)py * WIDTH + px) * 4u;
            int worst = 0;
            int bare_difference = 0;
            for (int channel = 0; channel < 3; ++channel) {
                const int difference = abs((int)see_through[at + (size_t)channel] - (int)opaque[at + (size_t)channel]);
                worst = difference > worst ? difference : worst;
                const int other = abs((int)see_through[at + (size_t)channel] - (int)bare[at + (size_t)channel]);
                bare_difference = other > bare_difference ? other : bare_difference;
            }
            matching += worst <= 2 ? 1u : 0u;
            blended += bare_difference > 10 ? 1u : 0u;
        }
    }
    printf("see-through: %u fully covered pixels, %u as the opaque batch's, %u apart from the bare see-through one\n",
        full, matching, blended);
    assert(full > 500u && matching * 100u >= full * 99u && blended * 100u >= full * 99u);
    free(opaque);
    free(see_through);
    free(bare);
}

/* ---- Still ice ---- */

static const float ice_albedo[3] = {0.25f, 0.35f, 0.45f};

static Scene ice_scene(const SmokeWindow *smoke, bool unlit) {
    Scene scene;
    memset(&scene, 0, sizeof(scene));
    scene.camera = look_at(ice_eye, ice_target);
    scene.lighting = lighting_plain();
    scene.post = post_plain();
    scene.shaders = &shaders;
    instance(scene.instances[0], ice_albedo);
    scene.batches[0] = batch_of(smoke->ice, 0u, unlit);
    scene.batches[0].surface_variant = BTBH_GPU_SCENE_SURFACE_ICE;
    scene.batches[0].roughness = 0.07f;   /* (v2's water roughness: ICE's is its own, not scaled by it) */
    scene.batches[0].occlusion = 1.0f;
    scene.batches[0].surface_parameters0[0] = 1.2f;
    scene.batches[0].surface_parameters0[1] = 0.08f;
    scene.batches[0].surface_parameters0[2] = 0.5f;
    scene.batches[0].surface_parameters0[3] = 0.035f;
    scene.batches[0].surface_parameters1[0] = 0.35f;
    scene.batches[0].surface_parameters1[1] = 3.0f;
    scene.batches[0].surface_parameters1[2] = 0.06f;
    scene.batches[0].surface_parameters1[3] = 0.6f;
    scene.batch_count = 1u;
    return scene;
}

static void check_ice(SmokeWindow *smoke) {
    set_cover(smoke, NULL);
    Scene scene = ice_scene(smoke, true);
    uint8_t *frame = frame_of(smoke, &scene, NULL);
    dump("ice-unlit", frame);
    float p0[4];
    float p1[4];
    btbh_ice_defaults(scene.batches[0].surface_parameters0, scene.batches[0].surface_parameters1, p0, p1);
    Compare result;
    memset(&result, 0, sizeof(result));
    uint32_t lifted = 0u;
    for (uint32_t py = 0u; py < HEIGHT; ++py) {
        for (uint32_t px = 0u; px < WIDTH; ++px) {
            if (!inside_margin(&ice_rect, 1u, &scene.camera, px, py)) {
                continue;
            }
            const Hit hit = hit_at(&ice_rect, 1u, &scene.camera, px, py);
            const BTBHIceSample ice = btbh_ice_sample(hit.point[0], hit.point[2], hit.footprint, p0, p1);
            const uint8_t *pixel = &frame[((size_t)py * WIDTH + px) * 4u];
            int worst = 0;
            for (int channel = 0; channel < 3; ++channel) {
                float top = ice_albedo[channel] * (1.0f + p0[2]);
                top = top < 1.0f ? top : 1.0f;
                const float expected = ice_albedo[channel] + (top - ice_albedo[channel]) * ice.lift_share;
                const int want = (int)floorf(srgb_of(expected) * 255.0f + 0.5f);
                const int difference = abs((int)pixel[channel] - want);
                worst = difference > worst ? difference : worst;
            }
            lifted += ice.lift_share > 0.5f ? 1u : 0u;
            result.compared += 1u;
            result.over_tolerance += worst > COLOUR_TOLERANCE ? 1u : 0u;
            result.largest = worst > result.largest ? worst : result.largest;
        }
    }
    printf("ice (unlit): %u pixels lifted past half\n", lifted);
    compare_report("ice (unlit)", &result, ICE_OUTLIERS);
    assert(lifted > 1000u && lifted * 2u < result.compared);
    free(frame);

    /* Lit: the roughness (its own: the batch's 0.07 doesn't scale it) and the normal in the targets. */
    scene = ice_scene(smoke, false);
    post_reflections(&scene.post);
    frame = frame_of(smoke, &scene, NULL);
    dump("ice-lit", frame);
    free(frame);
    Targets targets = read_targets(smoke);
    uint32_t compared = 0u;
    uint32_t roughness_off = 0u;
    uint32_t normal_off = 0u;
    uint32_t clear = 0u;
    float tilt_sum = 0.0f;
    for (uint32_t py = 0u; py < HEIGHT; ++py) {
        for (uint32_t px = 0u; px < WIDTH; ++px) {
            if (!inside_margin(&ice_rect, 1u, &scene.camera, px, py)) {
                continue;
            }
            const Hit hit = hit_at(&ice_rect, 1u, &scene.camera, px, py);
            const BTBHIceSample ice = btbh_ice_sample(hit.point[0], hit.point[2], hit.footprint, p0, p1);
            float normal[3] = {-ice.gradient[0], 1.0f, -ice.gradient[1]};
            normalize3(normal);
            const size_t at = ((size_t)py * WIDTH + px) * 4u;
            float normal_error = 0.0f;
            for (int axis = 0; axis < 3; ++axis) {
                const float error = fabsf(targets.normal[at + (size_t)axis] * 2.0f - 1.0f - normal[axis]);
                normal_error = error > normal_error ? error : normal_error;
            }
            compared += 1u;
            roughness_off += fabsf(targets.specular[at + 3u] - ice.roughness) > 0.01f ? 1u : 0u;
            normal_off += normal_error > 0.02f ? 1u : 0u;
            clear += ice.lift_share == 0.0f && fabsf(targets.specular[at + 3u] - 0.06f) < 0.002f ? 1u : 0u;
            tilt_sum += 1.0f - normal[1];
        }
    }
    printf("ice (lit targets): %u pixels; roughness off by more than 0.01 at %u, normal by more than 0.02 at %u; %u "
           "clear at 0.06\n", compared, roughness_off, normal_off, clear);
    assert(compared > 2000u && roughness_off * 50u <= compared && normal_off * 50u <= compared && clear > 1000u);
    assert(tilt_sum > 0.0f);
    free_targets(&targets);

    /* The zero values draw (and differ from plain water). */
    scene = ice_scene(smoke, true);
    memset(scene.batches[0].surface_parameters0, 0, sizeof(scene.batches[0].surface_parameters0));
    memset(scene.batches[0].surface_parameters1, 0, sizeof(scene.batches[0].surface_parameters1));
    uint8_t *defaults = frame_of(smoke, &scene, NULL);
    scene.batches[0].surface_variant = BTBH_GPU_SCENE_SURFACE_NONE;
    uint8_t *plain = frame_of(smoke, &scene, NULL);
    dump("ice-defaults", defaults);
    assert(memcmp(defaults, plain, (size_t)WIDTH * HEIGHT * 4u) != 0);
    free(defaults);
    free(plain);
    printf("ice: the lift, the roughness and the normal are the reference's; the zero values draw\n");
}

/* ---- Runs, views, a render into a target ---- */

static void check_runs(SmokeWindow *smoke, bool verify_requested) {
    BTBHGPUSceneCover cover;
    memset(&cover, 0, sizeof(cover));
    cover.layers[0] = snow_layer();
    set_cover(smoke, &cover);
    uint32_t runs[2] = {0u, 0u};
    for (int covered = 0; covered < 2; ++covered) {
        Scene scene = facets_scene(smoke, false, covered != 0 ? (uint8_t)BTBH_GPU_SCENE_BATCH_FLAGS2_COVER_0 : 0u);
        uint8_t *frame = frame_of(smoke, &scene, NULL);
        free(frame);
        BTBHGPUSceneTelemetryReport telemetry;
        memset(&telemetry, 0, sizeof(telemetry));
        assert(btbh_gpu_window_scene_read_telemetry(smoke->window, &telemetry) != 0u);
        runs[covered] = telemetry.geometry_run_draw_count;
    }
    printf("runs: %u run draws uncovered, %u covered (the eight facets share a material)\n", runs[0], runs[1]);
    assert(runs[0] > 0u && runs[1] == runs[0]);
    /* A bit whose layer is off joins the run of a batch without one: every other facet with COVER_0, the first layer
       off (the second on), draws in as many runs as without the bits, and the same frame. */
    BTBHGPUSceneCover second_only;
    memset(&second_only, 0, sizeof(second_only));
    second_only.layers[1] = snow_layer();
    set_cover(smoke, &second_only);
    Scene mixed = facets_scene(smoke, false, 0u);
    for (uint32_t index = 1u; index < FACETS; index += 2u) {
        mixed.batches[index].flags2 = (uint8_t)BTBH_GPU_SCENE_BATCH_FLAGS2_COVER_0;
    }
    uint8_t *mixed_frame = frame_of(smoke, &mixed, NULL);
    BTBHGPUSceneTelemetryReport mixed_telemetry;
    memset(&mixed_telemetry, 0, sizeof(mixed_telemetry));
    assert(btbh_gpu_window_scene_read_telemetry(smoke->window, &mixed_telemetry) != 0u);
    Scene bare = facets_scene(smoke, false, 0u);
    uint8_t *bare_frame = frame_of(smoke, &bare, NULL);
    printf("runs: every other facet with a bit whose layer is off: %u run draws, the frame %s the bare one's\n",
        mixed_telemetry.geometry_run_draw_count,
        memcmp(mixed_frame, bare_frame, (size_t)WIDTH * HEIGHT * 4u) == 0 ? "byte for byte" : "NOT");
    assert(mixed_telemetry.geometry_run_draw_count == runs[0]);
    assert(memcmp(mixed_frame, bare_frame, (size_t)WIDTH * HEIGHT * 4u) == 0);
    free(mixed_frame);
    free(bare_frame);
    set_cover(smoke, &cover);
    /* The same in the run verifier's mode (unless the whole smoke runs in it). */
    if (!verify_requested) {
        (void)SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "BTBH_GPU_SCENE_RUNS", "verify", true);
        BTBHGPUSceneRunVerifyReport before;
        memset(&before, 0, sizeof(before));
        (void)btbh_gpu_window_scene_read_run_verify(smoke->window, &before);
        Scene scene = facets_scene(smoke, false, (uint8_t)BTBH_GPU_SCENE_BATCH_FLAGS2_COVER_0);
        uint8_t *frame = frame_of(smoke, &scene, NULL);
        free(frame);
        /* And the mixed runs, the first layer off. */
        set_cover(smoke, &second_only);
        frame = frame_of(smoke, &mixed, NULL);
        free(frame);
        set_cover(smoke, &cover);
        BTBHGPUSceneRunVerifyReport after;
        memset(&after, 0, sizeof(after));
        assert(btbh_gpu_window_scene_read_run_verify(smoke->window, &after) != 0u);
        (void)SDL_UnsetEnvironmentVariable(SDL_GetEnvironment(), "BTBH_GPU_SCENE_RUNS");
        printf("runs: verified %llu covered and mixed run draws, %llu mismatches\n",
            (unsigned long long)(after.checked_draw_count - before.checked_draw_count),
            (unsigned long long)(after.mismatch_count - before.mismatch_count));
        assert(after.checked_draw_count > before.checked_draw_count && after.mismatch_count == before.mismatch_count);
    }
    set_cover(smoke, NULL);
}

static void check_views_and_target(SmokeWindow *smoke) {
    BTBHGPUSceneCover cover;
    memset(&cover, 0, sizeof(cover));
    cover.layers[0] = snow_layer();
    cover.layers[0].edge_noise_degrees = 15.0f;
    set_cover(smoke, &cover);
    Scene scene = facets_scene(smoke, true, (uint8_t)BTBH_GPU_SCENE_BATCH_FLAGS2_COVER_0);
    uint8_t *whole = frame_of(smoke, &scene, NULL);
    scene.split = true;
    uint8_t *split = frame_of(smoke, &scene, NULL);
    scene.split = false;
    /* Away from the line (two pixels each side), each half is the whole frame's. */
    uint32_t differing = 0u;
    uint32_t compared = 0u;
    for (uint32_t py = 0u; py < HEIGHT; ++py) {
        for (uint32_t px = 0u; px < WIDTH; ++px) {
            if (px + 2u >= WIDTH / 2u && px <= WIDTH / 2u + 2u) {
                continue;
            }
            const size_t at = ((size_t)py * WIDTH + px) * 4u;
            compared += 1u;
            differing += memcmp(&whole[at], &split[at], 3u) != 0 ? 1u : 0u;
        }
    }
    printf("views: %u of %u pixels away from the line differ from the one-view frame\n", differing, compared);
    assert(differing == 0u);
    /* A render into a target takes the cover too. */
    scene.into_target = true;
    uint8_t *target_covered = frame_of(smoke, &scene, NULL);
    set_cover(smoke, NULL);
    uint8_t *target_bare = frame_of(smoke, &scene, NULL);
    assert(memcmp(target_covered, target_bare, (size_t)WIDTH * HEIGHT * 4u) != 0);
    uint32_t equal = 0u;
    for (size_t at = 0u; at < (size_t)WIDTH * HEIGHT * 4u; at += 4u) {
        equal += memcmp(&target_covered[at], &whole[at], 3u) == 0 ? 1u : 0u;
    }
    printf("render into a target: covered, and %u of %u pixels as the window's covered frame\n", equal, WIDTH * HEIGHT);
    assert(equal * 100u >= WIDTH * HEIGHT * 99u);
    free(whole);
    free(split);
    free(target_covered);
    free(target_bare);
}

/* ---- Prewarm ---- */

static uint32_t made_by_prewarm(BTBHGPUWindow *window, bool print) {
    BTBHGPULazyCreationEntry entries[64];
    uint32_t covered = 0u;
    for (;;) {
        uint32_t count = 0u;
        assert(btbh_gpu_window_lazy_creation_report(window, entries, 64u, &count) == BTBH_GPU_OK);
        for (uint32_t index = 0u; index < count; ++index) {
            if (entries[index].prewarm_covers != 0u) {
                covered += 1u;
                if (print) {
                    printf("  made: %s\n", entries[index].name);
                }
            }
        }
        if (count < 64u) {
            break;
        }
    }
    return covered;
}

/* The facets lit (alternate ones with COVER_0 and COVER_1 when `bits`) and an ice sheet (still ice when `ice`). */
static Scene prewarm_scene(const SmokeWindow *smoke, bool bits, bool ice) {
    Scene scene = facets_scene(smoke, false, 0u);
    post_reflections(&scene.post);
    for (uint32_t index = 0u; bits && index < FACETS; ++index) {
        scene.batches[index].flags2 = index % 2u == 0u ? (uint8_t)BTBH_GPU_SCENE_BATCH_FLAGS2_COVER_0 :
            (uint8_t)BTBH_GPU_SCENE_BATCH_FLAGS2_COVER_1;
    }
    instance(scene.instances[FACETS], ice_albedo);
    scene.batches[FACETS] = batch_of(smoke->ice, FACETS, false);
    if (ice) {
        scene.batches[FACETS].surface_variant = BTBH_GPU_SCENE_SURFACE_ICE;
    }
    scene.batch_count = FACETS + 1u;
    return scene;
}

static void prewarm(SmokeWindow *smoke, const Scene *scene) {
    const ShaderSet *set = &shaders;
    BTBHGPUSceneReport report;
    memset(&report, 0, sizeof(report));
    const BTBHGPUResult prewarmed = btbh_gpu_window_scene_prewarm(smoke->window, set->paths[0], set->paths[1],
        set->paths[2], set->paths[3], set->paths[4], set->paths[5], set->paths[6], set->paths[7], set->paths[8],
        set->paths[9], set->paths[10], SMOKE_SHADER_FORMAT, WIDTH, HEIGHT, &scene->lighting, &scene->post,
        scene->batches, scene->batch_count, &report);
    if (prewarmed != BTBH_GPU_OK) {
        fprintf(stderr, "prewarm: %s\n", report.message);
    }
    assert(prewarmed == BTBH_GPU_OK);
}

/* Two layers on (SNOW, MUD), or none. */
static void two_layers(SmokeWindow *smoke, bool on) {
    BTBHGPUSceneCover cover;
    memset(&cover, 0, sizeof(cover));
    cover.layers[0] = snow_layer();
    cover.layers[1] = snow_layer();
    cover.layers[1].pattern = BTBH_GPU_SCENE_COVER_MUD;
    set_cover(smoke, on ? &cover : NULL);
}

/* `covered` prewarms with the bits and still ice, the layers off; `bare` without either. */
static void check_prewarm(SmokeWindow *covered, SmokeWindow *bare) {
    two_layers(covered, false);
    btbh_gpu_window_set_lazy_creation_log_enabled(covered->window, 1u);
    Scene scene = prewarm_scene(covered, true, true);
    prewarm(covered, &scene);
    (void)made_by_prewarm(covered->window, false);
    two_layers(covered, true);
    uint8_t *frame = frame_of(covered, &scene, NULL);
    free(frame);
    const uint32_t made_on = made_by_prewarm(covered->window, true);
    two_layers(covered, false);
    frame = frame_of(covered, &scene, NULL);
    free(frame);
    const uint32_t made_off = made_by_prewarm(covered->window, true);
    printf("prewarm with the bits and ICE (layers off): the first frame with the layers on made %u things a prewarm "
           "makes, with them off %u\n", made_on, made_off);
    assert(made_on == 0u && made_off == 0u);
    btbh_gpu_window_set_lazy_creation_log_enabled(covered->window, 0u);

    two_layers(bare, false);
    btbh_gpu_window_set_lazy_creation_log_enabled(bare->window, 1u);
    scene = prewarm_scene(bare, false, false);
    prewarm(bare, &scene);
    (void)made_by_prewarm(bare->window, false);
    two_layers(bare, true);
    scene = prewarm_scene(bare, true, true);
    frame = frame_of(bare, &scene, NULL);
    free(frame);
    const uint32_t made_bare = made_by_prewarm(bare->window, true);
    printf("prewarm without the bits or ICE: the first covered frame made %u things a prewarm makes (the cover "
           "variants' pipelines)\n", made_bare);
    assert(made_bare > 0u);
    btbh_gpu_window_set_lazy_creation_log_enabled(bare->window, 0u);
    two_layers(bare, false);
}

/* ---- Refusals ---- */

static void expect_set_refused(SmokeWindow *smoke, const BTBHGPUSceneCover *cover, const char *needle) {
    BTBHGPUSceneReport report;
    memset(&report, 0, sizeof(report));
    const BTBHGPUResult result = btbh_gpu_window_scene_set_cover(smoke->window, cover, &report);
    if (result != BTBH_GPU_INVALID_ARGUMENT || strstr(report.message, needle) == NULL) {
        fprintf(stderr, "set_cover: expected a refusal naming \"%s\", got %d \"%s\"\n", needle, (int)result,
            report.message);
    }
    assert(result == BTBH_GPU_INVALID_ARGUMENT && report.result == BTBH_GPU_INVALID_ARGUMENT);
    assert(strstr(report.message, needle) != NULL);
}

static void expect_present_refused(SmokeWindow *smoke, const Scene *scene, const char *needle) {
    BTBHGPUSceneReport report;
    memset(&report, 0, sizeof(report));
    const BTBHGPUResult result = present(smoke, scene, &report);
    if (result != BTBH_GPU_INVALID_ARGUMENT || strstr(report.message, needle) == NULL) {
        fprintf(stderr, "present: expected a refusal naming \"%s\", got %d \"%s\"\n", needle, (int)result,
            report.message);
    }
    assert(result == BTBH_GPU_INVALID_ARGUMENT);
    assert(strstr(report.message, needle) != NULL);
    assert(!smoke->window->pending_scene_ready && smoke->window->pending_command == NULL);
}

static void check_refusals(SmokeWindow *smoke) {
    BTBHGPUSceneReport report;
    memset(&report, 0, sizeof(report));
    BTBHGPUSceneCover cover;
    memset(&cover, 0, sizeof(cover));
    assert(btbh_gpu_window_scene_set_cover(NULL, &cover, &report) == BTBH_GPU_INVALID_ARGUMENT);
    assert(strstr(report.message, "window is NULL") != NULL);
    assert(btbh_gpu_window_scene_set_cover(smoke->window, NULL, NULL) == BTBH_GPU_OK);
    /* A refused call keeps the setting: the frame stays the one the valid cover draws. */
    cover.layers[0] = snow_layer();
    set_cover(smoke, &cover);
    Scene scene = facets_scene(smoke, true, (uint8_t)BTBH_GPU_SCENE_BATCH_FLAGS2_COVER_0);
    uint8_t *before = frame_of(smoke, &scene, NULL);
    BTBHGPUSceneCover bad = cover;
    bad.layers[1].amount = 2.0f;
    expect_set_refused(smoke, &bad, "Cover layer 1: amount");
    bad = cover;
    bad.layers[0].color[2] = NAN;
    expect_set_refused(smoke, &bad, "Cover layer 0: color[2] is not finite");
    bad = cover;
    bad.layers[0].wind[1] = 0.5f;
    expect_set_refused(smoke, &bad, "phase 2");
    bad = cover;
    bad.layers[1].pattern = 7u;
    expect_set_refused(smoke, &bad, "pattern 7 is unknown");
    bad = cover;
    bad.reserved = 3u;
    expect_set_refused(smoke, &bad, "Cover: reserved");
    uint8_t *after = frame_of(smoke, &scene, NULL);
    assert(memcmp(before, after, (size_t)WIDTH * HEIGHT * 4u) == 0);
    free(before);
    free(after);
    /* The batches. */
    Scene refused = facets_scene(smoke, false, (uint8_t)(BTBH_GPU_SCENE_BATCH_FLAGS2_COVER_0 |
        BTBH_GPU_SCENE_BATCH_FLAGS2_COVER_1));
    expect_present_refused(smoke, &refused, "flags2 COVER: COVER_0 and COVER_1");
    refused = facets_scene(smoke, false, (uint8_t)BTBH_GPU_SCENE_BATCH_FLAGS2_COVER_1);
    refused.batches[2].surface_variant = BTBH_GPU_SCENE_SURFACE_LIT_PARTICLE;
    refused.batches[2].transparency_enabled = 1u;
    expect_present_refused(smoke, &refused, "GPU scene batch 2: flags2 COVER: a lit particle");
    refused = facets_scene(smoke, false, (uint8_t)BTBH_GPU_SCENE_BATCH_FLAGS2_COVER_0);
    refused.batches[3].surface_variant = BTBH_GPU_SCENE_SURFACE_REFRACT;
    refused.batches[3].transparency_enabled = 1u;
    expect_present_refused(smoke, &refused, "GPU scene batch 3: flags2 COVER: a refracting batch");
    refused = facets_scene(smoke, true, (uint8_t)BTBH_GPU_SCENE_BATCH_FLAGS2_COVER_0);
    refused.batches[5].transparency_enabled = 1u;
    expect_present_refused(smoke, &refused, "GPU scene batch 5: flags2 COVER: the fast unlit shader");
    refused = ice_scene(smoke, false);
    refused.batches[0].surface_parameters1[0] = 1.5f;
    expect_present_refused(smoke, &refused, "surface_variant ICE");
    refused.batches[0].surface_parameters1[0] = 0.2f;
    refused.batches[0].surface_parameters0[1] = -0.1f;
    expect_present_refused(smoke, &refused, "surface_variant ICE");
    /* A bit the release doesn't know (bit 1 is RFC 0019's) is still refused. */
    refused = facets_scene(smoke, false, 2u);
    BTBHGPUSceneReport unknown;
    memset(&unknown, 0, sizeof(unknown));
    assert(present(smoke, &refused, &unknown) == BTBH_GPU_INVALID_ARGUMENT);
    set_cover(smoke, NULL);
    printf("refusals: the setter's (a refused call keeps the setting), both bits, a lit particle, a refracting batch, "
           "the fast unlit shader's batch, ICE's parameters, flags2 bit 1\n");
}

typedef struct ShaderCopy {
    char from[1100];
    char to[1100];
    uint32_t copied;
} ShaderCopy;

static SDL_EnumerationResult SDLCALL copy_shader_file(void *userdata, const char *dirname, const char *fname) {
    ShaderCopy *copy = (ShaderCopy *)userdata;
    char from[1200];
    char to[1200];
    (void)dirname;
    (void)snprintf(from, sizeof(from), "%s/%s", copy->from, fname);
    (void)snprintf(to, sizeof(to), "%s/%s", copy->to, fname);
    SDL_PathInfo info;
    if (SDL_GetPathInfo(from, &info) && info.type == SDL_PATHTYPE_FILE) {
        assert(SDL_CopyFile(from, to));
        copy->copied += 1u;
    }
    return SDL_ENUM_CONTINUE;
}

/* Renames the cover's block in a SPIR-V file's names (one byte), as a scene shader from before 0.18.0 has none. */
static void unname_cover(const char *path) {
    size_t size = 0u;
    uint8_t *bytes = (uint8_t *)SDL_LoadFile(path, &size);
    assert(bytes != NULL);
    static const char name[] = "cover_color_roughness";
    uint32_t renamed = 0u;
    for (size_t at = 0u; at + sizeof(name) - 1u <= size; ++at) {
        if (memcmp(bytes + at, name, sizeof(name) - 1u) == 0) {
            bytes[at + sizeof(name) - 2u] = 'X';
            renamed += 1u;
        }
    }
    assert(renamed > 0u);
    assert(SDL_SaveFile(path, bytes, size));
    SDL_free(bytes);
}

static const char *const cover_shaders[6] = {
    "production_model_cover.frag", "production_model_cover_2d_shadows.frag",
    "production_model_cover_unshadowed.frag", "production_model_cover_rc.frag",
    "production_model_cover_rc_2d_shadows.frag", "production_model_cover_rc_unshadowed.frag",
};

/* The built shaders copied into a folder beside them (`name`), with the cover variants unnamed (rename) or removed. */
static void copy_shaders(const char *name, bool rename, ShaderSet *set) {
    ShaderCopy copy;
    memset(&copy, 0, sizeof(copy));
    (void)snprintf(copy.from, sizeof(copy.from), "%s", BTBH_GPU_SMOKE_SHADER_DIR);
    (void)snprintf(copy.to, sizeof(copy.to), "%s/../%s", BTBH_GPU_SMOKE_SHADER_DIR, name);
    (void)SDL_CreateDirectory(copy.to);
    assert(SDL_EnumerateDirectory(copy.from, copy_shader_file, &copy));
    assert(copy.copied > 20u);
    for (int index = 0; index < 6; ++index) {
        char path[1300];
        (void)snprintf(path, sizeof(path), "%s/%s%s", copy.to, cover_shaders[index], SMOKE_SHADER_EXTENSION);
        if (rename) {
            unname_cover(path);
        } else {
            assert(SDL_RemovePath(path));
        }
    }
    shader_set_in(set, copy.to);
}

static void check_old_shaders(SmokeWindow *smoke) {
    ShaderSet old;
    copy_shaders("cover_old_shaders", true, &old);
    BTBHGPUSceneCover cover;
    memset(&cover, 0, sizeof(cover));
    cover.layers[0] = snow_layer();
    set_cover(smoke, &cover);
    /* Without a covered batch or ICE the old shaders draw. */
    Scene scene = facets_scene(smoke, false, 0u);
    scene.shaders = &old;
    uint8_t *frame = frame_of(smoke, &scene, NULL);
    free(frame);
    scene = facets_scene(smoke, false, (uint8_t)BTBH_GPU_SCENE_BATCH_FLAGS2_COVER_0);
    scene.shaders = &old;
    expect_present_refused(smoke, &scene, "cover_old_shaders/production_model_cover");
    expect_present_refused(smoke, &scene, "doesn't name the cover's block (build the shaders from 0.18.0");
    scene = ice_scene(smoke, false);
    scene.shaders = &old;
    expect_present_refused(smoke, &scene, "still-ice batch's cover shader");
    /* A layer at amount 0 lays nothing, so it needs nothing of them. */
    cover.layers[0].amount = 0.0f;
    set_cover(smoke, &cover);
    scene = facets_scene(smoke, false, (uint8_t)BTBH_GPU_SCENE_BATCH_FLAGS2_COVER_0);
    scene.shaders = &old;
    frame = frame_of(smoke, &scene, NULL);
    free(frame);
    /* Shaders from before 0.18.0 have no cover variants: refused the same way, and the frames without them draw. */
    copy_shaders("cover_none_shaders", false, &old);
    cover.layers[0].amount = snow_layer().amount;
    set_cover(smoke, &cover);
    scene = facets_scene(smoke, false, (uint8_t)BTBH_GPU_SCENE_BATCH_FLAGS2_COVER_0);
    scene.shaders = &old;
    expect_present_refused(smoke, &scene, "cover shader is missing (build the shaders from 0.18.0");
    expect_present_refused(smoke, &scene, "cover_none_shaders/production_model_cover");
    scene = ice_scene(smoke, false);
    scene.shaders = &old;
    expect_present_refused(smoke, &scene, "still-ice batch's cover shader");
    scene = facets_scene(smoke, false, 0u);
    scene.shaders = &old;
    frame = frame_of(smoke, &scene, NULL);
    free(frame);
    set_cover(smoke, NULL);
    printf("old cover shaders (no cover block) and none at all: a covered frame and an ICE frame refused, naming the "
           "shader; frames without them draw\n");
}

/* ---- Main ---- */

static SmokeWindow open_window(BTBHGPUDevice *device, const char *title) {
    SmokeWindow smoke;
    memset(&smoke, 0, sizeof(smoke));
    const BTBHGPUWindowDescriptor descriptor = {
        .title = title,
        .width = WIDTH,
        .height = HEIGHT,
        .hidden = 1,
        .high_pixel_density = 0,
    };
    BTBHGPUWindowReport report;
    memset(&report, 0, sizeof(report));
    smoke.window = btbh_gpu_window_create(&descriptor, &report);
    assert(smoke.window != NULL);
    assert(btbh_gpu_window_claim(device, smoke.window, &report) == BTBH_GPU_OK);
    for (uint32_t index = 0u; index < FACETS; ++index) {
        smoke.facet_meshes[index] = rect_mesh(smoke.window, &facets[index]);
    }
    smoke.ice = rect_mesh(smoke.window, &ice_rect);
    smoke.floor = rect_mesh(smoke.window, &floor_rect);
    smoke.wall = rect_mesh(smoke.window, &wall_rect);
    BTBHGPUTextureReport texture_report;
    smoke.frame = btbh_gpu_window_render_target_create(smoke.window, WIDTH, HEIGHT, BTBH_GPU_TEXTURE_FILTER_NEAREST,
        &texture_report);
    assert(smoke.frame != NULL);
    smoke.target = btbh_gpu_window_render_target_create(smoke.window, WIDTH, HEIGHT, BTBH_GPU_TEXTURE_FILTER_NEAREST,
        &texture_report);
    assert(smoke.target != NULL);
    /* A normal map leaning every normal by asin(0.2) toward the tangent (+x). */
    uint8_t tilt[4 * 4 * 4];
    for (int index = 0; index < 16; ++index) {
        tilt[index * 4] = 153u;
        tilt[index * 4 + 1] = 128u;
        tilt[index * 4 + 2] = 255u;
        tilt[index * 4 + 3] = 255u;
    }
    smoke.tilt_map = btbh_gpu_window_texture_create_rgba8_ex(smoke.window, tilt, 4u, 4u, 0u,
        BTBH_GPU_TEXTURE_FILTER_NEAREST, BTBH_GPU_TEXTURE_WRAP_REPEAT, BTBH_GPU_TEXTURE_COLOR_SPACE_LINEAR,
        &texture_report);
    assert(smoke.tilt_map != NULL);
    return smoke;
}

static void close_window(SmokeWindow *smoke) {
    btbh_gpu_texture_destroy(smoke->frame);
    btbh_gpu_texture_destroy(smoke->target);
    btbh_gpu_texture_destroy(smoke->tilt_map);
    for (uint32_t index = 0u; index < FACETS; ++index) {
        btbh_gpu_custom_mesh_destroy(smoke->facet_meshes[index]);
    }
    btbh_gpu_custom_mesh_destroy(smoke->ice);
    btbh_gpu_custom_mesh_destroy(smoke->floor);
    btbh_gpu_custom_mesh_destroy(smoke->wall);
    btbh_gpu_window_destroy(smoke->window);
}

int main(void) {
    (void)setvbuf(stdout, NULL, _IONBF, 0);
    const bool verify_requested = smoke_runs_verify_requested();
    shader_set_in(&shaders, BTBH_GPU_SMOKE_SHADER_DIR);
    for (int index = 0; index < 11; ++index) {
        FILE *file = fopen(shaders.paths[index], "rb");
        if (file == NULL) {
            fprintf(stderr, "skipped: no shader %s\n", shaders.paths[index]);
            return SMOKE_SKIP;
        }
        fclose(file);
    }
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
    make_rects();
    SmokeWindow a = open_window(device, "BTBH cover smoke A");
    SmokeWindow b = open_window(device, "BTBH cover smoke B");

    check_identity(&a);
    check_patterns(&a);
    check_targets(&a);
    check_cascades(&b);
    check_reflections(&a);
    check_see_through(&a);
    check_ice(&a);
    check_runs(&a, verify_requested);
    check_views_and_target(&a);
    check_refusals(&a);
    check_old_shaders(&a);
    SmokeWindow c = open_window(device, "BTBH cover smoke C");
    SmokeWindow d = open_window(device, "BTBH cover smoke D");
    check_prewarm(&c, &d);
    smoke_check_runs_verify(a.window, "btbh_gpu_scene_cover_smoke", verify_requested);

    close_window(&a);
    close_window(&b);
    close_window(&c);
    close_window(&d);
    btbh_gpu_device_shutdown(device);
    btbh_gpu_device_destroy(device);
    printf("cover smoke passed\n");
    return 0;
}
