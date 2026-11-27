/* A measurement, not a test (0.16.0, RFC 0014): what scene views (btbh_gpu_window_scene_present_views) cost against one
 * view, and against 0.15.0's way of drawing Be The Bullet Hell v2's co-op split (a whole present of the first view and
 * a target view of the second, composed).
 *
 * The scene is btbh_gpu_target_view_bench's city: 48 x 48 lit boxes (heights 0.5 to 6) in 96 instanced batches (half a
 * row of boxes each) on a ground, under a sun with two maps (primary 2048^2 and secondary), HBAO with contact AO, SSR,
 * bloom, fog, vignette and SMAA medium, ACES, rendered at the window's size (3840 x 2160 by default) with FSR 1 asked
 * for (at the same size it does nothing). Two cameras with one angle and lens, 16 units apart, as v2's split's.
 * Each view draws the batches its own frustum lets in (and, split, its half-plane: VP^T (a, b, 0, c)); 0.15.0's way
 * draws in both calls the union of the two frustums, as v2 does.
 *
 * Configurations, each timed in every round, in another order each round:
 * - present: one view through btbh_gpu_window_scene_present_instances_with_visibility;
 * - views-one: the same through btbh_gpu_window_scene_present_views;
 * - views-vertical, views-diagonal: two views split at the frame's middle, vertically and at BTBH_VIEWS_BENCH_ANGLE
 *   degrees from vertical (default 30);
 * - 0150-vertical, 0150-diagonal: btbh_gpu_window_scene_render_to_target_view (view_id 1, kept) of the second camera
 *   into a target of the frame's size with the rect of its half's bounding box grown by 96 pixels, then the present of
 *   the first camera over the whole frame, and the second view's half's box blitted over it (v2 lays the half's polygon).
 * Each frame is copied out of the window into a render target (btbh_gpu_window_copy_pending_scene_texture), in every
 * configuration alike. A sample is BTBH_VIEWS_BENCH_BATCH (default 8) frames submitted back to back and a wait for the
 * GPU: the GPU's time a frame is the sample's wall time over the batch. BTBH_VIEWS_BENCH_ROUNDS (default 22) rounds; the
 * first 3 are dropped. Shadow update periods of BTBH_VIEWS_BENCH_PERIOD (default 2, v2's while split) frames;
 * BTBH_VIEWS_BENCH_OWN_SUN=1 gives the second view a sun matrix of its own (its own map, taking turns). The CPU a frame
 * is the calls' wall time with the GPU idle before each (median of 12 frames). BTBH_VIEWS_BENCH_SIZE=WxH,
 * BTBH_VIEWS_BENCH_DETAIL=<n> (default 8: 768 triangles a box, 1.8 million in the scene). BTBH_VIEWS_BENCH_BAND=1 first
 * measures the seam's band: the vertical split against its two cameras' one-view frames joined at the line, read back,
 * with the whole chain, without bloom, and without bloom and SSR. With it, BTBH_VIEWS_BENCH_DUMP=<dir> (0.16.1) writes
 * each variant's three frames there as PNGs (band_<variant>_first, _second and _split). BTBH_VIEWS_BENCH_SSR=<n> (0.16.1)
 * shows the reflections, to see them by the line: intensity n (default 0.5) and Be The Bullet Hell v2's flags,
 * SSR_SURFACE_REFLECTIVITY and SSR_FACING_HITS, in every frame. (Without the first the scene's targets mark every
 * surface unlit, and the reflections the bench times by default reflect nothing.)
 *
 * Variants (0.16.0, for the cost log's 4K sample): BTBH_VIEWS_BENCH_UPSCALE=fsr3 runs FSR 3 at the frame's size (render
 * scale 1) in place of SMAA, as Be The Bullet Hell v2's 4K High does; BTBH_VIEWS_BENCH_CAS=<sharpness> turns CAS on;
 * BTBH_VIEWS_BENCH_CONFIGS=<names, comma-separated> runs only those configurations.
 * With BTBH_GPU_COST_LOG=1 (the renderer's cost log; BTBH_GPU_COST_PASSES=1 adds its pass frames) each frame ends with a
 * canvas present instead of the copy (the canvas submits the frame and the log times it; 0.15.0's way blits no box),
 * and a line "BENCH sample <configuration> <round>" comes before each sample's frames, so that the log's lines can be
 * told apart. The log waits for every frame, so the bench's own timings mean nothing then, and are not printed.
 *
 * Output: one line a configuration (median, 10th and 90th percentiles, min, max in ms a frame), the CPU a frame and the
 * draws (each view's, or each call's). With no device it is skipped (77). */
#include "btbh_gpu/btbh_gpu.h"
#include "btbh_gpu_internal.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_timer.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BENCH_SKIP 77
#define GRID 48u
#define BATCHES 96u
#define CONFIGS 6u
#define MARGIN 96u

#if defined(__APPLE__)
#define BENCH_SHADER_EXTENSION ".metallib"
#define BENCH_SHADER_FORMAT BTBH_GPU_SHADER_FORMAT_METALLIB
#define BENCH_BACKEND BTBH_GPU_BACKEND_METAL
#else
#define BENCH_SHADER_EXTENSION ".spv"
#define BENCH_SHADER_FORMAT BTBH_GPU_SHADER_FORMAT_SPIRV
#define BENCH_BACKEND BTBH_GPU_BACKEND_VULKAN
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
/* BTBH_GPU_COST_LOG=1: each frame ends with a canvas present (the cost log times it) instead of the copy. */
static bool cost_log_mode;

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

static BenchCamera look_at(const float eye[3], const float centre[3], float fov_y, float aspect,
    const float projection_override[16]) {
    BenchCamera camera;
    memset(&camera, 0, sizeof(camera));
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
        const float near_plane = 0.2f;
        const float far_plane = 200.0f;
        const float h = 1.0f / tanf(0.5f * fov_y);
        const float r = far_plane / (near_plane - far_plane);
        const float perspective[16] = {
            h / aspect, 0.0f, 0.0f, 0.0f,
            0.0f, h, 0.0f, 0.0f,
            0.0f, 0.0f, r, -1.0f,
            0.0f, 0.0f, r * near_plane, 0.0f,
        };
        memcpy(projection, perspective, sizeof(projection));
    }
    multiply(projection, view, camera.view_projection);
    memcpy(camera.right, side, sizeof(side));
    memcpy(camera.up, up, sizeof(up));
    memcpy(camera.position, eye, sizeof(camera.position));
    return camera;
}

