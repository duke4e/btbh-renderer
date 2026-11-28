// AMD FidelityFX Super Resolution 1.0, Robust Contrast Adaptive Sharpening (RCAS): a port of FsrRcasF from
// ffx_fsr1.h (MIT licence, Copyright (c) 2021 Advanced Micro Devices, Inc.), run on EASU's output. It
// sharpens as far as the cross of neighbours allows without clipping, so it adds no ringing.
Texture2D<float4> source_texture : register(t0, space2);
SamplerState source_sampler : register(s0, space2);

cbuffer UpscaleUniforms : register(b0, space3)
{
    // 1 / width, 1 / height, width, height of the EASU output; w of the second vector is unused.
    float4 source_metrics;
#if BTBH_HDR_OUTPUT
    // HDR output (RFC 0017): x the tail's peak (P^(1/2.2)), yzw unused.
    float4 hdr_peak;
#endif
};

#if BTBH_HDR_OUTPUT
#define RCAS_PEAK hdr_peak.x
#define RCAS_PEAK4 (4.0f * hdr_peak.x)
#define RCAS_CLAMP(value) clamp(value, 0.0f, hdr_peak.x)
#else
#define RCAS_PEAK 1.0f
#define RCAS_PEAK4 4.0f
#define RCAS_CLAMP(value) saturate(value)
#endif

struct FragmentInput
{
    float4 position : SV_Position;
    float2 texture_coordinate : TEXCOORD0;
};

// Sharpness in stops below the maximum (0 is the strongest); 0.2 is AMD's default.
static const float RCAS_SHARPNESS = 0.2f;
static const float RCAS_LIMIT = 0.25f - 1.0f / 16.0f;

float3 load_source(int2 texel)
{
    const int2 limit = int2(source_metrics.zw) - 1;
    return source_texture.Load(int3(clamp(texel, int2(0, 0), limit), 0)).rgb;
}

float4 main(FragmentInput input) : SV_Target0
{
    const int2 texel = int2(input.position.xy);
    //    b
    //  d e f
    //    h
    const float3 b = load_source(texel + int2(0, -1));
    const float3 d = load_source(texel + int2(-1, 0));
    const float3 e = load_source(texel);
    const float3 f = load_source(texel + int2(1, 0));
    const float3 h = load_source(texel + int2(0, 1));

    const float3 minimum = min(min(b, d), min(f, h));
    const float3 maximum = max(max(b, d), max(f, h));
    // How far the lobe may go before the result leaves [0, RCAS_PEAK] in any channel.
    const float3 hit_minimum = min(minimum, e) / max(4.0f * maximum, 1e-5f);
    const float3 hit_maximum = (RCAS_PEAK - max(maximum, e)) / min(4.0f * minimum - RCAS_PEAK4, -1e-5f);
    const float3 lobe_rgb = max(-hit_minimum, hit_maximum);
    const float lobe = max(-RCAS_LIMIT, min(max(lobe_rgb.r, max(lobe_rgb.g, lobe_rgb.b)), 0.0f))
        * exp2(-RCAS_SHARPNESS);
    const float3 color = (lobe * (b + d + f + h) + e) / (4.0f * lobe + 1.0f);
    return float4(RCAS_CLAMP(color), 1.0f);
}
