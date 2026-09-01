Texture2D<float4> albedo_texture : register(t0, space2);
SamplerState albedo_sampler : register(s0, space2);
Texture2D<float4> emission_texture : register(t1, space2);
SamplerState emission_sampler : register(s1, space2);

cbuffer MaterialPBR : register(b0, space3)
{
    float4 unused_material_prefix[11];
    float4 emission_color_energy;
    uint unlit_enabled;
    float alpha_cutoff;
    uint unused_surface_variant;
    float unused_surface_time;
};

struct FragmentInput
{
    float4 color : TEXCOORD0;
    float2 texture_coordinate : TEXCOORD1;
};

float4 main(FragmentInput input) : SV_Target0
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
    float3 radiance = emission_texture.Sample(
        emission_sampler,
        input.texture_coordinate
    ).rgb * emission_color_energy.rgb * emission_color_energy.w;
    if (unlit_enabled != 0u)
    {
        radiance += albedo.rgb;
    }
    radiance *= albedo.a;
    const float coverage = max(radiance.r, max(radiance.g, radiance.b));
    if (coverage <= 0.00001f)
    {
        discard;
    }
    return float4(radiance, saturate(coverage));
}
