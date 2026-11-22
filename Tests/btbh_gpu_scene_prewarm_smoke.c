/* btbh_gpu_window_scene_prewarm (since 0.7.0): the scene's pipelines made at load, not by the first frames that
 * need them.
 *
 * A scene like Be The Bullet Hell v2's (FSR 3, the sun's shadow, SSAO, SSR, bloom, fog, vignette; opaque ground and
 * buildings, a skinned unit, an x-ray stencil writer) runs for a few frames, then its effects appear (a decal, an
 * additive unlit flash, a premultiplied soft particle, the x-ray tester, a refraction strip), as a game's first shot
 * would. Window A runs it cold: its first frame and the frame the effects appear make shaders and pipelines (counted
 * by btbh_gpu_creation_stats). Window B prewarms with every batch first: the call makes what A's frames made, a
 * second call nothing, no frame of B anything, and each of B's frames is byte for byte A's. B then prewarms for FSR 1
 * (other pipelines), and its FSR 1 frame makes nothing. The refusals run on A. Windows D (cold) and C (prewarmed)
 * repeat the comparison with the radiance cascades on and an omni spot shadow (their pipeline variants).
 *
 * btbh_gpu_window_scene_prewarm_render_to_target (0.9.0): the same sequence rendered into a target with
 * btbh_gpu_window_scene_render_to_target (FSR 3 runs there as FSR 1, into a target twice the render size). Window E
 * runs it cold: its first render and the render the effects appear in make shaders and pipelines. Window F prewarms
 * for the target with every batch: the call makes what E's renders made, a second call nothing, no render of F
 * anything, and each of F's images is byte for byte E's. The call leaves F's own frames alone (its scene runtime
 * isn't made; its first frame still makes its pipelines). Windows G (cold) and H (prewarmed) repeat it with the
 * radiance cascades and an omni spot shadow. The refusals run on E.
 *
 * BTBH_SMOKE_BENCH=cold or =prewarm runs only that window's sequence at 1920x1080 (rendered at 960x540) and prints
 * each frame's times, to compare the two in separate processes (a process's first window pays for the driver's
 * caches); =target-cold or =target-prewarm presents four frames of the window first (its own runtime warm, as a
 * game's), then renders the sequence into a 960x540 target (rendered at 480x270), with or without the target's
 * prewarm. BTBH_PREWARM_SMOKE_DUMP and _REFERENCE write and compare window A's frames and window E's images (two
 * builds). It needs a GPU and the production shaders; with no device it is skipped (77). */
#undef NDEBUG

#include "btbh_gpu/btbh_gpu.h"
#include "btbh_gpu_smoke_meshes.h"
#include "btbh_gpu_internal.h"
#include "btbh_gpu_package_internal.h"

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_timer.h>

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SMOKE_SKIP 77
#define FRAME_COUNT 16u
/* The frame the effects first appear in. */
#define EFFECT_FRAME 6u
#define INSTANCE_FLOATS 24u

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

static uint32_t window_width = 320u;
static uint32_t window_height = 180u;
static uint32_t render_width = 160u;
static uint32_t render_height = 90u;

/* The settings a sequence runs with. */
typedef struct SmokeSettings {
    bool radiance_cascades;
    bool omni_spot_shadow;
} SmokeSettings;

typedef struct SmokeWindow {
    BTBHGPUWindow *window;
    BTBHGPUCustomMesh *cube;
    BTBHGPUModel *skinned;
    BTBHGPUAnimationLibrary *animation;
    /* Where each frame is copied (the frame's scene is finished by the copy, not a canvas). */
    BTBHGPUTexture *frame;
} SmokeWindow;

/* One frame's costs: CPU milliseconds of the scene present, of the copy and submit, of the wait for the GPU, and
   what it made. */
typedef struct FrameCost {
    double present_milliseconds;
    double submit_milliseconds;
    double gpu_wait_milliseconds;
    double total_milliseconds;
    BTBHGPUCreationStats made;
    /* The present's own split (its report and, with frame telemetry, the window's). */
    BTBHGPUSceneReport report;
    BTBHGPUSceneTelemetryReport telemetry;
} FrameCost;

static BTBHGPUCreationStats creation_stats(void) {
    BTBHGPUCreationStats stats;
    btbh_gpu_creation_stats(&stats);
    return stats;
}

static BTBHGPUCreationStats creation_since(const BTBHGPUCreationStats *before) {
    const BTBHGPUCreationStats now = creation_stats();
    BTBHGPUCreationStats made;
    made.shaders = now.shaders - before->shaders;
    made.graphics_pipelines = now.graphics_pipelines - before->graphics_pipelines;
    made.compute_pipelines = now.compute_pipelines - before->compute_pipelines;
    made.nanoseconds = now.nanoseconds - before->nanoseconds;
    return made;
}

static uint64_t made_count(const BTBHGPUCreationStats *made) {
    return made->shaders + made->graphics_pipelines + made->compute_pipelines;
}

static double milliseconds_since(uint64_t started_ticks) {
    return (double)(SDL_GetTicksNS() - started_ticks) / 1000000.0;
}

/* Column-major 4x4 helpers in the renderer's convention (right-handed, depth 0..1). */
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
    float near_plane;
    float far_plane;
    float fov_y;
} SmokeCamera;

static SmokeCamera look_at(
    const float eye[3],
    const float centre[3],
    float fov_y,
    float aspect,
    const float projection_override[16]
) {
    SmokeCamera camera;
    memset(&camera, 0, sizeof(camera));
    camera.near_plane = 0.1f;
    camera.far_plane = 50.0f;
    camera.fov_y = fov_y;
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
        const float h = 1.0f / tanf(0.5f * fov_y);
        const float r = camera.far_plane / (camera.near_plane - camera.far_plane);
        const float perspective[16] = {
            h / aspect, 0.0f, 0.0f, 0.0f,
            0.0f, h, 0.0f, 0.0f,
            0.0f, 0.0f, r, -1.0f,
            0.0f, 0.0f, r * camera.near_plane, 0.0f,
        };
        memcpy(projection, perspective, sizeof(projection));
    }
    multiply(projection, view, camera.view_projection);
    memcpy(camera.right, side, sizeof(side));
    memcpy(camera.up, up, sizeof(up));
    memcpy(camera.position, eye, sizeof(camera.position));
    return camera;
}

/* The sun from above and to the side, its shadow over the ground; with omni_spot_shadow also a point light's
   cube shadow at the centre. */
static BTBHGPUSceneLighting smoke_lighting(const SmokeSettings *settings) {
    BTBHGPUSceneLighting lighting;
    memset(&lighting, 0, sizeof(lighting));
    lighting.ambient_red = 1.0f;
    lighting.ambient_green = 1.0f;
    lighting.ambient_blue = 1.0f;
    lighting.ambient_energy = 0.25f;
    lighting.directional_energy = 2.0f;
    lighting.directional_red = 1.0f;
    lighting.directional_green = 0.95f;
    lighting.directional_blue = 0.9f;
    lighting.spot_range = 8.0f;
    lighting.spot_falloff = 1.0f;
    lighting.spot_inner_cutoff = 1.0f;
    lighting.spot_outer_cutoff = 0.0f;
    const float sun[3] = {4.0f, 10.0f, 3.0f};
    lighting.directional_x = -sun[0];
    lighting.directional_y = -sun[1];
    lighting.directional_z = -sun[2];
    const float centre[3] = {0.0f, 0.0f, 0.0f};
    const float rcp = 1.0f / 8.0f;
    const float range = 1.0f / (1.0f - 30.0f);
    const float orthographic[16] = {
        rcp, 0.0f, 0.0f, 0.0f,
        0.0f, rcp, 0.0f, 0.0f,
        0.0f, 0.0f, range, 0.0f,
        0.0f, 0.0f, range * 1.0f, 1.0f,
    };
    const SmokeCamera light = look_at(sun, centre, 1.0f, 1.0f, orthographic);
    memcpy(
        lighting.directional_shadow_view_projection,
        light.view_projection,
        sizeof(lighting.directional_shadow_view_projection)
    );
    lighting.directional_shadow_enabled = 1u;
    lighting.directional_shadow_softness = 1.0f;
    if (settings->omni_spot_shadow) {
        lighting.spot_position_x = 0.0f;
        lighting.spot_position_y = 2.0f;
        lighting.spot_position_z = 0.0f;
        lighting.spot_energy = 3.0f;
        lighting.spot_red = 1.0f;
        lighting.spot_green = 0.7f;
        lighting.spot_blue = 0.4f;
        lighting.spot_shadow_enabled = 1u;
        lighting.spot_shadow_is_omni = 1u;
        lighting.spot_shadow_near = 0.1f;
        lighting.spot_shadow_softness = 1.0f;
        static const float faces[6][2][3] = {
            {{1, 0, 0}, {0, -1, 0}}, {{-1, 0, 0}, {0, -1, 0}}, {{0, 1, 0}, {0, 0, 1}},
            {{0, -1, 0}, {0, 0, -1}}, {{0, 0, 1}, {0, -1, 0}}, {{0, 0, -1}, {0, -1, 0}},
        };
        const float eye[3] = {0.0f, 2.0f, 0.0f};
        for (uint32_t face = 0u; face < 6u; ++face) {
            const float target[3] = {
                eye[0] + faces[face][0][0], eye[1] + faces[face][0][1], eye[2] + faces[face][0][2],
            };
            SmokeCamera view = look_at(eye, target, 1.5707964f, 1.0f, NULL);
            /* (look_at's up is +y; the straight-up and straight-down faces take it from their forward instead.) */
            if (face == 2u || face == 3u) {
                const float tilted[3] = {target[0] + 0.001f, target[1], target[2] + 0.001f};
                view = look_at(eye, tilted, 1.5707964f, 1.0f, NULL);
            }
            memcpy(lighting.omni_shadow_view_projection[face], view.view_projection, sizeof(view.view_projection));
        }
    }
    return lighting;
}

