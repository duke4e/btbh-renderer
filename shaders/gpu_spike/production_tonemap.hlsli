static const uint BTBH_TONEMAP_REINHARD = 0u;
static const uint BTBH_TONEMAP_PBR_NEUTRAL = 1u;

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

float3 btbh_tone_map(
    float3 color,
    uint tonemap_mode,
    float white_point
)
{
    return tonemap_mode == BTBH_TONEMAP_PBR_NEUTRAL
        ? btbh_pbr_neutral_tone_map(color)
        : btbh_reinhard_tone_map(color, white_point);
}

float3 btbh_prepare_lut_input(
    float3 color,
    uint tonemap_mode,
    float white_point
)
{
    return tonemap_mode == BTBH_TONEMAP_PBR_NEUTRAL
        ? btbh_tone_map(color, tonemap_mode, white_point)
        : color;
}

float3 btbh_tone_map_for_output(
    float3 color,
    uint tonemap_mode,
    float white_point,
    uint lut_enabled
)
{
    if (tonemap_mode == BTBH_TONEMAP_PBR_NEUTRAL && lut_enabled != 0u)
    {
        return saturate(color);
    }
    return btbh_tone_map(color, tonemap_mode, white_point);
}
