cbuffer CanvasUniforms : register(b0, space1)
{
    float2 drawable_size;
    float2 padding;
};

struct VertexInput
{
    float2 position : TEXCOORD0;
    float2 texture_coordinate : TEXCOORD1;
    float4 color : TEXCOORD2;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float4 color : TEXCOORD0;
    float2 texture_coordinate : TEXCOORD1;
};

VertexOutput main(VertexInput input)
{
    VertexOutput output;
    const float2 normalized = input.position / drawable_size;
    output.position = float4(
        normalized.x * 2.0f - 1.0f,
        1.0f - normalized.y * 2.0f,
        0.0f,
        1.0f
    );
    output.color = input.color;
    output.texture_coordinate = input.texture_coordinate;
    return output;
}