static BTBHGPUScenePost smoke_post(const SmokeCamera *camera, uint8_t reset, const SmokeSettings *settings) {
    BTBHGPUScenePost post;
    memset(&post, 0, sizeof(post));
    post.tonemap_white = 1.0f;
    post.exposure = 1.0f;
    post.contrast = 1.0f;
    post.saturation = 1.0f;
    post.blur_curve = 1.0f;
    post.cas_sharpness = 0.3f;
    post.cas_enabled = 1u;
    post.tonemap_mode = BTBH_GPU_SCENE_TONEMAP_ACES;
    post.upscale_mode = BTBH_GPU_SCENE_UPSCALE_FSR3;
    post.ssao_enabled = 1u;
    post.ssr_intensity = 0.5f;
    post.ssr_max_roughness = 0.6f;
    post.ssr_max_distance = 6.0f;
    post.ssr_thickness = 0.3f;
    post.bloom_enabled = 1u;
    post.bloom_intensity = 0.4f;
    post.bloom_threshold = 1.0f;
    post.bloom_soft_knee = 0.5f;
    post.bloom_scatter = 0.7f;
    post.vignette_enabled = 1u;
    post.vignette_intensity = 0.3f;
    post.vignette_smoothness = 0.5f;
    post.fog_enabled = 1u;
    post.fog_color[0] = 0.5f;
    post.fog_color[1] = 0.55f;
    post.fog_color[2] = 0.6f;
    post.fog_density = 0.02f;
    post.fog_maximum_opacity = 0.6f;
    post.temporal_camera_near = camera->near_plane;
    post.temporal_camera_far = camera->far_plane;
    post.temporal_vertical_fov = camera->fov_y;
    post.temporal_frame_seconds = 1.0f / 60.0f;
    post.temporal_meters_per_unit = 1.0f;
    post.temporal_reactive_scale = 0.3f;
    post.temporal_reset = reset;
    if (settings->radiance_cascades) {
        static const BTBHGPUSceneRadianceEmitter emitter = {
            .stable_id = 1u,
            .position_x = 1.0f,
            .position_y = 1.0f,
            .position_z = 0.5f,
            .radius = 3.0f,
            .red = 1.0f,
            .green = 0.6f,
            .blue = 0.3f,
            .energy = 2.0f,
            .direct_energy = 1.0f,
            .direction_y = -1.0f,
            .inner_cutoff = 1.0f,
            .outer_cutoff = 0.0f,
            .shape = BTBH_GPU_SCENE_RADIANCE_EMITTER_DISK,
            .light_class = BTBH_GPU_SCENE_RADIANCE_EMITTER_CLASS_WORLD,
        };
        post.radiance_cascades.tier = BTBH_GPU_SCENE_RC_LOW;
        post.radiance_cascades.backend = BTBH_GPU_SCENE_RC_BACKEND_HOLOGRAPHIC;
        post.radiance_cascades.indirect_intensity = 1.0f;
        post.radiance_cascades.emitter_count = 1u;
        post.radiance_cascades.hero_omni_emitter_index = -1;
        post.radiance_cascades.emitters = &emitter;
    }
    return post;
}

/* Instances: 16 floats of transform (column-major), colour, custom. */
static void instance(float out[INSTANCE_FLOATS], float x, float y, float z, float sx, float sy, float sz, const float colour[4]) {
    memset(out, 0, INSTANCE_FLOATS * sizeof(float));
    out[0] = sx;
    out[5] = sy;
    out[10] = sz;
    out[12] = x;
    out[13] = y;
    out[14] = z;
    out[15] = 1.0f;
    memcpy(&out[16], colour, 4 * sizeof(float));
}

static BTBHGPUSceneGeometryBatch base_batch(uint32_t first, uint32_t count) {
    BTBHGPUSceneGeometryBatch batch;
    memset(&batch, 0, sizeof(batch));
    batch.material_slot = -1;
    batch.first_instance = first;
    batch.instance_count = count;
    batch.albedo_red = 255u;
    batch.albedo_green = 255u;
    batch.albedo_blue = 255u;
    batch.albedo_alpha = 255u;
    batch.uv_scale_x = 1.0f;
    batch.uv_scale_y = 1.0f;
    batch.roughness = 0.6f;
    batch.occlusion = 1.0f;
    batch.depth_compare = BTBH_GPU_SCENE_DEPTH_LESS;
    batch.cull_mode = BTBH_GPU_SCENE_CULL_BACK;
    /* No stencil: compare always, replace with reference 0. */
    batch.stencil_pass = BTBH_GPU_SCENE_STENCIL_REPLACE;
    batch.spot_shadow_visible = 1u;
    return batch;
}

static BTBHGPUSceneGeometryBatch cube_batch(const SmokeWindow *smoke, uint32_t first, uint32_t count) {
    BTBHGPUSceneGeometryBatch batch = base_batch(first, count);
    batch.kind = BTBH_GPU_SCENE_GEOMETRY_CUSTOM_MESH;
    batch.custom_mesh = smoke->cube;
    return batch;
}

/* The scene's instances: the same every frame but for the unit's, which walks. */
enum {
    INSTANCE_GROUND = 0,
    INSTANCE_BUILDINGS = 1, /* 3 */
    INSTANCE_UNIT = 4,
    INSTANCE_XRAY_WALL = 5,
    INSTANCE_DECAL = 6,
    INSTANCE_FLASH = 7,
    INSTANCE_SMOKE = 8,
    INSTANCE_XRAY_GHOST = 9,
    INSTANCE_REFRACTION = 10,
    INSTANCE_COUNT = 11
};

/* The batches drawn from the first frame (world) and from EFFECT_FRAME on (effects). */
#define WORLD_BATCH_COUNT 4u
#define EFFECT_BATCH_COUNT 5u
#define ALL_BATCH_COUNT (WORLD_BATCH_COUNT + EFFECT_BATCH_COUNT)

static uint32_t smoke_batches(const SmokeWindow *smoke, uint32_t frame, BTBHGPUSceneGeometryBatch out[ALL_BATCH_COUNT]) {
    /* The ground and the buildings: opaque, depth LESS, back faces culled. */
    out[0] = cube_batch(smoke, INSTANCE_GROUND, 1u);
    out[0].detail_parameters[0] = 0.2f;
    out[0].detail_parameters[2] = 2.0f;
    out[0].detail_parameters[3] = 1.0f;
    out[1] = cube_batch(smoke, INSTANCE_BUILDINGS, 3u);
    /* A skinned unit, drawn over what is level with it (LESS_EQUAL), both sides. */
    out[2] = base_batch(INSTANCE_UNIT, 1u);
    out[2].kind = BTBH_GPU_SCENE_GEOMETRY_MODEL;
    out[2].model = smoke->skinned;
    out[2].animation_library = smoke->animation;
    out[2].animation_index = 0u;
    out[2].animation_time = 0.05f * (float)frame;
    out[2].depth_compare = BTBH_GPU_SCENE_DEPTH_LESS_EQUAL;
    out[2].cull_mode = BTBH_GPU_SCENE_CULL_NONE;
    /* The x-ray mask: a wall that writes the stencil. */
    out[3] = cube_batch(smoke, INSTANCE_XRAY_WALL, 1u);
    out[3].stencil_reference = 1u;
    out[3].stencil_mask = 0xffu;
    out[3].stencil_compare = BTBH_GPU_SCENE_DEPTH_ALWAYS;
    out[3].stencil_pass = BTBH_GPU_SCENE_STENCIL_REPLACE;
    if (frame < EFFECT_FRAME) {
        return WORLD_BATCH_COUNT;
    }
    /* A decal on the ground (alpha, LESS_EQUAL, adds nothing to the reactive mask). */
    out[4] = cube_batch(smoke, INSTANCE_DECAL, 1u);
    out[4].transparency_enabled = 1u;
    out[4].albedo_alpha = 160u;
    out[4].depth_compare = BTBH_GPU_SCENE_DEPTH_LESS_EQUAL;
    out[4].cull_mode = BTBH_GPU_SCENE_CULL_NONE;
    out[4].reactive_mode = BTBH_GPU_SCENE_REACTIVE_NONE;
    /* A muzzle flash: additive and unlit (the fast unlit pipeline). */
    out[5] = cube_batch(smoke, INSTANCE_FLASH, 1u);
    out[5].transparency_enabled = 1u;
    out[5].blend_mode = BTBH_GPU_SCENE_BLEND_ADDITIVE;
    out[5].unlit_enabled = 1u;
    out[5].cull_mode = BTBH_GPU_SCENE_CULL_NONE;
    /* Smoke: a premultiplied soft particle. */
    out[6] = cube_batch(smoke, INSTANCE_SMOKE, 1u);
    out[6].transparency_enabled = 1u;
    out[6].blend_mode = BTBH_GPU_SCENE_BLEND_PREMULTIPLIED_ALPHA;
    out[6].surface_variant = BTBH_GPU_SCENE_SURFACE_SOFT_PARTICLE;
    out[6].albedo_alpha = 128u;
    out[6].cull_mode = BTBH_GPU_SCENE_CULL_NONE;
    /* The unit's ghost where the wall hides it: drawn where the stencil isn't the wall's. */
    out[7] = cube_batch(smoke, INSTANCE_XRAY_GHOST, 1u);
    out[7].transparency_enabled = 1u;
    out[7].albedo_alpha = 96u;
    out[7].depth_compare = BTBH_GPU_SCENE_DEPTH_ALWAYS;
    out[7].stencil_reference = 1u;
    out[7].stencil_mask = 0xffu;
    out[7].stencil_compare = BTBH_GPU_SCENE_DEPTH_NOT_EQUAL;
    out[7].stencil_pass = BTBH_GPU_SCENE_STENCIL_KEEP;
    out[7].cull_mode = BTBH_GPU_SCENE_CULL_NONE;
    /* A refraction strip (a shockwave). */
    out[8] = cube_batch(smoke, INSTANCE_REFRACTION, 1u);
    out[8].transparency_enabled = 1u;
    out[8].surface_variant = BTBH_GPU_SCENE_SURFACE_REFRACT;
    out[8].surface_parameters0[0] = 12.0f;
    out[8].surface_parameters0[1] = 0.3f;
    out[8].surface_parameters0[2] = 1.0f;
    out[8].surface_parameters0[3] = 0.5f;
    out[8].surface_time = 0.1f * (float)frame;
    out[8].cull_mode = BTBH_GPU_SCENE_CULL_NONE;
    return ALL_BATCH_COUNT;
}

