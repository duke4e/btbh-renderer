/* RFC 0013 phase 1 (0.11.0): lit particles (BTBH_GPU_SCENE_SURFACE_LIT_PARTICLE) and the instance UV rect
 * (BTBH_GPU_SCENE_BATCH_INSTANCE_UV_RECT), rendered into a target and read back (hidden windows, a debug device).
 *
 * - A puff against a CPU reference (src/gpu/btbh_gpu_particle.h's wrap and phase, the post's tone map and dither) at
 *   chosen pixels: the sun front-lit and back-lit; the wrap at the terminator for w 0, 0.5 and 1; the transmission
 *   through thicknesses 0, 0.5 and 1; a point light in reach in front and behind, and one out of reach (the picture
 *   without it, to the byte); the fill, the spot (with and without the omni shadow) and a lamp in front (their wrap)
 *   and behind with the transmission on (front-only: the picture without them, to the byte).
 * - The sun's shadow, front-lit and back-lit: a box shadows the puff's left half (ambient alone there), the right half
 *   lit; four plain taps whatever the sun's filter (the tent and the soft filter give the same picture).
 * - Fog at the puff's own depth over a wall (made up for the composite's fog at the wall's depth) and over the sky,
 *   against the formula; and how far the composite alone would have put it.
 * - The radiance cascades, additive and not: a glow under a puff lights it (black without them); a lit particle with
 *   an emission is no source (the floor round it as without the emission), where an unlit glow there is one.
 * - The soft fade where the puff meets the floor and the near fade at the camera, per pixel against the CPU's
 *   distances; the soft fade as SOFT_PARTICLE's.
 * - The UV rect: one batch of four atlas cells against four batches with uv_scale and uv_offset, to the byte, on
 *   every static vertex shader's path: lit particles, the fast unlit shader, soft particles, an opaque lit batch, a
 *   lifted overlay, a world-detail table, a lifted table, and the radiance cascades' source pass.
 * - The refusals of a lit particle and of the UV rect (each naming the batch), an unknown batch bit and variant 12.
 * - Prewarm makes the lit particles' pipelines: a fresh window that prewarmed the scenes renders them without making a
 *   shader or a pipeline; so does a window present under FSR 3 (the transparent pass's reactive target).
 * - Old shaders: a directory without production_particle_lit*.frag fails a lit particle naming the file; one whose
 *   file isn't the lit particle's is refused; a static vertex shader without the UV rect fails a rect batch naming it.
 *   BTBH_LIT_PARTICLE_OLD_SHADER_DIR=<a release's shaders from before 0.11.0> also checks that their
 *   production_model.vert draws unflagged batches as this release's does, to the byte, and that their
 *   production_rc_material_source.vert fails a rect batch that is the cascades' source.
 * - BTBH_LIT_PARTICLE_DUMP=<dir> writes the pictures.
 *
 * It needs a GPU and the production shaders; with no device it is skipped (77). */
#undef NDEBUG

#include "btbh_gpu/btbh_gpu.h"
#include "btbh_gpu_internal.h"
#include "btbh_gpu_particle.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_stdinc.h>

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SMOKE_SKIP 77
#define TARGET_SIZE 128u
#define TEXELS (TARGET_SIZE * TARGET_SIZE)
#define FLOATS 24u
#define MAX_BATCHES 16u
#define MAX_INSTANCES 24u
/* A CPU reference agrees within this many levels of the 8-bit output (half-float targets, the dither). */
#define LEVEL_TOLERANCE 2
#define FIELD_SIZE 17u
static const float pi = 3.14159265358979f;

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
    char paths[11][1100];
} ShaderSet;

static ShaderSet shaders;
static char canvas_shader_paths[3][1100];
static int checks;

static void shader_set_in(ShaderSet *set, const char *directory) {
    for (int index = 0; index < 11; ++index) {
        (void)snprintf(set->paths[index], sizeof(set->paths[index]), "%s/%s%s", directory, shader_names[index],
            SMOKE_SHADER_EXTENSION);
    }
}

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

/* A 4x4 inverse (column-major), by Gauss-Jordan in double. */
static void invert4(const float m[16], double out[16]) {
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
            const double t = a[pivot][column];
            a[pivot][column] = a[best][column];
            a[best][column] = t;
        }
        const double scale = 1.0 / a[pivot][pivot];
        for (int column = 0; column < 8; ++column) {
            a[pivot][column] *= scale;
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
            out[column * 4 + row] = a[row][column + 4];
        }
    }
}

typedef struct SmokeCamera {
    float view_projection[16];
    float right[3];
    float up[3];
    float position[3];
} SmokeCamera;

/* Column-major, right-handed, depth 0..1; `projection` NULL: a perspective one of fov_y. */
static SmokeCamera look_at(const float eye[3], const float centre[3], float fov_y, const float *projection) {
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
    if (projection != NULL) {
        multiply(projection, view, camera.view_projection);
    } else {
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
    }
    memcpy(camera.right, side, sizeof(side));
    memcpy(camera.up, up, sizeof(up));
    memcpy(camera.position, eye, sizeof(camera.position));
    return camera;
}

/* The ray through a pixel's centre: its direction, unit. */
static void pixel_ray(const SmokeCamera *camera, int px, int py, float direction[3]) {
    double inverse[16];
    invert4(camera->view_projection, inverse);
    const double ndc[2] = {
        ((double)px + 0.5) / (double)TARGET_SIZE * 2.0 - 1.0,
        1.0 - ((double)py + 0.5) / (double)TARGET_SIZE * 2.0,
    };
    double points[2][3];
    for (int which = 0; which < 2; ++which) {
        const double clip[4] = { ndc[0], ndc[1], which == 0 ? 0.0 : 1.0, 1.0 };
        double world[4] = { 0.0, 0.0, 0.0, 0.0 };
        for (int row = 0; row < 4; ++row) {
            for (int k = 0; k < 4; ++k) {
                world[row] += inverse[k * 4 + row] * clip[k];
            }
        }
        for (int axis = 0; axis < 3; ++axis) {
            points[which][axis] = world[axis] / world[3];
        }
    }
    for (int axis = 0; axis < 3; ++axis) {
        direction[axis] = (float)(points[1][axis] - points[0][axis]);
    }
    normalize3(direction);
}

/* Where a pixel's ray meets the plane through `point` with normal `normal`: its distance along the ray (FLT_MAX
   when it doesn't, ahead) and the point. */
static float pixel_on_plane(const SmokeCamera *camera, int px, int py, const float point[3], const float normal[3],
    float hit[3]) {
    float direction[3];
    pixel_ray(camera, px, py, direction);
    const float denominator = dot3(direction, normal);
    if (fabsf(denominator) < 1.0e-6f) {
        return 3.0e38f;
    }
    const float offset[3] = {
        point[0] - camera->position[0], point[1] - camera->position[1], point[2] - camera->position[2],
    };
    const float t = dot3(offset, normal) / denominator;
    if (t <= 0.0f) {
        return 3.0e38f;
    }
    for (int axis = 0; axis < 3; ++axis) {
        hit[axis] = camera->position[axis] + direction[axis] * t;
    }
    return t;
}

/* production_post.frag.hlsl's dither, at a pixel of a TARGET_SIZE picture. */
static float post_dither(int px, int py) {
    const float x = (float)px + 0.5f;
    const float y = (float)TARGET_SIZE - ((float)py + 0.5f);
    float d = x * 0.06711056f + y * 0.00583715f;
    d -= floorf(d);
    float n = 52.9829189f * d;
    n -= floorf(n);
    return (n - 0.5f) / 255.0f;
}

/* The 8-bit level a linear value comes out as (the post's tone map is the identity: Reinhard with white 1, exposure,
   contrast and saturation 1), and back. */
static int output_level(float linear, int px, int py) {
    const float c = linear < 0.0f ? 0.0f : (linear > 1.0f ? 1.0f : linear);
    float s = 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
    s = (s > 0.0f ? s : 0.0f) + post_dither(px, py);
    int level = (int)floorf(s * 255.0f + 0.5f);
    return level < 0 ? 0 : (level > 255 ? 255 : level);
}

static float level_linear(int level, int px, int py) {
    float s = (float)level / 255.0f - post_dither(px, py);
    s = s > 0.0f ? s : 0.0f;
    return powf((s + 0.055f) / 1.055f, 2.4f);
}

static uint32_t smoke_random(uint32_t *state) {
    *state = *state * 1664525u + 1013904223u;
    return *state >> 8u;
}

static BTBHGPUTexture *texture_from(BTBHGPUWindow *window, const uint8_t *pixels, uint32_t width, uint32_t height,
    BTBHGPUTextureFilter filter, BTBHGPUTextureWrap wrap, BTBHGPUTextureColorSpace color_space) {
    BTBHGPUTextureReport report;
    memset(&report, 0, sizeof(report));
    BTBHGPUTexture *texture = btbh_gpu_window_texture_create_rgba8_ex(window, pixels, width, height, 1u, filter, wrap,
        color_space, &report);
    if (texture == NULL) {
        fprintf(stderr, "texture: %s\n", report.message);
    }
    assert(texture != NULL);
    return texture;
}

/* A flat normal map (rg 128) with the thickness `thickness` in b. */
static BTBHGPUTexture *flat_normal_texture(BTBHGPUWindow *window, uint8_t thickness) {
    uint8_t pixels[4 * 4 * 4];
    for (uint32_t index = 0u; index < 16u; ++index) {
        pixels[index * 4u + 0u] = 128u;
        pixels[index * 4u + 1u] = 128u;
        pixels[index * 4u + 2u] = thickness;
        pixels[index * 4u + 3u] = 255u;
    }
    return texture_from(window, pixels, 4u, 4u, BTBH_GPU_TEXTURE_FILTER_LINEAR, BTBH_GPU_TEXTURE_WRAP_CLAMP,
        BTBH_GPU_TEXTURE_COLOR_SPACE_LINEAR);
}

/* A 64 x 64 atlas of 4 x 4 cells: each a disc of its own colour with a soft edge in alpha. */
static BTBHGPUTexture *atlas_texture(BTBHGPUWindow *window) {
    uint8_t *pixels = malloc(64u * 64u * 4u);
    assert(pixels != NULL);
    for (uint32_t y = 0u; y < 64u; ++y) {
        for (uint32_t x = 0u; x < 64u; ++x) {
            const uint32_t cell = (y / 16u) * 4u + x / 16u;
            const float u = ((float)(x % 16u) + 0.5f) / 8.0f - 1.0f;
            const float v = ((float)(y % 16u) + 0.5f) / 8.0f - 1.0f;
            const float r = sqrtf(u * u + v * v);
            float alpha = 1.25f - r;
            alpha = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
            uint8_t *texel = &pixels[(y * 64u + x) * 4u];
            texel[0] = (uint8_t)(40u + (cell * 53u) % 200u);
            texel[1] = (uint8_t)(40u + (cell * 97u) % 200u);
            texel[2] = (uint8_t)(40u + (cell * 151u) % 200u);
            texel[3] = (uint8_t)(alpha * 255.0f + 0.5f);
        }
    }
    BTBHGPUTexture *texture = texture_from(window, pixels, 64u, 64u, BTBH_GPU_TEXTURE_FILTER_LINEAR,
        BTBH_GPU_TEXTURE_WRAP_CLAMP, BTBH_GPU_TEXTURE_COLOR_SPACE_SRGB);
    free(pixels);
    return texture;
}

