#ifndef BTBH_GPU_H
#define BTBH_GPU_H

#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

#define BTBH_GPU_DRIVER_NAME_CAPACITY 32u

typedef enum BTBHGPUBackendRequest {
    BTBH_GPU_BACKEND_INVALID = 0,
    BTBH_GPU_BACKEND_VULKAN = 1,
    BTBH_GPU_BACKEND_METAL = 2
} BTBHGPUBackendRequest;

typedef enum BTBHGPUShaderFormat {
    BTBH_GPU_SHADER_FORMAT_INVALID = 0,
    BTBH_GPU_SHADER_FORMAT_SPIRV = 1,
    BTBH_GPU_SHADER_FORMAT_METALLIB = 2
} BTBHGPUShaderFormat;

typedef enum BTBHGPUResult {
    BTBH_GPU_OK = 0,
    BTBH_GPU_INVALID_ARGUMENT = 1,
    BTBH_GPU_BACKEND_MISMATCH = 2,
    BTBH_GPU_SOFTWARE_DEVICE = 3,
    BTBH_GPU_MISSING_CAPABILITY = 4,
    BTBH_GPU_PLATFORM_FAILURE = 5,
    BTBH_GPU_DEVICE_FAILURE = 6,
    BTBH_GPU_WINDOW_FAILURE = 7
} BTBHGPUResult;

typedef enum BTBHGPUErrorStage {
    BTBH_GPU_ERROR_STAGE_NONE = 0,
    BTBH_GPU_ERROR_STAGE_PLATFORM_INITIALIZATION = 1,
    BTBH_GPU_ERROR_STAGE_DEVICE_PROPERTIES = 2,
    BTBH_GPU_ERROR_STAGE_DEVICE_CREATION = 3,
    BTBH_GPU_ERROR_STAGE_DRIVER_VALIDATION = 4,
    BTBH_GPU_ERROR_STAGE_CAPABILITY_VALIDATION = 5,
    BTBH_GPU_ERROR_STAGE_WINDOW_CREATION = 6,
    BTBH_GPU_ERROR_STAGE_WINDOW_CLAIM = 7,
    BTBH_GPU_ERROR_STAGE_SWAPCHAIN_CONFIGURATION = 8,
    BTBH_GPU_ERROR_STAGE_WINDOW_RESIZE = 9
} BTBHGPUErrorStage;

typedef uint64_t BTBHGPUFeatureFlags;

#define BTBH_GPU_FEATURE_GBUFFER_RGBA8_SRGB      (UINT64_C(1) << 0u)
#define BTBH_GPU_FEATURE_GBUFFER_RGBA8_UNORM     (UINT64_C(1) << 1u)
#define BTBH_GPU_FEATURE_GBUFFER_RGBA16_FLOAT    (UINT64_C(1) << 2u)
#define BTBH_GPU_FEATURE_SAMPLEABLE_DEPTH        (UINT64_C(1) << 3u)
#define BTBH_GPU_FEATURE_SHADOW_2D_ARRAY         (UINT64_C(1) << 4u)
#define BTBH_GPU_FEATURE_SHADOW_CUBE_ARRAY       (UINT64_C(1) << 5u)
#define BTBH_GPU_FEATURE_STORAGE_RGBA16_FLOAT    (UINT64_C(1) << 6u)
#define BTBH_GPU_FEATURE_COMPUTE                 (UINT64_C(1) << 7u)
#define BTBH_GPU_FEATURE_TEXTURE_READBACK        (UINT64_C(1) << 8u)
#define BTBH_GPU_PHASE2_REQUIRED_FEATURES ( \
    BTBH_GPU_FEATURE_GBUFFER_RGBA8_SRGB | \
    BTBH_GPU_FEATURE_GBUFFER_RGBA8_UNORM | \
    BTBH_GPU_FEATURE_GBUFFER_RGBA16_FLOAT | \
    BTBH_GPU_FEATURE_SAMPLEABLE_DEPTH | \
    BTBH_GPU_FEATURE_SHADOW_2D_ARRAY | \
    BTBH_GPU_FEATURE_SHADOW_CUBE_ARRAY | \
    BTBH_GPU_FEATURE_STORAGE_RGBA16_FLOAT | \
    BTBH_GPU_FEATURE_COMPUTE | \
    BTBH_GPU_FEATURE_TEXTURE_READBACK \
)

