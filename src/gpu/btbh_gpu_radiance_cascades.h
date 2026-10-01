#ifndef BTBH_GPU_RADIANCE_CASCADES_H
#define BTBH_GPU_RADIANCE_CASCADES_H

#include "btbh_gpu/btbh_gpu.h"

#include <stdbool.h>
#include <stdint.h>

#define BTBH_GPU_RC_COMPLETION_SAMPLE_CAPACITY 32u
#define BTBH_GPU_RC_COMPLETION_SAMPLE_MINIMUM 16u

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

uint32_t btbh_gpu_radiance_cascades_direct_resolution_scale(
    BTBHGPUSceneRadianceCascadesTier tier
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
