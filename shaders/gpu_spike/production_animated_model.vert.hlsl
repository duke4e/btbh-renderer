#include "production_surface.hlsli"

struct InstanceData
{
    float4x4 model;
    float4 color;
    float4 custom;
};

StructuredBuffer<InstanceData> instances : register(t0, space0);
StructuredBuffer<float4x4> bones : register(t1, space0);

cbuffer SceneUniforms : register(b0, space1)
{
    float4x4 view_projection;
    float4 camera_right;
    float4 camera_up;
    float4 camera_position;
    float4 material_color;
    float4 ambient_color_energy;
    float4 directional_direction_energy;
    float4 directional_color;
    float4 secondary_directional_direction_energy;
    float4 secondary_directional_color;
    uint bone_offset;
    uint billboard_mode;
    uint unlit_enabled;
    uint surface_variant;
    float4 surface_controls;
    float4 surface_parameters0;
    float4 surface_parameters1;
};

struct VertexInput
{
    float3 position : TEXCOORD0;
    float4 color : TEXCOORD1;
    float2 texture_coordinate : TEXCOORD2;
    float3 normal : TEXCOORD3;
    float4 joints : TEXCOORD4;
    float4 weights : TEXCOORD5;
    float4 tangent : TEXCOORD6;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float4 color : TEXCOORD0;
    float2 texture_coordinate : TEXCOORD1;
    float3 world_position : TEXCOORD2;
    float3 world_normal : TEXCOORD3;
    float3 world_tangent : TEXCOORD4;
    float4 instance_custom : TEXCOORD5;
    float3 world_bitangent : TEXCOORD6;
    float4 surface_data : TEXCOORD7;
    nointerpolation float animated_model : TEXCOORD8;
};

float3 stable_normalize(float3 value, float3 fallback)
{
    const float length_squared = dot(value, value);
    return length_squared > 0.00000001f
        ? value * rsqrt(length_squared)
        : fallback;
}

float3 transform_normal(float3x3 matrix, float3 normal)
{
    const float3 row0 = matrix[0];
    const float3 row1 = matrix[1];
    const float3 row2 = matrix[2];
    const float matrix_scale = max(
        max(max(abs(row0.x), abs(row0.y)), abs(row0.z)),
        max(
            max(max(abs(row1.x), abs(row1.y)), abs(row1.z)),
            max(max(abs(row2.x), abs(row2.y)), abs(row2.z))
        )
    );
    if (matrix_scale <= 0.0f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }
    const float3 scaled_row0 = row0 / matrix_scale;
    const float3 scaled_row1 = row1 / matrix_scale;
    const float3 scaled_row2 = row2 / matrix_scale;
    const float3 cofactor_normal = float3(
        dot(cross(scaled_row1, scaled_row2), normal),
        dot(cross(scaled_row2, scaled_row0), normal),
        dot(cross(scaled_row0, scaled_row1), normal)
    );
    const float handedness =
        dot(scaled_row0, cross(scaled_row1, scaled_row2)) < 0.0f
            ? -1.0f
            : 1.0f;
    return cofactor_normal * handedness;
}

VertexOutput main(VertexInput input, uint instance_id : SV_InstanceID)
{
    const InstanceData instance = instances[instance_id];
    const BTBHSurfaceVertexResult surface =
        btbh_apply_surface_vertex(
            input.position,
            input.normal,
            instance.custom,
            surface_variant,
            surface_controls.x,
            surface_parameters0,
            surface_parameters1
        );
    const uint4 joint_indices = uint4(input.joints);
    const float4 local_position = float4(surface.position, 1.0f);
    const float4x4 skin_matrix =
        bones[bone_offset + joint_indices.x] * input.weights.x +
        bones[bone_offset + joint_indices.y] * input.weights.y +
        bones[bone_offset + joint_indices.z] * input.weights.z +
        bones[bone_offset + joint_indices.w] * input.weights.w;
    const float3x3 skin_linear = (float3x3)skin_matrix;
    const float4 skinned_position =
        mul(skin_matrix, local_position);
    const float3 skinned_normal =
        transform_normal(skin_linear, surface.normal);
    const float3 skinned_tangent =
        transform_normal(skin_linear, input.tangent.xyz);

    const float4 world_position = mul(instance.model, skinned_position);
    const float3x3 instance_linear = (float3x3)instance.model;
    const float3 world_normal = stable_normalize(
        transform_normal(instance_linear, skinned_normal),
        float3(0.0f, 0.0f, 1.0f)
    );
    const float3 raw_world_tangent =
        transform_normal(instance_linear, skinned_tangent);
    const float3 tangent_fallback = abs(world_normal.z) < 0.999f
        ? stable_normalize(
            cross(float3(0.0f, 0.0f, 1.0f), world_normal),
            float3(1.0f, 0.0f, 0.0f)
        )
        : float3(1.0f, 0.0f, 0.0f);
    const float3 world_tangent = stable_normalize(
        raw_world_tangent,
        tangent_fallback
    );
    const float tangent_handedness =
        input.tangent.w < 0.0f ? -1.0f : 1.0f;
    const float3 world_bitangent = stable_normalize(
        cross(world_normal, world_tangent) * tangent_handedness,
        float3(0.0f, 1.0f, 0.0f)
    );

    VertexOutput output;
    output.position = mul(view_projection, world_position);
    output.color =
        input.color *
        instance.color *
        material_color;
    output.texture_coordinate = input.texture_coordinate;
    output.world_position = world_position.xyz;
    output.world_normal = world_normal;
    output.world_tangent = world_tangent;
    output.instance_custom = instance.custom;
    output.world_bitangent = world_bitangent;
    output.surface_data = surface.data;
    output.animated_model = 1.0f;
    return output;
}
