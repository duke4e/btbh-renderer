#include "btbh_gpu_internal.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void btbh_gpu_set_report_error(
    BTBHGPUDeviceReport *report,
    BTBHGPUResult result,
    BTBHGPUErrorStage stage,
    const char *message
) {
    report->result = result;
    report->error_stage = stage;
    (void) snprintf(
        report->message,
        sizeof(report->message),
        "%s",
        message != NULL ? message : ""
    );
}

static bool btbh_gpu_set_device_properties(
    SDL_PropertiesID properties,
    const BTBHGPUDeviceDescriptor *descriptor
) {
    const char *driver = btbh_gpu_backend_driver_name(descriptor->backend);
    if (
        !SDL_SetStringProperty(
            properties,
            SDL_PROP_GPU_DEVICE_CREATE_NAME_STRING,
            driver
        ) ||
        !SDL_SetBooleanProperty(
            properties,
            SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN,
            descriptor->debug_mode != 0
        ) ||
        !SDL_SetBooleanProperty(
            properties,
            SDL_PROP_GPU_DEVICE_CREATE_VERBOSE_BOOLEAN,
            descriptor->debug_mode != 0
        )
    ) {
        return false;
    }

    switch (descriptor->backend) {
        case BTBH_GPU_BACKEND_VULKAN:
            return
                SDL_SetBooleanProperty(
                    properties,
                    SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN,
                    true
                ) &&
                SDL_SetBooleanProperty(
                    properties,
                    SDL_PROP_GPU_DEVICE_CREATE_VULKAN_REQUIRE_HARDWARE_ACCELERATION_BOOLEAN,
                    true
                );
        case BTBH_GPU_BACKEND_METAL:
            return SDL_SetBooleanProperty(
                properties,
                SDL_PROP_GPU_DEVICE_CREATE_SHADERS_METALLIB_BOOLEAN,
                true
            );
        case BTBH_GPU_BACKEND_INVALID:
        default:
            return false;
    }
}

static bool btbh_gpu_supports_color(
    SDL_GPUDevice *device,
    SDL_GPUTextureFormat format
) {
    return SDL_GPUTextureSupportsFormat(
        device,
        format,
        SDL_GPU_TEXTURETYPE_2D,
        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
            SDL_GPU_TEXTUREUSAGE_SAMPLER
    );
}

static bool btbh_gpu_supports_depth_type(
    SDL_GPUDevice *device,
    SDL_GPUTextureType type
) {
    const SDL_GPUTextureUsageFlags usage =
        SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET |
        SDL_GPU_TEXTUREUSAGE_SAMPLER;
    return
        SDL_GPUTextureSupportsFormat(
            device,
            SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
            type,
            usage
        ) ||
        SDL_GPUTextureSupportsFormat(
            device,
            SDL_GPU_TEXTUREFORMAT_D24_UNORM,
            type,
            usage
        );
}

static BTBHGPUFeatureFlags btbh_gpu_query_features(SDL_GPUDevice *device) {
    BTBHGPUFeatureFlags features = 0;

    if (btbh_gpu_supports_color(
        device,
        SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB
    )) {
        features |= BTBH_GPU_FEATURE_GBUFFER_RGBA8_SRGB;
    }
    if (btbh_gpu_supports_color(
        device,
        SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM
    )) {
        features |= BTBH_GPU_FEATURE_GBUFFER_RGBA8_UNORM;
    }
    if (btbh_gpu_supports_color(
        device,
        SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT
    )) {
        features |= BTBH_GPU_FEATURE_GBUFFER_RGBA16_FLOAT;
    }
    if (btbh_gpu_supports_depth_type(device, SDL_GPU_TEXTURETYPE_2D)) {
        features |= BTBH_GPU_FEATURE_SAMPLEABLE_DEPTH;
    }
    if (btbh_gpu_supports_depth_type(device, SDL_GPU_TEXTURETYPE_2D_ARRAY)) {
        features |= BTBH_GPU_FEATURE_SHADOW_2D_ARRAY;
    }
    if (btbh_gpu_supports_depth_type(device, SDL_GPU_TEXTURETYPE_CUBE_ARRAY)) {
        features |= BTBH_GPU_FEATURE_SHADOW_CUBE_ARRAY;
    }
    if (SDL_GPUTextureSupportsFormat(
        device,
        SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
        SDL_GPU_TEXTURETYPE_2D,
        SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE |
            SDL_GPU_TEXTUREUSAGE_SAMPLER
    )) {
        features |= BTBH_GPU_FEATURE_STORAGE_RGBA16_FLOAT;
    }

    features |= BTBH_GPU_FEATURE_COMPUTE;
    if (
        (features & BTBH_GPU_FEATURE_GBUFFER_RGBA8_UNORM) != 0
    ) {
        features |= BTBH_GPU_FEATURE_TEXTURE_READBACK;
    }
    return features;
}

