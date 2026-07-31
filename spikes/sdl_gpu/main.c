#include "btbh_gpu/btbh_gpu.h"

#include <inttypes.h>
#include <stdio.h>

static const BTBHGPUFeatureFlags required_features =
    BTBH_GPU_PHASE2_REQUIRED_FEATURES;

static void print_json_string(const char *value) {
    putchar('"');
    if (value != NULL) {
        for (const unsigned char *cursor = (const unsigned char *) value;
             *cursor != '\0';
             ++cursor) {
            switch (*cursor) {
                case '"': fputs("\\\"", stdout); break;
                case '\\': fputs("\\\\", stdout); break;
                case '\n': fputs("\\n", stdout); break;
                case '\r': fputs("\\r", stdout); break;
                case '\t': fputs("\\t", stdout); break;
                default:
                    if (*cursor < 0x20u) {
                        (void) printf("\\u%04x", (unsigned int) *cursor);
                    } else {
                        putchar((int) *cursor);
                    }
                    break;
            }
        }
    }
    putchar('"');
}

int main(void) {
#if defined(_WIN32)
    const BTBHGPUBackendRequest backend = BTBH_GPU_BACKEND_VULKAN;
#elif defined(__APPLE__)
    const BTBHGPUBackendRequest backend = BTBH_GPU_BACKEND_METAL;
#else
#error "The Phase 2 spike supports Windows/Vulkan and macOS/Metal only"
#endif

    const BTBHGPUDeviceDescriptor descriptor = {
        .backend = backend,
        .required_features = required_features,
        .debug_mode = 1
    };
    BTBHGPUDeviceReport report = {0};
    BTBHGPUDevice *device = btbh_gpu_device_create(&descriptor, &report);

    putchar('{');
    fputs("\"status\":", stdout);
    print_json_string(device != NULL ? "ok" : "failed");
    fputs(",\"requested_backend\":", stdout);
    print_json_string(btbh_gpu_backend_driver_name(backend));
    fputs(",\"actual_backend\":", stdout);
    print_json_string(report.capabilities.driver_name);
    (void) printf(
        ",\"hardware_accelerated\":%s"
        ",\"required_features\":\"0x%016" PRIx64 "\""
        ",\"supported_features\":\"0x%016" PRIx64 "\""
        ",\"missing_features\":\"0x%016" PRIx64 "\""
        ",\"result\":%d"
        ",\"error_stage\":%d"
        ",\"message\":",
        report.capabilities.hardware_accelerated != 0 ? "true" : "false",
        (uint64_t) required_features,
        (uint64_t) report.capabilities.features,
        (uint64_t) report.validation.missing_features,
        (int) report.result,
        (int) report.error_stage
    );
    print_json_string(report.message);
    fputs("}\n", stdout);

    if (device == NULL) {
        return 1;
    }
    btbh_gpu_device_destroy(device);
    return 0;
}
