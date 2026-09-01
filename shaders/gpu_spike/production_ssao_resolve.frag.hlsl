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
    float2 post_padding;
    float4x4 inverse_view_projection;
    float projection_scale_y;
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
    const float3 far_position = world_position(
        center_pixel,
        0.9999f
    );
    const float3 view = normalize(center_position - far_position);
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
    const float3 far_position =
        world_position(center_pixel, 0.9999f);
    const float3 view =
        normalize(center_position - far_position);
    const float normal_dot_view =
        max(dot(normal, view), 0.0f);
    const float material_occlusion =
        saturate(encoded_normal.w);
    const float ambient_occlusion =
        depth_aware_upsample(uv);
    if (ssao_enabled == 2u)
    {
        static const float rc_contact_ao_strength = 0.75f;
        const float3 emission = emission_texture.Sample(
            emission_sampler,
            uv
        ).rgb;
        const float3 preserved_emission = min(emission, scene);
        const float3 occludable_lighting = max(
            scene - emission,
            0.0f
        );
        const float contact_visibility = lerp(
            1.0f,
            ambient_occlusion,
            rc_contact_ao_strength
        );
        return float4(
            preserved_emission + occludable_lighting * contact_visibility,
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
    return float4(resolved, 1.0f);
}
