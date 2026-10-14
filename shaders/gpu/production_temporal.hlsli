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
