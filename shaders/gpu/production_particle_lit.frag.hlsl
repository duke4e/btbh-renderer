// RFC 0013: the lit particle (BTBH_GPU_SCENE_SURFACE_LIT_PARTICLE), a see-through surface lit per pixel for smoke,
// dust and steam, drawn in the transparent pass after production_model.vert. Diffuse only: per light the wrap
// saturate((n.l + w) / (1 + w)) and, for the sun and the point lights, the transmission (light from behind, through
// it) x * exp(-thickness * z) * 4 pi * HG(y), all times the albedo, each light at the scale a lit surface takes it
// (1 / pi for the sun, the fill, the spot and the lamps; none for the point lights). Then fog at its own depth, made
// up so that the HDR composite's fog at the opaque depth behind it leaves the right colour.
//
// Three variants, as production_model.frag: this file (the sun's map, the fill's, the spot's and the omni light's
// cube), production_particle_lit_2d_shadows (no cube) and production_particle_lit_unshadowed (no maps). Its textures
// put the shadow maps last, so that each variant's are the next one's and some more.
// src/gpu/btbh_gpu_particle.h holds C copies of the wrap, the phase and the fog, for the native tests.

#ifndef BTBH_DIRECTIONAL_SHADOWS
#define BTBH_DIRECTIONAL_SHADOWS 1
#endif
#ifndef BTBH_OMNI_SHADOWS
#define BTBH_OMNI_SHADOWS 1
#endif

// BTBH_GPU_SCENE_MAX_LAMPS
#define BTBH_MAX_LAMPS 40

#include "production_surface.hlsli"
#include "production_temporal.hlsli"

Texture2D<float4> albedo_texture : register(t0, space2);
SamplerState albedo_sampler : register(s0, space2);
// rg the tangent-space normal, b the thickness (the normal fallback, flat and 1 thick, without a normal_texture).
Texture2D<float4> normal_texture : register(t1, space2);
SamplerState normal_sampler : register(s1, space2);
// The opaque scene's distance from the camera per pixel (production_scene_distance; 1e6 over the background), or a
// fallback never read when the frame couldn't draw it (particle_controls.z). Read at the pixel through a nearest
// sampler (a texture only loaded would be reflected as a storage texture).
Texture2D<float> particle_scene_distance : register(t2, space2);
SamplerState particle_scene_distance_sampler : register(s2, space2);
Texture2D<float4> emission_texture : register(t3, space2);
SamplerState emission_sampler : register(s3, space2);
// The radiance cascades' light this frame: their four-quadrant atlas (production_hrc_resolve.comp.hlsl's
// quadrant_texture), or a fallback never read when they are off (particle_cascade_origin.w 0).
Texture2D<float4> particle_cascade_quadrants : register(t4, space2);
SamplerState particle_cascade_sampler : register(s4, space2);
#if BTBH_DIRECTIONAL_SHADOWS
// The sun's map through the comparison sampler, the fill's and the spot's.
Texture2D<float> directional_shadow_compare_texture : register(t5, space2);
SamplerComparisonState directional_shadow_compare_sampler : register(s5, space2);
Texture2D<float> secondary_directional_shadow_texture : register(t6, space2);
SamplerState secondary_directional_shadow_sampler : register(s6, space2);
Texture2D<float> spot_shadow_texture : register(t7, space2);
SamplerState spot_shadow_sampler : register(s7, space2);
#if BTBH_OMNI_SHADOWS
TextureCube<float> omni_shadow_texture : register(t8, space2);
SamplerState omni_shadow_sampler : register(s8, space2);
#endif
#endif

// The scene fragment shaders' material block (production_model.frag.hlsl's MaterialPBR, BTBHGPUSceneFragmentUniforms),
// declared as there: the core pushes the same bytes.
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
    // x the normal's tilt (normal_scale; 0 without a normal_texture)
    float4 material_factors;
    float4 emission_color_energy;
    // bit 8 (256): BTBH_GPU_SCENE_BATCH_RECEIVES_NO_SPOT_SHADOW
    uint unlit_enabled;
    float alpha_cutoff;
    uint surface_variant;
    float surface_time;
    // x the soft fade, y and z the near fade, w the wrap
    float4 surface_parameters0;
    // x the transmission, y the asymmetry, z the optical depth at thickness 1, w 0
    float4 surface_parameters1;
    float4 irradiance_sh[9];
    float4 ground_bottom_energy;
    float4 ground_horizon_curve;
    float4 sky_top_energy;
    float4 sky_horizon_curve;
    float4 sun_color_energy;
    float4 sun_direction_size;
    // y 1 with image-based lighting, z the reactive mask's scale, w the texture LOD bias
    float4 procedural_controls;
    float4 detail_parameters;
    float4 surface_wetness;
    float4 cloud_controls;
    float4 cloud_shape;
    float4 lamp_controls;
    float4 lamp_position_reach[BTBH_MAX_LAMPS];
    float4 lamp_color_energy[BTBH_MAX_LAMPS];
    float4 point_light_count;
    float4 point_lights[64];
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

