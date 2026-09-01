Texture2D<float4> cascade_texture : register(t0, space0);
SamplerState cascade_sampler : register(s0, space0);
Texture2D<float4> diffuse_cache_texture : register(t1, space0);
SamplerState diffuse_cache_sampler : register(s1, space0);
Texture2D<float> depth_texture : register(t2, space0);
SamplerState depth_sampler : register(s2, space0);
Texture2D<float4> normal_texture : register(t3, space0);
SamplerState normal_sampler : register(s3, space0);
Texture2D<float4> albedo_material_texture : register(t4, space0);
SamplerState albedo_material_sampler : register(s4, space0);
Texture2D<float4> specular_material_texture : register(t5, space0);
SamplerState specular_material_sampler : register(s5, space0);
Texture2D<float4> emission_texture : register(t6, space0);
SamplerState emission_sampler : register(s6, space0);
Texture2D<float4> direct_light_texture : register(t7, space0);
SamplerState direct_light_sampler : register(s7, space0);
RWTexture2D<float4> indirect_output : register(u0, space1);

cbuffer RadianceResolveUniforms : register(b0, space2)
{
    uint2 rc_resolution;
    uint2 render_resolution;
    uint2 atlas_resolution;
    uint2 probe_count;
    uint2 direction_tile;
    uint direction_count;
    uint probe_spacing_cells;
    float indirect_strength;
    uint diffuse_cache_enabled;
    uint2 diffuse_cache_atlas_resolution;
    uint2 diffuse_cache_normal_tile;
    uint2 diffuse_cache_padding;
    float4x4 inverse_view_projection;
    float4 origin_cell;
    uint2 field_dimensions;
    uint debug_view;
    uint padding1;
    float4 irradiance_sh[9];
    float4 environment_controls;
};

static const float pi = 3.14159265359f;

float3 world_position(float2 uv, float depth)
{
    const float4 clip_position = float4(
        uv.x * 2.0f - 1.0f,
        1.0f - uv.y * 2.0f,
        depth,
        1.0f
    );
    const float4 homogeneous_world = mul(
        inverse_view_projection,
        clip_position
    );
    return homogeneous_world.xyz / homogeneous_world.w;
}

float4 probe_interval(uint2 probe, uint direction_index)
{
    const uint2 direction_coordinate = uint2(
        direction_index % direction_tile.x,
        direction_index / direction_tile.x
    );
    const uint2 atlas_coordinate = uint2(
        probe.x * direction_tile.x + direction_coordinate.x,
        probe.y * direction_tile.y + direction_coordinate.y
    );
    return cascade_texture.SampleLevel(
        cascade_sampler,
        (float2(atlas_coordinate) + 0.5f) / float2(atlas_resolution),
        0.0f
    );
}

float4 spatial_interval(float2 probe_coordinate, uint direction_index)
{
    const uint2 probe_00 = uint2(floor(probe_coordinate));
    const uint2 probe_11 = min(probe_00 + 1u, probe_count - 1u);
    const uint2 probe_10 = uint2(probe_11.x, probe_00.y);
    const uint2 probe_01 = uint2(probe_00.x, probe_11.y);
    const float2 weight = frac(probe_coordinate);
    const float4 bottom = lerp(
        probe_interval(probe_00, direction_index),
        probe_interval(probe_10, direction_index),
        weight.x
    );
    const float4 top = lerp(
        probe_interval(probe_01, direction_index),
        probe_interval(probe_11, direction_index),
        weight.x
    );
    return lerp(bottom, top, weight.y);
}

float4 diffuse_cache_probe(uint2 probe, uint normal_index)
{
    const uint2 normal_coordinate = uint2(
        normal_index % diffuse_cache_normal_tile.x,
        normal_index / diffuse_cache_normal_tile.x
    );
    const uint2 atlas_coordinate =
        probe * diffuse_cache_normal_tile + normal_coordinate;
    return diffuse_cache_texture.SampleLevel(
        diffuse_cache_sampler,
        (float2(atlas_coordinate) + 0.5f) /
            float2(diffuse_cache_atlas_resolution),
        0.0f
    );
}

float4 spatial_diffuse_cache(
    float2 probe_coordinate,
    uint normal_index
)
{
    const uint2 probe_00 = uint2(floor(probe_coordinate));
    const uint2 probe_11 = min(probe_00 + 1u, probe_count - 1u);
    const uint2 probe_10 = uint2(probe_11.x, probe_00.y);
    const uint2 probe_01 = uint2(probe_00.x, probe_11.y);
    const float2 weight = frac(probe_coordinate);
    const float4 bottom = lerp(
        diffuse_cache_probe(probe_00, normal_index),
        diffuse_cache_probe(probe_10, normal_index),
        weight.x
    );
    const float4 top = lerp(
        diffuse_cache_probe(probe_01, normal_index),
        diffuse_cache_probe(probe_11, normal_index),
        weight.x
    );
    return lerp(bottom, top, weight.y);
}

