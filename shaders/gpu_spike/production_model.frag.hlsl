#ifndef BTBH_UNLIT_SHADOW_LIGHT
// How much light unlit surfaces that receive shadows (unlit_enabled 2 and 3) keep in shadow.
#define BTBH_UNLIT_SHADOW_LIGHT 0.45f
#endif
#ifndef BTBH_DIRECTIONAL_SHADOWS
#define BTBH_DIRECTIONAL_SHADOWS 1
#endif

#ifndef BTBH_OMNI_SHADOWS
#define BTBH_OMNI_SHADOWS 1
#endif

#ifndef BTBH_RC_MATERIAL_OUTPUT
#define BTBH_RC_MATERIAL_OUTPUT 0
#endif

#include "production_surface.hlsli"

Texture2D<float4> albedo_texture : register(t0, space2);
SamplerState albedo_sampler : register(s0, space2);
Texture2D<float4> emission_texture : register(t1, space2);
SamplerState emission_sampler : register(s1, space2);
Texture2D<float4> normal_texture : register(t2, space2);
SamplerState normal_sampler : register(s2, space2);
Texture2D<float4> orm_texture : register(t3, space2);
SamplerState orm_sampler : register(s3, space2);
Texture2D<float2> brdf_lut_texture : register(t4, space2);
SamplerState brdf_lut_sampler : register(s4, space2);
#if BTBH_DIRECTIONAL_SHADOWS
// The shadow maps are sampled through comparison samplers (each tap compares the 2x2 texels round it and
// filters the results); the primary directional map is bound a second time (t8) to read its depths, for
// the blocker search of its soft shadows.
Texture2D<float> directional_shadow_texture : register(t5, space2);
SamplerComparisonState directional_shadow_sampler : register(s5, space2);
Texture2D<float> secondary_directional_shadow_texture
    : register(t6, space2);
SamplerComparisonState secondary_directional_shadow_sampler
    : register(s6, space2);
Texture2D<float> spot_shadow_texture : register(t7, space2);
SamplerComparisonState spot_shadow_sampler : register(s7, space2);
Texture2D<float> directional_shadow_depth_texture : register(t8, space2);
SamplerState directional_shadow_depth_sampler : register(s8, space2);
#if BTBH_OMNI_SHADOWS
TextureCube<float> omni_shadow_texture : register(t9, space2);
SamplerComparisonState omni_shadow_sampler : register(s9, space2);
#endif
#endif

cbuffer MaterialPBR : register(b0, space3)
{
    float4 camera_position;
    float4 ambient_color_energy;
    float4 directional_direction_energy;
    float4 directional_color;
    float4 secondary_directional_direction_energy;
    float4 secondary_directional_color;
    float4 spot_position_range;
    float4 spot_direction_energy;
    float4 spot_color_falloff;
    float4 spot_cutoffs;
    float4 material_factors;
    float4 emission_color_energy;
    uint unlit_enabled;
    float alpha_cutoff;
    uint surface_variant;
    float surface_time;
    float4 surface_parameters0;
    float4 surface_parameters1;
    float4 irradiance_sh[9];
    float4 ground_bottom_energy;
    float4 ground_horizon_curve;
    float4 sky_top_energy;
    float4 sky_horizon_curve;
    float4 sun_color_energy;
    float4 sun_direction_size;
    float4 procedural_controls;
    float4 point_light_count;   // x: how many of point_lights are used
    float4 point_lights[64];    // per light: position, radius; colour, energy
#if BTBH_DIRECTIONAL_SHADOWS
    float4x4 directional_shadow_view_projection;
    float4x4 secondary_directional_shadow_view_projection;
    float4x4 spot_shadow_view_projection;
    float4 directional_shadow_controls;
    float4 secondary_directional_shadow_controls;
    float4 spot_shadow_controls;
    float4 omni_shadow_position_range;
#endif
};

struct FragmentInput
{
    float4 pixel_position : SV_Position;
    float4 color : TEXCOORD0;
    float2 texture_coordinate : TEXCOORD1;
    float3 world_position : TEXCOORD2;
    float3 world_normal : TEXCOORD3;
    float3 world_tangent : TEXCOORD4;
    float4 instance_custom : TEXCOORD5;
    float3 world_bitangent : TEXCOORD6;
    float4 surface_data : TEXCOORD7;
    nointerpolation float animated_model : TEXCOORD8;
    bool is_front_face : SV_IsFrontFace;
};

struct FragmentOutput
{
    float4 color : SV_Target0;
    float4 ambient : SV_Target1;
    float4 normal : SV_Target2;
    float4 ambient_specular : SV_Target3;
    float4 emission : SV_Target4;
};

static const float inverse_pi = 0.3183098861837907f;
static const float legacy_default_light_specular = 0.5f;

float3 stable_normalize(float3 value, float3 fallback)
{
    const float length_squared = dot(value, value);
    return length_squared > 0.00000001f
        ? value * rsqrt(length_squared)
        : fallback;
}

// A directional wave's height and slope (gradient) at a position: amplitude, wavenumber, speed.
void add_wave(float2 position, float2 direction, float amplitude, float wavelength, float speed, float time,
              inout float height, inout float2 gradient)
{
    const float k = 6.2831853f / wavelength;
    const float phase = dot(direction, position) * k + time * speed;
    height += amplitude * sin(phase);
    gradient += direction * (amplitude * k * cos(phase));
}

// The pixel water's mask (the ORM texture's blue channel, a texel a game pixel) smooth: interpolated
// between its texels (wrapping, as the ring textures do) and cut at a half with an edge about a screen
// pixel soft, so a shore runs smoothly across the pixels rather than in their steps.
float smooth_water_mask(float2 texture_coordinate)
{
    uint width, height;
    orm_texture.GetDimensions(width, height);
    const int2 size = int2(width, height);
    const float2 texel = texture_coordinate * float2(size) - 0.5f;
    const float2 base = floor(texel);
    const float2 f = texel - base;
    const int2 i0 = ((int2(base) % size) + size) % size;
    const int2 i1 = (i0 + 1) % size;
    const float m00 = orm_texture.Load(int3(i0.x, i0.y, 0)).b;
    const float m10 = orm_texture.Load(int3(i1.x, i0.y, 0)).b;
    const float m01 = orm_texture.Load(int3(i0.x, i1.y, 0)).b;
    const float m11 = orm_texture.Load(int3(i1.x, i1.y, 0)).b;
    const float m = lerp(lerp(m00, m10, f.x), lerp(m01, m11, f.x), f.y);
    const float edge = max(fwidth(m), 1e-4f);
    return saturate((m - 0.5f) / edge + 0.5f);
}

// Where the surface is pixel water (apply_surface): it takes no ambient occlusion there (ssao_normal_scale).
static float ssao_water = 0.0f;

