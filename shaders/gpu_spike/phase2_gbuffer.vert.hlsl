struct InstanceData
{
    float4 translation;
    float4 color;
};

StructuredBuffer<InstanceData> instances : register(t0, space0);
StructuredBuffer<float4x4> bones : register(t1, space0);

cbuffer SceneUniforms : register(b0, space1)
{
    float4x4 view_projection;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float3 color : TEXCOORD0;
    float3 normal : TEXCOORD1;
};

static const float3 positions[3] =
{
    float3(-0.35f, -0.35f, 0.0f),
    float3(0.35f, -0.35f, 0.0f),
    float3(0.0f, 0.35f, 0.0f)
};

VertexOutput main(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID)
{
    const InstanceData instance = instances[instance_id];
    const float4 skinned = mul(
        bones[instance_id & 3u],
        float4(positions[vertex_id], 1.0f)
    );
    VertexOutput output;
    output.position = mul(
        view_projection,
        float4(skinned.xyz + instance.translation.xyz, 1.0f)
    );
    output.color = instance.color.rgb;
    output.normal = normalize(mul(
        (float3x3)bones[instance_id & 3u],
        float3(0.0f, 0.0f, 1.0f)
    ));
    return output;
}
