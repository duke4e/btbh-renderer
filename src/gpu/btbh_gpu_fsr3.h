#ifndef BTBH_GPU_FSR3_H
#define BTBH_GPU_FSR3_H

/* AMD FidelityFX Super Resolution 3.1 temporal upscaler (FidelityFX SDK v2.0.0, FSR 3.1.5) driven as
   the renderer's own SDL GPU compute passes: the SDK itself needs native D3D12 or Vulkan command
   lists, so its HLSL (shaders/gpu/fsr3) runs here with the dispatch sequence of AMD's
   ffx_fsr3upscaler.cpp. No frame generation. */

#include "btbh_gpu/btbh_gpu.h"

#include <SDL3/SDL.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum BTBHGPUFSR3Pipeline {
    BTBH_GPU_FSR3_PIPELINE_PREPARE_INPUTS = 0,
    BTBH_GPU_FSR3_PIPELINE_LUMA_PYRAMID,
    BTBH_GPU_FSR3_PIPELINE_SHADING_CHANGE_PYRAMID,
    BTBH_GPU_FSR3_PIPELINE_SHADING_CHANGE,
    BTBH_GPU_FSR3_PIPELINE_PREPARE_REACTIVITY,
    BTBH_GPU_FSR3_PIPELINE_LUMA_INSTABILITY,
    BTBH_GPU_FSR3_PIPELINE_ACCUMULATE,
    BTBH_GPU_FSR3_PIPELINE_DEBUG_VIEW,
    BTBH_GPU_FSR3_PIPELINE_CLEAR,
    BTBH_GPU_FSR3_PIPELINE_MOTION_VIEW,
    BTBH_GPU_FSR3_PIPELINE_COUNT
} BTBHGPUFSR3Pipeline;

typedef enum BTBHGPUFSR3DebugView {
    BTBH_GPU_FSR3_DEBUG_NONE = 0,
    /* The frame under its motion vectors and reactive mask (production_fsr3_motion_view). */
    BTBH_GPU_FSR3_DEBUG_MOTION_VECTORS = 1,
    /* AMD's debug view of the upscaler's own buffers. */
    BTBH_GPU_FSR3_DEBUG_UPSCALER = 2,
    /* How far each pixel is from the previous frame's colour where its motion vector points. */
    BTBH_GPU_FSR3_DEBUG_MOTION_ERROR = 3
} BTBHGPUFSR3DebugView;

/* cbFSR3Upscaler in fsr3/btbh_fsr3_callbacks_hlsl.h (AMD's Fsr3UpscalerConstants). */
typedef struct BTBHGPUFSR3Constants {
    int32_t render_size[2];
    int32_t previous_frame_render_size[2];
    int32_t upscale_size[2];
    int32_t previous_frame_upscale_size[2];
    int32_t max_render_size[2];
    int32_t max_upscale_size[2];
    float device_to_view_depth[4];
    float jitter_offset[2];
    float previous_frame_jitter_offset[2];
    float motion_vector_scale[2];
    float downscale_factor[2];
    float motion_vector_jitter_cancellation[2];
    float tan_half_fov;
    float jitter_phase_count;
    float delta_time;
    float delta_pre_exposure;
    float view_space_to_meters_factor;
    float frame_index;
    float velocity_factor;
    float reactiveness_scale;
    float shading_change_scale;
    float accumulation_added_per_frame;
    float min_disocclusion_accumulation;
    float padding[3];
} BTBHGPUFSR3Constants;

