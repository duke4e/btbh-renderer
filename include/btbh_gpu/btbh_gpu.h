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

typedef struct BTBHGPUSceneGeometryBatch {
    BTBHGPUSceneGeometryKind kind;
    BTBHGPUModel *model;
    BTBHGPUCustomMesh *custom_mesh;
    BTBHGPUAnimationLibrary *animation_library;
    uint32_t animation_index;
    float animation_time;
    uint32_t first_instance;
    uint32_t instance_count;
} BTBHGPUSceneGeometryBatch;

typedef struct BTBHGPUSceneReport {
    BTBHGPUResult result;
    uint32_t drawable_width;
    uint32_t drawable_height;
    uint32_t batch_count;
    uint32_t primitive_draw_count;
    uint32_t instance_count;
    char message[256];
} BTBHGPUSceneReport;

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
void btbh_gpu_texture_destroy(BTBHGPUTexture *texture);
BTBHGPUModel *btbh_gpu_window_model_load(
    BTBHGPUWindow *window,
    const char *path,
    BTBHGPUModelReport *report
);
int32_t btbh_gpu_model_material_slot(
    const BTBHGPUModel *model,
    uint32_t mesh_index
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
