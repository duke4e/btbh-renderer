/* RFC 0012 (0.11.0): the vertex-stage height lift (BTBHGPUSceneGeometryBatch.height_lift), rendered into a target and
 * read back (hidden windows, a debug device).
 *
 * - Lifted batches against the same meshes lifted on the CPU (src/gpu/btbh_gpu_height_lift.h's copy of the shader,
 *   from the heightfield's halves) and drawn without a lift, through every pass that draws a lifted batch: a lit
 *   terrain grid with a normal map and a box standing on it (the sun's shadow, the sun's and the omni shadow, none,
 *   the opaque pass's front-to-back sort, the radiance cascades), the terrain transparent, overlays with
 *   KEEP_NORMALS 0.07 over it (unlit alpha: the fast unlit pipeline; additive; a soft particle), a refracting
 *   overlay, and the terrain with a world-detail table (production_model_detail_table_lift.vert). The two pictures
 *   agree within the GPU filter's 8-bit weights (a few pixels at edges); each differs from the unlifted picture.
 * - Be The Bullet Hell v2's overlays at night (0.10.0's KEEP_HBAO meets the lift): a lifted unlit alpha overlay
 *   (KEEP_NORMALS) by two point lights under additive cascades with BTBH_GPU_SCENE_POST_ADDITIVE_CASCADES_KEEP_HBAO
 *   and the HBAO, nothing glowing, draws with the fast unlit pipeline under the cascades: as the CPU-lifted overlay,
 *   and as the frame without the cascades, to the byte (the flag's promise).
 * - A lift whose heightfield is NULL draws byte for byte as height_lift NULL.
 * - The refusals: the heightfield's creation; the lift's fields; a heightfield that isn't one or is another
 *   window's; a heightfield in every other slot (a batch's four, a world detail, the post LUT, the canvas's texture
 *   and target, the render target, the capture destination, btbh_gpu_texture_update_rgba8); a lifted batch that is
 *   animated, a billboard, dug, or casts a shadow (and its visibility byte letting it off).
 * - Prewarm (btbh_gpu_window_scene_prewarm_render_to_target) makes the lifted pipelines: a fresh window that
 *   prewarmed every scene renders them all without making a shader or a pipeline.
 * - Old shaders: a shader directory without production_model_lift.vert fails a lifted batch naming the file, and
 *   one whose production_model_lift.vert doesn't read a heightfield is refused too; unlifted batches draw.
 * - BTBH_HEIGHT_LIFT_DUMP=<dir> writes each scene's lifted, CPU-lifted and unlifted pictures.
 *
 * It needs a GPU and the production shaders; with no device it is skipped (77). */
#undef NDEBUG

#include "btbh_gpu/btbh_gpu.h"
#include "btbh_gpu_internal.h"
#include "btbh_gpu_height_lift.h"

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
#define TARGET_SIZE 192u
#define TEXELS (TARGET_SIZE * TARGET_SIZE)
#define FLOATS 24u
#define MAX_BATCHES 16u
#define MAX_INSTANCES 16u
/* The heightfield: a lattice from (-4, -4) with step 0.25, 33 x 33 samples (on the texel centres). */
#define FIELD_SIZE 33u
#define FIELD_ORIGIN (-4.0f)
#define FIELD_STEP 0.25f
/* The terrain: a grid over [-3, 3]^2, 48 quads a side (vertices on the lattice and half-way between). */
#define TERRAIN_QUADS 48u
#define OVERLAY_QUADS 24u
/* Two pictures agree when at most this many pixels differ by more than PIXEL_TOLERANCE in a channel. */
#define PIXEL_TOLERANCE 8
#define DIFFERING_LIMIT (TEXELS / 100u)

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

static void shader_set_in(ShaderSet *set, const char *directory) {
    for (int index = 0; index < 11; ++index) {
        (void)snprintf(set->paths[index], sizeof(set->paths[index]), "%s/%s%s", directory, shader_names[index],
            SMOKE_SHADER_EXTENSION);
    }
}

