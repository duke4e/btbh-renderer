#ifndef BTBH_GPU_COVER_H
#define BTBH_GPU_COVER_H

/* The cover laid by facing and still ice on the CPU (RFC 0020, since 0.18.0): the reference the shaders
   (production_cover.hlsli, production_model.frag's apply_cover and its ICE case of apply_surface) follow, used by the
   renderer (the setter's checks, the block it pushes, the batch's choice and ICE's parameters) and by the tests (the
   pictures and the targets against the GPU's). The arithmetic is the shaders', in the same order. Header only, no
   SDL. */

#include "btbh_gpu/btbh_gpu.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BTBH_COVER_PI 3.14159265358979323846f
/* A layer's zero values. */
#define BTBH_COVER_DEFAULT_SLOPE_DEGREES 60.0f
#define BTBH_COVER_DEFAULT_EDGE_DEGREES 8.0f
#define BTBH_COVER_DEFAULT_NOISE_SCALE 1.0f
#define BTBH_COVER_SNOW_ROUGHNESS 0.85f
#define BTBH_COVER_SAND_ROUGHNESS 0.95f
#define BTBH_COVER_MUD_ROUGHNESS 0.55f
/* MUD's puddles: their roughness, reached where the noise is low (half of it dry, all of it soaked). */
#define BTBH_COVER_PUDDLE_ROUGHNESS 0.12f
/* The second octave's frequency over the first's, and its offset. */
#define BTBH_COVER_OCTAVE_SCALE 2.03f
#define BTBH_COVER_OCTAVE_OFFSET 17.31f
/* production_model.frag's cover_pattern[].y bits. */
#define BTBH_COVER_FLAG_COARSE 1u
#define BTBH_COVER_FLAG_DEBUG 2u
/* BatchExtension.world_detail_mode.z: bits 0-1 the layer + 1 (0 none), bit 2 the batch is drawn in the transparent pass
   (its alpha rises to the cover's). */
#define BTBH_COVER_MODE_LAYER_MASK 3u
#define BTBH_COVER_MODE_SEE_THROUGH 4u
/* Still ice's zero values. */
#define BTBH_ICE_DEFAULT_PLATE 1.2f
#define BTBH_ICE_DEFAULT_WIDTH 0.025f
#define BTBH_ICE_DEFAULT_LIFT 0.5f
#define BTBH_ICE_DEFAULT_TILT 0.035f
#define BTBH_ICE_DEFAULT_FROST_SIZE 3.0f
#define BTBH_ICE_DEFAULT_CLEAR_ROUGHNESS 0.06f
#define BTBH_ICE_DEFAULT_FROST_ROUGHNESS 0.6f

/* The fourth fragment uniform buffer of the scene shaders (production_model.frag's SurfaceCover). 128 bytes. */
typedef struct BTBHGPUCoverUniforms {
    float color_roughness[2][4];   /* linear rgb; w the roughness (the pattern's when the layer's is 0) */
    float facing[2][4];            /* x amount x slope, y 1 / edge, z the noise's reach (radians), w cells a unit */
    float relief[2][4];            /* x bump (0 with COARSE), y normal influence, zw 0 */
    uint32_t pattern[2][4];        /* x the pattern, y BTBH_COVER_FLAG_* bits, zw 0 */
} BTBHGPUCoverUniforms;

/* ---------------------------------------------------------------------------------------------- the setter's checks */

