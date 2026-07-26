#ifndef BTBH_GPU_INTERNAL_H
#define BTBH_GPU_INTERNAL_H

#include "btbh_gpu/btbh_gpu.h"

#include <SDL3/SDL.h>

#include <stdbool.h>

struct BTBHGPUDevice {
    SDL_GPUDevice *gpu;
    bool owns_video_subsystem;
    bool shutdown;
};

#endif
