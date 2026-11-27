/* Scene views (btbh_gpu_window_scene_present_views, RFC 0014 phase 1, since 0.16.0): two cameras in one present, each
 * clipped to its half of the frame. Hidden windows; each frame is copied out of the window and read back.
 *
 * - One view with a zero clip draws byte for byte as btbh_gpu_window_scene_present_instances_with_visibility, in two
 *   windows that present the same frames: six post chains (none; SSAO, SSR and FXAA; HBAO with contact AO, fog,
 *   vignette, CAS and SMAA; with bloom; the radiance cascades with SSAO; FSR 3 from half the size over four frames),
 *   with the call's visibility and the opaque sort. Its report holds every draw. (The identity runs also send the
 *   other smokes' window presents through it: BTBH_SMOKE_PRESENT_VIEWS=1, btbh_gpu_smoke_present.h.)
 * - Splits against the two views' one-view frames joined along the line (join_check). The rasterizer clips each view's
 *   triangles at its line, and the vertices it makes there interpolate a rounding apart from the whole triangle's: a
 *   clipped half differs from a whole frame by up to TOLERANCE levels here and there, and a pixel on an edge or a
 *   threshold (a silhouette, a shadow's edge) can fall the other way, taking a value its own frame has beside it (an
 *   edge flip, at most EDGE_FLIP_LIMIT). Anything else beyond LINE_BAND of the line is a fault.
 *   - A vertical split, (1, 0, -W/2) and (-1, 0, W/2), with fog and tonemap (no pass that samples its neighbours): no
 *     fault. With the HBAO and SSR, or the SSAO and SSR: faults only within TAP_BAND_LIMIT, where the whole frames'
 *     taps reached across the line. That the split's taps don't: the same camera in both views, one seeing a raised
 *     step the other doesn't, leaves the other's half byte for byte as without the step (where one view of the step
 *     darkens the floor beside it). So does a line through a half-size texel's centre, and the diagonal, for view 0's
 *     pixels beyond the band by the line that both views draw (the second last): no read takes what lies in it.
 *   - A diagonal split at 30 degrees, at the window's size and at three quarters of it (scaled linearly): no fault, and
 *     no pixel drawn by neither view. The control, both views of one camera on the same line, shows the edge flips
 *     the clipping alone makes. BTBH_VIEWS_SMOKE_DIAG=1 prints every pixel that differs by more than TOLERANCE.
 * - Shadows: equal sun matrices draw one map a frame, different ones two (btbh_gpu_window_scene_shadow_render_count);
 *   at a sun period of 2 the two maps take turns, one a frame. A second view's own lighting (its sun map, its spot
 *   light and map, a point light) draws its half as a one-view frame with that lighting does. The report's sun_map and
 *   spot_map say which map each view read.
 * - The per-view report: each view's draws and instances, their sum the frame's draws.
 * - Fallbacks: two views with FSR 3 run FSR 1 (report.upscale_mode), with the radiance cascades none
 *   (BTBH_GPU_SCENE_RC_FALLBACK_VIEWS); the next one-view frame runs both again, the cascades with a new history. A
 *   prewarm for two views, and a present of two views refused, leave the history. btbh_gpu_window_scene_prewarm_views
 *   says FSR 1 for two views.
 * - Refusals (BTBH_GPU_INVALID_ARGUMENT, naming the view, no frame left waiting): views NULL, 0 or 3 views, a camera
 *   not finite or not invertible, a clip not finite, two views without a clip line, one view's clip (0, 0, c) with c
 *   not 0, a visibility byte above 1, an unknown flag, reserved set, an invalid lighting, a lifted batch that a view's
 *   shadow_visibility lets into its sun map; and (SPIR-V) old vertex shaders without the clip distance, and a missing
 *   production_post_view.vert, for two views but not for one.
 *
 * BTBH_SMOKE_DUMP=<dir> writes the images. It needs a GPU and the production shaders; with no device it is skipped
 * (77). */
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

#if !defined(BTBH_GPU_SCENE_HAS_VIEWS)
#error "BTBH_GPU_SCENE_HAS_VIEWS"
#endif

#define SMOKE_SKIP 77
#define WIDTH 320u
#define HEIGHT 180u
/* Three quarters of it, for the diagonal at another render size (scaled linearly). */
#define SMALL_WIDTH 240u
#define SMALL_HEIGHT 135u
/* How far from the line (output pixels) a pixel of a split may differ from its view's one-view frame by more than a
   rounding: without neighbour passes (the line itself: a pixel within a quarter of a render pixel of it is drawn by
   both views), and with the HBAO or the SSAO with SSR (the reach of the whole frames' taps across the line; measured on
   NVIDIA within 6 px at 320 x 180, printed). */
#define LINE_BAND 1.5f
#define TAP_BAND_LIMIT 48.0f

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
    BTBHGPUCustomMesh *cube;
    BTBHGPUTexture *frame;
    BTBHGPUTexture *small;
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

#define CAMERA_NEAR 0.1f
#define CAMERA_FAR 60.0f
#define CAMERA_FOV 0.85f