/* Why a layer is refused, or true. message names the layer and the field. */
static inline bool btbh_cover_layer_check(
    const BTBHGPUSceneCoverLayer *layer,
    uint32_t index,
    char *message,
    size_t capacity
) {
    const struct {
        const char *name;
        float value;
        float low;
        float high;
    } floats[] = {
        {"color[0]", layer->color[0], 0.0f, 1.0f},
        {"color[1]", layer->color[1], 0.0f, 1.0f},
        {"color[2]", layer->color[2], 0.0f, 1.0f},
        {"amount", layer->amount, 0.0f, 1.0f},
        {"slope_degrees", layer->slope_degrees, 0.0f, 90.0f},
        {"edge_degrees", layer->edge_degrees, 0.0f, 90.0f},
        {"edge_noise_degrees", layer->edge_noise_degrees, 0.0f, 90.0f},
        {"noise_scale", layer->noise_scale, 0.0f, INFINITY},
        {"roughness", layer->roughness, 0.0f, 1.0f},
        {"bump", layer->bump, 0.0f, 1.0f},
        {"normal_influence", layer->normal_influence, 0.0f, 1.0f},
        {"base_height", layer->base_height, 0.0f, 0.0f},
        {"base_fade", layer->base_fade, 0.0f, 0.0f},
        {"wind[0]", layer->wind[0], 0.0f, 0.0f},
        {"wind[1]", layer->wind[1], 0.0f, 0.0f},
    };
    for (size_t field = 0u; field < sizeof(floats) / sizeof(floats[0]); ++field) {
        const float value = floats[field].value;
        if (!isfinite(value)) {
            (void)snprintf(message, capacity, "Cover layer %u: %s is not finite", index, floats[field].name);
            return false;
        }
        if (value < floats[field].low || value > floats[field].high) {
            if (floats[field].high == 0.0f) {
                (void)snprintf(
                    message,
                    capacity,
                    "Cover layer %u: %s is RFC 0020 phase 2's and must be 0 in this release",
                    index,
                    floats[field].name
                );
            } else if (isinf(floats[field].high)) {
                (void)snprintf(message, capacity, "Cover layer %u: %s is below 0", index, floats[field].name);
            } else {
                (void)snprintf(
                    message,
                    capacity,
                    "Cover layer %u: %s is outside %g..%g",
                    index,
                    floats[field].name,
                    (double)floats[field].low,
                    (double)floats[field].high
                );
            }
            return false;
        }
    }
    if (layer->pattern >= (uint8_t)BTBH_GPU_SCENE_COVER_PATTERN_COUNT) {
        (void)snprintf(message, capacity, "Cover layer %u: pattern %u is unknown", index, (unsigned)layer->pattern);
        return false;
    }
    if ((layer->flags & ~(uint8_t)BTBH_GPU_SCENE_COVER_LAYER_COARSE) != 0u) {
        (void)snprintf(message, capacity, "Cover layer %u: flags has an unknown bit", index);
        return false;
    }
    if (layer->reserved[0] != 0u || layer->reserved[1] != 0u) {
        (void)snprintf(message, capacity, "Cover layer %u: reserved is not 0", index);
        return false;
    }
    return true;
}

/* Why a cover is refused, or true (NULL clears: always taken). */
static inline bool btbh_cover_check(const BTBHGPUSceneCover *cover, char *message, size_t capacity) {
    if (cover == NULL) {
        return true;
    }
    if (cover->flags != 0u) {
        (void)snprintf(message, capacity, "Cover: flags is not 0");
        return false;
    }
    if (cover->reserved != 0u) {
        (void)snprintf(message, capacity, "Cover: reserved is not 0");
        return false;
    }
    for (uint32_t index = 0u; index < BTBH_GPU_SCENE_COVER_LAYER_COUNT; ++index) {
        if (!btbh_cover_layer_check(&cover->layers[index], index, message, capacity)) {
            return false;
        }
    }
    return true;
}

/* -------------------------------------------------------------------------------------------------------- the block */

/* A layer's roughness with its zero value resolved. */
static inline float btbh_cover_layer_roughness(const BTBHGPUSceneCoverLayer *layer) {
    if (layer->roughness > 0.0f) {
        return layer->roughness;
    }
    return layer->pattern == BTBH_GPU_SCENE_COVER_SNOW
        ? BTBH_COVER_SNOW_ROUGHNESS
        : (layer->pattern == BTBH_GPU_SCENE_COVER_SAND ? BTBH_COVER_SAND_ROUGHNESS : BTBH_COVER_MUD_ROUGHNESS);
}

