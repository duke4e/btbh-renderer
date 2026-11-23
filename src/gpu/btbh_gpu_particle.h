#ifndef BTBH_GPU_PARTICLE_H
#define BTBH_GPU_PARTICLE_H

/* RFC 0013 phase 1 (0.11.0): what a batch with BTBH_GPU_SCENE_SURFACE_LIT_PARTICLE or
 * BTBH_GPU_SCENE_BATCH_INSTANCE_UV_RECT may be, the lit particles' frame block, and C copies of
 * production_particle_lit.frag.hlsl's wrap, phase and fog for the native tests and the smoke's references (as
 * btbh_gpu_world_detail.h is for RFC 0008). Also (0.11.0) what a batch with BTBH_GPU_SCENE_BATCH_INSTANCE_LIGHT_FACTOR
 * may be, which reads a custom float as the rect does. RFC 0013 phase 2 (0.14.0): what a batch with
 * BTBH_GPU_SCENE_BATCH_PARTICLE_SHADOW may be, the particle_shadow byte's values and the map's size, and C copies of
 * the map's optical depth and of a slab's transmittance. Header-only: btbh_gpu_scene.c and the tests include it. */

#include "btbh_gpu/btbh_gpu.h"
#include "btbh_gpu_world_detail.h"
#include "btbh_gpu_height_lift.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static inline bool btbh_gpu_particle_lit(const BTBHGPUSceneGeometryBatch *batch) {
    return batch->surface_variant == BTBH_GPU_SCENE_SURFACE_LIT_PARTICLE;
}

static inline bool btbh_gpu_particle_uv_rect(const BTBHGPUSceneGeometryBatch *batch) {
    return ((uint32_t)batch->flags & BTBH_GPU_SCENE_BATCH_INSTANCE_UV_RECT) != 0u;
}

/* Why a lit particle batch can't draw, or NULL. */
static inline const char *btbh_gpu_particle_lit_problem(const BTBHGPUSceneGeometryBatch *batch) {
    if (batch->transparency_enabled == 0u) {
        return "a lit particle is see-through: transparency_enabled must be 1";
    }
    if (batch->blend_mode != BTBH_GPU_SCENE_BLEND_ALPHA) {
        return "a lit particle blends with straight alpha: blend_mode must be BTBH_GPU_SCENE_BLEND_ALPHA";
    }
    if (batch->unlit_enabled != 0u) {
        return "a lit particle is lit: unlit_enabled must be 0";
    }
    if (batch->animation_library != NULL || batch->animation_pose != NULL) {
        return "a lit particle must be static: animation_library and animation_pose must be NULL";
    }
    if (batch->dug_by != NULL) {
        return "a lit particle can't be dug (dug_by must be NULL)";
    }
    if (btbh_gpu_world_detail_maps(batch->world_detail) != 0u) {
        return "a lit particle takes no world_detail with a texture";
    }
    if (batch->world_detail_table != NULL && batch->world_detail_table->map_layers != NULL) {
        return "a lit particle takes no world_detail_table with map_layers";
    }
    if (batch->height_lift != NULL && batch->height_lift->heightfield != NULL) {
        return "a lit particle can't be lifted: height_lift's heightfield must be NULL";
    }
    if (batch->detail_parameters[0] != 0.0f || batch->detail_parameters[1] != 0.0f ||
        batch->detail_parameters[2] != 0.0f || batch->detail_parameters[3] != 0.0f) {
        return "a lit particle takes no procedural detail: detail_parameters must be 0";
    }
    /* (Reserved, and refused unless 0, in 0.11.0 to 0.13.0; since 0.14.0 the density the particle shadow takes.) */
    if (batch->surface_parameters1[3] < 0.0f) {
        return "surface_parameters1[3], the particle shadow's density, must be 0 or more";
    }
    return NULL;
}

/* RFC 0013 phase 2 (0.14.0): BTBH_GPU_SCENE_BATCH_PARTICLE_SHADOW. */
static inline bool btbh_gpu_particle_shadow(const BTBHGPUSceneGeometryBatch *batch) {
    return ((uint32_t)batch->flags & BTBH_GPU_SCENE_BATCH_PARTICLE_SHADOW) != 0u;
}

