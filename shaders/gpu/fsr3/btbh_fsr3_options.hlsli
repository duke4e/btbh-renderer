// FSR 3.1 upscaler compile options for the renderer's passes: the permutation AMD's build script
// (BuildFSR3UpscalerShaders.bat, FidelityFX SDK v2.0.0) compiles as the 32-bit, wave32 variant, with
// LDR colour input (the tonemapped frame), motion vectors at the render resolution without jitter,
// standard depth (1 is far), the reference reprojection filter and no built-in sharpening (the
// renderer's own RCAS pass runs after it). SDL_shadercross compiles compute shaders as cs_6_0, so the
// SPD reductions use group-shared memory instead of wave operations.
#ifndef BTBH_FSR3_OPTIONS_HLSLI
#define BTBH_FSR3_OPTIONS_HLSLI

#define FFX_GPU 1
#define FFX_HLSL 1
#define FFX_HALF 0
#define FFX_HLSL_SM 60
#define FFX_IMPLICIT_SHADER_REGISTER_BINDING_HLSL 0
#define FFX_FSR3UPSCALER_EMBED_ROOTSIG 0
#define FFX_SPD_NO_WAVE_OPERATIONS 1

#define FFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF 0
#define FFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF 0
#define FFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF 1
#define FFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF 0
#define FFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE 2
#define FFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE 0
#define FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT 0
#define FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS 1
#define FFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS 0
#define FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH 0
#define FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING 0

#endif
