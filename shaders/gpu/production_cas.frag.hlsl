Texture2D<float4> scene_texture : register(t0, space2);
SamplerState scene_sampler : register(s0, space2);

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

float3 post_cas(float2 uv)
{
    const float3 a = scene_sample(
        uv + float2(0.0f, -inverse_resolution.y)
    );
    const float3 b = scene_sample(
        uv + float2(-inverse_resolution.x, 0.0f)
    );
    const float3 e = scene_sample(uv);
    const float3 c = scene_sample(
        uv + float2(inverse_resolution.x, 0.0f)
    );
    const float3 d = scene_sample(
        uv + float2(0.0f, inverse_resolution.y)
    );
    const float minimum_green = min(
        e.g,
        min(min(a.g, b.g), min(c.g, d.g))
    );
    const float maximum_green = max(
        e.g,
        max(max(a.g, b.g), max(c.g, d.g))
    );
    const float amplitude =
        1.0f / lerp(8.0f, 5.0f, saturate(cas_sharpness));
    const float amount = sqrt(
        min(1.0f - maximum_green, minimum_green) /
        max(maximum_green, 0.0001f)
    );
    const float weight = amplitude * amount;
    return
        (e + (a + b + c + d) * weight) /
        (1.0f + 4.0f * weight);
}

float4 main(FragmentInput input) : SV_Target0
{
    return float4(post_cas(input.texture_coordinate), 1.0f);
}