/* orthographic: a light's map (half extent `extent`, depth 0.5..depth); else a perspective camera. */
static SmokeCamera look_at(const float eye[3], const float centre[3], float fov_y, float aspect, float near_plane,
    float far_plane, float extent) {
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
    if (extent > 0.0f) {
        const float rcp = 1.0f / extent;
        const float range = 1.0f / (near_plane - far_plane);
        const float orthographic[16] = {
            rcp, 0.0f, 0.0f, 0.0f,
            0.0f, rcp, 0.0f, 0.0f,
            0.0f, 0.0f, range, 0.0f,
            0.0f, 0.0f, range * near_plane, 1.0f,
        };
        memcpy(projection, orthographic, sizeof(projection));
    } else {
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

/* The cameras: round the scene at `angle`, looking down on it, so that the floor fills the frame. */
static SmokeCamera camera_at(float angle) {
    const float eye[3] = {7.0f * sinf(angle), 6.5f, 7.0f * cosf(angle)};
    const float centre[3] = {0.0f, 0.3f, 0.0f};
    return look_at(eye, centre, CAMERA_FOV, (float)WIDTH / (float)HEIGHT, CAMERA_NEAR, CAMERA_FAR, 0.0f);
}

static const float suns[3][3] = {{4.0f, 10.0f, 3.0f}, {-9.0f, 6.0f, 2.0f}, {2.0f, 7.0f, -9.0f}};

/* The call's light: a sun from suns[sun] with its map over the middle of the scene (shadow on or off), and with `spot`
   a spot light from spot_at (its own map). */
static BTBHGPUSceneLighting lighting_from(uint32_t sun, bool shadow, int spot) {
    BTBHGPUSceneLighting lighting;
    memset(&lighting, 0, sizeof(lighting));
    lighting.ambient_red = 1.0f;
    lighting.ambient_green = 1.0f;
    lighting.ambient_blue = 1.0f;
    lighting.ambient_energy = 0.35f;
    lighting.directional_energy = 2.0f;
    lighting.directional_red = 1.0f;
    lighting.directional_green = 0.95f;
    lighting.directional_blue = 0.9f;
    const float *from = suns[sun % 3u];
    float direction[3] = {-from[0], -from[1], -from[2]};
    normalize3(direction);
    lighting.directional_x = direction[0];
    lighting.directional_y = direction[1];
    lighting.directional_z = direction[2];
    lighting.spot_range = 8.0f;
    lighting.spot_falloff = 1.0f;
    lighting.spot_inner_cutoff = 1.0f;
    const float centre[3] = {0.0f, 0.0f, 0.0f};
    const SmokeCamera light = look_at(from, centre, 1.0f, 1.0f, 1.0f, 30.0f, 9.0f);
    memcpy(lighting.directional_shadow_view_projection, light.view_projection,
        sizeof(lighting.directional_shadow_view_projection));
    lighting.directional_shadow_enabled = shadow ? 1u : 0u;
    lighting.directional_shadow_softness = 1.0f;
    if (spot >= 0) {
        /* A spot over one side of the scene, looking down on it, with its shadow. */
        const float spot_positions[2][3] = {{2.5f, 5.0f, 1.5f}, {-2.0f, 5.0f, -2.0f}};
        const float *position = spot_positions[spot % 2];
        const float target[3] = {position[0] * 0.3f, 0.0f, position[2] * 0.3f};
        float spot_direction[3] = {target[0] - position[0], target[1] - position[1], target[2] - position[2]};
        normalize3(spot_direction);
        lighting.spot_position_x = position[0];
        lighting.spot_position_y = position[1];
        lighting.spot_position_z = position[2];
        lighting.spot_range = 20.0f;
        lighting.spot_direction_x = spot_direction[0];
        lighting.spot_direction_y = spot_direction[1];
        lighting.spot_direction_z = spot_direction[2];
        lighting.spot_energy = 6.0f;
        lighting.spot_red = 1.0f;
        lighting.spot_green = 0.7f;
        lighting.spot_blue = 0.4f;
        lighting.spot_falloff = 1.0f;
        lighting.spot_inner_cutoff = cosf(0.35f);
        lighting.spot_outer_cutoff = cosf(0.6f);
        const SmokeCamera spot_camera = look_at(position, target, 1.3f, 1.0f, 0.05f, 20.0f, 0.0f);
        memcpy(lighting.spot_shadow_view_projection, spot_camera.view_projection,
            sizeof(lighting.spot_shadow_view_projection));
        lighting.spot_shadow_enabled = 1u;
        lighting.spot_shadow_softness = 1.0f;
        lighting.spot_shadow_near = 0.05f;
    }
    return lighting;
}

/* The radiance cascades (chain 4): additive, tier Low, one glow between the cubes. */
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

enum {
    CHAIN_PLAIN = 0,         /* ACES alone */
    CHAIN_SSAO_SSR_FXAA = 1,
    CHAIN_HBAO_CONTACT = 2,  /* HBAO with contact AO, fog, vignette, CAS, SMAA high */
    CHAIN_BLOOM = 3,         /* chain 1 with bloom */
    CHAIN_CASCADES = 4,      /* the radiance cascades with SSAO */
    CHAIN_FOG = 5,           /* fog and ACES: nothing that samples a neighbour */
    CHAIN_HBAO_SSR = 6,      /* HBAO, SSR and fog, no anti-aliasing */
    CHAIN_SSAO_SSR = 7,      /* SSAO and SSR, no anti-aliasing */
    CHAIN_COUNT = 8
};

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
    /* (Read by FSR 3 alone.) */
    post.temporal_camera_near = CAMERA_NEAR;
    post.temporal_camera_far = CAMERA_FAR;
    post.temporal_vertical_fov = CAMERA_FOV;
    post.temporal_frame_seconds = 1.0f / 60.0f;
    post.temporal_meters_per_unit = 1.0f;
    post.temporal_reactive_scale = 0.3f;
    if (chain == CHAIN_SSAO_SSR_FXAA || chain == CHAIN_BLOOM || chain == CHAIN_SSAO_SSR || chain == CHAIN_HBAO_SSR) {
        post.ssao_enabled = chain == CHAIN_HBAO_SSR ? 2u : 1u;
        post.ssr_intensity = 0.6f;
        post.ssr_max_roughness = 0.9f;
        post.ssr_max_distance = 12.0f;
        post.ssr_thickness = 0.5f;
        post.anti_aliasing = chain == CHAIN_SSAO_SSR || chain == CHAIN_HBAO_SSR ? 0u : 3u; /* FXAA */
    }
    if (chain == CHAIN_HBAO_SSR) {
        post.ssao_radius = 0.8f;
        post.ssao_intensity = 1.0f;
        post.ssao_power = 1.5f;
        post.ssao_bias = 0.05f;
    }
    if (chain == CHAIN_HBAO_CONTACT) {
        post.ssao_enabled = 2u;
        post.ssao_radius = 0.8f;
        post.ssao_intensity = 1.0f;
        post.ssao_power = 1.5f;
        post.ssao_bias = 0.05f;
        post.ssao_contact = 1u;
        post.vignette_enabled = 1u;
        post.vignette_intensity = 0.4f;
        post.vignette_smoothness = 0.5f;
        post.cas_enabled = 1u;
        post.cas_sharpness = 0.5f;
        post.anti_aliasing = 2u; /* SMAA high */
    }
    if (chain == CHAIN_HBAO_CONTACT || chain == CHAIN_FOG || chain == CHAIN_HBAO_SSR) {
        post.fog_enabled = 1u;
        post.fog_color[0] = 0.5f;
        post.fog_color[1] = 0.55f;
        post.fog_color[2] = 0.6f;
        post.fog_density = 0.06f;
        post.fog_start_distance = 4.0f;
        post.fog_maximum_opacity = 0.7f;
    }
    if (chain == CHAIN_CASCADES) {
        post.ssao_enabled = 1u;
        BTBHGPUSceneRadianceCascades *cascades = &post.radiance_cascades;
        cascades->tier = BTBH_GPU_SCENE_RC_LOW;
        cascades->indirect_intensity = 1.0f;
        cascades->hero_omni_emitter_index = -1;
        cascades->additive = 1u;
        cascades->emitter_count = 1u;
        cascades->emitters = cascades_emitter();
    }
    if (chain == CHAIN_BLOOM) {
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

static void dump(const char *name, const uint8_t *pixels, uint32_t width, uint32_t height) {
    const char *directory = SDL_getenv("BTBH_SMOKE_DUMP");
    if (directory == NULL) {
        return;
    }
    char path[1024];
    (void)snprintf(path, sizeof(path), "%s/%s.png", directory, name);
    (void)btbh_gpu_write_png_rgba8(path, width, height, pixels);
}

static const uint8_t clear_colour[4] = {23u, 41u, 67u, 255u};

/* The scene: a floor, cubes round the middle, two glowing ones, and a raised step over the floor's +x side (batch 3,
   drawn where a visibility lets it in). */
#define BATCH_COUNT 4u
#define INSTANCE_COUNT 10u

typedef struct Frame {
    const ShaderSet *shaders;
    uint32_t view_count;
    float angle[2];
    float clip[2][3];
    const uint8_t *visibility[2];
    const uint8_t *shadow_visibility[2];
    const BTBHGPUSceneLighting *view_lighting[2];
    uint32_t flags[2];
    uint32_t reserved[2];
    /* NULL: views as above; else this array of views (refusals). */
    const BTBHGPUSceneView *views_override;
    uint32_t view_count_override;
    bool use_override;
    BTBHGPUSceneLighting lighting;
    uint32_t chain;
    uint8_t upscale_mode;
    uint8_t temporal_reset;
    uint8_t opaque_sort;
    uint32_t render_width;
    uint32_t render_height;
    /* One view through btbh_gpu_window_scene_present_instances_with_visibility. */
    bool old_entry;
    const uint8_t *scene_visibility;
    const uint8_t *directional_shadow_visibility;
    /* The floor (batch 0) lifted by this (refusals). */
    const BTBHGPUSceneHeightLift *floor_lift;
} Frame;

static Frame frame_default(void) {
    Frame frame;
    memset(&frame, 0, sizeof(frame));
    frame.shaders = &shaders;
    frame.view_count = 1u;
    frame.angle[0] = 0.35f;
    frame.angle[1] = 1.45f;
    frame.lighting = lighting_from(0u, true, -1);
    frame.chain = CHAIN_PLAIN;
    frame.upscale_mode = BTBH_GPU_SCENE_UPSCALE_LINEAR;
    frame.render_width = WIDTH;
    frame.render_height = HEIGHT;
    return frame;
}

/* Without the step, unless a visibility shows it. */
static const uint8_t no_step[BATCH_COUNT] = {1u, 1u, 1u, 0u};
static const uint8_t with_step[BATCH_COUNT] = {1u, 1u, 1u, 1u};

static BTBHGPUResult present_frame(SmokeWindow *smoke, const Frame *frame, BTBHGPUSceneViewReport view_reports[2],
    BTBHGPUSceneReport *report) {
    const float floor_colour[4] = {0.55f, 0.58f, 0.5f, 1.0f};
    const float step_colour[4] = {0.62f, 0.5f, 0.45f, 1.0f};
    const float colours[6][4] = {
        {0.9f, 0.25f, 0.2f, 1.0f},
        {0.2f, 0.6f, 0.9f, 1.0f},
        {0.95f, 0.85f, 0.3f, 1.0f},
        {0.3f, 0.8f, 0.35f, 1.0f},
        {1.0f, 1.0f, 1.0f, 1.0f},
        {0.8f, 0.4f, 0.9f, 1.0f},
    };
    float instances[INSTANCE_COUNT][24];
    instance(instances[0], 0.0f, -0.55f, 0.0f, 60.0f, 0.1f, 60.0f, floor_colour);
    for (uint32_t index = 0u; index < 6u; ++index) {
        const float a = 1.05f * (float)index;
        const float r = 1.2f + 0.45f * (float)index;
        /* (Standing a little over the floor: faces that touch it fight for its depth, and the clipped halves' depths
           round apart, so a contact edge could fall either way.) */
        const float height = 1.0f + 0.3f * (float)(index % 2u) + 0.15f * (float)(index % 3u);
        instance(instances[1u + index], r * sinf(a), -0.48f + 0.5f * height, r * cosf(a), 0.9f, height, 0.9f,
            colours[index]);
    }
    instance(instances[7], -1.8f, 1.6f, 0.6f, 0.5f, 0.5f, 0.5f, colours[4]);
    instance(instances[8], 1.4f, 1.9f, -1.2f, 0.45f, 0.45f, 0.45f, colours[2]);
    /* The step: 0.35 above the floor over x > 0.4 (to the frame's edge). */
    instance(instances[9], 15.4f, -0.35f, 0.0f, 30.0f, 0.4f, 60.0f, step_colour);
    BTBHGPUSceneGeometryBatch batches[BATCH_COUNT];
    batches[0] = cube_batch(smoke->cube, 0u, 1u);
    batches[1] = cube_batch(smoke->cube, 1u, 6u);
    batches[2] = cube_batch(smoke->cube, 7u, 2u);
    batches[2].emission_red = 255u;
    batches[2].emission_green = 230u;
    batches[2].emission_blue = 180u;
    batches[2].emission_energy = 6.0f;
    batches[3] = cube_batch(smoke->cube, 9u, 1u);
    /* (A lifted batch casts into no spot map: RFC 0012 phase 1.) */
    batches[0].height_lift = frame->floor_lift;
    batches[0].spot_shadow_visible = frame->floor_lift != NULL ? 0u : batches[0].spot_shadow_visible;
    BTBHGPUScenePost post = post_chain(frame->chain, frame->upscale_mode);
    post.temporal_reset = frame->temporal_reset;
    post.opaque_sort = frame->opaque_sort;
    const uint8_t *scene_visibility = frame->scene_visibility != NULL ? frame->scene_visibility : NULL;
    const ShaderSet *set = frame->shaders;
    if (frame->old_entry) {
        const SmokeCamera camera = camera_at(frame->angle[0]);
        return btbh_gpu_window_scene_present_instances_with_visibility(
            smoke->window,
            set->paths[0], set->paths[1], set->paths[2], set->paths[3], set->paths[4], set->paths[5],
            set->paths[6], set->paths[7], set->paths[8], set->paths[9], set->paths[10],
            SMOKE_SHADER_FORMAT, frame->render_width, frame->render_height,
            camera.view_projection, camera.right, camera.up, camera.position,
            &frame->lighting, &post, &instances[0][0], INSTANCE_COUNT, INSTANCE_COUNT, batches,
            scene_visibility, frame->directional_shadow_visibility, BATCH_COUNT,
            clear_colour[0], clear_colour[1], clear_colour[2], clear_colour[3], report);
    }
    BTBHGPUSceneView views[2];
    memset(views, 0, sizeof(views));
    for (uint32_t index = 0u; index < 2u; ++index) {
        const SmokeCamera camera = camera_at(frame->angle[index]);
        memcpy(views[index].view_projection, camera.view_projection, sizeof(views[index].view_projection));
        memcpy(views[index].right, camera.right, sizeof(views[index].right));
        memcpy(views[index].up, camera.up, sizeof(views[index].up));
        memcpy(views[index].position, camera.position, sizeof(views[index].position));
        memcpy(views[index].clip, frame->clip[index], sizeof(views[index].clip));
        views[index].visibility = frame->visibility[index];
        views[index].shadow_visibility = frame->shadow_visibility[index];
        views[index].lighting = frame->view_lighting[index];
        views[index].flags = frame->flags[index];
        views[index].reserved = frame->reserved[index];
    }
    return btbh_gpu_window_scene_present_views(
        smoke->window,
        set->paths[0], set->paths[1], set->paths[2], set->paths[3], set->paths[4], set->paths[5],
        set->paths[6], set->paths[7], set->paths[8], set->paths[9], set->paths[10],
        SMOKE_SHADER_FORMAT, frame->render_width, frame->render_height,
        frame->use_override ? frame->views_override : views,
        frame->use_override ? frame->view_count_override : frame->view_count,
        &frame->lighting, &post, &instances[0][0], INSTANCE_COUNT, INSTANCE_COUNT, batches,
        scene_visibility, frame->directional_shadow_visibility, BATCH_COUNT,
        clear_colour[0], clear_colour[1], clear_colour[2], clear_colour[3], view_reports, report);
}

/* Presents `frame`, which must succeed, and reads it back (the window's frame texture, or the small one for a render
   that stays at its size). */
static uint8_t *frame_pixels(SmokeWindow *smoke, const Frame *frame, BTBHGPUSceneViewReport view_reports[2],
    BTBHGPUSceneReport *out_report) {
    BTBHGPUSceneReport report;
    BTBHGPUSceneViewReport local_views[2];
    const BTBHGPUResult result = present_frame(smoke, frame, view_reports != NULL ? view_reports : local_views,
        &report);
    if (result != BTBH_GPU_OK) {
        fprintf(stderr, "present: %s\n", report.message);
    }
    assert(result == BTBH_GPU_OK);
    BTBHGPUTexture *texture = report.drawable_width == WIDTH &&
            (frame->render_width == WIDTH || frame->upscale_mode != BTBH_GPU_SCENE_UPSCALE_LINEAR)
        ? smoke->frame
        : smoke->small;
    BTBHGPUTextureReport copied;
    const BTBHGPUResult copy_result = btbh_gpu_window_copy_pending_scene_texture(smoke->window, texture, &copied);
    if (copy_result != BTBH_GPU_OK) {
        fprintf(stderr, "copy: %s\n", copied.message);
    }
    assert(copy_result == BTBH_GPU_OK);
    if (out_report != NULL) {
        *out_report = report;
    }
    return read_texture(smoke->window, texture);
}

static uint32_t differing_pixels(const uint8_t *a, const uint8_t *b, uint32_t width, uint32_t height) {
    uint32_t count = 0u;
    for (uint32_t index = 0u; index < width * height; ++index) {
        if (memcmp(a + index * 4u, b + index * 4u, 4u) != 0) {
            count += 1u;
        }
    }
    return count;
}

/* A split's pixels against its two views' one-view frames joined along its line (clip of view 0, in output pixels;
   the frames at width x height, the output at WIDTH x HEIGHT).
   - The rasterizer clips each view's triangles that cross its line (the full-screen one too), and the vertices it
     makes there interpolate the varyings a rounding apart: the clipped halves differ from the whole frames by a level
     or two here and there (TOLERANCE), and a pixel whose centre lies on an edge or a threshold (a silhouette, a shadow's
     edge) of such a triangle can fall the other way. Such a pixel takes a value its own view's frame has right beside
     it: an edge flip, counted apart.
   - Within LINE_BAND of the line, a pixel drawn by both views shows the nearer surface (and the second view's
     screen-space passes): it may differ by any amount.
   - Anything else is a fault: a pixel beyond the band that differs by more than TOLERANCE and is no edge flip
     (faults, the farthest of them from the line), and one that is neither view's at all (neither). */
#define TOLERANCE 4

typedef struct JoinResult {
    uint32_t differing;
    uint32_t in_band;
    uint32_t edge_flips;
    /* (Those farther than TAP_BAND_LIMIT from the line.) */
    uint32_t far_edge_flips;
    uint32_t faults;
    float farthest_fault;
    float farthest_flip;
    uint32_t neither;
} JoinResult;

static int level_difference(const uint8_t *a, const uint8_t *b) {
    int largest = 0;
    for (uint32_t channel = 0u; channel < 4u; ++channel) {
        const int difference = abs((int)a[channel] - (int)b[channel]);
        largest = difference > largest ? difference : largest;
    }
    return largest;
}

/* Whether `value` is (within TOLERANCE) one of the 8 pixels round (x, y) in `frame`. */
static bool beside(const uint8_t *frame, uint32_t width, uint32_t height, uint32_t x, uint32_t y,
    const uint8_t *value) {
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const int nx = (int)x + dx;
            const int ny = (int)y + dy;
            if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= (int)width || ny >= (int)height) {
                continue;
            }
            if (level_difference(frame + ((size_t)ny * width + (size_t)nx) * 4u, value) <= TOLERANCE) {
                return true;
            }
        }
    }
    return false;
}

