struct InstanceData
{
    float4x4 model;
    float4 color;
};

StructuredBuffer<InstanceData> instances : register(t0, space0);
StructuredBuffer<float4x4> bones : register(t1, space0);

cbuffer SceneUniforms : register(b0, space1)
{
    float4x4 view_projection;
    float4 camera_right;
    float4 camera_up;
    uint bone_offset;
    uint billboard_mode;
    uint2 scene_padding;
};

struct VertexInput
{
    float3 position : TEXCOORD0;
    float4 color : TEXCOORD1;
    float2 texture_coordinate : TEXCOORD2;
    float4 joints : TEXCOORD3;
    float4 weights : TEXCOORD4;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float4 color : TEXCOORD0;
    float2 texture_coordinate : TEXCOORD1;
};

VertexOutput main(VertexInput input, uint instance_id : SV_InstanceID)
{
    const InstanceData instance = instances[instance_id];
    const uint4 joint_indices = uint4(input.joints);
    const float4 local_position = float4(input.position, 1.0f);
    float4 skinned_position =
        mul(bones[bone_offset + joint_indices.x], local_position)
            * input.weights.x;
    skinned_position +=
        mul(bones[bone_offset + joint_indices.y], local_position)
            * input.weights.y;
    skinned_position +=
        mul(bones[bone_offset + joint_indices.z], local_position)
            * input.weights.z;
    skinned_position +=
        mul(bones[bone_offset + joint_indices.w], local_position)
            * input.weights.w;

    const float4 world_position = mul(instance.model, skinned_position);
    VertexOutput output;
    output.position = mul(view_projection, world_position);
    output.color = input.color * instance.color;
    output.texture_coordinate = input.texture_coordinate;
    return output;
}
