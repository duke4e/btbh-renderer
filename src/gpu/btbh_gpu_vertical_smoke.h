#ifndef BTBH_GPU_VERTICAL_SMOKE_H
#define BTBH_GPU_VERTICAL_SMOKE_H

#include "btbh_gpu/btbh_gpu.h"

#include <stdint.h>

typedef struct BTBHGPUVerticalSmokeConfig {
    const char *vertex_shader_path;
    const char *fragment_shader_path;
    const char *compute_shader_path;
    const char *shader_entrypoint;
    const char *capture_path;
    BTBHGPUBackendRequest backend;
    BTBHGPUShaderFormat shader_format;
    uint32_t width;
    uint32_t height;
} BTBHGPUVerticalSmokeConfig;

typedef struct BTBHGPUVerticalSmokeReport {
    BTBHGPUResult result;
    uint32_t width;
    uint32_t height;
    uint8_t center_rgba[4];
    float compute_center_rgba[4];
    char driver_name[BTBH_GPU_DRIVER_NAME_CAPACITY];
    char message[256];
} BTBHGPUVerticalSmokeReport;

int btbh_gpu_run_vertical_smoke(
    const BTBHGPUVerticalSmokeConfig *config,
    BTBHGPUVerticalSmokeReport *report
);

#endif
