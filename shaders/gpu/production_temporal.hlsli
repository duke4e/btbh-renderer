#ifndef PRODUCTION_TEMPORAL_HLSLI
#define PRODUCTION_TEMPORAL_HLSLI

// Temporal upscaling (FSR 3.1) inputs from the scene shaders. While the upscaler runs, the projection
// carries a sub-pixel jitter (temporal_controls.xy, in clip space), the opaque pass writes each pixel's
// motion since the previous frame to its sixth colour target (UV units, previous minus current, jitter
// removed) and the transparent pass writes how much each pixel's colour comes from effects drawn over
// the scene (the reactive mask) to its second.

// x, y: the jitter added to clip-space x and y (times w); z: 1 when the draw's instances carry their
// motion since the previous frame; w: 1 while the upscaler runs. previous_bone_offset: where the bone
// palette the previous frame drew a skinned geometry with starts (its bone_offset when there was none).
#define BTBH_TEMPORAL_UNIFORMS \
    float4x4 previous_view_projection; \
    float4 temporal_controls; \
    uint previous_bone_offset; \
    uint3 temporal_reserved;

// Render-only world curvature (curvature: x the signed curvature, 1 / radius; y, z the world x and z it
// bends around). Positive: a small planet. The ground keeps its x and z and drops by the sphere's sag,
// and anything above it leans out along the sphere's normal. Negative: a hyperbolic bowl. Distance from
// the centre compresses to 2R tanh(r / 2R), so the whole city fits inside a ring, heights shrink with
// it, and the ground rises toward the ring (BTBH_WORLD_BOWL of its radius), so the far city climbs up
// around the kaiju where the planet's falls away. Zero draws the world flat. Lighting, shadows and world_position stay flat; only where a
// vertex is drawn moves (RenderWorldCurvature.bend in Swift is the same formula).
#define BTBH_WORLD_BOWL 0.75f

float3 btbh_world_bend(float3 position, float4 curvature)
{
    const float k = curvature.x;
    if (abs(k) < 1e-5f)
    {
        return position;
    }
    const float2 offset = position.xz - curvature.yz;
    const float r = length(offset);
    if (r < 1e-6f)
    {
        return position;
    }
    const float2 direction = offset / r;
    if (k > 0.0f)
    {
        const float angle = min(r * k, 3.14159265f);
        const float sag = (1.0f - cos(angle)) / k;
        const float lean = position.y * sin(angle);
        const float2 xz = curvature.yz + direction * (r + lean);
        return float3(xz.x, position.y * cos(angle) - sag, xz.y);
    }
    const float span = 2.0f / -k;
    const float u = r / span;
    const float compressed = span * tanh(u);
    const float shrink = 1.0f / (cosh(u) * cosh(u));
    const float2 xz = curvature.yz + direction * compressed;
    const float rim = compressed / span;
    return float3(xz.x, position.y * shrink + BTBH_WORLD_BOWL * span * rim * rim, xz.y);
}

// The vertex's clip position under this frame's projection without the jitter (xy) and under the
// previous frame's (zw), and both w.
struct BTBHTemporalClip
{
    float4 xy;
    float2 w;
};

BTBHTemporalClip btbh_temporal_clip(
    float4 jittered_clip,
    float3 world_position,
    float3 previous_world_position,
    float4x4 previous_view_projection,
    float4 temporal_controls
)
{
    BTBHTemporalClip clip;
    const float2 current = jittered_clip.xy - temporal_controls.xy * jittered_clip.w;
    float4 previous = float4(current, 0.0f, jittered_clip.w);
    if (temporal_controls.w > 0.5f)
    {
        previous = mul(previous_view_projection, float4(previous_world_position, 1.0f));
    }
    clip.xy = float4(current, previous.xy);
    clip.w = float2(jittered_clip.w, previous.w);
    return clip;
}

// Motion in UV units from this pixel to where it was in the previous frame (FSR's convention).
float2 btbh_temporal_velocity(float4 clip_xy, float2 clip_w)
{
    if (clip_w.x <= 0.000001f || clip_w.y <= 0.000001f)
    {
        return float2(0.0f, 0.0f);
    }
    const float2 current = clip_xy.xy / clip_w.x;
    const float2 previous = clip_xy.zw / clip_w.y;
    return (previous - current) * float2(0.5f, -0.5f);
}

// The reactive mask's value for an effect's colour: its coverage (alpha) times the frame's scale.
float btbh_temporal_reactive(float4 color, float scale)
{
    return saturate(color.a * scale);
}

#endif
