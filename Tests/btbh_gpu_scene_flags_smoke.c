/* 0.9.0's opt-in flags, asked for by Be The Bullet Hell v2, rendered into a target and read back (hidden windows, a
 * debug device): BTBH_GPU_SCENE_BATCH_NO_RC_SOURCE, BTBH_GPU_SCENE_POST_ADDITIVE_CASCADES_KEEP_HBAO and
 * BTBH_GPU_SCENE_POST_SSR_SURFACE_REFLECTIVITY.
 *
 * Unset. This part also builds against 0.8.0's header, which has none of them: there it only renders (and writes)
 * the gallery, so that a build of 0.8.0 and a build of this one can be compared.
 * - A gallery of a small street (a glossy dielectric ground, a building, a glass pane, a grey and a gold metal box,
 *   an unlit receiver, an emissive box and an unlit source, and glowing boxes outside the view) through what the
 *   flags touch: the horizon-based ambient occlusion without the cascades, with them and with additive ones (the
 *   sun's shadow, the omni shadow, no shadow; its contact mode), the SSAO under additive cascades, the screen-space
 *   reflections without the cascades, with them and with additive ones and the HBAO, and the cascades' material
 *   sources (on and additive). Each is rendered twice and must come out the same.
 * - BTBH_SCENE_FLAGS_DUMP=<dir> writes the gallery (unset-<name>.png); BTBH_SCENE_FLAGS_REFERENCE=<dir> compares
 *   each image with that directory's and requires 0 differing pixels.
 *
 * Set (0.9.0):
 * - NO_RC_SOURCE: an emissive box and an unlit source outside the view (inside the cascades' field), on and
 *   additive, light the ground in view; flagged, they render byte for byte as the same scene with the box's emission
 *   at 0 and the source a receiver (no source either), and so does the scene with every other batch flagged too
 *   (nothing else changes: the occlusion is the host's, the shadows as before). In view, a flagged glow differs
 *   from the same scene without its emission only on its own pixels (it still glows), and from the unflagged one
 *   (its bounce light is gone). Without the cascades the flag changes nothing.
 * - ADDITIVE_CASCADES_KEEP_HBAO: with nothing glowing (the unlit source flagged NO_RC_SOURCE), additive cascades and
 *   the HBAO render byte for byte as no cascades, through the sun's, the omni and no shadow; without the flag they
 *   are darker (printed). With glowing boxes and an HBAO that darkens nothing, the bounce light is the same with the
 *   flag as without (metalness 0 or 1). With the reflections, additive cascades, the HBAO and the flag render as no
 *   cascades with SSR_SURFACE_REFLECTIVITY. Ignored where it doesn't apply: cascades not additive, ssao_contact,
 *   the SSAO, no cascades.
 * - SSR_SURFACE_REFLECTIVITY: without the cascades the reflections (which reflect nothing there unset) appear; on
 *   the dielectric ground, the glass and the grey metal they are byte for byte the additive cascades' with nothing
 *   glowing (which read the F0 from their targets), through the three shadow variants; a gold metal reflects
 *   untinted (differs, printed). With the cascades, or without the reflections, it changes nothing.
 * - Refusals: an unknown batch flag, unknown post flags (present and prewarm).
 * - BTBH_SMOKE_BENCH=1 (or =ssr, =hbao): the flags' cost at 1920x1080, in interleaved turns (prints only):
 *   the reflections without the cascades unset (which return before they march), with the flag and off; the HBAO
 *   and the reflections under additive cascades with KEEP_HBAO and without, and without the cascades.
 *   BTBH_SMOKE_BENCH_ROUNDS turns (10, up to 40); BTBH_SCENE_FLAGS_BASELINE_SHADER_DIR=<dir> (0.8.0's shaders) adds
 *   the unset configuration in a window whose pipelines are made from those.
 *
 * It needs a GPU and the production shaders; with no device it is skipped (77). */
#undef NDEBUG

#include "btbh_gpu/btbh_gpu.h"
#include "btbh_gpu_smoke_meshes.h"
#include "btbh_gpu_internal.h"
#include "btbh_gpu_package_internal.h"

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_surface.h>
#include <SDL3/SDL_timer.h>

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SMOKE_SKIP 77
#define TARGET_SIZE 160u
#define TEXELS (TARGET_SIZE * TARGET_SIZE)
#define MAX_BOXES 16u

#if defined(BTBH_GPU_SCENE_HAS_BATCH_NO_RC_SOURCE) && defined(BTBH_GPU_SCENE_HAS_ADDITIVE_CASCADES_KEEP_HBAO) && \
    defined(BTBH_GPU_SCENE_HAS_SSR_SURFACE_REFLECTIVITY)
#define SMOKE_HAS_FLAGS 1
#endif

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
    BTBHGPUTexture *target;   /* TARGET_SIZE^2 */
    BTBHGPUTexture *glow;     /* an emission texture */
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

static void look_at_matrix(const float eye[3], const float centre[3], float out_view[16], float side[3], float up[3]) {
    float forward[3] = { centre[0] - eye[0], centre[1] - eye[1], centre[2] - eye[2] };
    normalize3(forward);
    const float world_up[3] = { 0.0f, 1.0f, 0.0f };
    cross3(forward, world_up, side);
    normalize3(side);
    cross3(side, forward, up);
    const float view[16] = {
        side[0], up[0], -forward[0], 0.0f,
        side[1], up[1], -forward[1], 0.0f,
        side[2], up[2], -forward[2], 0.0f,
        -dot3(eye, side), -dot3(eye, up), dot3(eye, forward), 1.0f,
    };
    memcpy(out_view, view, sizeof(view));
}

static SmokeCamera look_at(const float eye[3], const float centre[3], float fov_y, float aspect) {
    SmokeCamera camera;
    memset(&camera, 0, sizeof(camera));
    const float near_plane = 0.1f;
    const float far_plane = 60.0f;
    float view[16];
    look_at_matrix(eye, centre, view, camera.right, camera.up);
    const float h = 1.0f / tanf(0.5f * fov_y);
    const float r = far_plane / (near_plane - far_plane);
    const float projection[16] = {
        h / aspect, 0.0f, 0.0f, 0.0f,
        0.0f, h, 0.0f, 0.0f,
        0.0f, 0.0f, r, -1.0f,
        0.0f, 0.0f, r * near_plane, 0.0f,
    };
    multiply(projection, view, camera.view_projection);
    memcpy(camera.position, eye, sizeof(camera.position));
    return camera;
}

typedef struct Box {
    float centre[3];
    float scale[3];
    float colour[4];
} Box;

static void box_instance(const Box *box, float out[24]) {
    memset(out, 0, 24u * sizeof(float));
    out[0] = box->scale[0];
    out[5] = box->scale[1];
    out[10] = box->scale[2];
    out[12] = box->centre[0];
    out[13] = box->centre[1];
    out[14] = box->centre[2];
    out[15] = 1.0f;
    memcpy(&out[16], box->colour, 4u * sizeof(float));
}

static Box make_box(float x, float y, float z, float sx, float sy, float sz, const float colour[4]) {
    Box box;
    memset(&box, 0, sizeof(box));
    box.centre[0] = x;
    box.centre[1] = y;
    box.centre[2] = z;
    box.scale[0] = sx;
    box.scale[1] = sy;
    box.scale[2] = sz;
    memcpy(box.colour, colour, sizeof(box.colour));
    return box;
}

static BTBHGPUSceneGeometryBatch cube_batch(BTBHGPUCustomMesh *cube) {
    BTBHGPUSceneGeometryBatch batch;
    memset(&batch, 0, sizeof(batch));
    batch.kind = BTBH_GPU_SCENE_GEOMETRY_CUSTOM_MESH;
    batch.custom_mesh = cube;
    batch.material_slot = -1;
    batch.instance_count = 1u;
    batch.albedo_red = 255u;
    batch.albedo_green = 255u;
    batch.albedo_blue = 255u;
    batch.albedo_alpha = 255u;
    batch.uv_scale_x = 1.0f;
    batch.uv_scale_y = 1.0f;
    batch.normal_scale = 1.0f;
    batch.roughness = 0.6f;
    batch.occlusion = 1.0f;
    batch.depth_compare = BTBH_GPU_SCENE_DEPTH_LESS;
    batch.cull_mode = BTBH_GPU_SCENE_CULL_BACK;
    batch.stencil_pass = BTBH_GPU_SCENE_STENCIL_REPLACE;
    batch.spot_shadow_visible = 1u;
    return batch;
}

