#include "production_surface.hlsli"
#include "production_dig_mask.hlsli"

// RFC 0007: an SDF terrain's dig mask (production_dig_mask.hlsli), so dug ground casts no shadow over its crater.
Texture2D<float> dig_mask_texture : register(t0, space2);
SamplerState dig_mask_sampler : register(s0, space2);

cbuffer DigMask : register(b0, space3)
{
    float4 dig_mask_origin;
    float4 dig_mask_scale;
};

struct FragmentInput
{
    float4 pixel_position : SV_Position;
    float4 surface_data : TEXCOORD0;
    float3 world_position : TEXCOORD1;
};

void main(FragmentInput input)
{
    if (!btbh_surface_shadow_visible(input.surface_data, input.pixel_position.xy) ||
        btbh_dig_mask_hidden(dig_mask_texture, dig_mask_sampler, dig_mask_origin, dig_mask_scale, input.world_position))
    {
        discard;
    }
}
