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

#define BTBH_GPU_FRAME_TELEMETRY_CAPACITY 256u

/* The window's upload queue (since 0.11.0): BTBH_GPU_CUSTOM_MESH_UPDATE_QUEUED (RFC 0012's extra, whole and partial
   updates) and BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED. An entry is one range of bytes for one destination: a custom
   mesh's vertices or indices, or an instance buffer (both of its buffers). */
typedef enum BTBHGPUUploadPart {
    BTBH_GPU_UPLOAD_MESH_VERTICES = 0,
    BTBH_GPU_UPLOAD_MESH_INDICES = 1,
    BTBH_GPU_UPLOAD_INSTANCES = 2
} BTBHGPUUploadPart;

/* One queued range: byte_count bytes at data_offset of the queue's bytes, for byte_offset of the destination (from the
   mesh's first vertex or first index in its buffers; from an instance buffer's start). Exactly one of mesh and
   instance_buffer is set while the entry is in its target's chain (both NULL once a whole update or the target's destroy
   dropped it); live is false once a later write replaced the range or it was dropped. next chains a target's entries,
   newest first: 1 + the index of the one before, 0 at the end. Every entry that names a target is in its chain. cycle: a whole update of a mesh with buffers of its own (SDL's cycling); every other range is
   written in place. */
typedef struct BTBHGPUUploadQueueEntry {
    BTBHGPUCustomMesh *mesh;
    BTBHGPUInstanceBuffer *instance_buffer;
    uint32_t next;
    uint32_t data_offset;
    uint32_t byte_offset;
    uint32_t byte_count;
    uint8_t part;
    bool cycle;
    bool live;
} BTBHGPUUploadQueueEntry;

/* A window's queued uploads (btbh_gpu_custom_mesh.c): what the next scene copies at the start of its command buffer
   (btbh_gpu_upload_queue_record), until a scene that copied them is kept (btbh_gpu_upload_queue_commit). Ranges that
   overlap carry the same bytes in the overlap (a later write patches the earlier ranges), so the order of their copies
   doesn't matter. copied_serial counts the scenes that copied any; pending_serial is the one whose frame waits for the
   canvas (window->pending_command), 0 when none does. pending_holds: that frame holds queued copies, or a write outside
   the queue recorded after its draws, so it is submitted even when the canvas drops it
   (btbh_gpu_upload_queue_finish_pending). */
typedef struct BTBHGPUUploadQueue {
    BTBHGPUUploadQueueEntry *entries;
    uint32_t entry_count;
    uint32_t entry_capacity;
    uint32_t live_count;
    uint8_t *bytes;
    uint32_t byte_count;
    uint32_t byte_capacity;
    uint32_t live_bytes;
    uint64_t copied_serial;
    uint64_t pending_serial;
    bool pending_holds;
    /* Set by btbh_gpu_upload_queue_record until the scene that recorded is kept or dropped. */
    bool recorded;
} BTBHGPUUploadQueue;

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
    /* BTBH_GPU_CUSTOM_MESH_UPDATE_QUEUED and BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED (since 0.11.0): the uploads queued
       for the next scene (btbh_gpu_custom_mesh.c). */
    BTBHGPUUploadQueue upload_queue;
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
    /* BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED (since 0.11.0): 1 + its newest entry in the owner's upload_queue while
       uploads of it wait there (0: none), and the serial of the last scene that copied a queued upload of it (0:
       none). */
    uint32_t queue_head;
    uint64_t queue_copied_serial;
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
    /* BTBH_GPU_CUSTOM_MESH_UPDATE_QUEUED (since 0.11.0): 1 + its newest entry in the owner's upload_queue while
       updates of it wait there (0: none), and the serial of the last scene that copied a queued update of it (0:
       none). */
    uint32_t queue_head;
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
/* The window's upload queue (since 0.11.0, btbh_gpu_custom_mesh.c). For the scene that copies it (btbh_gpu_scene.c):
   - _size: the bytes the window's live queued ranges take in a transfer buffer, each at a multiple of 16 (0: nothing
     queued);
   - _write: packs them so at `destination` (a mapped transfer buffer, _size bytes);
   - _record: uploads them from `transfer` at `offset` (where _write's destination was) to their buffers in `copy`,
     cycling only a whole update of a mesh with buffers of its own; returns how many uploads;
   - _commit: the scene that recorded them is kept (its command buffer submitted, or waiting for the canvas when
     `pending`): the queue empties. Without a record it only notes that no queued copy waits in a pending frame. A
     scene that recorded and then failed calls nothing: the queue stays for the next one. */
