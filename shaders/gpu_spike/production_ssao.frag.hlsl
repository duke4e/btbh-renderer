// Screen-space ambient occlusion at half the scene's resolution: how much of the hemisphere over each point the
// surfaces within ssao_radius (world units) of it hide, from the depth buffer and the normals. Horizon-based (HBAO,
// SSAO_METHOD 1): 4 directions of 4 steps each (16 depth samples), turned and spaced by a 4x4 pattern that the
// denoise pass evens out (each of its outputs averages all 16 of the pattern's turns, so the result keeps still
// while the picture moves under it); visibility = exp(-ssao_intensity * occlusion) ^ ssao_power. Out: the visibility
// (1: nothing hides the point; -1: the surface takes no occlusion) and the point's view depth, for the denoise and
// resolve passes.
// A normal's length in the normal target tells how its surface takes part (production_model.frag.hlsl,
// BTBHGPUSceneSSAOMode): 1 it takes the occlusion and casts it, 0.75 it casts it only, 0.5 it takes it only (flat
// layers drawn over other surfaces, which would darken what lies round their edges), 0.25 neither.
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
    float exposure;
    float contrast;
    float4x4 inverse_view_projection;
    float projection_scale_y;
    float saturation;
    float ssao_radius;     // world units
    float ssao_intensity;
    float ssao_power;
    float ssao_bias;       // world units
    uint ssao_flags;       // bit 0: the debug view
};

struct FragmentInput
{
    float4 position : SV_Position;
    float2 texture_coordinate : TEXCOORD0;
};

// 0: samples on a spiral; 1: horizons in SSAO_DIRECTIONS directions of SSAO_STEPS steps.
#ifndef SSAO_METHOD
#define SSAO_METHOD 1
#endif
#ifndef SSAO_SAMPLES
#define SSAO_SAMPLES 16
#endif
#ifndef SSAO_DIRECTIONS
#define SSAO_DIRECTIONS 4
#endif
#ifndef SSAO_STEPS
#define SSAO_STEPS 4
#endif
// How the samples spread from the centre: their distance goes as t^SSAO_SPREAD (t even from 0 to 1): above 1 more
// of them near the centre, where what touches the surface is.
#ifndef SSAO_SPREAD
#define SSAO_SPREAD 1.0f
#endif
// The radius on the screen at most (a share of the view's height): far wider, the samples miss the texture caches.
#ifndef SSAO_MOST_RADIUS
#define SSAO_MOST_RADIUS 0.1f
#endif
// A sample a little over the surface's plane is mostly the surface's own curve (a cosine).
#ifndef SSAO_ANGLE_BIAS
#define SSAO_ANGLE_BIAS 0.1f
#endif

static const uint pattern[16] = { 0u, 8u, 2u, 10u, 12u, 4u, 14u, 6u, 3u, 11u, 1u, 9u, 15u, 7u, 13u, 5u };

float depth_at(int2 pixel, float2 texel)
{
    return depth_texture.SampleLevel(depth_sampler, (float2(pixel) + 0.5f) * texel, 0.0f);
}

float3 encoded_normal_at(int2 pixel, float2 texel)
{
    return normal_texture.SampleLevel(normal_sampler, (float2(pixel) + 0.5f) * texel, 0.0f).xyz;
}

// 0: takes and casts, 1: takes only, 2: casts only, 3: neither.
uint ssao_mode_of(float3 encoded)
{
    const float3 n = encoded * 2.0f - 1.0f;
    const float length_squared = dot(n, n);
    return length_squared > 0.7656f ? 0u : length_squared > 0.3906f ? 2u : length_squared > 0.1406f ? 1u : 3u;
}

float4 clip_of(int2 pixel, float2 texel, float depth)
{
    const float2 uv = (float2(pixel) + 0.5f) * texel;
    return float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, depth, 1.0f);
}

float3 world_position(int2 pixel, float2 texel, float depth)
{
    const float4 h = mul(inverse_view_projection, clip_of(pixel, texel, depth));
    return h.xyz / h.w;
}

float view_depth(int2 pixel, float2 texel, float depth)
{
    return abs(1.0f / dot(inverse_view_projection[3], clip_of(pixel, texel, depth)));
}

// The full-resolution pixel a half-resolution one stands for: the nearest of its four or the farthest, by turns
// in a checkerboard, so that both sides of an edge are kept for the depth-aware passes after.
int2 representative_pixel(int2 half_pixel, int2 full_resolution, float2 texel, out float depth)
{
    const int2 base = half_pixel * 2;
    int2 selected = min(base, full_resolution - 1);
    depth = depth_at(selected, texel);
    const bool farthest = ((half_pixel.x + half_pixel.y) & 1) == 0;
    [unroll]
    for (int index = 1; index < 4; ++index)
    {
        const int2 candidate = min(base + int2(index & 1, index >> 1), full_resolution - 1);
        const float candidate_depth = depth_at(candidate, texel);
        if (farthest ? candidate_depth > depth : candidate_depth < depth)
        {
            selected = candidate;
            depth = candidate_depth;
        }
    }
    return selected;
}

