#include "btbh_gpu_radiance_cascades.h"
#include "btbh_gpu_holographic_radiance_cascades.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

static BTBHGPUSceneRadianceCascades config(
    BTBHGPUSceneRadianceCascadesTier tier
) {
    BTBHGPUSceneRadianceCascades result = {0};
    result.tier = tier;
    result.auto_frame_budget_milliseconds = 16.0f;
    result.indirect_intensity = 0.60f;
    result.hero_omni_emitter_index = -1;
    return result;
}

static BTBHGPUSceneRadianceEmitter valid_emitter(uint64_t stable_id) {
    BTBHGPUSceneRadianceEmitter result = {0};
    result.stable_id = stable_id;
    result.position_x = 1.0f;
    result.position_y = 2.0f;
    result.position_z = 3.0f;
    result.radius = 10.0f;
    result.height = 1.0f;
    result.red = 0.25f;
    result.green = 0.50f;
    result.blue = 1.0f;
    result.energy = 4.0f;
    result.direction_z = 1.0f;
    result.inner_cutoff = -1.0f;
    result.outer_cutoff = -2.0f;
    result.shape = BTBH_GPU_SCENE_RADIANCE_EMITTER_DISK;
    result.light_class = BTBH_GPU_SCENE_RADIANCE_EMITTER_CLASS_WORLD;
    return result;
}

static void test_configuration_validation(void) {
    BTBHGPUSceneRadianceCascades value =
        config(BTBH_GPU_SCENE_RC_OFF);
    assert(btbh_gpu_radiance_cascades_config_valid(&value));
    value.tier = (BTBHGPUSceneRadianceCascadesTier)99;
    assert(!btbh_gpu_radiance_cascades_config_valid(&value));
    value = config(BTBH_GPU_SCENE_RC_HIGH);
    value.backend = (BTBHGPUSceneRadianceCascadesBackend)99;
    assert(!btbh_gpu_radiance_cascades_config_valid(&value));
    value = config(BTBH_GPU_SCENE_RC_LOW);
    value.debug_view =
        (BTBHGPUSceneRadianceCascadesDebugView)99;
    assert(!btbh_gpu_radiance_cascades_config_valid(&value));
    value = config(BTBH_GPU_SCENE_RC_AUTO);
    value.auto_frame_budget_milliseconds = 0.0f;
    assert(!btbh_gpu_radiance_cascades_config_valid(&value));
    value.auto_frame_budget_milliseconds = NAN;
    assert(!btbh_gpu_radiance_cascades_config_valid(&value));
    value = config(BTBH_GPU_SCENE_RC_LOW);
    value.indirect_intensity = 0.0f;
    assert(btbh_gpu_radiance_cascades_config_valid(&value));
    value.indirect_intensity = 2.0f;
    assert(btbh_gpu_radiance_cascades_config_valid(&value));
    value.indirect_intensity = -0.01f;
    assert(!btbh_gpu_radiance_cascades_config_valid(&value));
    value.indirect_intensity = 2.01f;
    assert(!btbh_gpu_radiance_cascades_config_valid(&value));
    value.indirect_intensity = NAN;
    assert(!btbh_gpu_radiance_cascades_config_valid(&value));

    value = config(BTBH_GPU_SCENE_RC_LOW);
    BTBHGPUSceneRadianceEmitter emitter = valid_emitter(7u);
    value.emitter_count = 1u;
    value.emitters = &emitter;
    assert(btbh_gpu_radiance_cascades_config_valid(&value));
    value.hero_omni_emitter_index = 0;
    assert(btbh_gpu_radiance_cascades_config_valid(&value));
    value.hero_omni_emitter_index = 1;
    assert(!btbh_gpu_radiance_cascades_config_valid(&value));
    value.hero_omni_emitter_index = -2;
    assert(!btbh_gpu_radiance_cascades_config_valid(&value));
    value.hero_omni_emitter_index = -1;
    emitter.energy = NAN;
    assert(!btbh_gpu_radiance_cascades_config_valid(&value));
    emitter.energy = 4.0f;
    emitter.radius = 0.0f;
    assert(!btbh_gpu_radiance_cascades_config_valid(&value));
    emitter.radius = 10.0f;
    emitter.light_class = (BTBHGPUSceneRadianceEmitterClass)99;
    assert(!btbh_gpu_radiance_cascades_config_valid(&value));
    emitter.light_class = BTBH_GPU_SCENE_RADIANCE_EMITTER_CLASS_WORLD;
    value.emitters = NULL;
    assert(!btbh_gpu_radiance_cascades_config_valid(&value));

    const uint32_t many_emitter_count = 10000u;
    BTBHGPUSceneRadianceEmitter *many_emitters =
        (BTBHGPUSceneRadianceEmitter *)malloc(
            sizeof(*many_emitters) * many_emitter_count
        );
    assert(many_emitters != NULL);
    for (uint32_t index = 0u; index < many_emitter_count; ++index) {
        many_emitters[index] = valid_emitter(index);
    }
    value.emitter_count = many_emitter_count;
    value.emitters = many_emitters;
    assert(btbh_gpu_radiance_cascades_config_valid(&value));
    free(many_emitters);

    float occlusion_samples[2] = {1.0f, 0.0f};
    value = config(BTBH_GPU_SCENE_RC_HIGH);
    value.occlusion_cascade_count = 1u;
    value.occlusion_cascades[0].origin_world_x = -4.0f;
    value.occlusion_cascades[0].origin_world_z = -4.0f;
    value.occlusion_cascades[0].cell_size = 1.0f;
    value.occlusion_cascades[0].width = 1u;
    value.occlusion_cascades[0].height = 1u;
    value.occlusion_cascades[0].revision = 7u;
    value.occlusion_cascades[0].samples = occlusion_samples;
    assert(btbh_gpu_radiance_cascades_config_valid(&value));
    value.occlusion_cascades[0].samples = NULL;
    assert(!btbh_gpu_radiance_cascades_config_valid(&value));
    value.occlusion_cascades[0].samples = occlusion_samples;
    value.occlusion_cascades[0].cell_size = 0.0f;
    assert(!btbh_gpu_radiance_cascades_config_valid(&value));
    value.occlusion_cascade_count =
        BTBH_GPU_RC_MAX_OCCLUSION_CASCADES + 1u;
    assert(!btbh_gpu_radiance_cascades_config_valid(&value));
}

