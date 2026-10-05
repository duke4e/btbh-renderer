// Soft particles: each pixel's distance from the camera to the opaque scene, measured along the
// view ray the way a transparent billboard measures its own, so the billboard can fade where it
// meets the scene behind it. Runs between the opaque and transparent geometry passes.
Texture2D<float> depth_texture : register(t0, space2);
SamplerState depth_sampler : register(s0, space2);

cbuffer SceneDistanceUniforms : register(b0, space3)
{
    float4x4 inverse_view_projection;
    // xyz = camera position, w = unused
    float4 camera_position;
};

struct FragmentInput
{
    float4 position : SV_Position;
    float2 texture_coordinate : TEXCOORD0;
};

float main(FragmentInput input) : SV_Target0
{
    const float depth = depth_texture.Load(int3(input.position.xy, 0));
    // The cleared background has no surface: nothing behind a billboard to fade against.
    if (depth >= 0.99999f)
    {
        return 1.0e6f;
    }
    const float2 uv = input.texture_coordinate;
    const float4 world = mul(
        inverse_view_projection,
        float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, depth, 1.0f)
    );
    return length(world.xyz / world.w - camera_position.xyz);
}
