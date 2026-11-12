/* RFC 0007's GPU contract test: an SDF terrain's crater patch over a dug ground plane, rendered into a target and
 * read back (a hidden window, a debug device).
 *
 * - A dug_by ground with no terrain mask, and a terrain that has no edit yet, render byte-identical to a plain
 *   ground (zeroed is as before).
 * - With a crater: the crater shows its patch (the scorched floor's colour) where the ground was, nowhere does the
 *   clear colour show through (no hole without its patch), and away from the crater the picture is the plain
 *   one's. The ground casts no shadow over its crater (the dug batch takes the fragment-stage shadow pipeline).
 * - While the work is spread over budgeted updates, every frame in between has no hole: the mask's rectangle
 *   and the chunk's mesh switch together.
 * - A curved world (world_curvature > 0) keeps the hole under its patch.
 * - The crater frame is compared with a reference image (Tests/Fixtures/sdf_terrain/crater.png) within a
 *   tolerance; BTBH_SDF_TERRAIN_WRITE_REFERENCE=1 writes it. A frame with a hole is saved beside the shaders.
 * - With BTBH_SDF_TERRAIN_BASELINE_SHADER_DIR (an earlier release's compiled shaders and assets), the plain ground
 *   renders byte-identical with those shaders too.
 * - Patches face as the renderer's front faces under the Swift layer's camera (drawn with CULL_BACK).
 * - Removing the edits renders the plain ground again, byte for byte.
 * - Be The Bullet Hell's crater from its gameplay camera (a torus rim, then an ellipsoid bowl that cuts it; 0.5 m
 *   cells, 32-cell chunks, smooth normals, a heightfield base at the top of 1 m floor slabs): no floor pixel over the
 *   bowl where its wall drops across the cell columns under the lip (a sawtooth along the lip in 0.5.0), and no
 *   background anywhere. That frame is saved beside the shaders (sdf_terrain_lip.png).
 *
 * It needs a GPU and the production shaders; with no device it is skipped (77). */
#undef NDEBUG

#include "btbh_gpu/btbh_gpu.h"
#include "btbh_gpu_internal.h"
#include "btbh_gpu_package_internal.h"

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SMOKE_SKIP 77
#define NEWLINE "\n"
#define TARGET_SIZE 160u

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

typedef struct Scene {
    BTBHGPUWindow *window;
    BTBHGPUCustomMesh *cube;
    BTBHGPUTexture *target;
    BTBHGPUSdfTerrain *terrain;
} Scene;

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

typedef struct Camera {
    float view_projection[16];
    float right[3];
    float up[3];
    float position[3];
} Camera;

static Camera look_at(const float eye[3], const float centre[3], float fov_y, const float *orthographic) {
    Camera camera;
    memset(&camera, 0, sizeof(camera));
    float forward[3] = { centre[0] - eye[0], centre[1] - eye[1], centre[2] - eye[2] };
    normalize3(forward);
    const float world_up[3] = { 0.0f, 0.0f, -1.0f };
    float side[3];
    cross3(forward, world_up, side);
    if (dot3(side, side) < 1.0e-6f) {
        const float other_up[3] = { 0.0f, 1.0f, 0.0f };
        cross3(forward, other_up, side);
    }
    normalize3(side);
    float up[3];
    cross3(side, forward, up);
    /* The Swift layer's camera (GPUSceneBackend.viewProjection): right = forward x up, view z along forward, clip
     * w = +z. SDL flips Vulkan's viewport and the scene takes clockwise as front: under this camera, triangles
     * clockwise seen from outside (the terrain's patches) are front faces. */
    const float view[16] = {
        side[0], up[0], forward[0], 0.0f,
        side[1], up[1], forward[1], 0.0f,
        side[2], up[2], forward[2], 0.0f,
        -dot3(eye, side), -dot3(eye, up), -dot3(eye, forward), 1.0f,
    };
    float projection[16];
    if (orthographic != NULL) {
        memcpy(projection, orthographic, sizeof(projection));
    } else {
        const float near_plane = 0.1f;
        const float far_plane = 60.0f;
        const float h = 1.0f / tanf(0.5f * fov_y);
        const float depth_scale = far_plane / (far_plane - near_plane);
        const float perspective[16] = {
            h, 0.0f, 0.0f, 0.0f,
            0.0f, h, 0.0f, 0.0f,
            0.0f, 0.0f, depth_scale, 1.0f,
            0.0f, 0.0f, -near_plane * depth_scale, 0.0f,
        };
        memcpy(projection, perspective, sizeof(projection));
    }
    multiply(projection, view, camera.view_projection);
    memcpy(camera.right, side, sizeof(side));
    memcpy(camera.up, up, sizeof(up));
    memcpy(camera.position, eye, sizeof(camera.position));
    return camera;
}

static BTBHGPUSceneLighting scene_lighting(void) {
    BTBHGPUSceneLighting lighting;
    memset(&lighting, 0, sizeof(lighting));
    lighting.ambient_red = 1.0f;
    lighting.ambient_green = 1.0f;
    lighting.ambient_blue = 1.0f;
    lighting.ambient_energy = 0.3f;
    lighting.directional_energy = 2.0f;
    lighting.directional_red = 1.0f;
    lighting.directional_green = 0.97f;
    lighting.directional_blue = 0.92f;
    lighting.spot_range = 8.0f;
    lighting.spot_falloff = 1.0f;
    lighting.spot_inner_cutoff = 1.0f;
    const float sun[3] = { 3.0f, 10.0f, 2.0f };
    lighting.directional_x = -sun[0];
    lighting.directional_y = -sun[1];
    lighting.directional_z = -sun[2];
    const float centre[3] = { 0.0f, 0.0f, 0.0f };
    const float rcp = 1.0f / 9.0f;
    const float near_plane = 1.0f;
    const float far_plane = 30.0f;
    const float orthographic[16] = {
        rcp, 0.0f, 0.0f, 0.0f,
        0.0f, rcp, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f / (far_plane - near_plane), 0.0f,
        0.0f, 0.0f, -near_plane / (far_plane - near_plane), 1.0f,
    };
    const Camera light = look_at(sun, centre, 1.0f, orthographic);
    memcpy(lighting.directional_shadow_view_projection, light.view_projection, sizeof(light.view_projection));
    lighting.directional_shadow_enabled = 1u;
    lighting.directional_shadow_softness = 1.0f;
    return lighting;
}

