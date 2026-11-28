Texture2D<float4> scene_texture : register(t0, space2);
SamplerState scene_sampler : register(s0, space2);

#include "production_tonemap.hlsli"
#if BTBH_HDR_OUTPUT
#include "production_hdr.hlsli"
#endif

cbuffer PostUniforms : register(b0, space3)
{
    float2 inverse_resolution;
    float tonemap_white;
    float cas_sharpness;
    float2 focus_point;
    float2 focus_direction;
    float focus_half_width;
    float maximum_radius_pixels;
    float blur_curve;
    float highlight_threshold;
    float highlight_gain;
    uint lut_enabled;
    uint cas_enabled;
    uint tilt_shift_enabled;
    uint ssao_enabled;
    uint tonemap_mode;
    float exposure;
    float contrast;
    float4x4 inverse_view_projection;
    float projection_scale_y;
    float saturation;
#if BTBH_HDR_OUTPUT
    // BTBHGPUPostUniforms' tail (unused here), then BTBHGPUPostHDRUniforms' two vectors (production_hdr.hlsli).
    float2 hdr_unused_ssr_options;
    float4x4 hdr_unused_view_projection;
    float4 hdr_unused_ssr_controls;
    float4 hdr_unused_ao_controls;
    float4 hdr_unused_ssr_hit_options;
    float4 hdr_tone;
    float4 hdr_misc;
#endif
};

struct FragmentInput
{
    float4 position : SV_Position;
    float2 texture_coordinate : TEXCOORD0;
};

float3 linear_to_srgb(float3 color)
{
    return max(
        1.055f * pow(color, 0.416666667f) - 0.055f,
        0.0f
    );
}

float3 output_color(float3 color, float2 fragment_coordinate)
{
    color = linear_to_srgb(btbh_tone_map_for_output(
        color,
        tonemap_mode,
        tonemap_white,
        lut_enabled,
        exposure,
        contrast,
        saturation
    ));
    const float noise = frac(
        52.9829189f *
        frac(dot(fragment_coordinate, float2(0.06711056f, 0.00583715f)))
    );
    return color + (noise - 0.5f) / 255.0f;
}

#if BTBH_HDR_OUTPUT
// The SDR result before its dither, lifted by the highlight gain. The heat comes from the LUT pass's alpha when it
// graded the scene (it is the last pass to see the scene's values), else from the input.
float4 output_color_hdr(float4 scene_value)
{
    const float3 encoded = linear_to_srgb(btbh_tone_map_for_output(
        scene_value.rgb,
        tonemap_mode,
        tonemap_white,
        lut_enabled,
        exposure,
        contrast,
        saturation
    ));
    const float heat = lut_enabled != 0u ? scene_value.a : btbh_hdr_heat(scene_value.rgb, exposure);
    return float4(
        btbh_hdr_lift(encoded, btbh_hdr_gain(heat, hdr_tone)),
        hdr_misc.x != 0.0f ? heat : 1.0f
    );
}
#endif

float4 main(FragmentInput input) : SV_Target0
{
    const float4 scene_sample = scene_texture.SampleLevel(
        scene_sampler,
        input.texture_coordinate,
        0.0f
    );
#if BTBH_HDR_OUTPUT
    return output_color_hdr(scene_sample);
#else
    const float2 legacy_fragment_coordinate = float2(
        input.position.x,
        1.0f / inverse_resolution.y - input.position.y
    );
    return float4(
        output_color(
            scene_sample.rgb,
            legacy_fragment_coordinate
        ),
        1.0f
    );
#endif
}
