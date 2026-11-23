// RFC 0012: the vertex-stage height lift (BTBHGPUSceneHeightLift), included by production_model.vert.hlsl under
// BTBH_HEIGHT_LIFT (production_model_lift.vert, production_model_detail_table_lift.vert). The C copy is
// src/gpu/btbh_gpu_height_lift.h (btbh_gpu_height_lift_at, btbh_gpu_height_lift_apply).
//
// The heightfield is the vertex stage's one sampled texture, so it takes t0 / s0 in space 0 and the storage buffers
// (instances, motions) follow it at t1 and t2 (SDL's order: sampled textures, then storage buffers); the core binds
// the storage buffers by slot as for every static batch. The block is the vertex stage's second uniform buffer.
#ifndef PRODUCTION_HEIGHT_LIFT_HLSLI
#define PRODUCTION_HEIGHT_LIFT_HLSLI

// RGBA16F: the height, its slope along u and along v in heights per sample, 1. Bilinear, clamped at the edges.
Texture2D<float4> height_lift_field : register(t0, space0);
SamplerState height_lift_sampler : register(s0, space0);

cbuffer HeightLift : register(b1, space1)
{
    // u scale, v scale, u offset, v offset (BTBHGPUSceneHeightLift.world_to_uv).
    float4 height_lift_uv;
    // x the height scale, y the offset; z and w turn a sample's slope into dh/dx and dh/dz in world units (0 with
    // BTBH_GPU_SCENE_HEIGHT_LIFT_KEEP_NORMALS, so the tilt adds nothing).
    float4 height_lift_scale;
};

// x the lift at a flat world (x, z); yz its slope dh/dx, dh/dz in world units. No derivatives in the vertex stage:
// level 0.
float3 btbh_height_lift(float2 world_xz)
{
    const float2 uv = world_xz * height_lift_uv.xy + height_lift_uv.zw;
    const float3 h = height_lift_field.SampleLevel(height_lift_sampler, uv, 0.0f).xyz;
    return float3(h.x * height_lift_scale.x + height_lift_scale.y, h.yz * height_lift_scale.zw);
}

#endif
