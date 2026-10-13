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
#define BTBH_GPU_REQUIRED_FEATURES ( \
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
    /* Non-zero lets the user resize and maximize the window. */
    uint8_t resizable;
    /* Non-zero makes Alt+Enter switch between the window and borderless desktop
       fullscreen; that key press never reaches the key state. */
    uint8_t fullscreen_toggle;
    /* 1...100 sizes the window to that percentage of the primary display (keeping the
       width:height aspect and fitting the usable area); 0 uses width x height exactly. */
    uint8_t display_percent;
    uint8_t reserved[3];
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

/* Button bits follow the public GamepadButton order: south, east, west,
 * north, left/right shoulder, left/right stick, start, back, guide,
 * dpad up/down/left/right. Axis values are normalized to [-1, 1]; triggers
 * use [0, 1]. instance_id == 0 means disconnected. */
typedef struct BTBHGPUGamepadSnapshot {
    uint32_t instance_id;
    uint32_t buttons_down;
    uint32_t buttons_pressed;
    float left_x;
    float left_y;
    float right_x;
    float right_y;
    float left_trigger;
    float right_trigger;
} BTBHGPUGamepadSnapshot;

typedef enum BTBHGPUFileDialogKind {
    BTBH_GPU_FILE_DIALOG_OPEN = 1,
    BTBH_GPU_FILE_DIALOG_SAVE = 2
} BTBHGPUFileDialogKind;

typedef struct BTBHGPUFileDialogDescriptor {
    BTBHGPUFileDialogKind kind;
    const char *title;
    const char *const *filter_names;
    const char *const *filter_patterns;
    uint32_t filter_count;
    uint8_t allows_multiple_selection;
    uint8_t reserved[3];
    const char *location;
} BTBHGPUFileDialogDescriptor;

typedef struct BTBHGPUFileDialogReport {
    BTBHGPUResult result;
    char message[256];
} BTBHGPUFileDialogReport;

typedef void (*BTBHGPUFileDialogCallback)(
    void *userdata,
    const char *const *paths,
    const char *error_message
);

typedef struct BTBHGPUWindow BTBHGPUWindow;
typedef struct BTBHGPUTexture BTBHGPUTexture;
typedef struct BTBHGPUFont BTBHGPUFont;
typedef struct BTBHGPUModel BTBHGPUModel;
typedef struct BTBHGPUInstanceBuffer BTBHGPUInstanceBuffer;
typedef struct BTBHGPUCustomMesh BTBHGPUCustomMesh;
typedef struct BTBHGPUAnimationLibrary BTBHGPUAnimationLibrary;
typedef struct BTBHGPUAnimationPose BTBHGPUAnimationPose;

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

typedef enum BTBHGPUTextureColorSpace {
    BTBH_GPU_TEXTURE_COLOR_SPACE_LINEAR = 0,
    BTBH_GPU_TEXTURE_COLOR_SPACE_SRGB = 1
} BTBHGPUTextureColorSpace;

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

enum {
    BTBH_GPU_MODEL_IMPORT_IGNORE_MATERIAL_TEXTURES = 1u << 2
};

typedef struct BTBHGPUModelMaterialInfo {
    BTBHGPUTexture *albedo_texture;
    uint8_t albedo_red;
    uint8_t albedo_green;
    uint8_t albedo_blue;
    uint8_t albedo_alpha;
    BTBHGPUTexture *normal_texture;
    BTBHGPUTexture *orm_texture;
    float normal_scale;
    float occlusion;
    float roughness;
    float metalness;
    BTBHGPUTexture *emission_texture;
    uint8_t emission_red;
    uint8_t emission_green;
    uint8_t emission_blue;
    uint8_t emission_alpha;
    float emission_energy;
    float alpha_cutoff;
    uint8_t unlit_enabled;
    uint8_t double_sided_enabled;
    uint8_t transparency_enabled;
    uint8_t reserved;
} BTBHGPUModelMaterialInfo;

typedef struct BTBHGPUModelMeshInfo {
    uint32_t vertex_count;
    uint32_t index_count;
} BTBHGPUModelMeshInfo;

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