static void smoke_instances(float out[INSTANCE_COUNT][INSTANCE_FLOATS], uint32_t frame) {
    const float grey[4] = {0.55f, 0.6f, 0.5f, 1.0f};
    const float stone[4] = {0.7f, 0.68f, 0.62f, 1.0f};
    const float red[4] = {0.9f, 0.25f, 0.2f, 1.0f};
    const float wall[4] = {0.4f, 0.45f, 0.5f, 1.0f};
    const float decal[4] = {0.1f, 0.1f, 0.1f, 0.8f};
    const float flash[4] = {3.0f, 2.2f, 1.0f, 1.0f};
    const float smoke[4] = {0.5f, 0.5f, 0.5f, 0.6f};
    const float ghost[4] = {0.3f, 0.8f, 1.0f, 0.5f};
    const float glass[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    instance(out[INSTANCE_GROUND], 0.0f, -0.05f, 0.0f, 10.0f, 0.1f, 10.0f, grey);
    instance(out[INSTANCE_BUILDINGS + 0], -2.0f, 0.75f, -1.5f, 1.2f, 1.5f, 1.2f, stone);
    instance(out[INSTANCE_BUILDINGS + 1], 2.2f, 1.0f, -2.0f, 1.0f, 2.0f, 1.4f, stone);
    instance(out[INSTANCE_BUILDINGS + 2], -1.8f, 0.5f, 2.0f, 1.5f, 1.0f, 1.0f, stone);
    instance(out[INSTANCE_UNIT], -0.5f + 0.04f * (float)frame, 0.0f, 0.3f, 0.8f, 0.8f, 0.8f, red);
    instance(out[INSTANCE_XRAY_WALL], 0.8f, 0.6f, 1.2f, 1.6f, 1.2f, 0.2f, wall);
    instance(out[INSTANCE_DECAL], 0.6f, 0.01f, -0.4f, 1.2f, 0.02f, 1.2f, decal);
    instance(out[INSTANCE_FLASH], 0.2f, 0.6f, 0.4f, 0.3f, 0.3f, 0.3f, flash);
    instance(out[INSTANCE_SMOKE], 0.0f, 0.8f, -0.3f, 0.8f, 0.8f, 0.8f, smoke);
    instance(out[INSTANCE_XRAY_GHOST], 0.8f, 0.4f, 1.6f, 0.4f, 0.8f, 0.4f, ghost);
    instance(out[INSTANCE_REFRACTION], -0.6f, 0.5f, 0.9f, 1.4f, 0.8f, 0.05f, glass);
}

/* A unit cube (-0.5..0.5), 24 floats a vertex: position, normal, uv, tangent, colour, joints, weights; its faces
   wound so that the renderer lights their outsides. */
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
    BTBHGPUCustomMesh *mesh = smoke_custom_mesh_create(
        window, vertices, (uint32_t)sizeof(vertices), 24u, indices, 36u, &report);
    if (mesh == NULL) {
        fprintf(stderr, "cube: %s\n", report.message);
    }
    assert(mesh != NULL);
    return mesh;
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
    return pixels;
}

static SmokeCamera smoke_camera(uint32_t frame) {
    const float angle = 0.6f + 0.02f * (float)frame;
    const float eye[3] = {6.0f * sinf(angle), 3.5f, 6.0f * cosf(angle)};
    const float centre[3] = {0.0f, 0.4f, 0.0f};
    return look_at(eye, centre, 0.8f, (float)window_width / (float)window_height, NULL);
}

static BTBHGPUResult present_frame(
    SmokeWindow *smoke,
    uint32_t frame,
    const SmokeSettings *settings,
    BTBHGPUSceneReport *report
) {
    const SmokeCamera camera = smoke_camera(frame);
    const BTBHGPUSceneLighting lighting = smoke_lighting(settings);
    const BTBHGPUScenePost post = smoke_post(&camera, frame == 0u ? 1u : 0u, settings);
    float instances[INSTANCE_COUNT][INSTANCE_FLOATS];
    smoke_instances(instances, frame);
    BTBHGPUSceneGeometryBatch batches[ALL_BATCH_COUNT];
    const uint32_t batch_count = smoke_batches(smoke, frame, batches);
    return btbh_gpu_window_scene_present_instances(
        smoke->window,
        shader_paths[0], shader_paths[1], shader_paths[2], shader_paths[3],
        shader_paths[4], shader_paths[5], shader_paths[6], shader_paths[7],
        shader_paths[8], shader_paths[9], shader_paths[10],
        SMOKE_SHADER_FORMAT,
        render_width,
        render_height,
        camera.view_projection,
        camera.right,
        camera.up,
        camera.position,
        &lighting,
        &post,
        &instances[0][0],
        INSTANCE_COUNT,
        INSTANCE_COUNT,
        batches,
        batch_count,
        40u, 60u, 90u, 255u,
        report
    );
}

/* A frame: the scene presented, copied into the window's frame texture and submitted, the GPU waited for. Its
   pixels are returned when `pixels` is given. */
static FrameCost run_frame(SmokeWindow *smoke, uint32_t frame, const SmokeSettings *settings, uint8_t **pixels) {
    FrameCost cost;
    memset(&cost, 0, sizeof(cost));
    const BTBHGPUCreationStats before = creation_stats();
    const uint64_t started = SDL_GetTicksNS();
    BTBHGPUSceneReport report;
    const BTBHGPUResult result = present_frame(smoke, frame, settings, &report);
    if (result != BTBH_GPU_OK) {
        fprintf(stderr, "frame %u: %s\n", frame, report.message);
    }
    assert(result == BTBH_GPU_OK);
    cost.present_milliseconds = milliseconds_since(started);
    cost.report = report;
    (void)btbh_gpu_window_scene_read_telemetry(smoke->window, &cost.telemetry);
    const uint64_t submit_started = SDL_GetTicksNS();
    BTBHGPUTextureReport copied;
    assert(btbh_gpu_window_copy_pending_scene_texture(smoke->window, smoke->frame, &copied) == BTBH_GPU_OK);
    cost.submit_milliseconds = milliseconds_since(submit_started);
    const uint64_t wait_started = SDL_GetTicksNS();
    assert(SDL_WaitForGPUIdle(smoke->window->device->gpu));
    cost.gpu_wait_milliseconds = milliseconds_since(wait_started);
    cost.total_milliseconds = milliseconds_since(started);
    cost.made = creation_since(&before);
    if (pixels != NULL) {
        *pixels = read_texture(smoke->window, smoke->frame);
    }
    return cost;
}

/* The frame's scene rendered into `target` (btbh_gpu_window_scene_render_to_target): FSR 3 runs as FSR 1 there. */
static BTBHGPUResult render_target_frame(
    SmokeWindow *smoke,
    BTBHGPUTexture *target,
    uint32_t frame,
    const SmokeSettings *settings,
    BTBHGPUSceneReport *report
) {
    const SmokeCamera camera = smoke_camera(frame);
    const BTBHGPUSceneLighting lighting = smoke_lighting(settings);
    const BTBHGPUScenePost post = smoke_post(&camera, frame == 0u ? 1u : 0u, settings);
    float instances[INSTANCE_COUNT][INSTANCE_FLOATS];
    smoke_instances(instances, frame);
    BTBHGPUSceneGeometryBatch batches[ALL_BATCH_COUNT];
    const uint32_t batch_count = smoke_batches(smoke, frame, batches);
    return btbh_gpu_window_scene_render_to_target(
        smoke->window,
        target,
        shader_paths[0], shader_paths[1], shader_paths[2], shader_paths[3],
        shader_paths[4], shader_paths[5], shader_paths[6], shader_paths[7],
        shader_paths[8], shader_paths[9], shader_paths[10],
        SMOKE_SHADER_FORMAT,
        render_width,
        render_height,
        camera.view_projection,
        camera.right,
        camera.up,
        camera.position,
        &lighting,
        &post,
        &instances[0][0],
        INSTANCE_COUNT,
        INSTANCE_COUNT,
        batches,
        NULL,
        NULL,
        batch_count,
        40u, 60u, 90u, 255u,
        report
    );
}

/* A render into `target` (the call submits its own work), the GPU waited for; its pixels returned when `pixels` is
   given. present_milliseconds is the call's CPU time. */
static FrameCost run_target_frame(
    SmokeWindow *smoke,
    BTBHGPUTexture *target,
    uint32_t frame,
    const SmokeSettings *settings,
    uint8_t **pixels
) {
    FrameCost cost;
    memset(&cost, 0, sizeof(cost));
    const BTBHGPUCreationStats before = creation_stats();
    const uint64_t started = SDL_GetTicksNS();
    BTBHGPUSceneReport report;
    const BTBHGPUResult result = render_target_frame(smoke, target, frame, settings, &report);
    if (result != BTBH_GPU_OK) {
        fprintf(stderr, "target frame %u: %s\n", frame, report.message);
    }
    assert(result == BTBH_GPU_OK);
    cost.present_milliseconds = milliseconds_since(started);
    cost.report = report;
    const uint64_t wait_started = SDL_GetTicksNS();
    assert(SDL_WaitForGPUIdle(smoke->window->device->gpu));
    cost.gpu_wait_milliseconds = milliseconds_since(wait_started);
    cost.total_milliseconds = milliseconds_since(started);
    cost.made = creation_since(&before);
    if (pixels != NULL) {
        *pixels = read_texture(smoke->window, target);
    }
    return cost;
}

