Texture2D<float4> merged_cascade_texture : register(t0, space0);
SamplerState merged_cascade_sampler : register(s0, space0);
RWTexture2D<float4> filtered_cascade_output : register(u0, space1);

cbuffer RadiancePrefilterUniforms : register(b0, space2)
{
    uint2 input_atlas_resolution;
    uint2 probe_count;
    uint2 input_direction_tile;
    uint input_direction_count;
    uint directions_per_output;
    uint2 output_atlas_resolution;
    uint2 output_direction_tile;
    uint output_direction_count;
    uint3 padding;
};

float4 input_interval(uint2 probe, uint direction_index)
{
    const uint2 direction_coordinate = uint2(
        direction_index % input_direction_tile.x,
        direction_index / input_direction_tile.x
    );
    const uint2 texel = probe * input_direction_tile +
        direction_coordinate;
    const float2 uv =
        (float2(texel) + 0.5f) / float2(input_atlas_resolution);
    return merged_cascade_texture.SampleLevel(
        merged_cascade_sampler,
        uv,
        0.0f
    );
}

[numthreads(8, 8, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID)
{
    if (any(dispatch_id.xy >= output_atlas_resolution))
    {
        return;
    }
    const uint2 probe = dispatch_id.xy / output_direction_tile;
    const uint2 direction_coordinate =
        dispatch_id.xy % output_direction_tile;
    const uint output_direction_index =
        direction_coordinate.y * output_direction_tile.x +
        direction_coordinate.x;
    if (any(probe >= probe_count) ||
        output_direction_index >= output_direction_count)
    {
        return;
    }

    const uint first_input_direction =
        output_direction_index * directions_per_output;
    float4 interval_sum = 0.0f;
    [loop]
    for (uint child = 0u; child < directions_per_output; ++child)
    {
        const uint input_direction_index =
            min(first_input_direction + child,
                input_direction_count - 1u);
        interval_sum += input_interval(probe, input_direction_index);
    }
    filtered_cascade_output[dispatch_id.xy] =
        interval_sum / float(max(directions_per_output, 1u));
}
