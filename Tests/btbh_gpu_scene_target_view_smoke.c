/* Target views (btbh_gpu_window_scene_render_to_target_view, since 0.15.0): a runtime kept between calls, and a rect
 * of the target shaded alone.
 *
 * - view_id 0 (and a NULL view) renders byte for byte as btbh_gpu_window_scene_render_to_target.
 * - A kept view with the shadow update periods at 1 renders byte for byte as an afresh render, call after call; at 3
 *   it draws its sun map on its first call and every third after (btbh_gpu_window_scene_shadow_render_count), and a
 *   sun moved in between shows the map it kept.
 * - A kept view keeps its radiance cascades' history (the second call on reprojects the first's and traces 2 of its 4
 *   quadrants), where view 0 traces all 4 every call.
 * - Two kept views don't disturb each other or the window's frames: four hidden windows present the same FSR 3 world
 *   frames (shadows every other frame), and between them render view 1 and view 2 (A), view 1 alone (B), view 2 alone
 *   (C) or nothing (D). Every world frame is the same in all four, view 1's pictures in A and B, view 2's in A and C.
 * - A rect: outside it the target holds the clear colour, byte for byte; inside it equals the whole target's render
 *   but for a band along its border, measured (printed) and held under BAND_LIMIT (BLOOM_BAND_LIMIT with bloom), with
 *   five post chains (none; SSAO, SSR and FXAA; HBAO with contact AO, fog, vignette, CAS and SMAA; the second with
 *   bloom; the radiance cascades with SSAO) at three render sizes (the target's, half of it with FSR 1, twice it
 *   halved), and mirrored. Within it, the seam (more than SEAM_DIFFERENCE off: the anti-aliasing, CAS and FSR 1 reading
 *   the cleared outside) stays within SEAM_LIMIT px (UPSCALED_SEAM_LIMIT rendering at half, BLOOM_SEAM_LIMIT
 *   with bloom). BTBH_SMOKE_DUMP=<dir> writes the images,
 *   BTBH_SMOKE_BANDS_ONLY=1 measures without the limits.
 * - btbh_gpu_window_scene_release_target_view frees a view's runtime; its next call makes it again (the lazy creation
 *   log records its pipelines and targets anew) and renders as its first call did. A view prewarmed with a rect makes
 *   nothing in its first render.
 * - Refusals (BTBH_GPU_INVALID_ARGUMENT, nothing made): a view_id over the capacity, a reserved field set, a rect
 *   outside the target, for the render, the prewarm and the release.
 *
 * It needs a GPU and the production shaders; with no device it is skipped (77). */
#undef NDEBUG

#include "btbh_gpu/btbh_gpu.h"
#include "btbh_gpu_smoke_meshes.h"
#include "btbh_gpu_internal.h"

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_hints.h>

#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The struct stays as bindgen and Swift read it: eight 32-bit words. */
_Static_assert(sizeof(BTBHGPUSceneTargetView) == 32u, "BTBHGPUSceneTargetView is 32 bytes");
_Static_assert(offsetof(BTBHGPUSceneTargetView, view_id) == 0u, "view_id first");
_Static_assert(offsetof(BTBHGPUSceneTargetView, scissor_x) == 4u, "scissor_x");
_Static_assert(offsetof(BTBHGPUSceneTargetView, scissor_y) == 8u, "scissor_y");
_Static_assert(offsetof(BTBHGPUSceneTargetView, scissor_width) == 12u, "scissor_width");
_Static_assert(offsetof(BTBHGPUSceneTargetView, scissor_height) == 16u, "scissor_height");
_Static_assert(offsetof(BTBHGPUSceneTargetView, reserved) == 20u, "reserved last");
_Static_assert(BTBH_GPU_SCENE_TARGET_VIEW_CAPACITY == 8u, "capacity");
#if !defined(BTBH_GPU_SCENE_HAS_TARGET_VIEWS)
#error "BTBH_GPU_SCENE_HAS_TARGET_VIEWS"
#endif

#define SMOKE_SKIP 77
#define WIDTH 320u
#define HEIGHT 180u
/* The rect of the scissor checks: inside the target on every side, at odd coordinates. */
#define RECT_X 97u
#define RECT_Y 37u
#define RECT_W 151u
#define RECT_H 101u
/* The widest band along the rect's border (in the target's pixels) where a render with the rect may differ from the
   whole target's: without bloom (measured on NVIDIA: 24 px at the target's size, 34 rendering at half with FSR 1) and
   with it (51). Within it, the seam: the band where a pixel differs by more than SEAM_DIFFERENCE 8-bit steps, the
   anti-aliasing, CAS and FSR 1 reading the cleared outside (measured: 3 px at the target's size or more, 19 at half
   with FSR 1, 44 with bloom). Past the seam the SSAO, HBAO and bloom reach further, by less. */
#define BAND_LIMIT 40u
#define BLOOM_BAND_LIMIT 64u
#define SEAM_DIFFERENCE 16
#define SEAM_LIMIT 4u
#define UPSCALED_SEAM_LIMIT 24u
#define BLOOM_SEAM_LIMIT 48u

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

typedef struct SmokeWindow {
    BTBHGPUWindow *window;
    BTBHGPUCustomMesh *cube;
    BTBHGPUTexture *frame;
    BTBHGPUTexture *target;
} SmokeWindow;

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

