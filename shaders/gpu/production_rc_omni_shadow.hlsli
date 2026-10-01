float rc_omni_projected_depth(
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

float rc_hero_omni_visibility(float3 world_position, float3 normal)
{
    if (hero_omni_shadow_controls.z < 0.5f)
    {
        return 1.0f;
    }
    const float3 from_light =
        world_position - hero_omni_shadow_position_range.xyz;
    const float major_distance = max(
        max(abs(from_light.x), abs(from_light.y)),
        abs(from_light.z)
    );
    const float near_plane = hero_omni_shadow_controls.x;
    const float far_plane = hero_omni_shadow_position_range.w;
    if (major_distance <= near_plane || major_distance >= far_plane)
    {
        return 1.0f;
    }
    const float vector_length = length(from_light);
    const float3 sample_direction = vector_length > 0.000001f
        ? from_light / vector_length
        : float3(0.0f, 0.0f, 1.0f);
    const float3 light_direction = -sample_direction;
    const float normal_dot_light = saturate(dot(normal, light_direction));
    const float world_bias =
        0.025f + 0.1f * (1.0f - normal_dot_light);
    const float reference_depth = rc_omni_projected_depth(
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
        max(hero_omni_shadow_controls.y, 0.0f) * 2.0f / 512.0f;
    float visibility = 0.0f;
    float minimum_visibility = 1.0f;
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
            const float stored_depth = hero_omni_shadow_texture.SampleLevel(
                hero_omni_shadow_sampler,
                direction,
                0.0f
            );
            const float sample_visibility =
                reference_depth <= stored_depth ? 1.0f : 0.0f;
            visibility += sample_visibility;
            minimum_visibility = min(
                minimum_visibility,
                sample_visibility
            );
        }
    }
    // RC is a low-frequency signal, so blend the PCF average with the most
    // conservative tap to prevent thin blockers leaking at cubemap seams.
    return lerp(minimum_visibility, visibility / 9.0f, 0.5f);
}