/* Why a batch with BTBH_GPU_SCENE_BATCH_PARTICLE_SHADOW can't draw into the particle transmittance map, or NULL. */
static inline const char *btbh_gpu_particle_shadow_problem(const BTBHGPUSceneGeometryBatch *batch) {
    if (batch->transparency_enabled == 0u && batch->blend_mode == BTBH_GPU_SCENE_BLEND_ALPHA) {
        return "the particle shadow is for see-through batches: transparency_enabled must be 1 (or the blend other "
            "than BTBH_GPU_SCENE_BLEND_ALPHA)";
    }
    if (batch->surface_variant != BTBH_GPU_SCENE_SURFACE_NONE &&
        batch->surface_variant != BTBH_GPU_SCENE_SURFACE_SOFT_PARTICLE &&
        batch->surface_variant != BTBH_GPU_SCENE_SURFACE_LIT_PARTICLE) {
        return "the particle shadow takes surface variant NONE, SOFT_PARTICLE or LIT_PARTICLE";
    }
    if (batch->animation_library != NULL || batch->animation_pose != NULL) {
        return "the particle shadow needs a static batch: animation_library and animation_pose must be NULL";
    }
    if (batch->height_lift != NULL && batch->height_lift->heightfield != NULL) {
        return "the particle shadow can't be lifted: height_lift's heightfield must be NULL";
    }
    if (batch->dug_by != NULL) {
        return "the particle shadow can't be dug (dug_by must be NULL)";
    }
    return NULL;
}

/* Whether BTBHGPUSceneLighting.particle_shadow is one the renderer knows: readers 0, 1 or 2 (bits 0-1), the quarter
   size (bit 2), nothing above. */
static inline bool btbh_gpu_particle_shadow_setting_valid(uint8_t particle_shadow) {
    const uint32_t known = BTBH_GPU_SCENE_PARTICLE_SHADOW_READERS | BTBH_GPU_SCENE_PARTICLE_SHADOW_QUARTER;
    return (particle_shadow & BTBH_GPU_SCENE_PARTICLE_SHADOW_READERS) <= BTBH_GPU_SCENE_PARTICLE_SHADOW_GROUND &&
        (particle_shadow & ~known) == 0u;
}

/* The particle transmittance map's size for a sun map's: half of it each way, or a quarter with
   BTBH_GPU_SCENE_PARTICLE_SHADOW_QUARTER; at least 1. */
static inline uint32_t btbh_gpu_particle_shadow_size(uint32_t sun_map_size, uint8_t particle_shadow) {
    const uint32_t size =
        sun_map_size / ((particle_shadow & BTBH_GPU_SCENE_PARTICLE_SHADOW_QUARTER) != 0u ? 4u : 2u);
    return size > 0u ? size : 1u;
}

/* production_particle_transmittance.frag.hlsl: the optical depth a fragment of alpha `alpha` adds, -ln(1 - min(alpha,
   0.999)) times the density (a lit particle's surface_parameters1[3], 0 taken as 1); 0 where the shader discards it
   (alpha 1/255 or less, or below the batch's alpha_cutoff). */
static inline float btbh_gpu_particle_optical_depth(float alpha, float density, float alpha_cutoff) {
    if (alpha <= 1.0f / 255.0f || alpha < alpha_cutoff) {
        return 0.0f;
    }
    const float scale = density > 0.0f ? density : 1.0f;
    return -logf(1.0f - (alpha < 0.999f ? alpha : 0.999f)) * scale;
}

/* production_particle_shadow.hlsli's btbh_particle_slab_transmittance: the share of the sun a texel's slab (`optical`
   the summed optical depth, `weighted` its depth-weighted sum, `nearest` the nearest depth) leaves at `depth`. */