static JoinResult join_check(const uint8_t *split, const uint8_t *first, const uint8_t *second, const float clip[3],
    uint32_t width, uint32_t height) {
    JoinResult result;
    memset(&result, 0, sizeof(result));
    const float scale_x = (float)WIDTH / (float)width;
    const float scale_y = (float)HEIGHT / (float)height;
    const float normal = sqrtf(clip[0] * clip[0] + clip[1] * clip[1]);
    for (uint32_t y = 0u; y < height; ++y) {
        for (uint32_t x = 0u; x < width; ++x) {
            const float ox = ((float)x + 0.5f) * scale_x;
            const float oy = ((float)y + 0.5f) * scale_y;
            const float side = (clip[0] * ox + clip[1] * oy + clip[2]) / normal;
            const size_t offset = ((size_t)y * width + x) * 4u;
            const uint8_t *own = side >= 0.0f ? first : second;
            const int difference = level_difference(split + offset, own + offset);
            if (difference == 0) {
                continue;
            }
            result.differing += 1u;
            if (difference <= TOLERANCE) {
                continue;
            }
            if (fabsf(side) <= LINE_BAND) {
                result.in_band += 1u;
                continue;
            }
            const bool flip = beside(own, width, height, x, y, split + offset);
            if (SDL_getenv("BTBH_VIEWS_SMOKE_DIAG") != NULL) {
                printf("  differs at %u,%u (side %.2f) by %d%s\n", x, y, (double)side, difference,
                    flip ? ", an edge flip" : "");
            }
            if (flip) {
                result.edge_flips += 1u;
                result.far_edge_flips += fabsf(side) > TAP_BAND_LIMIT ? 1u : 0u;
                result.farthest_flip = fmaxf(result.farthest_flip, fabsf(side));
            } else {
                result.faults += 1u;
                result.farthest_fault = fmaxf(result.farthest_fault, fabsf(side));
            }
            if (level_difference(split + offset, first + offset) > TOLERANCE &&
                level_difference(split + offset, second + offset) > TOLERANCE && !flip) {
                result.neither += 1u;
            }
        }
    }
    return result;
}

