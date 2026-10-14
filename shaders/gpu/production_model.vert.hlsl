#include "production_surface.hlsli"
#include "production_temporal.hlsli"

#ifndef BTBH_RC_SOURCE_PROJECTION
#define BTBH_RC_SOURCE_PROJECTION 0
#endif

struct InstanceData
{
    float4x4 model;
    float4 color;
    float4 custom;
};

StructuredBuffer<InstanceData> instances : register(t0, space0);
#if !BTBH_RC_SOURCE_PROJECTION
// Each instance's world translation since the previous frame (xyz) and how many seconds its vertex-animated
// surface (dissolve, shatter) advanced since then (w), read while temporal_controls.z is set.
StructuredBuffer<float4> instance_motion : register(t1, space0);
#endif

cbuffer SceneUniforms : register(b0, space1)
{
    float4x4 view_projection;
    float4 camera_right;
    float4 camera_up;
    float4 camera_position;
    float4 material_color;
    float4 uv_transform;
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
    float4 world_curvature;
    float4 previous_world_curvature;
    BTBH_TEMPORAL_UNIFORMS
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
    float3 world_tangent : TEXCOORD4;
    float4 instance_custom : TEXCOORD5;
    float3 world_bitangent : TEXCOORD6;
    float4 surface_data : TEXCOORD7;
    nointerpolation float animated_model : TEXCOORD8;
    float4 motion_clip_xy : TEXCOORD9;
    float2 motion_clip_w : TEXCOORD10;
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
    const float3x3 instance_linear = (float3x3)instance.model;
    float4 world_position;
    float3 raw_world_normal;
    float3 raw_world_tangent;
    if (billboard_mode == 0)
    {
        world_position = mul(
            instance.model,
            float4(surface.position, 1.0f)
        );
        raw_world_normal =
            transform_normal(instance_linear, surface.normal);
        raw_world_tangent =
            transform_normal(instance_linear, input.tangent.xyz);
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
        const float3 local_normal =
            transform_normal(instance_linear, surface.normal);
        const float3 local_tangent = transform_normal(
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
#if BTBH_RC_SOURCE_PROJECTION
    const float2 source_uv =
        (world_position.xz - ambient_color_energy.xy) *
        ambient_color_energy.zw;
    output.position = float4(
        source_uv.x * 2.0f - 1.0f,
        1.0f - source_uv.y * 2.0f,
        0.0f,
        1.0f
    );
    output.motion_clip_xy = float4(0.0f, 0.0f, 0.0f, 0.0f);
    output.motion_clip_w = float2(0.0f, 0.0f);
#else
    output.position = mul(view_projection, float4(btbh_world_bend(world_position.xyz, world_curvature), 1.0f));
    float3 previous_world_position = world_position.xyz;
    if (temporal_controls.z > 0.5f)
    {
        const float4 motion = instance_motion[instance_id];
        previous_world_position -= motion.xyz;
        if (motion.w != 0.0f && billboard_mode == 0)
        {
            // Where the dissolve or collapse had moved this vertex one frame earlier.
            const BTBHSurfaceVertexResult previous_surface =
                btbh_apply_surface_vertex(
                    input.position,
                    input.normal,
                    btbh_surface_previous_custom(instance.custom, surface_variant, motion.w),
                    surface_variant,
                    surface_controls.x,
                    surface_parameters0,
                    surface_parameters1
                );
            previous_world_position += mul(
                instance.model,
                float4(previous_surface.position - surface.position, 0.0f)
            ).xyz;
        }
    }
    const BTBHTemporalClip motion_clip = btbh_temporal_clip(
        output.position,
        world_position.xyz,
        btbh_world_bend(previous_world_position, previous_world_curvature),
        previous_view_projection,
        temporal_controls
    );
    output.motion_clip_xy = motion_clip.xy;
    output.motion_clip_w = motion_clip.w;
#endif
    output.color =
        input.color *
        instance.color *
        material_color;
    output.texture_coordinate =
        input.texture_coordinate * uv_transform.xy + uv_transform.zw;
    output.world_position = world_position.xyz;
    output.world_normal = world_normal;
    output.world_tangent = world_tangent;
    output.instance_custom = instance.custom;
    output.world_bitangent = world_bitangent;
    output.surface_data = surface.data;
    output.animated_model = 0.0f;
    return output;
}
