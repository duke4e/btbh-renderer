#ifndef BTBH_GPU_INTERNAL_H
#define BTBH_GPU_INTERNAL_H

#include "btbh_gpu_package_internal.h"

#include <SDL3/SDL.h>

#include <stdbool.h>

/* One buffer of a window's geometry arena: model primitives, and custom meshes made with
   BTBH_GPU_CUSTOM_MESH_SHARED_BUFFERS (RFC 0011, since 0.10.0), take ranges of it instead of buffers of their own,
   so a frame binds a handful of buffers, not one or two per primitive (SDL tracks every bound buffer with a scan
   of the buffers its command buffer used, 2026-10-18). Released when its last primitive or mesh goes; ranges are
   only ever appended to the arena's last block, so a released range is a hole that is never reused. */
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

#define BTBH_GPU_GEOMETRY_VERTEX_BLOCK_BYTES (64u * 1024u * 1024u)
#define BTBH_GPU_GEOMETRY_INDEX_BLOCK_BYTES (32u * 1024u * 1024u)

/* Copies `size` bytes into the arena's open (last) block, or a new one when they do not fit, at an offset that is a
   multiple of `alignment`, written to *offset; returns the block (its live count raised), or NULL. A range bigger
   than `block_bytes` gets a block of its own size. (btbh_gpu_model.c) */
BTBHGPUGeometryBlock *btbh_gpu_geometry_arena_upload(
    SDL_GPUDevice *device,
    BTBHGPUGeometryArena *arena,
    SDL_GPUBufferUsageFlags usage,
    uint32_t block_bytes,
    uint32_t alignment,
    const void *bytes,
    uint32_t size,
    uint32_t *offset
);
/* Copies `size` bytes into `buffer` at `offset`, without cycling it: the copy is ordered after the GPU's earlier use
   of the buffer and before its later use. False when the GPU failed. */
bool btbh_gpu_geometry_copy(
    SDL_GPUDevice *device,
    SDL_GPUBuffer *buffer,
    uint32_t offset,
    const void *bytes,
    uint32_t size
);
/* Lowers a block's live count; at zero its buffer is released (with `device`; NULL only forgets it). */
void btbh_gpu_geometry_block_release(
    BTBHGPUGeometryBlock *block,
    SDL_GPUDevice *device
);

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
typedef struct BTBHGPUModelTextureShare BTBHGPUModelTextureShare;
typedef struct BTBHGPULazyCreationLog BTBHGPULazyCreationLog;

#define BTBH_GPU_FRAME_TELEMETRY_CAPACITY 256u

/* BTBH_GPU_CUSTOM_MESH_UPDATE_QUEUED (RFC 0012's extra, since 0.11.0): one queued update, its vertices and indices at
   these offsets of the queue's bytes. mesh NULL: replaced by a later update, or its mesh destroyed. */
typedef struct BTBHGPUCustomMeshQueueEntry {
    BTBHGPUCustomMesh *mesh;
    uint32_t vertex_offset;
    uint32_t vertex_byte_count;
    uint32_t index_offset;
    uint32_t index_byte_count;
} BTBHGPUCustomMeshQueueEntry;

/* A window's queued updates (btbh_gpu_custom_mesh.c): what the next scene copies at the start of its command buffer
   (btbh_gpu_custom_mesh_queue_record), until a scene that copied them is kept (btbh_gpu_custom_mesh_queue_commit).
   copied_serial counts the scenes that copied any; pending_serial is the one whose frame waits for the canvas
   (window->pending_command), 0 when none does. */