typedef enum BTBHGPUSceneSurfaceVariant {
    BTBH_GPU_SCENE_SURFACE_NONE = 0,
    BTBH_GPU_SCENE_SURFACE_GLITTER_STATIC = 1,
    BTBH_GPU_SCENE_SURFACE_WATER = 2,
    BTBH_GPU_SCENE_SURFACE_DISSOLVE = 3,
    BTBH_GPU_SCENE_SURFACE_SHATTER = 4,
    BTBH_GPU_SCENE_SURFACE_GLITTER = 5,
    BTBH_GPU_SCENE_SURFACE_SOFT_PARTICLE = 6,
    /* A self-lit rim in a tint (parameters0: rgb tint, w rim strength; parameters1.x body glow). */
    BTBH_GPU_SCENE_SURFACE_RIM = 7,
    BTBH_GPU_SCENE_SURFACE_COUNT = 8
} BTBHGPUSceneSurfaceVariant;

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

typedef enum BTBHGPUSceneCullMode {
    BTBH_GPU_SCENE_CULL_NONE = 0,
    BTBH_GPU_SCENE_CULL_FRONT = 1,
    BTBH_GPU_SCENE_CULL_BACK = 2,
    BTBH_GPU_SCENE_CULL_COUNT = 3
} BTBHGPUSceneCullMode;

typedef enum BTBHGPUSceneStencilOperation {
    BTBH_GPU_SCENE_STENCIL_KEEP = 0,
    BTBH_GPU_SCENE_STENCIL_ZERO = 1,
    BTBH_GPU_SCENE_STENCIL_REPLACE = 2,
    BTBH_GPU_SCENE_STENCIL_INCREMENT_CLAMP = 3,
    BTBH_GPU_SCENE_STENCIL_DECREMENT_CLAMP = 4,
    BTBH_GPU_SCENE_STENCIL_OPERATION_COUNT = 5
} BTBHGPUSceneStencilOperation;

typedef enum BTBHGPUSceneUpscaleMode {
    BTBH_GPU_SCENE_UPSCALE_LINEAR = 0,
    BTBH_GPU_SCENE_UPSCALE_LANCZOS = 1,
    /* AMD FidelityFX Super Resolution 1: EASU upscale, then RCAS sharpening at the output size. */
    BTBH_GPU_SCENE_UPSCALE_FSR1 = 2
} BTBHGPUSceneUpscaleMode;

typedef enum BTBHGPUSceneTonemapMode {
    BTBH_GPU_SCENE_TONEMAP_REINHARD = 0,
    BTBH_GPU_SCENE_TONEMAP_PBR_NEUTRAL = 1,
    BTBH_GPU_SCENE_TONEMAP_ACES = 2,
    BTBH_GPU_SCENE_TONEMAP_COUNT = 3
} BTBHGPUSceneTonemapMode;

typedef enum BTBHGPUSceneRadianceCascadesTier {
    BTBH_GPU_SCENE_RC_OFF = 0,
    BTBH_GPU_SCENE_RC_LOW = 1,
    BTBH_GPU_SCENE_RC_HIGH = 2,
    BTBH_GPU_SCENE_RC_AUTO = 3
} BTBHGPUSceneRadianceCascadesTier;

typedef enum BTBHGPUSceneRadianceCascadesBackend {
    BTBH_GPU_SCENE_RC_BACKEND_HOLOGRAPHIC = 0
} BTBHGPUSceneRadianceCascadesBackend;

typedef enum BTBHGPUSceneRadianceCascadesDebugView {
    BTBH_GPU_SCENE_RC_DEBUG_NONE = 0,
    BTBH_GPU_SCENE_RC_DEBUG_CASCADE_0 = 1,
    BTBH_GPU_SCENE_RC_DEBUG_CASCADE_1 = 2,
    BTBH_GPU_SCENE_RC_DEBUG_CASCADE_2 = 3,
    BTBH_GPU_SCENE_RC_DEBUG_CASCADE_3 = 4,
    BTBH_GPU_SCENE_RC_DEBUG_HIT_DISTANCE = 5,
    BTBH_GPU_SCENE_RC_DEBUG_CONFIDENCE = 6,
    BTBH_GPU_SCENE_RC_DEBUG_FINAL_INDIRECT = 7,
    BTBH_GPU_SCENE_RC_DEBUG_FALLBACK_REASON = 8,
    BTBH_GPU_SCENE_RC_DEBUG_DIRECTIONAL_SOURCE = 9,
    BTBH_GPU_SCENE_RC_DEBUG_DIRECT_LIGHT = 10
} BTBHGPUSceneRadianceCascadesDebugView;

