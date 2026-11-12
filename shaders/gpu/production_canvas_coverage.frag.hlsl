// The canvas for a one-channel texture (a font atlas baked with BTBH_GPU_FONT_FLAG_R8_ATLAS): its red
// channel is coverage, drawn as white x coverage x the vertex colour, which is what
// production_canvas_textured.frag draws from the RGBA atlas (white, coverage in alpha).
Texture2D<float4> canvas_texture : register(t0, space2);
SamplerState canvas_sampler : register(s0, space2);

struct FragmentInput
{
    float4 color : TEXCOORD0;
    float2 texture_coordinate : TEXCOORD1;
};

float4 main(FragmentInput input) : SV_Target0
{
    const float coverage = canvas_texture.Sample(
        canvas_sampler,
        input.texture_coordinate
    ).r;
    return float4(input.color.rgb, input.color.a * coverage);
}
