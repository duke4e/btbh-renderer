/* The lazy creation log and the upload staging budget (0.11.0, Be The Bullet Hell v2's night perf round).
 *
 * Three hidden windows present the same short scene (a sun shadow, FSR 1, SMAA, bloom; boxes on a ground; an additive
 * unlit flash and an alpha box appear in frame EFFECT_FRAME), each frame copied into a texture and read back:
 * - C: log off, no staging budget, not prewarmed (the reference);
 * - A: log on (btbh_gpu_window_set_lazy_creation_log_enabled), not prewarmed;
 * - B: log on, prewarmed with every batch, made after the device took an upload staging budget (its cube in the shared
 *   geometry buffers, its textures and the scene's own through the pool).
 * Every frame of A and B is C's to the byte. A's log names what its frames made: in frame 0 and EFFECT_FRAME only,
 * shaders, pipelines (as many graphics pipelines as btbh_gpu_creation_stats counted), samplers and textures marked
 * prewarm_covers, and of what isn't, only the buffers that grow with the instances; frame_index is the frame. B's log has nothing a
 * prewarm covers. A's canvas present and render to a target are logged as theirs. The report's refusals, its copy in
 * pieces and turning the log off are checked. Then, with the budget, textures of every upload path (made, updated,
 * loaded, an array, a heightfield, many small ones in a row, one bigger than the budget) read back as their pixels,
 * and the budget's refusals; and instance buffers (plain and NO_CYCLE uploads, NO_CYCLE into a frame waiting for its
 * canvas, QUEUED beside them) and custom meshes with buffers of their own (made, updated whole and in part, in part
 * into a waiting frame) read back as written, in rounds before anything is read. Last, frame telemetry's fences: one of a
 * frame that presented nothing is released only once its frame has run (blits queued before it are done by then), and
 * a window that never polls keeps its newest samples. It needs a GPU and the production shaders; with no device it is
 * skipped (77). */
#undef NDEBUG

#include "btbh_gpu/btbh_gpu.h"
#include "btbh_gpu_internal.h"

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_hints.h>

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SMOKE_SKIP 77
#define FRAME_COUNT 6u
#define EFFECT_FRAME 3u
#define INSTANCE_FLOATS 24u
#define INSTANCE_COUNT 7u
#define WINDOW_WIDTH 192u
#define WINDOW_HEIGHT 108u
#define RENDER_WIDTH 96u
#define RENDER_HEIGHT 54u
#define ENTRY_CAPACITY 512u

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
    BTBHGPUTexture *frame;
} SmokeWindow;

static BTBHGPULazyCreationEntry entries[ENTRY_CAPACITY];

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

