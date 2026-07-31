#include "btbh_gpu_vertical_smoke.h"

#include <stdio.h>
#include <stdlib.h>

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

int main(int argc, char **argv)
{
    const char *capture_path =
        argc > 1 ? argv[1] : "btbh_gpu_vertical_smoke.bmp";
    const uint32_t width =
        argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 10) : 256u;
    const uint32_t height =
        argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 10) : 256u;
    const BTBHGPUVerticalSmokeConfig config = {
        BTBH_VERTEX_SHADER_PATH,
        BTBH_FRAGMENT_SHADER_PATH,
        BTBH_COMPUTE_SHADER_PATH,
        BTBH_SHADER_ENTRYPOINT,
        capture_path,
#if defined(__APPLE__)
        BTBH_GPU_BACKEND_METAL,
        BTBH_GPU_SHADER_FORMAT_METALLIB,
#else
        BTBH_GPU_BACKEND_VULKAN,
        BTBH_GPU_SHADER_FORMAT_SPIRV,
#endif
        width,
        height
    };
    BTBHGPUVerticalSmokeReport report;
    const int exit_code = btbh_gpu_run_vertical_smoke(&config, &report);

    printf(
        "{\"status\":\"%s\",\"driver\":\"%s\","
        "\"width\":%u,\"height\":%u,"
        "\"center_rgba\":[%u,%u,%u,%u],"
        "\"compute_center_rgba\":[%.6f,%.6f,%.6f,%.6f],"
        "\"capture\":\"%s\",\"message\":\"%s\"}\n",
        exit_code == 0 ? "ok" : "error",
        report.driver_name,
        report.width,
        report.height,
        report.center_rgba[0],
        report.center_rgba[1],
        report.center_rgba[2],
        report.center_rgba[3],
        report.compute_center_rgba[0],
        report.compute_center_rgba[1],
        report.compute_center_rgba[2],
        report.compute_center_rgba[3],
        capture_path,
        report.message
    );
    return exit_code;
}