static uint8_t *noise_pixels(uint32_t size, uint32_t seed, uint8_t low, uint8_t span) {
    uint8_t *pixels = malloc((size_t)size * size * 4u);
    assert(pixels != NULL);
    uint32_t state = seed;
    for (size_t i = 0u; i < (size_t)size * size * 4u; ++i) {
        pixels[i] = (i % 4u) == 3u ? 255u : (uint8_t)(low + smoke_random(&state) % span);
    }
    return pixels;
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

static uint32_t differing_pixels(const uint8_t *a, const uint8_t *b) {
    uint32_t count = 0u;
    for (uint32_t index = 0u; index < TEXELS; ++index) {
        count += memcmp(&a[index * 4u], &b[index * 4u], 3u) != 0 ? 1u : 0u;
    }
    return count;
}

/* ------------------------------------------------------------------------------------------------- meshes */

typedef struct MeshData {
    float *vertices;
    uint32_t *indices;
    uint32_t vertex_count;
    uint32_t index_count;
} MeshData;

static void mesh_data_free(MeshData *data) {
    free(data->vertices);
    free(data->indices);
    memset(data, 0, sizeof(*data));
}

/* A quad over local x and y in -1..1 facing +z, tangent +x (w +1), uv (0, 0) at (-1, 1) and (1, 1) at (1, -1); or,
   `ground`, over x and z at y = 0 facing up (uv (0, 0) at (-1, -1)). */
static MeshData quad_data(bool ground) {
    MeshData data;
    data.vertex_count = 4u;
    data.index_count = 6u;
    data.vertices = calloc((size_t)4u * FLOATS, sizeof(float));
    data.indices = calloc(6u, sizeof(uint32_t));
    assert(data.vertices != NULL && data.indices != NULL);
    static const float corners[4][2] = { { -1.0f, 1.0f }, { 1.0f, 1.0f }, { 1.0f, -1.0f }, { -1.0f, -1.0f } };
    for (uint32_t corner = 0u; corner < 4u; ++corner) {
        float *vertex = &data.vertices[(size_t)corner * FLOATS];
        if (ground) {
            vertex[0] = corners[corner][0];
            vertex[2] = -corners[corner][1];
            vertex[4] = 1.0f;
        } else {
            vertex[0] = corners[corner][0];
            vertex[1] = corners[corner][1];
            vertex[5] = 1.0f;
        }
        vertex[6] = 0.5f * corners[corner][0] + 0.5f;
        vertex[7] = 0.5f - 0.5f * corners[corner][1];
        vertex[8] = 1.0f;
        vertex[11] = 1.0f;
        vertex[12] = 1.0f;
        vertex[13] = 1.0f;
        vertex[14] = 1.0f;
        vertex[15] = 1.0f;
        vertex[20] = 1.0f;
    }
    /* Clockwise seen from the side it faces (the pipelines' front face). */
    static const uint32_t wound[6] = { 0u, 1u, 2u, 0u, 2u, 3u };
    memcpy(data.indices, wound, sizeof(wound));
    return data;
}

static MeshData box_data(void) {
    static const float faces[6][3] = {
        { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 },
    };
    MeshData data;
    data.vertex_count = 24u;
    data.index_count = 36u;
    data.vertices = calloc((size_t)24u * FLOATS, sizeof(float));
    data.indices = calloc(36u, sizeof(uint32_t));
    assert(data.vertices != NULL && data.indices != NULL);
    for (uint32_t face = 0u; face < 6u; ++face) {
        const float *n = faces[face];
        const float u[3] = { n[1], n[2], n[0] };
        float v[3];
        cross3(n, u, v);
        for (uint32_t corner = 0u; corner < 4u; ++corner) {
            const float su = (corner == 1u || corner == 2u) ? 1.0f : -1.0f;
            const float sv = corner >= 2u ? 1.0f : -1.0f;
            float *vertex = &data.vertices[((size_t)face * 4u + corner) * FLOATS];
            for (int axis = 0; axis < 3; ++axis) {
                vertex[axis] = n[axis] + su * u[axis] + sv * v[axis];
                vertex[3 + axis] = n[axis];
                vertex[8 + axis] = u[axis];
            }
            vertex[6] = 0.5f * su + 0.5f;
            vertex[7] = 0.5f * sv + 0.5f;
            vertex[11] = 1.0f;
            vertex[12] = 1.0f;
            vertex[13] = 1.0f;
            vertex[14] = 1.0f;
            vertex[15] = 1.0f;
            vertex[20] = 1.0f;
        }
        const uint32_t base = face * 4u;
        const uint32_t wound[6] = { base, base + 2u, base + 1u, base, base + 3u, base + 2u };
        memcpy(&data.indices[face * 6u], wound, sizeof(wound));
    }
    return data;
}

static BTBHGPUCustomMesh *create_mesh(BTBHGPUWindow *window, const MeshData *data) {
    BTBHGPUCustomMeshReport report;
    memset(&report, 0, sizeof(report));
    BTBHGPUCustomMesh *mesh = btbh_gpu_window_custom_mesh_create(window, data->vertices,
        data->vertex_count * FLOATS * (uint32_t)sizeof(float), data->vertex_count, data->indices, data->index_count,
        &report);
    if (mesh == NULL) {
        fprintf(stderr, "mesh: %s\n", report.message);
    }
    assert(mesh != NULL);
    return mesh;
}

/* ------------------------------------------------------------------------------------------------- windows */

typedef struct SmokeWindow {
    BTBHGPUWindow *window;
    BTBHGPUTexture *target;
    BTBHGPUTexture *thickness[3];       /* flat normals, thickness 0, 128 and 255 */
    BTBHGPUTexture *atlas;
    BTBHGPUTexture *atlas_normals;
    BTBHGPUTexture *maps;               /* a world-detail table's array: 2 layers */
    BTBHGPUTexture *detail_albedo;
    BTBHGPUTexture *field;              /* a heightfield */
    BTBHGPUSceneWorldDetailTableEntry entry;
    BTBHGPUSceneWorldDetailTable table;
    BTBHGPUSceneHeightLift lift;
    BTBHGPUSceneHeightLift overlay_lift;
    BTBHGPUCustomMesh *quad;
    BTBHGPUCustomMesh *ground;
    BTBHGPUCustomMesh *box;
    BTBHGPUModel *skinned;
    BTBHGPUAnimationLibrary *animation;
} SmokeWindow;

static SmokeWindow open_smoke_window(BTBHGPUDevice *device) {
    SmokeWindow smoke;
    memset(&smoke, 0, sizeof(smoke));
    const BTBHGPUWindowDescriptor descriptor = {
        .title = "BTBH lit particle smoke",
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
    smoke.thickness[0] = flat_normal_texture(smoke.window, 0u);
    smoke.thickness[1] = flat_normal_texture(smoke.window, 128u);
    smoke.thickness[2] = flat_normal_texture(smoke.window, 255u);
    smoke.atlas = atlas_texture(smoke.window);
    {
        uint8_t *pixels = noise_pixels(64u, 21u, 70u, 116u);
        smoke.atlas_normals = texture_from(smoke.window, pixels, 64u, 64u, BTBH_GPU_TEXTURE_FILTER_LINEAR,
            BTBH_GPU_TEXTURE_WRAP_CLAMP, BTBH_GPU_TEXTURE_COLOR_SPACE_LINEAR);
        free(pixels);
        pixels = noise_pixels(64u, 22u, 150u, 100u);
        smoke.detail_albedo = texture_from(smoke.window, pixels, 64u, 64u, BTBH_GPU_TEXTURE_FILTER_TRILINEAR,
            BTBH_GPU_TEXTURE_WRAP_REPEAT, BTBH_GPU_TEXTURE_COLOR_SPACE_SRGB);
        free(pixels);
    }
    {
        uint8_t *layers[2] = { noise_pixels(64u, 31u, 100u, 56u), noise_pixels(64u, 32u, 60u, 56u) };
        smoke.maps = btbh_gpu_window_texture_array_create_rgba8_ex(smoke.window, (const uint8_t *const *)layers, 64u,
            64u, 2u, 1u, BTBH_GPU_TEXTURE_FILTER_TRILINEAR, BTBH_GPU_TEXTURE_WRAP_REPEAT,
            BTBH_GPU_TEXTURE_COLOR_SPACE_LINEAR, &texture_report);
        free(layers[0]);
        free(layers[1]);
        assert(smoke.maps != NULL);
        memset(&smoke.entry, 0, sizeof(smoke.entry));
        smoke.entry.level_tile[0] = smoke.entry.level_tile[1] = 0.7f;
        smoke.entry.wall_tile[0] = smoke.entry.wall_tile[1] = 0.6f;
        smoke.entry.normal_layer = 0u;
        smoke.entry.orm_layer = 1u;
        smoke.table.map_layers = smoke.maps;
        smoke.table.entries = &smoke.entry;
        smoke.table.entry_count = 1u;
    }
    {
        float heights[FIELD_SIZE * FIELD_SIZE];
        for (uint32_t j = 0u; j < FIELD_SIZE; ++j) {
            for (uint32_t i = 0u; i < FIELD_SIZE; ++i) {
                heights[j * FIELD_SIZE + i] = 0.3f * sinf(0.6f * (float)i) * cosf(0.5f * (float)j);
            }
        }
        smoke.field = btbh_gpu_window_heightfield_create(smoke.window, heights, FIELD_SIZE, FIELD_SIZE,
            &texture_report);
        assert(smoke.field != NULL);
        /* The lattice from (-4, -4) with step 0.5, on the texel centres. */
        memset(&smoke.lift, 0, sizeof(smoke.lift));
        smoke.lift.heightfield = smoke.field;
        smoke.lift.world_to_uv[0] = 1.0f / ((float)FIELD_SIZE * 0.5f);
        smoke.lift.world_to_uv[1] = 1.0f / ((float)FIELD_SIZE * 0.5f);
        smoke.lift.world_to_uv[2] = 0.5f / (float)FIELD_SIZE + 4.0f * smoke.lift.world_to_uv[0];
        smoke.lift.world_to_uv[3] = 0.5f / (float)FIELD_SIZE + 4.0f * smoke.lift.world_to_uv[1];
        smoke.overlay_lift = smoke.lift;
        smoke.overlay_lift.height_offset = 0.05f;
        smoke.overlay_lift.flags = BTBH_GPU_SCENE_HEIGHT_LIFT_KEEP_NORMALS;
    }
    MeshData data = quad_data(false);
    smoke.quad = create_mesh(smoke.window, &data);
    mesh_data_free(&data);
    data = quad_data(true);
    smoke.ground = create_mesh(smoke.window, &data);
    mesh_data_free(&data);
    data = box_data();
    smoke.box = create_mesh(smoke.window, &data);
    mesh_data_free(&data);
    BTBHGPUModelReport model_report;
    memset(&model_report, 0, sizeof(model_report));
    smoke.skinned = btbh_gpu_window_model_load(smoke.window, BTBH_GPU_SMOKE_FIXTURE_DIR "/skinned-quad.gltf",
        &model_report);
    assert(smoke.skinned != NULL);
    BTBHGPUAnimationLibraryReport animation_report;
    memset(&animation_report, 0, sizeof(animation_report));
    smoke.animation = btbh_gpu_animation_library_load(BTBH_GPU_SMOKE_FIXTURE_DIR "/skinned-quad.gltf",
        &animation_report);
    assert(smoke.animation != NULL);
    return smoke;
}

static void close_smoke_window(SmokeWindow *smoke) {
    btbh_gpu_custom_mesh_destroy(smoke->quad);
    btbh_gpu_custom_mesh_destroy(smoke->ground);
    btbh_gpu_custom_mesh_destroy(smoke->box);
    btbh_gpu_model_destroy(smoke->skinned);
    btbh_gpu_animation_library_destroy(smoke->animation);
    for (int index = 0; index < 3; ++index) {
        btbh_gpu_texture_destroy(smoke->thickness[index]);
    }
    btbh_gpu_texture_destroy(smoke->atlas);
    btbh_gpu_texture_destroy(smoke->atlas_normals);
    btbh_gpu_texture_destroy(smoke->detail_albedo);
    btbh_gpu_texture_destroy(smoke->maps);
    btbh_gpu_texture_destroy(smoke->field);
    btbh_gpu_texture_destroy(smoke->target);
    btbh_gpu_window_destroy(smoke->window);
    memset(smoke, 0, sizeof(*smoke));
}

/* ------------------------------------------------------------------------------------------------- scenes */

typedef struct Scene {
    const char *name;
    SmokeCamera camera;
    BTBHGPUSceneLighting lighting;
    BTBHGPUScenePost post;
    float instances[MAX_INSTANCES][FLOATS];
    uint32_t instance_count;
    BTBHGPUSceneGeometryBatch batches[MAX_BATCHES];
    uint32_t batch_count;
} Scene;

static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

/* A camera at `eye` looking at the origin; ambient light alone (grey, 0.1); the post's tone map the identity. */
static void scene_begin(Scene *scene, const char *name, const float eye[3]) {
    memset(scene, 0, sizeof(*scene));
    scene->name = name;
    const float centre[3] = { 0.0f, 0.0f, 0.0f };
    scene->camera = look_at(eye, centre, 0.9f, NULL);
    scene->lighting.ambient_red = 1.0f;
    scene->lighting.ambient_green = 1.0f;
    scene->lighting.ambient_blue = 1.0f;
    scene->lighting.ambient_energy = 0.1f;
    scene->lighting.spot_range = 8.0f;
    scene->lighting.spot_falloff = 1.0f;
    scene->lighting.spot_inner_cutoff = 1.0f;
    scene->lighting.spot_outer_cutoff = 0.0f;
    BTBHGPUScenePost *post = &scene->post;
    post->tonemap_white = 1.0f;
    post->exposure = 1.0f;
    post->contrast = 1.0f;
    post->saturation = 1.0f;
    post->blur_curve = 1.0f;
    post->tonemap_mode = BTBH_GPU_SCENE_TONEMAP_REINHARD;
    post->upscale_mode = BTBH_GPU_SCENE_UPSCALE_LINEAR;
}

static const float front_eye[3] = { 0.0f, 0.0f, 5.0f };

static BTBHGPUSceneGeometryBatch base_batch(void) {
    BTBHGPUSceneGeometryBatch batch;
    memset(&batch, 0, sizeof(batch));
    batch.kind = BTBH_GPU_SCENE_GEOMETRY_CUSTOM_MESH;
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
    return batch;
}

/* The puff: a lit particle billboard (a flat quad, its normal toward the camera), straight alpha. */
static BTBHGPUSceneGeometryBatch puff_batch(const SmokeWindow *smoke) {
    BTBHGPUSceneGeometryBatch batch = base_batch();
    batch.custom_mesh = smoke->quad;
    batch.surface_variant = BTBH_GPU_SCENE_SURFACE_LIT_PARTICLE;
    batch.transparency_enabled = 1u;
    batch.blend_mode = BTBH_GPU_SCENE_BLEND_ALPHA;
    batch.billboard_mode = BTBH_GPU_SCENE_BILLBOARD_FRONT;
    batch.cull_mode = BTBH_GPU_SCENE_CULL_NONE;
    batch.depth_compare = BTBH_GPU_SCENE_DEPTH_LESS_EQUAL;
    batch.albedo_red = 200u;
    batch.albedo_green = 160u;
    batch.albedo_blue = 120u;
    batch.normal_scale = 0.0f;
    return batch;
}

static void instance_at(float out[FLOATS], float x, float y, float z, float scale, const float colour[4]) {
    memset(out, 0, FLOATS * sizeof(float));
    out[0] = scale;
    out[5] = scale;
    out[10] = scale;
    out[12] = x;
    out[13] = y;
    out[14] = z;
    out[15] = 1.0f;
    memcpy(&out[16], colour, 4u * sizeof(float));
}

static uint32_t add_instance(Scene *scene, float x, float y, float z, float scale, const float colour[4]) {
    assert(scene->instance_count < MAX_INSTANCES);
    const uint32_t index = scene->instance_count++;
    instance_at(scene->instances[index], x, y, z, scale, colour);
    return index;
}

static BTBHGPUSceneGeometryBatch *add_batch(Scene *scene, BTBHGPUSceneGeometryBatch batch) {
    assert(scene->batch_count < MAX_BATCHES);
    scene->batches[scene->batch_count] = batch;
    return &scene->batches[scene->batch_count++];
}

static BTBHGPUSceneGeometryBatch *add_puff(Scene *scene, const SmokeWindow *smoke, float x, float y, float z,
    float scale) {
    BTBHGPUSceneGeometryBatch batch = puff_batch(smoke);
    batch.first_instance = add_instance(scene, x, y, z, scale, white);
    return add_batch(scene, batch);
}

/* An orthographic sun map covering +-5 round the origin from `toward_light` (unit). */
static void sun_shadow(BTBHGPUSceneLighting *lighting, const float toward_light[3], BTBHGPUSceneShadowFilter filter) {
    const float centre[3] = { 0.0f, 0.0f, 0.0f };
    const float sun[3] = { toward_light[0] * 12.0f, toward_light[1] * 12.0f, toward_light[2] * 12.0f };
    const float rcp = 1.0f / 5.0f;
    const float range = 1.0f / (1.0f - 30.0f);
    const float orthographic[16] = {
        rcp, 0.0f, 0.0f, 0.0f,
        0.0f, rcp, 0.0f, 0.0f,
        0.0f, 0.0f, range, 0.0f,
        0.0f, 0.0f, range, 1.0f,
    };
    const SmokeCamera light = look_at(sun, centre, 1.0f, orthographic);
    memcpy(lighting->directional_shadow_view_projection, light.view_projection,
        sizeof(lighting->directional_shadow_view_projection));
    lighting->directional_shadow_enabled = 1u;
    lighting->directional_shadow_softness = 1.0f;
    lighting->directional_shadow_filter = filter;
}

/* The sun travelling along -toward_light. */
static void set_sun(BTBHGPUSceneLighting *lighting, const float toward_light[3], float energy) {
    lighting->directional_x = -toward_light[0];
    lighting->directional_y = -toward_light[1];
    lighting->directional_z = -toward_light[2];
    lighting->directional_energy = energy;
    lighting->directional_red = 1.0f;
    lighting->directional_green = 0.95f;
    lighting->directional_blue = 0.85f;
}

static void omni_shadow(BTBHGPUSceneLighting *lighting, const float eye[3]) {
    lighting->spot_shadow_enabled = 1u;
    lighting->spot_shadow_is_omni = 1u;
    lighting->spot_shadow_near = 0.1f;
    lighting->spot_shadow_softness = 1.0f;
    static const float faces[6][3] = {
        { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 },
    };
    for (uint32_t face = 0u; face < 6u; ++face) {
        float target[3] = { eye[0] + faces[face][0], eye[1] + faces[face][1], eye[2] + faces[face][2] };
        if (face == 2u || face == 3u) {
            target[0] += 0.001f;
            target[2] += 0.001f;
        }
        const SmokeCamera view = look_at(eye, target, 1.5707964f, NULL);
        memcpy(lighting->omni_shadow_view_projection[face], view.view_projection, sizeof(view.view_projection));
    }
}

static BTBHGPUResult render_with(SmokeWindow *smoke, const ShaderSet *set, const Scene *scene,
    BTBHGPUSceneReport *report) {
    memset(report, 0, sizeof(*report));
    return btbh_gpu_window_scene_render_to_target(
        smoke->window, smoke->target,
        set->paths[0], set->paths[1], set->paths[2], set->paths[3],
        set->paths[4], set->paths[5], set->paths[6], set->paths[7],
        set->paths[8], set->paths[9], set->paths[10],
        SMOKE_SHADER_FORMAT, TARGET_SIZE, TARGET_SIZE,
        scene->camera.view_projection, scene->camera.right, scene->camera.up, scene->camera.position,
        &scene->lighting, &scene->post,
        &scene->instances[0][0], scene->instance_count, scene->instance_count,
        scene->batches, NULL, NULL, scene->batch_count,
        0u, 0u, 0u, 255u, report);
}

static void dump(const char *name, const uint8_t *pixels) {
    const char *directory = SDL_getenv("BTBH_LIT_PARTICLE_DUMP");
    if (directory == NULL || directory[0] == '\0') {
        return;
    }
    char path[1200];
    (void)snprintf(path, sizeof(path), "%s/lit-%s.png", directory, name);
    assert(btbh_gpu_write_png_rgba8(path, TARGET_SIZE, TARGET_SIZE, pixels) == BTBH_GPU_OK);
}

static uint8_t *render(SmokeWindow *smoke, const Scene *scene) {
    BTBHGPUSceneReport report;
    const BTBHGPUResult result = render_with(smoke, &shaders, scene, &report);
    if (result != BTBH_GPU_OK) {
        fprintf(stderr, "%s: %s\n", scene->name, report.message);
    }
    assert(result == BTBH_GPU_OK);
    uint8_t *pixels = read_texture(smoke->window, smoke->target);
    dump(scene->name, pixels);
    return pixels;
}

/* ----------------------------------------------------------------------------------- the CPU's reference */

/* The puff's colour at `point` (flat world), normal `normal` and view `view` (unit), as
   production_particle_lit.frag.hlsl lights it: no clouds, no cascades, the sun's visibility given, every other
   light unshadowed. */
static void expected_colour(const Scene *scene, const BTBHGPUSceneGeometryBatch *puff, float thickness,
    float sun_visibility, const float point[3], const float normal[3], const float view[3], float out[3]) {
    const BTBHGPUSceneLighting *lighting = &scene->lighting;
    const float albedo[3] = {
        (float)puff->albedo_red / 255.0f, (float)puff->albedo_green / 255.0f, (float)puff->albedo_blue / 255.0f,
    };
    const float wrap = puff->surface_parameters0[3];
    const float g = puff->surface_parameters1[1];
    const float transmission =
        btbh_gpu_particle_transmission(puff->surface_parameters1[0], thickness, puff->surface_parameters1[2]);
    float light[3] = {
        lighting->ambient_red * lighting->ambient_energy,
        lighting->ambient_green * lighting->ambient_energy,
        lighting->ambient_blue * lighting->ambient_energy,
    };
    if (lighting->directional_energy > 0.0f) {
        float l[3] = { -lighting->directional_x, -lighting->directional_y, -lighting->directional_z };
        normalize3(l);
        const float back[3] = { -l[0], -l[1], -l[2] };
        const float share = btbh_gpu_particle_wrap(dot3(normal, l), wrap) +
            transmission * btbh_gpu_particle_phase(g, dot3(back, view));
        const float scale = lighting->directional_energy * sun_visibility * share / pi;
        light[0] += lighting->directional_red * scale;
        light[1] += lighting->directional_green * scale;
        light[2] += lighting->directional_blue * scale;
    }
    if (lighting->secondary_directional_energy > 0.0f) {
        float l[3] = {
            -lighting->secondary_directional_x, -lighting->secondary_directional_y, -lighting->secondary_directional_z,
        };
        normalize3(l);
        const float scale = lighting->secondary_directional_energy * btbh_gpu_particle_wrap(dot3(normal, l), wrap) / pi;
        light[0] += lighting->secondary_directional_red * scale;
        light[1] += lighting->secondary_directional_green * scale;
        light[2] += lighting->secondary_directional_blue * scale;
    }
    if (lighting->spot_energy > 0.0f && lighting->spot_range > 0.0f) {
        float delta[3] = {
            lighting->spot_position_x - point[0], lighting->spot_position_y - point[1],
            lighting->spot_position_z - point[2],
        };
        const float distance = sqrtf(dot3(delta, delta));
        normalize3(delta);
        float direction[3] = { lighting->spot_direction_x, lighting->spot_direction_y, lighting->spot_direction_z };
        normalize3(direction);
        float range = 1.0f - distance / lighting->spot_range;
        range = range < 0.0f ? 0.0f : range;
        const float range_attenuation = powf(range, lighting->spot_falloff);
        const float epsilon = fmaxf(lighting->spot_inner_cutoff - lighting->spot_outer_cutoff, 0.0001f);
        const float theta = -dot3(delta, direction);
        float t = (theta - lighting->spot_outer_cutoff) / epsilon;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        const float angular = t * t * (3.0f - 2.0f * t);
        const float scale = lighting->spot_energy * range_attenuation * angular *
            btbh_gpu_particle_wrap(dot3(normal, delta), wrap) / pi;
        light[0] += lighting->spot_red * scale;
        light[1] += lighting->spot_green * scale;
        light[2] += lighting->spot_blue * scale;
    }
    const float core_squared = fmaxf(lighting->lamp_core_radius * lighting->lamp_core_radius, 0.0001f);
    for (uint32_t lamp = 0u; lamp < lighting->lamp_count; ++lamp) {
        const float *position = lighting->lamp_position_reach[lamp];
        float delta[3] = { position[0] - point[0], position[1] - point[1], position[2] - point[2] };
        const float distance_squared = dot3(delta, delta);
        const float reach_squared = position[3] * position[3];
        const float energy = lighting->lamp_color_energy[lamp][3];
        if (distance_squared >= reach_squared || energy <= 0.0f) {
            continue;
        }
        const float fade = 1.0f - distance_squared / reach_squared;
        const float falloff = fade * fade * core_squared / (distance_squared + core_squared);
        normalize3(delta);
        const float scale = energy * falloff * btbh_gpu_particle_wrap(dot3(normal, delta), wrap) / pi;
        for (int c = 0; c < 3; ++c) {
            light[c] += lighting->lamp_color_energy[lamp][c] * scale;
        }
    }
    for (int c = 0; c < 3; ++c) {
        out[c] = albedo[c] * light[c];
    }
    /* The point lights (ceiling 0: not eased). */
    for (uint32_t index = 0u; index < lighting->point_light_count; ++index) {
        const float *p = lighting->point_lights[index];
        float to_light[3] = { p[0] - point[0], p[1] - point[1], p[2] - point[2] };
        const float distance = sqrtf(dot3(to_light, to_light));
        if (distance >= p[3]) {
            continue;
        }
        const float falloff = 1.0f - distance / p[3];
        normalize3(to_light);
        const float back[3] = { -to_light[0], -to_light[1], -to_light[2] };
        const float share = btbh_gpu_particle_wrap(dot3(normal, to_light), wrap) +
            transmission * btbh_gpu_particle_phase(g, dot3(back, view));
        for (int c = 0; c < 3; ++c) {
            out[c] += albedo[c] * p[4 + c] * p[7] * falloff * falloff * share;
        }
    }
}

/* The front-facing puff of scene_begin(front_eye) at the origin, scale 1: its world point at a pixel. */
static void puff_point(const Scene *scene, int px, int py, float point[3]) {
    const float origin[3] = { 0.0f, 0.0f, 0.0f };
    float normal[3];
    cross3(scene->camera.right, scene->camera.up, normal);
    normalize3(normal);
    const float t = pixel_on_plane(&scene->camera, px, py, origin, normal, point);
    assert(t < 1.0e30f);
}

/* Checks the picture at a pixel of the puff against the reference. */
static void expect_puff(const uint8_t *pixels, const Scene *scene, const BTBHGPUSceneGeometryBatch *puff,
    float thickness, float sun_visibility, int px, int py, const char *what) {
    float point[3];
    puff_point(scene, px, py, point);
    float normal[3];
    cross3(scene->camera.right, scene->camera.up, normal);
    normalize3(normal);
    float view[3] = {
        scene->camera.position[0] - point[0], scene->camera.position[1] - point[1],
        scene->camera.position[2] - point[2],
    };
    normalize3(view);
    float colour[3];
    expected_colour(scene, puff, thickness, sun_visibility, point, normal, view, colour);
    int worst = 0;
    int expected[3];
    for (int c = 0; c < 3; ++c) {
        expected[c] = output_level(colour[c], px, py);
        const int delta = abs((int)pixels[(py * (int)TARGET_SIZE + px) * 4 + c] - expected[c]);
        worst = delta > worst ? delta : worst;
    }
    const uint8_t *got = &pixels[(py * (int)TARGET_SIZE + px) * 4];
    printf("  %-58s (%3d, %3d): %3u %3u %3u, CPU %3d %3d %3d (linear %.4f %.4f %.4f)\n", what, px, py, got[0],
        got[1], got[2], expected[0], expected[1], expected[2], colour[0], colour[1], colour[2]);
    assert(worst <= LEVEL_TOLERANCE);
    checks += 1;
}

/* ----------------------------------------------------------------------------------------------- the light */

static void check_light(SmokeWindow *smoke) {
    static Scene scene;
    puts("the light against the CPU:");
    const int samples[3][2] = { { 40, 64 }, { 64, 64 }, { 88, 70 } };

    /* The sun front-lit (from behind the camera) and back-lit, and the sides for the wrap. */
    {
        const float toward[3] = { 0.0f, 0.0f, 1.0f };
        scene_begin(&scene, "sun-front", front_eye);
        set_sun(&scene.lighting, toward, 2.0f);
        BTBHGPUSceneGeometryBatch *puff = add_puff(&scene, smoke, 0.0f, 0.0f, 0.0f, 1.0f);
        uint8_t *pixels = render(smoke, &scene);
        for (int s = 0; s < 3; ++s) {
            expect_puff(pixels, &scene, puff, 1.0f, 1.0f, samples[s][0], samples[s][1], "sun, front-lit");
        }
        free(pixels);
    }
    {
        const float toward[3] = { 0.0f, 0.0f, -1.0f };
        scene_begin(&scene, "sun-back", front_eye);
        set_sun(&scene.lighting, toward, 0.2f);
        BTBHGPUSceneGeometryBatch *puff = add_puff(&scene, smoke, 0.0f, 0.0f, 0.0f, 1.0f);
        uint8_t *dark = render(smoke, &scene);
        expect_puff(dark, &scene, puff, 1.0f, 1.0f, 64, 64, "sun, back-lit, no transmission (ambient)");
        puff->surface_parameters1[0] = 1.0f;
        puff->surface_parameters1[1] = 0.3f;
        uint8_t *pixels = render(smoke, &scene);
        for (int s = 0; s < 3; ++s) {
            expect_puff(pixels, &scene, puff, 1.0f, 1.0f, samples[s][0], samples[s][1],
                "sun, back-lit, transmission 1, g 0.3, no thickness map");
        }
        free(pixels);
        /* The transmission through thicknesses 0, 0.5 and 1 (b 0, 128, 255; optical depth 2 at 1). */
        puff->surface_parameters1[2] = 2.0f;
        float previous = 1.0e9f;
        for (int which = 0; which < 3; ++which) {
            static const float thickness[3] = { 0.0f, 128.0f / 255.0f, 1.0f };
            static const char *const names[3] = { "sun-back-thickness-0", "sun-back-thickness-half",
                "sun-back-thickness-1" };
            puff->normal_texture = smoke->thickness[which];
            scene.name = names[which];
            pixels = render(smoke, &scene);
            char what[96];
            (void)snprintf(what, sizeof(what), "sun, back-lit, optical depth 2, thickness %.3f", thickness[which]);
            expect_puff(pixels, &scene, puff, thickness[which], 1.0f, 64, 64, what);
            const float level = (float)pixels[(64 * TARGET_SIZE + 64) * 4];
            assert(level < previous);
            previous = level;
            free(pixels);
        }
        free(dark);
    }
    {
        /* The wrap at the terminator (the sun across the puff's plane): 0 for w 0, a third for 0.5, a half for 1. */
        const float toward[3] = { 1.0f, 0.0f, 0.0f };
        static const float wraps[3] = { 0.0f, 0.5f, 1.0f };
        static const char *const names[3] = { "wrap-0", "wrap-half", "wrap-1" };
        for (int which = 0; which < 3; ++which) {
            scene_begin(&scene, names[which], front_eye);
            set_sun(&scene.lighting, toward, 1.5f);
            BTBHGPUSceneGeometryBatch *puff = add_puff(&scene, smoke, 0.0f, 0.0f, 0.0f, 1.0f);
            puff->surface_parameters0[3] = wraps[which];
            uint8_t *pixels = render(smoke, &scene);
            char what[64];
            (void)snprintf(what, sizeof(what), "sun at the terminator, wrap %.1f", wraps[which]);
            for (int s = 0; s < 3; ++s) {
                expect_puff(pixels, &scene, puff, 1.0f, 1.0f, samples[s][0], samples[s][1], what);
            }
            free(pixels);
        }
    }

    /* Point lights: in reach in front, in reach behind with the transmission, out of reach. */
    {
        scene_begin(&scene, "point-none", front_eye);
        BTBHGPUSceneGeometryBatch *puff = add_puff(&scene, smoke, 0.0f, 0.0f, 0.0f, 1.0f);
        puff->surface_parameters1[0] = 1.0f;
        puff->surface_parameters1[1] = 0.3f;
        uint8_t *none = render(smoke, &scene);
        float *point = scene.lighting.point_lights[0];
        scene.lighting.point_light_count = 1u;
        point[0] = 0.0f;
        point[1] = 0.2f;
        point[2] = 1.0f;
        point[3] = 3.0f;
        point[4] = 1.0f;
        point[5] = 0.6f;
        point[6] = 0.3f;
        point[7] = 1.2f;
        scene.name = "point-front";
        uint8_t *pixels = render(smoke, &scene);
        for (int s = 0; s < 3; ++s) {
            expect_puff(pixels, &scene, puff, 1.0f, 1.0f, samples[s][0], samples[s][1], "point light in reach, in front");
        }
        free(pixels);
        point[2] = -1.0f;
        point[7] = 0.3f;
        scene.name = "point-behind";
        pixels = render(smoke, &scene);
        for (int s = 0; s < 3; ++s) {
            expect_puff(pixels, &scene, puff, 1.0f, 1.0f, samples[s][0], samples[s][1],
                "point light in reach, behind, transmission 1, g 0.3");
        }
        free(pixels);
        point[2] = -4.0f;
        point[7] = 5.0f;
        scene.name = "point-out-of-reach";
        pixels = render(smoke, &scene);
        assert(memcmp(pixels, none, (size_t)TEXELS * 4u) == 0);
        printf("  point light out of reach: as without it, to the byte (%u px)\n", TEXELS);
        checks += 1;
        free(pixels);
        free(none);
    }

    /* The fill, the spot and a lamp: front (their wrap) and behind with the transmission on (front-only: nothing). */
    for (int light = 0; light < 4; ++light) {
        static const char *const names[4][2] = {
            { "fill-front", "fill-behind" }, { "spot-front", "spot-behind" },
            { "spot-omni-front", "spot-omni-behind" }, { "lamp-front", "lamp-behind" },
        };
        static const char *const whats[4] = { "the fill", "the spot", "the spot with the omni shadow", "a lamp" };
        scene_begin(&scene, "front-only-none", front_eye);
        BTBHGPUSceneGeometryBatch *puff = add_puff(&scene, smoke, 0.0f, 0.0f, 0.0f, 1.0f);
        puff->surface_parameters1[0] = 1.0f;
        puff->surface_parameters1[1] = 0.6f;
        if (light == 2) {
            const float eye[3] = { 0.0f, 0.3f, 2.0f };
            omni_shadow(&scene.lighting, eye);
        }
        uint8_t *none = render(smoke, &scene);
        for (int side = 0; side < 2; ++side) {
            const float z = side == 0 ? 2.0f : -2.0f;
            BTBHGPUSceneLighting *lighting = &scene.lighting;
            switch (light) {
                case 0:
                    lighting->secondary_directional_x = 0.0f;
                    lighting->secondary_directional_y = -0.2f;
                    lighting->secondary_directional_z = -z;
                    lighting->secondary_directional_energy = 0.8f;
                    lighting->secondary_directional_red = 0.6f;
                    lighting->secondary_directional_green = 0.7f;
                    lighting->secondary_directional_blue = 1.0f;
                    break;
                case 1:
                case 2:
                    lighting->spot_position_x = 0.0f;
                    lighting->spot_position_y = 0.3f;
                    lighting->spot_position_z = z;
                    lighting->spot_direction_x = 0.0f;
                    lighting->spot_direction_y = -0.15f;
                    lighting->spot_direction_z = -z;
                    lighting->spot_range = 6.0f;
                    lighting->spot_falloff = 1.0f;
                    lighting->spot_energy = 2.0f;
                    lighting->spot_red = 1.0f;
                    lighting->spot_green = 0.8f;
                    lighting->spot_blue = 0.5f;
                    lighting->spot_inner_cutoff = 0.97f;
                    lighting->spot_outer_cutoff = 0.85f;
                    break;
                default:
                    lighting->lamp_count = 1u;
                    lighting->lamp_core_radius = 0.6f;
                    lighting->lamp_position_reach[0][0] = 0.3f;
                    lighting->lamp_position_reach[0][1] = 0.4f;
                    lighting->lamp_position_reach[0][2] = z * 0.5f;
                    lighting->lamp_position_reach[0][3] = 3.0f;
                    lighting->lamp_color_energy[0][0] = 1.0f;
                    lighting->lamp_color_energy[0][1] = 0.75f;
                    lighting->lamp_color_energy[0][2] = 0.4f;
                    lighting->lamp_color_energy[0][3] = 3.0f;
                    break;
            }
            scene.name = names[light][side];
            uint8_t *pixels = render(smoke, &scene);
            if (side == 0) {
                char what[96];
                (void)snprintf(what, sizeof(what), "%s in front (its wrap, 1 / pi)", whats[light]);
                for (int s = 0; s < 3; ++s) {
                    expect_puff(pixels, &scene, puff, 1.0f, 1.0f, samples[s][0], samples[s][1], what);
                }
            } else {
                assert(memcmp(pixels, none, (size_t)TEXELS * 4u) == 0);
                printf("  %s behind, transmission 1, g 0.6: as without it, to the byte (front only)\n", whats[light]);
                checks += 1;
            }
            free(pixels);
        }
        free(none);
    }
}

/* The sun's shadow: a box off screen shadows the puff's left half. Front-lit and back-lit, under both sun filters. */
static void check_sun_shadow(SmokeWindow *smoke) {
    static Scene scene;
    puts("the sun's shadow:");
    for (int back = 0; back < 2; ++back) {
        float toward[3] = { 0.4f, 1.0f, back ? -0.6f : 0.6f };
        normalize3(toward);
        uint8_t *by_filter[2];
        for (int filter = 0; filter < 2; ++filter) {
            scene_begin(&scene, back ? (filter ? "shadow-back-soft" : "shadow-back") :
                (filter ? "shadow-front-soft" : "shadow-front"), front_eye);
            set_sun(&scene.lighting, toward, back ? 0.25f : 2.0f);
            sun_shadow(&scene.lighting, toward,
                filter ? BTBH_GPU_SCENE_SHADOW_FILTER_SOFT : BTBH_GPU_SCENE_SHADOW_FILTER_TENT);
            /* The blocker 3.5 units toward the sun from the puff's left half, x = -0.6. */
            BTBHGPUSceneGeometryBatch box = base_batch();
            box.custom_mesh = smoke->box;
            box.albedo_red = 90u;
            box.albedo_green = 110u;
            box.albedo_blue = 200u;
            box.first_instance = add_instance(&scene, -0.6f + toward[0] * 3.5f, toward[1] * 3.5f, toward[2] * 3.5f,
                0.45f, white);
            add_batch(&scene, box);
            BTBHGPUSceneGeometryBatch *puff = add_puff(&scene, smoke, 0.0f, 0.0f, 0.0f, 1.0f);
            if (back) {
                puff->surface_parameters1[0] = 1.0f;
                puff->surface_parameters1[1] = 0.3f;
            }
            uint8_t *pixels = render(smoke, &scene);
            if (filter == 0) {
                expect_puff(pixels, &scene, puff, 1.0f, 0.0f, 48, 64,
                    back ? "back-lit, the left half in the box's shadow" : "front-lit, the left half in the shadow");
                expect_puff(pixels, &scene, puff, 1.0f, 1.0f, 80, 64,
                    back ? "back-lit, the right half in the sun" : "front-lit, the right half in the sun");
            }
            by_filter[filter] = pixels;
        }
        /* (Within the puff's square: the box itself shows above it when back-lit, shaded by the filter.) */
        for (uint32_t py = 40u; py <= 88u; ++py) {
            assert(memcmp(&by_filter[0][(py * TARGET_SIZE + 40u) * 4u], &by_filter[1][(py * TARGET_SIZE + 40u) * 4u],
                49u * 4u) == 0);
        }
        printf("  %s: the tent filter and the soft filter, the same puff (four plain taps)\n",
            back ? "back-lit" : "front-lit");
        checks += 1;
        free(by_filter[0]);
        free(by_filter[1]);
    }
}

/* ------------------------------------------------------------------------------------------------- fog */

static void check_fog(SmokeWindow *smoke) {
    static Scene scene;
    puts("fog at the puff's own depth:");
    scene_begin(&scene, "fog-wall", front_eye);
    scene.lighting.ambient_energy = 0.5f;
    scene.post.fog_enabled = 1u;
    scene.post.fog_color[0] = 0.55f;
    scene.post.fog_color[1] = 0.6f;
    scene.post.fog_color[2] = 0.7f;
    scene.post.fog_density = 0.25f;
    scene.post.fog_start_distance = 2.0f;
    scene.post.fog_maximum_opacity = 0.8f;
    /* An unlit wall over the left half, at view depth 9. */
    BTBHGPUSceneGeometryBatch wall = base_batch();
    wall.custom_mesh = smoke->quad;
    wall.unlit_enabled = 1u;
    wall.albedo_red = 51u;
    wall.albedo_green = 102u;
    wall.albedo_blue = 26u;
    wall.first_instance = add_instance(&scene, -3.0f, 0.0f, -4.0f, 3.0f, white);
    add_batch(&scene, wall);
    uint8_t *without = render(smoke, &scene);
    BTBHGPUSceneGeometryBatch *puff = add_puff(&scene, smoke, 0.0f, 0.0f, 0.0f, 0.5f);
    puff->albedo_alpha = 128u;
    scene.name = "fog-puff";
    uint8_t *pixels = render(smoke, &scene);
    const float own = btbh_gpu_particle_fog_amount(&scene.post, 5.0f, 0.0f, 0.0f);
    const float behind = btbh_gpu_particle_fog_amount(&scene.post, 9.0f, 0.0f, 0.0f);
    const float alpha = 128.0f / 255.0f;
    printf("  fog's opacity at the puff (view depth 5) %.4f, at the wall (9) %.4f\n", own, behind);
    const float albedo[3] = { 200.0f / 255.0f, 160.0f / 255.0f, 120.0f / 255.0f };
    const float wall_colour[3] = { 51.0f / 255.0f, 102.0f / 255.0f, 26.0f / 255.0f };
    static const int columns[2] = { 60, 68 };
    for (int which = 0; which < 2; ++which) {
        const int px = columns[which];
        const int py = 64;
        int worst = 0;
        int worst_naive = 0;
        for (int c = 0; c < 3; ++c) {
            const float fog = scene.post.fog_color[c];
            const float colour = albedo[c] * 0.5f;
            const float background = which == 0
                ? wall_colour[c] * (1.0f - behind) + fog * behind
                : fog * scene.post.fog_maximum_opacity;
            const float expected = alpha * (colour * (1.0f - own) + fog * own) + (1.0f - alpha) * background;
            /* The composite alone: the puff fogged at the depth behind it, or not at all over the sky. */
            const float naive = which == 0
                ? (alpha * colour + (1.0f - alpha) * wall_colour[c]) * (1.0f - behind) + fog * behind
                : alpha * colour + (1.0f - alpha) * background;
            const int got = pixels[(py * (int)TARGET_SIZE + px) * 4 + c];
            const int background_got = without[(py * (int)TARGET_SIZE + px) * 4 + c];
            assert(abs(background_got - output_level(background, px, py)) <= LEVEL_TOLERANCE);
            worst = abs(got - output_level(expected, px, py)) > worst ? abs(got - output_level(expected, px, py)) : worst;
            worst_naive = abs(got - output_level(naive, px, py)) > worst_naive
                ? abs(got - output_level(naive, px, py))
                : worst_naive;
        }
        printf("  over %s: within %d levels of the puff fogged at its own depth (the composite alone: %d levels off)\n",
            which == 0 ? "the wall" : "the sky", worst, worst_naive);
        assert(worst <= LEVEL_TOLERANCE);
        assert(worst_naive > 4);
        checks += 1;
    }
    free(pixels);
    free(without);
}

/* ------------------------------------------------------------------------------------------------- cascades */

static void cascades_on(BTBHGPUScenePost *post, bool additive) {
    post->radiance_cascades.tier = BTBH_GPU_SCENE_RC_LOW;
    post->radiance_cascades.auto_frame_budget_milliseconds = 16.0f;
    post->radiance_cascades.indirect_intensity = 1.0f;
    post->radiance_cascades.hero_omni_emitter_index = -1;
    post->radiance_cascades.invalidate = 1u;
    post->radiance_cascades.additive = additive ? 1u : 0u;
}

static void check_cascades(SmokeWindow *smoke) {
    static Scene scene;
    puts("the radiance cascades:");
    const float eye[3] = { 0.0f, 1.5f, 5.0f };
    for (int mode = 0; mode < 3; ++mode) {
        /* Night: no ambient, no sun. A glowing patch on the floor under the puff. */
        static const char *const names[3] = { "cascades-off", "cascades-additive", "cascades-replacing" };
        scene_begin(&scene, names[mode], eye);
        scene.lighting.ambient_energy = 0.0f;
        if (mode != 0) {
            cascades_on(&scene.post, mode == 1);
        }
        BTBHGPUSceneGeometryBatch floor = base_batch();
        floor.custom_mesh = smoke->ground;
        floor.albedo_red = 120u;
        floor.albedo_green = 120u;
        floor.albedo_blue = 120u;
        floor.first_instance = add_instance(&scene, 0.0f, -1.2f, 0.0f, 6.0f, white);
        add_batch(&scene, floor);
        BTBHGPUSceneGeometryBatch glow = base_batch();
        glow.custom_mesh = smoke->ground;
        glow.unlit_enabled = 1u;
        glow.albedo_red = 255u;
        glow.albedo_green = 150u;
        glow.albedo_blue = 60u;
        glow.emission_red = 255u;
        glow.emission_green = 150u;
        glow.emission_blue = 60u;
        glow.emission_energy = 6.0f;
        glow.first_instance = add_instance(&scene, 0.0f, -1.18f, 0.0f, 0.6f, white);
        add_batch(&scene, glow);
        add_puff(&scene, smoke, 0.0f, 0.0f, 0.0f, 0.6f);
        uint8_t *pixels = render(smoke, &scene);
        const uint8_t *centre = &pixels[(60 * TARGET_SIZE + 64) * 4];
        printf("  %-20s the puff's centre %3u %3u %3u\n", names[mode], centre[0], centre[1], centre[2]);
        if (mode == 0) {
            assert(centre[0] <= 1 && centre[1] <= 1 && centre[2] <= 1);
        } else {
            assert(centre[0] >= 6 && centre[0] > centre[2]);
        }
        checks += 1;
        free(pixels);
    }
    /* A lit particle with an emission is no source; an unlit glow in its place is one. Both lie flat over the floor
       (the source pass projects straight down: a billboard's footprint there is a sliver). The floor outside their
       square on screen is the control. */
    uint8_t *pictures[3];
    for (int which = 0; which < 3; ++which) {
        static const char *const names[3] = { "source-none", "source-lit-emissive", "source-unlit-glow" };
        scene_begin(&scene, names[which], eye);
        /* (A little ambient: a lit surface with no light at all shows its albedo.) */
        scene.lighting.ambient_energy = 0.002f;
        cascades_on(&scene.post, true);
        BTBHGPUSceneGeometryBatch floor = base_batch();
        floor.custom_mesh = smoke->ground;
        floor.albedo_red = 140u;
        floor.albedo_green = 140u;
        floor.albedo_blue = 140u;
        floor.first_instance = add_instance(&scene, 0.0f, -0.6f, 0.0f, 6.0f, white);
        add_batch(&scene, floor);
        /* (0.8 a side's half: it covers the cascades' field texels' centres, a unit apart.) */
        BTBHGPUSceneGeometryBatch *puff = add_puff(&scene, smoke, 0.0f, -0.5f, 0.0f, 0.8f);
        puff->custom_mesh = smoke->ground;
        puff->billboard_mode = BTBH_GPU_SCENE_BILLBOARD_DISABLED;
        if (which == 1) {
            puff->emission_red = 255u;
            puff->emission_green = 200u;
            puff->emission_blue = 100u;
            puff->emission_energy = 8.0f;
        } else if (which == 2) {
            puff->surface_variant = BTBH_GPU_SCENE_SURFACE_NONE;
            puff->unlit_enabled = 1u;
            puff->transparency_enabled = 0u;
            puff->emission_red = 255u;
            puff->emission_green = 200u;
            puff->emission_blue = 100u;
            puff->emission_energy = 8.0f;
        }
        pictures[which] = render(smoke, &scene);
    }
    /* The puff's square on screen, and a margin. */
    float corner_min[2] = { 1.0e9f, 1.0e9f };
    float corner_max[2] = { -1.0e9f, -1.0e9f };
    for (int corner = 0; corner < 4; ++corner) {
        const float world[3] = { corner & 1 ? 0.8f : -0.8f, -0.5f, corner & 2 ? 0.8f : -0.8f };
        const float *m = scene.camera.view_projection;
        const float clip[4] = {
            m[0] * world[0] + m[4] * world[1] + m[8] * world[2] + m[12],
            m[1] * world[0] + m[5] * world[1] + m[9] * world[2] + m[13],
            0.0f,
            m[3] * world[0] + m[7] * world[1] + m[11] * world[2] + m[15],
        };
        const float x = (clip[0] / clip[3] * 0.5f + 0.5f) * (float)TARGET_SIZE;
        const float y = (0.5f - clip[1] / clip[3] * 0.5f) * (float)TARGET_SIZE;
        corner_min[0] = fminf(corner_min[0], x);
        corner_min[1] = fminf(corner_min[1], y);
        corner_max[0] = fmaxf(corner_max[0], x);
        corner_max[1] = fmaxf(corner_max[1], y);
    }
    uint32_t outside[2] = { 0u, 0u };
    for (uint32_t py = 0u; py < TARGET_SIZE; ++py) {
        for (uint32_t px = 0u; px < TARGET_SIZE; ++px) {
            const bool inside = (float)px >= corner_min[0] - 2.0f && (float)px <= corner_max[0] + 2.0f &&
                (float)py >= corner_min[1] - 2.0f && (float)py <= corner_max[1] + 2.0f;
            if (inside) {
                continue;
            }
            for (int which = 1; which < 3; ++which) {
                const size_t at = ((size_t)py * TARGET_SIZE + px) * 4u;
                outside[which - 1] += memcmp(&pictures[0][at], &pictures[which][at], 3u) != 0 ? 1u : 0u;
            }
        }
    }
    printf("  additive cascades: pixels outside the puff that change with a lit particle's emission %u; with an unlit "
        "glow there %u\n", outside[0], outside[1]);
    assert(outside[0] == 0u);
    assert(outside[1] > 50u);
    checks += 2;
    for (int which = 0; which < 3; ++which) {
        free(pictures[which]);
    }
}

/* ------------------------------------------------------------------------------------------------- fades */

/* The alpha a picture shows where a puff of colour `colour` (linear, grey) covers black. */
static float alpha_at(const uint8_t *pixels, int px, int py, float colour) {
    return level_linear(pixels[(py * (int)TARGET_SIZE + px) * 4], px, py) / colour;
}

static void check_fades(SmokeWindow *smoke) {
    static Scene scene;
    puts("the fades:");
    /* The soft fade: a puff standing through a black floor, seen from above, fading over 0.5 where it meets it. */
    const float eye[3] = { 0.0f, 1.2f, 5.0f };
    float soft_alpha[TARGET_SIZE * TARGET_SIZE];
    uint32_t compared = 0u;
    float worst = 0.0f;
    for (int kind = 0; kind < 2; ++kind) {
        scene_begin(&scene, kind == 0 ? "soft-fade-lit" : "soft-fade-soft-particle", eye);
        scene.lighting.ambient_energy = 0.5f;
        BTBHGPUSceneGeometryBatch floor = base_batch();
        floor.custom_mesh = smoke->ground;
        floor.unlit_enabled = 1u;
        floor.albedo_red = floor.albedo_green = floor.albedo_blue = 0u;
        floor.first_instance = add_instance(&scene, 0.0f, -0.6f, 0.0f, 6.0f, white);
        add_batch(&scene, floor);
        BTBHGPUSceneGeometryBatch *puff = add_puff(&scene, smoke, 0.0f, 0.0f, 0.0f, 1.0f);
        puff->albedo_red = puff->albedo_green = puff->albedo_blue = 255u;
        puff->surface_parameters0[0] = 0.5f;
        float colour = 0.5f;
        if (kind == 1) {
            puff->surface_variant = BTBH_GPU_SCENE_SURFACE_SOFT_PARTICLE;
            puff->unlit_enabled = 1u;
            puff->albedo_red = puff->albedo_green = puff->albedo_blue = 128u;
            colour = 128.0f / 255.0f;
        }
        uint8_t *pixels = render(smoke, &scene);
        float plane_normal[3];
        cross3(scene.camera.right, scene.camera.up, plane_normal);
        normalize3(plane_normal);
        const float origin[3] = { 0.0f, 0.0f, 0.0f };
        const float floor_point[3] = { 0.0f, -0.6f, 0.0f };
        const float floor_normal[3] = { 0.0f, 1.0f, 0.0f };
        for (int py = 0; py < (int)TARGET_SIZE; ++py) {
            for (int px = 0; px < (int)TARGET_SIZE; ++px) {
                float hit[3];
                float floor_hit[3];
                const float d = pixel_on_plane(&scene.camera, px, py, origin, plane_normal, hit);
                const float local_x = dot3(hit, scene.camera.right);
                const float local_y = dot3(hit, scene.camera.up);
                const bool on_puff = d < 1.0e30f && fabsf(local_x) < 0.97f && fabsf(local_y) < 0.97f;
                const float floor_distance = pixel_on_plane(&scene.camera, px, py, floor_point, floor_normal, floor_hit);
                const int at = py * (int)TARGET_SIZE + px;
                soft_alpha[at] = -1.0f;
                if (!on_puff || floor_distance < d + 0.02f) {
                    continue;
                }
                float expected = (floor_distance - d) / 0.5f;
                expected = expected > 1.0f ? 1.0f : expected;
                const float got = alpha_at(pixels, px, py, colour);
                if (kind == 0) {
                    soft_alpha[at] = got;
                    if (expected > 0.05f && expected < 0.95f) {
                        worst = fmaxf(worst, fabsf(got - expected));
                        compared += 1u;
                    }
                } else if (soft_alpha[at] >= 0.0f && expected > 0.05f && expected < 0.95f) {
                    assert(fabsf(got - soft_alpha[at]) <= 0.03f);
                }
            }
        }
        if (kind == 0) {
            printf("  soft fade 0.5: %u pixels fading, at most %.4f from the CPU's alpha\n", compared, worst);
            assert(compared > 100u && worst <= 0.03f);
            checks += 1;
        } else {
            printf("  soft fade 0.5 against SOFT_PARTICLE's: the same alpha within 0.03 over the fading pixels\n");
            checks += 1;
        }
        free(pixels);
    }

    /* The near fade: a puff 2 units from the camera, gone nearer than 1.8, whole from 2.3 on. */
    scene_begin(&scene, "near-fade", front_eye);
    scene.lighting.ambient_energy = 0.5f;
    BTBHGPUSceneGeometryBatch *puff = add_puff(&scene, smoke, 0.0f, 0.0f, 3.0f, 1.0f);
    puff->albedo_red = puff->albedo_green = puff->albedo_blue = 255u;
    puff->surface_parameters0[1] = 1.8f;
    puff->surface_parameters0[2] = 0.5f;
    uint8_t *pixels = render(smoke, &scene);
    compared = 0u;
    worst = 0.0f;
    const float point[3] = { 0.0f, 0.0f, 3.0f };
    const float normal[3] = { 0.0f, 0.0f, 1.0f };
    for (int py = 0; py < (int)TARGET_SIZE; py += 3) {
        for (int px = 0; px < (int)TARGET_SIZE; px += 3) {
            float hit[3];
            const float d = pixel_on_plane(&scene.camera, px, py, point, normal, hit);
            float expected = (d - 1.8f) / 0.5f;
            expected = expected < 0.0f ? 0.0f : (expected > 1.0f ? 1.0f : expected);
            if (expected <= 0.05f || expected >= 0.95f) {
                continue;
            }
            worst = fmaxf(worst, fabsf(alpha_at(pixels, px, py, 0.5f) - expected));
            compared += 1u;
        }
    }
    printf("  near fade 1.8 + 0.5: %u pixels compared, at most %.4f from the CPU's alpha\n", compared, worst);
    assert(compared > 100u && worst <= 0.03f);
    checks += 1;
    free(pixels);
}

/* ------------------------------------------------------------------------------------------------- the UV rect */

typedef enum RectPath {
    RECT_LIT_PARTICLE = 0,
    RECT_FAST_UNLIT,
    RECT_SOFT_PARTICLE,
    RECT_OPAQUE_LIT,
    RECT_LIFTED_OVERLAY,
    RECT_TABLE,
    RECT_LIFTED_TABLE,
    RECT_CASCADE_SOURCE,
    RECT_PATH_COUNT
} RectPath;

static const char *const rect_names[RECT_PATH_COUNT] = {
    "lit particles", "the fast unlit shader", "soft particles", "an opaque lit batch", "a lifted overlay",
    "a world-detail table", "a lifted table", "the cascades' source pass",
};

/* Four instances, a cell each (0, 0), (1, 0), (2, 1), (3, 3) of the 4 x 4 atlas: `rect` one batch with the UV rect,
   else four batches with uv_scale and uv_offset (the instances' custom floats the same either way). `flagless` keeps
   the one batch but leaves the flag off (the whole atlas on each). */
static void build_rect_scene(SmokeWindow *smoke, RectPath path, bool rect, bool flagless, Scene *scene) {
    static const float cells[4][2] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 2.0f, 1.0f }, { 3.0f, 3.0f } };
    const bool flat = path == RECT_LIFTED_OVERLAY || path == RECT_LIFTED_TABLE || path == RECT_CASCADE_SOURCE;
    const float top_eye[3] = { 0.0f, 4.0f, 4.0f };
    scene_begin(scene, rect_names[path], flat ? top_eye : front_eye);
    const float toward[3] = { 0.3f, 0.6f, 0.75f };
    set_sun(&scene->lighting, toward, 1.6f);
    scene->lighting.point_light_count = 1u;
    float *point = scene->lighting.point_lights[0];
    point[0] = 0.5f;
    point[1] = 0.5f;
    point[2] = 1.0f;
    point[3] = 3.0f;
    point[4] = 1.0f;
    point[5] = 0.5f;
    point[6] = 0.2f;
    point[7] = 1.0f;
    if (path == RECT_CASCADE_SOURCE) {
        cascades_on(&scene->post, true);
        scene->lighting.ambient_energy = 0.02f;
        scene->lighting.directional_energy = 0.0f;
        scene->lighting.point_light_count = 0u;
    }
    if (path == RECT_SOFT_PARTICLE || path == RECT_CASCADE_SOURCE || path == RECT_LIFTED_OVERLAY) {
        BTBHGPUSceneGeometryBatch floor = base_batch();
        floor.custom_mesh = smoke->ground;
        floor.albedo_red = 110u;
        floor.albedo_green = 120u;
        floor.albedo_blue = 100u;
        floor.first_instance = add_instance(scene, 0.0f, path == RECT_SOFT_PARTICLE ? -0.25f : -0.4f, 0.0f, 5.0f,
            white);
        add_batch(scene, floor);
    }
    BTBHGPUSceneGeometryBatch batch = base_batch();
    batch.custom_mesh = flat ? smoke->ground : smoke->quad;
    batch.albedo_texture = smoke->atlas;
    batch.cull_mode = BTBH_GPU_SCENE_CULL_NONE;
    batch.flags = BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW;
    switch (path) {
        case RECT_LIT_PARTICLE:
            batch.surface_variant = BTBH_GPU_SCENE_SURFACE_LIT_PARTICLE;
            batch.transparency_enabled = 1u;
            batch.billboard_mode = BTBH_GPU_SCENE_BILLBOARD_FRONT;
            batch.normal_texture = smoke->atlas_normals;
            batch.surface_parameters0[3] = 0.4f;
            batch.surface_parameters1[0] = 0.6f;
            batch.surface_parameters1[1] = 0.5f;
            batch.surface_parameters1[2] = 2.0f;
            break;
        case RECT_FAST_UNLIT:
            batch.unlit_enabled = 1u;
            batch.transparency_enabled = 1u;
            batch.billboard_mode = BTBH_GPU_SCENE_BILLBOARD_FRONT;
            break;
        case RECT_SOFT_PARTICLE:
            batch.surface_variant = BTBH_GPU_SCENE_SURFACE_SOFT_PARTICLE;
            batch.unlit_enabled = 1u;
            batch.transparency_enabled = 1u;
            batch.billboard_mode = BTBH_GPU_SCENE_BILLBOARD_FRONT;
            batch.surface_parameters0[0] = 0.3f;
            break;
        case RECT_OPAQUE_LIT:
            batch.normal_texture = smoke->atlas_normals;
            batch.alpha_cutoff = 0.3f;
            break;
        case RECT_LIFTED_OVERLAY:
            batch.unlit_enabled = 1u;
            batch.transparency_enabled = 1u;
            batch.height_lift = &smoke->overlay_lift;
            break;
        case RECT_TABLE:
            batch.world_detail_table = &smoke->table;
            batch.alpha_cutoff = 0.3f;
            break;
        case RECT_LIFTED_TABLE:
            batch.world_detail_table = &smoke->table;
            batch.height_lift = &smoke->lift;
            batch.alpha_cutoff = 0.3f;
            break;
        case RECT_CASCADE_SOURCE:
        default:
            /* Unlit glowing cells on the floor: the cascades' sources, lighting the floor round them. */
            batch.unlit_enabled = 1u;
            batch.alpha_cutoff = 0.3f;
            batch.albedo_red = batch.albedo_green = batch.albedo_blue = 255u;
            break;
    }
    if (path == RECT_LIFTED_OVERLAY) {
        batch.depth_compare = BTBH_GPU_SCENE_DEPTH_LESS_EQUAL;
    }
    uint32_t first = scene->instance_count;
    for (uint32_t index = 0u; index < 4u; ++index) {
        const float x = -1.5f + (float)index;
        const uint32_t at = flat
            ? add_instance(scene, x, path == RECT_CASCADE_SOURCE ? -0.38f : 0.0f, 0.2f * (float)index - 0.3f, 0.45f,
                white)
            : add_instance(scene, x, 0.1f * (float)index, 0.0f, 0.45f, white);
        float *custom = &scene->instances[at][20];
        custom[0] = 0.25f;
        custom[1] = 0.25f;
        custom[2] = 0.25f * cells[index][0];
        custom[3] = 0.25f * cells[index][1];
    }
    if (rect || flagless) {
        batch.first_instance = first;
        batch.instance_count = 4u;
        if (rect) {
            batch.flags |= BTBH_GPU_SCENE_BATCH_INSTANCE_UV_RECT;
        }
        add_batch(scene, batch);
    } else {
        for (uint32_t index = 0u; index < 4u; ++index) {
            BTBHGPUSceneGeometryBatch cell = batch;
            cell.first_instance = first + index;
            cell.instance_count = 1u;
            cell.uv_scale_x = 0.25f;
            cell.uv_scale_y = 0.25f;
            cell.uv_offset_x = 0.25f * cells[index][0];
            cell.uv_offset_y = 0.25f * cells[index][1];
            add_batch(scene, cell);
        }
    }
}