static BTBHGPUScenePost scene_post(float curvature) {
    BTBHGPUScenePost post;
    memset(&post, 0, sizeof(post));
    post.tonemap_white = 1.0f;
    post.exposure = 1.0f;
    post.contrast = 1.0f;
    post.saturation = 1.0f;
    post.blur_curve = 1.0f;
    post.tonemap_mode = BTBH_GPU_SCENE_TONEMAP_ACES;
    post.upscale_mode = BTBH_GPU_SCENE_UPSCALE_LINEAR;
    post.world_curvature[0] = curvature;
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

static BTBHGPUSceneGeometryBatch mesh_batch(BTBHGPUCustomMesh *mesh, uint32_t first) {
    BTBHGPUSceneGeometryBatch batch;
    memset(&batch, 0, sizeof(batch));
    batch.kind = BTBH_GPU_SCENE_GEOMETRY_CUSTOM_MESH;
    batch.custom_mesh = mesh;
    batch.material_slot = -1;
    batch.first_instance = first;
    batch.instance_count = 1u;
    batch.albedo_red = 255u;
    batch.albedo_green = 255u;
    batch.albedo_blue = 255u;
    batch.albedo_alpha = 255u;
    batch.uv_scale_x = 1.0f;
    batch.uv_scale_y = 1.0f;
    batch.roughness = 0.8f;
    batch.occlusion = 1.0f;
    batch.depth_compare = BTBH_GPU_SCENE_DEPTH_LESS_EQUAL;
    batch.cull_mode = BTBH_GPU_SCENE_CULL_BACK;
    batch.stencil_pass = BTBH_GPU_SCENE_STENCIL_REPLACE;
    batch.spot_shadow_visible = 1u;
    return batch;
}

/* A unit cube (-0.5..0.5) in the 96-byte custom-mesh vertex. */
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
        /* Clockwise from outside, as the terrain's patches: front faces under this camera. */
        const uint32_t quad[6] = { base, base + 2u, base + 1u, base, base + 3u, base + 2u };
        memcpy(&indices[face * 6u], quad, sizeof(quad));
    }
    BTBHGPUCustomMeshReport report;
    memset(&report, 0, sizeof(report));
    BTBHGPUCustomMesh *mesh = btbh_gpu_window_custom_mesh_create(
        window, vertices, (uint32_t)sizeof(vertices), 24u, indices, 36u, &report);
    assert(mesh != NULL);
    return mesh;
}