typedef struct BTBHGPUCapabilitySnapshot {
    char driver_name[BTBH_GPU_DRIVER_NAME_CAPACITY];
    uint8_t hardware_accelerated;
    uint8_t reserved[7];
    BTBHGPUFeatureFlags features;
} BTBHGPUCapabilitySnapshot;

typedef struct BTBHGPUValidationReport {
    BTBHGPUResult result;
    char expected_driver[BTBH_GPU_DRIVER_NAME_CAPACITY];
    char actual_driver[BTBH_GPU_DRIVER_NAME_CAPACITY];
    BTBHGPUFeatureFlags missing_features;
} BTBHGPUValidationReport;

typedef struct BTBHGPUDeviceDescriptor {
    BTBHGPUBackendRequest backend;
    BTBHGPUFeatureFlags required_features;
    uint8_t debug_mode;
    uint8_t reserved[7];
} BTBHGPUDeviceDescriptor;

typedef struct BTBHGPUDeviceReport {
    BTBHGPUResult result;
    BTBHGPUErrorStage error_stage;
    BTBHGPUCapabilitySnapshot capabilities;
    BTBHGPUValidationReport validation;
    char message[256];
} BTBHGPUDeviceReport;

typedef struct BTBHGPUDevice BTBHGPUDevice;

typedef enum BTBHGPUTextureFormat {
    BTBH_GPU_TEXTURE_FORMAT_INVALID = 0,
    BTBH_GPU_TEXTURE_FORMAT_RGBA8_UNORM = 1,
    BTBH_GPU_TEXTURE_FORMAT_RGBA8_SRGB = 2,
    BTBH_GPU_TEXTURE_FORMAT_BGRA8_UNORM = 3,
    BTBH_GPU_TEXTURE_FORMAT_BGRA8_SRGB = 4,
    BTBH_GPU_TEXTURE_FORMAT_RGB10A2_UNORM = 5,
    BTBH_GPU_TEXTURE_FORMAT_RGBA16_FLOAT = 6
} BTBHGPUTextureFormat;

typedef enum BTBHGPUPresentMode {
    BTBH_GPU_PRESENT_MODE_INVALID = 0,
    BTBH_GPU_PRESENT_MODE_VSYNC = 1,
    BTBH_GPU_PRESENT_MODE_IMMEDIATE = 2,
    BTBH_GPU_PRESENT_MODE_MAILBOX = 3
} BTBHGPUPresentMode;

typedef enum BTBHGPUSwapchainComposition {
    BTBH_GPU_SWAPCHAIN_INVALID = 0,
    BTBH_GPU_SWAPCHAIN_SDR = 1
} BTBHGPUSwapchainComposition;

typedef struct BTBHGPUWindowDescriptor {
    const char *title;
    uint32_t width;
    uint32_t height;
    uint8_t hidden;
    uint8_t high_pixel_density;
    uint8_t reserved[6];
} BTBHGPUWindowDescriptor;

typedef struct BTBHGPUWindowReport {
    BTBHGPUResult result;
    BTBHGPUErrorStage error_stage;
    uint32_t logical_width;
    uint32_t logical_height;
    uint32_t drawable_width;
    uint32_t drawable_height;
    BTBHGPUTextureFormat swapchain_format;
    BTBHGPUPresentMode present_mode;
    BTBHGPUSwapchainComposition composition;
    uint8_t claimed;
    uint8_t minimized;
    uint8_t hidden;
    uint8_t reserved[5];
    char message[256];
} BTBHGPUWindowReport;

