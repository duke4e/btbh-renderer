// RFC 0018 (screen-space distortion, since 0.18.0): the source pass. One quad a source (six vertices), the source from
// the instance index in a block of up to 60, each its shape's bounds in its plane (src/gpu/btbh_gpu_distortion.h's
// record: the plane's origin and axes, worked out on the CPU), projected with the view's matrix into the offset field
// (half the render size). Clipped to the view's half of the frame (RFC 0014), as production_post_view.vert.
cbuffer DistortionUniforms : register(b0, space1)
{
    float4x4 view_projection;
    // xyz: the view's half as a plane in clip space (a point is drawn where x * a + y * b + w * c >= 0); all zero: the
    // whole frame. w: 0.
    float4 view_clip_plane;
    float4 distortion_reserved[3];
    // Four float4 a source: origin and radius; axis_u and span; axis_v and strength; chroma, haze scale, haze drift and
    // kind.
    float4 sources[240];
};

struct VertexOutput
{
    float4 position : SV_Position;
    float2 local : TEXCOORD0;
    nointerpolation float4 shape : TEXCOORD1;
    nointerpolation float4 extra : TEXCOORD2;
    float clip_distance : SV_ClipDistance0;
};

VertexOutput main(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID)
{
    const float4 a = sources[instance_id * 4u + 0u];
    const float4 b = sources[instance_id * 4u + 1u];
    const float4 c = sources[instance_id * 4u + 2u];
    const float4 d = sources[instance_id * 4u + 3u];
    const uint kind = (uint)(d.w + 0.5f);
    const uint form = kind % 3u;
    float2 low;
    float2 high;
    if (form == 0u)
    {
        const float extent = a.w + 0.5f * b.w;
        low = float2(-extent, -extent);
        high = float2(extent, extent);
    }
    else if (form == 1u)
    {
        low = float2(-a.w, -a.w);
        high = float2(a.w, a.w);
    }
    else
    {
        low = float2(-a.w, -a.w);
        high = float2(b.w + a.w, a.w);
    }
    const float2 corners[6] = {
        float2(0.0f, 0.0f),
        float2(1.0f, 0.0f),
        float2(0.0f, 1.0f),
        float2(0.0f, 1.0f),
        float2(1.0f, 0.0f),
        float2(1.0f, 1.0f)
    };
    const float2 local = lerp(low, high, corners[vertex_id]);
    const float3 world = a.xyz + b.xyz * local.x + c.xyz * local.y;
    VertexOutput output;
    output.position = mul(view_projection, float4(world, 1.0f));
    output.local = local;
    output.shape = float4(a.w, b.w, c.w, d.w);
    output.extra = float4(d.x, d.y, d.z, 0.0f);
    output.clip_distance = any(view_clip_plane.xyz != 0.0f)
        ? dot(view_clip_plane.xyz, output.position.xyw)
        : 1.0f;
    return output;
}