static void print_join(const char *what, const JoinResult *join) {
    printf("%s: %u pixels differ from the joined views (by at most %d levels but %u within %.1f px of the line, %u "
        "edge flips out to %.1f px, %u faults out to %.1f px, %u neither view's)\n", what, join->differing,
        TOLERANCE, join->in_band, (double)LINE_BAND, join->edge_flips, (double)join->farthest_flip, join->faults,
        (double)join->farthest_fault, join->neither);
}

/* At most this many edge flips in a 320 x 180 frame (0.02 % of it; measured: 0 to 2). */
#define EDGE_FLIP_LIMIT 12u

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
    smoke.frame = btbh_gpu_window_render_target_create(smoke.window, WIDTH, HEIGHT, BTBH_GPU_TEXTURE_FILTER_LINEAR,
        &texture_report);
    assert(smoke.frame != NULL);
    smoke.small = btbh_gpu_window_render_target_create(smoke.window, SMALL_WIDTH, SMALL_HEIGHT,
        BTBH_GPU_TEXTURE_FILTER_LINEAR, &texture_report);
    assert(smoke.small != NULL);
    return smoke;
}

static void set_vertical_split(Frame *frame) {
    frame->view_count = 2u;
    const float right[3] = {1.0f, 0.0f, -0.5f * (float)WIDTH};
    const float left[3] = {-1.0f, 0.0f, 0.5f * (float)WIDTH};
    memcpy(frame->clip[0], right, sizeof(right));
    memcpy(frame->clip[1], left, sizeof(left));
}

static void set_diagonal_split(Frame *frame) {
    frame->view_count = 2u;
    const float c = cosf(0.5235988f);
    const float s = sinf(0.5235988f);
    const float centre_x = 0.5f * (float)WIDTH;
    const float centre_y = 0.5f * (float)HEIGHT;
    const float first[3] = {c, s, -(c * centre_x + s * centre_y)};
    const float second[3] = {-c, -s, c * centre_x + s * centre_y};
    memcpy(frame->clip[0], first, sizeof(first));
    memcpy(frame->clip[1], second, sizeof(second));
}

/* ---- One view against the present ---- */

static void check_one_view_identity(SmokeWindow *a, SmokeWindow *b) {
    static const uint8_t some_batches[BATCH_COUNT] = {1u, 0u, 1u, 1u};
    uint32_t compared = 0u;
    for (uint32_t chain = 0u; chain < CHAIN_COUNT; ++chain) {
        for (uint32_t variant = 0u; variant < 2u; ++variant) {
            Frame frame = frame_default();
            frame.chain = chain;
            frame.visibility[0] = NULL;
            /* Variant 1: the call's visibility (the step drawn, the cubes not) and the opaque sort. */
            frame.scene_visibility = variant == 0u ? no_step : some_batches;
            frame.opaque_sort = variant == 0u
                ? (uint8_t)BTBH_GPU_SCENE_OPAQUE_SORT_SUBMITTED
                : (uint8_t)BTBH_GPU_SCENE_OPAQUE_SORT_FRONT_TO_BACK;
            frame.old_entry = true;
            BTBHGPUSceneReport old_report;
            uint8_t *old_pixels = frame_pixels(a, &frame, NULL, &old_report);
            frame.old_entry = false;
            BTBHGPUSceneReport new_report;
            BTBHGPUSceneViewReport view_reports[2];
            uint8_t *new_pixels = frame_pixels(b, &frame, view_reports, &new_report);
            const uint32_t differing = differing_pixels(old_pixels, new_pixels, WIDTH, HEIGHT);
            if (differing != 0u) {
                fprintf(stderr, "one view, chain %u variant %u: %u pixels differ\n", chain, variant, differing);
            }
            assert(differing == 0u);
            assert(new_report.primitive_draw_count == old_report.primitive_draw_count);
            assert(new_report.upscale_mode == old_report.upscale_mode);
            assert(new_report.radiance_cascades.active_tier == old_report.radiance_cascades.active_tier);
            assert(frame.old_entry || view_reports[0].primitive_draw_count == new_report.primitive_draw_count);
            assert(frame.old_entry || view_reports[0].instance_count == (variant == 0u ? 9u : 4u));
            assert(frame.old_entry || view_reports[0].sun_map == 0u);
            assert(frame.old_entry || view_reports[0].spot_map == UINT32_MAX);
            if (chain == CHAIN_CASCADES) {
                assert(new_report.radiance_cascades.active_tier != BTBH_GPU_SCENE_RC_OFF);
            }
            if (chain == CHAIN_HBAO_CONTACT && variant == 0u) {
                dump("one_view_hbao_contact", new_pixels, WIDTH, HEIGHT);
            }
            free(old_pixels);
            free(new_pixels);
            compared += 1u;
        }
    }
    /* FSR 3 from half the size, four frames as the camera turns (a history), then a cut. (Not from three quarters: there
       two windows given the same FSR 3 frames through the present itself differ by 56 to 146 pixels on the frames
       after a reset, so the two calls can't be compared window against window.) */
    bool fsr3 = false;
    for (uint32_t index = 0u; index < 5u; ++index) {
        Frame frame = frame_default();
        frame.chain = CHAIN_SSAO_SSR_FXAA;
        frame.upscale_mode = BTBH_GPU_SCENE_UPSCALE_FSR3;
        frame.render_width = WIDTH / 2u;
        frame.render_height = HEIGHT / 2u;
        frame.angle[0] = 0.35f + 0.03f * (float)index;
        frame.temporal_reset = index == 4u ? 1u : 0u;
        frame.scene_visibility = no_step;
        frame.old_entry = true;
        BTBHGPUSceneReport old_report;
        uint8_t *old_pixels = frame_pixels(a, &frame, NULL, &old_report);
        frame.old_entry = false;
        BTBHGPUSceneReport new_report;
        uint8_t *new_pixels = frame_pixels(b, &frame, NULL, &new_report);
        const uint32_t temporal_differing = differing_pixels(old_pixels, new_pixels, WIDTH, HEIGHT);
        if (temporal_differing != 0u) {
            fprintf(stderr, "FSR 3 frame %u: %u pixels differ\n", index, temporal_differing);
            dump("fsr3_old", old_pixels, WIDTH, HEIGHT);
            dump("fsr3_new", new_pixels, WIDTH, HEIGHT);
        }
        assert(temporal_differing == 0u);
        assert(new_report.upscale_mode == old_report.upscale_mode);
        fsr3 = new_report.upscale_mode == BTBH_GPU_SCENE_UPSCALE_FSR3;
        free(old_pixels);
        free(new_pixels);
        compared += 1u;
    }
    printf("one view: %u frames byte for byte as the present (%s)\n", compared,
        fsr3 ? "FSR 3 among them" : "FSR 3 unavailable: FSR 1");
#if !defined(__APPLE__)
    assert(fsr3);
#endif
}

/* ---- Splits ---- */

static void check_vertical_split(SmokeWindow *smoke) {
    static const uint32_t chains[3] = {CHAIN_FOG, CHAIN_HBAO_SSR, CHAIN_SSAO_SSR};
    for (uint32_t which = 0u; which < 3u; ++which) {
        Frame frame = frame_default();
        frame.chain = chains[which];
        frame.scene_visibility = no_step;
        frame.old_entry = true;
        uint8_t *first = frame_pixels(smoke, &frame, NULL, NULL);
        const float first_angle = frame.angle[0];
        frame.angle[0] = frame.angle[1];
        uint8_t *second = frame_pixels(smoke, &frame, NULL, NULL);
        frame.angle[0] = first_angle;
        frame.old_entry = false;
        set_vertical_split(&frame);
        BTBHGPUSceneViewReport view_reports[2];
        BTBHGPUSceneReport report;
        uint8_t *split = frame_pixels(smoke, &frame, view_reports, &report);
        const JoinResult join = join_check(split, first, second, frame.clip[0], WIDTH, HEIGHT);
        print_join(which == 0u
            ? "vertical split, fog (no neighbour passes)"
            : (which == 1u ? "vertical split, HBAO and SSR" : "vertical split, SSAO and SSR"), &join);
        char name[64];
        (void)snprintf(name, sizeof(name), "vertical_split_%u", which);
        dump(name, split, WIDTH, HEIGHT);
        /* Without neighbour passes nothing differs beyond the line's band; with them, within the reach of the taps that
           the whole frames took across the line (the split's stay on their side). */
        assert(join.faults == 0u || which != 0u);
        assert(join.farthest_fault <= TAP_BAND_LIMIT);
        assert((which == 0u ? join.edge_flips : join.far_edge_flips) <= EDGE_FLIP_LIMIT);
        assert(join.neither == 0u || which != 0u);
        assert(view_reports[0].primitive_draw_count + view_reports[1].primitive_draw_count ==
            report.primitive_draw_count);
        free(first);
        free(second);
        free(split);
    }
}