static void test_directional_emitter_count_only_includes_spots(void) {
    BTBHGPUSceneRadianceEmitter emitters[4] = {
        valid_emitter(1u),
        valid_emitter(2u),
        valid_emitter(3u),
        valid_emitter(4u),
    };
    emitters[1].inner_cutoff = 0.95f;
    emitters[1].outer_cutoff = 0.75f;
    emitters[3].inner_cutoff = 0.90f;
    emitters[3].outer_cutoff = 0.60f;
    BTBHGPUSceneRadianceCascades value = config(BTBH_GPU_SCENE_RC_HIGH);
    value.emitter_count = 4u;
    value.emitters = emitters;
    assert(btbh_gpu_radiance_cascades_directional_emitter_count(&value) == 2u);
    value.emitter_count = 0u;
    value.emitters = NULL;
    assert(btbh_gpu_radiance_cascades_directional_emitter_count(&value) == 0u);
}

static void test_fixed_tiers_apply_immediately(void) {
    BTBHGPUSceneRadianceCascadesState state;
    btbh_gpu_radiance_cascades_state_initialize(&state);
    BTBHGPUSceneRadianceCascades value =
        config(BTBH_GPU_SCENE_RC_HIGH);
    assert(btbh_gpu_radiance_cascades_state_configure(&state, &value));
    assert(
        btbh_gpu_radiance_cascades_effective_tier(&state) ==
        BTBH_GPU_SCENE_RC_HIGH
    );
    assert(state.pending_tier == BTBH_GPU_SCENE_RC_HIGH);
    value.tier = BTBH_GPU_SCENE_RC_OFF;
    assert(btbh_gpu_radiance_cascades_state_configure(&state, &value));
    assert(state.fallback_reason == BTBH_GPU_SCENE_RC_FALLBACK_DISABLED);
}

