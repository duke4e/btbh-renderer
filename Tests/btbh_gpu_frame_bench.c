/* A frame like Be The Bullet Hell v2's city at its Steam Deck preset, for measuring what a change costs the opaque
 * pass (RFC 0009 phase 2's follow-up: the static draws without a world-detail table must cost what 0.7.0's did).
 *
 * About 2,000 draws of instanced kit pieces (a wall slab with a grid of window frames, sills and ledges: 8 to 40
 * small boxes, 96 to 480 triangles a piece, 1 to 5 instances a draw), ~2.2 million triangles at 960x600 under a
 * soft sun shadow and 8 lamps, upscaled with FSR 1 to 1280x800, HBAO and FXAA; pattern 7 on the pieces. The GPU's
 * time comes from the core's cost log (BTBH_GPU_COST_LOG=1, set here): about every other frame is submitted in
 * pieces, and main_ms is the opaque pass alone.
 *
 * It uses only the public API, so it builds against an older release's header and library too (0.7.0's: compile
 * this file with that checkout's include directory and link its btbh_gpu library), which compares the cores.
 *
 * Environment:
 * - BTBH_FRAME_BENCH_DIRS: shader directories separated by '|', one hidden window each (default: the build's);
 *   their turns are interleaved, each round in another order.
 * - BTBH_FRAME_BENCH_ROUNDS (default 30), BTBH_FRAME_BENCH_FRAMES (frames a turn, default 8),
 *   BTBH_FRAME_BENCH_DRAWS (default 2000).
 * - BTBH_FRAME_BENCH_MODES (0.8.0 and later): one for every window, or one for all, separated by '|': none (the
 *   default), table (every piece's draw has a world-detail table of 8 entries over 16 512^2 layers; the slabs and
 *   ledges on entries 0..7, the frames and sills on 255, as v2's merged models carry their metal and frames),
 *   table-all (the frames on entry 3 too), table-off (a table without map_layers).
 *
 * Output: "BENCH window <n> <directory> ..." per window, "BENCH turn <window> <round>" before each turn, then the
 * core's "BTBH_GPU_COST gpu_ms=... split=1 ... main_ms=..." lines (split=0: the whole frame). tools/
 * frame_bench_summary.py pairs each window's turns with the first window's in the same round, skips each turn's
 * first frames and drops rounds another process slowed. With no device it is skipped (77). */
#include "btbh_gpu/btbh_gpu.h"

#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_stdinc.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BENCH_SKIP 77
#define BENCH_WIDTH 1280u
#define BENCH_HEIGHT 800u
#define BENCH_RENDER_WIDTH 960u
#define BENCH_RENDER_HEIGHT 600u
#define BENCH_PIECES 32u
#define BENCH_MAX_WINDOWS 6u
#define BENCH_FLOATS 24u

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

typedef struct BenchWindow {
    BTBHGPUWindow *window;
    char mode[16];
    char scene_shaders[11][1024];
    char canvas_shaders[3][1024];
    BTBHGPUCustomMesh *pieces[BENCH_PIECES];
    BTBHGPUCustomMesh *ground;
    BTBHGPUTexture *albedo;
    BTBHGPUTexture *normal;
    BTBHGPUTexture *orm;
#if defined(BTBH_GPU_SCENE_HAS_WORLD_DETAIL_TABLE)
    BTBHGPUTexture *maps;
    BTBHGPUSceneWorldDetailTable table;
    BTBHGPUSceneWorldDetailTableEntry entries[8];
#endif
    float *instances;
    uint32_t instance_count;
    BTBHGPUSceneGeometryBatch *batches;
    uint32_t batch_count;
    uint64_t triangles;
} BenchWindow;

static uint32_t bench_random(uint32_t *state) {
    *state = *state * 1664525u + 1013904223u;
    return *state >> 8u;
}

static float bench_unit(uint32_t *state) {
    return (float)(bench_random(state) & 0xffffu) / 65535.0f;
}

