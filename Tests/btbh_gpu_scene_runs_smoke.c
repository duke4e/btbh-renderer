/* RFC 0011 phase 1 (0.10.0): custom meshes in the window's shared geometry buffers
 * (BTBH_GPU_CUSTOM_MESH_SHARED_BUFFERS) and same-material runs, rendered into a target and read back (hidden windows,
 * a debug device).
 *
 * Drawn as before. This part also builds against 0.9.0's header, which has neither: there it only renders (and
 * writes) the gallery, so that a build of 0.9.0 and a build of this one can be compared.
 * - A gallery of one street of kit pieces through the passes that encode batches: ten distinct small meshes (one
 *   without indices) in two chunks, each with a persistent instance buffer; the first chunk's walls (a world-detail
 *   table) and glass (none) interleaved piece by piece, the second's sorted (its walls, then its glass); a model with
 *   two primitives in two batches with a skinned model between them; runs of emissive, unlit, coplanar LESS_EQUAL
 *   decals, a stencil writer and its skippers, a KEEP_ORDER batch and transparent batches. Through no shadow, the
 *   sun's soft shadow, the sun's and the omni shadow, the radiance cascades (their material sources) and the
 *   opaque pass's front-to-back sort.
 * - Each scene is rendered with the meshes in buffers of their own and, from 0.10.0, in the shared buffers, each
 *   with runs and without (BTBH_GPU_SCENE_RUNS=0): all four give the same bytes, and the same bytes twice.
 * - From 0.10.0, presented to the window once with runs and once without, the scene telemetry counts the draws that
 *   continued a run in the geometry passes, the shadow passes and the cascades' sources (none without runs).
 * - BTBH_SCENE_RUNS_DUMP=<dir> writes the gallery (runs-<name>.png); BTBH_SCENE_RUNS_REFERENCE=<dir> compares every
 *   render (own and shared buffers, runs on and off) with that directory's and requires 0 differing pixels.
 *
 * Shared meshes (0.10.0):
 * - Refusals: unknown flags, invalid data, no report; an update past the mesh's vertices or indices, and indices for
 *   a mesh made without them (the mesh stays as it was and still draws so).
 * - An update within the ranges draws as a mesh with buffers of its own made with the new data, a smaller one and the
 *   full one again.
 * - Two meshes and then a model take ranges of one block; destroying the meshes keeps it while the model holds it,
 *   and the block's buffers go with the model.
 * - A window closed with a shared mesh alive: the mesh is destroyed after, and an update of it is refused.
 *
 * It needs a GPU and the production shaders; with no device it is skipped (77). */
#undef NDEBUG

#include "btbh_gpu/btbh_gpu.h"
#include "btbh_gpu_internal.h"
#include "btbh_gpu_package_internal.h"

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_surface.h>

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SMOKE_SKIP 77
#define TARGET_SIZE 192u
#define TEXELS (TARGET_SIZE * TARGET_SIZE)
#define PIECES 10u
#define CHUNK_INSTANCES 12u
#define MAX_BATCHES 64u
#define MAX_TRANSIENT 64u
#define FLOATS 24u

#if defined(BTBH_GPU_HAS_SHARED_MESH_BUFFERS)
#define SMOKE_HAS_SHARED 1
#endif

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
static const char *const canvas_shader_names[3] = {
    "production_canvas.vert",
    "production_canvas.frag",
    "production_canvas_textured.frag",
};
static char canvas_shader_paths[3][1024];

typedef struct SmokeWindow {
    BTBHGPUWindow *window;
    BTBHGPUTexture *target;
    BTBHGPUTexture *albedo;
    BTBHGPUTexture *normal;
    BTBHGPUTexture *orm;
    BTBHGPUTexture *glow;
    BTBHGPUTexture *maps;                       /* the table's array: 4 layers */
    BTBHGPUSceneWorldDetailTableEntry entries[2];
    BTBHGPUSceneWorldDetailTable table;
    BTBHGPUCustomMesh *own[PIECES];             /* buffers of their own */
    BTBHGPUCustomMesh *shared[PIECES];          /* the shared buffers (0.10.0) */
    BTBHGPUCustomMesh *cube;                    /* the ground, the decals' plate, the stencil boxes */
    BTBHGPUCustomMesh *plate;                   /* an upward quad at y = 0 */
    BTBHGPUModel *pair;                         /* two primitives on two nodes */
    BTBHGPUModel *skinned;
    BTBHGPUAnimationLibrary *animation;
    BTBHGPUInstanceBuffer *chunks[2];
} SmokeWindow;

/* ------------------------------------------------------------------------------------------------- helpers */

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
} SmokeCamera;

/* Column-major, right-handed, depth 0..1; `projection` NULL: a perspective one of fov_y. */
static SmokeCamera look_at(const float eye[3], const float centre[3], float fov_y, const float *projection) {
    SmokeCamera camera;
    memset(&camera, 0, sizeof(camera));
    float forward[3] = { centre[0] - eye[0], centre[1] - eye[1], centre[2] - eye[2] };
    normalize3(forward);
    const float world_up[3] = { 0.0f, 1.0f, 0.0f };
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
    if (projection != NULL) {
        multiply(projection, view, camera.view_projection);
    } else {
        const float near_plane = 0.1f;
        const float far_plane = 60.0f;
        const float h = 1.0f / tanf(0.5f * fov_y);
        const float r = far_plane / (near_plane - far_plane);
        const float perspective[16] = {
            h, 0.0f, 0.0f, 0.0f,
            0.0f, h, 0.0f, 0.0f,
            0.0f, 0.0f, r, -1.0f,
            0.0f, 0.0f, r * near_plane, 0.0f,
        };
        multiply(perspective, view, camera.view_projection);
    }
    memcpy(camera.right, side, sizeof(side));
    memcpy(camera.up, up, sizeof(up));
    memcpy(camera.position, eye, sizeof(camera.position));
    return camera;
}

static uint32_t smoke_random(uint32_t *state) {
    *state = *state * 1664525u + 1013904223u;
    return *state >> 8u;
}

/* An axis-aligned box (centre, half sizes): 24 vertices and 36 indices, its outsides front faces; joints.x (the
   table's entry) is `entry`, the vertex colour a shade of `shade`. */
static void add_box(float *vertices, uint32_t *vertex_count, uint32_t *indices, uint32_t *index_count,
    const float centre[3], const float half[3], float entry, float shade) {
    static const float faces[6][3] = {
        { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 },
    };
    for (uint32_t face = 0u; face < 6u; ++face) {
        const float *n = faces[face];
        const float u[3] = { n[1], n[2], n[0] };
        float v[3];
        cross3(n, u, v);
        const uint32_t base = *vertex_count;
        for (uint32_t corner = 0u; corner < 4u; ++corner) {
            const float su = (corner == 1u || corner == 2u) ? 1.0f : -1.0f;
            const float sv = corner >= 2u ? 1.0f : -1.0f;
            float *vertex = &vertices[(size_t)(*vertex_count) * FLOATS];
            memset(vertex, 0, FLOATS * sizeof(float));
            for (int axis = 0; axis < 3; ++axis) {
                vertex[axis] = centre[axis] + half[axis] * (n[axis] + su * u[axis] + sv * v[axis]);
                vertex[3 + axis] = n[axis];
                vertex[8 + axis] = u[axis];
            }
            vertex[6] = 0.5f * su + 0.5f;
            vertex[7] = 0.5f * sv + 0.5f;
            vertex[11] = 1.0f;
            vertex[12] = shade;
            vertex[13] = shade;
            vertex[14] = 0.9f * shade + 0.1f;
            vertex[15] = 1.0f;
            vertex[16] = entry;
            vertex[20] = 1.0f;
            *vertex_count += 1u;
        }
        const uint32_t wound[6] = { base, base + 2u, base + 1u, base, base + 3u, base + 2u };
        memcpy(&indices[*index_count], wound, sizeof(wound));
        *index_count += 6u;
    }
}

