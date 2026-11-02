// The occlusion laid over the opaque scene (before the transparent surfaces are drawn), blended into it: the
// target becomes src.rgb + target * src.a. The half-resolution occlusion is taken up at each pixel from the four
// texels round it, weighted by how near their view depths are to the pixel's (a texel of another surface counts
// for nothing, so edges stay sharp). Where nothing darkens a pixel it is left as it is (discarded).
// ssao_enabled: 1 darkens the ambient light of lit surfaces (the ambient targets); 2 (with the radiance cascades)
// all of a surface's light but its emission, three quarters as much; 3 (contact) all of it but the emission, lit
// surfaces or unlit. ssao_flags bit 0: the occlusion alone, in grey (the debug view).
Texture2D<float> depth_texture : register(t0, space2);
SamplerState depth_sampler : register(s0, space2);
Texture2D<float4> ambient_texture : register(t1, space2);
SamplerState ambient_sampler : register(s1, space2);
Texture2D<float4> normal_texture : register(t2, space2);
SamplerState normal_sampler : register(s2, space2);
Texture2D<float4> ambient_specular_texture : register(t3, space2);
SamplerState ambient_specular_sampler : register(s3, space2);
Texture2D<float2> ssao_texture : register(t4, space2);
SamplerState ssao_sampler : register(s4, space2);
Texture2D<float4> emission_texture : register(t5, space2);
SamplerState emission_sampler : register(s5, space2);

#include "production_hbao.hlsli"

struct FragmentInput
{
    float4 position : SV_Position;
    float2 texture_coordinate : TEXCOORD0;
};

float4 clip_of(float2 uv, float depth)
{
    return float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, depth, 1.0f);
}

float3 world_position(float2 uv, float depth)
{
    const float4 h = mul(inverse_view_projection, clip_of(uv, depth));
    return h.xyz / h.w;
}

float specular_occlusion(float normal_dot_view, float occlusion, float roughness)
{
    return saturate(pow(normal_dot_view + occlusion, exp2(-16.0f * roughness - 1.0f)) - 1.0f + occlusion);
}

float4 main(FragmentInput input) : SV_Target0
{
    const bool debug_view = (ssao_flags & 1u) != 0u;
    uint width, height;
    depth_texture.GetDimensions(width, height);
    const int2 pixel = clamp(int2(input.position.xy), int2(0, 0), int2(width, height) - 1);
    const float2 uv = (float2(pixel) + 0.5f) / float2(width, height);
    uint low_width, low_height;
    ssao_texture.GetDimensions(low_width, low_height);
    const int2 low_size = int2(low_width, low_height);

    // The four half-resolution texels round the pixel, and how much of each its place takes (bilinear).
    const float2 low = (float2(pixel) + 0.5f) * 0.5f - 0.5f;
    const int2 base = int2(floor(low));
    const float2 f = low - float2(base);
    float2 taps[4];
    float weights[4] = {
        (1.0f - f.x) * (1.0f - f.y),
        f.x * (1.0f - f.y),
        (1.0f - f.x) * f.y,
        f.x * f.y
    };
    bool darkened = false;
    [unroll]
    for (int index = 0; index < 4; ++index)
    {
        // (Texel centres through the sampler: a texture read without one would share another's slot in Metal.)
        const int2 at = clamp(base + int2(index & 1, index >> 1), int2(0, 0), low_size - 1);
        taps[index] = ssao_texture.SampleLevel(ssao_sampler, (float2(at) + 0.5f) / float2(low_size), 0.0f);
        darkened = darkened || (taps[index].x >= 0.0f && taps[index].x < 0.998f);
    }
    if (ssao_enabled == 0u || (!darkened && !debug_view))
    {
        discard;
    }
    const float depth = depth_texture.SampleLevel(depth_sampler, uv, 0.0f);
    if (depth >= 0.99999f)
    {
        if (debug_view)
        {
            return float4(1.0f, 1.0f, 1.0f, 0.0f);
        }
        discard;
    }
    // (A surface that takes none (its normal's length, production_hbao.frag.hlsl) stays as it is, though a texel of
    // what lies beside it at its depth darkens: water by its shore.)
    const float4 encoded_normal = normal_texture.SampleLevel(normal_sampler, uv, 0.0f);
    const float3 decoded_normal = encoded_normal.xyz * 2.0f - 1.0f;
    const float normal_length_squared = dot(decoded_normal, decoded_normal);
    if ((normal_length_squared <= 0.7656f && normal_length_squared > 0.3906f) || normal_length_squared <= 0.1406f)
    {
        if (debug_view)
        {
            return float4(1.0f, 1.0f, 1.0f, 0.0f);
        }
        discard;
    }
    const float linear_depth = abs(1.0f / dot(inverse_view_projection[3], clip_of(uv, depth)));
    const float radius = ssao_radius > 0.0f ? ssao_radius : 1.0f;
    const float tolerance = 0.25f * radius + 0.006f * linear_depth;
    float visibility = 0.0f;
    float weight_sum = 0.0f;
    [unroll]
    for (int index = 0; index < 4; ++index)
    {
        const float w = taps[index].x < 0.0f
            ? 0.0f
            : weights[index] * saturate(1.0f - abs(taps[index].y - linear_depth) / tolerance) + 0.00001f;
        visibility += taps[index].x * w;
        weight_sum += w;
    }
    // (No texel of this surface near: nothing darkens it.)
    visibility = weight_sum > 0.001f ? saturate(visibility / weight_sum) : 1.0f;
    if (debug_view)
    {
        return float4(visibility, visibility, visibility, 0.0f);
    }
    if (visibility >= 0.998f)
    {
        discard;
    }
    if (ssao_enabled >= 2u)
    {
        // All the light but the emission.
        const float kept = ssao_enabled == 2u ? lerp(1.0f, visibility, 0.75f) : visibility;
        const float3 emission = emission_texture.SampleLevel(emission_sampler, uv, 0.0f).rgb;
        return float4(emission * (1.0f - kept), kept);
    }
    // The ambient light of lit surfaces (diffuse, and specular as its occlusion goes).
    const float3 normal = normalize(decoded_normal);
    const float3 view = normalize(world_position(uv, depth) - world_position(uv, 0.9999f));
    const float normal_dot_view = max(dot(normal, view), 0.0f);
    const float material_occlusion = saturate(encoded_normal.w);
    const float3 ambient_diffuse = ambient_texture.SampleLevel(ambient_sampler, uv, 0.0f).rgb;
    const float4 ambient_specular = ambient_specular_texture.SampleLevel(ambient_specular_sampler, uv, 0.0f);
    const float roughness = saturate(ambient_specular.w);
    const float original = specular_occlusion(normal_dot_view, material_occlusion, roughness);
    const float resolved = specular_occlusion(normal_dot_view, material_occlusion * visibility, roughness);
    const float ratio = original > 0.00001f ? resolved / original : 0.0f;
    return float4(-(ambient_diffuse * (1.0f - visibility) + ambient_specular.rgb * (1.0f - ratio)), 1.0f);
}
