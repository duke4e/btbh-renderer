struct VertexOutput
{
    float4 position : SV_Position;
    float2 texture_coordinate : TEXCOORD0;
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
    return output;
}
