// AMD FidelityFX Super Resolution 3.1 upscaler, luma instability pass: measures how much each pixel's luma flickers across the last frames.
// AMD's ffx_fsr3upscaler_luma_instability_pass.hlsl (FidelityFX SDK v2.0.0, FSR 3.1.5, MIT licence,
// Copyright (c) 2025 Advanced Micro Devices, Inc.; see fsr3/LICENSE.md) on SDL GPU's compute bindings.
#include "fsr3/btbh_fsr3_options.hlsli"

#define FSR3UPSCALER_BIND_SRV_INPUT_EXPOSURE 0
#define FSR3UPSCALER_BIND_SRV_DILATED_REACTIVE_MASKS 1
#define FSR3UPSCALER_BIND_SRV_DILATED_MOTION_VECTORS 2
#define FSR3UPSCALER_BIND_SRV_FRAME_INFO 3
#define FSR3UPSCALER_BIND_SRV_LUMA_HISTORY 4
#define FSR3UPSCALER_BIND_SRV_FARTHEST_DEPTH_MIP1 5
#define FSR3UPSCALER_BIND_SRV_CURRENT_LUMA 6
#define FSR3UPSCALER_BIND_UAV_LUMA_HISTORY 0
#define FSR3UPSCALER_BIND_UAV_LUMA_INSTABILITY 1
#define FSR3UPSCALER_BIND_CB_FSR3UPSCALER 0

#include "fsr3/btbh_fsr3_callbacks_hlsl.h"
#include "fsr3/fsr3upscaler/ffx_fsr3upscaler_common.h"
#include "fsr3/fsr3upscaler/ffx_fsr3upscaler_luma_instability.h"

[numthreads(8, 8, 1)]
void main(int2 iDispatchThreadId : SV_DispatchThreadID)
{
    LumaInstability(iDispatchThreadId);
}