static void check_uv_rect(SmokeWindow *smoke) {
    static Scene scene;
    puts("the UV rect:");
    for (int path = 0; path < (int)RECT_PATH_COUNT; ++path) {
        build_rect_scene(smoke, (RectPath)path, true, false, &scene);
        char name[96];
        (void)snprintf(name, sizeof(name), "rect-%d-one-batch", path);
        scene.name = name;
        uint8_t *one = render(smoke, &scene);
        build_rect_scene(smoke, (RectPath)path, false, false, &scene);
        (void)snprintf(name, sizeof(name), "rect-%d-four-batches", path);
        scene.name = name;
        uint8_t *four = render(smoke, &scene);
        build_rect_scene(smoke, (RectPath)path, false, true, &scene);
        (void)snprintf(name, sizeof(name), "rect-%d-no-flag", path);
        scene.name = name;
        uint8_t *whole = render(smoke, &scene);
        const uint32_t against_four = differing_pixels(one, four);
        const uint32_t against_whole = differing_pixels(one, whole);
        printf("  %-28s one batch with the rect against four with uv_offset: %u px differ; against one without the "
            "flag: %u px\n", rect_names[path], against_four, against_whole);
        assert(against_four == 0u);
        assert(against_whole > 100u);
        checks += 2;
        free(one);
        free(four);
        free(whole);
    }
}

