/* BTBH_GPU_CUSTOM_MESH_UPDATE_QUEUED (0.11.0, RFC 0012's extra): custom-mesh updates queued into the window's next
 * scene (btbh_gpu_custom_mesh_update_ex), rendered into a target or presented, and read back (hidden windows, a debug
 * device). A queued mesh draws byte for byte as a mesh made with its data:
 * - in the next render-to-target, and in the next present (its scene captured);
 * - the last of several queued updates; a plain update after a queued one, and a queued one after a plain one;
 * - growing a mesh with buffers of its own; a mesh in the shared buffers in place (a bigger update refused, the mesh
 *   as it was);
 * - the ordering rule: a plain update made between a present that copied a queued one and the canvas that submits
 *   it lands after it (the presented frame shows the queued data, the next scene the plain one), for a mesh in the
 *   shared buffers and one with buffers of its own;
 * - a mesh destroyed with an update queued (the others still land); a scene that fails before it draws leaves the
 *   queue for the next; a window closed with updates queued;
 * - the refusals: an unknown flag, no report, invalid data (nothing queued).
 * BTBH_SMOKE_BENCH=mesh-queue: 200 updates of a 64-vertex mesh a frame, plain against queued (and in the shared
 * buffers), interleaved rounds; prints the CPU time of the updates and of the present per update.
 *
 * It needs a GPU and the production shaders; with no device it is skipped (77). */
#undef NDEBUG

#include "btbh_gpu/btbh_gpu.h"
#include "btbh_gpu_internal.h"

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SMOKE_SKIP 77
#define TARGET_SIZE 128u
#define TEXELS (TARGET_SIZE * TARGET_SIZE)
#define FLOATS 24u

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
static char shader_paths[11][1100];
static char canvas_shader_paths[3][1100];