static SmokeWindow open_window(BTBHGPUDevice *device, const char *title) {
    SmokeWindow smoke;
    memset(&smoke, 0, sizeof(smoke));
    const BTBHGPUWindowDescriptor descriptor = {
        .title = title,
        .width = window_width,
        .height = window_height,
        .hidden = 1,
        .high_pixel_density = 0,
    };
    BTBHGPUWindowReport report;
    memset(&report, 0, sizeof(report));
    smoke.window = btbh_gpu_window_create(&descriptor, &report);
    assert(smoke.window != NULL);
    assert(btbh_gpu_window_claim(device, smoke.window, &report) == BTBH_GPU_OK);
    smoke.cube = create_cube(smoke.window);
    BTBHGPUModelReport model_report;
    memset(&model_report, 0, sizeof(model_report));
    smoke.skinned = btbh_gpu_window_model_load(
        smoke.window, BTBH_GPU_SMOKE_FIXTURE_DIR "/skinned-quad.gltf", &model_report);
    if (smoke.skinned == NULL) {
        fprintf(stderr, "skinned-quad.gltf: %s\n", model_report.message);
    }
    assert(smoke.skinned != NULL);
    BTBHGPUAnimationLibraryReport animation_report;
    memset(&animation_report, 0, sizeof(animation_report));
    smoke.animation = btbh_gpu_animation_library_load(
        BTBH_GPU_SMOKE_FIXTURE_DIR "/skinned-quad.gltf", &animation_report);
    if (smoke.animation == NULL) {
        fprintf(stderr, "skinned-quad.gltf animation: %s\n", animation_report.message);
    }
    assert(smoke.animation != NULL);
    BTBHGPUTextureReport texture_report;
    smoke.frame = btbh_gpu_window_render_target_create(
        smoke.window, window_width, window_height, BTBH_GPU_TEXTURE_FILTER_LINEAR, &texture_report);
    assert(smoke.frame != NULL);
    return smoke;
}

static void close_window(SmokeWindow *smoke) {
    btbh_gpu_animation_library_destroy(smoke->animation);
    btbh_gpu_window_destroy(smoke->window);
    memset(smoke, 0, sizeof(*smoke));
}

static void print_frame(const char *label, uint32_t frame, const FrameCost *cost) {
    printf(
        "%s frame %2u: total %7.2f ms (present %7.2f, submit %6.2f, gpu wait %6.2f); made %llu shaders, "
        "%llu graphics + %llu compute pipelines in %.2f ms\n",
        label,
        frame,
        cost->total_milliseconds,
        cost->present_milliseconds,
        cost->submit_milliseconds,
        cost->gpu_wait_milliseconds,
        (unsigned long long)cost->made.shaders,
        (unsigned long long)cost->made.graphics_pipelines,
        (unsigned long long)cost->made.compute_pipelines,
        (double)cost->made.nanoseconds / 1000000.0
    );
    if (SDL_getenv("BTBH_SMOKE_BENCH") != NULL) {
        printf(
            "    present split: setup %.2f, bones %.2f, pipelines %.2f, buffers %.2f | upload %.2f, shadow %.2f, "
            "targets %.2f, geometry %.2f, post %.2f\n",
            cost->telemetry.cpu_prepare_setup_milliseconds,
            cost->telemetry.cpu_prepare_bones_milliseconds,
            cost->telemetry.cpu_prepare_pipelines_milliseconds,
            cost->telemetry.cpu_prepare_buffers_milliseconds,
            cost->report.cpu_upload_milliseconds,
            cost->report.cpu_shadow_encode_milliseconds,
            cost->report.cpu_target_ensure_milliseconds,
            cost->report.cpu_geometry_encode_milliseconds,
            cost->report.cpu_post_encode_milliseconds
        );
    }
}

static bool load_shader_paths(void) {
    for (int index = 0; index < 11; ++index) {
        (void)snprintf(
            shader_paths[index],
            sizeof(shader_paths[index]),
            "%s/%s%s",
            BTBH_GPU_SMOKE_SHADER_DIR,
            shader_names[index],
            SMOKE_SHADER_EXTENSION
        );
        FILE *file = fopen(shader_paths[index], "rb");
        if (file == NULL) {
            fprintf(stderr, "skipped: no shader %s\n", shader_paths[index]);
            return false;
        }
        fclose(file);
    }
    return true;
}

static BTBHGPUDevice *create_device(bool debug_mode) {
    const BTBHGPUDeviceDescriptor device_descriptor = {
        .backend = SMOKE_BACKEND,
        .required_features = BTBH_GPU_REQUIRED_FEATURES,
        .debug_mode = debug_mode ? 1u : 0u,
    };
    BTBHGPUDeviceReport device_report;
    memset(&device_report, 0, sizeof(device_report));
    BTBHGPUDevice *device = btbh_gpu_device_create(&device_descriptor, &device_report);
    if (device == NULL) {
        fprintf(stderr, "skipped: no device (%s)\n", device_report.message);
    }
    return device;
}

/* Prewarms the window with every batch of the sequence (the world's and the effects') and its settings. */
static BTBHGPUResult prewarm_with(
    SmokeWindow *smoke,
    const SmokeSettings *settings,
    const BTBHGPUScenePost *post_override,
    BTBHGPUSceneReport *report
) {
    const SmokeCamera camera = smoke_camera(0u);
    const BTBHGPUSceneLighting lighting = smoke_lighting(settings);
    const BTBHGPUScenePost post = post_override != NULL ? *post_override : smoke_post(&camera, 1u, settings);
    BTBHGPUSceneGeometryBatch batches[ALL_BATCH_COUNT];
    const uint32_t batch_count = smoke_batches(smoke, EFFECT_FRAME, batches);
    assert(batch_count == ALL_BATCH_COUNT);
    return btbh_gpu_window_scene_prewarm(
        smoke->window,
        shader_paths[0], shader_paths[1], shader_paths[2], shader_paths[3],
        shader_paths[4], shader_paths[5], shader_paths[6], shader_paths[7],
        shader_paths[8], shader_paths[9], shader_paths[10],
        SMOKE_SHADER_FORMAT,
        render_width,
        render_height,
        &lighting,
        &post,
        batches,
        batch_count,
        report
    );
}

/* Prewarms and checks that it succeeded; what it made comes back. */
static BTBHGPUCreationStats prewarm(SmokeWindow *smoke, const SmokeSettings *settings, double *milliseconds) {
    const BTBHGPUCreationStats before = creation_stats();
    const uint64_t started = SDL_GetTicksNS();
    BTBHGPUSceneReport report;
    const BTBHGPUResult result = prewarm_with(smoke, settings, NULL, &report);
    if (result != BTBH_GPU_OK) {
        fprintf(stderr, "prewarm: %s\n", report.message);
    }
    assert(result == BTBH_GPU_OK);
    if (milliseconds != NULL) {
        *milliseconds = milliseconds_since(started);
    }
    assert(report.drawable_width == window_width && report.drawable_height == window_height);
    assert(report.batch_count == ALL_BATCH_COUNT);
    /* A cube a batch, and the skinned quad's one primitive. */
    assert(report.prepared_geometry_count == ALL_BATCH_COUNT);
    return creation_since(&before);
}

static void print_made(const char *label, const BTBHGPUCreationStats *made, double milliseconds) {
    printf(
        "%s: %.2f ms; made %llu shaders, %llu graphics + %llu compute pipelines in %.2f ms\n",
        label,
        milliseconds,
        (unsigned long long)made->shaders,
        (unsigned long long)made->graphics_pipelines,
        (unsigned long long)made->compute_pipelines,
        (double)made->nanoseconds / 1000000.0
    );
}

#if defined(BTBH_GPU_HAS_SCENE_RENDER_TO_TARGET_PREWARM)
/* Prewarms the window's renders into `target` with every batch of the sequence and its settings. */
static BTBHGPUResult prewarm_target_with(
    SmokeWindow *smoke,
    BTBHGPUTexture *target,
    const SmokeSettings *settings,
    BTBHGPUSceneReport *report
) {
    const SmokeCamera camera = smoke_camera(0u);
    const BTBHGPUSceneLighting lighting = smoke_lighting(settings);
    const BTBHGPUScenePost post = smoke_post(&camera, 1u, settings);
    BTBHGPUSceneGeometryBatch batches[ALL_BATCH_COUNT];
    const uint32_t batch_count = smoke_batches(smoke, EFFECT_FRAME, batches);
    assert(batch_count == ALL_BATCH_COUNT);
    return btbh_gpu_window_scene_prewarm_render_to_target(
        smoke->window,
        target,
        shader_paths[0], shader_paths[1], shader_paths[2], shader_paths[3],
        shader_paths[4], shader_paths[5], shader_paths[6], shader_paths[7],
        shader_paths[8], shader_paths[9], shader_paths[10],
        SMOKE_SHADER_FORMAT,
        render_width,
        render_height,
        &lighting,
        &post,
        batches,
        batch_count,
        report
    );
}

/* Prewarms for the target and checks that it succeeded; what it made comes back. */
static BTBHGPUCreationStats prewarm_target(
    SmokeWindow *smoke,
    BTBHGPUTexture *target,
    const SmokeSettings *settings,
    double *milliseconds
) {
    const BTBHGPUCreationStats before = creation_stats();
    const uint64_t started = SDL_GetTicksNS();
    BTBHGPUSceneReport report;
    const BTBHGPUResult result = prewarm_target_with(smoke, target, settings, &report);
    if (result != BTBH_GPU_OK) {
        fprintf(stderr, "target prewarm: %s\n", report.message);
    }
    assert(result == BTBH_GPU_OK);
    if (milliseconds != NULL) {
        *milliseconds = milliseconds_since(started);
    }
    /* The target's size, and what the renders run: FSR 3 runs as FSR 1 into a target bigger than the render. */
    assert(report.drawable_width == target->width && report.drawable_height == target->height);
    assert(report.batch_count == ALL_BATCH_COUNT);
    assert(report.prepared_geometry_count == ALL_BATCH_COUNT);
    assert(report.upscale_mode == BTBH_GPU_SCENE_UPSCALE_FSR1);
    return creation_since(&before);
}
#endif

