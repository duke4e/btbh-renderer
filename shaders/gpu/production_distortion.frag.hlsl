// RFC 0018 (screen-space distortion, since 0.18.0): one source into the offset field (RGBA16F at half the render size,
// added up): rg its displacement in render pixels (x right, y down), b its length times its chroma, a its length.
#include "production_distortion.hlsli"

struct FragmentInput
{
    float4 position : SV_Position;
    float2 local : TEXCOORD0;
    nointerpolation float4 shape : TEXCOORD1;
    nointerpolation float4 extra : TEXCOORD2;
};

float4 main(FragmentInput input) : SV_Target0
{
    // (The plane's coordinates are smooth across the quad: their derivatives give the outward direction on screen.)
    const float2 dl_dx = ddx(input.local);
    const float2 dl_dy = ddy(input.local);
    return distortion_displacement(input.shape, input.extra, input.local, dl_dx, dl_dy);
}