/* The data of kit piece p: a slab and p % 4 ledges and posts, every second box on entry 1 and the last box of an odd
   piece on 255 (no detail). Piece PIECES - 1 has no indices (its triangles' vertices one after another). */
typedef struct PieceData {
    float vertices[8u * 24u * FLOATS];
    uint32_t indices[8u * 36u];
    uint32_t vertex_count;
    uint32_t index_count;
} PieceData;

static void piece_data(uint32_t piece, PieceData *data) {
    memset(data, 0, sizeof(*data));
    const uint32_t boxes = 1u + piece % 4u + piece / 4u;
    uint32_t state = 77u + piece;
    for (uint32_t box = 0u; box < boxes; ++box) {
        const float entry = (piece % 2u == 1u && box + 1u == boxes) ? 255.0f : (float)(box % 2u);
        float centre[3];
        float half[3];
        if (box == 0u) {
            centre[0] = 0.0f;
            centre[1] = 0.5f + 0.1f * (float)(piece % 3u);
            centre[2] = 0.0f;
            half[0] = 0.35f + 0.05f * (float)(piece % 2u);
            half[1] = centre[1];
            half[2] = 0.3f;
        } else {
            centre[0] = -0.3f + 0.6f * (float)(smoke_random(&state) % 100u) / 100.0f;
            centre[1] = 0.2f + 0.8f * (float)(smoke_random(&state) % 100u) / 100.0f;
            centre[2] = 0.32f;
            half[0] = 0.05f + 0.1f * (float)(smoke_random(&state) % 100u) / 100.0f;
            half[1] = 0.04f + 0.08f * (float)(smoke_random(&state) % 100u) / 100.0f;
            half[2] = 0.06f;
        }
        add_box(data->vertices, &data->vertex_count, data->indices, &data->index_count, centre, half, entry,
            0.6f + 0.4f * (float)((piece + box) % 3u) / 2.0f);
    }
    if (piece == PIECES - 1u) {
        /* Without indices: each index's vertex, in order. */
        static float expanded[8u * 36u * FLOATS];
        for (uint32_t index = 0u; index < data->index_count; ++index) {
            memcpy(&expanded[(size_t)index * FLOATS], &data->vertices[(size_t)data->indices[index] * FLOATS],
                FLOATS * sizeof(float));
        }
        assert(data->index_count <= 8u * 24u);
        memcpy(data->vertices, expanded, (size_t)data->index_count * FLOATS * sizeof(float));
        data->vertex_count = data->index_count;
        data->index_count = 0u;
    }
}

static BTBHGPUCustomMesh *create_mesh(BTBHGPUWindow *window, const float *vertices, uint32_t vertex_count,
    const uint32_t *indices, uint32_t index_count, bool shared) {
    BTBHGPUCustomMeshReport report;
    memset(&report, 0, sizeof(report));
    BTBHGPUCustomMesh *mesh = NULL;
#if defined(SMOKE_HAS_SHARED)
    mesh = btbh_gpu_window_custom_mesh_create_ex(window, vertices, vertex_count * FLOATS * (uint32_t)sizeof(float),
        vertex_count, indices, index_count, shared ? BTBH_GPU_CUSTOM_MESH_SHARED_BUFFERS : 0u, &report);
#else
    assert(!shared);
    mesh = btbh_gpu_window_custom_mesh_create(window, vertices, vertex_count * FLOATS * (uint32_t)sizeof(float),
        vertex_count, indices, index_count, &report);
#endif
    if (mesh == NULL) {
        fprintf(stderr, "mesh: %s\n", report.message);
    }
    assert(mesh != NULL);
    assert(report.result == BTBH_GPU_OK);
    assert(report.vertex_count == vertex_count && report.index_count == index_count);
    return mesh;
}

static BTBHGPUCustomMesh *create_piece(BTBHGPUWindow *window, uint32_t piece, bool shared) {
    static PieceData data;
    piece_data(piece, &data);
    return create_mesh(window, data.vertices, data.vertex_count, data.indices, data.index_count, shared);
}

static uint8_t *noise_pixels(uint32_t size, uint32_t seed, uint8_t low, uint8_t span) {
    uint8_t *pixels = malloc((size_t)size * size * 4u);
    assert(pixels != NULL);
    uint32_t state = seed;
    for (size_t i = 0u; i < (size_t)size * size * 4u; ++i) {
        pixels[i] = (i % 4u) == 3u ? 255u : (uint8_t)(low + smoke_random(&state) % span);
    }
    return pixels;
}

static BTBHGPUTexture *noise_texture(BTBHGPUWindow *window, uint32_t seed, uint8_t low, uint8_t span,
    BTBHGPUTextureColorSpace color_space) {
    uint8_t *pixels = noise_pixels(64u, seed, low, span);
    BTBHGPUTextureReport report;
    memset(&report, 0, sizeof(report));
    BTBHGPUTexture *texture = btbh_gpu_window_texture_create_rgba8_ex(window, pixels, 64u, 64u, 1u,
        BTBH_GPU_TEXTURE_FILTER_TRILINEAR, BTBH_GPU_TEXTURE_WRAP_REPEAT, color_space, &report);
    free(pixels);
    if (texture == NULL) {
        fprintf(stderr, "texture: %s\n", report.message);
    }
    assert(texture != NULL);
    return texture;
}

/* The texture's bytes, RGBA (4 a texel), read back through SDL_GPU. */
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
    if (texture->format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM ||
        texture->format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB) {
        for (uint32_t index = 0u; index < size; index += 4u) {
            const uint8_t blue = pixels[index];
            pixels[index] = pixels[index + 2u];
            pixels[index + 2u] = blue;
        }
    }
    return pixels;
}

static uint32_t differing_pixels(const uint8_t *a, const uint8_t *b) {
    uint32_t count = 0u;
    for (uint32_t index = 0u; index < TEXELS; ++index) {
        count += memcmp(&a[index * 4u], &b[index * 4u], 4u) != 0 ? 1u : 0u;
    }
    return count;
}

static uint32_t lit_pixels(const uint8_t *pixels, const uint8_t clear[3]) {
    uint32_t count = 0u;
    for (uint32_t index = 0u; index < TEXELS; ++index) {
        count += memcmp(&pixels[index * 4u], clear, 3u) != 0 ? 1u : 0u;
    }
    return count;
}

static bool load_png(const char *path, uint8_t *pixels) {
    SDL_Surface *surface = SDL_LoadPNG(path);
    if (surface == NULL) {
        return false;
    }
    SDL_Surface *rgba = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_ABGR8888);
    SDL_DestroySurface(surface);
    if (rgba == NULL || (uint32_t)rgba->w != TARGET_SIZE || (uint32_t)rgba->h != TARGET_SIZE) {
        SDL_DestroySurface(rgba);
        return false;
    }
    for (uint32_t y = 0u; y < TARGET_SIZE; ++y) {
        memcpy(&pixels[(size_t)y * TARGET_SIZE * 4u], (const uint8_t *)rgba->pixels + (size_t)y * (size_t)rgba->pitch,
            (size_t)TARGET_SIZE * 4u);
    }
    SDL_DestroySurface(rgba);
    return true;
}

/* The core reads BTBH_GPU_SCENE_RUNS once a present. */
static void set_runs(bool enabled) {
    if (enabled) {
        (void)SDL_UnsetEnvironmentVariable(SDL_GetEnvironment(), "BTBH_GPU_SCENE_RUNS");
    } else {
        (void)SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "BTBH_GPU_SCENE_RUNS", "0", true);
    }
}

/* --------------------------------------------------------------------------------------------- the scenes */

typedef enum Shadows { SHADOWS_NONE = 0, SHADOWS_SUN = 1, SHADOWS_OMNI = 2 } Shadows;

typedef struct Options {
    const char *name;
    Shadows shadows;
    bool cascades;
    bool sorted;          /* BTBH_GPU_SCENE_OPAQUE_SORT_FRONT_TO_BACK */
} Options;

