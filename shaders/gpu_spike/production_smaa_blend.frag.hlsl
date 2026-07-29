#define SMAA_PRESET_HIGH

Texture2D<float4> colorTex : register(t0, space2);
SamplerState colorTexSampler : register(s0, space2);
Texture2D<float4> blendTex : register(t1, space2);
SamplerState blendTexSampler : register(s1, space2);

#define edgesTexSampler blendTexSampler
#define areaTexSampler blendTexSampler
#define searchTexSampler blendTexSampler
#include "production_smaa_common.hlsli"

struct FragmentInput
{
    float2 texture_coordinate : TEXCOORD0;
};

float4 main(FragmentInput input) : SV_Target0
{
    float4 offset;
    SMAANeighborhoodBlendingVS(input.texture_coordinate, offset);
    return SMAANeighborhoodBlendingPS(
        input.texture_coordinate,
        offset,
        colorTex,
        blendTex
    );
}
