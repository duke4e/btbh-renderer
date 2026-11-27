/* The smokes' presents of the window's frame. From 0.16.0, BTBH_SMOKE_PRESENT_VIEWS=1 sends them through
 * btbh_gpu_window_scene_present_views with one view (the camera, a zero clip, no visibility or lighting of its own,
 * RFC 0014), which must draw byte for byte as btbh_gpu_window_scene_present_instances: an identity run with it set
 * compares the smokes' images with a build of 0.15.0's. Before 0.16.0 (no BTBH_GPU_SCENE_HAS_VIEWS) the variable does
 * nothing, and so does it for a call with a NULL camera array (which both refuse). */
#ifndef BTBH_GPU_SMOKE_PRESENT_H
#define BTBH_GPU_SMOKE_PRESENT_H

#include "btbh_gpu/btbh_gpu.h"

#include <SDL3/SDL_stdinc.h>

static inline BTBHGPUResult smoke_present_instances(
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
    uint32_t batch_count,
    uint8_t clear_red,
    uint8_t clear_green,
    uint8_t clear_blue,
    uint8_t clear_alpha,
    BTBHGPUSceneReport *report
) {
#if defined(BTBH_GPU_SCENE_HAS_VIEWS)
    const char *views = SDL_getenv("BTBH_SMOKE_PRESENT_VIEWS");
    if (views != NULL && views[0] == '1' && view_projection != NULL && camera_right != NULL && camera_up != NULL &&
        camera_position != NULL) {
        BTBHGPUSceneView view;
        SDL_zero(view);
        SDL_memcpy(view.view_projection, view_projection, sizeof(view.view_projection));
        SDL_memcpy(view.right, camera_right, sizeof(view.right));
        SDL_memcpy(view.up, camera_up, sizeof(view.up));
        SDL_memcpy(view.position, camera_position, sizeof(view.position));
        BTBHGPUSceneViewReport view_report;
        const BTBHGPUResult result = btbh_gpu_window_scene_present_views(window, vertex_shader_path,
            animated_vertex_shader_path, fast_shadow_vertex_shader_path, fast_shadow_fragment_shader_path,
            shadow_vertex_shader_path, animated_shadow_vertex_shader_path, shadow_fragment_shader_path,
            fragment_shader_path, unshadowed_fragment_shader_path, post_vertex_shader_path, post_fragment_shader_path,
            shader_format, render_width, render_height, &view, 1u, lighting, post, instances, transient_instance_count,
            logical_instance_count, batches, NULL, NULL, batch_count, clear_red, clear_green, clear_blue, clear_alpha,
            &view_report, report);
        /* One view draws every draw of the frame. */
        if (result == BTBH_GPU_OK && view_report.primitive_draw_count != report->primitive_draw_count) {
            report->result = BTBH_GPU_DEVICE_FAILURE;
            SDL_strlcpy(report->message, "BTBH_SMOKE_PRESENT_VIEWS: the view's draws aren't the frame's",
                sizeof(report->message));
            return report->result;
        }
        return result;
    }
#endif
    return btbh_gpu_window_scene_present_instances(window, vertex_shader_path, animated_vertex_shader_path,
        fast_shadow_vertex_shader_path, fast_shadow_fragment_shader_path, shadow_vertex_shader_path,
        animated_shadow_vertex_shader_path, shadow_fragment_shader_path, fragment_shader_path,
        unshadowed_fragment_shader_path, post_vertex_shader_path, post_fragment_shader_path, shader_format,
        render_width, render_height, view_projection, camera_right, camera_up, camera_position, lighting, post,
        instances, transient_instance_count, logical_instance_count, batches, batch_count, clear_red, clear_green,
        clear_blue, clear_alpha, report);
}

#endif