static const Options gallery[] = {
    { .name = "unshadowed", .shadows = SHADOWS_NONE },
    { .name = "sun", .shadows = SHADOWS_SUN },
    { .name = "omni", .shadows = SHADOWS_OMNI },
    { .name = "cascades", .shadows = SHADOWS_SUN, .cascades = true },
    { .name = "sorted", .shadows = SHADOWS_SUN, .sorted = true },
};
#define GALLERY_COUNT (sizeof(gallery) / sizeof(gallery[0]))

typedef struct Scene {
    const char *name;
    SmokeCamera camera;
    BTBHGPUSceneLighting lighting;
    BTBHGPUScenePost post;
    float instances[MAX_TRANSIENT][FLOATS];
    uint32_t instance_count;
    BTBHGPUSceneGeometryBatch batches[MAX_BATCHES];
    uint32_t batch_count;
} Scene;

static void instance_at(float out[FLOATS], float x, float y, float z, float scale, float yaw, const float colour[4],
    float seed) {
    memset(out, 0, FLOATS * sizeof(float));
    out[0] = scale * cosf(yaw);
    out[2] = -scale * sinf(yaw);
    out[5] = scale;
    out[8] = scale * sinf(yaw);
    out[10] = scale * cosf(yaw);
    out[12] = x;
    out[13] = y;
    out[14] = z;
    out[15] = 1.0f;
    memcpy(&out[16], colour, 4u * sizeof(float));
    out[23] = seed;
}

/* The two chunks' instances: a row of twelve lots each. */
static void chunk_instance(uint32_t chunk, uint32_t index, float out[FLOATS]) {
    static const float tint[4] = { 0.95f, 0.9f, 0.85f, 1.0f };
    const float x = -2.75f + 0.5f * (float)index;
    const float z = chunk == 0u ? -1.4f : -0.2f;
    instance_at(out, x, 0.0f, z, 0.45f, 0.3f * (float)(index % 3u), tint, (float)(index * 7u + chunk));
}

static BTBHGPUSceneGeometryBatch base_batch(void) {
    BTBHGPUSceneGeometryBatch batch;
    memset(&batch, 0, sizeof(batch));
    batch.kind = BTBH_GPU_SCENE_GEOMETRY_CUSTOM_MESH;
    batch.material_slot = -1;
    batch.instance_count = 1u;
    batch.albedo_red = 255u;
    batch.albedo_green = 255u;
    batch.albedo_blue = 255u;
    batch.albedo_alpha = 255u;
    batch.uv_scale_x = 1.0f;
    batch.uv_scale_y = 1.0f;
    batch.normal_scale = 1.0f;
    batch.roughness = 0.6f;
    batch.occlusion = 1.0f;
    batch.depth_compare = BTBH_GPU_SCENE_DEPTH_LESS;
    batch.cull_mode = BTBH_GPU_SCENE_CULL_BACK;
    batch.stencil_pass = BTBH_GPU_SCENE_STENCIL_REPLACE;
    batch.spot_shadow_visible = 1u;
    return batch;
}

static uint32_t add_transient(Scene *scene, float x, float y, float z, float scale, const float colour[4]) {
    assert(scene->instance_count < MAX_TRANSIENT);
    const uint32_t index = scene->instance_count++;
    instance_at(scene->instances[index], x, y, z, scale, 0.0f, colour, (float)index);
    return index;
}

static void add_batch(Scene *scene, BTBHGPUSceneGeometryBatch batch) {
    assert(scene->batch_count < MAX_BATCHES);
    scene->batches[scene->batch_count++] = batch;
}

static BTBHGPUSceneLighting scene_lighting(Shadows shadows) {
    BTBHGPUSceneLighting lighting;
    memset(&lighting, 0, sizeof(lighting));
    lighting.ambient_red = 0.8f;
    lighting.ambient_green = 0.85f;
    lighting.ambient_blue = 1.0f;
    lighting.ambient_energy = 0.4f;
    const float sun[3] = { 4.0f, 10.0f, 3.0f };
    lighting.directional_x = -sun[0];
    lighting.directional_y = -sun[1];
    lighting.directional_z = -sun[2];
    lighting.directional_energy = 1.6f;
    lighting.directional_red = 1.0f;
    lighting.directional_green = 0.95f;
    lighting.directional_blue = 0.9f;
    lighting.spot_range = 8.0f;
    lighting.spot_falloff = 1.0f;
    lighting.spot_inner_cutoff = 1.0f;
    lighting.spot_outer_cutoff = 0.0f;
    lighting.lamp_count = 2u;
    lighting.lamp_core_radius = 0.2f;
    for (uint32_t lamp = 0u; lamp < 2u; ++lamp) {
        lighting.lamp_position_reach[lamp][0] = -1.5f + 3.0f * (float)lamp;
        lighting.lamp_position_reach[lamp][1] = 1.0f;
        lighting.lamp_position_reach[lamp][2] = 0.6f;
        lighting.lamp_position_reach[lamp][3] = 3.0f;
        lighting.lamp_color_energy[lamp][0] = 1.0f;
        lighting.lamp_color_energy[lamp][1] = 0.8f;
        lighting.lamp_color_energy[lamp][2] = 0.6f;
        lighting.lamp_color_energy[lamp][3] = 0.5f;
    }
    if (shadows != SHADOWS_NONE) {
        const float centre[3] = { 0.0f, 0.0f, 0.0f };
        const float rcp = 1.0f / 5.0f;
        const float range = 1.0f / (1.0f - 30.0f);
        const float orthographic[16] = {
            rcp, 0.0f, 0.0f, 0.0f,
            0.0f, rcp, 0.0f, 0.0f,
            0.0f, 0.0f, range, 0.0f,
            0.0f, 0.0f, range, 1.0f,
        };
        const SmokeCamera light = look_at(sun, centre, 1.0f, orthographic);
        memcpy(lighting.directional_shadow_view_projection, light.view_projection,
            sizeof(lighting.directional_shadow_view_projection));
        lighting.directional_shadow_enabled = 1u;
        lighting.directional_shadow_softness = 1.0f;
        lighting.directional_shadow_filter = BTBH_GPU_SCENE_SHADOW_FILTER_SOFT;
    }
    if (shadows == SHADOWS_OMNI) {
        lighting.spot_position_x = 0.3f;
        lighting.spot_position_y = 1.6f;
        lighting.spot_position_z = 0.4f;
        lighting.spot_range = 6.0f;
        lighting.spot_direction_y = -1.0f;
        lighting.spot_energy = 2.0f;
        lighting.spot_red = 1.0f;
        lighting.spot_green = 0.7f;
        lighting.spot_blue = 0.4f;
        lighting.spot_inner_cutoff = 0.2f;
        lighting.spot_outer_cutoff = -1.0f;
        lighting.spot_shadow_enabled = 1u;
        lighting.spot_shadow_is_omni = 1u;
        lighting.spot_shadow_near = 0.1f;
        lighting.spot_shadow_softness = 1.0f;
        static const float faces[6][3] = {
            { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 },
        };
        const float eye[3] = { 0.3f, 1.6f, 0.4f };
        for (uint32_t face = 0u; face < 6u; ++face) {
            float target[3] = { eye[0] + faces[face][0], eye[1] + faces[face][1], eye[2] + faces[face][2] };
            if (face == 2u || face == 3u) {
                target[0] += 0.001f;
                target[2] += 0.001f;
            }
            const SmokeCamera view = look_at(eye, target, 1.5707964f, NULL);
            memcpy(lighting.omni_shadow_view_projection[face], view.view_projection, sizeof(view.view_projection));
        }
    }
    return lighting;
}

