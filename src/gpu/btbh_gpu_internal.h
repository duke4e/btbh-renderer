#ifndef BTBH_GPU_INTERNAL_H
#define BTBH_GPU_INTERNAL_H

#include "btbh_gpu/btbh_gpu.h"

#include <SDL3/SDL.h>

#include <stdbool.h>

typedef struct BTBHGPUModelPrimitive {
    SDL_GPUBuffer *vertex_buffer;
    SDL_GPUBuffer *index_buffer;
    uint32_t vertex_count;
    uint32_t index_count;
    int32_t material_slot;
} BTBHGPUModelPrimitive;

typedef struct BTBHGPUSceneRuntime BTBHGPUSceneRuntime;

struct BTBHGPUDevice {
    SDL_GPUDevice *gpu;
    bool owns_video_subsystem;
    bool shutdown;
};

struct BTBHGPUWindow {
    SDL_Window *window;
    BTBHGPUDevice *device;
    BTBHGPUTextureFormat swapchain_format;
    BTBHGPUPresentMode present_mode;
    BTBHGPUSwapchainComposition composition;
    bool owns_video_subsystem;
    bool claimed;
    bool should_close;
    bool cursor_visible;
    uint64_t last_frame_ticks;
    float mouse_delta_x;
    float mouse_delta_y;
    float mouse_wheel_x;
    float mouse_wheel_y;
    bool key_pressed[SDL_SCANCODE_COUNT];
    bool mouse_pressed[4];
    bool mouse_released[4];
    BTBHGPUTexture *texture_head;
    BTBHGPUModel *model_head;
    BTBHGPUInstanceBuffer *instance_buffer_head;
    BTBHGPUCustomMesh *custom_mesh_head;
    BTBHGPUSceneRuntime *scene_runtime;
};

struct BTBHGPUModel {
    BTBHGPUWindow *owner;
    BTBHGPUModel *previous;
    BTBHGPUModel *next;
    BTBHGPUModelPrimitive *primitives;
    uint32_t primitive_count;
    uint32_t material_count;
    float minimum[3];
    float maximum[3];
};

struct BTBHGPUCustomMesh {
    BTBHGPUWindow *owner;
    BTBHGPUCustomMesh *previous;
    BTBHGPUCustomMesh *next;
    SDL_GPUBuffer *vertex_buffer;
    SDL_GPUBuffer *index_buffer;
    uint32_t vertex_count;
    uint32_t index_count;
    uint32_t vertex_byte_count;
};

void btbh_gpu_window_release_textures(BTBHGPUWindow *window);
void btbh_gpu_window_release_models(BTBHGPUWindow *window);
void btbh_gpu_window_release_instance_buffers(BTBHGPUWindow *window);
void btbh_gpu_window_release_custom_meshes(BTBHGPUWindow *window);
void btbh_gpu_window_release_scene(BTBHGPUWindow *window);

#endif
