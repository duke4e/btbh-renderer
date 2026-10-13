// AMD FidelityFX Super Resolution 3.1 upscaler, luma pyramid pass: reduces luma and farthest depth (single-pass downsampler) and updates the frame info (exposure, average luma).
// AMD's ffx_fsr3upscaler_luma_pyramid_pass.hlsl (FidelityFX SDK v2.0.0, FSR 3.1.5, MIT licence,
// Copyright (c) 2025 Advanced Micro Devices, Inc.; see fsr3/LICENSE.md) on SDL GPU's compute bindings.
#include "fsr3/btbh_fsr3_options.hlsli"

#define FSR3UPSCALER_BIND_SRV_CURRENT_LUMA 0
#define FSR3UPSCALER_BIND_SRV_FARTHEST_DEPTH 1
#define FSR3UPSCALER_BIND_UAV_SPD_GLOBAL_ATOMIC 0
#define FSR3UPSCALER_BIND_UAV_FRAME_INFO 1
#define FSR3UPSCALER_BIND_UAV_SPD_MIPS_LEVEL_5 2
#define FSR3UPSCALER_BIND_UAV_FARTHEST_DEPTH_MIP1 3
#define FSR3UPSCALER_BIND_CB_FSR3UPSCALER 0
#define FSR3UPSCALER_BIND_CB_SPD 1

#include "fsr3/btbh_fsr3_callbacks_hlsl.h"
#include "fsr3/fsr3upscaler/ffx_fsr3upscaler_common.h"
#include "fsr3/fsr3upscaler/ffx_fsr3upscaler_luma_pyramid.h"

[numthreads(256, 1, 1)]
void main(uint3 WorkGroupId : SV_GroupID, uint LocalThreadIndex : SV_GroupIndex)
{
    ComputeAutoExposure(WorkGroupId, LocalThreadIndex);
}
