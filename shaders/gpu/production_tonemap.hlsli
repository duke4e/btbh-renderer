static const uint BTBH_TONEMAP_REINHARD = 0u;
static const uint BTBH_TONEMAP_PBR_NEUTRAL = 1u;
static const uint BTBH_TONEMAP_ACES = 2u;

float3 btbh_reinhard_tone_map(float3 color, float white_point)
{
    const float white_squared = white_point * white_point;
    const float3 white_squared_color = white_squared * color;
    return
        (white_squared_color + color * color) /
        (white_squared_color + white_squared);
}

// Khronos PBR Neutral reference algorithm. Input and output are linear
// Rec.709; output is guaranteed to remain in [0, 1].
float3 btbh_pbr_neutral_tone_map(float3 input_color)
{
    const float start_compression = 0.8f - 0.04f;
    const float desaturation = 0.15f;
    float3 color = max(input_color, 0.0f);

    const float minimum_channel = min(color.r, min(color.g, color.b));
    const float offset = minimum_channel < 0.08f
        ? minimum_channel - 6.25f * minimum_channel * minimum_channel
        : 0.04f;
    color -= offset;

    const float peak = max(color.r, max(color.g, color.b));
    if (peak < start_compression)
    {
        return color;
    }

    const float shoulder = 1.0f - start_compression;
    const float new_peak = 1.0f -
        shoulder * shoulder /
        (peak + shoulder - start_compression);
    color *= new_peak / peak;

    const float desaturation_amount = 1.0f - 1.0f /
        (desaturation * (peak - new_peak) + 1.0f);
    return lerp(color, new_peak.xxx, desaturation_amount);
}

// Narkowicz's fitted ACES curve, matching the compact ACES presentation
// mode used by the legacy R3D backend.
float3 btbh_aces_tone_map(float3 color)
{
    const float a = 2.51f;
    const float b = 0.03f;
    const float c = 2.43f;
    const float d = 0.59f;
    const float e = 0.14f;
    return saturate((color * (a * color + b)) /
        (color * (c * color + d) + e));
}

float3 btbh_apply_color_adjustment(
    float3 color,
    float contrast,
    float saturation
)
{
    // Contrast scales around linear mid grey (0.18) in log space: black stays black and the darks spread
    // instead of clipping (the old 0.5 pivot on linear values crushed a dark night at 1.22 to black).
    color = 0.18f * pow(max(color, 0.0f) / 0.18f, contrast);
    const float luminance = dot(
        color,
        float3(0.2126f, 0.7152f, 0.0722f)
    );
    return saturate(lerp(luminance.xxx, color, saturation));
}

float3 btbh_tone_map(
    float3 color,
    uint tonemap_mode,
    float white_point,
    float exposure,
    float contrast,
    float saturation
)
{
    color = max(color * exposure, 0.0f);
    float3 mapped;
    if (tonemap_mode == BTBH_TONEMAP_PBR_NEUTRAL)
    {
        mapped = btbh_pbr_neutral_tone_map(color);
    }
    else if (tonemap_mode == BTBH_TONEMAP_ACES)
    {
        mapped = btbh_aces_tone_map(color);
    }
    else
    {
        mapped = btbh_reinhard_tone_map(color, white_point);
    }
    return btbh_apply_color_adjustment(mapped, contrast, saturation);
}

float3 btbh_prepare_lut_input(
    float3 color,
    uint tonemap_mode,
    float white_point,
    float exposure,
    float contrast,
    float saturation
)
{
    return tonemap_mode != BTBH_TONEMAP_REINHARD
        ? btbh_tone_map(
            color,
            tonemap_mode,
            white_point,
            exposure,
            contrast,
            saturation
        )
        : color;
}

float3 btbh_tone_map_for_output(
    float3 color,
    uint tonemap_mode,
    float white_point,
    uint lut_enabled,
    float exposure,
    float contrast,
    float saturation
)
{
    if (tonemap_mode != BTBH_TONEMAP_REINHARD && lut_enabled != 0u)
    {
        return saturate(color);
    }
    return btbh_tone_map(
        color,
        tonemap_mode,
        white_point,
        exposure,
        contrast,
        saturation
    );
}