/* ------------------------------------------------------------------------------------------------- refusals */

static void expect_refused(SmokeWindow *smoke, Scene *scene, const char *what, const char *needle) {
    BTBHGPUSceneReport report;
    const BTBHGPUResult result = render_with(smoke, &shaders, scene, &report);
    printf("  refused %-48s %d: %s\n", what, (int)result, report.message);
    assert(result == BTBH_GPU_INVALID_ARGUMENT);
    if (needle != NULL) {
        assert(strstr(report.message, needle) != NULL);
    }
    checks += 1;
}

static void check_refusals(SmokeWindow *smoke) {
    static Scene scene;
    puts("the refusals:");
    static const int stand_in = 0;
    BTBHGPUSceneWorldDetail detail;
    memset(&detail, 0, sizeof(detail));
    detail.albedo_texture = smoke->detail_albedo;
    detail.level_tile[0] = detail.level_tile[1] = 1.0f;
    detail.wall_tile[0] = detail.wall_tile[1] = 1.0f;
    for (int field = 0; field < 11; ++field) {
        scene_begin(&scene, "refusal", front_eye);
        add_puff(&scene, smoke, -1.0f, 0.0f, 0.0f, 0.5f);
        BTBHGPUSceneGeometryBatch *puff = add_puff(&scene, smoke, 1.0f, 0.0f, 0.0f, 0.5f);
        const char *what = NULL;
        const char *needle = NULL;
        switch (field) {
            case 0: puff->transparency_enabled = 0u; what = "a lit particle, opaque"; needle = "transparency_enabled";
                break;
            case 1: puff->blend_mode = BTBH_GPU_SCENE_BLEND_PREMULTIPLIED_ALPHA; what = "a lit particle, premultiplied";
                needle = "BLEND_ALPHA"; break;
            case 2: puff->blend_mode = BTBH_GPU_SCENE_BLEND_ADDITIVE; what = "a lit particle, additive";
                needle = "BLEND_ALPHA"; break;
            case 3: puff->unlit_enabled = 1u; what = "a lit particle, unlit"; needle = "unlit_enabled"; break;
            case 4:
                puff->kind = BTBH_GPU_SCENE_GEOMETRY_MODEL;
                puff->model = smoke->skinned;
                puff->custom_mesh = NULL;
                puff->animation_library = smoke->animation;
                puff->animation_time = 0.25f;
                what = "a lit particle, skinned";
                needle = "static";
                break;
            case 5: puff->dug_by = (const BTBHGPUSdfTerrain *)(const void *)&stand_in; what = "a lit particle, dug";
                needle = "dug"; break;
            case 6: puff->world_detail = &detail; what = "a lit particle with a world detail"; needle = "world_detail";
                break;
            case 7: puff->world_detail_table = &smoke->table; what = "a lit particle with a table";
                needle = "map_layers"; break;
            case 8:
                puff->billboard_mode = BTBH_GPU_SCENE_BILLBOARD_DISABLED;
                puff->height_lift = &smoke->overlay_lift;
                what = "a lit particle, lifted";
                needle = "lifted";
                break;
            case 9: puff->detail_parameters[3] = 7.0f; what = "a lit particle with a procedural detail";
                needle = "detail_parameters"; break;
            default: puff->surface_parameters1[3] = 1.0f; what = "a lit particle, surface_parameters1[3] 1";
                needle = "surface_parameters1[3]"; break;
        }
        expect_refused(smoke, &scene, what, needle);
        expect_refused(smoke, &scene, what, "GPU scene batch 1: surface_variant LIT_PARTICLE: ");
    }
    for (int field = 0; field < 6; ++field) {
        scene_begin(&scene, "refusal", front_eye);
        BTBHGPUSceneGeometryBatch batch = base_batch();
        batch.custom_mesh = smoke->quad;
        batch.flags = BTBH_GPU_SCENE_BATCH_INSTANCE_UV_RECT;
        batch.first_instance = add_instance(&scene, 0.0f, 0.0f, 0.0f, 0.5f, white);
        BTBHGPUSceneGeometryBatch *rect = add_batch(&scene, batch);
        BTBHGPUSceneWorldDetail seeded = detail;
        BTBHGPUSceneWorldDetailTable seeded_table = smoke->table;
        const char *what = NULL;
        const char *needle = NULL;
        switch (field) {
            case 0:
                rect->kind = BTBH_GPU_SCENE_GEOMETRY_MODEL;
                rect->model = smoke->skinned;
                rect->custom_mesh = NULL;
                rect->animation_library = smoke->animation;
                rect->animation_time = 0.25f;
                rect->cull_mode = BTBH_GPU_SCENE_CULL_NONE;
                what = "the UV rect, skinned";
                needle = "static";
                break;
            case 1: rect->surface_variant = BTBH_GPU_SCENE_SURFACE_DISSOLVE; what = "the UV rect with DISSOLVE";
                needle = "custom floats"; break;
            case 2: rect->surface_variant = BTBH_GPU_SCENE_SURFACE_SHATTER; what = "the UV rect with SHATTER";
                needle = "custom floats"; break;
            case 3: rect->surface_variant = BTBH_GPU_SCENE_SURFACE_GLITTER; what = "the UV rect with GLITTER";
                needle = "custom floats"; break;
            case 4: seeded.seed_component = 1u; rect->world_detail = &seeded;
                what = "the UV rect, a world detail seeded by x"; needle = "seed_component"; break;
            default: seeded_table.seed_component = 3u; rect->world_detail_table = &seeded_table;
                what = "the UV rect, a table seeded by z"; needle = "seed_component"; break;
        }
        expect_refused(smoke, &scene, what, needle);
        expect_refused(smoke, &scene, what, "GPU scene batch 0: flags INSTANCE_UV_RECT: ");
    }
    /* Bit 4 (phase 2's) and variant 12 are unknown. */
    scene_begin(&scene, "refusal", front_eye);
    BTBHGPUSceneGeometryBatch *puff = add_puff(&scene, smoke, 0.0f, 0.0f, 0.0f, 0.5f);
    puff->flags = 1u << 4;
    expect_refused(smoke, &scene, "batch flag bit 4", "Invalid GPU model instance scene submission");
    puff->flags = 0u;
    puff->surface_variant = (BTBHGPUSceneSurfaceVariant)12;
    expect_refused(smoke, &scene, "surface variant 12", "Invalid GPU model instance scene submission");
    /* A lit particle that is a billboard, with the UV rect, a normal map, both fades and the transmission draws. */
    puff->surface_variant = BTBH_GPU_SCENE_SURFACE_LIT_PARTICLE;
    puff->flags = BTBH_GPU_SCENE_BATCH_INSTANCE_UV_RECT;
    scene.instances[puff->first_instance][20] = 1.0f;
    scene.instances[puff->first_instance][21] = 1.0f;
    puff->normal_texture = smoke->atlas_normals;
    puff->surface_parameters0[0] = 0.4f;
    puff->surface_parameters0[1] = 0.5f;
    puff->surface_parameters0[2] = 1.0f;
    puff->surface_parameters1[0] = 0.8f;
    BTBHGPUSceneReport report;
    assert(render_with(smoke, &shaders, &scene, &report) == BTBH_GPU_OK);
    checks += 1;
}

