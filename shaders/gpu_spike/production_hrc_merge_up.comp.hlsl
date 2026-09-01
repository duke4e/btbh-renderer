Texture2D<float4> previous_acceleration_texture : register(t0, space0);
SamplerState previous_acceleration_sampler : register(s0, space0);
RWTexture2D<float4> acceleration_output : register(u0, space1);

cbuffer HRCMergeUpUniforms : register(b0, space2)
{
    uint2 output_atlas_resolution;
    uint2 previous_atlas_resolution;
    uint grid_width;
    uint grid_height;
    uint direction_count;
    uint previous_direction_count;
    uint segment_height;
    uint padding0;
    uint2 padding1;
};

float4 over_fluence(float4 near_value, float4 far_value)
{
    return float4(
        near_value.rgb + near_value.a * far_value.rgb,
        near_value.a * far_value.a
    );
}

float4 load_previous(int2 cell, uint direction, uint segment)
{
    const int segment_begin = int(segment * segment_height);
    const int segment_end = segment_begin + int(segment_height);
    if (cell.x < 0 || cell.x >= int(grid_width * 2u) ||
        cell.y < segment_begin || cell.y >= segment_end ||
        direction > previous_direction_count)
    {
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }
    const int2 atlas_cell = int2(
        cell.x * int(previous_direction_count + 1u) + int(direction),
        cell.y
    );
    return previous_acceleration_texture.SampleLevel(
        previous_acceleration_sampler,
        (float2(atlas_cell) + 0.5f) / float2(previous_atlas_resolution),
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
    const uint stride = direction_count + 1u;
    const uint2 cell = uint2(dispatch_id.x / stride, dispatch_id.y);
    const uint direction = dispatch_id.x % stride;
    if (cell.x >= grid_width || cell.y >= grid_height)
    {
        acceleration_output[dispatch_id.xy] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }
    const uint segment = cell.y / segment_height;
    const int2 previous_cell = int2(int(cell.x * 2u), int(cell.y));
    const int ray_offset = int(direction) - int(direction_count / 2u);
    float4 value;
    if ((direction & 1u) == 0u)
    {
        const uint previous_direction = direction / 2u;
        const int2 midpoint = previous_cell + int2(1, ray_offset / 2);
        value = over_fluence(
            load_previous(previous_cell, previous_direction, segment),
            load_previous(midpoint, previous_direction, segment)
        );
    }
    else
    {
        const uint lower_direction = direction / 2u;
        const uint upper_direction = lower_direction + 1u;
        const int lower_offset = (int)floor(float(ray_offset) * 0.5f);
        const int upper_offset = (int)ceil(float(ray_offset) * 0.5f);
        const float4 lower = over_fluence(
            load_previous(previous_cell, lower_direction, segment),
            load_previous(
                previous_cell + int2(1, lower_offset),
                upper_direction,
                segment
            )
        );
        const float4 upper = over_fluence(
            load_previous(previous_cell, upper_direction, segment),
            load_previous(
                previous_cell + int2(1, upper_offset),
                lower_direction,
                segment
            )
        );
        value = (lower + upper) * 0.5f;
    }
    acceleration_output[dispatch_id.xy] = value;
}
