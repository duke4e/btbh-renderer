#ifndef BTBH_GPU_PHASE2_RUNTIME_H
#define BTBH_GPU_PHASE2_RUNTIME_H

#include "btbh_gpu/btbh_gpu.h"

#include <stdint.h>

typedef struct BTBHGPUPhase2Config {
    const char *shadow_vertex_path;
    const char *shadow_fragment_path;
    const char *gbuffer_vertex_path;
    const char *gbuffer_fragment_path;
    const char *pattern_compute_path;
    const char *interval_compute_path;
    const char *lighting_vertex_path;
    const char *lighting_fragment_path;
    const char *transparent_fragment_path;
    const char *shader_entrypoint;
    const char *capture_path;
    BTBHGPUBackendRequest backend;
    BTBHGPUShaderFormat shader_format;
    uint32_t width;
    uint32_t height;
} BTBHGPUPhase2Config;

typedef struct BTBHGPUPhase2Report {
    BTBHGPUResult result;
    uint32_t width;
    uint32_t height;
    uint32_t executed_pass_count;
    uint64_t scene_signature;
    uint64_t final_image_checksum;
    uint8_t center_rgba[4];
    uint8_t skinned_rgba[4];
    float interval_center_rgba[4];
    char driver_name[BTBH_GPU_DRIVER_NAME_CAPACITY];
    char message[256];
} BTBHGPUPhase2Report;

int btbh_gpu_run_phase2(
    const BTBHGPUPhase2Config *config,
    BTBHGPUPhase2Report *report
);

#endif
