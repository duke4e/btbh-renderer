Texture2D<float4> scene_texture : register(t0, space2);
SamplerState scene_sampler : register(s0, space2);
Texture2D<float> depth_texture : register(t1, space2);
SamplerState depth_sampler : register(s1, space2);
Texture2D<float4> ambient_texture : register(t2, space2);
SamplerState ambient_sampler : register(s2, space2);
Texture2D<float4> normal_texture : register(t3, space2);
SamplerState normal_sampler : register(s3, space2);
Texture2D<float4> ambient_specular_texture : register(t4, space2);
SamplerState ambient_specular_sampler : register(s4, space2);
Texture2D<float> ssao_texture : register(t5, space2);
SamplerState ssao_sampler : register(s5, space2);
Texture2D<float4> emission_texture : register(t6, space2);
SamplerState emission_sampler : register(s6, space2);

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
    // Screen-space reflections (screen_space_reflection): ssr_options x 1 when the specular target carries the
    // surface's F0 (the cascades' material targets; 0.04 stands in otherwise), y reserved; view_projection
    // projects the reflected rays; ssr_controls x the intensity (0 off), y the roughest surface that reflects,
    // z the farthest a ray looks and w the thickness a hit may lie behind a surface (render units)
    float2 ssr_options;
    float4x4 view_projection;
    float4 ssr_controls;
    // Ambient occlusion: xy the SSAO's radius and strength (production_ssao.frag.hlsl); with the cascades z the
    // share of the indirect light it occludes, w the share of that direct light keeps
    float4 ao_controls;
};

struct FragmentInput
{
    float4 position : SV_Position;
    float2 texture_coordinate : TEXCOORD0;
};

float sample_depth_at(int2 pixel)
{
    const float2 uv =
        (float2(pixel) + 0.5f) * inverse_resolution;
    return depth_texture.SampleLevel(depth_sampler, uv, 0.0f);
}

float4 sample_normal_at(int2 pixel)
{
    const float2 uv =
        (float2(pixel) + 0.5f) * inverse_resolution;
    return normal_texture.SampleLevel(normal_sampler, uv, 0.0f);
}

float sample_ssao_at(int2 pixel, float2 resolution)
{
    const float2 uv =
        (float2(pixel) + 0.5f) / resolution;
    return ssao_texture.SampleLevel(ssao_sampler, uv, 0.0f);
}

