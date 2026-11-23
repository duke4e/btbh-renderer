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
 * Queued instance buffer uploads (BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED, _NO_CYCLE; 0.11.0): an instance buffer drawn
 * by two batches, one per half, byte for byte as a fresh buffer uploaded whole with the data it must hold:
 * - a queued half keeps the other half (a frame in flight on the buffer before it); overlapping queued uploads in one
 *   frame (each later one wins, the queue's entries checked); NO_CYCLE and plain uploads after queued ones, and queued
 *   after plain;
 * - a render-to-target first, then the present (and the other way round); a presented frame waiting for the canvas
 *   never shows an upload made after it (queued, NO_CYCLE, the ordering rule, a plain cycling one); frames in flight
 *   (submitted, not waited for) keep their data;
 * - a buffer destroyed with an upload queued, and after a present copied one; a failed scene; a frame dropped after it
 *   copied queued uploads still lands them; the refusals; a window closed with uploads queued.
 * Partial custom-mesh updates (btbh_gpu_custom_mesh_update_range; 0.11.0), for a mesh with buffers of its own and one in
 * the shared buffers: plain and queued vertex ranges, indices only, overlapping ranges in one frame, a range over a
 * queued whole update and a whole update over a queued range, the bounds, a frame waiting for the canvas, the refusals.
 * BTBH_SMOKE_BENCH=mesh-queue: 200 updates of a 64-vertex mesh a frame, plain against queued (and in the shared
 * buffers), against the same frames without updates ("still"), interleaved rounds; prints the CPU time of the updates,
 * of the present and of its upload, per update.
 * BTBH_SMOKE_BENCH=upload-queue: 60 instance buffers of 4 to 64 KiB uploaded every frame: whole and plain, whole and
 * queued, a quarter queued, a quarter NO_CYCLE, against no uploads; per upload and per frame.
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

static BTBHGPUResult render_scene_to(
    SmokeWindow *smoke,
    BTBHGPUTexture *target,
    const Scene *scene,
    BTBHGPUSceneReport *report
) {
    memset(report, 0, sizeof(*report));
    return btbh_gpu_window_scene_render_to_target(
        smoke->window, target,
        shader_paths[0], shader_paths[1], shader_paths[2], shader_paths[3],
        shader_paths[4], shader_paths[5], shader_paths[6], shader_paths[7],
        shader_paths[8], shader_paths[9], shader_paths[10],
        SMOKE_SHADER_FORMAT, TARGET_SIZE, TARGET_SIZE,
        scene->camera.view_projection, scene->camera.right, scene->camera.up, scene->camera.position,
        &scene->lighting, &scene->post, &scene->instances[0][0], 3u, 3u,
        scene->batches, NULL, NULL, scene->batch_count,
        20u, 24u, 30u, 255u, report);
}

static BTBHGPUResult render_scene(SmokeWindow *smoke, const Scene *scene, BTBHGPUSceneReport *report) {
    return render_scene_to(smoke, smoke->target, scene, report);
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

static BTBHGPUResult try_present(SmokeWindow *smoke, const Scene *scene, BTBHGPUSceneReport *report) {
    memset(report, 0, sizeof(*report));
    return btbh_gpu_window_scene_present_instances(smoke->window,
        shader_paths[0], shader_paths[1], shader_paths[2], shader_paths[3],
        shader_paths[4], shader_paths[5], shader_paths[6], shader_paths[7],
        shader_paths[8], shader_paths[9], shader_paths[10],
        SMOKE_SHADER_FORMAT, 160u, 90u,
        scene->camera.view_projection, scene->camera.right, scene->camera.up, scene->camera.position,
        &scene->lighting, &scene->post, &scene->instances[0][0], 3u, 3u, scene->batches, scene->batch_count,
        20u, 24u, 30u, 255u, report);
}

static void present_built(SmokeWindow *smoke, const Scene *scene) {
    BTBHGPUSceneReport report;
    const BTBHGPUResult result = try_present(smoke, scene, &report);
    if (result != BTBH_GPU_OK) {
        fprintf(stderr, "present: %s\n", report.message);
    }
    assert(result == BTBH_GPU_OK);
}

static void present_scene(SmokeWindow *smoke, BTBHGPUCustomMesh *mesh) {
    Scene scene;
    build_scene(smoke, mesh, &scene);
    present_built(smoke, &scene);
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
    BTBHGPUUploadQueue *queue = &smoke->window->upload_queue;

    /* Buffers of its own. */
    BTBHGPUCustomMesh *own = create_mesh(smoke->window, &shapes[0], false);
    uint8_t *pixels = render(smoke, own);
    expect_same(pixels, expected[0], "own mesh as made");
    free(pixels);
    update(own, &shapes[1], true);
    assert(queue->live_count == 2u && own->queue_head == 2u);   /* its vertices and its indices */
    assert(own->vertex_count == shapes[1].vertex_count);   /* the counts change at the call */
    pixels = render(smoke, own);
    assert(queue->live_count == 0u && queue->entry_count == 0u && own->queue_head == 0u);
    expect_same(pixels, expected[1], "own mesh, a queued update, the next render-to-target");
    free(pixels);
    pixels = render(smoke, own);
    expect_same(pixels, expected[1], "own mesh, and the render after it");
    free(pixels);
    update(own, &shapes[3], true);
    update(own, &shapes[0], true);
    update(own, &shapes[1], true);
    assert(queue->live_count == 2u && queue->entry_count == 6u);
    pixels = render(smoke, own);
    expect_same(pixels, expected[1], "own mesh, three queued updates: the last one");
    free(pixels);
    update(own, &shapes[0], true);
    update(own, &shapes[3], false);
    assert(queue->live_count == 0u && own->queue_head == 0u);
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
    update(doomed, &shapes[1], true);   /* replaces the first: its entries are dropped */
    update(own, &shapes[3], true);
    const uintptr_t doomed_address = (uintptr_t)doomed;
    btbh_gpu_custom_mesh_destroy(doomed);
    assert(queue->live_count == 2u && queue->entry_count == 6u);
    for (uint32_t index = 0u; index < queue->entry_count; ++index) {
        assert((uintptr_t)queue->entries[index].mesh != doomed_address);   /* no entry names it any more */
    }
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
        assert(queue->live_count == 2u);
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

/* ------------------------------------------------------------------------------------------------- instance uploads */

/* BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED and _NO_CYCLE: an instance buffer of 8 boxes, drawn by two batches (its
   first and its second half), against a fresh buffer uploaded whole with the data it must hold. */

#define CHUNK_INSTANCES 8u
#define INSTANCE_BYTES (FLOATS * (uint32_t)sizeof(float))
#define CHUNK_BYTES (CHUNK_INSTANCES * INSTANCE_BYTES)

typedef struct Chunk {
    float records[CHUNK_INSTANCES][FLOATS];
} Chunk;

/* State `which` of the 8 instances: a row of boxes, each state's row moved, stretched and tinted its own way. */
static Chunk chunk_state(uint32_t which) {
    static const float tints[5][3] = {
        { 1.0f, 0.35f, 0.25f }, { 0.3f, 1.0f, 0.4f }, { 0.35f, 0.5f, 1.0f }, { 1.0f, 0.95f, 0.3f }, { 0.9f, 0.3f, 1.0f },
    };
    Chunk chunk;
    for (uint32_t index = 0u; index < CHUNK_INSTANCES; ++index) {
        instance_at(chunk.records[index], -1.05f + 0.3f * (float)index, 0.0f, -0.6f + 0.35f * (float)which,
            1.0f, 1.0f + 0.5f * (float)((index + which) % 3u), 1.0f);
        chunk.records[index][16] = tints[which % 5u][0];
        chunk.records[index][17] = tints[which % 5u][1];
        chunk.records[index][18] = tints[which % 5u][2];
    }
    return chunk;
}

/* `base` with instances [first, first + count) taken from `over`. */
static Chunk chunk_mix(const Chunk *base, const Chunk *over, uint32_t first, uint32_t count) {
    Chunk chunk = *base;
    memcpy(chunk.records[first], over->records[first], (size_t)count * INSTANCE_BYTES);
    return chunk;
}

static BTBHGPUInstanceBuffer *chunk_buffer(BTBHGPUWindow *window) {
    BTBHGPUInstanceBufferReport report;
    memset(&report, 0, sizeof(report));
    BTBHGPUInstanceBuffer *buffer = btbh_gpu_window_instance_buffer_create(window, CHUNK_INSTANCES, INSTANCE_BYTES,
        &report);
    assert(buffer != NULL);
    return buffer;
}

/* Instances [first, first + count) of `chunk` into `buffer`, with BTBH_GPU_INSTANCE_BUFFER_UPLOAD_* `flags`. */
static void upload(BTBHGPUInstanceBuffer *buffer, const Chunk *chunk, uint32_t first, uint32_t count, uint32_t flags) {
    BTBHGPUInstanceBufferReport report;
    memset(&report, 0, sizeof(report));
    const BTBHGPUResult result = btbh_gpu_instance_buffer_upload_ex(buffer, first * INSTANCE_BYTES,
        chunk->records[first], count * INSTANCE_BYTES, flags, &report);
    if (result != BTBH_GPU_OK) {
        fprintf(stderr, "upload: %s\n", report.message);
    }
    assert(result == BTBH_GPU_OK);
    assert(report.byte_capacity == CHUNK_BYTES);
}

/* The ground, and `box` drawn by the buffer's instances: batch 1 its first half, batch 2 its second. */
static void build_chunk_scene(const SmokeWindow *smoke, BTBHGPUCustomMesh *box, BTBHGPUInstanceBuffer *buffer,
    Scene *scene) {
    build_scene(smoke, box, scene);
    scene->batches[1] = base_batch(box, 0u);
    scene->batches[1].instance_buffer = buffer;
    scene->batches[1].instance_count = CHUNK_INSTANCES / 2u;
    scene->batches[2] = base_batch(box, CHUNK_INSTANCES / 2u);
    scene->batches[2].instance_buffer = buffer;
    scene->batches[2].instance_count = CHUNK_INSTANCES / 2u;
}

/* Renders the chunk into `target` (submitted, not waited for). */
static void render_chunk_to(SmokeWindow *smoke, BTBHGPUTexture *target, BTBHGPUCustomMesh *box,
    BTBHGPUInstanceBuffer *buffer) {
    Scene scene;
    build_chunk_scene(smoke, box, buffer, &scene);
    BTBHGPUSceneReport report;
    const BTBHGPUResult result = render_scene_to(smoke, target, &scene, &report);
    if (result != BTBH_GPU_OK) {
        fprintf(stderr, "render: %s\n", report.message);
    }
    assert(result == BTBH_GPU_OK);
}

static uint8_t *render_chunk(SmokeWindow *smoke, BTBHGPUCustomMesh *box, BTBHGPUInstanceBuffer *buffer) {
    render_chunk_to(smoke, smoke->target, box, buffer);
    return read_texture(smoke->window, smoke->target);
}

/* What the chunk must look like: a fresh buffer uploaded whole with `chunk`, drawn. */
static uint8_t *chunk_reference(SmokeWindow *smoke, BTBHGPUCustomMesh *box, const Chunk *chunk) {
    BTBHGPUInstanceBuffer *fresh = chunk_buffer(smoke->window);
    upload(fresh, chunk, 0u, CHUNK_INSTANCES, 0u);
    uint8_t *pixels = render_chunk(smoke, box, fresh);
    btbh_gpu_instance_buffer_destroy(fresh);
    return pixels;
}

static void expect_chunk(SmokeWindow *smoke, BTBHGPUCustomMesh *box, BTBHGPUInstanceBuffer *buffer,
    const Chunk *chunk, const char *what) {
    uint8_t *pixels = render_chunk(smoke, box, buffer);
    uint8_t *expected = chunk_reference(smoke, box, chunk);
    expect_same(pixels, expected, what);
    free(expected);
    free(pixels);
}

/* The chunk presented (from a fresh buffer holding `chunk`) and captured: what a presented frame must look like. */
static uint8_t *chunk_presented_reference(SmokeWindow *smoke, BTBHGPUCustomMesh *box, const Chunk *chunk,
    uint32_t *byte_count) {
    BTBHGPUInstanceBuffer *fresh = chunk_buffer(smoke->window);
    upload(fresh, chunk, 0u, CHUNK_INSTANCES, 0u);
    Scene scene;
    build_chunk_scene(smoke, box, fresh, &scene);
    present_built(smoke, &scene);
    uint8_t *pixels = capture_frame(smoke, byte_count);
    finish_frame(smoke);
    btbh_gpu_instance_buffer_destroy(fresh);
    return pixels;
}

static void present_chunk(SmokeWindow *smoke, BTBHGPUCustomMesh *box, BTBHGPUInstanceBuffer *buffer) {
    Scene scene;
    build_chunk_scene(smoke, box, buffer, &scene);
    present_built(smoke, &scene);
}

/* Captures the presented frame, lets the canvas submit it, and compares it with the presented reference of `chunk`. */
static void expect_presented_chunk(SmokeWindow *smoke, BTBHGPUCustomMesh *box, const Chunk *chunk, const char *what) {
    uint32_t frame_bytes = 0u;
    uint8_t *presented = capture_frame(smoke, &frame_bytes);
    finish_frame(smoke);
    uint32_t reference_bytes = 0u;
    uint8_t *expected = chunk_presented_reference(smoke, box, chunk, &reference_bytes);
    assert(frame_bytes == reference_bytes);
    const bool same = memcmp(presented, expected, frame_bytes) == 0;
    printf("%-78s %s\n", what, same ? "same bytes" : "DIFFERENT");
    assert(same);
    free(expected);
    free(presented);
}

static void check_instance_uploads(SmokeWindow *smoke) {
    MeshData box_data;
    memset(&box_data, 0, sizeof(box_data));
    add_box(&box_data, (const float[3]){ 0.0f, 0.12f, 0.0f }, (const float[3]){ 0.1f, 0.12f, 0.1f },
        (const float[3]){ 1.0f, 1.0f, 1.0f });
    BTBHGPUCustomMesh *box = create_mesh(smoke->window, &box_data, false);
    BTBHGPUUploadQueue *queue = &smoke->window->upload_queue;
    Chunk states[5];
    for (uint32_t which = 0u; which < 5u; ++which) {
        states[which] = chunk_state(which);
    }
    {
        /* Each half is in view and every state differs in each. */
        uint8_t *a = chunk_reference(smoke, box, &states[0]);
        const Chunk first = chunk_mix(&states[0], &states[1], 0u, CHUNK_INSTANCES / 2u);
        const Chunk second = chunk_mix(&states[0], &states[1], CHUNK_INSTANCES / 2u, CHUNK_INSTANCES / 2u);
        uint8_t *b = chunk_reference(smoke, box, &first);
        uint8_t *c = chunk_reference(smoke, box, &second);
        assert(memcmp(a, b, (size_t)TEXELS * 4u) != 0 && memcmp(a, c, (size_t)TEXELS * 4u) != 0 &&
            memcmp(b, c, (size_t)TEXELS * 4u) != 0);
        free(a);
        free(b);
        free(c);
    }

    BTBHGPUInstanceBuffer *buffer = chunk_buffer(smoke->window);
    upload(buffer, &states[0], 0u, CHUNK_INSTANCES, 0u);
    expect_chunk(smoke, box, buffer, &states[0], "instance buffer uploaded whole (plain)");

    /* Partial queued uploads keep the rest: a frame in flight on the buffer, then its first half queued. */
    render_chunk_to(smoke, smoke->target, box, buffer);
    upload(buffer, &states[1], 0u, 4u, BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED);
    assert(queue->live_count == 1u && buffer->queue_head == 1u);
    Chunk current = chunk_mix(&states[0], &states[1], 0u, 4u);
    expect_chunk(smoke, box, buffer, &current, "queued upload of the first half: the second keeps its data");
    assert(queue->live_count == 0u && queue->entry_count == 0u && buffer->queue_head == 0u);
    assert(buffer->queue_copied_serial != 0u);
    upload(buffer, &states[2], 4u, 4u, BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED);
    current = chunk_mix(&current, &states[2], 4u, 4u);
    expect_chunk(smoke, box, buffer, &current, "then the second half queued: the first keeps its data");
    upload(buffer, &states[3], 2u, 1u, BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED);
    upload(buffer, &states[4], 6u, 1u, BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED);
    assert(queue->live_count == 2u);
    current = chunk_mix(&current, &states[3], 2u, 1u);
    current = chunk_mix(&current, &states[4], 6u, 1u);
    expect_chunk(smoke, box, buffer, &current, "two single instances queued in one frame, one in each half");

    /* Overlapping queued uploads in one frame: the later one's bytes land where they overlap. */
    upload(buffer, &states[3], 0u, 6u, BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED);
    upload(buffer, &states[4], 2u, 2u, BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED);   /* inside the first: it takes them */
    assert(queue->live_count == 1u && queue->entry_count == 1u);
    upload(buffer, &states[0], 5u, 3u, BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED);   /* over its end */
    assert(queue->live_count == 2u && queue->entry_count == 2u);
    upload(buffer, &states[2], 6u, 2u, BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED);   /* inside the second */
    assert(queue->live_count == 2u && queue->entry_count == 2u);
    upload(buffer, &states[1], 4u, 4u, BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED);   /* covers the second */
    assert(queue->live_count == 2u && queue->entry_count == 3u);
    current = chunk_mix(&current, &states[3], 0u, 6u);
    current = chunk_mix(&current, &states[4], 2u, 2u);
    current = chunk_mix(&current, &states[0], 5u, 3u);
    current = chunk_mix(&current, &states[2], 6u, 2u);
    current = chunk_mix(&current, &states[1], 4u, 4u);
    expect_chunk(smoke, box, buffer, &current, "five overlapping queued uploads in one frame: each later one wins");

    /* A later upload outside the queue wins over a queued one. */
    upload(buffer, &states[2], 0u, 8u, BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED);
    upload(buffer, &states[3], 2u, 4u, BTBH_GPU_INSTANCE_BUFFER_UPLOAD_NO_CYCLE);
    current = chunk_mix(&states[2], &states[3], 2u, 4u);
    expect_chunk(smoke, box, buffer, &current, "queued whole, then NO_CYCLE over its middle: the NO_CYCLE bytes there");
    upload(buffer, &states[0], 0u, 8u, BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED);
    upload(buffer, &states[4], 0u, 4u, 0u);
    current = chunk_mix(&states[0], &states[4], 0u, 4u);
    expect_chunk(smoke, box, buffer, &current, "queued whole, then a plain (cycling) upload of a half: its bytes there");
    upload(buffer, &states[1], 4u, 4u, BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED);
    upload(buffer, &states[2], 0u, 8u, 0u);
    assert(queue->live_count == 0u);   /* covered: dropped */
    current = states[2];
    expect_chunk(smoke, box, buffer, &current, "queued half, then a plain upload of the whole: the plain one");
    upload(buffer, &states[3], 0u, 8u, 0u);
    upload(buffer, &states[0], 0u, 4u, BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED);
    current = chunk_mix(&states[3], &states[0], 0u, 4u);
    expect_chunk(smoke, box, buffer, &current, "plain whole, then a queued half: the queued one there, the rest kept");
    upload(buffer, &states[4], 4u, 4u, BTBH_GPU_INSTANCE_BUFFER_UPLOAD_NO_CYCLE);
    current = chunk_mix(&current, &states[4], 4u, 4u);
    expect_chunk(smoke, box, buffer, &current, "NO_CYCLE of a half, no frame waiting: the rest kept");
    upload(buffer, &states[1], 0u, 8u, BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED | BTBH_GPU_INSTANCE_BUFFER_UPLOAD_NO_CYCLE);
    assert(queue->live_count == 1u);   /* QUEUED with NO_CYCLE is queued */
    current = states[1];
    expect_chunk(smoke, box, buffer, &current, "QUEUED | NO_CYCLE: queued");

    /* A render-to-target first: it copies the queue; the present after it shows the data too. */
    upload(buffer, &states[4], 4u, 4u, BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED);
    current = chunk_mix(&current, &states[4], 4u, 4u);
    expect_chunk(smoke, box, buffer, &current, "queued, then a render-to-target draws it");
    present_chunk(smoke, box, buffer);
    expect_presented_chunk(smoke, box, &current, "  and the present after it");
    upload(buffer, &states[2], 0u, 4u, BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED);
    current = chunk_mix(&current, &states[2], 0u, 4u);
    present_chunk(smoke, box, buffer);
    expect_presented_chunk(smoke, box, &current, "queued, then the present first");
    expect_chunk(smoke, box, buffer, &current, "  and the render-to-target after it");

    /* A presented scene waiting for the canvas never shows an upload made after it. */
    const Chunk before = current;
    present_chunk(smoke, box, buffer);
    upload(buffer, &states[3], 0u, 8u, BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED);
    expect_presented_chunk(smoke, box, &before, "presented, then a queued upload before the canvas: the frame");
    current = states[3];
    expect_chunk(smoke, box, buffer, &current, "  and the next scene shows the upload");
    present_chunk(smoke, box, buffer);
    upload(buffer, &states[0], 0u, 4u, BTBH_GPU_INSTANCE_BUFFER_UPLOAD_NO_CYCLE);
    assert(queue->pending_holds);   /* recorded into the frame after its draws */
    expect_presented_chunk(smoke, box, &current, "presented, then a NO_CYCLE upload before the canvas: the frame");
    assert(!queue->pending_holds);
    current = chunk_mix(&current, &states[0], 0u, 4u);
    expect_chunk(smoke, box, buffer, &current, "  and the next scene shows the upload, the rest kept");
    /* The ordering rule: a queued upload the waiting frame copied, then NO_CYCLE over it before the canvas. */
    upload(buffer, &states[1], 0u, 8u, BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED);
    present_chunk(smoke, box, buffer);
    assert(queue->pending_serial != 0u && buffer->queue_copied_serial == queue->pending_serial);
    upload(buffer, &states[2], 4u, 4u, BTBH_GPU_INSTANCE_BUFFER_UPLOAD_NO_CYCLE);
    expect_presented_chunk(smoke, box, &states[1], "queued, presented, NO_CYCLE before the canvas: the frame");
    current = chunk_mix(&states[1], &states[2], 4u, 4u);
    expect_chunk(smoke, box, buffer, &current, "  and the next scene: the NO_CYCLE bytes over the queued ones");
    upload(buffer, &states[4], 0u, 8u, BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED);
    present_chunk(smoke, box, buffer);
    upload(buffer, &states[0], 0u, 8u, 0u);
    expect_presented_chunk(smoke, box, &states[4], "queued, presented, a plain upload before the canvas: the frame");
    current = states[0];
    expect_chunk(smoke, box, buffer, &current, "  and the next scene: the plain one");

    /* Frames in flight (submitted, not waited for) keep the data they were drawn with. */
    {
        BTBHGPUTextureReport texture_report;
        memset(&texture_report, 0, sizeof(texture_report));
        BTBHGPUTexture *second = btbh_gpu_window_render_target_create(smoke->window, TARGET_SIZE, TARGET_SIZE,
            BTBH_GPU_TEXTURE_FILTER_LINEAR, &texture_report);
        assert(second != NULL);
        for (uint32_t way = 0u; way < 2u; ++way) {
            const Chunk old_data = current;
            render_chunk_to(smoke, smoke->target, box, buffer);
            if (way == 0u) {
                upload(buffer, &states[2], 0u, 8u, BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED);
                current = states[2];
            } else {
                upload(buffer, &states[3], 2u, 4u, BTBH_GPU_INSTANCE_BUFFER_UPLOAD_NO_CYCLE);
                current = chunk_mix(&current, &states[3], 2u, 4u);
            }
            render_chunk_to(smoke, second, box, buffer);
            uint8_t *first_frame = read_texture(smoke->window, smoke->target);
            uint8_t *second_frame = read_texture(smoke->window, second);
            uint8_t *old_expected = chunk_reference(smoke, box, &old_data);
            uint8_t *new_expected = chunk_reference(smoke, box, &current);
            expect_same(first_frame, old_expected, way == 0u
                ? "a frame in flight, then a queued upload and a frame: the first keeps the old data"
                : "a frame in flight, then a NO_CYCLE upload and a frame: the first keeps the old data");
            expect_same(second_frame, new_expected, "  and the second shows the new");
            free(first_frame);
            free(second_frame);
            free(old_expected);
            free(new_expected);
        }
        btbh_gpu_texture_destroy(second);
    }

    /* A buffer destroyed with uploads queued: they go, another buffer's still land. */
    BTBHGPUInstanceBuffer *doomed = chunk_buffer(smoke->window);
    upload(doomed, &states[0], 0u, 8u, 0u);
    upload(doomed, &states[1], 0u, 4u, BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED);
    upload(doomed, &states[2], 0u, 8u, 0u);   /* covers it: dropped */
    upload(doomed, &states[3], 2u, 4u, BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED);
    upload(buffer, &states[4], 0u, 8u, BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED);
    const uintptr_t doomed_address = (uintptr_t)doomed;
    btbh_gpu_instance_buffer_destroy(doomed);
    assert(queue->live_count == 1u && queue->entry_count == 3u);
    for (uint32_t index = 0u; index < queue->entry_count; ++index) {
        assert((uintptr_t)queue->entries[index].instance_buffer != doomed_address);
    }
    current = states[4];
    expect_chunk(smoke, box, buffer, &current, "a buffer destroyed with an upload queued; another's still lands");
    /* Destroyed after a present copied its queued upload, before the canvas: that frame still runs (the debug device
       would object to a freed buffer). */
    doomed = chunk_buffer(smoke->window);
    upload(doomed, &states[0], 0u, 8u, 0u);
    upload(doomed, &states[2], 0u, 4u, BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED);
    present_chunk(smoke, box, doomed);
    btbh_gpu_instance_buffer_destroy(doomed);
    const Chunk doomed_data = chunk_mix(&states[0], &states[2], 0u, 4u);
    expect_presented_chunk(smoke, box, &doomed_data,
        "destroyed after a present copied its queued upload: that frame still draws it");

    /* A scene that fails before it draws leaves the queue for the next one. */
    upload(buffer, &states[1], 2u, 4u, BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED);
    {
        Scene scene;
        build_chunk_scene(smoke, box, buffer, &scene);
        scene.batches[2].instance_count = 0u;
        BTBHGPUSceneReport scene_report;
        assert(render_scene(smoke, &scene, &scene_report) == BTBH_GPU_INVALID_ARGUMENT);
        assert(queue->live_count == 1u);
    }
    current = chunk_mix(&current, &states[1], 2u, 4u);
    expect_chunk(smoke, box, buffer, &current, "a failed scene, then the next one shows the queued upload");

    /* A frame that copied queued uploads and is then dropped (the next scene presented without the canvas) is still
       submitted: the uploads land. */
    upload(buffer, &states[3], 0u, 8u, BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED);
    present_chunk(smoke, box, buffer);
    assert(queue->live_count == 0u && queue->pending_holds);
    {
        Scene scene;
        build_chunk_scene(smoke, box, buffer, &scene);
        BTBHGPUSceneReport scene_report;
        assert(try_present(smoke, &scene, &scene_report) == BTBH_GPU_INVALID_ARGUMENT);
        assert(strstr(scene_report.message, "Canvas2D") != NULL && !queue->pending_holds);
        assert(smoke->window->pending_command == NULL);
    }
    current = states[3];
    expect_chunk(smoke, box, buffer, &current, "a dropped frame that copied a queued upload: it still lands");

    /* Refusals: nothing queued, the buffer as it was. */
    {
        BTBHGPUInstanceBufferReport report;
        memset(&report, 0, sizeof(report));
        assert(btbh_gpu_instance_buffer_upload_ex(buffer, 0u, states[0].records[0], CHUNK_BYTES, 1u << 2, &report) ==
            BTBH_GPU_INVALID_ARGUMENT);
        printf("refused an unknown upload flag: %s\n", report.message);
        assert(strstr(report.message, "flags") != NULL);
        assert(btbh_gpu_instance_buffer_upload_ex(buffer, 0u, states[0].records[0], CHUNK_BYTES,
            BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED, NULL) == BTBH_GPU_INVALID_ARGUMENT);
        assert(btbh_gpu_instance_buffer_upload_ex(buffer, INSTANCE_BYTES, states[0].records[0], CHUNK_BYTES,
            BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED, &report) == BTBH_GPU_INVALID_ARGUMENT);
        printf("refused a queued upload past the buffer's end: %s\n", report.message);
        assert(btbh_gpu_instance_buffer_upload_ex(buffer, 0u, NULL, CHUNK_BYTES,
            BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED, &report) == BTBH_GPU_INVALID_ARGUMENT);
        assert(btbh_gpu_instance_buffer_upload_ex(buffer, 0u, states[0].records[0], 0u,
            BTBH_GPU_INSTANCE_BUFFER_UPLOAD_NO_CYCLE, &report) == BTBH_GPU_INVALID_ARGUMENT);
        assert(queue->live_count == 0u && queue->entry_count == 0u);
    }
    expect_chunk(smoke, box, buffer, &current, "after the refusals: as it was");

    btbh_gpu_instance_buffer_destroy(buffer);
    btbh_gpu_custom_mesh_destroy(box);
}

/* ------------------------------------------------------------------------------------------------- range updates */

/* btbh_gpu_custom_mesh_update_range: part of a mesh's vertices or indices, plain and queued, for a mesh with buffers of
   its own and one in the shared buffers, against a fresh mesh made with the data it must hold (`current`, the same
   writes applied on the CPU). */

static void shift_vertices(MeshData *data, uint32_t first, uint32_t count, float dx, float dy, float dz) {
    for (uint32_t vertex = first; vertex < first + count; ++vertex) {
        data->vertices[(size_t)vertex * FLOATS + 0u] += dx;
        data->vertices[(size_t)vertex * FLOATS + 1u] += dy;
        data->vertices[(size_t)vertex * FLOATS + 2u] += dz;
    }
}

static void tint_vertices(MeshData *data, uint32_t first, uint32_t count, float r, float g, float b) {
    for (uint32_t vertex = first; vertex < first + count; ++vertex) {
        data->vertices[(size_t)vertex * FLOATS + 12u] = r;
        data->vertices[(size_t)vertex * FLOATS + 13u] = g;
        data->vertices[(size_t)vertex * FLOATS + 14u] = b;
    }
}

/* Vertices [first, first + count) of `source` (and indices [first_index, first_index + index_count)) into `mesh`, and
   the same into `current`. */
static void update_range(BTBHGPUCustomMesh *mesh, MeshData *current, const MeshData *source, uint32_t first,
    uint32_t count, uint32_t first_index, uint32_t index_count, bool queued) {
    BTBHGPUCustomMeshReport report;
    memset(&report, 0, sizeof(report));
    const BTBHGPUResult result = btbh_gpu_custom_mesh_update_range(mesh, first,
        count > 0u ? &source->vertices[(size_t)first * FLOATS] : NULL, count * FLOATS * (uint32_t)sizeof(float),
        first_index, index_count > 0u ? &source->indices[first_index] : NULL, index_count,
        queued ? BTBH_GPU_CUSTOM_MESH_UPDATE_QUEUED : 0u, &report);
    if (result != BTBH_GPU_OK) {
        fprintf(stderr, "range update: %s\n", report.message);
    }
    assert(result == BTBH_GPU_OK);
    assert(report.vertex_count == mesh->vertex_count && report.index_count == mesh->index_count);
    memcpy(&current->vertices[(size_t)first * FLOATS], &source->vertices[(size_t)first * FLOATS],
        (size_t)count * FLOATS * sizeof(float));
    memcpy(&current->indices[first_index], &source->indices[first_index], (size_t)index_count * sizeof(uint32_t));
}

static void expect_mesh(SmokeWindow *smoke, BTBHGPUCustomMesh *mesh, const MeshData *data, const char *what) {
    uint8_t *pixels = render(smoke, mesh);
    uint8_t *expected = reference(smoke, data);
    expect_same(pixels, expected, what);
    free(expected);
    free(pixels);
}

static void expect_presented_mesh(SmokeWindow *smoke, const MeshData *data, const char *what) {
    uint32_t frame_bytes = 0u;
    uint8_t *presented = capture_frame(smoke, &frame_bytes);
    finish_frame(smoke);
    BTBHGPUCustomMesh *fresh = create_mesh(smoke->window, data, false);
    present_scene(smoke, fresh);
    uint32_t reference_bytes = 0u;
    uint8_t *expected = capture_frame(smoke, &reference_bytes);
    finish_frame(smoke);
    btbh_gpu_custom_mesh_destroy(fresh);
    assert(frame_bytes == reference_bytes);
    const bool same = memcmp(presented, expected, frame_bytes) == 0;
    printf("%-78s %s\n", what, same ? "same bytes" : "DIFFERENT");
    assert(same);
    free(expected);
    free(presented);
}

static void check_range_updates(SmokeWindow *smoke) {
    BTBHGPUUploadQueue *queue = &smoke->window->upload_queue;
    const MeshData two = shape(2);   /* two boxes: vertices 0..23 and 24..47, indices 0..35 and 36..71 */
    assert(two.vertex_count == 48u && two.index_count == 72u);
    MeshData moved = two;            /* the second box moved and tinted */
    shift_vertices(&moved, 24u, 24u, 0.35f, 0.0f, -0.1f);
    tint_vertices(&moved, 24u, 24u, 1.0f, 0.9f, 0.2f);
    MeshData lowered = two;          /* the first box moved down and nearer */
    shift_vertices(&lowered, 0u, 24u, -0.3f, -0.25f, 0.2f);
    MeshData hidden = two;           /* the first box's triangles degenerate */
    memset(hidden.indices, 0, 36u * sizeof(uint32_t));
    MeshData striped = two;          /* faces of both boxes retinted */
    tint_vertices(&striped, 12u, 18u, 0.2f, 0.9f, 1.0f);
    MeshData dotted = two;
    tint_vertices(&dotted, 20u, 2u, 1.0f, 0.2f, 0.9f);

    for (uint32_t kind = 0u; kind < 2u; ++kind) {
        const bool shared = kind == 1u;
        char what[128];
        BTBHGPUCustomMesh *mesh = create_mesh(smoke->window, &two, shared);
        MeshData current = two;
#define WHAT(text) ((void)snprintf(what, sizeof(what), "%s mesh, %s", shared ? "shared" : "own", text), what)
        expect_mesh(smoke, mesh, &current, WHAT("as made"));

        update_range(mesh, &current, &moved, 24u, 24u, 0u, 0u, false);
        expect_mesh(smoke, mesh, &current, WHAT("a plain range update of the second box's vertices"));
        float minimum[3];
        float maximum[3];
        assert(btbh_gpu_custom_mesh_bounds(moved.vertices, 48u, minimum, maximum));
        assert(mesh->bounds_valid && mesh->maximum[0] >= maximum[0] && mesh->minimum[2] <= minimum[2]);

        update_range(mesh, &current, &lowered, 0u, 24u, 0u, 0u, true);
        assert(queue->live_count == 1u && mesh->queue_head == 1u);
        expect_mesh(smoke, mesh, &current, WHAT("a queued range update of the first box's vertices"));
        assert(btbh_gpu_custom_mesh_bounds(lowered.vertices, 24u, minimum, maximum));
        assert(mesh->minimum[1] <= minimum[1] && mesh->maximum[1] >= 0.99f);   /* grown; the old top kept */

        update_range(mesh, &current, &hidden, 0u, 0u, 0u, 36u, true);
        expect_mesh(smoke, mesh, &current, WHAT("a queued range update of indices only (the first box hidden)"));

        /* Overlapping in one frame: queued whole ranges, a queued range inside them, a plain one over that. */
        update_range(mesh, &current, &two, 0u, 48u, 0u, 72u, true);
        update_range(mesh, &current, &striped, 12u, 18u, 0u, 0u, true);
        assert(queue->live_count == 2u && queue->entry_count == 2u);   /* held by the first: it takes them */
        update_range(mesh, &current, &dotted, 20u, 2u, 0u, 0u, false);
        expect_mesh(smoke, mesh, &current, WHAT("queued all, queued inside, plain inside that: each later one wins"));

        /* A partial update after a queued whole one changes its data; a whole update replaces the partial ones. */
        update(mesh, &two, true);
        current = two;
        update_range(mesh, &current, &moved, 24u, 24u, 0u, 0u, true);
        assert(queue->live_count == 2u && queue->entry_count == 2u);
        expect_mesh(smoke, mesh, &current, WHAT("queued whole, then queued range: the range over it"));
        update_range(mesh, &current, &lowered, 0u, 24u, 0u, 0u, true);
        update(mesh, &moved, true);
        current = moved;
        assert(queue->live_count == 2u);
        expect_mesh(smoke, mesh, &current, WHAT("queued range, then queued whole: the whole one"));
        /* The whole update measured afresh: the bounds are `moved`'s again. */
        assert(btbh_gpu_custom_mesh_bounds(moved.vertices, 48u, minimum, maximum));
        assert(mesh->minimum[1] == minimum[1] && mesh->maximum[0] == maximum[0]);

        /* A frame waiting for the canvas never shows a range update made after it. */
        present_scene(smoke, mesh);
        update_range(mesh, &current, &lowered, 0u, 24u, 0u, 0u, false);
        assert(queue->pending_holds);
        expect_presented_mesh(smoke, &moved, WHAT("presented, a plain range update before the canvas: the frame"));
        expect_mesh(smoke, mesh, &current, WHAT("  and the next scene shows it"));
        update(mesh, &two, true);
        current = two;
        present_scene(smoke, mesh);
        assert(mesh->queue_copied_serial == queue->pending_serial);
        update_range(mesh, &current, &striped, 12u, 18u, 0u, 0u, false);
        expect_presented_mesh(smoke, &two, WHAT("queued whole, presented, plain range before the canvas: the frame"));
        expect_mesh(smoke, mesh, &current, WHAT("  and the next scene: the range over the whole"));

        /* Refusals: the mesh and the queue as they were. */
        BTBHGPUCustomMeshReport report;
        memset(&report, 0, sizeof(report));
        assert(btbh_gpu_custom_mesh_update_range(mesh, 40u, &two.vertices[0], 24u * INSTANCE_BYTES, 0u, NULL, 0u,
            BTBH_GPU_CUSTOM_MESH_UPDATE_QUEUED, &report) == BTBH_GPU_INVALID_ARGUMENT);
        if (!shared) {
            printf("refused a range past the vertices: %s\n", report.message);
        }
        assert(strstr(report.message, "within") != NULL);
        assert(btbh_gpu_custom_mesh_update_range(mesh, 0u, NULL, 0u, 60u, &two.indices[0], 36u, 0u, &report) ==
            BTBH_GPU_INVALID_ARGUMENT);
        const uint32_t bad[3] = { 0u, 1u, 48u };
        assert(btbh_gpu_custom_mesh_update_range(mesh, 0u, NULL, 0u, 0u, bad, 3u, BTBH_GPU_CUSTOM_MESH_UPDATE_QUEUED,
            &report) == BTBH_GPU_INVALID_ARGUMENT);
        assert(strstr(report.message, "outside") != NULL);
        assert(btbh_gpu_custom_mesh_update_range(mesh, 0u, NULL, 0u, 0u, NULL, 0u, 0u, &report) ==
            BTBH_GPU_INVALID_ARGUMENT);
        assert(btbh_gpu_custom_mesh_update_range(mesh, 0u, &two.vertices[0], 100u, 0u, NULL, 0u, 0u, &report) ==
            BTBH_GPU_INVALID_ARGUMENT);
        assert(btbh_gpu_custom_mesh_update_range(mesh, 0u, &two.vertices[0], INSTANCE_BYTES, 0u, NULL, 0u, 1u << 1,
            &report) == BTBH_GPU_INVALID_ARGUMENT);
        assert(strstr(report.message, "flags") != NULL);
        assert(btbh_gpu_custom_mesh_update_range(mesh, 0u, &two.vertices[0], INSTANCE_BYTES, 0u, NULL, 0u, 0u, NULL) ==
            BTBH_GPU_INVALID_ARGUMENT);
        assert(queue->live_count == 0u && queue->entry_count == 0u);
        expect_mesh(smoke, mesh, &current, WHAT("after the refusals: as it was"));
#undef WHAT
        btbh_gpu_custom_mesh_destroy(mesh);
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
    BTBHGPUInstanceBuffer *buffer = chunk_buffer(window);
    const Chunk chunk = chunk_state(1u);
    upload(buffer, &chunk, 0u, CHUNK_INSTANCES, BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED);
    assert(buffer->queue_head != 0u);
    btbh_gpu_window_destroy(window);
    assert(own->owner == NULL && own->queue_head == 0u && shared->queue_head == 0u);
    assert(buffer->owner == NULL && buffer->queue_head == 0u);
    BTBHGPUInstanceBufferReport buffer_report;
    memset(&buffer_report, 0, sizeof(buffer_report));
    assert(btbh_gpu_instance_buffer_upload_ex(buffer, 0u, chunk.records[0], CHUNK_BYTES,
        BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED, &buffer_report) == BTBH_GPU_INVALID_ARGUMENT);
    btbh_gpu_instance_buffer_destroy(buffer);
    BTBHGPUCustomMeshReport report;
    memset(&report, 0, sizeof(report));
    assert(btbh_gpu_custom_mesh_update_ex(own, b.vertices, bytes_of(&b), b.indices, b.index_count,
        BTBH_GPU_CUSTOM_MESH_UPDATE_QUEUED, &report) == BTBH_GPU_INVALID_ARGUMENT);
    btbh_gpu_custom_mesh_destroy(own);
    btbh_gpu_custom_mesh_destroy(shared);
    printf("a window closed with updates and uploads queued: dropped; its meshes and buffer destroyed after it\n");
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
    bool updated;
    bool queued;
    bool shared;
    uint32_t still;   /* the mode without updates of the same meshes, whose present this one's is compared with */
} BenchMode;

#define BENCH_MODES 5u

static void run_bench(SmokeWindow *smoke) {
    static const BenchMode modes[BENCH_MODES] = {
        { "still", false, false, false, 0u },
        { "plain", true, false, false, 0u },
        { "queued", true, true, false, 0u },
        { "still, shared", false, false, true, 3u },
        { "queued, shared", true, true, true, 3u },
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
    BTBHGPUCustomMesh *meshes[BENCH_MODES][BENCH_MESHES];
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
    for (uint32_t mode = 0u; mode < BENCH_MODES; ++mode) {
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
    static double update_ms[BENCH_MODES][ROUNDS * FRAMES];
    static double present_ms[BENCH_MODES][ROUNDS * FRAMES];
    static double upload_ms[BENCH_MODES][ROUNDS * FRAMES];
    uint32_t samples[BENCH_MODES] = { 0u, 0u, 0u, 0u, 0u };
    for (uint32_t round = 0u; round < ROUNDS; ++round) {
        for (uint32_t step = 0u; step < BENCH_MODES; ++step) {
            const uint32_t mode = (round + step) % BENCH_MODES;
            for (uint32_t index = 0u; index < BENCH_MESHES; ++index) {
                batches[index] = base_batch(meshes[mode][index], index);
            }
            for (uint32_t frame = 0u; frame < FRAMES + 2u; ++frame) {
                /* Each mark moves a little every frame. */
                const uint64_t update_started = SDL_GetTicksNS();
                for (uint32_t index = 0u; index < BENCH_MESHES && modes[mode].updated; ++index) {
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
                    /* The scene's upload: its map, copies and copy pass (the queued updates' included). */
                    upload_ms[mode][samples[mode]] = report.cpu_upload_milliseconds;
                    samples[mode] += 1u;
                }
            }
        }
    }
    double median_update[BENCH_MODES];
    double median_present[BENCH_MODES];
    double median_upload[BENCH_MODES];
    for (uint32_t mode = 0u; mode < BENCH_MODES; ++mode) {
        qsort(update_ms[mode], samples[mode], sizeof(double), compare_doubles);
        qsort(present_ms[mode], samples[mode], sizeof(double), compare_doubles);
        qsort(upload_ms[mode], samples[mode], sizeof(double), compare_doubles);
        median_update[mode] = update_ms[mode][samples[mode] / 2u];
        median_present[mode] = present_ms[mode][samples[mode] / 2u];
        median_upload[mode] = upload_ms[mode][samples[mode] / 2u];
        printf("bench %-15s %3u updates of %u vertices a frame, %u frames: updates %.3f ms (p10 %.3f, p90 %.3f), "
            "%.2f us each; present %.3f ms (p10 %.3f), its upload %.4f ms\n",
            modes[mode].name, modes[mode].updated ? BENCH_MESHES : 0u, BENCH_VERTICES, samples[mode],
            median_update[mode], update_ms[mode][samples[mode] / 10u], update_ms[mode][samples[mode] * 9u / 10u],
            1000.0 * median_update[mode] / BENCH_MESHES, median_present[mode], present_ms[mode][samples[mode] / 10u],
            median_upload[mode]);
    }
    for (uint32_t mode = 0u; mode < BENCH_MODES; ++mode) {
        if (!modes[mode].updated) {
            continue;
        }
        const uint32_t still = modes[mode].still;
        printf("bench %-15s per update: %.2f us at the call, %.2f us in the present (%.2f us in its upload) over "
            "\"%s\": %.2f us in all\n",
            modes[mode].name, 1000.0 * median_update[mode] / BENCH_MESHES,
            1000.0 * (median_present[mode] - median_present[still]) / BENCH_MESHES,
            1000.0 * (median_upload[mode] - median_upload[still]) / BENCH_MESHES, modes[still].name,
            1000.0 * (median_update[mode] + median_present[mode] - median_present[still]) / BENCH_MESHES);
    }
    for (uint32_t mode = 0u; mode < BENCH_MODES; ++mode) {
        for (uint32_t index = 0u; index < BENCH_MESHES; ++index) {
            btbh_gpu_custom_mesh_destroy(meshes[mode][index]);
        }
    }
}

/* BTBH_SMOKE_BENCH=upload-queue: 60 chunks (instance buffers of 4 to 64 KiB, 42 to 682 instances of a box), each drawn
   by a batch, all uploaded every frame, in interleaved rounds of five modes: none ("still"), whole with the plain
   upload (each its own submission, cycling: what a host does today), whole queued, a quarter queued (at an offset that
   moves every frame), a quarter NO_CYCLE (each its own submission). Prints, per upload, the CPU time at the call and in
   the present (its upload) over "still", and the frame's CPU total (uploads, present, canvas). */
#define UPLOAD_BENCH_CHUNKS 60u
#define UPLOAD_BENCH_MODES 5u

typedef struct UploadBenchMode {
    const char *name;
    bool uploads;
    bool whole;
    uint32_t flags;
} UploadBenchMode;

static void run_upload_bench(SmokeWindow *smoke) {
    static const UploadBenchMode modes[UPLOAD_BENCH_MODES] = {
        { "still", false, false, 0u },
        { "plain whole", true, true, 0u },
        { "queued whole", true, true, BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED },
        { "queued quarter", true, false, BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED },
        { "no-cycle quarter", true, false, BTBH_GPU_INSTANCE_BUFFER_UPLOAD_NO_CYCLE },
    };
    enum { ROUNDS = 12, FRAMES = 10 };
    MeshData box_data;
    memset(&box_data, 0, sizeof(box_data));
    add_box(&box_data, (const float[3]){ 0.0f, 0.02f, 0.0f }, (const float[3]){ 0.015f, 0.02f, 0.015f },
        (const float[3]){ 1.0f, 1.0f, 1.0f });
    BTBHGPUCustomMesh *box = create_mesh(smoke->window, &box_data, true);
    BTBHGPUInstanceBuffer *chunks[UPLOAD_BENCH_CHUNKS];
    uint32_t counts[UPLOAD_BENCH_CHUNKS];
    float *data[UPLOAD_BENCH_CHUNKS];
    uint64_t total_bytes = 0u;
    uint64_t quarter_bytes = 0u;
    for (uint32_t chunk = 0u; chunk < UPLOAD_BENCH_CHUNKS; ++chunk) {
        const uint32_t bytes = 4096u + chunk * (65536u - 4096u) / (UPLOAD_BENCH_CHUNKS - 1u);
        counts[chunk] = bytes / INSTANCE_BYTES;
        total_bytes += (uint64_t)counts[chunk] * INSTANCE_BYTES;
        quarter_bytes += (uint64_t)(counts[chunk] / 4u) * INSTANCE_BYTES;
        BTBHGPUInstanceBufferReport report;
        memset(&report, 0, sizeof(report));
        chunks[chunk] = btbh_gpu_window_instance_buffer_create(smoke->window, counts[chunk], INSTANCE_BYTES, &report);
        assert(chunks[chunk] != NULL);
        data[chunk] = malloc((size_t)counts[chunk] * INSTANCE_BYTES);
        assert(data[chunk] != NULL);
        for (uint32_t index = 0u; index < counts[chunk]; ++index) {
            instance_at(&data[chunk][(size_t)index * FLOATS], -2.0f + 0.4f * (float)(chunk % 10u) +
                0.012f * (float)(index % 30u), 0.0f, -2.0f + 0.6f * (float)(chunk / 10u) + 0.012f * (float)(index / 30u),
                1.0f, 1.0f, 1.0f);
        }
        assert(btbh_gpu_instance_buffer_upload(chunks[chunk], 0u, data[chunk], counts[chunk] * INSTANCE_BYTES,
            &report) == BTBH_GPU_OK);
    }
    static BTBHGPUSceneGeometryBatch batches[UPLOAD_BENCH_CHUNKS];
    for (uint32_t chunk = 0u; chunk < UPLOAD_BENCH_CHUNKS; ++chunk) {
        batches[chunk] = base_batch(box, 0u);
        batches[chunk].instance_buffer = chunks[chunk];
        batches[chunk].instance_count = counts[chunk];
    }
    Scene scene;
    build_scene(smoke, smoke->ground, &scene);
    static double upload_ms[UPLOAD_BENCH_MODES][ROUNDS * FRAMES];
    static double present_ms[UPLOAD_BENCH_MODES][ROUNDS * FRAMES];
    static double scene_upload_ms[UPLOAD_BENCH_MODES][ROUNDS * FRAMES];
    static double canvas_ms[UPLOAD_BENCH_MODES][ROUNDS * FRAMES];
    static double frame_ms[UPLOAD_BENCH_MODES][ROUNDS * FRAMES];
    uint32_t samples[UPLOAD_BENCH_MODES] = { 0u, 0u, 0u, 0u, 0u };
    uint32_t frame_serial = 0u;
    for (uint32_t round = 0u; round < ROUNDS; ++round) {
        for (uint32_t step = 0u; step < UPLOAD_BENCH_MODES; ++step) {
            const uint32_t mode = (round + step) % UPLOAD_BENCH_MODES;
            for (uint32_t frame = 0u; frame < FRAMES + 2u; ++frame) {
                frame_serial += 1u;
                const uint64_t upload_started = SDL_GetTicksNS();
                for (uint32_t chunk = 0u; chunk < UPLOAD_BENCH_CHUNKS && modes[mode].uploads; ++chunk) {
                    /* The chunk's data changes a little every frame. */
                    data[chunk][13] = 0.0001f * (float)(frame_serial % 100u);
                    const uint32_t quarter = counts[chunk] / 4u;
                    const uint32_t first = modes[mode].whole ? 0u : (frame_serial % 4u) * quarter;
                    const uint32_t count = modes[mode].whole ? counts[chunk] : quarter;
                    BTBHGPUInstanceBufferReport report;
                    const BTBHGPUResult result = btbh_gpu_instance_buffer_upload_ex(chunks[chunk],
                        first * INSTANCE_BYTES, &data[chunk][(size_t)first * FLOATS], count * INSTANCE_BYTES,
                        modes[mode].flags, &report);
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
                    &scene.lighting, &scene.post, &scene.instances[0][0], 1u, 1u, batches, UPLOAD_BENCH_CHUNKS,
                    20u, 24u, 30u, 255u, &report);
                assert(result == BTBH_GPU_OK);
                const uint64_t present_finished = SDL_GetTicksNS();
                finish_frame(smoke);
                const uint64_t canvas_finished = SDL_GetTicksNS();
                if (frame >= 2u) {
                    const uint32_t sample = samples[mode];
                    upload_ms[mode][sample] = (double)(present_started - upload_started) / 1.0e6;
                    present_ms[mode][sample] = (double)(present_finished - present_started) / 1.0e6;
                    scene_upload_ms[mode][sample] = report.cpu_upload_milliseconds;
                    canvas_ms[mode][sample] = (double)(canvas_finished - present_finished) / 1.0e6;
                    frame_ms[mode][sample] = (double)(canvas_finished - upload_started) / 1.0e6;
                    samples[mode] += 1u;
                }
            }
        }
    }
    printf("bench upload-queue: %u chunks, %.1f KiB whole (%.1f KiB a quarter) a frame, %u frames a mode\n",
        UPLOAD_BENCH_CHUNKS, (double)total_bytes / 1024.0, (double)quarter_bytes / 1024.0, samples[0]);
    double median_upload[UPLOAD_BENCH_MODES];
    double median_present[UPLOAD_BENCH_MODES];
    double median_scene_upload[UPLOAD_BENCH_MODES];
    double median_canvas[UPLOAD_BENCH_MODES];
    double median_frame[UPLOAD_BENCH_MODES];
    for (uint32_t mode = 0u; mode < UPLOAD_BENCH_MODES; ++mode) {
        const uint32_t n = samples[mode];
        qsort(upload_ms[mode], n, sizeof(double), compare_doubles);
        qsort(present_ms[mode], n, sizeof(double), compare_doubles);
        qsort(scene_upload_ms[mode], n, sizeof(double), compare_doubles);
        qsort(canvas_ms[mode], n, sizeof(double), compare_doubles);
        qsort(frame_ms[mode], n, sizeof(double), compare_doubles);
        median_upload[mode] = upload_ms[mode][n / 2u];
        median_present[mode] = present_ms[mode][n / 2u];
        median_scene_upload[mode] = scene_upload_ms[mode][n / 2u];
        median_canvas[mode] = canvas_ms[mode][n / 2u];
        median_frame[mode] = frame_ms[mode][n / 2u];
        printf("bench %-17s uploads %.3f ms (p10 %.3f, p90 %.3f); present %.3f ms (its upload %.4f); canvas %.3f ms; "
            "frame %.3f ms (p10 %.3f, p90 %.3f)\n",
            modes[mode].name, median_upload[mode], upload_ms[mode][n / 10u], upload_ms[mode][n * 9u / 10u],
            median_present[mode], median_scene_upload[mode], median_canvas[mode], median_frame[mode],
            frame_ms[mode][n / 10u], frame_ms[mode][n * 9u / 10u]);
    }
    for (uint32_t mode = 1u; mode < UPLOAD_BENCH_MODES; ++mode) {
        printf("bench %-17s per upload: %.2f us at the call, %.2f us in the present (%.2f us in its upload), %.2f us "
            "in the canvas, over \"still\"; frame +%.3f ms over \"still\"\n",
            modes[mode].name, 1000.0 * median_upload[mode] / UPLOAD_BENCH_CHUNKS,
            1000.0 * (median_present[mode] - median_present[0]) / UPLOAD_BENCH_CHUNKS,
            1000.0 * (median_scene_upload[mode] - median_scene_upload[0]) / UPLOAD_BENCH_CHUNKS,
            1000.0 * (median_canvas[mode] - median_canvas[0]) / UPLOAD_BENCH_CHUNKS,
            median_frame[mode] - median_frame[0]);
    }
    for (uint32_t chunk = 0u; chunk < UPLOAD_BENCH_CHUNKS; ++chunk) {
        btbh_gpu_instance_buffer_destroy(chunks[chunk]);
        free(data[chunk]);
    }
    btbh_gpu_custom_mesh_destroy(box);
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
    const bool bench_meshes = bench != NULL && strcmp(bench, "mesh-queue") == 0;
    const bool bench_uploads = bench != NULL && strcmp(bench, "upload-queue") == 0;
    const bool benching = bench_meshes || bench_uploads;
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
    if (bench_meshes) {
        run_bench(&smoke);
    } else if (bench_uploads) {
        run_upload_bench(&smoke);
    } else {
        check_queue(&smoke);
        check_instance_uploads(&smoke);
        check_range_updates(&smoke);
        check_closed_window(device);
    }
    btbh_gpu_custom_mesh_destroy(smoke.ground);
    btbh_gpu_texture_destroy(smoke.target);
    btbh_gpu_window_destroy(smoke.window);
    btbh_gpu_device_destroy(device);
    puts("btbh_gpu_mesh_queue_smoke: passed");
    return 0;
}