static SmokeCamera look_at(const float eye[3], const float centre[3], float fov_y, float aspect,
    const float projection_override[16]) {
    SmokeCamera camera;
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
        const float near_plane = 0.1f;
        const float far_plane = 50.0f;
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

/* A sun from `sun` (a point it shines from, towards the origin), its map covering the scene. */
static BTBHGPUSceneLighting lighting_from(const float sun[3]) {
    BTBHGPUSceneLighting lighting;
    memset(&lighting, 0, sizeof(lighting));
    lighting.ambient_red = 1.0f;
    lighting.ambient_green = 1.0f;
    lighting.ambient_blue = 1.0f;
    lighting.ambient_energy = 0.3f;
    lighting.directional_energy = 2.2f;
    lighting.directional_red = 1.0f;
    lighting.directional_green = 0.95f;
    lighting.directional_blue = 0.9f;
    lighting.spot_range = 8.0f;
    lighting.spot_falloff = 1.0f;
    lighting.spot_inner_cutoff = 1.0f;
    lighting.directional_x = -sun[0];
    lighting.directional_y = -sun[1];
    lighting.directional_z = -sun[2];
    const float centre[3] = {0.0f, 0.0f, 0.0f};
    const float rcp = 1.0f / 9.0f;
    const float range = 1.0f / (1.0f - 30.0f);
    const float orthographic[16] = {
        rcp, 0.0f, 0.0f, 0.0f,
        0.0f, rcp, 0.0f, 0.0f,
        0.0f, 0.0f, range, 0.0f,
        0.0f, 0.0f, range * 1.0f, 1.0f,
    };
    const SmokeCamera light = look_at(sun, centre, 1.0f, 1.0f, orthographic);
    memcpy(lighting.directional_shadow_view_projection, light.view_projection,
        sizeof(lighting.directional_shadow_view_projection));
    lighting.directional_shadow_enabled = 1u;
    lighting.directional_shadow_softness = 1.0f;
    return lighting;
}

/* The radiance cascades' chain (4): additive, tier Low, one glow between the cubes, no occlusion field. */
static const BTBHGPUSceneRadianceEmitter *cascades_emitter(void) {
    static BTBHGPUSceneRadianceEmitter emitter;
    memset(&emitter, 0, sizeof(emitter));
    emitter.stable_id = 1u;
    emitter.position_x = 0.4f;
    emitter.position_y = 0.6f;
    emitter.position_z = 0.3f;
    emitter.radius = 0.6f;
    emitter.height = 0.5f;
    emitter.red = 1.0f;
    emitter.green = 0.6f;
    emitter.blue = 0.3f;
    emitter.energy = 4.0f;
    emitter.direction_z = 1.0f;
    emitter.inner_cutoff = -1.0f;
    emitter.outer_cutoff = -2.0f;
    emitter.shape = BTBH_GPU_SCENE_RADIANCE_EMITTER_DISK;
    emitter.light_class = BTBH_GPU_SCENE_RADIANCE_EMITTER_CLASS_TRANSIENT;
    return &emitter;
}

/* Post chains: 0 none but ACES; 1 SSAO, SSR and FXAA; 2 HBAO with contact AO, fog, vignette, CAS and SMAA; 3 chain 1
   with bloom; 4 the radiance cascades (additive, Low) with SSAO. upscale_mode as asked (a render to a target runs FSR 3
   as FSR 1). */
static BTBHGPUScenePost post_chain(uint32_t chain, uint8_t upscale_mode) {
    BTBHGPUScenePost post;
    memset(&post, 0, sizeof(post));
    post.tonemap_white = 1.0f;
    post.exposure = 1.0f;
    post.contrast = 1.0f;
    post.saturation = 1.0f;
    post.blur_curve = 1.0f;
    post.tonemap_mode = BTBH_GPU_SCENE_TONEMAP_ACES;
    post.upscale_mode = upscale_mode;
    if (chain == 1u || chain == 3u) {
        post.ssao_enabled = 1u;
        post.ssr_intensity = 0.6f;
        post.ssr_max_roughness = 0.9f;
        post.ssr_max_distance = 12.0f;
        post.ssr_thickness = 0.5f;
        post.anti_aliasing = 3u; /* FXAA */
    }
    if (chain == 2u) {
        post.ssao_enabled = 2u;
        post.ssao_radius = 0.8f;
        post.ssao_intensity = 1.0f;
        post.ssao_power = 1.5f;
        post.ssao_bias = 0.05f;
        post.ssao_contact = 1u;
        post.fog_enabled = 1u;
        post.fog_color[0] = 0.5f;
        post.fog_color[1] = 0.55f;
        post.fog_color[2] = 0.6f;
        post.fog_density = 0.04f;
        post.fog_start_distance = 3.0f;
        post.fog_maximum_opacity = 0.7f;
        post.vignette_enabled = 1u;
        post.vignette_intensity = 0.4f;
        post.vignette_smoothness = 0.5f;
        post.cas_enabled = 1u;
        post.cas_sharpness = 0.5f;
        post.anti_aliasing = 2u; /* SMAA high */
    }
    if (chain == 4u) {
        post.ssao_enabled = 1u;
        BTBHGPUSceneRadianceCascades *cascades = &post.radiance_cascades;
        cascades->tier = BTBH_GPU_SCENE_RC_LOW;
        cascades->indirect_intensity = 1.0f;
        cascades->hero_omni_emitter_index = -1;
        cascades->additive = 1u;
        cascades->emitter_count = 1u;
        cascades->emitters = cascades_emitter();
    }
    if (chain == 3u) {
        post.bloom_enabled = 1u;
        post.bloom_intensity = 0.6f;
        post.bloom_threshold = 0.8f;
        post.bloom_soft_knee = 0.5f;
        post.bloom_scatter = 0.7f;
    }
    return post;
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

static BTBHGPUSceneGeometryBatch cube_batch(BTBHGPUCustomMesh *cube, uint32_t first, uint32_t count) {
    BTBHGPUSceneGeometryBatch batch;
    memset(&batch, 0, sizeof(batch));
    batch.kind = BTBH_GPU_SCENE_GEOMETRY_CUSTOM_MESH;
    batch.custom_mesh = cube;
    batch.material_slot = -1;
    batch.first_instance = first;
    batch.instance_count = count;
    batch.albedo_red = 255u;
    batch.albedo_green = 255u;
    batch.albedo_blue = 255u;
    batch.albedo_alpha = 255u;
    batch.uv_scale_x = 1.0f;
    batch.uv_scale_y = 1.0f;
    batch.roughness = 0.35f;
    batch.metalness = 0.1f;
    batch.occlusion = 1.0f;
    batch.depth_compare = BTBH_GPU_SCENE_DEPTH_LESS;
    batch.cull_mode = BTBH_GPU_SCENE_CULL_NONE;
    batch.stencil_pass = BTBH_GPU_SCENE_STENCIL_REPLACE;
    batch.spot_shadow_visible = 1u;
    return batch;
}

/* A unit cube (-0.5..0.5) wound so the renderer lights its outsides; 24 floats a vertex. */
static BTBHGPUCustomMesh *create_cube(BTBHGPUWindow *window) {
    static const float faces[6][3] = {
        {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
    };
    float vertices[24][24];
    uint32_t indices[36];
    memset(vertices, 0, sizeof(vertices));
    for (uint32_t face = 0u; face < 6u; ++face) {
        const float *n = faces[face];
        float u[3] = {n[1], n[2], n[0]};
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
        const uint32_t reversed[6] = {base, base + 2u, base + 1u, base, base + 3u, base + 2u};
        memcpy(&indices[face * 6u], reversed, sizeof(reversed));
    }
    BTBHGPUCustomMeshReport report;
    memset(&report, 0, sizeof(report));
    BTBHGPUCustomMesh *mesh = smoke_custom_mesh_create(window, vertices, (uint32_t)sizeof(vertices), 24u, indices,
        36u, &report);
    if (mesh == NULL) {
        fprintf(stderr, "cube: %s\n", report.message);
    }
    assert(mesh != NULL);
    return mesh;
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
    /* RGBA for the PNGs and the checks. */
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

static void dump(const char *name, const uint8_t *pixels) {
    const char *directory = SDL_getenv("BTBH_SMOKE_DUMP");
    if (directory == NULL) {
        return;
    }
    char path[1024];
    (void)snprintf(path, sizeof(path), "%s/%s.png", directory, name);
    (void)btbh_gpu_write_png_rgba8(path, WIDTH, HEIGHT, pixels);
}

/* The scene the views render: a slab, cubes round it (two glowing, for bloom), the sun from `sun_index`'s side. */
typedef struct ViewShot {
    float angle;
    uint32_t sun_index;
    uint32_t chain;
    uint32_t render_width;
    uint32_t render_height;
    uint8_t upscale_mode;
    /* BTBHGPUScenePost.mirror_horizontal. */
    uint8_t mirror;
} ViewShot;

static const uint8_t clear_colour[4] = {23u, 41u, 67u, 255u};

/* Renders `shot` into the window's target: through btbh_gpu_window_scene_render_to_target when plain, else through
   the view entry with `view` (NULL allowed). */
static BTBHGPUResult render_shot(SmokeWindow *smoke, const ViewShot *shot, bool plain,
    const BTBHGPUSceneTargetView *view, BTBHGPUSceneReport *report) {
    const float eye[3] = {7.5f * sinf(shot->angle), 4.2f, 7.5f * cosf(shot->angle)};
    const float centre[3] = {0.0f, 0.4f, 0.0f};
    const SmokeCamera camera = look_at(eye, centre, 0.85f, (float)WIDTH / (float)HEIGHT, NULL);
    static const float suns[3][3] = {{4.0f, 10.0f, 3.0f}, {-9.0f, 5.0f, 2.0f}, {2.0f, 6.0f, -9.0f}};
    const BTBHGPUSceneLighting lighting = lighting_from(suns[shot->sun_index % 3u]);
    BTBHGPUScenePost post = post_chain(shot->chain, shot->upscale_mode);
    post.mirror_horizontal = shot->mirror;
    const float slab[4] = {0.55f, 0.58f, 0.5f, 1.0f};
    const float colours[6][4] = {
        {0.9f, 0.25f, 0.2f, 1.0f},
        {0.2f, 0.6f, 0.9f, 1.0f},
        {0.95f, 0.85f, 0.3f, 1.0f},
        {0.3f, 0.8f, 0.35f, 1.0f},
        {1.0f, 1.0f, 1.0f, 1.0f},
        {0.8f, 0.4f, 0.9f, 1.0f},
    };
    float instances[9][24];
    instance(instances[0], 0.0f, -0.55f, 0.0f, 12.0f, 0.1f, 12.0f, slab);
    for (uint32_t index = 0u; index < 6u; ++index) {
        const float a = 1.05f * (float)index;
        const float r = 1.2f + 0.45f * (float)index;
        instance(instances[1u + index], r * sinf(a), 0.0f + 0.15f * (float)(index % 3u), r * cosf(a),
            0.9f, 1.0f + 0.3f * (float)(index % 2u), 0.9f, colours[index]);
    }
    /* Two glowing cubes (drawn by their own batch, emissive). */
    instance(instances[7], -1.8f, 1.6f, 0.6f, 0.5f, 0.5f, 0.5f, colours[4]);
    instance(instances[8], 1.4f, 1.9f, -1.2f, 0.45f, 0.45f, 0.45f, colours[2]);
    BTBHGPUSceneGeometryBatch batches[2];
    batches[0] = cube_batch(smoke->cube, 0u, 7u);
    batches[1] = cube_batch(smoke->cube, 7u, 2u);
    batches[1].emission_red = 255u;
    batches[1].emission_green = 230u;
    batches[1].emission_blue = 180u;
    batches[1].emission_energy = 6.0f;
    if (plain) {
        return btbh_gpu_window_scene_render_to_target(
            smoke->window, smoke->target,
            shader_paths[0], shader_paths[1], shader_paths[2], shader_paths[3],
            shader_paths[4], shader_paths[5], shader_paths[6], shader_paths[7],
            shader_paths[8], shader_paths[9], shader_paths[10],
            SMOKE_SHADER_FORMAT, shot->render_width, shot->render_height,
            camera.view_projection, camera.right, camera.up, camera.position,
            &lighting, &post, &instances[0][0], 9u, 9u, batches, NULL, NULL, 2u,
            clear_colour[0], clear_colour[1], clear_colour[2], clear_colour[3], report);
    }
    return btbh_gpu_window_scene_render_to_target_view(
        smoke->window, smoke->target,
        shader_paths[0], shader_paths[1], shader_paths[2], shader_paths[3],
        shader_paths[4], shader_paths[5], shader_paths[6], shader_paths[7],
        shader_paths[8], shader_paths[9], shader_paths[10],
        SMOKE_SHADER_FORMAT, shot->render_width, shot->render_height,
        camera.view_projection, camera.right, camera.up, camera.position,
        &lighting, &post, &instances[0][0], 9u, 9u, batches, NULL, NULL, 2u,
        clear_colour[0], clear_colour[1], clear_colour[2], clear_colour[3], view, report);
}

/* Renders and reads back; the render must succeed. */
static uint8_t *shot_pixels(SmokeWindow *smoke, const ViewShot *shot, bool plain, const BTBHGPUSceneTargetView *view) {
    BTBHGPUSceneReport report;
    const BTBHGPUResult result = render_shot(smoke, shot, plain, view, &report);
    if (result != BTBH_GPU_OK) {
        fprintf(stderr, "render: %s\n", report.message);
    }
    assert(result == BTBH_GPU_OK);
    assert(report.drawable_width == WIDTH && report.drawable_height == HEIGHT);
    return read_texture(smoke->window, smoke->target);
}

static BTBHGPUSceneTargetView make_view(uint32_t view_id, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    BTBHGPUSceneTargetView view;
    memset(&view, 0, sizeof(view));
    view.view_id = view_id;
    view.scissor_x = x;
    view.scissor_y = y;
    view.scissor_width = w;
    view.scissor_height = h;
    return view;
}

/* A world frame with FSR 3 (shadows as the window's periods say), finished by a copy into the window's frame. */
static void world_frame(SmokeWindow *smoke, uint32_t index) {
    const float angle = 0.4f + 0.03f * (float)index;
    const float eye[3] = {6.0f * sinf(angle), 3.0f, 6.0f * cosf(angle)};
    const float centre[3] = {0.0f, 0.3f, 0.0f};
    const SmokeCamera camera = look_at(eye, centre, 0.8f, (float)WIDTH / (float)HEIGHT, NULL);
    const float sun[3] = {4.0f, 10.0f, 3.0f};
    const BTBHGPUSceneLighting lighting = lighting_from(sun);
    BTBHGPUScenePost post = post_chain(1u, BTBH_GPU_SCENE_UPSCALE_FSR3);
    post.anti_aliasing = 0u;
    post.temporal_camera_near = 0.1f;
    post.temporal_camera_far = 50.0f;
    post.temporal_vertical_fov = 0.8f;
    post.temporal_frame_seconds = 1.0f / 60.0f;
    post.temporal_meters_per_unit = 1.0f;
    post.temporal_reactive_scale = 0.3f;
    post.temporal_reset = index == 0u ? 1u : 0u;
    const float grey[4] = {0.55f, 0.6f, 0.5f, 1.0f};
    const float red[4] = {0.9f, 0.25f, 0.2f, 1.0f};
    float instances[2][24];
    instance(instances[0], 0.0f, -0.55f, 0.0f, 8.0f, 0.1f, 8.0f, grey);
    instance(instances[1], 0.0f, 0.5f, 0.0f, 1.0f, 1.0f, 1.0f, red);
    const BTBHGPUSceneGeometryBatch batch = cube_batch(smoke->cube, 0u, 2u);
    BTBHGPUSceneReport report;
    const BTBHGPUResult result = btbh_gpu_window_scene_present_instances(
        smoke->window,
        shader_paths[0], shader_paths[1], shader_paths[2], shader_paths[3],
        shader_paths[4], shader_paths[5], shader_paths[6], shader_paths[7],
        shader_paths[8], shader_paths[9], shader_paths[10],
        SMOKE_SHADER_FORMAT, WIDTH / 2u, HEIGHT / 2u,
        camera.view_projection, camera.right, camera.up, camera.position,
        &lighting, &post, &instances[0][0], 2u, 2u, &batch, 1u,
        40u, 60u, 90u, 255u, &report);
    if (result != BTBH_GPU_OK) {
        fprintf(stderr, "world frame %u: %s\n", index, report.message);
    }
    assert(result == BTBH_GPU_OK);
    BTBHGPUTextureReport copied;
    assert(btbh_gpu_window_copy_pending_scene_texture(smoke->window, smoke->frame, &copied) == BTBH_GPU_OK);
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
    smoke.cube = create_cube(smoke.window);
    BTBHGPUTextureReport texture_report;
    smoke.frame = btbh_gpu_window_render_target_create(
        smoke.window, WIDTH, HEIGHT, BTBH_GPU_TEXTURE_FILTER_LINEAR, &texture_report);
    assert(smoke.frame != NULL);
    smoke.target = btbh_gpu_window_render_target_create(
        smoke.window, WIDTH, HEIGHT, BTBH_GPU_TEXTURE_FILTER_LINEAR, &texture_report);
    assert(smoke.target != NULL);
    return smoke;
}

static void close_window(SmokeWindow *smoke) {
    btbh_gpu_texture_destroy(smoke->frame);
    btbh_gpu_texture_destroy(smoke->target);
    btbh_gpu_custom_mesh_destroy(smoke->cube);
    btbh_gpu_window_destroy(smoke->window);
}

static bool same(const uint8_t *a, const uint8_t *b) {
    return memcmp(a, b, (size_t)WIDTH * HEIGHT * 4u) == 0;
}

/* ---- view_id 0 and NULL: render_to_target to the byte ---- */
static void check_view_zero(SmokeWindow *smoke) {
    const ViewShot shot = {0.5f, 0u, 1u, WIDTH, HEIGHT, BTBH_GPU_SCENE_UPSCALE_FSR3};
    uint8_t *plain = shot_pixels(smoke, &shot, true, NULL);
    uint8_t *null_view = shot_pixels(smoke, &shot, false, NULL);
    const BTBHGPUSceneTargetView zero = make_view(0u, 0u, 0u, 0u, 0u);
    uint8_t *zero_view = shot_pixels(smoke, &shot, false, &zero);
    /* A rect over the whole target is no rect. */
    const BTBHGPUSceneTargetView whole = make_view(0u, 0u, 0u, WIDTH, HEIGHT);
    uint8_t *whole_view = shot_pixels(smoke, &shot, false, &whole);
    /* Width 0: the whole target, whatever x and y say. */
    const BTBHGPUSceneTargetView no_width = make_view(0u, 7u, 9u, 0u, 20u);
    uint8_t *no_width_view = shot_pixels(smoke, &shot, false, &no_width);
    assert(same(plain, null_view));
    assert(same(plain, zero_view));
    assert(same(plain, whole_view));
    assert(same(plain, no_width_view));
    /* And the next plain render is still the same: view 0 starts afresh every call. */
    uint8_t *again = shot_pixels(smoke, &shot, true, NULL);
    assert(same(plain, again));
    dump("view-zero", plain);
    free(plain);
    free(null_view);
    free(zero_view);
    free(whole_view);
    free(no_width_view);
    free(again);
    printf("view_id 0 and a NULL view: byte for byte render_to_target\n");
}

/* ---- a kept view: period 1 equals afresh; period 3 keeps its map ---- */
static void check_kept_shadows(SmokeWindow *smoke) {
    assert(btbh_gpu_window_scene_set_shadow_update_periods(smoke->window, 1u, 1u, 1u) == 1u);
    const BTBHGPUSceneTargetView kept = make_view(3u, 0u, 0u, 0u, 0u);
    assert(btbh_gpu_window_scene_shadow_render_count(smoke->window, 3u) == -1);
    for (uint32_t call = 0u; call < 4u; ++call) {
        const ViewShot shot = {0.2f + 0.15f * (float)call, call, 1u, WIDTH, HEIGHT, BTBH_GPU_SCENE_UPSCALE_LINEAR};
        uint8_t *kept_pixels = shot_pixels(smoke, &shot, false, &kept);
        uint8_t *afresh = shot_pixels(smoke, &shot, true, NULL);
        assert(same(kept_pixels, afresh));
        assert(btbh_gpu_window_scene_shadow_render_count(smoke->window, 3u) == (int64_t)call + 1);
        free(kept_pixels);
        free(afresh);
    }
    printf("kept view, shadow periods 1: byte for byte an afresh render, 4 calls, a sun map each\n");

    assert(btbh_gpu_window_scene_set_shadow_update_periods(smoke->window, 3u, 3u, 3u) == 1u);
    const BTBHGPUSceneTargetView slow = make_view(4u, 0u, 0u, 0u, 0u);
    const int64_t aside_before = btbh_gpu_window_scene_shadow_render_count(smoke->window, 0u);
    const int64_t window_before = btbh_gpu_window_scene_shadow_render_count(smoke->window, BTBH_GPU_SCENE_WINDOW_RUNTIME);
    int64_t counts[6];
    uint8_t *second = NULL;
    for (uint32_t call = 0u; call < 6u; ++call) {
        /* Call 1 (the second) moves the sun: the map kept from call 0 still lies where it fell. */
        const ViewShot shot = {0.6f, call == 0u ? 0u : 1u, 1u, WIDTH, HEIGHT, BTBH_GPU_SCENE_UPSCALE_LINEAR};
        uint8_t *pixels = shot_pixels(smoke, &shot, false, &slow);
        counts[call] = btbh_gpu_window_scene_shadow_render_count(smoke->window, 4u);
        if (call == 1u) {
            second = pixels;
        } else {
            free(pixels);
        }
    }
    printf("kept view, shadow periods 3: sun maps drawn after each call: %lld %lld %lld %lld %lld %lld\n",
        (long long)counts[0], (long long)counts[1], (long long)counts[2], (long long)counts[3],
        (long long)counts[4], (long long)counts[5]);
    assert(counts[0] == 1 && counts[1] == 1 && counts[2] == 1 && counts[3] == 2 && counts[4] == 2 && counts[5] == 2);
    /* Afresh with the moved sun: its own map, so another picture. */
    const ViewShot moved = {0.6f, 1u, 1u, WIDTH, HEIGHT, BTBH_GPU_SCENE_UPSCALE_LINEAR};
    uint8_t *afresh = shot_pixels(smoke, &moved, true, NULL);
    assert(!same(second, afresh));
    dump("kept-sun-moved", second);
    dump("afresh-sun-moved", afresh);
    free(second);
    free(afresh);
    /* The afresh runtime drew a map every call; the window's own runtime none. */
    assert(btbh_gpu_window_scene_shadow_render_count(smoke->window, 0u) == aside_before + 1);
    assert(btbh_gpu_window_scene_shadow_render_count(smoke->window, BTBH_GPU_SCENE_WINDOW_RUNTIME) == window_before);
    printf("kept view, shadow periods 3: its kept map shows when the sun moves (an afresh render differs)\n");
}

/* ---- a kept view keeps its cascades' history; view 0 starts afresh ---- */
static void check_kept_cascades(SmokeWindow *smoke) {
    const ViewShot shot = {0.7f, 0u, 4u, WIDTH, HEIGHT, BTBH_GPU_SCENE_UPSCALE_LINEAR};
    const BTBHGPUSceneTargetView kept = make_view(2u, 0u, 0u, 0u, 0u);
    BTBHGPUSceneReport report;
    uint32_t afresh_valid[3];
    uint32_t afresh_fresh[3];
    uint32_t kept_valid[3];
    uint32_t kept_fresh[3];
    for (uint32_t call = 0u; call < 3u; ++call) {
        assert(render_shot(smoke, &shot, true, NULL, &report) == BTBH_GPU_OK);
        if (report.radiance_cascades.active_tier == BTBH_GPU_SCENE_RC_OFF) {
            printf("radiance cascades unavailable here (fallback %d): their history isn't checked\n",
                (int)report.radiance_cascades.fallback_reason);
            return;
        }
        afresh_valid[call] = report.radiance_cascades.pass_telemetry.temporal_history_valid;
        afresh_fresh[call] = report.radiance_cascades.pass_telemetry.fresh_quadrant_count;
        assert(render_shot(smoke, &shot, false, &kept, &report) == BTBH_GPU_OK);
        kept_valid[call] = report.radiance_cascades.pass_telemetry.temporal_history_valid;
        kept_fresh[call] = report.radiance_cascades.pass_telemetry.fresh_quadrant_count;
    }
    printf("radiance cascades, 3 calls: afresh history %u %u %u (fresh quadrants %u %u %u), kept view history %u %u %u"
        " (fresh quadrants %u %u %u)\n", afresh_valid[0], afresh_valid[1], afresh_valid[2], afresh_fresh[0],
        afresh_fresh[1], afresh_fresh[2], kept_valid[0], kept_valid[1], kept_valid[2], kept_fresh[0], kept_fresh[1],
        kept_fresh[2]);
    for (uint32_t call = 0u; call < 3u; ++call) {
        assert(afresh_valid[call] == 0u && afresh_fresh[call] == 4u);
    }
    assert(kept_valid[0] == 0u && kept_fresh[0] == 4u);
    assert(kept_valid[1] == 1u && kept_fresh[1] == 2u);
    assert(kept_valid[2] == 1u && kept_fresh[2] == 2u);
    /* Released, it starts afresh again. */
    assert(btbh_gpu_window_scene_release_target_view(smoke->window, 2u) == BTBH_GPU_OK);
    assert(render_shot(smoke, &shot, false, &kept, &report) == BTBH_GPU_OK);
    assert(report.radiance_cascades.pass_telemetry.temporal_history_valid == 0u);
}

/* ---- two kept views and the window's frames ---- */
static void check_views_apart(BTBHGPUDevice *device) {
    SmokeWindow windows[4];
    static const char *const titles[4] = {
        "BTBH target view smoke A", "BTBH target view smoke B", "BTBH target view smoke C", "BTBH target view smoke D",
    };
    /* [window]: renders view 1, renders view 2. */
    static const bool renders[4][2] = {{true, true}, {true, false}, {false, true}, {false, false}};
    for (uint32_t index = 0u; index < 4u; ++index) {
        windows[index] = open_window(device, titles[index]);
        assert(btbh_gpu_window_scene_set_shadow_update_periods(windows[index].window, 2u, 2u, 2u) == 1u);
    }
    const BTBHGPUSceneTargetView one = make_view(1u, 0u, 0u, 0u, 0u);
    /* View 2 with a rect: the scissored path beside the plain one. */
    const BTBHGPUSceneTargetView two = make_view(2u, RECT_X, RECT_Y, RECT_W, RECT_H);
    for (uint32_t frame = 0u; frame < 5u; ++frame) {
        uint8_t *view_pixels[4][2];
        memset(view_pixels, 0, sizeof(view_pixels));
        for (uint32_t index = 0u; index < 4u; ++index) {
            world_frame(&windows[index], frame);
            if (renders[index][0]) {
                const ViewShot shot = {0.3f + 0.05f * (float)frame, 0u, 1u, WIDTH, HEIGHT, BTBH_GPU_SCENE_UPSCALE_FSR1};
                view_pixels[index][0] = shot_pixels(&windows[index], &shot, false, &one);
            }
            if (renders[index][1]) {
                const ViewShot shot = {2.4f - 0.07f * (float)frame, 2u, 2u, WIDTH, HEIGHT, BTBH_GPU_SCENE_UPSCALE_FSR1};
                view_pixels[index][1] = shot_pixels(&windows[index], &shot, false, &two);
            }
        }
        uint8_t *world[4];
        for (uint32_t index = 0u; index < 4u; ++index) {
            world[index] = read_texture(windows[index].window, windows[index].frame);
        }
        for (uint32_t index = 1u; index < 4u; ++index) {
            assert(same(world[0], world[index]));
        }
        assert(same(view_pixels[0][0], view_pixels[1][0]));
        assert(same(view_pixels[0][1], view_pixels[2][1]));
        assert(!same(view_pixels[0][0], view_pixels[0][1]));
        if (frame == 4u) {
            dump("apart-world", world[0]);
            dump("apart-view-1", view_pixels[0][0]);
            dump("apart-view-2", view_pixels[0][1]);
        }
        for (uint32_t index = 0u; index < 4u; ++index) {
            free(world[index]);
            free(view_pixels[index][0]);
            free(view_pixels[index][1]);
        }
    }
    /* Each kept view drew its sun map on frames 0, 2 and 4 (periods 2). */
    assert(btbh_gpu_window_scene_shadow_render_count(windows[0].window, 1u) == 3);
    assert(btbh_gpu_window_scene_shadow_render_count(windows[0].window, 2u) == 3);
    assert(btbh_gpu_window_scene_shadow_render_count(windows[3].window, 1u) == -1);
    for (uint32_t index = 0u; index < 4u; ++index) {
        close_window(&windows[index]);
    }
    printf("two kept views beside FSR 3 world frames: every world frame, view 1 and view 2 the same with or without"
        " the other\n");
}

/* ---- the rect ---- */

typedef struct BandResult {
    uint32_t band;              /* the widest distance from the border (plus one) where a pixel differs */
    uint32_t seam;              /* ... where one differs by more than SEAM_DIFFERENCE */
    int largest;                /* the largest channel difference inside */
    uint32_t outside_wrong;     /* pixels outside the rect that aren't the clear colour */
} BandResult;

static BandResult measure(const uint8_t *full, const uint8_t *part, const uint8_t clear_bytes[4]) {
    BandResult result;
    memset(&result, 0, sizeof(result));
    for (uint32_t y = 0u; y < HEIGHT; ++y) {
        for (uint32_t x = 0u; x < WIDTH; ++x) {
            const uint8_t *a = &full[(y * WIDTH + x) * 4u];
            const uint8_t *b = &part[(y * WIDTH + x) * 4u];
            const bool inside = x >= RECT_X && x < RECT_X + RECT_W && y >= RECT_Y && y < RECT_Y + RECT_H;
            if (!inside) {
                if (memcmp(b, clear_bytes, 4u) != 0) {
                    result.outside_wrong += 1u;
                }
                continue;
            }
            int difference = 0;
            for (uint32_t channel = 0u; channel < 4u; ++channel) {
                const int d = abs((int)a[channel] - (int)b[channel]);
                difference = d > difference ? d : difference;
            }
            if (difference == 0) {
                continue;
            }
            uint32_t distance = x - RECT_X;
            distance = SDL_min(distance, RECT_X + RECT_W - 1u - x);
            distance = SDL_min(distance, y - RECT_Y);
            distance = SDL_min(distance, RECT_Y + RECT_H - 1u - y);
            result.band = SDL_max(result.band, distance + 1u);
            if (difference > SEAM_DIFFERENCE) {
                result.seam = SDL_max(result.seam, distance + 1u);
            }
            result.largest = difference > result.largest ? difference : result.largest;
        }
    }
    return result;
}

static void check_rects(SmokeWindow *smoke) {
    assert(btbh_gpu_window_scene_set_shadow_update_periods(smoke->window, 1u, 1u, 1u) == 1u);
    /* The clear colour as the target stores a clear of it: an 8-bit UNORM target (the window's swapchain format)
       holds these bytes (read back as RGBA). */
    assert(smoke->target->format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM ||
        smoke->target->format == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
    const uint8_t *clear_bytes = clear_colour;
    static const char *const chain_names[5] = {"none", "SSAO+SSR+FXAA", "HBAO contact+fog+vignette+CAS+SMAA",
        "SSAO+SSR+FXAA+bloom", "radiance cascades+SSAO"};
    static const struct {
        uint32_t width;
        uint32_t height;
        uint8_t upscale;
        const char *name;
    } sizes[3] = {
        {WIDTH, HEIGHT, BTBH_GPU_SCENE_UPSCALE_FSR1, "at the target's size"},
        {WIDTH / 2u, HEIGHT / 2u, BTBH_GPU_SCENE_UPSCALE_FSR1, "at half, FSR 1"},
        {WIDTH * 2u, HEIGHT * 2u, BTBH_GPU_SCENE_UPSCALE_FSR1, "at twice, halved"},
    };
    uint32_t widest = 0u;
    uint32_t widest_bloom = 0u;
    uint32_t widest_seam = 0u;
    for (uint32_t chain = 0u; chain < 5u; ++chain) {
        for (uint32_t size = 0u; size < 3u; ++size) {
            const ViewShot shot = {0.9f, 0u, chain, sizes[size].width, sizes[size].height, sizes[size].upscale};
            /* Both afresh (view 0), and again with a kept view's rect. */
            const BTBHGPUSceneTargetView whole = make_view(0u, 0u, 0u, 0u, 0u);
            const BTBHGPUSceneTargetView rect = make_view(0u, RECT_X, RECT_Y, RECT_W, RECT_H);
            const BTBHGPUSceneTargetView kept_rect = make_view(5u, RECT_X, RECT_Y, RECT_W, RECT_H);
            uint8_t *full = shot_pixels(smoke, &shot, false, &whole);
            uint8_t *part = shot_pixels(smoke, &shot, false, &rect);
            uint8_t *kept = shot_pixels(smoke, &shot, false, &kept_rect);
            const BandResult band = measure(full, part, clear_bytes);
            printf("rect, %-34s %-20s: outside %u wrong; band %2u px, seam %2u px, largest difference %3d\n",
                chain_names[chain], sizes[size].name, band.outside_wrong, band.band, band.seam, band.largest);
            assert(band.outside_wrong == 0u);
            /* A kept view's rect (shadow periods 1) renders as an afresh one (but for the cascades, whose history it
               keeps). */
            assert(chain == 4u || same(part, kept));
            /* BTBH_SMOKE_BANDS_ONLY=1 measures without the band's limits. */
            const bool limits = SDL_getenv("BTBH_SMOKE_BANDS_ONLY") == NULL;
            if (chain == 3u) {
                widest_bloom = SDL_max(widest_bloom, band.band);
                assert(!limits || (band.band <= BLOOM_BAND_LIMIT && band.seam <= BLOOM_SEAM_LIMIT));
            } else {
                widest = SDL_max(widest, band.band);
                widest_seam = SDL_max(widest_seam, band.seam);
                assert(!limits || band.band <= BAND_LIMIT);
                assert(!limits || band.seam <= (size == 1u ? UPSCALED_SEAM_LIMIT : SEAM_LIMIT));
            }
            if (size == 0u) {
                char name[64];
                (void)snprintf(name, sizeof(name), "rect-chain%u-full", chain);
                dump(name, full);
                (void)snprintf(name, sizeof(name), "rect-chain%u-part", chain);
                dump(name, part);
            }
            free(full);
            free(part);
            free(kept);
        }
    }
    /* Mirrored (Mirror Curse): the rect is the target's, after the mirror. */
    for (uint32_t size = 0u; size < 2u; ++size) {
        const ViewShot shot = {0.9f, 0u, 1u, sizes[size].width, sizes[size].height, sizes[size].upscale, 1u};
        const BTBHGPUSceneTargetView whole = make_view(0u, 0u, 0u, 0u, 0u);
        const BTBHGPUSceneTargetView rect = make_view(0u, RECT_X, RECT_Y, RECT_W, RECT_H);
        uint8_t *full = shot_pixels(smoke, &shot, false, &whole);
        uint8_t *part = shot_pixels(smoke, &shot, false, &rect);
        const BandResult band = measure(full, part, clear_bytes);
        printf("rect, %-34s %-20s: outside %u wrong; band %2u px, seam %2u px, largest difference %3d\n",
            "mirrored, SSAO+SSR+FXAA", sizes[size].name, band.outside_wrong, band.band, band.seam, band.largest);
        assert(band.outside_wrong == 0u);
        assert(SDL_getenv("BTBH_SMOKE_BANDS_ONLY") != NULL ||
            (band.band <= BAND_LIMIT && band.seam <= (size == 1u ? UPSCALED_SEAM_LIMIT : SEAM_LIMIT)));
        widest = SDL_max(widest, band.band);
        widest_seam = SDL_max(widest_seam, band.seam);
        if (size == 0u) {
            dump("rect-mirrored-part", part);
        }
        free(full);
        free(part);
    }
    printf("rect: outside it the clear colour; inside, the whole render's pixels past %u px from its border (bloom:"
        " %u px), within %d steps of them past %u px (without bloom)\n", widest, widest_bloom, SEAM_DIFFERENCE,
        widest_seam);
}

/* ---- release, prewarm, the lazy creation log ---- */

/* What the window's scenes made since the last call (the lazy creation log): everything but the slow calls, or with
   prewarmable_only what a prewarm makes (shaders, pipelines, samplers, textures; not the buffers that grow with a
   frame's instances). */
static uint32_t made_since(BTBHGPUWindow *window, bool prewarmable_only) {
    BTBHGPULazyCreationEntry entries[BTBH_GPU_LAZY_CREATION_WINDOW_CAPACITY];
    uint32_t made = 0u;
    uint32_t count = 0u;
    do {
        assert(btbh_gpu_window_lazy_creation_report(window, entries, BTBH_GPU_LAZY_CREATION_WINDOW_CAPACITY, &count) ==
            BTBH_GPU_OK);
        for (uint32_t index = 0u; index < count; ++index) {
            const uint8_t kind = entries[index].kind;
            if (kind != BTBH_GPU_LAZY_CREATION_SLOW_CALL &&
                (!prewarmable_only ||
                 (kind != BTBH_GPU_LAZY_CREATION_BUFFER && kind != BTBH_GPU_LAZY_CREATION_TRANSFER_BUFFER))) {
                made += 1u;
                if (prewarmable_only) {
                    printf("  made: kind %u, %s\n", (unsigned)kind, entries[index].name);
                }
            }
        }
    } while (count == BTBH_GPU_LAZY_CREATION_WINDOW_CAPACITY);
    return made;
}

static void check_release_and_prewarm(SmokeWindow *smoke) {
    assert(btbh_gpu_window_scene_set_shadow_update_periods(smoke->window, 2u, 2u, 2u) == 1u);
    btbh_gpu_window_set_lazy_creation_log_enabled(smoke->window, 1u);
    (void)made_since(smoke->window, false);
    const BTBHGPUSceneTargetView view = make_view(6u, 0u, 0u, 0u, 0u);
    const ViewShot shot = {1.3f, 1u, 1u, WIDTH, HEIGHT, BTBH_GPU_SCENE_UPSCALE_LINEAR};
    uint8_t *first = shot_pixels(smoke, &shot, false, &view);
    const uint32_t first_made = made_since(smoke->window, false);
    uint8_t *warm = shot_pixels(smoke, &shot, false, &view);
    const uint32_t warm_made = made_since(smoke->window, false);
    assert(btbh_gpu_window_scene_shadow_render_count(smoke->window, 6u) == 1);
    assert(btbh_gpu_window_scene_release_target_view(smoke->window, 6u) == BTBH_GPU_OK);
    assert(btbh_gpu_window_scene_shadow_render_count(smoke->window, 6u) == -1);
    /* Again: nothing to free. */
    assert(btbh_gpu_window_scene_release_target_view(smoke->window, 6u) == BTBH_GPU_OK);
    uint8_t *after = shot_pixels(smoke, &shot, false, &view);
    const uint32_t after_made = made_since(smoke->window, false);
    assert(btbh_gpu_window_scene_shadow_render_count(smoke->window, 6u) == 1);
    printf("release: made %u things in the view's first call, %u in the next, %u after its release\n",
        first_made, warm_made, after_made);
    assert(first_made > 0u && warm_made == 0u && after_made > 0u);
    /* The call after the release starts afresh, as the first one did (the second reused the first's map). */
    assert(same(first, after));
    free(first);
    free(warm);
    free(after);
    /* view_id 0: render_to_target's runtime, made again by the next render. */
    assert(btbh_gpu_window_scene_shadow_render_count(smoke->window, 0u) >= 0);
    assert(btbh_gpu_window_scene_release_target_view(smoke->window, 0u) == BTBH_GPU_OK);
    assert(btbh_gpu_window_scene_shadow_render_count(smoke->window, 0u) == -1);
    uint8_t *plain = shot_pixels(smoke, &shot, true, NULL);
    assert(btbh_gpu_window_scene_shadow_render_count(smoke->window, 0u) == 1);
    free(plain);
    (void)made_since(smoke->window, false);

    /* A kept view prewarmed with a rect: its first render makes nothing. */
    const BTBHGPUSceneTargetView prewarmed = make_view(7u, RECT_X, RECT_Y, RECT_W, RECT_H);
    const ViewShot prewarm_shot = {1.7f, 2u, 2u, WIDTH / 2u, HEIGHT / 2u, BTBH_GPU_SCENE_UPSCALE_FSR1};
    const BTBHGPUSceneLighting lighting = lighting_from((const float[3]){2.0f, 6.0f, -9.0f});
    const BTBHGPUScenePost post = post_chain(prewarm_shot.chain, prewarm_shot.upscale_mode);
    BTBHGPUSceneGeometryBatch batches[2];
    batches[0] = cube_batch(smoke->cube, 0u, 7u);
    batches[1] = cube_batch(smoke->cube, 7u, 2u);
    batches[1].emission_red = 255u;
    batches[1].emission_green = 230u;
    batches[1].emission_blue = 180u;
    batches[1].emission_energy = 6.0f;
    BTBHGPUSceneReport report;
    assert(btbh_gpu_window_scene_shadow_render_count(smoke->window, 7u) == -1);
    BTBHGPUResult result = btbh_gpu_window_scene_prewarm_render_to_target_view(
        smoke->window, smoke->target,
        shader_paths[0], shader_paths[1], shader_paths[2], shader_paths[3],
        shader_paths[4], shader_paths[5], shader_paths[6], shader_paths[7],
        shader_paths[8], shader_paths[9], shader_paths[10],
        SMOKE_SHADER_FORMAT, prewarm_shot.render_width, prewarm_shot.render_height,
        &lighting, &post, batches, 2u, &prewarmed, &report);
    if (result != BTBH_GPU_OK) {
        fprintf(stderr, "prewarm: %s\n", report.message);
    }
    assert(result == BTBH_GPU_OK);
    assert(report.drawable_width == WIDTH && report.upscale_mode == BTBH_GPU_SCENE_UPSCALE_FSR1);
    assert(btbh_gpu_window_scene_shadow_render_count(smoke->window, 7u) == 0);
    (void)made_since(smoke->window, false);
    uint8_t *prewarmed_pixels = shot_pixels(smoke, &prewarm_shot, false, &prewarmed);
    const uint32_t prewarmed_made = made_since(smoke->window, true);
    printf("a view prewarmed with a rect: its first render made %u things\n", prewarmed_made);
    assert(prewarmed_made == 0u);
    free(prewarmed_pixels);
    btbh_gpu_window_set_lazy_creation_log_enabled(smoke->window, 0u);
    (void)made_since(smoke->window, false);
}

/* ---- refusals ---- */
static void check_refusals(SmokeWindow *smoke) {
    const ViewShot shot = {0.5f, 0u, 1u, WIDTH, HEIGHT, BTBH_GPU_SCENE_UPSCALE_LINEAR};
    BTBHGPUSceneTargetView bad[8];
    bad[0] = make_view(BTBH_GPU_SCENE_TARGET_VIEW_CAPACITY + 1u, 0u, 0u, 0u, 0u);
    bad[1] = make_view(UINT32_MAX, 0u, 0u, 0u, 0u);
    bad[2] = make_view(1u, 0u, 0u, 0u, 0u);
    bad[2].reserved[0] = 1u;
    bad[3] = make_view(1u, 0u, 0u, 0u, 0u);
    bad[3].reserved[1] = 1u;
    bad[4] = make_view(1u, 0u, 0u, 0u, 0u);
    bad[4].reserved[2] = 0x80000000u;
    bad[5] = make_view(0u, 1u, 0u, WIDTH, HEIGHT);
    bad[6] = make_view(1u, 0u, HEIGHT - 9u, 10u, 10u);
    bad[7] = make_view(1u, UINT32_MAX, 0u, 2u, 2u);
    const BTBHGPUSceneLighting lighting = lighting_from((const float[3]){4.0f, 10.0f, 3.0f});
    const BTBHGPUScenePost post = post_chain(1u, BTBH_GPU_SCENE_UPSCALE_LINEAR);
    const BTBHGPUSceneGeometryBatch batch = cube_batch(smoke->cube, 0u, 1u);
    const int64_t view_one = btbh_gpu_window_scene_shadow_render_count(smoke->window, 1u);
    for (uint32_t index = 0u; index < 8u; ++index) {
        BTBHGPUSceneReport report;
        assert(render_shot(smoke, &shot, false, &bad[index], &report) == BTBH_GPU_INVALID_ARGUMENT);
        assert(report.result == BTBH_GPU_INVALID_ARGUMENT);
        assert(btbh_gpu_window_scene_prewarm_render_to_target_view(
            smoke->window, smoke->target,
            shader_paths[0], shader_paths[1], shader_paths[2], shader_paths[3],
            shader_paths[4], shader_paths[5], shader_paths[6], shader_paths[7],
            shader_paths[8], shader_paths[9], shader_paths[10],
            SMOKE_SHADER_FORMAT, WIDTH, HEIGHT, &lighting, &post, &batch, 1u, &bad[index], &report) ==
            BTBH_GPU_INVALID_ARGUMENT);
    }
    /* Nothing was made for them. */
    assert(btbh_gpu_window_scene_shadow_render_count(smoke->window, 1u) == view_one);
    assert(btbh_gpu_window_scene_release_target_view(smoke->window, BTBH_GPU_SCENE_TARGET_VIEW_CAPACITY + 1u) ==
        BTBH_GPU_INVALID_ARGUMENT);
    assert(btbh_gpu_window_scene_release_target_view(NULL, 1u) == BTBH_GPU_INVALID_ARGUMENT);
    /* The edges that fit. */
    const BTBHGPUSceneTargetView corner = make_view(BTBH_GPU_SCENE_TARGET_VIEW_CAPACITY, WIDTH - 1u, HEIGHT - 1u,
        1u, 1u);
    uint8_t *pixels = shot_pixels(smoke, &shot, false, &corner);
    /* All but the last pixel hold the clear colour. */
    for (uint32_t index = 0u; index + 1u < WIDTH * HEIGHT; ++index) {
        assert(memcmp(&pixels[index * 4u], clear_colour, 4u) == 0);
    }
    free(pixels);
    assert(btbh_gpu_window_scene_release_target_view(smoke->window, BTBH_GPU_SCENE_TARGET_VIEW_CAPACITY) ==
        BTBH_GPU_OK);
    printf("refusals: a view_id over the capacity, a reserved field, a rect outside the target\n");
}

int main(void) {
    (void)setvbuf(stdout, NULL, _IONBF, 0);
    for (int index = 0; index < 11; ++index) {
        (void)snprintf(shader_paths[index], sizeof(shader_paths[index]), "%s/%s%s", BTBH_GPU_SMOKE_SHADER_DIR,
            shader_names[index], SMOKE_SHADER_EXTENSION);
        FILE *file = fopen(shader_paths[index], "rb");
        if (file == NULL) {
            fprintf(stderr, "skipped: no shader %s\n", shader_paths[index]);
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
    SmokeWindow smoke = open_window(device, "BTBH target view smoke");
    printf("target format %d\n", (int)smoke.target->format);
    check_view_zero(&smoke);
    check_kept_shadows(&smoke);
    check_kept_cascades(&smoke);
    check_rects(&smoke);
    check_release_and_prewarm(&smoke);
    check_refusals(&smoke);
    close_window(&smoke);
    check_views_apart(device);
    btbh_gpu_device_shutdown(device);
    btbh_gpu_device_destroy(device);
    printf("target view smoke passed\n");
    return 0;
}
