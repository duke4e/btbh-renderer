Texture2D<float4> scene_texture : register(t0, space2);
SamplerState scene_sampler : register(s0, space2);

#include "production_tonemap.hlsli"

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
    float2 post_padding;
    float4x4 inverse_view_projection;
    float projection_scale_y;
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
        lut_enabled
    ));
    const float noise = frac(
        52.9829189f *
        frac(dot(fragment_coordinate, float2(0.06711056f, 0.00583715f)))
    );
    return color + (noise - 0.5f) / 255.0f;
}

float4 main(FragmentInput input) : SV_Target0
{
    const float4 scene_sample = scene_texture.SampleLevel(
        scene_sampler,
        input.texture_coordinate,
        0.0f
    );
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
}
