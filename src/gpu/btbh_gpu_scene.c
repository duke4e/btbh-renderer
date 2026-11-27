#include "btbh_gpu_internal.h"
#include "btbh_gpu_radiance_cascades_runtime.h"
#include "btbh_gpu_fsr3.h"
#include "btbh_gpu_sdf_terrain_internal.h"
#include "btbh_gpu_world_detail.h"
#include "btbh_gpu_height_lift.h"
#include "btbh_gpu_particle.h"

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
/* BTBH_GPU_SCENE_REACTIVE_NONE: a reactive scale that rounds full alpha to 0 in the R8 mask. */
#define BTBH_GPU_SCENE_REACTIVE_NONE_SCALE 1.0e-6f

/* RFC 0009: what a scene fragment shader file samples, read from its SPIR-V names (btbh_gpu_scene_check_scene_shader).
   Six variants, and room for a host that switches shader directories; RFC 0013 adds the static vertex shaders a
   batch with the UV rect draws with (up to five), and its phase 2 the two shadowed lit particle shaders. */
#define BTBH_GPU_SCENE_SHADER_CHECK_CAPACITY 24u
typedef struct BTBHGPUSceneShaderCheck {
    char *path;
    bool has_dig_mask;
    bool has_world_detail;
    bool has_world_detail_table;
    /* RFC 0010: the radiance cascades' trace shader reads the sources' tops (btbh_gpu_scene_check_rc_shader). */
    bool has_source_top;
    /* RFC 0013: a static vertex shader reads the instance's UV rect (btbh_gpu_scene_check_uv_rect_shaders). */
    bool has_instance_uv_rect;
    /* RFC 0013 phase 2: a lit particle or scene fragment shader reads the particle transmittance map
       (btbh_gpu_scene_check_particle_shadow_shaders). */
    bool has_particle_transmittance;
    /* RFC 0014: a vertex shader writes a clip distance (its SPIR-V declares the ClipDistance capability;
       btbh_gpu_scene_check_view_shaders). */
    bool has_clip_distance;
} BTBHGPUSceneShaderCheck;

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
    /* RFC 0013: 1 for a batch with BTBH_GPU_SCENE_BATCH_INSTANCE_UV_RECT (production_temporal.hlsli's
       BTBH_TEMPORAL_UNIFORMS_STATIC; the skinned shaders' first reserved word). */
    uint32_t instance_uv_rect;
    uint32_t temporal_reserved[2];
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

/* RFC 0007: where an SDF terrain's dig mask lies (production_dig_mask.hlsli); zero for a batch without one. */
typedef struct BTBHGPUSceneDigMaskUniforms {
    float origin[4];
    float scale[4];
} BTBHGPUSceneDigMaskUniforms;

/* The scene fragment shaders' second uniform buffer (production_model.frag.hlsl BatchExtension): the dig mask's
   placement, then RFC 0008's world detail (btbh_gpu_world_detail_pack); all zero for a batch without them. The
   fast unlit, radiance-cascade source and shadow shaders declare the dig mask's part only and read just that. */
typedef struct BTBHGPUSceneBatchExtensionUniforms {
    BTBHGPUSceneDigMaskUniforms dig_mask;
    float world_detail_tiles[4];
    float world_detail_controls[4];
    uint32_t world_detail_mode[4];
} BTBHGPUSceneBatchExtensionUniforms;

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
    /* The reflections' hits (since 0.10.0): x 1 for BTBH_GPU_SCENE_POST_SSR_FACING_HITS; yzw 0. */
    float ssr_hit_options[4];
} BTBHGPUPostUniforms;

/* The horizon-based ambient occlusion's passes (BTBHGPUScenePost.ssao_enabled 2, production_hbao.hlsli). */
typedef struct BTBHGPUSceneHBAOUniforms {
    float inverse_view_projection[16];
    /* ssao_radius, ssao_intensity, ssao_power, ssao_bias. */
    float controls[4];
    /* What it darkens (1 ambient, 2 with the radiance cascades, 3 contact), the debug view (bit 0). */
    uint32_t options[4];
    /* The projection's y scale. */
    float view[4];
} BTBHGPUSceneHBAOUniforms;

#define BTBH_GPU_SCENE_HBAO_CONTACT_MODE 3u

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

/* RFC 0012: one set of static model pipelines, indexed as model_pipelines. */
typedef SDL_GPUGraphicsPipeline *BTBHGPUSceneModelPipelineSet[
    BTBH_GPU_SCENE_COLOR_TARGET_VARIANT_COUNT
][
    BTBH_GPU_SCENE_BLEND_COUNT
][BTBH_GPU_SCENE_DEPTH_COUNT]
[BTBH_GPU_SCENE_TRANSPARENCY_COUNT]
[BTBH_GPU_SCENE_CULL_COUNT]
[BTBH_GPU_SCENE_STENCIL_VARIANT_COUNT]
[BTBH_GPU_SCENE_UNLIT_VARIANT_COUNT];

/* RFC 0012: the lifted static batches' pipelines (production_model_lift.vert, production_model_detail_table_lift.vert),
   [0 without a world-detail table, 1 with][0 the sun's and the omni shadow, 1 the sun's only, 2 unshadowed]. Made by
   the first lifted batch (btbh_gpu_scene_ensure_model_pipeline), so a host that lifts nothing never holds them. */
typedef struct BTBHGPUSceneLiftPipelines {
    BTBHGPUSceneModelPipelineSet sets[2][3];
} BTBHGPUSceneLiftPipelines;

typedef struct BTBHGPUSceneRunVerify BTBHGPUSceneRunVerify;

/* RFC 0014 (scene views, since 0.16.0): the screen-space passes that read a view's camera, drawn clipped to each view's
   half (production_post_view.vert with the pass's own fragment shader). */
typedef enum BTBHGPUSceneViewPipeline {
    BTBH_GPU_SCENE_VIEW_PIPELINE_SSAO = 0,
    BTBH_GPU_SCENE_VIEW_PIPELINE_SSAO_DENOISE = 1,
    BTBH_GPU_SCENE_VIEW_PIPELINE_SSAO_RESOLVE = 2,
    BTBH_GPU_SCENE_VIEW_PIPELINE_HBAO = 3,
    BTBH_GPU_SCENE_VIEW_PIPELINE_HBAO_DENOISE = 4,
    BTBH_GPU_SCENE_VIEW_PIPELINE_HBAO_RESOLVE = 5,
    BTBH_GPU_SCENE_VIEW_PIPELINE_DISTANCE = 6,
    BTBH_GPU_SCENE_VIEW_PIPELINE_COMPOSITE = 7,
    BTBH_GPU_SCENE_VIEW_PIPELINE_COUNT = 8
} BTBHGPUSceneViewPipeline;

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
    /* RFC 0009: a static batch with a world-detail table draws with production_model_detail_table.vert, which
       reads each vertex's table index; the other static batches' production_model.vert doesn't (a sixth vertex
       attribute made every static draw dearer). Their pipelines, as model_pipelines, non_omni_shadowed_ and
       unshadowed_model_pipelines. */
    SDL_GPUGraphicsPipeline *table_model_pipelines[
        BTBH_GPU_SCENE_COLOR_TARGET_VARIANT_COUNT
    ][
        BTBH_GPU_SCENE_BLEND_COUNT
    ][BTBH_GPU_SCENE_DEPTH_COUNT]
    [BTBH_GPU_SCENE_TRANSPARENCY_COUNT]
    [BTBH_GPU_SCENE_CULL_COUNT]
    [BTBH_GPU_SCENE_STENCIL_VARIANT_COUNT]
    [BTBH_GPU_SCENE_UNLIT_VARIANT_COUNT];
    SDL_GPUGraphicsPipeline *non_omni_shadowed_table_model_pipelines[
        BTBH_GPU_SCENE_COLOR_TARGET_VARIANT_COUNT
    ][
        BTBH_GPU_SCENE_BLEND_COUNT
    ][BTBH_GPU_SCENE_DEPTH_COUNT]
    [BTBH_GPU_SCENE_TRANSPARENCY_COUNT]
    [BTBH_GPU_SCENE_CULL_COUNT]
    [BTBH_GPU_SCENE_STENCIL_VARIANT_COUNT]
    [BTBH_GPU_SCENE_UNLIT_VARIANT_COUNT];
    SDL_GPUGraphicsPipeline *unshadowed_table_model_pipelines[
        BTBH_GPU_SCENE_COLOR_TARGET_VARIANT_COUNT
    ][
        BTBH_GPU_SCENE_BLEND_COUNT
    ][BTBH_GPU_SCENE_DEPTH_COUNT]
    [BTBH_GPU_SCENE_TRANSPARENCY_COUNT]
    [BTBH_GPU_SCENE_CULL_COUNT]
    [BTBH_GPU_SCENE_STENCIL_VARIANT_COUNT]
    [BTBH_GPU_SCENE_UNLIT_VARIANT_COUNT];
    BTBHGPUSceneLiftPipelines *lift_pipelines;
    /* RFC 0013: the lit particles' pipelines (production_model.vert with production_particle_lit.frag, its
       _2d_shadows or its _unshadowed sibling), [0 the sun's and the omni shadow, 1 the sun's only, 2 unshadowed]
       [1 while the temporal upscaler runs][depth][cull][stencil]: always transparent with straight alpha, so their
       colour targets are the transparent pass's alone. */
    SDL_GPUGraphicsPipeline *lit_particle_pipelines[3][2]
        [BTBH_GPU_SCENE_DEPTH_COUNT]
        [BTBH_GPU_SCENE_CULL_COUNT]
        [BTBH_GPU_SCENE_STENCIL_VARIANT_COUNT];
    /* RFC 0013 phase 2: the particle transmittance map (RGBA16F in sun space, half or a quarter of the sun map's size)
       and its pipeline (production_particle_transmittance.vert and .frag), made by the first frame (or prewarm) that
       draws it, so a host that never sets BTBHGPUSceneLighting.particle_shadow holds neither. */
    SDL_GPUTexture *particle_transmittance_texture;
    uint32_t particle_transmittance_width;
    uint32_t particle_transmittance_height;
    SDL_GPUGraphicsPipeline *particle_transmittance_pipeline;
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
    SDL_GPUGraphicsPipeline *hbao_pipeline;
    SDL_GPUGraphicsPipeline *hbao_denoise_pipeline;
    SDL_GPUGraphicsPipeline *hbao_resolve_pipeline;
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
    /* Per batch of the scene being drawn: its dug_by terrain's dig mask, or a NULL texture (RFC 0007). */
    BTBHGPUSdfTerrainMaskBinding *dig_bindings;
    uint32_t dig_binding_capacity;
    /* The scene fragment shaders checked for the dig mask, the world detail and the world-detail table
       (btbh_gpu_scene_check_scene_shader), and whether they sample them: each path is read once (the variant a
       present draws with changes with its shadows and the radiance cascades). */
    BTBHGPUSceneShaderCheck scene_shader_checks[BTBH_GPU_SCENE_SHADER_CHECK_CAPACITY];
    uint32_t scene_shader_check_next;
    BTBHGPUTexture *fallback_texture;
    BTBHGPUTexture *normal_fallback_texture;
    /* RFC 0009: a 1x1, one-layer array in the world-detail table's slot of a batch without a table (the slot's
       view must be an array even where nothing samples it). */
    BTBHGPUTexture *fallback_array_texture;
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
    /* The horizon-based occlusion's half-resolution visibility and view depth, before and after its denoise. */
    SDL_GPUTexture *hbao_texture;
    SDL_GPUTexture *hbao_denoised_texture;
    uint32_t hbao_width;
    uint32_t hbao_height;
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
    /* BTBH_GPU_SCENE_OPAQUE_SORT_FRONT_TO_BACK, per frame: the opaque pass's batches in drawing order, where
       each batch's geometries start among the prepared ones (their bone offsets), and the sort's keys (twice
       opaque_order_capacity). */
    uint32_t *opaque_order;
    uint32_t *opaque_geometry_base;
    uint64_t *opaque_order_keys;
    uint32_t opaque_order_capacity;
    /* BTBH_GPU_SCENE_RUNS=verify (since 0.13.0): what the pass encoding batches holds, made by the first present in
       the mode. */
    BTBHGPUSceneRunVerify *run_verify;
    /* The shadow maps this runtime has drawn, each map a frame updates once (btbh_gpu_window_scene_shadow_render_count,
       for the smokes; since 0.15.0). Never reset. */
    uint64_t shadow_render_count;
    /* A target view's rect (since 0.15.0): a 1x1 texture in the target's format, cleared to the call's clear colour
       and blitted over the target outside the rect (btbh_gpu_scene_fill_outside_rect). Made by the first render (or
       prewarm) with a rect into a target of that format. */
    SDL_GPUTexture *target_clear_texture;
    SDL_GPUTextureFormat target_clear_format;
    /* RFC 0014 (scene views, since 0.16.0), made by the first frame of views that needs them (or
       btbh_gpu_window_scene_prewarm_views): the clipped screen-space passes' pipelines; the second view's own sun map
       ([0], at the sun map's size) and spot map ([1]), with their retained matrices and state as retained_shadow_*
       keeps view 0's, and its own particle transmittance map; the second view's opaque order, and the scene's and each
       view's visibility combined for the sorts (2 x view_order_capacity bytes). */
    SDL_GPUGraphicsPipeline *view_pipelines[BTBH_GPU_SCENE_VIEW_PIPELINE_COUNT];
    SDL_GPUTexture *view_shadow_textures[2];
    uint32_t view_shadow_width;
    uint32_t view_shadow_height;
    float view_retained_shadow_view_projection[2][16];
    uint8_t view_retained_shadow_ready[2];
    uint8_t view_retained_shadow_active[2];
    SDL_GPUTexture *view_particle_transmittance_texture;
    uint32_t view_particle_transmittance_width;
    uint32_t view_particle_transmittance_height;
    uint32_t *view_opaque_order;
    uint8_t *view_visibility;
    uint32_t view_order_capacity;
};

/* A target view's rect (BTBHGPUSceneTargetView, since 0.15.0): the part of the target a scene rendered into it shades,
   in the target's pixels (output) and mapped to the render size (render: scaled, rounded outward, clamped). Each pass
   limits itself to it at its own target's size (btbh_gpu_scene_scissor_at). Inactive (or NULL): every pixel, as
   before; a rect that covers the whole target is inactive too. */
typedef struct BTBHGPUSceneScissor {
    bool active;
    uint32_t output_width;
    uint32_t output_height;
    uint32_t render_width;
    uint32_t render_height;
    SDL_Rect output;
    SDL_Rect render;
} BTBHGPUSceneScissor;

/* [start, start + length) of `from` pixels in `to` pixels: scaled, rounded outward and clamped (at least one pixel for
   a length of 1 or more and a start inside `from`). */
static void btbh_gpu_scene_scale_span(
    uint32_t start,
    uint32_t length,
    uint32_t from,
    uint32_t to,
    int *first,
    int *count
) {
    uint64_t low = ((uint64_t)start * to) / from;
    uint64_t high = (((uint64_t)start + length) * to + from - 1u) / from;
    if (high > to) {
        high = to;
    }
    if (low >= high) {
        low = high > 0u ? high - 1u : 0u;
    }
    *first = (int)low;
    *count = (int)(high - low);
}

/* The rect a pass into a target of width x height made from the render size limits itself to (SDL_SetGPUScissor): the
   render-size rect, or that rect scaled to the target's size (the SSAO's and HBAO's half size, bloom's levels), rounded
   outward. NULL when the scene shades every pixel. */
static const SDL_Rect *btbh_gpu_scene_scissor_at(
    const BTBHGPUSceneScissor *scissor,
    uint32_t width,
    uint32_t height,
    SDL_Rect *storage
) {
    if (scissor == NULL || !scissor->active || width == 0u || height == 0u) {
        return NULL;
    }
    if (width == scissor->render_width && height == scissor->render_height) {
        return &scissor->render;
    }
    btbh_gpu_scene_scale_span(
        (uint32_t)scissor->render.x,
        (uint32_t)scissor->render.w,
        scissor->render_width,
        width,
        &storage->x,
        &storage->w
    );
    btbh_gpu_scene_scale_span(
        (uint32_t)scissor->render.y,
        (uint32_t)scissor->render.h,
        scissor->render_height,
        height,
        &storage->y,
        &storage->h
    );
    return storage;
}

/* The rect a pass at the output size (the upscalers') limits itself to: the view's own, in the target's pixels. NULL
   when the scene shades every pixel. */
static const SDL_Rect *btbh_gpu_scene_output_scissor(const BTBHGPUSceneScissor *scissor) {
    return scissor != NULL && scissor->active ? &scissor->output : NULL;
}

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

uint8_t btbh_gpu_window_scene_read_run_verify(
    const BTBHGPUWindow *window,
    BTBHGPUSceneRunVerifyReport *report
) {
    if (window == NULL || report == NULL) {
        return 0u;
    }
    *report = window->scene_run_verify;
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
    "upload", "shadow", "main", "rc_src", "rc_field", "rc_screen", "ssao", "transparent", "post", "ui",
    "view1_main", "view1_transparent"
};
static SDL_GPUDevice *btbh_gpu_cost_split_device;
static uint64_t btbh_gpu_cost_frame_serial;
static double btbh_gpu_cost_group_milliseconds[BTBH_GPU_COST_GROUP_COUNT];
/* The radiance-cascade emitters (gameplay lights) the frame drew into the source field and the direct light. */
static uint32_t btbh_gpu_cost_rc_emitters;

/* The pass frames (see `BTBHGPUCostStep`, since 0.16.0): whether the current split frame is one, the step its passes
   go to now, each step's time less the floor, the floor, the pieces submitted and their time. */
static const char *const btbh_gpu_cost_step_names[BTBH_GPU_COST_STEP_COUNT] = {
    "ao", "ssr", "bloom", "composite", "lut", "cas", "output", "fxaa", "smaa",
    "fsr3_prepare_inputs", "fsr3_luma_pyramid", "fsr3_shading_change_pyramid", "fsr3_shading_change",
    "fsr3_prepare_reactivity", "fsr3_luma_instability", "fsr3_accumulate", "fsr3_rcas",
    "fsr1_easu", "fsr1_rcas", "lanczos"
};
static int btbh_gpu_cost_passes_state = -1;
static uint64_t btbh_gpu_cost_split_serial;
static bool btbh_gpu_cost_pass_frame;
static int btbh_gpu_cost_current_step = BTBH_GPU_COST_STEP_NONE;
static double btbh_gpu_cost_step_milliseconds[BTBH_GPU_COST_STEP_COUNT];
static double btbh_gpu_cost_floor_milliseconds;
static uint32_t btbh_gpu_cost_pieces;
static double btbh_gpu_cost_pieces_milliseconds;

static bool btbh_gpu_cost_passes_enabled(void) {
    if (btbh_gpu_cost_passes_state < 0) {
        const char *value = SDL_getenv("BTBH_GPU_COST_PASSES");
        btbh_gpu_cost_passes_state = value != NULL && strcmp(value, "1") == 0 ? 1 : 0;
    }
    return btbh_gpu_cost_passes_state == 1;
}

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

/* A pass frame's submission floor: an empty command buffer's submit-to-fence time, the median of three, with the GPU
   idle as before each piece (0 when the GPU failed). */
static double btbh_gpu_cost_measure_floor(SDL_GPUDevice *device) {
    double samples[3] = {0.0, 0.0, 0.0};
    for (uint32_t index = 0u; index < 3u; ++index) {
        SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(device);
        if (command == NULL) {
            return 0.0;
        }
        const uint64_t submit_started_ticks = SDL_GetTicksNS();
        SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
        if (fence == NULL) {
            return 0.0;
        }
        (void)SDL_WaitForGPUFences(device, true, &fence, 1u);
        samples[index] = (double)(SDL_GetTicksNS() - submit_started_ticks) / 1000000.0;
        SDL_ReleaseGPUFence(device, fence);
    }
    const double low = SDL_min(samples[0], samples[1]);
    const double high = SDL_max(samples[0], samples[1]);
    return SDL_max(low, SDL_min(high, samples[2]));
}

void btbh_gpu_cost_abandon_frame(void) {
    btbh_gpu_cost_split_device = NULL;
    btbh_gpu_cost_pass_frame = false;
    btbh_gpu_cost_current_step = BTBH_GPU_COST_STEP_NONE;
}

void btbh_gpu_cost_begin_frame(SDL_GPUDevice *device) {
    btbh_gpu_cost_abandon_frame();
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
        if (btbh_gpu_cost_passes_enabled()) {
            /* Of the split frames about every other one is a pass frame, in the sequence {k sqrt(2)} >= 0.5 over
               them (another irrational, so that it follows neither the frames' parity nor their sequence). */
            btbh_gpu_cost_split_serial += 1u;
            btbh_gpu_cost_pass_frame =
                (btbh_gpu_cost_split_serial * UINT64_C(0x6A09E667F3BCC908)) >> 63 != 0u;
        }
        if (btbh_gpu_cost_pass_frame) {
            SDL_zeroa(btbh_gpu_cost_step_milliseconds);
            btbh_gpu_cost_pieces = 0u;
            btbh_gpu_cost_pieces_milliseconds = 0.0;
            btbh_gpu_cost_floor_milliseconds = btbh_gpu_cost_measure_floor(device);
        }
    }
}

/* In a split frame: submits `*command`, waits for it (its submit-to-fence time into `*milliseconds`, 0 when the submit
   failed) and acquires the next command buffer. False, with `*command` NULL, when the GPU failed either. */
static bool btbh_gpu_cost_submit_piece(SDL_GPUCommandBuffer **command, double *milliseconds) {
    SDL_GPUDevice *device = btbh_gpu_cost_split_device;
    *milliseconds = 0.0;
    const uint64_t submit_started_ticks = SDL_GetTicksNS();
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(*command);
    *command = NULL;
    if (fence == NULL) {
        return false;
    }
    (void)SDL_WaitForGPUFences(device, true, &fence, 1u);
    *milliseconds = (double)(SDL_GetTicksNS() - submit_started_ticks) / 1000000.0;
    SDL_ReleaseGPUFence(device, fence);
    btbh_gpu_cost_pieces += 1u;
    btbh_gpu_cost_pieces_milliseconds += *milliseconds;
    *command = SDL_AcquireGPUCommandBuffer(device);
    return *command != NULL;
}

bool btbh_gpu_cost_split(SDL_GPUCommandBuffer **command, BTBHGPUCostGroup finished_group) {
    if (btbh_gpu_cost_split_device == NULL) {
        return true;
    }
    double milliseconds = 0.0;
    const bool split = btbh_gpu_cost_submit_piece(command, &milliseconds);
    btbh_gpu_cost_group_milliseconds[finished_group] += milliseconds;
    if (btbh_gpu_cost_current_step != BTBH_GPU_COST_STEP_NONE) {
        /* A pass frame: the group's last step ends here. */
        btbh_gpu_cost_step_milliseconds[btbh_gpu_cost_current_step] +=
            milliseconds - btbh_gpu_cost_floor_milliseconds;
        btbh_gpu_cost_current_step = BTBH_GPU_COST_STEP_NONE;
    }
    return split;
}

bool btbh_gpu_cost_step(SDL_GPUCommandBuffer **command, BTBHGPUCostStep step, bool *submitted) {
    if (submitted != NULL) {
        *submitted = false;
    }
    if (btbh_gpu_cost_split_device == NULL ||
        !btbh_gpu_cost_pass_frame ||
        (int)step == btbh_gpu_cost_current_step) {
        return true;
    }
    if (btbh_gpu_cost_current_step == BTBH_GPU_COST_STEP_NONE) {
        /* A group's first step: it began at the group's split, with nothing recorded since. */
        btbh_gpu_cost_current_step = (int)step;
        return true;
    }
    double milliseconds = 0.0;
    const bool split = btbh_gpu_cost_submit_piece(command, &milliseconds);
    btbh_gpu_cost_step_milliseconds[btbh_gpu_cost_current_step] += milliseconds - btbh_gpu_cost_floor_milliseconds;
    btbh_gpu_cost_current_step = (int)step;
    if (submitted != NULL) {
        *submitted = true;
    }
    return split;
}

/* A pass frame's line (see `BTBHGPUCostStep`): gpu_ms and pieces count every piece, the canvas's last one too. */
static void btbh_gpu_cost_emit_passes(double canvas_milliseconds) {
    char steps[1024];
    int length = 0;
    steps[0] = '\0';
    for (uint32_t step = 0u; step < BTBH_GPU_COST_STEP_COUNT; ++step) {
        if (length >= 0 && (size_t)length < sizeof(steps)) {
            length += snprintf(
                steps + length,
                sizeof(steps) - (size_t)length,
                " %s_ms=%.4f",
                btbh_gpu_cost_step_names[step],
                btbh_gpu_cost_step_milliseconds[step]
            );
        }
    }
    (void)fprintf(
        stdout,
        "BTBH_GPU_COST_PASSES gpu_ms=%.4f pieces=%u floor_ms=%.4f%s\n",
        btbh_gpu_cost_pieces_milliseconds + canvas_milliseconds,
        btbh_gpu_cost_pieces + 1u,
        btbh_gpu_cost_floor_milliseconds,
        steps
    );
    (void)fflush(stdout);
}

void btbh_gpu_cost_emit_frame(double gpu_milliseconds) {
    btbh_gpu_cost_model_frames += 1u;
    if (btbh_gpu_cost_model_frames == BTBH_GPU_COST_MODEL_FRAMES) {
        btbh_gpu_cost_model_frames = 0u;
        btbh_gpu_cost_emit_models();
    }
    if (btbh_gpu_cost_split_device != NULL && btbh_gpu_cost_pass_frame) {
        /* A pass frame prints its own line instead of the frame's (its groups hold the extra pieces). */
        btbh_gpu_cost_emit_passes(gpu_milliseconds);
        btbh_gpu_cost_split_device = NULL;
        btbh_gpu_cost_pass_frame = false;
        btbh_gpu_cost_current_step = BTBH_GPU_COST_STEP_NONE;
        SDL_zeroa(btbh_gpu_cost_triangles);
        SDL_zeroa(btbh_gpu_cost_draws);
        btbh_gpu_cost_rc_emitters = 0u;
        return;
    }
    /* A split frame's last piece is the canvas's; its gpu_ms is the sum of the pieces. */
    char groups[320] = "split=0";
    if (btbh_gpu_cost_split_device != NULL) {
        btbh_gpu_cost_group_milliseconds[BTBH_GPU_COST_GROUP_UI] += gpu_milliseconds;
        gpu_milliseconds = 0.0;
        int length = snprintf(groups, sizeof(groups), "split=1");
        for (uint32_t group = 0u; group < BTBH_GPU_COST_GROUP_COUNT; ++group) {
            gpu_milliseconds += btbh_gpu_cost_group_milliseconds[group];
            if (group >= BTBH_GPU_COST_GROUP_VIEW1_MAIN && btbh_gpu_cost_group_milliseconds[group] == 0.0) {
                /* (RFC 0014's second view's groups: only on frames that drew one.) */
                continue;
            }
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

/* The triangles a pass has drawn since the last frame's line, the city's and the rest. */
static uint64_t btbh_gpu_cost_pass_triangles(BTBHGPUCostPass pass) {
    return btbh_gpu_cost_triangles[pass][0] + btbh_gpu_cost_triangles[pass][1];
}

/* With the cost log (since 0.16.0), a scene rendered into a target (btbh_gpu_window_scene_render_to_target and _view,
   which submit their own command buffer) is submitted with the GPU idle and waited for, and one `BTBH_GPU_COST_TARGET`
   line a call gives its GPU time (submit to fence, in one piece: never split), the view's id (`none` without a view),
   the target's and the render's sizes, and the triangles its main and shadow passes drew since `main_triangles` and
   `shadow_triangles` were read (they also count in the next frame's line, as before; that line's time doesn't hold
   the render's). It waits for the warming the last frame's line left, and warms nothing itself: the host frame's one
   warming (after its line) covers the host's own gap between frames, and what follows a render here is the next call's
   encoding, the same short gap as between a split frame's pieces (measured: a frame after a target render costs what
   one without does). False, with SDL's error set, when the submit failed. */
static bool btbh_gpu_cost_submit_target(
    SDL_GPUDevice *device,
    SDL_GPUCommandBuffer *command,
    const BTBHGPUSceneTargetView *view,
    const BTBHGPUTexture *target,
    uint32_t render_width,
    uint32_t render_height,
    uint64_t main_triangles,
    uint64_t shadow_triangles
) {
    btbh_gpu_cost_wait_warm();
    (void)SDL_WaitForGPUIdle(device);
    const uint64_t lazy_started_ticks = btbh_gpu_lazy_creation_clock();
    const uint64_t submit_started_ticks = SDL_GetTicksNS();
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
    btbh_gpu_lazy_creation_call_timed(BTBH_GPU_LAZY_CALL_TARGET_SUBMIT, lazy_started_ticks);
    if (fence == NULL) {
        return false;
    }
    (void)SDL_WaitForGPUFences(device, true, &fence, 1u);
    const double gpu_milliseconds = (double)(SDL_GetTicksNS() - submit_started_ticks) / 1000000.0;
    SDL_ReleaseGPUFence(device, fence);
    char view_id[16] = "none";
    if (view != NULL) {
        (void)snprintf(view_id, sizeof(view_id), "%u", view->view_id);
    }
    (void)fprintf(
        stdout,
        "BTBH_GPU_COST_TARGET gpu_ms=%.4f view=%s target=%ux%u render=%ux%u main_tris=%llu shadow_tris=%llu\n",
        gpu_milliseconds,
        view_id,
        target->width,
        target->height,
        render_width,
        render_height,
        (unsigned long long)(btbh_gpu_cost_pass_triangles(BTBH_GPU_COST_PASS_MAIN) - main_triangles),
        (unsigned long long)(btbh_gpu_cost_pass_triangles(BTBH_GPU_COST_PASS_SHADOW) - shadow_triangles)
    );
    (void)fflush(stdout);
    return true;
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
    /* (RFC 0014: and a second view's own maps.) */
    (void)memset(runtime->view_retained_shadow_ready, 0, sizeof(runtime->view_retained_shadow_ready));
    (void)memset(runtime->view_retained_shadow_active, 0, sizeof(runtime->view_retained_shadow_active));
}

static void btbh_gpu_scene_cancel_submission(
    BTBHGPUWindow *window,
    SDL_GPUCommandBuffer *command
) {
    if (command != NULL) {
        (void)SDL_CancelGPUCommandBuffer(command);
    }
    btbh_gpu_cost_abandon_frame();
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

/* `btbh_gpu_cost_step` for the scene, the same way. */
static bool btbh_gpu_scene_cost_step(
    BTBHGPUWindow *window,
    SDL_GPUCommandBuffer **command,
    BTBHGPUCostStep step,
    BTBHGPUSceneReport *report
) {
    if (btbh_gpu_cost_step(command, step, NULL)) {
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
    const uint64_t load_started_ticks = btbh_gpu_lazy_creation_clock();
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
    SDL_GPUShader *shader = btbh_gpu_create_shader_from(device, &info, path, load_started_ticks, NULL);
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
    /* production_model_unlit.frag samples no world detail (RFC 0008) and no table (RFC 0009): a batch drawing one
       takes the full shader. */
    return batch != NULL &&
        batch->unlit_enabled != 0u &&
        batch->surface_variant == 0u &&
        !btbh_gpu_world_detail_active(batch) &&
        !btbh_gpu_world_detail_table_active(batch) &&
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
    bool lifted,
    uint32_t fragment_sampler_count,
    uint32_t fragment_uniform_buffer_count,
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
        /* Instances, the bone palette when animated, and the instances' motion; RFC 0012: a lifted static shader's
           heightfield (one sampler) and its block (a second uniform buffer). */
        animated ? 3u : 2u,
        lifted ? 1u : 0u,
        lifted ? 2u : 1u,
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
        /* the material, an SDF terrain's dig mask (RFC 0007) with the world detail (RFC 0008), and the world-detail
           table (RFC 0009; not in the fast unlit shader) */
        fragment_uniform_buffer_count,
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
    if (!animated) {
        /* RFC 0009: the joint slot's first float, the world-detail table index, which only
           production_model_detail_table.vert (a table batch's) declares. Every static pipeline describes it:
           production_model.vert, and a static vertex shader from before 0.8.0, leave it unread, which costs
           nothing (measured). */
        attributes[5].location = 5u;
        attributes[5].buffer_slot = 0u;
        attributes[5].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT;
        attributes[5].offset = BTBH_GPU_MODEL_JOINTS_OFFSET;
    }
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
    info.vertex_input_state.num_vertex_attributes = animated ? 7u : 6u;
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
    *pipeline = btbh_gpu_create_graphics_pipeline(device, &info);
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
        btbh_gpu_create_graphics_pipeline(device, &info);
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

/* blend: how the pass's output is blended into its target (NULL: it replaces it). vertex_uniform_count: the vertex
   shader's uniform blocks (0 for production_post.vert; 1 for RFC 0014's production_post_view.vert). */
static bool btbh_gpu_scene_create_fullscreen_pipeline_staged(
    BTBHGPUWindow *window,
    const char *vertex_shader_path,
    const char *fragment_shader_path,
    BTBHGPUShaderFormat project_format,
    SDL_GPUTextureFormat target_format,
    uint32_t fragment_sampler_count,
    const SDL_GPUColorTargetBlendState *blend,
    uint32_t vertex_uniform_count,
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
        vertex_uniform_count,
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
    if (blend != NULL) {
        color_target.blend_state = *blend;
    } else {
        color_target.blend_state.color_write_mask = 0x0fu;
    }
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
    *pipeline = btbh_gpu_create_graphics_pipeline(device, &info);
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

static bool btbh_gpu_scene_create_fullscreen_pipeline_blended(
    BTBHGPUWindow *window,
    const char *vertex_shader_path,
    const char *fragment_shader_path,
    BTBHGPUShaderFormat project_format,
    SDL_GPUTextureFormat target_format,
    uint32_t fragment_sampler_count,
    const SDL_GPUColorTargetBlendState *blend,
    SDL_GPUGraphicsPipeline **pipeline,
    BTBHGPUSceneReport *report
) {
    return btbh_gpu_scene_create_fullscreen_pipeline_staged(
        window,
        vertex_shader_path,
        fragment_shader_path,
        project_format,
        target_format,
        fragment_sampler_count,
        blend,
        0u,
        pipeline,
        report
    );
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
    return btbh_gpu_scene_create_fullscreen_pipeline_blended(
        window,
        vertex_shader_path,
        fragment_shader_path,
        project_format,
        target_format,
        fragment_sampler_count,
        NULL,
        pipeline,
        report
    );
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

static void btbh_gpu_scene_release_hbao_pipelines(
    BTBHGPUSceneRuntime *runtime,
    SDL_GPUDevice *device
) {
    SDL_GPUGraphicsPipeline **pipelines[] = {
        &runtime->hbao_pipeline,
        &runtime->hbao_denoise_pipeline,
        &runtime->hbao_resolve_pipeline
    };
    for (size_t index = 0u;
         index < sizeof(pipelines) / sizeof(pipelines[0]);
         ++index) {
        if (*pipelines[index] != NULL) {
            SDL_ReleaseGPUGraphicsPipeline(device, *pipelines[index]);
            *pipelines[index] = NULL;
        }
    }
}

/* The HBAO resolve's blend: target = src.rgb + target * src.a, its alpha kept. */
static SDL_GPUColorTargetBlendState btbh_gpu_scene_hbao_over_blend(void) {
    SDL_GPUColorTargetBlendState over;
    SDL_zero(over);
    over.enable_blend = true;
    over.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    over.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    over.color_blend_op = SDL_GPU_BLENDOP_ADD;
    over.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
    over.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    over.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    over.color_write_mask =
        SDL_GPU_COLORCOMPONENT_R |
        SDL_GPU_COLORCOMPONENT_G |
        SDL_GPU_COLORCOMPONENT_B;
    over.enable_color_write_mask = true;
    return over;
}

/* The horizon-based ambient occlusion's three passes (ssao_enabled 2): the occlusion and its denoise at half
   the resolution, and the resolve laid over the opaque scene (target = src.rgb + target * src.a, its alpha
   kept). */
static bool btbh_gpu_scene_ensure_hbao_pipelines(
    BTBHGPUWindow *window,
    const char *post_vertex_shader_path,
    const char *post_fragment_shader_path,
    BTBHGPUShaderFormat shader_format,
    BTBHGPUSceneReport *report
) {
    BTBHGPUSceneRuntime *runtime = window->scene_runtime;
    if (runtime->hbao_pipeline != NULL &&
        runtime->hbao_denoise_pipeline != NULL &&
        runtime->hbao_resolve_pipeline != NULL) {
        return true;
    }
    SDL_GPUDevice *device = window->device->gpu;
    btbh_gpu_scene_release_hbao_pipelines(runtime, device);
    const SDL_GPUColorTargetBlendState over = btbh_gpu_scene_hbao_over_blend();
    const char *const names[3] = {
        "production_hbao.frag",
        "production_hbao_denoise.frag",
        "production_hbao_resolve.frag"
    };
    const SDL_GPUTextureFormat formats[3] = {
        SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT,
        SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT,
        runtime->scene_color_format
    };
    const uint32_t sampler_counts[3] = { 2u, 1u, 6u };
    SDL_GPUGraphicsPipeline **pipelines[3] = {
        &runtime->hbao_pipeline,
        &runtime->hbao_denoise_pipeline,
        &runtime->hbao_resolve_pipeline
    };
    const char *extension =
        shader_format == BTBH_GPU_SHADER_FORMAT_METALLIB
            ? "metallib"
            : "spv";
    for (uint32_t index = 0u; index < 3u; ++index) {
        char path[BTBH_GPU_SCENE_PATH_CAPACITY];
        char filename[128];
        (void)snprintf(
            filename,
            sizeof(filename),
            "%s.%s",
            names[index],
            extension
        );
        if (!btbh_gpu_scene_sibling_path(
                post_fragment_shader_path,
                filename,
                path
            ) ||
            !btbh_gpu_scene_create_fullscreen_pipeline_blended(
                window,
                post_vertex_shader_path,
                path,
                shader_format,
                formats[index],
                sampler_counts[index],
                index == 2u ? &over : NULL,
                pipelines[index],
                report
            )) {
            btbh_gpu_scene_release_hbao_pipelines(runtime, device);
            return false;
        }
    }
    return true;
}

static void btbh_gpu_scene_release_hbao_targets(
    BTBHGPUSceneRuntime *runtime,
    SDL_GPUDevice *device
) {
    if (runtime->hbao_texture != NULL) {
        SDL_ReleaseGPUTexture(device, runtime->hbao_texture);
        runtime->hbao_texture = NULL;
    }
    if (runtime->hbao_denoised_texture != NULL) {
        SDL_ReleaseGPUTexture(device, runtime->hbao_denoised_texture);
        runtime->hbao_denoised_texture = NULL;
    }
    runtime->hbao_width = 0u;
    runtime->hbao_height = 0u;
}

/* The horizon-based occlusion's two targets, at half the render resolution: the visibility and the view depth. */
static bool btbh_gpu_scene_ensure_hbao_targets(
    BTBHGPUWindow *window,
    uint32_t width,
    uint32_t height,
    BTBHGPUSceneReport *report
) {
    BTBHGPUSceneRuntime *runtime = window->scene_runtime;
    SDL_GPUDevice *device = window->device->gpu;
    const uint32_t half_width = (width + 1u) / 2u;
    const uint32_t half_height = (height + 1u) / 2u;
    if (runtime->hbao_texture != NULL &&
        runtime->hbao_denoised_texture != NULL &&
        runtime->hbao_width == half_width &&
        runtime->hbao_height == half_height) {
        return true;
    }
    btbh_gpu_scene_release_hbao_targets(runtime, device);
    SDL_GPUTextureCreateInfo info;
    SDL_zero(info);
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT;
    info.usage =
        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
        SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = half_width;
    info.height = half_height;
    info.layer_count_or_depth = 1u;
    info.num_levels = 1u;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    runtime->hbao_texture = SDL_CreateGPUTexture(device, &info);
    runtime->hbao_denoised_texture = SDL_CreateGPUTexture(device, &info);
    if (runtime->hbao_texture == NULL ||
        runtime->hbao_denoised_texture == NULL) {
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        btbh_gpu_scene_release_hbao_targets(runtime, device);
        return false;
    }
    runtime->hbao_width = half_width;
    runtime->hbao_height = half_height;
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

/* RFC 0013: lit particles read the scene's distance (their soft fade and their fog's make-up), so the distance pass
   runs on frames that draw one. */
static bool btbh_gpu_scene_batches_have_lit_particles(
    const BTBHGPUSceneGeometryBatch *batches,
    uint32_t batch_count
) {
    for (uint32_t index = 0u; index < batch_count; ++index) {
        if (btbh_gpu_particle_lit(&batches[index])) {
            return true;
        }
    }
    return false;
}

/* RFC 0013 phase 2: a batch draws into the particle transmittance map (with particle_shadow set and the sun's shadow
   on, the map is drawn this frame). */
static bool btbh_gpu_scene_batches_have_particle_shadow(
    const BTBHGPUSceneGeometryBatch *batches,
    uint32_t batch_count
) {
    for (uint32_t index = 0u; index < batch_count; ++index) {
        if (btbh_gpu_particle_shadow(&batches[index])) {
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

/* Whether a batch draws with the fast unlit shader (production_model_unlit.frag, btbh_gpu_scene_uses_fast_unlit_pipeline:
   a transparent unlit batch): without the radiance cascades, and under them with cascades_fast_unlit
   (BTBH_GPU_SCENE_POST_ADDITIVE_CASCADES_KEEP_HBAO under additive cascades: since 0.10.0 with the HBAO, since 0.11.0
   with any ambient occlusion or none, BTBHGPUSceneFrameSetup.cascades_fast_unlit). The fast shader takes no point
   light, no dark scene and no sun shadow; the cascades' scene shader does, so under the flag the transparent pass draws
   as without the cascades (Be The Bullet Hell v2's additive night overlays by its neon point lights came out up to 200
   levels brighter). Cascades without the flag keep the scene shader, as before; so does, under the flag, a batch with
   BTBH_GPU_SCENE_BATCH_FLAGS2_KEEP_CASCADES_LOOK (since 0.12.0: v2's beams and glows keep the cascades' look). */
static bool btbh_gpu_scene_batch_fast_unlit(
    const BTBHGPUSceneGeometryBatch *batch,
    bool radiance_cascades_active,
    bool cascades_fast_unlit
) {
    return (!radiance_cascades_active ||
            (cascades_fast_unlit &&
             (batch->flags2 & BTBH_GPU_SCENE_BATCH_FLAGS2_KEEP_CASCADES_LOOK) == 0u)) &&
        btbh_gpu_scene_uses_fast_unlit_pipeline(batch);
}

/* BTBH_GPU_SCENE_BATCH_FLAGS2_KEEP_CASCADES_LOOK where it changes something: a batch the fast unlit shader would draw
   under KEEP_HBAO's transparent part (cascades_fast_unlit) draws with the cascades' scene shader instead, and with the
   fragment block it has under the cascades without KEEP_HBAO (the flag's scene-target bits cleared). */
static bool btbh_gpu_scene_batch_keeps_cascades_look(
    const BTBHGPUSceneGeometryBatch *batch,
    bool cascades_fast_unlit
) {
    return cascades_fast_unlit &&
        (batch->flags2 & BTBH_GPU_SCENE_BATCH_FLAGS2_KEEP_CASCADES_LOOK) != 0u &&
        btbh_gpu_scene_uses_fast_unlit_pipeline(batch);
}

static SDL_GPUGraphicsPipeline **btbh_gpu_scene_pipeline_slot(
    BTBHGPUSceneRuntime *runtime,
    bool auxiliary_geometry_outputs,
    bool temporal,
    bool radiance_cascades_active,
    bool cascades_fast_unlit,
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
    const uint32_t fast_unlit = btbh_gpu_scene_batch_fast_unlit(
        batch,
        radiance_cascades_active,
        cascades_fast_unlit
    ) ? 1u : 0u;
    const uint32_t color_target_variant =
        (temporal ? 4u : 0u) +
        (radiance_cascades_active ? 2u : 0u) +
        (auxiliary_geometry_outputs ? 1u : 0u);
    if (!animated && btbh_gpu_particle_lit(batch)) {
        /* RFC 0013: a lit particle's pipeline (always transparent, straight alpha, never lifted or with a table). */
        const uint32_t shadow_set = !shadows_active ? 2u : (omni_shadows_active ? 0u : 1u);
        return &runtime->lit_particle_pipelines[shadow_set][temporal ? 1u : 0u][depth][cull][stencil];
    }
    if (!animated && btbh_gpu_height_lift_active(batch)) {
        /* RFC 0012: production_model_lift.vert's and production_model_detail_table_lift.vert's pipelines; NULL until
           the first lifted batch made their sets (btbh_gpu_scene_ensure_model_pipeline). */
        if (runtime->lift_pipelines == NULL) {
            return NULL;
        }
        const uint32_t table = btbh_gpu_world_detail_table_active(batch) ? 1u : 0u;
        const uint32_t shadow_set = !shadows_active ? 2u : (omni_shadows_active ? 0u : 1u);
        return &runtime->lift_pipelines->sets[table][shadow_set]
            [color_target_variant][blend][depth][transparent]
            [cull][stencil][fast_unlit];
    }
    if (!animated && btbh_gpu_world_detail_table_active(batch)) {
        /* RFC 0009: production_model_detail_table.vert's pipelines (never fast unlit). */
        if (!shadows_active) {
            return &runtime->unshadowed_table_model_pipelines
                [color_target_variant][blend][depth][transparent]
                [cull][stencil][fast_unlit];
        }
        return omni_shadows_active
            ? &runtime->table_model_pipelines
                [color_target_variant][blend][depth][transparent]
                [cull][stencil][fast_unlit]
            : &runtime->non_omni_shadowed_table_model_pipelines
                [color_target_variant][blend][depth][transparent]
                [cull][stencil][fast_unlit];
    }
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

/* The scene fragment shader a batch that doesn't take the fast unlit pipeline draws with: the host's fragment shader
   (the sun's and the omni shadow), its production_model_2d_shadows sibling (the sun's only) or the host's unshadowed
   one; with the radiance cascades, the production_model_rc* sibling of that. False with the report set when a path
   is too long. */
static bool btbh_gpu_scene_variant_fragment_shader_path(
    const char *fragment_shader_path,
    const char *unshadowed_fragment_shader_path,
    BTBHGPUShaderFormat project_format,
    bool shadows_active,
    bool omni_shadows_active,
    bool radiance_cascades_active,
    char path[BTBH_GPU_SCENE_PATH_CAPACITY],
    BTBHGPUSceneReport *report
) {
    const char *extension =
        project_format == BTBH_GPU_SHADER_FORMAT_METALLIB
            ? "metallib"
            : "spv";
    const char *selected_fragment_shader_path =
        shadows_active
            ? fragment_shader_path
            : unshadowed_fragment_shader_path;
    char non_omni_fragment_shader_path[
        BTBH_GPU_SCENE_PATH_CAPACITY
    ];
    if (shadows_active && !omni_shadows_active) {
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
    if (radiance_cascades_active) {
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
                path
            )) {
            btbh_gpu_scene_error(
                report,
                BTBH_GPU_INVALID_ARGUMENT,
                "RC material fragment shader path is too long"
            );
            return false;
        }
        return true;
    }
    const size_t length = strlen(selected_fragment_shader_path);
    if (length + 1u > BTBH_GPU_SCENE_PATH_CAPACITY) {
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            "Fragment shader path is too long"
        );
        return false;
    }
    (void)memcpy(path, selected_fragment_shader_path, length + 1u);
    return true;
}

/* RFC 0012's name check: a lifted batch needs production_model_lift.vert (or production_model_detail_table_lift.vert),
   files new in 0.11.0, beside the host's static vertex shader. A missing one fails the present with its path; a
   SPIR-V one must name the heightfield (height_lift_field: SPIR-V keeps its resources' names), so that a file of
   that name that doesn't lift isn't drawn with. Read when a lifted pipeline is made (once per pipeline). */
static bool btbh_gpu_scene_check_lift_shader(
    const char *path,
    BTBHGPUShaderFormat project_format,
    BTBHGPUSceneReport *report
) {
    size_t byte_count = 0u;
    uint8_t *bytes = (uint8_t *)SDL_LoadFile(path, &byte_count);
    char message[sizeof(report->message)];
    if (bytes == NULL || byte_count == 0u) {
        SDL_free(bytes);
        (void)snprintf(
            message,
            sizeof(message),
            "A batch has a height_lift, and its vertex shader is missing (build the shaders from 0.11.0 or later): %s",
            path
        );
        btbh_gpu_scene_error(report, BTBH_GPU_INVALID_ARGUMENT, message);
        return false;
    }
    static const char name[] = "height_lift_field";
    bool named = project_format != BTBH_GPU_SHADER_FORMAT_SPIRV;
    for (size_t offset = 0u; !named && offset + sizeof(name) - 1u <= byte_count; ++offset) {
        named = memcmp(bytes + offset, name, sizeof(name) - 1u) == 0;
    }
    SDL_free(bytes);
    if (!named) {
        (void)snprintf(
            message,
            sizeof(message),
            "A batch has a height_lift, and its vertex shader doesn't read a heightfield (build the shaders from "
            "0.11.0 or later): %s",
            path
        );
        btbh_gpu_scene_error(report, BTBH_GPU_INVALID_ARGUMENT, message);
        return false;
    }
    return true;
}

/* RFC 0013's name check: a lit particle needs production_particle_lit.frag (or its _2d_shadows or _unshadowed
   sibling), files new in 0.11.0, beside the host's fragment shader. A missing one fails the present with its path;
   a SPIR-V one must name the scene's distance (particle_scene_distance), so that a file of that name that isn't the
   lit particle isn't drawn with. Read when its pipeline is made (once per pipeline). */
static bool btbh_gpu_scene_check_particle_shader(
    const char *path,
    BTBHGPUShaderFormat project_format,
    BTBHGPUSceneReport *report
) {
    size_t byte_count = 0u;
    uint8_t *bytes = (uint8_t *)SDL_LoadFile(path, &byte_count);
    char message[sizeof(report->message)];
    if (bytes == NULL || byte_count == 0u) {
        SDL_free(bytes);
        (void)snprintf(
            message,
            sizeof(message),
            "A lit particle's fragment shader is missing (build the shaders from 0.11.0 or later): %s",
            path
        );
        btbh_gpu_scene_error(report, BTBH_GPU_INVALID_ARGUMENT, message);
        return false;
    }
    static const char name[] = "particle_scene_distance";
    bool named = project_format != BTBH_GPU_SHADER_FORMAT_SPIRV;
    for (size_t offset = 0u; !named && offset + sizeof(name) - 1u <= byte_count; ++offset) {
        named = memcmp(bytes + offset, name, sizeof(name) - 1u) == 0;
    }
    SDL_free(bytes);
    if (!named) {
        (void)snprintf(
            message,
            sizeof(message),
            "A lit particle's fragment shader doesn't draw lit particles (build the shaders from 0.11.0 or later): %s",
            path
        );
        btbh_gpu_scene_error(report, BTBH_GPU_INVALID_ARGUMENT, message);
        return false;
    }
    return true;
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
    bool cascades_fast_unlit,
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
        cascades_fast_unlit,
        shadows_active,
        omni_shadows_active,
        animated,
        batch,
        stencil_variant
    );
    if (slot != NULL && *slot != NULL) {
        return true;
    }
    if (!animated && btbh_gpu_particle_lit(batch)) {
        /* RFC 0013: production_particle_lit.frag (its _2d_shadows or _unshadowed sibling by the frame's shadows),
           beside the host's fragment shader, after the host's static vertex shader. Its textures: the albedo, the
           normal, the scene's distance, the emission, the cascades' atlas, then the sun's (comparison), the fill's
           and the spot's maps and the omni cube as the variant has them; its uniforms the material and the frame's
           block. */
        char particle_path[BTBH_GPU_SCENE_PATH_CAPACITY];
        char filename[128];
        (void)snprintf(
            filename,
            sizeof(filename),
            !shadows_active
                ? "production_particle_lit_unshadowed.frag.%s"
                : (omni_shadows_active
                    ? "production_particle_lit.frag.%s"
                    : "production_particle_lit_2d_shadows.frag.%s"),
            project_format == BTBH_GPU_SHADER_FORMAT_METALLIB ? "metallib" : "spv"
        );
        if (!btbh_gpu_scene_sibling_path(fragment_shader_path, filename, particle_path)) {
            btbh_gpu_scene_error(report, BTBH_GPU_INVALID_ARGUMENT, "Lit particle fragment shader path is too long");
            return false;
        }
        if (!btbh_gpu_scene_check_particle_shader(particle_path, project_format, report)) {
            return false;
        }
        return btbh_gpu_scene_create_model_pipeline(
            window,
            vertex_shader_path,
            particle_path,
            project_format,
            false,
            false,
            /* (RFC 0013 phase 2: the shadowed variants' last, the particle transmittance map.) */
            !shadows_active ? 5u : (omni_shadows_active ? 10u : 9u),
            2u,
            /* Transparent: the pass's own targets, with the auxiliary outputs or without. */
            false,
            temporal,
            batch,
            stencil_variant,
            slot,
            report
        );
    }
    const bool lifted = !animated && btbh_gpu_height_lift_active(batch);
    if (slot == NULL) {
        /* RFC 0012: the first lifted batch makes the lifted pipelines' sets (only a lifted batch finds none). */
        window->scene_runtime->lift_pipelines =
            (BTBHGPUSceneLiftPipelines *)calloc(1u, sizeof(BTBHGPUSceneLiftPipelines));
        if (window->scene_runtime->lift_pipelines == NULL) {
            btbh_gpu_scene_error(
                report,
                BTBH_GPU_PLATFORM_FAILURE,
                "Could not allocate the lifted batches' pipelines"
            );
            return false;
        }
        slot = btbh_gpu_scene_pipeline_slot(
            window->scene_runtime,
            auxiliary_geometry_outputs,
            temporal,
            radiance_cascades_active,
            cascades_fast_unlit,
            shadows_active,
            omni_shadows_active,
            animated,
            batch,
            stencil_variant
        );
    }
    const bool fast_unlit = btbh_gpu_scene_batch_fast_unlit(
        batch,
        radiance_cascades_active,
        cascades_fast_unlit
    );
    char variant_fragment_shader_path[
        BTBH_GPU_SCENE_PATH_CAPACITY
    ];
    if (!btbh_gpu_scene_variant_fragment_shader_path(
            fragment_shader_path,
            unshadowed_fragment_shader_path,
            project_format,
            shadows_active,
            omni_shadows_active,
            radiance_cascades_active,
            variant_fragment_shader_path,
            report
        )) {
        return false;
    }
    const char *selected_fragment_shader_path =
        variant_fragment_shader_path;
    char fast_unlit_fragment_shader_path[
        BTBH_GPU_SCENE_PATH_CAPACITY
    ];
    if (fast_unlit) {
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
    /* RFC 0009: a static batch with a world-detail table reads its vertices' table index with
       production_model_detail_table.vert, beside the host's production_model.vert; a host without that file
       can't draw a table (the shader load fails with its path). RFC 0012: a lifted static batch draws with
       production_model_lift.vert, or production_model_detail_table_lift.vert with a table, from there too; a host
       without the file can't draw a lift (btbh_gpu_scene_check_lift_shader names it). */
    const char *selected_vertex_shader_path =
        animated ? animated_vertex_shader_path : vertex_shader_path;
    char table_vertex_shader_path[
        BTBH_GPU_SCENE_PATH_CAPACITY
    ];
    if (lifted || (!animated && btbh_gpu_world_detail_table_active(batch))) {
        char filename[128];
        (void)snprintf(
            filename,
            sizeof(filename),
            lifted
                ? (btbh_gpu_world_detail_table_active(batch)
                    ? "production_model_detail_table_lift.vert.%s"
                    : "production_model_lift.vert.%s")
                : "production_model_detail_table.vert.%s",
            project_format == BTBH_GPU_SHADER_FORMAT_METALLIB
                ? "metallib"
                : "spv"
        );
        if (!btbh_gpu_scene_sibling_path(
                vertex_shader_path,
                filename,
                table_vertex_shader_path
            )) {
            btbh_gpu_scene_error(
                report,
                BTBH_GPU_INVALID_ARGUMENT,
                lifted
                    ? "Lifted vertex shader path is too long"
                    : "World-detail table vertex shader path is too long"
            );
            return false;
        }
        selected_vertex_shader_path = table_vertex_shader_path;
    }
    if (lifted && !btbh_gpu_scene_check_lift_shader(selected_vertex_shader_path, project_format, report)) {
        return false;
    }
    return btbh_gpu_scene_create_model_pipeline(
        window,
        selected_vertex_shader_path,
        selected_fragment_shader_path,
        project_format,
        animated,
        lifted,
        /* Each variant's textures, then an SDF terrain's dig mask (RFC 0007), then the world detail's three
           (RFC 0008) and the world-detail table's array (RFC 0009), then in the shadowed variants the particle
           transmittance map (RFC 0013 phase 2: the full variant's 16th and last); the last five not in the fast unlit
           shader. */
        fast_unlit
            ? 3u
            : (shadows_active
                ? (omni_shadows_active ? 16u : 15u)
                : 10u),
        fast_unlit ? 2u : 3u,
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
    /* Its texture and uniforms are an SDF terrain's dig mask (RFC 0007). */
    SDL_GPUShader *shadow_fragment_shader =
        btbh_gpu_scene_load_shader(
            device,
            shadow_fragment_shader_path,
            entrypoint,
            format,
            SDL_GPU_SHADERSTAGE_FRAGMENT,
            0u,
            1u,
            1u,
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
            btbh_gpu_create_graphics_pipeline(device, &info);
        info.fragment_shader = fast_shadow_fragment_shader;
        window->scene_runtime
            ->non_discard_shadow_pipelines[cull_index] =
                btbh_gpu_create_graphics_pipeline(device, &info);
        info.vertex_shader = fast_shadow_vertex_shader;
        info.vertex_input_state.num_vertex_buffers = 2u;
        info.vertex_input_state.vertex_attributes =
            fast_shadow_attributes;
        info.vertex_input_state.num_vertex_attributes = 5u;
        window->scene_runtime->fast_shadow_pipelines[cull_index] =
            btbh_gpu_create_graphics_pipeline(device, &info);
        info.vertex_shader = animated_shadow_vertex_shader;
        info.fragment_shader = shadow_fragment_shader;
        info.vertex_input_state.num_vertex_buffers = 1u;
        info.vertex_input_state.vertex_attributes = shadow_attributes;
        info.vertex_input_state.num_vertex_attributes = 3u;
        window->scene_runtime
            ->animated_shadow_pipelines[cull_index] =
                btbh_gpu_create_graphics_pipeline(device, &info);
        info.fragment_shader = fast_shadow_fragment_shader;
        window->scene_runtime
            ->animated_non_discard_shadow_pipelines[cull_index] =
                btbh_gpu_create_graphics_pipeline(device, &info);
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
    const uint8_t *const fallback_layers[1] = { white_pixel };
    window->scene_runtime->fallback_array_texture =
        btbh_gpu_window_texture_array_create_rgba8_ex(
            window,
            fallback_layers,
            1u,
            1u,
            1u,
            0u,
            BTBH_GPU_TEXTURE_FILTER_NEAREST,
            BTBH_GPU_TEXTURE_WRAP_CLAMP,
            BTBH_GPU_TEXTURE_COLOR_SPACE_LINEAR,
            &texture_report
        );
    if (window->scene_runtime->fallback_array_texture == NULL) {
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

bool btbh_gpu_scene_detail_parameters_valid(const float detail_parameters[4]) {
    const float pattern = detail_parameters[3];
    if (!isfinite(detail_parameters[0]) ||
        !isfinite(detail_parameters[1]) ||
        !isfinite(detail_parameters[2]) ||
        !isfinite(pattern) ||
        pattern < 0.0f ||
        pattern > (float)BTBH_GPU_SCENE_DETAIL_FACADE_WEATHER) {
        return false;
    }
    if (pattern < 0.5f) {
        return true;
    }
    if (pattern < 6.5f) {
        /* Ground, grass, facade, paving, glass, a lamp's lens: noise cells (or panes) per render unit. */
        return detail_parameters[2] > 0.0f;
    }
    /* The facade's weather: x, y and z mean nothing yet, so they stay free for later. */
    return detail_parameters[0] == 0.0f &&
        detail_parameters[1] == 0.0f &&
        detail_parameters[2] == 0.0f;
}

/* check_instances false (btbh_gpu_window_scene_prewarm, which reads no instances): the instance ranges go
   unchecked. */
static bool btbh_gpu_scene_validate_batches(
    BTBHGPUWindow *window,
    uint32_t transient_instance_count,
    const BTBHGPUSceneGeometryBatch *batches,
    const uint8_t *scene_visibility,
    const uint8_t *directional_shadow_visibility,
    uint32_t batch_count,
    bool check_instances
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
            batch->reactive_mode < BTBH_GPU_SCENE_REACTIVE_COUNT &&
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
            batch->ssao_mode < BTBH_GPU_SCENE_SSAO_MODE_COUNT &&
            (batch->flags & ~(uint32_t)(
                BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW |
                BTBH_GPU_SCENE_BATCH_KEEP_ORDER |
                BTBH_GPU_SCENE_BATCH_NO_RC_SOURCE |
                BTBH_GPU_SCENE_BATCH_INSTANCE_UV_RECT |
                BTBH_GPU_SCENE_BATCH_PARTICLE_SHADOW |
                BTBH_GPU_SCENE_BATCH_RECEIVES_NO_SPOT_SHADOW |
                BTBH_GPU_SCENE_BATCH_INSTANCE_LIGHT_FACTOR
            )) == 0u &&
            (batch->flags2 & ~(uint32_t)BTBH_GPU_SCENE_BATCH_FLAGS2_KEEP_CASCADES_LOOK) == 0u &&
            btbh_gpu_scene_detail_parameters_valid(batch->detail_parameters) &&
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
            (check_instances &&
             (batch->instance_count == 0u ||
              batch->first_instance > available_instance_count ||
              batch->instance_count >
                  available_instance_count - batch->first_instance))) {
            return false;
        }
    }
    return true;
}

static bool btbh_gpu_scene_texture_usable(const BTBHGPUWindow *window, const BTBHGPUTexture *texture) {
    return texture == NULL ||
        (texture->owner == window && texture->texture != NULL && texture->sampler != NULL);
}

static void btbh_gpu_scene_refuse_batch(
    BTBHGPUSceneReport *report,
    uint32_t batch_index,
    const char *what,
    const char *problem
) {
    char message[sizeof(report->message)];
    (void)snprintf(message, sizeof(message), "GPU scene batch %u: %s: %s", batch_index, what, problem);
    btbh_gpu_scene_error(report, BTBH_GPU_INVALID_ARGUMENT, message);
}

/* RFC 0008: each batch's world detail, and RFC 0009: its world-detail table, and no array texture in a 2D slot.
   RFC 0012: its height lift, and no heightfield in any other slot. RFC 0013: what a lit particle and a batch with the
   UV rect may be; and (0.11.0) one with the light factor; and (phase 2, 0.14.0) one with the particle shadow. Batches
   often share a detail, a table or a lift, so a run of batches pointing at the same one checks it once.
   directional_shadow_visibility: the _with_visibility calls' array, or NULL. views (RFC 0014, view_count of them; NULL
   with 0): a lifted batch's byte in a view's shadow_visibility, which picks the casters of that view's sun map, follows
   the rule directional_shadow_visibility's does. False with the report naming the batch and the fault. */
static bool btbh_gpu_scene_validate_world_details(
    const BTBHGPUWindow *window,
    const BTBHGPUSceneGeometryBatch *batches,
    const uint8_t *directional_shadow_visibility,
    const BTBHGPUSceneView *views,
    uint32_t view_count,
    uint32_t batch_count,
    BTBHGPUSceneReport *report
) {
    static const char array_in_2d_slot[] =
        "is a 2D array texture (only world_detail_table.map_layers takes one)";
    static const char heightfield_in_slot[] =
        "is a heightfield (only height_lift.heightfield takes one)";
    const BTBHGPUSceneWorldDetail *checked = NULL;
    const BTBHGPUSceneWorldDetailTable *checked_table = NULL;
    const BTBHGPUSceneHeightLift *checked_lift = NULL;
    for (uint32_t index = 0u; index < batch_count; ++index) {
        const BTBHGPUSceneGeometryBatch *batch = &batches[index];
        const char *const slot_names[4] = { "albedo_texture", "normal_texture", "orm_texture", "emission_texture" };
        const BTBHGPUTexture *const slots[4] = {
            batch->albedo_texture, batch->normal_texture, batch->orm_texture, batch->emission_texture
        };
        for (uint32_t slot = 0u; slot < 4u; ++slot) {
            /* (One test for both kinds a 2D slot refuses: this loop runs for every batch of every present.) */
            const BTBHGPUTexture *texture = slots[slot];
            if (texture != NULL && (texture->layer_count != 0u || texture->heights != NULL)) {
                btbh_gpu_scene_refuse_batch(report, index, slot_names[slot],
                    texture->layer_count != 0u ? array_in_2d_slot : heightfield_in_slot);
                return false;
            }
        }
        if (btbh_gpu_particle_lit(batch)) {
            const char *problem = btbh_gpu_particle_lit_problem(batch);
            if (problem != NULL) {
                btbh_gpu_scene_refuse_batch(report, index, "surface_variant LIT_PARTICLE", problem);
                return false;
            }
        }
        if (btbh_gpu_particle_uv_rect(batch)) {
            const char *problem = btbh_gpu_particle_uv_rect_problem(batch);
            if (problem != NULL) {
                btbh_gpu_scene_refuse_batch(report, index, "flags INSTANCE_UV_RECT", problem);
                return false;
            }
        }
        if (btbh_gpu_scene_light_factor(batch)) {
            const char *problem = btbh_gpu_scene_light_factor_problem(batch);
            if (problem != NULL) {
                btbh_gpu_scene_refuse_batch(report, index, "flags INSTANCE_LIGHT_FACTOR", problem);
                return false;
            }
        }
        if (btbh_gpu_particle_shadow(batch)) {
            const char *problem = btbh_gpu_particle_shadow_problem(batch);
            if (problem != NULL) {
                btbh_gpu_scene_refuse_batch(report, index, "flags PARTICLE_SHADOW", problem);
                return false;
            }
        }
        const BTBHGPUSceneWorldDetail *detail = batch->world_detail;
        if (detail != NULL && detail != checked) {
            const char *problem = btbh_gpu_world_detail_problem(detail);
            if (problem == NULL &&
                (!btbh_gpu_scene_texture_usable(window, detail->albedo_texture) ||
                 !btbh_gpu_scene_texture_usable(window, detail->normal_texture) ||
                 !btbh_gpu_scene_texture_usable(window, detail->orm_texture))) {
                problem = "a texture belongs to another window or was destroyed";
            }
            if (problem == NULL &&
                (btbh_gpu_texture_is_array(detail->albedo_texture) ||
                 btbh_gpu_texture_is_array(detail->normal_texture) ||
                 btbh_gpu_texture_is_array(detail->orm_texture))) {
                problem = "a texture is a 2D array texture (only world_detail_table.map_layers takes one)";
            }
            if (problem == NULL &&
                (btbh_gpu_texture_is_heightfield(detail->albedo_texture) ||
                 btbh_gpu_texture_is_heightfield(detail->normal_texture) ||
                 btbh_gpu_texture_is_heightfield(detail->orm_texture))) {
                problem = "a texture is a heightfield (only height_lift.heightfield takes one)";
            }
            if (problem != NULL) {
                btbh_gpu_scene_refuse_batch(report, index, "world_detail", problem);
                return false;
            }
            checked = detail;
        }
        const BTBHGPUSceneWorldDetailTable *table = batch->world_detail_table;
        if (table != NULL && table != checked_table) {
            const BTBHGPUTexture *maps = table->map_layers;
            const char *problem = NULL;
            if (maps != NULL && !btbh_gpu_scene_texture_usable(window, maps)) {
                problem = "map_layers belongs to another window or was destroyed";
            } else if (maps != NULL && !btbh_gpu_texture_is_array(maps)) {
                problem = "map_layers is a 2D texture; it must be an array (btbh_gpu_window_texture_array_*)";
            }
            uint32_t entry = UINT32_MAX;
            if (problem == NULL) {
                problem = btbh_gpu_world_detail_table_problem(table, maps != NULL ? maps->layer_count : 0u, &entry);
            }
            if (problem != NULL) {
                if (entry != UINT32_MAX) {
                    char what[64];
                    (void)snprintf(what, sizeof(what), "world_detail_table entry %u", entry);
                    btbh_gpu_scene_refuse_batch(report, index, what, problem);
                } else {
                    btbh_gpu_scene_refuse_batch(report, index, "world_detail_table", problem);
                }
                return false;
            }
            checked_table = table;
        }
        /* One or the other: a table with maps and a detail with a texture would each want the world detail's
           place in the shader (the RFC refuses rather than let one win). */
        if (table != NULL && table->map_layers != NULL && btbh_gpu_world_detail_maps(detail) != 0u) {
            btbh_gpu_scene_refuse_batch(report, index, "world_detail_table",
                "the batch has a world_detail with a texture too; set one or the other");
            return false;
        }
        /* RFC 0012: the lift itself (once per run of batches sharing it), then what a lifted batch may be. */
        const BTBHGPUSceneHeightLift *lift = batch->height_lift;
        if (lift == NULL) {
            continue;
        }
        if (lift != checked_lift) {
            const char *problem = btbh_gpu_height_lift_problem(lift);
            if (problem == NULL && lift->heightfield != NULL) {
                if (!btbh_gpu_scene_texture_usable(window, lift->heightfield)) {
                    problem = "heightfield belongs to another window or was destroyed";
                } else if (!btbh_gpu_texture_is_heightfield(lift->heightfield)) {
                    problem = "heightfield isn't one (btbh_gpu_window_heightfield_create)";
                }
            }
            if (problem != NULL) {
                btbh_gpu_scene_refuse_batch(report, index, "height_lift", problem);
                return false;
            }
            checked_lift = lift;
        }
        if (lift->heightfield != NULL) {
            const char *problem = btbh_gpu_height_lift_batch_problem(
                batch,
                directional_shadow_visibility != NULL ? directional_shadow_visibility[index] : 1u
            );
            const char *what = "height_lift";
            for (uint32_t view = 0u; problem == NULL && view < view_count; ++view) {
                if (views[view].shadow_visibility != NULL) {
                    problem = btbh_gpu_height_lift_batch_problem(batch, views[view].shadow_visibility[index]);
                    what = view == 0u
                        ? "height_lift (view 0's shadow_visibility)"
                        : "height_lift (view 1's shadow_visibility)";
                }
            }
            if (problem != NULL) {
                btbh_gpu_scene_refuse_batch(report, index, what, problem);
                return false;
            }
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

/* RFC 0011 phase 1b, the run key: whether `batch` continues the run of `previous`, the batch drawn before it in the
   same pass. True when they are equal, byte for byte, in every field but their geometry (kind, model, custom_mesh,
   material_slot) and their instance range (first_instance, instance_count); a float compares by its bits (-0 and
   +0 differ), and padding isn't compared. Everything a pass binds or pushes for a batch other than its vertex and
   index buffers and its draw comes from those fields, from the pass and from what the fields point at (which a
   present doesn't change), so a batch that continues a run binds and pushes nothing new. The passes never let a
   skinned geometry start or continue a run: its bone offsets are its own. */
bool btbh_gpu_scene_batch_continues_run(
    const BTBHGPUSceneGeometryBatch *previous,
    const BTBHGPUSceneGeometryBatch *batch
) {
#define BTBH_GPU_SCENE_RUN_SAME(field) (memcmp(&previous->field, &batch->field, sizeof(batch->field)) == 0)
    return
        BTBH_GPU_SCENE_RUN_SAME(instance_buffer) &&
        BTBH_GPU_SCENE_RUN_SAME(albedo_texture) &&
        BTBH_GPU_SCENE_RUN_SAME(normal_texture) &&
        BTBH_GPU_SCENE_RUN_SAME(orm_texture) &&
        BTBH_GPU_SCENE_RUN_SAME(emission_texture) &&
        BTBH_GPU_SCENE_RUN_SAME(world_detail) &&
        BTBH_GPU_SCENE_RUN_SAME(world_detail_table) &&
        BTBH_GPU_SCENE_RUN_SAME(height_lift) &&
        BTBH_GPU_SCENE_RUN_SAME(dug_by) &&
        BTBH_GPU_SCENE_RUN_SAME(albedo_red) &&
        BTBH_GPU_SCENE_RUN_SAME(albedo_green) &&
        BTBH_GPU_SCENE_RUN_SAME(albedo_blue) &&
        BTBH_GPU_SCENE_RUN_SAME(albedo_alpha) &&
        BTBH_GPU_SCENE_RUN_SAME(uv_scale_x) &&
        BTBH_GPU_SCENE_RUN_SAME(uv_scale_y) &&
        BTBH_GPU_SCENE_RUN_SAME(uv_offset_x) &&
        BTBH_GPU_SCENE_RUN_SAME(uv_offset_y) &&
        BTBH_GPU_SCENE_RUN_SAME(normal_scale) &&
        BTBH_GPU_SCENE_RUN_SAME(occlusion) &&
        BTBH_GPU_SCENE_RUN_SAME(roughness) &&
        BTBH_GPU_SCENE_RUN_SAME(metalness) &&
        BTBH_GPU_SCENE_RUN_SAME(emission_red) &&
        BTBH_GPU_SCENE_RUN_SAME(emission_green) &&
        BTBH_GPU_SCENE_RUN_SAME(emission_blue) &&
        BTBH_GPU_SCENE_RUN_SAME(emission_alpha) &&
        BTBH_GPU_SCENE_RUN_SAME(emission_energy) &&
        BTBH_GPU_SCENE_RUN_SAME(blend_mode) &&
        BTBH_GPU_SCENE_RUN_SAME(billboard_mode) &&
        BTBH_GPU_SCENE_RUN_SAME(depth_compare) &&
        BTBH_GPU_SCENE_RUN_SAME(alpha_cutoff) &&
        BTBH_GPU_SCENE_RUN_SAME(stencil_compare) &&
        BTBH_GPU_SCENE_RUN_SAME(cull_mode) &&
        BTBH_GPU_SCENE_RUN_SAME(stencil_fail) &&
        BTBH_GPU_SCENE_RUN_SAME(stencil_depth_fail) &&
        BTBH_GPU_SCENE_RUN_SAME(stencil_pass) &&
        BTBH_GPU_SCENE_RUN_SAME(stencil_reference) &&
        BTBH_GPU_SCENE_RUN_SAME(stencil_mask) &&
        BTBH_GPU_SCENE_RUN_SAME(transparency_enabled) &&
        BTBH_GPU_SCENE_RUN_SAME(unlit_enabled) &&
        BTBH_GPU_SCENE_RUN_SAME(spot_shadow_visible) &&
        BTBH_GPU_SCENE_RUN_SAME(rc_receiver) &&
        BTBH_GPU_SCENE_RUN_SAME(reactive_mode) &&
        BTBH_GPU_SCENE_RUN_SAME(surface_variant) &&
        BTBH_GPU_SCENE_RUN_SAME(surface_time) &&
        BTBH_GPU_SCENE_RUN_SAME(surface_parameters0) &&
        BTBH_GPU_SCENE_RUN_SAME(surface_parameters1) &&
        BTBH_GPU_SCENE_RUN_SAME(detail_parameters) &&
        BTBH_GPU_SCENE_RUN_SAME(ssao_mode) &&
        BTBH_GPU_SCENE_RUN_SAME(flags) &&
        BTBH_GPU_SCENE_RUN_SAME(flags2) &&
        BTBH_GPU_SCENE_RUN_SAME(ssao_reserved) &&
        BTBH_GPU_SCENE_RUN_SAME(animation_library) &&
        BTBH_GPU_SCENE_RUN_SAME(animation_pose) &&
        BTBH_GPU_SCENE_RUN_SAME(animation_index) &&
        BTBH_GPU_SCENE_RUN_SAME(animation_time);
#undef BTBH_GPU_SCENE_RUN_SAME
}

/* BTBH_GPU_SCENE_RUNS, read once a present (RFC 0011). Unset (or any other value): runs on. "0", a measurement tool:
   every batch binds and pushes its whole state again, as before 0.10.0 (the frame's uniforms are still filled once a
   pass). "verify" (since 0.13.0), a debug mode: runs on, and every draw that continues a run also rebuilds what it
   would push and bind without the run and compares that with what the pass holds (BTBHGPUSceneRunVerify below). */
typedef enum BTBHGPUSceneRunsMode {
    BTBH_GPU_SCENE_RUNS_ON = 0,
    BTBH_GPU_SCENE_RUNS_OFF = 1,
    BTBH_GPU_SCENE_RUNS_VERIFY = 2
} BTBHGPUSceneRunsMode;

static BTBHGPUSceneRunsMode btbh_gpu_scene_runs_mode(void) {
    const char *value = SDL_getenv("BTBH_GPU_SCENE_RUNS");
    if (value == NULL) {
        return BTBH_GPU_SCENE_RUNS_ON;
    }
    if (strcmp(value, "0") == 0) {
        return BTBH_GPU_SCENE_RUNS_OFF;
    }
    return strcmp(value, "verify") == 0 ? BTBH_GPU_SCENE_RUNS_VERIFY : BTBH_GPU_SCENE_RUNS_ON;
}

/* ------------------------------------------------------------------------- BTBH_GPU_SCENE_RUNS=verify (0.13.0) */

/* A pass that encodes batches holds one BTBHGPUSceneRunVerify while the mode is on (NULL otherwise, and then every
   helper below is the SDL call it wraps). The pass routes the pushes and binds a batch makes for its run through the
   helpers. A draw that starts a run (or draws without one) records what it pushes and binds; a draw that continues a
   run runs the same code with `checking` set, and each helper compares instead of pushing or binding: the uniform
   blocks byte for byte at the size they would be pushed with, the vertex and fragment samplers, the vertex storage
   buffers, the stencil reference and the pipeline. The pass leaves the state it caches (the pipeline bound, the
   table, lift and particle block pushed, run_batch) as it was. A difference is a run key that misses a field the
   pass reads: it is logged (SDL_Log, the pass, the batch and the field; the first
   BTBH_GPU_SCENE_RUN_VERIFY_LOG_LIMIT per window) and counted (the scene telemetry's run_verify_* for a present, and
   btbh_gpu_window_scene_read_run_verify over the window's life, scenes rendered into a target included). The frame
   is the frame with runs on: a checking draw pushes and binds nothing. */
#define BTBH_GPU_SCENE_RUN_VERIFY_BLOCK_CAPACITY 4096u
#define BTBH_GPU_SCENE_RUN_VERIFY_VERTEX_SLOTS 2u
#define BTBH_GPU_SCENE_RUN_VERIFY_FRAGMENT_SLOTS 3u
#define BTBH_GPU_SCENE_RUN_VERIFY_SAMPLERS 16u
#define BTBH_GPU_SCENE_RUN_VERIFY_STORAGE_BUFFERS 4u
#define BTBH_GPU_SCENE_RUN_VERIFY_LOG_LIMIT 64u
/* The helpers that wrap the SDL calls are always inlined, so that with the mode off a push or bind costs one test of
   `verify` more; the checks themselves stay out of line. */
#if defined(_MSC_VER)
#define BTBH_GPU_SCENE_RUN_INLINE static __forceinline
#define BTBH_GPU_SCENE_RUN_NOINLINE static __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define BTBH_GPU_SCENE_RUN_INLINE static inline __attribute__((always_inline))
#define BTBH_GPU_SCENE_RUN_NOINLINE static __attribute__((noinline))
#else
#define BTBH_GPU_SCENE_RUN_INLINE static inline
#define BTBH_GPU_SCENE_RUN_NOINLINE static
#endif

/* A uniform block's fields by offset, so that a difference names the first field it touches. */
typedef struct BTBHGPUSceneRunVerifyField {
    const char *name;
    uint32_t offset;
} BTBHGPUSceneRunVerifyField;

typedef struct BTBHGPUSceneRunVerifyLayout {
    const char *name;
    const BTBHGPUSceneRunVerifyField *fields;
    uint32_t field_count;
} BTBHGPUSceneRunVerifyLayout;

#define BTBH_GPU_SCENE_RUN_FIELD(type, field) { #field, (uint32_t)offsetof(type, field) }
#define BTBH_GPU_SCENE_RUN_LAYOUT(name, fields) { name, fields, (uint32_t)(sizeof(fields) / sizeof(fields[0])) }

static const BTBHGPUSceneRunVerifyField btbh_gpu_scene_run_vertex_fields[] = {
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneUniforms, view_projection),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneUniforms, camera_right),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneUniforms, camera_up),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneUniforms, camera_position),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneUniforms, material_color),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneUniforms, uv_transform),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneUniforms, ambient_color_energy),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneUniforms, directional_direction_energy),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneUniforms, directional_color),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneUniforms, secondary_directional_direction_energy),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneUniforms, secondary_directional_color),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneUniforms, bone_offset),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneUniforms, billboard_mode),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneUniforms, unlit_enabled),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneUniforms, surface_variant),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneUniforms, surface_controls),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneUniforms, surface_parameters0),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneUniforms, surface_parameters1),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneUniforms, world_curvature),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneUniforms, previous_world_curvature),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneUniforms, previous_view_projection),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneUniforms, temporal_controls),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneUniforms, previous_bone_offset),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneUniforms, instance_uv_rect),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneUniforms, temporal_reserved),
};

static const BTBHGPUSceneRunVerifyField btbh_gpu_scene_run_fragment_fields[] = {
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneFragmentUniforms, camera_position),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneFragmentUniforms, ambient_color_energy),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneFragmentUniforms, directional_direction_energy),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneFragmentUniforms, directional_color),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneFragmentUniforms, secondary_directional_direction_energy),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneFragmentUniforms, secondary_directional_color),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneFragmentUniforms, spot_position_range),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneFragmentUniforms, spot_direction_energy),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneFragmentUniforms, spot_color_falloff),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneFragmentUniforms, spot_cutoffs),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneFragmentUniforms, material_factors),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneFragmentUniforms, emission_color_energy),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneFragmentUniforms, unlit_enabled),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneFragmentUniforms, alpha_cutoff),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneFragmentUniforms, surface_variant),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneFragmentUniforms, surface_time),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneFragmentUniforms, surface_parameters0),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneFragmentUniforms, surface_parameters1),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneFragmentUniforms, irradiance_sh),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneFragmentUniforms, ground_bottom_energy),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneFragmentUniforms, ground_horizon_curve),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneFragmentUniforms, sky_top_energy),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneFragmentUniforms, sky_horizon_curve),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneFragmentUniforms, sun_color_energy),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneFragmentUniforms, sun_direction_size),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneFragmentUniforms, procedural_controls),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneFragmentUniforms, detail_parameters),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneFragmentUniforms, surface_wetness),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneFragmentUniforms, cloud_controls),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneFragmentUniforms, cloud_shape),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneFragmentUniforms, lamp_controls),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneFragmentUniforms, lamp_position_reach),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneFragmentUniforms, lamp_color_energy),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneFragmentUniforms, point_light_count),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneFragmentUniforms, point_lights),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneFragmentUniforms, directional_shadow_view_projection),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneFragmentUniforms, secondary_directional_shadow_view_projection),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneFragmentUniforms, spot_shadow_view_projection),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneFragmentUniforms, directional_shadow_controls),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneFragmentUniforms, secondary_directional_shadow_controls),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneFragmentUniforms, spot_shadow_controls),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneFragmentUniforms, omni_shadow_position_range),
};

static const BTBHGPUSceneRunVerifyField btbh_gpu_scene_run_extension_fields[] = {
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneBatchExtensionUniforms, dig_mask.origin),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneBatchExtensionUniforms, dig_mask.scale),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneBatchExtensionUniforms, world_detail_tiles),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneBatchExtensionUniforms, world_detail_controls),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneBatchExtensionUniforms, world_detail_mode),
};

static const BTBHGPUSceneRunVerifyField btbh_gpu_scene_run_shadow_fields[] = {
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUShadowUniforms, light_view_projection),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUShadowUniforms, camera_right),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUShadowUniforms, camera_up),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUShadowUniforms, bone_offset),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUShadowUniforms, billboard_mode),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUShadowUniforms, surface_variant),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUShadowUniforms, surface_time),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUShadowUniforms, surface_parameters0),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUShadowUniforms, surface_parameters1),
};

static const BTBHGPUSceneRunVerifyField btbh_gpu_scene_run_dig_fields[] = {
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneDigMaskUniforms, origin),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneDigMaskUniforms, scale),
};

static const BTBHGPUSceneRunVerifyField btbh_gpu_scene_run_lift_fields[] = {
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUHeightLiftUniforms, uv),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUHeightLiftUniforms, scale),
};

static const BTBHGPUSceneRunVerifyField btbh_gpu_scene_run_table_fields[] = {
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUWorldDetailTableUniforms, tiles),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUWorldDetailTableUniforms, controls),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUWorldDetailTableUniforms, layers),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUWorldDetailTableUniforms, count),
};

static const BTBHGPUSceneRunVerifyField btbh_gpu_scene_run_particle_fields[] = {
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneParticleFrameUniforms, inverse_view_projection),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneParticleFrameUniforms, controls),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneParticleFrameUniforms, fog_color_density),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneParticleFrameUniforms, fog_shape),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneParticleFrameUniforms, fog_edge_bounds),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneParticleFrameUniforms, fog_edge),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneParticleFrameUniforms, cascade_origin),
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneParticleFrameUniforms, cascade_field),
    /* (RFC 0013 phase 2, 0.14.0.) */
    BTBH_GPU_SCENE_RUN_FIELD(BTBHGPUSceneParticleFrameUniforms, shadow_controls),
};

static const BTBHGPUSceneRunVerifyLayout btbh_gpu_scene_run_vertex_layout =
    BTBH_GPU_SCENE_RUN_LAYOUT("BTBHGPUSceneUniforms", btbh_gpu_scene_run_vertex_fields);
static const BTBHGPUSceneRunVerifyLayout btbh_gpu_scene_run_fragment_layout =
    BTBH_GPU_SCENE_RUN_LAYOUT("BTBHGPUSceneFragmentUniforms", btbh_gpu_scene_run_fragment_fields);
static const BTBHGPUSceneRunVerifyLayout btbh_gpu_scene_run_extension_layout =
    BTBH_GPU_SCENE_RUN_LAYOUT("BTBHGPUSceneBatchExtensionUniforms", btbh_gpu_scene_run_extension_fields);
static const BTBHGPUSceneRunVerifyLayout btbh_gpu_scene_run_shadow_layout =
    BTBH_GPU_SCENE_RUN_LAYOUT("BTBHGPUShadowUniforms", btbh_gpu_scene_run_shadow_fields);
static const BTBHGPUSceneRunVerifyLayout btbh_gpu_scene_run_dig_layout =
    BTBH_GPU_SCENE_RUN_LAYOUT("BTBHGPUSceneDigMaskUniforms", btbh_gpu_scene_run_dig_fields);
static const BTBHGPUSceneRunVerifyLayout btbh_gpu_scene_run_lift_layout =
    BTBH_GPU_SCENE_RUN_LAYOUT("BTBHGPUHeightLiftUniforms", btbh_gpu_scene_run_lift_fields);
static const BTBHGPUSceneRunVerifyLayout btbh_gpu_scene_run_table_layout =
    BTBH_GPU_SCENE_RUN_LAYOUT("BTBHGPUWorldDetailTableUniforms", btbh_gpu_scene_run_table_fields);
static const BTBHGPUSceneRunVerifyLayout btbh_gpu_scene_run_particle_layout =
    BTBH_GPU_SCENE_RUN_LAYOUT("BTBHGPUSceneParticleFrameUniforms", btbh_gpu_scene_run_particle_fields);

#undef BTBH_GPU_SCENE_RUN_LAYOUT
#undef BTBH_GPU_SCENE_RUN_FIELD

/* What the pass holds: the last block pushed to each uniform slot and the last bindings, in this pass. */
typedef struct BTBHGPUSceneRunVerifyBlock {
    const BTBHGPUSceneRunVerifyLayout *layout;
    uint32_t size;  /* 0: nothing pushed to the slot in this pass */
    uint8_t data[BTBH_GPU_SCENE_RUN_VERIFY_BLOCK_CAPACITY];
} BTBHGPUSceneRunVerifyBlock;

struct BTBHGPUSceneRunVerify {
    BTBHGPUWindow *window;
    BTBHGPUSceneTelemetryReport *telemetry;
    const char *pass_name;
    uint32_t batch_index;
    bool checking;
    BTBHGPUSceneRunVerifyBlock vertex_uniforms[BTBH_GPU_SCENE_RUN_VERIFY_VERTEX_SLOTS];
    BTBHGPUSceneRunVerifyBlock fragment_uniforms[BTBH_GPU_SCENE_RUN_VERIFY_FRAGMENT_SLOTS];
    SDL_GPUTextureSamplerBinding vertex_samplers[BTBH_GPU_SCENE_RUN_VERIFY_SAMPLERS];
    uint32_t vertex_sampler_count;
    SDL_GPUTextureSamplerBinding fragment_samplers[BTBH_GPU_SCENE_RUN_VERIFY_SAMPLERS];
    uint32_t fragment_sampler_count;
    SDL_GPUBuffer *vertex_storage_buffers[BTBH_GPU_SCENE_RUN_VERIFY_STORAGE_BUFFERS];
    uint32_t vertex_storage_buffer_count;
    uint8_t stencil_reference;
    bool stencil_reference_set;
};

/* The window's state for this present when the mode is on, made the first time (NULL when it is off, or when it
   can't be made: the present then draws with runs, unchecked). */
static BTBHGPUSceneRunVerify *btbh_gpu_scene_run_verify_prepare(
    BTBHGPUWindow *window,
    BTBHGPUSceneRuntime *runtime,
    BTBHGPUSceneTelemetryReport *telemetry,
    BTBHGPUSceneRunsMode mode
) {
    if (mode != BTBH_GPU_SCENE_RUNS_VERIFY || window == NULL || runtime == NULL) {
        return NULL;
    }
    if (runtime->run_verify == NULL) {
        runtime->run_verify = (BTBHGPUSceneRunVerify *)calloc(1u, sizeof(*runtime->run_verify));
        if (runtime->run_verify == NULL) {
            return NULL;
        }
    }
    runtime->run_verify->window = window;
    runtime->run_verify->telemetry = telemetry;
    return runtime->run_verify;
}

/* A pass that encodes batches begins: it holds nothing yet. */
static void btbh_gpu_scene_run_verify_begin_pass(BTBHGPUSceneRunVerify *verify, const char *pass_name) {
    if (verify == NULL) {
        return;
    }
    verify->pass_name = pass_name;
    verify->checking = false;
    for (uint32_t slot = 0u; slot < BTBH_GPU_SCENE_RUN_VERIFY_VERTEX_SLOTS; ++slot) {
        verify->vertex_uniforms[slot].size = 0u;
    }
    for (uint32_t slot = 0u; slot < BTBH_GPU_SCENE_RUN_VERIFY_FRAGMENT_SLOTS; ++slot) {
        verify->fragment_uniforms[slot].size = 0u;
    }
    verify->vertex_sampler_count = 0u;
    verify->fragment_sampler_count = 0u;
    verify->vertex_storage_buffer_count = 0u;
    verify->stencil_reference_set = false;
}

/* A geometry's batch state: recorded (it starts a run or has none) or checked (it continues one). */
BTBH_GPU_SCENE_RUN_INLINE void btbh_gpu_scene_run_verify_draw(
    BTBHGPUSceneRunVerify *verify,
    uint32_t batch_index,
    bool checking
) {
    if (verify == NULL) {
        return;
    }
    verify->batch_index = batch_index;
    verify->checking = checking;
    if (checking) {
        verify->telemetry->run_verify_draw_count += 1u;
        verify->window->scene_run_verify.checked_draw_count += 1u;
    }
}

BTBH_GPU_SCENE_RUN_NOINLINE void btbh_gpu_scene_run_verify_mismatch(
    BTBHGPUSceneRunVerify *verify,
    const char *what,
    const char *detail
) {
    verify->telemetry->run_verify_mismatch_count += 1u;
    BTBHGPUSceneRunVerifyReport *totals = &verify->window->scene_run_verify;
    totals->mismatch_count += 1u;
    if (totals->mismatch_count <= BTBH_GPU_SCENE_RUN_VERIFY_LOG_LIMIT) {
        SDL_Log(
            "BTBH_GPU_SCENE_RUNS=verify: mismatch in the %s pass, batch %u: %s%s%s",
            verify->pass_name != NULL ? verify->pass_name : "scene",
            verify->batch_index,
            what,
            detail != NULL && detail[0] != '\0' ? ": " : "",
            detail != NULL ? detail : ""
        );
        if (totals->mismatch_count == BTBH_GPU_SCENE_RUN_VERIFY_LOG_LIMIT) {
            SDL_Log(
                "BTBH_GPU_SCENE_RUNS=verify: %u mismatches logged; the window counts the rest without logging them",
                BTBH_GPU_SCENE_RUN_VERIFY_LOG_LIMIT
            );
        }
    }
}

/* Records a block about to be pushed (true: push it), or compares it with the block the slot holds (false). */
BTBH_GPU_SCENE_RUN_NOINLINE bool btbh_gpu_scene_run_verify_block(
    BTBHGPUSceneRunVerify *verify,
    BTBHGPUSceneRunVerifyBlock *held,
    const char *stage,
    uint32_t slot,
    const void *data,
    uint32_t size,
    const BTBHGPUSceneRunVerifyLayout *layout
) {
    const uint32_t kept = size < BTBH_GPU_SCENE_RUN_VERIFY_BLOCK_CAPACITY ? size : BTBH_GPU_SCENE_RUN_VERIFY_BLOCK_CAPACITY;
    if (!verify->checking) {
        (void)memcpy(held->data, data, kept);
        held->size = size;
        held->layout = layout;
        return true;
    }
    char what[160];
    char detail[256];
    (void)snprintf(what, sizeof(what), "%s uniform slot %u (%s)", stage, slot, layout != NULL ? layout->name : "block");
    if (held->size == 0u) {
        btbh_gpu_scene_run_verify_mismatch(verify, what, "the run's first batch pushed none");
        return false;
    }
    if (held->size != size || held->layout != layout) {
        (void)snprintf(
            detail,
            sizeof(detail),
            "%u bytes (%s) against the %u held (%s)",
            size,
            layout != NULL ? layout->name : "block",
            held->size,
            held->layout != NULL ? held->layout->name : "block"
        );
        btbh_gpu_scene_run_verify_mismatch(verify, what, detail);
        return false;
    }
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t first = 0u;
    while (first < kept && bytes[first] == held->data[first]) {
        first += 1u;
    }
    if (first == kept) {
        return false;
    }
    const char *field = "?";
    uint32_t field_offset = 0u;
    if (layout != NULL) {
        for (uint32_t index = 0u; index < layout->field_count; ++index) {
            if (layout->fields[index].offset <= first) {
                field = layout->fields[index].name;
                field_offset = layout->fields[index].offset;
            }
        }
    }
    (void)snprintf(detail, sizeof(detail), "%s (+%u), byte %u of %u", field, first - field_offset, first, size);
    btbh_gpu_scene_run_verify_mismatch(verify, what, detail);
    return false;
}

BTBH_GPU_SCENE_RUN_INLINE void btbh_gpu_scene_run_push_vertex_uniforms(
    BTBHGPUSceneRunVerify *verify,
    SDL_GPUCommandBuffer *command,
    uint32_t slot,
    const void *data,
    uint32_t size,
    const BTBHGPUSceneRunVerifyLayout *layout
) {
    if (verify != NULL && slot < BTBH_GPU_SCENE_RUN_VERIFY_VERTEX_SLOTS &&
        !btbh_gpu_scene_run_verify_block(verify, &verify->vertex_uniforms[slot], "vertex", slot, data, size, layout)) {
        return;
    }
    SDL_PushGPUVertexUniformData(command, slot, data, size);
}

BTBH_GPU_SCENE_RUN_INLINE void btbh_gpu_scene_run_push_fragment_uniforms(
    BTBHGPUSceneRunVerify *verify,
    SDL_GPUCommandBuffer *command,
    uint32_t slot,
    const void *data,
    uint32_t size,
    const BTBHGPUSceneRunVerifyLayout *layout
) {
    if (verify != NULL && slot < BTBH_GPU_SCENE_RUN_VERIFY_FRAGMENT_SLOTS &&
        !btbh_gpu_scene_run_verify_block(verify, &verify->fragment_uniforms[slot], "fragment", slot, data, size, layout)) {
        return;
    }
    SDL_PushGPUFragmentUniformData(command, slot, data, size);
}

/* Records bindings from slot 0 about to be made (true: make them), or compares them with those held (false). */
BTBH_GPU_SCENE_RUN_NOINLINE bool btbh_gpu_scene_run_verify_bindings(
    BTBHGPUSceneRunVerify *verify,
    void *held,
    uint32_t *held_count,
    const void *bindings,
    uint32_t count,
    size_t binding_size,
    uint32_t capacity,
    const char *what
) {
    const uint32_t kept = count < capacity ? count : capacity;
    if (!verify->checking) {
        (void)memcpy(held, bindings, binding_size * kept);
        *held_count = count;
        return true;
    }
    if (*held_count != count) {
        char detail[96];
        (void)snprintf(detail, sizeof(detail), "%u bound against the %u held", count, *held_count);
        btbh_gpu_scene_run_verify_mismatch(verify, what, detail);
        return false;
    }
    for (uint32_t index = 0u; index < kept; ++index) {
        if (memcmp(
                (const uint8_t *)bindings + binding_size * index,
                (const uint8_t *)held + binding_size * index,
                binding_size
            ) != 0) {
            char detail[64];
            (void)snprintf(detail, sizeof(detail), "slot %u differs", index);
            btbh_gpu_scene_run_verify_mismatch(verify, what, detail);
            return false;
        }
    }
    return false;
}

BTBH_GPU_SCENE_RUN_INLINE void btbh_gpu_scene_run_bind_fragment_samplers(
    BTBHGPUSceneRunVerify *verify,
    SDL_GPURenderPass *pass,
    const SDL_GPUTextureSamplerBinding *bindings,
    uint32_t count
) {
    if (verify != NULL &&
        !btbh_gpu_scene_run_verify_bindings(
            verify,
            verify->fragment_samplers,
            &verify->fragment_sampler_count,
            bindings,
            count,
            sizeof(bindings[0]),
            BTBH_GPU_SCENE_RUN_VERIFY_SAMPLERS,
            "fragment samplers"
        )) {
        return;
    }
    SDL_BindGPUFragmentSamplers(pass, 0u, bindings, count);
}

BTBH_GPU_SCENE_RUN_INLINE void btbh_gpu_scene_run_bind_vertex_samplers(
    BTBHGPUSceneRunVerify *verify,
    SDL_GPURenderPass *pass,
    const SDL_GPUTextureSamplerBinding *bindings,
    uint32_t count
) {
    if (verify != NULL &&
        !btbh_gpu_scene_run_verify_bindings(
            verify,
            verify->vertex_samplers,
            &verify->vertex_sampler_count,
            bindings,
            count,
            sizeof(bindings[0]),
            BTBH_GPU_SCENE_RUN_VERIFY_SAMPLERS,
            "vertex samplers"
        )) {
        return;
    }
    SDL_BindGPUVertexSamplers(pass, 0u, bindings, count);
}

BTBH_GPU_SCENE_RUN_INLINE void btbh_gpu_scene_run_bind_vertex_storage_buffers(
    BTBHGPUSceneRunVerify *verify,
    SDL_GPURenderPass *pass,
    SDL_GPUBuffer *const *buffers,
    uint32_t count
) {
    if (verify != NULL &&
        !btbh_gpu_scene_run_verify_bindings(
            verify,
            verify->vertex_storage_buffers,
            &verify->vertex_storage_buffer_count,
            buffers,
            count,
            sizeof(buffers[0]),
            BTBH_GPU_SCENE_RUN_VERIFY_STORAGE_BUFFERS,
            "vertex storage buffers"
        )) {
        return;
    }
    SDL_BindGPUVertexStorageBuffers(pass, 0u, buffers, count);
}

/* Records the stencil reference about to be set (true: set it), or compares it with the one held (false). */
BTBH_GPU_SCENE_RUN_NOINLINE bool btbh_gpu_scene_run_verify_stencil(BTBHGPUSceneRunVerify *verify, uint8_t reference) {
    if (!verify->checking) {
        verify->stencil_reference = reference;
        verify->stencil_reference_set = true;
        return true;
    }
    if (!verify->stencil_reference_set || verify->stencil_reference != reference) {
        char detail[64];
        (void)snprintf(detail, sizeof(detail), "%u against the %u held", reference, verify->stencil_reference);
        btbh_gpu_scene_run_verify_mismatch(verify, "stencil reference", detail);
    }
    return false;
}

BTBH_GPU_SCENE_RUN_INLINE void btbh_gpu_scene_run_set_stencil_reference(
    BTBHGPUSceneRunVerify *verify,
    SDL_GPURenderPass *pass,
    uint8_t reference
) {
    if (verify != NULL && !btbh_gpu_scene_run_verify_stencil(verify, reference)) {
        return;
    }
    SDL_SetGPUStencilReference(pass, reference);
}

/* A checking draw's pipeline against the one the pass has bound (a recording draw binds as the pass always did). */
static void btbh_gpu_scene_run_verify_pipeline(
    BTBHGPUSceneRunVerify *verify,
    const SDL_GPUGraphicsPipeline *pipeline,
    const SDL_GPUGraphicsPipeline *bound
) {
    if (pipeline != bound) {
        btbh_gpu_scene_run_verify_mismatch(verify, "pipeline", "another pipeline than the one bound");
    }
}

/* The buffers and ranges a batch's geometry draws from: a model's primitive, or a custom mesh, in buffers of its own
   (vertex_base and first_index 0) or in the window's shared ones (BTBH_GPU_CUSTOM_MESH_SHARED_BUFFERS, RFC 0011). */
typedef struct BTBHGPUSceneGeometryDraw {
    SDL_GPUBuffer *vertex_buffer;
    SDL_GPUBuffer *index_buffer;
    uint32_t vertex_count;
    uint32_t index_count;
    uint32_t first_index;
    int32_t vertex_base;
} BTBHGPUSceneGeometryDraw;

static BTBHGPUSceneGeometryDraw btbh_gpu_scene_geometry_draw(
    const BTBHGPUSceneGeometryBatch *batch,
    uint32_t geometry_index
) {
    BTBHGPUSceneGeometryDraw draw;
    if (batch->kind == BTBH_GPU_SCENE_GEOMETRY_MODEL) {
        const BTBHGPUModelPrimitive *primitive = &batch->model->primitives[geometry_index];
        draw.vertex_buffer = primitive->vertex_buffer;
        draw.index_buffer = primitive->index_buffer;
        draw.vertex_count = primitive->vertex_count;
        draw.index_count = primitive->index_count;
        draw.first_index = primitive->first_index;
        draw.vertex_base = primitive->vertex_base;
    } else {
        const BTBHGPUCustomMesh *mesh = batch->custom_mesh;
        draw.vertex_buffer = mesh->vertex_buffer;
        draw.index_buffer = mesh->index_buffer;
        draw.vertex_count = mesh->vertex_count;
        draw.index_count = mesh->index_count;
        draw.first_index = mesh->first_index;
        draw.vertex_base = mesh->vertex_base;
    }
    return draw;
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

/* ---- The opaque pass's front-to-back order (BTBH_GPU_SCENE_OPAQUE_SORT_FRONT_TO_BACK) ---- */

/* The opaque pass draws a batch that is neither transparent nor refracting (as the geometry phases sort them). */
static bool btbh_gpu_scene_batch_draws_opaque(
    const BTBHGPUSceneGeometryBatch *batch
) {
    return batch->surface_variant != BTBH_GPU_SCENE_SURFACE_REFRACT &&
        !btbh_gpu_scene_uses_transparent_pass(
            batch->blend_mode,
            batch->transparency_enabled != 0u
        );
}

/* Whether the order may move an opaque batch: with depth LESS and the stencil off, what it leaves in the
   targets doesn't depend on what was drawn before it (the nearest surface shows, whoever drew first), and the
   host didn't pin it. LESS_EQUAL too when the host asked (FRONT_TO_BACK_LESS_EQUAL: it pins coplanar overlays
   itself). Its depth must be known too (btbh_gpu_scene_batch_view_depth). */
static bool btbh_gpu_scene_batch_movable(
    const BTBHGPUSceneGeometryBatch *batch,
    bool move_less_equal
) {
    BTBHGPUSceneStencilVariant stencil_variant;
    return (batch->flags & BTBH_GPU_SCENE_BATCH_KEEP_ORDER) == 0u &&
        (batch->depth_compare == BTBH_GPU_SCENE_DEPTH_LESS ||
         (move_less_equal && batch->depth_compare == BTBH_GPU_SCENE_DEPTH_LESS_EQUAL)) &&
        btbh_gpu_scene_stencil_variant(batch, &stencil_variant) &&
        stencil_variant == BTBH_GPU_SCENE_STENCIL_DISABLED &&
        batch->instance_buffer == NULL;
}

/* The view depth a batch sorts by: of its geometry's bounds' centre, through each of its transient instances,
   averaged (depth is affine in the position, so this is the depth of the instances' mean centre). A lifted batch's
   centres are lifted by its heightfield's CPU heights where they stand (RFC 0012). False when it can't be known
   cheaply: instances in a persistent buffer, a custom mesh without valid bounds. */
static bool btbh_gpu_scene_batch_view_depth(
    const BTBHGPUSceneGeometryBatch *batch,
    const float *instances,
    const float depth_row[4],
    float *depth
) {
    const float *minimum = NULL;
    const float *maximum = NULL;
    if (batch->kind == BTBH_GPU_SCENE_GEOMETRY_MODEL) {
        minimum = batch->model->minimum;
        maximum = batch->model->maximum;
    } else if (batch->custom_mesh->bounds_valid) {
        minimum = batch->custom_mesh->minimum;
        maximum = batch->custom_mesh->maximum;
    }
    if (minimum == NULL ||
        instances == NULL ||
        batch->instance_buffer != NULL ||
        batch->instance_count == 0u) {
        return false;
    }
    const float centre[3] = {
        0.5f * (minimum[0] + maximum[0]),
        0.5f * (minimum[1] + maximum[1]),
        0.5f * (minimum[2] + maximum[2])
    };
    /* The instance transform is column-major (translation in 12-14), as the vertex shader reads it. */
    const float *record =
        instances + (size_t)batch->first_instance * BTBH_GPU_SCENE_INSTANCE_FLOATS;
    const BTBHGPUSceneHeightLift *lift = btbh_gpu_height_lift_active(batch) ? batch->height_lift : NULL;
    double sum = 0.0;
    for (uint32_t index = 0u; index < batch->instance_count; ++index) {
        const float x = record[0] * centre[0] + record[4] * centre[1] +
            record[8] * centre[2] + record[12];
        float y = record[1] * centre[0] + record[5] * centre[1] +
            record[9] * centre[2] + record[13];
        const float z = record[2] * centre[0] + record[6] * centre[1] +
            record[10] * centre[2] + record[14];
        if (lift != NULL) {
            y += btbh_gpu_height_lift_height(lift, lift->heightfield->heights, lift->heightfield->width,
                lift->heightfield->height, x, z);
        }
        sum += (double)(depth_row[0] * x + depth_row[1] * y +
            depth_row[2] * z + depth_row[3]);
        record += BTBH_GPU_SCENE_INSTANCE_FLOATS;
    }
    const float mean = (float)(sum / (double)batch->instance_count);
    if (!isfinite(mean)) {
        return false;
    }
    *depth = mean;
    return true;
}

/* Sorts `count` keys ascending: insertion sort in runs of 16, then bottom-up merges through `buffer`. */
static void btbh_gpu_scene_sort_keys(
    uint64_t *keys,
    uint64_t *buffer,
    uint32_t count
) {
    const size_t run = 16u;
    for (size_t start = 0u; start < count; start += run) {
        const size_t end = SDL_min(start + run, (size_t)count);
        for (size_t index = start + 1u; index < end; ++index) {
            const uint64_t key = keys[index];
            size_t slot = index;
            while (slot > start && keys[slot - 1u] > key) {
                keys[slot] = keys[slot - 1u];
                slot -= 1u;
            }
            keys[slot] = key;
        }
    }
    uint64_t *source = keys;
    uint64_t *target = buffer;
    for (size_t width = run; width < count; width *= 2u) {
        for (size_t start = 0u; start < count; start += 2u * width) {
            const size_t middle = SDL_min(start + width, (size_t)count);
            const size_t end = SDL_min(start + 2u * width, (size_t)count);
            size_t left = start;
            size_t right = middle;
            size_t out = start;
            while (left < middle && right < end) {
                target[out++] = source[left] <= source[right]
                    ? source[left++]
                    : source[right++];
            }
            while (left < middle) {
                target[out++] = source[left++];
            }
            while (right < end) {
                target[out++] = source[right++];
            }
        }
        uint64_t *swap = source;
        source = target;
        target = swap;
    }
    if (source != keys) {
        (void)memcpy(keys, source, sizeof(uint64_t) * count);
    }
}

/* Sorts the run of movable batches gathered in keys[0..run_count) and appends it to the order. */
static uint32_t btbh_gpu_scene_flush_opaque_run(
    uint64_t *keys,
    uint64_t *buffer,
    uint32_t run_count,
    uint32_t *order,
    uint32_t order_count
) {
    btbh_gpu_scene_sort_keys(keys, buffer, run_count);
    for (uint32_t index = 0u; index < run_count; ++index) {
        order[order_count++] = (uint32_t)(keys[index] & UINT32_MAX);
    }
    return order_count;
}

uint32_t btbh_gpu_scene_opaque_order(
    const BTBHGPUSceneGeometryBatch *batches,
    const uint8_t *scene_visibility,
    uint32_t batch_count,
    const float *instances,
    const float view_projection[16],
    bool move_less_equal,
    uint32_t *order,
    uint64_t *scratch
) {
    /* Perspective: clip w grows with the distance in front of the camera. Orthographic (clip w constant):
       clip z does (0 near to 1 far, as the LESS test reads it). The matrix is column-major. */
    const uint32_t row =
        fabsf(view_projection[3]) + fabsf(view_projection[7]) +
            fabsf(view_projection[11]) > 0.0f
            ? 3u
            : 2u;
    const float depth_row[4] = {
        view_projection[row],
        view_projection[4u + row],
        view_projection[8u + row],
        view_projection[12u + row]
    };
    uint64_t *keys = scratch;
    uint64_t *buffer = scratch + batch_count;
    uint32_t order_count = 0u;
    uint32_t run_count = 0u;
    for (uint32_t index = 0u; index < batch_count; ++index) {
        const BTBHGPUSceneGeometryBatch *batch = &batches[index];
        if ((scene_visibility != NULL && scene_visibility[index] == 0u) ||
            !btbh_gpu_scene_batch_draws_opaque(batch)) {
            continue;
        }
        float depth = 0.0f;
        if (btbh_gpu_scene_batch_movable(batch, move_less_equal) &&
            btbh_gpu_scene_batch_view_depth(batch, instances, depth_row, &depth)) {
            /* The key: the depth's bits ordered as the floats are, then the index (stable, and unique). */
            const uint32_t bits = btbh_gpu_scene_float_bits(depth);
            const uint32_t ordered =
                (bits & 0x80000000u) != 0u ? ~bits : bits | 0x80000000u;
            keys[run_count++] = ((uint64_t)ordered << 32u) | index;
            continue;
        }
        /* It keeps its place: the run before it sorts on its own. */
        order_count = btbh_gpu_scene_flush_opaque_run(
            keys,
            buffer,
            run_count,
            order,
            order_count
        );
        run_count = 0u;
        order[order_count++] = index;
    }
    return btbh_gpu_scene_flush_opaque_run(
        keys,
        buffer,
        run_count,
        order,
        order_count
    );
}

static bool btbh_gpu_scene_ensure_opaque_order(
    BTBHGPUSceneRuntime *runtime,
    uint32_t batch_count
) {
    if (batch_count <= runtime->opaque_order_capacity) {
        return true;
    }
    if ((size_t)batch_count > SIZE_MAX / (2u * sizeof(uint64_t))) {
        return false;
    }
    uint32_t *order = (uint32_t *)realloc(
        runtime->opaque_order,
        sizeof(uint32_t) * batch_count
    );
    if (order == NULL) {
        return false;
    }
    runtime->opaque_order = order;
    uint32_t *geometry_base = (uint32_t *)realloc(
        runtime->opaque_geometry_base,
        sizeof(uint32_t) * batch_count
    );
    if (geometry_base == NULL) {
        return false;
    }
    runtime->opaque_geometry_base = geometry_base;
    uint64_t *keys = (uint64_t *)realloc(
        runtime->opaque_order_keys,
        2u * sizeof(uint64_t) * batch_count
    );
    if (keys == NULL) {
        return false;
    }
    runtime->opaque_order_keys = keys;
    runtime->opaque_order_capacity = batch_count;
    return true;
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

/* Where the frame's samples are looked up (since 0.16.0; a scan of every sample before): a hash of what
   btbh_gpu_scene_animation_sample_matches compares, the pose alone for a batch with one. */
static uint32_t btbh_gpu_scene_animation_sample_hash(
    const BTBHGPUSceneGeometryBatch *batch,
    uint32_t skin_index,
    uint32_t joint_count
) {
    uint64_t hash = 0u;
    if (batch->animation_pose != NULL) {
        hash = (uint64_t)(uintptr_t)batch->animation_pose;
    } else {
        hash = (uint64_t)(uintptr_t)batch->animation_library ^
            ((((uint64_t)batch->animation_index << 32u) |
              btbh_gpu_scene_float_bits(batch->animation_time)) *
             0x9e3779b97f4a7c15ull);
    }
    hash = (hash ^ (((uint64_t)skin_index << 32u) | joint_count)) *
        0xbf58476d1ce4e5b9ull;
    hash ^= hash >> 31u;
    hash *= 0x94d049bb133111ebull;
    hash ^= hash >> 29u;
    return (uint32_t)hash;
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
   temporal frames). temporal_runtime is the runtime while the frame renders temporally, NULL otherwise.
   palette_cache is the window's kept palettes (since 0.16.0; NULL: every palette is sampled): a sample whose palette
   is kept copies it, and a sampled palette is kept. */
static bool btbh_gpu_scene_prepare_bones(
    const BTBHGPUSceneGeometryBatch *batches,
    uint32_t batch_count,
    uint32_t **bone_offsets,
    uint32_t *geometry_count,
    float **bone_matrices,
    uint32_t *bone_count,
    bool detailed_telemetry,
    BTBHGPUSceneTelemetryReport *telemetry,
    BTBHGPUSceneRuntime *temporal_runtime,
    BTBHGPUBonePaletteCache **palette_cache_slot
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
    /* The samples' hash table: a power of two of slots, at least twice the animated geometries, UINT32_MAX empty. */
    size_t sample_table_size = 16u;
    while (sample_table_size < (size_t)total_animated_geometry_count * 2u &&
           sample_table_size <= SIZE_MAX / (2u * sizeof(uint32_t))) {
        sample_table_size *= 2u;
    }
    uint32_t *sample_table =
        total_animated_geometry_count > 0u &&
                sample_table_size >= (size_t)total_animated_geometry_count * 2u &&
                sample_table_size <= UINT32_MAX
            ? malloc(sample_table_size * sizeof(*sample_table))
            : NULL;
    if (
        offsets == NULL ||
        (total_bone_count > 0u && matrices == NULL) ||
        (
            total_animated_geometry_count > 0u &&
            (samples == NULL || sample_table == NULL)
        )
    ) {
        free(sample_table);
        free(samples);
        free(matrices);
        free(offsets);
        return false;
    }
    const uint32_t sample_table_mask = (uint32_t)(sample_table_size - 1u);
    BTBHGPUBonePaletteCache *palette_cache = NULL;
    if (total_animated_geometry_count > 0u) {
        (void)memset(sample_table, 0xff, sample_table_size * sizeof(*sample_table));
        palette_cache = btbh_gpu_bone_palette_cache_begin(palette_cache_slot, false, true);
    } else if (palette_cache_slot != NULL && *palette_cache_slot != NULL) {
        /* No skin: the kept palettes still age (a target-only host's scenes count uses too). */
        (void)btbh_gpu_bone_palette_cache_begin(palette_cache_slot, false, false);
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
    uint32_t palette_hit_count = 0u;
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
                    /* The frame's sample it shares (at most one matches: a sample is added only when none does), or
                       the empty slot it takes. */
                    uint32_t table_slot = btbh_gpu_scene_animation_sample_hash(
                        batch,
                        (uint32_t)skin_index,
                        joint_count
                    ) & sample_table_mask;
                    uint32_t matching_sample = UINT32_MAX;
                    while (sample_table[table_slot] != UINT32_MAX) {
                        if (btbh_gpu_scene_animation_sample_matches(
                                &samples[sample_table[table_slot]],
                                batch,
                                (uint32_t)skin_index,
                                joint_count
                            )) {
                            matching_sample = sample_table[table_slot];
                            break;
                        }
                        table_slot = (table_slot + 1u) & sample_table_mask;
                    }
                    if (matching_sample != UINT32_MAX) {
                        offsets[geometry_index] =
                            samples[matching_sample].bone_offset;
                    } else {
                        offsets[geometry_index] = bone_index;
                        float *palette = &matrices[(size_t)bone_index * 16u];
                        BTBHGPUBonePaletteKey palette_key;
                        const bool keyed =
                            palette_cache != NULL &&
                            btbh_gpu_bone_palette_key(
                                batch->animation_library,
                                batch->animation_pose,
                                batch->animation_index,
                                batch->animation_time,
                                (uint32_t)skin_index,
                                joint_count,
                                &palette_key
                            );
                        const float *kept = keyed
                            ? btbh_gpu_bone_palette_cache_find(palette_cache, &palette_key)
                            : NULL;
                        if (kept != NULL) {
                            (void)memcpy(
                                palette,
                                kept,
                                (size_t)joint_count * 16u * sizeof(float)
                            );
                            palette_hit_count += 1u;
                        } else {
                            valid = batch->animation_pose != NULL
                                ? btbh_gpu_animation_pose_sample_skin(
                                    batch->animation_pose,
                                    (uint32_t)skin_index,
                                    palette,
                                    joint_count
                                )
                                : btbh_gpu_animation_library_sample_skin(
                                    batch->animation_library,
                                    batch->animation_index,
                                    batch->animation_time,
                                    (uint32_t)skin_index,
                                    palette,
                                    joint_count
                                );
                            if (valid && keyed) {
                                btbh_gpu_bone_palette_cache_store(
                                    palette_cache,
                                    &palette_key,
                                    palette
                                );
                            }
                        }
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
                            sample_table[table_slot] = sample_count;
                            sample_count += 1u;
                            bone_index += joint_count;
                        }
                    }
                }
            }
            geometry_index += 1u;
        }
    }
    free(sample_table);
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
    telemetry->animation_palette_cache_hit_count = palette_hit_count;
    telemetry->bone_matrix_count = bone_index;
    return true;
}

bool btbh_gpu_scene_prepare_bones_for_test(
    const BTBHGPUSceneGeometryBatch *batches,
    uint32_t batch_count,
    BTBHGPUBonePaletteCache **palette_cache,
    uint32_t **bone_offsets,
    uint32_t *geometry_count,
    float **bone_matrices,
    uint32_t *bone_count,
    uint32_t *palette_hit_count
) {
    BTBHGPUSceneTelemetryReport telemetry;
    SDL_zero(telemetry);
    const bool prepared = btbh_gpu_scene_prepare_bones(
        batches,
        batch_count,
        bone_offsets,
        geometry_count,
        bone_matrices,
        bone_count,
        false,
        &telemetry,
        NULL,
        palette_cache
    );
    if (palette_hit_count != NULL) {
        *palette_hit_count = telemetry.animation_palette_cache_hit_count;
    }
    return prepared;
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
        (lighting->spot_occluder_flags & ~(uint32_t)(
            BTBH_GPU_SCENE_SPOT_OCCLUDER_SUN_ONLY |
            BTBH_GPU_SCENE_SPOT_OCCLUDER_ORTHOGRAPHIC
        )) != 0u ||
        !btbh_gpu_particle_shadow_setting_valid(lighting->particle_shadow) ||
        !isfinite(lighting->point_light_ceiling) ||
        lighting->point_light_ceiling < 0.0f ||
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
        post->ssao_enabled <= 2u &&
        (post->ssao_enabled != 2u ||
         (isfinite(post->ssao_radius) &&
          post->ssao_radius >= 0.0f &&
          isfinite(post->ssao_intensity) &&
          post->ssao_intensity >= 0.0f &&
          isfinite(post->ssao_power) &&
          post->ssao_power >= 0.0f &&
          isfinite(post->ssao_bias) &&
          post->ssao_bias >= 0.0f &&
          post->ssao_contact <= 1u &&
          post->ssao_debug_view <= 1u)) &&
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
        post->opaque_sort < BTBH_GPU_SCENE_OPAQUE_SORT_COUNT &&
        (post->flags & ~(uint32_t)(
            BTBH_GPU_SCENE_POST_ADDITIVE_CASCADES_KEEP_HBAO |
            BTBH_GPU_SCENE_POST_SSR_SURFACE_REFLECTIVITY |
            BTBH_GPU_SCENE_POST_SSR_FACING_HITS
        )) == 0u &&
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
        !btbh_gpu_texture_is_array(post->lut_texture) &&
        !btbh_gpu_texture_is_heightfield(post->lut_texture) &&
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

/* RFC 0007: each batch's dig mask for this scene (the dug_by terrain attached to this window, once it has a mask). */
static bool btbh_gpu_scene_prepare_dig_bindings(
    const BTBHGPUWindow *window,
    BTBHGPUSceneRuntime *runtime,
    const BTBHGPUSceneGeometryBatch *batches,
    uint32_t batch_count
) {
    if (batch_count > runtime->dig_binding_capacity) {
        BTBHGPUSdfTerrainMaskBinding *bindings = (BTBHGPUSdfTerrainMaskBinding *)realloc(
            runtime->dig_bindings,
            sizeof(BTBHGPUSdfTerrainMaskBinding) * batch_count
        );
        if (bindings == NULL) {
            return false;
        }
        runtime->dig_bindings = bindings;
        runtime->dig_binding_capacity = batch_count;
    }
    for (uint32_t index = 0u; index < batch_count; ++index) {
        BTBHGPUSdfTerrainMaskBinding *binding = &runtime->dig_bindings[index];
        memset(binding, 0, sizeof(*binding));
        if (batches[index].dug_by != NULL &&
            !btbh_gpu_sdf_terrain_mask_binding(batches[index].dug_by, window, binding)) {
            memset(binding, 0, sizeof(*binding));
        }
    }
    return true;
}

/* Whether a shader file's bytes contain `name` (SPIR-V keeps its resources' names, OpName). */
static bool btbh_gpu_scene_bytes_name(const uint8_t *bytes, size_t byte_count, const char *name) {
    const size_t name_length = strlen(name);
    for (size_t offset = 0u; offset + name_length <= byte_count; ++offset) {
        if (memcmp(bytes + offset, name, name_length) == 0) {
            return true;
        }
    }
    return false;
}

/* RFC 0014: whether a SPIR-V module declares the ClipDistance capability (OpCapability, opcode 17, two words, operand
   32). The capabilities come first, after the five-word header. */
static bool btbh_gpu_scene_spirv_has_clip_distance(const uint8_t *bytes, size_t byte_count) {
    const size_t word_count = byte_count / 4u;
    uint32_t word = 0u;
    if (word_count < 5u) {
        return false;
    }
    (void)memcpy(&word, bytes, sizeof(word));
    if (word != 0x07230203u) {
        return false;
    }
    size_t index = 5u;
    while (index < word_count) {
        (void)memcpy(&word, bytes + index * 4u, sizeof(word));
        const uint32_t length = word >> 16u;
        if ((word & 0xffffu) != 17u || length < 2u || index + 1u >= word_count) {
            return false;
        }
        uint32_t capability = 0u;
        (void)memcpy(&capability, bytes + (index + 1u) * 4u, sizeof(capability));
        if (capability == 32u) {
            return true;
        }
        index += length;
    }
    return false;
}

/* What a scene fragment shader file names (btbh_gpu_scene_check_scene_shader): read once per path and kept in
   runtime->scene_shader_checks. NULL when the file can't be read (the shader load then reports it). */
static const BTBHGPUSceneShaderCheck *btbh_gpu_scene_shader_check(BTBHGPUSceneRuntime *runtime, const char *path) {
    for (uint32_t check = 0u; check < BTBH_GPU_SCENE_SHADER_CHECK_CAPACITY; ++check) {
        const char *checked = runtime->scene_shader_checks[check].path;
        if (checked != NULL && strcmp(checked, path) == 0) {
            return &runtime->scene_shader_checks[check];
        }
    }
    size_t byte_count = 0u;
    uint8_t *bytes = (uint8_t *)SDL_LoadFile(path, &byte_count);
    if (bytes == NULL) {
        return NULL;
    }
    BTBHGPUSceneShaderCheck fresh;
    fresh.has_dig_mask = btbh_gpu_scene_bytes_name(bytes, byte_count, "dig_mask_texture");
    fresh.has_world_detail = btbh_gpu_scene_bytes_name(bytes, byte_count, "world_detail_normal_texture");
    fresh.has_world_detail_table = btbh_gpu_scene_bytes_name(bytes, byte_count, "world_detail_maps");
    fresh.has_source_top = btbh_gpu_scene_bytes_name(bytes, byte_count, "source_top_texture");
    fresh.has_instance_uv_rect = btbh_gpu_scene_bytes_name(bytes, byte_count, "instance_uv_rect");
    fresh.has_particle_transmittance = btbh_gpu_scene_bytes_name(bytes, byte_count, "particle_transmittance_texture");
    fresh.has_clip_distance = btbh_gpu_scene_spirv_has_clip_distance(bytes, byte_count);
    SDL_free(bytes);
    const size_t path_length = strlen(path);
    fresh.path = (char *)malloc(path_length + 1u);
    if (fresh.path == NULL) {
        return NULL;
    }
    memcpy(fresh.path, path, path_length + 1u);
    BTBHGPUSceneShaderCheck *slot = &runtime->scene_shader_checks[runtime->scene_shader_check_next];
    runtime->scene_shader_check_next = (runtime->scene_shader_check_next + 1u) % BTBH_GPU_SCENE_SHADER_CHECK_CAPACITY;
    free(slot->path);
    *slot = fresh;
    return slot;
}

/* Older scene shaders bind and draw a batch whose features they don't know without a word: the extra bindings are
   ones they never read. RFC 0007: a dug batch (dug_by) needs 0.5.0's shaders or later, or the ground shows undug
   over its crater patches. RFC 0008: a batch that draws a world detail needs 0.6.0's, or the detail is ignored.
   RFC 0009: a batch that draws a world-detail table needs 0.8.0's, or the table is ignored. So the scene's fragment
   shader must name dig_mask_texture, world_detail_normal_texture or world_detail_maps: SPIR-V keeps the names of its
   resources (OpName). Two files are read: the host's fragment shader, as before 0.8.0, and (from 0.8.0) the variant
   the present draws with for its shadows and radiance cascades (production_model_2d_shadows, the unshadowed one,
   production_model_rc*), when that is another file; a stale variant beside a current fragment shader is refused
   too. Each path is read once. Metal libraries aren't checked, nor the fast unlit shader, which no detailed batch
   takes. False, with the report set, when one doesn't; the files' other faults are left to the shader load, which
   reports them with the path. */
static bool btbh_gpu_scene_check_scene_shader(
    BTBHGPUSceneRuntime *runtime,
    const BTBHGPUSceneGeometryBatch *batches,
    uint32_t batch_count,
    const char *fragment_shader_path,
    const char *unshadowed_fragment_shader_path,
    BTBHGPUShaderFormat shader_format,
    bool shadows_active,
    bool omni_shadows_active,
    bool radiance_cascades_active,
    BTBHGPUSceneReport *report
) {
    if (shader_format != BTBH_GPU_SHADER_FORMAT_SPIRV || fragment_shader_path == NULL ||
        unshadowed_fragment_shader_path == NULL) {
        return true;
    }
    bool dug = false;
    bool detailed = false;
    bool tabled = false;
    for (uint32_t index = 0u; index < batch_count && !(dug && detailed && tabled); ++index) {
        dug = dug || batches[index].dug_by != NULL;
        detailed = detailed || btbh_gpu_world_detail_active(&batches[index]);
        tabled = tabled || btbh_gpu_world_detail_table_active(&batches[index]);
    }
    if (!dug && !detailed && !tabled) {
        return true;
    }
    char variant_path[BTBH_GPU_SCENE_PATH_CAPACITY];
    if (!btbh_gpu_scene_variant_fragment_shader_path(
            fragment_shader_path,
            unshadowed_fragment_shader_path,
            shader_format,
            shadows_active,
            omni_shadows_active,
            radiance_cascades_active,
            variant_path,
            report
        )) {
        return false;
    }
    const char *const paths[2] = { fragment_shader_path, variant_path };
    const uint32_t path_count = strcmp(fragment_shader_path, variant_path) != 0 ? 2u : 1u;
    for (uint32_t which = 0u; which < path_count; ++which) {
        const BTBHGPUSceneShaderCheck *checked = btbh_gpu_scene_shader_check(runtime, paths[which]);
        if (checked == NULL) {
            continue;
        }
        char message[sizeof(report->message)];
        if (dug && !checked->has_dig_mask) {
            (void)snprintf(
                message,
                sizeof(message),
                "A batch has dug_by, and the scene shaders don't sample the dig mask (build them from 0.5.0 or later): "
                "%s",
                paths[which]
            );
            btbh_gpu_scene_error(report, BTBH_GPU_INVALID_ARGUMENT, message);
            return false;
        }
        if (detailed && !checked->has_world_detail) {
            (void)snprintf(
                message,
                sizeof(message),
                "A batch has a world_detail, and the scene shaders don't sample it (build them from 0.6.0 or later): "
                "%s",
                paths[which]
            );
            btbh_gpu_scene_error(report, BTBH_GPU_INVALID_ARGUMENT, message);
            return false;
        }
        if (tabled && !checked->has_world_detail_table) {
            (void)snprintf(
                message,
                sizeof(message),
                "A batch has a world_detail_table, and the scene shaders don't sample it (build them from 0.8.0 or "
                "later): %s",
                paths[which]
            );
            btbh_gpu_scene_error(report, BTBH_GPU_INVALID_ARGUMENT, message);
            return false;
        }
    }
    return true;
}

/* RFC 0010: older radiance-cascade shaders draw a frame with BTBHGPUSceneRadianceCascades.source_tops on without a
   word (the tops texture and the switch go unread), so every source would light every height. With the switch on and
   the cascades running, the trace shader beside the post shaders (production_hrc_trace.comp, built with its
   siblings) must name source_top_texture. Read once per path, as the scene shaders are; Metal libraries aren't
   checked. False, with the report set (BTBH_GPU_INVALID_ARGUMENT, the path), when it doesn't. */
static bool btbh_gpu_scene_check_rc_shader(
    BTBHGPUSceneRuntime *runtime,
    const BTBHGPUScenePost *post,
    const char *post_fragment_shader_path,
    BTBHGPUShaderFormat shader_format,
    bool radiance_cascades_active,
    BTBHGPUSceneReport *report
) {
    if (!radiance_cascades_active || post->radiance_cascades.source_tops == 0u ||
        shader_format != BTBH_GPU_SHADER_FORMAT_SPIRV || post_fragment_shader_path == NULL) {
        return true;
    }
    char trace_path[BTBH_GPU_SCENE_PATH_CAPACITY];
    if (!btbh_gpu_scene_sibling_path(post_fragment_shader_path, "production_hrc_trace.comp.spv", trace_path)) {
        return true;
    }
    const BTBHGPUSceneShaderCheck *checked = btbh_gpu_scene_shader_check(runtime, trace_path);
    if (checked == NULL || checked->has_source_top) {
        return true;
    }
    char message[sizeof(report->message)];
    (void)snprintf(
        message,
        sizeof(message),
        "radiance_cascades.source_tops is on, and the radiance-cascade shaders don't read the sources' tops (build "
        "them from 0.10.0 or later): %s",
        trace_path
    );
    btbh_gpu_scene_error(report, BTBH_GPU_INVALID_ARGUMENT, message);
    return false;
}

/* Whether a batch is a source of the radiance cascades' light through their material pass
   (btbh_gpu_scene_encode_rc_material_sources): unlit and no receiver, or with an emission; never with
   BTBH_GPU_SCENE_BATCH_NO_RC_SOURCE, never refracting, and (RFC 0013) never a lit particle. */
static bool btbh_gpu_scene_batch_material_source(const BTBHGPUSceneGeometryBatch *batch) {
    return (batch->flags & BTBH_GPU_SCENE_BATCH_NO_RC_SOURCE) == 0u &&
        batch->surface_variant != BTBH_GPU_SCENE_SURFACE_REFRACT &&
        !btbh_gpu_particle_lit(batch) &&
        ((batch->unlit_enabled != 0u && batch->rc_receiver == 0u) ||
         (batch->emission_energy > 0.0f &&
          (batch->emission_red != 0u ||
           batch->emission_green != 0u ||
           batch->emission_blue != 0u)));
}

/* RFC 0013: older static vertex shaders draw a batch with BTBH_GPU_SCENE_BATCH_INSTANCE_UV_RECT without a word,
   showing the whole atlas on every instance. So each vertex shader such a batch draws with must name
   instance_uv_rect (SPIR-V keeps a block's member names): the host's production_model.vert, or its table or lifted
   sibling, and with the cascades on, for a batch that is their source, production_rc_material_source.vert beside the
   post vertex shader. Each path is read once, as the scene shaders are; Metal libraries aren't checked. A file that
   can't be read is left to the shader load (or the lift's check), which names it. False, with the report set
   (BTBH_GPU_INVALID_ARGUMENT, the path), when one doesn't name it. */
static bool btbh_gpu_scene_check_uv_rect_shaders(
    BTBHGPUSceneRuntime *runtime,
    const BTBHGPUSceneGeometryBatch *batches,
    uint32_t batch_count,
    const char *vertex_shader_path,
    const char *post_vertex_shader_path,
    BTBHGPUShaderFormat shader_format,
    bool radiance_cascades_active,
    BTBHGPUSceneReport *report
) {
    if (shader_format != BTBH_GPU_SHADER_FORMAT_SPIRV || vertex_shader_path == NULL) {
        return true;
    }
    /* Bit 0 production_model.vert, 1 the table's, 2 the lifted, 3 the lifted table's, 4 the cascades' source. */
    uint32_t wanted = 0u;
    for (uint32_t index = 0u; index < batch_count; ++index) {
        const BTBHGPUSceneGeometryBatch *batch = &batches[index];
        if (!btbh_gpu_particle_uv_rect(batch)) {
            continue;
        }
        const bool lifted = btbh_gpu_height_lift_active(batch);
        const bool tabled = btbh_gpu_world_detail_table_active(batch);
        wanted |= 1u << ((lifted ? 2u : 0u) + (tabled ? 1u : 0u));
        if (radiance_cascades_active && btbh_gpu_scene_batch_material_source(batch)) {
            wanted |= 1u << 4u;
        }
    }
    static const char *const names[5] = {
        NULL,
        "production_model_detail_table.vert.spv",
        "production_model_lift.vert.spv",
        "production_model_detail_table_lift.vert.spv",
        "production_rc_material_source.vert.spv",
    };
    for (uint32_t which = 0u; which < 5u; ++which) {
        if ((wanted & (1u << which)) == 0u) {
            continue;
        }
        char path[BTBH_GPU_SCENE_PATH_CAPACITY];
        if (which == 0u) {
            const size_t length = strlen(vertex_shader_path);
            if (length + 1u > sizeof(path)) {
                continue;
            }
            (void)memcpy(path, vertex_shader_path, length + 1u);
        } else if (!btbh_gpu_scene_sibling_path(
                which == 4u ? post_vertex_shader_path : vertex_shader_path,
                names[which],
                path
            )) {
            continue;
        }
        const BTBHGPUSceneShaderCheck *checked = btbh_gpu_scene_shader_check(runtime, path);
        if (checked == NULL || checked->has_instance_uv_rect) {
            continue;
        }
        char message[sizeof(report->message)];
        (void)snprintf(
            message,
            sizeof(message),
            "A batch's UV rect (INSTANCE_UV_RECT) isn't read by its vertex shader (build the shaders from 0.11.0 or "
            "later): %s",
            path
        );
        btbh_gpu_scene_error(report, BTBH_GPU_INVALID_ARGUMENT, message);
        return false;
    }
    return true;
}

/* RFC 0013 phase 2: older lit particle and scene shaders would draw a frame with the particle transmittance map without
   a word (its binding goes unread), so no puff or street would take the smoke's shadow. On a frame that draws the map
   (particle_shadow_active), the lit particles' shader (production_particle_lit.frag or its _2d_shadows sibling, beside
   the host's fragment shader), when a lit particle draws, and with particle_shadow 2 the scene fragment shaders (the
   host's and the variant the frame draws with, as btbh_gpu_scene_check_scene_shader reads them) must name
   particle_transmittance_texture. Each path is read once; Metal libraries aren't checked; a file that can't be read is
   left to the shader load, which names it. False, with the report set (BTBH_GPU_INVALID_ARGUMENT, the path), when one
   doesn't name it. */
static bool btbh_gpu_scene_check_particle_shadow_shaders(
    BTBHGPUSceneRuntime *runtime,
    const BTBHGPUSceneGeometryBatch *batches,
    uint32_t batch_count,
    const char *fragment_shader_path,
    const char *unshadowed_fragment_shader_path,
    BTBHGPUShaderFormat shader_format,
    bool particle_shadow_active,
    uint8_t particle_shadow,
    bool shadows_active,
    bool omni_shadows_active,
    bool radiance_cascades_active,
    BTBHGPUSceneReport *report
) {
    if (!particle_shadow_active || shader_format != BTBH_GPU_SHADER_FORMAT_SPIRV || fragment_shader_path == NULL ||
        unshadowed_fragment_shader_path == NULL) {
        return true;
    }
    char message[sizeof(report->message)];
    if (btbh_gpu_scene_batches_have_lit_particles(batches, batch_count)) {
        char particle_path[BTBH_GPU_SCENE_PATH_CAPACITY];
        const char *name = omni_shadows_active
            ? "production_particle_lit.frag.spv"
            : "production_particle_lit_2d_shadows.frag.spv";
        if (btbh_gpu_scene_sibling_path(fragment_shader_path, name, particle_path)) {
            const BTBHGPUSceneShaderCheck *checked = btbh_gpu_scene_shader_check(runtime, particle_path);
            if (checked != NULL && !checked->has_particle_transmittance) {
                (void)snprintf(
                    message,
                    sizeof(message),
                    "particle_shadow is set, and a lit particle shader doesn't read its map (build the shaders from "
                    "0.14.0 or later): %s",
                    particle_path
                );
                btbh_gpu_scene_error(report, BTBH_GPU_INVALID_ARGUMENT, message);
                return false;
            }
        }
    }
    if ((particle_shadow & BTBH_GPU_SCENE_PARTICLE_SHADOW_READERS) != BTBH_GPU_SCENE_PARTICLE_SHADOW_GROUND) {
        return true;
    }
    char variant_path[BTBH_GPU_SCENE_PATH_CAPACITY];
    if (!btbh_gpu_scene_variant_fragment_shader_path(
            fragment_shader_path,
            unshadowed_fragment_shader_path,
            shader_format,
            shadows_active,
            omni_shadows_active,
            radiance_cascades_active,
            variant_path,
            report
        )) {
        return false;
    }
    const char *const paths[2] = { fragment_shader_path, variant_path };
    const uint32_t path_count = strcmp(fragment_shader_path, variant_path) != 0 ? 2u : 1u;
    for (uint32_t which = 0u; which < path_count; ++which) {
        const BTBHGPUSceneShaderCheck *checked = btbh_gpu_scene_shader_check(runtime, paths[which]);
        if (checked == NULL || checked->has_particle_transmittance) {
            continue;
        }
        (void)snprintf(
            message,
            sizeof(message),
            "particle_shadow is 2, and a scene shader doesn't read its map (build the shaders from 0.14.0 or later): "
            "%s",
            paths[which]
        );
        btbh_gpu_scene_error(report, BTBH_GPU_INVALID_ARGUMENT, message);
        return false;
    }
    return true;
}

static void btbh_gpu_scene_dig_uniforms(
    const BTBHGPUSdfTerrainMaskBinding *binding,
    BTBHGPUSceneDigMaskUniforms *uniforms
) {
    memset(uniforms, 0, sizeof(*uniforms));
    if (binding->texture != NULL) {
        memcpy(uniforms->origin, binding->origin, sizeof(uniforms->origin));
        memcpy(uniforms->scale, binding->scale, sizeof(uniforms->scale));
    }
}

/* The dig mask's texture for a batch, or the fallback texture where it has none (its uniforms then say so). */
static SDL_GPUTextureSamplerBinding btbh_gpu_scene_dig_sampler(
    const BTBHGPUSceneRuntime *runtime,
    const BTBHGPUSdfTerrainMaskBinding *binding
) {
    SDL_GPUTextureSamplerBinding sampler;
    SDL_zero(sampler);
    if (binding->texture != NULL) {
        sampler.texture = binding->texture;
        sampler.sampler = binding->sampler;
    } else {
        sampler.texture = runtime->fallback_texture->texture;
        sampler.sampler = runtime->fallback_texture->sampler;
    }
    return sampler;
}

/* RFC 0008: a batch's three world-detail textures (albedo, normal, ORM), or the fallbacks where it lacks one (its
   uniforms then say so, and they are never sampled). */
static void btbh_gpu_scene_world_detail_samplers(
    const BTBHGPUSceneRuntime *runtime,
    const BTBHGPUSceneGeometryBatch *batch,
    SDL_GPUTextureSamplerBinding samplers[3]
) {
    const BTBHGPUSceneWorldDetail *detail =
        btbh_gpu_world_detail_active(batch) ? batch->world_detail : NULL;
    const BTBHGPUTexture *textures[3] = {
        detail != NULL && detail->albedo_texture != NULL ? detail->albedo_texture : runtime->fallback_texture,
        detail != NULL && detail->normal_texture != NULL ? detail->normal_texture : runtime->normal_fallback_texture,
        detail != NULL && detail->orm_texture != NULL ? detail->orm_texture : runtime->fallback_texture,
    };
    for (uint32_t index = 0u; index < 3u; ++index) {
        SDL_zero(samplers[index]);
        samplers[index].texture = textures[index]->texture;
        samplers[index].sampler = textures[index]->sampler;
    }
}

/* RFC 0013: the lit particles' block for a pass (production_particle_lit.frag.hlsl's ParticleFrame): the frame's
   inverse view-projection and render size, whether the scene's distance is bound, the HDR composite's fog when it
   fogs (hdr_composite_active and the post's fog on), and the radiance cascades' field when they lit this frame
   (their light, this frame's, is in hrc_quadrant_textures[hrc_history_read_index] once their compose ran), and (phase
   2) whether the particle transmittance map is drawn this frame. */
static void btbh_gpu_scene_particle_frame(
    const BTBHGPUSceneRuntime *runtime,
    const BTBHGPUScenePost *post,
    const float inverse_view_projection[16],
    uint32_t render_width,
    uint32_t render_height,
    bool scene_distance_ready,
    bool hdr_composite_active,
    bool radiance_cascades_lit,
    bool particle_shadow_active,
    BTBHGPUSceneParticleFrameUniforms *frame
) {
    SDL_zerop(frame);
    /* RFC 0013 phase 2: the particle transmittance map, drawn this frame (BTBHGPUSceneLighting.particle_shadow 1 or
       2). */
    frame->shadow_controls[0] = particle_shadow_active ? 1.0f : 0.0f;
    (void)memcpy(frame->inverse_view_projection, inverse_view_projection, sizeof(frame->inverse_view_projection));
    frame->controls[0] = 1.0f / (float)render_width;
    frame->controls[1] = 1.0f / (float)render_height;
    frame->controls[2] = scene_distance_ready ? 1.0f : 0.0f;
    frame->controls[3] = hdr_composite_active && post->fog_enabled != 0u ? 1.0f : 0.0f;
    frame->fog_color_density[0] = post->fog_color[0];
    frame->fog_color_density[1] = post->fog_color[1];
    frame->fog_color_density[2] = post->fog_color[2];
    frame->fog_color_density[3] = post->fog_density;
    frame->fog_shape[0] = post->fog_start_distance;
    frame->fog_shape[1] = post->fog_maximum_opacity;
    frame->fog_shape[2] = post->fog_enabled != 0u ? 1.0f : 0.0f;
    for (uint32_t edge = 0u; edge < 4u; ++edge) {
        frame->fog_edge_bounds[edge] = post->fog_edge_bounds[edge];
    }
    frame->fog_edge[0] = post->fog_edge_fade_distance;
    frame->fog_edge[1] = post->fog_enabled != 0u && post->fog_edge_enabled != 0u ? 1.0f : 0.0f;
    frame->fog_edge[2] = post->fog_edge_clear_distance;
    const BTBHGPUSceneRadianceCascadesRuntime *cascades = &runtime->radiance_cascades;
    const uint32_t read_index = cascades->hrc_history_read_index;
    if (radiance_cascades_lit &&
        cascades->hrc_history_valid &&
        read_index <= 2u &&
        cascades->hrc_quadrant_textures[read_index] != NULL &&
        cascades->sampler != NULL &&
        cascades->hrc_history_cell_size > 0.0f &&
        cascades->source_field_width != 0u &&
        cascades->source_field_height != 0u &&
        post->radiance_cascades.indirect_intensity > 0.0f) {
        frame->cascade_origin[0] = cascades->hrc_history_origin_x;
        frame->cascade_origin[1] = cascades->hrc_history_origin_z;
        frame->cascade_origin[2] = 1.0f / cascades->hrc_history_cell_size;
        frame->cascade_origin[3] = 2.0f * post->radiance_cascades.indirect_intensity;
        frame->cascade_field[0] = (float)cascades->source_field_width;
        frame->cascade_field[1] = (float)cascades->source_field_height;
        frame->cascade_field[2] = post->radiance_cascades.source_tops != 0u ? 1.0f : 0.0f;
    }
}

/* RFC 0013: a lit particle's textures, in production_particle_lit.frag's slots: the albedo, the normal (rg) and
   thickness (b), the scene's distance (a fallback never read without it), the emission, the cascades' quadrant atlas
   (a fallback never read when they didn't light this frame), then the variant's shadow maps: the sun's through the
   comparison sampler, the fill's and the spot's, and the omni cube; then (RFC 0013 phase 2) the particle
   transmittance map, or a fallback never read when the frame draws none. sun_map, spot_map and transmittance_map: the
   view's (RFC 0014: view 0's, runtime->directional_shadow_textures[0] and [2] and particle_transmittance_texture, or a
   second view's own). */
static void btbh_gpu_scene_bind_lit_particle(
    BTBHGPUSceneRunVerify *verify,
    SDL_GPURenderPass *pass,
    const BTBHGPUSceneRuntime *runtime,
    const BTBHGPUTexture *albedo,
    const BTBHGPUTexture *normal,
    const BTBHGPUTexture *emission,
    bool scene_distance_ready,
    bool radiance_cascades_lit,
    bool shadows_active,
    bool omni_shadows_active,
    bool particle_shadow_active,
    SDL_GPUTexture *sun_map,
    SDL_GPUTexture *spot_map,
    SDL_GPUTexture *transmittance_map
) {
    SDL_GPUTextureSamplerBinding samplers[10];
    SDL_zeroa(samplers);
    samplers[0].texture = albedo->texture;
    samplers[0].sampler = albedo->sampler;
    samplers[1].texture = normal->texture;
    samplers[1].sampler = normal->sampler;
    samplers[2].texture = scene_distance_ready
        ? runtime->scene_distance_texture
        : runtime->fallback_texture->texture;
    /* (Nearest and clamped: the fallback's sampler. The shader reads the texel at its pixel.) */
    samplers[2].sampler = runtime->fallback_texture->sampler;
    samplers[3].texture = emission->texture;
    samplers[3].sampler = emission->sampler;
    const BTBHGPUSceneRadianceCascadesRuntime *cascades = &runtime->radiance_cascades;
    const uint32_t read_index = cascades->hrc_history_read_index;
    const bool cascade_light =
        radiance_cascades_lit &&
        read_index <= 2u &&
        cascades->hrc_quadrant_textures[read_index] != NULL &&
        cascades->sampler != NULL;
    samplers[4].texture = cascade_light
        ? cascades->hrc_quadrant_textures[read_index]
        : runtime->fallback_texture->texture;
    samplers[4].sampler = cascade_light ? cascades->sampler : runtime->scene_color_sampler;
    uint32_t count = 5u;
    if (shadows_active) {
        samplers[5].texture = sun_map;
        samplers[5].sampler = runtime->directional_shadow_compare_sampler;
        samplers[6].texture = runtime->directional_shadow_textures[1];
        samplers[6].sampler = runtime->directional_shadow_sampler;
        samplers[7].texture = spot_map;
        samplers[7].sampler = runtime->directional_shadow_sampler;
        count = 8u;
        if (omni_shadows_active) {
            samplers[8].texture = runtime->omni_shadow_texture;
            samplers[8].sampler = runtime->directional_shadow_sampler;
            count = 9u;
        }
        samplers[count].texture = particle_shadow_active
            ? transmittance_map
            : runtime->fallback_texture->texture;
        samplers[count].sampler = particle_shadow_active
            ? runtime->directional_shadow_sampler
            : runtime->fallback_texture->sampler;
        count += 1u;
    }
    btbh_gpu_scene_run_bind_fragment_samplers(verify, pass, samplers, count);
}

/* RFC 0013 phase 2: production_particle_transmittance.vert.hlsl's TransmittanceUniforms (the vertex block; 112
   bytes). The fragment block is four floats: the optical depth's scale and the alpha cutoff. */
typedef struct BTBHGPUSceneTransmittanceUniforms {
    float light_view_projection[16];
    float material_color[4];
    float uv_transform[4];
    /* x the billboard mode, y 1 with the UV rect, zw 0 */
    uint32_t options[4];
} BTBHGPUSceneTransmittanceUniforms;

/* RFC 0013 phase 2's name check: the map's shaders, production_particle_transmittance.vert and .frag, are new in 0.14.0
   and sit beside the host's static vertex shader. A missing one fails the present with its path; a SPIR-V one must
   name its block's member (transmittance_options, transmittance_controls), so that a file of that name that doesn't
   draw the map isn't drawn with. Read when the map's pipeline is made (once). */
static bool btbh_gpu_scene_check_transmittance_shader(
    const char *path,
    const char *name,
    BTBHGPUShaderFormat project_format,
    BTBHGPUSceneReport *report
) {
    size_t byte_count = 0u;
    uint8_t *bytes = (uint8_t *)SDL_LoadFile(path, &byte_count);
    char message[sizeof(report->message)];
    if (bytes == NULL || byte_count == 0u) {
        SDL_free(bytes);
        (void)snprintf(
            message,
            sizeof(message),
            "A particle shadow's map shader is missing (build the shaders from 0.14.0 or later): %s",
            path
        );
        btbh_gpu_scene_error(report, BTBH_GPU_INVALID_ARGUMENT, message);
        return false;
    }
    const bool named =
        project_format != BTBH_GPU_SHADER_FORMAT_SPIRV || btbh_gpu_scene_bytes_name(bytes, byte_count, name);
    SDL_free(bytes);
    if (!named) {
        (void)snprintf(
            message,
            sizeof(message),
            "A particle shadow's map shader doesn't draw the map (build the shaders from 0.14.0 or later): %s",
            path
        );
        btbh_gpu_scene_error(report, BTBH_GPU_INVALID_ARGUMENT, message);
        return false;
    }
    return true;
}

/* RFC 0013 phase 2: the particle transmittance map and its pipeline, made where missing on a frame that draws the map
   (BTBHGPUSceneFrameSetup.particle_shadow_active; `particle_shadow` the lighting's byte): shared by the present and
   the prewarms through btbh_gpu_scene_ensure_model_pipelines.
   The map is RGBA16F at half the sun map's size each way, or a quarter (BTBH_GPU_SCENE_PARTICLE_SHADOW_QUARTER), made
   anew when that changes. Its pipeline draws the flagged batches with no depth target, the colour blended ADD (one,
   one) and the alpha MIN, culling nothing. False, with the report set, when one can't be made. */
static bool btbh_gpu_scene_ensure_particle_shadow(
    BTBHGPUWindow *window,
    const char *vertex_shader_path,
    BTBHGPUShaderFormat project_format,
    bool particle_shadow_active,
    uint8_t particle_shadow,
    BTBHGPUSceneReport *report
) {
    if (!particle_shadow_active) {
        return true;
    }
    BTBHGPUSceneRuntime *runtime = window->scene_runtime;
    SDL_GPUDevice *device = window->device->gpu;
    const uint32_t width = btbh_gpu_particle_shadow_size(runtime->directional_shadow_width, particle_shadow);
    const uint32_t height = btbh_gpu_particle_shadow_size(runtime->directional_shadow_height, particle_shadow);
    if (runtime->particle_transmittance_texture != NULL &&
        (runtime->particle_transmittance_width != width || runtime->particle_transmittance_height != height)) {
        SDL_ReleaseGPUTexture(device, runtime->particle_transmittance_texture);
        runtime->particle_transmittance_texture = NULL;
    }
    if (runtime->particle_transmittance_texture == NULL) {
        SDL_GPUTextureCreateInfo info;
        SDL_zero(info);
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = width;
        info.height = height;
        info.layer_count_or_depth = 1u;
        info.num_levels = 1u;
        info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        runtime->particle_transmittance_texture = SDL_CreateGPUTexture(device, &info);
        if (runtime->particle_transmittance_texture == NULL) {
            btbh_gpu_scene_error(report, BTBH_GPU_DEVICE_FAILURE, SDL_GetError());
            return false;
        }
        runtime->particle_transmittance_width = width;
        runtime->particle_transmittance_height = height;
    }
    if (runtime->particle_transmittance_pipeline != NULL) {
        return true;
    }
    const char *extension = project_format == BTBH_GPU_SHADER_FORMAT_METALLIB ? "metallib" : "spv";
    char vertex_path[BTBH_GPU_SCENE_PATH_CAPACITY];
    char fragment_path[BTBH_GPU_SCENE_PATH_CAPACITY];
    char filename[128];
    (void)snprintf(filename, sizeof(filename), "production_particle_transmittance.vert.%s", extension);
    const bool vertex_named = btbh_gpu_scene_sibling_path(vertex_shader_path, filename, vertex_path);
    (void)snprintf(filename, sizeof(filename), "production_particle_transmittance.frag.%s", extension);
    if (!vertex_named || !btbh_gpu_scene_sibling_path(vertex_shader_path, filename, fragment_path)) {
        btbh_gpu_scene_error(report, BTBH_GPU_INVALID_ARGUMENT, "Particle transmittance shader path is too long");
        return false;
    }
    if (!btbh_gpu_scene_check_transmittance_shader(vertex_path, "transmittance_options", project_format, report) ||
        !btbh_gpu_scene_check_transmittance_shader(fragment_path, "transmittance_controls", project_format, report)) {
        return false;
    }
    const SDL_GPUShaderFormat format = btbh_gpu_scene_shader_format(project_format);
    const char *entrypoint = project_format == BTBH_GPU_SHADER_FORMAT_METALLIB ? "main0" : "main";
    /* The vertex stage: the instances, the block; the fragment stage: the albedo, its block. */
    SDL_GPUShader *vertex_shader = btbh_gpu_scene_load_shader(
        device, vertex_path, entrypoint, format, SDL_GPU_SHADERSTAGE_VERTEX, 1u, 0u, 1u, report);
    if (vertex_shader == NULL) {
        return false;
    }
    SDL_GPUShader *fragment_shader = btbh_gpu_scene_load_shader(
        device, fragment_path, entrypoint, format, SDL_GPU_SHADERSTAGE_FRAGMENT, 0u, 1u, 1u, report);
    if (fragment_shader == NULL) {
        SDL_ReleaseGPUShader(device, vertex_shader);
        return false;
    }
    SDL_GPUColorTargetDescription color_target;
    SDL_GPUVertexBufferDescription vertex_buffer;
    SDL_GPUVertexAttribute attributes[3];
    SDL_GPUGraphicsPipelineCreateInfo info;
    SDL_zero(color_target);
    SDL_zero(vertex_buffer);
    SDL_zeroa(attributes);
    SDL_zero(info);
    color_target.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    color_target.blend_state.enable_blend = true;
    color_target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    color_target.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    color_target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    color_target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    color_target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    color_target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_MIN;
    color_target.blend_state.color_write_mask = 0x0fu;
    vertex_buffer.slot = 0u;
    vertex_buffer.pitch = BTBH_GPU_MODEL_VERTEX_STRIDE;
    vertex_buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    attributes[0].location = 0u;
    attributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    attributes[0].offset = 0u;
    attributes[1].location = 1u;
    attributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    attributes[1].offset = BTBH_GPU_MODEL_COLOR_OFFSET;
    attributes[2].location = 2u;
    attributes[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    attributes[2].offset = 24u;
    info.vertex_shader = vertex_shader;
    info.fragment_shader = fragment_shader;
    info.vertex_input_state.vertex_buffer_descriptions = &vertex_buffer;
    info.vertex_input_state.num_vertex_buffers = 1u;
    info.vertex_input_state.vertex_attributes = attributes;
    info.vertex_input_state.num_vertex_attributes = 3u;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;
    info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.target_info.color_target_descriptions = &color_target;
    info.target_info.num_color_targets = 1u;
    info.target_info.has_depth_stencil_target = false;
    runtime->particle_transmittance_pipeline = btbh_gpu_create_graphics_pipeline(device, &info);
    SDL_ReleaseGPUShader(device, fragment_shader);
    SDL_ReleaseGPUShader(device, vertex_shader);
    if (runtime->particle_transmittance_pipeline == NULL) {
        btbh_gpu_scene_error(report, BTBH_GPU_DEVICE_FAILURE, SDL_GetError());
        return false;
    }
    return true;
}

/* RFC 0013 phase 2: the particle transmittance map, drawn after the shadow maps with the sun map's matrix (the one the
   receivers read). Cleared to (0, 0, 0, 1); each batch with BTBH_GPU_SCENE_BATCH_PARTICLE_SHADOW (that its
   directional_shadow_visibility byte lets in) draws its instances there, with its albedo, colour, UV transform and
   rect, as production_particle_transmittance.vert lays them out. False, with SDL's error set, when the pass failed.
   map: runtime->particle_transmittance_texture, or (RFC 0014) a second view's own. */
static bool btbh_gpu_scene_render_particle_transmittance(
    SDL_GPUCommandBuffer *command,
    BTBHGPUSceneRuntime *runtime,
    SDL_GPUTexture *map,
    const float light_view_projection[16],
    SDL_GPUBuffer *transient_instance_buffer,
    const BTBHGPUSceneGeometryBatch *batches,
    const uint8_t *directional_shadow_visibility,
    uint32_t batch_count
) {
    SDL_GPUColorTargetInfo target;
    SDL_zero(target);
    target.texture = map;
    target.clear_color.r = 0.0f;
    target.clear_color.g = 0.0f;
    target.clear_color.b = 0.0f;
    target.clear_color.a = 1.0f;
    target.load_op = SDL_GPU_LOADOP_CLEAR;
    target.store_op = SDL_GPU_STOREOP_STORE;
    target.cycle = true;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, &target, 1u, NULL);
    if (pass == NULL) {
        return false;
    }
    SDL_BindGPUGraphicsPipeline(pass, runtime->particle_transmittance_pipeline);
    BTBHGPUSceneTransmittanceUniforms uniforms;
    SDL_zero(uniforms);
    (void)memcpy(uniforms.light_view_projection, light_view_projection, sizeof(uniforms.light_view_projection));
    SDL_GPUBuffer *bound_index_buffer = NULL;
    for (uint32_t batch_index = 0u; batch_index < batch_count; ++batch_index) {
        const BTBHGPUSceneGeometryBatch *batch = &batches[batch_index];
        if (!btbh_gpu_particle_shadow(batch) ||
            (directional_shadow_visibility != NULL && directional_shadow_visibility[batch_index] == 0u)) {
            continue;
        }
        uniforms.material_color[0] = (float)batch->albedo_red / 255.0f;
        uniforms.material_color[1] = (float)batch->albedo_green / 255.0f;
        uniforms.material_color[2] = (float)batch->albedo_blue / 255.0f;
        uniforms.material_color[3] = (float)batch->albedo_alpha / 255.0f;
        uniforms.uv_transform[0] = batch->uv_scale_x;
        uniforms.uv_transform[1] = batch->uv_scale_y;
        uniforms.uv_transform[2] = batch->uv_offset_x;
        uniforms.uv_transform[3] = batch->uv_offset_y;
        uniforms.options[0] = (uint32_t)batch->billboard_mode;
        uniforms.options[1] = btbh_gpu_particle_uv_rect(batch) ? 1u : 0u;
        SDL_PushGPUVertexUniformData(command, 0u, &uniforms, (uint32_t)sizeof(uniforms));
        const float controls[4] = {
            btbh_gpu_particle_lit(batch) && batch->surface_parameters1[3] > 0.0f ? batch->surface_parameters1[3] : 1.0f,
            batch->alpha_cutoff,
            0.0f,
            0.0f,
        };
        SDL_PushGPUFragmentUniformData(command, 0u, controls, (uint32_t)sizeof(controls));
        const BTBHGPUTexture *albedo =
            batch->albedo_texture != NULL ? batch->albedo_texture : runtime->fallback_texture;
        SDL_GPUTextureSamplerBinding albedo_binding;
        SDL_zero(albedo_binding);
        albedo_binding.texture = albedo->texture;
        albedo_binding.sampler = albedo->sampler;
        SDL_BindGPUFragmentSamplers(pass, 0u, &albedo_binding, 1u);
        SDL_GPUBuffer *instance_buffer =
            batch->instance_buffer != NULL ? batch->instance_buffer->buffer : transient_instance_buffer;
        SDL_BindGPUVertexStorageBuffers(pass, 0u, &instance_buffer, 1u);
        const uint32_t geometry_count =
            batch->kind == BTBH_GPU_SCENE_GEOMETRY_MODEL ? batch->model->primitive_count : 1u;
        for (uint32_t geometry_index = 0u; geometry_index < geometry_count; ++geometry_index) {
            if (!btbh_gpu_scene_batch_uses_geometry(batch, geometry_index)) {
                continue;
            }
            const BTBHGPUSceneGeometryDraw geometry = btbh_gpu_scene_geometry_draw(batch, geometry_index);
            SDL_GPUBufferBinding vertex_binding;
            SDL_zero(vertex_binding);
            vertex_binding.buffer = geometry.vertex_buffer;
            SDL_BindGPUVertexBuffers(pass, 0u, &vertex_binding, 1u);
            if (geometry.index_buffer != NULL && geometry.index_count > 0u) {
                if (geometry.index_buffer != bound_index_buffer) {
                    SDL_GPUBufferBinding index_binding;
                    SDL_zero(index_binding);
                    index_binding.buffer = geometry.index_buffer;
                    SDL_BindGPUIndexBuffer(pass, &index_binding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
                    bound_index_buffer = geometry.index_buffer;
                }
                SDL_DrawGPUIndexedPrimitives(
                    pass,
                    geometry.index_count,
                    batch->instance_count,
                    geometry.first_index,
                    geometry.vertex_base,
                    batch->first_instance
                );
            } else {
                SDL_DrawGPUPrimitives(
                    pass,
                    geometry.vertex_count,
                    batch->instance_count,
                    (uint32_t)geometry.vertex_base,
                    batch->first_instance
                );
            }
        }
    }
    SDL_EndGPURenderPass(pass);
    return true;
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
    /* RFC 0014: in the spot map, the batches a view's own map takes besides spot_shadow_visible (NULL: every one). */
    const uint8_t *spot_visibility,
    uint32_t batch_count,
    uint8_t shadow_layer,
    bool cycle_texture,
    bool bind_telemetry,
    BTBHGPUSceneTelemetryReport *telemetry,
    bool runs_enabled,
    BTBHGPUSceneRunVerify *verify
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
    btbh_gpu_scene_run_verify_begin_pass(verify, "shadow");

    uint32_t geometry_cursor = 0u;
    SDL_GPUGraphicsPipeline *bound_pipeline = NULL;
    SDL_GPUBuffer *bound_index_buffer = NULL;
    bool shadow_uniform_bound = false;
    /* The dig mask last given to the fragment-stage pipelines in this pass (RFC 0007). */
    bool dig_bound = false;
    SDL_GPUTexture *bound_dig_texture = NULL;
    /* RFC 0011: the batch whose uniforms, pipeline, dig mask and storage buffers the pass holds (NULL before the first
       draw and after a skinned geometry); a batch that continues its run binds and pushes none of them. The frame's
       part of the shadow uniforms is filled once. */
    const BTBHGPUSceneGeometryBatch *run_batch = NULL;
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
            /* The casters: opaque batches; in the spot (or omni) map those with spot_shadow_visible, in the
               directional ones those the visibility bytes and BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW let in. */
            if (batch->transparency_enabled != 0u ||
                (shadow_index == BTBH_GPU_SCENE_SHADOW_SPOT
                    ? batch->spot_shadow_visible == 0u ||
                        (spot_visibility != NULL && spot_visibility[batch_index] == 0u)
                    : ((directional_shadow_visibility != NULL &&
                        directional_shadow_visibility[batch_index] == 0u) ||
                       (batch->flags & BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW) != 0u))) {
                continue;
            }
            const bool animated = bone_offset != UINT32_MAX;
            /* A dug batch (RFC 0007) takes the pipeline with a fragment stage once its terrain has a mask, so it
               casts no shadow over the crater. */
            const BTBHGPUSdfTerrainMaskBinding *dig = &runtime->dig_bindings[batch_index];
            const bool dug = dig->texture != NULL;
            const bool requires_shadow_discard =
                btbh_gpu_scene_surface_requires_shadow_discard(
                    batch->surface_variant
                ) || dug;
            const bool uses_fast_shadow =
                !animated &&
                !dug &&
                btbh_gpu_scene_surface_uses_fast_shadow_vertex(
                    batch->surface_variant
                ) &&
                batch->billboard_mode ==
                    BTBH_GPU_SCENE_BILLBOARD_DISABLED;
            const bool continues_run =
                runs_enabled &&
                !animated &&
                run_batch != NULL &&
                (run_batch == batch || btbh_gpu_scene_batch_continues_run(run_batch, batch));
            /* BTBH_GPU_SCENE_RUNS=verify: a draw that continues the run rebuilds its state and compares it. */
            const bool run_check = continues_run && verify != NULL;
            if (continues_run) {
                telemetry->shadow_run_draw_count += 1u;
                if (bind_telemetry) {
                    telemetry->shadow_pipeline_bind_skip_count += 1u;
                }
            }
            if (!continues_run || run_check) {
                btbh_gpu_scene_run_verify_draw(verify, batch_index, run_check);
                if (uses_fast_shadow) {
                    if (!shadow_uniform_bound) {
                        /* (The fast shadow shader reads the block's first 64 bytes, the light's matrix.) */
                        btbh_gpu_scene_run_push_vertex_uniforms(
                            verify,
                            command,
                            0u,
                            light_view_projection,
                            sizeof(float) * 16u,
                            &btbh_gpu_scene_run_shadow_layout
                        );
                        shadow_uniform_bound = true;
                    }
                } else {
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
                    btbh_gpu_scene_run_push_vertex_uniforms(
                        verify,
                        command,
                        0u,
                        &uniforms,
                        sizeof(uniforms),
                        &btbh_gpu_scene_run_shadow_layout
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
                if (run_check) {
                    btbh_gpu_scene_run_verify_pipeline(verify, shadow_pipeline, bound_pipeline);
                } else if (shadow_pipeline != bound_pipeline) {
                    SDL_BindGPUGraphicsPipeline(pass, shadow_pipeline);
                    bound_pipeline = shadow_pipeline;
                    if (bind_telemetry) {
                        telemetry->shadow_pipeline_bind_count += 1u;
                    }
                } else if (bind_telemetry) {
                    telemetry->shadow_pipeline_bind_skip_count += 1u;
                }
                if (!uses_fast_shadow && requires_shadow_discard &&
                    (!dig_bound || bound_dig_texture != dig->texture)) {
                    BTBHGPUSceneDigMaskUniforms dig_uniforms;
                    btbh_gpu_scene_dig_uniforms(dig, &dig_uniforms);
                    btbh_gpu_scene_run_push_fragment_uniforms(
                        verify,
                        command,
                        0u,
                        &dig_uniforms,
                        (uint32_t)sizeof(dig_uniforms),
                        &btbh_gpu_scene_run_dig_layout
                    );
                    const SDL_GPUTextureSamplerBinding dig_sampler = btbh_gpu_scene_dig_sampler(runtime, dig);
                    btbh_gpu_scene_run_bind_fragment_samplers(verify, pass, &dig_sampler, 1u);
                    if (!run_check) {
                        dig_bound = true;
                        bound_dig_texture = dig->texture;
                    }
                }
                if (animated) {
                    SDL_GPUBuffer *storage_buffers[2] = {
                        batch_instance_buffer,
                        bone_buffer
                    };
                    btbh_gpu_scene_run_bind_vertex_storage_buffers(
                        verify,
                        pass,
                        storage_buffers,
                        2u
                    );
                } else if (!uses_fast_shadow) {
                    btbh_gpu_scene_run_bind_vertex_storage_buffers(
                        verify,
                        pass,
                        &batch_instance_buffer,
                        1u
                    );
                }
                if (run_check) {
                    verify->checking = false;
                } else {
                    run_batch = animated ? NULL : batch;
                }
            }
            const BTBHGPUSceneGeometryDraw geometry =
                btbh_gpu_scene_geometry_draw(batch, geometry_index);
            SDL_GPUBuffer *vertex_buffer = geometry.vertex_buffer;
            SDL_GPUBuffer *index_buffer = geometry.index_buffer;
            const uint32_t vertex_count = geometry.vertex_count;
            const uint32_t index_count = geometry.index_count;
            const uint32_t first_index = geometry.first_index;
            const int32_t vertex_base = geometry.vertex_base;
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

/* A fullscreen pass into `target`, cleared first. scissor: the rect it shades (a target view's, at the target's
   size; since 0.15.0), the rest keeping the clear; NULL shades every pixel. */
static bool btbh_gpu_scene_render_fullscreen(
    SDL_GPUCommandBuffer *command,
    const char *debug_label,
    SDL_GPUGraphicsPipeline *pipeline,
    SDL_GPUTexture *target,
    const void *uniforms,
    uint32_t uniform_size,
    SDL_GPUTextureSamplerBinding *samplers,
    uint32_t sampler_count,
    const SDL_Rect *scissor
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
    if (scissor != NULL) {
        SDL_SetGPUScissor(pass, scissor);
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

/* As btbh_gpu_scene_render_fullscreen, drawn over what target holds (a blended pipeline). */
static bool btbh_gpu_scene_render_fullscreen_over(
    SDL_GPUCommandBuffer *command,
    const char *debug_label,
    SDL_GPUGraphicsPipeline *pipeline,
    SDL_GPUTexture *target,
    const void *uniforms,
    uint32_t uniform_size,
    SDL_GPUTextureSamplerBinding *samplers,
    uint32_t sampler_count,
    const SDL_Rect *scissor
) {
    SDL_InsertGPUDebugLabel(command, debug_label);
    SDL_GPUColorTargetInfo color_target;
    SDL_zero(color_target);
    color_target.texture = target;
    color_target.load_op = SDL_GPU_LOADOP_LOAD;
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
    if (scissor != NULL) {
        SDL_SetGPUScissor(pass, scissor);
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
    SDL_GPUBuffer *bone_buffer,
    bool runs_enabled,
    uint32_t *run_draw_count,
    BTBHGPUSceneRunVerify *verify
) {
    if (command == NULL || runtime == NULL || config == NULL ||
        (batches == NULL && batch_count > 0u) ||
        (bone_offsets == NULL && prepared_geometry_count > 0u) ||
        runtime->radiance_cascades.source_field_texture == NULL ||
        runtime->radiance_cascades.source_top_texture == NULL ||
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

    /* The source field and (RFC 0010) the sources' tops, both as the emitter pass left them. */
    SDL_GPUColorTargetInfo targets[2];
    SDL_zeroa(targets);
    targets[0].texture = runtime->radiance_cascades.source_field_texture;
    targets[0].load_op = SDL_GPU_LOADOP_LOAD;
    targets[0].store_op = SDL_GPU_STOREOP_STORE;
    targets[1].texture = runtime->radiance_cascades.source_top_texture;
    targets[1].load_op = SDL_GPU_LOADOP_LOAD;
    targets[1].store_op = SDL_GPU_STOREOP_STORE;
    SDL_InsertGPUDebugLabel(command, "BTBH RC Material Sources");
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(
        command,
        targets,
        2u,
        NULL
    );
    if (pass == NULL) {
        return false;
    }
    btbh_gpu_scene_run_verify_begin_pass(verify, "cascade material source");

    uint32_t geometry_cursor = 0u;
    SDL_GPUGraphicsPipeline *bound_pipeline = NULL;
    SDL_GPUBuffer *bound_index_buffer = NULL;
    /* RFC 0011: the batch whose uniforms, storage buffers, dig mask and samplers the pass holds (NULL before the first
       draw and after a skinned geometry); a batch that continues its run binds and pushes none of them. The frame's
       part of the vertex uniforms is filled once. */
    const BTBHGPUSceneGeometryBatch *run_batch = NULL;
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
    uniforms.ambient_color_energy[0] = origin_x;
    uniforms.ambient_color_energy[1] = origin_z;
    uniforms.ambient_color_energy[2] = inverse_world_width;
    uniforms.ambient_color_energy[3] = inverse_world_height;
    for (uint32_t batch_index = 0u;
         batch_index < batch_count;
         ++batch_index) {
        const BTBHGPUSceneGeometryBatch *batch = &batches[batch_index];
        /* BTBH_GPU_SCENE_BATCH_NO_RC_SOURCE: no source, whatever it shows (since 0.9.0); nor is a lit particle
           (RFC 0013), with an emission or without. Its geometries still count below, for the bone offsets. */
        const bool material_emits = btbh_gpu_scene_batch_material_source(batch);
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
            const bool continues_run =
                runs_enabled &&
                !animated &&
                run_batch != NULL &&
                (run_batch == batch || btbh_gpu_scene_batch_continues_run(run_batch, batch));
            /* BTBH_GPU_SCENE_RUNS=verify: a draw that continues the run rebuilds its state and compares it. */
            const bool run_check = continues_run && verify != NULL;
            if (continues_run) {
                *run_draw_count += 1u;
            }
            if (!continues_run || run_check) {
                btbh_gpu_scene_run_verify_draw(verify, batch_index, run_check);
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
                uniforms.bone_offset = animated ? bone_offset : 0u;
                /* RFC 0013: the source's texture coordinate takes the instance's UV rect too. */
                uniforms.instance_uv_rect = btbh_gpu_particle_uv_rect(batch) ? 1u : 0u;
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
                btbh_gpu_scene_run_push_vertex_uniforms(
                    verify,
                    command,
                    0u,
                    &uniforms,
                    (uint32_t)sizeof(uniforms),
                    &btbh_gpu_scene_run_vertex_layout
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
                /* Bit 9 (512): BTBH_GPU_SCENE_BATCH_INSTANCE_LIGHT_FACTOR (since 0.11.0), the instance's custom x
                   scales its emission here too (production_light_factor.hlsli). */
                fragment_uniforms.unlit_enabled =
                    (uint32_t)batch->unlit_enabled |
                    (batch->rc_receiver != 0u ? 4u : 0u) |
                    (btbh_gpu_scene_light_factor(batch) ? 512u : 0u);
                fragment_uniforms.alpha_cutoff = batch->alpha_cutoff;
                fragment_uniforms.surface_variant =
                    batch->surface_variant ==
                            BTBH_GPU_SCENE_SURFACE_SOFT_PARTICLE
                        ? (uint32_t)BTBH_GPU_SCENE_SURFACE_NONE
                        : (uint32_t)batch->surface_variant;
                fragment_uniforms.surface_time = batch->surface_time;
                btbh_gpu_scene_run_push_fragment_uniforms(
                    verify,
                    command,
                    0u,
                    &fragment_uniforms,
                    (uint32_t)offsetof(
                        BTBHGPUSceneFragmentUniforms,
                        surface_parameters0
                    ),
                    &btbh_gpu_scene_run_fragment_layout
                );

                SDL_GPUBuffer *storage_buffers[2] = {
                    batch_instance_buffer,
                    bone_buffer
                };
                btbh_gpu_scene_run_bind_vertex_storage_buffers(
                    verify,
                    pass,
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
                const BTBHGPUSdfTerrainMaskBinding *dig = &runtime->dig_bindings[batch_index];
                BTBHGPUSceneDigMaskUniforms dig_uniforms;
                btbh_gpu_scene_dig_uniforms(dig, &dig_uniforms);
                btbh_gpu_scene_run_push_fragment_uniforms(
                    verify,
                    command,
                    1u,
                    &dig_uniforms,
                    (uint32_t)sizeof(dig_uniforms),
                    &btbh_gpu_scene_run_dig_layout
                );
                SDL_GPUTextureSamplerBinding samplers[3];
                SDL_zeroa(samplers);
                samplers[0].texture = albedo_texture->texture;
                samplers[0].sampler = albedo_texture->sampler;
                samplers[1].texture = emission_texture->texture;
                samplers[1].sampler = emission_texture->sampler;
                samplers[2] = btbh_gpu_scene_dig_sampler(runtime, dig);
                btbh_gpu_scene_run_bind_fragment_samplers(verify, pass, samplers, 3u);
                if (run_check) {
                    verify->checking = false;
                } else {
                    run_batch = animated ? NULL : batch;
                }
            }

            const BTBHGPUSceneGeometryDraw geometry =
                btbh_gpu_scene_geometry_draw(batch, geometry_index);
            SDL_GPUBuffer *vertex_buffer = geometry.vertex_buffer;
            SDL_GPUBuffer *index_buffer = geometry.index_buffer;
            const uint32_t vertex_count = geometry.vertex_count;
            const uint32_t index_count = geometry.index_count;
            const uint32_t first_index = geometry.first_index;
            const int32_t vertex_base = geometry.vertex_base;
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

/* What a scene frame turns on, worked out from its settings and batches by btbh_gpu_scene_prepare_frame. */
typedef struct BTBHGPUSceneFrameSetup {
    bool radiance_cascades_active;
    bool radiance_cascades_fallback_debug;
    bool temporal_active;
    int temporal_window_width;
    uint8_t upscale_mode;
    uint8_t anti_aliasing;
    bool ambient_occlusion_active;
    bool hbao_active;
    bool ssr_active;
    bool ssao_resolve_active;
    bool bloom_active;
    bool hdr_composite_active;
    bool soft_particles_active;
    /* RFC 0013: a batch is a lit particle (the distance pass runs). */
    bool lit_particles_active;
    bool refraction_active;
    bool auxiliary_geometry_outputs;
    uint32_t shadow_enabled[3];
    bool omni_shadows_active;
    bool shadows_active;
    /* RFC 0013 phase 2: the particle transmittance map is drawn this frame (BTBHGPUSceneLighting.particle_shadow's
       readers not 0, the sun's shadow on, a batch with BTBH_GPU_SCENE_BATCH_PARTICLE_SHADOW), and its setting (who
       reads it, its size). Off, nothing of it is made, drawn or bound but the scene shaders' fallback. */
    bool particle_shadow_active;
    uint8_t particle_shadow;
    /* BTBH_GPU_SCENE_POST_ADDITIVE_CASCADES_KEEP_HBAO where it applies (the HBAO, since 0.12.0 its contact mode and the
       SSAO too): the opaque pass writes the scene's own ambient targets under the cascades (unlit_enabled bit 7 for
       production_model_rc*.frag), the cascades' resolve takes the albedo from the emission target, and the occlusion's
       resolve and the SSR read them as without the cascades. */
    bool scene_ambient_targets;
    /* Its contact mode (ssao_contact 1 with the HBAO, since 0.12.0): the opaque pass writes the emission to the
       ambient target's rgb (unlit_enabled bit 10), and the HBAO's passes run before the cascades' (over the scene they
       compose with their bounce light, which they then leave undarkened), their contact resolve keeping the emission
       from the ambient target. */
    bool scene_contact_targets;
    /* BTBH_GPU_SCENE_POST_ADDITIVE_CASCADES_KEEP_HBAO's transparent part (with any ambient occlusion since 0.11.0; it
       needed the HBAO's in 0.10.0): under additive cascades, transparent unlit batches take the fast unlit shader, as
       without the cascades (btbh_gpu_scene_batch_fast_unlit). True wherever scene_ambient_targets is. */
    bool cascades_fast_unlit;
    /* BTBH_GPU_SCENE_POST_SSR_SURFACE_REFLECTIVITY where it applies (the reflections on, the cascades off): the
       opaque pass writes the reflectivity to the ambient target's alpha and marks unlit surfaces in the emission
       target's (unlit_enabled bit 6), and the SSR reads them. */
    bool ssr_surface_reflectivity;
    /* RFC 0014: a clipped frame of scene views asked for the radiance cascades, and runs without them
       (BTBH_GPU_SCENE_RC_FALLBACK_VIEWS). */
    bool radiance_cascades_held_for_views;
} BTBHGPUSceneFrameSetup;

/* The horizon-based occlusion's passes (BTBHGPUScenePost.ssao_enabled 2): the HBAO at half resolution, its denoise,
   and its resolve laid over `target` (production_hbao_resolve.frag's blend). mode: what it darkens (1 the ambient
   light, BTBH_GPU_SCENE_SSAO_RC_CONTACT_MODE with the radiance cascades, BTBH_GPU_SCENE_HBAO_CONTACT_MODE contact);
   kept_emission: the texture whose rgb the contact mode keeps (the emission target, or the ambient target under
   KEEP_HBAO's contact part, which carries the emission there). scissor: a target view's rect (since 0.15.0), NULL for
   every pixel. False, with SDL's error set, when a pass failed. */
static bool btbh_gpu_scene_encode_hbao(
    SDL_GPUCommandBuffer *command,
    BTBHGPUSceneRuntime *runtime,
    const BTBHGPUScenePost *post,
    const float inverse_view_projection[16],
    float projection_scale_y,
    uint32_t mode,
    SDL_GPUTexture *kept_emission,
    SDL_GPUTexture *target,
    const BTBHGPUSceneScissor *scissor
) {
    SDL_Rect half_rect;
    SDL_Rect render_rect;
    const SDL_Rect *half_scissor =
        btbh_gpu_scene_scissor_at(scissor, runtime->hbao_width, runtime->hbao_height, &half_rect);
    const SDL_Rect *render_scissor = btbh_gpu_scene_scissor_at(
        scissor,
        runtime->scene_color_width,
        runtime->scene_color_height,
        &render_rect
    );
    BTBHGPUSceneHBAOUniforms hbao_uniforms;
    SDL_zero(hbao_uniforms);
    (void)memcpy(
        hbao_uniforms.inverse_view_projection,
        inverse_view_projection,
        sizeof(hbao_uniforms.inverse_view_projection)
    );
    hbao_uniforms.controls[0] = post->ssao_radius;
    hbao_uniforms.controls[1] = post->ssao_intensity;
    hbao_uniforms.controls[2] = post->ssao_power;
    hbao_uniforms.controls[3] = post->ssao_bias;
    hbao_uniforms.options[0] = mode;
    hbao_uniforms.options[1] = post->ssao_debug_view != 0u ? 1u : 0u;
    hbao_uniforms.view[0] = projection_scale_y;
    SDL_GPUTextureSamplerBinding hbao_samplers[2];
    SDL_zeroa(hbao_samplers);
    hbao_samplers[0].texture = runtime->depth_texture;
    hbao_samplers[1].texture = runtime->normal_texture;
    SDL_GPUTextureSamplerBinding denoise_sampler;
    SDL_zero(denoise_sampler);
    denoise_sampler.texture = runtime->hbao_texture;
    SDL_GPUTextureSamplerBinding hbao_resolve_samplers[6];
    SDL_zeroa(hbao_resolve_samplers);
    hbao_resolve_samplers[0].texture = runtime->depth_texture;
    hbao_resolve_samplers[1].texture = runtime->ambient_texture;
    hbao_resolve_samplers[2].texture = runtime->normal_texture;
    hbao_resolve_samplers[3].texture =
        runtime->ambient_specular_texture;
    hbao_resolve_samplers[4].texture =
        runtime->hbao_denoised_texture;
    hbao_resolve_samplers[5].texture = kept_emission;
    hbao_samplers[0].sampler = runtime->scene_color_sampler;
    hbao_samplers[1].sampler = runtime->scene_color_sampler;
    denoise_sampler.sampler = runtime->scene_color_sampler;
    for (uint32_t index = 0u; index < 6u; ++index) {
        hbao_resolve_samplers[index].sampler =
            runtime->scene_color_sampler;
    }
    return btbh_gpu_scene_render_fullscreen(
            command,
            "BTBH HBAO",
            runtime->hbao_pipeline,
            runtime->hbao_texture,
            &hbao_uniforms,
            (uint32_t)sizeof(hbao_uniforms),
            hbao_samplers,
            2u,
            half_scissor
        ) &&
        btbh_gpu_scene_render_fullscreen(
            command,
            "BTBH HBAO Denoise",
            runtime->hbao_denoise_pipeline,
            runtime->hbao_denoised_texture,
            &hbao_uniforms,
            (uint32_t)sizeof(hbao_uniforms),
            &denoise_sampler,
            1u,
            half_scissor
        ) &&
        btbh_gpu_scene_render_fullscreen_over(
            command,
            "BTBH HBAO Resolve",
            runtime->hbao_resolve_pipeline,
            target,
            &hbao_uniforms,
            (uint32_t)sizeof(hbao_uniforms),
            hbao_resolve_samplers,
            6u,
            render_scissor
        );
}

/* ---- RFC 0014: scene views (btbh_gpu_window_scene_present_views, since 0.16.0) ---- */

/* One view of a frame as the present draws it: its camera with the frame's jitter, the lighting and visibility it
   reads, its half of the frame as the passes test it, the shadow maps it reads, its opaque order and what it drew. */
typedef struct BTBHGPUSceneViewFrame {
    const BTBHGPUSceneView *view;
    float view_projection[16];
    float inverse_view_projection[16];
    float projection_scale_y;
    float previous_view_projection[16];
    /* The view's lighting (its spot, lamps and point lights), or the call's. */
    const BTBHGPUSceneLighting *lighting;
    /* The view's own visibility (NULL: every batch the call's lets in); for its sort, the two combined. */
    const uint8_t *visibility;
    const uint8_t *order_visibility;
    /* btbh_gpu_scene_view_planes: zero for the whole frame. */
    float clip_plane[4];
    float uv_plane[4];
    /* The maps it reads: view 0's (slot 0) or its own (slot 1), and their matrices as the receivers read them. */
    uint32_t sun_slot;
    uint32_t spot_slot;
    SDL_GPUTexture *sun_map;
    SDL_GPUTexture *spot_map;
    SDL_GPUTexture *transmittance_map;
    const float *sun_matrix;
    const float *spot_matrix;
    uint32_t *opaque_order;
    uint32_t opaque_order_count;
    /* What it drew (BTBHGPUSceneViewReport). */
    uint32_t draw_count;
    uint32_t instance_count;
    double order_milliseconds;
    uint64_t encode_nanoseconds;
} BTBHGPUSceneViewFrame;

void btbh_gpu_scene_view_planes(
    const float clip[3],
    uint32_t output_width,
    uint32_t output_height,
    uint32_t render_width,
    uint32_t render_height,
    float clip_plane[4],
    float uv_plane[4]
) {
    SDL_memset(clip_plane, 0, sizeof(float) * 4u);
    SDL_memset(uv_plane, 0, sizeof(float) * 4u);
    if ((clip[0] == 0.0f && clip[1] == 0.0f && clip[2] == 0.0f) ||
        output_width == 0u || output_height == 0u || render_width == 0u || render_height == 0u) {
        return;
    }
    const double width = (double)output_width;
    const double height = (double)output_height;
    /* A quarter of a render pixel: the plane's normal measured in render pixels (output pixels scaled by output / render
       each way), so a line at any angle and any render scale grows by a quarter of a pixel of the frame the passes draw.
       Two complementary halves then overlap in a band half a pixel wide: a pixel centre within a quarter pixel of the
       line is drawn by both views (at most one pixel a row across a vertical line, none for a line on a pixel
       boundary), and none is drawn by neither, with room for the float error of the two views' clip distances
       (about 1e-4 pixel at 4K). (RFC 0014 widened by half a pixel: a line on a pixel boundary, as a vertical split
       at the frame's middle, then gave a band two pixels wide.) */
    const double normal_x = (double)clip[0] * width / (double)render_width;
    const double normal_y = (double)clip[1] * height / (double)render_height;
    const double widened = (double)clip[2] + 0.25 * sqrt(normal_x * normal_x + normal_y * normal_y);
    /* x = (x_ndc + 1) W / 2 and y = (1 - y_ndc) H / 2 (SDL's NDC: +y up, pixel rows down): times clip w. */
    clip_plane[0] = (float)((double)clip[0] * width * 0.5);
    clip_plane[1] = (float)(-(double)clip[1] * height * 0.5);
    clip_plane[2] = (float)((double)clip[0] * width * 0.5 + (double)clip[1] * height * 0.5 + widened);
    /* x = u W, y = v H. */
    uv_plane[1] = (float)((double)clip[0] * width);
    uv_plane[2] = (float)((double)clip[1] * height);
    uv_plane[3] = (float)widened;
}

/* Whether a frame of these views (valid) is drawn clipped: two views, or one whose clip isn't zero. One unclipped view
   is the present's frame. */
static bool btbh_gpu_scene_views_clipped(const BTBHGPUSceneView *views, uint32_t view_count) {
    return view_count > 1u ||
        (view_count == 1u && (views[0].clip[0] != 0.0f || views[0].clip[1] != 0.0f || views[0].clip[2] != 0.0f));
}

/* btbh_gpu_window_scene_present_views' (and its prewarm's) refusals of its views (btbh_gpu.h). visibility_count: the
   batches their visibility bytes cover (0: not read). False, with the report set (BTBH_GPU_INVALID_ARGUMENT, naming the
   view), when one is refused. */
static bool btbh_gpu_scene_views_valid(
    const BTBHGPUSceneView *views,
    uint32_t view_count,
    uint32_t visibility_count,
    BTBHGPUSceneReport *report
) {
    char message[sizeof(report->message)];
    if (views == NULL || view_count == 0u || view_count > BTBH_GPU_SCENE_MAX_VIEWS) {
        (void)snprintf(
            message,
            sizeof(message),
            "Invalid GPU scene views: views NULL, or a count of 0 or above %u",
            (unsigned)BTBH_GPU_SCENE_MAX_VIEWS
        );
        btbh_gpu_scene_error(report, BTBH_GPU_INVALID_ARGUMENT, message);
        return false;
    }
    for (uint32_t index = 0u; index < view_count; ++index) {
        const BTBHGPUSceneView *view = &views[index];
        const char *fault = NULL;
        bool finite = true;
        for (uint32_t element = 0u; element < 16u; ++element) {
            finite = finite && isfinite(view->view_projection[element]);
        }
        for (uint32_t axis = 0u; axis < 3u; ++axis) {
            finite = finite && isfinite(view->right[axis]) && isfinite(view->up[axis]) && isfinite(view->position[axis]);
        }
        float inverse[16];
        const float projection_scale_y = sqrtf(
            view->view_projection[1] * view->view_projection[1] +
            view->view_projection[5] * view->view_projection[5] +
            view->view_projection[9] * view->view_projection[9]
        );
        if (!finite) {
            fault = "its view_projection, right, up or position isn't finite";
        } else if (!btbh_gpu_scene_invert_matrix4(view->view_projection, inverse) ||
                   !isfinite(projection_scale_y) ||
                   projection_scale_y <= 0.0f) {
            fault = "its view-projection matrix is not invertible";
        } else if (!isfinite(view->clip[0]) || !isfinite(view->clip[1]) || !isfinite(view->clip[2])) {
            fault = "its clip isn't finite";
        } else if (view->clip[0] == 0.0f && view->clip[1] == 0.0f && (view_count > 1u || view->clip[2] != 0.0f)) {
            /* (clip[0] and clip[1] 0 with clip[2] not: no line, the whole frame or none of it.) */
            fault = view_count > 1u
                ? "two views need each a clip line (clip[0] or clip[1] not 0)"
                : "its clip has no line (clip[0] and clip[1] 0) but clip[2] isn't 0: all zero for the whole frame";
        } else if ((view->flags & ~(uint32_t)BTBH_GPU_SCENE_VIEW_TEMPORAL_RESET) != 0u || view->reserved != 0u) {
            fault = "an unknown flag, or reserved isn't 0";
        } else if (view->lighting != NULL && !btbh_gpu_scene_lighting_valid(view->lighting)) {
            fault = "its lighting is invalid";
        } else {
            for (uint32_t batch = 0u; batch < visibility_count && fault == NULL; ++batch) {
                if ((view->visibility != NULL && view->visibility[batch] > 1u) ||
                    (view->shadow_visibility != NULL && view->shadow_visibility[batch] > 1u)) {
                    fault = "a visibility or shadow_visibility byte is above 1";
                }
            }
        }
        if (fault != NULL) {
            (void)snprintf(message, sizeof(message), "GPU scene view %u: %s", (unsigned)index, fault);
            btbh_gpu_scene_error(report, BTBH_GPU_INVALID_ARGUMENT, message);
            return false;
        }
    }
    return true;
}

/* The second view's own sun and spot maps for a frame of these views: a view whose matrix differs from view 0's (byte
   for byte) has its own, when the frame draws that map (the call's sun or spot shadow on; never for an omni spot,
   whose map is the call's). */
static void btbh_gpu_scene_view_own_maps(
    const BTBHGPUSceneView *views,
    uint32_t view_count,
    const BTBHGPUSceneLighting *lighting,
    const uint32_t shadow_enabled[3],
    bool *own_sun,
    bool *own_spot
) {
    *own_sun = false;
    *own_spot = false;
    if (view_count < 2u) {
        return;
    }
    const BTBHGPUSceneLighting *first = views[0].lighting != NULL ? views[0].lighting : lighting;
    const BTBHGPUSceneLighting *second = views[1].lighting != NULL ? views[1].lighting : lighting;
    *own_sun =
        shadow_enabled[0] != 0u &&
        memcmp(
            first->directional_shadow_view_projection,
            second->directional_shadow_view_projection,
            sizeof(first->directional_shadow_view_projection)
        ) != 0;
    *own_spot =
        shadow_enabled[2] != 0u &&
        lighting->spot_shadow_is_omni == 0u &&
        memcmp(
            first->spot_shadow_view_projection,
            second->spot_shadow_view_projection,
            sizeof(first->spot_shadow_view_projection)
        ) != 0;
}

/* A clipped frame's vertex shaders must write the clip distance (this release's production_model.vert, its table and
   lifted siblings, production_animated_model.vert): SPIR-V that doesn't would draw every view over the whole frame. Each
   one a geometry of the frame draws with is read once (bone_offsets: btbh_gpu_scene_prepare_bones'), and
   production_post_view.vert beside the post vertex shader must exist (and, in SPIR-V, clip): read once too, in either
   format (a split frame stats no file). Metal libraries aren't otherwise read. False, with the report set
   (BTBH_GPU_INVALID_ARGUMENT, the path), when one doesn't. */
static bool btbh_gpu_scene_check_view_shaders(
    BTBHGPUSceneRuntime *runtime,
    const BTBHGPUSceneGeometryBatch *batches,
    uint32_t batch_count,
    const uint32_t *bone_offsets,
    const char *vertex_shader_path,
    const char *animated_vertex_shader_path,
    const char *post_vertex_shader_path,
    BTBHGPUShaderFormat shader_format,
    BTBHGPUSceneReport *report
) {
    char message[sizeof(report->message)];
    char post_view_path[BTBH_GPU_SCENE_PATH_CAPACITY];
    if (!btbh_gpu_scene_sibling_path(
            post_vertex_shader_path,
            shader_format == BTBH_GPU_SHADER_FORMAT_METALLIB
                ? "production_post_view.vert.metallib"
                : "production_post_view.vert.spv",
            post_view_path
        )) {
        btbh_gpu_scene_error(report, BTBH_GPU_INVALID_ARGUMENT, "Scene views' post vertex shader path is too long");
        return false;
    }
    /* (Kept by path in runtime->scene_shader_checks, as the clip distance's test: only a missing file is looked for
       again.) */
    if (btbh_gpu_scene_shader_check(runtime, post_view_path) == NULL) {
        (void)snprintf(
            message,
            sizeof(message),
            "Scene views need production_post_view.vert beside the post vertex shader (build the shaders from 0.16.0 "
            "or later): %s",
            post_view_path
        );
        btbh_gpu_scene_error(report, BTBH_GPU_INVALID_ARGUMENT, message);
        return false;
    }
    if (shader_format != BTBH_GPU_SHADER_FORMAT_SPIRV) {
        return true;
    }
    /* Bit 0 production_model.vert, 1 the table's, 2 the lifted, 3 the lifted table's, 4 the skinned, 5 the post view. */
    uint32_t wanted = 1u << 5u;
    uint32_t cursor = 0u;
    for (uint32_t index = 0u; index < batch_count; ++index) {
        const BTBHGPUSceneGeometryBatch *batch = &batches[index];
        const uint32_t geometry_count =
            batch->kind == BTBH_GPU_SCENE_GEOMETRY_MODEL ? batch->model->primitive_count : 1u;
        for (uint32_t geometry = 0u; geometry < geometry_count; ++geometry) {
            if (!btbh_gpu_scene_batch_uses_geometry(batch, geometry)) {
                continue;
            }
            if (bone_offsets[cursor++] != UINT32_MAX) {
                wanted |= 1u << 4u;
            } else {
                wanted |= 1u << ((btbh_gpu_height_lift_active(batch) ? 2u : 0u) +
                    (btbh_gpu_world_detail_table_active(batch) ? 1u : 0u));
            }
        }
    }
    static const char *const names[4] = {
        NULL,
        "production_model_detail_table.vert.spv",
        "production_model_lift.vert.spv",
        "production_model_detail_table_lift.vert.spv",
    };
    for (uint32_t which = 0u; which < 6u; ++which) {
        if ((wanted & (1u << which)) == 0u) {
            continue;
        }
        char path[BTBH_GPU_SCENE_PATH_CAPACITY];
        const char *given = which == 0u ? vertex_shader_path : (which == 4u ? animated_vertex_shader_path : NULL);
        if (which == 5u) {
            (void)memcpy(path, post_view_path, strlen(post_view_path) + 1u);
        } else if (given != NULL) {
            const size_t length = strlen(given);
            if (length + 1u > sizeof(path)) {
                continue;
            }
            (void)memcpy(path, given, length + 1u);
        } else if (!btbh_gpu_scene_sibling_path(vertex_shader_path, names[which], path)) {
            continue;
        }
        const BTBHGPUSceneShaderCheck *checked = btbh_gpu_scene_shader_check(runtime, path);
        if (checked == NULL || checked->has_clip_distance) {
            continue;
        }
        (void)snprintf(
            message,
            sizeof(message),
            "Scene views need vertex shaders that write the clip distance (build the shaders from 0.16.0 or later): %s",
            path
        );
        btbh_gpu_scene_error(report, BTBH_GPU_INVALID_ARGUMENT, message);
        return false;
    }
    return true;
}

/* The clipped screen-space passes' pipelines a clipped frame with these settings draws with, made where missing:
   production_post_view.vert with the SSAO's three (with the SSAO resolve), the HBAO's three, the distance pass's (when
   a batch needs it: a missing one only leaves distance_ready false, as the present's own) and the HDR composite's.
   False, with the report set, when one can't be made. */
static bool btbh_gpu_scene_ensure_view_pipelines(
    BTBHGPUWindow *window,
    const BTBHGPUSceneFrameSetup *setup,
    const char *post_vertex_shader_path,
    const char *post_fragment_shader_path,
    BTBHGPUShaderFormat shader_format,
    bool *distance_ready,
    BTBHGPUSceneReport *report
) {
    BTBHGPUSceneRuntime *runtime = window->scene_runtime;
    const char *extension = shader_format == BTBH_GPU_SHADER_FORMAT_METALLIB ? "metallib" : "spv";
    char vertex_path[BTBH_GPU_SCENE_PATH_CAPACITY];
    char filename[128];
    (void)snprintf(filename, sizeof(filename), "production_post_view.vert.%s", extension);
    if (!btbh_gpu_scene_sibling_path(post_vertex_shader_path, filename, vertex_path)) {
        btbh_gpu_scene_error(report, BTBH_GPU_INVALID_ARGUMENT, "Scene views' post vertex shader path is too long");
        return false;
    }
    const SDL_GPUColorTargetBlendState over = btbh_gpu_scene_hbao_over_blend();
    const struct {
        const char *name;
        SDL_GPUTextureFormat format;
        uint32_t sampler_count;
        bool over;
        bool wanted;
    } passes[BTBH_GPU_SCENE_VIEW_PIPELINE_COUNT] = {
        {"production_ssao.frag", SDL_GPU_TEXTUREFORMAT_R8_UNORM, 2u, false, setup->ssao_resolve_active},
        {"production_ssao_denoise.frag", SDL_GPU_TEXTUREFORMAT_R8_UNORM, 3u, false, setup->ssao_resolve_active},
        {"production_ssao_resolve.frag", runtime->scene_color_format, 7u, false, setup->ssao_resolve_active},
        {"production_hbao.frag", SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT, 2u, false, setup->hbao_active},
        {"production_hbao_denoise.frag", SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT, 1u, false, setup->hbao_active},
        {"production_hbao_resolve.frag", runtime->scene_color_format, 6u, true, setup->hbao_active},
        {"production_scene_distance.frag", SDL_GPU_TEXTUREFORMAT_R32_FLOAT, 1u, false,
            setup->soft_particles_active || setup->refraction_active || setup->lit_particles_active},
        {"production_hdr_composite.frag", SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, 3u, false,
            setup->hdr_composite_active},
    };
    for (uint32_t index = 0u; index < BTBH_GPU_SCENE_VIEW_PIPELINE_COUNT; ++index) {
        if (!passes[index].wanted || runtime->view_pipelines[index] != NULL) {
            continue;
        }
        char fragment_path[BTBH_GPU_SCENE_PATH_CAPACITY];
        (void)snprintf(filename, sizeof(filename), "%s.%s", passes[index].name, extension);
        BTBHGPUSceneReport pass_report;
        SDL_zero(pass_report);
        const bool made =
            btbh_gpu_scene_sibling_path(post_fragment_shader_path, filename, fragment_path) &&
            btbh_gpu_scene_create_fullscreen_pipeline_staged(
                window,
                vertex_path,
                fragment_path,
                shader_format,
                passes[index].format,
                passes[index].sampler_count,
                passes[index].over ? &over : NULL,
                1u,
                &runtime->view_pipelines[index],
                &pass_report
            );
        if (made) {
            continue;
        }
        if (index == BTBH_GPU_SCENE_VIEW_PIPELINE_DISTANCE) {
            /* (As the present's own distance pass: soft particles then draw unfaded.) */
            continue;
        }
        if (pass_report.result != BTBH_GPU_OK) {
            btbh_gpu_scene_error(report, pass_report.result, pass_report.message);
        } else {
            btbh_gpu_scene_error(report, BTBH_GPU_INVALID_ARGUMENT, "Scene views' post shader path is too long");
        }
        return false;
    }
    if (distance_ready != NULL) {
        *distance_ready = runtime->view_pipelines[BTBH_GPU_SCENE_VIEW_PIPELINE_DISTANCE] != NULL;
    }
    return true;
}

/* The second view's own maps, made where missing (own_sun, own_spot), and its particle transmittance map with its own
   sun map on a frame that draws the map: the sun map at the primary map's size and the transmittance map at its share
   of it, made anew when that changes (their retained state then starts afresh). The shadow resources exist
   (btbh_gpu_scene_prepare_frame). False, with the report set, when one can't be made. */
static bool btbh_gpu_scene_ensure_view_shadow_maps(
    BTBHGPUWindow *window,
    bool own_sun,
    bool own_spot,
    bool particle_shadow_active,
    uint8_t particle_shadow,
    BTBHGPUSceneReport *report
) {
    BTBHGPUSceneRuntime *runtime = window->scene_runtime;
    SDL_GPUDevice *device = window->device->gpu;
    if (runtime->view_shadow_textures[0] != NULL &&
        (runtime->view_shadow_width != runtime->directional_shadow_width ||
         runtime->view_shadow_height != runtime->directional_shadow_height)) {
        SDL_ReleaseGPUTexture(device, runtime->view_shadow_textures[0]);
        runtime->view_shadow_textures[0] = NULL;
        runtime->view_retained_shadow_ready[0] = 0u;
    }
    SDL_GPUTextureCreateInfo info;
    SDL_zero(info);
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.layer_count_or_depth = 1u;
    info.num_levels = 1u;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    if (own_sun && runtime->view_shadow_textures[0] == NULL) {
        info.width = runtime->directional_shadow_width;
        info.height = runtime->directional_shadow_height;
        runtime->view_shadow_textures[0] = SDL_CreateGPUTexture(device, &info);
        if (runtime->view_shadow_textures[0] == NULL) {
            btbh_gpu_scene_error(report, BTBH_GPU_DEVICE_FAILURE, SDL_GetError());
            return false;
        }
        runtime->view_shadow_width = info.width;
        runtime->view_shadow_height = info.height;
        runtime->view_retained_shadow_ready[0] = 0u;
    }
    if (own_spot && runtime->view_shadow_textures[1] == NULL) {
        info.width = BTBH_GPU_SPOT_SHADOW_MAP_SIZE;
        info.height = BTBH_GPU_SPOT_SHADOW_MAP_SIZE;
        runtime->view_shadow_textures[1] = SDL_CreateGPUTexture(device, &info);
        if (runtime->view_shadow_textures[1] == NULL) {
            btbh_gpu_scene_error(report, BTBH_GPU_DEVICE_FAILURE, SDL_GetError());
            return false;
        }
        runtime->view_retained_shadow_ready[1] = 0u;
    }
    if (!own_sun || !particle_shadow_active) {
        return true;
    }
    const uint32_t width = btbh_gpu_particle_shadow_size(runtime->directional_shadow_width, particle_shadow);
    const uint32_t height = btbh_gpu_particle_shadow_size(runtime->directional_shadow_height, particle_shadow);
    if (runtime->view_particle_transmittance_texture != NULL &&
        (runtime->view_particle_transmittance_width != width ||
         runtime->view_particle_transmittance_height != height)) {
        SDL_ReleaseGPUTexture(device, runtime->view_particle_transmittance_texture);
        runtime->view_particle_transmittance_texture = NULL;
    }
    if (runtime->view_particle_transmittance_texture == NULL) {
        SDL_GPUTextureCreateInfo map_info;
        SDL_zero(map_info);
        map_info.type = SDL_GPU_TEXTURETYPE_2D;
        map_info.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
        map_info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        map_info.width = width;
        map_info.height = height;
        map_info.layer_count_or_depth = 1u;
        map_info.num_levels = 1u;
        map_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        runtime->view_particle_transmittance_texture = SDL_CreateGPUTexture(device, &map_info);
        if (runtime->view_particle_transmittance_texture == NULL) {
            btbh_gpu_scene_error(report, BTBH_GPU_DEVICE_FAILURE, SDL_GetError());
            return false;
        }
        runtime->view_particle_transmittance_width = width;
        runtime->view_particle_transmittance_height = height;
    }
    return true;
}

/* Room for the second view's opaque order and the views' combined visibility (batch_count each). */
static bool btbh_gpu_scene_ensure_view_order(BTBHGPUSceneRuntime *runtime, uint32_t batch_count) {
    if (batch_count <= runtime->view_order_capacity) {
        return true;
    }
    uint32_t *order = (uint32_t *)realloc(runtime->view_opaque_order, sizeof(uint32_t) * batch_count);
    if (order == NULL) {
        return false;
    }
    runtime->view_opaque_order = order;
    uint8_t *visibility = (uint8_t *)realloc(runtime->view_visibility, 2u * (size_t)batch_count);
    if (visibility == NULL) {
        return false;
    }
    runtime->view_visibility = visibility;
    runtime->view_order_capacity = batch_count;
    return true;
}

/* btbh_gpu_scene_render_fullscreen for a clipped frame: one pass into `target` (cleared first, or loaded for a blended
   pipeline: over), where each view draws its full-screen triangle clipped to its half (production_post_view.vert's block,
   its clip_plane) with its own fragment block (blocks + view * block_stride, block_size bytes). */
static bool btbh_gpu_scene_render_fullscreen_views(
    SDL_GPUCommandBuffer *command,
    const char *debug_label,
    SDL_GPUGraphicsPipeline *pipeline,
    SDL_GPUTexture *target,
    bool over,
    const void *blocks,
    uint32_t block_size,
    size_t block_stride,
    SDL_GPUTextureSamplerBinding *samplers,
    uint32_t sampler_count,
    const BTBHGPUSceneViewFrame *views,
    uint32_t view_count
) {
    SDL_InsertGPUDebugLabel(command, debug_label);
    SDL_GPUColorTargetInfo color_target;
    SDL_zero(color_target);
    color_target.texture = target;
    color_target.clear_color.a = 1.0f;
    color_target.load_op = over ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
    color_target.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, &color_target, 1u, NULL);
    if (pass == NULL) {
        return false;
    }
    SDL_BindGPUGraphicsPipeline(pass, pipeline);
    if (samplers != NULL && sampler_count > 0u) {
        SDL_BindGPUFragmentSamplers(pass, 0u, samplers, sampler_count);
    }
    for (uint32_t index = 0u; index < view_count; ++index) {
        SDL_PushGPUVertexUniformData(command, 0u, views[index].clip_plane, (uint32_t)sizeof(views[index].clip_plane));
        SDL_PushGPUFragmentUniformData(
            command,
            0u,
            (const uint8_t *)blocks + block_stride * index,
            block_size
        );
        SDL_DrawGPUPrimitives(pass, 3u, 1u, 0u, 0u);
    }
    SDL_EndGPURenderPass(pass);
    return true;
}

/* btbh_gpu_scene_encode_hbao for a clipped frame: each pass draws every view clipped to its half, with its own
   matrices and its half's plane (BTBHGPUSceneHBAOUniforms.view yzw), so that no tap crosses the line. */
static bool btbh_gpu_scene_encode_hbao_views(
    SDL_GPUCommandBuffer *command,
    BTBHGPUSceneRuntime *runtime,
    const BTBHGPUScenePost *post,
    const BTBHGPUSceneViewFrame *views,
    uint32_t view_count,
    uint32_t mode,
    SDL_GPUTexture *kept_emission,
    SDL_GPUTexture *target
) {
    BTBHGPUSceneHBAOUniforms blocks[BTBH_GPU_SCENE_MAX_VIEWS];
    SDL_zeroa(blocks);
    for (uint32_t index = 0u; index < view_count; ++index) {
        BTBHGPUSceneHBAOUniforms *block = &blocks[index];
        (void)memcpy(
            block->inverse_view_projection,
            views[index].inverse_view_projection,
            sizeof(block->inverse_view_projection)
        );
        block->controls[0] = post->ssao_radius;
        block->controls[1] = post->ssao_intensity;
        block->controls[2] = post->ssao_power;
        block->controls[3] = post->ssao_bias;
        block->options[0] = mode;
        block->options[1] = post->ssao_debug_view != 0u ? 1u : 0u;
        block->view[0] = views[index].projection_scale_y;
        block->view[1] = views[index].uv_plane[1];
        block->view[2] = views[index].uv_plane[2];
        block->view[3] = views[index].uv_plane[3];
    }
    SDL_GPUTextureSamplerBinding hbao_samplers[2];
    SDL_zeroa(hbao_samplers);
    hbao_samplers[0].texture = runtime->depth_texture;
    hbao_samplers[1].texture = runtime->normal_texture;
    SDL_GPUTextureSamplerBinding denoise_sampler;
    SDL_zero(denoise_sampler);
    denoise_sampler.texture = runtime->hbao_texture;
    SDL_GPUTextureSamplerBinding resolve_samplers[6];
    SDL_zeroa(resolve_samplers);
    resolve_samplers[0].texture = runtime->depth_texture;
    resolve_samplers[1].texture = runtime->ambient_texture;
    resolve_samplers[2].texture = runtime->normal_texture;
    resolve_samplers[3].texture = runtime->ambient_specular_texture;
    resolve_samplers[4].texture = runtime->hbao_denoised_texture;
    resolve_samplers[5].texture = kept_emission;
    hbao_samplers[0].sampler = runtime->scene_color_sampler;
    hbao_samplers[1].sampler = runtime->scene_color_sampler;
    denoise_sampler.sampler = runtime->scene_color_sampler;
    for (uint32_t index = 0u; index < 6u; ++index) {
        resolve_samplers[index].sampler = runtime->scene_color_sampler;
    }
    return btbh_gpu_scene_render_fullscreen_views(
            command,
            "BTBH HBAO Views",
            runtime->view_pipelines[BTBH_GPU_SCENE_VIEW_PIPELINE_HBAO],
            runtime->hbao_texture,
            false,
            blocks,
            (uint32_t)sizeof(blocks[0]),
            sizeof(blocks[0]),
            hbao_samplers,
            2u,
            views,
            view_count
        ) &&
        btbh_gpu_scene_render_fullscreen_views(
            command,
            "BTBH HBAO Denoise Views",
            runtime->view_pipelines[BTBH_GPU_SCENE_VIEW_PIPELINE_HBAO_DENOISE],
            runtime->hbao_denoised_texture,
            false,
            blocks,
            (uint32_t)sizeof(blocks[0]),
            sizeof(blocks[0]),
            &denoise_sampler,
            1u,
            views,
            view_count
        ) &&
        btbh_gpu_scene_render_fullscreen_views(
            command,
            "BTBH HBAO Resolve Views",
            runtime->view_pipelines[BTBH_GPU_SCENE_VIEW_PIPELINE_HBAO_RESOLVE],
            target,
            true,
            blocks,
            (uint32_t)sizeof(blocks[0]),
            sizeof(blocks[0]),
            resolve_samplers,
            6u,
            views,
            view_count
        );
}

/* The frame's part of the geometry passes' two uniform blocks for one view (RFC 0011 phase 1c: filled once a pass, each
   batch writes its own fields over it; RFC 0014: once a view): its camera and its half (surface_controls.yzw), the
   call's light and shadows' settings, the view's spot light, lamps and point lights (view_lighting), and the shadow
   matrices it reads (sun_matrix, spot_matrix: its maps' as the receivers read them). */
static void btbh_gpu_scene_frame_uniforms(
    const BTBHGPUSceneRuntime *runtime,
    const BTBHGPUSceneLighting *lighting,
    const BTBHGPUSceneLighting *view_lighting,
    const BTBHGPUScenePost *post,
    const BTBHGPUSceneFrameSetup *setup,
    const float view_projection[16],
    const float camera_right[3],
    const float camera_up[3],
    const float camera_position[3],
    const float previous_view_projection[16],
    const float previous_world_curvature[4],
    const float temporal_controls[4],
    float temporal_lod_bias,
    const float clip_plane[4],
    const float *sun_matrix,
    const float *spot_matrix,
    BTBHGPUSceneUniforms *uniforms,
    BTBHGPUSceneFragmentUniforms *fragment_uniforms
) {
    const uint32_t *shadow_enabled = setup->shadow_enabled;
    (void)memset(uniforms, 0, sizeof(*uniforms));
    (void)memcpy(
        uniforms->view_projection,
        view_projection,
        sizeof(uniforms->view_projection)
    );
    (void)memcpy(
        uniforms->camera_right,
        camera_right,
        sizeof(float) * 3u
    );
    (void)memcpy(
        uniforms->camera_up,
        camera_up,
        sizeof(float) * 3u
    );
    (void)memcpy(
        uniforms->camera_position,
        camera_position,
        sizeof(float) * 3u
    );
    uniforms->ambient_color_energy[0] = lighting->ambient_red;
    uniforms->ambient_color_energy[1] = lighting->ambient_green;
    uniforms->ambient_color_energy[2] = lighting->ambient_blue;
    uniforms->ambient_color_energy[3] = lighting->ambient_energy;
    uniforms->directional_direction_energy[0] = lighting->directional_x;
    uniforms->directional_direction_energy[1] = lighting->directional_y;
    uniforms->directional_direction_energy[2] = lighting->directional_z;
    uniforms->directional_direction_energy[3] = lighting->directional_energy;
    uniforms->directional_color[0] = lighting->directional_red;
    uniforms->directional_color[1] = lighting->directional_green;
    uniforms->directional_color[2] = lighting->directional_blue;
    uniforms->directional_color[3] = 1.0f;
    uniforms->secondary_directional_direction_energy[0] = lighting->secondary_directional_x;
    uniforms->secondary_directional_direction_energy[1] = lighting->secondary_directional_y;
    uniforms->secondary_directional_direction_energy[2] = lighting->secondary_directional_z;
    uniforms->secondary_directional_direction_energy[3] = lighting->secondary_directional_energy;
    uniforms->secondary_directional_color[0] = lighting->secondary_directional_red;
    uniforms->secondary_directional_color[1] = lighting->secondary_directional_green;
    uniforms->secondary_directional_color[2] = lighting->secondary_directional_blue;
    uniforms->secondary_directional_color[3] = 1.0f;
    /* RFC 0014: the view's half (all zero for the whole frame: the vertex shaders then clip nothing). */
    uniforms->surface_controls[1] = clip_plane[0];
    uniforms->surface_controls[2] = clip_plane[1];
    uniforms->surface_controls[3] = clip_plane[2];
    (void)memcpy(
        uniforms->previous_view_projection,
        previous_view_projection,
        sizeof(uniforms->previous_view_projection)
    );
    (void)memcpy(
        uniforms->world_curvature,
        post->world_curvature,
        sizeof(uniforms->world_curvature)
    );
    (void)memcpy(
        uniforms->previous_world_curvature,
        previous_world_curvature,
        sizeof(uniforms->previous_world_curvature)
    );
    (void)memcpy(
        uniforms->temporal_controls,
        temporal_controls,
        sizeof(uniforms->temporal_controls)
    );
    (void)memset(fragment_uniforms, 0, sizeof(*fragment_uniforms));
    (void)memcpy(
        fragment_uniforms->camera_position,
        camera_position,
        sizeof(float) * 3u
    );
    (void)memcpy(
        fragment_uniforms->ambient_color_energy,
        uniforms->ambient_color_energy,
        sizeof(fragment_uniforms->ambient_color_energy)
    );
    (void)memcpy(
        fragment_uniforms->directional_direction_energy,
        uniforms->directional_direction_energy,
        sizeof(fragment_uniforms->directional_direction_energy)
    );
    (void)memcpy(
        fragment_uniforms->directional_color,
        uniforms->directional_color,
        sizeof(fragment_uniforms->directional_color)
    );
    (void)memcpy(
        fragment_uniforms->secondary_directional_direction_energy,
        uniforms->secondary_directional_direction_energy,
        sizeof(fragment_uniforms->secondary_directional_direction_energy)
    );
    (void)memcpy(
        fragment_uniforms->secondary_directional_color,
        uniforms->secondary_directional_color,
        sizeof(fragment_uniforms->secondary_directional_color)
    );
    fragment_uniforms->spot_position_range[0] = view_lighting->spot_position_x;
    fragment_uniforms->spot_position_range[1] = view_lighting->spot_position_y;
    fragment_uniforms->spot_position_range[2] = view_lighting->spot_position_z;
    fragment_uniforms->spot_position_range[3] = view_lighting->spot_range;
    fragment_uniforms->spot_direction_energy[0] = view_lighting->spot_direction_x;
    fragment_uniforms->spot_direction_energy[1] = view_lighting->spot_direction_y;
    fragment_uniforms->spot_direction_energy[2] = view_lighting->spot_direction_z;
    fragment_uniforms->spot_direction_energy[3] = view_lighting->spot_energy;
    fragment_uniforms->spot_color_falloff[0] = view_lighting->spot_red;
    fragment_uniforms->spot_color_falloff[1] = view_lighting->spot_green;
    fragment_uniforms->spot_color_falloff[2] = view_lighting->spot_blue;
    fragment_uniforms->spot_color_falloff[3] = view_lighting->spot_falloff;
    fragment_uniforms->spot_cutoffs[0] = view_lighting->spot_inner_cutoff;
    fragment_uniforms->spot_cutoffs[1] = view_lighting->spot_outer_cutoff;
    fragment_uniforms->spot_cutoffs[2] = view_lighting->spot_occluder_shadow_strength;
    fragment_uniforms->spot_cutoffs[3] = view_lighting->spot_occluder_shadow_reach;
    {
        const uint32_t point_count = SDL_min(
            view_lighting->point_light_count,
            (uint32_t)BTBH_GPU_SCENE_POINT_LIGHTS
        );
        fragment_uniforms->point_light_count[0] = (float)point_count;
        for (uint32_t light = 0u; light < point_count; ++light) {
            (void)memcpy(
                fragment_uniforms->point_lights[light * 2u],
                &view_lighting->point_lights[light][0],
                4u * sizeof(float)
            );
            (void)memcpy(
                fragment_uniforms->point_lights[light * 2u + 1u],
                &view_lighting->point_lights[light][4],
                4u * sizeof(float)
            );
        }
    }
    for (size_t coefficient = 0u; coefficient < 9u; coefficient += 1u) {
        for (size_t channel = 0u; channel < 3u; channel += 1u) {
            fragment_uniforms->irradiance_sh[coefficient][channel] =
                lighting->irradiance_sh[coefficient * 3u + channel];
        }
    }
    fragment_uniforms->ground_bottom_energy[0] = lighting->ground_bottom_red;
    fragment_uniforms->ground_bottom_energy[1] = lighting->ground_bottom_green;
    fragment_uniforms->ground_bottom_energy[2] = lighting->ground_bottom_blue;
    fragment_uniforms->ground_bottom_energy[3] = lighting->ground_energy;
    fragment_uniforms->ground_horizon_curve[0] = lighting->ground_horizon_red;
    fragment_uniforms->ground_horizon_curve[1] = lighting->ground_horizon_green;
    fragment_uniforms->ground_horizon_curve[2] = lighting->ground_horizon_blue;
    fragment_uniforms->ground_horizon_curve[3] = lighting->ground_horizon_curve;
    fragment_uniforms->sky_top_energy[0] = lighting->sky_top_red;
    fragment_uniforms->sky_top_energy[1] = lighting->sky_top_green;
    fragment_uniforms->sky_top_energy[2] = lighting->sky_top_blue;
    fragment_uniforms->sky_top_energy[3] = lighting->sky_energy;
    fragment_uniforms->sky_horizon_curve[0] = lighting->sky_horizon_red;
    fragment_uniforms->sky_horizon_curve[1] = lighting->sky_horizon_green;
    fragment_uniforms->sky_horizon_curve[2] = lighting->sky_horizon_blue;
    fragment_uniforms->sky_horizon_curve[3] = lighting->sky_horizon_curve;
    fragment_uniforms->sun_color_energy[0] = lighting->sun_red;
    fragment_uniforms->sun_color_energy[1] = lighting->sun_green;
    fragment_uniforms->sun_color_energy[2] = lighting->sun_blue;
    fragment_uniforms->sun_color_energy[3] = lighting->sun_energy;
    fragment_uniforms->sun_direction_size[0] = lighting->sun_direction_x;
    fragment_uniforms->sun_direction_size[1] = lighting->sun_direction_y;
    fragment_uniforms->sun_direction_size[2] = lighting->sun_direction_z;
    fragment_uniforms->sun_direction_size[3] = lighting->sun_size_radians;
    fragment_uniforms->procedural_controls[0] = lighting->sun_curve;
    fragment_uniforms->procedural_controls[1] = (float)lighting->ibl_enabled;
    fragment_uniforms->procedural_controls[3] = temporal_lod_bias;
    fragment_uniforms->surface_wetness[0] =
        isfinite(lighting->surface_wetness)
            ? fminf(fmaxf(lighting->surface_wetness, 0.0f), 1.0f)
            : 0.0f;
    fragment_uniforms->surface_wetness[1] =
        isfinite(lighting->window_glow)
            ? fmaxf(lighting->window_glow, 0.0f)
            : 0.0f;
    fragment_uniforms->surface_wetness[2] =
        isfinite(lighting->rain)
            ? fminf(fmaxf(lighting->rain, 0.0f), 1.0f)
            : 0.0f;
    fragment_uniforms->cloud_controls[0] = lighting->cloud_offset[0];
    fragment_uniforms->cloud_controls[1] = lighting->cloud_offset[1];
    fragment_uniforms->cloud_controls[2] = lighting->cloud_frequency;
    fragment_uniforms->cloud_controls[3] = lighting->cloud_strength;
    fragment_uniforms->cloud_shape[0] = lighting->cloud_coverage;
    fragment_uniforms->cloud_shape[1] = lighting->cloud_softness;
    fragment_uniforms->cloud_shape[2] = lighting->cloud_height;
    {
        const uint32_t lamp_count =
            view_lighting->lamp_count < BTBH_GPU_SCENE_MAX_LAMPS
                ? view_lighting->lamp_count
                : BTBH_GPU_SCENE_MAX_LAMPS;
        fragment_uniforms->lamp_controls[0] = (float)lamp_count;
        fragment_uniforms->lamp_controls[1] = view_lighting->lamp_core_radius;
        if (lamp_count > 0u) {
            (void)memcpy(
                fragment_uniforms->lamp_position_reach,
                view_lighting->lamp_position_reach,
                sizeof(float) * 4u * lamp_count
            );
            (void)memcpy(
                fragment_uniforms->lamp_color_energy,
                view_lighting->lamp_color_energy,
                sizeof(float) * 4u * lamp_count
            );
        }
    }
    /* The dark scene's controls ride in the point lights' header. */
    fragment_uniforms->point_light_count[1] = lighting->unlit_darkness;
    fragment_uniforms->point_light_count[2] = lighting->unlit_glow_threshold;
    fragment_uniforms->point_light_count[3] = SDL_max(lighting->point_light_ceiling, 0.0f);
    const float *secondary_shadow_matrix =
        runtime->retained_shadow_ready[1] != 0u
            ? runtime->retained_shadow_view_projection[1]
            : lighting->secondary_directional_shadow_view_projection;
    (void)memcpy(
        fragment_uniforms->directional_shadow_view_projection,
        sun_matrix,
        sizeof(fragment_uniforms->directional_shadow_view_projection)
    );
    (void)memcpy(
        fragment_uniforms->secondary_directional_shadow_view_projection,
        secondary_shadow_matrix,
        sizeof(fragment_uniforms->secondary_directional_shadow_view_projection)
    );
    (void)memcpy(
        fragment_uniforms->spot_shadow_view_projection,
        spot_matrix,
        sizeof(fragment_uniforms->spot_shadow_view_projection)
    );
    fragment_uniforms->directional_shadow_controls[0] = (float)shadow_enabled[0];
    fragment_uniforms->directional_shadow_controls[1] = lighting->directional_shadow_softness;
    fragment_uniforms->directional_shadow_controls[2] =
        lighting->directional_shadow_filter == BTBH_GPU_SCENE_SHADOW_FILTER_SOFT ? 1.0f : 0.0f;
    /* RFC 0013 phase 2: the surfaces the sun shadows read the particle transmittance map (particle_shadow 2). */
    fragment_uniforms->directional_shadow_controls[3] =
        setup->particle_shadow_active &&
                (setup->particle_shadow & BTBH_GPU_SCENE_PARTICLE_SHADOW_READERS) ==
                    BTBH_GPU_SCENE_PARTICLE_SHADOW_GROUND
            ? 1.0f
            : 0.0f;
    fragment_uniforms->secondary_directional_shadow_controls[0] = (float)shadow_enabled[1];
    fragment_uniforms->secondary_directional_shadow_controls[1] = lighting->secondary_directional_shadow_softness;
    /* BTBHGPUSceneLighting.spot_occluder_flags (since 0.10.0), as a whole number. */
    fragment_uniforms->secondary_directional_shadow_controls[2] = (float)view_lighting->spot_occluder_flags;
    fragment_uniforms->spot_shadow_controls[0] = (float)shadow_enabled[2];
    fragment_uniforms->spot_shadow_controls[1] = view_lighting->spot_shadow_softness;
    fragment_uniforms->spot_shadow_controls[2] = (float)runtime->retained_spot_shadow_is_omni;
    fragment_uniforms->spot_shadow_controls[3] = view_lighting->spot_shadow_near;
    if (runtime->retained_shadow_ready[2] != 0u &&
        runtime->retained_spot_shadow_is_omni != 0u) {
        (void)memcpy(
            fragment_uniforms->omni_shadow_position_range,
            runtime->retained_omni_shadow_position_range,
            sizeof(fragment_uniforms->omni_shadow_position_range)
        );
    } else {
        fragment_uniforms->omni_shadow_position_range[0] = lighting->spot_position_x;
        fragment_uniforms->omni_shadow_position_range[1] = lighting->spot_position_y;
        fragment_uniforms->omni_shadow_position_range[2] = lighting->spot_position_z;
        fragment_uniforms->omni_shadow_position_range[3] = lighting->spot_range;
    }
}

/* The first part of a scene frame, shared by the present and btbh_gpu_window_scene_prewarm so that the two make
   the same things: the window's scene runtime and BRDF lookup, the radiance cascades' and FSR 3's pipelines and
   resources, the shadow maps and their pipelines, SMAA's or FXAA's, and what the frame turns on (`setup`). It
   draws nothing and leaves a frame's state alone (FSR 3's jitter and history, the retained shadow maps).
   output_width: 0 for the window's frame, else a render target's width (never temporal). views_clipped (RFC 0014): a
   frame of two scene views, or one clipped, which runs neither the radiance cascades nor FSR 3. False, with the report
   set, when it failed (a failed shadow setup releases the window's scene). */
static bool btbh_gpu_scene_prepare_frame(
    BTBHGPUWindow *window,
    const char *fast_shadow_vertex_shader_path,
    const char *fast_shadow_fragment_shader_path,
    const char *shadow_vertex_shader_path,
    const char *animated_shadow_vertex_shader_path,
    const char *shadow_fragment_shader_path,
    const char *fragment_shader_path,
    const char *post_vertex_shader_path,
    const char *post_fragment_shader_path,
    BTBHGPUShaderFormat shader_format,
    uint32_t render_width,
    uint32_t render_height,
    uint32_t output_width,
    bool views_clipped,
    const BTBHGPUSceneLighting *lighting,
    const BTBHGPUScenePost *post,
    const BTBHGPUSceneGeometryBatch *batches,
    const uint8_t *scene_visibility,
    uint32_t batch_count,
    BTBHGPUSceneFrameSetup *setup,
    BTBHGPUSceneReport *report
) {
    if (!btbh_gpu_scene_ensure_runtime(
            window,
            report
        )) {
        return false;
    }
    if (!btbh_gpu_scene_ensure_brdf_lookup(
            window,
            fragment_shader_path,
            report
        )) {
        return false;
    }
    BTBHGPUSceneRuntime *runtime = window->scene_runtime;
    char radiance_cascades_message[256];
    radiance_cascades_message[0] = '\0';
    /* RFC 0014: a clipped frame of views runs no cascades. Their state and resources stay as they were; their history
       starts afresh on the next frame that runs them (dropped by the present once such a frame is drawn: not here, which
       a prewarm and a refused present pass through too). */
    const bool radiance_cascades_held =
        views_clipped && post->radiance_cascades.tier != BTBH_GPU_SCENE_RC_OFF;
    const bool radiance_cascades_ready =
        !radiance_cascades_held &&
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
        !radiance_cascades_held &&
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
        !radiance_cascades_held &&
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
        output_width == 0u &&
        !views_clipped &&
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
    const uint8_t upscale_mode =
        post->upscale_mode == BTBH_GPU_SCENE_UPSCALE_FSR3 && !temporal_active
            ? (uint8_t)BTBH_GPU_SCENE_UPSCALE_FSR1
            : post->upscale_mode;
    const uint8_t anti_aliasing = temporal_active ? 0u : post->anti_aliasing;
    const bool ambient_occlusion_active =
        post->ssao_enabled == 1u &&
        !radiance_cascades_debug_output;
    /* ssao_enabled 2: the horizon-based occlusion (its own passes, laid over the opaque scene). */
    const bool hbao_active =
        post->ssao_enabled == 2u &&
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
    const bool lit_particles_active =
        batch_count > 0u &&
        batches != NULL &&
        btbh_gpu_scene_batches_have_lit_particles(batches, batch_count);
    const bool refraction_active =
        batch_count > 0u &&
        batches != NULL &&
        btbh_gpu_scene_batches_have_refraction(
            batches,
            batch_count,
            scene_visibility
        );
    const bool auxiliary_geometry_outputs =
        ssao_resolve_active || hbao_active || radiance_cascades_active ||
        temporal_active;
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
        return false;
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
        return false;
    }
    if (anti_aliasing == BTBH_GPU_SCENE_ANTI_ALIASING_FXAA &&
        !btbh_gpu_scene_ensure_fxaa_pipeline(
            window,
            post_vertex_shader_path,
            post_fragment_shader_path,
            shader_format,
            report
        )) {
        return false;
    }

    SDL_zerop(setup);
    setup->radiance_cascades_active = radiance_cascades_active;
    setup->radiance_cascades_fallback_debug = radiance_cascades_fallback_debug;
    setup->temporal_active = temporal_active;
    setup->temporal_window_width = temporal_window_width;
    setup->upscale_mode = upscale_mode;
    setup->anti_aliasing = anti_aliasing;
    setup->ambient_occlusion_active = ambient_occlusion_active;
    setup->hbao_active = hbao_active;
    setup->ssr_active = ssr_active;
    setup->ssao_resolve_active = ssao_resolve_active;
    setup->bloom_active = bloom_active;
    setup->hdr_composite_active = hdr_composite_active;
    setup->soft_particles_active = soft_particles_active;
    setup->lit_particles_active = lit_particles_active;
    setup->refraction_active = refraction_active;
    setup->auxiliary_geometry_outputs = auxiliary_geometry_outputs;
    (void)memcpy(setup->shadow_enabled, shadow_enabled, sizeof(setup->shadow_enabled));
    setup->omni_shadows_active = omni_shadows_active;
    setup->shadows_active = shadows_active;
    /* RFC 0013 phase 2: the map needs the sun's map (its matrix and size) and a batch to draw. */
    setup->particle_shadow = lighting->particle_shadow;
    setup->particle_shadow_active =
        (lighting->particle_shadow & BTBH_GPU_SCENE_PARTICLE_SHADOW_READERS) != 0u &&
        shadow_enabled[0] != 0u &&
        batch_count > 0u &&
        batches != NULL &&
        btbh_gpu_scene_batches_have_particle_shadow(batches, batch_count);
    /* Since 0.12.0 with the HBAO in either mode and with the SSAO (0.9.0 to 0.11.0: the HBAO, ssao_contact 0). With no
       ambient occlusion nothing darkens the scene, which the cascades then only add their bounce light to: the flag's
       transparent part alone (cascades_fast_unlit). */
    setup->scene_ambient_targets =
        (hbao_active || ambient_occlusion_active) &&
        radiance_cascades_active &&
        post->radiance_cascades.additive != 0u &&
        (post->flags & BTBH_GPU_SCENE_POST_ADDITIVE_CASCADES_KEEP_HBAO) != 0u;
    /* (ssao_contact's mode keeps the emission target's rgb, which the cascades' resolve reads the albedo from here.) */
    setup->scene_contact_targets =
        setup->scene_ambient_targets &&
        hbao_active &&
        post->ssao_contact != 0u;
    setup->cascades_fast_unlit =
        radiance_cascades_active &&
        post->radiance_cascades.additive != 0u &&
        (post->flags & BTBH_GPU_SCENE_POST_ADDITIVE_CASCADES_KEEP_HBAO) != 0u;
    setup->ssr_surface_reflectivity =
        ssr_active &&
        !radiance_cascades_active &&
        (post->flags & BTBH_GPU_SCENE_POST_SSR_SURFACE_REFLECTIVITY) != 0u;
    setup->radiance_cascades_held_for_views = radiance_cascades_held;
    return true;
}

/* The model pipelines a frame's batches need (each geometry's, and a transparent one's without the auxiliary
   outputs too), and (RFC 0013 phase 2) the particle transmittance map's, made where missing: shared by the present and
   btbh_gpu_window_scene_prewarm. bone_offsets is
   btbh_gpu_scene_prepare_bones' (UINT32_MAX for a geometry drawn without bones). opaque_geometry_base, when not
   NULL, gets where each batch's geometries start among them (for the opaque order). */
static bool btbh_gpu_scene_ensure_model_pipelines(
    BTBHGPUWindow *window,
    const char *vertex_shader_path,
    const char *animated_vertex_shader_path,
    const char *fragment_shader_path,
    const char *unshadowed_fragment_shader_path,
    BTBHGPUShaderFormat shader_format,
    const BTBHGPUSceneFrameSetup *setup,
    const BTBHGPUSceneGeometryBatch *batches,
    uint32_t batch_count,
    const uint32_t *bone_offsets,
    uint32_t prepared_geometry_count,
    uint32_t *opaque_geometry_base,
    BTBHGPUSceneReport *report
) {
    uint32_t pipeline_geometry_cursor = 0u;
    for (uint32_t batch_index = 0u;
         batch_index < batch_count;
         ++batch_index) {
        const BTBHGPUSceneGeometryBatch *batch = &batches[batch_index];
        const uint32_t geometry_count =
            batch->kind == BTBH_GPU_SCENE_GEOMETRY_MODEL
                ? batch->model->primitive_count
                : 1u;
        if (opaque_geometry_base != NULL) {
            opaque_geometry_base[batch_index] =
                pipeline_geometry_cursor;
        }
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
                    setup->auxiliary_geometry_outputs,
                    setup->temporal_active,
                    setup->radiance_cascades_active,
                    setup->cascades_fast_unlit,
                    setup->shadows_active,
                    setup->omni_shadows_active,
                    animated,
                    batch,
                    report
                )) {
                return false;
            }
            if (
                setup->auxiliary_geometry_outputs &&
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
                    setup->temporal_active,
                    setup->radiance_cascades_active,
                    setup->cascades_fast_unlit,
                    setup->shadows_active,
                    setup->omni_shadows_active,
                    animated,
                    batch,
                    report
                )
            ) {
                return false;
            }
        }
    }
    if (pipeline_geometry_cursor != prepared_geometry_count) {
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            "GPU scene pipeline preparation changed during submission"
        );
        return false;
    }
    /* RFC 0013 phase 2: the particle transmittance map and its pipeline, on a frame that draws it. */
    return btbh_gpu_scene_ensure_particle_shadow(
        window,
        vertex_shader_path,
        shader_format,
        setup->particle_shadow_active,
        setup->particle_shadow,
        report
    );
}

/* The frame's targets at its render (and drawable) size and the post chain's pipelines, made where missing: shared
   by the present and btbh_gpu_window_scene_prewarm. The drawable size is output_width x output_height, or the
   window's in pixels when output_width is 0. scene_distance_ready says whether the soft particles' and the
   refraction's distance pass can run (a missing pass never fails the frame). False when a target or a pipeline is
   missing, with the report set where it said why. */
static bool btbh_gpu_scene_ensure_frame_targets(
    BTBHGPUWindow *window,
    const BTBHGPUSceneFrameSetup *setup,
    const BTBHGPUScenePost *post,
    const char *post_vertex_shader_path,
    const char *post_fragment_shader_path,
    BTBHGPUShaderFormat shader_format,
    uint32_t render_width,
    uint32_t render_height,
    uint32_t output_width,
    uint32_t output_height,
    uint32_t *drawable_width_out,
    uint32_t *drawable_height_out,
    bool *scene_distance_ready,
    BTBHGPUSceneReport *report
) {
    int drawable_width_value = (int)output_width;
    int drawable_height_value = (int)output_height;
    const bool drawable_size_available =
        (output_width != 0u ||
         SDL_GetWindowSizeInPixels(
             window->window,
             &drawable_width_value,
             &drawable_height_value
         )) &&
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
    *drawable_width_out = drawable_width;
    *drawable_height_out = drawable_height;
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
            setup->anti_aliasing != 0u ||
                post->mirror_horizontal != 0u,
            setup->anti_aliasing >= 1u &&
                setup->anti_aliasing <=
                    BTBH_GPU_SCENE_SMAA_QUALITY_COUNT,
            report
        ) &&
        (!setup->temporal_active ||
         btbh_gpu_scene_ensure_temporal_targets(
             window,
             render_width,
             render_height,
             report
         )) &&
        (setup->upscale_mode == BTBH_GPU_SCENE_UPSCALE_LINEAR ||
         (!setup->temporal_active &&
          render_width == drawable_width &&
          render_height == drawable_height) ||
         (btbh_gpu_scene_ensure_upscale_pipelines(
              window,
              setup->temporal_active
                  ? (uint8_t)BTBH_GPU_SCENE_UPSCALE_FSR1
                  : setup->upscale_mode,
              post_vertex_shader_path,
              post_fragment_shader_path,
              shader_format,
              report
          ) &&
          btbh_gpu_scene_ensure_upscaled_color_target(
              window,
              drawable_width,
              drawable_height,
              setup->upscale_mode == BTBH_GPU_SCENE_UPSCALE_FSR1 ||
                  setup->temporal_active,
              report
          ))) &&
        (!setup->ssao_resolve_active ||
         btbh_gpu_scene_ensure_ssao_resolve_pipeline(
             window,
             post_vertex_shader_path,
             post_fragment_shader_path,
             shader_format,
              report
          )) &&
        (!setup->hbao_active ||
         (btbh_gpu_scene_ensure_hbao_pipelines(
              window,
              post_vertex_shader_path,
              post_fragment_shader_path,
              shader_format,
              report
          ) &&
          btbh_gpu_scene_ensure_hbao_targets(
              window,
              render_width,
              render_height,
              report
          ))) &&
        btbh_gpu_scene_ensure_post_stage_pipelines(
            window,
            post_vertex_shader_path,
            post_fragment_shader_path,
            shader_format,
            report
        ) &&
        (!setup->hdr_composite_active ||
         btbh_gpu_scene_ensure_hdr_composite_pipelines(
             window,
             post_vertex_shader_path,
             post_fragment_shader_path,
             shader_format,
             report
         )) &&
        (!setup->bloom_active ||
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
        ) &&
        /* The refracting batches' copy of the scene. */
        (!setup->refraction_active ||
         btbh_gpu_scene_ensure_refraction_target(
             window,
             render_width,
             render_height,
             report
         ));
    /* Without the distance pass soft particles simply draw unfaded; a missing pass never fails
       the frame. */
    BTBHGPUSceneReport scene_distance_report;
    SDL_zero(scene_distance_report);
    /* Refraction reads the distance too: a displaced sample nearer than the refracting surface (a unit in
       front of it) falls back to the undisplaced one. So do lit particles (RFC 0013): their soft fade, and their fog
       made up for the composite's at the depth behind them. */
    *scene_distance_ready =
        (setup->soft_particles_active || setup->refraction_active || setup->lit_particles_active) &&
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
    return targets_ready;
}

/* A present's camera arguments as its one view (RFC 0014: the present draws views); false, the view zeroed, when one of
   them is NULL (the present then refuses the call, as before). */
static bool btbh_gpu_scene_camera_view(
    const float view_projection[16],
    const float camera_right[3],
    const float camera_up[3],
    const float camera_position[3],
    BTBHGPUSceneView *view
) {
    SDL_zerop(view);
    if (view_projection == NULL || camera_right == NULL || camera_up == NULL || camera_position == NULL) {
        return false;
    }
    (void)memcpy(view->view_projection, view_projection, sizeof(view->view_projection));
    (void)memcpy(view->right, camera_right, sizeof(view->right));
    (void)memcpy(view->up, camera_up, sizeof(view->up));
    (void)memcpy(view->position, camera_position, sizeof(view->position));
    return true;
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
    /* The cameras (RFC 0014, since 0.16.0): one view for the present and a render into a target, built from its camera
       arguments (NULL when one of them was NULL, which is refused as before); up to BTBH_GPU_SCENE_MAX_VIEWS for
       btbh_gpu_window_scene_present_views (views_call: its views are refused as that call refuses them, and
       view_reports, when not NULL, takes view_count entries). */
    const BTBHGPUSceneView *views,
    uint32_t view_count,
    bool views_call,
    BTBHGPUSceneViewReport *view_reports,
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
    /* The size the finished scene is upscaled (or not) to: the window's drawable size when zero,
       else a render target's (btbh_gpu_window_scene_render_to_target, never temporal). */
    uint32_t output_width,
    uint32_t output_height,
    /* A target view's rect (since 0.15.0): the part of a render target the scene shades. NULL for the window's frame
       and every other present: every pixel. */
    const BTBHGPUSceneScissor *scissor,
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
    const BTBHGPUSceneRunsMode runs_mode = btbh_gpu_scene_runs_mode();
    const bool runs_enabled = runs_mode != BTBH_GPU_SCENE_RUNS_OFF;
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
    if (views_call) {
        if (view_reports != NULL && view_count >= 1u && view_count <= BTBH_GPU_SCENE_MAX_VIEWS) {
            (void)memset(view_reports, 0, sizeof(*view_reports) * view_count);
        }
        if (!btbh_gpu_scene_views_valid(views, view_count, batches != NULL ? batch_count : 0u, report)) {
            return report->result;
        }
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
        views == NULL ||
        view_count == 0u ||
        !isfinite(views[0].position[0]) ||
        !isfinite(views[0].position[1]) ||
        !isfinite(views[0].position[2]) ||
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
            batch_count,
            true
        )) {
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            "Invalid GPU model instance scene submission"
        );
        return report->result;
    }
    if (!btbh_gpu_scene_validate_world_details(window, batches, directional_shadow_visibility, views, view_count,
            batch_count, report)) {
        return report->result;
    }
    /* View 0's camera (the present's own); RFC 0014: a clipped frame (two views, or one clipped) draws each view clipped
       to its half, with neither the radiance cascades nor FSR 3. */
    const float *requested_view_projection = views[0].view_projection;
    const float *camera_right = views[0].right;
    const float *camera_up = views[0].up;
    const bool views_clipped = btbh_gpu_scene_views_clipped(views, view_count);
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
        post->radiance_cascades.tier == BTBH_GPU_SCENE_RC_AUTO && !views_clipped;
    BTBHGPUSceneFrameSetup setup;
    /* (The lazy creation log: what the present makes in the steps it shares with the prewarm, a prewarm makes.) */
    btbh_gpu_lazy_creation_prewarm_step(true);
    const bool frame_prepared = btbh_gpu_scene_prepare_frame(
            window,
            fast_shadow_vertex_shader_path,
            fast_shadow_fragment_shader_path,
            shadow_vertex_shader_path,
            animated_shadow_vertex_shader_path,
            shadow_fragment_shader_path,
            fragment_shader_path,
            post_vertex_shader_path,
            post_fragment_shader_path,
            shader_format,
            render_width,
            render_height,
            output_width,
            views_clipped,
            lighting,
            post,
            batches,
            scene_visibility,
            batch_count,
            &setup,
            report
        );
    btbh_gpu_lazy_creation_prewarm_step(false);
    if (!frame_prepared) {
        return report->result;
    }
    BTBHGPUSceneTelemetryReport *telemetry =
        &window->scene_telemetry;
    BTBHGPUSceneRuntime *runtime = window->scene_runtime;
    /* BTBH_GPU_SCENE_RUNS=verify (since 0.13.0): NULL unless the mode is on. */
    BTBHGPUSceneRunVerify *const run_verify =
        btbh_gpu_scene_run_verify_prepare(window, runtime, telemetry, runs_mode);
    const bool radiance_cascades_active = setup.radiance_cascades_active;
    const bool radiance_cascades_fallback_debug =
        setup.radiance_cascades_fallback_debug;
    const bool temporal_active = setup.temporal_active;
    const int temporal_window_width = setup.temporal_window_width;
    const uint8_t upscale_mode = setup.upscale_mode;
    const uint8_t anti_aliasing = setup.anti_aliasing;
    const bool ambient_occlusion_active = setup.ambient_occlusion_active;
    const bool hbao_active = setup.hbao_active;
    const bool ssr_active = setup.ssr_active;
    const bool ssao_resolve_active = setup.ssao_resolve_active;
    const bool bloom_active = setup.bloom_active;
    const bool hdr_composite_active = setup.hdr_composite_active;
    const bool refraction_active = setup.refraction_active;
    const bool auxiliary_geometry_outputs = setup.auxiliary_geometry_outputs;
    const uint32_t *shadow_enabled = setup.shadow_enabled;
    const bool omni_shadows_active = setup.omni_shadows_active;
    const bool shadows_active = setup.shadows_active;
    const bool scene_ambient_targets = setup.scene_ambient_targets;
    const bool scene_contact_targets = setup.scene_contact_targets;
    /* KEEP_HBAO's contact part lays the HBAO over the opaque scene before the cascades compose it with their bounce
       light; its debug view (the occlusion alone, in grey) stays after them, as without the flag. */
    const bool hbao_before_cascades = scene_contact_targets && post->ssao_debug_view == 0u;
    const bool cascades_fast_unlit = setup.cascades_fast_unlit;
    const bool ssr_surface_reflectivity = setup.ssr_surface_reflectivity;
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
    /* RFC 0014: each view as the passes draw it. View 0 is the camera above; another view's matrices were checked with
       the views (a clipped frame is never temporal: no jitter, no previous matrix of its own). */
    BTBHGPUSceneViewFrame view_frames[BTBH_GPU_SCENE_MAX_VIEWS];
    SDL_zeroa(view_frames);
    for (uint32_t view_index = 0u; view_index < view_count; ++view_index) {
        BTBHGPUSceneViewFrame *frame = &view_frames[view_index];
        frame->view = &views[view_index];
        frame->lighting = views[view_index].lighting != NULL ? views[view_index].lighting : lighting;
        frame->visibility = views[view_index].visibility;
        frame->order_visibility = scene_visibility;
        if (view_index == 0u) {
            (void)memcpy(frame->view_projection, view_projection, sizeof(frame->view_projection));
            (void)memcpy(frame->inverse_view_projection, inverse_view_projection, sizeof(frame->inverse_view_projection));
            frame->projection_scale_y = projection_scale_y;
            (void)memcpy(
                frame->previous_view_projection,
                previous_view_projection,
                sizeof(frame->previous_view_projection)
            );
        } else {
            const float *matrix = views[view_index].view_projection;
            (void)memcpy(frame->view_projection, matrix, sizeof(frame->view_projection));
            (void)btbh_gpu_scene_invert_matrix4(frame->view_projection, frame->inverse_view_projection);
            frame->projection_scale_y = sqrtf(matrix[1] * matrix[1] + matrix[5] * matrix[5] + matrix[9] * matrix[9]);
            (void)memcpy(frame->previous_view_projection, matrix, sizeof(frame->previous_view_projection));
        }
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
            temporal_active ? runtime : NULL,
            window != NULL ? &window->bone_palette_cache : NULL
        ) ||
        bone_count > UINT32_MAX / (sizeof(float) * 16u) ||
        !btbh_gpu_scene_prepare_dig_bindings(window, runtime, batches, batch_count)
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
    if (!btbh_gpu_scene_check_scene_shader(
            runtime,
            batches,
            batch_count,
            fragment_shader_path,
            unshadowed_fragment_shader_path,
            shader_format,
            setup.shadows_active,
            setup.omni_shadows_active,
            setup.radiance_cascades_active,
            report
        ) ||
        !btbh_gpu_scene_check_rc_shader(
            runtime,
            post,
            post_fragment_shader_path,
            shader_format,
            setup.radiance_cascades_active,
            report
        ) ||
        !btbh_gpu_scene_check_uv_rect_shaders(
            runtime,
            batches,
            batch_count,
            vertex_shader_path,
            post_vertex_shader_path,
            shader_format,
            setup.radiance_cascades_active,
            report
        ) ||
        !btbh_gpu_scene_check_particle_shadow_shaders(
            runtime,
            batches,
            batch_count,
            fragment_shader_path,
            unshadowed_fragment_shader_path,
            shader_format,
            setup.particle_shadow_active,
            setup.particle_shadow,
            setup.shadows_active,
            setup.omni_shadows_active,
            setup.radiance_cascades_active,
            report
        ) ||
        (views_clipped &&
         !btbh_gpu_scene_check_view_shaders(
            runtime,
            batches,
            batch_count,
            bone_offsets,
            vertex_shader_path,
            animated_vertex_shader_path,
            post_vertex_shader_path,
            shader_format,
            report
        ))) {
        free(bone_matrices);
        free(bone_offsets);
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
    /* BTBH_GPU_SCENE_OPAQUE_SORT_FRONT_TO_BACK(_LESS_EQUAL): the opaque pass draws in this order. The bone offsets
       stay laid out as the batches were given, so it also notes where each batch's geometries start among them. */
    const bool opaque_sorted =
        post->opaque_sort != BTBH_GPU_SCENE_OPAQUE_SORT_SUBMITTED;
    /* RFC 0014: each view sorts its own batches (the call's visibility and its own), by its own camera. */
    const bool view_orders = opaque_sorted && (view_count > 1u || views[0].visibility != NULL);
    if (opaque_sorted &&
        (!btbh_gpu_scene_ensure_opaque_order(runtime, batch_count) ||
         (view_orders && !btbh_gpu_scene_ensure_view_order(runtime, batch_count)))) {
        free(bone_matrices);
        free(bone_offsets);
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_PLATFORM_FAILURE,
            "Could not allocate the GPU scene's opaque order"
        );
        return report->result;
    }
    for (uint32_t view_index = 0u; opaque_sorted && view_index < view_count; ++view_index) {
        BTBHGPUSceneViewFrame *frame = &view_frames[view_index];
        const uint64_t order_started_ticks = SDL_GetTicksNS();
        const uint8_t *view_visibility = views[view_index].visibility;
        if (view_visibility != NULL && scene_visibility == NULL) {
            frame->order_visibility = view_visibility;
        } else if (view_visibility != NULL) {
            uint8_t *combined = runtime->view_visibility + (size_t)view_index * runtime->view_order_capacity;
            for (uint32_t batch_index = 0u; batch_index < batch_count; ++batch_index) {
                combined[batch_index] =
                    scene_visibility[batch_index] != 0u && view_visibility[batch_index] != 0u ? 1u : 0u;
            }
            frame->order_visibility = combined;
        }
        frame->opaque_order = view_index == 0u ? runtime->opaque_order : runtime->view_opaque_order;
        frame->opaque_order_count = btbh_gpu_scene_opaque_order(
            batches,
            frame->order_visibility,
            batch_count,
            instances,
            views[view_index].view_projection,
            post->opaque_sort == BTBH_GPU_SCENE_OPAQUE_SORT_FRONT_TO_BACK_LESS_EQUAL,
            frame->opaque_order,
            runtime->opaque_order_keys
        );
        frame->order_milliseconds = (double)(SDL_GetTicksNS() - order_started_ticks) / 1000000.0;
    }
    btbh_gpu_lazy_creation_prewarm_step(true);
    const bool pipelines_ready = btbh_gpu_scene_ensure_model_pipelines(
            window,
            vertex_shader_path,
            animated_vertex_shader_path,
            fragment_shader_path,
            unshadowed_fragment_shader_path,
            shader_format,
            &setup,
            batches,
            batch_count,
            bone_offsets,
            prepared_geometry_count,
            opaque_sorted ? runtime->opaque_geometry_base : NULL,
            report
        );
    btbh_gpu_lazy_creation_prewarm_step(false);
    if (!pipelines_ready) {
        free(bone_matrices);
        free(bone_offsets);
        return report->result;
    }
    /* RFC 0014: which maps a second view has of its own, and for a clipped frame its screen-space passes' pipelines and
       those maps (made where missing). */
    bool own_sun = false;
    bool own_spot = false;
    btbh_gpu_scene_view_own_maps(views, view_count, lighting, setup.shadow_enabled, &own_sun, &own_spot);
    bool view_distance_ready = false;
    if (views_clipped) {
        btbh_gpu_lazy_creation_prewarm_step(true);
        const bool views_ready =
            btbh_gpu_scene_ensure_view_pipelines(
                window,
                &setup,
                post_vertex_shader_path,
                post_fragment_shader_path,
                shader_format,
                &view_distance_ready,
                report
            ) &&
            btbh_gpu_scene_ensure_view_shadow_maps(
                window,
                own_sun,
                own_spot,
                setup.particle_shadow_active,
                setup.particle_shadow,
                report
            );
        btbh_gpu_lazy_creation_prewarm_step(false);
        if (!views_ready) {
            free(bone_matrices);
            free(bone_offsets);
            return report->result;
        }
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
    /* The window's upload queue (since 0.11.0: BTBH_GPU_CUSTOM_MESH_UPDATE_QUEUED, BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED)
       goes in the same transfer, after the instances, bones and motions (at a multiple of 16), and in the same copy
       pass. Without any, the transfer is as before. */
    const uint32_t mesh_queue_bytes = btbh_gpu_upload_queue_size(window);
    const uint32_t scene_upload_bytes = instance_bytes + bone_bytes + motion_bytes;
    const uint32_t mesh_queue_offset = mesh_queue_bytes > 0u
        ? (scene_upload_bytes + 15u) & ~15u
        : scene_upload_bytes;
    if (mesh_queue_bytes > 0u &&
        (scene_upload_bytes > UINT32_MAX - 15u || mesh_queue_bytes > UINT32_MAX - mesh_queue_offset)) {
        free(bone_matrices);
        free(bone_offsets);
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            "GPU scene upload exceeds buffer limits"
        );
        return report->result;
    }
    const uint32_t transfer_bytes = mesh_queue_bytes > 0u
        ? mesh_queue_offset + mesh_queue_bytes
        : scene_upload_bytes;
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
        const uint64_t map_started_ticks = btbh_gpu_lazy_creation_clock();
        void *mapped = SDL_MapGPUTransferBuffer(device, transfer, true);
        btbh_gpu_lazy_creation_call_timed(BTBH_GPU_LAZY_CALL_SCENE_UPLOAD_MAP, map_started_ticks);
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
        if (mesh_queue_bytes > 0u) {
            btbh_gpu_upload_queue_write(window, (uint8_t *)mapped + mesh_queue_offset);
        }
        SDL_UnmapGPUTransferBuffer(device, transfer);
    }
    free(bone_matrices);
    bone_matrices = NULL;

    const uint64_t command_started_ticks = btbh_gpu_lazy_creation_clock();
    SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(device);
    btbh_gpu_lazy_creation_call_timed(BTBH_GPU_LAZY_CALL_SCENE_COMMAND_BUFFER, command_started_ticks);
    if (command == NULL) {
        free(bone_offsets);
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return report->result;
    }
    /* (A scene rendered to a target is no frame of its own in the cost log: its triangles count in the frame
       it is drawn for, its time in a line of its own, btbh_gpu_cost_submit_target.) */
    if (output_width == 0u) {
        btbh_gpu_cost_begin_frame(device);
    }
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
        if (mesh_queue_bytes > 0u) {
            /* Before anything draws: every pass of this scene sees the queued data. */
            (void)btbh_gpu_upload_queue_record(window, copy, transfer, mesh_queue_offset);
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

    /* The maps by slot (RFC 0014): slot 0 is view 0's (the present's, read by every view that shares it), slot 1 a second
       view's own. View 0's sun and spot matrices are its lighting's (the call's without one); the secondary and omni
       maps are always the call's. A map's casters: the view's shadow_visibility when the map is its own (NULL: the
       call's directional_shadow_visibility for the sun, every spot caster for the spot), the call's when it is shared;
       its billboard casters face its view's camera. */
    const float *sun_matrices[2] = {
        view_frames[0].lighting->directional_shadow_view_projection,
        view_frames[view_count - 1u].lighting->directional_shadow_view_projection
    };
    const float *spot_matrices[2] = {
        view_frames[0].lighting->spot_shadow_view_projection,
        view_frames[view_count - 1u].lighting->spot_shadow_view_projection
    };
    const uint8_t *sun_casters[2] = {
        (view_count > 1u && !own_sun) || views[0].shadow_visibility == NULL
            ? directional_shadow_visibility
            : views[0].shadow_visibility,
        views[view_count - 1u].shadow_visibility != NULL
            ? views[view_count - 1u].shadow_visibility
            : directional_shadow_visibility
    };
    const uint8_t *spot_casters[2] = {
        (view_count > 1u && !own_spot) || lighting->spot_shadow_is_omni != 0u
            ? NULL
            : views[0].shadow_visibility,
        views[view_count - 1u].shadow_visibility
    };
    const uint32_t shadow_update_periods[3] = {
        btbh_gpu_scene_shadow_update_period(window, 0u),
        btbh_gpu_scene_shadow_update_period(window, 1u),
        btbh_gpu_scene_shadow_update_period(window, 2u)
    };
    /* The maps in the order the update periods count them: a second view's own sun map right after view 0's, its own
       spot map after view 0's, so that two maps of a kind take turns. Without a second view's maps, as before 0.16.0:
       primary, secondary, spot. */
    static const uint32_t shadow_map_kinds[5] = { 0u, 0u, 1u, 2u, 2u };
    static const uint32_t shadow_map_slots[5] = { 0u, 1u, 0u, 0u, 1u };
    static const char *shadow_debug_labels[5] = {
        "BTBH Shadow Primary",
        "BTBH Shadow Primary View 1",
        "BTBH Shadow Secondary",
        "BTBH Shadow Local",
        "BTBH Shadow Local View 1"
    };
    uint32_t active_shadow_ordinal = 0u;
    const uint64_t shadow_started_ticks = SDL_GetTicksNS();
    for (uint32_t shadow_map = 0u;
         shadow_map < 5u;
         ++shadow_map) {
        const uint32_t shadow_index = shadow_map_kinds[shadow_map];
        const uint32_t shadow_slot = shadow_map_slots[shadow_map];
        const uint32_t view_map = shadow_index == 0u ? 0u : 1u;
        uint8_t *retained_active = shadow_slot == 0u
            ? &runtime->retained_shadow_active[shadow_index]
            : &runtime->view_retained_shadow_active[view_map];
        uint8_t *retained_ready = shadow_slot == 0u
            ? &runtime->retained_shadow_ready[shadow_index]
            : &runtime->view_retained_shadow_ready[view_map];
        float *retained_matrix = shadow_slot == 0u
            ? runtime->retained_shadow_view_projection[shadow_index]
            : runtime->view_retained_shadow_view_projection[view_map];
        const bool map_enabled =
            shadow_enabled[shadow_index] != 0u &&
            (shadow_slot == 0u || (shadow_index == 0u ? own_sun : own_spot));
        if (!map_enabled) {
            *retained_active = 0u;
            continue;
        }
        const uint32_t shadow_update_period =
            shadow_update_periods[shadow_index];
        const uint32_t update_phase =
            active_shadow_ordinal % shadow_update_period;
        active_shadow_ordinal += 1u;
        const bool force_update =
            *retained_active == 0u ||
            *retained_ready == 0u ||
            (shadow_slot == 0u &&
             shadow_index == BTBH_GPU_SCENE_SHADOW_SPOT &&
             runtime->retained_spot_shadow_is_omni !=
                (uint8_t)(lighting->spot_shadow_is_omni != 0u));
        *retained_active = 1u;
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
            shadow_debug_labels[shadow_map]
        );
        bool shadow_rendered = true;
        const bool omni_shadow =
            shadow_slot == 0u &&
            shadow_index == BTBH_GPU_SCENE_SHADOW_SPOT &&
            lighting->spot_shadow_is_omni != 0u;
        const float *map_camera_right = shadow_slot == 0u ? camera_right : views[view_count - 1u].right;
        const float *map_camera_up = shadow_slot == 0u ? camera_up : views[view_count - 1u].up;
        const float *map_matrix = shadow_index == 0u
            ? sun_matrices[shadow_slot]
            : (shadow_index == 1u
                ? lighting->secondary_directional_shadow_view_projection
                : spot_matrices[shadow_slot]);
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
                        NULL,
                        batch_count,
                        (uint8_t)face,
                        face == 0u,
                        detailed_prepare_telemetry,
                        telemetry,
                        runs_enabled,
                        run_verify
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
                shadow_slot == 0u
                    ? runtime->directional_shadow_textures[shadow_index]
                    : runtime->view_shadow_textures[view_map],
                map_matrix,
                map_camera_right,
                map_camera_up,
                transient_instance_buffer,
                transient_instance_vertex_buffer,
                bone_buffer,
                bone_offsets,
                batches,
                shadow_index == 0u ? sun_casters[shadow_slot] : directional_shadow_visibility,
                shadow_index == BTBH_GPU_SCENE_SHADOW_SPOT ? spot_casters[shadow_slot] : NULL,
                batch_count,
                0u,
                true,
                detailed_prepare_telemetry,
                telemetry,
                runs_enabled,
                run_verify
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
                retained_matrix,
                map_matrix,
                sizeof(runtime->retained_shadow_view_projection[0])
            );
        }
        if (shadow_slot == 0u && shadow_index == BTBH_GPU_SCENE_SHADOW_SPOT) {
            runtime->retained_spot_shadow_is_omni =
                (uint8_t)omni_shadow;
        }
        *retained_ready = 1u;
        runtime->shadow_render_count += 1u;
    }
    runtime->shadow_frame_serial += 1u;
    /* The maps each view reads (RFC 0014: view 0's, or a second view's own), and their matrices as the receivers read
       them: the retained one when the map is kept over several frames. */
    for (uint32_t view_index = 0u; view_index < view_count; ++view_index) {
        BTBHGPUSceneViewFrame *frame = &view_frames[view_index];
        frame->sun_slot = view_index > 0u && own_sun ? 1u : 0u;
        frame->spot_slot = view_index > 0u && own_spot ? 1u : 0u;
        frame->sun_map = frame->sun_slot != 0u
            ? runtime->view_shadow_textures[0]
            : runtime->directional_shadow_textures[0];
        frame->spot_map = frame->spot_slot != 0u
            ? runtime->view_shadow_textures[1]
            : runtime->directional_shadow_textures[2];
        frame->transmittance_map = frame->sun_slot != 0u
            ? runtime->view_particle_transmittance_texture
            : runtime->particle_transmittance_texture;
        frame->sun_matrix = frame->sun_slot != 0u
            ? (runtime->view_retained_shadow_ready[0] != 0u
                ? runtime->view_retained_shadow_view_projection[0]
                : sun_matrices[1])
            : (runtime->retained_shadow_ready[0] != 0u
                ? runtime->retained_shadow_view_projection[0]
                : sun_matrices[0]);
        frame->spot_matrix = frame->spot_slot != 0u
            ? (runtime->view_retained_shadow_ready[1] != 0u
                ? runtime->view_retained_shadow_view_projection[1]
                : spot_matrices[1])
            : (runtime->retained_shadow_ready[2] != 0u
                ? runtime->retained_shadow_view_projection[2]
                : spot_matrices[0]);
    }
    if (setup.particle_shadow_active) {
        /* RFC 0013 phase 2: the particle transmittance map, with the sun matrix the receivers read (the retained one
           when the sun's map is kept over several frames), redrawn every frame (the puffs move). RFC 0014: a second
           view with a sun map of its own has its own transmittance map too. */
        for (uint32_t sun_slot = 0u; sun_slot < (own_sun ? 2u : 1u); ++sun_slot) {
            SDL_InsertGPUDebugLabel(
                command,
                sun_slot == 0u ? "BTBH Particle Transmittance" : "BTBH Particle Transmittance View 1"
            );
            if (!btbh_gpu_scene_render_particle_transmittance(
                    command,
                    runtime,
                    sun_slot == 0u
                        ? runtime->particle_transmittance_texture
                        : runtime->view_particle_transmittance_texture,
                    view_frames[sun_slot == 0u ? 0u : view_count - 1u].sun_matrix,
                    transient_instance_buffer,
                    batches,
                    sun_casters[sun_slot],
                    batch_count
                )) {
                btbh_gpu_scene_cancel_submission(window, command);
                free(bone_offsets);
                btbh_gpu_scene_error(report, BTBH_GPU_DEVICE_FAILURE, SDL_GetError());
                return report->result;
            }
        }
    }
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

    uint32_t drawable_width = 0u;
    uint32_t drawable_height = 0u;
    bool scene_distance_ready = false;
    report->cpu_swapchain_acquire_milliseconds = 0.0;
    const uint64_t target_ensure_started_ticks = SDL_GetTicksNS();
    btbh_gpu_lazy_creation_prewarm_step(true);
    const bool targets_ready = btbh_gpu_scene_ensure_frame_targets(
        window,
        &setup,
        post,
        post_vertex_shader_path,
        post_fragment_shader_path,
        shader_format,
        render_width,
        render_height,
        output_width,
        output_height,
        &drawable_width,
        &drawable_height,
        &scene_distance_ready,
        report
    );
    btbh_gpu_lazy_creation_prewarm_step(false);
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
    /* RFC 0014: each view's half as its passes test it, at this drawable size (zero planes: the whole frame). A clipped
       frame's distance pass draws with its clipped pipeline. */
    for (uint32_t view_index = 0u; view_index < view_count; ++view_index) {
        btbh_gpu_scene_view_planes(
            views[view_index].clip,
            drawable_width,
            drawable_height,
            render_width,
            render_height,
            view_frames[view_index].clip_plane,
            view_frames[view_index].uv_plane
        );
    }
    if (views_clipped) {
        scene_distance_ready = scene_distance_ready && view_distance_ready;
    }
    /* A target view's rect (since 0.15.0) for the passes at the render size, at half of it (the SSAO's and the
       HBAO's) and at the output size (the upscalers'); all NULL without one. The shadow maps are never limited. */
    SDL_Rect render_scissor_rect;
    SDL_Rect half_scissor_rect;
    SDL_Rect level_scissor_rect;
    const SDL_Rect *render_scissor =
        btbh_gpu_scene_scissor_at(scissor, render_width, render_height, &render_scissor_rect);
    const SDL_Rect *half_scissor = btbh_gpu_scene_scissor_at(
        scissor,
        (render_width + 1u) / 2u,
        (render_height + 1u) / 2u,
        &half_scissor_rect
    );
    /* (A render into a target has the target's size as its drawable size.) */
    const SDL_Rect *output_scissor = btbh_gpu_scene_output_scissor(scissor);

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
    /* RFC 0013: the cascades ran this frame (not their fallback view): the lit particles take their light. */
    bool radiance_cascades_lit = false;
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
    /* BTBH_GPU_SCENE_POST_ADDITIVE_CASCADES_KEEP_HBAO with the SSAO (since 0.12.0): the ambient light, from the scene's
       own targets, as without the cascades. */
    post_uniforms.ssao_enabled = ambient_occlusion_active
        ? (radiance_cascades_active && !scene_ambient_targets
            ? BTBH_GPU_SCENE_SSAO_RC_CONTACT_MODE
            : 1u)
        : 0u;
    post_uniforms.ssr_options[0] =
        radiance_cascades_active && !scene_ambient_targets ? 1.0f : 0.0f;
    post_uniforms.ssr_options[1] =
        scene_ambient_targets || ssr_surface_reflectivity ? 1.0f : 0.0f;
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
        post_uniforms.ssr_hit_options[0] =
            (post->flags & BTBH_GPU_SCENE_POST_SSR_FACING_HITS) != 0u ? 1.0f : 0.0f;
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
    /* RFC 0014: each view's block for a clipped frame's SSAO passes and reflections: its matrices and its half
       (ssr_hit_options yzw). The grading and output passes read none of them and take view 0's block. */
    BTBHGPUPostUniforms view_post_uniforms[BTBH_GPU_SCENE_MAX_VIEWS];
    for (uint32_t view_index = 0u; view_index < view_count; ++view_index) {
        BTBHGPUPostUniforms *block = &view_post_uniforms[view_index];
        const BTBHGPUSceneViewFrame *frame = &view_frames[view_index];
        *block = post_uniforms;
        (void)memcpy(block->view_projection, frame->view_projection, sizeof(block->view_projection));
        (void)memcpy(
            block->inverse_view_projection,
            frame->inverse_view_projection,
            sizeof(block->inverse_view_projection)
        );
        block->projection_scale_y = frame->projection_scale_y;
        block->ssr_hit_options[1] = frame->uv_plane[1];
        block->ssr_hit_options[2] = frame->uv_plane[2];
        block->ssr_hit_options[3] = frame->uv_plane[3];
    }
    /* Phases: opaque, transparent, and on frames that draw one, refraction (a transparent pass over a copy of
       the scene so far). The radiance cascades' debug views (since 0.10.0) take the opaque pass alone: the
       transparent and refracting batches drew over the view (Be The Bullet Hell v2's additive night overlays read
       as the cascades' light). */
    const bool radiance_cascades_debug_output =
        (radiance_cascades_active || radiance_cascades_fallback_debug) &&
        post->radiance_cascades.debug_view != BTBH_GPU_SCENE_RC_DEBUG_NONE;
    const uint32_t geometry_phase_count = radiance_cascades_debug_output
        ? 1u
        : BTBH_GPU_SCENE_TRANSPARENCY_COUNT + (refraction_active ? 1u : 0u);
    for (uint32_t transparency_phase = 0u;
         transparency_phase < geometry_phase_count;
         ++transparency_phase) {
        const uint64_t geometry_phase_started_ticks = SDL_GetTicksNS();
        const bool transparent_phase = transparency_phase != 0u;
        const bool refraction_phase =
            transparency_phase == BTBH_GPU_SCENE_TRANSPARENCY_COUNT;
        const bool phase_auxiliary_geometry_outputs =
            !transparent_phase && auxiliary_geometry_outputs;
        if (transparency_phase == 1u && hbao_active && !hbao_before_cascades) {
            /* The horizon-based occlusion, laid over the opaque scene before the transparent pass (under KEEP_HBAO
               with its contact mode, before the cascades instead: below). Under KEEP_HBAO without contact AO: the
               ambient light, from the scene's own targets. */
            const uint32_t hbao_mode =
                post->ssao_contact != 0u
                    ? BTBH_GPU_SCENE_HBAO_CONTACT_MODE
                    : (radiance_cascades_active && !scene_ambient_targets
                        ? BTBH_GPU_SCENE_SSAO_RC_CONTACT_MODE
                        : 1u);
            SDL_GPUTexture *hbao_kept_emission =
                scene_contact_targets ? runtime->ambient_texture : runtime->emission_texture;
            /* (The cost log's pass frames: the occlusion's step.) */
            if (!btbh_gpu_scene_cost_step(window, &command, BTBH_GPU_COST_STEP_AO, report)) {
                free(bone_offsets);
                return report->result;
            }
            /* (RFC 0014: a clipped frame's passes draw each view clipped to its half.) */
            if (!(views_clipped
                    ? btbh_gpu_scene_encode_hbao_views(
                        command,
                        runtime,
                        post,
                        view_frames,
                        view_count,
                        hbao_mode,
                        hbao_kept_emission,
                        radiance_cascades_post_input
                    )
                    : btbh_gpu_scene_encode_hbao(
                        command,
                        runtime,
                        post,
                        inverse_view_projection,
                        projection_scale_y,
                        hbao_mode,
                        hbao_kept_emission,
                        radiance_cascades_post_input,
                        scissor
                    ))) {
                btbh_gpu_scene_cancel_submission(window, command);
                free(bone_offsets);
                btbh_gpu_scene_error(
                    report,
                    BTBH_GPU_DEVICE_FAILURE,
                    SDL_GetError()
                );
                return report->result;
            }
            /* (With the SSAO resolve's reflections on as well, its split takes both.) */
            if (!ssao_resolve_active &&
                !btbh_gpu_scene_cost_split(
                    window,
                    &command,
                    BTBH_GPU_COST_GROUP_SSAO,
                    report
                )) {
                free(bone_offsets);
                return report->result;
            }
        }
        if (transparency_phase == 1u && ssao_resolve_active) {
            /* Ambient occlusion darkens the opaque scene only: resolved before the
               transparent pass, an effect drawn over a dark corner keeps its light. */
            bool ssao_ok = true;
            /* (The cost log's pass frames: the occlusion's step, then the reflections' for the resolve.) */
            if (!btbh_gpu_scene_cost_step(
                    window,
                    &command,
                    ambient_occlusion_active ? BTBH_GPU_COST_STEP_AO : BTBH_GPU_COST_STEP_SSR,
                    report
                )) {
                free(bone_offsets);
                return report->result;
            }
            SDL_GPUTextureSamplerBinding ssao_samplers[2];
            SDL_zeroa(ssao_samplers);
            ssao_samplers[0].texture = runtime->depth_texture;
            ssao_samplers[0].sampler =
                runtime->scene_color_sampler;
            ssao_samplers[1].texture = runtime->normal_texture;
            ssao_samplers[1].sampler =
                runtime->scene_color_sampler;
            /* Reflections alone skip the occlusion passes: the resolve then reads no occlusion. RFC 0014: a clipped
               frame's passes draw each view clipped to its half, with its own block. */
            ssao_ok = !ambient_occlusion_active || (views_clipped
                ? btbh_gpu_scene_render_fullscreen_views(
                    command,
                    "BTBH SSAO Views",
                    runtime->view_pipelines[BTBH_GPU_SCENE_VIEW_PIPELINE_SSAO],
                    runtime->ssao_texture,
                    false,
                    view_post_uniforms,
                    (uint32_t)sizeof(view_post_uniforms[0]),
                    sizeof(view_post_uniforms[0]),
                    ssao_samplers,
                    2u,
                    view_frames,
                    view_count
                )
                : btbh_gpu_scene_render_fullscreen(
                    command,
                    "BTBH SSAO",
                    runtime->ssao_pipeline,
                    runtime->ssao_texture,
                    &post_uniforms,
                    (uint32_t)sizeof(post_uniforms),
                    ssao_samplers,
                    2u,
                    half_scissor
                ));
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
                ssao_ok = views_clipped
                    ? btbh_gpu_scene_render_fullscreen_views(
                        command,
                        "BTBH SSAO Denoise Views",
                        runtime->view_pipelines[BTBH_GPU_SCENE_VIEW_PIPELINE_SSAO_DENOISE],
                        runtime->ssao_denoised_texture,
                        false,
                        view_post_uniforms,
                        (uint32_t)sizeof(view_post_uniforms[0]),
                        sizeof(view_post_uniforms[0]),
                        denoise_samplers,
                        3u,
                        view_frames,
                        view_count
                    )
                    : btbh_gpu_scene_render_fullscreen(
                        command,
                        "BTBH SSAO Denoise",
                        runtime->ssao_denoise_pipeline,
                        runtime->ssao_denoised_texture,
                        &post_uniforms,
                        (uint32_t)sizeof(post_uniforms),
                        denoise_samplers,
                        3u,
                        half_scissor
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
            if (ssao_ok &&
                !btbh_gpu_scene_cost_step(
                    window,
                    &command,
                    ssr_active ? BTBH_GPU_COST_STEP_SSR : BTBH_GPU_COST_STEP_AO,
                    report
                )) {
                free(bone_offsets);
                return report->result;
            }
            if (ssao_ok) {
                ssao_ok = views_clipped
                    ? btbh_gpu_scene_render_fullscreen_views(
                        command,
                        "BTBH SSAO Resolve Views",
                        runtime->view_pipelines[BTBH_GPU_SCENE_VIEW_PIPELINE_SSAO_RESOLVE],
                        runtime->ssao_resolved_texture,
                        false,
                        view_post_uniforms,
                        (uint32_t)sizeof(view_post_uniforms[0]),
                        sizeof(view_post_uniforms[0]),
                        resolve_samplers,
                        7u,
                        view_frames,
                        view_count
                    )
                    : btbh_gpu_scene_render_fullscreen(
                        command,
                        "BTBH SSAO Resolve",
                        runtime->ssao_resolve_pipeline,
                        runtime->ssao_resolved_texture,
                        &post_uniforms,
                        (uint32_t)sizeof(post_uniforms),
                        resolve_samplers,
                        7u,
                        render_scissor
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
            /* (RFC 0014: each view's distance from its own camera.) */
            BTBHGPUSceneDistanceUniforms distance_uniforms[BTBH_GPU_SCENE_MAX_VIEWS];
            SDL_zeroa(distance_uniforms);
            for (uint32_t view_index = 0u; view_index < view_count; ++view_index) {
                (void)memcpy(
                    distance_uniforms[view_index].inverse_view_projection,
                    view_frames[view_index].inverse_view_projection,
                    sizeof(distance_uniforms[view_index].inverse_view_projection)
                );
                distance_uniforms[view_index].camera_position[0] = views[view_index].position[0];
                distance_uniforms[view_index].camera_position[1] = views[view_index].position[1];
                distance_uniforms[view_index].camera_position[2] = views[view_index].position[2];
            }
            SDL_GPUTextureSamplerBinding distance_sampler;
            SDL_zero(distance_sampler);
            distance_sampler.texture = runtime->depth_texture;
            distance_sampler.sampler = runtime->scene_color_sampler;
            if (!(views_clipped
                    ? btbh_gpu_scene_render_fullscreen_views(
                        command,
                        "BTBH Scene Distance Views",
                        runtime->view_pipelines[BTBH_GPU_SCENE_VIEW_PIPELINE_DISTANCE],
                        runtime->scene_distance_texture,
                        false,
                        distance_uniforms,
                        (uint32_t)sizeof(distance_uniforms[0]),
                        sizeof(distance_uniforms[0]),
                        &distance_sampler,
                        1u,
                        view_frames,
                        view_count
                    )
                    : btbh_gpu_scene_render_fullscreen(
                        command,
                        "BTBH Scene Distance",
                        runtime->scene_distance_pipeline,
                        runtime->scene_distance_texture,
                        &distance_uniforms[0],
                        (uint32_t)sizeof(distance_uniforms[0]),
                        &distance_sampler,
                        1u,
                        render_scissor
                    ))) {
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
        if (render_scissor != NULL) {
            /* A target view's rect: the opaque pass cleared every target whole; the draws shade the rect alone. */
            SDL_SetGPUScissor(pass, render_scissor);
        }
        const char *const pass_name =
            !transparent_phase ? "opaque" : (refraction_phase ? "refraction" : "transparent");
        btbh_gpu_scene_run_verify_begin_pass(run_verify, pass_name);
        SDL_GPUGraphicsPipeline *bound_model_pipeline = NULL;
        SDL_GPUBuffer *bound_index_buffer = NULL;
        /* RFC 0011: the vertex buffer bound last. */
        SDL_GPUBuffer *bound_vertex_buffer = NULL;
        /* RFC 0014: the pass holds every view in turn, each clipped to its half, with its own frame block, batches,
           order and maps; a view's first batch binds and pushes all it needs. Under BTBH_GPU_COST_LOG's split frames a
           second view draws in a pass of its own (the targets loaded), so that the log times the views apart. */
        const bool view_cost_split = view_count > 1u && btbh_gpu_cost_split_device != NULL;
        for (uint32_t view_index = 0u; view_index < view_count; ++view_index) {
        BTBHGPUSceneViewFrame *const view_frame = &view_frames[view_index];
        const uint64_t view_started_ticks = SDL_GetTicksNS();
        if (view_index > 0u && view_cost_split) {
            SDL_EndGPURenderPass(pass);
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
            const SDL_GPUColorTargetInfo *phase_targets = transparent_phase ? transparent_targets : color_targets;
            const uint32_t phase_target_count = transparent_phase
                ? (temporal_active ? 2u : 1u)
                : color_target_count;
            SDL_GPUColorTargetInfo continued_targets[6];
            SDL_zeroa(continued_targets);
            for (uint32_t target = 0u; target < phase_target_count; ++target) {
                continued_targets[target] = phase_targets[target];
                continued_targets[target].load_op = SDL_GPU_LOADOP_LOAD;
                continued_targets[target].cycle = false;
            }
            SDL_GPUDepthStencilTargetInfo continued_depth =
                transparent_phase ? transparent_depth_target : depth_target;
            continued_depth.load_op = SDL_GPU_LOADOP_LOAD;
            continued_depth.stencil_load_op = SDL_GPU_LOADOP_LOAD;
            continued_depth.cycle = false;
            pass = SDL_BeginGPURenderPass(command, continued_targets, phase_target_count, &continued_depth);
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
            btbh_gpu_scene_run_verify_begin_pass(run_verify, pass_name);
            bound_model_pipeline = NULL;
            bound_index_buffer = NULL;
            bound_vertex_buffer = NULL;
        }
        /* RFC 0009: the world-detail table whose block the fragment uniform slot 2 holds in this pass. Pushed data
           stays for the command buffer's later draws, and only table batches read the slot, so a run of batches
           sharing a table pushes its 400 bytes once. */
        const BTBHGPUSceneWorldDetailTable *pushed_table = NULL;
        /* RFC 0012: the height lift whose block the vertex uniform slot 1 holds in this pass, pushed as the table's
           is: once per run of lifted batches sharing it. */
        const BTBHGPUSceneHeightLift *pushed_lift = NULL;
        /* RFC 0013: the lit particles' block, the same for every one in the pass (RFC 0014: in the view), and whether
           fragment uniform slot 1 holds it (every other batch pushes its own block there). */
        BTBHGPUSceneParticleFrameUniforms particle_frame;
        SDL_zero(particle_frame);
        bool particle_frame_pushed = false;
        if (transparent_phase && setup.lit_particles_active) {
            btbh_gpu_scene_particle_frame(
                runtime,
                post,
                view_frame->inverse_view_projection,
                render_width,
                render_height,
                scene_distance_ready,
                hdr_composite_active,
                radiance_cascades_lit,
                setup.particle_shadow_active,
                &particle_frame
            );
        }
        /* RFC 0011: the batch whose state the pass holds (phase 1b; NULL before the first draw and after a skinned
           geometry), and the frame's part of both uniform blocks, filled once a pass (phase 1c; RFC 0014: once a view):
           each batch writes its own fields over it before it pushes them. */
        const BTBHGPUSceneGeometryBatch *run_batch = NULL;
        BTBHGPUSceneUniforms uniforms;
        BTBHGPUSceneFragmentUniforms fragment_uniforms;
        btbh_gpu_scene_frame_uniforms(
            runtime,
            lighting,
            view_frame->lighting,
            post,
            &setup,
            view_frame->view_projection,
            views[view_index].right,
            views[view_index].up,
            views[view_index].position,
            view_frame->previous_view_projection,
            previous_world_curvature,
            temporal_controls,
            temporal_lod_bias,
            view_frame->clip_plane,
            view_frame->sun_matrix,
            view_frame->spot_matrix,
            &uniforms,
            &fragment_uniforms
        );
        SDL_InsertGPUDebugLabel(
            command,
            !transparent_phase
                ? "BTBH Opaque Draws"
                : (refraction_phase
                    ? "BTBH Refraction Draws"
                    : "BTBH Forward Transparent Draws")
        );
        uint32_t geometry_cursor = 0u;
        /* Sorted, the opaque pass walks its own batches in their order (the view's), each from its first geometry's
           place among the prepared ones; otherwise every pass walks all batches as given. */
        const bool phase_ordered = opaque_sorted && !transparent_phase;
        const uint32_t phase_batch_count =
            phase_ordered ? view_frame->opaque_order_count : batch_count;
        for (uint32_t phase_index = 0u;
             phase_index < phase_batch_count;
             ++phase_index) {
        const uint32_t batch_index = phase_ordered
            ? view_frame->opaque_order[phase_index]
            : phase_index;
        if (phase_ordered) {
            geometry_cursor = runtime->opaque_geometry_base[batch_index];
        }
        const BTBHGPUSceneGeometryBatch *batch = &batches[batch_index];
        SDL_GPUBuffer *batch_instance_buffer =
            batch->instance_buffer != NULL
                ? batch->instance_buffer->buffer
                : transient_instance_buffer;
        const uint32_t geometry_count =
            batch->kind == BTBH_GPU_SCENE_GEOMETRY_MODEL
                ? batch->model->primitive_count
                : 1u;
        /* (RFC 0014: the view's instances count each batch it draws once.) */
        bool batch_drawn_in_view = false;
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
                (view_frame->visibility != NULL &&
                 view_frame->visibility[batch_index] == 0u) ||
                batch_transparency_phase != transparency_phase) {
                continue;
            }
            const bool animated = bone_offset != UINT32_MAX;
            /* RFC 0011 phase 1b: a geometry whose batch continues the run (btbh_gpu_scene_batch_continues_run) finds
               its uniforms, stencil reference, pipeline, storage buffers and samplers bound by the run's first batch,
               and binds only its vertex and index buffers. */
            const bool continues_run =
                runs_enabled &&
                !animated &&
                run_batch != NULL &&
                (run_batch == batch || btbh_gpu_scene_batch_continues_run(run_batch, batch));
            /* BTBH_GPU_SCENE_RUNS=verify: a draw that continues the run rebuilds its state and compares it. */
            const bool run_check = continues_run && run_verify != NULL;
            if (continues_run) {
                telemetry->geometry_run_draw_count += 1u;
                if (detailed_prepare_telemetry) {
                    telemetry->geometry_pipeline_bind_skip_count += 1u;
                }
            }
            if (!continues_run || run_check) {
                btbh_gpu_scene_run_verify_draw(run_verify, batch_index, run_check);
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
                uniforms.bone_offset = animated ? bone_offset : 0u;
                uniforms.previous_bone_offset =
                    animated ? previous_bone_offset : 0u;
                uniforms.billboard_mode =
                    (uint32_t)batch->billboard_mode;
                /* RFC 0013: BTBH_GPU_SCENE_BATCH_INSTANCE_UV_RECT (0 for every other batch, as the reserved word was). */
                uniforms.instance_uv_rect = btbh_gpu_particle_uv_rect(batch) ? 1u : 0u;
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
                /* Persistent instances (the static city) have no motion of their own. */
                const bool batch_motion =
                    temporal_motion_ready && batch->instance_buffer == NULL;
                uniforms.temporal_controls[2] = batch_motion ? 1.0f : 0.0f;
                btbh_gpu_scene_run_push_vertex_uniforms(
                    run_verify,
                    command,
                    0u,
                    &uniforms,
                    sizeof(uniforms),
                    &btbh_gpu_scene_run_vertex_layout
                );
                /* (RFC 0013: a lit particle without a normal_texture is flat; the fallback's rg is 127/255.) */
                fragment_uniforms.material_factors[0] =
                    btbh_gpu_particle_lit(batch) && batch->normal_texture == NULL
                        ? 0.0f
                        : batch->normal_scale;
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
                    (batch->rc_receiver != 0u ? 4u : 0u) |
                    (hbao_active
                        ? 32u | ((uint32_t)batch->ssao_mode << 3u)
                        : 0u) |
                    (ssr_surface_reflectivity ? 64u : 0u) |
                    (scene_ambient_targets &&
                     !btbh_gpu_scene_batch_keeps_cascades_look(batch, cascades_fast_unlit)
                        ? (scene_contact_targets ? 128u | 1024u : 128u)
                        : 0u) |
                    ((batch->flags &
                      BTBH_GPU_SCENE_BATCH_RECEIVES_NO_SPOT_SHADOW) != 0u
                        ? 256u
                        : 0u) |
                    /* BTBH_GPU_SCENE_BATCH_INSTANCE_LIGHT_FACTOR (since 0.11.0): the instance's custom x scales its
                       light (production_light_factor.hlsli; the fast unlit shader reads it too). */
                    (btbh_gpu_scene_light_factor(batch) ? 512u : 0u);
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
                /* The reactive mask's scale: the frame's, unless this batch stays out of the mask (a
                   transparent layer that should be accumulated like the opaque scene, such as ground
                   detail drawn over its base). The shader writes the mask only for a positive scale (at
                   zero the pass's second target keeps the shading's ambient term), so "none" is a scale
                   too small to reach the mask's first step (1/255 of full alpha). */
                fragment_uniforms.procedural_controls[2] =
                    transparent_phase && temporal_active
                        ? (batch->reactive_mode == BTBH_GPU_SCENE_REACTIVE_NONE
                            ? BTBH_GPU_SCENE_REACTIVE_NONE_SCALE
                            : post->temporal_reactive_scale)
                        : 0.0f;
                (void)memcpy(
                    fragment_uniforms.detail_parameters,
                    batch->detail_parameters,
                    sizeof(fragment_uniforms.detail_parameters)
                );
                btbh_gpu_scene_run_push_fragment_uniforms(
                    run_verify,
                    command,
                    0u,
                    &fragment_uniforms,
                    shadows_active
                        ? (uint32_t)sizeof(fragment_uniforms)
                        : (uint32_t)offsetof(
                            BTBHGPUSceneFragmentUniforms,
                            directional_shadow_view_projection
                        ),
                    &btbh_gpu_scene_run_fragment_layout
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
                btbh_gpu_scene_run_set_stencil_reference(
                    run_verify,
                    pass,
                    batch->stencil_reference
                );
                SDL_GPUGraphicsPipeline *model_pipeline =
                    *btbh_gpu_scene_pipeline_slot(
                        window->scene_runtime,
                        phase_auxiliary_geometry_outputs,
                        temporal_active,
                        radiance_cascades_active,
                        cascades_fast_unlit,
                        shadows_active,
                        omni_shadows_active,
                        animated,
                        batch,
                        stencil_variant
                    );
                if (run_check) {
                    btbh_gpu_scene_run_verify_pipeline(run_verify, model_pipeline, bound_model_pipeline);
                } else if (model_pipeline != bound_model_pipeline) {
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
                    btbh_gpu_scene_run_bind_vertex_storage_buffers(
                        run_verify,
                        pass,
                        storage_buffers,
                        3u
                    );
                } else {
                    SDL_GPUBuffer *storage_buffers[2] = {
                        batch_instance_buffer,
                        batch_motion_buffer
                    };
                    btbh_gpu_scene_run_bind_vertex_storage_buffers(
                        run_verify,
                        pass,
                        storage_buffers,
                        2u
                    );
                    if (btbh_gpu_height_lift_active(batch)) {
                        /* RFC 0012: the lifted vertex shader's heightfield and block (an unlifted batch binds
                           neither; its shader reads neither). */
                        const BTBHGPUTexture *field = batch->height_lift->heightfield;
                        SDL_GPUTextureSamplerBinding field_binding;
                        SDL_zero(field_binding);
                        field_binding.texture = field->texture;
                        field_binding.sampler = field->sampler;
                        btbh_gpu_scene_run_bind_vertex_samplers(run_verify, pass, &field_binding, 1u);
                        if (batch->height_lift != pushed_lift) {
                            BTBHGPUHeightLiftUniforms lift_uniforms;
                            btbh_gpu_height_lift_pack(batch->height_lift, field->width, field->height, &lift_uniforms);
                            btbh_gpu_scene_run_push_vertex_uniforms(
                                run_verify,
                                command,
                                1u,
                                &lift_uniforms,
                                (uint32_t)sizeof(lift_uniforms),
                                &btbh_gpu_scene_run_lift_layout
                            );
                            if (!run_check) {
                                pushed_lift = batch->height_lift;
                            }
                        }
                    }
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
                if (btbh_gpu_particle_lit(batch)) {
                    /* RFC 0013: the lit particle's textures (production_particle_lit.frag's), and the pass's particle
                       block in fragment slot 1 when another batch's block took it since. */
                    btbh_gpu_scene_bind_lit_particle(
                        run_verify,
                        pass,
                        runtime,
                        texture,
                        normal_texture,
                        emission_texture,
                        scene_distance_ready,
                        radiance_cascades_lit,
                        shadows_active,
                        omni_shadows_active,
                        setup.particle_shadow_active,
                        view_frame->sun_map,
                        view_frame->spot_map,
                        view_frame->transmittance_map
                    );
                    if (!particle_frame_pushed) {
                        btbh_gpu_scene_run_push_fragment_uniforms(
                            run_verify,
                            command,
                            1u,
                            &particle_frame,
                            (uint32_t)sizeof(particle_frame),
                            &btbh_gpu_scene_run_particle_layout
                        );
                        if (!run_check) {
                            particle_frame_pushed = true;
                        }
                    }
                } else {
                    SDL_GPUTextureSamplerBinding samplers[16];
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
                    /* (RFC 0014: the view's sun and spot maps, view 0's or its own.) */
                    samplers[5].texture = view_frame->sun_map;
                    samplers[5].sampler =
                        runtime->directional_shadow_sampler;
                    samplers[6].texture =
                        runtime->directional_shadow_textures[1];
                    samplers[6].sampler =
                        runtime->directional_shadow_sampler;
                    samplers[7].texture = view_frame->spot_map;
                    samplers[7].sampler =
                        runtime->directional_shadow_sampler;
                    /* The primary map again, through the comparison sampler (the soft sun shadow filter's taps). */
                    samplers[8].texture = view_frame->sun_map;
                    samplers[8].sampler =
                        runtime->directional_shadow_compare_sampler;
                    if (omni_shadows_active) {
                        samplers[9].texture = runtime->omni_shadow_texture;
                        samplers[9].sampler =
                            runtime->directional_shadow_sampler;
                    }
                    /* Each variant's textures, then an SDF terrain's dig mask (RFC 0007), then the world detail's three
                       (RFC 0008) and the world-detail table's array (RFC 0009; these four not in the fast unlit shader),
                       with their placement in the fragment uniform slot 1, and a table's entries in slot 2. */
                    const bool batch_fast_unlit = btbh_gpu_scene_batch_fast_unlit(
                        batch,
                        radiance_cascades_active,
                        cascades_fast_unlit
                    );
                    const uint32_t material_sampler_count =
                        batch_fast_unlit
                            ? 2u
                            : (shadows_active
                                ? (omni_shadows_active ? 10u : 9u)
                                : 5u);
                    const BTBHGPUSdfTerrainMaskBinding *dig = &runtime->dig_bindings[batch_index];
                    samplers[material_sampler_count] = btbh_gpu_scene_dig_sampler(runtime, dig);
                    uint32_t bound_sampler_count = material_sampler_count + 1u;
                    BTBHGPUSceneBatchExtensionUniforms extension_uniforms;
                    btbh_gpu_scene_dig_uniforms(dig, &extension_uniforms.dig_mask);
                    btbh_gpu_world_detail_pack(
                        batch,
                        extension_uniforms.world_detail_tiles,
                        extension_uniforms.world_detail_controls,
                        extension_uniforms.world_detail_mode
                    );
                    if (!batch_fast_unlit) {
                        btbh_gpu_scene_world_detail_samplers(runtime, batch, &samplers[bound_sampler_count]);
                        bound_sampler_count += 3u;
                        const BTBHGPUTexture *maps = btbh_gpu_world_detail_table_active(batch)
                            ? batch->world_detail_table->map_layers
                            : runtime->fallback_array_texture;
                        samplers[bound_sampler_count].texture = maps->texture;
                        samplers[bound_sampler_count].sampler = maps->sampler;
                        bound_sampler_count += 1u;
                        if (shadows_active) {
                            /* RFC 0013 phase 2: the particle transmittance map (particle_shadow 2), or the fallback,
                               never sampled, in the shadowed variants' last slot. */
                            const bool ground_reads_map =
                                setup.particle_shadow_active &&
                                (setup.particle_shadow & BTBH_GPU_SCENE_PARTICLE_SHADOW_READERS) ==
                                    BTBH_GPU_SCENE_PARTICLE_SHADOW_GROUND;
                            samplers[bound_sampler_count].texture = ground_reads_map
                                ? view_frame->transmittance_map
                                : runtime->fallback_texture->texture;
                            samplers[bound_sampler_count].sampler = ground_reads_map
                                ? runtime->directional_shadow_sampler
                                : runtime->fallback_texture->sampler;
                            bound_sampler_count += 1u;
                        }
                        if (btbh_gpu_world_detail_table_active(batch) && batch->world_detail_table != pushed_table) {
                            /* Only a table batch pushes the block, when its table isn't the one pushed last; the others
                               never read the slot. */
                            BTBHGPUWorldDetailTableUniforms table_uniforms;
                            btbh_gpu_world_detail_table_pack(batch->world_detail_table, &table_uniforms);
                            btbh_gpu_scene_run_push_fragment_uniforms(
                                run_verify,
                                command,
                                2u,
                                &table_uniforms,
                                (uint32_t)sizeof(table_uniforms),
                                &btbh_gpu_scene_run_table_layout
                            );
                            if (!run_check) {
                                pushed_table = batch->world_detail_table;
                            }
                        }
                    }
                    btbh_gpu_scene_run_push_fragment_uniforms(
                        run_verify,
                        command,
                        1u,
                        &extension_uniforms,
                        batch_fast_unlit
                            ? (uint32_t)sizeof(extension_uniforms.dig_mask)
                            : (uint32_t)sizeof(extension_uniforms),
                        &btbh_gpu_scene_run_extension_layout
                    );
                    btbh_gpu_scene_run_bind_fragment_samplers(
                        run_verify,
                        pass,
                        samplers,
                        bound_sampler_count
                    );
                    if (!run_check) {
                        particle_frame_pushed = false;
                    }
                }
                if (run_check) {
                    run_verify->checking = false;
                } else {
                    run_batch = animated ? NULL : batch;
                }
            }
            const BTBHGPUSceneGeometryDraw geometry =
                btbh_gpu_scene_geometry_draw(batch, geometry_index);
            SDL_GPUBuffer *vertex_buffer = geometry.vertex_buffer;
            SDL_GPUBuffer *index_buffer = geometry.index_buffer;
            const uint32_t vertex_count = geometry.vertex_count;
            const uint32_t index_count = geometry.index_count;
            const uint32_t first_index = geometry.first_index;
            const int32_t vertex_base = geometry.vertex_base;
            if (vertex_buffer != bound_vertex_buffer) {
                SDL_GPUBufferBinding vertex_binding;
                SDL_zero(vertex_binding);
                vertex_binding.buffer = vertex_buffer;
                SDL_BindGPUVertexBuffers(
                    pass,
                    0u,
                    &vertex_binding,
                    1u
                );
                bound_vertex_buffer = vertex_buffer;
            }
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
            view_frame->draw_count += 1u;
            if (!batch_drawn_in_view) {
                view_frame->instance_count += batch->instance_count;
                batch_drawn_in_view = true;
            }
        }
        }
        if (!phase_ordered && geometry_cursor != prepared_geometry_count) {
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
        view_frame->encode_nanoseconds += SDL_GetTicksNS() - view_started_ticks;
        }
        SDL_EndGPURenderPass(pass);
        geometry_encode_nanoseconds +=
            SDL_GetTicksNS() - geometry_phase_started_ticks;
        if (!btbh_gpu_scene_cost_split(
                window,
                &command,
                view_cost_split
                    ? (transparent_phase
                        ? BTBH_GPU_COST_GROUP_VIEW1_TRANSPARENT
                        : BTBH_GPU_COST_GROUP_VIEW1_MAIN)
                    : (transparent_phase
                        ? BTBH_GPU_COST_GROUP_TRANSPARENT
                        : BTBH_GPU_COST_GROUP_MAIN),
                report
            )) {
            free(bone_offsets);
            return report->result;
        }

        if (!transparent_phase && hbao_before_cascades) {
            /* BTBH_GPU_SCENE_POST_ADDITIVE_CASCADES_KEEP_HBAO with the HBAO's contact mode (since 0.12.0): laid over
               the opaque scene before the cascades add their bounce light to it, which it then leaves as it is; the
               emission it keeps rides the ambient target (unlit_enabled bit 10). */
            if (!btbh_gpu_scene_cost_step(window, &command, BTBH_GPU_COST_STEP_AO, report)) {
                free(bone_offsets);
                return report->result;
            }
            if (!btbh_gpu_scene_encode_hbao(
                    command,
                    runtime,
                    post,
                    inverse_view_projection,
                    projection_scale_y,
                    BTBH_GPU_SCENE_HBAO_CONTACT_MODE,
                    runtime->ambient_texture,
                    radiance_cascades_post_input,
                    scissor
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
                        bone_buffer,
                        runs_enabled,
                        &telemetry->rc_source_run_draw_count,
                        run_verify
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
                    scene_ambient_targets,
                    runtime->normal_texture,
                    runtime->depth_texture,
                    hero_omni_shadow_ready
                        ? runtime->omni_shadow_texture
                        : NULL,
                    runtime->retained_omni_shadow_position_range,
                    lighting->spot_shadow_near,
                    lighting->spot_shadow_softness,
                    &composed_texture,
                    render_scissor
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
                radiance_cascades_lit = radiance_cascades_active;
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
    if (setup.radiance_cascades_held_for_views) {
        /* RFC 0014: asked for, and not run in a clipped frame of views. */
        report->radiance_cascades.requested_tier = post->radiance_cascades.tier;
        report->radiance_cascades.active_tier = BTBH_GPU_SCENE_RC_OFF;
        report->radiance_cascades.pending_tier = BTBH_GPU_SCENE_RC_OFF;
        report->radiance_cascades.fallback_reason = BTBH_GPU_SCENE_RC_FALLBACK_VIEWS;
        report->radiance_cascades.internal_width = 0u;
        report->radiance_cascades.internal_height = 0u;
        report->radiance_cascades.cpu_encode_milliseconds = 0.0;
        SDL_zero(report->radiance_cascades.pass_telemetry);
    }

    const uint64_t post_started_ticks = SDL_GetTicksNS();
    SDL_GPUTexture *post_input = radiance_cascades_post_input;
    bool fullscreen_ok = true;
    /* (The cost log's pass frames take a step a pass, or a few (bloom's levels, SMAA's three), from here to FSR 1 or
       Lanczos: btbh_gpu_cost_step, which may replace `command`.) */
    if (fullscreen_ok && hdr_composite_active) {
        SDL_GPUTexture *bloom_texture = NULL;
        if (bloom_active) {
            fullscreen_ok = btbh_gpu_cost_step(&command, BTBH_GPU_COST_STEP_BLOOM, NULL);
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
                    1u,
                    btbh_gpu_scene_scissor_at(
                        scissor,
                        runtime->bloom_level_width[level],
                        runtime->bloom_level_height[level],
                        &level_scissor_rect
                    )
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
                    2u,
                    btbh_gpu_scene_scissor_at(
                        scissor,
                        runtime->bloom_level_width[level],
                        runtime->bloom_level_height[level],
                        &level_scissor_rect
                    )
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
        fullscreen_ok = fullscreen_ok && btbh_gpu_cost_step(&command, BTBH_GPU_COST_STEP_COMPOSITE, NULL);
        /* The LUT stage always writes post_ping, so the composite uses pong. */
        if (fullscreen_ok && views_clipped) {
            /* RFC 0014: each view's fog (its distance and the world-edge fade) at its own depth, from its own camera;
               bloom and the vignette are the frame's. */
            BTBHGPUSceneCompositeUniforms view_composite_uniforms[BTBH_GPU_SCENE_MAX_VIEWS];
            for (uint32_t view_index = 0u; view_index < view_count; ++view_index) {
                view_composite_uniforms[view_index] = composite_uniforms;
                (void)memcpy(
                    view_composite_uniforms[view_index].inverse_view_projection,
                    view_frames[view_index].inverse_view_projection,
                    sizeof(view_composite_uniforms[view_index].inverse_view_projection)
                );
            }
            fullscreen_ok = btbh_gpu_scene_render_fullscreen_views(
                command,
                "BTBH HDR Composite Views",
                runtime->view_pipelines[BTBH_GPU_SCENE_VIEW_PIPELINE_COMPOSITE],
                runtime->post_pong_texture,
                false,
                view_composite_uniforms,
                (uint32_t)sizeof(view_composite_uniforms[0]),
                sizeof(view_composite_uniforms[0]),
                composite_samplers,
                3u,
                view_frames,
                view_count
            );
        } else if (fullscreen_ok) {
            fullscreen_ok = btbh_gpu_scene_render_fullscreen(
                command,
                "BTBH HDR Composite",
                runtime->hdr_composite_pipeline,
                runtime->post_pong_texture,
                &composite_uniforms,
                (uint32_t)sizeof(composite_uniforms),
                composite_samplers,
                3u,
                render_scissor
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
        fullscreen_ok = btbh_gpu_cost_step(&command, BTBH_GPU_COST_STEP_LUT, NULL) &&
            btbh_gpu_scene_render_fullscreen(
            command,
            "BTBH Post LUT",
            runtime->post_lut_pipeline,
            runtime->post_ping_texture,
            &post_uniforms,
            (uint32_t)sizeof(post_uniforms),
            lut_samplers,
            2u,
            render_scissor
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
        fullscreen_ok = btbh_gpu_cost_step(&command, BTBH_GPU_COST_STEP_CAS, NULL) &&
            btbh_gpu_scene_render_fullscreen(
            command,
            "BTBH Post CAS",
            runtime->post_cas_pipeline,
            cas_target,
            &post_uniforms,
            (uint32_t)sizeof(post_uniforms),
            &cas_sampler,
            1u,
            render_scissor
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
        fullscreen_ok = btbh_gpu_cost_step(&command, BTBH_GPU_COST_STEP_OUTPUT, NULL) &&
            btbh_gpu_scene_render_fullscreen(
            command,
            "BTBH Post Tilt Shift + Output",
            runtime->post_tilt_shift_pipeline,
            tilt_shift_target,
            &post_uniforms,
            (uint32_t)sizeof(post_uniforms),
            &tilt_shift_sampler,
            1u,
            render_scissor
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
        fullscreen_ok = btbh_gpu_cost_step(&command, BTBH_GPU_COST_STEP_OUTPUT, NULL) &&
            btbh_gpu_scene_render_fullscreen(
            command,
            "BTBH Production Output",
            runtime->post_pipeline,
            post_target,
            &post_uniforms,
            (uint32_t)sizeof(post_uniforms),
            &post_sampler,
            1u,
            render_scissor
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
        fullscreen_ok = btbh_gpu_cost_step(&command, BTBH_GPU_COST_STEP_FXAA, NULL) &&
            btbh_gpu_scene_render_fullscreen(
            command,
            "BTBH FXAA",
            runtime->fxaa_pipeline,
            runtime->final_color_texture,
            &fxaa_uniforms,
            (uint32_t)sizeof(fxaa_uniforms),
            &fxaa_sampler,
            1u,
            render_scissor
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
        fullscreen_ok = btbh_gpu_cost_step(&command, BTBH_GPU_COST_STEP_SMAA, NULL) &&
            btbh_gpu_scene_render_fullscreen(
            command,
            "BTBH SMAA Edge",
            runtime->smaa_edge_pipelines[quality_index],
            runtime->smaa_edges_texture,
            &smaa_uniforms,
            (uint32_t)sizeof(smaa_uniforms),
            &edge_sampler,
            1u,
            render_scissor
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
                3u,
                render_scissor
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
                2u,
                render_scissor
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
        /* (The cost log's pass frames take a step a dispatch: the encode may replace `command`.) */
        bool fsr3_encoded =
            drawable_width == runtime->fsr3.output_width &&
            drawable_height == runtime->fsr3.output_height &&
            btbh_gpu_fsr3_encode(&command, &runtime->fsr3, &fsr3_inputs);
        if (command == NULL) {
            /* The cost log's split inside the upscaler failed: the frame fails below. */
            fsr3_encoded = false;
            fullscreen_ok = false;
        } else if (!fsr3_encoded) {
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
        if (fsr3_encoded && !btbh_gpu_cost_step(&command, BTBH_GPU_COST_STEP_FSR3_RCAS, NULL)) {
            fsr3_encoded = false;
            fullscreen_ok = false;
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
                1u,
                output_scissor
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
        fullscreen_ok = btbh_gpu_cost_step(&command, BTBH_GPU_COST_STEP_FSR1_EASU, NULL) &&
            btbh_gpu_scene_render_fullscreen(
            command,
            "BTBH FSR1 EASU",
            runtime->upscale_fsr_easu_pipeline,
            runtime->upscaled_color_texture,
            &easu_uniforms,
            (uint32_t)sizeof(easu_uniforms),
            &easu_sampler,
            1u,
            output_scissor
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
        fullscreen_ok = fullscreen_ok &&
            btbh_gpu_cost_step(&command, BTBH_GPU_COST_STEP_FSR1_RCAS, NULL) &&
            btbh_gpu_scene_render_fullscreen(
            command,
            "BTBH FSR1 RCAS",
            runtime->upscale_fsr_rcas_pipeline,
            runtime->sharpened_color_texture,
            &rcas_uniforms,
            (uint32_t)sizeof(rcas_uniforms),
            &rcas_sampler,
            1u,
            output_scissor
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
        fullscreen_ok = btbh_gpu_cost_step(&command, BTBH_GPU_COST_STEP_LANCZOS, NULL) &&
            btbh_gpu_scene_render_fullscreen(
            command,
            "BTBH Lanczos Upscale",
            runtime->upscale_lanczos_pipeline,
            runtime->upscaled_color_texture,
            &upscale_uniforms,
            (uint32_t)sizeof(upscale_uniforms),
            &upscale_sampler,
            1u,
            output_scissor
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
    /* RFC 0014: a clipped frame of views, drawn, leaves the cascades' history behind (the next frame that runs them
       starts a new one); a refused one, or a prewarm, keeps it. */
    if (setup.radiance_cascades_held_for_views) {
        btbh_gpu_radiance_cascades_runtime_invalidate_history(&runtime->radiance_cascades);
    }
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
    /* The queued uploads this scene copied are done with (a scene for the window waits for the canvas; one for a
       target is submitted by the caller). */
    btbh_gpu_upload_queue_commit(window, output_width == 0u);
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
    /* What ran: FSR 3 only while it is available (a failed encode turns it off from then on). */
    report->upscale_mode =
        upscale_mode == (uint8_t)BTBH_GPU_SCENE_UPSCALE_FSR3 &&
            runtime->fsr3.unavailable
            ? (uint8_t)BTBH_GPU_SCENE_UPSCALE_FSR1
            : upscale_mode;
    if (view_reports != NULL) {
        /* RFC 0014: what each view drew, and the maps it read (UINT32_MAX without the shadow). */
        for (uint32_t view_index = 0u; view_index < view_count; ++view_index) {
            const BTBHGPUSceneViewFrame *frame = &view_frames[view_index];
            BTBHGPUSceneViewReport *view_report = &view_reports[view_index];
            view_report->primitive_draw_count = frame->draw_count;
            view_report->instance_count = frame->instance_count;
            view_report->sun_map = shadow_enabled[0] != 0u
                ? (frame->sun_slot != 0u ? view_index : 0u)
                : UINT32_MAX;
            view_report->spot_map = shadow_enabled[2] != 0u
                ? (frame->spot_slot != 0u ? view_index : 0u)
                : UINT32_MAX;
            view_report->cpu_opaque_order_milliseconds = frame->order_milliseconds;
            view_report->cpu_geometry_encode_milliseconds = (double)frame->encode_nanoseconds / 1000000.0;
        }
    }
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
    BTBHGPUSceneView camera_view;
    const bool camera_given =
        btbh_gpu_scene_camera_view(view_projection, camera_right, camera_up, camera_position, &camera_view);
    BTBHGPULazyCreationScope lazy_scope;
    btbh_gpu_lazy_creation_begin(window, BTBH_GPU_LAZY_CREATION_CALL_SCENE_PRESENT, &lazy_scope);
    const BTBHGPUResult result = btbh_gpu_window_scene_present_instances_internal(
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
        camera_given ? &camera_view : NULL,
        1u,
        false,
        NULL,
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
        0u,
        0u,
        NULL,
        report
    );
    btbh_gpu_lazy_creation_end(&lazy_scope);
    return result;
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
    BTBHGPUSceneView camera_view;
    const bool camera_given =
        btbh_gpu_scene_camera_view(view_projection, camera_right, camera_up, camera_position, &camera_view);
    BTBHGPULazyCreationScope lazy_scope;
    btbh_gpu_lazy_creation_begin(window, BTBH_GPU_LAZY_CREATION_CALL_SCENE_PRESENT, &lazy_scope);
    const BTBHGPUResult result = btbh_gpu_window_scene_present_instances_internal(
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
        camera_given ? &camera_view : NULL,
        1u,
        false,
        NULL,
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
        0u,
        0u,
        NULL,
        report
    );
    btbh_gpu_lazy_creation_end(&lazy_scope);
    return result;
}

/* Makes what the next scene presents would make the first time they draw these batches with these settings,
   through the present's own steps (btbh_gpu_scene_prepare_frame, btbh_gpu_scene_ensure_model_pipelines,
   btbh_gpu_scene_ensure_frame_targets), and draws nothing: for output_width x output_height (0: the window's
   drawable size, as the present's own argument). btbh_gpu_window_scene_prewarm runs it for the window's frames,
   btbh_gpu_window_scene_prewarm_render_to_target in the window's aside runtime with a render's post settings, and
   (RFC 0014) btbh_gpu_window_scene_prewarm_views with views (NULL for the others; checked): a clipped frame's
   settings, its pipelines and a second view's own maps. */
static BTBHGPUResult btbh_gpu_scene_prewarm_internal(
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
    const BTBHGPUSceneLighting *lighting,
    const BTBHGPUScenePost *post,
    const BTBHGPUSceneGeometryBatch *batches,
    uint32_t batch_count,
    uint32_t output_width,
    uint32_t output_height,
    const BTBHGPUSceneView *views,
    uint32_t view_count,
    BTBHGPUSceneReport *report
) {
    if (report == NULL) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    (void)memset(report, 0, sizeof(*report));
    const bool views_clipped = views != NULL && btbh_gpu_scene_views_clipped(views, view_count);
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
        !btbh_gpu_scene_lighting_valid(lighting) ||
        !btbh_gpu_scene_post_valid(post) ||
        batches == NULL ||
        batch_count == 0u ||
        btbh_gpu_scene_shader_format(shader_format) ==
            SDL_GPU_SHADERFORMAT_INVALID ||
        !btbh_gpu_scene_validate_batches(
            window,
            0u,
            batches,
            NULL,
            NULL,
            batch_count,
            false
        )) {
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            "Invalid GPU scene prewarm"
        );
        return report->result;
    }
    if (window->pending_scene_ready || window->pending_command != NULL) {
        /* The frame's scene waits for its canvas: its targets must stay as they are until then. */
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            "GPU scene prewarm while the frame's scene waits for its canvas"
        );
        return report->result;
    }
    if (!btbh_gpu_scene_validate_world_details(window, batches, NULL, NULL, 0u, batch_count, report)) {
        return report->result;
    }
    BTBHGPUSceneFrameSetup setup;
    if (!btbh_gpu_scene_prepare_frame(
            window,
            fast_shadow_vertex_shader_path,
            fast_shadow_fragment_shader_path,
            shadow_vertex_shader_path,
            animated_shadow_vertex_shader_path,
            shadow_fragment_shader_path,
            fragment_shader_path,
            post_vertex_shader_path,
            post_fragment_shader_path,
            shader_format,
            render_width,
            render_height,
            output_width,
            views_clipped,
            lighting,
            post,
            batches,
            NULL,
            batch_count,
            &setup,
            report
        )) {
        return report->result;
    }
    /* Which geometries are skinned, as the present finds them (the poses are sampled and dropped; no temporal
       palette is kept). */
    uint32_t *bone_offsets = NULL;
    uint32_t prepared_geometry_count = 0u;
    float *bone_matrices = NULL;
    uint32_t bone_count = 0u;
    BTBHGPUSceneTelemetryReport telemetry;
    SDL_zero(telemetry);
    if (!btbh_gpu_scene_prepare_bones(
            batches,
            batch_count,
            &bone_offsets,
            &prepared_geometry_count,
            &bone_matrices,
            &bone_count,
            false,
            &telemetry,
            NULL,
            NULL
        )) {
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            "Could not prepare GPU animation pose"
        );
        return report->result;
    }
    free(bone_matrices);
    if (!btbh_gpu_scene_check_scene_shader(
            window->scene_runtime,
            batches,
            batch_count,
            fragment_shader_path,
            unshadowed_fragment_shader_path,
            shader_format,
            setup.shadows_active,
            setup.omni_shadows_active,
            setup.radiance_cascades_active,
            report
        ) ||
        !btbh_gpu_scene_check_rc_shader(
            window->scene_runtime,
            post,
            post_fragment_shader_path,
            shader_format,
            setup.radiance_cascades_active,
            report
        ) ||
        !btbh_gpu_scene_check_uv_rect_shaders(
            window->scene_runtime,
            batches,
            batch_count,
            vertex_shader_path,
            post_vertex_shader_path,
            shader_format,
            setup.radiance_cascades_active,
            report
        ) ||
        !btbh_gpu_scene_check_particle_shadow_shaders(
            window->scene_runtime,
            batches,
            batch_count,
            fragment_shader_path,
            unshadowed_fragment_shader_path,
            shader_format,
            setup.particle_shadow_active,
            setup.particle_shadow,
            setup.shadows_active,
            setup.omni_shadows_active,
            setup.radiance_cascades_active,
            report
        ) ||
        !btbh_gpu_scene_ensure_model_pipelines(
            window,
            vertex_shader_path,
            animated_vertex_shader_path,
            fragment_shader_path,
            unshadowed_fragment_shader_path,
            shader_format,
            &setup,
            batches,
            batch_count,
            bone_offsets,
            prepared_geometry_count,
            NULL,
            report
        )) {
        free(bone_offsets);
        return report->result;
    }
    if (views_clipped) {
        /* RFC 0014: a clipped frame's shaders, pipelines and the second view's own maps, as the present decides them. */
        bool own_sun = false;
        bool own_spot = false;
        btbh_gpu_scene_view_own_maps(views, view_count, lighting, setup.shadow_enabled, &own_sun, &own_spot);
        if (!btbh_gpu_scene_check_view_shaders(
                window->scene_runtime,
                batches,
                batch_count,
                bone_offsets,
                vertex_shader_path,
                animated_vertex_shader_path,
                post_vertex_shader_path,
                shader_format,
                report
            ) ||
            !btbh_gpu_scene_ensure_view_pipelines(
                window,
                &setup,
                post_vertex_shader_path,
                post_fragment_shader_path,
                shader_format,
                NULL,
                report
            ) ||
            !btbh_gpu_scene_ensure_view_shadow_maps(
                window,
                own_sun,
                own_spot,
                setup.particle_shadow_active,
                setup.particle_shadow,
                report
            )) {
            free(bone_offsets);
            return report->result;
        }
    }
    free(bone_offsets);
    uint32_t drawable_width = 0u;
    uint32_t drawable_height = 0u;
    bool scene_distance_ready = false;
    if (!btbh_gpu_scene_ensure_frame_targets(
            window,
            &setup,
            post,
            post_vertex_shader_path,
            post_fragment_shader_path,
            shader_format,
            render_width,
            render_height,
            output_width,
            output_height,
            &drawable_width,
            &drawable_height,
            &scene_distance_ready,
            report
        )) {
        if (report->result == BTBH_GPU_OK) {
            btbh_gpu_scene_error(
                report,
                BTBH_GPU_WINDOW_FAILURE,
                SDL_GetError()
            );
        }
        return report->result;
    }
    /* Since 0.13.0, on Vulkan: when the core submitted command buffers since the device's last presented frame (a
       level's uploads, renders into targets, the prewarm's own uploads), wait for the GPU once, here. SDL 3.4 gives every
       command buffer a descriptor-set cache from a pool (last in, first out) and takes the caches back only in a submit
       that presents, or in a wait. A load's uploads emptied the pool, so the first frame after it drew with a new, empty
       cache and made a descriptor pool for each layout it bound, 15 for a scene: about 0.5 ms, and 24 ms once when the
       driver stalled in one. The wait gives every cache back, those the frames filled on top. Without such submissions
       nothing waits. */
    if (btbh_gpu_unpresented_submit_count() > 0u) {
        const char *driver = SDL_GetGPUDeviceDriver(window->device->gpu);
        if (driver != NULL && strcmp(driver, "vulkan") == 0) {
            (void)SDL_WaitForGPUIdle(window->device->gpu);
            btbh_gpu_note_command_buffers_settled();
        }
    }
    report->result = BTBH_GPU_OK;
    report->drawable_width = drawable_width;
    report->drawable_height = drawable_height;
    report->batch_count = batch_count;
    report->prepared_geometry_count = prepared_geometry_count;
    report->upscale_mode = setup.upscale_mode;
    report->message[0] = '\0';
    return report->result;
}

BTBHGPUResult btbh_gpu_window_scene_prewarm(
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
    const BTBHGPUSceneLighting *lighting,
    const BTBHGPUScenePost *post,
    const BTBHGPUSceneGeometryBatch *batches,
    uint32_t batch_count,
    BTBHGPUSceneReport *report
) {
    return btbh_gpu_scene_prewarm_internal(
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
        lighting,
        post,
        batches,
        batch_count,
        0u,
        0u,
        NULL,
        0u,
        report
    );
}

/* RFC 0014 (since 0.16.0): the present with views (btbh_gpu.h). */
BTBHGPUResult btbh_gpu_window_scene_present_views(
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
    const BTBHGPUSceneView *views,
    uint32_t view_count,
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
    BTBHGPUSceneViewReport *view_reports,
    BTBHGPUSceneReport *report
) {
    BTBHGPULazyCreationScope lazy_scope;
    btbh_gpu_lazy_creation_begin(window, BTBH_GPU_LAZY_CREATION_CALL_SCENE_PRESENT, &lazy_scope);
    const BTBHGPUResult result = btbh_gpu_window_scene_present_instances_internal(
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
        views,
        view_count,
        true,
        view_reports,
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
        0u,
        0u,
        NULL,
        report
    );
    btbh_gpu_lazy_creation_end(&lazy_scope);
    return result;
}

/* RFC 0014 (since 0.16.0): the prewarm for views (btbh_gpu.h): the views refused as the present refuses them, then the
   window's prewarm with them. */
BTBHGPUResult btbh_gpu_window_scene_prewarm_views(
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
    const BTBHGPUSceneView *views,
    uint32_t view_count,
    const BTBHGPUSceneLighting *lighting,
    const BTBHGPUScenePost *post,
    const BTBHGPUSceneGeometryBatch *batches,
    uint32_t batch_count,
    BTBHGPUSceneReport *report
) {
    if (report == NULL) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    (void)memset(report, 0, sizeof(*report));
    if (!btbh_gpu_scene_views_valid(views, view_count, 0u, report)) {
        return report->result;
    }
    return btbh_gpu_scene_prewarm_internal(
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
        lighting,
        post,
        batches,
        batch_count,
        0u,
        0u,
        views,
        view_count,
        report
    );
}

/* A target btbh_gpu_window_scene_render_to_target can render into (and its prewarm make ready): a 2D render
   target of this window. */
static bool btbh_gpu_scene_render_target_usable(
    const BTBHGPUWindow *window,
    const BTBHGPUTexture *target
) {
    return target != NULL &&
        target->owner == window &&
        target->texture != NULL &&
        target->color_target &&
        !btbh_gpu_texture_is_array(target) &&
        !btbh_gpu_texture_is_heightfield(target) &&
        target->width != 0u &&
        target->height != 0u;
}

/* The post settings a scene rendered into `target` runs with (btbh_gpu_window_scene_render_to_target, and its
   prewarm). Never temporal: FSR 3 runs as FSR 1, and the instance motions and the reset don't apply. The upscalers
   only enlarge: a render at the target's size or bigger is scaled bilinearly (a render twice the target's size is
   halved exactly), with no sharpening. */
static BTBHGPUScenePost btbh_gpu_scene_render_target_post(
    const BTBHGPUScenePost *post,
    uint32_t render_width,
    uint32_t render_height,
    const BTBHGPUTexture *target
) {
    BTBHGPUScenePost target_post = *post;
    if (target_post.upscale_mode == BTBH_GPU_SCENE_UPSCALE_FSR3) {
        target_post.upscale_mode = BTBH_GPU_SCENE_UPSCALE_FSR1;
    }
    if (render_width >= target->width && render_height >= target->height) {
        target_post.upscale_mode = BTBH_GPU_SCENE_UPSCALE_LINEAR;
    }
    target_post.temporal_instance_motions = NULL;
    target_post.temporal_instance_motion_count = 0u;
    target_post.temporal_reset = 0u;
    return target_post;
}

/* What a scene rendered into a target swaps out of the window while it runs (btbh_gpu_scene_enter_aside). */
typedef struct BTBHGPUSceneAsideSwap {
    BTBHGPUSceneRuntime *window_runtime;
    BTBHGPUSceneRuntime **slot;
    BTBHGPUSceneTelemetryReport window_telemetry;
    bool window_radiance_cascades_telemetry;
    /* The cost log's frame state (a failed render inside the aside cancels, which clears it). */
    SDL_GPUDevice *window_cost_split_device;
    bool window_cost_pass_frame;
    int window_cost_current_step;
} BTBHGPUSceneAsideSwap;

/* The runtime in `slot` in place of the window's own (btbh_gpu_window_scene_render_to_target and its prewarm: the
   window's aside runtime; since 0.15.0 a kept target view's, its own): its own scene targets, shadow maps, pipelines
   and (never temporal) state, so that the window's are neither read nor written. The cost log's splits are off
   meanwhile. btbh_gpu_scene_leave_aside puts the window's back. */
static void btbh_gpu_scene_enter_aside(
    BTBHGPUWindow *window,
    BTBHGPUSceneRuntime **slot,
    BTBHGPUSceneAsideSwap *swap
) {
    swap->window_runtime = window->scene_runtime;
    swap->slot = slot;
    swap->window_telemetry = window->scene_telemetry;
    swap->window_radiance_cascades_telemetry = window->radiance_cascades_telemetry_enabled;
    swap->window_cost_split_device = btbh_gpu_cost_split_device;
    swap->window_cost_pass_frame = btbh_gpu_cost_pass_frame;
    swap->window_cost_current_step = btbh_gpu_cost_current_step;
    btbh_gpu_cost_abandon_frame();
    window->scene_runtime = *slot;
}

static void btbh_gpu_scene_leave_aside(BTBHGPUWindow *window, const BTBHGPUSceneAsideSwap *swap) {
    /* (The runtime may have been made, or released by a failure, in the call.) */
    *swap->slot = window->scene_runtime;
    window->scene_runtime = swap->window_runtime;
    window->scene_telemetry = swap->window_telemetry;
    window->radiance_cascades_telemetry_enabled = swap->window_radiance_cascades_telemetry;
    btbh_gpu_cost_split_device = swap->window_cost_split_device;
    btbh_gpu_cost_pass_frame = swap->window_cost_pass_frame;
    btbh_gpu_cost_current_step = swap->window_cost_current_step;
}

/* Where the runtime a render into a target uses lives (since 0.15.0): render_to_target's, for no view or view_id 0;
   else the target view's own (the view is valid). */
static BTBHGPUSceneRuntime **btbh_gpu_scene_target_view_slot(
    BTBHGPUWindow *window,
    const BTBHGPUSceneTargetView *view
) {
    return view == NULL || view->view_id == 0u
        ? &window->aside_scene_runtime
        : &window->target_view_runtimes[view->view_id - 1u];
}

/* Whether a render into `target` takes `view` (since 0.15.0): a view_id up to the capacity, the reserved fields zero
   and a rect (both sizes nonzero) inside the target. No view is fine. */
static bool btbh_gpu_scene_target_view_valid(
    const BTBHGPUSceneTargetView *view,
    const BTBHGPUTexture *target
) {
    if (view == NULL) {
        return true;
    }
    if (view->view_id > BTBH_GPU_SCENE_TARGET_VIEW_CAPACITY) {
        return false;
    }
    for (uint32_t index = 0u; index < 3u; ++index) {
        if (view->reserved[index] != 0u) {
            return false;
        }
    }
    if (view->scissor_width != 0u && view->scissor_height != 0u &&
        ((uint64_t)view->scissor_x + view->scissor_width > target->width ||
         (uint64_t)view->scissor_y + view->scissor_height > target->height)) {
        return false;
    }
    return true;
}

/* The scissor of a render at render_width x render_height into `target` with `view`'s rect (since 0.15.0): inactive
   without a rect, and for one that covers the whole target. mirrored (BTBHGPUScenePost.mirror_horizontal): the render
   size's passes run before the mirror's blit, so their rect is mirrored too, and lands on the view's once mirrored.
   The view is valid; a render size of 0 is refused later, before the rect is used. */
static BTBHGPUSceneScissor btbh_gpu_scene_target_scissor(
    const BTBHGPUSceneTargetView *view,
    uint32_t render_width,
    uint32_t render_height,
    const BTBHGPUTexture *target,
    bool mirrored
) {
    BTBHGPUSceneScissor scissor;
    SDL_zero(scissor);
    if (view == NULL ||
        view->scissor_width == 0u ||
        view->scissor_height == 0u ||
        (view->scissor_x == 0u &&
         view->scissor_y == 0u &&
         view->scissor_width == target->width &&
         view->scissor_height == target->height)) {
        return scissor;
    }
    scissor.active = true;
    scissor.output_width = target->width;
    scissor.output_height = target->height;
    scissor.render_width = render_width;
    scissor.render_height = render_height;
    scissor.output.x = (int)view->scissor_x;
    scissor.output.y = (int)view->scissor_y;
    scissor.output.w = (int)view->scissor_width;
    scissor.output.h = (int)view->scissor_height;
    if (render_width != 0u && render_height != 0u) {
        btbh_gpu_scene_scale_span(
            view->scissor_x,
            view->scissor_width,
            target->width,
            render_width,
            &scissor.render.x,
            &scissor.render.w
        );
        btbh_gpu_scene_scale_span(
            view->scissor_y,
            view->scissor_height,
            target->height,
            render_height,
            &scissor.render.y,
            &scissor.render.h
        );
        if (mirrored) {
            scissor.render.x = (int)render_width - (scissor.render.x + scissor.render.w);
        }
    }
    return scissor;
}

/* The 1x1 texture a target view's rect fills the target with outside it (since 0.15.0), in the target's format; NULL,
   with SDL's error set, when it can't be made. */
static SDL_GPUTexture *btbh_gpu_scene_create_target_clear_texture(
    SDL_GPUDevice *device,
    SDL_GPUTextureFormat format
) {
    SDL_GPUTextureCreateInfo info;
    SDL_zero(info);
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = format;
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = 1u;
    info.height = 1u;
    info.layer_count_or_depth = 1u;
    info.num_levels = 1u;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    return SDL_CreateGPUTexture(device, &info);
}

/* Whether `runtime` (NULL: not made yet) holds the fill texture for a target of `format`. */
static bool btbh_gpu_scene_has_target_clear_texture(
    const BTBHGPUSceneRuntime *runtime,
    SDL_GPUTextureFormat format
) {
    return runtime != NULL && runtime->target_clear_texture != NULL && runtime->target_clear_format == format;
}

/* Gives `runtime` its fill texture (made for `format`), releasing one of another format. */
static void btbh_gpu_scene_install_target_clear_texture(
    SDL_GPUDevice *device,
    BTBHGPUSceneRuntime *runtime,
    SDL_GPUTexture *texture,
    SDL_GPUTextureFormat format
) {
    if (runtime->target_clear_texture != NULL) {
        SDL_ReleaseGPUTexture(device, runtime->target_clear_texture);
    }
    runtime->target_clear_texture = texture;
    runtime->target_clear_format = format;
}

/* The fill texture in the window's current scene runtime (the prewarm's), made where missing or of another format.
   False, with the report set, when it can't be made. */
static bool btbh_gpu_scene_ensure_target_clear_texture(
    BTBHGPUWindow *window,
    SDL_GPUTextureFormat format,
    BTBHGPUSceneReport *report
) {
    if (btbh_gpu_scene_has_target_clear_texture(window->scene_runtime, format)) {
        return true;
    }
    SDL_GPUDevice *device = window->device->gpu;
    SDL_GPUTexture *texture = btbh_gpu_scene_create_target_clear_texture(device, format);
    if (texture == NULL) {
        btbh_gpu_scene_error(report, BTBH_GPU_DEVICE_FAILURE, SDL_GetError());
        return false;
    }
    btbh_gpu_scene_install_target_clear_texture(device, window->scene_runtime, texture, format);
    return true;
}

/* Fills `target` outside `rect` (in its pixels) with the clear colour (since 0.15.0), after the finished scene was
   blitted into it whole: the 1x1 texture is cleared to the colour and blitted, unfiltered, over each of the four bands
   round the rect. A clear stores the colour as the target's format does (an 8-bit UNORM target: these bytes; an sRGB
   one: their sRGB encoding). False when the clear's pass can't begin. */
static bool btbh_gpu_scene_fill_outside_rect(
    SDL_GPUCommandBuffer *command,
    BTBHGPUSceneRuntime *runtime,
    const BTBHGPUTexture *target,
    const SDL_Rect *rect,
    uint8_t clear_red,
    uint8_t clear_green,
    uint8_t clear_blue,
    uint8_t clear_alpha
) {
    SDL_GPUColorTargetInfo clear;
    SDL_zero(clear);
    clear.texture = runtime->target_clear_texture;
    clear.clear_color.r = (float)clear_red / 255.0f;
    clear.clear_color.g = (float)clear_green / 255.0f;
    clear.clear_color.b = (float)clear_blue / 255.0f;
    clear.clear_color.a = (float)clear_alpha / 255.0f;
    clear.load_op = SDL_GPU_LOADOP_CLEAR;
    clear.store_op = SDL_GPU_STOREOP_STORE;
    clear.cycle = true;
    SDL_InsertGPUDebugLabel(command, "BTBH Scene To Target Outside Rect");
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, &clear, 1u, NULL);
    if (pass == NULL) {
        return false;
    }
    SDL_EndGPURenderPass(pass);
    const uint32_t right = (uint32_t)rect->x + (uint32_t)rect->w;
    const uint32_t bottom = (uint32_t)rect->y + (uint32_t)rect->h;
    /* x, y, w, h: above the rect, below it (full width), left of it and right of it (its height). */
    const uint32_t bands[4][4] = {
        {0u, 0u, target->width, (uint32_t)rect->y},
        {0u, bottom, target->width, target->height - bottom},
        {0u, (uint32_t)rect->y, (uint32_t)rect->x, (uint32_t)rect->h},
        {right, (uint32_t)rect->y, target->width - right, (uint32_t)rect->h},
    };
    for (uint32_t band = 0u; band < 4u; ++band) {
        if (bands[band][2] == 0u || bands[band][3] == 0u) {
            continue;
        }
        SDL_GPUBlitInfo blit;
        SDL_zero(blit);
        blit.source.texture = runtime->target_clear_texture;
        blit.source.w = 1u;
        blit.source.h = 1u;
        blit.destination.texture = target->texture;
        blit.destination.x = bands[band][0];
        blit.destination.y = bands[band][1];
        blit.destination.w = bands[band][2];
        blit.destination.h = bands[band][3];
        blit.load_op = SDL_GPU_LOADOP_LOAD;
        blit.filter = SDL_GPU_FILTER_NEAREST;
        SDL_BlitGPUTexture(command, &blit);
    }
    return true;
}

/* btbh_gpu_window_scene_prewarm for a render into `target`: the prewarm's steps in the runtime that render uses (the
   window's aside runtime, or a kept target view's own since 0.15.0), with the render's post settings and the target's
   size, as the render runs the present's; and for a view with a rect, its fill texture. */
static BTBHGPUResult btbh_gpu_scene_prewarm_to_target_internal(
    BTBHGPUWindow *window,
    BTBHGPUTexture *target,
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
    const BTBHGPUSceneLighting *lighting,
    const BTBHGPUScenePost *post,
    const BTBHGPUSceneGeometryBatch *batches,
    uint32_t batch_count,
    const BTBHGPUSceneTargetView *view,
    BTBHGPUSceneReport *report
) {
    if (report == NULL) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    (void)memset(report, 0, sizeof(*report));
    if (window == NULL ||
        window->window == NULL ||
        window->device == NULL ||
        window->device->gpu == NULL ||
        !window->claimed ||
        post == NULL ||
        !btbh_gpu_scene_render_target_usable(window, target)) {
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            "Invalid GPU scene prewarm for a render target"
        );
        return report->result;
    }
    if (!btbh_gpu_scene_target_view_valid(view, target)) {
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            "Invalid GPU scene target view (its id, a reserved field or its rect)"
        );
        return report->result;
    }
    const BTBHGPUScenePost target_post =
        btbh_gpu_scene_render_target_post(post, render_width, render_height, target);
    const BTBHGPUSceneScissor scissor = btbh_gpu_scene_target_scissor(
        view,
        render_width,
        render_height,
        target,
        target_post.mirror_horizontal != 0u
    );
    BTBHGPUSceneAsideSwap swap;
    btbh_gpu_scene_enter_aside(window, btbh_gpu_scene_target_view_slot(window, view), &swap);
    BTBHGPUResult result = btbh_gpu_scene_prewarm_internal(
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
        lighting,
        &target_post,
        batches,
        batch_count,
        target->width,
        target->height,
        NULL,
        0u,
        report
    );
    if (result == BTBH_GPU_OK &&
        scissor.active &&
        !btbh_gpu_scene_ensure_target_clear_texture(window, target->format, report)) {
        result = report->result;
    }
    btbh_gpu_scene_leave_aside(window, &swap);
    return result;
}

BTBHGPUResult btbh_gpu_window_scene_prewarm_render_to_target(
    BTBHGPUWindow *window,
    BTBHGPUTexture *target,
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
    const BTBHGPUSceneLighting *lighting,
    const BTBHGPUScenePost *post,
    const BTBHGPUSceneGeometryBatch *batches,
    uint32_t batch_count,
    BTBHGPUSceneReport *report
) {
    return btbh_gpu_scene_prewarm_to_target_internal(
        window,
        target,
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
        lighting,
        post,
        batches,
        batch_count,
        NULL,
        report
    );
}

BTBHGPUResult btbh_gpu_window_scene_prewarm_render_to_target_view(
    BTBHGPUWindow *window,
    BTBHGPUTexture *target,
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
    const BTBHGPUSceneLighting *lighting,
    const BTBHGPUScenePost *post,
    const BTBHGPUSceneGeometryBatch *batches,
    uint32_t batch_count,
    const BTBHGPUSceneTargetView *view,
    BTBHGPUSceneReport *report
) {
    return btbh_gpu_scene_prewarm_to_target_internal(
        window,
        target,
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
        lighting,
        post,
        batches,
        batch_count,
        view,
        report
    );
}

/* A scene drawn into a render target apart from the window's frame (a unit's portrait). It renders
   with another runtime in place of the window's own: its own scene targets at this render size, its
   own shadow maps and its own (never temporal) state. So the window's runtime (the FSR 3 history, the
   previous view-projection and bone palettes, the retained shadow maps, the radiance cascades' history)
   is neither read nor written, and its targets keep the frame's size. Models, meshes, textures and
   instance buffers are the window's, shared. The finished scene, upscaled to the target's size (FSR 1
   or Lanczos as asked, else bilinear), is blitted into the target and submitted here.
   view (since 0.15.0, btbh_gpu_window_scene_render_to_target_view; NULL for render_to_target): view_id 0 renders with
   the window's aside runtime, every call afresh, as render_to_target; 1 and up with the view's own runtime, kept from
   call to call (its retained shadow maps, its shadow frame count and its cascades' history). Its rect limits the
   passes (BTBHGPUSceneScissor), and the target is filled with the clear colour outside it. */
static BTBHGPUResult btbh_gpu_scene_render_to_target_internal(
    BTBHGPUWindow *window,
    BTBHGPUTexture *target,
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
    const BTBHGPUSceneTargetView *view,
    BTBHGPUSceneReport *report
) {
    if (report == NULL) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    (void)memset(report, 0, sizeof(*report));
    if (window == NULL ||
        window->window == NULL ||
        window->device == NULL ||
        window->device->gpu == NULL ||
        !window->claimed ||
        post == NULL ||
        !btbh_gpu_scene_render_target_usable(window, target)) {
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            "GPU scene cannot render into that target"
        );
        return report->result;
    }
    if (!btbh_gpu_scene_target_view_valid(view, target)) {
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            "Invalid GPU scene target view (its id, a reserved field or its rect)"
        );
        return report->result;
    }
    if (window->pending_scene_ready || window->pending_command != NULL) {
        /* The frame's scene waits for its canvas; rendering now would drop it. */
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            "GPU scene to a target while the frame's scene waits for its canvas"
        );
        return report->result;
    }
    const BTBHGPUScenePost target_post =
        btbh_gpu_scene_render_target_post(post, render_width, render_height, target);
    const BTBHGPUSceneScissor scissor = btbh_gpu_scene_target_scissor(
        view,
        render_width,
        render_height,
        target,
        target_post.mirror_horizontal != 0u
    );
    const bool kept = view != NULL && view->view_id != 0u;
    BTBHGPULazyCreationScope lazy_scope;
    btbh_gpu_lazy_creation_begin(window, BTBH_GPU_LAZY_CREATION_CALL_RENDER_TO_TARGET, &lazy_scope);
    BTBHGPUSceneAsideSwap swap;
    btbh_gpu_scene_enter_aside(window, btbh_gpu_scene_target_view_slot(window, view), &swap);
    /* A rect's fill texture, made before the present: a present that returns OK has committed the window's upload
       queue (its copies are in the command buffer), so nothing after it may cancel that command buffer. Made apart
       from the runtime (which the present may make first), and handed to it once the present succeeded; released if
       the present refused or failed, so that a refused call leaves the window as it was. (What
       btbh_gpu_window_scene_prewarm_render_to_target_view makes for a view with a rect.) */
    SDL_GPUTexture *fresh_clear_texture = NULL;
    if (scissor.active && !btbh_gpu_scene_has_target_clear_texture(window->scene_runtime, target->format)) {
        btbh_gpu_lazy_creation_prewarm_step(true);
        fresh_clear_texture = btbh_gpu_scene_create_target_clear_texture(window->device->gpu, target->format);
        btbh_gpu_lazy_creation_prewarm_step(false);
        if (fresh_clear_texture == NULL) {
            btbh_gpu_scene_error(report, BTBH_GPU_DEVICE_FAILURE, SDL_GetError());
            btbh_gpu_scene_leave_aside(window, &swap);
            btbh_gpu_lazy_creation_end(&lazy_scope);
            return report->result;
        }
    }
    BTBHGPUSceneView camera_view;
    const bool camera_given =
        btbh_gpu_scene_camera_view(view_projection, camera_right, camera_up, camera_position, &camera_view);
    if (!kept) {
        /* Each such scene starts afresh: no shadow map kept from the last one (whatever the window's shadow
           update periods), no radiance-cascade history (two portraits are two pictures). A kept view's runtime
           keeps both, as the window's does from frame to frame. */
        btbh_gpu_window_scene_submission_finished(window, false);
    }
    /* (The cost log's line for this call counts the triangles drawn from here.) */
    const uint64_t cost_main_triangles = btbh_gpu_cost_pass_triangles(BTBH_GPU_COST_PASS_MAIN);
    const uint64_t cost_shadow_triangles = btbh_gpu_cost_pass_triangles(BTBH_GPU_COST_PASS_SHADOW);
    const BTBHGPUResult result =
        btbh_gpu_window_scene_present_instances_internal(
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
            camera_given ? &camera_view : NULL,
            1u,
            false,
            NULL,
            lighting,
            &target_post,
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
            target->width,
            target->height,
            scissor.active ? &scissor : NULL,
            report
        );
    if (result == BTBH_GPU_OK) {
        BTBHGPUSceneRuntime *runtime = window->scene_runtime;
        SDL_GPUCommandBuffer *command = window->pending_command;
        window->pending_command = NULL;
        window->pending_scene_ready = false;
        if (fresh_clear_texture != NULL) {
            btbh_gpu_scene_install_target_clear_texture(
                window->device->gpu,
                runtime,
                fresh_clear_texture,
                target->format
            );
            fresh_clear_texture = NULL;
        }
        /* The composite is at the target's size after an upscale, else at the render size: a blit
           scales it (bilinearly; a render twice the target's size is halved exactly). */
        SDL_GPUBlitInfo blit;
        SDL_zero(blit);
        blit.source.texture = runtime->composite_color_texture;
        blit.source.w = runtime->composite_width;
        blit.source.h = runtime->composite_height;
        blit.destination.texture = target->texture;
        blit.destination.w = target->width;
        blit.destination.h = target->height;
        blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
        blit.filter = runtime->composite_width == target->width &&
                runtime->composite_height == target->height
            ? SDL_GPU_FILTER_NEAREST
            : SDL_GPU_FILTER_LINEAR;
        SDL_InsertGPUDebugLabel(command, "BTBH Scene To Target");
        SDL_BlitGPUTexture(command, &blit);
        /* A target view's rect: outside it, the clear colour (the blit filtered across its border). If the fill's
           pass can't begin, what was recorded is still submitted, as a frame holding queued uploads is when the canvas
           drops it (btbh_gpu_window_finish_pending_frame): the queue was committed with this command buffer. The call
           then fails, the target holding the scene but not the fill. */
        char fill_error[256];
        fill_error[0] = '\0';
        if (scissor.active &&
            !btbh_gpu_scene_fill_outside_rect(
                command,
                runtime,
                target,
                &scissor.output,
                clear_red,
                clear_green,
                clear_blue,
                clear_alpha
            )) {
            (void)snprintf(fill_error, sizeof(fill_error), "GPU scene could not fill outside the rect: %s",
                SDL_GetError());
        }
        bool submitted = false;
        if (btbh_gpu_cost_log_enabled()) {
            /* The cost log times it alone (since 0.16.0): BTBH_GPU_COST_TARGET. */
            submitted = btbh_gpu_cost_submit_target(
                window->device->gpu,
                command,
                view,
                target,
                render_width,
                render_height,
                cost_main_triangles,
                cost_shadow_triangles
            );
        } else {
            const uint64_t submit_started_ticks = btbh_gpu_lazy_creation_clock();
            submitted = SDL_SubmitGPUCommandBuffer(command);
            btbh_gpu_lazy_creation_call_timed(BTBH_GPU_LAZY_CALL_TARGET_SUBMIT, submit_started_ticks);
        }
        btbh_gpu_window_scene_submission_finished(window, submitted);
        if (!submitted) {
            btbh_gpu_scene_error(
                report,
                BTBH_GPU_DEVICE_FAILURE,
                SDL_GetError()
            );
        } else if (fill_error[0] != '\0') {
            btbh_gpu_scene_error(report, BTBH_GPU_DEVICE_FAILURE, fill_error);
        }
    }
    if (fresh_clear_texture != NULL) {
        /* The present refused or failed: the texture made for it goes. */
        SDL_ReleaseGPUTexture(window->device->gpu, fresh_clear_texture);
    }
    btbh_gpu_scene_leave_aside(window, &swap);
    btbh_gpu_lazy_creation_end(&lazy_scope);
    return report->result;
}

BTBHGPUResult btbh_gpu_window_scene_render_to_target(
    BTBHGPUWindow *window,
    BTBHGPUTexture *target,
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
    return btbh_gpu_scene_render_to_target_internal(
        window,
        target,
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
        NULL,
        report
    );
}

BTBHGPUResult btbh_gpu_window_scene_render_to_target_view(
    BTBHGPUWindow *window,
    BTBHGPUTexture *target,
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
    const BTBHGPUSceneTargetView *view,
    BTBHGPUSceneReport *report
) {
    return btbh_gpu_scene_render_to_target_internal(
        window,
        target,
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
        view,
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
        /* (Submitted outside the canvas: no cost-log line for this frame.) */
        btbh_gpu_cost_abandon_frame();
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
        btbh_gpu_texture_is_array(destination) ||
        btbh_gpu_texture_is_heightfield(destination) ||
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
    /* (Submitted outside the canvas: no cost-log line for this frame.) */
    btbh_gpu_cost_abandon_frame();
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
    if (submitted) {
        window->submitted_frame_count += 1u;
    }
    /* A frame of the window's: a new use of its kept palettes (since 0.16.0). */
    if (window->bone_palette_cache != NULL) {
        (void)btbh_gpu_bone_palette_cache_begin(&window->bone_palette_cache, true, false);
    }
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
        btbh_gpu_scene_release_hbao_pipelines(window->scene_runtime, device);
        btbh_gpu_scene_release_hbao_targets(window->scene_runtime, device);
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
        if (window->scene_runtime->target_clear_texture != NULL) {
            SDL_ReleaseGPUTexture(
                device,
                window->scene_runtime->target_clear_texture
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
        if (window->scene_runtime->particle_transmittance_texture != NULL) {
            SDL_ReleaseGPUTexture(device, window->scene_runtime->particle_transmittance_texture);
        }
        /* RFC 0014: the clipped passes' pipelines and a second view's own maps. */
        btbh_gpu_scene_release_pipeline_array(
            device,
            window->scene_runtime->view_pipelines,
            BTBH_GPU_SCENE_VIEW_PIPELINE_COUNT
        );
        for (uint32_t map = 0u; map < 2u; ++map) {
            if (window->scene_runtime->view_shadow_textures[map] != NULL) {
                SDL_ReleaseGPUTexture(device, window->scene_runtime->view_shadow_textures[map]);
            }
        }
        if (window->scene_runtime->view_particle_transmittance_texture != NULL) {
            SDL_ReleaseGPUTexture(device, window->scene_runtime->view_particle_transmittance_texture);
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
        if (window->scene_runtime->particle_transmittance_pipeline != NULL) {
            SDL_ReleaseGPUGraphicsPipeline(device, window->scene_runtime->particle_transmittance_pipeline);
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
        btbh_gpu_scene_release_pipeline_array(
            device,
            &window->scene_runtime
                ->table_model_pipelines[0][0][0][0][0][0][0],
            pipeline_count
        );
        btbh_gpu_scene_release_pipeline_array(
            device,
            &window->scene_runtime
                ->non_omni_shadowed_table_model_pipelines
                    [0][0][0][0][0][0][0],
            pipeline_count
        );
        btbh_gpu_scene_release_pipeline_array(
            device,
            &window->scene_runtime
                ->unshadowed_table_model_pipelines
                    [0][0][0][0][0][0][0],
            pipeline_count
        );
        if (window->scene_runtime->lift_pipelines != NULL) {
            for (uint32_t table = 0u; table < 2u; ++table) {
                for (uint32_t shadow_set = 0u; shadow_set < 3u; ++shadow_set) {
                    btbh_gpu_scene_release_pipeline_array(
                        device,
                        &window->scene_runtime->lift_pipelines->sets[table][shadow_set][0][0][0][0][0][0][0],
                        pipeline_count
                    );
                }
            }
        }
        btbh_gpu_scene_release_pipeline_array(
            device,
            &window->scene_runtime->lit_particle_pipelines[0][0][0][0][0],
            sizeof(window->scene_runtime->lit_particle_pipelines) / sizeof(SDL_GPUGraphicsPipeline *)
        );
    }
    free(window->scene_runtime->lift_pipelines);
    window->scene_runtime->lift_pipelines = NULL;
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
    if (window->scene_runtime->fallback_array_texture != NULL) {
        btbh_gpu_texture_destroy(
            window->scene_runtime->fallback_array_texture
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
    free(window->scene_runtime->dig_bindings);
    free(window->scene_runtime->temporal_palettes);
    free(window->scene_runtime->temporal_palette_matrices);
    free(window->scene_runtime->opaque_order);
    free(window->scene_runtime->opaque_geometry_base);
    free(window->scene_runtime->opaque_order_keys);
    free(window->scene_runtime->view_opaque_order);
    free(window->scene_runtime->view_visibility);
    free(window->scene_runtime->run_verify);
    for (uint32_t check = 0u; check < BTBH_GPU_SCENE_SHADER_CHECK_CAPACITY; ++check) {
        free(window->scene_runtime->scene_shader_checks[check].path);
    }
    free(window->scene_runtime);
    window->scene_runtime = NULL;
}

/* Releases the runtime in `slot`, one a scene rendered to a target uses (render_to_target's, a target view's); the
   window's own runtime and its pending frame stay as they are. */
static void btbh_gpu_scene_release_runtime_slot(BTBHGPUWindow *window, BTBHGPUSceneRuntime **slot) {
    if (*slot == NULL) {
        return;
    }
    BTBHGPUSceneRuntime *window_runtime = window->scene_runtime;
    const bool pending_scene_ready = window->pending_scene_ready;
    window->scene_runtime = *slot;
    btbh_gpu_window_release_scene(window);
    *slot = NULL;
    window->scene_runtime = window_runtime;
    window->pending_scene_ready = pending_scene_ready;
}

/* Releases the runtimes of the scenes rendered to a target: btbh_gpu_window_scene_render_to_target's and every target
   view's (since 0.15.0); the window's own runtime and its pending frame stay as they are. */
void btbh_gpu_window_release_aside_scene(BTBHGPUWindow *window) {
    if (window == NULL) {
        return;
    }
    btbh_gpu_scene_release_runtime_slot(window, &window->aside_scene_runtime);
    for (uint32_t view = 0u; view < BTBH_GPU_SCENE_TARGET_VIEW_CAPACITY; ++view) {
        btbh_gpu_scene_release_runtime_slot(window, &window->target_view_runtimes[view]);
    }
}

BTBHGPUResult btbh_gpu_window_scene_release_target_view(
    BTBHGPUWindow *window,
    uint32_t view_id
) {
    if (window == NULL || view_id > BTBH_GPU_SCENE_TARGET_VIEW_CAPACITY) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    btbh_gpu_scene_release_runtime_slot(
        window,
        view_id == 0u
            ? &window->aside_scene_runtime
            : &window->target_view_runtimes[view_id - 1u]
    );
    return BTBH_GPU_OK;
}

int64_t btbh_gpu_window_scene_shadow_render_count(const BTBHGPUWindow *window, uint32_t view_id) {
    if (window == NULL) {
        return -1;
    }
    const BTBHGPUSceneRuntime *runtime = NULL;
    if (view_id == BTBH_GPU_SCENE_WINDOW_RUNTIME) {
        runtime = window->scene_runtime;
    } else if (view_id == 0u) {
        runtime = window->aside_scene_runtime;
    } else if (view_id <= BTBH_GPU_SCENE_TARGET_VIEW_CAPACITY) {
        runtime = window->target_view_runtimes[view_id - 1u];
    }
    return runtime != NULL ? (int64_t)runtime->shadow_render_count : -1;
}