// What every lit particle of a pass shares (BTBHGPUSceneParticleFrameUniforms), pushed once a pass.
cbuffer ParticleFrame : register(b1, space3)
{
    // The frame's inverse view-projection (the HDR composite's and the distance pass's).
    float4x4 particle_inverse_view_projection;
    // x, y 1 / the render size; z 1 when particle_scene_distance holds the scene's distance; w 1 when the HDR
    // composite fogs this frame
    float4 particle_controls;
    // The HDR composite's fog (BTBHGPUSceneCompositeUniforms): rgb its colour, a its density
    float4 particle_fog_color_density;
    // x its start, y its greatest opacity
    float4 particle_fog_shape;
    // the world's x and z rectangle the edge fog closes over
    float4 particle_fog_edge_bounds;
    // x its fade, y 1 when on, z the view depth that stays clear of it
    float4 particle_fog_edge;
    // The radiance cascades' field this frame: x, y its origin's world x and z, z 1 / its cell, w 2 * their indirect
    // intensity (0: no cascades' light this frame)
    float4 particle_cascade_origin;
    // x, y its width and height in cells, z 1 with the sources' tops (read half a texel back, as the resolve), w 0
    float4 particle_cascade_field;
};

struct FragmentInput
{
    float4 pixel_position : SV_Position;
    [[vk::location(0)]] float4 color : TEXCOORD0;
    [[vk::location(1)]] float2 texture_coordinate : TEXCOORD1;
    [[vk::location(2)]] float3 world_position : TEXCOORD2;
    [[vk::location(3)]] float3 world_normal : TEXCOORD3;
    [[vk::location(4)]] float3 world_tangent : TEXCOORD4;
    [[vk::location(6)]] float3 world_bitangent : TEXCOORD6;
};

struct FragmentOutput
{
    float4 color : SV_Target0;
    // The transparent pass's second target while the temporal upscaler runs: the reactive mask.
    float4 reactive : SV_Target1;
};

static const float inverse_pi = 0.3183098861837907f;

float3 particle_normalize(float3 value, float3 fallback)
{
    const float length_squared = dot(value, value);
    return length_squared > 0.00000001f
        ? value * rsqrt(length_squared)
        : fallback;
}

// The wrap, surface_parameters0.w: 0 Lambert, 1 lit all the way round to the back.
float particle_wrap(float3 normal, float3 light)
{
    const float wrap = max(surface_parameters0.w, 0.0f);
    return saturate((dot(normal, light) + wrap) / (1.0f + wrap));
}

// Henyey-Greenstein's phase times 4 pi (1 at every angle for g = 0), at the angle between the light's way and the
// eye's (cos_theta = dot(-light, view)); g is surface_parameters1.y, clamped.
float particle_phase(float cos_theta)
{
    const float g = clamp(surface_parameters1.y, -0.99f, 0.99f);
    const float g2 = g * g;
    const float d = max(1.0f + g2 - 2.0f * g * cos_theta, 0.0001f);
    return (1.0f - g2) / (d * sqrt(d));
}

float3 particle_irradiance_sh(float3 direction)
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

// production_model.frag.hlsl's btbh_cloud_light: the share of the sun a cloud layer's shadow leaves.
float particle_cloud_light(float3 world_position)
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

// production_model.frag.hlsl's ease_point_light: the point lights' light added to a surface already this bright,
// eased under their ceiling (point_light_count.w).
float3 particle_ease_point_light(float3 base, float3 added)
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

