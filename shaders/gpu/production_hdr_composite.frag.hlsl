// HDR composite before grading: distance fog toward the horizon colour, bloom and vignette.
// Runs on the post-SSAO HDR image, so every later stage (LUT, CAS, tilt-shift, output) is unchanged.
Texture2D<float4> scene_texture : register(t0, space2);
SamplerState scene_sampler : register(s0, space2);
Texture2D<float> depth_texture : register(t1, space2);
SamplerState depth_sampler : register(s1, space2);
Texture2D<float4> bloom_texture : register(t2, space2);
SamplerState bloom_sampler : register(s2, space2);
#if BTBH_DISTORTION
// RFC 0018 (screen-space distortion, since 0.18.0): the offset field at half the render size (rg the summed displacement
// in render pixels, x right and y down; b the displacements' lengths times their chroma; a their lengths).
Texture2D<float4> distortion_texture : register(t3, space2);
SamplerState distortion_sampler : register(s3, space2);
#endif

cbuffer CompositeUniforms : register(b0, space3)
{
    float4x4 inverse_view_projection;
    // rgb = linear fog colour, a = density per world unit
    float4 fog_color_density;
    // x = start distance, y = maximum opacity, z = enabled (0/1), w = unused
    float4 fog_shape;
    // x = intensity, y = enabled (0/1), z/w = unused
    float4 bloom_parameters;
    // x = intensity, y = smoothness, z = aspect (width / height), w = enabled (0/1)
    float4 vignette_parameters;
    // world XZ rectangle the fog closes over: min x, min z, max x, max z
    float4 fog_edge_bounds;
    // x = fade distance (inside the rectangle and in view depth), y = enabled (0/1),
    // z = view depth that stays clear of the edge fog, w = unused
    float4 fog_edge;
#if BTBH_DISTORTION
    // xy = 1 / render size, z = the summed displacement's largest length (render pixels), w = the reactive mask's scale
    // (FSR 3's variant: post->temporal_reactive_scale)
    float4 distortion_frame;
    // yzw = the view's half of the frame as a plane in UV units (RFC 0014's, as the reflections' ssr_hit_options.yzw);
    // all zero with one view. x = unused
    float4 distortion_view_plane;
    // x = 1 shows the field instead of the scene (BTBH_DEBUG_DISTORTION); yzw = unused
    float4 distortion_debug;
#endif
};

struct FragmentInput
{
    float4 position : SV_Position;
    float2 texture_coordinate : TEXCOORD0;
};

float4 homogeneous_world(float2 uv, float depth)
{
    const float4 clip_position = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, depth, 1.0f);
    return mul(inverse_view_projection, clip_position);
}

float fog_amount(float2 uv)
{
    const float depth = depth_texture.SampleLevel(depth_sampler, uv, 0.0f);
    // The cleared background has no surface: it is the horizon, already cleared to the fog
    // colour by the scene pass, so transparent effects drawn over it keep their colour.
    if (depth >= 0.99999f)
    {
        return 0.0f;
    }
    const float4 world = homogeneous_world(uv, depth);
    // View depth, like the SSAO passes.
    const float distance = abs(1.0f / world.w);
    const float optical = max(distance - fog_shape.x, 0.0f) * fog_color_density.a;
    float amount = min(1.0f - exp(-optical * optical), fog_shape.y);
    if (fog_edge.y > 0.5f)
    {
        // Thicken to full fog across the band inside the edge of the world, so the city does not
        // end in a hard line against the fogged background. Up to the clear depth (the play area
        // around the camera target) the edge stays crisp, so the action next to it stays readable.
        const float2 position = world.xz / world.w;
        const float inside = min(
            min(position.x - fog_edge_bounds.x, fog_edge_bounds.z - position.x),
            min(position.y - fog_edge_bounds.y, fog_edge_bounds.w - position.y)
        );
        const float edge = 1.0f - saturate(inside / fog_edge.x);
        const float depth_gate = saturate((distance - fog_edge.z) / fog_edge.x);
        amount = max(
            amount,
            edge * edge * (3.0f - 2.0f * edge) * depth_gate * depth_gate * (3.0f - 2.0f * depth_gate)
        );
    }
    return amount;
}

