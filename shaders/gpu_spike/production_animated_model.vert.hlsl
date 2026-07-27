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
    float3 skinned_normal =
        mul(
            (float3x3)bones[bone_offset + joint_indices.x],
            input.normal
        ) * input.weights.x;
    skinned_normal +=
        mul(
            (float3x3)bones[bone_offset + joint_indices.y],
            input.normal
        ) * input.weights.y;
    skinned_normal +=
        mul(
            (float3x3)bones[bone_offset + joint_indices.z],
            input.normal
        ) * input.weights.z;
    skinned_normal +=
        mul(
            (float3x3)bones[bone_offset + joint_indices.w],
            input.normal
        ) * input.weights.w;

    const float4 world_position = mul(instance.model, skinned_position);
    VertexOutput output;
    output.position = mul(view_projection, world_position);
    float3 light_factor = 1.0f;
    const float total_energy =
        ambient_color_energy.w +
        directional_direction_energy.w +
        secondary_directional_direction_energy.w;
    if (unlit_enabled == 0 && total_energy > 0.0f)
    {
        const float3 raw_world_normal =
            mul((float3x3)instance.model, skinned_normal);
        const float normal_length = max(
            length(raw_world_normal),
            0.00001f
        );
        const float3 world_normal =
            raw_world_normal / normal_length;
        float3 illumination =
            ambient_color_energy.rgb * ambient_color_energy.w;
        if (directional_direction_energy.w > 0.0f)
        {
            const float direction_length = max(
                length(directional_direction_energy.xyz),
                0.00001f
            );
            const float3 surface_to_light =
                -directional_direction_energy.xyz / direction_length;
            const float diffuse = max(
                dot(world_normal, surface_to_light),
                0.0f
            );
            illumination +=
                directional_color.rgb *
                directional_direction_energy.w *
                diffuse;
        }
        if (secondary_directional_direction_energy.w > 0.0f)
        {
            const float direction_length = max(
                length(secondary_directional_direction_energy.xyz),
                0.00001f
            );
            const float3 surface_to_light =
                -secondary_directional_direction_energy.xyz /
                direction_length;
            const float diffuse = max(
                dot(world_normal, surface_to_light),
                0.0f
            );
            illumination +=
                secondary_directional_color.rgb *
                secondary_directional_direction_energy.w *
                diffuse;
        }
        light_factor = illumination / (1.0f + illumination);
    }
    output.color =
        input.color *
        instance.color *
        material_color *
        float4(light_factor, 1.0f);
    output.texture_coordinate = input.texture_coordinate;
    return output;
}