#if BTBH_DIRECTIONAL_SHADOWS
// Where a point falls in a map: false outside it.
bool particle_map_position(float4x4 view_projection, float3 world_position, out float3 projected)
{
    const float4 clip = mul(view_projection, float4(world_position, 1.0f));
    projected = float3(0.0f, 0.0f, 0.0f);
    if (clip.w <= 0.0f)
    {
        return false;
    }
    projected = clip.xyz / clip.w;
    return !(
        projected.x < -1.0f || projected.x > 1.0f ||
        projected.y < -1.0f || projected.y > 1.0f ||
        projected.z < 0.0f || projected.z > 1.0f
    );
}

// The sun's shadow: four comparison taps half a texel round the point (a tent over 3 x 3 texels), whatever the
// sun's filter, with a constant bias of a texel and a half (a billboard's plane isn't the smoke's surface).
float particle_sun_visibility(float3 world_position)
{
    float3 projected;
    if (directional_shadow_controls.x < 0.5f ||
        !particle_map_position(directional_shadow_view_projection, world_position, projected))
    {
        return 1.0f;
    }
    const float2 uv = float2(projected.x * 0.5f + 0.5f, -projected.y * 0.5f + 0.5f);
    uint width;
    uint height;
    directional_shadow_compare_texture.GetDimensions(width, height);
    const float2 texel = 1.0f / float2(width, height);
    const float2 uv_per_world = 0.5f * float2(
        length(directional_shadow_view_projection[0].xyz),
        length(directional_shadow_view_projection[1].xyz)
    );
    const float texel_world = max(texel.x / uv_per_world.x, texel.y / uv_per_world.y);
    const float reference =
        projected.z - 1.5f * texel_world * length(directional_shadow_view_projection[2].xyz);
    float lit = 0.0f;
    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        const float2 tap = float2(i & 1 ? 0.5f : -0.5f, i & 2 ? 0.5f : -0.5f) * texel;
        lit += directional_shadow_compare_texture.SampleCmpLevelZero(
            directional_shadow_compare_sampler,
            uv + tap,
            reference
        );
    }
    return lit * 0.25f;
}

// A map's shadow through one gather, weighted bilinearly (2 x 2 texels round the point). Gather's components:
// w (0, 0), z (1, 0), x (0, 1), y (1, 1).
float particle_gather_visibility(
    Texture2D<float> shadow_map,
    SamplerState shadow_sampler,
    float4x4 view_projection,
    float3 world_position,
    float bias
)
{
    float3 projected;
    if (!particle_map_position(view_projection, world_position, projected))
    {
        return 1.0f;
    }
    const float2 uv = float2(projected.x * 0.5f + 0.5f, -projected.y * 0.5f + 0.5f);
    uint width;
    uint height;
    shadow_map.GetDimensions(width, height);
    const float2 size = float2(width, height);
    const float2 texel = uv * size - 0.5f;
    const float2 corner = floor(texel);
    const float2 f = texel - corner;
    const float4 lit = step(projected.z - bias, shadow_map.Gather(shadow_sampler, (corner + 1.0f) / size));
    return lerp(lerp(lit.w, lit.z, f.x), lerp(lit.x, lit.y, f.x), f.y);
}

float particle_fill_visibility(float3 world_position)
{
    if (secondary_directional_shadow_controls.x < 0.5f)
    {
        return 1.0f;
    }
    return particle_gather_visibility(
        secondary_directional_shadow_texture,
        secondary_directional_shadow_sampler,
        secondary_directional_shadow_view_projection,
        world_position,
        0.0025f
    );
}

float particle_omni_projected_depth(float major_distance, float near_plane, float far_plane)
{
    const float safe_near = max(near_plane, 0.0001f);
    const float safe_far = max(far_plane, safe_near + 0.0001f);
    const float depth_scale = safe_far / (safe_far - safe_near);
    return depth_scale - safe_near * depth_scale / max(major_distance, safe_near);
}

// The spot's shadow (one gather), or the omni light's (one cube sample); none with
// BTBH_GPU_SCENE_BATCH_RECEIVES_NO_SPOT_SHADOW.
float particle_spot_visibility(float3 world_position)
{
    if (spot_shadow_controls.x < 0.5f || (unlit_enabled & 256u) != 0u)
    {
        return 1.0f;
    }
#if BTBH_OMNI_SHADOWS
    if (spot_shadow_controls.z > 0.5f)
    {
        const float3 from_light = world_position - omni_shadow_position_range.xyz;
        const float major_distance = max(max(abs(from_light.x), abs(from_light.y)), abs(from_light.z));
        const float near_plane = spot_shadow_controls.w;
        const float far_plane = omni_shadow_position_range.w;
        if (major_distance <= near_plane || major_distance >= far_plane)
        {
            return 1.0f;
        }
        const float reference = particle_omni_projected_depth(
            max(major_distance - 0.125f, near_plane),
            near_plane,
            far_plane
        );
        const float stored = omni_shadow_texture.SampleLevel(
            omni_shadow_sampler,
            particle_normalize(from_light, float3(0.0f, 0.0f, 1.0f)),
            0.0f
        );
        return reference <= stored ? 1.0f : 0.0f;
    }
#endif
    return particle_gather_visibility(
        spot_shadow_texture,
        spot_shadow_sampler,
        spot_shadow_view_projection,
        world_position,
        0.0006f
    );
}
#endif