float4 cached_diffuse_irradiance(
    float2 probe_coordinate,
    float3 normal
)
{
    const uint tilted_normal_count = 8u;
    const float azimuth = frac(
        atan2(normal.z, normal.x) / (2.0f * pi) + 1.0f
    ) * float(tilted_normal_count);
    const uint tilted_0 = uint(floor(azimuth)) % tilted_normal_count;
    const uint tilted_1 = (tilted_0 + 1u) % tilted_normal_count;
    const float4 up = spatial_diffuse_cache(probe_coordinate, 0u);
    const float4 side = lerp(
        spatial_diffuse_cache(probe_coordinate, tilted_0 + 1u),
        spatial_diffuse_cache(probe_coordinate, tilted_1 + 1u),
        frac(azimuth)
    );
    return lerp(up, side, saturate(length(normal.xz)));
}

float3 fresnel_schlick(float cosine, float3 f0)
{
    return f0 + (1.0f - f0) * pow(1.0f - cosine, 5.0f);
}

float3 fresnel_schlick_roughness(float cosine, float3 f0, float roughness)
{
    return f0 +
        (max(1.0f - roughness, f0) - f0) *
        pow(1.0f - cosine, 5.0f);
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
    for (uint coefficient = 0u; coefficient < 9u; ++coefficient)
    {
        irradiance += irradiance_sh[coefficient].rgb * basis[coefficient];
    }
    irradiance = max(irradiance, 0.0f);
    const float luminance = dot(
        irradiance,
        float3(0.2126f, 0.7152f, 0.0722f)
    );
    return max(lerp(luminance.xxx, irradiance, 1.18f), 0.0f);
}

float distribution_ggx(float normal_dot_half, float roughness)
{
    const float alpha = max(roughness * roughness, 0.0025f);
    const float alpha_squared = alpha * alpha;
    const float denominator =
        normal_dot_half * normal_dot_half * (alpha_squared - 1.0f) + 1.0f;
    return alpha_squared / max(pi * denominator * denominator, 0.00001f);
}

float geometry_schlick_ggx(float normal_dot_direction, float roughness)
{
    const float shifted = roughness + 1.0f;
    const float k = shifted * shifted * 0.125f;
    return normal_dot_direction /
        max(normal_dot_direction * (1.0f - k) + k, 0.00001f);
}