float3 world_position(int2 pixel, float depth)
{
    const float2 uv =
        (float2(pixel) + 0.5f) * inverse_resolution;
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

// From `position` (at `pixel`) toward the camera: the pixel's point on the near plane is the camera to within
// a quarter of a render unit. (A point at depth 0.9999, the old far reference, lay only a hundred units out while
// the near plane was 0.01: past it the direction turned around and reflections went into the ground.)
float3 view_direction(int2 pixel, float3 position)
{
    return normalize(world_position(pixel, 0.0f) - position);
}

float view_depth(int2 pixel, float depth)
{
    const float2 uv =
        (float2(pixel) + 0.5f) * inverse_resolution;
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
    return abs(1.0f / homogeneous_world.w);
}

int2 representative_full_pixel(
    int2 half_pixel,
    int2 full_resolution,
    out float selected_depth
)
{
    const int2 offsets[4] = {
        int2(0, 0),
        int2(1, 0),
        int2(0, 1),
        int2(1, 1)
    };
    const int2 base_pixel = half_pixel * 2;
    int2 selected_pixel = clamp(
        base_pixel,
        int2(0, 0),
        full_resolution - 1
    );
    selected_depth = sample_depth_at(selected_pixel);
    const bool use_maximum =
        ((half_pixel.x + half_pixel.y) & 1) == 0;
    [unroll]
    for (int index = 1; index < 4; ++index)
    {
        const int2 candidate_pixel = clamp(
            base_pixel + offsets[index],
            int2(0, 0),
            full_resolution - 1
        );
        const float candidate_depth = sample_depth_at(candidate_pixel);
        if ((use_maximum && candidate_depth > selected_depth) ||
            (!use_maximum && candidate_depth < selected_depth))
        {
            selected_pixel = candidate_pixel;
            selected_depth = candidate_depth;
        }
    }
    return selected_pixel;
}

float depth_aware_upsample(float2 uv)
{
    if (ssao_enabled == 0u)
    {
        return 1.0f;
    }
    const int2 full_resolution = int2(
        1.0f / inverse_resolution
    );
    const int2 center_pixel = clamp(
        int2(uv * float2(full_resolution)),
        int2(0, 0),
        full_resolution - 1
    );
    const float center_depth = sample_depth_at(center_pixel);
    if (center_depth >= 0.99999f)
    {
        return 1.0f;
    }
    uint ssao_width;
    uint ssao_height;
    ssao_texture.GetDimensions(ssao_width, ssao_height);
    const float2 ssao_resolution =
        float2(float(ssao_width), float(ssao_height));
    const float3 center_position = world_position(
        center_pixel,
        center_depth
    );
    const float3 center_normal = normalize(
        sample_normal_at(center_pixel).xyz *
            2.0f - 1.0f
    );
    const float3 view = view_direction(center_pixel, center_position);
    const float normal_dot_view =
        max(dot(center_normal, view), 0.0f);
    const float center_view_depth =
        view_depth(center_pixel, center_depth);

    const float2 low_pixel =
        uv * ssao_resolution - 0.5f;
    const int2 base = int2(floor(low_pixel));
    const float2 fraction = frac(low_pixel);
    const int2 coordinates[4] = {
        clamp(base, int2(0, 0), int2(ssao_width, ssao_height) - 1),
        clamp(base + int2(1, 0), int2(0, 0), int2(ssao_width, ssao_height) - 1),
        clamp(base + int2(0, 1), int2(0, 0), int2(ssao_width, ssao_height) - 1),
        clamp(base + int2(1, 1), int2(0, 0), int2(ssao_width, ssao_height) - 1)
    };
    float weights[4] = {
        (1.0f - fraction.x) * (1.0f - fraction.y),
        fraction.x * (1.0f - fraction.y),
        (1.0f - fraction.x) * fraction.y,
        fraction.x * fraction.y
    };
    const float depth_edge_tolerance = 0.05f;
    const float depth_sharpness =
        max(normal_dot_view, 0.05f) / depth_edge_tolerance;
    float result = 0.0f;
    float weight_sum = 0.0f;
    [unroll]
    for (int index = 0; index < 4; ++index)
    {
        float sample_depth;
        const int2 sample_pixel = representative_full_pixel(
            coordinates[index],
            full_resolution,
            sample_depth
        );
        const float sample_view_depth =
            view_depth(sample_pixel, sample_depth);
        weights[index] *= exp(
            -abs(sample_view_depth - center_view_depth) *
                depth_sharpness
        );
        result += sample_ssao_at(coordinates[index], ssao_resolution) *
            weights[index];
        weight_sum += weights[index];
    }
    return saturate(result / max(weight_sum, 0.00001f));
}

// The view depth (render units along the camera's axis) of the scene at `uv` and depth-buffer `depth`.
float view_depth_at(float2 uv, float depth)
{
    const float4 homogeneous_world = mul(
        inverse_view_projection,
        float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, depth, 1.0f)
    );
    return abs(1.0f / homogeneous_world.w);
}

// A surface's flat normal for its reflection: the shading normal snapped to the ground or to a wall (the city's
// surfaces), slopes kept. The wet bump's steep slopes sent reflected rays into the ground beside them. (The
// depth's derivatives were no stand-in while the near plane was 0.01: positions rebuilt from the depth jittered
// by centimetres a hundred units away, as much as the pixels lie apart; the snap is cheaper anyway.)
float3 flat_surface_normal(float3 normal)
{
    if (abs(normal.y) > 0.5f)
    {
        return float3(0.0f, normal.y > 0.0f ? 1.0f : -1.0f, 0.0f);
    }
    const float across = length(normal.xz);
    if (abs(normal.y) < 0.35f && across > 0.0001f)
    {
        return float3(normal.x / across, 0.0f, normal.z / across);
    }
    return normal;
}

float2 clip_to_uv(float4 clip)
{
    return float2(clip.x / clip.w * 0.5f + 0.5f, 0.5f - clip.y / clip.w * 0.5f);
}

