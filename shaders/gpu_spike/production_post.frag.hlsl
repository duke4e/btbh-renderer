Texture2D<float4> scene_texture : register(t0, space2);
SamplerState scene_sampler : register(s0, space2);
Texture2D<float4> lut_texture : register(t1, space2);
SamplerState lut_sampler : register(s1, space2);

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

float3 tone_map(float3 color)
{
    const float white = max(tonemap_white, 0.0001f);
    const float white_squared = white * white;
    return color * (1.0f + color / white_squared) / (1.0f + color);
}

float3 grade_lut(float3 color)
{
    if (lut_enabled == 0u)
    {
        return color;
    }
    const float lut_size = 64.0f;
    const float tiles_per_row = 8.0f;
    const float texture_size = 512.0f;
    const float3 raw = saturate(pow(max(color, 0.0f), 1.0f / 2.2f));
    const float blue_index = raw.b * (lut_size - 1.0f);
    const float lower_index = floor(blue_index);
    const float upper_index = min(lut_size - 1.0f, lower_index + 1.0f);
    const float2 lower_tile = float2(
        fmod(lower_index, tiles_per_row),
        floor(lower_index / tiles_per_row)
    );
    const float2 upper_tile = float2(
        fmod(upper_index, tiles_per_row),
        floor(upper_index / tiles_per_row)
    );
    const float2 tile_size = 1.0f / tiles_per_row;
    const float2 texel = 0.5f / texture_size;
    const float2 sample_uv =
        texel + raw.rg * ((lut_size - 1.0f) / texture_size);
    const float3 lower = lut_texture.Sample(
        lut_sampler,
        lower_tile * tile_size + sample_uv
    ).rgb;
    const float3 upper = lut_texture.Sample(
        lut_sampler,
        upper_tile * tile_size + sample_uv
    ).rgb;
    return pow(
        max(lerp(lower, upper, frac(blue_index)), 0.0f),
        2.2f
    );
}

float3 scene_sample(float2 uv)
{
    return scene_texture.Sample(scene_sampler, uv).rgb;
}

float3 cas_sample(float2 uv)
{
    const float3 e = grade_lut(scene_sample(uv));
    if (cas_enabled == 0u)
    {
        return e;
    }
    const float3 a = grade_lut(scene_sample(
        uv + float2(0.0f, -inverse_resolution.y)
    ));
    const float3 b = grade_lut(scene_sample(
        uv + float2(-inverse_resolution.x, 0.0f)
    ));
    const float3 c = grade_lut(scene_sample(
        uv + float2(inverse_resolution.x, 0.0f)
    ));
    const float3 d = grade_lut(scene_sample(
        uv + float2(0.0f, inverse_resolution.y)
    ));
    const float minimum_green = min(
        e.g,
        min(min(a.g, b.g), min(c.g, d.g))
    );
    const float maximum_green = max(
        e.g,
        max(max(a.g, b.g), max(c.g, d.g))
    );
    const float amplitude =
        1.0f / lerp(8.0f, 5.0f, saturate(cas_sharpness));
    const float amount = sqrt(
        min(1.0f - maximum_green, minimum_green) /
        max(maximum_green, 0.0001f)
    );
    const float weight = amplitude * amount;
    return
        (e + (a + b + c + d) * weight) /
        (1.0f + 4.0f * weight);
}

float luminance(float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

float hash12(float2 value)
{
    float3 p3 = frac(float3(value.xyx) * 0.1031f);
    p3 += dot(p3, p3.yzx + 33.33f);
    return frac((p3.x + p3.y) * p3.z);
}

float3 tilt_shift(float2 uv)
{
    if (tilt_shift_enabled == 0u)
    {
        return cas_sample(uv);
    }
    const float2 direction = normalize(
        dot(focus_direction, focus_direction) > 0.000001f
            ? focus_direction
            : float2(1.0f, 0.0f)
    );
    const float2 normal = float2(-direction.y, direction.x);
    const float distance_to_line = abs(dot(uv - focus_point, normal));
    float amount = max(
        0.0f,
        (distance_to_line - focus_half_width) /
            max(0.00001f, 1.0f - focus_half_width)
    );
    amount = pow(saturate(amount), max(blur_curve, 0.0001f));
    const float radius = amount * maximum_radius_pixels;
    if (radius < 0.5f)
    {
        return cas_sample(uv);
    }
    const float angle =
        hash12(uv / inverse_resolution) * 6.2831853f;
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
    const float3 center = cas_sample(uv);
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
            cosine * base.x - sine * base.y,
            sine * base.x + cosine * base.y
        );
        const float3 outer = cas_sample(
            uv + rotated * inverse_resolution * radius
        );
        const float outer_weight =
            1.1f *
            (
                1.0f +
                smoothstep(
                    highlight_threshold,
                    1.0f,
                    luminance(outer)
                ) * highlight_gain
            );
        accumulated += outer * outer_weight;
        weight_sum += outer_weight;
        const float3 inner = cas_sample(
            uv + rotated * inverse_resolution * radius * 0.55f
        );
        const float inner_weight =
            0.85f *
            (
                1.0f +
                smoothstep(
                    highlight_threshold,
                    1.0f,
                    luminance(inner)
                ) * highlight_gain
            );
        accumulated += inner * inner_weight;
        weight_sum += inner_weight;
    }
    return accumulated / max(weight_sum, 0.00001f);
}

float3 linear_to_srgb(float3 color)
{
    return max(
        1.055f * pow(color, 0.416666667f) - 0.055f,
        0.0f
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

float4 main(FragmentInput input) : SV_Target0
{
    const float2 uv = input.texture_coordinate;
    const float3 post_color = tilt_shift(uv);
    return float4(
        output_color(post_color, input.position.xy),
        1.0f
    );
}
