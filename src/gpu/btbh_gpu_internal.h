#ifndef BTBH_GPU_INTERNAL_H
#define BTBH_GPU_INTERNAL_H

#include "btbh_gpu_package_internal.h"

#include <SDL3/SDL.h>

#include <stdbool.h>

/* One buffer of a window's geometry arena: model primitives take ranges of it instead of buffers of their own,
   so a frame binds a handful of buffers, not one or two per primitive (SDL tracks every bound buffer with a scan
   of the buffers its command buffer used, 2026-10-18). Released when its last primitive goes. */
typedef struct BTBHGPUGeometryBlock {
    SDL_GPUBuffer *buffer;
    uint32_t capacity;
    uint32_t used;
    uint32_t live;
} BTBHGPUGeometryBlock;

typedef struct BTBHGPUGeometryArena {
    BTBHGPUGeometryBlock **blocks;
    uint32_t block_count;
    uint32_t block_capacity;
} BTBHGPUGeometryArena;

typedef struct BTBHGPUModelPrimitive {
    /* The arena blocks' buffers; the primitive's vertices start at vertex_base and its indices at first_index. */
    SDL_GPUBuffer *vertex_buffer;
    SDL_GPUBuffer *index_buffer;
    BTBHGPUGeometryBlock *vertex_block;
    BTBHGPUGeometryBlock *index_block;
    int32_t vertex_base;
    uint32_t first_index;
    float *positions;
    uint32_t *indices;
    uint32_t vertex_count;
    uint32_t index_count;
    int32_t material_slot;
    int32_t skin_index;
} BTBHGPUModelPrimitive;

typedef struct BTBHGPUSceneRuntime BTBHGPUSceneRuntime;
typedef struct BTBHGPUCanvasRuntime BTBHGPUCanvasRuntime;

#define BTBH_GPU_FRAME_TELEMETRY_CAPACITY 256u

typedef struct BTBHGPUFrameTelemetryPending {
    SDL_GPUFence *fence;
    uint64_t frame_serial;
    uint64_t submit_started_ticks;
    double cpu_submit_milliseconds;
} BTBHGPUFrameTelemetryPending;

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
    bool fullscreen_toggle;
    uint64_t last_frame_ticks;
    float mouse_delta_x;
    float mouse_delta_y;
    float mouse_wheel_x;
    float mouse_wheel_y;
    bool owns_gamepad_subsystem;
    SDL_Gamepad *gamepad;
    BTBHGPUGamepadSnapshot gamepad_snapshot;
    bool key_pressed[SDL_SCANCODE_COUNT];
    bool mouse_pressed[4];
    bool mouse_released[4];
    BTBHGPUTexture *texture_head;
    BTBHGPUFont *font_head;
    BTBHGPUModel *model_head;
    BTBHGPUGeometryArena vertex_arena;
    BTBHGPUGeometryArena index_arena;
    BTBHGPUInstanceBuffer *instance_buffer_head;
    BTBHGPUCustomMesh *custom_mesh_head;
    BTBHGPUSceneRuntime *scene_runtime;
    BTBHGPUSceneTelemetryReport scene_telemetry;
    uint32_t scene_shadow_update_period_frames[3];
    BTBHGPUCanvasRuntime *canvas_runtime;
    BTBHGPUTexture *presented_frame;
    bool presented_frame_readback_requested;
    SDL_GPUCommandBuffer *pending_command;
    SDL_GPUTexture *pending_swapchain;
    bool pending_scene_ready;
    SDL_GPUTransferBuffer *pending_scene_transfer;
    SDL_GPUBuffer *pending_instance_buffer;
    SDL_GPUBuffer *pending_bone_buffer;
    uint32_t pending_drawable_width;
    uint32_t pending_drawable_height;
    bool frame_telemetry_enabled;
    bool radiance_cascades_telemetry_enabled;
    uint64_t next_frame_telemetry_serial;
    BTBHGPUFrameTelemetryPending frame_telemetry_pending[
        BTBH_GPU_FRAME_TELEMETRY_CAPACITY
    ];
    uint32_t frame_telemetry_pending_count;
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
    /* The source file's last two path components, for `BTBH_GPU_COST_LOG`'s per-model lines. */
    char debug_name[96];
};

