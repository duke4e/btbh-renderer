#include "btbh_gpu_phase2_runtime.h"

#include <stdio.h>

#ifndef BTBH_PHASE2_CAPTURE_PATH
#error "BTBH_PHASE2_CAPTURE_PATH must be defined"
#endif
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

int main(void)
{
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
        BTBH_PHASE2_CAPTURE_PATH,
#if defined(__APPLE__)
        BTBH_GPU_BACKEND_METAL,
        BTBH_GPU_SHADER_FORMAT_METALLIB,
#else
        BTBH_GPU_BACKEND_VULKAN,
        BTBH_GPU_SHADER_FORMAT_SPIRV,
#endif
        320u,
        180u
    };
    BTBHGPUPhase2Report report;

    if (btbh_gpu_run_phase2(&config, &report) != 0) {
        fprintf(stderr, "Phase 2 runtime failed: %s\n", report.message);
        return 1;
    }
    if (report.result != BTBH_GPU_OK ||
        report.width != 320u ||
        report.height != 180u ||
        report.scene_signature != UINT64_C(0x7cc30645be466e5f) ||
        report.executed_pass_count != 11u ||
        report.final_image_checksum == 0u ||
        report.center_rgba[0] < 45u ||
        report.center_rgba[0] > 49u ||
        report.center_rgba[1] < 115u ||
        report.center_rgba[1] > 119u ||
        report.center_rgba[2] < 162u ||
        report.center_rgba[2] > 166u ||
        report.center_rgba[3] != 255u ||
        report.directional_shadow_rgba[0] < 54u ||
        report.directional_shadow_rgba[0] > 62u ||
        report.directional_shadow_rgba[1] < 92u ||
        report.directional_shadow_rgba[1] > 100u ||
        report.directional_shadow_rgba[2] < 126u ||
        report.directional_shadow_rgba[2] > 136u ||
        report.directional_shadow_rgba[3] != 255u ||
        report.skinned_rgba[0] < 115u ||
        report.skinned_rgba[0] > 150u ||
        report.skinned_rgba[1] < 50u ||
        report.skinned_rgba[1] > 85u ||
        report.skinned_rgba[2] < 70u ||
        report.skinned_rgba[2] > 105u ||
        report.skinned_rgba[3] != 255u ||
        report.interval_center_rgba[0] < 0.1249f ||
        report.interval_center_rgba[0] > 0.1251f ||
        report.interval_center_rgba[1] < 0.1249f ||
        report.interval_center_rgba[1] > 0.1251f ||
        report.interval_center_rgba[2] < -0.0001f ||
        report.interval_center_rgba[2] > 0.0001f ||
        report.interval_center_rgba[3] < 0.9999f) {
        fprintf(
            stderr,
            "invalid Phase 2 runtime report: %ux%u passes=%u "
            "signature=%016llx checksum=%016llx center=%u,%u,%u,%u "
            "directional_shadow=%u,%u,%u,%u "
            "skinned=%u,%u,%u,%u "
            "interval=%.6f,%.6f,%.6f,%.6f\n",
            report.width,
            report.height,
            report.executed_pass_count,
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
            report.interval_center_rgba[3]
        );
        return 1;
    }
#if !defined(__APPLE__)
    if (report.final_image_checksum != UINT64_C(0xd2ee844acc01f04c)) {
        fprintf(
            stderr,
            "unexpected Windows Phase 2 image checksum: %016llx\n",
            (unsigned long long)report.final_image_checksum
        );
        return 1;
    }
#endif
    return 0;
}
