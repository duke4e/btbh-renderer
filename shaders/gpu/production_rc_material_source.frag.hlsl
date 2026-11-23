Texture2D<float4> albedo_texture : register(t0, space2);
SamplerState albedo_sampler : register(s0, space2);
Texture2D<float4> emission_texture : register(t1, space2);
SamplerState emission_sampler : register(s1, space2);
// RFC 0007: an SDF terrain's dig mask (production_dig_mask.hlsli).
Texture2D<float> dig_mask_texture : register(t2, space2);
SamplerState dig_mask_sampler : register(s2, space2);

#include "production_dig_mask.hlsli"
#include "production_light_factor.hlsli"

cbuffer MaterialPBR : register(b0, space3)
{
    float4 unused_material_prefix[11];
    float4 emission_color_energy;
    uint unlit_enabled;
    float alpha_cutoff;
    uint unused_surface_variant;
    float unused_surface_time;
};

cbuffer DigMask : register(b1, space3)
{
    float4 dig_mask_origin;
    float4 dig_mask_scale;
};

// The locations are production_model.vert's outputs' (and production_animated_model.vert's).
struct FragmentInput
{
    [[vk::location(0)]] float4 color : TEXCOORD0;
    [[vk::location(1)]] float2 texture_coordinate : TEXCOORD1;
    [[vk::location(2)]] float3 world_position : TEXCOORD2;
    // BTBH_GPU_SCENE_BATCH_INSTANCE_LIGHT_FACTOR (since 0.11.0): custom x, the instance's light, scales its emission
    // here as on screen (an unlit batch's colour stays a source whole).
    [[vk::location(5)]] float4 instance_custom : TEXCOORD5;
};

struct FragmentOutput
{
    float4 source : SV_Target0;
    // RFC 0010: the glowing fragment's world height, blended with MAX into the R16F tops texture: the highest in
    // each texel is that texel's top. A discarded fragment writes nothing.
    float top : SV_Target1;
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
    if (albedo.a < alpha_cutoff ||
        btbh_dig_mask_hidden(dig_mask_texture, dig_mask_sampler, dig_mask_origin, dig_mask_scale, input.world_position))
    {
        discard;
    }
    float3 radiance = emission_texture.Sample(
        emission_sampler,
        input.texture_coordinate
    ).rgb * emission_color_energy.rgb *
        (emission_color_energy.w * btbh_instance_light_factor(unlit_enabled, input.instance_custom));
    // Unlit surfaces are sources (their colour), unless they receive (bit 4, rc_receiver).
    if ((unlit_enabled & 3u) != 0u && (unlit_enabled & 4u) == 0u)
    {
        radiance += albedo.rgb;
    }
    radiance *= albedo.a;
    const float coverage = max(radiance.r, max(radiance.g, radiance.b));
    if (coverage <= 0.00001f)
    {
        discard;
    }
    FragmentOutput output;
    output.source = float4(radiance, saturate(coverage));
    output.top = input.world_position.y;
    return output;
}