/* BTBH_SMOKE_BENCH=target-cold or =target-prewarm: four frames of the window, then the sequence into a target. */
static int run_target_bench(const char *mode) {
    const bool prewarmed = strcmp(mode, "target-prewarm") == 0;
#if !defined(BTBH_GPU_HAS_SCENE_RENDER_TO_TARGET_PREWARM)
    if (prewarmed) {
        fprintf(stderr, "BTBH_SMOKE_BENCH=target-prewarm needs btbh_gpu_window_scene_prewarm_render_to_target\n");
        return 1;
    }
#endif
    window_width = 1920u;
    window_height = 1080u;
    render_width = 960u;
    render_height = 540u;
    BTBHGPUDevice *device = create_device(false);
    if (device == NULL) {
        return SMOKE_SKIP;
    }
    SmokeWindow smoke = open_window(device, "BTBH target prewarm bench");
    const SmokeSettings settings = {0};
    for (uint32_t frame = 0u; frame < 4u; ++frame) {
        const FrameCost cost = run_frame(&smoke, frame, &settings, NULL);
        print_frame("window", frame, &cost);
    }
    /* The target: half the window's size, rendered at half its own (FSR 1 enlarges it). */
    render_width = 480u;
    render_height = 270u;
    BTBHGPUTextureReport texture_report;
    BTBHGPUTexture *target = btbh_gpu_window_render_target_create(
        smoke.window, 960u, 540u, BTBH_GPU_TEXTURE_FILTER_LINEAR, &texture_report);
    assert(target != NULL);
#if defined(BTBH_GPU_HAS_SCENE_RENDER_TO_TARGET_PREWARM)
    if (prewarmed) {
        double milliseconds = 0.0;
        const BTBHGPUCreationStats made = prewarm_target(&smoke, target, &settings, &milliseconds);
        print_made("target prewarm", &made, milliseconds);
    }
#endif
    for (uint32_t frame = 0u; frame < FRAME_COUNT; ++frame) {
        const FrameCost cost = run_target_frame(&smoke, target, frame, &settings, NULL);
        print_frame(mode, frame, &cost);
    }
    btbh_gpu_texture_destroy(target);
    close_window(&smoke);
    btbh_gpu_device_destroy(device);
    return 0;
}

/* BTBH_SMOKE_BENCH: one sequence at 1080p, its frames' times printed. */
static int run_bench(const char *mode) {
    if (strcmp(mode, "target-cold") == 0 || strcmp(mode, "target-prewarm") == 0) {
        return run_target_bench(mode);
    }
    const bool prewarmed = strcmp(mode, "prewarm") == 0;
    if (!prewarmed && strcmp(mode, "cold") != 0) {
        fprintf(stderr, "BTBH_SMOKE_BENCH: cold, prewarm, target-cold or target-prewarm\n");
        return 1;
    }
    window_width = 1920u;
    window_height = 1080u;
    render_width = 960u;
    render_height = 540u;
    BTBHGPUDevice *device = create_device(false);
    if (device == NULL) {
        return SMOKE_SKIP;
    }
    SmokeWindow smoke = open_window(device, "BTBH prewarm bench");
    btbh_gpu_window_set_frame_telemetry_enabled(smoke.window, 1u);
    const SmokeSettings settings = {0};
    if (prewarmed) {
        double milliseconds = 0.0;
        const BTBHGPUCreationStats made = prewarm(&smoke, &settings, &milliseconds);
        print_made("prewarm", &made, milliseconds);
    }
    for (uint32_t frame = 0u; frame < FRAME_COUNT; ++frame) {
        const FrameCost cost = run_frame(&smoke, frame, &settings, NULL);
        print_frame(mode, frame, &cost);
    }
    close_window(&smoke);
    btbh_gpu_device_destroy(device);
    return 0;
}

/* Runs frames [first, first + count) and keeps their pixels; checks what each made. */
static void run_sequence(
    SmokeWindow *smoke,
    const char *label,
    const SmokeSettings *settings,
    uint32_t first,
    uint32_t count,
    bool expect_nothing_made,
    uint8_t **pixels,
    BTBHGPUCreationStats *made_per_frame
) {
    for (uint32_t index = 0u; index < count; ++index) {
        const uint32_t frame = first + index;
        const FrameCost cost = run_frame(smoke, frame, settings, &pixels[index]);
        print_frame(label, frame, &cost);
        made_per_frame[index] = cost.made;
        if (expect_nothing_made) {
            assert(made_count(&cost.made) == 0u);
        }
        if (settings->radiance_cascades) {
            if (cost.report.radiance_cascades.active_tier != BTBH_GPU_SCENE_RC_LOW) {
                fprintf(stderr, "%s frame %u: cascades requested %d, active %d, pending %d, fallback %d\n", label, frame,
                    (int)cost.report.radiance_cascades.requested_tier, (int)cost.report.radiance_cascades.active_tier,
                    (int)cost.report.radiance_cascades.pending_tier, (int)cost.report.radiance_cascades.fallback_reason);
            }
            assert(cost.report.radiance_cascades.active_tier == BTBH_GPU_SCENE_RC_LOW);
        }
    }
}

/* Renders frames [first, first + count) into `target` and keeps their pixels; checks what each made. */
static void run_target_sequence(
    SmokeWindow *smoke,
    BTBHGPUTexture *target,
    const char *label,
    const SmokeSettings *settings,
    uint32_t first,
    uint32_t count,
    bool expect_nothing_made,
    uint8_t **pixels,
    BTBHGPUCreationStats *made_per_frame
) {
    for (uint32_t index = 0u; index < count; ++index) {
        const uint32_t frame = first + index;
        const FrameCost cost = run_target_frame(smoke, target, frame, settings, &pixels[index]);
        print_frame(label, frame, &cost);
        made_per_frame[index] = cost.made;
        if (expect_nothing_made) {
            assert(made_count(&cost.made) == 0u);
        }
        assert(cost.report.upscale_mode == BTBH_GPU_SCENE_UPSCALE_FSR1);
        if (settings->radiance_cascades) {
            assert(cost.report.radiance_cascades.active_tier == BTBH_GPU_SCENE_RC_LOW);
        }
    }
}

static void assert_same_frames(uint8_t **a, uint8_t **b, uint32_t count, const char *what) {
    const size_t size = (size_t)window_width * window_height * 4u;
    for (uint32_t index = 0u; index < count; ++index) {
        if (memcmp(a[index], b[index], size) != 0) {
            size_t differing = 0u;
            for (size_t byte = 0u; byte < size; ++byte) {
                differing += a[index][byte] != b[index][byte] ? 1u : 0u;
            }
            fprintf(stderr, "%s: frame %u differs in %zu bytes\n", what, index, differing);
        }
        assert(memcmp(a[index], b[index], size) == 0);
    }
}

/* BTBH_PREWARM_SMOKE_DUMP=<dir> writes the cold window's frames (<prefix>-NN.rgba, RGBA8 rows from the top: frame-
   for window A's frames, target-frame- for window E's renders into its target); BTBH_PREWARM_SMOKE_REFERENCE=<dir>
   compares them with that directory's, which must match to the byte (two builds, two releases). */
static void dump_or_compare_frames(uint8_t **pixels, uint32_t count, const char *prefix) {
    const char *dump = SDL_getenv("BTBH_PREWARM_SMOKE_DUMP");
    const char *reference = SDL_getenv("BTBH_PREWARM_SMOKE_REFERENCE");
    const size_t size = (size_t)window_width * window_height * 4u;
    for (uint32_t index = 0u; index < count; ++index) {
        char path[1100];
        if (dump != NULL && dump[0] != '\0') {
            (void)snprintf(path, sizeof(path), "%s/%s-%02u.rgba", dump, prefix, index);
            FILE *file = fopen(path, "wb");
            assert(file != NULL);
            assert(fwrite(pixels[index], 1u, size, file) == size);
            fclose(file);
        }
        if (reference != NULL && reference[0] != '\0') {
            (void)snprintf(path, sizeof(path), "%s/%s-%02u.rgba", reference, prefix, index);
            FILE *file = fopen(path, "rb");
            assert(file != NULL);
            uint8_t *expected = malloc(size);
            assert(expected != NULL);
            assert(fread(expected, 1u, size, file) == size);
            fclose(file);
            size_t differing = 0u;
            for (size_t byte = 0u; byte < size; ++byte) {
                differing += expected[byte] != pixels[index][byte] ? 1u : 0u;
            }
            printf("reference %s %2u: %zu bytes differ\n", prefix, index, differing);
            assert(differing == 0u);
            free(expected);
        }
    }
}

static void free_frames(uint8_t **pixels, uint32_t count) {
    for (uint32_t index = 0u; index < count; ++index) {
        free(pixels[index]);
        pixels[index] = NULL;
    }
}