BTBHGPUDevice *btbh_gpu_device_create(
    const BTBHGPUDeviceDescriptor *descriptor,
    BTBHGPUDeviceReport *report
) {
    if (descriptor == NULL || report == NULL) {
        return NULL;
    }

    memset(report, 0, sizeof(*report));
    if (btbh_gpu_backend_driver_name(descriptor->backend) == NULL) {
        btbh_gpu_set_report_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            BTBH_GPU_ERROR_STAGE_DEVICE_PROPERTIES,
            "Invalid GPU backend request"
        );
        return NULL;
    }

    BTBHGPUDevice *device = calloc(1, sizeof(*device));
    if (device == NULL) {
        btbh_gpu_set_report_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            BTBH_GPU_ERROR_STAGE_DEVICE_CREATION,
            "Could not allocate the GPU device context"
        );
        return NULL;
    }

    device->owns_video_subsystem =
        (SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) == 0;
    if (device->owns_video_subsystem && !SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        btbh_gpu_set_report_error(
            report,
            BTBH_GPU_PLATFORM_FAILURE,
            BTBH_GPU_ERROR_STAGE_PLATFORM_INITIALIZATION,
            SDL_GetError()
        );
        free(device);
        return NULL;
    }

    SDL_PropertiesID properties = SDL_CreateProperties();
    if (
        properties == 0 ||
        !btbh_gpu_set_device_properties(properties, descriptor)
    ) {
        btbh_gpu_set_report_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            BTBH_GPU_ERROR_STAGE_DEVICE_PROPERTIES,
            SDL_GetError()
        );
        if (properties != 0) {
            SDL_DestroyProperties(properties);
        }
        btbh_gpu_device_destroy(device);
        return NULL;
    }

    device->gpu = SDL_CreateGPUDeviceWithProperties(properties);
    SDL_DestroyProperties(properties);
    if (device->gpu == NULL) {
        btbh_gpu_set_report_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            BTBH_GPU_ERROR_STAGE_DEVICE_CREATION,
            SDL_GetError()
        );
        btbh_gpu_device_destroy(device);
        return NULL;
    }

    const char *driver = SDL_GetGPUDeviceDriver(device->gpu);
    (void) snprintf(
        report->capabilities.driver_name,
        sizeof(report->capabilities.driver_name),
        "%s",
        driver != NULL ? driver : ""
    );
    report->capabilities.hardware_accelerated = 1;
    report->capabilities.features = btbh_gpu_query_features(device->gpu);

    report->result = btbh_gpu_validate_capabilities(
        descriptor->backend,
        descriptor->required_features,
        &report->capabilities,
        &report->validation
    );
    if (report->result != BTBH_GPU_OK) {
        btbh_gpu_set_report_error(
            report,
            report->result,
            report->result == BTBH_GPU_BACKEND_MISMATCH
                ? BTBH_GPU_ERROR_STAGE_DRIVER_VALIDATION
                : BTBH_GPU_ERROR_STAGE_CAPABILITY_VALIDATION,
            report->result == BTBH_GPU_BACKEND_MISMATCH
                ? "SDL_GPU selected an unexpected backend"
                : "SDL_GPU device is missing required Phase 2 capabilities"
        );
        btbh_gpu_device_destroy(device);
        return NULL;
    }

    report->error_stage = BTBH_GPU_ERROR_STAGE_NONE;
    return device;
}

void btbh_gpu_device_shutdown(BTBHGPUDevice *device) {
    if (device == NULL || device->shutdown) {
        return;
    }
    device->shutdown = true;
    if (device->gpu != NULL) {
        SDL_DestroyGPUDevice(device->gpu);
        device->gpu = NULL;
    }
    if (device->owns_video_subsystem) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        device->owns_video_subsystem = false;
    }
}

void btbh_gpu_device_destroy(BTBHGPUDevice *device) {
    if (device == NULL) {
        return;
    }
    btbh_gpu_device_shutdown(device);
    free(device);
}
