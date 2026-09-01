Texture2D<float4> radiance_texture : register(t0, space0);
SamplerState radiance_sampler : register(s0, space0);
Texture2D<float4> source_field_texture : register(t1, space0);
SamplerState source_field_sampler : register(s1, space0);
Texture2D<float2> occlusion_texture : register(t2, space0);
SamplerState occlusion_sampler : register(s2, space0);
Texture2D<float4> directional_source_texture : register(t3, space0);
SamplerState directional_source_sampler : register(s3, space0);
RWTexture2D<float4> quadrant_output : register(u0, space1);

cbuffer HRCFinishUniforms : register(b0, space2)
{
    uint2 output_atlas_resolution;
    uint2 radiance_atlas_resolution;
    uint display_size;
    uint base_grid_size;
    uint segment_height;
    uint linear_scale;
    float4 origin_cell;
    uint2 field_dimensions;
    uint max_step_count;
    uint refresh_rotation;
    float4 environment_radiance;
    float4 directional_direction_energy;
    float4 directional_color_lobe;
    float4 secondary_directional_direction_energy;
    float4 secondary_directional_color_lobe;
    float4 sky_top_energy;
    float4 sky_horizon_curve;
};

static const float pi = 3.14159265359f;
static const float two_pi = 6.28318530718f;
static const uint2 directional_source_tile = uint2(8u, 8u);
static const uint directional_source_count = 64u;

float2 rotation_vector(uint rotation)
{
    const float angle = float(rotation & 3u) * (pi * 0.5f);
    return round(float2(cos(angle), sin(angle)));
}

int2 aligned_cell(uint2 output_cell, uint rotation)
{
    const float2 x_direction = rotation_vector(rotation);
    const float2 y_direction = float2(-x_direction.y, x_direction.x);
    const float2 diagonal = x_direction + y_direction;
    const float half_size = float(display_size) * 0.5f;
    const float2 output_origin =
        half_size.xx - half_size * diagonal + diagonal * 0.5f;
    const float2 relative = float2(output_cell) + 0.5f - output_origin;
    return int2(
        round(dot(relative, x_direction)),
        round(dot(relative, y_direction))
    );
}

float2 to_display(float2 cell, uint segment)
{
    const uint rotation = segment / 2u;
    const uint parity = segment & 1u;
    const float2 x_direction = rotation_vector(rotation);
    const float2 y_direction = float2(-x_direction.y, x_direction.x);
    const float2 diagonal = x_direction + y_direction;
    const float half_size = float(display_size) * 0.5f;
    const float2 segment_origin = half_size.xx - half_size * diagonal +
        y_direction * (float(parity) + 0.499f) + x_direction * 0.501f;
    const float2 local_cell = float2(
        cell.x,
        cell.y - float(segment * segment_height)
    );
    const float2 normalized = local_cell /
        float2(float(base_grid_size), float(segment_height));
    return segment_origin +
        (x_direction * normalized.x + y_direction * normalized.y) *
            float(display_size);
}

float4 directional_source_bin(float2 field_uv, uint direction_index)
{
    const uint2 tile = uint2(
        direction_index % directional_source_tile.x,
        direction_index / directional_source_tile.x
    );
    const float2 atlas_uv =
        (float2(tile) + field_uv) / float2(directional_source_tile);
    return directional_source_texture.SampleLevel(
        directional_source_sampler,
        atlas_uv,
        0.0f
    );
}

float4 directional_source(float2 field_uv, float angle)
{
    const float centered = frac(angle / two_pi + 1.0f) *
        float(directional_source_count) - 0.5f;
    const int lower_signed = (int)floor(centered);
    const uint lower = (uint)(
        (lower_signed % int(directional_source_count)) +
        int(directional_source_count)
    ) % directional_source_count;
    const uint upper = (lower + 1u) % directional_source_count;
    return lerp(
        directional_source_bin(field_uv, lower),
        directional_source_bin(field_uv, upper),
        frac(centered)
    );
}

float3 directional_boundary_radiance(
    float2 ray_direction,
    float4 direction_energy,
    float4 color_lobe
)
{
    const float projected_length = length(direction_energy.xz);
    if (direction_energy.w <= 0.0f || projected_length <= 0.0001f)
    {
        return 0.0f;
    }
    const float2 direction_to_light =
        -direction_energy.xz / projected_length;
    const float cosine = saturate(dot(ray_direction, direction_to_light));
    return color_lobe.rgb * direction_energy.w *
        pow(cosine, color_lobe.w) / 1.17809724510f;
}

float3 evaluate_boundary_radiance(float2 ray_direction)
{
    float3 value = environment_radiance.rgb;
    value += directional_boundary_radiance(
        ray_direction,
        directional_direction_energy,
        directional_color_lobe
    );
    value += directional_boundary_radiance(
        ray_direction,
        secondary_directional_direction_energy,
        secondary_directional_color_lobe
    );
    return value;
}

