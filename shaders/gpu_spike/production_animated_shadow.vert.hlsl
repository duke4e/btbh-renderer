#include "production_surface.hlsli"

struct InstanceData
{
    float4x4 model;
    float4 color;
    float4 custom;
};

StructuredBuffer<InstanceData> instances : register(t0, space0);
StructuredBuffer<float4x4> bones : register(t1, space0);

cbuffer ShadowUniforms : register(b0, space1)
{
    float4x4 light_view_projection;
    float4 camera_right;
    float4 camera_up;
    uint bone_offset;
    uint billboard_mode;
    uint surface_variant;
    float surface_time;
    float4 surface_parameters0;
    float4 surface_parameters1;
};

struct VertexInput
{
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD3;
    float4 joints : TEXCOORD4;
    float4 weights : TEXCOORD5;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float4 surface_data : TEXCOORD0;
};

VertexOutput main(VertexInput input, uint instance_id : SV_InstanceID)
{
    const InstanceData instance = instances[instance_id];
    const BTBHSurfaceVertexResult surface =
        btbh_apply_surface_vertex(
            input.position,
            input.normal,
            instance.custom,
            surface_variant,
            surface_time,
            surface_parameters0,
            surface_parameters1
        );
    const uint4 joint_indices = uint4(input.joints);
    const float4x4 skin_matrix =
        bones[bone_offset + joint_indices.x] * input.weights.x +
        bones[bone_offset + joint_indices.y] * input.weights.y +
        bones[bone_offset + joint_indices.z] * input.weights.z +
        bones[bone_offset + joint_indices.w] * input.weights.w;
    const float4 skinned_position = mul(
        skin_matrix,
        float4(surface.position, 1.0f)
    );
    VertexOutput output;
    output.position = mul(
        light_view_projection,
        mul(instance.model, skinned_position)
    );
    output.surface_data = surface.data;
    return output;
}
