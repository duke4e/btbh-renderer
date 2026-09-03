#include "btbh_gpu_radiance_cascades.h"
#include "btbh_gpu_radiance_cascades_runtime.h"

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define BTBH_GPU_RC_COMPATIBILITY_FIELD_SIZE 64u

#define BTBH_GPU_RC_MODEL_VERTEX_STRIDE 96u
#define BTBH_GPU_RC_MODEL_COLOR_OFFSET 48u
#define BTBH_GPU_RC_MODEL_JOINTS_OFFSET 64u
#define BTBH_GPU_RC_MODEL_WEIGHTS_OFFSET 80u
#define BTBH_GPU_RC_MODEL_TANGENT_OFFSET 32u
#define BTBH_GPU_RC_DIRECTIONAL_SOURCE_TILE_WIDTH 8u
#define BTBH_GPU_RC_DIRECTIONAL_SOURCE_TILE_HEIGHT 8u
#define BTBH_GPU_RC_DIRECTIONAL_SOURCE_COUNT 64u
#define BTBH_GPU_RC_HIGH_RESOLVE_DIRECTION_TILE_WIDTH 4u
#define BTBH_GPU_RC_HIGH_RESOLVE_DIRECTION_TILE_HEIGHT 4u
#define BTBH_GPU_RC_HIGH_RESOLVE_DIRECTION_COUNT 16u
#define BTBH_GPU_RC_DIFFUSE_CACHE_NORMAL_TILE_WIDTH 3u
#define BTBH_GPU_RC_DIFFUSE_CACHE_NORMAL_TILE_HEIGHT 3u
#define BTBH_GPU_RC_DIFFUSE_CACHE_NORMAL_COUNT 9u
#define BTBH_GPU_RC_REPROJECTED_BILINEAR_MERGE 1u

static bool btbh_gpu_radiance_cascades_power_of_two(uint32_t value) {
    return value != 0u && (value & (value - 1u)) == 0u;
}