/* No tap crosses the line: the same camera in both views; view 0 (the right half) sees the step, view 1 (the left)
   doesn't. View 1's half is the same with or without the step in view 0's. */
static void check_taps_stay(SmokeWindow *smoke) {
    static const uint32_t chains[2] = {CHAIN_HBAO_SSR, CHAIN_SSAO_SSR};
    for (uint32_t which = 0u; which < 2u; ++which) {
        Frame frame = frame_default();
        frame.chain = chains[which];
        /* No shadows: the step's would fall on both halves (world space, as it should). */
        frame.lighting = lighting_from(0u, false, -1);
        frame.angle[1] = frame.angle[0];
        set_vertical_split(&frame);
        frame.visibility[0] = with_step;
        frame.visibility[1] = no_step;
        uint8_t *stepped = frame_pixels(smoke, &frame, NULL, NULL);
        frame.visibility[0] = no_step;
        uint8_t *flat = frame_pixels(smoke, &frame, NULL, NULL);
        /* The left half (view 1's): x < W/2, the line's own column aside. */
        uint32_t differing = 0u;
        for (uint32_t y = 0u; y < HEIGHT; ++y) {
            for (uint32_t x = 0u; x + 1u < WIDTH / 2u; ++x) {
                const size_t offset = ((size_t)y * WIDTH + x) * 4u;
                if (memcmp(stepped + offset, flat + offset, 4u) != 0) {
                    differing += 1u;
                }
            }
        }
        /* The control: one view of the step. Its occlusion darkens the floor beside its edge. */
        Frame control = frame_default();
        control.chain = chains[which];
        control.lighting = frame.lighting;
        control.scene_visibility = with_step;
        uint8_t *occluded = frame_pixels(smoke, &control, NULL, NULL);
        printf("taps, %s: the other view's step changes %u pixels of this view's half\n",
            which == 0u ? "HBAO and SSR" : "SSAO and SSR", differing);
        dump(which == 0u ? "taps_hbao_stepped" : "taps_ssao_stepped", stepped, WIDTH, HEIGHT);
        dump(which == 0u ? "taps_hbao_one_view_step" : "taps_ssao_one_view_step", occluded, WIDTH, HEIGHT);
        assert(differing == 0u);
        free(stepped);
        free(flat);
        free(occluded);
    }
    /* The control proper: the HBAO darkens the floor beside the step in one view (so a tap that crossed would show). */
    Frame on = frame_default();
    on.chain = CHAIN_HBAO_SSR;
    on.lighting = lighting_from(0u, false, -1);
    on.scene_visibility = with_step;
    uint8_t *with_ao = frame_pixels(smoke, &on, NULL, NULL);
    on.chain = CHAIN_FOG;
    uint8_t *fog_only = frame_pixels(smoke, &on, NULL, NULL);
    /* (Fog differs from chain 6 only by the occlusion and the reflections.) */
    uint32_t darker = 0u;
    for (uint32_t index = 0u; index < WIDTH * HEIGHT; ++index) {
        const int luma_ao = with_ao[index * 4u] + with_ao[index * 4u + 1u] + with_ao[index * 4u + 2u];
        const int luma_fog = fog_only[index * 4u] + fog_only[index * 4u + 1u] + fog_only[index * 4u + 2u];
        if (luma_fog - luma_ao > 12) {
            darker += 1u;
        }
    }
    printf("taps, control: the HBAO darkens %u pixels of one view of the step\n", darker);
    assert(darker > 200u);
    free(with_ao);
    free(fog_only);
}

/* The band by the line: what lies within a quarter of a render pixel of it is drawn by both views, the second last (a
   screen-space pass's pixels and texels) or the nearer (the depth). A vertical line through a half-size texel's centre
   (x = 161: texel 80 of the SSAO's and HBAO's targets), and the diagonal, where pixels lie on it too. View 0 the left
   half, view 1 the right seeing the step or not, one camera: view 0's pixels (those it alone draws, beyond a quarter
   pixel of the line) are the same either way, for neither view's reads take what lies in the band. */
static void check_taps_by_the_line(SmokeWindow *smoke) {
    static const uint32_t chains[2] = {CHAIN_HBAO_SSR, CHAIN_SSAO_SSR};
    for (uint32_t which = 0u; which < 2u; ++which) {
        for (uint32_t layout = 0u; layout < 2u; ++layout) {
            Frame frame = frame_default();
            frame.chain = chains[which];
            frame.lighting = lighting_from(0u, false, -1);
            frame.angle[1] = frame.angle[0];
            if (layout == 0u) {
                frame.view_count = 2u;
                const float left[3] = {-1.0f, 0.0f, 161.0f};
                const float right[3] = {1.0f, 0.0f, -161.0f};
                memcpy(frame.clip[0], left, sizeof(left));
                memcpy(frame.clip[1], right, sizeof(right));
            } else {
                /* (View 0 the left half: the step lies on the right.) */
                set_diagonal_split(&frame);
                float right[3];
                memcpy(right, frame.clip[0], sizeof(right));
                memcpy(frame.clip[0], frame.clip[1], sizeof(right));
                memcpy(frame.clip[1], right, sizeof(right));
            }
            frame.visibility[0] = no_step;
            frame.visibility[1] = with_step;
            uint8_t *stepped = frame_pixels(smoke, &frame, NULL, NULL);
            frame.visibility[1] = no_step;
            uint8_t *flat = frame_pixels(smoke, &frame, NULL, NULL);
            /* (The clips are unit normals: d is the distance in pixels.) */
            uint32_t differing = 0u;
            uint32_t near_step = 0u;
            float farthest = 0.0f;
            for (uint32_t y = 0u; y < HEIGHT; ++y) {
                for (uint32_t x = 0u; x < WIDTH; ++x) {
                    const size_t offset = ((size_t)y * WIDTH + x) * 4u;
                    const float d = frame.clip[0][0] * ((float)x + 0.5f) + frame.clip[0][1] * ((float)y + 0.5f) +
                        frame.clip[0][2];
                    const bool differs = memcmp(stepped + offset, flat + offset, 4u) != 0;
                    if (differs && d >= 0.3f) {
                        differing += 1u;
                        farthest = d > farthest ? d : farthest;
                    }
                    if (differs && d < -0.3f && d > -8.0f) {
                        near_step += 1u;
                    }
                }
            }
            printf("taps by the line, %s, %s: the other view's step changes %u pixels of view 0's half (out to %.1f px; "
                "%u of its own within 8 px of the line)\n",
                which == 0u ? "HBAO and SSR" : "SSAO and SSR",
                layout == 0u ? "through texel 80's centre" : "diagonal", differing, (double)farthest, near_step);
            char name[64];
            (void)snprintf(name, sizeof(name), "taps_line_%s_%s", which == 0u ? "hbao" : "ssao",
                layout == 0u ? "texel" : "diagonal");
            dump(name, stepped, WIDTH, HEIGHT);
            assert(differing == 0u);
            assert(near_step > 0u);
            free(stepped);
            free(flat);
        }
    }
}