/* The texture's texels as RGBA8. */
static uint8_t *read_texture(BTBHGPUWindow *window, BTBHGPUTexture *texture) {
    SDL_GPUDevice *device = window->device->gpu;
    const uint32_t size = texture->width * texture->height * 4u;
    SDL_GPUTransferBufferCreateInfo info;
    SDL_zero(info);
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    info.size = size;
    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(device, &info);
    assert(transfer != NULL);
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
    assert(fence != NULL);
    assert(SDL_WaitForGPUFences(device, true, &fence, 1u));
    SDL_ReleaseGPUFence(device, fence);
    uint8_t *pixels = malloc(size);
    assert(pixels != NULL);
    const void *mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
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

typedef struct RenderOptions {
    int ground_dug;          /* the ground batch has dug_by = the terrain */
    int ground_casts;        /* the ground casts into the directional shadow */
    int patches;             /* the terrain's chunks are drawn */
    float curvature;
    int expect_old_shader_refusal; /* the present must fail: dug_by with shaders that lack the dig mask */
} RenderOptions;

static float ground_colour[4] = { 0.75f, 0.75f, 0.72f, 1.0f };

/* The crater seen from above and to the side, on 14 x 14 m of ground tiles whose top is at y = 0. */
static uint8_t *render(Scene *scene, RenderOptions options) {
    const float eye[3] = { 0.0f, 9.0f, 5.5f };
    const float centre[3] = { 0.0f, -0.4f, 0.0f };
    const Camera camera = look_at(eye, centre, 0.75f, NULL);
    const BTBHGPUSceneLighting lighting = scene_lighting();
    const BTBHGPUScenePost post = scene_post(options.curvature);
    const uint32_t chunk_count = btbh_gpu_sdf_terrain_chunk_count(scene->terrain);
    /* The ground as 0.5 m tiles (as BTBH's), so the world curvature bends it as it bends the patches. */
    const uint32_t tiles_per_side = 28u;
    const uint32_t tile_count = tiles_per_side * tiles_per_side;
    float *instances = calloc((size_t)(tile_count + 1u) * 24u, sizeof(float));
    BTBHGPUSceneGeometryBatch *batches = calloc(1u + chunk_count, sizeof(BTBHGPUSceneGeometryBatch));
    uint8_t *shadow_visibility = calloc(1u + chunk_count, 1u);
    assert(instances != NULL && batches != NULL && shadow_visibility != NULL);
    const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    for (uint32_t k = 0u; k < tiles_per_side; ++k) {
        for (uint32_t i = 0u; i < tiles_per_side; ++i) {
            instance(&instances[(k * tiles_per_side + i) * 24u], -6.75f + 0.5f * (float)i, -0.05f,
                -6.75f + 0.5f * (float)k, 0.5f, 0.1f, 0.5f, ground_colour);
        }
    }
    instance(&instances[tile_count * 24u], 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, white);
    uint32_t batch_count = 0u;
    batches[batch_count] = mesh_batch(scene->cube, 0u);
    batches[batch_count].instance_count = tile_count;
    batches[batch_count].dug_by = options.ground_dug ? scene->terrain : NULL;
    shadow_visibility[batch_count] = options.ground_casts ? 1u : 0u;
    batch_count += 1u;
    for (uint32_t index = 0u; options.patches && index < chunk_count; ++index) {
        BTBHGPUSdfTerrainChunk chunk;
        if (btbh_gpu_sdf_terrain_chunk(scene->terrain, index, &chunk) && chunk.mesh != NULL) {
            batches[batch_count] = mesh_batch(chunk.mesh, tile_count);
            shadow_visibility[batch_count] = 1u;
            batch_count += 1u;
        }
    }
    BTBHGPUSceneReport report;
    memset(&report, 0, sizeof(report));
    const BTBHGPUResult result = btbh_gpu_window_scene_render_to_target(
        scene->window,
        scene->target,
        shader_paths[0], shader_paths[1], shader_paths[2], shader_paths[3],
        shader_paths[4], shader_paths[5], shader_paths[6], shader_paths[7],
        shader_paths[8], shader_paths[9], shader_paths[10],
        SMOKE_SHADER_FORMAT,
        TARGET_SIZE,
        TARGET_SIZE,
        camera.view_projection,
        camera.right,
        camera.up,
        camera.position,
        &lighting,
        &post,
        instances,
        tile_count + 1u,
        tile_count + 1u,
        batches,
        NULL,
        shadow_visibility,
        batch_count,
        255u, 0u, 255u, 255u,
        &report
    );
    if (options.expect_old_shader_refusal) {
        printf("old shaders with dug_by: %s%s", report.message, NEWLINE);
        assert(result == BTBH_GPU_INVALID_ARGUMENT);
        assert(strstr(report.message, "dig mask") != NULL);
        assert(strstr(report.message, shader_paths[7]) != NULL);
        free(instances);
        free(batches);
        free(shadow_visibility);
        return NULL;
    }
    if (result != BTBH_GPU_OK) {
        fprintf(stderr, "render: %s\n", report.message);
    }
    assert(result == BTBH_GPU_OK);
    free(instances);
    free(batches);
    free(shadow_visibility);
    return read_texture(scene->window, scene->target);
}

static uint32_t pink_pixels(const uint8_t *pixels) {
    uint32_t count = 0u;
    for (uint32_t index = 0u; index < TARGET_SIZE * TARGET_SIZE; ++index) {
        const uint8_t *p = &pixels[index * 4u];
        if (p[0] > 120u && p[2] > 120u && p[1] < 80u) {
            count += 1u;
        }
    }
    return count;
}

static uint32_t differing_pixels(const uint8_t *a, const uint8_t *b, int tolerance) {
    uint32_t count = 0u;
    for (uint32_t index = 0u; index < TARGET_SIZE * TARGET_SIZE; ++index) {
        int distance = 0;
        for (int channel = 0; channel < 3; ++channel) {
            distance += abs((int)a[index * 4u + (uint32_t)channel] - (int)b[index * 4u + (uint32_t)channel]);
        }
        if (distance > tolerance) {
            count += 1u;
        }
    }
    return count;
}

static float luminance_at(const uint8_t *pixels, uint32_t x, uint32_t y) {
    const uint8_t *p = &pixels[(y * TARGET_SIZE + x) * 4u];
    return 0.2126f * (float)p[0] + 0.7152f * (float)p[1] + 0.0722f * (float)p[2];
}

static void add_crater(BTBHGPUSdfTerrain *terrain, uint32_t ids[2]) {
    BTBHGPUSdfEdit bowl;
    memset(&bowl, 0, sizeof(bowl));
    bowl.shape = BTBH_GPU_SDF_SHAPE_ELLIPSOID;
    bowl.operation = BTBH_GPU_SDF_OPERATION_SUBTRACT;
    bowl.position[1] = 0.6f;
    bowl.size[0] = 2.6f;
    bowl.size[1] = 1.5f;
    bowl.size[2] = 2.6f;
    bowl.smoothing = 0.3f;
    bowl.material = 2u;
    bowl.material_band = 0.2f;
    BTBHGPUSdfEdit rim;
    memset(&rim, 0, sizeof(rim));
    rim.shape = BTBH_GPU_SDF_SHAPE_TORUS;
    rim.operation = BTBH_GPU_SDF_OPERATION_ADD;
    rim.position[1] = -0.25f;
    rim.size[0] = 2.7f;
    rim.size[1] = 0.5f;
    rim.smoothing = 0.6f;
    rim.material = 1u;
    ids[0] = btbh_gpu_sdf_terrain_add_edit(terrain, &bowl);
    ids[1] = btbh_gpu_sdf_terrain_add_edit(terrain, &rim);
    assert(ids[0] != 0u && ids[1] != 0u);
}

static void flat_base(void *user, const float *points, uint32_t count, float *distances, uint8_t *materials) {
    (void)user;
    for (uint32_t index = 0u; index < count; ++index) {
        const float y = points[index * 3u + 1u];
        distances[index] = y;
        materials[index] = -y <= 0.3f ? 0u : 1u;
    }
}

static int compare_reference(const uint8_t *pixels) {
    char path[1024];
    (void)snprintf(path, sizeof(path), "%s/sdf_terrain/crater.png", BTBH_GPU_SMOKE_FIXTURE_DIR);
    const char *write = SDL_getenv("BTBH_SDF_TERRAIN_WRITE_REFERENCE");
    if (write != NULL && write[0] == '1') {
        assert(btbh_gpu_write_png_rgba8(path, TARGET_SIZE, TARGET_SIZE, pixels) == BTBH_GPU_OK);
        printf("wrote the reference %s\n", path);
        return 1;
    }
    SDL_Surface *surface = SDL_LoadPNG(path);
    if (surface == NULL) {
        fprintf(stderr, "no reference image %s (%s)\n", path, SDL_GetError());
        return 0;
    }
    SDL_Surface *rgba = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_ABGR8888);
    SDL_DestroySurface(surface);
    assert(rgba != NULL && (uint32_t)rgba->w == TARGET_SIZE && (uint32_t)rgba->h == TARGET_SIZE);
    uint8_t *reference = malloc(TARGET_SIZE * TARGET_SIZE * 4u);
    assert(reference != NULL);
    for (uint32_t y = 0u; y < TARGET_SIZE; ++y) {
        memcpy(&reference[y * TARGET_SIZE * 4u], (const uint8_t *)rgba->pixels + (size_t)y * (size_t)rgba->pitch,
            TARGET_SIZE * 4u);
    }
    SDL_DestroySurface(rgba);
    /* Another GPU or driver may round a little differently: a few soft-edged pixels may differ, nothing else. */
    const uint32_t differing = differing_pixels(pixels, reference, 24);
    free(reference);
    printf("reference: %u of %u pixels differ by more than 24 (of 765)\n", differing, TARGET_SIZE * TARGET_SIZE);
    return differing <= TARGET_SIZE * TARGET_SIZE / 100u;
}