/* The street: the batches in the order a host would submit them. `shared`: the pieces in the shared buffers. */
static void build_scene(const SmokeWindow *smoke, const Options *options, bool shared, Scene *scene) {
    memset(scene, 0, sizeof(*scene));
    scene->name = options->name;
    const float eye[3] = { 0.0f, 3.2f, 3.6f };
    const float centre[3] = { 0.0f, 0.3f, -0.6f };
    scene->camera = look_at(eye, centre, 0.9f, NULL);
    scene->lighting = scene_lighting(options->shadows);

    BTBHGPUScenePost *post = &scene->post;
    post->tonemap_white = 1.0f;
    post->exposure = 1.0f;
    post->contrast = 1.0f;
    post->saturation = 1.0f;
    post->blur_curve = 1.0f;
    post->tonemap_mode = BTBH_GPU_SCENE_TONEMAP_REINHARD;
    post->upscale_mode = BTBH_GPU_SCENE_UPSCALE_LINEAR;
    post->opaque_sort = options->sorted ? (uint8_t)BTBH_GPU_SCENE_OPAQUE_SORT_FRONT_TO_BACK : 0u;
    if (options->cascades) {
        post->radiance_cascades.tier = BTBH_GPU_SCENE_RC_LOW;
        post->radiance_cascades.auto_frame_budget_milliseconds = 16.0f;
        post->radiance_cascades.indirect_intensity = 1.0f;
        post->radiance_cascades.hero_omni_emitter_index = -1;
        post->radiance_cascades.invalidate = 1u;
    }
    BTBHGPUCustomMesh *const *pieces = shared ? smoke->shared : smoke->own;
    static const float grey[4] = { 0.5f, 0.5f, 0.48f, 1.0f };
    static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    static const float sky[4] = { 0.6f, 0.75f, 0.95f, 1.0f };
    static const float hot[4] = { 1.0f, 0.5f, 0.2f, 1.0f };
    static const float blue[4] = { 0.3f, 0.45f, 0.9f, 0.7f };

    /* The ground. */
    BTBHGPUSceneGeometryBatch batch = base_batch();
    batch.custom_mesh = smoke->cube;
    batch.roughness = 0.4f;
    batch.first_instance = add_transient(scene, 0.0f, -0.25f, 0.0f, 1.0f, grey);
    scene->instances[batch.first_instance][0] = 12.0f;
    scene->instances[batch.first_instance][5] = 0.5f;
    scene->instances[batch.first_instance][10] = 12.0f;
    add_batch(scene, batch);

    /* The walls' and the glass's materials. */
    BTBHGPUSceneGeometryBatch wall = base_batch();
    wall.albedo_texture = smoke->albedo;
    wall.normal_texture = smoke->normal;
    wall.orm_texture = smoke->orm;
    wall.roughness = 0.8f;
    wall.detail_parameters[3] = 7.0f;
    wall.world_detail_table = &smoke->table;
    wall.flags = BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW;
    BTBHGPUSceneGeometryBatch glass = base_batch();
    glass.albedo_red = 150u;
    glass.albedo_green = 180u;
    glass.albedo_blue = 210u;
    glass.roughness = 0.1f;
    glass.detail_parameters[2] = 3.0f;
    glass.detail_parameters[3] = 5.0f;
    /* Chunk 0, interleaved piece by piece: a wall, then its glass. */
    for (uint32_t index = 0u; index < CHUNK_INSTANCES / 2u; ++index) {
        batch = wall;
        batch.custom_mesh = pieces[index % 5u];
        batch.instance_buffer = smoke->chunks[0];
        batch.first_instance = 2u * index;
        batch.instance_count = 2u;
        add_batch(scene, batch);
        batch = glass;
        batch.custom_mesh = pieces[5u + index % 5u];
        batch.instance_buffer = smoke->chunks[0];
        batch.first_instance = 2u * index;
        batch.instance_count = 2u;
        add_batch(scene, batch);
    }
    /* Chunk 1, sorted: its walls, then its glass. */
    for (uint32_t index = 0u; index < CHUNK_INSTANCES / 2u; ++index) {
        batch = wall;
        batch.custom_mesh = pieces[(index + 2u) % 5u];
        batch.instance_buffer = smoke->chunks[1];
        batch.first_instance = 2u * index;
        batch.instance_count = 2u;
        add_batch(scene, batch);
    }
    for (uint32_t index = 0u; index < CHUNK_INSTANCES / 2u; ++index) {
        batch = glass;
        batch.custom_mesh = pieces[5u + (index + 3u) % 5u];
        batch.instance_buffer = smoke->chunks[1];
        batch.first_instance = 2u * index;
        batch.instance_count = 2u;
        add_batch(scene, batch);
    }

    /* The model of two primitives, twice, a skinned model between: the second batch can't continue the first. */
    const float model_scale = 0.18f;
    for (uint32_t copy = 0u; copy < 2u; ++copy) {
        batch = base_batch();
        batch.kind = BTBH_GPU_SCENE_GEOMETRY_MODEL;
        batch.model = smoke->pair;
        batch.cull_mode = BTBH_GPU_SCENE_CULL_NONE;
        batch.albedo_red = 200u;
        batch.albedo_green = 120u;
        batch.albedo_blue = 90u;
        batch.first_instance = add_transient(scene, -1.2f + 0.2f * (float)copy, 0.05f + 0.3f * (float)copy, 1.0f,
            model_scale, white);
        add_batch(scene, batch);
        if (copy == 0u) {
            BTBHGPUSceneGeometryBatch skinned = base_batch();
            skinned.kind = BTBH_GPU_SCENE_GEOMETRY_MODEL;
            skinned.model = smoke->skinned;
            skinned.animation_library = smoke->animation;
            skinned.animation_time = 0.25f;
            skinned.cull_mode = BTBH_GPU_SCENE_CULL_NONE;
            skinned.albedo_red = 200u;
            skinned.albedo_green = 120u;
            skinned.albedo_blue = 90u;
            skinned.first_instance = add_transient(scene, 1.6f, 0.4f, 1.0f, 0.4f, white);
            add_batch(scene, skinned);
        }
    }

    /* A run of emissive pieces (the cascades' sources) and one of unlit ones. */
    for (uint32_t index = 0u; index < 3u; ++index) {
        batch = base_batch();
        batch.custom_mesh = pieces[(index * 3u) % PIECES];
        batch.emission_texture = smoke->glow;
        batch.emission_red = 255u;
        batch.emission_green = 200u;
        batch.emission_blue = 140u;
        batch.emission_energy = 3.0f;
        batch.first_instance = add_transient(scene, -0.5f + 0.5f * (float)index, 0.0f, 1.6f, 0.3f, white);
        add_batch(scene, batch);
    }
    for (uint32_t index = 0u; index < 2u; ++index) {
        batch = base_batch();
        batch.custom_mesh = pieces[1u + index];
        batch.unlit_enabled = 1u;
        batch.first_instance = add_transient(scene, 1.0f + 0.4f * (float)index, 0.0f, 1.7f, 0.25f, hot);
        add_batch(scene, batch);
    }

    /* Coplanar decals on the ground (LESS_EQUAL, after it). */
    for (uint32_t index = 0u; index < 3u; ++index) {
        batch = base_batch();
        batch.custom_mesh = smoke->plate;
        batch.depth_compare = BTBH_GPU_SCENE_DEPTH_LESS_EQUAL;
        batch.albedo_red = 90u;
        batch.albedo_green = 90u;
        batch.albedo_blue = 100u;
        batch.flags = BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW;
        batch.spot_shadow_visible = 0u;
        batch.first_instance = add_transient(scene, -2.0f + 1.4f * (float)index, 0.0f, 0.75f, 0.5f, white);
        add_batch(scene, batch);
    }

    /* A stencil writer, then two boxes that skip where it wrote. */
    batch = base_batch();
    batch.custom_mesh = smoke->cube;
    batch.stencil_reference = 1u;
    batch.stencil_mask = 0xffu;
    batch.first_instance = add_transient(scene, 2.2f, 0.3f, 0.9f, 0.6f, sky);
    add_batch(scene, batch);
    for (uint32_t index = 0u; index < 2u; ++index) {
        batch = base_batch();
        batch.custom_mesh = pieces[index + 4u];
        batch.stencil_compare = BTBH_GPU_SCENE_DEPTH_NOT_EQUAL;
        batch.stencil_pass = BTBH_GPU_SCENE_STENCIL_KEEP;
        batch.stencil_reference = 1u;
        batch.stencil_mask = 0xffu;
        batch.first_instance = add_transient(scene, 2.0f + 0.3f * (float)index, 0.0f, 1.2f, 0.45f, white);
        add_batch(scene, batch);
    }

    /* Three boxes, the middle one KEEP_ORDER (the flag breaks a run, as any field does). */
    for (uint32_t index = 0u; index < 3u; ++index) {
        batch = base_batch();
        batch.custom_mesh = pieces[index];
        batch.roughness = 0.3f;
        batch.metalness = 1.0f;
        batch.flags = index == 1u ? BTBH_GPU_SCENE_BATCH_KEEP_ORDER : 0u;
        batch.first_instance = add_transient(scene, -2.4f + 0.4f * (float)index, 0.0f, 1.3f, 0.3f, white);
        add_batch(scene, batch);
    }

    /* Transparent pieces. */
    for (uint32_t index = 0u; index < 3u; ++index) {
        batch = base_batch();
        batch.custom_mesh = pieces[6u + index];
        batch.transparency_enabled = 1u;
        batch.depth_compare = BTBH_GPU_SCENE_DEPTH_LESS_EQUAL;
        batch.albedo_alpha = 160u;
        batch.first_instance = add_transient(scene, -0.9f + 0.6f * (float)index, 0.1f, 0.5f, 0.35f, blue);
        add_batch(scene, batch);
    }
}

