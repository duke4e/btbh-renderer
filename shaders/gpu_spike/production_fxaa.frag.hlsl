Texture2D<float4> post_texture : register(t0, space2);
SamplerState post_sampler : register(s0, space2);

cbuffer FXAAUniforms : register(b0, space3)
{
    float4 metrics;
};

struct FragmentInput
{
    float4 position : SV_Position;
    float2 texture_coordinate : TEXCOORD0;
};

float4 sample_post(float2 uv)
{
    return post_texture.SampleLevel(post_sampler, uv, 0.0f);
}

float sample_luma(float2 uv)
{
    return sample_post(uv).g;
}

float sample_luma_offset(float2 uv, int2 offset)
{
    return post_texture.SampleLevel(post_sampler, uv, 0.0f, offset).g;
}

float4 fxaa_quality_15(float2 uv)
{
    const float2 texel = metrics.xy;
    float2 position = uv;
    const float4 center = sample_post(position);
    const float luma_center = center.g;
    float luma_south = sample_luma_offset(position, int2(0, 1));
    const float luma_east = sample_luma_offset(position, int2(1, 0));
    float luma_north = sample_luma_offset(position, int2(0, -1));
    const float luma_west = sample_luma_offset(position, int2(-1, 0));

    const float maximum_south_center = max(luma_south, luma_center);
    const float minimum_south_center = min(luma_south, luma_center);
    const float maximum_east_south_center = max(luma_east, maximum_south_center);
    const float minimum_east_south_center = min(luma_east, minimum_south_center);
    const float maximum_west_north = max(luma_north, luma_west);
    const float minimum_west_north = min(luma_north, luma_west);
    const float range_maximum = max(maximum_west_north, maximum_east_south_center);
    const float range_minimum = min(minimum_west_north, minimum_east_south_center);
    const float range_value = range_maximum - range_minimum;
    const float range_threshold = max(1.0f / 16.0f, range_maximum * (1.0f / 8.0f));
    if (range_value < range_threshold)
    {
        return center;
    }

    const float luma_north_west = sample_luma_offset(position, int2(-1, -1));
    const float luma_south_east = sample_luma_offset(position, int2(1, 1));
    const float luma_north_east = sample_luma_offset(position, int2(1, -1));
    const float luma_south_west = sample_luma_offset(position, int2(-1, 1));

    const float luma_north_south = luma_north + luma_south;
    const float luma_west_east = luma_west + luma_east;
    const float reciprocal_range = 1.0f / range_value;
    const float subpixel_north_south_west_east =
        luma_north_south + luma_west_east;
    const float edge_horizontal_1 =
        (-2.0f * luma_center) + luma_north_south;
    const float edge_vertical_1 =
        (-2.0f * luma_center) + luma_west_east;

    const float luma_north_east_south_east =
        luma_north_east + luma_south_east;
    const float luma_north_west_north_east =
        luma_north_west + luma_north_east;
    const float edge_horizontal_2 =
        (-2.0f * luma_east) + luma_north_east_south_east;
    const float edge_vertical_2 =
        (-2.0f * luma_north) + luma_north_west_north_east;

    const float luma_north_west_south_west =
        luma_north_west + luma_south_west;
    const float luma_south_west_south_east =
        luma_south_west + luma_south_east;
    const float edge_horizontal_4 =
        abs(edge_horizontal_1) * 2.0f + abs(edge_horizontal_2);
    const float edge_vertical_4 =
        abs(edge_vertical_1) * 2.0f + abs(edge_vertical_2);
    const float edge_horizontal_3 =
        (-2.0f * luma_west) + luma_north_west_south_west;
    const float edge_vertical_3 =
        (-2.0f * luma_south) + luma_south_west_south_east;
    const float edge_horizontal = abs(edge_horizontal_3) + edge_horizontal_4;
    const float edge_vertical = abs(edge_vertical_3) + edge_vertical_4;

    const float subpixel_corners =
        luma_north_west_south_west + luma_north_east_south_east;
    float length_sign = texel.x;
    const bool horizontal_span = edge_horizontal >= edge_vertical;
    const float subpixel_a =
        subpixel_north_south_west_east * 2.0f + subpixel_corners;

    if (!horizontal_span)
    {
        luma_north = luma_west;
        luma_south = luma_east;
    }
    if (horizontal_span)
    {
        length_sign = texel.y;
    }
    const float subpixel_b = subpixel_a * (1.0f / 12.0f) - luma_center;

    const float gradient_north = luma_north - luma_center;
    const float gradient_south = luma_south - luma_center;
    float luma_north_pair = luma_north + luma_center;
    const float luma_south_pair = luma_south + luma_center;
    const bool use_north_pair = abs(gradient_north) >= abs(gradient_south);
    const float gradient = max(abs(gradient_north), abs(gradient_south));
    if (use_north_pair)
    {
        length_sign = -length_sign;
    }
    const float subpixel_c = saturate(abs(subpixel_b) * reciprocal_range);

    float2 base_position = position;
    const float2 edge_offset = float2(
        horizontal_span ? texel.x : 0.0f,
        horizontal_span ? 0.0f : texel.y
    );
    if (!horizontal_span)
    {
        base_position.x += length_sign * 0.5f;
    }
    if (horizontal_span)
    {
        base_position.y += length_sign * 0.5f;
    }

    float2 negative_position = base_position - edge_offset;
    float2 positive_position = base_position + edge_offset;
    const float subpixel_d = -2.0f * subpixel_c + 3.0f;
    float negative_end_luma = sample_luma(negative_position);
    const float subpixel_e = subpixel_c * subpixel_c;
    float positive_end_luma = sample_luma(positive_position);

    if (!use_north_pair)
    {
        luma_north_pair = luma_south_pair;
    }
    const float scaled_gradient = gradient * 0.25f;
    const float center_pair_luma = luma_center - luma_north_pair * 0.5f;
    const float subpixel_f = subpixel_d * subpixel_e;
    const bool center_luma_is_negative = center_pair_luma < 0.0f;

    negative_end_luma -= luma_north_pair * 0.5f;
    positive_end_luma -= luma_north_pair * 0.5f;
    bool negative_done = abs(negative_end_luma) >= scaled_gradient;
    bool positive_done = abs(positive_end_luma) >= scaled_gradient;
    if (!negative_done)
    {
        negative_position -= edge_offset * 1.5f;
    }
    if (!positive_done)
    {
        positive_position += edge_offset * 1.5f;
    }

    if (!negative_done || !positive_done)
    {
        if (!negative_done)
        {
            negative_end_luma =
                sample_luma(negative_position) - luma_north_pair * 0.5f;
            negative_done = abs(negative_end_luma) >= scaled_gradient;
            if (!negative_done)
            {
                negative_position -= edge_offset * 2.0f;
            }
        }
        if (!positive_done)
        {
            positive_end_luma =
                sample_luma(positive_position) - luma_north_pair * 0.5f;
            positive_done = abs(positive_end_luma) >= scaled_gradient;
            if (!positive_done)
            {
                positive_position += edge_offset * 2.0f;
            }
        }
    }

    if (!negative_done || !positive_done)
    {
        if (!negative_done)
        {
            negative_end_luma =
                sample_luma(negative_position) - luma_north_pair * 0.5f;
            negative_done = abs(negative_end_luma) >= scaled_gradient;
            if (!negative_done)
            {
                negative_position -= edge_offset * 2.0f;
            }
        }
        if (!positive_done)
        {
            positive_end_luma =
                sample_luma(positive_position) - luma_north_pair * 0.5f;
            positive_done = abs(positive_end_luma) >= scaled_gradient;
            if (!positive_done)
            {
                positive_position += edge_offset * 2.0f;
            }
        }
    }

    if (!negative_done || !positive_done)
    {
        if (!negative_done)
        {
            negative_end_luma =
                sample_luma(negative_position) - luma_north_pair * 0.5f;
            negative_done = abs(negative_end_luma) >= scaled_gradient;
            if (!negative_done)
            {
                negative_position -= edge_offset * 2.0f;
            }
        }
        if (!positive_done)
        {
            positive_end_luma =
                sample_luma(positive_position) - luma_north_pair * 0.5f;
            positive_done = abs(positive_end_luma) >= scaled_gradient;
            if (!positive_done)
            {
                positive_position += edge_offset * 2.0f;
            }
        }
    }

    if (!negative_done || !positive_done)
    {
        if (!negative_done)
        {
            negative_end_luma =
                sample_luma(negative_position) - luma_north_pair * 0.5f;
            negative_done = abs(negative_end_luma) >= scaled_gradient;
            if (!negative_done)
            {
                negative_position -= edge_offset * 2.0f;
            }
        }
        if (!positive_done)
        {
            positive_end_luma =
                sample_luma(positive_position) - luma_north_pair * 0.5f;
            positive_done = abs(positive_end_luma) >= scaled_gradient;
            if (!positive_done)
            {
                positive_position += edge_offset * 2.0f;
            }
        }
    }

    if (!negative_done || !positive_done)
    {
        if (!negative_done)
        {
            negative_end_luma =
                sample_luma(negative_position) - luma_north_pair * 0.5f;
            negative_done = abs(negative_end_luma) >= scaled_gradient;
            if (!negative_done)
            {
                negative_position -= edge_offset * 4.0f;
            }
        }
        if (!positive_done)
        {
            positive_end_luma =
                sample_luma(positive_position) - luma_north_pair * 0.5f;
            positive_done = abs(positive_end_luma) >= scaled_gradient;
            if (!positive_done)
            {
                positive_position += edge_offset * 4.0f;
            }
        }
    }

    if (!negative_done || !positive_done)
    {
        if (!negative_done)
        {
            negative_end_luma =
                sample_luma(negative_position) - luma_north_pair * 0.5f;
            negative_done = abs(negative_end_luma) >= scaled_gradient;
            if (!negative_done)
            {
                negative_position -= edge_offset * 12.0f;
            }
        }
        if (!positive_done)
        {
            positive_end_luma =
                sample_luma(positive_position) - luma_north_pair * 0.5f;
            positive_done = abs(positive_end_luma) >= scaled_gradient;
            if (!positive_done)
            {
                positive_position += edge_offset * 12.0f;
            }
        }
    }

    float negative_distance = position.x - negative_position.x;
    float positive_distance = positive_position.x - position.x;
    if (!horizontal_span)
    {
        negative_distance = position.y - negative_position.y;
        positive_distance = positive_position.y - position.y;
    }
    const bool negative_span_good =
        (negative_end_luma < 0.0f) != center_luma_is_negative;
    const bool positive_span_good =
        (positive_end_luma < 0.0f) != center_luma_is_negative;
    const float span_length = positive_distance + negative_distance;
    const float reciprocal_span_length = 1.0f / span_length;
    const bool negative_direction = negative_distance < positive_distance;
    const float nearest_distance = min(negative_distance, positive_distance);
    const bool span_good =
        negative_direction ? negative_span_good : positive_span_good;
    const float subpixel_g = subpixel_f * subpixel_f;
    const float pixel_offset =
        nearest_distance * (-reciprocal_span_length) + 0.5f;
    const float subpixel_h = subpixel_g * 0.75f;
    const float good_pixel_offset = span_good ? pixel_offset : 0.0f;
    const float final_offset = max(good_pixel_offset, subpixel_h);
    if (!horizontal_span)
    {
        position.x += final_offset * length_sign;
    }
    if (horizontal_span)
    {
        position.y += final_offset * length_sign;
    }

    return float4(sample_post(position).rgb, luma_center);
}

float4 main(FragmentInput input) : SV_Target0
{
    return fxaa_quality_15(input.texture_coordinate);
}
