Texture2D<float4> scene_texture : register(t0, space2);
SamplerState scene_sampler : register(s0, space2);

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
    float3 ssao_padding;
    float4x4 inverse_view_projection;
    float projection_scale_y;
};

struct FragmentInput
{
    float4 position : SV_Position;
    float2 texture_coordinate : TEXCOORD0;
};

float3 scene_sample(float2 uv)
{
    return scene_texture.Sample(scene_sampler, uv).rgb;
}

float2 legacy_screen_coordinate(float2 gpu_texture_coordinate)
{
    return float2(gpu_texture_coordinate.x, 1.0f - gpu_texture_coordinate.y);
}

float2 gpu_texture_offset(float2 legacy_screen_offset)
{
    return float2(legacy_screen_offset.x, -legacy_screen_offset.y);
}

float luminance(float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

float3 tone_map(float3 color)
{
    const float white_squared = tonemap_white * tonemap_white;
    const float3 white_squared_color = white_squared * color;
    return
        (white_squared_color + color * color) /
        (white_squared_color + white_squared);
}

float3 linear_to_srgb(float3 color)
{
    return max(
        1.055f * pow(color, 0.416666667f) - 0.055f,
        0.0f
    );
}

float3 quantize_half3(float3 color)
{
    return float3(
        f16tof32(f32tof16(color.x)),
        f16tof32(f32tof16(color.y)),
        f16tof32(f32tof16(color.z))
    );
}

float3 output_color(float3 color, float2 fragment_coordinate)
{
    color = linear_to_srgb(tone_map(color));
    const float noise = frac(
        52.9829189f *
        frac(dot(fragment_coordinate, float2(0.06711056f, 0.00583715f)))
    );
    return color + (noise - 0.5f) / 255.0f;
}

float hash12(float2 value)
{
    float3 p3 = frac(float3(value.xyx) * 0.1031f);
    p3 += dot(p3, p3.yzx + 33.33f);
    return frac((p3.x + p3.y) * p3.z);
}

float3 post_tilt_shift(float2 uv)
{
    const float2 legacy_uv = legacy_screen_coordinate(uv);
    const float2 direction = normalize(focus_direction);
    const float2 normal = float2(-direction.y, direction.x);
    const float distance_to_line = abs(dot(legacy_uv - focus_point, normal));
    float amount = max(
        0.0f,
        (distance_to_line - focus_half_width) /
            max(0.00001f, 1.0f - focus_half_width)
    );
    amount = pow(saturate(amount), blur_curve);
    const float radius = amount * maximum_radius_pixels;
    if (radius < 0.5f)
    {
        return scene_sample(uv);
    }

    const float angle =
        hash12(legacy_uv / inverse_resolution) * 6.2831853f;
    const float cosine = cos(angle);
    const float sine = sin(angle);
    const float2 directions[6] = {
        float2(1.0f, 0.0f),
        float2(0.5f, 0.8660254f),
        float2(-0.5f, 0.8660254f),
        float2(-1.0f, 0.0f),
        float2(-0.5f, -0.8660254f),
        float2(0.5f, -0.8660254f)
    };

    const float3 center = scene_sample(uv);
    const float center_boost =
        1.0f +
        smoothstep(
            highlight_threshold,
            1.0f,
            luminance(center)
        ) * highlight_gain;
    float3 accumulated = center * center_boost;
    float weight_sum = center_boost;

    [unroll]
    for (int index = 0; index < 6; ++index)
    {
        const float2 base = directions[index];
        const float2 rotated = float2(
            cosine * base.x + sine * base.y,
            -sine * base.x + cosine * base.y
        );
        const float3 sample_color = scene_sample(
            uv + gpu_texture_offset(rotated) * inverse_resolution * radius
        );
        const float sample_weight =
            1.1f *
            (
                1.0f +
                smoothstep(
                    highlight_threshold,
                    1.0f,
                    luminance(sample_color)
                ) * highlight_gain
            );
        accumulated += sample_color * sample_weight;
        weight_sum += sample_weight;
    }

    [unroll]
    for (int index = 0; index < 6; ++index)
    {
        const float2 base = directions[index];
        const float2 rotated = float2(
            cosine * base.x + sine * base.y,
            -sine * base.x + cosine * base.y
        );
        const float3 sample_color = scene_sample(
            uv + gpu_texture_offset(rotated) *
                inverse_resolution * radius * 0.55f
        );
        const float sample_weight =
            0.85f *
            (
                1.0f +
                smoothstep(
                    highlight_threshold,
                    1.0f,
                    luminance(sample_color)
                ) * highlight_gain
            );
        accumulated += sample_color * sample_weight;
        weight_sum += sample_weight;
    }
    return accumulated / max(weight_sum, 0.00001f);
}

float4 main(FragmentInput input) : SV_Target0
{
    const float2 legacy_fragment_coordinate = float2(
        input.position.x,
        1.0f / inverse_resolution.y - input.position.y
    );
    return float4(
        output_color(
            quantize_half3(post_tilt_shift(input.texture_coordinate)),
            legacy_fragment_coordinate
        ),
        1.0f
    );
}
