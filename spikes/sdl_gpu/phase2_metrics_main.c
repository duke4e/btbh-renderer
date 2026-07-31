#include "btbh_gpu_phase2_runtime.h"

#include <stdio.h>
#include <stdlib.h>

#define BTBH_PHASE2_MAX_METRIC_FRAMES 256u

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

static int btbh_compare_u64(const void *left, const void *right)
{
    const uint64_t lhs = *(const uint64_t *)left;
    const uint64_t rhs = *(const uint64_t *)right;
    return lhs < rhs ? -1 : lhs > rhs ? 1 : 0;
}

static uint64_t btbh_percentile(
    uint64_t *samples,
    uint32_t count,
    uint32_t numerator,
    uint32_t denominator
) {
    qsort(samples, count, sizeof(samples[0]), btbh_compare_u64);
    uint32_t rank = (count * numerator + denominator - 1u) / denominator;
    if (rank == 0u) {
        rank = 1u;
    }
    return samples[rank - 1u];
}

static uint64_t btbh_median(uint64_t *samples, uint32_t count)
{
    qsort(samples, count, sizeof(samples[0]), btbh_compare_u64);
    const uint32_t midpoint = count / 2u;
    if ((count & 1u) != 0u) {
        return samples[midpoint];
    }
    const uint64_t lower = samples[midpoint - 1u];
    const uint64_t upper = samples[midpoint];
    return lower + (upper - lower) / 2u;
}

int main(int argc, char **argv)
{
    const char *capture_path =
        argc > 1 ? argv[1] : "btbh_gpu_phase2_metrics.bmp";
    const uint32_t width =
        argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 10) : 1920u;
    const uint32_t height =
        argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 10) : 1080u;
    const uint32_t warmup_count =
        argc > 4 ? (uint32_t)strtoul(argv[4], NULL, 10) : 5u;
    const uint32_t measured_count =
        argc > 5 ? (uint32_t)strtoul(argv[5], NULL, 10) : 30u;
    if (warmup_count > BTBH_PHASE2_MAX_METRIC_FRAMES ||
        measured_count == 0u ||
        measured_count > BTBH_PHASE2_MAX_METRIC_FRAMES) {
        (void)fprintf(stderr, "Invalid Phase 2 metric frame counts\n");
        return 2;
    }

    BTBHGPUPhase2Config config = {
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
        NULL,
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
    uint64_t submit_samples[BTBH_PHASE2_MAX_METRIC_FRAMES];
    uint64_t wait_samples[BTBH_PHASE2_MAX_METRIC_FRAMES];
    uint64_t expected_signature = 0u;
    uint64_t expected_checksum = 0u;
    BTBHGPUPhase2Report report;

    for (uint32_t frame = 0u;
         frame < warmup_count + measured_count;
         ++frame) {
        if (btbh_gpu_run_phase2(&config, &report) != 0 ||
            report.result != BTBH_GPU_OK ||
            report.tracked_resource_count != 30u ||
            report.deferred_release_count != 30u) {
            (void)fprintf(
                stderr,
                "Phase 2 metric frame %u failed: %s\n",
                frame,
                report.message
            );
            return 1;
        }
        if (frame == 0u) {
            expected_signature = report.scene_signature;
            expected_checksum = report.final_image_checksum;
        } else if (report.scene_signature != expected_signature ||
                   report.final_image_checksum != expected_checksum) {
            (void)fprintf(
                stderr,
                "Phase 2 metric frame %u changed signature/checksum\n",
                frame
            );
            return 1;
        }
        if (frame >= warmup_count) {
            const uint32_t sample = frame - warmup_count;
            submit_samples[sample] = report.cpu_submit_nanoseconds;
            wait_samples[sample] = report.gpu_wait_nanoseconds;
        }
    }

    const uint64_t submit_median =
        btbh_median(submit_samples, measured_count);
    const uint64_t submit_p95 = btbh_percentile(
        submit_samples,
        measured_count,
        95u,
        100u
    );
    const uint64_t submit_worst = submit_samples[measured_count - 1u];
    const uint64_t wait_median =
        btbh_median(wait_samples, measured_count);
    const uint64_t wait_p95 = btbh_percentile(
        wait_samples,
        measured_count,
        95u,
        100u
    );
    const uint64_t wait_worst = wait_samples[measured_count - 1u];

    config.capture_path = capture_path;
    if (btbh_gpu_run_phase2(&config, &report) != 0 ||
        report.scene_signature != expected_signature ||
        report.final_image_checksum != expected_checksum) {
        (void)fprintf(stderr, "Final Phase 2 metric capture failed\n");
        return 1;
    }

    (void)printf(
        "{\"status\":\"ok\",\"driver\":\"%s\",\"width\":%u,"
        "\"height\":%u,\"warmup_frames\":%u,\"measured_frames\":%u,"
        "\"scene_signature\":\"%016llx\","
        "\"final_image_checksum\":\"%016llx\","
        "\"resources_per_frame\":30,"
        "\"cpu_submit_ns\":{\"median\":%llu,\"p95\":%llu,\"worst\":%llu},"
        "\"gpu_wait_ns\":{\"median\":%llu,\"p95\":%llu,\"worst\":%llu}}\n",
        report.driver_name,
        report.width,
        report.height,
        warmup_count,
        measured_count,
        (unsigned long long)expected_signature,
        (unsigned long long)expected_checksum,
        (unsigned long long)submit_median,
        (unsigned long long)submit_p95,
        (unsigned long long)submit_worst,
        (unsigned long long)wait_median,
        (unsigned long long)wait_p95,
        (unsigned long long)wait_worst
    );
    return 0;
}
