// RFC 0013 phase 2 (0.14.0): a fragment of a batch with BTBH_GPU_SCENE_BATCH_PARTICLE_SHADOW in the particle
// transmittance map (one RGBA16F texture in sun space, cleared to (0, 0, 0, 1)). Its alpha becomes optical depth
// tau = -ln(1 - min(alpha, 0.999)) times the density; it writes (tau, tau * depth, 0, depth), with the colour blended
// ADD (one, one) and the alpha MIN, so that each texel ends with the summed optical depth, its depth-weighted sum and
// the nearest depth, whatever the order (production_particle_shadow.hlsli reads them as one slab). No depth target:
// what hides a puff from the sun shadows its shadow as well. src/gpu/btbh_gpu_particle.h holds the C copy.

Texture2D<float4> albedo_texture : register(t0, space2);
SamplerState albedo_sampler : register(s0, space2);

cbuffer TransmittanceMaterial : register(b0, space3)
{
    // x the optical depth's scale (a lit particle's surface_parameters1.w, 0 taken as 1; 1 for the others), y the
    // batch's alpha_cutoff, zw 0
    float4 transmittance_controls;
};

struct FragmentInput
{
    float4 pixel_position : SV_Position;
    float4 color : TEXCOORD0;
    float2 texture_coordinate : TEXCOORD1;
};

float4 main(FragmentInput input) : SV_Target0
{
    const float alpha = albedo_texture.Sample(albedo_sampler, input.texture_coordinate).a * input.color.a;
    // (A fragment at 1/255 or less would set the nearest depth for almost no optical depth.)
    if (alpha <= 1.0f / 255.0f || alpha < transmittance_controls.y)
    {
        discard;
    }
    const float optical = -log(1.0f - min(alpha, 0.999f)) * transmittance_controls.x;
    const float depth = input.pixel_position.z;
    return float4(optical, optical * depth, 0.0f, depth);
}
