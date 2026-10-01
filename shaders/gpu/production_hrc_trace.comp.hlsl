Texture2D<float4> source_field_texture : register(t0, space0);
SamplerState source_field_sampler : register(s0, space0);
Texture2D<float2> occlusion_texture : register(t1, space0);
SamplerState occlusion_sampler : register(s1, space0);
Texture2D<float4> directional_source_texture : register(t2, space0);
SamplerState directional_source_sampler : register(s2, space0);
RWTexture2D<float4> acceleration_output : register(u0, space1);

cbuffer HRCTraceUniforms : register(b0, space2)
{
    uint2 output_atlas_resolution;
    uint2 grid_dimensions;
    uint direction_count;
    uint segment_height;
    uint display_size;
    uint linear_scale;
    float4 origin_cell;
    uint2 field_dimensions;
    uint max_step_count;
    uint padding;
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
    if (rotation == 0u) return float2(1.0f, 0.0f);
    if (rotation == 1u) return float2(0.0f, 1.0f);
    if (rotation == 2u) return float2(-1.0f, 0.0f);
    return float2(0.0f, -1.0f);
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
        float2(float(grid_dimensions.x), float(segment_height));
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
    if (sky_top_energy.w > 0.0f)
    {
        const float elevation_y = 0.65f / sqrt(1.0f + 0.65f * 0.65f);
        const float vertical_angle = acos(saturate(elevation_y));
        const float elevation = 1.0f - vertical_angle / (pi * 0.5f);
        const float gradient = saturate(1.0f - pow(
            1.0f - elevation,
            1.0f / max(sky_horizon_curve.w, 0.001f)
        ));
        value += lerp(sky_horizon_curve.rgb, sky_top_energy.rgb, gradient) *
            sky_top_energy.w;
    }
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
    const float receiver_height = start_occlusion.x <= 0.0f
        ? start_occlusion.y
        : 0.0f;

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
    return float4(radiance, transmittance);
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
    if (cell.x >= grid_dimensions.x || cell.y >= grid_dimensions.y)
    {
        acceleration_output[dispatch_id.xy] =
            float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }
    const uint segment = cell.y / segment_height;
    const int ray_offset = int(direction) - int(direction_count / 2u);
    const int2 end_cell = int2(cell) + int2(1, ray_offset);
    const float2 start = to_display(float2(cell), segment);
    const float2 end = to_display(float2(end_cell), segment);
    acceleration_output[dispatch_id.xy] =
        trace_display_segment(start, end);
}