static void check_diagonal_split(SmokeWindow *smoke) {
    for (uint32_t size = 0u; size < 2u; ++size) {
        const uint32_t width = size == 0u ? WIDTH : SMALL_WIDTH;
        const uint32_t height = size == 0u ? HEIGHT : SMALL_HEIGHT;
        Frame frame = frame_default();
        frame.chain = CHAIN_PLAIN;
        frame.scene_visibility = no_step;
        frame.render_width = width;
        frame.render_height = height;
        frame.old_entry = true;
        uint8_t *first = frame_pixels(smoke, &frame, NULL, NULL);
        const float first_angle = frame.angle[0];
        frame.angle[0] = frame.angle[1];
        uint8_t *second = frame_pixels(smoke, &frame, NULL, NULL);
        frame.angle[0] = first_angle;
        /* The clear colour as the chain leaves it: a frame with nothing drawn. */
        static const uint8_t nothing[BATCH_COUNT] = {0u, 0u, 0u, 0u};
        frame.scene_visibility = nothing;
        uint8_t *empty = frame_pixels(smoke, &frame, NULL, NULL);
        frame.scene_visibility = no_step;
        frame.old_entry = false;
        set_diagonal_split(&frame);
        uint8_t *split = frame_pixels(smoke, &frame, NULL, NULL);
        dump(size == 0u ? "diagonal_first" : "diagonal_first_small", first, width, height);
        dump(size == 0u ? "diagonal_second" : "diagonal_second_small", second, width, height);
        const JoinResult join = join_check(split, first, second, frame.clip[0], width, height);
        uint32_t cleared = 0u;
        for (uint32_t index = 0u; index < width * height; ++index) {
            if (memcmp(split + index * 4u, empty, 4u) == 0 ||
                (split[index * 4u] == 0u && split[index * 4u + 1u] == 0u && split[index * 4u + 2u] == 0u)) {
                cleared += 1u;
            }
        }
        char what[96];
        (void)snprintf(what, sizeof(what), "diagonal split at %ux%u (%u drawn by neither)", width, height, cleared);
        print_join(what, &join);
        dump(size == 0u ? "diagonal_split" : "diagonal_split_small", split, width, height);
        /* (Every first frame pixel is the floor or a cube: none is the clear colour.) */
        for (uint32_t index = 0u; index < width * height; ++index) {
            assert(memcmp(first + index * 4u, empty, 4u) != 0);
        }
        assert(cleared == 0u);
        assert(join.faults == 0u && join.neither == 0u);
        assert(join.edge_flips <= EDGE_FLIP_LIMIT);
        /* The control: both views of one camera, split on the same line. No pixel can be the other view's or the
           band's: what differs from the one-view frame is the clipping's rounding alone. */
        frame.angle[1] = frame.angle[0];
        uint8_t *same = frame_pixels(smoke, &frame, NULL, NULL);
        const JoinResult control = join_check(same, first, first, frame.clip[0], width, height);
        (void)snprintf(what, sizeof(what), "diagonal split of one camera at %ux%u", width, height);
        print_join(what, &control);
        assert(control.faults == 0u && control.neither == 0u);
        assert(control.edge_flips <= EDGE_FLIP_LIMIT);
        free(same);
        free(first);
        free(second);
        free(empty);
        free(split);
    }
}

/* ---- Shadows ---- */

static int64_t shadow_maps(SmokeWindow *smoke) {
    return btbh_gpu_window_scene_shadow_render_count(smoke->window, BTBH_GPU_SCENE_WINDOW_RUNTIME);
}

static void check_shadows(SmokeWindow *smoke) {
    assert(btbh_gpu_window_scene_set_shadow_update_periods(smoke->window, 1u, 1u, 1u) == 1u);
    Frame frame = frame_default();
    frame.chain = CHAIN_FOG;
    frame.scene_visibility = no_step;
    set_vertical_split(&frame);
    BTBHGPUSceneViewReport view_reports[2];
    /* Equal matrices: one map a frame, read by both. */
    int64_t before = shadow_maps(smoke);
    free(frame_pixels(smoke, &frame, view_reports, NULL));
    assert(shadow_maps(smoke) - before == 1);
    assert(view_reports[0].sun_map == 0u && view_reports[1].sun_map == 0u);
    assert(view_reports[0].spot_map == UINT32_MAX && view_reports[1].spot_map == UINT32_MAX);
    /* The second view's own lighting: another sun matrix (its map), a spot of its own (its map), a point light. */
    BTBHGPUSceneLighting own = lighting_from(1u, true, 1);
    own.point_light_count = 1u;
    const float point[8] = {-1.0f, 1.0f, 2.0f, 4.0f, 0.3f, 0.6f, 1.0f, 3.0f};
    memcpy(own.point_lights[0], point, sizeof(point));
    /* (Its sun's direction and colour are the call's: the call's light with its spot.) */
    frame.lighting = lighting_from(0u, true, 0);
    own.directional_x = frame.lighting.directional_x;
    own.directional_y = frame.lighting.directional_y;
    own.directional_z = frame.lighting.directional_z;
    frame.view_lighting[1] = &own;
    before = shadow_maps(smoke);
    uint8_t *split = frame_pixels(smoke, &frame, view_reports, NULL);
    /* Sun and spot, each twice. */
    assert(shadow_maps(smoke) - before == 4);
    assert(view_reports[0].sun_map == 0u && view_reports[1].sun_map == 1u);
    assert(view_reports[0].spot_map == 0u && view_reports[1].spot_map == 1u);
    /* Each half as a one-view frame with its view's lighting. */
    Frame single = frame;
    single.view_count = 1u;
    single.view_lighting[1] = NULL;
    memset(single.clip, 0, sizeof(single.clip));
    single.old_entry = true;
    uint8_t *first = frame_pixels(smoke, &single, NULL, NULL);
    single.angle[0] = frame.angle[1];
    single.lighting = own;
    uint8_t *second = frame_pixels(smoke, &single, NULL, NULL);
    const JoinResult join = join_check(split, first, second, frame.clip[0], WIDTH, HEIGHT);
    print_join("own lighting, against each view's one-view frame with its lighting", &join);
    dump("own_lighting_split", split, WIDTH, HEIGHT);
    assert(join.faults == 0u && join.neither == 0u);
    assert(join.edge_flips <= EDGE_FLIP_LIMIT);
    free(split);
    free(first);
    free(second);
    /* The same lighting for both views again: a spot map shared, read by both. */
    frame.view_lighting[1] = NULL;
    free(frame_pixels(smoke, &frame, view_reports, NULL));
    assert(view_reports[1].spot_map == 0u && view_reports[1].sun_map == 0u);
    /* At a sun period of 2 the two sun maps take turns: after the first frame (both drawn), one a frame. */
    frame.lighting = lighting_from(0u, true, -1);
    own = lighting_from(1u, true, -1);
    frame.view_lighting[1] = &own;
    assert(btbh_gpu_window_scene_set_shadow_update_periods(smoke->window, 2u, 1u, 1u) == 1u);
    int64_t drawn[6];
    for (uint32_t index = 0u; index < 6u; ++index) {
        before = shadow_maps(smoke);
        free(frame_pixels(smoke, &frame, NULL, NULL));
        drawn[index] = shadow_maps(smoke) - before;
    }
    printf("sun period 2, two sun maps: %lld %lld %lld %lld %lld %lld drawn a frame\n", (long long)drawn[0],
        (long long)drawn[1], (long long)drawn[2], (long long)drawn[3], (long long)drawn[4], (long long)drawn[5]);
    for (uint32_t index = 2u; index < 6u; ++index) {
        assert(drawn[index] == 1);
    }
    /* One view at period 2: its map every second frame. */
    Frame one = frame_default();
    one.chain = CHAIN_FOG;
    one.scene_visibility = no_step;
    int64_t one_total = 0;
    for (uint32_t index = 0u; index < 4u; ++index) {
        before = shadow_maps(smoke);
        free(frame_pixels(smoke, &one, NULL, NULL));
        one_total += shadow_maps(smoke) - before;
    }
    assert(one_total == 2);
    assert(btbh_gpu_window_scene_set_shadow_update_periods(smoke->window, 1u, 1u, 1u) == 1u);
    printf("shadows: shared by equal matrices (one map), own by different ones (two), taking turns at period 2\n");
}

/* ---- The report, fallbacks, prewarm ---- */