typedef struct BTBHGPUCustomMeshQueue {
    BTBHGPUCustomMeshQueueEntry *entries;
    uint32_t entry_count;
    uint32_t entry_capacity;
    uint32_t live_count;
    uint8_t *bytes;
    uint32_t byte_count;
    uint32_t byte_capacity;
    uint32_t live_bytes;
    uint64_t copied_serial;
    uint64_t pending_serial;
    /* Set by btbh_gpu_custom_mesh_queue_record until the scene that recorded is kept or dropped. */
    bool recorded;
} BTBHGPUCustomMeshQueue;

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
    /* What the host asked for (BTBHGPUWindowDescriptor.present_mode, btbh_gpu_window_set_present_mode);
       present_mode above is what the swapchain took. */
    BTBHGPUPresentMode requested_present_mode;
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
    /* The model textures that BTBH_GPU_MODEL_IMPORT_SHARE_EXTERNAL_TEXTURES shares (btbh_gpu_model.c). */
    BTBHGPUModelTextureShare *model_texture_share_head;
    BTBHGPUGeometryArena vertex_arena;
    BTBHGPUGeometryArena index_arena;
    BTBHGPUInstanceBuffer *instance_buffer_head;
    BTBHGPUCustomMesh *custom_mesh_head;
    /* BTBH_GPU_CUSTOM_MESH_UPDATE_QUEUED (since 0.11.0): the updates queued for the next scene (btbh_gpu_custom_mesh.c),
       their data in mesh_queue_bytes. */
    BTBHGPUCustomMeshQueue mesh_queue;
    /* The SDF terrains attached to this window (btbh_gpu_window_sdf_terrain_attach). */
    BTBHGPUSdfTerrain *sdf_terrain_head;
    BTBHGPUSceneRuntime *scene_runtime;
    /* The scenes rendered to a target (btbh_gpu_window_scene_render_to_target): their own targets,
       shadow maps and history, so they never touch the window's frame (its FSR 3 history above all).
       Made by the first such scene. */
    BTBHGPUSceneRuntime *aside_scene_runtime;
    BTBHGPUSceneTelemetryReport scene_telemetry;
    uint32_t scene_shadow_update_period_frames[3];
    uint32_t scene_directional_shadow_size[2];  /* the primary directional shadow map's; 0: the default */
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
    /* The lazy creation log (since 0.11.0, btbh_gpu_device.c): made when it is first turned on. */
    BTBHGPULazyCreationLog *lazy_creation;
    /* The frames submitted so far (btbh_gpu_window_finish_pending_frame): the log's frame_index. */
    uint64_t submitted_frame_count;
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
    /* 0: a 2D texture. Otherwise a 2D array texture of this many layers (btbh_gpu_window_texture_array_*, RFC 0009),
       which only the slots that take an array accept (btbh_gpu_texture_is_array). */
    uint32_t layer_count;
    /* Made with SDL_GPU_TEXTUREUSAGE_COLOR_TARGET (a render target): a scene can be drawn into it. */
    bool color_target;
    /* RFC 0012 (since 0.11.0): a heightfield (btbh_gpu_window_heightfield_create), which only
       BTBHGPUSceneHeightLift.heightfield takes (btbh_gpu_texture_is_heightfield), and its heights on the CPU
       (width x height floats, row by row), for the opaque sort. NULL for every other texture. */
    float *heights;
    /* Set while models share it (BTBH_GPU_MODEL_IMPORT_SHARE_EXTERNAL_TEXTURES): the share owns it,
       so btbh_gpu_texture_destroy leaves it alone; the last model that holds it frees it. */
    BTBHGPUModelTextureShare *share;
};

/* A texture that models loaded with BTBH_GPU_MODEL_IMPORT_SHARE_EXTERNAL_TEXTURES share: one per
   external image (or, for an ORM texture, pair of images) and format, kept in the window's list.
   Each material slot that holds it counts once; the last one released frees it, and the window's
   destroy frees what is left. */
struct BTBHGPUModelTextureShare {
    BTBHGPUWindow *owner;
    BTBHGPUModelTextureShare *previous;
    BTBHGPUModelTextureShare *next;
    BTBHGPUTexture *texture;
    /* Normalised resolved paths: the image (for an ORM texture, its occlusion image) and the ORM
       texture's metallic-roughness image. NULL where an ORM texture has no such image. */
    char *path;
    char *second_path;
    SDL_GPUTextureFormat format;
    bool composed_orm;
    uint32_t reference_count;
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
    /* Its vertices' positions' bounds, measured on create and update, for the opaque pass's front-to-back
       order (BTBH_GPU_SCENE_OPAQUE_SORT_FRONT_TO_BACK); bounds_valid is false when a position isn't finite. */
    float minimum[3];
    float maximum[3];
    bool bounds_valid;
    /* The buffers' sizes in bytes: an update that fits reuses them (grow-only, since 0.5.0). For a shared mesh,
       the sizes of its ranges, which an update must fit. */
    uint32_t vertex_buffer_capacity;
    uint32_t index_buffer_capacity;
    /* BTBH_GPU_CUSTOM_MESH_SHARED_BUFFERS (RFC 0011, since 0.10.0): the window's arena blocks that vertex_buffer and
       index_buffer belong to (the mesh owns neither buffer; index_block is NULL without indices), and where its
       vertices (vertex_base, in vertices) and indices (first_index) start in them. Without the flag the blocks are
       NULL, and vertex_base and first_index 0. */
    BTBHGPUGeometryBlock *vertex_block;
    BTBHGPUGeometryBlock *index_block;
    int32_t vertex_base;
    uint32_t first_index;
    /* BTBH_GPU_CUSTOM_MESH_UPDATE_QUEUED (since 0.11.0): 1 + its entry in the owner's mesh_queue while an update of it
       waits there (0: none), and the serial of the last scene that copied a queued update of it (0: none). */
    uint32_t queue_entry;
    uint64_t queue_copied_serial;
};