static uint8_t *render(SmokeWindow *smoke, const Scene *scene) {
    BTBHGPUSceneReport report;
    memset(&report, 0, sizeof(report));
    const BTBHGPUResult result = btbh_gpu_window_scene_render_to_target(
        smoke->window, smoke->target,
        shader_paths[0], shader_paths[1], shader_paths[2], shader_paths[3],
        shader_paths[4], shader_paths[5], shader_paths[6], shader_paths[7],
        shader_paths[8], shader_paths[9], shader_paths[10],
        SMOKE_SHADER_FORMAT, TARGET_SIZE, TARGET_SIZE,
        scene->camera.view_projection, scene->camera.right, scene->camera.up, scene->camera.position,
        &scene->lighting, &scene->post,
        &scene->instances[0][0], scene->instance_count, scene->instance_count,
        scene->batches, NULL, NULL, scene->batch_count,
        12u, 14u, 20u, 255u, &report);
    if (result != BTBH_GPU_OK) {
        fprintf(stderr, "%s: %s\n", scene->name, report.message);
    }
    assert(result == BTBH_GPU_OK);
    if (scene->post.radiance_cascades.tier != BTBH_GPU_SCENE_RC_OFF) {
        assert(report.radiance_cascades.active_tier != BTBH_GPU_SCENE_RC_OFF);
    }
    return read_texture(smoke->window, smoke->target);
}

#if defined(SMOKE_HAS_SHARED)
/* The scene presented to the window (a frame the canvas submits), and the scene telemetry after it. */
static BTBHGPUSceneTelemetryReport present(SmokeWindow *smoke, const Scene *scene) {
    BTBHGPUSceneReport report;
    memset(&report, 0, sizeof(report));
    const BTBHGPUResult presented = btbh_gpu_window_scene_present_instances(smoke->window,
        shader_paths[0], shader_paths[1], shader_paths[2], shader_paths[3],
        shader_paths[4], shader_paths[5], shader_paths[6], shader_paths[7],
        shader_paths[8], shader_paths[9], shader_paths[10],
        SMOKE_SHADER_FORMAT, 160u, 90u,
        scene->camera.view_projection, scene->camera.right, scene->camera.up, scene->camera.position,
        &scene->lighting, &scene->post,
        &scene->instances[0][0], scene->instance_count, scene->instance_count,
        scene->batches, scene->batch_count,
        12u, 14u, 20u, 255u, &report);
    if (presented != BTBH_GPU_OK) {
        fprintf(stderr, "%s: present: %s\n", scene->name, report.message);
    }
    assert(presented == BTBH_GPU_OK);
    BTBHGPUSceneTelemetryReport telemetry;
    memset(&telemetry, 0, sizeof(telemetry));
    assert(btbh_gpu_window_scene_read_telemetry(smoke->window, &telemetry) != 0u);
    BTBHGPUCanvasPass pass;
    memset(&pass, 0, sizeof(pass));
    BTBHGPUCanvasReport canvas_report;
    memset(&canvas_report, 0, sizeof(canvas_report));
    const BTBHGPUResult canvas = btbh_gpu_window_canvas_present(smoke->window, canvas_shader_paths[0],
        canvas_shader_paths[1], canvas_shader_paths[2], SMOKE_SHADER_FORMAT, NULL, 0u, NULL, 0u, &pass, 1u,
        &canvas_report);
    if (canvas != BTBH_GPU_OK) {
        fprintf(stderr, "%s: canvas: %s\n", scene->name, canvas_report.message);
    }
    assert(canvas == BTBH_GPU_OK);
    return telemetry;
}
#endif

/* ------------------------------------------------------------------------------------------- the gallery */

static void check_gallery(SmokeWindow *smoke) {
    const char *dump = SDL_getenv("BTBH_SCENE_RUNS_DUMP");
    const char *reference_dir = SDL_getenv("BTBH_SCENE_RUNS_REFERENCE");
    static Scene scene;
    static const uint8_t clear[3] = { 12u, 14u, 20u };
    uint32_t compared = 0u;
#if defined(SMOKE_HAS_SHARED)
    const uint32_t variants = 4u;
#else
    const uint32_t variants = 1u;
#endif
    for (uint32_t index = 0u; index < GALLERY_COUNT; ++index) {
        uint8_t *first = NULL;
        uint8_t *reference = NULL;
        if (reference_dir != NULL && reference_dir[0] != '\0') {
            char path[1100];
            (void)snprintf(path, sizeof(path), "%s/runs-%s.png", reference_dir, gallery[index].name);
            reference = malloc((size_t)TEXELS * 4u);
            assert(reference != NULL);
            const bool loaded = load_png(path, reference);
            if (!loaded) {
                fprintf(stderr, "no reference %s\n", path);
            }
            assert(loaded);
        }
        printf("gallery %-11s", gallery[index].name);
        /* Own buffers with runs, own without, shared with, shared without. */
        for (uint32_t variant = 0u; variant < variants; ++variant) {
            const bool shared = variant >= 2u;
            const bool runs = variant % 2u == 0u;
            build_scene(smoke, &gallery[index], shared, &scene);
            set_runs(runs);
            uint8_t *image = render(smoke, &scene);
            uint8_t *again = render(smoke, &scene);
            set_runs(true);
            assert(memcmp(image, again, (size_t)TEXELS * 4u) == 0);
            free(again);
            if (first == NULL) {
                first = image;
                printf(" %u lit pixels;", lit_pixels(image, clear));
                assert(lit_pixels(image, clear) > TEXELS / 2u);
            } else {
                const uint32_t differing = differing_pixels(first, image);
                printf(" %s buffers, runs %s: %u differ;", shared ? "shared" : "own", runs ? "on" : "off",
                    differing);
                assert(differing == 0u);
            }
            if (reference != NULL) {
                const uint32_t differing = differing_pixels(reference, image);
                if (differing != 0u) {
                    printf(" reference: %u differ\n", differing);
                }
                assert(differing == 0u);
                compared += 1u;
            }
            if (image != first) {
                free(image);
            }
        }
        printf("\n");
        if (dump != NULL && dump[0] != '\0') {
            char path[1100];
            (void)snprintf(path, sizeof(path), "%s/runs-%s.png", dump, gallery[index].name);
            assert(btbh_gpu_write_png_rgba8(path, TARGET_SIZE, TARGET_SIZE, first) == BTBH_GPU_OK);
        }
        free(first);
        free(reference);
    }
    if (compared > 0u) {
        printf("gallery: %u renders (%u pixels) match the references\n", compared, compared * TEXELS);
    }
}