/* A unit cube (-0.5..0.5), 24 floats a vertex, wound so the renderer takes its outsides for front faces. */
static BTBHGPUCustomMesh *create_cube(BTBHGPUWindow *window) {
    static const float faces[6][3] = {
        { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 },
    };
    float vertices[24][24];
    uint32_t indices[36];
    memset(vertices, 0, sizeof(vertices));
    for (uint32_t face = 0u; face < 6u; ++face) {
        const float *n = faces[face];
        float u[3] = { n[1], n[2], n[0] };
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
        const uint32_t reversed[6] = { base, base + 2u, base + 1u, base, base + 3u, base + 2u };
        memcpy(&indices[face * 6u], reversed, sizeof(reversed));
    }
    BTBHGPUCustomMeshReport report;
    memset(&report, 0, sizeof(report));
    BTBHGPUCustomMesh *mesh = smoke_custom_mesh_create(
        window, vertices, (uint32_t)sizeof(vertices), 24u, indices, 36u, &report);
    if (mesh == NULL) {
        fprintf(stderr, "cube: %s\n", report.message);
    }
    assert(mesh != NULL);
    return mesh;
}

static BTBHGPUTexture *create_glow(BTBHGPUWindow *window) {
    enum { SIZE = 32 };
    static uint8_t pixels[SIZE * SIZE * 4];
    for (uint32_t y = 0u; y < SIZE; ++y) {
        for (uint32_t x = 0u; x < SIZE; ++x) {
            const bool lit = (x / 4u) % 2u == 0u || (y / 4u) % 3u == 0u;
            uint8_t *texel = &pixels[(y * SIZE + x) * 4u];
            texel[0] = lit ? 255u : 40u;
            texel[1] = lit ? 190u : 20u;
            texel[2] = lit ? 110u : 10u;
            texel[3] = 255u;
        }
    }
    BTBHGPUTextureReport report;
    BTBHGPUTexture *texture = btbh_gpu_window_texture_create_rgba8_ex(window, pixels, SIZE, SIZE, 1u,
        BTBH_GPU_TEXTURE_FILTER_TRILINEAR, BTBH_GPU_TEXTURE_WRAP_REPEAT, BTBH_GPU_TEXTURE_COLOR_SPACE_SRGB, &report);
    if (texture == NULL) {
        fprintf(stderr, "glow: %s\n", report.message);
    }
    assert(texture != NULL);
    return texture;
}

/* The texture's bytes, RGBA (4 a texel), read back through SDL_GPU. */
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

/* Pixels whose largest channel difference is above 0, and the largest difference. */
static uint32_t differing_pixels(const uint8_t *a, const uint8_t *b, int *largest) {
    uint32_t count = 0u;
    int worst = 0;
    for (uint32_t index = 0u; index < TEXELS; ++index) {
        int difference = 0;
        for (uint32_t channel = 0u; channel < 4u; ++channel) {
            const int d = abs((int)a[index * 4u + channel] - (int)b[index * 4u + channel]);
            difference = d > difference ? d : difference;
        }
        worst = difference > worst ? difference : worst;
        count += difference > 0 ? 1u : 0u;
    }
    if (largest != NULL) {
        *largest = worst;
    }
    return count;
}

/* --------------------------------------------------------------------------------------------- the scenes */

typedef enum Shadows { SHADOWS_NONE = 0, SHADOWS_SUN = 1, SHADOWS_OMNI = 2 } Shadows;
typedef enum Cascades { CASCADES_OFF = 0, CASCADES_ON = 1, CASCADES_ADDITIVE = 2 } Cascades;
typedef enum Occlusion {
    OCCLUSION_NONE = 0,
    OCCLUSION_SSAO = 1,
    OCCLUSION_HBAO = 2,
    OCCLUSION_HBAO_CONTACT = 3,
    /* The HBAO's passes, with a bias no point stands above: it darkens nothing. */
    OCCLUSION_HBAO_NONE = 4
} Occlusion;

typedef struct Options {
    const char *name;
    Shadows shadows;
    Cascades cascades;
    Occlusion occlusion;
    bool reflections;
    bool glow;            /* an emissive box and an unlit source, in view */
    bool far_glow;        /* an emissive box and an unlit source outside the view, inside the cascades' field */
    bool gold;            /* a gold metal box among the reflecting surfaces */
    bool unlit_box;       /* an unlit box (a source, unless flagged) */
    bool no_metal;        /* without the grey metal box */
} Options;

typedef struct Scene {
    const char *name;
    SmokeCamera camera;
    BTBHGPUSceneLighting lighting;
    BTBHGPUScenePost post;
    Box boxes[MAX_BOXES];
    float instances[MAX_BOXES][24];
    BTBHGPUSceneGeometryBatch batches[MAX_BOXES];
    uint32_t count;
    /* Where the glowing batches are (MAX_BOXES: none). */
    uint32_t glow_box;
    uint32_t source_box;
    uint32_t far_glow_box;
    uint32_t far_source_box;
    uint32_t unlit_box;
    uint32_t metal_boxes[2];
    uint32_t metal_box_count;
} Scene;