static void save(const char *name, const uint8_t *pixels) {
    char path[1024];
    (void)snprintf(path, sizeof(path), "%s/../../sdf_terrain_%s.png", BTBH_GPU_SMOKE_SHADER_DIR, name);
    (void)btbh_gpu_write_png_rgba8(path, TARGET_SIZE, TARGET_SIZE, pixels);
    printf("saved %s\n", path);
}

/* ------------------------------------------------------------------------------------ the crater's lip */

/* Be The Bullet Hell's set-up: 0.5 m cells, 32-cell chunks, smooth normals, a heightfield base at its floor tiles'
 * tops, a crater as a torus rim (ADD) and then an ellipsoid bowl (SUBTRACT) that cuts through the rim, and floor
 * tiles that are 1 m slabs with dug_by. The floor is green here, so every floor pixel is easy to tell. */
#define LIP_FLOOR_TOP 0.3f
#define LIP_TILES 16u        /* the heightfield's 1 m cells over the terrain's bounds */
#define LIP_FLOOR_TILES 28u  /* the floor's 1 m tiles, past the bounds to the edges of the view */

static const float lip_floor_colour[4] = { 0.25f, 0.8f, 0.3f, 1.0f };

static Camera lip_camera(void) {
    /* Be The Bullet Hell's gameplay camera: high and oblique, so the far side of the bowl faces it. */
    const float eye[3] = { 0.0f, 9.5f, 7.0f };
    const float centre[3] = { 0.0f, -0.3f, -0.6f };
    return look_at(eye, centre, 0.8f, NULL);
}

static uint8_t *render_lip(Scene *scene, BTBHGPUSdfTerrain *terrain) {
    const Camera camera = lip_camera();
    const BTBHGPUSceneLighting lighting = scene_lighting();
    const BTBHGPUScenePost post = scene_post(0.0f);
    const uint32_t chunk_count = btbh_gpu_sdf_terrain_chunk_count(terrain);
    const uint32_t tile_count = LIP_FLOOR_TILES * LIP_FLOOR_TILES;
    float *instances = calloc((size_t)(tile_count + 1u) * 24u, sizeof(float));
    BTBHGPUSceneGeometryBatch *batches = calloc(1u + chunk_count, sizeof(BTBHGPUSceneGeometryBatch));
    uint8_t *shadow_visibility = calloc(1u + chunk_count, 1u);
    assert(instances != NULL && batches != NULL && shadow_visibility != NULL);
    const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    for (uint32_t k = 0u; k < LIP_FLOOR_TILES; ++k) {
        for (uint32_t i = 0u; i < LIP_FLOOR_TILES; ++i) {
            instance(&instances[(k * LIP_FLOOR_TILES + i) * 24u], -13.5f + (float)i, LIP_FLOOR_TOP - 0.5f,
                -13.5f + (float)k, 1.0f, 1.0f, 1.0f, lip_floor_colour);
        }
    }
    instance(&instances[tile_count * 24u], 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, white);
    uint32_t batch_count = 0u;
    batches[batch_count] = mesh_batch(scene->cube, 0u);
    batches[batch_count].instance_count = tile_count;
    batches[batch_count].dug_by = terrain;
    shadow_visibility[batch_count] = 0u;   /* as Be The Bullet Hell's floor: castsShadow false */
    batch_count += 1u;
    for (uint32_t index = 0u; index < chunk_count; ++index) {
        BTBHGPUSdfTerrainChunk chunk;
        if (btbh_gpu_sdf_terrain_chunk(terrain, index, &chunk) && chunk.mesh != NULL) {
            batches[batch_count] = mesh_batch(chunk.mesh, tile_count);
            shadow_visibility[batch_count] = 1u;
            batch_count += 1u;
        }
    }
    BTBHGPUSceneReport report;
    memset(&report, 0, sizeof(report));
    const BTBHGPUResult result = btbh_gpu_window_scene_render_to_target(
        scene->window, scene->target,
        shader_paths[0], shader_paths[1], shader_paths[2], shader_paths[3],
        shader_paths[4], shader_paths[5], shader_paths[6], shader_paths[7],
        shader_paths[8], shader_paths[9], shader_paths[10],
        SMOKE_SHADER_FORMAT, TARGET_SIZE, TARGET_SIZE,
        camera.view_projection, camera.right, camera.up, camera.position,
        &lighting, &post, instances, tile_count + 1u, tile_count + 1u, batches, NULL, shadow_visibility,
        batch_count, 255u, 0u, 255u, 255u, &report);
    if (result != BTBH_GPU_OK) {
        fprintf(stderr, "render: %s\n", report.message);
    }
    assert(result == BTBH_GPU_OK);
    free(instances);
    free(batches);
    free(shadow_visibility);
    return read_texture(scene->window, scene->target);
}

