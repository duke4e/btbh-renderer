// Bloom: one step up the chain: the smaller level, spread with a tent filter, added to this one.
Texture2D<float4> low_texture : register(t0, space2);
SamplerState low_sampler : register(s0, space2);
Texture2D<float4> high_texture : register(t1, space2);
SamplerState high_sampler : register(s1, space2);

cbuffer BloomUniforms : register(b0, space3)
{
    float2 source_texel;
    float threshold;
    float knee;
    float intensity;
    float radius;
    uint prefilter;
    uint padding;
};

struct FragmentInput
{
    float4 position : SV_Position;
    float2 texture_coordinate : TEXCOORD0;
};

float3 low(float2 uv)
{
    return low_texture.Sample(low_sampler, uv).rgb;
}

float4 main(FragmentInput input) : SV_Target0
{
    const float2 uv = input.texture_coordinate;
    const float2 t = source_texel * radius;
    float3 spread =
        low(uv + t * float2(-1.0f, -1.0f)) +
        low(uv + t * float2(0.0f, -1.0f)) * 2.0f +
        low(uv + t * float2(1.0f, -1.0f)) +
        low(uv + t * float2(-1.0f, 0.0f)) * 2.0f +
        low(uv) * 4.0f +
        low(uv + t * float2(1.0f, 0.0f)) * 2.0f +
        low(uv + t * float2(-1.0f, 1.0f)) +
        low(uv + t * float2(0.0f, 1.0f)) * 2.0f +
        low(uv + t * float2(1.0f, 1.0f));
    spread *= 1.0f / 16.0f;
    return float4(high_texture.Sample(high_sampler, uv).rgb + spread, 1.0f);
}