static void test_backend_selection_updates_generation_and_status(void) {
    BTBHGPUSceneRadianceCascadesState state;
    btbh_gpu_radiance_cascades_state_initialize(&state);
    BTBHGPUSceneRadianceCascades value =
        config(BTBH_GPU_SCENE_RC_HIGH);
    assert(btbh_gpu_radiance_cascades_state_configure(&state, &value));
    const uint64_t classic_generation = state.generation;
    value.backend = BTBH_GPU_SCENE_RC_BACKEND_HOLOGRAPHIC;
    assert(btbh_gpu_radiance_cascades_state_configure(&state, &value));
    assert(state.requested_backend ==
        BTBH_GPU_SCENE_RC_BACKEND_HOLOGRAPHIC);
    assert(state.active_backend ==
        BTBH_GPU_SCENE_RC_BACKEND_HOLOGRAPHIC);
    assert(state.generation == classic_generation + 1u);

    BTBHGPUSceneRadianceCascadesStatus status;
    btbh_gpu_radiance_cascades_status(&state, 512u, 512u, 0.25, &status);
    assert(status.requested_backend ==
        BTBH_GPU_SCENE_RC_BACKEND_HOLOGRAPHIC);
    assert(status.active_backend ==
        BTBH_GPU_SCENE_RC_BACKEND_HOLOGRAPHIC);
}

static void record_window(
    BTBHGPUSceneRadianceCascadesState *state,
    double milliseconds
) {
    for (uint32_t index = 0u;
         index < BTBH_GPU_RC_COMPLETION_SAMPLE_MINIMUM;
         ++index) {
        btbh_gpu_radiance_cascades_record_completion(
            state,
            milliseconds,
            16.0
        );
    }
}

static void test_auto_is_gated_and_adjacent(void) {
    BTBHGPUSceneRadianceCascadesState state;
    btbh_gpu_radiance_cascades_state_initialize(&state);
    BTBHGPUSceneRadianceCascades value =
        config(BTBH_GPU_SCENE_RC_AUTO);
    assert(btbh_gpu_radiance_cascades_state_configure(&state, &value));
    assert(state.selected_tier == BTBH_GPU_SCENE_RC_LOW);
    record_window(&state, 10.0);
    assert(state.pending_tier == BTBH_GPU_SCENE_RC_HIGH);
    assert(state.selected_tier == BTBH_GPU_SCENE_RC_LOW);
    value.allow_auto_transition = 1u;
    assert(btbh_gpu_radiance_cascades_state_configure(&state, &value));
    assert(state.selected_tier == BTBH_GPU_SCENE_RC_HIGH);
    value.allow_auto_transition = 0u;
    record_window(&state, 18.0);
    assert(state.pending_tier == BTBH_GPU_SCENE_RC_LOW);
    assert(state.selected_tier == BTBH_GPU_SCENE_RC_HIGH);
}

static void test_auto_hysteresis_holds_middle_band(void) {
    BTBHGPUSceneRadianceCascadesState state;
    btbh_gpu_radiance_cascades_state_initialize(&state);
    BTBHGPUSceneRadianceCascades value =
        config(BTBH_GPU_SCENE_RC_AUTO);
    assert(btbh_gpu_radiance_cascades_state_configure(&state, &value));
    record_window(&state, 15.0);
    assert(state.pending_tier == BTBH_GPU_SCENE_RC_LOW);
}

