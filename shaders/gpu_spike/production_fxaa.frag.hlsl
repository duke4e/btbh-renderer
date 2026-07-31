Texture2D<float4> post_texture : register(t0, space2);
SamplerState post_sampler : register(s0, space2);

cbuffer FXAAUniforms : register(b0, space3)
{
    float4 metrics;
};

struct FragmentInput
{
    float4 position : SV_Position;
    float2 texture_coordinate : TEXCOORD0;
};

float luma(float3 color)
{
    return dot(color, float3(0.299f, 0.587f, 0.114f));
}

float3 sample_post(float2 uv)
{
    return post_texture.Sample(post_sampler, uv).rgb;
}

float3 fxaa(float2 uv)
{
    const float2 texel = metrics.xy;
    const float3 rgb_nw = sample_post(uv + texel * float2(-1.0f, -1.0f));
    const float3 rgb_ne = sample_post(uv + texel * float2(1.0f, -1.0f));
    const float3 rgb_sw = sample_post(uv + texel * float2(-1.0f, 1.0f));
    const float3 rgb_se = sample_post(uv + texel * float2(1.0f, 1.0f));
    const float3 rgb_m = sample_post(uv);
    const float luma_nw = luma(rgb_nw);
    const float luma_ne = luma(rgb_ne);
    const float luma_sw = luma(rgb_sw);
    const float luma_se = luma(rgb_se);
    const float luma_m = luma(rgb_m);
    const float luma_min = min(
        luma_m,
        min(min(luma_nw, luma_ne), min(luma_sw, luma_se))
    );
    const float luma_max = max(
        luma_m,
        max(max(luma_nw, luma_ne), max(luma_sw, luma_se))
    );
    float2 direction = float2(
        -((luma_nw + luma_ne) - (luma_sw + luma_se)),
        (luma_nw + luma_sw) - (luma_ne + luma_se)
    );
    const float direction_reduce = max(
        (luma_nw + luma_ne + luma_sw + luma_se) * 0.03125f,
        0.0078125f
    );
    const float reciprocal_minimum =
        1.0f / (min(abs(direction.x), abs(direction.y)) + direction_reduce);
    direction = clamp(
        direction * reciprocal_minimum,
        float2(-8.0f, -8.0f),
        float2(8.0f, 8.0f)
    ) * texel;
    const float3 rgb_a = 0.5f * (
        sample_post(uv + direction * (1.0f / 3.0f - 0.5f)) +
        sample_post(uv + direction * (2.0f / 3.0f - 0.5f))
    );
    const float3 rgb_b =
        rgb_a * 0.5f +
        0.25f * (
            sample_post(uv + direction * -0.5f) +
            sample_post(uv + direction * 0.5f)
        );
    const float luma_b = luma(rgb_b);
    return luma_b < luma_min || luma_b > luma_max
        ? rgb_a
        : rgb_b;
}

float4 main(FragmentInput input) : SV_Target0
{
    return float4(fxaa(input.texture_coordinate), 1.0f);
}