/* ------------------------------------------------------------------------------------------- prewarm */

static BTBHGPUCreationStats creation_stats(void) {
    BTBHGPUCreationStats stats;
    btbh_gpu_creation_stats(&stats);
    return stats;
}

/* The scenes prewarm covers: the puff under each shadow set, the cascades, fog, and the rect on every path. */
#define PREWARM_SCENES (6u + (uint32_t)RECT_PATH_COUNT)

static void build_prewarm_scene(SmokeWindow *smoke, uint32_t index, Scene *scene) {
    if (index >= 6u) {
        build_rect_scene(smoke, (RectPath)(index - 6u), true, false, scene);
        return;
    }
    scene_begin(scene, "prewarm", front_eye);
    const float toward[3] = { 0.4f, 1.0f, 0.6f };
    set_sun(&scene->lighting, toward, 1.0f);
    switch (index) {
        case 0:
            break;
        case 1:
            sun_shadow(&scene->lighting, toward, BTBH_GPU_SCENE_SHADOW_FILTER_TENT);
            break;
        case 2: {
            sun_shadow(&scene->lighting, toward, BTBH_GPU_SCENE_SHADOW_FILTER_TENT);
            const float eye[3] = { 0.0f, 0.3f, 2.0f };
            scene->lighting.spot_energy = 1.0f;
            omni_shadow(&scene->lighting, eye);
            break;
        }
        case 3:
            cascades_on(&scene->post, true);
            break;
        case 4:
            scene->post.fog_enabled = 1u;
            scene->post.fog_density = 0.2f;
            scene->post.fog_maximum_opacity = 0.6f;
            break;
        default:
            cascades_on(&scene->post, false);
            sun_shadow(&scene->lighting, toward, BTBH_GPU_SCENE_SHADOW_FILTER_SOFT);
            break;
    }
    BTBHGPUSceneGeometryBatch *puff = add_puff(scene, smoke, 0.0f, 0.0f, 0.0f, 1.0f);
    puff->surface_parameters0[0] = 0.3f;
    puff->depth_compare = index == 5u ? BTBH_GPU_SCENE_DEPTH_LESS : BTBH_GPU_SCENE_DEPTH_LESS_EQUAL;
}

