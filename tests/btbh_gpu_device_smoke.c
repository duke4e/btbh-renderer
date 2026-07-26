#include "btbh_gpu/btbh_gpu.h"

#include <assert.h>
#include <stdio.h>

static const BTBHGPUFeatureFlags required_features =
    BTBH_GPU_PHASE2_REQUIRED_FEATURES;

int main(void) {
#if defined(_WIN32)
    const BTBHGPUBackendRequest backend = BTBH_GPU_BACKEND_VULKAN;
#elif defined(__APPLE__)
    const BTBHGPUBackendRequest backend = BTBH_GPU_BACKEND_METAL;
#else
#error "The Phase 2 smoke test supports Windows/Vulkan and macOS/Metal only"
#endif

    const BTBHGPUDeviceDescriptor descriptor = {
        .backend = backend,
        .required_features = required_features,
        .debug_mode = 1
    };
    BTBHGPUDeviceReport report = {0};
    BTBHGPUDevice *device = btbh_gpu_device_create(&descriptor, &report);

    if (device == NULL) {
        fprintf(
            stderr,
            "GPU device smoke failed at stage %d: %s\n",
            (int) report.error_stage,
            report.message
        );
        return 1;
    }

    assert(report.result == BTBH_GPU_OK);
    assert(report.validation.result == BTBH_GPU_OK);
    assert(report.validation.missing_features == 0);
    assert(report.capabilities.features == required_features);
    assert(report.capabilities.hardware_accelerated != 0);
    assert(report.capabilities.driver_name[0] != '\0');

    btbh_gpu_device_shutdown(device);
    btbh_gpu_device_shutdown(device);
    btbh_gpu_device_destroy(device);
    return 0;
}
