// AMD FidelityFX Super Resolution 3.1 upscaler, prepare inputs pass: dilates depth and motion vectors, reconstructs the previous frame's nearest depth and stores this frame's luma.
// AMD's ffx_fsr3upscaler_prepare_inputs_pass.hlsl (FidelityFX SDK v2.0.0, FSR 3.1.5, MIT licence,
// Copyright (c) 2025 Advanced Micro Devices, Inc.; see fsr3/LICENSE.md) on SDL GPU's compute bindings.
#include "fsr3/btbh_fsr3_options.hlsli"

#define FSR3UPSCALER_BIND_SRV_INPUT_MOTION_VECTORS 0
#define FSR3UPSCALER_BIND_SRV_INPUT_DEPTH 1
#define FSR3UPSCALER_BIND_SRV_INPUT_COLOR 2
#define FSR3UPSCALER_BIND_UAV_DILATED_MOTION_VECTORS 0
#define FSR3UPSCALER_BIND_UAV_DILATED_DEPTH 1
#define FSR3UPSCALER_BIND_UAV_RECONSTRUCTED_PREV_NEAREST_DEPTH 2
#define FSR3UPSCALER_BIND_UAV_FARTHEST_DEPTH 3
#define FSR3UPSCALER_BIND_UAV_CURRENT_LUMA 4
#define FSR3UPSCALER_BIND_CB_FSR3UPSCALER 0

#include "fsr3/btbh_fsr3_callbacks_hlsl.h"
#include "fsr3/fsr3upscaler/ffx_fsr3upscaler_common.h"
#include "fsr3/fsr3upscaler/ffx_fsr3upscaler_prepare_inputs.h"

[numthreads(8, 8, 1)]
void main(int2 iDispatchThreadId : SV_DispatchThreadID)
{
    PrepareInputs(iDispatchThreadId);
}
