struct VertexOutput
{
    float4 position : SV_Position;
    float3 color : TEXCOORD0;
};

static const float2 positions[3] =
{
    float2(0.0f, -0.75f),
    float2(0.75f, 0.65f),
    float2(-0.75f, 0.65f)
};

static const float3 colors[3] =
{
    float3(1.0f, 0.15f, 0.05f),
    float3(0.10f, 0.85f, 0.25f),
    float3(0.05f, 0.25f, 1.0f)
};

VertexOutput main(uint vertex_id : SV_VertexID)
{
    VertexOutput output;
    output.position = float4(positions[vertex_id], 0.5f, 1.0f);
    output.color = colors[vertex_id];
    return output;
}
