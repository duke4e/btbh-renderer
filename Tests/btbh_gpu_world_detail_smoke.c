/* RFC 0008's GPU smoke: the world detail (BTBHGPUSceneGeometryBatch.world_detail), rendered into a target and read
 * back (hidden windows, a debug device). Also RFC 0009's phase 1: the procedural detail's pattern 7.
 *
 * Unset. This part also builds against 0.5.1's header, which has no world detail: there it only renders (and
 * writes) the gallery, so that a build of the earlier release and a build of this one can be compared.
 * - A gallery of scenes through the scene fragment shader's variants: the sun's shadow (tent, and soft with lamps,
 *   point lights, the spot light, wetness, rain and the procedural surface detail), the omni shadow, no shadow,
 *   unlit batches (one through the fast unlit pipeline), see-through batches, the surface variants (water,
 *   dissolve, shatter, glitter, rim), and the radiance cascades where they run. Each is rendered twice and must
 *   come out the same (no state carried from one render to the next).
 * - Then the procedural surface detail's patterns 0..6 together (ground, grass, facade, paving, glass, a lamp's
 *   lens and none, some on the first set's textures) through each of the six scene fragment shaders: the sun's
 *   shadow, the sun's and the omni shadow, no shadow, and the three again with the radiance cascades, wet in the
 *   rain at night (lit windows); and once more dry by day with the sun's soft shadow.
 * - Each renders byte for byte the same with world_detail NULL and with a detail whose three textures are NULL.
 * - BTBH_WORLD_DETAIL_BASELINE_SHADER_DIR=<dir> (an earlier release's compiled shaders and assets): the gallery
 *   renders byte for byte the same with those shaders too (in another window, whose pipelines are made from them).
 * - BTBH_WORLD_DETAIL_DUMP=<dir> writes the gallery (unset-<name>.png); BTBH_WORLD_DETAIL_REFERENCE=<dir> compares
 *   each image with that directory's and requires 0 differing pixels.
 *
 * Set (0.6.0):
 * - The projection, read back pixel by pixel: unlit boxes over a ground slab under four cameras, with a detail
 *   whose albedo texture holds each texel's column in red and its row in green (nearest filtering, no mips). Each
 *   pixel's texel is decoded and compared with what the C copy of the projection (btbh_gpu_world_detail.h) gives
 *   at the point the pixel's ray hits: level faces seen from above and below (roofs, the ground, a floating slab's
 *   underside) with level_tile, vertical walls facing four ways and a 45 degree wall, and pitched faces (a box
 *   tilted 30 degrees) with wall_tile. The instances carry seeds in custom w: two touching boxes with one seed
 *   continue one pattern, and one box drawn with two seeds shows two offsets.
 * - Packed against separate: the grey albedo in the ORM's blue (ALBEDO_IN_ORM_BLUE) and the same values in an
 *   albedo texture render the same lit scene, and both differ from no detail.
 * - Neutral: an ORM of (1, 0.8, 0.8) packed with both scales 1.25 (v2's mean), or a separate albedo of 0.8,
 *   render byte for byte as no detail; with a flat normal map (128, 128) too, within 3/255 (128 is a tilt of a
 *   255th, not none).
 * - An unlit see-through batch with a detail leaves the fast unlit pipeline and shows the detail; a refracting
 *   batch ignores its detail.
 * - Refusals: the present fails with BTBH_GPU_INVALID_ARGUMENT, naming the batch, for a bad tile, angle, seed
 *   component, flag, reserved byte, packed layout or texture.
 * - Old shaders: a batch with a detail and a scene fragment shader that doesn't name world_detail_normal_texture
 *   (shaders from before 0.6.0) fails the present with the shader's path; without the detail it draws.
 * - BTBH_SMOKE_BENCH=1: what the detail costs (prints only). Eight coplanar screen-filling layers drawn with
 *   LESS_EQUAL (every layer shades every pixel) under a heavy pixel (the opaque-sort bench's) and a light one, at
 *   1920x1080 and 960x600, in interleaved turns: no detail, packed, three maps, and (with
 *   BTBH_WORLD_DETAIL_BASELINE_SHADER_DIR) no detail with those shaders. The difference over eight is the cost
 *   per screen of detailed surface.
 *
 * Pattern 7, the facade's weather (RFC 0009 phase 1, 0.7.0):
 * - Equal: pattern 7 ([0, 0, 0, 7]) renders byte for byte as pattern 3 with x and y at 0, at z 6 and at z 0.37,
 *   through the six scene fragment shaders, dry by day and soaked at night: the pattern scenes with every batch
 *   but the lens on it, some on the first set's textures, the ground and a box with a packed world detail too
 *   (v2's combination). And it changes them: the street darkens dry, and the walls get wet.
 * - Refusals: x, y or z set with pattern 7, and a pattern past 7, fail the present with BTBH_GPU_INVALID_ARGUMENT.
 * - Old shaders (BTBH_WORLD_DETAIL_BASELINE_SHADER_DIR at 0.6.0's): pattern 7 draws without its weather (prints).
 * - BTBH_SMOKE_BENCH=1 (or =weather, this bench only): the eight layers again, dry and soaked, under the heavy and
 *   the light pixel at both sizes: pattern 0 against 7 and against 3 (x 0.07, y 0, z 6), and (with the baseline
 *   shaders) pattern 0 with those.
 *
 * It needs a GPU and the production shaders; with no device it is skipped (77). */
#undef NDEBUG

#include "btbh_gpu/btbh_gpu.h"
#include "btbh_gpu_internal.h"
#include "btbh_gpu_package_internal.h"
#if defined(BTBH_GPU_SCENE_HAS_WORLD_DETAIL)
#include "btbh_gpu_world_detail.h"
#endif

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_surface.h>
#include <SDL3/SDL_timer.h>

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SMOKE_SKIP 77
#define GALLERY_SIZE 128u
#define PROJECTION_SIZE 192u
#define PI_F 3.14159265f

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
    char paths[11][1024];
} ShaderSet;

