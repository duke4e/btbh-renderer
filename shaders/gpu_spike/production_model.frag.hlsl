Texture2D<float4> albedo_texture : register(t0, space2);
SamplerState albedo_sampler : register(s0, space2);

struct FragmentInput
{
    float4 color : TEXCOORD0;
    float2 texture_coordinate : TEXCOORD1;
};

float4 main(FragmentInput input) : SV_Target0
{
    return albedo_texture.Sample(
        albedo_sampler,
        input.texture_coordinate
    ) * input.color;
}