/* The block for a window's cover (NULL: none; every layer off). debug: BTBH_DEBUG_COVER. */
static inline void btbh_cover_pack(const BTBHGPUSceneCover *cover, bool debug, BTBHGPUCoverUniforms *uniforms) {
    memset(uniforms, 0, sizeof(*uniforms));
    if (cover == NULL) {
        return;
    }
    const float radians = BTBH_COVER_PI / 180.0f;
    for (uint32_t index = 0u; index < BTBH_GPU_SCENE_COVER_LAYER_COUNT; ++index) {
        const BTBHGPUSceneCoverLayer *layer = &cover->layers[index];
        const float slope = layer->slope_degrees > 0.0f ? layer->slope_degrees : BTBH_COVER_DEFAULT_SLOPE_DEGREES;
        const float edge = layer->edge_degrees > 0.0f ? layer->edge_degrees : BTBH_COVER_DEFAULT_EDGE_DEGREES;
        const bool coarse = (layer->flags & BTBH_GPU_SCENE_COVER_LAYER_COARSE) != 0u;
        uniforms->color_roughness[index][0] = layer->color[0];
        uniforms->color_roughness[index][1] = layer->color[1];
        uniforms->color_roughness[index][2] = layer->color[2];
        uniforms->color_roughness[index][3] = btbh_cover_layer_roughness(layer);
        uniforms->facing[index][0] = layer->amount * slope * radians;
        uniforms->facing[index][1] = 1.0f / (edge * radians);
        /* The noise's reach grows with the amount up to 0.25, so a thin cover thins out to nothing (at amount 0 none of
           it is left anywhere, noise or not) instead of jumping to patches. */
        const float noise_share = layer->amount * 4.0f < 1.0f ? layer->amount * 4.0f : 1.0f;
        uniforms->facing[index][2] = layer->edge_noise_degrees * noise_share * radians;
        uniforms->facing[index][3] = layer->noise_scale > 0.0f ? layer->noise_scale : BTBH_COVER_DEFAULT_NOISE_SCALE;
        uniforms->relief[index][0] = coarse ? 0.0f : layer->bump;
        uniforms->relief[index][1] = layer->normal_influence;
        uniforms->pattern[index][0] = layer->pattern;
        uniforms->pattern[index][1] = (coarse ? BTBH_COVER_FLAG_COARSE : 0u) | (debug ? BTBH_COVER_FLAG_DEBUG : 0u);
    }
}

/* Whether the window's layer `index` lays anything (a cover set, its amount above 0). */
static inline bool btbh_cover_layer_on(const BTBHGPUSceneCover *cover, uint32_t index) {
    return cover != NULL && index < BTBH_GPU_SCENE_COVER_LAYER_COUNT && cover->layers[index].amount > 0.0f;
}

/* The layer a batch's bits pick: 1 the first, 2 the second, 0 none (both bits are refused before this is asked). */
static inline uint32_t btbh_cover_batch_bits(const BTBHGPUSceneGeometryBatch *batch) {
    if ((batch->flags2 & BTBH_GPU_SCENE_BATCH_FLAGS2_COVER_0) != 0u) {
        return 1u;
    }
    return (batch->flags2 & BTBH_GPU_SCENE_BATCH_FLAGS2_COVER_1) != 0u ? 2u : 0u;
}

/* world_detail_mode.z for a batch: the layer it takes when that layer is on (0 otherwise, so the shader runs none of
   it), and whether it is drawn in the transparent pass. */
static inline uint32_t btbh_cover_batch_mode(
    const BTBHGPUSceneCover *cover,
    const BTBHGPUSceneGeometryBatch *batch,
    bool see_through
) {
    const uint32_t bits = btbh_cover_batch_bits(batch);
    if (bits == 0u || !btbh_cover_layer_on(cover, bits - 1u)) {
        return 0u;
    }
    return bits | (see_through ? BTBH_COVER_MODE_SEE_THROUGH : 0u);
}

/* Why a still-ice batch's parameters are refused, or NULL. */
static inline const char *btbh_ice_parameters_problem(const BTBHGPUSceneGeometryBatch *batch) {
    const float *p0 = batch->surface_parameters0;
    const float *p1 = batch->surface_parameters1;
    for (int index = 0; index < 4; ++index) {
        if (!(p0[index] >= 0.0f) || !(p1[index] >= 0.0f) || !isfinite(p0[index]) || !isfinite(p1[index])) {
            return "ICE's surface_parameters must be finite and 0 or more";
        }
    }
    if (p1[0] > 1.0f || p1[2] > 1.0f || p1[3] > 1.0f) {
        return "ICE's frost (surface_parameters1[0]) and roughnesses (surface_parameters1[2], [3]) must be within 0..1";
    }
    return NULL;
}

/* ---------------------------------------------------------------------------------------- the reference arithmetic */

