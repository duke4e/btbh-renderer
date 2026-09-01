Texture2D<float4> acceleration_texture : register(t0, space0);
SamplerState acceleration_sampler : register(s0, space0);
Texture2D<float4> next_acceleration_texture : register(t1, space0);
SamplerState next_acceleration_sampler : register(s1, space0);
Texture2D<float4> next_radiance_texture : register(t2, space0);
SamplerState next_radiance_sampler : register(s2, space0);
RWTexture2D<float4> radiance_output : register(u0, space1);

cbuffer HRCMergeDownUniforms : register(b0, space2)
{
    uint2 radiance_atlas_resolution;
    uint grid_width;
    uint grid_height;
    uint direction_count;
    uint next_grid_width;
    uint next_direction_count;
    uint segment_height;
    uint next_radiance_valid;
    uint3 padding;
};

static const float two_pi = 6.28318530718f;

float angle_size(uint directions, uint direction)
{
    const float half_directions = float(directions) * 0.5f;
    const float center = float(direction) - half_directions + 0.5f;
    const float upper = atan2(center + 0.5f, half_directions);
    const float lower = atan2(center - 0.5f, half_directions);
    return upper - lower;
}

float4 load_acceleration(
    Texture2D<float4> texture_value,
    SamplerState sampler_value,
    int2 cell,
    uint direction,
    uint width,
    uint directions,
    uint segment
)
{
    const int segment_begin = int(segment * segment_height);
    const int segment_end = segment_begin + int(segment_height);
    if (cell.x < 0 || cell.x >= int(width) ||
        cell.y < segment_begin || cell.y >= segment_end ||
        direction > directions)
    {
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }
    const int2 atlas_coordinate = int2(
        cell.x * int(directions + 1u) + int(direction),
        cell.y
    );
    const uint2 atlas_size = uint2(
        width * (directions + 1u),
        grid_height
    );
    return texture_value.SampleLevel(
        sampler_value,
        (float2(atlas_coordinate) + 0.5f) / float2(atlas_size),
        0.0f
    );
}

float3 load_next_radiance(int2 cell, uint direction, uint segment)
{
    const int segment_begin = int(segment * segment_height);
    const int segment_end = segment_begin + int(segment_height);
    if (next_radiance_valid == 0u ||
        cell.x < 0 || cell.x >= int(next_grid_width) ||
        cell.y < segment_begin || cell.y >= segment_end ||
        direction >= next_direction_count)
    {
        return 0.0f;
    }
    const int2 atlas_coordinate = int2(
        cell.x * int(next_direction_count) + int(direction),
        cell.y
    );
    return next_radiance_texture.SampleLevel(
        next_radiance_sampler,
        (float2(atlas_coordinate) + 0.5f) /
            float2(radiance_atlas_resolution),
        0.0f
    ).rgb;
}

float3 over_radiance(float4 near_value, float3 far_value)
{
    return near_value.rgb + near_value.a * far_value;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID)
{
    if (any(dispatch_id.xy >= radiance_atlas_resolution))
    {
        return;
    }
    const uint2 cell = uint2(
        dispatch_id.x / direction_count,
        dispatch_id.y
    );
    const uint direction = dispatch_id.x % direction_count;
    if (cell.x >= grid_width || cell.y >= grid_height)
    {
        radiance_output[dispatch_id.xy] = 0.0f;
        return;
    }
    const uint segment = cell.y / segment_height;
    const bool even_cell = (cell.x & 1u) == 0u;
    const int2 current_cell = int2(cell);
    const int2 next_cell = int2(int(cell.x / 2u), int(cell.y));
    const uint lower_direction = direction * 2u;
    const uint upper_direction = lower_direction + 1u;
    const float lower_measure =
        angle_size(next_direction_count, lower_direction) / two_pi;
    const float upper_measure =
        angle_size(next_direction_count, upper_direction) / two_pi;

    float4 lower;
    float4 upper;
    if (even_cell)
    {
        lower = load_acceleration(
            next_acceleration_texture,
            next_acceleration_sampler,
            next_cell,
            lower_direction,
            next_grid_width,
            next_direction_count,
            segment
        );
        upper = load_acceleration(
            next_acceleration_texture,
            next_acceleration_sampler,
            next_cell,
            (direction + 1u) * 2u,
            next_grid_width,
            next_direction_count,
            segment
        );
    }
    else
    {
        lower = load_acceleration(
            acceleration_texture,
            acceleration_sampler,
            current_cell,
            direction,
            grid_width,
            direction_count,
            segment
        );
        upper = load_acceleration(
            acceleration_texture,
            acceleration_sampler,
            current_cell,
            direction + 1u,
            grid_width,
            direction_count,
            segment
        );
    }
    lower.rgb *= lower_measure;
    upper.rgb *= upper_measure;

    const int lower_offset = int(direction) - int(direction_count / 2u);
    const int upper_offset = lower_offset + 1;
    const int factor = even_cell ? 2 : 1;
    const int2 lower_target_current =
        current_cell + int2(1, lower_offset) * factor;
    const int2 upper_target_current =
        current_cell + int2(1, upper_offset) * factor;
    const int2 lower_target = int2(lower_target_current.x / 2,
        lower_target_current.y);
    const int2 upper_target = int2(upper_target_current.x / 2,
        upper_target_current.y);
    const float3 next_lower = over_radiance(
        lower,
        load_next_radiance(lower_target, lower_direction, segment)
    );
    const float3 next_upper = over_radiance(
        upper,
        load_next_radiance(upper_target, upper_direction, segment)
    );

    float3 result = next_lower + next_upper;
    if (even_cell)
    {
        result += load_next_radiance(next_cell, lower_direction, segment);
        result += load_next_radiance(next_cell, upper_direction, segment);
        result *= 0.5f;
    }
    radiance_output[dispatch_id.xy] = float4(result, 1.0f);
}