static void test_failure_is_sticky_until_invalidation(void) {
    BTBHGPUSceneRadianceCascadesState state;
    btbh_gpu_radiance_cascades_state_initialize(&state);
    BTBHGPUSceneRadianceCascades value =
        config(BTBH_GPU_SCENE_RC_HIGH);
    assert(btbh_gpu_radiance_cascades_state_configure(&state, &value));
    btbh_gpu_radiance_cascades_mark_failure(
        &state,
        BTBH_GPU_SCENE_RC_FALLBACK_RESOURCE_ALLOCATION_FAILED
    );
    assert(
        btbh_gpu_radiance_cascades_effective_tier(&state) ==
        BTBH_GPU_SCENE_RC_OFF
    );
    assert(state.sticky_failure);
    assert(btbh_gpu_radiance_cascades_state_configure(&state, &value));
    assert(state.sticky_failure);
    value.invalidate = 1u;
    assert(btbh_gpu_radiance_cascades_state_configure(&state, &value));
    assert(!state.sticky_failure);
    assert(state.fallback_reason == BTBH_GPU_SCENE_RC_FALLBACK_NONE);
}

static void test_valid_configuration_recovers_invalid_configuration(void) {
    BTBHGPUSceneRadianceCascadesState state;
    btbh_gpu_radiance_cascades_state_initialize(&state);
    BTBHGPUSceneRadianceCascades value =
        config(BTBH_GPU_SCENE_RC_HIGH);
    assert(btbh_gpu_radiance_cascades_state_configure(&state, &value));
    const uint64_t generation = state.generation;
    btbh_gpu_radiance_cascades_mark_failure(
        &state,
        BTBH_GPU_SCENE_RC_FALLBACK_INVALID_CONFIGURATION
    );
    assert(state.sticky_failure);

    assert(btbh_gpu_radiance_cascades_state_configure(&state, &value));
    assert(!state.sticky_failure);
    assert(state.fallback_reason == BTBH_GPU_SCENE_RC_FALLBACK_NONE);
    assert(state.generation == generation + 1u);
    assert(
        btbh_gpu_radiance_cascades_effective_tier(&state) ==
        BTBH_GPU_SCENE_RC_HIGH
    );
}

static void test_tier_dimensions_round_to_threadgroups(void) {
    uint32_t width = 0u;
    uint32_t height = 0u;
    btbh_gpu_radiance_cascades_dimensions(
        BTBH_GPU_SCENE_RC_LOW,
        1919u,
        1079u,
        &width,
        &height
    );
    assert(width == 480u);
    assert(height == 272u);
    btbh_gpu_radiance_cascades_dimensions(
        BTBH_GPU_SCENE_RC_HIGH,
        1919u,
        1079u,
        &width,
        &height
    );
    assert(width == 1920u);
    assert(height == 1080u);
    assert(width % 8u == 0u);
    assert(height % 8u == 0u);
}

static void assert_close(float actual, float expected) {
    assert(fabsf(actual - expected) < 0.0001f);
}

