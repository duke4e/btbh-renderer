cbuffer ShadowUniforms : register(b0, space1)
{
    float4x4 light_view_projection;
};

struct VertexInput
{
    float3 position : TEXCOORD0;
    float4 model_column_0 : TEXCOORD1;
    float4 model_column_1 : TEXCOORD2;
    float4 model_column_2 : TEXCOORD3;
    float4 model_column_3 : TEXCOORD4;
};

struct VertexOutput
{
    float4 position : SV_Position;
};

VertexOutput main(VertexInput input)
{
    const float4 world_position =
        input.model_column_0 * input.position.x +
        input.model_column_1 * input.position.y +
        input.model_column_2 * input.position.z +
        input.model_column_3;
    VertexOutput output;
    output.position = mul(light_view_projection, world_position);
    return output;
}
