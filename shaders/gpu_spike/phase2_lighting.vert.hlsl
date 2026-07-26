static const float2 positions[3] =
{
    float2(-1.0f, -1.0f),
    float2(3.0f, -1.0f),
    float2(-1.0f, 3.0f)
};

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VertexOutput main(uint vertex_id : SV_VertexID)
{
    VertexOutput output;
    output.position = float4(positions[vertex_id], 0.0f, 1.0f);
    output.uv = positions[vertex_id] * float2(0.5f, -0.5f) + 0.5f;
    return output;
}