/* The bounds of `vertex_count` custom mesh vertices' positions (the first three floats of each 24-float
   vertex); false, leaving minimum and maximum undefined, when one isn't finite. */
bool btbh_gpu_custom_mesh_bounds(
    const void *vertices,
    uint32_t vertex_count,
    float minimum[3],
    float maximum[3]
);
/* BTBH_GPU_CUSTOM_MESH_UPDATE_QUEUED (since 0.11.0), for the scene that copies the queue (btbh_gpu_scene.c):
   - _size: the bytes the window's live queued updates take in a transfer buffer, each vertex or index range at a
     multiple of 16 (0: nothing queued);
   - _write: packs them so at `destination` (a mapped transfer buffer, _size bytes);
   - _record: uploads them from `transfer` at `offset` (where _write's destination was) to their meshes' buffers in
     `copy`, cycling a mesh's own buffers and writing a shared mesh's ranges in place; returns how many uploads;
   - _commit: the scene that recorded them is kept (its command buffer submitted, or waiting for the canvas when
     `pending`): the queue empties. Without a record it only notes that no queued copy waits in a pending frame. A
     scene that recorded and then failed calls nothing: the queue stays for the next one. */
uint32_t btbh_gpu_custom_mesh_queue_size(const BTBHGPUWindow *window);
void btbh_gpu_custom_mesh_queue_write(const BTBHGPUWindow *window, uint8_t *destination);
uint32_t btbh_gpu_custom_mesh_queue_record(
    BTBHGPUWindow *window,
    SDL_GPUCopyPass *copy,
    SDL_GPUTransferBuffer *transfer,
    uint32_t offset
);
void btbh_gpu_custom_mesh_queue_commit(BTBHGPUWindow *window, bool pending);
/* Drops the window's queued updates and frees the queue (the window closes). */
void btbh_gpu_window_release_custom_mesh_queue(BTBHGPUWindow *window);
/* BTBH_GPU_SCENE_OPAQUE_SORT_FRONT_TO_BACK (btbh_gpu.h says which batches it moves and by what key): the batches
   the opaque pass draws, as indices into `batches`, in the order it draws them. Writes them to `order` (room
   for batch_count) and returns how many; scratch has room for 2 * batch_count keys. `batches` must be valid
   (as the scene draw checks them) and `instances` hold their transient instances (24 floats each).
   move_less_equal: BTBH_GPU_SCENE_OPAQUE_SORT_FRONT_TO_BACK_LESS_EQUAL, LESS_EQUAL batches move too. */
uint32_t btbh_gpu_scene_opaque_order(
    const BTBHGPUSceneGeometryBatch *batches,
    const uint8_t *scene_visibility,
    uint32_t batch_count,
    const float *instances,
    const float view_projection[16],
    bool move_less_equal,
    uint32_t *order,
    uint64_t *scratch
);

/* RFC 0011 phase 1b, the run key (btbh_gpu_scene.c): whether `batch` continues the run of `previous`, the batch the
   pass drew before it, so that it binds and pushes nothing but its vertex and index buffers. True when the two are
   equal, byte for byte, in every field but kind, model, custom_mesh, material_slot, first_instance and
   instance_count (padding isn't compared; a float compares by its bits). The passes also require both geometries to
   be unskinned. */
bool btbh_gpu_scene_batch_continues_run(
    const BTBHGPUSceneGeometryBatch *previous,
    const BTBHGPUSceneGeometryBatch *batch
);

/* BTBHGPUSceneGeometryBatch.detail_parameters as the present accepts them: finite; a pattern in [0, 7], rounded
   to the nearest whole number as the shader rounds it; patterns 1..6 with z above 0; pattern 7
   (BTBH_GPU_SCENE_DETAIL_FACADE_WEATHER) with x, y and z at 0. */