#if defined(SMOKE_HAS_SHARED)
static void check_run_counts(SmokeWindow *smoke) {
    static Scene scene;
    for (uint32_t index = 0u; index < GALLERY_COUNT; ++index) {
        for (uint32_t shared = 0u; shared < 2u; ++shared) {
            build_scene(smoke, &gallery[index], shared != 0u, &scene);
            const BTBHGPUSceneTelemetryReport with_runs = present(smoke, &scene);
            set_runs(false);
            const BTBHGPUSceneTelemetryReport without = present(smoke, &scene);
            set_runs(true);
            printf("runs %-11s %s: %u geometry, %u shadow, %u cascade-source draws continued a run\n",
                gallery[index].name, shared != 0u ? "shared" : "own   ", with_runs.geometry_run_draw_count,
                with_runs.shadow_run_draw_count, with_runs.rc_source_run_draw_count);
            /* Chunk 1's six walls and six glass panes (5 + 5), the model's second primitives, the emissive and
               unlit runs, the decals, the skippers, the transparent run. */
            assert(with_runs.geometry_run_draw_count >= 20u);
            assert((gallery[index].shadows != SHADOWS_NONE) == (with_runs.shadow_run_draw_count > 0u));
            assert(gallery[index].cascades == (with_runs.rc_source_run_draw_count > 0u));
            assert(without.geometry_run_draw_count == 0u);
            assert(without.shadow_run_draw_count == 0u);
            assert(without.rc_source_run_draw_count == 0u);
        }
    }
}

/* -------------------------------------------------------------------------------- shared meshes' rules */

static uint8_t *render_one(SmokeWindow *smoke, BTBHGPUCustomMesh *mesh) {
    static Scene scene;
    const Options options = { .name = "one", .shadows = SHADOWS_SUN };
    build_scene(smoke, &options, false, &scene);
    /* The ground, and the mesh twice. */
    scene.batch_count = 1u;
    BTBHGPUSceneGeometryBatch batch = base_batch();
    batch.custom_mesh = mesh;
    batch.cull_mode = BTBH_GPU_SCENE_CULL_NONE;
    batch.first_instance = add_transient(&scene, -0.6f, 0.0f, 0.0f, 1.2f, (const float[4]){ 1.0f, 1.0f, 1.0f, 1.0f });
    add_batch(&scene, batch);
    batch.first_instance = add_transient(&scene, 0.8f, 0.0f, -0.3f, 0.9f, (const float[4]){ 0.8f, 0.9f, 1.0f, 1.0f });
    add_batch(&scene, batch);
    return render(smoke, &scene);
}

static void expect_refused(BTBHGPUResult result, const BTBHGPUCustomMeshReport *report, const char *what) {
    printf("refused %-44s %d: %s\n", what, (int)result, report->message);
    assert(result == BTBH_GPU_INVALID_ARGUMENT);
    assert(report->result == BTBH_GPU_INVALID_ARGUMENT);
    assert(report->message[0] != '\0');
}

