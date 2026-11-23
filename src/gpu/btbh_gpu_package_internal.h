#ifndef BTBH_GPU_PACKAGE_INTERNAL_H
#define BTBH_GPU_PACKAGE_INTERNAL_H

#include "../../include/btbh_gpu/btbh_gpu.h"

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct BTBHGPUSceneTelemetryReport {
    uint32_t animated_geometry_count;
    uint32_t animation_sample_count;
    uint32_t bone_matrix_count;
    uint32_t shadow_pipeline_bind_count;
    uint32_t shadow_pipeline_bind_skip_count;
    uint32_t geometry_pipeline_bind_count;
    uint32_t geometry_pipeline_bind_skip_count;
    double cpu_prepare_setup_milliseconds;
    double cpu_prepare_bones_milliseconds;
    double cpu_prepare_bone_count_milliseconds;
    double cpu_prepare_bone_allocate_milliseconds;
    double cpu_prepare_bone_sample_milliseconds;
    double cpu_prepare_pipelines_milliseconds;
    double cpu_prepare_buffers_milliseconds;
    /* RFC 0011 (since 0.10.0): the geometries that continued a run (btbh_gpu_scene_batch_continues_run), binding and
       pushing nothing but their vertex and index buffers, in the opaque, transparent and refraction passes, in the
       shadow passes (each map and face counts) and among the radiance cascades' material sources. */
    uint32_t geometry_run_draw_count;
    uint32_t shadow_run_draw_count;
    uint32_t rc_source_run_draw_count;
    /* BTBH_GPU_SCENE_RUNS=verify (since 0.13.0; 0 otherwise): the draws above that rebuilt their batch's state and
       compared it with what the pass held, and the differences found (each field of each draw counts once). */
    uint32_t run_verify_draw_count;
    uint32_t run_verify_mismatch_count;
} BTBHGPUSceneTelemetryReport;

/* BTBH_GPU_SCENE_RUNS=verify (since 0.13.0): the run_verify_* counts over a window's life, its presents and the scenes
   it rendered into a target (whose telemetry the window doesn't keep) alike (btbh_gpu_window_scene_read_run_verify). */
typedef struct BTBHGPUSceneRunVerifyReport {
    uint64_t checked_draw_count;
    uint64_t mismatch_count;
} BTBHGPUSceneRunVerifyReport;
/* Defined when BTBHGPUSceneRunVerifyReport and btbh_gpu_window_scene_read_run_verify exist (since 0.13.0). */
#define BTBH_GPU_SCENE_HAS_RUN_VERIFY 1

void btbh_gpu_delay_precise(uint64_t nanoseconds);
uint8_t btbh_gpu_window_scene_shadow_resources_ready(
    const BTBHGPUWindow *window
);
uint8_t btbh_gpu_window_scene_set_shadow_update_periods(
    BTBHGPUWindow *window,
    uint32_t directional_frames,
    uint32_t secondary_directional_frames,
    uint32_t spot_frames
);
uint8_t btbh_gpu_window_scene_read_telemetry(
    const BTBHGPUWindow *window,
    BTBHGPUSceneTelemetryReport *report
);
uint8_t btbh_gpu_window_scene_read_run_verify(
    const BTBHGPUWindow *window,
    BTBHGPUSceneRunVerifyReport *report
);
BTBHGPUResult btbh_gpu_window_scene_present_instances_with_visibility(
    BTBHGPUWindow *window,
    const char *vertex_shader_path,
    const char *animated_vertex_shader_path,
    const char *fast_shadow_vertex_shader_path,
    const char *fast_shadow_fragment_shader_path,
    const char *shadow_vertex_shader_path,
    const char *animated_shadow_vertex_shader_path,
    const char *shadow_fragment_shader_path,
    const char *fragment_shader_path,
    const char *unshadowed_fragment_shader_path,
    const char *post_vertex_shader_path,
    const char *post_fragment_shader_path,
    BTBHGPUShaderFormat shader_format,
    uint32_t render_width,
    uint32_t render_height,
    const float view_projection[16],
    const float camera_right[3],
    const float camera_up[3],
    const float camera_position[3],
    const BTBHGPUSceneLighting *lighting,
    const BTBHGPUScenePost *post,
    const float *instances,
    uint32_t transient_instance_count,
    uint32_t logical_instance_count,
    const BTBHGPUSceneGeometryBatch *batches,
    const uint8_t *scene_visibility,
    const uint8_t *directional_shadow_visibility,
    uint32_t batch_count,
    uint8_t clear_red,
    uint8_t clear_green,
    uint8_t clear_blue,
    uint8_t clear_alpha,
    BTBHGPUSceneReport *report
);

#if defined(__cplusplus)
}
#endif

#endif
