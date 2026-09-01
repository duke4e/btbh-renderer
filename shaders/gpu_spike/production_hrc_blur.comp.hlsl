Texture2D<float4> quadrant_texture : register(t0, space0);
SamplerState quadrant_sampler : register(s0, space0);
RWTexture2D<float4> quadrant_output : register(u0, space1);

cbuffer HRCBlurUniforms : register(b0, space2)
{
    uint2 atlas_resolution;
    uint layer_height;
    uint padding;
};

[numthreads(8, 8, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID)
{
    if (any(dispatch_id.xy >= atlas_resolution))
    {
        return;
    }
    const int2 center_coordinate = int2(dispatch_id.xy);
    const float4 center = quadrant_texture.SampleLevel(
        quadrant_sampler,
        (float2(center_coordinate) + 0.5f) / float2(atlas_resolution),
        0.0f
    );
    const uint layer = dispatch_id.y / layer_height;
    static const int2 offsets[4] = {
        int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1)
    };
    float4 sum = center * 4.0f;
    float weight = 4.0f;
    [unroll]
    for (uint index = 0u; index < 4u; ++index)
    {
        const int2 coordinate = center_coordinate + offsets[index];
        if (coordinate.x < 0 || coordinate.y < 0 ||
            coordinate.x >= int(atlas_resolution.x) ||
            coordinate.y >= int(atlas_resolution.y) ||
            uint(coordinate.y) / layer_height != layer)
        {
            continue;
        }
        const float4 sample_value = quadrant_texture.SampleLevel(
            quadrant_sampler,
            (float2(coordinate) + 0.5f) / float2(atlas_resolution),
            0.0f
        );
        if (abs(sample_value.a - center.a) > 0.25f)
        {
            continue;
        }
        sum += sample_value;
        weight += 1.0f;
    }
    quadrant_output[dispatch_id.xy] = sum / weight;
}