/* A hidden window with its own meshes, textures and targets (each of these belongs to one window). */
typedef struct SmokeWindow {
    BTBHGPUWindow *window;
    BTBHGPUCustomMesh *cube;      /* lit on its outside (front faces outward) */
    BTBHGPUTexture *gallery;      /* GALLERY_SIZE^2 */
    BTBHGPUTexture *projection;   /* PROJECTION_SIZE^2 */
    BTBHGPUTexture *checker;      /* the first set's albedo */
    BTBHGPUTexture *bumps;        /* the first set's normal map */
    BTBHGPUTexture *orm;          /* the first set's ORM */
    BTBHGPUTexture *glow;         /* the first set's emission */
    ShaderSet shaders;
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

static SmokeCamera look_at(const float eye[3], const float centre[3], float fov_y, float aspect) {
    SmokeCamera camera;
    memset(&camera, 0, sizeof(camera));
    const float near_plane = 0.1f;
    const float far_plane = 60.0f;
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

/* A box: the unit cube scaled, turned by pitch (about x) and then yaw (about y), and moved. */
typedef struct Box {
    float centre[3];
    float yaw;
    float pitch;
    float scale[3];
    float colour[4];
    float custom[4];
} Box;

static void box_rotation(const Box *box, float r[3][3]) {
    const float cy = cosf(box->yaw);
    const float sy = sinf(box->yaw);
    const float cp = cosf(box->pitch);
    const float sp = sinf(box->pitch);
    /* Ry(yaw) Rx(pitch), rows. */
    r[0][0] = cy;   r[0][1] = sy * sp; r[0][2] = sy * cp;
    r[1][0] = 0.0f; r[1][1] = cp;      r[1][2] = -sp;
    r[2][0] = -sy;  r[2][1] = cy * sp; r[2][2] = cy * cp;
}

/* Instances: 16 floats of transform (column-major), colour, custom. */
static void box_instance(const Box *box, float out[24]) {
    float r[3][3];
    box_rotation(box, r);
    memset(out, 0, 24u * sizeof(float));
    for (int column = 0; column < 3; ++column) {
        for (int row = 0; row < 3; ++row) {
            out[column * 4 + row] = r[row][column] * box->scale[column];
        }
    }
    out[12] = box->centre[0];
    out[13] = box->centre[1];
    out[14] = box->centre[2];
    out[15] = 1.0f;
    memcpy(&out[16], box->colour, 4u * sizeof(float));
    memcpy(&out[20], box->custom, 4u * sizeof(float));
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
    batch.normal_scale = 1.0f;
    batch.roughness = 0.6f;
    batch.occlusion = 1.0f;
    batch.depth_compare = BTBH_GPU_SCENE_DEPTH_LESS;
    batch.cull_mode = BTBH_GPU_SCENE_CULL_NONE;
    batch.stencil_pass = BTBH_GPU_SCENE_STENCIL_REPLACE;
    batch.spot_shadow_visible = 1u;
    return batch;
}

/* A unit cube (-0.5..0.5), 24 floats a vertex: position, normal, uv, tangent, colour, joints, weights; wound so the
   renderer takes its outsides for front faces. */
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
    BTBHGPUCustomMesh *mesh = btbh_gpu_window_custom_mesh_create(
        window, vertices, (uint32_t)sizeof(vertices), 24u, indices, 36u, &report);
    if (mesh == NULL) {
        fprintf(stderr, "cube: %s\n", report.message);
    }
    assert(mesh != NULL);
    return mesh;
}

typedef uint8_t (*TexelFunction)(uint32_t x, uint32_t y, uint32_t channel);

static BTBHGPUTexture *make_texture(
    BTBHGPUWindow *window,
    uint32_t size,
    TexelFunction texel,
    uint8_t mipmaps,
    BTBHGPUTextureFilter filter,
    BTBHGPUTextureColorSpace color_space
) {
    uint8_t *pixels = malloc((size_t)size * size * 4u);
    assert(pixels != NULL);
    for (uint32_t y = 0u; y < size; ++y) {
        for (uint32_t x = 0u; x < size; ++x) {
            for (uint32_t channel = 0u; channel < 4u; ++channel) {
                pixels[((size_t)y * size + x) * 4u + channel] = texel(x, y, channel);
            }
        }
    }
    BTBHGPUTextureReport report;
    BTBHGPUTexture *texture = btbh_gpu_window_texture_create_rgba8_ex(
        window, pixels, size, size, mipmaps, filter, BTBH_GPU_TEXTURE_WRAP_REPEAT, color_space, &report);
    free(pixels);
    if (texture == NULL) {
        fprintf(stderr, "texture: %s\n", report.message);
    }
    assert(texture != NULL);
    return texture;
}

static uint8_t checker_texel(uint32_t x, uint32_t y, uint32_t channel) {
    const uint32_t cell = ((x / 8u) + (y / 8u)) % 2u;
    static const uint8_t colours[2][4] = { { 200, 190, 170, 255 }, { 120, 130, 150, 255 } };
    return colours[cell][channel];
}

static uint8_t bump_texel(uint32_t x, uint32_t y, uint32_t channel) {
    const float tau = 2.0f * PI_F;
    switch (channel) {
        case 0: return (uint8_t)(128.0f + 70.0f * sinf(tau * (float)x / 16.0f));
        case 1: return (uint8_t)(128.0f + 70.0f * cosf(tau * (float)y / 12.0f));
        case 2: return 255u;
        default: return 255u;
    }
}

static uint8_t orm_texel(uint32_t x, uint32_t y, uint32_t channel) {
    switch (channel) {
        case 0: return (uint8_t)(180u + (x * 7u + y * 3u) % 70u);
        case 1: return (uint8_t)(90u + (x * 5u + y * 11u) % 150u);
        case 2: return (uint8_t)((x / 16u + y / 16u) % 2u == 0u ? 0u : 160u);
        default: return 255u;
    }
}

static uint8_t glow_texel(uint32_t x, uint32_t y, uint32_t channel) {
    const bool lit = (x / 4u) % 4u == 0u && (y / 4u) % 3u == 0u;
    static const uint8_t colour[4] = { 255, 180, 90, 255 };
    return lit ? colour[channel] : (channel == 3u ? 255u : 0u);
}

/* The texture's bytes (4 a texel), read back through SDL_GPU. */
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
    /* RGBA whatever the target's byte order (render targets take the swapchain's, BGRA here). */
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

static uint32_t differing_pixels(const uint8_t *a, const uint8_t *b, uint32_t texels, int tolerance, int *largest) {
    uint32_t count = 0u;
    int worst = 0;
    for (uint32_t index = 0u; index < texels; ++index) {
        int difference = 0;
        for (uint32_t channel = 0u; channel < 4u; ++channel) {
            const int d = abs((int)a[index * 4u + channel] - (int)b[index * 4u + channel]);
            difference = d > difference ? d : difference;
        }
        worst = difference > worst ? difference : worst;
        if (difference > tolerance) {
            count += 1u;
        }
    }
    if (largest != NULL) {
        *largest = worst;
    }
    return count;
}

static void set_shaders(ShaderSet *set, const char *directory) {
    for (int index = 0; index < 11; ++index) {
        (void)snprintf(set->paths[index], sizeof(set->paths[index]), "%s/%s%s", directory, shader_names[index],
            SMOKE_SHADER_EXTENSION);
    }
}

static SmokeWindow open_window(BTBHGPUDevice *device, const char *title, const char *shader_directory) {
    SmokeWindow smoke;
    memset(&smoke, 0, sizeof(smoke));
    const BTBHGPUWindowDescriptor descriptor = {
        .title = title,
        .width = 320,
        .height = 180,
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
    smoke.gallery = btbh_gpu_window_render_target_create(
        smoke.window, GALLERY_SIZE, GALLERY_SIZE, BTBH_GPU_TEXTURE_FILTER_LINEAR, &texture_report);
    assert(smoke.gallery != NULL);
    smoke.projection = btbh_gpu_window_render_target_create(
        smoke.window, PROJECTION_SIZE, PROJECTION_SIZE, BTBH_GPU_TEXTURE_FILTER_LINEAR, &texture_report);
    assert(smoke.projection != NULL);
    smoke.checker = make_texture(smoke.window, 64u, checker_texel, 1u, BTBH_GPU_TEXTURE_FILTER_TRILINEAR,
        BTBH_GPU_TEXTURE_COLOR_SPACE_SRGB);
    smoke.bumps = make_texture(smoke.window, 64u, bump_texel, 1u, BTBH_GPU_TEXTURE_FILTER_TRILINEAR,
        BTBH_GPU_TEXTURE_COLOR_SPACE_LINEAR);
    smoke.orm = make_texture(smoke.window, 64u, orm_texel, 1u, BTBH_GPU_TEXTURE_FILTER_TRILINEAR,
        BTBH_GPU_TEXTURE_COLOR_SPACE_LINEAR);
    smoke.glow = make_texture(smoke.window, 64u, glow_texel, 1u, BTBH_GPU_TEXTURE_FILTER_TRILINEAR,
        BTBH_GPU_TEXTURE_COLOR_SPACE_SRGB);
    set_shaders(&smoke.shaders, shader_directory);
    return smoke;
}

static void close_window(SmokeWindow *smoke) {
    btbh_gpu_texture_destroy(smoke->gallery);
    btbh_gpu_texture_destroy(smoke->projection);
    btbh_gpu_texture_destroy(smoke->checker);
    btbh_gpu_texture_destroy(smoke->bumps);
    btbh_gpu_texture_destroy(smoke->orm);
    btbh_gpu_texture_destroy(smoke->glow);
    btbh_gpu_custom_mesh_destroy(smoke->cube);
    btbh_gpu_window_destroy(smoke->window);
    memset(smoke, 0, sizeof(*smoke));
}

/* ------------------------------------------------------------------------------------------- scenes */

static BTBHGPUSceneLighting base_lighting(bool sun_shadow) {
    BTBHGPUSceneLighting lighting;
    memset(&lighting, 0, sizeof(lighting));
    lighting.ambient_red = 0.8f;
    lighting.ambient_green = 0.85f;
    lighting.ambient_blue = 1.0f;
    lighting.ambient_energy = 0.3f;
    const float sun[3] = { 4.0f, 10.0f, 3.0f };
    lighting.directional_x = -sun[0];
    lighting.directional_y = -sun[1];
    lighting.directional_z = -sun[2];
    lighting.directional_energy = 2.0f;
    lighting.directional_red = 1.0f;
    lighting.directional_green = 0.95f;
    lighting.directional_blue = 0.9f;
    lighting.spot_range = 8.0f;
    lighting.spot_falloff = 1.0f;
    lighting.spot_inner_cutoff = 1.0f;
    lighting.spot_outer_cutoff = 0.0f;
    if (sun_shadow) {
        const float centre[3] = { 0.0f, 0.0f, 0.0f };
        float forward[3] = { -sun[0], -sun[1], -sun[2] };
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
            -dot3(sun, side), -dot3(sun, up), dot3(sun, forward), 1.0f,
        };
        (void)centre;
        const float rcp = 1.0f / 9.0f;
        const float range = 1.0f / (1.0f - 30.0f);
        const float orthographic[16] = {
            rcp, 0.0f, 0.0f, 0.0f,
            0.0f, rcp, 0.0f, 0.0f,
            0.0f, 0.0f, range, 0.0f,
            0.0f, 0.0f, range * 1.0f, 1.0f,
        };
        multiply(orthographic, view, lighting.directional_shadow_view_projection);
        lighting.directional_shadow_enabled = 1u;
        lighting.directional_shadow_softness = 1.0f;
    }
    return lighting;
}

static BTBHGPUScenePost base_post(void) {
    BTBHGPUScenePost post;
    memset(&post, 0, sizeof(post));
    /* Reinhard with a white point of 1 is the identity: the target holds sRGB(linear colour), dithered. */
    post.tonemap_white = 1.0f;
    post.exposure = 1.0f;
    post.contrast = 1.0f;
    post.saturation = 1.0f;
    post.blur_curve = 1.0f;
    post.tonemap_mode = BTBH_GPU_SCENE_TONEMAP_REINHARD;
    post.upscale_mode = BTBH_GPU_SCENE_UPSCALE_LINEAR;
    return post;
}

/* What a render needs, in static storage (see the scene builders). */
#define MAX_BOXES 16u
typedef struct Scene {
    const char *name;
    SmokeCamera camera;
    BTBHGPUSceneLighting lighting;
    BTBHGPUScenePost post;
    Box boxes[MAX_BOXES];
    float instances[MAX_BOXES][24];
    uint32_t box_count;
    BTBHGPUSceneGeometryBatch batches[MAX_BOXES];
    uint32_t batch_count;
} Scene;

static void scene_add(Scene *scene, Box box, BTBHGPUSceneGeometryBatch batch) {
    assert(scene->box_count < MAX_BOXES);
    const uint32_t index = scene->box_count++;
    scene->boxes[index] = box;
    box_instance(&box, scene->instances[index]);
    batch.first_instance = index;
    batch.instance_count = 1u;
    scene->batches[scene->batch_count++] = batch;
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

static const float grey[4] = { 0.62f, 0.62f, 0.6f, 1.0f };
static const float brick[4] = { 0.75f, 0.42f, 0.32f, 1.0f };
static const float stone[4] = { 0.7f, 0.72f, 0.75f, 1.0f };
static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

/* A ground slab and a few boxes (one turned 45 degrees, one pitched 30), with the first set's textures. */
static void add_city(Scene *scene, const SmokeWindow *smoke, uint8_t unlit) {
    BTBHGPUSceneGeometryBatch batch = cube_batch(smoke->cube, 0u, 1u);
    batch.unlit_enabled = unlit;
    scene_add(scene, make_box(0.0f, -0.25f, 0.0f, 14.0f, 0.5f, 14.0f, grey), batch);
    batch.albedo_texture = smoke->checker;
    batch.normal_texture = smoke->bumps;
    batch.orm_texture = smoke->orm;
    scene_add(scene, make_box(1.0f, 0.75f, 0.6f, 1.4f, 1.5f, 1.2f, brick), batch);
    Box turned = make_box(-2.2f, 0.7f, -0.6f, 1.1f, 1.4f, 1.1f, stone);
    turned.yaw = 0.25f * PI_F;
    scene_add(scene, turned, batch);
    Box pitched = make_box(1.3f, 0.7f, -2.4f, 1.6f, 0.5f, 1.6f, stone);
    pitched.pitch = PI_F / 6.0f;
    scene_add(scene, pitched, batch);
}

/* The procedural surface detail's patterns 0..6 on the city and five more boxes (pattern_scene). */
#define PATTERN_GALLERY_FIRST 8u
#define PATTERN_VARIANTS 7u
#define PATTERN_LENS 7u
static const char *const pattern_scene_names[PATTERN_VARIANTS] = {
    "patterns-tent", "patterns-omni", "patterns-unshadowed", "patterns-rc", "patterns-rc-omni",
    "patterns-rc-unshadowed", "patterns-dry",
};

static void add_pattern_city(Scene *scene, const SmokeWindow *smoke) {
    static const float lawn[4] = { 0.35f, 0.55f, 0.25f, 1.0f };
    add_city(scene, smoke, 0u);                    /* 0 the ground, 1 a box, 2 one turned, 3 one pitched */
    const BTBHGPUSceneGeometryBatch plain = cube_batch(smoke->cube, 0u, 1u);
    scene_add(scene, make_box(-2.6f, 0.05f, 1.9f, 1.8f, 0.1f, 1.4f, lawn), plain);    /* 4 a lawn */
    scene_add(scene, make_box(2.5f, 0.04f, 1.6f, 1.6f, 0.08f, 1.8f, stone), plain);   /* 5 a sidewalk */
    scene_add(scene, make_box(0.1f, 0.2f, 2.5f, 1.2f, 0.4f, 0.5f, brick), plain);     /* 6 a low wall */
    scene_add(scene, make_box(-1.0f, 0.3f, 2.4f, 0.3f, 0.3f, 0.3f, white), plain);    /* 7 a lens */
    scene_add(scene, make_box(-3.2f, 0.6f, -2.6f, 1.0f, 1.2f, 1.0f, stone), plain);   /* 8 none */
}

/* Variant 0..5: the sun's shadow (production_model_2d_shadows.frag), the sun's and the omni shadow
   (production_model.frag), no shadow (production_model_unshadowed.frag), then the same three with the radiance
   cascades (production_model_rc_2d_shadows, _rc, _rc_unshadowed), wet in the rain at night; 6: the sun's soft
   shadow, dry by day. Lamps and point lights in each. */
static void pattern_scene(uint32_t variant, const SmokeWindow *smoke, Scene *scene) {
    scene->name = pattern_scene_names[variant];
    const uint32_t shadows = variant == 6u ? 0u : variant % 3u; /* 0 the sun's, 1 and the omni, 2 none */
    scene->lighting = base_lighting(shadows != 2u);
    BTBHGPUSceneLighting *lighting = &scene->lighting;
    if (variant == 6u) {
        lighting->directional_shadow_filter = BTBH_GPU_SCENE_SHADOW_FILTER_SOFT;
    } else {
        lighting->surface_wetness = 0.7f;
        lighting->rain = 0.5f;
        lighting->window_glow = 0.8f;
    }
    if (shadows == 1u) {
        lighting->spot_position_x = -0.5f;
        lighting->spot_position_y = 2.6f;
        lighting->spot_position_z = 1.5f;
        lighting->spot_range = 8.0f;
        lighting->spot_direction_y = -1.0f;
        lighting->spot_energy = 3.0f;
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
    lighting->lamp_count = 4u;
    lighting->lamp_core_radius = 0.4f;
    for (uint32_t lamp = 0u; lamp < 4u; ++lamp) {
        lighting->lamp_position_reach[lamp][0] = -3.0f + 2.0f * (float)lamp;
        lighting->lamp_position_reach[lamp][1] = 1.2f;
        lighting->lamp_position_reach[lamp][2] = 2.8f;
        lighting->lamp_position_reach[lamp][3] = 5.0f;
        lighting->lamp_color_energy[lamp][0] = 1.0f;
        lighting->lamp_color_energy[lamp][1] = 0.8f;
        lighting->lamp_color_energy[lamp][2] = 0.6f;
        lighting->lamp_color_energy[lamp][3] = 0.6f;
    }
    lighting->point_light_count = 3u;
    for (uint32_t light = 0u; light < 3u; ++light) {
        float *point = lighting->point_lights[light];
        point[0] = -2.0f + 2.0f * (float)light;
        point[1] = 1.0f;
        point[2] = 0.0f;
        point[3] = 3.0f;
        point[4] = 0.4f;
        point[5] = 0.6f;
        point[6] = 1.0f;
        point[7] = 0.8f;
    }
    if (variant >= 3u && variant <= 5u) {
        scene->post.radiance_cascades.tier = BTBH_GPU_SCENE_RC_LOW;
        scene->post.radiance_cascades.auto_frame_budget_milliseconds = 16.0f;
        scene->post.radiance_cascades.indirect_intensity = 0.8f;
        scene->post.radiance_cascades.hero_omni_emitter_index = -1;
        scene->post.radiance_cascades.invalidate = 1u;
    }
    add_pattern_city(scene, smoke);
    static const float patterns[9][4] = {
        { 0.2f, 0.02f, 3.0f, 1.0f },   /* the ground */
        { 0.3f, 0.03f, 4.0f, 3.0f },   /* a facade, on the first set's textures */
        { 0.2f, 0.02f, 2.0f, 4.0f },   /* paving */
        { 0.3f, 0.0f, 3.0f, 5.0f },    /* glass, pitched */
        { 0.25f, 0.004f, 3.0f, 2.0f }, /* grass */
        { 0.15f, 0.01f, 2.5f, 4.0f },  /* paving */
        { 0.07f, 0.02f, 6.0f, 3.0f },  /* a facade at street level */
        { 0.0f, 0.0f, 1.0f, 6.0f },    /* a lamp's lens */
        { 0.0f, 0.0f, 0.0f, 0.0f },    /* none */
    };
    for (uint32_t batch = 0u; batch < scene->batch_count; ++batch) {
        memcpy(scene->batches[batch].detail_parameters, patterns[batch], sizeof(patterns[batch]));
        scene->batches[batch].surface_time = 1.3f;
    }
}

static SmokeCamera gallery_camera(void) {
    const float eye[3] = { 3.6f, 3.0f, 4.6f };
    const float centre[3] = { -0.2f, 0.5f, -0.2f };
    return look_at(eye, centre, 0.9f, 1.0f);
}

/* Gallery scene `index` (0..GALLERY_COUNT-1). */
#define GALLERY_COUNT (PATTERN_GALLERY_FIRST + PATTERN_VARIANTS)
static void gallery_scene(uint32_t index, const SmokeWindow *smoke, Scene *scene) {
    memset(scene, 0, sizeof(*scene));
    scene->camera = gallery_camera();
    scene->post = base_post();
    switch (index) {
        case 0u: /* The sun's shadow (tent): production_model_2d_shadows.frag. */
            scene->name = "sun-tent";
            scene->lighting = base_lighting(true);
            add_city(scene, smoke, 0u);
            break;
        case 1u: { /* The soft sun shadow, lamps, point lights, the spot light, wet ground in the rain and the
                      procedural detail (ground, facade, paving, glass, a lamp's lens). */
            scene->name = "sun-soft";
            scene->lighting = base_lighting(true);
            BTBHGPUSceneLighting *lighting = &scene->lighting;
            lighting->directional_shadow_filter = BTBH_GPU_SCENE_SHADOW_FILTER_SOFT;
            lighting->lamp_count = 4u;
            lighting->lamp_core_radius = 0.4f;
            for (uint32_t lamp = 0u; lamp < 4u; ++lamp) {
                lighting->lamp_position_reach[lamp][0] = -3.0f + 2.0f * (float)lamp;
                lighting->lamp_position_reach[lamp][1] = 1.2f;
                lighting->lamp_position_reach[lamp][2] = 2.5f;
                lighting->lamp_position_reach[lamp][3] = 5.0f;
                lighting->lamp_color_energy[lamp][0] = 1.0f;
                lighting->lamp_color_energy[lamp][1] = 0.8f;
                lighting->lamp_color_energy[lamp][2] = 0.6f;
                lighting->lamp_color_energy[lamp][3] = 0.6f;
            }
            lighting->point_light_count = 3u;
            for (uint32_t light = 0u; light < 3u; ++light) {
                float *point = lighting->point_lights[light];
                point[0] = -2.0f + 2.0f * (float)light;
                point[1] = 1.0f;
                point[2] = 0.0f;
                point[3] = 3.0f;
                point[4] = 0.4f;
                point[5] = 0.6f;
                point[6] = 1.0f;
                point[7] = 0.8f;
            }
            lighting->spot_position_x = 0.0f;
            lighting->spot_position_y = 4.0f;
            lighting->spot_position_z = 2.0f;
            lighting->spot_range = 9.0f;
            lighting->spot_direction_y = -1.0f;
            lighting->spot_energy = 1.5f;
            lighting->spot_red = 1.0f;
            lighting->spot_green = 0.9f;
            lighting->spot_blue = 0.7f;
            lighting->spot_inner_cutoff = 0.95f;
            lighting->spot_outer_cutoff = 0.8f;
            lighting->surface_wetness = 0.6f;
            lighting->rain = 0.5f;
            lighting->window_glow = 0.8f;
            add_city(scene, smoke, 0u);
            const float patterns[4][4] = {
                { 0.2f, 0.02f, 3.0f, 1.0f }, /* ground */
                { 0.3f, 0.03f, 4.0f, 3.0f }, /* facade */
                { 0.2f, 0.02f, 2.0f, 4.0f }, /* paving */
                { 0.3f, 0.0f, 3.0f, 5.0f },  /* glass */
            };
            for (uint32_t batch = 0u; batch < 4u; ++batch) {
                memcpy(scene->batches[batch].detail_parameters, patterns[batch], sizeof(patterns[batch]));
                scene->batches[batch].surface_time = 1.3f;
            }
            BTBHGPUSceneGeometryBatch lens = cube_batch(smoke->cube, 0u, 1u);
            lens.detail_parameters[2] = 1.0f;
            lens.detail_parameters[3] = 6.0f;
            scene_add(scene, make_box(-1.0f, 0.3f, 2.4f, 0.3f, 0.3f, 0.3f, white), lens);
            break;
        }
        case 2u: /* The omni shadow beside the sun's: production_model.frag. */
            scene->name = "omni";
            scene->lighting = base_lighting(true);
            scene->lighting.spot_position_x = -0.5f;
            scene->lighting.spot_position_y = 2.6f;
            scene->lighting.spot_position_z = 1.5f;
            scene->lighting.spot_range = 8.0f;
            scene->lighting.spot_direction_y = -1.0f;
            scene->lighting.spot_energy = 3.0f;
            scene->lighting.spot_red = 1.0f;
            scene->lighting.spot_green = 0.7f;
            scene->lighting.spot_blue = 0.4f;
            scene->lighting.spot_inner_cutoff = 0.2f;
            scene->lighting.spot_outer_cutoff = -1.0f;
            scene->lighting.spot_shadow_enabled = 1u;
            scene->lighting.spot_shadow_is_omni = 1u;
            scene->lighting.spot_shadow_softness = 1.0f;
            scene->lighting.spot_shadow_near = 0.1f;
            add_city(scene, smoke, 0u);
            break;
        case 3u: { /* No shadow, a second light and image-based lighting: production_model_unshadowed.frag. */
            scene->name = "unshadowed";
            scene->lighting = base_lighting(false);
            BTBHGPUSceneLighting *lighting = &scene->lighting;
            lighting->secondary_directional_x = 0.5f;
            lighting->secondary_directional_y = -0.3f;
            lighting->secondary_directional_z = -1.0f;
            lighting->secondary_directional_energy = 0.6f;
            lighting->secondary_directional_red = 0.5f;
            lighting->secondary_directional_green = 0.6f;
            lighting->secondary_directional_blue = 1.0f;
            lighting->ibl_enabled = 1u;
            lighting->sky_top_red = 0.3f;
            lighting->sky_top_green = 0.5f;
            lighting->sky_top_blue = 0.9f;
            lighting->sky_energy = 0.8f;
            lighting->sky_horizon_red = 0.7f;
            lighting->sky_horizon_green = 0.75f;
            lighting->sky_horizon_blue = 0.8f;
            lighting->sky_horizon_curve = 0.5f;
            lighting->ground_bottom_red = 0.2f;
            lighting->ground_bottom_green = 0.18f;
            lighting->ground_bottom_blue = 0.15f;
            lighting->ground_energy = 0.5f;
            lighting->ground_horizon_red = 0.4f;
            lighting->ground_horizon_green = 0.4f;
            lighting->ground_horizon_blue = 0.4f;
            lighting->ground_horizon_curve = 0.5f;
            lighting->sun_red = 1.0f;
            lighting->sun_green = 0.9f;
            lighting->sun_blue = 0.8f;
            lighting->sun_energy = 1.0f;
            lighting->sun_direction_x = -0.4f;
            lighting->sun_direction_y = -0.8f;
            lighting->sun_direction_z = -0.3f;
            lighting->sun_size_radians = 0.05f;
            lighting->sun_curve = 1.0f;
            add_city(scene, smoke, 0u);
            scene->batches[0].detail_parameters[0] = 0.25f;
            scene->batches[0].detail_parameters[2] = 3.0f;
            scene->batches[0].detail_parameters[3] = 2.0f; /* grass */
            scene->batches[1].emission_texture = smoke->glow;
            scene->batches[1].emission_red = 255u;
            scene->batches[1].emission_green = 255u;
            scene->batches[1].emission_blue = 255u;
            scene->batches[1].emission_energy = 2.0f;
            scene->batches[2].metalness = 1.0f;
            scene->batches[2].roughness = 0.25f;
            break;
        }
        case 4u: { /* Unlit batches: plain, darkened in the sun's shadow (2, 3), relief from brightness, and a
                      see-through one through the fast unlit pipeline; a dark scene with flashes. */
            scene->name = "unlit";
            scene->lighting = base_lighting(true);
            scene->lighting.unlit_darkness = 0.3f;
            scene->lighting.unlit_glow_threshold = 0.7f;
            scene->lighting.point_light_count = 1u;
            scene->lighting.point_lights[0][1] = 1.5f;
            scene->lighting.point_lights[0][3] = 4.0f;
            scene->lighting.point_lights[0][4] = 1.0f;
            scene->lighting.point_lights[0][5] = 0.5f;
            scene->lighting.point_lights[0][6] = 0.2f;
            scene->lighting.point_lights[0][7] = 1.0f;
            add_city(scene, smoke, 1u);
            scene->batches[1].unlit_enabled = 2u;
            scene->batches[2].unlit_enabled = 3u;
            scene->batches[3].normal_scale = -2.0f;
            BTBHGPUSceneGeometryBatch glass = cube_batch(smoke->cube, 0u, 1u);
            glass.unlit_enabled = 1u;
            glass.transparency_enabled = 1u;
            glass.albedo_texture = smoke->checker;
            const float tint[4] = { 0.4f, 0.8f, 1.0f, 0.5f };
            scene_add(scene, make_box(-0.5f, 0.8f, 2.0f, 1.0f, 1.0f, 1.0f, tint), glass);
            break;
        }
        case 5u: { /* See-through lit batches (alpha, additive, premultiplied). */
            scene->name = "transparent";
            scene->lighting = base_lighting(true);
            add_city(scene, smoke, 0u);
            const float tints[3][4] = {
                { 0.9f, 0.9f, 0.2f, 0.5f }, { 0.2f, 0.9f, 0.9f, 0.6f }, { 0.9f, 0.3f, 0.6f, 0.7f } };
            const BTBHGPUSceneBlendMode blends[3] = {
                BTBH_GPU_SCENE_BLEND_ALPHA, BTBH_GPU_SCENE_BLEND_ADDITIVE, BTBH_GPU_SCENE_BLEND_PREMULTIPLIED_ALPHA };
            for (uint32_t layer = 0u; layer < 3u; ++layer) {
                BTBHGPUSceneGeometryBatch see = cube_batch(smoke->cube, 0u, 1u);
                see.transparency_enabled = 1u;
                see.blend_mode = blends[layer];
                see.normal_texture = smoke->bumps;
                scene_add(scene, make_box(-1.5f + 1.2f * (float)layer, 0.8f, 2.2f, 0.9f, 0.9f, 0.3f, tints[layer]),
                    see);
            }
            break;
        }
        case 6u: { /* The surface variants: water, dissolve, shatter, glitter, rim. */
            scene->name = "surfaces";
            scene->lighting = base_lighting(true);
            add_city(scene, smoke, 0u);
            scene->batches[0].surface_variant = BTBH_GPU_SCENE_SURFACE_WATER;
            scene->batches[0].surface_time = 2.0f;
            scene->batches[1].surface_variant = BTBH_GPU_SCENE_SURFACE_DISSOLVE;
            scene->boxes[1].custom[0] = 0.4f;
            scene->boxes[1].custom[1] = 3.0f;
            scene->boxes[1].custom[2] = 1.0f;
            box_instance(&scene->boxes[1], scene->instances[1]);
            scene->batches[2].surface_variant = BTBH_GPU_SCENE_SURFACE_GLITTER;
            scene->boxes[2].custom[0] = 0.6f;
            box_instance(&scene->boxes[2], scene->instances[2]);
            scene->batches[3].surface_variant = BTBH_GPU_SCENE_SURFACE_RIM;
            scene->batches[3].surface_parameters0[0] = 0.2f;
            scene->batches[3].surface_parameters0[1] = 0.6f;
            scene->batches[3].surface_parameters0[2] = 1.0f;
            scene->batches[3].surface_parameters0[3] = 1.0f;
            BTBHGPUSceneGeometryBatch shatter = cube_batch(smoke->cube, 0u, 1u);
            shatter.surface_variant = BTBH_GPU_SCENE_SURFACE_SHATTER;
            shatter.surface_time = 1.0f;
            Box falling = make_box(-0.6f, 0.6f, 2.2f, 1.0f, 1.0f, 1.0f, brick);
            falling.custom[1] = 0.3f;
            falling.custom[2] = 5.0f;
            falling.custom[3] = 1.0f;
            scene_add(scene, falling, shatter);
            break;
        }
        case 7u: { /* The radiance cascades (production_model_rc*.frag), where they run: an emissive box and an
                      unlit source. */
            scene->name = "rc";
            scene->lighting = base_lighting(true);
            add_city(scene, smoke, 0u);
            scene->batches[1].emission_texture = smoke->glow;
            scene->batches[1].emission_red = 255u;
            scene->batches[1].emission_green = 255u;
            scene->batches[1].emission_blue = 255u;
            scene->batches[1].emission_energy = 4.0f;
            BTBHGPUSceneGeometryBatch source = cube_batch(smoke->cube, 0u, 1u);
            source.unlit_enabled = 1u;
            const float hot[4] = { 1.0f, 0.4f, 0.1f, 1.0f };
            scene_add(scene, make_box(-0.8f, 0.3f, 2.0f, 0.6f, 0.6f, 0.6f, hot), source);
            scene->post.radiance_cascades.tier = BTBH_GPU_SCENE_RC_LOW;
            scene->post.radiance_cascades.auto_frame_budget_milliseconds = 16.0f;
            scene->post.radiance_cascades.indirect_intensity = 0.8f;
            scene->post.radiance_cascades.hero_omni_emitter_index = -1;
            scene->post.radiance_cascades.invalidate = 1u;
            break;
        }
        default: /* The procedural surface detail's patterns through each variant. */
            pattern_scene(index - PATTERN_GALLERY_FIRST, smoke, scene);
            break;
    }
}

static uint8_t *render_scene(
    SmokeWindow *smoke,
    const ShaderSet *shaders,
    BTBHGPUTexture *target,
    const Scene *scene,
    const BTBHGPUSceneGeometryBatch *batches,
    BTBHGPUResult expected,
    BTBHGPUSceneReport *report
) {
    const ShaderSet *set = shaders != NULL ? shaders : &smoke->shaders;
    const BTBHGPUResult result = btbh_gpu_window_scene_render_to_target(
        smoke->window,
        target,
        set->paths[0], set->paths[1], set->paths[2], set->paths[3],
        set->paths[4], set->paths[5], set->paths[6], set->paths[7],
        set->paths[8], set->paths[9], set->paths[10],
        SMOKE_SHADER_FORMAT,
        target->width,
        target->height,
        scene->camera.view_projection,
        scene->camera.right,
        scene->camera.up,
        scene->camera.position,
        &scene->lighting,
        &scene->post,
        &scene->instances[0][0],
        scene->box_count,
        scene->box_count,
        batches != NULL ? batches : scene->batches,
        NULL,
        NULL,
        scene->batch_count,
        12u, 14u, 20u, 255u,
        report
    );
    if (result != expected) {
        fprintf(stderr, "%s: %d (expected %d): %s\n", scene->name, (int)result, (int)expected, report->message);
    }
    assert(result == expected);
    return result == BTBH_GPU_OK ? read_texture(smoke->window, target) : NULL;
}

/* ----------------------------------------------------------------------------------------------- the gallery */

static bool load_png(const char *path, uint32_t size, uint8_t *pixels) {
    SDL_Surface *surface = SDL_LoadPNG(path);
    if (surface == NULL) {
        return false;
    }
    SDL_Surface *rgba = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_ABGR8888);
    SDL_DestroySurface(surface);
    if (rgba == NULL || (uint32_t)rgba->w != size || (uint32_t)rgba->h != size) {
        SDL_DestroySurface(rgba);
        return false;
    }
    for (uint32_t y = 0u; y < size; ++y) {
        memcpy(&pixels[(size_t)y * size * 4u], (const uint8_t *)rgba->pixels + (size_t)y * (size_t)rgba->pitch,
            (size_t)size * 4u);
    }
    SDL_DestroySurface(rgba);
    return true;
}

static void check_gallery(SmokeWindow *smoke, SmokeWindow *baseline, uint32_t *references_compared) {
    const char *dump = SDL_getenv("BTBH_WORLD_DETAIL_DUMP");
    const char *reference_dir = SDL_getenv("BTBH_WORLD_DETAIL_REFERENCE");
    const uint32_t texels = GALLERY_SIZE * GALLERY_SIZE;
    const size_t bytes = (size_t)texels * 4u;
    static Scene scene;
    *references_compared = 0u;
    for (uint32_t index = 0u; index < GALLERY_COUNT; ++index) {
        gallery_scene(index, smoke, &scene);
        BTBHGPUSceneReport report;
        uint8_t *plain = render_scene(smoke, NULL, smoke->gallery, &scene, NULL, BTBH_GPU_OK, &report);
        const bool rc = report.radiance_cascades.active_tier != BTBH_GPU_SCENE_RC_OFF;
        uint8_t *again = render_scene(smoke, NULL, smoke->gallery, &scene, NULL, BTBH_GPU_OK, &report);
        assert(memcmp(plain, again, bytes) == 0);
        free(again);
        printf("gallery %-12s rendered%s", scene.name, rc ? " (radiance cascades on)" : "");
#if defined(BTBH_GPU_SCENE_HAS_WORLD_DETAIL)
        {
            /* A detail without textures on every batch: as with none. */
            BTBHGPUSceneWorldDetail empty;
            memset(&empty, 0, sizeof(empty));
            empty.level_tile[0] = empty.level_tile[1] = 2.0f;
            empty.wall_tile[0] = empty.wall_tile[1] = 1.5f;
            empty.seed_component = 4u;
            BTBHGPUSceneGeometryBatch batches[MAX_BOXES];
            memcpy(batches, scene.batches, sizeof(batches));
            for (uint32_t batch = 0u; batch < scene.batch_count; ++batch) {
                batches[batch].world_detail = &empty;
            }
            uint8_t *empty_image = render_scene(smoke, NULL, smoke->gallery, &scene, batches, BTBH_GPU_OK, &report);
            const uint32_t differing = differing_pixels(plain, empty_image, texels, 0, NULL);
            printf("; a detail without textures: %u pixels differ", differing);
            assert(differing == 0u);
            free(empty_image);
        }
#endif
        if (baseline != NULL) {
            gallery_scene(index, baseline, &scene);
            uint8_t *old = render_scene(baseline, NULL, baseline->gallery, &scene, NULL, BTBH_GPU_OK, &report);
            const uint32_t differing = differing_pixels(plain, old, texels, 0, NULL);
            printf("; baseline shaders: %u pixels differ", differing);
            assert(differing == 0u);
            free(old);
        }
        if (reference_dir != NULL && reference_dir[0] != 0) {
            char path[1024];
            (void)snprintf(path, sizeof(path), "%s/unset-%s.png", reference_dir, scene.name);
            uint8_t *reference = malloc(bytes);
            assert(reference != NULL);
            const bool loaded = load_png(path, GALLERY_SIZE, reference);
            if (!loaded) {
                fprintf(stderr, "\nno reference %s\n", path);
            }
            assert(loaded);
            const uint32_t differing = differing_pixels(plain, reference, texels, 0, NULL);
            printf("; reference: %u pixels differ", differing);
            assert(differing == 0u);
            *references_compared += 1u;
            free(reference);
        }
        printf("\n");
        (void)fflush(stdout);
        if (dump != NULL && dump[0] != 0) {
            char path[1024];
            (void)snprintf(path, sizeof(path), "%s/unset-%s.png", dump, scene.name);
            assert(btbh_gpu_write_png_rgba8(path, GALLERY_SIZE, GALLERY_SIZE, plain) == BTBH_GPU_OK);
        }
        free(plain);
    }
}

#if defined(BTBH_GPU_SCENE_HAS_WORLD_DETAIL)
/* ---------------------------------------------------------------------------- set: the projection read back */

/* The code texture: column x in red, row y in green, as 40 + 6 x (linear), nearest, no mips. */
#define CODE_TEXELS 32u
#define CODE_BASE 40.0
#define CODE_STEP 6.0

static uint8_t code_texel(uint32_t x, uint32_t y, uint32_t channel) {
    switch (channel) {
        case 0: return (uint8_t)(CODE_BASE + CODE_STEP * (double)x);
        case 1: return (uint8_t)(CODE_BASE + CODE_STEP * (double)y);
        case 2: return 200u;
        default: return 255u;
    }
}

/* The texel index a channel shows: the target holds linear_to_srgb(the albedo multiplier) (production_post.frag,
   Reinhard with a white point of 1 being the identity), dithered by half a step. -1 when it is no code value. */
static int decode_channel(uint8_t value) {
    const double srgb = (double)value / 255.0;
    const double linear = pow((srgb + 0.055) / 1.055, 2.4) * 255.0;
    const double index = (linear - CODE_BASE) / CODE_STEP;
    const double rounded = floor(index + 0.5);
    if (rounded < 0.0 || rounded > (double)(CODE_TEXELS - 1u) || fabs(index - rounded) > 0.45) {
        return -1;
    }
    return (int)rounded;
}

static int cyclic_distance(int a, int b) {
    const int d = abs(a - b) % (int)CODE_TEXELS;
    return d < (int)CODE_TEXELS - d ? d : (int)CODE_TEXELS - d;
}

static bool invert4(const float m[16], double out[16]) {
    double a[4][8];
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            a[row][column] = m[column * 4 + row];
            a[row][column + 4] = row == column ? 1.0 : 0.0;
        }
    }
    for (int column = 0; column < 4; ++column) {
        int pivot = column;
        for (int row = column + 1; row < 4; ++row) {
            if (fabs(a[row][column]) > fabs(a[pivot][column])) {
                pivot = row;
            }
        }
        if (fabs(a[pivot][column]) < 1.0e-12) {
            return false;
        }
        for (int k = 0; k < 8; ++k) {
            const double swap = a[column][k];
            a[column][k] = a[pivot][k];
            a[pivot][k] = swap;
        }
        const double scale = 1.0 / a[column][column];
        for (int k = 0; k < 8; ++k) {
            a[column][k] *= scale;
        }
        for (int row = 0; row < 4; ++row) {
            if (row != column) {
                const double factor = a[row][column];
                for (int k = 0; k < 8; ++k) {
                    a[row][k] -= factor * a[column][k];
                }
            }
        }
    }
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            out[column * 4 + row] = a[row][column + 4];
        }
    }
    return true;
}

