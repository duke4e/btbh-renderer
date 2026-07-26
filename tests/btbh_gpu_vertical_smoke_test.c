#include "btbh_gpu_vertical_smoke.h"

#include <stdio.h>

#ifndef BTBH_VERTEX_SHADER_PATH
#error "BTBH_VERTEX_SHADER_PATH must be defined"
#endif
#ifndef BTBH_FRAGMENT_SHADER_PATH
#error "BTBH_FRAGMENT_SHADER_PATH must be defined"
#endif
#ifndef BTBH_COMPUTE_SHADER_PATH
#error "BTBH_COMPUTE_SHADER_PATH must be defined"
#endif
#ifndef BTBH_SHADER_ENTRYPOINT
#error "BTBH_SHADER_ENTRYPOINT must be defined"
#endif
#ifndef BTBH_CAPTURE_PATH
#error "BTBH_CAPTURE_PATH must be defined"
#endif

int main(void)
{
    const BTBHGPUVerticalSmokeConfig config = {
        BTBH_VERTEX_SHADER_PATH,
        BTBH_FRAGMENT_SHADER_PATH,
        BTBH_COMPUTE_SHADER_PATH,
        BTBH_SHADER_ENTRYPOINT,
        BTBH_CAPTURE_PATH,
#if defined(__APPLE__)
        BTBH_GPU_BACKEND_METAL,
        BTBH_GPU_SHADER_FORMAT_METALLIB,
#else
        BTBH_GPU_BACKEND_VULKAN,
        BTBH_GPU_SHADER_FORMAT_SPIRV,
#endif
        256,
        256
    };
    BTBHGPUVerticalSmokeReport report;

    if (btbh_gpu_run_vertical_smoke(&config, &report) != 0) {
        fprintf(stderr, "vertical smoke failed: %s\n", report.message);
        return 1;
    }
    if (report.result != BTBH_GPU_OK ||
        report.width != 256 ||
        report.height != 256) {
        fprintf(stderr, "invalid vertical smoke report\n");
        return 1;
    }
    if (report.center_rgba[3] != 255 ||
        report.center_rgba[0] < 16 ||
        report.center_rgba[1] < 16 ||
        report.center_rgba[2] < 16) {
        fprintf(
            stderr,
            "unexpected center pixel: %u,%u,%u,%u\n",
            report.center_rgba[0],
            report.center_rgba[1],
            report.center_rgba[2],
            report.center_rgba[3]
        );
        return 1;
    }
    if (report.compute_center_rgba[0] < 0.49f ||
        report.compute_center_rgba[0] > 0.52f ||
        report.compute_center_rgba[1] < 0.49f ||
        report.compute_center_rgba[1] > 0.52f ||
        report.compute_center_rgba[2] < 0.24f ||
        report.compute_center_rgba[2] > 0.26f ||
        report.compute_center_rgba[3] < 0.99f) {
        fprintf(stderr, "compute output did not match the deterministic cue\n");
        return 1;
    }

    return 0;
}
