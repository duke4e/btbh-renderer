struct InstanceData
{
    float4x4 model;
    float4 color;
};

StructuredBuffer<InstanceData> instances : register(t0, space0);

cbuffer SceneUniforms : register(b0, space1)
{
    float4x4 view_projection;
};

struct VertexInput
{
    float3 position : TEXCOORD0;
    float4 color : TEXCOORD1;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float4 color : TEXCOORD0;
};

VertexOutput main(VertexInput input, uint instance_id : SV_InstanceID)
{
    const InstanceData instance = instances[instance_id];
    const float4 world_position = mul(
        instance.model,
        float4(input.position, 1.0f)
    );
    VertexOutput output;
    output.position = mul(view_projection, world_position);
    output.color = input.color * instance.color;
    return output;
}