static void check_prewarm(SmokeWindow *fresh) {
    static Scene scenes[PREWARM_SCENES];
    const BTBHGPUCreationStats before_prewarm = creation_stats();
    for (uint32_t index = 0u; index < PREWARM_SCENES; ++index) {
        build_prewarm_scene(fresh, index, &scenes[index]);
        BTBHGPUSceneReport report;
        memset(&report, 0, sizeof(report));
        const BTBHGPUResult result = btbh_gpu_window_scene_prewarm_render_to_target(fresh->window, fresh->target,
            shaders.paths[0], shaders.paths[1], shaders.paths[2], shaders.paths[3],
            shaders.paths[4], shaders.paths[5], shaders.paths[6], shaders.paths[7],
            shaders.paths[8], shaders.paths[9], shaders.paths[10],
            SMOKE_SHADER_FORMAT, TARGET_SIZE, TARGET_SIZE, &scenes[index].lighting, &scenes[index].post,
            scenes[index].batches, scenes[index].batch_count, &report);
        if (result != BTBH_GPU_OK) {
            fprintf(stderr, "prewarm %u: %s\n", index, report.message);
        }
        assert(result == BTBH_GPU_OK);
    }
    const BTBHGPUCreationStats after_prewarm = creation_stats();
    for (uint32_t index = 0u; index < PREWARM_SCENES; ++index) {
        BTBHGPUSceneReport report;
        assert(render_with(fresh, &shaders, &scenes[index], &report) == BTBH_GPU_OK);
    }
    const BTBHGPUCreationStats after_renders = creation_stats();
    printf("prewarm: %llu pipelines and %llu shaders made by prewarming %u scenes into a target; the renders then made "
        "%llu pipelines and %llu shaders\n",
        (unsigned long long)(after_prewarm.graphics_pipelines - before_prewarm.graphics_pipelines),
        (unsigned long long)(after_prewarm.shaders - before_prewarm.shaders), (unsigned)PREWARM_SCENES,
        (unsigned long long)(after_renders.graphics_pipelines - after_prewarm.graphics_pipelines),
        (unsigned long long)(after_renders.shaders - after_prewarm.shaders));
    assert(after_prewarm.graphics_pipelines > before_prewarm.graphics_pipelines);
    assert(after_renders.graphics_pipelines == after_prewarm.graphics_pipelines);
    assert(after_renders.shaders == after_prewarm.shaders);
    checks += 1;

    /* The window's own prewarm and present, under FSR 3 (the transparent pass's second target, the reactive mask).
       A first frame with the puff drawn as a plain opaque quad makes the canvas's pipelines (the scene prewarm doesn't
       make them) and the frame's other ones. */
    for (int temporal = 0; temporal < 2; ++temporal) {
        Scene *scene = &scenes[temporal == 0 ? 1u : 3u];
        scene->post.upscale_mode = temporal ? BTBH_GPU_SCENE_UPSCALE_FSR3 : BTBH_GPU_SCENE_UPSCALE_LINEAR;
        if (temporal) {
            scene->post.temporal_camera_near = 0.1f;
            scene->post.temporal_camera_far = 60.0f;
            scene->post.temporal_vertical_fov = 0.9f;
            scene->post.temporal_frame_seconds = 1.0f / 60.0f;
            scene->post.temporal_meters_per_unit = 1.0f;
            scene->post.temporal_reactive_scale = 1.0f;
        }
        BTBHGPUSceneReport report;
        {
            static Scene plain;
            plain = *scene;
            BTBHGPUSceneGeometryBatch *quad = &plain.batches[plain.batch_count - 1u];
            quad->surface_variant = BTBH_GPU_SCENE_SURFACE_NONE;
            quad->transparency_enabled = 0u;
            quad->surface_parameters0[0] = 0.0f;
            memset(&report, 0, sizeof(report));
            const BTBHGPUResult plain_result = btbh_gpu_window_scene_present_instances(fresh->window,
                shaders.paths[0], shaders.paths[1], shaders.paths[2], shaders.paths[3],
                shaders.paths[4], shaders.paths[5], shaders.paths[6], shaders.paths[7],
                shaders.paths[8], shaders.paths[9], shaders.paths[10],
                SMOKE_SHADER_FORMAT, 160u, 90u,
                plain.camera.view_projection, plain.camera.right, plain.camera.up, plain.camera.position,
                &plain.lighting, &plain.post, &plain.instances[0][0], plain.instance_count, plain.instance_count,
                plain.batches, plain.batch_count, 0u, 0u, 0u, 255u, &report);
            if (plain_result != BTBH_GPU_OK) {
                fprintf(stderr, "window present (plain): %s\n", report.message);
            }
            assert(plain_result == BTBH_GPU_OK);
            BTBHGPUCanvasPass pass;
            memset(&pass, 0, sizeof(pass));
            BTBHGPUCanvasReport canvas_report;
            memset(&canvas_report, 0, sizeof(canvas_report));
            assert(btbh_gpu_window_canvas_present(fresh->window, canvas_shader_paths[0], canvas_shader_paths[1],
                canvas_shader_paths[2], SMOKE_SHADER_FORMAT, NULL, 0u, NULL, 0u, &pass, 1u, &canvas_report) ==
                BTBH_GPU_OK);
        }
        memset(&report, 0, sizeof(report));
        assert(btbh_gpu_window_scene_prewarm(fresh->window,
            shaders.paths[0], shaders.paths[1], shaders.paths[2], shaders.paths[3],
            shaders.paths[4], shaders.paths[5], shaders.paths[6], shaders.paths[7],
            shaders.paths[8], shaders.paths[9], shaders.paths[10],
            SMOKE_SHADER_FORMAT, 160u, 90u, &scene->lighting, &scene->post, scene->batches, scene->batch_count,
            &report) == BTBH_GPU_OK);
        const BTBHGPUCreationStats after_window_prewarm = creation_stats();
        for (int frame = 0; frame < 2; ++frame) {
            memset(&report, 0, sizeof(report));
            const BTBHGPUResult result = btbh_gpu_window_scene_present_instances(fresh->window,
                shaders.paths[0], shaders.paths[1], shaders.paths[2], shaders.paths[3],
                shaders.paths[4], shaders.paths[5], shaders.paths[6], shaders.paths[7],
                shaders.paths[8], shaders.paths[9], shaders.paths[10],
                SMOKE_SHADER_FORMAT, 160u, 90u,
                scene->camera.view_projection, scene->camera.right, scene->camera.up, scene->camera.position,
                &scene->lighting, &scene->post, &scene->instances[0][0], scene->instance_count, scene->instance_count,
                scene->batches, scene->batch_count, 0u, 0u, 0u, 255u, &report);
            if (result != BTBH_GPU_OK) {
                fprintf(stderr, "window present: %s\n", report.message);
            }
            assert(result == BTBH_GPU_OK);
            BTBHGPUCanvasPass pass;
            memset(&pass, 0, sizeof(pass));
            BTBHGPUCanvasReport canvas_report;
            memset(&canvas_report, 0, sizeof(canvas_report));
            assert(btbh_gpu_window_canvas_present(fresh->window, canvas_shader_paths[0], canvas_shader_paths[1],
                canvas_shader_paths[2], SMOKE_SHADER_FORMAT, NULL, 0u, NULL, 0u, &pass, 1u, &canvas_report) ==
                BTBH_GPU_OK);
        }
        const BTBHGPUCreationStats after_presents = creation_stats();
        printf("window prewarm%s: the presents then made %llu pipelines and %llu shaders\n",
            temporal ? " under FSR 3" : "",
            (unsigned long long)(after_presents.graphics_pipelines - after_window_prewarm.graphics_pipelines),
            (unsigned long long)(after_presents.shaders - after_window_prewarm.shaders));
        assert(after_presents.graphics_pipelines == after_window_prewarm.graphics_pipelines);
        assert(after_presents.shaders == after_window_prewarm.shaders);
        checks += 1;
    }
}