// The spot's light at the point before its shadow and the albedo: its range and cone as evaluate_spot_light's, the
// wrap, 1 / pi.
float3 particle_spot_light(float3 world_position, float3 normal)
{
    const float3 delta = spot_position_range.xyz - world_position;
    const float light_distance = length(delta);
    if (spot_direction_energy.w <= 0.0f || spot_position_range.w <= 0.0f || light_distance <= 0.000001f)
    {
        return 0.0f;
    }
    const float3 light = delta / light_distance;
    const float range_attenuation = pow(
        1.0f - saturate(light_distance / spot_position_range.w),
        spot_color_falloff.w
    );
    const float epsilon = max(spot_cutoffs.x - spot_cutoffs.y, 0.0001f);
    const float theta = dot(light, -particle_normalize(spot_direction_energy.xyz, float3(0.0f, -1.0f, 0.0f)));
    const float angular_attenuation = smoothstep(0.0f, 1.0f, (theta - spot_cutoffs.y) / epsilon);
    return spot_color_falloff.rgb *
        (spot_direction_energy.w * range_attenuation * angular_attenuation * particle_wrap(normal, light) *
            inverse_pi);
}

// The street lamps' light before the albedo: each with evaluate_lamps' falloff, the wrap, 1 / pi; a lamp out of
// reach costs its distance test alone.
float3 particle_lamps(float3 world_position, float3 normal)
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
        const float energy = lamp_color_energy[index].w;
        if (distance_squared >= reach_squared || energy <= 0.0f)
        {
            continue;
        }
        const float fade = 1.0f - distance_squared / reach_squared;
        const float falloff = fade * fade * core_squared / (distance_squared + core_squared);
        const float3 light = particle_normalize(delta, float3(0.0f, 1.0f, 0.0f));
        lighting += lamp_color_energy[index].rgb * (energy * falloff * particle_wrap(normal, light));
    }
    return lighting * inverse_pi;
}

// The point lights' light before the albedo: each with evaluate_point_lights' falloff squared, the wrap and the
// transmission, no 1 / pi; a light out of reach costs its distance test alone.
float3 particle_point_lights(float3 world_position, float3 normal, float3 view, float transmission)
{
    const uint count = min((uint)point_light_count.x, 32u);
    float3 lighting = 0.0f;
    [loop]
    for (uint index = 0u; index < count; ++index)
    {
        const float4 position_radius = point_lights[index * 2u];
        const float3 to_light = position_radius.xyz - world_position;
        const float light_distance = length(to_light);
        if (light_distance >= position_radius.w)
        {
            continue;
        }
        const float4 color_energy = point_lights[index * 2u + 1u];
        const float falloff = 1.0f - light_distance / position_radius.w;
        const float3 light = to_light / max(light_distance, 0.0001f);
        const float share = particle_wrap(normal, light) + transmission * particle_phase(dot(-light, view));
        lighting += color_energy.rgb * (color_energy.w * falloff * falloff * share);
    }
    return lighting;
}

// The radiance cascades' light at the point's x and z before the albedo, from the four directions the resolve's
// receivers take it from ((+-1, 0.65, 0) and (0, 0.65, +-1)), each through the wrap; 0 outside their field.
static const float3 particle_cascade_directions[4] = {
    float3(0.83844f, 0.54499f, 0.0f),
    float3(0.0f, 0.54499f, 0.83844f),
    float3(-0.83844f, 0.54499f, 0.0f),
    float3(0.0f, 0.54499f, -0.83844f)
};

