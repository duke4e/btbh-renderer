/* The smokes' presents of the window's frame. From 0.16.0, BTBH_SMOKE_PRESENT_VIEWS=1 sends them through
 * btbh_gpu_window_scene_present_views with one view (the camera, a zero clip, no visibility or lighting of its own,
 * RFC 0014), which must draw byte for byte as btbh_gpu_window_scene_present_instances: an identity run with it set
 * compares the smokes' images with a build of 0.15.0's. Before 0.16.0 (no BTBH_GPU_SCENE_HAS_VIEWS) the variable does
 * nothing, and so does it for a call with a NULL camera array (which both refuse).
 * From 0.18.0, BTBH_SMOKE_DISTORTION=1 sets BTBH_GPU_SCENE_POST_DISTORTION in the post and, before each present, two
 * distortion sources that draw nothing (one of strength 0, one far off to the camera's side: skipped and culled, RFC
 * 0018): the frames must stay byte for byte as without them. Before 0.18.0 (no BTBH_GPU_SCENE_HAS_DISTORTION) it does
 * nothing.
 * From 0.18.0, BTBH_SMOKE_COVER=1 sets a cover whose two layers lay nothing (amount 0, every other field set) and
 * BTBH_GPU_SCENE_BATCH_FLAGS2_COVER_0 on every batch that may take it (RFC 0020; not a lit particle, a refracting batch
 * or one the fast unlit shader draws; up to SMOKE_COVER_BATCHES batches, none past that): the frames must stay byte for
 * byte as without them. Before 0.18.0 (no BTBH_GPU_SCENE_HAS_COVER) it does nothing. */
#ifndef BTBH_GPU_SMOKE_PRESENT_H
#define BTBH_GPU_SMOKE_PRESENT_H

#include "btbh_gpu/btbh_gpu.h"

#include <SDL3/SDL_stdinc.h>

#if defined(BTBH_GPU_SCENE_HAS_COVER)
#define SMOKE_COVER_BATCHES 2048u

/* BTBH_SMOKE_COVER=1 (see above): the batches with the cover's bit where they may take it, in a copy. */
static inline const BTBHGPUSceneGeometryBatch *smoke_cover_batches(
    BTBHGPUWindow *window,
    const BTBHGPUSceneGeometryBatch *batches,
    uint32_t batch_count
) {
    static BTBHGPUSceneGeometryBatch copies[SMOKE_COVER_BATCHES];
    const char *cover = SDL_getenv("BTBH_SMOKE_COVER");
    if (cover == NULL || cover[0] != '1') {
        return batches;
    }
    BTBHGPUSceneCover inert;
    SDL_zero(inert);
    for (uint32_t layer = 0u; layer < BTBH_GPU_SCENE_COVER_LAYER_COUNT; ++layer) {
        inert.layers[layer].color[0] = 1.0f;
        inert.layers[layer].color[1] = 0.5f;
        inert.layers[layer].slope_degrees = 80.0f;
        inert.layers[layer].edge_degrees = 20.0f;
        inert.layers[layer].edge_noise_degrees = 30.0f;
        inert.layers[layer].noise_scale = 2.0f;
        inert.layers[layer].roughness = 0.3f;
        inert.layers[layer].bump = 1.0f;
        inert.layers[layer].normal_influence = 1.0f;
        inert.layers[layer].pattern = (uint8_t)(layer == 0u ? BTBH_GPU_SCENE_COVER_SNOW : BTBH_GPU_SCENE_COVER_MUD);
    }
    (void)btbh_gpu_window_scene_set_cover(window, &inert, NULL);
    if (batches == NULL || batch_count > SMOKE_COVER_BATCHES) {
        return batches;
    }
    for (uint32_t index = 0u; index < batch_count; ++index) {
        BTBHGPUSceneGeometryBatch *batch = &copies[index];
        *batch = batches[index];
        const bool see_through = batch->transparency_enabled != 0u || batch->blend_mode != BTBH_GPU_SCENE_BLEND_ALPHA;
        const BTBHGPUSceneWorldDetail *detail = batch->world_detail;
        const bool detailed = detail != NULL &&
            (detail->albedo_texture != NULL || detail->normal_texture != NULL || detail->orm_texture != NULL);
        const bool tabled = batch->world_detail_table != NULL && batch->world_detail_table->map_layers != NULL;
        const bool fast_unlit = batch->unlit_enabled != 0u && batch->surface_variant == BTBH_GPU_SCENE_SURFACE_NONE &&
            !detailed && !tabled && see_through;
        if (!fast_unlit && batch->surface_variant != BTBH_GPU_SCENE_SURFACE_LIT_PARTICLE &&
            batch->surface_variant != BTBH_GPU_SCENE_SURFACE_REFRACT) {
            batch->flags2 = (uint8_t)(batch->flags2 | BTBH_GPU_SCENE_BATCH_FLAGS2_COVER_0);
        }
    }
    return copies;
}
#endif

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
#if defined(BTBH_GPU_SCENE_HAS_DISTORTION)
    BTBHGPUScenePost distortion_post;
    const char *distortion = SDL_getenv("BTBH_SMOKE_DISTORTION");
    if (distortion != NULL && distortion[0] == '1' && post != NULL && camera_position != NULL &&
        camera_right != NULL) {
        distortion_post = *post;
        distortion_post.flags |= (uint8_t)BTBH_GPU_SCENE_POST_DISTORTION;
        post = &distortion_post;
        BTBHGPUSceneDistortionSource sources[2];
        SDL_zeroa(sources);
        for (int index = 0; index < 2; ++index) {
            sources[index].shape = BTBH_GPU_SCENE_DISTORTION_DISC;
            sources[index].radius = 1.0f;
            sources[index].strength = index == 0 ? 0.0f : 12.0f;
            for (int axis = 0; axis < 3; ++axis) {
                sources[index].position[axis] = camera_position[axis] + (index == 0 ? 0.0f : 1.0e6f * camera_right[axis]);
            }
        }
        (void)btbh_gpu_window_scene_set_distortion(window, sources, 2u, 0.0f, NULL);
    }
#endif
#if defined(BTBH_GPU_SCENE_HAS_COVER)
    batches = smoke_cover_batches(window, batches, batch_count);
#endif
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
