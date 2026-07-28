Texture2D<float4> albedo_texture : register(t0, space2);
SamplerState albedo_sampler : register(s0, space2);
Texture2D<float4> emission_texture : register(t1, space2);
SamplerState emission_sampler : register(s1, space2);
Texture2D<float4> normal_texture : register(t2, space2);
SamplerState normal_sampler : register(s2, space2);
Texture2D<float4> orm_texture : register(t3, space2);
SamplerState orm_sampler : register(s3, space2);
Texture2D<float> directional_shadow_texture : register(t4, space2);
SamplerState directional_shadow_sampler : register(s4, space2);
Texture2D<float> secondary_directional_shadow_texture
    : register(t5, space2);
SamplerState secondary_directional_shadow_sampler
    : register(s5, space2);

cbuffer MaterialPBR : register(b0, space3)
{
    float4 camera_position;
    float4 ambient_color_energy;
    float4 directional_direction_energy;
    float4 directional_color;
    float4 secondary_directional_direction_energy;
    float4 secondary_directional_color;
    float4 material_factors;
    float4 emission_color_energy;
    uint unlit_enabled;
    uint3 material_padding;
    float4 irradiance_sh[9];
    float4 ground_bottom_energy;
    float4 ground_horizon_curve;
    float4 sky_top_energy;
    float4 sky_horizon_curve;
    float4 sun_color_energy;
    float4 sun_direction_size;
    float4 procedural_controls;
    float4x4 directional_shadow_view_projection;
    float4x4 secondary_directional_shadow_view_projection;
    float4 directional_shadow_controls;
    float4 secondary_directional_shadow_controls;
};

struct FragmentInput
{
    float4 color : TEXCOORD0;
    float2 texture_coordinate : TEXCOORD1;
    float3 world_position : TEXCOORD2;
    float3 world_normal : TEXCOORD3;
    float4 world_tangent : TEXCOORD4;
};

static const float inverse_pi = 0.3183098861837907f;

float3 stable_normalize(float3 value, float3 fallback)
{
    const float length_squared = dot(value, value);
    return length_squared > 0.00000001f
        ? value * rsqrt(length_squared)
        : fallback;
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
        geometry;
    return
        (diffuse + specular) *
        light_color *
        light_energy *
        visibility;
}

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
        max(directional_shadow_controls.y, 0.0f) / 2048.0f;
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
        2048.0f;
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

float2 environment_brdf(float normal_dot_view, float roughness)
{
    const float4 c0 =
        float4(-1.0f, -0.0275f, -0.572f, 0.022f);
    const float4 c1 =
        float4(1.0f, 0.0425f, 1.04f, -0.04f);
    const float4 r = roughness * c0 + c1;
    const float a004 =
        min(r.x * r.x, exp2(-9.28f * normal_dot_view)) *
        r.x + r.y;
    return float2(-1.04f, 1.04f) * a004 + r.zw;
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

float4 main(FragmentInput input) : SV_Target0
{
    const float4 albedo_sample = albedo_texture.Sample(
        albedo_sampler,
        input.texture_coordinate
    ) * input.color;
    const float3 emission = emission_texture.Sample(
        emission_sampler,
        input.texture_coordinate
    ).rgb * emission_color_energy.rgb * emission_color_energy.w;
    if (unlit_enabled != 0u)
    {
        return float4(albedo_sample.rgb + emission, albedo_sample.a);
    }

    float3 geometric_normal = stable_normalize(
        input.world_normal,
        float3(0.0f, 0.0f, 1.0f)
    );
    float3 tangent = input.world_tangent.xyz -
        geometric_normal *
        dot(input.world_tangent.xyz, geometric_normal);
    const float3 tangent_fallback = abs(geometric_normal.z) < 0.999f
        ? stable_normalize(
            cross(float3(0.0f, 0.0f, 1.0f), geometric_normal),
            float3(1.0f, 0.0f, 0.0f)
        )
        : float3(1.0f, 0.0f, 0.0f);
    tangent = stable_normalize(tangent, tangent_fallback);
    const float handedness =
        input.world_tangent.w < 0.0f ? -1.0f : 1.0f;
    const float3 bitangent =
        stable_normalize(
            cross(geometric_normal, tangent),
            float3(0.0f, 1.0f, 0.0f)
        ) * handedness;
    float3 mapped_normal = normal_texture.Sample(
        normal_sampler,
        input.texture_coordinate
    ).xyz * 2.0f - 1.0f;
    mapped_normal.xy *= material_factors.x;
    float3 normal = stable_normalize(
        tangent * mapped_normal.x +
        bitangent * mapped_normal.y +
        geometric_normal * mapped_normal.z,
        geometric_normal
    );
    const float3 orm = orm_texture.Sample(
        orm_sampler,
        input.texture_coordinate
    ).rgb;
    const float occlusion =
        saturate(orm.r * material_factors.y);
    const float roughness = saturate(orm.g * material_factors.z);
    const float metalness = saturate(orm.b * material_factors.w);
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
        secondary_directional_direction_energy.w;
    if (total_energy <= 0.0f)
    {
        return float4(albedo_sample.rgb + emission, albedo_sample.a);
    }

    float3 lighting;
    if (procedural_controls.y > 0.5f)
    {
        const float normal_dot_view =
            max(dot(normal, view), 0.0001f);
        const float3 reflection = reflect(-view, normal);
        const float3 irradiance =
            evaluate_irradiance_sh(normal);
        const float3 sharp_radiance =
            sample_procedural_sky(reflection);
        const float3 broad_radiance =
            evaluate_irradiance_sh(reflection);
        const float3 radiance = lerp(
            sharp_radiance,
            broad_radiance,
            roughness * roughness
        );
        const float2 brdf =
            environment_brdf(normal_dot_view, roughness);
        const float3 diffuse =
            irradiance *
            albedo_sample.rgb *
            (1.0f - metalness) *
            occlusion;
        const float3 specular =
            radiance *
            (f0 * brdf.x + brdf.y) *
            specular_occlusion(
                normal_dot_view,
                occlusion,
                roughness
            );
        lighting = (diffuse + specular) *
            ambient_color_energy.w;
    }
    else
    {
        lighting =
            albedo_sample.rgb *
            (1.0f - metalness) *
            ambient_color_energy.rgb *
            ambient_color_energy.w *
            occlusion;
    }
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
        primary_directional_visibility(
            input.world_position,
            normal
        )
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
        secondary_directional_visibility(
            input.world_position,
            normal
        )
    );
    const float3 tone_mapped = lighting / (1.0f + lighting);
    return float4(tone_mapped + emission, albedo_sample.a);
}
