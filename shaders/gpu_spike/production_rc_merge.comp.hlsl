Texture2D<float4> lower_interval_texture : register(t0, space0);
SamplerState lower_interval_sampler : register(s0, space0);
Texture2D<float4> upper_merged_texture : register(t1, space0);
SamplerState upper_merged_sampler : register(s1, space0);
Texture2D<float4> source_field_texture : register(t2, space0);
SamplerState source_field_sampler : register(s2, space0);
Texture2D<float2> occlusion_texture : register(t3, space0);
SamplerState occlusion_sampler : register(s3, space0);
Texture2D<float4> directional_source_texture : register(t4, space0);
SamplerState directional_source_sampler : register(s4, space0);
RWTexture2D<float4> merged_output : register(u0, space1);

cbuffer RadianceMergeUniforms : register(b0, space2)
{
    uint2 lower_atlas_resolution;
    uint2 lower_probe_count;
    uint2 lower_direction_tile;
    uint lower_direction_count;
    uint lower_probe_spacing_cells;
    uint2 upper_atlas_resolution;
    uint2 upper_probe_count;
    uint2 upper_direction_tile;
    uint upper_direction_count;
    uint upper_probe_spacing_cells;
    uint angular_branching;
    uint bilinear_fix;
    float lower_interval_start_cells;
    float upper_interval_start_cells;
    float4 origin_cell;
    uint2 field_dimensions;
    uint max_step_count;
    uint padding;
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

float4 directional_source(float2 field_uv, float2 receiver_to_source)
{
    float angle = atan2(receiver_to_source.y, receiver_to_source.x);
    if (angle < 0.0f)
    {
        angle += two_pi;
    }
    const float centered =
        angle / two_pi * float(directional_source_count) - 0.5f;
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

float4 upper_interval(uint2 probe, uint direction_index)
{
    const uint2 direction_coordinate = uint2(
        direction_index % upper_direction_tile.x,
        direction_index / upper_direction_tile.x
    );
    const uint2 atlas_coordinate = uint2(
        probe.x * upper_direction_tile.x + direction_coordinate.x,
        probe.y * upper_direction_tile.y + direction_coordinate.y
    );
    return upper_merged_texture.SampleLevel(
        upper_merged_sampler,
        (float2(atlas_coordinate) + 0.5f) /
            float2(upper_atlas_resolution),
        0.0f
    );
}

float4 filtered_upper_interval(uint2 probe, uint lower_direction_index)
{
    const uint first_direction =
        lower_direction_index * angular_branching;
    float4 result = 0.0f;
    [loop]
    for (uint child = 0u; child < angular_branching; ++child)
    {
        result += upper_interval(
            probe,
            min(first_direction + child, upper_direction_count - 1u)
        );
    }
    return result / float(angular_branching);
}

float4 merge_intervals(float4 lower, float4 upper)
{
    return float4(
        lower.rgb + lower.a * upper.rgb,
        lower.a * upper.a
    );
}

float4 trace_reprojected_interval(
    float2 receiver_cell,
    float2 start_cell,
    float2 end_cell
)
{
    const float2 segment = end_cell - start_cell;
    const float segment_length = length(segment);
    const float2 direction = segment_length > 0.00001f
        ? segment / segment_length
        : float2(1.0f, 0.0f);
    float distance_cells = 0.0f;
    float3 radiance = 0.0f;
    float transmittance = 1.0f;
    const float2 receiver_uv =
        (receiver_cell + 0.5f) / float2(field_dimensions);
    const float2 receiver_occlusion = occlusion_texture.SampleLevel(
        occlusion_sampler,
        receiver_uv,
        0.0f
    );
    const float receiver_surface_height = receiver_occlusion.x <= 0.0f
        ? receiver_occlusion.y
        : 0.0f;
    [loop]
    for (uint step_index = 0u;
         step_index < max_step_count && distance_cells <= segment_length;
         ++step_index)
    {
        const float2 cell = start_cell + direction * distance_cells;
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
        const float4 directed_source = directional_source(uv, direction);
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
        const float receiver_distance_cells = length(cell - receiver_cell);
        const float ray_height = receiver_surface_height +
            receiver_distance_cells * origin_cell.z * 0.65f;
        if (field.x <= origin_cell.z * 0.55f &&
            field.y > ray_height + origin_cell.z * 0.25f)
        {
            transmittance = 0.0f;
            break;
        }
        const float sdf_cells = abs(field.x) * origin_cell.w;
        distance_cells += clamp(sdf_cells * 0.65f, 0.35f, 4.0f);
    }
    return float4(radiance, transmittance);
}

float4 reprojected_merge(
    uint2 upper_probe,
    float2 lower_probe_cell,
    float2 lower_direction,
    uint lower_direction_index
)
{
    const float2 upper_probe_cell = min(
        (float2(upper_probe) + 0.5f) * float(upper_probe_spacing_cells),
        float2(field_dimensions) - 0.5f
    );
    const float2 start_cell =
        lower_probe_cell + lower_direction * lower_interval_start_cells;
    const float2 end_cell =
        upper_probe_cell + lower_direction * upper_interval_start_cells;
    const float4 lower = trace_reprojected_interval(
        lower_probe_cell,
        start_cell,
        end_cell
    );
    const float4 upper = filtered_upper_interval(
        upper_probe,
        lower_direction_index
    );
    return merge_intervals(lower, upper);
}

[numthreads(8, 8, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID)
{
    if (any(dispatch_id.xy >= lower_atlas_resolution))
    {
        return;
    }
    const uint2 lower_probe = uint2(
        dispatch_id.x / lower_direction_tile.x,
        dispatch_id.y / lower_direction_tile.y
    );
    const uint2 direction_coordinate = uint2(
        dispatch_id.x % lower_direction_tile.x,
        dispatch_id.y % lower_direction_tile.y
    );
    const uint lower_direction_index =
        direction_coordinate.y * lower_direction_tile.x +
        direction_coordinate.x;
    if (any(lower_probe >= lower_probe_count) ||
        lower_direction_index >= lower_direction_count)
    {
        merged_output[dispatch_id.xy] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

    const float2 lower_probe_cell = min(
        (float2(lower_probe) + 0.5f) * float(lower_probe_spacing_cells),
        float2(field_dimensions) - 0.5f
    );
    const float2 upper_coordinate = clamp(
        lower_probe_cell / float(upper_probe_spacing_cells) - 0.5f,
        0.0f,
        float2(upper_probe_count - 1u)
    );
    const uint2 upper_probe_00 = uint2(floor(upper_coordinate));
    const uint2 upper_probe_11 = min(
        upper_probe_00 + 1u,
        upper_probe_count - 1u
    );
    const uint2 upper_probe_10 = uint2(
        upper_probe_11.x,
        upper_probe_00.y
    );
    const uint2 upper_probe_01 = uint2(
        upper_probe_00.x,
        upper_probe_11.y
    );
    const float2 weight = frac(upper_coordinate);

    float4 merged_00;
    float4 merged_10;
    float4 merged_01;
    float4 merged_11;
    if (bilinear_fix != 0u)
    {
        const float angle = 6.28318530718f *
            (float(lower_direction_index) + 0.5f) /
            float(lower_direction_count);
        const float2 lower_direction = float2(cos(angle), sin(angle));
        merged_00 = reprojected_merge(
            upper_probe_00,
            lower_probe_cell,
            lower_direction,
            lower_direction_index
        );
        merged_10 = reprojected_merge(
            upper_probe_10,
            lower_probe_cell,
            lower_direction,
            lower_direction_index
        );
        merged_01 = reprojected_merge(
            upper_probe_01,
            lower_probe_cell,
            lower_direction,
            lower_direction_index
        );
        merged_11 = reprojected_merge(
            upper_probe_11,
            lower_probe_cell,
            lower_direction,
            lower_direction_index
        );
    }
    else
    {
        const float4 lower = lower_interval_texture.SampleLevel(
            lower_interval_sampler,
            (float2(dispatch_id.xy) + 0.5f) /
                float2(lower_atlas_resolution),
            0.0f
        );
        merged_00 = merge_intervals(
            lower,
            filtered_upper_interval(upper_probe_00, lower_direction_index)
        );
        merged_10 = merge_intervals(
            lower,
            filtered_upper_interval(upper_probe_10, lower_direction_index)
        );
        merged_01 = merge_intervals(
            lower,
            filtered_upper_interval(upper_probe_01, lower_direction_index)
        );
        merged_11 = merge_intervals(
            lower,
            filtered_upper_interval(upper_probe_11, lower_direction_index)
        );
    }
    const float4 bottom = lerp(merged_00, merged_10, weight.x);
    const float4 top = lerp(merged_01, merged_11, weight.x);
    merged_output[dispatch_id.xy] = lerp(bottom, top, weight.y);
}
