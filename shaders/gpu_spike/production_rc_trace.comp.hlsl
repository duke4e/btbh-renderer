Texture2D<float4> source_field_texture : register(t0, space0);
SamplerState source_field_sampler : register(s0, space0);
Texture2D<float2> occlusion_texture : register(t1, space0);
SamplerState occlusion_sampler : register(s1, space0);
Texture2D<float4> directional_source_texture : register(t2, space0);
SamplerState directional_source_sampler : register(s2, space0);
RWTexture2D<float4> interval_output : register(u0, space1);

cbuffer RadianceTraceUniforms : register(b0, space2)
{
    uint2 atlas_resolution;
    uint2 probe_count;
    uint2 direction_tile;
    uint direction_count;
    uint probe_spacing_cells;
    float interval_start_cells;
    float interval_end_cells;
    float2 padding0;
    float4 origin_cell;
    uint2 field_dimensions;
    uint max_step_count;
    uint padding1;
    float4 environment_radiance;
    float4 directional_direction_energy;
    float4 directional_color_lobe;
    float4 secondary_directional_direction_energy;
    float4 secondary_directional_color_lobe;
    float4 sky_top_energy;
    float4 sky_horizon_curve;
};

static const float two_pi = 6.28318530718f;
static const uint2 directional_source_tile = uint2(8u, 8u);
static const uint directional_source_count = 64u;

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
    const float wrapped = frac(angle / two_pi);
    const float centered = wrapped * float(directional_source_count) - 0.5f;
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
    const float lobe = pow(cosine, color_lobe.w) / 1.17809724510f;
    return color_lobe.rgb * direction_energy.w * lobe;
}

float3 evaluate_boundary_radiance(float2 ray_direction)
{
    float3 radiance = environment_radiance.rgb;
    if (sky_top_energy.w > 0.0f)
    {
        const float elevation_y = 0.65f / sqrt(1.0f + 0.65f * 0.65f);
        const float vertical_angle = acos(saturate(elevation_y));
        const float elevation =
            1.0f - vertical_angle / (3.14159265359f * 0.5f);
        const float gradient = saturate(
            1.0f - pow(
                1.0f - elevation,
                1.0f / max(sky_horizon_curve.w, 0.001f)
            )
        );
        radiance += lerp(
            sky_horizon_curve.rgb,
            sky_top_energy.rgb,
            gradient
        ) * sky_top_energy.w;
    }
    radiance += directional_boundary_radiance(
        ray_direction,
        directional_direction_energy,
        directional_color_lobe
    );
    radiance += directional_boundary_radiance(
        ray_direction,
        secondary_directional_direction_energy,
        secondary_directional_color_lobe
    );
    return radiance;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID)
{
    if (any(dispatch_id.xy >= atlas_resolution))
    {
        return;
    }
    const uint2 probe = uint2(
        dispatch_id.x / direction_tile.x,
        dispatch_id.y / direction_tile.y
    );
    const uint2 direction_coordinate = uint2(
        dispatch_id.x % direction_tile.x,
        dispatch_id.y % direction_tile.y
    );
    const uint direction_index =
        direction_coordinate.y * direction_tile.x +
        direction_coordinate.x;
    if (any(probe >= probe_count) || direction_index >= direction_count)
    {
        interval_output[dispatch_id.xy] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

    const float2 probe_cell = min(
        (float2(probe) + 0.5f) * float(probe_spacing_cells),
        float2(field_dimensions) - 0.5f
    );
    const float2 ray_origin =
        origin_cell.xy + probe_cell * origin_cell.z;
    const float2 probe_uv =
        (probe_cell + 0.5f) / float2(field_dimensions);
    const float2 probe_occlusion = occlusion_texture.SampleLevel(
        occlusion_sampler,
        probe_uv,
        0.0f
    );
    const float receiver_surface_height = probe_occlusion.x <= 0.0f
        ? probe_occlusion.y
        : 0.0f;
    const float angle = two_pi *
        (float(direction_index) + 0.5f) / float(direction_count);
    const float2 ray_direction = float2(cos(angle), sin(angle));

    float distance_cells = interval_start_cells;
    float3 radiance = 0.0f;
    float transmittance = 1.0f;
    [loop]
    for (uint step_index = 0u;
         step_index < max_step_count &&
         distance_cells <= interval_end_cells;
         ++step_index)
    {
        const float2 world_position =
            ray_origin +
            ray_direction * (distance_cells * origin_cell.z);
        const float2 cell =
            (world_position - origin_cell.xy) * origin_cell.w;
        if (any(cell < 0.0f) || any(cell >= float2(field_dimensions)))
        {
            break;
        }
        const float2 uv = (cell + 0.5f) / float2(field_dimensions);
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
        const float ray_height = receiver_surface_height +
            distance_cells * origin_cell.z * 0.65f;
        if (field.x <= origin_cell.z * 0.55f &&
            field.y > ray_height + origin_cell.z * 0.25f)
        {
            transmittance = 0.0f;
            break;
        }
        const float sdf_cells = abs(field.x) * origin_cell.w;
        distance_cells += clamp(sdf_cells * 0.65f, 0.35f, 4.0f);
    }
    if (environment_radiance.w > 0.5f && transmittance > 0.0f)
    {
        radiance += transmittance * evaluate_boundary_radiance(ray_direction);
    }
    interval_output[dispatch_id.xy] = float4(
        radiance,
        transmittance
    );
}
