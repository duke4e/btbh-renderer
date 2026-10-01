Texture2D<float4> source_texture : register(t0, space2);
SamplerState source_sampler : register(s0, space2);

cbuffer UpscaleUniforms : register(b0, space3)
{
    float4 source_metrics;
};

struct FragmentInput
{
    float4 position : SV_Position;
    float2 texture_coordinate : TEXCOORD0;
};

static const float PI = 3.1415926535897931f;
static const float HALF_PI = 1.5707963267948966f;

float4 lanczos(float4 value)
{
    value += 0.00001f;
    return
        sin(value * PI) * sin(value * HALF_PI) /
        (value * value);
}

float3 sample_lanczos(float2 texture_coordinate)
{
    const float2 source_size = source_metrics.zw;
    const float2 inverse_source_size = source_metrics.xy;
    const float2 dx = float2(1.0f, 0.0f);
    const float2 dy = float2(0.0f, 1.0f);
    const float2 pixel_coordinate = texture_coordinate * source_size;
    const float2 texel_center =
        floor(pixel_coordinate - 0.5f) + 0.5f;

    const float4 weights0 = lanczos(float4(
        1.0f,
        distance(pixel_coordinate, texel_center - dy),
        distance(pixel_coordinate, texel_center + dx - dy),
        1.0f
    ));
    const float4 weights1 = lanczos(float4(
        distance(pixel_coordinate, texel_center - dx),
        distance(pixel_coordinate, texel_center),
        distance(pixel_coordinate, texel_center + dx),
        distance(pixel_coordinate, texel_center + 2.0f * dx)
    ));
    const float4 weights2 = lanczos(float4(
        distance(pixel_coordinate, texel_center - dx + dy),
        distance(pixel_coordinate, texel_center + dy),
        distance(pixel_coordinate, texel_center + dx + dy),
        distance(pixel_coordinate, texel_center + 2.0f * dx + dy)
    ));
    const float4 weights3 = lanczos(float4(
        1.0f,
        distance(pixel_coordinate, texel_center + 2.0f * dy),
        distance(pixel_coordinate, texel_center + dx + 2.0f * dy),
        1.0f
    ));

    const float2 center_uv = texel_center * inverse_source_size;
    const float2 texel_dx = dx * inverse_source_size;
    const float2 texel_dy = dy * inverse_source_size;
    const float3 color10 = source_texture.SampleLevel(
        source_sampler, center_uv - texel_dy, 0.0f
    ).rgb;
    const float3 color20 = source_texture.SampleLevel(
        source_sampler, center_uv + texel_dx - texel_dy, 0.0f
    ).rgb;
    const float3 color01 = source_texture.SampleLevel(
        source_sampler, center_uv - texel_dx, 0.0f
    ).rgb;
    const float3 color11 = source_texture.SampleLevel(
        source_sampler, center_uv, 0.0f
    ).rgb;
    const float3 color21 = source_texture.SampleLevel(
        source_sampler, center_uv + texel_dx, 0.0f
    ).rgb;
    const float3 color31 = source_texture.SampleLevel(
        source_sampler, center_uv + 2.0f * texel_dx, 0.0f
    ).rgb;
    const float3 color02 = source_texture.SampleLevel(
        source_sampler, center_uv - texel_dx + texel_dy, 0.0f
    ).rgb;
    const float3 color12 = source_texture.SampleLevel(
        source_sampler, center_uv + texel_dy, 0.0f
    ).rgb;
    const float3 color22 = source_texture.SampleLevel(
        source_sampler, center_uv + texel_dx + texel_dy, 0.0f
    ).rgb;
    const float3 color32 = source_texture.SampleLevel(
        source_sampler, center_uv + 2.0f * texel_dx + texel_dy, 0.0f
    ).rgb;
    const float3 color13 = source_texture.SampleLevel(
        source_sampler, center_uv + 2.0f * texel_dy, 0.0f
    ).rgb;
    const float3 color23 = source_texture.SampleLevel(
        source_sampler,
        center_uv + texel_dx + 2.0f * texel_dy,
        0.0f
    ).rgb;

    const float3 weighted_color =
        weights0.y * color10 + weights0.z * color20 +
        weights1.x * color01 + weights1.y * color11 +
        weights1.z * color21 + weights1.w * color31 +
        weights2.x * color02 + weights2.y * color12 +
        weights2.z * color22 + weights2.w * color32 +
        weights3.y * color13 + weights3.z * color23;
    const float weight_sum =
        dot(weights0, 1.0f) + dot(weights1, 1.0f) +
        dot(weights2, 1.0f) + dot(weights3, 1.0f);
    return weighted_color / weight_sum;
}

float4 main(FragmentInput input) : SV_Target0
{
    return float4(sample_lanczos(input.texture_coordinate), 1.0f);
}