static void test_directional_cascade_profiles_and_layout(void) {
    BTBHGPUSceneRadianceCascadesProfile low = {0};
    assert(btbh_gpu_radiance_cascades_profile_for_tier(
        BTBH_GPU_SCENE_RC_LOW,
        &low
    ));
    assert(btbh_gpu_radiance_cascades_profile_valid(&low));
    assert(low.cascade_count == 4u);
    assert(low.angular_branching == 2u);
    assert(btbh_gpu_radiance_cascades_direct_resolution_scale(&low) == 1u);

    BTBHGPUSceneRadianceCascadeLayout layout = {0};
    assert(btbh_gpu_radiance_cascade_layout(
        &low,
        64u,
        32u,
        0u,
        4096u,
        &layout
    ));
    assert(layout.probe_spacing_cells == 2u);
    assert(layout.probe_count_x == 32u);
    assert(layout.probe_count_y == 16u);
    assert(layout.direction_tile_width == 2u);
    assert(layout.direction_tile_height == 2u);
    assert(layout.direction_count == 4u);
    assert(layout.atlas_width == 64u);
    assert(layout.atlas_height == 32u);
    assert_close(layout.interval_start_cells, 0.0f);
    assert_close(layout.interval_end_cells, 2.0f);

    assert(btbh_gpu_radiance_cascade_layout(
        &low,
        64u,
        32u,
        2u,
        4096u,
        &layout
    ));
    assert(layout.probe_spacing_cells == 8u);
    assert(layout.direction_tile_width == 4u);
    assert(layout.direction_tile_height == 4u);
    assert(layout.direction_count == 16u);
    assert(layout.atlas_width == 32u);
    assert(layout.atlas_height == 16u);
    assert_close(layout.interval_start_cells, 6.0f);
    assert_close(layout.interval_end_cells, 14.0f);

    BTBHGPUSceneRadianceCascadesProfile high = {0};
    assert(btbh_gpu_radiance_cascades_profile_for_tier(
        BTBH_GPU_SCENE_RC_HIGH,
        &high
    ));
    assert(high.cascade_count == 5u);
    assert(high.angular_branching == 2u);
    assert(high.interval_branching == 4u);
    assert(high.base_probe_spacing_cells == 1u);
    assert(high.base_direction_tile_width == 8u);
    assert(high.base_direction_tile_height == 8u);
    assert(btbh_gpu_radiance_cascades_direct_resolution_scale(&high) == 3u);
    assert(btbh_gpu_radiance_cascade_layout(
        &high,
        64u,
        32u,
        4u,
        4096u,
        &layout
    ));
    assert(layout.probe_spacing_cells == 16u);
    assert(layout.probe_count_x == 4u);
    assert(layout.probe_count_y == 2u);
    assert(layout.direction_tile_width == 32u);
    assert(layout.direction_tile_height == 32u);
    assert(layout.direction_count == 1024u);
    assert(layout.atlas_width == 128u);
    assert(layout.atlas_height == 64u);
    assert_close(layout.interval_start_cells, 170.0f);
    assert_close(layout.interval_end_cells, 682.0f);

    assert(!btbh_gpu_radiance_cascade_layout(
        &high,
        64u,
        32u,
        4u,
        64u,
        &layout
    ));
    assert(!btbh_gpu_radiance_cascades_profile_for_tier(
        BTBH_GPU_SCENE_RC_OFF,
        &high
    ));
    low.angular_branching = 3u;
    assert(!btbh_gpu_radiance_cascades_profile_valid(&low));
}

static void test_directional_atlas_address_and_children(void) {
    BTBHGPUSceneRadianceCascadesProfile profile = {0};
    BTBHGPUSceneRadianceCascadeLayout layout = {0};
    assert(btbh_gpu_radiance_cascades_profile_for_tier(
        BTBH_GPU_SCENE_RC_HIGH,
        &profile
    ));
    assert(btbh_gpu_radiance_cascade_layout(
        &profile,
        64u,
        32u,
        1u,
        4096u,
        &layout
    ));
    assert(layout.direction_tile_width == 16u);
    assert(layout.direction_tile_height == 8u);

    uint32_t atlas_x = 0u;
    uint32_t atlas_y = 0u;
    assert(btbh_gpu_radiance_cascade_atlas_address(
        &layout,
        5u,
        3u,
        19u,
        &atlas_x,
        &atlas_y
    ));
    assert(atlas_x == 83u);
    assert(atlas_y == 25u);
    assert(!btbh_gpu_radiance_cascade_atlas_address(
        &layout,
        layout.probe_count_x,
        0u,
        0u,
        &atlas_x,
        &atlas_y
    ));

    assert(btbh_gpu_radiance_cascade_layout(
        &profile,
        64u,
        32u,
        0u,
        4096u,
        &layout
    ));
    uint32_t first_child = 0u;
    uint32_t child_count = 0u;
    assert(btbh_gpu_radiance_cascade_direction_children(
        &profile,
        &layout,
        3u,
        &first_child,
        &child_count
    ));
    assert(first_child == 6u);
    assert(child_count == 2u);
}