static void check_shared_rules(BTBHGPUDevice *device, SmokeWindow *smoke) {
    /* A fresh window: its arena is empty. */
    const BTBHGPUWindowDescriptor descriptor = {
        .title = "BTBH scene runs smoke (shared meshes)",
        .width = 64,
        .height = 64,
        .hidden = 1,
        .high_pixel_density = 0,
    };
    BTBHGPUWindowReport window_report;
    memset(&window_report, 0, sizeof(window_report));
    BTBHGPUWindow *window = btbh_gpu_window_create(&descriptor, &window_report);
    assert(window != NULL);
    assert(btbh_gpu_window_claim(device, window, &window_report) == BTBH_GPU_OK);

    static PieceData cube;
    memset(&cube, 0, sizeof(cube));
    {
        const float centre[3] = { 0.0f, 0.5f, 0.0f };
        const float half[3] = { 0.5f, 0.5f, 0.5f };
        add_box(cube.vertices, &cube.vertex_count, cube.indices, &cube.index_count, centre, half, 0.0f, 1.0f);
    }
    const uint32_t cube_bytes = cube.vertex_count * FLOATS * (uint32_t)sizeof(float);
    BTBHGPUCustomMeshReport report;

    /* Refusals at creation. */
    memset(&report, 0, sizeof(report));
    assert(btbh_gpu_window_custom_mesh_create_ex(window, cube.vertices, cube_bytes, cube.vertex_count, cube.indices,
        cube.index_count, 1u << 1u, &report) == NULL);
    expect_refused(report.result, &report, "an unknown flag (1 << 1)");
    assert(strstr(report.message, "flags") != NULL);
    memset(&report, 0, sizeof(report));
    assert(btbh_gpu_window_custom_mesh_create_ex(window, cube.vertices, cube_bytes, cube.vertex_count, cube.indices,
        cube.index_count, BTBH_GPU_CUSTOM_MESH_SHARED_BUFFERS | 0x80000000u, &report) == NULL);
    expect_refused(report.result, &report, "the flag with an unknown one (1 << 31)");
    memset(&report, 0, sizeof(report));
    assert(btbh_gpu_window_custom_mesh_create_ex(window, NULL, cube_bytes, cube.vertex_count, cube.indices,
        cube.index_count, BTBH_GPU_CUSTOM_MESH_SHARED_BUFFERS, &report) == NULL);
    expect_refused(report.result, &report, "shared, no vertices");
    memset(&report, 0, sizeof(report));
    assert(btbh_gpu_window_custom_mesh_create_ex(window, cube.vertices, cube_bytes - 4u, cube.vertex_count,
        cube.indices, cube.index_count, BTBH_GPU_CUSTOM_MESH_SHARED_BUFFERS, &report) == NULL);
    expect_refused(report.result, &report, "shared, a byte count that isn't the vertices'");
    {
        uint32_t bad[36];
        memcpy(bad, cube.indices, sizeof(bad));
        bad[7] = cube.vertex_count;
        memset(&report, 0, sizeof(report));
        assert(btbh_gpu_window_custom_mesh_create_ex(window, cube.vertices, cube_bytes, cube.vertex_count, bad,
            36u, BTBH_GPU_CUSTOM_MESH_SHARED_BUFFERS, &report) == NULL);
        expect_refused(report.result, &report, "shared, an index past the vertices");
    }
    assert(btbh_gpu_window_custom_mesh_create_ex(window, cube.vertices, cube_bytes, cube.vertex_count, cube.indices,
        cube.index_count, BTBH_GPU_CUSTOM_MESH_SHARED_BUFFERS, NULL) == NULL);
    assert(window->vertex_arena.block_count == 0u && window->index_arena.block_count == 0u);

    /* Two meshes, then a model, in one block each for vertices and indices. */
    BTBHGPUCustomMesh *a = create_mesh(window, cube.vertices, cube.vertex_count, cube.indices, cube.index_count, true);
    BTBHGPUCustomMesh *b = create_mesh(window, cube.vertices, 6u, NULL, 0u, true);
    assert(a->vertex_block != NULL && a->vertex_block == b->vertex_block && a->vertex_buffer == b->vertex_buffer);
    assert(a->index_block != NULL && b->index_block == NULL && b->index_buffer == NULL);
    assert(a->vertex_base == 0 && b->vertex_base == (int32_t)cube.vertex_count && a->first_index == 0u);
    BTBHGPUModelReport model_report;
    memset(&model_report, 0, sizeof(model_report));
    BTBHGPUModel *model = btbh_gpu_window_model_load(window,
        BTBH_GPU_SMOKE_FIXTURE_DIR "/instanced-transformed-default-material.gltf", &model_report);
    assert(model != NULL);
    assert(model->primitives[0].vertex_block == a->vertex_block);
    assert(model->primitives[0].index_block == a->index_block);
    assert(model->primitives[0].vertex_base == (int32_t)cube.vertex_count + 6);
    assert(model->primitives[0].first_index == cube.index_count);
    assert(window->vertex_arena.block_count == 1u && window->index_arena.block_count == 1u);
    printf("shared: two meshes and a model in one vertex block (bases 0, %d, %d) and one index block\n",
        b->vertex_base, model->primitives[0].vertex_base);

    /* Updates past the ranges are refused, and the mesh stays as it was. */
    {
        static float more[25u * FLOATS];
        memcpy(more, cube.vertices, sizeof(float) * FLOATS * 24u);
        memcpy(&more[24u * FLOATS], cube.vertices, sizeof(float) * FLOATS);
        memset(&report, 0, sizeof(report));
        expect_refused(btbh_gpu_custom_mesh_update(a, more, 25u * FLOATS * (uint32_t)sizeof(float), cube.indices,
            cube.index_count, &report), &report, "an update of 25 vertices (made with 24)");
        assert(strstr(report.message, "fit") != NULL);
        uint32_t indices[42];
        memcpy(indices, cube.indices, sizeof(uint32_t) * 36u);
        memcpy(&indices[36], cube.indices, sizeof(uint32_t) * 6u);
        memset(&report, 0, sizeof(report));
        expect_refused(btbh_gpu_custom_mesh_update(a, cube.vertices, cube_bytes, indices, 42u, &report), &report,
            "an update of 42 indices (made with 36)");
        memset(&report, 0, sizeof(report));
        expect_refused(btbh_gpu_custom_mesh_update(b, cube.vertices, 6u * FLOATS * (uint32_t)sizeof(float),
            cube.indices, 3u, &report), &report, "indices for a mesh made without");
        assert(a->vertex_count == 24u && a->index_count == 36u && b->vertex_count == 6u && b->index_count == 0u);
    }
    /* (The rest draws in the gallery's window: a mesh is drawn by the window that made it.) */
    BTBHGPUCustomMesh *full_own = create_mesh(smoke->window, cube.vertices, cube.vertex_count, cube.indices,
        cube.index_count, false);
    BTBHGPUCustomMesh *full_shared = create_mesh(smoke->window, cube.vertices, cube.vertex_count, cube.indices,
        cube.index_count, true);
    uint8_t *expected = render_one(smoke, full_own);
    uint8_t *actual = render_one(smoke, full_shared);
    assert(differing_pixels(expected, actual) == 0u);
    free(actual);
    memset(&report, 0, sizeof(report));
    assert(btbh_gpu_custom_mesh_update(full_shared, cube.vertices, cube_bytes + FLOATS * (uint32_t)sizeof(float),
        cube.indices, cube.index_count, &report) == BTBH_GPU_INVALID_ARGUMENT);
    actual = render_one(smoke, full_shared);
    assert(differing_pixels(expected, actual) == 0u);
    free(actual);
    /* An update within them: three faces (12 vertices, 18 indices), then the whole box again. */
    BTBHGPUCustomMesh *half_own = create_mesh(smoke->window, cube.vertices, 12u, cube.indices, 18u, false);
    memset(&report, 0, sizeof(report));
    assert(btbh_gpu_custom_mesh_update(full_shared, cube.vertices, 12u * FLOATS * (uint32_t)sizeof(float),
        cube.indices, 18u, &report) == BTBH_GPU_OK);
    assert(report.vertex_count == 12u && report.index_count == 18u);
    uint8_t *half_expected = render_one(smoke, half_own);
    actual = render_one(smoke, full_shared);
    assert(differing_pixels(half_expected, actual) == 0u);
    assert(differing_pixels(half_expected, expected) > 0u);
    free(actual);
    memset(&report, 0, sizeof(report));
    assert(btbh_gpu_custom_mesh_update(full_shared, cube.vertices, cube_bytes, cube.indices, cube.index_count,
        &report) == BTBH_GPU_OK);
    actual = render_one(smoke, full_shared);
    assert(differing_pixels(expected, actual) == 0u);
    free(actual);
    printf("shared: an update within the ranges draws as a mesh of its own (3 faces, then 6)\n");
    free(half_expected);
    free(expected);
    btbh_gpu_custom_mesh_destroy(half_own);
    btbh_gpu_custom_mesh_destroy(full_own);
    btbh_gpu_custom_mesh_destroy(full_shared);

    /* Destroying leaves holes; the block's buffers go with its last range. */
    BTBHGPUGeometryBlock *vertex_block = a->vertex_block;
    BTBHGPUGeometryBlock *index_block = a->index_block;
    assert(vertex_block->live == 2u + model->primitive_count && index_block->live == 1u + model->primitive_count);
    btbh_gpu_custom_mesh_destroy(a);
    btbh_gpu_custom_mesh_destroy(b);
    assert(vertex_block->live == model->primitive_count && vertex_block->buffer != NULL);
    assert(index_block->live == model->primitive_count && index_block->buffer != NULL);
    btbh_gpu_model_destroy(model);
    assert(vertex_block->live == 0u && vertex_block->buffer == NULL);
    assert(index_block->live == 0u && index_block->buffer == NULL);
    printf("shared: the blocks' buffers went with the model, after the meshes\n");

    /* A window closed with a shared mesh alive. */
    BTBHGPUCustomMesh *survivor = create_mesh(window, cube.vertices, cube.vertex_count, cube.indices,
        cube.index_count, true);
    btbh_gpu_window_destroy(window);
    assert(survivor->owner == NULL && survivor->vertex_block == NULL && survivor->vertex_buffer == NULL);
    memset(&report, 0, sizeof(report));
    expect_refused(btbh_gpu_custom_mesh_update(survivor, cube.vertices, cube_bytes, cube.indices, cube.index_count,
        &report), &report, "an update after the window closed");
    btbh_gpu_custom_mesh_destroy(survivor);
    printf("shared: a mesh outlived its window and was destroyed after it\n");
}
#endif

/* ------------------------------------------------------------------------------------------------ main */

static bool load_shader_paths(void) {
    for (int index = 0; index < 11; ++index) {
        (void)snprintf(shader_paths[index], sizeof(shader_paths[index]), "%s/%s%s", BTBH_GPU_SMOKE_SHADER_DIR,
            shader_names[index], SMOKE_SHADER_EXTENSION);
        FILE *file = fopen(shader_paths[index], "rb");
        if (file == NULL) {
            fprintf(stderr, "skipped: no shader %s\n", shader_paths[index]);
            return false;
        }
        fclose(file);
    }
    for (int index = 0; index < 3; ++index) {
        (void)snprintf(canvas_shader_paths[index], sizeof(canvas_shader_paths[index]), "%s/%s%s",
            BTBH_GPU_SMOKE_SHADER_DIR, canvas_shader_names[index], SMOKE_SHADER_EXTENSION);
    }
    return true;
}

