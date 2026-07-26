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
#error "The Phase 2 window smoke supports Windows and macOS only"
#endif

    const BTBHGPUWindowDescriptor window_descriptor = {
        .title = "BTBH SDL_GPU window smoke",
        .width = 640,
        .height = 360,
        .hidden = 1,
        .high_pixel_density = 1
    };
    BTBHGPUWindowReport window_report = {0};
    BTBHGPUWindow *window = btbh_gpu_window_create(
        &window_descriptor,
        &window_report
    );
    if (window == NULL) {
        fprintf(stderr, "Window creation failed: %s\n", window_report.message);
        return 1;
    }

    const BTBHGPUDeviceDescriptor device_descriptor = {
        .backend = backend,
        .required_features = required_features,
        .debug_mode = 1
    };
    BTBHGPUDeviceReport device_report = {0};
    BTBHGPUDevice *device = btbh_gpu_device_create(
        &device_descriptor,
        &device_report
    );
    if (device == NULL) {
        fprintf(stderr, "Device creation failed: %s\n", device_report.message);
        btbh_gpu_window_destroy(window);
        return 1;
    }

    assert(
        btbh_gpu_window_claim(device, window, &window_report) ==
        BTBH_GPU_OK
    );
    assert(window_report.claimed != 0);
    assert(window_report.logical_width == 640);
    assert(window_report.logical_height == 360);
    assert(window_report.drawable_width > 0);
    assert(window_report.drawable_height > 0);
    assert(window_report.swapchain_format != BTBH_GPU_TEXTURE_FORMAT_INVALID);
    assert(window_report.composition == BTBH_GPU_SWAPCHAIN_SDR);
    assert(window_report.present_mode != BTBH_GPU_PRESENT_MODE_INVALID);

    assert(
        btbh_gpu_window_resize(window, 800, 450, &window_report) ==
        BTBH_GPU_OK
    );
    assert(window_report.logical_width == 800);
    assert(window_report.logical_height == 450);
    assert(window_report.drawable_width > 0);
    assert(window_report.drawable_height > 0);

    btbh_gpu_window_destroy(window);
    btbh_gpu_device_shutdown(device);
    btbh_gpu_device_destroy(device);
    return 0;
}