typedef struct SmokeWindow {
    BTBHGPUWindow *window;
    BTBHGPUTexture *target;
    BTBHGPUTexture *albedo;
    BTBHGPUTexture *normal;
    BTBHGPUTexture *maps;               /* the table's array: 4 layers */
    BTBHGPUTexture *field;              /* the heightfield */
    float *field_texels;                /* its texels' halves as floats, 3 a texel */
    BTBHGPUSceneWorldDetailTableEntry entries[2];
    BTBHGPUSceneWorldDetailTable table;
    BTBHGPUSceneHeightLift lift;        /* the terrain's */
    BTBHGPUSceneHeightLift overlay_lift;   /* 0.07 over it, KEEP_NORMALS */
    BTBHGPUSceneHeightLift null_lift;   /* heightfield NULL */
    BTBHGPUCustomMesh *terrain;         /* flat grids at y = 0 (entry 1 in joints.x) */
    BTBHGPUCustomMesh *overlay;
    BTBHGPUCustomMesh *box;
    BTBHGPUCustomMesh *floor;           /* an unlifted plate under the transparent terrain */
    BTBHGPUModel *skinned;
    BTBHGPUAnimationLibrary *animation;
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

static uint32_t smoke_random(uint32_t *state) {
    *state = *state * 1664525u + 1013904223u;
    return *state >> 8u;
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

static BTBHGPUTexture *noise_texture(BTBHGPUWindow *window, uint32_t seed, uint8_t low, uint8_t span,
    BTBHGPUTextureColorSpace color_space) {
    uint8_t *pixels = noise_pixels(64u, seed, low, span);
    BTBHGPUTextureReport report;
    memset(&report, 0, sizeof(report));
    BTBHGPUTexture *texture = btbh_gpu_window_texture_create_rgba8_ex(window, pixels, 64u, 64u, 1u,
        BTBH_GPU_TEXTURE_FILTER_TRILINEAR, BTBH_GPU_TEXTURE_WRAP_REPEAT, color_space, &report);
    free(pixels);
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

typedef struct Difference {
    uint32_t differing;      /* pixels with a channel off by more than PIXEL_TOLERANCE */
    uint32_t any;            /* pixels that differ at all */
    int largest;
} Difference;

static Difference compare_pictures(const uint8_t *a, const uint8_t *b) {
    Difference difference;
    memset(&difference, 0, sizeof(difference));
    for (uint32_t index = 0u; index < TEXELS; ++index) {
        int worst = 0;
        for (uint32_t channel = 0u; channel < 3u; ++channel) {
            const int delta = abs((int)a[index * 4u + channel] - (int)b[index * 4u + channel]);
            worst = delta > worst ? delta : worst;
        }
        difference.any += worst > 0 ? 1u : 0u;
        difference.differing += worst > PIXEL_TOLERANCE ? 1u : 0u;
        difference.largest = worst > difference.largest ? worst : difference.largest;
    }
    return difference;
}

/* --------------------------------------------------------------------------------------- the heightfield */

/* Hills and a tilt: about -0.8..2.1 over the lattice. */
static float field_height(uint32_t i, uint32_t j) {
    return 0.8f * sinf(0.35f * (float)i) * cosf(0.3f * (float)j) + 0.04f * (float)i;
}

static void lattice_lift(BTBHGPUSceneHeightLift *lift, BTBHGPUTexture *field) {
    memset(lift, 0, sizeof(*lift));
    lift->heightfield = field;
    lift->world_to_uv[0] = 1.0f / ((float)FIELD_SIZE * FIELD_STEP);
    lift->world_to_uv[1] = 1.0f / ((float)FIELD_SIZE * FIELD_STEP);
    lift->world_to_uv[2] = 0.5f / (float)FIELD_SIZE - FIELD_ORIGIN * lift->world_to_uv[0];
    lift->world_to_uv[3] = 0.5f / (float)FIELD_SIZE - FIELD_ORIGIN * lift->world_to_uv[1];
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

/* A flat grid at y = 0 over [x0, x1] x [z0, z1], `quads` a side, facing up, tangent +x; joints.x = `entry`. */
static MeshData grid_data(float x0, float x1, float z0, float z1, uint32_t quads, float entry) {
    MeshData data;
    data.vertex_count = (quads + 1u) * (quads + 1u);
    data.index_count = quads * quads * 6u;
    data.vertices = calloc((size_t)data.vertex_count * FLOATS, sizeof(float));
    data.indices = calloc(data.index_count, sizeof(uint32_t));
    assert(data.vertices != NULL && data.indices != NULL);
    for (uint32_t l = 0u; l <= quads; ++l) {
        for (uint32_t k = 0u; k <= quads; ++k) {
            float *vertex = &data.vertices[((size_t)l * (quads + 1u) + k) * FLOATS];
            vertex[0] = x0 + (x1 - x0) * (float)k / (float)quads;
            vertex[2] = z0 + (z1 - z0) * (float)l / (float)quads;
            vertex[4] = 1.0f;
            vertex[6] = (float)k / (float)quads * 2.0f;
            vertex[7] = (float)l / (float)quads * 2.0f;
            vertex[8] = 1.0f;
            vertex[11] = 1.0f;
            vertex[12] = 1.0f;
            vertex[13] = 1.0f;
            vertex[14] = 1.0f;
            vertex[15] = 1.0f;
            vertex[16] = entry;
            vertex[20] = 1.0f;
        }
    }
    uint32_t cursor = 0u;
    for (uint32_t l = 0u; l < quads; ++l) {
        for (uint32_t k = 0u; k < quads; ++k) {
            const uint32_t a = l * (quads + 1u) + k;
            const uint32_t b = a + 1u;
            const uint32_t c = a + quads + 1u;
            const uint32_t d = c + 1u;
            /* Clockwise seen from above (the pipelines' front face). */
            const uint32_t wound[6] = { a, b, d, a, d, c };
            memcpy(&data.indices[cursor], wound, sizeof(wound));
            cursor += 6u;
        }
    }
    return data;
}

/* An axis-aligned box (centre, half sizes): 24 vertices and 36 indices, its outsides front faces. */
static MeshData box_data(const float centre[3], const float half[3]) {
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
                vertex[axis] = centre[axis] + half[axis] * (n[axis] + su * u[axis] + sv * v[axis]);
                vertex[3 + axis] = n[axis];
                vertex[8 + axis] = u[axis];
            }
            vertex[6] = 0.5f * su + 0.5f;
            vertex[7] = 0.5f * sv + 0.5f;
            vertex[11] = 1.0f;
            vertex[12] = 1.0f;
            vertex[13] = 0.9f;
            vertex[14] = 0.8f;
            vertex[15] = 1.0f;
            vertex[16] = 1.0f;
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

/* The instance's column-major matrix: a yaw, a uniform scale and a translation. */
static void instance_at(float out[FLOATS], float x, float y, float z, float scale, float yaw, const float colour[4]) {
    memset(out, 0, FLOATS * sizeof(float));
    out[0] = scale * cosf(yaw);
    out[2] = -scale * sinf(yaw);
    out[5] = scale;
    out[8] = scale * sinf(yaw);
    out[10] = scale * cosf(yaw);
    out[12] = x;
    out[13] = y;
    out[14] = z;
    out[15] = 1.0f;
    memcpy(&out[16], colour, 4u * sizeof(float));
    out[20] = 0.25f;
    out[23] = 3.0f;
}

/* The mesh in world space through `instance`, lifted on the CPU as production_model_lift.vert lifts it (with the
   heightfield's halves); its normals and tangents through the instance's cofactor (here its rotation times the scale
   squared), then tilted. Drawn with an identity instance and no lift, it is the lifted batch's picture. */
static MeshData lift_on_cpu(const SmokeWindow *smoke, const MeshData *data, const float instance[FLOATS],
    const BTBHGPUSceneHeightLift *lift) {
    MeshData lifted;
    lifted.vertex_count = data->vertex_count;
    lifted.index_count = data->index_count;
    lifted.vertices = malloc((size_t)data->vertex_count * FLOATS * sizeof(float));
    lifted.indices = malloc((size_t)data->index_count * sizeof(uint32_t));
    assert(lifted.vertices != NULL && lifted.indices != NULL);
    memcpy(lifted.vertices, data->vertices, (size_t)data->vertex_count * FLOATS * sizeof(float));
    memcpy(lifted.indices, data->indices, (size_t)data->index_count * sizeof(uint32_t));
    BTBHGPUHeightLiftUniforms uniforms;
    btbh_gpu_height_lift_pack(lift, FIELD_SIZE, FIELD_SIZE, &uniforms);
    const float scale_squared = instance[5] * instance[5];
    for (uint32_t index = 0u; index < data->vertex_count; ++index) {
        float *vertex = &lifted.vertices[(size_t)index * FLOATS];
        const float p[3] = { vertex[0], vertex[1], vertex[2] };
        float position[3];
        float normal[3];
        float tangent[3];
        for (int row = 0; row < 3; ++row) {
            position[row] = instance[row] * p[0] + instance[4 + row] * p[1] + instance[8 + row] * p[2] +
                instance[12 + row];
            normal[row] = (instance[row] * vertex[3] + instance[4 + row] * vertex[4] + instance[8 + row] * vertex[5]) *
                scale_squared / instance[5];
            tangent[row] = (instance[row] * vertex[8] + instance[4 + row] * vertex[9] + instance[8 + row] * vertex[10]) *
                scale_squared / instance[5];
        }
        float amount[3];
        btbh_gpu_height_lift_at(&uniforms, smoke->field_texels, FIELD_SIZE, FIELD_SIZE, position[0], position[2],
            amount);
        btbh_gpu_height_lift_apply(amount, position, normal, tangent);
        memcpy(&vertex[0], position, sizeof(position));
        memcpy(&vertex[3], normal, sizeof(normal));
        memcpy(&vertex[8], tangent, sizeof(tangent));
    }
    return lifted;
}

/* --------------------------------------------------------------------------------------------- the scenes */

typedef enum Shadows { SHADOWS_NONE = 0, SHADOWS_SUN = 1, SHADOWS_OMNI = 2 } Shadows;

typedef enum Layer {
    LAYER_NONE = 0,
    LAYER_UNLIT,         /* unlit alpha overlay, KEEP_NORMALS: the fast unlit pipeline */
    LAYER_ADDITIVE,      /* additive unlit overlay, KEEP_NORMALS */
    LAYER_SOFT,          /* a soft particle overlay, KEEP_NORMALS */
    LAYER_REFRACT,       /* a refracting overlay */
} Layer;

/* Additive radiance cascades with nothing glowing (0.10.0's KEEP_HBAO case), without the flag or with it. */
typedef enum Additive { ADDITIVE_OFF = 0, ADDITIVE_ON = 1, ADDITIVE_KEEP_HBAO = 2 } Additive;

typedef struct Options {
    const char *name;
    Shadows shadows;
    bool cascades;       /* the cascades (not additive), the standing box glowing */
    Additive additive;
    bool hbao;           /* the horizon-based ambient occlusion */
    bool neon;           /* two point lights by the overlay, which is no source of the cascades' light */
    bool sorted;
    bool transparent;    /* the terrain transparent over an unlifted floor */
    bool table;          /* the terrain with a world-detail table */
    Layer layer;
} Options;

static const Options gallery[] = {
    { .name = "sun", .shadows = SHADOWS_SUN },
    { .name = "omni", .shadows = SHADOWS_OMNI },
    { .name = "unshadowed", .shadows = SHADOWS_NONE },
    { .name = "sorted", .shadows = SHADOWS_SUN, .sorted = true },
    { .name = "cascades", .shadows = SHADOWS_SUN, .cascades = true },
    { .name = "transparent", .shadows = SHADOWS_SUN, .transparent = true },
    { .name = "unlit-overlay", .shadows = SHADOWS_SUN, .layer = LAYER_UNLIT },
    { .name = "additive-overlay", .shadows = SHADOWS_NONE, .layer = LAYER_ADDITIVE },
    { .name = "soft-overlay", .shadows = SHADOWS_SUN, .layer = LAYER_SOFT },
    { .name = "refraction", .shadows = SHADOWS_SUN, .layer = LAYER_REFRACT },
    { .name = "table", .shadows = SHADOWS_SUN, .table = true },
    { .name = "table-omni", .shadows = SHADOWS_OMNI, .table = true },
    { .name = "keep-hbao-overlay", .shadows = SHADOWS_SUN, .additive = ADDITIVE_KEEP_HBAO, .hbao = true,
      .neon = true, .layer = LAYER_UNLIT },
};
#define GALLERY_COUNT (sizeof(gallery) / sizeof(gallery[0]))

/* How a scene's lifted batches are drawn: by the GPU's lift, on meshes lifted on the CPU, or not lifted at all. */
typedef enum Mode { MODE_LIFTED = 0, MODE_CPU = 1, MODE_FLAT = 2, MODE_NULL_LIFT = 3 } Mode;

typedef struct Scene {
    const char *name;
    SmokeCamera camera;
    BTBHGPUSceneLighting lighting;
    BTBHGPUScenePost post;
    float instances[MAX_INSTANCES][FLOATS];
    uint32_t instance_count;
    BTBHGPUSceneGeometryBatch batches[MAX_BATCHES];
    uint32_t batch_count;
    /* MODE_CPU's meshes, destroyed with the scene. */
    BTBHGPUCustomMesh *cpu_meshes[4];
    uint32_t cpu_mesh_count;
} Scene;

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

static uint32_t add_instance(Scene *scene, float x, float y, float z, float scale, float yaw, const float colour[4]) {
    assert(scene->instance_count < MAX_INSTANCES);
    const uint32_t index = scene->instance_count++;
    instance_at(scene->instances[index], x, y, z, scale, yaw, colour);
    return index;
}

static void add_batch(Scene *scene, BTBHGPUSceneGeometryBatch batch) {
    assert(scene->batch_count < MAX_BATCHES);
    scene->batches[scene->batch_count++] = batch;
}

static BTBHGPUSceneLighting scene_lighting(Shadows shadows) {
    BTBHGPUSceneLighting lighting;
    memset(&lighting, 0, sizeof(lighting));
    lighting.ambient_red = 0.8f;
    lighting.ambient_green = 0.85f;
    lighting.ambient_blue = 1.0f;
    lighting.ambient_energy = 0.35f;
    const float sun[3] = { 4.0f, 9.0f, 3.0f };
    lighting.directional_x = -sun[0];
    lighting.directional_y = -sun[1];
    lighting.directional_z = -sun[2];
    lighting.directional_energy = 1.8f;
    lighting.directional_red = 1.0f;
    lighting.directional_green = 0.95f;
    lighting.directional_blue = 0.9f;
    lighting.spot_range = 8.0f;
    lighting.spot_falloff = 1.0f;
    lighting.spot_inner_cutoff = 1.0f;
    lighting.spot_outer_cutoff = 0.0f;
    if (shadows != SHADOWS_NONE) {
        const float centre[3] = { 0.0f, 0.0f, 0.0f };
        const float rcp = 1.0f / 5.0f;
        const float range = 1.0f / (1.0f - 30.0f);
        const float orthographic[16] = {
            rcp, 0.0f, 0.0f, 0.0f,
            0.0f, rcp, 0.0f, 0.0f,
            0.0f, 0.0f, range, 0.0f,
            0.0f, 0.0f, range, 1.0f,
        };
        const SmokeCamera light = look_at(sun, centre, 1.0f, orthographic);
        memcpy(lighting.directional_shadow_view_projection, light.view_projection,
            sizeof(lighting.directional_shadow_view_projection));
        lighting.directional_shadow_enabled = 1u;
        lighting.directional_shadow_softness = 1.0f;
        lighting.directional_shadow_filter = BTBH_GPU_SCENE_SHADOW_FILTER_SOFT;
    }
    if (shadows == SHADOWS_OMNI) {
        const float eye[3] = { 0.4f, 3.2f, 0.6f };
        lighting.spot_position_x = eye[0];
        lighting.spot_position_y = eye[1];
        lighting.spot_position_z = eye[2];
        lighting.spot_range = 7.0f;
        lighting.spot_direction_y = -1.0f;
        lighting.spot_energy = 2.0f;
        lighting.spot_red = 1.0f;
        lighting.spot_green = 0.7f;
        lighting.spot_blue = 0.4f;
        lighting.spot_inner_cutoff = 0.2f;
        lighting.spot_outer_cutoff = -1.0f;
        lighting.spot_shadow_enabled = 1u;
        lighting.spot_shadow_is_omni = 1u;
        lighting.spot_shadow_near = 0.1f;
        lighting.spot_shadow_softness = 1.0f;
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
            memcpy(lighting.omni_shadow_view_projection[face], view.view_projection, sizeof(view.view_projection));
        }
    }
    return lighting;
}

static MeshData terrain_data(void) {
    return grid_data(-3.0f, 3.0f, -3.0f, 3.0f, TERRAIN_QUADS, 1.0f);
}

static MeshData overlay_data(void) {
    return grid_data(-1.6f, 1.4f, -1.2f, 1.0f, OVERLAY_QUADS, 1.0f);
}

static MeshData standing_box_data(void) {
    const float centre[3] = { 0.0f, 0.5f, 0.0f };
    const float half[3] = { 0.5f, 0.5f, 0.5f };
    return box_data(centre, half);
}

/* A lifted batch of `scene` in `mode`: the GPU's lift, the CPU-lifted mesh (made now) with an identity instance, the
   flat mesh unlifted, or a lift whose heightfield is NULL. */
static void add_lifted(SmokeWindow *smoke, Scene *scene, BTBHGPUSceneGeometryBatch batch, BTBHGPUCustomMesh *mesh,
    const MeshData *data, const float instance[FLOATS], const BTBHGPUSceneHeightLift *lift, Mode mode) {
    if (mode == MODE_CPU) {
        MeshData lifted = lift_on_cpu(smoke, data, instance, lift);
        assert(scene->cpu_mesh_count < 4u);
        BTBHGPUCustomMesh *cpu = create_mesh(smoke->window, &lifted);
        scene->cpu_meshes[scene->cpu_mesh_count++] = cpu;
        mesh_data_free(&lifted);
        batch.custom_mesh = cpu;
        batch.first_instance = add_instance(scene, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, &instance[16]);
        batch.height_lift = NULL;
    } else {
        batch.custom_mesh = mesh;
        assert(scene->instance_count < MAX_INSTANCES);
        batch.first_instance = scene->instance_count;
        memcpy(scene->instances[scene->instance_count++], instance, FLOATS * sizeof(float));
        batch.height_lift = mode == MODE_LIFTED ? lift : (mode == MODE_NULL_LIFT ? &smoke->null_lift : NULL);
    }
    add_batch(scene, batch);
}

static void destroy_scene_meshes(Scene *scene) {
    for (uint32_t index = 0u; index < scene->cpu_mesh_count; ++index) {
        btbh_gpu_custom_mesh_destroy(scene->cpu_meshes[index]);
    }
    scene->cpu_mesh_count = 0u;
}

static void build_scene(SmokeWindow *smoke, const Options *options, Mode mode, Scene *scene) {
    memset(scene, 0, sizeof(*scene));
    scene->name = options->name;
    const float eye[3] = { 0.3f, 5.2f, 5.6f };
    const float centre[3] = { 0.0f, 0.4f, -0.3f };
    scene->camera = look_at(eye, centre, 0.95f, NULL);
    scene->lighting = scene_lighting(options->shadows);
    BTBHGPUScenePost *post = &scene->post;
    post->tonemap_white = 1.0f;
    post->exposure = 1.0f;
    post->contrast = 1.0f;
    post->saturation = 1.0f;
    post->blur_curve = 1.0f;
    post->tonemap_mode = BTBH_GPU_SCENE_TONEMAP_REINHARD;
    post->upscale_mode = BTBH_GPU_SCENE_UPSCALE_LINEAR;
    post->opaque_sort = options->sorted ? (uint8_t)BTBH_GPU_SCENE_OPAQUE_SORT_FRONT_TO_BACK : 0u;
    if (options->cascades || options->additive != ADDITIVE_OFF) {
        post->radiance_cascades.tier = BTBH_GPU_SCENE_RC_LOW;
        post->radiance_cascades.auto_frame_budget_milliseconds = 16.0f;
        post->radiance_cascades.indirect_intensity = 1.0f;
        post->radiance_cascades.hero_omni_emitter_index = -1;
        post->radiance_cascades.invalidate = 1u;
        post->radiance_cascades.additive = options->additive != ADDITIVE_OFF ? 1u : 0u;
    }
    if (options->additive == ADDITIVE_KEEP_HBAO) {
        post->flags = (uint8_t)BTBH_GPU_SCENE_POST_ADDITIVE_CASCADES_KEEP_HBAO;
    }
    if (options->hbao) {
        post->ssao_enabled = 2u;
        post->ssao_radius = 1.2f;
        post->ssao_intensity = 1.5f;
        post->ssao_power = 1.0f;
        post->ssao_bias = 0.03f;
    }
    if (options->neon) {
        /* Be The Bullet Hell v2's neon signs at night: point lights of reach 2.6 and energy 2.1, 0.15 over the lifted
           overlay. They light the lit terrain; the fast unlit shader adds none to the overlay. */
        static const float spots[2][2] = { { -0.6f, -0.3f }, { 0.7f, 0.4f } };
        static const float colours[2][3] = { { 1.0f, 0.35f, 0.85f }, { 0.3f, 0.9f, 1.0f } };
        scene->lighting.point_light_count = 2u;
        for (uint32_t light = 0u; light < 2u; ++light) {
            float *point = scene->lighting.point_lights[light];
            point[0] = spots[light][0];
            point[1] = btbh_gpu_height_lift_height(&smoke->overlay_lift, smoke->field->heights, FIELD_SIZE, FIELD_SIZE,
                spots[light][0], spots[light][1]) + 0.15f;
            point[2] = spots[light][1];
            point[3] = 2.6f;
            memcpy(&point[4], colours[light], sizeof(colours[light]));
            point[7] = 2.1f;
        }
    }
    static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    static const float stone[4] = { 0.75f, 0.72f, 0.66f, 1.0f };
    static const float blue[4] = { 0.3f, 0.5f, 1.0f, 1.0f };

    if (options->transparent) {
        /* An unlifted floor under the see-through terrain. */
        BTBHGPUSceneGeometryBatch floor = base_batch();
        floor.custom_mesh = smoke->floor;
        floor.albedo_red = 70u;
        floor.albedo_green = 90u;
        floor.albedo_blue = 60u;
        floor.first_instance = add_instance(scene, 0.0f, -1.3f, 0.0f, 1.0f, 0.0f, white);
        add_batch(scene, floor);
    }

    /* The terrain: lit, a normal map (its tangents tilt), casting no shadow (the rule for a lifted batch). */
    MeshData terrain = terrain_data();
    BTBHGPUSceneGeometryBatch ground = base_batch();
    ground.albedo_texture = smoke->albedo;
    ground.normal_texture = smoke->normal;
    ground.roughness = 0.7f;
    ground.flags = BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW;
    if (options->transparent) {
        ground.transparency_enabled = 1u;
        ground.albedo_alpha = 170u;
        ground.depth_compare = BTBH_GPU_SCENE_DEPTH_LESS_EQUAL;
    }
    if (options->table) {
        ground.world_detail_table = &smoke->table;
    }
    float instance[FLOATS];
    instance_at(instance, 0.15f, 0.0f, -0.1f, 1.0f, 0.2f, stone);
    add_lifted(smoke, scene, ground, smoke->terrain, &terrain, instance, &smoke->lift, mode);
    mesh_data_free(&terrain);

    /* A box standing on it, lifted vertex by vertex (its walls keep their normals, its top tilts); emissive under the
       cascades (their material sources take it unlifted, projected straight down). */
    MeshData box = standing_box_data();
    BTBHGPUSceneGeometryBatch block = base_batch();
    block.albedo_red = 200u;
    block.albedo_green = 150u;
    block.albedo_blue = 120u;
    block.flags = BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW;
    if (options->cascades) {
        block.emission_red = 255u;
        block.emission_green = 160u;
        block.emission_blue = 80u;
        block.emission_energy = 2.5f;
    }
    if (options->table) {
        block.world_detail_table = &smoke->table;
    }
    instance_at(instance, 1.1f, 0.0f, -0.9f, 0.7f, 0.5f, white);
    add_lifted(smoke, scene, block, smoke->box, &box, instance, &smoke->lift, mode);
    mesh_data_free(&box);

    /* An unlifted box above, casting the sun's (and the omni) shadow onto the lifted terrain. */
    BTBHGPUSceneGeometryBatch caster = base_batch();
    caster.custom_mesh = smoke->box;
    caster.albedo_red = 90u;
    caster.albedo_green = 110u;
    caster.albedo_blue = 200u;
    caster.spot_shadow_visible = 1u;
    caster.first_instance = add_instance(scene, -0.7f, 2.4f, 0.4f, 0.5f, 0.3f, white);
    add_batch(scene, caster);

    if (options->layer != LAYER_NONE) {
        MeshData overlay = overlay_data();
        BTBHGPUSceneGeometryBatch layer = base_batch();
        layer.transparency_enabled = 1u;
        layer.depth_compare = BTBH_GPU_SCENE_DEPTH_LESS_EQUAL;
        layer.cull_mode = BTBH_GPU_SCENE_CULL_NONE;
        layer.flags = BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW;
        if (options->neon) {
            /* It draws over the frame and lights nothing: under the cascades nothing glows. */
            layer.flags |= BTBH_GPU_SCENE_BATCH_NO_RC_SOURCE;
        }
        const BTBHGPUSceneHeightLift *lift = &smoke->overlay_lift;
        switch (options->layer) {
            case LAYER_UNLIT:
                layer.unlit_enabled = 1u;
                layer.albedo_red = 255u;
                layer.albedo_green = 80u;
                layer.albedo_blue = 40u;
                layer.albedo_alpha = 140u;
                break;
            case LAYER_ADDITIVE:
                layer.unlit_enabled = 1u;
                layer.blend_mode = BTBH_GPU_SCENE_BLEND_ADDITIVE;
                layer.albedo_red = 60u;
                layer.albedo_green = 140u;
                layer.albedo_blue = 255u;
                layer.albedo_alpha = 160u;
                break;
            case LAYER_SOFT:
                layer.surface_variant = BTBH_GPU_SCENE_SURFACE_SOFT_PARTICLE;
                layer.surface_parameters0[0] = 0.3f;
                layer.albedo_red = 240u;
                layer.albedo_green = 240u;
                layer.albedo_blue = 120u;
                layer.albedo_alpha = 180u;
                break;
            case LAYER_REFRACT:
            default:
                layer.surface_variant = BTBH_GPU_SCENE_SURFACE_REFRACT;
                layer.surface_parameters0[0] = 12.0f;
                layer.surface_parameters0[2] = 2.0f;
                layer.albedo_red = 230u;
                layer.albedo_green = 240u;
                layer.albedo_blue = 255u;
                layer.albedo_alpha = 255u;
                lift = &smoke->lift;   /* the refraction tilts with the ground */
                break;
        }
        instance_at(instance, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, options->layer == LAYER_ADDITIVE ? blue : white);
        add_lifted(smoke, scene, layer, smoke->overlay, &overlay, instance, lift, mode);
        mesh_data_free(&overlay);
    }
}

static BTBHGPUResult render_with(SmokeWindow *smoke, const ShaderSet *set, const Scene *scene,
    const uint8_t *directional_visibility, BTBHGPUSceneReport *report) {
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
        scene->batches, NULL, directional_visibility, scene->batch_count,
        24u, 26u, 34u, 255u, report);
}

static uint8_t *render(SmokeWindow *smoke, const Scene *scene) {
    BTBHGPUSceneReport report;
    const BTBHGPUResult result = render_with(smoke, &shaders, scene, NULL, &report);
    if (result != BTBH_GPU_OK) {
        fprintf(stderr, "%s: %s\n", scene->name, report.message);
    }
    assert(result == BTBH_GPU_OK);
    return read_texture(smoke->window, smoke->target);
}

static void dump(const char *name, const char *mode, const uint8_t *pixels) {
    const char *directory = SDL_getenv("BTBH_HEIGHT_LIFT_DUMP");
    if (directory == NULL || directory[0] == '\0') {
        return;
    }
    char path[1200];
    (void)snprintf(path, sizeof(path), "%s/lift-%s-%s.png", directory, name, mode);
    assert(btbh_gpu_write_png_rgba8(path, TARGET_SIZE, TARGET_SIZE, pixels) == BTBH_GPU_OK);
}

/* ----------------------------------------------------------------------------------------------- the checks */

static void check_gallery(SmokeWindow *smoke) {
    static Scene scene;
    uint32_t worst_differing = 0u;
    for (uint32_t index = 0u; index < GALLERY_COUNT; ++index) {
        const Options *options = &gallery[index];
        build_scene(smoke, options, MODE_LIFTED, &scene);
        uint8_t *lifted = render(smoke, &scene);
        uint8_t *again = render(smoke, &scene);
        assert(memcmp(lifted, again, (size_t)TEXELS * 4u) == 0);
        free(again);
        build_scene(smoke, options, MODE_CPU, &scene);
        uint8_t *cpu = render(smoke, &scene);
        destroy_scene_meshes(&scene);
        build_scene(smoke, options, MODE_FLAT, &scene);
        uint8_t *flat = render(smoke, &scene);
        const Difference against_cpu = compare_pictures(lifted, cpu);
        const Difference against_flat = compare_pictures(lifted, flat);
        printf("lifted %-17s vs CPU-lifted: %5u px differ (%4u by more than %d, largest %3d); vs unlifted: %5u px\n",
            options->name, against_cpu.any, against_cpu.differing, PIXEL_TOLERANCE, against_cpu.largest,
            against_flat.any);
        dump(options->name, "gpu", lifted);
        dump(options->name, "cpu", cpu);
        dump(options->name, "flat", flat);
        assert(against_cpu.differing <= DIFFERING_LIMIT);
        assert(against_flat.differing > TEXELS / 10u);
        worst_differing = against_cpu.differing > worst_differing ? against_cpu.differing : worst_differing;
        if (index == 0u) {
            /* A lift whose heightfield is NULL draws byte for byte as no lift. */
            build_scene(smoke, options, MODE_NULL_LIFT, &scene);
            uint8_t *null_lift = render(smoke, &scene);
            assert(memcmp(null_lift, flat, (size_t)TEXELS * 4u) == 0);
            free(null_lift);
            printf("lift with heightfield NULL: byte for byte as height_lift NULL (%u px)\n", TEXELS);
        }
        free(lifted);
        free(cpu);
        free(flat);
    }
    printf("gallery: %u scenes; lifted and CPU-lifted agree (at most %u px off by more than %d)\n",
        (unsigned)GALLERY_COUNT, worst_differing, PIXEL_TOLERANCE);
}

/* Be The Bullet Hell v2's overlays at night, where 0.10.0's KEEP_HBAO meets the lift: the gallery's
   keep-hbao-overlay, a lifted unlit alpha overlay (KEEP_NORMALS, no source of the cascades' light) by two point
   lights, under additive cascades with BTBH_GPU_SCENE_POST_ADDITIVE_CASCADES_KEEP_HBAO and the HBAO, nothing glowing.
   Under the flag a transparent unlit batch takes the fast unlit shader under the cascades too (0.10.0), lifted with
   production_model_lift.vert, and that shader adds no point light: so the frame is the frame without the cascades,
   to the byte, as the flag promises, the overlay lifted on the GPU as on the CPU. Under the omni shadow a pixel or two
   may differ by a level (the scene shader's two variants round that light apart, as in 0.10.0). Without the flag
   the cascades' scene shader draws the overlay, brightened by the point lights, and their HBAO darkens more
   (printed). */
static void check_keep_hbao_overlay(SmokeWindow *smoke) {
    static Scene scene;
    static const char *const shadow_names[3] = { "no shadow", "the sun's shadow", "the omni shadow" };
    static const char *const mode_names[2] = { "lifted", "CPU-lifted" };
    static const char *const dump_modes[2] = { "gpu", "cpu" };
    for (uint32_t shadows = 0u; shadows < 3u; ++shadows) {
        uint8_t *kept_by_mode[2];
        for (uint32_t mode = 0u; mode < 2u; ++mode) {
            Options options = { .name = "keep-hbao-overlay", .shadows = (Shadows)shadows, .hbao = true,
                .neon = true, .layer = LAYER_UNLIT };
            uint8_t *pictures[3];
            for (uint32_t additive = 0u; additive < 3u; ++additive) {
                options.additive = (Additive)additive;
                build_scene(smoke, &options, (Mode)mode, &scene);
                pictures[additive] = render(smoke, &scene);
                destroy_scene_meshes(&scene);
            }
            const uint8_t *off = pictures[ADDITIVE_OFF];
            const Difference kept = compare_pictures(pictures[ADDITIVE_KEEP_HBAO], off);
            const Difference without = compare_pictures(pictures[ADDITIVE_ON], off);
            printf("KEEP_HBAO, %s, a %s overlay by point lights: additive cascades with it against none %u px "
                "differ (largest %d); without it %u px\n", shadow_names[shadows], mode_names[mode], kept.any,
                kept.largest, without.any);
            if (shadows == (uint32_t)SHADOWS_OMNI) {
                assert(kept.any <= 4u && kept.largest <= 1);
            } else {
                assert(memcmp(pictures[ADDITIVE_KEEP_HBAO], off, (size_t)TEXELS * 4u) == 0);
            }
            assert(without.differing > 0u);
            if (shadows == (uint32_t)SHADOWS_SUN) {
                dump("keep-hbao-overlay-off", dump_modes[mode], off);
                dump("keep-hbao-overlay-additive", dump_modes[mode], pictures[ADDITIVE_ON]);
            }
            kept_by_mode[mode] = pictures[ADDITIVE_KEEP_HBAO];
            free(pictures[ADDITIVE_OFF]);
            free(pictures[ADDITIVE_ON]);
        }
        const Difference against_cpu = compare_pictures(kept_by_mode[0], kept_by_mode[1]);
        printf("KEEP_HBAO, %s: the lifted overlay against the CPU-lifted one %u px differ (%u by more than %d, "
            "largest %d)\n", shadow_names[shadows], against_cpu.any, against_cpu.differing, PIXEL_TOLERANCE,
            against_cpu.largest);
        assert(against_cpu.differing <= DIFFERING_LIMIT);
        free(kept_by_mode[0]);
        free(kept_by_mode[1]);
    }
}

static void expect_refused(SmokeWindow *smoke, Scene *scene, const uint8_t *visibility, const char *what,
    const char *needle) {
    BTBHGPUSceneReport report;
    const BTBHGPUResult result = render_with(smoke, &shaders, scene, visibility, &report);
    printf("refused %-52s %d: %s\n", what, (int)result, report.message);
    assert(result == BTBH_GPU_INVALID_ARGUMENT);
    if (needle != NULL) {
        assert(strstr(report.message, needle) != NULL);
    }
}

static void check_refusals(SmokeWindow *smoke, SmokeWindow *other) {
    BTBHGPUTextureReport texture_report;
    /* The heightfield's creation. */
    float heights[16];
    for (uint32_t index = 0u; index < 16u; ++index) {
        heights[index] = (float)index * 0.1f;
    }
    assert(btbh_gpu_window_heightfield_create(smoke->window, heights, 4u, 4u, NULL) == NULL);
    struct {
        const float *heights;
        uint32_t width;
        uint32_t height;
        const char *what;
    } bad[] = {
        { NULL, 4u, 4u, "no heights" },
        { heights, 1u, 4u, "1 wide" },
        { heights, 4u, 1u, "1 high" },
        { heights, 4097u, 2u, "4097 wide" },
        { heights, 2u, 4097u, "4097 high" },
    };
    for (size_t index = 0u; index < sizeof(bad) / sizeof(bad[0]); ++index) {
        memset(&texture_report, 0, sizeof(texture_report));
        assert(btbh_gpu_window_heightfield_create(smoke->window, bad[index].heights, bad[index].width,
            bad[index].height, &texture_report) == NULL);
        printf("refused heightfield, %-41s %d: %s\n", bad[index].what, (int)texture_report.result,
            texture_report.message);
        assert(texture_report.result == BTBH_GPU_INVALID_ARGUMENT && texture_report.message[0] != '\0');
    }
    heights[5] = NAN;
    assert(btbh_gpu_window_heightfield_create(smoke->window, heights, 4u, 4u, &texture_report) == NULL);
    printf("refused heightfield, a NaN height                          %d: %s\n", (int)texture_report.result,
        texture_report.message);
    assert(texture_report.result == BTBH_GPU_INVALID_ARGUMENT && strstr(texture_report.message, "(1, 1)") != NULL);
    heights[5] = 40000.0f;
    assert(btbh_gpu_window_heightfield_create(smoke->window, heights, 4u, 4u, &texture_report) == NULL);
    assert(texture_report.result == BTBH_GPU_INVALID_ARGUMENT);
    heights[5] = 0.5f;
    BTBHGPUTexture *small = btbh_gpu_window_heightfield_create(smoke->window, heights, 4u, 4u, &texture_report);
    assert(small != NULL && texture_report.result == BTBH_GPU_OK && texture_report.width == 4u &&
        texture_report.height == 4u && texture_report.mip_count == 1u);
    /* btbh_gpu_texture_update_rgba8 refuses a heightfield. */
    uint8_t pixels[16] = { 0 };
    assert(btbh_gpu_texture_update_rgba8(small, pixels, 0u, 0u, 2u, 2u) == BTBH_GPU_INVALID_ARGUMENT);
    printf("refused btbh_gpu_texture_update_rgba8 of a heightfield\n");

    static Scene scene;
    const Options sun = { .name = "refusal", .shadows = SHADOWS_SUN };
    BTBHGPUSceneHeightLift lift;
    /* The lift's fields (batch 0 is the terrain). */
    typedef struct LiftCase {
        const char *what;
        const char *needle;
        int field;
    } LiftCase;
    static const LiftCase lift_cases[] = {
        { "world_to_uv NaN", "world_to_uv must be finite", 0 },
        { "world_to_uv's u scale 0", "scales", 1 },
        { "world_to_uv's v scale 0", "scales", 2 },
        { "height_scale infinite", "height_scale", 3 },
        { "height_offset NaN", "height_offset", 4 },
        { "an unknown flag", "flags", 5 },
        { "reserved 1", "reserved", 6 },
        { "a plain texture as the heightfield", "isn't one", 7 },
        { "another window's heightfield", "another window", 8 },
    };
    for (size_t index = 0u; index < sizeof(lift_cases) / sizeof(lift_cases[0]); ++index) {
        build_scene(smoke, &sun, MODE_LIFTED, &scene);
        lift = smoke->lift;
        switch (lift_cases[index].field) {
            case 0: lift.world_to_uv[2] = NAN; break;
            case 1: lift.world_to_uv[0] = 0.0f; break;
            case 2: lift.world_to_uv[1] = 0.0f; break;
            case 3: lift.height_scale = INFINITY; break;
            case 4: lift.height_offset = NAN; break;
            case 5: lift.flags = 1u << 1; break;
            case 6: lift.reserved = 1u; break;
            case 7: lift.heightfield = smoke->albedo; break;
            default: lift.heightfield = other->field; break;
        }
        scene.batches[0].height_lift = &lift;
        scene.batches[1].height_lift = &lift;
        char what[96];
        (void)snprintf(what, sizeof(what), "height_lift: %s", lift_cases[index].what);
        expect_refused(smoke, &scene, NULL, what, lift_cases[index].needle);
        expect_refused(smoke, &scene, NULL, what, "GPU scene batch 0: height_lift: ");
    }
    /* Without a heightfield only flags and reserved are checked: a NaN mapping draws (as no lift). */
    build_scene(smoke, &sun, MODE_LIFTED, &scene);
    lift = smoke->lift;
    lift.heightfield = NULL;
    lift.world_to_uv[0] = NAN;
    scene.batches[0].height_lift = &lift;
    {
        BTBHGPUSceneReport report;
        assert(render_with(smoke, &shaders, &scene, NULL, &report) == BTBH_GPU_OK);
    }
    lift.flags = 4u;
    expect_refused(smoke, &scene, NULL, "height_lift without a heightfield, an unknown flag", "batch 0: height_lift");

    /* A heightfield in every other slot. */
    typedef struct SlotCase {
        const char *what;
        const char *needle;
        int slot;
    } SlotCase;
    static const SlotCase slot_cases[] = {
        { "a heightfield as albedo_texture", "albedo_texture: is a heightfield", 0 },
        { "a heightfield as normal_texture", "normal_texture: is a heightfield", 1 },
        { "a heightfield as orm_texture", "orm_texture: is a heightfield", 2 },
        { "a heightfield as emission_texture", "emission_texture: is a heightfield", 3 },
        { "a heightfield in a world detail", "world_detail: a texture is a heightfield", 4 },
        { "a heightfield as a world-detail table's map_layers", "map_layers", 5 },
    };
    BTBHGPUSceneWorldDetail detail;
    BTBHGPUSceneWorldDetailTable table;
    for (size_t index = 0u; index < sizeof(slot_cases) / sizeof(slot_cases[0]); ++index) {
        build_scene(smoke, &sun, MODE_FLAT, &scene);
        BTBHGPUSceneGeometryBatch *batch = &scene.batches[2];
        switch (slot_cases[index].slot) {
            case 0: batch->albedo_texture = smoke->field; break;
            case 1: batch->normal_texture = smoke->field; break;
            case 2: batch->orm_texture = smoke->field; break;
            case 3: batch->emission_texture = smoke->field; break;
            case 4:
                memset(&detail, 0, sizeof(detail));
                detail.normal_texture = smoke->field;
                detail.level_tile[0] = detail.level_tile[1] = 1.0f;
                detail.wall_tile[0] = detail.wall_tile[1] = 1.0f;
                batch->world_detail = &detail;
                break;
            default:
                table = smoke->table;
                table.map_layers = smoke->field;
                batch->world_detail_table = &table;
                break;
        }
        expect_refused(smoke, &scene, NULL, slot_cases[index].what, slot_cases[index].needle);
        expect_refused(smoke, &scene, NULL, slot_cases[index].what, "GPU scene batch 2: ");
    }
    {
        build_scene(smoke, &sun, MODE_FLAT, &scene);
        scene.post.lut_enabled = 1u;
        scene.post.lut_texture = smoke->field;
        expect_refused(smoke, &scene, NULL, "a heightfield as the post LUT", NULL);
    }
    {
        /* The render target. */
        build_scene(smoke, &sun, MODE_FLAT, &scene);
        BTBHGPUSceneReport report;
        memset(&report, 0, sizeof(report));
        const BTBHGPUResult result = btbh_gpu_window_scene_render_to_target(
            smoke->window, smoke->field,
            shaders.paths[0], shaders.paths[1], shaders.paths[2], shaders.paths[3],
            shaders.paths[4], shaders.paths[5], shaders.paths[6], shaders.paths[7],
            shaders.paths[8], shaders.paths[9], shaders.paths[10],
            SMOKE_SHADER_FORMAT, TARGET_SIZE, TARGET_SIZE,
            scene.camera.view_projection, scene.camera.right, scene.camera.up, scene.camera.position,
            &scene.lighting, &scene.post, &scene.instances[0][0], scene.instance_count, scene.instance_count,
            scene.batches, NULL, NULL, scene.batch_count, 0u, 0u, 0u, 255u, &report);
        printf("refused a heightfield as the render target                   %d: %s\n", (int)result, report.message);
        assert(result == BTBH_GPU_INVALID_ARGUMENT);
    }
    {
        /* The canvas's texture and target. */
        BTBHGPUCanvasVertex vertices[3];
        memset(vertices, 0, sizeof(vertices));
        BTBHGPUCanvasBatch batch;
        memset(&batch, 0, sizeof(batch));
        batch.vertex_count = 3u;
        batch.texture = smoke->field;
        BTBHGPUCanvasPass pass;
        memset(&pass, 0, sizeof(pass));
        pass.batch_count = 1u;
        BTBHGPUCanvasReport canvas_report;
        memset(&canvas_report, 0, sizeof(canvas_report));
        assert(btbh_gpu_window_canvas_present(smoke->window, canvas_shader_paths[0], canvas_shader_paths[1],
            canvas_shader_paths[2], SMOKE_SHADER_FORMAT, vertices, 3u, &batch, 1u, &pass, 1u, &canvas_report) ==
            BTBH_GPU_INVALID_ARGUMENT);
        batch.texture = NULL;
        pass.target = smoke->field;
        assert(btbh_gpu_window_canvas_present(smoke->window, canvas_shader_paths[0], canvas_shader_paths[1],
            canvas_shader_paths[2], SMOKE_SHADER_FORMAT, vertices, 3u, &batch, 1u, &pass, 1u, &canvas_report) ==
            BTBH_GPU_INVALID_ARGUMENT);
        printf("refused a heightfield as the canvas's texture and as its target\n");
    }
    {
        /* The capture destination: a frame presented to the window, copied into the heightfield. */
        build_scene(smoke, &sun, MODE_FLAT, &scene);
        BTBHGPUSceneReport report;
        memset(&report, 0, sizeof(report));
        assert(btbh_gpu_window_scene_present_instances(smoke->window,
            shaders.paths[0], shaders.paths[1], shaders.paths[2], shaders.paths[3],
            shaders.paths[4], shaders.paths[5], shaders.paths[6], shaders.paths[7],
            shaders.paths[8], shaders.paths[9], shaders.paths[10],
            SMOKE_SHADER_FORMAT, 160u, 90u,
            scene.camera.view_projection, scene.camera.right, scene.camera.up, scene.camera.position,
            &scene.lighting, &scene.post, &scene.instances[0][0], scene.instance_count, scene.instance_count,
            scene.batches, scene.batch_count, 0u, 0u, 0u, 255u, &report) == BTBH_GPU_OK);
        BTBHGPUTextureReport copy_report;
        memset(&copy_report, 0, sizeof(copy_report));
        assert(btbh_gpu_window_copy_pending_scene_texture(smoke->window, smoke->field, &copy_report) ==
            BTBH_GPU_INVALID_ARGUMENT);
        printf("refused a heightfield as the capture destination             %d: %s\n", (int)copy_report.result,
            copy_report.message);
        BTBHGPUCanvasPass pass;
        memset(&pass, 0, sizeof(pass));
        BTBHGPUCanvasReport canvas_report;
        memset(&canvas_report, 0, sizeof(canvas_report));
        assert(btbh_gpu_window_canvas_present(smoke->window, canvas_shader_paths[0], canvas_shader_paths[1],
            canvas_shader_paths[2], SMOKE_SHADER_FORMAT, NULL, 0u, NULL, 0u, &pass, 1u, &canvas_report) ==
            BTBH_GPU_OK);
    }

    /* What a lifted batch may be. */
    {
        build_scene(smoke, &sun, MODE_LIFTED, &scene);
        scene.batches[0].flags = 0u;
        expect_refused(smoke, &scene, NULL, "a lifted batch casting the sun's shadow", "casts no shadow");
        uint8_t visibility[MAX_BATCHES];
        memset(visibility, 1, sizeof(visibility));
        visibility[0] = 0u;
        BTBHGPUSceneReport report;
        assert(render_with(smoke, &shaders, &scene, visibility, &report) == BTBH_GPU_OK);
        printf("drawn   a lifted batch without the flag whose visibility byte keeps it out of the sun's maps\n");
        scene.batches[0].flags = BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW;
        scene.batches[0].spot_shadow_visible = 1u;
        expect_refused(smoke, &scene, NULL, "a lifted batch casting the spot's shadow", "casts no shadow");
        scene.batches[0].transparency_enabled = 1u;
        scene.batches[0].flags = 0u;
        assert(render_with(smoke, &shaders, &scene, NULL, &report) == BTBH_GPU_OK);
        printf("drawn   a transparent lifted batch without the flags (it casts none)\n");
        scene.batches[0].transparency_enabled = 0u;
        scene.batches[0].flags = BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW;
        scene.batches[0].spot_shadow_visible = 0u;
        scene.batches[0].billboard_mode = BTBH_GPU_SCENE_BILLBOARD_FRONT;
        expect_refused(smoke, &scene, NULL, "a lifted billboard", "billboard");
        scene.batches[0].billboard_mode = BTBH_GPU_SCENE_BILLBOARD_DISABLED;
        /* (Never followed: the refusal comes first.) */
        static const int not_a_terrain = 0;
        scene.batches[0].dug_by = (const BTBHGPUSdfTerrain *)(const void *)&not_a_terrain;
        expect_refused(smoke, &scene, NULL, "a lifted batch with dug_by", "dug");
        scene.batches[0].dug_by = NULL;
        /* A skinned model lifted. */
        BTBHGPUSceneGeometryBatch skinned = base_batch();
        skinned.kind = BTBH_GPU_SCENE_GEOMETRY_MODEL;
        skinned.model = smoke->skinned;
        skinned.custom_mesh = NULL;
        skinned.animation_library = smoke->animation;
        skinned.animation_time = 0.25f;
        skinned.cull_mode = BTBH_GPU_SCENE_CULL_NONE;
        skinned.flags = BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW;
        skinned.height_lift = &smoke->lift;
        static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        skinned.first_instance = add_instance(&scene, 0.0f, 0.5f, 1.0f, 0.4f, 0.0f, white);
        add_batch(&scene, skinned);
        char needle[64];
        (void)snprintf(needle, sizeof(needle), "batch %u: height_lift: a lifted batch must be static",
            scene.batch_count - 1u);
        expect_refused(smoke, &scene, NULL, "a lifted skinned batch", needle);
        scene.batches[scene.batch_count - 1u].height_lift = NULL;
        assert(render_with(smoke, &shaders, &scene, NULL, &report) == BTBH_GPU_OK);
    }
    btbh_gpu_texture_destroy(small);
}

/* ------------------------------------------------------------------------------------------- prewarm */

static BTBHGPUCreationStats creation_stats(void) {
    BTBHGPUCreationStats stats;
    btbh_gpu_creation_stats(&stats);
    return stats;
}

static void check_prewarm(SmokeWindow *fresh) {
    static Scene scenes[GALLERY_COUNT];
    const BTBHGPUCreationStats before_prewarm = creation_stats();
    for (uint32_t index = 0u; index < GALLERY_COUNT; ++index) {
        build_scene(fresh, &gallery[index], MODE_LIFTED, &scenes[index]);
        BTBHGPUSceneReport report;
        memset(&report, 0, sizeof(report));
        const BTBHGPUResult result = btbh_gpu_window_scene_prewarm_render_to_target(fresh->window, fresh->target,
            shaders.paths[0], shaders.paths[1], shaders.paths[2], shaders.paths[3],
            shaders.paths[4], shaders.paths[5], shaders.paths[6], shaders.paths[7],
            shaders.paths[8], shaders.paths[9], shaders.paths[10],
            SMOKE_SHADER_FORMAT, TARGET_SIZE, TARGET_SIZE, &scenes[index].lighting, &scenes[index].post,
            scenes[index].batches, scenes[index].batch_count, &report);
        if (result != BTBH_GPU_OK) {
            fprintf(stderr, "prewarm %s: %s\n", gallery[index].name, report.message);
        }
        assert(result == BTBH_GPU_OK);
    }
    const BTBHGPUCreationStats after_prewarm = creation_stats();
    for (uint32_t index = 0u; index < GALLERY_COUNT; ++index) {
        uint8_t *pixels = render(fresh, &scenes[index]);
        free(pixels);
    }
    const BTBHGPUCreationStats after_renders = creation_stats();
    printf("prewarm: %llu pipelines and %llu shaders made by prewarming the %u lifted scenes; the renders then made "
        "%llu pipelines and %llu shaders\n",
        (unsigned long long)(after_prewarm.graphics_pipelines - before_prewarm.graphics_pipelines),
        (unsigned long long)(after_prewarm.shaders - before_prewarm.shaders), (unsigned)GALLERY_COUNT,
        (unsigned long long)(after_renders.graphics_pipelines - after_prewarm.graphics_pipelines),
        (unsigned long long)(after_renders.shaders - after_prewarm.shaders));
    assert(after_prewarm.graphics_pipelines > before_prewarm.graphics_pipelines);
    assert(after_renders.graphics_pipelines == after_prewarm.graphics_pipelines);
    assert(after_renders.shaders == after_prewarm.shaders);
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
    /* (SPIR-V names only: a Metal library isn't checked for the heightfield.) */
    (void)fresh;
    return;
#endif
    ShaderCopy copy;
    memset(&copy, 0, sizeof(copy));
    (void)snprintf(copy.from, sizeof(copy.from), "%s", BTBH_GPU_SMOKE_SHADER_DIR);
    /* A short name: the refusal names the file by its whole path in the report's 255 characters, which a long
       build directory's path filled. */
    (void)snprintf(copy.to, sizeof(copy.to), "%s/../lift_old", BTBH_GPU_SMOKE_SHADER_DIR);
    (void)SDL_CreateDirectory(copy.to);
    assert(SDL_EnumerateDirectory(copy.from, copy_shader_file, &copy));
    assert(copy.copied > 11u);
    ShaderSet old;
    shader_set_in(&old, copy.to);
    char lift_path[1200];
    (void)snprintf(lift_path, sizeof(lift_path), "%s/production_model_lift.vert.spv", copy.to);
    assert(SDL_RemovePath(lift_path));
    static Scene scene;
    const Options sun = { .name = "old shaders", .shadows = SHADOWS_SUN };
    /* Without the file: a lifted batch fails naming it; unlifted ones draw. */
    build_scene(fresh, &sun, MODE_LIFTED, &scene);
    BTBHGPUSceneReport report;
    assert(render_with(fresh, &old, &scene, NULL, &report) == BTBH_GPU_INVALID_ARGUMENT);
    printf("old shaders, no production_model_lift.vert: \"%s\"\n", report.message);
    assert(strstr(report.message, lift_path) != NULL && strstr(report.message, "missing") != NULL);
    build_scene(fresh, &sun, MODE_FLAT, &scene);
    assert(render_with(fresh, &old, &scene, NULL, &report) == BTBH_GPU_OK);
    /* A production_model_lift.vert that doesn't lift (production_model.vert under its name): refused. */
    char plain_path[1200];
    (void)snprintf(plain_path, sizeof(plain_path), "%s/production_model.vert.spv", copy.to);
    assert(SDL_CopyFile(plain_path, lift_path));
    build_scene(fresh, &sun, MODE_LIFTED, &scene);
    assert(render_with(fresh, &old, &scene, NULL, &report) == BTBH_GPU_INVALID_ARGUMENT);
    printf("old shaders, a production_model_lift.vert that doesn't lift: \"%s\"\n", report.message);
    assert(strstr(report.message, lift_path) != NULL && strstr(report.message, "doesn't read a heightfield") != NULL);
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

static SmokeWindow open_smoke_window(BTBHGPUDevice *device) {
    SmokeWindow smoke;
    memset(&smoke, 0, sizeof(smoke));
    const BTBHGPUWindowDescriptor descriptor = {
        .title = "BTBH height lift smoke",
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
    smoke.albedo = noise_texture(smoke.window, 11u, 150u, 100u, BTBH_GPU_TEXTURE_COLOR_SPACE_SRGB);
    smoke.normal = noise_texture(smoke.window, 12u, 100u, 56u, BTBH_GPU_TEXTURE_COLOR_SPACE_LINEAR);
    {
        uint8_t *layers[4];
        for (uint32_t layer = 0u; layer < 4u; ++layer) {
            layers[layer] = noise_pixels(64u, 100u + layer, layer % 2u == 0u ? 100u : 60u, 56u);
        }
        smoke.maps = btbh_gpu_window_texture_array_create_rgba8_ex(smoke.window, (const uint8_t *const *)layers, 64u,
            64u, 4u, 1u, BTBH_GPU_TEXTURE_FILTER_TRILINEAR, BTBH_GPU_TEXTURE_WRAP_REPEAT,
            BTBH_GPU_TEXTURE_COLOR_SPACE_LINEAR, &texture_report);
        for (uint32_t layer = 0u; layer < 4u; ++layer) {
            free(layers[layer]);
        }
        assert(smoke.maps != NULL);
        for (uint32_t entry = 0u; entry < 2u; ++entry) {
            BTBHGPUSceneWorldDetailTableEntry *e = &smoke.entries[entry];
            memset(e, 0, sizeof(*e));
            e->level_tile[0] = e->level_tile[1] = 0.6f + 0.3f * (float)entry;
            e->wall_tile[0] = e->wall_tile[1] = 0.5f + 0.25f * (float)entry;
            e->normal_layer = (uint8_t)(2u * entry);
            e->orm_layer = (uint8_t)(2u * entry + 1u);
        }
        smoke.table.map_layers = smoke.maps;
        smoke.table.entries = smoke.entries;
        smoke.table.entry_count = 2u;
        smoke.table.seed_component = 4u;
    }
    {
        float heights[FIELD_SIZE * FIELD_SIZE];
        for (uint32_t j = 0u; j < FIELD_SIZE; ++j) {
            for (uint32_t i = 0u; i < FIELD_SIZE; ++i) {
                heights[j * FIELD_SIZE + i] = field_height(i, j);
            }
        }
        smoke.field = btbh_gpu_window_heightfield_create(smoke.window, heights, FIELD_SIZE, FIELD_SIZE,
            &texture_report);
        if (smoke.field == NULL) {
            fprintf(stderr, "heightfield: %s\n", texture_report.message);
        }
        assert(smoke.field != NULL && texture_report.result == BTBH_GPU_OK);
        assert(btbh_gpu_texture_is_heightfield(smoke.field));
        uint16_t *halves = malloc((size_t)FIELD_SIZE * FIELD_SIZE * 4u * sizeof(uint16_t));
        smoke.field_texels = malloc((size_t)FIELD_SIZE * FIELD_SIZE * 3u * sizeof(float));
        assert(halves != NULL && smoke.field_texels != NULL);
        btbh_gpu_height_lift_texels(heights, FIELD_SIZE, FIELD_SIZE, halves);
        for (size_t index = 0u; index < (size_t)FIELD_SIZE * FIELD_SIZE; ++index) {
            for (uint32_t c = 0u; c < 3u; ++c) {
                smoke.field_texels[index * 3u + c] = btbh_gpu_height_lift_from_half(halves[index * 4u + c]);
            }
        }
        free(halves);
    }
    lattice_lift(&smoke.lift, smoke.field);
    lattice_lift(&smoke.overlay_lift, smoke.field);
    smoke.overlay_lift.height_offset = 0.07f;
    smoke.overlay_lift.flags = BTBH_GPU_SCENE_HEIGHT_LIFT_KEEP_NORMALS;
    lattice_lift(&smoke.null_lift, NULL);
    MeshData data = terrain_data();
    smoke.terrain = create_mesh(smoke.window, &data);
    mesh_data_free(&data);
    data = overlay_data();
    smoke.overlay = create_mesh(smoke.window, &data);
    mesh_data_free(&data);
    data = standing_box_data();
    smoke.box = create_mesh(smoke.window, &data);
    mesh_data_free(&data);
    data = grid_data(-4.0f, 4.0f, -4.0f, 4.0f, 2u, 1.0f);
    smoke.floor = create_mesh(smoke.window, &data);
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
    btbh_gpu_custom_mesh_destroy(smoke->terrain);
    btbh_gpu_custom_mesh_destroy(smoke->overlay);
    btbh_gpu_custom_mesh_destroy(smoke->box);
    btbh_gpu_custom_mesh_destroy(smoke->floor);
    btbh_gpu_model_destroy(smoke->skinned);
    btbh_gpu_animation_library_destroy(smoke->animation);
    btbh_gpu_texture_destroy(smoke->field);
    btbh_gpu_texture_destroy(smoke->maps);
    btbh_gpu_texture_destroy(smoke->normal);
    btbh_gpu_texture_destroy(smoke->albedo);
    btbh_gpu_texture_destroy(smoke->target);
    btbh_gpu_window_destroy(smoke->window);
    free(smoke->field_texels);
    memset(smoke, 0, sizeof(*smoke));
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
    SmokeWindow other = open_smoke_window(device);
    check_gallery(&smoke);
    check_keep_hbao_overlay(&smoke);
    check_refusals(&smoke, &other);
    close_smoke_window(&other);
    SmokeWindow fresh = open_smoke_window(device);
    check_prewarm(&fresh);
    close_smoke_window(&fresh);
    SmokeWindow old = open_smoke_window(device);
    check_old_shaders(&old);
    close_smoke_window(&old);
    close_smoke_window(&smoke);
    btbh_gpu_device_destroy(device);
    puts("btbh_gpu_height_lift_smoke: passed");
    return 0;
}