static inline float btbh_cover_saturate(float value) {
    return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

static inline float btbh_cover_smoothstep(float low, float high, float value) {
    const float t = btbh_cover_saturate((value - low) / (high - low));
    return t * t * (3.0f - 2.0f * t);
}

static inline float btbh_cover_lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

/* production_cover.hlsli's btbh_cover_hash (btbh_detail_hash's integer hash, whole). */
static inline uint32_t btbh_cover_hash(float cell_x, float cell_y) {
    const int32_t x = (int32_t)cell_x;
    const int32_t y = (int32_t)cell_y;
    uint32_t hash = (uint32_t)x * 0x8da6b343u ^ (uint32_t)y * 0xd8163841u;
    hash ^= hash >> 15;
    hash *= 0x2c1b3c6du;
    hash ^= hash >> 12;
    hash *= 0x297a2d39u;
    hash ^= hash >> 15;
    return hash;
}

static inline float btbh_cover_hash01(float cell_x, float cell_y) {
    return (float)(btbh_cover_hash(cell_x, cell_y) & 0x00ffffffu) * (1.0f / 16777216.0f);
}

/* Value noise in 0..1 at (x, y) and its gradient per cell. */
static inline float btbh_cover_value_noise(float x, float y, float gradient[2]) {
    const float cell_x = floorf(x);
    const float cell_y = floorf(y);
    const float fx = x - cell_x;
    const float fy = y - cell_y;
    const float ux = fx * fx * (3.0f - 2.0f * fx);
    const float uy = fy * fy * (3.0f - 2.0f * fy);
    const float dux = 6.0f * fx * (1.0f - fx);
    const float duy = 6.0f * fy * (1.0f - fy);
    const float a = btbh_cover_hash01(cell_x, cell_y);
    const float b = btbh_cover_hash01(cell_x + 1.0f, cell_y);
    const float c = btbh_cover_hash01(cell_x, cell_y + 1.0f);
    const float d = btbh_cover_hash01(cell_x + 1.0f, cell_y + 1.0f);
    const float k1 = b - a;
    const float k2 = c - a;
    const float k3 = a - b - c + d;
    gradient[0] = dux * (k1 + k3 * uy);
    gradient[1] = duy * (k2 + k3 * ux);
    return a + k1 * ux + k2 * uy + k3 * ux * uy;
}

/* The cover's noise N (-1..1) at noise-cell position (x, y), footprint the first octave's cells per pixel; each
   octave's gradient (faded) out. */
static inline float btbh_cover_noise(
    float x,
    float y,
    float footprint,
    bool coarse,
    float gradient1[2],
    float gradient2[2]
) {
    float g1[2];
    const float n1 = btbh_cover_value_noise(x, y, g1);
    const float fade1 = btbh_cover_saturate((0.6f - footprint) * 2.5f);
    gradient1[0] = g1[0] * fade1;
    gradient1[1] = g1[1] * fade1;
    float sum = (n1 - 0.5f) * fade1;
    gradient2[0] = 0.0f;
    gradient2[1] = 0.0f;
    if (!coarse) {
        float g2[2];
        const float n2 = btbh_cover_value_noise(
            x * BTBH_COVER_OCTAVE_SCALE + BTBH_COVER_OCTAVE_OFFSET,
            y * BTBH_COVER_OCTAVE_SCALE + BTBH_COVER_OCTAVE_OFFSET,
            g2
        );
        const float fade2 = btbh_cover_saturate((0.6f - footprint * BTBH_COVER_OCTAVE_SCALE) * 2.5f);
        gradient2[0] = g2[0] * fade2;
        gradient2[1] = g2[1] * fade2;
        sum += 0.5f * (n2 - 0.5f) * fade2;
    }
    return sum * (4.0f / 3.0f);
}

static inline float btbh_cover_edge(uint32_t pattern, float c) {
    if (pattern == (uint32_t)BTBH_GPU_SCENE_COVER_SNOW) {
        return btbh_cover_smoothstep(0.3f, 0.7f, c);
    }
    if (pattern == (uint32_t)BTBH_GPU_SCENE_COVER_MUD) {
        return btbh_cover_smoothstep(0.15f, 0.55f, c);
    }
    return c;
}

static inline float btbh_cover_grain(uint32_t pattern) {
    return pattern == (uint32_t)BTBH_GPU_SCENE_COVER_SNOW
        ? 0.04f
        : (pattern == (uint32_t)BTBH_GPU_SCENE_COVER_SAND ? 0.10f : 0.15f);
}

static inline void btbh_cover_normalize(float v[3], const float fallback[3]) {
    const float length = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (length > 1.0e-6f) {
        v[0] /= length;
        v[1] /= length;
        v[2] /= length;
    } else {
        v[0] = fallback[0];
        v[1] = fallback[1];
        v[2] = fallback[2];
    }
}

/* What the cover does at a point: its coverage c (after the pattern's edge), the noise, the albedo, roughness, normal
   and occlusion it leaves. */
typedef struct BTBHCoverSample {
    float c;
    float noise;
    float albedo[3];
    float roughness;
    float metalness;
    float occlusion;
    float normal[3];
} BTBHCoverSample;

/* The reference of apply_cover for layer `index` of a packed block, at flat world (x, y, z) with a pixel footprint of
   `pixel` world units (the larger of the two screen derivatives' x-z lengths, the LOD bias applied), the vertex normal
   and the shading normal (both unit, turned to the side seen), and the surface's albedo, roughness, metalness and
   occlusion before it. wetness: BTBHGPUSceneLighting.surface_wetness. Past the slope the layer can reach (amount x
   slope + the noise's reach), the surface is left as it was. */
static inline BTBHCoverSample btbh_cover_sample(
    const BTBHGPUCoverUniforms *block,
    uint32_t index,
    const float position[3],
    float pixel,
    const float vertex_normal[3],
    const float shading_normal[3],
    const float albedo[3],
    float roughness,
    float metalness,
    float occlusion,
    float wetness
) {
    BTBHCoverSample sample;
    const float *color_roughness = block->color_roughness[index];
    const float *facing = block->facing[index];
    const float *relief = block->relief[index];
    const uint32_t pattern = block->pattern[index][0];
    const bool coarse = (block->pattern[index][1] & BTBH_COVER_FLAG_COARSE) != 0u;
    float facing_normal[3];
    for (int axis = 0; axis < 3; ++axis) {
        facing_normal[axis] = btbh_cover_lerp(vertex_normal[axis], shading_normal[axis], relief[1]);
    }
    btbh_cover_normalize(facing_normal, vertex_normal);
    const float up = facing_normal[1] < -1.0f ? -1.0f : (facing_normal[1] > 1.0f ? 1.0f : facing_normal[1]);
    const float slope = acosf(up);
    if (slope >= facing[0] + facing[2]) {
        /* Past the farthest it reaches: the surface as it was, no noise worked out. */
        sample.c = 0.0f;
        sample.noise = 0.0f;
        for (int channel = 0; channel < 3; ++channel) {
            sample.albedo[channel] = albedo[channel];
            sample.normal[channel] = shading_normal[channel];
        }
        sample.roughness = roughness;
        sample.metalness = metalness;
        sample.occlusion = occlusion;
        return sample;
    }
    float gradient1[2] = {0.0f, 0.0f};
    float gradient2[2] = {0.0f, 0.0f};
    float noise = 0.0f;
    if (facing[2] > 0.0f) {
        noise = btbh_cover_noise(
            position[0] * facing[3],
            position[2] * facing[3],
            facing[3] * pixel,
            coarse,
            gradient1,
            gradient2
        );
    }
    const float c = btbh_cover_edge(
        pattern,
        btbh_cover_saturate((facing[0] + facing[2] * noise - slope) * facing[1])
    );
    float cover_normal[3] = {vertex_normal[0], vertex_normal[1], vertex_normal[2]};
    if (relief[0] > 0.0f) {
        float tilt[2];
        if (pattern == (uint32_t)BTBH_GPU_SCENE_COVER_SNOW) {
            tilt[0] = gradient1[0] * 0.5f;
            tilt[1] = gradient1[1] * 0.5f;
        } else if (pattern == (uint32_t)BTBH_GPU_SCENE_COVER_SAND) {
            tilt[0] = gradient2[0] * 0.35f;
            tilt[1] = gradient2[1] * 0.35f;
        } else {
            tilt[0] = (gradient1[0] + gradient2[0]) * 0.6f;
            tilt[1] = (gradient1[1] + gradient2[1]) * 0.6f;
        }
        const float gradient[3] = {tilt[0] * relief[0], 0.0f, tilt[1] * relief[0]};
        const float along = vertex_normal[0] * gradient[0] + vertex_normal[1] * gradient[1] +
            vertex_normal[2] * gradient[2];
        for (int axis = 0; axis < 3; ++axis) {
            cover_normal[axis] = vertex_normal[axis] - (gradient[axis] - vertex_normal[axis] * along);
        }
        btbh_cover_normalize(cover_normal, vertex_normal);
    }
    float cover_roughness = color_roughness[3];
    if (pattern == (uint32_t)BTBH_GPU_SCENE_COVER_MUD) {
        const float puddle = 1.0f - btbh_cover_smoothstep(-0.4f, 0.2f, noise);
        cover_roughness = btbh_cover_lerp(
            cover_roughness,
            BTBH_COVER_PUDDLE_ROUGHNESS,
            puddle * (0.5f + 0.5f * btbh_cover_saturate(wetness))
        );
    }
    const float grain = 1.0f + btbh_cover_grain(pattern) * noise;
    for (int channel = 0; channel < 3; ++channel) {
        sample.albedo[channel] = btbh_cover_lerp(albedo[channel], color_roughness[channel] * grain, c);
        sample.normal[channel] = btbh_cover_lerp(shading_normal[channel], cover_normal[channel], c);
    }
    btbh_cover_normalize(sample.normal, shading_normal);
    sample.c = c;
    sample.noise = noise;
    sample.roughness = btbh_cover_lerp(roughness, cover_roughness, c);
    sample.metalness = btbh_cover_lerp(metalness, 0.0f, c);
    sample.occlusion = btbh_cover_lerp(occlusion, 1.0f, c);
    return sample;
}

/* -------------------------------------------------------------------------------------------------------- still ice */

/* surface_parameters0 and 1 with their zero values resolved. */
static inline void btbh_ice_defaults(const float in0[4], const float in1[4], float p0[4], float p1[4]) {
    p0[0] = in0[0] > 0.0f ? in0[0] : BTBH_ICE_DEFAULT_PLATE;
    p0[1] = in0[1] > 0.0f ? in0[1] : BTBH_ICE_DEFAULT_WIDTH;
    p0[2] = in0[2] > 0.0f ? in0[2] : BTBH_ICE_DEFAULT_LIFT;
    p0[3] = in0[3] > 0.0f ? in0[3] : BTBH_ICE_DEFAULT_TILT;
    p1[0] = in1[0];
    p1[1] = in1[1] > 0.0f ? in1[1] : BTBH_ICE_DEFAULT_FROST_SIZE;
    p1[2] = in1[2] > 0.0f ? in1[2] : BTBH_ICE_DEFAULT_CLEAR_ROUGHNESS;
    p1[3] = in1[3] > 0.0f ? in1[3] : BTBH_ICE_DEFAULT_FROST_ROUGHNESS;
}

/* btbh_ice_voronoi: the distance from p to the border between the nearest seed and the second nearest (cells), the
   direction across it, the nearest seed's cell. */
static inline float btbh_ice_voronoi(float px, float py, float across[2], float nearest_cell[2]) {
    const float base_x = floorf(px);
    const float base_y = floorf(py);
    float best = 1.0e9f;
    float second = 1.0e9f;
    float best_seed[2] = {0.0f, 0.0f};
    float second_seed[2] = {0.0f, 0.0f};
    nearest_cell[0] = base_x;
    nearest_cell[1] = base_y;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            const float cell_x = base_x + (float)x;
            const float cell_y = base_y + (float)y;
            const uint32_t hash = btbh_cover_hash(cell_x, cell_y);
            const float jitter_x = (float)(hash & 0xffffu) * (1.0f / 65536.0f);
            const float jitter_y = (float)(hash >> 16) * (1.0f / 65536.0f);
            const float seed_x = cell_x + 0.5f + (jitter_x - 0.5f) * 0.85f;
            const float seed_y = cell_y + 0.5f + (jitter_y - 0.5f) * 0.85f;
            const float dx = seed_x - px;
            const float dy = seed_y - py;
            const float distance2 = dx * dx + dy * dy;
            if (distance2 < best) {
                second = best;
                second_seed[0] = best_seed[0];
                second_seed[1] = best_seed[1];
                best = distance2;
                best_seed[0] = seed_x;
                best_seed[1] = seed_y;
                nearest_cell[0] = cell_x;
                nearest_cell[1] = cell_y;
            } else if (distance2 < second) {
                second = distance2;
                second_seed[0] = seed_x;
                second_seed[1] = seed_y;
            }
        }
    }
    const float link_x = second_seed[0] - best_seed[0];
    const float link_y = second_seed[1] - best_seed[1];
    float link_length = sqrtf(link_x * link_x + link_y * link_y);
    link_length = link_length > 1.0e-6f ? link_length : 1.0e-6f;
    across[0] = link_x / link_length;
    across[1] = link_y / link_length;
    const float distance = (0.5f * (best_seed[0] + second_seed[0]) - px) * across[0] +
        (0.5f * (best_seed[1] + second_seed[1]) - py) * across[1];
    return distance > 0.0f ? distance : 0.0f;
}

