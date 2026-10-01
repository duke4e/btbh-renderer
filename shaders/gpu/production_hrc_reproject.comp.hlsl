Texture2D<float4> fresh_texture : register(t0, space0);
SamplerState fresh_sampler : register(s0, space0);
Texture2D<float4> history_texture : register(t1, space0);
SamplerState history_sampler : register(s1, space0);
Texture2D<float2> occlusion_texture : register(t2, space0);
SamplerState occlusion_sampler : register(s2, space0);
RWTexture2D<float4> quadrant_output : register(u0, space1);

cbuffer HRCReprojectUniforms : register(b0, space2)
{
    uint2 atlas_resolution;
    uint layer_height;
    uint linear_scale;
    int2 history_offset_pixels;
    uint2 field_dimensions;
    float4 origin_cell;
    uint fresh_phase;
    uint3 padding;
};

float current_opacity(int2 destination_cell)
{
    const float2 field_cell =
        (float2(destination_cell) + 0.5f) / float(linear_scale);
    const float2 field_value = occlusion_texture.SampleLevel(
        occlusion_sampler,
        field_cell / float2(field_dimensions),
        0.0f
    );
    return field_value.x <= origin_cell.z * 0.55f ? 1.0f : 0.0f;
}

float4 source_value(
    int2 destination_cell,
    uint layer,
    bool fresh,
    float expected_opacity
)
{
    int2 source_cell = destination_cell;
    if (!fresh)
    {
        source_cell += history_offset_pixels;
    }
    if (source_cell.x < 0 || source_cell.y < 0 ||
        source_cell.x >= int(atlas_resolution.x) ||
        source_cell.y >= int(layer_height))
    {
        return float4(0.0f, 0.0f, 0.0f, expected_opacity);
    }
    const int2 coordinate = int2(
        source_cell.x,
        int(layer * layer_height) + source_cell.y
    );
    const float2 uv =
        (float2(coordinate) + 0.5f) / float2(atlas_resolution);
    const float4 value = fresh
        ? fresh_texture.SampleLevel(fresh_sampler, uv, 0.0f)
        : history_texture.SampleLevel(history_sampler, uv, 0.0f);
    if (!fresh && abs(value.a - expected_opacity) > 0.25f)
    {
        return float4(0.0f, 0.0f, 0.0f, expected_opacity);
    }
    return value;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID)
{
    if (any(dispatch_id.xy >= atlas_resolution))
    {
        return;
    }
    const uint layer = dispatch_id.y / layer_height;
    const bool fresh = fresh_phase == 0xffffffffu ||
        (layer & 1u) == fresh_phase;
    const int2 center_cell = int2(
        dispatch_id.x,
        dispatch_id.y % layer_height
    );
    const float expected_opacity = fresh
        ? 0.0f
        : current_opacity(center_cell);
    const float4 center = source_value(
        center_cell,
        layer,
        fresh,
        expected_opacity
    );
    static const int2 offsets[4] = {
        int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1)
    };
    float4 sum = center * 4.0f;
    float weight = 4.0f;
    [unroll]
    for (uint index = 0u; index < 4u; ++index)
    {
        const int2 neighbor_cell = center_cell + offsets[index];
        if (neighbor_cell.x < 0 || neighbor_cell.y < 0 ||
            neighbor_cell.x >= int(atlas_resolution.x) ||
            neighbor_cell.y >= int(layer_height))
        {
            continue;
        }
        const float4 neighbor = source_value(
            neighbor_cell,
            layer,
            fresh,
            center.a
        );
        if (abs(neighbor.a - center.a) > 0.25f)
        {
            continue;
        }
        sum += neighbor;
        weight += 1.0f;
    }
    quadrant_output[dispatch_id.xy] = sum / weight;
}
