#include "btbh_gpu/btbh_gpu.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static void btbh_gpu_copy_driver_name(
    char destination[BTBH_GPU_DRIVER_NAME_CAPACITY],
    const char *source
) {
    if (source == NULL) {
        destination[0] = '\0';
        return;
    }
    (void) snprintf(
        destination,
        BTBH_GPU_DRIVER_NAME_CAPACITY,
        "%s",
        source
    );
}

const char *btbh_gpu_backend_driver_name(BTBHGPUBackendRequest backend) {
    switch (backend) {
        case BTBH_GPU_BACKEND_VULKAN:
            return "vulkan";
        case BTBH_GPU_BACKEND_METAL:
            return "metal";
        case BTBH_GPU_BACKEND_INVALID:
        default:
            return NULL;
    }
}

BTBHGPUShaderFormat btbh_gpu_backend_shader_format(BTBHGPUBackendRequest backend) {
    switch (backend) {
        case BTBH_GPU_BACKEND_VULKAN:
            return BTBH_GPU_SHADER_FORMAT_SPIRV;
        case BTBH_GPU_BACKEND_METAL:
            return BTBH_GPU_SHADER_FORMAT_METALLIB;
        case BTBH_GPU_BACKEND_INVALID:
        default:
            return BTBH_GPU_SHADER_FORMAT_INVALID;
    }
}

BTBHGPUResult btbh_gpu_validate_capabilities(
    BTBHGPUBackendRequest backend,
    BTBHGPUFeatureFlags required_features,
    const BTBHGPUCapabilitySnapshot *snapshot,
    BTBHGPUValidationReport *report
) {
    const char *expected_driver = btbh_gpu_backend_driver_name(backend);
    if (expected_driver == NULL || snapshot == NULL || report == NULL) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }

    memset(report, 0, sizeof(*report));
    btbh_gpu_copy_driver_name(report->expected_driver, expected_driver);
    btbh_gpu_copy_driver_name(report->actual_driver, snapshot->driver_name);

    if (strcmp(expected_driver, snapshot->driver_name) != 0) {
        report->result = BTBH_GPU_BACKEND_MISMATCH;
        return report->result;
    }

    if (
        backend == BTBH_GPU_BACKEND_VULKAN &&
        snapshot->hardware_accelerated == 0
    ) {
        report->result = BTBH_GPU_SOFTWARE_DEVICE;
        return report->result;
    }

    report->missing_features = required_features & ~snapshot->features;
    if (report->missing_features != 0) {
        report->result = BTBH_GPU_MISSING_CAPABILITY;
        return report->result;
    }

    report->result = BTBH_GPU_OK;
    return report->result;
}
