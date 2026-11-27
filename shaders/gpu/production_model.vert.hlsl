#include "production_surface.hlsli"
#include "production_temporal.hlsli"

#ifndef BTBH_RC_SOURCE_PROJECTION
#define BTBH_RC_SOURCE_PROJECTION 0
#endif
// RFC 0009: 1 in production_model_detail_table.vert, the static vertex shader of a batch with a world-detail table,
// which reads each vertex's table index (the joint slot's first float) and passes it in animated_model. Every
// other static batch draws with this file, which reads and passes on what 0.7.0's did: the extra vertex attribute
// made a frame of many small instanced draws dearer, a table or not (RFC 0009, phase 2 notes).
#ifndef BTBH_WORLD_DETAIL_TABLE
#define BTBH_WORLD_DETAIL_TABLE 0
#endif
#if BTBH_WORLD_DETAIL_TABLE
#include "production_world_detail.hlsli"
#endif
// RFC 0012: 1 in production_model_lift.vert and production_model_detail_table_lift.vert, the static vertex shaders of a
// batch with a height lift (BTBHGPUSceneGeometryBatch.height_lift), which lift every vertex by a heightfield after
// its instance's matrix and the wind. Every other static batch draws without it: these lines compile to nothing.
#ifndef BTBH_HEIGHT_LIFT
#define BTBH_HEIGHT_LIFT 0
#endif
#if BTBH_HEIGHT_LIFT
#include "production_height_lift.hlsli"
#endif

struct InstanceData
{
    float4x4 model;
    float4 color;
    float4 custom;
};

#if BTBH_HEIGHT_LIFT
// After the heightfield (production_height_lift.hlsli).
StructuredBuffer<InstanceData> instances : register(t1, space0);
StructuredBuffer<float4> instance_motion : register(t2, space0);
#else
StructuredBuffer<InstanceData> instances : register(t0, space0);
#if !BTBH_RC_SOURCE_PROJECTION
// Each instance's world translation since the previous frame (xyz) and how many seconds its vertex-animated
// surface (dissolve, shatter) advanced since then (w), read while temporal_controls.z is set.
StructuredBuffer<float4> instance_motion : register(t1, space0);
#endif
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
    BTBH_TEMPORAL_UNIFORMS_STATIC
};

struct VertexInput
{
    float3 position : TEXCOORD0;
    float4 color : TEXCOORD1;
    float2 texture_coordinate : TEXCOORD2;
    float3 normal : TEXCOORD3;
    float4 tangent : TEXCOORD6;
#if BTBH_WORLD_DETAIL_TABLE && !BTBH_RC_SOURCE_PROJECTION
    // RFC 0009: the first float of the vertex's joint slot (byte 64), its world-detail table index. Declared after
    // the tangent, so it takes location 5 and locations 0..4 stay where 0.7.0's shader had them.
    float detail_index : TEXCOORD7;
#endif
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
    // 0 static, 1 skinned; RFC 0009: -1 - the triangle's world-detail table index from
    // production_model_detail_table.vert (flat: all three vertices must carry the same index).
    nointerpolation float animated_model : TEXCOORD8;
    float4 motion_clip_xy : TEXCOORD9;
    float2 motion_clip_w : TEXCOORD10;
#if !BTBH_RC_SOURCE_PROJECTION
    // RFC 0014: a scene view's half of the frame (btbh_gpu_window_scene_present_views): the rasterizer drops what lies
    // on the other side. A built-in, no varying: the fragment shaders don't read it.
    float clip_distance : SV_ClipDistance0;
#endif
};

// Only a zero vector takes the fallback: a normal through the cofactor of an instance at the city's 0.002 scale
// is about 4e-6 long, and the old 1e-4 cutoff gave every Megapolis surface the fallback (0, 0, 1) (until
// 2026-10-17 the whole city was lit, occluded and reflected as if it faced +z).
float3 stable_normalize(float3 value, float3 fallback)
{
    const float length_squared = dot(value, value);
    return length_squared > 1.0e-30f
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
        world_position.xyz += btbh_surface_wind_offset(
            world_position.xyz,
            mul(instance.model, float4(0.0f, 0.0f, 0.0f, 1.0f)).xyz,
            surface_variant,
            surface_controls.x,
            surface_parameters1
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

#if BTBH_HEIGHT_LIFT
    // RFC 0012: the lift maps (x, y, z) to (x, y + h(x, z), z) after the instance and the wind (a lifted batch is
    // never a billboard): the tangent goes through its Jacobian, the normal through its inverse transpose, so
    // vertical faces keep their normals and level ones tilt with the slope. The flat position is kept for the
    // previous frame's, which is lifted at its own xz.
    const float3 height_lift_flat_world = world_position.xyz;
    const float3 height_lift = btbh_height_lift(height_lift_flat_world.xz);
    world_position.y += height_lift.x;
    raw_world_normal = float3(
        raw_world_normal.x - height_lift.y * raw_world_normal.y,
        raw_world_normal.y,
        raw_world_normal.z - height_lift.z * raw_world_normal.y
    );
    raw_world_tangent.y += height_lift.y * raw_world_tangent.x + height_lift.z * raw_world_tangent.z;
#endif
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
    // RFC 0014: the view's half as a plane in clip space (surface_controls.yzw: a point is drawn where
    // a x + b y + c w >= 0); all zero (one view, the whole frame) draws every point.
    output.clip_distance = any(surface_controls.yzw != 0.0f)
        ? dot(surface_controls.yzw, output.position.xyw)
        : 1.0f;
    float3 previous_world_position = world_position.xyz;
    if (temporal_controls.z > 0.5f)
    {
        const float4 motion = instance_motion[instance_id];
#if BTBH_HEIGHT_LIFT
        previous_world_position = height_lift_flat_world;
#endif
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
#if BTBH_HEIGHT_LIFT
        // Where the moving instance stood a frame ago, on the ground there (a second read).
        previous_world_position.y += btbh_height_lift(previous_world_position.xz).x;
#endif
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
    // RFC 0013: BTBH_GPU_SCENE_BATCH_INSTANCE_UV_RECT, the instance's cell of an atlas (custom xy its size, zw its
    // corner). Other batches skip it, and their texture coordinate is what it was.
    if (instance_uv_rect != 0u)
    {
        output.texture_coordinate = output.texture_coordinate * instance.custom.xy + instance.custom.zw;
    }
    output.world_position = world_position.xyz;
    output.world_normal = world_normal;
    output.world_tangent = world_tangent;
    output.instance_custom = instance.custom;
    output.world_bitangent = world_bitangent;
    output.surface_data = surface.data;
#if BTBH_WORLD_DETAIL_TABLE && !BTBH_RC_SOURCE_PROJECTION
    output.animated_model = btbh_world_detail_table_pack(btbh_world_detail_table_index(input.detail_index));
#else
    output.animated_model = 0.0f;
#endif
    return output;
}
