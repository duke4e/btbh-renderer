// Bloom downsample (13-tap, Jimenez 2014). The first level also applies the soft threshold and a
// Karis average so single bright pixels do not flicker.
Texture2D<float4> source_texture : register(t0, space2);
SamplerState source_sampler : register(s0, space2);

cbuffer BloomUniforms : register(b0, space3)
{
    // xy = 1 / source size, z = threshold, w = soft knee (0...1)
    float4 source_texel_threshold;
    // x = 1 on the first (prefilter) level, y = upsample scatter, zw = unused
    float4 pass_parameters;
};

struct FragmentInput
{
    float4 position : SV_Position;
    float2 texture_coordinate : TEXCOORD0;
};

float luma(float3 c) { return dot(c, float3(0.2126f, 0.7152f, 0.0722f)); }

float3 prefilter(float3 c)
{
    const float threshold = source_texel_threshold.z;
    const float knee = threshold * source_texel_threshold.w + 1.0e-5f;
    const float brightness = max(c.r, max(c.g, c.b));
    float soft = clamp(brightness - threshold + knee, 0.0f, 2.0f * knee);
    soft = soft * soft / (4.0f * knee);
    const float contribution = max(soft, brightness - threshold) / max(brightness, 1.0e-5f);
    return c * contribution;
}

float3 sample_source(float2 uv)
{
    return source_texture.SampleLevel(source_sampler, uv, 0.0f).rgb;
}

float3 karis_group(float3 a, float3 b, float3 c, float3 d)
{
    const float3 average = (a + b + c + d) * 0.25f;
    return average / (1.0f + luma(average));
}

float4 main(FragmentInput input) : SV_Target0
{
    const float2 uv = input.texture_coordinate;
    const float2 t = source_texel_threshold.xy;
    const float3 a = sample_source(uv + t * float2(-2.0f, -2.0f));
    const float3 b = sample_source(uv + t * float2(0.0f, -2.0f));
    const float3 c = sample_source(uv + t * float2(2.0f, -2.0f));
    const float3 d = sample_source(uv + t * float2(-2.0f, 0.0f));
    const float3 e = sample_source(uv);
    const float3 f = sample_source(uv + t * float2(2.0f, 0.0f));
    const float3 g = sample_source(uv + t * float2(-2.0f, 2.0f));
    const float3 h = sample_source(uv + t * float2(0.0f, 2.0f));
    const float3 i = sample_source(uv + t * float2(2.0f, 2.0f));
    const float3 j = sample_source(uv + t * float2(-1.0f, -1.0f));
    const float3 k = sample_source(uv + t * float2(1.0f, -1.0f));
    const float3 l = sample_source(uv + t * float2(-1.0f, 1.0f));
    const float3 m = sample_source(uv + t * float2(1.0f, 1.0f));

    float3 result;
    if (pass_parameters.x > 0.5f)
    {
        // Karis average per 2x2 group, then the soft threshold.
        result =
            karis_group(j, k, l, m) * 0.5f +
            karis_group(a, b, d, e) * 0.125f +
            karis_group(b, c, e, f) * 0.125f +
            karis_group(d, e, g, h) * 0.125f +
            karis_group(e, f, h, i) * 0.125f;
        // Undo the Karis compression's average scale before thresholding.
        result = prefilter(result / max(1.0f - luma(result), 1.0e-3f));
    }
    else
    {
        result =
            (j + k + l + m) * 0.125f +
            (a + c + g + i) * 0.03125f +
            (b + d + f + h) * 0.0625f +
            e * 0.125f;
    }
    return float4(max(result, 0.0f), 1.0f);
}
