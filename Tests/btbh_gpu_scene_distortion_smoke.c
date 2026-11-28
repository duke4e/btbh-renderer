/* Screen-space distortion (btbh_gpu_window_scene_set_distortion, RFC 0018, since 0.18.0). Hidden windows; each frame is
 * copied out of the window and read back, and so are the offset field and FSR 3's reactive mask (through the core's
 * btbh_gpu_window_scene_distortion_textures).
 *
 * The scene: an unlit ground under a camera looking down at 45 degrees, its vertex colours a smooth pattern (so a
 * displaced read shows), tone mapped with ACES and nothing else in the chain.
 *
 * - Identity: a frame with BTBH_GPU_SCENE_POST_DISTORTION and no source, with sources all skipped (strength 0, a ring
 *   of width 0) or culled (off to the side), and with sources but without the flag, is byte for byte the frame without
 *   any of it; its report's distortion_sources_drawn is 0. Sources are taken by one present: the next has none.
 * - Each source against the CPU reference (btbh_gpu_distortion.h): rings, discs and strips, facing and flat, pushed,
 *   pulled and hazy, with chroma, past the fold clamp, two overlapping. The field read back must match the reference's
 *   at every texel (FIELD_TOLERANCE of the strongest displacement, a few texels on a shape's rim excepted), and the
 *   frame the undisplaced frame resampled at p - d (d the reference's field read bilinearly, as the composite reads it;
 *   red and blue at their own offsets) within IMAGE_TOLERANCE levels but for IMAGE_OUTLIERS of the pixels.
 * - Scene views: a vertical split of one camera with a ring across the line equals the one-view frame farther than
 *   VIEW_BAND render pixels from the line; with view 0 seeing a red ground and view 1 a blue one and a strong ring on the
 *   line, no pixel of a half shows the other's colour. A source far from the line leaves the band by it byte for byte
 *   (no displacement, no read moved). A source in view 0's frustum but wholly on view 1's side (view 1's camera elsewhere)
 *   is culled: the frame is the frame without it, byte for byte, and none is counted.
 * - FSR 3 (from half the size, where the device has it): the reactive mask holds min(1, |d| / 6) times the frame's
 *   reactive scale (d: the field's summed displacement read bilinearly and clamped) where the field displaces, 0
 *   elsewhere; a ring that grows over 30 frames under a panning camera presents every frame.
 * - The setter: each refusal (BTBH_GPU_INVALID_ARGUMENT, the message naming the source and the field), after which
 *   nothing is drawn; a render into a target neither draws nor takes the sources; a present refused for its arguments
 *   leaves them set.
 * - Prewarm: after a prewarm with the flag, the first frame with sources makes nothing; without it, the lazy creation
 *   log names the distortion's pipelines and field, each one a prewarm makes.
 * - A shader directory without production_distortion.frag fails a frame with sources (naming the file), and not one
 *   without.
 *
 * BTBH_DISTORTION_SMOKE_DUMP=<dir> writes the images. It needs a GPU and the production shaders; with no device it is
 * skipped (77). */
#undef NDEBUG

#include "btbh_gpu/btbh_gpu.h"
#include "btbh_gpu_distortion.h"
#include "btbh_gpu_hdr.h"
#include "btbh_gpu_internal.h"

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_hints.h>

#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(BTBH_GPU_SCENE_HAS_DISTORTION)
#error "BTBH_GPU_SCENE_HAS_DISTORTION"
#endif

#define SMOKE_SKIP 77
#define WIDTH 320u
#define HEIGHT 180u
/* The field read back against the reference's: per texel, the displacement's error within this share of the case's
   strongest displacement (plus a hundredth of a pixel); a texel on a shape's rim may round the other way (the GPU's
   derivatives are a pixel's differences, the reference's exact), at most FIELD_RIM_LIMIT of the texels the shape
   covers. */
#define FIELD_TOLERANCE 0.03f
#define FIELD_RIM_LIMIT 0.01f
/* The frame against the undisplaced one resampled: the composite filters in HDR before the tone map, the reference in
   the 8-bit frame after it (with its dither), so a level or two apart, more where the pattern bends fast. */
#define IMAGE_TOLERANCE 4
#define IMAGE_OUTLIERS 0.003f
/* Scene views: render pixels from the line beyond which a split frame equals the one-view frame (the fade's 8, and a
   pixel for the half-size field's reach and the tone map's rounding). */
#define VIEW_BAND 9.5f

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

typedef struct SmokeWindow {
    BTBHGPUWindow *window;
    BTBHGPUCustomMesh *ground;
    BTBHGPUTexture *frame;
    BTBHGPUTexture *portrait;
} SmokeWindow;

/* ---- Cameras ---- */

typedef struct SmokeCamera {
    float view_projection[16];
    float inverse[16];
    float right[3];
    float up[3];
    float position[3];
} SmokeCamera;

#define CAMERA_NEAR 0.1f
#define CAMERA_FAR 60.0f
#define CAMERA_FOV 0.85f

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

/* Looking down at 45 degrees on (eye_x, 0, 0) from 8 up and 8 back. */
static SmokeCamera camera_at(float eye_x, uint32_t width, uint32_t height) {
    SmokeCamera camera;
    memset(&camera, 0, sizeof(camera));
    const float eye[3] = {eye_x, 8.0f, 8.0f};
    float forward[3] = {0.0f, -8.0f, -8.0f};
    (void)btbh_distortion_normalize3(forward, 0.0f);
    const float world_up[3] = {0.0f, 1.0f, 0.0f};
    float side[3];
    btbh_distortion_cross3(forward, world_up, side);
    (void)btbh_distortion_normalize3(side, 0.0f);
    float up[3];
    btbh_distortion_cross3(side, forward, up);
    const float view[16] = {
        side[0], up[0], -forward[0], 0.0f,
        side[1], up[1], -forward[1], 0.0f,
        side[2], up[2], -forward[2], 0.0f,
        -btbh_distortion_dot3(eye, side), -btbh_distortion_dot3(eye, up), btbh_distortion_dot3(eye, forward), 1.0f,
    };
    const float h = 1.0f / tanf(0.5f * CAMERA_FOV);
    const float aspect = (float)width / (float)height;
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
    memcpy(camera.position, eye, sizeof(eye));
    return camera;
}

/* ---- The scene ---- */

static BTBHGPUSceneLighting lighting_plain(void) {
    BTBHGPUSceneLighting lighting;
    memset(&lighting, 0, sizeof(lighting));
    lighting.ambient_red = 1.0f;
    lighting.ambient_green = 1.0f;
    lighting.ambient_blue = 1.0f;
    lighting.ambient_energy = 0.35f;
    lighting.directional_energy = 2.0f;
    lighting.directional_red = 1.0f;
    lighting.directional_green = 1.0f;
    lighting.directional_blue = 1.0f;
    lighting.directional_y = -1.0f;
    lighting.spot_range = 8.0f;
    lighting.spot_falloff = 1.0f;
    lighting.spot_inner_cutoff = 1.0f;
    return lighting;
}

static BTBHGPUScenePost post_plain(uint8_t upscale_mode, bool flag) {
    BTBHGPUScenePost post;
    memset(&post, 0, sizeof(post));
    post.tonemap_white = 1.0f;
    post.exposure = 1.0f;
    post.contrast = 1.0f;
    post.saturation = 1.0f;
    post.blur_curve = 1.0f;
    post.tonemap_mode = BTBH_GPU_SCENE_TONEMAP_ACES;
    post.upscale_mode = upscale_mode;
    post.temporal_camera_near = CAMERA_NEAR;
    post.temporal_camera_far = CAMERA_FAR;
    post.temporal_vertical_fov = CAMERA_FOV;
    post.temporal_frame_seconds = 1.0f / 60.0f;
    post.temporal_meters_per_unit = 1.0f;
    post.temporal_reactive_scale = 0.8f;
    post.flags = flag ? (uint8_t)BTBH_GPU_SCENE_POST_DISTORTION : 0u;
    return post;
}

