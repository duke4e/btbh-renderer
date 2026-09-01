#include "btbh_gpu_holographic_radiance_cascades.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

#define BTBH_GPU_HRC_INTERVAL_BYTES 8ull

static bool btbh_gpu_hrc_power_of_two(uint32_t value) {
    return value != 0u && (value & (value - 1u)) == 0u;
}

static bool btbh_gpu_hrc_multiply_u32(
    uint32_t left,
    uint32_t right,
    uint32_t *result
) {
    if (result == NULL ||
        (right != 0u && left > UINT32_MAX / right)) {
        return false;
    }
    *result = left * right;
    return true;
}

static bool btbh_gpu_hrc_multiply_u64(
    uint64_t left,
    uint64_t right,
    uint64_t *result
) {
    if (result == NULL ||
        (right != 0ull && left > UINT64_MAX / right)) {
        return false;
    }
    *result = left * right;
    return true;
}

static bool btbh_gpu_hrc_add_u64(
    uint64_t left,
    uint64_t right,
    uint64_t *result
) {
    if (result == NULL || left > UINT64_MAX - right) {
        return false;
    }
    *result = left + right;
    return true;
}

bool btbh_gpu_holographic_radiance_cascades_layout(
    uint32_t input_width,
    uint32_t input_height,
    uint32_t linear_scale,
    uint32_t max_texture_dimension,
    uint64_t memory_budget_bytes,
    BTBHGPUHolographicRadianceCascadesLayout *layout
) {
    if (layout == NULL || input_width != input_height ||
        !btbh_gpu_hrc_power_of_two(input_width) ||
        !btbh_gpu_hrc_power_of_two(linear_scale) ||
        linear_scale == 0u || max_texture_dimension == 0u ||
        memory_budget_bytes == 0ull) {
        return false;
    }

    BTBHGPUHolographicRadianceCascadesLayout candidate;
    (void)memset(&candidate, 0, sizeof(candidate));
    if (!btbh_gpu_hrc_multiply_u32(
            input_width,
            linear_scale,
            &candidate.output_width
        ) ||
        !btbh_gpu_hrc_multiply_u32(
            input_height,
            linear_scale,
            &candidate.output_height
        ) ||
        candidate.output_width != candidate.output_height ||
        candidate.output_width < 2u) {
        return false;
    }
    candidate.input_width = input_width;
    candidate.input_height = input_height;
    candidate.base_grid_size = candidate.output_width / 2u;
    candidate.segment_count = BTBH_GPU_HRC_SEGMENT_COUNT;
    candidate.traced_level_count = BTBH_GPU_HRC_TRACED_LEVEL_COUNT;

    uint32_t base_height = 0u;
    if (!btbh_gpu_hrc_multiply_u32(
            candidate.base_grid_size,
            candidate.segment_count,
            &base_height
        ) ||
        !btbh_gpu_hrc_multiply_u32(
            candidate.base_grid_size,
            2u,
            &candidate.radiance_atlas_width
        ) ||
        !btbh_gpu_hrc_multiply_u32(
            candidate.output_height,
            4u,
            &candidate.quadrant_atlas_height
        )) {
        return false;
    }
    candidate.radiance_atlas_height = base_height;
    candidate.quadrant_atlas_width = candidate.output_width;
    if (candidate.radiance_atlas_width > max_texture_dimension ||
        candidate.radiance_atlas_height > max_texture_dimension ||
        candidate.quadrant_atlas_width > max_texture_dimension ||
        candidate.quadrant_atlas_height > max_texture_dimension) {
        return false;
    }

    uint32_t level_count = 1u;
    for (uint32_t width = candidate.base_grid_size;
         width > 1u;
         width >>= 1u) {
        ++level_count;
    }
    if (level_count > BTBH_GPU_HRC_MAX_LEVELS) {
        return false;
    }
    candidate.level_count = level_count;
    if (candidate.traced_level_count > level_count) {
        candidate.traced_level_count = level_count;
    }

    uint64_t acceleration_texels = 0ull;
    for (uint32_t level = 0u; level < level_count; ++level) {
        BTBHGPUHolographicRadianceCascadesLevelLayout *level_layout =
            &candidate.levels[level];
        level_layout->level = level;
        level_layout->grid_width = candidate.base_grid_size >> level;
        level_layout->grid_height = base_height;
        if (level >= 31u ||
            !btbh_gpu_hrc_multiply_u32(
                2u,
                1u << level,
                &level_layout->direction_count
            ) ||
            level_layout->direction_count == UINT32_MAX) {
            return false;
        }
        level_layout->acceleration_direction_count =
            level_layout->direction_count + 1u;
        if (!btbh_gpu_hrc_multiply_u32(
                level_layout->grid_width,
                level_layout->acceleration_direction_count,
                &level_layout->acceleration_atlas_width
            )) {
            return false;
        }
        level_layout->acceleration_atlas_height = base_height;
        level_layout->radiance_atlas_width =
            candidate.radiance_atlas_width;
        level_layout->radiance_atlas_height =
            candidate.radiance_atlas_height;
        if (level_layout->acceleration_atlas_width >
                max_texture_dimension ||
            level_layout->acceleration_atlas_height >
                max_texture_dimension) {
            return false;
        }
        if (!btbh_gpu_hrc_multiply_u64(
                (uint64_t)level_layout->acceleration_atlas_width,
                (uint64_t)level_layout->acceleration_atlas_height,
                &level_layout->acceleration_texel_count
            ) ||
            !btbh_gpu_hrc_add_u64(
                acceleration_texels,
                level_layout->acceleration_texel_count,
                &acceleration_texels
            )) {
            return false;
        }
    }

    if (!btbh_gpu_hrc_multiply_u64(
            acceleration_texels,
            BTBH_GPU_HRC_INTERVAL_BYTES,
            &candidate.acceleration_bytes
        )) {
        return false;
    }
    uint64_t radiance_texels = 0ull;
    uint64_t fixed_texture_bytes = 0ull;
    if (!btbh_gpu_hrc_multiply_u64(
            (uint64_t)candidate.radiance_atlas_width,
            (uint64_t)candidate.radiance_atlas_height,
            &radiance_texels
        ) ||
        !btbh_gpu_hrc_multiply_u64(
            radiance_texels,
            5ull * BTBH_GPU_HRC_INTERVAL_BYTES,
            &fixed_texture_bytes
        ) ||
        !btbh_gpu_hrc_add_u64(
            candidate.acceleration_bytes,
            fixed_texture_bytes,
            &candidate.working_set_bytes
        ) ||
        candidate.working_set_bytes > memory_budget_bytes) {
        return false;
    }

    *layout = candidate;
    return true;
}
