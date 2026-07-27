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
    int32_t skin_index;
} BTBHGPUModelPrimitive;

typedef struct BTBHGPUSceneRuntime BTBHGPUSceneRuntime;
typedef struct BTBHGPUCanvasRuntime BTBHGPUCanvasRuntime;

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
    BTBHGPUFont *font_head;
    BTBHGPUModel *model_head;
    BTBHGPUInstanceBuffer *instance_buffer_head;
    BTBHGPUCustomMesh *custom_mesh_head;
    BTBHGPUSceneRuntime *scene_runtime;
    BTBHGPUCanvasRuntime *canvas_runtime;
    BTBHGPUTexture *presented_frame;
    bool presented_frame_readback_requested;
    SDL_GPUCommandBuffer *pending_command;
    SDL_GPUTexture *pending_swapchain;
    SDL_GPUTransferBuffer *pending_scene_transfer;
    SDL_GPUBuffer *pending_instance_buffer;
    SDL_GPUBuffer *pending_bone_buffer;
    uint32_t pending_drawable_width;
    uint32_t pending_drawable_height;
};

struct BTBHGPUTexture {
    BTBHGPUWindow *owner;
    BTBHGPUTexture *previous;
    BTBHGPUTexture *next;
    SDL_GPUTexture *texture;
    SDL_GPUSampler *sampler;
    SDL_GPUTextureFormat format;
    uint32_t width;
    uint32_t height;
    uint32_t mip_count;
};

struct BTBHGPUModel {
    BTBHGPUWindow *owner;
    BTBHGPUModel *previous;
    BTBHGPUModel *next;
    BTBHGPUModelPrimitive *primitives;
    BTBHGPUModelMaterialInfo *materials;
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
void btbh_gpu_window_release_fonts(BTBHGPUWindow *window);
void btbh_gpu_window_release_models(BTBHGPUWindow *window);
void btbh_gpu_window_release_instance_buffers(BTBHGPUWindow *window);
void btbh_gpu_window_release_custom_meshes(BTBHGPUWindow *window);
void btbh_gpu_window_release_scene(BTBHGPUWindow *window);
void btbh_gpu_window_release_canvas(BTBHGPUWindow *window);
bool btbh_gpu_window_finish_pending_frame(
    BTBHGPUWindow *window,
    bool submit
);
bool btbh_gpu_window_record_presented_frame(
    BTBHGPUWindow *window
);
uint32_t btbh_gpu_animation_library_skin_joint_count(
    const BTBHGPUAnimationLibrary *library,
    uint32_t skin_index
);
bool btbh_gpu_animation_library_sample_skin(
    const BTBHGPUAnimationLibrary *library,
    uint32_t animation_index,
    float time,
    uint32_t skin_index,
    float *matrices,
    uint32_t matrix_capacity
);
BTBHGPUTexture *btbh_gpu_window_texture_create_rgba8(
    BTBHGPUWindow *window,
    const uint8_t *pixels,
    uint32_t width,
    uint32_t height,
    BTBHGPUTextureFilter filter,
    BTBHGPUTextureWrap wrap,
    BTBHGPUTextureReport *report
);
BTBHGPUTexture *btbh_gpu_window_texture_load_memory(
    BTBHGPUWindow *window,
    const uint8_t *encoded,
    size_t encoded_size,
    BTBHGPUTextureFilter filter,
    BTBHGPUTextureWrap wrap,
    BTBHGPUTextureReport *report
);
BTBHGPUTexture *btbh_gpu_window_texture_create_empty(
    BTBHGPUWindow *window,
    uint32_t width,
    uint32_t height,
    SDL_GPUTextureFormat format,
    SDL_GPUTextureUsageFlags usage,
    BTBHGPUTextureFilter filter,
    BTBHGPUTextureWrap wrap,
    BTBHGPUTextureReport *report
);

#endif
