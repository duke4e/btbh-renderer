Texture2D<float> depth_texture : register(t0, space2);
SamplerState depth_sampler : register(s0, space2);
Texture2D<float4> normal_texture : register(t1, space2);
SamplerState normal_sampler : register(s1, space2);
Texture2D<float> ssao_texture : register(t2, space2);
SamplerState ssao_sampler : register(s2, space2);

cbuffer PostUniforms : register(b0, space3)
{
    float2 inverse_resolution;
    float tonemap_white;
    float cas_sharpness;
    float2 focus_point;
    float2 focus_direction;
    float focus_half_width;
    float maximum_radius_pixels;
    float blur_curve;
    float highlight_threshold;
    float highlight_gain;
    uint lut_enabled;
    uint cas_enabled;
    uint tilt_shift_enabled;
    uint ssao_enabled;
    uint tonemap_mode;
    float2 post_padding;
    float4x4 inverse_view_projection;
    float projection_scale_y;
};

struct FragmentInput
{
    float4 position : SV_Position;
    float2 texture_coordinate : TEXCOORD0;
};

float sample_depth_at(int2 pixel)
{
    const float2 uv =
        (float2(pixel) + 0.5f) * inverse_resolution;
    return depth_texture.SampleLevel(depth_sampler, uv, 0.0f);
}

float4 sample_normal_at(int2 pixel)
{
    const float2 uv =
        (float2(pixel) + 0.5f) * inverse_resolution;
    return normal_texture.SampleLevel(normal_sampler, uv, 0.0f);
}

float sample_ssao_at(int2 pixel, int2 resolution)
{
    const float2 uv =
        (float2(pixel) + 0.5f) / float2(resolution);
    return ssao_texture.SampleLevel(ssao_sampler, uv, 0.0f);
}

static const float2 legacy_denoise_disk[8] = {
    float2( 0.8376f,  0.1478f),
    float2( 0.3502f,  0.2715f),
    float2(-0.1253f,  0.8960f),
    float2(-0.3420f,  0.2562f),
    float2(-0.8573f, -0.1832f),
    float2(-0.3472f, -0.3570f),
    float2( 0.1599f, -0.9071f),
    float2( 0.3951f, -0.2541f)
};

float3 world_position(int2 pixel, float depth)
{
    const float2 uv =
        (float2(pixel) + 0.5f) * inverse_resolution;
    const float4 clip_position = float4(
        uv.x * 2.0f - 1.0f,
        1.0f - uv.y * 2.0f,
        depth,
        1.0f
    );
    const float4 homogeneous_world = mul(
        inverse_view_projection,
        clip_position
    );
    return homogeneous_world.xyz / homogeneous_world.w;
}

int2 representative_full_pixel(
    int2 half_pixel,
    int2 full_resolution,
    out float selected_depth
)
{
    const int2 offsets[4] = {
        int2(0, 0),
        int2(1, 0),
        int2(0, 1),
        int2(1, 1)
    };
    const int2 base_pixel = half_pixel * 2;
    int2 selected_pixel = clamp(
        base_pixel,
        int2(0, 0),
        full_resolution - 1
    );
    selected_depth = sample_depth_at(selected_pixel);
    const bool use_maximum =
        ((half_pixel.x + half_pixel.y) & 1) == 0;
    [unroll]
    for (int index = 1; index < 4; ++index)
    {
        const int2 candidate_pixel = clamp(
            base_pixel + offsets[index],
            int2(0, 0),
            full_resolution - 1
        );
        const float candidate_depth = sample_depth_at(candidate_pixel);
        if ((use_maximum && candidate_depth > selected_depth) ||
            (!use_maximum && candidate_depth < selected_depth))
        {
            selected_pixel = candidate_pixel;
            selected_depth = candidate_depth;
        }
    }
    return selected_pixel;
}

float main(FragmentInput input) : SV_Target0
{
    if (ssao_enabled == 0u)
    {
        return 1.0f;
    }
    const int2 full_resolution = int2(1.0f / inverse_resolution);
    uint ssao_width;
    uint ssao_height;
    ssao_texture.GetDimensions(ssao_width, ssao_height);
    const int2 half_resolution =
        int2(int(ssao_width), int(ssao_height));
    const int2 center_half_pixel = clamp(
        int2(input.position.xy),
        int2(0, 0),
        half_resolution - 1
    );
    float center_depth;
    const int2 center_pixel = representative_full_pixel(
        center_half_pixel,
        full_resolution,
        center_depth
    );
    if (center_depth >= 0.99999f)
    {
        return 1.0f;
    }
    const float3 center_position =
        world_position(center_pixel, center_depth);
    const float3 center_normal = normalize(
        sample_normal_at(center_pixel).xyz *
            2.0f - 1.0f
    );
    float result = sample_ssao_at(center_half_pixel, half_resolution);
    float weight_sum = 1.0f;
    const float blur_radius = 4.0f;
    [unroll]
    for (int index = 0; index < 8; ++index)
    {
        const float2 offset =
            legacy_denoise_disk[index] * blur_radius;
        const int2 sample_half_pixel =
            int2(input.position.xy + offset);
        if (any(sample_half_pixel < int2(0, 0)) ||
            any(sample_half_pixel >= half_resolution))
        {
            continue;
        }
        const float2 sample_uv =
            (input.position.xy + offset) /
            float2(half_resolution);
        float sample_depth;
        const int2 sample_pixel = representative_full_pixel(
            sample_half_pixel,
            full_resolution,
            sample_depth
        );
        const float3 sample_position =
            world_position(sample_pixel, sample_depth);
        const float3 sample_normal = normalize(
            sample_normal_at(sample_pixel).xyz *
                2.0f - 1.0f
        );
        const float plane_distance = dot(
            sample_position - center_position,
            center_normal
        );
        const float normal_difference =
            1.0f - dot(center_normal, sample_normal);
        const float weight = exp(
            -dot(offset, offset) /
                (blur_radius * blur_radius) -
            plane_distance * plane_distance * 100.0f -
            normal_difference * normal_difference * 20.0f
        );
        result +=
            ssao_texture.Sample(ssao_sampler, sample_uv) *
            weight;
        weight_sum += weight;
    }
    return saturate(result / weight_sum);
}