static inline float btbh_gpu_particle_slab_transmittance(float optical, float weighted, float nearest, float depth) {
    const float r = optical > 0.0f ? optical : 0.0f;
    const float mean = weighted / (r > 1.0e-6f ? r : 1.0e-6f);
    const float front = nearest < mean ? nearest : mean;
    const float span = 2.0f * (mean - front);
    float share = 0.0f;
    if (span > 1.0e-6f) {
        share = (depth - front) / span;
        share = share < 0.0f ? 0.0f : (share > 1.0f ? 1.0f : share);
    } else {
        share = depth >= front ? 1.0f : 0.0f;
    }
    return expf(-r * share);
}

/* Why a batch with BTBH_GPU_SCENE_BATCH_INSTANCE_UV_RECT can't draw, or NULL. */
static inline const char *btbh_gpu_particle_uv_rect_problem(const BTBHGPUSceneGeometryBatch *batch) {
    if (batch->animation_library != NULL || batch->animation_pose != NULL) {
        return "the UV rect needs a static batch: animation_library and animation_pose must be NULL";
    }
    if (batch->surface_variant == BTBH_GPU_SCENE_SURFACE_DISSOLVE ||
        batch->surface_variant == BTBH_GPU_SCENE_SURFACE_SHATTER ||
        batch->surface_variant == BTBH_GPU_SCENE_SURFACE_GLITTER) {
        return "the surface variant reads the instance's custom floats (DISSOLVE, SHATTER, GLITTER), which the UV rect "
            "takes";
    }
    if (batch->world_detail != NULL && batch->world_detail->seed_component != 0u) {
        return "world_detail's seed_component reads a custom float, which the UV rect takes (it must be 0)";
    }
    if (batch->world_detail_table != NULL && batch->world_detail_table->seed_component != 0u) {
        return "world_detail_table's seed_component reads a custom float, which the UV rect takes (it must be 0)";
    }
    return NULL;
}

static inline bool btbh_gpu_scene_light_factor(const BTBHGPUSceneGeometryBatch *batch) {
    return ((uint32_t)batch->flags & BTBH_GPU_SCENE_BATCH_INSTANCE_LIGHT_FACTOR) != 0u;
}

/* Why a batch with BTBH_GPU_SCENE_BATCH_INSTANCE_LIGHT_FACTOR can't draw, or NULL. Skinned batches may: their vertex
   shader passes the custom floats on as the static one does. */
static inline const char *btbh_gpu_scene_light_factor_problem(const BTBHGPUSceneGeometryBatch *batch) {
    if (btbh_gpu_particle_uv_rect(batch)) {
        return "the light factor is custom x, which the UV rect (INSTANCE_UV_RECT) takes: set one or the other";
    }
    if (btbh_gpu_particle_lit(batch)) {
        return "a lit particle takes no light factor (its shader doesn't read it)";
    }
    if (batch->surface_variant == BTBH_GPU_SCENE_SURFACE_REFRACT) {
        return "a refracting batch draws no material, so it has no light to scale";
    }
    if (batch->surface_variant == BTBH_GPU_SCENE_SURFACE_DISSOLVE ||
        batch->surface_variant == BTBH_GPU_SCENE_SURFACE_GLITTER) {
        return "the surface variant reads custom x (DISSOLVE, GLITTER), which the light factor takes";
    }
    if (batch->world_detail != NULL && batch->world_detail->seed_component == 1u) {
        return "world_detail's seed_component 1 reads custom x, which the light factor takes (y, z, w are free)";
    }
    if (batch->world_detail_table != NULL && batch->world_detail_table->seed_component == 1u) {
        return "world_detail_table's seed_component 1 reads custom x, which the light factor takes (y, z, w are free)";
    }
    return NULL;
}

/* production_particle_lit.frag.hlsl's ParticleFrame (b1): what every lit particle of a pass shares, pushed once a
   pass. 192 bytes (176 in 0.11.0 to 0.13.0; RFC 0013 phase 2 added shadow_controls). */