bool btbh_gpu_scene_detail_parameters_valid(const float detail_parameters[4]);

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
void btbh_gpu_window_release_aside_scene(BTBHGPUWindow *window);
void btbh_gpu_window_release_canvas(BTBHGPUWindow *window);
uint32_t btbh_gpu_parse_allowed_frames_in_flight(const char *value);
/* The present mode a window takes for `requested` (a BTBHGPUPresentMode), given whether it supports
   IMMEDIATE and MAILBOX; every window supports VSYNC. BTBH_GPU_PRESENT_MODE_INVALID for a value
   that isn't a request. */
BTBHGPUPresentMode btbh_gpu_resolve_present_mode(
    int requested,
    bool supports_immediate,
    bool supports_mailbox
);
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
/* One channel, width x height bytes (R8_UNORM): a font atlas baked with
   BTBH_GPU_FONT_FLAG_R8_ATLAS, which the canvas draws as coverage. */
BTBHGPUTexture *btbh_gpu_window_texture_create_r8(
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

/* Whether a texture is a 2D array (btbh_gpu_window_texture_array_*): a 2D slot refuses one (RFC 0009). */
static inline bool btbh_gpu_texture_is_array(const BTBHGPUTexture *texture) {
    return texture != NULL && texture->layer_count != 0u;
}

/* Whether a texture is a heightfield (btbh_gpu_window_heightfield_create): only a height lift takes one (RFC 0012). */
static inline bool btbh_gpu_texture_is_heightfield(const BTBHGPUTexture *texture) {
    return texture != NULL && texture->heights != NULL;
}

/* Every shader and pipeline the renderer makes goes through these three (since 0.7.0): they count what was made
   and the time it took, process-wide, so a test can tell whether a present made any (the prewarm smoke) and a
   measurement can tell how much of a frame went into making them. Call them through the macros below.
   `path` is the shader file (NULL when unknown) and `load_started_ticks` when reading it began (0: not timed), for
   the lazy creation log; `function` is the caller's (__func__). */
typedef struct BTBHGPUCreationStats {
    uint64_t shaders;
    uint64_t graphics_pipelines;
    uint64_t compute_pipelines;
    uint64_t nanoseconds;
} BTBHGPUCreationStats;
SDL_GPUShader *btbh_gpu_create_shader_from(
    SDL_GPUDevice *device,
    const SDL_GPUShaderCreateInfo *info,
    const char *path,
    uint64_t load_started_ticks,
    const char *function
);
SDL_GPUGraphicsPipeline *btbh_gpu_create_graphics_pipeline_at(
    SDL_GPUDevice *device,
    const SDL_GPUGraphicsPipelineCreateInfo *info,
    const char *function
);
SDL_GPUComputePipeline *btbh_gpu_create_compute_pipeline_from(
    SDL_GPUDevice *device,
    const SDL_GPUComputePipelineCreateInfo *info,
    const char *path,
    uint64_t load_started_ticks,
    const char *function
);
void btbh_gpu_creation_stats(BTBHGPUCreationStats *stats);
#define btbh_gpu_create_shader(device, info) \
    btbh_gpu_create_shader_from((device), (info), NULL, 0u, __func__)
#define btbh_gpu_create_graphics_pipeline(device, info) \
    btbh_gpu_create_graphics_pipeline_at((device), (info), __func__)
#define btbh_gpu_create_compute_pipeline(device, info) \
    btbh_gpu_create_compute_pipeline_from((device), (info), NULL, 0u, __func__)

/* The lazy creation log (since 0.11.0, btbh_gpu_window_lazy_creation_report; btbh_gpu_device.c). A present, a render
   to a target and a canvas present run inside a scope (btbh_gpu_lazy_creation_begin / _end). While the window's log
   is on (or BTBH_GPU_LOG_LAZY_CREATION=1), what the scope's thread makes in it is recorded: the shaders and pipelines
   through the functions above, and SDL's textures, samplers, buffers and transfer buffers through the macros below,
   which every file that includes this header calls in place of SDL's (the caller's __func__ names where). Off, a
   scope costs a flag test and the wrappers one atomic load more than SDL's own call. */
typedef struct BTBHGPULazyCreationScope {
    BTBHGPUWindow *window;   /* the window this scope records, NULL when it records nothing */
    BTBHGPUWindow *previous_window;
    SDL_ThreadID previous_thread;
    uint8_t previous_call;
    bool previous_in_prewarm_step;
} BTBHGPULazyCreationScope;
/* The timed SDL calls (BTBH_GPU_LAZY_CREATION_SLOW_CALL): recorded when one takes 1 ms or more over its usual time. */
typedef enum BTBHGPULazyTimedCall {
    BTBH_GPU_LAZY_CALL_SCENE_COMMAND_BUFFER = 0,
    BTBH_GPU_LAZY_CALL_SCENE_UPLOAD_MAP,
    BTBH_GPU_LAZY_CALL_TARGET_SUBMIT,
    BTBH_GPU_LAZY_CALL_CANVAS_COMMAND_BUFFER,
    BTBH_GPU_LAZY_CALL_SWAPCHAIN_ACQUIRE,
    BTBH_GPU_LAZY_CALL_CANVAS_UPLOAD_MAP,
    BTBH_GPU_LAZY_CALL_FRAME_SUBMIT,
    BTBH_GPU_LAZY_CALL_COUNT
} BTBHGPULazyTimedCall;
void btbh_gpu_lazy_creation_begin(
    BTBHGPUWindow *window,
    uint8_t call,
    BTBHGPULazyCreationScope *scope
);
void btbh_gpu_lazy_creation_end(const BTBHGPULazyCreationScope *scope);
/* Brackets a step the present shares with the prewarm (btbh_gpu_scene_prepare_frame, _ensure_model_pipelines,
   _ensure_frame_targets): what is made inside is marked prewarm_covers. */
void btbh_gpu_lazy_creation_prewarm_step(bool entering);
/* SDL_GetTicksNS() while this thread's scope records, else 0: the start of a timed call. */
uint64_t btbh_gpu_lazy_creation_clock(void);
/* Ends a timed call started with btbh_gpu_lazy_creation_clock (nothing when started_ticks is 0). */
void btbh_gpu_lazy_creation_call_timed(BTBHGPULazyTimedCall call, uint64_t started_ticks);
/* Frees the window's log (btbh_gpu_window_destroy). */
void btbh_gpu_lazy_creation_release(BTBHGPUWindow *window);
/* For measurements (the stage bench): a timed call is recorded when it takes `nanoseconds` over its usual time
   (default 1 ms); 0 records every timed call. */
void btbh_gpu_lazy_creation_set_call_threshold(uint64_t nanoseconds);
SDL_GPUTexture *btbh_gpu_create_texture_at(
    SDL_GPUDevice *device,
    const SDL_GPUTextureCreateInfo *info,
    const char *function
);
SDL_GPUSampler *btbh_gpu_create_sampler_at(
    SDL_GPUDevice *device,
    const SDL_GPUSamplerCreateInfo *info,
    const char *function
);
SDL_GPUBuffer *btbh_gpu_create_buffer_at(
    SDL_GPUDevice *device,
    const SDL_GPUBufferCreateInfo *info,
    const char *function
);
SDL_GPUTransferBuffer *btbh_gpu_create_transfer_buffer_at(
    SDL_GPUDevice *device,
    const SDL_GPUTransferBufferCreateInfo *info,
    const char *function
);
/* Upload staging (since 0.11.0, btbh_gpu_device_set_upload_staging_budget): the uploads of textures
   (btbh_gpu_texture.c) and of the geometry arena (models, shared custom meshes) take `size` bytes of a transfer
   buffer at *offset with btbh_gpu_staging_acquire (mapped without cycling: write at the mapped pointer plus *offset,
   and copy from *offset), submit their command buffer with btbh_gpu_staging_submit (which hands the range back) and
   hand it back unused with btbh_gpu_staging_abandon. With a budget, the range is part of a transfer buffer the pool
   keeps, which starts again once the GPU has finished the uploads that read it (fences, never waited for); without
   one (the default), each upload makes its own buffer (*offset 0) and releases it, as before. */
SDL_GPUTransferBuffer *btbh_gpu_staging_acquire(SDL_GPUDevice *device, uint32_t size, uint32_t *offset);
bool btbh_gpu_staging_submit(SDL_GPUDevice *device, SDL_GPUCommandBuffer *command, SDL_GPUTransferBuffer *transfer);
void btbh_gpu_staging_abandon(SDL_GPUDevice *device, SDL_GPUTransferBuffer *transfer);
/* Releases the pool (btbh_gpu_device_shutdown, before the device goes). */
void btbh_gpu_staging_release_device(SDL_GPUDevice *device);
#define SDL_CreateGPUTexture(device, info) btbh_gpu_create_texture_at((device), (info), __func__)
#define SDL_CreateGPUSampler(device, info) btbh_gpu_create_sampler_at((device), (info), __func__)
#define SDL_CreateGPUBuffer(device, info) btbh_gpu_create_buffer_at((device), (info), __func__)
#define SDL_CreateGPUTransferBuffer(device, info) btbh_gpu_create_transfer_buffer_at((device), (info), __func__)

#endif
