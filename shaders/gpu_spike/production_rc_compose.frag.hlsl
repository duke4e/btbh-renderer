Texture2D<float4> scene_texture : register(t0, space2);
SamplerState scene_sampler : register(s0, space2);
Texture2D<float4> indirect_texture : register(t1, space2);
SamplerState indirect_sampler : register(s1, space2);
Texture2D<float4> cascade_0_texture : register(t2, space2);
SamplerState cascade_0_sampler : register(s2, space2);
Texture2D<float4> cascade_1_texture : register(t3, space2);
SamplerState cascade_1_sampler : register(s3, space2);
Texture2D<float4> cascade_2_texture : register(t4, space2);
SamplerState cascade_2_sampler : register(s4, space2);
Texture2D<float4> cascade_3_texture : register(t5, space2);
SamplerState cascade_3_sampler : register(s5, space2);
Texture2D<float> depth_texture : register(t6, space2);
SamplerState depth_sampler : register(s6, space2);
Texture2D<float4> directional_source_texture : register(t7, space2);
SamplerState directional_source_sampler : register(s7, space2);

cbuffer RadianceCascadesComposeUniforms : register(b0, space3)
{
    uint debug_view;
    uint fallback_reason;
    float indirect_strength;
    float padding;
};

struct FragmentInput
{
    float4 position : SV_Position;
    float2 texture_coordinate : TEXCOORD0;
};

float3 display_hdr(float3 value)
{
    return value / (1.0f + value);
}

float4 main(FragmentInput input) : SV_Target0
{
    const float2 uv = input.texture_coordinate;
    const float4 indirect = indirect_texture.Sample(
        indirect_sampler,
        uv
    );
    if (debug_view >= 1u && debug_view <= 4u)
    {
        float4 cascade = cascade_0_texture.Sample(
            cascade_0_sampler,
            uv
        );
        if (debug_view == 2u)
        {
            cascade = cascade_1_texture.Sample(cascade_1_sampler, uv);
        }
        else if (debug_view == 3u)
        {
            cascade = cascade_2_texture.Sample(cascade_2_sampler, uv);
        }
        else if (debug_view == 4u)
        {
            cascade = cascade_3_texture.Sample(cascade_3_sampler, uv);
        }
        return float4(display_hdr(cascade.rgb), 1.0f);
    }
    if (debug_view == 5u)
    {
        const float distance = cascade_0_texture.Sample(
            cascade_0_sampler,
            uv
        ).a;
        return float4(distance.xxx, 1.0f);
    }
    if (debug_view == 6u)
    {
        return float4(indirect.aaa, 1.0f);
    }
    if (debug_view == 7u)
    {
        return float4(display_hdr(indirect.rgb), 1.0f);
    }
    if (debug_view == 8u)
    {
        const float3 state_color = fallback_reason == 0u
            ? float3(0.05f, 0.8f, 0.15f)
            : float3(0.85f, 0.1f, 0.05f);
        return float4(state_color, 1.0f);
    }
    if (debug_view == 9u)
    {
        const float3 directional_source = directional_source_texture.Sample(
            directional_source_sampler,
            uv
        ).rgb;
        return float4(display_hdr(directional_source), 1.0f);
    }
    if (debug_view == 10u)
    {
        return float4(display_hdr(indirect.rgb), 1.0f);
    }
    const float depth = depth_texture.Sample(depth_sampler, uv);
    if (depth >= 0.99999f)
    {
        return float4(scene_texture.Sample(scene_sampler, uv).rgb, 1.0f);
    }
    if (indirect.a < -0.5f)
    {
        return float4(scene_texture.Sample(scene_sampler, uv).rgb, 1.0f);
    }
    const float3 scene = scene_texture.Sample(scene_sampler, uv).rgb;
    if (indirect.a > 1.5f)  // an unlit receiver: the bounce light added
    {
        return float4(scene + indirect.rgb, 1.0f);
    }
    return float4(max(scene, indirect.rgb), 1.0f);
}
