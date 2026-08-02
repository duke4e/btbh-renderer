Texture2D<float4> scene_texture : register(t0, space2);
SamplerState scene_sampler : register(s0, space2);
Texture2D<float4> lut_texture : register(t1, space2);
SamplerState lut_sampler : register(s1, space2);

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
    float3 ssao_padding;
    float4x4 inverse_view_projection;
    float projection_scale_y;
};

struct FragmentInput
{
    float4 position : SV_Position;
    float2 texture_coordinate : TEXCOORD0;
};

float3 scene_sample(float2 uv)
{
    return scene_texture.Sample(scene_sampler, uv).rgb;
}

float3 grade_lut(float3 color)
{
    const float lut_size = 64.0f;
    const float tiles_per_row = 8.0f;
    const float texture_size = 512.0f;
    const float3 raw = saturate(pow(max(color, 0.0f), 1.0f / 2.2f));
    const float blue_index = raw.b * (lut_size - 1.0f);
    const float lower_index = floor(blue_index);
    const float upper_index = min(lut_size - 1.0f, lower_index + 1.0f);
    const float2 lower_tile = float2(
        fmod(lower_index, tiles_per_row),
        floor(lower_index / tiles_per_row)
    );
    const float2 upper_tile = float2(
        fmod(upper_index, tiles_per_row),
        floor(upper_index / tiles_per_row)
    );
    const float2 tile_size = 1.0f / tiles_per_row;
    const float2 texel = 0.5f / texture_size;
    const float2 sample_uv =
        texel + raw.rg * ((lut_size - 1.0f) / texture_size);
    const float3 lower = lut_texture.Sample(
        lut_sampler,
        lower_tile * tile_size + sample_uv
    ).rgb;
    const float3 upper = lut_texture.Sample(
        lut_sampler,
        upper_tile * tile_size + sample_uv
    ).rgb;
    return pow(
        max(lerp(lower, upper, frac(blue_index)), 0.0f),
        2.2f
    );
}

float3 post_lut(float2 uv)
{
    return grade_lut(scene_sample(uv));
}

float4 main(FragmentInput input) : SV_Target0
{
    return float4(post_lut(input.texture_coordinate), 1.0f);
}