typedef struct BTBHGPUFSR3Runtime {
    SDL_GPUComputePipeline *pipelines[BTBH_GPU_FSR3_PIPELINE_COUNT];
    SDL_GPUSampler *point_sampler;
    SDL_GPUSampler *linear_sampler;
    /* Render-resolution resources, allocated at render_capacity so dynamic resolution below it
       keeps the history (FSR's MaxRenderSize). */
    SDL_GPUTexture *dilated_depth;
    SDL_GPUTexture *dilated_motion_vectors;
    SDL_GPUTexture *reconstructed_previous_depth;
    /* Farthest depth in prepare inputs, then luma instability (FSR's INTERMEDIATE_FP16x1). */
    SDL_GPUTexture *intermediate;
    SDL_GPUTexture *luma[2];
    SDL_GPUTexture *accumulation[2];
    SDL_GPUTexture *luma_history[2];
    SDL_GPUTexture *dilated_reactive_masks;
    /* Half the render capacity. */
    SDL_GPUTexture *shading_change;
    SDL_GPUTexture *spd_mips;
    SDL_GPUTexture *farthest_depth_mip1;
    uint32_t spd_levels;
    uint32_t render_capacity_width;
    uint32_t render_capacity_height;
    /* Output-resolution resources. */
    SDL_GPUTexture *internal_upscaled[2];
    SDL_GPUTexture *new_locks;
    SDL_GPUTexture *output;
    uint32_t output_width;
    uint32_t output_height;
    /* One texel. */
    SDL_GPUTexture *spd_atomic;
    SDL_GPUTexture *frame_info;
    SDL_GPUTexture *default_exposure;
    SDL_GPUTexture *default_transparency;
    BTBHGPUFSR3Constants constants;
    bool resources_need_clear;
    bool history_valid;
    bool unavailable;
    bool logged_failure;
    uint32_t resource_frame_index;
    uint32_t jitter_index;
    float jitter_x;
    float jitter_y;
} BTBHGPUFSR3Runtime;

typedef struct BTBHGPUFSR3Inputs {
    SDL_GPUTexture *color;
    SDL_GPUTexture *depth;
    SDL_GPUTexture *motion_vectors;
    SDL_GPUTexture *reactive;
    /* The previous frame's colour, for BTBH_GPU_FSR3_DEBUG_MOTION_ERROR. */
    SDL_GPUTexture *previous_color;
    uint32_t render_width;
    uint32_t render_height;
    uint32_t output_width;
    uint32_t output_height;
    float camera_near;
    float camera_far;
    float vertical_fov_radians;
    float frame_seconds;
    float view_space_to_meters;
    /* Pixels of motion that light the motion-vector debug view fully. */
    float debug_motion_pixels;
    bool reset;
    BTBHGPUFSR3DebugView debug_view;
} BTBHGPUFSR3Inputs;

/* Loads the pipelines (once) and sizes the resources for this frame. False when the upscaler
   cannot run on this device; message says why. */
bool btbh_gpu_fsr3_prepare(
    SDL_GPUDevice *device,
    BTBHGPUFSR3Runtime *runtime,
    const char *shader_reference_path,
    BTBHGPUShaderFormat shader_format,
    uint32_t render_width,
    uint32_t render_height,
    uint32_t output_width,
    uint32_t output_height,
    char *message,
    size_t message_capacity
);

/* Advances the jitter sequence and returns this frame's sub-pixel offset in render pixels (x right,
   y down); the projection takes (2 x / width, -2 y / height) in clip space. */
void btbh_gpu_fsr3_next_jitter(
    BTBHGPUFSR3Runtime *runtime,
    uint32_t render_width,
    uint32_t output_width,
    float *jitter_x,
    float *jitter_y
);

/* Encodes the upscaler's passes for this frame into runtime->output (RGBA16F, the output size). */
bool btbh_gpu_fsr3_encode(
    SDL_GPUCommandBuffer *command,
    BTBHGPUFSR3Runtime *runtime,
    const BTBHGPUFSR3Inputs *inputs
);

/* The next frame starts a new history. */
void btbh_gpu_fsr3_invalidate_history(BTBHGPUFSR3Runtime *runtime);

void btbh_gpu_fsr3_release(
    SDL_GPUDevice *device,
    BTBHGPUFSR3Runtime *runtime
);

/* AMD's ffxFsr3UpscalerGetJitterPhaseCount: 8 times the square of the upscale ratio. */
int32_t btbh_gpu_fsr3_jitter_phase_count(
    uint32_t render_width,
    uint32_t output_width
);

/* AMD's Halton(2, 3) jitter, -0.5..0.5 render pixels, for index modulo phase_count. */
void btbh_gpu_fsr3_jitter_offset(
    int32_t index,
    int32_t phase_count,
    float *jitter_x,
    float *jitter_y
);

#endif
