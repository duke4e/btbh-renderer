// Bloom: one step down the chain of half-size levels. The first step (prefilter) keeps only what
// is brighter than the threshold, with a soft knee, and averages against fireflies.
Texture2D<float4> source_texture : register(t0, space2);
SamplerState source_sampler : register(s0, space2);

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

float3 fetch(float2 uv)
{
    return source_texture.Sample(source_sampler, uv).rgb;
}

float luminance(float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

float3 bright_part(float3 color)
{
    const float brightness = max(color.r, max(color.g, color.b));
    float soft = clamp(brightness - threshold + knee, 0.0f, 2.0f * knee);
    soft = soft * soft / (4.0f * knee + 0.00001f);
    return color * (max(soft, brightness - threshold) / max(brightness, 0.00001f));
}

float4 main(FragmentInput input) : SV_Target0
{
    const float2 uv = input.texture_coordinate;
    const float2 t = source_texel;
    // Thirteen taps (Jimenez, "Next generation post processing in Call of Duty", 2014).
    const float3 a = fetch(uv + t * float2(-2.0f, -2.0f));
    const float3 b = fetch(uv + t * float2(0.0f, -2.0f));
    const float3 c = fetch(uv + t * float2(2.0f, -2.0f));
    const float3 d = fetch(uv + t * float2(-1.0f, -1.0f));
    const float3 e = fetch(uv + t * float2(1.0f, -1.0f));
    const float3 f = fetch(uv + t * float2(-2.0f, 0.0f));
    const float3 g = fetch(uv);
    const float3 h = fetch(uv + t * float2(2.0f, 0.0f));
    const float3 i = fetch(uv + t * float2(-1.0f, 1.0f));
    const float3 j = fetch(uv + t * float2(1.0f, 1.0f));
    const float3 k = fetch(uv + t * float2(-2.0f, 2.0f));
    const float3 l = fetch(uv + t * float2(0.0f, 2.0f));
    const float3 m = fetch(uv + t * float2(2.0f, 2.0f));

    float3 groups[5] = {
        (d + e + i + j) * 0.25f,
        (a + b + f + g) * 0.25f,
        (b + c + g + h) * 0.25f,
        (f + g + k + l) * 0.25f,
        (g + h + l + m) * 0.25f
    };
    const float weights[5] = { 0.5f, 0.125f, 0.125f, 0.125f, 0.125f };
    float3 result = float3(0.0f, 0.0f, 0.0f);
    if (prefilter != 0u)
    {
        float total = 0.0f;
        for (int n = 0; n < 5; ++n)
        {
            const float3 part = bright_part(groups[n]);
            const float w = weights[n] / (1.0f + luminance(part));
            result += part * w;
            total += w;
        }
        result /= max(total, 0.00001f);
    }
    else
    {
        for (int n = 0; n < 5; ++n)
        {
            result += groups[n] * weights[n];
        }
    }
    return float4(result, 1.0f);
}
