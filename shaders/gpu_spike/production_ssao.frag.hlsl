Texture2D<float> depth_texture : register(t0, space2);
SamplerState depth_sampler : register(s0, space2);
Texture2D<float4> normal_texture : register(t1, space2);
SamplerState normal_sampler : register(s1, space2);

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

float view_depth(float2 uv, float depth)
{
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
    return abs(1.0f / homogeneous_world.w);
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
    const int2 full_resolution = int2(
        1.0f / inverse_resolution
    );
    const int2 half_resolution =
        (full_resolution + 1) / 2;
    const int2 half_pixel = clamp(
        int2(input.position.xy),
        int2(0, 0),
        half_resolution - 1
    );
    float center_depth;
    const int2 center_pixel = representative_full_pixel(
        half_pixel,
        full_resolution,
        center_depth
    );
    if (center_depth >= 0.99999f)
    {
        return 1.0f;
    }
    const float2 center_uv =
        (float2(center_pixel) + 0.5f) * inverse_resolution;
    const float3 position = world_position(
        center_pixel,
        center_depth
    );
    const float3 normal = normalize(
        sample_normal_at(center_pixel).xyz *
            2.0f - 1.0f
    );
    const float linear_depth =
        view_depth(center_uv, center_depth);
    const float world_radius = 1.0f;
    const float world_bias = 0.03f;
    const float legacy_intensity = 0.4f;
    const float radius_squared = world_radius * world_radius;
    const float raw_screen_radius =
        projection_scale_y *
        (0.5f * float(half_resolution.y)) *
        world_radius /
        max(linear_depth, 0.1f);
    const float screen_radius = min(
        raw_screen_radius,
        0.2f *
            min(
                float(half_resolution.x),
                float(half_resolution.y)
            )
    );
    const float radius_scale =
        screen_radius / max(raw_screen_radius, 0.0001f);
    const float spin =
        6.283185307f *
        frac(dot(
            float2(half_pixel),
            float2(0.75487766624669276f, 0.56984029099805327f)
        ));
    float occlusion_sum = 0.0f;
    [unroll]
    for (int index = 0; index < 16; ++index)
    {
        const float alpha = (float(index) + 0.5f) / 16.0f;
        const float angle =
            alpha * (7.0f * 6.283185307f) + spin;
        const float2 direction =
            float2(cos(angle), sin(angle));
        const int2 sample_half_pixel = clamp(
            half_pixel +
                int2(direction * screen_radius * alpha),
            int2(0, 0),
            half_resolution - 1
        );
        float sample_depth;
        const int2 sample_pixel = representative_full_pixel(
            sample_half_pixel,
            full_resolution,
            sample_depth
        );
        const float3 sample_position = world_position(
            sample_pixel,
            sample_depth
        );
        const float3 sample_vector =
            sample_position - position;
        const float vv = dot(sample_vector, sample_vector);
        const float vn = dot(sample_vector, normal);
        const float radius_falloff = max(radius_squared - vv, 0.0f);
        occlusion_sum +=
            radius_falloff *
            radius_falloff *
            radius_falloff *
            max((vn - world_bias) / (0.02f + vv), 0.0f);
    }
    const float normalization =
        radius_squared * world_radius;
    occlusion_sum /= normalization * normalization;
    float occlusion = max(
        0.0f,
        1.0f -
            occlusion_sum *
            legacy_intensity *
            (2.0f / 16.0f) *
            radius_scale
    );
    if (abs(ddx(linear_depth)) < 0.2f)
    {
        occlusion -=
            ddx(occlusion) *
            (float(half_pixel.x & 1) - 0.5f);
    }
    if (abs(ddy(linear_depth)) < 0.2f)
    {
        occlusion -=
            ddy(occlusion) *
            (float(half_pixel.y & 1) - 0.5f);
    }
    return saturate(occlusion);
}