typedef struct Ray {
    double origin[3];
    double direction[3];
} Ray;

/* The ray through pixel (x, y)'s centre (row 0 at the top, NDC y up, depth 0..1). */
static Ray pixel_ray(const double inverse[16], uint32_t x, uint32_t y, uint32_t size) {
    const double ndc[2] = {
        ((double)x + 0.5) / (double)size * 2.0 - 1.0,
        1.0 - ((double)y + 0.5) / (double)size * 2.0,
    };
    double points[2][3];
    for (int end = 0; end < 2; ++end) {
        const double clip[4] = { ndc[0], ndc[1], (double)end, 1.0 };
        double world[4];
        for (int row = 0; row < 4; ++row) {
            world[row] = 0.0;
            for (int column = 0; column < 4; ++column) {
                world[row] += inverse[column * 4 + row] * clip[column];
            }
        }
        for (int axis = 0; axis < 3; ++axis) {
            points[end][axis] = world[axis] / world[3];
        }
    }
    Ray ray;
    double length = 0.0;
    for (int axis = 0; axis < 3; ++axis) {
        ray.origin[axis] = points[0][axis];
        ray.direction[axis] = points[1][axis] - points[0][axis];
        length += ray.direction[axis] * ray.direction[axis];
    }
    length = sqrt(length);
    for (int axis = 0; axis < 3; ++axis) {
        ray.direction[axis] /= length;
    }
    return ray;
}

