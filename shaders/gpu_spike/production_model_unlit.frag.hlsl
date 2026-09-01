Texture2D<float4> albedo_texture : register(t0, space2);
SamplerState albedo_sampler : register(s0, space2);
Texture2D<float4> emission_texture : register(t1, space2);
SamplerState emission_sampler : register(s1, space2);

cbuffer MaterialPBR : register(b0, space3)
{
    float4 unused_material_prefix[11];
    float4 emission_color_energy;
    uint unused_unlit_enabled;
    float alpha_cutoff;
    uint unused_surface_variant;
    float unused_surface_time;
};

struct FragmentInput
{
    float4 color : TEXCOORD0;
    float2 texture_coordinate : TEXCOORD1;
};

struct FragmentOutput
{
    float4 color : SV_Target0;
    float4 ambient : SV_Target1;
    float4 normal : SV_Target2;
    float4 ambient_specular : SV_Target3;
    float4 emission : SV_Target4;
};

FragmentOutput main(FragmentInput input)
{
    const float4 raw_albedo = albedo_texture.Sample(
        albedo_sampler,
        input.texture_coordinate
    );
    const float4 albedo = float4(
        raw_albedo.rgb * input.color.rgb,
        raw_albedo.a * input.color.a
    );
    if (albedo.a < alpha_cutoff)
    {
        discard;
    }
    const float3 emission = emission_texture.Sample(
        emission_sampler,
        input.texture_coordinate
    ).rgb * emission_color_energy.rgb * emission_color_energy.w;
    FragmentOutput output;
    output.color = float4(albedo.rgb + emission, albedo.a);
    output.ambient = 0.0f;
    output.normal = 0.0f;
    output.ambient_specular = 0.0f;
    output.emission = float4(emission, 1.0f);
    return output;
}