typedef struct BTBHGPUPlatformSnapshot {
    double elapsed_seconds;
    float delta_seconds;
    float mouse_x;
    float mouse_y;
    float mouse_delta_x;
    float mouse_delta_y;
    float mouse_wheel_x;
    float mouse_wheel_y;
    int32_t refresh_rate;
    int32_t frames_per_second;
    uint8_t should_close;
    uint8_t focused;
    uint8_t cursor_visible;
    uint8_t reserved[5];
} BTBHGPUPlatformSnapshot;

typedef struct BTBHGPUWindow BTBHGPUWindow;
typedef struct BTBHGPUTexture BTBHGPUTexture;
typedef struct BTBHGPUFont BTBHGPUFont;
typedef struct BTBHGPUModel BTBHGPUModel;
typedef struct BTBHGPUInstanceBuffer BTBHGPUInstanceBuffer;
typedef struct BTBHGPUCustomMesh BTBHGPUCustomMesh;
typedef struct BTBHGPUAnimationLibrary BTBHGPUAnimationLibrary;

typedef enum BTBHGPUTextureFilter {
    BTBH_GPU_TEXTURE_FILTER_NEAREST = 0,
    BTBH_GPU_TEXTURE_FILTER_LINEAR = 1,
    BTBH_GPU_TEXTURE_FILTER_TRILINEAR = 2
} BTBHGPUTextureFilter;

typedef enum BTBHGPUTextureWrap {
    BTBH_GPU_TEXTURE_WRAP_CLAMP = 0,
    BTBH_GPU_TEXTURE_WRAP_REPEAT = 1,
    BTBH_GPU_TEXTURE_WRAP_MIRROR = 2
} BTBHGPUTextureWrap;

typedef struct BTBHGPUTextureReport {
    BTBHGPUResult result;
    uint32_t width;
    uint32_t height;
    uint32_t mip_count;
    char message[256];
} BTBHGPUTextureReport;

typedef struct BTBHGPUFontReport {
    BTBHGPUResult result;
    uint32_t base_size;
    uint32_t glyph_count;
    uint32_t atlas_width;
    uint32_t atlas_height;
    float ascent;
    float descent;
    float line_gap;
    char message[256];
} BTBHGPUFontReport;

typedef struct BTBHGPUFontGlyph {
    uint32_t codepoint;
    float advance;
    float offset_x;
    float offset_y;
    float width;
    float height;
    float u0;
    float v0;
    float u1;
    float v1;
} BTBHGPUFontGlyph;

typedef enum BTBHGPUCanvasBlendMode {
    BTBH_GPU_CANVAS_BLEND_ALPHA = 0,
    BTBH_GPU_CANVAS_BLEND_ADDITIVE = 1,
    BTBH_GPU_CANVAS_BLEND_MULTIPLIED = 2,
    BTBH_GPU_CANVAS_BLEND_PREMULTIPLIED_ALPHA = 3
} BTBHGPUCanvasBlendMode;

typedef struct BTBHGPUCanvasVertex {
    float x;
    float y;
    float u;
    float v;
    float red;
    float green;
    float blue;
    float alpha;
} BTBHGPUCanvasVertex;

typedef struct BTBHGPUCanvasBatch {
    uint32_t first_vertex;
    uint32_t vertex_count;
    BTBHGPUTexture *texture;
    BTBHGPUCanvasBlendMode blend_mode;
    int32_t scissor_x;
    int32_t scissor_y;
    int32_t scissor_width;
    int32_t scissor_height;
    uint8_t scissor_enabled;
    uint8_t reserved[3];
} BTBHGPUCanvasBatch;

typedef struct BTBHGPUCanvasPass {
    uint32_t first_batch;
    uint32_t batch_count;
    BTBHGPUTexture *target;
    uint8_t clear_enabled;
    uint8_t clear_red;
    uint8_t clear_green;
    uint8_t clear_blue;
    uint8_t clear_alpha;
    uint8_t reserved[3];
} BTBHGPUCanvasPass;

