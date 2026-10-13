// AMD FidelityFX Super Resolution 3.1 upscaler, accumulate pass: upsamples this frame and blends it into the reprojected history at the output resolution.
// AMD's ffx_fsr3upscaler_accumulate_pass.hlsl (FidelityFX SDK v2.0.0, FSR 3.1.5, MIT licence,
// Copyright (c) 2025 Advanced Micro Devices, Inc.; see fsr3/LICENSE.md) on SDL GPU's compute bindings.
#include "fsr3/btbh_fsr3_options.hlsli"

#define FSR3UPSCALER_BIND_SRV_INPUT_EXPOSURE 0
#define FSR3UPSCALER_BIND_SRV_DILATED_REACTIVE_MASKS 1
#define FSR3UPSCALER_BIND_SRV_DILATED_MOTION_VECTORS 2
#define FSR3UPSCALER_BIND_SRV_INTERNAL_UPSCALED 3
#define FSR3UPSCALER_BIND_SRV_FARTHEST_DEPTH_MIP1 4
#define FSR3UPSCALER_BIND_SRV_CURRENT_LUMA 5
#define FSR3UPSCALER_BIND_SRV_LUMA_INSTABILITY 6
#define FSR3UPSCALER_BIND_SRV_INPUT_COLOR 7
#define FSR3UPSCALER_BIND_UAV_INTERNAL_UPSCALED 0
#define FSR3UPSCALER_BIND_UAV_UPSCALED_OUTPUT 1
#define FSR3UPSCALER_BIND_UAV_NEW_LOCKS 2
#define FSR3UPSCALER_BIND_CB_FSR3UPSCALER 0

#include "fsr3/btbh_fsr3_callbacks_hlsl.h"
#include "fsr3/fsr3upscaler/ffx_fsr3upscaler_common.h"
#include "fsr3/fsr3upscaler/ffx_fsr3upscaler_sample.h"
#include "fsr3/fsr3upscaler/ffx_fsr3upscaler_upsample.h"
#include "fsr3/fsr3upscaler/ffx_fsr3upscaler_reproject.h"
#include "fsr3/fsr3upscaler/ffx_fsr3upscaler_accumulate.h"

[numthreads(8, 8, 1)]
void main(int2 iDispatchThreadId : SV_DispatchThreadID)
{
    Accumulate(iDispatchThreadId);
}