static void check_report_and_fallbacks(SmokeWindow *smoke) {
    static const uint8_t first_sees[BATCH_COUNT] = {1u, 1u, 1u, 1u};
    static const uint8_t second_sees[BATCH_COUNT] = {1u, 0u, 1u, 0u};
    Frame frame = frame_default();
    frame.chain = CHAIN_FOG;
    set_vertical_split(&frame);
    frame.visibility[0] = first_sees;
    frame.visibility[1] = second_sees;
    BTBHGPUSceneViewReport view_reports[2];
    BTBHGPUSceneReport report;
    free(frame_pixels(smoke, &frame, view_reports, &report));
    assert(view_reports[0].primitive_draw_count == 4u && view_reports[1].primitive_draw_count == 2u);
    assert(view_reports[0].instance_count == 10u && view_reports[1].instance_count == 3u);
    assert(report.primitive_draw_count == 6u);
    assert(view_reports[0].cpu_geometry_encode_milliseconds >= 0.0 &&
        view_reports[1].cpu_geometry_encode_milliseconds >= 0.0);
    assert(view_reports[0].cpu_opaque_order_milliseconds == 0.0);
    /* With the opaque sort, each view times its own. */
    frame.opaque_sort = BTBH_GPU_SCENE_OPAQUE_SORT_FRONT_TO_BACK;
    free(frame_pixels(smoke, &frame, view_reports, &report));
    assert(view_reports[0].primitive_draw_count == 4u && view_reports[1].primitive_draw_count == 2u);
    frame.opaque_sort = BTBH_GPU_SCENE_OPAQUE_SORT_SUBMITTED;
    /* A cut flag is accepted (it changes nothing without FSR 3 over views). */
    frame.flags[1] = BTBH_GPU_SCENE_VIEW_TEMPORAL_RESET;
    free(frame_pixels(smoke, &frame, NULL, NULL));
    frame.flags[1] = 0u;
    printf("report: view draws 4 and 2 (instances 10 and 3), the frame's 6\n");

    /* FSR 3 with two views runs FSR 1; one view after runs FSR 3 again. */
    frame.visibility[0] = NULL;
    frame.visibility[1] = NULL;
    frame.scene_visibility = no_step;
    frame.upscale_mode = BTBH_GPU_SCENE_UPSCALE_FSR3;
    frame.render_width = SMALL_WIDTH;
    frame.render_height = SMALL_HEIGHT;
    free(frame_pixels(smoke, &frame, NULL, &report));
    assert(report.upscale_mode == BTBH_GPU_SCENE_UPSCALE_FSR1);
    Frame one = frame;
    one.view_count = 1u;
    memset(one.clip, 0, sizeof(one.clip));
    free(frame_pixels(smoke, &one, NULL, &report));
#if !defined(__APPLE__)
    assert(report.upscale_mode == BTBH_GPU_SCENE_UPSCALE_FSR3);
#endif
    /* The radiance cascades with two views: off, the views' reason; one view after runs them again, with a new history
       (one view's frames before keep theirs). */
    frame.upscale_mode = BTBH_GPU_SCENE_UPSCALE_LINEAR;
    frame.render_width = WIDTH;
    frame.render_height = HEIGHT;
    frame.chain = CHAIN_CASCADES;
    one = frame;
    one.view_count = 1u;
    memset(one.clip, 0, sizeof(one.clip));
    free(frame_pixels(smoke, &one, NULL, &report));
    free(frame_pixels(smoke, &one, NULL, &report));
    assert(report.radiance_cascades.active_tier == BTBH_GPU_SCENE_RC_LOW);
    assert(report.radiance_cascades.pass_telemetry.temporal_history_valid == 1u);
    free(frame_pixels(smoke, &frame, NULL, &report));
    assert(report.radiance_cascades.active_tier == BTBH_GPU_SCENE_RC_OFF);
    assert(report.radiance_cascades.requested_tier == BTBH_GPU_SCENE_RC_LOW);
    assert(report.radiance_cascades.fallback_reason == BTBH_GPU_SCENE_RC_FALLBACK_VIEWS);
    free(frame_pixels(smoke, &one, NULL, &report));
    assert(report.radiance_cascades.active_tier == BTBH_GPU_SCENE_RC_LOW);
    assert(report.radiance_cascades.fallback_reason != BTBH_GPU_SCENE_RC_FALLBACK_VIEWS);
    assert(report.radiance_cascades.pass_telemetry.temporal_history_valid == 0u);
    free(frame_pixels(smoke, &one, NULL, &report));
    assert(report.radiance_cascades.pass_telemetry.temporal_history_valid == 1u);
    BTBHGPUSceneView views[2];
    memset(views, 0, sizeof(views));
    for (uint32_t index = 0u; index < 2u; ++index) {
        const SmokeCamera camera = camera_at(frame.angle[index]);
        memcpy(views[index].view_projection, camera.view_projection, sizeof(views[index].view_projection));
        memcpy(views[index].clip, frame.clip[index], sizeof(views[index].clip));
    }
    BTBHGPUSceneLighting lighting = lighting_from(0u, true, -1);
    BTBHGPUSceneGeometryBatch batch = cube_batch(smoke->cube, 0u, 1u);
    BTBHGPUSceneReport prewarm_report;
    /* A prewarm for two views with the cascades asked for draws nothing: the next one-view frame keeps their history. */
    BTBHGPUScenePost post = post_chain(CHAIN_CASCADES, BTBH_GPU_SCENE_UPSCALE_LINEAR);
    assert(btbh_gpu_window_scene_prewarm_views(smoke->window,
        shaders.paths[0], shaders.paths[1], shaders.paths[2], shaders.paths[3], shaders.paths[4], shaders.paths[5],
        shaders.paths[6], shaders.paths[7], shaders.paths[8], shaders.paths[9], shaders.paths[10],
        SMOKE_SHADER_FORMAT, WIDTH, HEIGHT, views, 2u, &lighting, &post, &batch, 1u, &prewarm_report) == BTBH_GPU_OK);
    free(frame_pixels(smoke, &one, NULL, &report));
    assert(report.radiance_cascades.active_tier == BTBH_GPU_SCENE_RC_LOW);
    assert(report.radiance_cascades.pass_telemetry.temporal_history_valid == 1u);
    /* The prewarm for two views says FSR 1. */
    post = post_chain(CHAIN_HBAO_SSR, BTBH_GPU_SCENE_UPSCALE_FSR3);
    const BTBHGPUResult prewarmed = btbh_gpu_window_scene_prewarm_views(smoke->window,
        shaders.paths[0], shaders.paths[1], shaders.paths[2], shaders.paths[3], shaders.paths[4], shaders.paths[5],
        shaders.paths[6], shaders.paths[7], shaders.paths[8], shaders.paths[9], shaders.paths[10],
        SMOKE_SHADER_FORMAT, SMALL_WIDTH, SMALL_HEIGHT, views, 2u, &lighting, &post, &batch, 1u, &prewarm_report);
    if (prewarmed != BTBH_GPU_OK) {
        fprintf(stderr, "prewarm views: %s\n", prewarm_report.message);
    }
    assert(prewarmed == BTBH_GPU_OK);
    assert(prewarm_report.upscale_mode == BTBH_GPU_SCENE_UPSCALE_FSR1);
    assert(btbh_gpu_window_scene_prewarm_views(smoke->window,
        shaders.paths[0], shaders.paths[1], shaders.paths[2], shaders.paths[3], shaders.paths[4], shaders.paths[5],
        shaders.paths[6], shaders.paths[7], shaders.paths[8], shaders.paths[9], shaders.paths[10],
        SMOKE_SHADER_FORMAT, SMALL_WIDTH, SMALL_HEIGHT, views, 3u, &lighting, &post, &batch, 1u, &prewarm_report) ==
        BTBH_GPU_INVALID_ARGUMENT);
    printf("fallbacks: FSR 3 runs FSR 1 and the cascades are off with two views, both back with one (the cascades' "
        "history new after a frame of two views, kept after a prewarm for them)\n");
}

/* ---- Refusals ---- */