/* The ray's first hit on the box: its distance, the point, the face (0..5) and its outward normal. */
static bool hit_box(const Box *box, const Ray *ray, double *distance, float position[3], int *face, float normal[3]) {
    float r[3][3];
    box_rotation(box, r);
    double origin[3];
    double direction[3];
    for (int axis = 0; axis < 3; ++axis) {
        origin[axis] = 0.0;
        direction[axis] = 0.0;
        for (int k = 0; k < 3; ++k) {
            origin[axis] += (double)r[k][axis] * (ray->origin[k] - (double)box->centre[k]);
            direction[axis] += (double)r[k][axis] * ray->direction[k];
        }
        origin[axis] /= (double)box->scale[axis];
        direction[axis] /= (double)box->scale[axis];
    }
    double enter = -1.0e30;
    double leave = 1.0e30;
    int enter_axis = -1;
    double enter_sign = 0.0;
    for (int axis = 0; axis < 3; ++axis) {
        if (fabs(direction[axis]) < 1.0e-15) {
            if (fabs(origin[axis]) > 0.5) {
                return false;
            }
            continue;
        }
        const double a = (-0.5 - origin[axis]) / direction[axis];
        const double b = (0.5 - origin[axis]) / direction[axis];
        const double lower = a < b ? a : b;
        const double upper = a < b ? b : a;
        if (lower > enter) {
            enter = lower;
            enter_axis = axis;
            enter_sign = direction[axis] > 0.0 ? -1.0 : 1.0;
        }
        leave = upper < leave ? upper : leave;
    }
    if (enter_axis < 0 || enter > leave || enter <= 0.0) {
        return false;
    }
    *distance = enter;
    for (int axis = 0; axis < 3; ++axis) {
        position[axis] = (float)(ray->origin[axis] + enter * ray->direction[axis]);
    }
    *face = enter_axis * 2 + (enter_sign > 0.0 ? 0 : 1);
    float local[3] = { 0.0f, 0.0f, 0.0f };
    local[enter_axis] = (float)enter_sign / box->scale[enter_axis];
    for (int axis = 0; axis < 3; ++axis) {
        normal[axis] = r[axis][0] * local[0] + r[axis][1] * local[1] + r[axis][2] * local[2];
    }
    normalize3(normal);
    return true;
}

enum { CLASS_LEVEL_UP, CLASS_LEVEL_DOWN, CLASS_WALL, CLASS_WALL_45, CLASS_PITCHED, CLASS_COUNT };
static const char *const class_names[CLASS_COUNT] = {
    "level faces from above", "level faces from below", "vertical walls", "45 degree walls", "pitched faces"
};

typedef struct ProjectionTally {
    uint32_t checked[CLASS_COUNT];
    uint32_t matched[CLASS_COUNT];
    uint32_t box_checked[MAX_BOXES];
    uint32_t box_matched[MAX_BOXES];
} ProjectionTally;

static const float projection_level_tile[2] = { 3.0f, 2.5f };
static const float projection_wall_tile[2] = { 1.5f, 1.0f };

/* The projection scene: unlit white boxes over a ground slab, each with its seed in custom w. */
static void projection_scene(const SmokeWindow *smoke, const BTBHGPUSceneWorldDetail *detail, Scene *scene) {
    memset(scene, 0, sizeof(*scene));
    scene->name = "projection";
    scene->lighting = base_lighting(false);
    scene->post = base_post();
    BTBHGPUSceneGeometryBatch batch = cube_batch(smoke->cube, 0u, 1u);
    batch.unlit_enabled = 1u;
    batch.world_detail = detail;
    const struct {
        float centre[3];
        float scale[3];
        float yaw;
        float pitch;
        float seed;
    } boxes[] = {
        { { 0.0f, -0.25f, 0.0f }, { 14.0f, 0.5f, 14.0f }, 0.0f, 0.0f, 0.0f },     /* 0 ground */
        { { 1.0f, 0.75f, 0.6f }, { 1.4f, 1.5f, 1.2f }, 0.0f, 0.0f, 7.0f },       /* 1 A */
        { { 2.2f, 0.75f, 0.6f }, { 1.0f, 1.5f, 1.2f }, 0.0f, 0.0f, 7.0f },       /* 2 E: touches A, A's seed */
        { { -0.1f, 0.6f, 0.6f }, { 0.8f, 1.2f, 1.2f }, 0.0f, 0.0f, 99.0f },      /* 3 F: touches A, another seed */
        { { -2.2f, 0.7f, -0.6f }, { 1.1f, 1.4f, 1.1f }, 0.25f * PI_F, 0.0f, 1234.0f }, /* 4 B: 45 degrees */
        { { 1.3f, 0.7f, -2.4f }, { 1.6f, 0.5f, 1.6f }, 0.0f, PI_F / 6.0f, 5.0f }, /* 5 C: a 30 degree pitch */
        { { -1.2f, 2.2f, 2.0f }, { 1.0f, 0.3f, 1.0f }, 0.0f, 0.0f, 3.0f },        /* 6 D: floating */
    };
    for (uint32_t index = 0u; index < sizeof(boxes) / sizeof(boxes[0]); ++index) {
        Box box = make_box(boxes[index].centre[0], boxes[index].centre[1], boxes[index].centre[2],
            boxes[index].scale[0], boxes[index].scale[1], boxes[index].scale[2], white);
        box.yaw = boxes[index].yaw;
        box.pitch = boxes[index].pitch;
        box.custom[3] = boxes[index].seed;
        scene_add(scene, box, batch);
    }
}