static const float grey[4] = { 0.55f, 0.55f, 0.52f, 1.0f };
static const float stone[4] = { 0.7f, 0.68f, 0.62f, 1.0f };
static const float glass[4] = { 0.75f, 0.8f, 0.85f, 1.0f };
static const float steel[4] = { 0.75f, 0.75f, 0.75f, 1.0f };
static const float gold[4] = { 1.0f, 0.72f, 0.3f, 1.0f };
static const float slate[4] = { 0.45f, 0.55f, 0.7f, 1.0f };
static const float warm[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
static const float hot[4] = { 1.0f, 0.45f, 0.15f, 1.0f };

static uint32_t scene_add(Scene *scene, Box box, BTBHGPUSceneGeometryBatch batch) {
    assert(scene->count < MAX_BOXES);
    const uint32_t index = scene->count++;
    scene->boxes[index] = box;
    box_instance(&box, scene->instances[index]);
    batch.first_instance = index;
    scene->batches[index] = batch;
    return index;
}

static void build_scene(const SmokeWindow *smoke, const Options *options, Scene *scene) {
    memset(scene, 0, sizeof(*scene));
    scene->name = options->name;
    scene->glow_box = scene->source_box = scene->far_glow_box = scene->far_source_box = scene->unlit_box =
        MAX_BOXES;
    const float eye[3] = { 0.0f, 5.5f, 6.5f };
    const float centre[3] = { 0.0f, 0.2f, -0.5f };
    scene->camera = look_at(eye, centre, 0.85f, 1.0f);

    BTBHGPUSceneLighting *lighting = &scene->lighting;
    lighting->ambient_red = 0.8f;
    lighting->ambient_green = 0.85f;
    lighting->ambient_blue = 1.0f;
    lighting->ambient_energy = 0.45f;
    const float sun[3] = { 4.0f, 10.0f, 3.0f };
    lighting->directional_x = -sun[0];
    lighting->directional_y = -sun[1];
    lighting->directional_z = -sun[2];
    lighting->directional_energy = 1.4f;
    lighting->directional_red = 1.0f;
    lighting->directional_green = 0.95f;
    lighting->directional_blue = 0.9f;
    lighting->spot_range = 8.0f;
    lighting->spot_falloff = 1.0f;
    lighting->spot_inner_cutoff = 1.0f;
    lighting->spot_outer_cutoff = 0.0f;
    if (options->shadows != SHADOWS_NONE) {
        float view[16];
        float side[3];
        float up[3];
        const float origin[3] = { 0.0f, 0.0f, 0.0f };
        look_at_matrix(sun, origin, view, side, up);
        const float rcp = 1.0f / 10.0f;
        const float range = 1.0f / (1.0f - 30.0f);
        const float orthographic[16] = {
            rcp, 0.0f, 0.0f, 0.0f,
            0.0f, rcp, 0.0f, 0.0f,
            0.0f, 0.0f, range, 0.0f,
            0.0f, 0.0f, range * 1.0f, 1.0f,
        };
        multiply(orthographic, view, lighting->directional_shadow_view_projection);
        lighting->directional_shadow_enabled = 1u;
        lighting->directional_shadow_softness = 1.0f;
    }
    if (options->shadows == SHADOWS_OMNI) {
        lighting->spot_position_x = -0.5f;
        lighting->spot_position_y = 2.6f;
        lighting->spot_position_z = 1.5f;
        lighting->spot_range = 8.0f;
        lighting->spot_direction_y = -1.0f;
        lighting->spot_energy = 2.0f;
        lighting->spot_red = 1.0f;
        lighting->spot_green = 0.7f;
        lighting->spot_blue = 0.4f;
        lighting->spot_inner_cutoff = 0.2f;
        lighting->spot_outer_cutoff = -1.0f;
        lighting->spot_shadow_enabled = 1u;
        lighting->spot_shadow_is_omni = 1u;
        lighting->spot_shadow_softness = 1.0f;
        lighting->spot_shadow_near = 0.1f;
    }

    BTBHGPUScenePost *post = &scene->post;
    /* Reinhard with a white point of 1: the target holds sRGB(linear colour), dithered. */
    post->tonemap_white = 1.0f;
    post->exposure = 1.0f;
    post->contrast = 1.0f;
    post->saturation = 1.0f;
    post->blur_curve = 1.0f;
    post->tonemap_mode = BTBH_GPU_SCENE_TONEMAP_REINHARD;
    post->upscale_mode = BTBH_GPU_SCENE_UPSCALE_LINEAR;
    switch (options->occlusion) {
        case OCCLUSION_SSAO:
            post->ssao_enabled = 1u;
            break;
        case OCCLUSION_HBAO:
        case OCCLUSION_HBAO_CONTACT:
        case OCCLUSION_HBAO_NONE:
            post->ssao_enabled = 2u;
            post->ssao_radius = 1.2f;
            post->ssao_intensity = 1.5f;
            post->ssao_power = 1.0f;
            post->ssao_bias = options->occlusion == OCCLUSION_HBAO_NONE ? 1000.0f : 0.03f;
            post->ssao_contact = options->occlusion == OCCLUSION_HBAO_CONTACT ? 1u : 0u;
            break;
        default:
            break;
    }
    if (options->reflections) {
        post->ssr_intensity = 1.0f;
        post->ssr_max_roughness = 0.5f;
        post->ssr_max_distance = 8.0f;
        post->ssr_thickness = 0.5f;
    }
    if (options->cascades != CASCADES_OFF) {
        post->radiance_cascades.tier = BTBH_GPU_SCENE_RC_LOW;
        post->radiance_cascades.auto_frame_budget_milliseconds = 16.0f;
        post->radiance_cascades.indirect_intensity = 1.0f;
        post->radiance_cascades.hero_omni_emitter_index = -1;
        post->radiance_cascades.invalidate = 1u;
        post->radiance_cascades.additive = options->cascades == CASCADES_ADDITIVE ? 1u : 0u;
    }

    /* The ground: a glossy dielectric. */
    BTBHGPUSceneGeometryBatch batch = cube_batch(smoke->cube);
    batch.roughness = 0.12f;
    (void)scene_add(scene, make_box(0.0f, -0.25f, 0.0f, 16.0f, 0.5f, 16.0f, grey), batch);
    /* A building. */
    batch = cube_batch(smoke->cube);
    batch.roughness = 0.7f;
    (void)scene_add(scene, make_box(-2.2f, 1.0f, -1.6f, 1.4f, 2.0f, 1.4f, stone), batch);
    /* A glass pane (detail pattern 5: F0 0.12), by day. */
    batch = cube_batch(smoke->cube);
    batch.roughness = 0.05f;
    batch.detail_parameters[2] = 3.0f;
    batch.detail_parameters[3] = 5.0f;
    (void)scene_add(scene, make_box(1.7f, 0.9f, -1.9f, 1.4f, 1.8f, 0.3f, glass), batch);
    /* A grey metal box. */
    if (!options->no_metal) {
        batch = cube_batch(smoke->cube);
        batch.metalness = 1.0f;
        batch.roughness = 0.1f;
        scene->metal_boxes[scene->metal_box_count++] =
            scene_add(scene, make_box(-0.5f, 0.5f, -0.6f, 1.0f, 1.0f, 1.0f, steel), batch);
    }
    /* An unlit receiver. */
    batch = cube_batch(smoke->cube);
    batch.unlit_enabled = 1u;
    batch.rc_receiver = 1u;
    (void)scene_add(scene, make_box(2.4f, 0.3f, 0.9f, 0.6f, 0.6f, 0.6f, slate), batch);
    if (options->gold) {
        batch = cube_batch(smoke->cube);
        batch.metalness = 1.0f;
        batch.roughness = 0.1f;
        scene->metal_boxes[scene->metal_box_count++] =
            scene_add(scene, make_box(0.9f, 0.45f, 0.3f, 0.9f, 0.9f, 0.9f, gold), batch);
    }
    if (options->unlit_box) {
        batch = cube_batch(smoke->cube);
        batch.unlit_enabled = 1u;
        scene->unlit_box = scene_add(scene, make_box(-2.6f, 0.25f, 1.6f, 0.5f, 0.5f, 0.5f, hot), batch);
    }
    if (options->glow) {
        batch = cube_batch(smoke->cube);
        batch.emission_texture = smoke->glow;
        batch.emission_red = 255u;
        batch.emission_green = 255u;
        batch.emission_blue = 255u;
        batch.emission_energy = 4.0f;
        scene->glow_box = scene_add(scene, make_box(-1.6f, 0.35f, 1.2f, 0.7f, 0.7f, 0.7f, stone), batch);
        batch = cube_batch(smoke->cube);
        batch.unlit_enabled = 1u;
        scene->source_box = scene_add(scene, make_box(0.3f, 0.25f, 2.0f, 0.5f, 0.5f, 0.5f, hot), batch);
    }
    if (options->far_glow) {
        /* Outside the view (the camera looks 37 degrees down; these lie 55 degrees off its axis), within three
           render units of the ground in view. */
        batch = cube_batch(smoke->cube);
        batch.emission_red = 255u;
        batch.emission_green = 200u;
        batch.emission_blue = 140u;
        batch.emission_energy = 10.0f;
        scene->far_glow_box = scene_add(scene, make_box(0.0f, 0.75f, 6.8f, 1.5f, 1.5f, 1.5f, warm), batch);
        batch = cube_batch(smoke->cube);
        batch.unlit_enabled = 1u;
        scene->far_source_box = scene_add(scene, make_box(2.5f, 0.5f, 6.5f, 1.0f, 1.0f, 1.0f, hot), batch);
    }
}

static uint8_t *render_scene(SmokeWindow *smoke, const Scene *scene, BTBHGPUResult expected, BTBHGPUSceneReport *report) {
    const BTBHGPUResult result = btbh_gpu_window_scene_render_to_target(
        smoke->window,
        smoke->target,
        shader_paths[0], shader_paths[1], shader_paths[2], shader_paths[3],
        shader_paths[4], shader_paths[5], shader_paths[6], shader_paths[7],
        shader_paths[8], shader_paths[9], shader_paths[10],
        SMOKE_SHADER_FORMAT,
        TARGET_SIZE,
        TARGET_SIZE,
        scene->camera.view_projection,
        scene->camera.right,
        scene->camera.up,
        scene->camera.position,
        &scene->lighting,
        &scene->post,
        &scene->instances[0][0],
        scene->count,
        scene->count,
        scene->batches,
        NULL,
        NULL,
        scene->count,
        12u, 14u, 20u, 255u,
        report
    );
    if (result != expected) {
        fprintf(stderr, "%s: %d (expected %d): %s\n", scene->name, (int)result, (int)expected, report->message);
    }
    assert(result == expected);
    if (result != BTBH_GPU_OK) {
        return NULL;
    }
    if (scene->post.radiance_cascades.tier != BTBH_GPU_SCENE_RC_OFF) {
        /* (Every comparison here needs the cascades to run.) */
        if (report->radiance_cascades.active_tier == BTBH_GPU_SCENE_RC_OFF) {
            fprintf(stderr, "%s: the cascades didn't run (fallback %d)\n", scene->name,
                (int)report->radiance_cascades.fallback_reason);
        }
        assert(report->radiance_cascades.active_tier != BTBH_GPU_SCENE_RC_OFF);
    }
    return read_texture(smoke->window, smoke->target);
}

static uint8_t *render(SmokeWindow *smoke, const Scene *scene) {
    BTBHGPUSceneReport report;
    return render_scene(smoke, scene, BTBH_GPU_OK, &report);
}

/* ------------------------------------------------------------------------------------------- the gallery */

static const Options gallery[] = {
    { .name = "hbao-sun", .shadows = SHADOWS_SUN, .occlusion = OCCLUSION_HBAO, .glow = true, .unlit_box = true },
    { .name = "hbao-rc-sun", .shadows = SHADOWS_SUN, .cascades = CASCADES_ON, .occlusion = OCCLUSION_HBAO,
      .glow = true, .unlit_box = true },
    { .name = "hbao-additive-sun", .shadows = SHADOWS_SUN, .cascades = CASCADES_ADDITIVE,
      .occlusion = OCCLUSION_HBAO, .glow = true, .unlit_box = true },
    { .name = "hbao-additive-omni", .shadows = SHADOWS_OMNI, .cascades = CASCADES_ADDITIVE,
      .occlusion = OCCLUSION_HBAO, .glow = true, .unlit_box = true },
    { .name = "hbao-additive-unshadowed", .shadows = SHADOWS_NONE, .cascades = CASCADES_ADDITIVE,
      .occlusion = OCCLUSION_HBAO, .glow = true, .unlit_box = true },
    { .name = "hbao-contact-additive", .shadows = SHADOWS_SUN, .cascades = CASCADES_ADDITIVE,
      .occlusion = OCCLUSION_HBAO_CONTACT, .glow = true },
    { .name = "ssao-additive", .shadows = SHADOWS_SUN, .cascades = CASCADES_ADDITIVE, .occlusion = OCCLUSION_SSAO,
      .glow = true },
    { .name = "ssr-sun", .shadows = SHADOWS_SUN, .reflections = true, .gold = true },
    { .name = "ssr-hbao-unshadowed", .shadows = SHADOWS_NONE, .occlusion = OCCLUSION_HBAO, .reflections = true,
      .gold = true },
    { .name = "ssr-rc", .shadows = SHADOWS_SUN, .cascades = CASCADES_ON, .reflections = true, .gold = true,
      .glow = true },
    { .name = "ssr-hbao-additive", .shadows = SHADOWS_SUN, .cascades = CASCADES_ADDITIVE,
      .occlusion = OCCLUSION_HBAO, .reflections = true, .gold = true, .glow = true },
    { .name = "sources-rc", .shadows = SHADOWS_SUN, .cascades = CASCADES_ON, .glow = true, .far_glow = true,
      .unlit_box = true },
    { .name = "sources-additive", .shadows = SHADOWS_OMNI, .cascades = CASCADES_ADDITIVE, .glow = true,
      .far_glow = true, .unlit_box = true },
};
#define GALLERY_COUNT (sizeof(gallery) / sizeof(gallery[0]))

static bool load_png(const char *path, uint8_t *pixels) {
    SDL_Surface *surface = SDL_LoadPNG(path);
    if (surface == NULL) {
        return false;
    }
    SDL_Surface *rgba = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_ABGR8888);
    SDL_DestroySurface(surface);
    if (rgba == NULL || (uint32_t)rgba->w != TARGET_SIZE || (uint32_t)rgba->h != TARGET_SIZE) {
        SDL_DestroySurface(rgba);
        return false;
    }
    for (uint32_t y = 0u; y < TARGET_SIZE; ++y) {
        memcpy(&pixels[(size_t)y * TARGET_SIZE * 4u], (const uint8_t *)rgba->pixels + (size_t)y * (size_t)rgba->pitch,
            (size_t)TARGET_SIZE * 4u);
    }
    SDL_DestroySurface(rgba);
    return true;
}