static void orthographic_sun(const float sun[3], const float centre[3], float half_extent, float out[16]) {
    const float rcp = 1.0f / half_extent;
    const float range = 1.0f / (1.0f - 120.0f);
    const float orthographic[16] = {
        rcp, 0.0f, 0.0f, 0.0f,
        0.0f, rcp, 0.0f, 0.0f,
        0.0f, 0.0f, range, 0.0f,
        0.0f, 0.0f, range * 1.0f, 1.0f,
    };
    const float eye[3] = {centre[0] + sun[0], centre[1] + sun[1], centre[2] + sun[2]};
    const BenchCamera light = look_at(eye, centre, 1.0f, 1.0f, orthographic);
    memcpy(out, light.view_projection, sizeof(float) * 16u);
}

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

/* A unit box (-0.5..0.5), each face cut into detail x detail quads; 24 floats a vertex. */
static BTBHGPUCustomMesh *create_cube(BTBHGPUWindow *window, uint32_t detail) {
    static const float faces[6][3] = {
        {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
    };
    const uint32_t side = detail + 1u;
    const uint32_t vertex_count = 6u * side * side;
    const uint32_t index_count = 6u * detail * detail * 6u;
    float *vertices = calloc((size_t)vertex_count * 24u, sizeof(float));
    uint32_t *indices = calloc(index_count, sizeof(uint32_t));
    if (vertices == NULL || indices == NULL) {
        free(vertices);
        free(indices);
        return NULL;
    }
    uint32_t next_index = 0u;
    for (uint32_t face = 0u; face < 6u; ++face) {
        const float *n = faces[face];
        float u[3] = {n[1], n[2], n[0]};
        float v[3];
        cross3(n, u, v);
        const uint32_t base = face * side * side;
        for (uint32_t j = 0u; j < side; ++j) {
            for (uint32_t i = 0u; i < side; ++i) {
                const float su = -0.5f + (float)i / (float)detail;
                const float sv = -0.5f + (float)j / (float)detail;
                float *vertex = &vertices[(size_t)(base + j * side + i) * 24u];
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
        }
        for (uint32_t j = 0u; j < detail; ++j) {
            for (uint32_t i = 0u; i < detail; ++i) {
                const uint32_t a = base + j * side + i;
                const uint32_t b = a + 1u;
                const uint32_t c = a + side + 1u;
                const uint32_t d = a + side;
                const uint32_t quad[6] = {a, c, b, a, d, c};
                memcpy(&indices[next_index], quad, sizeof(quad));
                next_index += 6u;
            }
        }
    }
    BTBHGPUCustomMeshReport report;
    memset(&report, 0, sizeof(report));
    BTBHGPUCustomMesh *mesh = btbh_gpu_window_custom_mesh_create(window, vertices,
        (uint32_t)(sizeof(float) * 24u * vertex_count), vertex_count, indices, index_count, &report);
    free(vertices);
    free(indices);
    return mesh;
}

static int compare_doubles(const void *a, const void *b) {
    const double x = *(const double *)a;
    const double y = *(const double *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* Whether the box [minimum, maximum] may be seen through view_projection (its six planes, Gribb and Hartmann) and, when
   clip isn't NULL, on the clip line's side (the plane VP^T (a, b, 0, c), a b c the line in clip space). */
static bool box_visible(const float vp[16], const float clip_plane[4], const float minimum[3], const float maximum[3]) {
    float planes[7][4];
    uint32_t plane_count = 6u;
    for (uint32_t axis = 0u; axis < 3u; ++axis) {
        for (uint32_t sign = 0u; sign < 2u; ++sign) {
            float *plane = planes[axis * 2u + sign];
            for (uint32_t column = 0u; column < 4u; ++column) {
                const float w = vp[column * 4u + 3u];
                const float value = vp[column * 4u + axis];
                plane[column] = axis == 2u && sign == 0u ? value : (sign == 0u ? w + value : w - value);
            }
        }
    }
    if (clip_plane != NULL) {
        for (uint32_t column = 0u; column < 4u; ++column) {
            planes[6][column] = clip_plane[0] * vp[column * 4u + 0u] + clip_plane[1] * vp[column * 4u + 1u] +
                clip_plane[2] * vp[column * 4u + 3u];
        }
        plane_count = 7u;
    }
    for (uint32_t index = 0u; index < plane_count; ++index) {
        const float *plane = planes[index];
        const float x = plane[0] >= 0.0f ? maximum[0] : minimum[0];
        const float y = plane[1] >= 0.0f ? maximum[1] : minimum[1];
        const float z = plane[2] >= 0.0f ? maximum[2] : minimum[2];
        if (plane[0] * x + plane[1] * y + plane[2] * z + plane[3] < 0.0f) {
            return false;
        }
    }
    return true;
}

typedef struct BenchScene {
    BTBHGPUWindow *window;
    BTBHGPUTexture *frame;
    BTBHGPUTexture *target;
    float *instances;
    uint32_t instance_count;
    BTBHGPUSceneGeometryBatch batches[BATCHES + 1u];
    float batch_minimum[BATCHES + 1u][3];
    float batch_maximum[BATCHES + 1u][3];
    BTBHGPUSceneLighting lighting;
    BTBHGPUSceneLighting second_lighting;
    BTBHGPUScenePost post;
    BenchCamera cameras[2];
    uint32_t width;
    uint32_t height;
    /* Per configuration: each view's visibility, the union's, the views, the target view's rect, the blit's box. */
    uint8_t visibility[CONFIGS][2][BATCHES + 1u];
    uint8_t union_visibility[BATCHES + 1u];
    BTBHGPUSceneView views[CONFIGS][2];
    uint32_t view_counts[CONFIGS];
    BTBHGPUSceneTargetView target_views[CONFIGS];
    SDL_Rect blit_box[CONFIGS];
    /* The draws (each view's or each call's) of the last frame. */
    uint32_t draws[CONFIGS][2];
} BenchScene;

static const char *const names[CONFIGS] = {
    "present", "views-one", "views-vertical", "views-diagonal", "0150-vertical", "0150-diagonal"
};

static const char *const *paths(void) {
    static const char *list[11];
    for (int index = 0; index < 11; ++index) {
        list[index] = shader_paths[index];
    }
    return list;
}

/* One frame of `config`: its calls (timed into cpu_nanoseconds), then the frame copied out (and, 0.15.0's way, the
   second view's box blitted over it). False on a failure, printed. */
static bool bench_frame(BenchScene *scene, uint32_t config, uint64_t *cpu_nanoseconds) {
    const char *const *p = paths();
    BTBHGPUSceneReport report;
    BTBHGPUSceneViewReport view_reports[2];
    const uint64_t started = SDL_GetTicksNS();
    BTBHGPUResult result = BTBH_GPU_OK;
    if (config >= 4u) {
        const BenchCamera *second = &scene->cameras[1];
        result = btbh_gpu_window_scene_render_to_target_view(scene->window, scene->target,
            p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10],
            BENCH_SHADER_FORMAT, scene->width, scene->height,
            second->view_projection, second->right, second->up, second->position,
            &scene->second_lighting, &scene->post, scene->instances, scene->instance_count, scene->instance_count,
            scene->batches, scene->union_visibility, NULL, BATCHES + 1u, 30u, 40u, 55u, 255u,
            &scene->target_views[config], &report);
        if (result != BTBH_GPU_OK) {
            fprintf(stderr, "%s: target view: %s\n", names[config], report.message);
            return false;
        }
        scene->draws[config][1] = report.primitive_draw_count;
    }
    if (config == 0u || config >= 4u) {
        const BenchCamera *first = &scene->cameras[0];
        result = btbh_gpu_window_scene_present_instances_with_visibility(scene->window,
            p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10],
            BENCH_SHADER_FORMAT, scene->width, scene->height,
            first->view_projection, first->right, first->up, first->position,
            &scene->lighting, &scene->post, scene->instances, scene->instance_count, scene->instance_count,
            scene->batches, config == 0u ? scene->visibility[0][0] : scene->union_visibility, NULL, BATCHES + 1u,
            30u, 40u, 55u, 255u, &report);
        scene->draws[config][0] = report.primitive_draw_count;
    } else {
        result = btbh_gpu_window_scene_present_views(scene->window,
            p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10],
            BENCH_SHADER_FORMAT, scene->width, scene->height,
            scene->views[config], scene->view_counts[config],
            &scene->lighting, &scene->post, scene->instances, scene->instance_count, scene->instance_count,
            scene->batches, NULL, NULL, BATCHES + 1u, 30u, 40u, 55u, 255u, view_reports, &report);
        if (result == BTBH_GPU_OK) {
            scene->draws[config][0] = view_reports[0].primitive_draw_count;
            scene->draws[config][1] = scene->view_counts[config] > 1u ? view_reports[1].primitive_draw_count : 0u;
        }
    }
    if (result != BTBH_GPU_OK) {
        fprintf(stderr, "%s: present: %s\n", names[config], report.message);
        return false;
    }
    if (cpu_nanoseconds != NULL) {
        *cpu_nanoseconds = SDL_GetTicksNS() - started;
    }
    if (cost_log_mode) {
        BTBHGPUCanvasPass pass;
        memset(&pass, 0, sizeof(pass));
        BTBHGPUCanvasReport canvas_report;
        if (btbh_gpu_window_canvas_present(scene->window, canvas_shader_paths[0], canvas_shader_paths[1],
                canvas_shader_paths[2], BENCH_SHADER_FORMAT, NULL, 0u, NULL, 0u, &pass, 1u, &canvas_report) !=
            BTBH_GPU_OK) {
            fprintf(stderr, "%s: canvas: %s\n", names[config], canvas_report.message);
            return false;
        }
        return true;
    }
    BTBHGPUTextureReport copied;
    if (btbh_gpu_window_copy_pending_scene_texture(scene->window, scene->frame, &copied) != BTBH_GPU_OK) {
        fprintf(stderr, "%s: copy: %s\n", names[config], copied.message);
        return false;
    }
    if (config >= 4u) {
        SDL_GPUDevice *device = scene->window->device->gpu;
        SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(device);
        if (command == NULL) {
            return false;
        }
        const SDL_Rect *box = &scene->blit_box[config];
        SDL_GPUBlitInfo blit;
        SDL_zero(blit);
        blit.source.texture = scene->target->texture;
        blit.source.x = (uint32_t)box->x;
        blit.source.y = (uint32_t)box->y;
        blit.source.w = (uint32_t)box->w;
        blit.source.h = (uint32_t)box->h;
        blit.destination = blit.source;
        blit.destination.texture = scene->frame->texture;
        blit.load_op = SDL_GPU_LOADOP_LOAD;
        blit.filter = SDL_GPU_FILTER_NEAREST;
        SDL_BlitGPUTexture(command, &blit);
        if (!SDL_SubmitGPUCommandBuffer(command)) {
            return false;
        }
    }
    return true;
}

/* The texture's bytes (4 a texel), read back. */
static uint8_t *read_texture(BTBHGPUWindow *window, BTBHGPUTexture *texture) {
    SDL_GPUDevice *device = window->device->gpu;
    const uint32_t size = texture->width * texture->height * 4u;
    SDL_GPUTransferBufferCreateInfo info;
    SDL_zero(info);
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    info.size = size;
    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(device, &info);
    SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(device);
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
    (void)SDL_WaitForGPUFences(device, true, &fence, 1u);
    SDL_ReleaseGPUFence(device, fence);
    uint8_t *pixels = malloc(size);
    const void *mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
    memcpy(pixels, mapped, size);
    SDL_UnmapGPUTransferBuffer(device, transfer);
    SDL_ReleaseGPUTransferBuffer(device, transfer);
    return pixels;
}

/* BTBH_VIEWS_BENCH_DUMP=<dir>: a band frame read back (the texture's bytes) as <dir>/band_<variant>_<which>.png. */
static void dump_frame(const BTBHGPUTexture *texture, const uint8_t *pixels, uint32_t variant, const char *which) {
    const char *directory = SDL_getenv("BTBH_VIEWS_BENCH_DUMP");
    if (directory == NULL || directory[0] == '\0') {
        return;
    }
    const size_t size = (size_t)texture->width * texture->height * 4u;
    uint8_t *rgba = malloc(size);
    if (rgba == NULL) {
        return;
    }
    memcpy(rgba, pixels, size);
    if (texture->format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM ||
        texture->format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB) {
        for (size_t index = 0u; index < size; index += 4u) {
            const uint8_t blue = rgba[index];
            rgba[index] = rgba[index + 2u];
            rgba[index + 2u] = blue;
        }
    }
    static const char *const variant_names[3] = {"chain", "nobloom", "nossr"};
    char path[1200];
    (void)snprintf(path, sizeof(path), "%s/band_%s_%s.png", directory, variant_names[variant], which);
    if (btbh_gpu_write_png_rgba8(path, texture->width, texture->height, rgba) != BTBH_GPU_OK) {
        fprintf(stderr, "dump: can't write %s\n", path);
    }
    free(rgba);
}

static void wait_idle(BenchScene *scene) {
    (void)SDL_WaitForGPUIdle(scene->window->device->gpu);
}

/* The half beyond a line through the centre at `degrees` from vertical (the half on its right, x growing; 0 a vertical
   line): its clip (output pixels) and its bounding box. */
static void half_beyond(uint32_t width, uint32_t height, double degrees, float clip[3], double box[4]) {
    const double radians = degrees * 3.14159265358979 / 180.0;
    const double nx = cos(radians);
    const double ny = -sin(radians);
    const double cx = 0.5 * (double)width;
    const double cy = 0.5 * (double)height;
    clip[0] = (float)nx;
    clip[1] = (float)ny;
    clip[2] = (float)(-(nx * cx + ny * cy));
    double minimum_x = (double)width;
    double minimum_y = (double)height;
    double maximum_x = 0.0;
    double maximum_y = 0.0;
    const double corners[4][2] = {{0.0, 0.0}, {(double)width, 0.0}, {0.0, (double)height},
        {(double)width, (double)height}};
    for (uint32_t index = 0u; index < 4u; ++index) {
        const double side = (corners[index][0] - cx) * nx + (corners[index][1] - cy) * ny;
        if (side >= 0.0) {
            minimum_x = fmin(minimum_x, corners[index][0]);
            maximum_x = fmax(maximum_x, corners[index][0]);
            minimum_y = fmin(minimum_y, corners[index][1]);
            maximum_y = fmax(maximum_y, corners[index][1]);
        }
    }
    for (uint32_t edge = 0u; edge < 4u; ++edge) {
        double px;
        double py;
        if (edge < 2u) {
            py = edge == 0u ? 0.0 : (double)height;
            if (fabs(nx) < 1.0e-9) {
                continue;
            }
            px = cx - (py - cy) * ny / nx;
        } else {
            px = edge == 2u ? 0.0 : (double)width;
            if (fabs(ny) < 1.0e-9) {
                continue;
            }
            py = cy - (px - cx) * nx / ny;
        }
        if (px < -0.5 || px > (double)width + 0.5 || py < -0.5 || py > (double)height + 0.5) {
            continue;
        }
        minimum_x = fmin(minimum_x, px);
        maximum_x = fmax(maximum_x, px);
        minimum_y = fmin(minimum_y, py);
        maximum_y = fmax(maximum_y, py);
    }
    box[0] = minimum_x;
    box[1] = minimum_y;
    box[2] = maximum_x;
    box[3] = maximum_y;
}

int main(void) {
    for (int index = 0; index < 11; ++index) {
        (void)snprintf(shader_paths[index], sizeof(shader_paths[index]), "%s/%s%s", BTBH_GPU_SMOKE_SHADER_DIR,
            shader_names[index], BENCH_SHADER_EXTENSION);
    }
    for (int index = 0; index < 3; ++index) {
        (void)snprintf(canvas_shader_paths[index], sizeof(canvas_shader_paths[index]), "%s/%s%s",
            BTBH_GPU_SMOKE_SHADER_DIR, canvas_shader_names[index], BENCH_SHADER_EXTENSION);
    }
    const char *cost_text = SDL_getenv("BTBH_GPU_COST_LOG");
    cost_log_mode = cost_text != NULL && strcmp(cost_text, "1") == 0;
    const char *upscale_text = SDL_getenv("BTBH_VIEWS_BENCH_UPSCALE");
    const bool fsr3 = upscale_text != NULL && strcmp(upscale_text, "fsr3") == 0;
    const char *cas_text = SDL_getenv("BTBH_VIEWS_BENCH_CAS");
    const float cas_sharpness = cas_text != NULL ? (float)atof(cas_text) : 0.0f;
    /* The configurations to run (all by default). */
    bool enabled[CONFIGS];
    const char *configs_text = SDL_getenv("BTBH_VIEWS_BENCH_CONFIGS");
    for (uint32_t config = 0u; config < CONFIGS; ++config) {
        enabled[config] = true;
        if (configs_text != NULL && configs_text[0] != '\0') {
            const size_t length = strlen(names[config]);
            enabled[config] = false;
            for (const char *at = strstr(configs_text, names[config]); at != NULL; at = strstr(at + 1, names[config])) {
                if ((at == configs_text || at[-1] == ',') && (at[length] == '\0' || at[length] == ',')) {
                    enabled[config] = true;
                }
            }
        }
    }
    uint32_t width = 3840u;
    uint32_t height = 2160u;
    const char *size = SDL_getenv("BTBH_VIEWS_BENCH_SIZE");
    if (size != NULL) {
        char *end = NULL;
        const unsigned long w = strtoul(size, &end, 10);
        const unsigned long h = end != NULL && *end == 'x' ? strtoul(end + 1, NULL, 10) : 0ul;
        if (w >= 64ul && h >= 64ul && w <= 8192ul && h <= 8192ul) {
            width = (uint32_t)w;
            height = (uint32_t)h;
        }
    }
    const char *angle_text = SDL_getenv("BTBH_VIEWS_BENCH_ANGLE");
    const double angle_degrees = angle_text != NULL ? atof(angle_text) : 30.0;
    const char *period_text = SDL_getenv("BTBH_VIEWS_BENCH_PERIOD");
    const uint32_t period = period_text != NULL ? (uint32_t)atoi(period_text) : 2u;
    const char *batch_text = SDL_getenv("BTBH_VIEWS_BENCH_BATCH");
    const uint32_t frames_per_sample = batch_text != NULL && atoi(batch_text) > 0 ? (uint32_t)atoi(batch_text) : 8u;
    const char *rounds_text = SDL_getenv("BTBH_VIEWS_BENCH_ROUNDS");
    const uint32_t rounds = rounds_text != NULL && atoi(rounds_text) > 3 ? (uint32_t)atoi(rounds_text) : 22u;
    const char *own_text = SDL_getenv("BTBH_VIEWS_BENCH_OWN_SUN");
    const bool own_sun = own_text != NULL && own_text[0] == '1';
    const char *detail_text = SDL_getenv("BTBH_VIEWS_BENCH_DETAIL");
    const uint32_t detail = detail_text != NULL && atoi(detail_text) > 0 && atoi(detail_text) <= 64
        ? (uint32_t)atoi(detail_text)
        : 8u;

    (void)SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
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
    const BTBHGPUWindowDescriptor descriptor = {
        .title = "BTBH scene views bench",
        .width = (int)width,
        .height = (int)height,
        .hidden = 1,
        .high_pixel_density = 0,
    };
    BTBHGPUWindowReport window_report;
    memset(&window_report, 0, sizeof(window_report));
    static BenchScene scene;
    memset(&scene, 0, sizeof(scene));
    scene.window = btbh_gpu_window_create(&descriptor, &window_report);
    if (scene.window == NULL || btbh_gpu_window_claim(device, scene.window, &window_report) != BTBH_GPU_OK) {
        fprintf(stderr, "window: %s\n", window_report.message);
        return 1;
    }
    int pixel_width = 0;
    int pixel_height = 0;
    (void)SDL_GetWindowSizeInPixels(scene.window->window, &pixel_width, &pixel_height);
    if (pixel_width <= 0 || pixel_height <= 0) {
        fprintf(stderr, "window: no size\n");
        return 1;
    }
    /* The frame is the window's drawable size (a hidden window may be held to the desktop's). */
    scene.width = (uint32_t)pixel_width;
    scene.height = (uint32_t)pixel_height;
    width = scene.width;
    height = scene.height;
    BTBHGPUCustomMesh *cube = create_cube(scene.window, detail);
    BTBHGPUTextureReport texture_report;
    scene.frame = btbh_gpu_window_render_target_create(scene.window, width, height, BTBH_GPU_TEXTURE_FILTER_LINEAR,
        &texture_report);
    scene.target = btbh_gpu_window_render_target_create(scene.window, width, height, BTBH_GPU_TEXTURE_FILTER_LINEAR,
        &texture_report);
    if (cube == NULL || scene.frame == NULL || scene.target == NULL) {
        fprintf(stderr, "resources: %s\n", texture_report.message);
        return 1;
    }

    /* The block: GRID x GRID boxes, BATCHES batches of the same mesh (half a row each), then the ground. */
    scene.instance_count = GRID * GRID + 1u;
    scene.instances = calloc((size_t)scene.instance_count * 24u, sizeof(float));
    uint32_t seed = 12345u;
    const uint32_t per_batch = GRID * GRID / BATCHES;
    for (uint32_t index = 0u; index <= BATCHES; ++index) {
        for (uint32_t axis = 0u; axis < 3u; ++axis) {
            scene.batch_minimum[index][axis] = 1.0e9f;
            scene.batch_maximum[index][axis] = -1.0e9f;
        }
    }
    for (uint32_t z = 0u; z < GRID; ++z) {
        for (uint32_t x = 0u; x < GRID; ++x) {
            seed = seed * 1664525u + 1013904223u;
            const float h = 0.5f + 5.5f * (float)(seed >> 8) / 16777216.0f;
            seed = seed * 1664525u + 1013904223u;
            const float tint = 0.45f + 0.4f * (float)(seed >> 8) / 16777216.0f;
            const float colour[4] = {tint, tint * 0.95f, tint * 0.85f, 1.0f};
            const float px = -36.0f + 1.5f * (float)x;
            const float pz = -36.0f + 1.5f * (float)z;
            instance(&scene.instances[(size_t)(z * GRID + x) * 24u], px, 0.5f * h, pz, 1.1f, h, 1.1f, colour);
            const uint32_t batch = (z * GRID + x) / per_batch;
            const float minimum[3] = {px - 0.55f, 0.0f, pz - 0.55f};
            const float maximum[3] = {px + 0.55f, h, pz + 0.55f};
            for (uint32_t axis = 0u; axis < 3u; ++axis) {
                scene.batch_minimum[batch][axis] = fminf(scene.batch_minimum[batch][axis], minimum[axis]);
                scene.batch_maximum[batch][axis] = fmaxf(scene.batch_maximum[batch][axis], maximum[axis]);
            }
        }
    }
    const float ground[4] = {0.4f, 0.42f, 0.38f, 1.0f};
    instance(&scene.instances[(size_t)(GRID * GRID) * 24u], 0.0f, -0.05f, 0.0f, 90.0f, 0.1f, 90.0f, ground);
    const float ground_minimum[3] = {-45.0f, -0.1f, -45.0f};
    const float ground_maximum[3] = {45.0f, 0.0f, 45.0f};
    memcpy(scene.batch_minimum[BATCHES], ground_minimum, sizeof(ground_minimum));
    memcpy(scene.batch_maximum[BATCHES], ground_maximum, sizeof(ground_maximum));
    for (uint32_t index = 0u; index <= BATCHES; ++index) {
        BTBHGPUSceneGeometryBatch *batch = &scene.batches[index];
        memset(batch, 0, sizeof(*batch));
        batch->kind = BTBH_GPU_SCENE_GEOMETRY_CUSTOM_MESH;
        batch->custom_mesh = cube;
        batch->material_slot = -1;
        batch->first_instance = index < BATCHES ? index * per_batch : GRID * GRID;
        batch->instance_count = index < BATCHES ? per_batch : 1u;
        batch->albedo_red = 255u;
        batch->albedo_green = 255u;
        batch->albedo_blue = 255u;
        batch->albedo_alpha = 255u;
        batch->uv_scale_x = 1.0f;
        batch->uv_scale_y = 1.0f;
        batch->roughness = index < BATCHES ? 0.5f : 0.25f;
        batch->metalness = 0.05f;
        batch->occlusion = 1.0f;
        batch->depth_compare = BTBH_GPU_SCENE_DEPTH_LESS;
        batch->cull_mode = BTBH_GPU_SCENE_CULL_BACK;
        batch->stencil_pass = BTBH_GPU_SCENE_STENCIL_REPLACE;
        batch->spot_shadow_visible = 1u;
        if (index % 9u == 4u) {
            batch->emission_red = 255u;
            batch->emission_green = 210u;
            batch->emission_blue = 150u;
            batch->emission_energy = 2.5f;
        }
    }

    /* Two cameras with one angle and lens, 16 units apart; the sun's map round the first (the second's own round it,
       with BTBH_VIEWS_BENCH_OWN_SUN). */
    const float offset[3] = {14.0f, 0.0f, -8.0f};
    const float eye[3] = {-20.0f, 22.0f, 34.0f};
    const float centre[3] = {-6.0f, 0.0f, 4.0f};
    const float second_eye[3] = {eye[0] + offset[0], eye[1], eye[2] + offset[2]};
    const float second_centre[3] = {centre[0] + offset[0], centre[1], centre[2] + offset[2]};
    scene.cameras[0] = look_at(eye, centre, 0.9f, (float)width / (float)height, NULL);
    scene.cameras[1] = look_at(second_eye, second_centre, 0.9f, (float)width / (float)height, NULL);

    BTBHGPUSceneLighting *lighting = &scene.lighting;
    lighting->ambient_red = 1.0f;
    lighting->ambient_green = 1.0f;
    lighting->ambient_blue = 1.0f;
    lighting->ambient_energy = 0.3f;
    lighting->directional_energy = 2.4f;
    lighting->directional_red = 1.0f;
    lighting->directional_green = 0.95f;
    lighting->directional_blue = 0.88f;
    const float sun[3] = {30.0f, 60.0f, 20.0f};
    lighting->directional_x = -sun[0];
    lighting->directional_y = -sun[1];
    lighting->directional_z = -sun[2];
    lighting->spot_range = 8.0f;
    lighting->spot_falloff = 1.0f;
    lighting->spot_inner_cutoff = 1.0f;
    const float sun_centre[3] = {0.0f, 0.0f, 0.0f};
    orthographic_sun(sun, sun_centre, 20.0f, lighting->directional_shadow_view_projection);
    lighting->directional_shadow_enabled = 1u;
    lighting->directional_shadow_softness = 1.0f;
    lighting->directional_shadow_filter = BTBH_GPU_SCENE_SHADOW_FILTER_SOFT;
    lighting->secondary_directional_x = -sun[0];
    lighting->secondary_directional_y = -sun[1];
    lighting->secondary_directional_z = -sun[2];
    orthographic_sun(sun, sun_centre, 55.0f, lighting->secondary_directional_shadow_view_projection);
    lighting->secondary_directional_shadow_enabled = 1u;
    lighting->secondary_directional_shadow_softness = 1.0f;
    scene.second_lighting = scene.lighting;
    if (own_sun) {
        const float second_sun_centre[3] = {offset[0], 0.0f, offset[2]};
        orthographic_sun(sun, second_sun_centre, 20.0f, scene.second_lighting.directional_shadow_view_projection);
    }

    BTBHGPUScenePost *post = &scene.post;
    post->tonemap_white = 1.0f;
    post->exposure = 1.0f;
    post->contrast = 1.0f;
    post->saturation = 1.0f;
    post->blur_curve = 1.0f;
    post->tonemap_mode = BTBH_GPU_SCENE_TONEMAP_ACES;
    post->upscale_mode = BTBH_GPU_SCENE_UPSCALE_FSR1;
    post->ssao_enabled = 2u;
    post->ssao_radius = 1.0f;
    post->ssao_intensity = 1.0f;
    post->ssao_power = 1.5f;
    post->ssao_bias = 0.05f;
    post->ssao_contact = 1u;
    const char *ssr_text = SDL_getenv("BTBH_VIEWS_BENCH_SSR");
    const bool ssr_shown = ssr_text != NULL && atof(ssr_text) > 0.0;
    post->ssr_intensity = ssr_shown ? (float)atof(ssr_text) : 0.5f;
    if (ssr_shown) {
        /* (Without SSR_SURFACE_REFLECTIVITY the scene's targets mark every surface unlit: they reflect nothing.) */
        post->flags |= BTBH_GPU_SCENE_POST_SSR_SURFACE_REFLECTIVITY | BTBH_GPU_SCENE_POST_SSR_FACING_HITS;
    }
    post->ssr_max_roughness = 0.7f;
    post->ssr_max_distance = 30.0f;
    post->ssr_thickness = 0.6f;
    post->bloom_enabled = 1u;
    post->bloom_intensity = 0.4f;
    post->bloom_threshold = 1.0f;
    post->bloom_soft_knee = 0.5f;
    post->bloom_scatter = 0.7f;
    post->fog_enabled = 1u;
    post->fog_color[0] = 0.6f;
    post->fog_color[1] = 0.65f;
    post->fog_color[2] = 0.7f;
    post->fog_density = 0.01f;
    post->fog_start_distance = 20.0f;
    post->fog_maximum_opacity = 0.6f;
    post->vignette_enabled = 1u;
    post->vignette_intensity = 0.3f;
    post->vignette_smoothness = 0.5f;
    post->anti_aliasing = 1u; /* SMAA medium */
    if (fsr3) {
        /* FSR 3 at the frame's size (render scale 1) replaces the anti-aliasing (with one view; two run FSR 1). */
        post->upscale_mode = BTBH_GPU_SCENE_UPSCALE_FSR3;
        post->temporal_camera_near = 0.2f;
        post->temporal_camera_far = 200.0f;
        post->temporal_vertical_fov = 0.9f;
        post->temporal_frame_seconds = 1.0f / 60.0f;
        post->temporal_meters_per_unit = 1.0f;
        post->temporal_reactive_scale = 0.3f;
    }
    if (cas_sharpness > 0.0f) {
        post->cas_enabled = 1u;
        post->cas_sharpness = cas_sharpness;
    }

    /* The configurations' views, visibilities, rects and boxes. */
    float vertical_clip[3];
    double vertical_box[4];
    half_beyond(width, height, 0.0, vertical_clip, vertical_box);
    float diagonal_clip[3];
    double diagonal_box[4];
    half_beyond(width, height, angle_degrees, diagonal_clip, diagonal_box);
    for (uint32_t batch = 0u; batch <= BATCHES; ++batch) {
        const bool first = box_visible(scene.cameras[0].view_projection, NULL, scene.batch_minimum[batch],
            scene.batch_maximum[batch]);
        const bool second = box_visible(scene.cameras[1].view_projection, NULL, scene.batch_minimum[batch],
            scene.batch_maximum[batch]);
        scene.union_visibility[batch] = first || second ? 1u : 0u;
        scene.visibility[0][0][batch] = first ? 1u : 0u;
        scene.visibility[1][0][batch] = first ? 1u : 0u;
    }
    for (uint32_t config = 0u; config < CONFIGS; ++config) {
        BTBHGPUSceneView *views = scene.views[config];
        for (uint32_t index = 0u; index < 2u; ++index) {
            const BenchCamera *camera = &scene.cameras[index];
            memcpy(views[index].view_projection, camera->view_projection, sizeof(views[index].view_projection));
            memcpy(views[index].right, camera->right, sizeof(views[index].right));
            memcpy(views[index].up, camera->up, sizeof(views[index].up));
            memcpy(views[index].position, camera->position, sizeof(views[index].position));
        }
        scene.view_counts[config] = config == 2u || config == 3u ? 2u : 1u;
        views[0].visibility = scene.visibility[config][0];
        if (config == 2u || config == 3u) {
            /* The second camera on the half beyond the line, the first on the other: each culled by its frustum and
               its half-plane. */
            const float *clip = config == 2u ? vertical_clip : diagonal_clip;
            const float second_clip[3] = {clip[0], clip[1], clip[2]};
            const float first_clip[3] = {-clip[0], -clip[1], -clip[2]};
            memcpy(views[0].clip, first_clip, sizeof(first_clip));
            memcpy(views[1].clip, second_clip, sizeof(second_clip));
            views[1].lighting = own_sun ? &scene.second_lighting : NULL;
            for (uint32_t index = 0u; index < 2u; ++index) {
                float clip_plane[4];
                float uv_plane[4];
                btbh_gpu_scene_view_planes(views[index].clip, width, height, width, height, clip_plane, uv_plane);
                for (uint32_t batch = 0u; batch <= BATCHES; ++batch) {
                    scene.visibility[config][index][batch] = box_visible(scene.cameras[index].view_projection,
                        clip_plane, scene.batch_minimum[batch], scene.batch_maximum[batch]) ? 1u : 0u;
                }
            }
            views[1].visibility = scene.visibility[config][1];
        }
        if (config >= 4u) {
            const double *box = config == 4u ? vertical_box : diagonal_box;
            scene.target_views[config].view_id = 1u;
            const double grown[4] = {
                fmax(0.0, floor(box[0]) - (double)MARGIN),
                fmax(0.0, floor(box[1]) - (double)MARGIN),
                fmin((double)width, ceil(box[2]) + (double)MARGIN),
                fmin((double)height, ceil(box[3]) + (double)MARGIN),
            };
            scene.target_views[config].scissor_x = (uint32_t)grown[0];
            scene.target_views[config].scissor_y = (uint32_t)grown[1];
            scene.target_views[config].scissor_width = (uint32_t)(grown[2] - grown[0]);
            scene.target_views[config].scissor_height = (uint32_t)(grown[3] - grown[1]);
            scene.blit_box[config].x = (int)floor(box[0]);
            scene.blit_box[config].y = (int)floor(box[1]);
            scene.blit_box[config].w = (int)(ceil(box[2]) - floor(box[0]));
            scene.blit_box[config].h = (int)(ceil(box[3]) - floor(box[1]));
        }
    }
    if (!btbh_gpu_window_scene_set_shadow_update_periods(scene.window, period, period, period)) {
        fprintf(stderr, "bad period %u\n", period);
        return 1;
    }
    printf("BENCH frame %ux%u (rendered at its size), %u boxes of %u triangles in %u batches, diagonal %.0f deg, "
        "shadow period %u, second view's sun map %s, %u frames a sample, %u rounds\n", width, height, GRID * GRID,
        12u * detail * detail, BATCHES, angle_degrees, period, own_sun ? "its own" : "shared", frames_per_sample,
        rounds);
    printf("BENCH post %s, CAS %s%s\n", fsr3 ? "FSR 3 at scale 1" : "SMAA medium", cas_sharpness > 0.0f ? "on" : "off",
        cost_log_mode ? ", cost log (canvas presents)" : "");

    for (uint32_t config = 0u; config < CONFIGS; ++config) {
        for (uint32_t warm = 0u; warm < 6u && enabled[config]; ++warm) {
            if (!bench_frame(&scene, config, NULL)) {
                return 1;
            }
        }
    }
    if (SDL_getenv("BTBH_VIEWS_BENCH_BAND") != NULL) {
        /* The seam's band: the vertical split against its two cameras' one-view frames joined at the line, by distance
           from it, the widest distance where a pixel differs by more than 0, 2, 8 and 32 (8-bit steps), with the whole
           chain, without bloom, and without bloom and SSR. No visibility: every batch in every frame. */
        const BTBHGPUScenePost chain = scene.post;
        const char *const *p = paths();
        for (uint32_t variant = 0u; variant < 3u; ++variant) {
            scene.post = chain;
            if (variant >= 1u) {
                scene.post.bloom_enabled = 0u;
            }
            if (variant >= 2u) {
                scene.post.ssr_intensity = 0.0f;
            }
            uint8_t *frames[3] = {NULL, NULL, NULL};
            for (uint32_t which = 0u; which < 3u; ++which) {
                BTBHGPUSceneView views[2];
                memcpy(views, scene.views[2], sizeof(views));
                views[0].visibility = NULL;
                views[1].visibility = NULL;
                if (which < 2u) {
                    views[0] = views[which];
                    memset(views[0].clip, 0, sizeof(views[0].clip));
                    views[0].lighting = NULL;
                }
                BTBHGPUSceneReport report;
                if (btbh_gpu_window_scene_present_views(scene.window,
                        p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10],
                        BENCH_SHADER_FORMAT, width, height, views, which < 2u ? 1u : 2u,
                        which == 1u && own_sun ? &scene.second_lighting : &scene.lighting, &scene.post,
                        scene.instances, scene.instance_count, scene.instance_count, scene.batches, NULL, NULL,
                        BATCHES + 1u, 30u, 40u, 55u, 255u, NULL, &report) != BTBH_GPU_OK) {
                    fprintf(stderr, "band: %s\n", report.message);
                    return 1;
                }
                BTBHGPUTextureReport copied;
                if (btbh_gpu_window_copy_pending_scene_texture(scene.window, scene.frame, &copied) != BTBH_GPU_OK) {
                    return 1;
                }
                frames[which] = read_texture(scene.window, scene.frame);
                static const char *const which_names[3] = {"first", "second", "split"};
                dump_frame(scene.frame, frames[which], variant, which_names[which]);
            }
            const float *clip = scene.views[2][1].clip;
            uint32_t widest[4] = {0u, 0u, 0u, 0u};
            static const int thresholds[4] = {0, 2, 8, 32};
            int largest = 0;
            for (uint32_t y = 0u; y < height; ++y) {
                for (uint32_t x = 0u; x < width; ++x) {
                    const float side = clip[0] * ((float)x + 0.5f) + clip[1] * ((float)y + 0.5f) + clip[2];
                    const size_t texel = ((size_t)y * width + x) * 4u;
                    const uint8_t *expected = side >= 0.0f ? frames[1] + texel : frames[0] + texel;
                    const uint8_t *split = frames[2] + texel;
                    int difference = 0;
                    for (uint32_t channel = 0u; channel < 3u; ++channel) {
                        const int d = abs((int)split[channel] - (int)expected[channel]);
                        difference = d > difference ? d : difference;
                    }
                    largest = difference > largest ? difference : largest;
                    const uint32_t distance = (uint32_t)fabsf(side) + 1u;
                    for (uint32_t index = 0u; index < 4u; ++index) {
                        if (difference > thresholds[index] && distance > widest[index]) {
                            widest[index] = distance;
                        }
                    }
                }
            }
            static const char *const variant_names[3] = {"whole chain", "no bloom", "no bloom, no SSR"};
            printf("BENCH band %-17s: differs (>0) to %4u px, >2 to %4u px, >8 to %4u px, >32 to %4u px; largest %3d\n",
                variant_names[variant], widest[0], widest[1], widest[2], widest[3], largest);
            for (uint32_t which = 0u; which < 3u; ++which) {
                free(frames[which]);
            }
        }
        scene.post = chain;
    }
    /* The CPU a frame: the calls alone, the GPU idle before each. */
    double cpu_milliseconds[CONFIGS];
    for (uint32_t config = 0u; config < CONFIGS; ++config) {
        double values[12];
        cpu_milliseconds[config] = 0.0;
        if (!enabled[config]) {
            continue;
        }
        for (uint32_t index = 0u; index < 12u; ++index) {
            wait_idle(&scene);
            uint64_t nanoseconds = 0u;
            if (!bench_frame(&scene, config, &nanoseconds)) {
                return 1;
            }
            values[index] = (double)nanoseconds / 1.0e6;
        }
        qsort(values, 12u, sizeof(double), compare_doubles);
        cpu_milliseconds[config] = values[6];
    }
    double *samples = calloc((size_t)CONFIGS * rounds, sizeof(double));
    for (uint32_t round = 0u; round < rounds; ++round) {
        for (uint32_t step = 0u; step < CONFIGS; ++step) {
            const uint32_t config = (step + round) % CONFIGS;
            if (!enabled[config]) {
                continue;
            }
            if (cost_log_mode) {
                printf("BENCH sample %s %u\n", names[config], round);
            }
            wait_idle(&scene);
            const uint64_t started = SDL_GetTicksNS();
            for (uint32_t index = 0u; index < frames_per_sample; ++index) {
                if (!bench_frame(&scene, config, NULL)) {
                    return 1;
                }
            }
            wait_idle(&scene);
            samples[config * rounds + round] =
                (double)(SDL_GetTicksNS() - started) / 1.0e6 / (double)frames_per_sample;
        }
    }
    wait_idle(&scene);
    for (uint32_t config = 0u; config < CONFIGS && !cost_log_mode; ++config) {
        if (!enabled[config]) {
            continue;
        }
        const uint32_t kept = rounds - 3u;
        double *values = &samples[config * rounds + 3u];
        qsort(values, kept, sizeof(double), compare_doubles);
        printf("BENCH %-15s median %7.3f ms  p10 %7.3f  p90 %7.3f  min %7.3f  max %7.3f  cpu %6.3f ms  draws %u + %u\n",
            names[config], values[kept / 2u], values[kept / 10u], values[(kept * 9u) / 10u], values[0],
            values[kept - 1u], cpu_milliseconds[config], scene.draws[config][0], scene.draws[config][1]);
    }
    free(samples);
    free(scene.instances);
    btbh_gpu_texture_destroy(scene.frame);
    btbh_gpu_texture_destroy(scene.target);
    btbh_gpu_custom_mesh_destroy(cube);
    btbh_gpu_window_destroy(scene.window);
    btbh_gpu_device_shutdown(device);
    btbh_gpu_device_destroy(device);
    return 0;
}
