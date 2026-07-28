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
    uint scene_padding;
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
    float4 world_tangent : TEXCOORD4;
};

float3 stable_normalize(float3 value, float3 fallback)
{
    const float length_squared = dot(value, value);
    return length_squared > 0.00000001f
        ? value * rsqrt(length_squared)
        : fallback;
}

float matrix_handedness(float3x3 matrix)
{
    return determinant(matrix) < 0.0f ? -1.0f : 1.0f;
}

float3 transform_normal(float3x3 matrix, float3 normal)
{
    const float3 row0 = matrix[0];
    const float3 row1 = matrix[1];
    const float3 row2 = matrix[2];
    const float3 cofactor_normal = float3(
        dot(cross(row1, row2), normal),
        dot(cross(row2, row0), normal),
        dot(cross(row0, row1), normal)
    );
    return cofactor_normal * matrix_handedness(matrix);
}

VertexOutput main(VertexInput input, uint instance_id : SV_InstanceID)
{
    const InstanceData instance = instances[instance_id];
    const uint4 joint_indices = uint4(input.joints);
    const float4 local_position = float4(input.position, 1.0f);
    const float4x4 skin_matrix =
        bones[bone_offset + joint_indices.x] * input.weights.x +
        bones[bone_offset + joint_indices.y] * input.weights.y +
        bones[bone_offset + joint_indices.z] * input.weights.z +
        bones[bone_offset + joint_indices.w] * input.weights.w;
    const float3x3 skin_linear = (float3x3)skin_matrix;
    const float4 skinned_position =
        mul(skin_matrix, local_position);
    const float3 skinned_normal =
        transform_normal(skin_linear, input.normal);
    const float3 skinned_tangent =
        mul(skin_linear, input.tangent.xyz);

    const float4 world_position = mul(instance.model, skinned_position);
    const float3x3 instance_linear = (float3x3)instance.model;
    const float3 world_normal = stable_normalize(
        transform_normal(instance_linear, skinned_normal),
        float3(0.0f, 0.0f, 1.0f)
    );
    const float3 raw_world_tangent =
        mul(instance_linear, skinned_tangent);
    const float3 orthogonal_tangent =
        raw_world_tangent -
        world_normal * dot(raw_world_tangent, world_normal);
    const float3 tangent_fallback = abs(world_normal.z) < 0.999f
        ? stable_normalize(
            cross(float3(0.0f, 0.0f, 1.0f), world_normal),
            float3(1.0f, 0.0f, 0.0f)
        )
        : float3(1.0f, 0.0f, 0.0f);

    VertexOutput output;
    output.position = mul(view_projection, world_position);
    output.color =
        input.color *
        instance.color *
        material_color;
    output.texture_coordinate = input.texture_coordinate;
    output.world_position = world_position.xyz;
    output.world_normal = world_normal;
    output.world_tangent = float4(
        stable_normalize(orthogonal_tangent, tangent_fallback),
        (input.tangent.w < 0.0f ? -1.0f : 1.0f) *
            matrix_handedness(skin_linear) *
            matrix_handedness(instance_linear)
    );
    return output;
}