static void check_gallery(SmokeWindow *smoke) {
    const char *dump = SDL_getenv("BTBH_SCENE_FLAGS_DUMP");
    const char *reference_dir = SDL_getenv("BTBH_SCENE_FLAGS_REFERENCE");
    static Scene scene;
    uint32_t compared = 0u;
    for (uint32_t index = 0u; index < GALLERY_COUNT; ++index) {
        build_scene(smoke, &gallery[index], &scene);
        uint8_t *image = render(smoke, &scene);
        uint8_t *again = render(smoke, &scene);
        assert(memcmp(image, again, (size_t)TEXELS * 4u) == 0);
        free(again);
        printf("gallery %-26s rendered", scene.name);
        if (reference_dir != NULL && reference_dir[0] != '\0') {
            char path[1100];
            (void)snprintf(path, sizeof(path), "%s/unset-%s.png", reference_dir, scene.name);
            uint8_t *reference = malloc((size_t)TEXELS * 4u);
            assert(reference != NULL);
            const bool loaded = load_png(path, reference);
            if (!loaded) {
                fprintf(stderr, "\nno reference %s\n", path);
            }
            assert(loaded);
            const uint32_t differing = differing_pixels(image, reference, NULL);
            printf("; reference: %u pixels differ", differing);
            assert(differing == 0u);
            compared += 1u;
            free(reference);
        }
        printf("\n");
        if (dump != NULL && dump[0] != '\0') {
            char path[1100];
            (void)snprintf(path, sizeof(path), "%s/unset-%s.png", dump, scene.name);
            assert(btbh_gpu_write_png_rgba8(path, TARGET_SIZE, TARGET_SIZE, image) == BTBH_GPU_OK);
        }
        free(image);
    }
    if (compared > 0u) {
        printf("gallery: %u images (%u pixels) match the references\n", compared, compared * TEXELS);
    }
    (void)fflush(stdout);
}

#if defined(SMOKE_HAS_FLAGS)
/* ------------------------------------------------------------------------------------------ set: the flags */

/* The mean signed difference of b from a over the rgb channels (sRGB levels), and the share of pixels darker by more
   than four levels (in their mean channel): Be The Bullet Hell v2's measures. */
static void print_darkening(const char *what, const uint8_t *a, const uint8_t *b) {
    double sum = 0.0;
    uint32_t darker = 0u;
    for (uint32_t index = 0u; index < TEXELS; ++index) {
        double pixel = 0.0;
        for (uint32_t channel = 0u; channel < 3u; ++channel) {
            pixel += (double)b[index * 4u + channel] - (double)a[index * 4u + channel];
        }
        sum += pixel;
        darker += pixel / 3.0 < -4.0 ? 1u : 0u;
    }
    printf("%s: %+.2f sRGB levels on average, %.1f %% of the pixels darker by more than 4\n", what,
        sum / (3.0 * TEXELS), 100.0 * (double)darker / (double)TEXELS);
}

static void dump_image(const char *name, const uint8_t *pixels) {
    const char *dump = SDL_getenv("BTBH_SCENE_FLAGS_DUMP");
    if (dump != NULL && dump[0] != '\0') {
        char path[1100];
        (void)snprintf(path, sizeof(path), "%s/set-%s.png", dump, name);
        assert(btbh_gpu_write_png_rgba8(path, TARGET_SIZE, TARGET_SIZE, pixels) == BTBH_GPU_OK);
    }
}

static void expect_same(const char *what, const uint8_t *a, const uint8_t *b) {
    int largest = 0;
    const uint32_t differing = differing_pixels(a, b, &largest);
    printf("%s: %u pixels differ\n", what, differing);
    if (differing != 0u) {
        fprintf(stderr, "%s: %u pixels differ (largest %d)\n", what, differing, largest);
    }
    assert(differing == 0u);
}

static uint32_t expect_different(const char *what, const uint8_t *a, const uint8_t *b) {
    int largest = 0;
    const uint32_t differing = differing_pixels(a, b, &largest);
    printf("%s: %u pixels differ (largest %d levels)\n", what, differing, largest);
    assert(differing > 0u);
    return differing;
}

