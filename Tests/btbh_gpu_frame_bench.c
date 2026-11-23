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
 *   table-all (the frames on entry 3 too), table-none (every triangle on 255), table-off (a table without
 *   map_layers).
 * - RFC 0011's ring (0.10.0's bench; it builds against 0.9.0 too): a mode "ring" with '+' options adds, after the
 *   frame's draws and its ground, Be The Bullet Hell v2's outskirts ring as it measured it: 257 batches, a ground and
 *   128 building pieces, each a wall and its glass, every one a distinct custom mesh (24 to 144 vertices), in 8
 *   chunks of 16 pieces with a persistent instance buffer each (1 to 3 instances a piece), no sun shadow of their
 *   own. Options: +table (world detail on: the frame's draws and the ring's walls take the table of mode table, so
 *   the walls' static vertex shader differs from the glass's), +sorted (each chunk's walls, then its glass;
 *   otherwise a wall, its glass, the next wall: interleaved), +arena (the ring's meshes in the shared geometry
 *   buffers, BTBH_GPU_CUSTOM_MESH_SHARED_BUFFERS; ignored before 0.10.0), +noruns (BTBH_GPU_SCENE_RUNS=0 around this
 *   window's presents; nothing before 0.10.0), +one (every ring batch draws the first wall's mesh: v2's "one shared
 *   mesh").
 *   Every frame of a ring window alternates with the ring and without it (the frame's draws alone). The other modes
 *   take +noruns too, and +arena for the 32 kit pieces (table+noruns, table+arena), for the GPU's time.
 * - The radiance cascades (0.10.0's bench; it builds against 0.9.0 too): a mode with +rc turns them on as Be The
 *   Bullet Hell v2 runs them at night: additive, tier High over 256^2 texels of half a unit (+low: tier Low over 128^2
 *   of a unit), an occlusion field of 2 x 2 unit blocks on the 3 unit lots (0.385 to 3.535 high, v2's way: 0 inside,
 *   the distance to the nearest inside texel outside), every 10th draw glowing (a material source) and 32 disk
 *   emitters at 0.55. +tops sets RFC 0010's source_tops (emitters' top_above 0; ignored before 0.10.0). The cost
 *   log's rc_src_ms, rc_field_ms and rc_screen_ms time the cascades' passes (frame_bench_summary.py --group).
 * - RFC 0012's lift (0.11.0's bench): lift (every piece's draw lifted by a heightfield of 193 x 145 samples, one a
 *   unit, over the city: hills up to about 3 units), lift-keep (the same with KEEP_NORMALS), lift-flat (the same
 *   draws without a lift). In all three the pieces cast no shadow (a lifted batch casts none), so lift - lift-flat is
 *   what the lift costs. Before 0.11.0 lift and lift-keep draw as lift-flat.
 * - RFC 0013's smoke (0.11.0's bench): smoke-off, smoke-soft and smoke-lit add Be The Bullet Hell v2's burning fight to the
 *   frame: 170 puffs of a 2 x 2 atlas of 128-pixel cells (5 columns of 12 over fires, 110 low ones scattered), sorted
 *   far to near, straight alpha capped at 0.5, their discs about 5.6 % of the screen's height (printed: the share of
 *   the screen they cover, the deepest stack, the lights in reach of a puff), by 6 fire and 4 neon point lights and
 *   16 lamps (the frame's 8 and a second row: v2's High preset at night).
 *   smoke-soft draws them as v2 does before RFC 0013 (soft particles, unlit, a batch per cell and tilt: 10 draws),
 *   smoke-lit as one lit particle batch with the UV rect, a normal and thickness atlas, the soft fade and the
 *   transmission; smoke-off draws none (the same lights). The cost log's transparent_ms holds the distance pass and
 *   the puffs. Before 0.11.0 smoke-lit draws as smoke-off.
 * - RFC 0013 phase 2's particle shadow (0.14.0's bench): smoke-shadow is smoke-lit with the batch's
 *   BTBH_GPU_SCENE_BATCH_PARTICLE_SHADOW and particle_shadow 1 (the map pass, in shadow_ms, and the lit particles'
 *   read, in transparent_ms); smoke-ground with particle_shadow 2 (the ground's read too, in main_ms); +quarter the map
 *   at a quarter of the sun map's size instead of half. All 170 puffs draw into the map. Before 0.14.0 both draw as
 *   smoke-lit.
 * - BTBH_FRAME_BENCH_PRESET=high (0.14.0's bench): 1920 x 1080, rendered at that size (no upscaling), instead of the
 *   Deck preset's 960 x 600 upscaled to 1280 x 800. BTBH_FRAME_BENCH_SUN_MAP=<size>: the sun map's size (256 to 8192;
 *   default the renderer's 2048).
 * - The light factor (0.11.0's bench): light-off, light and light-mixed light the frame's draws as Be The Bullet Hell
 *   v2's lit kit at night: every piece's draw glows (emission 1.5) and every fourth is glass (pattern 5, half its
 *   panes lit, under window_glow 1.5) instead of pattern 7. light flags every draw with
 *   BTBH_GPU_SCENE_BATCH_INSTANCE_LIGHT_FACTOR at custom x 1 (the same pixels as light-off: light - light-off is what
 *   the flag costs); light-mixed at 1, 0.5 and 0 by instance (a district going dark). Before 0.11.0 both draw as
 *   light-off.
 * - BTBH_FRAME_BENCH_CPU=1: no cost log; after each present a line "BENCH cpu <window> <round> ring=<0|1>
 *   prepare=<ms> shadow=<ms> geometry=<ms> present=<ms> draws=<n>" with the scene report's CPU fields and the
 *   present call's wall clock (tools/frame_bench_cpu.py summarises them).
 *
 * Output: "BENCH window <n> <directory> ..." per window, "BENCH turn <window> <round>" before each turn, then the
 * core's "BTBH_GPU_COST gpu_ms=... split=1 ... main_ms=..." lines (split=0: the whole frame). tools/
 * frame_bench_summary.py pairs each window's turns with the first window's in the same round, skips each turn's
 * first frames and drops rounds another process slowed. With no device it is skipped (77). */
#include "btbh_gpu/btbh_gpu.h"

#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BENCH_SKIP 77
/* The Deck preset's sizes; BTBH_FRAME_BENCH_PRESET=high takes 1920 x 1080 for both. */
static uint32_t BENCH_WIDTH = 1280u;
static uint32_t BENCH_HEIGHT = 800u;
static uint32_t BENCH_RENDER_WIDTH = 960u;
static uint32_t BENCH_RENDER_HEIGHT = 600u;
#define BENCH_MAX_RENDER_PIXELS (1920u * 1080u)
#define BENCH_PIECES 32u
#define BENCH_MAX_WINDOWS 10u
#define BENCH_FLOATS 24u
#define RING_CHUNKS 8u
#define RING_CHUNK_PIECES 16u
#define RING_PIECES (RING_CHUNKS * RING_CHUNK_PIECES)
#define RING_MESHES (2u * RING_PIECES + 1u)
#define RING_CHUNK_INSTANCES (3u * RING_CHUNK_PIECES)
#define BENCH_RC_EMITTERS 32u
#define BENCH_RC_FIELD 256u

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

typedef struct BenchRing {
    bool enabled;
    bool table;
    bool sorted;
    bool arena;
    bool runs_off;
    bool one;
    BTBHGPUCustomMesh *meshes[RING_MESHES];   /* walls, then glass, then the ground */
    BTBHGPUInstanceBuffer *chunks[RING_CHUNKS + 1u];
    uint32_t batch_count;
} BenchRing;

#define SMOKE_PUFFS 170u
#define SMOKE_COLUMNS 5u
#define SMOKE_COLUMN_PUFFS 12u
#define SMOKE_LIGHTS 6u
#define SMOKE_NEON 4u

typedef struct BenchWindow {
    BTBHGPUWindow *window;
    char mode[48];
    /* RFC 0013's smoke: 0 none, 1 smoke-off, 2 smoke-soft, 3 smoke-lit, 4 smoke-shadow, 5 smoke-ground. */
    int smoke;
    /* RFC 0013 phase 2: +quarter, the particle transmittance map at a quarter of the sun map's size. */
    bool smoke_quarter;
    /* The light factor: 0 none, 1 light-off, 2 light, 3 light-mixed. */
    int light;
    BTBHGPUTexture *smoke_atlas;
    BTBHGPUTexture *smoke_normals;
    BTBHGPUCustomMesh *puff;
    BenchRing ring;
    bool rc;
    bool rc_low;
    bool rc_tops;
    float *occlusion;
    BTBHGPUSceneRadianceEmitter emitters[BENCH_RC_EMITTERS];
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
#if defined(BTBH_GPU_SCENE_HAS_HEIGHT_LIFT)
    BTBHGPUTexture *field;
    BTBHGPUSceneHeightLift lift;
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

static BTBHGPUCustomMesh *bench_mesh(BTBHGPUWindow *window, const float *vertices, uint32_t vertex_count,
    const uint32_t *indices, uint32_t index_count, bool arena);

/* Kit piece p: a wall slab and its ledges on class p % 8 (or on `frame_index` with `all_frame`), a frame and a sill
   per window on `frame_index`; `arena`: in the shared geometry buffers (from 0.10.0). */
static BTBHGPUCustomMesh *make_piece(BTBHGPUWindow *window, uint32_t piece, float frame_index, bool all_frame,
    bool arena) {
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
    const float wall_index = all_frame ? frame_index : (float)(piece % 8u);
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
    BTBHGPUCustomMesh *mesh = bench_mesh(window, vertices, vertex_count, indices, index_count, arena);
    free(vertices);
    free(indices);
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
    bench->instances = calloc(((size_t)draws * 5u + 1u + SMOKE_PUFFS) * BENCH_FLOATS, sizeof(float));
    bench->batches = calloc((size_t)draws + 1u + 2u * RING_PIECES + 1u + 16u, sizeof(BTBHGPUSceneGeometryBatch));
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
        if (bench->light != 0) {
            /* v2's lit kit at night: a glow on every piece, glass on every fourth. */
            batch->emission_red = 255u;
            batch->emission_green = 190u;
            batch->emission_blue = 120u;
            batch->emission_energy = 1.5f;
            if (draw % 4u == 0u) {
                batch->detail_parameters[1] = 0.5f;
                batch->detail_parameters[2] = 3.0f;
                batch->detail_parameters[3] = 5.0f;
            }
#if defined(BTBH_GPU_SCENE_HAS_INSTANCE_LIGHT_FACTOR)
            if (bench->light >= 2) {
                batch->flags |= BTBH_GPU_SCENE_BATCH_INSTANCE_LIGHT_FACTOR;
            }
#endif
        }
        if (bench->rc && draw % 10u == 0u) {
            /* A glow: a material source of the cascades. */
            batch->emission_red = 255u;
            batch->emission_green = 190u;
            batch->emission_blue = 120u;
            batch->emission_energy = 2.0f;
        }
#if defined(BTBH_GPU_SCENE_HAS_WORLD_DETAIL_TABLE)
        if (strncmp(mode, "table", 5) == 0 || bench->ring.table) {
            batch->world_detail_table = &bench->table;
        }
#else
        (void)mode;
#endif
        if (strncmp(mode, "lift", 4) == 0) {
            /* (A lifted batch casts no shadow; lift-flat draws the same without the lift.) */
            batch->flags = BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW;
            batch->spot_shadow_visible = 0u;
#if defined(BTBH_GPU_SCENE_HAS_HEIGHT_LIFT)
            if (strncmp(mode, "lift-flat", 9) != 0) {
                batch->height_lift = &bench->lift;
            }
#endif
        }
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
            if (bench->light != 0) {
                /* The light factor in custom x: 1, or (light-mixed) 1, 0.5 and 0 by instance. */
                instance[20] = bench->light == 3 ? 0.5f * (float)(bench->instance_count % 3u) : 1.0f;
            }
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

/* Whether a '+'-separated mode has `option`. */
static bool mode_has(const char *mode, const char *option) {
    const size_t length = strlen(option);
    for (const char *at = strchr(mode, '+'); at != NULL; at = strchr(at + 1, '+')) {
        if (strncmp(at + 1, option, length) == 0 && (at[1 + length] == '+' || at[1 + length] == 0)) {
            return true;
        }
    }
    return false;
}

static BTBHGPUCustomMesh *bench_mesh(BTBHGPUWindow *window, const float *vertices, uint32_t vertex_count,
    const uint32_t *indices, uint32_t index_count, bool arena) {
    BTBHGPUCustomMeshReport report;
    memset(&report, 0, sizeof(report));
    BTBHGPUCustomMesh *mesh = NULL;
#if defined(BTBH_GPU_HAS_SHARED_MESH_BUFFERS)
    mesh = btbh_gpu_window_custom_mesh_create_ex(window, vertices, vertex_count * BENCH_FLOATS * (uint32_t)sizeof(float),
        vertex_count, indices, index_count, arena ? BTBH_GPU_CUSTOM_MESH_SHARED_BUFFERS : 0u, &report);
#else
    (void)arena;
    mesh = btbh_gpu_window_custom_mesh_create(window, vertices, vertex_count * BENCH_FLOATS * (uint32_t)sizeof(float),
        vertex_count, indices, index_count, &report);
#endif
    if (mesh == NULL) {
        fprintf(stderr, "mesh: %s\n", report.message);
        exit(1);
    }
    return mesh;
}

/* Ring mesh m: a wall (m < RING_PIECES: a slab and 0..5 ledges, entries 0..7), a glass piece (panes, on 255), or the
   ground (a thin slab). Each one distinct. */
static BTBHGPUCustomMesh *make_ring_mesh(BTBHGPUWindow *window, uint32_t m, bool arena) {
    float vertices[6u * 24u * BENCH_FLOATS];
    uint32_t indices[6u * 36u];
    uint32_t vertex_count = 0u;
    uint32_t index_count = 0u;
    uint32_t state = 4242u + m * 17u;
    if (m < RING_PIECES) {
        const float height = 1.5f + 0.25f * (float)(m % 7u);
        const float slab_centre[3] = { 0.0f, 0.5f * height, 0.0f };
        const float slab_half[3] = { 0.8f + 0.05f * (float)(m % 5u), 0.5f * height, 0.12f };
        add_box(vertices, &vertex_count, indices, &index_count, slab_centre, slab_half, (float)(m % 8u));
        const uint32_t ledges = m % 6u;
        for (uint32_t ledge = 0u; ledge < ledges; ++ledge) {
            const float y = height * ((float)ledge + 0.7f) / (float)(ledges + 1u);
            const float centre[3] = { 0.0f, y, 0.15f };
            const float half[3] = { slab_half[0], 0.03f + 0.01f * (float)(bench_random(&state) % 3u), 0.04f };
            add_box(vertices, &vertex_count, indices, &index_count, centre, half, (float)((m + ledge) % 8u));
        }
    } else if (m < 2u * RING_PIECES) {
        const uint32_t piece = m - RING_PIECES;
        const uint32_t panes = 1u + piece % 4u;
        for (uint32_t pane = 0u; pane < panes; ++pane) {
            const float x = -0.6f + 1.2f * ((float)pane + 0.5f) / (float)panes;
            const float centre[3] = { x, 0.9f + 0.1f * (float)(piece % 3u), 0.13f };
            const float half[3] = { 0.45f / (float)panes, 0.3f + 0.02f * (float)(piece % 5u), 0.01f };
            add_box(vertices, &vertex_count, indices, &index_count, centre, half, 255.0f);
        }
    } else {
        const float centre[3] = { 0.0f, -0.05f, 0.0f };
        const float half[3] = { 40.0f, 0.05f, 6.0f };
        add_box(vertices, &vertex_count, indices, &index_count, centre, half, 0.0f);
    }
    return bench_mesh(window, vertices, vertex_count, indices, index_count, arena);
}

static void ring_instance(float *instance, float x, float z, float yaw, float fade, float seed) {
    memset(instance, 0, BENCH_FLOATS * sizeof(float));
    instance[0] = cosf(yaw);
    instance[2] = -sinf(yaw);
    instance[5] = 1.0f;
    instance[8] = sinf(yaw);
    instance[10] = cosf(yaw);
    instance[12] = x;
    instance[14] = z;
    instance[15] = 1.0f;
    instance[16] = fade;
    instance[17] = fade;
    instance[18] = 0.95f * fade;
    instance[19] = 1.0f;
    instance[23] = seed;   /* the lot's seed in custom.w */
}

/* The ring: its meshes, its chunks' instance buffers (a ninth for the ground) and its batches, appended to the
   frame's. A band behind the city, 8 chunks of 16 pieces. */
static void build_ring(BenchWindow *bench) {
    BenchRing *ring = &bench->ring;
    for (uint32_t m = 0u; m < RING_MESHES; ++m) {
        ring->meshes[m] = make_ring_mesh(bench->window, m, ring->arena);
    }
    uint32_t state = 999u;
    uint32_t firsts[RING_PIECES];
    uint32_t counts[RING_PIECES];
    for (uint32_t chunk = 0u; chunk <= RING_CHUNKS; ++chunk) {
        float instances[RING_CHUNK_INSTANCES][BENCH_FLOATS];
        uint32_t count = 0u;
        if (chunk == RING_CHUNKS) {
            ring_instance(instances[count++], 0.0f, -84.0f, 0.0f, 0.8f, 0.0f);
        } else {
            for (uint32_t local = 0u; local < RING_CHUNK_PIECES; ++local) {
                const uint32_t piece = chunk * RING_CHUNK_PIECES + local;
                firsts[piece] = count;
                counts[piece] = 1u + bench_random(&state) % 3u;
                for (uint32_t copy = 0u; copy < counts[piece]; ++copy) {
                    const float x = -96.0f + 1.5f * (float)piece + 0.4f * (float)copy;
                    const float z = -80.0f - 3.0f * (float)copy - 0.5f * bench_unit(&state);
                    ring_instance(instances[count++], x, z, 0.0f, 0.55f + 0.3f * bench_unit(&state),
                        (float)(piece % 97u));
                }
            }
        }
        BTBHGPUInstanceBufferReport report;
        memset(&report, 0, sizeof(report));
        ring->chunks[chunk] = btbh_gpu_window_instance_buffer_create(bench->window, RING_CHUNK_INSTANCES,
            BENCH_FLOATS * (uint32_t)sizeof(float), &report);
        if (ring->chunks[chunk] == NULL ||
            btbh_gpu_instance_buffer_upload(ring->chunks[chunk], 0u, instances,
                count * BENCH_FLOATS * (uint32_t)sizeof(float), &report) != BTBH_GPU_OK) {
            fprintf(stderr, "ring instances: %s\n", report.message);
            exit(1);
        }
    }
    /* The walls' material (the city's textures, pattern 7, the table with +table) and the glass's. */
    BTBHGPUSceneGeometryBatch wall;
    fill_batch(&wall, NULL, 0u, 1u);
    wall.albedo_texture = bench->albedo;
    wall.normal_texture = bench->normal;
    wall.orm_texture = bench->orm;
    wall.detail_parameters[3] = 7.0f;
    wall.flags = BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW;
    wall.spot_shadow_visible = 0u;
#if defined(BTBH_GPU_SCENE_HAS_WORLD_DETAIL_TABLE)
    if (ring->table) {
        wall.world_detail_table = &bench->table;
    }
#endif
    BTBHGPUSceneGeometryBatch glass = wall;
    glass.albedo_texture = NULL;
    glass.normal_texture = NULL;
    glass.orm_texture = NULL;
#if defined(BTBH_GPU_SCENE_HAS_WORLD_DETAIL_TABLE)
    glass.world_detail_table = NULL;
#endif
    glass.albedo_red = 140u;
    glass.albedo_green = 170u;
    glass.albedo_blue = 200u;
    glass.roughness = 0.1f;
    glass.detail_parameters[2] = 3.0f;
    glass.detail_parameters[3] = 5.0f;
    BTBHGPUSceneGeometryBatch *out = &bench->batches[bench->batch_count];
    uint32_t added = 0u;
    BTBHGPUSceneGeometryBatch ground = glass;
    ground.custom_mesh = ring->meshes[RING_MESHES - 1u];
    ground.instance_buffer = ring->chunks[RING_CHUNKS];
    ground.first_instance = 0u;
    ground.instance_count = 1u;
    ground.albedo_red = 110u;
    ground.albedo_green = 110u;
    ground.albedo_blue = 100u;
    ground.detail_parameters[2] = 0.0f;
    ground.detail_parameters[3] = 7.0f;
    out[added++] = ground;
    for (uint32_t chunk = 0u; chunk < RING_CHUNKS; ++chunk) {
        for (uint32_t pass = 0u; pass < (ring->sorted ? 2u : 1u); ++pass) {
            for (uint32_t local = 0u; local < RING_CHUNK_PIECES; ++local) {
                const uint32_t piece = chunk * RING_CHUNK_PIECES + local;
                for (uint32_t part = 0u; part < 2u; ++part) {
                    if (ring->sorted && part != pass) {
                        continue;
                    }
                    BTBHGPUSceneGeometryBatch batch = part == 0u ? wall : glass;
                    batch.custom_mesh = ring->one
                        ? ring->meshes[0]
                        : ring->meshes[part == 0u ? piece : RING_PIECES + piece];
                    batch.instance_buffer = ring->chunks[chunk];
                    batch.first_instance = firsts[piece];
                    batch.instance_count = counts[piece];
                    out[added++] = batch;
                }
            }
        }
    }
    ring->batch_count = added;
    bench->batch_count += added;
}

/* v2's occlusion field over the bench's lots: a 2 x 2 unit block on each 3 unit lot (lots from -96, -72), most of them,
   0.385 to 3.535 high; 0 inside, outside the distance to the nearest inside texel's centre and that block's top. */
static uint32_t bench_lot_hash(int32_t lot_x, int32_t lot_z) {
    uint32_t h = (uint32_t)(lot_x * 73856093) ^ (uint32_t)(lot_z * 19349663);
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return h;
}

static void build_occlusion(BenchWindow *bench) {
    const uint32_t size = bench->rc_low ? BENCH_RC_FIELD / 2u : BENCH_RC_FIELD;
    const float cell = bench->rc_low ? 1.0f : 0.5f;
    const float origin = -64.0f;
    bench->occlusion = calloc((size_t)size * size * 2u, sizeof(float));
    if (bench->occlusion == NULL) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    for (uint32_t row = 0u; row < size; ++row) {
        for (uint32_t column = 0u; column < size; ++column) {
            const float x = origin + ((float)column + 0.5f) * cell;
            const float z = origin + ((float)row + 0.5f) * cell;
            const int32_t lot_x = (int32_t)floorf((x + 96.0f) / 3.0f);
            const int32_t lot_z = (int32_t)floorf((z + 72.0f) / 3.0f);
            float nearest = 1.0e6f;
            float top = 0.0f;
            for (int32_t dz = -1; dz <= 1; ++dz) {
                for (int32_t dx = -1; dx <= 1; ++dx) {
                    const uint32_t hash = bench_lot_hash(lot_x + dx, lot_z + dz);
                    if (hash % 4u == 0u) {
                        continue;
                    }
                    /* The block's inside texel centres. */
                    const float bx = -96.0f + 3.0f * (float)(lot_x + dx) + 0.5f;
                    const float bz = -72.0f + 3.0f * (float)(lot_z + dz) + 0.5f;
                    const float first_x = origin + (ceilf((bx - origin) / cell - 0.5f) + 0.5f) * cell;
                    const float last_x = origin + (floorf((bx + 2.0f - origin) / cell - 0.5f) + 0.5f) * cell;
                    const float first_z = origin + (ceilf((bz - origin) / cell - 0.5f) + 0.5f) * cell;
                    const float last_z = origin + (floorf((bz + 2.0f - origin) / cell - 0.5f) + 0.5f) * cell;
                    const float ex = fmaxf(fmaxf(first_x - x, x - last_x), 0.0f);
                    const float ez = fmaxf(fmaxf(first_z - z, z - last_z), 0.0f);
                    const float distance = sqrtf(ex * ex + ez * ez);
                    if (distance < nearest) {
                        nearest = distance;
                        top = 0.385f + 0.35f * (float)((hash >> 8) % 10u);
                    }
                }
            }
            bench->occlusion[((size_t)row * size + column) * 2u] = nearest;
            bench->occlusion[((size_t)row * size + column) * 2u + 1u] = top;
        }
    }
    uint32_t state = 4242u;
    for (uint32_t index = 0u; index < BENCH_RC_EMITTERS; ++index) {
        BTBHGPUSceneRadianceEmitter *emitter = &bench->emitters[index];
        memset(emitter, 0, sizeof(*emitter));
        emitter->stable_id = 1u + index;
        emitter->position_x = -48.0f + 96.0f * bench_unit(&state);
        emitter->position_y = 0.55f;
        emitter->position_z = -48.0f + 96.0f * bench_unit(&state);
        emitter->radius = 0.6f;
        emitter->height = 0.5f;
        emitter->red = 1.0f;
        emitter->green = 0.6f;
        emitter->blue = 0.3f;
        emitter->energy = 3.0f;
        emitter->direction_z = 1.0f;
        emitter->inner_cutoff = -1.0f;
        emitter->outer_cutoff = -2.0f;
        emitter->shape = BTBH_GPU_SCENE_RADIANCE_EMITTER_DISK;
        emitter->light_class = BTBH_GPU_SCENE_RADIANCE_EMITTER_CLASS_TRANSIENT;
    }
}

/* The smoke's fires: the columns' feet, where the point lights burn (and one more in the middle). */
static const float smoke_fires[SMOKE_LIGHTS][2] = {
    { -20.0f, -5.0f }, { -8.0f, 6.0f }, { 4.0f, -3.0f }, { 14.0f, 8.0f }, { 24.0f, -8.0f }, { 0.0f, 2.0f },
};

/* A 256 x 256 atlas of 2 x 2 cells of 128 pixels: a soft blob each (`normals`: its sphere's normal in rg, OpenGL +Y,
   and its thickness in b, linear; else grey, alpha the coverage). */
static BTBHGPUTexture *smoke_texture(BTBHGPUWindow *window, bool normals) {
    uint8_t *pixels = malloc(256u * 256u * 4u);
    if (pixels == NULL) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    uint32_t state = 99u;
    for (uint32_t y = 0u; y < 256u; ++y) {
        for (uint32_t x = 0u; x < 256u; ++x) {
            const float u = ((float)(x % 128u) + 0.5f) / 64.0f - 1.0f;
            const float v = ((float)(y % 128u) + 0.5f) / 64.0f - 1.0f;
            const float r = sqrtf(u * u + v * v) / 0.8f;
            const float wobble = 0.08f * bench_unit(&state);
            float coverage = 1.0f - r * r + wobble;
            coverage = coverage < 0.0f ? 0.0f : (coverage > 1.0f ? 1.0f : coverage);
            const float rise = r < 1.0f ? sqrtf(1.0f - r * r) : 0.0f;
            uint8_t *texel = &pixels[(y * 256u + x) * 4u];
            if (normals) {
                const float nx = r < 1.0f ? u / 0.8f : 0.0f;
                const float ny = r < 1.0f ? -v / 0.8f : 0.0f;
                texel[0] = (uint8_t)(127.5f + 127.0f * nx);
                texel[1] = (uint8_t)(127.5f + 127.0f * ny);
                texel[2] = (uint8_t)(255.0f * rise);
                texel[3] = 255u;
            } else {
                texel[0] = (uint8_t)(200u + bench_random(&state) % 40u);
                texel[1] = texel[0];
                texel[2] = texel[0];
                texel[3] = (uint8_t)(255.0f * coverage);
            }
        }
    }
    BTBHGPUTextureReport report;
    BTBHGPUTexture *texture = btbh_gpu_window_texture_create_rgba8_ex(window, pixels, 256u, 256u, 1u,
        BTBH_GPU_TEXTURE_FILTER_TRILINEAR, BTBH_GPU_TEXTURE_WRAP_CLAMP,
        normals ? BTBH_GPU_TEXTURE_COLOR_SPACE_LINEAR : BTBH_GPU_TEXTURE_COLOR_SPACE_SRGB, &report);
    free(pixels);
    if (texture == NULL) {
        fprintf(stderr, "smoke texture: %s\n", report.message);
        exit(1);
    }
    return texture;
}

typedef struct SmokePuff {
    float position[3];
    float scale;
    uint32_t cell;
    float depth;
} SmokePuff;

static int smoke_far_first(const void *left, const void *right) {
    const float a = ((const SmokePuff *)left)->depth;
    const float b = ((const SmokePuff *)right)->depth;
    return a < b ? 1 : (a > b ? -1 : 0);
}

/* The burning fight's puffs after the frame's draws: smoke-soft as soft particles, a batch per cell and tilt (10),
   each sorted far to near; smoke-lit as one lit particle batch with the UV rect, sorted far to near; smoke-off none.
   Prints what they cover at the render size and the lights in reach of a puff. */
static void build_smoke(BenchWindow *bench, const BenchCamera *camera, const BTBHGPUSceneLighting *lighting) {
    if (bench->smoke == 0) {
        return;
    }
    static SmokePuff puffs[SMOKE_PUFFS];
    uint32_t state = 4242u;
    uint32_t count = 0u;
    for (uint32_t column = 0u; column < SMOKE_COLUMNS; ++column) {
        for (uint32_t k = 0u; k < SMOKE_COLUMN_PUFFS; ++k) {
            SmokePuff *puff = &puffs[count++];
            puff->position[0] = smoke_fires[column][0] + 1.2f * (bench_unit(&state) - 0.5f) + 0.15f * (float)k;
            puff->position[1] = 1.5f + 0.9f * (float)k;
            puff->position[2] = smoke_fires[column][1] + 1.2f * (bench_unit(&state) - 0.5f);
            puff->scale = 3.6f * (0.8f + 0.5f * bench_unit(&state)) * (1.0f + 0.04f * (float)k);
            puff->cell = bench_random(&state) % 4u;
        }
    }
    while (count < SMOKE_PUFFS) {
        SmokePuff *puff = &puffs[count++];
        puff->position[0] = -46.0f + 92.0f * bench_unit(&state);
        puff->position[1] = 0.5f + 3.0f * bench_unit(&state);
        puff->position[2] = -28.0f + 56.0f * bench_unit(&state);
        puff->scale = 4.6f * (0.7f + 0.6f * bench_unit(&state));
        puff->cell = bench_random(&state) % 4u;
    }
    float forward[3];
    cross3(camera->up, camera->right, forward);
    for (uint32_t index = 0u; index < count; ++index) {
        const float offset[3] = {
            puffs[index].position[0] - camera->position[0], puffs[index].position[1] - camera->position[1],
            puffs[index].position[2] - camera->position[2],
        };
        puffs[index].depth = dot3(offset, forward);
    }
    qsort(puffs, count, sizeof(puffs[0]), smoke_far_first);
    /* What they cover, the coverage discs (0.8 of a quad's half size) at the render size, and the lights in reach. */
    static uint8_t layers[BENCH_MAX_RENDER_PIXELS];
    memset(layers, 0, sizeof(layers));
    double disc_sum = 0.0;
    float lights_in_reach = 0.0f;
    const float *m = camera->view_projection;
    for (uint32_t index = 0u; index < count; ++index) {
        const float *p = puffs[index].position;
        const float clip_x = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12];
        const float clip_y = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13];
        const float clip_w = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
        const float cx = (clip_x / clip_w * 0.5f + 0.5f) * (float)BENCH_RENDER_WIDTH;
        const float cy = (0.5f - clip_y / clip_w * 0.5f) * (float)BENCH_RENDER_HEIGHT;
        /* (The projection's y scale, 1 / tan(0.45), the camera's fov 0.9.) */
        const float radius = 0.8f * puffs[index].scale * (1.0f / tanf(0.45f)) / clip_w * 0.5f *
            (float)BENCH_RENDER_HEIGHT;
        disc_sum += 2.0 * (double)radius / (double)BENCH_RENDER_HEIGHT;
        for (int y = (int)(cy - radius); y <= (int)(cy + radius); ++y) {
            for (int x = (int)(cx - radius); x <= (int)(cx + radius); ++x) {
                if (x < 0 || y < 0 || x >= (int)BENCH_RENDER_WIDTH || y >= (int)BENCH_RENDER_HEIGHT) {
                    continue;
                }
                const float dx = (float)x + 0.5f - cx;
                const float dy = (float)y + 0.5f - cy;
                if (dx * dx + dy * dy <= radius * radius && layers[y * (int)BENCH_RENDER_WIDTH + x] < 255u) {
                    layers[y * (int)BENCH_RENDER_WIDTH + x] += 1u;
                }
            }
        }
        for (uint32_t lamp = 0u; lamp < lighting->lamp_count; ++lamp) {
            const float *l = lighting->lamp_position_reach[lamp];
            const float d[3] = { l[0] - p[0], l[1] - p[1], l[2] - p[2] };
            lights_in_reach += dot3(d, d) < l[3] * l[3] ? 1.0f : 0.0f;
        }
        for (uint32_t light = 0u; light < lighting->point_light_count; ++light) {
            const float *l = lighting->point_lights[light];
            const float d[3] = { l[0] - p[0], l[1] - p[1], l[2] - p[2] };
            lights_in_reach += dot3(d, d) < l[3] * l[3] ? 1.0f : 0.0f;
        }
    }
    uint32_t covered[3] = { 0u, 0u, 0u };
    uint32_t deepest = 0u;
    for (uint32_t index = 0u; index < BENCH_RENDER_WIDTH * BENCH_RENDER_HEIGHT; ++index) {
        covered[0] += layers[index] >= 1u ? 1u : 0u;
        covered[1] += layers[index] >= 3u ? 1u : 0u;
        covered[2] += layers[index] >= 6u ? 1u : 0u;
        deepest = layers[index] > deepest ? layers[index] : deepest;
    }
    const double pixels = (double)BENCH_RENDER_WIDTH * BENCH_RENDER_HEIGHT;
    printf("BENCH smoke %s: %u puffs, mean disc %.1f %% of the height; covered >=1 %.1f %%, >=3 %.1f %%, >=6 %.1f %%, "
        "deepest %u; %.2f lights in reach of a puff\n", bench->mode, count, 100.0 * disc_sum / (double)count,
        100.0 * covered[0] / pixels, 100.0 * covered[1] / pixels, 100.0 * covered[2] / pixels, deepest,
        lights_in_reach / (float)count);
    if (bench->smoke == 1) {
        return;
    }
    /* The puffs' instances: billboards, straight alpha capped at 0.5, the cell in custom (the UV rect). */
    const uint32_t first = bench->instance_count;
    for (uint32_t index = 0u; index < count; ++index) {
        float *instance = &bench->instances[(size_t)bench->instance_count++ * BENCH_FLOATS];
        memset(instance, 0, BENCH_FLOATS * sizeof(float));
        instance[0] = puffs[index].scale;
        instance[5] = puffs[index].scale;
        instance[10] = puffs[index].scale;
        instance[12] = puffs[index].position[0];
        instance[13] = puffs[index].position[1];
        instance[14] = puffs[index].position[2];
        instance[15] = 1.0f;
        instance[16] = 1.0f;
        instance[17] = 1.0f;
        instance[18] = 1.0f;
        instance[19] = 0.5f;
        instance[20] = 0.5f;
        instance[21] = 0.5f;
        instance[22] = 0.5f * (float)(puffs[index].cell % 2u);
        instance[23] = 0.5f * (float)(puffs[index].cell / 2u);
    }
    BTBHGPUSceneGeometryBatch puff;
    fill_batch(&puff, bench->puff, first, count);
    puff.albedo_texture = bench->smoke_atlas;
    puff.albedo_red = 110u;
    puff.albedo_green = 105u;
    puff.albedo_blue = 100u;
    puff.transparency_enabled = 1u;
    puff.blend_mode = BTBH_GPU_SCENE_BLEND_ALPHA;
    puff.billboard_mode = BTBH_GPU_SCENE_BILLBOARD_FRONT;
    puff.cull_mode = BTBH_GPU_SCENE_CULL_NONE;
    puff.depth_compare = BTBH_GPU_SCENE_DEPTH_LESS_EQUAL;
    puff.spot_shadow_visible = 0u;
    puff.surface_parameters0[0] = 0.4f;
    if (bench->smoke >= 3) {
#if defined(BTBH_GPU_SCENE_HAS_LIT_PARTICLE)
        puff.surface_variant = BTBH_GPU_SCENE_SURFACE_LIT_PARTICLE;
        puff.normal_texture = bench->smoke_normals;
        puff.normal_scale = 1.0f;
        puff.surface_parameters0[3] = 0.5f;
        puff.surface_parameters1[0] = 0.8f;
        puff.surface_parameters1[1] = 0.6f;
        puff.surface_parameters1[2] = 9.0f;
        puff.flags = BTBH_GPU_SCENE_BATCH_INSTANCE_UV_RECT;
#if defined(BTBH_GPU_SCENE_HAS_PARTICLE_SHADOW)
        if (bench->smoke >= 4) {
            puff.flags |= BTBH_GPU_SCENE_BATCH_PARTICLE_SHADOW;
        }
#endif
        bench->batches[bench->batch_count++] = puff;
#endif
        return;
    }
    /* v2's soft particles: unlit, a batch per cell and tilt (2 tilts of 4 cells, and 2 more for the columns' tops),
       each batch's puffs contiguous and far to near. */
    puff.surface_variant = BTBH_GPU_SCENE_SURFACE_SOFT_PARTICLE;
    puff.unlit_enabled = 1u;
    puff.uv_scale_x = 0.5f;
    puff.uv_scale_y = 0.5f;
    static SmokePuff grouped[SMOKE_PUFFS];
    uint32_t cursor = 0u;
    for (uint32_t group = 0u; group < 10u; ++group) {
        const uint32_t start = cursor;
        for (uint32_t index = 0u; index < count; ++index) {
            if ((index * 7u + puffs[index].cell) % 10u == group) {
                grouped[cursor++] = puffs[index];
            }
        }
        if (cursor == start) {
            continue;
        }
        BTBHGPUSceneGeometryBatch batch = puff;
        batch.first_instance = first + start;
        batch.instance_count = cursor - start;
        batch.uv_offset_x = 0.5f * (float)(group % 2u);
        batch.uv_offset_y = 0.5f * (float)((group / 2u) % 2u);
        bench->batches[bench->batch_count++] = batch;
    }
    for (uint32_t index = 0u; index < count; ++index) {
        float *instance = &bench->instances[((size_t)first + index) * BENCH_FLOATS];
        instance[0] = grouped[index].scale;
        instance[5] = grouped[index].scale;
        instance[10] = grouped[index].scale;
        instance[12] = grouped[index].position[0];
        instance[13] = grouped[index].position[1];
        instance[14] = grouped[index].position[2];
    }
}

static void open_bench_window(BenchWindow *bench, BTBHGPUDevice *device, const char *directory, uint32_t number,
    uint32_t draws) {
    memset(bench, 0, sizeof(*bench));
    bench_list_item(SDL_getenv("BTBH_FRAME_BENCH_MODES"), number, "none", bench->mode, sizeof(bench->mode));
    if (strncmp(bench->mode, "ring", 4) == 0) {
        bench->ring.enabled = true;
        bench->ring.table = mode_has(bench->mode, "table");
        bench->ring.sorted = mode_has(bench->mode, "sorted");
        bench->ring.arena = mode_has(bench->mode, "arena");
        bench->ring.runs_off = mode_has(bench->mode, "noruns");
        bench->ring.one = mode_has(bench->mode, "one");
    } else {
        /* (table+noruns, table+arena: runs off, the kit pieces in the shared buffers.) */
        bench->ring.runs_off = mode_has(bench->mode, "noruns");
    }
    bench->rc = mode_has(bench->mode, "rc");
    bench->rc_low = mode_has(bench->mode, "low");
    bench->rc_tops = mode_has(bench->mode, "tops");
    bench->light = strncmp(bench->mode, "light-off", 9) == 0 ? 1 :
        (strncmp(bench->mode, "light-mixed", 11) == 0 ? 3 : (strncmp(bench->mode, "light", 5) == 0 ? 2 : 0));
    bench->smoke = strncmp(bench->mode, "smoke-off", 9) == 0 ? 1 :
        (strncmp(bench->mode, "smoke-soft", 10) == 0 ? 2 : (strncmp(bench->mode, "smoke-lit", 9) == 0 ? 3 :
        (strncmp(bench->mode, "smoke-shadow", 12) == 0 ? 4 : (strncmp(bench->mode, "smoke-ground", 12) == 0 ? 5 : 0))));
    bench->smoke_quarter = mode_has(bench->mode, "quarter");
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
    {
        const int sun_map = bench_env_int("BTBH_FRAME_BENCH_SUN_MAP", 0, 0, 8192);
        if (sun_map >= 256 &&
            btbh_gpu_window_scene_set_directional_shadow_size(bench->window, (uint32_t)sun_map, (uint32_t)sun_map) ==
                0u) {
            fprintf(stderr, "the sun map's size %d isn't taken\n", sun_map);
            exit(1);
        }
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
        bench->pieces[piece] = make_piece(bench->window, piece, frame_index, strcmp(mode, "table-none") == 0,
            !bench->ring.enabled && mode_has(mode, "arena"));
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
    if (bench->smoke != 0) {
        bench->smoke_atlas = smoke_texture(bench->window, false);
        bench->smoke_normals = smoke_texture(bench->window, true);
        /* The puff: a quad over local x and y facing +z, tangent +x, uv 0..1. */
        float vertices[4u * BENCH_FLOATS];
        memset(vertices, 0, sizeof(vertices));
        static const float corners[4][2] = { { -1.0f, 1.0f }, { 1.0f, 1.0f }, { 1.0f, -1.0f }, { -1.0f, -1.0f } };
        for (uint32_t corner = 0u; corner < 4u; ++corner) {
            float *vertex = &vertices[corner * BENCH_FLOATS];
            vertex[0] = corners[corner][0];
            vertex[1] = corners[corner][1];
            vertex[5] = 1.0f;
            vertex[6] = 0.5f * corners[corner][0] + 0.5f;
            vertex[7] = 0.5f - 0.5f * corners[corner][1];
            vertex[8] = 1.0f;
            vertex[11] = 1.0f;
            vertex[12] = vertex[13] = vertex[14] = vertex[15] = 1.0f;
            vertex[20] = 1.0f;
        }
        const uint32_t indices[6] = { 0u, 1u, 2u, 0u, 2u, 3u };
        BTBHGPUCustomMeshReport mesh_report;
        memset(&mesh_report, 0, sizeof(mesh_report));
        bench->puff = btbh_gpu_window_custom_mesh_create(bench->window, vertices, (uint32_t)sizeof(vertices), 4u,
            indices, 6u, &mesh_report);
        if (bench->puff == NULL) {
            fprintf(stderr, "puff: %s\n", mesh_report.message);
            exit(1);
        }
    }
    bench->albedo = noise_texture(bench->window, 11u, BTBH_GPU_TEXTURE_COLOR_SPACE_SRGB);
    bench->normal = noise_texture(bench->window, 12u, BTBH_GPU_TEXTURE_COLOR_SPACE_LINEAR);
    bench->orm = noise_texture(bench->window, 13u, BTBH_GPU_TEXTURE_COLOR_SPACE_LINEAR);
#if defined(BTBH_GPU_SCENE_HAS_WORLD_DETAIL_TABLE)
    if (strncmp(mode, "table", 5) == 0 || bench->ring.table) {
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
#if defined(BTBH_GPU_SCENE_HAS_HEIGHT_LIFT)
    if (strncmp(mode, "lift", 4) == 0 && strncmp(mode, "lift-flat", 9) != 0) {
        /* A field over the city's lots (x -96..96, z -72..72), one sample a unit. */
        enum { FIELD_W = 193, FIELD_H = 145 };
        float *heights = malloc(sizeof(float) * FIELD_W * FIELD_H);
        if (heights == NULL) {
            fprintf(stderr, "out of memory\n");
            exit(1);
        }
        for (uint32_t j = 0u; j < FIELD_H; ++j) {
            for (uint32_t i = 0u; i < FIELD_W; ++i) {
                heights[j * FIELD_W + i] = 1.5f + 1.2f * sinf(0.07f * (float)i) * cosf(0.05f * (float)j) +
                    0.3f * sinf(0.31f * (float)(i + j));
            }
        }
        BTBHGPUTextureReport texture_report;
        bench->field = btbh_gpu_window_heightfield_create(bench->window, heights, FIELD_W, FIELD_H, &texture_report);
        free(heights);
        if (bench->field == NULL) {
            fprintf(stderr, "heightfield: %s\n", texture_report.message);
            exit(1);
        }
        memset(&bench->lift, 0, sizeof(bench->lift));
        bench->lift.heightfield = bench->field;
        bench->lift.world_to_uv[0] = 1.0f / (float)FIELD_W;
        bench->lift.world_to_uv[1] = 1.0f / (float)FIELD_H;
        bench->lift.world_to_uv[2] = 0.5f / (float)FIELD_W + 96.0f / (float)FIELD_W;
        bench->lift.world_to_uv[3] = 0.5f / (float)FIELD_H + 72.0f / (float)FIELD_H;
        bench->lift.flags = strncmp(mode, "lift-keep", 9) == 0 ? BTBH_GPU_SCENE_HEIGHT_LIFT_KEEP_NORMALS : 0u;
    }
#endif
    build_frame(bench, draws);
    if (bench->ring.enabled) {
        build_ring(bench);
    }
    if (bench->rc) {
        build_occlusion(bench);
    }
}

/* The window's post: the cascades as its mode asks. */
static BTBHGPUScenePost window_post(const BenchWindow *bench, const BTBHGPUScenePost *post) {
    BTBHGPUScenePost result = *post;
    if (!bench->rc) {
        return result;
    }
    BTBHGPUSceneRadianceCascades *cascades = &result.radiance_cascades;
    cascades->tier = bench->rc_low ? BTBH_GPU_SCENE_RC_LOW : BTBH_GPU_SCENE_RC_HIGH;
    cascades->auto_frame_budget_milliseconds = 16.0f;
    cascades->indirect_intensity = 1.0f;
    cascades->hero_omni_emitter_index = -1;
    cascades->additive = 1u;
    cascades->emitter_count = BENCH_RC_EMITTERS;
    cascades->emitters = bench->emitters;
    cascades->occlusion_cascade_count = 1u;
    cascades->occlusion_cascades[0].origin_world_x = -64.0f;
    cascades->occlusion_cascades[0].origin_world_z = -64.0f;
    cascades->occlusion_cascades[0].cell_size = bench->rc_low ? 1.0f : 0.5f;
    cascades->occlusion_cascades[0].width = bench->rc_low ? BENCH_RC_FIELD / 2u : BENCH_RC_FIELD;
    cascades->occlusion_cascades[0].height = cascades->occlusion_cascades[0].width;
    cascades->occlusion_cascades[0].revision = 1u;
    cascades->occlusion_cascades[0].samples = bench->occlusion;
#if defined(BTBH_GPU_SCENE_HAS_RC_SOURCE_TOPS)
    cascades->source_tops = bench->rc_tops ? 1u : 0u;
#endif
    return result;
}

/* The window's lighting: the smoke modes add the fires' and four neon signs' point lights, and a second row of lamps. */
static BTBHGPUSceneLighting window_lighting(const BenchWindow *bench, const BTBHGPUSceneLighting *lighting) {
    BTBHGPUSceneLighting result = *lighting;
    if (bench->light != 0) {
        result.window_glow = 1.5f;
    }
    if (bench->smoke == 0) {
        return result;
    }
#if defined(BTBH_GPU_SCENE_HAS_PARTICLE_SHADOW)
    if (bench->smoke >= 4) {
        result.particle_shadow = (uint8_t)((bench->smoke == 5 ? BTBH_GPU_SCENE_PARTICLE_SHADOW_GROUND
                                                              : BTBH_GPU_SCENE_PARTICLE_SHADOW_PARTICLES) |
            (bench->smoke_quarter ? BTBH_GPU_SCENE_PARTICLE_SHADOW_QUARTER : 0u));
    }
#endif
    static const float neon[SMOKE_NEON][3] = {
        { -30.0f, 4.0f, 15.0f }, { 30.0f, 4.0f, -15.0f }, { -36.0f, 4.0f, -16.0f }, { 36.0f, 4.0f, 17.0f },
    };
    result.point_light_count = SMOKE_LIGHTS + SMOKE_NEON;
    for (uint32_t light = 0u; light < SMOKE_LIGHTS + SMOKE_NEON; ++light) {
        float *point = result.point_lights[light];
        const bool fire = light < SMOKE_LIGHTS;
        point[0] = fire ? smoke_fires[light][0] : neon[light - SMOKE_LIGHTS][0];
        point[1] = fire ? 1.0f : neon[light - SMOKE_LIGHTS][1];
        point[2] = fire ? smoke_fires[light][1] : neon[light - SMOKE_LIGHTS][2];
        point[3] = fire ? 11.0f : 12.0f;
        point[4] = fire ? 1.0f : 0.9f;
        point[5] = fire ? 0.5f : 0.3f;
        point[6] = fire ? 0.2f : 1.0f;
        point[7] = fire ? 2.0f : 1.2f;
    }
    const uint32_t first_lamp = result.lamp_count;
    for (uint32_t lamp = 0u; lamp < 8u && first_lamp + lamp < BTBH_GPU_SCENE_MAX_LAMPS; ++lamp) {
        float *position = result.lamp_position_reach[first_lamp + lamp];
        position[0] = -35.0f + 10.0f * (float)lamp;
        position[1] = 3.0f;
        position[2] = 16.0f;
        position[3] = 15.0f;
        memcpy(result.lamp_color_energy[first_lamp + lamp], result.lamp_color_energy[0], 4u * sizeof(float));
        result.lamp_count += 1u;
    }
    return result;
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

static bool bench_cpu_mode;

static void run_frame(BenchWindow *bench, const BenchCamera *camera, const BTBHGPUSceneLighting *lighting,
    const BTBHGPUScenePost *post, uint32_t number, int round, bool with_ring) {
    BTBHGPUSceneReport report;
    const uint32_t batch_count = bench->batch_count - (with_ring ? 0u : bench->ring.batch_count);
    if (bench->ring.runs_off) {
        (void)SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "BTBH_GPU_SCENE_RUNS", "0", true);
    }
    const BTBHGPUScenePost frame_post = window_post(bench, post);
    post = &frame_post;
    const BTBHGPUSceneLighting frame_lighting = window_lighting(bench, lighting);
    lighting = &frame_lighting;
    const uint64_t started = SDL_GetTicksNS();
    const BTBHGPUResult presented = btbh_gpu_window_scene_present_instances(bench->window,
        bench->scene_shaders[0], bench->scene_shaders[1], bench->scene_shaders[2], bench->scene_shaders[3],
        bench->scene_shaders[4], bench->scene_shaders[5], bench->scene_shaders[6], bench->scene_shaders[7],
        bench->scene_shaders[8], bench->scene_shaders[9], bench->scene_shaders[10],
        BENCH_SHADER_FORMAT, BENCH_RENDER_WIDTH, BENCH_RENDER_HEIGHT,
        camera->view_projection, camera->right, camera->up, camera->position, lighting, post,
        bench->instances, bench->instance_count, bench->instance_count, bench->batches, batch_count,
        90u, 110u, 140u, 255u, &report);
    const double present_milliseconds = (double)(SDL_GetTicksNS() - started) / 1.0e6;
    if (bench->ring.runs_off) {
        (void)SDL_UnsetEnvironmentVariable(SDL_GetEnvironment(), "BTBH_GPU_SCENE_RUNS");
    }
    if (presented != BTBH_GPU_OK) {
        fprintf(stderr, "present: %s\n", report.message);
        exit(1);
    }
    if (bench->rc && report.radiance_cascades.active_tier == BTBH_GPU_SCENE_RC_OFF) {
        fprintf(stderr, "the cascades didn't run (fallback %d)\n", (int)report.radiance_cascades.fallback_reason);
        exit(1);
    }
    if (bench_cpu_mode && round >= 0) {
        printf("BENCH cpu %u %d ring=%d prepare=%.4f shadow=%.4f geometry=%.4f present=%.4f draws=%u\n", number,
            round, with_ring ? 1 : 0, report.cpu_prepare_milliseconds, report.cpu_shadow_encode_milliseconds,
            report.cpu_geometry_encode_milliseconds, present_milliseconds, report.primitive_draw_count);
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
    const char *cpu = SDL_getenv("BTBH_FRAME_BENCH_CPU");
    bench_cpu_mode = cpu != NULL && cpu[0] == '1';
    const char *preset = SDL_getenv("BTBH_FRAME_BENCH_PRESET");
    if (preset != NULL && strcmp(preset, "high") == 0) {
        BENCH_WIDTH = 1920u;
        BENCH_HEIGHT = 1080u;
        BENCH_RENDER_WIDTH = 1920u;
        BENCH_RENDER_HEIGHT = 1080u;
    }
    printf("BENCH preset %s: %ux%u rendered at %ux%u, sun map %d\n", preset != NULL ? preset : "deck", BENCH_WIDTH,
        BENCH_HEIGHT, BENCH_RENDER_WIDTH, BENCH_RENDER_HEIGHT,
        bench_env_int("BTBH_FRAME_BENCH_SUN_MAP", 2048, 0, 8192));
    if (!bench_cpu_mode) {
        (void)SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "BTBH_GPU_COST_LOG", "1", true);
    }
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
    const float eye[3] = { 0.0f, 95.0f, 95.0f };
    const float centre[3] = { 0.0f, 0.0f, 0.0f };
    const BenchCamera camera = look_at(eye, centre, 0.9f, (float)BENCH_WIDTH / (float)BENCH_HEIGHT);
    const BTBHGPUSceneLighting lighting = bench_lighting();
    const BTBHGPUScenePost post = bench_post();
    static BenchWindow windows[BENCH_MAX_WINDOWS];
    uint32_t window_count = 0u;
    for (const char *at = list; *at != 0 && window_count < BENCH_MAX_WINDOWS;) {
        const char *end = strchr(at, '|');
        const size_t length = end != NULL ? (size_t)(end - at) : strlen(at);
        char directory[1024];
        (void)snprintf(directory, sizeof(directory), "%.*s", (int)length, at);
        open_bench_window(&windows[window_count], device, directory, window_count, draws);
        {
            const BTBHGPUSceneLighting smoke_lighting = window_lighting(&windows[window_count], &lighting);
            build_smoke(&windows[window_count], &camera, &smoke_lighting);
        }
        printf("BENCH window %u %s mode=%s draws=%u instances=%u triangles=%llu ring=%u%s\n", window_count,
            directory, windows[window_count].mode, windows[window_count].batch_count,
            windows[window_count].instance_count, (unsigned long long)windows[window_count].triangles,
            windows[window_count].ring.batch_count,
#if defined(BTBH_GPU_HAS_SHARED_MESH_BUFFERS)
            ""
#else
            windows[window_count].ring.arena ? " (no shared buffers before 0.10.0)" : ""
#endif
        );
        window_count += 1u;
        at = end != NULL ? end + 1 : at + length;
    }
    /* Pipelines, caches and clocks first. */
    for (uint32_t w = 0u; w < window_count; ++w) {
        for (int frame = 0; frame < 10; ++frame) {
            run_frame(&windows[w], &camera, &lighting, &post, w, -1, frame % 2 == 0);
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
                /* A ring window alternates: with the ring, then without. */
                BenchWindow *bench = &windows[order[position]];
                run_frame(bench, &camera, &lighting, &post, order[position], round,
                    !bench->ring.enabled || frame % 2 == 0);
            }
        }
    }
    printf("BENCH done\n");
    (void)fflush(stdout);
    for (uint32_t w = 0u; w < window_count; ++w) {
        free(windows[w].instances);
        free(windows[w].batches);
        free(windows[w].occlusion);
        btbh_gpu_window_destroy(windows[w].window);
    }
    btbh_gpu_device_shutdown(device);
    btbh_gpu_device_destroy(device);
    return 0;
}
