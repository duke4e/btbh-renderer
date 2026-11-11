#include "btbh_gpu/btbh_gpu.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint64_t required_features =
    BTBH_GPU_REQUIRED_FEATURES;

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

/* present_mode takes the first reserved byte after display_percent, so the window descriptor
 * keeps its size (0.3.0). */
_Static_assert(
    offsetof(BTBHGPUWindowDescriptor, present_mode) ==
        offsetof(BTBHGPUWindowDescriptor, display_percent) + 1u,
    "present_mode must follow display_percent"
);
_Static_assert(
    sizeof(void *) != 8u || sizeof(BTBHGPUWindowDescriptor) == 24u,
    "the window descriptor must keep its 64-bit size"
);

/* The mirror flag takes the padding byte after upscale_mode, so the
 * radiance-cascade block keeps its offset and the struct its size. */
_Static_assert(
    offsetof(BTBHGPUScenePost, mirror_horizontal) ==
        offsetof(BTBHGPUScenePost, upscale_mode) + 1u,
    "mirror_horizontal must follow upscale_mode"
);
_Static_assert(
    sizeof(void *) != 8u ||
        offsetof(BTBHGPUScenePost, radiance_cascades) == 72u,
    "the scene post radiance block must keep its 64-bit offset"
);

static void test_scene_post_mirror_flag_defaults_off(void) {
    BTBHGPUScenePost post = {0};
    if (post.mirror_horizontal != 0u) {
        fprintf(stderr, "zeroed scene post must not mirror\n");
        abort();
    }
}

/* Fog, bloom and vignette were appended after the radiance block, so older
 * offsets are unchanged and a zeroed post keeps the composite off. */
_Static_assert(
    offsetof(BTBHGPUScenePost, fog_color) >
        offsetof(BTBHGPUScenePost, radiance_cascades),
    "the HDR composite fields must follow the radiance block"
);

static void test_scene_post_hdr_composite_defaults_off(void) {
    BTBHGPUScenePost post = {0};
    if (post.fog_enabled != 0u || post.bloom_enabled != 0u ||
        post.vignette_enabled != 0u || post.fog_edge_enabled != 0u) {
        fprintf(stderr, "zeroed scene post must not fog, bloom or vignette\n");
        abort();
    }
}

/* Surface detail was appended after the surface parameters, so a zeroed
 * geometry batch keeps every older field and draws without detail. */
_Static_assert(
    offsetof(BTBHGPUSceneGeometryBatch, detail_parameters) ==
        offsetof(BTBHGPUSceneGeometryBatch, surface_parameters1) +
            4u * sizeof(float),
    "detail_parameters must follow surface_parameters1"
);

static void test_geometry_batch_detail_defaults_off(void) {
    BTBHGPUSceneGeometryBatch batch = {0};
    if (batch.detail_parameters[3] != 0.0f) {
        fprintf(stderr, "zeroed geometry batch must not draw surface detail\n");
        abort();
    }
}

static int file_dialog_callback_count = 0;

static void file_dialog_callback(
    void *userdata,
    const char *const *paths,
    const char *error_message
) {
    (void) userdata;
    (void) paths;
    (void) error_message;
    file_dialog_callback_count += 1;
}

static void test_file_dialog_rejects_invalid_ownership_transfer(void) {
    const char *names[] = {"JSON"};
    const char *patterns[] = {"json"};
    BTBHGPUFileDialogDescriptor descriptor = {0};
    descriptor.kind = BTBH_GPU_FILE_DIALOG_OPEN;
    descriptor.title = "Open";
    descriptor.filter_names = names;
    descriptor.filter_patterns = patterns;
    descriptor.filter_count = 1;
    BTBHGPUFileDialogReport report = {0};

    assert(
        btbh_gpu_window_show_file_dialog(
            NULL,
            &descriptor,
            file_dialog_callback,
            NULL,
            &report
        ) == BTBH_GPU_INVALID_ARGUMENT
    );
    assert(report.result == BTBH_GPU_INVALID_ARGUMENT);
    assert(strcmp(
        report.message,
        "Invalid GPU file-dialog descriptor"
    ) == 0);
    assert(file_dialog_callback_count == 0);
    assert(
        btbh_gpu_window_show_file_dialog(
            NULL,
            &descriptor,
            file_dialog_callback,
            NULL,
            NULL
        ) == BTBH_GPU_INVALID_ARGUMENT
    );
    assert(file_dialog_callback_count == 0);
}

int main(void) {
    test_backend_contract();
    test_exact_driver_is_required();
    test_software_vulkan_is_rejected();
    test_missing_features_are_reported_exactly();
    test_matching_snapshot_passes();
    test_scene_post_mirror_flag_defaults_off();
    test_scene_post_hdr_composite_defaults_off();
    test_geometry_batch_detail_defaults_off();
    test_file_dialog_rejects_invalid_ownership_transfer();
    return 0;
}