/* The box's pixels on the screen, as a rectangle (x0, y0, x1, y1), a pixel wider each way. */
static void box_rectangle(const Scene *scene, uint32_t box_index, int rectangle[4]) {
    const Box *box = &scene->boxes[box_index];
    float minimum[2] = { 1.0e9f, 1.0e9f };
    float maximum[2] = { -1.0e9f, -1.0e9f };
    for (uint32_t corner = 0u; corner < 8u; ++corner) {
        const float p[4] = {
            box->centre[0] + ((corner & 1u) ? 0.5f : -0.5f) * box->scale[0],
            box->centre[1] + ((corner & 2u) ? 0.5f : -0.5f) * box->scale[1],
            box->centre[2] + ((corner & 4u) ? 0.5f : -0.5f) * box->scale[2],
            1.0f,
        };
        float clip[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        for (int row = 0; row < 4; ++row) {
            for (int k = 0; k < 4; ++k) {
                clip[row] += scene->camera.view_projection[k * 4 + row] * p[k];
            }
        }
        const float x = (clip[0] / clip[3] * 0.5f + 0.5f) * (float)TARGET_SIZE;
        const float y = (0.5f - clip[1] / clip[3] * 0.5f) * (float)TARGET_SIZE;
        minimum[0] = fminf(minimum[0], x);
        minimum[1] = fminf(minimum[1], y);
        maximum[0] = fmaxf(maximum[0], x);
        maximum[1] = fmaxf(maximum[1], y);
    }
    rectangle[0] = (int)floorf(minimum[0]) - 1;
    rectangle[1] = (int)floorf(minimum[1]) - 1;
    rectangle[2] = (int)ceilf(maximum[0]) + 1;
    rectangle[3] = (int)ceilf(maximum[1]) + 1;
}

/* NO_RC_SOURCE. */
static void check_no_rc_source(SmokeWindow *smoke) {
    static Scene scene;
    const Cascades kinds[2] = { CASCADES_ON, CASCADES_ADDITIVE };
    const Shadows shadows[2] = { SHADOWS_SUN, SHADOWS_OMNI };
    for (uint32_t kind = 0u; kind < 2u; ++kind) {
        const char *label = kinds[kind] == CASCADES_ON ? "cascades" : "additive cascades";
        const Options far = { .name = "far-glow", .shadows = shadows[kind], .cascades = kinds[kind],
            .far_glow = true };
        build_scene(smoke, &far, &scene);
        uint8_t *glowing = render(smoke, &scene);
        scene.batches[scene.far_glow_box].flags = BTBH_GPU_SCENE_BATCH_NO_RC_SOURCE;
        scene.batches[scene.far_source_box].flags = BTBH_GPU_SCENE_BATCH_NO_RC_SOURCE;
        uint8_t *flagged = render(smoke, &scene);
        for (uint32_t index = 0u; index < scene.count; ++index) {
            scene.batches[index].flags = BTBH_GPU_SCENE_BATCH_NO_RC_SOURCE;
        }
        uint8_t *all_flagged = render(smoke, &scene);
        build_scene(smoke, &far, &scene);
        scene.batches[scene.far_glow_box].emission_energy = 0.0f;
        scene.batches[scene.far_source_box].rc_receiver = 1u;
        uint8_t *dark = render(smoke, &scene);
        char what[160];
        (void)snprintf(what, sizeof(what), "NO_RC_SOURCE, %s: a glow outside the view against none", label);
        (void)expect_different(what, glowing, dark);
        (void)snprintf(what, sizeof(what), "NO_RC_SOURCE, %s: the glow flagged against none", label);
        expect_same(what, flagged, dark);
        (void)snprintf(what, sizeof(what), "NO_RC_SOURCE, %s: every batch flagged against none", label);
        expect_same(what, all_flagged, dark);
        dump_image(kinds[kind] == CASCADES_ON ? "far-glow-rc" : "far-glow-additive", glowing);
        dump_image(kinds[kind] == CASCADES_ON ? "far-glow-rc-flagged" : "far-glow-additive-flagged", flagged);
        free(glowing);
        free(flagged);
        free(all_flagged);
        free(dark);
    }
    /* In view: the flagged glow still glows (it differs from no emission on its own pixels only) and lights nothing
       (it differs from the unflagged one). With additive cascades its pixels are exactly the box's; with cascades
       that aren't additive its emission on screen also reaches a few pixels round it through their screen pass at
       their own resolution (their resolve adds a surface's emission, as before), but no farther. */
    for (uint32_t kind = 0u; kind < 2u; ++kind) {
        const Options near = { .name = "glow", .shadows = SHADOWS_SUN, .cascades = kinds[1u - kind], .glow = true };
        const char *label = near.cascades == CASCADES_ADDITIVE ? "additive cascades" : "cascades";
        build_scene(smoke, &near, &scene);
        uint8_t *glowing = render(smoke, &scene);
        scene.batches[scene.glow_box].flags = BTBH_GPU_SCENE_BATCH_NO_RC_SOURCE;
        uint8_t *flagged = render(smoke, &scene);
        scene.batches[scene.glow_box].emission_energy = 0.0f;
        scene.batches[scene.glow_box].flags = 0u;
        uint8_t *dark = render(smoke, &scene);
        char what[160];
        (void)snprintf(what, sizeof(what), "NO_RC_SOURCE in view, %s: the glow flagged against unflagged", label);
        (void)expect_different(what, flagged, glowing);
        int rectangle[4];
        box_rectangle(&scene, scene.glow_box, rectangle);
        uint32_t differing = 0u;
        uint32_t outside = 0u;
        int farthest = 0;
        for (uint32_t y = 0u; y < TARGET_SIZE; ++y) {
            for (uint32_t x = 0u; x < TARGET_SIZE; ++x) {
                const uint32_t index = (y * TARGET_SIZE + x) * 4u;
                if (memcmp(&flagged[index], &dark[index], 4u) != 0) {
                    differing += 1u;
                    const int dx = (int)x < rectangle[0] ? rectangle[0] - (int)x
                        : ((int)x > rectangle[2] ? (int)x - rectangle[2] : 0);
                    const int dy = (int)y < rectangle[1] ? rectangle[1] - (int)y
                        : ((int)y > rectangle[3] ? (int)y - rectangle[3] : 0);
                    const int distance = dx > dy ? dx : dy;
                    outside += distance > 0 ? 1u : 0u;
                    farthest = distance > farthest ? distance : farthest;
                }
            }
        }
        printf("NO_RC_SOURCE in view, %s: the glow flagged against no emission: %u pixels differ, %u outside the box's "
               "rectangle (%d..%d, %d..%d), at most %d pixels from it\n", label, differing, outside, rectangle[0],
               rectangle[2], rectangle[1], rectangle[3], farthest);
        assert(differing > 0u);
        assert(near.cascades == CASCADES_ADDITIVE ? outside == 0u : farthest <= 8);
        free(glowing);
        free(flagged);
        free(dark);
    }
    /* Without the cascades: nothing. */
    {
        const Options plain = { .name = "glow-no-cascades", .shadows = SHADOWS_SUN, .glow = true,
            .far_glow = true, .unlit_box = true };
        build_scene(smoke, &plain, &scene);
        uint8_t *unflagged = render(smoke, &scene);
        for (uint32_t index = 0u; index < scene.count; ++index) {
            scene.batches[index].flags = BTBH_GPU_SCENE_BATCH_NO_RC_SOURCE;
        }
        uint8_t *flagged = render(smoke, &scene);
        expect_same("NO_RC_SOURCE without the cascades: every batch flagged against none", flagged, unflagged);
        free(unflagged);
        free(flagged);
    }
}

/* Renders `options`, with the cascades as given and the post flags. */
static uint8_t *render_with(SmokeWindow *smoke, Options options, Cascades cascades, uint8_t post_flags,
    bool flag_unlit_box) {
    static Scene scene;
    options.cascades = cascades;
    build_scene(smoke, &options, &scene);
    scene.post.flags = post_flags;
    if (flag_unlit_box && scene.unlit_box != MAX_BOXES) {
        scene.batches[scene.unlit_box].flags = BTBH_GPU_SCENE_BATCH_NO_RC_SOURCE;
    }
    return render(smoke, &scene);
}

/* ADDITIVE_CASCADES_KEEP_HBAO. */
static void check_keep_hbao(SmokeWindow *smoke) {
    const uint8_t keep = BTBH_GPU_SCENE_POST_ADDITIVE_CASCADES_KEEP_HBAO;
    const uint8_t reflectivity = BTBH_GPU_SCENE_POST_SSR_SURFACE_REFLECTIVITY;
    static const char *const shadow_names[3] = { "no shadow", "the sun's shadow", "the omni shadow" };
    /* Nothing glowing (the unlit box flagged NO_RC_SOURCE): as without the cascades, to the byte. */
    for (uint32_t shadows = 0u; shadows < 3u; ++shadows) {
        const Options options = { .name = "keep-hbao", .shadows = (Shadows)shadows, .occlusion = OCCLUSION_HBAO,
            .gold = true, .unlit_box = true };
        uint8_t *off = render_with(smoke, options, CASCADES_OFF, 0u, true);
        uint8_t *additive = render_with(smoke, options, CASCADES_ADDITIVE, 0u, true);
        uint8_t *kept = render_with(smoke, options, CASCADES_ADDITIVE, keep, true);
        char what[200];
        (void)snprintf(what, sizeof(what), "KEEP_HBAO, %s, nothing glowing: additive cascades against none",
            shadow_names[shadows]);
        (void)expect_different(what, off, additive);
        print_darkening("  additive cascades against none", off, additive);
        (void)snprintf(what, sizeof(what), "KEEP_HBAO, %s, nothing glowing: additive cascades with it against none",
            shadow_names[shadows]);
        expect_same(what, off, kept);
        if (shadows == 1u) {
            dump_image("keep-hbao-off", off);
            dump_image("keep-hbao-additive", additive);
            dump_image("keep-hbao-additive-kept", kept);
        }
        free(off);
        free(additive);
        free(kept);
    }
    /* Glowing, with an HBAO that darkens nothing: the bounce light is the same with it as without. Next to a metal
       surface it differs a little: the cascades' resolve samples the targets at its own, coarser resolution, and
       blends the albedo and the metalness separately where it reads the albedo target, but albedo times
       (1 - metalness) under the flag. */
    for (uint32_t metal = 0u; metal < 2u; ++metal) {
        const Options options = { .name = "keep-hbao-bounce", .shadows = SHADOWS_SUN,
            .occlusion = OCCLUSION_HBAO_NONE, .glow = true, .gold = metal != 0u, .no_metal = metal == 0u,
            .unlit_box = true };
        static Scene scene;
        build_scene(smoke, &options, &scene);
        uint8_t *off = render_with(smoke, options, CASCADES_OFF, 0u, false);
        uint8_t *additive = render_with(smoke, options, CASCADES_ADDITIVE, 0u, false);
        uint8_t *kept = render_with(smoke, options, CASCADES_ADDITIVE, keep, false);
        (void)expect_different("KEEP_HBAO, glowing, an HBAO that darkens nothing: additive cascades against none",
            off, additive);
        if (metal == 0u) {
            expect_same("KEEP_HBAO, glowing, an HBAO that darkens nothing, no metal: additive cascades with it "
                "against without", additive, kept);
        } else {
            int rectangles[2][4];
            for (uint32_t box = 0u; box < scene.metal_box_count; ++box) {
                box_rectangle(&scene, scene.metal_boxes[box], rectangles[box]);
            }
            uint32_t differing = 0u;
            int farthest = 0;
            int largest = 0;
            for (uint32_t y = 0u; y < TARGET_SIZE; ++y) {
                for (uint32_t x = 0u; x < TARGET_SIZE; ++x) {
                    const uint32_t index = (y * TARGET_SIZE + x) * 4u;
                    int difference = 0;
                    for (uint32_t channel = 0u; channel < 4u; ++channel) {
                        const int d = abs((int)additive[index + channel] - (int)kept[index + channel]);
                        difference = d > difference ? d : difference;
                    }
                    if (difference == 0) {
                        continue;
                    }
                    differing += 1u;
                    largest = difference > largest ? difference : largest;
                    int nearest = 1 << 20;
                    for (uint32_t box = 0u; box < scene.metal_box_count; ++box) {
                        const int *r = rectangles[box];
                        const int dx = (int)x < r[0] ? r[0] - (int)x : ((int)x > r[2] ? (int)x - r[2] : 0);
                        const int dy = (int)y < r[1] ? r[1] - (int)y : ((int)y > r[3] ? (int)y - r[3] : 0);
                        const int distance = dx > dy ? dx : dy;
                        nearest = distance < nearest ? distance : nearest;
                    }
                    farthest = nearest > farthest ? nearest : farthest;
                }
            }
            printf("KEEP_HBAO, glowing, an HBAO that darkens nothing, metal boxes: additive cascades with it against "
                   "without: %u pixels differ (largest %d levels), at most %d pixels from a metal box's rectangle\n",
                differing, largest, farthest);
            assert(farthest <= 6);
        }
        free(off);
        free(additive);
        free(kept);
    }
    /* Glowing, with the HBAO: printed. */
    {
        const Options options = { .name = "keep-hbao-glow", .shadows = SHADOWS_SUN, .occlusion = OCCLUSION_HBAO,
            .glow = true, .gold = true, .unlit_box = true };
        uint8_t *additive = render_with(smoke, options, CASCADES_ADDITIVE, 0u, false);
        uint8_t *kept = render_with(smoke, options, CASCADES_ADDITIVE, keep, false);
        (void)expect_different("KEEP_HBAO, glowing: additive cascades with it against without", additive, kept);
        print_darkening("  without it against with it", kept, additive);
        dump_image("keep-hbao-glow-additive", additive);
        dump_image("keep-hbao-glow-additive-kept", kept);
        free(additive);
        free(kept);
    }
    /* With the reflections: the reflectivity from the ambient target, as without the cascades with
       SSR_SURFACE_REFLECTIVITY. */
    {
        const Options options = { .name = "keep-hbao-ssr", .shadows = SHADOWS_SUN, .occlusion = OCCLUSION_HBAO,
            .reflections = true, .gold = true, .unlit_box = true };
        uint8_t *off = render_with(smoke, options, CASCADES_OFF, reflectivity, true);
        uint8_t *kept = render_with(smoke, options, CASCADES_ADDITIVE, keep, true);
        uint8_t *both = render_with(smoke, options, CASCADES_ADDITIVE, (uint8_t)(keep | reflectivity), true);
        expect_same("KEEP_HBAO with the reflections, nothing glowing: against none with SSR_SURFACE_REFLECTIVITY",
            off, kept);
        expect_same("KEEP_HBAO with the reflections: with SSR_SURFACE_REFLECTIVITY too", kept, both);
        free(off);
        free(kept);
        free(both);
    }
    /* Ignored where it doesn't apply. */
    {
        struct { const char *what; Options options; Cascades cascades; } cases[4] = {
            { "cascades not additive", { .name = "keep-ignored-rc", .shadows = SHADOWS_SUN,
                .occlusion = OCCLUSION_HBAO, .glow = true }, CASCADES_ON },
            { "ssao_contact", { .name = "keep-ignored-contact", .shadows = SHADOWS_SUN,
                .occlusion = OCCLUSION_HBAO_CONTACT, .glow = true }, CASCADES_ADDITIVE },
            { "the SSAO", { .name = "keep-ignored-ssao", .shadows = SHADOWS_SUN,
                .occlusion = OCCLUSION_SSAO, .glow = true }, CASCADES_ADDITIVE },
            { "no cascades", { .name = "keep-ignored-off", .shadows = SHADOWS_SUN,
                .occlusion = OCCLUSION_HBAO, .glow = true }, CASCADES_OFF },
        };
        for (uint32_t index = 0u; index < 4u; ++index) {
            uint8_t *without = render_with(smoke, cases[index].options, cases[index].cascades, 0u, false);
            uint8_t *with = render_with(smoke, cases[index].options, cases[index].cascades, keep, false);
            char what[160];
            (void)snprintf(what, sizeof(what), "KEEP_HBAO ignored, %s", cases[index].what);
            expect_same(what, without, with);
            free(without);
            free(with);
        }
    }
}

/* SSR_SURFACE_REFLECTIVITY. */
static void check_ssr_reflectivity(SmokeWindow *smoke) {
    const uint8_t reflectivity = BTBH_GPU_SCENE_POST_SSR_SURFACE_REFLECTIVITY;
    static const char *const shadow_names[3] = { "no shadow", "the sun's shadow", "the omni shadow" };
    for (uint32_t shadows = 0u; shadows < 3u; ++shadows) {
        Options options = { .name = "ssr", .shadows = (Shadows)shadows, .reflections = false };
        uint8_t *none = render_with(smoke, options, CASCADES_OFF, 0u, false);
        uint8_t *none_flagged = render_with(smoke, options, CASCADES_OFF, reflectivity, false);
        options.reflections = true;
        uint8_t *unset = render_with(smoke, options, CASCADES_OFF, 0u, false);
        uint8_t *set = render_with(smoke, options, CASCADES_OFF, reflectivity, false);
        uint8_t *additive = render_with(smoke, options, CASCADES_ADDITIVE, 0u, false);
        uint8_t *additive_flagged = render_with(smoke, options, CASCADES_ADDITIVE, reflectivity, false);
        char what[200];
        (void)snprintf(what, sizeof(what), "SSR_SURFACE_REFLECTIVITY, %s: unset reflections against none (0.8.0's)",
            shadow_names[shadows]);
        expect_same(what, none, unset);
        (void)snprintf(what, sizeof(what), "SSR_SURFACE_REFLECTIVITY, %s: without the reflections, set against unset",
            shadow_names[shadows]);
        expect_same(what, none, none_flagged);
        (void)snprintf(what, sizeof(what), "SSR_SURFACE_REFLECTIVITY, %s: the reflections set against none",
            shadow_names[shadows]);
        (void)expect_different(what, none, set);
        (void)snprintf(what, sizeof(what),
            "SSR_SURFACE_REFLECTIVITY, %s: set against additive cascades with nothing glowing (glass, grey metal, "
            "dielectric)", shadow_names[shadows]);
        expect_same(what, set, additive);
        (void)snprintf(what, sizeof(what), "SSR_SURFACE_REFLECTIVITY, %s: additive cascades, set against unset",
            shadow_names[shadows]);
        expect_same(what, additive, additive_flagged);
        if (shadows == 1u) {
            dump_image("ssr-none", none);
            dump_image("ssr-set", set);
            dump_image("ssr-additive", additive);
        }
        free(none);
        free(none_flagged);
        free(unset);
        free(set);
        free(additive);
        free(additive_flagged);
    }
    /* A gold metal: as bright, untinted. */
    {
        const Options options = { .name = "ssr-gold", .shadows = SHADOWS_SUN, .reflections = true, .gold = true };
        uint8_t *set = render_with(smoke, options, CASCADES_OFF, reflectivity, false);
        uint8_t *additive = render_with(smoke, options, CASCADES_ADDITIVE, 0u, false);
        (void)expect_different("SSR_SURFACE_REFLECTIVITY with a gold metal: set against additive cascades", set,
            additive);
        free(set);
        free(additive);
    }
    /* With the cascades on: nothing. */
    {
        const Options options = { .name = "ssr-rc", .shadows = SHADOWS_SUN, .reflections = true, .gold = true,
            .glow = true };
        uint8_t *unset = render_with(smoke, options, CASCADES_ON, 0u, false);
        uint8_t *set = render_with(smoke, options, CASCADES_ON, reflectivity, false);
        expect_same("SSR_SURFACE_REFLECTIVITY with the cascades on: set against unset", unset, set);
        free(unset);
        free(set);
    }
}

static void check_refusals(SmokeWindow *smoke) {
    static Scene scene;
    const Options options = { .name = "refusal", .shadows = SHADOWS_SUN, .occlusion = OCCLUSION_HBAO,
        .reflections = true };
    BTBHGPUSceneReport report;
    build_scene(smoke, &options, &scene);
    scene.batches[1].flags = (uint8_t)(1u << 3);
    assert(render_scene(smoke, &scene, BTBH_GPU_INVALID_ARGUMENT, &report) == NULL);
    build_scene(smoke, &options, &scene);
    scene.post.flags = (uint8_t)(1u << 2);
    assert(render_scene(smoke, &scene, BTBH_GPU_INVALID_ARGUMENT, &report) == NULL);
    scene.post.flags = 0x80u;
    assert(render_scene(smoke, &scene, BTBH_GPU_INVALID_ARGUMENT, &report) == NULL);
    BTBHGPUSceneReport prewarm_report;
    assert(btbh_gpu_window_scene_prewarm(
        smoke->window,
        shader_paths[0], shader_paths[1], shader_paths[2], shader_paths[3],
        shader_paths[4], shader_paths[5], shader_paths[6], shader_paths[7],
        shader_paths[8], shader_paths[9], shader_paths[10],
        SMOKE_SHADER_FORMAT, TARGET_SIZE, TARGET_SIZE, &scene.lighting, &scene.post, scene.batches, scene.count,
        &prewarm_report) == BTBH_GPU_INVALID_ARGUMENT);
    /* Both known flags together are taken. */
    scene.post.flags = (uint8_t)(BTBH_GPU_SCENE_POST_ADDITIVE_CASCADES_KEEP_HBAO |
        BTBH_GPU_SCENE_POST_SSR_SURFACE_REFLECTIVITY);
    uint8_t *image = render_scene(smoke, &scene, BTBH_GPU_OK, &report);
    free(image);
    puts("refusals: an unknown batch flag, unknown post flags (present and prewarm)");
}

/* ----------------------------------------------------------------------------------------------- the bench */

#define BENCH_MAX_ROUNDS 40
#define BENCH_WARM_FRAMES 3
#define BENCH_FRAMES 20

typedef struct BenchConfig {
    const char *name;
    Options options;
    uint8_t post_flags;
    /* The window (and its shaders) the turns render with: the smoke's, or the one with the baseline shaders. */
    SmokeWindow *smoke;
    const char (*paths)[1024];
    BTBHGPUTexture *target;
    double times[BENCH_MAX_ROUNDS];
    int sample_count;
} BenchConfig;

static int compare_doubles(const void *left, const void *right) {
    const double a = *(const double *)left;
    const double b = *(const double *)right;
    return a < b ? -1 : (a > b ? 1 : 0);
}

/* One turn: BENCH_WARM_FRAMES renders, a wait, then BENCH_FRAMES renders back to back and one wait. */
static void bench_turn(BenchConfig *config, int rounds) {
    static Scene scene;
    SmokeWindow *smoke = config->smoke;
    build_scene(smoke, &config->options, &scene);
    scene.post.flags = config->post_flags;
    if (scene.unlit_box != MAX_BOXES) {
        scene.batches[scene.unlit_box].flags = BTBH_GPU_SCENE_BATCH_NO_RC_SOURCE;
    }
    const char (*paths)[1024] = config->paths;
    BTBHGPUTexture *target = config->target;
    SDL_GPUDevice *device = smoke->window->device->gpu;
    uint64_t started = 0u;
    for (int frame = 0; frame < BENCH_WARM_FRAMES + BENCH_FRAMES; ++frame) {
        if (frame == BENCH_WARM_FRAMES) {
            (void)SDL_WaitForGPUIdle(device);
            started = SDL_GetTicksNS();
        }
        BTBHGPUSceneReport report;
        const BTBHGPUResult rendered = btbh_gpu_window_scene_render_to_target(
            smoke->window, target,
            paths[0], paths[1], paths[2], paths[3], paths[4], paths[5], paths[6], paths[7], paths[8], paths[9],
            paths[10],
            SMOKE_SHADER_FORMAT, target->width, target->height,
            scene.camera.view_projection, scene.camera.right, scene.camera.up, scene.camera.position,
            &scene.lighting, &scene.post, &scene.instances[0][0], scene.count, scene.count,
            scene.batches, NULL, NULL, scene.count, 12u, 14u, 20u, 255u, &report);
        if (rendered != BTBH_GPU_OK) {
            fprintf(stderr, "bench: %s\n", report.message);
        }
        assert(rendered == BTBH_GPU_OK);
    }
    (void)SDL_WaitForGPUIdle(device);
    if (config->sample_count < rounds) {
        config->times[config->sample_count++] = (double)(SDL_GetTicksNS() - started) / 1.0e6 / BENCH_FRAMES;
    }
}

static void bench_configs(const char *title, BenchConfig *configs, uint32_t count, int rounds) {
    for (int round = 0; round < rounds; ++round) {
        for (uint32_t index = 0u; index < count; ++index) {
            /* (Each round in another order, so no configuration always follows the same one.) */
            const uint32_t which = (index + (uint32_t)round) % count;
            bench_turn(&configs[which], rounds);
        }
    }
    printf("bench: %s, %ux%u, %d interleaved turns of %d frames:\n", title, configs[0].target->width,
        configs[0].target->height, rounds, BENCH_FRAMES);
    for (uint32_t index = 0u; index < count; ++index) {
        BenchConfig *config = &configs[index];
        qsort(config->times, (size_t)config->sample_count, sizeof(config->times[0]), compare_doubles);
        printf("  %-52s fastest %7.3f ms, median %7.3f ms a frame (%+.3f, %+.3f against the first)\n", config->name,
            config->times[0], config->times[config->sample_count / 2], config->times[0] - configs[0].times[0],
            config->times[config->sample_count / 2] - configs[0].times[configs[0].sample_count / 2]);
    }
    (void)fflush(stdout);
}

/* BTBH_SMOKE_BENCH=1, =ssr or =hbao; BTBH_SMOKE_BENCH_ROUNDS turns (10 by default, up to 40); with
   BTBH_SCENE_FLAGS_BASELINE_SHADER_DIR (an earlier release's compiled shaders), the unset configuration again in a
   window whose pipelines are made from those. */
static void run_bench(SmokeWindow *smoke, SmokeWindow *baseline, const char (*baseline_paths)[1024], const char *mode) {
    const char *device_name = SDL_GetStringProperty(
        SDL_GetGPUDeviceProperties(smoke->window->device->gpu), SDL_PROP_GPU_DEVICE_NAME_STRING, "unknown");
    const char *rounds_text = SDL_getenv("BTBH_SMOKE_BENCH_ROUNDS");
    int rounds = rounds_text != NULL ? atoi(rounds_text) : 10;
    rounds = rounds < 1 ? 10 : (rounds > BENCH_MAX_ROUNDS ? BENCH_MAX_ROUNDS : rounds);
    printf("bench on %s%s\n", device_name, baseline != NULL ? ", with the baseline shaders" : "");
    BTBHGPUTextureReport texture_report;
    BTBHGPUTexture *target = btbh_gpu_window_render_target_create(smoke->window, 1920u, 1080u,
        BTBH_GPU_TEXTURE_FILTER_LINEAR, &texture_report);
    assert(target != NULL);
    BTBHGPUTexture *baseline_target = NULL;
    if (baseline != NULL) {
        baseline_target = btbh_gpu_window_render_target_create(baseline->window, 1920u, 1080u,
            BTBH_GPU_TEXTURE_FILTER_LINEAR, &texture_report);
        assert(baseline_target != NULL);
    }
    const uint32_t extra = baseline != NULL ? 1u : 0u;
    if (strcmp(mode, "1") == 0 || strcmp(mode, "ssr") == 0) {
        BenchConfig configs[4];
        memset(configs, 0, sizeof(configs));
        const Options base = { .name = "bench-ssr", .shadows = SHADOWS_SUN, .reflections = true, .gold = true };
        configs[0] = (BenchConfig){ .name = "no cascades, SSR unset (reflects nothing)", .options = base };
        configs[1] = (BenchConfig){ .name = "no cascades, SSR with SSR_SURFACE_REFLECTIVITY", .options = base,
            .post_flags = BTBH_GPU_SCENE_POST_SSR_SURFACE_REFLECTIVITY };
        configs[2] = (BenchConfig){ .name = "no cascades, no SSR", .options = base };
        configs[2].options.reflections = false;
        configs[3] = (BenchConfig){ .name = "no cascades, SSR unset, baseline shaders", .options = base };
        for (uint32_t index = 0u; index < 4u; ++index) {
            configs[index].smoke = index == 3u ? baseline : smoke;
            configs[index].paths = index == 3u ? baseline_paths : (const char (*)[1024])shader_paths;
            configs[index].target = index == 3u ? baseline_target : target;
        }
        bench_configs("the reflections' reflectivity", configs, 3u + extra, rounds);
    }
    if (strcmp(mode, "1") == 0 || strcmp(mode, "hbao") == 0) {
        BenchConfig configs[4];
        memset(configs, 0, sizeof(configs));
        const Options base = { .name = "bench-hbao", .shadows = SHADOWS_SUN, .cascades = CASCADES_ADDITIVE,
            .occlusion = OCCLUSION_HBAO, .reflections = true, .glow = true, .gold = true, .unlit_box = true };
        configs[0] = (BenchConfig){ .name = "additive cascades, HBAO and SSR unset", .options = base };
        configs[1] = (BenchConfig){ .name = "additive cascades, HBAO and SSR with KEEP_HBAO", .options = base,
            .post_flags = BTBH_GPU_SCENE_POST_ADDITIVE_CASCADES_KEEP_HBAO };
        configs[2] = (BenchConfig){ .name = "no cascades, HBAO and SSR", .options = base };
        configs[2].options.cascades = CASCADES_OFF;
        configs[3] = (BenchConfig){ .name = "additive cascades, HBAO and SSR unset, baseline shaders",
            .options = base };
        for (uint32_t index = 0u; index < 4u; ++index) {
            configs[index].smoke = index == 3u ? baseline : smoke;
            configs[index].paths = index == 3u ? baseline_paths : (const char (*)[1024])shader_paths;
            configs[index].target = index == 3u ? baseline_target : target;
        }
        bench_configs("the HBAO under additive cascades", configs, 3u + extra, rounds);
    }
    if (baseline_target != NULL) {
        btbh_gpu_texture_destroy(baseline_target);
    }
    btbh_gpu_texture_destroy(target);
}
#endif

/* ------------------------------------------------------------------------------------------------------ main */

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
    return true;
}