/* out = the inverse of the column-major m (Gauss-Jordan with partial pivoting). */
static void invert4(const float m[16], float out[16]) {
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
            out[column * 4 + row] = (float)a[row][column + 4];
        }
    }
}

static void unproject(const float inverse[16], float x, float y, float z, float out[3]) {
    const float clip[4] = { x, y, z, 1.0f };
    float world[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    for (int row = 0; row < 4; ++row) {
        for (int k = 0; k < 4; ++k) {
            world[row] += inverse[k * 4 + row] * clip[k];
        }
    }
    out[0] = world[0] / world[3];
    out[1] = world[1] / world[3];
    out[2] = world[2] / world[3];
}

/* The floor's green, lit or in shadow; the patch's colours (grey, soil, scorch) are not green. */
static int is_lip_floor(const uint8_t *p) {
    return (int)p[1] >= (int)p[0] + 24 && (int)p[1] >= (int)p[2] + 24;
}

/* The floor pixels over the bowl: pixels that show the floor (green) where the pixel's ray meets the floor's
 * top over a spot where the terrain's surface lies more than a fifth of a cell under it. The floor is dug away
 * there, so the pixel should show the bowl. `marked` (may be NULL) gets those pixels in red. */
static uint32_t floor_over_bowl(const BTBHGPUSdfTerrain *terrain, const uint8_t *pixels, uint8_t *marked,
    uint32_t *floor_pixels) {
    const Camera camera = lip_camera();
    float inverse[16];
    invert4(camera.view_projection, inverse);
    uint32_t count = 0u;
    uint32_t floor_count = 0u;
    for (uint32_t py = 0u; py < TARGET_SIZE; ++py) {
        for (uint32_t px = 0u; px < TARGET_SIZE; ++px) {
            const uint8_t *p = &pixels[(py * TARGET_SIZE + px) * 4u];
            if (!is_lip_floor(p)) {
                continue;
            }
            floor_count += 1u;
            /* SDL flips Vulkan's viewport: the top row is NDC y = +1. */
            const float ndc_x = 2.0f * ((float)px + 0.5f) / (float)TARGET_SIZE - 1.0f;
            const float ndc_y = 1.0f - 2.0f * ((float)py + 0.5f) / (float)TARGET_SIZE;
            float near_point[3];
            float far_point[3];
            unproject(inverse, ndc_x, ndc_y, 0.0f, near_point);
            unproject(inverse, ndc_x, ndc_y, 1.0f, far_point);
            const float dy = far_point[1] - near_point[1];
            if (fabsf(dy) < 1.0e-6f) {
                continue;
            }
            const float s = (LIP_FLOOR_TOP - near_point[1]) / dy;
            const float x = near_point[0] + s * (far_point[0] - near_point[0]);
            const float z = near_point[2] + s * (far_point[2] - near_point[2]);
            const float surface = btbh_gpu_sdf_terrain_height(terrain, x, z, LIP_FLOOR_TOP + 4.0f);
            if (surface < LIP_FLOOR_TOP - 0.1f) {
                count += 1u;
                if (marked != NULL) {
                    uint8_t *m = &marked[(py * TARGET_SIZE + px) * 4u];
                    m[0] = 255u;
                    m[1] = 0u;
                    m[2] = 0u;
                }
            }
        }
    }
    if (floor_pixels != NULL) {
        *floor_pixels = floor_count;
    }
    return count;
}

/* A sawtooth of floor one cell wide showed along the top of the bowl wall (0.5.0, Be The Bullet Hell). The floor's
 * column under the lip is the patch's, but its mask held the surface's height on the column's high side only. */
static int test_lip(Scene *scene) {
    float heights[(LIP_TILES + 1u) * (LIP_TILES + 1u)];
    for (uint32_t index = 0u; index < (LIP_TILES + 1u) * (LIP_TILES + 1u); ++index) {
        heights[index] = LIP_FLOOR_TOP;
    }
    BTBHGPUSdfTerrainDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.bounds_min[0] = -8.0f;
    desc.bounds_min[1] = -8.0f;
    desc.bounds_min[2] = -8.0f;
    desc.bounds_max[0] = 8.0f;
    desc.bounds_max[1] = 8.0f;
    desc.bounds_max[2] = 8.0f;
    desc.cell_size = 0.5f;
    desc.chunk_cells = 32u;
    desc.base_heights = heights;
    desc.base_width = LIP_TILES;
    desc.base_depth = LIP_TILES;
    desc.base_cell = 1.0f;
    desc.base_surface_material = 0u;
    desc.base_subsurface_material = 1u;
    desc.base_subsurface_depth = 0.3f;
    desc.normals = BTBH_GPU_SDF_NORMALS_SMOOTH;
    const float colours[3][4] = {
        { 0.36f, 0.37f, 0.42f, 1.0f }, { 0.45f, 0.30f, 0.16f, 1.0f }, { 0.08f, 0.06f, 0.05f, 1.0f }
    };
    memcpy(desc.material_colors, colours, sizeof(colours));
    BTBHGPUSdfTerrainReport report;
    BTBHGPUSdfTerrain *terrain = btbh_gpu_sdf_terrain_create(&desc, &report);
    assert(terrain != NULL);
    assert(btbh_gpu_window_sdf_terrain_attach(scene->window, terrain, &report) == BTBH_GPU_OK);
    uint8_t *untouched = render_lip(scene, terrain);
    assert(pink_pixels(untouched) == 0u);
    assert(floor_over_bowl(terrain, untouched, NULL, NULL) == 0u);

    /* The rim first, then the bowl, as Be The Bullet Hell digs them: the bowl cuts the rim about a cell (0.55 m)
     * above the floor, and its wall drops steeply from there. */
    BTBHGPUSdfEdit rim;
    memset(&rim, 0, sizeof(rim));
    rim.shape = BTBH_GPU_SDF_SHAPE_TORUS;
    rim.operation = BTBH_GPU_SDF_OPERATION_ADD;
    rim.position[1] = LIP_FLOOR_TOP - 0.3f;
    rim.size[0] = 3.6f;
    rim.size[1] = 0.9f;
    rim.smoothing = 0.6f;
    rim.material = 1u;
    BTBHGPUSdfEdit bowl;
    memset(&bowl, 0, sizeof(bowl));
    bowl.shape = BTBH_GPU_SDF_SHAPE_ELLIPSOID;
    bowl.operation = BTBH_GPU_SDF_OPERATION_SUBTRACT;
    bowl.position[1] = LIP_FLOOR_TOP + 0.9f;
    bowl.size[0] = 3.4f;
    bowl.size[1] = 2.4f;
    bowl.size[2] = 3.4f;
    bowl.smoothing = 0.3f;
    bowl.material = 2u;
    bowl.material_band = 0.3f;
    assert(btbh_gpu_sdf_terrain_add_edit(terrain, &rim) != 0u);
    assert(btbh_gpu_sdf_terrain_add_edit(terrain, &bowl) != 0u);
    do {
        assert(btbh_gpu_sdf_terrain_update(terrain, 0u, &report) == BTBH_GPU_OK);
    } while (report.bricks_evaluated != 0u || report.chunks_meshed != 0u);
    assert(report.mask_bytes > 0u);

    uint8_t *lip = render_lip(scene, terrain);
    save("lip", lip);
    uint8_t *marked = malloc(TARGET_SIZE * TARGET_SIZE * 4u);
    assert(marked != NULL);
    memcpy(marked, lip, TARGET_SIZE * TARGET_SIZE * 4u);
    uint32_t floor_pixels = 0u;
    const uint32_t over_bowl = floor_over_bowl(terrain, lip, marked, &floor_pixels);
    const uint32_t background = pink_pixels(lip);
    printf("crater lip: %u floor pixels over the bowl (of %u floor pixels), %u background pixels\n", over_bowl,
        floor_pixels, background);
    if (over_bowl != 0u) {
        save("lip_floor_over_bowl", marked);
    }
    /* Away from the crater the floor is as without it: the image's corners (more than 2 m from the rim). */
    const uint32_t corners[4][2] = { { 4u, 4u }, { TARGET_SIZE - 5u, 4u }, { 4u, TARGET_SIZE - 5u },
                                    { TARGET_SIZE - 5u, TARGET_SIZE - 5u } };
    for (int index = 0; index < 4; ++index) {
        const size_t at = ((size_t)corners[index][1] * TARGET_SIZE + corners[index][0]) * 4u;
        assert(memcmp(&untouched[at], &lip[at], 4u) == 0);
    }
    free(marked);
    free(lip);
    free(untouched);
    btbh_gpu_sdf_terrain_destroy(terrain);
    /* No background anywhere (no hole without its patch), and no floor over the bowl (no sawtooth). */
    return background == 0u && over_bowl == 0u;
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
    Scene scene;
    memset(&scene, 0, sizeof(scene));
    const BTBHGPUWindowDescriptor descriptor = {
        .title = "BTBH SDF terrain smoke",
        .width = 64u,
        .height = 64u,
        .hidden = 1,
    };
    BTBHGPUWindowReport window_report;
    memset(&window_report, 0, sizeof(window_report));
    scene.window = btbh_gpu_window_create(&descriptor, &window_report);
    assert(scene.window != NULL);
    assert(btbh_gpu_window_claim(device, scene.window, &window_report) == BTBH_GPU_OK);
    scene.cube = create_cube(scene.window);
    BTBHGPUTextureReport texture_report;
    scene.target = btbh_gpu_window_render_target_create(
        scene.window, TARGET_SIZE, TARGET_SIZE, BTBH_GPU_TEXTURE_FILTER_LINEAR, &texture_report);
    assert(scene.target != NULL);

    BTBHGPUSdfTerrainDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.bounds_min[0] = -8.0f;
    desc.bounds_min[1] = -4.0f;
    desc.bounds_min[2] = -8.0f;
    desc.bounds_max[0] = 8.0f;
    desc.bounds_max[1] = 4.0f;
    desc.bounds_max[2] = 8.0f;
    desc.cell_size = 0.25f;
    desc.chunk_cells = 16u;   /* 4 m chunks: the crater spans several */
    desc.base_function = flat_base;
    desc.normals = BTBH_GPU_SDF_NORMALS_FACETED;
    /* asphalt, soil, scorch: the ground's own colour for asphalt, so only the crater's colours show */
    const float colours[3][4] = {
        { 0.75f, 0.75f, 0.72f, 1.0f }, { 0.45f, 0.30f, 0.16f, 1.0f }, { 0.08f, 0.06f, 0.05f, 1.0f }
    };
    memcpy(desc.material_colors, colours, sizeof(colours));
    BTBHGPUSdfTerrainReport report;
    scene.terrain = btbh_gpu_sdf_terrain_create(&desc, &report);
    assert(scene.terrain != NULL);
    assert(btbh_gpu_window_sdf_terrain_attach(scene.window, scene.terrain, &report) == BTBH_GPU_OK);
    assert(report.mask_bytes == 0u);

    /* Zeroed is as before: a dug_by ground whose terrain has no mask renders as the plain ground. */
    const RenderOptions plain = { 0, 1, 1, 0.0f };
    const RenderOptions dug = { 1, 1, 1, 0.0f };
    uint8_t *plain_image = render(&scene, plain);
    /* With BTBH_SDF_TERRAIN_BASELINE_SHADER_DIR (an earlier release's compiled shaders): the plain ground renders
     * byte-identical with them, so the shaders' new branch changes nothing for a batch without a mask. */
    const char *baseline_dir = SDL_getenv("BTBH_SDF_TERRAIN_BASELINE_SHADER_DIR");
    if (baseline_dir != NULL && baseline_dir[0] != 0) {
        char saved[11][1024];
        memcpy(saved, shader_paths, sizeof(saved));
        for (int index = 0; index < 11; ++index) {
            (void)snprintf(shader_paths[index], sizeof(shader_paths[index]), "%s/%s%s", baseline_dir,
                shader_names[index], SMOKE_SHADER_EXTENSION);
        }
        /* Another window: its pipelines are made from these shaders. */
        Scene baseline = scene;
        BTBHGPUWindowReport baseline_report;
        memset(&baseline_report, 0, sizeof(baseline_report));
        baseline.window = btbh_gpu_window_create(&descriptor, &baseline_report);
        assert(baseline.window != NULL);
        assert(btbh_gpu_window_claim(device, baseline.window, &baseline_report) == BTBH_GPU_OK);
        baseline.cube = create_cube(baseline.window);
        baseline.target = btbh_gpu_window_render_target_create(
            baseline.window, TARGET_SIZE, TARGET_SIZE, BTBH_GPU_TEXTURE_FILTER_LINEAR, &texture_report);
        assert(baseline.target != NULL);
        uint8_t *baseline_image = render(&baseline, plain);
        const uint32_t differing = differing_pixels(plain_image, baseline_image, 0);
        printf("baseline shaders (%s): %u pixels differ%s", baseline_dir, differing, NEWLINE);
        assert(memcmp(plain_image, baseline_image, TARGET_SIZE * TARGET_SIZE * 4u) == 0);
        free(baseline_image);
        btbh_gpu_window_destroy(baseline.window);
        memcpy(shader_paths, saved, sizeof(saved));
    }
    uint8_t *no_mask_image = render(&scene, dug);
    assert(memcmp(plain_image, no_mask_image, TARGET_SIZE * TARGET_SIZE * 4u) == 0);
    assert(pink_pixels(plain_image) == 0u);
    free(no_mask_image);

    /* A dug batch with a fragment shader that doesn't sample the dig mask (shaders from before 0.5.0) fails the
     * present, naming the shader, instead of drawing the ground undug. Stood in for by a copy of this one with
     * the name of the mask's texture changed (SPIR-V; Metal libraries aren't checked). Without dug_by it draws. */
    if (SMOKE_SHADER_FORMAT == BTBH_GPU_SHADER_FORMAT_SPIRV) {
        size_t byte_count = 0u;
        uint8_t *bytes = (uint8_t *)SDL_LoadFile(shader_paths[7], &byte_count);
        assert(bytes != NULL);
        static const char name[] = "dig_mask_texture";
        uint32_t renamed = 0u;
        for (size_t offset = 0u; offset + sizeof(name) - 1u <= byte_count; ++offset) {
            if (memcmp(bytes + offset, name, sizeof(name) - 1u) == 0) {
                bytes[offset] = 'x';
                renamed += 1u;
            }
        }
        assert(renamed > 0u);
        char saved[1024];
        memcpy(saved, shader_paths[7], sizeof(saved));
        (void)snprintf(shader_paths[7], sizeof(shader_paths[7]), "%s/production_model_no_dig_mask.frag.spv",
            BTBH_GPU_SMOKE_SHADER_DIR);
        assert(SDL_SaveFile(shader_paths[7], bytes, byte_count));
        SDL_free(bytes);
        const RenderOptions old_dug = { 1, 1, 1, 0.0f, 1 };
        assert(render(&scene, old_dug) == NULL);
        uint8_t *old_plain = render(&scene, plain);
        assert(memcmp(plain_image, old_plain, TARGET_SIZE * TARGET_SIZE * 4u) == 0);
        free(old_plain);
        (void)SDL_RemovePath(shader_paths[7]);
        memcpy(shader_paths[7], saved, sizeof(saved));
        printf("old shaders: a dug batch fails the present with the shader's path; a plain one draws%s", NEWLINE);
    }

    /* The crater, its work spread over budgeted updates: no frame in between shows a hole. */
    uint32_t ids[2];
    add_crater(scene.terrain, ids);
    uint32_t frames = 0u;
    for (;;) {
        assert(btbh_gpu_sdf_terrain_update(scene.terrain, 50u, &report) == BTBH_GPU_OK);
        uint8_t *between = render(&scene, dug);
        if (pink_pixels(between) != 0u) {
            printf("frame %u after %u bricks and %u chunks: %u background pixels\n", frames, report.bricks_evaluated,
                report.chunks_meshed, pink_pixels(between));
            save("hole", between);
        }
        assert(pink_pixels(between) == 0u);
        free(between);
        frames += 1u;
        if (report.pending_bricks == 0u && report.bricks_evaluated == 0u && report.chunks_meshed == 0u) {
            break;
        }
        assert(frames < 10000u);
    }
    assert(report.mask_bytes > 0u);
    printf("crater: %u budgeted updates, each frame without a hole; mesh %llu bytes, mask %llu bytes\n", frames,
        (unsigned long long)report.mesh_bytes, (unsigned long long)report.mask_bytes);

    uint8_t *crater = render(&scene, dug);
    assert(pink_pixels(crater) == 0u);
    /* The crater's floor (the image's centre, a little below it) shows the scorched patch, not the ground. */
    const uint32_t cx = TARGET_SIZE / 2u;
    const uint32_t cy = TARGET_SIZE / 2u;
    const float plain_centre = luminance_at(plain_image, cx, cy);
    const float crater_centre = luminance_at(crater, cx, cy);
    printf("centre luminance: ground %.1f, crater floor %.1f\n", plain_centre, crater_centre);
    assert(crater_centre < 0.5f * plain_centre);
    /* Away from the crater, the plain picture (corners of the image: more than 4 m from the crater). */
    const uint32_t corners[4][2] = { { 4u, 4u }, { TARGET_SIZE - 5u, 4u }, { 4u, TARGET_SIZE - 5u },
                                    { TARGET_SIZE - 5u, TARGET_SIZE - 5u } };
    for (int index = 0; index < 4; ++index) {
        const uint8_t *a = &plain_image[(corners[index][1] * TARGET_SIZE + corners[index][0]) * 4u];
        const uint8_t *b = &crater[(corners[index][1] * TARGET_SIZE + corners[index][0]) * 4u];
        assert(memcmp(a, b, 4u) == 0);
    }
    /* The dug ground casts no shadow over its crater: the floor is as bright as when the ground casts nothing. */
    const RenderOptions dug_not_casting = { 1, 0, 1, 0.0f };
    uint8_t *not_casting = render(&scene, dug_not_casting);
    const float floor_lit = luminance_at(not_casting, cx, cy);
    printf("crater floor: %.1f with the dug ground casting, %.1f without\n", crater_centre, floor_lit);
    assert(fabsf(crater_centre - floor_lit) <= 3.0f);
    free(not_casting);
    /* Without the dig, the ground would cover the crater (its centre the ground's colour). */
    const RenderOptions undug = { 0, 1, 1, 0.0f };
    uint8_t *covered = render(&scene, undug);
    assert(fabsf(luminance_at(covered, cx, cy) - plain_centre) <= 3.0f);
    free(covered);
    const int matches_reference = compare_reference(crater);

    /* A curved world: the hole stays under its patch. */
    const RenderOptions curved = { 1, 1, 1, 0.03f };
    const RenderOptions curved_plain = { 0, 1, 0, 0.03f };
    uint8_t *curved_image = render(&scene, curved);
    uint8_t *curved_plain_image = render(&scene, curved_plain);
    const uint32_t curved_ground_pink = pink_pixels(curved_plain_image);
    printf("curved world: %u background pixels without the crater, %u with it\n", curved_ground_pink,
        pink_pixels(curved_image));
    assert(pink_pixels(curved_image) == curved_ground_pink);
    assert(luminance_at(curved_image, cx, cy) < 0.5f * luminance_at(curved_plain_image, cx, cy));
    free(curved_image);
    free(curved_plain_image);

    /* Removing the edits renders the plain ground again, byte for byte. */
    assert(btbh_gpu_sdf_terrain_remove_edit(scene.terrain, ids[1]) == BTBH_GPU_OK);
    assert(btbh_gpu_sdf_terrain_remove_edit(scene.terrain, ids[0]) == BTBH_GPU_OK);
    do {
        assert(btbh_gpu_sdf_terrain_update(scene.terrain, 0u, &report) == BTBH_GPU_OK);
    } while (report.bricks_evaluated != 0u || report.chunks_meshed != 0u);
    assert(report.mask_bytes == 0u && report.mesh_bytes == 0u);
    uint8_t *restored = render(&scene, dug);
    assert(memcmp(plain_image, restored, TARGET_SIZE * TARGET_SIZE * 4u) == 0);
    free(restored);

    free(crater);
    free(plain_image);
    btbh_gpu_sdf_terrain_destroy(scene.terrain);
    scene.terrain = NULL;

    /* Be The Bullet Hell's crater from its gameplay camera: no floor over the bowl, no background anywhere. */
    const int lip_ok = test_lip(&scene);

    btbh_gpu_custom_mesh_destroy(scene.cube);
    btbh_gpu_texture_destroy(scene.target);
    btbh_gpu_window_destroy(scene.window);
    btbh_gpu_device_destroy(device);
    if (!matches_reference) {
        fprintf(stderr, "the crater frame does not match its reference\n");
        return 1;
    }
    if (!lip_ok) {
        fprintf(stderr, "the crater's lip shows floor over the bowl, or the background\n");
        return 1;
    }
    printf("btbh_gpu_sdf_terrain_smoke: all passed\n");
    return 0;
}
