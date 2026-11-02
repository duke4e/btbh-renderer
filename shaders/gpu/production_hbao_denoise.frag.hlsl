// The occlusion evened out (half resolution): a 5x5 box whose border taps count half, so that each output takes
// each of production_hbao.frag.hlsl's 16 turns of its samples once (its 4x4 pattern leaves no trace), weighted by
// how near each tap's view depth is to the centre's (edges stay sharp). Surfaces that take no occlusion (-1) are
// left out, and keep -1. In and out: the visibility and the view depth.
Texture2D<float2> ssao_texture : register(t0, space2);
SamplerState ssao_sampler : register(s0, space2);

#include "production_hbao.hlsli"

struct FragmentInput
{
    float4 position : SV_Position;
    float2 texture_coordinate : TEXCOORD0;
};

// (Texel centres through the sampler: a texture read without one would share another's slot in Metal.)
float2 texel_at(int2 pixel, int2 size)
{
    return ssao_texture.SampleLevel(ssao_sampler, (float2(clamp(pixel, int2(0, 0), size - 1)) + 0.5f) / float2(size), 0.0f);
}

float2 main(FragmentInput input) : SV_Target0
{
    uint width, height;
    ssao_texture.GetDimensions(width, height);
    const int2 size = int2(width, height);
    const int2 pixel = clamp(int2(input.position.xy), int2(0, 0), size - 1);
    const float2 centre = texel_at(pixel, size);
    if (ssao_enabled == 0u || centre.x < 0.0f)
    {
        return centre;
    }
    // (How far in depth a tap may lie from the centre: a quarter of the radius, more where a pixel spans more
    // depth, far off and on slopes.)
    const float radius = ssao_radius > 0.0f ? ssao_radius : 1.0f;
    const float tolerance = 0.25f * radius + 0.006f * centre.y;
    float sum = 0.0f;
    float weight_sum = 0.0f;
    [unroll]
    for (int y = -2; y <= 2; ++y)
    {
        [unroll]
        for (int x = -2; x <= 2; ++x)
        {
            const float2 tap = texel_at(pixel + int2(x, y), size);
            const float box = (abs(x) == 2 ? 0.5f : 1.0f) * (abs(y) == 2 ? 0.5f : 1.0f);
            const float weight = tap.x < 0.0f ? 0.0f : box * saturate(1.0f - abs(tap.y - centre.y) / tolerance);
            sum += tap.x * weight;
            weight_sum += weight;
        }
    }
    return float2(weight_sum > 0.0001f ? sum / weight_sum : centre.x, centre.y);
}
