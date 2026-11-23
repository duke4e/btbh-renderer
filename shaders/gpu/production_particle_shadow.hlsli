// RFC 0013 phase 2 (0.14.0): reading the particle transmittance map (BTBHGPUSceneLighting.particle_shadow), which
// production_particle_transmittance.frag writes in sun space with the sun map's matrix. A texel holds one slab: r the
// summed optical depth, g the sum of optical depth times sun-space depth, a the nearest depth (1 where nothing was
// drawn); b is unused. Included by production_particle_lit.frag (particle_shadow 1 and 2) and, in its shadowed
// variants, production_model.frag (2). src/gpu/btbh_gpu_particle.h holds the C copy of the slab's transmittance.
#ifndef BTBH_PARTICLE_SHADOW_HLSLI
#define BTBH_PARTICLE_SHADOW_HLSLI

// The share of the sun a slab leaves at sun-space depth `depth`: all of it before the front, exp(-r) past the back,
// the coverage taken as even between them. The back is twice the mean depth less the front; the front is the nearest
// depth, but no deeper than the mean (where the filtered texel blends a column's edge with empty texels, whose depth
// is 1, the nearest depth slides deep while the mean stays: the slab then shrinks to the mean).
float btbh_particle_slab_transmittance(float4 slab, float depth)
{
    const float optical = max(slab.r, 0.0f);
    const float mean = slab.g / max(optical, 1.0e-6f);
    const float front = min(slab.a, mean);
    const float span = 2.0f * (mean - front);
    const float share = span > 1.0e-6f
        ? saturate((depth - front) / span)
        : (depth >= front ? 1.0f : 0.0f);
    return exp(-optical * share);
}

// The map's transmittance at a flat world position: one bilinear fetch at its place in the sun's map; 1 outside it.
float btbh_particle_transmittance(
    Texture2D<float4> transmittance_map,
    SamplerState transmittance_sampler,
    float4x4 sun_view_projection,
    float3 world_position
)
{
    const float4 clip = mul(sun_view_projection, float4(world_position, 1.0f));
    if (clip.w <= 0.0f)
    {
        return 1.0f;
    }
    const float3 projected = clip.xyz / clip.w;
    if (
        projected.x < -1.0f || projected.x > 1.0f ||
        projected.y < -1.0f || projected.y > 1.0f ||
        projected.z < 0.0f || projected.z > 1.0f
    )
    {
        return 1.0f;
    }
    const float2 uv = float2(projected.x * 0.5f + 0.5f, -projected.y * 0.5f + 0.5f);
    return btbh_particle_slab_transmittance(
        transmittance_map.SampleLevel(transmittance_sampler, uv, 0.0f),
        projected.z
    );
}

#endif
