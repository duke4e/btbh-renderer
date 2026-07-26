Texture2D<float4> gbuffer_albedo : register(t0, space2);
Texture2D<float4> gbuffer_normal : register(t1, space2);
Texture2D<float4> gbuffer_emission : register(t2, space2);
Texture2D<float4> compute_cue : register(t3, space2);
Texture2D<float> directional_shadow : register(t4, space2);
Texture2D<float> spot_shadow : register(t5, space2);
SamplerState albedo_sampler : register(s0, space2);
SamplerState normal_sampler : register(s1, space2);
SamplerState emission_sampler : register(s2, space2);
SamplerState compute_sampler : register(s3, space2);
SamplerState directional_shadow_sampler : register(s4, space2);
SamplerState spot_shadow_sampler : register(s5, space2);

struct FragmentInput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 main(FragmentInput input) : SV_Target0
{
    const float4 albedo = gbuffer_albedo.Sample(albedo_sampler, input.uv);
    const float4 material = gbuffer_normal.Sample(normal_sampler, input.uv);
    const float4 emission = gbuffer_emission.Sample(
        emission_sampler,
        input.uv
    );
    const float4 cue = compute_cue.Sample(compute_sampler, input.uv);
    const float directional_depth = directional_shadow.Sample(
        directional_shadow_sampler,
        input.uv
    );
    const float spot_depth = spot_shadow.Sample(
        spot_shadow_sampler,
        input.uv
    );
    const float directional_visibility =
        directional_depth < 0.75f ? 0.62f : 1.0f;
    const float spot_visibility = spot_depth < 0.75f ? 0.78f : 1.0f;
    const float shadow_visibility =
        directional_visibility * spot_visibility;
    const float diffuse = 0.25f + 0.75f * material.z;
    return float4(
        albedo.rgb * diffuse * shadow_visibility +
            emission.rgb +
            cue.rgb * 0.12f,
        1.0f
    );
}