/* The ground's colour: smooth bands, every channel within 0.1..0.8 (the tone map stays nearly linear there). */
static void ground_colour(float x, float z, float out[3]) {
    out[0] = 0.45f + 0.33f * sinf(0.9f * x + 0.3f * z);
    out[1] = 0.45f + 0.33f * sinf(0.75f * z - 0.2f * x + 1.0f);
    out[2] = 0.45f + 0.33f * sinf(0.55f * (x + z) + 2.0f);
}

#define GRID 96u
/* (Past the frame's corners: no background shows.) */
#define GROUND_EXTENT 30.0f

/* A grid over x, z in -GROUND_EXTENT..GROUND_EXTENT at y 0, facing up, its vertex colours ground_colour; 24 floats a
   vertex. */
static BTBHGPUCustomMesh *create_ground(BTBHGPUWindow *window) {
    const uint32_t side = GRID + 1u;
    float *vertices = calloc((size_t)side * side * 24u, sizeof(float));
    uint32_t *indices = calloc((size_t)GRID * GRID * 6u, sizeof(uint32_t));
    assert(vertices != NULL && indices != NULL);
    for (uint32_t row = 0u; row < side; ++row) {
        for (uint32_t column = 0u; column < side; ++column) {
            float *vertex = &vertices[((size_t)row * side + column) * 24u];
            const float x = -GROUND_EXTENT + 2.0f * GROUND_EXTENT * (float)column / (float)GRID;
            const float z = -GROUND_EXTENT + 2.0f * GROUND_EXTENT * (float)row / (float)GRID;
            vertex[0] = x;
            vertex[1] = 0.0f;
            vertex[2] = z;
            vertex[4] = 1.0f;
            vertex[6] = (float)column / (float)GRID;
            vertex[7] = (float)row / (float)GRID;
            vertex[8] = 1.0f;
            vertex[11] = 1.0f;
            float colour[3];
            ground_colour(x, z, colour);
            vertex[12] = colour[0];
            vertex[13] = colour[1];
            vertex[14] = colour[2];
            vertex[15] = 1.0f;
            vertex[20] = 1.0f;
        }
    }
    uint32_t *index = indices;
    for (uint32_t row = 0u; row < GRID; ++row) {
        for (uint32_t column = 0u; column < GRID; ++column) {
            const uint32_t a = row * side + column;
            const uint32_t b = a + 1u;
            const uint32_t c = a + side;
            const uint32_t d = c + 1u;
            const uint32_t quad[6] = {a, c, b, b, c, d};
            memcpy(index, quad, sizeof(quad));
            index += 6;
        }
    }
    BTBHGPUCustomMeshReport report;
    memset(&report, 0, sizeof(report));
    BTBHGPUCustomMesh *mesh = btbh_gpu_window_custom_mesh_create(window, vertices,
        (uint32_t)((size_t)side * side * 24u * sizeof(float)), side * side, indices, GRID * GRID * 6u, &report);
    if (mesh == NULL) {
        fprintf(stderr, "ground: %s\n", report.message);
    }
    assert(mesh != NULL);
    free(vertices);
    free(indices);
    return mesh;
}

static void instance(float out[24], const float colour[4]) {
    memset(out, 0, 24 * sizeof(float));
    out[0] = 1.0f;
    out[5] = 1.0f;
    out[10] = 1.0f;
    out[15] = 1.0f;
    memcpy(&out[16], colour, 4 * sizeof(float));
}

static BTBHGPUSceneGeometryBatch ground_batch(BTBHGPUCustomMesh *ground, uint32_t first) {
    BTBHGPUSceneGeometryBatch batch;
    memset(&batch, 0, sizeof(batch));
    batch.kind = BTBH_GPU_SCENE_GEOMETRY_CUSTOM_MESH;
    batch.custom_mesh = ground;
    batch.material_slot = -1;
    batch.first_instance = first;
    batch.instance_count = 1u;
    batch.albedo_red = 255u;
    batch.albedo_green = 255u;
    batch.albedo_blue = 255u;
    batch.albedo_alpha = 255u;
    batch.uv_scale_x = 1.0f;
    batch.uv_scale_y = 1.0f;
    batch.roughness = 0.5f;
    batch.occlusion = 1.0f;
    batch.unlit_enabled = 1u;
    batch.depth_compare = BTBH_GPU_SCENE_DEPTH_LESS;
    batch.cull_mode = BTBH_GPU_SCENE_CULL_NONE;
    batch.stencil_pass = BTBH_GPU_SCENE_STENCIL_REPLACE;   /* (with compare ALWAYS and reference 0: no stencil) */
    return batch;
}

typedef struct Params {
    const ShaderSet *shaders;
    bool flag;
    /* Two views of one camera, split at the frame's middle (view 0 left). */
    bool split;
    /* With the split: view 0 sees a red ground, view 1 a blue one. */
    bool colour_split;
    uint8_t upscale_mode;
    uint32_t render_width;
    uint32_t render_height;
    float eye_x;
    /* With the split: view 1's camera this far along x from view 0's (0: the same camera). */
    float view1_offset;
    /* A present the core refuses (no batches). */
    bool refused;
} Params;

static Params params_default(void) {
    Params params;
    memset(&params, 0, sizeof(params));
    params.shaders = &shaders;
    params.flag = true;
    params.upscale_mode = BTBH_GPU_SCENE_UPSCALE_LINEAR;
    params.render_width = WIDTH;
    params.render_height = HEIGHT;
    return params;
}

