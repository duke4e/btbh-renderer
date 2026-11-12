#ifndef BTBH_UNLIT_SHADOW_LIGHT
// How much light unlit surfaces that receive shadows (unlit_enabled 2) keep in shadow.
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

// BTBH_GPU_SCENE_MAX_LAMPS
#define BTBH_MAX_LAMPS 40

#include "production_surface.hlsli"
#include "production_temporal.hlsli"
#include "production_dig_mask.hlsli"

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
// The primary directional map again, through a comparison sampler (each tap compares the 2x2 texels round
// it and filters the results), for the soft sun shadow filter.
Texture2D<float> directional_shadow_compare_texture : register(t8, space2);
SamplerComparisonState directional_shadow_compare_sampler : register(s8, space2);
#if BTBH_OMNI_SHADOWS
TextureCube<float> omni_shadow_texture : register(t9, space2);
SamplerState omni_shadow_sampler : register(s9, space2);
#endif
#endif
// RFC 0007: an SDF terrain's dig mask (production_dig_mask.hlsli), after the variant's other textures.
#if BTBH_DIRECTIONAL_SHADOWS && BTBH_OMNI_SHADOWS
Texture2D<float> dig_mask_texture : register(t10, space2);
SamplerState dig_mask_sampler : register(s10, space2);
#elif BTBH_DIRECTIONAL_SHADOWS
Texture2D<float> dig_mask_texture : register(t9, space2);
SamplerState dig_mask_sampler : register(s9, space2);
#else
Texture2D<float> dig_mask_texture : register(t5, space2);
SamplerState dig_mask_sampler : register(s5, space2);
#endif

cbuffer DigMask : register(b1, space3)
{
    float4 dig_mask_origin;
    float4 dig_mask_scale;
};

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
    // the temporal upscaler runs, 0 otherwise), w the texture LOD bias (negative while the temporal
    // upscaler runs, so textures and the surface detail keep the output size's detail; 0 otherwise)
    float4 procedural_controls;
    // x albedo variation, y bump height (render units), z noise cells per render unit,
    // w pattern (0 none, 1 ground, 2 grass, 3 facade, 4 paving, 5 glass: y is the share of panes lit at
    // night instead and z panes per render unit, 6 a lamp's lens)
    float4 detail_parameters;
    // x surface wetness, 0 dry ... 1 soaked (apply_surface_wetness); y the lit windows' energy (the glass
    // detail's panes, 0 by day); z the rain, 0 dry ... 1 a downpour (btbh_rain_ripples); w reserved
    float4 surface_wetness;
    // Cloud shadows (btbh_cloud_light): xy the cloud layer's drift (render units), z its noise cells per render
    // unit, w the share of the sun a cloud's shadow takes (0 none); cloud_shape x the sky's share under clouds,
    // y their edges' width, z the layer's height (render units), w reserved
    float4 cloud_controls;
    float4 cloud_shape;
    // x the street lamps in use (evaluate_lamps), y their core radius (render units); zw reserved
    float4 lamp_controls;
    // xyz a lamp, w the distance its light ends at
    float4 lamp_position_reach[BTBH_MAX_LAMPS];
    // linear rgb, w the energy
    float4 lamp_color_energy[BTBH_MAX_LAMPS];
    float4 point_light_count;   // x: how many of point_lights are used, y unlit darkness, z unlit glow threshold, w their ceiling (0: none)
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