float2 main(FragmentInput input) : SV_Target0
{
    uint width, height;
    depth_texture.GetDimensions(width, height);
    const int2 full_resolution = int2(width, height);
    const float2 texel = 1.0f / float2(full_resolution);
    const int2 half_resolution = (full_resolution + 1) / 2;
    const int2 half_pixel = clamp(int2(input.position.xy), int2(0, 0), half_resolution - 1);
    float depth;
    const int2 pixel = representative_pixel(half_pixel, full_resolution, texel, depth);
    if (ssao_enabled == 0u || depth >= 0.99999f)
    {
        return float2(-1.0f, 65000.0f);
    }
    const float linear_depth = view_depth(pixel, texel, depth);
    const float3 encoded = encoded_normal_at(pixel, texel);
    const uint mode = ssao_mode_of(encoded);
    if (mode >= 2u)
    {
        return float2(-1.0f, linear_depth);
    }
    const float3 position = world_position(pixel, texel, depth);
    float3 normal = normalize(encoded * 2.0f - 1.0f);
    // (Turned towards the camera.)
    if (dot(normal, position - world_position(pixel, texel, 0.0f)) > 0.0f)
    {
        normal = -normal;
    }

    // The radius on the screen (full-resolution pixels), at most SSAO_MOST_RADIUS of the view's height.
    float radius = ssao_radius > 0.0f ? ssao_radius : 1.0f;
    float screen_radius = projection_scale_y * 0.5f * float(full_resolution.y) * radius / max(linear_depth, 0.0001f);
    const float most = SSAO_MOST_RADIUS * float(full_resolution.y);
    if (screen_radius > most)
    {
        radius *= most / screen_radius;
        screen_radius = most;
    }
    if (screen_radius < 2.0f)
    {
        return float2(1.0f, linear_depth);
    }
    const float radius_squared = radius * radius;
    const float bias = ssao_bias > 0.0f ? ssao_bias : 0.03f;
    const float angle_bias = SSAO_ANGLE_BIAS;

    const uint k = pattern[(half_pixel.y & 3) * 4 + (half_pixel.x & 3)];
    const float offset = frac(float(k) * 0.6180340f + 0.37f);
    const float2 centre = float2(half_pixel) * 2.0f + 1.0f;
    float occlusion = 0.0f;
#if SSAO_METHOD == 1
    // Horizons: in SSAO_DIRECTIONS directions round the point (turned by the pattern), steps outwards; each step that
    // stands higher over the surface's plane than those before it (its elevation's sine, from the normal) adds what
    // it raises the horizon, less with its distance (HBAO): one occluder in a direction is enough to shade it.
    const float turn = (float(k) + 0.5f) * (6.2831853f / (16.0f * float(SSAO_DIRECTIONS)));
    [unroll]
    for (int d = 0; d < SSAO_DIRECTIONS; ++d)
    {
        const float angle = turn + float(d) * (6.2831853f / float(SSAO_DIRECTIONS));
        const float2 direction = float2(cos(angle), sin(angle));
        float horizon = angle_bias;
        [unroll]
        for (int i = 0; i < SSAO_STEPS; ++i)
        {
            const float t = pow((float(i) + offset) / float(SSAO_STEPS), SSAO_SPREAD);
            const int2 sample_pixel = clamp(int2(centre + direction * (screen_radius * t)), int2(0, 0), full_resolution - 1);
            const float sample_depth = depth_at(sample_pixel, texel);
            if (sample_depth >= 0.99999f)
            {
                continue;
            }
            const float3 v = world_position(sample_pixel, texel, sample_depth) - position;
            const float d2 = dot(v, v);
            if (d2 >= radius_squared || d2 < 0.000001f)
            {
                continue;
            }
            const float c = (dot(v, normal) - bias) * rsqrt(d2);
            if (c <= horizon)
            {
                continue;
            }
            // (Only what casts it occludes: not flat layers.)
            const uint sample_mode = ssao_mode_of(encoded_normal_at(sample_pixel, texel));
            if (sample_mode == 1u || sample_mode == 3u)
            {
                continue;
            }
            occlusion += (c - horizon) * (1.0f - d2 / radius_squared);
            horizon = c;
        }
    }
    occlusion /= (1.0f - angle_bias) * float(SSAO_DIRECTIONS);
#else
    // Samples on a spiral (turned by the pattern): the share of them over the surface's plane, by how high (their
    // elevation's sine) and less with their distance.
    const float turn = (float(k) + 0.5f) * (6.2831853f / 16.0f);
    [unroll]
    for (int i = 0; i < SSAO_SAMPLES; ++i)
    {
        const float t = pow((float(i) + offset) / float(SSAO_SAMPLES), SSAO_SPREAD);
        const float angle = turn + float(i) * 2.3999632f;
        const float2 at = centre + float2(cos(angle), sin(angle)) * (screen_radius * t);
        const int2 sample_pixel = clamp(int2(at), int2(0, 0), full_resolution - 1);
        const float sample_depth = depth_at(sample_pixel, texel);
        if (sample_depth >= 0.99999f)
        {
            continue;
        }
        const float3 v = world_position(sample_pixel, texel, sample_depth) - position;
        const float d2 = dot(v, v);
        if (d2 >= radius_squared || d2 < 0.000001f)
        {
            continue;
        }
        const float c = (dot(v, normal) - bias) * rsqrt(d2) - angle_bias;
        if (c <= 0.0f)
        {
            continue;
        }
        // (Only what casts it occludes: not flat layers.)
        const uint sample_mode = ssao_mode_of(encoded_normal_at(sample_pixel, texel));
        if (sample_mode == 1u || sample_mode == 3u)
        {
            continue;
        }
        occlusion += c * (1.0f - d2 / radius_squared);
    }
    occlusion *= 2.0f / ((1.0f - angle_bias) * float(SSAO_SAMPLES));
#endif
    const float intensity = ssao_intensity > 0.0f ? ssao_intensity : 1.0f;
    // (Exponential: where much hides a point it darkens less than in proportion, so deep corners don't go black.)
    const float visibility = exp2(-1.442695f * intensity * occlusion);
    return float2(pow(visibility, ssao_power > 0.0f ? ssao_power : 1.0f), linear_depth);
}