/* ------------------------------------------------------------------------------------------- old shaders */

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

static void check_old_shaders(SmokeWindow *fresh) {
#if defined(__APPLE__)
    /* (SPIR-V names only: Metal libraries aren't checked.) */
    (void)fresh;
    return;
#endif
    puts("old shaders:");
    ShaderCopy copy;
    memset(&copy, 0, sizeof(copy));
    (void)snprintf(copy.from, sizeof(copy.from), "%s", BTBH_GPU_SMOKE_SHADER_DIR);
    (void)snprintf(copy.to, sizeof(copy.to), "%s/../lit_old", BTBH_GPU_SMOKE_SHADER_DIR);
    (void)SDL_CreateDirectory(copy.to);
    assert(SDL_EnumerateDirectory(copy.from, copy_shader_file, &copy));
    assert(copy.copied > 11u);
    ShaderSet old;
    shader_set_in(&old, copy.to);
    static const char *const variants[3] = {
        "production_particle_lit", "production_particle_lit_2d_shadows", "production_particle_lit_unshadowed",
    };
    char paths[3][1200];
    for (int index = 0; index < 3; ++index) {
        (void)snprintf(paths[index], sizeof(paths[index]), "%s/%s.frag.spv", copy.to, variants[index]);
        assert(SDL_RemovePath(paths[index]));
    }
    static Scene scene;
    /* The sun's shadow alone: the _2d_shadows variant. */
    build_prewarm_scene(fresh, 1u, &scene);
    BTBHGPUSceneReport report;
    assert(render_with(fresh, &old, &scene, &report) == BTBH_GPU_INVALID_ARGUMENT);
    printf("  no production_particle_lit*.frag: \"%s\"\n", report.message);
    /* (The report's message may cut a long path short: its file's name comes first.) */
    assert(strstr(report.message, "lit_old/production_particle_lit_2d_shadows") != NULL &&
        strstr(report.message, "missing") != NULL);
    checks += 1;
    /* Every other batch draws: the puff as an unlit soft particle. */
    scene.batches[0].surface_variant = BTBH_GPU_SCENE_SURFACE_SOFT_PARTICLE;
    scene.batches[0].unlit_enabled = 1u;
    assert(render_with(fresh, &old, &scene, &report) == BTBH_GPU_OK);
    /* A file of that name that isn't the lit particle's (production_model_2d_shadows.frag under it): refused. */
    char model_path[1200];
    (void)snprintf(model_path, sizeof(model_path), "%s/production_model_2d_shadows.frag.spv", copy.to);
    assert(SDL_CopyFile(model_path, paths[1]));
    build_prewarm_scene(fresh, 1u, &scene);
    assert(render_with(fresh, &old, &scene, &report) == BTBH_GPU_INVALID_ARGUMENT);
    printf("  a production_particle_lit_2d_shadows.frag that isn't the lit particle's: \"%s\"\n", report.message);
    assert(strstr(report.message, "lit_old/production_particle_lit_2d_shadows") != NULL &&
        strstr(report.message, "doesn't draw lit particles") != NULL);
    checks += 1;
    /* A static vertex shader that doesn't read the UV rect (the skinned one under its name): a rect batch fails
       naming it. */
    char vertex_path[1200];
    char animated_path[1200];
    (void)snprintf(vertex_path, sizeof(vertex_path), "%s/production_model.vert.spv", copy.to);
    (void)snprintf(animated_path, sizeof(animated_path), "%s/production_animated_model.vert.spv", copy.to);
    assert(SDL_CopyFile(animated_path, vertex_path));
    build_rect_scene(fresh, RECT_FAST_UNLIT, true, false, &scene);
    assert(render_with(fresh, &old, &scene, &report) == BTBH_GPU_INVALID_ARGUMENT);
    printf("  a production_model.vert that doesn't read the UV rect: \"%s\"\n", report.message);
    assert(strstr(report.message, "lit_old/production_model.vert") != NULL &&
        strstr(report.message, "UV rect") != NULL);
    checks += 1;
}