void apply_surface(
    FragmentInput input,
    inout float4 albedo,
    inout float3 emission,
    inout float3 normal_map,
    inout float3 orm
)
{
    if (surface_variant == 1u)
    {
        const float2 wave_uv =
            input.world_position.xz * 1.4f +
            float2(surface_time * 0.9f, surface_time * -0.6f);
        const float wave_a =
            sin(wave_uv.x * 7.2f + sin(wave_uv.y * 4.1f)) * 0.5f + 0.5f;
        const float wave_b =
            cos(wave_uv.y * 6.4f + sin(wave_uv.x * 5.7f)) * 0.5f + 0.5f;
        const float wave_c =
            sin((wave_uv.x + wave_uv.y) * 5.3f) * 0.5f + 0.5f;
        const float waves = smoothstep(
            0.3f,
            0.9f,
            wave_a * 0.4f + wave_b * 0.4f + wave_c * 0.2f
        );
        const float edge = pow(
            1.0f - max(
                dot(
                    stable_normalize(
                        input.world_normal,
                        float3(0.0f, 1.0f, 0.0f)
                    ),
                    float3(0.0f, 1.0f, 0.0f)
                ),
                0.0f
            ),
            2.0f
        );
        const float pulse =
            0.82f +
            0.18f * sin(surface_time * 4.0f + input.world_position.y * 2.0f);
        const float strength =
            (0.22f + waves * 0.42f + edge * 0.55f) * pulse;
        const float3 tint = surface_parameters0.rgb;
        emission += tint * (0.95f * strength);
        albedo.rgb = lerp(
            albedo.rgb,
            albedo.rgb * (1.0f + tint * 0.18f),
            min(1.0f, strength * 0.55f)
        );
    }
    else if (surface_variant == 2u)
    {
        const float2 position = input.world_position.xz * 7.5f;
        const float2 warp = float2(
            sin(position.y * 0.23f + surface_time * 0.20f),
            cos(position.x * 0.17f + surface_time * 0.28f)
        );
        const float3 wave_normal = btbh_surface_wave_normal(
            position + warp * 2.6f,
            0.018f,
            0.55f,
            surface_time
        );
        const float foam =
            smoothstep(0.18f, 0.62f, input.surface_data.x);
        const float ripple = btbh_surface_waves(
            position * 0.72f + warp,
            4,
            surface_time
        );
        albedo.rgb = lerp(
            float3(0.035f, 0.145f, 0.230f),
            float3(0.125f, 0.355f, 0.455f),
            0.35f + 0.20f * ripple
        );
        albedo.rgb = lerp(
            albedo.rgb,
            float3(0.660f, 0.870f, 0.900f),
            foam * 0.20f
        );
        albedo.a = 1.0f;
        normal_map = normalize(
            float3(wave_normal.x * 0.42f, wave_normal.z * 0.42f, 1.0f)
        ) * 2.0f - 1.0f;
        orm = float3(1.0f, 0.18f, 0.0f);
    }
    else if (surface_variant == 3u && input.instance_custom.x > 0.0f)
    {
        const float duration = max(0.01f, input.instance_custom.z);
        const float t = saturate(input.instance_custom.x / duration);
        const float noise = input.surface_data.x;
        const float threshold = input.surface_data.z;
        if (noise < threshold)
        {
            albedo.a = 0.0f;
            return;
        }
        const float edge =
            smoothstep(threshold, threshold + 0.15f, noise);
        albedo.a *= edge * input.surface_data.y;
        const float rim = 1.0f - smoothstep(
            threshold + 0.02f,
            threshold + 0.18f,
            noise
        );
        emission +=
            float3(1.2f, 0.5f, 0.1f) *
            rim *
            (1.5f + 0.5f * (1.0f - t));
        emission +=
            float3(1.0f, 0.7f, 0.3f) *
            smoothstep(0.12f, 0.0f, t) *
            2.0f;
        albedo.rgb *= 1.0f - t * 0.5f;
    }
    else if (surface_variant == 4u && input.instance_custom.y > 0.0f)
    {
        if (!btbh_surface_coverage_visible(input.surface_data.y, input.pixel_position.xy))
        {
            discard;
        }
        const float t = input.surface_data.z;
        emission +=
            float3(1.0f, 0.4f, 0.1f) *
            smoothstep(0.15f, 0.0f, t) *
            2.5f;
        albedo.rgb *= 1.0f - t * 0.4f;
    }
    else if (surface_variant == 5u)
    {
        const float level = input.instance_custom.x;
        if (level > surface_parameters1.y + 0.5f)
        {
            const float2 delta =
                input.world_position.xz - surface_parameters0.xz;
            const float radius = max(surface_parameters0.w, 0.001f);
            const float inner =
                radius * clamp(surface_parameters1.x, 0.05f, 0.95f);
            const float mask =
                1.0f - smoothstep(inner, radius, length(delta));
            const float relative_distance =
                length(delta / radius);
            const float edge =
                smoothstep(0.6f, 1.0f, relative_distance);
            const float ring =
                smoothstep(0.92f, 1.0f, relative_distance) -
                smoothstep(1.0f, 1.08f, relative_distance);
            const float2 wave_uv =
                input.world_position.xz * 1.4f +
                float2(surface_time * 0.9f, surface_time * -0.6f);
            const float wave_a =
                sin(wave_uv.x * 7.2f + sin(wave_uv.y * 4.1f)) *
                    0.5f +
                0.5f;
            const float wave_b =
                cos(wave_uv.y * 6.4f + sin(wave_uv.x * 5.7f)) *
                    0.5f +
                0.5f;
            const float wave_c =
                sin((wave_uv.x + wave_uv.y) * 5.3f) * 0.5f + 0.5f;
            const float waves = smoothstep(
                0.3f,
                0.9f,
                wave_a * 0.4f + wave_b * 0.4f + wave_c * 0.2f
            );
            const float fresnel = pow(
                1.0f -
                    max(
                        dot(
                            stable_normalize(
                                input.world_normal,
                                float3(0.0f, 1.0f, 0.0f)
                            ),
                            float3(0.0f, 1.0f, 0.0f)
                        ),
                        0.0f
                    ),
                2.0f
            );
            const float strength =
                mask *
                0.2f *
                (0.4f + edge * 0.9f + ring * 1.4f) *
                (0.6f + waves * 0.8f + fresnel * 0.7f);
            const float3 tint = float3(1.0f, 0.15f, 0.15f);
            emission += tint * (2.0f * strength);
            albedo.rgb = lerp(
                albedo.rgb,
                albedo.rgb + tint * 0.35f,
                strength * 0.7f
            );
        }
    }
    else if (surface_variant == 7u)
    {
        // Pixel water: where the ORM texture's blue channel (the mask) says, the surface is
        // procedural water instead of its texture: waves in several directions (x of
        // surface_parameters0 scales them), the water's colour (surface_parameters1.rgb, linear)
        // deeper in the troughs and lighter on the slopes, the sky in them, the primary light's
        // highlights (y: their strength, z: sharpness) and foam on the crests. surface_parameters1.w
        // is how much of the texture shows through (its detail).
        const float mask = smooth_water_mask(input.texture_coordinate);
        ssao_water = mask;
        if (mask > 0.0f)
        {
            const float scale = surface_parameters0.x > 0.0f ? surface_parameters0.x : 1.0f;
            const float2 position = input.world_position.xz / scale;
            const float time = surface_time;
            // Long swell and shorter chop; the chop alone makes the fine foam.
            float swell = 0.0f, chop = 0.0f;
            float2 gradient = float2(0.0f, 0.0f), chop_gradient = float2(0.0f, 0.0f);
            add_wave(position, normalize(float2(0.62f, 0.78f)), 1.00f, 74.0f, 1.10f, time, swell, gradient);
            add_wave(position, normalize(float2(-0.71f, 0.70f)), 0.60f, 47.0f, 1.45f, time, swell, gradient);
            add_wave(position, normalize(float2(0.20f, -0.98f)), 0.40f, 29.0f, 1.90f, time, swell, gradient);
            add_wave(position, normalize(float2(-0.94f, -0.33f)), 0.30f, 13.0f, 2.80f, time, chop, chop_gradient);
            add_wave(position, normalize(float2(0.85f, -0.52f)), 0.22f, 8.5f, 3.60f, time, chop, chop_gradient);
            add_wave(position, normalize(float2(0.10f, 0.99f)), 0.16f, 6.0f, 4.40f, time, chop, chop_gradient);
            const float3 ripple = btbh_surface_wave_normal(position * 0.45f, 0.02f, 0.6f, time);
            // Far away (more water per screen pixel) the fine detail would alias into stripes and
            // a haze of sparkles: it fades out, and the highlights soften.
            const float footprint = length(fwidth(position));
            const float fine = saturate(1.6f - footprint * 0.9f);
            const float2 slope = gradient * 0.16f + chop_gradient * (0.09f * fine);
            const float3 normal = stable_normalize(
                float3(-slope.x + ripple.x * 0.5f * fine, 1.0f, -slope.y + ripple.z * 0.5f * fine),
                float3(0.0f, 1.0f, 0.0f)
            );
            // The swell and chop alone (smooth), for the sun's glints.
            const float3 glint_normal = stable_normalize(float3(-slope.x, 1.0f, -slope.y), float3(0.0f, 1.0f, 0.0f));
            const float3 light = stable_normalize(-directional_direction_energy.xyz, float3(0.0f, 1.0f, 0.0f));
            const float3 view = stable_normalize(camera_position.xyz - input.world_position, float3(0.0f, 1.0f, 0.0f));
            const float3 half_vector = stable_normalize(light + view, float3(0.0f, 1.0f, 0.0f));

            const float3 deep = surface_parameters1.rgb;
            const float3 shallow = deep * 1.7f + float3(0.006f, 0.02f, 0.026f);
            const float3 sky = float3(0.30f, 0.45f, 0.60f);
            const float crest = saturate(swell * 0.3f + 0.5f);
            float3 water = lerp(deep, shallow, crest * 0.7f);
            // The waves' slopes facing the sun lighter, the others darker.
            water *= 0.62f + 0.6f * saturate(dot(normal, light) * 1.6f - 0.55f);
            const float fresnel = 0.03f + 0.97f * pow(1.0f - saturate(dot(normal, view)), 5.0f);
            water = lerp(water, sky, min(0.18f, fresnel * 0.5f + (1.0f - normal.y) * 0.5f));
            const float sharpness = surface_parameters0.z > 0.0f ? surface_parameters0.z : 220.0f;
            const float strength = surface_parameters0.y > 0.0f ? surface_parameters0.y : 1.0f;
            const float n_dot_h = saturate(dot(glint_normal, half_vector));
            // The sun's reflection: glints on the waves. A sharp highlight aliases into stripes where the
            // normal changes much from one screen pixel to the next, so the highlight widens (and
            // dims) by how much it does there (Toksvig), and fades where a pixel covers many waves.
            const float3 normal_change = fwidth(glint_normal);
            const float variance = dot(normal_change, normal_change);
            const float effective = sharpness / (1.0f + sharpness * variance * 6.0f);
            const float near = saturate(1.3f - footprint * 2.5f);
            water += directional_color.rgb *
                     (pow(n_dot_h, effective) * 0.6f * (effective / sharpness) * near + pow(saturate(dot(normal, half_vector)), 30.0f) * 0.02f) * strength;
            // Foam in irregular flecks (the renderer's wave noise) on the swell's crests.
            const float flecks = btbh_surface_waves(position * 0.22f + float2(chop, -chop) * 0.6f, 6, time * 0.7f);
            const float foam = smoothstep(0.62f, 0.8f, flecks) * smoothstep(0.6f, 0.95f, crest) * fine;
            water = lerp(water, float3(0.62f, 0.72f, 0.78f), foam * 0.4f);

            const float keep = saturate(surface_parameters1.w);
            const float3 detail = albedo.rgb / max(dot(deep, float3(0.299f, 0.587f, 0.114f)) * 3.0f, 0.02f);
            water = lerp(water, water * saturate(detail), keep);
            // Under the bloom's threshold: the glints sparkle without a haze round them.
            water = min(water, float3(0.92f, 0.92f, 0.92f));
            albedo.rgb = lerp(albedo.rgb, water, mask);
        }
    }
    else if (surface_variant == 6u)
    {
        // Glints: the primary light's highlights on moving waves, added to the colour where
        // the ORM texture's blue channel (the mask) says so; the surface keeps its own colours
        // (for pre-shaded, unlit surfaces such as water drawn in a texture).
        // surface_parameters0: x = scale of the waves, y = strength, z = sharpness, w = how
        // much the waves tilt the surface.
        const float mask = orm.b;
        if (mask > 0.0f)
        {
            const float scale = surface_parameters0.x > 0.0f ? surface_parameters0.x : 0.35f;
            const float2 position = input.world_position.xz * scale;
            const float3 wave = btbh_surface_wave_normal(
                position,
                0.02f,
                0.6f,
                surface_time
            );
            const float tilt = surface_parameters0.w > 0.0f ? surface_parameters0.w : 3.0f;
            const float3 normal = stable_normalize(
                float3(wave.x * tilt, 1.0f, wave.z * tilt),
                float3(0.0f, 1.0f, 0.0f)
            );
            const float3 light = stable_normalize(
                -directional_direction_energy.xyz,
                float3(0.0f, 1.0f, 0.0f)
            );
            const float3 view = stable_normalize(
                camera_position.xyz - input.world_position,
                float3(0.0f, 1.0f, 0.0f)
            );
            const float3 half_vector = stable_normalize(
                light + view,
                float3(0.0f, 1.0f, 0.0f)
            );
            const float sharpness =
                surface_parameters0.z > 0.0f ? surface_parameters0.z : 90.0f;
            const float highlight = pow(
                saturate(dot(normal, half_vector)),
                sharpness
            );
            albedo.rgb +=
                directional_color.rgb *
                (highlight * mask * surface_parameters0.y);
        }
    }
}

