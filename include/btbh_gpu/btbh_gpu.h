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

typedef struct BTBHGPUWindow BTBHGPUWindow;

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
void btbh_gpu_window_destroy(BTBHGPUWindow *window);

#if defined(__cplusplus)
}
#endif

#endif
