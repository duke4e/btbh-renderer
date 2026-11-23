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

/* RFC 0010 (source_tops). The share of a field texel by which a ray piece must start above a source's top to pass
   over it (production_hrc_trace.comp, production_hrc_finish.comp). */
#define BTBH_GPU_RC_SOURCE_TOP_MARGIN 0.05f
/* What the tops texture is cleared to, where no source is: the lowest half float. */
#define BTBH_GPU_RC_SOURCE_TOP_NONE (-65504.0f)

/* An emitter's top with source_tops 1 (position_y + top_above), as production_rc_source writes it. */
float btbh_gpu_radiance_cascades_emitter_top(
    const BTBHGPUSceneRadianceEmitter *emitter
);

/* A C copy of the trace's test: whether a piece that starts at start_height passes over a source whose top the tops
   texture holds as `top`, in a field of cell_size world units a texel. */
bool btbh_gpu_radiance_cascades_piece_passes_over(
    float start_height,
    float top,
    float cell_size
);

/* A top as the R16F tops texture keeps it: the nearest half float (ties to even), infinite past 65504. */
float btbh_gpu_radiance_cascades_half_round(float value);

/* A C copy of the resolve's lookup with source_tops 1: the offset, in field texels (x along world x, y along world z),
   from where the resolve reads a receiver's field with source_tops 0 to where it reads it with 1. Every receiver reads
   it half a texel back toward -x and -z (where the field's texels lie: 0.9.0's lookup reads texel k's centre at
   origin + k * cell, and it lies at origin + (k + 0.5) * cell), and a steep face (|normal y| < 0.7) half a texel out
   along its normal's level part from there. */
void btbh_gpu_radiance_cascades_tops_lookup_offset(
    const float normal[3],
    float offset[2]
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
