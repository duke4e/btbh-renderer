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
#include "production_temporal.hlsli"

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
    // x sun curve, y 1 with image-based lighting, z the reactive mask's scale (transparent pass while
    // the temporal upscaler runs, 0 otherwise)
    float4 procedural_controls;
    // x albedo variation, y bump height (render units), z noise cells per render unit,
    // w pattern (0 none, 1 ground, 2 grass, 3 facade)
    float4 detail_parameters;
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
    float4 motion_clip_xy : TEXCOORD9;
    float2 motion_clip_w : TEXCOORD10;
    bool is_front_face : SV_IsFrontFace;
};

struct FragmentOutput
{
    float4 color : SV_Target0;
    float4 ambient : SV_Target1;
    float4 normal : SV_Target2;
    float4 ambient_specular : SV_Target3;
    float4 emission : SV_Target4;
    // Motion vector (UV units) while the temporal upscaler runs; pipelines without a sixth target drop it.
    float4 velocity : SV_Target5;
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
        // World-space waves, so neighbouring water tiles share one surface. The asset keeps its
        // colour: ripples shade it, crests catch a pale foam and the low roughness a sun glint.
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
        // The swell and fine ripples riding it shade the water, crest pale and tilt its normal.
        const float swell = btbh_surface_waves(
            position * 1.6f + warp,
            4,
            surface_time
        );
        const float2 fine_position = position * 4.2f + warp * 1.7f;
        const float fine_time = surface_time * 1.4f;
        const float fine = btbh_surface_waves(fine_position, 4, fine_time);
        const float fine_dx = btbh_surface_waves(
            fine_position + float2(0.04f, 0.0f),
            4,
            fine_time
        ) - fine;
        const float fine_dz = btbh_surface_waves(
            fine_position + float2(0.0f, 0.04f),
            4,
            fine_time
        ) - fine;
        const float shade = swell * 0.6f + fine * 0.4f;
        const float crest = smoothstep(0.58f, 0.90f, shade);
        albedo.rgb *= lerp(0.80f, 1.12f, shade);
        albedo.rgb = lerp(
            albedo.rgb,
            float3(0.860f, 0.950f, 0.970f),
            crest * 0.30f
        );
        albedo.a = 1.0f;
        // Tangent space, as the sampled normal map is after its decode.
        normal_map = normalize(float3(
            wave_normal.x * 0.42f - fine_dx * 6.0f,
            wave_normal.z * 0.42f - fine_dz * 6.0f,
            1.0f
        ));
        orm = float3(1.0f, 0.12f, 0.0f);
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
    else if (surface_variant == 7u)
    {
        // A rim that lights the silhouette in the material's tint, and a faint glow over the whole body, so a
        // body reads against a muted ground whatever light reaches it.
        const float3 normal = stable_normalize(
            input.world_normal,
            float3(0.0f, 1.0f, 0.0f)
        );
        const float3 view = stable_normalize(
            camera_position.xyz - input.world_position,
            float3(0.0f, 1.0f, 0.0f)
        );
        const float rim = pow(1.0f - saturate(abs(dot(normal, view))), 2.5f);
        emission += surface_parameters0.rgb *
            (rim * surface_parameters0.w + surface_parameters1.x);
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
    else if (surface_variant == 6u)
    {
        // Soft particle: the normal map's slot carries the opaque scene's distance from the
        // camera (production_scene_distance), and the billboard fades over
        // surface_parameters0.x render units where it meets the scene instead of cutting a
        // straight edge through the ground.
        normal_map = float3(0.0f, 0.0f, 1.0f);
        const float scene_distance =
            normal_texture.Load(int3(input.pixel_position.xy, 0)).r;
        const float fragment_distance =
            length(input.world_position - camera_position.xyz);
        albedo.a *= saturate(
            (scene_distance - fragment_distance) /
            max(surface_parameters0.x, 0.001f)
        );
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
// Tent-filtered percentage-closer filtering. Every texel in a window around the receiver is
// compared and weighted by a tent of `radius_texels` centred on the exact sample position, so
// the weights move continuously with the receiver and edges fade without the steps of a few
// hard taps. Gathers fetch the window two by two texels (3x3 gathers cover radius 2, 4x4
// cover radius 3). Gather components: w = (0, 0), z = (1, 0), x = (0, 1), y = (1, 1).
float tent_shadow_lit(
    float4 depths,
    float2 corner,
    float2 texel,
    float reference_depth,
    float radius_texels,
    inout float weight_sum
)
{
    const float4 dx =
        float4(corner.x, corner.x + 1.0f, corner.x + 1.0f, corner.x) - texel.x;
    const float4 dy =
        float4(corner.y + 1.0f, corner.y + 1.0f, corner.y, corner.y) - texel.y;
    const float4 weights =
        saturate(1.0f - abs(dx) / radius_texels) *
        saturate(1.0f - abs(dy) / radius_texels);
    weight_sum += dot(weights, 1.0f);
    return dot(weights, step(reference_depth, depths));
}

float tent_shadow_visibility_3x3(
    Texture2D<float> shadow_map,
    SamplerState shadow_sampler,
    float2 uv,
    float reference_depth,
    float map_size,
    float radius_texels
)
{
    const float radius = clamp(radius_texels, 1.0f, 2.0f);
    const float2 texel = uv * map_size - 0.5f;
    const float2 origin = floor(texel) - 2.0f;
    float lit = 0.0f;
    float weight_sum = 0.0f;
    [unroll]
    for (int y = 0; y < 3; ++y)
    {
        [unroll]
        for (int x = 0; x < 3; ++x)
        {
            const float2 corner = origin + float2(x, y) * 2.0f;
            const float4 depths = shadow_map.Gather(
                shadow_sampler,
                (corner + 1.0f) / map_size
            );
            lit += tent_shadow_lit(
                depths, corner, texel, reference_depth, radius, weight_sum
            );
        }
    }
    return weight_sum > 0.0f ? lit / weight_sum : 1.0f;
}

float tent_shadow_visibility_4x4(
    Texture2D<float> shadow_map,
    SamplerState shadow_sampler,
    float2 uv,
    float reference_depth,
    float map_size,
    float radius_texels
)
{
    const float radius = clamp(radius_texels, 1.0f, 3.0f);
    const float2 texel = uv * map_size - 0.5f;
    const float2 origin = floor(texel) - 3.0f;
    float lit = 0.0f;
    float weight_sum = 0.0f;
    [unroll]
    for (int y = 0; y < 4; ++y)
    {
        [unroll]
        for (int x = 0; x < 4; ++x)
        {
            const float2 corner = origin + float2(x, y) * 2.0f;
            const float4 depths = shadow_map.Gather(
                shadow_sampler,
                (corner + 1.0f) / map_size
            );
            lit += tent_shadow_lit(
                depths, corner, texel, reference_depth, radius, weight_sum
            );
        }
    }
    return weight_sum > 0.0f ? lit / weight_sum : 1.0f;
}

// The sun's map (BTBH_GPU_PRIMARY_DIRECTIONAL_SHADOW_MAP_SIZE).
static const float primary_directional_shadow_map_size = 2048.0f;

// `normal` is the interpolated surface normal without normal maps or the surface-detail bump,
// which would speckle flat ground with acne.
float primary_directional_visibility(
    float3 world_position,
    float3 normal
)
{
    if (directional_shadow_controls.x < 0.5f)
    {
        return 1.0f;
    }
    const float3 light = stable_normalize(
        -directional_direction_energy.xyz,
        float3(0.0f, 1.0f, 0.0f)
    );
    const float normal_dot_light = saturate(dot(normal, light));
    // Biases in shadow texels, so they hold at every zoom: the light-space rows are orthonormal
    // axes scaled by 1 / half extent (x, y) and 1 / depth range (z), so one texel spans
    // 2 / (size * |row x|) world units. The receiver moves off its surface by up to two texels on
    // grazing faces and compares a texel per filter step nearer the light.
    const float texel_world =
        2.0f / (
            primary_directional_shadow_map_size *
            max(
                length(directional_shadow_view_projection[0].xyz),
                0.000001f
            )
        );
    const float softness = max(directional_shadow_controls.y, 0.0f);
    const float3 receiver =
        world_position +
        normal * texel_world * (0.5f + 1.5f * (1.0f - normal_dot_light));
    const float4 clip = mul(
        directional_shadow_view_projection,
        float4(receiver, 1.0f)
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
    // The tent spans one texel more than the softness (up to three), and the depth bias grows
    // with it because the farthest texels compared sit that far along the receiver's slope.
    const float radius_texels = min(1.0f + softness, 3.0f);
    const float bias =
        texel_world * radius_texels *
        length(directional_shadow_view_projection[2].xyz);
    return tent_shadow_visibility_4x4(
        directional_shadow_texture,
        directional_shadow_sampler,
        uv,
        projected.z - bias,
        primary_directional_shadow_map_size,
        radius_texels
    );
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
    return tent_shadow_visibility_3x3(
        secondary_directional_shadow_texture,
        secondary_directional_shadow_sampler,
        uv,
        projected.z - bias,
        512.0f,
        1.0f + max(secondary_directional_shadow_controls.y, 0.0f)
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
    return tent_shadow_visibility_3x3(
        spot_shadow_texture,
        spot_shadow_sampler,
        uv,
        projected.z - bias,
        1024.0f,
        1.0f + max(spot_shadow_controls.y, 0.0f)
    );
}
#endif

#if BTBH_DIRECTIONAL_SHADOWS
/* The share of the sun, fill and sky light the spot's own occluder takes where it shadows the
   receiver: spot_cutoffs.z of it within spot_cutoffs.w of the light. A lamp carried by a character
   then shadows the ground with the character; its own light, all the classic spot shadow removes, is
   a small part of daylight. Farther occluders (buildings in the cone) keep the classic shadow. */
float spot_occluder_share(
    float3 world_position,
    float visibility
)
{
    const float strength = saturate(spot_cutoffs.z);
    const float reach = spot_cutoffs.w;
    if (
        strength <= 0.0f || reach <= 0.0f || visibility >= 0.999f ||
        spot_shadow_controls.x < 0.5f || spot_shadow_controls.z > 0.5f
    )
    {
        return 0.0f;
    }
    const float4 clip = mul(
        spot_shadow_view_projection,
        float4(world_position, 1.0f)
    );
    if (clip.w <= 0.0f)
    {
        return 0.0f;
    }
    const float3 projected = clip.xyz / clip.w;
    if (
        projected.x < -1.0f || projected.x > 1.0f ||
        projected.y < -1.0f || projected.y > 1.0f ||
        projected.z < 0.0f || projected.z > 1.0f
    )
    {
        return 0.0f;
    }
    const float2 uv = float2(
        projected.x * 0.5f + 0.5f,
        -projected.y * 0.5f + 0.5f
    );
    /* The nearest occluder around the receiver, as a distance from the light: the spot shadow
       projection is perspective from a near plane of 0.05 to the light's range. */
    const float4 depths = spot_shadow_texture.Gather(spot_shadow_sampler, uv);
    const float nearest = min(min(depths.x, depths.y), min(depths.z, depths.w));
    const float near_plane = 0.05f;
    const float far_plane = max(spot_position_range.w, near_plane * 2.0f);
    const float occluder_distance =
        near_plane * far_plane /
        max(far_plane - nearest * (far_plane - near_plane), 0.0001f);
    const float close =
        1.0f - smoothstep(reach * 0.75f, reach, occluder_distance);
    return strength * (1.0f - visibility) * close;
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
    output.velocity = float4(0.0f, 0.0f, 0.0f, 0.0f);
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
    output.velocity = float4(0.0f, 0.0f, 0.0f, 0.0f);
    return output;
}
#endif

// Paving slab profile at `value` (in cells: a slab is two cells long and one wide, every other row
// shifted by half a slab): 1 on a slab's top, 0 in its joints, with a short bevel between. `shade`
// returns the slab's own shade in -0.5..0.5. Joints and bevels widen with the pixel footprint (cells
// per pixel), so they never get thinner than a pixel.
float btbh_paving_slab(float2 value, float footprint, out float shade)
{
    const float row = floor(value.y);
    const float along = value.x * 0.5f + (fmod(row, 2.0f) != 0.0f ? 0.5f : 0.0f);
    const float column = floor(along);
    shade = btbh_detail_hash(float2(column, row) + float2(3.0f, 7.0f)) - 0.5f;
    const float2 local = float2(frac(along) * 2.0f, frac(value.y));
    const float2 to_joint = min(local, float2(2.0f, 1.0f) - local);
    const float edge = min(to_joint.x, to_joint.y);
    const float joint = 0.04f + footprint * 0.5f;
    return smoothstep(joint, joint + 0.1f + footprint, edge);
}

// Procedural surface detail (SurfaceDetail): albedo grain, broad stains and a bump, all from world-space
// noise. Returns the bump as a surface gradient (Mikkelsen) built from screen-space derivatives, so it
// needs no tangent frame. The frame and the triplanar weights use the face normal of the position
// derivatives, not the interpolated vertex normal, which some imported production map instances
// carry nearly in the surface plane. detail_parameters is per draw, so every branch here is uniform
// and the derivatives stay valid; it runs before any discard.
float3 apply_surface_detail(FragmentInput input, inout float4 albedo)
{
    const uint pattern = (uint)(detail_parameters.w + 0.5f);
    if (pattern == 0u)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }
    const float3 position = input.world_position * detail_parameters.z;
    const float footprint = max(length(ddx(position)), length(ddy(position)));
    const float3 position_dx = ddx(input.world_position);
    const float3 position_dy = ddy(input.world_position);
    const float3 face = cross(position_dx, position_dy);
    const float face_area = length(face);
    const float3 face_normal = face_area > 0.0f ? face / face_area : float3(0.0f, 1.0f, 0.0f);
    float height;
    float slab_shade = 0.0f;
    float joint = 0.0f;
    if (pattern == 3u)
    {
        float3 weights = abs(face_normal);
        weights *= weights;
        weights *= weights;
        weights /= max(weights.x + weights.y + weights.z, 0.0001f);
        height = weights.x * btbh_detail_fbm(position.zy, footprint)
            + weights.y * btbh_detail_fbm(position.xz, footprint)
            + weights.z * btbh_detail_fbm(position.xy, footprint);
    }
    else if (pattern == 4u)
    {
        // Slabs narrower than about three pixels fade into the plain grain, so far growth stages
        // do not shimmer.
        const float slab = btbh_paving_slab(position.xz, footprint, slab_shade);
        const float visible = saturate((0.34f - footprint) * 5.0f);
        slab_shade *= visible;
        joint = (1.0f - slab) * visible;
        height = btbh_detail_fbm(position.xz * 2.5f, footprint * 2.5f) * 0.35f - joint;
    }
    else
    {
        height = btbh_detail_fbm(position.xz, footprint);
    }
    // Broad stains a dozen cells wide never alias, so they stay at any distance (far growth stages
    // keep only these).
    const float stain = btbh_detail_noise(position.xz * 0.083f + 5.17f) - 0.5f;
    float3 tint = float3(1.0f, 1.0f, 1.0f) + detail_parameters.x * (2.0f * height + 1.0f * stain);
    if (pattern == 2u)
    {
        const float drift = saturate(btbh_detail_noise(position.xz * 0.041f + 11.3f) * 1.4f - 0.2f);
        tint *= lerp(
            float3(1.0f, 1.0f, 1.0f) + detail_parameters.x * float3(0.35f, 0.1f, -0.55f),
            float3(1.0f, 1.0f, 1.0f) + detail_parameters.x * float3(-0.45f, 0.08f, 0.3f),
            drift
        );
    }
    else if (pattern == 3u)
    {
        // Walls darken over their lowest render unit fifth, which grounds buildings on the street.
        tint *= lerp(0.8f, 1.0f, saturate(input.world_position.y * 4.0f));
    }
    else if (pattern == 4u)
    {
        // Each slab a shade apart; dirt settles in the joints.
        tint *= (1.0f + detail_parameters.x * 2.4f * slab_shade) * (1.0f - 0.26f * joint);
    }
    albedo.rgb *= max(tint, float3(0.0f, 0.0f, 0.0f));

    const float bump = height * detail_parameters.y;
    // With the face normal the determinant is the pixel footprint area itself.
    const float3 r1 = cross(position_dy, face_normal);
    const float3 r2 = cross(face_normal, position_dx);
    if (face_area < 0.000000000001f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }
    return (ddx(bump) * r1 + ddy(bump) * r2) / face_area;
}

FragmentOutput shade_fragment(FragmentInput input)
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
    const float3 detail_gradient = apply_surface_detail(input, albedo_sample);
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
    normal = stable_normalize(normal - detail_gradient, normal);
    const bool flip_normal =
        input.animated_model > 0.5f
            ? input.is_front_face
            : !input.is_front_face;
    if (flip_normal)
    {
        normal = -normal;
    }
    const float3 shadow_normal =
        flip_normal ? -geometric_normal : geometric_normal;
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
#if BTBH_DIRECTIONAL_SHADOWS
    const float spot_shadow_visibility =
        spot_visibility(input.world_position, normal);
    const float spot_occluder =
        spot_occluder_share(input.world_position, spot_shadow_visibility);
#else
    const float spot_shadow_visibility = 1.0f;
    const float spot_occluder = 0.0f;
#endif
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
            shadow_normal
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
    /* The sun, fill and sky dim where the spot's own occluder shadows them (see
       spot_occluder_share); the spot then adds its own light, shadowed as before. */
    lighting *= 1.0f - spot_occluder;
    lighting += evaluate_spot_light(
        input.world_position,
        albedo_sample.rgb,
        normal,
        view,
        roughness,
        metalness,
        f0,
        spot_shadow_visibility
    );
#if BTBH_RC_MATERIAL_OUTPUT
    /* The cascades' light is scaled by this occlusion: the occluder's shadow keeps its share out of
       it too, or the resolve's max() fills the shadow back in. */
    return rc_material_output(
        float4(lighting + emission, albedo_sample.a),
        albedo_sample,
        emission,
        normal,
        occlusion * (1.0f - spot_occluder),
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

FragmentOutput main(FragmentInput input)
{
    FragmentOutput output = shade_fragment(input);
    output.velocity = float4(
        btbh_temporal_velocity(input.motion_clip_xy, input.motion_clip_w),
        0.0f,
        0.0f
    );
    if (procedural_controls.z > 0.0f)
    {
        // The transparent pass's second target is the reactive mask, not the ambient light.
        output.ambient = float4(
            btbh_temporal_reactive(output.color, procedural_controls.z),
            0.0f,
            0.0f,
            0.0f
        );
    }
    return output;
}