typedef struct BTBHGPUCanvasReport {
    BTBHGPUResult result;
    uint32_t drawable_width;
    uint32_t drawable_height;
    uint32_t batch_count;
    uint32_t vertex_count;
    char message[256];
} BTBHGPUCanvasReport;

typedef struct BTBHGPUModelReport {
    BTBHGPUResult result;
    float minimum_x;
    float minimum_y;
    float minimum_z;
    float maximum_x;
    float maximum_y;
    float maximum_z;
    uint32_t mesh_count;
    uint32_t material_count;
    char message[256];
} BTBHGPUModelReport;

typedef struct BTBHGPUModelMaterialInfo {
    BTBHGPUTexture *albedo_texture;
    uint8_t albedo_red;
    uint8_t albedo_green;
    uint8_t albedo_blue;
    uint8_t albedo_alpha;
    BTBHGPUTexture *emission_texture;
    uint8_t emission_red;
    uint8_t emission_green;
    uint8_t emission_blue;
    uint8_t emission_alpha;
    float emission_energy;
    uint8_t unlit_enabled;
    uint8_t reserved[3];
} BTBHGPUModelMaterialInfo;

typedef struct BTBHGPUInstanceBufferReport {
    BTBHGPUResult result;
    uint32_t capacity;
    uint32_t stride;
    uint32_t byte_capacity;
    char message[256];
} BTBHGPUInstanceBufferReport;

typedef struct BTBHGPUCustomMeshReport {
    BTBHGPUResult result;
    uint32_t vertex_count;
    uint32_t index_count;
    uint32_t vertex_byte_count;
    char message[256];
} BTBHGPUCustomMeshReport;

typedef struct BTBHGPUAnimationLibraryReport {
    BTBHGPUResult result;
    uint32_t animation_count;
    char message[256];
} BTBHGPUAnimationLibraryReport;

typedef enum BTBHGPUSceneGeometryKind {
    BTBH_GPU_SCENE_GEOMETRY_INVALID = 0,
    BTBH_GPU_SCENE_GEOMETRY_MODEL = 1,
    BTBH_GPU_SCENE_GEOMETRY_CUSTOM_MESH = 2
} BTBHGPUSceneGeometryKind;

typedef enum BTBHGPUSceneBlendMode {
    BTBH_GPU_SCENE_BLEND_ALPHA = 0,
    BTBH_GPU_SCENE_BLEND_ADDITIVE = 1,
    BTBH_GPU_SCENE_BLEND_MULTIPLIED = 2,
    BTBH_GPU_SCENE_BLEND_PREMULTIPLIED_ALPHA = 3,
    BTBH_GPU_SCENE_BLEND_COUNT = 4
} BTBHGPUSceneBlendMode;

typedef enum BTBHGPUSceneBillboardMode {
    BTBH_GPU_SCENE_BILLBOARD_DISABLED = 0,
    BTBH_GPU_SCENE_BILLBOARD_FRONT = 1,
    BTBH_GPU_SCENE_BILLBOARD_Y_AXIS = 2
} BTBHGPUSceneBillboardMode;

typedef enum BTBHGPUSceneDepthCompare {
    BTBH_GPU_SCENE_DEPTH_ALWAYS = 0,
    BTBH_GPU_SCENE_DEPTH_LESS_EQUAL = 1,
    BTBH_GPU_SCENE_DEPTH_GREATER = 2,
    BTBH_GPU_SCENE_DEPTH_NOT_EQUAL = 3,
    BTBH_GPU_SCENE_DEPTH_LESS = 4,
    BTBH_GPU_SCENE_DEPTH_EQUAL = 5,
    BTBH_GPU_SCENE_DEPTH_GREATER_EQUAL = 6,
    BTBH_GPU_SCENE_DEPTH_NEVER = 7,
    BTBH_GPU_SCENE_DEPTH_COUNT = 8
} BTBHGPUSceneDepthCompare;