static inline float btbh_ice_crack(float distance, float width, float footprint) {
    const float wide = width > 0.5f * footprint ? width : 0.5f * footprint;
    return (1.0f - btbh_cover_smoothstep(0.0f, wide, distance)) * (width / (wide > 1.0e-6f ? wide : 1.0e-6f));
}

typedef struct BTBHIceSample {
    float lift_share;
    float roughness;
    float gradient[2];
    float crack_distance;   /* world units, the main grid's */
} BTBHIceSample;

/* The reference of btbh_ice at flat world (x, z), a pixel `footprint` world units wide, with resolved parameters. */
static inline BTBHIceSample btbh_ice_sample(float x, float z, float footprint, const float p0[4], const float p1[4]) {
    BTBHIceSample ice;
    const float plate = p0[0];
    const float width = p0[1];
    const float tilt = p0[3];
    float across[2];
    float cell[2];
    const float distance = btbh_ice_voronoi(x / plate, z / plate, across, cell) * plate;
    ice.crack_distance = distance;
    const float crack = btbh_ice_crack(distance, width, footprint);
    const uint32_t plate_hash = btbh_cover_hash(cell[0] + 113.0f, cell[1] + 71.0f);
    const float tilt_x = tilt * ((float)(plate_hash & 0xffffu) * (1.0f / 65536.0f) * 2.0f - 1.0f);
    const float tilt_y = tilt * ((float)(plate_hash >> 16) * (1.0f / 65536.0f) * 2.0f - 1.0f);
    const float wide = width > 0.5f * footprint ? width : 0.5f * footprint;
    const float bevel = 0.35f * (1.0f - btbh_cover_saturate(distance / (2.0f * wide))) * (width / wide);
    ice.gradient[0] = tilt_x - bevel * across[0];
    ice.gradient[1] = tilt_y - bevel * across[1];
    float share = crack;
    const float fine_width = 0.5f * width;
    if (fine_width * 2.0f >= footprint) {
        float fine_across[2];
        float fine_cell[2];
        const float fine_plate = plate / 2.6f;
        const float fine_distance =
            btbh_ice_voronoi(x / fine_plate + 7.1f, z / fine_plate + 3.3f, fine_across, fine_cell) * fine_plate;
        const float fine = 0.6f * btbh_ice_crack(fine_distance, fine_width, footprint);
        share = share > fine ? share : fine;
    }
    const float frost = p1[0];
    if (frost > 0.0f) {
        float g1[2];
        float g2[2];
        const float fx = x / p1[1];
        const float fz = z / p1[1];
        const float n1 = btbh_cover_value_noise(fx, fz, g1);
        const float n2 = btbh_cover_value_noise(fx * 2.03f + 5.7f, fz * 2.03f + 5.7f, g2);
        const float noise = n1 * (2.0f / 3.0f) + n2 * (1.0f / 3.0f);
        const float frosted = btbh_cover_smoothstep(1.0f - frost - 0.1f, 1.0f - frost + 0.1f, noise);
        share = share > frosted ? share : frosted;
    }
    ice.lift_share = share;
    ice.roughness = btbh_cover_lerp(p1[2], p1[3], share);
    return ice;
}

#endif
