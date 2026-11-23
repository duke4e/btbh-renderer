#ifndef PRODUCTION_LIGHT_FACTOR_HLSLI
#define PRODUCTION_LIGHT_FACTOR_HLSLI

// BTBH_GPU_SCENE_BATCH_INSTANCE_LIGHT_FACTOR (since 0.11.0): bit 9 (512) of the fragment block's unlit_enabled. The
// instance's custom x scales its material's emission, its glass's share of lit panes and its lamp lens's glow
// (production_model.frag, and the emission in production_model_unlit.frag), and its emission in the radiance
// cascades' source field (production_rc_material_source.frag).
static const uint btbh_light_factor_bit = 512u;

// The instance's light: custom x clamped to 0..1 (a NaN is 0) and taken to the nearest 1/4096, so that a constant
// interpolated across a triangle, which may come a rounding off, is exact at 0, 1 and every k/4096. 1 without the bit.
float btbh_instance_light_factor(uint unlit_enabled, float4 instance_custom)
{
    if ((unlit_enabled & btbh_light_factor_bit) == 0u)
    {
        return 1.0f;
    }
    const float clamped = instance_custom.x > 0.0f ? min(instance_custom.x, 1.0f) : 0.0f;
    return floor(clamped * 4096.0f + 0.5f) * (1.0f / 4096.0f);
}

#endif
