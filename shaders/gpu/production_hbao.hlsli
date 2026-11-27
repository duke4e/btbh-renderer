// The uniforms of the horizon-based ambient occlusion's passes (BTBHGPUScenePost.ssao_enabled 2, from Tyrian3D:
// production_hbao.frag.hlsl, production_hbao_denoise.frag.hlsl, production_hbao_resolve.frag.hlsl), as
// btbh_gpu_scene.c's BTBHGPUSceneHBAOUniforms lays them out. The passes name them as BTBHGPUScenePost does.
cbuffer HBAOUniforms : register(b0, space3)
{
    float4x4 inverse_view_projection;
    // ssao_radius and ssao_bias in world units, ssao_intensity, ssao_power (0: their defaults).
    float4 hbao_controls;
    // x: what it darkens (0 nothing, 1 the ambient light of lit surfaces, 2 with the radiance cascades all of a
    // surface's light but its emission three quarters as much, 3 all of it but the emission); y: bit 0 the debug view.
    uint4 hbao_options;
    // x: the projection's y scale. yzw (RFC 0014, scene views, since 0.16.0): the view's half of the frame as a plane in
    // UV units (hbao_own_side); all zero (one view) the whole frame.
    float4 hbao_view;
};

#define ssao_radius hbao_controls.x
#define ssao_intensity hbao_controls.y
#define ssao_power hbao_controls.z
#define ssao_bias hbao_controls.w
#define ssao_enabled hbao_options.x
#define ssao_flags hbao_options.y
#define projection_scale_y hbao_view.x

// RFC 0014: whether a pixel's or texel's centre (UV units) lies in the view's half of the frame, for a read of what
// the passes wrote there. The passes draw each view clipped to its half widened by a quarter of a render pixel
// (render_texel: one, in UV units), so what lies within a quarter pixel of the line was drawn by both views, the second
// last: a read takes only what lies 3/8 of a render pixel or more inside the view's own line, which the other view
// never drew (a quarter, and an eighth for the rasterizer's rounding). A tap elsewhere counts for nothing, as one on the
// background does. Always with one view.
bool hbao_own_side(float2 uv, float2 render_texel)
{
    return dot(hbao_view.yzw, float3(uv, 1.0f)) >= 0.625f * length(hbao_view.yz * render_texel);
}