// Screen-space reflections (ssr_controls.x > 0), added after the AO: a glossy surface (wet streets and puddles,
// water, glass; rougher than ssr_controls.y reflects nothing) marches its reflected ray through the depth buffer
// for up to ssr_controls.z render units, in steps that lengthen with the distance, and reflects the opaque
// scene where the ray first passes behind a surface by less than ssr_controls.w. Its Fresnel reflectance times
// ssr_controls.x weighs the reflection (a top-down camera sees wet ground near Fresnel's minimum), at most
// half; rougher surfaces blur and fade it, as do hits near the screen's edges and the march's end. A ray nearly
// straight back toward the camera finds nothing on screen and reflects nothing. The ray leaves along the surface's flat
// normal (`flat_normal`, flat_surface_normal) bent a quarter of the way toward its shading normal, so ripples and
// grain still waver in it.
float3 screen_space_reflection(
    float2 uv,
    float depth,
    float3 position,
    float3 normal,
    float3 flat_normal,
    float3 view
)
{
    const float intensity = ssr_controls.x;
    const float max_roughness = ssr_controls.y;
    if (intensity <= 0.0f || depth >= 0.99999f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }
    const float4 specular = ambient_specular_texture.SampleLevel(ambient_specular_sampler, uv, 0.0f);
    const float roughness = saturate(specular.a);
    if (roughness >= max_roughness ||
        emission_texture.SampleLevel(emission_sampler, uv, 0.0f).a > 0.5f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }
    const float3 reflecting_normal = normalize(lerp(flat_normal, normal, 0.25f));
    const float3 ray = reflect(-view, reflecting_normal);
    if (dot(ray, view) > 0.9f || dot(ray, flat_normal) < 0.05f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }
    // Each pixel staggers its first step, so the steps do not band.
    const float dither = frac(
        52.9829189f * frac(dot(uv / inverse_resolution, float2(0.06711056f, 0.00583715f)))
    );
    const float3 origin = position + flat_normal * 0.02f;
    float step_length = 0.12f;
    float previous_t = 0.0f;
    float t = step_length * (0.5f + dither);
    bool hit = false;
    float2 hit_uv = float2(0.0f, 0.0f);
    [loop]
    for (uint march = 0u; march < 24u && t < ssr_controls.z; ++march)
    {
        const float4 clip = mul(view_projection, float4(origin + ray * t, 1.0f));
        if (clip.w <= 0.0001f)
        {
            break;
        }
        const float2 sample_uv = clip_to_uv(clip);
        if (any(sample_uv <= 0.0f) || any(sample_uv >= 1.0f))
        {
            break;
        }
        const float scene_depth = depth_texture.SampleLevel(depth_sampler, sample_uv, 0.0f);
        if (clip.z / clip.w > scene_depth && scene_depth < 0.99999f)
        {
            // Behind a surface: halve the last step down to where the ray crossed it. Within the thickness behind
            // the surface there, the ray hit it; farther, it passed behind a nearer object (a pole, a tree) and
            // marches on. (Testing before the halving missed walls: a long step lands far behind them.)
            float low = previous_t;
            float high = t;
            [unroll]
            for (int refine = 0; refine < 5; ++refine)
            {
                const float middle = (low + high) * 0.5f;
                const float4 middle_clip = mul(view_projection, float4(origin + ray * middle, 1.0f));
                const float middle_depth =
                    depth_texture.SampleLevel(depth_sampler, clip_to_uv(middle_clip), 0.0f);
                if (middle_clip.z / middle_clip.w > middle_depth)
                {
                    high = middle;
                }
                else
                {
                    low = middle;
                }
            }
            const float4 crossing_clip = mul(view_projection, float4(origin + ray * high, 1.0f));
            const float2 crossing_uv = clip_to_uv(crossing_clip);
            const float crossing_depth = depth_texture.SampleLevel(depth_sampler, crossing_uv, 0.0f);
            if (crossing_depth < 0.99999f &&
                crossing_clip.w - view_depth_at(crossing_uv, crossing_depth) < ssr_controls.w)
            {
                hit_uv = crossing_uv;
                t = high;
                hit = true;
                break;
            }
        }
        previous_t = t;
        step_length *= 1.18f;
        t += step_length;
    }
    if (!hit)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }
    const float2 edge = min(hit_uv, 1.0f - hit_uv);
    const float fade =
        saturate(min(edge.x, edge.y) * 12.0f) *
        (1.0f - smoothstep(0.6f, 1.0f, t / ssr_controls.z)) *
        (1.0f - smoothstep(max_roughness * 0.5f, max_roughness, roughness));
    // Rough surfaces blur what they reflect, the more the farther the hit.
    const float blur = min(roughness * t * 0.004f, 0.012f);
    float3 reflected = scene_texture.SampleLevel(scene_sampler, hit_uv, 0.0f).rgb;
    if (blur > 0.0005f)
    {
        reflected = (
            reflected * 2.0f +
            scene_texture.SampleLevel(scene_sampler, hit_uv + float2(blur, 0.0f), 0.0f).rgb +
            scene_texture.SampleLevel(scene_sampler, hit_uv - float2(blur, 0.0f), 0.0f).rgb +
            scene_texture.SampleLevel(scene_sampler, hit_uv + float2(0.0f, blur), 0.0f).rgb +
            scene_texture.SampleLevel(scene_sampler, hit_uv - float2(0.0f, blur), 0.0f).rgb
        ) / 6.0f;
    }
    const float3 f0 = ssr_options.x > 0.5f ? specular.rgb : float3(0.04f, 0.04f, 0.04f);
    const float3 fresnel = f0 + (1.0f - f0) * pow(1.0f - saturate(dot(reflecting_normal, view)), 5.0f);
    return reflected * min(fresnel * intensity, 0.5f) * fade;
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
    const float2 uv = input.texture_coordinate;
    const float3 scene = scene_texture.Sample(scene_sampler, uv).rgb;
    const int2 full_resolution = int2(
        1.0f / inverse_resolution
    );
    const int2 center_pixel = clamp(
        int2(input.position.xy),
        int2(0, 0),
        full_resolution - 1
    );
    const float center_depth = sample_depth_at(center_pixel);
    const float4 encoded_normal = sample_normal_at(center_pixel);
    const float3 normal = normalize(
        encoded_normal.xyz * 2.0f - 1.0f
    );
    const float3 center_position =
        world_position(center_pixel, center_depth);
    const float3 view =
        view_direction(center_pixel, center_position);
    const float normal_dot_view =
        max(dot(normal, view), 0.0f);
    const float material_occlusion =
        saturate(encoded_normal.w);
    const float ambient_occlusion =
        depth_aware_upsample(uv);
    const float3 flat_normal = flat_surface_normal(normal);
    const float3 reflection =
        screen_space_reflection(uv, center_depth, center_position, normal, flat_normal, view);
    if (ssao_enabled == 0u)
    {
        // Reflections without ambient occlusion.
        return float4(scene + reflection, 1.0f);
    }
    if (ssao_enabled == 2u)
    {
        const float rc_contact_ao_strength = saturate(ao_controls.z);
        /* The contact AO is the ambient light's shadow: direct light (the sun's, the spot's) keeps only this
           share of it. The scene shader writes its light's directness to the emission target's alpha, times
           rc_directness_scale (btbh_rc_directness_scale there; unlit materials write 1). */
        const float rc_contact_ao_direct_share = saturate(ao_controls.w);
        static const float rc_directness_scale = 0.45f;
        const float4 emission_material = emission_texture.Sample(
            emission_sampler,
            uv
        );
        const float3 emission = emission_material.rgb;
        const float directness = saturate(emission_material.a / rc_directness_scale);
        const float3 preserved_emission = min(emission, scene);
        const float3 occludable_lighting = max(
            scene - emission,
            0.0f
        );
        const float contact_visibility = lerp(
            1.0f,
            ambient_occlusion,
            rc_contact_ao_strength * lerp(1.0f, rc_contact_ao_direct_share, directness)
        );
        return float4(
            preserved_emission + occludable_lighting * contact_visibility + reflection,
            1.0f
        );
    }
    const float4 ambient_diffuse_sample =
        ambient_texture.Sample(ambient_sampler, uv);
    const float4 ambient_specular_sample =
        ambient_specular_texture.Sample(
            ambient_specular_sampler,
            uv
        );
    const float3 ambient_diffuse =
        ambient_diffuse_sample.rgb;
    const float3 ambient_specular =
        ambient_specular_sample.rgb;
    const float roughness =
        saturate(ambient_specular_sample.w);
    const float original_specular_occlusion =
        specular_occlusion(
            normal_dot_view,
            material_occlusion,
            roughness
        );
    const float resolved_specular_occlusion =
        specular_occlusion(
            normal_dot_view,
            material_occlusion * ambient_occlusion,
            roughness
        );
    const float specular_ratio =
        original_specular_occlusion > 0.00001f
            ? resolved_specular_occlusion /
                original_specular_occlusion
            : 0.0f;
    const float3 resolved =
        scene -
        ambient_diffuse -
        ambient_specular +
        ambient_diffuse * ambient_occlusion +
        ambient_specular * specular_ratio;
    return float4(resolved + reflection, 1.0f);
}
