Texture2D<float4> indirect_texture : register(t0, space0);
SamplerState indirect_sampler : register(s0, space0);
Texture2D<float> depth_texture : register(t1, space0);
SamplerState depth_sampler : register(s1, space0);
Texture2D<float4> normal_texture : register(t2, space0);
SamplerState normal_sampler : register(s2, space0);
RWTexture2D<float4> reconstructed_output : register(u0, space1);

cbuffer HRCReconstructUniforms : register(b0, space2)
{
    uint2 source_resolution;
    uint2 output_resolution;
    float4x4 inverse_view_projection;
    float plane_distance_sigma;
    float normal_power;
    float visibility_sigma;
    float spatial_sigma;
};

float3 world_position(float2 uv, float depth)
{
    const float4 clip_position = float4(
        uv.x * 2.0f - 1.0f,
        1.0f - uv.y * 2.0f,
        depth,
        1.0f
    );
    const float4 homogeneous_world = mul(
        inverse_view_projection,
        clip_position
    );
    return homogeneous_world.xyz / homogeneous_world.w;
}

float3 decoded_normal(float2 uv)
{
    const float3 encoded = normal_texture.SampleLevel(
        normal_sampler,
        uv,
        0.0f
    ).xyz;
    const float3 value = encoded * 2.0f - 1.0f;
    return dot(value, value) > 0.00001f
        ? normalize(value)
        : float3(0.0f, 1.0f, 0.0f);
}

[numthreads(8, 8, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID)
{
    if (any(dispatch_id.xy >= output_resolution))
    {
        return;
    }
    const float2 output_uv =
        (float2(dispatch_id.xy) + 0.5f) / float2(output_resolution);
    const float center_depth = depth_texture.SampleLevel(
        depth_sampler,
        output_uv,
        0.0f
    );
    if (center_depth >= 0.99999f)
    {
        reconstructed_output[dispatch_id.xy] = 0.0f;
        return;
    }

    const float4 center_value = indirect_texture.SampleLevel(
        indirect_sampler,
        output_uv,
        0.0f
    );
    const float center_confidence = saturate(center_value.a);
    const float3 center_normal = decoded_normal(output_uv);
    const float3 center_position = world_position(output_uv, center_depth);
    const float2 source_coordinate =
        output_uv * float2(source_resolution) - 0.5f;
    const int2 source_center = int2(floor(source_coordinate));

    float4 weighted_value = 0.0f;
    float total_weight = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const int2 source_texel = clamp(
                source_center + int2(x, y),
                int2(0, 0),
                int2(source_resolution) - 1
            );
            const float2 source_uv =
                (float2(source_texel) + 0.5f) /
                float2(source_resolution);
            const float source_depth = depth_texture.SampleLevel(
                depth_sampler,
                source_uv,
                0.0f
            );
            if (source_depth >= 0.99999f)
            {
                continue;
            }
            const float4 source_value = indirect_texture.SampleLevel(
                indirect_sampler,
                source_uv,
                0.0f
            );
            const float3 source_normal = decoded_normal(source_uv);
            const float3 source_position = world_position(
                source_uv,
                source_depth
            );
            const float plane_distance = abs(dot(
                source_position - center_position,
                center_normal
            ));
            const float normal_agreement = saturate(dot(
                center_normal,
                source_normal
            ));
            const float confidence_delta = abs(
                saturate(source_value.a) - center_confidence
            );
            const float2 spatial_delta =
                float2(source_texel) - source_coordinate;
            const float spatial_weight = exp2(
                -dot(spatial_delta, spatial_delta) * spatial_sigma
            );
            const float plane_weight = exp2(
                -plane_distance * plane_distance_sigma
            );
            const float normal_weight = pow(
                normal_agreement,
                normal_power
            );
            const float visibility_weight = exp2(
                -confidence_delta * visibility_sigma
            );
            const float weight = spatial_weight * plane_weight *
                normal_weight * visibility_weight;
            weighted_value += source_value * weight;
            total_weight += weight;
        }
    }
    reconstructed_output[dispatch_id.xy] = total_weight > 0.0001f
        ? weighted_value / total_weight
        : center_value;
}