float3 particle_cascades(float3 world_position, float3 normal)
{
    const float2 dimensions = particle_cascade_field.xy;
    const float2 field_cell = (world_position.xz - particle_cascade_origin.xy) * particle_cascade_origin.z;
    if (any(field_cell < 0.0f) || any(field_cell >= dimensions))
    {
        return 0.0f;
    }
    const float2 light_cell = particle_cascade_field.z > 0.5f ? field_cell - 0.5f : field_cell;
    const float2 field_uv = saturate((light_cell + 0.5f) / dimensions);
    float3 bounce = 0.0f;
    [unroll]
    for (uint rotation = 0u; rotation < 4u; ++rotation)
    {
        const float3 quadrant = particle_cascade_quadrants.SampleLevel(
            particle_cascade_sampler,
            float2(field_uv.x, ((float)rotation + field_uv.y) * 0.25f),
            0.0f
        ).rgb;
        bounce += quadrant * particle_wrap(normal, particle_cascade_directions[rotation]);
    }
    return bounce * particle_cascade_origin.w;
}

// The HDR composite's fog (production_hdr_composite.frag.hlsl's fog_amount) at a view depth and a world x and z.
float particle_fog_amount(float view_depth, float2 position)
{
    const float optical = max(view_depth - particle_fog_shape.x, 0.0f) * particle_fog_color_density.a;
    float amount = min(1.0f - exp(-optical * optical), particle_fog_shape.y);
    if (particle_fog_edge.y > 0.5f)
    {
        const float inside = min(
            min(position.x - particle_fog_edge_bounds.x, particle_fog_edge_bounds.z - position.x),
            min(position.y - particle_fog_edge_bounds.y, particle_fog_edge_bounds.w - position.y)
        );
        const float edge = 1.0f - saturate(inside / particle_fog_edge.x);
        const float depth_gate = saturate((view_depth - particle_fog_edge.z) / particle_fog_edge.x);
        amount = max(
            amount,
            edge * edge * (3.0f - 2.0f * edge) * depth_gate * depth_gate * (3.0f - 2.0f * depth_gate)
        );
    }
    return amount;
}