/* The refusals: each leaves the window as it was. */
static void check_refusals(SmokeWindow *smoke, SmokeWindow *other) {
    const SmokeSettings plain = {0};
    const SmokeCamera camera = smoke_camera(0u);
    const BTBHGPUSceneLighting lighting = smoke_lighting(&plain);
    const BTBHGPUScenePost post = smoke_post(&camera, 1u, &plain);
    BTBHGPUSceneGeometryBatch batches[ALL_BATCH_COUNT];
    const uint32_t batch_count = smoke_batches(smoke, EFFECT_FRAME, batches);
    BTBHGPUSceneReport report;
    const BTBHGPUCreationStats before = creation_stats();
#define PREWARM_CALL(window_, paths_, format_, width_, height_, lighting_, post_, batches_, count_, report_) \
    btbh_gpu_window_scene_prewarm( \
        (window_), \
        (paths_)[0], (paths_)[1], (paths_)[2], (paths_)[3], (paths_)[4], (paths_)[5], \
        (paths_)[6], (paths_)[7], (paths_)[8], (paths_)[9], (paths_)[10], \
        (format_), (width_), (height_), (lighting_), (post_), (batches_), (count_), (report_))
    const char *paths[11];
    for (int index = 0; index < 11; ++index) {
        paths[index] = shader_paths[index];
    }
    /* No report, no window. */
    assert(PREWARM_CALL(smoke->window, paths, SMOKE_SHADER_FORMAT, render_width, render_height, &lighting, &post,
        batches, batch_count, NULL) == BTBH_GPU_INVALID_ARGUMENT);
    assert(PREWARM_CALL(NULL, paths, SMOKE_SHADER_FORMAT, render_width, render_height, &lighting, &post,
        batches, batch_count, &report) == BTBH_GPU_INVALID_ARGUMENT);
    assert(report.result == BTBH_GPU_INVALID_ARGUMENT && strstr(report.message, "prewarm") != NULL);
    /* Each shader path NULL in turn. */
    for (int missing = 0; missing < 11; ++missing) {
        const char *holes[11];
        for (int index = 0; index < 11; ++index) {
            holes[index] = index == missing ? NULL : shader_paths[index];
        }
        assert(PREWARM_CALL(smoke->window, holes, SMOKE_SHADER_FORMAT, render_width, render_height, &lighting,
            &post, batches, batch_count, &report) == BTBH_GPU_INVALID_ARGUMENT);
    }
    /* A shader format, a size, the lighting, the post, the batches. */
    assert(PREWARM_CALL(smoke->window, paths, BTBH_GPU_SHADER_FORMAT_INVALID, render_width, render_height,
        &lighting, &post, batches, batch_count, &report) == BTBH_GPU_INVALID_ARGUMENT);
    assert(PREWARM_CALL(smoke->window, paths, SMOKE_SHADER_FORMAT, 0u, render_height, &lighting, &post,
        batches, batch_count, &report) == BTBH_GPU_INVALID_ARGUMENT);
    assert(PREWARM_CALL(smoke->window, paths, SMOKE_SHADER_FORMAT, render_width, 0u, &lighting, &post,
        batches, batch_count, &report) == BTBH_GPU_INVALID_ARGUMENT);
    assert(PREWARM_CALL(smoke->window, paths, SMOKE_SHADER_FORMAT, render_width, render_height, NULL, &post,
        batches, batch_count, &report) == BTBH_GPU_INVALID_ARGUMENT);
    assert(PREWARM_CALL(smoke->window, paths, SMOKE_SHADER_FORMAT, render_width, render_height, &lighting, NULL,
        batches, batch_count, &report) == BTBH_GPU_INVALID_ARGUMENT);
    BTBHGPUScenePost bad_post = post;
    bad_post.exposure = -1.0f;
    assert(PREWARM_CALL(smoke->window, paths, SMOKE_SHADER_FORMAT, render_width, render_height, &lighting,
        &bad_post, batches, batch_count, &report) == BTBH_GPU_INVALID_ARGUMENT);
    BTBHGPUSceneLighting bad_lighting = lighting;
    bad_lighting.ambient_energy = NAN;
    assert(PREWARM_CALL(smoke->window, paths, SMOKE_SHADER_FORMAT, render_width, render_height, &bad_lighting,
        &post, batches, batch_count, &report) == BTBH_GPU_INVALID_ARGUMENT);
    assert(PREWARM_CALL(smoke->window, paths, SMOKE_SHADER_FORMAT, render_width, render_height, &lighting, &post,
        NULL, batch_count, &report) == BTBH_GPU_INVALID_ARGUMENT);
    assert(PREWARM_CALL(smoke->window, paths, SMOKE_SHADER_FORMAT, render_width, render_height, &lighting, &post,
        batches, 0u, &report) == BTBH_GPU_INVALID_ARGUMENT);
    /* A batch the present would refuse: an unknown blend, a stencil the renderer has no variant for, an unknown
       flag, a mesh of another window. */
    BTBHGPUSceneGeometryBatch bad[ALL_BATCH_COUNT];
    memcpy(bad, batches, sizeof(bad));
    bad[4].blend_mode = BTBH_GPU_SCENE_BLEND_COUNT;
    assert(PREWARM_CALL(smoke->window, paths, SMOKE_SHADER_FORMAT, render_width, render_height, &lighting, &post,
        bad, batch_count, &report) == BTBH_GPU_INVALID_ARGUMENT);
    memcpy(bad, batches, sizeof(bad));
    bad[1].stencil_compare = BTBH_GPU_SCENE_DEPTH_LESS;
    assert(PREWARM_CALL(smoke->window, paths, SMOKE_SHADER_FORMAT, render_width, render_height, &lighting, &post,
        bad, batch_count, &report) == BTBH_GPU_INVALID_ARGUMENT);
    memcpy(bad, batches, sizeof(bad));
    bad[0].flags = 0x80u;
    assert(PREWARM_CALL(smoke->window, paths, SMOKE_SHADER_FORMAT, render_width, render_height, &lighting, &post,
        bad, batch_count, &report) == BTBH_GPU_INVALID_ARGUMENT);
    memcpy(bad, batches, sizeof(bad));
    bad[1].custom_mesh = other->cube;
    assert(PREWARM_CALL(smoke->window, paths, SMOKE_SHADER_FORMAT, render_width, render_height, &lighting, &post,
        bad, batch_count, &report) == BTBH_GPU_INVALID_ARGUMENT);
    {
        const BTBHGPUCreationStats made = creation_since(&before);
        assert(made_count(&made) == 0u);
    }
    /* While a scene waits for its canvas: refused, and the scene still copies. */
    BTBHGPUSceneReport present_report;
    assert(present_frame(smoke, EFFECT_FRAME, &plain, &present_report) == BTBH_GPU_OK);
    assert(PREWARM_CALL(smoke->window, paths, SMOKE_SHADER_FORMAT, render_width, render_height, &lighting, &post,
        batches, batch_count, &report) == BTBH_GPU_INVALID_ARGUMENT);
    assert(strstr(report.message, "canvas") != NULL);
    BTBHGPUTextureReport copied;
    assert(btbh_gpu_window_copy_pending_scene_texture(smoke->window, smoke->frame, &copied) == BTBH_GPU_OK);
    /* A shader that can't be loaded, for a pipeline the window lacks (back faces kept, which no batch had yet):
       the present's error, with the path. */
    char missing_vertex[1100];
    (void)snprintf(missing_vertex, sizeof(missing_vertex), "%s/missing_model.vert%s",
        BTBH_GPU_SMOKE_SHADER_DIR, SMOKE_SHADER_EXTENSION);
    const char *broken[11];
    for (int index = 0; index < 11; ++index) {
        broken[index] = index == 0 ? missing_vertex : shader_paths[index];
    }
    memcpy(bad, batches, sizeof(bad));
    bad[0].cull_mode = BTBH_GPU_SCENE_CULL_FRONT;
    assert(PREWARM_CALL(smoke->window, broken, SMOKE_SHADER_FORMAT, render_width, render_height, &lighting, &post,
        bad, batch_count, &report) == BTBH_GPU_INVALID_ARGUMENT);
    assert(strstr(report.message, "missing_model.vert") != NULL);
#undef PREWARM_CALL
}

