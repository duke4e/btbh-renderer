// AMD FidelityFX Super Resolution 3.1 upscaler, shading change pass: turns the shading-change pyramid into one half-resolution change mask.
// AMD's ffx_fsr3upscaler_shading_change_pass.hlsl (FidelityFX SDK v2.0.0, FSR 3.1.5, MIT licence,
// Copyright (c) 2025 Advanced Micro Devices, Inc.; see fsr3/LICENSE.md) on SDL GPU's compute bindings.
#include "fsr3/btbh_fsr3_options.hlsli"

#define FSR3UPSCALER_BIND_SRV_SPD_MIPS 0
#define FSR3UPSCALER_BIND_UAV_SHADING_CHANGE 0
#define FSR3UPSCALER_BIND_CB_FSR3UPSCALER 0

#include "fsr3/btbh_fsr3_callbacks_hlsl.h"
#include "fsr3/fsr3upscaler/ffx_fsr3upscaler_common.h"
#include "fsr3/fsr3upscaler/ffx_fsr3upscaler_shading_change.h"

[numthreads(8, 8, 1)]
void main(int2 iDispatchThreadId : SV_DispatchThreadID)
{
    ShadingChange(iDispatchThreadId);
}