float schlick_fresnel(float value)
{
    const float m = 1.0f - value;
    const float m2 = m * m;
    return m2 * m2 * m;
}

float distribution_ggx(float normal_dot_half, float alpha)
{
    const float a = normal_dot_half * alpha;
    const float denominator =
        1.0f - normal_dot_half * normal_dot_half + a * a;
    const float k = alpha / max(denominator, 0.000001f);
    return k * k * inverse_pi;
}

float geometry_ggx(
    float normal_dot_light,
    float normal_dot_view,
    float alpha
)
{
    const float denominator = lerp(
        2.0f * normal_dot_light * normal_dot_view,
        normal_dot_light + normal_dot_view,
        alpha
    );
    return 0.5f / max(denominator, 0.000001f);
}

float3 evaluate_directional_light(
    float3 light_direction,
    float light_energy,
    float3 light_color,
    float3 albedo,
    float3 normal,
    float3 view,
    float roughness,
    float metalness,
    float3 f0,
    float visibility
)
{
    if (light_energy <= 0.0f)
    {
        return 0.0f;
    }
    const float3 light = stable_normalize(
        -light_direction,
        float3(0.0f, 1.0f, 0.0f)
    );
    const float normal_dot_light = dot(normal, light);
    if (normal_dot_light <= 0.0f)
    {
        return 0.0f;
    }
    const float3 half_direction = stable_normalize(
        view + light,
        normal
    );
    const float normal_dot_view = max(dot(normal, view), 0.0001f);
    const float light_dot_half = max(dot(light, half_direction), 0.0f);
    const float normal_dot_half =
        max(dot(normal, half_direction), 0.0f);

    const float fd90_minus_one =
        2.0f * light_dot_half * light_dot_half * roughness - 0.5f;
    const float fd_view =
        1.0f +
        fd90_minus_one * schlick_fresnel(normal_dot_view);
    const float fd_light =
        1.0f +
        fd90_minus_one * schlick_fresnel(normal_dot_light);
    const float3 diffuse =
        albedo *
        (1.0f - metalness) *
        inverse_pi *
        fd_view *
        fd_light *
        normal_dot_light;

    const float clamped_roughness = max(roughness, 0.02f);
    const float alpha = clamped_roughness * clamped_roughness;
    const float distribution =
        distribution_ggx(normal_dot_half, alpha);
    const float geometry = geometry_ggx(
        normal_dot_light,
        normal_dot_view,
        alpha
    );
    const float f90 = saturate(50.0f * f0.g);
    const float3 fresnel =
        f0 +
        (f90 - f0) * schlick_fresnel(light_dot_half);
    const float3 specular =
        normal_dot_light *
        distribution *
        fresnel *
        geometry * legacy_default_light_specular;
    return
        (diffuse + specular) *
        light_color *
        light_energy *
        visibility;
}

