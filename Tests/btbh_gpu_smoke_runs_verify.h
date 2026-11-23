/* BTBH_GPU_SCENE_RUNS=verify (since 0.13.0) in the scene smokes. A smoke started with the variable set to "verify"
 * reports at its end what its window checked (btbh_gpu_window_scene_read_run_verify) and fails when a draw that
 * continued a run found a difference, or when no draw was checked. CTest runs the runs and flags smokes so
 * (<smoke>_runs_verify, which also fail on the core's mismatch log). Without the variable, or against a core older
 * than 0.13.0 (no BTBH_GPU_SCENE_HAS_RUN_VERIFY), it does nothing. */
#ifndef BTBH_GPU_SMOKE_RUNS_VERIFY_H
#define BTBH_GPU_SMOKE_RUNS_VERIFY_H

#include "btbh_gpu/btbh_gpu.h"
#include "btbh_gpu_package_internal.h"

#include <SDL3/SDL_stdinc.h>

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* Whether the smoke was started in the mode (read it before the smoke changes the variable). */
static inline bool smoke_runs_verify_requested(void) {
    const char *value = SDL_getenv("BTBH_GPU_SCENE_RUNS");
    return value != NULL && strcmp(value, "verify") == 0;
}

static inline void smoke_check_runs_verify(const BTBHGPUWindow *window, const char *smoke, bool requested) {
#if defined(BTBH_GPU_SCENE_HAS_RUN_VERIFY)
    if (!requested) {
        return;
    }
    BTBHGPUSceneRunVerifyReport report;
    memset(&report, 0, sizeof(report));
    assert(btbh_gpu_window_scene_read_run_verify(window, &report) != 0u);
    printf("%s: BTBH_GPU_SCENE_RUNS=verify checked %llu run draws: %llu mismatches\n", smoke,
        (unsigned long long)report.checked_draw_count, (unsigned long long)report.mismatch_count);
    assert(report.mismatch_count == 0u);
    assert(report.checked_draw_count > 0u);
#else
    (void)window;
    (void)smoke;
    (void)requested;
#endif
}

#endif