typedef struct BTBHGPUSceneGeometryBatch {
    BTBHGPUSceneGeometryKind kind;
    BTBHGPUModel *model;
    BTBHGPUCustomMesh *custom_mesh;
    BTBHGPUAnimationLibrary *animation_library;
    uint32_t animation_index;
    float animation_time;
    int32_t material_slot;
    uint32_t first_instance;
    uint32_t instance_count;
    BTBHGPUTexture *albedo_texture;
    uint8_t albedo_red;
    uint8_t albedo_green;
    uint8_t albedo_blue;
    uint8_t albedo_alpha;
    BTBHGPUTexture *emission_texture;
    uint8_t emission_red;
    uint8_t emission_green;
    uint8_t emission_blue;
    uint8_t emission_alpha;
    float emission_energy;
    BTBHGPUSceneBlendMode blend_mode;
    BTBHGPUSceneBillboardMode billboard_mode;
    BTBHGPUSceneDepthCompare depth_compare;
    uint8_t transparency_enabled;
    uint8_t unlit_enabled;
    uint8_t reserved;
} BTBHGPUSceneGeometryBatch;

typedef struct BTBHGPUSceneLighting {
    float ambient_red;
    float ambient_green;
    float ambient_blue;
    float ambient_energy;
    float directional_x;
    float directional_y;
    float directional_z;
    float directional_energy;
    float directional_red;
    float directional_green;
    float directional_blue;
    float secondary_directional_energy;
    float secondary_directional_x;
    float secondary_directional_y;
    float secondary_directional_z;
    float reserved;
    float secondary_directional_red;
    float secondary_directional_green;
    float secondary_directional_blue;
    float secondary_reserved;
} BTBHGPUSceneLighting;

typedef struct BTBHGPUSceneReport {
    BTBHGPUResult result;
    uint32_t drawable_width;
    uint32_t drawable_height;
    uint32_t batch_count;
    uint32_t primitive_draw_count;
    uint32_t prepared_geometry_count;
    uint32_t instance_count;
    char message[256];
} BTBHGPUSceneReport;

typedef struct BTBHGPUFrameTelemetrySample {
    uint64_t frame_serial;
    double cpu_submit_milliseconds;
    double fence_completion_milliseconds;
} BTBHGPUFrameTelemetrySample;

