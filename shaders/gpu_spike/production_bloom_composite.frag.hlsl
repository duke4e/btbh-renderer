// Bloom: the chain's result added to the scene, before tone mapping.
Texture2D<float4> scene_texture : register(t0, space2);
SamplerState scene_sampler : register(s0, space2);
Texture2D<float4> bloom_texture : register(t1, space2);
SamplerState bloom_sampler : register(s1, space2);

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

float4 main(FragmentInput input) : SV_Target0
{
    const float4 scene = scene_texture.Sample(scene_sampler, input.texture_coordinate);
    const float3 bloom = bloom_texture.Sample(bloom_sampler, input.texture_coordinate).rgb;
    return float4(scene.rgb + bloom * intensity, scene.a);
}