struct BTBHGPUInstanceBuffer {
    BTBHGPUWindow *owner;
    BTBHGPUInstanceBuffer *previous;
    BTBHGPUInstanceBuffer *next;
    SDL_GPUBuffer *buffer;
    SDL_GPUBuffer *vertex_buffer;
    uint32_t capacity;
    uint32_t stride;
    uint32_t byte_capacity;
    uint32_t uploaded_prefix_byte_count;
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

bool btbh_gpu_scene_uses_transparent_pass(
    BTBHGPUSceneBlendMode mode,
    bool transparency_enabled
);
bool btbh_gpu_scene_uses_fast_unlit_pipeline(
    const BTBHGPUSceneGeometryBatch *batch
);
/* A refracting batch (BTBH_GPU_SCENE_SURFACE_REFRACT) draws in the transparent pipeline with alpha blending
   and keeps surface_parameters1.w for the renderer; any other batch passes. */
bool btbh_gpu_scene_refraction_valid(
    const BTBHGPUSceneGeometryBatch *batch
);
bool btbh_gpu_scene_lighting_valid(
    const BTBHGPUSceneLighting *lighting
);
void btbh_gpu_scene_resolve_blend_state(
    BTBHGPUSceneBlendMode mode,
    bool transparency_enabled,
    SDL_GPUColorTargetBlendState *state
);
void btbh_gpu_animation_slerp_shortest(
    const float lower[4],
    const float upper[4],
    float amount,
    float output[4]
);
bool btbh_gpu_model_assign_bone_weight(
    float joints[4],
    float weights[4],
    uint32_t bone_index,
    float influence
);
void btbh_gpu_model_pack_gltf_bone_weights(
    float joints[4],
    float weights[4]
);
void btbh_gpu_model_normalize_bone_weights(
    float joints[4],
    float weights[4]
);
void btbh_gpu_window_release_textures(BTBHGPUWindow *window);
void btbh_gpu_window_release_fonts(BTBHGPUWindow *window);
void btbh_gpu_window_release_models(BTBHGPUWindow *window);
void btbh_gpu_window_release_instance_buffers(BTBHGPUWindow *window);
void btbh_gpu_window_release_custom_meshes(BTBHGPUWindow *window);
void btbh_gpu_window_release_scene(BTBHGPUWindow *window);
void btbh_gpu_window_release_canvas(BTBHGPUWindow *window);
uint32_t btbh_gpu_parse_allowed_frames_in_flight(const char *value);
bool btbh_gpu_window_finish_pending_frame(
    BTBHGPUWindow *window,
    bool submit
);
bool btbh_gpu_window_blit_pending_scene(
    BTBHGPUWindow *window,
    SDL_GPUCommandBuffer *command,
    SDL_GPUTexture *destination,
    uint32_t destination_width,
    uint32_t destination_height
);
void btbh_gpu_window_scene_submission_finished(
    BTBHGPUWindow *window,
    bool submitted
);
void btbh_gpu_window_release_geometry_arenas(BTBHGPUWindow *window);
void btbh_gpu_window_scene_record_frame_completion(
    BTBHGPUWindow *window,
    double completion_milliseconds
);
/* `BTBH_GPU_COST_LOG=1`, a measurement tool (2026-10-16): each submitted frame waits for the GPU and prints
   one `BTBH_GPU_COST` line with the GPU's milliseconds for it and the triangles and draws each pass sent, split
   into the static city (draws from a persistent instance buffer, which only the city keeps) and the rest.
   The wait serializes the CPU and the GPU, so the frame rate of such a run means nothing. */
typedef enum BTBHGPUCostPass {
    BTBH_GPU_COST_PASS_MAIN = 0,
    BTBH_GPU_COST_PASS_SHADOW = 1,
    BTBH_GPU_COST_PASS_RC_SOURCE = 2,
    BTBH_GPU_COST_PASS_COUNT = 3
} BTBHGPUCostPass;
bool btbh_gpu_cost_log_enabled(void);
void btbh_gpu_cost_record_draw(
    BTBHGPUCostPass pass,
    uint32_t element_count,
    uint32_t instance_count,
    bool static_city,
    const BTBHGPUModel *model,
    const BTBHGPUCustomMesh *custom_mesh
);
void btbh_gpu_model_set_debug_name(BTBHGPUModel *model, const char *path);
bool btbh_gpu_cost_skips_model(const BTBHGPUModel *model);
void btbh_gpu_cost_emit_frame(double gpu_milliseconds);
/* The cost log's per-pass split (2026-10-17): about every other frame is submitted in pieces, one per group of
   passes below, and each piece is waited for before the next is recorded, so its submit-to-fence time is the
   GPU's time for those passes alone; the line then carries `split=1` and `<group>_ms` per group, and gpu_ms is
   their sum. The other frames are submitted whole (`split=0`), so their gpu_ms shows what the split itself adds. */
typedef enum BTBHGPUCostGroup {
    BTBH_GPU_COST_GROUP_UPLOAD = 0,      /* the scene's instance, bone and motion upload */
    BTBH_GPU_COST_GROUP_SHADOW = 1,      /* sun, secondary and spot/omni shadow maps */
    BTBH_GPU_COST_GROUP_MAIN = 2,        /* the opaque geometry pass */
    BTBH_GPU_COST_GROUP_RC_SOURCE = 3,   /* radiance-cascade emitters and material sources */
    BTBH_GPU_COST_GROUP_RC_FIELD = 4,    /* HRC trace, merges, quadrant finish, temporal reprojection (probe space) */
    BTBH_GPU_COST_GROUP_RC_SCREEN = 5,   /* HRC resolve (internal size), reconstruction and compose (render size) */
    BTBH_GPU_COST_GROUP_SSAO = 6,        /* SSAO, its denoise and the lighting resolve */
    BTBH_GPU_COST_GROUP_TRANSPARENT = 7, /* the soft-particle distance and the transparent/effects pass */
    BTBH_GPU_COST_GROUP_POST = 8,        /* bloom, HDR composite, LUT, CAS, tilt-shift, FXAA/SMAA, FSR, upscale */
    BTBH_GPU_COST_GROUP_UI = 9,          /* the canvas: the scene onto the swapchain, the UI */
    BTBH_GPU_COST_GROUP_COUNT = 10
} BTBHGPUCostGroup;
/* Starts a scene frame; with the cost log about every other one is split. */
void btbh_gpu_cost_begin_frame(SDL_GPUDevice *device);
/* In a split frame: submits `*command` (the passes of `finished_group`), waits for it and acquires the next
   command buffer into `*command`. False, with `*command` NULL, when the GPU failed either. Otherwise a no-op. */
bool btbh_gpu_cost_split(SDL_GPUCommandBuffer **command, BTBHGPUCostGroup finished_group);
/* With the cost log, after each measured frame: keeps the GPU busy (`BTBH_GPU_COST_WARM_MS`, default 12) so the
   driver does not lower its clocks while the CPU prepares the next frame; `btbh_gpu_cost_wait_warm` waits for it
   before measured work is submitted. */
void btbh_gpu_cost_warm(SDL_GPUDevice *device);
void btbh_gpu_cost_wait_warm(void);
void btbh_gpu_cost_release_warm(void);
void btbh_gpu_window_release_frame_telemetry(
    BTBHGPUWindow *window
);
bool btbh_gpu_window_record_presented_frame(
    BTBHGPUWindow *window
);
SDL_GPUTexture *btbh_gpu_window_present_target(
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
bool btbh_gpu_animation_pose_sample_skin(
    const BTBHGPUAnimationPose *pose,
    uint32_t skin_index,
    float *matrices,
    uint32_t matrix_capacity
);
uint32_t btbh_gpu_animation_pose_skin_joint_count(
    const BTBHGPUAnimationPose *pose,
    uint32_t skin_index
);
bool btbh_gpu_model_compose_orm_rgba8(
    const uint8_t *occlusion_pixels,
    uint32_t occlusion_width,
    uint32_t occlusion_height,
    const uint8_t *metallic_roughness_pixels,
    uint32_t metallic_roughness_width,
    uint32_t metallic_roughness_height,
    uint8_t **composed_pixels,
    uint32_t *composed_width,
    uint32_t *composed_height
);
bool btbh_gpu_texture_decode_memory_rgba8(
    const uint8_t *encoded,
    size_t encoded_size,
    uint8_t **pixels,
    uint32_t *width,
    uint32_t *height
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
BTBHGPUTexture *btbh_gpu_window_texture_create_rgba8_mipmapped(
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
BTBHGPUTexture *btbh_gpu_window_texture_load_memory_format(
    BTBHGPUWindow *window,
    const uint8_t *encoded,
    size_t encoded_size,
    uint8_t generate_mipmaps,
    BTBHGPUTextureFilter filter,
    BTBHGPUTextureWrap wrap,
    SDL_GPUTextureFormat format,
    BTBHGPUTextureReport *report
);
BTBHGPUTexture *btbh_gpu_window_texture_load_format(
    BTBHGPUWindow *window,
    const char *path,
    uint8_t generate_mipmaps,
    BTBHGPUTextureFilter filter,
    BTBHGPUTextureWrap wrap,
    SDL_GPUTextureFormat format,
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