typedef struct SmokeWindow {
    BTBHGPUWindow *window;
    BTBHGPUTexture *target;
    BTBHGPUCustomMesh *ground;
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

static SmokeCamera look_at(const float eye[3], const float centre[3], float fov_y) {
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
    memcpy(camera.right, side, sizeof(side));
    memcpy(camera.up, up, sizeof(up));
    memcpy(camera.position, eye, sizeof(camera.position));
    return camera;
}

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

/* ------------------------------------------------------------------------------------------------- meshes */

typedef struct MeshData {
    float vertices[72u * FLOATS];
    uint32_t indices[108];
    uint32_t vertex_count;
    uint32_t index_count;
} MeshData;

/* Appends an axis-aligned box (centre, half sizes): 24 vertices and 36 indices, tinted. */
static void add_box(MeshData *data, const float centre[3], const float half[3], const float tint[3]) {
    static const float faces[6][3] = {
        { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 },
    };
    for (uint32_t face = 0u; face < 6u; ++face) {
        const float *n = faces[face];
        const float u[3] = { n[1], n[2], n[0] };
        float v[3];
        cross3(n, u, v);
        const uint32_t base = data->vertex_count;
        for (uint32_t corner = 0u; corner < 4u; ++corner) {
            const float su = (corner == 1u || corner == 2u) ? 1.0f : -1.0f;
            const float sv = corner >= 2u ? 1.0f : -1.0f;
            float *vertex = &data->vertices[(size_t)data->vertex_count * FLOATS];
            memset(vertex, 0, FLOATS * sizeof(float));
            for (int axis = 0; axis < 3; ++axis) {
                vertex[axis] = centre[axis] + half[axis] * (n[axis] + su * u[axis] + sv * v[axis]);
                vertex[3 + axis] = n[axis];
                vertex[8 + axis] = u[axis];
                vertex[12 + axis] = tint[axis];
            }
            vertex[6] = 0.5f * su + 0.5f;
            vertex[7] = 0.5f * sv + 0.5f;
            vertex[11] = 1.0f;
            vertex[15] = 1.0f;
            vertex[20] = 1.0f;
            data->vertex_count += 1u;
        }
        const uint32_t wound[6] = { base, base + 2u, base + 1u, base, base + 3u, base + 2u };
        memcpy(&data->indices[data->index_count], wound, sizeof(wound));
        data->index_count += 6u;
    }
}

/* Shapes: 0 and 1 one box each (same counts, apart), 2 two boxes (twice the counts), 3 a flat slab. */
static MeshData shape(uint32_t which) {
    MeshData data;
    memset(&data, 0, sizeof(data));
    static const float red[3] = { 1.0f, 0.3f, 0.2f };
    static const float green[3] = { 0.3f, 1.0f, 0.4f };
    static const float blue[3] = { 0.3f, 0.5f, 1.0f };
    switch (which) {
        case 0: add_box(&data, (const float[3]){ -0.6f, 0.4f, 0.0f }, (const float[3]){ 0.4f, 0.4f, 0.4f }, red); break;
        case 1: add_box(&data, (const float[3]){ 0.7f, 0.3f, 0.2f }, (const float[3]){ 0.3f, 0.3f, 0.6f }, green); break;
        case 2:
            add_box(&data, (const float[3]){ 0.0f, 0.8f, -0.4f }, (const float[3]){ 0.5f, 0.2f, 0.2f }, blue);
            add_box(&data, (const float[3]){ 0.2f, 0.2f, 0.5f }, (const float[3]){ 0.2f, 0.2f, 0.2f }, red);
            break;
        default:
            add_box(&data, (const float[3]){ 0.0f, 0.1f, 0.0f }, (const float[3]){ 0.9f, 0.1f, 0.9f }, green);
            break;
    }
    return data;
}

static uint32_t bytes_of(const MeshData *data) {
    return data->vertex_count * FLOATS * (uint32_t)sizeof(float);
}

static BTBHGPUCustomMesh *create_mesh(BTBHGPUWindow *window, const MeshData *data, bool shared) {
    BTBHGPUCustomMeshReport report;
    memset(&report, 0, sizeof(report));
    BTBHGPUCustomMesh *mesh = btbh_gpu_window_custom_mesh_create_ex(window, data->vertices, bytes_of(data),
        data->vertex_count, data->indices, data->index_count, shared ? BTBH_GPU_CUSTOM_MESH_SHARED_BUFFERS : 0u,
        &report);
    assert(mesh != NULL);
    return mesh;
}

static void update(BTBHGPUCustomMesh *mesh, const MeshData *data, bool queued) {
    BTBHGPUCustomMeshReport report;
    memset(&report, 0, sizeof(report));
    const BTBHGPUResult result = btbh_gpu_custom_mesh_update_ex(mesh, data->vertices, bytes_of(data), data->indices,
        data->index_count, queued ? BTBH_GPU_CUSTOM_MESH_UPDATE_QUEUED : 0u, &report);
    if (result != BTBH_GPU_OK) {
        fprintf(stderr, "update: %s\n", report.message);
    }
    assert(result == BTBH_GPU_OK);
    assert(report.vertex_count == data->vertex_count && report.index_count == data->index_count);
}

/* ------------------------------------------------------------------------------------------------- scenes */

typedef struct Scene {
    SmokeCamera camera;
    BTBHGPUSceneLighting lighting;
    BTBHGPUScenePost post;
    float instances[4][FLOATS];
    BTBHGPUSceneGeometryBatch batches[4];
    uint32_t batch_count;
} Scene;

static void instance_at(float out[FLOATS], float x, float y, float z, float sx, float sy, float sz) {
    memset(out, 0, FLOATS * sizeof(float));
    out[0] = sx;
    out[5] = sy;
    out[10] = sz;
    out[12] = x;
    out[13] = y;
    out[14] = z;
    out[15] = 1.0f;
    out[16] = out[17] = out[18] = out[19] = 1.0f;
}

static BTBHGPUSceneGeometryBatch base_batch(BTBHGPUCustomMesh *mesh, uint32_t instance) {
    BTBHGPUSceneGeometryBatch batch;
    memset(&batch, 0, sizeof(batch));
    batch.kind = BTBH_GPU_SCENE_GEOMETRY_CUSTOM_MESH;
    batch.custom_mesh = mesh;
    batch.material_slot = -1;
    batch.first_instance = instance;
    batch.instance_count = 1u;
    batch.albedo_red = batch.albedo_green = batch.albedo_blue = batch.albedo_alpha = 255u;
    batch.uv_scale_x = batch.uv_scale_y = 1.0f;
    batch.normal_scale = 1.0f;
    batch.roughness = 0.6f;
    batch.occlusion = 1.0f;
    batch.depth_compare = BTBH_GPU_SCENE_DEPTH_LESS;
    batch.cull_mode = BTBH_GPU_SCENE_CULL_BACK;
    batch.stencil_pass = BTBH_GPU_SCENE_STENCIL_REPLACE;
    return batch;
}

/* The ground and `mesh` (twice), lit by a sun, no shadow maps. */
static void build_scene(const SmokeWindow *smoke, BTBHGPUCustomMesh *mesh, Scene *scene) {
    memset(scene, 0, sizeof(*scene));
    const float eye[3] = { 0.5f, 2.6f, 3.2f };
    const float centre[3] = { 0.0f, 0.3f, 0.0f };
    scene->camera = look_at(eye, centre, 0.9f);
    BTBHGPUSceneLighting *lighting = &scene->lighting;
    lighting->ambient_red = lighting->ambient_green = lighting->ambient_blue = 1.0f;
    lighting->ambient_energy = 0.35f;
    lighting->directional_x = -0.4f;
    lighting->directional_y = -1.0f;
    lighting->directional_z = -0.3f;
    lighting->directional_energy = 1.6f;
    lighting->directional_red = lighting->directional_green = lighting->directional_blue = 1.0f;
    lighting->spot_range = 8.0f;
    lighting->spot_falloff = 1.0f;
    lighting->spot_inner_cutoff = 1.0f;
    BTBHGPUScenePost *post = &scene->post;
    post->tonemap_white = 1.0f;
    post->exposure = 1.0f;
    post->contrast = 1.0f;
    post->saturation = 1.0f;
    post->blur_curve = 1.0f;
    post->tonemap_mode = BTBH_GPU_SCENE_TONEMAP_REINHARD;
    post->upscale_mode = BTBH_GPU_SCENE_UPSCALE_LINEAR;
    instance_at(scene->instances[0], 0.0f, -0.05f, 0.0f, 3.0f, 0.1f, 3.0f);
    instance_at(scene->instances[1], 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f);
    instance_at(scene->instances[2], -0.4f, 0.0f, -1.2f, 0.6f, 0.6f, 0.6f);
    BTBHGPUSceneGeometryBatch ground = base_batch(smoke->ground, 0u);
    ground.albedo_red = 120u;
    ground.albedo_green = 120u;
    ground.albedo_blue = 110u;
    scene->batches[0] = ground;
    scene->batches[1] = base_batch(mesh, 1u);
    scene->batches[2] = base_batch(mesh, 2u);
    scene->batch_count = 3u;
}

static BTBHGPUResult render_scene(SmokeWindow *smoke, const Scene *scene, BTBHGPUSceneReport *report) {
    memset(report, 0, sizeof(*report));
    return btbh_gpu_window_scene_render_to_target(
        smoke->window, smoke->target,
        shader_paths[0], shader_paths[1], shader_paths[2], shader_paths[3],
        shader_paths[4], shader_paths[5], shader_paths[6], shader_paths[7],
        shader_paths[8], shader_paths[9], shader_paths[10],
        SMOKE_SHADER_FORMAT, TARGET_SIZE, TARGET_SIZE,
        scene->camera.view_projection, scene->camera.right, scene->camera.up, scene->camera.position,
        &scene->lighting, &scene->post, &scene->instances[0][0], 3u, 3u,
        scene->batches, NULL, NULL, scene->batch_count,
        20u, 24u, 30u, 255u, report);
}

/* `mesh` drawn into the target and read back. */
static uint8_t *render(SmokeWindow *smoke, BTBHGPUCustomMesh *mesh) {
    Scene scene;
    build_scene(smoke, mesh, &scene);
    BTBHGPUSceneReport report;
    const BTBHGPUResult result = render_scene(smoke, &scene, &report);
    if (result != BTBH_GPU_OK) {
        fprintf(stderr, "render: %s\n", report.message);
    }
    assert(result == BTBH_GPU_OK);
    return read_texture(smoke->window, smoke->target);
}

/* A mesh made with `data` (buffers of its own), drawn: what an updated mesh must look like. */
static uint8_t *reference(SmokeWindow *smoke, const MeshData *data) {
    BTBHGPUCustomMesh *fresh = create_mesh(smoke->window, data, false);
    uint8_t *pixels = render(smoke, fresh);
    btbh_gpu_custom_mesh_destroy(fresh);
    return pixels;
}

static void present_scene(SmokeWindow *smoke, BTBHGPUCustomMesh *mesh) {
    Scene scene;
    build_scene(smoke, mesh, &scene);
    BTBHGPUSceneReport report;
    memset(&report, 0, sizeof(report));
    const BTBHGPUResult result = btbh_gpu_window_scene_present_instances(smoke->window,
        shader_paths[0], shader_paths[1], shader_paths[2], shader_paths[3],
        shader_paths[4], shader_paths[5], shader_paths[6], shader_paths[7],
        shader_paths[8], shader_paths[9], shader_paths[10],
        SMOKE_SHADER_FORMAT, 160u, 90u,
        scene.camera.view_projection, scene.camera.right, scene.camera.up, scene.camera.position,
        &scene.lighting, &scene.post, &scene.instances[0][0], 3u, 3u, scene.batches, scene.batch_count,
        20u, 24u, 30u, 255u, &report);
    if (result != BTBH_GPU_OK) {
        fprintf(stderr, "present: %s\n", report.message);
    }
    assert(result == BTBH_GPU_OK);
}

/* The presented scene, captured (btbh_gpu_window_capture_pending_scene_texture submits the frame's command buffer
   and copies its finished scene) and read back; the frame still waits for the canvas. */
static uint8_t *capture_frame(SmokeWindow *smoke, uint32_t *byte_count) {
    BTBHGPUTextureReport report;
    memset(&report, 0, sizeof(report));
    BTBHGPUTexture *capture = btbh_gpu_window_capture_pending_scene_texture(smoke->window, &report);
    if (capture == NULL) {
        fprintf(stderr, "capture: %s\n", report.message);
    }
    assert(capture != NULL);
    uint8_t *pixels = read_texture(smoke->window, capture);
    *byte_count = capture->width * capture->height * 4u;
    btbh_gpu_texture_destroy(capture);
    return pixels;
}

/* The canvas submits the frame. */
static void finish_frame(SmokeWindow *smoke) {
    BTBHGPUCanvasPass pass;
    memset(&pass, 0, sizeof(pass));
    BTBHGPUCanvasReport canvas_report;
    memset(&canvas_report, 0, sizeof(canvas_report));
    const BTBHGPUResult canvas = btbh_gpu_window_canvas_present(smoke->window, canvas_shader_paths[0],
        canvas_shader_paths[1], canvas_shader_paths[2], SMOKE_SHADER_FORMAT, NULL, 0u, NULL, 0u, &pass, 1u,
        &canvas_report);
    if (canvas != BTBH_GPU_OK) {
        fprintf(stderr, "canvas: %s\n", canvas_report.message);
    }
    assert(canvas == BTBH_GPU_OK);
}

static void expect_same(const uint8_t *a, const uint8_t *b, const char *what) {
    const bool same = memcmp(a, b, (size_t)TEXELS * 4u) == 0;
    printf("%-78s %s\n", what, same ? "same bytes" : "DIFFERENT");
    assert(same);
}

/* ------------------------------------------------------------------------------------------------- checks */

static void check_queue(SmokeWindow *smoke) {
    MeshData shapes[4];
    uint8_t *expected[4];
    for (uint32_t which = 0u; which < 4u; ++which) {
        shapes[which] = shape(which);
        expected[which] = reference(smoke, &shapes[which]);
    }
    assert(memcmp(expected[0], expected[1], (size_t)TEXELS * 4u) != 0);
    assert(memcmp(expected[1], expected[2], (size_t)TEXELS * 4u) != 0);
    BTBHGPUCustomMeshQueue *queue = &smoke->window->mesh_queue;

    /* Buffers of its own. */
    BTBHGPUCustomMesh *own = create_mesh(smoke->window, &shapes[0], false);
    uint8_t *pixels = render(smoke, own);
    expect_same(pixels, expected[0], "own mesh as made");
    free(pixels);
    update(own, &shapes[1], true);
    assert(queue->live_count == 1u && own->queue_entry == 1u);
    assert(own->vertex_count == shapes[1].vertex_count);   /* the counts change at the call */
    pixels = render(smoke, own);
    assert(queue->live_count == 0u && queue->entry_count == 0u && own->queue_entry == 0u);
    expect_same(pixels, expected[1], "own mesh, a queued update, the next render-to-target");
    free(pixels);
    pixels = render(smoke, own);
    expect_same(pixels, expected[1], "own mesh, and the render after it");
    free(pixels);
    update(own, &shapes[3], true);
    update(own, &shapes[0], true);
    update(own, &shapes[1], true);
    assert(queue->live_count == 1u && queue->entry_count == 3u);
    pixels = render(smoke, own);
    expect_same(pixels, expected[1], "own mesh, three queued updates: the last one");
    free(pixels);
    update(own, &shapes[0], true);
    update(own, &shapes[3], false);
    assert(queue->live_count == 0u && own->queue_entry == 0u);
    pixels = render(smoke, own);
    expect_same(pixels, expected[3], "own mesh, a queued update then a plain one: the plain one");
    free(pixels);
    update(own, &shapes[1], false);
    update(own, &shapes[0], true);
    pixels = render(smoke, own);
    expect_same(pixels, expected[0], "own mesh, a plain update then a queued one: the queued one");
    free(pixels);
    const uint32_t capacity = own->vertex_buffer_capacity;
    update(own, &shapes[2], true);
    assert(own->vertex_buffer_capacity > capacity);
    pixels = render(smoke, own);
    expect_same(pixels, expected[2], "own mesh grown by a queued update (twice the vertices)");
    free(pixels);

    /* In the shared buffers: in place, within its ranges. */
    BTBHGPUCustomMesh *shared = create_mesh(smoke->window, &shapes[0], true);
    update(shared, &shapes[1], true);
    pixels = render(smoke, shared);
    expect_same(pixels, expected[1], "shared mesh, a queued update");
    free(pixels);
    BTBHGPUCustomMeshReport report;
    memset(&report, 0, sizeof(report));
    assert(btbh_gpu_custom_mesh_update_ex(shared, shapes[2].vertices, bytes_of(&shapes[2]), shapes[2].indices,
        shapes[2].index_count, BTBH_GPU_CUSTOM_MESH_UPDATE_QUEUED, &report) == BTBH_GPU_INVALID_ARGUMENT);
    printf("refused a queued update past a shared mesh's ranges: %s\n", report.message);
    assert(strstr(report.message, "fit") != NULL && queue->live_count == 0u);
    assert(shared->vertex_count == shapes[1].vertex_count);
    pixels = render(smoke, shared);
    expect_same(pixels, expected[1], "shared mesh after the refused update: as it was");
    free(pixels);
    update(shared, &shapes[3], true);
    pixels = render(smoke, shared);
    expect_same(pixels, expected[3], "shared mesh, another queued update");
    free(pixels);

    /* The present: the next frame to the window shows the update (its scene captured). */
    uint32_t frame_bytes = 0u;
    uint32_t reference_bytes = 0u;
    BTBHGPUCustomMesh *fresh = create_mesh(smoke->window, &shapes[1], false);
    present_scene(smoke, fresh);
    uint8_t *presented_reference = capture_frame(smoke, &reference_bytes);
    finish_frame(smoke);
    btbh_gpu_custom_mesh_destroy(fresh);
    update(own, &shapes[1], true);
    present_scene(smoke, own);
    uint8_t *presented = capture_frame(smoke, &frame_bytes);
    finish_frame(smoke);
    assert(frame_bytes == reference_bytes);
    printf("%-78s %s\n", "own mesh, a queued update, the next present", memcmp(presented, presented_reference,
        frame_bytes) == 0 ? "same bytes" : "DIFFERENT");
    assert(memcmp(presented, presented_reference, frame_bytes) == 0);
    free(presented);

    /* The ordering rule: a plain update between a present that copied a queued one and the canvas lands after it. */
    for (uint32_t kind = 0u; kind < 2u; ++kind) {
        BTBHGPUCustomMesh *mesh = kind == 0u ? shared : own;
        update(mesh, &shapes[1], true);
        present_scene(smoke, mesh);
        assert(queue->pending_serial != 0u && mesh->queue_copied_serial == queue->pending_serial);
        update(mesh, &shapes[0], false);
        presented = capture_frame(smoke, &frame_bytes);
        finish_frame(smoke);
        assert(queue->pending_serial != 0u);
        const bool frame_kept = memcmp(presented, presented_reference, frame_bytes) == 0;
        printf("%-78s %s\n", kind == 0u
            ? "shared mesh: queued, presented, a plain update before the canvas: the frame"
            : "own mesh: queued, presented, a plain update before the canvas: the frame",
            frame_kept ? "shows the queued data" : "DOESN'T SHOW THE QUEUED DATA");
        assert(frame_kept);
        free(presented);
        pixels = render(smoke, mesh);
        expect_same(pixels, expected[0], "  and the next scene shows the plain update");
        free(pixels);
    }
    free(presented_reference);
    assert(queue->pending_serial == 0u);   /* the render-to-target noted no queued copy waits */

    /* A mesh destroyed with an update queued; the others still land. */
    BTBHGPUCustomMesh *doomed = create_mesh(smoke->window, &shapes[0], false);
    update(doomed, &shapes[2], true);
    update(own, &shapes[3], true);
    btbh_gpu_custom_mesh_destroy(doomed);
    assert(queue->live_count == 1u && queue->entry_count == 2u);
    pixels = render(smoke, own);
    expect_same(pixels, expected[3], "a mesh destroyed with an update queued; another's still lands");
    free(pixels);

    /* A scene that fails before it draws leaves the queue for the next one. */
    update(own, &shapes[1], true);
    {
        Scene scene;
        build_scene(smoke, own, &scene);
        scene.batches[2].instance_count = 0u;
        BTBHGPUSceneReport scene_report;
        assert(render_scene(smoke, &scene, &scene_report) == BTBH_GPU_INVALID_ARGUMENT);
        assert(queue->live_count == 1u);
    }
    pixels = render(smoke, own);
    expect_same(pixels, expected[1], "a failed scene, then the next one shows the update");
    free(pixels);

    /* Refusals: nothing queued. */
    memset(&report, 0, sizeof(report));
    assert(btbh_gpu_custom_mesh_update_ex(own, shapes[0].vertices, bytes_of(&shapes[0]), shapes[0].indices,
        shapes[0].index_count, 1u << 1, &report) == BTBH_GPU_INVALID_ARGUMENT);
    printf("refused an unknown update flag: %s\n", report.message);
    assert(strstr(report.message, "flags") != NULL);
    assert(btbh_gpu_custom_mesh_update_ex(own, shapes[0].vertices, bytes_of(&shapes[0]), shapes[0].indices,
        shapes[0].index_count, BTBH_GPU_CUSTOM_MESH_UPDATE_QUEUED, NULL) == BTBH_GPU_INVALID_ARGUMENT);
    {
        uint32_t bad[36];
        memcpy(bad, shapes[0].indices, sizeof(bad));
        bad[4] = shapes[0].vertex_count;
        memset(&report, 0, sizeof(report));
        assert(btbh_gpu_custom_mesh_update_ex(own, shapes[0].vertices, bytes_of(&shapes[0]), bad, 36u,
            BTBH_GPU_CUSTOM_MESH_UPDATE_QUEUED, &report) == BTBH_GPU_INVALID_ARGUMENT);
        printf("refused a queued update with an index past the vertices: %s\n", report.message);
    }
    assert(queue->live_count == 0u && queue->entry_count == 0u);

    btbh_gpu_custom_mesh_destroy(own);
    btbh_gpu_custom_mesh_destroy(shared);
    for (uint32_t which = 0u; which < 4u; ++which) {
        free(expected[which]);
    }
}

static void check_closed_window(BTBHGPUDevice *device) {
    const BTBHGPUWindowDescriptor descriptor = {
        .title = "BTBH mesh queue smoke (closed)",
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
    MeshData a = shape(0);
    MeshData b = shape(1);
    BTBHGPUCustomMesh *own = create_mesh(window, &a, false);
    BTBHGPUCustomMesh *shared = create_mesh(window, &a, true);
    update(own, &b, true);
    update(shared, &b, true);
    btbh_gpu_window_destroy(window);
    assert(own->owner == NULL && own->queue_entry == 0u && shared->queue_entry == 0u);
    BTBHGPUCustomMeshReport report;
    memset(&report, 0, sizeof(report));
    assert(btbh_gpu_custom_mesh_update_ex(own, b.vertices, bytes_of(&b), b.indices, b.index_count,
        BTBH_GPU_CUSTOM_MESH_UPDATE_QUEUED, &report) == BTBH_GPU_INVALID_ARGUMENT);
    btbh_gpu_custom_mesh_destroy(own);
    btbh_gpu_custom_mesh_destroy(shared);
    printf("a window closed with updates queued: dropped; its meshes destroyed after it\n");
}

/* ------------------------------------------------------------------------------------------------- bench */

#define BENCH_MESHES 200u
#define BENCH_VERTICES 64u

static int compare_doubles(const void *a, const void *b) {
    const double x = *(const double *)a;
    const double y = *(const double *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

typedef struct BenchMode {
    const char *name;
    bool queued;
    bool shared;
} BenchMode;

static void run_bench(SmokeWindow *smoke) {
    static const BenchMode modes[3] = {
        { "plain", false, false },
        { "queued", true, false },
        { "queued, shared", true, true },
    };
    enum { ROUNDS = 12, FRAMES = 10 };
    /* 200 meshes of 64 vertices (a strip of 31 quads, 186 indices), each a batch of the scene. */
    static float vertices[BENCH_VERTICES * FLOATS];
    static uint32_t indices[(BENCH_VERTICES / 2u - 1u) * 6u];
    const uint32_t index_count = (BENCH_VERTICES / 2u - 1u) * 6u;
    for (uint32_t q = 0u; q + 1u < BENCH_VERTICES / 2u; ++q) {
        const uint32_t a = q * 2u;
        const uint32_t wound[6] = { a, a + 2u, a + 1u, a + 1u, a + 2u, a + 3u };
        memcpy(&indices[q * 6u], wound, sizeof(wound));
    }
    BTBHGPUCustomMesh *meshes[3][BENCH_MESHES];
    memset(vertices, 0, sizeof(vertices));
    for (uint32_t v = 0u; v < BENCH_VERTICES; ++v) {
        vertices[v * FLOATS + 0u] = 0.02f * (float)(v / 2u);
        vertices[v * FLOATS + 2u] = 0.05f * (float)(v % 2u);
        vertices[v * FLOATS + 4u] = 1.0f;
        vertices[v * FLOATS + 8u] = 1.0f;
        vertices[v * FLOATS + 11u] = 1.0f;
        vertices[v * FLOATS + 12u] = vertices[v * FLOATS + 13u] = vertices[v * FLOATS + 14u] = 1.0f;
        vertices[v * FLOATS + 15u] = 1.0f;
        vertices[v * FLOATS + 20u] = 1.0f;
    }
    for (uint32_t mode = 0u; mode < 3u; ++mode) {
        for (uint32_t index = 0u; index < BENCH_MESHES; ++index) {
            BTBHGPUCustomMeshReport report;
            memset(&report, 0, sizeof(report));
            meshes[mode][index] = btbh_gpu_window_custom_mesh_create_ex(smoke->window, vertices,
                BENCH_VERTICES * FLOATS * (uint32_t)sizeof(float), BENCH_VERTICES, indices, index_count,
                modes[mode].shared ? BTBH_GPU_CUSTOM_MESH_SHARED_BUFFERS : 0u, &report);
            assert(meshes[mode][index] != NULL);
        }
    }
    static float instances[BENCH_MESHES][FLOATS];
    static BTBHGPUSceneGeometryBatch batches[BENCH_MESHES];
    for (uint32_t index = 0u; index < BENCH_MESHES; ++index) {
        instance_at(instances[index], -2.0f + 0.2f * (float)(index % 20u), 0.0f, -2.0f + 0.4f * (float)(index / 20u),
            1.0f, 1.0f, 1.0f);
    }
    Scene scene;
    build_scene(smoke, smoke->ground, &scene);
    static double update_ms[3][ROUNDS * FRAMES];
    static double present_ms[3][ROUNDS * FRAMES];
    uint32_t samples[3] = { 0u, 0u, 0u };
    for (uint32_t round = 0u; round < ROUNDS; ++round) {
        for (uint32_t step = 0u; step < 3u; ++step) {
            const uint32_t mode = (round + step) % 3u;
            for (uint32_t index = 0u; index < BENCH_MESHES; ++index) {
                batches[index] = base_batch(meshes[mode][index], index);
            }
            for (uint32_t frame = 0u; frame < FRAMES + 2u; ++frame) {
                /* Each mark moves a little every frame. */
                const uint64_t update_started = SDL_GetTicksNS();
                for (uint32_t index = 0u; index < BENCH_MESHES; ++index) {
                    vertices[1] = 0.001f * (float)(frame + index);
                    BTBHGPUCustomMeshReport report;
                    const BTBHGPUResult result = btbh_gpu_custom_mesh_update_ex(meshes[mode][index], vertices,
                        BENCH_VERTICES * FLOATS * (uint32_t)sizeof(float), indices, index_count,
                        modes[mode].queued ? BTBH_GPU_CUSTOM_MESH_UPDATE_QUEUED : 0u, &report);
                    assert(result == BTBH_GPU_OK);
                }
                const uint64_t present_started = SDL_GetTicksNS();
                BTBHGPUSceneReport report;
                memset(&report, 0, sizeof(report));
                const BTBHGPUResult result = btbh_gpu_window_scene_present_instances(smoke->window,
                    shader_paths[0], shader_paths[1], shader_paths[2], shader_paths[3],
                    shader_paths[4], shader_paths[5], shader_paths[6], shader_paths[7],
                    shader_paths[8], shader_paths[9], shader_paths[10],
                    SMOKE_SHADER_FORMAT, 320u, 180u,
                    scene.camera.view_projection, scene.camera.right, scene.camera.up, scene.camera.position,
                    &scene.lighting, &scene.post, &instances[0][0], BENCH_MESHES, BENCH_MESHES, batches,
                    BENCH_MESHES, 20u, 24u, 30u, 255u, &report);
                assert(result == BTBH_GPU_OK);
                const uint64_t present_finished = SDL_GetTicksNS();
                finish_frame(smoke);
                if (frame >= 2u) {
                    update_ms[mode][samples[mode]] = (double)(present_started - update_started) / 1.0e6;
                    present_ms[mode][samples[mode]] = (double)(present_finished - present_started) / 1.0e6;
                    samples[mode] += 1u;
                }
            }
        }
    }
    double median_update[3];
    double median_present[3];
    for (uint32_t mode = 0u; mode < 3u; ++mode) {
        qsort(update_ms[mode], samples[mode], sizeof(double), compare_doubles);
        qsort(present_ms[mode], samples[mode], sizeof(double), compare_doubles);
        median_update[mode] = update_ms[mode][samples[mode] / 2u];
        median_present[mode] = present_ms[mode][samples[mode] / 2u];
        printf("bench %-15s %u updates of %u vertices a frame, %u frames: updates %.3f ms (p10 %.3f, p90 %.3f), "
            "%.2f us each; present %.3f ms (p10 %.3f)\n",
            modes[mode].name, BENCH_MESHES, BENCH_VERTICES, samples[mode], median_update[mode],
            update_ms[mode][samples[mode] / 10u], update_ms[mode][samples[mode] * 9u / 10u],
            1000.0 * median_update[mode] / BENCH_MESHES, median_present[mode],
            present_ms[mode][samples[mode] / 10u]);
    }
    for (uint32_t mode = 1u; mode < 3u; ++mode) {
        printf("bench %-15s updates + present per update: %.2f us (plain %.2f us)\n", modes[mode].name,
            1000.0 * (median_update[mode] + median_present[mode] - median_present[0]) / BENCH_MESHES,
            1000.0 * median_update[0] / BENCH_MESHES);
    }
    for (uint32_t mode = 0u; mode < 3u; ++mode) {
        for (uint32_t index = 0u; index < BENCH_MESHES; ++index) {
            btbh_gpu_custom_mesh_destroy(meshes[mode][index]);
        }
    }
}

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
    static const char *const canvas_names[3] = {
        "production_canvas.vert", "production_canvas.frag", "production_canvas_textured.frag",
    };
    for (int index = 0; index < 3; ++index) {
        (void)snprintf(canvas_shader_paths[index], sizeof(canvas_shader_paths[index]), "%s/%s%s",
            BTBH_GPU_SMOKE_SHADER_DIR, canvas_names[index], SMOKE_SHADER_EXTENSION);
    }
    return true;
}

int main(void) {
    (void)setvbuf(stdout, NULL, _IONBF, 0);
    if (!load_shader_paths()) {
        return SMOKE_SKIP;
    }
    (void)SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    const char *bench = SDL_getenv("BTBH_SMOKE_BENCH");
    const bool benching = bench != NULL && strcmp(bench, "mesh-queue") == 0;
    const BTBHGPUDeviceDescriptor device_descriptor = {
        .backend = SMOKE_BACKEND,
        .required_features = BTBH_GPU_REQUIRED_FEATURES,
        /* (The bench measures without the debug device, as a game runs.) */
        .debug_mode = benching ? 0u : 1u,
    };
    BTBHGPUDeviceReport device_report;
    memset(&device_report, 0, sizeof(device_report));
    BTBHGPUDevice *device = btbh_gpu_device_create(&device_descriptor, &device_report);
    if (device == NULL) {
        fprintf(stderr, "skipped: no device (%s)\n", device_report.message);
        return SMOKE_SKIP;
    }
    SmokeWindow smoke;
    memset(&smoke, 0, sizeof(smoke));
    const BTBHGPUWindowDescriptor descriptor = {
        .title = "BTBH mesh queue smoke",
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
    MeshData slab = shape(3);
    smoke.ground = create_mesh(smoke.window, &slab, false);
    if (benching) {
        run_bench(&smoke);
    } else {
        check_queue(&smoke);
        check_closed_window(device);
    }
    btbh_gpu_custom_mesh_destroy(smoke.ground);
    btbh_gpu_texture_destroy(smoke.target);
    btbh_gpu_window_destroy(smoke.window);
    btbh_gpu_device_destroy(device);
    puts("btbh_gpu_mesh_queue_smoke: passed");
    return 0;
}
