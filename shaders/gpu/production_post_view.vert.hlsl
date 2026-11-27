// RFC 0014 (scene views, since 0.16.0): production_post.vert's full-screen triangle, clipped to one view's half of the
// frame, for the screen-space passes that read a view's camera (the SSAO and HBAO with their denoise and resolve, the
// distance pass, the HDR composite). Each view draws the triangle with its own block in the same pass, so every pixel
// is shaded with its own view's matrices; the fragment shaders are the ones production_post.vert draws.
cbuffer PostViewUniforms : register(b0, space1)
{
    // xyz: the view's half as a plane in clip space (w is 1 here): a point is drawn where
    // x * view_clip_plane.x + y * view_clip_plane.y + view_clip_plane.z >= 0. w: 0.
    float4 view_clip_plane;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float2 texture_coordinate : TEXCOORD0;
    float clip_distance : SV_ClipDistance0;
};

VertexOutput main(uint vertex_id : SV_VertexID)
{
    const float2 positions[3] = {
        float2(-1.0f, -1.0f),
        float2(-1.0f, 3.0f),
        float2(3.0f, -1.0f)
    };
    VertexOutput output;
    output.position = float4(positions[vertex_id], 0.0f, 1.0f);
    output.texture_coordinate = float2(
        positions[vertex_id].x * 0.5f + 0.5f,
        0.5f - positions[vertex_id].y * 0.5f
    );
    output.clip_distance = dot(view_clip_plane.xyz, float3(positions[vertex_id], 1.0f));
    return output;
}