static SmokeCamera projection_camera(uint32_t index) {
    static const float eyes[4][3] = {
        { 4.2f, 3.4f, 5.6f },    /* front right, above: +z and +x walls, roofs, the pitch, the 45 degree wall */
        { -4.6f, 3.0f, -4.8f },  /* back left: -z and -x walls */
        { 0.0f, 11.0f, 2.2f },   /* from above, north (-z) up the screen */
        { -0.4f, 0.5f, 5.2f },   /* low, under the floating slab */
    };
    static const float centres[4][3] = {
        { 0.0f, 0.6f, -0.4f }, { 0.0f, 0.6f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { -1.1f, 1.9f, 1.8f },
    };
    return look_at(eyes[index], centres[index], index == 3u ? 0.75f : 0.9f, 1.0f);
}

/* Renders the projection scene from `camera_index` and checks each interior pixel's texel against the C copy. Keeps
   the decoded texels of box `watch` (or all of them) in `decoded` when given. */
static void check_projection_view(
    SmokeWindow *smoke,
    Scene *scene,
    uint32_t camera_index,
    ProjectionTally *tally,
    int16_t *decoded
) {
    scene->camera = projection_camera(camera_index);
    BTBHGPUSceneReport report;
    uint8_t *pixels = render_scene(smoke, NULL, smoke->projection, scene, NULL, BTBH_GPU_OK, &report);
    double inverse[16];
    assert(invert4(scene->camera.view_projection, inverse));
    const uint32_t size = PROJECTION_SIZE;
    static int ids[PROJECTION_SIZE * PROJECTION_SIZE];
    static float hits[PROJECTION_SIZE * PROJECTION_SIZE][6];
    for (uint32_t y = 0u; y < size; ++y) {
        for (uint32_t x = 0u; x < size; ++x) {
            const Ray ray = pixel_ray(inverse, x, y, size);
            double nearest = 1.0e30;
            int id = -1;
            float *hit = hits[y * size + x];
            for (uint32_t box = 0u; box < scene->box_count; ++box) {
                double distance;
                float position[3];
                float normal[3];
                int face;
                if (hit_box(&scene->boxes[box], &ray, &distance, position, &face, normal) && distance < nearest) {
                    nearest = distance;
                    id = (int)box * 6 + face;
                    memcpy(hit, position, sizeof(position));
                    memcpy(hit + 3, normal, sizeof(normal));
                }
            }
            ids[y * size + x] = id;
        }
    }
    const float cos_level = btbh_gpu_world_detail_cos_level(0.0f);
    const float inverse_tiles[4] = {
        1.0f / projection_level_tile[0], 1.0f / projection_level_tile[1],
        1.0f / projection_wall_tile[0], 1.0f / projection_wall_tile[1],
    };
    for (uint32_t y = 1u; y + 1u < size; ++y) {
        for (uint32_t x = 1u; x + 1u < size; ++x) {
            const uint32_t index = y * size + x;
            const int id = ids[index];
            if (decoded != NULL) {
                decoded[index * 2u] = -1;
                decoded[index * 2u + 1u] = -1;
            }
            /* Interior pixels only: the face's edges may round either way. */
            if (id < 0 || ids[index - 1u] != id || ids[index + 1u] != id || ids[index - size] != id ||
                ids[index + size] != id) {
                continue;
            }
            const uint32_t box = (uint32_t)id / 6u;
            const float *position = hits[index];
            const float *normal = hits[index] + 3;
            const float to_camera[3] = {
                scene->camera.position[0] - position[0],
                scene->camera.position[1] - position[1],
                scene->camera.position[2] - position[2],
            };
            BTBHGPUWorldDetailFrame frame;
            btbh_gpu_world_detail_frame(position, normal, normal, to_camera, cos_level, inverse_tiles, &frame);
            float offset[2];
            btbh_gpu_world_detail_offset(scene->boxes[box].custom, 4u, offset);
            float coordinate[2];
            btbh_gpu_world_detail_coordinate(&frame, offset, coordinate);
            const int expected[2] = {
                (int)floorf((coordinate[0] - floorf(coordinate[0])) * (float)CODE_TEXELS) % (int)CODE_TEXELS,
                (int)floorf((coordinate[1] - floorf(coordinate[1])) * (float)CODE_TEXELS) % (int)CODE_TEXELS,
            };
            const int shown[2] = { decode_channel(pixels[index * 4u]), decode_channel(pixels[index * 4u + 1u]) };
            if (decoded != NULL) {
                decoded[index * 2u] = (int16_t)shown[0];
                decoded[index * 2u + 1u] = (int16_t)shown[1];
            }
            int category;
            if (frame.level) {
                category = frame.normal[1] > 0.0f ? CLASS_LEVEL_UP : CLASS_LEVEL_DOWN;
            } else if (fabsf(frame.normal[1]) > 0.01f) {
                category = CLASS_PITCHED;
            } else {
                category = fabsf(frame.normal[0]) > 0.3f && fabsf(frame.normal[2]) > 0.3f ? CLASS_WALL_45 : CLASS_WALL;
            }
            const bool match = shown[0] >= 0 && shown[1] >= 0 &&
                cyclic_distance(shown[0], expected[0]) <= 1 && cyclic_distance(shown[1], expected[1]) <= 1;
            tally->checked[category] += 1u;
            tally->box_checked[box] += 1u;
            if (match) {
                tally->matched[category] += 1u;
                tally->box_matched[box] += 1u;
            }
        }
    }
    const char *dump = SDL_getenv("BTBH_WORLD_DETAIL_DUMP");
    if (dump != NULL && dump[0] != 0) {
        char path[1024];
        (void)snprintf(path, sizeof(path), "%s/projection-%u.png", dump, camera_index);
        assert(btbh_gpu_write_png_rgba8(path, size, size, pixels) == BTBH_GPU_OK);
    }
    free(pixels);
}

static void check_projection(SmokeWindow *smoke) {
    BTBHGPUSceneWorldDetail detail;
    memset(&detail, 0, sizeof(detail));
    detail.albedo_texture = make_texture(smoke->window, CODE_TEXELS, code_texel, 0u, BTBH_GPU_TEXTURE_FILTER_NEAREST,
        BTBH_GPU_TEXTURE_COLOR_SPACE_LINEAR);
    detail.level_tile[0] = projection_level_tile[0];
    detail.level_tile[1] = projection_level_tile[1];
    detail.wall_tile[0] = projection_wall_tile[0];
    detail.wall_tile[1] = projection_wall_tile[1];
    detail.seed_component = 4u;
    static Scene scene;
    projection_scene(smoke, &detail, &scene);
    ProjectionTally tally;
    memset(&tally, 0, sizeof(tally));
    for (uint32_t camera = 0u; camera < 4u; ++camera) {
        check_projection_view(smoke, &scene, camera, &tally, NULL);
    }
    static const uint32_t least[CLASS_COUNT] = { 2000u, 150u, 1500u, 300u, 300u };
    for (uint32_t category = 0u; category < CLASS_COUNT; ++category) {
        const double share = tally.checked[category] > 0u
            ? (double)tally.matched[category] / (double)tally.checked[category]
            : 0.0;
        printf("projection, %-23s %5u of %5u pixels show the C copy's texel (%.2f%%)\n", class_names[category],
            tally.matched[category], tally.checked[category], 100.0 * share);
        assert(tally.checked[category] >= least[category]);
        assert(share >= 0.98);
    }
    /* Two touching boxes with one seed (A, E) continue one pattern across their seam; F (another seed) doesn't. */
    float offsets[3][2];
    btbh_gpu_world_detail_offset(scene.boxes[1].custom, 4u, offsets[0]);
    btbh_gpu_world_detail_offset(scene.boxes[2].custom, 4u, offsets[1]);
    btbh_gpu_world_detail_offset(scene.boxes[3].custom, 4u, offsets[2]);
    assert(offsets[0][0] == offsets[1][0] && offsets[0][1] == offsets[1][1]);
    assert(fabsf(offsets[0][0] - offsets[2][0]) > 0.1f || fabsf(offsets[0][1] - offsets[2][1]) > 0.1f);
    for (uint32_t box = 1u; box <= 3u; ++box) {
        assert(tally.box_checked[box] >= 200u);
        assert((double)tally.box_matched[box] >= 0.98 * (double)tally.box_checked[box]);
    }

    /* One box with two seeds: box A drawn with 7, then with 99, from the front. Its texels move by the offsets'
       difference: most pixels show another texel, and both match the C copy. */
    static int16_t first[PROJECTION_SIZE * PROJECTION_SIZE * 2];
    static int16_t second[PROJECTION_SIZE * PROJECTION_SIZE * 2];
    ProjectionTally seven;
    ProjectionTally ninety_nine;
    memset(&seven, 0, sizeof(seven));
    memset(&ninety_nine, 0, sizeof(ninety_nine));
    check_projection_view(smoke, &scene, 0u, &seven, first);
    scene.boxes[1].custom[3] = 99.0f;
    box_instance(&scene.boxes[1], scene.instances[1]);
    check_projection_view(smoke, &scene, 0u, &ninety_nine, second);
    scene.boxes[1].custom[3] = 7.0f;
    box_instance(&scene.boxes[1], scene.instances[1]);
    assert((double)ninety_nine.box_matched[1] >= 0.98 * (double)ninety_nine.box_checked[1]);
    uint32_t box_pixels = 0u;
    uint32_t moved = 0u;
    double inverse[16];
    const SmokeCamera front = projection_camera(0u);
    assert(invert4(front.view_projection, inverse));
    for (uint32_t index = 0u; index < PROJECTION_SIZE * PROJECTION_SIZE; ++index) {
        const Ray ray = pixel_ray(inverse, index % PROJECTION_SIZE, index / PROJECTION_SIZE, PROJECTION_SIZE);
        double nearest = 1.0e30;
        uint32_t nearest_box = UINT32_MAX;
        for (uint32_t box = 0u; box < scene.box_count; ++box) {
            double distance;
            float position[3];
            float normal[3];
            int face;
            if (hit_box(&scene.boxes[box], &ray, &distance, position, &face, normal) && distance < nearest) {
                nearest = distance;
                nearest_box = box;
            }
        }
        if (nearest_box != 1u || first[index * 2u] < 0 || second[index * 2u] < 0) {
            continue;
        }
        box_pixels += 1u;
        if (first[index * 2u] != second[index * 2u] || first[index * 2u + 1u] != second[index * 2u + 1u]) {
            moved += 1u;
        }
    }
    float offset_99[2];
    const float custom_99[4] = { 0.0f, 0.0f, 0.0f, 99.0f };
    btbh_gpu_world_detail_offset(custom_99, 4u, offset_99);
    printf("projection: seeds 7 and 99 offset box A's pattern by (%.3f, %.3f) and (%.3f, %.3f) repeats: %u of its %u "
           "pixels show another texel; A and E (one seed) continue one pattern, F (another) doesn't\n",
        offsets[0][0], offsets[0][1], offset_99[0], offset_99[1], moved, box_pixels);
    assert(box_pixels >= 300u);
    assert((double)moved >= 0.9 * (double)box_pixels);
    btbh_gpu_texture_destroy(detail.albedo_texture);
}

/* ------------------------------------------------------------------ set: packed, neutral, unlit, refraction */

static uint8_t detail_normal_texel(uint32_t x, uint32_t y, uint32_t channel) {
    const float tau = 2.0f * PI_F;
    switch (channel) {
        case 0: return (uint8_t)(128.0f + 90.0f * sinf(tau * (float)(x + 2u * y) / 32.0f));
        case 1: return (uint8_t)(128.0f + 90.0f * cosf(tau * (float)(3u * x + y) / 64.0f));
        case 2: return 255u;
        default: return 255u;
    }
}

static uint8_t detail_orm_value(uint32_t x, uint32_t y, uint32_t channel) {
    switch (channel) {
        case 0: return (uint8_t)(150u + (x * 13u + y * 7u) % 105u);
        case 1: return (uint8_t)(120u + (x * 3u + y * 17u) % 135u);
        default: return (uint8_t)(110u + ((x / 4u) * 29u + (y / 4u) * 41u) % 145u);
    }
}

static uint8_t detail_orm_packed_texel(uint32_t x, uint32_t y, uint32_t channel) {
    return channel < 3u ? detail_orm_value(x, y, channel) : 255u;
}

static uint8_t detail_orm_plain_texel(uint32_t x, uint32_t y, uint32_t channel) {
    return channel < 2u ? detail_orm_value(x, y, channel) : (channel == 2u ? 77u : 255u);
}

static uint8_t detail_albedo_texel(uint32_t x, uint32_t y, uint32_t channel) {
    return channel < 3u ? detail_orm_value(x, y, 2u) : 255u;
}

static uint8_t flat_normal_texel(uint32_t x, uint32_t y, uint32_t channel) {
    (void)x;
    (void)y;
    return channel < 2u ? 128u : 255u;
}

static uint8_t neutral_orm_texel(uint32_t x, uint32_t y, uint32_t channel) {
    (void)x;
    (void)y;
    static const uint8_t value[4] = { 255u, 204u, 204u, 255u };
    return value[channel];
}

static uint8_t neutral_orm_plain_texel(uint32_t x, uint32_t y, uint32_t channel) {
    (void)x;
    (void)y;
    static const uint8_t value[4] = { 255u, 204u, 0u, 255u };
    return value[channel];
}

static uint8_t neutral_albedo_texel(uint32_t x, uint32_t y, uint32_t channel) {
    (void)x;
    (void)y;
    return channel < 3u ? 204u : 255u;
}

static BTBHGPUTexture *detail_texture(SmokeWindow *smoke, TexelFunction texel) {
    return make_texture(smoke->window, 64u, texel, 1u, BTBH_GPU_TEXTURE_FILTER_TRILINEAR,
        BTBH_GPU_TEXTURE_COLOR_SPACE_LINEAR);
}

static BTBHGPUSceneWorldDetail v2_detail(void) {
    BTBHGPUSceneWorldDetail detail;
    memset(&detail, 0, sizeof(detail));
    detail.level_tile[0] = 4.0f;
    detail.level_tile[1] = 4.0f;
    detail.wall_tile[0] = 2.0f;
    detail.wall_tile[1] = 1.4f;
    detail.albedo_scale = 1.25f;
    detail.roughness_scale = 1.25f;
    detail.seed_component = 4u;
    return detail;
}

/* The lit gallery scene (the sun's shadow) with `detail` on every batch, its boxes seeded. */
static uint8_t *render_lit(SmokeWindow *smoke, const BTBHGPUSceneWorldDetail *detail, uint32_t gallery_index) {
    static Scene scene;
    gallery_scene(gallery_index, smoke, &scene);
    for (uint32_t box = 0u; box < scene.box_count; ++box) {
        scene.boxes[box].custom[3] = (float)(box * 17u + 3u);
        box_instance(&scene.boxes[box], scene.instances[box]);
        scene.batches[box].world_detail = detail;
    }
    BTBHGPUSceneReport report;
    return render_scene(smoke, NULL, smoke->gallery, &scene, NULL, BTBH_GPU_OK, &report);
}

static void check_layouts(SmokeWindow *smoke) {
    const uint32_t texels = GALLERY_SIZE * GALLERY_SIZE;
    BTBHGPUTexture *normal = detail_texture(smoke, detail_normal_texel);
    BTBHGPUTexture *orm_packed = detail_texture(smoke, detail_orm_packed_texel);
    BTBHGPUTexture *orm_plain = detail_texture(smoke, detail_orm_plain_texel);
    BTBHGPUTexture *albedo = detail_texture(smoke, detail_albedo_texel);
    BTBHGPUSceneWorldDetail packed = v2_detail();
    packed.normal_texture = normal;
    packed.orm_texture = orm_packed;
    packed.flags = BTBH_GPU_SCENE_WORLD_DETAIL_ALBEDO_IN_ORM_BLUE;
    BTBHGPUSceneWorldDetail separate = v2_detail();
    separate.albedo_texture = albedo;
    separate.normal_texture = normal;
    separate.orm_texture = orm_plain;
    /* Lit through the sun's shadow (2D variant), the omni shadow (full) and no shadow (unshadowed), and RC. */
    static const uint32_t scenes[4] = { 0u, 2u, 3u, 7u };
    for (uint32_t index = 0u; index < 4u; ++index) {
        uint8_t *none = render_lit(smoke, NULL, scenes[index]);
        uint8_t *packed_image = render_lit(smoke, &packed, scenes[index]);
        uint8_t *separate_image = render_lit(smoke, &separate, scenes[index]);
        int largest = 0;
        const uint32_t differing = differing_pixels(packed_image, separate_image, texels, 1, &largest);
        const uint32_t detailed = differing_pixels(none, packed_image, texels, 2, NULL);
        static Scene named;
        gallery_scene(scenes[index], smoke, &named);
        printf("packed against separate (%s): %u pixels differ by more than 1 (largest %d); the detail changes %u "
               "pixels\n", named.name, differing, largest, detailed);
        assert(differing == 0u);
        assert(detailed >= texels / 4u);
        const char *dump = SDL_getenv("BTBH_WORLD_DETAIL_DUMP");
        if (dump != NULL && dump[0] != 0 && index == 0u) {
            char path[1024];
            (void)snprintf(path, sizeof(path), "%s/detail-none.png", dump);
            assert(btbh_gpu_write_png_rgba8(path, GALLERY_SIZE, GALLERY_SIZE, none) == BTBH_GPU_OK);
            (void)snprintf(path, sizeof(path), "%s/detail-packed.png", dump);
            assert(btbh_gpu_write_png_rgba8(path, GALLERY_SIZE, GALLERY_SIZE, packed_image) == BTBH_GPU_OK);
        }
        free(none);
        free(packed_image);
        free(separate_image);
    }

    /* Neutral: an ORM of (1, 0.8, 0.8) with both scales 1.25 (v2's mean), packed or with a separate albedo of 0.8:
       every factor is exactly 1, so byte for byte as no detail. With a flat normal map too, within 3/255: a texel's
       128 decodes to a tilt of 1/255, not 0. */
    BTBHGPUTexture *flat = detail_texture(smoke, flat_normal_texel);
    BTBHGPUTexture *neutral_orm = detail_texture(smoke, neutral_orm_texel);
    BTBHGPUTexture *neutral_orm_plain = detail_texture(smoke, neutral_orm_plain_texel);
    BTBHGPUTexture *neutral_albedo = detail_texture(smoke, neutral_albedo_texel);
    BTBHGPUSceneWorldDetail neutral = v2_detail();
    neutral.orm_texture = neutral_orm;
    neutral.flags = BTBH_GPU_SCENE_WORLD_DETAIL_ALBEDO_IN_ORM_BLUE;
    BTBHGPUSceneWorldDetail neutral_separate = v2_detail();
    neutral_separate.albedo_texture = neutral_albedo;
    neutral_separate.orm_texture = neutral_orm_plain;
    BTBHGPUSceneWorldDetail neutral_flat = neutral;
    neutral_flat.normal_texture = flat;
    uint8_t *none = render_lit(smoke, NULL, 0u);
    uint8_t *neutral_image = render_lit(smoke, &neutral, 0u);
    uint8_t *neutral_separate_image = render_lit(smoke, &neutral_separate, 0u);
    uint8_t *neutral_flat_image = render_lit(smoke, &neutral_flat, 0u);
    int largest = 0;
    int largest_separate = 0;
    int largest_flat = 0;
    const uint32_t differing = differing_pixels(none, neutral_image, texels, 0, &largest);
    const uint32_t differing_separate = differing_pixels(none, neutral_separate_image, texels, 0, &largest_separate);
    const uint32_t differing_flat = differing_pixels(none, neutral_flat_image, texels, 0, &largest_flat);
    printf("neutral detail: %u pixels differ from no detail packed, %u with a separate albedo; with a flat normal "
           "map, %u pixels within %d\n", differing, differing_separate, differing_flat, largest_flat);
    assert(differing == 0u && differing_separate == 0u);
    assert(largest_flat <= 3);
    free(none);
    free(neutral_image);
    free(neutral_separate_image);
    free(neutral_flat_image);

    /* An unlit see-through batch with a detail leaves the fast unlit pipeline and shows it; a refracting batch
       ignores its detail. */
    {
        static Scene scene;
        gallery_scene(0u, smoke, &scene);
        BTBHGPUSceneGeometryBatch glass = cube_batch(smoke->cube, 0u, 1u);
        glass.unlit_enabled = 1u;
        glass.transparency_enabled = 1u;
        const float tint[4] = { 0.9f, 0.9f, 0.9f, 0.7f };
        scene_add(&scene, make_box(-0.5f, 0.9f, 2.0f, 1.2f, 1.2f, 1.2f, tint), glass);
        BTBHGPUSceneGeometryBatch refracting = cube_batch(smoke->cube, 0u, 1u);
        refracting.surface_variant = BTBH_GPU_SCENE_SURFACE_REFRACT;
        refracting.transparency_enabled = 1u;
        refracting.surface_parameters0[0] = 0.05f;
        const float clear_tint[4] = { 0.8f, 0.9f, 1.0f, 0.6f };
        scene_add(&scene, make_box(1.6f, 0.6f, 2.2f, 1.0f, 1.0f, 1.0f, clear_tint), refracting);
        BTBHGPUSceneReport report;
        uint8_t *plain = render_scene(smoke, NULL, smoke->gallery, &scene, NULL, BTBH_GPU_OK, &report);
        BTBHGPUSceneGeometryBatch batches[MAX_BOXES];
        memcpy(batches, scene.batches, sizeof(batches));
        batches[scene.batch_count - 1u].world_detail = &separate;
        uint8_t *refracted = render_scene(smoke, NULL, smoke->gallery, &scene, batches, BTBH_GPU_OK, &report);
        const uint32_t refraction_differing = differing_pixels(plain, refracted, texels, 0, NULL);
        batches[scene.batch_count - 2u].world_detail = &separate;
        uint8_t *glazed = render_scene(smoke, NULL, smoke->gallery, &scene, batches, BTBH_GPU_OK, &report);
        const uint32_t glass_differing = differing_pixels(refracted, glazed, texels, 2, NULL);
        printf("a refracting batch with a detail: %u pixels differ; an unlit see-through one: %u pixels show it\n",
            refraction_differing, glass_differing);
        assert(refraction_differing == 0u);
        assert(glass_differing >= 200u);
        free(plain);
        free(refracted);
        free(glazed);
    }
    btbh_gpu_texture_destroy(normal);
    btbh_gpu_texture_destroy(orm_packed);
    btbh_gpu_texture_destroy(orm_plain);
    btbh_gpu_texture_destroy(albedo);
    btbh_gpu_texture_destroy(flat);
    btbh_gpu_texture_destroy(neutral_orm);
    btbh_gpu_texture_destroy(neutral_orm_plain);
    btbh_gpu_texture_destroy(neutral_albedo);
}

/* -------------------------------------------------------------------------------- refusals and old shaders */

static void check_refusals(SmokeWindow *smoke, SmokeWindow *other) {
    BTBHGPUTexture *normal = detail_texture(smoke, flat_normal_texel);
    BTBHGPUTexture *orm = detail_texture(smoke, neutral_orm_texel);
    BTBHGPUTexture *foreign = make_texture(other->window, 4u, flat_normal_texel, 0u, BTBH_GPU_TEXTURE_FILTER_LINEAR,
        BTBH_GPU_TEXTURE_COLOR_SPACE_LINEAR);
    BTBHGPUSceneWorldDetail valid = v2_detail();
    valid.normal_texture = normal;
    valid.orm_texture = orm;
    valid.flags = BTBH_GPU_SCENE_WORLD_DETAIL_ALBEDO_IN_ORM_BLUE;
    static Scene scene;
    projection_scene(smoke, NULL, &scene);
    scene.camera = projection_camera(0u);
    BTBHGPUSceneGeometryBatch batches[MAX_BOXES];
    memcpy(batches, scene.batches, sizeof(batches));
    BTBHGPUSceneReport report;
    batches[1].world_detail = &valid;
    uint8_t *ok = render_scene(smoke, NULL, smoke->projection, &scene, batches, BTBH_GPU_OK, &report);
    free(ok);
    const char *names[11] = {
        "a zero level tile", "a NaN wall tile", "level_degrees 90", "seed_component 5", "an unknown flag",
        "reserved", "packed with an albedo texture", "packed without an ORM texture", "another window's texture",
        "an infinite normal_scale", "a negative wall tile",
    };
    for (uint32_t refusal = 0u; refusal < 11u; ++refusal) {
        BTBHGPUSceneWorldDetail bad = valid;
        switch (refusal) {
            case 0u: bad.level_tile[0] = 0.0f; break;
            case 1u: bad.wall_tile[1] = NAN; break;
            case 2u: bad.level_degrees = 90.0f; break;
            case 3u: bad.seed_component = 5u; break;
            case 4u: bad.flags |= 0x80u; break;
            case 5u: bad.reserved[0] = 1u; break;
            case 6u: bad.albedo_texture = orm; break;
            case 7u: bad.orm_texture = NULL; break;
            case 8u: bad.normal_texture = foreign; break;
            case 9u: bad.normal_scale = INFINITY; break;
            default: bad.wall_tile[0] = -2.0f; break;
        }
        batches[1].world_detail = &valid;
        batches[2].world_detail = &bad;
        uint8_t *none = render_scene(smoke, NULL, smoke->projection, &scene, batches, BTBH_GPU_INVALID_ARGUMENT,
            &report);
        assert(none == NULL);
        if (strstr(report.message, "batch 2") == NULL) {
            fprintf(stderr, "%s: %s\n", names[refusal], report.message);
        }
        assert(strstr(report.message, "batch 2") != NULL);
        if (refusal == 0u) {
            printf("refused: \"%s\"\n", report.message);
        }
    }
    printf("refusals: %s, %s, %s, %s, %s, %s, %s, %s, %s, %s and %s fail the present, naming the batch\n", names[0],
        names[1], names[2], names[3], names[4], names[5], names[6], names[7], names[8], names[9], names[10]);
    btbh_gpu_texture_destroy(foreign);

    /* Old shaders: a copy of the scene fragment shader with world_detail_normal_texture renamed stands in for one
       from before 0.6.0 (SPIR-V; Metal libraries aren't checked). In a fresh window, so its pipelines come from it. */
    if (SMOKE_SHADER_FORMAT == BTBH_GPU_SHADER_FORMAT_SPIRV) {
        size_t byte_count = 0u;
        uint8_t *bytes = (uint8_t *)SDL_LoadFile(other->shaders.paths[7], &byte_count);
        assert(bytes != NULL);
        static const char name[] = "world_detail_normal_texture";
        uint32_t renamed = 0u;
        for (size_t offset = 0u; offset + sizeof(name) - 1u <= byte_count; ++offset) {
            if (memcmp(bytes + offset, name, sizeof(name) - 1u) == 0) {
                bytes[offset] = 'x';
                renamed += 1u;
            }
        }
        assert(renamed > 0u);
        ShaderSet old = other->shaders;
        (void)snprintf(old.paths[7], sizeof(old.paths[7]), "%s/production_model_no_world_detail.frag.spv",
            BTBH_GPU_SMOKE_SHADER_DIR);
        assert(SDL_SaveFile(old.paths[7], bytes, byte_count));
        SDL_free(bytes);
        BTBHGPUTexture *other_normal = detail_texture(other, flat_normal_texel);
        BTBHGPUTexture *other_orm = detail_texture(other, neutral_orm_texel);
        BTBHGPUSceneWorldDetail other_detail = valid;
        other_detail.normal_texture = other_normal;
        other_detail.orm_texture = other_orm;
        static Scene old_scene;
        gallery_scene(2u, other, &old_scene); /* the omni shadow: the scene fragment shader itself */
        uint8_t *current = render_scene(other, NULL, other->gallery, &old_scene, NULL, BTBH_GPU_OK, &report);
        BTBHGPUSceneGeometryBatch old_batches[MAX_BOXES];
        memcpy(old_batches, old_scene.batches, sizeof(old_batches));
        old_batches[1].world_detail = &other_detail;
        uint8_t *refused_image = render_scene(other, &old, other->gallery, &old_scene, old_batches,
            BTBH_GPU_INVALID_ARGUMENT, &report);
        assert(refused_image == NULL);
        assert(strstr(report.message, old.paths[7]) != NULL);
        printf("old shaders: \"%s\"\n", report.message);
        /* Without the detail (or with one that has no texture) it draws, as before. */
        uint8_t *plain = render_scene(other, &old, other->gallery, &old_scene, NULL, BTBH_GPU_OK, &report);
        assert(memcmp(plain, current, (size_t)GALLERY_SIZE * GALLERY_SIZE * 4u) == 0);
        BTBHGPUSceneWorldDetail textureless = v2_detail();
        old_batches[1].world_detail = &textureless;
        uint8_t *textureless_image = render_scene(other, &old, other->gallery, &old_scene, old_batches, BTBH_GPU_OK,
            &report);
        assert(memcmp(textureless_image, current, (size_t)GALLERY_SIZE * GALLERY_SIZE * 4u) == 0);
        free(current);
        free(plain);
        free(textureless_image);
        (void)SDL_RemovePath(old.paths[7]);
        btbh_gpu_texture_destroy(other_normal);
        btbh_gpu_texture_destroy(other_orm);
        printf("old shaders: a batch with a world detail fails the present with the shader's path; without one (or "
               "with one without textures) it draws as with the current shaders\n");
    }
    btbh_gpu_texture_destroy(normal);
    btbh_gpu_texture_destroy(orm);
}

/* ------------------------------------------------------------------------------- BTBH_SMOKE_BENCH=1: the cost */

#define BENCH_ROUNDS 10
#define BENCH_WARM_FRAMES 3
#define BENCH_FRAMES 20
#define BENCH_LAYERS 8u

typedef struct BenchConfig {
    const char *name;
    SmokeWindow *smoke;
    const BTBHGPUSceneWorldDetail *detail;
    float detail_parameters[4];        /* the procedural detail; zero: none */
    double times[BENCH_ROUNDS];
    uint32_t sample_count;
} BenchConfig;

static int compare_doubles(const void *left, const void *right) {
    const double a = *(const double *)left;
    const double b = *(const double *)right;
    return a < b ? -1 : (a > b ? 1 : 0);
}

/* The opaque-sort bench's heavy pixel: the sun's soft shadow, 40 lamps and 32 point lights, here all in front of
   the layers (behind them, a flat surface would skip the lights a bumped one takes, and the bench would time
   that rather than the detail). */
static BTBHGPUSceneLighting bench_lighting(void) {
    BTBHGPUSceneLighting lighting = base_lighting(true);
    lighting.directional_shadow_filter = BTBH_GPU_SCENE_SHADOW_FILTER_SOFT;
    lighting.lamp_count = BTBH_GPU_SCENE_MAX_LAMPS;
    lighting.lamp_core_radius = 0.5f;
    for (uint32_t lamp = 0u; lamp < BTBH_GPU_SCENE_MAX_LAMPS; ++lamp) {
        lighting.lamp_position_reach[lamp][0] = -12.0f + 0.6f * (float)lamp;
        lighting.lamp_position_reach[lamp][1] = 1.5f;
        lighting.lamp_position_reach[lamp][2] = 2.0f + 0.3f * (float)(lamp % 7u);
        lighting.lamp_position_reach[lamp][3] = 30.0f;
        lighting.lamp_color_energy[lamp][0] = 1.0f;
        lighting.lamp_color_energy[lamp][1] = 0.8f;
        lighting.lamp_color_energy[lamp][2] = 0.6f;
        lighting.lamp_color_energy[lamp][3] = 0.05f;
    }
    lighting.point_light_count = BTBH_GPU_SCENE_POINT_LIGHTS;
    for (uint32_t light = 0u; light < BTBH_GPU_SCENE_POINT_LIGHTS; ++light) {
        float *point = lighting.point_lights[light];
        point[0] = -10.0f + 0.65f * (float)light;
        point[1] = 2.0f;
        point[2] = 2.5f + 0.25f * (float)(light % 5u);
        point[3] = 40.0f;
        point[4] = 0.6f;
        point[5] = 0.7f;
        point[6] = 1.0f;
        point[7] = 0.05f;
    }
    return lighting;
}

static uint32_t bench_random(uint32_t *state) {
    *state = *state * 1664525u + 1013904223u;
    return *state >> 8u;
}

/* 512^2 maps with mips, like v2's macro sets: noise in every channel. */
static uint8_t bench_noise_texel(uint32_t x, uint32_t y, uint32_t channel) {
    uint32_t state = (x * 73856093u) ^ (y * 19349663u) ^ (channel * 83492791u);
    const uint32_t value = bench_random(&state);
    if (channel == 3u) {
        return 255u;
    }
    return (uint8_t)(96u + value % 128u);
}

/* One turn: BENCH_WARM_FRAMES frames, a wait, then BENCH_FRAMES frames back to back and one wait. */
static void bench_turn(BenchConfig *config, BTBHGPUTexture *target, const BTBHGPUSceneLighting *lighting) {
    SmokeWindow *smoke = config->smoke;
    const float aspect = (float)target->width / (float)target->height;
    const float eye[3] = { 0.0f, 0.0f, 8.0f };
    const float centre[3] = { 0.0f, 0.0f, 0.0f };
    const SmokeCamera camera = look_at(eye, centre, 0.9f, aspect);
    static float instances[1][24];
    const Box slab = make_box(0.0f, 0.0f, 0.0f, 60.0f, 34.0f, 0.02f, grey);
    box_instance(&slab, instances[0]);
    BTBHGPUSceneGeometryBatch batches[BENCH_LAYERS];
    for (uint32_t layer = 0u; layer < BENCH_LAYERS; ++layer) {
        batches[layer] = cube_batch(smoke->cube, 0u, 1u);
        batches[layer].depth_compare = BTBH_GPU_SCENE_DEPTH_LESS_EQUAL;
        batches[layer].world_detail = config->detail;
        memcpy(batches[layer].detail_parameters, config->detail_parameters, sizeof(config->detail_parameters));
    }
    BTBHGPUScenePost post = base_post();
    SDL_GPUDevice *device = smoke->window->device->gpu;
    uint64_t started = 0u;
    for (int frame = 0; frame < BENCH_WARM_FRAMES + BENCH_FRAMES; ++frame) {
        if (frame == BENCH_WARM_FRAMES) {
            (void)SDL_WaitForGPUIdle(device);
            started = SDL_GetTicksNS();
        }
        BTBHGPUSceneReport report;
        const ShaderSet *set = &smoke->shaders;
        const BTBHGPUResult rendered = btbh_gpu_window_scene_render_to_target(
            smoke->window, target,
            set->paths[0], set->paths[1], set->paths[2], set->paths[3],
            set->paths[4], set->paths[5], set->paths[6], set->paths[7],
            set->paths[8], set->paths[9], set->paths[10],
            SMOKE_SHADER_FORMAT, target->width, target->height,
            camera.view_projection, camera.right, camera.up, camera.position,
            lighting, &post, &instances[0][0], 1u, 1u,
            batches, NULL, NULL, BENCH_LAYERS,
            12u, 14u, 20u, 255u, &report);
        if (rendered != BTBH_GPU_OK) {
            fprintf(stderr, "bench: %s\n", report.message);
        }
        assert(rendered == BTBH_GPU_OK);
    }
    (void)SDL_WaitForGPUIdle(device);
    if (config->sample_count < BENCH_ROUNDS) {
        config->times[config->sample_count++] = (double)(SDL_GetTicksNS() - started) / 1.0e6 / BENCH_FRAMES;
    }
}

static void bench_world_detail(SmokeWindow *smoke, SmokeWindow *baseline) {
    const char *device_name = SDL_GetStringProperty(
        SDL_GetGPUDeviceProperties(smoke->window->device->gpu), SDL_PROP_GPU_DEVICE_NAME_STRING, "unknown");
    BTBHGPUTexture *albedo = make_texture(smoke->window, 512u, bench_noise_texel, 1u,
        BTBH_GPU_TEXTURE_FILTER_TRILINEAR, BTBH_GPU_TEXTURE_COLOR_SPACE_LINEAR);
    BTBHGPUTexture *normal = make_texture(smoke->window, 512u, bench_noise_texel, 1u,
        BTBH_GPU_TEXTURE_FILTER_TRILINEAR, BTBH_GPU_TEXTURE_COLOR_SPACE_LINEAR);
    BTBHGPUTexture *orm = make_texture(smoke->window, 512u, bench_noise_texel, 1u,
        BTBH_GPU_TEXTURE_FILTER_TRILINEAR, BTBH_GPU_TEXTURE_COLOR_SPACE_LINEAR);
    BTBHGPUSceneWorldDetail packed = v2_detail();
    packed.normal_texture = normal;
    packed.orm_texture = orm;
    packed.flags = BTBH_GPU_SCENE_WORLD_DETAIL_ALBEDO_IN_ORM_BLUE;
    BTBHGPUSceneWorldDetail three = v2_detail();
    three.albedo_texture = albedo;
    three.normal_texture = normal;
    three.orm_texture = orm;
    const BTBHGPUSceneLighting heavy = bench_lighting();
    BTBHGPUSceneLighting light = base_lighting(false);
    light.ambient_energy = 0.5f;
    static const uint32_t sizes[2][2] = { { 1920u, 1080u }, { 960u, 600u } };
    for (uint32_t pixel = 0u; pixel < 2u; ++pixel) {
        const BTBHGPUSceneLighting *lighting = pixel == 0u ? &heavy : &light;
        for (uint32_t size = 0u; size < 2u; ++size) {
            BTBHGPUTextureReport texture_report;
            BTBHGPUTexture *target = btbh_gpu_window_render_target_create(smoke->window, sizes[size][0],
                sizes[size][1], BTBH_GPU_TEXTURE_FILTER_LINEAR, &texture_report);
            assert(target != NULL);
            BTBHGPUTexture *baseline_target = NULL;
            if (baseline != NULL) {
                baseline_target = btbh_gpu_window_render_target_create(baseline->window, sizes[size][0],
                    sizes[size][1], BTBH_GPU_TEXTURE_FILTER_LINEAR, &texture_report);
                assert(baseline_target != NULL);
            }
            BenchConfig configs[4];
            memset(configs, 0, sizeof(configs));
            configs[0] = (BenchConfig){ .name = "no detail", .smoke = smoke, .detail = NULL };
            configs[1] = (BenchConfig){ .name = "packed (normal, ORM)", .smoke = smoke, .detail = &packed };
            configs[2] = (BenchConfig){ .name = "three maps", .smoke = smoke, .detail = &three };
            configs[3] = (BenchConfig){ .name = "no detail, baseline shaders", .smoke = baseline, .detail = NULL };
            const uint32_t config_count = baseline != NULL ? 4u : 3u;
            for (int round = 0; round < BENCH_ROUNDS; ++round) {
                for (uint32_t index = 0u; index < config_count; ++index) {
                    bench_turn(&configs[index], index == 3u ? baseline_target : target, lighting);
                }
            }
            printf("bench on %s, %ux%u, %s pixel, %u coplanar screen-filling layers (LESS_EQUAL), %d turns of %d "
                   "frames:\n", device_name, sizes[size][0], sizes[size][1], pixel == 0u ? "a heavy" : "a light",
                BENCH_LAYERS, BENCH_ROUNDS, BENCH_FRAMES);
            double fastest[4];
            for (uint32_t index = 0u; index < config_count; ++index) {
                BenchConfig *config = &configs[index];
                qsort(config->times, config->sample_count, sizeof(config->times[0]), compare_doubles);
                fastest[index] = config->times[0];
                printf("  %-28s fastest %7.3f ms, median %7.3f ms a frame\n", config->name, config->times[0],
                    config->times[config->sample_count / 2u]);
            }
            printf("  per screen of detailed surface (the difference over %u): packed %+.3f ms, three maps %+.3f ms",
                BENCH_LAYERS, (fastest[1] - fastest[0]) / BENCH_LAYERS, (fastest[2] - fastest[0]) / BENCH_LAYERS);
            if (config_count == 4u) {
                printf("; the branch unset, against the baseline shaders: %+.3f ms", (fastest[0] - fastest[3]) /
                    BENCH_LAYERS);
            }
            printf("\n");
            (void)fflush(stdout);
            btbh_gpu_texture_destroy(target);
            if (baseline_target != NULL) {
                btbh_gpu_texture_destroy(baseline_target);
            }
        }
    }
    btbh_gpu_texture_destroy(albedo);
    btbh_gpu_texture_destroy(normal);
    btbh_gpu_texture_destroy(orm);
}

static void run_world_detail_checks(BTBHGPUDevice *device, SmokeWindow *smoke, SmokeWindow *baseline) {
    check_projection(smoke);
    check_layouts(smoke);
    SmokeWindow other = open_window(device, "BTBH world detail smoke (another window)", BTBH_GPU_SMOKE_SHADER_DIR);
    check_refusals(smoke, &other);
    close_window(&other);
    const char *bench = SDL_getenv("BTBH_SMOKE_BENCH");
    if (bench != NULL && strcmp(bench, "weather") != 0) {
        bench_world_detail(smoke, baseline);
    }
}
#endif

#if defined(BTBH_GPU_SCENE_HAS_FACADE_WEATHER)
/* ------------------------------------------------------------ RFC 0009 phase 1: pattern 7, the facade's weather */

/* Pattern scene `variant` (0..5) with every batch but the lens on `detail`, dry by day or soaked in the rain at
   night, and `world_detail` on the ground and the turned box. */
static void weather_scene(
    uint32_t variant,
    bool wet,
    const SmokeWindow *smoke,
    const float detail[4],
    const BTBHGPUSceneWorldDetail *world_detail,
    Scene *scene
) {
    gallery_scene(PATTERN_GALLERY_FIRST + variant, smoke, scene);
    scene->lighting.surface_wetness = wet ? 1.0f : 0.0f;
    scene->lighting.rain = wet ? 0.6f : 0.0f;
    scene->lighting.window_glow = wet ? 0.8f : 0.0f;
    for (uint32_t batch = 0u; batch < scene->batch_count; ++batch) {
        if (batch != PATTERN_LENS) {
            memcpy(scene->batches[batch].detail_parameters, detail, 4u * sizeof(float));
        }
    }
    scene->batches[0].world_detail = world_detail;
    scene->batches[2].world_detail = world_detail;
}

static uint8_t *render_weather(
    SmokeWindow *smoke,
    uint32_t variant,
    bool wet,
    const float detail[4],
    const BTBHGPUSceneWorldDetail *world_detail,
    bool *radiance_cascades
) {
    static Scene scene;
    weather_scene(variant, wet, smoke, detail, world_detail, &scene);
    BTBHGPUSceneReport report;
    uint8_t *image = render_scene(smoke, NULL, smoke->gallery, &scene, NULL, BTBH_GPU_OK, &report);
    if (radiance_cascades != NULL) {
        *radiance_cascades = report.radiance_cascades.active_tier != BTBH_GPU_SCENE_RC_OFF;
    }
    return image;
}

static void check_facade_weather(SmokeWindow *smoke, SmokeWindow *baseline) {
    const uint32_t texels = GALLERY_SIZE * GALLERY_SIZE;
    BTBHGPUTexture *normal = detail_texture(smoke, detail_normal_texel);
    BTBHGPUTexture *orm = detail_texture(smoke, detail_orm_packed_texel);
    BTBHGPUSceneWorldDetail packed = v2_detail();
    packed.normal_texture = normal;
    packed.orm_texture = orm;
    packed.flags = BTBH_GPU_SCENE_WORLD_DETAIL_ALBEDO_IN_ORM_BLUE;
    const float weather[4] = { 0.0f, 0.0f, 0.0f, (float)BTBH_GPU_SCENE_DETAIL_FACADE_WEATHER };
    const float facade[2][4] = {
        { 0.0f, 0.0f, 6.0f, (float)BTBH_GPU_SCENE_DETAIL_FACADE },   /* v2's preview on 0.6.0 */
        { 0.0f, 0.0f, 0.37f, (float)BTBH_GPU_SCENE_DETAIL_FACADE },
    };
    const float none[4] = { 0.0f, 0.0f, 0.0f, (float)BTBH_GPU_SCENE_DETAIL_NONE };
    static const char *const variant_names[6] = {
        "sun shadow", "sun and omni shadow", "no shadow", "RC, sun shadow", "RC, sun and omni shadow", "RC, no shadow",
    };
    uint64_t compared = 0u;
    for (uint32_t variant = 0u; variant < 6u; ++variant) {
        uint8_t *dry_weather = NULL;
        for (uint32_t wet = 0u; wet < 2u; ++wet) {
            bool rc = false;
            uint8_t *seven = render_weather(smoke, variant, wet != 0u, weather, &packed, &rc);
            uint32_t differing[2];
            for (uint32_t z = 0u; z < 2u; ++z) {
                uint8_t *three = render_weather(smoke, variant, wet != 0u, facade[z], &packed, NULL);
                differing[z] = differing_pixels(seven, three, texels, 0, NULL);
                compared += texels;
                free(three);
            }
            uint8_t *plain = render_weather(smoke, variant, wet != 0u, none, &packed, NULL);
            const uint32_t changed = differing_pixels(seven, plain, texels, 0, NULL);
            printf("pattern 7 (%s%s, %s): against pattern 3 with x = y = 0, %u pixels differ at z 6 and %u at z "
                   "0.37; it changes %u pixels of no pattern", variant_names[variant],
                variant >= 3u && !rc ? " (the cascades didn't run)" : "", wet != 0u ? "soaked" : "dry", differing[0],
                differing[1],
                changed);
            assert(differing[0] == 0u && differing[1] == 0u);
            /* The ground and every wall's foot darken (most of the image); soaked, the walls darken more. */
            assert(changed >= texels / 4u);
            if (wet == 0u) {
                dry_weather = seven;
                printf("\n");
            } else {
                const uint32_t wetted = differing_pixels(dry_weather, seven, texels, 0, NULL);
                printf("; %u pixels differ from dry\n", wetted);
                assert(wetted >= texels / 8u);
                free(dry_weather);
                free(seven);
            }
            free(plain);
        }
    }
    printf("pattern 7: byte for byte pattern 3 with x = y = 0 (z 6 and 0.37) through six shaders, dry and soaked, "
           "%llu pixels compared\n", (unsigned long long)compared);
    (void)fflush(stdout);

    /* Refusals: pattern 7 takes x, y and z at 0; nothing past 7 is a pattern. */
    {
        static Scene scene;
        gallery_scene(0u, smoke, &scene);
        BTBHGPUSceneGeometryBatch batches[MAX_BOXES];
        memcpy(batches, scene.batches, sizeof(batches));
        BTBHGPUSceneReport report;
        memcpy(batches[1].detail_parameters, weather, sizeof(weather));
        uint8_t *accepted = render_scene(smoke, NULL, smoke->gallery, &scene, batches, BTBH_GPU_OK, &report);
        free(accepted);
        static const float refused[5][4] = {
            { 0.0f, 0.0f, 6.0f, 7.0f }, { 0.07f, 0.0f, 0.0f, 7.0f }, { 0.0f, 0.01f, 0.0f, 7.0f },
            { 0.0f, 0.0f, 0.0f, 8.0f }, { 0.0f, 0.0f, 1.0f, 7.25f },
        };
        for (uint32_t refusal = 0u; refusal < 5u; ++refusal) {
            memcpy(batches[1].detail_parameters, refused[refusal], sizeof(refused[refusal]));
            uint8_t *image = render_scene(smoke, NULL, smoke->gallery, &scene, batches, BTBH_GPU_INVALID_ARGUMENT,
                &report);
            assert(image == NULL);
        }
        printf("refusals: [0, 0, 0, 7] draws; [0, 0, 6, 7], [0.07, 0, 0, 7], [0, 0.01, 0, 7], [0, 0, 0, 8] and "
               "[0, 0, 1, 7.25] fail the present (\"%s\")\n", report.message);
    }

    /* Old shaders: 0.6.0's take pattern 7 for the ground's with x and y at 0 (no darkening, no wet walls). */
    if (baseline != NULL) {
        uint8_t *old_weather = render_weather(baseline, 0u, true, weather, NULL, NULL);
        uint8_t *old_none = render_weather(baseline, 0u, true, none, NULL, NULL);
        uint8_t *new_weather = render_weather(smoke, 0u, true, weather, NULL, NULL);
        const uint32_t as_none = differing_pixels(old_weather, old_none, texels, 0, NULL);
        const uint32_t as_new = differing_pixels(old_weather, new_weather, texels, 0, NULL);
        printf("old shaders: pattern 7 differs from no pattern on %u pixels and from these shaders' pattern 7 on %u\n",
            as_none, as_new);
        assert(as_new >= texels / 4u);
        free(old_weather);
        free(old_none);
        free(new_weather);
    }
    btbh_gpu_texture_destroy(normal);
    btbh_gpu_texture_destroy(orm);
}

/* BTBH_SMOKE_BENCH: pattern 7 against none and against pattern 3, per screen of facade. */
static void bench_facade_weather(SmokeWindow *smoke, SmokeWindow *baseline) {
    const char *device_name = SDL_GetStringProperty(
        SDL_GetGPUDeviceProperties(smoke->window->device->gpu), SDL_PROP_GPU_DEVICE_NAME_STRING, "unknown");
    static const uint32_t sizes[2][2] = { { 1920u, 1080u }, { 960u, 600u } };
    for (uint32_t pixel = 0u; pixel < 2u; ++pixel) {
        for (uint32_t wet = 0u; wet < 2u; ++wet) {
            BTBHGPUSceneLighting lighting = bench_lighting();
            if (pixel == 1u) {
                lighting = base_lighting(false);
                lighting.ambient_energy = 0.5f;
            }
            lighting.surface_wetness = wet != 0u ? 1.0f : 0.0f;
            for (uint32_t size = 0u; size < 2u; ++size) {
                BTBHGPUTextureReport texture_report;
                BTBHGPUTexture *target = btbh_gpu_window_render_target_create(smoke->window, sizes[size][0],
                    sizes[size][1], BTBH_GPU_TEXTURE_FILTER_LINEAR, &texture_report);
                assert(target != NULL);
                BTBHGPUTexture *baseline_target = NULL;
                if (baseline != NULL) {
                    baseline_target = btbh_gpu_window_render_target_create(baseline->window, sizes[size][0],
                        sizes[size][1], BTBH_GPU_TEXTURE_FILTER_LINEAR, &texture_report);
                    assert(baseline_target != NULL);
                }
                BenchConfig configs[4];
                memset(configs, 0, sizeof(configs));
                configs[0] = (BenchConfig){ .name = "pattern 0 (none)", .smoke = smoke };
                configs[1] = (BenchConfig){ .name = "pattern 7 (weather)", .smoke = smoke,
                    .detail_parameters = { 0.0f, 0.0f, 0.0f, 7.0f } };
                configs[2] = (BenchConfig){ .name = "pattern 3 (x 0.07, y 0, z 6)", .smoke = smoke,
                    .detail_parameters = { 0.07f, 0.0f, 6.0f, 3.0f } };
                configs[3] = (BenchConfig){ .name = "pattern 0, baseline shaders", .smoke = baseline };
                const uint32_t config_count = baseline != NULL ? 4u : 3u;
                for (int round = 0; round < BENCH_ROUNDS; ++round) {
                    for (uint32_t index = 0u; index < config_count; ++index) {
                        bench_turn(&configs[index], index == 3u ? baseline_target : target, &lighting);
                    }
                }
                printf("weather bench on %s, %ux%u, %s pixel, %s, %u coplanar screen-filling walls (LESS_EQUAL), %d "
                       "turns of %d frames:\n", device_name, sizes[size][0], sizes[size][1],
                    pixel == 0u ? "a heavy" : "a light", wet != 0u ? "soaked" : "dry", BENCH_LAYERS, BENCH_ROUNDS,
                    BENCH_FRAMES);
                double fastest[4];
                for (uint32_t index = 0u; index < config_count; ++index) {
                    BenchConfig *config = &configs[index];
                    qsort(config->times, config->sample_count, sizeof(config->times[0]), compare_doubles);
                    fastest[index] = config->times[0];
                    printf("  %-30s fastest %7.3f ms, median %7.3f ms a frame\n", config->name, config->times[0],
                        config->times[config->sample_count / 2u]);
                }
                printf("  per screen of facade (the difference over %u): pattern 7 %+.4f ms, pattern 3 %+.4f ms, 7 "
                       "against 3 %+.4f ms", BENCH_LAYERS, (fastest[1] - fastest[0]) / BENCH_LAYERS,
                    (fastest[2] - fastest[0]) / BENCH_LAYERS, (fastest[1] - fastest[2]) / BENCH_LAYERS);
                if (config_count == 4u) {
                    printf("; pattern 0 against the baseline shaders %+.4f ms", (fastest[0] - fastest[3]) /
                        BENCH_LAYERS);
                }
                printf("\n");
                (void)fflush(stdout);
                btbh_gpu_texture_destroy(target);
                if (baseline_target != NULL) {
                    btbh_gpu_texture_destroy(baseline_target);
                }
            }
        }
    }
}
#endif

int main(void) {
    /* Unbuffered: a failed assert keeps what was printed before it. */
    (void)setvbuf(stdout, NULL, _IONBF, 0);
    char shader_directory[1024];
    (void)snprintf(shader_directory, sizeof(shader_directory), "%s", BTBH_GPU_SMOKE_SHADER_DIR);
    for (int index = 0; index < 11; ++index) {
        char path[1024];
        (void)snprintf(path, sizeof(path), "%s/%s%s", shader_directory, shader_names[index], SMOKE_SHADER_EXTENSION);
        FILE *file = fopen(path, "rb");
        if (file == NULL) {
            fprintf(stderr, "skipped: no shader %s\n", path);
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
    SmokeWindow smoke = open_window(device, "BTBH world detail smoke", shader_directory);
    const char *baseline_dir = SDL_getenv("BTBH_WORLD_DETAIL_BASELINE_SHADER_DIR");
    SmokeWindow baseline;
    memset(&baseline, 0, sizeof(baseline));
    const bool with_baseline = baseline_dir != NULL && baseline_dir[0] != 0;
    if (with_baseline) {
        baseline = open_window(device, "BTBH world detail smoke (baseline shaders)", baseline_dir);
        printf("baseline shaders: %s\n", baseline_dir);
    }

    uint32_t references = 0u;
    check_gallery(&smoke, with_baseline ? &baseline : NULL, &references);
    printf("unset: %u gallery scenes, each rendered twice the same%s%s%s\n", GALLERY_COUNT,
#if defined(BTBH_GPU_SCENE_HAS_WORLD_DETAIL)
        ", the same with a detail without textures",
#else
        " (0.5.1's header: no world detail to check)",
#endif
        with_baseline ? ", the same with the baseline shaders" : "",
        references > 0u ? ", the same as the reference images" : "");

#if defined(BTBH_GPU_SCENE_HAS_WORLD_DETAIL)
    run_world_detail_checks(device, &smoke, with_baseline ? &baseline : NULL);
#endif
#if defined(BTBH_GPU_SCENE_HAS_FACADE_WEATHER)
    check_facade_weather(&smoke, with_baseline ? &baseline : NULL);
    if (SDL_getenv("BTBH_SMOKE_BENCH") != NULL) {
        bench_facade_weather(&smoke, with_baseline ? &baseline : NULL);
    }
#endif

    if (with_baseline) {
        close_window(&baseline);
    }
    close_window(&smoke);
    btbh_gpu_device_shutdown(device);
    btbh_gpu_device_destroy(device);
    printf("world detail smoke passed\n");
    return 0;
}