// The small point lights' light at a point (diffuse, softened so that surfaces facing away from
// a light keep some of it, as a flash lights up its surroundings).
float3 evaluate_point_lights(float3 world_position, float3 normal)
{
    float3 light = float3(0.0f, 0.0f, 0.0f);
    const uint count = min((uint)point_light_count.x, 32u);
    for (uint i = 0u; i < count; ++i)
    {
        const float4 position_radius = point_lights[i * 2u];
        const float4 color_energy = point_lights[i * 2u + 1u];
        const float3 to_light = position_radius.xyz - world_position;
        const float light_distance = length(to_light);
        if (light_distance >= position_radius.w)
        {
            continue;
        }
        const float falloff = 1.0f - light_distance / position_radius.w;
        const float facing = saturate(
            dot(normal, to_light / max(light_distance, 0.0001f)) * 0.7f + 0.3f
        );
        light += color_energy.rgb * (color_energy.w * falloff * falloff * facing);
    }
    return light;
}

float3 evaluate_spot_light(
    float3 world_position,
    float3 albedo,
    float3 normal,
    float3 view,
    float roughness,
    float metalness,
    float3 f0,
    float shadow_visibility
)
{
    const float3 light_delta =
        spot_position_range.xyz - world_position;
    const float light_distance = length(light_delta);
    if (
        spot_direction_energy.w <= 0.0f ||
        spot_position_range.w <= 0.0f ||
        light_distance <= 0.000001f
    )
    {
        return 0.0f;
    }
    const float3 light = light_delta / light_distance;
    const float range_attenuation = pow(
        1.0f - saturate(light_distance / spot_position_range.w),
        spot_color_falloff.w
    );
    const float epsilon = max(
        spot_cutoffs.x - spot_cutoffs.y,
        0.0001f
    );
    const float theta = dot(
        light,
        -stable_normalize(
            spot_direction_energy.xyz,
            float3(0.0f, -1.0f, 0.0f)
        )
    );
    const float angular_attenuation = smoothstep(
        0.0f,
        1.0f,
        (theta - spot_cutoffs.y) / epsilon
    );
    const float visibility =
        shadow_visibility *
        range_attenuation *
        angular_attenuation;
    return evaluate_directional_light(
        -light,
        spot_direction_energy.w,
        spot_color_falloff.rgb,
        albedo,
        normal,
        view,
        roughness,
        metalness,
        f0,
        visibility
    );
}

#if BTBH_DIRECTIONAL_SHADOWS
// Vogel discs (golden-angle spirals, radius 1: even in area, so no tap pattern repeats) of 8, 16 and 32 taps in one
// table: x, y and the tap's weight (1 - 0.75 r^2: less towards the rim, the penumbra's ends smoother).
static const float3 shadow_taps[56] = {
    // 8
    float3(0.2500f, 0.0000f, 0.9531f), float3(-0.3193f, 0.2925f, 0.8594f), float3(0.0489f, -0.5569f, 0.7656f), float3(0.4024f, 0.5249f, 0.6719f),
    float3(-0.7385f, -0.1306f, 0.5781f), float3(0.6996f, -0.4450f, 0.4844f), float3(-0.2340f, 0.8705f, 0.3906f), float3(-0.4463f, -0.8593f, 0.2969f),
    // 16
    float3(0.1768f, 0.0000f, 0.9766f), float3(-0.2258f, 0.2068f, 0.9297f), float3(0.0346f, -0.3938f, 0.8828f), float3(0.2846f, 0.3712f, 0.8359f),
    float3(-0.5222f, -0.0924f, 0.7891f), float3(0.4947f, -0.3147f, 0.7422f), float3(-0.1655f, 0.6155f, 0.6953f), float3(-0.3156f, -0.6076f, 0.6484f),
    float3(0.6846f, 0.2500f, 0.6016f), float3(-0.7123f, 0.2940f, 0.5547f), float3(0.3434f, -0.7337f, 0.5078f), float3(0.2537f, 0.8089f, 0.4609f),
    float3(-0.7647f, -0.4432f, 0.4141f), float3(0.8971f, -0.1972f, 0.3672f), float3(-0.5475f, 0.7788f, 0.3203f), float3(-0.1265f, -0.9761f, 0.2734f),
    // 32
    float3(0.1250f, 0.0000f, 0.9883f), float3(-0.1596f, 0.1462f, 0.9648f), float3(0.0244f, -0.2784f, 0.9414f), float3(0.2012f, 0.2625f, 0.9180f),
    float3(-0.3693f, -0.0653f, 0.8945f), float3(0.3498f, -0.2225f, 0.8711f), float3(-0.1170f, 0.4352f, 0.8477f), float3(-0.2231f, -0.4296f, 0.8242f),
    float3(0.4841f, 0.1768f, 0.8008f), float3(-0.5036f, 0.2079f, 0.7773f), float3(0.2428f, -0.5188f, 0.7539f), float3(0.1794f, 0.5720f, 0.7305f),
    float3(-0.5408f, -0.3134f, 0.7070f), float3(0.6344f, -0.1395f, 0.6836f), float3(-0.3871f, 0.5507f, 0.6602f), float3(-0.0894f, -0.6902f, 0.6367f),
    float3(0.5491f, 0.4628f, 0.6133f), float3(-0.7389f, 0.0306f, 0.5898f), float3(0.5390f, -0.5363f, 0.5664f), float3(-0.0361f, 0.7798f, 0.5430f),
    float3(-0.5128f, -0.6145f, 0.5195f), float3(0.8124f, 0.1093f, 0.4961f), float3(-0.6883f, 0.4789f, 0.4727f), float3(0.1881f, -0.8361f, 0.4492f),
    float3(0.4350f, 0.7592f, 0.4258f), float3(-0.8504f, -0.2713f, 0.4023f), float3(0.8261f, -0.3817f, 0.3789f), float3(-0.3579f, 0.8552f, 0.3555f),
    float3(-0.3194f, -0.8880f, 0.3320f), float3(0.8499f, 0.4467f, 0.3086f), float3(-0.9440f, 0.2488f, 0.2852f), float3(0.5366f, -0.8345f, 0.2617f)
};

// The blocker search's four gathers round the point (radius 0.45, turned off the axes).
static const float2 shadow_search[4] = {
    float2(0.4365f, 0.1091f), float2(-0.1091f, 0.4365f), float2(-0.4365f, -0.1091f), float2(0.1091f, -0.4365f)
};

// The sun's apparent size: how much wider its penumbra grows per world unit between a blocker and the
// surface it shadows (at softness 1; directional_shadow_controls.y scales it, 0 leaves the comparison
// sampler's own 2x2 filtering).
#ifndef BTBH_SUN_PENUMBRA
#define BTBH_SUN_PENUMBRA 0.035f
#endif
// The greatest distance between blocker and receiver whose penumbra the blocker search covers (world units).
#ifndef BTBH_SUN_SEARCH_GAP
#define BTBH_SUN_SEARCH_GAP 110.0f
#endif

// A directional (orthographic) shadow map's receiver plane: how the depth it stores changes per UV step
// along its u and v on the plane through the point with the given normal, so that every tap of a wide
// filter compares against the surface's own depth there (no acne on slopes, no light leaking). Where the
// light grazes the plane it is clamped (at 73 degrees).
float2 shadow_receiver_gradient(float4x4 view_projection, float3 face_normal)
{
    const float scale_u = length(view_projection[0].xyz);
    const float scale_v = length(view_projection[1].xyz);
    const float scale_z = length(view_projection[2].xyz);
    const float normal_u = dot(face_normal, view_projection[0].xyz) / scale_u;
    const float normal_v = dot(face_normal, view_projection[1].xyz) / scale_v;
    const float normal_z = min(dot(face_normal, view_projection[2].xyz) / scale_z, -0.3f);
    // World depth per world step: -(normal_u, normal_v) / normal_z; a world step is 0.5 scale UV (v down).
    return float2(-normal_u / (0.5f * scale_u), normal_v / (0.5f * scale_v)) * (scale_z / normal_z);
}