typedef struct BTBHGPUSceneParticleFrameUniforms {
    /* The frame's inverse view-projection, as the HDR composite's and the distance pass's. */
    float inverse_view_projection[16];
    /* x, y 1 / the render size; z 1 with the scene's distance bound; w 1 when the HDR composite fogs. */
    float controls[4];
    /* The composite's fog (BTBHGPUSceneCompositeUniforms'): colour and density, start and greatest opacity, the edge
       fog's rectangle, and its fade, switch and clear depth. */
    float fog_color_density[4];
    float fog_shape[4];
    float fog_edge_bounds[4];
    float fog_edge[4];
    /* The radiance cascades this frame: their field's origin x and z, 1 / its cell, 2 * indirect_intensity (0 off);
       its width and height in cells, 1 with the sources' tops, 0. */
    float cascade_origin[4];
    float cascade_field[4];
    /* RFC 0013 phase 2 (0.14.0): x 1 when the particle transmittance map is drawn this frame (the shader reads it),
       yzw 0. */
    float shadow_controls[4];
} BTBHGPUSceneParticleFrameUniforms;

/* ---------------------------------------------------------------------------------------- the shader's light */

/* The wrap: saturate((n.l + w) / (1 + w)), w below 0 taken as 0. */
static inline float btbh_gpu_particle_wrap(float normal_dot_light, float wrap) {
    const float w = wrap > 0.0f ? wrap : 0.0f;
    const float value = (normal_dot_light + w) / (1.0f + w);
    return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

/* Henyey-Greenstein's phase times 4 pi at cos_theta = dot(-light, view), g clamped to -0.99..0.99 (1 everywhere at
   g = 0; its mean over the sphere is 1 for every g). */
static inline float btbh_gpu_particle_phase(float g, float cos_theta) {
    const float clamped = g < -0.99f ? -0.99f : (g > 0.99f ? 0.99f : g);
    const float g2 = clamped * clamped;
    float d = 1.0f + g2 - 2.0f * clamped * cos_theta;
    d = d > 0.0001f ? d : 0.0001f;
    return (1.0f - g2) / (d * sqrtf(d));
}

/* The transmission's factor: x * exp(-thickness * z), x and z below 0 taken as 0. */
static inline float btbh_gpu_particle_transmission(float transmission, float thickness, float optical_depth) {
    const float x = transmission > 0.0f ? transmission : 0.0f;
    const float z = optical_depth > 0.0f ? optical_depth : 0.0f;
    return x * expf(-thickness * z);
}

/* The HDR composite's fog at a view depth and a world x, z (production_hdr_composite.frag.hlsl's fog_amount), with
   the post's settings. */
static inline float btbh_gpu_particle_fog_amount(const BTBHGPUScenePost *post, float view_depth, float x, float z) {
    float start_gap = view_depth - post->fog_start_distance;
    start_gap = start_gap > 0.0f ? start_gap : 0.0f;
    const float optical = start_gap * post->fog_density;
    float amount = 1.0f - expf(-optical * optical);
    amount = amount < post->fog_maximum_opacity ? amount : post->fog_maximum_opacity;
    if (post->fog_edge_enabled != 0u) {
        const float inside_x = fminf(x - post->fog_edge_bounds[0], post->fog_edge_bounds[2] - x);
        const float inside_z = fminf(z - post->fog_edge_bounds[1], post->fog_edge_bounds[3] - z);
        float edge = 1.0f - fminf(fmaxf(fminf(inside_x, inside_z) / post->fog_edge_fade_distance, 0.0f), 1.0f);
        float gate = fminf(fmaxf((view_depth - post->fog_edge_clear_distance) / post->fog_edge_fade_distance, 0.0f),
            1.0f);
        edge = edge * edge * (3.0f - 2.0f * edge);
        gate = gate * gate * (3.0f - 2.0f * gate);
        amount = fmaxf(amount, edge * gate);
    }
    return amount;
}

/* The colour a lit particle writes so that the composite's lerp(pixel, fog, behind) leaves it fogged at its own
   depth (own): (c (1 - own) + fog (own - behind)) / (1 - behind). */
static inline float btbh_gpu_particle_fog_makeup(float color, float fog_color, float own, float behind) {
    const float keep = 1.0f - behind;
    return (color * (1.0f - own) + fog_color * (own - behind)) / (keep > 0.0001f ? keep : 0.0001f);
}

#endif
