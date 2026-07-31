cbuffer ShadowUniforms : register(b0, space1)
{
    float4x4 light_view_projection;
};

static const float3 positions[3] =
{
    float3(-0.5f, -0.5f, 0.0f),
    float3(0.5f, -0.5f, 0.0f),
    float3(0.0f, 0.5f, 0.0f)
};

float4 main(uint vertex_id : SV_VertexID) : SV_Position
{
    return mul(light_view_projection, float4(positions[vertex_id], 1.0f));
}
