/* RFC 0010's radiance-cascade sources with a top (BTBHGPUSceneRadianceCascades.source_tops,
 * BTBHGPUSceneRadianceEmitter.top_above), asked for by Be The Bullet Hell v2, rendered into a target and read back
 * (hidden windows, a debug device).
 *
 * The street: a two-storey house (roof at 0.735: two storeys of 0.35 and a 0.035 parapet, v2's numbers) and a
 * one-storey one (0.385) either side of a two-cell street, at night (a faint ambient, no sun), in the cascades' field
 * of v2's two presets: Low (32 x 32 texels of one world unit, tier LOW) and High (64 x 64 texels of half a unit, tier
 * HIGH). The occlusion field is v2's: 0 inside a house, outside the distance to the nearest inside texel, and that
 * house's top. A lamp head glows in the street, half a cell out from the two-storey facade (a lit slab with an
 * emission, its top at 0.6, as v2's lamp heads are); emitters stand in for v2's gameplay glows. The light a surface
 * takes is the linear luminance its pixels gain from the glow (the same scene without it subtracted). With tops the
 * cascades read the field half a texel from where they read it without (the header, source_tops), so what tops
 * shield is measured against the same scene with tops and nothing shielded: the lamp with its hood (a dimmer glow at
 * 2 units with its footprint, so its texels' top is 2), an emitter with top_above 1000.
 *
 * Off. This part also builds against 0.9.0's header (no BTBH_GPU_SCENE_HAS_RC_SOURCE_TOPS): there it only renders
 * (and writes) the gallery, with the same garbage in what was the emitter's reserved word, so that a build of the
 * base and a build of this one can be compared.
 * - A gallery with source_tops 0 (Low and High, additive and not, the street from above and the facade, every
 *   emitter shape, the cascades off): each scene rendered twice must come out the same, and (0.10.0) the same as with
 *   top_above 0, whatever garbage top_above holds (NaN, infinities, negative, huge); without the cascades, the same
 *   as with source_tops 1.
 * - BTBH_RC_TOPS_DUMP=<dir> writes the gallery (off-<name>.png) and what the checks below render;
 *   BTBH_RC_TOPS_REFERENCE=<dir> compares each gallery image with that directory's and requires 0 differing pixels.
 *
 * On (0.10.0):
 * - Street and roof, Low and High, the street turned four ways, additive (and High not additive): with tops the
 *   two-storey roof's interior (2.5 units in and more) keeps under 5 % of the lamp's light (Low) or under 25 %
 *   (High), the one-storey roof and the street keep it all (within 2 %), and every way the street is turned gives
 *   the same shares and light.
 * - The edge band: across the two-storey roof from its street edge, the share kept, for lamps half a cell, a cell and
 *   a cell and a half out (High) and half a cell and a cell (Low): a band 0.25 to 2 units wide (High) and 0.1 to 1
 *   (Low) for the lamp half a cell out, wider and an interior less shielded for lamps farther out (printed).
 * - Walls: the two-storey facade facing the lamp, facing each of the four ways, Low and High, keeps the lamp's light
 *   at street level (within 3 % of nothing shielded) and at least nine tenths of what it took without tops.
 * - A texel's top is the highest of its sources (MAX): the hood lets the roof take the lamp's light; flagged
 *   BTBH_GPU_SCENE_BATCH_NO_RC_SOURCE it writes no top either.
 * - Emitters half a cell out: a shot (a disk at 0.55, top_above 0), a rectangle, a segment and a spot shield the
 *   two-storey roof's interior (under 25 %) and light the one-storey roof. A fire at a one-storey roof's height
 *   (position_y 0.385) is passed over with top_above 0 and lights the two-storey roof with 0.6 (top 0.985) as with
 *   1000.
 * - Refusals: source_tops 2 and 255, and with 1 a top_above that is NaN, infinite or negative, leave the cascades off
 *   for that present (BTBH_GPU_SCENE_RC_FALLBACK_INVALID_CONFIGURATION), and the next valid present runs them again;
 *   with 0 the same values draw. The prewarm takes tops.
 * - BTBH_RC_TOPS_OLD_SHADER_DIR=<dir> (an earlier release's compiled shaders): in a window whose pipelines are made
 *   from them, the gallery draws byte for byte as with this release's shaders, and a present or a prewarm with
 *   source_tops on fails with BTBH_GPU_INVALID_ARGUMENT, naming the trace shader.
 * - BTBH_RC_TOPS_MEASURE=1: a measurement out of its bounds is printed and the smoke goes on, and the points measured
 *   are marked in images written with the dump (for tuning).
 *
 * It needs a GPU and the production shaders; with no device it is skipped (77). */
#undef NDEBUG

#include "btbh_gpu/btbh_gpu.h"
#include "btbh_gpu_smoke_meshes.h"
#include "btbh_gpu_internal.h"

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_surface.h>

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SMOKE_SKIP 77
#define TARGET_SIZE 256u
#define TEXELS (TARGET_SIZE * TARGET_SIZE)
#define MAX_BOXES 8u
#define FIELD_MAX 64u

#if defined(BTBH_GPU_SCENE_HAS_RC_SOURCE_TOPS)
#define SMOKE_HAS_TOPS 1
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
typedef char ShaderPaths[11][1024];
static ShaderPaths shader_paths;
/* BTBH_RC_TOPS_MEASURE=1: a measurement outside its bounds is printed, and the smoke goes on (for tuning). */
static bool measuring;