typedef enum BTBHGPUSceneRadianceCascadesFallbackReason {
    BTBH_GPU_SCENE_RC_FALLBACK_NONE = 0,
    BTBH_GPU_SCENE_RC_FALLBACK_DISABLED = 1,
    BTBH_GPU_SCENE_RC_FALLBACK_CAPABILITY_UNAVAILABLE = 2,
    BTBH_GPU_SCENE_RC_FALLBACK_SHADER_UNAVAILABLE = 3,
    BTBH_GPU_SCENE_RC_FALLBACK_PIPELINE_CREATION_FAILED = 4,
    BTBH_GPU_SCENE_RC_FALLBACK_RESOURCE_ALLOCATION_FAILED = 5,
    BTBH_GPU_SCENE_RC_FALLBACK_DISPATCH_FAILED = 6,
    BTBH_GPU_SCENE_RC_FALLBACK_INVALID_CONFIGURATION = 7
} BTBHGPUSceneRadianceCascadesFallbackReason;

#define BTBH_GPU_RC_MAX_OCCLUSION_CASCADES 3u

typedef enum BTBHGPUSceneRadianceEmitterShape {
    BTBH_GPU_SCENE_RADIANCE_EMITTER_DISK = 0,
    BTBH_GPU_SCENE_RADIANCE_EMITTER_RECTANGLE = 1,
    BTBH_GPU_SCENE_RADIANCE_EMITTER_SEGMENT = 2
} BTBHGPUSceneRadianceEmitterShape;

typedef enum BTBHGPUSceneRadianceEmitterClass {
    BTBH_GPU_SCENE_RADIANCE_EMITTER_CLASS_HERO = 0,
    BTBH_GPU_SCENE_RADIANCE_EMITTER_CLASS_WORLD = 1,
    BTBH_GPU_SCENE_RADIANCE_EMITTER_CLASS_PICKUP = 2,
    BTBH_GPU_SCENE_RADIANCE_EMITTER_CLASS_TRANSIENT = 3
} BTBHGPUSceneRadianceEmitterClass;

typedef struct BTBHGPUSceneRadianceEmitter {
    uint64_t stable_id;
    float position_x;
    float position_y;
    float position_z;
    float radius;
    float height;
    float red;
    float green;
    float blue;
    float energy;
    float direct_energy;
    float direction_x;
    float direction_y;
    float direction_z;
    float inner_cutoff;
    float outer_cutoff;
    BTBHGPUSceneRadianceEmitterShape shape;
    BTBHGPUSceneRadianceEmitterClass light_class;
    uint32_t reserved;
} BTBHGPUSceneRadianceEmitter;

typedef struct BTBHGPUSceneRadianceCascadesOcclusionCascade {
    float origin_world_x;
    float origin_world_z;
    float cell_size;
    uint32_t width;
    uint32_t height;
    uint64_t revision;
    const float *samples;
} BTBHGPUSceneRadianceCascadesOcclusionCascade;

typedef struct BTBHGPUSceneRadianceCascades {
    BTBHGPUSceneRadianceCascadesTier tier;
    BTBHGPUSceneRadianceCascadesBackend backend;
    BTBHGPUSceneRadianceCascadesDebugView debug_view;
    float auto_frame_budget_milliseconds;
    float indirect_intensity;
    uint32_t emitter_count;
    int32_t hero_omni_emitter_index;
    uint32_t occlusion_cascade_count;
    uint8_t allow_auto_transition;
    uint8_t invalidate;
    uint8_t reserved[2];
    const BTBHGPUSceneRadianceEmitter *emitters;
    BTBHGPUSceneRadianceCascadesOcclusionCascade
        occlusion_cascades[BTBH_GPU_RC_MAX_OCCLUSION_CASCADES];
} BTBHGPUSceneRadianceCascades;

