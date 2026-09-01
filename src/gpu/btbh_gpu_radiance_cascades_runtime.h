#ifndef BTBH_GPU_RADIANCE_CASCADES_RUNTIME_H
#define BTBH_GPU_RADIANCE_CASCADES_RUNTIME_H

#include "btbh_gpu_radiance_cascades.h"
#include "btbh_gpu_holographic_radiance_cascades.h"

#include <SDL3/SDL.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BTBH_GPU_RC_CASCADE_COUNT BTBH_GPU_RC_MAX_PRODUCTION_CASCADES

typedef struct BTBHGPUSceneRadianceCascadesRuntime {
    BTBHGPUSceneRadianceCascadesState state;
    SDL_GPUComputePipeline *trace_pipeline;
    SDL_GPUComputePipeline *merge_pipeline;
    SDL_GPUComputePipeline *prefilter_pipeline;
    SDL_GPUComputePipeline *diffuse_cache_pipeline;
    SDL_GPUComputePipeline *resolve_pipeline;
    SDL_GPUComputePipeline *hrc_trace_pipeline;
    SDL_GPUComputePipeline *hrc_merge_up_pipeline;
    SDL_GPUComputePipeline *hrc_merge_down_pipeline;
    SDL_GPUComputePipeline *hrc_reproject_pipeline;
    SDL_GPUComputePipeline *hrc_finish_pipeline;
    SDL_GPUComputePipeline *hrc_blur_pipeline;
    SDL_GPUComputePipeline *hrc_resolve_pipeline;
    SDL_GPUComputePipeline *hrc_reconstruct_pipeline;
    SDL_GPUGraphicsPipeline *source_pipeline;
    SDL_GPUGraphicsPipeline *directional_source_pipeline;
    SDL_GPUGraphicsPipeline *direct_light_pipeline;
    SDL_GPUGraphicsPipeline *material_source_pipelines[2];
    SDL_GPUGraphicsPipeline *compose_pipeline;
    SDL_GPUSampler *sampler;
    SDL_GPUBuffer *emitter_buffer;
    SDL_GPUTransferBuffer *emitter_transfer_buffer;
    uint32_t emitter_capacity;
    uint32_t emitter_count;
    uint32_t directional_emitter_count;
    uint32_t emitter_upload_size;
    SDL_GPUTexture *source_field_texture;
    SDL_GPUTexture *directional_source_texture;
    SDL_GPUTexture *direct_light_texture;
    uint32_t source_field_width;
    uint32_t source_field_height;
    BTBHGPUSceneRadianceCascadesProfile profile;
    BTBHGPUSceneRadianceCascadeLayout
        cascade_layouts[BTBH_GPU_RC_CASCADE_COUNT];
    uint32_t cascade_count;
    SDL_GPUTexture *raw_cascade_textures[BTBH_GPU_RC_CASCADE_COUNT];
    SDL_GPUTexture *cascade_textures[BTBH_GPU_RC_CASCADE_COUNT];
    SDL_GPUTexture *prefiltered_cascade_texture;
    SDL_GPUTexture *diffuse_cache_texture;
    BTBHGPUHolographicRadianceCascadesLayout hrc_layout;
    SDL_GPUTexture *hrc_acceleration_textures[BTBH_GPU_HRC_MAX_LEVELS];
    SDL_GPUTexture *hrc_radiance_textures[2];
    SDL_GPUTexture *hrc_quadrant_textures[3];
    BTBHGPUSceneRadianceCascadesBackend resource_backend;
    uint32_t resolve_atlas_width;
    uint32_t resolve_atlas_height;
    uint32_t resolve_direction_tile_width;
    uint32_t resolve_direction_tile_height;
    uint32_t resolve_direction_count;
    uint32_t diffuse_cache_atlas_width;
    uint32_t diffuse_cache_atlas_height;
    SDL_GPUTexture *occlusion_textures[
        BTBH_GPU_RC_MAX_OCCLUSION_CASCADES
    ];
    SDL_GPUTransferBuffer *occlusion_transfer_buffers[
        BTBH_GPU_RC_MAX_OCCLUSION_CASCADES
    ];
    uint32_t occlusion_widths[BTBH_GPU_RC_MAX_OCCLUSION_CASCADES];
    uint32_t occlusion_heights[BTBH_GPU_RC_MAX_OCCLUSION_CASCADES];
    uint64_t occlusion_revisions[BTBH_GPU_RC_MAX_OCCLUSION_CASCADES];
    uint32_t occlusion_transfer_capacities[
        BTBH_GPU_RC_MAX_OCCLUSION_CASCADES
    ];
    uint32_t occlusion_upload_widths[
        BTBH_GPU_RC_MAX_OCCLUSION_CASCADES
    ];
    uint32_t occlusion_upload_heights[
        BTBH_GPU_RC_MAX_OCCLUSION_CASCADES
    ];
    uint64_t occlusion_pending_revisions[
        BTBH_GPU_RC_MAX_OCCLUSION_CASCADES
    ];
    uint32_t occlusion_upload_pending_mask;
    SDL_GPUTexture *indirect_texture;
    SDL_GPUTexture *hrc_reconstructed_texture;
    SDL_GPUTexture *composed_texture;
    uint32_t internal_width;
    uint32_t internal_height;
    uint32_t render_width;
    uint32_t render_height;
    float auto_frame_budget_milliseconds;
    BTBHGPUSceneRadianceCascadesPassTelemetry pass_telemetry;
    uint32_t hrc_history_generation;
    uint32_t hrc_temporal_phase;
    uint32_t hrc_history_read_index;
    float hrc_history_origin_x;
    float hrc_history_origin_z;
    float hrc_history_cell_size;
    bool hrc_history_valid;
    bool logged_failure;
} BTBHGPUSceneRadianceCascadesRuntime;

