// AMD FidelityFX Super Resolution 3.1 upscaler, prepare reactivity pass: combines the reactive mask, disocclusion and shading change, and updates the accumulation and new locks.
// AMD's ffx_fsr3upscaler_prepare_reactivity_pass.hlsl (FidelityFX SDK v2.0.0, FSR 3.1.5, MIT licence,
// Copyright (c) 2025 Advanced Micro Devices, Inc.; see fsr3/LICENSE.md) on SDL GPU's compute bindings.
#include "fsr3/btbh_fsr3_options.hlsli"

#define FSR3UPSCALER_BIND_SRV_RECONSTRUCTED_PREV_NEAREST_DEPTH 0
#define FSR3UPSCALER_BIND_SRV_DILATED_MOTION_VECTORS 1
#define FSR3UPSCALER_BIND_SRV_DILATED_DEPTH 2
#define FSR3UPSCALER_BIND_SRV_REACTIVE_MASK 3
#define FSR3UPSCALER_BIND_SRV_TRANSPARENCY_AND_COMPOSITION_MASK 4
#define FSR3UPSCALER_BIND_SRV_ACCUMULATION 5
#define FSR3UPSCALER_BIND_SRV_SHADING_CHANGE 6
#define FSR3UPSCALER_BIND_SRV_CURRENT_LUMA 7
#define FSR3UPSCALER_BIND_SRV_INPUT_EXPOSURE 8
#define FSR3UPSCALER_BIND_UAV_DILATED_REACTIVE_MASKS 0
#define FSR3UPSCALER_BIND_UAV_NEW_LOCKS 1
#define FSR3UPSCALER_BIND_UAV_ACCUMULATION 2
#define FSR3UPSCALER_BIND_CB_FSR3UPSCALER 0

#include "fsr3/btbh_fsr3_callbacks_hlsl.h"
#include "fsr3/fsr3upscaler/ffx_fsr3upscaler_common.h"
#include "fsr3/fsr3upscaler/ffx_fsr3upscaler_prepare_reactivity.h"

[numthreads(8, 8, 1)]
void main(int2 iDispatchThreadId : SV_DispatchThreadID)
{
    PrepareReactivity(iDispatchThreadId);
}