// The sun's shadow, soft: percentage-closer soft shadows. A blocker search (gathered depths round the point)
// finds how far above the surface the blockers stand; the penumbra is as wide as that times the sun's size,
// at least about a texel (contact shadows stay sharp and smooth), and filtered over a disc of 4 to 32 taps
// through the comparison sampler, each against the receiver's plane. Where no blocker is found the surface
// is lit at the cost of the search alone, where all it finds is nearer the light it is in the umbra.
// plain: four comparison taps a texel round the point (a tent over 3x3 texels), no blocker search, for see-through
// surfaces drawn over one another (clouds: unlit_enabled 3), whose fragments are many.
float primary_directional_visibility(
    float3 world_position,
    float3 face_normal,
    bool plain
)
{
    if (directional_shadow_controls.x < 0.5f)
    {
        return 1.0f;
    }
    const float4 clip = mul(
        directional_shadow_view_projection,
        float4(world_position, 1.0f)
    );
    if (clip.w <= 0.0f)
    {
        return 1.0f;
    }
    const float3 projected = clip.xyz / clip.w;
    if (
        projected.x < -1.0f || projected.x > 1.0f ||
        projected.y < -1.0f || projected.y > 1.0f ||
        projected.z < 0.0f || projected.z > 1.0f
    )
    {
        return 1.0f;
    }
    const float2 uv = float2(
        projected.x * 0.5f + 0.5f,
        -projected.y * 0.5f + 0.5f
    );
    uint width;
    uint height;
    directional_shadow_texture.GetDimensions(width, height);
    const float2 texel = 1.0f / float2(width, height);
    // UV per world unit along the map's axes, depth per world unit along the light, a texel in world units.
    const float2 uv_per_world = 0.5f * float2(
        length(directional_shadow_view_projection[0].xyz),
        length(directional_shadow_view_projection[1].xyz)
    );
    const float depth_per_world = length(directional_shadow_view_projection[2].xyz);
    const float texel_world = max(texel.x / uv_per_world.x, texel.y / uv_per_world.y);
    const float2 gradient = shadow_receiver_gradient(directional_shadow_view_projection, face_normal);
    // The plane's change over a texel (a tap compares texels up to one away), and a little more.
    const float bias = dot(abs(gradient), texel) + 0.03f * depth_per_world;
    [branch]
    if (plain)
    {
        float plain_lit = 0.0f;
        [unroll]
        for (int i = 0; i < 4; ++i)
        {
            const float2 tap = float2(i & 1 ? 0.5f : -0.5f, i & 2 ? 0.5f : -0.5f) * texel;
            plain_lit += directional_shadow_texture.SampleCmpLevelZero(
                directional_shadow_sampler,
                uv + tap,
                projected.z - bias + dot(gradient, tap)
            );
        }
        return plain_lit * 0.25f;
    }
    const float penumbra_per_world =
        BTBH_SUN_PENUMBRA * max(directional_shadow_controls.y, 0.0f);
    const float least_radius = 0.75f * texel_world;
    const float search_radius = max(least_radius, 0.5f * penumbra_per_world * BTBH_SUN_SEARCH_GAP);
    const float2 search_uv = search_radius * uv_per_world;
    // A depth found counts as a blocker only where its penumbra can reach this point: no further to the side
    // than half its penumbra (its height over the receiver times the sun's size) and the least filter's reach,
    // so the receiver's own bumps round it (terrain, grass) leave it lit at the search's cost alone. Where every
    // depth found is nearer the light than the receiver's plane, it is in the umbra.
    const float reach = least_radius + texel_world;
    const float rise_per_side = penumbra_per_world > 0.0001f ? 2.0f / penumbra_per_world : 100000.0f;

    float blocker_depths = 0.0f;
    float blockers = 0.0f;
    float nearer = 0.0f;
    // Five gathers (20 depths): the point's own and four round it, at 0.45 of the widest penumbra's radius (each
    // gather costs: the commonest case, a lit or narrow penumbra's point, pays for them alone).
    [loop]
    for (int i = 0; i <= 4; ++i)
    {
        const float2 disc = i < 4 ? shadow_search[i] : float2(0.0f, 0.0f);
        const float2 offset = disc * search_uv;
        const float4 depths = directional_shadow_depth_texture.GatherRed(
            directional_shadow_depth_sampler,
            uv + offset
        );
        const float plane = projected.z + dot(gradient, offset) - bias;
        const float margin =
            max(length(disc) * search_radius - reach, 0.0f) * rise_per_side * depth_per_world;
        const float4 near_plane = step(depths, float4(plane, plane, plane, plane));
        const float4 blocking = step(depths, float4(plane, plane, plane, plane) - margin);
        nearer += dot(near_plane, float4(1.0f, 1.0f, 1.0f, 1.0f));
        blockers += dot(blocking, float4(1.0f, 1.0f, 1.0f, 1.0f));
        blocker_depths += dot(blocking, depths);
    }
    [branch]
    if (nearer > 19.5f)
    {
        return 0.0f;
    }
    [branch]
    if (blockers < 0.5f)
    {
        return 1.0f;
    }
    const float gap = max(projected.z - blocker_depths / blockers, 0.0f) / depth_per_world;
    const float radius = clamp(0.5f * penumbra_per_world * gap, least_radius, search_radius);
    const float2 radius_uv = radius * uv_per_world;
    const float radius_texels = radius / texel_world;
    const float2 tap_gradient = gradient * radius_uv;
    const float reference = projected.z - bias;
    // A disc of taps, more of them for wider penumbrae (4 in a square for the narrowest).
    float lit = 0.0f;
    float weights = 0.0f;
    [branch]
    if (radius_texels <= 1.5f)
    {
        [unroll]
        for (int i = 0; i < 4; ++i)
        {
            const float2 tap = float2(i & 1 ? 0.5f : -0.5f, i & 2 ? 0.5f : -0.5f);
            lit += directional_shadow_texture.SampleCmpLevelZero(
                directional_shadow_sampler,
                uv + tap * radius_uv,
                reference + dot(tap_gradient, tap)
            );
        }
        return lit * 0.25f;
    }
    else if (radius_texels <= 3.0f)
    {
        [unroll]
        for (int i = 0; i < 8; ++i)
        {
            const float3 tap = shadow_taps[i];
            lit += tap.z * directional_shadow_texture.SampleCmpLevelZero(
                directional_shadow_sampler,
                uv + tap.xy * radius_uv,
                reference + dot(tap_gradient, tap.xy)
            );
            weights += tap.z;
        }
    }
    else if (radius_texels <= 5.5f)
    {
        [unroll]
        for (int i = 8; i < 24; ++i)
        {
            const float3 tap = shadow_taps[i];
            lit += tap.z * directional_shadow_texture.SampleCmpLevelZero(
                directional_shadow_sampler,
                uv + tap.xy * radius_uv,
                reference + dot(tap_gradient, tap.xy)
            );
            weights += tap.z;
        }
    }
    else
    {
        [unroll]
        for (int i = 24; i < 56; ++i)
        {
            const float3 tap = shadow_taps[i];
            lit += tap.z * directional_shadow_texture.SampleCmpLevelZero(
                directional_shadow_sampler,
                uv + tap.xy * radius_uv,
                reference + dot(tap_gradient, tap.xy)
            );
            weights += tap.z;
        }
    }
    return lit / weights;
}

// A shadow map (orthographic or perspective) filtered over a 3x3 grid of comparison taps, softness texels
// apart (each tap already filtering 2x2 texels).
float shadow_grid_visibility(
    Texture2D<float> shadow_texture,
    SamplerComparisonState shadow_sampler,
    float2 uv,
    float reference,
    float softness
)
{
    uint width;
    uint height;
    shadow_texture.GetDimensions(width, height);
    const float2 step_uv = max(softness, 0.0f) / float2(width, height);
    float visibility = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            // (A tent: the corners count least.)
            const float weight = (2.0f - abs((float)x)) * (2.0f - abs((float)y));
            visibility += weight * shadow_texture.SampleCmpLevelZero(
                shadow_sampler,
                uv + float2(x, y) * step_uv,
                reference
            );
        }
    }
    return visibility / 16.0f;
}

