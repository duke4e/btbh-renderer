cbuffer SMAAUniforms : register(b0, space3)
{
    float4 smaa_rt_metrics;
};

float4 btbh_smaa_point_sample(
    Texture2D<float4> texture_value,
    float2 coordinate
)
{
    uint width;
    uint height;
    texture_value.GetDimensions(width, height);
    const float2 bounded = saturate(coordinate);
    const uint2 pixel = min(
        uint2(bounded * float2(width, height)),
        uint2(width - 1u, height - 1u)
    );
    return texture_value.Load(int3(pixel, 0));
}

#define SMAA_CUSTOM_SL
#define SMAA_INCLUDE_VS 1
#define SMAA_INCLUDE_PS 1
#define SMAA_RT_METRICS smaa_rt_metrics
#define SMAATexture2D(tex) Texture2D<float4> tex
#define SMAATexturePass2D(tex) tex
#define BTBH_SMAA_SAMPLER_NAME(tex) tex##Sampler
#define SMAASampleLevelZero(tex, coord) \
    tex.SampleLevel(BTBH_SMAA_SAMPLER_NAME(tex), coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) \
    btbh_smaa_point_sample(tex, coord)
#define SMAASampleLevelZeroOffset(tex, coord, offset) \
    tex.SampleLevel(BTBH_SMAA_SAMPLER_NAME(tex), coord, 0, offset)
#define SMAASample(tex, coord) \
    tex.Sample(BTBH_SMAA_SAMPLER_NAME(tex), coord)
#define SMAASamplePoint(tex, coord) \
    btbh_smaa_point_sample(tex, coord)
#define SMAASampleOffset(tex, coord, offset) \
    tex.Sample(BTBH_SMAA_SAMPLER_NAME(tex), \
        coord + float2(offset) * SMAA_RT_METRICS.xy)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
#define SMAATexture2DMS2(tex) Texture2DMS<float4, 2> tex
#define SMAALoad(tex, pos, sample) tex.Load(pos, sample)

#include "../external/SMAA.hlsl"