typedef struct BTBHGPUSceneRadianceCascadesPassTelemetry {
    double cpu_prepare_milliseconds;
    double cpu_pipeline_prepare_milliseconds;
    double cpu_resource_prepare_milliseconds;
    double cpu_trace_encode_milliseconds;
    double cpu_merge_up_encode_milliseconds;
    double cpu_merge_down_encode_milliseconds;
    double cpu_reproject_encode_milliseconds;
    double cpu_finish_encode_milliseconds;
    double cpu_blur_encode_milliseconds;
    double cpu_resolve_encode_milliseconds;
    double cpu_reconstruct_encode_milliseconds;
    double cpu_compose_encode_milliseconds;
    uint32_t trace_dispatch_count;
    uint32_t merge_up_dispatch_count;
    uint32_t merge_down_dispatch_count;
    uint32_t reproject_dispatch_count;
    uint32_t finish_dispatch_count;
    uint32_t blur_dispatch_count;
    uint32_t resolve_dispatch_count;
    uint32_t reconstruct_dispatch_count;
    uint32_t compose_draw_count;
    uint32_t temporal_history_valid;
    uint32_t temporal_phase;
    uint32_t fresh_quadrant_count;
    uint32_t reserved;
} BTBHGPUSceneRadianceCascadesPassTelemetry;

typedef struct BTBHGPUSceneRadianceCascadesStatus {
    BTBHGPUSceneRadianceCascadesTier requested_tier;
    BTBHGPUSceneRadianceCascadesTier active_tier;
    BTBHGPUSceneRadianceCascadesTier pending_tier;
    BTBHGPUSceneRadianceCascadesBackend requested_backend;
    BTBHGPUSceneRadianceCascadesBackend active_backend;
    BTBHGPUSceneRadianceCascadesFallbackReason fallback_reason;
    uint32_t internal_width;
    uint32_t internal_height;
    uint32_t generation;
    uint32_t sample_count;
    double cpu_encode_milliseconds;
    BTBHGPUSceneRadianceCascadesPassTelemetry pass_telemetry;
} BTBHGPUSceneRadianceCascadesStatus;

typedef struct BTBHGPUSceneGeometryBatch {
    BTBHGPUSceneGeometryKind kind;
    BTBHGPUModel *model;
    BTBHGPUCustomMesh *custom_mesh;
    BTBHGPUAnimationLibrary *animation_library;
    BTBHGPUAnimationPose *animation_pose;
    uint32_t animation_index;
    float animation_time;
    int32_t material_slot;
    BTBHGPUInstanceBuffer *instance_buffer;
    uint32_t first_instance;
    uint32_t instance_count;
    BTBHGPUTexture *albedo_texture;
    uint8_t albedo_red;
    uint8_t albedo_green;
    uint8_t albedo_blue;
    uint8_t albedo_alpha;
    float uv_scale_x;
    float uv_scale_y;
    float uv_offset_x;
    float uv_offset_y;
    BTBHGPUTexture *normal_texture;
    BTBHGPUTexture *orm_texture;
    float normal_scale;
    float occlusion;
    float roughness;
    float metalness;
    BTBHGPUTexture *emission_texture;
    uint8_t emission_red;
    uint8_t emission_green;
    uint8_t emission_blue;
    uint8_t emission_alpha;
    float emission_energy;
    BTBHGPUSceneBlendMode blend_mode;
    BTBHGPUSceneBillboardMode billboard_mode;
    BTBHGPUSceneDepthCompare depth_compare;
    float alpha_cutoff;
    BTBHGPUSceneDepthCompare stencil_compare;
    BTBHGPUSceneCullMode cull_mode;
    BTBHGPUSceneStencilOperation stencil_fail;
    BTBHGPUSceneStencilOperation stencil_depth_fail;
    BTBHGPUSceneStencilOperation stencil_pass;
    uint8_t stencil_reference;
    uint8_t stencil_mask;
    uint8_t transparency_enabled;
    uint8_t unlit_enabled;
    uint8_t spot_shadow_visible;
    BTBHGPUSceneSurfaceVariant surface_variant;
    float surface_time;
    float surface_parameters0[4];
    float surface_parameters1[4];
    /* Procedural surface detail: albedo variation, bump height (render units),
     * noise cells per render unit, pattern (0 none, 1 ground, 2 grass, 3 facade).
     * A zeroed batch has none. */
    float detail_parameters[4];
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
    float spot_position_x;
    float spot_position_y;
    float spot_position_z;
    float spot_range;
    float spot_direction_x;
    float spot_direction_y;
    float spot_direction_z;
    float spot_energy;
    float spot_red;
    float spot_green;
    float spot_blue;
    float spot_falloff;
    float spot_inner_cutoff;
    float spot_outer_cutoff;
    /* The share of the sun, fill and sky light the spot's shadow also takes where its occluder lies
       within spot_occluder_shadow_reach of the light (0 = the classic spot shadow). */
    float spot_occluder_shadow_strength;
    float spot_occluder_shadow_reach;
    float irradiance_sh[27];
    uint32_t ibl_enabled;
    float ground_bottom_red;
    float ground_bottom_green;
    float ground_bottom_blue;
    float ground_energy;
    float ground_horizon_red;
    float ground_horizon_green;
    float ground_horizon_blue;
    float ground_horizon_curve;
    float sky_top_red;
    float sky_top_green;
    float sky_top_blue;
    float sky_energy;
    float sky_horizon_red;
    float sky_horizon_green;
    float sky_horizon_blue;
    float sky_horizon_curve;
    float sun_red;
    float sun_green;
    float sun_blue;
    float sun_energy;
    float sun_direction_x;
    float sun_direction_y;
    float sun_direction_z;
    float sun_size_radians;
    float sun_curve;
    float ibl_reserved[3];
    float directional_shadow_view_projection[16];
    float secondary_directional_shadow_view_projection[16];
    float spot_shadow_view_projection[16];
    float omni_shadow_view_projection[6][16];
    uint32_t directional_shadow_enabled;
    float directional_shadow_softness;
    uint32_t secondary_directional_shadow_enabled;
    float secondary_directional_shadow_softness;
    uint32_t spot_shadow_enabled;
    float spot_shadow_softness;
    uint32_t spot_shadow_is_omni;
    float spot_shadow_near;
} BTBHGPUSceneLighting;