void btbh_gpu_radiance_cascades_runtime_initialize(
    BTBHGPUSceneRadianceCascadesRuntime *runtime
);

void btbh_gpu_radiance_cascades_runtime_release(
    SDL_GPUDevice *device,
    BTBHGPUSceneRadianceCascadesRuntime *runtime
);

bool btbh_gpu_radiance_cascades_runtime_prepare(
    SDL_GPUDevice *device,
    BTBHGPUSceneRadianceCascadesRuntime *runtime,
    const BTBHGPUSceneRadianceCascades *config,
    const char *post_vertex_shader_path,
    const char *post_fragment_shader_path,
    BTBHGPUShaderFormat shader_format,
    SDL_GPUTextureFormat scene_color_format,
    uint32_t render_width,
    uint32_t render_height,
    char *message,
    size_t message_capacity
);

bool btbh_gpu_radiance_cascades_runtime_encode(
    SDL_GPUCommandBuffer *command,
    BTBHGPUSceneRadianceCascadesRuntime *runtime,
    const BTBHGPUSceneRadianceCascades *config,
    const BTBHGPUSceneLighting *lighting,
    const float inverse_view_projection[16],
    SDL_GPUTexture *scene_texture,
    SDL_GPUTexture *ambient_texture,
    SDL_GPUTexture *ambient_specular_texture,
    SDL_GPUTexture *emission_texture,
    SDL_GPUTexture *normal_texture,
    SDL_GPUTexture *depth_texture,
    SDL_GPUTexture *hero_omni_shadow_texture,
    const float hero_omni_shadow_position_range[4],
    float hero_omni_shadow_near,
    float hero_omni_shadow_softness,
    SDL_GPUTexture **composed_texture
);

bool btbh_gpu_radiance_cascades_runtime_encode_emitters(
    SDL_GPUCommandBuffer *command,
    BTBHGPUSceneRadianceCascadesRuntime *runtime,
    const BTBHGPUSceneRadianceCascades *config
);

bool btbh_gpu_radiance_cascades_runtime_encode_fallback_debug(
    SDL_GPUCommandBuffer *command,
    BTBHGPUSceneRadianceCascadesRuntime *runtime,
    SDL_GPUTexture *scene_texture,
    SDL_GPUTexture **composed_texture
);

void btbh_gpu_radiance_cascades_runtime_record_completion(
    BTBHGPUSceneRadianceCascadesRuntime *runtime,
    double completion_milliseconds
);

void btbh_gpu_radiance_cascades_runtime_invalidate_history(
    BTBHGPUSceneRadianceCascadesRuntime *runtime
);

#endif
