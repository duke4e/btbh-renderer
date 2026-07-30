struct InstanceData
{
    float4x4 model;
    float4 color;
    float4 custom;
};

StructuredBuffer<InstanceData> instances : register(t0, space0);

cbuffer ShadowUniforms : register(b0, space1)
{
    float4x4 light_view_projection;
};

struct VertexInput
{
    float3 position : TEXCOORD0;
};

struct VertexOutput
{
    float4 position : SV_Position;
};

VertexOutput main(VertexInput input, uint instance_id : SV_InstanceID)
{
    const InstanceData instance = instances[instance_id];
    const float4 world_position = mul(
        instance.model,
        float4(input.position, 1.0f)
    );
    VertexOutput output;
    output.position = mul(light_view_projection, world_position);
    return output;
}