static bool btbh_gpu_radiance_cascades_multiply_u32(
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

static bool btbh_gpu_radiance_cascades_power_u32(
    uint32_t base,
    uint32_t exponent,
    uint32_t *result
) {
    if (result == NULL) {
        return false;
    }
    uint32_t value = 1u;
    for (uint32_t index = 0u; index < exponent; ++index) {
        if (!btbh_gpu_radiance_cascades_multiply_u32(
                value,
                base,
                &value)) {
            return false;
        }
    }
    *result = value;
    return true;
}

bool btbh_gpu_radiance_cascades_raw_trace_required(
    uint32_t cascade_index,
    uint32_t cascade_count,
    bool reprojected_bilinear_merge
) {
    if (cascade_count == 0u || cascade_index >= cascade_count) {
        return false;
    }
    return !reprojected_bilinear_merge ||
        cascade_index + 1u == cascade_count;
}

bool btbh_gpu_radiance_cascades_profile_for_tier(
    BTBHGPUSceneRadianceCascadesTier tier,
    BTBHGPUSceneRadianceCascadesProfile *profile
) {
    if (profile == NULL ||
        (tier != BTBH_GPU_SCENE_RC_LOW &&
         tier != BTBH_GPU_SCENE_RC_HIGH)) {
        return false;
    }
    (void)memset(profile, 0, sizeof(*profile));
    profile->cascade_count =
        tier == BTBH_GPU_SCENE_RC_HIGH ? 5u : 4u;
    profile->base_probe_spacing_cells =
        tier == BTBH_GPU_SCENE_RC_HIGH ? 1u : 2u;
    profile->base_direction_tile_width =
        tier == BTBH_GPU_SCENE_RC_HIGH ? 8u : 2u;
    profile->base_direction_tile_height =
        tier == BTBH_GPU_SCENE_RC_HIGH ? 8u : 2u;
    profile->angular_branching =
        2u;
    profile->interval_branching =
        tier == BTBH_GPU_SCENE_RC_HIGH ? 4u : 2u;
    profile->base_interval_length_cells = 2.0f;
    return true;
}

uint32_t btbh_gpu_radiance_cascades_holographic_scale_for_tier(
    BTBHGPUSceneRadianceCascadesTier tier
) {
    switch (tier) {
        case BTBH_GPU_SCENE_RC_LOW:
            return BTBH_GPU_HRC_LOW_SCALE;
        case BTBH_GPU_SCENE_RC_HIGH:
            return BTBH_GPU_HRC_HIGH_SCALE;
        default:
            return 0u;
    }
}

uint32_t btbh_gpu_radiance_cascades_direct_resolution_scale(
    const BTBHGPUSceneRadianceCascadesProfile *profile
) {
    return profile != NULL && profile->base_probe_spacing_cells == 1u
        ? 3u
        : 1u;
}

bool btbh_gpu_radiance_cascades_profile_valid(
    const BTBHGPUSceneRadianceCascadesProfile *profile
) {
    if (profile == NULL || profile->cascade_count == 0u ||
        profile->cascade_count > BTBH_GPU_RC_MAX_PRODUCTION_CASCADES ||
        !btbh_gpu_radiance_cascades_power_of_two(
            profile->base_probe_spacing_cells) ||
        !btbh_gpu_radiance_cascades_power_of_two(
            profile->base_direction_tile_width) ||
        !btbh_gpu_radiance_cascades_power_of_two(
            profile->base_direction_tile_height) ||
        (profile->angular_branching != 2u &&
         profile->angular_branching != 4u) ||
        (profile->interval_branching != 2u &&
         profile->interval_branching != 4u) ||
        !isfinite(profile->base_interval_length_cells) ||
        profile->base_interval_length_cells <= 0.0f) {
        return false;
    }

    uint32_t unused = 0u;
    return btbh_gpu_radiance_cascades_power_u32(
        2u,
        profile->cascade_count - 1u,
        &unused
    ) && btbh_gpu_radiance_cascades_power_u32(
        profile->angular_branching,
        profile->cascade_count - 1u,
        &unused
    ) && btbh_gpu_radiance_cascades_power_u32(
        profile->interval_branching,
        profile->cascade_count - 1u,
        &unused
    );
}

static uint32_t btbh_gpu_radiance_cascades_divide_ceil(
    uint32_t value,
    uint32_t divisor
) {
    return value / divisor + (value % divisor != 0u ? 1u : 0u);
}

bool btbh_gpu_radiance_cascade_layout(
    const BTBHGPUSceneRadianceCascadesProfile *profile,
    uint32_t domain_width_cells,
    uint32_t domain_height_cells,
    uint32_t cascade_index,
    uint32_t max_texture_dimension,
    BTBHGPUSceneRadianceCascadeLayout *layout
) {
    if (!btbh_gpu_radiance_cascades_profile_valid(profile) ||
        layout == NULL || domain_width_cells == 0u ||
        domain_height_cells == 0u || max_texture_dimension == 0u ||
        cascade_index >= profile->cascade_count) {
        return false;
    }

    uint32_t spatial_scale = 0u;
    uint32_t angular_scale = 0u;
    uint32_t interval_scale = 0u;
    if (!btbh_gpu_radiance_cascades_power_u32(
            2u,
            cascade_index,
            &spatial_scale) ||
        !btbh_gpu_radiance_cascades_power_u32(
            profile->angular_branching,
            cascade_index,
            &angular_scale) ||
        !btbh_gpu_radiance_cascades_power_u32(
            profile->interval_branching,
            cascade_index,
            &interval_scale)) {
        return false;
    }

    BTBHGPUSceneRadianceCascadeLayout candidate = {0};
    candidate.cascade_index = cascade_index;
    if (!btbh_gpu_radiance_cascades_multiply_u32(
            profile->base_probe_spacing_cells,
            spatial_scale,
            &candidate.probe_spacing_cells)) {
        return false;
    }
    candidate.probe_count_x = btbh_gpu_radiance_cascades_divide_ceil(
        domain_width_cells,
        candidate.probe_spacing_cells
    );
    candidate.probe_count_y = btbh_gpu_radiance_cascades_divide_ceil(
        domain_height_cells,
        candidate.probe_spacing_cells
    );

    uint32_t tile_width_scale = 1u;
    uint32_t tile_height_scale = 1u;
    if (profile->angular_branching == 4u) {
        tile_width_scale = spatial_scale;
        tile_height_scale = spatial_scale;
    } else {
        if (!btbh_gpu_radiance_cascades_power_u32(
                2u,
                (cascade_index + 1u) / 2u,
                &tile_width_scale) ||
            !btbh_gpu_radiance_cascades_power_u32(
                2u,
                cascade_index / 2u,
                &tile_height_scale)) {
            return false;
        }
    }
    if (!btbh_gpu_radiance_cascades_multiply_u32(
            profile->base_direction_tile_width,
            tile_width_scale,
            &candidate.direction_tile_width) ||
        !btbh_gpu_radiance_cascades_multiply_u32(
            profile->base_direction_tile_height,
            tile_height_scale,
            &candidate.direction_tile_height) ||
        !btbh_gpu_radiance_cascades_multiply_u32(
            candidate.direction_tile_width,
            candidate.direction_tile_height,
            &candidate.direction_count) ||
        !btbh_gpu_radiance_cascades_multiply_u32(
            candidate.probe_count_x,
            candidate.direction_tile_width,
            &candidate.atlas_width) ||
        !btbh_gpu_radiance_cascades_multiply_u32(
            candidate.probe_count_y,
            candidate.direction_tile_height,
            &candidate.atlas_height) ||
        candidate.atlas_width > max_texture_dimension ||
        candidate.atlas_height > max_texture_dimension) {
        return false;
    }

    const double interval_length =
        (double)profile->base_interval_length_cells *
        (double)interval_scale;
    double interval_start = 0.0;
    double prior_length =
        (double)profile->base_interval_length_cells;
    for (uint32_t index = 0u; index < cascade_index; ++index) {
        interval_start += prior_length;
        prior_length *= (double)profile->interval_branching;
    }
    const double interval_end = interval_start + interval_length;
    if (!isfinite(interval_start) || !isfinite(interval_end) ||
        interval_end > (double)FLT_MAX) {
        return false;
    }
    candidate.interval_start_cells = (float)interval_start;
    candidate.interval_end_cells = (float)interval_end;
    *layout = candidate;
    return true;
}

bool btbh_gpu_radiance_cascade_atlas_address(
    const BTBHGPUSceneRadianceCascadeLayout *layout,
    uint32_t probe_x,
    uint32_t probe_y,
    uint32_t direction_index,
    uint32_t *atlas_x,
    uint32_t *atlas_y
) {
    if (layout == NULL || atlas_x == NULL || atlas_y == NULL ||
        probe_x >= layout->probe_count_x ||
        probe_y >= layout->probe_count_y ||
        direction_index >= layout->direction_count ||
        layout->direction_tile_width == 0u) {
        return false;
    }
    const uint32_t direction_x =
        direction_index % layout->direction_tile_width;
    const uint32_t direction_y =
        direction_index / layout->direction_tile_width;
    *atlas_x = probe_x * layout->direction_tile_width + direction_x;
    *atlas_y = probe_y * layout->direction_tile_height + direction_y;
    return *atlas_x < layout->atlas_width && *atlas_y < layout->atlas_height;
}

bool btbh_gpu_radiance_cascade_direction_children(
    const BTBHGPUSceneRadianceCascadesProfile *profile,
    const BTBHGPUSceneRadianceCascadeLayout *lower_layout,
    uint32_t lower_direction_index,
    uint32_t *first_upper_direction,
    uint32_t *upper_direction_count
) {
    if (!btbh_gpu_radiance_cascades_profile_valid(profile) ||
        lower_layout == NULL || first_upper_direction == NULL ||
        upper_direction_count == NULL ||
        lower_layout->cascade_index + 1u >= profile->cascade_count ||
        lower_direction_index >= lower_layout->direction_count ||
        lower_direction_index >
            UINT32_MAX / profile->angular_branching) {
        return false;
    }
    *first_upper_direction =
        lower_direction_index * profile->angular_branching;
    *upper_direction_count = profile->angular_branching;
    return true;
}

BTBHGPUSceneRadianceInterval btbh_gpu_radiance_interval_merge(
    BTBHGPUSceneRadianceInterval near_interval,
    BTBHGPUSceneRadianceInterval far_interval
) {
    BTBHGPUSceneRadianceInterval result = {
        near_interval.red +
            near_interval.transmittance * far_interval.red,
        near_interval.green +
            near_interval.transmittance * far_interval.green,
        near_interval.blue +
            near_interval.transmittance * far_interval.blue,
        near_interval.transmittance * far_interval.transmittance
    };
    return result;
}

static BTBHGPUSceneRadianceInterval
btbh_gpu_radiance_interval_linear(
    BTBHGPUSceneRadianceInterval left,
    BTBHGPUSceneRadianceInterval right,
    float weight
) {
    const float inverse_weight = 1.0f - weight;
    BTBHGPUSceneRadianceInterval result = {
        left.red * inverse_weight + right.red * weight,
        left.green * inverse_weight + right.green * weight,
        left.blue * inverse_weight + right.blue * weight,
        left.transmittance * inverse_weight +
            right.transmittance * weight
    };
    return result;
}

BTBHGPUSceneRadianceInterval btbh_gpu_radiance_interval_bilinear(
    BTBHGPUSceneRadianceInterval bottom_left,
    BTBHGPUSceneRadianceInterval bottom_right,
    BTBHGPUSceneRadianceInterval top_left,
    BTBHGPUSceneRadianceInterval top_right,
    float weight_x,
    float weight_y
) {
    const BTBHGPUSceneRadianceInterval bottom =
        btbh_gpu_radiance_interval_linear(
            bottom_left,
            bottom_right,
            weight_x
        );
    const BTBHGPUSceneRadianceInterval top =
        btbh_gpu_radiance_interval_linear(
            top_left,
            top_right,
            weight_x
        );
    return btbh_gpu_radiance_interval_linear(bottom, top, weight_y);
}

static bool btbh_gpu_radiance_cascades_tier_valid(
    BTBHGPUSceneRadianceCascadesTier tier
) {
    return tier >= BTBH_GPU_SCENE_RC_OFF &&
        tier <= BTBH_GPU_SCENE_RC_AUTO;
}

static bool btbh_gpu_radiance_cascades_backend_valid(
    BTBHGPUSceneRadianceCascadesBackend backend
) {
    return backend >= BTBH_GPU_SCENE_RC_BACKEND_CLASSIC &&
        backend <= BTBH_GPU_SCENE_RC_BACKEND_HOLOGRAPHIC;
}

static bool btbh_gpu_radiance_cascades_debug_valid(
    BTBHGPUSceneRadianceCascadesDebugView debug_view
) {
    return debug_view >= BTBH_GPU_SCENE_RC_DEBUG_NONE &&
        debug_view <= BTBH_GPU_SCENE_RC_DEBUG_DIRECT_LIGHT;
}

bool btbh_gpu_radiance_cascades_config_valid(
    const BTBHGPUSceneRadianceCascades *config
) {
    if (config == NULL ||
        (config->emitter_count > 0u && config->emitters == NULL) ||
        config->hero_omni_emitter_index < -1 ||
        (config->hero_omni_emitter_index >= 0 &&
         (uint32_t)config->hero_omni_emitter_index >=
            config->emitter_count) ||
        config->occlusion_cascade_count >
            BTBH_GPU_RC_MAX_OCCLUSION_CASCADES) {
        return false;
    }
    for (uint32_t index = 0u;
         index < config->occlusion_cascade_count;
         ++index) {
        const BTBHGPUSceneRadianceCascadesOcclusionCascade *cascade =
            &config->occlusion_cascades[index];
        if (!isfinite(cascade->origin_world_x) ||
            !isfinite(cascade->origin_world_z) ||
            !isfinite(cascade->cell_size) ||
            cascade->cell_size <= 0.0f ||
            cascade->width == 0u || cascade->width > 2048u ||
            cascade->height == 0u || cascade->height > 2048u ||
            cascade->samples == NULL) {
            return false;
        }
    }
    for (uint32_t index = 0u;
         index < config->emitter_count;
         ++index) {
        const BTBHGPUSceneRadianceEmitter *emitter =
            &config->emitters[index];
        if (!isfinite(emitter->position_x) ||
            !isfinite(emitter->position_y) ||
            !isfinite(emitter->position_z) ||
            !isfinite(emitter->radius) || emitter->radius <= 0.0f ||
            !isfinite(emitter->height) || emitter->height < 0.0f ||
            !isfinite(emitter->red) || emitter->red < 0.0f ||
            !isfinite(emitter->green) || emitter->green < 0.0f ||
            !isfinite(emitter->blue) || emitter->blue < 0.0f ||
            !isfinite(emitter->energy) || emitter->energy <= 0.0f ||
            !isfinite(emitter->direct_energy) ||
            emitter->direct_energy < 0.0f ||
            !isfinite(emitter->direction_x) ||
            !isfinite(emitter->direction_y) ||
            !isfinite(emitter->direction_z) ||
            emitter->direction_x * emitter->direction_x +
                emitter->direction_y * emitter->direction_y +
                emitter->direction_z * emitter->direction_z <= 0.0f ||
            !isfinite(emitter->inner_cutoff) ||
            !isfinite(emitter->outer_cutoff) ||
            emitter->inner_cutoff <= emitter->outer_cutoff ||
            emitter->shape < BTBH_GPU_SCENE_RADIANCE_EMITTER_DISK ||
            emitter->shape > BTBH_GPU_SCENE_RADIANCE_EMITTER_SEGMENT ||
            emitter->light_class <
                BTBH_GPU_SCENE_RADIANCE_EMITTER_CLASS_HERO ||
            emitter->light_class >
                BTBH_GPU_SCENE_RADIANCE_EMITTER_CLASS_TRANSIENT) {
            return false;
        }
    }
    return
        btbh_gpu_radiance_cascades_tier_valid(config->tier) &&
        btbh_gpu_radiance_cascades_backend_valid(config->backend) &&
        btbh_gpu_radiance_cascades_debug_valid(config->debug_view) &&
        config->allow_auto_transition <= 1u &&
        config->invalidate <= 1u &&
        isfinite(config->indirect_intensity) &&
        config->indirect_intensity >= 0.0f &&
        config->indirect_intensity <= 2.0f &&
        (config->tier != BTBH_GPU_SCENE_RC_AUTO ||
         (isfinite(config->auto_frame_budget_milliseconds) &&
          config->auto_frame_budget_milliseconds > 0.0f));
}

static bool btbh_gpu_radiance_emitter_is_directional(
    const BTBHGPUSceneRadianceEmitter *emitter
) {
    return emitter != NULL &&
        emitter->inner_cutoff > emitter->outer_cutoff &&
        emitter->outer_cutoff > 0.0001f &&
        emitter->inner_cutoff <= 1.0f;
}

uint32_t btbh_gpu_radiance_cascades_directional_emitter_count(
    const BTBHGPUSceneRadianceCascades *config
) {
    if (config == NULL ||
        (config->emitter_count > 0u && config->emitters == NULL)) {
        return 0u;
    }
    uint32_t count = 0u;
    for (uint32_t index = 0u; index < config->emitter_count; ++index) {
        if (btbh_gpu_radiance_emitter_is_directional(
                &config->emitters[index])) {
            count += 1u;
        }
    }
    return count;
}

static void btbh_gpu_radiance_cascades_clear_samples(
    BTBHGPUSceneRadianceCascadesState *state
) {
    (void)memset(
        state->completion_samples,
        0,
        sizeof(state->completion_samples)
    );
    state->completion_sample_count = 0u;
    state->completion_sample_cursor = 0u;
}

void btbh_gpu_radiance_cascades_state_initialize(
    BTBHGPUSceneRadianceCascadesState *state
) {
    if (state == NULL) {
        return;
    }
    (void)memset(state, 0, sizeof(*state));
    state->requested_tier = BTBH_GPU_SCENE_RC_OFF;
    state->selected_tier = BTBH_GPU_SCENE_RC_OFF;
    state->pending_tier = BTBH_GPU_SCENE_RC_OFF;
    state->requested_backend = BTBH_GPU_SCENE_RC_BACKEND_CLASSIC;
    state->active_backend = BTBH_GPU_SCENE_RC_BACKEND_CLASSIC;
    state->fallback_reason = BTBH_GPU_SCENE_RC_FALLBACK_DISABLED;
}

bool btbh_gpu_radiance_cascades_state_configure(
    BTBHGPUSceneRadianceCascadesState *state,
    const BTBHGPUSceneRadianceCascades *config
) {
    if (state == NULL ||
        !btbh_gpu_radiance_cascades_config_valid(config)) {
        return false;
    }

    const bool request_changed =
        state->requested_tier != config->tier ||
        state->requested_backend != config->backend;
    const bool recovered_invalid_configuration =
        state->sticky_failure &&
        state->fallback_reason ==
            BTBH_GPU_SCENE_RC_FALLBACK_INVALID_CONFIGURATION;
    if (request_changed || config->invalidate != 0u ||
        recovered_invalid_configuration) {
        state->generation += 1u;
        state->sticky_failure = false;
        state->fallback_reason = BTBH_GPU_SCENE_RC_FALLBACK_NONE;
        btbh_gpu_radiance_cascades_clear_samples(state);
    }
    state->requested_tier = config->tier;
    state->requested_backend = config->backend;
    state->active_backend = config->backend;

    if (config->tier == BTBH_GPU_SCENE_RC_OFF) {
        state->selected_tier = BTBH_GPU_SCENE_RC_OFF;
        state->pending_tier = BTBH_GPU_SCENE_RC_OFF;
        state->fallback_reason = BTBH_GPU_SCENE_RC_FALLBACK_DISABLED;
        state->sticky_failure = false;
        return true;
    }

    if (config->tier == BTBH_GPU_SCENE_RC_LOW ||
        config->tier == BTBH_GPU_SCENE_RC_HIGH) {
        state->selected_tier = config->tier;
        state->pending_tier = config->tier;
        return true;
    }

    if (request_changed) {
        state->selected_tier = BTBH_GPU_SCENE_RC_LOW;
        state->pending_tier = BTBH_GPU_SCENE_RC_LOW;
    } else if (config->allow_auto_transition != 0u &&
               state->pending_tier != state->selected_tier) {
        state->selected_tier = state->pending_tier;
        state->generation += 1u;
        state->sticky_failure = false;
        state->fallback_reason =
            state->selected_tier == BTBH_GPU_SCENE_RC_OFF
                ? BTBH_GPU_SCENE_RC_FALLBACK_DISABLED
                : BTBH_GPU_SCENE_RC_FALLBACK_NONE;
        btbh_gpu_radiance_cascades_clear_samples(state);
    }
    return true;
}

static int btbh_gpu_radiance_cascades_compare_double(
    const void *left,
    const void *right
) {
    const double left_value = *(const double *)left;
    const double right_value = *(const double *)right;
    return left_value < right_value ? -1 : left_value > right_value ? 1 : 0;
}

static double btbh_gpu_radiance_cascades_p95(
    const BTBHGPUSceneRadianceCascadesState *state
) {
    double sorted[BTBH_GPU_RC_COMPLETION_SAMPLE_CAPACITY];
    const uint32_t count = state->completion_sample_count;
    (void)memcpy(
        sorted,
        state->completion_samples,
        sizeof(double) * count
    );
    qsort(
        sorted,
        count,
        sizeof(double),
        btbh_gpu_radiance_cascades_compare_double
    );
    const uint32_t index = (uint32_t)ceil(0.95 * (double)count) - 1u;
    return sorted[index];
}

void btbh_gpu_radiance_cascades_record_completion(
    BTBHGPUSceneRadianceCascadesState *state,
    double completion_milliseconds,
    double frame_budget_milliseconds
) {
    if (state == NULL ||
        state->requested_tier != BTBH_GPU_SCENE_RC_AUTO ||
        state->sticky_failure ||
        !isfinite(completion_milliseconds) ||
        completion_milliseconds < 0.0 ||
        !isfinite(frame_budget_milliseconds) ||
        frame_budget_milliseconds <= 0.0) {
        return;
    }

    state->completion_samples[state->completion_sample_cursor] =
        completion_milliseconds;
    state->completion_sample_cursor =
        (state->completion_sample_cursor + 1u) %
        BTBH_GPU_RC_COMPLETION_SAMPLE_CAPACITY;
    if (state->completion_sample_count <
        BTBH_GPU_RC_COMPLETION_SAMPLE_CAPACITY) {
        state->completion_sample_count += 1u;
    }
    if (state->completion_sample_count <
        BTBH_GPU_RC_COMPLETION_SAMPLE_MINIMUM) {
        return;
    }

    const double p95 = btbh_gpu_radiance_cascades_p95(state);
    BTBHGPUSceneRadianceCascadesTier candidate =
        state->selected_tier;
    switch (state->selected_tier) {
        case BTBH_GPU_SCENE_RC_HIGH:
            if (p95 > frame_budget_milliseconds * 1.05) {
                candidate = BTBH_GPU_SCENE_RC_LOW;
            }
            break;
        case BTBH_GPU_SCENE_RC_LOW:
            if (p95 > frame_budget_milliseconds * 1.20) {
                candidate = BTBH_GPU_SCENE_RC_OFF;
            } else if (p95 < frame_budget_milliseconds * 0.82) {
                candidate = BTBH_GPU_SCENE_RC_HIGH;
            }
            break;
        case BTBH_GPU_SCENE_RC_OFF:
            if (p95 < frame_budget_milliseconds * 0.70) {
                candidate = BTBH_GPU_SCENE_RC_LOW;
            }
            break;
        case BTBH_GPU_SCENE_RC_AUTO:
        default:
            candidate = BTBH_GPU_SCENE_RC_LOW;
            break;
    }
    state->pending_tier = candidate;
}

void btbh_gpu_radiance_cascades_mark_failure(
    BTBHGPUSceneRadianceCascadesState *state,
    BTBHGPUSceneRadianceCascadesFallbackReason reason
) {
    if (state == NULL ||
        reason <= BTBH_GPU_SCENE_RC_FALLBACK_DISABLED ||
        reason > BTBH_GPU_SCENE_RC_FALLBACK_INVALID_CONFIGURATION) {
        return;
    }
    state->fallback_reason = reason;
    state->sticky_failure = true;
}

BTBHGPUSceneRadianceCascadesTier
btbh_gpu_radiance_cascades_effective_tier(
    const BTBHGPUSceneRadianceCascadesState *state
) {
    if (state == NULL || state->sticky_failure) {
        return BTBH_GPU_SCENE_RC_OFF;
    }
    return state->selected_tier;
}

static uint32_t btbh_gpu_radiance_cascades_rounded_dimension(
    uint32_t dimension,
    uint32_t divisor
) {
    const uint32_t scaled =
        dimension / divisor + (dimension % divisor != 0u ? 1u : 0u);
    return (scaled + 7u) & ~UINT32_C(7);
}

void btbh_gpu_radiance_cascades_dimensions(
    BTBHGPUSceneRadianceCascadesTier tier,
    uint32_t render_width,
    uint32_t render_height,
    uint32_t *internal_width,
    uint32_t *internal_height
) {
    if (internal_width == NULL || internal_height == NULL) {
        return;
    }
    *internal_width = 0u;
    *internal_height = 0u;
    const uint32_t divisor = tier == BTBH_GPU_SCENE_RC_HIGH
        ? 1u
        : tier == BTBH_GPU_SCENE_RC_LOW ? 4u : 0u;
    if (divisor == 0u || render_width == 0u || render_height == 0u) {
        return;
    }
    *internal_width = btbh_gpu_radiance_cascades_rounded_dimension(
        render_width,
        divisor
    );
    *internal_height = btbh_gpu_radiance_cascades_rounded_dimension(
        render_height,
        divisor
    );
}

void btbh_gpu_radiance_cascades_status(
    const BTBHGPUSceneRadianceCascadesState *state,
    uint32_t internal_width,
    uint32_t internal_height,
    double cpu_encode_milliseconds,
    BTBHGPUSceneRadianceCascadesStatus *status
) {
    if (state == NULL || status == NULL) {
        return;
    }
    (void)memset(status, 0, sizeof(*status));
    status->requested_tier = state->requested_tier;
    status->active_tier =
        btbh_gpu_radiance_cascades_effective_tier(state);
    status->pending_tier = state->pending_tier;
    status->requested_backend = state->requested_backend;
    status->active_backend = state->active_backend;
    status->fallback_reason = state->fallback_reason;
    status->internal_width = internal_width;
    status->internal_height = internal_height;
    status->generation = state->generation;
    status->sample_count = state->completion_sample_count;
    status->cpu_encode_milliseconds = cpu_encode_milliseconds;
}

typedef struct BTBHGPUSceneRadianceTraceUniforms {
    uint32_t atlas_resolution[2];
    uint32_t probe_count[2];
    uint32_t direction_tile[2];
    uint32_t direction_count;
    uint32_t probe_spacing_cells;
    float interval_start_cells;
    float interval_end_cells;
    float padding0[2];
    float origin_cell[4];
    uint32_t field_dimensions[2];
    uint32_t max_step_count;
    uint32_t padding1;
    float environment_radiance[4];
    float directional_direction_energy[4];
    float directional_color_lobe[4];
    float secondary_directional_direction_energy[4];
    float secondary_directional_color_lobe[4];
    float sky_top_energy[4];
    float sky_horizon_curve[4];
} BTBHGPUSceneRadianceTraceUniforms;

typedef struct BTBHGPUSceneRadianceMergeUniforms {
    uint32_t lower_atlas_resolution[2];
    uint32_t lower_probe_count[2];
    uint32_t lower_direction_tile[2];
    uint32_t lower_direction_count;
    uint32_t lower_probe_spacing_cells;
    uint32_t upper_atlas_resolution[2];
    uint32_t upper_probe_count[2];
    uint32_t upper_direction_tile[2];
    uint32_t upper_direction_count;
    uint32_t upper_probe_spacing_cells;
    uint32_t angular_branching;
    uint32_t bilinear_fix;
    float lower_interval_start_cells;
    float upper_interval_start_cells;
    float origin_cell[4];
    uint32_t field_dimensions[2];
    uint32_t max_step_count;
    uint32_t padding;
} BTBHGPUSceneRadianceMergeUniforms;

typedef struct BTBHGPUSceneRadiancePrefilterUniforms {
    uint32_t input_atlas_resolution[2];
    uint32_t probe_count[2];
    uint32_t input_direction_tile[2];
    uint32_t input_direction_count;
    uint32_t directions_per_output;
    uint32_t output_atlas_resolution[2];
    uint32_t output_direction_tile[2];
    uint32_t output_direction_count;
    uint32_t padding[3];
} BTBHGPUSceneRadiancePrefilterUniforms;

typedef struct BTBHGPUSceneRadianceDiffuseCacheUniforms {
    uint32_t input_atlas_resolution[2];
    uint32_t probe_count[2];
    uint32_t input_direction_tile[2];
    uint32_t input_direction_count;
    uint32_t padding0;
    uint32_t output_atlas_resolution[2];
    uint32_t output_normal_tile[2];
    uint32_t output_normal_count;
    uint32_t padding1[3];
} BTBHGPUSceneRadianceDiffuseCacheUniforms;

typedef struct BTBHGPUSceneRadianceResolveUniforms {
    uint32_t rc_resolution[2];
    uint32_t render_resolution[2];
    uint32_t atlas_resolution[2];
    uint32_t probe_count[2];
    uint32_t direction_tile[2];
    uint32_t direction_count;
    uint32_t probe_spacing_cells;
    float indirect_strength;
    uint32_t diffuse_cache_enabled;
    uint32_t diffuse_cache_atlas_resolution[2];
    uint32_t diffuse_cache_normal_tile[2];
    uint32_t diffuse_cache_padding[2];
    float inverse_view_projection[16];
    float origin_cell[4];
    uint32_t field_dimensions[2];
    uint32_t debug_view;
    uint32_t padding1;
    float irradiance_sh[9][4];
    float environment_controls[4];
    float hero_omni_shadow_position_range[4];
    float hero_omni_shadow_controls[4];
} BTBHGPUSceneRadianceResolveUniforms;

typedef struct BTBHGPUHRCTraceUniforms {
    uint32_t output_atlas_resolution[2];
    uint32_t grid_dimensions[2];
    uint32_t direction_count;
    uint32_t segment_height;
    uint32_t display_size;
    uint32_t linear_scale;
    float origin_cell[4];
    uint32_t field_dimensions[2];
    uint32_t max_step_count;
    uint32_t padding;
    float environment_radiance[4];
    float directional_direction_energy[4];
    float directional_color_lobe[4];
    float secondary_directional_direction_energy[4];
    float secondary_directional_color_lobe[4];
    float sky_top_energy[4];
    float sky_horizon_curve[4];
} BTBHGPUHRCTraceUniforms;

typedef struct BTBHGPUHRCMergeUpUniforms {
    uint32_t output_atlas_resolution[2];
    uint32_t previous_atlas_resolution[2];
    uint32_t grid_width;
    uint32_t grid_height;
    uint32_t direction_count;
    uint32_t previous_direction_count;
    uint32_t segment_height;
    uint32_t padding0;
    uint32_t padding1[2];
} BTBHGPUHRCMergeUpUniforms;

typedef struct BTBHGPUHRCMergeDownUniforms {
    uint32_t radiance_atlas_resolution[2];
    uint32_t grid_width;
    uint32_t grid_height;
    uint32_t direction_count;
    uint32_t next_grid_width;
    uint32_t next_direction_count;
    uint32_t segment_height;
    uint32_t next_radiance_valid;
    uint32_t padding[3];
} BTBHGPUHRCMergeDownUniforms;

typedef struct BTBHGPUHRCReprojectUniforms {
    uint32_t atlas_resolution[2];
    uint32_t layer_height;
    uint32_t linear_scale;
    int32_t history_offset_pixels[2];
    uint32_t field_dimensions[2];
    float origin_cell[4];
    uint32_t fresh_phase;
    uint32_t padding[3];
} BTBHGPUHRCReprojectUniforms;

typedef struct BTBHGPUHRCFinishUniforms {
    uint32_t output_atlas_resolution[2];
    uint32_t radiance_atlas_resolution[2];
    uint32_t display_size;
    uint32_t base_grid_size;
    uint32_t segment_height;
    uint32_t linear_scale;
    float origin_cell[4];
    uint32_t field_dimensions[2];
    uint32_t max_step_count;
    uint32_t refresh_rotation;
    float environment_radiance[4];
    float directional_direction_energy[4];
    float directional_color_lobe[4];
    float secondary_directional_direction_energy[4];
    float secondary_directional_color_lobe[4];
    float sky_top_energy[4];
    float sky_horizon_curve[4];
} BTBHGPUHRCFinishUniforms;

typedef struct BTBHGPUHRCBlurUniforms {
    uint32_t atlas_resolution[2];
    uint32_t layer_height;
    uint32_t padding;
} BTBHGPUHRCBlurUniforms;

typedef struct BTBHGPUHRCResolveUniforms {
    uint32_t rc_resolution[2];
    uint32_t render_resolution[2];
    uint32_t hrc_resolution[2];
    uint32_t quadrant_atlas_resolution[2];
    float indirect_strength;
    uint32_t debug_view;
    uint32_t padding0[2];
    float inverse_view_projection[16];
    float origin_cell[4];
    uint32_t field_dimensions[2];
    uint32_t padding1[2];
    float irradiance_sh[9][4];
    float environment_controls[4];
    float hero_omni_shadow_position_range[4];
    float hero_omni_shadow_controls[4];
} BTBHGPUHRCResolveUniforms;

typedef struct BTBHGPUHRCReconstructUniforms {
    uint32_t source_resolution[2];
    uint32_t output_resolution[2];
    float inverse_view_projection[16];
    float plane_distance_sigma;
    float normal_power;
    float visibility_sigma;
    float spatial_sigma;
} BTBHGPUHRCReconstructUniforms;

typedef struct BTBHGPUSceneRadianceEmitterGPU {
    float position_radius[4];
    float radiance_height[4];
    float direction_inner[4];
    float outer_shape[4];
} BTBHGPUSceneRadianceEmitterGPU;

typedef struct BTBHGPUSceneRadianceSourceUniforms {
    float origin_cell[4];
    uint32_t dimensions[2];
    uint32_t padding[2];
} BTBHGPUSceneRadianceSourceUniforms;

typedef struct BTBHGPUSceneRadianceDirectionalSourceUniforms {
    float origin_cell[4];
    uint32_t field_dimensions[2];
    uint32_t direction_tile[2];
    uint32_t direction_count;
    uint32_t padding[3];
} BTBHGPUSceneRadianceDirectionalSourceUniforms;

_Static_assert(
    sizeof(BTBHGPUSceneRadianceDirectionalSourceUniforms) == 48u,
    "RC directional-source uniform block must match shader layout"
);

_Static_assert(
    sizeof(BTBHGPUSceneRadianceEmitterGPU) == 64u,
    "RC GPU emitter stride must match StructuredBuffer layout"
);

_Static_assert(
    sizeof(BTBHGPUSceneRadianceTraceUniforms) == 192u,
    "RC trace uniform block must match shader layout"
);
_Static_assert(
    sizeof(BTBHGPUSceneRadianceMergeUniforms) == 112u,
    "RC merge uniform block must match shader layout"
);
_Static_assert(
    sizeof(BTBHGPUSceneRadiancePrefilterUniforms) == 64u,
    "RC prefilter uniform block must match shader layout"
);
_Static_assert(
    sizeof(BTBHGPUSceneRadianceDiffuseCacheUniforms) == 64u,
    "RC diffuse-cache uniform block must match shader layout"
);
_Static_assert(
    offsetof(BTBHGPUSceneRadianceResolveUniforms,
        inverse_view_projection) == 80u,
    "RC resolve inverse view-projection offset must match shader layout"
);
_Static_assert(
    sizeof(BTBHGPUSceneRadianceResolveUniforms) == 368u,
    "RC resolve uniform block must match shader layout"
);
_Static_assert(sizeof(BTBHGPUHRCTraceUniforms) == 176u,
    "HRC trace uniform block must match shader layout");
_Static_assert(sizeof(BTBHGPUHRCMergeUpUniforms) == 48u,
    "HRC merge-up uniform block must match shader layout");
_Static_assert(sizeof(BTBHGPUHRCMergeDownUniforms) == 48u,
    "HRC merge-down uniform block must match shader layout");
_Static_assert(sizeof(BTBHGPUHRCReprojectUniforms) == 64u,
    "HRC reproject uniform block must match shader layout");
_Static_assert(sizeof(BTBHGPUHRCFinishUniforms) == 176u,
    "HRC finish uniform block must match shader layout");
_Static_assert(sizeof(BTBHGPUHRCBlurUniforms) == 16u,
    "HRC blur uniform block must match shader layout");
_Static_assert(sizeof(BTBHGPUHRCResolveUniforms) == 336u,
    "HRC resolve uniform block must match shader layout");
_Static_assert(sizeof(BTBHGPUHRCReconstructUniforms) == 96u,
    "HRC reconstruct uniform block must match shader layout");

typedef struct BTBHGPUSceneRadianceCascadesComposeUniforms {
    uint32_t debug_view;
    uint32_t fallback_reason;
    float indirect_strength;
    float padding;
} BTBHGPUSceneRadianceCascadesComposeUniforms;

static void btbh_gpu_radiance_cascades_message(
    char *message,
    size_t capacity,
    const char *value
) {
    if (message == NULL || capacity == 0u) {
        return;
    }
    (void)snprintf(message, capacity, "%s", value != NULL ? value : "");
}

static SDL_GPUShaderFormat btbh_gpu_radiance_cascades_shader_format(
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

static const char *btbh_gpu_radiance_cascades_entrypoint(
    BTBHGPUShaderFormat format
) {
    return format == BTBH_GPU_SHADER_FORMAT_METALLIB ? "main0" : "main";
}

static bool btbh_gpu_radiance_cascades_sibling_path(
    const char *existing_path,
    const char *filename,
    char path[1024]
) {
    if (existing_path == NULL || filename == NULL || path == NULL) {
        return false;
    }
    const char *separator = strrchr(existing_path, '/');
#if defined(_WIN32)
    const char *backslash = strrchr(existing_path, '\\');
    if (backslash != NULL && (separator == NULL || backslash > separator)) {
        separator = backslash;
    }
#endif
    const size_t directory_length = separator != NULL
        ? (size_t)(separator - existing_path + 1)
        : 0u;
    const size_t filename_length = strlen(filename);
    if (directory_length + filename_length + 1u > 1024u) {
        return false;
    }
    (void)memcpy(path, existing_path, directory_length);
    (void)memcpy(path + directory_length, filename, filename_length + 1u);
    return true;
}

static void *btbh_gpu_radiance_cascades_load_shader(
    const char *path,
    size_t *size,
    char *message,
    size_t message_capacity
) {
    void *bytes = SDL_LoadFile(path, size);
    if (bytes == NULL || size == NULL || *size == 0u) {
        SDL_free(bytes);
        btbh_gpu_radiance_cascades_message(
            message,
            message_capacity,
            path
        );
        return NULL;
    }
    return bytes;
}

static SDL_GPUComputePipeline *
btbh_gpu_radiance_cascades_create_compute_pipeline(
    SDL_GPUDevice *device,
    const char *path,
    BTBHGPUShaderFormat shader_format,
    uint32_t sampler_count,
    bool *shader_loaded,
    char *message,
    size_t message_capacity
) {
    if (shader_loaded != NULL) {
        *shader_loaded = false;
    }
    size_t size = 0u;
    void *bytes = btbh_gpu_radiance_cascades_load_shader(
        path,
        &size,
        message,
        message_capacity
    );
    if (bytes == NULL) {
        return NULL;
    }
    if (shader_loaded != NULL) {
        *shader_loaded = true;
    }
    SDL_GPUComputePipelineCreateInfo info;
    SDL_zero(info);
    info.code = bytes;
    info.code_size = size;
    info.entrypoint =
        btbh_gpu_radiance_cascades_entrypoint(shader_format);
    info.format =
        btbh_gpu_radiance_cascades_shader_format(shader_format);
    info.num_samplers = sampler_count;
    info.num_readwrite_storage_textures = 1u;
    info.num_uniform_buffers = 1u;
    info.threadcount_x = 8u;
    info.threadcount_y = 8u;
    info.threadcount_z = 1u;
    SDL_GPUComputePipeline *pipeline =
        SDL_CreateGPUComputePipeline(device, &info);
    SDL_free(bytes);
    if (pipeline == NULL) {
        btbh_gpu_radiance_cascades_message(
            message,
            message_capacity,
            SDL_GetError()
        );
    }
    return pipeline;
}

static SDL_GPUShader *btbh_gpu_radiance_cascades_create_shader(
    SDL_GPUDevice *device,
    const char *path,
    BTBHGPUShaderFormat shader_format,
    SDL_GPUShaderStage stage,
    uint32_t storage_buffer_count,
    uint32_t sampler_count,
    uint32_t uniform_buffer_count,
    char *message,
    size_t message_capacity
) {
    size_t size = 0u;
    void *bytes = btbh_gpu_radiance_cascades_load_shader(
        path,
        &size,
        message,
        message_capacity
    );
    if (bytes == NULL) {
        return NULL;
    }
    SDL_GPUShaderCreateInfo info;
    SDL_zero(info);
    info.code = bytes;
    info.code_size = size;
    info.entrypoint =
        btbh_gpu_radiance_cascades_entrypoint(shader_format);
    info.format =
        btbh_gpu_radiance_cascades_shader_format(shader_format);
    info.stage = stage;
    info.num_storage_buffers = storage_buffer_count;
    info.num_samplers = sampler_count;
    info.num_uniform_buffers = uniform_buffer_count;
    SDL_GPUShader *shader = SDL_CreateGPUShader(device, &info);
    SDL_free(bytes);
    if (shader == NULL) {
        btbh_gpu_radiance_cascades_message(
            message,
            message_capacity,
            SDL_GetError()
        );
    }
    return shader;
}

static SDL_GPUGraphicsPipeline *
btbh_gpu_radiance_cascades_create_compose_pipeline(
    SDL_GPUDevice *device,
    const char *vertex_path,
    const char *fragment_path,
    BTBHGPUShaderFormat shader_format,
    SDL_GPUTextureFormat color_format,
    bool *shaders_loaded,
    char *message,
    size_t message_capacity
) {
    if (shaders_loaded != NULL) {
        *shaders_loaded = false;
    }
    SDL_GPUShader *vertex = btbh_gpu_radiance_cascades_create_shader(
        device,
        vertex_path,
        shader_format,
        SDL_GPU_SHADERSTAGE_VERTEX,
        0u,
        0u,
        0u,
        message,
        message_capacity
    );
    SDL_GPUShader *fragment = btbh_gpu_radiance_cascades_create_shader(
        device,
        fragment_path,
        shader_format,
        SDL_GPU_SHADERSTAGE_FRAGMENT,
        0u,
        8u,
        1u,
        message,
        message_capacity
    );
    if (vertex == NULL || fragment == NULL) {
        if (fragment != NULL) {
            SDL_ReleaseGPUShader(device, fragment);
        }
        if (vertex != NULL) {
            SDL_ReleaseGPUShader(device, vertex);
        }
        return NULL;
    }
    if (shaders_loaded != NULL) {
        *shaders_loaded = true;
    }
    SDL_GPUColorTargetDescription color_target;
    SDL_zero(color_target);
    color_target.format = color_format;
    SDL_GPUGraphicsPipelineCreateInfo info;
    SDL_zero(info);
    info.vertex_shader = vertex;
    info.fragment_shader = fragment;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.rasterizer_state.front_face =
        SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.target_info.color_target_descriptions = &color_target;
    info.target_info.num_color_targets = 1u;
    SDL_GPUGraphicsPipeline *pipeline =
        SDL_CreateGPUGraphicsPipeline(device, &info);
    SDL_ReleaseGPUShader(device, fragment);
    SDL_ReleaseGPUShader(device, vertex);
    if (pipeline == NULL) {
        btbh_gpu_radiance_cascades_message(
            message,
            message_capacity,
            SDL_GetError()
        );
    }
    return pipeline;
}

static SDL_GPUGraphicsPipeline *
btbh_gpu_radiance_cascades_create_source_pipeline(
    SDL_GPUDevice *device,
    const char *vertex_path,
    const char *fragment_path,
    BTBHGPUShaderFormat shader_format,
    uint32_t fragment_sampler_count,
    bool *shaders_loaded,
    char *message,
    size_t message_capacity
) {
    if (shaders_loaded != NULL) {
        *shaders_loaded = false;
    }
    SDL_GPUShader *vertex = btbh_gpu_radiance_cascades_create_shader(
        device,
        vertex_path,
        shader_format,
        SDL_GPU_SHADERSTAGE_VERTEX,
        1u,
        0u,
        1u,
        message,
        message_capacity
    );
    SDL_GPUShader *fragment = btbh_gpu_radiance_cascades_create_shader(
        device,
        fragment_path,
        shader_format,
        SDL_GPU_SHADERSTAGE_FRAGMENT,
        0u,
        fragment_sampler_count,
        0u,
        message,
        message_capacity
    );
    if (vertex == NULL || fragment == NULL) {
        if (fragment != NULL) {
            SDL_ReleaseGPUShader(device, fragment);
        }
        if (vertex != NULL) {
            SDL_ReleaseGPUShader(device, vertex);
        }
        return NULL;
    }
    if (shaders_loaded != NULL) {
        *shaders_loaded = true;
    }
    SDL_GPUColorTargetDescription target;
    SDL_zero(target);
    target.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    target.blend_state.enable_blend = true;
    target.blend_state.color_write_mask = 0x0fu;
    target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    target.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    SDL_GPUGraphicsPipelineCreateInfo info;
    SDL_zero(info);
    info.vertex_shader = vertex;
    info.fragment_shader = fragment;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.rasterizer_state.front_face =
        SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.target_info.color_target_descriptions = &target;
    info.target_info.num_color_targets = 1u;
    SDL_GPUGraphicsPipeline *pipeline =
        SDL_CreateGPUGraphicsPipeline(device, &info);
    SDL_ReleaseGPUShader(device, fragment);
    SDL_ReleaseGPUShader(device, vertex);
    if (pipeline == NULL) {
        btbh_gpu_radiance_cascades_message(
            message,
            message_capacity,
            SDL_GetError()
        );
    }
    return pipeline;
}

static SDL_GPUGraphicsPipeline *
btbh_gpu_radiance_cascades_create_material_source_pipeline(
    SDL_GPUDevice *device,
    const char *vertex_path,
    const char *fragment_path,
    BTBHGPUShaderFormat shader_format,
    bool animated,
    bool *shaders_loaded,
    char *message,
    size_t message_capacity
) {
    if (shaders_loaded != NULL) {
        *shaders_loaded = false;
    }
    SDL_GPUShader *vertex = btbh_gpu_radiance_cascades_create_shader(
        device,
        vertex_path,
        shader_format,
        SDL_GPU_SHADERSTAGE_VERTEX,
        animated ? 2u : 1u,
        0u,
        1u,
        message,
        message_capacity
    );
    SDL_GPUShader *fragment = btbh_gpu_radiance_cascades_create_shader(
        device,
        fragment_path,
        shader_format,
        SDL_GPU_SHADERSTAGE_FRAGMENT,
        0u,
        2u,
        1u,
        message,
        message_capacity
    );
    if (vertex == NULL || fragment == NULL) {
        if (fragment != NULL) {
            SDL_ReleaseGPUShader(device, fragment);
        }
        if (vertex != NULL) {
            SDL_ReleaseGPUShader(device, vertex);
        }
        return NULL;
    }
    if (shaders_loaded != NULL) {
        *shaders_loaded = true;
    }

    SDL_GPUColorTargetDescription target;
    SDL_GPUVertexBufferDescription vertex_buffer;
    SDL_GPUVertexAttribute attributes[7];
    SDL_GPUGraphicsPipelineCreateInfo info;
    SDL_zero(target);
    SDL_zero(vertex_buffer);
    SDL_zeroa(attributes);
    SDL_zero(info);
    target.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    target.blend_state.enable_blend = true;
    target.blend_state.color_write_mask = 0x0fu;
    target.blend_state.color_blend_op = SDL_GPU_BLENDOP_MAX;
    target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_MAX;
    target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    target.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;

    vertex_buffer.slot = 0u;
    vertex_buffer.pitch = BTBH_GPU_RC_MODEL_VERTEX_STRIDE;
    vertex_buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    attributes[0].location = 0u;
    attributes[0].buffer_slot = 0u;
    attributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    attributes[0].offset = 0u;
    attributes[1].location = 1u;
    attributes[1].buffer_slot = 0u;
    attributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    attributes[1].offset = BTBH_GPU_RC_MODEL_COLOR_OFFSET;
    attributes[2].location = 2u;
    attributes[2].buffer_slot = 0u;
    attributes[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    attributes[2].offset = 24u;
    attributes[3].location = 3u;
    attributes[3].buffer_slot = 0u;
    attributes[3].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    attributes[3].offset = 12u;
    attributes[4].location = 4u;
    attributes[4].buffer_slot = 0u;
    attributes[4].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    attributes[4].offset = animated
        ? BTBH_GPU_RC_MODEL_JOINTS_OFFSET
        : BTBH_GPU_RC_MODEL_TANGENT_OFFSET;
    if (animated) {
        attributes[5].location = 5u;
        attributes[5].buffer_slot = 0u;
        attributes[5].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
        attributes[5].offset = BTBH_GPU_RC_MODEL_WEIGHTS_OFFSET;
        attributes[6].location = 6u;
        attributes[6].buffer_slot = 0u;
        attributes[6].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
        attributes[6].offset = BTBH_GPU_RC_MODEL_TANGENT_OFFSET;
    }

    info.vertex_shader = vertex;
    info.fragment_shader = fragment;
    info.vertex_input_state.vertex_buffer_descriptions = &vertex_buffer;
    info.vertex_input_state.num_vertex_buffers = 1u;
    info.vertex_input_state.vertex_attributes = attributes;
    info.vertex_input_state.num_vertex_attributes = animated ? 7u : 5u;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;
    info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.target_info.color_target_descriptions = &target;
    info.target_info.num_color_targets = 1u;
    SDL_GPUGraphicsPipeline *pipeline =
        SDL_CreateGPUGraphicsPipeline(device, &info);
    SDL_ReleaseGPUShader(device, fragment);
    SDL_ReleaseGPUShader(device, vertex);
    if (pipeline == NULL) {
        btbh_gpu_radiance_cascades_message(
            message,
            message_capacity,
            SDL_GetError()
        );
    }
    return pipeline;
}

static SDL_GPUTexture *btbh_gpu_radiance_cascades_create_texture(
    SDL_GPUDevice *device,
    SDL_GPUTextureFormat format,
    SDL_GPUTextureUsageFlags usage,
    uint32_t width,
    uint32_t height
) {
    SDL_GPUTextureCreateInfo info;
    SDL_zero(info);
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = format;
    info.usage = usage;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1u;
    info.num_levels = 1u;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    return SDL_CreateGPUTexture(device, &info);
}

static SDL_GPUTexture *btbh_gpu_radiance_cascades_create_fallback_cube(
    SDL_GPUDevice *device
) {
    SDL_GPUTextureCreateInfo info;
    SDL_zero(info);
    info.type = SDL_GPU_TEXTURETYPE_CUBE;
    info.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = 1u;
    info.height = 1u;
    info.layer_count_or_depth = 6u;
    info.num_levels = 1u;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    return SDL_CreateGPUTexture(device, &info);
}

static bool btbh_gpu_radiance_cascades_prepare_occlusion_upload(
    SDL_GPUDevice *device,
    BTBHGPUSceneRadianceCascadesRuntime *runtime,
    uint32_t index,
    uint32_t width,
    uint32_t height,
    uint64_t revision,
    const float *samples
) {
    if (device == NULL || runtime == NULL ||
        index >= BTBH_GPU_RC_MAX_OCCLUSION_CASCADES ||
        runtime->occlusion_textures[index] == NULL || samples == NULL) {
        return false;
    }
    const uint64_t byte_count_64 =
        (uint64_t)width * (uint64_t)height * 2u * sizeof(float);
    if (byte_count_64 == 0u || byte_count_64 > UINT32_MAX) {
        return false;
    }
    const uint32_t byte_count = (uint32_t)byte_count_64;
    if (runtime->occlusion_transfer_buffers[index] == NULL ||
        runtime->occlusion_transfer_capacities[index] < byte_count) {
        SDL_GPUTransferBufferCreateInfo transfer_info;
        SDL_zero(transfer_info);
        transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transfer_info.size = byte_count;
        SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(
            device,
            &transfer_info
        );
        if (transfer == NULL) {
            return false;
        }
        if (runtime->occlusion_transfer_buffers[index] != NULL) {
            SDL_ReleaseGPUTransferBuffer(
                device,
                runtime->occlusion_transfer_buffers[index]
            );
        }
        runtime->occlusion_transfer_buffers[index] = transfer;
        runtime->occlusion_transfer_capacities[index] = byte_count;
    }
    void *mapped = SDL_MapGPUTransferBuffer(
        device,
        runtime->occlusion_transfer_buffers[index],
        true
    );
    if (mapped == NULL) {
        return false;
    }
    (void)memcpy(mapped, samples, (size_t)byte_count_64);
    SDL_UnmapGPUTransferBuffer(
        device,
        runtime->occlusion_transfer_buffers[index]
    );
    runtime->occlusion_upload_widths[index] = width;
    runtime->occlusion_upload_heights[index] = height;
    runtime->occlusion_pending_revisions[index] = revision;
    runtime->occlusion_upload_pending_mask |= UINT32_C(1) << index;
    return true;
}

static void btbh_gpu_radiance_cascades_release_occlusion_textures(
    SDL_GPUDevice *device,
    BTBHGPUSceneRadianceCascadesRuntime *runtime
) {
    if (device == NULL || runtime == NULL) {
        return;
    }
    for (uint32_t index = 0u;
         index < BTBH_GPU_RC_MAX_OCCLUSION_CASCADES;
         ++index) {
        if (runtime->occlusion_textures[index] != NULL) {
            SDL_ReleaseGPUTexture(
                device,
                runtime->occlusion_textures[index]
            );
            runtime->occlusion_textures[index] = NULL;
        }
        if (runtime->occlusion_transfer_buffers[index] != NULL) {
            SDL_ReleaseGPUTransferBuffer(
                device,
                runtime->occlusion_transfer_buffers[index]
            );
            runtime->occlusion_transfer_buffers[index] = NULL;
        }
        runtime->occlusion_widths[index] = 0u;
        runtime->occlusion_heights[index] = 0u;
        runtime->occlusion_revisions[index] = 0u;
        runtime->occlusion_transfer_capacities[index] = 0u;
        runtime->occlusion_upload_widths[index] = 0u;
        runtime->occlusion_upload_heights[index] = 0u;
        runtime->occlusion_pending_revisions[index] = 0u;
    }
    runtime->occlusion_upload_pending_mask = 0u;
}

static bool btbh_gpu_radiance_cascades_ensure_occlusion_textures(
    SDL_GPUDevice *device,
    BTBHGPUSceneRadianceCascadesRuntime *runtime,
    const BTBHGPUSceneRadianceCascades *config
) {
    static const float empty_samples[2] = {1000000.0f, 0.0f};
    for (uint32_t index = 0u;
         index < BTBH_GPU_RC_MAX_OCCLUSION_CASCADES;
         ++index) {
        const bool supplied = index < config->occlusion_cascade_count;
        const BTBHGPUSceneRadianceCascadesOcclusionCascade *cascade =
            supplied ? &config->occlusion_cascades[index] : NULL;
        const uint32_t width = supplied ? cascade->width : 1u;
        const uint32_t height = supplied ? cascade->height : 1u;
        const uint64_t revision = supplied ? cascade->revision : 0u;
        const float *samples = supplied ? cascade->samples : empty_samples;
        const bool dimensions_changed =
            runtime->occlusion_widths[index] != width ||
            runtime->occlusion_heights[index] != height;
        if (runtime->occlusion_textures[index] == NULL ||
            dimensions_changed) {
            if (runtime->occlusion_textures[index] != NULL) {
                SDL_ReleaseGPUTexture(
                    device,
                    runtime->occlusion_textures[index]
                );
            }
            runtime->occlusion_textures[index] =
                btbh_gpu_radiance_cascades_create_texture(
                    device,
                    SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT,
                    SDL_GPU_TEXTUREUSAGE_SAMPLER,
                    width,
                    height
                );
            runtime->occlusion_widths[index] = 0u;
            runtime->occlusion_heights[index] = 0u;
            if (runtime->occlusion_textures[index] == NULL) {
                return false;
            }
        }
        if (dimensions_changed ||
            runtime->occlusion_widths[index] == 0u ||
            runtime->occlusion_revisions[index] != revision) {
            if (!btbh_gpu_radiance_cascades_prepare_occlusion_upload(
                    device,
                    runtime,
                    index,
                    width,
                    height,
                    revision,
                    samples
                )) {
                return false;
            }
            runtime->occlusion_widths[index] = width;
            runtime->occlusion_heights[index] = height;
        }
    }
    return true;
}

static bool btbh_gpu_radiance_cascades_ensure_emitter_buffer(
    SDL_GPUDevice *device,
    BTBHGPUSceneRadianceCascadesRuntime *runtime,
    const BTBHGPUSceneRadianceCascades *config
) {
    if (device == NULL || runtime == NULL || config == NULL) {
        return false;
    }
    runtime->emitter_count = 0u;
    runtime->directional_emitter_count = 0u;
    runtime->emitter_upload_size = 0u;
    if (config->emitter_count == 0u) {
        return true;
    }
    const uint64_t upload_size_64 =
        (uint64_t)config->emitter_count *
        sizeof(BTBHGPUSceneRadianceEmitterGPU);
    if (upload_size_64 == 0u || upload_size_64 > UINT32_MAX) {
        return false;
    }

    if (runtime->emitter_buffer == NULL ||
        runtime->emitter_transfer_buffer == NULL ||
        runtime->emitter_capacity < config->emitter_count) {
        uint32_t capacity = runtime->emitter_capacity > 0u
            ? runtime->emitter_capacity
            : 64u;
        while (capacity < config->emitter_count) {
            if (capacity > UINT32_MAX / 2u) {
                capacity = config->emitter_count;
                break;
            }
            capacity *= 2u;
        }
        const uint64_t buffer_size_64 =
            (uint64_t)capacity * sizeof(BTBHGPUSceneRadianceEmitterGPU);
        if (buffer_size_64 == 0u || buffer_size_64 > UINT32_MAX) {
            return false;
        }
        SDL_GPUBufferCreateInfo buffer_info;
        SDL_zero(buffer_info);
        buffer_info.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
        buffer_info.size = (uint32_t)buffer_size_64;
        SDL_GPUBuffer *buffer = SDL_CreateGPUBuffer(device, &buffer_info);
        if (buffer == NULL) {
            return false;
        }
        SDL_GPUTransferBufferCreateInfo transfer_info;
        SDL_zero(transfer_info);
        transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transfer_info.size = (uint32_t)buffer_size_64;
        SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(
            device,
            &transfer_info
        );
        if (transfer == NULL) {
            SDL_ReleaseGPUBuffer(device, buffer);
            return false;
        }
        if (runtime->emitter_buffer != NULL) {
            SDL_ReleaseGPUBuffer(device, runtime->emitter_buffer);
        }
        if (runtime->emitter_transfer_buffer != NULL) {
            SDL_ReleaseGPUTransferBuffer(
                device,
                runtime->emitter_transfer_buffer
            );
        }
        runtime->emitter_buffer = buffer;
        runtime->emitter_transfer_buffer = transfer;
        runtime->emitter_capacity = capacity;
    }

    BTBHGPUSceneRadianceEmitterGPU *mapped =
        (BTBHGPUSceneRadianceEmitterGPU *)SDL_MapGPUTransferBuffer(
            device,
            runtime->emitter_transfer_buffer,
            true
        );
    if (mapped == NULL) {
        return false;
    }
    const uint32_t directional_count =
        btbh_gpu_radiance_cascades_directional_emitter_count(config);
    uint32_t directional_cursor = 0u;
    uint32_t omni_cursor = directional_count;
    for (uint32_t index = 0u; index < config->emitter_count; ++index) {
        const BTBHGPUSceneRadianceEmitter *source = &config->emitters[index];
        const uint32_t destination_index =
            btbh_gpu_radiance_emitter_is_directional(source)
            ? directional_cursor++
            : omni_cursor++;
        BTBHGPUSceneRadianceEmitterGPU *destination =
            &mapped[destination_index];
        (void)memset(destination, 0, sizeof(*destination));
        destination->position_radius[0] = source->position_x;
        destination->position_radius[1] = source->position_y;
        destination->position_radius[2] = source->position_z;
        destination->position_radius[3] = source->radius;
        destination->radiance_height[0] = source->red * source->energy;
        destination->radiance_height[1] = source->green * source->energy;
        destination->radiance_height[2] = source->blue * source->energy;
        destination->radiance_height[3] = source->height;
        destination->direction_inner[0] = source->direction_x;
        destination->direction_inner[1] = source->direction_y;
        destination->direction_inner[2] = source->direction_z;
        destination->direction_inner[3] = source->inner_cutoff;
        destination->outer_shape[0] = source->outer_cutoff;
        destination->outer_shape[1] = (float)source->shape;
        destination->outer_shape[2] =
            source->direct_energy / source->energy;
        destination->outer_shape[3] = (float)source->light_class;
    }
    SDL_UnmapGPUTransferBuffer(device, runtime->emitter_transfer_buffer);
    runtime->emitter_count = config->emitter_count;
    runtime->directional_emitter_count = directional_count;
    runtime->emitter_upload_size = (uint32_t)upload_size_64;
    return true;
}

void btbh_gpu_radiance_cascades_runtime_initialize(
    BTBHGPUSceneRadianceCascadesRuntime *runtime
) {
    if (runtime == NULL) {
        return;
    }
    (void)memset(runtime, 0, sizeof(*runtime));
    btbh_gpu_radiance_cascades_state_initialize(&runtime->state);
}

static void btbh_gpu_radiance_cascades_release_resources(
    SDL_GPUDevice *device,
    BTBHGPUSceneRadianceCascadesRuntime *runtime
) {
    if (device == NULL || runtime == NULL) {
        return;
    }
    if (runtime->composed_texture != NULL) {
        SDL_ReleaseGPUTexture(device, runtime->composed_texture);
        runtime->composed_texture = NULL;
    }
    if (runtime->indirect_texture != NULL) {
        SDL_ReleaseGPUTexture(device, runtime->indirect_texture);
        runtime->indirect_texture = NULL;
    }
    if (runtime->hrc_reconstructed_texture != NULL) {
        SDL_ReleaseGPUTexture(
            device,
            runtime->hrc_reconstructed_texture
        );
        runtime->hrc_reconstructed_texture = NULL;
    }
    if (runtime->source_field_texture != NULL) {
        SDL_ReleaseGPUTexture(device, runtime->source_field_texture);
        runtime->source_field_texture = NULL;
    }
    if (runtime->directional_source_texture != NULL) {
        SDL_ReleaseGPUTexture(
            device,
            runtime->directional_source_texture
        );
        runtime->directional_source_texture = NULL;
    }
    if (runtime->direct_light_texture != NULL) {
        SDL_ReleaseGPUTexture(device, runtime->direct_light_texture);
        runtime->direct_light_texture = NULL;
    }
    if (runtime->fallback_cube_texture != NULL) {
        SDL_ReleaseGPUTexture(device, runtime->fallback_cube_texture);
        runtime->fallback_cube_texture = NULL;
    }
    if (runtime->prefiltered_cascade_texture != NULL) {
        SDL_ReleaseGPUTexture(
            device,
            runtime->prefiltered_cascade_texture
        );
        runtime->prefiltered_cascade_texture = NULL;
    }
    if (runtime->diffuse_cache_texture != NULL) {
        SDL_ReleaseGPUTexture(device, runtime->diffuse_cache_texture);
        runtime->diffuse_cache_texture = NULL;
    }
    for (uint32_t index = 0u;
         index < BTBH_GPU_RC_CASCADE_COUNT;
         ++index) {
        if (runtime->raw_cascade_textures[index] != NULL) {
            SDL_ReleaseGPUTexture(
                device,
                runtime->raw_cascade_textures[index]
            );
            runtime->raw_cascade_textures[index] = NULL;
        }
        if (runtime->cascade_textures[index] != NULL) {
            SDL_ReleaseGPUTexture(
                device,
                runtime->cascade_textures[index]
            );
            runtime->cascade_textures[index] = NULL;
        }
    }
    for (uint32_t index = 0u;
         index < BTBH_GPU_HRC_MAX_LEVELS;
         ++index) {
        if (runtime->hrc_acceleration_textures[index] != NULL) {
            SDL_ReleaseGPUTexture(
                device,
                runtime->hrc_acceleration_textures[index]
            );
            runtime->hrc_acceleration_textures[index] = NULL;
        }
    }
    for (uint32_t index = 0u; index < 2u; ++index) {
        if (runtime->hrc_radiance_textures[index] != NULL) {
            SDL_ReleaseGPUTexture(
                device,
                runtime->hrc_radiance_textures[index]
            );
            runtime->hrc_radiance_textures[index] = NULL;
        }
    }
    for (uint32_t index = 0u; index < 3u; ++index) {
        if (runtime->hrc_quadrant_textures[index] != NULL) {
            SDL_ReleaseGPUTexture(
                device,
                runtime->hrc_quadrant_textures[index]
            );
            runtime->hrc_quadrant_textures[index] = NULL;
        }
    }
    (void)memset(&runtime->hrc_layout, 0, sizeof(runtime->hrc_layout));
    runtime->hrc_history_generation = 0u;
    runtime->hrc_temporal_phase = 0u;
    runtime->hrc_history_read_index = 1u;
    runtime->hrc_history_origin_x = 0.0f;
    runtime->hrc_history_origin_z = 0.0f;
    runtime->hrc_history_cell_size = 0.0f;
    runtime->hrc_history_valid = false;
    runtime->resource_backend = BTBH_GPU_SCENE_RC_BACKEND_CLASSIC;
    runtime->cascade_count = 0u;
    (void)memset(&runtime->profile, 0, sizeof(runtime->profile));
    (void)memset(
        runtime->cascade_layouts,
        0,
        sizeof(runtime->cascade_layouts)
    );
    runtime->internal_width = 0u;
    runtime->internal_height = 0u;
    runtime->source_field_width = 0u;
    runtime->source_field_height = 0u;
    runtime->resolve_atlas_width = 0u;
    runtime->resolve_atlas_height = 0u;
    runtime->resolve_direction_tile_width = 0u;
    runtime->resolve_direction_tile_height = 0u;
    runtime->resolve_direction_count = 0u;
    runtime->diffuse_cache_atlas_width = 0u;
    runtime->diffuse_cache_atlas_height = 0u;
}

void btbh_gpu_radiance_cascades_runtime_release(
    SDL_GPUDevice *device,
    BTBHGPUSceneRadianceCascadesRuntime *runtime
) {
    if (device == NULL || runtime == NULL) {
        return;
    }
    btbh_gpu_radiance_cascades_release_resources(device, runtime);
    btbh_gpu_radiance_cascades_release_occlusion_textures(
        device,
        runtime
    );
    if (runtime->sampler != NULL) {
        SDL_ReleaseGPUSampler(device, runtime->sampler);
        runtime->sampler = NULL;
    }
    if (runtime->emitter_buffer != NULL) {
        SDL_ReleaseGPUBuffer(device, runtime->emitter_buffer);
        runtime->emitter_buffer = NULL;
    }
    if (runtime->emitter_transfer_buffer != NULL) {
        SDL_ReleaseGPUTransferBuffer(
            device,
            runtime->emitter_transfer_buffer
        );
        runtime->emitter_transfer_buffer = NULL;
    }
    runtime->emitter_capacity = 0u;
    runtime->emitter_count = 0u;
    runtime->directional_emitter_count = 0u;
    runtime->emitter_upload_size = 0u;
    if (runtime->compose_pipeline != NULL) {
        SDL_ReleaseGPUGraphicsPipeline(
            device,
            runtime->compose_pipeline
        );
        runtime->compose_pipeline = NULL;
    }
    if (runtime->source_pipeline != NULL) {
        SDL_ReleaseGPUGraphicsPipeline(
            device,
            runtime->source_pipeline
        );
        runtime->source_pipeline = NULL;
    }
    if (runtime->directional_source_pipeline != NULL) {
        SDL_ReleaseGPUGraphicsPipeline(
            device,
            runtime->directional_source_pipeline
        );
        runtime->directional_source_pipeline = NULL;
    }
    if (runtime->direct_light_pipeline != NULL) {
        SDL_ReleaseGPUGraphicsPipeline(
            device,
            runtime->direct_light_pipeline
        );
        runtime->direct_light_pipeline = NULL;
    }
    for (uint32_t index = 0u; index < 2u; ++index) {
        if (runtime->material_source_pipelines[index] != NULL) {
            SDL_ReleaseGPUGraphicsPipeline(
                device,
                runtime->material_source_pipelines[index]
            );
            runtime->material_source_pipelines[index] = NULL;
        }
    }
    if (runtime->resolve_pipeline != NULL) {
        SDL_ReleaseGPUComputePipeline(device, runtime->resolve_pipeline);
        runtime->resolve_pipeline = NULL;
    }
    if (runtime->prefilter_pipeline != NULL) {
        SDL_ReleaseGPUComputePipeline(device, runtime->prefilter_pipeline);
        runtime->prefilter_pipeline = NULL;
    }
    if (runtime->diffuse_cache_pipeline != NULL) {
        SDL_ReleaseGPUComputePipeline(
            device,
            runtime->diffuse_cache_pipeline
        );
        runtime->diffuse_cache_pipeline = NULL;
    }
    if (runtime->trace_pipeline != NULL) {
        SDL_ReleaseGPUComputePipeline(device, runtime->trace_pipeline);
        runtime->trace_pipeline = NULL;
    }
    if (runtime->merge_pipeline != NULL) {
        SDL_ReleaseGPUComputePipeline(device, runtime->merge_pipeline);
        runtime->merge_pipeline = NULL;
    }
    SDL_GPUComputePipeline **hrc_pipelines[] = {
        &runtime->hrc_trace_pipeline,
        &runtime->hrc_merge_up_pipeline,
        &runtime->hrc_merge_down_pipeline,
        &runtime->hrc_reproject_pipeline,
        &runtime->hrc_finish_pipeline,
        &runtime->hrc_blur_pipeline,
        &runtime->hrc_resolve_pipeline,
        &runtime->hrc_reconstruct_pipeline
    };
    for (uint32_t index = 0u;
         index < (uint32_t)(sizeof(hrc_pipelines) / sizeof(hrc_pipelines[0]));
         ++index) {
        if (*hrc_pipelines[index] != NULL) {
            SDL_ReleaseGPUComputePipeline(device, *hrc_pipelines[index]);
            *hrc_pipelines[index] = NULL;
        }
    }
    runtime->render_width = 0u;
    runtime->render_height = 0u;
}

static bool btbh_gpu_radiance_cascades_ensure_pipelines(
    SDL_GPUDevice *device,
    BTBHGPUSceneRadianceCascadesRuntime *runtime,
    const char *post_vertex_shader_path,
    const char *post_fragment_shader_path,
    BTBHGPUShaderFormat shader_format,
    SDL_GPUTextureFormat scene_color_format,
    BTBHGPUSceneRadianceCascadesFallbackReason *failure_reason,
    char *message,
    size_t message_capacity
) {
    if (runtime->trace_pipeline != NULL &&
        runtime->merge_pipeline != NULL &&
        runtime->prefilter_pipeline != NULL &&
        runtime->diffuse_cache_pipeline != NULL &&
        runtime->resolve_pipeline != NULL &&
        runtime->source_pipeline != NULL &&
        runtime->directional_source_pipeline != NULL &&
        runtime->direct_light_pipeline != NULL &&
        runtime->material_source_pipelines[0] != NULL &&
        runtime->material_source_pipelines[1] != NULL &&
        runtime->compose_pipeline != NULL &&
        runtime->sampler != NULL) {
        return true;
    }
    const char *extension =
        shader_format == BTBH_GPU_SHADER_FORMAT_METALLIB
            ? "metallib"
            : "spv";
    char filename[128];
    char path[1024];
    bool shader_loaded = false;
    (void)snprintf(
        filename,
        sizeof(filename),
        "production_rc_compose.frag.%s",
        extension
    );
    if (!btbh_gpu_radiance_cascades_sibling_path(
            post_fragment_shader_path,
            filename,
            path
        )) {
        *failure_reason = BTBH_GPU_SCENE_RC_FALLBACK_SHADER_UNAVAILABLE;
        return false;
    }
    if (runtime->compose_pipeline == NULL) {
        runtime->compose_pipeline =
            btbh_gpu_radiance_cascades_create_compose_pipeline(
                device,
                post_vertex_shader_path,
                path,
                shader_format,
                scene_color_format,
                &shader_loaded,
                message,
                message_capacity
            );
        if (runtime->compose_pipeline == NULL) {
            *failure_reason = shader_loaded
                ? BTBH_GPU_SCENE_RC_FALLBACK_PIPELINE_CREATION_FAILED
                : BTBH_GPU_SCENE_RC_FALLBACK_SHADER_UNAVAILABLE;
            return false;
        }
    }
    SDL_GPUSamplerCreateInfo sampler_info;
    SDL_zero(sampler_info);
    sampler_info.min_filter = SDL_GPU_FILTER_LINEAR;
    sampler_info.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampler_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    if (runtime->sampler == NULL) {
        runtime->sampler = SDL_CreateGPUSampler(device, &sampler_info);
    }
    if (runtime->sampler == NULL) {
        btbh_gpu_radiance_cascades_message(
            message,
            message_capacity,
            SDL_GetError()
        );
        *failure_reason =
            BTBH_GPU_SCENE_RC_FALLBACK_PIPELINE_CREATION_FAILED;
        return false;
    }
    char source_vertex_path[1024];
    char source_fragment_path[1024];
    (void)snprintf(
        filename,
        sizeof(filename),
        "production_rc_source.vert.%s",
        extension
    );
    if (!btbh_gpu_radiance_cascades_sibling_path(
            post_vertex_shader_path,
            filename,
            source_vertex_path
        )) {
        *failure_reason = BTBH_GPU_SCENE_RC_FALLBACK_SHADER_UNAVAILABLE;
        return false;
    }
    (void)snprintf(
        filename,
        sizeof(filename),
        "production_rc_source.frag.%s",
        extension
    );
    if (!btbh_gpu_radiance_cascades_sibling_path(
            post_fragment_shader_path,
            filename,
            source_fragment_path
        )) {
        *failure_reason = BTBH_GPU_SCENE_RC_FALLBACK_SHADER_UNAVAILABLE;
        return false;
    }
    if (runtime->source_pipeline == NULL) {
        runtime->source_pipeline =
            btbh_gpu_radiance_cascades_create_source_pipeline(
                device,
                source_vertex_path,
                source_fragment_path,
                shader_format,
                0u,
                &shader_loaded,
                message,
                message_capacity
            );
        if (runtime->source_pipeline == NULL) {
            *failure_reason = shader_loaded
                ? BTBH_GPU_SCENE_RC_FALLBACK_PIPELINE_CREATION_FAILED
                : BTBH_GPU_SCENE_RC_FALLBACK_SHADER_UNAVAILABLE;
            return false;
        }
    }
    char directional_source_vertex_path[1024];
    char directional_source_fragment_path[1024];
    (void)snprintf(
        filename,
        sizeof(filename),
        "production_rc_directional_source.vert.%s",
        extension
    );
    if (!btbh_gpu_radiance_cascades_sibling_path(
            post_vertex_shader_path,
            filename,
            directional_source_vertex_path
        )) {
        *failure_reason = BTBH_GPU_SCENE_RC_FALLBACK_SHADER_UNAVAILABLE;
        return false;
    }
    (void)snprintf(
        filename,
        sizeof(filename),
        "production_rc_directional_source.frag.%s",
        extension
    );
    if (!btbh_gpu_radiance_cascades_sibling_path(
            post_fragment_shader_path,
            filename,
            directional_source_fragment_path
        )) {
        *failure_reason = BTBH_GPU_SCENE_RC_FALLBACK_SHADER_UNAVAILABLE;
        return false;
    }
    if (runtime->directional_source_pipeline == NULL) {
        runtime->directional_source_pipeline =
            btbh_gpu_radiance_cascades_create_source_pipeline(
                device,
                directional_source_vertex_path,
                directional_source_fragment_path,
                shader_format,
                0u,
                &shader_loaded,
                message,
                message_capacity
            );
        if (runtime->directional_source_pipeline == NULL) {
            *failure_reason = shader_loaded
                ? BTBH_GPU_SCENE_RC_FALLBACK_PIPELINE_CREATION_FAILED
                : BTBH_GPU_SCENE_RC_FALLBACK_SHADER_UNAVAILABLE;
            return false;
        }
    }
    char direct_light_vertex_path[1024];
    char direct_light_fragment_path[1024];
    (void)snprintf(
        filename,
        sizeof(filename),
        "production_rc_direct_light.vert.%s",
        extension
    );
    if (!btbh_gpu_radiance_cascades_sibling_path(
            post_vertex_shader_path,
            filename,
            direct_light_vertex_path
        )) {
        *failure_reason = BTBH_GPU_SCENE_RC_FALLBACK_SHADER_UNAVAILABLE;
        return false;
    }
    (void)snprintf(
        filename,
        sizeof(filename),
        "production_rc_direct_light.frag.%s",
        extension
    );
    if (!btbh_gpu_radiance_cascades_sibling_path(
            post_fragment_shader_path,
            filename,
            direct_light_fragment_path
        )) {
        *failure_reason = BTBH_GPU_SCENE_RC_FALLBACK_SHADER_UNAVAILABLE;
        return false;
    }
    if (runtime->direct_light_pipeline == NULL) {
        runtime->direct_light_pipeline =
            btbh_gpu_radiance_cascades_create_source_pipeline(
                device,
                direct_light_vertex_path,
                direct_light_fragment_path,
                shader_format,
                1u,
                &shader_loaded,
                message,
                message_capacity
            );
        if (runtime->direct_light_pipeline == NULL) {
            *failure_reason = shader_loaded
                ? BTBH_GPU_SCENE_RC_FALLBACK_PIPELINE_CREATION_FAILED
                : BTBH_GPU_SCENE_RC_FALLBACK_SHADER_UNAVAILABLE;
            return false;
        }
    }
    char material_source_fragment_path[1024];
    (void)snprintf(
        filename,
        sizeof(filename),
        "production_rc_material_source.frag.%s",
        extension
    );
    if (!btbh_gpu_radiance_cascades_sibling_path(
            post_fragment_shader_path,
            filename,
            material_source_fragment_path
        )) {
        *failure_reason = BTBH_GPU_SCENE_RC_FALLBACK_SHADER_UNAVAILABLE;
        return false;
    }
    for (uint32_t animated = 0u; animated < 2u; ++animated) {
        if (runtime->material_source_pipelines[animated] != NULL) {
            continue;
        }
        (void)snprintf(
            filename,
            sizeof(filename),
            animated != 0u
                ? "production_rc_animated_material_source.vert.%s"
                : "production_rc_material_source.vert.%s",
            extension
        );
        if (!btbh_gpu_radiance_cascades_sibling_path(
                post_vertex_shader_path,
                filename,
                path
            )) {
            *failure_reason =
                BTBH_GPU_SCENE_RC_FALLBACK_SHADER_UNAVAILABLE;
            return false;
        }
        runtime->material_source_pipelines[animated] =
            btbh_gpu_radiance_cascades_create_material_source_pipeline(
                device,
                path,
                material_source_fragment_path,
                shader_format,
                animated != 0u,
                &shader_loaded,
                message,
                message_capacity
            );
        if (runtime->material_source_pipelines[animated] == NULL) {
            *failure_reason = shader_loaded
                ? BTBH_GPU_SCENE_RC_FALLBACK_PIPELINE_CREATION_FAILED
                : BTBH_GPU_SCENE_RC_FALLBACK_SHADER_UNAVAILABLE;
            return false;
        }
    }
    (void)snprintf(
        filename,
        sizeof(filename),
        "production_rc_trace.comp.%s",
        extension
    );
    if (!btbh_gpu_radiance_cascades_sibling_path(
            post_fragment_shader_path,
            filename,
            path
        )) {
        *failure_reason = BTBH_GPU_SCENE_RC_FALLBACK_SHADER_UNAVAILABLE;
        return false;
    }
    if (runtime->trace_pipeline == NULL) {
        runtime->trace_pipeline =
            btbh_gpu_radiance_cascades_create_compute_pipeline(
            device,
            path,
            shader_format,
            3u,
            &shader_loaded,
            message,
            message_capacity
        );
        if (runtime->trace_pipeline == NULL) {
            *failure_reason = shader_loaded
                ? BTBH_GPU_SCENE_RC_FALLBACK_PIPELINE_CREATION_FAILED
                : BTBH_GPU_SCENE_RC_FALLBACK_SHADER_UNAVAILABLE;
            return false;
        }
    }
    (void)snprintf(
        filename,
        sizeof(filename),
        "production_rc_merge.comp.%s",
        extension
    );
    if (!btbh_gpu_radiance_cascades_sibling_path(
            post_fragment_shader_path,
            filename,
            path
        )) {
        *failure_reason = BTBH_GPU_SCENE_RC_FALLBACK_SHADER_UNAVAILABLE;
        return false;
    }
    if (runtime->merge_pipeline == NULL) {
        runtime->merge_pipeline =
            btbh_gpu_radiance_cascades_create_compute_pipeline(
                device,
            path,
            shader_format,
            5u,
                &shader_loaded,
                message,
                message_capacity
            );
        if (runtime->merge_pipeline == NULL) {
            *failure_reason = shader_loaded
                ? BTBH_GPU_SCENE_RC_FALLBACK_PIPELINE_CREATION_FAILED
                : BTBH_GPU_SCENE_RC_FALLBACK_SHADER_UNAVAILABLE;
            return false;
        }
    }
    (void)snprintf(
        filename,
        sizeof(filename),
        "production_rc_prefilter.comp.%s",
        extension
    );
    if (!btbh_gpu_radiance_cascades_sibling_path(
            post_fragment_shader_path,
            filename,
            path
        )) {
        *failure_reason = BTBH_GPU_SCENE_RC_FALLBACK_SHADER_UNAVAILABLE;
        return false;
    }
    if (runtime->prefilter_pipeline == NULL) {
        runtime->prefilter_pipeline =
            btbh_gpu_radiance_cascades_create_compute_pipeline(
                device,
                path,
                shader_format,
                1u,
                &shader_loaded,
                message,
                message_capacity
            );
        if (runtime->prefilter_pipeline == NULL) {
            *failure_reason = shader_loaded
                ? BTBH_GPU_SCENE_RC_FALLBACK_PIPELINE_CREATION_FAILED
                : BTBH_GPU_SCENE_RC_FALLBACK_SHADER_UNAVAILABLE;
            return false;
        }
    }
    (void)snprintf(
        filename,
        sizeof(filename),
        "production_rc_diffuse_cache.comp.%s",
        extension
    );
    if (!btbh_gpu_radiance_cascades_sibling_path(
            post_fragment_shader_path,
            filename,
            path
        )) {
        *failure_reason = BTBH_GPU_SCENE_RC_FALLBACK_SHADER_UNAVAILABLE;
        return false;
    }
    if (runtime->diffuse_cache_pipeline == NULL) {
        runtime->diffuse_cache_pipeline =
            btbh_gpu_radiance_cascades_create_compute_pipeline(
                device,
                path,
                shader_format,
                1u,
                &shader_loaded,
                message,
                message_capacity
            );
        if (runtime->diffuse_cache_pipeline == NULL) {
            *failure_reason = shader_loaded
                ? BTBH_GPU_SCENE_RC_FALLBACK_PIPELINE_CREATION_FAILED
                : BTBH_GPU_SCENE_RC_FALLBACK_SHADER_UNAVAILABLE;
            return false;
        }
    }
    (void)snprintf(
        filename,
        sizeof(filename),
        "production_rc_resolve.comp.%s",
        extension
    );
    if (!btbh_gpu_radiance_cascades_sibling_path(
            post_fragment_shader_path,
            filename,
            path
        )) {
        *failure_reason = BTBH_GPU_SCENE_RC_FALLBACK_SHADER_UNAVAILABLE;
        return false;
    }
    if (runtime->resolve_pipeline == NULL) {
        runtime->resolve_pipeline =
            btbh_gpu_radiance_cascades_create_compute_pipeline(
            device,
            path,
            shader_format,
            9u,
            &shader_loaded,
            message,
            message_capacity
        );
        if (runtime->resolve_pipeline == NULL) {
            *failure_reason = shader_loaded
                ? BTBH_GPU_SCENE_RC_FALLBACK_PIPELINE_CREATION_FAILED
                : BTBH_GPU_SCENE_RC_FALLBACK_SHADER_UNAVAILABLE;
            return false;
        }
    }
    return true;
}

static bool btbh_gpu_radiance_cascades_ensure_hrc_pipelines(
    SDL_GPUDevice *device,
    BTBHGPUSceneRadianceCascadesRuntime *runtime,
    const char *post_fragment_shader_path,
    BTBHGPUShaderFormat shader_format,
    char *message,
    size_t message_capacity
) {
    if (device == NULL || runtime == NULL ||
        post_fragment_shader_path == NULL) {
        return false;
    }
    SDL_GPUComputePipeline **pipelines[] = {
        &runtime->hrc_trace_pipeline,
        &runtime->hrc_merge_up_pipeline,
        &runtime->hrc_merge_down_pipeline,
        &runtime->hrc_reproject_pipeline,
        &runtime->hrc_finish_pipeline,
        &runtime->hrc_blur_pipeline,
        &runtime->hrc_resolve_pipeline,
        &runtime->hrc_reconstruct_pipeline
    };
    static const char *shader_names[] = {
        "production_hrc_trace.comp",
        "production_hrc_merge_up.comp",
        "production_hrc_merge_down.comp",
        "production_hrc_reproject.comp",
        "production_hrc_finish.comp",
        "production_hrc_blur.comp",
        "production_hrc_resolve.comp",
        "production_hrc_reconstruct.comp"
    };
    static const uint32_t sampler_counts[] = {
        3u, 1u, 3u, 3u, 4u, 1u, 8u, 3u
    };
    const char *extension =
        shader_format == BTBH_GPU_SHADER_FORMAT_METALLIB
            ? "metallib"
            : "spv";
    for (uint32_t index = 0u;
         index < (uint32_t)(sizeof(pipelines) / sizeof(pipelines[0]));
         ++index) {
        if (*pipelines[index] != NULL) {
            continue;
        }
        char filename[128];
        char path[1024];
        (void)snprintf(
            filename,
            sizeof(filename),
            "%s.%s",
            shader_names[index],
            extension
        );
        if (!btbh_gpu_radiance_cascades_sibling_path(
                post_fragment_shader_path,
                filename,
                path)) {
            return false;
        }
        bool shader_loaded = false;
        *pipelines[index] =
            btbh_gpu_radiance_cascades_create_compute_pipeline(
                device,
                path,
                shader_format,
                sampler_counts[index],
                &shader_loaded,
                message,
                message_capacity
            );
        if (*pipelines[index] == NULL) {
            return false;
        }
    }
    return true;
}

static bool btbh_gpu_radiance_cascades_ensure_textures(
    SDL_GPUDevice *device,
    BTBHGPUSceneRadianceCascadesRuntime *runtime,
    BTBHGPUSceneRadianceCascadesBackend backend,
    const BTBHGPUHolographicRadianceCascadesLayout *hrc_layout,
    const BTBHGPUSceneRadianceCascadesProfile *profile,
    const BTBHGPUSceneRadianceCascadeLayout *layouts,
    uint32_t cascade_count,
    SDL_GPUTextureFormat scene_color_format,
    uint32_t internal_width,
    uint32_t internal_height,
    uint32_t source_field_width,
    uint32_t source_field_height,
    uint32_t render_width,
    uint32_t render_height,
    char *message,
    size_t message_capacity
) {
    if (profile == NULL || layouts == NULL || cascade_count == 0u ||
        cascade_count > BTBH_GPU_RC_CASCADE_COUNT) {
        return false;
    }
    const bool holographic =
        backend == BTBH_GPU_SCENE_RC_BACKEND_HOLOGRAPHIC;
    if ((backend != BTBH_GPU_SCENE_RC_BACKEND_CLASSIC && !holographic) ||
        (holographic && (hrc_layout == NULL ||
            hrc_layout->level_count == 0u ||
            hrc_layout->level_count > BTBH_GPU_HRC_MAX_LEVELS))) {
        return false;
    }
    const bool angular_prefilter_enabled =
        !holographic && layouts[0].direction_count >
            BTBH_GPU_RC_HIGH_RESOLVE_DIRECTION_COUNT;
    const uint32_t resolve_direction_tile_width =
        angular_prefilter_enabled
            ? BTBH_GPU_RC_HIGH_RESOLVE_DIRECTION_TILE_WIDTH
            : layouts[0].direction_tile_width;
    const uint32_t resolve_direction_tile_height =
        angular_prefilter_enabled
            ? BTBH_GPU_RC_HIGH_RESOLVE_DIRECTION_TILE_HEIGHT
            : layouts[0].direction_tile_height;
    const uint32_t resolve_direction_count = angular_prefilter_enabled
        ? BTBH_GPU_RC_HIGH_RESOLVE_DIRECTION_COUNT
        : layouts[0].direction_count;
    const bool diffuse_cache_enabled =
        !holographic &&
        layouts[0].direction_count > resolve_direction_count;
    uint32_t resolve_atlas_width = 0u;
    uint32_t resolve_atlas_height = 0u;
    if (!btbh_gpu_radiance_cascades_multiply_u32(
            layouts[0].probe_count_x,
            resolve_direction_tile_width,
            &resolve_atlas_width
        ) ||
        !btbh_gpu_radiance_cascades_multiply_u32(
            layouts[0].probe_count_y,
            resolve_direction_tile_height,
            &resolve_atlas_height
        )) {
        return false;
    }
    uint32_t diffuse_cache_atlas_width = 0u;
    uint32_t diffuse_cache_atlas_height = 0u;
    if (diffuse_cache_enabled &&
        (!btbh_gpu_radiance_cascades_multiply_u32(
            layouts[0].probe_count_x,
            BTBH_GPU_RC_DIFFUSE_CACHE_NORMAL_TILE_WIDTH,
            &diffuse_cache_atlas_width
        ) ||
         !btbh_gpu_radiance_cascades_multiply_u32(
            layouts[0].probe_count_y,
            BTBH_GPU_RC_DIFFUSE_CACHE_NORMAL_TILE_HEIGHT,
            &diffuse_cache_atlas_height
        ))) {
        return false;
    }
    bool backend_resources_ready =
        runtime->resource_backend == backend;
    if (backend_resources_ready && holographic) {
        backend_resources_ready =
            memcmp(
                &runtime->hrc_layout,
                hrc_layout,
                sizeof(*hrc_layout)
            ) == 0 &&
            runtime->hrc_radiance_textures[0] != NULL &&
            runtime->hrc_radiance_textures[1] != NULL &&
            runtime->hrc_quadrant_textures[0] != NULL &&
            runtime->hrc_quadrant_textures[1] != NULL &&
            runtime->hrc_quadrant_textures[2] != NULL &&
            runtime->hrc_reconstructed_texture != NULL;
        for (uint32_t index = 0u;
             backend_resources_ready && index < hrc_layout->level_count;
             ++index) {
            backend_resources_ready =
                runtime->hrc_acceleration_textures[index] != NULL;
        }
    } else if (backend_resources_ready) {
        for (uint32_t index = 0u;
             backend_resources_ready && index < cascade_count;
             ++index) {
            backend_resources_ready =
                runtime->raw_cascade_textures[index] != NULL &&
                runtime->cascade_textures[index] != NULL;
        }
    }
    if (runtime->indirect_texture != NULL &&
        runtime->composed_texture != NULL &&
        runtime->source_field_texture != NULL &&
        runtime->directional_source_texture != NULL &&
        runtime->direct_light_texture != NULL &&
        runtime->fallback_cube_texture != NULL &&
        (!angular_prefilter_enabled ||
         runtime->prefiltered_cascade_texture != NULL) &&
        (!diffuse_cache_enabled ||
         runtime->diffuse_cache_texture != NULL) &&
        backend_resources_ready &&
        runtime->cascade_count == cascade_count &&
        memcmp(&runtime->profile, profile, sizeof(*profile)) == 0 &&
        memcmp(
            runtime->cascade_layouts,
            layouts,
            sizeof(*layouts) * cascade_count
        ) == 0 &&
        runtime->internal_width == internal_width &&
        runtime->internal_height == internal_height &&
        runtime->source_field_width == source_field_width &&
        runtime->source_field_height == source_field_height &&
        runtime->resolve_atlas_width == resolve_atlas_width &&
        runtime->resolve_atlas_height == resolve_atlas_height &&
        runtime->resolve_direction_count == resolve_direction_count &&
        runtime->diffuse_cache_atlas_width ==
            diffuse_cache_atlas_width &&
        runtime->diffuse_cache_atlas_height ==
            diffuse_cache_atlas_height &&
        runtime->render_width == render_width &&
        runtime->render_height == render_height) {
        return true;
    }
    btbh_gpu_radiance_cascades_release_resources(device, runtime);
    const SDL_GPUTextureUsageFlags compute_usage =
        SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE |
        SDL_GPU_TEXTUREUSAGE_SAMPLER;
    runtime->source_field_texture =
        btbh_gpu_radiance_cascades_create_texture(
            device,
            SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
            SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
                SDL_GPU_TEXTUREUSAGE_SAMPLER,
            source_field_width,
            source_field_height
        );
    uint32_t directional_source_width = 0u;
    uint32_t directional_source_height = 0u;
    if (!btbh_gpu_radiance_cascades_multiply_u32(
            source_field_width,
            BTBH_GPU_RC_DIRECTIONAL_SOURCE_TILE_WIDTH,
            &directional_source_width
        ) ||
        !btbh_gpu_radiance_cascades_multiply_u32(
            source_field_height,
            BTBH_GPU_RC_DIRECTIONAL_SOURCE_TILE_HEIGHT,
            &directional_source_height
        )) {
        btbh_gpu_radiance_cascades_release_resources(device, runtime);
        return false;
    }
    runtime->directional_source_texture =
        btbh_gpu_radiance_cascades_create_texture(
            device,
            SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
            SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
                SDL_GPU_TEXTUREUSAGE_SAMPLER,
            directional_source_width,
            directional_source_height
        );
    uint32_t direct_light_width = source_field_width;
    uint32_t direct_light_height = source_field_height;
    const uint32_t direct_resolution_scale =
        btbh_gpu_radiance_cascades_direct_resolution_scale(profile);
    if (direct_resolution_scale > 1u &&
        (!btbh_gpu_radiance_cascades_multiply_u32(
            source_field_width,
            direct_resolution_scale,
            &direct_light_width
        ) ||
         !btbh_gpu_radiance_cascades_multiply_u32(
            source_field_height,
            direct_resolution_scale,
            &direct_light_height
        ))) {
        btbh_gpu_radiance_cascades_release_resources(device, runtime);
        return false;
    }
    runtime->direct_light_texture =
        btbh_gpu_radiance_cascades_create_texture(
            device,
            SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
            SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
                SDL_GPU_TEXTUREUSAGE_SAMPLER,
            direct_light_width,
            direct_light_height
        );
    runtime->fallback_cube_texture =
        btbh_gpu_radiance_cascades_create_fallback_cube(device);
    if (!holographic) {
        for (uint32_t index = 0u;
             index < cascade_count;
             ++index) {
            runtime->raw_cascade_textures[index] =
                btbh_gpu_radiance_cascades_create_texture(
                    device,
                    SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
                    compute_usage,
                    layouts[index].atlas_width,
                    layouts[index].atlas_height
                );
            runtime->cascade_textures[index] =
                btbh_gpu_radiance_cascades_create_texture(
                    device,
                    SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
                    compute_usage,
                    layouts[index].atlas_width,
                    layouts[index].atlas_height
                );
        }
        if (angular_prefilter_enabled) {
            runtime->prefiltered_cascade_texture =
                btbh_gpu_radiance_cascades_create_texture(
                    device,
                    SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
                    compute_usage,
                    resolve_atlas_width,
                    resolve_atlas_height
                );
        }
        if (diffuse_cache_enabled) {
            runtime->diffuse_cache_texture =
                btbh_gpu_radiance_cascades_create_texture(
                    device,
                    SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
                    compute_usage,
                    diffuse_cache_atlas_width,
                    diffuse_cache_atlas_height
                );
        }
    } else {
        for (uint32_t index = 0u;
             index < hrc_layout->level_count;
             ++index) {
            runtime->hrc_acceleration_textures[index] =
                btbh_gpu_radiance_cascades_create_texture(
                    device,
                    SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
                    compute_usage,
                    hrc_layout->levels[index].acceleration_atlas_width,
                    hrc_layout->levels[index].acceleration_atlas_height
                );
        }
        for (uint32_t index = 0u; index < 2u; ++index) {
            runtime->hrc_radiance_textures[index] =
                btbh_gpu_radiance_cascades_create_texture(
                    device,
                    SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
                    compute_usage,
                    hrc_layout->radiance_atlas_width,
                    hrc_layout->radiance_atlas_height
                );
        }
        for (uint32_t index = 0u; index < 3u; ++index) {
            runtime->hrc_quadrant_textures[index] =
                btbh_gpu_radiance_cascades_create_texture(
                    device,
                    SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
                    compute_usage,
                    hrc_layout->quadrant_atlas_width,
                    hrc_layout->quadrant_atlas_height
                );
        }
    }
    runtime->indirect_texture =
        btbh_gpu_radiance_cascades_create_texture(
            device,
            SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
            compute_usage,
            internal_width,
            internal_height
        );
    if (holographic) {
        runtime->hrc_reconstructed_texture =
            btbh_gpu_radiance_cascades_create_texture(
                device,
                SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
                compute_usage,
                render_width,
                render_height
            );
    }
    runtime->composed_texture =
        btbh_gpu_radiance_cascades_create_texture(
            device,
            scene_color_format,
            SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
                SDL_GPU_TEXTUREUSAGE_SAMPLER,
            render_width,
            render_height
        );
    if (runtime->source_field_texture == NULL ||
        runtime->directional_source_texture == NULL ||
        runtime->direct_light_texture == NULL ||
        runtime->fallback_cube_texture == NULL ||
        runtime->indirect_texture == NULL ||
        (holographic &&
         runtime->hrc_reconstructed_texture == NULL) ||
        runtime->composed_texture == NULL ||
        (angular_prefilter_enabled &&
         runtime->prefiltered_cascade_texture == NULL) ||
        (diffuse_cache_enabled &&
         runtime->diffuse_cache_texture == NULL)) {
        btbh_gpu_radiance_cascades_message(
            message,
            message_capacity,
            SDL_GetError()
        );
        btbh_gpu_radiance_cascades_release_resources(device, runtime);
        return false;
    }
    if (!holographic) {
        for (uint32_t index = 0u;
             index < cascade_count;
             ++index) {
            if (runtime->raw_cascade_textures[index] != NULL &&
                runtime->cascade_textures[index] != NULL) {
                continue;
            }
            btbh_gpu_radiance_cascades_message(
                message,
                message_capacity,
                SDL_GetError()
            );
            btbh_gpu_radiance_cascades_release_resources(device, runtime);
            return false;
        }
    } else {
        for (uint32_t index = 0u;
             index < hrc_layout->level_count;
             ++index) {
            if (runtime->hrc_acceleration_textures[index] != NULL) {
                continue;
            }
            btbh_gpu_radiance_cascades_message(
                message,
                message_capacity,
                SDL_GetError()
            );
            btbh_gpu_radiance_cascades_release_resources(device, runtime);
            return false;
        }
        for (uint32_t index = 0u; index < 2u; ++index) {
            if (runtime->hrc_radiance_textures[index] == NULL) {
                btbh_gpu_radiance_cascades_message(
                    message,
                    message_capacity,
                    SDL_GetError()
                );
                btbh_gpu_radiance_cascades_release_resources(device, runtime);
                return false;
            }
        }
        for (uint32_t index = 0u; index < 3u; ++index) {
            if (runtime->hrc_quadrant_textures[index] == NULL) {
                btbh_gpu_radiance_cascades_message(
                    message,
                    message_capacity,
                    SDL_GetError()
                );
                btbh_gpu_radiance_cascades_release_resources(
                    device,
                    runtime
                );
                return false;
            }
        }
    }
    runtime->internal_width = internal_width;
    runtime->internal_height = internal_height;
    runtime->profile = *profile;
    (void)memcpy(
        runtime->cascade_layouts,
        layouts,
        sizeof(*layouts) * cascade_count
    );
    runtime->cascade_count = cascade_count;
    runtime->source_field_width = source_field_width;
    runtime->source_field_height = source_field_height;
    runtime->resolve_atlas_width = resolve_atlas_width;
    runtime->resolve_atlas_height = resolve_atlas_height;
    runtime->resolve_direction_tile_width =
        resolve_direction_tile_width;
    runtime->resolve_direction_tile_height =
        resolve_direction_tile_height;
    runtime->resolve_direction_count = resolve_direction_count;
    runtime->diffuse_cache_atlas_width = diffuse_cache_atlas_width;
    runtime->diffuse_cache_atlas_height = diffuse_cache_atlas_height;
    if (holographic) {
        runtime->hrc_layout = *hrc_layout;
    }
    runtime->resource_backend = backend;
    runtime->render_width = render_width;
    runtime->render_height = render_height;
    return true;
}

static bool btbh_gpu_radiance_cascades_ensure_debug_output(
    SDL_GPUDevice *device,
    BTBHGPUSceneRadianceCascadesRuntime *runtime,
    SDL_GPUTextureFormat scene_color_format,
    uint32_t render_width,
    uint32_t render_height
) {
    if (runtime->compose_pipeline == NULL || runtime->sampler == NULL) {
        return false;
    }
    if (runtime->composed_texture != NULL &&
        runtime->render_width == render_width &&
        runtime->render_height == render_height) {
        return true;
    }
    if (runtime->composed_texture != NULL) {
        SDL_ReleaseGPUTexture(device, runtime->composed_texture);
    }
    runtime->composed_texture =
        btbh_gpu_radiance_cascades_create_texture(
            device,
            scene_color_format,
            SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
                SDL_GPU_TEXTUREUSAGE_SAMPLER,
            render_width,
            render_height
        );
    if (runtime->composed_texture == NULL) {
        return false;
    }
    runtime->render_width = render_width;
    runtime->render_height = render_height;
    return true;
}

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
) {
    const uint64_t prepare_started_ticks = SDL_GetTicksNS();
    if (runtime != NULL) {
        (void)memset(
            &runtime->pass_telemetry,
            0,
            sizeof(runtime->pass_telemetry)
        );
    }
    if (device == NULL || runtime == NULL ||
        !btbh_gpu_radiance_cascades_config_valid(config)) {
        btbh_gpu_radiance_cascades_message(
            message,
            message_capacity,
            "Invalid Radiance Cascades configuration"
        );
        if (runtime != NULL) {
            runtime->pass_telemetry.cpu_prepare_milliseconds =
                (double)(SDL_GetTicksNS() - prepare_started_ticks) /
                1000000.0;
            btbh_gpu_radiance_cascades_mark_failure(
                &runtime->state,
                BTBH_GPU_SCENE_RC_FALLBACK_INVALID_CONFIGURATION
            );
        }
        return false;
    }
    const bool resized =
        runtime->render_width != 0u &&
        (runtime->render_width != render_width ||
         runtime->render_height != render_height);
    const bool recovered_invalid_configuration =
        runtime->state.sticky_failure &&
        runtime->state.fallback_reason ==
            BTBH_GPU_SCENE_RC_FALLBACK_INVALID_CONFIGURATION;
    if (!btbh_gpu_radiance_cascades_state_configure(
            &runtime->state,
            config
        )) {
        return false;
    }
    if (recovered_invalid_configuration) {
        runtime->logged_failure = false;
    }
    if (resized) {
        runtime->state.generation += 1u;
        runtime->state.sticky_failure = false;
        runtime->state.fallback_reason =
            BTBH_GPU_SCENE_RC_FALLBACK_NONE;
        runtime->logged_failure = false;
        btbh_gpu_radiance_cascades_release_resources(device, runtime);
    }
    runtime->auto_frame_budget_milliseconds =
        config->auto_frame_budget_milliseconds;
    if (runtime->state.sticky_failure) {
        return false;
    }
    const BTBHGPUSceneRadianceCascadesTier tier =
        btbh_gpu_radiance_cascades_effective_tier(&runtime->state);
    if (tier == BTBH_GPU_SCENE_RC_OFF) {
        btbh_gpu_radiance_cascades_release_resources(device, runtime);
        runtime->state.active_backend =
            BTBH_GPU_SCENE_RC_BACKEND_CLASSIC;
        runtime->render_width = render_width;
        runtime->render_height = render_height;
        runtime->pass_telemetry.cpu_prepare_milliseconds =
            (double)(SDL_GetTicksNS() - prepare_started_ticks) /
            1000000.0;
        return true;
    }
    uint32_t internal_width = 0u;
    uint32_t internal_height = 0u;
    btbh_gpu_radiance_cascades_dimensions(
        tier,
        render_width,
        render_height,
        &internal_width,
        &internal_height
    );
    const uint32_t source_field_width =
        config->occlusion_cascade_count > 0u
            ? config->occlusion_cascades[0].width
            : BTBH_GPU_RC_COMPATIBILITY_FIELD_SIZE;
    const uint32_t source_field_height =
        config->occlusion_cascade_count > 0u
            ? config->occlusion_cascades[0].height
            : BTBH_GPU_RC_COMPATIBILITY_FIELD_SIZE;
    BTBHGPUSceneRadianceCascadesBackend active_backend = config->backend;
    BTBHGPUHolographicRadianceCascadesLayout hrc_layout;
    (void)memset(&hrc_layout, 0, sizeof(hrc_layout));
    bool backend_fallback = false;
    const uint32_t hrc_linear_scale =
        btbh_gpu_radiance_cascades_holographic_scale_for_tier(tier);
    if (active_backend == BTBH_GPU_SCENE_RC_BACKEND_HOLOGRAPHIC &&
        (hrc_linear_scale == 0u ||
         !btbh_gpu_holographic_radiance_cascades_layout(
            source_field_width,
            source_field_height,
            hrc_linear_scale,
            16384u,
            BTBH_GPU_HRC_DEFAULT_MEMORY_BUDGET_BYTES,
            &hrc_layout
        ))) {
        active_backend = BTBH_GPU_SCENE_RC_BACKEND_CLASSIC;
        backend_fallback = true;
        btbh_gpu_radiance_cascades_message(
            message,
            message_capacity,
            "HRC layout unavailable; using classic RC"
        );
    }
    runtime->state.active_backend = active_backend;
    BTBHGPUSceneRadianceCascadesProfile profile;
    BTBHGPUSceneRadianceCascadeLayout
        layouts[BTBH_GPU_RC_CASCADE_COUNT];
    (void)memset(&profile, 0, sizeof(profile));
    (void)memset(layouts, 0, sizeof(layouts));
    if (!btbh_gpu_radiance_cascades_profile_for_tier(tier, &profile)) {
        btbh_gpu_radiance_cascades_mark_failure(
            &runtime->state,
            BTBH_GPU_SCENE_RC_FALLBACK_INVALID_CONFIGURATION
        );
        return false;
    }
    for (uint32_t index = 0u; index < profile.cascade_count; ++index) {
        if (!btbh_gpu_radiance_cascade_layout(
                &profile,
                source_field_width,
                source_field_height,
                index,
                16384u,
                &layouts[index]
            )) {
            btbh_gpu_radiance_cascades_message(
                message,
                message_capacity,
                "RC cascade atlas exceeds the production texture contract"
            );
            btbh_gpu_radiance_cascades_mark_failure(
                &runtime->state,
                BTBH_GPU_SCENE_RC_FALLBACK_CAPABILITY_UNAVAILABLE
            );
            return false;
        }
    }
    BTBHGPUSceneRadianceCascadesFallbackReason pipeline_failure_reason =
        BTBH_GPU_SCENE_RC_FALLBACK_PIPELINE_CREATION_FAILED;
    const uint64_t pipeline_prepare_started_ticks = SDL_GetTicksNS();
    if (!btbh_gpu_radiance_cascades_ensure_pipelines(
            device,
            runtime,
            post_vertex_shader_path,
            post_fragment_shader_path,
            shader_format,
            scene_color_format,
            &pipeline_failure_reason,
            message,
            message_capacity
        )) {
        runtime->pass_telemetry.cpu_pipeline_prepare_milliseconds =
            (double)(SDL_GetTicksNS() - pipeline_prepare_started_ticks) /
            1000000.0;
        runtime->pass_telemetry.cpu_prepare_milliseconds =
            (double)(SDL_GetTicksNS() - prepare_started_ticks) /
            1000000.0;
        if (config->debug_view ==
            BTBH_GPU_SCENE_RC_DEBUG_FALLBACK_REASON) {
            (void)btbh_gpu_radiance_cascades_ensure_debug_output(
                device,
                runtime,
                scene_color_format,
                render_width,
                render_height
            );
        }
        btbh_gpu_radiance_cascades_mark_failure(
            &runtime->state,
            pipeline_failure_reason
        );
        return false;
    }
    if (active_backend == BTBH_GPU_SCENE_RC_BACKEND_HOLOGRAPHIC &&
        !btbh_gpu_radiance_cascades_ensure_hrc_pipelines(
            device,
            runtime,
            post_fragment_shader_path,
            shader_format,
            message,
            message_capacity
        )) {
        active_backend = BTBH_GPU_SCENE_RC_BACKEND_CLASSIC;
        runtime->state.active_backend = active_backend;
        backend_fallback = true;
        btbh_gpu_radiance_cascades_message(
            message,
            message_capacity,
            "HRC shaders unavailable; using classic RC"
        );
    }
    runtime->pass_telemetry.cpu_pipeline_prepare_milliseconds =
        (double)(SDL_GetTicksNS() - pipeline_prepare_started_ticks) /
        1000000.0;
    const uint64_t resource_prepare_started_ticks = SDL_GetTicksNS();
    const SDL_GPUTextureUsageFlags compute_usage =
        SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE |
        SDL_GPU_TEXTUREUSAGE_SAMPLER;
    if (!SDL_GPUTextureSupportsFormat(
            device,
            SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
            SDL_GPU_TEXTURETYPE_2D,
            compute_usage
        )) {
        btbh_gpu_radiance_cascades_message(
            message,
            message_capacity,
            "RGBA16F compute-storage textures are unavailable"
        );
        if (config->debug_view ==
            BTBH_GPU_SCENE_RC_DEBUG_FALLBACK_REASON) {
            (void)btbh_gpu_radiance_cascades_ensure_debug_output(
                device,
                runtime,
                scene_color_format,
                render_width,
                render_height
            );
        }
        btbh_gpu_radiance_cascades_mark_failure(
            &runtime->state,
            BTBH_GPU_SCENE_RC_FALLBACK_CAPABILITY_UNAVAILABLE
        );
        return false;
    }
    if (!SDL_GPUTextureSupportsFormat(
            device,
            SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT,
            SDL_GPU_TEXTURETYPE_2D,
            SDL_GPU_TEXTUREUSAGE_SAMPLER
        )) {
        btbh_gpu_radiance_cascades_message(
            message,
            message_capacity,
            "RG32F sampled occlusion textures are unavailable"
        );
        btbh_gpu_radiance_cascades_mark_failure(
            &runtime->state,
            BTBH_GPU_SCENE_RC_FALLBACK_CAPABILITY_UNAVAILABLE
        );
        return false;
    }
    if (!SDL_GPUTextureSupportsFormat(
            device,
            SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
            SDL_GPU_TEXTURETYPE_2D,
            SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
                SDL_GPU_TEXTUREUSAGE_SAMPLER
        )) {
        btbh_gpu_radiance_cascades_message(
            message,
            message_capacity,
            "RGBA16F source-field render targets are unavailable"
        );
        btbh_gpu_radiance_cascades_mark_failure(
            &runtime->state,
            BTBH_GPU_SCENE_RC_FALLBACK_CAPABILITY_UNAVAILABLE
        );
        return false;
    }
    if (!btbh_gpu_radiance_cascades_ensure_occlusion_textures(
            device,
            runtime,
            config
        )) {
        btbh_gpu_radiance_cascades_message(
            message,
            message_capacity,
            SDL_GetError()
        );
        btbh_gpu_radiance_cascades_mark_failure(
            &runtime->state,
            BTBH_GPU_SCENE_RC_FALLBACK_RESOURCE_ALLOCATION_FAILED
        );
        return false;
    }
    if (!btbh_gpu_radiance_cascades_ensure_emitter_buffer(
            device,
            runtime,
            config
        )) {
        btbh_gpu_radiance_cascades_message(
            message,
            message_capacity,
            SDL_GetError()
        );
        btbh_gpu_radiance_cascades_mark_failure(
            &runtime->state,
            BTBH_GPU_SCENE_RC_FALLBACK_RESOURCE_ALLOCATION_FAILED
        );
        return false;
    }
    bool textures_ready = btbh_gpu_radiance_cascades_ensure_textures(
            device,
            runtime,
            active_backend,
            active_backend == BTBH_GPU_SCENE_RC_BACKEND_HOLOGRAPHIC
                ? &hrc_layout
                : NULL,
            &profile,
            layouts,
            profile.cascade_count,
            scene_color_format,
            internal_width,
            internal_height,
            source_field_width,
            source_field_height,
            render_width,
            render_height,
            message,
            message_capacity
        );
    if (!textures_ready &&
        active_backend == BTBH_GPU_SCENE_RC_BACKEND_HOLOGRAPHIC) {
        active_backend = BTBH_GPU_SCENE_RC_BACKEND_CLASSIC;
        runtime->state.active_backend = active_backend;
        backend_fallback = true;
        textures_ready = btbh_gpu_radiance_cascades_ensure_textures(
            device,
            runtime,
            active_backend,
            NULL,
            &profile,
            layouts,
            profile.cascade_count,
            scene_color_format,
            internal_width,
            internal_height,
            source_field_width,
            source_field_height,
            render_width,
            render_height,
            message,
            message_capacity
        );
        if (textures_ready) {
            btbh_gpu_radiance_cascades_message(
                message,
                message_capacity,
                "HRC allocation failed; using classic RC"
            );
        }
    }
    if (!textures_ready) {
        if (config->debug_view ==
            BTBH_GPU_SCENE_RC_DEBUG_FALLBACK_REASON) {
            (void)btbh_gpu_radiance_cascades_ensure_debug_output(
                device,
                runtime,
                scene_color_format,
                render_width,
                render_height
            );
        }
        btbh_gpu_radiance_cascades_mark_failure(
            &runtime->state,
            BTBH_GPU_SCENE_RC_FALLBACK_RESOURCE_ALLOCATION_FAILED
        );
        return false;
    }
    runtime->state.fallback_reason = backend_fallback
        ? BTBH_GPU_SCENE_RC_FALLBACK_CAPABILITY_UNAVAILABLE
        : BTBH_GPU_SCENE_RC_FALLBACK_NONE;
    runtime->pass_telemetry.cpu_resource_prepare_milliseconds =
        (double)(SDL_GetTicksNS() - resource_prepare_started_ticks) /
        1000000.0;
    runtime->pass_telemetry.cpu_prepare_milliseconds =
        (double)(SDL_GetTicksNS() - prepare_started_ticks) /
        1000000.0;
    runtime->logged_failure = false;
    return true;
}

bool btbh_gpu_radiance_cascades_runtime_encode_emitters(
    SDL_GPUCommandBuffer *command,
    BTBHGPUSceneRadianceCascadesRuntime *runtime,
    const BTBHGPUSceneRadianceCascades *config
) {
    if (command == NULL || runtime == NULL || config == NULL ||
        runtime->source_pipeline == NULL ||
        runtime->directional_source_pipeline == NULL ||
        runtime->direct_light_pipeline == NULL ||
        runtime->source_field_texture == NULL ||
        runtime->directional_source_texture == NULL ||
        runtime->direct_light_texture == NULL ||
        runtime->occlusion_textures[0] == NULL) {
        return false;
    }
    const bool uploads_pending =
        runtime->emitter_count > 0u ||
        runtime->occlusion_upload_pending_mask != 0u;
    if (uploads_pending) {
        if (runtime->emitter_buffer == NULL ||
            runtime->emitter_transfer_buffer == NULL ||
            runtime->emitter_upload_size == 0u) {
            if (runtime->emitter_count > 0u) {
                return false;
            }
        }
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command);
        if (copy == NULL) {
            return false;
        }
        for (uint32_t index = 0u;
             index < BTBH_GPU_RC_MAX_OCCLUSION_CASCADES;
             ++index) {
            const uint32_t mask = UINT32_C(1) << index;
            if ((runtime->occlusion_upload_pending_mask & mask) == 0u) {
                continue;
            }
            const uint32_t width =
                runtime->occlusion_upload_widths[index];
            const uint32_t height =
                runtime->occlusion_upload_heights[index];
            if (runtime->occlusion_transfer_buffers[index] == NULL ||
                runtime->occlusion_textures[index] == NULL ||
                width == 0u || height == 0u) {
                SDL_EndGPUCopyPass(copy);
                return false;
            }
            SDL_GPUTextureTransferInfo source;
            SDL_zero(source);
            source.transfer_buffer =
                runtime->occlusion_transfer_buffers[index];
            source.pixels_per_row = width;
            source.rows_per_layer = height;
            SDL_GPUTextureRegion destination;
            SDL_zero(destination);
            destination.texture = runtime->occlusion_textures[index];
            destination.w = width;
            destination.h = height;
            destination.d = 1u;
            SDL_UploadToGPUTexture(
                copy,
                &source,
                &destination,
                false
            );
        }
        if (runtime->emitter_count > 0u) {
            SDL_GPUTransferBufferLocation source_location;
            SDL_zero(source_location);
            source_location.transfer_buffer =
                runtime->emitter_transfer_buffer;
            SDL_GPUBufferRegion destination_region;
            SDL_zero(destination_region);
            destination_region.buffer = runtime->emitter_buffer;
            destination_region.size = runtime->emitter_upload_size;
            SDL_UploadToGPUBuffer(
                copy,
                &source_location,
                &destination_region,
                true
            );
        }
        SDL_EndGPUCopyPass(copy);
        for (uint32_t index = 0u;
             index < BTBH_GPU_RC_MAX_OCCLUSION_CASCADES;
             ++index) {
            const uint32_t mask = UINT32_C(1) << index;
            if ((runtime->occlusion_upload_pending_mask & mask) != 0u) {
                runtime->occlusion_revisions[index] =
                    runtime->occlusion_pending_revisions[index];
            }
        }
        runtime->occlusion_upload_pending_mask = 0u;
    }
    SDL_GPUColorTargetInfo target;
    SDL_zero(target);
    target.texture = runtime->source_field_texture;
    target.load_op = SDL_GPU_LOADOP_CLEAR;
    target.store_op = SDL_GPU_STOREOP_STORE;
    target.cycle = true;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(
        command,
        &target,
        1u,
        NULL
    );
    if (pass == NULL) {
        return false;
    }
    if (runtime->emitter_count > 0u &&
        runtime->emitter_buffer != NULL) {
        BTBHGPUSceneRadianceSourceUniforms uniforms;
        (void)memset(&uniforms, 0, sizeof(uniforms));
        if (config->occlusion_cascade_count > 0u) {
            const BTBHGPUSceneRadianceCascadesOcclusionCascade *field =
                &config->occlusion_cascades[0];
            uniforms.origin_cell[0] = field->origin_world_x;
            uniforms.origin_cell[1] = field->origin_world_z;
            uniforms.origin_cell[2] = field->cell_size;
            uniforms.origin_cell[3] = 1.0f / field->cell_size;
        } else {
            uniforms.origin_cell[0] =
                -0.5f * (float)runtime->source_field_width;
            uniforms.origin_cell[1] =
                -0.5f * (float)runtime->source_field_height;
            uniforms.origin_cell[2] = 1.0f;
            uniforms.origin_cell[3] = 1.0f;
        }
        uniforms.dimensions[0] = runtime->source_field_width;
        uniforms.dimensions[1] = runtime->source_field_height;
        SDL_BindGPUGraphicsPipeline(pass, runtime->source_pipeline);
        SDL_BindGPUVertexStorageBuffers(
            pass,
            0u,
            &runtime->emitter_buffer,
            1u
        );
        SDL_PushGPUVertexUniformData(
            command,
            0u,
            &uniforms,
            (uint32_t)sizeof(uniforms)
        );
        SDL_DrawGPUPrimitives(
            pass,
            6u,
            runtime->emitter_count,
            0u,
            0u
        );
    }
    SDL_EndGPURenderPass(pass);

    SDL_GPUColorTargetInfo directional_target;
    SDL_zero(directional_target);
    directional_target.texture = runtime->directional_source_texture;
    directional_target.load_op = SDL_GPU_LOADOP_CLEAR;
    directional_target.store_op = SDL_GPU_STOREOP_STORE;
    directional_target.cycle = true;
    SDL_GPURenderPass *directional_pass = SDL_BeginGPURenderPass(
        command,
        &directional_target,
        1u,
        NULL
    );
    if (directional_pass == NULL) {
        return false;
    }
    if (runtime->directional_emitter_count > 0u &&
        runtime->emitter_buffer != NULL) {
        uint32_t directional_instance_count = 0u;
        if (!btbh_gpu_radiance_cascades_multiply_u32(
                runtime->directional_emitter_count,
                BTBH_GPU_RC_DIRECTIONAL_SOURCE_COUNT,
                &directional_instance_count
            )) {
            SDL_EndGPURenderPass(directional_pass);
            return false;
        }
        BTBHGPUSceneRadianceDirectionalSourceUniforms uniforms;
        (void)memset(&uniforms, 0, sizeof(uniforms));
        if (config->occlusion_cascade_count > 0u) {
            const BTBHGPUSceneRadianceCascadesOcclusionCascade *field =
                &config->occlusion_cascades[0];
            uniforms.origin_cell[0] = field->origin_world_x;
            uniforms.origin_cell[1] = field->origin_world_z;
            uniforms.origin_cell[2] = field->cell_size;
            uniforms.origin_cell[3] = 1.0f / field->cell_size;
        } else {
            uniforms.origin_cell[0] =
                -0.5f * (float)runtime->source_field_width;
            uniforms.origin_cell[1] =
                -0.5f * (float)runtime->source_field_height;
            uniforms.origin_cell[2] = 1.0f;
            uniforms.origin_cell[3] = 1.0f;
        }
        uniforms.field_dimensions[0] = runtime->source_field_width;
        uniforms.field_dimensions[1] = runtime->source_field_height;
        uniforms.direction_tile[0] =
            BTBH_GPU_RC_DIRECTIONAL_SOURCE_TILE_WIDTH;
        uniforms.direction_tile[1] =
            BTBH_GPU_RC_DIRECTIONAL_SOURCE_TILE_HEIGHT;
        uniforms.direction_count = BTBH_GPU_RC_DIRECTIONAL_SOURCE_COUNT;
        SDL_BindGPUGraphicsPipeline(
            directional_pass,
            runtime->directional_source_pipeline
        );
        SDL_BindGPUVertexStorageBuffers(
            directional_pass,
            0u,
            &runtime->emitter_buffer,
            1u
        );
        SDL_PushGPUVertexUniformData(
            command,
            0u,
            &uniforms,
            (uint32_t)sizeof(uniforms)
        );
        SDL_DrawGPUPrimitives(
            directional_pass,
            6u,
            directional_instance_count,
            0u,
            0u
        );
    }
    SDL_EndGPURenderPass(directional_pass);

    SDL_GPUColorTargetInfo direct_target;
    SDL_zero(direct_target);
    direct_target.texture = runtime->direct_light_texture;
    direct_target.load_op = SDL_GPU_LOADOP_CLEAR;
    direct_target.store_op = SDL_GPU_STOREOP_STORE;
    direct_target.cycle = true;
    SDL_GPURenderPass *direct_pass = SDL_BeginGPURenderPass(
        command,
        &direct_target,
        1u,
        NULL
    );
    if (direct_pass == NULL) {
        return false;
    }
    if (runtime->emitter_count > 0u &&
        runtime->emitter_buffer != NULL) {
        BTBHGPUSceneRadianceSourceUniforms uniforms;
        (void)memset(&uniforms, 0, sizeof(uniforms));
        if (config->occlusion_cascade_count > 0u) {
            const BTBHGPUSceneRadianceCascadesOcclusionCascade *field =
                &config->occlusion_cascades[0];
            uniforms.origin_cell[0] = field->origin_world_x;
            uniforms.origin_cell[1] = field->origin_world_z;
            uniforms.origin_cell[2] = field->cell_size;
            uniforms.origin_cell[3] = 1.0f / field->cell_size;
        } else {
            uniforms.origin_cell[0] =
                -0.5f * (float)runtime->source_field_width;
            uniforms.origin_cell[1] =
                -0.5f * (float)runtime->source_field_height;
            uniforms.origin_cell[2] = 1.0f;
            uniforms.origin_cell[3] = 1.0f;
        }
        uniforms.dimensions[0] = runtime->source_field_width;
        uniforms.dimensions[1] = runtime->source_field_height;
        SDL_BindGPUGraphicsPipeline(
            direct_pass,
            runtime->direct_light_pipeline
        );
        SDL_BindGPUVertexStorageBuffers(
            direct_pass,
            0u,
            &runtime->emitter_buffer,
            1u
        );
        SDL_GPUTextureSamplerBinding occlusion_sampler;
        SDL_zero(occlusion_sampler);
        occlusion_sampler.texture = runtime->occlusion_textures[0];
        occlusion_sampler.sampler = runtime->sampler;
        SDL_BindGPUFragmentSamplers(
            direct_pass,
            0u,
            &occlusion_sampler,
            1u
        );
        SDL_PushGPUVertexUniformData(
            command,
            0u,
            &uniforms,
            (uint32_t)sizeof(uniforms)
        );
        SDL_DrawGPUPrimitives(
            direct_pass,
            6u,
            runtime->emitter_count,
            0u,
            0u
        );
    }
    SDL_EndGPURenderPass(direct_pass);
    return true;
}

static bool btbh_gpu_radiance_cascades_dispatch_with_cycle(
    SDL_GPUCommandBuffer *command,
    SDL_GPUComputePipeline *pipeline,
    SDL_GPUTexture *output,
    const SDL_GPUTextureSamplerBinding *samplers,
    uint32_t sampler_count,
    const void *uniforms,
    uint32_t uniform_size,
    uint32_t dispatch_width,
    uint32_t dispatch_height,
    bool cycle_output
) {
    if (command == NULL || pipeline == NULL || output == NULL ||
        uniforms == NULL || uniform_size == 0u ||
        dispatch_width == 0u || dispatch_height == 0u) {
        return false;
    }
    SDL_GPUStorageTextureReadWriteBinding output_binding;
    SDL_zero(output_binding);
    output_binding.texture = output;
    output_binding.cycle = cycle_output;
    SDL_GPUComputePass *pass = SDL_BeginGPUComputePass(
        command,
        &output_binding,
        1u,
        NULL,
        0u
    );
    if (pass == NULL) {
        return false;
    }
    SDL_BindGPUComputePipeline(pass, pipeline);
    SDL_BindGPUComputeSamplers(pass, 0u, samplers, sampler_count);
    SDL_PushGPUComputeUniformData(
        command,
        0u,
        uniforms,
        uniform_size
    );
    SDL_DispatchGPUCompute(
        pass,
        (dispatch_width + 7u) / 8u,
        (dispatch_height + 7u) / 8u,
        1u
    );
    SDL_EndGPUComputePass(pass);
    return true;
}

static bool btbh_gpu_radiance_cascades_dispatch(
    SDL_GPUCommandBuffer *command,
    SDL_GPUComputePipeline *pipeline,
    SDL_GPUTexture *output,
    const SDL_GPUTextureSamplerBinding *samplers,
    uint32_t sampler_count,
    const void *uniforms,
    uint32_t uniform_size,
    uint32_t dispatch_width,
    uint32_t dispatch_height
) {
    return btbh_gpu_radiance_cascades_dispatch_with_cycle(
        command,
        pipeline,
        output,
        samplers,
        sampler_count,
        uniforms,
        uniform_size,
        dispatch_width,
        dispatch_height,
        true
    );
}

static bool btbh_gpu_hrc_profiled_dispatch_with_cycle(
    SDL_GPUCommandBuffer *command,
    const char *label,
    SDL_GPUComputePipeline *pipeline,
    SDL_GPUTexture *output,
    const SDL_GPUTextureSamplerBinding *samplers,
    uint32_t sampler_count,
    const void *uniforms,
    uint32_t uniform_size,
    uint32_t dispatch_width,
    uint32_t dispatch_height,
    bool cycle_output,
    double *cpu_encode_milliseconds,
    uint32_t *dispatch_count
) {
    if (command == NULL || label == NULL ||
        cpu_encode_milliseconds == NULL || dispatch_count == NULL) {
        return false;
    }
    const uint64_t started_ticks = SDL_GetTicksNS();
    SDL_PushGPUDebugGroup(command, label);
    const bool encoded = btbh_gpu_radiance_cascades_dispatch_with_cycle(
        command,
        pipeline,
        output,
        samplers,
        sampler_count,
        uniforms,
        uniform_size,
        dispatch_width,
        dispatch_height,
        cycle_output
    );
    SDL_PopGPUDebugGroup(command);
    *cpu_encode_milliseconds +=
        (double)(SDL_GetTicksNS() - started_ticks) / 1000000.0;
    if (encoded) {
        *dispatch_count += 1u;
    }
    return encoded;
}

static bool btbh_gpu_hrc_profiled_dispatch(
    SDL_GPUCommandBuffer *command,
    const char *label,
    SDL_GPUComputePipeline *pipeline,
    SDL_GPUTexture *output,
    const SDL_GPUTextureSamplerBinding *samplers,
    uint32_t sampler_count,
    const void *uniforms,
    uint32_t uniform_size,
    uint32_t dispatch_width,
    uint32_t dispatch_height,
    double *cpu_encode_milliseconds,
    uint32_t *dispatch_count
) {
    return btbh_gpu_hrc_profiled_dispatch_with_cycle(
        command,
        label,
        pipeline,
        output,
        samplers,
        sampler_count,
        uniforms,
        uniform_size,
        dispatch_width,
        dispatch_height,
        true,
        cpu_encode_milliseconds,
        dispatch_count
    );
}

static void btbh_gpu_hrc_fill_lighting_uniforms(
    const BTBHGPUSceneLighting *lighting,
    float environment_radiance[4],
    float directional_direction_energy[4],
    float directional_color_lobe[4],
    float secondary_directional_direction_energy[4],
    float secondary_directional_color_lobe[4],
    float sky_top_energy[4],
    float sky_horizon_curve[4]
) {
    environment_radiance[0] = lighting->ibl_enabled == 0u
        ? lighting->ambient_red * lighting->ambient_energy
        : 0.0f;
    environment_radiance[1] = lighting->ibl_enabled == 0u
        ? lighting->ambient_green * lighting->ambient_energy
        : 0.0f;
    environment_radiance[2] = lighting->ibl_enabled == 0u
        ? lighting->ambient_blue * lighting->ambient_energy
        : 0.0f;
    environment_radiance[3] = 1.0f;
    directional_direction_energy[0] = lighting->directional_x;
    directional_direction_energy[1] = lighting->directional_y;
    directional_direction_energy[2] = lighting->directional_z;
    directional_direction_energy[3] = lighting->directional_energy;
    directional_color_lobe[0] = lighting->directional_red;
    directional_color_lobe[1] = lighting->directional_green;
    directional_color_lobe[2] = lighting->directional_blue;
    directional_color_lobe[3] = 4.0f;
    secondary_directional_direction_energy[0] =
        lighting->secondary_directional_x;
    secondary_directional_direction_energy[1] =
        lighting->secondary_directional_y;
    secondary_directional_direction_energy[2] =
        lighting->secondary_directional_z;
    secondary_directional_direction_energy[3] =
        lighting->secondary_directional_energy;
    secondary_directional_color_lobe[0] =
        lighting->secondary_directional_red;
    secondary_directional_color_lobe[1] =
        lighting->secondary_directional_green;
    secondary_directional_color_lobe[2] =
        lighting->secondary_directional_blue;
    secondary_directional_color_lobe[3] = 4.0f;
    sky_top_energy[0] = lighting->sky_top_red;
    sky_top_energy[1] = lighting->sky_top_green;
    sky_top_energy[2] = lighting->sky_top_blue;
    sky_top_energy[3] = lighting->sky_energy;
    sky_horizon_curve[0] = lighting->sky_horizon_red;
    sky_horizon_curve[1] = lighting->sky_horizon_green;
    sky_horizon_curve[2] = lighting->sky_horizon_blue;
    sky_horizon_curve[3] = lighting->sky_horizon_curve;
}

static bool btbh_gpu_hrc_temporal_history_offset(
    const BTBHGPUSceneRadianceCascadesRuntime *runtime,
    const BTBHGPUHolographicRadianceCascadesLayout *layout,
    float current_origin_x,
    float current_origin_z,
    float current_cell_size,
    int32_t offset_pixels[2]
) {
    if (runtime == NULL || layout == NULL || offset_pixels == NULL ||
        !runtime->hrc_history_valid ||
        runtime->hrc_history_generation != runtime->state.generation ||
        !isfinite(current_origin_x) || !isfinite(current_origin_z) ||
        !isfinite(current_cell_size) || current_cell_size <= 0.0f ||
        !isfinite(runtime->hrc_history_origin_x) ||
        !isfinite(runtime->hrc_history_origin_z) ||
        !isfinite(runtime->hrc_history_cell_size) ||
        runtime->hrc_history_cell_size <= 0.0f ||
        fabsf(runtime->hrc_history_cell_size - current_cell_size) >
            fmaxf(current_cell_size * 0.0001f, 0.00001f)) {
        return false;
    }
    if (layout->input_width == 0u ||
        layout->output_width % layout->input_width != 0u) {
        return false;
    }
    const uint32_t linear_scale =
        layout->output_width / layout->input_width;
    if (linear_scale != BTBH_GPU_HRC_LOW_SCALE &&
        linear_scale != BTBH_GPU_HRC_HIGH_SCALE) {
        return false;
    }
    const float scale = (float)linear_scale;
    const float shifts[2] = {
        (current_origin_x - runtime->hrc_history_origin_x) /
            current_cell_size * scale,
        (current_origin_z - runtime->hrc_history_origin_z) /
            current_cell_size * scale
    };
    for (uint32_t axis = 0u; axis < 2u; ++axis) {
        if (!isfinite(shifts[axis])) {
            return false;
        }
        const float rounded = roundf(shifts[axis]);
        if (fabsf(shifts[axis] - rounded) > 0.001f ||
            rounded <= -(float)layout->output_width ||
            rounded >= (float)layout->output_width ||
            rounded < (float)INT32_MIN || rounded > (float)INT32_MAX) {
            return false;
        }
        offset_pixels[axis] = (int32_t)rounded;
    }
    return true;
}

static bool btbh_gpu_radiance_cascades_encode_hrc(
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
    float field_origin_x,
    float field_origin_z,
    float field_cell_size,
    SDL_GPUTexture **composed_texture
) {
    const BTBHGPUHolographicRadianceCascadesLayout *layout =
        &runtime->hrc_layout;
    const uint32_t linear_scale = layout->input_width > 0u &&
            layout->output_width % layout->input_width == 0u
        ? layout->output_width / layout->input_width
        : 0u;
    if ((linear_scale != BTBH_GPU_HRC_LOW_SCALE &&
         linear_scale != BTBH_GPU_HRC_HIGH_SCALE) ||
        layout->level_count < 2u || runtime->sampler == NULL ||
        runtime->hrc_trace_pipeline == NULL ||
        runtime->hrc_merge_up_pipeline == NULL ||
        runtime->hrc_merge_down_pipeline == NULL ||
        runtime->hrc_reproject_pipeline == NULL ||
        runtime->hrc_finish_pipeline == NULL ||
        runtime->hrc_blur_pipeline == NULL ||
        runtime->hrc_resolve_pipeline == NULL ||
        runtime->hrc_reconstruct_pipeline == NULL ||
        runtime->hrc_reconstructed_texture == NULL) {
        return false;
    }
    BTBHGPUSceneRadianceCascadesPassTelemetry *telemetry =
        &runtime->pass_telemetry;
    telemetry->cpu_trace_encode_milliseconds = 0.0;
    telemetry->cpu_merge_up_encode_milliseconds = 0.0;
    telemetry->cpu_merge_down_encode_milliseconds = 0.0;
    telemetry->cpu_reproject_encode_milliseconds = 0.0;
    telemetry->cpu_finish_encode_milliseconds = 0.0;
    telemetry->cpu_blur_encode_milliseconds = 0.0;
    telemetry->cpu_resolve_encode_milliseconds = 0.0;
    telemetry->cpu_reconstruct_encode_milliseconds = 0.0;
    telemetry->cpu_compose_encode_milliseconds = 0.0;
    telemetry->trace_dispatch_count = 0u;
    telemetry->reproject_dispatch_count = 0u;
    telemetry->temporal_history_valid = 0u;
    telemetry->temporal_phase = 0u;
    telemetry->fresh_quadrant_count = 0u;
    telemetry->merge_up_dispatch_count = 0u;
    telemetry->merge_down_dispatch_count = 0u;
    telemetry->finish_dispatch_count = 0u;
    telemetry->blur_dispatch_count = 0u;
    telemetry->resolve_dispatch_count = 0u;
    telemetry->reconstruct_dispatch_count = 0u;
    telemetry->compose_draw_count = 0u;

    SDL_GPUTextureSamplerBinding trace_samplers[3];
    SDL_zeroa(trace_samplers);
    trace_samplers[0].texture = runtime->source_field_texture;
    trace_samplers[1].texture = runtime->occlusion_textures[0];
    trace_samplers[2].texture = runtime->directional_source_texture;
    for (uint32_t index = 0u; index < 3u; ++index) {
        trace_samplers[index].sampler = runtime->sampler;
    }
    for (uint32_t level = 0u; level < layout->level_count; ++level) {
        const BTBHGPUHolographicRadianceCascadesLevelLayout *level_layout =
            &layout->levels[level];
        char label[48];
        if (level < layout->traced_level_count) {
            BTBHGPUHRCTraceUniforms uniforms;
            (void)memset(&uniforms, 0, sizeof(uniforms));
            uniforms.output_atlas_resolution[0] =
                level_layout->acceleration_atlas_width;
            uniforms.output_atlas_resolution[1] =
                level_layout->acceleration_atlas_height;
            uniforms.grid_dimensions[0] = level_layout->grid_width;
            uniforms.grid_dimensions[1] = level_layout->grid_height;
            uniforms.direction_count = level_layout->direction_count;
            uniforms.segment_height = layout->base_grid_size;
            uniforms.display_size = layout->output_width;
            uniforms.linear_scale = linear_scale;
            uniforms.origin_cell[0] = field_origin_x;
            uniforms.origin_cell[1] = field_origin_z;
            uniforms.origin_cell[2] = field_cell_size;
            uniforms.origin_cell[3] = 1.0f / field_cell_size;
            uniforms.field_dimensions[0] = runtime->source_field_width;
            uniforms.field_dimensions[1] = runtime->source_field_height;
            uniforms.max_step_count = 128u;
            btbh_gpu_hrc_fill_lighting_uniforms(
                lighting,
                uniforms.environment_radiance,
                uniforms.directional_direction_energy,
                uniforms.directional_color_lobe,
                uniforms.secondary_directional_direction_energy,
                uniforms.secondary_directional_color_lobe,
                uniforms.sky_top_energy,
                uniforms.sky_horizon_curve
            );
            (void)SDL_snprintf(
                label,
                sizeof(label),
                "BTBH HRC Trace T%u",
                level
            );
            SDL_InsertGPUDebugLabel(command, label);
            if (!btbh_gpu_hrc_profiled_dispatch(
                    command,
                    label,
                    runtime->hrc_trace_pipeline,
                    runtime->hrc_acceleration_textures[level],
                    trace_samplers,
                    3u,
                    &uniforms,
                    (uint32_t)sizeof(uniforms),
                    level_layout->acceleration_atlas_width,
                    level_layout->acceleration_atlas_height,
                    &telemetry->cpu_trace_encode_milliseconds,
                    &telemetry->trace_dispatch_count
                )) {
                return false;
            }
        } else {
            const BTBHGPUHolographicRadianceCascadesLevelLayout *previous =
                &layout->levels[level - 1u];
            BTBHGPUHRCMergeUpUniforms uniforms;
            (void)memset(&uniforms, 0, sizeof(uniforms));
            uniforms.output_atlas_resolution[0] =
                level_layout->acceleration_atlas_width;
            uniforms.output_atlas_resolution[1] =
                level_layout->acceleration_atlas_height;
            uniforms.previous_atlas_resolution[0] =
                previous->acceleration_atlas_width;
            uniforms.previous_atlas_resolution[1] =
                previous->acceleration_atlas_height;
            uniforms.grid_width = level_layout->grid_width;
            uniforms.grid_height = level_layout->grid_height;
            uniforms.direction_count = level_layout->direction_count;
            uniforms.previous_direction_count = previous->direction_count;
            uniforms.segment_height = layout->base_grid_size;
            SDL_GPUTextureSamplerBinding sampler;
            SDL_zero(sampler);
            sampler.texture = runtime->hrc_acceleration_textures[level - 1u];
            sampler.sampler = runtime->sampler;
            (void)SDL_snprintf(
                label,
                sizeof(label),
                "BTBH HRC Merge Up T%u",
                level
            );
            SDL_InsertGPUDebugLabel(command, label);
            if (!btbh_gpu_hrc_profiled_dispatch(
                    command,
                    label,
                    runtime->hrc_merge_up_pipeline,
                    runtime->hrc_acceleration_textures[level],
                    &sampler,
                    1u,
                    &uniforms,
                    (uint32_t)sizeof(uniforms),
                    level_layout->acceleration_atlas_width,
                    level_layout->acceleration_atlas_height,
                    &telemetry->cpu_merge_up_encode_milliseconds,
                    &telemetry->merge_up_dispatch_count
                )) {
                return false;
            }
        }
    }

    uint32_t current_radiance = 0u;
    bool next_radiance_valid = false;
    for (int32_t level = (int32_t)layout->level_count - 2;
         level >= 0;
         --level) {
        const uint32_t index = (uint32_t)level;
        const uint32_t output_radiance = next_radiance_valid
            ? 1u - current_radiance
            : 0u;
        const BTBHGPUHolographicRadianceCascadesLevelLayout *level_layout =
            &layout->levels[index];
        const BTBHGPUHolographicRadianceCascadesLevelLayout *next_layout =
            &layout->levels[index + 1u];
        BTBHGPUHRCMergeDownUniforms uniforms;
        (void)memset(&uniforms, 0, sizeof(uniforms));
        uniforms.radiance_atlas_resolution[0] =
            layout->radiance_atlas_width;
        uniforms.radiance_atlas_resolution[1] =
            layout->radiance_atlas_height;
        uniforms.grid_width = level_layout->grid_width;
        uniforms.grid_height = level_layout->grid_height;
        uniforms.direction_count = level_layout->direction_count;
        uniforms.next_grid_width = next_layout->grid_width;
        uniforms.next_direction_count = next_layout->direction_count;
        uniforms.segment_height = layout->base_grid_size;
        uniforms.next_radiance_valid = next_radiance_valid ? 1u : 0u;
        SDL_GPUTextureSamplerBinding samplers[3];
        SDL_zeroa(samplers);
        samplers[0].texture = runtime->hrc_acceleration_textures[index];
        samplers[1].texture = runtime->hrc_acceleration_textures[index + 1u];
        samplers[2].texture = runtime->hrc_radiance_textures[
            next_radiance_valid ? current_radiance : 1u
        ];
        for (uint32_t sampler = 0u; sampler < 3u; ++sampler) {
            samplers[sampler].sampler = runtime->sampler;
        }
        char label[48];
        (void)SDL_snprintf(
            label,
            sizeof(label),
            "BTBH HRC Merge Down R%u",
            index
        );
        SDL_InsertGPUDebugLabel(command, label);
        if (!btbh_gpu_hrc_profiled_dispatch(
                command,
                label,
                runtime->hrc_merge_down_pipeline,
                runtime->hrc_radiance_textures[output_radiance],
                samplers,
                3u,
                &uniforms,
                (uint32_t)sizeof(uniforms),
                layout->radiance_atlas_width,
                layout->radiance_atlas_height,
                &telemetry->cpu_merge_down_encode_milliseconds,
                &telemetry->merge_down_dispatch_count
            )) {
            return false;
        }
        current_radiance = output_radiance;
        next_radiance_valid = true;
    }

    int32_t history_offset_pixels[2] = {0, 0};
    const bool temporal_history_valid =
        btbh_gpu_hrc_temporal_history_offset(
            runtime,
            layout,
            field_origin_x,
            field_origin_z,
            field_cell_size,
            history_offset_pixels
        );
    const uint32_t temporal_phase = runtime->hrc_temporal_phase & 1u;
    telemetry->temporal_history_valid = temporal_history_valid ? 1u : 0u;
    telemetry->temporal_phase = temporal_history_valid
        ? temporal_phase
        : 0u;
    telemetry->fresh_quadrant_count = temporal_history_valid ? 2u : 4u;
    BTBHGPUHRCFinishUniforms finish_uniforms;
    (void)memset(&finish_uniforms, 0, sizeof(finish_uniforms));
    finish_uniforms.output_atlas_resolution[0] =
        layout->quadrant_atlas_width;
    finish_uniforms.output_atlas_resolution[1] =
        layout->quadrant_atlas_height;
    finish_uniforms.radiance_atlas_resolution[0] =
        layout->radiance_atlas_width;
    finish_uniforms.radiance_atlas_resolution[1] =
        layout->radiance_atlas_height;
    finish_uniforms.display_size = layout->output_width;
    finish_uniforms.base_grid_size = layout->base_grid_size;
    finish_uniforms.segment_height = layout->base_grid_size;
    finish_uniforms.linear_scale = linear_scale;
    finish_uniforms.origin_cell[0] = field_origin_x;
    finish_uniforms.origin_cell[1] = field_origin_z;
    finish_uniforms.origin_cell[2] = field_cell_size;
    finish_uniforms.origin_cell[3] = 1.0f / field_cell_size;
    finish_uniforms.field_dimensions[0] = runtime->source_field_width;
    finish_uniforms.field_dimensions[1] = runtime->source_field_height;
    finish_uniforms.max_step_count = 64u;
    btbh_gpu_hrc_fill_lighting_uniforms(
        lighting,
        finish_uniforms.environment_radiance,
        finish_uniforms.directional_direction_energy,
        finish_uniforms.directional_color_lobe,
        finish_uniforms.secondary_directional_direction_energy,
        finish_uniforms.secondary_directional_color_lobe,
        finish_uniforms.sky_top_energy,
        finish_uniforms.sky_horizon_curve
    );
    SDL_GPUTextureSamplerBinding finish_samplers[4];
    SDL_zeroa(finish_samplers);
    finish_samplers[0].texture =
        runtime->hrc_radiance_textures[current_radiance];
    finish_samplers[1].texture = runtime->source_field_texture;
    finish_samplers[2].texture = runtime->occlusion_textures[0];
    finish_samplers[3].texture = runtime->directional_source_texture;
    for (uint32_t index = 0u; index < 4u; ++index) {
        finish_samplers[index].sampler = runtime->sampler;
    }
    if (!temporal_history_valid) {
        finish_uniforms.refresh_rotation = UINT32_MAX;
        SDL_InsertGPUDebugLabel(command, "BTBH HRC Finish Quadrants Full");
        if (!btbh_gpu_hrc_profiled_dispatch(
                command,
                "BTBH HRC Finish Quadrants Full",
                runtime->hrc_finish_pipeline,
                runtime->hrc_quadrant_textures[0],
                finish_samplers,
                4u,
                &finish_uniforms,
                (uint32_t)sizeof(finish_uniforms),
                layout->quadrant_atlas_width,
                layout->quadrant_atlas_height,
                &telemetry->cpu_finish_encode_milliseconds,
                &telemetry->finish_dispatch_count
            )) {
            return false;
        }
    } else {
        for (uint32_t pair_index = 0u; pair_index < 2u; ++pair_index) {
            const uint32_t rotation = temporal_phase + pair_index * 2u;
            finish_uniforms.refresh_rotation = rotation;
            char label[56];
            (void)SDL_snprintf(
                label,
                sizeof(label),
                "BTBH HRC Finish Quadrant R%u",
                rotation
            );
            SDL_InsertGPUDebugLabel(command, label);
            if (!btbh_gpu_hrc_profiled_dispatch_with_cycle(
                    command,
                    label,
                    runtime->hrc_finish_pipeline,
                    runtime->hrc_quadrant_textures[0],
                    finish_samplers,
                    4u,
                    &finish_uniforms,
                    (uint32_t)sizeof(finish_uniforms),
                    layout->quadrant_atlas_width,
                    layout->output_height,
                    pair_index == 0u,
                    &telemetry->cpu_finish_encode_milliseconds,
                    &telemetry->finish_dispatch_count
                )) {
                return false;
            }
        }
    }

    const uint32_t history_read_index =
        runtime->hrc_history_read_index >= 1u &&
        runtime->hrc_history_read_index <= 2u
            ? runtime->hrc_history_read_index
            : 1u;
    const uint32_t history_write_index = temporal_history_valid
        ? (history_read_index == 1u ? 2u : 1u)
        : 1u;
    BTBHGPUHRCReprojectUniforms reproject_uniforms;
    (void)memset(&reproject_uniforms, 0, sizeof(reproject_uniforms));
    reproject_uniforms.atlas_resolution[0] =
        layout->quadrant_atlas_width;
    reproject_uniforms.atlas_resolution[1] =
        layout->quadrant_atlas_height;
    reproject_uniforms.layer_height = layout->output_height;
    reproject_uniforms.linear_scale = linear_scale;
    reproject_uniforms.history_offset_pixels[0] =
        history_offset_pixels[0];
    reproject_uniforms.history_offset_pixels[1] =
        history_offset_pixels[1];
    reproject_uniforms.field_dimensions[0] = runtime->source_field_width;
    reproject_uniforms.field_dimensions[1] = runtime->source_field_height;
    reproject_uniforms.origin_cell[0] = field_origin_x;
    reproject_uniforms.origin_cell[1] = field_origin_z;
    reproject_uniforms.origin_cell[2] = field_cell_size;
    reproject_uniforms.origin_cell[3] = 1.0f / field_cell_size;
    reproject_uniforms.fresh_phase = temporal_history_valid
        ? temporal_phase
        : UINT32_MAX;
    SDL_GPUTextureSamplerBinding reconstruct_samplers[3];
    SDL_zeroa(reconstruct_samplers);
    reconstruct_samplers[0].texture = runtime->hrc_quadrant_textures[0];
    reconstruct_samplers[1].texture = temporal_history_valid
        ? runtime->hrc_quadrant_textures[history_read_index]
        : runtime->hrc_quadrant_textures[0];
    reconstruct_samplers[2].texture = runtime->occlusion_textures[0];
    for (uint32_t index = 0u; index < 3u; ++index) {
        reconstruct_samplers[index].sampler = runtime->sampler;
    }
    SDL_InsertGPUDebugLabel(
        command,
        "BTBH HRC Temporal Reproject and Reconstruct"
    );
    if (!btbh_gpu_hrc_profiled_dispatch(
            command,
            "BTBH HRC Temporal Reproject and Reconstruct",
            runtime->hrc_reproject_pipeline,
            runtime->hrc_quadrant_textures[history_write_index],
            reconstruct_samplers,
            3u,
            &reproject_uniforms,
            (uint32_t)sizeof(reproject_uniforms),
            layout->quadrant_atlas_width,
            layout->quadrant_atlas_height,
            &telemetry->cpu_reproject_encode_milliseconds,
            &telemetry->reproject_dispatch_count
        )) {
        return false;
    }
    SDL_GPUTexture *filtered_quadrant_texture =
        runtime->hrc_quadrant_textures[history_write_index];

    BTBHGPUHRCResolveUniforms resolve_uniforms;
    (void)memset(&resolve_uniforms, 0, sizeof(resolve_uniforms));
    resolve_uniforms.rc_resolution[0] = runtime->internal_width;
    resolve_uniforms.rc_resolution[1] = runtime->internal_height;
    resolve_uniforms.render_resolution[0] = runtime->render_width;
    resolve_uniforms.render_resolution[1] = runtime->render_height;
    resolve_uniforms.hrc_resolution[0] = layout->output_width;
    resolve_uniforms.hrc_resolution[1] = layout->output_height;
    resolve_uniforms.quadrant_atlas_resolution[0] =
        layout->quadrant_atlas_width;
    resolve_uniforms.quadrant_atlas_resolution[1] =
        layout->quadrant_atlas_height;
    resolve_uniforms.indirect_strength = config->indirect_intensity;
    resolve_uniforms.debug_view = (uint32_t)config->debug_view;
    (void)memcpy(
        resolve_uniforms.inverse_view_projection,
        inverse_view_projection,
        sizeof(resolve_uniforms.inverse_view_projection)
    );
    resolve_uniforms.origin_cell[0] = field_origin_x;
    resolve_uniforms.origin_cell[1] = field_origin_z;
    resolve_uniforms.origin_cell[2] = field_cell_size;
    resolve_uniforms.origin_cell[3] = 1.0f / field_cell_size;
    resolve_uniforms.field_dimensions[0] = runtime->source_field_width;
    resolve_uniforms.field_dimensions[1] = runtime->source_field_height;
    for (uint32_t coefficient = 0u; coefficient < 9u; ++coefficient) {
        for (uint32_t channel = 0u; channel < 3u; ++channel) {
            resolve_uniforms.irradiance_sh[coefficient][channel] =
                lighting->irradiance_sh[coefficient * 3u + channel];
        }
    }
    resolve_uniforms.environment_controls[0] =
        lighting->ibl_enabled != 0u ? lighting->ambient_energy : 0.0f;
    resolve_uniforms.environment_controls[1] =
        lighting->ibl_enabled == 0u
            ? lighting->ambient_red * lighting->ambient_energy
            : 0.0f;
    resolve_uniforms.environment_controls[2] =
        lighting->ibl_enabled == 0u
            ? lighting->ambient_green * lighting->ambient_energy
            : 0.0f;
    resolve_uniforms.environment_controls[3] =
        lighting->ibl_enabled == 0u
            ? lighting->ambient_blue * lighting->ambient_energy
            : 0.0f;
    (void)memcpy(
        resolve_uniforms.hero_omni_shadow_position_range,
        hero_omni_shadow_position_range,
        sizeof(resolve_uniforms.hero_omni_shadow_position_range)
    );
    resolve_uniforms.hero_omni_shadow_controls[0] =
        hero_omni_shadow_near;
    resolve_uniforms.hero_omni_shadow_controls[1] =
        hero_omni_shadow_softness;
    resolve_uniforms.hero_omni_shadow_controls[2] =
        hero_omni_shadow_texture != NULL &&
        config->hero_omni_emitter_index >= 0 ? 1.0f : 0.0f;
    SDL_GPUTextureSamplerBinding resolve_samplers[8];
    SDL_zeroa(resolve_samplers);
    resolve_samplers[0].texture = filtered_quadrant_texture;
    resolve_samplers[1].texture = depth_texture;
    resolve_samplers[2].texture = normal_texture;
    resolve_samplers[3].texture = ambient_texture;
    resolve_samplers[4].texture = ambient_specular_texture;
    resolve_samplers[5].texture = emission_texture;
    resolve_samplers[6].texture = runtime->direct_light_texture;
    resolve_samplers[7].texture = hero_omni_shadow_texture != NULL
        ? hero_omni_shadow_texture
        : runtime->fallback_cube_texture;
    for (uint32_t index = 0u; index < 8u; ++index) {
        resolve_samplers[index].sampler = runtime->sampler;
    }
    SDL_InsertGPUDebugLabel(command, "BTBH HRC Four-quadrant Resolve");
    if (!btbh_gpu_hrc_profiled_dispatch(
            command,
            "BTBH HRC Four-quadrant Resolve",
            runtime->hrc_resolve_pipeline,
            runtime->indirect_texture,
            resolve_samplers,
            8u,
            &resolve_uniforms,
            (uint32_t)sizeof(resolve_uniforms),
            runtime->internal_width,
            runtime->internal_height,
            &telemetry->cpu_resolve_encode_milliseconds,
            &telemetry->resolve_dispatch_count
        )) {
        return false;
    }

    BTBHGPUHRCReconstructUniforms reconstruct_uniforms;
    (void)memset(&reconstruct_uniforms, 0, sizeof(reconstruct_uniforms));
    reconstruct_uniforms.source_resolution[0] = runtime->internal_width;
    reconstruct_uniforms.source_resolution[1] = runtime->internal_height;
    reconstruct_uniforms.output_resolution[0] = runtime->render_width;
    reconstruct_uniforms.output_resolution[1] = runtime->render_height;
    (void)memcpy(
        reconstruct_uniforms.inverse_view_projection,
        inverse_view_projection,
        sizeof(reconstruct_uniforms.inverse_view_projection)
    );
    reconstruct_uniforms.plane_distance_sigma = 3.0f;
    reconstruct_uniforms.normal_power = 16.0f;
    reconstruct_uniforms.visibility_sigma = 10.0f;
    reconstruct_uniforms.spatial_sigma = 0.85f;
    SDL_GPUTextureSamplerBinding upscale_samplers[3];
    SDL_zeroa(upscale_samplers);
    upscale_samplers[0].texture = runtime->indirect_texture;
    upscale_samplers[1].texture = depth_texture;
    upscale_samplers[2].texture = normal_texture;
    for (uint32_t index = 0u; index < 3u; ++index) {
        upscale_samplers[index].sampler = runtime->sampler;
    }
    SDL_InsertGPUDebugLabel(command, "BTBH HRC Edge-aware Reconstruction");
    if (!btbh_gpu_hrc_profiled_dispatch(
            command,
            "BTBH HRC Edge-aware Reconstruction",
            runtime->hrc_reconstruct_pipeline,
            runtime->hrc_reconstructed_texture,
            upscale_samplers,
            3u,
            &reconstruct_uniforms,
            (uint32_t)sizeof(reconstruct_uniforms),
            runtime->render_width,
            runtime->render_height,
            &telemetry->cpu_reconstruct_encode_milliseconds,
            &telemetry->reconstruct_dispatch_count
        )) {
        return false;
    }

    SDL_InsertGPUDebugLabel(command, "BTBH HRC Compose");
    const uint64_t compose_started_ticks = SDL_GetTicksNS();
    SDL_PushGPUDebugGroup(command, "BTBH HRC Compose");
    SDL_GPUColorTargetInfo target;
    SDL_zero(target);
    target.texture = runtime->composed_texture;
    target.load_op = SDL_GPU_LOADOP_DONT_CARE;
    target.store_op = SDL_GPU_STOREOP_STORE;
    target.cycle = true;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(
        command,
        &target,
        1u,
        NULL
    );
    if (pass == NULL) {
        SDL_PopGPUDebugGroup(command);
        telemetry->cpu_compose_encode_milliseconds =
            (double)(SDL_GetTicksNS() - compose_started_ticks) /
            1000000.0;
        return false;
    }
    SDL_BindGPUGraphicsPipeline(pass, runtime->compose_pipeline);
    SDL_GPUTextureSamplerBinding compose_samplers[8];
    SDL_zeroa(compose_samplers);
    compose_samplers[0].texture = scene_texture;
    compose_samplers[1].texture = runtime->hrc_reconstructed_texture;
    for (uint32_t index = 2u; index < 6u; ++index) {
        compose_samplers[index].texture = filtered_quadrant_texture;
    }
    compose_samplers[6].texture = depth_texture;
    compose_samplers[7].texture = runtime->directional_source_texture;
    for (uint32_t index = 0u; index < 8u; ++index) {
        compose_samplers[index].sampler = runtime->sampler;
    }
    SDL_BindGPUFragmentSamplers(pass, 0u, compose_samplers, 8u);
    BTBHGPUSceneRadianceCascadesComposeUniforms compose_uniforms;
    compose_uniforms.debug_view = (uint32_t)config->debug_view;
    compose_uniforms.fallback_reason =
        (uint32_t)runtime->state.fallback_reason;
    compose_uniforms.indirect_strength = config->indirect_intensity;
    compose_uniforms.padding = 0.0f;
    SDL_PushGPUFragmentUniformData(
        command,
        0u,
        &compose_uniforms,
        (uint32_t)sizeof(compose_uniforms)
    );
    SDL_DrawGPUPrimitives(pass, 3u, 1u, 0u, 0u);
    SDL_EndGPURenderPass(pass);
    SDL_PopGPUDebugGroup(command);
    telemetry->cpu_compose_encode_milliseconds =
        (double)(SDL_GetTicksNS() - compose_started_ticks) /
        1000000.0;
    telemetry->compose_draw_count = 1u;
    runtime->hrc_history_generation = runtime->state.generation;
    runtime->hrc_history_read_index = history_write_index;
    runtime->hrc_history_origin_x = field_origin_x;
    runtime->hrc_history_origin_z = field_origin_z;
    runtime->hrc_history_cell_size = field_cell_size;
    runtime->hrc_temporal_phase = temporal_history_valid
        ? temporal_phase ^ 1u
        : 0u;
    runtime->hrc_history_valid = true;
    *composed_texture = runtime->composed_texture;
    return true;
}

bool btbh_gpu_radiance_cascades_runtime_encode_fallback_debug(
    SDL_GPUCommandBuffer *command,
    BTBHGPUSceneRadianceCascadesRuntime *runtime,
    SDL_GPUTexture *scene_texture,
    SDL_GPUTexture **composed_texture
) {
    if (command == NULL || runtime == NULL || scene_texture == NULL ||
        composed_texture == NULL || runtime->compose_pipeline == NULL ||
        runtime->sampler == NULL || runtime->composed_texture == NULL ||
        !runtime->state.sticky_failure) {
        return false;
    }
    *composed_texture = NULL;
    SDL_InsertGPUDebugLabel(command, "BTBH RC Fallback Debug");
    SDL_GPUColorTargetInfo target;
    SDL_zero(target);
    target.texture = runtime->composed_texture;
    target.load_op = SDL_GPU_LOADOP_DONT_CARE;
    target.store_op = SDL_GPU_STOREOP_STORE;
    target.cycle = true;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(
        command,
        &target,
        1u,
        NULL
    );
    if (pass == NULL) {
        return false;
    }
    SDL_BindGPUGraphicsPipeline(pass, runtime->compose_pipeline);
    SDL_GPUTextureSamplerBinding samplers[8];
    SDL_zeroa(samplers);
    for (uint32_t index = 0u; index < 8u; ++index) {
        samplers[index].texture = scene_texture;
        samplers[index].sampler = runtime->sampler;
    }
    SDL_BindGPUFragmentSamplers(pass, 0u, samplers, 8u);
    BTBHGPUSceneRadianceCascadesComposeUniforms uniforms;
    uniforms.debug_view = BTBH_GPU_SCENE_RC_DEBUG_FALLBACK_REASON;
    uniforms.fallback_reason =
        (uint32_t)runtime->state.fallback_reason;
    uniforms.indirect_strength = 0.0f;
    uniforms.padding = 0.0f;
    SDL_PushGPUFragmentUniformData(
        command,
        0u,
        &uniforms,
        (uint32_t)sizeof(uniforms)
    );
    SDL_DrawGPUPrimitives(pass, 3u, 1u, 0u, 0u);
    SDL_EndGPURenderPass(pass);
    *composed_texture = runtime->composed_texture;
    return true;
}

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
) {
    if (command == NULL || runtime == NULL || config == NULL ||
        lighting == NULL ||
        inverse_view_projection == NULL ||
        hero_omni_shadow_position_range == NULL ||
        composed_texture == NULL ||
        scene_texture == NULL || ambient_texture == NULL ||
        ambient_specular_texture == NULL || emission_texture == NULL ||
        normal_texture == NULL || depth_texture == NULL ||
        runtime->source_field_texture == NULL ||
        runtime->directional_source_texture == NULL ||
        btbh_gpu_radiance_cascades_effective_tier(&runtime->state) ==
            BTBH_GPU_SCENE_RC_OFF ||
        runtime->internal_width == 0u ||
        runtime->internal_height == 0u) {
        return false;
    }
    *composed_texture = NULL;
    const BTBHGPUSceneRadianceCascadesOcclusionCascade *field =
        config->occlusion_cascade_count > 0u
            ? &config->occlusion_cascades[0]
            : NULL;
    const float field_origin_x = field != NULL
        ? field->origin_world_x
        : -0.5f * (float)runtime->source_field_width;
    const float field_origin_z = field != NULL
        ? field->origin_world_z
        : -0.5f * (float)runtime->source_field_height;
    const float field_cell_size = field != NULL ? field->cell_size : 1.0f;

    if (runtime->state.active_backend ==
        BTBH_GPU_SCENE_RC_BACKEND_HOLOGRAPHIC) {
        return btbh_gpu_radiance_cascades_encode_hrc(
            command,
            runtime,
            config,
            lighting,
            inverse_view_projection,
            scene_texture,
            ambient_texture,
            ambient_specular_texture,
            emission_texture,
            normal_texture,
            depth_texture,
            hero_omni_shadow_texture,
            hero_omni_shadow_position_range,
            hero_omni_shadow_near,
            hero_omni_shadow_softness,
            field_origin_x,
            field_origin_z,
            field_cell_size,
            composed_texture
        );
    }

    for (int32_t cascade = (int32_t)runtime->cascade_count - 1;
         cascade >= 0;
         --cascade) {
        const uint32_t index = (uint32_t)cascade;
        if (!btbh_gpu_radiance_cascades_raw_trace_required(
                index,
                runtime->cascade_count,
                BTBH_GPU_RC_REPROJECTED_BILINEAR_MERGE != 0u
            )) {
            continue;
        }
        const BTBHGPUSceneRadianceCascadeLayout *layout =
            &runtime->cascade_layouts[index];
        BTBHGPUSceneRadianceTraceUniforms trace_uniforms;
        (void)memset(&trace_uniforms, 0, sizeof(trace_uniforms));
        trace_uniforms.atlas_resolution[0] = layout->atlas_width;
        trace_uniforms.atlas_resolution[1] = layout->atlas_height;
        trace_uniforms.probe_count[0] = layout->probe_count_x;
        trace_uniforms.probe_count[1] = layout->probe_count_y;
        trace_uniforms.direction_tile[0] =
            layout->direction_tile_width;
        trace_uniforms.direction_tile[1] =
            layout->direction_tile_height;
        trace_uniforms.direction_count = layout->direction_count;
        trace_uniforms.probe_spacing_cells =
            layout->probe_spacing_cells;
        trace_uniforms.interval_start_cells =
            layout->interval_start_cells;
        trace_uniforms.interval_end_cells = layout->interval_end_cells;
        trace_uniforms.origin_cell[0] = field_origin_x;
        trace_uniforms.origin_cell[1] = field_origin_z;
        trace_uniforms.origin_cell[2] = field_cell_size;
        trace_uniforms.origin_cell[3] = 1.0f / field_cell_size;
        trace_uniforms.field_dimensions[0] = runtime->source_field_width;
        trace_uniforms.field_dimensions[1] = runtime->source_field_height;
        const float interval_length =
            layout->interval_end_cells - layout->interval_start_cells;
        const uint32_t nominal_steps =
            (uint32_t)ceilf(interval_length / 4.0f) + 32u;
        trace_uniforms.max_step_count = nominal_steps < 256u
            ? nominal_steps
            : 256u;
        if (index + 1u == runtime->cascade_count) {
            trace_uniforms.environment_radiance[0] =
                lighting->ibl_enabled == 0u
                    ? lighting->ambient_red * lighting->ambient_energy
                    : 0.0f;
            trace_uniforms.environment_radiance[1] =
                lighting->ibl_enabled == 0u
                    ? lighting->ambient_green * lighting->ambient_energy
                    : 0.0f;
            trace_uniforms.environment_radiance[2] =
                lighting->ibl_enabled == 0u
                    ? lighting->ambient_blue * lighting->ambient_energy
                    : 0.0f;
            trace_uniforms.environment_radiance[3] = 1.0f;
            trace_uniforms.directional_direction_energy[0] =
                lighting->directional_x;
            trace_uniforms.directional_direction_energy[1] =
                lighting->directional_y;
            trace_uniforms.directional_direction_energy[2] =
                lighting->directional_z;
            trace_uniforms.directional_direction_energy[3] =
                lighting->directional_energy;
            trace_uniforms.directional_color_lobe[0] =
                lighting->directional_red;
            trace_uniforms.directional_color_lobe[1] =
                lighting->directional_green;
            trace_uniforms.directional_color_lobe[2] =
                lighting->directional_blue;
            trace_uniforms.directional_color_lobe[3] = 4.0f;
            trace_uniforms.secondary_directional_direction_energy[0] =
                lighting->secondary_directional_x;
            trace_uniforms.secondary_directional_direction_energy[1] =
                lighting->secondary_directional_y;
            trace_uniforms.secondary_directional_direction_energy[2] =
                lighting->secondary_directional_z;
            trace_uniforms.secondary_directional_direction_energy[3] =
                lighting->secondary_directional_energy;
            trace_uniforms.secondary_directional_color_lobe[0] =
                lighting->secondary_directional_red;
            trace_uniforms.secondary_directional_color_lobe[1] =
                lighting->secondary_directional_green;
            trace_uniforms.secondary_directional_color_lobe[2] =
                lighting->secondary_directional_blue;
            trace_uniforms.secondary_directional_color_lobe[3] = 4.0f;
            trace_uniforms.sky_top_energy[0] = lighting->sky_top_red;
            trace_uniforms.sky_top_energy[1] = lighting->sky_top_green;
            trace_uniforms.sky_top_energy[2] = lighting->sky_top_blue;
            trace_uniforms.sky_top_energy[3] = 0.0f;
            trace_uniforms.sky_horizon_curve[0] =
                lighting->sky_horizon_red;
            trace_uniforms.sky_horizon_curve[1] =
                lighting->sky_horizon_green;
            trace_uniforms.sky_horizon_curve[2] =
                lighting->sky_horizon_blue;
            trace_uniforms.sky_horizon_curve[3] =
                lighting->sky_horizon_curve;
        }
        SDL_GPUTextureSamplerBinding trace_samplers[3];
        SDL_zeroa(trace_samplers);
        trace_samplers[0].texture = runtime->source_field_texture;
        trace_samplers[1].texture = runtime->occlusion_textures[0];
        trace_samplers[2].texture =
            runtime->directional_source_texture;
        trace_samplers[0].sampler = runtime->sampler;
        trace_samplers[1].sampler = runtime->sampler;
        trace_samplers[2].sampler = runtime->sampler;
        char trace_label[48];
        (void)SDL_snprintf(
            trace_label,
            sizeof(trace_label),
            "BTBH RC Trace C%u",
            index
        );
        SDL_InsertGPUDebugLabel(command, trace_label);
        SDL_GPUTexture *trace_output =
            index + 1u == runtime->cascade_count
                ? runtime->cascade_textures[index]
                : runtime->raw_cascade_textures[index];
        if (!btbh_gpu_radiance_cascades_dispatch(
                command,
                runtime->trace_pipeline,
                trace_output,
                trace_samplers,
                3u,
                &trace_uniforms,
                (uint32_t)sizeof(trace_uniforms),
                layout->atlas_width,
                layout->atlas_height
            )) {
            return false;
        }
    }

    for (int32_t cascade = (int32_t)runtime->cascade_count - 2;
         cascade >= 0;
         --cascade) {
        const uint32_t index = (uint32_t)cascade;
        const BTBHGPUSceneRadianceCascadeLayout *lower =
            &runtime->cascade_layouts[index];
        const BTBHGPUSceneRadianceCascadeLayout *upper =
            &runtime->cascade_layouts[index + 1u];
        BTBHGPUSceneRadianceMergeUniforms merge_uniforms;
        (void)memset(&merge_uniforms, 0, sizeof(merge_uniforms));
        merge_uniforms.lower_atlas_resolution[0] = lower->atlas_width;
        merge_uniforms.lower_atlas_resolution[1] = lower->atlas_height;
        merge_uniforms.lower_probe_count[0] = lower->probe_count_x;
        merge_uniforms.lower_probe_count[1] = lower->probe_count_y;
        merge_uniforms.lower_direction_tile[0] =
            lower->direction_tile_width;
        merge_uniforms.lower_direction_tile[1] =
            lower->direction_tile_height;
        merge_uniforms.lower_direction_count = lower->direction_count;
        merge_uniforms.lower_probe_spacing_cells =
            lower->probe_spacing_cells;
        merge_uniforms.upper_atlas_resolution[0] = upper->atlas_width;
        merge_uniforms.upper_atlas_resolution[1] = upper->atlas_height;
        merge_uniforms.upper_probe_count[0] = upper->probe_count_x;
        merge_uniforms.upper_probe_count[1] = upper->probe_count_y;
        merge_uniforms.upper_direction_tile[0] =
            upper->direction_tile_width;
        merge_uniforms.upper_direction_tile[1] =
            upper->direction_tile_height;
        merge_uniforms.upper_direction_count = upper->direction_count;
        merge_uniforms.upper_probe_spacing_cells =
            upper->probe_spacing_cells;
        merge_uniforms.angular_branching = runtime->profile.angular_branching;
        merge_uniforms.bilinear_fix =
            BTBH_GPU_RC_REPROJECTED_BILINEAR_MERGE;
        merge_uniforms.lower_interval_start_cells =
            lower->interval_start_cells;
        merge_uniforms.upper_interval_start_cells =
            upper->interval_start_cells;
        merge_uniforms.origin_cell[0] = field_origin_x;
        merge_uniforms.origin_cell[1] = field_origin_z;
        merge_uniforms.origin_cell[2] = field_cell_size;
        merge_uniforms.origin_cell[3] = 1.0f / field_cell_size;
        merge_uniforms.field_dimensions[0] =
            runtime->source_field_width;
        merge_uniforms.field_dimensions[1] =
            runtime->source_field_height;
        const float corrected_length =
            upper->interval_start_cells - lower->interval_start_cells +
            (float)upper->probe_spacing_cells * 1.5f;
        const uint32_t corrected_steps =
            (uint32_t)ceilf(corrected_length / 4.0f) + 32u;
        merge_uniforms.max_step_count = corrected_steps < 256u
            ? corrected_steps
            : 256u;
        SDL_GPUTextureSamplerBinding merge_samplers[5];
        SDL_zeroa(merge_samplers);
        merge_samplers[0].texture = runtime->raw_cascade_textures[index];
        merge_samplers[1].texture = runtime->cascade_textures[index + 1u];
        merge_samplers[2].texture = runtime->source_field_texture;
        merge_samplers[3].texture = runtime->occlusion_textures[0];
        merge_samplers[4].texture =
            runtime->directional_source_texture;
        for (uint32_t sampler = 0u; sampler < 5u; ++sampler) {
            merge_samplers[sampler].sampler = runtime->sampler;
        }
        char merge_label[64];
        (void)SDL_snprintf(
            merge_label,
            sizeof(merge_label),
            "BTBH RC Merge C%u <- C%u",
            index,
            index + 1u
        );
        SDL_InsertGPUDebugLabel(command, merge_label);
        if (!btbh_gpu_radiance_cascades_dispatch(
                command,
                runtime->merge_pipeline,
                runtime->cascade_textures[index],
                merge_samplers,
                5u,
                &merge_uniforms,
                (uint32_t)sizeof(merge_uniforms),
                lower->atlas_width,
                lower->atlas_height
            )) {
            return false;
        }
    }

    const BTBHGPUSceneRadianceCascadeLayout *resolve_layout =
        &runtime->cascade_layouts[0];
    SDL_GPUTexture *resolve_cascade_texture =
        runtime->cascade_textures[0];
    if (runtime->prefiltered_cascade_texture != NULL) {
        if (runtime->resolve_direction_count == 0u ||
            resolve_layout->direction_count %
                runtime->resolve_direction_count != 0u) {
            return false;
        }
        BTBHGPUSceneRadiancePrefilterUniforms prefilter_uniforms;
        (void)memset(
            &prefilter_uniforms,
            0,
            sizeof(prefilter_uniforms)
        );
        prefilter_uniforms.input_atlas_resolution[0] =
            resolve_layout->atlas_width;
        prefilter_uniforms.input_atlas_resolution[1] =
            resolve_layout->atlas_height;
        prefilter_uniforms.probe_count[0] =
            resolve_layout->probe_count_x;
        prefilter_uniforms.probe_count[1] =
            resolve_layout->probe_count_y;
        prefilter_uniforms.input_direction_tile[0] =
            resolve_layout->direction_tile_width;
        prefilter_uniforms.input_direction_tile[1] =
            resolve_layout->direction_tile_height;
        prefilter_uniforms.input_direction_count =
            resolve_layout->direction_count;
        prefilter_uniforms.directions_per_output =
            resolve_layout->direction_count /
            runtime->resolve_direction_count;
        prefilter_uniforms.output_atlas_resolution[0] =
            runtime->resolve_atlas_width;
        prefilter_uniforms.output_atlas_resolution[1] =
            runtime->resolve_atlas_height;
        prefilter_uniforms.output_direction_tile[0] =
            runtime->resolve_direction_tile_width;
        prefilter_uniforms.output_direction_tile[1] =
            runtime->resolve_direction_tile_height;
        prefilter_uniforms.output_direction_count =
            runtime->resolve_direction_count;
        SDL_GPUTextureSamplerBinding prefilter_sampler;
        SDL_zero(prefilter_sampler);
        prefilter_sampler.texture = runtime->cascade_textures[0];
        prefilter_sampler.sampler = runtime->sampler;
        SDL_InsertGPUDebugLabel(
            command,
            "BTBH RC Angular Prefilter 64-to-16"
        );
        if (!btbh_gpu_radiance_cascades_dispatch(
                command,
                runtime->prefilter_pipeline,
                runtime->prefiltered_cascade_texture,
                &prefilter_sampler,
                1u,
                &prefilter_uniforms,
                (uint32_t)sizeof(prefilter_uniforms),
                runtime->resolve_atlas_width,
                runtime->resolve_atlas_height
            )) {
            return false;
        }
        resolve_cascade_texture =
            runtime->prefiltered_cascade_texture;
    }
    if (runtime->diffuse_cache_texture != NULL) {
        BTBHGPUSceneRadianceDiffuseCacheUniforms cache_uniforms;
        (void)memset(&cache_uniforms, 0, sizeof(cache_uniforms));
        cache_uniforms.input_atlas_resolution[0] =
            resolve_layout->atlas_width;
        cache_uniforms.input_atlas_resolution[1] =
            resolve_layout->atlas_height;
        cache_uniforms.probe_count[0] = resolve_layout->probe_count_x;
        cache_uniforms.probe_count[1] = resolve_layout->probe_count_y;
        cache_uniforms.input_direction_tile[0] =
            resolve_layout->direction_tile_width;
        cache_uniforms.input_direction_tile[1] =
            resolve_layout->direction_tile_height;
        cache_uniforms.input_direction_count =
            resolve_layout->direction_count;
        cache_uniforms.output_atlas_resolution[0] =
            runtime->diffuse_cache_atlas_width;
        cache_uniforms.output_atlas_resolution[1] =
            runtime->diffuse_cache_atlas_height;
        cache_uniforms.output_normal_tile[0] =
            BTBH_GPU_RC_DIFFUSE_CACHE_NORMAL_TILE_WIDTH;
        cache_uniforms.output_normal_tile[1] =
            BTBH_GPU_RC_DIFFUSE_CACHE_NORMAL_TILE_HEIGHT;
        cache_uniforms.output_normal_count =
            BTBH_GPU_RC_DIFFUSE_CACHE_NORMAL_COUNT;
        SDL_GPUTextureSamplerBinding cache_sampler;
        SDL_zero(cache_sampler);
        cache_sampler.texture = runtime->cascade_textures[0];
        cache_sampler.sampler = runtime->sampler;
        SDL_InsertGPUDebugLabel(
            command,
            "BTBH RC Diffuse Cache 64-to-9"
        );
        if (!btbh_gpu_radiance_cascades_dispatch(
                command,
                runtime->diffuse_cache_pipeline,
                runtime->diffuse_cache_texture,
                &cache_sampler,
                1u,
                &cache_uniforms,
                (uint32_t)sizeof(cache_uniforms),
                runtime->diffuse_cache_atlas_width,
                runtime->diffuse_cache_atlas_height
            )) {
            return false;
        }
    }
    BTBHGPUSceneRadianceResolveUniforms resolve_uniforms;
    (void)memset(&resolve_uniforms, 0, sizeof(resolve_uniforms));
    resolve_uniforms.rc_resolution[0] = runtime->internal_width;
    resolve_uniforms.rc_resolution[1] = runtime->internal_height;
    resolve_uniforms.render_resolution[0] = runtime->render_width;
    resolve_uniforms.render_resolution[1] = runtime->render_height;
    resolve_uniforms.atlas_resolution[0] = runtime->resolve_atlas_width;
    resolve_uniforms.atlas_resolution[1] = runtime->resolve_atlas_height;
    resolve_uniforms.probe_count[0] = resolve_layout->probe_count_x;
    resolve_uniforms.probe_count[1] = resolve_layout->probe_count_y;
    resolve_uniforms.direction_tile[0] =
        runtime->resolve_direction_tile_width;
    resolve_uniforms.direction_tile[1] =
        runtime->resolve_direction_tile_height;
    resolve_uniforms.direction_count = runtime->resolve_direction_count;
    resolve_uniforms.probe_spacing_cells =
        resolve_layout->probe_spacing_cells;
    resolve_uniforms.indirect_strength = config->indirect_intensity;
    resolve_uniforms.diffuse_cache_enabled =
        runtime->diffuse_cache_texture != NULL ? 1u : 0u;
    resolve_uniforms.diffuse_cache_atlas_resolution[0] =
        runtime->diffuse_cache_atlas_width;
    resolve_uniforms.diffuse_cache_atlas_resolution[1] =
        runtime->diffuse_cache_atlas_height;
    resolve_uniforms.diffuse_cache_normal_tile[0] =
        BTBH_GPU_RC_DIFFUSE_CACHE_NORMAL_TILE_WIDTH;
    resolve_uniforms.diffuse_cache_normal_tile[1] =
        BTBH_GPU_RC_DIFFUSE_CACHE_NORMAL_TILE_HEIGHT;
    (void)memcpy(
        resolve_uniforms.inverse_view_projection,
        inverse_view_projection,
        sizeof(resolve_uniforms.inverse_view_projection)
    );
    resolve_uniforms.origin_cell[0] = field_origin_x;
    resolve_uniforms.origin_cell[1] = field_origin_z;
    resolve_uniforms.origin_cell[2] = field_cell_size;
    resolve_uniforms.origin_cell[3] = 1.0f / field_cell_size;
    resolve_uniforms.field_dimensions[0] = runtime->source_field_width;
    resolve_uniforms.field_dimensions[1] = runtime->source_field_height;
    resolve_uniforms.debug_view = (uint32_t)config->debug_view;
    for (uint32_t coefficient = 0u; coefficient < 9u; ++coefficient) {
        for (uint32_t channel = 0u; channel < 3u; ++channel) {
            resolve_uniforms.irradiance_sh[coefficient][channel] =
                lighting->irradiance_sh[coefficient * 3u + channel];
        }
    }
    resolve_uniforms.environment_controls[0] =
        lighting->ibl_enabled != 0u ? lighting->ambient_energy : 0.0f;
    resolve_uniforms.environment_controls[1] =
        lighting->ibl_enabled == 0u
            ? lighting->ambient_red * lighting->ambient_energy
            : 0.0f;
    resolve_uniforms.environment_controls[2] =
        lighting->ibl_enabled == 0u
            ? lighting->ambient_green * lighting->ambient_energy
            : 0.0f;
    resolve_uniforms.environment_controls[3] =
        lighting->ibl_enabled == 0u
            ? lighting->ambient_blue * lighting->ambient_energy
            : 0.0f;
    (void)memcpy(
        resolve_uniforms.hero_omni_shadow_position_range,
        hero_omni_shadow_position_range,
        sizeof(resolve_uniforms.hero_omni_shadow_position_range)
    );
    resolve_uniforms.hero_omni_shadow_controls[0] =
        hero_omni_shadow_near;
    resolve_uniforms.hero_omni_shadow_controls[1] =
        hero_omni_shadow_softness;
    resolve_uniforms.hero_omni_shadow_controls[2] =
        hero_omni_shadow_texture != NULL &&
        config->hero_omni_emitter_index >= 0 ? 1.0f : 0.0f;
    SDL_GPUTextureSamplerBinding resolve_samplers[9];
    SDL_zeroa(resolve_samplers);
    resolve_samplers[0].texture = resolve_cascade_texture;
    resolve_samplers[1].texture = runtime->diffuse_cache_texture != NULL
        ? runtime->diffuse_cache_texture
        : resolve_cascade_texture;
    resolve_samplers[2].texture = depth_texture;
    resolve_samplers[3].texture = normal_texture;
    resolve_samplers[4].texture = ambient_texture;
    resolve_samplers[5].texture = ambient_specular_texture;
    resolve_samplers[6].texture = emission_texture;
    resolve_samplers[7].texture = runtime->direct_light_texture;
    resolve_samplers[8].texture = hero_omni_shadow_texture != NULL
        ? hero_omni_shadow_texture
        : runtime->fallback_cube_texture;
    for (uint32_t index = 0u; index < 9u; ++index) {
        resolve_samplers[index].sampler = runtime->sampler;
    }
    SDL_InsertGPUDebugLabel(command, "BTBH RC Directional Resolve");
    if (!btbh_gpu_radiance_cascades_dispatch(
            command,
            runtime->resolve_pipeline,
            runtime->indirect_texture,
            resolve_samplers,
            9u,
            &resolve_uniforms,
            (uint32_t)sizeof(resolve_uniforms),
            runtime->internal_width,
            runtime->internal_height
        )) {
        return false;
    }

    SDL_InsertGPUDebugLabel(command, "BTBH RC Compose");
    SDL_GPUColorTargetInfo target;
    SDL_zero(target);
    target.texture = runtime->composed_texture;
    target.load_op = SDL_GPU_LOADOP_DONT_CARE;
    target.store_op = SDL_GPU_STOREOP_STORE;
    target.cycle = true;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(
        command,
        &target,
        1u,
        NULL
    );
    if (pass == NULL) {
        return false;
    }
    SDL_BindGPUGraphicsPipeline(pass, runtime->compose_pipeline);
    SDL_GPUTextureSamplerBinding compose_samplers[8];
    SDL_zeroa(compose_samplers);
    compose_samplers[0].texture = scene_texture;
    compose_samplers[1].texture = runtime->indirect_texture;
    for (uint32_t index = 0u;
         index < 4u;
         ++index) {
        compose_samplers[index + 2u].texture =
            runtime->cascade_textures[index];
    }
    compose_samplers[6].texture = depth_texture;
    compose_samplers[7].texture = runtime->directional_source_texture;
    for (uint32_t index = 0u; index < 8u; ++index) {
        compose_samplers[index].sampler = runtime->sampler;
    }
    SDL_BindGPUFragmentSamplers(pass, 0u, compose_samplers, 8u);
    BTBHGPUSceneRadianceCascadesComposeUniforms compose_uniforms;
    compose_uniforms.debug_view = (uint32_t)config->debug_view;
    compose_uniforms.fallback_reason =
        (uint32_t)runtime->state.fallback_reason;
    compose_uniforms.indirect_strength = config->indirect_intensity;
    compose_uniforms.padding = 0.0f;
    SDL_PushGPUFragmentUniformData(
        command,
        0u,
        &compose_uniforms,
        (uint32_t)sizeof(compose_uniforms)
    );
    SDL_DrawGPUPrimitives(pass, 3u, 1u, 0u, 0u);
    SDL_EndGPURenderPass(pass);
    *composed_texture = runtime->composed_texture;
    return true;
}

void btbh_gpu_radiance_cascades_runtime_record_completion(
    BTBHGPUSceneRadianceCascadesRuntime *runtime,
    double completion_milliseconds
) {
    if (runtime == NULL) {
        return;
    }
    btbh_gpu_radiance_cascades_record_completion(
        &runtime->state,
        completion_milliseconds,
        (double)runtime->auto_frame_budget_milliseconds
    );
}

void btbh_gpu_radiance_cascades_runtime_invalidate_history(
    BTBHGPUSceneRadianceCascadesRuntime *runtime
) {
    if (runtime == NULL) {
        return;
    }
    runtime->hrc_history_generation = 0u;
    runtime->hrc_temporal_phase = 0u;
    runtime->hrc_history_read_index = 1u;
    runtime->hrc_history_valid = false;
}