static int bench_env_int(const char *name, int fallback, int low, int high) {
    const char *text = SDL_getenv(name);
    const int value = text != NULL && text[0] != 0 ? atoi(text) : fallback;
    return value < low ? low : (value > high ? high : value);
}

/* The `index`th of a '|'-separated list (the last one past its end), or `fallback`. */
static void bench_list_item(const char *list, uint32_t index, const char *fallback, char *out, size_t capacity) {
    if (list == NULL || list[0] == 0) {
        (void)snprintf(out, capacity, "%s", fallback);
        return;
    }
    const char *at = list;
    for (uint32_t skipped = 0u; skipped < index; ++skipped) {
        const char *bar = strchr(at, '|');
        if (bar == NULL) {
            break;
        }
        at = bar + 1;
    }
    const char *end = strchr(at, '|');
    const size_t length = end != NULL ? (size_t)(end - at) : strlen(at);
    (void)snprintf(out, capacity, "%.*s", (int)length, at);
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

typedef struct BenchCamera {
    float view_projection[16];
    float right[3];
    float up[3];
    float position[3];
} BenchCamera;

/* Column-major, right-handed, depth 0..1 (the renderer's convention). */
static BenchCamera look_at(const float eye[3], const float centre[3], float fov_y, float aspect) {
    BenchCamera camera;
    memset(&camera, 0, sizeof(camera));
    const float near_plane = 1.0f;
    const float far_plane = 400.0f;
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

/* An axis-aligned box (centre, half sizes), 24 vertices and 36 indices, its outsides front faces; every vertex's
   joints.x (BTBH_GPU_CUSTOM_MESH_DETAIL_INDEX_FLOAT from 0.8.0) is `index`. */
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

static uint32_t piece_columns(uint32_t piece) {
    return 2u + piece % 4u;
}

static uint32_t piece_rows(uint32_t piece) {
    return 2u + (piece / 4u) % 5u;
}

static uint32_t piece_boxes(uint32_t piece) {
    return 1u + piece_columns(piece) * piece_rows(piece) * 2u + piece_rows(piece);
}

/* Kit piece p: a wall slab and its ledges on class p % 8, a frame and a sill per window on `frame_index`. */
static BTBHGPUCustomMesh *make_piece(BTBHGPUWindow *window, uint32_t piece, float frame_index) {
    const uint32_t columns = piece_columns(piece);
    const uint32_t rows = piece_rows(piece);
    const uint32_t boxes = piece_boxes(piece);
    float *vertices = calloc((size_t)boxes * 24u * BENCH_FLOATS, sizeof(float));
    uint32_t *indices = calloc((size_t)boxes * 36u, sizeof(uint32_t));
    if (vertices == NULL || indices == NULL) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    uint32_t vertex_count = 0u;
    uint32_t index_count = 0u;
    const float width = 2.0f;
    const float height = 3.0f;
    const float wall_index = (float)(piece % 8u);
    const float slab_centre[3] = { 0.0f, 0.5f * height, 0.0f };
    const float slab_half[3] = { 0.5f * width, 0.5f * height, 0.15f };
    add_box(vertices, &vertex_count, indices, &index_count, slab_centre, slab_half, wall_index);
    for (uint32_t row = 0u; row < rows; ++row) {
        const float y = height * ((float)row + 0.5f) / (float)rows;
        for (uint32_t column = 0u; column < columns; ++column) {
            const float x = -0.5f * width + width * ((float)column + 0.5f) / (float)columns;
            const float frame_centre[3] = { x, y, 0.17f };
            const float frame_half[3] = { 0.3f * width / (float)columns, 0.3f * height / (float)rows, 0.03f };
            add_box(vertices, &vertex_count, indices, &index_count, frame_centre, frame_half, frame_index);
            const float sill_centre[3] = { x, y - 0.33f * height / (float)rows, 0.2f };
            const float sill_half[3] = { 0.34f * width / (float)columns, 0.02f, 0.06f };
            add_box(vertices, &vertex_count, indices, &index_count, sill_centre, sill_half, frame_index);
        }
        const float ledge_centre[3] = { 0.0f, y + 0.45f * height / (float)rows, 0.2f };
        const float ledge_half[3] = { 0.5f * width, 0.03f, 0.05f };
        add_box(vertices, &vertex_count, indices, &index_count, ledge_centre, ledge_half, wall_index);
    }
    BTBHGPUCustomMeshReport report;
    memset(&report, 0, sizeof(report));
    BTBHGPUCustomMesh *mesh = btbh_gpu_window_custom_mesh_create(window, vertices,
        vertex_count * BENCH_FLOATS * (uint32_t)sizeof(float), vertex_count, indices, index_count, &report);
    free(vertices);
    free(indices);
    if (mesh == NULL) {
        fprintf(stderr, "kit piece: %s\n", report.message);
        exit(1);
    }
    return mesh;
}

static uint8_t *noise_pixels(uint32_t size, uint32_t seed) {
    uint8_t *pixels = malloc((size_t)size * size * 4u);
    if (pixels == NULL) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    uint32_t state = seed;
    for (size_t i = 0u; i < (size_t)size * size * 4u; ++i) {
        pixels[i] = (i % 4u) == 3u ? 255u : (uint8_t)(96u + bench_random(&state) % 128u);
    }
    return pixels;
}

static BTBHGPUTexture *noise_texture(BTBHGPUWindow *window, uint32_t seed, BTBHGPUTextureColorSpace color_space) {
    uint8_t *pixels = noise_pixels(256u, seed);
    BTBHGPUTextureReport report;
    BTBHGPUTexture *texture = btbh_gpu_window_texture_create_rgba8_ex(window, pixels, 256u, 256u, 1u,
        BTBH_GPU_TEXTURE_FILTER_TRILINEAR, BTBH_GPU_TEXTURE_WRAP_REPEAT, color_space, &report);
    free(pixels);
    if (texture == NULL) {
        fprintf(stderr, "texture: %s\n", report.message);
        exit(1);
    }
    return texture;
}

static void fill_batch(BTBHGPUSceneGeometryBatch *batch, BTBHGPUCustomMesh *mesh, uint32_t first, uint32_t count) {
    memset(batch, 0, sizeof(*batch));
    batch->kind = BTBH_GPU_SCENE_GEOMETRY_CUSTOM_MESH;
    batch->custom_mesh = mesh;
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

/* The draws: kit piece (draw % 32) with 1..5 instances on the lots of a 64 x 48 city, then the ground. */
static void build_frame(BenchWindow *bench, uint32_t draws) {
    const char *mode = bench->mode;
    uint32_t state = 12345u;
    bench->instances = calloc(((size_t)draws * 5u + 1u) * BENCH_FLOATS, sizeof(float));
    bench->batches = calloc((size_t)draws + 1u, sizeof(BTBHGPUSceneGeometryBatch));
    if (bench->instances == NULL || bench->batches == NULL) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    for (uint32_t draw = 0u; draw < draws; ++draw) {
        const uint32_t piece = draw % BENCH_PIECES;
        const uint32_t count = 1u + bench_random(&state) % 5u;
        BTBHGPUSceneGeometryBatch *batch = &bench->batches[bench->batch_count++];
        fill_batch(batch, bench->pieces[piece], bench->instance_count, count);
        batch->albedo_texture = bench->albedo;
        batch->normal_texture = bench->normal;
        batch->orm_texture = bench->orm;
        batch->detail_parameters[3] = 7.0f;   /* the facade's weather (0.7.0) */
#if defined(BTBH_GPU_SCENE_HAS_WORLD_DETAIL_TABLE)
        if (strncmp(mode, "table", 5) == 0) {
            batch->world_detail_table = &bench->table;
        }
#else
        (void)mode;
#endif
        for (uint32_t instance_index = 0u; instance_index < count; ++instance_index) {
            const uint32_t lot = bench_random(&state) % (64u * 48u);
            const float x = -96.0f + 3.0f * (float)(lot % 64u) + bench_unit(&state);
            const float z = -72.0f + 3.0f * (float)(lot / 64u) + bench_unit(&state);
            const float y = 3.0f * (float)(bench_random(&state) % 4u);
            const float yaw = 1.5707963f * (float)(bench_random(&state) % 4u);
            float *instance = &bench->instances[(size_t)bench->instance_count++ * BENCH_FLOATS];
            instance[0] = cosf(yaw);
            instance[2] = -sinf(yaw);
            instance[5] = 1.0f;
            instance[8] = sinf(yaw);
            instance[10] = cosf(yaw);
            instance[12] = x;
            instance[13] = y;
            instance[14] = z;
            instance[15] = 1.0f;
            instance[16] = 0.6f + 0.4f * bench_unit(&state);
            instance[17] = 0.6f + 0.4f * bench_unit(&state);
            instance[18] = 0.6f + 0.4f * bench_unit(&state);
            instance[19] = 1.0f;
            instance[23] = (float)(lot % 97u);   /* the lot's seed in custom.w */
            bench->triangles += (uint64_t)piece_boxes(piece) * 12u;
        }
    }
    BTBHGPUSceneGeometryBatch *ground = &bench->batches[bench->batch_count++];
    fill_batch(ground, bench->ground, bench->instance_count, 1u);
    float *instance = &bench->instances[(size_t)bench->instance_count++ * BENCH_FLOATS];
    instance[0] = 1.0f;
    instance[5] = 1.0f;
    instance[10] = 1.0f;
    instance[15] = 1.0f;
    instance[16] = 0.45f;
    instance[17] = 0.45f;
    instance[18] = 0.42f;
    instance[19] = 1.0f;
    bench->triangles += 12u;
}

static void open_bench_window(BenchWindow *bench, BTBHGPUDevice *device, const char *directory, uint32_t number,
    uint32_t draws) {
    memset(bench, 0, sizeof(*bench));
    bench_list_item(SDL_getenv("BTBH_FRAME_BENCH_MODES"), number, "none", bench->mode, sizeof(bench->mode));
    char title[64];
    (void)snprintf(title, sizeof(title), "BTBH frame bench %u", number);
    const BTBHGPUWindowDescriptor descriptor = {
        .title = title,
        .width = BENCH_WIDTH,
        .height = BENCH_HEIGHT,
        .hidden = 1,
        .high_pixel_density = 0,
    };
    BTBHGPUWindowReport report;
    memset(&report, 0, sizeof(report));
    bench->window = btbh_gpu_window_create(&descriptor, &report);
    if (bench->window == NULL || btbh_gpu_window_claim(device, bench->window, &report) != BTBH_GPU_OK) {
        fprintf(stderr, "window: %s\n", report.message);
        exit(1);
    }
    for (uint32_t index = 0u; index < 11u; ++index) {
        (void)snprintf(bench->scene_shaders[index], sizeof(bench->scene_shaders[index]), "%s/%s%s", directory,
            scene_shader_names[index], BENCH_SHADER_EXTENSION);
    }
    for (uint32_t index = 0u; index < 3u; ++index) {
        (void)snprintf(bench->canvas_shaders[index], sizeof(bench->canvas_shaders[index]), "%s/%s%s", directory,
            canvas_shader_names[index], BENCH_SHADER_EXTENSION);
    }
    const char *mode = bench->mode;
    const float frame_index = strcmp(mode, "table-all") == 0 ? 3.0f : 255.0f;
    for (uint32_t piece = 0u; piece < BENCH_PIECES; ++piece) {
        bench->pieces[piece] = make_piece(bench->window, piece, frame_index);
    }
    {
        float vertices[24u * BENCH_FLOATS];
        uint32_t indices[36];
        uint32_t vertex_count = 0u;
        uint32_t index_count = 0u;
        const float centre[3] = { 0.0f, -0.5f, 0.0f };
        const float half[3] = { 200.0f, 0.5f, 200.0f };
        add_box(vertices, &vertex_count, indices, &index_count, centre, half, 0.0f);
        BTBHGPUCustomMeshReport mesh_report;
        memset(&mesh_report, 0, sizeof(mesh_report));
        bench->ground = btbh_gpu_window_custom_mesh_create(bench->window, vertices, (uint32_t)sizeof(vertices),
            vertex_count, indices, index_count, &mesh_report);
        if (bench->ground == NULL) {
            fprintf(stderr, "ground: %s\n", mesh_report.message);
            exit(1);
        }
    }
    bench->albedo = noise_texture(bench->window, 11u, BTBH_GPU_TEXTURE_COLOR_SPACE_SRGB);
    bench->normal = noise_texture(bench->window, 12u, BTBH_GPU_TEXTURE_COLOR_SPACE_LINEAR);
    bench->orm = noise_texture(bench->window, 13u, BTBH_GPU_TEXTURE_COLOR_SPACE_LINEAR);
#if defined(BTBH_GPU_SCENE_HAS_WORLD_DETAIL_TABLE)
    if (strncmp(mode, "table", 5) == 0) {
        /* v2's macro maps: class k's normal at layer 2k, its ORM at 2k + 1, 512^2 with mips. */
        uint8_t *layers[16];
        for (uint32_t layer = 0u; layer < 16u; ++layer) {
            layers[layer] = noise_pixels(512u, 100u + layer);
        }
        BTBHGPUTextureReport texture_report;
        bench->maps = btbh_gpu_window_texture_array_create_rgba8_ex(bench->window, (const uint8_t *const *)layers,
            512u, 512u, 16u, 1u, BTBH_GPU_TEXTURE_FILTER_TRILINEAR, BTBH_GPU_TEXTURE_WRAP_REPEAT,
            BTBH_GPU_TEXTURE_COLOR_SPACE_LINEAR, &texture_report);
        for (uint32_t layer = 0u; layer < 16u; ++layer) {
            free(layers[layer]);
        }
        if (bench->maps == NULL) {
            fprintf(stderr, "array: %s\n", texture_report.message);
            exit(1);
        }
        for (uint32_t entry = 0u; entry < 8u; ++entry) {
            BTBHGPUSceneWorldDetailTableEntry *e = &bench->entries[entry];
            memset(e, 0, sizeof(*e));
            e->level_tile[0] = 1.5f + 0.25f * (float)entry;
            e->level_tile[1] = e->level_tile[0];
            e->wall_tile[0] = 1.0f + 0.2f * (float)entry;
            e->wall_tile[1] = e->wall_tile[0];
            e->normal_layer = (uint8_t)(2u * entry);
            e->orm_layer = (uint8_t)(2u * entry + 1u);
        }
        bench->table.map_layers = strcmp(mode, "table-off") == 0 ? NULL : bench->maps;
        bench->table.entries = bench->entries;
        bench->table.entry_count = 8u;
        bench->table.seed_component = 4u;
    }
#endif
    build_frame(bench, draws);
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

static void run_frame(BenchWindow *bench, const BenchCamera *camera, const BTBHGPUSceneLighting *lighting,
    const BTBHGPUScenePost *post) {
    BTBHGPUSceneReport report;
    const BTBHGPUResult presented = btbh_gpu_window_scene_present_instances(bench->window,
        bench->scene_shaders[0], bench->scene_shaders[1], bench->scene_shaders[2], bench->scene_shaders[3],
        bench->scene_shaders[4], bench->scene_shaders[5], bench->scene_shaders[6], bench->scene_shaders[7],
        bench->scene_shaders[8], bench->scene_shaders[9], bench->scene_shaders[10],
        BENCH_SHADER_FORMAT, BENCH_RENDER_WIDTH, BENCH_RENDER_HEIGHT,
        camera->view_projection, camera->right, camera->up, camera->position, lighting, post,
        bench->instances, bench->instance_count, bench->instance_count, bench->batches, bench->batch_count,
        90u, 110u, 140u, 255u, &report);
    if (presented != BTBH_GPU_OK) {
        fprintf(stderr, "present: %s\n", report.message);
        exit(1);
    }
    /* The canvas submits the frame (and the cost log times it). */
    BTBHGPUCanvasPass pass;
    memset(&pass, 0, sizeof(pass));
    BTBHGPUCanvasReport canvas_report;
    if (btbh_gpu_window_canvas_present(bench->window, bench->canvas_shaders[0], bench->canvas_shaders[1],
            bench->canvas_shaders[2], BENCH_SHADER_FORMAT, NULL, 0u, NULL, 0u, &pass, 1u, &canvas_report) !=
        BTBH_GPU_OK) {
        fprintf(stderr, "canvas: %s\n", canvas_report.message);
        exit(1);
    }
}

int main(void) {
    (void)SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    (void)SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "BTBH_GPU_COST_LOG", "1", true);
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
    const char *list = SDL_getenv("BTBH_FRAME_BENCH_DIRS");
    if (list == NULL || list[0] == 0) {
        list = BTBH_GPU_SMOKE_SHADER_DIR;
    }
    const int rounds = bench_env_int("BTBH_FRAME_BENCH_ROUNDS", 30, 1, 1000);
    const int frames = bench_env_int("BTBH_FRAME_BENCH_FRAMES", 8, 1, 1000);
    const uint32_t draws = (uint32_t)bench_env_int("BTBH_FRAME_BENCH_DRAWS", 2000, 1, 20000);
    static BenchWindow windows[BENCH_MAX_WINDOWS];
    uint32_t window_count = 0u;
    for (const char *at = list; *at != 0 && window_count < BENCH_MAX_WINDOWS;) {
        const char *end = strchr(at, '|');
        const size_t length = end != NULL ? (size_t)(end - at) : strlen(at);
        char directory[1024];
        (void)snprintf(directory, sizeof(directory), "%.*s", (int)length, at);
        open_bench_window(&windows[window_count], device, directory, window_count, draws);
        printf("BENCH window %u %s mode=%s draws=%u instances=%u triangles=%llu\n", window_count, directory,
            windows[window_count].mode, windows[window_count].batch_count, windows[window_count].instance_count,
            (unsigned long long)windows[window_count].triangles);
        window_count += 1u;
        at = end != NULL ? end + 1 : at + length;
    }
    const float eye[3] = { 0.0f, 95.0f, 95.0f };
    const float centre[3] = { 0.0f, 0.0f, 0.0f };
    const BenchCamera camera = look_at(eye, centre, 0.9f, (float)BENCH_WIDTH / (float)BENCH_HEIGHT);
    const BTBHGPUSceneLighting lighting = bench_lighting();
    const BTBHGPUScenePost post = bench_post();
    /* Pipelines, caches and clocks first. */
    for (uint32_t w = 0u; w < window_count; ++w) {
        for (int frame = 0; frame < 10; ++frame) {
            run_frame(&windows[w], &camera, &lighting, &post);
        }
    }
    uint32_t order_state = 777u;
    for (int round = 0; round < rounds; ++round) {
        uint32_t order[BENCH_MAX_WINDOWS];
        for (uint32_t w = 0u; w < window_count; ++w) {
            order[w] = w;
        }
        for (uint32_t w = window_count; w > 1u; --w) {
            const uint32_t pick = bench_random(&order_state) % w;
            const uint32_t swap = order[w - 1u];
            order[w - 1u] = order[pick];
            order[pick] = swap;
        }
        for (uint32_t position = 0u; position < window_count; ++position) {
            printf("BENCH turn %u %d\n", order[position], round);
            for (int frame = 0; frame < frames; ++frame) {
                run_frame(&windows[order[position]], &camera, &lighting, &post);
            }
        }
    }
    printf("BENCH done\n");
    (void)fflush(stdout);
    for (uint32_t w = 0u; w < window_count; ++w) {
        free(windows[w].instances);
        free(windows[w].batches);
        btbh_gpu_window_destroy(windows[w].window);
    }
    btbh_gpu_device_shutdown(device);
    btbh_gpu_device_destroy(device);
    return 0;
}
