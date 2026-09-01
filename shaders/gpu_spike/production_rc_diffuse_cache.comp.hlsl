Texture2D<float4> merged_cascade_texture : register(t0, space0);
SamplerState merged_cascade_sampler : register(s0, space0);
RWTexture2D<float4> diffuse_cache_output : register(u0, space1);

cbuffer RadianceDiffuseCacheUniforms : register(b0, space2)
{
    uint2 input_atlas_resolution;
    uint2 probe_count;
    uint2 input_direction_tile;
    uint input_direction_count;
    uint padding0;
    uint2 output_atlas_resolution;
    uint2 output_normal_tile;
    uint output_normal_count;
    uint3 padding1;
};

static const float pi = 3.14159265359f;
static const uint tilted_normal_count = 8u;

float4 input_interval(uint2 probe, uint direction_index)
{
    const uint2 direction_coordinate = uint2(
        direction_index % input_direction_tile.x,
        direction_index / input_direction_tile.x
    );
    const uint2 texel = probe * input_direction_tile +
        direction_coordinate;
    return merged_cascade_texture.SampleLevel(
        merged_cascade_sampler,
        (float2(texel) + 0.5f) / float2(input_atlas_resolution),
        0.0f
    );
}

float3 normal_for_bin(uint normal_index)
{
    if (normal_index == 0u)
    {
        return float3(0.0f, 1.0f, 0.0f);
    }
    const float angle = 2.0f * pi *
        float(normal_index - 1u) / float(tilted_normal_count);
    return normalize(float3(cos(angle), 0.25f, sin(angle)));
}

[numthreads(8, 8, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID)
{
    if (any(dispatch_id.xy >= output_atlas_resolution))
    {
        return;
    }
    const uint2 probe = dispatch_id.xy / output_normal_tile;
    const uint2 normal_coordinate = dispatch_id.xy % output_normal_tile;
    const uint normal_index =
        normal_coordinate.y * output_normal_tile.x + normal_coordinate.x;
    if (any(probe >= probe_count) || normal_index >= output_normal_count)
    {
        return;
    }

    const float3 receiver_normal = normal_for_bin(normal_index);
    const float direction_measure = 2.0f * pi /
        float(max(input_direction_count, 1u));
    float3 irradiance = 0.0f;
    float transmitted_response = 0.0f;
    float response_sum = 0.0f;
    [loop]
    for (uint direction_index = 0u;
         direction_index < input_direction_count;
         ++direction_index)
    {
        const float angle = 2.0f * pi *
            (float(direction_index) + 0.5f) /
            float(input_direction_count);
        const float2 direction = float2(cos(angle), sin(angle));
        const float3 light = normalize(float3(
            direction.x,
            0.65f,
            direction.y
        ));
        const float response = saturate(dot(receiver_normal, light));
        if (response <= 0.0f)
        {
            continue;
        }
        const float4 interval = input_interval(probe, direction_index);
        const float weighted_response = response * direction_measure;
        irradiance += interval.rgb * weighted_response;
        transmitted_response += interval.a * weighted_response;
        response_sum += weighted_response;
    }
    diffuse_cache_output[dispatch_id.xy] = float4(
        irradiance,
        saturate(transmitted_response / max(response_sum, 0.00001f))
    );
}