/* BTBH_LIT_PARTICLE_OLD_SHADER_DIR=<a release's shaders from before 0.11.0>: its production_model.vert draws unflagged
   batches as this release's does, to the byte (`now` renders with this release's shaders, `before` with the old
   vertex shader: windows of their own, since pipelines are kept by their state, not their files), and a rect batch
   is refused naming it; its production_rc_material_source.vert fails a rect batch that is the cascades' source. */
static void check_base_shaders(SmokeWindow *now, SmokeWindow *before, const char *base_directory) {
#if defined(__APPLE__)
    (void)now;
    (void)before;
    (void)base_directory;
    return;
#endif
    puts("a release's shaders from before 0.11.0:");
    static Scene scene;
    BTBHGPUSceneReport report;
    char base_path[1200];
    (void)snprintf(base_path, sizeof(base_path), "%s/production_model.vert.spv", base_directory);
    ShaderSet base = shaders;
    (void)snprintf(base.paths[0], sizeof(base.paths[0]), "%s", base_path);
    for (int path = 0; path < (int)RECT_PATH_COUNT; ++path) {
        if (path == RECT_LIFTED_OVERLAY || path == RECT_TABLE || path == RECT_LIFTED_TABLE ||
            path == RECT_CASCADE_SOURCE) {
            continue;   /* their vertex shaders are siblings of the given one */
        }
        build_rect_scene(now, (RectPath)path, false, false, &scene);
        assert(render_with(now, &shaders, &scene, &report) == BTBH_GPU_OK);
        uint8_t *current = read_texture(now->window, now->target);
        build_rect_scene(before, (RectPath)path, false, false, &scene);
        assert(render_with(before, &base, &scene, &report) == BTBH_GPU_OK);
        uint8_t *old = read_texture(before->window, before->target);
        assert(memcmp(current, old, (size_t)TEXELS * 4u) == 0);
        free(current);
        free(old);
        build_rect_scene(before, (RectPath)path, true, false, &scene);
        assert(render_with(before, &base, &scene, &report) == BTBH_GPU_INVALID_ARGUMENT);
        assert(strstr(report.message, "production_model.vert") != NULL && strstr(report.message, "UV rect") != NULL);
        checks += 2;
    }
    printf("  %s: unflagged batches as this release's production_model.vert draws them, to the byte; a rect "
        "batch refused naming it\n", base_path);
    /* The cascades' source: a copy of this release's shaders with the old production_rc_material_source.vert. */
    ShaderCopy copy;
    memset(&copy, 0, sizeof(copy));
    (void)snprintf(copy.from, sizeof(copy.from), "%s", BTBH_GPU_SMOKE_SHADER_DIR);
    (void)snprintf(copy.to, sizeof(copy.to), "%s/../lit_old_rc", BTBH_GPU_SMOKE_SHADER_DIR);
    (void)SDL_CreateDirectory(copy.to);
    assert(SDL_EnumerateDirectory(copy.from, copy_shader_file, &copy));
    char old_source[1200];
    char source_path[1200];
    (void)snprintf(old_source, sizeof(old_source), "%s/production_rc_material_source.vert.spv", base_directory);
    (void)snprintf(source_path, sizeof(source_path), "%s/production_rc_material_source.vert.spv", copy.to);
    assert(SDL_CopyFile(old_source, source_path));
    ShaderSet with_old_source;
    shader_set_in(&with_old_source, copy.to);
    build_rect_scene(before, RECT_CASCADE_SOURCE, true, false, &scene);
    assert(render_with(before, &with_old_source, &scene, &report) == BTBH_GPU_INVALID_ARGUMENT);
    printf("  an old production_rc_material_source.vert, a rect batch that is the cascades' source: \"%s\"\n",
        report.message);
    assert(strstr(report.message, "lit_old_rc/production_rc_material_source.vert") != NULL &&
        strstr(report.message, "UV rect") != NULL);
    checks += 1;
}

/* ------------------------------------------------------------------------------------------------ main */

static bool load_shader_paths(void) {
    shader_set_in(&shaders, BTBH_GPU_SMOKE_SHADER_DIR);
    for (int index = 0; index < 11; ++index) {
        FILE *file = fopen(shaders.paths[index], "rb");
        if (file == NULL) {
            fprintf(stderr, "skipped: no shader %s\n", shaders.paths[index]);
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
    const BTBHGPUDeviceDescriptor device_descriptor = {
        .backend = SMOKE_BACKEND,
        .required_features = BTBH_GPU_REQUIRED_FEATURES,
        .debug_mode = 1u,
    };
    BTBHGPUDeviceReport device_report;
    memset(&device_report, 0, sizeof(device_report));
    BTBHGPUDevice *device = btbh_gpu_device_create(&device_descriptor, &device_report);
    if (device == NULL) {
        fprintf(stderr, "skipped: no device (%s)\n", device_report.message);
        return SMOKE_SKIP;
    }
    SmokeWindow smoke = open_smoke_window(device);
    check_light(&smoke);
    check_sun_shadow(&smoke);
    check_fog(&smoke);
    check_cascades(&smoke);
    check_fades(&smoke);
    check_uv_rect(&smoke);
    check_refusals(&smoke);
    SmokeWindow fresh = open_smoke_window(device);
    check_prewarm(&fresh);
    close_smoke_window(&fresh);
    SmokeWindow old = open_smoke_window(device);
    check_old_shaders(&old);
    close_smoke_window(&old);
    const char *base_directory = SDL_getenv("BTBH_LIT_PARTICLE_OLD_SHADER_DIR");
    if (base_directory != NULL && base_directory[0] != '\0') {
        SmokeWindow before = open_smoke_window(device);
        check_base_shaders(&smoke, &before, base_directory);
        close_smoke_window(&before);
    }
    close_smoke_window(&smoke);
    btbh_gpu_device_destroy(device);
    printf("btbh_gpu_lit_particle_smoke: %d checks passed\n", checks);
    return 0;
}
