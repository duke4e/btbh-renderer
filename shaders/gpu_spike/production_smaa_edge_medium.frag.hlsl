#define SMAA_PRESET_MEDIUM

Texture2D<float4> colorTex : register(t0, space2);
SamplerState colorTexSampler : register(s0, space2);

#define blendTexSampler colorTexSampler
#define edgesTexSampler colorTexSampler
#define areaTexSampler colorTexSampler
#define searchTexSampler colorTexSampler
#include "production_smaa_common.hlsli"

struct FragmentInput
{
    float2 texture_coordinate : TEXCOORD0;
};

float4 main(FragmentInput input) : SV_Target0
{
    float4 offsets[3];
    SMAAEdgeDetectionVS(input.texture_coordinate, offsets);
    const float2 edges = SMAAColorEdgeDetectionPS(
        input.texture_coordinate,
        offsets,
        colorTex
    );
    return float4(edges, 0.0f, 0.0f);
}