float3 evaluate_brdf(
    float3 albedo,
    float metalness,
    float roughness,
    float3 f0,
    float3 normal,
    float3 view,
    float3 light
)
{
    const float normal_dot_light = saturate(dot(normal, light));
    const float normal_dot_view = max(saturate(dot(normal, view)), 0.0001f);
    if (normal_dot_light <= 0.0f)
    {
        return 0.0f;
    }
    const float3 half_direction = normalize(view + light);
    const float normal_dot_half = saturate(dot(normal, half_direction));
    const float view_dot_half = saturate(dot(view, half_direction));
    const float3 fresnel = fresnel_schlick(view_dot_half, f0);
    const float distribution = distribution_ggx(normal_dot_half, roughness);
    const float geometry =
        geometry_schlick_ggx(normal_dot_view, roughness) *
        geometry_schlick_ggx(normal_dot_light, roughness);
    const float3 specular =
        distribution * geometry * fresnel /
        max(4.0f * normal_dot_view * normal_dot_light, 0.0001f);
    const float3 diffuse =
        (1.0f - fresnel) * (1.0f - metalness) * albedo / pi;
    return (diffuse + specular) * normal_dot_light;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID)
{
    if (any(dispatch_id.xy >= rc_resolution))
    {
        return;
    }
    const float2 uv =
        (float2(dispatch_id.xy) + 0.5f) / float2(rc_resolution);
    const float depth = depth_texture.SampleLevel(depth_sampler, uv, 0.0f);
    if (depth >= 0.99999f)
    {
        indirect_output[dispatch_id.xy] = 0.0f;
        return;
    }
    const float3 position = world_position(uv, depth);
    const float2 field_cell =
        (position.xz - origin_cell.xy) * origin_cell.w;
    const float4 emission_material = emission_texture.SampleLevel(
        emission_sampler,
        uv,
        0.0f
    );
    const float3 emission = emission_material.rgb;
    const bool unlit_material = emission_material.a > 0.5f;
    if (any(field_cell < 0.0f) ||
        any(field_cell >= float2(field_dimensions)))
    {
        indirect_output[dispatch_id.xy] = float4(
            emission,
            unlit_material ? 1.0f : 0.0f
        );
        return;
    }
    const float2 probe_coordinate = clamp(
        field_cell / float(probe_spacing_cells) - 0.5f,
        0.0f,
        float2(probe_count - 1u)
    );
    const float4 encoded_normal = normal_texture.SampleLevel(
        normal_sampler,
        uv,
        0.0f
    );
    const float3 normal = dot(encoded_normal.xyz, encoded_normal.xyz) > 0.00001f
        ? normalize(encoded_normal.xyz * 2.0f - 1.0f)
        : float3(0.0f, 1.0f, 0.0f);
    const float occlusion = saturate(encoded_normal.a);
    const float4 albedo_material = albedo_material_texture.SampleLevel(
        albedo_material_sampler,
        uv,
        0.0f
    );
    const float3 albedo = albedo_material.rgb;
    const float metalness = saturate(albedo_material.a);
    const float4 specular_material = specular_material_texture.SampleLevel(
        specular_material_sampler,
        uv,
        0.0f
    );
    const float3 f0 = specular_material.rgb;
    const float roughness = max(saturate(specular_material.a), 0.05f);
    if (unlit_material)
    {
        indirect_output[dispatch_id.xy] = debug_view == 10u
            ? float4(0.0f, 0.0f, 0.0f, 1.0f)
            : float4(emission, 1.0f);
        return;
    }
    const float2 field_uv =
        (field_cell + 0.5f) / float2(field_dimensions);
    const float3 raw_direct_radiance = direct_light_texture.SampleLevel(
        direct_light_sampler,
        field_uv,
        0.0f
    ).rgb;
    const float direct_peak = max(
        raw_direct_radiance.r,
        max(raw_direct_radiance.g, raw_direct_radiance.b)
    );
    const float3 direct_radiance =
        raw_direct_radiance / (1.0f + direct_peak * 0.25f);
    const float3 direct_diffuse =
        direct_radiance *
        (1.0f - metalness) *
        albedo / pi;
    if (debug_view == 10u)
    {
        indirect_output[dispatch_id.xy] = float4(direct_diffuse, 1.0f);
        return;
    }
    const float3 view = normalize(world_position(uv, 0.0f) - position);

    float3 reflected_radiance = 0.0f;
    float confidence = 0.0f;
    float open_sky_response = 0.0f;
    float response_sum = 0.0f;
    const bool use_diffuse_cache =
        diffuse_cache_enabled != 0u &&
        metalness <= 0.05f &&
        roughness >= 0.62f &&
        normal.y >= 0.0f;
    if (use_diffuse_cache)
    {
        const float4 cached = cached_diffuse_irradiance(
            probe_coordinate,
            normal
        );
        const float3 diffuse_fresnel = fresnel_schlick(
            saturate(dot(normal, view)),
            f0
        );
        reflected_radiance = cached.rgb *
            (1.0f - diffuse_fresnel) *
            (1.0f - metalness) *
            albedo / pi;
        confidence = 1.0f - cached.a;
        open_sky_response = cached.a;
        response_sum = 1.0f;
    }
    else
    {
        const float direction_measure = 2.0f * pi / float(direction_count);
        [loop]
        for (uint direction_index = 0u;
             direction_index < direction_count;
             ++direction_index)
        {
            const float angle = 2.0f * pi *
                (float(direction_index) + 0.5f) / float(direction_count);
            const float2 direction = float2(cos(angle), sin(angle));
            const float3 light = normalize(float3(
                direction.x,
                0.65f,
                direction.y
            ));
            const float normal_dot_light = saturate(dot(normal, light));
            if (normal_dot_light <= 0.0f)
            {
                continue;
            }
            const float4 interval = spatial_interval(
                probe_coordinate,
                direction_index
            );
            const float3 brdf = evaluate_brdf(
                albedo,
                metalness,
                roughness,
                f0,
                normal,
                view,
                light
            );
            reflected_radiance += interval.rgb * brdf * direction_measure;
            confidence += (1.0f - interval.a) * normal_dot_light;
            open_sky_response += interval.a * normal_dot_light;
            response_sum += normal_dot_light;
        }
    }
    const float traced_sky_visibility = saturate(
        open_sky_response / max(response_sum, 0.00001f)
    );
    // The 2.5D rays measure the low-elevation part of the hemisphere. Vertical
    // sky remains visible above extruded city blockers, so retain its diffuse
    // contribution while using RC transmittance to darken the horizon.
    const float sky_visibility = lerp(
        0.55f,
        1.0f,
        sqrt(traced_sky_visibility)
    );
    const float normal_dot_view = saturate(dot(normal, view));
    const float3 environment_fresnel = fresnel_schlick(
        normal_dot_view,
        f0
    );
    const float3 environment_diffuse =
        evaluate_irradiance_sh(normal) *
        environment_controls.x *
        sky_visibility *
        (1.0f - environment_fresnel) *
        (1.0f - metalness) *
        albedo;
    const float3 reflection = reflect(-view, normal);
    const float3 environment_specular =
        evaluate_irradiance_sh(reflection) *
        environment_controls.x *
        sky_visibility *
        fresnel_schlick_roughness(
            normal_dot_view,
            f0,
            roughness
        ) *
        specular_occlusion(normal_dot_view, occlusion, roughness);
    indirect_output[dispatch_id.xy] = float4(
        emission +
            direct_diffuse * occlusion +
            reflected_radiance * occlusion * indirect_strength +
            environment_diffuse * occlusion +
            environment_specular,
        confidence / max(response_sum, 0.00001f)
    );
}