/* A view-projection looking from eye at centre (perspective, or orthographic with half size `ortho`). */
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
    const float view[16] = {
        side[0], up[0], -forward[0], 0.0f,
        side[1], up[1], -forward[1], 0.0f,
        side[2], up[2], -forward[2], 0.0f,
        -dot3(eye, side), -dot3(eye, up), dot3(eye, forward), 1.0f,
    };
    float projection[16];
    if (ortho > 0.0f) {
        const float range = 1.0f / (1.0f - 30.0f);
        const float orthographic[16] = {
            1.0f / ortho, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f / ortho, 0.0f, 0.0f,
            0.0f, 0.0f, range, 0.0f,
            0.0f, 0.0f, range, 1.0f,
        };
        memcpy(projection, orthographic, sizeof(projection));
    } else {
        const float h = 1.0f / tanf(0.4f);
        const float aspect = (float)WINDOW_WIDTH / (float)WINDOW_HEIGHT;
        const float r = 50.0f / (0.1f - 50.0f);
        const float perspective[16] = {
            h / aspect, 0.0f, 0.0f, 0.0f,
            0.0f, h, 0.0f, 0.0f,
            0.0f, 0.0f, r, -1.0f,
            0.0f, 0.0f, r * 0.1f, 0.0f,
        };
        memcpy(projection, perspective, sizeof(projection));
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
    lighting.directional_energy = 2.0f;
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

static BTBHGPUScenePost smoke_post(void) {
    BTBHGPUScenePost post;
    memset(&post, 0, sizeof(post));
    post.tonemap_white = 1.0f;
    post.exposure = 1.0f;
    post.contrast = 1.0f;
    post.saturation = 1.0f;
    post.blur_curve = 1.0f;
    post.tonemap_mode = BTBH_GPU_SCENE_TONEMAP_ACES;
    post.upscale_mode = BTBH_GPU_SCENE_UPSCALE_FSR1;
    post.anti_aliasing = 1u;
    post.bloom_enabled = 1u;
    post.bloom_intensity = 0.4f;
    post.bloom_threshold = 1.0f;
    post.bloom_soft_knee = 0.5f;
    post.bloom_scatter = 0.7f;
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

/* The ground and three boxes from the first frame; from EFFECT_FRAME an additive unlit flash and an alpha box. */
static uint32_t smoke_batches(const SmokeWindow *smoke, uint32_t frame, BTBHGPUSceneGeometryBatch out[4]) {
    out[0] = cube_batch(smoke, 0u, 1u);
    out[1] = cube_batch(smoke, 1u, 3u);
    if (frame < EFFECT_FRAME) {
        return 2u;
    }
    out[2] = cube_batch(smoke, 4u, 1u);
    out[2].transparency_enabled = 1u;
    out[2].blend_mode = BTBH_GPU_SCENE_BLEND_ADDITIVE;
    out[2].unlit_enabled = 1u;
    out[2].cull_mode = BTBH_GPU_SCENE_CULL_NONE;
    out[3] = cube_batch(smoke, 5u, 2u);
    out[3].transparency_enabled = 1u;
    out[3].albedo_alpha = 140u;
    out[3].cull_mode = BTBH_GPU_SCENE_CULL_NONE;
    return 4u;
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

static void smoke_instances(float out[INSTANCE_COUNT][INSTANCE_FLOATS], uint32_t frame) {
    put_instance(out[0], 0.0f, -0.05f, 0.0f, 10.0f, 0.1f, 10.0f, 0.55f, 0.6f, 0.5f);
    put_instance(out[1], -2.0f, 0.75f, -1.5f, 1.2f, 1.5f, 1.2f, 0.7f, 0.68f, 0.62f);
    put_instance(out[2], 2.2f, 1.0f, -2.0f, 1.0f, 2.0f, 1.4f, 0.7f, 0.68f, 0.62f);
    put_instance(out[3], -1.8f + 0.05f * (float)frame, 0.5f, 2.0f, 1.5f, 1.0f, 1.0f, 0.9f, 0.3f, 0.2f);
    put_instance(out[4], 0.2f, 0.6f, 0.4f, 0.4f, 0.4f, 0.4f, 3.0f, 2.2f, 1.0f);
    put_instance(out[5], 0.8f, 0.6f, 1.2f, 1.0f, 1.0f, 0.2f, 0.3f, 0.8f, 1.0f);
    put_instance(out[6], -0.6f, 0.4f, 0.9f, 0.6f, 0.8f, 0.6f, 0.9f, 0.9f, 0.3f);
}

/* A unit cube, 24 floats a vertex, its outsides lit. */
static BTBHGPUCustomMesh *create_cube(BTBHGPUWindow *window, bool shared) {
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
        indices, 36u, shared ? BTBH_GPU_CUSTOM_MESH_SHARED_BUFFERS : 0u, &report);
    if (mesh == NULL) {
        fprintf(stderr, "cube: %s\n", report.message);
    }
    assert(mesh != NULL);
    return mesh;
}

/* A texture's layer read back through SDL_GPU (its first mip). */
static uint8_t *read_texture(BTBHGPUWindow *window, const BTBHGPUTexture *texture, uint32_t layer) {
    SDL_GPUDevice *device = window->device->gpu;
    const uint32_t size = texture->width * texture->height * SDL_GPUTextureFormatTexelBlockSize(texture->format);
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
    region.layer = layer;
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

static SmokeWindow open_window(BTBHGPUDevice *device, const char *title, bool shared) {
    SmokeWindow smoke;
    memset(&smoke, 0, sizeof(smoke));
    const BTBHGPUWindowDescriptor descriptor = {
        .title = title,
        .width = WINDOW_WIDTH,
        .height = WINDOW_HEIGHT,
        .hidden = 1,
        .high_pixel_density = 0,
    };
    BTBHGPUWindowReport report;
    memset(&report, 0, sizeof(report));
    smoke.window = btbh_gpu_window_create(&descriptor, &report);
    assert(smoke.window != NULL);
    assert(btbh_gpu_window_claim(device, smoke.window, &report) == BTBH_GPU_OK);
    smoke.cube = create_cube(smoke.window, shared);
    BTBHGPUTextureReport texture_report;
    smoke.frame = btbh_gpu_window_render_target_create(
        smoke.window, WINDOW_WIDTH, WINDOW_HEIGHT, BTBH_GPU_TEXTURE_FILTER_LINEAR, &texture_report);
    assert(smoke.frame != NULL);
    return smoke;
}

static BTBHGPUResult prewarm(SmokeWindow *smoke) {
    const BTBHGPUSceneLighting lighting = smoke_lighting();
    const BTBHGPUScenePost post = smoke_post();
    BTBHGPUSceneGeometryBatch batches[4];
    const uint32_t batch_count = smoke_batches(smoke, EFFECT_FRAME, batches);
    BTBHGPUSceneReport report;
    return btbh_gpu_window_scene_prewarm(smoke->window, shader_paths[0], shader_paths[1], shader_paths[2],
        shader_paths[3], shader_paths[4], shader_paths[5], shader_paths[6], shader_paths[7], shader_paths[8],
        shader_paths[9], shader_paths[10], SMOKE_SHADER_FORMAT, RENDER_WIDTH, RENDER_HEIGHT, &lighting, &post,
        batches, batch_count, &report);
}

/* Set: run_frame presents the scene and leaves it waiting for its canvas (returns NULL). */
static bool run_frame_leaves_pending;

/* Frame `frame` of the scene, presented (into `target` when given, else the window's frame, then copied out) and
   read back. */
static uint8_t *run_frame(SmokeWindow *smoke, uint32_t frame, BTBHGPUTexture *target) {
    float view_projection[16];
    float right[3];
    float up[3];
    const float eye[3] = { 6.0f * sinf(0.6f + 0.02f * (float)frame), 3.5f, 6.0f * cosf(0.6f + 0.02f * (float)frame) };
    const float centre[3] = { 0.0f, 0.4f, 0.0f };
    look_at(eye, centre, 0.0f, view_projection, right, up);
    const BTBHGPUSceneLighting lighting = smoke_lighting();
    const BTBHGPUScenePost post = smoke_post();
    float instances[INSTANCE_COUNT][INSTANCE_FLOATS];
    smoke_instances(instances, frame);
    BTBHGPUSceneGeometryBatch batches[4];
    const uint32_t batch_count = smoke_batches(smoke, frame, batches);
    BTBHGPUSceneReport report;
    BTBHGPUResult result;
    if (target != NULL) {
        result = btbh_gpu_window_scene_render_to_target(smoke->window, target, shader_paths[0], shader_paths[1],
            shader_paths[2], shader_paths[3], shader_paths[4], shader_paths[5], shader_paths[6], shader_paths[7],
            shader_paths[8], shader_paths[9], shader_paths[10], SMOKE_SHADER_FORMAT, RENDER_WIDTH, RENDER_HEIGHT,
            view_projection, right, up, eye, &lighting, &post, &instances[0][0], INSTANCE_COUNT, INSTANCE_COUNT,
            batches, NULL, NULL, batch_count, 40u, 60u, 90u, 255u, &report);
    } else {
        result = btbh_gpu_window_scene_present_instances(smoke->window, shader_paths[0], shader_paths[1],
            shader_paths[2], shader_paths[3], shader_paths[4], shader_paths[5], shader_paths[6], shader_paths[7],
            shader_paths[8], shader_paths[9], shader_paths[10], SMOKE_SHADER_FORMAT, RENDER_WIDTH, RENDER_HEIGHT,
            view_projection, right, up, eye, &lighting, &post, &instances[0][0], INSTANCE_COUNT, INSTANCE_COUNT,
            batches, batch_count, 40u, 60u, 90u, 255u, &report);
    }
    if (result != BTBH_GPU_OK) {
        fprintf(stderr, "frame %u: %s\n", frame, report.message);
    }
    assert(result == BTBH_GPU_OK);
    if (target != NULL) {
        return read_texture(smoke->window, target, 0u);
    }
    if (run_frame_leaves_pending) {
        return NULL;
    }
    BTBHGPUTextureReport copied;
    assert(btbh_gpu_window_copy_pending_scene_texture(smoke->window, smoke->frame, &copied) == BTBH_GPU_OK);
    return read_texture(smoke->window, smoke->frame, 0u);
}

/* What a window's log holds since the last report, counted. */
typedef struct LogCounts {
    uint32_t entries;
    uint32_t covered;          /* prewarm_covers 1 */
    uint32_t uncovered;        /* creations with prewarm_covers 0 */
    uint32_t slow_calls;
    uint32_t graphics_pipelines;
    uint32_t shaders;
    uint32_t textures;
    uint32_t wrong_call;       /* entries of another call than expected */
    uint32_t wrong_frame;      /* entries of another frame than expected */
    uint64_t first_sequence;
    uint64_t last_sequence;
} LogCounts;

static LogCounts drain(BTBHGPUWindow *window, uint8_t call, uint64_t frame_index, uint32_t capacity) {
    LogCounts counts;
    memset(&counts, 0, sizeof(counts));
    uint32_t count = 0u;
    bool first = true;
    do {
        assert(btbh_gpu_window_lazy_creation_report(window, entries, capacity, &count) == BTBH_GPU_OK);
        assert(count <= capacity);
        for (uint32_t index = 0u; index < count; ++index) {
            const BTBHGPULazyCreationEntry *entry = &entries[index];
            assert(entry->kind >= BTBH_GPU_LAZY_CREATION_SHADER && entry->kind <= BTBH_GPU_LAZY_CREATION_SLOW_CALL);
            assert(entry->name[0] != '\0');
            assert(memchr(entry->name, '\0', sizeof(entry->name)) != NULL);
            if (!first) {
                /* Nothing was dropped: the sequence runs on. */
                assert(entry->sequence == counts.last_sequence + 1u);
            } else {
                counts.first_sequence = entry->sequence;
            }
            first = false;
            counts.last_sequence = entry->sequence;
            counts.entries += 1u;
            counts.wrong_call += entry->call != call ? 1u : 0u;
            counts.wrong_frame += entry->frame_index != frame_index ? 1u : 0u;
            if (entry->kind == BTBH_GPU_LAZY_CREATION_SLOW_CALL) {
                counts.slow_calls += 1u;
                assert(entry->prewarm_covers == 0u);
                continue;
            }
            if (entry->prewarm_covers != 0u) {
                counts.covered += 1u;
            } else {
                /* What no prewarm makes: buffers that grow with a frame's contents, or the canvas's. */
                assert(entry->kind == BTBH_GPU_LAZY_CREATION_BUFFER ||
                    entry->kind == BTBH_GPU_LAZY_CREATION_TRANSFER_BUFFER ||
                    entry->call == BTBH_GPU_LAZY_CREATION_CALL_CANVAS_PRESENT);
                counts.uncovered += 1u;
            }
            counts.graphics_pipelines += entry->kind == BTBH_GPU_LAZY_CREATION_GRAPHICS_PIPELINE ? 1u : 0u;
            counts.shaders += entry->kind == BTBH_GPU_LAZY_CREATION_SHADER ? 1u : 0u;
            counts.textures += entry->kind == BTBH_GPU_LAZY_CREATION_TEXTURE ? 1u : 0u;
        }
    } while (count == capacity);
    return counts;
}

static BTBHGPUCreationStats creation_stats(void) {
    BTBHGPUCreationStats stats;
    btbh_gpu_creation_stats(&stats);
    return stats;
}

static void assert_same_texels(const uint8_t *a, const uint8_t *b, uint32_t width, uint32_t height, uint32_t texel,
    const char *what) {
    if (memcmp(a, b, (size_t)width * height * texel) != 0) {
        uint32_t differing = 0u;
        for (size_t index = 0u; index < (size_t)width * height; ++index) {
            differing += memcmp(a + index * texel, b + index * texel, texel) != 0 ? 1u : 0u;
        }
        fprintf(stderr, "%s: %u texels differ\n", what, differing);
        assert(false);
    }
}

static void assert_same(const uint8_t *a, const uint8_t *b, uint32_t width, uint32_t height, const char *what) {
    assert_same_texels(a, b, width, height, 4u, what);
}

/* The log: what A's cold frames made, B's prewarmed frames nothing a prewarm covers, every frame C's. */
static void check_log(BTBHGPUDevice *device) {
    SmokeWindow c = open_window(device, "lazy C", false);
    SmokeWindow a = open_window(device, "lazy A", false);
    btbh_gpu_window_set_lazy_creation_log_enabled(a.window, 1u);
    assert(btbh_gpu_device_set_upload_staging_budget(device, 8u << 20) == BTBH_GPU_OK);
    SmokeWindow b = open_window(device, "lazy B", true);
    btbh_gpu_window_set_lazy_creation_log_enabled(b.window, 1u);
    assert(prewarm(&b) == BTBH_GPU_OK);
    /* (The prewarm isn't a present: it logs nothing.) */
    LogCounts counts = drain(b.window, BTBH_GPU_LAZY_CREATION_CALL_SCENE_PRESENT, 0u, ENTRY_CAPACITY);
    assert(counts.entries == 0u);
    uint64_t a_last_sequence = 0u;
    for (uint32_t frame = 0u; frame < FRAME_COUNT; ++frame) {
        uint8_t *reference = run_frame(&c, frame, NULL);
        const BTBHGPUCreationStats before = creation_stats();
        uint8_t *logged = run_frame(&a, frame, NULL);
        const BTBHGPUCreationStats after = creation_stats();
        uint8_t *prewarmed = run_frame(&b, frame, NULL);
        const BTBHGPUCreationStats after_b = creation_stats();
        assert_same(reference, logged, WINDOW_WIDTH, WINDOW_HEIGHT, "log on against off");
        assert_same(reference, prewarmed, WINDOW_WIDTH, WINDOW_HEIGHT, "prewarmed with staging against cold");
        free(reference);
        free(logged);
        free(prewarmed);
        /* (A small capacity: the report is taken in pieces.) */
        counts = drain(a.window, BTBH_GPU_LAZY_CREATION_CALL_SCENE_PRESENT, frame, frame == 0u ? 7u : ENTRY_CAPACITY);
        printf("A frame %u: %u entries, %u covered (%u shaders, %u graphics pipelines, %u textures), %u not, %u slow\n",
            frame, counts.entries, counts.covered, counts.shaders, counts.graphics_pipelines, counts.textures,
            counts.uncovered, counts.slow_calls);
        assert(counts.wrong_call == 0u);
        assert(counts.wrong_frame == 0u);
        assert(counts.graphics_pipelines == after.graphics_pipelines - before.graphics_pipelines);
        assert(counts.shaders == after.shaders - before.shaders);
        if (frame == 0u || frame == EFFECT_FRAME) {
            assert(counts.covered > 0u);
            assert(counts.graphics_pipelines > 0u);
        } else {
            assert(counts.covered == 0u);
        }
        if (frame == 0u) {
            /* The first frame's instance, bone and upload buffers: nothing makes them ahead. */
            assert(counts.textures > 0u && counts.shaders > 0u);
            assert(counts.uncovered > 0u);
        }
        if (counts.entries > 0u) {
            assert(a_last_sequence == 0u ? counts.first_sequence == 0u : counts.first_sequence == a_last_sequence + 1u);
            a_last_sequence = counts.last_sequence;
        }
        counts = drain(b.window, BTBH_GPU_LAZY_CREATION_CALL_SCENE_PRESENT, frame, ENTRY_CAPACITY);
        printf("B frame %u: %u entries, %u covered, %u not, %u slow\n", frame, counts.entries, counts.covered,
            counts.uncovered, counts.slow_calls);
        assert(counts.wrong_call == 0u);
        assert(counts.covered == 0u);
        assert(after_b.shaders == after.shaders && after_b.graphics_pipelines == after.graphics_pipelines &&
            after_b.compute_pipelines == after.compute_pipelines);
    }
    /* A canvas present (its runtime is made now): the canvas's, none covered by a prewarm. */
    BTBHGPUCanvasPass pass;
    memset(&pass, 0, sizeof(pass));
    BTBHGPUCanvasReport canvas_report;
    assert(btbh_gpu_window_canvas_present(a.window, shader_paths[11], shader_paths[12], shader_paths[13],
        SMOKE_SHADER_FORMAT, NULL, 0u, NULL, 0u, &pass, 1u, &canvas_report) == BTBH_GPU_OK);
    counts = drain(a.window, BTBH_GPU_LAZY_CREATION_CALL_CANVAS_PRESENT, FRAME_COUNT, ENTRY_CAPACITY);
    printf("A canvas: %u entries (%u graphics pipelines, %u shaders)\n", counts.entries, counts.graphics_pipelines,
        counts.shaders);
    assert(counts.entries > 0u && counts.graphics_pipelines > 0u);
    assert(counts.covered == 0u && counts.wrong_call == 0u && counts.wrong_frame == 0u);
    /* A render to a target: its own runtime, made by the first call, a prewarm (its target form) would have. */
    BTBHGPUTextureReport texture_report;
    BTBHGPUTexture *target = btbh_gpu_window_render_target_create(a.window, RENDER_WIDTH, RENDER_HEIGHT,
        BTBH_GPU_TEXTURE_FILTER_LINEAR, &texture_report);
    assert(target != NULL);
    free(run_frame(&a, 0u, target));
    counts = drain(a.window, BTBH_GPU_LAZY_CREATION_CALL_RENDER_TO_TARGET, FRAME_COUNT + 1u, ENTRY_CAPACITY);
    printf("A render to a target: %u entries, %u covered\n", counts.entries, counts.covered);
    assert(counts.covered > 0u && counts.graphics_pipelines > 0u && counts.wrong_call == 0u);
    free(run_frame(&a, 1u, target));
    counts = drain(a.window, BTBH_GPU_LAZY_CREATION_CALL_RENDER_TO_TARGET, FRAME_COUNT + 1u, ENTRY_CAPACITY);
    assert(counts.covered == 0u);
    /* Turned off, the log records nothing. */
    btbh_gpu_window_set_lazy_creation_log_enabled(a.window, 0u);
    btbh_gpu_window_scene_set_directional_shadow_size(a.window, 1024u, 1024u);
    free(run_frame(&a, 2u, NULL));
    counts = drain(a.window, 0u, 0u, ENTRY_CAPACITY);
    assert(counts.entries == 0u);
    /* Turned on again, a remade shadow map is the present's (a prewarm at these settings would have made it). */
    btbh_gpu_window_set_lazy_creation_log_enabled(a.window, 1u);
    btbh_gpu_window_scene_set_directional_shadow_size(a.window, 512u, 512u);
    free(run_frame(&a, 3u, NULL));
    counts = drain(a.window, BTBH_GPU_LAZY_CREATION_CALL_SCENE_PRESENT, FRAME_COUNT + 2u, ENTRY_CAPACITY);
    printf("A after a shadow map resize: %u entries, %u covered (%u textures)\n", counts.entries, counts.covered,
        counts.textures);
    assert(counts.textures > 0u && counts.covered > 0u);
    assert(counts.first_sequence > a_last_sequence);
    /* The report's refusals. */
    uint32_t count = 99u;
    assert(btbh_gpu_window_lazy_creation_report(NULL, entries, 1u, &count) == BTBH_GPU_INVALID_ARGUMENT);
    assert(count == 0u);
    assert(btbh_gpu_window_lazy_creation_report(a.window, entries, 1u, NULL) == BTBH_GPU_INVALID_ARGUMENT);
    assert(btbh_gpu_window_lazy_creation_report(a.window, NULL, 1u, &count) == BTBH_GPU_INVALID_ARGUMENT);
    assert(btbh_gpu_window_lazy_creation_report(a.window, NULL, 0u, &count) == BTBH_GPU_OK && count == 0u);
    count = 99u;
    assert(btbh_gpu_window_lazy_creation_report(c.window, entries, ENTRY_CAPACITY, &count) == BTBH_GPU_OK);
    assert(count == 0u);
    btbh_gpu_window_set_lazy_creation_log_enabled(NULL, 1u);
    btbh_gpu_window_destroy(a.window);
    btbh_gpu_window_destroy(b.window);
    btbh_gpu_window_destroy(c.window);
    assert(btbh_gpu_device_set_upload_staging_budget(device, 0u) == BTBH_GPU_OK);
}

static uint8_t *pattern(uint32_t width, uint32_t height, uint32_t seed) {
    uint8_t *pixels = malloc((size_t)width * height * 4u);
    assert(pixels != NULL);
    uint32_t state = seed * 2654435761u + 1u;
    for (size_t index = 0u; index < (size_t)width * height * 4u; ++index) {
        state = state * 1664525u + 1013904223u;
        pixels[index] = (uint8_t)(state >> 24);
    }
    return pixels;
}

static BTBHGPUTexture *texture_from(BTBHGPUWindow *window, const uint8_t *pixels, uint32_t width, uint32_t height) {
    BTBHGPUTextureReport report;
    memset(&report, 0, sizeof(report));
    BTBHGPUTexture *texture = btbh_gpu_window_texture_create_rgba8_ex(window, pixels, width, height, 0u,
        BTBH_GPU_TEXTURE_FILTER_LINEAR, BTBH_GPU_TEXTURE_WRAP_CLAMP, BTBH_GPU_TEXTURE_COLOR_SPACE_LINEAR, &report);
    if (texture == NULL) {
        fprintf(stderr, "texture %ux%u: %s\n", width, height, report.message);
    }
    assert(texture != NULL);
    return texture;
}

static void check_texture(BTBHGPUWindow *window, const BTBHGPUTexture *texture, uint32_t layer, const uint8_t *pixels,
    const char *what) {
    uint8_t *read = read_texture(window, texture, layer);
    assert_same_texels(pixels, read, texture->width, texture->height,
        SDL_GPUTextureFormatTexelBlockSize(texture->format), what);
    free(read);
}

/* Every upload path through the staging pool: the pixels arrive. */
static void check_staging(BTBHGPUDevice *device) {
    assert(btbh_gpu_device_set_upload_staging_budget(NULL, 1u << 20) == BTBH_GPU_INVALID_ARGUMENT);
    SmokeWindow smoke = open_window(device, "staging", true);
    BTBHGPUWindow *window = smoke.window;
    /* Without a budget, the reference for the file loaded below. */
    BTBHGPUTextureReport report;
    BTBHGPUTexture *loaded_plain = btbh_gpu_window_texture_load(window, BTBH_GPU_SMOKE_FIXTURE_DIR "/placeholder.png",
        0u, BTBH_GPU_TEXTURE_FILTER_LINEAR, BTBH_GPU_TEXTURE_WRAP_CLAMP, &report);
    assert(loaded_plain != NULL);
    uint8_t *loaded_pixels = read_texture(window, loaded_plain, 0u);
    /* And for a heightfield (half floats: a height and its slopes a texel). */
    float heights[33u * 17u];
    for (uint32_t index = 0u; index < 33u * 17u; ++index) {
        heights[index] = 0.25f * (float)(index % 61u) - 3.0f;
    }
    BTBHGPUTexture *field_plain = btbh_gpu_window_heightfield_create(window, heights, 33u, 17u, &report);
    assert(field_plain != NULL);
    uint8_t *field_texels = read_texture(window, field_plain, 0u);
    btbh_gpu_texture_destroy(field_plain);
    assert(btbh_gpu_device_set_upload_staging_budget(device, 8u << 20) == BTBH_GPU_OK);
    /* Small ones in a row share a chunk; a 4 MiB one fills one; one bigger than the budget makes its own. */
    static const uint32_t sizes[][2] = {
        { 1u, 1u }, { 4u, 4u }, { 300u, 200u }, { 17u, 33u }, { 1024u, 1024u }, { 64u, 64u }, { 2048u, 1536u },
        { 513u, 7u },
    };
    for (uint32_t round = 0u; round < 3u; ++round) {
        for (uint32_t index = 0u; index < SDL_arraysize(sizes); ++index) {
            const uint32_t width = sizes[index][0];
            const uint32_t height = sizes[index][1];
            uint8_t *pixels = pattern(width, height, round * 100u + index);
            BTBHGPUTexture *texture = texture_from(window, pixels, width, height);
            check_texture(window, texture, 0u, pixels, "made with a staging budget");
            /* An update of a region, from the pool too. */
            if (width >= 4u && height >= 4u) {
                uint8_t *patch = pattern(2u, 3u, 7u + index);
                assert(btbh_gpu_texture_update_rgba8(texture, patch, 1u, 1u, 2u, 3u) == BTBH_GPU_OK);
                for (uint32_t y = 0u; y < 3u; ++y) {
                    memcpy(&pixels[((size_t)(1u + y) * width + 1u) * 4u], &patch[(size_t)y * 2u * 4u], 8u);
                }
                check_texture(window, texture, 0u, pixels, "updated with a staging budget");
                free(patch);
            }
            free(pixels);
            btbh_gpu_texture_destroy(texture);
        }
    }
    /* Many before any is read back, in rounds: the uploads take consecutive ranges of a chunk, and a chunk starts
       again only once the GPU has read every one (some are updated too). Each round's textures read back as made.
       BTBH_LAZY_SMOKE_STAGING_ROUNDS sets the rounds (default 25); 300 rounds found SDL's fence reuse when a pending
       upload's fence was released early. */
    const char *rounds_text = SDL_getenv("BTBH_LAZY_SMOKE_STAGING_ROUNDS");
    const int rounds = rounds_text != NULL && rounds_text[0] != '\0' ? atoi(rounds_text) : 25;
    uint32_t state = 12345u;
    for (int round = 0; round < rounds; ++round) {
        BTBHGPUTexture *textures[64];
        uint8_t *texture_pixels[64];
        for (uint32_t index = 0u; index < 64u; ++index) {
            state = state * 1664525u + 1013904223u;
            const uint32_t width = 1u + (state >> 8) % 300u;
            state = state * 1664525u + 1013904223u;
            const uint32_t height = 1u + (state >> 8) % 200u;
            texture_pixels[index] = pattern(width, height, state);
            textures[index] = texture_from(window, texture_pixels[index], width, height);
            if ((state >> 4) % 5u == 0u && width >= 4u && height >= 4u) {
                uint8_t *patch = pattern(3u, 2u, state + 1u);
                assert(btbh_gpu_texture_update_rgba8(textures[index], patch, 0u, 1u, 3u, 2u) == BTBH_GPU_OK);
                for (uint32_t y = 0u; y < 2u; ++y) {
                    memcpy(&texture_pixels[index][(size_t)(1u + y) * width * 4u], &patch[(size_t)y * 3u * 4u], 12u);
                }
                free(patch);
            }
        }
        for (uint32_t index = 0u; index < 64u; ++index) {
            check_texture(window, textures[index], 0u, texture_pixels[index], "one of many with a staging budget");
            free(texture_pixels[index]);
            btbh_gpu_texture_destroy(textures[index]);
        }
    }
    printf("staging: %d rounds of 64 textures read back as made\n", rounds);
    /* An array, layer by layer. */
    uint8_t *layers[3];
    for (uint32_t layer = 0u; layer < 3u; ++layer) {
        layers[layer] = pattern(64u, 32u, 900u + layer);
    }
    BTBHGPUTexture *array = btbh_gpu_window_texture_array_create_rgba8_ex(window, (const uint8_t *const *)layers, 64u,
        32u, 3u, 0u, BTBH_GPU_TEXTURE_FILTER_LINEAR, BTBH_GPU_TEXTURE_WRAP_CLAMP, BTBH_GPU_TEXTURE_COLOR_SPACE_LINEAR,
        &report);
    assert(array != NULL);
    for (uint32_t layer = 0u; layer < 3u; ++layer) {
        check_texture(window, array, layer, layers[layer], "an array layer with a staging budget");
        free(layers[layer]);
    }
    btbh_gpu_texture_destroy(array);
    /* A heightfield. */
    BTBHGPUTexture *field = btbh_gpu_window_heightfield_create(window, heights, 33u, 17u, &report);
    assert(field != NULL);
    check_texture(window, field, 0u, field_texels, "a heightfield with a staging budget");
    free(field_texels);
    btbh_gpu_texture_destroy(field);
    /* A file. */
    BTBHGPUTexture *loaded = btbh_gpu_window_texture_load(window, BTBH_GPU_SMOKE_FIXTURE_DIR "/placeholder.png", 0u,
        BTBH_GPU_TEXTURE_FILTER_LINEAR, BTBH_GPU_TEXTURE_WRAP_CLAMP, &report);
    assert(loaded != NULL);
    check_texture(window, loaded, 0u, loaded_pixels, "a file with a staging budget");
    free(loaded_pixels);
    btbh_gpu_texture_destroy(loaded);
    btbh_gpu_texture_destroy(loaded_plain);
    /* A lower budget, then none: the pool lets its buffers go. */
    assert(btbh_gpu_device_set_upload_staging_budget(device, 1u << 20) == BTBH_GPU_OK);
    uint8_t *pixels = pattern(100u, 100u, 77u);
    BTBHGPUTexture *texture = texture_from(window, pixels, 100u, 100u);
    check_texture(window, texture, 0u, pixels, "made with a lowered staging budget");
    btbh_gpu_texture_destroy(texture);
    free(pixels);
    assert(btbh_gpu_device_set_upload_staging_budget(device, 0u) == BTBH_GPU_OK);
    btbh_gpu_window_destroy(window);
}

/* A GPU buffer's bytes [offset, offset + size), read back through SDL_GPU. */
static uint8_t *read_buffer(BTBHGPUWindow *window, SDL_GPUBuffer *buffer, uint32_t offset, uint32_t size) {
    SDL_GPUDevice *device = window->device->gpu;
    SDL_GPUTransferBufferCreateInfo info;
    SDL_zero(info);
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    info.size = size;
    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(device, &info);
    assert(transfer != NULL);
    SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(device);
    assert(command != NULL);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command);
    SDL_GPUBufferRegion region;
    SDL_zero(region);
    region.buffer = buffer;
    region.offset = offset;
    region.size = size;
    SDL_GPUTransferBufferLocation destination;
    SDL_zero(destination);
    destination.transfer_buffer = transfer;
    SDL_DownloadFromGPUBuffer(copy, &region, &destination);
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

static void check_bytes(BTBHGPUWindow *window, SDL_GPUBuffer *buffer, uint32_t offset, const void *expected,
    uint32_t size, const char *what) {
    uint8_t *read = read_buffer(window, buffer, offset, size);
    if (memcmp(read, expected, size) != 0) {
        uint32_t differing = 0u;
        for (uint32_t index = 0u; index < size; ++index) {
            differing += read[index] != ((const uint8_t *)expected)[index] ? 1u : 0u;
        }
        fprintf(stderr, "%s: %u of %u bytes differ\n", what, differing, size);
        assert(false);
    }
    free(read);
}

static uint32_t next_random(uint32_t *state) {
    *state = *state * 1664525u + 1013904223u;
    return *state >> 8;
}

/* `count` vertices of 24 floats (finite, the positions small) and `index_count` indices below the vertex count. */
static float *mesh_vertices(uint32_t count, uint32_t *state) {
    float *vertices = malloc((size_t)count * 24u * sizeof(float));
    assert(vertices != NULL);
    for (size_t index = 0u; index < (size_t)count * 24u; ++index) {
        vertices[index] = (float)(next_random(state) % 2001u) / 1000.0f - 1.0f;
    }
    return vertices;
}

static uint32_t *mesh_indices(uint32_t count, uint32_t vertex_count, uint32_t *state) {
    uint32_t *indices = malloc((size_t)count * sizeof(uint32_t));
    assert(indices != NULL);
    for (uint32_t index = 0u; index < count; ++index) {
        indices[index] = next_random(state) % vertex_count;
    }
    return indices;
}

#define STAGING_BUFFERS 24u
#define STAGING_MESHES 16u
#define STAGING_STRIDE 96u

/* Instance buffer uploads (plain, NO_CYCLE, NO_CYCLE into a frame waiting for its canvas, QUEUED beside them) and
   custom meshes with buffers of their own (made, updated whole, updated in part, in part into a waiting frame), under
   a staging budget, in rounds before anything is read back: every byte arrives. */
static void check_staging_buffers(BTBHGPUDevice *device) {
    assert(btbh_gpu_device_set_upload_staging_budget(device, 8u << 20) == BTBH_GPU_OK);
    SmokeWindow smoke = open_window(device, "staging buffers", false);
    BTBHGPUWindow *window = smoke.window;
    const char *rounds_text = SDL_getenv("BTBH_LAZY_SMOKE_STAGING_ROUNDS");
    const int rounds = rounds_text != NULL && rounds_text[0] != '\0' ? atoi(rounds_text) : 25;
    uint32_t state = 777u;
    uint32_t checks = 0u;
    for (int round = 0; round < rounds; ++round) {
        BTBHGPUInstanceBuffer *buffers[STAGING_BUFFERS];
        uint8_t *expected[STAGING_BUFFERS];
        uint32_t sizes[STAGING_BUFFERS];
        for (uint32_t index = 0u; index < STAGING_BUFFERS; ++index) {
            const uint32_t capacity = 1u + next_random(&state) % 1500u;
            sizes[index] = capacity * STAGING_STRIDE;
            BTBHGPUInstanceBufferReport report;
            buffers[index] = btbh_gpu_window_instance_buffer_create(window, capacity, STAGING_STRIDE, &report);
            assert(buffers[index] != NULL);
            expected[index] = pattern(sizes[index] / 4u, 1u, next_random(&state));
            assert(btbh_gpu_instance_buffer_upload(buffers[index], 0u, expected[index], sizes[index], &report) ==
                BTBH_GPU_OK);
            /* A part in place (NO_CYCLE), the rest kept. */
            const uint32_t first = next_random(&state) % capacity;
            const uint32_t count = 1u + next_random(&state) % (capacity - first);
            uint8_t *part = pattern(count * STAGING_STRIDE / 4u, 1u, next_random(&state));
            assert(btbh_gpu_instance_buffer_upload_ex(buffers[index], first * STAGING_STRIDE, part,
                count * STAGING_STRIDE, BTBH_GPU_INSTANCE_BUFFER_UPLOAD_NO_CYCLE, &report) == BTBH_GPU_OK);
            memcpy(expected[index] + (size_t)first * STAGING_STRIDE, part, (size_t)count * STAGING_STRIDE);
            free(part);
        }
        BTBHGPUCustomMesh *meshes[STAGING_MESHES];
        float *mesh_data[STAGING_MESHES];
        uint32_t *mesh_index_data[STAGING_MESHES];
        uint32_t vertex_counts[STAGING_MESHES];
        uint32_t index_counts[STAGING_MESHES];
        for (uint32_t index = 0u; index < STAGING_MESHES; ++index) {
            vertex_counts[index] = 3u + next_random(&state) % 900u;
            index_counts[index] = 3u * (1u + next_random(&state) % 600u);
            mesh_data[index] = mesh_vertices(vertex_counts[index], &state);
            mesh_index_data[index] = mesh_indices(index_counts[index], vertex_counts[index], &state);
            BTBHGPUCustomMeshReport report;
            meshes[index] = btbh_gpu_window_custom_mesh_create_ex(window, mesh_data[index],
                vertex_counts[index] * STAGING_STRIDE, vertex_counts[index], mesh_index_data[index],
                index_counts[index], 0u, &report);
            assert(meshes[index] != NULL);
            if (index % 3u == 0u) {
                /* Updated whole (a new size), as a mesh of its own does (cycling). */
                free(mesh_data[index]);
                free(mesh_index_data[index]);
                vertex_counts[index] = 3u + next_random(&state) % 900u;
                index_counts[index] = 3u * (1u + next_random(&state) % 600u);
                mesh_data[index] = mesh_vertices(vertex_counts[index], &state);
                mesh_index_data[index] = mesh_indices(index_counts[index], vertex_counts[index], &state);
                assert(btbh_gpu_custom_mesh_update_ex(meshes[index], mesh_data[index],
                    vertex_counts[index] * STAGING_STRIDE, mesh_index_data[index], index_counts[index], 0u,
                    &report) == BTBH_GPU_OK);
            }
            /* In part, in place. */
            const uint32_t first_vertex = next_random(&state) % vertex_counts[index];
            const uint32_t vertices = 1u + next_random(&state) % (vertex_counts[index] - first_vertex);
            const uint32_t first_index = next_random(&state) % index_counts[index];
            const uint32_t indices = index % 2u == 0u ? 1u + next_random(&state) % (index_counts[index] - first_index)
                                                      : 0u;
            float *new_vertices = mesh_vertices(vertices, &state);
            uint32_t *new_indices = indices > 0u ? mesh_indices(indices, vertex_counts[index], &state) : NULL;
            assert(btbh_gpu_custom_mesh_update_range(meshes[index], first_vertex, new_vertices,
                vertices * STAGING_STRIDE, first_index, new_indices, indices, 0u, &report) == BTBH_GPU_OK);
            memcpy(mesh_data[index] + (size_t)first_vertex * 24u, new_vertices, (size_t)vertices * STAGING_STRIDE);
            if (indices > 0u) {
                memcpy(mesh_index_data[index] + first_index, new_indices, (size_t)indices * sizeof(uint32_t));
            }
            free(new_vertices);
            free(new_indices);
        }
        /* A frame waiting for its canvas: a NO_CYCLE upload and a range update go into it after its draws (buffers of
           their own, not the pool's); a queued upload is copied by the next scene from the scene's transfer buffer. */
        if (round % 5 == 0) {
            const uint32_t queued = (uint32_t)round % STAGING_BUFFERS;
            uint8_t *queued_part = pattern(STAGING_STRIDE / 4u, 1u, next_random(&state));
            BTBHGPUInstanceBufferReport instance_report;
            assert(btbh_gpu_instance_buffer_upload_ex(buffers[queued], 0u, queued_part, STAGING_STRIDE,
                BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED, &instance_report) == BTBH_GPU_OK);
            memcpy(expected[queued], queued_part, STAGING_STRIDE);
            free(queued_part);
            run_frame_leaves_pending = true;
            (void)run_frame(&smoke, (uint32_t)round % FRAME_COUNT, NULL);
            run_frame_leaves_pending = false;
            const uint32_t into = ((uint32_t)round + 1u) % STAGING_BUFFERS;
            uint8_t *into_part = pattern(STAGING_STRIDE / 4u, 1u, next_random(&state));
            assert(btbh_gpu_instance_buffer_upload_ex(buffers[into], 0u, into_part, STAGING_STRIDE,
                BTBH_GPU_INSTANCE_BUFFER_UPLOAD_NO_CYCLE, &instance_report) == BTBH_GPU_OK);
            memcpy(expected[into], into_part, STAGING_STRIDE);
            free(into_part);
            const uint32_t mesh = (uint32_t)round % STAGING_MESHES;
            float *into_vertices = mesh_vertices(1u, &state);
            BTBHGPUCustomMeshReport mesh_report;
            assert(btbh_gpu_custom_mesh_update_range(meshes[mesh], 0u, into_vertices, STAGING_STRIDE, 0u, NULL, 0u, 0u,
                &mesh_report) == BTBH_GPU_OK);
            memcpy(mesh_data[mesh], into_vertices, STAGING_STRIDE);
            free(into_vertices);
            BTBHGPUTextureReport copied;
            assert(btbh_gpu_window_copy_pending_scene_texture(window, smoke.frame, &copied) == BTBH_GPU_OK);
        }
        for (uint32_t index = 0u; index < STAGING_BUFFERS; ++index) {
            check_bytes(window, buffers[index]->buffer, 0u, expected[index], sizes[index],
                "an instance buffer with a staging budget");
            check_bytes(window, buffers[index]->vertex_buffer, 0u, expected[index], sizes[index],
                "an instance buffer's vertex copy with a staging budget");
            checks += 2u;
            free(expected[index]);
            btbh_gpu_instance_buffer_destroy(buffers[index]);
        }
        for (uint32_t index = 0u; index < STAGING_MESHES; ++index) {
            check_bytes(window, meshes[index]->vertex_buffer, 0u, mesh_data[index],
                vertex_counts[index] * STAGING_STRIDE, "a custom mesh's vertices with a staging budget");
            check_bytes(window, meshes[index]->index_buffer, 0u, mesh_index_data[index],
                index_counts[index] * (uint32_t)sizeof(uint32_t), "a custom mesh's indices with a staging budget");
            checks += 2u;
            free(mesh_data[index]);
            free(mesh_index_data[index]);
            btbh_gpu_custom_mesh_destroy(meshes[index]);
        }
    }
    printf("staging: %d rounds of %u instance buffers and %u meshes, %u buffers read back as written\n", rounds,
        STAGING_BUFFERS, STAGING_MESHES, checks);
    assert(btbh_gpu_device_set_upload_staging_budget(device, 0u) == BTBH_GPU_OK);
    btbh_gpu_window_destroy(window);
}

/* The canvas present that ends a frame (a hidden window: no swapchain image, so the frame presents nothing). */
static void canvas_frame(BTBHGPUWindow *window) {
    BTBHGPUCanvasPass pass;
    memset(&pass, 0, sizeof(pass));
    BTBHGPUCanvasReport report;
    assert(btbh_gpu_window_canvas_present(window, shader_paths[11], shader_paths[12], shader_paths[13],
        SMOKE_SHADER_FORMAT, NULL, 0u, NULL, 0u, &pass, 1u, &report) == BTBH_GPU_OK);
}

/* Frame telemetry's fences (0.11.0 fix): SDL hands a released fence to the next submission at once, so the core must
   not release one before its frame has run. A frame that presented nothing (a hidden or minimized window) is held only
   by the core's reference. The GPU is kept busy by blits submitted first (their fence `busy`); a frame follows them;
   turning the telemetry off releases the frame's fence: it may do so only once the frame (and so the blits before it)
   has run. Then a window that never polls keeps its newest samples, every fence held until it signalled. */
static void check_frame_telemetry_fences(BTBHGPUDevice *device) {
    SmokeWindow smoke = open_window(device, "telemetry fences", false);
    BTBHGPUWindow *window = smoke.window;
    SDL_GPUDevice *gpu = device->gpu;
    /* (The scene's and the canvas's pipelines made first, so that a frame below takes little CPU.) */
    free(run_frame(&smoke, 0u, NULL));
    canvas_frame(window);
    SDL_GPUTextureCreateInfo info;
    SDL_zero(info);
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    info.width = 4096u;
    info.height = 4096u;
    info.layer_count_or_depth = 1u;
    info.num_levels = 1u;
    SDL_GPUTexture *textures[2] = { SDL_CreateGPUTexture(gpu, &info), SDL_CreateGPUTexture(gpu, &info) };
    assert(textures[0] != NULL && textures[1] != NULL);
    for (uint32_t attempt = 0u; attempt < 3u; ++attempt) {
        btbh_gpu_window_set_frame_telemetry_enabled(window, 1u);
        SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(gpu);
        assert(command != NULL);
        SDL_GPUBlitInfo blit;
        SDL_zero(blit);
        blit.source.w = info.width;
        blit.source.h = info.height;
        blit.destination.w = info.width;
        blit.destination.h = info.height;
        blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
        blit.filter = SDL_GPU_FILTER_LINEAR;
        for (uint32_t index = 0u; index < 200u * (attempt + 1u); ++index) {
            blit.source.texture = textures[index & 1u];
            blit.destination.texture = textures[(index + 1u) & 1u];
            SDL_BlitGPUTexture(command, &blit);
        }
        const uint64_t started = SDL_GetTicksNS();
        SDL_GPUFence *busy = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
        assert(busy != NULL);
        run_frame_leaves_pending = true;
        (void)run_frame(&smoke, 1u, NULL);
        run_frame_leaves_pending = false;
        canvas_frame(window);
        assert(window->frame_telemetry_pending_count == 1u);
        const bool running = !SDL_QueryGPUFence(gpu, busy);
        btbh_gpu_window_set_frame_telemetry_enabled(window, 0u);
        const bool finished = SDL_QueryGPUFence(gpu, busy);
        assert(SDL_WaitForGPUFences(gpu, true, &busy, 1u));
        printf("telemetry fences: blits for %.1f ms; running when the frame's fence was released: %s; finished "
            "after: %s\n", (double)(SDL_GetTicksNS() - started) / 1.0e6, running ? "yes" : "no",
            finished ? "yes" : "no");
        SDL_ReleaseGPUFence(gpu, busy);
        if (running) {
            /* The frame ran after the blits: its fence went back to SDL only once they had run too. */
            assert(finished);
            break;
        }
        /* (The GPU finished the blits before the check: try again with more.) */
        assert(attempt + 1u < 3u);
    }
    SDL_ReleaseGPUTexture(gpu, textures[0]);
    SDL_ReleaseGPUTexture(gpu, textures[1]);
    /* A window that never polls: the newest samples stay, in order, and finishing waits for every one (256 at once
       didn't wait at all). */
    btbh_gpu_window_set_frame_telemetry_enabled(window, 1u);
    const uint32_t frames = BTBH_GPU_FRAME_TELEMETRY_CAPACITY + 40u;
    for (uint32_t frame = 0u; frame < frames; ++frame) {
        run_frame_leaves_pending = true;
        (void)run_frame(&smoke, frame % FRAME_COUNT, NULL);
        run_frame_leaves_pending = false;
        canvas_frame(window);
        assert(window->frame_telemetry_pending_count <= BTBH_GPU_FRAME_TELEMETRY_CAPACITY);
    }
    static BTBHGPUFrameTelemetrySample samples[BTBH_GPU_FRAME_TELEMETRY_CAPACITY];
    const uint32_t count = btbh_gpu_window_finish_frame_telemetry(window, samples, BTBH_GPU_FRAME_TELEMETRY_CAPACITY);
    printf("telemetry fences: %u frames unpolled, %u samples kept, serials %llu..%llu\n", frames, count,
        count > 0u ? (unsigned long long)samples[0].frame_serial : 0ull,
        count > 0u ? (unsigned long long)samples[count - 1u].frame_serial : 0ull);
    assert(count == BTBH_GPU_FRAME_TELEMETRY_CAPACITY);
    for (uint32_t index = 0u; index < count; ++index) {
        assert(samples[index].frame_serial == frames - BTBH_GPU_FRAME_TELEMETRY_CAPACITY + 1u + index);
    }
    btbh_gpu_window_set_frame_telemetry_enabled(window, 0u);
    btbh_gpu_window_destroy(window);
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
    check_log(device);
    check_staging(device);
    check_staging_buffers(device);
    check_frame_telemetry_fences(device);
    btbh_gpu_device_shutdown(device);
    btbh_gpu_device_destroy(device);
    printf("lazy creation smoke passed\n");
    return 0;
}
