#include "production_surface.hlsli"

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
};

struct VertexOutput
{
    float4 position : SV_Position;
    float4 surface_data : TEXCOORD0;
};

float3 stable_normalize(float3 value, float3 fallback)
{
    const float length_squared = dot(value, value);
    return length_squared > 0.00000001f
        ? value * rsqrt(length_squared)
        : fallback;
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
            surface_time,
            surface_parameters0,
            surface_parameters1
        );
    float4 world_position;
    if (billboard_mode == 0u)
    {
        world_position = mul(
            instance.model,
            float4(surface.position, 1.0f)
        );
    }
    else
    {
        const float3 origin = mul(
            instance.model,
            float4(0.0f, 0.0f, 0.0f, 1.0f)
        ).xyz;
        const float3 local_offset = mul(
            (float3x3)instance.model,
            surface.position
        );
        float3 right = camera_right.xyz;
        float3 up = camera_up.xyz;
        if (billboard_mode == 2u)
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
    }
    VertexOutput output;
    output.position = mul(light_view_projection, world_position);
    output.surface_data = surface.data;
    return output;
}
