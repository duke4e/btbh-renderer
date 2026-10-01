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

static void test_status_reports_the_holographic_backend(void) {
    BTBHGPUSceneRadianceCascadesState state;
    btbh_gpu_radiance_cascades_state_initialize(&state);
    assert(state.requested_backend ==
        BTBH_GPU_SCENE_RC_BACKEND_HOLOGRAPHIC);
    assert(state.active_backend ==
        BTBH_GPU_SCENE_RC_BACKEND_HOLOGRAPHIC);
    BTBHGPUSceneRadianceCascades value =
        config(BTBH_GPU_SCENE_RC_HIGH);
    assert(btbh_gpu_radiance_cascades_state_configure(&state, &value));

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

static void test_direct_light_resolution_follows_tier(void) {
    assert(btbh_gpu_radiance_cascades_direct_resolution_scale(
        BTBH_GPU_SCENE_RC_HIGH
    ) == 3u);
    assert(btbh_gpu_radiance_cascades_direct_resolution_scale(
        BTBH_GPU_SCENE_RC_LOW
    ) == 1u);
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
    test_status_reports_the_holographic_backend();
    test_auto_is_gated_and_adjacent();
    test_auto_hysteresis_holds_middle_band();
    test_failure_is_sticky_until_invalidation();
    test_valid_configuration_recovers_invalid_configuration();
    test_tier_dimensions_round_to_threadgroups();
    test_direct_light_resolution_follows_tier();
    test_holographic_layout_scales_256_domain_to_512();
    test_holographic_tiers_have_distinct_scales_and_working_sets();
    test_holographic_layout_rejects_invalid_or_oversized_domains();
    return 0;
}
