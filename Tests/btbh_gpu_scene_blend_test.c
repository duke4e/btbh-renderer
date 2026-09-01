#include "btbh_gpu_internal.h"

#include <assert.h>
#include <math.h>

static void assert_factors(
    BTBHGPUSceneBlendMode mode,
    bool transparency_enabled,
    SDL_GPUBlendFactor source,
    SDL_GPUBlendFactor destination
) {
    SDL_GPUColorTargetBlendState state;
    btbh_gpu_scene_resolve_blend_state(
        mode,
        transparency_enabled,
        &state
    );
    assert(state.enable_blend);
    assert(state.color_blend_op == SDL_GPU_BLENDOP_ADD);
    assert(state.alpha_blend_op == SDL_GPU_BLENDOP_ADD);
    assert(state.src_color_blendfactor == source);
    assert(state.dst_color_blendfactor == destination);
    assert(state.src_alpha_blendfactor == source);
    assert(state.dst_alpha_blendfactor == destination);
}

static BTBHGPUSceneLighting valid_lighting(void) {
    BTBHGPUSceneLighting lighting = {0};
    lighting.spot_range = 8.0f;
    lighting.spot_falloff = 1.0f;
    lighting.spot_inner_cutoff = 1.0f;
    lighting.spot_outer_cutoff = 0.0f;
    return lighting;
}

static void test_omni_shadow_validation(void) {
    BTBHGPUSceneLighting lighting = valid_lighting();
    assert(btbh_gpu_scene_lighting_valid(&lighting));

    lighting.spot_shadow_is_omni = 2u;
    assert(!btbh_gpu_scene_lighting_valid(&lighting));

    lighting.spot_shadow_is_omni = 1u;
    lighting.spot_shadow_near = 0.05f;
    assert(btbh_gpu_scene_lighting_valid(&lighting));
    lighting.spot_shadow_near = 0.0f;
    assert(!btbh_gpu_scene_lighting_valid(&lighting));
    lighting.spot_shadow_near = lighting.spot_range;
    assert(!btbh_gpu_scene_lighting_valid(&lighting));

    lighting = valid_lighting();
    lighting.omni_shadow_view_projection[5][15] = NAN;
    assert(!btbh_gpu_scene_lighting_valid(&lighting));
}

int main(void) {
    test_omni_shadow_validation();
    BTBHGPUSceneGeometryBatch batch = {0};
    batch.unlit_enabled = 1u;
    batch.transparency_enabled = 1u;
    assert(btbh_gpu_scene_uses_fast_unlit_pipeline(&batch));
    batch.surface_variant = 1u;
    assert(!btbh_gpu_scene_uses_fast_unlit_pipeline(&batch));
    batch.surface_variant = 0u;
    batch.transparency_enabled = 0u;
    assert(!btbh_gpu_scene_uses_fast_unlit_pipeline(&batch));
    batch.blend_mode = BTBH_GPU_SCENE_BLEND_ADDITIVE;
    assert(btbh_gpu_scene_uses_fast_unlit_pipeline(&batch));
    batch.unlit_enabled = 0u;
    assert(!btbh_gpu_scene_uses_fast_unlit_pipeline(&batch));

    assert(!btbh_gpu_scene_uses_transparent_pass(
        BTBH_GPU_SCENE_BLEND_ALPHA,
        false
    ));
    assert(btbh_gpu_scene_uses_transparent_pass(
        BTBH_GPU_SCENE_BLEND_ALPHA,
        true
    ));
    assert(btbh_gpu_scene_uses_transparent_pass(
        BTBH_GPU_SCENE_BLEND_ADDITIVE,
        false
    ));
    assert(btbh_gpu_scene_uses_transparent_pass(
        BTBH_GPU_SCENE_BLEND_MULTIPLIED,
        false
    ));
    assert(btbh_gpu_scene_uses_transparent_pass(
        BTBH_GPU_SCENE_BLEND_PREMULTIPLIED_ALPHA,
        false
    ));

    assert_factors(
        BTBH_GPU_SCENE_BLEND_ALPHA,
        true,
        SDL_GPU_BLENDFACTOR_SRC_ALPHA,
        SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA
    );
    assert_factors(
        BTBH_GPU_SCENE_BLEND_ADDITIVE,
        false,
        SDL_GPU_BLENDFACTOR_ONE,
        SDL_GPU_BLENDFACTOR_ONE
    );
    assert_factors(
        BTBH_GPU_SCENE_BLEND_ADDITIVE,
        true,
        SDL_GPU_BLENDFACTOR_SRC_ALPHA,
        SDL_GPU_BLENDFACTOR_ONE
    );
    assert_factors(
        BTBH_GPU_SCENE_BLEND_MULTIPLIED,
        false,
        SDL_GPU_BLENDFACTOR_DST_COLOR,
        SDL_GPU_BLENDFACTOR_ZERO
    );
    assert_factors(
        BTBH_GPU_SCENE_BLEND_PREMULTIPLIED_ALPHA,
        false,
        SDL_GPU_BLENDFACTOR_ONE,
        SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA
    );
    return 0;
}
