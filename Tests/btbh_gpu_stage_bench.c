/* The first frames of a new stage (Be The Bullet Hell v2's night perf round, 0.11.0): a measurement, not a test.
 *
 * A window at v2's Steam Deck preset (1280x800, rendered at 960x600, FSR 1, HBAO, FXAA, a soft sun shadow, 8 lamps)
 * runs a stage, then switches to the next one the way a game loads a level between two frames: it destroys the stage's
 * kit meshes, models, textures and instance buffers, makes the next stage's (new kit meshes in the shared geometry
 * buffers and on their own, glTF models, textures with mips up to 4096^2, a world-detail table over a new texture
 * array, persistent instance buffers), prewarms the scene for it, and presents its first frames. Each frame is a
 * scene present and a canvas present (which acquires the swapchain image and submits), timed on the CPU, with the
 * scene report's split and the lazy creation log (every timed SDL call is recorded here: threshold 0).
 *
 * Environment:
 * - BTBH_STAGE_BENCH_MODE: '+'-separated options:
 *   noprewarm (the stage isn't prewarmed), keep (the previous stage isn't destroyed), idle (SDL_WaitForGPUIdle after
 *   the load and the prewarm), flush (one canvas-only present, a loading screen's last frame, after the load),
 *   resize (the window is resized at each switch, 1280x800 <-> 1440x900), small (no texture over 1024^2), shared (every
 *   kit mesh in the shared geometry buffers, as v2 makes its kit; else every other one), staging (an upload staging
 *   budget of 128 MiB, btbh_gpu_device_set_upload_staging_budget; BTBH_STAGE_BENCH_STAGING_MB, since 0.13.0, another
 *   size).
 * - BTBH_STAGE_BENCH_SWITCHES (default 4), BTBH_STAGE_BENCH_FRAMES (frames after each switch, default 10),
 *   BTBH_STAGE_BENCH_DRAWS (default 2000), BTBH_STAGE_BENCH_HIDDEN=1 (a hidden window: no swapchain image, so the
 *   frames neither present nor run SDL's presenting clean-up).
 * - The exit (0.13.0, Be The Bullet Hell v1's exit stall), timed step by step after the last frame:
 *   BTBH_STAGE_BENCH_EXIT_EXTRA=<n> (default 0) makes n more textures (256^2 with mips), n custom meshes with buffers
 *   of their own and n instance buffers just before it, each its own upload, with no frame after them (a level that
 *   streamed more in, then quit); BTBH_STAGE_BENCH_EXIT ('+'-separated): nodestroy (the host destroys nothing itself:
 *   the window's destroy releases it), idle (SDL_WaitForGPUIdle before anything is destroyed), flush (after the
 *   resources are destroyed, one canvas-only present: SDL frees released memory in a submit that presents). The
 *   staging mode's budget applies to the extra uploads too. Output: "STAGE exit ..." with each step's milliseconds.
 *
 * Output: "STAGE load ..." per switch (the CPU time of each part of the load), then one "STAGE frame ..." line per
 * frame (stage, frame after the switch, the scene present's wall clock and its report's split, the canvas present's
 * wall clock, its command buffer, swapchain acquire and submit, the lazy creation entries' count and time) and, for
 * the first frames after a switch, "STAGE lazy ..." lines with the entries. With no device it is skipped (77). */
#include "btbh_gpu/btbh_gpu.h"
#include "btbh_gpu_internal.h"

#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BENCH_SKIP 77
#define BENCH_RENDER_WIDTH 960u
#define BENCH_RENDER_HEIGHT 600u
#define BENCH_FLOATS 24u
#define BENCH_PIECES 48u
#define BENCH_MODELS 6u
#define BENCH_BIG_TEXTURES 4u
#define BENCH_CHUNKS 8u
#define BENCH_CHUNK_INSTANCES 96u
#define BENCH_ENTRIES 256u

#if defined(__APPLE__)
#define BENCH_SHADER_EXTENSION ".metallib"
#define BENCH_SHADER_FORMAT BTBH_GPU_SHADER_FORMAT_METALLIB
#define BENCH_BACKEND BTBH_GPU_BACKEND_METAL
#else
#define BENCH_SHADER_EXTENSION ".spv"
#define BENCH_SHADER_FORMAT BTBH_GPU_SHADER_FORMAT_SPIRV
#define BENCH_BACKEND BTBH_GPU_BACKEND_VULKAN
#endif

