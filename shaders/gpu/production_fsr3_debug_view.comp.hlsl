// AMD FidelityFX Super Resolution 3.1 upscaler, debug view pass: draws AMD's debug view (dilated motion, depth, reactive masks, history) over the output.
// AMD's ffx_fsr3upscaler_debug_view_pass.hlsl (FidelityFX SDK v2.0.0, FSR 3.1.5, MIT licence,
// Copyright (c) 2025 Advanced Micro Devices, Inc.; see fsr3/LICENSE.md) on SDL GPU's compute bindings.
#include "fsr3/btbh_fsr3_options.hlsli"

#define FSR3UPSCALER_BIND_SRV_DILATED_REACTIVE_MASKS 0
#define FSR3UPSCALER_BIND_SRV_DILATED_MOTION_VECTORS 1
#define FSR3UPSCALER_BIND_SRV_DILATED_DEPTH 2
#define FSR3UPSCALER_BIND_SRV_INTERNAL_UPSCALED 3
#define FSR3UPSCALER_BIND_SRV_INPUT_EXPOSURE 4
#define FSR3UPSCALER_BIND_UAV_UPSCALED_OUTPUT 0
#define FSR3UPSCALER_BIND_CB_FSR3UPSCALER 0

#include "fsr3/btbh_fsr3_callbacks_hlsl.h"
#include "fsr3/fsr3upscaler/ffx_fsr3upscaler_common.h"
#include "fsr3/fsr3upscaler/ffx_fsr3upscaler_debug_view.h"

[numthreads(8, 8, 1)]
void main(int2 iPxPos : SV_DispatchThreadID)
{
    DebugView(iPxPos);
}