static void expect_refused(SmokeWindow *smoke, const Frame *frame, const char *named, const char *what) {
    BTBHGPUSceneReport report;
    BTBHGPUSceneViewReport view_reports[2];
    const BTBHGPUResult result = present_frame(smoke, frame, view_reports, &report);
    if (result != BTBH_GPU_INVALID_ARGUMENT || (named != NULL && strstr(report.message, named) == NULL)) {
        fprintf(stderr, "%s: result %d, \"%s\"\n", what, (int)result, report.message);
    }
    assert(result == BTBH_GPU_INVALID_ARGUMENT);
    assert(named == NULL || strstr(report.message, named) != NULL);
    assert(!smoke->window->pending_scene_ready && smoke->window->pending_command == NULL);
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

/* A SPIR-V vertex shader as an older release built it: its ClipDistance capability turned into a second Shader one. */
static void strip_clip_distance(const char *path) {
    size_t size = 0u;
    uint8_t *bytes = (uint8_t *)SDL_LoadFile(path, &size);
    assert(bytes != NULL && size > 20u);
    bool stripped = false;
    for (size_t offset = 20u; offset + 8u <= size; offset += 4u) {
        uint32_t word = 0u;
        uint32_t operand = 0u;
        memcpy(&word, bytes + offset, 4u);
        memcpy(&operand, bytes + offset + 4u, 4u);
        if (word == ((2u << 16u) | 17u) && operand == 32u) {
            const uint32_t shader_capability = 1u;
            memcpy(bytes + offset + 4u, &shader_capability, 4u);
            stripped = true;
        }
    }
    assert(stripped);
    assert(SDL_SaveFile(path, bytes, size));
    SDL_free(bytes);
}

static void check_refusals(SmokeWindow *smoke) {
    Frame frame = frame_default();
    frame.scene_visibility = no_step;
    set_vertical_split(&frame);
    frame.use_override = true;
    frame.views_override = NULL;
    frame.view_count_override = 2u;
    expect_refused(smoke, &frame, "views", "views NULL");
    BTBHGPUSceneView views[3];
    memset(views, 0, sizeof(views));
    for (uint32_t index = 0u; index < 3u; ++index) {
        const SmokeCamera camera = camera_at(frame.angle[index % 2u]);
        memcpy(views[index].view_projection, camera.view_projection, sizeof(views[index].view_projection));
        memcpy(views[index].right, camera.right, sizeof(views[index].right));
        memcpy(views[index].up, camera.up, sizeof(views[index].up));
        memcpy(views[index].position, camera.position, sizeof(views[index].position));
        memcpy(views[index].clip, frame.clip[index % 2u], sizeof(views[index].clip));
    }
    frame.views_override = views;
    frame.view_count_override = 0u;
    expect_refused(smoke, &frame, NULL, "no views");
    frame.view_count_override = 3u;
    expect_refused(smoke, &frame, NULL, "three views");
    frame.view_count_override = 2u;
    BTBHGPUSceneView saved = views[1];
    views[1].view_projection[5] = NAN;
    expect_refused(smoke, &frame, "view 1", "a matrix not finite");
    views[1] = saved;
    views[1].position[2] = INFINITY;
    expect_refused(smoke, &frame, "view 1", "a position not finite");
    views[1] = saved;
    memset(views[1].view_projection, 0, sizeof(views[1].view_projection));
    expect_refused(smoke, &frame, "view 1", "a matrix not invertible");
    views[1] = saved;
    views[1].clip[2] = NAN;
    expect_refused(smoke, &frame, "view 1", "a clip not finite");
    views[1] = saved;
    views[0].clip[0] = 0.0f;
    views[0].clip[1] = 0.0f;
    expect_refused(smoke, &frame, "view 0", "two views without a clip line");
    /* One view: a clip without a line, (0, 0, c) with c not 0 (the whole frame or none of it), either sign. */
    frame.view_count_override = 1u;
    views[0].clip[2] = 5.0f;
    expect_refused(smoke, &frame, "view 0", "one view's clip without a line");
    views[0].clip[2] = -5.0f;
    expect_refused(smoke, &frame, "view 0", "one view's clip without a line, negative");
    frame.view_count_override = 2u;
    views[0].clip[0] = 1.0f;
    views[0].clip[2] = -0.5f * (float)WIDTH;
    static const uint8_t bad_bytes[BATCH_COUNT] = {1u, 2u, 1u, 0u};
    views[1].visibility = bad_bytes;
    expect_refused(smoke, &frame, "view 1", "a visibility byte above 1");
    views[1] = saved;
    views[1].shadow_visibility = bad_bytes;
    expect_refused(smoke, &frame, "view 1", "a shadow visibility byte above 1");
    views[1] = saved;
    views[1].flags = 2u;
    expect_refused(smoke, &frame, "view 1", "an unknown flag");
    views[1] = saved;
    views[1].reserved = 1u;
    expect_refused(smoke, &frame, "view 1", "reserved set");
    views[1] = saved;
    BTBHGPUSceneLighting bad_lighting = lighting_from(0u, true, -1);
    bad_lighting.ambient_red = NAN;
    views[1].lighting = &bad_lighting;
    expect_refused(smoke, &frame, "view 1", "an invalid lighting");
    views[1] = saved;
    /* A lifted floor (RFC 0012: it casts no shadow yet) kept out of the sun map by the call's byte, but let in by a
       view's shadow_visibility, which picks the casters of that view's own map: refused, naming the view's array. */
    const float flat_heights[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    BTBHGPUTextureReport field_report;
    BTBHGPUTexture *field = btbh_gpu_window_heightfield_create(smoke->window, flat_heights, 2u, 2u, &field_report);
    if (field == NULL) {
        fprintf(stderr, "heightfield: %s\n", field_report.message);
    }
    assert(field != NULL);
    BTBHGPUSceneHeightLift lift;
    memset(&lift, 0, sizeof(lift));
    lift.heightfield = field;
    lift.world_to_uv[0] = 1.0f / 60.0f;
    lift.world_to_uv[1] = 1.0f / 60.0f;
    lift.world_to_uv[2] = 0.5f;
    lift.world_to_uv[3] = 0.5f;
    static const uint8_t floor_out[BATCH_COUNT] = {0u, 1u, 1u, 1u};
    static const uint8_t floor_in[BATCH_COUNT] = {1u, 1u, 1u, 1u};
    frame.floor_lift = &lift;
    frame.directional_shadow_visibility = floor_out;
    views[0].shadow_visibility = floor_in;
    expect_refused(smoke, &frame, "view 0's shadow_visibility", "a lifted batch casting by view 0's bytes");
    views[0].shadow_visibility = floor_out;
    views[1].shadow_visibility = floor_in;
    expect_refused(smoke, &frame, "view 1's shadow_visibility", "a lifted batch casting by view 1's bytes");
    /* Kept out by every array: drawn. */
    views[1].shadow_visibility = floor_out;
    free(frame_pixels(smoke, &frame, NULL, NULL));
    views[0].shadow_visibility = NULL;
    views[1] = saved;
    frame.floor_lift = NULL;
    frame.directional_shadow_visibility = NULL;
    /* After them, the views draw. */
    free(frame_pixels(smoke, &frame, NULL, NULL));

#if !defined(__APPLE__)
    /* Old shaders: a copy of the shader directory whose static vertex shader writes no clip distance; then without
       production_post_view.vert. Two views are refused naming the file; one view draws. */
    ShaderCopy copy;
    memset(&copy, 0, sizeof(copy));
    (void)snprintf(copy.from, sizeof(copy.from), "%s", BTBH_GPU_SMOKE_SHADER_DIR);
    /* (A short name: the refusal names the file by its whole path in the report's 255 characters.) */
    (void)snprintf(copy.to, sizeof(copy.to), "%s/../views_old", BTBH_GPU_SMOKE_SHADER_DIR);
    (void)SDL_CreateDirectory(copy.to);
    assert(SDL_EnumerateDirectory(copy.from, copy_shader_file, &copy));
    assert(copy.copied > 11u);
    static ShaderSet old;
    shader_set_in(&old, copy.to);
    strip_clip_distance(old.paths[0]);
    frame.shaders = &old;
    expect_refused(smoke, &frame, "production_model.vert.spv", "a vertex shader without the clip distance");
    /* The window left as it was: a frame of two views with the cascades asked for, refused for its shaders, leaves the
       cascades' history to the next one-view frame. */
    Frame cascades = frame_default();
    cascades.chain = CHAIN_CASCADES;
    cascades.scene_visibility = no_step;
    BTBHGPUSceneReport cascades_report;
    free(frame_pixels(smoke, &cascades, NULL, &cascades_report));
    free(frame_pixels(smoke, &cascades, NULL, &cascades_report));
    assert(cascades_report.radiance_cascades.pass_telemetry.temporal_history_valid == 1u);
    frame.chain = CHAIN_CASCADES;
    expect_refused(smoke, &frame, "production_model.vert.spv", "old shaders with the cascades asked for");
    frame.chain = CHAIN_PLAIN;
    free(frame_pixels(smoke, &cascades, NULL, &cascades_report));
    assert(cascades_report.radiance_cascades.pass_telemetry.temporal_history_valid == 1u);
    Frame one = frame_default();
    one.shaders = &old;
    one.scene_visibility = no_step;
    free(frame_pixels(smoke, &one, NULL, NULL));
    /* (Another copy: the window keeps what it found of each shader path, so a file changed under a path it has read
       isn't looked at again.) */
    ShaderCopy fresh;
    memset(&fresh, 0, sizeof(fresh));
    (void)snprintf(fresh.from, sizeof(fresh.from), "%s", BTBH_GPU_SMOKE_SHADER_DIR);
    (void)snprintf(fresh.to, sizeof(fresh.to), "%s/../views_npv", BTBH_GPU_SMOKE_SHADER_DIR);
    (void)SDL_CreateDirectory(fresh.to);
    assert(SDL_EnumerateDirectory(fresh.from, copy_shader_file, &fresh));
    static ShaderSet no_post_view;
    shader_set_in(&no_post_view, fresh.to);
    char post_view[1300];
    (void)snprintf(post_view, sizeof(post_view), "%s/production_post_view.vert.spv", fresh.to);
    assert(SDL_RemovePath(post_view));
    frame.shaders = &no_post_view;
    expect_refused(smoke, &frame, "production_post_view.vert", "no production_post_view.vert");
    one.shaders = &no_post_view;
    free(frame_pixels(smoke, &one, NULL, NULL));
    frame.shaders = &shaders;
    free(frame_pixels(smoke, &frame, NULL, NULL));
#endif
    btbh_gpu_texture_destroy(field);
    printf("refusals: each with BTBH_GPU_INVALID_ARGUMENT, naming the view or the shader, no frame left waiting (the "
        "cascades' history kept)\n");
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
    SmokeWindow a = open_window(device, "BTBH scene views smoke A");
    SmokeWindow b = open_window(device, "BTBH scene views smoke B");

    check_one_view_identity(&a, &b);
    check_vertical_split(&b);
    check_taps_stay(&b);
    check_taps_by_the_line(&b);
    check_diagonal_split(&b);
    check_shadows(&b);
    check_report_and_fallbacks(&b);
    check_refusals(&a);

    SmokeWindow *windows[2] = {&a, &b};
    for (uint32_t index = 0u; index < 2u; ++index) {
        btbh_gpu_texture_destroy(windows[index]->frame);
        btbh_gpu_texture_destroy(windows[index]->small);
        btbh_gpu_custom_mesh_destroy(windows[index]->cube);
        btbh_gpu_window_destroy(windows[index]->window);
    }
    btbh_gpu_device_shutdown(device);
    btbh_gpu_device_destroy(device);
    printf("scene views smoke passed\n");
    return 0;
}