float4 trace_display_segment(float2 start, float2 end)
{
    const float2 delta = end - start;
    const float segment_length = max(length(delta), 0.0001f);
    const float2 ray_direction = delta / segment_length;
    const float angle = atan2(ray_direction.y, ray_direction.x);
    const float2 start_field = start / float(linear_scale);
    const float2 start_uv = saturate(
        (start_field + 0.5f) / float2(field_dimensions)
    );
    const float2 start_occlusion = occlusion_texture.SampleLevel(
        occlusion_sampler,
        start_uv,
        0.0f
    );
    float receiver_height = 0.0f;
    if (start_occlusion.x <= 0.0f)
    {
        receiver_height = start_occlusion.y;
    }
    float distance_display = 0.0f;
    float3 radiance = 0.0f;
    float transmittance = 1.0f;
    [loop]
    for (uint step_index = 0u;
         step_index < max_step_count &&
         distance_display <= segment_length;
         ++step_index)
    {
        const float2 display_position =
            start + ray_direction * distance_display;
        if (any(display_position < 0.0f) ||
            any(display_position >= float(display_size)))
        {
            radiance += evaluate_boundary_radiance(ray_direction);
            transmittance = 0.0f;
            break;
        }
        const float2 field_cell = display_position / float(linear_scale);
        const float2 uv =
            (field_cell + 0.5f) / float2(field_dimensions);
        const float4 source = source_field_texture.SampleLevel(
            source_field_sampler,
            uv,
            0.0f
        );
        const float4 directed_source = directional_source(uv, angle);
        if (source.a > 0.001f || directed_source.a > 0.001f)
        {
            radiance = source.rgb + directed_source.rgb;
            transmittance = 0.0f;
            break;
        }
        const float2 field = occlusion_texture.SampleLevel(
            occlusion_sampler,
            uv,
            0.0f
        );
        const float source_distance =
            distance_display / float(linear_scale);
        const float ray_height = receiver_height +
            source_distance * origin_cell.z * 0.65f;
        if (field.x <= origin_cell.z * 0.55f &&
            field.y > ray_height + origin_cell.z * 0.25f)
        {
            transmittance = 0.0f;
            break;
        }
        const float sdf_source_cells = abs(field.x) * origin_cell.w;
        distance_display += clamp(
            sdf_source_cells * float(linear_scale) * 0.65f,
            0.35f * float(linear_scale),
            4.0f * float(linear_scale)
        );
    }
    if (transmittance > 0.0f &&
        (any(end < 0.0f) || any(end >= float(display_size))))
    {
        radiance += evaluate_boundary_radiance(ray_direction);
        transmittance = 0.0f;
    }
    radiance *= (pi * 0.25f) / two_pi;
    return float4(radiance, transmittance);
}

float3 load_radiance(int2 cell, uint direction, uint segment)
{
    const int segment_begin = int(segment * segment_height);
    const int segment_end = segment_begin + int(segment_height);
    if (cell.x < 0 || cell.x >= int(base_grid_size) ||
        cell.y < segment_begin || cell.y >= segment_end ||
        direction >= 2u)
    {
        return 0.0f;
    }
    const int2 atlas_coordinate = int2(
        cell.x * 2 + int(direction),
        cell.y
    );
    return radiance_texture.SampleLevel(
        radiance_sampler,
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
    const bool full_refresh = refresh_rotation == 0xffffffffu;
    const uint rotation = full_refresh
        ? dispatch_id.y / display_size
        : refresh_rotation;
    const uint2 output_cell = uint2(
        dispatch_id.x,
        full_refresh ? dispatch_id.y % display_size : dispatch_id.y
    );
    const uint2 atlas_coordinate = uint2(
        dispatch_id.x,
        rotation * display_size + output_cell.y
    );
    if (rotation >= 4u ||
        any(atlas_coordinate >= output_atlas_resolution) ||
        any(output_cell >= uint2(display_size, display_size)))
    {
        return;
    }
    int2 cell = aligned_cell(output_cell, rotation) + int2(1, 0);
    if (cell.x >= int(display_size))
    {
        quadrant_output[atlas_coordinate] = 0.0f;
        return;
    }
    const int x_parity = cell.x & 1;
    const int y_parity = cell.y & 1;
    const int mixed_parity = x_parity ^ y_parity;
    int offset = 0;
    if (mixed_parity != 0)
    {
        offset = int(base_grid_size);
        if ((cell.y & 1) == 0)
        {
            offset -= 1;
        }
    }
    const uint segment = rotation * 2u + uint(mixed_parity);
    const int2 next_cell = int2(
        cell.x / 2,
        cell.y / 2 + offset + int(rotation * 2u * base_grid_size)
    );
    const bool parity = x_parity == 0;
    const float parity_offset = float(x_parity) * 0.5f;
    const float2 cell_f = float2(next_cell) + parity_offset.xx;
    const float2 start = to_display(cell_f - float2(0.49f, 0.0f), segment);
    const float factor = 2.0f - float(x_parity);
    const int2 lower_trace_cell = int2(
        cell_f + float2(0.5f, -0.5f) * factor
    );
    const int2 upper_trace_cell = int2(
        cell_f + float2(0.5f, 0.5f) * factor
    );
    const float4 lower = trace_display_segment(
        start,
        to_display(float2(lower_trace_cell), segment)
    );
    const float4 upper = trace_display_segment(
        start,
        to_display(float2(upper_trace_cell), segment)
    );
    const int lower_y_offset = x_parity - 1;
    const int2 lower_offset = int2(1, lower_y_offset);
    const int2 upper_offset = int2(1, 1);
    const float3 next_lower = over_radiance(
        lower,
        load_radiance(next_cell + lower_offset, 0u, segment)
    );
    const float3 next_upper = over_radiance(
        upper,
        load_radiance(next_cell + upper_offset, 1u, segment)
    );
    float3 result = next_lower + next_upper;
    if (parity)
    {
        result += load_radiance(next_cell, 0u, segment);
        result += load_radiance(next_cell, 1u, segment);
        result *= 0.5f;
    }
    const float2 field_cell =
        (float2(output_cell) + 0.5f) / float(linear_scale);
    const float2 field_value = occlusion_texture.SampleLevel(
        occlusion_sampler,
        field_cell / float2(field_dimensions),
        0.0f
    );
    float opacity = 0.0f;
    if (field_value.x <= origin_cell.z * 0.55f)
    {
        opacity = 1.0f;
    }
    quadrant_output[atlas_coordinate] = float4(result, opacity);
}
