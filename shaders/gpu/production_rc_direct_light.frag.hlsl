Texture2D<float2> occlusion_texture : register(t0, space2);
SamplerState occlusion_sampler : register(s0, space2);

struct FragmentInput
{
    float4 position : SV_Position;
    float2 field_uv : TEXCOORD0;
    nointerpolation float2 source_uv : TEXCOORD1;
    float3 radiance : TEXCOORD2;
    nointerpolation float2 forward : TEXCOORD3;
    nointerpolation float4 cone_parameters : TEXCOORD4;
    nointerpolation float3 field_size_cell : TEXCOORD5;
    nointerpolation uint is_spot : TEXCOORD6;
};

float source_visibility(
    float2 receiver_uv,
    float2 source_uv,
    float2 field_size,
    float cell_size,
    float source_height
)
{
    const float2 segment_cells =
        (source_uv - receiver_uv) * field_size;
    const float segment_length = length(segment_cells);
    if (segment_length <= 1.5f)
    {
        return 1.0f;
    }
    const float2 direction_uv =
        (source_uv - receiver_uv) / segment_length;
    float distance_cells = 1.25f;
    float visibility = 1.0f;
    [loop]
    for (uint step_index = 0u;
         step_index < 64u && distance_cells < segment_length - 0.75f;
         ++step_index)
    {
        const float progress = distance_cells / segment_length;
        const float2 sample_uv =
            receiver_uv + direction_uv * distance_cells;
        const float2 field = occlusion_texture.SampleLevel(
            occlusion_sampler,
            sample_uv,
            0.0f
        );
        const float ray_height = lerp(
            cell_size * 0.15f,
            source_height,
            progress
        );
        if (field.x <= cell_size * 0.75f &&
            field.y > ray_height + cell_size * 0.25f)
        {
            if (field.x <= 0.0f)
            {
                return 0.0f;
            }
            const float subcell_visibility = smoothstep(
                0.0f,
                cell_size * 0.75f,
                field.x
            );
            visibility = min(visibility, subcell_visibility);
        }
        distance_cells += clamp(abs(field.x) * 0.65f, 0.5f, 4.0f);
    }
    return visibility;
}

float4 main(FragmentInput input) : SV_Target0
{
    clip(float(input.is_spot) - 0.5f);
    const float2 source_to_receiver_cells =
        (input.field_uv - input.source_uv) * input.field_size_cell.xy;
    const float distance_cells = length(source_to_receiver_cells);
    const float range_cells = max(input.cone_parameters.z, 0.0001f);
    clip(range_cells - distance_cells);
    const float2 source_to_receiver = distance_cells > 0.0001f
        ? source_to_receiver_cells / distance_cells
        : input.forward;
    const float angular = smoothstep(
        input.cone_parameters.y,
        input.cone_parameters.x,
        dot(input.forward, source_to_receiver)
    );
    const float normalized_distance = saturate(
        distance_cells / range_cells
    );
    const float radial =
        1.0f - normalized_distance * normalized_distance;
    // Start the visible footprint ahead of the physical source. This keeps
    // sub-cell source capture from reading as a symmetric disk around the
    // carrier while retaining a feathered forward cone.
    const float near_field = smoothstep(0.75f, 3.0f, distance_cells);
    const float attenuation = angular * radial * near_field;
    clip(attenuation - 0.0001f);
    const float visibility = source_visibility(
        input.field_uv,
        input.source_uv,
        input.field_size_cell.xy,
        input.field_size_cell.z,
        input.cone_parameters.w
    );
    clip(visibility - 0.001f);
    return float4(
        input.radiance * attenuation * visibility,
        attenuation * visibility
    );
}
