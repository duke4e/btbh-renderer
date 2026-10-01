#define SMAA_PRESET_MEDIUM

Texture2D<float4> edgesTex : register(t0, space2);
SamplerState edgesTexSampler : register(s0, space2);
Texture2D<float4> areaTex : register(t1, space2);
SamplerState areaTexSampler : register(s1, space2);
Texture2D<float4> searchTex : register(t2, space2);
SamplerState searchTexSampler : register(s2, space2);

#define colorTexSampler edgesTexSampler
#define blendTexSampler edgesTexSampler
#include "production_smaa_common.hlsli"

struct FragmentInput
{
    float2 texture_coordinate : TEXCOORD0;
};

float4 main(FragmentInput input) : SV_Target0
{
    float2 pixel_coordinate;
    float4 offsets[3];
    SMAABlendingWeightCalculationVS(
        input.texture_coordinate,
        pixel_coordinate,
        offsets
    );
    return SMAABlendingWeightCalculationPS(
        input.texture_coordinate,
        pixel_coordinate,
        offsets,
        edgesTex,
        areaTex,
        searchTex,
        int4(0, 0, 0, 0)
    );
}
