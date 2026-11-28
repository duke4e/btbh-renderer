// HDR output (RFC 0017): an HDR capture (RGBA16F, sRGB-encoded, highlights above 1) folded into an SDR target. In
// display light (gamma 2.2) the largest channel M above the knee k is brought into (k, 1]:
// F(M) = k + (M - k) / (1 + (P - 1)(M - k) / ((1 - k)(P - k))), the hue kept, then encoded back. It maps the peak P to
// 1 and is the identity when P is 1. The CPU reference is btbh_hdr_fold_pixel in src/gpu/btbh_gpu_hdr.h.
Texture2D<float4> frame_texture : register(t0, space2);
SamplerState frame_sampler : register(s0, space2);

cbuffer HDRFoldUniforms : register(b0, space3)
{
    // x the knee k, y the peak over paper white P; zw unused.
    float4 fold_parameters;
};

struct FragmentInput
{
    float4 position : SV_Position;
    float2 texture_coordinate : TEXCOORD0;
};

float4 main(FragmentInput input) : SV_Target0
{
    const float4 value = frame_texture.Load(int3(int2(input.position.xy), 0));
    float3 light = pow(max(value.rgb, 0.0f), 2.2f);
    const float largest = max(light.r, max(light.g, light.b));
    const float knee = fold_parameters.x;
    const float ratio = fold_parameters.y;
    // At or below the knee, or with no highlights (P = 1), the value as it is.
    if (largest <= knee || ratio <= 1.0f)
    {
        return value;
    }
    const float over = largest - knee;
    const float folded = knee + over / (1.0f + (ratio - 1.0f) * over / ((1.0f - knee) * (ratio - knee)));
    light *= folded / largest;
    return float4(pow(light, 1.0f / 2.2f), value.a);
}
