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
 *   the SSAO, no cascades (scenes without a transparent unlit batch).
 * - SSR_SURFACE_REFLECTIVITY: without the cascades the reflections (which reflect nothing there unset) appear; on
 *   the dielectric ground, the glass and the grey metal they are byte for byte the additive cascades' with nothing
 *   glowing (which read the F0 from their targets), through the three shadow variants; a gold metal reflects
 *   untinted (differs, printed). With the cascades, or without the reflections, it changes nothing.
 * - Refusals: an unknown batch flag, unknown post flags (present and prewarm).
 *
 * Set (0.10.0, Be The Bullet Hell v2's findings against 0.9.0 and its kaiju's spot shadow):
 * - The radiance cascades' debug views render the same with an additive overlay and a refracting box as without
 *   (the transparent passes no longer draw over them); the frame itself differs.
 * - KEEP_HBAO at night, strong point lights in the building's corners, the overlay and the refracting box: as no
 *   cascades (the transparent unlit batches draw with the fast unlit shader under the flag too); without the flag the
 *   overlay is brighter.
 * - SSR_FACING_HITS on the steep street (a slab floating over wet ground, a tall box standing on it): the slab's
 *   column goes, the box's reflection stays, with no, normal and additive cascades; without SSR nothing changes.
 * - The spot occluder's flags (an orthographic spot map along the sun round a tall box) and
 *   BTBH_GPU_SCENE_BATCH_RECEIVES_NO_SPOT_SHADOW; the new refusals.
 * - BTBH_SMOKE_BENCH=facing: SSR_FACING_HITS's cost on the steep street.
 *
 * Set (0.11.0): KEEP_HBAO's transparent part with any ambient occlusion (v2 draws with contact AO): additive
 * cascades, nothing glowing, the overlays by the neon, with no shadow and the sun's: with contact AO or none the flag
 * renders the frame without the cascades, byte for byte; with the SSAO it changes the overlays; without it they are
 * brighter. With an opaque unlit box by the neon as well (contact AO), only that box's pixels differ from the frame
 * without the cascades. The gallery's neon-contact-additive, neon-ssao-additive and neon-no-ao-additive are those
 * frames unset.
 *
 * Set (0.11.0, Be The Bullet Hell v2's district blackouts): BTBH_GPU_SCENE_BATCH_INSTANCE_LIGHT_FACTOR on the street at
 * night with its windows lit (window_glow 2), its light batches each one of v2's light-carrying kit pieces: glass
 * (pattern 5, 0.6 of its panes lit), a lamp lens (pattern 6), an emissive box, a see-through unlit card with an
 * emission lying on the street (the fast unlit shader where it applies), the skinned quad with an emission laid flat,
 * an emissive box outside the view (the radiance cascades' source field only), an unlit source and an unlit glow
 * outside the view. Through no cascades (the sun's shadow, none), cascades,
 * additive cascades with the omni shadow and the HBAO, and additive cascades with KEEP_HBAO:
 * - every light batch at custom x 1 (or 3) renders byte for byte as unflagged (with custom x 0.25: only the flag reads
 *   it); at 0 (or -1.5, or NaN) as v2's Dark batches (emission_energy 0, the glass's share 0, the lens's pattern 0);
 *   at 0.5 as half the emission's energy and half the glass's share, and the lens at 0.5 as half the window light;
 * - each light batch alone, at 0, is its own Dark look and differs from unflagged, and at 0.5 its half; but the unlit
 *   source without an emission, whose colour stays a source (at 0 it renders as unflagged). Under the cascades the
 *   glows outside the view (lit and unlit) and the skinned quad at 0 and 0.5 are the same batches with no emission and
 *   half its energy, and differ from unflagged across the frame: the source field is scaled, the animated pass too;
 * - prewarming the unflagged scenes in a fresh window leaves the flagged ones no pipeline or shader to make;
 * - refusals, naming the batch (present and prewarm): with INSTANCE_UV_RECT, DISSOLVE, GLITTER, a lit particle, a
 *   refracting batch, a world detail or table seeded by custom x; a detail seeded by w and SHATTER are taken.
 * - BTBH_SMOKE_BENCH=1 (or =ssr, =hbao, =facing, =contact): the flags' cost at 1920x1080, in interleaved turns
 *   (prints only): the reflections without the cascades unset (which return before they march), with the flag and
 *   off; the HBAO and the reflections under additive cascades with KEEP_HBAO and without, and without the cascades;
 *   (0.12.0) contact AO on the night street with its lamps under additive cascades with KEEP_HBAO and without, and
 *   without the cascades. BTBH_SMOKE_BENCH_ROUNDS turns (10, up to 40); BTBH_SCENE_FLAGS_BASELINE_SHADER_DIR=<dir> (an
 *   earlier release's shaders) adds the unset configuration in a window whose pipelines are made from those.
 *
 * Set (0.12.0, Be The Bullet Hell v2's two asks):
 * - KEEP_HBAO's ambient part with contact AO and the SSAO (check_keep_hbao_contact): nothing glowing, additive
 *   cascades, the day street and the night street with two street lamps (over the metal box, before the building's
 *   front) and the neon, through no shadow, the sun's and the omni shadow: with the flag, contact AO and the SSAO
 *   render the frame without the cascades, byte for byte (under the omni shadow at most 4 pixels by a level, the
 *   cascades' shader variant's rounding); without it they differ. With no AO the night street renders that frame with
 *   the flag and without. On a street only the bounce light lights, contact AO and the SSAO with the flag render as no
 *   AO with it (the bounce light isn't darkened); without it they darken it. Contact AO's debug view is the same with
 *   the flag, without it and without the cascades. With the reflections, contact AO and the SSAO under the flag render
 *   as no cascades with SSR_SURFACE_REFLECTIVITY. The opaque unlit box by the neon (0.11.0's leftover) now renders that
 *   frame too, and the SSAO case of 0.11.0's check renders it byte for byte.
 * - BTBH_GPU_SCENE_BATCH_FLAGS2_KEEP_CASCADES_LOOK (check_keep_cascades_look): a beam (a long thin additive unlit box)
 *   by the neon, additive cascades, no AO: with KEEP_HBAO the frame without the cascades; with the bit on the beam too,
 *   the frame under the cascades without KEEP_HBAO, byte for byte. With contact AO or the HBAO the bit changes the
 *   beam's pixels alone. It changes nothing without the cascades, under cascades not additive, without KEEP_HBAO, in a
 *   debug view, on an opaque unlit box, on every batch of the street, on the beam drawn lit. Prewarming the flagged
 *   scenes leaves their renders no pipeline to make. Refusals: flags2's bits 1 to 7 (present and prewarm).
 * - The gallery's lamps-*, dark-bounce-contact-additive and beam-* frames are those scenes unset (as 0.11.0 draws
 *   them).
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
/* 0.10.0: its fixes and flags (the debug views, KEEP_HBAO under point lights, SSR_FACING_HITS, the spot occluder's
   flags and RECEIVES_NO_SPOT_SHADOW). */
#if defined(SMOKE_HAS_FLAGS) && defined(BTBH_GPU_SCENE_HAS_SSR_FACING_HITS) && \
    defined(BTBH_GPU_SCENE_HAS_SPOT_OCCLUDER_FLAGS) && defined(BTBH_GPU_SCENE_HAS_BATCH_RECEIVES_NO_SPOT_SHADOW)
#define SMOKE_HAS_0100 1
#endif
/* 0.11.0: the instance's light factor (Be The Bullet Hell v2's blackouts). */
#if defined(SMOKE_HAS_0100) && defined(BTBH_GPU_SCENE_HAS_INSTANCE_LIGHT_FACTOR) && defined(BTBH_GPU_SMOKE_FIXTURE_DIR)
#define SMOKE_HAS_0110 1
#endif
/* 0.12.0: the batch flags' second byte and KEEP_CASCADES_LOOK; KEEP_HBAO with contact AO and the SSAO. Built against an
   older header (0.11.0's) only the 0.12.0 checks that need no new API compile: run there, they fail where the older
   core draws otherwise (the gallery, first, renders and writes as ever). */
#if defined(SMOKE_HAS_0110) && defined(BTBH_GPU_SCENE_HAS_BATCH_KEEP_CASCADES_LOOK)
#define SMOKE_HAS_0120 1
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
#if defined(SMOKE_HAS_0110)
    BTBHGPUModel *skinned;    /* skinned-quad.gltf */
    BTBHGPUAnimationLibrary *animation;
#endif
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
    bool neon;            /* strong point lights at the building's foot (Be The Bullet Hell v2's signs at night) */
    bool overlay;         /* an additive transparent quad over the street (v2's night overlays) */
    bool steep;           /* the steep street instead (build_steep_scene): thin things over wet ground */
    bool kaiju;           /* dusk, and a tall box alone in an orthographic spot map along the sun (Scene.kaiju_box) */
    /* 0.12.0: */
    bool lamps;           /* night, and two street lamps (BTBHGPUSceneLighting's lamps): over the metal box (a car's
                             roof) and before the building's front (v2's lamp pools) */
    bool beam;            /* a long thin additive unlit box in the air by the building's foot (v2's weapon beams and
                             glows), no source of the cascades' light (Scene.beam_box) */
    bool dark;            /* no light of the scene's own on the street (ambient, sun 0; a spot that reaches nothing):
                             what lights it is the cascades' bounce light alone */
    BTBHGPUSceneRadianceCascadesDebugView debug_view;
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
    uint32_t kaiju_box;
    uint32_t beam_box;
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

/* The steep street (Options.steep): wet, glossy ground under a camera that looks 67 degrees down, a wall at the
   back, and three thin things over the ground: a tall box (a kaiju's body), a slab floating 1.6 units up (a jet) and
   a pole. With SSR, the ground under each reflects it. */
static void build_steep_scene(const SmokeWindow *smoke, Scene *scene) {
    static const float wet[4] = { 0.22f, 0.23f, 0.25f, 1.0f };
    static const float rust[4] = { 0.8f, 0.4f, 0.2f, 1.0f };
    static const float pale[4] = { 0.9f, 0.85f, 0.7f, 1.0f };
    static const float white[4] = { 0.85f, 0.85f, 0.9f, 1.0f };
    BTBHGPUSceneGeometryBatch batch = cube_batch(smoke->cube);
    batch.roughness = 0.06f;
    (void)scene_add(scene, make_box(0.0f, -0.25f, 0.0f, 16.0f, 0.5f, 16.0f, wet), batch);
    batch = cube_batch(smoke->cube);
    batch.roughness = 0.7f;
    (void)scene_add(scene, make_box(0.0f, 1.5f, -4.6f, 9.0f, 3.0f, 1.0f, slate), batch);
    batch = cube_batch(smoke->cube);
    batch.roughness = 0.5f;
    (void)scene_add(scene, make_box(-1.7f, 1.4f, -1.0f, 0.9f, 2.8f, 0.7f, rust), batch);
    (void)scene_add(scene, make_box(1.3f, 1.6f, -0.3f, 1.4f, 0.18f, 0.7f, pale), batch);
    (void)scene_add(scene, make_box(0.1f, 1.25f, 0.9f, 0.15f, 2.5f, 0.15f, white), batch);
}

static void build_scene(const SmokeWindow *smoke, const Options *options, Scene *scene) {
    memset(scene, 0, sizeof(*scene));
    scene->name = options->name;
    scene->glow_box = scene->source_box = scene->far_glow_box = scene->far_source_box = scene->unlit_box =
        scene->kaiju_box = scene->beam_box = MAX_BOXES;
    /* The steep street: the camera looks 67 degrees down. */
    const float eye[3] = { 0.0f, options->steep ? 9.0f : 5.5f, options->steep ? 3.2f : 6.5f };
    const float centre[3] = { 0.0f, options->steep ? 0.0f : 0.2f, options->steep ? -0.6f : -0.5f };
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

    if (options->neon) {
        /* Night, and v2's neon signs: point lights of reach 2.6 and energy 2.1, 0.15 over the ground at the
           building's foot (in the corner it makes with the ground, and in the one between it and the metal box). */
        lighting->ambient_energy = 0.12f;
        lighting->directional_energy = 0.15f;
        static const float neon[2][8] = {
            { -2.2f, 0.15f, -0.75f, 2.6f, 1.0f, 0.35f, 0.85f, 2.1f },
            { -1.3f, 0.15f, -1.1f, 2.6f, 0.3f, 0.9f, 1.0f, 2.1f },
        };
        lighting->point_light_count = 2u;
        memcpy(lighting->point_lights, neon, sizeof(neon));
    }
    if (options->lamps) {
        /* Night, and two warm street lamps: one over the metal box's front (a car's roof and side), one before the
           building's front, low. They light lit surfaces through the scene's BRDF (evaluate_lamps). */
        lighting->ambient_energy = 0.12f;
        lighting->directional_energy = 0.15f;
        static const float lamp_positions[2][4] = {
            { -0.5f, 1.4f, 0.3f, 3.0f },
            { -2.0f, 1.0f, -0.35f, 2.6f },
        };
        static const float lamp_colours[2][4] = {
            { 1.0f, 0.82f, 0.55f, 6.0f },
            { 1.0f, 0.75f, 0.5f, 6.0f },
        };
        lighting->lamp_count = 2u;
        lighting->lamp_core_radius = 0.5f;
        memcpy(lighting->lamp_position_reach, lamp_positions, sizeof(lamp_positions));
        memcpy(lighting->lamp_color_energy, lamp_colours, sizeof(lamp_colours));
    }
    if (options->dark) {
        /* No light of its own on the street: a spot (the scene's light must not be 0 altogether, or every surface
           draws its albedo) far off, whose range ends before it. */
        lighting->ambient_energy = 0.0f;
        lighting->directional_energy = 0.0f;
        lighting->spot_position_x = 0.0f;
        lighting->spot_position_y = 40.0f;
        lighting->spot_position_z = 0.0f;
        lighting->spot_direction_y = -1.0f;
        lighting->spot_range = 1.0f;
        lighting->spot_energy = 1.0f;
        lighting->spot_red = 1.0f;
        lighting->spot_green = 1.0f;
        lighting->spot_blue = 1.0f;
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
    if (options->reflections && options->steep) {
        /* Be The Bullet Hell v2's reflections in the rain. */
        post->ssr_intensity = 6.0f;
        post->ssr_max_roughness = 0.35f;
        post->ssr_max_distance = 12.0f;
        post->ssr_thickness = 0.6f;
    }
    if (options->cascades != CASCADES_OFF) {
        post->radiance_cascades.tier = BTBH_GPU_SCENE_RC_LOW;
        post->radiance_cascades.auto_frame_budget_milliseconds = 16.0f;
        post->radiance_cascades.indirect_intensity = 1.0f;
        post->radiance_cascades.hero_omni_emitter_index = -1;
        post->radiance_cascades.invalidate = 1u;
        post->radiance_cascades.additive = options->cascades == CASCADES_ADDITIVE ? 1u : 0u;
        post->radiance_cascades.debug_view = options->debug_view;
    }
    if (options->steep) {
        build_steep_scene(smoke, scene);
        return;
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
    /* An unlit receiver (not on the dark street, which only the bounce light lights: its colour is its own). */
    if (!options->dark) {
        batch = cube_batch(smoke->cube);
        batch.unlit_enabled = 1u;
        batch.rc_receiver = 1u;
        (void)scene_add(scene, make_box(2.4f, 0.3f, 0.9f, 0.6f, 0.6f, 0.6f, slate), batch);
    }
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
    if (options->overlay) {
        /* An additive wash over the street, as v2's night overlays (street wash, lamp pools): a thin box drawn in the
           transparent pass. */
        batch = cube_batch(smoke->cube);
        batch.unlit_enabled = 1u;
        batch.transparency_enabled = 1u;
        batch.blend_mode = BTBH_GPU_SCENE_BLEND_ADDITIVE;
        batch.depth_compare = BTBH_GPU_SCENE_DEPTH_LESS_EQUAL;
        batch.albedo_alpha = 160u;
        batch.spot_shadow_visible = 0u;
        batch.flags = BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW;
#if defined(BTBH_GPU_SCENE_HAS_BATCH_NO_RC_SOURCE)
        /* (No source of the cascades' light: it only draws over the frame.) */
        batch.flags |= BTBH_GPU_SCENE_BATCH_NO_RC_SOURCE;
#endif
        (void)scene_add(scene, make_box(0.6f, 0.02f, 0.6f, 3.0f, 0.02f, 2.5f, hot), batch);
        /* And a refracting box (the refraction pass). */
        batch = cube_batch(smoke->cube);
        batch.surface_variant = BTBH_GPU_SCENE_SURFACE_REFRACT;
        batch.transparency_enabled = 1u;
        batch.surface_parameters0[0] = 0.05f;
        batch.spot_shadow_visible = 0u;
        (void)scene_add(scene, make_box(1.2f, 0.5f, 1.4f, 0.8f, 0.8f, 0.8f, glass), batch);
    }
    if (options->beam) {
        /* A beam: a long thin additive unlit box 0.3 over the street by the neon at the building's foot, drawn in the
           transparent pass, no source of the cascades' light. */
        static const float beam_colour[4] = { 0.15f, 0.3f, 0.45f, 1.0f };
        batch = cube_batch(smoke->cube);
        batch.unlit_enabled = 1u;
        batch.transparency_enabled = 1u;
        batch.blend_mode = BTBH_GPU_SCENE_BLEND_ADDITIVE;
        batch.depth_compare = BTBH_GPU_SCENE_DEPTH_LESS_EQUAL;
        batch.albedo_alpha = 200u;
        batch.spot_shadow_visible = 0u;
        batch.flags = BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW;
#if defined(BTBH_GPU_SCENE_HAS_BATCH_NO_RC_SOURCE)
        batch.flags |= BTBH_GPU_SCENE_BATCH_NO_RC_SOURCE;
#endif
        scene->beam_box = scene_add(scene, make_box(-1.0f, 0.3f, 0.05f, 3.4f, 0.16f, 0.16f, beam_colour), batch);
    }
    if (options->kaiju) {
        /* Dusk: a low warm sun, a cool fill and sky. Be The Bullet Hell v2's kaiju shadow: only the kaiju (a tall box)
           in the spot map, an orthographic one fitted along the sun round it (near plane 1 unit from the eye, 8 units
           sunward of the box's middle; far plane 30), its occluder darkening at full strength; no spot light. The
           box casts into the sun's map too, unless a check flags it. */
        lighting->ambient_red = 0.55f;
        lighting->ambient_green = 0.65f;
        lighting->ambient_blue = 1.0f;
        lighting->ambient_energy = 0.35f;
        lighting->directional_red = 1.0f;
        lighting->directional_green = 0.62f;
        lighting->directional_blue = 0.35f;
        lighting->directional_energy = 1.3f;
        lighting->secondary_directional_x = 0.4f;
        lighting->secondary_directional_y = -1.0f;
        lighting->secondary_directional_z = -0.3f;
        lighting->secondary_directional_red = 0.5f;
        lighting->secondary_directional_green = 0.6f;
        lighting->secondary_directional_blue = 1.0f;
        lighting->secondary_directional_energy = 0.4f;
        for (uint32_t index = 0u; index < scene->count; ++index) {
            scene->batches[index].spot_shadow_visible = 0u;
        }
        batch = cube_batch(smoke->cube);
        batch.roughness = 0.6f;
        static const float body[4] = { 0.45f, 0.5f, 0.42f, 1.0f };
        const float centre_point[3] = { 1.0f, 1.1f, 1.3f };
        scene->kaiju_box = scene_add(scene, make_box(centre_point[0], centre_point[1], centre_point[2], 0.8f, 2.2f,
            0.8f, body), batch);
        float to_sun[3] = { 4.0f, 10.0f, 3.0f };
        normalize3(to_sun);
        const float eye_point[3] = {
            centre_point[0] + to_sun[0] * 8.0f, centre_point[1] + to_sun[1] * 8.0f, centre_point[2] + to_sun[2] * 8.0f,
        };
        float view[16];
        float side[3];
        float up[3];
        look_at_matrix(eye_point, centre_point, view, side, up);
        const float rcp = 1.0f / 2.0f;
        const float range = 1.0f / (1.0f - 30.0f);
        const float orthographic[16] = {
            rcp, 0.0f, 0.0f, 0.0f,
            0.0f, rcp, 0.0f, 0.0f,
            0.0f, 0.0f, range, 0.0f,
            0.0f, 0.0f, range * 1.0f, 1.0f,
        };
        multiply(orthographic, view, lighting->spot_shadow_view_projection);
        lighting->spot_position_x = eye_point[0];
        lighting->spot_position_y = eye_point[1];
        lighting->spot_position_z = eye_point[2];
        lighting->spot_direction_x = -to_sun[0];
        lighting->spot_direction_y = -to_sun[1];
        lighting->spot_direction_z = -to_sun[2];
        lighting->spot_range = 8.0f;
        lighting->spot_energy = 0.0f;
        lighting->spot_shadow_enabled = 1u;
        lighting->spot_shadow_is_omni = 0u;
        lighting->spot_shadow_softness = 1.0f;
        lighting->spot_occluder_shadow_strength = 1.0f;
        lighting->spot_occluder_shadow_reach = 10000.0f;
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
    /* Since 0.10.0's fixes: what they touch, unset. */
    { .name = "neon-hbao", .shadows = SHADOWS_SUN, .occlusion = OCCLUSION_HBAO, .gold = true, .unlit_box = true,
      .neon = true, .overlay = true },
    { .name = "neon-hbao-additive", .shadows = SHADOWS_SUN, .cascades = CASCADES_ADDITIVE,
      .occlusion = OCCLUSION_HBAO, .gold = true, .unlit_box = true, .neon = true, .overlay = true },
    { .name = "neon-hbao-additive-omni", .shadows = SHADOWS_OMNI, .cascades = CASCADES_ADDITIVE,
      .occlusion = OCCLUSION_HBAO, .gold = true, .unlit_box = true, .neon = true, .overlay = true },
    { .name = "neon-hbao-rc", .shadows = SHADOWS_SUN, .cascades = CASCADES_ON, .occlusion = OCCLUSION_HBAO,
      .gold = true, .unlit_box = true, .neon = true, .overlay = true },
    { .name = "overlay-rc", .shadows = SHADOWS_SUN, .cascades = CASCADES_ON, .glow = true, .overlay = true },
    { .name = "overlay-additive", .shadows = SHADOWS_SUN, .cascades = CASCADES_ADDITIVE, .occlusion = OCCLUSION_HBAO,
      .glow = true, .overlay = true },
    { .name = "steep-ssr-rc", .shadows = SHADOWS_SUN, .cascades = CASCADES_ON, .reflections = true, .steep = true },
    { .name = "steep-ssr-additive", .shadows = SHADOWS_SUN, .cascades = CASCADES_ADDITIVE, .reflections = true,
      .steep = true },
    { .name = "steep-ssr-hbao", .shadows = SHADOWS_SUN, .occlusion = OCCLUSION_HBAO, .reflections = true,
      .steep = true },
    { .name = "kaiju-spot", .shadows = SHADOWS_SUN, .kaiju = true },
    { .name = "kaiju-spot-rc", .shadows = SHADOWS_SUN, .cascades = CASCADES_ON, .kaiju = true },
    { .name = "kaiju-spot-hbao-additive", .shadows = SHADOWS_SUN, .cascades = CASCADES_ADDITIVE,
      .occlusion = OCCLUSION_HBAO, .kaiju = true },
    /* Since 0.11.0's KEEP_HBAO with any ambient occlusion: the overlays by the neon under additive cascades with contact
       AO, the SSAO and none, unset (as 0.10.0 draws them). */
    { .name = "neon-contact-additive", .shadows = SHADOWS_SUN, .cascades = CASCADES_ADDITIVE,
      .occlusion = OCCLUSION_HBAO_CONTACT, .gold = true, .unlit_box = true, .neon = true, .overlay = true },
    { .name = "neon-ssao-additive", .shadows = SHADOWS_SUN, .cascades = CASCADES_ADDITIVE,
      .occlusion = OCCLUSION_SSAO, .gold = true, .unlit_box = true, .neon = true, .overlay = true },
    { .name = "neon-no-ao-additive", .shadows = SHADOWS_NONE, .cascades = CASCADES_ADDITIVE,
      .gold = true, .unlit_box = true, .neon = true, .overlay = true },
    /* Since 0.12.0's KEEP_HBAO with contact AO and the SSAO, and its per-batch opt-out: what they touch, unset (as
       0.11.0 draws them). */
    { .name = "lamps-contact", .shadows = SHADOWS_SUN, .occlusion = OCCLUSION_HBAO_CONTACT, .gold = true,
      .unlit_box = true, .neon = true, .lamps = true },
    { .name = "lamps-contact-additive", .shadows = SHADOWS_SUN, .cascades = CASCADES_ADDITIVE,
      .occlusion = OCCLUSION_HBAO_CONTACT, .gold = true, .unlit_box = true, .neon = true, .lamps = true },
    { .name = "lamps-contact-additive-omni", .shadows = SHADOWS_OMNI, .cascades = CASCADES_ADDITIVE,
      .occlusion = OCCLUSION_HBAO_CONTACT, .gold = true, .unlit_box = true, .neon = true, .lamps = true },
    { .name = "lamps-ssao-additive", .shadows = SHADOWS_SUN, .cascades = CASCADES_ADDITIVE,
      .occlusion = OCCLUSION_SSAO, .gold = true, .unlit_box = true, .neon = true, .lamps = true },
    { .name = "dark-bounce-contact-additive", .shadows = SHADOWS_NONE, .cascades = CASCADES_ADDITIVE,
      .occlusion = OCCLUSION_HBAO_CONTACT, .far_glow = true, .dark = true },
    { .name = "beam-additive", .shadows = SHADOWS_SUN, .cascades = CASCADES_ADDITIVE, .neon = true, .beam = true },
    { .name = "beam-contact-additive", .shadows = SHADOWS_SUN, .cascades = CASCADES_ADDITIVE,
      .occlusion = OCCLUSION_HBAO_CONTACT, .neon = true, .lamps = true, .beam = true },
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
        for (uint32_t index = 0u, shown = 0u; index < TEXELS && shown < 8u; ++index) {
            if (memcmp(&a[index * 4u], &b[index * 4u], 4u) != 0) {
                fprintf(stderr, "  at %u,%u: %d %d %d against %d %d %d\n", index % TARGET_SIZE, index / TARGET_SIZE,
                    a[index * 4u], a[index * 4u + 1u], a[index * 4u + 2u], b[index * 4u], b[index * 4u + 1u],
                    b[index * 4u + 2u]);
                shown += 1u;
            }
        }
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
    /* Ignored where it doesn't apply. (No transparent unlit batch here: since 0.11.0 the flag's transparent part
       applies with contact AO, the SSAO and no AO too, check_keep_hbao_any_ao; since 0.12.0 the rest applies with
       contact AO and the SSAO too, check_keep_hbao_contact. With no AO nothing is darkened: only the transparent part
       applies.) */
    {
        struct { const char *what; Options options; Cascades cascades; } cases[3] = {
            { "cascades not additive", { .name = "keep-ignored-rc", .shadows = SHADOWS_SUN,
                .occlusion = OCCLUSION_HBAO, .glow = true }, CASCADES_ON },
            { "no AO", { .name = "keep-ignored-no-ao", .shadows = SHADOWS_SUN,
                .occlusion = OCCLUSION_NONE, .glow = true, .gold = true, .unlit_box = true }, CASCADES_ADDITIVE },
            { "no cascades", { .name = "keep-ignored-off", .shadows = SHADOWS_SUN,
                .occlusion = OCCLUSION_HBAO, .glow = true }, CASCADES_OFF },
        };
        for (uint32_t index = 0u; index < 3u; ++index) {
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

#if defined(SMOKE_HAS_0100)
/* The radiance cascades' debug views (0.10.0): the transparent and refracting batches no longer draw over them. In
   the frame itself they still show. */
static void check_rc_debug_views(SmokeWindow *smoke) {
    static const BTBHGPUSceneRadianceCascadesDebugView views[] = {
        BTBH_GPU_SCENE_RC_DEBUG_CASCADE_0, BTBH_GPU_SCENE_RC_DEBUG_HIT_DISTANCE, BTBH_GPU_SCENE_RC_DEBUG_CONFIDENCE,
        BTBH_GPU_SCENE_RC_DEBUG_FINAL_INDIRECT, BTBH_GPU_SCENE_RC_DEBUG_DIRECTIONAL_SOURCE,
        BTBH_GPU_SCENE_RC_DEBUG_DIRECT_LIGHT,
    };
    const Cascades kinds[2] = { CASCADES_ON, CASCADES_ADDITIVE };
    for (uint32_t kind = 0u; kind < 2u; ++kind) {
        const char *label = kinds[kind] == CASCADES_ON ? "cascades" : "additive cascades";
        Options options = { .name = "debug-view", .shadows = SHADOWS_SUN, .cascades = kinds[kind],
            .occlusion = OCCLUSION_HBAO, .glow = true };
        uint8_t *plain = render_with(smoke, options, kinds[kind], 0u, false);
        options.overlay = true;
        uint8_t *overlaid = render_with(smoke, options, kinds[kind], 0u, false);
        char what[200];
        (void)snprintf(what, sizeof(what), "debug views, %s: the frame with the overlays against without", label);
        (void)expect_different(what, plain, overlaid);
        free(plain);
        free(overlaid);
        for (uint32_t index = 0u; index < sizeof(views) / sizeof(views[0]); ++index) {
            options.debug_view = views[index];
            options.overlay = false;
            plain = render_with(smoke, options, kinds[kind], 0u, false);
            options.overlay = true;
            overlaid = render_with(smoke, options, kinds[kind], 0u, false);
            (void)snprintf(what, sizeof(what), "debug view %d, %s: with the overlays against without",
                (int)views[index], label);
            expect_same(what, plain, overlaid);
            if (views[index] == BTBH_GPU_SCENE_RC_DEBUG_FINAL_INDIRECT && kinds[kind] == CASCADES_ADDITIVE) {
                dump_image("debug-final-indirect-overlaid", overlaid);
            }
            free(plain);
            free(overlaid);
        }
    }
}

/* KEEP_HBAO at night under strong point lights by the building, with an additive overlay and a refracting box in the
   transparent pass (0.10.0; Be The Bullet Hell v2's neon signs over its night overlays). Under the cascades a
   transparent unlit batch drew with their scene shader, which brightens unlit surfaces by the point lights; without
   them, and now with the flag, the fast unlit shader draws it, which takes none. Without the flag the cascades keep
   the scene shader (the overlay brighter, printed). Under the omni shadow, a pixel or two of the ground differs by a
   level: the two scene shader variants (production_model.frag, production_model_rc.frag) round that light
   differently, with or without the point lights (as in 0.9.0). */
static void check_keep_hbao_point_lights(SmokeWindow *smoke) {
    const uint8_t keep = BTBH_GPU_SCENE_POST_ADDITIVE_CASCADES_KEEP_HBAO;
    static const char *const shadow_names[3] = { "no shadow", "the sun's shadow", "the omni shadow" };
    for (uint32_t shadows = 0u; shadows < 3u; ++shadows) {
        const Options options = { .name = "keep-hbao-neon", .shadows = (Shadows)shadows,
            .occlusion = OCCLUSION_HBAO, .gold = true, .unlit_box = true, .neon = true, .overlay = true };
        uint8_t *off = render_with(smoke, options, CASCADES_OFF, 0u, true);
        uint8_t *additive = render_with(smoke, options, CASCADES_ADDITIVE, 0u, true);
        uint8_t *kept = render_with(smoke, options, CASCADES_ADDITIVE, keep, true);
        char what[200];
        (void)snprintf(what, sizeof(what),
            "KEEP_HBAO, %s, nothing glowing, point lights by the overlays: additive cascades with it against none",
            shadow_names[shadows]);
        if (shadows == (uint32_t)SHADOWS_OMNI) {
            int largest = 0;
            const uint32_t differing = differing_pixels(off, kept, &largest);
            printf("%s: %u pixels differ (largest %d levels)\n", what, differing, largest);
            assert(differing <= 4u && largest <= 1);
        } else {
            expect_same(what, off, kept);
        }
        (void)snprintf(what, sizeof(what),
            "KEEP_HBAO, %s, nothing glowing, point lights by the overlays: additive cascades without it against none",
            shadow_names[shadows]);
        (void)expect_different(what, off, additive);
        if (shadows == 1u) {
            dump_image("keep-hbao-neon-off", off);
            dump_image("keep-hbao-neon-additive", additive);
            dump_image("keep-hbao-neon-kept", kept);
        }
        free(off);
        free(additive);
        free(kept);
    }
}

#if defined(SMOKE_HAS_0110)
/* 0.11.0: KEEP_HBAO's transparent part with any ambient occlusion (Be The Bullet Hell v2 draws with contact AO, where
   0.10.0 ignored the flag). Additive cascades, nothing glowing, the overlays by the neon: with contact AO, the SSAO or
   none the flag renders the frame without the cascades (with the SSAO since 0.12.0, whose own darkening differed under
   the cascades in 0.11.0). Without it the overlays are brighter (the gallery's unset neon-*-additive frames keep
   0.10.0's). With an opaque unlit box by the neon too, contact AO left that box as the cascades draw it in 0.11.0 (the
   flag's other part needed the HBAO there); since 0.12.0 the frame is the frame without the cascades there too. */
static void check_keep_hbao_any_ao(SmokeWindow *smoke) {
    const uint8_t keep = BTBH_GPU_SCENE_POST_ADDITIVE_CASCADES_KEEP_HBAO;
    static const struct {
        const char *what;
        Occlusion occlusion;
        bool as_off;
    } cases[3] = {
        { "contact AO", OCCLUSION_HBAO_CONTACT, true },
        { "no AO", OCCLUSION_NONE, true },
#if defined(SMOKE_HAS_0120)
        { "the SSAO", OCCLUSION_SSAO, true },
#else
        { "the SSAO", OCCLUSION_SSAO, false },
#endif
    };
    static const char *const shadow_names[2] = { "no shadow", "the sun's shadow" };
    for (uint32_t shadows = 0u; shadows < 2u; ++shadows) {
        for (uint32_t index = 0u; index < 3u; ++index) {
            const Options options = { .name = "keep-any-ao", .shadows = (Shadows)shadows,
                .occlusion = cases[index].occlusion, .gold = true, .neon = true, .overlay = true };
            uint8_t *off = render_with(smoke, options, CASCADES_OFF, 0u, true);
            uint8_t *additive = render_with(smoke, options, CASCADES_ADDITIVE, 0u, true);
            uint8_t *kept = render_with(smoke, options, CASCADES_ADDITIVE, keep, true);
            char what[200];
            (void)snprintf(what, sizeof(what),
                "KEEP_HBAO, %s, %s, nothing glowing, point lights by the overlays: additive cascades with it against "
                "without", cases[index].what, shadow_names[shadows]);
            (void)expect_different(what, kept, additive);
            (void)snprintf(what, sizeof(what),
                "KEEP_HBAO, %s, %s, nothing glowing, point lights by the overlays: additive cascades with it against "
                "none", cases[index].what, shadow_names[shadows]);
            if (cases[index].as_off) {
                expect_same(what, kept, off);
            } else {
                int largest = 0;
                printf("%s: %u pixels differ (largest %d levels; the SSAO's own)\n", what,
                    differing_pixels(kept, off, &largest), largest);
            }
            (void)snprintf(what, sizeof(what),
                "KEEP_HBAO, %s, %s, nothing glowing, point lights by the overlays: additive cascades without it "
                "against none", cases[index].what, shadow_names[shadows]);
            (void)expect_different(what, additive, off);
            if (shadows == 1u && index == 0u) {
                dump_image("keep-contact-neon-off", off);
                dump_image("keep-contact-neon-additive", additive);
                dump_image("keep-contact-neon-kept", kept);
            }
            free(off);
            free(additive);
            free(kept);
        }
    }
    static Scene scene;
    const Options unlit = { .name = "keep-contact-unlit", .shadows = SHADOWS_SUN, .occlusion = OCCLUSION_HBAO_CONTACT,
        .gold = true, .unlit_box = true, .neon = true, .overlay = true };
    uint8_t *off = render_with(smoke, unlit, CASCADES_OFF, 0u, true);
    uint8_t *kept = render_with(smoke, unlit, CASCADES_ADDITIVE, keep, true);
    build_scene(smoke, &unlit, &scene);
    int rectangle[4];
    box_rectangle(&scene, scene.unlit_box, rectangle);
    uint32_t inside = 0u;
    uint32_t outside = 0u;
    for (uint32_t y = 0u; y < TARGET_SIZE; ++y) {
        for (uint32_t x = 0u; x < TARGET_SIZE; ++x) {
            const uint32_t at = (y * TARGET_SIZE + x) * 4u;
            if (memcmp(&off[at], &kept[at], 4u) != 0) {
                const bool in = (int)x >= rectangle[0] && (int)x <= rectangle[2] && (int)y >= rectangle[1] &&
                    (int)y <= rectangle[3];
                inside += in ? 1u : 0u;
                outside += in ? 0u : 1u;
            }
        }
    }
    printf("KEEP_HBAO, contact AO, the sun's shadow, an opaque unlit box by the point lights too: additive cascades "
           "with it against none: %u pixels differ on the box's rectangle, %u outside it\n",
        inside, outside);
    assert(outside == 0u);
#if defined(SMOKE_HAS_0120)
    assert(inside == 0u);
#endif
    free(off);
    free(kept);
}

/* Prints and asserts a comparison that must come out the same, but under the omni shadow, where at most 4 pixels may
   differ by a level: the cascades' variant of the scene shader (production_model_rc.frag) rounds that light apart from
   the plain one (0.9.0's and 0.10.0's note). Returns the pixels that differ. */
static uint32_t expect_same_but_omni_rounding(const char *what, Shadows shadows, const uint8_t *a, const uint8_t *b) {
    if (shadows != SHADOWS_OMNI) {
        expect_same(what, a, b);
        return 0u;
    }
    int largest = 0;
    const uint32_t differing = differing_pixels(a, b, &largest);
    printf("%s: %u pixels differ (largest %d levels; the omni shadow's variant rounding allows 4 by 1)\n", what,
        differing, largest);
    assert(differing <= 4u && largest <= 1);
    return differing;
}

/* 0.12.0: KEEP_HBAO's ambient part with contact AO (ssao_contact 1) and the SSAO (ssao_enabled 1). Be The Bullet Hell
   v2 draws with contact AO, where under additive cascades lit surfaces in a street lamp's pool (a car's roof, a facade)
   came out up to 94 levels brighter than without the cascades, with the flag or without: the cascades' scene shader
   puts the street lamps' light in the emission target, which contact AO keeps; and an opaque unlit surface's colour.
   Additive cascades, nothing glowing (the unlit box no source):
   - the day street, and the night street with its street lamps (over the metal box and before the building's front)
     and the neon at the building's foot, through no shadow, the sun's and the omni shadow: with the flag, contact AO
     and the SSAO render the frame without the cascades, byte for byte (under the omni shadow at most 4 pixels by a
     level, expect_same_but_omni_rounding); without it they differ (printed; the gallery's unset lamps-* frames keep
     0.11.0's);
   - with no AO the night street renders the frame without the cascades with the flag and without: nothing darkens it,
     and the flag changes nothing there (no transparent unlit batch).
   The bounce light isn't darkened: on a street with no light of its own, which the cascades' bounce light from glows
   outside the view lights alone, contact AO and the SSAO with the flag render as no AO with it, byte for byte; without
   the flag they darken that light. Contact AO's debug view is the same with the flag, without it and without the
   cascades; with the reflections, both render as no cascades with SSR_SURFACE_REFLECTIVITY. */
static void check_keep_hbao_contact(SmokeWindow *smoke) {
    const uint8_t keep = BTBH_GPU_SCENE_POST_ADDITIVE_CASCADES_KEEP_HBAO;
    static const char *const shadow_names[3] = { "no shadow", "the sun's shadow", "the omni shadow" };
    static const struct {
        const char *what;
        Occlusion occlusion;
    } occlusions[2] = {
        { "contact AO", OCCLUSION_HBAO_CONTACT },
        { "the SSAO", OCCLUSION_SSAO },
    };
    uint32_t rounded = 0u;
    for (uint32_t street = 0u; street < 2u; ++street) {
        const char *street_name = street == 0u ? "by day" : "at night by the street lamps and the neon";
        for (uint32_t which = 0u; which < 2u; ++which) {
            for (uint32_t shadows = 0u; shadows < 3u; ++shadows) {
                const Options options = { .name = street == 0u ? "keep-contact-day" : "keep-contact-lamps",
                    .shadows = (Shadows)shadows, .occlusion = occlusions[which].occlusion, .gold = true,
                    .unlit_box = true, .neon = street != 0u, .lamps = street != 0u };
                uint8_t *off = render_with(smoke, options, CASCADES_OFF, 0u, true);
                uint8_t *additive = render_with(smoke, options, CASCADES_ADDITIVE, 0u, true);
                uint8_t *kept = render_with(smoke, options, CASCADES_ADDITIVE, keep, true);
                char what[240];
                (void)snprintf(what, sizeof(what),
                    "KEEP_HBAO, %s, %s, %s, nothing glowing: additive cascades with it against none",
                    occlusions[which].what, street_name, shadow_names[shadows]);
                rounded += expect_same_but_omni_rounding(what, (Shadows)shadows, kept, off);
                (void)snprintf(what, sizeof(what),
                    "KEEP_HBAO, %s, %s, %s, nothing glowing: additive cascades without it against none",
                    occlusions[which].what, street_name, shadow_names[shadows]);
                (void)expect_different(what, additive, off);
                if (street == 1u && shadows == 1u) {
                    print_darkening("  without it against none", off, additive);
                    dump_image(which == 0u ? "keep-contact-lamps-off" : "keep-ssao-lamps-off", off);
                    dump_image(which == 0u ? "keep-contact-lamps-additive" : "keep-ssao-lamps-additive", additive);
                    dump_image(which == 0u ? "keep-contact-lamps-kept" : "keep-ssao-lamps-kept", kept);
                }
                free(off);
                free(additive);
                free(kept);
            }
        }
    }
    for (uint32_t shadows = 0u; shadows < 3u; ++shadows) {
        const Options options = { .name = "keep-no-ao-lamps", .shadows = (Shadows)shadows,
            .occlusion = OCCLUSION_NONE, .gold = true, .unlit_box = true, .neon = true, .lamps = true };
        uint8_t *off = render_with(smoke, options, CASCADES_OFF, 0u, true);
        uint8_t *additive = render_with(smoke, options, CASCADES_ADDITIVE, 0u, true);
        uint8_t *kept = render_with(smoke, options, CASCADES_ADDITIVE, keep, true);
        char what[240];
        (void)snprintf(what, sizeof(what), "KEEP_HBAO, no AO, at night by the street lamps and the neon, %s, nothing "
            "glowing: additive cascades with it against none", shadow_names[shadows]);
        rounded += expect_same_but_omni_rounding(what, (Shadows)shadows, kept, off);
        (void)snprintf(what, sizeof(what), "KEEP_HBAO, no AO, at night by the street lamps and the neon, %s, nothing "
            "glowing: additive cascades without it against none", shadow_names[shadows]);
        rounded += expect_same_but_omni_rounding(what, (Shadows)shadows, additive, off);
        free(off);
        free(additive);
        free(kept);
    }
    /* The bounce light alone. */
    {
        Options options = { .name = "keep-contact-bounce", .shadows = SHADOWS_NONE, .occlusion = OCCLUSION_NONE,
            .far_glow = true, .dark = true };
        uint8_t *none = render_with(smoke, options, CASCADES_OFF, 0u, false);
        uint8_t *bounce = render_with(smoke, options, CASCADES_ADDITIVE, 0u, false);
        uint8_t *bounce_kept = render_with(smoke, options, CASCADES_ADDITIVE, keep, false);
        (void)expect_different("KEEP_HBAO, the bounce light alone, no AO: additive cascades against none", bounce,
            none);
        expect_same("KEEP_HBAO, the bounce light alone, no AO: additive cascades with it against without", bounce_kept,
            bounce);
        for (uint32_t which = 0u; which < 2u; ++which) {
            options.occlusion = occlusions[which].occlusion;
            uint8_t *occluded = render_with(smoke, options, CASCADES_ADDITIVE, 0u, false);
            uint8_t *occluded_kept = render_with(smoke, options, CASCADES_ADDITIVE, keep, false);
            char what[240];
            (void)snprintf(what, sizeof(what), "KEEP_HBAO, the bounce light alone, %s: additive cascades with it "
                "against no AO with it", occlusions[which].what);
            expect_same(what, occluded_kept, bounce_kept);
            (void)snprintf(what, sizeof(what), "KEEP_HBAO, the bounce light alone, %s: additive cascades without it "
                "against no AO without it (it darkens the bounce light)", occlusions[which].what);
            (void)expect_different(what, occluded, bounce);
            if (which == 0u) {
                dump_image("keep-contact-bounce-additive", occluded);
                dump_image("keep-contact-bounce-kept", occluded_kept);
            }
            free(occluded);
            free(occluded_kept);
        }
        free(none);
        free(bounce);
        free(bounce_kept);
    }
    /* Contact AO's debug view (the occlusion alone, in grey) on the glowing street: the same with the flag, without
       it and without the cascades (it comes after the cascades' bounce light, as before). */
    {
        static Scene scene;
        const Options options = { .name = "keep-contact-debug", .shadows = SHADOWS_SUN,
            .occlusion = OCCLUSION_HBAO_CONTACT, .glow = true, .far_glow = true, .gold = true, .unlit_box = true,
            .lamps = true };
        uint8_t *views[3];
        static const Cascades cascades[3] = { CASCADES_OFF, CASCADES_ADDITIVE, CASCADES_ADDITIVE };
        for (uint32_t index = 0u; index < 3u; ++index) {
            Options each = options;
            each.cascades = cascades[index];
            build_scene(smoke, &each, &scene);
            scene.post.ssao_debug_view = 1u;
            scene.post.flags = index == 2u ? keep : 0u;
            views[index] = render(smoke, &scene);
        }
        expect_same("KEEP_HBAO, contact AO's debug view, glowing: additive cascades with it against without",
            views[2], views[1]);
        expect_same("KEEP_HBAO, contact AO's debug view, glowing: additive cascades with it against none", views[2],
            views[0]);
        for (uint32_t index = 0u; index < 3u; ++index) {
            free(views[index]);
        }
    }
    /* With the reflections, nothing glowing: the reflectivity from the ambient target's alpha, as without the cascades
       with SSR_SURFACE_REFLECTIVITY (as with the HBAO under the flag, check_keep_hbao). */
    for (uint32_t which = 0u; which < 2u; ++which) {
        const Options options = { .name = "keep-contact-ssr", .shadows = SHADOWS_SUN,
            .occlusion = occlusions[which].occlusion, .reflections = true, .gold = true, .unlit_box = true,
            .neon = true, .lamps = true };
        uint8_t *off = render_with(smoke, options, CASCADES_OFF, BTBH_GPU_SCENE_POST_SSR_SURFACE_REFLECTIVITY, true);
        uint8_t *kept = render_with(smoke, options, CASCADES_ADDITIVE, keep, true);
        char what[240];
        (void)snprintf(what, sizeof(what), "KEEP_HBAO with the reflections, %s, at night by the street lamps, nothing "
            "glowing: against none with SSR_SURFACE_REFLECTIVITY", occlusions[which].what);
        expect_same(what, kept, off);
        free(off);
        free(kept);
    }
    printf("KEEP_HBAO with contact AO and the SSAO: %u pixels by a level under the omni shadow in all\n", rounded);
}
#endif

/* Pixels below a box on the screen (its rectangle's columns, from its bottom row to the image's) where a and b differ. */
static uint32_t differing_below(const Scene *scene, uint32_t box_index, const uint8_t *a, const uint8_t *b) {
    int rectangle[4];
    box_rectangle(scene, box_index, rectangle);
    uint32_t count = 0u;
    for (int y = rectangle[3] + 1; y < (int)TARGET_SIZE; ++y) {
        for (int x = rectangle[0] < 0 ? 0 : rectangle[0]; x <= rectangle[2] && x < (int)TARGET_SIZE; ++x) {
            const uint32_t index = ((uint32_t)y * TARGET_SIZE + (uint32_t)x) * 4u;
            count += memcmp(&a[index], &b[index], 4u) != 0 ? 1u : 0u;
        }
    }
    return count;
}

/* SSR_FACING_HITS (0.10.0). The steep street: the floating slab's column (the ground under it reflected its top,
   where the rays passed beneath it) goes; the tall box, which stands on the ground, keeps its reflection. Then the
   street by day (printed). */
static void check_ssr_facing_hits(SmokeWindow *smoke) {
    const uint8_t reflectivity = BTBH_GPU_SCENE_POST_SSR_SURFACE_REFLECTIVITY;
    const uint8_t facing = BTBH_GPU_SCENE_POST_SSR_FACING_HITS;
    struct { const char *what; const char *name; Cascades cascades; uint8_t flags; } cases[3] = {
        { "no cascades, SSR_SURFACE_REFLECTIVITY", "steep-ssr", CASCADES_OFF, reflectivity },
        { "additive cascades", "steep-ssr-additive", CASCADES_ADDITIVE, 0u },
        { "cascades", "steep-ssr-rc", CASCADES_ON, 0u },
    };
    for (uint32_t index = 0u; index < 3u; ++index) {
        Options options = { .name = cases[index].name, .shadows = SHADOWS_SUN, .steep = true };
        uint8_t *none = render_with(smoke, options, cases[index].cascades, cases[index].flags, false);
        uint8_t *none_set = render_with(smoke, options, cases[index].cascades,
            (uint8_t)(cases[index].flags | facing), false);
        options.reflections = true;
        uint8_t *unset = render_with(smoke, options, cases[index].cascades, cases[index].flags, false);
        uint8_t *set = render_with(smoke, options, cases[index].cascades, (uint8_t)(cases[index].flags | facing),
            false);
        char what[200];
        (void)snprintf(what, sizeof(what), "SSR_FACING_HITS, %s, without the reflections: set against unset",
            cases[index].what);
        expect_same(what, none, none_set);
        (void)snprintf(what, sizeof(what), "SSR_FACING_HITS, %s: the reflections unset against none",
            cases[index].what);
        (void)expect_different(what, none, unset);
        (void)snprintf(what, sizeof(what), "SSR_FACING_HITS, %s: set against unset", cases[index].what);
        (void)expect_different(what, unset, set);
        print_darkening("  set against unset", unset, set);
        static Scene scene;
        build_scene(smoke, &options, &scene);
        /* The steep street's boxes: 2 the tall box, 3 the floating slab. */
        const uint32_t slab_unset = differing_below(&scene, 3u, none, unset);
        const uint32_t slab_set = differing_below(&scene, 3u, none, set);
        const uint32_t tall_unset = differing_below(&scene, 2u, none, unset);
        const uint32_t tall_set = differing_below(&scene, 2u, none, set);
        printf("  reflected below the floating slab: %u pixels unset, %u set; below the tall box: %u, %u\n", slab_unset,
            slab_set, tall_unset, tall_set);
        assert(slab_set * 3u < slab_unset);
        assert(tall_set * 10u >= tall_unset * 9u);
        char name[64];
        (void)snprintf(name, sizeof(name), "%s-none", cases[index].name);
        dump_image(name, none);
        (void)snprintf(name, sizeof(name), "%s-unset", cases[index].name);
        dump_image(name, unset);
        (void)snprintf(name, sizeof(name), "%s-facing", cases[index].name);
        dump_image(name, set);
        free(none);
        free(none_set);
        free(unset);
        free(set);
    }
    /* The street (the building, the glass pane, the metal boxes) by day, without the cascades: printed. */
    {
        const Options options = { .name = "ssr-facing-street", .shadows = SHADOWS_SUN, .reflections = true,
            .gold = true };
        uint8_t *none = render_with(smoke, options, CASCADES_OFF, 0u, false);
        uint8_t *unset = render_with(smoke, options, CASCADES_OFF, reflectivity, false);
        uint8_t *set = render_with(smoke, options, CASCADES_OFF, (uint8_t)(reflectivity | facing), false);
        int largest = 0;
        const uint32_t reflected = differing_pixels(none, unset, &largest);
        const uint32_t changed = differing_pixels(unset, set, &largest);
        printf("SSR_FACING_HITS, the street by day: %u pixels reflect; set against unset: %u differ (largest %d levels)\n",
            reflected, changed, largest);
        dump_image("ssr-facing-street-unset", unset);
        dump_image("ssr-facing-street-set", set);
        free(none);
        free(unset);
        free(set);
    }
}

/* Pixels where b lies outside [min(lo, hi), max(lo, hi)] by more than `slack` levels in a channel, among those where
   `mask_a` and `mask_b` differ. */
static uint32_t outside_between(const uint8_t *mask_a, const uint8_t *mask_b, const uint8_t *lo, const uint8_t *hi,
    const uint8_t *b, int slack, uint32_t *masked) {
    uint32_t outside = 0u;
    *masked = 0u;
    for (uint32_t index = 0u; index < TEXELS; ++index) {
        if (memcmp(&mask_a[index * 4u], &mask_b[index * 4u], 3u) == 0) {
            continue;
        }
        *masked += 1u;
        for (uint32_t channel = 0u; channel < 3u; ++channel) {
            const int l = lo[index * 4u + channel];
            const int h = hi[index * 4u + channel];
            const int v = b[index * 4u + channel];
            if (v < (l < h ? l : h) - slack || v > (l > h ? l : h) + slack) {
                outside += 1u;
                break;
            }
        }
    }
    return outside;
}

/* The spot occluder's flags and RECEIVES_NO_SPOT_SHADOW (0.10.0; Be The Bullet Hell v2's kaiju shadow). */
static void check_spot_occluder(SmokeWindow *smoke) {
    static Scene scene;
    const Options options = { .name = "kaiju", .shadows = SHADOWS_SUN, .kaiju = true };
    const uint8_t sun_only = BTBH_GPU_SCENE_SPOT_OCCLUDER_SUN_ONLY;
    const uint8_t orthographic = BTBH_GPU_SCENE_SPOT_OCCLUDER_ORTHOGRAPHIC;
    /* The box out of the sun's map: its map is its sun shadow. */
    build_scene(smoke, &options, &scene);
    scene.batches[scene.kaiju_box].flags = BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW;
    uint8_t *classic = render(smoke, &scene);
    scene.lighting.spot_occluder_flags = sun_only;
    uint8_t *sun = render(smoke, &scene);
    scene.lighting.spot_occluder_flags = 0u;
    scene.lighting.spot_occluder_shadow_strength = 0.0f;
    uint8_t *lit = render(smoke, &scene);
    scene.lighting.spot_occluder_flags = sun_only;
    uint8_t *lit_flagged = render(smoke, &scene);
    scene.lighting.spot_occluder_flags = 0u;
    scene.lighting.directional_energy = 0.0f;
    uint8_t *sunless = render(smoke, &scene);
    scene.lighting.spot_occluder_shadow_strength = 1.0f;
    scene.lighting.spot_occluder_flags = sun_only;
    uint8_t *sunless_sun_only = render(smoke, &scene);
    scene.lighting.spot_occluder_flags = 0u;
    uint8_t *sunless_classic = render(smoke, &scene);
    (void)expect_different("spot occluder: its darkening (the classic one) against none", lit, classic);
    expect_same("SPOT_OCCLUDER_SUN_ONLY with no darkening: set against unset", lit, lit_flagged);
    expect_same("SPOT_OCCLUDER_SUN_ONLY without a sun: the darkening takes nothing", sunless, sunless_sun_only);
    (void)expect_different("spot occluder without a sun: the classic darkening still takes the fill and the sky",
        sunless, sunless_classic);
    /* In the shadow, the sun-only darkening lies between the frame without the sun (the umbra) and the one without
       the darkening; the classic one is darker. */
    uint32_t masked = 0u;
    const uint32_t outside = outside_between(lit, classic, sunless, lit, sun, 1, &masked);
    print_darkening("SPOT_OCCLUDER_SUN_ONLY: the shadow, set against unset", classic, sun);
    printf("SPOT_OCCLUDER_SUN_ONLY: %u pixels in the shadow, %u outside [no sun, no darkening] by more than a level\n",
        masked, outside);
    assert(masked > 200u && outside == 0u);
    double classic_sum = 0.0;
    double sun_sum = 0.0;
    for (uint32_t index = 0u; index < TEXELS * 4u; ++index) {
        classic_sum += classic[index];
        sun_sum += sun[index];
    }
    assert(sun_sum > classic_sum);
    dump_image("kaiju-classic", classic);
    dump_image("kaiju-sun-only", sun);
    dump_image("kaiju-no-darkening", lit);
    dump_image("kaiju-sunless", sunless);
    /* Under cascades that aren't additive their light is darkened as before (the sun-only darkening still lightens the
       shadow: the scene's own fill and sky stay). */
    {
        Options rc = options;
        rc.cascades = CASCADES_ON;
        build_scene(smoke, &rc, &scene);
        scene.batches[scene.kaiju_box].flags = BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW;
        uint8_t *rc_classic = render(smoke, &scene);
        scene.lighting.spot_occluder_flags = sun_only;
        uint8_t *rc_sun = render(smoke, &scene);
        scene.lighting.spot_occluder_shadow_strength = 0.0f;
        uint8_t *rc_lit = render(smoke, &scene);
        (void)expect_different("SPOT_OCCLUDER_SUN_ONLY under the cascades: set against unset", rc_classic, rc_sun);
        (void)expect_different("SPOT_OCCLUDER_SUN_ONLY under the cascades: set against no darkening", rc_lit, rc_sun);
        print_darkening("  set against unset", rc_classic, rc_sun);
        print_darkening("  no darkening against set", rc_sun, rc_lit);
        free(rc_classic);
        free(rc_sun);
        free(rc_lit);
    }
    /* RECEIVES_NO_SPOT_SHADOW on the box: only its own pixels change (its own map no longer darkens it), lighter. */
    {
        build_scene(smoke, &options, &scene);
        uint8_t *own = render(smoke, &scene);
        scene.batches[scene.kaiju_box].flags = BTBH_GPU_SCENE_BATCH_RECEIVES_NO_SPOT_SHADOW;
        uint8_t *spared = render(smoke, &scene);
        int rectangle[4];
        box_rectangle(&scene, scene.kaiju_box, rectangle);
        uint32_t differing = 0u;
        uint32_t outside_box = 0u;
        uint32_t darker = 0u;
        for (uint32_t y = 0u; y < TARGET_SIZE; ++y) {
            for (uint32_t x = 0u; x < TARGET_SIZE; ++x) {
                const uint32_t index = (y * TARGET_SIZE + x) * 4u;
                if (memcmp(&own[index], &spared[index], 4u) == 0) {
                    continue;
                }
                differing += 1u;
                outside_box += (int)x < rectangle[0] || (int)x > rectangle[2] || (int)y < rectangle[1] ||
                    (int)y > rectangle[3] ? 1u : 0u;
                darker += spared[index] + spared[index + 1u] + spared[index + 2u] <
                    own[index] + own[index + 1u] + own[index + 2u] ? 1u : 0u;
            }
        }
        printf("RECEIVES_NO_SPOT_SHADOW on the box: %u pixels differ, %u outside its rectangle, %u darker\n",
            differing, outside_box, darker);
        assert(differing > 20u && outside_box == 0u && darker == 0u);
        dump_image("kaiju-own-shadow", own);
        dump_image("kaiju-receives-none", spared);
        free(own);
        free(spared);
    }
    /* SPOT_OCCLUDER_ORTHOGRAPHIC: the reach in world units from the map's near plane. The box's sunward face lies
       about 6.3 units from it (its far side 7.8). */
    {
        build_scene(smoke, &options, &scene);
        scene.lighting.spot_occluder_flags = orthographic;
        uint8_t *far_reach = render(smoke, &scene);
        scene.lighting.spot_occluder_shadow_reach = 80.0f;
        uint8_t *reach_30 = render(smoke, &scene);
        scene.lighting.spot_occluder_shadow_reach = 5.0f;
        uint8_t *reach_5 = render(smoke, &scene);
        scene.lighting.spot_occluder_shadow_reach = 12.0f;
        uint8_t *reach_12 = render(smoke, &scene);
        scene.lighting.spot_occluder_shadow_strength = 0.0f;
        uint8_t *none = render(smoke, &scene);
        scene.lighting.spot_occluder_shadow_strength = 1.0f;
        scene.lighting.spot_occluder_flags = 0u;
        scene.lighting.spot_occluder_shadow_reach = 5.0f;
        uint8_t *perspective_5 = render(smoke, &scene);
        scene.lighting.spot_occluder_shadow_reach = 10000.0f;
        uint8_t *perspective_far = render(smoke, &scene);
        expect_same("SPOT_OCCLUDER_ORTHOGRAPHIC: reach 80 (2.5 times the map's 29 units of depth) against 10,000",
            far_reach, reach_30);
        expect_same("SPOT_OCCLUDER_ORTHOGRAPHIC: reach 5 (short of the box) against no darkening", none, reach_5);
        (void)expect_different("SPOT_OCCLUDER_ORTHOGRAPHIC: reach 12 (easing over the box) against 80", reach_30,
            reach_12);
        (void)expect_different("SPOT_OCCLUDER_ORTHOGRAPHIC: reach 12 against no darkening", none, reach_12);
        /* Unset, the matrix's depth is read as perspective (the box some 0.07 units from the light): reach 5 eases
           nothing but the map's empty edges. */
        assert(expect_different("unset, an orthographic matrix read as perspective: reach 5 against no darkening",
            none, perspective_5) > 1000u);
        int largest = 0;
        printf("  reach 5 against 10,000: %u pixels differ\n", differing_pixels(perspective_5, perspective_far,
            &largest));
        free(far_reach);
        free(reach_30);
        free(reach_5);
        free(reach_12);
        free(none);
        free(perspective_5);
        free(perspective_far);
    }
    free(classic);
    free(sun);
    free(lit);
    free(lit_flagged);
    free(sunless);
    free(sunless_sun_only);
    free(sunless_classic);
}
#endif

#if defined(SMOKE_HAS_0110)
/* ------------------------------------------------------------------------------ set: the light factor (0.11.0) */

static SmokeWindow open_smoke_window(BTBHGPUDevice *device, const char *title);
static void close_smoke_window(SmokeWindow *smoke);

/* The blackout scene's light batches: Be The Bullet Hell v2's kit pieces that carry light, by role. */
enum {
    LIGHT_GLASS = 0,     /* glass, detail pattern 5, 0.6 of its panes lit at night */
    LIGHT_LENS,          /* a lamp lens (pattern 6) on the building's front: v2's signs */
    LIGHT_GLOW,          /* a lit box with an emission texture */
    LIGHT_CARD,          /* a see-through unlit card with an emission: the fast unlit shader where it applies */
    LIGHT_SKINNED,       /* the skinned quad with an emission */
    LIGHT_FAR_GLOW,      /* an emissive box outside the view: the radiance cascades' source field only */
    LIGHT_SOURCE,        /* an unlit box without an emission: its colour is a source of the cascades, and stays one */
    LIGHT_FAR_SOURCE,    /* an unlit box with an emission outside the view: the emission goes, the colour stays */
    LIGHT_COUNT
};
#define LIGHT_BIT(role) (1u << (role))
#define LIGHT_ALL (LIGHT_BIT(LIGHT_COUNT) - 1u)

static const char *const light_names[LIGHT_COUNT] = {
    "glass", "lamp lens", "glow", "unlit card", "skinned quad", "glow outside the view", "unlit source",
    "unlit glow outside the view",
};

typedef struct LightConfig {
    const char *name;
    Shadows shadows;
    Cascades cascades;
    Occlusion occlusion;
    uint8_t post_flags;
} LightConfig;

static const LightConfig light_configs[] = {
    { .name = "sun", .shadows = SHADOWS_SUN },
    { .name = "unshadowed", .shadows = SHADOWS_NONE },
    { .name = "rc-sun", .shadows = SHADOWS_SUN, .cascades = CASCADES_ON },
    { .name = "additive-omni-hbao", .shadows = SHADOWS_OMNI, .cascades = CASCADES_ADDITIVE,
      .occlusion = OCCLUSION_HBAO },
    { .name = "additive-keep-hbao", .shadows = SHADOWS_SUN, .cascades = CASCADES_ADDITIVE,
      .occlusion = OCCLUSION_HBAO, .post_flags = BTBH_GPU_SCENE_POST_ADDITIVE_CASCADES_KEEP_HBAO },
};
#define LIGHT_CONFIG_COUNT ((uint32_t)(sizeof(light_configs) / sizeof(light_configs[0])))

/* The street at night with its windows lit (window_glow 2) and the light batches; light[role] is each one's box. */
static void build_blackout_scene(const SmokeWindow *smoke, const LightConfig *config, Scene *scene,
    uint32_t light[LIGHT_COUNT]) {
    const Options options = { .name = config->name, .shadows = config->shadows, .cascades = config->cascades,
        .occlusion = config->occlusion, .glow = true, .far_glow = true };
    build_scene(smoke, &options, scene);
    scene->post.flags = config->post_flags;
    scene->lighting.ambient_energy = 0.15f;
    scene->lighting.directional_energy = 0.25f;
    scene->lighting.window_glow = 2.0f;
    light[LIGHT_GLASS] = MAX_BOXES;
    for (uint32_t index = 0u; index < scene->count; ++index) {
        if (scene->batches[index].detail_parameters[3] == 5.0f) {
            light[LIGHT_GLASS] = index;
        }
    }
    assert(light[LIGHT_GLASS] != MAX_BOXES);
    scene->batches[light[LIGHT_GLASS]].detail_parameters[1] = 0.6f;
    BTBHGPUSceneGeometryBatch batch = cube_batch(smoke->cube);
    batch.detail_parameters[2] = 1.0f;   /* (patterns 1..6 take a scale) */
    batch.detail_parameters[3] = 6.0f;
    light[LIGHT_LENS] = scene_add(scene, make_box(-2.2f, 1.3f, -0.85f, 1.0f, 0.45f, 0.1f, hot), batch);
    light[LIGHT_GLOW] = scene->glow_box;
    batch = cube_batch(smoke->cube);
    batch.unlit_enabled = 1u;
    batch.transparency_enabled = 1u;
    batch.albedo_alpha = 200u;
    batch.emission_texture = smoke->glow;
    batch.emission_red = 255u;
    batch.emission_green = 255u;
    batch.emission_blue = 255u;
    batch.emission_energy = 1.5f;
    batch.spot_shadow_visible = 0u;
    batch.flags = BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW;
    /* (Lying on the street, as v2's washes do: the cascades' sources are drawn from above.) */
    light[LIGHT_CARD] = scene_add(scene, make_box(1.0f, 0.04f, 0.8f, 0.9f, 0.02f, 0.9f, slate), batch);
    batch = cube_batch(smoke->cube);
    batch.kind = BTBH_GPU_SCENE_GEOMETRY_MODEL;
    batch.custom_mesh = NULL;
    batch.model = smoke->skinned;
    batch.animation_library = smoke->animation;
    batch.animation_time = 0.25f;
    batch.cull_mode = BTBH_GPU_SCENE_CULL_NONE;
    batch.emission_red = 120u;
    batch.emission_green = 220u;
    batch.emission_blue = 255u;
    batch.emission_energy = 3.0f;
    light[LIGHT_SKINNED] = scene_add(scene, make_box(-0.4f, 0.03f, 2.0f, 0.8f, 0.8f, 0.8f, warm), batch);
    /* The quad (x -0.5..0.5, y 0..1, facing +z) laid flat, facing up: (x, y, z) to (x, z, -y), so the cascades'
       source pass, which projects straight down, sees its face. */
    float *quad = scene->instances[light[LIGHT_SKINNED]];
    quad[5] = 0.0f;
    quad[6] = -0.8f;
    quad[9] = 0.8f;
    quad[10] = 0.0f;
    light[LIGHT_FAR_GLOW] = scene->far_glow_box;
    light[LIGHT_SOURCE] = scene->source_box;
    light[LIGHT_FAR_SOURCE] = scene->far_source_box;
    BTBHGPUSceneGeometryBatch *far_source = &scene->batches[light[LIGHT_FAR_SOURCE]];
    far_source->emission_red = 255u;
    far_source->emission_green = 120u;
    far_source->emission_blue = 60u;
    far_source->emission_energy = 6.0f;
}

/* Custom x of the chosen light batches' instances, and (flag) the flag on them. */
static void set_light_factor(Scene *scene, const uint32_t light[LIGHT_COUNT], uint32_t which, float factor, bool flag) {
    for (uint32_t role = 0u; role < LIGHT_COUNT; ++role) {
        if ((which & LIGHT_BIT(role)) != 0u) {
            if (flag) {
                scene->batches[light[role]].flags |= BTBH_GPU_SCENE_BATCH_INSTANCE_LIGHT_FACTOR;
            }
            scene->instances[light[role]][20] = factor;
        }
    }
}

/* The chosen light batches as a host scales them without the flag: the emission's energy and the glass's share of lit
   panes times scale; at 0 the lamp lens loses its pattern (v2's Dark batches). The lens's glow at another scale is the
   window light's (the caller scales window_glow). */
static void scale_light(Scene *scene, const uint32_t light[LIGHT_COUNT], uint32_t which, float scale) {
    for (uint32_t role = 0u; role < LIGHT_COUNT; ++role) {
        if ((which & LIGHT_BIT(role)) == 0u) {
            continue;
        }
        BTBHGPUSceneGeometryBatch *batch = &scene->batches[light[role]];
        batch->emission_energy *= scale;
        if (role == LIGHT_GLASS) {
            batch->detail_parameters[1] *= scale;
        }
        if (role == LIGHT_LENS && scale == 0.0f) {
            batch->detail_parameters[3] = 0.0f;
        }
    }
}

/* The blackout scene with the `flagged` light batches at `factor`, the `scaled` ones scaled. */
static uint8_t *render_blackout(SmokeWindow *smoke, const LightConfig *config, uint32_t flagged, float factor,
    uint32_t scaled, float scale) {
    static Scene scene;
    uint32_t light[LIGHT_COUNT];
    build_blackout_scene(smoke, config, &scene, light);
    set_light_factor(&scene, light, flagged, factor, true);
    scale_light(&scene, light, scaled, scale);
    return render(smoke, &scene);
}

/* Every light batch at 1, 0 and 0.5 against the batches scaled as v2 does without the flag. */
static void check_light_factor_config(SmokeWindow *smoke, const LightConfig *config) {
    static Scene scene;
    uint32_t light[LIGHT_COUNT];
    char what[200];
    /* Unflagged, with 0.25 in custom x: only the flag reads it. */
    build_blackout_scene(smoke, config, &scene, light);
    set_light_factor(&scene, light, LIGHT_ALL, 0.25f, false);
    uint8_t *unflagged = render(smoke, &scene);
    /* At 1, and above (clamped to 1). */
    build_blackout_scene(smoke, config, &scene, light);
    set_light_factor(&scene, light, LIGHT_ALL & 0x55u, 1.0f, true);
    set_light_factor(&scene, light, LIGHT_ALL & 0xaau, 3.0f, true);
    uint8_t *powered = render(smoke, &scene);
    (void)snprintf(what, sizeof(what), "LIGHT_FACTOR, %s: every light batch at 1 (or 3) against unflagged",
        config->name);
    expect_same(what, powered, unflagged);
    /* At 0, below it and NaN (0). */
    build_blackout_scene(smoke, config, &scene, light);
    set_light_factor(&scene, light, LIGHT_ALL & 0x49u, 0.0f, true);
    set_light_factor(&scene, light, LIGHT_ALL & 0x92u, -1.5f, true);
    set_light_factor(&scene, light, LIGHT_ALL & 0x24u, NAN, true);
    uint8_t *dark = render(smoke, &scene);
    uint8_t *old_dark = render_blackout(smoke, config, 0u, 0.0f, LIGHT_ALL, 0.0f);
    (void)snprintf(what, sizeof(what),
        "LIGHT_FACTOR, %s: every light batch at 0 (or -1.5, or NaN) against v2's Dark batches", config->name);
    expect_same(what, dark, old_dark);
    (void)snprintf(what, sizeof(what), "LIGHT_FACTOR, %s: unflagged against v2's Dark batches", config->name);
    (void)expect_different(what, unflagged, old_dark);
    /* At 0.5: half the emissions' energy and half the glass's share (the lens apart). */
    const uint32_t but_lens = LIGHT_ALL & ~LIGHT_BIT(LIGHT_LENS);
    uint8_t *half = render_blackout(smoke, config, but_lens, 0.5f, 0u, 1.0f);
    uint8_t *old_half = render_blackout(smoke, config, 0u, 0.0f, but_lens, 0.5f);
    (void)snprintf(what, sizeof(what),
        "LIGHT_FACTOR, %s: the light batches but the lens at 0.5 against half the energy and half the share",
        config->name);
    expect_same(what, half, old_half);
    (void)snprintf(what, sizeof(what), "LIGHT_FACTOR, %s: at 0.5 against 1", config->name);
    (void)expect_different(what, half, unflagged);
    (void)snprintf(what, sizeof(what), "LIGHT_FACTOR, %s: at 0.5 against 0", config->name);
    (void)expect_different(what, half, dark);
    /* The lens at 0.5: half the window light (the glass without lit panes in both). */
    build_blackout_scene(smoke, config, &scene, light);
    scene.batches[light[LIGHT_GLASS]].detail_parameters[1] = 0.0f;
    set_light_factor(&scene, light, LIGHT_BIT(LIGHT_LENS), 0.5f, true);
    uint8_t *half_lens = render(smoke, &scene);
    build_blackout_scene(smoke, config, &scene, light);
    scene.batches[light[LIGHT_GLASS]].detail_parameters[1] = 0.0f;
    scene.lighting.window_glow *= 0.5f;
    uint8_t *old_half_lens = render(smoke, &scene);
    (void)snprintf(what, sizeof(what), "LIGHT_FACTOR, %s: the lens at 0.5 against half the window light",
        config->name);
    expect_same(what, half_lens, old_half_lens);
    char name[96];
    (void)snprintf(name, sizeof(name), "light-%s-1", config->name);
    dump_image(name, unflagged);
    (void)snprintf(name, sizeof(name), "light-%s-0.5", config->name);
    dump_image(name, half);
    (void)snprintf(name, sizeof(name), "light-%s-0", config->name);
    dump_image(name, dark);
    free(unflagged);
    free(powered);
    free(dark);
    free(old_dark);
    free(half);
    free(old_half);
    free(half_lens);
    free(old_half_lens);
}

/* Each light batch alone: at 0 its own Dark look (and unlike unflagged, but for the unlit source without an emission,
   whose colour stays); at 0.5 half its energy or share. */
static void check_light_factor_alone(SmokeWindow *smoke, const LightConfig *config, uint32_t roles) {
    uint8_t *unflagged = render_blackout(smoke, config, 0u, 0.0f, 0u, 1.0f);
    for (uint32_t role = 0u; role < LIGHT_COUNT; ++role) {
        if ((roles & LIGHT_BIT(role)) == 0u) {
            continue;
        }
        char what[200];
        uint8_t *dark = render_blackout(smoke, config, LIGHT_BIT(role), 0.0f, 0u, 1.0f);
        uint8_t *old_dark = render_blackout(smoke, config, 0u, 0.0f, LIGHT_BIT(role), 0.0f);
        (void)snprintf(what, sizeof(what), "LIGHT_FACTOR, %s: the %s alone at 0 against its Dark look", config->name,
            light_names[role]);
        expect_same(what, dark, old_dark);
        (void)snprintf(what, sizeof(what), "LIGHT_FACTOR, %s: the %s alone at 0 against unflagged", config->name,
            light_names[role]);
        if (role == LIGHT_SOURCE) {
            expect_same(what, dark, unflagged);
        } else {
            (void)expect_different(what, dark, unflagged);
        }
        if (role != LIGHT_LENS) {
            uint8_t *half = render_blackout(smoke, config, LIGHT_BIT(role), 0.5f, 0u, 1.0f);
            uint8_t *old_half = render_blackout(smoke, config, 0u, 0.0f, LIGHT_BIT(role), 0.5f);
            (void)snprintf(what, sizeof(what), "LIGHT_FACTOR, %s: the %s alone at 0.5 against half", config->name,
                light_names[role]);
            expect_same(what, half, old_half);
            free(half);
            free(old_half);
        }
        free(dark);
        free(old_dark);
    }
    free(unflagged);
}

static void check_light_factor_refusals(SmokeWindow *smoke) {
    static Scene scene;
    uint32_t light[LIGHT_COUNT];
    BTBHGPUSceneReport report;
    const LightConfig *config = &light_configs[0];
    BTBHGPUSceneWorldDetail detail;
    memset(&detail, 0, sizeof(detail));
    detail.level_tile[0] = detail.level_tile[1] = detail.wall_tile[0] = detail.wall_tile[1] = 1.0f;
    BTBHGPUSceneWorldDetailTableEntry entry;
    memset(&entry, 0, sizeof(entry));
    entry.level_tile[0] = entry.level_tile[1] = entry.wall_tile[0] = entry.wall_tile[1] = 1.0f;
    BTBHGPUSceneWorldDetailTable table;
    memset(&table, 0, sizeof(table));
    table.entries = &entry;
    table.entry_count = 1u;
    uint32_t refused = 0u;
    for (int which = 0; which < 9; ++which) {
        build_blackout_scene(smoke, config, &scene, light);
        const uint32_t index = light[LIGHT_GLOW];
        BTBHGPUSceneGeometryBatch *batch = &scene.batches[index];
        set_light_factor(&scene, light, LIGHT_BIT(LIGHT_GLOW), 0.5f, true);
        const char *needle = NULL;
        switch (which) {
            case 0:
                batch->flags |= BTBH_GPU_SCENE_BATCH_INSTANCE_UV_RECT;
                needle = "INSTANCE_UV_RECT";
                break;
            case 1:
                batch->surface_variant = BTBH_GPU_SCENE_SURFACE_DISSOLVE;
                needle = "custom x";
                break;
            case 2:
                batch->surface_variant = BTBH_GPU_SCENE_SURFACE_GLITTER;
                needle = "custom x";
                break;
            case 3:
                /* (A lit particle the lit particle's own rules take.) */
                batch->surface_variant = BTBH_GPU_SCENE_SURFACE_LIT_PARTICLE;
                batch->transparency_enabled = 1u;
                batch->blend_mode = BTBH_GPU_SCENE_BLEND_ALPHA;
                needle = "lit particle";
                break;
            case 4:
                batch->surface_variant = BTBH_GPU_SCENE_SURFACE_REFRACT;
                batch->transparency_enabled = 1u;
                batch->blend_mode = BTBH_GPU_SCENE_BLEND_ALPHA;
                needle = "refracting";
                break;
            case 5:
                detail.seed_component = 1u;
                batch->world_detail = &detail;
                needle = "world_detail's seed_component 1";
                break;
            case 6:
                table.seed_component = 1u;
                batch->world_detail_table = &table;
                needle = "world_detail_table's seed_component 1";
                break;
            case 7:
                /* Taken: a detail seeded by custom w (v2's), a table by y, and SHATTER (y, z, w). */
                detail.seed_component = 4u;
                batch->world_detail = &detail;
                table.seed_component = 2u;
                scene.batches[light[LIGHT_LENS]].world_detail_table = &table;
                set_light_factor(&scene, light, LIGHT_BIT(LIGHT_LENS), 0.5f, true);
                scene.batches[light[LIGHT_CARD]].surface_variant = BTBH_GPU_SCENE_SURFACE_SHATTER;
                set_light_factor(&scene, light, LIGHT_BIT(LIGHT_CARD), 0.5f, true);
                break;
            default:
                /* Prewarm refuses too. */
                batch->flags |= BTBH_GPU_SCENE_BATCH_INSTANCE_UV_RECT;
                needle = "INSTANCE_UV_RECT";
                break;
        }
        BTBHGPUResult result;
        memset(&report, 0, sizeof(report));
        if (which == 8) {
            result = btbh_gpu_window_scene_prewarm_render_to_target(smoke->window, smoke->target,
                shader_paths[0], shader_paths[1], shader_paths[2], shader_paths[3],
                shader_paths[4], shader_paths[5], shader_paths[6], shader_paths[7],
                shader_paths[8], shader_paths[9], shader_paths[10],
                SMOKE_SHADER_FORMAT, TARGET_SIZE, TARGET_SIZE, &scene.lighting, &scene.post, scene.batches, scene.count,
                &report);
        } else {
            result = btbh_gpu_window_scene_render_to_target(smoke->window, smoke->target,
                shader_paths[0], shader_paths[1], shader_paths[2], shader_paths[3],
                shader_paths[4], shader_paths[5], shader_paths[6], shader_paths[7],
                shader_paths[8], shader_paths[9], shader_paths[10],
                SMOKE_SHADER_FORMAT, TARGET_SIZE, TARGET_SIZE,
                scene.camera.view_projection, scene.camera.right, scene.camera.up, scene.camera.position,
                &scene.lighting, &scene.post, &scene.instances[0][0], scene.count, scene.count,
                scene.batches, NULL, NULL, scene.count, 12u, 14u, 20u, 255u, &report);
        }
        if (needle == NULL) {
            if (result != BTBH_GPU_OK) {
                fprintf(stderr, "light factor, taken: %s\n", report.message);
            }
            assert(result == BTBH_GPU_OK);
            continue;
        }
        char prefix[96];
        (void)snprintf(prefix, sizeof(prefix), "GPU scene batch %u: flags INSTANCE_LIGHT_FACTOR: ", index);
        const bool named = strstr(report.message, prefix) != NULL && strstr(report.message, needle) != NULL;
        if (result != BTBH_GPU_INVALID_ARGUMENT || !named) {
            fprintf(stderr, "light factor refusal %d: %d \"%s\" (expected \"%s...%s\")\n", which, (int)result,
                report.message, prefix, needle);
        }
        assert(result == BTBH_GPU_INVALID_ARGUMENT && named);
        refused += 1u;
        if (which == 0) {
            printf("LIGHT_FACTOR refusal: %s\n", report.message);
        }
    }
    printf("LIGHT_FACTOR: %u refusals naming the batch (present and prewarm); a detail seeded by w, a table by y and "
           "SHATTER taken\n", refused);
}

/* Prewarming the unflagged scenes in a fresh window makes every pipeline and shader the flagged ones draw with. */
static void check_light_factor_prewarm(BTBHGPUDevice *device) {
    SmokeWindow fresh = open_smoke_window(device, "BTBH scene flags smoke (light factor prewarm)");
    static Scene scene;
    uint32_t light[LIGHT_COUNT];
    BTBHGPUCreationStats before;
    btbh_gpu_creation_stats(&before);
    for (uint32_t index = 0u; index < LIGHT_CONFIG_COUNT; ++index) {
        build_blackout_scene(&fresh, &light_configs[index], &scene, light);
        BTBHGPUSceneReport report;
        memset(&report, 0, sizeof(report));
        const BTBHGPUResult result = btbh_gpu_window_scene_prewarm_render_to_target(fresh.window, fresh.target,
            shader_paths[0], shader_paths[1], shader_paths[2], shader_paths[3],
            shader_paths[4], shader_paths[5], shader_paths[6], shader_paths[7],
            shader_paths[8], shader_paths[9], shader_paths[10],
            SMOKE_SHADER_FORMAT, TARGET_SIZE, TARGET_SIZE, &scene.lighting, &scene.post, scene.batches, scene.count,
            &report);
        if (result != BTBH_GPU_OK) {
            fprintf(stderr, "light factor prewarm %s: %s\n", light_configs[index].name, report.message);
        }
        assert(result == BTBH_GPU_OK);
    }
    BTBHGPUCreationStats prewarmed;
    btbh_gpu_creation_stats(&prewarmed);
    for (uint32_t index = 0u; index < LIGHT_CONFIG_COUNT; ++index) {
        free(render_blackout(&fresh, &light_configs[index], LIGHT_ALL, 0.5f, 0u, 1.0f));
    }
    BTBHGPUCreationStats rendered;
    btbh_gpu_creation_stats(&rendered);
    printf("LIGHT_FACTOR prewarm: %llu pipelines and %llu shaders made by prewarming %u unflagged scenes in a fresh "
           "window; their flagged renders then made %llu pipelines and %llu shaders\n",
        (unsigned long long)(prewarmed.graphics_pipelines - before.graphics_pipelines),
        (unsigned long long)(prewarmed.shaders - before.shaders), LIGHT_CONFIG_COUNT,
        (unsigned long long)(rendered.graphics_pipelines - prewarmed.graphics_pipelines),
        (unsigned long long)(rendered.shaders - prewarmed.shaders));
    assert(prewarmed.graphics_pipelines > before.graphics_pipelines);
    assert(rendered.graphics_pipelines == prewarmed.graphics_pipelines);
    assert(rendered.shaders == prewarmed.shaders);
    close_smoke_window(&fresh);
}

static void check_light_factor(SmokeWindow *smoke, BTBHGPUDevice *device) {
    for (uint32_t index = 0u; index < LIGHT_CONFIG_COUNT; ++index) {
        check_light_factor_config(smoke, &light_configs[index]);
    }
    /* Each alone: without the cascades every one on screen (the card through the fast unlit shader); under the
       cascades and additive ones the emissive ones, the sources and the glow outside the view. */
    check_light_factor_alone(smoke, &light_configs[0], LIGHT_BIT(LIGHT_GLASS) | LIGHT_BIT(LIGHT_LENS) |
        LIGHT_BIT(LIGHT_GLOW) | LIGHT_BIT(LIGHT_CARD) | LIGHT_BIT(LIGHT_SKINNED));
    for (uint32_t index = 2u; index < 4u; ++index) {
        check_light_factor_alone(smoke, &light_configs[index], LIGHT_BIT(LIGHT_GLOW) | LIGHT_BIT(LIGHT_CARD) |
            LIGHT_BIT(LIGHT_SKINNED) | LIGHT_BIT(LIGHT_FAR_GLOW) | LIGHT_BIT(LIGHT_SOURCE) |
            LIGHT_BIT(LIGHT_FAR_SOURCE));
    }
    check_light_factor_refusals(smoke);
    check_light_factor_prewarm(device);
}
#endif

#if defined(SMOKE_HAS_0120)
/* What carries BTBH_GPU_SCENE_BATCH_FLAGS2_KEEP_CASCADES_LOOK in render_look. */
enum { LOOK_BEAM = 1u, LOOK_UNLIT_BOX = 2u, LOOK_EVERY = 4u };

/* Renders `options` as render_with does (the unlit box no source), with the bit on what `look` names. */
static uint8_t *render_look(SmokeWindow *smoke, Options options, Cascades cascades, uint8_t post_flags, uint32_t look) {
    static Scene scene;
    options.cascades = cascades;
    build_scene(smoke, &options, &scene);
    scene.post.flags = post_flags;
    if (scene.unlit_box != MAX_BOXES) {
        scene.batches[scene.unlit_box].flags = BTBH_GPU_SCENE_BATCH_NO_RC_SOURCE;
    }
    for (uint32_t index = 0u; index < scene.count; ++index) {
        if ((look & LOOK_EVERY) != 0u || ((look & LOOK_BEAM) != 0u && index == scene.beam_box) ||
            ((look & LOOK_UNLIT_BOX) != 0u && index == scene.unlit_box)) {
            scene.batches[index].flags2 = BTBH_GPU_SCENE_BATCH_FLAGS2_KEEP_CASCADES_LOOK;
        }
    }
    return render(smoke, &scene);
}

/* Pixels where a and b differ outside the box's rectangle, and inside it. */
static void differing_around(const Scene *scene, uint32_t box_index, const uint8_t *a, const uint8_t *b,
    uint32_t *inside, uint32_t *outside) {
    int rectangle[4];
    box_rectangle(scene, box_index, rectangle);
    *inside = 0u;
    *outside = 0u;
    for (uint32_t y = 0u; y < TARGET_SIZE; ++y) {
        for (uint32_t x = 0u; x < TARGET_SIZE; ++x) {
            const uint32_t at = (y * TARGET_SIZE + x) * 4u;
            if (memcmp(&a[at], &b[at], 4u) != 0) {
                const bool in = (int)x >= rectangle[0] && (int)x <= rectangle[2] && (int)y >= rectangle[1] &&
                    (int)y <= rectangle[3];
                *inside += in ? 1u : 0u;
                *outside += in ? 0u : 1u;
            }
        }
    }
}

/* 0.12.0: BTBH_GPU_SCENE_BATCH_FLAGS2_KEEP_CASCADES_LOOK (Be The Bullet Hell v2's weapon beams and glows keep the
   cascades' point-light boost under KEEP_HBAO, while its light overlays draw as without the cascades). A beam (a long
   thin additive unlit box) over the street by the neon, nothing glowing, additive cascades:
   - no AO (where KEEP_HBAO's transparent part alone applies), no shadow and the sun's: with KEEP_HBAO the beam draws as
     without the cascades (the fast unlit shader), the frame without them; with the bit on the beam too, as under the
     cascades without KEEP_HBAO, that frame, byte for byte; and the two differ;
   - with contact AO and street lamps, and with the HBAO, under KEEP_HBAO: the bit changes the beam's pixels alone;
   - the bit on every batch changes nothing without the cascades, under cascades that aren't additive (KEEP_HBAO set),
     under additive ones without KEEP_HBAO, nor in a cascades' debug view; on an opaque unlit box (contact AO,
     KEEP_HBAO) and on the beam drawn lit it changes nothing either;
   - prewarming the flagged scenes in a fresh window leaves their renders no pipeline or shader to make. */
static void check_keep_cascades_look(SmokeWindow *smoke, BTBHGPUDevice *device) {
    const uint8_t keep = BTBH_GPU_SCENE_POST_ADDITIVE_CASCADES_KEEP_HBAO;
    static const char *const shadow_names[2] = { "no shadow", "the sun's shadow" };
    static Scene scene;
    for (uint32_t shadows = 0u; shadows < 2u; ++shadows) {
        const Options options = { .name = "look-beam", .shadows = (Shadows)shadows, .neon = true, .beam = true };
        uint8_t *off = render_look(smoke, options, CASCADES_OFF, 0u, 0u);
        uint8_t *additive = render_look(smoke, options, CASCADES_ADDITIVE, 0u, 0u);
        uint8_t *kept = render_look(smoke, options, CASCADES_ADDITIVE, keep, 0u);
        uint8_t *looked = render_look(smoke, options, CASCADES_ADDITIVE, keep, LOOK_BEAM);
        char what[240];
        (void)snprintf(what, sizeof(what), "KEEP_CASCADES_LOOK, no AO, %s, a beam by the neon: KEEP_HBAO without the "
            "bit against no cascades", shadow_names[shadows]);
        expect_same(what, kept, off);
        (void)snprintf(what, sizeof(what), "KEEP_CASCADES_LOOK, no AO, %s, a beam by the neon: KEEP_HBAO with the bit "
            "against additive cascades without KEEP_HBAO", shadow_names[shadows]);
        expect_same(what, looked, additive);
        (void)snprintf(what, sizeof(what), "KEEP_CASCADES_LOOK, no AO, %s, a beam by the neon: KEEP_HBAO with the bit "
            "against without it", shadow_names[shadows]);
        (void)expect_different(what, looked, kept);
        if (shadows == 1u) {
            dump_image("look-beam-off", off);
            dump_image("look-beam-kept", kept);
            dump_image("look-beam-looked", looked);
        }
        free(off);
        free(additive);
        free(kept);
        free(looked);
    }
    /* With contact AO (and the street lamps), and with the HBAO: the beam's pixels alone. */
    {
        static const struct {
            const char *what;
            Occlusion occlusion;
        } cases[2] = { { "contact AO", OCCLUSION_HBAO_CONTACT }, { "the HBAO", OCCLUSION_HBAO } };
        for (uint32_t index = 0u; index < 2u; ++index) {
            const Options options = { .name = "look-beam-ao", .shadows = SHADOWS_SUN,
                .occlusion = cases[index].occlusion, .neon = true, .lamps = true, .beam = true, .unlit_box = true };
            uint8_t *kept = render_look(smoke, options, CASCADES_ADDITIVE, keep, 0u);
            uint8_t *looked = render_look(smoke, options, CASCADES_ADDITIVE, keep, LOOK_BEAM);
            build_scene(smoke, &options, &scene);
            uint32_t inside = 0u;
            uint32_t outside = 0u;
            differing_around(&scene, scene.beam_box, kept, looked, &inside, &outside);
            printf("KEEP_CASCADES_LOOK, %s, the sun's shadow, a beam by the neon and the lamps: KEEP_HBAO with the "
                   "bit against without it: %u pixels differ on the beam's rectangle, %u outside it\n",
                cases[index].what, inside, outside);
            assert(inside > 0u && outside == 0u);
            free(kept);
            free(looked);
        }
    }
    /* Where it changes nothing. */
    {
        struct {
            const char *what;
            Options options;
            Cascades cascades;
            uint8_t post_flags;
            uint32_t look;
        } cases[6] = {
            { "no cascades", { .name = "look-off", .shadows = SHADOWS_SUN, .neon = true, .beam = true,
                .occlusion = OCCLUSION_HBAO_CONTACT }, CASCADES_OFF, 0u, LOOK_EVERY },
            { "cascades not additive, KEEP_HBAO", { .name = "look-rc", .shadows = SHADOWS_SUN, .neon = true,
                .beam = true }, CASCADES_ON, keep, LOOK_EVERY },
            { "additive cascades without KEEP_HBAO", { .name = "look-additive", .shadows = SHADOWS_SUN, .neon = true,
                .beam = true, .occlusion = OCCLUSION_HBAO_CONTACT }, CASCADES_ADDITIVE, 0u, LOOK_EVERY },
            { "a debug view, KEEP_HBAO", { .name = "look-debug", .shadows = SHADOWS_SUN, .neon = true, .beam = true,
                .debug_view = BTBH_GPU_SCENE_RC_DEBUG_FINAL_INDIRECT }, CASCADES_ADDITIVE, keep, LOOK_EVERY },
            { "an opaque unlit box, contact AO, KEEP_HBAO", { .name = "look-unlit", .shadows = SHADOWS_SUN,
                .neon = true, .lamps = true, .unlit_box = true, .occlusion = OCCLUSION_HBAO_CONTACT },
                CASCADES_ADDITIVE, keep, LOOK_UNLIT_BOX },
            { "every batch of the street without a beam, the HBAO, KEEP_HBAO", { .name = "look-others",
                .shadows = SHADOWS_SUN, .neon = true, .lamps = true, .unlit_box = true, .gold = true,
                .occlusion = OCCLUSION_HBAO }, CASCADES_ADDITIVE, keep, LOOK_EVERY },
        };
        for (uint32_t index = 0u; index < 6u; ++index) {
            uint8_t *without = render_look(smoke, cases[index].options, cases[index].cascades,
                cases[index].post_flags, 0u);
            uint8_t *with = render_look(smoke, cases[index].options, cases[index].cascades, cases[index].post_flags,
                cases[index].look);
            char what[200];
            (void)snprintf(what, sizeof(what), "KEEP_CASCADES_LOOK changes nothing: %s", cases[index].what);
            expect_same(what, without, with);
            free(without);
            free(with);
        }
        /* The beam drawn lit (transparent, not unlit): no fast unlit shader to leave. */
        const Options options = { .name = "look-lit-beam", .shadows = SHADOWS_SUN, .cascades = CASCADES_ADDITIVE,
            .neon = true, .beam = true };
        build_scene(smoke, &options, &scene);
        scene.post.flags = keep;
        scene.batches[scene.beam_box].unlit_enabled = 0u;
        uint8_t *lit = render(smoke, &scene);
        scene.batches[scene.beam_box].flags2 = BTBH_GPU_SCENE_BATCH_FLAGS2_KEEP_CASCADES_LOOK;
        uint8_t *lit_looked = render(smoke, &scene);
        expect_same("KEEP_CASCADES_LOOK changes nothing: the beam drawn lit, KEEP_HBAO", lit, lit_looked);
        free(lit);
        free(lit_looked);
    }
    /* Prewarm: the flagged scenes' pipelines are made by prewarming them. */
    {
        SmokeWindow fresh = open_smoke_window(device, "BTBH scene flags smoke (keep cascades look prewarm)");
        const Options prewarmed_options[3] = {
            { .name = "look-prewarm-none", .shadows = SHADOWS_NONE, .cascades = CASCADES_ADDITIVE, .neon = true,
                .beam = true },
            { .name = "look-prewarm-sun", .shadows = SHADOWS_SUN, .cascades = CASCADES_ADDITIVE, .neon = true,
                .beam = true },
            { .name = "look-prewarm-contact", .shadows = SHADOWS_SUN, .cascades = CASCADES_ADDITIVE, .neon = true,
                .lamps = true, .beam = true, .unlit_box = true, .occlusion = OCCLUSION_HBAO_CONTACT },
        };
        BTBHGPUCreationStats before;
        btbh_gpu_creation_stats(&before);
        for (uint32_t index = 0u; index < 3u; ++index) {
            build_scene(&fresh, &prewarmed_options[index], &scene);
            scene.post.flags = keep;
            scene.batches[scene.beam_box].flags2 = BTBH_GPU_SCENE_BATCH_FLAGS2_KEEP_CASCADES_LOOK;
            if (scene.unlit_box != MAX_BOXES) {
                scene.batches[scene.unlit_box].flags = BTBH_GPU_SCENE_BATCH_NO_RC_SOURCE;
            }
            BTBHGPUSceneReport report;
            memset(&report, 0, sizeof(report));
            const BTBHGPUResult result = btbh_gpu_window_scene_prewarm_render_to_target(fresh.window, fresh.target,
                shader_paths[0], shader_paths[1], shader_paths[2], shader_paths[3],
                shader_paths[4], shader_paths[5], shader_paths[6], shader_paths[7],
                shader_paths[8], shader_paths[9], shader_paths[10],
                SMOKE_SHADER_FORMAT, TARGET_SIZE, TARGET_SIZE, &scene.lighting, &scene.post, scene.batches,
                scene.count, &report);
            if (result != BTBH_GPU_OK) {
                fprintf(stderr, "keep cascades look prewarm %s: %s\n", prewarmed_options[index].name, report.message);
            }
            assert(result == BTBH_GPU_OK);
        }
        BTBHGPUCreationStats prewarmed;
        btbh_gpu_creation_stats(&prewarmed);
        for (uint32_t index = 0u; index < 3u; ++index) {
            free(render_look(&fresh, prewarmed_options[index], CASCADES_ADDITIVE, keep, LOOK_BEAM));
        }
        BTBHGPUCreationStats rendered;
        btbh_gpu_creation_stats(&rendered);
        printf("KEEP_CASCADES_LOOK prewarm: %llu pipelines and %llu shaders made by prewarming 3 flagged scenes in a "
               "fresh window; their renders then made %llu pipelines and %llu shaders\n",
            (unsigned long long)(prewarmed.graphics_pipelines - before.graphics_pipelines),
            (unsigned long long)(prewarmed.shaders - before.shaders),
            (unsigned long long)(rendered.graphics_pipelines - prewarmed.graphics_pipelines),
            (unsigned long long)(rendered.shaders - prewarmed.shaders));
        assert(prewarmed.graphics_pipelines > before.graphics_pipelines);
        assert(rendered.graphics_pipelines == prewarmed.graphics_pipelines);
        assert(rendered.shaders == prewarmed.shaders);
        close_smoke_window(&fresh);
    }
}
#endif

static void check_refusals(SmokeWindow *smoke) {
    static Scene scene;
    const Options options = { .name = "refusal", .shadows = SHADOWS_SUN, .occlusion = OCCLUSION_HBAO,
        .reflections = true };
    BTBHGPUSceneReport report;
    build_scene(smoke, &options, &scene);
    /* (Bit 3 is RFC 0013's UV rect since 0.11.0; bit 4 stays unknown until its phase 2. Bit 6 is the light factor
       since 0.11.0; bit 7 stays unknown.) */
    scene.batches[1].flags = (uint8_t)(1u << 4);
    assert(render_scene(smoke, &scene, BTBH_GPU_INVALID_ARGUMENT, &report) == NULL);
#if defined(BTBH_GPU_SCENE_HAS_INSTANCE_LIGHT_FACTOR)
    scene.batches[1].flags = (uint8_t)(1u << 7);
#else
    scene.batches[1].flags = (uint8_t)(1u << 6);
#endif
    assert(render_scene(smoke, &scene, BTBH_GPU_INVALID_ARGUMENT, &report) == NULL);
#if defined(SMOKE_HAS_0100)
    build_scene(smoke, &options, &scene);
    scene.lighting.spot_occluder_flags = (uint8_t)(1u << 2);
    assert(render_scene(smoke, &scene, BTBH_GPU_INVALID_ARGUMENT, &report) == NULL);
    scene.lighting.spot_occluder_flags =
        (uint8_t)(BTBH_GPU_SCENE_SPOT_OCCLUDER_SUN_ONLY | BTBH_GPU_SCENE_SPOT_OCCLUDER_ORTHOGRAPHIC);
    scene.batches[1].flags = BTBH_GPU_SCENE_BATCH_RECEIVES_NO_SPOT_SHADOW;
    free(render_scene(smoke, &scene, BTBH_GPU_OK, &report));
#endif
    build_scene(smoke, &options, &scene);
    scene.post.flags = (uint8_t)(1u << 3);
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
#if defined(SMOKE_HAS_0120)
    /* 0.12.0: flags2's bits 1 to 7 are unknown (present and prewarm); its bit 0 alone is taken. */
    for (uint32_t bit = 1u; bit < 8u; ++bit) {
        build_scene(smoke, &options, &scene);
        scene.batches[1].flags2 = (uint8_t)((1u << bit) | BTBH_GPU_SCENE_BATCH_FLAGS2_KEEP_CASCADES_LOOK);
        assert(render_scene(smoke, &scene, BTBH_GPU_INVALID_ARGUMENT, &report) == NULL);
        assert(btbh_gpu_window_scene_prewarm(
            smoke->window,
            shader_paths[0], shader_paths[1], shader_paths[2], shader_paths[3],
            shader_paths[4], shader_paths[5], shader_paths[6], shader_paths[7],
            shader_paths[8], shader_paths[9], shader_paths[10],
            SMOKE_SHADER_FORMAT, TARGET_SIZE, TARGET_SIZE, &scene.lighting, &scene.post, scene.batches, scene.count,
            &prewarm_report) == BTBH_GPU_INVALID_ARGUMENT);
    }
    build_scene(smoke, &options, &scene);
    scene.batches[1].flags2 = BTBH_GPU_SCENE_BATCH_FLAGS2_KEEP_CASCADES_LOOK;
    free(render_scene(smoke, &scene, BTBH_GPU_OK, &report));
    puts("refusals: unknown batch flags and flags2 bits, unknown post flags (present and prewarm), unknown spot "
         "occluder flags");
#else
    puts("refusals: unknown batch flags, unknown post flags (present and prewarm), unknown spot occluder flags");
#endif
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
#if defined(SMOKE_HAS_0100)
    if (strcmp(mode, "1") == 0 || strcmp(mode, "facing") == 0) {
        /* 0.10.0's SSR_FACING_HITS on the steep street (its reflections without the cascades, as v2's rain). */
        BenchConfig configs[4];
        memset(configs, 0, sizeof(configs));
        const Options base = { .name = "bench-facing", .shadows = SHADOWS_SUN, .reflections = true, .steep = true };
        configs[0] = (BenchConfig){ .name = "steep street, SSR with SSR_SURFACE_REFLECTIVITY", .options = base,
            .post_flags = BTBH_GPU_SCENE_POST_SSR_SURFACE_REFLECTIVITY };
        configs[1] = (BenchConfig){ .name = "steep street, SSR with it and SSR_FACING_HITS", .options = base,
            .post_flags = BTBH_GPU_SCENE_POST_SSR_SURFACE_REFLECTIVITY | BTBH_GPU_SCENE_POST_SSR_FACING_HITS };
        configs[2] = (BenchConfig){ .name = "steep street, no SSR", .options = base };
        configs[2].options.reflections = false;
        configs[3] = (BenchConfig){ .name = "steep street, SSR with it, baseline shaders", .options = base,
            .post_flags = BTBH_GPU_SCENE_POST_SSR_SURFACE_REFLECTIVITY };
        for (uint32_t index = 0u; index < 4u; ++index) {
            configs[index].smoke = index == 3u ? baseline : smoke;
            configs[index].paths = index == 3u ? baseline_paths : (const char (*)[1024])shader_paths;
            configs[index].target = index == 3u ? baseline_target : target;
        }
        bench_configs("the reflections' facing hits", configs, 3u + extra, rounds);
    }
#endif
#if defined(SMOKE_HAS_0110)
    if (strcmp(mode, "1") == 0 || strcmp(mode, "contact") == 0) {
        /* 0.12.0's KEEP_HBAO with contact AO: the night street with its lamps and neon under additive cascades. */
        BenchConfig configs[4];
        memset(configs, 0, sizeof(configs));
        const Options base = { .name = "bench-contact", .shadows = SHADOWS_SUN, .cascades = CASCADES_ADDITIVE,
            .occlusion = OCCLUSION_HBAO_CONTACT, .gold = true, .unlit_box = true, .neon = true, .lamps = true };
        configs[0] = (BenchConfig){ .name = "additive cascades, contact AO unset", .options = base };
        configs[1] = (BenchConfig){ .name = "additive cascades, contact AO with KEEP_HBAO", .options = base,
            .post_flags = BTBH_GPU_SCENE_POST_ADDITIVE_CASCADES_KEEP_HBAO };
        configs[2] = (BenchConfig){ .name = "no cascades, contact AO", .options = base };
        configs[2].options.cascades = CASCADES_OFF;
        configs[3] = (BenchConfig){ .name = "additive cascades, contact AO unset, baseline shaders",
            .options = base };
        for (uint32_t index = 0u; index < 4u; ++index) {
            configs[index].smoke = index == 3u ? baseline : smoke;
            configs[index].paths = index == 3u ? baseline_paths : (const char (*)[1024])shader_paths;
            configs[index].target = index == 3u ? baseline_target : target;
        }
        bench_configs("contact AO under additive cascades", configs, 3u + extra, rounds);
    }
#endif
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
#if defined(SMOKE_HAS_0110)
    BTBHGPUModelReport model_report;
    memset(&model_report, 0, sizeof(model_report));
    smoke.skinned = btbh_gpu_window_model_load(smoke.window, BTBH_GPU_SMOKE_FIXTURE_DIR "/skinned-quad.gltf",
        &model_report);
    if (smoke.skinned == NULL) {
        fprintf(stderr, "skinned-quad.gltf: %s\n", model_report.message);
    }
    assert(smoke.skinned != NULL);
    BTBHGPUAnimationLibraryReport animation_report;
    memset(&animation_report, 0, sizeof(animation_report));
    smoke.animation = btbh_gpu_animation_library_load(BTBH_GPU_SMOKE_FIXTURE_DIR "/skinned-quad.gltf",
        &animation_report);
    if (smoke.animation == NULL) {
        fprintf(stderr, "skinned-quad.gltf animation: %s\n", animation_report.message);
    }
    assert(smoke.animation != NULL);
#endif
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
#if defined(SMOKE_HAS_0110)
    btbh_gpu_model_destroy(smoke->skinned);
    btbh_gpu_animation_library_destroy(smoke->animation);
#endif
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
#if defined(SMOKE_HAS_0100)
        check_rc_debug_views(&smoke);
        check_keep_hbao_point_lights(&smoke);
        check_ssr_facing_hits(&smoke);
        check_spot_occluder(&smoke);
#endif
#if defined(SMOKE_HAS_0110)
        check_keep_hbao_any_ao(&smoke);
        check_keep_hbao_contact(&smoke);
        check_light_factor(&smoke, device);
#endif
#if defined(SMOKE_HAS_0120)
        check_keep_cascades_look(&smoke, device);
#endif
        check_refusals(&smoke);
#endif
    }

    close_smoke_window(&smoke);
    btbh_gpu_device_destroy(device);
    puts("btbh_gpu_scene_flags_smoke: passed");
    return 0;
}
