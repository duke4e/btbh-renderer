struct InstanceData
{
    float4x4 model;
    float4 color;
    float4 custom;
};

StructuredBuffer<InstanceData> instances : register(t0, space0);

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
    const float3x3 instance_linear = (float3x3)instance.model;
    const float instance_handedness =
        matrix_handedness(instance_linear);
    float4 world_position;
    float3 raw_world_normal;
    float3 raw_world_tangent;
    if (billboard_mode == 0)
    {
        world_position = mul(
            instance.model,
            float4(input.position, 1.0f)
        );
        raw_world_normal =
            transform_normal(instance_linear, input.normal);
        raw_world_tangent =
            mul(instance_linear, input.tangent.xyz);
    }
    else
    {
        const float3 origin = mul(
            instance.model,
            float4(0.0f, 0.0f, 0.0f, 1.0f)
        ).xyz;
        const float3 local_offset = mul(
            (float3x3)instance.model,
            input.position
        );
        const float3 local_normal =
            transform_normal(instance_linear, input.normal);
        const float3 local_tangent = mul(
            instance_linear,
            input.tangent.xyz
        );
        float3 right = camera_right.xyz;
        float3 up = camera_up.xyz;
        if (billboard_mode == 2)
        {
            right = stable_normalize(
                float3(right.x, 0.0f, right.z),
                float3(1.0f, 0.0f, 0.0f)
            );
            up = float3(0.0f, 1.0f, 0.0f);
        }
        const float3 forward = stable_normalize(
            cross(right, up),
            float3(0.0f, 0.0f, 1.0f)
        );
        world_position = float4(
            origin +
                right * local_offset.x +
                up * local_offset.y +
                forward * local_offset.z,
            1.0f
        );
        raw_world_normal =
            right * local_normal.x +
            up * local_normal.y +
            forward * local_normal.z;
        raw_world_tangent =
            right * local_tangent.x +
            up * local_tangent.y +
            forward * local_tangent.z;
    }

    const float3 world_normal = stable_normalize(
        raw_world_normal,
        float3(0.0f, 0.0f, 1.0f)
    );
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
            instance_handedness
    );
    return output;
}