static void test_radiance_interval_composition(void) {
    const BTBHGPUSceneRadianceInterval near_interval = {
        1.0f, 0.5f, 0.25f, 0.25f
    };
    const BTBHGPUSceneRadianceInterval far_interval = {
        4.0f, 2.0f, 1.0f, 0.5f
    };
    const BTBHGPUSceneRadianceInterval merged =
        btbh_gpu_radiance_interval_merge(near_interval, far_interval);
    assert_close(merged.red, 2.0f);
    assert_close(merged.green, 1.0f);
    assert_close(merged.blue, 0.5f);
    assert_close(merged.transmittance, 0.125f);

    const BTBHGPUSceneRadianceInterval bottom_left = {
        0.0f, 1.0f, 2.0f, 0.0f
    };
    const BTBHGPUSceneRadianceInterval bottom_right = {
        2.0f, 3.0f, 4.0f, 0.2f
    };
    const BTBHGPUSceneRadianceInterval top_left = {
        4.0f, 5.0f, 6.0f, 0.4f
    };
    const BTBHGPUSceneRadianceInterval top_right = {
        6.0f, 7.0f, 8.0f, 0.6f
    };
    const BTBHGPUSceneRadianceInterval interpolated =
        btbh_gpu_radiance_interval_bilinear(
            bottom_left,
            bottom_right,
            top_left,
            top_right,
            0.25f,
            0.5f
        );
    assert_close(interpolated.red, 2.5f);
    assert_close(interpolated.green, 3.5f);
    assert_close(interpolated.blue, 4.5f);
    assert_close(interpolated.transmittance, 0.25f);
}

static void test_reprojected_merge_only_traces_top_cascade(void) {
    for (uint32_t cascade = 0u; cascade < 4u; ++cascade) {
        assert(!btbh_gpu_radiance_cascades_raw_trace_required(
            cascade,
            5u,
            true
        ));
    }
    assert(btbh_gpu_radiance_cascades_raw_trace_required(4u, 5u, true));

    for (uint32_t cascade = 0u; cascade < 5u; ++cascade) {
        assert(btbh_gpu_radiance_cascades_raw_trace_required(
            cascade,
            5u,
            false
        ));
    }

    assert(!btbh_gpu_radiance_cascades_raw_trace_required(0u, 0u, true));
    assert(!btbh_gpu_radiance_cascades_raw_trace_required(5u, 5u, true));
}

static void test_holographic_layout_scales_256_domain_to_512(void) {
    BTBHGPUHolographicRadianceCascadesLayout layout = {0};
    assert(btbh_gpu_holographic_radiance_cascades_layout(
        256u,
        256u,
        BTBH_GPU_HRC_DEFAULT_SCALE,
        8192u,
        BTBH_GPU_HRC_DEFAULT_MEMORY_BUDGET_BYTES,
        &layout
    ));
    assert(layout.output_width == 512u);
    assert(layout.output_height == 512u);
    assert(layout.base_grid_size == 256u);
    assert(layout.level_count == 9u);
    assert(layout.traced_level_count == 3u);
    assert(layout.segment_count == 8u);
    assert(layout.radiance_atlas_width == 512u);
    assert(layout.radiance_atlas_height == 2048u);
    assert(layout.quadrant_atlas_width == 512u);
    assert(layout.quadrant_atlas_height == 2048u);
    assert(layout.levels[0].grid_width == 256u);
    assert(layout.levels[0].direction_count == 2u);
    assert(layout.levels[0].acceleration_direction_count == 3u);
    assert(layout.levels[0].acceleration_atlas_width == 768u);
    assert(layout.levels[8].grid_width == 1u);
    assert(layout.levels[8].direction_count == 512u);
    assert(layout.levels[8].acceleration_atlas_width == 513u);
    assert(layout.working_set_bytes <=
        BTBH_GPU_HRC_DEFAULT_MEMORY_BUDGET_BYTES);
}

