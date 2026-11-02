#include "btbh_gpu_internal.h"
#include "btbh_gpu_radiance_cascades_runtime.h"
#include "btbh_gpu_fsr3.h"

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BTBH_GPU_MODEL_VERTEX_STRIDE 96u
#define BTBH_GPU_MODEL_NORMAL_OFFSET 12u
#define BTBH_GPU_MODEL_TANGENT_OFFSET 32u
#define BTBH_GPU_MODEL_COLOR_OFFSET 48u
#define BTBH_GPU_MODEL_JOINTS_OFFSET 64u
#define BTBH_GPU_MODEL_WEIGHTS_OFFSET 80u
#define BTBH_GPU_SCENE_INSTANCE_FLOATS 24u
#define BTBH_GPU_SCENE_INSTANCE_STRIDE \
    (BTBH_GPU_SCENE_INSTANCE_FLOATS * sizeof(float))
#define BTBH_GPU_DIRECTIONAL_SHADOW_MAP_SIZE 512u
/* The primary directional light (the sun) covers the whole view: 16 MB of D32. */
#define BTBH_GPU_PRIMARY_DIRECTIONAL_SHADOW_MAP_SIZE 2048u
#define BTBH_GPU_SPOT_SHADOW_MAP_SIZE 1024u
#define BTBH_GPU_OMNI_SHADOW_MAP_SIZE 512u
#define BTBH_GPU_OMNI_SHADOW_FACE_COUNT 6u
#define BTBH_GPU_SCENE_TRANSPARENCY_COUNT 2u
#define BTBH_GPU_SCENE_UNLIT_VARIANT_COUNT 2u
#define BTBH_GPU_SCENE_STENCIL_VARIANT_COUNT 3u
/* Bits: 1 auxiliary geometry outputs, 2 radiance cascades, 4 temporal upscaling. */
#define BTBH_GPU_SCENE_COLOR_TARGET_VARIANT_COUNT 8u
#define BTBH_GPU_SCENE_SMAA_QUALITY_COUNT 2u
#define BTBH_GPU_SCENE_ANTI_ALIASING_FXAA 3u
#define BTBH_GPU_SCENE_ANTI_ALIASING_MAX 3u
#define BTBH_GPU_SCENE_UPSCALE_MODE_MAX \
    BTBH_GPU_SCENE_UPSCALE_FSR3
#define BTBH_GPU_SCENE_PATH_CAPACITY 1024u
#define BTBH_GPU_SCENE_SHADOW_SPOT 2u
#define BTBH_GPU_SCENE_SSAO_RC_CONTACT_MODE 2u

typedef enum BTBHGPUSceneStencilVariant {
    BTBH_GPU_SCENE_STENCIL_DISABLED = 0,
    BTBH_GPU_SCENE_STENCIL_WRITE_REFERENCE = 1,
    BTBH_GPU_SCENE_STENCIL_NOT_EQUAL_KEEP = 2
} BTBHGPUSceneStencilVariant;

typedef struct BTBHGPUSceneUniforms {
    float view_projection[16];
    float camera_right[4];
    float camera_up[4];
    float camera_position[4];
    float material_color[4];
    float uv_transform[4];
    float ambient_color_energy[4];
    float directional_direction_energy[4];
    float directional_color[4];
    float secondary_directional_direction_energy[4];
    float secondary_directional_color[4];
    uint32_t bone_offset;
    uint32_t billboard_mode;
    uint32_t unlit_enabled;
    uint32_t surface_variant;
    float surface_controls[4];
    float surface_parameters0[4];
    float surface_parameters1[4];
    /* The world curvature this frame and the previous temporal frame drew with (BTBHGPUScenePost). */
    float world_curvature[4];
    float previous_world_curvature[4];
    /* Temporal upscaling (production_temporal.hlsli): the previous frame's view-projection without
       jitter; x, y the clip-space jitter, z 1 when the draw's instances carry their motion, w 1
       while the upscaler runs. */
    float previous_view_projection[16];
    float temporal_controls[4];
    /* The bone palette the previous temporal frame drew this geometry's skin with
       (bones[previous_bone_offset + joint]); bone_offset when that frame had none. */
    uint32_t previous_bone_offset;
    uint32_t temporal_reserved[3];
} BTBHGPUSceneUniforms;

typedef struct BTBHGPUSceneFragmentUniforms {
    float camera_position[4];
    float ambient_color_energy[4];
    float directional_direction_energy[4];
    float directional_color[4];
    float secondary_directional_direction_energy[4];
    float secondary_directional_color[4];
    float spot_position_range[4];
    float spot_direction_energy[4];
    float spot_color_falloff[4];
    float spot_cutoffs[4];
    float material_factors[4];
    float emission_color_energy[4];
    uint32_t unlit_enabled;
    float alpha_cutoff;
    uint32_t surface_variant;
    float surface_time;
    float surface_parameters0[4];
    float surface_parameters1[4];
    float irradiance_sh[9][4];
    float ground_bottom_energy[4];
    float ground_horizon_curve[4];
    float sky_top_energy[4];
    float sky_horizon_curve[4];
    float sun_color_energy[4];
    float sun_direction_size[4];
    float procedural_controls[4];
    float detail_parameters[4];
    /* x the surface wetness, y the window glow, z the rain (BTBHGPUSceneLighting), w reserved. */
    float surface_wetness[4];
    /* Cloud shadows (BTBHGPUSceneLighting): xy the layer's drift, z noise cells per render unit, w the share of
       the sun they take; cloud_shape x the sky's share under clouds, y their edges' width, z the layer's
       height, w reserved. */
    float cloud_controls[4];
    float cloud_shape[4];
    /* x the street lamps in use, y their core radius (BTBHGPUSceneLighting), zw reserved. */
    float lamp_controls[4];
    float lamp_position_reach[BTBH_GPU_SCENE_MAX_LAMPS][4];
    float lamp_color_energy[BTBH_GPU_SCENE_MAX_LAMPS][4];
    float point_light_count[4];
    float point_lights[BTBH_GPU_SCENE_POINT_LIGHTS * 2][4];
    float directional_shadow_view_projection[16];
    float secondary_directional_shadow_view_projection[16];
    float spot_shadow_view_projection[16];
    float directional_shadow_controls[4];
    float secondary_directional_shadow_controls[4];
    float spot_shadow_controls[4];
    float omni_shadow_position_range[4];
} BTBHGPUSceneFragmentUniforms;

typedef struct BTBHGPUShadowUniforms {
    float light_view_projection[16];
    float camera_right[4];
    float camera_up[4];
    uint32_t bone_offset;
    uint32_t billboard_mode;
    uint32_t surface_variant;
    float surface_time;
    float surface_parameters0[4];
    float surface_parameters1[4];
} BTBHGPUShadowUniforms;

typedef struct BTBHGPUPostUniforms {
    float inverse_resolution[2];
    float tonemap_white;
    float cas_sharpness;
    float focus_point[2];
    float focus_direction[2];
    float focus_half_width;
    float maximum_radius_pixels;
    float blur_curve;
    float highlight_threshold;
    float highlight_gain;
    uint32_t lut_enabled;
    uint32_t cas_enabled;
    uint32_t tilt_shift_enabled;
    uint32_t ssao_enabled;
    uint32_t tonemap_mode;
    float exposure;
    float contrast;
    float inverse_view_projection[16];
    float projection_scale_y;
    float saturation;
    /* Screen-space reflections (production_ssao_resolve.frag.hlsl): x 1 when the specular target carries the
       surface's F0 (the cascades' material targets), y reserved; the view-projection the rays project with;
       the intensity, roughest reflecting surface, ray reach and hit thickness (BTBHGPUScenePost). */
    float ssr_options[2];
    float view_projection[16];
    float ssr_controls[4];
    /* The AO's radius and strength, its contact strength and direct share (BTBHGPUScenePost). */
    float ao_controls[4];
} BTBHGPUPostUniforms;

typedef struct BTBHGPUSceneSMAAUniforms {
    float metrics[4];
} BTBHGPUSceneSMAAUniforms;

/* Bloom mip chain below the render resolution: level 0 is half size. */
#define BTBH_GPU_SCENE_BLOOM_LEVELS 6u

typedef struct BTBHGPUSceneBloomUniforms {
    float source_texel_threshold[4];
    float pass_parameters[4];
} BTBHGPUSceneBloomUniforms;

/* Soft particles: the opaque scene's distance from the camera per pixel. */
typedef struct BTBHGPUSceneDistanceUniforms {
    float inverse_view_projection[16];
    float camera_position[4];
} BTBHGPUSceneDistanceUniforms;

typedef struct BTBHGPUSceneCompositeUniforms {
    float inverse_view_projection[16];
    float fog_color_density[4];
    float fog_shape[4];
    float bloom_parameters[4];
    float vignette_parameters[4];
    float fog_edge_bounds[4];
    float fog_edge[4];
} BTBHGPUSceneCompositeUniforms;

/* One skinned pose's bone palette as a temporal frame drew it (the next frame's previous palette). */
typedef struct BTBHGPUSceneTemporalPalette {
    const BTBHGPUAnimationPose *pose;
    uint32_t skin_index;
    uint32_t joint_count;
    uint32_t first_matrix;
} BTBHGPUSceneTemporalPalette;

struct BTBHGPUSceneRuntime {
    SDL_GPUGraphicsPipeline *model_pipelines[
        BTBH_GPU_SCENE_COLOR_TARGET_VARIANT_COUNT
    ][
        BTBH_GPU_SCENE_BLEND_COUNT
    ][BTBH_GPU_SCENE_DEPTH_COUNT]
    [BTBH_GPU_SCENE_TRANSPARENCY_COUNT]
    [BTBH_GPU_SCENE_CULL_COUNT]
    [BTBH_GPU_SCENE_STENCIL_VARIANT_COUNT]
    [BTBH_GPU_SCENE_UNLIT_VARIANT_COUNT];
    SDL_GPUGraphicsPipeline *animated_model_pipelines[
        BTBH_GPU_SCENE_COLOR_TARGET_VARIANT_COUNT
    ][
        BTBH_GPU_SCENE_BLEND_COUNT
    ][BTBH_GPU_SCENE_DEPTH_COUNT]
    [BTBH_GPU_SCENE_TRANSPARENCY_COUNT]
    [BTBH_GPU_SCENE_CULL_COUNT]
    [BTBH_GPU_SCENE_STENCIL_VARIANT_COUNT]
    [BTBH_GPU_SCENE_UNLIT_VARIANT_COUNT];
    SDL_GPUGraphicsPipeline *non_omni_shadowed_model_pipelines[
        BTBH_GPU_SCENE_COLOR_TARGET_VARIANT_COUNT
    ][
        BTBH_GPU_SCENE_BLEND_COUNT
    ][BTBH_GPU_SCENE_DEPTH_COUNT]
    [BTBH_GPU_SCENE_TRANSPARENCY_COUNT]
    [BTBH_GPU_SCENE_CULL_COUNT]
    [BTBH_GPU_SCENE_STENCIL_VARIANT_COUNT]
    [BTBH_GPU_SCENE_UNLIT_VARIANT_COUNT];
    SDL_GPUGraphicsPipeline
        *non_omni_shadowed_animated_model_pipelines[
            BTBH_GPU_SCENE_COLOR_TARGET_VARIANT_COUNT
        ][
            BTBH_GPU_SCENE_BLEND_COUNT
        ][BTBH_GPU_SCENE_DEPTH_COUNT]
        [BTBH_GPU_SCENE_TRANSPARENCY_COUNT]
        [BTBH_GPU_SCENE_CULL_COUNT]
        [BTBH_GPU_SCENE_STENCIL_VARIANT_COUNT]
        [BTBH_GPU_SCENE_UNLIT_VARIANT_COUNT];
    SDL_GPUGraphicsPipeline *unshadowed_model_pipelines[
        BTBH_GPU_SCENE_COLOR_TARGET_VARIANT_COUNT
    ][
        BTBH_GPU_SCENE_BLEND_COUNT
    ][BTBH_GPU_SCENE_DEPTH_COUNT]
    [BTBH_GPU_SCENE_TRANSPARENCY_COUNT]
    [BTBH_GPU_SCENE_CULL_COUNT]
    [BTBH_GPU_SCENE_STENCIL_VARIANT_COUNT]
    [BTBH_GPU_SCENE_UNLIT_VARIANT_COUNT];
    SDL_GPUGraphicsPipeline *unshadowed_animated_model_pipelines[
        BTBH_GPU_SCENE_COLOR_TARGET_VARIANT_COUNT
    ][
        BTBH_GPU_SCENE_BLEND_COUNT
    ][BTBH_GPU_SCENE_DEPTH_COUNT]
    [BTBH_GPU_SCENE_TRANSPARENCY_COUNT]
    [BTBH_GPU_SCENE_CULL_COUNT]
    [BTBH_GPU_SCENE_STENCIL_VARIANT_COUNT]
    [BTBH_GPU_SCENE_UNLIT_VARIANT_COUNT];
    SDL_GPUGraphicsPipeline
        *shadow_pipelines[BTBH_GPU_SCENE_CULL_COUNT];
    SDL_GPUGraphicsPipeline
        *fast_shadow_pipelines[BTBH_GPU_SCENE_CULL_COUNT];
    SDL_GPUGraphicsPipeline
        *non_discard_shadow_pipelines[BTBH_GPU_SCENE_CULL_COUNT];
    SDL_GPUGraphicsPipeline
        *animated_shadow_pipelines[BTBH_GPU_SCENE_CULL_COUNT];
    SDL_GPUGraphicsPipeline
        *animated_non_discard_shadow_pipelines[
            BTBH_GPU_SCENE_CULL_COUNT
        ];
    SDL_GPUGraphicsPipeline *ssao_pipeline;
    SDL_GPUGraphicsPipeline *ssao_denoise_pipeline;
    SDL_GPUGraphicsPipeline *ssao_resolve_pipeline;
    SDL_GPUGraphicsPipeline *post_lut_pipeline;
    SDL_GPUGraphicsPipeline *post_cas_pipeline;
    SDL_GPUGraphicsPipeline *post_tilt_shift_pipeline;
    SDL_GPUGraphicsPipeline *upscale_lanczos_pipeline;
    SDL_GPUGraphicsPipeline *upscale_fsr_easu_pipeline;
    SDL_GPUGraphicsPipeline *upscale_fsr_rcas_pipeline;
    SDL_GPUGraphicsPipeline *post_pipeline;
    SDL_GPUGraphicsPipeline *smaa_edge_pipelines[
        BTBH_GPU_SCENE_SMAA_QUALITY_COUNT
    ];
    SDL_GPUGraphicsPipeline *smaa_weight_pipelines[
        BTBH_GPU_SCENE_SMAA_QUALITY_COUNT
    ];
    SDL_GPUGraphicsPipeline *smaa_blend_pipeline;
    SDL_GPUGraphicsPipeline *fxaa_pipeline;
    SDL_GPUGraphicsPipeline *bloom_downsample_pipeline;
    SDL_GPUGraphicsPipeline *bloom_upsample_pipeline;
    SDL_GPUGraphicsPipeline *hdr_composite_pipeline;
    SDL_GPUGraphicsPipeline *scene_distance_pipeline;
    BTBHGPUTexture *fallback_texture;
    BTBHGPUTexture *normal_fallback_texture;
    BTBHGPUTexture *smaa_area_texture;
    BTBHGPUTexture *smaa_search_texture;
    SDL_GPUTexture *brdf_lut_texture;
    SDL_GPUSampler *brdf_lut_sampler;
    SDL_GPUTexture *depth_texture;
    SDL_GPUTexture *scene_color_texture;
    /* The scene as it was before the first refracting batch (BTBH_GPU_SCENE_SURFACE_REFRACT); made on the
       first frame that draws one. */
    SDL_GPUTexture *refraction_texture;
    uint32_t refraction_width;
    uint32_t refraction_height;
    SDL_GPUTexture *ambient_texture;
    SDL_GPUTexture *ambient_specular_texture;
    SDL_GPUTexture *normal_texture;
    SDL_GPUTexture *emission_texture;
    SDL_GPUTexture *ssao_texture;
    SDL_GPUTexture *ssao_denoised_texture;
    SDL_GPUTexture *ssao_resolved_texture;
    SDL_GPUTexture *scene_distance_texture;
    uint32_t scene_distance_width;
    uint32_t scene_distance_height;
    SDL_GPUTexture *post_ping_texture;
    SDL_GPUTexture *post_pong_texture;
    SDL_GPUTexture *final_color_texture;
    SDL_GPUTexture *upscaled_color_texture;
    /* FSR1's RCAS output, the size of upscaled_color_texture. */
    SDL_GPUTexture *sharpened_color_texture;
    SDL_GPUTexture *composite_color_texture;
    SDL_GPUTexture *post_color_texture;
    SDL_GPUTexture *smaa_edges_texture;
    SDL_GPUTexture *smaa_weights_texture;
    SDL_GPUTexture *bloom_down_textures[BTBH_GPU_SCENE_BLOOM_LEVELS];
    /* The last level has no upsample: its downsample is the widest blur. */
    SDL_GPUTexture *bloom_up_textures[BTBH_GPU_SCENE_BLOOM_LEVELS];
    uint32_t bloom_level_width[BTBH_GPU_SCENE_BLOOM_LEVELS];
    uint32_t bloom_level_height[BTBH_GPU_SCENE_BLOOM_LEVELS];
    uint32_t bloom_width;
    uint32_t bloom_height;
    SDL_GPUTexture *directional_shadow_textures[3];
    uint32_t directional_shadow_width;
    uint32_t directional_shadow_height;
    SDL_GPUTexture *omni_shadow_texture;
    SDL_GPUSampler *directional_shadow_sampler;
    /* The primary map's comparison sampler (2x2 percentage-closer filtering by the hardware), for the
       soft sun shadow filter. */
    SDL_GPUSampler *directional_shadow_compare_sampler;
    SDL_GPUSampler *scene_color_sampler;
    SDL_GPUBuffer *instance_buffer;
    SDL_GPUBuffer *instance_vertex_buffer;
    SDL_GPUBuffer *bone_buffer;
    SDL_GPUTransferBuffer *upload_transfer;
    uint32_t depth_width;
    uint32_t depth_height;
    SDL_GPUTextureFormat depth_format;
    SDL_GPUTextureFormat scene_color_format;
    uint32_t scene_color_width;
    uint32_t scene_color_height;
    uint32_t smaa_width;
    uint32_t smaa_height;
    uint32_t upscale_width;
    uint32_t upscale_height;
    uint32_t composite_width;
    uint32_t composite_height;
    uint32_t instance_capacity;
    uint32_t bone_capacity;
    uint32_t upload_capacity;
    uint64_t shadow_frame_serial;
    float retained_shadow_view_projection[3][16];
    float retained_omni_shadow_view_projection[
        BTBH_GPU_OMNI_SHADOW_FACE_COUNT
    ][16];
    float retained_omni_shadow_position_range[4];
    uint8_t retained_shadow_ready[3];
    uint8_t retained_shadow_active[3];
    uint8_t retained_spot_shadow_is_omni;
    uint8_t omni_shadow_unavailable;
    BTBHGPUSceneRadianceCascadesRuntime radiance_cascades;
    /* Temporal upscaling (FSR 3.1): motion vectors written by the opaque pass, the reactive mask
       written by the transparent pass (both at the render size), each transient instance's motion,
       and the previous frame's view-projection (without jitter) while it rendered temporally. */
    BTBHGPUFSR3Runtime fsr3;
    SDL_GPUTexture *velocity_texture;
    SDL_GPUTexture *reactive_texture;
    uint32_t temporal_target_width;
    uint32_t temporal_target_height;
    SDL_GPUBuffer *motion_buffer;
    uint32_t motion_capacity;
    float temporal_previous_view_projection[16];
    float temporal_previous_world_curvature[4];
    bool temporal_previous_valid;
    bool temporal_debug_view_read;
    bool temporal_jitter_disabled;
    /* BTBH_FSR3_MIP_BIAS: a fixed texture LOD bias under FSR 3 instead of AMD's formula ("off" is 0). */
    bool temporal_lod_bias_overridden;
    float temporal_lod_bias_override;
    /* The bone palettes the previous temporal frame drew, by animation pose and skin, so skinned limbs
       get their own motion vectors (matrices: 16 floats each). */
    BTBHGPUSceneTemporalPalette *temporal_palettes;
    uint32_t temporal_palette_count;
    uint32_t temporal_palette_capacity;
    float *temporal_palette_matrices;
    uint32_t temporal_palette_matrix_capacity;
    BTBHGPUFSR3DebugView temporal_debug_view;
    /* The previous frame's colour for the motion-error debug view. */
    SDL_GPUTexture *temporal_previous_color;
    uint32_t temporal_previous_color_width;
    uint32_t temporal_previous_color_height;
};

static bool btbh_gpu_scene_shadow_resources_ready(
    const BTBHGPUSceneRuntime *runtime
) {
    if (runtime == NULL ||
        runtime->directional_shadow_textures[0] == NULL ||
        runtime->directional_shadow_textures[1] == NULL ||
        runtime->directional_shadow_textures[2] == NULL ||
        runtime->directional_shadow_sampler == NULL ||
        runtime->directional_shadow_compare_sampler == NULL) {
        return false;
    }
    for (uint32_t cull_index = 0u;
         cull_index < BTBH_GPU_SCENE_CULL_COUNT;
         ++cull_index) {
        if (
            runtime->shadow_pipelines[cull_index] == NULL ||
            runtime->fast_shadow_pipelines[cull_index] == NULL ||
            runtime->non_discard_shadow_pipelines[cull_index] == NULL ||
            runtime->animated_shadow_pipelines[cull_index] == NULL ||
            runtime
                ->animated_non_discard_shadow_pipelines[cull_index] ==
                    NULL
        ) {
            return false;
        }
    }
    return true;
}

uint8_t btbh_gpu_window_scene_shadow_resources_ready(
    const BTBHGPUWindow *window
) {
    return window != NULL &&
        btbh_gpu_scene_shadow_resources_ready(window->scene_runtime)
            ? 1u
            : 0u;
}

uint8_t btbh_gpu_window_scene_set_shadow_update_periods(
    BTBHGPUWindow *window,
    uint32_t directional_frames,
    uint32_t secondary_directional_frames,
    uint32_t spot_frames
) {
    if (window == NULL ||
        directional_frames < 1u || directional_frames > 60u ||
        secondary_directional_frames < 1u ||
            secondary_directional_frames > 60u ||
        spot_frames < 1u || spot_frames > 60u) {
        return 0u;
    }
    window->scene_shadow_update_period_frames[0] = directional_frames;
    window->scene_shadow_update_period_frames[1] =
        secondary_directional_frames;
    window->scene_shadow_update_period_frames[2] = spot_frames;
    return 1u;
}

uint8_t btbh_gpu_window_scene_set_directional_shadow_size(
    BTBHGPUWindow *window,
    uint32_t width,
    uint32_t height
) {
    if (window == NULL ||
        width < 256u || width > 8192u ||
        height < 256u || height > 8192u) {
        return 0u;
    }
    window->scene_directional_shadow_size[0] = width;
    window->scene_directional_shadow_size[1] = height;
    return 1u;
}

uint8_t btbh_gpu_window_scene_read_telemetry(
    const BTBHGPUWindow *window,
    BTBHGPUSceneTelemetryReport *report
) {
    if (window == NULL || report == NULL) {
        return 0u;
    }
    *report = window->scene_telemetry;
    return 1u;
}

void btbh_gpu_window_scene_record_frame_completion(
    BTBHGPUWindow *window,
    double completion_milliseconds
) {
    if (window == NULL || window->scene_runtime == NULL) {
        return;
    }
    btbh_gpu_radiance_cascades_runtime_record_completion(
        &window->scene_runtime->radiance_cascades,
        completion_milliseconds
    );
}

static int btbh_gpu_cost_log_state = -1;
/* [pass][0 the rest, 1 the static city] */
static uint64_t btbh_gpu_cost_triangles[BTBH_GPU_COST_PASS_COUNT][2];
static uint64_t btbh_gpu_cost_draws[BTBH_GPU_COST_PASS_COUNT][2];

bool btbh_gpu_cost_log_enabled(void) {
    if (btbh_gpu_cost_log_state < 0) {
        const char *value = SDL_getenv("BTBH_GPU_COST_LOG");
        btbh_gpu_cost_log_state =
            value != NULL && strcmp(value, "1") == 0 ? 1 : 0;
    }
    return btbh_gpu_cost_log_state == 1;
}

/* `BTBH_GPU_COST_SKIP_MODEL=<text>` (measurement, with the cost log): models whose file name contains the
   text are not drawn in any pass, which measures what a cheaper version of them could save at most. */
static int btbh_gpu_cost_skip_state = -1;
static char btbh_gpu_cost_skip_text[96];

bool btbh_gpu_cost_skips_model(const BTBHGPUModel *model) {
    if (btbh_gpu_cost_skip_state < 0) {
        const char *value = SDL_getenv("BTBH_GPU_COST_SKIP_MODEL");
        btbh_gpu_cost_skip_state = value != NULL && value[0] != '\0' ? 1 : 0;
        if (btbh_gpu_cost_skip_state == 1) {
            (void)snprintf(btbh_gpu_cost_skip_text, sizeof(btbh_gpu_cost_skip_text), "%s", value);
        }
    }
    return btbh_gpu_cost_skip_state == 1 &&
        model != NULL &&
        strstr(model->debug_name, btbh_gpu_cost_skip_text) != NULL;
}

/* Per model (or custom mesh) over `BTBH_GPU_COST_MODEL_FRAMES` frames: the triangles each pass drew of it. */
#define BTBH_GPU_COST_MODEL_CAPACITY 4096u
#define BTBH_GPU_COST_MODEL_FRAMES 600u
#define BTBH_GPU_COST_MODEL_TOP 30u
typedef struct BTBHGPUCostModelEntry {
    const void *key;
    char name[96];
    uint64_t triangles[BTBH_GPU_COST_PASS_COUNT];
    uint64_t main_instances;
    uint32_t triangles_per_instance;
    bool static_city;
} BTBHGPUCostModelEntry;
static BTBHGPUCostModelEntry btbh_gpu_cost_models[BTBH_GPU_COST_MODEL_CAPACITY];
static uint32_t btbh_gpu_cost_model_frames;

static BTBHGPUCostModelEntry *btbh_gpu_cost_model_entry(
    const BTBHGPUModel *model,
    const BTBHGPUCustomMesh *custom_mesh,
    bool static_city
) {
    const void *key = model != NULL ? (const void *)model : (const void *)custom_mesh;
    if (key == NULL) {
        return NULL;
    }
    uint32_t slot = (uint32_t)(((uintptr_t)key >> 4) * 2654435761u) % BTBH_GPU_COST_MODEL_CAPACITY;
    for (uint32_t probe = 0u; probe < BTBH_GPU_COST_MODEL_CAPACITY; ++probe) {
        BTBHGPUCostModelEntry *entry = &btbh_gpu_cost_models[slot];
        if (entry->key == key) {
            return entry;
        }
        if (entry->key == NULL) {
            entry->key = key;
            entry->static_city = static_city;
            uint64_t triangles = 0u;
            if (model != NULL) {
                for (uint32_t index = 0u; index < model->primitive_count; ++index) {
                    triangles += model->primitives[index].index_count > 0u
                        ? model->primitives[index].index_count / 3u
                        : model->primitives[index].vertex_count / 3u;
                }
                (void)snprintf(entry->name, sizeof(entry->name), "%s", model->debug_name);
            } else {
                triangles = custom_mesh->index_count > 0u
                    ? custom_mesh->index_count / 3u
                    : custom_mesh->vertex_count / 3u;
                (void)snprintf(entry->name, sizeof(entry->name), "custom-mesh-%uv", custom_mesh->vertex_count);
            }
            entry->triangles_per_instance = (uint32_t)triangles;
            return entry;
        }
        slot = (slot + 1u) % BTBH_GPU_COST_MODEL_CAPACITY;
    }
    return NULL;
}

void btbh_gpu_cost_record_draw(
    BTBHGPUCostPass pass,
    uint32_t element_count,
    uint32_t instance_count,
    bool static_city,
    const BTBHGPUModel *model,
    const BTBHGPUCustomMesh *custom_mesh
) {
    if (!btbh_gpu_cost_log_enabled() || btbh_gpu_cost_skips_model(model)) {
        return;
    }
    const uint64_t triangles = (uint64_t)(element_count / 3u) * (uint64_t)instance_count;
    btbh_gpu_cost_triangles[pass][static_city ? 1 : 0] += triangles;
    btbh_gpu_cost_draws[pass][static_city ? 1 : 0] += 1u;
    BTBHGPUCostModelEntry *entry = btbh_gpu_cost_model_entry(model, custom_mesh, static_city);
    if (entry != NULL) {
        entry->triangles[pass] += triangles;
        if (pass == BTBH_GPU_COST_PASS_MAIN) {
            entry->main_instances += instance_count;
        }
    }
}

static int btbh_gpu_cost_model_compare(const void *left, const void *right) {
    const BTBHGPUCostModelEntry *a = left;
    const BTBHGPUCostModelEntry *b = right;
    const uint64_t total_a = a->triangles[0] + a->triangles[1] + a->triangles[2];
    const uint64_t total_b = b->triangles[0] + b->triangles[1] + b->triangles[2];
    return total_a < total_b ? 1 : (total_a > total_b ? -1 : 0);
}

/* Prints the models that drew the most triangles, per frame, and starts a new window. Draws of one model
   with several primitives count each primitive's draw as instances, so main_instances is draws x instances. */
static void btbh_gpu_cost_emit_models(void) {
    static BTBHGPUCostModelEntry sorted[BTBH_GPU_COST_MODEL_CAPACITY];
    uint32_t count = 0u;
    for (uint32_t index = 0u; index < BTBH_GPU_COST_MODEL_CAPACITY; ++index) {
        if (btbh_gpu_cost_models[index].key != NULL) {
            sorted[count++] = btbh_gpu_cost_models[index];
        }
    }
    qsort(sorted, count, sizeof(sorted[0]), btbh_gpu_cost_model_compare);
    const double frames = (double)BTBH_GPU_COST_MODEL_FRAMES;
    for (uint32_t index = 0u; index < count && index < BTBH_GPU_COST_MODEL_TOP; ++index) {
        const BTBHGPUCostModelEntry *entry = &sorted[index];
        (void)fprintf(
            stdout,
            "BTBH_GPU_COST_MODEL rank=%u city=%d tris_per_instance=%u main_instance_draws=%.1f "
            "main_ktris=%.1f shadow_ktris=%.1f rc_ktris=%.1f name=%s\n",
            index + 1u,
            entry->static_city ? 1 : 0,
            entry->triangles_per_instance,
            (double)entry->main_instances / frames,
            (double)entry->triangles[BTBH_GPU_COST_PASS_MAIN] / frames / 1000.0,
            (double)entry->triangles[BTBH_GPU_COST_PASS_SHADOW] / frames / 1000.0,
            (double)entry->triangles[BTBH_GPU_COST_PASS_RC_SOURCE] / frames / 1000.0,
            entry->name
        );
    }
    SDL_zeroa(btbh_gpu_cost_models);
}

/* The per-pass split (see `BTBHGPUCostGroup`): the device is set while the current frame is split. */
static const char *const btbh_gpu_cost_group_names[BTBH_GPU_COST_GROUP_COUNT] = {
    "upload", "shadow", "main", "rc_src", "rc_field", "rc_screen", "ssao", "transparent", "post", "ui"
};
static SDL_GPUDevice *btbh_gpu_cost_split_device;
static uint64_t btbh_gpu_cost_frame_serial;
static double btbh_gpu_cost_group_milliseconds[BTBH_GPU_COST_GROUP_COUNT];
/* The radiance-cascade emitters (gameplay lights) the frame drew into the source field and the direct light. */
static uint32_t btbh_gpu_cost_rc_emitters;

/* Warm clocks (2026-10-17). Waiting for every frame leaves the GPU idle while the CPU prepares the next one
   (about 60 % of a fight's frame on the RTX 3070 Ti), and the driver lowers its clocks at that load: a boss run
   measured 1605 MHz, the same run without enemies 1065 MHz with the memory at half its speed, so a cheaper frame
   looked hardly cheaper. After each measured frame the log therefore keeps the GPU busy for about
   `BTBH_GPU_COST_WARM_MS` (default 12, 0 = off) with blits between two scratch textures, and the next frame waits
   for the GPU to go idle before it submits anything, so they never overlap the measured work. */
#define BTBH_GPU_COST_WARM_SIZE 2048u
#define BTBH_GPU_COST_WARM_CALIBRATION_BLITS 64u
static double btbh_gpu_cost_warm_target_milliseconds = -1.0;
static SDL_GPUDevice *btbh_gpu_cost_warm_device;
static SDL_GPUTexture *btbh_gpu_cost_warm_textures[2];
static SDL_GPUFence *btbh_gpu_cost_warm_fence;
static double btbh_gpu_cost_warm_blit_milliseconds;
static uint32_t btbh_gpu_cost_warm_frames;

static double btbh_gpu_cost_warm_milliseconds(void) {
    if (btbh_gpu_cost_warm_target_milliseconds < 0.0) {
        const char *value = SDL_getenv("BTBH_GPU_COST_WARM_MS");
        const double parsed = value != NULL && value[0] != '\0' ? SDL_strtod(value, NULL) : 12.0;
        btbh_gpu_cost_warm_target_milliseconds = parsed >= 0.0 && parsed <= 100.0 ? parsed : 12.0;
    }
    return btbh_gpu_cost_warm_target_milliseconds;
}

static SDL_GPUFence *btbh_gpu_cost_submit_warm_blits(SDL_GPUDevice *device, uint32_t count) {
    SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(device);
    if (command == NULL) {
        return NULL;
    }
    SDL_GPUBlitInfo blit;
    SDL_zero(blit);
    blit.source.w = BTBH_GPU_COST_WARM_SIZE;
    blit.source.h = BTBH_GPU_COST_WARM_SIZE;
    blit.destination.w = BTBH_GPU_COST_WARM_SIZE;
    blit.destination.h = BTBH_GPU_COST_WARM_SIZE;
    blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
    blit.filter = SDL_GPU_FILTER_LINEAR;
    for (uint32_t index = 0u; index < count; ++index) {
        blit.source.texture = btbh_gpu_cost_warm_textures[index & 1u];
        blit.destination.texture = btbh_gpu_cost_warm_textures[(index + 1u) & 1u];
        SDL_BlitGPUTexture(command, &blit);
    }
    return SDL_SubmitGPUCommandBufferAndAcquireFence(command);
}

void btbh_gpu_cost_wait_warm(void) {
    if (btbh_gpu_cost_warm_fence == NULL) {
        return;
    }
    (void)SDL_WaitForGPUFences(btbh_gpu_cost_warm_device, true, &btbh_gpu_cost_warm_fence, 1u);
    SDL_ReleaseGPUFence(btbh_gpu_cost_warm_device, btbh_gpu_cost_warm_fence);
    btbh_gpu_cost_warm_fence = NULL;
}

void btbh_gpu_cost_warm(SDL_GPUDevice *device) {
    if (device == NULL || btbh_gpu_cost_warm_milliseconds() <= 0.0) {
        return;
    }
    btbh_gpu_cost_wait_warm();
    if (btbh_gpu_cost_warm_textures[0] == NULL) {
        SDL_GPUTextureCreateInfo info;
        SDL_zero(info);
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        info.width = BTBH_GPU_COST_WARM_SIZE;
        info.height = BTBH_GPU_COST_WARM_SIZE;
        info.layer_count_or_depth = 1u;
        info.num_levels = 1u;
        btbh_gpu_cost_warm_device = device;
        btbh_gpu_cost_warm_textures[0] = SDL_CreateGPUTexture(device, &info);
        btbh_gpu_cost_warm_textures[1] = SDL_CreateGPUTexture(device, &info);
        if (btbh_gpu_cost_warm_textures[0] == NULL || btbh_gpu_cost_warm_textures[1] == NULL) {
            btbh_gpu_cost_release_warm();
            btbh_gpu_cost_warm_target_milliseconds = 0.0;
            return;
        }
    }
    /* The blits' cost, measured now and every 600 frames: at first the GPU may still run at idle clocks. */
    if (btbh_gpu_cost_warm_frames % BTBH_GPU_COST_MODEL_FRAMES == 0u) {
        const uint64_t started_ticks = SDL_GetTicksNS();
        btbh_gpu_cost_warm_fence =
            btbh_gpu_cost_submit_warm_blits(device, BTBH_GPU_COST_WARM_CALIBRATION_BLITS);
        if (btbh_gpu_cost_warm_fence == NULL) {
            btbh_gpu_cost_release_warm();
            btbh_gpu_cost_warm_target_milliseconds = 0.0;
            return;
        }
        btbh_gpu_cost_wait_warm();
        btbh_gpu_cost_warm_blit_milliseconds = SDL_max(
            (double)(SDL_GetTicksNS() - started_ticks) / 1000000.0 /
                (double)BTBH_GPU_COST_WARM_CALIBRATION_BLITS,
            0.001
        );
        (void)fprintf(
            stdout,
            "BTBH_GPU_COST_WARM blit_ms=%.4f target_ms=%.1f\n",
            btbh_gpu_cost_warm_blit_milliseconds,
            btbh_gpu_cost_warm_milliseconds()
        );
    }
    btbh_gpu_cost_warm_frames += 1u;
    const double blits = btbh_gpu_cost_warm_milliseconds() / btbh_gpu_cost_warm_blit_milliseconds;
    btbh_gpu_cost_warm_fence = btbh_gpu_cost_submit_warm_blits(
        device,
        (uint32_t)SDL_clamp(blits, 1.0, 20000.0)
    );
}

void btbh_gpu_cost_release_warm(void) {
    if (btbh_gpu_cost_warm_device == NULL) {
        return;
    }
    btbh_gpu_cost_wait_warm();
    for (uint32_t index = 0u; index < 2u; ++index) {
        if (btbh_gpu_cost_warm_textures[index] != NULL) {
            SDL_ReleaseGPUTexture(btbh_gpu_cost_warm_device, btbh_gpu_cost_warm_textures[index]);
            btbh_gpu_cost_warm_textures[index] = NULL;
        }
    }
    btbh_gpu_cost_warm_device = NULL;
}

void btbh_gpu_cost_begin_frame(SDL_GPUDevice *device) {
    btbh_gpu_cost_split_device = NULL;
    if (!btbh_gpu_cost_log_enabled()) {
        return;
    }
    /* The warming blits and any upload submitted since end before the frame's first measured submit. */
    btbh_gpu_cost_wait_warm();
    (void)SDL_WaitForGPUIdle(device);
    SDL_zeroa(btbh_gpu_cost_group_milliseconds);
    btbh_gpu_cost_frame_serial += 1u;
    /* About every other frame, but in the golden-ratio sequence ({n / phi} >= 0.5), which has no period: work
       that alternates frame by frame (the shadow maps update every second frame, staggered; the cascades'
       temporal phase) then falls on split and whole frames alike. */
    if ((btbh_gpu_cost_frame_serial * UINT64_C(0x9E3779B97F4A7C15)) >> 63 != 0u) {
        btbh_gpu_cost_split_device = device;
    }
}

bool btbh_gpu_cost_split(SDL_GPUCommandBuffer **command, BTBHGPUCostGroup finished_group) {
    SDL_GPUDevice *device = btbh_gpu_cost_split_device;
    if (device == NULL) {
        return true;
    }
    const uint64_t submit_started_ticks = SDL_GetTicksNS();
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(*command);
    *command = NULL;
    if (fence == NULL) {
        return false;
    }
    (void)SDL_WaitForGPUFences(device, true, &fence, 1u);
    btbh_gpu_cost_group_milliseconds[finished_group] +=
        (double)(SDL_GetTicksNS() - submit_started_ticks) / 1000000.0;
    SDL_ReleaseGPUFence(device, fence);
    *command = SDL_AcquireGPUCommandBuffer(device);
    return *command != NULL;
}

void btbh_gpu_cost_emit_frame(double gpu_milliseconds) {
    btbh_gpu_cost_model_frames += 1u;
    if (btbh_gpu_cost_model_frames == BTBH_GPU_COST_MODEL_FRAMES) {
        btbh_gpu_cost_model_frames = 0u;
        btbh_gpu_cost_emit_models();
    }
    /* A split frame's last piece is the canvas's; its gpu_ms is the sum of the pieces. */
    char groups[320] = "split=0";
    if (btbh_gpu_cost_split_device != NULL) {
        btbh_gpu_cost_group_milliseconds[BTBH_GPU_COST_GROUP_UI] += gpu_milliseconds;
        gpu_milliseconds = 0.0;
        int length = snprintf(groups, sizeof(groups), "split=1");
        for (uint32_t group = 0u; group < BTBH_GPU_COST_GROUP_COUNT; ++group) {
            gpu_milliseconds += btbh_gpu_cost_group_milliseconds[group];
            if (length > 0 && (size_t)length < sizeof(groups)) {
                length += snprintf(
                    groups + length,
                    sizeof(groups) - (size_t)length,
                    " %s_ms=%.4f",
                    btbh_gpu_cost_group_names[group],
                    btbh_gpu_cost_group_milliseconds[group]
                );
            }
        }
        btbh_gpu_cost_split_device = NULL;
    }
    (void)fprintf(
        stdout,
        "BTBH_GPU_COST gpu_ms=%.4f %s "
        "main_city_tris=%llu main_other_tris=%llu "
        "shadow_city_tris=%llu shadow_other_tris=%llu "
        "rc_city_tris=%llu rc_other_tris=%llu "
        "main_city_draws=%llu main_other_draws=%llu "
        "shadow_city_draws=%llu shadow_other_draws=%llu rc_emitters=%u\n",
        gpu_milliseconds,
        groups,
        (unsigned long long)btbh_gpu_cost_triangles[BTBH_GPU_COST_PASS_MAIN][1],
        (unsigned long long)btbh_gpu_cost_triangles[BTBH_GPU_COST_PASS_MAIN][0],
        (unsigned long long)btbh_gpu_cost_triangles[BTBH_GPU_COST_PASS_SHADOW][1],
        (unsigned long long)btbh_gpu_cost_triangles[BTBH_GPU_COST_PASS_SHADOW][0],
        (unsigned long long)btbh_gpu_cost_triangles[BTBH_GPU_COST_PASS_RC_SOURCE][1],
        (unsigned long long)btbh_gpu_cost_triangles[BTBH_GPU_COST_PASS_RC_SOURCE][0],
        (unsigned long long)btbh_gpu_cost_draws[BTBH_GPU_COST_PASS_MAIN][1],
        (unsigned long long)btbh_gpu_cost_draws[BTBH_GPU_COST_PASS_MAIN][0],
        (unsigned long long)btbh_gpu_cost_draws[BTBH_GPU_COST_PASS_SHADOW][1],
        (unsigned long long)btbh_gpu_cost_draws[BTBH_GPU_COST_PASS_SHADOW][0],
        btbh_gpu_cost_rc_emitters
    );
    (void)fflush(stdout);
    SDL_zeroa(btbh_gpu_cost_triangles);
    SDL_zeroa(btbh_gpu_cost_draws);
    btbh_gpu_cost_rc_emitters = 0u;
}

static uint32_t btbh_gpu_scene_shadow_update_period(
    const BTBHGPUWindow *window,
    uint32_t shadow_index
) {
    const uint32_t period =
        window->scene_shadow_update_period_frames[shadow_index];
    return period >= 1u && period <= 60u ? period : 1u;
}

void btbh_gpu_window_scene_submission_finished(
    BTBHGPUWindow *window,
    bool submitted
) {
    if (submitted || window == NULL || window->scene_runtime == NULL) {
        return;
    }
    BTBHGPUSceneRuntime *runtime = window->scene_runtime;
    btbh_gpu_radiance_cascades_runtime_invalidate_history(
        &runtime->radiance_cascades
    );
    runtime->shadow_frame_serial = 0u;
    (void)memset(
        runtime->retained_shadow_ready,
        0,
        sizeof(runtime->retained_shadow_ready)
    );
    (void)memset(
        runtime->retained_shadow_active,
        0,
        sizeof(runtime->retained_shadow_active)
    );
}

static void btbh_gpu_scene_cancel_submission(
    BTBHGPUWindow *window,
    SDL_GPUCommandBuffer *command
) {
    if (command != NULL) {
        (void)SDL_CancelGPUCommandBuffer(command);
    }
    btbh_gpu_cost_split_device = NULL;
    btbh_gpu_window_scene_submission_finished(window, false);
}

static void btbh_gpu_scene_error(
    BTBHGPUSceneReport *report,
    BTBHGPUResult result,
    const char *message
) {
    report->result = result;
    (void)snprintf(
        report->message,
        sizeof(report->message),
        "%s",
        message != NULL ? message : ""
    );
}

/* `btbh_gpu_cost_split` for the scene: false, with the frame cancelled and the error set, when it failed. */
static bool btbh_gpu_scene_cost_split(
    BTBHGPUWindow *window,
    SDL_GPUCommandBuffer **command,
    BTBHGPUCostGroup finished_group,
    BTBHGPUSceneReport *report
) {
    if (btbh_gpu_cost_split(command, finished_group)) {
        return true;
    }
    btbh_gpu_scene_error(report, BTBH_GPU_DEVICE_FAILURE, SDL_GetError());
    btbh_gpu_scene_cancel_submission(window, *command);
    *command = NULL;
    return false;
}

static bool btbh_gpu_scene_invert_matrix4(
    const float matrix[16],
    float inverse[16]
) {
    double augmented[4][8];
    for (uint32_t row = 0u; row < 4u; ++row) {
        for (uint32_t column = 0u; column < 4u; ++column) {
            const float value = matrix[column * 4u + row];
            if (!isfinite(value)) {
                return false;
            }
            augmented[row][column] = (double)value;
            augmented[row][column + 4u] =
                row == column ? 1.0 : 0.0;
        }
    }
    for (uint32_t pivot_column = 0u;
         pivot_column < 4u;
         ++pivot_column) {
        uint32_t pivot_row = pivot_column;
        double pivot_magnitude =
            fabs(augmented[pivot_row][pivot_column]);
        for (uint32_t candidate = pivot_column + 1u;
             candidate < 4u;
             ++candidate) {
            const double candidate_magnitude =
                fabs(augmented[candidate][pivot_column]);
            if (candidate_magnitude > pivot_magnitude) {
                pivot_row = candidate;
                pivot_magnitude = candidate_magnitude;
            }
        }
        if (!isfinite(pivot_magnitude) ||
            pivot_magnitude <= 1.0e-12) {
            return false;
        }
        if (pivot_row != pivot_column) {
            for (uint32_t column = 0u; column < 8u; ++column) {
                const double temporary =
                    augmented[pivot_column][column];
                augmented[pivot_column][column] =
                    augmented[pivot_row][column];
                augmented[pivot_row][column] = temporary;
            }
        }
        const double pivot =
            augmented[pivot_column][pivot_column];
        for (uint32_t column = 0u; column < 8u; ++column) {
            augmented[pivot_column][column] /= pivot;
        }
        for (uint32_t row = 0u; row < 4u; ++row) {
            if (row == pivot_column) {
                continue;
            }
            const double factor =
                augmented[row][pivot_column];
            for (uint32_t column = 0u; column < 8u; ++column) {
                augmented[row][column] -=
                    factor * augmented[pivot_column][column];
            }
        }
    }
    for (uint32_t row = 0u; row < 4u; ++row) {
        for (uint32_t column = 0u; column < 4u; ++column) {
            const double value = augmented[row][column + 4u];
            if (!isfinite(value) ||
                value < -(double)FLT_MAX ||
                value > (double)FLT_MAX) {
                return false;
            }
            inverse[column * 4u + row] = (float)value;
        }
    }
    return true;
}

static SDL_GPUShaderFormat btbh_gpu_scene_shader_format(
    BTBHGPUShaderFormat format
) {
    switch (format) {
        case BTBH_GPU_SHADER_FORMAT_SPIRV:
            return SDL_GPU_SHADERFORMAT_SPIRV;
        case BTBH_GPU_SHADER_FORMAT_METALLIB:
            return SDL_GPU_SHADERFORMAT_METALLIB;
        case BTBH_GPU_SHADER_FORMAT_INVALID:
        default:
            return SDL_GPU_SHADERFORMAT_INVALID;
    }
}

static SDL_GPUShader *btbh_gpu_scene_load_shader(
    SDL_GPUDevice *device,
    const char *path,
    const char *entrypoint,
    SDL_GPUShaderFormat format,
    SDL_GPUShaderStage stage,
    uint32_t storage_buffer_count,
    uint32_t sampler_count,
    uint32_t uniform_buffer_count,
    BTBHGPUSceneReport *report
) {
    size_t byte_count = 0u;
    void *bytes = SDL_LoadFile(path, &byte_count);
    if (bytes == NULL || byte_count == 0u) {
        SDL_free(bytes);
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            path
        );
        return NULL;
    }
    SDL_GPUShaderCreateInfo info;
    SDL_zero(info);
    info.code = bytes;
    info.code_size = byte_count;
    info.entrypoint = entrypoint;
    info.format = format;
    info.stage = stage;
    info.num_storage_buffers = storage_buffer_count;
    info.num_samplers = sampler_count;
    info.num_uniform_buffers = uniform_buffer_count;
    SDL_GPUShader *shader = SDL_CreateGPUShader(device, &info);
    SDL_free(bytes);
    if (shader == NULL) {
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
    }
    return shader;
}

bool btbh_gpu_scene_uses_transparent_pass(
    BTBHGPUSceneBlendMode mode,
    bool transparency_enabled
) {
    return transparency_enabled ||
        mode != BTBH_GPU_SCENE_BLEND_ALPHA;
}

bool btbh_gpu_scene_uses_fast_unlit_pipeline(
    const BTBHGPUSceneGeometryBatch *batch
) {
    return batch != NULL &&
        batch->unlit_enabled != 0u &&
        batch->surface_variant == 0u &&
        btbh_gpu_scene_uses_transparent_pass(
            batch->blend_mode,
            batch->transparency_enabled != 0u
        );
}

bool btbh_gpu_scene_refraction_valid(
    const BTBHGPUSceneGeometryBatch *batch
) {
    return batch != NULL &&
        (batch->surface_variant != BTBH_GPU_SCENE_SURFACE_REFRACT ||
         (batch->transparency_enabled == 1u &&
          batch->blend_mode == BTBH_GPU_SCENE_BLEND_ALPHA &&
          batch->surface_parameters1[3] == 0.0f));
}

void btbh_gpu_scene_resolve_blend_state(
    BTBHGPUSceneBlendMode mode,
    bool transparency_enabled,
    SDL_GPUColorTargetBlendState *state
) {
    SDL_zero(*state);
    state->enable_blend = true;
    state->color_blend_op = SDL_GPU_BLENDOP_ADD;
    state->alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    switch (mode) {
        case BTBH_GPU_SCENE_BLEND_ADDITIVE:
            state->src_color_blendfactor =
                transparency_enabled
                    ? SDL_GPU_BLENDFACTOR_SRC_ALPHA
                    : SDL_GPU_BLENDFACTOR_ONE;
            state->dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            break;
        case BTBH_GPU_SCENE_BLEND_MULTIPLIED:
            state->src_color_blendfactor =
                SDL_GPU_BLENDFACTOR_DST_COLOR;
            state->dst_color_blendfactor =
                SDL_GPU_BLENDFACTOR_ZERO;
            break;
        case BTBH_GPU_SCENE_BLEND_PREMULTIPLIED_ALPHA:
            state->src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            state->dst_color_blendfactor =
                SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            break;
        case BTBH_GPU_SCENE_BLEND_ALPHA:
        default:
            state->src_color_blendfactor =
                SDL_GPU_BLENDFACTOR_SRC_ALPHA;
            state->dst_color_blendfactor =
                SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            break;
    }
    state->src_alpha_blendfactor =
        state->src_color_blendfactor;
    state->dst_alpha_blendfactor =
        state->dst_color_blendfactor;
}

static SDL_GPUCompareOp btbh_gpu_scene_depth_compare(
    BTBHGPUSceneDepthCompare compare
) {
    switch (compare) {
        case BTBH_GPU_SCENE_DEPTH_ALWAYS:
            return SDL_GPU_COMPAREOP_ALWAYS;
        case BTBH_GPU_SCENE_DEPTH_LESS_EQUAL:
            return SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
        case BTBH_GPU_SCENE_DEPTH_GREATER:
            return SDL_GPU_COMPAREOP_GREATER;
        case BTBH_GPU_SCENE_DEPTH_NOT_EQUAL:
            return SDL_GPU_COMPAREOP_NOT_EQUAL;
        case BTBH_GPU_SCENE_DEPTH_EQUAL:
            return SDL_GPU_COMPAREOP_EQUAL;
        case BTBH_GPU_SCENE_DEPTH_GREATER_EQUAL:
            return SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
        case BTBH_GPU_SCENE_DEPTH_NEVER:
            return SDL_GPU_COMPAREOP_NEVER;
        case BTBH_GPU_SCENE_DEPTH_LESS:
        default:
            return SDL_GPU_COMPAREOP_LESS;
    }
}

static SDL_GPUCullMode btbh_gpu_scene_cull_mode(
    BTBHGPUSceneCullMode mode
) {
    switch (mode) {
        case BTBH_GPU_SCENE_CULL_FRONT:
            return SDL_GPU_CULLMODE_FRONT;
        case BTBH_GPU_SCENE_CULL_BACK:
            return SDL_GPU_CULLMODE_BACK;
        case BTBH_GPU_SCENE_CULL_NONE:
        default:
            return SDL_GPU_CULLMODE_NONE;
    }
}

static SDL_GPUStencilOp btbh_gpu_scene_stencil_operation(
    BTBHGPUSceneStencilOperation operation
) {
    switch (operation) {
        case BTBH_GPU_SCENE_STENCIL_ZERO:
            return SDL_GPU_STENCILOP_ZERO;
        case BTBH_GPU_SCENE_STENCIL_REPLACE:
            return SDL_GPU_STENCILOP_REPLACE;
        case BTBH_GPU_SCENE_STENCIL_INCREMENT_CLAMP:
            return SDL_GPU_STENCILOP_INCREMENT_AND_CLAMP;
        case BTBH_GPU_SCENE_STENCIL_DECREMENT_CLAMP:
            return SDL_GPU_STENCILOP_DECREMENT_AND_CLAMP;
        case BTBH_GPU_SCENE_STENCIL_KEEP:
        default:
            return SDL_GPU_STENCILOP_KEEP;
    }
}

static bool btbh_gpu_scene_stencil_variant(
    const BTBHGPUSceneGeometryBatch *batch,
    BTBHGPUSceneStencilVariant *variant
) {
    if (batch == NULL || variant == NULL) {
        return false;
    }
    const bool default_operations =
        batch->stencil_fail == BTBH_GPU_SCENE_STENCIL_KEEP &&
        batch->stencil_depth_fail == BTBH_GPU_SCENE_STENCIL_KEEP;
    if (
        batch->stencil_reference == 0u &&
        batch->stencil_compare == BTBH_GPU_SCENE_DEPTH_ALWAYS &&
        default_operations &&
        batch->stencil_pass == BTBH_GPU_SCENE_STENCIL_REPLACE
    ) {
        *variant = BTBH_GPU_SCENE_STENCIL_DISABLED;
        return true;
    }
    if (
        batch->stencil_compare == BTBH_GPU_SCENE_DEPTH_ALWAYS &&
        default_operations &&
        batch->stencil_pass == BTBH_GPU_SCENE_STENCIL_REPLACE
    ) {
        *variant = BTBH_GPU_SCENE_STENCIL_WRITE_REFERENCE;
        return true;
    }
    if (
        batch->stencil_compare == BTBH_GPU_SCENE_DEPTH_NOT_EQUAL &&
        default_operations &&
        batch->stencil_pass == BTBH_GPU_SCENE_STENCIL_KEEP
    ) {
        *variant = BTBH_GPU_SCENE_STENCIL_NOT_EQUAL_KEEP;
        return true;
    }
    return false;
}

static bool btbh_gpu_scene_create_model_pipeline(
    BTBHGPUWindow *window,
    const char *vertex_shader_path,
    const char *fragment_shader_path,
    BTBHGPUShaderFormat project_format,
    bool animated,
    uint32_t fragment_sampler_count,
    bool auxiliary_geometry_outputs,
    bool temporal,
    const BTBHGPUSceneGeometryBatch *batch,
    BTBHGPUSceneStencilVariant stencil_variant,
    SDL_GPUGraphicsPipeline **pipeline,
    BTBHGPUSceneReport *report
) {
    SDL_GPUDevice *device = window->device->gpu;
    const SDL_GPUShaderFormat format =
        btbh_gpu_scene_shader_format(project_format);
    const char *entrypoint =
        project_format == BTBH_GPU_SHADER_FORMAT_METALLIB
            ? "main0"
            : "main";
    SDL_GPUShader *vertex_shader = btbh_gpu_scene_load_shader(
        device,
        vertex_shader_path,
        entrypoint,
        format,
        SDL_GPU_SHADERSTAGE_VERTEX,
        /* Instances, the bone palette when animated, and the instances' motion. */
        animated ? 3u : 2u,
        0u,
        1u,
        report
    );
    if (vertex_shader == NULL) {
        return false;
    }
    SDL_GPUShader *fragment_shader = btbh_gpu_scene_load_shader(
        device,
        fragment_shader_path,
        entrypoint,
        format,
        SDL_GPU_SHADERSTAGE_FRAGMENT,
        0u,
        fragment_sampler_count,
        1u,
        report
    );
    if (fragment_shader == NULL) {
        SDL_ReleaseGPUShader(device, vertex_shader);
        return false;
    }

    SDL_GPUColorTargetDescription color_targets[6];
    SDL_GPUVertexBufferDescription vertex_buffer;
    SDL_GPUVertexAttribute attributes[7];
    SDL_GPUGraphicsPipelineCreateInfo info;
    SDL_zeroa(color_targets);
    SDL_zero(vertex_buffer);
    SDL_zeroa(attributes);
    SDL_zero(info);

    color_targets[0].format =
        window->scene_runtime->scene_color_format;
    color_targets[0].blend_state.color_write_mask = 0x0fu;
    color_targets[1].format =
        window->scene_runtime->scene_color_format;
    color_targets[1].blend_state.color_write_mask = 0x0fu;
    color_targets[2].format =
        SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    color_targets[2].blend_state.color_write_mask = 0x0fu;
    color_targets[3].format =
        window->scene_runtime->scene_color_format;
    color_targets[3].blend_state.color_write_mask = 0x0fu;
    color_targets[4].format =
        window->scene_runtime->scene_color_format;
    color_targets[4].blend_state.color_write_mask = 0x0fu;
    color_targets[5].format = SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT;
    color_targets[5].blend_state.color_write_mask = 0x03u;
    vertex_buffer.slot = 0u;
    vertex_buffer.pitch = BTBH_GPU_MODEL_VERTEX_STRIDE;
    vertex_buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    vertex_buffer.instance_step_rate = 0u;
    attributes[0].location = 0u;
    attributes[0].buffer_slot = 0u;
    attributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    attributes[0].offset = 0u;
    attributes[1].location = 1u;
    attributes[1].buffer_slot = 0u;
    attributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    attributes[1].offset = BTBH_GPU_MODEL_COLOR_OFFSET;
    attributes[2].location = 2u;
    attributes[2].buffer_slot = 0u;
    attributes[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    attributes[2].offset = 24u;
    attributes[3].location = 3u;
    attributes[3].buffer_slot = 0u;
    attributes[3].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    attributes[3].offset = BTBH_GPU_MODEL_NORMAL_OFFSET;
    attributes[4].location = 4u;
    attributes[4].buffer_slot = 0u;
    attributes[4].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    attributes[4].offset = animated
        ? BTBH_GPU_MODEL_JOINTS_OFFSET
        : BTBH_GPU_MODEL_TANGENT_OFFSET;
    if (animated) {
        attributes[5].location = 5u;
        attributes[5].buffer_slot = 0u;
        attributes[5].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
        attributes[5].offset = BTBH_GPU_MODEL_WEIGHTS_OFFSET;
        attributes[6].location = 6u;
        attributes[6].buffer_slot = 0u;
        attributes[6].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
        attributes[6].offset = BTBH_GPU_MODEL_TANGENT_OFFSET;
    }

    info.vertex_shader = vertex_shader;
    info.fragment_shader = fragment_shader;
    info.vertex_input_state.vertex_buffer_descriptions = &vertex_buffer;
    info.vertex_input_state.num_vertex_buffers = 1u;
    info.vertex_input_state.vertex_attributes = attributes;
    info.vertex_input_state.num_vertex_attributes = animated ? 7u : 5u;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode =
        btbh_gpu_scene_cull_mode(batch->cull_mode);
    info.rasterizer_state.front_face =
        SDL_GPU_FRONTFACE_CLOCKWISE;
    info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.depth_stencil_state.enable_depth_test = true;
    info.depth_stencil_state.enable_depth_write =
        !btbh_gpu_scene_uses_transparent_pass(
            batch->blend_mode,
            batch->transparency_enabled != 0u
        );
    info.depth_stencil_state.compare_op =
        btbh_gpu_scene_depth_compare(batch->depth_compare);
    if (stencil_variant != BTBH_GPU_SCENE_STENCIL_DISABLED) {
        SDL_GPUStencilOpState stencil;
        SDL_zero(stencil);
        stencil.compare_op =
            btbh_gpu_scene_depth_compare(batch->stencil_compare);
        stencil.fail_op =
            btbh_gpu_scene_stencil_operation(batch->stencil_fail);
        stencil.depth_fail_op =
            btbh_gpu_scene_stencil_operation(
                batch->stencil_depth_fail
            );
        stencil.pass_op =
            btbh_gpu_scene_stencil_operation(batch->stencil_pass);
        info.depth_stencil_state.front_stencil_state = stencil;
        info.depth_stencil_state.back_stencil_state = stencil;
        info.depth_stencil_state.compare_mask = batch->stencil_mask;
        info.depth_stencil_state.write_mask = batch->stencil_mask;
        info.depth_stencil_state.enable_stencil_test = true;
    }
    const bool transparent =
        btbh_gpu_scene_uses_transparent_pass(
            batch->blend_mode,
            batch->transparency_enabled != 0u
        );
    info.target_info.color_target_descriptions = color_targets;
    /* While the temporal upscaler runs the opaque pass adds the motion vectors (6 targets) and the
       transparent pass the reactive mask (2). */
    info.target_info.num_color_targets = temporal
        ? (transparent ? 2u : 6u)
        : (auxiliary_geometry_outputs ? 5u : 1u);
    info.target_info.depth_stencil_format =
        window->scene_runtime->depth_format;
    info.target_info.has_depth_stencil_target = true;

    if (
        batch->transparency_enabled != 0u ||
        batch->blend_mode != BTBH_GPU_SCENE_BLEND_ALPHA
    ) {
        btbh_gpu_scene_resolve_blend_state(
            batch->blend_mode,
            batch->transparency_enabled != 0u,
            &color_targets[0].blend_state
        );
    } else {
        SDL_zero(color_targets[0].blend_state);
    }
    color_targets[0].blend_state.color_write_mask = 0x0fu;
    color_targets[1].blend_state.color_write_mask =
        transparent ? 0u : 0x0fu;
    color_targets[2].blend_state.color_write_mask =
        transparent ? 0u : 0x0fu;
    color_targets[3].blend_state.color_write_mask =
        transparent ? 0u : 0x0fu;
    color_targets[4].blend_state.color_write_mask =
        transparent ? 0u : 0x0fu;
    color_targets[5].blend_state.color_write_mask =
        transparent ? 0u : 0x03u;
    if (temporal && transparent) {
        /* The reactive mask keeps the strongest effect drawn over each pixel. */
        SDL_zero(color_targets[1]);
        color_targets[1].format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
        color_targets[1].blend_state.enable_blend = true;
        color_targets[1].blend_state.color_blend_op = SDL_GPU_BLENDOP_MAX;
        color_targets[1].blend_state.alpha_blend_op = SDL_GPU_BLENDOP_MAX;
        color_targets[1].blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        color_targets[1].blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        color_targets[1].blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        color_targets[1].blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        color_targets[1].blend_state.color_write_mask = 0x01u;
    }
    *pipeline = SDL_CreateGPUGraphicsPipeline(device, &info);
    SDL_ReleaseGPUShader(device, fragment_shader);
    SDL_ReleaseGPUShader(device, vertex_shader);
    if (*pipeline == NULL) {
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return false;
    }
    return true;
}

static bool btbh_gpu_scene_ensure_post_pipeline(
    BTBHGPUWindow *window,
    const char *vertex_shader_path,
    const char *fragment_shader_path,
    BTBHGPUShaderFormat project_format,
    BTBHGPUSceneReport *report
) {
    BTBHGPUSceneRuntime *runtime = window->scene_runtime;
    if (runtime->post_pipeline != NULL) {
        return true;
    }
    SDL_GPUDevice *device = window->device->gpu;
    const SDL_GPUShaderFormat format =
        btbh_gpu_scene_shader_format(project_format);
    const char *entrypoint =
        project_format == BTBH_GPU_SHADER_FORMAT_METALLIB
            ? "main0"
            : "main";
    SDL_GPUShader *vertex_shader = btbh_gpu_scene_load_shader(
        device,
        vertex_shader_path,
        entrypoint,
        format,
        SDL_GPU_SHADERSTAGE_VERTEX,
        0u,
        0u,
        0u,
        report
    );
    if (vertex_shader == NULL) {
        return false;
    }
    SDL_GPUShader *fragment_shader = btbh_gpu_scene_load_shader(
        device,
        fragment_shader_path,
        entrypoint,
        format,
        SDL_GPU_SHADERSTAGE_FRAGMENT,
        0u,
        1u,
        1u,
        report
    );
    if (fragment_shader == NULL) {
        SDL_ReleaseGPUShader(device, vertex_shader);
        return false;
    }
    SDL_GPUColorTargetDescription color_target;
    SDL_GPUGraphicsPipelineCreateInfo info;
    SDL_zero(color_target);
    SDL_zero(info);
    color_target.format = SDL_GetGPUSwapchainTextureFormat(
        device,
        window->window
    );
    color_target.blend_state.color_write_mask = 0x0fu;
    info.vertex_shader = vertex_shader;
    info.fragment_shader = fragment_shader;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.rasterizer_state.front_face =
        SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.target_info.color_target_descriptions = &color_target;
    info.target_info.num_color_targets = 1u;
    runtime->post_pipeline =
        SDL_CreateGPUGraphicsPipeline(device, &info);
    SDL_ReleaseGPUShader(device, fragment_shader);
    SDL_ReleaseGPUShader(device, vertex_shader);
    if (runtime->post_pipeline == NULL) {
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return false;
    }
    return true;
}

static bool btbh_gpu_scene_sibling_path(
    const char *reference,
    const char *filename,
    char output[BTBH_GPU_SCENE_PATH_CAPACITY]
) {
    if (reference == NULL || filename == NULL || output == NULL) {
        return false;
    }
    const char *slash = strrchr(reference, '/');
    const char *backslash = strrchr(reference, '\\');
    const char *separator = slash;
    if (backslash != NULL &&
        (separator == NULL || backslash > separator)) {
        separator = backslash;
    }
    const size_t directory_length = separator == NULL
        ? 0u
        : (size_t)(separator - reference + 1);
    const size_t filename_length = strlen(filename);
    if (directory_length + filename_length + 1u >
        BTBH_GPU_SCENE_PATH_CAPACITY) {
        return false;
    }
    if (directory_length > 0u) {
        (void)memcpy(output, reference, directory_length);
    }
    (void)memcpy(
        output + directory_length,
        filename,
        filename_length + 1u
    );
    return true;
}

static bool btbh_gpu_scene_create_fullscreen_pipeline(
    BTBHGPUWindow *window,
    const char *vertex_shader_path,
    const char *fragment_shader_path,
    BTBHGPUShaderFormat project_format,
    SDL_GPUTextureFormat target_format,
    uint32_t fragment_sampler_count,
    SDL_GPUGraphicsPipeline **pipeline,
    BTBHGPUSceneReport *report
) {
    SDL_GPUDevice *device = window->device->gpu;
    const SDL_GPUShaderFormat format =
        btbh_gpu_scene_shader_format(project_format);
    const char *entrypoint =
        project_format == BTBH_GPU_SHADER_FORMAT_METALLIB
            ? "main0"
            : "main";
    SDL_GPUShader *vertex_shader = btbh_gpu_scene_load_shader(
        device,
        vertex_shader_path,
        entrypoint,
        format,
        SDL_GPU_SHADERSTAGE_VERTEX,
        0u,
        0u,
        0u,
        report
    );
    if (vertex_shader == NULL) {
        return false;
    }
    SDL_GPUShader *fragment_shader = btbh_gpu_scene_load_shader(
        device,
        fragment_shader_path,
        entrypoint,
        format,
        SDL_GPU_SHADERSTAGE_FRAGMENT,
        0u,
        fragment_sampler_count,
        1u,
        report
    );
    if (fragment_shader == NULL) {
        SDL_ReleaseGPUShader(device, vertex_shader);
        return false;
    }
    SDL_GPUColorTargetDescription color_target;
    SDL_GPUGraphicsPipelineCreateInfo info;
    SDL_zero(color_target);
    SDL_zero(info);
    color_target.format = target_format;
    color_target.blend_state.color_write_mask = 0x0fu;
    info.vertex_shader = vertex_shader;
    info.fragment_shader = fragment_shader;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.rasterizer_state.front_face =
        SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.target_info.color_target_descriptions = &color_target;
    info.target_info.num_color_targets = 1u;
    *pipeline = SDL_CreateGPUGraphicsPipeline(device, &info);
    SDL_ReleaseGPUShader(device, fragment_shader);
    SDL_ReleaseGPUShader(device, vertex_shader);
    if (*pipeline == NULL) {
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return false;
    }
    return true;
}

static bool btbh_gpu_scene_ensure_post_stage_pipelines(
    BTBHGPUWindow *window,
    const char *post_vertex_shader_path,
    const char *post_fragment_shader_path,
    BTBHGPUShaderFormat shader_format,
    BTBHGPUSceneReport *report
) {
    BTBHGPUSceneRuntime *runtime = window->scene_runtime;
    if (runtime->post_lut_pipeline != NULL &&
        runtime->post_cas_pipeline != NULL &&
        runtime->post_tilt_shift_pipeline != NULL) {
        return true;
    }
    const char *extension =
        shader_format == BTBH_GPU_SHADER_FORMAT_METALLIB
            ? "metallib"
            : "spv";
    char filename[128];
    char path[BTBH_GPU_SCENE_PATH_CAPACITY];

    if (runtime->post_lut_pipeline == NULL) {
        (void)snprintf(
            filename,
            sizeof(filename),
            "production_lut.frag.%s",
            extension
        );
        if (!btbh_gpu_scene_sibling_path(
                post_fragment_shader_path,
                filename,
                path
            ) ||
            !btbh_gpu_scene_create_fullscreen_pipeline(
                window,
                post_vertex_shader_path,
                path,
                shader_format,
                SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
                2u,
                &runtime->post_lut_pipeline,
                report
            )) {
            return false;
        }
    }
    if (runtime->post_cas_pipeline == NULL) {
        (void)snprintf(
            filename,
            sizeof(filename),
            "production_cas.frag.%s",
            extension
        );
        if (!btbh_gpu_scene_sibling_path(
                post_fragment_shader_path,
                filename,
                path
            ) ||
            !btbh_gpu_scene_create_fullscreen_pipeline(
                window,
                post_vertex_shader_path,
                path,
                shader_format,
                SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
                1u,
                &runtime->post_cas_pipeline,
                report
            )) {
            return false;
        }
    }
    if (runtime->post_tilt_shift_pipeline == NULL) {
        (void)snprintf(
            filename,
            sizeof(filename),
            "production_tilt_shift.frag.%s",
            extension
        );
        if (!btbh_gpu_scene_sibling_path(
                post_fragment_shader_path,
                filename,
                path
            ) ||
            !btbh_gpu_scene_create_fullscreen_pipeline(
                window,
                post_vertex_shader_path,
                path,
                shader_format,
                SDL_GetGPUSwapchainTextureFormat(
                    window->device->gpu,
                    window->window
                ),
                1u,
                &runtime->post_tilt_shift_pipeline,
                report
            )) {
            return false;
        }
    }
    return true;
}

/* Bloom and the HDR composite are created on first use, so frames that leave
 * fog, bloom and vignette off never load their shaders. */
static bool btbh_gpu_scene_ensure_hdr_composite_pipelines(
    BTBHGPUWindow *window,
    const char *post_vertex_shader_path,
    const char *post_fragment_shader_path,
    BTBHGPUShaderFormat shader_format,
    BTBHGPUSceneReport *report
) {
    BTBHGPUSceneRuntime *runtime = window->scene_runtime;
    if (runtime->bloom_downsample_pipeline != NULL &&
        runtime->bloom_upsample_pipeline != NULL &&
        runtime->hdr_composite_pipeline != NULL) {
        return true;
    }
    const char *extension =
        shader_format == BTBH_GPU_SHADER_FORMAT_METALLIB
            ? "metallib"
            : "spv";
    const struct {
        const char *name;
        uint32_t sampler_count;
        SDL_GPUGraphicsPipeline **pipeline;
    } stages[] = {
        {"production_bloom_downsample.frag", 1u, &runtime->bloom_downsample_pipeline},
        {"production_bloom_upsample.frag", 2u, &runtime->bloom_upsample_pipeline},
        {"production_hdr_composite.frag", 3u, &runtime->hdr_composite_pipeline},
    };
    char filename[128];
    char path[BTBH_GPU_SCENE_PATH_CAPACITY];
    for (size_t index = 0u; index < sizeof(stages) / sizeof(stages[0]); ++index) {
        if (*stages[index].pipeline != NULL) {
            continue;
        }
        (void)snprintf(
            filename,
            sizeof(filename),
            "%s.%s",
            stages[index].name,
            extension
        );
        if (!btbh_gpu_scene_sibling_path(
                post_fragment_shader_path,
                filename,
                path
            ) ||
            !btbh_gpu_scene_create_fullscreen_pipeline(
                window,
                post_vertex_shader_path,
                path,
                shader_format,
                SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
                stages[index].sampler_count,
                stages[index].pipeline,
                report
            )) {
            return false;
        }
    }
    return true;
}

static bool btbh_gpu_scene_ensure_upscale_pipeline(
    BTBHGPUWindow *window,
    const char *shader_name,
    SDL_GPUGraphicsPipeline **pipeline,
    const char *post_vertex_shader_path,
    const char *post_fragment_shader_path,
    BTBHGPUShaderFormat shader_format,
    BTBHGPUSceneReport *report
) {
    if (*pipeline != NULL) {
        return true;
    }
    const char *extension =
        shader_format == BTBH_GPU_SHADER_FORMAT_METALLIB
            ? "metallib"
            : "spv";
    char filename[128];
    char path[BTBH_GPU_SCENE_PATH_CAPACITY];
    (void)snprintf(
        filename,
        sizeof(filename),
        "%s.frag.%s",
        shader_name,
        extension
    );
    return btbh_gpu_scene_sibling_path(
            post_fragment_shader_path,
            filename,
            path
        ) &&
        btbh_gpu_scene_create_fullscreen_pipeline(
            window,
            post_vertex_shader_path,
            path,
            shader_format,
            SDL_GetGPUSwapchainTextureFormat(
                window->device->gpu,
                window->window
            ),
            1u,
            pipeline,
            report
        );
}

static bool btbh_gpu_scene_ensure_upscale_pipelines(
    BTBHGPUWindow *window,
    uint8_t upscale_mode,
    const char *post_vertex_shader_path,
    const char *post_fragment_shader_path,
    BTBHGPUShaderFormat shader_format,
    BTBHGPUSceneReport *report
) {
    BTBHGPUSceneRuntime *runtime = window->scene_runtime;
    if (upscale_mode == BTBH_GPU_SCENE_UPSCALE_LANCZOS) {
        return btbh_gpu_scene_ensure_upscale_pipeline(
            window, "production_upscale_lanczos",
            &runtime->upscale_lanczos_pipeline,
            post_vertex_shader_path, post_fragment_shader_path,
            shader_format, report
        );
    }
    return btbh_gpu_scene_ensure_upscale_pipeline(
            window, "production_upscale_fsr_easu",
            &runtime->upscale_fsr_easu_pipeline,
            post_vertex_shader_path, post_fragment_shader_path,
            shader_format, report
        ) &&
        btbh_gpu_scene_ensure_upscale_pipeline(
            window, "production_upscale_fsr_rcas",
            &runtime->upscale_fsr_rcas_pipeline,
            post_vertex_shader_path, post_fragment_shader_path,
            shader_format, report
        );
}

static bool btbh_gpu_scene_load_smaa_lookup(
    BTBHGPUWindow *window,
    const char *path,
    uint32_t width,
    uint32_t height,
    uint32_t channels,
    BTBHGPUTextureFilter filter,
    BTBHGPUTexture **texture,
    BTBHGPUSceneReport *report
) {
    size_t source_size = 0u;
    uint8_t *source = SDL_LoadFile(path, &source_size);
    const size_t pixel_count = (size_t)width * (size_t)height;
    if (source == NULL ||
        source_size != pixel_count * (size_t)channels) {
        SDL_free(source);
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            path
        );
        return false;
    }
    uint8_t *rgba = SDL_malloc(pixel_count * 4u);
    if (rgba == NULL) {
        SDL_free(source);
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            "Could not allocate SMAA lookup expansion"
        );
        return false;
    }
    for (size_t index = 0u; index < pixel_count; ++index) {
        rgba[index * 4u] = source[index * channels];
        rgba[index * 4u + 1u] =
            channels > 1u ? source[index * channels + 1u] : 0u;
        rgba[index * 4u + 2u] = 0u;
        rgba[index * 4u + 3u] = 255u;
    }
    BTBHGPUTextureReport texture_report;
    *texture = btbh_gpu_window_texture_create_rgba8(
        window,
        rgba,
        width,
        height,
        filter,
        BTBH_GPU_TEXTURE_WRAP_CLAMP,
        &texture_report
    );
    SDL_free(rgba);
    SDL_free(source);
    if (*texture == NULL) {
        btbh_gpu_scene_error(
            report,
            texture_report.result,
            texture_report.message
        );
        return false;
    }
    return true;
}

static bool btbh_gpu_scene_ensure_ssao_resolve_pipeline(
    BTBHGPUWindow *window,
    const char *post_vertex_shader_path,
    const char *post_fragment_shader_path,
    BTBHGPUShaderFormat shader_format,
    BTBHGPUSceneReport *report
) {
    BTBHGPUSceneRuntime *runtime = window->scene_runtime;
    if (runtime->ssao_pipeline != NULL &&
        runtime->ssao_denoise_pipeline != NULL &&
        runtime->ssao_resolve_pipeline != NULL) {
        return true;
    }
    SDL_GPUDevice *device = window->device->gpu;
    if (runtime->ssao_pipeline != NULL) {
        SDL_ReleaseGPUGraphicsPipeline(
            device,
            runtime->ssao_pipeline
        );
        runtime->ssao_pipeline = NULL;
    }
    if (runtime->ssao_resolve_pipeline != NULL) {
        SDL_ReleaseGPUGraphicsPipeline(
            device,
            runtime->ssao_resolve_pipeline
        );
        runtime->ssao_resolve_pipeline = NULL;
    }
    if (runtime->ssao_denoise_pipeline != NULL) {
        SDL_ReleaseGPUGraphicsPipeline(
            device,
            runtime->ssao_denoise_pipeline
        );
        runtime->ssao_denoise_pipeline = NULL;
    }
    char path[BTBH_GPU_SCENE_PATH_CAPACITY];
    char filename[128];
    const char *extension =
        shader_format == BTBH_GPU_SHADER_FORMAT_METALLIB
            ? "metallib"
            : "spv";
    (void)snprintf(
        filename,
        sizeof(filename),
        "production_ssao.frag.%s",
        extension
    );
    if (!btbh_gpu_scene_sibling_path(
            post_fragment_shader_path,
            filename,
            path
        ) ||
        !btbh_gpu_scene_create_fullscreen_pipeline(
            window,
            post_vertex_shader_path,
            path,
            shader_format,
            SDL_GPU_TEXTUREFORMAT_R8_UNORM,
            2u,
            &runtime->ssao_pipeline,
            report
    )) {
        return false;
    }
    (void)snprintf(
        filename,
        sizeof(filename),
        "production_ssao_denoise.frag.%s",
        extension
    );
    if (!btbh_gpu_scene_sibling_path(
            post_fragment_shader_path,
            filename,
            path
        ) ||
        !btbh_gpu_scene_create_fullscreen_pipeline(
            window,
            post_vertex_shader_path,
            path,
            shader_format,
            SDL_GPU_TEXTUREFORMAT_R8_UNORM,
            3u,
            &runtime->ssao_denoise_pipeline,
            report
        )) {
        SDL_ReleaseGPUGraphicsPipeline(
            device,
            runtime->ssao_pipeline
        );
        runtime->ssao_pipeline = NULL;
        return false;
    }
    (void)snprintf(
        filename,
        sizeof(filename),
        "production_ssao_resolve.frag.%s",
        extension
    );
    if (!btbh_gpu_scene_sibling_path(
            post_fragment_shader_path,
            filename,
            path
        ) ||
        !btbh_gpu_scene_create_fullscreen_pipeline(
            window,
            post_vertex_shader_path,
            path,
            shader_format,
            runtime->scene_color_format,
            7u,
            &runtime->ssao_resolve_pipeline,
            report
    )) {
        SDL_ReleaseGPUGraphicsPipeline(
            device,
            runtime->ssao_denoise_pipeline
        );
        SDL_ReleaseGPUGraphicsPipeline(
            device,
            runtime->ssao_pipeline
        );
        runtime->ssao_denoise_pipeline = NULL;
        runtime->ssao_pipeline = NULL;
        return false;
    }
    return true;
}

static bool btbh_gpu_scene_ensure_scene_distance_pipeline(
    BTBHGPUWindow *window,
    const char *post_vertex_shader_path,
    const char *post_fragment_shader_path,
    BTBHGPUShaderFormat shader_format,
    BTBHGPUSceneReport *report
) {
    BTBHGPUSceneRuntime *runtime = window->scene_runtime;
    if (runtime->scene_distance_pipeline != NULL) {
        return true;
    }
    char path[BTBH_GPU_SCENE_PATH_CAPACITY];
    char filename[128];
    (void)snprintf(
        filename,
        sizeof(filename),
        "production_scene_distance.frag.%s",
        shader_format == BTBH_GPU_SHADER_FORMAT_METALLIB
            ? "metallib"
            : "spv"
    );
    return btbh_gpu_scene_sibling_path(
            post_fragment_shader_path,
            filename,
            path
        ) &&
        btbh_gpu_scene_create_fullscreen_pipeline(
            window,
            post_vertex_shader_path,
            path,
            shader_format,
            SDL_GPU_TEXTUREFORMAT_R32_FLOAT,
            1u,
            &runtime->scene_distance_pipeline,
            report
        );
}

static bool btbh_gpu_scene_ensure_scene_distance_target(
    BTBHGPUWindow *window,
    uint32_t width,
    uint32_t height,
    BTBHGPUSceneReport *report
) {
    BTBHGPUSceneRuntime *runtime = window->scene_runtime;
    if (runtime->scene_distance_texture != NULL &&
        runtime->scene_distance_width == width &&
        runtime->scene_distance_height == height) {
        return true;
    }
    SDL_GPUDevice *device = window->device->gpu;
    if (runtime->scene_distance_texture != NULL) {
        SDL_ReleaseGPUTexture(device, runtime->scene_distance_texture);
        runtime->scene_distance_texture = NULL;
    }
    SDL_GPUTextureCreateInfo info;
    SDL_zero(info);
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
    info.usage =
        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
        SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1u;
    info.num_levels = 1u;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    runtime->scene_distance_texture = SDL_CreateGPUTexture(device, &info);
    if (runtime->scene_distance_texture == NULL) {
        runtime->scene_distance_width = 0u;
        runtime->scene_distance_height = 0u;
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return false;
    }
    runtime->scene_distance_width = width;
    runtime->scene_distance_height = height;
    return true;
}

/* The copy refracting batches sample: the HDR scene at the render size. */
static bool btbh_gpu_scene_ensure_refraction_target(
    BTBHGPUWindow *window,
    uint32_t width,
    uint32_t height,
    BTBHGPUSceneReport *report
) {
    BTBHGPUSceneRuntime *runtime = window->scene_runtime;
    if (runtime->refraction_texture != NULL &&
        runtime->refraction_width == width &&
        runtime->refraction_height == height) {
        return true;
    }
    SDL_GPUDevice *device = window->device->gpu;
    if (runtime->refraction_texture != NULL) {
        SDL_ReleaseGPUTexture(device, runtime->refraction_texture);
        runtime->refraction_texture = NULL;
    }
    SDL_GPUTextureCreateInfo info;
    SDL_zero(info);
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    info.usage =
        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
        SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1u;
    info.num_levels = 1u;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    runtime->refraction_texture = SDL_CreateGPUTexture(device, &info);
    if (runtime->refraction_texture == NULL) {
        runtime->refraction_width = 0u;
        runtime->refraction_height = 0u;
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return false;
    }
    runtime->refraction_width = width;
    runtime->refraction_height = height;
    return true;
}

/* Soft particles fade where they meet the scene behind them; the distance pass runs only on
   frames that draw one. */
static bool btbh_gpu_scene_batches_have_soft_particles(
    const BTBHGPUSceneGeometryBatch *batches,
    uint32_t batch_count
) {
    for (uint32_t index = 0u; index < batch_count; ++index) {
        if (batches[index].surface_variant ==
                BTBH_GPU_SCENE_SURFACE_SOFT_PARTICLE &&
            btbh_gpu_scene_uses_transparent_pass(
                batches[index].blend_mode,
                batches[index].transparency_enabled != 0u
            )) {
            return true;
        }
    }
    return false;
}

/* Refracting batches draw in a pass of their own after the transparent one, over a copy of the scene. */
static bool btbh_gpu_scene_batches_have_refraction(
    const BTBHGPUSceneGeometryBatch *batches,
    uint32_t batch_count,
    const uint8_t *scene_visibility
) {
    for (uint32_t index = 0u; index < batch_count; ++index) {
        if (batches[index].surface_variant ==
                BTBH_GPU_SCENE_SURFACE_REFRACT &&
            (scene_visibility == NULL || scene_visibility[index] != 0u)) {
            return true;
        }
    }
    return false;
}

static bool btbh_gpu_scene_ensure_brdf_lookup(
    BTBHGPUWindow *window,
    const char *fragment_shader_path,
    BTBHGPUSceneReport *report
) {
    BTBHGPUSceneRuntime *runtime = window->scene_runtime;
    if (
        runtime->brdf_lut_texture != NULL &&
        runtime->brdf_lut_sampler != NULL
    ) {
        return true;
    }

    char path[BTBH_GPU_SCENE_PATH_CAPACITY];
    if (!btbh_gpu_scene_sibling_path(
            fragment_shader_path,
            "brdf_lut_512_rg16_float.raw",
            path
        )) {
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            "Could not resolve the BRDF lookup path"
        );
        return false;
    }
    size_t source_size = 0u;
    void *source_pixels = SDL_LoadFile(path, &source_size);
    const uint32_t width = 512u;
    const uint32_t height = 512u;
    const uint32_t byte_count =
        width * height * 2u * (uint32_t)sizeof(uint16_t);
    if (source_pixels == NULL || source_size != byte_count) {
        SDL_free(source_pixels);
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            path
        );
        return false;
    }

    SDL_GPUDevice *device = window->device->gpu;
    if (!SDL_GPUTextureSupportsFormat(
            device,
            SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT,
            SDL_GPU_TEXTURETYPE_2D,
            SDL_GPU_TEXTUREUSAGE_SAMPLER
        )) {
        SDL_free(source_pixels);
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_MISSING_CAPABILITY,
            "GPU scene requires an RG16 float BRDF lookup texture"
        );
        return false;
    }

    SDL_GPUTextureCreateInfo texture_info;
    SDL_zero(texture_info);
    texture_info.type = SDL_GPU_TEXTURETYPE_2D;
    texture_info.format = SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT;
    texture_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    texture_info.width = width;
    texture_info.height = height;
    texture_info.layer_count_or_depth = 1u;
    texture_info.num_levels = 1u;
    texture_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture *texture =
        SDL_CreateGPUTexture(device, &texture_info);

    SDL_GPUSamplerCreateInfo sampler_info;
    SDL_zero(sampler_info);
    sampler_info.min_filter = SDL_GPU_FILTER_LINEAR;
    sampler_info.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampler_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler_info.address_mode_u =
        SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_v =
        SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_w =
        SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    SDL_GPUSampler *sampler =
        SDL_CreateGPUSampler(device, &sampler_info);

    SDL_GPUTransferBufferCreateInfo transfer_info;
    SDL_zero(transfer_info);
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size = byte_count;
    SDL_GPUTransferBuffer *transfer =
        SDL_CreateGPUTransferBuffer(device, &transfer_info);
    if (texture == NULL || sampler == NULL || transfer == NULL) {
        SDL_free(source_pixels);
        if (transfer != NULL) {
            SDL_ReleaseGPUTransferBuffer(device, transfer);
        }
        if (sampler != NULL) {
            SDL_ReleaseGPUSampler(device, sampler);
        }
        if (texture != NULL) {
            SDL_ReleaseGPUTexture(device, texture);
        }
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return false;
    }

    void *mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
    if (mapped == NULL) {
        SDL_free(source_pixels);
        SDL_ReleaseGPUTransferBuffer(device, transfer);
        SDL_ReleaseGPUSampler(device, sampler);
        SDL_ReleaseGPUTexture(device, texture);
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return false;
    }
    (void)memcpy(mapped, source_pixels, byte_count);
    SDL_UnmapGPUTransferBuffer(device, transfer);
    SDL_free(source_pixels);

    SDL_GPUCommandBuffer *command =
        SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass *copy = command != NULL
        ? SDL_BeginGPUCopyPass(command)
        : NULL;
    if (copy == NULL) {
        if (command != NULL) {
            (void)SDL_CancelGPUCommandBuffer(command);
        }
        SDL_ReleaseGPUTransferBuffer(device, transfer);
        SDL_ReleaseGPUSampler(device, sampler);
        SDL_ReleaseGPUTexture(device, texture);
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return false;
    }
    SDL_GPUTextureTransferInfo upload;
    SDL_zero(upload);
    upload.transfer_buffer = transfer;
    upload.pixels_per_row = width;
    upload.rows_per_layer = height;
    SDL_GPUTextureRegion destination;
    SDL_zero(destination);
    destination.texture = texture;
    destination.w = width;
    destination.h = height;
    destination.d = 1u;
    SDL_UploadToGPUTexture(copy, &upload, &destination, false);
    SDL_EndGPUCopyPass(copy);
    const bool submitted = SDL_SubmitGPUCommandBuffer(command);
    SDL_ReleaseGPUTransferBuffer(device, transfer);
    if (!submitted) {
        SDL_ReleaseGPUSampler(device, sampler);
        SDL_ReleaseGPUTexture(device, texture);
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return false;
    }
    runtime->brdf_lut_texture = texture;
    runtime->brdf_lut_sampler = sampler;
    return true;
}

static bool btbh_gpu_scene_ensure_smaa_resources(
    BTBHGPUWindow *window,
    const char *post_vertex_shader_path,
    const char *post_fragment_shader_path,
    BTBHGPUShaderFormat shader_format,
    uint8_t quality,
    BTBHGPUSceneReport *report
) {
    if (quality < 1u ||
        quality > BTBH_GPU_SCENE_SMAA_QUALITY_COUNT) {
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            "Invalid SMAA quality"
        );
        return false;
    }
    BTBHGPUSceneRuntime *runtime = window->scene_runtime;
    const char *extension = shader_format == BTBH_GPU_SHADER_FORMAT_METALLIB
        ? "metallib"
        : "spv";
    char path[BTBH_GPU_SCENE_PATH_CAPACITY];
    if (runtime->smaa_area_texture == NULL) {
        if (!btbh_gpu_scene_sibling_path(
                post_fragment_shader_path,
                "smaa_area_160x560_rg8.raw",
                path
            ) ||
            !btbh_gpu_scene_load_smaa_lookup(
                window,
                path,
                160u,
                560u,
                2u,
                BTBH_GPU_TEXTURE_FILTER_LINEAR,
                &runtime->smaa_area_texture,
                report
            )) {
            return false;
        }
    }
    if (runtime->smaa_search_texture == NULL) {
        if (!btbh_gpu_scene_sibling_path(
                post_fragment_shader_path,
                "smaa_search_64x16_r8.raw",
                path
            ) ||
            !btbh_gpu_scene_load_smaa_lookup(
                window,
                path,
                64u,
                16u,
                1u,
                BTBH_GPU_TEXTURE_FILTER_NEAREST,
                &runtime->smaa_search_texture,
                report
            )) {
            return false;
        }
    }
    const uint32_t index = (uint32_t)quality - 1u;
    const char *quality_name = quality == 1u ? "medium" : "high";
    if (runtime->smaa_edge_pipelines[index] == NULL) {
        char filename[128];
        (void)snprintf(
            filename,
            sizeof(filename),
            "production_smaa_edge_%s.frag.%s",
            quality_name,
            extension
        );
        if (!btbh_gpu_scene_sibling_path(
                post_fragment_shader_path,
                filename,
                path
            ) ||
            !btbh_gpu_scene_create_fullscreen_pipeline(
                window,
                post_vertex_shader_path,
                path,
                shader_format,
                SDL_GPU_TEXTUREFORMAT_R8G8_UNORM,
                1u,
                &runtime->smaa_edge_pipelines[index],
                report
            )) {
            return false;
        }
    }
    if (runtime->smaa_weight_pipelines[index] == NULL) {
        char filename[128];
        (void)snprintf(
            filename,
            sizeof(filename),
            "production_smaa_weights_%s.frag.%s",
            quality_name,
            extension
        );
        if (!btbh_gpu_scene_sibling_path(
                post_fragment_shader_path,
                filename,
                path
            ) ||
            !btbh_gpu_scene_create_fullscreen_pipeline(
                window,
                post_vertex_shader_path,
                path,
                shader_format,
                SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                3u,
                &runtime->smaa_weight_pipelines[index],
                report
            )) {
            return false;
        }
    }
    if (runtime->smaa_blend_pipeline == NULL) {
        char filename[128];
        (void)snprintf(
            filename,
            sizeof(filename),
            "production_smaa_blend.frag.%s",
            extension
        );
        if (!btbh_gpu_scene_sibling_path(
                post_fragment_shader_path,
                filename,
                path
            ) ||
            !btbh_gpu_scene_create_fullscreen_pipeline(
                window,
                post_vertex_shader_path,
                path,
                shader_format,
                SDL_GetGPUSwapchainTextureFormat(
                    window->device->gpu,
                    window->window
                ),
                2u,
                &runtime->smaa_blend_pipeline,
                report
            )) {
            return false;
        }
    }
    return true;
}

static bool btbh_gpu_scene_ensure_fxaa_pipeline(
    BTBHGPUWindow *window,
    const char *post_vertex_shader_path,
    const char *post_fragment_shader_path,
    BTBHGPUShaderFormat shader_format,
    BTBHGPUSceneReport *report
) {
    BTBHGPUSceneRuntime *runtime = window->scene_runtime;
    if (runtime->fxaa_pipeline != NULL) {
        return true;
    }
    const char *extension = shader_format == BTBH_GPU_SHADER_FORMAT_METALLIB
        ? "metallib"
        : "spv";
    char filename[128];
    (void)snprintf(
        filename,
        sizeof(filename),
        "production_fxaa.frag.%s",
        extension
    );
    char path[BTBH_GPU_SCENE_PATH_CAPACITY];
    if (!btbh_gpu_scene_sibling_path(
            post_fragment_shader_path,
            filename,
            path
        ) ||
        !btbh_gpu_scene_create_fullscreen_pipeline(
            window,
            post_vertex_shader_path,
            path,
            shader_format,
            SDL_GetGPUSwapchainTextureFormat(
                window->device->gpu,
                window->window
            ),
            1u,
            &runtime->fxaa_pipeline,
            report
        )) {
        return false;
    }
    return true;
}

static SDL_GPUGraphicsPipeline **btbh_gpu_scene_pipeline_slot(
    BTBHGPUSceneRuntime *runtime,
    bool auxiliary_geometry_outputs,
    bool temporal,
    bool radiance_cascades_active,
    bool shadows_active,
    bool omni_shadows_active,
    bool animated,
    const BTBHGPUSceneGeometryBatch *batch,
    BTBHGPUSceneStencilVariant stencil_variant
) {
    const uint32_t blend = (uint32_t)batch->blend_mode;
    const uint32_t depth = (uint32_t)batch->depth_compare;
    const uint32_t transparent =
        (uint32_t)batch->transparency_enabled;
    const uint32_t cull = (uint32_t)batch->cull_mode;
    const uint32_t stencil = (uint32_t)stencil_variant;
    const uint32_t fast_unlit =
        !radiance_cascades_active &&
        btbh_gpu_scene_uses_fast_unlit_pipeline(batch) ? 1u : 0u;
    const uint32_t color_target_variant =
        (temporal ? 4u : 0u) +
        (radiance_cascades_active ? 2u : 0u) +
        (auxiliary_geometry_outputs ? 1u : 0u);
    if (shadows_active) {
        if (!omni_shadows_active) {
            return animated
                ? &runtime->non_omni_shadowed_animated_model_pipelines
                    [color_target_variant][blend][depth][transparent]
                    [cull][stencil][fast_unlit]
                : &runtime->non_omni_shadowed_model_pipelines
                    [color_target_variant][blend][depth][transparent]
                    [cull][stencil][fast_unlit];
        }
        return animated
            ? &runtime->animated_model_pipelines
                [color_target_variant][blend][depth][transparent]
                [cull][stencil][fast_unlit]
            : &runtime->model_pipelines
                [color_target_variant][blend][depth][transparent]
                [cull][stencil][fast_unlit];
    }
    return animated
        ? &runtime->unshadowed_animated_model_pipelines
            [color_target_variant][blend][depth][transparent]
            [cull][stencil][fast_unlit]
        : &runtime->unshadowed_model_pipelines
            [color_target_variant][blend][depth][transparent]
            [cull][stencil][fast_unlit];
}

static bool btbh_gpu_scene_ensure_model_pipeline(
    BTBHGPUWindow *window,
    const char *vertex_shader_path,
    const char *animated_vertex_shader_path,
    const char *fragment_shader_path,
    const char *unshadowed_fragment_shader_path,
    BTBHGPUShaderFormat project_format,
    bool auxiliary_geometry_outputs,
    bool temporal,
    bool radiance_cascades_active,
    bool shadows_active,
    bool omni_shadows_active,
    bool animated,
    const BTBHGPUSceneGeometryBatch *batch,
    BTBHGPUSceneReport *report
) {
    BTBHGPUSceneStencilVariant stencil_variant;
    if (!btbh_gpu_scene_stencil_variant(batch, &stencil_variant)) {
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            "Unsupported GPU scene stencil material state"
        );
        return false;
    }
    SDL_GPUGraphicsPipeline **slot = btbh_gpu_scene_pipeline_slot(
        window->scene_runtime,
        auxiliary_geometry_outputs,
        temporal,
        radiance_cascades_active,
        shadows_active,
        omni_shadows_active,
        animated,
        batch,
        stencil_variant
    );
    if (*slot != NULL) {
        return true;
    }
    const bool fast_unlit =
        !radiance_cascades_active &&
        btbh_gpu_scene_uses_fast_unlit_pipeline(batch);
    const char *selected_fragment_shader_path =
        shadows_active
            ? fragment_shader_path
            : unshadowed_fragment_shader_path;
    char non_omni_fragment_shader_path[
        BTBH_GPU_SCENE_PATH_CAPACITY
    ];
    if (shadows_active && !omni_shadows_active) {
        const char *extension =
            project_format == BTBH_GPU_SHADER_FORMAT_METALLIB
                ? "metallib"
                : "spv";
        char filename[128];
        (void)snprintf(
            filename,
            sizeof(filename),
            "production_model_2d_shadows.frag.%s",
            extension
        );
        if (!btbh_gpu_scene_sibling_path(
                fragment_shader_path,
                filename,
                non_omni_fragment_shader_path
            )) {
            btbh_gpu_scene_error(
                report,
                BTBH_GPU_INVALID_ARGUMENT,
                "2D-shadow fragment shader path is too long"
            );
            return false;
        }
        selected_fragment_shader_path =
            non_omni_fragment_shader_path;
    }
    char fast_unlit_fragment_shader_path[
        BTBH_GPU_SCENE_PATH_CAPACITY
    ];
    if (radiance_cascades_active) {
        const char *extension =
            project_format == BTBH_GPU_SHADER_FORMAT_METALLIB
                ? "metallib"
                : "spv";
        char filename[128];
        const char *rc_fragment_name = !shadows_active
            ? "production_model_rc_unshadowed.frag.%s"
            : (omni_shadows_active
                ? "production_model_rc.frag.%s"
                : "production_model_rc_2d_shadows.frag.%s");
        (void)snprintf(
            filename,
            sizeof(filename),
            rc_fragment_name,
            extension
        );
        if (!btbh_gpu_scene_sibling_path(
                selected_fragment_shader_path,
                filename,
                fast_unlit_fragment_shader_path
            )) {
            btbh_gpu_scene_error(
                report,
                BTBH_GPU_INVALID_ARGUMENT,
                "RC material fragment shader path is too long"
            );
            return false;
        }
        selected_fragment_shader_path =
            fast_unlit_fragment_shader_path;
    } else if (fast_unlit) {
        const char *extension =
            project_format == BTBH_GPU_SHADER_FORMAT_METALLIB
                ? "metallib"
                : "spv";
        char filename[128];
        (void)snprintf(
            filename,
            sizeof(filename),
            "production_model_unlit.frag.%s",
            extension
        );
        if (!btbh_gpu_scene_sibling_path(
                selected_fragment_shader_path,
                filename,
                fast_unlit_fragment_shader_path
            )) {
            btbh_gpu_scene_error(
                report,
                BTBH_GPU_INVALID_ARGUMENT,
                "Fast unlit fragment shader path is too long"
            );
            return false;
        }
        selected_fragment_shader_path =
            fast_unlit_fragment_shader_path;
    }
    return btbh_gpu_scene_create_model_pipeline(
        window,
        animated ? animated_vertex_shader_path : vertex_shader_path,
        selected_fragment_shader_path,
        project_format,
        animated,
        fast_unlit
            ? 2u
            : (shadows_active
                ? (omni_shadows_active ? 10u : 9u)
                : 5u),
        auxiliary_geometry_outputs,
        temporal,
        batch,
        stencil_variant,
        slot,
        report
    );
}

static bool btbh_gpu_scene_create_shadow_pipelines(
    BTBHGPUWindow *window,
    const char *fast_shadow_vertex_shader_path,
    const char *fast_shadow_fragment_shader_path,
    const char *shadow_vertex_shader_path,
    const char *animated_shadow_vertex_shader_path,
    const char *shadow_fragment_shader_path,
    BTBHGPUShaderFormat project_format,
    BTBHGPUSceneReport *report
) {
    SDL_GPUDevice *device = window->device->gpu;
    const SDL_GPUShaderFormat format =
        btbh_gpu_scene_shader_format(project_format);
    const char *entrypoint =
        project_format == BTBH_GPU_SHADER_FORMAT_METALLIB
            ? "main0"
            : "main";
    SDL_GPUShader *shadow_vertex_shader = btbh_gpu_scene_load_shader(
        device,
        shadow_vertex_shader_path,
        entrypoint,
        format,
        SDL_GPU_SHADERSTAGE_VERTEX,
        1u,
        0u,
        1u,
        report
    );
    if (shadow_vertex_shader == NULL) {
        return false;
    }
    SDL_GPUShader *animated_shadow_vertex_shader =
        btbh_gpu_scene_load_shader(
            device,
            animated_shadow_vertex_shader_path,
            entrypoint,
            format,
            SDL_GPU_SHADERSTAGE_VERTEX,
            2u,
            0u,
            1u,
            report
        );
    if (animated_shadow_vertex_shader == NULL) {
        SDL_ReleaseGPUShader(device, shadow_vertex_shader);
        return false;
    }
    SDL_GPUShader *shadow_fragment_shader =
        btbh_gpu_scene_load_shader(
            device,
            shadow_fragment_shader_path,
            entrypoint,
            format,
            SDL_GPU_SHADERSTAGE_FRAGMENT,
            0u,
            0u,
            0u,
            report
        );
    if (shadow_fragment_shader == NULL) {
        SDL_ReleaseGPUShader(
            device,
            animated_shadow_vertex_shader
        );
        SDL_ReleaseGPUShader(device, shadow_vertex_shader);
        return false;
    }
    SDL_GPUShader *fast_shadow_vertex_shader =
        btbh_gpu_scene_load_shader(
            device,
            fast_shadow_vertex_shader_path,
            entrypoint,
            format,
            SDL_GPU_SHADERSTAGE_VERTEX,
            0u,
            0u,
            1u,
            report
        );
    if (fast_shadow_vertex_shader == NULL) {
        SDL_ReleaseGPUShader(device, shadow_fragment_shader);
        SDL_ReleaseGPUShader(device, animated_shadow_vertex_shader);
        SDL_ReleaseGPUShader(device, shadow_vertex_shader);
        return false;
    }
    SDL_GPUShader *fast_shadow_fragment_shader =
        btbh_gpu_scene_load_shader(
            device,
            fast_shadow_fragment_shader_path,
            entrypoint,
            format,
            SDL_GPU_SHADERSTAGE_FRAGMENT,
            0u,
            0u,
            0u,
            report
        );
    if (fast_shadow_fragment_shader == NULL) {
        SDL_ReleaseGPUShader(device, fast_shadow_vertex_shader);
        SDL_ReleaseGPUShader(device, shadow_fragment_shader);
        SDL_ReleaseGPUShader(device, animated_shadow_vertex_shader);
        SDL_ReleaseGPUShader(device, shadow_vertex_shader);
        return false;
    }
    SDL_GPUVertexBufferDescription vertex_buffers[2];
    SDL_GPUVertexAttribute shadow_attributes[3];
    SDL_GPUVertexAttribute fast_shadow_attributes[5];
    SDL_GPUGraphicsPipelineCreateInfo info;
    SDL_zeroa(vertex_buffers);
    SDL_zeroa(shadow_attributes);
    SDL_zeroa(fast_shadow_attributes);
    SDL_zero(info);
    vertex_buffers[0].slot = 0u;
    vertex_buffers[0].pitch = BTBH_GPU_MODEL_VERTEX_STRIDE;
    vertex_buffers[0].input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    vertex_buffers[0].instance_step_rate = 0u;
    vertex_buffers[1].slot = 1u;
    vertex_buffers[1].pitch = BTBH_GPU_SCENE_INSTANCE_STRIDE;
    vertex_buffers[1].input_rate = SDL_GPU_VERTEXINPUTRATE_INSTANCE;
    vertex_buffers[1].instance_step_rate = 0u;
    shadow_attributes[0].location = 0u;
    shadow_attributes[0].buffer_slot = 0u;
    shadow_attributes[0].format =
        SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    shadow_attributes[0].offset = 0u;
    shadow_attributes[1].location = 2u;
    shadow_attributes[1].buffer_slot = 0u;
    shadow_attributes[1].format =
        SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    shadow_attributes[1].offset = BTBH_GPU_MODEL_JOINTS_OFFSET;
    shadow_attributes[2].location = 3u;
    shadow_attributes[2].buffer_slot = 0u;
    shadow_attributes[2].format =
        SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    shadow_attributes[2].offset = BTBH_GPU_MODEL_WEIGHTS_OFFSET;
    fast_shadow_attributes[0] = shadow_attributes[0];
    for (uint32_t column = 0u; column < 4u; ++column) {
        fast_shadow_attributes[column + 1u].location = column + 1u;
        fast_shadow_attributes[column + 1u].buffer_slot = 1u;
        fast_shadow_attributes[column + 1u].format =
            SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
        fast_shadow_attributes[column + 1u].offset =
            column * 4u * sizeof(float);
    }
    info.vertex_shader = shadow_vertex_shader;
    info.fragment_shader = shadow_fragment_shader;
    info.vertex_input_state.vertex_buffer_descriptions = vertex_buffers;
    info.vertex_input_state.num_vertex_buffers = 1u;
    info.vertex_input_state.vertex_attributes = shadow_attributes;
    info.vertex_input_state.num_vertex_attributes = 1u;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.rasterizer_state.front_face =
        SDL_GPU_FRONTFACE_CLOCKWISE;
    info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
    info.depth_stencil_state.enable_depth_test = true;
    info.depth_stencil_state.enable_depth_write = true;
    info.target_info.num_color_targets = 0u;
    info.target_info.depth_stencil_format =
        SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    info.target_info.has_depth_stencil_target = true;
    for (uint32_t cull_index = 0u;
         cull_index < BTBH_GPU_SCENE_CULL_COUNT;
         ++cull_index) {
        info.rasterizer_state.cull_mode =
            btbh_gpu_scene_cull_mode((BTBHGPUSceneCullMode)cull_index);
        info.vertex_shader = shadow_vertex_shader;
        info.fragment_shader = shadow_fragment_shader;
        info.vertex_input_state.num_vertex_attributes = 1u;
        window->scene_runtime->shadow_pipelines[cull_index] =
            SDL_CreateGPUGraphicsPipeline(device, &info);
        info.fragment_shader = fast_shadow_fragment_shader;
        window->scene_runtime
            ->non_discard_shadow_pipelines[cull_index] =
                SDL_CreateGPUGraphicsPipeline(device, &info);
        info.vertex_shader = fast_shadow_vertex_shader;
        info.vertex_input_state.num_vertex_buffers = 2u;
        info.vertex_input_state.vertex_attributes =
            fast_shadow_attributes;
        info.vertex_input_state.num_vertex_attributes = 5u;
        window->scene_runtime->fast_shadow_pipelines[cull_index] =
            SDL_CreateGPUGraphicsPipeline(device, &info);
        info.vertex_shader = animated_shadow_vertex_shader;
        info.fragment_shader = shadow_fragment_shader;
        info.vertex_input_state.num_vertex_buffers = 1u;
        info.vertex_input_state.vertex_attributes = shadow_attributes;
        info.vertex_input_state.num_vertex_attributes = 3u;
        window->scene_runtime
            ->animated_shadow_pipelines[cull_index] =
                SDL_CreateGPUGraphicsPipeline(device, &info);
        info.fragment_shader = fast_shadow_fragment_shader;
        window->scene_runtime
            ->animated_non_discard_shadow_pipelines[cull_index] =
                SDL_CreateGPUGraphicsPipeline(device, &info);
        if (
            window->scene_runtime->shadow_pipelines[cull_index] == NULL ||
            window->scene_runtime
                ->fast_shadow_pipelines[cull_index] == NULL ||
            window->scene_runtime
                ->non_discard_shadow_pipelines[cull_index] == NULL ||
            window->scene_runtime
                ->animated_shadow_pipelines[cull_index] == NULL ||
            window->scene_runtime
                ->animated_non_discard_shadow_pipelines[cull_index] ==
                    NULL
        ) {
            SDL_ReleaseGPUShader(
                device,
                fast_shadow_fragment_shader
            );
            SDL_ReleaseGPUShader(device, fast_shadow_vertex_shader);
            SDL_ReleaseGPUShader(
                device,
                animated_shadow_vertex_shader
            );
            SDL_ReleaseGPUShader(device, shadow_vertex_shader);
            SDL_ReleaseGPUShader(device, shadow_fragment_shader);
            btbh_gpu_scene_error(
                report,
                BTBH_GPU_DEVICE_FAILURE,
                SDL_GetError()
            );
            return false;
        }
    }
    SDL_ReleaseGPUShader(device, fast_shadow_fragment_shader);
    SDL_ReleaseGPUShader(device, fast_shadow_vertex_shader);
    SDL_ReleaseGPUShader(device, animated_shadow_vertex_shader);
    SDL_ReleaseGPUShader(device, shadow_vertex_shader);
    SDL_ReleaseGPUShader(device, shadow_fragment_shader);
    return true;
}

static bool btbh_gpu_scene_ensure_runtime(
    BTBHGPUWindow *window,
    BTBHGPUSceneReport *report
) {
    if (window->scene_runtime != NULL) {
        return true;
    }
    window->scene_runtime = (BTBHGPUSceneRuntime *)calloc(
        1u,
        sizeof(BTBHGPUSceneRuntime)
    );
    if (window->scene_runtime == NULL) {
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_PLATFORM_FAILURE,
            "Could not allocate the GPU scene runtime"
        );
        return false;
    }
    btbh_gpu_radiance_cascades_runtime_initialize(
        &window->scene_runtime->radiance_cascades
    );
    SDL_GPUDevice *device = window->device->gpu;
    const SDL_GPUTextureUsageFlags depth_usage =
        SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET |
        SDL_GPU_TEXTUREUSAGE_SAMPLER;
    if (SDL_GPUTextureSupportsFormat(
            device,
            SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT,
            SDL_GPU_TEXTURETYPE_2D,
            depth_usage
        )) {
        window->scene_runtime->depth_format =
            SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;
    } else if (SDL_GPUTextureSupportsFormat(
            device,
            SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT,
            SDL_GPU_TEXTURETYPE_2D,
            depth_usage
        )) {
        window->scene_runtime->depth_format =
            SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
    } else {
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_MISSING_CAPABILITY,
            "GPU scene requires a depth-stencil texture format"
        );
        btbh_gpu_window_release_scene(window);
        return false;
    }
    const SDL_GPUTextureUsageFlags scene_color_usage =
        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
        SDL_GPU_TEXTUREUSAGE_SAMPLER;
    if (SDL_GPUTextureSupportsFormat(
            device,
            SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
            SDL_GPU_TEXTURETYPE_2D,
            scene_color_usage
        )) {
        window->scene_runtime->scene_color_format =
            SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    } else {
        window->scene_runtime->scene_color_format =
            SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    }
    const uint8_t white_pixel[4] = {255u, 255u, 255u, 255u};
    BTBHGPUTextureReport texture_report;
    window->scene_runtime->fallback_texture =
        btbh_gpu_window_texture_create_rgba8(
            window,
            white_pixel,
            1u,
            1u,
            BTBH_GPU_TEXTURE_FILTER_NEAREST,
            BTBH_GPU_TEXTURE_WRAP_CLAMP,
            &texture_report
        );
    if (window->scene_runtime->fallback_texture == NULL) {
        btbh_gpu_scene_error(
            report,
            texture_report.result,
            texture_report.message
        );
        btbh_gpu_window_release_scene(window);
        return false;
    }
    const uint8_t normal_pixel[4] = {127u, 127u, 255u, 0u};
    window->scene_runtime->normal_fallback_texture =
        btbh_gpu_window_texture_create_rgba8(
            window,
            normal_pixel,
            1u,
            1u,
            BTBH_GPU_TEXTURE_FILTER_NEAREST,
            BTBH_GPU_TEXTURE_WRAP_CLAMP,
            &texture_report
        );
    if (window->scene_runtime->normal_fallback_texture == NULL) {
        btbh_gpu_scene_error(
            report,
            texture_report.result,
            texture_report.message
        );
        btbh_gpu_window_release_scene(window);
        return false;
    }
    SDL_GPUSamplerCreateInfo sampler_info;
    SDL_zero(sampler_info);
    sampler_info.min_filter = SDL_GPU_FILTER_LINEAR;
    sampler_info.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampler_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler_info.address_mode_u =
        SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_v =
        SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_w =
        SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    window->scene_runtime->scene_color_sampler =
        SDL_CreateGPUSampler(device, &sampler_info);
    if (window->scene_runtime->scene_color_sampler == NULL) {
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        btbh_gpu_window_release_scene(window);
        return false;
    }
    return true;
}

static bool btbh_gpu_scene_ensure_shadow_resources(
    BTBHGPUWindow *window,
    const char *fast_shadow_vertex_shader_path,
    const char *fast_shadow_fragment_shader_path,
    const char *shadow_vertex_shader_path,
    const char *animated_shadow_vertex_shader_path,
    const char *shadow_fragment_shader_path,
    BTBHGPUShaderFormat shader_format,
    BTBHGPUSceneReport *report
) {
    BTBHGPUSceneRuntime *runtime = window->scene_runtime;
    SDL_GPUDevice *device = window->device->gpu;
    /* The primary directional map's size: as set (btbh_gpu_window_scene_set_directional_shadow_size), made
       anew when that changes. */
    const uint32_t primary_width =
        window->scene_directional_shadow_size[0] != 0u
            ? window->scene_directional_shadow_size[0]
            : BTBH_GPU_PRIMARY_DIRECTIONAL_SHADOW_MAP_SIZE;
    const uint32_t primary_height =
        window->scene_directional_shadow_size[1] != 0u
            ? window->scene_directional_shadow_size[1]
            : BTBH_GPU_PRIMARY_DIRECTIONAL_SHADOW_MAP_SIZE;
    if (runtime->directional_shadow_textures[0] != NULL &&
        (runtime->directional_shadow_width != primary_width ||
         runtime->directional_shadow_height != primary_height)) {
        SDL_ReleaseGPUTexture(
            device,
            runtime->directional_shadow_textures[0]
        );
        runtime->directional_shadow_textures[0] = NULL;
        runtime->retained_shadow_ready[0] = 0u;
    }
    if (btbh_gpu_scene_shadow_resources_ready(runtime)) {
        return true;
    }
    if (runtime->shadow_pipelines[0] == NULL &&
        !btbh_gpu_scene_create_shadow_pipelines(
            window,
            fast_shadow_vertex_shader_path,
            fast_shadow_fragment_shader_path,
            shadow_vertex_shader_path,
            animated_shadow_vertex_shader_path,
            shadow_fragment_shader_path,
            shader_format,
            report
        )) {
        btbh_gpu_window_release_scene(window);
        return false;
    }

    SDL_GPUTextureCreateInfo shadow_info;
    SDL_zero(shadow_info);
    shadow_info.type = SDL_GPU_TEXTURETYPE_2D;
    shadow_info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    shadow_info.usage =
        SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET |
        SDL_GPU_TEXTUREUSAGE_SAMPLER;
    shadow_info.layer_count_or_depth = 1u;
    shadow_info.num_levels = 1u;
    shadow_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    const uint32_t shadow_map_widths[3] = {
        primary_width,
        BTBH_GPU_DIRECTIONAL_SHADOW_MAP_SIZE,
        BTBH_GPU_SPOT_SHADOW_MAP_SIZE
    };
    const uint32_t shadow_map_heights[3] = {
        primary_height,
        BTBH_GPU_DIRECTIONAL_SHADOW_MAP_SIZE,
        BTBH_GPU_SPOT_SHADOW_MAP_SIZE
    };
    for (uint32_t index = 0u; index < 3u; ++index) {
        if (runtime->directional_shadow_textures[index] != NULL) {
            continue;
        }
        shadow_info.width = shadow_map_widths[index];
        shadow_info.height = shadow_map_heights[index];
        runtime->directional_shadow_textures[index] =
            SDL_CreateGPUTexture(device, &shadow_info);
        if (runtime->directional_shadow_textures[index] == NULL) {
            btbh_gpu_scene_error(
                report,
                BTBH_GPU_DEVICE_FAILURE,
                SDL_GetError()
            );
            btbh_gpu_window_release_scene(window);
            return false;
        }
    }
    runtime->directional_shadow_width = primary_width;
    runtime->directional_shadow_height = primary_height;
    SDL_GPUSamplerCreateInfo sampler_info;
    SDL_zero(sampler_info);
    sampler_info.min_filter = SDL_GPU_FILTER_LINEAR;
    sampler_info.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampler_info.mipmap_mode =
        SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler_info.address_mode_u =
        SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_v =
        SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_w =
        SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    if (runtime->directional_shadow_sampler == NULL) {
        runtime->directional_shadow_sampler =
            SDL_CreateGPUSampler(device, &sampler_info);
    }
    /* The same with a comparison: a tap passes where its depth is at most the stored one (lit), the
       four texels round it compared and filtered. */
    sampler_info.enable_compare = true;
    sampler_info.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    if (runtime->directional_shadow_compare_sampler == NULL) {
        runtime->directional_shadow_compare_sampler =
            SDL_CreateGPUSampler(device, &sampler_info);
    }
    if (runtime->directional_shadow_sampler == NULL ||
        runtime->directional_shadow_compare_sampler == NULL) {
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        btbh_gpu_window_release_scene(window);
        return false;
    }
    return true;
}

static bool btbh_gpu_scene_ensure_omni_shadow_texture(
    BTBHGPUWindow *window
) {
    BTBHGPUSceneRuntime *runtime = window->scene_runtime;
    if (runtime->omni_shadow_texture != NULL) {
        return true;
    }
    if (runtime->omni_shadow_unavailable != 0u) {
        return false;
    }

    SDL_GPUDevice *device = window->device->gpu;
    const SDL_GPUTextureUsageFlags usage =
        SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET |
        SDL_GPU_TEXTUREUSAGE_SAMPLER;
    if (!SDL_GPUTextureSupportsFormat(
            device,
            SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
            SDL_GPU_TEXTURETYPE_CUBE,
            usage
        )) {
        runtime->omni_shadow_unavailable = 1u;
        SDL_LogWarn(
            SDL_LOG_CATEGORY_RENDER,
            "Omni shadows disabled: sampled D32 cube textures are "
            "unavailable"
        );
        return false;
    }

    SDL_GPUTextureCreateInfo shadow_info;
    SDL_zero(shadow_info);
    shadow_info.type = SDL_GPU_TEXTURETYPE_CUBE;
    shadow_info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    shadow_info.usage = usage;
    shadow_info.width = BTBH_GPU_OMNI_SHADOW_MAP_SIZE;
    shadow_info.height = BTBH_GPU_OMNI_SHADOW_MAP_SIZE;
    shadow_info.layer_count_or_depth = BTBH_GPU_OMNI_SHADOW_FACE_COUNT;
    shadow_info.num_levels = 1u;
    shadow_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    runtime->omni_shadow_texture =
        SDL_CreateGPUTexture(device, &shadow_info);
    if (runtime->omni_shadow_texture == NULL) {
        runtime->omni_shadow_unavailable = 1u;
        SDL_LogWarn(
            SDL_LOG_CATEGORY_RENDER,
            "Omni shadows disabled: %s",
            SDL_GetError()
        );
        return false;
    }
    return true;
}

static bool btbh_gpu_scene_ensure_depth(
    BTBHGPUWindow *window,
    uint32_t width,
    uint32_t height,
    BTBHGPUSceneReport *report
) {
    BTBHGPUSceneRuntime *runtime = window->scene_runtime;
    SDL_GPUDevice *device = window->device->gpu;
    if (runtime->depth_texture != NULL &&
        runtime->depth_width == width &&
        runtime->depth_height == height) {
        return true;
    }
    if (runtime->depth_texture != NULL) {
        SDL_ReleaseGPUTexture(device, runtime->depth_texture);
        runtime->depth_texture = NULL;
    }
    SDL_GPUTextureCreateInfo info;
    SDL_zero(info);
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = runtime->depth_format;
    info.usage =
        SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET |
        SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1u;
    info.num_levels = 1u;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    runtime->depth_texture = SDL_CreateGPUTexture(device, &info);
    if (runtime->depth_texture == NULL) {
        runtime->depth_width = 0u;
        runtime->depth_height = 0u;
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return false;
    }
    runtime->depth_width = width;
    runtime->depth_height = height;
    return true;
}

static bool btbh_gpu_scene_ensure_color(
    BTBHGPUWindow *window,
    uint32_t width,
    uint32_t height,
    BTBHGPUSceneReport *report
) {
    BTBHGPUSceneRuntime *runtime = window->scene_runtime;
    SDL_GPUDevice *device = window->device->gpu;
    if (runtime->scene_color_texture != NULL &&
        runtime->ambient_texture != NULL &&
        runtime->ambient_specular_texture != NULL &&
        runtime->normal_texture != NULL &&
        runtime->emission_texture != NULL &&
        runtime->ssao_texture != NULL &&
        runtime->ssao_denoised_texture != NULL &&
        runtime->ssao_resolved_texture != NULL &&
        runtime->scene_color_width == width &&
        runtime->scene_color_height == height) {
        return true;
    }
    if (runtime->scene_color_texture != NULL) {
        SDL_ReleaseGPUTexture(
            device,
            runtime->scene_color_texture
        );
        runtime->scene_color_texture = NULL;
    }
    if (runtime->ambient_texture != NULL) {
        SDL_ReleaseGPUTexture(device, runtime->ambient_texture);
        runtime->ambient_texture = NULL;
    }
    if (runtime->ambient_specular_texture != NULL) {
        SDL_ReleaseGPUTexture(
            device,
            runtime->ambient_specular_texture
        );
        runtime->ambient_specular_texture = NULL;
    }
    if (runtime->normal_texture != NULL) {
        SDL_ReleaseGPUTexture(device, runtime->normal_texture);
        runtime->normal_texture = NULL;
    }
    if (runtime->emission_texture != NULL) {
        SDL_ReleaseGPUTexture(device, runtime->emission_texture);
        runtime->emission_texture = NULL;
    }
    if (runtime->ssao_texture != NULL) {
        SDL_ReleaseGPUTexture(device, runtime->ssao_texture);
        runtime->ssao_texture = NULL;
    }
    if (runtime->ssao_denoised_texture != NULL) {
        SDL_ReleaseGPUTexture(
            device,
            runtime->ssao_denoised_texture
        );
        runtime->ssao_denoised_texture = NULL;
    }
    if (runtime->ssao_resolved_texture != NULL) {
        SDL_ReleaseGPUTexture(device, runtime->ssao_resolved_texture);
        runtime->ssao_resolved_texture = NULL;
    }
    SDL_GPUTextureCreateInfo info;
    SDL_zero(info);
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = runtime->scene_color_format;
    info.usage =
        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
        SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1u;
    info.num_levels = 1u;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    runtime->scene_color_texture =
        SDL_CreateGPUTexture(device, &info);
    if (runtime->scene_color_texture == NULL) {
        runtime->scene_color_width = 0u;
        runtime->scene_color_height = 0u;
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return false;
    }
    runtime->ambient_texture = SDL_CreateGPUTexture(device, &info);
    if (runtime->ambient_texture == NULL) {
        SDL_ReleaseGPUTexture(device, runtime->scene_color_texture);
        runtime->scene_color_texture = NULL;
        runtime->scene_color_width = 0u;
        runtime->scene_color_height = 0u;
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return false;
    }
    runtime->ambient_specular_texture =
        SDL_CreateGPUTexture(device, &info);
    if (runtime->ambient_specular_texture == NULL) {
        SDL_ReleaseGPUTexture(device, runtime->ambient_texture);
        SDL_ReleaseGPUTexture(device, runtime->scene_color_texture);
        runtime->ambient_texture = NULL;
        runtime->scene_color_texture = NULL;
        runtime->scene_color_width = 0u;
        runtime->scene_color_height = 0u;
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return false;
    }
    runtime->ssao_resolved_texture =
        SDL_CreateGPUTexture(device, &info);
    if (runtime->ssao_resolved_texture == NULL) {
        SDL_ReleaseGPUTexture(
            device,
            runtime->ambient_specular_texture
        );
        SDL_ReleaseGPUTexture(device, runtime->ambient_texture);
        SDL_ReleaseGPUTexture(device, runtime->scene_color_texture);
        runtime->ambient_specular_texture = NULL;
        runtime->ambient_texture = NULL;
        runtime->scene_color_texture = NULL;
        runtime->scene_color_width = 0u;
        runtime->scene_color_height = 0u;
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return false;
    }
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    runtime->normal_texture = SDL_CreateGPUTexture(device, &info);
    if (runtime->normal_texture == NULL) {
        SDL_ReleaseGPUTexture(device, runtime->ssao_resolved_texture);
        SDL_ReleaseGPUTexture(
            device,
            runtime->ambient_specular_texture
        );
        SDL_ReleaseGPUTexture(device, runtime->ambient_texture);
        SDL_ReleaseGPUTexture(device, runtime->scene_color_texture);
        runtime->ssao_resolved_texture = NULL;
        runtime->ambient_specular_texture = NULL;
        runtime->ambient_texture = NULL;
        runtime->scene_color_texture = NULL;
        runtime->scene_color_width = 0u;
        runtime->scene_color_height = 0u;
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return false;
    }
    info.format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
    info.width = (width + 1u) / 2u;
    info.height = (height + 1u) / 2u;
    runtime->ssao_texture = SDL_CreateGPUTexture(device, &info);
    if (runtime->ssao_texture == NULL) {
        SDL_ReleaseGPUTexture(device, runtime->normal_texture);
        SDL_ReleaseGPUTexture(device, runtime->ssao_resolved_texture);
        SDL_ReleaseGPUTexture(
            device,
            runtime->ambient_specular_texture
        );
        SDL_ReleaseGPUTexture(device, runtime->ambient_texture);
        SDL_ReleaseGPUTexture(device, runtime->scene_color_texture);
        runtime->normal_texture = NULL;
        runtime->ssao_resolved_texture = NULL;
        runtime->ambient_specular_texture = NULL;
        runtime->ambient_texture = NULL;
        runtime->scene_color_texture = NULL;
        runtime->scene_color_width = 0u;
        runtime->scene_color_height = 0u;
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return false;
    }
    runtime->ssao_denoised_texture =
        SDL_CreateGPUTexture(device, &info);
    if (runtime->ssao_denoised_texture == NULL) {
        SDL_ReleaseGPUTexture(device, runtime->ssao_texture);
        SDL_ReleaseGPUTexture(device, runtime->normal_texture);
        SDL_ReleaseGPUTexture(device, runtime->ssao_resolved_texture);
        SDL_ReleaseGPUTexture(
            device,
            runtime->ambient_specular_texture
        );
        SDL_ReleaseGPUTexture(device, runtime->ambient_texture);
        SDL_ReleaseGPUTexture(device, runtime->scene_color_texture);
        runtime->ssao_texture = NULL;
        runtime->normal_texture = NULL;
        runtime->ssao_resolved_texture = NULL;
        runtime->ambient_specular_texture = NULL;
        runtime->ambient_texture = NULL;
        runtime->scene_color_texture = NULL;
        runtime->scene_color_width = 0u;
        runtime->scene_color_height = 0u;
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return false;
    }
    info.format = runtime->scene_color_format;
    info.width = width;
    info.height = height;
    runtime->emission_texture = SDL_CreateGPUTexture(device, &info);
    if (runtime->emission_texture == NULL) {
        SDL_ReleaseGPUTexture(device, runtime->ssao_denoised_texture);
        SDL_ReleaseGPUTexture(device, runtime->ssao_texture);
        SDL_ReleaseGPUTexture(device, runtime->normal_texture);
        SDL_ReleaseGPUTexture(device, runtime->ssao_resolved_texture);
        SDL_ReleaseGPUTexture(device, runtime->ambient_specular_texture);
        SDL_ReleaseGPUTexture(device, runtime->ambient_texture);
        SDL_ReleaseGPUTexture(device, runtime->scene_color_texture);
        runtime->ssao_denoised_texture = NULL;
        runtime->ssao_texture = NULL;
        runtime->normal_texture = NULL;
        runtime->ssao_resolved_texture = NULL;
        runtime->ambient_specular_texture = NULL;
        runtime->ambient_texture = NULL;
        runtime->scene_color_texture = NULL;
        runtime->scene_color_width = 0u;
        runtime->scene_color_height = 0u;
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return false;
    }
    runtime->scene_color_width = width;
    runtime->scene_color_height = height;
    return true;
}

/* The opaque pass's motion vectors (RG16F, UV units) and the transparent pass's reactive mask (R8),
   at the render size, while the temporal upscaler runs. */
static bool btbh_gpu_scene_ensure_temporal_targets(
    BTBHGPUWindow *window,
    uint32_t width,
    uint32_t height,
    BTBHGPUSceneReport *report
) {
    BTBHGPUSceneRuntime *runtime = window->scene_runtime;
    SDL_GPUDevice *device = window->device->gpu;
    if (runtime->velocity_texture != NULL &&
        runtime->reactive_texture != NULL &&
        runtime->temporal_target_width == width &&
        runtime->temporal_target_height == height) {
        return true;
    }
    if (runtime->velocity_texture != NULL) {
        SDL_ReleaseGPUTexture(device, runtime->velocity_texture);
        runtime->velocity_texture = NULL;
    }
    if (runtime->reactive_texture != NULL) {
        SDL_ReleaseGPUTexture(device, runtime->reactive_texture);
        runtime->reactive_texture = NULL;
    }
    SDL_GPUTextureCreateInfo info;
    SDL_zero(info);
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT;
    info.usage =
        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
        SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1u;
    info.num_levels = 1u;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    runtime->velocity_texture = SDL_CreateGPUTexture(device, &info);
    info.format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
    runtime->reactive_texture = SDL_CreateGPUTexture(device, &info);
    if (runtime->velocity_texture == NULL ||
        runtime->reactive_texture == NULL) {
        if (runtime->velocity_texture != NULL) {
            SDL_ReleaseGPUTexture(device, runtime->velocity_texture);
            runtime->velocity_texture = NULL;
        }
        if (runtime->reactive_texture != NULL) {
            SDL_ReleaseGPUTexture(device, runtime->reactive_texture);
            runtime->reactive_texture = NULL;
        }
        runtime->temporal_target_width = 0u;
        runtime->temporal_target_height = 0u;
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return false;
    }
    runtime->temporal_target_width = width;
    runtime->temporal_target_height = height;
    return true;
}

static SDL_GPUTexture *btbh_gpu_scene_create_smaa_target(
    SDL_GPUDevice *device,
    uint32_t width,
    uint32_t height,
    SDL_GPUTextureFormat format
) {
    SDL_GPUTextureCreateInfo info;
    SDL_zero(info);
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = format;
    info.usage =
        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
        SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1u;
    info.num_levels = 1u;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    return SDL_CreateGPUTexture(device, &info);
}

static bool btbh_gpu_scene_ensure_upscaled_color_target(
    BTBHGPUWindow *window,
    uint32_t width,
    uint32_t height,
    bool sharpened,
    BTBHGPUSceneReport *report
) {
    BTBHGPUSceneRuntime *runtime = window->scene_runtime;
    SDL_GPUDevice *device = window->device->gpu;
    if (runtime->upscaled_color_texture != NULL &&
        runtime->upscale_width == width &&
        runtime->upscale_height == height &&
        (!sharpened || runtime->sharpened_color_texture != NULL)) {
        return true;
    }
    if (runtime->upscaled_color_texture != NULL) {
        SDL_ReleaseGPUTexture(
            device,
            runtime->upscaled_color_texture
        );
        runtime->upscaled_color_texture = NULL;
    }
    if (runtime->sharpened_color_texture != NULL) {
        SDL_ReleaseGPUTexture(
            device,
            runtime->sharpened_color_texture
        );
        runtime->sharpened_color_texture = NULL;
    }
    if (sharpened) {
        runtime->sharpened_color_texture =
            btbh_gpu_scene_create_smaa_target(
                device,
                width,
                height,
                SDL_GetGPUSwapchainTextureFormat(
                    device,
                    window->window
                )
            );
        if (runtime->sharpened_color_texture == NULL) {
            btbh_gpu_scene_error(
                report,
                BTBH_GPU_DEVICE_FAILURE,
                SDL_GetError()
            );
            return false;
        }
    }
    runtime->upscale_width = 0u;
    runtime->upscale_height = 0u;
    runtime->upscaled_color_texture =
        btbh_gpu_scene_create_smaa_target(
            device,
            width,
            height,
            SDL_GetGPUSwapchainTextureFormat(
                device,
                window->window
            )
        );
    if (runtime->upscaled_color_texture == NULL) {
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return false;
    }
    runtime->upscale_width = width;
    runtime->upscale_height = height;
    return true;
}

static bool btbh_gpu_scene_ensure_anti_aliasing_targets(
    BTBHGPUWindow *window,
    uint32_t width,
    uint32_t height,
    bool post_color_enabled,
    bool smaa_enabled,
    BTBHGPUSceneReport *report
) {
    BTBHGPUSceneRuntime *runtime = window->scene_runtime;
    SDL_GPUDevice *device = window->device->gpu;
    if (runtime->final_color_texture != NULL &&
        runtime->post_ping_texture != NULL &&
        runtime->post_pong_texture != NULL &&
        runtime->smaa_width == width &&
        runtime->smaa_height == height &&
        (!post_color_enabled ||
         runtime->post_color_texture != NULL) &&
        (!smaa_enabled ||
         (runtime->smaa_edges_texture != NULL &&
          runtime->smaa_weights_texture != NULL))) {
        return true;
    }
    if (runtime->smaa_width != width ||
        runtime->smaa_height != height) {
        if (runtime->final_color_texture != NULL) {
            SDL_ReleaseGPUTexture(device, runtime->final_color_texture);
            runtime->final_color_texture = NULL;
        }
        if (runtime->post_color_texture != NULL) {
            SDL_ReleaseGPUTexture(device, runtime->post_color_texture);
            runtime->post_color_texture = NULL;
        }
        if (runtime->post_ping_texture != NULL) {
            SDL_ReleaseGPUTexture(device, runtime->post_ping_texture);
            runtime->post_ping_texture = NULL;
        }
        if (runtime->post_pong_texture != NULL) {
            SDL_ReleaseGPUTexture(device, runtime->post_pong_texture);
            runtime->post_pong_texture = NULL;
        }
        if (runtime->smaa_edges_texture != NULL) {
            SDL_ReleaseGPUTexture(device, runtime->smaa_edges_texture);
            runtime->smaa_edges_texture = NULL;
        }
        if (runtime->smaa_weights_texture != NULL) {
            SDL_ReleaseGPUTexture(device, runtime->smaa_weights_texture);
            runtime->smaa_weights_texture = NULL;
        }
    }
    const SDL_GPUTextureFormat swapchain_format =
        SDL_GetGPUSwapchainTextureFormat(device, window->window);
    if (runtime->final_color_texture == NULL) {
        runtime->final_color_texture =
            btbh_gpu_scene_create_smaa_target(
                device,
                width,
                height,
                swapchain_format
            );
    }
    if (runtime->post_ping_texture == NULL) {
        runtime->post_ping_texture = btbh_gpu_scene_create_smaa_target(
            device,
            width,
            height,
            SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT
        );
    }
    if (runtime->post_pong_texture == NULL) {
        runtime->post_pong_texture = btbh_gpu_scene_create_smaa_target(
            device,
            width,
            height,
            SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT
        );
    }
    if (post_color_enabled &&
        runtime->post_color_texture == NULL) {
        runtime->post_color_texture = btbh_gpu_scene_create_smaa_target(
            device,
            width,
            height,
            swapchain_format
        );
    }
    if (smaa_enabled && runtime->smaa_edges_texture == NULL) {
        runtime->smaa_edges_texture = btbh_gpu_scene_create_smaa_target(
            device,
            width,
            height,
            SDL_GPU_TEXTUREFORMAT_R8G8_UNORM
        );
    }
    if (smaa_enabled && runtime->smaa_weights_texture == NULL) {
        runtime->smaa_weights_texture = btbh_gpu_scene_create_smaa_target(
            device,
            width,
            height,
            SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM
        );
    }
    if (runtime->final_color_texture == NULL ||
        runtime->post_ping_texture == NULL ||
        runtime->post_pong_texture == NULL ||
        (post_color_enabled &&
         runtime->post_color_texture == NULL) ||
        (smaa_enabled &&
         (runtime->smaa_edges_texture == NULL ||
          runtime->smaa_weights_texture == NULL))) {
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return false;
    }
    runtime->smaa_width = width;
    runtime->smaa_height = height;
    return true;
}

static void btbh_gpu_scene_release_bloom_targets(
    SDL_GPUDevice *device,
    BTBHGPUSceneRuntime *runtime
) {
    for (uint32_t level = 0u; level < BTBH_GPU_SCENE_BLOOM_LEVELS; ++level) {
        if (runtime->bloom_down_textures[level] != NULL) {
            SDL_ReleaseGPUTexture(device, runtime->bloom_down_textures[level]);
            runtime->bloom_down_textures[level] = NULL;
        }
        if (runtime->bloom_up_textures[level] != NULL) {
            SDL_ReleaseGPUTexture(device, runtime->bloom_up_textures[level]);
            runtime->bloom_up_textures[level] = NULL;
        }
        runtime->bloom_level_width[level] = 0u;
        runtime->bloom_level_height[level] = 0u;
    }
    runtime->bloom_width = 0u;
    runtime->bloom_height = 0u;
}

/* Allocated only on a render size change, never per frame. */
static bool btbh_gpu_scene_ensure_bloom_targets(
    BTBHGPUWindow *window,
    uint32_t width,
    uint32_t height,
    BTBHGPUSceneReport *report
) {
    BTBHGPUSceneRuntime *runtime = window->scene_runtime;
    SDL_GPUDevice *device = window->device->gpu;
    if (runtime->bloom_down_textures[0] != NULL &&
        runtime->bloom_width == width &&
        runtime->bloom_height == height) {
        return true;
    }
    btbh_gpu_scene_release_bloom_targets(device, runtime);
    uint32_t level_width = width > 1u ? width / 2u : 1u;
    uint32_t level_height = height > 1u ? height / 2u : 1u;
    for (uint32_t level = 0u; level < BTBH_GPU_SCENE_BLOOM_LEVELS; ++level) {
        runtime->bloom_level_width[level] = level_width;
        runtime->bloom_level_height[level] = level_height;
        runtime->bloom_down_textures[level] = btbh_gpu_scene_create_smaa_target(
            device,
            level_width,
            level_height,
            SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT
        );
        if (level + 1u < BTBH_GPU_SCENE_BLOOM_LEVELS) {
            runtime->bloom_up_textures[level] = btbh_gpu_scene_create_smaa_target(
                device,
                level_width,
                level_height,
                SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT
            );
        }
        if (runtime->bloom_down_textures[level] == NULL ||
            (level + 1u < BTBH_GPU_SCENE_BLOOM_LEVELS &&
             runtime->bloom_up_textures[level] == NULL)) {
            btbh_gpu_scene_release_bloom_targets(device, runtime);
            btbh_gpu_scene_error(
                report,
                BTBH_GPU_DEVICE_FAILURE,
                SDL_GetError()
            );
            return false;
        }
        level_width = level_width > 1u ? level_width / 2u : 1u;
        level_height = level_height > 1u ? level_height / 2u : 1u;
    }
    runtime->bloom_width = width;
    runtime->bloom_height = height;
    return true;
}

static uint32_t btbh_gpu_scene_buffer_capacity(
    uint32_t required
) {
    uint32_t capacity = 256u;
    while (capacity < required && capacity <= UINT32_MAX / 2u) {
        capacity *= 2u;
    }
    return capacity >= required ? capacity : required;
}

static bool btbh_gpu_scene_ensure_upload_buffers(
    BTBHGPUWindow *window,
    uint32_t instance_bytes,
    uint32_t bone_bytes,
    uint32_t transfer_bytes
) {
    BTBHGPUSceneRuntime *runtime = window->scene_runtime;
    SDL_GPUDevice *device = window->device->gpu;
    SDL_GPUBufferCreateInfo buffer_info;
    SDL_GPUTransferBufferCreateInfo transfer_info;
    SDL_zero(buffer_info);
    SDL_zero(transfer_info);
    buffer_info.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    if (
        instance_bytes > 0u &&
        (runtime->instance_buffer == NULL ||
         runtime->instance_vertex_buffer == NULL ||
         runtime->instance_capacity < instance_bytes)
    ) {
        if (runtime->instance_buffer != NULL) {
            SDL_ReleaseGPUBuffer(device, runtime->instance_buffer);
        }
        if (runtime->instance_vertex_buffer != NULL) {
            SDL_ReleaseGPUBuffer(
                device,
                runtime->instance_vertex_buffer
            );
        }
        runtime->instance_buffer = NULL;
        runtime->instance_vertex_buffer = NULL;
        runtime->instance_capacity =
            btbh_gpu_scene_buffer_capacity(instance_bytes);
        buffer_info.size = runtime->instance_capacity;
        runtime->instance_buffer =
            SDL_CreateGPUBuffer(device, &buffer_info);
        if (runtime->instance_buffer == NULL) {
            runtime->instance_capacity = 0u;
            return false;
        }
        buffer_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        runtime->instance_vertex_buffer =
            SDL_CreateGPUBuffer(device, &buffer_info);
        if (runtime->instance_vertex_buffer == NULL) {
            SDL_ReleaseGPUBuffer(device, runtime->instance_buffer);
            runtime->instance_buffer = NULL;
            runtime->instance_capacity = 0u;
            return false;
        }
        buffer_info.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    }
    if (
        bone_bytes > 0u &&
        (
            runtime->bone_buffer == NULL ||
            runtime->bone_capacity < bone_bytes
        )
    ) {
        if (runtime->bone_buffer != NULL) {
            SDL_ReleaseGPUBuffer(device, runtime->bone_buffer);
        }
        runtime->bone_capacity =
            btbh_gpu_scene_buffer_capacity(bone_bytes);
        buffer_info.size = runtime->bone_capacity;
        runtime->bone_buffer = SDL_CreateGPUBuffer(
            device,
            &buffer_info
        );
        if (runtime->bone_buffer == NULL) {
            runtime->bone_capacity = 0u;
            return false;
        }
    }
    if (
        transfer_bytes > 0u &&
        (runtime->upload_transfer == NULL ||
         runtime->upload_capacity < transfer_bytes)
    ) {
        if (runtime->upload_transfer != NULL) {
            SDL_ReleaseGPUTransferBuffer(
                device,
                runtime->upload_transfer
            );
        }
        runtime->upload_capacity =
            btbh_gpu_scene_buffer_capacity(transfer_bytes);
        transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transfer_info.size = runtime->upload_capacity;
        runtime->upload_transfer =
            SDL_CreateGPUTransferBuffer(device, &transfer_info);
        if (runtime->upload_transfer == NULL) {
            runtime->upload_capacity = 0u;
            return false;
        }
    }
    return true;
}

static bool btbh_gpu_scene_ensure_motion_buffer(
    BTBHGPUWindow *window,
    uint32_t motion_bytes
) {
    BTBHGPUSceneRuntime *runtime = window->scene_runtime;
    if (motion_bytes == 0u ||
        (runtime->motion_buffer != NULL &&
         runtime->motion_capacity >= motion_bytes)) {
        return true;
    }
    SDL_GPUDevice *device = window->device->gpu;
    if (runtime->motion_buffer != NULL) {
        SDL_ReleaseGPUBuffer(device, runtime->motion_buffer);
        runtime->motion_buffer = NULL;
    }
    SDL_GPUBufferCreateInfo info;
    SDL_zero(info);
    info.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    runtime->motion_capacity = btbh_gpu_scene_buffer_capacity(motion_bytes);
    info.size = runtime->motion_capacity;
    runtime->motion_buffer = SDL_CreateGPUBuffer(device, &info);
    if (runtime->motion_buffer == NULL) {
        runtime->motion_capacity = 0u;
        return false;
    }
    return true;
}

static bool btbh_gpu_scene_validate_batches(
    BTBHGPUWindow *window,
    uint32_t transient_instance_count,
    const BTBHGPUSceneGeometryBatch *batches,
    const uint8_t *scene_visibility,
    const uint8_t *directional_shadow_visibility,
    uint32_t batch_count
) {
    for (uint32_t index = 0u; index < batch_count; ++index) {
        const BTBHGPUSceneGeometryBatch *batch = &batches[index];
        const bool valid_geometry =
            (batch->kind == BTBH_GPU_SCENE_GEOMETRY_MODEL &&
                batch->model != NULL &&
                batch->model->owner == window &&
                batch->model->primitives != NULL) ||
            (batch->kind == BTBH_GPU_SCENE_GEOMETRY_CUSTOM_MESH &&
                batch->custom_mesh != NULL &&
                batch->custom_mesh->owner == window &&
                batch->custom_mesh->vertex_buffer != NULL);
        const bool has_animation =
            batch->animation_pose != NULL ||
            batch->animation_library != NULL;
        const bool valid_animation =
            !has_animation ||
            (
                batch->kind == BTBH_GPU_SCENE_GEOMETRY_MODEL &&
                (
                    batch->animation_pose != NULL ||
                    isfinite(batch->animation_time)
                )
            );
        const bool valid_material =
            batch->blend_mode >= BTBH_GPU_SCENE_BLEND_ALPHA &&
            batch->blend_mode < BTBH_GPU_SCENE_BLEND_COUNT &&
            batch->billboard_mode >=
                BTBH_GPU_SCENE_BILLBOARD_DISABLED &&
            batch->billboard_mode <= BTBH_GPU_SCENE_BILLBOARD_Y_AXIS &&
            batch->depth_compare >= BTBH_GPU_SCENE_DEPTH_ALWAYS &&
            batch->depth_compare < BTBH_GPU_SCENE_DEPTH_COUNT &&
            batch->stencil_compare >= BTBH_GPU_SCENE_DEPTH_ALWAYS &&
            batch->stencil_compare < BTBH_GPU_SCENE_DEPTH_COUNT &&
            batch->cull_mode >= BTBH_GPU_SCENE_CULL_NONE &&
            batch->cull_mode < BTBH_GPU_SCENE_CULL_COUNT &&
            batch->stencil_fail >= BTBH_GPU_SCENE_STENCIL_KEEP &&
            batch->stencil_fail <
                BTBH_GPU_SCENE_STENCIL_OPERATION_COUNT &&
            batch->stencil_depth_fail >=
                BTBH_GPU_SCENE_STENCIL_KEEP &&
            batch->stencil_depth_fail <
                BTBH_GPU_SCENE_STENCIL_OPERATION_COUNT &&
            batch->stencil_pass >= BTBH_GPU_SCENE_STENCIL_KEEP &&
            batch->stencil_pass <
                BTBH_GPU_SCENE_STENCIL_OPERATION_COUNT &&
            batch->transparency_enabled <= 1u &&
            batch->unlit_enabled <= 3u &&
            (scene_visibility == NULL ||
             scene_visibility[index] <= 1u) &&
            (directional_shadow_visibility == NULL ||
             directional_shadow_visibility[index] <= 1u) &&
            batch->spot_shadow_visible <= 1u &&
            batch->surface_variant >=
                BTBH_GPU_SCENE_SURFACE_NONE &&
            batch->surface_variant <
                BTBH_GPU_SCENE_SURFACE_COUNT &&
            btbh_gpu_scene_refraction_valid(batch) &&
            isfinite(batch->alpha_cutoff) &&
            batch->alpha_cutoff >= 0.0f &&
            batch->alpha_cutoff <= 1.0f &&
            isfinite(batch->emission_energy) &&
            isfinite(batch->normal_scale) &&
            isfinite(batch->occlusion) &&
            isfinite(batch->roughness) &&
            isfinite(batch->metalness) &&
            isfinite(batch->uv_scale_x) &&
            isfinite(batch->uv_scale_y) &&
            isfinite(batch->uv_offset_x) &&
            isfinite(batch->uv_offset_y) &&
            isfinite(batch->surface_time) &&
            isfinite(batch->surface_parameters0[0]) &&
            isfinite(batch->surface_parameters0[1]) &&
            isfinite(batch->surface_parameters0[2]) &&
            isfinite(batch->surface_parameters0[3]) &&
            isfinite(batch->surface_parameters1[0]) &&
            isfinite(batch->surface_parameters1[1]) &&
            isfinite(batch->surface_parameters1[2]) &&
            isfinite(batch->surface_parameters1[3]) &&
            isfinite(batch->detail_parameters[0]) &&
            isfinite(batch->detail_parameters[1]) &&
            isfinite(batch->detail_parameters[2]) &&
            isfinite(batch->detail_parameters[3]) &&
            /* SurfaceDetail.Pattern: none, ground, grass, facade, paving. */
            batch->detail_parameters[3] >= 0.0f &&
            batch->detail_parameters[3] <= 6.0f &&
            (batch->detail_parameters[3] < 0.5f ||
             batch->detail_parameters[2] > 0.0f) &&
            (batch->albedo_texture == NULL ||
             (batch->albedo_texture->owner == window &&
              batch->albedo_texture->texture != NULL &&
              batch->albedo_texture->sampler != NULL)) &&
            (batch->emission_texture == NULL ||
             (batch->emission_texture->owner == window &&
              batch->emission_texture->texture != NULL &&
              batch->emission_texture->sampler != NULL)) &&
            (batch->normal_texture == NULL ||
             (batch->normal_texture->owner == window &&
              batch->normal_texture->texture != NULL &&
              batch->normal_texture->sampler != NULL)) &&
            (batch->orm_texture == NULL ||
             (batch->orm_texture->owner == window &&
              batch->orm_texture->texture != NULL &&
             batch->orm_texture->sampler != NULL));
        BTBHGPUSceneStencilVariant stencil_variant;
        const bool valid_stencil =
            btbh_gpu_scene_stencil_variant(
                batch,
                &stencil_variant
            );
        const bool valid_material_slot =
            (batch->kind == BTBH_GPU_SCENE_GEOMETRY_MODEL &&
                batch->model != NULL &&
                (batch->material_slot < 0 ||
                 (uint32_t)batch->material_slot <
                    batch->model->material_count)) ||
            (batch->kind == BTBH_GPU_SCENE_GEOMETRY_CUSTOM_MESH &&
                batch->material_slot < 0);
        const bool uses_persistent_instances =
            batch->instance_buffer != NULL;
        const bool valid_instance_buffer =
            !uses_persistent_instances ||
            (batch->instance_buffer->owner == window &&
             batch->instance_buffer->buffer != NULL &&
             batch->instance_buffer->vertex_buffer != NULL &&
             batch->instance_buffer->stride ==
                BTBH_GPU_SCENE_INSTANCE_FLOATS * sizeof(float));
        const uint32_t available_instance_count =
            uses_persistent_instances && valid_instance_buffer
                ? batch->instance_buffer->uploaded_prefix_byte_count /
                    batch->instance_buffer->stride
                : transient_instance_count;
        if (!valid_geometry ||
            !valid_animation ||
            !valid_material ||
            !valid_stencil ||
            !valid_material_slot ||
            !valid_instance_buffer ||
            batch->instance_count == 0u ||
            batch->first_instance > available_instance_count ||
            batch->instance_count >
                available_instance_count - batch->first_instance) {
            return false;
        }
    }
    return true;
}

static bool btbh_gpu_scene_batch_uses_geometry(
    const BTBHGPUSceneGeometryBatch *batch,
    uint32_t geometry_index
) {
    return
        batch->kind != BTBH_GPU_SCENE_GEOMETRY_MODEL ||
        batch->material_slot < 0 ||
        batch->model->primitives[geometry_index].material_slot ==
            batch->material_slot;
}

typedef struct BTBHGPUSceneAnimationSample {
    const BTBHGPUAnimationLibrary *library;
    const BTBHGPUAnimationPose *pose;
    uint32_t animation_index;
    uint32_t animation_time_bits;
    uint32_t skin_index;
    uint32_t joint_count;
    uint32_t bone_offset;
} BTBHGPUSceneAnimationSample;

static uint32_t btbh_gpu_scene_float_bits(float value) {
    uint32_t bits = 0u;
    (void)memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static bool btbh_gpu_scene_animation_sample_matches(
    const BTBHGPUSceneAnimationSample *sample,
    const BTBHGPUSceneGeometryBatch *batch,
    uint32_t skin_index,
    uint32_t joint_count
) {
    if (
        sample->skin_index != skin_index ||
        sample->joint_count != joint_count
    ) {
        return false;
    }
    if (batch->animation_pose != NULL) {
        return sample->pose == batch->animation_pose;
    }
    return
        sample->pose == NULL &&
        sample->library == batch->animation_library &&
        sample->animation_index == batch->animation_index &&
        sample->animation_time_bits ==
            btbh_gpu_scene_float_bits(batch->animation_time);
}

/* Temporal frames: points each skinned geometry's previous offset at its pose's palette from the previous
   temporal frame, appended after this frame's palettes (matrices has room for twice the current bones),
   then keeps this frame's pose palettes for the next. A pose without one keeps its current palette, so
   its skeleton draws no motion of its own. */
static void btbh_gpu_scene_apply_temporal_palettes(
    BTBHGPUSceneRuntime *runtime,
    const BTBHGPUSceneAnimationSample *samples,
    uint32_t sample_count,
    const uint32_t *offsets,
    uint32_t *previous_offsets,
    uint32_t geometry_count,
    float *matrices,
    uint32_t *bone_index
) {
    for (uint32_t sample_index = 0u; sample_index < sample_count; ++sample_index) {
        const BTBHGPUSceneAnimationSample *sample = &samples[sample_index];
        if (sample->pose == NULL) {
            continue;
        }
        for (uint32_t entry_index = 0u;
             entry_index < runtime->temporal_palette_count;
             ++entry_index) {
            const BTBHGPUSceneTemporalPalette *entry =
                &runtime->temporal_palettes[entry_index];
            if (entry->pose != sample->pose ||
                entry->skin_index != sample->skin_index ||
                entry->joint_count != sample->joint_count) {
                continue;
            }
            if (sample->joint_count > UINT32_MAX - *bone_index) {
                break;
            }
            (void)memcpy(
                &matrices[(size_t)*bone_index * 16u],
                &runtime->temporal_palette_matrices[(size_t)entry->first_matrix * 16u],
                (size_t)sample->joint_count * 16u * sizeof(float)
            );
            for (uint32_t geometry = 0u; geometry < geometry_count; ++geometry) {
                if (offsets[geometry] == sample->bone_offset) {
                    previous_offsets[geometry] = *bone_index;
                }
            }
            *bone_index += sample->joint_count;
            break;
        }
    }

    uint32_t palette_count = 0u;
    size_t matrix_count = 0u;
    for (uint32_t sample_index = 0u; sample_index < sample_count; ++sample_index) {
        if (samples[sample_index].pose != NULL) {
            palette_count += 1u;
            matrix_count += samples[sample_index].joint_count;
        }
    }
    runtime->temporal_palette_count = 0u;
    if (palette_count == 0u || matrix_count > UINT32_MAX) {
        return;
    }
    if (runtime->temporal_palette_capacity < palette_count) {
        BTBHGPUSceneTemporalPalette *grown = realloc(
            runtime->temporal_palettes,
            (size_t)palette_count * sizeof(*grown)
        );
        if (grown == NULL) {
            return;
        }
        runtime->temporal_palettes = grown;
        runtime->temporal_palette_capacity = palette_count;
    }
    if (runtime->temporal_palette_matrix_capacity < matrix_count) {
        float *grown = realloc(
            runtime->temporal_palette_matrices,
            matrix_count * 16u * sizeof(float)
        );
        if (grown == NULL) {
            return;
        }
        runtime->temporal_palette_matrices = grown;
        runtime->temporal_palette_matrix_capacity = (uint32_t)matrix_count;
    }
    uint32_t first_matrix = 0u;
    for (uint32_t sample_index = 0u; sample_index < sample_count; ++sample_index) {
        const BTBHGPUSceneAnimationSample *sample = &samples[sample_index];
        if (sample->pose == NULL) {
            continue;
        }
        (void)memcpy(
            &runtime->temporal_palette_matrices[(size_t)first_matrix * 16u],
            &matrices[(size_t)sample->bone_offset * 16u],
            (size_t)sample->joint_count * 16u * sizeof(float)
        );
        BTBHGPUSceneTemporalPalette *entry =
            &runtime->temporal_palettes[runtime->temporal_palette_count];
        entry->pose = sample->pose;
        entry->skin_index = sample->skin_index;
        entry->joint_count = sample->joint_count;
        entry->first_matrix = first_matrix;
        runtime->temporal_palette_count += 1u;
        first_matrix += sample->joint_count;
    }
}

/* bone_offsets holds each geometry's palette offset (UINT32_MAX when it is not skinned), then, at
   geometry_count + index, the palette the previous temporal frame drew it with (the same offset outside
   temporal frames). temporal_runtime is the runtime while the frame renders temporally, NULL otherwise. */
static bool btbh_gpu_scene_prepare_bones(
    const BTBHGPUSceneGeometryBatch *batches,
    uint32_t batch_count,
    uint32_t **bone_offsets,
    uint32_t *geometry_count,
    float **bone_matrices,
    uint32_t *bone_count,
    bool detailed_telemetry,
    BTBHGPUSceneTelemetryReport *telemetry,
    BTBHGPUSceneRuntime *temporal_runtime
) {
    const uint64_t count_started_ticks = detailed_telemetry
        ? SDL_GetTicksNS()
        : 0u;
    uint32_t total_geometry_count = 0u;
    uint32_t total_animated_geometry_count = 0u;
    uint32_t total_bone_count = 0u;
    for (uint32_t batch_index = 0u;
         batch_index < batch_count;
         batch_index += 1u) {
        const BTBHGPUSceneGeometryBatch *batch = &batches[batch_index];
        const uint32_t batch_geometry_count =
            batch->kind == BTBH_GPU_SCENE_GEOMETRY_MODEL
                ? batch->model->primitive_count
                : 1u;
        for (uint32_t primitive_index = 0u;
             primitive_index < batch_geometry_count;
             primitive_index += 1u) {
            if (!btbh_gpu_scene_batch_uses_geometry(
                    batch,
                    primitive_index
                )) {
                continue;
            }
            if (total_geometry_count == UINT32_MAX) {
                return false;
            }
            total_geometry_count += 1u;
            if (
                batch->animation_pose == NULL &&
                batch->animation_library == NULL
            ) {
                continue;
            }
            const int32_t skin_index =
                batch->model->primitives[primitive_index].skin_index;
            if (skin_index < 0) {
                continue;
            }
            const uint32_t joint_count =
                batch->animation_pose != NULL
                    ? btbh_gpu_animation_pose_skin_joint_count(
                        batch->animation_pose,
                        (uint32_t)skin_index
                    )
                    : btbh_gpu_animation_library_skin_joint_count(
                        batch->animation_library,
                        (uint32_t)skin_index
                    );
            if (
                joint_count == 0u ||
                joint_count > UINT32_MAX - total_bone_count
            ) {
                return false;
            }
            total_animated_geometry_count += 1u;
            total_bone_count += joint_count;
        }
    }
    const uint64_t count_finished_ticks = detailed_telemetry
        ? SDL_GetTicksNS()
        : 0u;
    if (detailed_telemetry) {
        telemetry->cpu_prepare_bone_count_milliseconds =
            (double)(count_finished_ticks - count_started_ticks) /
            1000000.0;
    }
    /* Temporal frames also hold the previous palettes: at most one per current one. */
    const size_t matrix_capacity =
        (size_t)total_bone_count * (temporal_runtime != NULL ? 2u : 1u);
    if (
        total_geometry_count == 0u ||
        (size_t)total_geometry_count >
            SIZE_MAX / (sizeof(uint32_t) * 2u) ||
        (size_t)total_animated_geometry_count >
            SIZE_MAX / sizeof(BTBHGPUSceneAnimationSample) ||
        matrix_capacity >
            SIZE_MAX / (sizeof(float) * 16u)
    ) {
        return false;
    }
    uint32_t *offsets = malloc(
        (size_t)total_geometry_count * 2u * sizeof(*offsets)
    );
    float *matrices = total_bone_count > 0u
        ? malloc(
            matrix_capacity *
            sizeof(float) *
            16u
        )
        : NULL;
    BTBHGPUSceneAnimationSample *samples =
        total_animated_geometry_count > 0u
            ? malloc(
                (size_t)total_animated_geometry_count *
                sizeof(*samples)
            )
            : NULL;
    if (
        offsets == NULL ||
        (total_bone_count > 0u && matrices == NULL) ||
        (
            total_animated_geometry_count > 0u &&
            samples == NULL
        )
    ) {
        free(samples);
        free(matrices);
        free(offsets);
        return false;
    }
    const uint64_t allocation_finished_ticks = detailed_telemetry
        ? SDL_GetTicksNS()
        : 0u;
    if (detailed_telemetry) {
        telemetry->cpu_prepare_bone_allocate_milliseconds =
            (double)(
                allocation_finished_ticks - count_finished_ticks
            ) /
            1000000.0;
    }

    uint32_t geometry_index = 0u;
    uint32_t bone_index = 0u;
    uint32_t sample_count = 0u;
    bool valid = true;
    for (uint32_t batch_index = 0u;
         valid && batch_index < batch_count;
         batch_index += 1u) {
        const BTBHGPUSceneGeometryBatch *batch = &batches[batch_index];
        const uint32_t batch_geometry_count =
            batch->kind == BTBH_GPU_SCENE_GEOMETRY_MODEL
                ? batch->model->primitive_count
                : 1u;
        for (uint32_t primitive_index = 0u;
             valid && primitive_index < batch_geometry_count;
             primitive_index += 1u) {
            if (!btbh_gpu_scene_batch_uses_geometry(
                    batch,
                    primitive_index
                )) {
                continue;
            }
            offsets[geometry_index] = UINT32_MAX;
            if (
                batch->animation_pose != NULL ||
                batch->animation_library != NULL
            ) {
                const int32_t skin_index =
                    batch->model->primitives[primitive_index].skin_index;
                if (skin_index >= 0) {
                    const uint32_t joint_count =
                        batch->animation_pose != NULL
                            ? btbh_gpu_animation_pose_skin_joint_count(
                                batch->animation_pose,
                                (uint32_t)skin_index
                            )
                            : btbh_gpu_animation_library_skin_joint_count(
                                batch->animation_library,
                                (uint32_t)skin_index
                            );
                    uint32_t matching_sample = UINT32_MAX;
                    for (uint32_t sample_index = 0u;
                         sample_index < sample_count;
                         sample_index += 1u) {
                        if (btbh_gpu_scene_animation_sample_matches(
                                &samples[sample_index],
                                batch,
                                (uint32_t)skin_index,
                                joint_count
                            )) {
                            matching_sample = sample_index;
                            break;
                        }
                    }
                    if (matching_sample != UINT32_MAX) {
                        offsets[geometry_index] =
                            samples[matching_sample].bone_offset;
                    } else {
                        offsets[geometry_index] = bone_index;
                        valid = batch->animation_pose != NULL
                            ? btbh_gpu_animation_pose_sample_skin(
                                batch->animation_pose,
                                (uint32_t)skin_index,
                                &matrices[(size_t)bone_index * 16u],
                                joint_count
                            )
                            : btbh_gpu_animation_library_sample_skin(
                                batch->animation_library,
                                batch->animation_index,
                                batch->animation_time,
                                (uint32_t)skin_index,
                                &matrices[(size_t)bone_index * 16u],
                                joint_count
                            );
                        if (valid) {
                            BTBHGPUSceneAnimationSample *sample =
                                &samples[sample_count];
                            sample->library =
                                batch->animation_library;
                            sample->pose = batch->animation_pose;
                            sample->animation_index =
                                batch->animation_index;
                            sample->animation_time_bits =
                                btbh_gpu_scene_float_bits(
                                    batch->animation_time
                                );
                            sample->skin_index =
                                (uint32_t)skin_index;
                            sample->joint_count = joint_count;
                            sample->bone_offset = bone_index;
                            sample_count += 1u;
                            bone_index += joint_count;
                        }
                    }
                }
            }
            geometry_index += 1u;
        }
    }
    if (!valid) {
        free(samples);
        free(matrices);
        free(offsets);
        return false;
    }
    uint32_t *previous_offsets = offsets + total_geometry_count;
    (void)memcpy(
        previous_offsets,
        offsets,
        (size_t)total_geometry_count * sizeof(*offsets)
    );
    if (temporal_runtime != NULL) {
        btbh_gpu_scene_apply_temporal_palettes(
            temporal_runtime,
            samples,
            sample_count,
            offsets,
            previous_offsets,
            total_geometry_count,
            matrices,
            &bone_index
        );
    }
    const uint64_t sample_finished_ticks = detailed_telemetry
        ? SDL_GetTicksNS()
        : 0u;
    if (detailed_telemetry) {
        telemetry->cpu_prepare_bone_sample_milliseconds =
            (double)(
                sample_finished_ticks - allocation_finished_ticks
            ) /
            1000000.0;
    }
    free(samples);
    *bone_offsets = offsets;
    *geometry_count = total_geometry_count;
    *bone_matrices = matrices;
    *bone_count = bone_index;
    telemetry->animated_geometry_count = total_animated_geometry_count;
    telemetry->animation_sample_count = sample_count;
    telemetry->bone_matrix_count = bone_index;
    return true;
}

bool btbh_gpu_scene_lighting_valid(
    const BTBHGPUSceneLighting *lighting
) {
    if (lighting == NULL ||
        !isfinite(lighting->ambient_red) ||
        !isfinite(lighting->ambient_green) ||
        !isfinite(lighting->ambient_blue) ||
        !isfinite(lighting->ambient_energy) ||
        lighting->ambient_energy < 0.0f ||
        !isfinite(lighting->directional_x) ||
        !isfinite(lighting->directional_y) ||
        !isfinite(lighting->directional_z) ||
        !isfinite(lighting->directional_energy) ||
        lighting->directional_energy < 0.0f ||
        !isfinite(lighting->directional_red) ||
        !isfinite(lighting->directional_green) ||
        !isfinite(lighting->directional_blue) ||
        !isfinite(lighting->secondary_directional_x) ||
        !isfinite(lighting->secondary_directional_y) ||
        !isfinite(lighting->secondary_directional_z) ||
        !isfinite(lighting->secondary_directional_energy) ||
        lighting->secondary_directional_energy < 0.0f ||
        !isfinite(lighting->secondary_directional_red) ||
        !isfinite(lighting->secondary_directional_green) ||
        !isfinite(lighting->secondary_directional_blue) ||
        !isfinite(lighting->spot_position_x) ||
        !isfinite(lighting->spot_position_y) ||
        !isfinite(lighting->spot_position_z) ||
        !isfinite(lighting->spot_range) ||
        lighting->spot_range < 0.0f ||
        !isfinite(lighting->spot_direction_x) ||
        !isfinite(lighting->spot_direction_y) ||
        !isfinite(lighting->spot_direction_z) ||
        !isfinite(lighting->spot_energy) ||
        lighting->spot_energy < 0.0f ||
        !isfinite(lighting->spot_red) ||
        !isfinite(lighting->spot_green) ||
        !isfinite(lighting->spot_blue) ||
        !isfinite(lighting->spot_falloff) ||
        lighting->spot_falloff <= 0.0f ||
        !isfinite(lighting->spot_inner_cutoff) ||
        !isfinite(lighting->spot_outer_cutoff) ||
        lighting->spot_inner_cutoff <= lighting->spot_outer_cutoff ||
        !isfinite(lighting->spot_occluder_shadow_strength) ||
        lighting->spot_occluder_shadow_strength < 0.0f ||
        lighting->spot_occluder_shadow_strength > 1.0f ||
        !isfinite(lighting->spot_occluder_shadow_reach) ||
        lighting->spot_occluder_shadow_reach < 0.0f ||
        lighting->ibl_enabled > 1u ||
        lighting->directional_shadow_enabled > 1u ||
        lighting->directional_shadow_filter >=
            BTBH_GPU_SCENE_SHADOW_FILTER_COUNT ||
        !isfinite(lighting->directional_shadow_softness) ||
        lighting->directional_shadow_softness < 0.0f ||
        lighting->secondary_directional_shadow_enabled > 1u ||
        !isfinite(lighting->secondary_directional_shadow_softness) ||
        lighting->secondary_directional_shadow_softness < 0.0f ||
        lighting->spot_shadow_enabled > 1u ||
        !isfinite(lighting->spot_shadow_softness) ||
        lighting->spot_shadow_softness < 0.0f ||
        lighting->spot_shadow_is_omni > 1u ||
        (lighting->spot_shadow_is_omni != 0u &&
         (!isfinite(lighting->spot_shadow_near) ||
          lighting->spot_shadow_near <= 0.0f ||
          lighting->spot_shadow_near >= lighting->spot_range)) ||
        lighting->lamp_count > BTBH_GPU_SCENE_MAX_LAMPS ||
        !isfinite(lighting->lamp_core_radius) ||
        lighting->lamp_core_radius < 0.0f ||
        !isfinite(lighting->window_glow) ||
        lighting->window_glow < 0.0f ||
        !isfinite(lighting->cloud_strength) ||
        lighting->cloud_strength < 0.0f ||
        lighting->cloud_strength > 1.0f ||
        !isfinite(lighting->cloud_coverage) ||
        lighting->cloud_coverage < 0.0f ||
        lighting->cloud_coverage > 1.0f ||
        !isfinite(lighting->cloud_softness) ||
        lighting->cloud_softness < 0.0f ||
        !isfinite(lighting->cloud_frequency) ||
        lighting->cloud_frequency < 0.0f ||
        !isfinite(lighting->cloud_height) ||
        !isfinite(lighting->cloud_offset[0]) ||
        !isfinite(lighting->cloud_offset[1]) ||
        !isfinite(lighting->rain) ||
        lighting->rain < 0.0f ||
        lighting->rain > 1.0f) {
        return false;
    }
    for (uint32_t lamp = 0u; lamp < lighting->lamp_count; lamp += 1u) {
        for (size_t component = 0u; component < 4u; component += 1u) {
            if (!isfinite(lighting->lamp_position_reach[lamp][component]) ||
                !isfinite(lighting->lamp_color_energy[lamp][component])) {
                return false;
            }
        }
        if (lighting->lamp_position_reach[lamp][3] < 0.0f ||
            lighting->lamp_color_energy[lamp][3] < 0.0f) {
            return false;
        }
    }
    for (size_t index = 0u; index < 16u; index += 1u) {
        if (!isfinite(
                lighting->directional_shadow_view_projection[index]
            ) ||
            !isfinite(
                lighting
                    ->secondary_directional_shadow_view_projection[index]
            ) ||
            !isfinite(
                lighting->spot_shadow_view_projection[index]
            )) {
            return false;
        }
    }
    for (size_t face = 0u;
         face < BTBH_GPU_OMNI_SHADOW_FACE_COUNT;
         face += 1u) {
        for (size_t index = 0u; index < 16u; index += 1u) {
            if (!isfinite(
                    lighting->omni_shadow_view_projection[face][index]
                )) {
                return false;
            }
        }
    }
    for (size_t index = 0u; index < 27u; index += 1u) {
        if (!isfinite(lighting->irradiance_sh[index])) {
            return false;
        }
    }
    const float procedural_values[] = {
        lighting->ground_bottom_red,
        lighting->ground_bottom_green,
        lighting->ground_bottom_blue,
        lighting->ground_energy,
        lighting->ground_horizon_red,
        lighting->ground_horizon_green,
        lighting->ground_horizon_blue,
        lighting->ground_horizon_curve,
        lighting->sky_top_red,
        lighting->sky_top_green,
        lighting->sky_top_blue,
        lighting->sky_energy,
        lighting->sky_horizon_red,
        lighting->sky_horizon_green,
        lighting->sky_horizon_blue,
        lighting->sky_horizon_curve,
        lighting->sun_red,
        lighting->sun_green,
        lighting->sun_blue,
        lighting->sun_energy,
        lighting->sun_direction_x,
        lighting->sun_direction_y,
        lighting->sun_direction_z,
        lighting->sun_size_radians,
        lighting->sun_curve
    };
    for (size_t index = 0u;
         index < sizeof(procedural_values) / sizeof(procedural_values[0]);
         index += 1u) {
        if (!isfinite(procedural_values[index])) {
            return false;
        }
    }
    return true;
}

static bool btbh_gpu_scene_post_valid(
    const BTBHGPUScenePost *post
) {
    return post != NULL &&
        isfinite(post->tonemap_white) &&
        post->tonemap_white > 0.0f &&
        isfinite(post->exposure) &&
        post->exposure >= 0.0f &&
        isfinite(post->contrast) &&
        post->contrast >= 0.0f &&
        isfinite(post->saturation) &&
        post->saturation >= 0.0f &&
        isfinite(post->cas_sharpness) &&
        post->cas_sharpness >= 0.0f &&
        post->cas_sharpness <= 1.0f &&
        isfinite(post->focus_point_x) &&
        isfinite(post->focus_point_y) &&
        isfinite(post->focus_direction_x) &&
        isfinite(post->focus_direction_y) &&
        isfinite(post->focus_half_width) &&
        post->focus_half_width >= 0.0f &&
        post->focus_half_width < 1.0f &&
        isfinite(post->maximum_radius_pixels) &&
        post->maximum_radius_pixels >= 0.0f &&
        isfinite(post->blur_curve) &&
        post->blur_curve > 0.0f &&
        isfinite(post->highlight_threshold) &&
        isfinite(post->highlight_gain) &&
        post->lut_enabled <= 1u &&
        post->cas_enabled <= 1u &&
        post->tilt_shift_enabled <= 1u &&
        post->ssao_enabled <= 1u &&
        isfinite(post->ssr_intensity) && post->ssr_intensity >= 0.0f &&
        isfinite(post->ssr_max_roughness) && post->ssr_max_roughness >= 0.0f &&
        isfinite(post->ssr_max_distance) && post->ssr_max_distance >= 0.0f &&
        isfinite(post->ssr_thickness) && post->ssr_thickness >= 0.0f &&
        isfinite(post->ao_radius) && post->ao_radius >= 0.0f &&
        isfinite(post->ao_intensity) && post->ao_intensity >= 0.0f &&
        isfinite(post->ao_contact_strength) && post->ao_contact_strength >= 0.0f &&
        post->ao_contact_strength <= 1.0f &&
        isfinite(post->ao_direct_share) && post->ao_direct_share >= 0.0f &&
        post->ao_direct_share <= 1.0f &&
        post->tonemap_mode < BTBH_GPU_SCENE_TONEMAP_COUNT &&
        post->anti_aliasing <= BTBH_GPU_SCENE_ANTI_ALIASING_MAX &&
        post->upscale_mode <= BTBH_GPU_SCENE_UPSCALE_MODE_MAX &&
        post->mirror_horizontal <= 1u &&
        (post->upscale_mode != BTBH_GPU_SCENE_UPSCALE_FSR3 ||
         (isfinite(post->temporal_camera_near) &&
          isfinite(post->temporal_camera_far) &&
          post->temporal_camera_near > 0.0f &&
          post->temporal_camera_far > post->temporal_camera_near &&
          isfinite(post->temporal_vertical_fov) &&
          post->temporal_vertical_fov > 0.0f &&
          post->temporal_vertical_fov < 3.14159265f &&
          isfinite(post->temporal_frame_seconds) &&
          post->temporal_frame_seconds >= 0.0f &&
          isfinite(post->temporal_meters_per_unit) &&
          post->temporal_meters_per_unit >= 0.0f &&
          isfinite(post->temporal_reactive_scale) &&
          post->temporal_reactive_scale >= 0.0f &&
          post->temporal_reset <= 1u)) &&
        (post->lut_enabled == 0u ||
         post->lut_texture != NULL) &&
        post->fog_enabled <= 1u &&
        post->bloom_enabled <= 1u &&
        post->vignette_enabled <= 1u &&
        post->fog_edge_enabled <= 1u &&
        (post->fog_edge_enabled == 0u ||
         (isfinite(post->fog_edge_bounds[0]) &&
          isfinite(post->fog_edge_bounds[1]) &&
          isfinite(post->fog_edge_bounds[2]) &&
          isfinite(post->fog_edge_bounds[3]) &&
          post->fog_edge_bounds[2] > post->fog_edge_bounds[0] &&
          post->fog_edge_bounds[3] > post->fog_edge_bounds[1] &&
          isfinite(post->fog_edge_fade_distance) &&
          post->fog_edge_fade_distance > 0.0f &&
          isfinite(post->fog_edge_clear_distance) &&
          post->fog_edge_clear_distance >= 0.0f)) &&
        isfinite(post->fog_color[0]) && post->fog_color[0] >= 0.0f &&
        isfinite(post->fog_color[1]) && post->fog_color[1] >= 0.0f &&
        isfinite(post->fog_color[2]) && post->fog_color[2] >= 0.0f &&
        isfinite(post->fog_density) && post->fog_density >= 0.0f &&
        isfinite(post->fog_start_distance) &&
        post->fog_start_distance >= 0.0f &&
        isfinite(post->fog_maximum_opacity) &&
        post->fog_maximum_opacity >= 0.0f &&
        post->fog_maximum_opacity <= 1.0f &&
        isfinite(post->bloom_intensity) && post->bloom_intensity >= 0.0f &&
        isfinite(post->bloom_threshold) && post->bloom_threshold >= 0.0f &&
        isfinite(post->bloom_soft_knee) &&
        post->bloom_soft_knee >= 0.0f &&
        post->bloom_soft_knee <= 1.0f &&
        isfinite(post->bloom_scatter) &&
        post->bloom_scatter >= 0.0f &&
        post->bloom_scatter <= 1.0f &&
        /* The vignette's fields only count when it is on, so a zeroed struct stays valid. */
        (post->vignette_enabled == 0u ||
         (isfinite(post->vignette_intensity) &&
          post->vignette_intensity >= 0.0f &&
          post->vignette_intensity <= 1.0f &&
          isfinite(post->vignette_smoothness) &&
          post->vignette_smoothness > 0.0f &&
          post->vignette_smoothness <= 1.0f));
}

static bool btbh_gpu_scene_surface_requires_shadow_discard(
    uint32_t surface_variant
) {
    return
        surface_variant == BTBH_GPU_SCENE_SURFACE_DISSOLVE ||
        surface_variant == BTBH_GPU_SCENE_SURFACE_SHATTER;
}

static bool btbh_gpu_scene_surface_uses_fast_shadow_vertex(
    uint32_t surface_variant
) {
    return
        surface_variant == BTBH_GPU_SCENE_SURFACE_NONE ||
        surface_variant == BTBH_GPU_SCENE_SURFACE_GLITTER_STATIC ||
        surface_variant == BTBH_GPU_SCENE_SURFACE_GLITTER ||
        surface_variant == BTBH_GPU_SCENE_SURFACE_RIM;
}

static bool btbh_gpu_scene_render_directional_shadow(
    SDL_GPUCommandBuffer *command,
    BTBHGPUSceneRuntime *runtime,
    uint32_t shadow_index,
    SDL_GPUTexture *shadow_texture,
    const float light_view_projection[16],
    const float camera_right[3],
    const float camera_up[3],
    SDL_GPUBuffer *transient_instance_buffer,
    SDL_GPUBuffer *transient_instance_vertex_buffer,
    SDL_GPUBuffer *bone_buffer,
    const uint32_t *bone_offsets,
    const BTBHGPUSceneGeometryBatch *batches,
    const uint8_t *directional_shadow_visibility,
    uint32_t batch_count,
    uint8_t shadow_layer,
    bool cycle_texture,
    bool bind_telemetry,
    BTBHGPUSceneTelemetryReport *telemetry
) {
    SDL_GPUDepthStencilTargetInfo depth_target;
    SDL_zero(depth_target);
    depth_target.texture = shadow_texture;
    depth_target.clear_depth = 1.0f;
    depth_target.load_op = SDL_GPU_LOADOP_CLEAR;
    depth_target.store_op = SDL_GPU_STOREOP_STORE;
    depth_target.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth_target.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth_target.cycle = cycle_texture;
    depth_target.layer = shadow_layer;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(
        command,
        NULL,
        0u,
        &depth_target
    );
    if (pass == NULL) {
        return false;
    }

    uint32_t geometry_cursor = 0u;
    SDL_GPUGraphicsPipeline *bound_pipeline = NULL;
    SDL_GPUBuffer *bound_index_buffer = NULL;
    bool shadow_uniform_bound = false;
    for (uint32_t batch_index = 0u;
         batch_index < batch_count;
         ++batch_index) {
        const BTBHGPUSceneGeometryBatch *batch = &batches[batch_index];
        SDL_GPUBuffer *batch_instance_buffer =
            batch->instance_buffer != NULL
                ? batch->instance_buffer->buffer
                : transient_instance_buffer;
        const uint32_t geometry_count =
            batch->kind == BTBH_GPU_SCENE_GEOMETRY_MODEL
                ? batch->model->primitive_count
                : 1u;
        for (uint32_t geometry_index = 0u;
             geometry_index < geometry_count;
             ++geometry_index) {
            if (!btbh_gpu_scene_batch_uses_geometry(
                    batch,
                    geometry_index
                )) {
                continue;
            }
            const uint32_t bone_offset =
                bone_offsets[geometry_cursor];
            geometry_cursor += 1u;
            if (batch->transparency_enabled != 0u ||
                (shadow_index == BTBH_GPU_SCENE_SHADOW_SPOT
                    ? batch->spot_shadow_visible == 0u
                    : (directional_shadow_visibility != NULL &&
                       directional_shadow_visibility[batch_index] == 0u))) {
                continue;
            }
            const bool animated = bone_offset != UINT32_MAX;
            const bool requires_shadow_discard =
                btbh_gpu_scene_surface_requires_shadow_discard(
                    batch->surface_variant
                );
            const bool uses_fast_shadow =
                !animated &&
                btbh_gpu_scene_surface_uses_fast_shadow_vertex(
                    batch->surface_variant
                ) &&
                batch->billboard_mode ==
                    BTBH_GPU_SCENE_BILLBOARD_DISABLED;
            if (uses_fast_shadow) {
                if (!shadow_uniform_bound) {
                    SDL_PushGPUVertexUniformData(
                        command,
                        0u,
                        light_view_projection,
                        sizeof(float) * 16u
                    );
                    shadow_uniform_bound = true;
                }
            } else {
                BTBHGPUShadowUniforms uniforms;
                (void)memset(&uniforms, 0, sizeof(uniforms));
                (void)memcpy(
                    uniforms.light_view_projection,
                    light_view_projection,
                    sizeof(uniforms.light_view_projection)
                );
                (void)memcpy(
                    uniforms.camera_right,
                    camera_right,
                    sizeof(float) * 3u
                );
                (void)memcpy(
                    uniforms.camera_up,
                    camera_up,
                    sizeof(float) * 3u
                );
                uniforms.bone_offset = animated ? bone_offset : 0u;
                uniforms.billboard_mode =
                    (uint32_t)batch->billboard_mode;
                uniforms.surface_variant =
                    (uint32_t)batch->surface_variant;
                uniforms.surface_time = batch->surface_time;
                (void)memcpy(
                    uniforms.surface_parameters0,
                    batch->surface_parameters0,
                    sizeof(uniforms.surface_parameters0)
                );
                (void)memcpy(
                    uniforms.surface_parameters1,
                    batch->surface_parameters1,
                    sizeof(uniforms.surface_parameters1)
                );
                SDL_PushGPUVertexUniformData(
                    command,
                    0u,
                    &uniforms,
                    sizeof(uniforms)
                );
                shadow_uniform_bound = true;
            }
            SDL_GPUGraphicsPipeline *shadow_pipeline = NULL;
            if (animated) {
                shadow_pipeline = requires_shadow_discard
                    ? runtime->animated_shadow_pipelines[
                        batch->cull_mode
                    ]
                    : runtime
                        ->animated_non_discard_shadow_pipelines[
                            batch->cull_mode
                        ];
            } else {
                shadow_pipeline = uses_fast_shadow
                    ? runtime->fast_shadow_pipelines[
                        batch->cull_mode
                    ]
                    : requires_shadow_discard
                        ? runtime->shadow_pipelines[
                            batch->cull_mode
                        ]
                        : runtime->non_discard_shadow_pipelines[
                            batch->cull_mode
                        ];
            }
            if (shadow_pipeline != bound_pipeline) {
                SDL_BindGPUGraphicsPipeline(pass, shadow_pipeline);
                bound_pipeline = shadow_pipeline;
                if (bind_telemetry) {
                    telemetry->shadow_pipeline_bind_count += 1u;
                }
            } else if (bind_telemetry) {
                telemetry->shadow_pipeline_bind_skip_count += 1u;
            }
            if (animated) {
                SDL_GPUBuffer *storage_buffers[2] = {
                    batch_instance_buffer,
                    bone_buffer
                };
                SDL_BindGPUVertexStorageBuffers(
                    pass,
                    0u,
                    storage_buffers,
                    2u
                );
            } else if (!uses_fast_shadow) {
                SDL_BindGPUVertexStorageBuffers(
                    pass,
                    0u,
                    &batch_instance_buffer,
                    1u
                );
            }
            SDL_GPUBuffer *vertex_buffer = NULL;
            SDL_GPUBuffer *index_buffer = NULL;
            uint32_t vertex_count = 0u;
            uint32_t index_count = 0u;
            uint32_t first_index = 0u;
            int32_t vertex_base = 0;
            if (batch->kind == BTBH_GPU_SCENE_GEOMETRY_MODEL) {
                const BTBHGPUModelPrimitive *primitive =
                    &batch->model->primitives[geometry_index];
                vertex_buffer = primitive->vertex_buffer;
                index_buffer = primitive->index_buffer;
                vertex_count = primitive->vertex_count;
                index_count = primitive->index_count;
                first_index = primitive->first_index;
                vertex_base = primitive->vertex_base;
            } else {
                vertex_buffer = batch->custom_mesh->vertex_buffer;
                index_buffer = batch->custom_mesh->index_buffer;
                vertex_count = batch->custom_mesh->vertex_count;
                index_count = batch->custom_mesh->index_count;
            }
            SDL_GPUBufferBinding vertex_bindings[2];
            SDL_zeroa(vertex_bindings);
            vertex_bindings[0].buffer = vertex_buffer;
            uint32_t vertex_binding_count = 1u;
            if (uses_fast_shadow) {
                vertex_bindings[1].buffer =
                    batch->instance_buffer != NULL
                        ? batch->instance_buffer->vertex_buffer
                        : transient_instance_vertex_buffer;
                vertex_bindings[1].offset =
                    batch->first_instance *
                    BTBH_GPU_SCENE_INSTANCE_STRIDE;
                vertex_binding_count = 2u;
            }
            SDL_BindGPUVertexBuffers(
                pass,
                0u,
                vertex_bindings,
                vertex_binding_count
            );
            const uint32_t draw_first_instance =
                uses_fast_shadow ? 0u : batch->first_instance;
            if (btbh_gpu_cost_skips_model(
                    batch->kind == BTBH_GPU_SCENE_GEOMETRY_MODEL
                        ? batch->model
                        : NULL
                )) {
                /* BTBH_GPU_COST_SKIP_MODEL: not drawn (measurement). */
            } else if (index_buffer != NULL && index_count > 0u) {
                /* SDL tracks every index bind with a scan of the buffers the command buffer used so far, and the
                   binding outlives pipeline changes within a pass: consecutive draws of one geometry (a model's
                   instance ranges, an enemy's batches) bind it once (2026-10-17). */
                if (index_buffer != bound_index_buffer) {
                    SDL_GPUBufferBinding index_binding;
                    SDL_zero(index_binding);
                    index_binding.buffer = index_buffer;
                    SDL_BindGPUIndexBuffer(
                        pass,
                        &index_binding,
                        SDL_GPU_INDEXELEMENTSIZE_32BIT
                    );
                    bound_index_buffer = index_buffer;
                }
                SDL_DrawGPUIndexedPrimitives(
                    pass,
                    index_count,
                    batch->instance_count,
                    first_index,
                    vertex_base,
                    draw_first_instance
                );
            } else {
                SDL_DrawGPUPrimitives(
                    pass,
                    vertex_count,
                    batch->instance_count,
                    (uint32_t) vertex_base,
                    draw_first_instance
                );
            }
            btbh_gpu_cost_record_draw(
                BTBH_GPU_COST_PASS_SHADOW,
                index_buffer != NULL && index_count > 0u
                    ? index_count
                    : vertex_count,
                batch->instance_count,
                batch->instance_buffer != NULL,
                batch->kind == BTBH_GPU_SCENE_GEOMETRY_MODEL
                    ? batch->model
                    : NULL,
                batch->kind == BTBH_GPU_SCENE_GEOMETRY_MODEL
                    ? NULL
                    : batch->custom_mesh
            );
        }
    }
    SDL_EndGPURenderPass(pass);
    return true;
}

static bool btbh_gpu_scene_render_fullscreen(
    SDL_GPUCommandBuffer *command,
    const char *debug_label,
    SDL_GPUGraphicsPipeline *pipeline,
    SDL_GPUTexture *target,
    const void *uniforms,
    uint32_t uniform_size,
    SDL_GPUTextureSamplerBinding *samplers,
    uint32_t sampler_count
) {
    SDL_InsertGPUDebugLabel(command, debug_label);
    SDL_GPUColorTargetInfo color_target;
    SDL_zero(color_target);
    color_target.texture = target;
    color_target.clear_color.r = 0.0f;
    color_target.clear_color.g = 0.0f;
    color_target.clear_color.b = 0.0f;
    color_target.clear_color.a = 1.0f;
    color_target.load_op = SDL_GPU_LOADOP_CLEAR;
    color_target.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(
        command,
        &color_target,
        1u,
        NULL
    );
    if (pass == NULL) {
        return false;
    }
    if (uniforms != NULL && uniform_size > 0u) {
        SDL_PushGPUFragmentUniformData(
            command,
            0u,
            uniforms,
            uniform_size
        );
    }
    SDL_BindGPUGraphicsPipeline(pass, pipeline);
    if (samplers != NULL && sampler_count > 0u) {
        SDL_BindGPUFragmentSamplers(
            pass,
            0u,
            samplers,
            sampler_count
        );
    }
    SDL_DrawGPUPrimitives(pass, 3u, 1u, 0u, 0u);
    SDL_EndGPURenderPass(pass);
    return true;
}

static bool btbh_gpu_scene_encode_rc_material_sources(
    SDL_GPUCommandBuffer *command,
    BTBHGPUSceneRuntime *runtime,
    const BTBHGPUSceneRadianceCascades *config,
    const float camera_right[3],
    const float camera_up[3],
    const BTBHGPUSceneGeometryBatch *batches,
    uint32_t batch_count,
    const uint32_t *bone_offsets,
    uint32_t prepared_geometry_count,
    SDL_GPUBuffer *transient_instance_buffer,
    SDL_GPUBuffer *bone_buffer
) {
    if (command == NULL || runtime == NULL || config == NULL ||
        (batches == NULL && batch_count > 0u) ||
        (bone_offsets == NULL && prepared_geometry_count > 0u) ||
        runtime->radiance_cascades.source_field_texture == NULL ||
        runtime->radiance_cascades.material_source_pipelines[0] == NULL ||
        runtime->radiance_cascades.material_source_pipelines[1] == NULL ||
        runtime->radiance_cascades.source_field_width == 0u ||
        runtime->radiance_cascades.source_field_height == 0u) {
        return false;
    }
    float origin_x = 0.0f;
    float origin_z = 0.0f;
    float cell_size = 1.0f;
    if (config->occlusion_cascade_count > 0u) {
        origin_x = config->occlusion_cascades[0].origin_world_x;
        origin_z = config->occlusion_cascades[0].origin_world_z;
        cell_size = config->occlusion_cascades[0].cell_size;
    } else {
        origin_x = -0.5f *
            (float)runtime->radiance_cascades.source_field_width;
        origin_z = -0.5f *
            (float)runtime->radiance_cascades.source_field_height;
    }
    const float inverse_world_width = 1.0f /
        ((float)runtime->radiance_cascades.source_field_width * cell_size);
    const float inverse_world_height = 1.0f /
        ((float)runtime->radiance_cascades.source_field_height * cell_size);

    SDL_GPUColorTargetInfo target;
    SDL_zero(target);
    target.texture = runtime->radiance_cascades.source_field_texture;
    target.load_op = SDL_GPU_LOADOP_LOAD;
    target.store_op = SDL_GPU_STOREOP_STORE;
    SDL_InsertGPUDebugLabel(command, "BTBH RC Material Sources");
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(
        command,
        &target,
        1u,
        NULL
    );
    if (pass == NULL) {
        return false;
    }

    uint32_t geometry_cursor = 0u;
    SDL_GPUGraphicsPipeline *bound_pipeline = NULL;
    SDL_GPUBuffer *bound_index_buffer = NULL;
    for (uint32_t batch_index = 0u;
         batch_index < batch_count;
         ++batch_index) {
        const BTBHGPUSceneGeometryBatch *batch = &batches[batch_index];
        const bool material_emits =
            batch->surface_variant != BTBH_GPU_SCENE_SURFACE_REFRACT &&
            ((batch->unlit_enabled != 0u && batch->rc_receiver == 0u) ||
             (batch->emission_energy > 0.0f &&
              (batch->emission_red != 0u ||
               batch->emission_green != 0u ||
               batch->emission_blue != 0u)));
        SDL_GPUBuffer *batch_instance_buffer =
            batch->instance_buffer != NULL
                ? batch->instance_buffer->buffer
                : transient_instance_buffer;
        const uint32_t geometry_count =
            batch->kind == BTBH_GPU_SCENE_GEOMETRY_MODEL
                ? batch->model->primitive_count
                : 1u;
        for (uint32_t geometry_index = 0u;
             geometry_index < geometry_count;
             ++geometry_index) {
            if (!btbh_gpu_scene_batch_uses_geometry(
                    batch,
                    geometry_index
                )) {
                continue;
            }
            const uint32_t bone_offset = bone_offsets[geometry_cursor];
            geometry_cursor += 1u;
            if (!material_emits) {
                continue;
            }
            const bool animated = bone_offset != UINT32_MAX;
            SDL_GPUGraphicsPipeline *pipeline =
                runtime->radiance_cascades.material_source_pipelines[
                    animated ? 1u : 0u
                ];
            if (pipeline != bound_pipeline) {
                SDL_BindGPUGraphicsPipeline(pass, pipeline);
                bound_pipeline = pipeline;
            }

            BTBHGPUSceneUniforms uniforms;
            (void)memset(&uniforms, 0, sizeof(uniforms));
            (void)memcpy(
                uniforms.camera_right,
                camera_right,
                sizeof(float) * 3u
            );
            (void)memcpy(
                uniforms.camera_up,
                camera_up,
                sizeof(float) * 3u
            );
            uniforms.material_color[0] =
                (float)batch->albedo_red / 255.0f;
            uniforms.material_color[1] =
                (float)batch->albedo_green / 255.0f;
            uniforms.material_color[2] =
                (float)batch->albedo_blue / 255.0f;
            uniforms.material_color[3] =
                (float)batch->albedo_alpha / 255.0f;
            uniforms.uv_transform[0] = batch->uv_scale_x;
            uniforms.uv_transform[1] = batch->uv_scale_y;
            uniforms.uv_transform[2] = batch->uv_offset_x;
            uniforms.uv_transform[3] = batch->uv_offset_y;
            uniforms.ambient_color_energy[0] = origin_x;
            uniforms.ambient_color_energy[1] = origin_z;
            uniforms.ambient_color_energy[2] = inverse_world_width;
            uniforms.ambient_color_energy[3] = inverse_world_height;
            uniforms.bone_offset = animated ? bone_offset : 0u;
            uniforms.billboard_mode = (uint32_t)batch->billboard_mode;
            uniforms.unlit_enabled = (uint32_t)batch->unlit_enabled;
            uniforms.surface_variant = (uint32_t)batch->surface_variant;
            uniforms.surface_controls[0] = batch->surface_time;
            (void)memcpy(
                uniforms.surface_parameters0,
                batch->surface_parameters0,
                sizeof(uniforms.surface_parameters0)
            );
            (void)memcpy(
                uniforms.surface_parameters1,
                batch->surface_parameters1,
                sizeof(uniforms.surface_parameters1)
            );
            SDL_PushGPUVertexUniformData(
                command,
                0u,
                &uniforms,
                (uint32_t)sizeof(uniforms)
            );

            BTBHGPUSceneFragmentUniforms fragment_uniforms;
            (void)memset(
                &fragment_uniforms,
                0,
                sizeof(fragment_uniforms)
            );
            fragment_uniforms.emission_color_energy[0] =
                (float)batch->emission_red / 255.0f;
            fragment_uniforms.emission_color_energy[1] =
                (float)batch->emission_green / 255.0f;
            fragment_uniforms.emission_color_energy[2] =
                (float)batch->emission_blue / 255.0f;
            fragment_uniforms.emission_color_energy[3] =
                batch->emission_energy;
            fragment_uniforms.unlit_enabled =
                (uint32_t)batch->unlit_enabled |
                (batch->rc_receiver != 0u ? 4u : 0u);
            fragment_uniforms.alpha_cutoff = batch->alpha_cutoff;
            fragment_uniforms.surface_variant =
                batch->surface_variant ==
                        BTBH_GPU_SCENE_SURFACE_SOFT_PARTICLE
                    ? (uint32_t)BTBH_GPU_SCENE_SURFACE_NONE
                    : (uint32_t)batch->surface_variant;
            fragment_uniforms.surface_time = batch->surface_time;
            SDL_PushGPUFragmentUniformData(
                command,
                0u,
                &fragment_uniforms,
                (uint32_t)offsetof(
                    BTBHGPUSceneFragmentUniforms,
                    surface_parameters0
                )
            );

            SDL_GPUBuffer *storage_buffers[2] = {
                batch_instance_buffer,
                bone_buffer
            };
            SDL_BindGPUVertexStorageBuffers(
                pass,
                0u,
                storage_buffers,
                animated ? 2u : 1u
            );
            BTBHGPUTexture *albedo_texture =
                batch->albedo_texture != NULL
                    ? batch->albedo_texture
                    : runtime->fallback_texture;
            BTBHGPUTexture *emission_texture =
                batch->emission_texture != NULL
                    ? batch->emission_texture
                    : runtime->fallback_texture;
            SDL_GPUTextureSamplerBinding samplers[2];
            SDL_zeroa(samplers);
            samplers[0].texture = albedo_texture->texture;
            samplers[0].sampler = albedo_texture->sampler;
            samplers[1].texture = emission_texture->texture;
            samplers[1].sampler = emission_texture->sampler;
            SDL_BindGPUFragmentSamplers(pass, 0u, samplers, 2u);

            SDL_GPUBuffer *vertex_buffer = NULL;
            SDL_GPUBuffer *index_buffer = NULL;
            uint32_t vertex_count = 0u;
            uint32_t index_count = 0u;
            uint32_t first_index = 0u;
            int32_t vertex_base = 0;
            if (batch->kind == BTBH_GPU_SCENE_GEOMETRY_MODEL) {
                const BTBHGPUModelPrimitive *primitive =
                    &batch->model->primitives[geometry_index];
                vertex_buffer = primitive->vertex_buffer;
                index_buffer = primitive->index_buffer;
                vertex_count = primitive->vertex_count;
                index_count = primitive->index_count;
                first_index = primitive->first_index;
                vertex_base = primitive->vertex_base;
            } else {
                vertex_buffer = batch->custom_mesh->vertex_buffer;
                index_buffer = batch->custom_mesh->index_buffer;
                vertex_count = batch->custom_mesh->vertex_count;
                index_count = batch->custom_mesh->index_count;
            }
            SDL_GPUBufferBinding vertex_binding;
            SDL_zero(vertex_binding);
            vertex_binding.buffer = vertex_buffer;
            SDL_BindGPUVertexBuffers(pass, 0u, &vertex_binding, 1u);
            if (btbh_gpu_cost_skips_model(
                    batch->kind == BTBH_GPU_SCENE_GEOMETRY_MODEL
                        ? batch->model
                        : NULL
                )) {
                /* BTBH_GPU_COST_SKIP_MODEL: not drawn (measurement). */
            } else if (index_buffer != NULL && index_count > 0u) {
                /* SDL tracks every index bind with a scan of the buffers the command buffer used so far, and the
                   binding outlives pipeline changes within a pass: consecutive draws of one geometry (a model's
                   instance ranges, an enemy's batches) bind it once (2026-10-17). */
                if (index_buffer != bound_index_buffer) {
                    SDL_GPUBufferBinding index_binding;
                    SDL_zero(index_binding);
                    index_binding.buffer = index_buffer;
                    SDL_BindGPUIndexBuffer(
                        pass,
                        &index_binding,
                        SDL_GPU_INDEXELEMENTSIZE_32BIT
                    );
                    bound_index_buffer = index_buffer;
                }
                SDL_DrawGPUIndexedPrimitives(
                    pass,
                    index_count,
                    batch->instance_count,
                    first_index,
                    vertex_base,
                    batch->first_instance
                );
            } else {
                SDL_DrawGPUPrimitives(
                    pass,
                    vertex_count,
                    batch->instance_count,
                    (uint32_t) vertex_base,
                    batch->first_instance
                );
            }
            btbh_gpu_cost_record_draw(
                BTBH_GPU_COST_PASS_RC_SOURCE,
                index_buffer != NULL && index_count > 0u
                    ? index_count
                    : vertex_count,
                batch->instance_count,
                batch->instance_buffer != NULL,
                batch->kind == BTBH_GPU_SCENE_GEOMETRY_MODEL
                    ? batch->model
                    : NULL,
                batch->kind == BTBH_GPU_SCENE_GEOMETRY_MODEL
                    ? NULL
                    : batch->custom_mesh
            );
        }
    }
    SDL_EndGPURenderPass(pass);
    return geometry_cursor == prepared_geometry_count;
}

static BTBHGPUResult btbh_gpu_window_scene_present_instances_internal(
    BTBHGPUWindow *window,
    const char *vertex_shader_path,
    const char *animated_vertex_shader_path,
    const char *fast_shadow_vertex_shader_path,
    const char *fast_shadow_fragment_shader_path,
    const char *shadow_vertex_shader_path,
    const char *animated_shadow_vertex_shader_path,
    const char *shadow_fragment_shader_path,
    const char *fragment_shader_path,
    const char *unshadowed_fragment_shader_path,
    const char *post_vertex_shader_path,
    const char *post_fragment_shader_path,
    BTBHGPUShaderFormat shader_format,
    uint32_t render_width,
    uint32_t render_height,
    const float requested_view_projection[16],
    const float camera_right[3],
    const float camera_up[3],
    const float camera_position[3],
    const BTBHGPUSceneLighting *lighting,
    const BTBHGPUScenePost *post,
    const float *instances,
    uint32_t transient_instance_count,
    uint32_t logical_instance_count,
    const BTBHGPUSceneGeometryBatch *batches,
    const uint8_t *scene_visibility,
    const uint8_t *directional_shadow_visibility,
    uint32_t batch_count,
    uint8_t clear_red,
    uint8_t clear_green,
    uint8_t clear_blue,
    uint8_t clear_alpha,
    BTBHGPUSceneReport *report
) {
    if (report == NULL) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    (void)memset(report, 0, sizeof(*report));
    if (window != NULL) {
        (void)memset(
            &window->scene_telemetry,
            0,
            sizeof(window->scene_telemetry)
        );
    }
    const uint64_t prepare_started_ticks = SDL_GetTicksNS();
    const bool detailed_prepare_telemetry =
        window != NULL && window->frame_telemetry_enabled;
    if (window != NULL &&
        (window->pending_scene_ready ||
         window->pending_command != NULL)) {
        if (window->pending_command != NULL) {
            (void)btbh_gpu_window_finish_pending_frame(window, false);
        } else {
            window->pending_scene_ready = false;
        }
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            "GPU scene frame was not finalized by Canvas2D"
        );
        return report->result;
    }
    if (window == NULL ||
        window->window == NULL ||
        window->device == NULL ||
        window->device->gpu == NULL ||
        !window->claimed ||
        vertex_shader_path == NULL ||
        animated_vertex_shader_path == NULL ||
        fast_shadow_vertex_shader_path == NULL ||
        fast_shadow_fragment_shader_path == NULL ||
        shadow_vertex_shader_path == NULL ||
        animated_shadow_vertex_shader_path == NULL ||
        shadow_fragment_shader_path == NULL ||
        fragment_shader_path == NULL ||
        unshadowed_fragment_shader_path == NULL ||
        post_vertex_shader_path == NULL ||
        post_fragment_shader_path == NULL ||
        render_width == 0u ||
        render_height == 0u ||
        requested_view_projection == NULL ||
        camera_right == NULL ||
        camera_up == NULL ||
        camera_position == NULL ||
        !isfinite(camera_position[0]) ||
        !isfinite(camera_position[1]) ||
        !isfinite(camera_position[2]) ||
        !btbh_gpu_scene_lighting_valid(lighting) ||
        !btbh_gpu_scene_post_valid(post) ||
        (instances == NULL && transient_instance_count > 0u) ||
        (post != NULL &&
         post->temporal_instance_motions != NULL &&
         post->temporal_instance_motion_count != transient_instance_count) ||
        logical_instance_count == 0u ||
        batches == NULL ||
        batch_count == 0u ||
        btbh_gpu_scene_shader_format(shader_format) ==
            SDL_GPU_SHADERFORMAT_INVALID ||
        (uint64_t)transient_instance_count *
            BTBH_GPU_SCENE_INSTANCE_FLOATS * sizeof(float) > UINT32_MAX ||
        !btbh_gpu_scene_validate_batches(
            window,
            transient_instance_count,
            batches,
            scene_visibility,
            directional_shadow_visibility,
            batch_count
        )) {
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            "Invalid GPU model instance scene submission"
        );
        return report->result;
    }
    /* The scene renders with this view-projection: the requested one, plus a sub-pixel jitter
       while the temporal upscaler runs. */
    float view_projection[16];
    (void)memcpy(
        view_projection,
        requested_view_projection,
        sizeof(view_projection)
    );
    float inverse_view_projection[16];
    const float projection_scale_y = sqrtf(
        view_projection[1] * view_projection[1] +
        view_projection[5] * view_projection[5] +
        view_projection[9] * view_projection[9]
    );
    if (!btbh_gpu_scene_invert_matrix4(
            view_projection,
            inverse_view_projection
        ) ||
        !isfinite(projection_scale_y) ||
        projection_scale_y <= 0.0f) {
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            "GPU scene view-projection matrix is not invertible"
        );
        return report->result;
    }
    window->radiance_cascades_telemetry_enabled =
        post->radiance_cascades.tier == BTBH_GPU_SCENE_RC_AUTO;
    if (!btbh_gpu_scene_ensure_runtime(
            window,
            report
        )) {
        return report->result;
    }
    BTBHGPUSceneTelemetryReport *telemetry =
        &window->scene_telemetry;
    if (!btbh_gpu_scene_ensure_brdf_lookup(
            window,
            fragment_shader_path,
            report
        )) {
        return report->result;
    }
    BTBHGPUSceneRuntime *runtime = window->scene_runtime;
    char radiance_cascades_message[256];
    radiance_cascades_message[0] = '\0';
    const bool radiance_cascades_ready =
        btbh_gpu_radiance_cascades_runtime_prepare(
            window->device->gpu,
            &runtime->radiance_cascades,
            &post->radiance_cascades,
            post_vertex_shader_path,
            post_fragment_shader_path,
            shader_format,
            runtime->scene_color_format,
            render_width,
            render_height,
            radiance_cascades_message,
            sizeof(radiance_cascades_message)
        );
    if (!radiance_cascades_ready &&
        !runtime->radiance_cascades.logged_failure) {
        SDL_LogWarn(
            SDL_LOG_CATEGORY_RENDER,
            "Radiance Cascades disabled: %s",
            radiance_cascades_message[0] != '\0'
                ? radiance_cascades_message
                : "unknown setup failure"
        );
        runtime->radiance_cascades.logged_failure = true;
    }
    const bool radiance_cascades_active =
        radiance_cascades_ready &&
        btbh_gpu_radiance_cascades_effective_tier(
            &runtime->radiance_cascades.state
        ) != BTBH_GPU_SCENE_RC_OFF;
    const bool radiance_cascades_fallback_debug =
        !radiance_cascades_ready &&
        post->radiance_cascades.debug_view ==
            BTBH_GPU_SCENE_RC_DEBUG_FALLBACK_REASON &&
        runtime->radiance_cascades.compose_pipeline != NULL &&
        runtime->radiance_cascades.sampler != NULL &&
        runtime->radiance_cascades.composed_texture != NULL;
    const bool radiance_cascades_debug_output =
        (radiance_cascades_active ||
         radiance_cascades_fallback_debug) &&
        post->radiance_cascades.debug_view !=
            BTBH_GPU_SCENE_RC_DEBUG_NONE;
    /* Temporal upscaling (FSR 3.1): the projection takes a sub-pixel jitter, the opaque pass writes
       motion vectors and the transparent pass a reactive mask, and the upscaler replaces FXAA and
       FSR 1 after the post chain. A device without its storage formats or shaders keeps FSR 1. */
    int temporal_window_width = 0;
    int temporal_window_height = 0;
    const bool temporal_requested =
        post->upscale_mode == BTBH_GPU_SCENE_UPSCALE_FSR3 &&
        !radiance_cascades_debug_output &&
        SDL_GetWindowSizeInPixels(
            window->window,
            &temporal_window_width,
            &temporal_window_height
        ) &&
        temporal_window_width > 0 &&
        temporal_window_height > 0;
    bool temporal_active = false;
    if (temporal_requested) {
        char temporal_message[256];
        temporal_active = btbh_gpu_fsr3_prepare(
            window->device->gpu,
            &runtime->fsr3,
            post_fragment_shader_path,
            shader_format,
            render_width,
            render_height,
            (uint32_t)temporal_window_width,
            (uint32_t)temporal_window_height,
            temporal_message,
            sizeof(temporal_message)
        );
        if (!temporal_active && !runtime->fsr3.logged_failure) {
            SDL_LogWarn(
                SDL_LOG_CATEGORY_RENDER,
                "FSR 3 upscaling unavailable, using FSR 1: %s",
                temporal_message
            );
            runtime->fsr3.logged_failure = true;
        }
    }
    if (!runtime->temporal_debug_view_read) {
        /* BTBH_DEBUG_MOTION_VECTORS=1 shows the upscaler's inputs, =2 the motion vectors' reprojection
           error; BTBH_FSR3_DEBUG_VIEW=1 AMD's view. */
        const char *motion_view = SDL_getenv("BTBH_DEBUG_MOTION_VECTORS");
        const char *upscaler_view = SDL_getenv("BTBH_FSR3_DEBUG_VIEW");
        runtime->temporal_debug_view =
            motion_view != NULL && motion_view[0] == '1'
                ? BTBH_GPU_FSR3_DEBUG_MOTION_VECTORS
                : (motion_view != NULL && motion_view[0] == '2'
                    ? BTBH_GPU_FSR3_DEBUG_MOTION_ERROR
                    : (upscaler_view != NULL && upscaler_view[0] == '1'
                        ? BTBH_GPU_FSR3_DEBUG_UPSCALER
                        : BTBH_GPU_FSR3_DEBUG_NONE));
        /* BTBH_FSR3_JITTER=off renders without the sub-pixel jitter (to read the motion-error view). */
        const char *jitter_mode = SDL_getenv("BTBH_FSR3_JITTER");
        runtime->temporal_jitter_disabled =
            jitter_mode != NULL && strcmp(jitter_mode, "off") == 0;
        /* BTBH_FSR3_MIP_BIAS=<bias> or =off fixes the texture LOD bias (A/B captures). */
        const char *lod_bias = SDL_getenv("BTBH_FSR3_MIP_BIAS");
        runtime->temporal_lod_bias_overridden = false;
        if (lod_bias != NULL && strcmp(lod_bias, "off") == 0) {
            runtime->temporal_lod_bias_overridden = true;
            runtime->temporal_lod_bias_override = 0.0f;
        } else if (lod_bias != NULL && lod_bias[0] != '\0') {
            char *end = NULL;
            const float value = strtof(lod_bias, &end);
            if (end != lod_bias && isfinite(value)) {
                runtime->temporal_lod_bias_overridden = true;
                runtime->temporal_lod_bias_override = value;
            }
        }
        runtime->temporal_debug_view_read = true;
    }
    const uint8_t upscale_mode =
        post->upscale_mode == BTBH_GPU_SCENE_UPSCALE_FSR3 && !temporal_active
            ? (uint8_t)BTBH_GPU_SCENE_UPSCALE_FSR1
            : post->upscale_mode;
    const uint8_t anti_aliasing = temporal_active ? 0u : post->anti_aliasing;
    /* Under FSR 3 the material textures and the procedural surface detail pick their detail for the
       output size, not the render size (AMD's guide: log2(render width / display width) - 1, so -1 at
       a render scale of 1 and -2 at 50 %): the upscaler accumulates the jittered frames into that
       detail, which a render-size LOD had already blurred away (paving faded at 50 %). */
    float temporal_lod_bias = 0.0f;
    if (temporal_active) {
        temporal_lod_bias = runtime->temporal_lod_bias_overridden
            ? runtime->temporal_lod_bias_override
            : log2f((float)render_width / (float)temporal_window_width) - 1.0f;
        temporal_lod_bias = fmaxf(-4.0f, fminf(0.0f, temporal_lod_bias));
    }
    const bool temporal_motion_ready =
        temporal_active &&
        post->temporal_instance_motions != NULL &&
        transient_instance_count > 0u;
    float temporal_controls[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float previous_view_projection[16];
    (void)memcpy(
        previous_view_projection,
        requested_view_projection,
        sizeof(previous_view_projection)
    );
    float previous_world_curvature[4];
    (void)memcpy(
        previous_world_curvature,
        post->world_curvature,
        sizeof(previous_world_curvature)
    );
    if (temporal_active) {
        const bool temporal_reset =
            post->temporal_reset != 0u ||
            !runtime->temporal_previous_valid;
        if (temporal_reset) {
            btbh_gpu_fsr3_invalidate_history(&runtime->fsr3);
        } else {
            (void)memcpy(
                previous_view_projection,
                runtime->temporal_previous_view_projection,
                sizeof(previous_view_projection)
            );
            (void)memcpy(
                previous_world_curvature,
                runtime->temporal_previous_world_curvature,
                sizeof(previous_world_curvature)
            );
        }
        float jitter_x = 0.0f;
        float jitter_y = 0.0f;
        btbh_gpu_fsr3_next_jitter(
            &runtime->fsr3,
            render_width,
            (uint32_t)temporal_window_width,
            &jitter_x,
            &jitter_y
        );
        if (runtime->temporal_jitter_disabled) {
            jitter_x = 0.0f;
            jitter_y = 0.0f;
            runtime->fsr3.jitter_x = 0.0f;
            runtime->fsr3.jitter_y = 0.0f;
        }
        /* AMD's convention: the jitter is in render pixels (y down), the projection moves by
           (2 x / width, -2 y / height) in clip space. */
        temporal_controls[0] = 2.0f * jitter_x / (float)render_width;
        temporal_controls[1] = -2.0f * jitter_y / (float)render_height;
        temporal_controls[3] = 1.0f;
        for (uint32_t column = 0u; column < 4u; ++column) {
            view_projection[column * 4u + 0u] +=
                temporal_controls[0] * view_projection[column * 4u + 3u];
            view_projection[column * 4u + 1u] +=
                temporal_controls[1] * view_projection[column * 4u + 3u];
        }
        if (!btbh_gpu_scene_invert_matrix4(
                view_projection,
                inverse_view_projection
            )) {
            btbh_gpu_scene_error(
                report,
                BTBH_GPU_INVALID_ARGUMENT,
                "GPU scene jittered view-projection matrix is not invertible"
            );
            return report->result;
        }
    }
    const bool ambient_occlusion_active =
        post->ssao_enabled != 0u &&
        !radiance_cascades_debug_output;
    /* Reflections ride the SSAO resolve, which then runs without the occlusion passes too. */
    const bool ssr_active =
        post->ssr_intensity > 0.0f &&
        post->ssr_max_roughness > 0.0f &&
        post->ssr_max_distance > 0.0f &&
        !radiance_cascades_debug_output;
    const bool ssao_resolve_active =
        ambient_occlusion_active || ssr_active;
    /* RC debug views show raw buffers, so they skip the HDR composite. */
    const bool bloom_active =
        post->bloom_enabled != 0u &&
        !radiance_cascades_debug_output;
    const bool hdr_composite_active =
        (post->fog_enabled != 0u ||
         post->bloom_enabled != 0u ||
         post->vignette_enabled != 0u) &&
        !radiance_cascades_debug_output;
    const bool soft_particles_active =
        batch_count > 0u &&
        batches != NULL &&
        btbh_gpu_scene_batches_have_soft_particles(batches, batch_count);
    const bool refraction_active =
        batch_count > 0u &&
        batches != NULL &&
        btbh_gpu_scene_batches_have_refraction(
            batches,
            batch_count,
            scene_visibility
        );
    const bool auxiliary_geometry_outputs =
        ssao_resolve_active || radiance_cascades_active || temporal_active;
    uint32_t shadow_enabled[3] = {
        lighting->directional_shadow_enabled,
        lighting->secondary_directional_shadow_enabled,
        lighting->spot_shadow_enabled
    };
    const bool requested_shadows_active =
        shadow_enabled[0] != 0u ||
        shadow_enabled[1] != 0u ||
        shadow_enabled[2] != 0u;
    if (requested_shadows_active &&
        !btbh_gpu_scene_ensure_shadow_resources(
            window,
            fast_shadow_vertex_shader_path,
            fast_shadow_fragment_shader_path,
            shadow_vertex_shader_path,
            animated_shadow_vertex_shader_path,
            shadow_fragment_shader_path,
            shader_format,
            report
        )) {
        return report->result;
    }
    const bool omni_shadow_requested =
        shadow_enabled[2] != 0u &&
        lighting->spot_shadow_is_omni != 0u;
    const bool omni_shadows_active =
        omni_shadow_requested &&
        btbh_gpu_scene_ensure_omni_shadow_texture(window);
    if (omni_shadow_requested && !omni_shadows_active) {
        shadow_enabled[2] = 0u;
    }
    const bool shadows_active =
        shadow_enabled[0] != 0u ||
        shadow_enabled[1] != 0u ||
        shadow_enabled[2] != 0u;

    if (anti_aliasing >= 1u &&
        anti_aliasing <= BTBH_GPU_SCENE_SMAA_QUALITY_COUNT &&
        !btbh_gpu_scene_ensure_smaa_resources(
            window,
            post_vertex_shader_path,
            post_fragment_shader_path,
            shader_format,
            anti_aliasing,
            report
        )) {
        return report->result;
    }
    if (anti_aliasing == BTBH_GPU_SCENE_ANTI_ALIASING_FXAA &&
        !btbh_gpu_scene_ensure_fxaa_pipeline(
            window,
            post_vertex_shader_path,
            post_fragment_shader_path,
            shader_format,
            report
        )) {
        return report->result;
    }

    uint32_t *bone_offsets = NULL;
    uint32_t prepared_geometry_count = 0u;
    float *bone_matrices = NULL;
    uint32_t bone_count = 0u;
    const uint64_t bones_started_ticks = detailed_prepare_telemetry
        ? SDL_GetTicksNS()
        : 0u;
    if (detailed_prepare_telemetry) {
        telemetry->cpu_prepare_setup_milliseconds =
            (double)(bones_started_ticks - prepare_started_ticks) /
            1000000.0;
    }
    if (!temporal_active ||
        post->temporal_reset != 0u ||
        !runtime->temporal_previous_valid) {
        runtime->temporal_palette_count = 0u;
    }
    if (
        !btbh_gpu_scene_prepare_bones(
            batches,
            batch_count,
            &bone_offsets,
            &prepared_geometry_count,
            &bone_matrices,
            &bone_count,
            detailed_prepare_telemetry,
            telemetry,
            temporal_active ? runtime : NULL
        ) ||
        bone_count > UINT32_MAX / (sizeof(float) * 16u)
    ) {
        free(bone_matrices);
        free(bone_offsets);
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            "Could not prepare GPU animation pose"
        );
        return report->result;
    }
    const uint64_t bones_finished_ticks = detailed_prepare_telemetry
        ? SDL_GetTicksNS()
        : 0u;
    if (detailed_prepare_telemetry) {
        telemetry->cpu_prepare_bones_milliseconds =
            (double)(bones_finished_ticks - bones_started_ticks) /
            1000000.0;
    }
    uint32_t pipeline_geometry_cursor = 0u;
    for (uint32_t batch_index = 0u;
         batch_index < batch_count;
         ++batch_index) {
        const BTBHGPUSceneGeometryBatch *batch = &batches[batch_index];
        const uint32_t geometry_count =
            batch->kind == BTBH_GPU_SCENE_GEOMETRY_MODEL
                ? batch->model->primitive_count
                : 1u;
        for (uint32_t geometry_index = 0u;
             geometry_index < geometry_count;
             ++geometry_index) {
            if (!btbh_gpu_scene_batch_uses_geometry(
                    batch,
                    geometry_index
                )) {
                continue;
            }
            const bool animated =
                bone_offsets[pipeline_geometry_cursor] != UINT32_MAX;
            pipeline_geometry_cursor += 1u;
            if (!btbh_gpu_scene_ensure_model_pipeline(
                    window,
                    vertex_shader_path,
                    animated_vertex_shader_path,
                    fragment_shader_path,
                    unshadowed_fragment_shader_path,
                    shader_format,
                    auxiliary_geometry_outputs,
                    temporal_active,
                    radiance_cascades_active,
                    shadows_active,
                    omni_shadows_active,
                    animated,
                    batch,
                    report
                )) {
                free(bone_matrices);
                free(bone_offsets);
                return report->result;
            }
            if (
                auxiliary_geometry_outputs &&
                btbh_gpu_scene_uses_transparent_pass(
                    batch->blend_mode,
                    batch->transparency_enabled != 0u
                ) &&
                !btbh_gpu_scene_ensure_model_pipeline(
                    window,
                    vertex_shader_path,
                    animated_vertex_shader_path,
                    fragment_shader_path,
                    unshadowed_fragment_shader_path,
                    shader_format,
                    false,
                    temporal_active,
                    radiance_cascades_active,
                    shadows_active,
                    omni_shadows_active,
                    animated,
                    batch,
                    report
                )
            ) {
                free(bone_matrices);
                free(bone_offsets);
                return report->result;
            }
        }
    }
    if (pipeline_geometry_cursor != prepared_geometry_count) {
        free(bone_matrices);
        free(bone_offsets);
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            "GPU scene pipeline preparation changed during submission"
        );
        return report->result;
    }
    const uint64_t pipelines_finished_ticks = detailed_prepare_telemetry
        ? SDL_GetTicksNS()
        : 0u;
    if (detailed_prepare_telemetry) {
        telemetry->cpu_prepare_pipelines_milliseconds =
            (double)(
                pipelines_finished_ticks - bones_finished_ticks
            ) /
            1000000.0;
    }

    SDL_GPUDevice *device = window->device->gpu;
    const uint32_t instance_bytes =
        transient_instance_count *
        BTBH_GPU_SCENE_INSTANCE_FLOATS *
        (uint32_t)sizeof(float);
    const uint32_t bone_bytes =
        bone_count * (uint32_t)(sizeof(float) * 16u);
    if (bone_bytes > UINT32_MAX - instance_bytes) {
        free(bone_matrices);
        free(bone_offsets);
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            "GPU scene upload exceeds buffer limits"
        );
        return report->result;
    }
    const uint32_t motion_bytes = temporal_motion_ready
        ? transient_instance_count * 4u * (uint32_t)sizeof(float)
        : 0u;
    if (motion_bytes > UINT32_MAX - instance_bytes - bone_bytes) {
        free(bone_matrices);
        free(bone_offsets);
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            "GPU scene upload exceeds buffer limits"
        );
        return report->result;
    }
    const uint32_t transfer_bytes = instance_bytes + bone_bytes + motion_bytes;
    if (!btbh_gpu_scene_ensure_upload_buffers(
            window,
            instance_bytes,
            bone_bytes,
            transfer_bytes
        ) ||
        !btbh_gpu_scene_ensure_motion_buffer(window, motion_bytes)) {
        free(bone_matrices);
        free(bone_offsets);
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return report->result;
    }
    SDL_GPUBuffer *transient_instance_buffer = instance_bytes > 0u
        ? runtime->instance_buffer
        : NULL;
    SDL_GPUBuffer *transient_instance_vertex_buffer =
        instance_bytes > 0u
            ? runtime->instance_vertex_buffer
            : NULL;
    SDL_GPUBuffer *bone_buffer = bone_bytes > 0u
        ? runtime->bone_buffer
        : NULL;
    SDL_GPUBuffer *motion_buffer = motion_bytes > 0u
        ? runtime->motion_buffer
        : NULL;
    SDL_GPUTransferBuffer *transfer = runtime->upload_transfer;
    const uint64_t buffers_finished_ticks = detailed_prepare_telemetry
        ? SDL_GetTicksNS()
        : 0u;
    if (detailed_prepare_telemetry) {
        telemetry->cpu_prepare_buffers_milliseconds =
            (double)(
                buffers_finished_ticks - pipelines_finished_ticks
            ) /
            1000000.0;
    }
    report->cpu_prepare_milliseconds =
        (double)(
            (
                detailed_prepare_telemetry
                    ? buffers_finished_ticks
                    : SDL_GetTicksNS()
            ) -
            prepare_started_ticks
        ) /
        1000000.0;
    const uint64_t upload_started_ticks = SDL_GetTicksNS();
    if (transfer_bytes > 0u) {
        void *mapped = SDL_MapGPUTransferBuffer(device, transfer, true);
        if (mapped == NULL) {
            free(bone_matrices);
            free(bone_offsets);
            btbh_gpu_scene_error(
                report,
                BTBH_GPU_DEVICE_FAILURE,
                SDL_GetError()
            );
            return report->result;
        }
        if (instance_bytes > 0u) {
            (void)memcpy(mapped, instances, instance_bytes);
        }
        if (bone_bytes > 0u) {
            (void)memcpy(
                (uint8_t *)mapped + instance_bytes,
                bone_matrices,
                bone_bytes
            );
        }
        if (motion_bytes > 0u) {
            (void)memcpy(
                (uint8_t *)mapped + instance_bytes + bone_bytes,
                post->temporal_instance_motions,
                motion_bytes
            );
        }
        SDL_UnmapGPUTransferBuffer(device, transfer);
    }
    free(bone_matrices);
    bone_matrices = NULL;

    SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(device);
    if (command == NULL) {
        free(bone_offsets);
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return report->result;
    }
    btbh_gpu_cost_begin_frame(device);
    if (transfer_bytes > 0u) {
        SDL_InsertGPUDebugLabel(command, "BTBH Scene Upload");
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command);
        if (copy == NULL) {
            btbh_gpu_scene_cancel_submission(window, command);
            free(bone_offsets);
            btbh_gpu_scene_error(
                report,
                BTBH_GPU_DEVICE_FAILURE,
                SDL_GetError()
            );
            return report->result;
        }
        SDL_GPUTransferBufferLocation source;
        SDL_GPUBufferRegion destination;
        SDL_zero(source);
        SDL_zero(destination);
        source.transfer_buffer = transfer;
        if (transient_instance_buffer != NULL) {
            destination.buffer = transient_instance_buffer;
            destination.size = instance_bytes;
            SDL_UploadToGPUBuffer(copy, &source, &destination, true);
            destination.buffer = transient_instance_vertex_buffer;
            SDL_UploadToGPUBuffer(copy, &source, &destination, true);
        }
        if (bone_buffer != NULL) {
            source.offset = instance_bytes;
            destination.buffer = bone_buffer;
            destination.size = bone_bytes;
            SDL_UploadToGPUBuffer(copy, &source, &destination, true);
        }
        if (motion_buffer != NULL) {
            source.offset = instance_bytes + bone_bytes;
            destination.buffer = motion_buffer;
            destination.size = motion_bytes;
            SDL_UploadToGPUBuffer(copy, &source, &destination, true);
        }
        SDL_EndGPUCopyPass(copy);
    }
    report->cpu_upload_milliseconds =
        (double)(SDL_GetTicksNS() - upload_started_ticks) /
        1000000.0;
    if (!btbh_gpu_scene_cost_split(
            window,
            &command,
            BTBH_GPU_COST_GROUP_UPLOAD,
            report
        )) {
        free(bone_offsets);
        return report->result;
    }

    const float *shadow_matrices[3] = {
        lighting->directional_shadow_view_projection,
        lighting->secondary_directional_shadow_view_projection,
        lighting->spot_shadow_view_projection
    };
    const uint32_t shadow_update_periods[3] = {
        btbh_gpu_scene_shadow_update_period(window, 0u),
        btbh_gpu_scene_shadow_update_period(window, 1u),
        btbh_gpu_scene_shadow_update_period(window, 2u)
    };
    static const char *shadow_debug_labels[3] = {
        "BTBH Shadow Primary",
        "BTBH Shadow Secondary",
        "BTBH Shadow Local"
    };
    uint32_t active_shadow_ordinal = 0u;
    const uint64_t shadow_started_ticks = SDL_GetTicksNS();
    for (uint32_t shadow_index = 0u;
         shadow_index < 3u;
         ++shadow_index) {
        if (shadow_enabled[shadow_index] == 0u) {
            runtime->retained_shadow_active[shadow_index] = 0u;
            continue;
        }
        const uint32_t shadow_update_period =
            shadow_update_periods[shadow_index];
        const uint32_t update_phase =
            active_shadow_ordinal % shadow_update_period;
        active_shadow_ordinal += 1u;
        const bool force_update =
            runtime->retained_shadow_active[shadow_index] == 0u ||
            runtime->retained_shadow_ready[shadow_index] == 0u ||
            (shadow_index == BTBH_GPU_SCENE_SHADOW_SPOT &&
             runtime->retained_spot_shadow_is_omni !=
                (uint8_t)(lighting->spot_shadow_is_omni != 0u));
        runtime->retained_shadow_active[shadow_index] = 1u;
        const bool should_update =
            force_update ||
            shadow_update_period == 1u ||
            runtime->shadow_frame_serial % shadow_update_period ==
                update_phase;
        if (!should_update) {
            continue;
        }
        SDL_InsertGPUDebugLabel(
            command,
            shadow_debug_labels[shadow_index]
        );
        bool shadow_rendered = true;
        const bool omni_shadow =
            shadow_index == BTBH_GPU_SCENE_SHADOW_SPOT &&
            lighting->spot_shadow_is_omni != 0u;
        if (omni_shadow) {
            for (uint32_t face = 0u;
                 face < BTBH_GPU_OMNI_SHADOW_FACE_COUNT;
                 ++face) {
                if (!btbh_gpu_scene_render_directional_shadow(
                        command,
                        runtime,
                        shadow_index,
                        runtime->omni_shadow_texture,
                        lighting->omni_shadow_view_projection[face],
                        camera_right,
                        camera_up,
                        transient_instance_buffer,
                        transient_instance_vertex_buffer,
                        bone_buffer,
                        bone_offsets,
                        batches,
                        directional_shadow_visibility,
                        batch_count,
                        (uint8_t)face,
                        face == 0u,
                        detailed_prepare_telemetry,
                        telemetry
                    )) {
                    shadow_rendered = false;
                    break;
                }
            }
        } else {
            shadow_rendered = btbh_gpu_scene_render_directional_shadow(
                command,
                runtime,
                shadow_index,
                runtime->directional_shadow_textures[shadow_index],
                shadow_matrices[shadow_index],
                camera_right,
                camera_up,
                transient_instance_buffer,
                transient_instance_vertex_buffer,
                bone_buffer,
                bone_offsets,
                batches,
                directional_shadow_visibility,
                batch_count,
                0u,
                true,
                detailed_prepare_telemetry,
                telemetry
            );
        }
        if (!shadow_rendered) {
            btbh_gpu_scene_cancel_submission(window, command);
            free(bone_offsets);
            btbh_gpu_scene_error(
                report,
                BTBH_GPU_DEVICE_FAILURE,
                SDL_GetError()
            );
            return report->result;
        }
        if (omni_shadow) {
            (void)memcpy(
                runtime->retained_omni_shadow_view_projection,
                lighting->omni_shadow_view_projection,
                sizeof(runtime->retained_omni_shadow_view_projection)
            );
            runtime->retained_omni_shadow_position_range[0] =
                lighting->spot_position_x;
            runtime->retained_omni_shadow_position_range[1] =
                lighting->spot_position_y;
            runtime->retained_omni_shadow_position_range[2] =
                lighting->spot_position_z;
            runtime->retained_omni_shadow_position_range[3] =
                lighting->spot_range;
        } else {
            (void)memcpy(
                runtime->retained_shadow_view_projection[shadow_index],
                shadow_matrices[shadow_index],
                sizeof(
                    runtime->retained_shadow_view_projection[shadow_index]
                )
            );
        }
        if (shadow_index == BTBH_GPU_SCENE_SHADOW_SPOT) {
            runtime->retained_spot_shadow_is_omni =
                (uint8_t)omni_shadow;
        }
        runtime->retained_shadow_ready[shadow_index] = 1u;
    }
    runtime->shadow_frame_serial += 1u;
    if (shadows_active) {
        report->cpu_shadow_encode_milliseconds =
            (double)(SDL_GetTicksNS() - shadow_started_ticks) /
            1000000.0;
    }
    if (!btbh_gpu_scene_cost_split(
            window,
            &command,
            BTBH_GPU_COST_GROUP_SHADOW,
            report
        )) {
        free(bone_offsets);
        return report->result;
    }

    int drawable_width_value = 0;
    int drawable_height_value = 0;
    const bool drawable_size_available =
        SDL_GetWindowSizeInPixels(
            window->window,
            &drawable_width_value,
            &drawable_height_value
        ) &&
        drawable_width_value > 0 &&
        drawable_height_value > 0;
    const uint32_t drawable_width =
        drawable_size_available
            ? (uint32_t)drawable_width_value
            : 0u;
    const uint32_t drawable_height =
        drawable_size_available
            ? (uint32_t)drawable_height_value
            : 0u;
    report->cpu_swapchain_acquire_milliseconds = 0.0;
    const uint64_t target_ensure_started_ticks = SDL_GetTicksNS();
    const bool targets_ready =
        drawable_size_available &&
        drawable_width != 0u &&
        drawable_height != 0u &&
        btbh_gpu_scene_ensure_depth(
            window,
            render_width,
            render_height,
            report
        ) &&
        btbh_gpu_scene_ensure_color(
            window,
            render_width,
            render_height,
            report
        ) &&
        btbh_gpu_scene_ensure_anti_aliasing_targets(
            window,
            render_width,
            render_height,
            anti_aliasing != 0u ||
                post->mirror_horizontal != 0u,
            anti_aliasing >= 1u &&
                anti_aliasing <=
                    BTBH_GPU_SCENE_SMAA_QUALITY_COUNT,
            report
        ) &&
        (!temporal_active ||
         btbh_gpu_scene_ensure_temporal_targets(
             window,
             render_width,
             render_height,
             report
         )) &&
        (upscale_mode == BTBH_GPU_SCENE_UPSCALE_LINEAR ||
         (!temporal_active &&
          render_width == drawable_width &&
          render_height == drawable_height) ||
         (btbh_gpu_scene_ensure_upscale_pipelines(
              window,
              temporal_active
                  ? (uint8_t)BTBH_GPU_SCENE_UPSCALE_FSR1
                  : upscale_mode,
              post_vertex_shader_path,
              post_fragment_shader_path,
              shader_format,
              report
          ) &&
          btbh_gpu_scene_ensure_upscaled_color_target(
              window,
              drawable_width,
              drawable_height,
              upscale_mode == BTBH_GPU_SCENE_UPSCALE_FSR1 ||
                  temporal_active,
              report
          ))) &&
        (!ssao_resolve_active ||
         btbh_gpu_scene_ensure_ssao_resolve_pipeline(
             window,
             post_vertex_shader_path,
             post_fragment_shader_path,
             shader_format,
              report
          )) &&
        btbh_gpu_scene_ensure_post_stage_pipelines(
            window,
            post_vertex_shader_path,
            post_fragment_shader_path,
            shader_format,
            report
        ) &&
        (!hdr_composite_active ||
         btbh_gpu_scene_ensure_hdr_composite_pipelines(
             window,
             post_vertex_shader_path,
             post_fragment_shader_path,
             shader_format,
             report
         )) &&
        (!bloom_active ||
         btbh_gpu_scene_ensure_bloom_targets(
             window,
             render_width,
             render_height,
             report
         )) &&
        btbh_gpu_scene_ensure_post_pipeline(
            window,
            post_vertex_shader_path,
            post_fragment_shader_path,
            shader_format,
            report
        );
    /* Without the distance pass soft particles simply draw unfaded; a missing pass never fails
       the frame. */
    BTBHGPUSceneReport scene_distance_report;
    SDL_zero(scene_distance_report);
    /* Refraction reads the distance too: a displaced sample nearer than the refracting surface (a unit in
       front of it) falls back to the undisplaced one. */
    const bool scene_distance_ready =
        (soft_particles_active || refraction_active) &&
        btbh_gpu_scene_ensure_scene_distance_pipeline(
            window,
            post_vertex_shader_path,
            post_fragment_shader_path,
            shader_format,
            &scene_distance_report
        ) &&
        btbh_gpu_scene_ensure_scene_distance_target(
            window,
            render_width,
            render_height,
            &scene_distance_report
        );
    report->cpu_target_ensure_milliseconds =
        (double)(SDL_GetTicksNS() - target_ensure_started_ticks) /
        1000000.0;
    if (!targets_ready) {
        if (report->result == BTBH_GPU_OK) {
            btbh_gpu_scene_error(
                report,
                BTBH_GPU_WINDOW_FAILURE,
                SDL_GetError()
            );
        }
        btbh_gpu_scene_cancel_submission(window, command);
        free(bone_offsets);
        return report->result;
    }

    SDL_GPUColorTargetInfo color_targets[6];
    SDL_GPUDepthStencilTargetInfo depth_target;
    SDL_zeroa(color_targets);
    SDL_zero(depth_target);
    color_targets[0].texture =
        window->scene_runtime->scene_color_texture;
    float scene_clear[3] = {
        (float)clear_red / 255.0f,
        (float)clear_green / 255.0f,
        (float)clear_blue / 255.0f,
    };
    /* With fog the cleared background is the horizon. It is cleared to the
     * fog colour here, in float precision, instead of being fogged by the
     * composite: transparent effects write no depth, so over the background
     * the composite could not tell them apart and would paint them over. */
    if (hdr_composite_active && post->fog_enabled != 0u) {
        const float fog_background = post->fog_edge_enabled != 0u
            ? 1.0f
            : post->fog_maximum_opacity;
        for (uint32_t channel = 0u; channel < 3u; ++channel) {
            scene_clear[channel] +=
                (post->fog_color[channel] - scene_clear[channel]) *
                fog_background;
        }
    }
    color_targets[0].clear_color.r = scene_clear[0];
    color_targets[0].clear_color.g = scene_clear[1];
    color_targets[0].clear_color.b = scene_clear[2];
    color_targets[0].clear_color.a = (float)clear_alpha / 255.0f;
    color_targets[0].load_op = SDL_GPU_LOADOP_CLEAR;
    color_targets[0].store_op = SDL_GPU_STOREOP_STORE;
    color_targets[1].texture =
        window->scene_runtime->ambient_texture;
    color_targets[1].clear_color.a = 1.0f;
    color_targets[1].load_op = SDL_GPU_LOADOP_CLEAR;
    color_targets[1].store_op = SDL_GPU_STOREOP_STORE;
    color_targets[2].texture =
        window->scene_runtime->normal_texture;
    color_targets[2].clear_color.r = 0.5f;
    color_targets[2].clear_color.g = 0.5f;
    color_targets[2].clear_color.b = 1.0f;
    color_targets[2].clear_color.a = 1.0f;
    color_targets[2].load_op = SDL_GPU_LOADOP_CLEAR;
    color_targets[2].store_op = SDL_GPU_STOREOP_STORE;
    color_targets[3].texture =
        window->scene_runtime->ambient_specular_texture;
    color_targets[3].clear_color.a = 1.0f;
    color_targets[3].load_op = SDL_GPU_LOADOP_CLEAR;
    color_targets[3].store_op = SDL_GPU_STOREOP_STORE;
    color_targets[4].texture =
        window->scene_runtime->emission_texture;
    color_targets[4].load_op = SDL_GPU_LOADOP_CLEAR;
    color_targets[4].store_op = SDL_GPU_STOREOP_STORE;
    color_targets[5].texture = window->scene_runtime->velocity_texture;
    color_targets[5].load_op = SDL_GPU_LOADOP_CLEAR;
    color_targets[5].store_op = SDL_GPU_STOREOP_STORE;
    const uint32_t color_target_count = temporal_active
        ? 6u
        : (auxiliary_geometry_outputs ? 5u : 1u);
    depth_target.texture = window->scene_runtime->depth_texture;
    depth_target.clear_depth = 1.0f;
    depth_target.clear_stencil = 0u;
    depth_target.load_op = SDL_GPU_LOADOP_CLEAR;
    depth_target.store_op = SDL_GPU_STOREOP_STORE;
    depth_target.stencil_load_op = SDL_GPU_LOADOP_CLEAR;
    depth_target.stencil_store_op = SDL_GPU_STOREOP_STORE;
    depth_target.cycle = true;

    SDL_InsertGPUDebugLabel(command, "BTBH Scene Geometry");
    uint32_t primitive_draw_count = 0u;
    uint64_t geometry_encode_nanoseconds = 0u;
    SDL_GPUTexture *radiance_cascades_post_input =
        runtime->scene_color_texture;
    double radiance_cascades_encode_milliseconds = 0.0;
    const bool hero_omni_shadow_ready =
        omni_shadows_active &&
        runtime->retained_shadow_ready[2] != 0u &&
        runtime->retained_spot_shadow_is_omni != 0u;
    BTBHGPUPostUniforms post_uniforms;
    (void)memset(&post_uniforms, 0, sizeof(post_uniforms));
    post_uniforms.inverse_resolution[0] =
        1.0f / (float)render_width;
    post_uniforms.inverse_resolution[1] =
        1.0f / (float)render_height;
    post_uniforms.tonemap_white = post->tonemap_white;
    post_uniforms.cas_sharpness = post->cas_sharpness;
    post_uniforms.focus_point[0] = post->focus_point_x;
    post_uniforms.focus_point[1] = post->focus_point_y;
    post_uniforms.focus_direction[0] = post->focus_direction_x;
    post_uniforms.focus_direction[1] = post->focus_direction_y;
    post_uniforms.focus_half_width = post->focus_half_width;
    post_uniforms.maximum_radius_pixels =
        post->maximum_radius_pixels;
    post_uniforms.blur_curve = post->blur_curve;
    post_uniforms.highlight_threshold =
        post->highlight_threshold;
    post_uniforms.highlight_gain = post->highlight_gain;
    post_uniforms.lut_enabled = post->lut_enabled;
    post_uniforms.cas_enabled = post->cas_enabled;
    /* 2: the tilt-shift blur turns its kernel the same way at every pixel. Its per-pixel random turn
       is noise fixed to the screen, which the temporal upscaler cannot reproject with the city: it
       left stripes over everything the blur touched while the camera moved; the jitter now smooths
       the fixed kernel over the frames instead. */
    post_uniforms.tilt_shift_enabled = post->tilt_shift_enabled != 0u
        ? (temporal_active ? 2u : 1u)
        : 0u;
    post_uniforms.ssao_enabled = ambient_occlusion_active
        ? (radiance_cascades_active
            ? BTBH_GPU_SCENE_SSAO_RC_CONTACT_MODE
            : 1u)
        : 0u;
    post_uniforms.ssr_options[0] = radiance_cascades_active ? 1.0f : 0.0f;
    (void)memcpy(
        post_uniforms.view_projection,
        view_projection,
        sizeof(post_uniforms.view_projection)
    );
    if (post->ao_radius > 0.0f) {
        post_uniforms.ao_controls[0] = post->ao_radius;
        post_uniforms.ao_controls[1] = post->ao_intensity;
        post_uniforms.ao_controls[2] = post->ao_contact_strength;
        post_uniforms.ao_controls[3] = post->ao_direct_share;
    } else {
        /* A host that leaves the AO controls zeroed gets the fixed values they replaced. */
        post_uniforms.ao_controls[0] = 1.0f;
        post_uniforms.ao_controls[1] = 0.4f;
        post_uniforms.ao_controls[2] = 0.75f;
        post_uniforms.ao_controls[3] = 0.2f;
    }
    if (ssr_active) {
        post_uniforms.ssr_controls[0] = post->ssr_intensity;
        post_uniforms.ssr_controls[1] = post->ssr_max_roughness;
        post_uniforms.ssr_controls[2] = post->ssr_max_distance;
        post_uniforms.ssr_controls[3] = post->ssr_thickness;
    }
    post_uniforms.tonemap_mode = post->tonemap_mode;
    post_uniforms.exposure = post->exposure;
    post_uniforms.contrast = post->contrast;
    (void)memcpy(
        post_uniforms.inverse_view_projection,
        inverse_view_projection,
        sizeof(post_uniforms.inverse_view_projection)
    );
    post_uniforms.projection_scale_y = projection_scale_y;
    post_uniforms.saturation = post->saturation;
    /* Phases: opaque, transparent, and on frames that draw one, refraction (a transparent pass over a copy of
       the scene so far). */
    const uint32_t geometry_phase_count =
        BTBH_GPU_SCENE_TRANSPARENCY_COUNT + (refraction_active ? 1u : 0u);
    for (uint32_t transparency_phase = 0u;
         transparency_phase < geometry_phase_count;
         ++transparency_phase) {
        const uint64_t geometry_phase_started_ticks = SDL_GetTicksNS();
        const bool transparent_phase = transparency_phase != 0u;
        const bool refraction_phase =
            transparency_phase == BTBH_GPU_SCENE_TRANSPARENCY_COUNT;
        const bool phase_auxiliary_geometry_outputs =
            !transparent_phase && auxiliary_geometry_outputs;
        if (transparency_phase == 1u && ssao_resolve_active) {
            /* Ambient occlusion darkens the opaque scene only: resolved before the
               transparent pass, an effect drawn over a dark corner keeps its light. */
            bool ssao_ok = true;
            SDL_GPUTextureSamplerBinding ssao_samplers[2];
            SDL_zeroa(ssao_samplers);
            ssao_samplers[0].texture = runtime->depth_texture;
            ssao_samplers[0].sampler =
                runtime->scene_color_sampler;
            ssao_samplers[1].texture = runtime->normal_texture;
            ssao_samplers[1].sampler =
                runtime->scene_color_sampler;
            /* Reflections alone skip the occlusion passes: the resolve then reads no occlusion. */
            ssao_ok = !ambient_occlusion_active || btbh_gpu_scene_render_fullscreen(
                command,
                "BTBH SSAO",
                runtime->ssao_pipeline,
                runtime->ssao_texture,
                &post_uniforms,
                (uint32_t)sizeof(post_uniforms),
                ssao_samplers,
                2u
            );
            SDL_GPUTextureSamplerBinding denoise_samplers[3];
            SDL_zeroa(denoise_samplers);
            denoise_samplers[0].texture = runtime->depth_texture;
            denoise_samplers[0].sampler =
                runtime->scene_color_sampler;
            denoise_samplers[1].texture = runtime->normal_texture;
            denoise_samplers[1].sampler =
                runtime->scene_color_sampler;
            denoise_samplers[2].texture = runtime->ssao_texture;
            denoise_samplers[2].sampler =
                runtime->scene_color_sampler;
            if (ssao_ok && ambient_occlusion_active) {
                ssao_ok = btbh_gpu_scene_render_fullscreen(
                    command,
                    "BTBH SSAO Denoise",
                    runtime->ssao_denoise_pipeline,
                    runtime->ssao_denoised_texture,
                    &post_uniforms,
                    (uint32_t)sizeof(post_uniforms),
                    denoise_samplers,
                    3u
                );
            }
            SDL_GPUTextureSamplerBinding resolve_samplers[7];
            SDL_zeroa(resolve_samplers);
            resolve_samplers[0].texture =
                radiance_cascades_post_input;
            resolve_samplers[0].sampler =
                runtime->scene_color_sampler;
            resolve_samplers[1].texture = runtime->depth_texture;
            resolve_samplers[1].sampler =
                runtime->scene_color_sampler;
            resolve_samplers[2].texture = runtime->ambient_texture;
            resolve_samplers[2].sampler =
                runtime->scene_color_sampler;
            resolve_samplers[3].texture = runtime->normal_texture;
            resolve_samplers[3].sampler =
                runtime->scene_color_sampler;
            resolve_samplers[4].texture =
                runtime->ambient_specular_texture;
            resolve_samplers[4].sampler =
                runtime->scene_color_sampler;
            resolve_samplers[5].texture =
                runtime->ssao_denoised_texture;
            resolve_samplers[5].sampler =
                runtime->scene_color_sampler;
            resolve_samplers[6].texture = runtime->emission_texture;
            resolve_samplers[6].sampler =
                runtime->scene_color_sampler;
            if (ssao_ok) {
                ssao_ok = btbh_gpu_scene_render_fullscreen(
                    command,
                    "BTBH SSAO Resolve",
                    runtime->ssao_resolve_pipeline,
                    runtime->ssao_resolved_texture,
                    &post_uniforms,
                    (uint32_t)sizeof(post_uniforms),
                    resolve_samplers,
                    7u
                );
            }
            if (!ssao_ok) {
                btbh_gpu_scene_cancel_submission(window, command);
                free(bone_offsets);
                btbh_gpu_scene_error(
                    report,
                    BTBH_GPU_DEVICE_FAILURE,
                    SDL_GetError()
                );
                return report->result;
            }
            radiance_cascades_post_input = runtime->ssao_resolved_texture;
            if (!btbh_gpu_scene_cost_split(
                    window,
                    &command,
                    BTBH_GPU_COST_GROUP_SSAO,
                    report
                )) {
                free(bone_offsets);
                return report->result;
            }
        }
        if (transparency_phase == 1u && scene_distance_ready) {
            BTBHGPUSceneDistanceUniforms distance_uniforms;
            SDL_zero(distance_uniforms);
            (void)memcpy(
                distance_uniforms.inverse_view_projection,
                inverse_view_projection,
                sizeof(distance_uniforms.inverse_view_projection)
            );
            distance_uniforms.camera_position[0] = camera_position[0];
            distance_uniforms.camera_position[1] = camera_position[1];
            distance_uniforms.camera_position[2] = camera_position[2];
            SDL_GPUTextureSamplerBinding distance_sampler;
            SDL_zero(distance_sampler);
            distance_sampler.texture = runtime->depth_texture;
            distance_sampler.sampler = runtime->scene_color_sampler;
            if (!btbh_gpu_scene_render_fullscreen(
                    command,
                    "BTBH Scene Distance",
                    runtime->scene_distance_pipeline,
                    runtime->scene_distance_texture,
                    &distance_uniforms,
                    (uint32_t)sizeof(distance_uniforms),
                    &distance_sampler,
                    1u
                )) {
                btbh_gpu_scene_cancel_submission(window, command);
                free(bone_offsets);
                btbh_gpu_scene_error(
                    report,
                    BTBH_GPU_DEVICE_FAILURE,
                    SDL_GetError()
                );
                return report->result;
            }
        }
        if (refraction_phase) {
            if (!btbh_gpu_scene_ensure_refraction_target(
                    window,
                    render_width,
                    render_height,
                    report
                )) {
                btbh_gpu_scene_cancel_submission(window, command);
                free(bone_offsets);
                return report->result;
            }
            /* The refracting batches see the scene as the transparent pass left it. */
            SDL_GPUBlitInfo refraction_copy;
            SDL_zero(refraction_copy);
            refraction_copy.source.texture = radiance_cascades_post_input;
            refraction_copy.source.w = render_width;
            refraction_copy.source.h = render_height;
            refraction_copy.destination.texture = runtime->refraction_texture;
            refraction_copy.destination.w = render_width;
            refraction_copy.destination.h = render_height;
            refraction_copy.load_op = SDL_GPU_LOADOP_DONT_CARE;
            refraction_copy.filter = SDL_GPU_FILTER_NEAREST;
            SDL_BlitGPUTexture(command, &refraction_copy);
        }
        SDL_GPUColorTargetInfo transparent_targets[2];
        SDL_zeroa(transparent_targets);
        transparent_targets[0].texture = radiance_cascades_post_input;
        transparent_targets[0].load_op = SDL_GPU_LOADOP_LOAD;
        transparent_targets[0].store_op = SDL_GPU_STOREOP_STORE;
        transparent_targets[1].texture = runtime->reactive_texture;
        /* The refraction pass adds to the transparent pass's reactive mask. */
        transparent_targets[1].load_op = refraction_phase
            ? SDL_GPU_LOADOP_LOAD
            : SDL_GPU_LOADOP_CLEAR;
        transparent_targets[1].store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPUDepthStencilTargetInfo transparent_depth_target = depth_target;
        transparent_depth_target.load_op = SDL_GPU_LOADOP_LOAD;
        transparent_depth_target.stencil_load_op = SDL_GPU_LOADOP_LOAD;
        transparent_depth_target.cycle = false;
        SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(
            command,
            transparent_phase ? transparent_targets : color_targets,
            transparent_phase
                ? (temporal_active ? 2u : 1u)
                : color_target_count,
            transparent_phase ? &transparent_depth_target : &depth_target
        );
        if (pass == NULL) {
            btbh_gpu_scene_cancel_submission(window, command);
            free(bone_offsets);
            btbh_gpu_scene_error(
                report,
                BTBH_GPU_DEVICE_FAILURE,
                SDL_GetError()
            );
            return report->result;
        }
        SDL_GPUGraphicsPipeline *bound_model_pipeline = NULL;
        SDL_GPUBuffer *bound_index_buffer = NULL;
        SDL_InsertGPUDebugLabel(
            command,
            !transparent_phase
                ? "BTBH Opaque Draws"
                : (refraction_phase
                    ? "BTBH Refraction Draws"
                    : "BTBH Forward Transparent Draws")
        );
        uint32_t geometry_cursor = 0u;
        for (uint32_t batch_index = 0u;
             batch_index < batch_count;
             ++batch_index) {
        const BTBHGPUSceneGeometryBatch *batch = &batches[batch_index];
        SDL_GPUBuffer *batch_instance_buffer =
            batch->instance_buffer != NULL
                ? batch->instance_buffer->buffer
                : transient_instance_buffer;
        const uint32_t geometry_count =
            batch->kind == BTBH_GPU_SCENE_GEOMETRY_MODEL
                ? batch->model->primitive_count
                : 1u;
        for (uint32_t geometry_index = 0u;
             geometry_index < geometry_count;
            ++geometry_index) {
            if (!btbh_gpu_scene_batch_uses_geometry(
                    batch,
                    geometry_index
                )) {
                continue;
            }
            const uint32_t bone_offset =
                bone_offsets[geometry_cursor];
            const uint32_t previous_bone_offset =
                bone_offsets[prepared_geometry_count + geometry_cursor];
            geometry_cursor += 1u;
            const bool batch_refracts =
                batch->surface_variant == BTBH_GPU_SCENE_SURFACE_REFRACT;
            const uint32_t batch_transparency_phase =
                batch_refracts
                    ? BTBH_GPU_SCENE_TRANSPARENCY_COUNT
                    : (btbh_gpu_scene_uses_transparent_pass(
                          batch->blend_mode,
                          batch->transparency_enabled != 0u
                      ) ? 1u : 0u);
            if ((scene_visibility != NULL &&
                 scene_visibility[batch_index] == 0u) ||
                batch_transparency_phase != transparency_phase) {
                continue;
            }
            const bool animated = bone_offset != UINT32_MAX;
            BTBHGPUSceneUniforms uniforms;
            (void)memset(&uniforms, 0, sizeof(uniforms));
            (void)memcpy(
                uniforms.view_projection,
                view_projection,
                sizeof(uniforms.view_projection)
            );
            (void)memcpy(
                uniforms.camera_right,
                camera_right,
                sizeof(float) * 3u
            );
            (void)memcpy(
                uniforms.camera_up,
                camera_up,
                sizeof(float) * 3u
            );
            (void)memcpy(
                uniforms.camera_position,
                camera_position,
                sizeof(float) * 3u
            );
            uniforms.material_color[0] =
                (float)batch->albedo_red / 255.0f;
            uniforms.material_color[1] =
                (float)batch->albedo_green / 255.0f;
            uniforms.material_color[2] =
                (float)batch->albedo_blue / 255.0f;
            uniforms.material_color[3] =
                (float)batch->albedo_alpha / 255.0f;
            uniforms.uv_transform[0] = batch->uv_scale_x;
            uniforms.uv_transform[1] = batch->uv_scale_y;
            uniforms.uv_transform[2] = batch->uv_offset_x;
            uniforms.uv_transform[3] = batch->uv_offset_y;
            uniforms.ambient_color_energy[0] =
                lighting->ambient_red;
            uniforms.ambient_color_energy[1] =
                lighting->ambient_green;
            uniforms.ambient_color_energy[2] =
                lighting->ambient_blue;
            uniforms.ambient_color_energy[3] =
                lighting->ambient_energy;
            uniforms.directional_direction_energy[0] =
                lighting->directional_x;
            uniforms.directional_direction_energy[1] =
                lighting->directional_y;
            uniforms.directional_direction_energy[2] =
                lighting->directional_z;
            uniforms.directional_direction_energy[3] =
                lighting->directional_energy;
            uniforms.directional_color[0] =
                lighting->directional_red;
            uniforms.directional_color[1] =
                lighting->directional_green;
            uniforms.directional_color[2] =
                lighting->directional_blue;
            uniforms.directional_color[3] = 1.0f;
            uniforms.secondary_directional_direction_energy[0] =
                lighting->secondary_directional_x;
            uniforms.secondary_directional_direction_energy[1] =
                lighting->secondary_directional_y;
            uniforms.secondary_directional_direction_energy[2] =
                lighting->secondary_directional_z;
            uniforms.secondary_directional_direction_energy[3] =
                lighting->secondary_directional_energy;
            uniforms.secondary_directional_color[0] =
                lighting->secondary_directional_red;
            uniforms.secondary_directional_color[1] =
                lighting->secondary_directional_green;
            uniforms.secondary_directional_color[2] =
                lighting->secondary_directional_blue;
            uniforms.secondary_directional_color[3] = 1.0f;
            uniforms.bone_offset = animated ? bone_offset : 0u;
            uniforms.previous_bone_offset =
                animated ? previous_bone_offset : 0u;
            uniforms.billboard_mode =
                (uint32_t)batch->billboard_mode;
            uniforms.unlit_enabled =
                (uint32_t)batch->unlit_enabled;
            uniforms.surface_variant =
                (uint32_t)batch->surface_variant;
            uniforms.surface_controls[0] =
                batch->surface_time;
            (void)memcpy(
                uniforms.surface_parameters0,
                batch->surface_parameters0,
                sizeof(uniforms.surface_parameters0)
            );
            (void)memcpy(
                uniforms.surface_parameters1,
                batch->surface_parameters1,
                sizeof(uniforms.surface_parameters1)
            );
            (void)memcpy(
                uniforms.previous_view_projection,
                previous_view_projection,
                sizeof(uniforms.previous_view_projection)
            );
            (void)memcpy(
                uniforms.world_curvature,
                post->world_curvature,
                sizeof(uniforms.world_curvature)
            );
            (void)memcpy(
                uniforms.previous_world_curvature,
                previous_world_curvature,
                sizeof(uniforms.previous_world_curvature)
            );
            (void)memcpy(
                uniforms.temporal_controls,
                temporal_controls,
                sizeof(uniforms.temporal_controls)
            );
            /* Persistent instances (the static city) have no motion of their own. */
            const bool batch_motion =
                temporal_motion_ready && batch->instance_buffer == NULL;
            uniforms.temporal_controls[2] = batch_motion ? 1.0f : 0.0f;
            SDL_PushGPUVertexUniformData(
                command,
                0u,
                &uniforms,
                sizeof(uniforms)
            );
            BTBHGPUSceneFragmentUniforms fragment_uniforms;
            (void)memset(
                &fragment_uniforms,
                0,
                sizeof(fragment_uniforms)
            );
            (void)memcpy(
                fragment_uniforms.camera_position,
                camera_position,
                sizeof(float) * 3u
            );
            (void)memcpy(
                fragment_uniforms.ambient_color_energy,
                uniforms.ambient_color_energy,
                sizeof(fragment_uniforms.ambient_color_energy)
            );
            (void)memcpy(
                fragment_uniforms.directional_direction_energy,
                uniforms.directional_direction_energy,
                sizeof(fragment_uniforms.directional_direction_energy)
            );
            (void)memcpy(
                fragment_uniforms.directional_color,
                uniforms.directional_color,
                sizeof(fragment_uniforms.directional_color)
            );
            (void)memcpy(
                fragment_uniforms
                    .secondary_directional_direction_energy,
                uniforms.secondary_directional_direction_energy,
                sizeof(
                    fragment_uniforms
                        .secondary_directional_direction_energy
                )
            );
            (void)memcpy(
                fragment_uniforms.secondary_directional_color,
                uniforms.secondary_directional_color,
                sizeof(fragment_uniforms.secondary_directional_color)
            );
            fragment_uniforms.spot_position_range[0] =
                lighting->spot_position_x;
            fragment_uniforms.spot_position_range[1] =
                lighting->spot_position_y;
            fragment_uniforms.spot_position_range[2] =
                lighting->spot_position_z;
            fragment_uniforms.spot_position_range[3] =
                lighting->spot_range;
            fragment_uniforms.spot_direction_energy[0] =
                lighting->spot_direction_x;
            fragment_uniforms.spot_direction_energy[1] =
                lighting->spot_direction_y;
            fragment_uniforms.spot_direction_energy[2] =
                lighting->spot_direction_z;
            fragment_uniforms.spot_direction_energy[3] =
                lighting->spot_energy;
            fragment_uniforms.spot_color_falloff[0] =
                lighting->spot_red;
            fragment_uniforms.spot_color_falloff[1] =
                lighting->spot_green;
            fragment_uniforms.spot_color_falloff[2] =
                lighting->spot_blue;
            fragment_uniforms.spot_color_falloff[3] =
                lighting->spot_falloff;
            fragment_uniforms.spot_cutoffs[0] =
                lighting->spot_inner_cutoff;
            fragment_uniforms.spot_cutoffs[1] =
                lighting->spot_outer_cutoff;
            fragment_uniforms.spot_cutoffs[2] =
                lighting->spot_occluder_shadow_strength;
            fragment_uniforms.spot_cutoffs[3] =
                lighting->spot_occluder_shadow_reach;
            {
                const uint32_t point_count = SDL_min(
                    lighting->point_light_count,
                    (uint32_t)BTBH_GPU_SCENE_POINT_LIGHTS
                );
                fragment_uniforms.point_light_count[0] =
                    (float)point_count;
                for (uint32_t light = 0u; light < point_count; ++light) {
                    (void)memcpy(
                        fragment_uniforms.point_lights[light * 2u],
                        &lighting->point_lights[light][0],
                        4u * sizeof(float)
                    );
                    (void)memcpy(
                        fragment_uniforms.point_lights[light * 2u + 1u],
                        &lighting->point_lights[light][4],
                        4u * sizeof(float)
                    );
                }
            }
            fragment_uniforms.material_factors[0] =
                batch->normal_scale;
            fragment_uniforms.material_factors[1] =
                batch->occlusion;
            fragment_uniforms.material_factors[2] =
                batch->roughness;
            fragment_uniforms.material_factors[3] =
                batch->metalness;
            fragment_uniforms.emission_color_energy[0] =
                (float)batch->emission_red / 255.0f;
            fragment_uniforms.emission_color_energy[1] =
                (float)batch->emission_green / 255.0f;
            fragment_uniforms.emission_color_energy[2] =
                (float)batch->emission_blue / 255.0f;
            fragment_uniforms.emission_color_energy[3] =
                batch->emission_energy;
            fragment_uniforms.unlit_enabled =
                uniforms.unlit_enabled |
                (batch->rc_receiver != 0u ? 4u : 0u);
            fragment_uniforms.alpha_cutoff =
                batch->alpha_cutoff;
            fragment_uniforms.surface_variant =
                batch->surface_variant ==
                        BTBH_GPU_SCENE_SURFACE_SOFT_PARTICLE &&
                    !(transparent_phase && scene_distance_ready)
                    ? (uint32_t)BTBH_GPU_SCENE_SURFACE_NONE
                    : (uint32_t)batch->surface_variant;
            fragment_uniforms.surface_time =
                batch->surface_time;
            (void)memcpy(
                fragment_uniforms.surface_parameters0,
                batch->surface_parameters0,
                sizeof(fragment_uniforms.surface_parameters0)
            );
            (void)memcpy(
                fragment_uniforms.surface_parameters1,
                batch->surface_parameters1,
                sizeof(fragment_uniforms.surface_parameters1)
            );
            if (batch_refracts) {
                /* Reserved for the host: 1 tells the shader the normal slot carries the scene's distance. */
                fragment_uniforms.surface_parameters1[3] =
                    scene_distance_ready ? 1.0f : 0.0f;
            }
            for (size_t coefficient = 0u;
                 coefficient < 9u;
                 coefficient += 1u) {
                for (size_t channel = 0u;
                     channel < 3u;
                     channel += 1u) {
                    fragment_uniforms
                        .irradiance_sh[coefficient][channel] =
                        lighting->irradiance_sh[
                            coefficient * 3u + channel
                        ];
                }
            }
            fragment_uniforms.ground_bottom_energy[0] =
                lighting->ground_bottom_red;
            fragment_uniforms.ground_bottom_energy[1] =
                lighting->ground_bottom_green;
            fragment_uniforms.ground_bottom_energy[2] =
                lighting->ground_bottom_blue;
            fragment_uniforms.ground_bottom_energy[3] =
                lighting->ground_energy;
            fragment_uniforms.ground_horizon_curve[0] =
                lighting->ground_horizon_red;
            fragment_uniforms.ground_horizon_curve[1] =
                lighting->ground_horizon_green;
            fragment_uniforms.ground_horizon_curve[2] =
                lighting->ground_horizon_blue;
            fragment_uniforms.ground_horizon_curve[3] =
                lighting->ground_horizon_curve;
            fragment_uniforms.sky_top_energy[0] =
                lighting->sky_top_red;
            fragment_uniforms.sky_top_energy[1] =
                lighting->sky_top_green;
            fragment_uniforms.sky_top_energy[2] =
                lighting->sky_top_blue;
            fragment_uniforms.sky_top_energy[3] =
                lighting->sky_energy;
            fragment_uniforms.sky_horizon_curve[0] =
                lighting->sky_horizon_red;
            fragment_uniforms.sky_horizon_curve[1] =
                lighting->sky_horizon_green;
            fragment_uniforms.sky_horizon_curve[2] =
                lighting->sky_horizon_blue;
            fragment_uniforms.sky_horizon_curve[3] =
                lighting->sky_horizon_curve;
            fragment_uniforms.sun_color_energy[0] =
                lighting->sun_red;
            fragment_uniforms.sun_color_energy[1] =
                lighting->sun_green;
            fragment_uniforms.sun_color_energy[2] =
                lighting->sun_blue;
            fragment_uniforms.sun_color_energy[3] =
                lighting->sun_energy;
            fragment_uniforms.sun_direction_size[0] =
                lighting->sun_direction_x;
            fragment_uniforms.sun_direction_size[1] =
                lighting->sun_direction_y;
            fragment_uniforms.sun_direction_size[2] =
                lighting->sun_direction_z;
            fragment_uniforms.sun_direction_size[3] =
                lighting->sun_size_radians;
            fragment_uniforms.procedural_controls[0] =
                lighting->sun_curve;
            fragment_uniforms.procedural_controls[1] =
                (float)lighting->ibl_enabled;
            fragment_uniforms.procedural_controls[2] =
                transparent_phase && temporal_active
                    ? post->temporal_reactive_scale
                    : 0.0f;
            fragment_uniforms.procedural_controls[3] =
                temporal_lod_bias;
            (void)memcpy(
                fragment_uniforms.detail_parameters,
                batch->detail_parameters,
                sizeof(fragment_uniforms.detail_parameters)
            );
            fragment_uniforms.surface_wetness[0] =
                isfinite(lighting->surface_wetness)
                    ? fminf(fmaxf(lighting->surface_wetness, 0.0f), 1.0f)
                    : 0.0f;
            fragment_uniforms.surface_wetness[1] =
                isfinite(lighting->window_glow)
                    ? fmaxf(lighting->window_glow, 0.0f)
                    : 0.0f;
            fragment_uniforms.surface_wetness[2] =
                isfinite(lighting->rain)
                    ? fminf(fmaxf(lighting->rain, 0.0f), 1.0f)
                    : 0.0f;
            fragment_uniforms.cloud_controls[0] = lighting->cloud_offset[0];
            fragment_uniforms.cloud_controls[1] = lighting->cloud_offset[1];
            fragment_uniforms.cloud_controls[2] = lighting->cloud_frequency;
            fragment_uniforms.cloud_controls[3] = lighting->cloud_strength;
            fragment_uniforms.cloud_shape[0] = lighting->cloud_coverage;
            fragment_uniforms.cloud_shape[1] = lighting->cloud_softness;
            fragment_uniforms.cloud_shape[2] = lighting->cloud_height;
            {
                const uint32_t lamp_count =
                    lighting->lamp_count < BTBH_GPU_SCENE_MAX_LAMPS
                        ? lighting->lamp_count
                        : BTBH_GPU_SCENE_MAX_LAMPS;
                fragment_uniforms.lamp_controls[0] = (float)lamp_count;
                fragment_uniforms.lamp_controls[1] =
                    lighting->lamp_core_radius;
                if (lamp_count > 0u) {
                    (void)memcpy(
                        fragment_uniforms.lamp_position_reach,
                        lighting->lamp_position_reach,
                        sizeof(float) * 4u * lamp_count
                    );
                    (void)memcpy(
                        fragment_uniforms.lamp_color_energy,
                        lighting->lamp_color_energy,
                        sizeof(float) * 4u * lamp_count
                    );
                }
            }
            /* The dark scene's controls ride in the point lights' header. */
            fragment_uniforms.point_light_count[1] =
                lighting->unlit_darkness;
            fragment_uniforms.point_light_count[2] =
                lighting->unlit_glow_threshold;
            fragment_uniforms.point_light_count[3] =
                SDL_max(lighting->point_light_ceiling, 0.0f);
            const float *primary_shadow_matrix =
                runtime->retained_shadow_ready[0] != 0u
                    ? runtime->retained_shadow_view_projection[0]
                    : lighting->directional_shadow_view_projection;
            const float *secondary_shadow_matrix =
                runtime->retained_shadow_ready[1] != 0u
                    ? runtime->retained_shadow_view_projection[1]
                    : lighting
                        ->secondary_directional_shadow_view_projection;
            const float *spot_shadow_matrix =
                runtime->retained_shadow_ready[2] != 0u
                    ? runtime->retained_shadow_view_projection[2]
                    : lighting->spot_shadow_view_projection;
            (void)memcpy(
                fragment_uniforms
                    .directional_shadow_view_projection,
                primary_shadow_matrix,
                sizeof(
                    fragment_uniforms
                        .directional_shadow_view_projection
                )
            );
            (void)memcpy(
                fragment_uniforms
                    .secondary_directional_shadow_view_projection,
                secondary_shadow_matrix,
                sizeof(
                    fragment_uniforms
                        .secondary_directional_shadow_view_projection
                )
            );
            (void)memcpy(
                fragment_uniforms.spot_shadow_view_projection,
                spot_shadow_matrix,
                sizeof(fragment_uniforms.spot_shadow_view_projection)
            );
            fragment_uniforms.directional_shadow_controls[0] =
                (float)shadow_enabled[0];
            fragment_uniforms.directional_shadow_controls[1] =
                lighting->directional_shadow_softness;
            fragment_uniforms.directional_shadow_controls[2] =
                lighting->directional_shadow_filter ==
                        BTBH_GPU_SCENE_SHADOW_FILTER_SOFT
                    ? 1.0f
                    : 0.0f;
            fragment_uniforms
                .secondary_directional_shadow_controls[0] =
                (float)shadow_enabled[1];
            fragment_uniforms
                .secondary_directional_shadow_controls[1] =
                lighting->secondary_directional_shadow_softness;
            fragment_uniforms.spot_shadow_controls[0] =
                (float)shadow_enabled[2];
            fragment_uniforms.spot_shadow_controls[1] =
                lighting->spot_shadow_softness;
            fragment_uniforms.spot_shadow_controls[2] =
                (float)runtime->retained_spot_shadow_is_omni;
            fragment_uniforms.spot_shadow_controls[3] =
                lighting->spot_shadow_near;
            if (runtime->retained_shadow_ready[2] != 0u &&
                runtime->retained_spot_shadow_is_omni != 0u) {
                (void)memcpy(
                    fragment_uniforms.omni_shadow_position_range,
                    runtime->retained_omni_shadow_position_range,
                    sizeof(
                        fragment_uniforms.omni_shadow_position_range
                    )
                );
            } else {
                fragment_uniforms.omni_shadow_position_range[0] =
                    lighting->spot_position_x;
                fragment_uniforms.omni_shadow_position_range[1] =
                    lighting->spot_position_y;
                fragment_uniforms.omni_shadow_position_range[2] =
                    lighting->spot_position_z;
                fragment_uniforms.omni_shadow_position_range[3] =
                    lighting->spot_range;
            }
            SDL_PushGPUFragmentUniformData(
                command,
                0u,
                &fragment_uniforms,
                shadows_active
                    ? (uint32_t)sizeof(fragment_uniforms)
                    : (uint32_t)offsetof(
                        BTBHGPUSceneFragmentUniforms,
                        directional_shadow_view_projection
                    )
            );
            BTBHGPUSceneStencilVariant stencil_variant;
            if (!btbh_gpu_scene_stencil_variant(
                    batch,
                    &stencil_variant
                )) {
                SDL_EndGPURenderPass(pass);
                btbh_gpu_scene_cancel_submission(window, command);
                free(bone_offsets);
                btbh_gpu_scene_error(
                    report,
                    BTBH_GPU_INVALID_ARGUMENT,
                    "Unsupported GPU scene stencil material state"
                );
                return report->result;
            }
            SDL_SetGPUStencilReference(
                pass,
                batch->stencil_reference
            );
            SDL_GPUGraphicsPipeline *model_pipeline =
                *btbh_gpu_scene_pipeline_slot(
                    window->scene_runtime,
                    phase_auxiliary_geometry_outputs,
                    temporal_active,
                    radiance_cascades_active,
                    shadows_active,
                    omni_shadows_active,
                    animated,
                    batch,
                    stencil_variant
                );
            if (model_pipeline != bound_model_pipeline) {
                SDL_BindGPUGraphicsPipeline(pass, model_pipeline);
                bound_model_pipeline = model_pipeline;
                if (detailed_prepare_telemetry) {
                    telemetry->geometry_pipeline_bind_count += 1u;
                }
            } else if (detailed_prepare_telemetry) {
                telemetry->geometry_pipeline_bind_skip_count += 1u;
            }
            /* The motion slot takes the instance buffer when the draw has no motion; the shader
               then never reads it. */
            SDL_GPUBuffer *batch_motion_buffer = batch_motion
                ? motion_buffer
                : batch_instance_buffer;
            if (animated) {
                SDL_GPUBuffer *storage_buffers[3] = {
                    batch_instance_buffer,
                    bone_buffer,
                    batch_motion_buffer
                };
                SDL_BindGPUVertexStorageBuffers(
                    pass,
                    0u,
                    storage_buffers,
                    3u
                );
            } else {
                SDL_GPUBuffer *storage_buffers[2] = {
                    batch_instance_buffer,
                    batch_motion_buffer
                };
                SDL_BindGPUVertexStorageBuffers(
                    pass,
                    0u,
                    storage_buffers,
                    2u
                );
            }
            BTBHGPUTexture *texture = batch->albedo_texture != NULL
                ? batch->albedo_texture
                : window->scene_runtime->fallback_texture;
            BTBHGPUTexture *emission_texture =
                batch->emission_texture != NULL
                    ? batch->emission_texture
                    : window->scene_runtime->fallback_texture;
            BTBHGPUTexture *normal_texture =
                batch->normal_texture != NULL
                    ? batch->normal_texture
                    : window->scene_runtime->normal_fallback_texture;
            BTBHGPUTexture *orm_texture =
                batch->orm_texture != NULL
                    ? batch->orm_texture
                    : window->scene_runtime->fallback_texture;
            SDL_GPUTextureSamplerBinding samplers[10];
            SDL_zeroa(samplers);
            samplers[0].texture = texture->texture;
            samplers[0].sampler = texture->sampler;
            samplers[1].texture = emission_texture->texture;
            samplers[1].sampler = emission_texture->sampler;
            samplers[2].texture = normal_texture->texture;
            samplers[2].sampler = normal_texture->sampler;
            if (transparent_phase &&
                scene_distance_ready &&
                batch->surface_variant ==
                    BTBH_GPU_SCENE_SURFACE_SOFT_PARTICLE) {
                /* A soft particle has no normal map; the slot carries the scene's distance. */
                samplers[2].texture = runtime->scene_distance_texture;
                samplers[2].sampler = runtime->scene_color_sampler;
            }
            if (batch_refracts) {
                /* Refraction samples no material: the albedo slot carries the scene copy, the normal
                   slot the scene's distance. */
                samplers[0].texture = runtime->refraction_texture;
                samplers[0].sampler = runtime->scene_color_sampler;
                if (scene_distance_ready) {
                    samplers[2].texture = runtime->scene_distance_texture;
                    samplers[2].sampler = runtime->scene_color_sampler;
                }
            }
            samplers[3].texture = orm_texture->texture;
            samplers[3].sampler = orm_texture->sampler;
            samplers[4].texture = runtime->brdf_lut_texture;
            samplers[4].sampler = runtime->brdf_lut_sampler;
            samplers[5].texture =
                runtime->directional_shadow_textures[0];
            samplers[5].sampler =
                runtime->directional_shadow_sampler;
            samplers[6].texture =
                runtime->directional_shadow_textures[1];
            samplers[6].sampler =
                runtime->directional_shadow_sampler;
            samplers[7].texture =
                runtime->directional_shadow_textures[2];
            samplers[7].sampler =
                runtime->directional_shadow_sampler;
            /* The primary map again, through the comparison sampler (the soft sun shadow filter's taps). */
            samplers[8].texture =
                runtime->directional_shadow_textures[0];
            samplers[8].sampler =
                runtime->directional_shadow_compare_sampler;
            if (omni_shadows_active) {
                samplers[9].texture = runtime->omni_shadow_texture;
                samplers[9].sampler =
                    runtime->directional_shadow_sampler;
            }
            SDL_BindGPUFragmentSamplers(
                pass,
                0u,
                samplers,
                (!radiance_cascades_active &&
                 btbh_gpu_scene_uses_fast_unlit_pipeline(batch))
                    ? 2u
                    : (shadows_active
                        ? (omni_shadows_active ? 10u : 9u)
                        : 5u)
            );
            SDL_GPUBuffer *vertex_buffer = NULL;
            SDL_GPUBuffer *index_buffer = NULL;
            uint32_t vertex_count = 0u;
            uint32_t index_count = 0u;
            uint32_t first_index = 0u;
            int32_t vertex_base = 0;
            if (batch->kind == BTBH_GPU_SCENE_GEOMETRY_MODEL) {
                const BTBHGPUModelPrimitive *primitive =
                    &batch->model->primitives[geometry_index];
                vertex_buffer = primitive->vertex_buffer;
                index_buffer = primitive->index_buffer;
                vertex_count = primitive->vertex_count;
                index_count = primitive->index_count;
                first_index = primitive->first_index;
                vertex_base = primitive->vertex_base;
            } else {
                vertex_buffer = batch->custom_mesh->vertex_buffer;
                index_buffer = batch->custom_mesh->index_buffer;
                vertex_count = batch->custom_mesh->vertex_count;
                index_count = batch->custom_mesh->index_count;
            }
            SDL_GPUBufferBinding vertex_binding;
            SDL_zero(vertex_binding);
            vertex_binding.buffer = vertex_buffer;
            SDL_BindGPUVertexBuffers(
                pass,
                0u,
                &vertex_binding,
                1u
            );
            if (btbh_gpu_cost_skips_model(
                    batch->kind == BTBH_GPU_SCENE_GEOMETRY_MODEL
                        ? batch->model
                        : NULL
                )) {
                /* BTBH_GPU_COST_SKIP_MODEL: not drawn (measurement). */
            } else if (index_buffer != NULL && index_count > 0u) {
                /* SDL tracks every index bind with a scan of the buffers the command buffer used so far, and the
                   binding outlives pipeline changes within a pass: consecutive draws of one geometry (a model's
                   instance ranges, an enemy's batches) bind it once (2026-10-17). */
                if (index_buffer != bound_index_buffer) {
                    SDL_GPUBufferBinding index_binding;
                    SDL_zero(index_binding);
                    index_binding.buffer = index_buffer;
                    SDL_BindGPUIndexBuffer(
                        pass,
                        &index_binding,
                        SDL_GPU_INDEXELEMENTSIZE_32BIT
                    );
                    bound_index_buffer = index_buffer;
                }
                SDL_DrawGPUIndexedPrimitives(
                    pass,
                    index_count,
                    batch->instance_count,
                    first_index,
                    vertex_base,
                    batch->first_instance
                );
            } else {
                SDL_DrawGPUPrimitives(
                    pass,
                    vertex_count,
                    batch->instance_count,
                    (uint32_t) vertex_base,
                    batch->first_instance
                );
            }
            btbh_gpu_cost_record_draw(
                BTBH_GPU_COST_PASS_MAIN,
                index_buffer != NULL && index_count > 0u
                    ? index_count
                    : vertex_count,
                batch->instance_count,
                batch->instance_buffer != NULL,
                batch->kind == BTBH_GPU_SCENE_GEOMETRY_MODEL
                    ? batch->model
                    : NULL,
                batch->kind == BTBH_GPU_SCENE_GEOMETRY_MODEL
                    ? NULL
                    : batch->custom_mesh
            );
            primitive_draw_count += 1u;
        }
        }
        if (geometry_cursor != prepared_geometry_count) {
            SDL_EndGPURenderPass(pass);
            btbh_gpu_scene_cancel_submission(window, command);
            free(bone_offsets);
            btbh_gpu_scene_error(
                report,
                BTBH_GPU_DEVICE_FAILURE,
                "GPU scene geometry preparation changed during submission"
            );
            return report->result;
        }
        SDL_EndGPURenderPass(pass);
        geometry_encode_nanoseconds +=
            SDL_GetTicksNS() - geometry_phase_started_ticks;
        if (!btbh_gpu_scene_cost_split(
                window,
                &command,
                transparent_phase
                    ? BTBH_GPU_COST_GROUP_TRANSPARENT
                    : BTBH_GPU_COST_GROUP_MAIN,
                report
            )) {
            free(bone_offsets);
            return report->result;
        }

        if (!transparent_phase &&
            (radiance_cascades_active ||
             radiance_cascades_fallback_debug)) {
            const uint64_t radiance_cascades_started_ticks =
                SDL_GetTicksNS();
            SDL_GPUTexture *composed_texture = NULL;
            bool radiance_cascades_encoded = false;
            if (radiance_cascades_active) {
                const bool source_field_encoded =
                    btbh_gpu_radiance_cascades_runtime_encode_emitters(
                        command,
                        &runtime->radiance_cascades,
                        &post->radiance_cascades
                    ) &&
                    btbh_gpu_scene_encode_rc_material_sources(
                        command,
                        runtime,
                        &post->radiance_cascades,
                        camera_right,
                        camera_up,
                        batches,
                        batch_count,
                        bone_offsets,
                        prepared_geometry_count,
                        transient_instance_buffer,
                        bone_buffer
                    );
                btbh_gpu_cost_rc_emitters =
                    runtime->radiance_cascades.emitter_count;
                if (!btbh_gpu_scene_cost_split(
                        window,
                        &command,
                        BTBH_GPU_COST_GROUP_RC_SOURCE,
                        report
                    )) {
                    free(bone_offsets);
                    return report->result;
                }
                radiance_cascades_encoded =
                    source_field_encoded &&
                    btbh_gpu_radiance_cascades_runtime_encode(
                    &command,
                    &runtime->radiance_cascades,
                    &post->radiance_cascades,
                    lighting,
                    inverse_view_projection,
                    runtime->scene_color_texture,
                    runtime->ambient_texture,
                    runtime->ambient_specular_texture,
                    runtime->emission_texture,
                    runtime->normal_texture,
                    runtime->depth_texture,
                    hero_omni_shadow_ready
                        ? runtime->omni_shadow_texture
                        : NULL,
                    runtime->retained_omni_shadow_position_range,
                    lighting->spot_shadow_near,
                    lighting->spot_shadow_softness,
                    &composed_texture
                    );
            } else {
                radiance_cascades_encoded =
                    btbh_gpu_radiance_cascades_runtime_encode_fallback_debug(
                    command,
                    &runtime->radiance_cascades,
                    runtime->scene_color_texture,
                    &composed_texture
                    );
            }
            if (command == NULL) {
                /* The cost log's split inside the cascades failed. */
                btbh_gpu_scene_error(
                    report,
                    BTBH_GPU_DEVICE_FAILURE,
                    SDL_GetError()
                );
                btbh_gpu_scene_cancel_submission(window, NULL);
                free(bone_offsets);
                return report->result;
            }
            if (radiance_cascades_encoded) {
                radiance_cascades_post_input = composed_texture;
            } else if (radiance_cascades_active) {
                btbh_gpu_radiance_cascades_runtime_invalidate_history(
                    &runtime->radiance_cascades
                );
                btbh_gpu_radiance_cascades_mark_failure(
                    &runtime->radiance_cascades.state,
                    BTBH_GPU_SCENE_RC_FALLBACK_DISPATCH_FAILED
                );
                if (!runtime->radiance_cascades.logged_failure) {
                    SDL_LogWarn(
                        SDL_LOG_CATEGORY_RENDER,
                        "Radiance Cascades disabled after dispatch failure: %s",
                        SDL_GetError()
                    );
                    runtime->radiance_cascades.logged_failure = true;
                }
            }
            radiance_cascades_encode_milliseconds =
                (double)(
                    SDL_GetTicksNS() - radiance_cascades_started_ticks
                ) / 1000000.0;
            if (!btbh_gpu_scene_cost_split(
                    window,
                    &command,
                    BTBH_GPU_COST_GROUP_RC_SCREEN,
                    report
                )) {
                free(bone_offsets);
                return report->result;
            }
        }
    }
    report->cpu_geometry_encode_milliseconds =
        (double)geometry_encode_nanoseconds / 1000000.0;
    btbh_gpu_radiance_cascades_status(
        &runtime->radiance_cascades.state,
        runtime->radiance_cascades.internal_width,
        runtime->radiance_cascades.internal_height,
        radiance_cascades_encode_milliseconds,
        &report->radiance_cascades
    );
    report->radiance_cascades.pass_telemetry =
        runtime->radiance_cascades.pass_telemetry;

    const uint64_t post_started_ticks = SDL_GetTicksNS();
    SDL_GPUTexture *post_input = radiance_cascades_post_input;
    bool fullscreen_ok = true;
    if (fullscreen_ok && hdr_composite_active) {
        SDL_GPUTexture *bloom_texture = NULL;
        if (bloom_active) {
            /* Downsample from the HDR image; level 0 also thresholds. The
             * image is not exposed yet, so the threshold (in exposed units)
             * is divided by the exposure the grading stage applies later. */
            const float bloom_threshold =
                post->bloom_threshold / fmaxf(post->exposure, 1.0e-4f);
            BTBHGPUSceneBloomUniforms bloom_uniforms;
            SDL_GPUTexture *source = post_input;
            uint32_t source_width = render_width;
            uint32_t source_height = render_height;
            for (uint32_t level = 0u;
                 fullscreen_ok && level < BTBH_GPU_SCENE_BLOOM_LEVELS;
                 ++level) {
                (void)memset(&bloom_uniforms, 0, sizeof(bloom_uniforms));
                bloom_uniforms.source_texel_threshold[0] = 1.0f / (float)source_width;
                bloom_uniforms.source_texel_threshold[1] = 1.0f / (float)source_height;
                bloom_uniforms.source_texel_threshold[2] = bloom_threshold;
                bloom_uniforms.source_texel_threshold[3] = post->bloom_soft_knee;
                bloom_uniforms.pass_parameters[0] = level == 0u ? 1.0f : 0.0f;
                SDL_GPUTextureSamplerBinding source_sampler;
                SDL_zero(source_sampler);
                source_sampler.texture = source;
                source_sampler.sampler = runtime->scene_color_sampler;
                fullscreen_ok = btbh_gpu_scene_render_fullscreen(
                    command,
                    "BTBH Bloom Downsample",
                    runtime->bloom_downsample_pipeline,
                    runtime->bloom_down_textures[level],
                    &bloom_uniforms,
                    (uint32_t)sizeof(bloom_uniforms),
                    &source_sampler,
                    1u
                );
                source = runtime->bloom_down_textures[level];
                source_width = runtime->bloom_level_width[level];
                source_height = runtime->bloom_level_height[level];
            }
            /* Upsample back to level 0, blending each wider blur into the
             * finer level by the scatter amount. */
            uint32_t lower_level = BTBH_GPU_SCENE_BLOOM_LEVELS - 1u;
            SDL_GPUTexture *lower = runtime->bloom_down_textures[lower_level];
            for (uint32_t step = BTBH_GPU_SCENE_BLOOM_LEVELS - 1u;
                 fullscreen_ok && step > 0u;
                 --step) {
                const uint32_t level = step - 1u;
                (void)memset(&bloom_uniforms, 0, sizeof(bloom_uniforms));
                bloom_uniforms.source_texel_threshold[0] =
                    1.0f / (float)runtime->bloom_level_width[lower_level];
                bloom_uniforms.source_texel_threshold[1] =
                    1.0f / (float)runtime->bloom_level_height[lower_level];
                bloom_uniforms.pass_parameters[1] = post->bloom_scatter;
                SDL_GPUTextureSamplerBinding upsample_samplers[2];
                SDL_zeroa(upsample_samplers);
                upsample_samplers[0].texture = lower;
                upsample_samplers[0].sampler = runtime->scene_color_sampler;
                upsample_samplers[1].texture = runtime->bloom_down_textures[level];
                upsample_samplers[1].sampler = runtime->scene_color_sampler;
                fullscreen_ok = btbh_gpu_scene_render_fullscreen(
                    command,
                    "BTBH Bloom Upsample",
                    runtime->bloom_upsample_pipeline,
                    runtime->bloom_up_textures[level],
                    &bloom_uniforms,
                    (uint32_t)sizeof(bloom_uniforms),
                    upsample_samplers,
                    2u
                );
                lower = runtime->bloom_up_textures[level];
                lower_level = level;
            }
            bloom_texture = lower;
        }
        BTBHGPUSceneCompositeUniforms composite_uniforms;
        (void)memset(&composite_uniforms, 0, sizeof(composite_uniforms));
        (void)memcpy(
            composite_uniforms.inverse_view_projection,
            inverse_view_projection,
            sizeof(composite_uniforms.inverse_view_projection)
        );
        composite_uniforms.fog_color_density[0] = post->fog_color[0];
        composite_uniforms.fog_color_density[1] = post->fog_color[1];
        composite_uniforms.fog_color_density[2] = post->fog_color[2];
        composite_uniforms.fog_color_density[3] = post->fog_density;
        composite_uniforms.fog_shape[0] = post->fog_start_distance;
        composite_uniforms.fog_shape[1] = post->fog_maximum_opacity;
        composite_uniforms.fog_shape[2] = post->fog_enabled != 0u ? 1.0f : 0.0f;
        for (uint32_t edge = 0u; edge < 4u; ++edge) {
            composite_uniforms.fog_edge_bounds[edge] = post->fog_edge_bounds[edge];
        }
        composite_uniforms.fog_edge[0] = post->fog_edge_fade_distance;
        composite_uniforms.fog_edge[2] = post->fog_edge_clear_distance;
        composite_uniforms.fog_edge[1] =
            post->fog_enabled != 0u && post->fog_edge_enabled != 0u ? 1.0f : 0.0f;
        composite_uniforms.bloom_parameters[0] = post->bloom_intensity;
        composite_uniforms.bloom_parameters[1] = bloom_texture != NULL ? 1.0f : 0.0f;
        composite_uniforms.vignette_parameters[0] = post->vignette_intensity;
        composite_uniforms.vignette_parameters[1] = post->vignette_smoothness;
        composite_uniforms.vignette_parameters[2] =
            (float)render_width / (float)render_height;
        composite_uniforms.vignette_parameters[3] =
            post->vignette_enabled != 0u ? 1.0f : 0.0f;
        SDL_GPUTextureSamplerBinding composite_samplers[3];
        SDL_zeroa(composite_samplers);
        composite_samplers[0].texture = post_input;
        composite_samplers[0].sampler = runtime->scene_color_sampler;
        composite_samplers[1].texture = runtime->depth_texture;
        composite_samplers[1].sampler = runtime->scene_color_sampler;
        composite_samplers[2].texture = bloom_texture != NULL
            ? bloom_texture
            : runtime->fallback_texture->texture;
        composite_samplers[2].sampler = runtime->scene_color_sampler;
        /* The LUT stage always writes post_ping, so the composite uses pong. */
        if (fullscreen_ok) {
            fullscreen_ok = btbh_gpu_scene_render_fullscreen(
                command,
                "BTBH HDR Composite",
                runtime->hdr_composite_pipeline,
                runtime->post_pong_texture,
                &composite_uniforms,
                (uint32_t)sizeof(composite_uniforms),
                composite_samplers,
                3u
            );
        }
        post_input = runtime->post_pong_texture;
    }
    SDL_GPUTexture *post_stage_input = post_input;
    if (fullscreen_ok && post->lut_enabled != 0u) {
        SDL_GPUTextureSamplerBinding lut_samplers[2];
        SDL_zeroa(lut_samplers);
        lut_samplers[0].texture = post_stage_input;
        lut_samplers[0].sampler = runtime->scene_color_sampler;
        lut_samplers[1].texture = post->lut_texture->texture;
        lut_samplers[1].sampler = post->lut_texture->sampler;
        fullscreen_ok = btbh_gpu_scene_render_fullscreen(
            command,
            "BTBH Post LUT",
            runtime->post_lut_pipeline,
            runtime->post_ping_texture,
            &post_uniforms,
            (uint32_t)sizeof(post_uniforms),
            lut_samplers,
            2u
        );
        post_stage_input = runtime->post_ping_texture;
    }
    if (fullscreen_ok && post->cas_enabled != 0u) {
        SDL_GPUTexture *cas_target =
            post_stage_input == runtime->post_ping_texture
                ? runtime->post_pong_texture
                : runtime->post_ping_texture;
        SDL_GPUTextureSamplerBinding cas_sampler;
        SDL_zero(cas_sampler);
        cas_sampler.texture = post_stage_input;
        cas_sampler.sampler = runtime->scene_color_sampler;
        fullscreen_ok = btbh_gpu_scene_render_fullscreen(
            command,
            "BTBH Post CAS",
            runtime->post_cas_pipeline,
            cas_target,
            &post_uniforms,
            (uint32_t)sizeof(post_uniforms),
            &cas_sampler,
            1u
        );
        post_stage_input = cas_target;
    }
    bool production_output_rendered = false;
    if (fullscreen_ok && post->tilt_shift_enabled != 0u) {
        SDL_GPUTexture *tilt_shift_target =
            anti_aliasing != 0u
                ? runtime->post_color_texture
                : runtime->final_color_texture;
        SDL_GPUTextureSamplerBinding tilt_shift_sampler;
        SDL_zero(tilt_shift_sampler);
        tilt_shift_sampler.texture = post_stage_input;
        tilt_shift_sampler.sampler = runtime->scene_color_sampler;
        fullscreen_ok = btbh_gpu_scene_render_fullscreen(
            command,
            "BTBH Post Tilt Shift + Output",
            runtime->post_tilt_shift_pipeline,
            tilt_shift_target,
            &post_uniforms,
            (uint32_t)sizeof(post_uniforms),
            &tilt_shift_sampler,
            1u
        );
        production_output_rendered = fullscreen_ok;
    }

    SDL_GPUTextureSamplerBinding post_sampler;
    SDL_zero(post_sampler);
    post_sampler.texture = post_stage_input;
    post_sampler.sampler = runtime->scene_color_sampler;
    SDL_GPUTexture *post_target =
        anti_aliasing != 0u
            ? runtime->post_color_texture
            : runtime->final_color_texture;
    if (fullscreen_ok && !production_output_rendered) {
        fullscreen_ok = btbh_gpu_scene_render_fullscreen(
            command,
            "BTBH Production Output",
            runtime->post_pipeline,
            post_target,
            &post_uniforms,
            (uint32_t)sizeof(post_uniforms),
            &post_sampler,
            1u
        );
    }
    if (fullscreen_ok &&
        anti_aliasing == BTBH_GPU_SCENE_ANTI_ALIASING_FXAA) {
        BTBHGPUSceneSMAAUniforms fxaa_uniforms;
        fxaa_uniforms.metrics[0] = 1.0f / (float)render_width;
        fxaa_uniforms.metrics[1] = 1.0f / (float)render_height;
        fxaa_uniforms.metrics[2] = (float)render_width;
        fxaa_uniforms.metrics[3] = (float)render_height;
        SDL_GPUTextureSamplerBinding fxaa_sampler;
        SDL_zero(fxaa_sampler);
        fxaa_sampler.texture = runtime->post_color_texture;
        fxaa_sampler.sampler = runtime->scene_color_sampler;
        fullscreen_ok = btbh_gpu_scene_render_fullscreen(
            command,
            "BTBH FXAA",
            runtime->fxaa_pipeline,
            runtime->final_color_texture,
            &fxaa_uniforms,
            (uint32_t)sizeof(fxaa_uniforms),
            &fxaa_sampler,
            1u
        );
    }
    if (fullscreen_ok &&
        anti_aliasing >= 1u &&
        anti_aliasing <= BTBH_GPU_SCENE_SMAA_QUALITY_COUNT) {
        BTBHGPUSceneSMAAUniforms smaa_uniforms;
        smaa_uniforms.metrics[0] = 1.0f / (float)render_width;
        smaa_uniforms.metrics[1] = 1.0f / (float)render_height;
        smaa_uniforms.metrics[2] = (float)render_width;
        smaa_uniforms.metrics[3] = (float)render_height;
        const uint32_t quality_index =
            (uint32_t)anti_aliasing - 1u;

        SDL_GPUTextureSamplerBinding edge_sampler;
        SDL_zero(edge_sampler);
        edge_sampler.texture = runtime->post_color_texture;
        edge_sampler.sampler = runtime->scene_color_sampler;
        fullscreen_ok = btbh_gpu_scene_render_fullscreen(
            command,
            "BTBH SMAA Edge",
            runtime->smaa_edge_pipelines[quality_index],
            runtime->smaa_edges_texture,
            &smaa_uniforms,
            (uint32_t)sizeof(smaa_uniforms),
            &edge_sampler,
            1u
        );

        SDL_GPUTextureSamplerBinding weight_samplers[3];
        SDL_zeroa(weight_samplers);
        weight_samplers[0].texture = runtime->smaa_edges_texture;
        weight_samplers[0].sampler = runtime->scene_color_sampler;
        weight_samplers[1].texture =
            runtime->smaa_area_texture->texture;
        weight_samplers[1].sampler =
            runtime->smaa_area_texture->sampler;
        weight_samplers[2].texture =
            runtime->smaa_search_texture->texture;
        weight_samplers[2].sampler =
            runtime->smaa_search_texture->sampler;
        if (fullscreen_ok) {
            fullscreen_ok = btbh_gpu_scene_render_fullscreen(
                command,
                "BTBH SMAA Weights",
                runtime->smaa_weight_pipelines[quality_index],
                runtime->smaa_weights_texture,
                &smaa_uniforms,
                (uint32_t)sizeof(smaa_uniforms),
                weight_samplers,
                3u
            );
        }

        SDL_GPUTextureSamplerBinding blend_samplers[2];
        SDL_zeroa(blend_samplers);
        blend_samplers[0].texture = runtime->post_color_texture;
        blend_samplers[0].sampler = runtime->scene_color_sampler;
        blend_samplers[1].texture = runtime->smaa_weights_texture;
        blend_samplers[1].sampler = runtime->scene_color_sampler;
        if (fullscreen_ok) {
            fullscreen_ok = btbh_gpu_scene_render_fullscreen(
                command,
                "BTBH SMAA Blend",
                runtime->smaa_blend_pipeline,
                runtime->final_color_texture,
                &smaa_uniforms,
                (uint32_t)sizeof(smaa_uniforms),
                blend_samplers,
                2u
            );
        }
    }
    runtime->composite_color_texture =
        runtime->final_color_texture;
    runtime->composite_width = render_width;
    runtime->composite_height = render_height;
    if (fullscreen_ok &&
        post->mirror_horizontal != 0u &&
        !temporal_active) {
        /* A same-size nearest blit mirrors pixel x to width - 1 - x
         * exactly. Anti-aliasing has already consumed
         * post_color_texture, so it holds the mirrored image, and the
         * capture, upscale and composite all read the mirrored frame. */
        SDL_GPUBlitInfo mirror_blit;
        SDL_zero(mirror_blit);
        mirror_blit.source.texture = runtime->final_color_texture;
        mirror_blit.source.w = render_width;
        mirror_blit.source.h = render_height;
        mirror_blit.destination.texture = runtime->post_color_texture;
        mirror_blit.destination.w = render_width;
        mirror_blit.destination.h = render_height;
        mirror_blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
        mirror_blit.flip_mode = SDL_FLIP_HORIZONTAL;
        mirror_blit.filter = SDL_GPU_FILTER_NEAREST;
        SDL_InsertGPUDebugLabel(command, "BTBH Scene Mirror");
        SDL_BlitGPUTexture(command, &mirror_blit);
        runtime->composite_color_texture =
            runtime->post_color_texture;
    }
    if (fullscreen_ok && temporal_active) {
        /* FSR 3.1 accumulates the jittered frames at the output size, the FSR 1 RCAS pass sharpens
           the result into the swapchain format, and Mirror Curse mirrors it last (the history and
           motion vectors stay unmirrored). */
        BTBHGPUFSR3Inputs fsr3_inputs;
        SDL_zero(fsr3_inputs);
        fsr3_inputs.color = runtime->final_color_texture;
        fsr3_inputs.depth = runtime->depth_texture;
        fsr3_inputs.motion_vectors = runtime->velocity_texture;
        fsr3_inputs.reactive = runtime->reactive_texture;
        fsr3_inputs.render_width = render_width;
        fsr3_inputs.render_height = render_height;
        fsr3_inputs.output_width = drawable_width;
        fsr3_inputs.output_height = drawable_height;
        fsr3_inputs.camera_near = post->temporal_camera_near;
        fsr3_inputs.camera_far = post->temporal_camera_far;
        fsr3_inputs.vertical_fov_radians = post->temporal_vertical_fov;
        fsr3_inputs.frame_seconds = post->temporal_frame_seconds;
        fsr3_inputs.view_space_to_meters = post->temporal_meters_per_unit;
        fsr3_inputs.debug_motion_pixels = 8.0f;
        fsr3_inputs.debug_view = runtime->temporal_debug_view;
        const bool motion_error_view =
            runtime->temporal_debug_view == BTBH_GPU_FSR3_DEBUG_MOTION_ERROR;
        if (motion_error_view &&
            runtime->temporal_previous_color != NULL &&
            runtime->temporal_previous_color_width == render_width &&
            runtime->temporal_previous_color_height == render_height) {
            fsr3_inputs.previous_color = runtime->temporal_previous_color;
        }
        const bool fsr3_encoded =
            drawable_width == runtime->fsr3.output_width &&
            drawable_height == runtime->fsr3.output_height &&
            btbh_gpu_fsr3_encode(command, &runtime->fsr3, &fsr3_inputs);
        if (!fsr3_encoded) {
            /* The frame goes out at the render size (the canvas scales it) and later frames use
               FSR 1, rather than the scene failing. */
            SDL_LogWarn(
                SDL_LOG_CATEGORY_RENDER,
                "FSR 3 upscaling failed, using FSR 1 from now on: %s",
                SDL_GetError()
            );
            runtime->fsr3.unavailable = true;
            runtime->fsr3.logged_failure = true;
        }
        if (fsr3_encoded &&
            runtime->temporal_debug_view == BTBH_GPU_FSR3_DEBUG_NONE) {
            BTBHGPUSceneSMAAUniforms rcas_uniforms;
            rcas_uniforms.metrics[0] = 1.0f / (float)drawable_width;
            rcas_uniforms.metrics[1] = 1.0f / (float)drawable_height;
            rcas_uniforms.metrics[2] = (float)drawable_width;
            rcas_uniforms.metrics[3] = (float)drawable_height;
            SDL_GPUTextureSamplerBinding rcas_sampler;
            SDL_zero(rcas_sampler);
            rcas_sampler.texture = runtime->fsr3.output;
            rcas_sampler.sampler = runtime->scene_color_sampler;
            fullscreen_ok = btbh_gpu_scene_render_fullscreen(
                command,
                "BTBH FSR3 RCAS",
                runtime->upscale_fsr_rcas_pipeline,
                runtime->sharpened_color_texture,
                &rcas_uniforms,
                (uint32_t)sizeof(rcas_uniforms),
                &rcas_sampler,
                1u
            );
        } else if (fsr3_encoded && fullscreen_ok) {
            SDL_GPUBlitInfo debug_blit;
            SDL_zero(debug_blit);
            debug_blit.source.texture = runtime->fsr3.output;
            debug_blit.source.w = drawable_width;
            debug_blit.source.h = drawable_height;
            debug_blit.destination.texture = runtime->sharpened_color_texture;
            debug_blit.destination.w = drawable_width;
            debug_blit.destination.h = drawable_height;
            debug_blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
            debug_blit.filter = SDL_GPU_FILTER_NEAREST;
            SDL_BlitGPUTexture(command, &debug_blit);
        }
        if (fsr3_encoded && fullscreen_ok && motion_error_view) {
            if (runtime->temporal_previous_color == NULL ||
                runtime->temporal_previous_color_width != render_width ||
                runtime->temporal_previous_color_height != render_height) {
                if (runtime->temporal_previous_color != NULL) {
                    SDL_ReleaseGPUTexture(device, runtime->temporal_previous_color);
                }
                runtime->temporal_previous_color = btbh_gpu_scene_create_smaa_target(
                    device,
                    render_width,
                    render_height,
                    SDL_GetGPUSwapchainTextureFormat(device, window->window)
                );
                runtime->temporal_previous_color_width = render_width;
                runtime->temporal_previous_color_height = render_height;
            }
            SDL_GPUCopyPass *previous_copy = runtime->temporal_previous_color != NULL
                ? SDL_BeginGPUCopyPass(command)
                : NULL;
            if (previous_copy != NULL) {
                SDL_GPUTextureLocation copy_source;
                SDL_GPUTextureLocation copy_target;
                SDL_zero(copy_source);
                SDL_zero(copy_target);
                copy_source.texture = runtime->final_color_texture;
                copy_target.texture = runtime->temporal_previous_color;
                SDL_CopyGPUTextureToTexture(
                    previous_copy,
                    &copy_source,
                    &copy_target,
                    render_width,
                    render_height,
                    1u,
                    false
                );
                SDL_EndGPUCopyPass(previous_copy);
            }
        }
        if (fsr3_encoded && fullscreen_ok) {
            runtime->composite_color_texture =
                runtime->sharpened_color_texture;
            runtime->composite_width = drawable_width;
            runtime->composite_height = drawable_height;
        }
        if (fsr3_encoded && fullscreen_ok && post->mirror_horizontal != 0u) {
            SDL_GPUBlitInfo mirror_blit;
            SDL_zero(mirror_blit);
            mirror_blit.source.texture = runtime->sharpened_color_texture;
            mirror_blit.source.w = drawable_width;
            mirror_blit.source.h = drawable_height;
            mirror_blit.destination.texture = runtime->upscaled_color_texture;
            mirror_blit.destination.w = drawable_width;
            mirror_blit.destination.h = drawable_height;
            mirror_blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
            mirror_blit.flip_mode = SDL_FLIP_HORIZONTAL;
            mirror_blit.filter = SDL_GPU_FILTER_NEAREST;
            SDL_InsertGPUDebugLabel(command, "BTBH Scene Mirror");
            SDL_BlitGPUTexture(command, &mirror_blit);
            runtime->composite_color_texture =
                runtime->upscaled_color_texture;
        }
    }
    if (fullscreen_ok &&
        upscale_mode == BTBH_GPU_SCENE_UPSCALE_FSR1 &&
        (render_width != drawable_width ||
         render_height != drawable_height)) {
        BTBHGPUSceneSMAAUniforms easu_uniforms;
        easu_uniforms.metrics[0] = 1.0f / (float)render_width;
        easu_uniforms.metrics[1] = 1.0f / (float)render_height;
        easu_uniforms.metrics[2] = (float)render_width;
        easu_uniforms.metrics[3] = (float)render_height;
        SDL_GPUTextureSamplerBinding easu_sampler;
        SDL_zero(easu_sampler);
        easu_sampler.texture = runtime->composite_color_texture;
        easu_sampler.sampler = runtime->scene_color_sampler;
        fullscreen_ok = btbh_gpu_scene_render_fullscreen(
            command,
            "BTBH FSR1 EASU",
            runtime->upscale_fsr_easu_pipeline,
            runtime->upscaled_color_texture,
            &easu_uniforms,
            (uint32_t)sizeof(easu_uniforms),
            &easu_sampler,
            1u
        );
        BTBHGPUSceneSMAAUniforms rcas_uniforms;
        rcas_uniforms.metrics[0] = 1.0f / (float)drawable_width;
        rcas_uniforms.metrics[1] = 1.0f / (float)drawable_height;
        rcas_uniforms.metrics[2] = (float)drawable_width;
        rcas_uniforms.metrics[3] = (float)drawable_height;
        SDL_GPUTextureSamplerBinding rcas_sampler;
        SDL_zero(rcas_sampler);
        rcas_sampler.texture = runtime->upscaled_color_texture;
        rcas_sampler.sampler = runtime->scene_color_sampler;
        fullscreen_ok = fullscreen_ok && btbh_gpu_scene_render_fullscreen(
            command,
            "BTBH FSR1 RCAS",
            runtime->upscale_fsr_rcas_pipeline,
            runtime->sharpened_color_texture,
            &rcas_uniforms,
            (uint32_t)sizeof(rcas_uniforms),
            &rcas_sampler,
            1u
        );
        if (fullscreen_ok) {
            runtime->composite_color_texture =
                runtime->sharpened_color_texture;
            runtime->composite_width = drawable_width;
            runtime->composite_height = drawable_height;
        }
    }
    if (fullscreen_ok &&
        upscale_mode == BTBH_GPU_SCENE_UPSCALE_LANCZOS &&
        (render_width != drawable_width ||
         render_height != drawable_height)) {
        BTBHGPUSceneSMAAUniforms upscale_uniforms;
        upscale_uniforms.metrics[0] =
            1.0f / (float)render_width;
        upscale_uniforms.metrics[1] =
            1.0f / (float)render_height;
        upscale_uniforms.metrics[2] = (float)render_width;
        upscale_uniforms.metrics[3] = (float)render_height;
        SDL_GPUTextureSamplerBinding upscale_sampler;
        SDL_zero(upscale_sampler);
        upscale_sampler.texture = runtime->composite_color_texture;
        upscale_sampler.sampler = runtime->scene_color_sampler;
        fullscreen_ok = btbh_gpu_scene_render_fullscreen(
            command,
            "BTBH Lanczos Upscale",
            runtime->upscale_lanczos_pipeline,
            runtime->upscaled_color_texture,
            &upscale_uniforms,
            (uint32_t)sizeof(upscale_uniforms),
            &upscale_sampler,
            1u
        );
        if (fullscreen_ok) {
            runtime->composite_color_texture =
            runtime->upscaled_color_texture;
            runtime->composite_width = drawable_width;
            runtime->composite_height = drawable_height;
        }
    }
    report->cpu_post_encode_milliseconds =
        (double)(SDL_GetTicksNS() - post_started_ticks) /
        1000000.0;
    if (!fullscreen_ok) {
        btbh_gpu_scene_cancel_submission(window, command);
        free(bone_offsets);
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return report->result;
    }
    if (!btbh_gpu_scene_cost_split(
            window,
            &command,
            BTBH_GPU_COST_GROUP_POST,
            report
        )) {
        free(bone_offsets);
        return report->result;
    }

    /* Only an unbroken run of temporal frames reprojects; any other frame restarts the history. */
    runtime->temporal_previous_valid = temporal_active;
    if (temporal_active) {
        (void)memcpy(
            runtime->temporal_previous_view_projection,
            requested_view_projection,
            sizeof(runtime->temporal_previous_view_projection)
        );
        (void)memcpy(
            runtime->temporal_previous_world_curvature,
            post->world_curvature,
            sizeof(runtime->temporal_previous_world_curvature)
        );
    }
    window->pending_command = command;
    window->pending_scene_ready = true;
    free(bone_offsets);

    report->result = BTBH_GPU_OK;
    report->drawable_width = drawable_width;
    report->drawable_height = drawable_height;
    report->batch_count = batch_count;
    report->primitive_draw_count = primitive_draw_count;
    report->prepared_geometry_count = prepared_geometry_count;
    uint32_t persistent_batch_count = 0u;
    for (uint32_t index = 0u; index < batch_count; ++index) {
        if (batches[index].instance_buffer != NULL) {
            persistent_batch_count += 1u;
        }
    }
    report->instance_count = logical_instance_count;
    report->transient_instance_count = transient_instance_count;
    report->persistent_batch_count = persistent_batch_count;
    report->message[0] = '\0';
    return report->result;
}

BTBHGPUResult btbh_gpu_window_scene_present_instances_with_visibility(
    BTBHGPUWindow *window,
    const char *vertex_shader_path,
    const char *animated_vertex_shader_path,
    const char *fast_shadow_vertex_shader_path,
    const char *fast_shadow_fragment_shader_path,
    const char *shadow_vertex_shader_path,
    const char *animated_shadow_vertex_shader_path,
    const char *shadow_fragment_shader_path,
    const char *fragment_shader_path,
    const char *unshadowed_fragment_shader_path,
    const char *post_vertex_shader_path,
    const char *post_fragment_shader_path,
    BTBHGPUShaderFormat shader_format,
    uint32_t render_width,
    uint32_t render_height,
    const float view_projection[16],
    const float camera_right[3],
    const float camera_up[3],
    const float camera_position[3],
    const BTBHGPUSceneLighting *lighting,
    const BTBHGPUScenePost *post,
    const float *instances,
    uint32_t transient_instance_count,
    uint32_t logical_instance_count,
    const BTBHGPUSceneGeometryBatch *batches,
    const uint8_t *scene_visibility,
    const uint8_t *directional_shadow_visibility,
    uint32_t batch_count,
    uint8_t clear_red,
    uint8_t clear_green,
    uint8_t clear_blue,
    uint8_t clear_alpha,
    BTBHGPUSceneReport *report
) {
    return btbh_gpu_window_scene_present_instances_internal(
        window,
        vertex_shader_path,
        animated_vertex_shader_path,
        fast_shadow_vertex_shader_path,
        fast_shadow_fragment_shader_path,
        shadow_vertex_shader_path,
        animated_shadow_vertex_shader_path,
        shadow_fragment_shader_path,
        fragment_shader_path,
        unshadowed_fragment_shader_path,
        post_vertex_shader_path,
        post_fragment_shader_path,
        shader_format,
        render_width,
        render_height,
        view_projection,
        camera_right,
        camera_up,
        camera_position,
        lighting,
        post,
        instances,
        transient_instance_count,
        logical_instance_count,
        batches,
        scene_visibility,
        directional_shadow_visibility,
        batch_count,
        clear_red,
        clear_green,
        clear_blue,
        clear_alpha,
        report
    );
}

BTBHGPUResult btbh_gpu_window_scene_present_instances(
    BTBHGPUWindow *window,
    const char *vertex_shader_path,
    const char *animated_vertex_shader_path,
    const char *fast_shadow_vertex_shader_path,
    const char *fast_shadow_fragment_shader_path,
    const char *shadow_vertex_shader_path,
    const char *animated_shadow_vertex_shader_path,
    const char *shadow_fragment_shader_path,
    const char *fragment_shader_path,
    const char *unshadowed_fragment_shader_path,
    const char *post_vertex_shader_path,
    const char *post_fragment_shader_path,
    BTBHGPUShaderFormat shader_format,
    uint32_t render_width,
    uint32_t render_height,
    const float view_projection[16],
    const float camera_right[3],
    const float camera_up[3],
    const float camera_position[3],
    const BTBHGPUSceneLighting *lighting,
    const BTBHGPUScenePost *post,
    const float *instances,
    uint32_t transient_instance_count,
    uint32_t logical_instance_count,
    const BTBHGPUSceneGeometryBatch *batches,
    uint32_t batch_count,
    uint8_t clear_red,
    uint8_t clear_green,
    uint8_t clear_blue,
    uint8_t clear_alpha,
    BTBHGPUSceneReport *report
) {
    return btbh_gpu_window_scene_present_instances_internal(
        window,
        vertex_shader_path,
        animated_vertex_shader_path,
        fast_shadow_vertex_shader_path,
        fast_shadow_fragment_shader_path,
        shadow_vertex_shader_path,
        animated_shadow_vertex_shader_path,
        shadow_fragment_shader_path,
        fragment_shader_path,
        unshadowed_fragment_shader_path,
        post_vertex_shader_path,
        post_fragment_shader_path,
        shader_format,
        render_width,
        render_height,
        view_projection,
        camera_right,
        camera_up,
        camera_position,
        lighting,
        post,
        instances,
        transient_instance_count,
        logical_instance_count,
        batches,
        NULL,
        NULL,
        batch_count,
        clear_red,
        clear_green,
        clear_blue,
        clear_alpha,
        report
    );
}

BTBHGPUTexture *btbh_gpu_window_capture_pending_scene_texture(
    BTBHGPUWindow *window,
    BTBHGPUTextureReport *report
) {
    if (report == NULL) {
        return NULL;
    }
    (void)memset(report, 0, sizeof(*report));
    if (
        window == NULL ||
        window->window == NULL ||
        window->device == NULL ||
        window->device->gpu == NULL ||
        !window->pending_scene_ready ||
        window->scene_runtime == NULL ||
        window->scene_runtime->composite_color_texture == NULL ||
        window->scene_runtime->composite_width == 0u ||
        window->scene_runtime->composite_height == 0u
    ) {
        report->result = BTBH_GPU_INVALID_ARGUMENT;
        (void)snprintf(
            report->message,
            sizeof(report->message),
            "%s",
            "GPU scene frame is not available for capture"
        );
        return NULL;
    }

    if (window->pending_command != NULL) {
        SDL_GPUCommandBuffer *scene_command =
            window->pending_command;
        window->pending_command = NULL;
        if (!SDL_SubmitGPUCommandBuffer(scene_command)) {
            window->pending_scene_ready = false;
            btbh_gpu_window_scene_submission_finished(
                window,
                false
            );
            report->result = BTBH_GPU_DEVICE_FAILURE;
            (void)snprintf(
                report->message,
                sizeof(report->message),
                "%s",
                SDL_GetError()
            );
            return NULL;
        }
    }

    BTBHGPUSceneRuntime *runtime = window->scene_runtime;
    SDL_GPUDevice *device = window->device->gpu;
    const SDL_GPUTextureFormat capture_format =
        SDL_GetGPUSwapchainTextureFormat(device, window->window);
    BTBHGPUTexture *capture = btbh_gpu_window_texture_create_empty(
        window,
        runtime->composite_width,
        runtime->composite_height,
        capture_format,
        SDL_GPU_TEXTUREUSAGE_SAMPLER |
            SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
        BTBH_GPU_TEXTURE_FILTER_LINEAR,
        BTBH_GPU_TEXTURE_WRAP_CLAMP,
        report
    );
    if (capture == NULL) {
        return NULL;
    }

    SDL_GPUCommandBuffer *command =
        SDL_AcquireGPUCommandBuffer(device);
    if (command == NULL) {
        btbh_gpu_texture_destroy(capture);
        report->result = BTBH_GPU_DEVICE_FAILURE;
        (void)snprintf(
            report->message,
            sizeof(report->message),
            "%s",
            SDL_GetError()
        );
        return NULL;
    }
    SDL_InsertGPUDebugLabel(command, "BTBH Scene Capture");
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command);
    if (copy == NULL) {
        (void)SDL_CancelGPUCommandBuffer(command);
        btbh_gpu_texture_destroy(capture);
        report->result = BTBH_GPU_DEVICE_FAILURE;
        (void)snprintf(
            report->message,
            sizeof(report->message),
            "%s",
            SDL_GetError()
        );
        return NULL;
    }
    SDL_GPUTextureLocation source;
    SDL_GPUTextureLocation destination;
    SDL_zero(source);
    SDL_zero(destination);
    source.texture = runtime->composite_color_texture;
    destination.texture = capture->texture;
    SDL_CopyGPUTextureToTexture(
        copy,
        &source,
        &destination,
        runtime->composite_width,
        runtime->composite_height,
        1u,
        false
    );
    SDL_EndGPUCopyPass(copy);
    if (!SDL_SubmitGPUCommandBuffer(command)) {
        btbh_gpu_texture_destroy(capture);
        report->result = BTBH_GPU_DEVICE_FAILURE;
        (void)snprintf(
            report->message,
            sizeof(report->message),
            "%s",
            SDL_GetError()
        );
        return NULL;
    }

    report->result = BTBH_GPU_OK;
    report->width = capture->width;
    report->height = capture->height;
    report->mip_count = 1u;
    report->message[0] = '\0';
    return capture;
}

bool btbh_gpu_window_blit_pending_scene(
    BTBHGPUWindow *window,
    SDL_GPUCommandBuffer *command,
    SDL_GPUTexture *destination,
    uint32_t destination_width,
    uint32_t destination_height
) {
    if (
        window == NULL ||
        command == NULL ||
        destination == NULL ||
        destination_width == 0u ||
        destination_height == 0u ||
        !window->pending_scene_ready ||
        window->scene_runtime == NULL ||
        window->scene_runtime->composite_color_texture == NULL ||
        window->scene_runtime->composite_width == 0u ||
        window->scene_runtime->composite_height == 0u
    ) {
        return false;
    }

    SDL_GPUBlitInfo blit;
    SDL_zero(blit);
    blit.source.texture =
        window->scene_runtime->composite_color_texture;
    blit.source.w = window->scene_runtime->composite_width;
    blit.source.h = window->scene_runtime->composite_height;
    blit.destination.texture = destination;
    blit.destination.w = destination_width;
    blit.destination.h = destination_height;
    blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
    blit.filter = SDL_GPU_FILTER_LINEAR;
    SDL_InsertGPUDebugLabel(command, "BTBH Scene Composite");
    SDL_BlitGPUTexture(command, &blit);
    return true;
}

/* Copies the finished scene into `destination` (a capture of the same size and format) and
   releases the frame, so another scene can render before the canvas presents. The frame is
   submitted here; the canvas then composites only what renders after it. */
BTBHGPUResult btbh_gpu_window_copy_pending_scene_texture(
    BTBHGPUWindow *window,
    BTBHGPUTexture *destination,
    BTBHGPUTextureReport *report
) {
    if (report == NULL) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    (void)memset(report, 0, sizeof(*report));
    if (
        window == NULL ||
        window->window == NULL ||
        window->device == NULL ||
        window->device->gpu == NULL ||
        !window->pending_scene_ready ||
        window->scene_runtime == NULL ||
        window->scene_runtime->composite_color_texture == NULL ||
        destination == NULL ||
        destination->owner != window ||
        destination->texture == NULL ||
        destination->width != window->scene_runtime->composite_width ||
        destination->height != window->scene_runtime->composite_height ||
        destination->format != SDL_GetGPUSwapchainTextureFormat(
            window->device->gpu,
            window->window
        )
    ) {
        report->result = BTBH_GPU_INVALID_ARGUMENT;
        (void)snprintf(
            report->message,
            sizeof(report->message),
            "%s",
            "GPU scene frame cannot be copied into that texture"
        );
        return report->result;
    }

    SDL_GPUDevice *device = window->device->gpu;
    SDL_GPUCommandBuffer *command = window->pending_command;
    window->pending_command = NULL;
    if (command == NULL) {
        command = SDL_AcquireGPUCommandBuffer(device);
    }
    window->pending_scene_ready = false;
    if (command == NULL) {
        btbh_gpu_window_scene_submission_finished(window, false);
        report->result = BTBH_GPU_DEVICE_FAILURE;
        (void)snprintf(
            report->message,
            sizeof(report->message),
            "%s",
            SDL_GetError()
        );
        return report->result;
    }
    SDL_InsertGPUDebugLabel(command, "BTBH Scene Copy");
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command);
    if (copy != NULL) {
        SDL_GPUTextureLocation source;
        SDL_GPUTextureLocation target;
        SDL_zero(source);
        SDL_zero(target);
        source.texture = window->scene_runtime->composite_color_texture;
        target.texture = destination->texture;
        SDL_CopyGPUTextureToTexture(
            copy,
            &source,
            &target,
            destination->width,
            destination->height,
            1u,
            false
        );
        SDL_EndGPUCopyPass(copy);
    }
    const bool submitted = SDL_SubmitGPUCommandBuffer(command);
    btbh_gpu_window_scene_submission_finished(window, submitted);
    if (copy == NULL || !submitted) {
        report->result = BTBH_GPU_DEVICE_FAILURE;
        (void)snprintf(
            report->message,
            sizeof(report->message),
            "%s",
            SDL_GetError()
        );
        return report->result;
    }
    report->result = BTBH_GPU_OK;
    report->width = destination->width;
    report->height = destination->height;
    report->mip_count = 1u;
    return report->result;
}

static void btbh_gpu_scene_release_pipeline_array(
    SDL_GPUDevice *device,
    SDL_GPUGraphicsPipeline **pipelines,
    size_t count
) {
    for (size_t index = 0u; index < count; ++index) {
        if (pipelines[index] != NULL) {
            SDL_ReleaseGPUGraphicsPipeline(
                device,
                pipelines[index]
            );
        }
    }
}

void btbh_gpu_window_release_scene(BTBHGPUWindow *window) {
    if (window == NULL || window->scene_runtime == NULL) {
        return;
    }
    SDL_GPUDevice *device =
        window->device != NULL && !window->device->shutdown
            ? window->device->gpu
            : NULL;
    if (device != NULL) {
        btbh_gpu_cost_release_warm();
        btbh_gpu_radiance_cascades_runtime_release(
            device,
            &window->scene_runtime->radiance_cascades
        );
        btbh_gpu_fsr3_release(device, &window->scene_runtime->fsr3);
        if (window->scene_runtime->temporal_previous_color != NULL) {
            SDL_ReleaseGPUTexture(
                device,
                window->scene_runtime->temporal_previous_color
            );
        }
        if (window->scene_runtime->velocity_texture != NULL) {
            SDL_ReleaseGPUTexture(
                device,
                window->scene_runtime->velocity_texture
            );
        }
        if (window->scene_runtime->reactive_texture != NULL) {
            SDL_ReleaseGPUTexture(
                device,
                window->scene_runtime->reactive_texture
            );
        }
        if (window->scene_runtime->motion_buffer != NULL) {
            SDL_ReleaseGPUBuffer(
                device,
                window->scene_runtime->motion_buffer
            );
        }
        if (window->scene_runtime->upload_transfer != NULL) {
            SDL_ReleaseGPUTransferBuffer(
                device,
                window->scene_runtime->upload_transfer
            );
        }
        if (window->scene_runtime->bone_buffer != NULL) {
            SDL_ReleaseGPUBuffer(
                device,
                window->scene_runtime->bone_buffer
            );
        }
        if (window->scene_runtime->instance_buffer != NULL) {
            SDL_ReleaseGPUBuffer(
                device,
                window->scene_runtime->instance_buffer
            );
        }
        if (window->scene_runtime->instance_vertex_buffer != NULL) {
            SDL_ReleaseGPUBuffer(
                device,
                window->scene_runtime->instance_vertex_buffer
            );
        }
        if (window->scene_runtime->brdf_lut_texture != NULL) {
            SDL_ReleaseGPUTexture(
                device,
                window->scene_runtime->brdf_lut_texture
            );
        }
        if (window->scene_runtime->brdf_lut_sampler != NULL) {
            SDL_ReleaseGPUSampler(
                device,
                window->scene_runtime->brdf_lut_sampler
            );
        }
        if (window->scene_runtime->depth_texture != NULL) {
            SDL_ReleaseGPUTexture(
                device,
                window->scene_runtime->depth_texture
            );
        }
        if (window->scene_runtime->scene_color_texture != NULL) {
            SDL_ReleaseGPUTexture(
                device,
                window->scene_runtime->scene_color_texture
            );
        }
        if (window->scene_runtime->ambient_texture != NULL) {
            SDL_ReleaseGPUTexture(
                device,
                window->scene_runtime->ambient_texture
            );
        }
        if (
            window->scene_runtime->ambient_specular_texture != NULL
        ) {
            SDL_ReleaseGPUTexture(
                device,
                window->scene_runtime->ambient_specular_texture
            );
        }
        if (window->scene_runtime->normal_texture != NULL) {
            SDL_ReleaseGPUTexture(
                device,
                window->scene_runtime->normal_texture
            );
        }
        if (window->scene_runtime->emission_texture != NULL) {
            SDL_ReleaseGPUTexture(
                device,
                window->scene_runtime->emission_texture
            );
        }
        if (window->scene_runtime->ssao_texture != NULL) {
            SDL_ReleaseGPUTexture(
                device,
                window->scene_runtime->ssao_texture
            );
        }
        if (window->scene_runtime->ssao_denoised_texture != NULL) {
            SDL_ReleaseGPUTexture(
                device,
                window->scene_runtime->ssao_denoised_texture
            );
        }
        if (window->scene_runtime->ssao_resolved_texture != NULL) {
            SDL_ReleaseGPUTexture(
                device,
                window->scene_runtime->ssao_resolved_texture
            );
        }
        if (window->scene_runtime->scene_distance_texture != NULL) {
            SDL_ReleaseGPUTexture(
                device,
                window->scene_runtime->scene_distance_texture
            );
        }
        if (window->scene_runtime->refraction_texture != NULL) {
            SDL_ReleaseGPUTexture(
                device,
                window->scene_runtime->refraction_texture
            );
        }
        if (window->scene_runtime->post_ping_texture != NULL) {
            SDL_ReleaseGPUTexture(
                device,
                window->scene_runtime->post_ping_texture
            );
        }
        if (window->scene_runtime->post_pong_texture != NULL) {
            SDL_ReleaseGPUTexture(
                device,
                window->scene_runtime->post_pong_texture
            );
        }
        if (window->scene_runtime->final_color_texture != NULL) {
            SDL_ReleaseGPUTexture(
                device,
                window->scene_runtime->final_color_texture
            );
        }
        if (window->scene_runtime->upscaled_color_texture != NULL) {
            SDL_ReleaseGPUTexture(
                device,
                window->scene_runtime->upscaled_color_texture
            );
        }
        if (window->scene_runtime->sharpened_color_texture != NULL) {
            SDL_ReleaseGPUTexture(
                device,
                window->scene_runtime->sharpened_color_texture
            );
        }
        if (window->scene_runtime->post_color_texture != NULL) {
            SDL_ReleaseGPUTexture(
                device,
                window->scene_runtime->post_color_texture
            );
        }
        if (window->scene_runtime->smaa_edges_texture != NULL) {
            SDL_ReleaseGPUTexture(
                device,
                window->scene_runtime->smaa_edges_texture
            );
        }
        if (window->scene_runtime->smaa_weights_texture != NULL) {
            SDL_ReleaseGPUTexture(
                device,
                window->scene_runtime->smaa_weights_texture
            );
        }
        btbh_gpu_scene_release_bloom_targets(device, window->scene_runtime);
        if (window->scene_runtime->scene_color_sampler != NULL) {
            SDL_ReleaseGPUSampler(
                device,
                window->scene_runtime->scene_color_sampler
            );
        }
        if (
            window->scene_runtime->directional_shadow_sampler != NULL
        ) {
            SDL_ReleaseGPUSampler(
                device,
                window->scene_runtime->directional_shadow_sampler
            );
        }
        if (
            window->scene_runtime->directional_shadow_compare_sampler !=
                NULL
        ) {
            SDL_ReleaseGPUSampler(
                device,
                window->scene_runtime->directional_shadow_compare_sampler
            );
        }
        for (uint32_t index = 0u; index < 3u; ++index) {
            if (
                window->scene_runtime
                    ->directional_shadow_textures[index] != NULL
            ) {
                SDL_ReleaseGPUTexture(
                    device,
                    window->scene_runtime
                        ->directional_shadow_textures[index]
                );
            }
        }
        if (window->scene_runtime->omni_shadow_texture != NULL) {
            SDL_ReleaseGPUTexture(
                device,
                window->scene_runtime->omni_shadow_texture
            );
        }
        for (uint32_t cull_index = 0u;
             cull_index < BTBH_GPU_SCENE_CULL_COUNT;
             ++cull_index) {
            if (
                window->scene_runtime
                    ->shadow_pipelines[cull_index] != NULL
            ) {
                SDL_ReleaseGPUGraphicsPipeline(
                    device,
                    window->scene_runtime
                        ->shadow_pipelines[cull_index]
                );
            }
            if (
                window->scene_runtime
                    ->fast_shadow_pipelines[cull_index] != NULL
            ) {
                SDL_ReleaseGPUGraphicsPipeline(
                    device,
                    window->scene_runtime
                        ->fast_shadow_pipelines[cull_index]
                );
            }
            if (
                window->scene_runtime
                    ->non_discard_shadow_pipelines[cull_index] != NULL
            ) {
                SDL_ReleaseGPUGraphicsPipeline(
                    device,
                    window->scene_runtime
                        ->non_discard_shadow_pipelines[cull_index]
                );
            }
            if (
                window->scene_runtime
                    ->animated_shadow_pipelines[cull_index] != NULL
            ) {
                SDL_ReleaseGPUGraphicsPipeline(
                    device,
                    window->scene_runtime
                        ->animated_shadow_pipelines[cull_index]
                );
            }
            if (
                window->scene_runtime
                    ->animated_non_discard_shadow_pipelines[
                        cull_index
                    ] != NULL
            ) {
                SDL_ReleaseGPUGraphicsPipeline(
                    device,
                    window->scene_runtime
                        ->animated_non_discard_shadow_pipelines[
                            cull_index
                        ]
                );
            }
        }
        if (window->scene_runtime->post_pipeline != NULL) {
            SDL_ReleaseGPUGraphicsPipeline(
                device,
                window->scene_runtime->post_pipeline
            );
        }
        if (window->scene_runtime->post_lut_pipeline != NULL) {
            SDL_ReleaseGPUGraphicsPipeline(
                device,
                window->scene_runtime->post_lut_pipeline
            );
        }
        if (window->scene_runtime->post_cas_pipeline != NULL) {
            SDL_ReleaseGPUGraphicsPipeline(
                device,
                window->scene_runtime->post_cas_pipeline
            );
        }
        if (window->scene_runtime->post_tilt_shift_pipeline != NULL) {
            SDL_ReleaseGPUGraphicsPipeline(
                device,
                window->scene_runtime->post_tilt_shift_pipeline
            );
        }
        if (window->scene_runtime->upscale_lanczos_pipeline != NULL) {
            SDL_ReleaseGPUGraphicsPipeline(
                device,
                window->scene_runtime->upscale_lanczos_pipeline
            );
        }
        if (window->scene_runtime->upscale_fsr_easu_pipeline != NULL) {
            SDL_ReleaseGPUGraphicsPipeline(
                device,
                window->scene_runtime->upscale_fsr_easu_pipeline
            );
        }
        if (window->scene_runtime->upscale_fsr_rcas_pipeline != NULL) {
            SDL_ReleaseGPUGraphicsPipeline(
                device,
                window->scene_runtime->upscale_fsr_rcas_pipeline
            );
        }
        if (window->scene_runtime->ssao_resolve_pipeline != NULL) {
            SDL_ReleaseGPUGraphicsPipeline(
                device,
                window->scene_runtime->ssao_resolve_pipeline
            );
        }
        if (window->scene_runtime->scene_distance_pipeline != NULL) {
            SDL_ReleaseGPUGraphicsPipeline(
                device,
                window->scene_runtime->scene_distance_pipeline
            );
        }
        if (window->scene_runtime->ssao_denoise_pipeline != NULL) {
            SDL_ReleaseGPUGraphicsPipeline(
                device,
                window->scene_runtime->ssao_denoise_pipeline
            );
        }
        if (window->scene_runtime->ssao_pipeline != NULL) {
            SDL_ReleaseGPUGraphicsPipeline(
                device,
                window->scene_runtime->ssao_pipeline
            );
        }
        btbh_gpu_scene_release_pipeline_array(
            device,
            window->scene_runtime->smaa_edge_pipelines,
            BTBH_GPU_SCENE_SMAA_QUALITY_COUNT
        );
        btbh_gpu_scene_release_pipeline_array(
            device,
            window->scene_runtime->smaa_weight_pipelines,
            BTBH_GPU_SCENE_SMAA_QUALITY_COUNT
        );
        if (window->scene_runtime->smaa_blend_pipeline != NULL) {
            SDL_ReleaseGPUGraphicsPipeline(
                device,
                window->scene_runtime->smaa_blend_pipeline
            );
        }
        if (window->scene_runtime->fxaa_pipeline != NULL) {
            SDL_ReleaseGPUGraphicsPipeline(
                device,
                window->scene_runtime->fxaa_pipeline
            );
        }
        if (window->scene_runtime->bloom_downsample_pipeline != NULL) {
            SDL_ReleaseGPUGraphicsPipeline(
                device,
                window->scene_runtime->bloom_downsample_pipeline
            );
        }
        if (window->scene_runtime->bloom_upsample_pipeline != NULL) {
            SDL_ReleaseGPUGraphicsPipeline(
                device,
                window->scene_runtime->bloom_upsample_pipeline
            );
        }
        if (window->scene_runtime->hdr_composite_pipeline != NULL) {
            SDL_ReleaseGPUGraphicsPipeline(
                device,
                window->scene_runtime->hdr_composite_pipeline
            );
        }
        const size_t pipeline_count =
            BTBH_GPU_SCENE_COLOR_TARGET_VARIANT_COUNT *
            BTBH_GPU_SCENE_BLEND_COUNT *
            BTBH_GPU_SCENE_DEPTH_COUNT *
            BTBH_GPU_SCENE_TRANSPARENCY_COUNT *
            BTBH_GPU_SCENE_CULL_COUNT *
            BTBH_GPU_SCENE_STENCIL_VARIANT_COUNT *
            BTBH_GPU_SCENE_UNLIT_VARIANT_COUNT;
        btbh_gpu_scene_release_pipeline_array(
            device,
            &window->scene_runtime
                ->model_pipelines[0][0][0][0][0][0][0],
            pipeline_count
        );
        btbh_gpu_scene_release_pipeline_array(
            device,
            &window->scene_runtime
                ->animated_model_pipelines[0][0][0][0][0][0][0],
            pipeline_count
        );
        btbh_gpu_scene_release_pipeline_array(
            device,
            &window->scene_runtime
                ->non_omni_shadowed_model_pipelines
                    [0][0][0][0][0][0][0],
            pipeline_count
        );
        btbh_gpu_scene_release_pipeline_array(
            device,
            &window->scene_runtime
                ->non_omni_shadowed_animated_model_pipelines
                    [0][0][0][0][0][0][0],
            pipeline_count
        );
        btbh_gpu_scene_release_pipeline_array(
            device,
            &window->scene_runtime
                ->unshadowed_model_pipelines[0][0][0][0][0][0][0],
            pipeline_count
        );
        btbh_gpu_scene_release_pipeline_array(
            device,
            &window->scene_runtime
                ->unshadowed_animated_model_pipelines
                    [0][0][0][0][0][0][0],
            pipeline_count
        );
    }
    if (window->scene_runtime->fallback_texture != NULL) {
        btbh_gpu_texture_destroy(
            window->scene_runtime->fallback_texture
        );
    }
    if (window->scene_runtime->normal_fallback_texture != NULL) {
        btbh_gpu_texture_destroy(
            window->scene_runtime->normal_fallback_texture
        );
    }
    if (window->scene_runtime->smaa_area_texture != NULL) {
        btbh_gpu_texture_destroy(
            window->scene_runtime->smaa_area_texture
        );
    }
    if (window->scene_runtime->smaa_search_texture != NULL) {
        btbh_gpu_texture_destroy(
            window->scene_runtime->smaa_search_texture
        );
    }
    window->pending_scene_ready = false;
    free(window->scene_runtime->temporal_palettes);
    free(window->scene_runtime->temporal_palette_matrices);
    free(window->scene_runtime);
    window->scene_runtime = NULL;
}
