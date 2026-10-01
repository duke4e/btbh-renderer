#include "production_surface.hlsli"

struct FragmentInput
{
    float4 pixel_position : SV_Position;
    float4 surface_data : TEXCOORD0;
};

void main(FragmentInput input)
{
    if (!btbh_surface_shadow_visible(input.surface_data, input.pixel_position.xy))
    {
        discard;
    }
}