static SmokeWindow open_smoke_window(BTBHGPUDevice *device, const char *title) {
    SmokeWindow smoke;
    memset(&smoke, 0, sizeof(smoke));
    const BTBHGPUWindowDescriptor descriptor = {
        .title = title,
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
    smoke.cube = create_cube(smoke.window);
    smoke.glow = create_glow(smoke.window);
    BTBHGPUTextureReport texture_report;
    smoke.target = btbh_gpu_window_render_target_create(smoke.window, TARGET_SIZE, TARGET_SIZE,
        BTBH_GPU_TEXTURE_FILTER_LINEAR, &texture_report);
    assert(smoke.target != NULL);
    return smoke;
}

static void close_smoke_window(SmokeWindow *smoke) {
    btbh_gpu_texture_destroy(smoke->target);
    btbh_gpu_texture_destroy(smoke->glow);
    btbh_gpu_custom_mesh_destroy(smoke->cube);
    btbh_gpu_window_destroy(smoke->window);
    memset(smoke, 0, sizeof(*smoke));
}

int main(void) {
    /* (Unbuffered: what was checked shows before a failed assertion.) */
    (void)setvbuf(stdout, NULL, _IONBF, 0);
    if (!load_shader_paths()) {
        return SMOKE_SKIP;
    }
    (void)SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    const char *bench = SDL_getenv("BTBH_SMOKE_BENCH");
    const bool benching = bench != NULL && bench[0] != '\0';
    const BTBHGPUDeviceDescriptor device_descriptor = {
        .backend = SMOKE_BACKEND,
        .required_features = BTBH_GPU_REQUIRED_FEATURES,
        .debug_mode = benching ? 0u : 1u,
    };
    BTBHGPUDeviceReport device_report;
    memset(&device_report, 0, sizeof(device_report));
    BTBHGPUDevice *device = btbh_gpu_device_create(&device_descriptor, &device_report);
    if (device == NULL) {
        fprintf(stderr, "skipped: no device (%s)\n", device_report.message);
        return SMOKE_SKIP;
    }
    SmokeWindow smoke = open_smoke_window(device, "BTBH scene flags smoke");

#if defined(SMOKE_HAS_FLAGS)
    if (benching) {
        const char *baseline_dir = SDL_getenv("BTBH_SCENE_FLAGS_BASELINE_SHADER_DIR");
        static char baseline_paths[11][1024];
        SmokeWindow baseline;
        memset(&baseline, 0, sizeof(baseline));
        const bool with_baseline = baseline_dir != NULL && baseline_dir[0] != '\0';
        if (with_baseline) {
            for (int index = 0; index < 11; ++index) {
                (void)snprintf(baseline_paths[index], sizeof(baseline_paths[index]), "%s/%s%s", baseline_dir,
                    shader_names[index], SMOKE_SHADER_EXTENSION);
            }
            baseline = open_smoke_window(device, "BTBH scene flags smoke (baseline shaders)");
        }
        run_bench(&smoke, with_baseline ? &baseline : NULL, (const char (*)[1024])baseline_paths, bench);
        if (with_baseline) {
            close_smoke_window(&baseline);
        }
    } else
#endif
    {
        check_gallery(&smoke);
#if defined(SMOKE_HAS_FLAGS)
        check_no_rc_source(&smoke);
        check_keep_hbao(&smoke);
        check_ssr_reflectivity(&smoke);
        check_refusals(&smoke);
#endif
    }

    close_smoke_window(&smoke);
    btbh_gpu_device_destroy(device);
    puts("btbh_gpu_scene_flags_smoke: passed");
    return 0;
}
