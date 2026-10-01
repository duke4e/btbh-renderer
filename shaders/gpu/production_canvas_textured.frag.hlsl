Texture2D<float4> canvas_texture : register(t0, space2);
SamplerState canvas_sampler : register(s0, space2);

struct FragmentInput
{
    float4 color : TEXCOORD0;
    float2 texture_coordinate : TEXCOORD1;
};

float4 main(FragmentInput input) : SV_Target0
{
    return canvas_texture.Sample(
        canvas_sampler,
        input.texture_coordinate
    ) * input.color;
}
