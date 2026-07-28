Texture2D<float4> albedo_texture : register(t0, space2);
SamplerState albedo_sampler : register(s0, space2);
Texture2D<float4> emission_texture : register(t1, space2);
SamplerState emission_sampler : register(s1, space2);
Texture2D<float4> normal_texture : register(t2, space2);
SamplerState normal_sampler : register(s2, space2);
Texture2D<float4> orm_texture : register(t3, space2);
SamplerState orm_sampler : register(s3, space2);

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
    float3 f0
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
    return (diffuse + specular) * light_color * light_energy;
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

    float3 lighting =
        albedo_sample.rgb *
        (1.0f - metalness) *
        ambient_color_energy.rgb *
        ambient_color_energy.w;
    lighting += evaluate_directional_light(
        directional_direction_energy.xyz,
        directional_direction_energy.w,
        directional_color.rgb,
        albedo_sample.rgb,
        normal,
        view,
        roughness,
        metalness,
        f0
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
        f0
    );
    const float3 tone_mapped = lighting / (1.0f + lighting);
    return float4(tone_mapped + emission, albedo_sample.a);
}