static BTBHGPUResult present(SmokeWindow *smoke, const Params *params, BTBHGPUSceneReport *report) {
    const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    const float red[4] = {1.0f, 0.12f, 0.12f, 1.0f};
    const float blue[4] = {0.12f, 0.12f, 1.0f, 1.0f};
    float instances[2][24];
    instance(instances[0], params->colour_split ? red : white);
    instance(instances[1], blue);
    BTBHGPUSceneGeometryBatch batches[2] = {ground_batch(smoke->ground, 0u), ground_batch(smoke->ground, 1u)};
    const uint32_t batch_count = params->colour_split ? 2u : 1u;
    const BTBHGPUSceneLighting lighting = lighting_plain();
    const BTBHGPUScenePost post = post_plain(params->upscale_mode, params->flag);
    const SmokeCamera camera = camera_at(params->eye_x, WIDTH, HEIGHT);
    const ShaderSet *set = params->shaders;
    if (!params->split) {
        return btbh_gpu_window_scene_present_instances(smoke->window, set->paths[0], set->paths[1], set->paths[2],
            set->paths[3], set->paths[4], set->paths[5], set->paths[6], set->paths[7], set->paths[8], set->paths[9],
            set->paths[10], SMOKE_SHADER_FORMAT, params->render_width, params->render_height, camera.view_projection,
            camera.right, camera.up, camera.position, &lighting, &post, &instances[0][0], 2u, 2u,
            params->refused ? NULL : batches, params->refused ? 0u : batch_count, 23u, 41u, 67u, 255u, report);
    }
    static const uint8_t only_red[2] = {1u, 0u};
    static const uint8_t only_blue[2] = {0u, 1u};
    BTBHGPUSceneView views[2];
    memset(views, 0, sizeof(views));
    const SmokeCamera second = camera_at(params->eye_x + params->view1_offset, WIDTH, HEIGHT);
    for (uint32_t index = 0u; index < 2u; ++index) {
        const SmokeCamera *view_camera = index == 0u ? &camera : &second;
        memcpy(views[index].view_projection, view_camera->view_projection, sizeof(views[index].view_projection));
        memcpy(views[index].right, view_camera->right, sizeof(views[index].right));
        memcpy(views[index].up, view_camera->up, sizeof(views[index].up));
        memcpy(views[index].position, view_camera->position, sizeof(views[index].position));
        const float sign = index == 0u ? -1.0f : 1.0f;
        views[index].clip[0] = sign;
        views[index].clip[1] = 0.0f;
        views[index].clip[2] = -sign * 0.5f * (float)WIDTH;
        if (params->colour_split) {
            views[index].visibility = index == 0u ? only_red : only_blue;
        }
    }
    BTBHGPUSceneViewReport view_reports[2];
    return btbh_gpu_window_scene_present_views(smoke->window, set->paths[0], set->paths[1], set->paths[2],
        set->paths[3], set->paths[4], set->paths[5], set->paths[6], set->paths[7], set->paths[8], set->paths[9],
        set->paths[10], SMOKE_SHADER_FORMAT, params->render_width, params->render_height, views, 2u, &lighting, &post,
        &instances[0][0], 2u, 2u, batches, NULL, NULL, batch_count, 23u, 41u, 67u, 255u, view_reports, report);
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

static uint8_t *read_frame(SmokeWindow *smoke) {
    BTBHGPUTexture *texture = smoke->frame;
    const uint32_t texel = SDL_GPUTextureFormatTexelBlockSize(texture->format);
    assert(texel == 4u);
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

/* The field read back, as floats (four a texel). */
static float *read_field(SmokeWindow *smoke, uint32_t *width, uint32_t *height) {
    SDL_GPUTexture *field = NULL;
    SDL_GPUTexture *reactive = NULL;
    btbh_gpu_window_scene_distortion_textures(smoke->window, &field, width, height, &reactive);
    assert(field != NULL && *width > 0u && *height > 0u);
    uint8_t *bytes = read_gpu_texture(smoke->window, field, *width, *height, 8u);
    const size_t count = (size_t)*width * *height * 4u;
    float *values = malloc(count * sizeof(float));
    assert(values != NULL);
    for (size_t index = 0u; index < count; ++index) {
        uint16_t half = 0u;
        memcpy(&half, bytes + index * 2u, 2u);
        values[index] = btbh_hdr_half_to_float(half);
    }
    free(bytes);
    return values;
}

static void dump(const char *name, const uint8_t *pixels, uint32_t width, uint32_t height) {
    const char *directory = SDL_getenv("BTBH_DISTORTION_SMOKE_DUMP");
    if (directory == NULL) {
        return;
    }
    char path[1024];
    (void)snprintf(path, sizeof(path), "%s/%s.png", directory, name);
    (void)btbh_gpu_write_png_rgba8(path, width, height, pixels);
}

/* Presents (which must succeed), copies the frame out and reads it back. */
static uint8_t *frame_pixels(SmokeWindow *smoke, const Params *params, BTBHGPUSceneReport *out_report) {
    BTBHGPUSceneReport report;
    const BTBHGPUResult result = present(smoke, params, &report);
    if (result != BTBH_GPU_OK) {
        fprintf(stderr, "present: %s\n", report.message);
    }
    assert(result == BTBH_GPU_OK);
    BTBHGPUTextureReport copied;
    const BTBHGPUResult copy_result = btbh_gpu_window_copy_pending_scene_texture(smoke->window, smoke->frame, &copied);
    if (copy_result != BTBH_GPU_OK) {
        fprintf(stderr, "copy: %s\n", copied.message);
    }
    assert(copy_result == BTBH_GPU_OK);
    if (out_report != NULL) {
        *out_report = report;
    }
    return read_frame(smoke);
}

static void set_sources(SmokeWindow *smoke, const BTBHGPUSceneDistortionSource *sources, uint32_t count, float time) {
    BTBHGPUSceneReport report;
    memset(&report, 0, sizeof(report));
    const BTBHGPUResult result = btbh_gpu_window_scene_set_distortion(smoke->window, sources, count, time, &report);
    if (result != BTBH_GPU_OK) {
        fprintf(stderr, "set_distortion: %s\n", report.message);
    }
    assert(result == BTBH_GPU_OK && report.result == BTBH_GPU_OK);
}

static uint32_t differing_pixels(const uint8_t *a, const uint8_t *b) {
    uint32_t count = 0u;
    for (uint32_t index = 0u; index < WIDTH * HEIGHT; ++index) {
        if (memcmp(a + index * 4u, b + index * 4u, 4u) != 0) {
            count += 1u;
        }
    }
    return count;
}

static int level_difference(const uint8_t *a, const uint8_t *b) {
    int largest = 0;
    for (int channel = 0; channel < 3; ++channel) {
        const int difference = abs((int)a[channel] - (int)b[channel]);
        largest = difference > largest ? difference : largest;
    }
    return largest;
}

/* ---- Sources ---- */

static BTBHGPUSceneDistortionSource source_of(uint32_t shape, uint32_t mode, bool flat, float x, float z) {
    BTBHGPUSceneDistortionSource source;
    memset(&source, 0, sizeof(source));
    source.shape = shape;
    source.mode = mode;
    source.flags = flat ? BTBH_GPU_SCENE_DISTORTION_FLAT : 0u;
    source.position[0] = x;
    source.position[1] = flat ? 0.0f : 0.6f;
    source.position[2] = z;
    if (shape == BTBH_GPU_SCENE_DISTORTION_RING) {
        source.radius = 2.2f;
        source.width = 1.4f;
    } else if (shape == BTBH_GPU_SCENE_DISTORTION_DISC) {
        source.radius = 2.0f;
    } else {
        source.radius = 0.9f;
        source.end[0] = x + 4.0f;
        source.end[1] = source.position[1];
        source.end[2] = z - 1.0f;
        source.position[0] = x - 2.0f;
    }
    /* (Strong enough to move this small frame by several pixels: strength is in pixels of 1080 lines.) */
    if (mode == BTBH_GPU_SCENE_DISTORTION_HAZE) {
        source.strength = 20.0f;
        source.haze_scale = 1.3f;
        source.haze_speed = 0.7f;
    } else {
        source.strength = 40.0f;
    }
    return source;
}

/* The reference's field of `sources` for `camera` at the window's render size (four floats a texel). */
static float *reference_field(const BTBHGPUSceneDistortionSource *sources, uint32_t count, const SmokeCamera *camera,
    float time, uint32_t field_width, uint32_t field_height) {
    float *field = calloc((size_t)field_width * field_height * 4u, sizeof(float));
    assert(field != NULL);
    for (uint32_t index = 0u; index < count; ++index) {
        BTBHDistortionRecord record;
        if (!btbh_distortion_pack(&sources[index], camera->view_projection, NULL, camera->right, camera->up, WIDTH,
                HEIGHT,
                (double)time, &record)) {
            continue;
        }
        for (uint32_t y = 0u; y < field_height; ++y) {
            for (uint32_t x = 0u; x < field_width; ++x) {
                float out[4];
                btbh_distortion_field_at(&record, camera->inverse, field_width, field_height, (float)x + 0.5f,
                    (float)y + 0.5f, out);
                float *texel = &field[((size_t)y * field_width + x) * 4u];
                for (int channel = 0; channel < 4; ++channel) {
                    texel[channel] += out[channel];
                }
            }
        }
    }
    return field;
}

/* A field read bilinearly at uv (clamped to its edge texels), as the composite's sampler reads it. */
static void field_sample(const float *field, uint32_t width, uint32_t height, float u, float v, float out[4]) {
    float fx = u * (float)width - 0.5f;
    float fy = v * (float)height - 0.5f;
    fx = fx < 0.0f ? 0.0f : (fx > (float)(width - 1u) ? (float)(width - 1u) : fx);
    fy = fy < 0.0f ? 0.0f : (fy > (float)(height - 1u) ? (float)(height - 1u) : fy);
    const uint32_t x0 = (uint32_t)fx;
    const uint32_t y0 = (uint32_t)fy;
    const uint32_t x1 = x0 + 1u < width ? x0 + 1u : x0;
    const uint32_t y1 = y0 + 1u < height ? y0 + 1u : y0;
    const float tx = fx - (float)x0;
    const float ty = fy - (float)y0;
    for (int channel = 0; channel < 4; ++channel) {
        const float a = field[((size_t)y0 * width + x0) * 4u + (size_t)channel];
        const float b = field[((size_t)y0 * width + x1) * 4u + (size_t)channel];
        const float c = field[((size_t)y1 * width + x0) * 4u + (size_t)channel];
        const float d = field[((size_t)y1 * width + x1) * 4u + (size_t)channel];
        out[channel] = (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
    }
}

/* One channel of an 8-bit frame read bilinearly at pixel coordinates (x, y: a pixel's centre at +0.5), clamped. */
static float frame_sample(const uint8_t *frame, float x, float y, int channel) {
    float fx = x - 0.5f;
    float fy = y - 0.5f;
    fx = fx < 0.0f ? 0.0f : (fx > (float)(WIDTH - 1u) ? (float)(WIDTH - 1u) : fx);
    fy = fy < 0.0f ? 0.0f : (fy > (float)(HEIGHT - 1u) ? (float)(HEIGHT - 1u) : fy);
    const uint32_t x0 = (uint32_t)fx;
    const uint32_t y0 = (uint32_t)fy;
    const uint32_t x1 = x0 + 1u < WIDTH ? x0 + 1u : x0;
    const uint32_t y1 = y0 + 1u < HEIGHT ? y0 + 1u : y0;
    const float tx = fx - (float)x0;
    const float ty = fy - (float)y0;
    const float a = frame[(y0 * WIDTH + x0) * 4u + (uint32_t)channel];
    const float b = frame[(y0 * WIDTH + x1) * 4u + (uint32_t)channel];
    const float c = frame[(y1 * WIDTH + x0) * 4u + (uint32_t)channel];
    const float d = frame[(y1 * WIDTH + x1) * 4u + (uint32_t)channel];
    return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
}

typedef struct FieldResult {
    uint32_t covered;
    uint32_t outliers;
    float peak;
    float largest_error;
} FieldResult;

static FieldResult compare_fields(const float *gpu, const float *reference, uint32_t width, uint32_t height) {
    FieldResult result;
    memset(&result, 0, sizeof(result));
    for (uint32_t index = 0u; index < width * height; ++index) {
        const float length = sqrtf(reference[index * 4u] * reference[index * 4u] +
            reference[index * 4u + 1u] * reference[index * 4u + 1u]);
        result.peak = length > result.peak ? length : result.peak;
    }
    const float tolerance = FIELD_TOLERANCE * result.peak + 0.01f;
    for (uint32_t index = 0u; index < width * height; ++index) {
        const float *g = &gpu[index * 4u];
        const float *r = &reference[index * 4u];
        if (r[3] > 0.0f || g[3] > 0.0f) {
            result.covered += 1u;
        }
        const float dx = g[0] - r[0];
        const float dy = g[1] - r[1];
        const float error = fmaxf(sqrtf(dx * dx + dy * dy), fmaxf(fabsf(g[2] - r[2]), fabsf(g[3] - r[3])));
        result.largest_error = error > result.largest_error ? error : result.largest_error;
        if (error > tolerance) {
            result.outliers += 1u;
        }
    }
    return result;
}

typedef struct ImageResult {
    uint32_t outliers;
    int largest;
} ImageResult;

/* The undisplaced frame resampled at p - d (the field's d read at p, clamped as the composite clamps it), red and blue
   at their own offsets, against the displaced frame. */
static ImageResult compare_images(const uint8_t *displaced, const uint8_t *plain, const float *field,
    uint32_t field_width, uint32_t field_height) {
    ImageResult result;
    memset(&result, 0, sizeof(result));
    const float limit = BTBH_DISTORTION_SUM_LIMIT * (float)HEIGHT / BTBH_DISTORTION_REFERENCE_LINES;
    for (uint32_t y = 0u; y < HEIGHT; ++y) {
        for (uint32_t x = 0u; x < WIDTH; ++x) {
            float f[4];
            field_sample(field, field_width, field_height, ((float)x + 0.5f) / (float)WIDTH,
                ((float)y + 0.5f) / (float)HEIGHT, f);
            float dx = f[0];
            float dy = f[1];
            const float length = sqrtf(dx * dx + dy * dy);
            if (length > limit) {
                dx *= limit / length;
                dy *= limit / length;
            }
            const float chroma = f[3] > 1.0e-4f ? fminf(fmaxf(f[2] / f[3], 0.0f), 1.0f) : 0.0f;
            const float scales[3] = {1.0f + BTBH_DISTORTION_CHROMA_SPLIT * chroma, 1.0f,
                1.0f - BTBH_DISTORTION_CHROMA_SPLIT * chroma};
            const uint8_t *got = &displaced[(y * WIDTH + x) * 4u];
            int largest = 0;
            for (int channel = 0; channel < 3; ++channel) {
                const float expected = frame_sample(plain, (float)x + 0.5f - dx * scales[channel],
                    (float)y + 0.5f - dy * scales[channel], channel);
                const int difference = (int)fabsf((float)got[channel] - expected + 0.0f);
                largest = difference > largest ? difference : largest;
            }
            result.largest = largest > result.largest ? largest : result.largest;
            if (largest > IMAGE_TOLERANCE) {
                result.outliers += 1u;
            }
        }
    }
    return result;
}

/* ---- Checks ---- */

static uint8_t *plain_frame(SmokeWindow *smoke) {
    Params params = params_default();
    params.flag = false;
    return frame_pixels(smoke, &params, NULL);
}

static void check_identity(SmokeWindow *smoke) {
    uint8_t *plain = plain_frame(smoke);
    dump("plain", plain, WIDTH, HEIGHT);
    Params params = params_default();
    BTBHGPUSceneReport report;
    /* The flag alone. */
    uint8_t *frame = frame_pixels(smoke, &params, &report);
    assert(differing_pixels(plain, frame) == 0u);
    assert(report.distortion_sources_drawn == 0u);
    free(frame);
    /* Sources all skipped or culled. */
    BTBHGPUSceneDistortionSource sources[3];
    sources[0] = source_of(BTBH_GPU_SCENE_DISTORTION_RING, BTBH_GPU_SCENE_DISTORTION_PUSH, true, 0.0f, 0.0f);
    sources[0].strength = 0.0f;
    sources[1] = source_of(BTBH_GPU_SCENE_DISTORTION_RING, BTBH_GPU_SCENE_DISTORTION_PUSH, true, 0.0f, 0.0f);
    sources[1].width = 0.0f;
    sources[2] = source_of(BTBH_GPU_SCENE_DISTORTION_DISC, BTBH_GPU_SCENE_DISTORTION_PUSH, false, 500.0f, 0.0f);
    set_sources(smoke, sources, 3u, 0.0f);
    frame = frame_pixels(smoke, &params, &report);
    assert(differing_pixels(plain, frame) == 0u);
    assert(report.distortion_sources_drawn == 0u);
    free(frame);
    /* Sources without the flag: dropped, and not left for the next frame. */
    BTBHGPUSceneDistortionSource drawn = source_of(BTBH_GPU_SCENE_DISTORTION_DISC, BTBH_GPU_SCENE_DISTORTION_PUSH,
        false, 0.0f, 0.0f);
    set_sources(smoke, &drawn, 1u, 0.0f);
    params.flag = false;
    frame = frame_pixels(smoke, &params, &report);
    assert(differing_pixels(plain, frame) == 0u && report.distortion_sources_drawn == 0u);
    free(frame);
    params.flag = true;
    frame = frame_pixels(smoke, &params, &report);
    assert(differing_pixels(plain, frame) == 0u && report.distortion_sources_drawn == 0u);
    free(frame);
    /* One present takes them: drawn once, then gone. */
    set_sources(smoke, &drawn, 1u, 0.0f);
    frame = frame_pixels(smoke, &params, &report);
    assert(report.distortion_sources_drawn == 1u);
    assert(differing_pixels(plain, frame) > 100u);
    free(frame);
    frame = frame_pixels(smoke, &params, &report);
    assert(differing_pixels(plain, frame) == 0u && report.distortion_sources_drawn == 0u);
    free(frame);
    free(plain);
    printf("identity: the flag alone, skipped and culled sources, sources without the flag: 0 pixels differ\n");
}

typedef struct Case {
    const char *name;
    BTBHGPUSceneDistortionSource sources[2];
    uint32_t count;
} Case;

static void check_case(SmokeWindow *smoke, const uint8_t *plain, const Case *test) {
    const float time = 12.75f;
    set_sources(smoke, test->sources, test->count, time);
    Params params = params_default();
    BTBHGPUSceneReport report;
    uint8_t *frame = frame_pixels(smoke, &params, &report);
    assert(report.distortion_sources_drawn == test->count);
    uint32_t field_width = 0u;
    uint32_t field_height = 0u;
    float *gpu = read_field(smoke, &field_width, &field_height);
    assert(field_width == (WIDTH + 1u) / 2u && field_height == (HEIGHT + 1u) / 2u);
    const SmokeCamera camera = camera_at(0.0f, WIDTH, HEIGHT);
    float *reference = reference_field(test->sources, test->count, &camera, time, field_width, field_height);
    const FieldResult field = compare_fields(gpu, reference, field_width, field_height);
    const ImageResult image = compare_images(frame, plain, reference, field_width, field_height);
    printf("%-24s field: peak %.2f px, largest error %.3f, %u of %u covered texels past the tolerance; frame: %u "
           "pixels past %d levels (largest %d)\n", test->name, (double)field.peak, (double)field.largest_error,
        field.outliers, field.covered, image.outliers, IMAGE_TOLERANCE, image.largest);
    char name[96];
    (void)snprintf(name, sizeof(name), "case-%s", test->name);
    dump(name, frame, WIDTH, HEIGHT);
    assert(field.covered > 50u);
    assert(field.peak > 1.0f);
    assert((float)field.outliers <= FIELD_RIM_LIMIT * (float)field.covered + 1.0f);
    assert((float)image.outliers <= IMAGE_OUTLIERS * (float)(WIDTH * HEIGHT));
    free(reference);
    free(gpu);
    free(frame);
}

static void check_sources(SmokeWindow *smoke) {
    uint8_t *plain = plain_frame(smoke);
    const uint32_t ring = BTBH_GPU_SCENE_DISTORTION_RING;
    const uint32_t disc = BTBH_GPU_SCENE_DISTORTION_DISC;
    const uint32_t strip = BTBH_GPU_SCENE_DISTORTION_STRIP;
    const uint32_t push = BTBH_GPU_SCENE_DISTORTION_PUSH;
    const uint32_t haze = BTBH_GPU_SCENE_DISTORTION_HAZE;
    Case cases[13];
    memset(cases, 0, sizeof(cases));
    uint32_t count = 0u;
    const struct {
        const char *name;
        uint32_t shape;
        uint32_t mode;
        bool flat;
    } singles[8] = {
        {"ring-facing-push", ring, push, false}, {"ring-flat-push", ring, push, true},
        {"disc-facing-push", disc, push, false}, {"disc-flat-push", disc, push, true},
        {"strip-facing-push", strip, push, false}, {"strip-flat-push", strip, push, true},
        {"ring-flat-haze", ring, haze, true}, {"disc-facing-haze", disc, haze, false},
    };
    for (uint32_t index = 0u; index < 8u; ++index) {
        cases[count].name = singles[index].name;
        cases[count].sources[0] = source_of(singles[index].shape, singles[index].mode, singles[index].flat, 0.3f, 0.2f);
        cases[count].count = 1u;
        count += 1u;
    }
    cases[count].name = "strip-flat-haze";
    cases[count].sources[0] = source_of(strip, haze, true, 0.0f, 0.0f);
    cases[count].count = 1u;
    count += 1u;
    cases[count].name = "disc-facing-pull";
    cases[count].sources[0] = source_of(disc, push, false, -0.5f, 0.5f);
    cases[count].sources[0].strength = -30.0f;
    cases[count].count = 1u;
    count += 1u;
    cases[count].name = "ring-flat-chroma";
    cases[count].sources[0] = source_of(ring, push, true, 0.0f, 0.0f);
    cases[count].sources[0].chroma = 0.6f;
    cases[count].count = 1u;
    count += 1u;
    cases[count].name = "ring-past-the-clamp";
    cases[count].sources[0] = source_of(ring, push, true, 0.0f, 0.0f);
    cases[count].sources[0].strength = 500.0f;
    cases[count].count = 1u;
    count += 1u;
    cases[count].name = "ring-and-disc";
    cases[count].sources[0] = source_of(ring, push, true, -1.0f, 0.0f);
    cases[count].sources[1] = source_of(disc, push, false, 0.8f, 0.4f);
    cases[count].sources[1].chroma = 0.5f;
    cases[count].count = 2u;
    count += 1u;
    for (uint32_t index = 0u; index < count; ++index) {
        check_case(smoke, plain, &cases[index]);
    }
    free(plain);
}

/* The pixels farther than `band` render pixels from the frame's middle (the split's line). */
static void check_views(SmokeWindow *smoke) {
    BTBHGPUSceneDistortionSource ring_source = source_of(BTBH_GPU_SCENE_DISTORTION_RING, BTBH_GPU_SCENE_DISTORTION_PUSH,
        true, 0.0f, 0.0f);
    ring_source.chroma = 0.4f;
    Params one = params_default();
    set_sources(smoke, &ring_source, 1u, 0.0f);
    uint8_t *whole = frame_pixels(smoke, &one, NULL);
    Params split = params_default();
    split.split = true;
    set_sources(smoke, &ring_source, 1u, 0.0f);
    BTBHGPUSceneReport report;
    uint8_t *halves = frame_pixels(smoke, &split, &report);
    assert(report.distortion_sources_drawn == 1u);
    dump("views-whole", whole, WIDTH, HEIGHT);
    dump("views-split", halves, WIDTH, HEIGHT);
    int largest_beyond = 0;
    int largest_within = 0;
    for (uint32_t y = 0u; y < HEIGHT; ++y) {
        for (uint32_t x = 0u; x < WIDTH; ++x) {
            const float from_line = fabsf((float)x + 0.5f - 0.5f * (float)WIDTH);
            const int difference = level_difference(&whole[(y * WIDTH + x) * 4u], &halves[(y * WIDTH + x) * 4u]);
            if (from_line > VIEW_BAND) {
                largest_beyond = difference > largest_beyond ? difference : largest_beyond;
            } else {
                largest_within = difference > largest_within ? difference : largest_within;
            }
        }
    }
    printf("views: a split of one camera against the whole frame: largest %d levels beyond %.1f px of the line, %d "
           "within\n", largest_beyond, (double)VIEW_BAND, largest_within);
    assert(largest_beyond <= 2);
    assert(largest_within > 0);   /* the fade by the line does show */
    free(whole);
    free(halves);

    /* Red for view 0, blue for view 1, a strong ring with chroma on the line: no read crosses it. */
    Params colours = params_default();
    colours.split = true;
    colours.colour_split = true;
    BTBHGPUSceneDistortionSource strong = ring_source;
    strong.radius = 1.6f;
    strong.width = 2.4f;
    strong.strength = 60.0f;
    strong.chroma = 1.0f;
    BTBHGPUSceneDistortionSource lens = source_of(BTBH_GPU_SCENE_DISTORTION_DISC, BTBH_GPU_SCENE_DISTORTION_PUSH,
        false, 0.0f, 2.0f);
    lens.strength = 40.0f;
    const BTBHGPUSceneDistortionSource pair[2] = {strong, lens};
    set_sources(smoke, pair, 2u, 0.0f);
    uint8_t *frame = frame_pixels(smoke, &colours, &report);
    dump("views-colours", frame, WIDTH, HEIGHT);
    uint32_t crossed = 0u;
    for (uint32_t y = 0u; y < HEIGHT; ++y) {
        for (uint32_t x = 0u; x < WIDTH; ++x) {
            const uint8_t *pixel = &frame[(y * WIDTH + x) * 4u];
            const bool left = x < WIDTH / 2u;
            if ((left && pixel[2] > pixel[0]) || (!left && pixel[0] > pixel[2])) {
                crossed += 1u;
            }
        }
    }
    printf("views: red and blue halves under a strong ring on the line: %u pixels show the other half's colour\n",
        crossed);
    assert(crossed == 0u);
    free(frame);

    /* A disc far from the line (on view 0's side): the band by the line, where nothing is displaced, keeps its bytes. */
    Params plain_split = params_default();
    plain_split.split = true;
    uint8_t *without = frame_pixels(smoke, &plain_split, &report);
    assert(report.distortion_sources_drawn == 0u);
    BTBHGPUSceneDistortionSource far_disc = source_of(BTBH_GPU_SCENE_DISTORTION_DISC, BTBH_GPU_SCENE_DISTORTION_PUSH,
        false, -5.0f, 0.0f);
    far_disc.chroma = 0.6f;
    set_sources(smoke, &far_disc, 1u, 0.0f);
    uint8_t *with_far = frame_pixels(smoke, &plain_split, &report);
    assert(report.distortion_sources_drawn == 1u);
    uint32_t band_changed = 0u;
    uint32_t elsewhere_changed = 0u;
    for (uint32_t y = 0u; y < HEIGHT; ++y) {
        for (uint32_t x = 0u; x < WIDTH; ++x) {
            const bool differs = memcmp(&without[(y * WIDTH + x) * 4u], &with_far[(y * WIDTH + x) * 4u], 4u) != 0;
            if (fabsf((float)x + 0.5f - 0.5f * (float)WIDTH) <= VIEW_BAND) {
                band_changed += differs ? 1u : 0u;
            } else {
                elsewhere_changed += differs ? 1u : 0u;
            }
        }
    }
    printf("views: a disc far from the line: %u pixels of the band by the line changed (%u elsewhere)\n", band_changed,
        elsewhere_changed);
    assert(band_changed == 0u);
    assert(elsewhere_changed > 100u);
    free(with_far);

    /* A disc just past the line on view 1's side, in view 0's frustum only (view 1's camera 40 units away): culled by
       view 0's half and view 1's frustum, so the frame is the frame without it, and no source counts. */
    Params apart = params_default();
    apart.split = true;
    apart.view1_offset = 40.0f;
    uint8_t *apart_without = frame_pixels(smoke, &apart, &report);
    BTBHGPUSceneDistortionSource past_line = source_of(BTBH_GPU_SCENE_DISTORTION_DISC, BTBH_GPU_SCENE_DISTORTION_PUSH,
        false, 1.6f, 0.0f);
    past_line.radius = 0.5f;
    set_sources(smoke, &past_line, 1u, 0.0f);
    uint8_t *apart_with = frame_pixels(smoke, &apart, &report);
    const uint32_t apart_changed = differing_pixels(apart_without, apart_with);
    printf("views: a disc just past the line on the other view's side: %u sources drawn, %u pixels changed\n",
        (unsigned)report.distortion_sources_drawn, apart_changed);
    assert(report.distortion_sources_drawn == 0u);
    assert(apart_changed == 0u);
    /* The same disc a little left, across the line: drawn by view 0 (its part on view 0's side). */
    past_line.position[0] = 0.15f;
    set_sources(smoke, &past_line, 1u, 0.0f);
    uint8_t *across = frame_pixels(smoke, &apart, &report);
    assert(report.distortion_sources_drawn == 1u);
    free(across);
    free(apart_with);
    free(apart_without);
    free(without);
}

static void check_fsr3(SmokeWindow *smoke) {
    Params params = params_default();
    params.upscale_mode = BTBH_GPU_SCENE_UPSCALE_FSR3;
    params.render_width = WIDTH / 2u;
    params.render_height = HEIGHT / 2u;
    BTBHGPUSceneReport report;
    /* A frame without sources first: the mask is the transparent pass's alone (none here). */
    uint8_t *frame = frame_pixels(smoke, &params, &report);
    free(frame);
    if (report.upscale_mode != BTBH_GPU_SCENE_UPSCALE_FSR3) {
        printf("fsr3: unavailable on this device (FSR 1 ran): skipped\n");
        return;
    }
    BTBHGPUSceneDistortionSource ring_source = source_of(BTBH_GPU_SCENE_DISTORTION_RING, BTBH_GPU_SCENE_DISTORTION_PUSH,
        true, 0.0f, 0.0f);
    set_sources(smoke, &ring_source, 1u, 0.0f);
    frame = frame_pixels(smoke, &params, &report);
    assert(report.distortion_sources_drawn == 1u);
    free(frame);
    uint32_t field_width = 0u;
    uint32_t field_height = 0u;
    float *field = read_field(smoke, &field_width, &field_height);
    SDL_GPUTexture *field_texture = NULL;
    SDL_GPUTexture *reactive_texture = NULL;
    uint32_t w = 0u;
    uint32_t h = 0u;
    btbh_gpu_window_scene_distortion_textures(smoke->window, &field_texture, &w, &h, &reactive_texture);
    assert(reactive_texture != NULL);
    uint8_t *reactive = read_gpu_texture(smoke->window, reactive_texture, params.render_width, params.render_height, 1u);
    int largest = 0;
    uint32_t marked = 0u;
    for (uint32_t y = 0u; y < params.render_height; ++y) {
        for (uint32_t x = 0u; x < params.render_width; ++x) {
            float f[4];
            field_sample(field, field_width, field_height, ((float)x + 0.5f) / (float)params.render_width,
                ((float)y + 0.5f) / (float)params.render_height, f);
            const float limit = BTBH_DISTORTION_SUM_LIMIT * (float)params.render_height /
                BTBH_DISTORTION_REFERENCE_LINES;
            const float length = fminf(sqrtf(f[0] * f[0] + f[1] * f[1]), limit);
            const float expected = fminf(length / BTBH_DISTORTION_REACTIVE_PIXELS, 1.0f) * 0.8f * 255.0f;
            const int difference = (int)fabsf((float)reactive[y * params.render_width + x] - expected);
            largest = difference > largest ? difference : largest;
            marked += reactive[y * params.render_width + x] > 0u ? 1u : 0u;
        }
    }
    printf("fsr3: the reactive mask against min(1, |d| / 6) x 0.8: largest %d levels; %u pixels marked\n", largest,
        marked);
    assert(largest <= 2);
    assert(marked > 50u);
    free(reactive);
    free(field);
    /* A ring growing over 30 frames under a panning camera. */
    for (uint32_t step = 0u; step < 30u; ++step) {
        BTBHGPUSceneDistortionSource growing = ring_source;
        growing.radius = 0.5f + 0.15f * (float)step;
        growing.strength = 16.0f * (1.0f - (float)step / 30.0f);
        set_sources(smoke, &growing, 1u, (float)step / 60.0f);
        params.eye_x = 0.05f * (float)step;
        frame = frame_pixels(smoke, &params, &report);
        if (step == 15u) {
            dump("fsr3-growing", frame, WIDTH, HEIGHT);
        }
        free(frame);
    }
    printf("fsr3: a growing ring over 30 panned frames presented\n");
}

static void expect_refusal(SmokeWindow *smoke, const BTBHGPUSceneDistortionSource *sources, uint32_t count,
    float time, const char *named) {
    BTBHGPUSceneReport report;
    memset(&report, 0, sizeof(report));
    const BTBHGPUResult result = btbh_gpu_window_scene_set_distortion(smoke->window, sources, count, time, &report);
    if (result != BTBH_GPU_INVALID_ARGUMENT || strstr(report.message, named) == NULL) {
        fprintf(stderr, "expected a refusal naming %s: %d '%s'\n", named, (int)result, report.message);
    }
    assert(result == BTBH_GPU_INVALID_ARGUMENT && report.result == BTBH_GPU_INVALID_ARGUMENT);
    assert(strstr(report.message, named) != NULL);
}

static void check_setter(SmokeWindow *smoke) {
    const BTBHGPUSceneDistortionSource good = source_of(BTBH_GPU_SCENE_DISTORTION_DISC, BTBH_GPU_SCENE_DISTORTION_PUSH,
        false, 0.0f, 0.0f);
    BTBHGPUSceneDistortionSource many[BTBH_GPU_SCENE_MAX_DISTORTION_SOURCES + 1u];
    for (uint32_t index = 0u; index <= BTBH_GPU_SCENE_MAX_DISTORTION_SOURCES; ++index) {
        many[index] = good;
    }
    BTBHGPUSceneDistortionSource bad[3] = {good, good, good};
    bad[2].chroma = 2.0f;
    expect_refusal(smoke, NULL, 1u, 0.0f, "NULL");
    expect_refusal(smoke, many, BTBH_GPU_SCENE_MAX_DISTORTION_SOURCES + 1u, 0.0f, "source_count");
    expect_refusal(smoke, &good, 1u, INFINITY, "time_seconds");
    expect_refusal(smoke, bad, 3u, 0.0f, "Distortion source 2: chroma");
    bad[2] = good;
    bad[2].end[1] = 1.0f;
    expect_refusal(smoke, bad, 3u, 0.0f, "Distortion source 2: end");
    bad[2] = good;
    bad[2].haze_scale = 1.0f;
    expect_refusal(smoke, bad, 3u, 0.0f, "Distortion source 2: haze_scale");
    /* The most is accepted. */
    set_sources(smoke, many, BTBH_GPU_SCENE_MAX_DISTORTION_SOURCES, 0.0f);
    Params params = params_default();
    BTBHGPUSceneReport report;
    uint8_t *frame = frame_pixels(smoke, &params, &report);
    assert(report.distortion_sources_drawn == BTBH_GPU_SCENE_MAX_DISTORTION_SOURCES);
    free(frame);
    /* A refusal after a set drops what was set. */
    set_sources(smoke, &good, 1u, 0.0f);
    expect_refusal(smoke, bad, 3u, 0.0f, "Distortion source 2");
    frame = frame_pixels(smoke, &params, &report);
    assert(report.distortion_sources_drawn == 0u);
    free(frame);
    /* source_count 0 clears. */
    set_sources(smoke, &good, 1u, 0.0f);
    set_sources(smoke, NULL, 0u, 0.0f);
    frame = frame_pixels(smoke, &params, &report);
    assert(report.distortion_sources_drawn == 0u);
    free(frame);
    /* A render into a target neither draws nor takes them. */
    set_sources(smoke, &good, 1u, 0.0f);
    {
        const BTBHGPUSceneLighting lighting = lighting_plain();
        const BTBHGPUScenePost post = post_plain(BTBH_GPU_SCENE_UPSCALE_LINEAR, true);
        const SmokeCamera camera = camera_at(0.0f, 160u, 90u);
        const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        float instances[24];
        instance(instances, white);
        const BTBHGPUSceneGeometryBatch batch = ground_batch(smoke->ground, 0u);
        const ShaderSet *set = &shaders;
        BTBHGPUSceneReport target_report;
        const BTBHGPUResult result = btbh_gpu_window_scene_render_to_target(smoke->window, smoke->portrait,
            set->paths[0], set->paths[1], set->paths[2], set->paths[3], set->paths[4], set->paths[5], set->paths[6],
            set->paths[7], set->paths[8], set->paths[9], set->paths[10], SMOKE_SHADER_FORMAT, 160u, 90u,
            camera.view_projection, camera.right, camera.up, camera.position, &lighting, &post, instances, 1u, 1u,
            &batch, NULL, NULL, 1u, 0u, 0u, 0u, 255u, &target_report);
        if (result != BTBH_GPU_OK) {
            fprintf(stderr, "render_to_target: %s\n", target_report.message);
        }
        assert(result == BTBH_GPU_OK);
        assert(target_report.distortion_sources_drawn == 0u);
    }
    frame = frame_pixels(smoke, &params, &report);
    assert(report.distortion_sources_drawn == 1u);
    free(frame);
    /* A present refused for its arguments leaves them set. */
    set_sources(smoke, &good, 1u, 0.0f);
    Params refused = params_default();
    refused.refused = true;
    BTBHGPUSceneReport refused_report;
    assert(present(smoke, &refused, &refused_report) == BTBH_GPU_INVALID_ARGUMENT);
    frame = frame_pixels(smoke, &params, &report);
    assert(report.distortion_sources_drawn == 1u);
    free(frame);
    printf("setter: refusals named, a refusal drops the sources, 240 drawn, a render into a target leaves them, a "
           "refused present leaves them\n");
}

/* The lazy creation log's entries that name the distortion ("distort": its shaders, pipelines and field, the composite's
   production_hdr_composite_distort.frag); how many a prewarm would have made. print: each one's name. */
static uint32_t distortion_entries(BTBHGPUWindow *window, uint32_t *covered, bool print) {
    BTBHGPULazyCreationEntry entries[64];
    uint32_t found = 0u;
    *covered = 0u;
    for (;;) {
        uint32_t count = 0u;
        assert(btbh_gpu_window_lazy_creation_report(window, entries, 64u, &count) == BTBH_GPU_OK);
        for (uint32_t index = 0u; index < count; ++index) {
            if (strstr(entries[index].name, "distort") != NULL) {
                found += 1u;
                if (print) {
                    printf("  made: %s%s\n", entries[index].name, entries[index].prewarm_covers != 0u ? "" :
                        " (no prewarm makes it)");
                }
                *covered += entries[index].prewarm_covers != 0u ? 1u : 0u;
            }
        }
        if (count < 64u) {
            break;
        }
    }
    return found;
}

static void check_prewarm(SmokeWindow *with, SmokeWindow *without) {
    const BTBHGPUSceneLighting lighting = lighting_plain();
    const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float instances[24];
    instance(instances, white);
    const ShaderSet *set = &shaders;
    SmokeWindow *windows[2] = {with, without};
    for (uint32_t which = 0u; which < 2u; ++which) {
        SmokeWindow *smoke = windows[which];
        const BTBHGPUSceneGeometryBatch batch = ground_batch(smoke->ground, 0u);
        const BTBHGPUScenePost post = post_plain(BTBH_GPU_SCENE_UPSCALE_LINEAR, which == 0u);
        btbh_gpu_window_set_lazy_creation_log_enabled(smoke->window, 1u);
        BTBHGPUSceneReport report;
        const BTBHGPUResult prewarmed = btbh_gpu_window_scene_prewarm(smoke->window, set->paths[0], set->paths[1],
            set->paths[2], set->paths[3], set->paths[4], set->paths[5], set->paths[6], set->paths[7], set->paths[8],
            set->paths[9], set->paths[10], SMOKE_SHADER_FORMAT, WIDTH, HEIGHT, &lighting, &post, &batch, 1u, &report);
        if (prewarmed != BTBH_GPU_OK) {
            fprintf(stderr, "prewarm: %s\n", report.message);
        }
        assert(prewarmed == BTBH_GPU_OK);
        uint32_t covered = 0u;
        (void)distortion_entries(smoke->window, &covered, false);
        const BTBHGPUSceneDistortionSource source = source_of(BTBH_GPU_SCENE_DISTORTION_RING,
            BTBH_GPU_SCENE_DISTORTION_PUSH, true, 0.0f, 0.0f);
        set_sources(smoke, &source, 1u, 0.0f);
        Params params = params_default();
        uint8_t *frame = frame_pixels(smoke, &params, &report);
        assert(report.distortion_sources_drawn == 1u);
        free(frame);
        const uint32_t made = distortion_entries(smoke->window, &covered, which == 1u);
        printf("prewarm %s the flag: the first frame with sources made %u things of the distortion's (%u a prewarm "
               "makes)\n", which == 0u ? "with" : "without", made, covered);
        if (which == 0u) {
            assert(made == 0u);
        } else {
            /* The field, the source pass's two shaders and its pipeline, the displaced composite's shader and pipeline. */
            assert(made >= 6u && covered == made);
        }
        btbh_gpu_window_set_lazy_creation_log_enabled(smoke->window, 0u);
    }
}

typedef struct ShaderCopy {
    char from[1100];
    char to[1100];
    uint32_t copied;
} ShaderCopy;

static SDL_EnumerationResult SDLCALL copy_shader_file(void *userdata, const char *dirname, const char *fname) {
    ShaderCopy *copy = (ShaderCopy *)userdata;
    (void)dirname;
    char from[2300];
    char to[2300];
    (void)snprintf(from, sizeof(from), "%s/%s", copy->from, fname);
    (void)snprintf(to, sizeof(to), "%s/%s", copy->to, fname);
    SDL_PathInfo info;
    if (SDL_GetPathInfo(from, &info) && info.type == SDL_PATHTYPE_FILE) {
        assert(SDL_CopyFile(from, to));
        copy->copied += 1u;
    }
    return SDL_ENUM_CONTINUE;
}

static void check_missing_shader(BTBHGPUDevice *device) {
    ShaderCopy copy;
    memset(&copy, 0, sizeof(copy));
    (void)snprintf(copy.from, sizeof(copy.from), "%s", BTBH_GPU_SMOKE_SHADER_DIR);
    (void)snprintf(copy.to, sizeof(copy.to), "%s/../distortion_missing", BTBH_GPU_SMOKE_SHADER_DIR);
    (void)SDL_CreateDirectory(copy.to);
    assert(SDL_EnumerateDirectory(copy.from, copy_shader_file, &copy));
    assert(copy.copied > 20u);
    char missing[1200];
    (void)snprintf(missing, sizeof(missing), "%s/production_distortion.frag%s", copy.to, SMOKE_SHADER_EXTENSION);
    assert(SDL_RemovePath(missing));
    ShaderSet partial;
    shader_set_in(&partial, copy.to);
    const BTBHGPUWindowDescriptor descriptor = {
        .title = "BTBH distortion smoke (missing shader)",
        .width = WIDTH,
        .height = HEIGHT,
        .hidden = 1,
        .high_pixel_density = 0,
    };
    BTBHGPUWindowReport window_report;
    memset(&window_report, 0, sizeof(window_report));
    SmokeWindow smoke;
    memset(&smoke, 0, sizeof(smoke));
    smoke.window = btbh_gpu_window_create(&descriptor, &window_report);
    assert(smoke.window != NULL);
    assert(btbh_gpu_window_claim(device, smoke.window, &window_report) == BTBH_GPU_OK);
    smoke.ground = create_ground(smoke.window);
    BTBHGPUTextureReport texture_report;
    smoke.frame = btbh_gpu_window_render_target_create(smoke.window, WIDTH, HEIGHT, BTBH_GPU_TEXTURE_FILTER_LINEAR,
        &texture_report);
    assert(smoke.frame != NULL);
    Params params = params_default();
    params.shaders = &partial;
    /* Without a source the frame needs none of the distortion's files. */
    uint8_t *frame = frame_pixels(&smoke, &params, NULL);
    free(frame);
    const BTBHGPUSceneDistortionSource source = source_of(BTBH_GPU_SCENE_DISTORTION_DISC,
        BTBH_GPU_SCENE_DISTORTION_PUSH, false, 0.0f, 0.0f);
    set_sources(&smoke, &source, 1u, 0.0f);
    BTBHGPUSceneReport report;
    const BTBHGPUResult result = present(&smoke, &params, &report);
    if (result != BTBH_GPU_INVALID_ARGUMENT || strstr(report.message, "production_distortion.frag") == NULL) {
        fprintf(stderr, "missing shader: %d '%s'\n", (int)result, report.message);
    }
    assert(result == BTBH_GPU_INVALID_ARGUMENT);
    assert(strstr(report.message, "production_distortion.frag") != NULL);
    assert(!smoke.window->pending_scene_ready && smoke.window->pending_command == NULL);
    btbh_gpu_texture_destroy(smoke.frame);
    btbh_gpu_custom_mesh_destroy(smoke.ground);
    btbh_gpu_window_destroy(smoke.window);
    printf("a shader directory without production_distortion.frag: a frame with sources refused, naming it\n");
}

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
    smoke.ground = create_ground(smoke.window);
    BTBHGPUTextureReport texture_report;
    smoke.frame = btbh_gpu_window_render_target_create(smoke.window, WIDTH, HEIGHT, BTBH_GPU_TEXTURE_FILTER_LINEAR,
        &texture_report);
    assert(smoke.frame != NULL);
    smoke.portrait = btbh_gpu_window_render_target_create(smoke.window, 160u, 90u, BTBH_GPU_TEXTURE_FILTER_LINEAR,
        &texture_report);
    assert(smoke.portrait != NULL);
    return smoke;
}

int main(void) {
    (void)setvbuf(stdout, NULL, _IONBF, 0);
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
    SmokeWindow a = open_window(device, "BTBH distortion smoke A");
    SmokeWindow b = open_window(device, "BTBH distortion smoke B");
    SmokeWindow c = open_window(device, "BTBH distortion smoke C");

    check_identity(&a);
    check_prewarm(&b, &c);
    check_sources(&a);
    check_views(&a);
    check_fsr3(&b);
    check_setter(&a);
    check_missing_shader(device);

    SmokeWindow *windows[3] = {&a, &b, &c};
    for (uint32_t index = 0u; index < 3u; ++index) {
        btbh_gpu_texture_destroy(windows[index]->frame);
        btbh_gpu_texture_destroy(windows[index]->portrait);
        btbh_gpu_custom_mesh_destroy(windows[index]->ground);
        btbh_gpu_window_destroy(windows[index]->window);
    }
    btbh_gpu_device_shutdown(device);
    btbh_gpu_device_destroy(device);
    printf("distortion smoke passed\n");
    return 0;
}