uint32_t btbh_gpu_upload_queue_size(const BTBHGPUWindow *window);
void btbh_gpu_upload_queue_write(const BTBHGPUWindow *window, uint8_t *destination);
uint32_t btbh_gpu_upload_queue_record(
    BTBHGPUWindow *window,
    SDL_GPUCopyPass *copy,
    SDL_GPUTransferBuffer *transfer,
    uint32_t offset
);
void btbh_gpu_upload_queue_commit(BTBHGPUWindow *window, bool pending);
/* For the uploads (exactly one of `mesh` and `instance_buffer` set; the window is its owner):
   - _prepare: room for a range of `first_byte_count` bytes and one of `second_byte_count` (0: none); false, with
     `message` written ("<what>: ..."), when the queue would pass BTBH_GPU_CUSTOM_MESH_QUEUE_MAX_BYTES (*out_of_memory
     false) or memory runs out (true). Nothing changes on false, and the _push calls it made room for can't fail;
   - _push: queues `byte_count` bytes for `byte_offset` of the target's part: the earlier ranges of that part it
     covers are dropped, those it overlaps take its bytes there, and one that holds it entirely takes them instead of
     a new range;
   - _overwrite: a write outside the queue (submitted, or recorded into the frame waiting for the canvas) made after
     the queued ones: the same, without queueing anything;
   - _drop: every range of the target goes, and its entries forget it (a whole update replaces them, or the target is
     destroyed);
   - _pending_command: the frame waiting for the canvas, when it copied a queued upload of a target that scene
     `copied_serial` copied last; else NULL;
   - _note_pending_write: a write outside the queue was recorded into the frame waiting for the canvas. */
bool btbh_gpu_upload_queue_prepare(
    BTBHGPUWindow *window,
    uint32_t first_byte_count,
    uint32_t second_byte_count,
    const char *what,
    char *message,
    size_t message_size,
    bool *out_of_memory
);
void btbh_gpu_upload_queue_push(
    BTBHGPUWindow *window,
    BTBHGPUCustomMesh *mesh,
    BTBHGPUInstanceBuffer *instance_buffer,
    BTBHGPUUploadPart part,
    uint32_t byte_offset,
    const void *bytes,
    uint32_t byte_count,
    bool cycle
);
void btbh_gpu_upload_queue_overwrite(
    BTBHGPUWindow *window,
    BTBHGPUCustomMesh *mesh,
    BTBHGPUInstanceBuffer *instance_buffer,
    BTBHGPUUploadPart part,
    uint32_t byte_offset,
    const void *bytes,
    uint32_t byte_count
);
void btbh_gpu_upload_queue_drop(
    BTBHGPUWindow *window,
    BTBHGPUCustomMesh *mesh,
    BTBHGPUInstanceBuffer *instance_buffer
);
SDL_GPUCommandBuffer *btbh_gpu_upload_queue_pending_command(
    const BTBHGPUWindow *window,
    uint64_t copied_serial
);
void btbh_gpu_upload_queue_note_pending_write(BTBHGPUWindow *window);
/* The frame waiting for the canvas is finished, submitted or dropped (btbh_gpu_window_finish_pending_frame): true when
   it holds queued copies or writes recorded after its draws, so that it must be submitted; either way the queue
   forgets that frame. */
bool btbh_gpu_upload_queue_finish_pending(BTBHGPUWindow *window);
/* Drops the window's queued uploads and frees the queue (the window closes). */
void btbh_gpu_window_release_upload_queue(BTBHGPUWindow *window);
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
   measurement can tell how much of a frame went into making them. */
typedef struct BTBHGPUCreationStats {
    uint64_t shaders;
    uint64_t graphics_pipelines;
    uint64_t compute_pipelines;
    uint64_t nanoseconds;
} BTBHGPUCreationStats;
SDL_GPUShader *btbh_gpu_create_shader(
    SDL_GPUDevice *device,
    const SDL_GPUShaderCreateInfo *info
);
SDL_GPUGraphicsPipeline *btbh_gpu_create_graphics_pipeline(
    SDL_GPUDevice *device,
    const SDL_GPUGraphicsPipelineCreateInfo *info
);
SDL_GPUComputePipeline *btbh_gpu_create_compute_pipeline(
    SDL_GPUDevice *device,
    const SDL_GPUComputePipelineCreateInfo *info
);
void btbh_gpu_creation_stats(BTBHGPUCreationStats *stats);

#endif
