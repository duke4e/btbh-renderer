// Bloom upsample: a 9-tap tent of the smaller (already accumulated) level added to this level's
// downsample, weighted by the scatter amount.
Texture2D<float4> lower_texture : register(t0, space2);
SamplerState lower_sampler : register(s0, space2);
Texture2D<float4> current_texture : register(t1, space2);
SamplerState current_sampler : register(s1, space2);

cbuffer BloomUniforms : register(b0, space3)
{
    // xy = 1 / lower level size, zw = unused
    float4 source_texel_threshold;
    // x = unused, y = scatter (0...1), zw = unused
    float4 pass_parameters;
};

struct FragmentInput
{
    float4 position : SV_Position;
    float2 texture_coordinate : TEXCOORD0;
};

float3 sample_lower(float2 uv)
{
    return lower_texture.SampleLevel(lower_sampler, uv, 0.0f).rgb;
}

float4 main(FragmentInput input) : SV_Target0
{
    const float2 uv = input.texture_coordinate;
    const float2 t = source_texel_threshold.xy;
    const float3 tent =
        (sample_lower(uv + t * float2(-1.0f, -1.0f)) +
         sample_lower(uv + t * float2(1.0f, -1.0f)) +
         sample_lower(uv + t * float2(-1.0f, 1.0f)) +
         sample_lower(uv + t * float2(1.0f, 1.0f))) * (1.0f / 16.0f) +
        (sample_lower(uv + t * float2(0.0f, -1.0f)) +
         sample_lower(uv + t * float2(-1.0f, 0.0f)) +
         sample_lower(uv + t * float2(1.0f, 0.0f)) +
         sample_lower(uv + t * float2(0.0f, 1.0f))) * (2.0f / 16.0f) +
        sample_lower(uv) * (4.0f / 16.0f);
    const float3 current = current_texture.SampleLevel(current_sampler, uv, 0.0f).rgb;
    // Energy-conserving blend: scatter 0 keeps only this level, 1 only the wider blur.
    return float4(lerp(current, tent, pass_parameters.y), 1.0f);
}
