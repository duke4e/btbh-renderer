#ifndef BTBH_GPU_HOLOGRAPHIC_RADIANCE_CASCADES_H
#define BTBH_GPU_HOLOGRAPHIC_RADIANCE_CASCADES_H

#include <stdbool.h>
#include <stdint.h>

#define BTBH_GPU_HRC_MAX_LEVELS 11u
#define BTBH_GPU_HRC_SEGMENT_COUNT 8u
#define BTBH_GPU_HRC_TRACED_LEVEL_COUNT 3u
#define BTBH_GPU_HRC_LOW_SCALE 1u
#define BTBH_GPU_HRC_HIGH_SCALE 2u
#define BTBH_GPU_HRC_DEFAULT_SCALE BTBH_GPU_HRC_HIGH_SCALE
#define BTBH_GPU_HRC_DEFAULT_MEMORY_BUDGET_BYTES (128ull * 1024ull * 1024ull)

typedef struct BTBHGPUHolographicRadianceCascadesLevelLayout {
    uint32_t level;
    uint32_t grid_width;
    uint32_t grid_height;
    uint32_t direction_count;
    uint32_t acceleration_direction_count;
    uint32_t acceleration_atlas_width;
    uint32_t acceleration_atlas_height;
    uint32_t radiance_atlas_width;
    uint32_t radiance_atlas_height;
    uint64_t acceleration_texel_count;
} BTBHGPUHolographicRadianceCascadesLevelLayout;

typedef struct BTBHGPUHolographicRadianceCascadesLayout {
    uint32_t input_width;
    uint32_t input_height;
    uint32_t output_width;
    uint32_t output_height;
    uint32_t base_grid_size;
    uint32_t level_count;
    uint32_t traced_level_count;
    uint32_t segment_count;
    uint32_t radiance_atlas_width;
    uint32_t radiance_atlas_height;
    uint32_t quadrant_atlas_width;
    uint32_t quadrant_atlas_height;
    uint64_t acceleration_bytes;
    uint64_t working_set_bytes;
    BTBHGPUHolographicRadianceCascadesLevelLayout
        levels[BTBH_GPU_HRC_MAX_LEVELS];
} BTBHGPUHolographicRadianceCascadesLayout;

bool btbh_gpu_holographic_radiance_cascades_layout(
    uint32_t input_width,
    uint32_t input_height,
    uint32_t linear_scale,
    uint32_t max_texture_dimension,
    uint64_t memory_budget_bytes,
    BTBHGPUHolographicRadianceCascadesLayout *layout
);

#endif