static void test_holographic_tiers_have_distinct_scales_and_working_sets(void) {
    assert(btbh_gpu_radiance_cascades_holographic_scale_for_tier(
        BTBH_GPU_SCENE_RC_LOW
    ) == BTBH_GPU_HRC_LOW_SCALE);
    assert(btbh_gpu_radiance_cascades_holographic_scale_for_tier(
        BTBH_GPU_SCENE_RC_HIGH
    ) == BTBH_GPU_HRC_HIGH_SCALE);
    assert(btbh_gpu_radiance_cascades_holographic_scale_for_tier(
        BTBH_GPU_SCENE_RC_OFF
    ) == 0u);
    assert(btbh_gpu_radiance_cascades_holographic_scale_for_tier(
        BTBH_GPU_SCENE_RC_AUTO
    ) == 0u);

    BTBHGPUHolographicRadianceCascadesLayout low = {0};
    BTBHGPUHolographicRadianceCascadesLayout high = {0};
    assert(btbh_gpu_holographic_radiance_cascades_layout(
        256u,
        256u,
        btbh_gpu_radiance_cascades_holographic_scale_for_tier(
            BTBH_GPU_SCENE_RC_LOW
        ),
        8192u,
        BTBH_GPU_HRC_DEFAULT_MEMORY_BUDGET_BYTES,
        &low
    ));
    assert(btbh_gpu_holographic_radiance_cascades_layout(
        256u,
        256u,
        btbh_gpu_radiance_cascades_holographic_scale_for_tier(
            BTBH_GPU_SCENE_RC_HIGH
        ),
        8192u,
        BTBH_GPU_HRC_DEFAULT_MEMORY_BUDGET_BYTES,
        &high
    ));
    assert(low.output_width == 256u);
    assert(low.output_height == 256u);
    assert(high.output_width == 512u);
    assert(high.output_height == 512u);
    assert(low.working_set_bytes < high.working_set_bytes);
}

static void test_holographic_layout_rejects_invalid_or_oversized_domains(void) {
    BTBHGPUHolographicRadianceCascadesLayout layout = {0};
    assert(!btbh_gpu_holographic_radiance_cascades_layout(
        256u,
        128u,
        2u,
        8192u,
        BTBH_GPU_HRC_DEFAULT_MEMORY_BUDGET_BYTES,
        &layout
    ));
    assert(!btbh_gpu_holographic_radiance_cascades_layout(
        300u,
        300u,
        2u,
        8192u,
        BTBH_GPU_HRC_DEFAULT_MEMORY_BUDGET_BYTES,
        &layout
    ));
    assert(!btbh_gpu_holographic_radiance_cascades_layout(
        256u,
        256u,
        2u,
        1024u,
        BTBH_GPU_HRC_DEFAULT_MEMORY_BUDGET_BYTES,
        &layout
    ));
    assert(!btbh_gpu_holographic_radiance_cascades_layout(
        256u,
        256u,
        2u,
        8192u,
        32ull * 1024ull * 1024ull,
        &layout
    ));
    assert(!btbh_gpu_holographic_radiance_cascades_layout(
        512u,
        512u,
        2u,
        8192u,
        BTBH_GPU_HRC_DEFAULT_MEMORY_BUDGET_BYTES,
        &layout
    ));
}

int main(void) {
    test_configuration_validation();
    test_directional_emitter_count_only_includes_spots();
    test_fixed_tiers_apply_immediately();
    test_backend_selection_updates_generation_and_status();
    test_auto_is_gated_and_adjacent();
    test_auto_hysteresis_holds_middle_band();
    test_failure_is_sticky_until_invalidation();
    test_valid_configuration_recovers_invalid_configuration();
    test_tier_dimensions_round_to_threadgroups();
    test_directional_cascade_profiles_and_layout();
    test_directional_atlas_address_and_children();
    test_radiance_interval_composition();
    test_reprojected_merge_only_traces_top_cascade();
    test_holographic_layout_scales_256_domain_to_512();
    test_holographic_tiers_have_distinct_scales_and_working_sets();
    test_holographic_layout_rejects_invalid_or_oversized_domains();
    return 0;
}