float secondary_directional_visibility(
    float3 world_position,
    float3 normal
)
{
    if (secondary_directional_shadow_controls.x < 0.5f)
    {
        return 1.0f;
    }
    const float4 clip = mul(
        secondary_directional_shadow_view_projection,
        float4(world_position, 1.0f)
    );
    if (clip.w <= 0.0f)
    {
        return 1.0f;
    }
    const float3 projected = clip.xyz / clip.w;
    if (
        projected.x < -1.0f || projected.x > 1.0f ||
        projected.y < -1.0f || projected.y > 1.0f ||
        projected.z < 0.0f || projected.z > 1.0f
    )
    {
        return 1.0f;
    }
    const float2 uv = float2(
        projected.x * 0.5f + 0.5f,
        -projected.y * 0.5f + 0.5f
    );
    const float3 light = stable_normalize(
        -secondary_directional_direction_energy.xyz,
        float3(0.0f, 1.0f, 0.0f)
    );
    const float normal_dot_light = saturate(dot(normal, light));
    const float bias =
        0.001f + 0.0015f * (1.0f - normal_dot_light);
    return shadow_grid_visibility(
        secondary_directional_shadow_texture,
        secondary_directional_shadow_sampler,
        uv,
        projected.z - bias,
        1.5f * secondary_directional_shadow_controls.y
    );
}

float omni_projected_depth(
    float major_distance,
    float near_plane,
    float far_plane
)
{
    const float safe_near = max(near_plane, 0.0001f);
    const float safe_far = max(far_plane, safe_near + 0.0001f);
    const float depth_scale = safe_far / (safe_far - safe_near);
    return depth_scale -
        safe_near * depth_scale / max(major_distance, safe_near);
}

#if BTBH_OMNI_SHADOWS
float omni_visibility(
    float3 world_position,
    float3 normal
)
{
    const float3 from_light =
        world_position - omni_shadow_position_range.xyz;
    const float major_distance = max(
        max(abs(from_light.x), abs(from_light.y)),
        abs(from_light.z)
    );
    const float near_plane = spot_shadow_controls.w;
    const float far_plane = omni_shadow_position_range.w;
    if (major_distance <= near_plane || major_distance >= far_plane)
    {
        return 1.0f;
    }
    const float3 sample_direction = stable_normalize(
        from_light,
        float3(0.0f, 0.0f, 1.0f)
    );
    const float3 light_direction = -sample_direction;
    const float normal_dot_light =
        saturate(dot(normal, light_direction));
    const float world_bias =
        0.025f + 0.1f * (1.0f - normal_dot_light);
    const float reference_depth = omni_projected_depth(
        max(major_distance - world_bias, near_plane),
        near_plane,
        far_plane
    );
    const float3 basis_seed = abs(sample_direction.y) < 0.99f
        ? float3(0.0f, 1.0f, 0.0f)
        : float3(1.0f, 0.0f, 0.0f);
    const float3 tangent = normalize(cross(basis_seed, sample_direction));
    const float3 bitangent = cross(sample_direction, tangent);
    // Taps about 1.5 texels apart at softness 1 (a cube face spans 2 in these units), each filtering 2x2.
    uint face_size;
    uint face_height;
    omni_shadow_texture.GetDimensions(face_size, face_height);
    const float texel_radius =
        max(spot_shadow_controls.y, 0.0f) * 3.0f / (float)face_size;
    float visibility = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const float3 direction = normalize(
                sample_direction +
                tangent * ((float)x * texel_radius) +
                bitangent * ((float)y * texel_radius)
            );
            const float weight = (2.0f - abs((float)x)) * (2.0f - abs((float)y));
            visibility += weight * omni_shadow_texture.SampleCmpLevelZero(
                omni_shadow_sampler,
                direction,
                reference_depth
            );
        }
    }
    return visibility / 16.0f;
}
#endif

float spot_visibility(
    float3 world_position,
    float3 normal
)
{
    if (spot_shadow_controls.x < 0.5f)
    {
        return 1.0f;
    }
#if BTBH_OMNI_SHADOWS
    if (spot_shadow_controls.z > 0.5f)
    {
        return omni_visibility(world_position, normal);
    }
#endif
    const float4 clip = mul(
        spot_shadow_view_projection,
        float4(world_position, 1.0f)
    );
    if (clip.w <= 0.0f)
    {
        return 1.0f;
    }
    const float3 projected = clip.xyz / clip.w;
    if (
        projected.x < -1.0f || projected.x > 1.0f ||
        projected.y < -1.0f || projected.y > 1.0f ||
        projected.z < 0.0f || projected.z > 1.0f
    )
    {
        return 1.0f;
    }
    const float2 uv = float2(
        projected.x * 0.5f + 0.5f,
        -projected.y * 0.5f + 0.5f
    );
    const float3 light = stable_normalize(
        spot_position_range.xyz - world_position,
        float3(0.0f, 1.0f, 0.0f)
    );
    const float normal_dot_light = saturate(dot(normal, light));
    const float bias =
        0.0001f + 0.0005f * (1.0f - normal_dot_light);
    return shadow_grid_visibility(
        spot_shadow_texture,
        spot_shadow_sampler,
        uv,
        projected.z - bias,
        1.5f * spot_shadow_controls.y
    );
}
#endif

float3 evaluate_irradiance_sh(float3 direction)
{
    const float x = direction.x;
    const float y = direction.y;
    const float z = direction.z;
    const float basis[9] = {
        0.2820948f,
        0.48860252f * y,
        0.48860252f * z,
        0.48860252f * x,
        1.0925485f * x * y,
        1.0925485f * y * z,
        0.31539157f * (3.0f * z * z - 1.0f),
        1.0925485f * x * z,
        0.54627424f * (x * x - y * y)
    };
    float3 irradiance = 0.0f;
    [unroll]
    for (uint index = 0u; index < 9u; ++index)
    {
        irradiance += irradiance_sh[index].rgb * basis[index];
    }
    return max(irradiance, 0.0f);
}

float3 sample_procedural_sky(float3 direction)
{
    direction = stable_normalize(
        direction,
        float3(0.0f, 1.0f, 0.0f)
    );
    const float vertical_angle =
        acos(clamp(direction.y, -1.0f, 1.0f));
    float3 color;
    if (direction.y >= 0.0f)
    {
        const float c =
            1.0f - vertical_angle / (3.14159265f * 0.5f);
        const float gradient = saturate(
            1.0f -
            pow(
                1.0f - c,
                1.0f / max(sky_horizon_curve.w, 0.001f)
            )
        );
        color = lerp(
            sky_horizon_curve.rgb,
            sky_top_energy.rgb,
            gradient
        ) * sky_top_energy.w;
    }
    else
    {
        const float c =
            (vertical_angle - 3.14159265f * 0.5f) /
            (3.14159265f * 0.5f);
        const float gradient = saturate(
            1.0f -
            pow(
                1.0f - c,
                1.0f / max(ground_horizon_curve.w, 0.001f)
            )
        );
        color = lerp(
            ground_horizon_curve.rgb,
            ground_bottom_energy.rgb,
            gradient
        ) * ground_bottom_energy.w;
    }

    if (
        sun_direction_size.w > 0.0f &&
        dot(sun_direction_size.xyz, sun_direction_size.xyz) >
            0.00000001f
    )
    {
        const float3 sun_direction = stable_normalize(
            sun_direction_size.xyz,
            float3(0.0f, -1.0f, 0.0f)
        );
        const float sun_angle = acos(clamp(
            dot(sun_direction, direction),
            -1.0f,
            1.0f
        ));
        const float3 sun = sun_color_energy.rgb *
            sun_color_energy.w;
        if (sun_angle < sun_direction_size.w)
        {
            return sun;
        }
        if (sun_angle < sun_direction_size.w * 10.0f)
        {
            const float c =
                (sun_angle - sun_direction_size.w) /
                (sun_direction_size.w * 9.0f);
            const float fade = saturate(
                1.0f -
                pow(
                    1.0f - c,
                    1.0f / max(procedural_controls.x, 0.001f)
                )
            );
            return lerp(sun, color, fade);
        }
    }
    return color;
}

