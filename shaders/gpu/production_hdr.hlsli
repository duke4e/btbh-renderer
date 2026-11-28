#ifndef BTBH_PRODUCTION_HDR_HLSLI
#define BTBH_PRODUCTION_HDR_HLSLI

// HDR output (RFC 0017). The CPU reference is src/gpu/btbh_gpu_hdr.h; keep the two the same.
// hdr_tone: x the highlight ratio P (peak over paper white, >= 1), y the knee m_k (exposed scene units), z the
// shoulder tau, w the tail's peak P^(1/2.2). hdr_misc: x 1 to write the scene's heat into alpha (BTBH_GPU_HDR_DEBUG).

static const float BTBH_HDR_DISPLAY_GAMMA = 2.2f;

// The scene's heat: exposure times its largest channel, before the tone map.
float btbh_hdr_heat(float3 scene_value, float exposure_value)
{
    return max(exposure_value * max(scene_value.r, max(scene_value.g, scene_value.b)), 0.0f);
}

// 1 at and below the knee (and C1 there), rising monotonically toward P.
float btbh_hdr_gain(float heat, float4 tone)
{
    const float t = max(heat - tone.y, 0.0f) / max(tone.z, 1.0e-6f);
    const float t2 = t * t;
    return 1.0f + (tone.x - 1.0f) * (t2 / (1.0f + t2));
}

// An sRGB-encoded SDR value lifted by `gain` in display light (gamma 2.2).
float3 btbh_hdr_lift(float3 encoded, float gain)
{
    return encoded * pow(gain, 1.0f / BTBH_HDR_DISPLAY_GAMMA);
}

#endif