#if !BTBH_DISTORTION
float4 main(FragmentInput input) : SV_Target0
{
    const float2 uv = input.texture_coordinate;
    float3 color = scene_texture.SampleLevel(scene_sampler, uv, 0.0f).rgb;

    if (fog_shape.z > 0.5f)
    {
        color = lerp(color, fog_color_density.rgb, fog_amount(uv));
    }
    if (bloom_parameters.y > 0.5f)
    {
        color += bloom_texture.SampleLevel(bloom_sampler, uv, 0.0f).rgb * bloom_parameters.x;
    }
    if (vignette_parameters.w > 0.5f)
    {
        // Round on screen: x is measured in half-heights, so the corners are at edge = 1.
        float2 offset = (uv - 0.5f) * 2.0f;
        offset.x *= vignette_parameters.z;
        const float edge = saturate(length(offset) / length(float2(vignette_parameters.z, 1.0f)));
        const float falloff = smoothstep(1.0f - vignette_parameters.y, 1.0f, edge);
        color *= 1.0f - vignette_parameters.x * falloff;
    }
    return float4(color, 1.0f);
}
#else
// RFC 0018: the scene displaced by the offset field. Each pixel shows the scene `d` back (d: the field read at the
// pixel, its length clamped; by a view's edge faded out, and no read nearer the edge than the reflections' margin);
// red and blue `d` times 1 +- 0.35 chroma. Fog and bloom are read where green is; the vignette stays on the pixel.
float distortion_line_scale()
{
    return max(length(distortion_view_plane.yz * distortion_frame.xy), 1.0e-30f);
}

// A displaced read moved back along the view's edge's normal to the reflections' margin, 0.5 + |nx| + |ny| render
// pixels inside it (none with one view).
float2 distortion_keep_inside(float2 uv)
{
    if (all(distortion_view_plane.yz == 0.0f))
    {
        return uv;
    }
    const float scale = distortion_line_scale();
    const float2 normal = distortion_view_plane.yz * distortion_frame.xy / scale;
    const float line_pixels = dot(distortion_view_plane.yzw, float3(uv, 1.0f)) / scale;
    const float margin = 0.5f + abs(normal.x) + abs(normal.y);
    if (line_pixels < margin)
    {
        uv += (margin - line_pixels) * normal * distortion_frame.xy;
    }
    return uv;
}

struct CompositeOutput
{
    float4 color : SV_Target0;
#if BTBH_DISTORTION == 2
    // FSR 3's reactive mask (blended with max over the transparent pass's).
    float4 reactive : SV_Target1;
#endif
};

CompositeOutput main(FragmentInput input)
{
    const float2 uv = input.texture_coordinate;
    const float4 field = distortion_texture.SampleLevel(distortion_sampler, uv, 0.0f);
    float2 d = field.xy;
    const float d_length = length(d);
    if (d_length > distortion_frame.z)
    {
        d *= distortion_frame.z / d_length;
    }
    if (any(distortion_view_plane.yz != 0.0f))
    {
        // By the line between two views the field's taps may hold the other view's sources: the displacement fades to
        // nothing between 8 render pixels and 0.5 + 2 (|nx| + |ny|) (the half-size field's bilinear reach).
        const float scale = distortion_line_scale();
        const float2 normal = abs(distortion_view_plane.yz * distortion_frame.xy) / scale;
        const float line_pixels = dot(distortion_view_plane.yzw, float3(uv, 1.0f)) / scale;
        const float margin = 0.5f + 2.0f * (normal.x + normal.y);
        d *= saturate((line_pixels - margin) / (8.0f - margin));
    }
    const float chroma = field.w > 1.0e-4f ? saturate(field.z / field.w) : 0.0f;
    const float2 green_uv = distortion_keep_inside(uv - d * distortion_frame.xy);
    float3 color = scene_texture.SampleLevel(scene_sampler, green_uv, 0.0f).rgb;
    if (chroma > 0.0f && any(d != 0.0f))
    {
        const float2 red_uv = distortion_keep_inside(uv - d * (1.0f + 0.35f * chroma) * distortion_frame.xy);
        const float2 blue_uv = distortion_keep_inside(uv - d * (1.0f - 0.35f * chroma) * distortion_frame.xy);
        color.r = scene_texture.SampleLevel(scene_sampler, red_uv, 0.0f).r;
        color.b = scene_texture.SampleLevel(scene_sampler, blue_uv, 0.0f).b;
    }

    if (fog_shape.z > 0.5f)
    {
        color = lerp(color, fog_color_density.rgb, fog_amount(green_uv));
    }
    if (bloom_parameters.y > 0.5f)
    {
        color += bloom_texture.SampleLevel(bloom_sampler, green_uv, 0.0f).rgb * bloom_parameters.x;
    }
    if (vignette_parameters.w > 0.5f)
    {
        float2 offset = (uv - 0.5f) * 2.0f;
        offset.x *= vignette_parameters.z;
        const float edge = saturate(length(offset) / length(float2(vignette_parameters.z, 1.0f)));
        const float falloff = smoothstep(1.0f - vignette_parameters.y, 1.0f, edge);
        color *= 1.0f - vignette_parameters.x * falloff;
    }
    if (distortion_debug.x > 0.5f)
    {
        color = float3(saturate(0.5f + d / 32.0f), chroma);
    }
    CompositeOutput output;
    output.color = float4(color, 1.0f);
#if BTBH_DISTORTION == 2
    output.reactive = float4(saturate(field.w / 6.0f) * distortion_frame.w, 0.0f, 0.0f, 0.0f);
#endif
    return output;
}
#endif
