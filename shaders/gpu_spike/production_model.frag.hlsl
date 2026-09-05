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
Texture2D<float> directional_shadow_texture : register(t5, space2);
SamplerState directional_shadow_sampler : register(s5, space2);
Texture2D<float> secondary_directional_shadow_texture
    : register(t6, space2);
SamplerState secondary_directional_shadow_sampler
    : register(s6, space2);
Texture2D<float> spot_shadow_texture : register(t7, space2);
SamplerState spot_shadow_sampler : register(s7, space2);
#if BTBH_OMNI_SHADOWS
TextureCube<float> omni_shadow_texture : register(t8, space2);
SamplerState omni_shadow_sampler : register(s8, space2);
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
float primary_directional_visibility(
    float3 world_position,
    float3 normal
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
    const float3 light = stable_normalize(
        -directional_direction_energy.xyz,
        float3(0.0f, 1.0f, 0.0f)
    );
    const float normal_dot_light = saturate(dot(normal, light));
    const float bias =
        0.001f + 0.0015f * (1.0f - normal_dot_light);
    const float texel_radius =
        max(directional_shadow_controls.y, 0.0f) / 512.0f;
    float visibility = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const float stored_depth = directional_shadow_texture.Sample(
                directional_shadow_sampler,
                uv + float2(x, y) * texel_radius
            );
            visibility +=
                projected.z - bias <= stored_depth ? 1.0f : 0.0f;
        }
    }
    return visibility / 9.0f;
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
    const float texel_radius =
        max(secondary_directional_shadow_controls.y, 0.0f) /
        512.0f;
    float visibility = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const float stored_depth =
                secondary_directional_shadow_texture.Sample(
                    secondary_directional_shadow_sampler,
                    uv + float2(x, y) * texel_radius
                );
            visibility +=
                projected.z - bias <= stored_depth ? 1.0f : 0.0f;
        }
    }
    return visibility / 9.0f;
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
    const float texel_radius =
        max(spot_shadow_controls.y, 0.0f) * 2.0f / 512.0f;
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
            const float stored_depth = omni_shadow_texture.Sample(
                omni_shadow_sampler,
                direction
            );
            visibility += reference_depth <= stored_depth ? 1.0f : 0.0f;
        }
    }
    return visibility / 9.0f;
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
    const float texel_radius =
        max(spot_shadow_controls.y, 0.0f) / 1024.0f;
    float visibility = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const float stored_depth = spot_shadow_texture.Sample(
                spot_shadow_sampler,
                uv + float2(x, y) * texel_radius
            );
            visibility +=
                projected.z - bias <= stored_depth ? 1.0f : 0.0f;
        }
    }
    return visibility / 9.0f;
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
        normal * 0.5f + 0.5f,
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
    const float3 source_emission = unlit
        ? albedo.rgb + emission
        : emission;
    output.color = lit_color;
    output.ambient = unlit
        ? float4(0.0f, 0.0f, 0.0f, 0.0f)
        : float4(albedo.rgb, metalness);
    output.normal = float4(
        normal * 0.5f + 0.5f,
        unlit ? 1.0f : occlusion
    );
    output.ambient_specular = float4(
        lerp(float3(0.04f, 0.04f, 0.04f), albedo.rgb, metalness),
        roughness
    );
    output.emission = float4(
        source_emission,
        unlit ? 1.0f : 0.0f
    );
    return output;
}
#endif

FragmentOutput main(FragmentInput input)
{
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
    mapped_normal.xy *= material_factors.x;
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
    if (unlit_enabled != 0u)
    {
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
        primary_directional_visibility(
            input.world_position,
            normal
        )
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
