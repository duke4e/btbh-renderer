// HDR composite before grading: distance fog toward the horizon colour, bloom and vignette.
// Runs on the post-SSAO HDR image, so every later stage (LUT, CAS, tilt-shift, output) is unchanged.
Texture2D<float4> scene_texture : register(t0, space2);
SamplerState scene_sampler : register(s0, space2);
Texture2D<float> depth_texture : register(t1, space2);
SamplerState depth_sampler : register(s1, space2);
Texture2D<float4> bloom_texture : register(t2, space2);
SamplerState bloom_sampler : register(s2, space2);

cbuffer CompositeUniforms : register(b0, space3)
{
    float4x4 inverse_view_projection;
    // rgb = linear fog colour, a = density per world unit
    float4 fog_color_density;
    // x = start distance, y = maximum opacity, z = enabled (0/1), w = unused
    float4 fog_shape;
    // x = intensity, y = enabled (0/1), z/w = unused
    float4 bloom_parameters;
    // x = intensity, y = smoothness, z = aspect (width / height), w = enabled (0/1)
    float4 vignette_parameters;
    // world XZ rectangle the fog closes over: min x, min z, max x, max z
    float4 fog_edge_bounds;
    // x = fade distance (inside the rectangle and in view depth), y = enabled (0/1),
    // z = view depth that stays clear of the edge fog, w = unused
    float4 fog_edge;
};

struct FragmentInput
{
    float4 position : SV_Position;
    float2 texture_coordinate : TEXCOORD0;
};

float4 homogeneous_world(float2 uv, float depth)
{
    const float4 clip_position = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, depth, 1.0f);
    return mul(inverse_view_projection, clip_position);
}

float fog_amount(float2 uv)
{
    const float depth = depth_texture.SampleLevel(depth_sampler, uv, 0.0f);
    // The cleared background has no surface: it is the horizon, already cleared to the fog
    // colour by the scene pass, so transparent effects drawn over it keep their colour.
    if (depth >= 0.99999f)
    {
        return 0.0f;
    }
    const float4 world = homogeneous_world(uv, depth);
    // View depth, like the SSAO passes.
    const float distance = abs(1.0f / world.w);
    const float optical = max(distance - fog_shape.x, 0.0f) * fog_color_density.a;
    float amount = min(1.0f - exp(-optical * optical), fog_shape.y);
    if (fog_edge.y > 0.5f)
    {
        // Thicken to full fog across the band inside the edge of the world, so the city does not
        // end in a hard line against the fogged background. Up to the clear depth (the play area
        // around the camera target) the edge stays crisp, so the action next to it stays readable.
        const float2 position = world.xz / world.w;
        const float inside = min(
            min(position.x - fog_edge_bounds.x, fog_edge_bounds.z - position.x),
            min(position.y - fog_edge_bounds.y, fog_edge_bounds.w - position.y)
        );
        const float edge = 1.0f - saturate(inside / fog_edge.x);
        const float depth_gate = saturate((distance - fog_edge.z) / fog_edge.x);
        amount = max(
            amount,
            edge * edge * (3.0f - 2.0f * edge) * depth_gate * depth_gate * (3.0f - 2.0f * depth_gate)
        );
    }
    return amount;
}

float4 main(FragmentInput input) : SV_Target0
{
    const float2 uv = input.texture_coordinate;
    float3 color = scene_texture.SampleLevel(scene_sampler, uv, 0.0f).rgb;

    if (fog_shape.z > 0.5f)
    {
        color = lerp(color, fog_color_density.rgb, fog_amount(uv));
    }
    if (bloom_parameters.y > 0.5f)
    {
        color += bloom_texture.SampleLevel(bloom_sampler, uv, 0.0f).rgb * bloom_parameters.x;
    }
    if (vignette_parameters.w > 0.5f)
    {
        // Round on screen: x is measured in half-heights, so the corners are at edge = 1.
        float2 offset = (uv - 0.5f) * 2.0f;
        offset.x *= vignette_parameters.z;
        const float edge = saturate(length(offset) / length(float2(vignette_parameters.z, 1.0f)));
        const float falloff = smoothstep(1.0f - vignette_parameters.y, 1.0f, edge);
        color *= 1.0f - vignette_parameters.x * falloff;
    }
    return float4(color, 1.0f);
}
