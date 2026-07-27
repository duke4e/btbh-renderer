Texture2D<float4> albedo_texture : register(t0, space2);
SamplerState albedo_sampler : register(s0, space2);
Texture2D<float4> emission_texture : register(t1, space2);
SamplerState emission_sampler : register(s1, space2);

cbuffer MaterialEmission : register(b0, space3)
{
    float4 emission_color_energy;
};

struct FragmentInput
{
    float4 color : TEXCOORD0;
    float2 texture_coordinate : TEXCOORD1;
};

float4 main(FragmentInput input) : SV_Target0
{
    const float4 albedo = albedo_texture.Sample(
        albedo_sampler,
        input.texture_coordinate
    ) * input.color;
    const float3 emission = emission_texture.Sample(
        emission_sampler,
        input.texture_coordinate
    ).rgb * emission_color_energy.rgb * emission_color_energy.w;
    return float4(albedo.rgb + emission, albedo.a);
}