#if defined(BTBH_GPU_HAS_SCENE_RENDER_TO_TARGET_PREWARM)
/* The target prewarm's refusals: each leaves the window as it was and makes nothing. */
static void check_target_refusals(SmokeWindow *smoke, SmokeWindow *other, BTBHGPUTexture *target) {
    const SmokeSettings plain = {0};
    const SmokeCamera camera = smoke_camera(0u);
    const BTBHGPUSceneLighting lighting = smoke_lighting(&plain);
    const BTBHGPUScenePost post = smoke_post(&camera, 1u, &plain);
    BTBHGPUSceneGeometryBatch batches[ALL_BATCH_COUNT];
    const uint32_t batch_count = smoke_batches(smoke, EFFECT_FRAME, batches);
    BTBHGPUSceneReport report;
    BTBHGPUSceneRuntime *const window_runtime = smoke->window->scene_runtime;
    BTBHGPUSceneRuntime *const aside_runtime = smoke->window->aside_scene_runtime;
    const BTBHGPUCreationStats before = creation_stats();
#define TARGET_PREWARM_CALL(window_, target_, paths_, format_, width_, height_, lighting_, post_, batches_, count_, \
    report_) \
    btbh_gpu_window_scene_prewarm_render_to_target( \
        (window_), (target_), \
        (paths_)[0], (paths_)[1], (paths_)[2], (paths_)[3], (paths_)[4], (paths_)[5], \
        (paths_)[6], (paths_)[7], (paths_)[8], (paths_)[9], (paths_)[10], \
        (format_), (width_), (height_), (lighting_), (post_), (batches_), (count_), (report_))
    const char *paths[11];
    for (int index = 0; index < 11; ++index) {
        paths[index] = shader_paths[index];
    }
    /* No report, no window, no target. */
    assert(TARGET_PREWARM_CALL(smoke->window, target, paths, SMOKE_SHADER_FORMAT, render_width, render_height,
        &lighting, &post, batches, batch_count, NULL) == BTBH_GPU_INVALID_ARGUMENT);
    assert(TARGET_PREWARM_CALL(NULL, target, paths, SMOKE_SHADER_FORMAT, render_width, render_height, &lighting,
        &post, batches, batch_count, &report) == BTBH_GPU_INVALID_ARGUMENT);
    assert(report.result == BTBH_GPU_INVALID_ARGUMENT && strstr(report.message, "prewarm") != NULL);
    assert(TARGET_PREWARM_CALL(smoke->window, NULL, paths, SMOKE_SHADER_FORMAT, render_width, render_height,
        &lighting, &post, batches, batch_count, &report) == BTBH_GPU_INVALID_ARGUMENT);
    assert(strstr(report.message, "render target") != NULL);
    /* Another window's target, a texture that is no render target, an array. */
    assert(TARGET_PREWARM_CALL(smoke->window, other->frame, paths, SMOKE_SHADER_FORMAT, render_width, render_height,
        &lighting, &post, batches, batch_count, &report) == BTBH_GPU_INVALID_ARGUMENT);
    {
        const uint8_t texels[16] = {255u, 255u, 255u, 255u, 255u, 255u, 255u, 255u,
            255u, 255u, 255u, 255u, 255u, 255u, 255u, 255u};
        BTBHGPUTextureReport texture_report;
        BTBHGPUTexture *plain_texture = btbh_gpu_window_texture_create_rgba8(smoke->window, texels, 2u, 2u,
            BTBH_GPU_TEXTURE_FILTER_LINEAR, BTBH_GPU_TEXTURE_WRAP_REPEAT, &texture_report);
        assert(plain_texture != NULL);
        assert(TARGET_PREWARM_CALL(smoke->window, plain_texture, paths, SMOKE_SHADER_FORMAT, render_width,
            render_height, &lighting, &post, batches, batch_count, &report) == BTBH_GPU_INVALID_ARGUMENT);
        btbh_gpu_texture_destroy(plain_texture);
        const uint8_t *layers[2] = {texels, texels};
        BTBHGPUTexture *array = btbh_gpu_window_texture_array_create_rgba8_ex(smoke->window, layers, 2u, 2u, 2u, 0u,
            BTBH_GPU_TEXTURE_FILTER_LINEAR, BTBH_GPU_TEXTURE_WRAP_REPEAT, BTBH_GPU_TEXTURE_COLOR_SPACE_LINEAR,
            &texture_report);
        assert(array != NULL);
        assert(TARGET_PREWARM_CALL(smoke->window, array, paths, SMOKE_SHADER_FORMAT, render_width, render_height,
            &lighting, &post, batches, batch_count, &report) == BTBH_GPU_INVALID_ARGUMENT);
        btbh_gpu_texture_destroy(array);
    }
    /* Each shader path NULL in turn; the format, a size, the lighting, the post, the batches. */
    for (int missing = 0; missing < 11; ++missing) {
        const char *holes[11];
        for (int index = 0; index < 11; ++index) {
            holes[index] = index == missing ? NULL : shader_paths[index];
        }
        assert(TARGET_PREWARM_CALL(smoke->window, target, holes, SMOKE_SHADER_FORMAT, render_width, render_height,
            &lighting, &post, batches, batch_count, &report) == BTBH_GPU_INVALID_ARGUMENT);
    }
    assert(TARGET_PREWARM_CALL(smoke->window, target, paths, BTBH_GPU_SHADER_FORMAT_INVALID, render_width,
        render_height, &lighting, &post, batches, batch_count, &report) == BTBH_GPU_INVALID_ARGUMENT);
    assert(TARGET_PREWARM_CALL(smoke->window, target, paths, SMOKE_SHADER_FORMAT, 0u, render_height, &lighting,
        &post, batches, batch_count, &report) == BTBH_GPU_INVALID_ARGUMENT);
    assert(TARGET_PREWARM_CALL(smoke->window, target, paths, SMOKE_SHADER_FORMAT, render_width, 0u, &lighting,
        &post, batches, batch_count, &report) == BTBH_GPU_INVALID_ARGUMENT);
    assert(TARGET_PREWARM_CALL(smoke->window, target, paths, SMOKE_SHADER_FORMAT, render_width, render_height, NULL,
        &post, batches, batch_count, &report) == BTBH_GPU_INVALID_ARGUMENT);
    assert(TARGET_PREWARM_CALL(smoke->window, target, paths, SMOKE_SHADER_FORMAT, render_width, render_height,
        &lighting, NULL, batches, batch_count, &report) == BTBH_GPU_INVALID_ARGUMENT);
    BTBHGPUScenePost bad_post = post;
    bad_post.exposure = -1.0f;
    assert(TARGET_PREWARM_CALL(smoke->window, target, paths, SMOKE_SHADER_FORMAT, render_width, render_height,
        &lighting, &bad_post, batches, batch_count, &report) == BTBH_GPU_INVALID_ARGUMENT);
    assert(TARGET_PREWARM_CALL(smoke->window, target, paths, SMOKE_SHADER_FORMAT, render_width, render_height,
        &lighting, &post, NULL, batch_count, &report) == BTBH_GPU_INVALID_ARGUMENT);
    assert(TARGET_PREWARM_CALL(smoke->window, target, paths, SMOKE_SHADER_FORMAT, render_width, render_height,
        &lighting, &post, batches, 0u, &report) == BTBH_GPU_INVALID_ARGUMENT);
    BTBHGPUSceneGeometryBatch bad[ALL_BATCH_COUNT];
    memcpy(bad, batches, sizeof(bad));
    bad[0].flags = 0x80u;
    assert(TARGET_PREWARM_CALL(smoke->window, target, paths, SMOKE_SHADER_FORMAT, render_width, render_height,
        &lighting, &post, bad, batch_count, &report) == BTBH_GPU_INVALID_ARGUMENT);
    memcpy(bad, batches, sizeof(bad));
    bad[1].custom_mesh = other->cube;
    assert(TARGET_PREWARM_CALL(smoke->window, target, paths, SMOKE_SHADER_FORMAT, render_width, render_height,
        &lighting, &post, bad, batch_count, &report) == BTBH_GPU_INVALID_ARGUMENT);
    {
        const BTBHGPUCreationStats made = creation_since(&before);
        assert(made_count(&made) == 0u);
        assert(smoke->window->scene_runtime == window_runtime);
        assert(smoke->window->aside_scene_runtime == aside_runtime);
    }
    /* While a scene waits for its canvas: refused, and the scene still copies. */
    BTBHGPUSceneReport present_report;
    assert(present_frame(smoke, EFFECT_FRAME, &plain, &present_report) == BTBH_GPU_OK);
    assert(TARGET_PREWARM_CALL(smoke->window, target, paths, SMOKE_SHADER_FORMAT, render_width, render_height,
        &lighting, &post, batches, batch_count, &report) == BTBH_GPU_INVALID_ARGUMENT);
    assert(strstr(report.message, "canvas") != NULL);
    BTBHGPUTextureReport copied;
    assert(btbh_gpu_window_copy_pending_scene_texture(smoke->window, smoke->frame, &copied) == BTBH_GPU_OK);
    /* A shader that can't be loaded, for a pipeline the target's runtime lacks (front faces culled): the present's
       error, with the path; the window's runtime is put back. */
    char missing_vertex[1100];
    (void)snprintf(missing_vertex, sizeof(missing_vertex), "%s/missing_model.vert%s",
        BTBH_GPU_SMOKE_SHADER_DIR, SMOKE_SHADER_EXTENSION);
    const char *broken[11];
    for (int index = 0; index < 11; ++index) {
        broken[index] = index == 0 ? missing_vertex : shader_paths[index];
    }
    memcpy(bad, batches, sizeof(bad));
    bad[0].cull_mode = BTBH_GPU_SCENE_CULL_FRONT;
    BTBHGPUSceneRuntime *const runtime_before = smoke->window->scene_runtime;
    assert(TARGET_PREWARM_CALL(smoke->window, target, broken, SMOKE_SHADER_FORMAT, render_width, render_height,
        &lighting, &post, bad, batch_count, &report) == BTBH_GPU_INVALID_ARGUMENT);
    assert(strstr(report.message, "missing_model.vert") != NULL);
    assert(smoke->window->scene_runtime == runtime_before);
#undef TARGET_PREWARM_CALL
    puts("target prewarm refusals: each argument, a scene waiting for its canvas, a missing shader");
}
#endif