static const char *const scene_shader_names[11] = {
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
static const char *const canvas_shader_names[3] = {
    "production_canvas.vert",
    "production_canvas.frag",
    "production_canvas_textured.frag",
};
static const char *const model_fixtures[3] = {
    "instanced-transformed-default-material.gltf",
    "material-factor-triangle.gltf",
    "external-texture-triangle.gltf",
};

static char scene_shaders[11][1024];
static char canvas_shaders[3][1024];
static bool all_shared;

typedef struct Stage {
    BTBHGPUCustomMesh *pieces[BENCH_PIECES];
    BTBHGPUCustomMesh *ground;
    BTBHGPUModel *models[BENCH_MODELS];
    BTBHGPUTexture *albedo;
    BTBHGPUTexture *normal;
    BTBHGPUTexture *orm;
    BTBHGPUTexture *big[BENCH_BIG_TEXTURES];
    BTBHGPUTexture *maps;
    BTBHGPUSceneWorldDetailTable table;
    BTBHGPUSceneWorldDetailTableEntry entries[8];
    BTBHGPUInstanceBuffer *chunks[BENCH_CHUNKS];
    float *instances;
    uint32_t instance_count;
    BTBHGPUSceneGeometryBatch *batches;
    uint32_t batch_count;
} Stage;

typedef struct Options {
    bool prewarm;
    bool keep;
    bool idle;
    bool flush;
    bool resize;
    bool small;
    bool staging;
} Options;

/* What the exit makes just before it (BTBH_STAGE_BENCH_EXIT_EXTRA). */
typedef struct ExitExtra {
    BTBHGPUTexture **textures;
    BTBHGPUCustomMesh **meshes;
    BTBHGPUInstanceBuffer **buffers;
    uint32_t count;
} ExitExtra;

static uint32_t bench_random(uint32_t *state) {
    *state = *state * 1664525u + 1013904223u;
    return *state >> 8u;
}

static float bench_unit(uint32_t *state) {
    return (float)(bench_random(state) & 0xffffu) / 65535.0f;
}

static int env_int(const char *name, int fallback, int low, int high) {
    const char *value = SDL_getenv(name);
    if (value == NULL || value[0] == 0) {
        return fallback;
    }
    const int parsed = atoi(value);
    return parsed < low ? low : (parsed > high ? high : parsed);
}

static bool mode_has(const char *mode, const char *option) {
    const size_t length = strlen(option);
    for (const char *at = mode; at != NULL && *at != 0;) {
        const char *end = strchr(at, '+');
        const size_t item = end != NULL ? (size_t)(end - at) : strlen(at);
        if (item == length && strncmp(at, option, length) == 0) {
            return true;
        }
        at = end != NULL ? end + 1 : NULL;
    }
    return false;
}

static double ms_since(uint64_t started) {
    return (double)(SDL_GetTicksNS() - started) / 1.0e6;
}

static void fail(const char *what, const char *message) {
    fprintf(stderr, "%s: %s\n", what, message);
    exit(1);
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

static Camera look_at(const float eye[3], const float centre[3], float fov_y, float aspect) {
    Camera camera;
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
    const float near_plane = 0.5f;
    const float far_plane = 400.0f;
    const float h = 1.0f / tanf(0.5f * fov_y);
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

/* An axis-aligned box, 24 vertices and 36 indices, joints.x (the world-detail index) = `index`. */
static void add_box(float *vertices, uint32_t *vertex_count, uint32_t *indices, uint32_t *index_count,
    const float centre[3], const float half[3], float index) {
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
            float *vertex = &vertices[(size_t)(*vertex_count) * BENCH_FLOATS];
            memset(vertex, 0, BENCH_FLOATS * sizeof(float));
            for (int axis = 0; axis < 3; ++axis) {
                vertex[axis] = centre[axis] + half[axis] * (n[axis] + su * u[axis] + sv * v[axis]);
                vertex[3 + axis] = n[axis];
                vertex[8 + axis] = u[axis];
            }
            vertex[6] = 0.5f * su + 0.5f;
            vertex[7] = 0.5f * sv + 0.5f;
            vertex[11] = 1.0f;
            for (int channel = 12; channel < 16; ++channel) {
                vertex[channel] = 1.0f;
            }
            vertex[16] = index;
            vertex[20] = 1.0f;
            *vertex_count += 1u;
        }
        const uint32_t wound[6] = { base, base + 2u, base + 1u, base, base + 3u, base + 2u };
        memcpy(&indices[*index_count], wound, sizeof(wound));
        *index_count += 6u;
    }
}

/* Kit piece p of a stage: a wall slab and a grid of window frames and sills (the stage's seed varies the grid). */
static BTBHGPUCustomMesh *make_piece(BTBHGPUWindow *window, uint32_t piece, uint32_t seed) {
    const uint32_t columns = 2u + (piece + seed) % 4u;
    const uint32_t rows = 2u + (piece / 4u + seed) % 5u;
    const uint32_t boxes = 1u + columns * rows * 2u;
    float *vertices = calloc((size_t)boxes * 24u * BENCH_FLOATS, sizeof(float));
    uint32_t *indices = calloc((size_t)boxes * 36u, sizeof(uint32_t));
    if (vertices == NULL || indices == NULL) {
        fail("piece", "out of memory");
    }
    uint32_t vertex_count = 0u;
    uint32_t index_count = 0u;
    const float slab_centre[3] = { 0.0f, 1.5f, 0.0f };
    const float slab_half[3] = { 1.0f, 1.5f, 0.15f };
    add_box(vertices, &vertex_count, indices, &index_count, slab_centre, slab_half, (float)(piece % 8u));
    for (uint32_t row = 0u; row < rows; ++row) {
        for (uint32_t column = 0u; column < columns; ++column) {
            const float x = -1.0f + 2.0f * ((float)column + 0.5f) / (float)columns;
            const float y = 3.0f * ((float)row + 0.5f) / (float)rows;
            const float frame_centre[3] = { x, y, 0.17f };
            const float frame_half[3] = { 0.6f / (float)columns, 0.9f / (float)rows, 0.03f };
            add_box(vertices, &vertex_count, indices, &index_count, frame_centre, frame_half, 255.0f);
            const float sill_centre[3] = { x, y - 1.0f / (float)rows, 0.2f };
            const float sill_half[3] = { 0.68f / (float)columns, 0.02f, 0.06f };
            add_box(vertices, &vertex_count, indices, &index_count, sill_centre, sill_half, 255.0f);
        }
    }
    BTBHGPUCustomMeshReport report;
    memset(&report, 0, sizeof(report));
    BTBHGPUCustomMesh *mesh = btbh_gpu_window_custom_mesh_create_ex(window, vertices,
        vertex_count * BENCH_FLOATS * (uint32_t)sizeof(float), vertex_count, indices, index_count,
        all_shared || piece % 2u == 0u ? BTBH_GPU_CUSTOM_MESH_SHARED_BUFFERS : 0u, &report);
    free(vertices);
    free(indices);
    if (mesh == NULL) {
        fail("piece", report.message);
    }
    return mesh;
}

static uint8_t *noise_pixels(uint32_t size, uint32_t seed) {
    uint8_t *pixels = malloc((size_t)size * size * 4u);
    if (pixels == NULL) {
        fail("pixels", "out of memory");
    }
    uint32_t state = seed;
    for (size_t i = 0u; i < (size_t)size * size * 4u; i += 4u) {
        const uint32_t value = bench_random(&state);
        pixels[i + 0u] = (uint8_t)(96u + value % 128u);
        pixels[i + 1u] = (uint8_t)(96u + (value >> 7) % 128u);
        pixels[i + 2u] = (uint8_t)(96u + (value >> 14) % 128u);
        pixels[i + 3u] = 255u;
    }
    return pixels;
}

static BTBHGPUTexture *noise_texture(BTBHGPUWindow *window, uint32_t size, uint32_t seed,
    BTBHGPUTextureColorSpace color_space) {
    uint8_t *pixels = noise_pixels(size, seed);
    BTBHGPUTextureReport report;
    memset(&report, 0, sizeof(report));
    BTBHGPUTexture *texture = btbh_gpu_window_texture_create_rgba8_ex(window, pixels, size, size, 1u,
        BTBH_GPU_TEXTURE_FILTER_TRILINEAR, BTBH_GPU_TEXTURE_WRAP_REPEAT, color_space, &report);
    free(pixels);
    if (texture == NULL) {
        fail("texture", report.message);
    }
    return texture;
}

static void fill_batch(BTBHGPUSceneGeometryBatch *batch, uint32_t first, uint32_t count) {
    memset(batch, 0, sizeof(*batch));
    batch->kind = BTBH_GPU_SCENE_GEOMETRY_CUSTOM_MESH;
    batch->material_slot = -1;
    batch->first_instance = first;
    batch->instance_count = count;
    batch->albedo_red = 255u;
    batch->albedo_green = 255u;
    batch->albedo_blue = 255u;
    batch->albedo_alpha = 255u;
    batch->uv_scale_x = 1.0f;
    batch->uv_scale_y = 1.0f;
    batch->normal_scale = 1.0f;
    batch->roughness = 0.7f;
    batch->occlusion = 1.0f;
    batch->depth_compare = BTBH_GPU_SCENE_DEPTH_LESS;
    batch->cull_mode = BTBH_GPU_SCENE_CULL_BACK;
    batch->stencil_pass = BTBH_GPU_SCENE_STENCIL_REPLACE;
    batch->spot_shadow_visible = 1u;
}

static void put_instance(float *instance, float x, float y, float z, float yaw, float scale, uint32_t *state) {
    memset(instance, 0, BENCH_FLOATS * sizeof(float));
    instance[0] = scale * cosf(yaw);
    instance[2] = -scale * sinf(yaw);
    instance[5] = scale;
    instance[8] = scale * sinf(yaw);
    instance[10] = scale * cosf(yaw);
    instance[12] = x;
    instance[13] = y;
    instance[14] = z;
    instance[15] = 1.0f;
    instance[16] = 0.6f + 0.4f * bench_unit(state);
    instance[17] = 0.6f + 0.4f * bench_unit(state);
    instance[18] = 0.6f + 0.4f * bench_unit(state);
    instance[19] = 1.0f;
    instance[23] = (float)(bench_random(state) % 97u);
}

typedef struct LoadTimes {
    double meshes;
    double models;
    double textures;
    double table;
    double instance_buffers;
    double total;
} LoadTimes;

/* The stage's content (its seed makes every mesh and texture new) and its batches. */
static LoadTimes load_stage(BTBHGPUWindow *window, Stage *stage, uint32_t seed, uint32_t draws,
    const Options *options) {
    LoadTimes times;
    memset(&times, 0, sizeof(times));
    memset(stage, 0, sizeof(*stage));
    const uint64_t started = SDL_GetTicksNS();
    uint64_t part = started;
    for (uint32_t piece = 0u; piece < BENCH_PIECES; ++piece) {
        stage->pieces[piece] = make_piece(window, piece, seed);
    }
    {
        float vertices[24u * BENCH_FLOATS];
        uint32_t indices[36];
        uint32_t vertex_count = 0u;
        uint32_t index_count = 0u;
        const float centre[3] = { 0.0f, -0.5f, 0.0f };
        const float half[3] = { 200.0f, 0.5f, 200.0f };
        add_box(vertices, &vertex_count, indices, &index_count, centre, half, 0.0f);
        BTBHGPUCustomMeshReport report;
        memset(&report, 0, sizeof(report));
        stage->ground = btbh_gpu_window_custom_mesh_create(window, vertices, (uint32_t)sizeof(vertices), vertex_count,
            indices, index_count, &report);
        if (stage->ground == NULL) {
            fail("ground", report.message);
        }
    }
    times.meshes = ms_since(part);
    part = SDL_GetTicksNS();
    for (uint32_t model = 0u; model < BENCH_MODELS; ++model) {
        char path[1024];
        (void)snprintf(path, sizeof(path), "%s/%s", BTBH_GPU_SMOKE_FIXTURE_DIR, model_fixtures[model % 3u]);
        BTBHGPUModelReport report;
        memset(&report, 0, sizeof(report));
        stage->models[model] = btbh_gpu_window_model_load(window, path, &report);
        if (stage->models[model] == NULL) {
            fail(path, report.message);
        }
    }
    times.models = ms_since(part);
    part = SDL_GetTicksNS();
    const uint32_t material_size = options->small ? 512u : 1024u;
    stage->albedo = noise_texture(window, material_size, seed * 31u + 1u, BTBH_GPU_TEXTURE_COLOR_SPACE_SRGB);
    stage->normal = noise_texture(window, material_size, seed * 31u + 2u, BTBH_GPU_TEXTURE_COLOR_SPACE_LINEAR);
    stage->orm = noise_texture(window, material_size, seed * 31u + 3u, BTBH_GPU_TEXTURE_COLOR_SPACE_LINEAR);
    for (uint32_t index = 0u; index < BENCH_BIG_TEXTURES; ++index) {
        const uint32_t size = options->small ? 1024u : (index == 0u ? 4096u : 2048u);
        stage->big[index] = noise_texture(window, size, seed * 31u + 10u + index, BTBH_GPU_TEXTURE_COLOR_SPACE_SRGB);
    }
    times.textures = ms_since(part);
    part = SDL_GetTicksNS();
    {
        const uint32_t layer_size = options->small ? 512u : 1024u;
        uint8_t *layers[16];
        for (uint32_t layer = 0u; layer < 16u; ++layer) {
            layers[layer] = noise_pixels(layer_size, seed * 100u + layer);
        }
        BTBHGPUTextureReport report;
        memset(&report, 0, sizeof(report));
        stage->maps = btbh_gpu_window_texture_array_create_rgba8_ex(window, (const uint8_t *const *)layers, layer_size,
            layer_size, 16u, 1u, BTBH_GPU_TEXTURE_FILTER_TRILINEAR, BTBH_GPU_TEXTURE_WRAP_REPEAT,
            BTBH_GPU_TEXTURE_COLOR_SPACE_LINEAR, &report);
        for (uint32_t layer = 0u; layer < 16u; ++layer) {
            free(layers[layer]);
        }
        if (stage->maps == NULL) {
            fail("array", report.message);
        }
        for (uint32_t entry = 0u; entry < 8u; ++entry) {
            BTBHGPUSceneWorldDetailTableEntry *e = &stage->entries[entry];
            memset(e, 0, sizeof(*e));
            e->level_tile[0] = 1.5f + 0.25f * (float)entry;
            e->level_tile[1] = e->level_tile[0];
            e->wall_tile[0] = 1.0f + 0.2f * (float)entry;
            e->wall_tile[1] = e->wall_tile[0];
            e->normal_layer = (uint8_t)(2u * entry);
            e->orm_layer = (uint8_t)(2u * entry + 1u);
        }
        stage->table.map_layers = stage->maps;
        stage->table.entries = stage->entries;
        stage->table.entry_count = 8u;
        stage->table.seed_component = 4u;
    }
    times.table = ms_since(part);
    part = SDL_GetTicksNS();
    uint32_t state = seed * 7919u + 13u;
    uint32_t chunk_firsts[BENCH_CHUNKS][16];
    uint32_t chunk_counts[BENCH_CHUNKS][16];
    for (uint32_t chunk = 0u; chunk < BENCH_CHUNKS; ++chunk) {
        float instances[BENCH_CHUNK_INSTANCES][BENCH_FLOATS];
        uint32_t count = 0u;
        for (uint32_t local = 0u; local < 16u; ++local) {
            chunk_firsts[chunk][local] = count;
            chunk_counts[chunk][local] = 1u + bench_random(&state) % 6u;
            for (uint32_t copy = 0u; copy < chunk_counts[chunk][local]; ++copy) {
                const float x = -96.0f + 24.0f * (float)chunk + 1.5f * (float)local;
                const float z = -80.0f - 3.0f * (float)copy;
                put_instance(instances[count++], x, 0.0f, z, 0.0f, 1.0f, &state);
            }
        }
        BTBHGPUInstanceBufferReport report;
        memset(&report, 0, sizeof(report));
        stage->chunks[chunk] = btbh_gpu_window_instance_buffer_create(window, BENCH_CHUNK_INSTANCES,
            BENCH_FLOATS * (uint32_t)sizeof(float), &report);
        if (stage->chunks[chunk] == NULL ||
            btbh_gpu_instance_buffer_upload(stage->chunks[chunk], 0u, instances,
                count * BENCH_FLOATS * (uint32_t)sizeof(float), &report) != BTBH_GPU_OK) {
            fail("instances", report.message);
        }
    }
    times.instance_buffers = ms_since(part);
    /* The batches: kit pieces with 1..5 transient instances on a 64 x 48 city of lots (the table on them), the
       persistent chunks' pieces, the models, and the ground under the 4096^2 texture. */
    const uint32_t batch_capacity = draws + BENCH_CHUNKS * 16u + BENCH_MODELS + 1u;
    stage->instances = calloc(((size_t)draws * 5u + BENCH_MODELS + 1u) * BENCH_FLOATS, sizeof(float));
    stage->batches = calloc(batch_capacity, sizeof(BTBHGPUSceneGeometryBatch));
    if (stage->instances == NULL || stage->batches == NULL) {
        fail("batches", "out of memory");
    }
    for (uint32_t draw = 0u; draw < draws; ++draw) {
        const uint32_t count = 1u + bench_random(&state) % 5u;
        BTBHGPUSceneGeometryBatch *batch = &stage->batches[stage->batch_count++];
        fill_batch(batch, stage->instance_count, count);
        batch->custom_mesh = stage->pieces[draw % BENCH_PIECES];
        batch->albedo_texture = draw % 7u == 0u ? stage->big[1u + draw % (BENCH_BIG_TEXTURES - 1u)] : stage->albedo;
        batch->normal_texture = stage->normal;
        batch->orm_texture = stage->orm;
        batch->detail_parameters[3] = 7.0f;
        batch->world_detail_table = &stage->table;
        for (uint32_t copy = 0u; copy < count; ++copy) {
            const uint32_t lot = bench_random(&state) % (64u * 48u);
            const float x = -96.0f + 3.0f * (float)(lot % 64u) + bench_unit(&state);
            const float z = -72.0f + 3.0f * (float)(lot / 64u) + bench_unit(&state);
            const float y = 3.0f * (float)(bench_random(&state) % 4u);
            const float yaw = 1.5707963f * (float)(bench_random(&state) % 4u);
            put_instance(&stage->instances[(size_t)stage->instance_count++ * BENCH_FLOATS], x, y, z, yaw, 1.0f,
                &state);
        }
    }
    for (uint32_t chunk = 0u; chunk < BENCH_CHUNKS; ++chunk) {
        for (uint32_t local = 0u; local < 16u; ++local) {
            BTBHGPUSceneGeometryBatch *batch = &stage->batches[stage->batch_count++];
            fill_batch(batch, chunk_firsts[chunk][local], chunk_counts[chunk][local]);
            batch->custom_mesh = stage->pieces[(chunk * 16u + local) % BENCH_PIECES];
            batch->instance_buffer = stage->chunks[chunk];
            batch->albedo_texture = stage->albedo;
            batch->detail_parameters[3] = 7.0f;
            batch->flags = BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW;
            batch->spot_shadow_visible = 0u;
        }
    }
    for (uint32_t model = 0u; model < BENCH_MODELS; ++model) {
        BTBHGPUSceneGeometryBatch *batch = &stage->batches[stage->batch_count++];
        fill_batch(batch, stage->instance_count, 1u);
        batch->kind = BTBH_GPU_SCENE_GEOMETRY_MODEL;
        batch->custom_mesh = NULL;
        batch->model = stage->models[model];
        batch->cull_mode = BTBH_GPU_SCENE_CULL_NONE;
        put_instance(&stage->instances[(size_t)stage->instance_count++ * BENCH_FLOATS], -6.0f + 2.5f * (float)model,
            1.0f, 10.0f, 0.3f * (float)model, 2.0f, &state);
    }
    BTBHGPUSceneGeometryBatch *ground = &stage->batches[stage->batch_count++];
    fill_batch(ground, stage->instance_count, 1u);
    ground->custom_mesh = stage->ground;
    ground->albedo_texture = stage->big[0];
    ground->uv_scale_x = 40.0f;
    ground->uv_scale_y = 40.0f;
    float *instance = &stage->instances[(size_t)stage->instance_count++ * BENCH_FLOATS];
    memset(instance, 0, BENCH_FLOATS * sizeof(float));
    instance[0] = 1.0f;
    instance[5] = 1.0f;
    instance[10] = 1.0f;
    instance[15] = 1.0f;
    instance[16] = 0.45f;
    instance[17] = 0.45f;
    instance[18] = 0.42f;
    instance[19] = 1.0f;
    times.total = ms_since(started);
    return times;
}

static double destroy_stage(Stage *stage) {
    const uint64_t started = SDL_GetTicksNS();
    for (uint32_t piece = 0u; piece < BENCH_PIECES; ++piece) {
        btbh_gpu_custom_mesh_destroy(stage->pieces[piece]);
    }
    btbh_gpu_custom_mesh_destroy(stage->ground);
    for (uint32_t model = 0u; model < BENCH_MODELS; ++model) {
        btbh_gpu_model_destroy(stage->models[model]);
    }
    btbh_gpu_texture_destroy(stage->albedo);
    btbh_gpu_texture_destroy(stage->normal);
    btbh_gpu_texture_destroy(stage->orm);
    for (uint32_t index = 0u; index < BENCH_BIG_TEXTURES; ++index) {
        btbh_gpu_texture_destroy(stage->big[index]);
    }
    btbh_gpu_texture_destroy(stage->maps);
    for (uint32_t chunk = 0u; chunk < BENCH_CHUNKS; ++chunk) {
        btbh_gpu_instance_buffer_destroy(stage->chunks[chunk]);
    }
    free(stage->instances);
    free(stage->batches);
    memset(stage, 0, sizeof(*stage));
    return ms_since(started);
}

static BTBHGPUSceneLighting bench_lighting(void) {
    BTBHGPUSceneLighting lighting;
    memset(&lighting, 0, sizeof(lighting));
    lighting.ambient_red = 0.8f;
    lighting.ambient_green = 0.85f;
    lighting.ambient_blue = 1.0f;
    lighting.ambient_energy = 0.35f;
    const float sun[3] = { 40.0f, 100.0f, 30.0f };
    lighting.directional_x = -sun[0];
    lighting.directional_y = -sun[1];
    lighting.directional_z = -sun[2];
    lighting.directional_energy = 2.5f;
    lighting.directional_red = 1.0f;
    lighting.directional_green = 0.95f;
    lighting.directional_blue = 0.9f;
    lighting.spot_range = 8.0f;
    lighting.spot_falloff = 1.0f;
    lighting.spot_inner_cutoff = 1.0f;
    lighting.spot_outer_cutoff = 0.0f;
    float forward[3] = { -sun[0], -sun[1], -sun[2] };
    normalize3(forward);
    const float world_up[3] = { 0.0f, 0.0f, 1.0f };
    float side[3];
    cross3(forward, world_up, side);
    normalize3(side);
    float up[3];
    cross3(side, forward, up);
    const float view[16] = {
        side[0], up[0], -forward[0], 0.0f,
        side[1], up[1], -forward[1], 0.0f,
        side[2], up[2], -forward[2], 0.0f,
        -dot3(sun, side), -dot3(sun, up), dot3(sun, forward), 1.0f,
    };
    const float scale = 1.0f / 70.0f;
    const float range = 1.0f / (1.0f - 300.0f);
    const float orthographic[16] = {
        scale, 0.0f, 0.0f, 0.0f,
        0.0f, scale, 0.0f, 0.0f,
        0.0f, 0.0f, range, 0.0f,
        0.0f, 0.0f, range, 1.0f,
    };
    multiply(orthographic, view, lighting.directional_shadow_view_projection);
    lighting.directional_shadow_enabled = 1u;
    lighting.directional_shadow_softness = 1.0f;
    lighting.directional_shadow_filter = BTBH_GPU_SCENE_SHADOW_FILTER_SOFT;
    lighting.lamp_count = 8u;
    lighting.lamp_core_radius = 0.5f;
    for (uint32_t lamp = 0u; lamp < 8u; ++lamp) {
        lighting.lamp_position_reach[lamp][0] = -40.0f + 10.0f * (float)lamp;
        lighting.lamp_position_reach[lamp][1] = 3.0f;
        lighting.lamp_position_reach[lamp][2] = -10.0f + 3.0f * (float)(lamp % 5u);
        lighting.lamp_position_reach[lamp][3] = 15.0f;
        lighting.lamp_color_energy[lamp][0] = 1.0f;
        lighting.lamp_color_energy[lamp][1] = 0.8f;
        lighting.lamp_color_energy[lamp][2] = 0.6f;
        lighting.lamp_color_energy[lamp][3] = 0.3f;
    }
    return lighting;
}

/* The Deck preset's post: FSR 1 from 75 %, HBAO, FXAA. */
static BTBHGPUScenePost bench_post(void) {
    BTBHGPUScenePost post;
    memset(&post, 0, sizeof(post));
    post.tonemap_white = 4.0f;
    post.exposure = 1.0f;
    post.contrast = 1.0f;
    post.saturation = 1.0f;
    post.blur_curve = 1.0f;
    post.tonemap_mode = BTBH_GPU_SCENE_TONEMAP_REINHARD;
    post.upscale_mode = BTBH_GPU_SCENE_UPSCALE_FSR1;
    post.ssao_enabled = 2u;
    post.anti_aliasing = 1u;
    return post;
}

static void prewarm(BTBHGPUWindow *window, const Stage *stage, const BTBHGPUSceneLighting *lighting,
    const BTBHGPUScenePost *post) {
    BTBHGPUSceneReport report;
    if (btbh_gpu_window_scene_prewarm(window, scene_shaders[0], scene_shaders[1], scene_shaders[2], scene_shaders[3],
            scene_shaders[4], scene_shaders[5], scene_shaders[6], scene_shaders[7], scene_shaders[8], scene_shaders[9],
            scene_shaders[10], BENCH_SHADER_FORMAT, BENCH_RENDER_WIDTH, BENCH_RENDER_HEIGHT, lighting, post,
            stage->batches, stage->batch_count, &report) != BTBH_GPU_OK) {
        fail("prewarm", report.message);
    }
}

static double canvas_only(BTBHGPUWindow *window) {
    BTBHGPUCanvasPass pass;
    memset(&pass, 0, sizeof(pass));
    BTBHGPUCanvasReport report;
    const uint64_t started = SDL_GetTicksNS();
    if (btbh_gpu_window_canvas_present(window, canvas_shaders[0], canvas_shaders[1], canvas_shaders[2],
            BENCH_SHADER_FORMAT, NULL, 0u, NULL, 0u, &pass, 1u, &report) != BTBH_GPU_OK) {
        fail("canvas", report.message);
    }
    return ms_since(started);
}

/* What the lazy creation log recorded in a frame: the timed calls by name, and the creations. */
typedef struct FrameLog {
    double scene_command;
    double scene_map;
    double canvas_command;
    double acquire;
    double submit;
    uint32_t created;
    double created_ms;
    uint32_t created_covered;
    double created_covered_ms;
} FrameLog;

static FrameLog drain_log(BTBHGPUWindow *window, bool print, const char *label) {
    FrameLog log;
    memset(&log, 0, sizeof(log));
    static BTBHGPULazyCreationEntry entries[BENCH_ENTRIES];
    uint32_t count = 0u;
    do {
        if (btbh_gpu_window_lazy_creation_report(window, entries, BENCH_ENTRIES, &count) != BTBH_GPU_OK) {
            fail("lazy report", "refused");
        }
        for (uint32_t index = 0u; index < count; ++index) {
            const BTBHGPULazyCreationEntry *entry = &entries[index];
            const double ms = (double)entry->microseconds / 1000.0;
            if (entry->kind == BTBH_GPU_LAZY_CREATION_SLOW_CALL) {
                if (strncmp(entry->name, "SDL_AcquireGPUCommandBuffer (scene)", 35) == 0) {
                    log.scene_command += ms;
                } else if (strncmp(entry->name, "SDL_MapGPUTransferBuffer (scene", 31) == 0) {
                    log.scene_map += ms;
                } else if (strncmp(entry->name, "SDL_AcquireGPUCommandBuffer (canvas)", 36) == 0) {
                    log.canvas_command += ms;
                } else if (strncmp(entry->name, "SDL_WaitAndAcquireGPUSwapchainTexture", 37) == 0) {
                    log.acquire += ms;
                } else if (strncmp(entry->name, "SDL_SubmitGPUCommandBuffer (frame)", 34) == 0) {
                    log.submit += ms;
                }
                continue;
            }
            log.created += 1u;
            log.created_ms += ms;
            if (entry->prewarm_covers != 0u) {
                log.created_covered += 1u;
                log.created_covered_ms += ms;
            }
            if (print) {
                printf("STAGE lazy %s frame=%llu call=%u kind=%u prewarm=%u ms=%.3f %s\n", label,
                    (unsigned long long)entry->frame_index, (unsigned)entry->call, (unsigned)entry->kind,
                    (unsigned)entry->prewarm_covers, ms, entry->name);
            }
        }
    } while (count == BENCH_ENTRIES);
    return log;
}

static void run_frame(BTBHGPUWindow *window, const Stage *stage, const Camera *camera,
    const BTBHGPUSceneLighting *lighting, const BTBHGPUScenePost *post, uint32_t switch_index, int frame,
    bool print_entries) {
    BTBHGPUSceneReport report;
    const uint64_t started = SDL_GetTicksNS();
    if (btbh_gpu_window_scene_present_instances(window, scene_shaders[0], scene_shaders[1], scene_shaders[2],
            scene_shaders[3], scene_shaders[4], scene_shaders[5], scene_shaders[6], scene_shaders[7],
            scene_shaders[8], scene_shaders[9], scene_shaders[10], BENCH_SHADER_FORMAT, BENCH_RENDER_WIDTH,
            BENCH_RENDER_HEIGHT, camera->view_projection, camera->right, camera->up, camera->position, lighting,
            post, stage->instances, stage->instance_count, stage->instance_count, stage->batches,
            stage->batch_count, 90u, 110u, 140u, 255u, &report) != BTBH_GPU_OK) {
        fail("present", report.message);
    }
    const double scene_ms = ms_since(started);
    const double canvas_ms = canvas_only(window);
    char label[48];
    (void)snprintf(label, sizeof(label), "switch=%u frame=%d", switch_index, frame);
    const FrameLog log = drain_log(window, print_entries, label);
    const double split = report.cpu_prepare_milliseconds + report.cpu_upload_milliseconds +
        report.cpu_shadow_encode_milliseconds + report.cpu_target_ensure_milliseconds +
        report.cpu_geometry_encode_milliseconds + report.cpu_post_encode_milliseconds;
    printf("STAGE frame switch=%u frame=%d total=%.3f scene=%.3f split=%.3f prepare=%.3f upload=%.3f shadow=%.3f "
        "targets=%.3f geometry=%.3f post=%.3f scene_cmd=%.3f scene_map=%.3f canvas=%.3f canvas_cmd=%.3f "
        "acquire=%.3f submit=%.3f made=%u made_ms=%.3f covered=%u covered_ms=%.3f\n",
        switch_index, frame, scene_ms + canvas_ms, scene_ms, split, report.cpu_prepare_milliseconds,
        report.cpu_upload_milliseconds, report.cpu_shadow_encode_milliseconds, report.cpu_target_ensure_milliseconds,
        report.cpu_geometry_encode_milliseconds, report.cpu_post_encode_milliseconds, log.scene_command,
        log.scene_map, canvas_ms, log.canvas_command, log.acquire, log.submit, log.created, log.created_ms,
        log.created_covered, log.created_covered_ms);
    (void)fflush(stdout);
}

int main(void) {
    /* (Unbuffered: the lines keep their order among SDL_Log's.) */
    (void)setvbuf(stdout, NULL, _IONBF, 0);
    (void)SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    const char *mode = SDL_getenv("BTBH_STAGE_BENCH_MODE");
    if (mode == NULL) {
        mode = "";
    }
    Options options;
    options.prewarm = !mode_has(mode, "noprewarm");
    options.keep = mode_has(mode, "keep");
    options.idle = mode_has(mode, "idle");
    options.flush = mode_has(mode, "flush");
    options.resize = mode_has(mode, "resize");
    options.small = mode_has(mode, "small");
    options.staging = mode_has(mode, "staging");
    all_shared = mode_has(mode, "shared");
    const int switches = env_int("BTBH_STAGE_BENCH_SWITCHES", 4, 1, 100);
    const int frames = env_int("BTBH_STAGE_BENCH_FRAMES", 10, 1, 1000);
    const uint32_t draws = (uint32_t)env_int("BTBH_STAGE_BENCH_DRAWS", 2000, 1, 20000);
    const bool hidden = env_int("BTBH_STAGE_BENCH_HIDDEN", 0, 0, 1) == 1;

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
    /* (BTBH_STAGE_BENCH_STAGING_MB, since 0.13.0: the staging mode's budget in MiB; 128 by default, 0 sets none.) */
    const uint32_t staging_mb = (uint32_t)env_int("BTBH_STAGE_BENCH_STAGING_MB", 128, 0, 4095);
    if (options.staging && btbh_gpu_device_set_upload_staging_budget(device, staging_mb << 20) != BTBH_GPU_OK) {
        fail("staging", "refused");
    }
    for (uint32_t index = 0u; index < 11u; ++index) {
        (void)snprintf(scene_shaders[index], sizeof(scene_shaders[index]), "%s/%s%s", BTBH_GPU_SMOKE_SHADER_DIR,
            scene_shader_names[index], BENCH_SHADER_EXTENSION);
    }
    for (uint32_t index = 0u; index < 3u; ++index) {
        (void)snprintf(canvas_shaders[index], sizeof(canvas_shaders[index]), "%s/%s%s", BTBH_GPU_SMOKE_SHADER_DIR,
            canvas_shader_names[index], BENCH_SHADER_EXTENSION);
    }
    const BTBHGPUWindowDescriptor descriptor = {
        .title = "BTBH stage bench",
        .width = 1280u,
        .height = 800u,
        .hidden = hidden ? 1 : 0,
        .high_pixel_density = 0,
    };
    BTBHGPUWindowReport window_report;
    memset(&window_report, 0, sizeof(window_report));
    BTBHGPUWindow *window = btbh_gpu_window_create(&descriptor, &window_report);
    if (window == NULL || btbh_gpu_window_claim(device, window, &window_report) != BTBH_GPU_OK) {
        fail("window", window_report.message);
    }
    btbh_gpu_window_set_lazy_creation_log_enabled(window, 1u);
    btbh_gpu_lazy_creation_set_call_threshold(0u);
    printf("STAGE start mode=%s switches=%d frames=%d draws=%u hidden=%d\n", mode, switches, frames, draws,
        hidden ? 1 : 0);

    const BTBHGPUSceneLighting lighting = bench_lighting();
    const BTBHGPUScenePost post = bench_post();
    const float eye[3] = { 0.0f, 95.0f, 95.0f };
    const float centre[3] = { 0.0f, 0.0f, 0.0f };
    Stage stages[2];
    memset(stages, 0, sizeof(stages));
    Stage kept[64];
    uint32_t kept_count = 0u;
    uint32_t current = 0u;
    uint32_t width = 1280u;
    uint32_t height = 800u;
    /* The first stage: loaded, prewarmed and run long enough that its frames are steady. */
    LoadTimes times = load_stage(window, &stages[current], 1u, draws, &options);
    printf("STAGE load switch=0 total=%.3f meshes=%.3f models=%.3f textures=%.3f table=%.3f instance_buffers=%.3f\n",
        times.total, times.meshes, times.models, times.textures, times.table, times.instance_buffers);
    if (options.prewarm) {
        prewarm(window, &stages[current], &lighting, &post);
    }
    (void)drain_log(window, false, "");
    Camera camera = look_at(eye, centre, 0.9f, (float)width / (float)height);
    for (int frame = 0; frame < 40; ++frame) {
        run_frame(window, &stages[current], &camera, &lighting, &post, 0u, frame, frame < 2);
    }
    for (int switch_index = 1; switch_index <= switches; ++switch_index) {
        const uint32_t next = current ^ 1u;
        double destroyed = 0.0;
        if (options.keep && kept_count < 64u) {
            kept[kept_count++] = stages[current];
            memset(&stages[current], 0, sizeof(stages[current]));
        } else {
            destroyed = destroy_stage(&stages[current]);
        }
        if (options.resize) {
            width = width == 1280u ? 1440u : 1280u;
            height = height == 800u ? 900u : 800u;
            if (btbh_gpu_window_resize(window, width, height, &window_report) != BTBH_GPU_OK) {
                fail("resize", window_report.message);
            }
            camera = look_at(eye, centre, 0.9f, (float)width / (float)height);
        }
        times = load_stage(window, &stages[next], 1u + (uint32_t)switch_index, draws, &options);
        uint64_t started = SDL_GetTicksNS();
        if (options.prewarm) {
            prewarm(window, &stages[next], &lighting, &post);
        }
        const double prewarm_ms = ms_since(started);
        started = SDL_GetTicksNS();
        if (options.idle) {
            (void)SDL_WaitForGPUIdle(device->gpu);
        }
        const double idle_ms = ms_since(started);
        const double flush_ms = options.flush ? canvas_only(window) : 0.0;
        (void)drain_log(window, false, "");
        printf("STAGE load switch=%d destroy=%.3f total=%.3f meshes=%.3f models=%.3f textures=%.3f table=%.3f "
            "instance_buffers=%.3f prewarm=%.3f idle=%.3f flush=%.3f\n",
            switch_index, destroyed, times.total, times.meshes, times.models, times.textures, times.table,
            times.instance_buffers, prewarm_ms, idle_ms, flush_ms);
        current = next;
        for (int frame = 0; frame < frames; ++frame) {
            run_frame(window, &stages[current], &camera, &lighting, &post, (uint32_t)switch_index, frame, frame < 2);
        }
    }
    printf("STAGE done\n");
    (void)fflush(stdout);
    /* The exit (0.13.0): each step timed. */
    const char *exit_mode = SDL_getenv("BTBH_STAGE_BENCH_EXIT");
    if (exit_mode == NULL) {
        exit_mode = "";
    }
    ExitExtra extra;
    memset(&extra, 0, sizeof(extra));
    extra.count = (uint32_t)env_int("BTBH_STAGE_BENCH_EXIT_EXTRA", 0, 0, 100000);
    uint64_t started = SDL_GetTicksNS();
    if (extra.count > 0u) {
        extra.textures = calloc(extra.count, sizeof(*extra.textures));
        extra.meshes = calloc(extra.count, sizeof(*extra.meshes));
        extra.buffers = calloc(extra.count, sizeof(*extra.buffers));
        if (extra.textures == NULL || extra.meshes == NULL || extra.buffers == NULL) {
            fail("extra", "out of memory");
        }
        float instances[8u * BENCH_FLOATS];
        memset(instances, 0, sizeof(instances));
        for (uint32_t index = 0u; index < extra.count; ++index) {
            extra.textures[index] = noise_texture(window, 256u, 9000u + index, BTBH_GPU_TEXTURE_COLOR_SPACE_SRGB);
            extra.meshes[index] = make_piece(window, index % BENCH_PIECES, 77u + index);
            BTBHGPUInstanceBufferReport buffer_report;
            memset(&buffer_report, 0, sizeof(buffer_report));
            extra.buffers[index] = btbh_gpu_window_instance_buffer_create(window, 8u,
                BENCH_FLOATS * (uint32_t)sizeof(float), &buffer_report);
            if (extra.buffers[index] == NULL ||
                btbh_gpu_instance_buffer_upload(extra.buffers[index], 0u, instances, (uint32_t)sizeof(instances),
                    &buffer_report) != BTBH_GPU_OK) {
                fail("extra instance buffer", buffer_report.message);
            }
        }
    }
    const double extra_ms = ms_since(started);
    started = SDL_GetTicksNS();
    if (mode_has(exit_mode, "idle")) {
        (void)SDL_WaitForGPUIdle(device->gpu);
    }
    const double idle_ms = ms_since(started);
    started = SDL_GetTicksNS();
    if (!mode_has(exit_mode, "nodestroy")) {
        destroy_stage(&stages[current]);
        for (uint32_t index = 0u; index < kept_count; ++index) {
            destroy_stage(&kept[index]);
        }
        for (uint32_t index = 0u; index < extra.count; ++index) {
            btbh_gpu_texture_destroy(extra.textures[index]);
            btbh_gpu_custom_mesh_destroy(extra.meshes[index]);
            btbh_gpu_instance_buffer_destroy(extra.buffers[index]);
        }
    }
    const double destroy_ms = ms_since(started);
    started = SDL_GetTicksNS();
    if (mode_has(exit_mode, "flush")) {
        (void)canvas_only(window);
    }
    const double flush_ms = ms_since(started);
    started = SDL_GetTicksNS();
    btbh_gpu_window_destroy(window);
    const double window_ms = ms_since(started);
    started = SDL_GetTicksNS();
    /* (The staging pool's release waits for the GPU when the device has one; timed on its own.) */
    btbh_gpu_staging_release_device(device->gpu);
    const double staging_ms = ms_since(started);
    started = SDL_GetTicksNS();
    btbh_gpu_device_shutdown(device);
    const double device_ms = ms_since(started);
    btbh_gpu_device_destroy(device);
    if (mode_has(exit_mode, "nodestroy")) {
        for (uint32_t index = 0u; index < kept_count; ++index) {
            free(kept[index].instances);
            free(kept[index].batches);
        }
        free(stages[current].instances);
        free(stages[current].batches);
    }
    free(extra.textures);
    free(extra.meshes);
    free(extra.buffers);
    printf("STAGE exit mode=%s extra=%u made=%.3f idle=%.3f destroy=%.3f flush=%.3f window=%.3f staging=%.3f "
        "device=%.3f total=%.3f\n", exit_mode, extra.count, extra_ms, idle_ms, destroy_ms, flush_ms, window_ms,
        staging_ms, device_ms, idle_ms + destroy_ms + flush_ms + window_ms + staging_ms + device_ms);
    return 0;
}
