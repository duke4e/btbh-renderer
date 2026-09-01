#ifndef BTBH_GPU_RADIANCE_CASCADES_H
#define BTBH_GPU_RADIANCE_CASCADES_H

#include "btbh_gpu/btbh_gpu.h"

#include <stdbool.h>
#include <stdint.h>

#define BTBH_GPU_RC_COMPLETION_SAMPLE_CAPACITY 32u
#define BTBH_GPU_RC_COMPLETION_SAMPLE_MINIMUM 16u
#define BTBH_GPU_RC_MAX_PRODUCTION_CASCADES 5u

/*
 * CPU reference contract shared by resource allocation, shaders, and
 * conformance tests. Directions are stored as a 2D tile inside every probe
 * so practical 4x angular branching does not create over-wide textures.
 */
typedef struct BTBHGPUSceneRadianceCascadesProfile {
    uint32_t cascade_count;
    uint32_t base_probe_spacing_cells;
    uint32_t base_direction_tile_width;
    uint32_t base_direction_tile_height;
    uint32_t angular_branching;
    uint32_t interval_branching;
    float base_interval_length_cells;
} BTBHGPUSceneRadianceCascadesProfile;

typedef struct BTBHGPUSceneRadianceCascadeLayout {
    uint32_t cascade_index;
    uint32_t probe_spacing_cells;
    uint32_t probe_count_x;
    uint32_t probe_count_y;
    uint32_t direction_tile_width;
    uint32_t direction_tile_height;
    uint32_t direction_count;
    uint32_t atlas_width;
    uint32_t atlas_height;
    float interval_start_cells;
    float interval_end_cells;
} BTBHGPUSceneRadianceCascadeLayout;

typedef struct BTBHGPUSceneRadianceInterval {
    float red;
    float green;
    float blue;
    float transmittance;
} BTBHGPUSceneRadianceInterval;

typedef struct BTBHGPUSceneRadianceCascadesState {
    BTBHGPUSceneRadianceCascadesTier requested_tier;
    BTBHGPUSceneRadianceCascadesTier selected_tier;
    BTBHGPUSceneRadianceCascadesTier pending_tier;
    BTBHGPUSceneRadianceCascadesBackend requested_backend;
    BTBHGPUSceneRadianceCascadesBackend active_backend;
    BTBHGPUSceneRadianceCascadesFallbackReason fallback_reason;
    double completion_samples[BTBH_GPU_RC_COMPLETION_SAMPLE_CAPACITY];
    uint32_t completion_sample_count;
    uint32_t completion_sample_cursor;
    uint32_t generation;
    bool sticky_failure;
} BTBHGPUSceneRadianceCascadesState;

bool btbh_gpu_radiance_cascades_profile_for_tier(
    BTBHGPUSceneRadianceCascadesTier tier,
    BTBHGPUSceneRadianceCascadesProfile *profile
);

bool btbh_gpu_radiance_cascades_profile_valid(
    const BTBHGPUSceneRadianceCascadesProfile *profile
);

bool btbh_gpu_radiance_cascades_raw_trace_required(
    uint32_t cascade_index,
    uint32_t cascade_count,
    bool reprojected_bilinear_merge
);

uint32_t btbh_gpu_radiance_cascades_direct_resolution_scale(
    const BTBHGPUSceneRadianceCascadesProfile *profile
);

bool btbh_gpu_radiance_cascade_layout(
    const BTBHGPUSceneRadianceCascadesProfile *profile,
    uint32_t domain_width_cells,
    uint32_t domain_height_cells,
    uint32_t cascade_index,
    uint32_t max_texture_dimension,
    BTBHGPUSceneRadianceCascadeLayout *layout
);

bool btbh_gpu_radiance_cascade_atlas_address(
    const BTBHGPUSceneRadianceCascadeLayout *layout,
    uint32_t probe_x,
    uint32_t probe_y,
    uint32_t direction_index,
    uint32_t *atlas_x,
    uint32_t *atlas_y
);

bool btbh_gpu_radiance_cascade_direction_children(
    const BTBHGPUSceneRadianceCascadesProfile *profile,
    const BTBHGPUSceneRadianceCascadeLayout *lower_layout,
    uint32_t lower_direction_index,
    uint32_t *first_upper_direction,
    uint32_t *upper_direction_count
);

BTBHGPUSceneRadianceInterval btbh_gpu_radiance_interval_merge(
    BTBHGPUSceneRadianceInterval near_interval,
    BTBHGPUSceneRadianceInterval far_interval
);

BTBHGPUSceneRadianceInterval btbh_gpu_radiance_interval_bilinear(
    BTBHGPUSceneRadianceInterval bottom_left,
    BTBHGPUSceneRadianceInterval bottom_right,
    BTBHGPUSceneRadianceInterval top_left,
    BTBHGPUSceneRadianceInterval top_right,
    float weight_x,
    float weight_y
);

bool btbh_gpu_radiance_cascades_config_valid(
    const BTBHGPUSceneRadianceCascades *config
);

uint32_t btbh_gpu_radiance_cascades_directional_emitter_count(
    const BTBHGPUSceneRadianceCascades *config
);

uint32_t btbh_gpu_radiance_cascades_holographic_scale_for_tier(
    BTBHGPUSceneRadianceCascadesTier tier
);

void btbh_gpu_radiance_cascades_state_initialize(
    BTBHGPUSceneRadianceCascadesState *state
);

bool btbh_gpu_radiance_cascades_state_configure(
    BTBHGPUSceneRadianceCascadesState *state,
    const BTBHGPUSceneRadianceCascades *config
);

void btbh_gpu_radiance_cascades_record_completion(
    BTBHGPUSceneRadianceCascadesState *state,
    double completion_milliseconds,
    double frame_budget_milliseconds
);

void btbh_gpu_radiance_cascades_mark_failure(
    BTBHGPUSceneRadianceCascadesState *state,
    BTBHGPUSceneRadianceCascadesFallbackReason reason
);

BTBHGPUSceneRadianceCascadesTier
btbh_gpu_radiance_cascades_effective_tier(
    const BTBHGPUSceneRadianceCascadesState *state
);

void btbh_gpu_radiance_cascades_dimensions(
    BTBHGPUSceneRadianceCascadesTier tier,
    uint32_t render_width,
    uint32_t render_height,
    uint32_t *internal_width,
    uint32_t *internal_height
);

void btbh_gpu_radiance_cascades_status(
    const BTBHGPUSceneRadianceCascadesState *state,
    uint32_t internal_width,
    uint32_t internal_height,
    double cpu_encode_milliseconds,
    BTBHGPUSceneRadianceCascadesStatus *status
);

#endif
