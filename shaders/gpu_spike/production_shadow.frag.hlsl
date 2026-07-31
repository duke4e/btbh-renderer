#include "production_surface.hlsli"

struct FragmentInput
{
    float4 surface_data : TEXCOORD0;
};

void main(FragmentInput input)
{
    if (!btbh_surface_shadow_visible(input.surface_data))
    {
        discard;
    }
}
