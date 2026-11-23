/* RFC 0012 (0.11.0): the vertex-stage height lift, shared by the core and the native tests.
 *
 * - The heightfield's texels (btbh_gpu_window_heightfield_create): RGBA16F, the height and its slope along u and v in
 *   heights per sample (central differences, one-sided at the edges), alpha 1; the half conversion rounds to nearest
 *   even, as the GPU would.
 * - The lift's checks (the batch's refusals name the batch: btbh_gpu_scene.c).
 * - The 32-byte block the lifted vertex shaders read (production_height_lift.hlsli, cbuffer HeightLift).
 * - A C copy of the shader's lift (the bilinear read with clamped edges, the position, the normal's and the tangent's
 *   tilt) for the tests and the opaque sort. */
#ifndef BTBH_GPU_HEIGHT_LIFT_H
#define BTBH_GPU_HEIGHT_LIFT_H

#include "btbh_gpu/btbh_gpu.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define BTBH_GPU_HEIGHT_LIFT_KNOWN_FLAGS ((uint32_t)BTBH_GPU_SCENE_HEIGHT_LIFT_KEEP_NORMALS)
/* The largest height a heightfield takes: half floats hold the heights and their differences (up to twice this). */
#define BTBH_GPU_HEIGHTFIELD_MAX_HEIGHT 32768.0f

/* ---- half floats ---- */

/* float to IEEE half, rounded to nearest even (finite input within the half range; larger rounds to infinity). */
static inline uint16_t btbh_gpu_height_lift_half(float value) {
    uint32_t bits = 0u;
    memcpy(&bits, &value, sizeof(bits));
    const uint32_t sign = (bits >> 16u) & 0x8000u;
    const uint32_t exponent = (bits >> 23u) & 0xffu;
    uint32_t mantissa = bits & 0x7fffffu;
    if (exponent == 0xffu) {
        return (uint16_t)(sign | 0x7c00u | (mantissa != 0u ? 0x200u : 0u));
    }
    const int32_t half_exponent = (int32_t)exponent - 127 + 15;
    if (half_exponent >= 31) {
        return (uint16_t)(sign | 0x7c00u);
    }
    if (half_exponent <= 0) {
        /* A subnormal half (or zero): shift the full mantissa down, rounding to nearest even. */
        if (half_exponent < -10) {
            return (uint16_t)sign;
        }
        mantissa |= 0x800000u;
        const uint32_t shift = (uint32_t)(14 - half_exponent);
        uint32_t half_mantissa = mantissa >> shift;
        const uint32_t remainder = mantissa & ((1u << shift) - 1u);
        const uint32_t halfway = 1u << (shift - 1u);
        if (remainder > halfway || (remainder == halfway && (half_mantissa & 1u) != 0u)) {
            half_mantissa += 1u;
        }
        return (uint16_t)(sign | half_mantissa);
    }
    uint32_t half = ((uint32_t)half_exponent << 10u) | (mantissa >> 13u);
    const uint32_t remainder = mantissa & 0x1fffu;
    if (remainder > 0x1000u || (remainder == 0x1000u && (half & 1u) != 0u)) {
        half += 1u;   /* may carry into the exponent, up to infinity: still the nearest */
    }
    return (uint16_t)(sign | half);
}