int main(void) {
    if (!load_shader_paths()) {
        return SMOKE_SKIP;
    }
    /* Hidden windows never take the focus; nothing here presents to a swapchain. */
    (void)SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    const char *bench = SDL_getenv("BTBH_SMOKE_BENCH");
    if (bench != NULL && bench[0] != '\0') {
        return run_bench(bench);
    }
    BTBHGPUDevice *device = create_device(true);
    if (device == NULL) {
        return SMOKE_SKIP;
    }
    uint8_t *cold[FRAME_COUNT];
    uint8_t *warm[FRAME_COUNT];
    BTBHGPUCreationStats cold_made[FRAME_COUNT];
    BTBHGPUCreationStats warm_made[FRAME_COUNT];

    /* A, cold: its first frame and the effects' first frame make shaders and pipelines; the others none. */
    const SmokeSettings plain = {0};
    SmokeWindow a = open_window(device, "BTBH prewarm smoke A (cold)");
    run_sequence(&a, "A cold", &plain, 0u, FRAME_COUNT, false, cold, cold_made);
    dump_or_compare_frames(cold, FRAME_COUNT, "frame");
    for (uint32_t frame = 0u; frame < FRAME_COUNT; ++frame) {
        const bool makes = frame == 0u || frame == EFFECT_FRAME;
        assert((made_count(&cold_made[frame]) > 0u) == makes);
    }
    /* The first frame makes the shadow pipelines (5 per cull mode), FSR 3's compute pipelines and the post chain's;
       the effects make their model pipelines (two shaders each) and the distance pass. */
    assert(cold_made[0].graphics_pipelines >= 15u);
#if !defined(__APPLE__)
    assert(cold_made[0].compute_pipelines > 0u); /* FSR 3's (Metal keeps FSR 1) */
#endif
    assert(cold_made[EFFECT_FRAME].graphics_pipelines >= EFFECT_BATCH_COUNT &&
           cold_made[EFFECT_FRAME].compute_pipelines == 0u);

    /* B, prewarmed with every batch: no frame of it makes anything, and each is A's to the byte. */
    SmokeWindow b = open_window(device, "BTBH prewarm smoke B (prewarmed)");
    double prewarm_milliseconds = 0.0;
    const BTBHGPUCreationStats prewarm_made = prewarm(&b, &plain, &prewarm_milliseconds);
    print_made("B prewarm", &prewarm_made, prewarm_milliseconds);
    /* It made what A's frames made, no more. */
    assert(prewarm_made.shaders == cold_made[0].shaders + cold_made[EFFECT_FRAME].shaders);
    assert(prewarm_made.graphics_pipelines ==
           cold_made[0].graphics_pipelines + cold_made[EFFECT_FRAME].graphics_pipelines);
    assert(prewarm_made.compute_pipelines == cold_made[0].compute_pipelines);
    /* Again: nothing more. */
    {
        const BTBHGPUCreationStats again = prewarm(&b, &plain, NULL);
        assert(made_count(&again) == 0u);
    }
    run_sequence(&b, "B prewarmed", &plain, 0u, FRAME_COUNT, true, warm, warm_made);
    assert_same_frames(cold, warm, FRAME_COUNT, "A and B");
    free_frames(cold, FRAME_COUNT);
    free_frames(warm, FRAME_COUNT);

    /* Other settings need other pipelines: prewarmed for FSR 1 (no temporal outputs), B makes them, and its FSR 1
       frames then make nothing. */
    {
        const SmokeCamera camera = smoke_camera(0u);
        BTBHGPUScenePost fsr1 = smoke_post(&camera, 1u, &plain);
        fsr1.upscale_mode = BTBH_GPU_SCENE_UPSCALE_FSR1;
        const BTBHGPUCreationStats before = creation_stats();
        BTBHGPUSceneReport report;
        assert(prewarm_with(&b, &plain, &fsr1, &report) == BTBH_GPU_OK);
        assert(report.upscale_mode == BTBH_GPU_SCENE_UPSCALE_FSR1);
        const BTBHGPUCreationStats made = creation_since(&before);
        assert(made.graphics_pipelines > 0u);
        const SmokeCamera frame_camera = smoke_camera(EFFECT_FRAME);
        const BTBHGPUSceneLighting lighting = smoke_lighting(&plain);
        BTBHGPUScenePost post = smoke_post(&frame_camera, 0u, &plain);
        post.upscale_mode = BTBH_GPU_SCENE_UPSCALE_FSR1;
        float instances[INSTANCE_COUNT][INSTANCE_FLOATS];
        smoke_instances(instances, EFFECT_FRAME);
        BTBHGPUSceneGeometryBatch batches[ALL_BATCH_COUNT];
        const uint32_t batch_count = smoke_batches(&b, EFFECT_FRAME, batches);
        const BTBHGPUCreationStats before_present = creation_stats();
        assert(btbh_gpu_window_scene_present_instances(
            b.window,
            shader_paths[0], shader_paths[1], shader_paths[2], shader_paths[3],
            shader_paths[4], shader_paths[5], shader_paths[6], shader_paths[7],
            shader_paths[8], shader_paths[9], shader_paths[10],
            SMOKE_SHADER_FORMAT, render_width, render_height,
            frame_camera.view_projection, frame_camera.right, frame_camera.up, frame_camera.position,
            &lighting, &post, &instances[0][0], INSTANCE_COUNT, INSTANCE_COUNT, batches, batch_count,
            40u, 60u, 90u, 255u, &report) == BTBH_GPU_OK);
        assert(report.upscale_mode == BTBH_GPU_SCENE_UPSCALE_FSR1);
        const BTBHGPUCreationStats present_made = creation_since(&before_present);
        assert(made_count(&present_made) == 0u);
        BTBHGPUTextureReport copied;
        assert(btbh_gpu_window_copy_pending_scene_texture(b.window, b.frame, &copied) == BTBH_GPU_OK);
    }

    check_refusals(&a, &b);
    close_window(&a);
    close_window(&b);

    /* E, cold, renders the sequence into a target (twice the render size: FSR 1): its first render and the effects'
       first make shaders and pipelines; the others none. */
    BTBHGPUTextureReport target_report;
    SmokeWindow e = open_window(device, "BTBH prewarm smoke E (target, cold)");
    BTBHGPUTexture *e_target = btbh_gpu_window_render_target_create(
        e.window, window_width, window_height, BTBH_GPU_TEXTURE_FILTER_LINEAR, &target_report);
    assert(e_target != NULL);
    run_target_sequence(&e, e_target, "E target cold", &plain, 0u, FRAME_COUNT, false, cold, cold_made);
    dump_or_compare_frames(cold, FRAME_COUNT, "target-frame");
    for (uint32_t frame = 0u; frame < FRAME_COUNT; ++frame) {
        const bool makes = frame == 0u || frame == EFFECT_FRAME;
        assert((made_count(&cold_made[frame]) > 0u) == makes);
    }
#if defined(BTBH_GPU_HAS_SCENE_RENDER_TO_TARGET_PREWARM)
    /* F, prewarmed for its target with every batch: no render makes anything, each is E's to the byte, and the
       window's own frames are left alone. */
    SmokeWindow f = open_window(device, "BTBH prewarm smoke F (target, prewarmed)");
    BTBHGPUTexture *f_target = btbh_gpu_window_render_target_create(
        f.window, window_width, window_height, BTBH_GPU_TEXTURE_FILTER_LINEAR, &target_report);
    assert(f_target != NULL);
    assert(f.window->scene_runtime == NULL && f.window->aside_scene_runtime == NULL);
    const BTBHGPUCreationStats target_made = prewarm_target(&f, f_target, &plain, &prewarm_milliseconds);
    print_made("F target prewarm", &target_made, prewarm_milliseconds);
    assert(f.window->scene_runtime == NULL && f.window->aside_scene_runtime != NULL);
    assert(target_made.shaders == cold_made[0].shaders + cold_made[EFFECT_FRAME].shaders);
    assert(target_made.graphics_pipelines ==
           cold_made[0].graphics_pipelines + cold_made[EFFECT_FRAME].graphics_pipelines);
    assert(target_made.compute_pipelines == cold_made[0].compute_pipelines + cold_made[EFFECT_FRAME].compute_pipelines);
    {
        const BTBHGPUCreationStats again = prewarm_target(&f, f_target, &plain, NULL);
        assert(made_count(&again) == 0u);
    }
    run_target_sequence(&f, f_target, "F target prewarmed", &plain, 0u, FRAME_COUNT, true, warm, warm_made);
    assert_same_frames(cold, warm, FRAME_COUNT, "E and F (targets)");
    free_frames(warm, FRAME_COUNT);
    {
        /* The window's first frame still makes its own pipelines (the target's runtime is apart). */
        uint8_t *pixels = NULL;
        const FrameCost cost = run_frame(&f, 0u, &plain, &pixels);
        print_frame("F window", 0u, &cost);
        assert(made_count(&cost.made) > 0u);
        free(pixels);
    }
    check_target_refusals(&e, &f, e_target);
    btbh_gpu_texture_destroy(f_target);
    close_window(&f);
#endif
    free_frames(cold, FRAME_COUNT);
    btbh_gpu_texture_destroy(e_target);
    close_window(&e);

    /* C prewarmed and D cold, with the radiance cascades and an omni spot shadow (their model and shadow variants,
       the cascades' pipelines), every batch drawn from the first frame. */
    const SmokeSettings lit = {.radiance_cascades = true, .omni_spot_shadow = true};
    enum { LIT_FRAMES = 4 };
    SmokeWindow d = open_window(device, "BTBH prewarm smoke D (cold, cascades)");
    run_sequence(&d, "D cold", &lit, EFFECT_FRAME, LIT_FRAMES, false, cold, cold_made);
    assert(made_count(&cold_made[0]) > 0u);
    for (uint32_t index = 1u; index < LIT_FRAMES; ++index) {
        assert(made_count(&cold_made[index]) == 0u);
    }
    SmokeWindow c = open_window(device, "BTBH prewarm smoke C (prewarmed, cascades)");
    const BTBHGPUCreationStats lit_made = prewarm(&c, &lit, &prewarm_milliseconds);
    print_made("C prewarm", &lit_made, prewarm_milliseconds);
    assert(lit_made.shaders == cold_made[0].shaders);
    assert(lit_made.graphics_pipelines == cold_made[0].graphics_pipelines);
    assert(lit_made.compute_pipelines == cold_made[0].compute_pipelines);
    run_sequence(&c, "C prewarmed", &lit, EFFECT_FRAME, LIT_FRAMES, true, warm, warm_made);
    assert_same_frames(cold, warm, LIT_FRAMES, "C and D");
    free_frames(cold, LIT_FRAMES);
    free_frames(warm, LIT_FRAMES);
    close_window(&c);
    close_window(&d);

#if defined(BTBH_GPU_HAS_SCENE_RENDER_TO_TARGET_PREWARM)
    /* H prewarmed for its target and G cold, with the radiance cascades and an omni spot shadow. */
    {
        SmokeWindow g = open_window(device, "BTBH prewarm smoke G (target, cold, cascades)");
        BTBHGPUTexture *g_target = btbh_gpu_window_render_target_create(
            g.window, window_width, window_height, BTBH_GPU_TEXTURE_FILTER_LINEAR, &target_report);
        assert(g_target != NULL);
        run_target_sequence(&g, g_target, "G target cold", &lit, EFFECT_FRAME, LIT_FRAMES, false, cold, cold_made);
        assert(made_count(&cold_made[0]) > 0u);
        for (uint32_t index = 1u; index < LIT_FRAMES; ++index) {
            assert(made_count(&cold_made[index]) == 0u);
        }
        SmokeWindow h = open_window(device, "BTBH prewarm smoke H (target, prewarmed, cascades)");
        BTBHGPUTexture *h_target = btbh_gpu_window_render_target_create(
            h.window, window_width, window_height, BTBH_GPU_TEXTURE_FILTER_LINEAR, &target_report);
        assert(h_target != NULL);
        const BTBHGPUCreationStats lit_target_made = prewarm_target(&h, h_target, &lit, &prewarm_milliseconds);
        print_made("H target prewarm", &lit_target_made, prewarm_milliseconds);
        assert(lit_target_made.shaders == cold_made[0].shaders);
        assert(lit_target_made.graphics_pipelines == cold_made[0].graphics_pipelines);
        assert(lit_target_made.compute_pipelines == cold_made[0].compute_pipelines);
        run_target_sequence(&h, h_target, "H target prewarmed", &lit, EFFECT_FRAME, LIT_FRAMES, true, warm,
            warm_made);
        assert_same_frames(cold, warm, LIT_FRAMES, "G and H (targets)");
        free_frames(cold, LIT_FRAMES);
        free_frames(warm, LIT_FRAMES);
        btbh_gpu_texture_destroy(h_target);
        btbh_gpu_texture_destroy(g_target);
        close_window(&h);
        close_window(&g);
    }
#endif

    btbh_gpu_device_destroy(device);
    puts("btbh_gpu_scene_prewarm_smoke: passed");
    return 0;
}
