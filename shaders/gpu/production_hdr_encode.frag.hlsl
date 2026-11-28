// HDR output's encode (RFC 0017): the window's frame (RGBA16F, sRGB-encoded, 1.0 = paper white, highlights above 1)
// into the swapchain image. The CPU reference is btbh_hdr_encode_pixel in src/gpu/btbh_gpu_hdr.h.
Texture2D<float4> frame_texture : register(t0, space2);
SamplerState frame_sampler : register(s0, space2);

cbuffer HDREncodeUniforms : register(b0, space3)
{
    float paper_white_nits;
    // The peak over paper white: light above it is scaled down, the hue kept.
    float highlight_ratio;
    // Extended linear: light times this (paper white / 80 on Windows and gamescope, 1 on Apple's EDR).
    float linear_scale;
    // HDR10: the dither's amplitude (one 10-bit step), 0 for none.
    float dither_scale;
    // 0 HDR10 (BT.2020, PQ), 1 extended linear (BT.709).
    uint encode_mode;
    float3 encode_unused;
};

struct FragmentInput
{
    float4 position : SV_Position;
    float2 texture_coordinate : TEXCOORD0;
};

static const float PQ_M1 = 0.1593017578125f;
static const float PQ_M2 = 78.84375f;
static const float PQ_C1 = 0.8359375f;
static const float PQ_C2 = 18.8515625f;
static const float PQ_C3 = 18.6875f;

float3 pq_encode(float3 y)
{
    const float3 p = pow(saturate(y), PQ_M1);
    return pow((PQ_C1 + PQ_C2 * p) / (1.0f + PQ_C3 * p), PQ_M2);
}

float4 main(FragmentInput input) : SV_Target0
{
    const float3 encoded = frame_texture.Load(int3(int2(input.position.xy), 0)).rgb;
    // The SDR display's decode (gamma 2.2), extended above 1: 1.0 is paper white.
    float3 light = pow(max(encoded, 0.0f), 2.2f);
    const float largest = max(light.r, max(light.g, light.b));
    if (largest > highlight_ratio && largest > 0.0f)
    {
        light *= highlight_ratio / largest;
    }
    if (encode_mode == 0u)
    {
        const float3 nits = light * paper_white_nits;
        const float3 wide = float3(
            dot(float3(0.6274040f, 0.3292820f, 0.0433136f), nits),
            dot(float3(0.0690970f, 0.9195400f, 0.0113612f), nits),
            dot(float3(0.0163916f, 0.0880132f, 0.8955950f), nits)
        );
        float3 signal = pq_encode(wide / 10000.0f);
        // The output pass's interleaved gradient noise, at one 10-bit step.
        const float noise = frac(
            52.9829189f *
            frac(dot(input.position.xy, float2(0.06711056f, 0.00583715f)))
        );
        signal += (noise - 0.5f) * dither_scale;
        return float4(saturate(signal), 1.0f);
    }
    return float4(light * linear_scale, 1.0f);
}