typedef struct BTBHGPUScenePost {
    BTBHGPUTexture *lut_texture;
    float tonemap_white;
    float exposure;
    float contrast;
    float saturation;
    float cas_sharpness;
    float focus_point_x;
    float focus_point_y;
    float focus_direction_x;
    float focus_direction_y;
    float focus_half_width;
    float maximum_radius_pixels;
    float blur_curve;
    float highlight_threshold;
    float highlight_gain;
    uint8_t lut_enabled;
    uint8_t cas_enabled;
    uint8_t tilt_shift_enabled;
    uint8_t ssao_enabled;
    uint8_t tonemap_mode;
    uint8_t anti_aliasing;
    uint8_t upscale_mode;
    /* 1 flips the final scene image left-right before it is composited. */
    uint8_t mirror_horizontal;
    BTBHGPUSceneRadianceCascades radiance_cascades;
    /* HDR composite before grading. Fog blends toward linear fog_color by
     * 1 - exp(-((view distance - start) * density)^2), capped at the maximum
     * opacity; the cleared background counts as infinitely far. */
    float fog_color[3];
    float fog_density;
    float fog_start_distance;
    float fog_maximum_opacity;
    /* Edge fade: within fog_edge_fade_distance of the world XZ rectangle
     * (min x, min z, max x, max z) the fog rises to full, so the end of the
     * world dissolves into the fog colour. Surfaces nearer than
     * fog_edge_clear_distance (view depth) keep a crisp edge; the fade ramps
     * in over the next fog_edge_fade_distance. */
    float fog_edge_bounds[4];
    float fog_edge_fade_distance;
    float fog_edge_clear_distance;
    /* Soft-threshold bloom added to the HDR image before grading; the
     * threshold is in exposed units (scene colour times exposure). */
    float bloom_intensity;
    float bloom_threshold;
    float bloom_soft_knee;
    float bloom_scatter;
    /* Round vignette that darkens the HDR image toward the corners. */
    float vignette_intensity;
    float vignette_smoothness;
    uint8_t fog_enabled;
    uint8_t bloom_enabled;
    uint8_t vignette_enabled;
    uint8_t fog_edge_enabled;
} BTBHGPUScenePost;