typedef struct SmokeWindow {
    BTBHGPUWindow *window;
    BTBHGPUCustomMesh *cube;
    BTBHGPUTexture *target;
    const ShaderPaths *paths;
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
    float up[3];
    cross3(forward, world_up, side);
    normalize3(side);
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
    const float projection[16] = {
        h, 0.0f, 0.0f, 0.0f,
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

static void expect_same(const char *what, const uint8_t *a, const uint8_t *b) {
    int largest = 0;
    const uint32_t differing = differing_pixels(a, b, &largest);
    if (differing != 0u) {
        fprintf(stderr, "%s: %u pixels differ (largest %d)\n", what, differing, largest);
    }
    assert(differing == 0u);
}

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

/* -------------------------------------------------------------------------------------------- the street */

typedef struct House {
    float min_x;
    float max_x;
    float min_z;
    float max_z;
    float top;
} House;

/* The two-storey house west of the street, the one-storey one east of it. */
static const House houses[2] = {
    { -6.0f, -1.0f, -3.0f, 3.0f, 0.735f },
    { 1.0f, 5.0f, -3.0f, 3.0f, 0.385f },
};

typedef struct Field {
    float origin;
    float cell;
    uint32_t size;
    uint64_t revision;
    float samples[FIELD_MAX * FIELD_MAX * 2u];
} Field;

/* The fields of Low and High (first index), for the street turned 0, 90, 180 and 270 degrees (second). */
static Field fields[2][4];

/* A point of the street as it stands (the two-storey house west of it, along z), turned `orientation` quarter turns
   (x to z: a quarter turn takes the house north of the street). */
static void orient(uint32_t orientation, float x, float z, float *out_x, float *out_z) {
    switch (orientation & 3u) {
        case 1u: *out_x = -z; *out_z = x; break;
        case 2u: *out_x = -x; *out_z = -z; break;
        case 3u: *out_x = z; *out_z = -x; break;
        default: *out_x = x; *out_z = z; break;
    }
}

/* A rectangle (x0..x1, z0..z1) turned: its corners' bounds. */
static void orient_rectangle(uint32_t orientation, float x0, float z0, float x1, float z1, float out[4]) {
    float ax;
    float az;
    float bx;
    float bz;
    orient(orientation, x0, z0, &ax, &az);
    orient(orientation, x1, z1, &bx, &bz);
    out[0] = fminf(ax, bx);
    out[1] = fminf(az, bz);
    out[2] = fmaxf(ax, bx);
    out[3] = fmaxf(az, bz);
}

/* The occlusion field as Be The Bullet Hell v2 writes it (v2-render's radiance.rs, build_field): a texel whose centre
   lies in a house is inside it (distance 0, the house's top); any other holds the distance from its centre to the
   nearest inside texel's centre (v2's chamfer, here exact) and that house's top. */
static void build_field(Field *field, float cell, uint32_t size, uint64_t revision, uint32_t orientation) {
    assert(size <= FIELD_MAX);
    field->cell = cell;
    field->size = size;
    field->origin = -0.5f * cell * (float)size;
    field->revision = revision;
    for (uint32_t row = 0u; row < size; ++row) {
        for (uint32_t column = 0u; column < size; ++column) {
            const float x = field->origin + ((float)column + 0.5f) * cell;
            const float z = field->origin + ((float)row + 0.5f) * cell;
            float nearest = 1.0e6f;
            float top = 0.0f;
            for (uint32_t index = 0u; index < 2u; ++index) {
                const House *house = &houses[index];
                float r[4];
                orient_rectangle(orientation, house->min_x, house->min_z, house->max_x, house->max_z, r);
                /* The house's inside texel centres span [first, last] on each axis (its walls lie between centres). */
                const float first_x = field->origin + (ceilf((r[0] - field->origin) / cell - 0.5f) + 0.5f) * cell;
                const float last_x = field->origin + (floorf((r[2] - field->origin) / cell - 0.5f) + 0.5f) * cell;
                const float first_z = field->origin + (ceilf((r[1] - field->origin) / cell - 0.5f) + 0.5f) * cell;
                const float last_z = field->origin + (floorf((r[3] - field->origin) / cell - 0.5f) + 0.5f) * cell;
                const float dx = fmaxf(fmaxf(first_x - x, x - last_x), 0.0f);
                const float dz = fmaxf(fmaxf(first_z - z, z - last_z), 0.0f);
                const float distance = sqrtf(dx * dx + dz * dz);
                if (distance < nearest) {
                    nearest = distance;
                    top = house->top;
                }
            }
            field->samples[(row * size + column) * 2u] = nearest;
            field->samples[(row * size + column) * 2u + 1u] = top;
        }
    }
}

typedef enum Emitter {
    EMITTER_SHOT = 0,      /* a disk at 0.55 in the street */
    EMITTER_RECTANGLE = 1, /* a square at 0.5 in the street */
    EMITTER_SEGMENT = 2,   /* a segment along the street at 0.55 (a power line) */
    EMITTER_SPOT = 3,      /* a spot at 0.55 in the street, aimed at the two-storey house */
    EMITTER_FIRE = 4,      /* a disk on the one-storey roof, at its top */
    EMITTER_COUNT = 5
} Emitter;

typedef enum CameraKind { CAMERA_TOP = 0, CAMERA_FACADE = 1 } CameraKind;

typedef struct Options {
    const char *name;
    BTBHGPUSceneRadianceCascadesTier tier;
    bool additive;
    CameraKind camera;
    uint8_t source_tops;
    bool lamp;               /* the lamp head: a slab at 0.5..0.6 glowing through its emission */
    float lamp_x;            /* its centre */
    float lamp_half_width;   /* along x (0.6 along z) */
    bool hood;               /* a dimmer glow at 1.9..2 over the lamp, with its footprint: the lamp's texels' top 2 */
    bool hood_flagged;       /* the hood flagged BTBH_GPU_SCENE_BATCH_NO_RC_SOURCE */
    uint32_t emitters;       /* bits of Emitter */
    float top_above[EMITTER_COUNT];
    bool garbage;            /* garbage in top_above (NaN, infinities, negative, huge), source_tops 0 only */
    uint32_t orientation;    /* the street turned this many quarter turns */
} Options;

typedef struct Scene {
    const char *name;
    SmokeCamera camera;
    BTBHGPUSceneLighting lighting;
    BTBHGPUScenePost post;
    float instances[MAX_BOXES][24];
    BTBHGPUSceneGeometryBatch batches[MAX_BOXES];
    uint32_t count;
    BTBHGPUSceneRadianceEmitter emitters[EMITTER_COUNT];
} Scene;

static const float grey[4] = { 0.55f, 0.55f, 0.52f, 1.0f };
static const float stone[4] = { 0.7f, 0.68f, 0.62f, 1.0f };
static const float lamp_glass[4] = { 0.9f, 0.85f, 0.7f, 1.0f };

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
    batch.roughness = 0.8f;
    batch.occlusion = 1.0f;
    batch.depth_compare = BTBH_GPU_SCENE_DEPTH_LESS;
    batch.cull_mode = BTBH_GPU_SCENE_CULL_BACK;
    batch.stencil_pass = BTBH_GPU_SCENE_STENCIL_REPLACE;
    batch.spot_shadow_visible = 1u;
    return batch;
}

/* A box from (x0, y0, z0) to (x1, y1, z1). */
static void scene_add(Scene *scene, BTBHGPUSceneGeometryBatch batch, float x0, float y0, float z0, float x1, float y1,
    float z1, const float colour[4]) {
    assert(scene->count < MAX_BOXES);
    const uint32_t index = scene->count++;
    float *instance = scene->instances[index];
    memset(instance, 0, 24u * sizeof(float));
    instance[0] = x1 - x0;
    instance[5] = y1 - y0;
    instance[10] = z1 - z0;
    instance[12] = 0.5f * (x0 + x1);
    instance[13] = 0.5f * (y0 + y1);
    instance[14] = 0.5f * (z0 + z1);
    instance[15] = 1.0f;
    memcpy(&instance[16], colour, 4u * sizeof(float));
    batch.first_instance = index;
    scene->batches[index] = batch;
}

static void set_top_above(BTBHGPUSceneRadianceEmitter *emitter, float value) {
#if defined(SMOKE_HAS_TOPS)
    emitter->top_above = value;
#else
    /* 0.9.0: the same bytes, in what was the reserved word. */
    memcpy(&emitter->reserved, &value, sizeof(value));
#endif
}

static void set_source_tops(BTBHGPUSceneRadianceCascades *cascades, uint8_t value) {
#if defined(SMOKE_HAS_TOPS)
    cascades->source_tops = value;
#else
    assert(value == 0u);
    cascades->reserved[0] = value;
#endif
}

static BTBHGPUSceneRadianceEmitter make_emitter(uint64_t id, float x, float y, float z, float radius, float energy) {
    BTBHGPUSceneRadianceEmitter emitter;
    memset(&emitter, 0, sizeof(emitter));
    emitter.stable_id = id;
    emitter.position_x = x;
    emitter.position_y = y;
    emitter.position_z = z;
    emitter.radius = radius;
    emitter.height = 0.5f;
    emitter.red = 1.0f;
    emitter.green = 0.75f;
    emitter.blue = 0.45f;
    emitter.energy = energy;
    emitter.direction_z = 1.0f;
    emitter.inner_cutoff = -1.0f;
    emitter.outer_cutoff = -2.0f;
    emitter.shape = BTBH_GPU_SCENE_RADIANCE_EMITTER_DISK;
    emitter.light_class = BTBH_GPU_SCENE_RADIANCE_EMITTER_CLASS_TRANSIENT;
    return emitter;
}

static float garbage_value(uint32_t index) {
    switch (index) {
        case 0u: return NAN;
        case 1u: return INFINITY;
        case 2u: return -INFINITY;
        case 3u: return -1.0f;
        default: return 1.0e30f;
    }
}

static void build_scene(const SmokeWindow *smoke, const Options *options, Scene *scene) {
    memset(scene, 0, sizeof(*scene));
    scene->name = options->name;
    const uint32_t o = options->orientation;
    float eye[3];
    float centre[3];
    if (options->camera == CAMERA_TOP) {
        eye[1] = 14.0f;
        centre[1] = 0.0f;
        orient(o, 0.0f, 6.0f, &eye[0], &eye[2]);
        orient(o, -0.5f, 0.0f, &centre[0], &centre[2]);
        scene->camera = look_at(eye, centre, 0.85f);
    } else {
        /* In the street, below the lamp head: the two-storey facade facing the street, past the lamp's north side. */
        eye[1] = 0.3f;
        centre[1] = 0.3f;
        orient(o, 0.8f, 3.5f, &eye[0], &eye[2]);
        orient(o, -1.0f, 0.0f, &centre[0], &centre[2]);
        scene->camera = look_at(eye, centre, 0.9f);
    }

    BTBHGPUSceneLighting *lighting = &scene->lighting;
    lighting->ambient_red = 0.6f;
    lighting->ambient_green = 0.7f;
    lighting->ambient_blue = 1.0f;
    lighting->ambient_energy = 0.04f;
    lighting->directional_y = -1.0f;
    lighting->directional_red = 1.0f;
    lighting->directional_green = 1.0f;
    lighting->directional_blue = 1.0f;
    lighting->spot_range = 8.0f;
    lighting->spot_falloff = 1.0f;
    lighting->spot_inner_cutoff = 1.0f;
    lighting->spot_outer_cutoff = 0.0f;

    BTBHGPUScenePost *post = &scene->post;
    /* Reinhard with a white point of 1: the target holds sRGB(linear colour), dithered. */
    post->tonemap_white = 1.0f;
    post->exposure = 1.0f;
    post->contrast = 1.0f;
    post->saturation = 1.0f;
    post->blur_curve = 1.0f;
    post->tonemap_mode = BTBH_GPU_SCENE_TONEMAP_REINHARD;
    post->upscale_mode = BTBH_GPU_SCENE_UPSCALE_LINEAR;
    BTBHGPUSceneRadianceCascades *cascades = &post->radiance_cascades;
    cascades->tier = options->tier;
    cascades->auto_frame_budget_milliseconds = 16.0f;
    cascades->indirect_intensity = 1.0f;
    cascades->hero_omni_emitter_index = -1;
    cascades->invalidate = 1u;
    cascades->additive = options->additive ? 1u : 0u;
    set_source_tops(cascades, options->source_tops);
    const Field *field = &fields[options->tier == BTBH_GPU_SCENE_RC_LOW ? 0u : 1u][o & 3u];
    cascades->occlusion_cascade_count = 1u;
    cascades->occlusion_cascades[0].origin_world_x = field->origin;
    cascades->occlusion_cascades[0].origin_world_z = field->origin;
    cascades->occlusion_cascades[0].cell_size = field->cell;
    cascades->occlusion_cascades[0].width = field->size;
    cascades->occlusion_cascades[0].height = field->size;
    cascades->occlusion_cascades[0].revision = field->revision;
    cascades->occlusion_cascades[0].samples = field->samples;

    /* The ground, the houses. */
    scene_add(scene, cube_batch(smoke->cube), -12.0f, -0.5f, -12.0f, 12.0f, 0.0f, 12.0f, grey);
    for (uint32_t index = 0u; index < 2u; ++index) {
        const House *house = &houses[index];
        float r[4];
        orient_rectangle(o, house->min_x, house->min_z, house->max_x, house->max_z, r);
        scene_add(scene, cube_batch(smoke->cube), r[0], 0.0f, r[1], r[2], house->top, r[3], stone);
    }
    BTBHGPUSceneGeometryBatch glow = cube_batch(smoke->cube);
    glow.emission_red = 255u;
    glow.emission_green = 190u;
    glow.emission_blue = 120u;
    glow.emission_energy = 2.0f;
    const float lamp_x = options->lamp_x;
    if (options->lamp) {
        float r[4];
        orient_rectangle(o, lamp_x - options->lamp_half_width, -0.6f, lamp_x + options->lamp_half_width, 0.6f, r);
        scene_add(scene, glow, r[0], 0.5f, r[1], r[2], 0.6f, r[3], lamp_glass);
    }
    if (options->hood) {
        BTBHGPUSceneGeometryBatch hood = glow;
        hood.emission_energy = 0.5f;
        hood.flags = options->hood_flagged ? BTBH_GPU_SCENE_BATCH_NO_RC_SOURCE : 0u;
        float r[4];
        orient_rectangle(o, lamp_x - options->lamp_half_width, -0.6f, lamp_x + options->lamp_half_width, 0.6f, r);
        scene_add(scene, hood, r[0], 1.9f, r[1], r[2], 2.0f, r[3], lamp_glass);
    }

    uint32_t count = 0u;
    for (uint32_t index = 0u; index < EMITTER_COUNT; ++index) {
        if ((options->emitters & (1u << index)) == 0u) {
            continue;
        }
        BTBHGPUSceneRadianceEmitter emitter;
        switch ((Emitter)index) {
            case EMITTER_SHOT:
                emitter = make_emitter(1u, -0.5f, 0.55f, 0.0f, 0.45f, 3.0f);
                break;
            case EMITTER_RECTANGLE:
                emitter = make_emitter(2u, -0.5f, 0.5f, 0.0f, 0.3f, 3.0f);
                emitter.shape = BTBH_GPU_SCENE_RADIANCE_EMITTER_RECTANGLE;
                break;
            case EMITTER_SEGMENT:
                emitter = make_emitter(3u, -0.5f, 0.55f, 0.0f, 1.5f, 2.0f);
                emitter.shape = BTBH_GPU_SCENE_RADIANCE_EMITTER_SEGMENT;
                emitter.height = 0.6f;
                break;
            case EMITTER_SPOT:
                emitter = make_emitter(4u, -0.5f, 0.55f, 0.0f, 0.3f, 6.0f);
                emitter.direction_x = -1.0f;
                emitter.direction_y = -0.2f;
                emitter.direction_z = 0.0f;
                emitter.inner_cutoff = 0.9f;
                emitter.outer_cutoff = 0.5f;
                break;
            case EMITTER_FIRE:
            default:
                /* On a one-storey annex's roof (not drawn) against the two-storey house. */
                emitter = make_emitter(5u, -0.5f, houses[1].top, 0.0f, 0.45f, 4.0f);
                emitter.red = 1.0f;
                emitter.green = 0.5f;
                emitter.blue = 0.2f;
                break;
        }
        const float position[2] = { emitter.position_x, emitter.position_z };
        const float direction[2] = { emitter.direction_x, emitter.direction_z };
        orient(o, position[0], position[1], &emitter.position_x, &emitter.position_z);
        orient(o, direction[0], direction[1], &emitter.direction_x, &emitter.direction_z);
        set_top_above(&emitter, options->garbage ? garbage_value(index) : options->top_above[index]);
        scene->emitters[count++] = emitter;
    }
    cascades->emitter_count = count;
    cascades->emitters = count > 0u ? scene->emitters : NULL;
}

static uint8_t *render_scene(SmokeWindow *smoke, const Scene *scene, BTBHGPUResult expected,
    BTBHGPUSceneReport *report) {
    const ShaderPaths *paths = smoke->paths;
    memset(report, 0, sizeof(*report));
    const BTBHGPUResult result = btbh_gpu_window_scene_render_to_target(
        smoke->window,
        smoke->target,
        (*paths)[0], (*paths)[1], (*paths)[2], (*paths)[3],
        (*paths)[4], (*paths)[5], (*paths)[6], (*paths)[7],
        (*paths)[8], (*paths)[9], (*paths)[10],
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
    return read_texture(smoke->window, smoke->target);
}

/* Renders the scene, which needs the cascades to run when it asks for them. */
static uint8_t *render(SmokeWindow *smoke, const Scene *scene) {
    BTBHGPUSceneReport report;
    uint8_t *image = render_scene(smoke, scene, BTBH_GPU_OK, &report);
    if (scene->post.radiance_cascades.tier != BTBH_GPU_SCENE_RC_OFF &&
        report.radiance_cascades.active_tier == BTBH_GPU_SCENE_RC_OFF) {
        fprintf(stderr, "%s: the cascades didn't run (fallback %d)\n", scene->name,
            (int)report.radiance_cascades.fallback_reason);
        assert(false);
    }
    return image;
}

static uint8_t *render_options(SmokeWindow *smoke, const Options *options) {
    static Scene scene;
    build_scene(smoke, options, &scene);
    return render(smoke, &scene);
}

/* ------------------------------------------------------------------------------------------- the gallery */

#define ALL_EMITTERS ((1u << EMITTER_COUNT) - 1u)

static const Options gallery[] = {
    { .name = "street-low-additive", .tier = BTBH_GPU_SCENE_RC_LOW, .additive = true, .lamp = true,
      .lamp_half_width = 0.6f, .emitters = (1u << EMITTER_FIRE) | (1u << EMITTER_SPOT), .garbage = true },
    { .name = "street-high-additive", .tier = BTBH_GPU_SCENE_RC_HIGH, .additive = true, .lamp = true,
      .lamp_half_width = 0.6f, .emitters = (1u << EMITTER_FIRE) | (1u << EMITTER_SEGMENT), .garbage = true },
    { .name = "street-high-on", .tier = BTBH_GPU_SCENE_RC_HIGH, .lamp = true, .lamp_half_width = 0.6f,
      .emitters = ALL_EMITTERS, .garbage = true },
    { .name = "street-low-on", .tier = BTBH_GPU_SCENE_RC_LOW, .lamp = true, .lamp_half_width = 0.6f, .hood = true,
      .emitters = (1u << EMITTER_RECTANGLE), .garbage = true },
    { .name = "facade-high-additive", .tier = BTBH_GPU_SCENE_RC_HIGH, .additive = true, .camera = CAMERA_FACADE,
      .lamp = true, .lamp_half_width = 0.6f, .emitters = (1u << EMITTER_SHOT), .garbage = true },
    { .name = "facade-low-turned-180", .tier = BTBH_GPU_SCENE_RC_LOW, .additive = true, .camera = CAMERA_FACADE,
      .lamp = true, .lamp_half_width = 0.6f, .emitters = (1u << EMITTER_SHOT), .garbage = true, .orientation = 2u },
    { .name = "emitters-high-additive", .tier = BTBH_GPU_SCENE_RC_HIGH, .additive = true,
      .emitters = ALL_EMITTERS, .garbage = true },
    { .name = "cascades-off", .tier = BTBH_GPU_SCENE_RC_OFF, .lamp = true, .lamp_half_width = 0.6f,
      .emitters = ALL_EMITTERS, .garbage = true },
};
#define GALLERY_COUNT (sizeof(gallery) / sizeof(gallery[0]))

static void write_image(const char *directory, const char *prefix, const char *name, const uint8_t *pixels) {
    if (directory == NULL || directory[0] == '\0') {
        return;
    }
    char path[1200];
    (void)snprintf(path, sizeof(path), "%s/%s-%s.png", directory, prefix, name);
    assert(btbh_gpu_write_png_rgba8(path, TARGET_SIZE, TARGET_SIZE, pixels) == BTBH_GPU_OK);
}

/* Renders the gallery (each scene twice), compares it with BTBH_RC_TOPS_REFERENCE's and with `compare` (images in
   the gallery's order, or NULL), writes it to BTBH_RC_TOPS_DUMP when `dump`, and returns the images. */
static uint8_t **check_gallery(SmokeWindow *smoke, uint8_t *const *compare, bool dump) {
    const char *dump_dir = dump ? SDL_getenv("BTBH_RC_TOPS_DUMP") : NULL;
    const char *reference_dir = SDL_getenv("BTBH_RC_TOPS_REFERENCE");
    uint8_t **images = calloc(GALLERY_COUNT, sizeof(uint8_t *));
    assert(images != NULL);
    uint32_t compared = 0u;
    for (uint32_t index = 0u; index < GALLERY_COUNT; ++index) {
        const Options *options = &gallery[index];
        uint8_t *image = render_options(smoke, options);
        uint8_t *again = render_options(smoke, options);
        expect_same(options->name, image, again);
        free(again);
        printf("gallery %-24s rendered", options->name);
#if defined(SMOKE_HAS_TOPS)
        /* top_above isn't read with source_tops 0: the garbage draws as 0 does. */
        Options zero = *options;
        zero.garbage = false;
        uint8_t *zeroed = render_options(smoke, &zero);
        expect_same("garbage top_above against 0", image, zeroed);
        free(zeroed);
        printf("; as with top_above 0");
        if (options->tier == BTBH_GPU_SCENE_RC_OFF) {
            /* Without the cascades the switch changes nothing. */
            Options on = zero;
            on.source_tops = 1u;
            uint8_t *switched = render_options(smoke, &on);
            expect_same("cascades off, source_tops 1 against 0", image, switched);
            free(switched);
            printf("; as with source_tops 1");
        }
#endif
        if (compare != NULL) {
            expect_same("against the other shaders", image, compare[index]);
            printf("; as with the other shaders");
        }
        if (reference_dir != NULL && reference_dir[0] != '\0') {
            char path[1200];
            (void)snprintf(path, sizeof(path), "%s/off-%s.png", reference_dir, options->name);
            uint8_t *reference = malloc((size_t)TEXELS * 4u);
            assert(reference != NULL);
            const bool loaded = load_png(path, reference);
            if (!loaded) {
                fprintf(stderr, "\nno reference %s\n", path);
            }
            assert(loaded);
            expect_same(path, image, reference);
            printf("; as the reference");
            compared += 1u;
            free(reference);
        }
        printf("\n");
        write_image(dump_dir, "off", options->name, image);
        images[index] = image;
    }
    if (compared > 0u) {
        printf("gallery: %u images (%u pixels) match the references\n", compared, compared * TEXELS);
    }
    return images;
}

static void free_images(uint8_t **images) {
    if (images == NULL) {
        return;
    }
    for (uint32_t index = 0u; index < GALLERY_COUNT; ++index) {
        free(images[index]);
    }
    free(images);
}

#if defined(SMOKE_HAS_TOPS)
/* ------------------------------------------------------------------------------- on: what the tops shield */

static uint32_t failed_measurements;

static void expect(bool ok, const char *what) {
    if (ok) {
        return;
    }
    fprintf(stderr, "measurement out of bounds: %s\n", what);
    failed_measurements += 1u;
    assert(measuring);
}

static float srgb_to_linear(float value) {
    return value <= 0.04045f ? value / 12.92f : powf((value + 0.055f) / 1.055f, 2.4f);
}

/* Measure mode: the pixels sampled are marked here (a copy of an image, written with the dump). */
static uint8_t *marks;

/* The mean linear luminance of the 3 x 3 pixels round a world point's pixel. */
static float luminance_at(const uint8_t *image, const SmokeCamera *camera, float x, float y, float z) {
    const float p[4] = { x, y, z, 1.0f };
    float clip[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    for (int row = 0; row < 4; ++row) {
        for (int k = 0; k < 4; ++k) {
            clip[row] += camera->view_projection[k * 4 + row] * p[k];
        }
    }
    assert(clip[3] > 0.0f);
    const float px = (clip[0] / clip[3] * 0.5f + 0.5f) * (float)TARGET_SIZE;
    const float py = (0.5f - clip[1] / clip[3] * 0.5f) * (float)TARGET_SIZE;
    const int cx = (int)floorf(px);
    const int cy = (int)floorf(py);
    assert(cx >= 1 && cy >= 1 && cx < (int)TARGET_SIZE - 1 && cy < (int)TARGET_SIZE - 1);
    if (marks != NULL) {
        uint8_t *mark = &marks[((size_t)cy * TARGET_SIZE + (size_t)cx) * 4u];
        mark[0] = 255u;
        mark[1] = 0u;
        mark[2] = 0u;
    }
    float sum = 0.0f;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const uint8_t *pixel = &image[((size_t)(cy + dy) * TARGET_SIZE + (size_t)(cx + dx)) * 4u];
            sum += 0.2126f * srgb_to_linear((float)pixel[0] / 255.0f) +
                0.7152f * srgb_to_linear((float)pixel[1] / 255.0f) +
                0.0722f * srgb_to_linear((float)pixel[2] / 255.0f);
        }
    }
    return sum / 9.0f;
}

/* What a glow adds at a world point: the lit image's luminance less the dark one's. */
typedef struct Light {
    const uint8_t *lit;
    const uint8_t *dark;
    const SmokeCamera *camera;
} Light;

/* The orientation of the street being measured: the points below are given for the street as it stands. */
static uint32_t measured_orientation;

static float light_at(const Light *light, float x, float y, float z) {
    const float given[2] = { x, z };
    orient(measured_orientation, given[0], given[1], &x, &z);
    return luminance_at(light->lit, light->camera, x, y, z) - luminance_at(light->dark, light->camera, x, y, z);
}

/* The light summed over points: the two-storey roof's interior (from `from` to 4.5 units in from its street edge),
   the one-storey roof's interior, the street south of the lamp (north of it the hood hides some of it). */
static float roof_a_light(const Light *light, float from) {
    float sum = 0.0f;
    for (float e = from; e <= 4.51f; e += 0.5f) {
        for (int z = -1; z <= 1; ++z) {
            sum += light_at(light, houses[0].max_x - e, houses[0].top, (float)z);
        }
    }
    return sum;
}

static float roof_b_light(const Light *light) {
    float sum = 0.0f;
    for (float e = 0.5f; e <= 3.51f; e += 0.5f) {
        for (int z = -1; z <= 1; ++z) {
            sum += light_at(light, houses[1].min_x + e, houses[1].top, (float)z);
        }
    }
    return sum;
}

static const float street_xs[3] = { -0.5f, 0.0f, 0.5f };
static const float street_zs[3] = { 1.2f, 1.8f, 2.4f };

static float street_light(const Light *light) {
    float sum = 0.0f;
    for (uint32_t i = 0u; i < 3u; ++i) {
        for (uint32_t j = 0u; j < 3u; ++j) {
            sum += light_at(light, street_xs[i], 0.0f, street_zs[j]);
        }
    }
    return sum;
}

static Options street_options(const char *name, BTBHGPUSceneRadianceCascadesTier tier, bool additive) {
    Options options;
    memset(&options, 0, sizeof(options));
    options.name = name;
    options.tier = tier;
    options.additive = additive;
    options.lamp = true;
    options.lamp_x = -0.5f;
    options.lamp_half_width = 0.3f;
    return options;
}

/* The renders a glow is measured with.
   - dark: with tops, without the glow; dark_off: the same without tops.
   - on: with tops; off: without tops.
   - column: with tops, nothing shielded. For the lamp, its hood above it (a glow at 2 units with the lamp's footprint,
     dimmer, so the field's light is the lamp's and its texels' top the hood's); for an emitter, top_above 1000. With
     tops the cascades read the field half a texel from where they read it without (the header, source_tops), so
     what tops shield is `on` against `column`; `off` is the look before. */
typedef struct Renders {
    uint8_t *dark;
    uint8_t *dark_off;
    uint8_t *on;
    uint8_t *off;
    uint8_t *column;
    SmokeCamera camera;
} Renders;

static Renders render_set(SmokeWindow *smoke, const Options *with_glow, const Options *dark, const Options *column) {
    Renders renders;
    static Scene scene;
    Options options = *dark;
    options.source_tops = 1u;
    build_scene(smoke, &options, &scene);
    renders.camera = scene.camera;
    renders.dark = render(smoke, &scene);
    options.source_tops = 0u;
    renders.dark_off = render_options(smoke, &options);
    options = *with_glow;
    options.source_tops = 1u;
    renders.on = render_options(smoke, &options);
    /* Deterministic with tops too. */
    uint8_t *again = render_options(smoke, &options);
    expect_same("tops on, rendered twice", renders.on, again);
    free(again);
    options.source_tops = 0u;
    renders.off = render_options(smoke, &options);
    options = *column;
    options.source_tops = 1u;
    renders.column = render_options(smoke, &options);
    return renders;
}

static void free_set(Renders *renders) {
    free(renders->dark);
    free(renders->dark_off);
    free(renders->on);
    free(renders->off);
    free(renders->column);
}

typedef struct Lights {
    Light on;
    Light off;
    Light column;
} Lights;

static Lights lights_of(const Renders *renders) {
    Lights lights;
    lights.on.lit = renders->on;
    lights.on.dark = renders->dark;
    lights.on.camera = &renders->camera;
    lights.off.lit = renders->off;
    lights.off.dark = renders->dark_off;
    lights.off.camera = &renders->camera;
    lights.column.lit = renders->column;
    lights.column.dark = renders->dark;
    lights.column.camera = &renders->camera;
    return lights;
}

static Renders render_lamp(SmokeWindow *smoke, const Options *with_lamp) {
    Options dark = *with_lamp;
    dark.lamp = false;
    Options column = *with_lamp;
    column.hood = true;
    return render_set(smoke, with_lamp, &dark, &column);
}

/* The two-storey roof's interior (2.5 units in and more) against the street, per tier, the street turned four ways:
   what each keeps of the glow with tops. */
typedef struct StreetShares {
    float roof_a;
    float roof_b;
    float street;
    float roof_b_light;
    float street_light;
} StreetShares;

static void check_street(SmokeWindow *smoke) {
    static const char *const names[2][4] = {
        { "low", "low, turned 90", "low, turned 180", "low, turned 270" },
        { "high", "high, turned 90", "high, turned 180", "high, turned 270" },
    };
    const char *dump = SDL_getenv("BTBH_RC_TOPS_DUMP");
    for (uint32_t tier = 0u; tier < 2u; ++tier) {
        StreetShares shares[4];
        for (uint32_t orientation = 0u; orientation < 4u; ++orientation) {
            /* The lamp half a cell out from the two-storey facade (the RFC's "lamp head next to a two-storey
               house"): its texels lie 0.25 and 0.75 units out (High) or 0.5 (Low). */
            Options with_lamp = street_options(names[tier][orientation],
                tier == 0u ? BTBH_GPU_SCENE_RC_LOW : BTBH_GPU_SCENE_RC_HIGH, true);
            with_lamp.orientation = orientation;
            Renders renders = render_lamp(smoke, &with_lamp);
            write_image(dump, "on-street", names[tier][orientation], renders.on);
            write_image(dump, "column-street", names[tier][orientation], renders.column);
            write_image(dump, "tops-off-street", names[tier][orientation], renders.off);
            const Lights lights = lights_of(&renders);
            measured_orientation = orientation;
            if (measuring) {
                marks = malloc((size_t)TEXELS * 4u);
                assert(marks != NULL);
                memcpy(marks, renders.on, (size_t)TEXELS * 4u);
            }
            const float roof_a_on = roof_a_light(&lights.on, 2.5f);
            const float roof_a_column = roof_a_light(&lights.column, 2.5f);
            const float roof_a_off = roof_a_light(&lights.off, 2.5f);
            const float roof_b_on = roof_b_light(&lights.on);
            const float roof_b_column = roof_b_light(&lights.column);
            const float roof_b_off = roof_b_light(&lights.off);
            const float street_on = street_light(&lights.on);
            const float street_column = street_light(&lights.column);
            const float street_off = street_light(&lights.off);
            if (marks != NULL) {
                write_image(dump, "marks-street", names[tier][orientation], marks);
                free(marks);
                marks = NULL;
            }
            measured_orientation = 0u;
            shares[orientation].roof_a = roof_a_on / roof_a_column;
            shares[orientation].roof_b = roof_b_on / roof_b_column;
            shares[orientation].street = street_on / street_column;
            printf("street %-17s kept with tops: two-storey roof's interior %.1f %%, one-storey roof %.1f %%, street "
                   "%.1f %% (without tops %.3f, %.3f, %.3f; with tops %.3f, %.3f, %.3f)\n",
                names[tier][orientation], 100.0f * shares[orientation].roof_a, 100.0f * shares[orientation].roof_b,
                100.0f * shares[orientation].street, roof_a_off, roof_b_off, street_off, roof_a_column,
                roof_b_column, street_column);
            expect(roof_a_column > 0.05f && roof_b_column > 0.05f && street_column > 0.05f, "the lamp lights");
            expect(shares[orientation].roof_a < (tier == 0u ? 0.05f : 0.25f),
                "the two-storey roof's interior shielded (Low: under 5 %, High: under 25 %)");
            expect(fabsf(shares[orientation].roof_b - 1.0f) < 0.02f, "the one-storey roof as with nothing shielded");
            expect(fabsf(shares[orientation].street - 1.0f) < 0.02f, "the street as with nothing shielded");
            shares[orientation].roof_b_light = roof_b_on;
            shares[orientation].street_light = street_on;
            free_set(&renders);
        }
        /* The street turned any way shields and lights alike with tops (the field read where its texels lie; without
           tops its light lies a texel apart between opposite directions, and the light printed above differs). */
        for (uint32_t orientation = 1u; orientation < 4u; ++orientation) {
            expect(fabsf(shares[orientation].roof_a - shares[0].roof_a) < 0.02f &&
                fabsf(shares[orientation].roof_b - shares[0].roof_b) < 0.02f &&
                fabsf(shares[orientation].street - shares[0].street) < 0.02f,
                "the street turned shields as the street as it stands");
            expect(fabsf(shares[orientation].roof_b_light - shares[0].roof_b_light) < 0.03f * shares[0].roof_b_light &&
                fabsf(shares[orientation].street_light - shares[0].street_light) < 0.03f * shares[0].street_light,
                "the street turned takes the light the street as it stands takes");
        }
    }
    /* The cascades not additive: the same shield. */
    Options on_mode = street_options("high, not additive", BTBH_GPU_SCENE_RC_HIGH, false);
    Renders renders = render_lamp(smoke, &on_mode);
    const Lights lights = lights_of(&renders);
    const float roof_a = roof_a_light(&lights.on, 2.5f) / roof_a_light(&lights.column, 2.5f);
    const float street = street_light(&lights.on) / street_light(&lights.column);
    printf("street %-17s kept with tops: two-storey roof's interior %.1f %%, street %.1f %%\n", on_mode.name,
        100.0f * roof_a, 100.0f * street);
    expect(roof_a < 0.25f, "not additive: the two-storey roof's interior shielded");
    expect(fabsf(street - 1.0f) < 0.02f, "not additive: the street as with nothing shielded");
    free_set(&renders);
}

/* Across the two-storey roof from its street edge (z near 0): the share of the lamp's light kept with tops. The band
   is how far in it keeps at least half. */
typedef struct Band {
    float width;
    float interior;  /* the share kept 2.5 to 4 units in */
} Band;

static Band edge_band(SmokeWindow *smoke, const char *name, BTBHGPUSceneRadianceCascadesTier tier, float lamp_x,
    float lamp_half_width) {
    Options with_lamp = street_options(name, tier, true);
    with_lamp.lamp_x = lamp_x;
    with_lamp.lamp_half_width = lamp_half_width;
    Renders renders = render_lamp(smoke, &with_lamp);
    const Lights lights = lights_of(&renders);
    float band = -1.0f;
    float interior_column = 0.0f;
    float interior_on = 0.0f;
    printf("edge band %-24s kept:", name);
    for (float e = 0.05f; e < 4.0f; e += 0.1f) {
        float kept_column = 0.0f;
        float kept_on = 0.0f;
        for (int z = -1; z <= 1; ++z) {
            kept_column += light_at(&lights.column, houses[0].max_x - e, houses[0].top, 0.25f * (float)z);
            kept_on += light_at(&lights.on, houses[0].max_x - e, houses[0].top, 0.25f * (float)z);
        }
        const float share = kept_column > 0.0f ? kept_on / kept_column : 0.0f;
        if (band < 0.0f && share < 0.5f) {
            band = e - 0.05f;
        }
        if (e >= 2.5f) {
            interior_column += kept_column;
            interior_on += kept_on;
        }
        printf(" %.0f", 100.0f * share);
    }
    if (band < 0.0f) {
        band = 4.0f;
    }
    Band result;
    result.width = band;
    result.interior = interior_on / interior_column;
    printf(" %%; band %.2f units, interior %.0f %%\n", band, 100.0f * result.interior);
    free_set(&renders);
    return result;
}

static void check_edge_band(SmokeWindow *smoke) {
    /* High (texels of half a unit, level-0 pieces of one texel): the lamp's texels at 0.25 and 0.75 units from the
       facade (half a cell, the RFC's "a lamp half a cell out from a facade"), at 0.75 and 1.25 (a cell: the middle of
       the two-cell street), at 1.25 and 1.75. Low (texels of a unit, level-0 pieces of two): the lamp's texels at
       0.5 units (half a cell), at 0.5 and 1.5 (a cell). */
    const Band near = edge_band(smoke, "high, half a cell", BTBH_GPU_SCENE_RC_HIGH, -0.5f, 0.3f);
    const Band middle = edge_band(smoke, "high, a cell", BTBH_GPU_SCENE_RC_HIGH, 0.0f, 0.3f);
    const Band far = edge_band(smoke, "high, a cell and a half", BTBH_GPU_SCENE_RC_HIGH, 0.5f, 0.3f);
    const Band low_near = edge_band(smoke, "low, half a cell", BTBH_GPU_SCENE_RC_LOW, -0.5f, 0.3f);
    const Band low = edge_band(smoke, "low, a cell", BTBH_GPU_SCENE_RC_LOW, 0.0f, 0.6f);
    expect(near.width >= 0.25f && near.width <= 2.0f, "high, half a cell: a band 0.25 to 2 units wide");
    expect(low_near.width >= 0.1f && low_near.width <= 1.0f, "low, half a cell: a band 0.1 to 1 unit wide");
    expect(near.interior < 0.25f, "high, half a cell: the interior keeps under a quarter");
    expect(low_near.interior < 0.05f, "low, half a cell: the interior dark");
    expect(near.width <= middle.width && middle.width <= far.width, "high: the band widens with the lamp's distance");
    expect(near.interior < middle.interior && middle.interior <= far.interior + 0.02f,
        "high: the interior keeps more of a farther lamp's light");
    expect(low_near.interior < low.interior, "low: the interior keeps more of a farther lamp's light");
}

/* The two-storey facade facing the lamp, facing each way (+x: the street as it stands), High and Low: at street level
   it keeps the lamp's light with tops (against nothing shielded), and at least as much as without tops. */
static void check_wall(SmokeWindow *smoke, const char *name, BTBHGPUSceneRadianceCascadesTier tier,
    uint32_t orientation) {
    Options with_lamp = street_options(name, tier, true);
    with_lamp.camera = CAMERA_FACADE;
    with_lamp.orientation = orientation;
    with_lamp.lamp_x = 0.0f;
    with_lamp.lamp_half_width = 0.6f;
    Renders renders = render_lamp(smoke, &with_lamp);
    write_image(SDL_getenv("BTBH_RC_TOPS_DUMP"), "on-wall", name, renders.on);
    write_image(SDL_getenv("BTBH_RC_TOPS_DUMP"), "tops-off-wall", name, renders.off);
    const Lights lights = lights_of(&renders);
    const float facade_x = houses[0].max_x;
    measured_orientation = orientation;
    if (measuring) {
        marks = malloc((size_t)TEXELS * 4u);
        assert(marks != NULL);
        memcpy(marks, renders.on, (size_t)TEXELS * 4u);
    }
    float on = 0.0f;
    float column = 0.0f;
    float off = 0.0f;
    for (int level = 0; level < 3; ++level) {
        const float y = 0.08f + 0.13f * (float)level;
        for (int z = -1; z <= 1; ++z) {
            on += light_at(&lights.on, facade_x, y, 0.4f * (float)z);
            column += light_at(&lights.column, facade_x, y, 0.4f * (float)z);
            off += light_at(&lights.off, facade_x, y, 0.4f * (float)z);
        }
    }
    if (marks != NULL) {
        write_image(SDL_getenv("BTBH_RC_TOPS_DUMP"), "marks-wall", name, marks);
        free(marks);
        marks = NULL;
    }
    measured_orientation = 0u;
    printf("wall %-18s at street level with tops %.3f: %.1f %% of nothing shielded (%.3f), %.1f %% of without tops "
           "(%.3f)\n", name, on, 100.0f * on / column, column, 100.0f * on / off, off);
    expect(column > 0.05f, "the lamp lights the facade");
    expect(fabsf(on - column) < 0.03f * column, "the facade keeps the lamp's light with tops");
    expect(on > 0.9f * off, "the facade keeps at least nine tenths of its light without tops");
    free_set(&renders);
}

static void check_walls(SmokeWindow *smoke) {
    static const char *const names[2][4] = {
        { "facing +x, low", "facing +z, low", "facing -x, low", "facing -z, low" },
        { "facing +x, high", "facing +z, high", "facing -x, high", "facing -z, high" },
    };
    for (uint32_t tier = 0u; tier < 2u; ++tier) {
        for (uint32_t orientation = 0u; orientation < 4u; ++orientation) {
            check_wall(smoke, names[tier][orientation], tier == 0u ? BTBH_GPU_SCENE_RC_LOW : BTBH_GPU_SCENE_RC_HIGH,
                orientation);
        }
    }
}

/* A texel's top is its highest source's (MAX): the lamp's hood (top 2) over it lets the two-storey roof take the
   lamp's light. Flagged NO_RC_SOURCE, the hood writes neither light nor top. */
static void check_highest_top(SmokeWindow *smoke) {
    Options with_lamp = street_options("hood", BTBH_GPU_SCENE_RC_HIGH, true);
    Renders renders = render_lamp(smoke, &with_lamp);
    const Lights lights = lights_of(&renders);
    Options flagged = with_lamp;
    flagged.source_tops = 1u;
    flagged.hood = true;
    flagged.hood_flagged = true;
    uint8_t *flagged_image = render_options(smoke, &flagged);
    const Light flagged_light = { flagged_image, renders.dark, &renders.camera };
    const float lamp = roof_a_light(&lights.on, 2.5f);
    const float hood = roof_a_light(&lights.column, 2.5f);
    const float hood_flagged = roof_a_light(&flagged_light, 2.5f);
    printf("highest top: the two-storey roof's interior with tops: the lamp %.4f, with its hood %.4f, with its hood "
           "flagged %.4f\n", lamp, hood, hood_flagged);
    expect(hood > 4.0f * lamp, "the hood's top lets the roof take the lamp's light");
    expect(fabsf(hood_flagged - lamp) < 0.002f + 0.01f * lamp, "the flagged hood writes no top");
    free(flagged_image);
    free_set(&renders);
}

static void check_emitters(SmokeWindow *smoke) {
    static const struct {
        const char *name;
        Emitter emitter;
    } street_emitters[] = {
        { "shot", EMITTER_SHOT },
        { "rectangle", EMITTER_RECTANGLE },
        { "segment", EMITTER_SEGMENT },
        { "spot", EMITTER_SPOT },
    };
    Options dark;
    memset(&dark, 0, sizeof(dark));
    dark.name = "emitters-dark";
    dark.tier = BTBH_GPU_SCENE_RC_HIGH;
    dark.additive = true;
    for (uint32_t index = 0u; index < sizeof(street_emitters) / sizeof(street_emitters[0]); ++index) {
        Options options = dark;
        options.name = street_emitters[index].name;
        options.emitters = 1u << street_emitters[index].emitter;
        Options column = options;
        column.top_above[street_emitters[index].emitter] = 1000.0f;
        Renders renders = render_set(smoke, &options, &dark, &column);
        const Lights lights = lights_of(&renders);
        const float a_on = roof_a_light(&lights.on, 2.5f);
        const float a_column = roof_a_light(&lights.column, 2.5f);
        const float a_off = roof_a_light(&lights.off, 2.5f);
        const bool spot = street_emitters[index].emitter == EMITTER_SPOT;
        /* (The spot is aimed at the two-storey house: the one-storey roof behind it takes none of its light.) */
        const float b_on = spot ? 1.0f : roof_b_light(&lights.on);
        const float b_column = spot ? 1.0f : roof_b_light(&lights.column);
        printf("emitter %-10s the two-storey roof's interior with tops: top_above 0 %.4f, 1000 %.4f (%.1f %% kept; "
               "without tops %.4f); one-storey roof %.1f %% kept\n", street_emitters[index].name, a_on, a_column,
            100.0f * a_on / a_column, a_off, 100.0f * b_on / b_column);
        expect(a_column > 0.05f, "the emitter lights the roof");
        expect(a_on < 0.25f * a_column, "top_above 0 shields the two-storey roof's interior");
        expect(fabsf(b_on - b_column) < 0.02f * b_column, "the one-storey roof takes the emitter's light");
        free_set(&renders);
    }

    /* A fire on a one-storey roof against the two-storey house: its top is the roof's (0.385) with top_above 0, so
       the two-storey roof's pieces pass over it; with top_above 0.6 (0.985) they don't. */
    Options fire = dark;
    fire.name = "fire";
    fire.emitters = 1u << EMITTER_FIRE;
    Options fire_column = fire;
    fire_column.top_above[EMITTER_FIRE] = 1000.0f;
    Renders renders = render_set(smoke, &fire, &dark, &fire_column);
    const Lights lights = lights_of(&renders);
    Options fire_high = fire;
    fire_high.source_tops = 1u;
    fire_high.top_above[EMITTER_FIRE] = 0.6f;
    uint8_t *high_image = render_options(smoke, &fire_high);
    write_image(SDL_getenv("BTBH_RC_TOPS_DUMP"), "on", "fire-top-above-0.6", high_image);
    const Light high = { high_image, renders.dark, &renders.camera };
    const float a_on = roof_a_light(&lights.on, 2.5f);
    const float a_high = roof_a_light(&high, 2.5f);
    const float a_column = roof_a_light(&lights.column, 2.5f);
    printf("emitter fire       the two-storey roof's interior with tops: top_above 0 %.4f, 0.6 %.4f, 1000 %.4f\n",
        a_on, a_high, a_column);
    expect(a_column > 0.05f, "the fire lights the roof");
    expect(a_on < 0.25f * a_column, "a fire with top_above 0 is passed over");
    expect(fabsf(a_high - a_column) < 0.005f * a_column, "a fire with top_above 0.6 lights the two-storey roof");
    free(high_image);
    free_set(&renders);
}

static BTBHGPUSceneReport refused_present(SmokeWindow *smoke, const Options *options) {
    static Scene scene;
    build_scene(smoke, options, &scene);
    BTBHGPUSceneReport report;
    uint8_t *image = render_scene(smoke, &scene, BTBH_GPU_OK, &report);
    free(image);
    return report;
}

static void check_refusals(SmokeWindow *smoke) {
    Options options = street_options("refusal", BTBH_GPU_SCENE_RC_HIGH, true);
    options.emitters = 1u << EMITTER_SHOT;
    /* source_tops above 1. */
    for (uint32_t value = 2u; value <= 255u; value += 253u) {
        options.source_tops = (uint8_t)value;
        const BTBHGPUSceneReport report = refused_present(smoke, &options);
        printf("refused: source_tops %u (fallback %d)\n", value, (int)report.radiance_cascades.fallback_reason);
        assert(report.radiance_cascades.active_tier == BTBH_GPU_SCENE_RC_OFF);
        assert(report.radiance_cascades.fallback_reason == BTBH_GPU_SCENE_RC_FALLBACK_INVALID_CONFIGURATION);
        /* The next valid present runs the cascades again. */
        options.source_tops = 1u;
        uint8_t *image = render_options(smoke, &options);
        free(image);
    }
    /* With 1, a top_above that isn't finite or is below 0; with 0 the same draws. */
    const float bad[] = { NAN, INFINITY, -INFINITY, -0.01f, -1.0e-30f };
    for (uint32_t index = 0u; index < sizeof(bad) / sizeof(bad[0]); ++index) {
        options.source_tops = 1u;
        options.top_above[EMITTER_SHOT] = bad[index];
        const BTBHGPUSceneReport report = refused_present(smoke, &options);
        printf("refused: top_above %g (fallback %d)\n", (double)bad[index],
            (int)report.radiance_cascades.fallback_reason);
        assert(report.radiance_cascades.active_tier == BTBH_GPU_SCENE_RC_OFF);
        assert(report.radiance_cascades.fallback_reason == BTBH_GPU_SCENE_RC_FALLBACK_INVALID_CONFIGURATION);
        options.source_tops = 0u;
        uint8_t *image = render_options(smoke, &options);
        free(image);
    }
    /* -0 is 0, and the largest float a column. */
    options.source_tops = 1u;
    options.top_above[EMITTER_SHOT] = -0.0f;
    free(render_options(smoke, &options));
    options.top_above[EMITTER_SHOT] = 3.4e38f;
    free(render_options(smoke, &options));
    printf("accepted: top_above -0 and 3.4e38\n");
}

/* The prewarm makes the pipelines a frame with tops draws with: after it, that frame makes none. */
static BTBHGPUResult prewarm(SmokeWindow *smoke, const Options *options, BTBHGPUSceneReport *report) {
    static Scene scene;
    build_scene(smoke, options, &scene);
    const ShaderPaths *paths = smoke->paths;
    memset(report, 0, sizeof(*report));
    return btbh_gpu_window_scene_prewarm_render_to_target(
        smoke->window,
        smoke->target,
        (*paths)[0], (*paths)[1], (*paths)[2], (*paths)[3],
        (*paths)[4], (*paths)[5], (*paths)[6], (*paths)[7],
        (*paths)[8], (*paths)[9], (*paths)[10],
        SMOKE_SHADER_FORMAT,
        TARGET_SIZE,
        TARGET_SIZE,
        &scene.lighting,
        &scene.post,
        scene.batches,
        scene.count,
        report
    );
}

static void check_prewarm(SmokeWindow *smoke) {
    Options options = street_options("prewarm", BTBH_GPU_SCENE_RC_HIGH, true);
    options.source_tops = 1u;
    options.emitters = ALL_EMITTERS;
    BTBHGPUSceneReport report;
    const BTBHGPUResult result = prewarm(smoke, &options, &report);
    if (result != BTBH_GPU_OK) {
        fprintf(stderr, "prewarm: %s\n", report.message);
    }
    assert(result == BTBH_GPU_OK);
    printf("prewarm with tops: ok\n");
}

/* An earlier release's shaders under this core: the gallery as with this release's, and tops refused. */
static void check_old_shaders(SmokeWindow *old, uint8_t *const *images) {
    uint8_t **old_images = check_gallery(old, images, false);
    free_images(old_images);
    Options options = street_options("old shaders", BTBH_GPU_SCENE_RC_HIGH, true);
    options.source_tops = 1u;
    static Scene scene;
    build_scene(old, &options, &scene);
    BTBHGPUSceneReport report;
    (void)render_scene(old, &scene, BTBH_GPU_INVALID_ARGUMENT, &report);
    printf("old shaders, tops on: refused: %s\n", report.message);
    assert(strstr(report.message, "production_hrc_trace.comp") != NULL);
    const BTBHGPUResult result = prewarm(old, &options, &report);
    printf("old shaders, tops on, prewarm: %d: %s\n", (int)result, report.message);
    assert(result == BTBH_GPU_INVALID_ARGUMENT);
    assert(strstr(report.message, "production_hrc_trace.comp") != NULL);
}
#endif

/* ------------------------------------------------------------------------------------------------------ main */

static bool load_shader_paths(ShaderPaths *paths, const char *directory) {
    for (int index = 0; index < 11; ++index) {
        (void)snprintf((*paths)[index], sizeof((*paths)[index]), "%s/%s%s", directory, shader_names[index],
            SMOKE_SHADER_EXTENSION);
        FILE *file = fopen((*paths)[index], "rb");
        if (file == NULL) {
            fprintf(stderr, "skipped: no shader %s\n", (*paths)[index]);
            return false;
        }
        fclose(file);
    }
    return true;
}

static SmokeWindow open_smoke_window(BTBHGPUDevice *device, const char *title, const ShaderPaths *paths) {
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
    BTBHGPUTextureReport texture_report;
    smoke.target = btbh_gpu_window_render_target_create(smoke.window, TARGET_SIZE, TARGET_SIZE,
        BTBH_GPU_TEXTURE_FILTER_LINEAR, &texture_report);
    assert(smoke.target != NULL);
    smoke.paths = paths;
    return smoke;
}

static void close_smoke_window(SmokeWindow *smoke) {
    btbh_gpu_texture_destroy(smoke->target);
    btbh_gpu_custom_mesh_destroy(smoke->cube);
    btbh_gpu_window_destroy(smoke->window);
    memset(smoke, 0, sizeof(*smoke));
}

int main(void) {
    /* (Unbuffered: what was checked shows before a failed assertion.) */
    (void)setvbuf(stdout, NULL, _IONBF, 0);
    if (!load_shader_paths(&shader_paths, BTBH_GPU_SMOKE_SHADER_DIR)) {
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
    for (uint32_t orientation = 0u; orientation < 4u; ++orientation) {
        build_field(&fields[0][orientation], 1.0f, 32u, 1u + orientation, orientation);
        build_field(&fields[1][orientation], 0.5f, 64u, 5u + orientation, orientation);
    }
    SmokeWindow smoke = open_smoke_window(device, "BTBH RC tops smoke", (const ShaderPaths *)&shader_paths);

    const char *measure = SDL_getenv("BTBH_RC_TOPS_MEASURE");
    measuring = measure != NULL && measure[0] == '1';
    uint8_t **images = check_gallery(&smoke, NULL, true);
#if defined(SMOKE_HAS_TOPS)
    check_street(&smoke);
    check_edge_band(&smoke);
    check_walls(&smoke);
    check_highest_top(&smoke);
    check_emitters(&smoke);
    check_refusals(&smoke);
    check_prewarm(&smoke);
    const char *old_dir = SDL_getenv("BTBH_RC_TOPS_OLD_SHADER_DIR");
    if (old_dir != NULL && old_dir[0] != '\0') {
        static ShaderPaths old_paths;
        if (load_shader_paths(&old_paths, old_dir)) {
            SmokeWindow old = open_smoke_window(device, "BTBH RC tops smoke (old shaders)",
                (const ShaderPaths *)&old_paths);
            check_old_shaders(&old, images);
            close_smoke_window(&old);
        } else {
            fprintf(stderr, "BTBH_RC_TOPS_OLD_SHADER_DIR: no shaders in %s\n", old_dir);
            assert(false);
        }
    }
#endif
    free_images(images);
#if defined(SMOKE_HAS_TOPS)
    if (failed_measurements > 0u) {
        printf("%u measurements out of bounds\n", failed_measurements);
    }
#endif

    close_smoke_window(&smoke);
    btbh_gpu_device_destroy(device);
    puts("btbh_gpu_rc_tops_smoke: passed");
    return 0;
}