static inline float btbh_gpu_height_lift_from_half(uint16_t half) {
    const uint32_t sign = ((uint32_t)half & 0x8000u) << 16u;
    const uint32_t exponent = ((uint32_t)half >> 10u) & 0x1fu;
    const uint32_t mantissa = (uint32_t)half & 0x3ffu;
    uint32_t bits;
    if (exponent == 0u) {
        if (mantissa == 0u) {
            bits = sign;
        } else {
            /* Subnormal: value = mantissa * 2^-24. */
            const float value = (float)mantissa * 5.9604644775390625e-8f;
            memcpy(&bits, &value, sizeof(bits));
            bits |= sign;
        }
    } else if (exponent == 31u) {
        bits = sign | 0x7f800000u | (mantissa << 13u);
    } else {
        bits = sign | ((exponent - 15u + 127u) << 23u) | (mantissa << 13u);
    }
    float value = 0.0f;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

/* ---- the heightfield ---- */

/* The slope at sample (i, j) along u (axis 0) or v (axis 1), in heights per sample: the central difference inside,
   one-sided at the edges. */
static inline float btbh_gpu_height_lift_slope(
    const float *heights,
    uint32_t width,
    uint32_t height,
    uint32_t i,
    uint32_t j,
    uint32_t axis
) {
    const uint32_t count = axis == 0u ? width : height;
    const uint32_t at = axis == 0u ? i : j;
    const uint32_t lower = at > 0u ? at - 1u : 0u;
    const uint32_t upper = at + 1u < count ? at + 1u : count - 1u;
    const size_t lower_index = axis == 0u ? (size_t)j * width + lower : (size_t)lower * width + i;
    const size_t upper_index = axis == 0u ? (size_t)j * width + upper : (size_t)upper * width + i;
    const float difference = heights[upper_index] - heights[lower_index];
    return upper - lower == 2u ? 0.5f * difference : difference;
}

/* The heightfield's texels, 4 halves each (height, slope u, slope v, 1), row by row. width and height are at least
   2. */
static inline void btbh_gpu_height_lift_texels(
    const float *heights,
    uint32_t width,
    uint32_t height,
    uint16_t *texels
) {
    for (uint32_t j = 0u; j < height; ++j) {
        for (uint32_t i = 0u; i < width; ++i) {
            uint16_t *texel = &texels[((size_t)j * width + i) * 4u];
            texel[0] = btbh_gpu_height_lift_half(heights[(size_t)j * width + i]);
            texel[1] = btbh_gpu_height_lift_half(btbh_gpu_height_lift_slope(heights, width, height, i, j, 0u));
            texel[2] = btbh_gpu_height_lift_half(btbh_gpu_height_lift_slope(heights, width, height, i, j, 1u));
            texel[3] = 0x3c00u;
        }
    }
}

/* A bilinear read of `channels` floats a texel (texel centres at (i + 0.5) / width, edges clamped), as the GPU's
   linear filter with SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE reads it (without its 8-bit weights). */
static inline void btbh_gpu_height_lift_bilinear(
    const float *texels,
    uint32_t channels,
    uint32_t width,
    uint32_t height,
    float u,
    float v,
    float *out
) {
    const float x = u * (float)width - 0.5f;
    const float y = v * (float)height - 0.5f;
    const float fx = floorf(x);
    const float fy = floorf(y);
    const float tx = x - fx;
    const float ty = y - fy;
    const float max_i = (float)(width - 1u);
    const float max_j = (float)(height - 1u);
    const uint32_t i0 = (uint32_t)fminf(fmaxf(fx, 0.0f), max_i);
    const uint32_t i1 = (uint32_t)fminf(fmaxf(fx + 1.0f, 0.0f), max_i);
    const uint32_t j0 = (uint32_t)fminf(fmaxf(fy, 0.0f), max_j);
    const uint32_t j1 = (uint32_t)fminf(fmaxf(fy + 1.0f, 0.0f), max_j);
    for (uint32_t c = 0u; c < channels; ++c) {
        const float a = texels[((size_t)j0 * width + i0) * channels + c];
        const float b = texels[((size_t)j0 * width + i1) * channels + c];
        const float d = texels[((size_t)j1 * width + i0) * channels + c];
        const float e = texels[((size_t)j1 * width + i1) * channels + c];
        const float top = a + (b - a) * tx;
        const float bottom = d + (e - d) * tx;
        out[c] = top + (bottom - top) * ty;
    }
}

/* ---- the lift ---- */

/* Whether a batch draws lifted: a lift with a heightfield. */
static inline bool btbh_gpu_height_lift_active(const BTBHGPUSceneGeometryBatch *batch) {
    return batch != NULL && batch->height_lift != NULL && batch->height_lift->heightfield != NULL;
}

/* The lift's own faults (NULL: none). Without a heightfield only its flags and reserved word are checked. */
static inline const char *btbh_gpu_height_lift_problem(const BTBHGPUSceneHeightLift *lift) {
    if ((lift->flags & ~BTBH_GPU_HEIGHT_LIFT_KNOWN_FLAGS) != 0u) {
        return "flags has a bit the renderer doesn't know (BTBH_GPU_SCENE_HEIGHT_LIFT_*)";
    }
    if (lift->reserved != 0u) {
        return "reserved must be 0";
    }
    if (lift->heightfield == NULL) {
        return NULL;
    }
    if (!isfinite(lift->world_to_uv[0]) || !isfinite(lift->world_to_uv[1]) ||
        !isfinite(lift->world_to_uv[2]) || !isfinite(lift->world_to_uv[3])) {
        return "world_to_uv must be finite";
    }
    if (lift->world_to_uv[0] == 0.0f || lift->world_to_uv[1] == 0.0f) {
        return "world_to_uv's scales ([0] and [1]) must not be 0";
    }
    if (!isfinite(lift->height_scale)) {
        return "height_scale must be finite";
    }
    if (!isfinite(lift->height_offset)) {
        return "height_offset must be finite";
    }
    return NULL;
}

/* The batch's faults with a lift whose heightfield is set (NULL: none): only static batches that cast no shadow.
   directional_visibility: the batch's directional_shadow_visibility byte, or 1 without that array. */
static inline const char *btbh_gpu_height_lift_batch_problem(
    const BTBHGPUSceneGeometryBatch *batch,
    uint8_t directional_visibility
) {
    if (batch->animation_library != NULL || batch->animation_pose != NULL) {
        return "a lifted batch must be static: animation_library and animation_pose must be NULL (phase 2)";
    }
    if (batch->billboard_mode != BTBH_GPU_SCENE_BILLBOARD_DISABLED) {
        return "a lifted batch can't be a billboard (billboard_mode must be 0; phase 2)";
    }
    if (batch->dug_by != NULL) {
        return "a lifted batch can't be dug (dug_by must be NULL)";
    }
    /* The shadow passes' casters (btbh_gpu_scene_render_directional_shadow and the spot and omni maps): opaque
       batches, in the directional maps unless BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW or their visibility byte says no, in
       the spot (or omni) map with spot_shadow_visible. */
    if (batch->transparency_enabled == 0u &&
        ((((uint32_t)batch->flags & BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW) == 0u && directional_visibility != 0u) ||
         batch->spot_shadow_visible != 0u)) {
        return "a lifted batch casts no shadow until phase 2: set BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW and leave "
            "spot_shadow_visible 0, or draw it transparent";
    }
    return NULL;
}

/* The lifted vertex shaders' block (production_height_lift.hlsli, cbuffer HeightLift): uv the mapping; scale x the
   height scale (0 means 1), y the offset, z and w what a sample's slope (heights per sample) is multiplied by to give
   dh/dx and dh/dz in world units, 0 with BTBH_GPU_SCENE_HEIGHT_LIFT_KEEP_NORMALS (the tilt then adds 0). */
typedef struct BTBHGPUHeightLiftUniforms {
    float uv[4];
    float scale[4];
} BTBHGPUHeightLiftUniforms;

static inline void btbh_gpu_height_lift_pack(
    const BTBHGPUSceneHeightLift *lift,
    uint32_t field_width,
    uint32_t field_height,
    BTBHGPUHeightLiftUniforms *uniforms
) {
    const float scale = lift->height_scale == 0.0f ? 1.0f : lift->height_scale;
    const bool keep_normals = (lift->flags & BTBH_GPU_SCENE_HEIGHT_LIFT_KEEP_NORMALS) != 0u;
    memcpy(uniforms->uv, lift->world_to_uv, sizeof(uniforms->uv));
    uniforms->scale[0] = scale;
    uniforms->scale[1] = lift->height_offset;
    uniforms->scale[2] = keep_normals ? 0.0f : scale * lift->world_to_uv[0] * (float)field_width;
    uniforms->scale[3] = keep_normals ? 0.0f : scale * lift->world_to_uv[1] * (float)field_height;
}

/* The shader's btbh_height_lift: x the lift at a flat world (x, z), yz its slope dh/dx and dh/dz in world units.
   texels: the heightfield's texels as floats (3 a texel: height, slope u, slope v), e.g. from its halves. */
static inline void btbh_gpu_height_lift_at(
    const BTBHGPUHeightLiftUniforms *uniforms,
    const float *texels,
    uint32_t field_width,
    uint32_t field_height,
    float x,
    float z,
    float lift[3]
) {
    const float u = x * uniforms->uv[0] + uniforms->uv[2];
    const float v = z * uniforms->uv[1] + uniforms->uv[3];
    float h[3];
    btbh_gpu_height_lift_bilinear(texels, 3u, field_width, field_height, u, v, h);
    lift[0] = h[0] * uniforms->scale[0] + uniforms->scale[1];
    lift[1] = h[1] * uniforms->scale[2];
    lift[2] = h[2] * uniforms->scale[3];
}

/* The lift applied to a vertex in world space, as production_model.vert under BTBH_HEIGHT_LIFT does: the position
   rises by lift[0]; the normal goes through the inverse transpose of the lift's Jacobian and the tangent through the
   Jacobian ((x, y, z) -> (x, y + h(x, z), z)), so vertical faces keep their normals and level ones tilt. Normals and
   tangents unnormalised (the shader normalises after). */
static inline void btbh_gpu_height_lift_apply(
    const float lift[3],
    float position[3],
    float normal[3],
    float tangent[3]
) {
    position[1] += lift[0];
    if (normal != NULL) {
        normal[0] = normal[0] - lift[1] * normal[1];
        normal[2] = normal[2] - lift[2] * normal[1];
    }
    if (tangent != NULL) {
        tangent[1] += lift[1] * tangent[0] + lift[2] * tangent[2];
    }
}

/* The opaque sort's lift (btbh_gpu_scene_batch_view_depth): the height at a flat world (x, z) from the heightfield's
   CPU heights, read bilinearly with clamped edges. */
static inline float btbh_gpu_height_lift_height(
    const BTBHGPUSceneHeightLift *lift,
    const float *heights,
    uint32_t field_width,
    uint32_t field_height,
    float x,
    float z
) {
    const float scale = lift->height_scale == 0.0f ? 1.0f : lift->height_scale;
    const float u = x * lift->world_to_uv[0] + lift->world_to_uv[2];
    const float v = z * lift->world_to_uv[1] + lift->world_to_uv[3];
    float h = 0.0f;
    btbh_gpu_height_lift_bilinear(heights, 1u, field_width, field_height, u, v, &h);
    return h * scale + lift->height_offset;
}

#endif