float specular_occlusion(
    float normal_dot_view,
    float occlusion,
    float roughness
)
{
    return saturate(
        pow(
            normal_dot_view + occlusion,
            exp2(-16.0f * roughness - 1.0f)
        ) -
        1.0f +
        occlusion
    );
}

void evaluate_ambient_multiscattering(
    float3 irradiance,
    float3 radiance,
    float3 albedo,
    float metalness,
    float roughness,
    float3 f0,
    float normal_dot_view,
    out float3 ambient_diffuse_lighting,
    out float3 ambient_specular_lighting
)
{
    const float2 brdf = brdf_lut_texture.Sample(
        brdf_lut_sampler,
        float2(normal_dot_view, roughness)
    );
    const float3 single_scattering =
        f0 * brdf.x + brdf.y;
    const float essential_energy = brdf.x + brdf.y;
    const float missing_energy = 1.0f - essential_energy;
    const float3 average_fresnel =
        f0 + (1.0f - f0) / 21.0f;
    const float3 denominator = max(
        1.0f -
            (1.0f - essential_energy) * average_fresnel,
        0.000001f
    );
    const float3 multiple_scattering =
        single_scattering * average_fresnel / denominator;
    const float3 ambient_diffuse =
        multiple_scattering * missing_energy +
        albedo *
            (1.0f - metalness) *
            (1.0f - single_scattering);
    ambient_diffuse_lighting =
        irradiance * ambient_diffuse;
    ambient_specular_lighting =
        radiance * single_scattering;
}

// How the surface takes part in the screen-space ambient occlusion (BTBHGPUSceneSSAOMode, unlit_enabled bits
// 3-4), told to its passes by the length of the normal written (production_ssao.frag.hlsl; every other reader
// normalizes it): 1 takes it and casts it, 0.5 takes it only, 0.75 casts it only, 0.25 neither. Pixel water
// casts it only.
float ssao_normal_scale()
{
    uint mode = (unlit_enabled >> 3u) & 3u;
    if (ssao_water > 0.5f)
    {
        mode = mode == 0u || mode == 2u ? 2u : 3u;
    }
    return mode == 0u ? 1.0f : mode == 1u ? 0.5f : mode == 2u ? 0.75f : 0.25f;
}

FragmentOutput fragment_output(
    float4 color,
    float3 emission,
    float3 ambient_diffuse,
    float3 ambient_specular,
    float3 normal,
    float occlusion,
    float roughness
)
{
    FragmentOutput output;
    output.color = color;
    output.emission = float4(emission, 1.0f);
    output.ambient = float4(ambient_diffuse, 1.0f);
    output.normal = float4(
        normal * (0.5f * ssao_normal_scale()) + 0.5f,
        occlusion
    );
    output.ambient_specular = float4(
        ambient_specular,
        roughness
    );
    return output;
}

#if BTBH_RC_MATERIAL_OUTPUT
FragmentOutput rc_material_output(
    float4 lit_color,
    float4 albedo,
    float3 emission,
    float3 normal,
    float occlusion,
    float roughness,
    float metalness,
    bool unlit
)
{
    FragmentOutput output;
    // An unlit receiver (unlit_enabled bit 2) keeps its albedo for the bounce light it takes, and
    // marks itself with 0.75 in the emission's alpha (unlit surfaces 1, lit ones 0).
    const bool receiver = unlit && (unlit_enabled & 4u) != 0u;
    const float3 source_emission = unlit && !receiver
        ? albedo.rgb + emission
        : emission;
    output.color = lit_color;
    output.ambient = unlit && !receiver
        ? float4(0.0f, 0.0f, 0.0f, 0.0f)
        : float4(albedo.rgb, unlit ? 0.0f : metalness);
    output.normal = float4(
        normal * (0.5f * ssao_normal_scale()) + 0.5f,
        unlit ? 1.0f : occlusion
    );
    output.ambient_specular = float4(
        lerp(float3(0.04f, 0.04f, 0.04f), albedo.rgb, metalness),
        roughness
    );
    output.emission = float4(
        source_emission,
        receiver ? 0.75f : unlit ? 1.0f : 0.0f
    );
    return output;
}
#endif

