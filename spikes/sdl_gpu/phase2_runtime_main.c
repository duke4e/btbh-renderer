#include "btbh_gpu_phase2_runtime.h"

#include <stdio.h>
#include <stdlib.h>

#ifndef BTBH_PHASE2_SHADOW_VERTEX_PATH
#error "BTBH_PHASE2_SHADOW_VERTEX_PATH must be defined"
#endif
#ifndef BTBH_PHASE2_SHADOW_FRAGMENT_PATH
#error "BTBH_PHASE2_SHADOW_FRAGMENT_PATH must be defined"
#endif
#ifndef BTBH_PHASE2_GBUFFER_VERTEX_PATH
#error "BTBH_PHASE2_GBUFFER_VERTEX_PATH must be defined"
#endif
#ifndef BTBH_PHASE2_GBUFFER_FRAGMENT_PATH
#error "BTBH_PHASE2_GBUFFER_FRAGMENT_PATH must be defined"
#endif
#ifndef BTBH_PHASE2_PATTERN_COMPUTE_PATH
#error "BTBH_PHASE2_PATTERN_COMPUTE_PATH must be defined"
#endif
#ifndef BTBH_PHASE2_INTERVAL_COMPUTE_PATH
#error "BTBH_PHASE2_INTERVAL_COMPUTE_PATH must be defined"
#endif
#ifndef BTBH_PHASE2_LIGHTING_VERTEX_PATH
#error "BTBH_PHASE2_LIGHTING_VERTEX_PATH must be defined"
#endif
#ifndef BTBH_PHASE2_LIGHTING_FRAGMENT_PATH
#error "BTBH_PHASE2_LIGHTING_FRAGMENT_PATH must be defined"
#endif
#ifndef BTBH_PHASE2_TRANSPARENT_FRAGMENT_PATH
#error "BTBH_PHASE2_TRANSPARENT_FRAGMENT_PATH must be defined"
#endif

int main(int argc, char **argv)
{
    const char *capture_path =
        argc > 1 ? argv[1] : "btbh_gpu_phase2_runtime.bmp";
    const uint32_t width =
        argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 10) : 1920u;
    const uint32_t height =
        argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 10) : 1080u;
    const BTBHGPUPhase2Config config = {
        BTBH_PHASE2_SHADOW_VERTEX_PATH,
        BTBH_PHASE2_SHADOW_FRAGMENT_PATH,
        BTBH_PHASE2_GBUFFER_VERTEX_PATH,
        BTBH_PHASE2_GBUFFER_FRAGMENT_PATH,
        BTBH_PHASE2_PATTERN_COMPUTE_PATH,
        BTBH_PHASE2_INTERVAL_COMPUTE_PATH,
        BTBH_PHASE2_LIGHTING_VERTEX_PATH,
        BTBH_PHASE2_LIGHTING_FRAGMENT_PATH,
        BTBH_PHASE2_TRANSPARENT_FRAGMENT_PATH,
#if defined(__APPLE__)
        "main0",
#else
        "main",
#endif
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
    BTBHGPUPhase2Report report;
    const int exit_code = btbh_gpu_run_phase2(&config, &report);

    (void)printf(
        "{\"status\":\"%s\",\"driver\":\"%s\","
        "\"width\":%u,\"height\":%u,\"passes\":%u,"
        "\"resources\":{\"tracked\":%u,\"released\":%u},"
        "\"cpu_submit_ns\":%llu,\"gpu_wait_ns\":%llu,"
        "\"scene_signature\":\"%016llx\","
        "\"final_image_checksum\":\"%016llx\","
        "\"center_rgba\":[%u,%u,%u,%u],"
        "\"directional_shadow_rgba\":[%u,%u,%u,%u],"
        "\"skinned_rgba\":[%u,%u,%u,%u],"
        "\"interval_center_rgba\":[%.6f,%.6f,%.6f,%.6f],"
        "\"capture\":\"%s\",\"message\":\"%s\"}\n",
        exit_code == 0 ? "ok" : "error",
        report.driver_name,
        report.width,
        report.height,
        report.executed_pass_count,
        report.tracked_resource_count,
        report.deferred_release_count,
        (unsigned long long)report.cpu_submit_nanoseconds,
        (unsigned long long)report.gpu_wait_nanoseconds,
        (unsigned long long)report.scene_signature,
        (unsigned long long)report.final_image_checksum,
        report.center_rgba[0],
        report.center_rgba[1],
        report.center_rgba[2],
        report.center_rgba[3],
        report.directional_shadow_rgba[0],
        report.directional_shadow_rgba[1],
        report.directional_shadow_rgba[2],
        report.directional_shadow_rgba[3],
        report.skinned_rgba[0],
        report.skinned_rgba[1],
        report.skinned_rgba[2],
        report.skinned_rgba[3],
        report.interval_center_rgba[0],
        report.interval_center_rgba[1],
        report.interval_center_rgba[2],
        report.interval_center_rgba[3],
        capture_path,
        report.message
    );
    return exit_code;
}