// Where the surface is pixel water (apply_surface): it takes no horizon-based ambient occlusion there
// (ssao_normal_scale).
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
        // Foam only on the highest crests, broken into the fine ripples' streaks: a whole crest washed pale
        // read as white blots on the dark water (2026-10-22).
        const float crest =
            smoothstep(0.72f, 0.95f, shade) * smoothstep(0.45f, 0.85f, fine);
        albedo.rgb *= lerp(0.80f, 1.12f, shade);
        albedo.rgb = lerp(
            albedo.rgb,
            float3(0.860f, 0.950f, 0.970f),
            crest * 0.22f
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
    else if (surface_variant == 9u)
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
    else if (surface_variant == 8u)
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

// The point lights' light added to a surface already this bright (base), eased out towards the
// room it has left under the ceiling (point_light_count.w, luminance; at least a fifth of it):
// dark ground still flashes bright, pale ground under many flashes only glows instead of
// burning white. A ceiling of 0 leaves the light as it is.
float3 ease_point_light(float3 base, float3 added)
{
    const float ceiling = point_light_count.w;
    const float3 weights = float3(0.299f, 0.587f, 0.114f);
    const float added_luminance = dot(added, weights);
    if (ceiling <= 0.0f || added_luminance <= 0.0001f)
    {
        return added;
    }
    const float room = max(ceiling - dot(base, weights), ceiling * 0.2f);
    return added * (room * (1.0f - exp(-added_luminance / room)) / added_luminance);
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

// Cloud shadows: a layer of soft clouds cloud_shape.z render units up, drifting with the wind, shades what the
// sun's ray from `world_position` reaches through a cloud. Three octaves of world-space noise around 0.5; the
// sky's share under clouds (cloud_shape.x) sets the threshold, cloud_shape.y the edges' width. Returns the share
// of the sun's light left (1 in the clear, 1 - cloud_controls.w under a cloud).
float btbh_cloud_light(float3 world_position)
{
    const float strength = cloud_controls.w;
    const float3 to_sun = -directional_direction_energy.xyz;
    if (strength <= 0.0f || to_sun.y <= 0.05f)
    {
        return 1.0f;
    }
    const float rise = max(cloud_shape.z - world_position.y, 0.0f);
    const float2 layer =
        (world_position.xz + to_sun.xz * (rise / to_sun.y) + cloud_controls.xy) * cloud_controls.z;
    const float density =
        0.5f * btbh_detail_noise(layer) +
        0.3f * btbh_detail_noise(layer * 2.13f + float2(17.3f, 5.9f)) +
        0.2f * btbh_detail_noise(layer * 4.37f + float2(3.1f, 23.7f));
    const float threshold = 1.0f - cloud_shape.x;
    const float cover = smoothstep(threshold - cloud_shape.y, threshold + cloud_shape.y, density);
    return 1.0f - strength * cover;
}

// Street lamps: point lights through the sun's BRDF, so the surface's own normal (its bump) and roughness (its
// wetness) shape them: soft pools on dry ground, glints and streaks on wet ground. A lamp's light stays near full
// within its core radius and falls off with distance, smoothly to nothing at its reach.
float3 evaluate_lamps(
    float3 world_position,
    float3 albedo,
    float3 normal,
    float3 view,
    float roughness,
    float metalness,
    float3 f0
)
{
    const uint count = min((uint)(lamp_controls.x + 0.5f), (uint)BTBH_MAX_LAMPS);
    const float core_squared = max(lamp_controls.y * lamp_controls.y, 0.0001f);
    float3 lighting = 0.0f;
    [loop]
    for (uint index = 0u; index < count; ++index)
    {
        const float4 lamp = lamp_position_reach[index];
        const float3 delta = lamp.xyz - world_position;
        const float distance_squared = dot(delta, delta);
        const float reach_squared = lamp.w * lamp.w;
        if (distance_squared >= reach_squared)
        {
            continue;
        }
        const float fade = 1.0f - distance_squared / reach_squared;
        const float falloff = fade * fade * core_squared / (distance_squared + core_squared);
        lighting += evaluate_directional_light(
            -delta * rsqrt(max(distance_squared, 0.000001f)),
            lamp_color_energy[index].w,
            lamp_color_energy[index].rgb,
            albedo,
            normal,
            view,
            roughness,
            metalness,
            f0,
            falloff
        );
    }
    return lighting;
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
    float2 map_size,
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

// The soft sun shadow filter (BTBH_GPU_SCENE_SHADOW_FILTER_SOFT, directional_shadow_controls.z), from
// Tyrian3D: percentage-closer soft shadows through the primary map's comparison binding (t8).
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
float primary_directional_soft_visibility(
    float3 world_position,
    float3 face_normal,
    bool plain
)
{
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
            plain_lit += directional_shadow_compare_texture.SampleCmpLevelZero(
                directional_shadow_compare_sampler,
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
        const float4 depths = directional_shadow_texture.GatherRed(
            directional_shadow_sampler,
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
            lit += directional_shadow_compare_texture.SampleCmpLevelZero(
                directional_shadow_compare_sampler,
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
            lit += tap.z * directional_shadow_compare_texture.SampleCmpLevelZero(
                directional_shadow_compare_sampler,
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
            lit += tap.z * directional_shadow_compare_texture.SampleCmpLevelZero(
                directional_shadow_compare_sampler,
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
            lit += tap.z * directional_shadow_compare_texture.SampleCmpLevelZero(
                directional_shadow_compare_sampler,
                uv + tap.xy * radius_uv,
                reference + dot(tap_gradient, tap.xy)
            );
            weights += tap.z;
        }
    }
    return lit / weights;
}


// `normal` is the interpolated surface normal without normal maps or the surface-detail bump,
// which would speckle flat ground with acne. The soft filter (directional_shadow_controls.z) takes
// the face's own normal instead, and plain (unlit_enabled 3) asks it for its four-tap lookup.
float primary_directional_visibility(
    float3 world_position,
    float3 normal,
    float3 face_normal,
    bool plain
)
{
    if (directional_shadow_controls.x < 0.5f)
    {
        return 1.0f;
    }
    [branch]
    if (directional_shadow_controls.z > 0.5f)
    {
        return primary_directional_soft_visibility(world_position, face_normal, plain);
    }
    // The sun's map (BTBH_GPU_PRIMARY_DIRECTIONAL_SHADOW_MAP_SIZE unless the host set its size).
    uint map_width;
    uint map_height;
    directional_shadow_texture.GetDimensions(map_width, map_height);
    const float2 primary_directional_shadow_map_size = float2(map_width, map_height);
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
            primary_directional_shadow_map_size.x *
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
    /* Eased in over most of the reach: across the last quarter alone, a building the carrier walked up
       to darkened its whole shadow in a few steps (the shadow "jumped", play-test 2026-10-14). */
    const float close =
        1.0f - smoothstep(reach * 0.4f, reach, occluder_distance);
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

// How the surface takes part in the horizon-based ambient occlusion (BTBHGPUSceneSSAOMode, unlit_enabled bits
// 3-4, bit 5 set while it runs), told to its passes by the length of the normal written (production_hbao.frag.hlsl;
// every other reader normalizes it): 1 takes it and casts it, 0.5 takes it only, 0.75 casts it only, 0.25 neither.
// Pixel water casts it only. Without the HBAO the normal is written whole.
float ssao_normal_scale()
{
    if ((unlit_enabled & 32u) == 0u)
    {
        return 1.0f;
    }
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
    output.velocity = float4(0.0f, 0.0f, 0.0f, 0.0f);
    return output;
}

#if BTBH_RC_MATERIAL_OUTPUT
/* The emission target's alpha marks unlit materials (1). A lit one's alpha is its light's directness (the share
   of it the sun and the spot cast) times this, below the HRC resolve's unlit test (0.5): the SSAO resolve's
   contact AO leaves direct light alone (rc_directness_scale there). */
static const float btbh_rc_directness_scale = 0.45f;
static const float3 btbh_rc_luma = float3(0.2126f, 0.7152f, 0.0722f);

FragmentOutput rc_material_output(
    float4 lit_color,
    float4 albedo,
    float3 emission,
    float3 normal,
    float occlusion,
    float roughness,
    float metalness,
    float3 f0,
    float directness,
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
        f0,
        roughness
    );
    output.emission = float4(
        source_emission,
        receiver ? 0.75f : unlit ? 1.0f : saturate(directness) * btbh_rc_directness_scale
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

// A lamp's lens (detail pattern 6: car head and tail lights, beacons) shines its own colour this many times over
// at night, as the windows' light (surface_wetness.y) rises: bright enough to bloom.
static const float btbh_lamp_lens_gain = 8.0f;

// Glass (detail pattern 5) reflects like coated glass, and its own colour shows half as strongly: what lies
// behind a window is darker than the wall around it.
static const float btbh_glass_reflectance = 0.12f;
static const float btbh_glass_diffuse = 0.5f;

// A glass wall's pane: a world-space grid of detail_parameters.z panes per render unit, across the wall and up
// it (a storey and a bay each). Returns false on level glass (roofs, skylights), which has no panes. `inside` is
// 1 within a pane and 0 on its frame; frames widen with the pixel footprint (panes per pixel), then fade into
// the glass, so far growth stages do not shimmer.
bool btbh_glass_pane(
    float3 world_position,
    float3 face_normal,
    float footprint,
    out float2 cell,
    out float inside
)
{
    cell = float2(0.0f, 0.0f);
    inside = 1.0f;
    if (length(face_normal.xz) < 0.5f)
    {
        return false;
    }
    // The wall's heading snaps to one of 16: the face normal from the position derivatives wobbles by a fraction
    // of a degree from pixel to pixel, which a few hundred render units from the origin moved the pane edges by
    // whole panes (ragged lit windows). A heading up to 11 degrees off stretches the panes by 2 % at most.
    const float heading = round(atan2(face_normal.z, face_normal.x) * (8.0f / 3.14159265f))
        * (3.14159265f / 8.0f);
    const float2 across = float2(-sin(heading), cos(heading));
    const float2 pane = float2(
        dot(world_position.xz, across),
        world_position.y
    ) * detail_parameters.z;
    cell = floor(pane);
    const float2 local = frac(pane);
    const float edge = min(min(local.x, 1.0f - local.x), min(local.y, 1.0f - local.y));
    const float frame = 0.03f + footprint * 0.5f;
    inside = lerp(
        1.0f,
        smoothstep(frame, frame + 0.04f + footprint, edge),
        saturate((0.3f - footprint) * 5.0f)
    );
    return true;
}

// A pane's light at night (surface_wetness.y is its energy): about detail_parameters.y of the panes are lit,
// some storeys of a block more than others, each a warm, neutral or cool white at its own brightness.
float3 btbh_window_light(float2 cell)
{
    const float energy = surface_wetness.y;
    const float share = detail_parameters.y;
    if (energy <= 0.0f || share <= 0.0f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }
    const float storey = btbh_detail_hash(float2(floor(cell.x * 0.2f), cell.y) + float2(41.0f, 13.0f));
    if (btbh_detail_hash(cell + float2(7.0f, 29.0f)) >= share * (0.3f + 1.4f * storey))
    {
        return float3(0.0f, 0.0f, 0.0f);
    }
    const float tone = btbh_detail_hash(cell + float2(53.0f, 3.0f));
    const float3 color = tone < 0.55f
        ? float3(1.0f, 0.62f, 0.32f)
        : (tone < 0.85f ? float3(1.0f, 0.8f, 0.58f) : float3(0.72f, 0.84f, 1.0f));
    return color * energy * (0.5f + 0.5f * btbh_detail_hash(cell + float2(11.0f, 61.0f)));
}

// Procedural surface detail (SurfaceDetail): albedo grain, broad stains and a bump, all from world-space
// noise. Returns the bump as a surface gradient (Mikkelsen) built from screen-space derivatives, so it
// needs no tangent frame. The frame and the triplanar weights use the face normal of the position
// derivatives, not the interpolated vertex normal, which some imported production map instances
// carry nearly in the surface plane. detail_parameters is per draw, so every branch here is uniform
// and the derivatives stay valid; it runs before any discard. Glass adds its lit panes to `emission`.
float3 apply_surface_detail(FragmentInput input, inout float4 albedo, inout float3 emission)
{
    const uint pattern = (uint)(detail_parameters.w + 0.5f);
    if (pattern == 0u)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }
    if (pattern == 6u)
    {
        emission += albedo.rgb * surface_wetness.y * btbh_lamp_lens_gain;
        return float3(0.0f, 0.0f, 0.0f);
    }
    const float3 position = input.world_position * detail_parameters.z;
    // The texture LOD bias scales the footprint the same way, so the slabs, joints and grain fade for
    // the output's pixels rather than the render's while the temporal upscaler runs.
    const float footprint = max(length(ddx(position)), length(ddy(position)))
        * exp2(procedural_controls.w);
    const float3 position_dx = ddx(input.world_position);
    const float3 position_dy = ddy(input.world_position);
    const float3 face = cross(position_dx, position_dy);
    const float face_area = length(face);
    const float3 face_normal = face_area > 0.0f ? face / face_area : float3(0.0f, 1.0f, 0.0f);
    if (pattern == 5u)
    {
        // Flat panes take no bump: each a shade apart (blinds, curtains, what lies behind it) within darker
        // frames, and at night some lit from inside.
        albedo.rgb *= btbh_glass_diffuse;
        float2 cell;
        float inside;
        if (btbh_glass_pane(input.world_position, face_normal, footprint, cell, inside))
        {
            const float shade = btbh_detail_hash(cell + float2(17.0f, 5.0f)) - 0.5f;
            albedo.rgb *= max(
                (1.0f + detail_parameters.x * 2.0f * shade) * lerp(0.6f, 1.0f, inside),
                0.0f
            );
            emission += btbh_window_light(cell) * inside;
        }
        return float3(0.0f, 0.0f, 0.0f);
    }
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

    // Wet streets and walls (surface_wetness.x) show their grain up to 3.5 times as strongly, so the
    // light catches it; lawns keep theirs.
    const float wet_bump = pattern == 2u ? 1.0f : 1.0f + 2.5f * saturate(surface_wetness.x);
    const float bump = height * detail_parameters.y * wet_bump;
    // With the face normal the determinant is the pixel footprint area itself.
    const float3 r1 = cross(position_dy, face_normal);
    const float3 r2 = cross(face_normal, position_dx);
    if (face_area < 0.000000000001f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }
    return (ddx(bump) * r1 + ddy(bump) * r2) / face_area;
}

// Rain (surface_wetness.z, 0 dry ... 1 a downpour) on wet level ground (the ground and paving details): in each
// cell of two offset world grids a drop lands at its own spot and moment, then again elsewhere in the cell, and
// sends out a ring that widens and fades. Puddles ripple fully, the wet street between them at a third, so lamps
// and reflections waver in them. Returns the rings' surface gradient, as the detail's; rings thinner than about a
// pixel fade out, so far streets do not shimmer. surface_time drives it (it runs on while the game pauses, as the
// water does).
static const float btbh_rain_cells = 3.0f;
static const float btbh_rain_rate = 0.9f;
static const float btbh_rain_ring_radius = 0.45f;
static const float btbh_rain_ring_width = 0.12f;
static const float btbh_rain_tilt = 0.09f;

float3 btbh_rain_ripples(FragmentInput input)
{
    const float rain = saturate(surface_wetness.z) * saturate(surface_wetness.x * 2.0f);
    const uint pattern = (uint)(detail_parameters.w + 0.5f);
    if (rain <= 0.0f || (pattern != 1u && pattern != 4u))
    {
        return float3(0.0f, 0.0f, 0.0f);
    }
    const float3 position_dx = ddx(input.world_position);
    const float3 position_dy = ddy(input.world_position);
    const float3 face = cross(position_dx, position_dy);
    const float face_area = length(face);
    const float level = face_area > 0.0f ? abs(face.y) / face_area : 0.0f;
    const float footprint = max(length(position_dx.xz), length(position_dy.xz)) * btbh_rain_cells;
    const float visible = saturate((0.25f - footprint) * 8.0f) * saturate((level - 0.8f) * 10.0f);
    if (visible <= 0.0f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }
    const float puddle = smoothstep(
        0.42f,
        0.68f,
        btbh_detail_noise(input.world_position.xz * 0.35f + 3.1f)
    );
    float2 slope = float2(0.0f, 0.0f);
    [unroll]
    for (int layer = 0; layer < 2; ++layer)
    {
        const float2 grid = input.world_position.xz * btbh_rain_cells + float2(0.5f, 0.37f) * (float)layer;
        const float2 cell = floor(grid);
        const float cycle_time = surface_time * btbh_rain_rate
            + btbh_detail_hash(cell + float2(41.0f, 17.0f + 5.0f * (float)layer));
        const float cycle = floor(cycle_time);
        const float t = cycle_time - cycle;
        const float2 drop_cell = cell + float2(cycle * 17.0f, cycle * 31.0f);
        const float2 spot = float2(
            btbh_detail_hash(drop_cell + float2(13.0f + 7.0f * (float)layer, 3.0f)),
            btbh_detail_hash(drop_cell + float2(5.0f, 29.0f + 11.0f * (float)layer))
        ) * 0.6f + 0.2f;
        const float2 offset = grid - cell - spot;
        const float distance = length(offset);
        const float x = (distance - t * btbh_rain_ring_radius) / btbh_rain_ring_width;
        const float fade = (1.0f - t) * (1.0f - t);
        // The ring's height falls off as a Gaussian around its radius; this is its slope across it.
        const float across = -2.0f * x * exp(-x * x) * fade;
        slope += distance > 0.0001f ? offset / distance * across : float2(0.0f, 0.0f);
    }
    const float strength = rain * lerp(0.35f, 1.0f, puddle) * visible * btbh_rain_tilt / btbh_rain_ring_width;
    return float3(slope.x, 0.0f, slope.y) * strength;
}

// Wet ground (surface_wetness.x, the game's city at night): the ground and paving details turn glossy and a
// shade darker, in soft puddles a few render units across more than between them, so lights glint on the
// street. Only level faces get wet: the face normal from the position's derivatives, as the surface detail
// takes it (the interpolated normal does not say which way the ground faces). Returns the wet roughness.
float apply_surface_wetness(
    FragmentInput input,
    inout float4 albedo,
    float roughness
)
{
    const float wetness = saturate(surface_wetness.x);
    const uint pattern = (uint)(detail_parameters.w + 0.5f);
    if (wetness <= 0.0f || (pattern != 1u && pattern != 3u && pattern != 4u))
    {
        return roughness;
    }
    const float3 face = cross(ddx(input.world_position), ddy(input.world_position));
    const float face_area = length(face);
    const float level = face_area > 0.0f ? abs(face.y) / face_area : 0.0f;
    if (pattern == 3u)
    {
        // Walls darken a little and turn semi-gloss, the steep faces most.
        const float wall_wet = wetness * 0.5f * (1.0f - level);
        albedo.rgb *= lerp(1.0f, 0.85f, wall_wet);
        return lerp(roughness, 0.3f, wall_wet);
    }
    const float up = saturate((level - 0.6f) * 2.5f);
    const float puddle = smoothstep(
        0.42f,
        0.68f,
        btbh_detail_noise(input.world_position.xz * 0.35f + 3.1f)
    );
    const float wet = wetness * up * lerp(0.6f, 1.0f, puddle);
    albedo.rgb *= lerp(1.0f, 0.7f, wet);
    return lerp(roughness, lerp(0.22f, 0.06f, puddle), wet);
}

FragmentOutput shade_fragment(FragmentInput input)
{
    // Taken before any derivative, and discarded with the alpha cut-off below.
    const bool dug = btbh_dig_mask_hidden(
        dig_mask_texture,
        dig_mask_sampler,
        dig_mask_origin,
        dig_mask_scale,
        input.world_position
    );
#if BTBH_DIRECTIONAL_SHADOWS
    // The face's own normal, turned towards the camera (from the position's screen derivatives, taken
    // before anything can be discarded): the soft sun shadow takes its receiver plane from it.
    float3 shadow_face_normal = stable_normalize(
        cross(ddx(input.world_position), ddy(input.world_position)),
        stable_normalize(input.world_normal, float3(0.0f, 1.0f, 0.0f))
    );
    if (dot(shadow_face_normal, camera_position.xyz - input.world_position) < 0.0f)
    {
        shadow_face_normal = -shadow_face_normal;
    }
#endif
    const float lod_bias = procedural_controls.w;
    const float4 raw_albedo_sample = albedo_texture.SampleBias(
        albedo_sampler,
        input.texture_coordinate,
        lod_bias
    );
    float4 albedo_sample = float4(
        raw_albedo_sample.rgb * input.color.rgb,
        raw_albedo_sample.a * input.color.a
    );
    float3 emission = emission_texture.SampleBias(
        emission_sampler,
        input.texture_coordinate,
        lod_bias
    ).rgb * emission_color_energy.rgb * emission_color_energy.w;
    float3 mapped_normal = normal_texture.SampleBias(
        normal_sampler,
        input.texture_coordinate,
        lod_bias
    ).xyz * 2.0f - 1.0f;
    float3 orm = orm_texture.SampleBias(
        orm_sampler,
        input.texture_coordinate,
        lod_bias
    ).rgb;
    apply_surface(
        input,
        albedo_sample,
        emission,
        mapped_normal,
        orm
    );
    const float3 detail_gradient = apply_surface_detail(input, albedo_sample, emission)
        + btbh_rain_ripples(input);
    if (albedo_sample.a < alpha_cutoff || dug)
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
    const float roughness = apply_surface_wetness(
        input,
        albedo_sample,
        saturate(orm.g * material_factors.z)
    );
    const float metalness = saturate(orm.b * material_factors.w);
    // Glass (detail pattern 5) reflects like coated glass.
    const float3 f0 = max(
        lerp(float3(0.04f, 0.04f, 0.04f), albedo_sample.rgb, metalness),
        (uint)(detail_parameters.w + 0.5f) == 5u ? btbh_glass_reflectance : 0.0f
    );
    // unlit_enabled: 1, 2 or 3 (bits 0-1), bit 2 (4): takes the radiance cascades' light, bits 3-5: how it takes
    // part in the horizon-based ambient occlusion (ssao_normal_scale).
    if ((unlit_enabled & 3u) != 0u)
    {
#if BTBH_DIRECTIONAL_SHADOWS
        // unlit_enabled 2: unlit, but darkened in the primary directional light's shadow; 3: the same, through
        // the soft filter's plain lookup.
        if ((unlit_enabled & 3u) >= 2u)
        {
            const float shadow_visibility = primary_directional_visibility(
                input.world_position,
                normal,
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
            // With the albedo's own mip bias (FSR 3's), so the relief reads the mip the albedo shows.
            const float height_left = dot(albedo_texture.SampleBias(albedo_sampler, uv - float2(texel.x, 0.0f), lod_bias).rgb, luma);
            const float height_right = dot(albedo_texture.SampleBias(albedo_sampler, uv + float2(texel.x, 0.0f), lod_bias).rgb, luma);
            const float height_up = dot(albedo_texture.SampleBias(albedo_sampler, uv - float2(0.0f, texel.y), lod_bias).rgb, luma);
            const float height_down = dot(albedo_texture.SampleBias(albedo_sampler, uv + float2(0.0f, texel.y), lod_bias).rgb, luma);
            const float strength = -material_factors.x;
            lit_normal = normalize(float3(
                -(height_right - height_left) * strength,
                1.0f,
                -(height_down - height_up) * strength
            ));
        }
        // A dark scene (point_light_count.y, how dark): unlit surfaces keep that much less of
        // their colour; the spot light lights them (with its shadow, and the relief), and their
        // pixels brighter than point_light_count.z glow on (lava, lamps).
        if (point_light_count.y > 0.0f)
        {
            const float darkness = saturate(point_light_count.y);
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
            const float glow = 0.7f * saturate((luminance - point_light_count.z) * 4.0f);
            albedo_sample.rgb *= (1.0f - darkness) + darkness * glow + spot * spot_color_falloff.rgb;
        }
        albedo_sample.rgb += ease_point_light(
            albedo_sample.rgb,
            albedo_sample.rgb * evaluate_point_lights(input.world_position, lit_normal)
        );
#if BTBH_RC_MATERIAL_OUTPUT
        return rc_material_output(
            float4(albedo_sample.rgb + emission, albedo_sample.a),
            albedo_sample,
            emission,
            normal,
            1.0f,
            1.0f,
            metalness,
            f0,
            0.0f,
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

    const float total_energy =
        ambient_color_energy.w +
        directional_direction_energy.w +
        secondary_directional_direction_energy.w +
        spot_direction_energy.w +
        lamp_controls.x;
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
            f0,
            0.0f,
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
#if BTBH_DIRECTIONAL_SHADOWS
    // (The soft filter is looked up only where the sun reaches the surface at all: elsewhere it adds nothing.)
    float sun_visibility = 0.0f;
    [branch]
    if (directional_shadow_controls.z < 0.5f || dot(normal, directional_direction_energy.xyz) < 0.0f)
    {
        sun_visibility = primary_directional_visibility(
            input.world_position,
            shadow_normal,
            shadow_face_normal,
            false
        );
    }
#endif
    const float3 sun_lighting = evaluate_directional_light(
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
        sun_visibility * btbh_cloud_light(input.world_position)
#else
        btbh_cloud_light(input.world_position)
#endif
    );
    lighting += sun_lighting;
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
    const float3 spot_lighting = evaluate_spot_light(
        input.world_position,
        albedo_sample.rgb,
        normal,
        view,
        roughness,
        metalness,
        f0,
        spot_shadow_visibility
    );
    lighting += spot_lighting;
    const float3 lamp_lighting = evaluate_lamps(
        input.world_position,
        albedo_sample.rgb,
        normal,
        view,
        roughness,
        metalness,
        f0
    );
    lighting += lamp_lighting;
    lighting += ease_point_light(
        lighting,
        albedo_sample.rgb * (1.0f - metalness * 0.7f) * evaluate_point_lights(input.world_position, normal)
    );
#if BTBH_RC_MATERIAL_OUTPUT
    /* The cascades' light is scaled by this occlusion: the occluder's shadow keeps its share out of
       it too, or the resolve's max() fills the shadow back in. */
    /* The street lamps ride the emission target, which the resolve adds to the cascades' light: through the
       compose's max() a lamp on dark asphalt never outshone the cascades' wash. */
    /* The contact AO is the ambient light's shadow: it leaves the share of the rest of this light that the sun
       and the spot cast (its directness) alone, so sunlit walls keep their sun. */
    const float occludable_luma = dot(lighting - lamp_lighting, btbh_rc_luma);
    const float directness = occludable_luma > 0.000001f
        ? saturate(
            dot(sun_lighting * (1.0f - spot_occluder) + spot_lighting, btbh_rc_luma) / occludable_luma
        )
        : 0.0f;
    return rc_material_output(
        float4(lighting + emission, albedo_sample.a),
        albedo_sample,
        emission + lamp_lighting,
        normal,
        occlusion * (1.0f - spot_occluder),
        roughness,
        metalness,
        f0,
        directness,
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

// Screen-space refraction (BTBH_GPU_SCENE_SURFACE_REFRACT), drawn in a pass of its own after the transparent
// one. It samples no material: the albedo slot carries the scene as it was before the first refracting batch,
// and the normal slot the opaque scene's distance from the camera when surface_parameters1.w is 1.
static const uint btbh_surface_refract = 10u;
// How far red and blue part from green at full dispersion, as a share of the displacement.
static const float btbh_refract_dispersion_spread = 0.35f;

float btbh_refract_noise(float2 position)
{
    const float2 cell = floor(position);
    const float2 f = position - cell;
    const float2 u = f * f * (3.0f - 2.0f * f);
    return lerp(
        lerp(btbh_detail_hash(cell), btbh_detail_hash(cell + float2(1.0f, 0.0f)), u.x),
        lerp(btbh_detail_hash(cell + float2(0.0f, 1.0f)), btbh_detail_hash(cell + float2(1.0f, 1.0f)), u.x),
        u.y
    );
}

// The flow's height field in noise cells: a slow layer and a finer one drifting another way, both bent by a
// slower warp, which makes the eddies.
float btbh_refract_field(float2 position, float time)
{
    const float2 warp = float2(
        btbh_refract_noise(position * 0.5f + float2(time * 0.13f, 3.7f)),
        btbh_refract_noise(position * 0.5f + float2(5.2f, -time * 0.11f))
    ) - 0.5f;
    const float2 warped = position + warp * 1.6f;
    return btbh_refract_noise(warped + float2(time * 0.35f, -time * 0.2f)) +
        0.5f * btbh_refract_noise(warped * 2.3f + float2(-time * 0.6f, time * 0.45f));
}

// A world-space direction as a direction on screen (pixels: x right, y down), through the position's
// screen-space derivatives; zero when it points along the view.
float2 btbh_refract_screen_direction(float3 direction, float3 position_dx, float3 position_dy)
{
    const float2 projected = float2(
        dot(direction, position_dx) / max(dot(position_dx, position_dx), 1.0e-12f),
        dot(direction, position_dy) / max(dot(position_dy, position_dy), 1.0e-12f)
    );
    const float length_squared = dot(projected, projected);
    return length_squared > 1.0e-12f ? projected * rsqrt(length_squared) : float2(0.0f, 0.0f);
}

FragmentOutput refract_fragment(FragmentInput input)
{
    float2 size;
    albedo_texture.GetDimensions(size.x, size.y);
    const float2 uv = input.pixel_position.xy / size;
    const float pixel_scale = size.y / 1080.0f;
    const float weight = saturate(input.color.a);
    const float3 position_dx = ddx(input.world_position);
    const float3 position_dy = ddy(input.world_position);

    // The flow, fixed in the world or, with the view coupling, shifted along the view direction.
    const float3 view = stable_normalize(
        input.world_position - camera_position.xyz,
        float3(0.0f, -1.0f, 0.0f)
    );
    const float flow_scale = max(surface_parameters0.z, 0.0f);
    const float time = surface_time * surface_parameters0.w;
    const float2 world = input.world_position.xz +
        view.xz / max(abs(view.y), 0.25f) * saturate(surface_parameters1.x);
    const float2 position = world * flow_scale;
    const float delta = 0.35f;
    const float2 gradient = float2(
        btbh_refract_field(position + float2(delta, 0.0f), time) -
            btbh_refract_field(position - float2(delta, 0.0f), time),
        btbh_refract_field(position + float2(0.0f, delta), time) -
            btbh_refract_field(position - float2(0.0f, delta), time)
    ) * (0.25f / delta);
    // Calm and unstable stretches: a slow, coarse mask scales the bend between a quarter and all of it.
    const float unrest = 0.25f + 0.75f * smoothstep(
        0.3f,
        0.75f,
        btbh_refract_noise(position * 0.17f + float2(time * 0.03f, -time * 0.02f))
    );
    const float gradient_length = length(gradient);
    float2 offset = gradient_length > 0.0f
        ? btbh_refract_screen_direction(float3(gradient.x, 0.0f, gradient.y), position_dx, position_dy) *
            (gradient_length * unrest * surface_parameters0.x)
        : float2(0.0f, 0.0f);
    // The normal push: a ring whose normals lean outward pushes the scene radially (a shockwave).
    offset += btbh_refract_screen_direction(
        stable_normalize(input.world_normal, float3(0.0f, 1.0f, 0.0f)),
        position_dx,
        position_dy
    ) * surface_parameters1.z;
    offset *= pixel_scale * weight;

    // Where the displaced sample lies nearer than this surface (a unit in front of it) the scene shows undisplaced.
    if (surface_parameters1.w > 0.5f)
    {
        const float surface_distance = length(input.world_position - camera_position.xyz);
        const float sample_distance =
            normal_texture.SampleLevel(normal_sampler, uv + offset / size, 0.0f).r;
        if (sample_distance < surface_distance * 0.995f)
        {
            offset = float2(0.0f, 0.0f);
        }
    }

    const float dispersion = saturate(surface_parameters0.y) * btbh_refract_dispersion_spread;
    const float2 offset_uv = offset / size;
    float3 color = float3(
        albedo_texture.SampleLevel(albedo_sampler, uv + offset_uv * (1.0f + dispersion), 0.0f).r,
        albedo_texture.SampleLevel(albedo_sampler, uv + offset_uv, 0.0f).g,
        albedo_texture.SampleLevel(albedo_sampler, uv + offset_uv * (1.0f - dispersion), 0.0f).b
    );
    // Thin chromatic lines where the bend changes fastest from one pixel to the next, tinted by its direction.
    const float steepness = length(ddx(offset)) + length(ddy(offset));
    const float line_amount = smoothstep(0.6f, 2.0f, steepness) * max(surface_parameters1.y, 0.0f);
    if (line_amount > 0.0f)
    {
        const float turn = atan2(offset.y, offset.x) * 0.15915494f;
        const float3 hue = 0.5f + 0.5f * cos(6.2831853f * (turn + float3(0.0f, 0.33f, 0.67f)));
        const float luminance = dot(color, float3(0.2126f, 0.7152f, 0.0722f));
        color += hue * line_amount * (luminance + 0.15f);
    }
    color *= input.color.rgb;

    FragmentOutput output;
    output.color = float4(color, 1.0f);
    // The transparent pass's second target is the reactive mask: the more the scene moves here, the less
    // the temporal upscaler should keep of its history (main scales it by the frame's reactive scale).
    output.ambient = float4(saturate(length(offset) / 6.0f), 0.0f, 0.0f, 0.0f);
    output.normal = float4(0.5f, 0.5f, 1.0f, 1.0f);
    output.ambient_specular = float4(0.0f, 0.0f, 0.0f, 1.0f);
    output.emission = float4(0.0f, 0.0f, 0.0f, 0.0f);
    output.velocity = float4(0.0f, 0.0f, 0.0f, 0.0f);
    return output;
}

FragmentOutput main(FragmentInput input)
{
    const bool refracts = surface_variant == btbh_surface_refract;
    FragmentOutput output;
    if (refracts)
    {
        output = refract_fragment(input);
    }
    else
    {
        output = shade_fragment(input);
    }
    output.velocity = float4(
        btbh_temporal_velocity(input.motion_clip_xy, input.motion_clip_w),
        0.0f,
        0.0f
    );
    if (procedural_controls.z > 0.0f)
    {
        // The transparent pass's second target is the reactive mask, not the ambient light.
        output.ambient = float4(
            refracts
                ? saturate(output.ambient.x * procedural_controls.z)
                : btbh_temporal_reactive(output.color, procedural_controls.z),
            0.0f,
            0.0f,
            0.0f
        );
    }
    return output;
}