static SmokeWindow open_smoke_window(BTBHGPUDevice *device) {
    SmokeWindow smoke;
    memset(&smoke, 0, sizeof(smoke));
    const BTBHGPUWindowDescriptor descriptor = {
        .title = "BTBH scene runs smoke",
        .width = 320,
        .height = 180,
        .hidden = 1,
        .high_pixel_density = 0,
    };
    BTBHGPUWindowReport window_report;
    memset(&window_report, 0, sizeof(window_report));
    smoke.window = btbh_gpu_window_create(&descriptor, &window_report);
    assert(smoke.window != NULL);
    assert(btbh_gpu_window_claim(device, smoke.window, &window_report) == BTBH_GPU_OK);
    BTBHGPUTextureReport texture_report;
    memset(&texture_report, 0, sizeof(texture_report));
    smoke.target = btbh_gpu_window_render_target_create(smoke.window, TARGET_SIZE, TARGET_SIZE,
        BTBH_GPU_TEXTURE_FILTER_LINEAR, &texture_report);
    assert(smoke.target != NULL);
    smoke.albedo = noise_texture(smoke.window, 11u, 150u, 100u, BTBH_GPU_TEXTURE_COLOR_SPACE_SRGB);
    smoke.normal = noise_texture(smoke.window, 12u, 110u, 36u, BTBH_GPU_TEXTURE_COLOR_SPACE_LINEAR);
    smoke.orm = noise_texture(smoke.window, 13u, 120u, 120u, BTBH_GPU_TEXTURE_COLOR_SPACE_LINEAR);
    smoke.glow = noise_texture(smoke.window, 14u, 160u, 90u, BTBH_GPU_TEXTURE_COLOR_SPACE_SRGB);
    {
        uint8_t *layers[4];
        for (uint32_t layer = 0u; layer < 4u; ++layer) {
            layers[layer] = noise_pixels(64u, 100u + layer, layer % 2u == 0u ? 100u : 60u, 56u);
        }
        smoke.maps = btbh_gpu_window_texture_array_create_rgba8_ex(smoke.window, (const uint8_t *const *)layers, 64u,
            64u, 4u, 1u, BTBH_GPU_TEXTURE_FILTER_TRILINEAR, BTBH_GPU_TEXTURE_WRAP_REPEAT,
            BTBH_GPU_TEXTURE_COLOR_SPACE_LINEAR, &texture_report);
        for (uint32_t layer = 0u; layer < 4u; ++layer) {
            free(layers[layer]);
        }
        assert(smoke.maps != NULL);
        for (uint32_t entry = 0u; entry < 2u; ++entry) {
            BTBHGPUSceneWorldDetailTableEntry *e = &smoke.entries[entry];
            memset(e, 0, sizeof(*e));
            e->level_tile[0] = e->level_tile[1] = 0.6f + 0.3f * (float)entry;
            e->wall_tile[0] = e->wall_tile[1] = 0.5f + 0.25f * (float)entry;
            e->normal_layer = (uint8_t)(2u * entry);
            e->orm_layer = (uint8_t)(2u * entry + 1u);
        }
        smoke.table.map_layers = smoke.maps;
        smoke.table.entries = smoke.entries;
        smoke.table.entry_count = 2u;
        smoke.table.seed_component = 3u;
    }
    for (uint32_t piece = 0u; piece < PIECES; ++piece) {
        smoke.own[piece] = create_piece(smoke.window, piece, false);
#if defined(SMOKE_HAS_SHARED)
        smoke.shared[piece] = create_piece(smoke.window, piece, true);
        assert(smoke.shared[piece]->vertex_buffer == smoke.shared[0]->vertex_buffer);
#endif
    }
    {
        static PieceData data;
        memset(&data, 0, sizeof(data));
        const float centre[3] = { 0.0f, 0.0f, 0.0f };
        const float half[3] = { 0.5f, 0.5f, 0.5f };
        add_box(data.vertices, &data.vertex_count, data.indices, &data.index_count, centre, half, 0.0f, 1.0f);
        smoke.cube = create_mesh(smoke.window, data.vertices, data.vertex_count, data.indices, data.index_count,
            false);
        /* The plate: the cube's top face, moved to y = 0. */
        float plate[4u * FLOATS];
        memcpy(plate, &data.vertices[8u * FLOATS], sizeof(plate));
        for (uint32_t vertex = 0u; vertex < 4u; ++vertex) {
            plate[vertex * FLOATS + 1u] = 0.0f;
        }
        const uint32_t plate_indices[6] = { 0u, 2u, 1u, 0u, 3u, 2u };
        smoke.plate = create_mesh(smoke.window, plate, 4u, plate_indices, 6u, false);
    }
    BTBHGPUModelReport model_report;
    memset(&model_report, 0, sizeof(model_report));
    smoke.pair = btbh_gpu_window_model_load(smoke.window,
        BTBH_GPU_SMOKE_FIXTURE_DIR "/instanced-transformed-default-material.gltf", &model_report);
    if (smoke.pair == NULL) {
        fprintf(stderr, "instanced-transformed-default-material.gltf: %s\n", model_report.message);
    }
    assert(smoke.pair != NULL && smoke.pair->primitive_count >= 2u);
    smoke.skinned = btbh_gpu_window_model_load(smoke.window, BTBH_GPU_SMOKE_FIXTURE_DIR "/skinned-quad.gltf",
        &model_report);
    assert(smoke.skinned != NULL);
    BTBHGPUAnimationLibraryReport animation_report;
    memset(&animation_report, 0, sizeof(animation_report));
    smoke.animation = btbh_gpu_animation_library_load(BTBH_GPU_SMOKE_FIXTURE_DIR "/skinned-quad.gltf",
        &animation_report);
    assert(smoke.animation != NULL);
    for (uint32_t chunk = 0u; chunk < 2u; ++chunk) {
        BTBHGPUInstanceBufferReport buffer_report;
        memset(&buffer_report, 0, sizeof(buffer_report));
        smoke.chunks[chunk] = btbh_gpu_window_instance_buffer_create(smoke.window, CHUNK_INSTANCES,
            FLOATS * (uint32_t)sizeof(float), &buffer_report);
        assert(smoke.chunks[chunk] != NULL);
        float instances[CHUNK_INSTANCES][FLOATS];
        for (uint32_t index = 0u; index < CHUNK_INSTANCES; ++index) {
            chunk_instance(chunk, index, instances[index]);
        }
        assert(btbh_gpu_instance_buffer_upload(smoke.chunks[chunk], 0u, instances, (uint32_t)sizeof(instances),
            &buffer_report) == BTBH_GPU_OK);
    }
    return smoke;
}

static void close_smoke_window(SmokeWindow *smoke) {
    for (uint32_t piece = 0u; piece < PIECES; ++piece) {
        btbh_gpu_custom_mesh_destroy(smoke->own[piece]);
        btbh_gpu_custom_mesh_destroy(smoke->shared[piece]);
    }
    btbh_gpu_custom_mesh_destroy(smoke->cube);
    btbh_gpu_custom_mesh_destroy(smoke->plate);
    btbh_gpu_instance_buffer_destroy(smoke->chunks[0]);
    btbh_gpu_instance_buffer_destroy(smoke->chunks[1]);
    btbh_gpu_model_destroy(smoke->pair);
    btbh_gpu_model_destroy(smoke->skinned);
    btbh_gpu_animation_library_destroy(smoke->animation);
    btbh_gpu_texture_destroy(smoke->maps);
    btbh_gpu_texture_destroy(smoke->glow);
    btbh_gpu_texture_destroy(smoke->orm);
    btbh_gpu_texture_destroy(smoke->normal);
    btbh_gpu_texture_destroy(smoke->albedo);
    btbh_gpu_texture_destroy(smoke->target);
    btbh_gpu_window_destroy(smoke->window);
    memset(smoke, 0, sizeof(*smoke));
}

int main(void) {
    /* (Unbuffered: what was checked shows before a failed assertion.) */
    (void)setvbuf(stdout, NULL, _IONBF, 0);
    if (!load_shader_paths()) {
        return SMOKE_SKIP;
    }
    (void)SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    const BTBHGPUDeviceDescriptor device_descriptor = {
        .backend = SMOKE_BACKEND,
        .required_features = BTBH_GPU_REQUIRED_FEATURES,
        .debug_mode = 1u,
    };
    BTBHGPUDeviceReport device_report;
    memset(&device_report, 0, sizeof(device_report));
    BTBHGPUDevice *device = btbh_gpu_device_create(&device_descriptor, &device_report);
    if (device == NULL) {
        fprintf(stderr, "skipped: no device (%s)\n", device_report.message);
        return SMOKE_SKIP;
    }
    SmokeWindow smoke = open_smoke_window(device);
    check_gallery(&smoke);
#if defined(SMOKE_HAS_SHARED)
    check_run_counts(&smoke);
    check_shared_rules(device, &smoke);
#endif
    close_smoke_window(&smoke);
    btbh_gpu_device_destroy(device);
    puts("btbh_gpu_scene_runs_smoke: passed");
    return 0;
}