FragmentOutput main(FragmentInput input)
{
    // Every texture with a derivative is read before anything can be discarded.
    const float lod_bias = procedural_controls.w;
    const float4 albedo_sample = albedo_texture.SampleBias(albedo_sampler, input.texture_coordinate, lod_bias);
    const float4 normal_sample = normal_texture.SampleBias(normal_sampler, input.texture_coordinate, lod_bias);
    float3 emission = float3(0.0f, 0.0f, 0.0f);
    [branch]
    if (emission_color_energy.w > 0.0f)
    {
        emission = emission_texture.SampleBias(emission_sampler, input.texture_coordinate, lod_bias).rgb *
            emission_color_energy.rgb * emission_color_energy.w;
    }
    const float3 albedo = albedo_sample.rgb * input.color.rgb;
    float alpha = albedo_sample.a * input.color.a;

    // Where the fragment lies on its pixel's ray, as the depth buffer would hold it (the world as drawn, bent by the
    // curvature): its view depth as the composite measures it, and its distance from the camera as the distance pass
    // measures the scene's.
    const float2 pixel_uv = input.pixel_position.xy * particle_controls.xy;
    const float4 homogeneous = mul(
        particle_inverse_view_projection,
        float4(pixel_uv.x * 2.0f - 1.0f, 1.0f - pixel_uv.y * 2.0f, input.pixel_position.z, 1.0f)
    );
    const float3 drawn_position = homogeneous.xyz / homogeneous.w;
    const float view_depth = abs(1.0f / homogeneous.w);
    const float camera_distance = length(drawn_position - camera_position.xyz);
    const bool has_distance = particle_controls.z > 0.5f;
    const float scene_distance = has_distance
        ? particle_scene_distance.SampleLevel(particle_scene_distance_sampler, pixel_uv, 0.0f)
        : 1.0e6f;
    // The soft fade where it meets the scene behind it, and the near fade at the camera.
    if (surface_parameters0.x > 0.0f && has_distance)
    {
        alpha *= saturate((scene_distance - camera_distance) / surface_parameters0.x);
    }
    if (surface_parameters0.z > 0.0f)
    {
        alpha *= saturate((camera_distance - surface_parameters0.y) / surface_parameters0.z);
    }
    if (alpha <= 0.0f || alpha < alpha_cutoff)
    {
        discard;
    }

    // The normal: the tilt in rg (OpenGL +Y, z rebuilt) in the vertex's frame, whose normal is turned toward the
    // side seen; the thickness in b.
    const float2 tilt = (normal_sample.rg * 2.0f - 1.0f) * max(material_factors.x, 0.0f);
    const float rise = sqrt(1.0f - saturate(dot(tilt, tilt)));
    const float3 view = particle_normalize(camera_position.xyz - input.world_position, float3(0.0f, 1.0f, 0.0f));
    float3 frame_normal = particle_normalize(input.world_normal, float3(0.0f, 0.0f, 1.0f));
    if (dot(frame_normal, view) < 0.0f)
    {
        frame_normal = -frame_normal;
    }
    const float3 normal = particle_normalize(
        input.world_tangent * tilt.x + input.world_bitangent * tilt.y + frame_normal * rise,
        frame_normal
    );
    const float transmission =
        max(surface_parameters1.x, 0.0f) * exp(-normal_sample.b * max(surface_parameters1.z, 0.0f));

    // The sky's ambient, the sun, the fill, the spot and the lamps, before the albedo.
    float3 lighting = procedural_controls.y > 0.5f
        ? particle_irradiance_sh(normal) * ambient_color_energy.w
        : ambient_color_energy.rgb * ambient_color_energy.w;
    if (directional_direction_energy.w > 0.0f)
    {
        const float3 light = particle_normalize(-directional_direction_energy.xyz, float3(0.0f, 1.0f, 0.0f));
        const float share = particle_wrap(normal, light) + transmission * particle_phase(dot(-light, view));
        [branch]
        if (share > 0.0f)
        {
            float visibility = particle_cloud_light(input.world_position);
#if BTBH_DIRECTIONAL_SHADOWS
            visibility *= particle_sun_visibility(input.world_position);
#endif
            lighting += directional_color.rgb * (directional_direction_energy.w * visibility * share * inverse_pi);
        }
    }
    if (secondary_directional_direction_energy.w > 0.0f)
    {
        const float3 light =
            particle_normalize(-secondary_directional_direction_energy.xyz, float3(0.0f, 1.0f, 0.0f));
        const float share = particle_wrap(normal, light);
        [branch]
        if (share > 0.0f)
        {
#if BTBH_DIRECTIONAL_SHADOWS
            const float visibility = particle_fill_visibility(input.world_position);
#else
            const float visibility = 1.0f;
#endif
            lighting += secondary_directional_color.rgb *
                (secondary_directional_direction_energy.w * visibility * share * inverse_pi);
        }
    }
    {
        const float3 spot = particle_spot_light(input.world_position, normal);
        [branch]
        if (any(spot > 0.0f))
        {
#if BTBH_DIRECTIONAL_SHADOWS
            lighting += spot * particle_spot_visibility(input.world_position);
#else
            lighting += spot;
#endif
        }
    }
    lighting += particle_lamps(input.world_position, normal);
    float3 color = albedo * lighting;
    // The point lights eased under their ceiling, as on a lit surface; then the cascades' light.
    color += particle_ease_point_light(
        color,
        albedo * particle_point_lights(input.world_position, normal, view, transmission)
    );
    [branch]
    if (particle_cascade_origin.w > 0.0f)
    {
        color += albedo * particle_cascades(input.world_position, normal);
    }
    color += emission;

    // Fog at its own depth (own), made up for the composite's fog at the opaque depth behind it (behind; 0 over the
    // background, which the composite leaves): the composite's lerp(pixel, fog, behind) then leaves the puff fogged
    // at its own depth over the background fogged at its own, for every layer of puffs over the pixel.
    [branch]
    if (particle_controls.w > 0.5f && has_distance)
    {
        const float own = particle_fog_amount(view_depth, drawn_position.xz);
        float behind = 0.0f;
        if (scene_distance < 5.0e5f)
        {
            const float ratio = scene_distance / max(camera_distance, 0.000001f);
            behind = particle_fog_amount(
                view_depth * ratio,
                camera_position.xz + (drawn_position.xz - camera_position.xz) * ratio
            );
        }
        color = (color * (1.0f - own) + particle_fog_color_density.rgb * (own - behind)) / max(1.0f - behind, 0.0001f);
    }

    FragmentOutput output;
    output.color = float4(color, alpha);
    output.reactive = procedural_controls.z > 0.0f
        ? float4(btbh_temporal_reactive(output.color, procedural_controls.z), 0.0f, 0.0f, 0.0f)
        : float4(0.0f, 0.0f, 0.0f, 0.0f);
    return output;
}