typedef struct BTBHGPUSceneReport {
    BTBHGPUResult result;
    uint32_t drawable_width;
    uint32_t drawable_height;
    uint32_t batch_count;
    uint32_t primitive_draw_count;
    uint32_t prepared_geometry_count;
    uint32_t instance_count;
    uint32_t transient_instance_count;
    uint32_t persistent_batch_count;
    double cpu_prepare_milliseconds;
    double cpu_upload_milliseconds;
    double cpu_shadow_encode_milliseconds;
    double cpu_swapchain_acquire_milliseconds;
    double cpu_target_ensure_milliseconds;
    double cpu_geometry_encode_milliseconds;
    double cpu_post_encode_milliseconds;
    BTBHGPUSceneRadianceCascadesStatus radiance_cascades;
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
BTBHGPUResult btbh_gpu_window_gamepad_snapshot(
    const BTBHGPUWindow *window,
    BTBHGPUGamepadSnapshot *snapshot
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
BTBHGPUResult btbh_gpu_window_show_file_dialog(
    BTBHGPUWindow *window,
    const BTBHGPUFileDialogDescriptor *descriptor,
    BTBHGPUFileDialogCallback callback,
    void *userdata,
    BTBHGPUFileDialogReport *report
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
BTBHGPUTexture *btbh_gpu_window_texture_load_ex(
    BTBHGPUWindow *window,
    const char *path,
    uint8_t generate_mipmaps,
    BTBHGPUTextureFilter filter,
    BTBHGPUTextureWrap wrap,
    BTBHGPUTextureColorSpace color_space,
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
/* Copies the finished scene into a capture of the same size and format and releases the frame,
   so another scene can render before the canvas presents. */
BTBHGPUResult btbh_gpu_window_copy_pending_scene_texture(
    BTBHGPUWindow *window,
    BTBHGPUTexture *destination,
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
BTBHGPUModel *btbh_gpu_window_model_load_ex(
    BTBHGPUWindow *window,
    const char *path,
    uint32_t import_flags,
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
BTBHGPUResult btbh_gpu_model_mesh_info(
    const BTBHGPUModel *model,
    uint32_t mesh_index,
    BTBHGPUModelMeshInfo *info
);
BTBHGPUResult btbh_gpu_model_mesh_copy_positions(
    const BTBHGPUModel *model,
    uint32_t mesh_index,
    float *positions,
    uint32_t float_capacity
);
BTBHGPUResult btbh_gpu_model_mesh_copy_indices(
    const BTBHGPUModel *model,
    uint32_t mesh_index,
    uint32_t *indices,
    uint32_t index_capacity
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
uint8_t btbh_gpu_pack_transform_stream_instances(
    float *destination,
    const void *positions,
    uint32_t position_stride,
    const void *rotations,
    uint32_t rotation_stride,
    const void *scales,
    uint32_t scale_stride,
    const void *colors,
    uint32_t color_stride,
    const void *customs,
    uint32_t custom_stride,
    uint32_t source_start,
    uint32_t count
);
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
    const char *fast_shadow_vertex_shader_path,
    const char *fast_shadow_fragment_shader_path,
    const char *shadow_vertex_shader_path,
    const char *animated_shadow_vertex_shader_path,
    const char *shadow_fragment_shader_path,
    const char *fragment_shader_path,
    const char *unshadowed_fragment_shader_path,
    const char *post_vertex_shader_path,
    const char *post_fragment_shader_path,
    BTBHGPUShaderFormat shader_format,
    uint32_t render_width,
    uint32_t render_height,
    const float view_projection[16],
    const float camera_right[3],
    const float camera_up[3],
    const float camera_position[3],
    const BTBHGPUSceneLighting *lighting,
    const BTBHGPUScenePost *post,
    const float *instances,
    uint32_t transient_instance_count,
    uint32_t logical_instance_count,
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
BTBHGPUAnimationPose *btbh_gpu_animation_pose_create(
    const BTBHGPUAnimationLibrary *library
);
uint8_t btbh_gpu_animation_pose_set_samples(
    BTBHGPUAnimationPose *pose,
    const uint32_t *animation_indices,
    const float *animation_times,
    const float *animation_weights,
    uint32_t sample_count
);
void btbh_gpu_animation_pose_destroy(
    BTBHGPUAnimationPose *pose
);
void btbh_gpu_animation_library_destroy(
    BTBHGPUAnimationLibrary *library
);
void btbh_gpu_window_destroy(BTBHGPUWindow *window);

#if defined(__cplusplus)
}
#endif

#endif