const char *btbh_gpu_backend_driver_name(BTBHGPUBackendRequest backend);
BTBHGPUShaderFormat btbh_gpu_backend_shader_format(BTBHGPUBackendRequest backend);
BTBHGPUResult btbh_gpu_validate_capabilities(
    BTBHGPUBackendRequest backend,
    BTBHGPUFeatureFlags required_features,
    const BTBHGPUCapabilitySnapshot *snapshot,
    BTBHGPUValidationReport *report
);
BTBHGPUDevice *btbh_gpu_device_create(
    const BTBHGPUDeviceDescriptor *descriptor,
    BTBHGPUDeviceReport *report
);
void btbh_gpu_device_shutdown(BTBHGPUDevice *device);
void btbh_gpu_device_destroy(BTBHGPUDevice *device);
double btbh_gpu_monotonic_elapsed_seconds(void);
BTBHGPUWindow *btbh_gpu_window_create(
    const BTBHGPUWindowDescriptor *descriptor,
    BTBHGPUWindowReport *report
);
BTBHGPUResult btbh_gpu_window_claim(
    BTBHGPUDevice *device,
    BTBHGPUWindow *window,
    BTBHGPUWindowReport *report
);
BTBHGPUResult btbh_gpu_window_resize(
    BTBHGPUWindow *window,
    uint32_t width,
    uint32_t height,
    BTBHGPUWindowReport *report
);
BTBHGPUResult btbh_gpu_window_minimize(
    BTBHGPUWindow *window,
    BTBHGPUWindowReport *report
);
BTBHGPUResult btbh_gpu_window_restore(
    BTBHGPUWindow *window,
    BTBHGPUWindowReport *report
);
BTBHGPUResult btbh_gpu_window_present_clear(
    BTBHGPUWindow *window,
    uint8_t red,
    uint8_t green,
    uint8_t blue,
    uint8_t alpha,
    BTBHGPUWindowReport *report
);
BTBHGPUResult btbh_gpu_window_poll_events(
    BTBHGPUWindow *window,
    BTBHGPUPlatformSnapshot *snapshot
);
BTBHGPUResult btbh_gpu_window_begin_frame(
    BTBHGPUWindow *window,
    BTBHGPUWindowReport *report,
    BTBHGPUPlatformSnapshot *snapshot
);
void btbh_gpu_window_set_frame_telemetry_enabled(
    BTBHGPUWindow *window,
    uint8_t enabled
);
uint32_t btbh_gpu_window_poll_frame_telemetry(
    BTBHGPUWindow *window,
    BTBHGPUFrameTelemetrySample *samples,
    uint32_t capacity
);
uint32_t btbh_gpu_window_finish_frame_telemetry(
    BTBHGPUWindow *window,
    BTBHGPUFrameTelemetrySample *samples,
    uint32_t capacity
);
BTBHGPUResult btbh_gpu_window_set_mouse_position(
    BTBHGPUWindow *window,
    float x,
    float y
);
BTBHGPUResult btbh_gpu_window_set_cursor_visible(
    BTBHGPUWindow *window,
    uint8_t visible
);
BTBHGPUResult btbh_gpu_window_set_clipboard_text(
    BTBHGPUWindow *window,
    const char *text
);
uint8_t btbh_gpu_window_key_down(
    BTBHGPUWindow *window,
    uint16_t project_key
);
uint8_t btbh_gpu_window_key_pressed(
    BTBHGPUWindow *window,
    uint16_t project_key
);
uint8_t btbh_gpu_window_mouse_button_down(
    BTBHGPUWindow *window,
    uint8_t project_button
);
uint8_t btbh_gpu_window_mouse_button_pressed(
    BTBHGPUWindow *window,
    uint8_t project_button
);
uint8_t btbh_gpu_window_mouse_button_released(
    BTBHGPUWindow *window,
    uint8_t project_button
);
BTBHGPUTexture *btbh_gpu_window_texture_load(
    BTBHGPUWindow *window,
    const char *path,
    uint8_t generate_mipmaps,
    BTBHGPUTextureFilter filter,
    BTBHGPUTextureWrap wrap,
    BTBHGPUTextureReport *report
);
BTBHGPUTexture *btbh_gpu_window_render_target_create(
    BTBHGPUWindow *window,
    uint32_t width,
    uint32_t height,
    BTBHGPUTextureFilter filter,
    BTBHGPUTextureReport *report
);
void btbh_gpu_texture_destroy(BTBHGPUTexture *texture);
BTBHGPUFont *btbh_gpu_window_font_load(
    BTBHGPUWindow *window,
    const char *path,
    uint32_t base_size,
    const uint32_t *glyphs,
    uint32_t glyph_count,
    BTBHGPUFontReport *report
);
BTBHGPUResult btbh_gpu_font_glyph(
    const BTBHGPUFont *font,
    uint32_t codepoint,
    BTBHGPUFontGlyph *glyph
);
BTBHGPUTexture *btbh_gpu_font_atlas(BTBHGPUFont *font);
void btbh_gpu_font_destroy(BTBHGPUFont *font);
BTBHGPUResult btbh_gpu_window_canvas_present(
    BTBHGPUWindow *window,
    const char *vertex_shader_path,
    const char *fragment_shader_path,
    const char *textured_fragment_shader_path,
    BTBHGPUShaderFormat shader_format,
    const BTBHGPUCanvasVertex *vertices,
    uint32_t vertex_count,
    const BTBHGPUCanvasBatch *batches,
    uint32_t batch_count,
    const BTBHGPUCanvasPass *passes,
    uint32_t pass_count,
    BTBHGPUCanvasReport *report
);
BTBHGPUTexture *btbh_gpu_window_capture_pending_scene_texture(
    BTBHGPUWindow *window,
    BTBHGPUTextureReport *report
);
void btbh_gpu_window_request_presented_frame_readback(
    BTBHGPUWindow *window
);
BTBHGPUResult btbh_gpu_window_readback_presented_frame(
    BTBHGPUWindow *window,
    uint8_t *rgba8,
    uint32_t byte_capacity,
    BTBHGPUCanvasReport *report
);
BTBHGPUResult btbh_gpu_write_png_rgba8(
    const char *path,
    uint32_t width,
    uint32_t height,
    const uint8_t *rgba8
);
BTBHGPUModel *btbh_gpu_window_model_load(
    BTBHGPUWindow *window,
    const char *path,
    BTBHGPUModelReport *report
);
int32_t btbh_gpu_model_material_slot(
    const BTBHGPUModel *model,
    uint32_t mesh_index
);
BTBHGPUResult btbh_gpu_model_material_info(
    const BTBHGPUModel *model,
    uint32_t material_index,
    BTBHGPUModelMaterialInfo *info
);
void btbh_gpu_model_destroy(BTBHGPUModel *model);
BTBHGPUInstanceBuffer *btbh_gpu_window_instance_buffer_create(
    BTBHGPUWindow *window,
    uint32_t capacity,
    uint32_t stride,
    BTBHGPUInstanceBufferReport *report
);
BTBHGPUResult btbh_gpu_instance_buffer_upload(
    BTBHGPUInstanceBuffer *buffer,
    uint32_t byte_offset,
    const void *bytes,
    uint32_t byte_count,
    BTBHGPUInstanceBufferReport *report
);
void btbh_gpu_instance_buffer_destroy(BTBHGPUInstanceBuffer *buffer);
BTBHGPUCustomMesh *btbh_gpu_window_custom_mesh_create(
    BTBHGPUWindow *window,
    const void *vertices,
    uint32_t vertex_byte_count,
    uint32_t vertex_count,
    const uint32_t *indices,
    uint32_t index_count,
    BTBHGPUCustomMeshReport *report
);
BTBHGPUResult btbh_gpu_custom_mesh_update(
    BTBHGPUCustomMesh *mesh,
    const void *vertices,
    uint32_t vertex_byte_count,
    const uint32_t *indices,
    uint32_t index_count,
    BTBHGPUCustomMeshReport *report
);
void btbh_gpu_custom_mesh_destroy(BTBHGPUCustomMesh *mesh);
BTBHGPUResult btbh_gpu_window_scene_present_instances(
    BTBHGPUWindow *window,
    const char *vertex_shader_path,
    const char *animated_vertex_shader_path,
    const char *fragment_shader_path,
    BTBHGPUShaderFormat shader_format,
    const float view_projection[16],
    const float camera_right[3],
    const float camera_up[3],
    const BTBHGPUSceneLighting *lighting,
    const float *instances,
    uint32_t total_instance_count,
    const BTBHGPUSceneGeometryBatch *batches,
    uint32_t batch_count,
    uint8_t clear_red,
    uint8_t clear_green,
    uint8_t clear_blue,
    uint8_t clear_alpha,
    BTBHGPUSceneReport *report
);
BTBHGPUAnimationLibrary *btbh_gpu_animation_library_load(
    const char *path,
    BTBHGPUAnimationLibraryReport *report
);
float btbh_gpu_animation_library_duration(
    const BTBHGPUAnimationLibrary *library,
    uint32_t animation_index
);
void btbh_gpu_animation_library_destroy(
    BTBHGPUAnimationLibrary *library
);
void btbh_gpu_window_destroy(BTBHGPUWindow *window);

#if defined(__cplusplus)
}
#endif

#endif
