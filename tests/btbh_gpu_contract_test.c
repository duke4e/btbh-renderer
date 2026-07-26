#include "btbh_gpu/btbh_gpu.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const uint64_t required_features =
    BTBH_GPU_PHASE2_REQUIRED_FEATURES;

static BTBHGPUCapabilitySnapshot full_snapshot(const char *driver) {
    BTBHGPUCapabilitySnapshot snapshot = {0};
    snprintf(snapshot.driver_name, sizeof(snapshot.driver_name), "%s", driver);
    snapshot.hardware_accelerated = 1;
    snapshot.features = required_features;
    return snapshot;
}

static void test_backend_contract(void) {
    assert(strcmp(
        btbh_gpu_backend_driver_name(BTBH_GPU_BACKEND_VULKAN),
        "vulkan"
    ) == 0);
    assert(strcmp(
        btbh_gpu_backend_driver_name(BTBH_GPU_BACKEND_METAL),
        "metal"
    ) == 0);
    assert(
        btbh_gpu_backend_shader_format(BTBH_GPU_BACKEND_VULKAN) ==
        BTBH_GPU_SHADER_FORMAT_SPIRV
    );
    assert(
        btbh_gpu_backend_shader_format(BTBH_GPU_BACKEND_METAL) ==
        BTBH_GPU_SHADER_FORMAT_METALLIB
    );
    assert(btbh_gpu_backend_driver_name(BTBH_GPU_BACKEND_INVALID) == NULL);
    assert(
        btbh_gpu_backend_shader_format(BTBH_GPU_BACKEND_INVALID) ==
        BTBH_GPU_SHADER_FORMAT_INVALID
    );
}

static void test_exact_driver_is_required(void) {
    BTBHGPUCapabilitySnapshot snapshot = full_snapshot("direct3d12");
    BTBHGPUValidationReport report = {0};

    assert(
        btbh_gpu_validate_capabilities(
            BTBH_GPU_BACKEND_VULKAN,
            required_features,
            &snapshot,
            &report
        ) == BTBH_GPU_BACKEND_MISMATCH
    );
    assert(report.result == BTBH_GPU_BACKEND_MISMATCH);
    assert(strcmp(report.expected_driver, "vulkan") == 0);
    assert(strcmp(report.actual_driver, "direct3d12") == 0);
}

static void test_software_vulkan_is_rejected(void) {
    BTBHGPUCapabilitySnapshot snapshot = full_snapshot("vulkan");
    BTBHGPUValidationReport report = {0};
    snapshot.hardware_accelerated = 0;

    assert(
        btbh_gpu_validate_capabilities(
            BTBH_GPU_BACKEND_VULKAN,
            required_features,
            &snapshot,
            &report
        ) == BTBH_GPU_SOFTWARE_DEVICE
    );
    assert(report.result == BTBH_GPU_SOFTWARE_DEVICE);
}

static void test_missing_features_are_reported_exactly(void) {
    BTBHGPUCapabilitySnapshot snapshot = full_snapshot("vulkan");
    BTBHGPUValidationReport report = {0};
    snapshot.features &= ~BTBH_GPU_FEATURE_STORAGE_RGBA16_FLOAT;
    snapshot.features &= ~BTBH_GPU_FEATURE_SHADOW_CUBE_ARRAY;

    assert(
        btbh_gpu_validate_capabilities(
            BTBH_GPU_BACKEND_VULKAN,
            required_features,
            &snapshot,
            &report
        ) == BTBH_GPU_MISSING_CAPABILITY
    );
    assert(
        report.missing_features ==
        (
            BTBH_GPU_FEATURE_STORAGE_RGBA16_FLOAT |
            BTBH_GPU_FEATURE_SHADOW_CUBE_ARRAY
        )
    );
}

static void test_matching_snapshot_passes(void) {
    BTBHGPUCapabilitySnapshot snapshot = full_snapshot("vulkan");
    BTBHGPUValidationReport report = {0};

    assert(
        btbh_gpu_validate_capabilities(
            BTBH_GPU_BACKEND_VULKAN,
            required_features,
            &snapshot,
            &report
        ) == BTBH_GPU_OK
    );
    assert(report.result == BTBH_GPU_OK);
    assert(report.missing_features == 0);
}

int main(void) {
    test_backend_contract();
    test_exact_driver_is_required();
    test_software_vulkan_is_rejected();
    test_missing_features_are_reported_exactly();
    test_matching_snapshot_passes();
    return 0;
}