FragmentOutput main(FragmentInput input)
{
#if BTBH_DIRECTIONAL_SHADOWS
    // The face's own normal, turned towards the camera (from the position's screen derivatives, taken
    // before anything can be discarded): the sun's shadow takes its receiver plane from it.
    float3 shadow_face_normal = stable_normalize(
        cross(ddx(input.world_position), ddy(input.world_position)),
        stable_normalize(input.world_normal, float3(0.0f, 1.0f, 0.0f))
    );
    if (dot(shadow_face_normal, camera_position.xyz - input.world_position) < 0.0f)
    {
        shadow_face_normal = -shadow_face_normal;
    }
#endif
    const float4 raw_albedo_sample = albedo_texture.Sample(
        albedo_sampler,
        input.texture_coordinate
    );
    float4 albedo_sample = float4(
        raw_albedo_sample.rgb * input.color.rgb,
        raw_albedo_sample.a * input.color.a
    );
    float3 emission = emission_texture.Sample(
        emission_sampler,
        input.texture_coordinate
    ).rgb * emission_color_energy.rgb * emission_color_energy.w;
    float3 mapped_normal = normal_texture.Sample(
        normal_sampler,
        input.texture_coordinate
    ).xyz * 2.0f - 1.0f;
    float3 orm = orm_texture.Sample(
        orm_sampler,
        input.texture_coordinate
    ).rgb;
    apply_surface(
        input,
        albedo_sample,
        emission,
        mapped_normal,
        orm
    );
    if (albedo_sample.a < alpha_cutoff)
    {
        discard;
    }
    const float3 geometric_normal = stable_normalize(
        input.world_normal,
        float3(0.0f, 0.0f, 1.0f)
    );
    // A negative normal scale asks for relief from the albedo (unlit surfaces, below), not a
    // normal map.
    mapped_normal.xy *= max(material_factors.x, 0.0f);
    mapped_normal.z = sqrt(
        1.0f - saturate(dot(mapped_normal.xy, mapped_normal.xy))
    );
    float3 normal = stable_normalize(
        input.world_tangent * mapped_normal.x +
        input.world_bitangent * mapped_normal.y +
        input.world_normal * mapped_normal.z,
        geometric_normal
    );
    const bool flip_normal =
        input.animated_model > 0.5f
            ? input.is_front_face
            : !input.is_front_face;
    if (flip_normal)
    {
        normal = -normal;
    }
    const float occlusion =
        saturate(orm.r * material_factors.y);
    const float roughness = saturate(orm.g * material_factors.z);
    const float metalness = saturate(orm.b * material_factors.w);
    // unlit_enabled: 1, 2 or 3 (bits 0-1), bit 2 (4): takes the radiance cascades' light, bits 3-4: how it takes
    // part in the ambient occlusion (ssao_normal_scale).
    if ((unlit_enabled & 3u) != 0u)
    {
#if BTBH_DIRECTIONAL_SHADOWS
        // unlit_enabled 2: unlit, but darkened in the primary directional light's shadow; 3: the same through
        // the plain filter.
        if ((unlit_enabled & 3u) >= 2u)
        {
            const float shadow_visibility = primary_directional_visibility(
                input.world_position,
                shadow_face_normal,
                (unlit_enabled & 3u) == 3u
            );
            albedo_sample.rgb *= lerp(
                BTBH_UNLIT_SHADOW_LIGHT,
                1.0f,
                shadow_visibility
            );
        }
#endif
        // Flashes light up unlit surfaces too (the ground under an explosion). A negative
        // normal_scale asks for relief from the texture's brightness (pixel art seen from above:
        // a horizontal surface with u along +X and v along +Z), which only the flashes show.
        float3 lit_normal = normal;
        if (material_factors.x < 0.0f)
        {
            float texture_width;
            float texture_height;
            albedo_texture.GetDimensions(texture_width, texture_height);
            const float2 texel = float2(1.0f / texture_width, 1.0f / texture_height);
            const float3 luma = float3(0.299f, 0.587f, 0.114f);
            const float2 uv = input.texture_coordinate;
            const float height_left = dot(albedo_texture.Sample(albedo_sampler, uv - float2(texel.x, 0.0f)).rgb, luma);
            const float height_right = dot(albedo_texture.Sample(albedo_sampler, uv + float2(texel.x, 0.0f)).rgb, luma);
            const float height_up = dot(albedo_texture.Sample(albedo_sampler, uv - float2(0.0f, texel.y)).rgb, luma);
            const float height_down = dot(albedo_texture.Sample(albedo_sampler, uv + float2(0.0f, texel.y)).rgb, luma);
            const float strength = -material_factors.x;
            lit_normal = normalize(float3(
                -(height_right - height_left) * strength,
                1.0f,
                -(height_down - height_up) * strength
            ));
        }
        // A dark scene (procedural_controls.z, how dark): unlit surfaces keep that much less of
        // their colour; the spot light lights them (with its shadow, and the relief), and their
        // pixels brighter than procedural_controls.w glow on (lava, lamps).
        if (procedural_controls.z > 0.0f)
        {
            const float darkness = saturate(procedural_controls.z);
            float spot = 0.0f;
            const float3 spot_delta = spot_position_range.xyz - input.world_position;
            const float spot_distance = length(spot_delta);
            if (spot_direction_energy.w > 0.0f && spot_position_range.w > 0.0f && spot_distance > 0.000001f)
            {
                const float3 to_spot = spot_delta / spot_distance;
                const float range_attenuation = pow(
                    1.0f - saturate(spot_distance / spot_position_range.w),
                    spot_color_falloff.w
                );
                const float theta = dot(
                    to_spot,
                    -stable_normalize(spot_direction_energy.xyz, float3(0.0f, -1.0f, 0.0f))
                );
                const float angular = smoothstep(
                    0.0f,
                    1.0f,
                    (theta - spot_cutoffs.y) / max(spot_cutoffs.x - spot_cutoffs.y, 0.0001f)
                );
#if BTBH_DIRECTIONAL_SHADOWS
                const float spot_shadow = spot_visibility(input.world_position, normal);
#else
                const float spot_shadow = 1.0f;
#endif
                // (Less of the lamp than lit surfaces take: the pixel art holds its own light.)
                spot = 0.6f * spot_direction_energy.w * range_attenuation * angular * spot_shadow *
                    saturate(dot(lit_normal, to_spot) * 0.7f + 0.3f);
            }
            const float luminance = dot(albedo_sample.rgb, float3(0.299f, 0.587f, 0.114f));
            const float glow = 0.7f * saturate((luminance - procedural_controls.w) * 4.0f);
            albedo_sample.rgb *= (1.0f - darkness) + darkness * glow + spot * spot_color_falloff.rgb;
        }
        albedo_sample.rgb += albedo_sample.rgb * evaluate_point_lights(input.world_position, lit_normal);
#if BTBH_RC_MATERIAL_OUTPUT
        return rc_material_output(
            float4(albedo_sample.rgb + emission, albedo_sample.a),
            albedo_sample,
            emission,
            normal,
            1.0f,
            1.0f,
            metalness,
            true
        );
#else
        return fragment_output(
            float4(albedo_sample.rgb + emission, albedo_sample.a),
            emission,
            0.0f,
            0.0f,
            normal,
            1.0f,
            1.0f
        );
#endif
    }
    const float3 view = stable_normalize(
        camera_position.xyz - input.world_position,
        -normal
    );
    const float3 f0 = lerp(
        float3(0.04f, 0.04f, 0.04f),
        albedo_sample.rgb,
        metalness
    );

    const float total_energy =
        ambient_color_energy.w +
        directional_direction_energy.w +
        secondary_directional_direction_energy.w +
        spot_direction_energy.w;
    if (total_energy <= 0.0f)
    {
#if BTBH_RC_MATERIAL_OUTPUT
        return rc_material_output(
            float4(albedo_sample.rgb + emission, albedo_sample.a),
            albedo_sample,
            emission,
            normal,
            occlusion,
            roughness,
            metalness,
            false
        );
#else
        return fragment_output(
            float4(albedo_sample.rgb + emission, albedo_sample.a),
            emission,
            0.0f,
            0.0f,
            normal,
            occlusion,
            roughness
        );
#endif
    }

    const float normal_dot_view =
        max(dot(normal, view), 0.0001f);
    float3 irradiance;
    float3 radiance;
    if (procedural_controls.y > 0.5f)
    {
        const float3 reflection = reflect(-view, normal);
        irradiance = evaluate_irradiance_sh(normal);
        const float3 sharp_radiance =
            sample_procedural_sky(reflection);
        const float3 broad_radiance =
            evaluate_irradiance_sh(reflection);
        radiance = lerp(
            sharp_radiance,
            broad_radiance,
            roughness * roughness
        );
        irradiance *=
            ambient_color_energy.w * occlusion;
        radiance *= specular_occlusion(
            normal_dot_view,
            occlusion,
            roughness
        );
    }
    else
    {
        irradiance =
            ambient_color_energy.rgb *
            ambient_color_energy.w *
            occlusion;
        radiance = 0.0f;
    }
    float3 ambient_diffuse_lighting;
    float3 ambient_specular_lighting;
    evaluate_ambient_multiscattering(
        irradiance,
        radiance,
        albedo_sample.rgb,
        metalness,
        roughness,
        f0,
        normal_dot_view,
        ambient_diffuse_lighting,
        ambient_specular_lighting
    );
    float3 lighting =
        ambient_diffuse_lighting +
        ambient_specular_lighting;
#if BTBH_DIRECTIONAL_SHADOWS
    // (Looked up only where the sun reaches the surface at all: elsewhere it adds nothing.)
    float sun_visibility = 0.0f;
    [branch]
    if (dot(normal, directional_direction_energy.xyz) < 0.0f)
    {
        sun_visibility = primary_directional_visibility(
            input.world_position,
            shadow_face_normal,
            false
        );
    }
#endif
    lighting += evaluate_directional_light(
        directional_direction_energy.xyz,
        directional_direction_energy.w,
        directional_color.rgb,
        albedo_sample.rgb,
        normal,
        view,
        roughness,
        metalness,
        f0,
#if BTBH_DIRECTIONAL_SHADOWS
        sun_visibility
#else
        1.0f
#endif
    );
    lighting += evaluate_directional_light(
        secondary_directional_direction_energy.xyz,
        secondary_directional_direction_energy.w,
        secondary_directional_color.rgb,
        albedo_sample.rgb,
        normal,
        view,
        roughness,
        metalness,
        f0,
#if BTBH_DIRECTIONAL_SHADOWS
        secondary_directional_visibility(
            input.world_position,
            normal
        )
#else
        1.0f
#endif
    );
    lighting += evaluate_spot_light(
        input.world_position,
        albedo_sample.rgb,
        normal,
        view,
        roughness,
        metalness,
        f0,
#if BTBH_DIRECTIONAL_SHADOWS
        spot_visibility(input.world_position, normal)
#else
        1.0f
#endif
    );
    lighting += albedo_sample.rgb * (1.0f - metalness * 0.7f) *
        evaluate_point_lights(input.world_position, normal);
#if BTBH_RC_MATERIAL_OUTPUT
    return rc_material_output(
        float4(lighting + emission, albedo_sample.a),
        albedo_sample,
        emission,
        normal,
        occlusion,
        roughness,
        metalness,
        false
    );
#else
    return fragment_output(
        float4(lighting + emission, albedo_sample.a),
        emission,
        ambient_diffuse_lighting,
        ambient_specular_lighting,
        normal,
        occlusion,
        roughness
    );
#endif
}
