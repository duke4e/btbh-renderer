/* RFC 0008: the world detail (BTBHGPUSceneWorldDetail) in C, and RFC 0009's table of them
 * (BTBHGPUSceneWorldDetailTable).
 *
 * - How the scene checks a detail and packs it into the batch's second fragment uniform buffer.
 * - How it checks a table and packs it into the third (WorldDetailTable), and which entry a vertex's index picks.
 * - A copy of shaders/gpu/production_world_detail.hlsli (the projection, its frame, the seed's hash, the normal
 *   map's tilt, and the table's index and entry choice), kept in step with it. Tests/btbh_gpu_world_detail_test.c
 *   checks this copy against the RFCs' examples without a GPU, and Tests/btbh_gpu_world_detail_smoke.c checks the
 *   shader against it.
 *
 * Header-only (static inline): the scene and the tests include it, so the C core's source list doesn't change. */
#ifndef BTBH_GPU_WORLD_DETAIL_H
#define BTBH_GPU_WORLD_DETAIL_H

#include "btbh_gpu/btbh_gpu.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* world_detail_mode.x (production_world_detail.hlsli): which maps the batch samples. */
#define BTBH_GPU_WORLD_DETAIL_MAP_ALBEDO 1u
#define BTBH_GPU_WORLD_DETAIL_MAP_NORMAL 2u
#define BTBH_GPU_WORLD_DETAIL_MAP_ORM 4u
#define BTBH_GPU_WORLD_DETAIL_MAP_ALBEDO_IN_ORM_BLUE 8u
/* RFC 0009: the batch draws its world-detail table (the WorldDetailTable block and the array), not world_detail.
   world_detail_mode.y is then the table's seed component; the tiles and controls are 0. */
#define BTBH_GPU_WORLD_DETAIL_MAP_TABLE 16u

/* level_degrees 0 means this. */
#define BTBH_GPU_WORLD_DETAIL_DEFAULT_LEVEL_DEGREES 18.0f
/* Where the face normal and the vertex normal are this close to perpendicular (|cos| below it), the face normal
 * is turned toward the camera instead of to the vertex normal's side. */
#define BTBH_GPU_WORLD_DETAIL_CAMERA_SIDE 0.05f
/* The seed is rounded and clamped to the floats that convert to a 32-bit int (the largest is 2^31 - 128). */
#define BTBH_GPU_WORLD_DETAIL_SEED_LIMIT 2147483520.0f

/* The flags this release knows. */
#define BTBH_GPU_WORLD_DETAIL_KNOWN_FLAGS ((uint32_t)BTBH_GPU_SCENE_WORLD_DETAIL_ALBEDO_IN_ORM_BLUE)
/* RFC 0009: the table's and an entry's flags (none yet). */
#define BTBH_GPU_WORLD_DETAIL_TABLE_KNOWN_FLAGS 0u
#define BTBH_GPU_WORLD_DETAIL_TABLE_ENTRY_KNOWN_FLAGS 0u

/* ------------------------------------------------------------------------------------------- the descriptor */

/* What is wrong with a detail's own fields (its textures' owners are the scene's to check), or NULL. */
static inline const char *btbh_gpu_world_detail_problem(const BTBHGPUSceneWorldDetail *detail) {
    for (int axis = 0; axis < 2; ++axis) {
        if (!isfinite(detail->level_tile[axis]) || !(detail->level_tile[axis] > 0.0f)) {
            return "level_tile must be finite and above 0";
        }
        if (!isfinite(detail->wall_tile[axis]) || !(detail->wall_tile[axis] > 0.0f)) {
            return "wall_tile must be finite and above 0";
        }
    }
    if (!isfinite(detail->albedo_scale) || !isfinite(detail->normal_scale) || !isfinite(detail->roughness_scale)) {
        return "albedo_scale, normal_scale and roughness_scale must be finite";
    }
    if (!isfinite(detail->level_degrees) || detail->level_degrees < 0.0f || detail->level_degrees >= 90.0f) {
        return "level_degrees must lie in [0, 90)";
    }
    if (detail->seed_component > 4u) {
        return "seed_component must be 0 (none) or 1..4 (custom x..w)";
    }
    if (((uint32_t)detail->flags & ~BTBH_GPU_WORLD_DETAIL_KNOWN_FLAGS) != 0u) {
        return "flags has a bit this release doesn't know";
    }
    if (detail->reserved[0] != 0u || detail->reserved[1] != 0u) {
        return "reserved must be 0";
    }
    if (((uint32_t)detail->flags & BTBH_GPU_SCENE_WORLD_DETAIL_ALBEDO_IN_ORM_BLUE) != 0u) {
        if (detail->albedo_texture != NULL) {
            return "ALBEDO_IN_ORM_BLUE takes the albedo from the ORM texture: albedo_texture must be NULL";
        }
        if (detail->orm_texture == NULL) {
            return "ALBEDO_IN_ORM_BLUE needs an orm_texture";
        }
    }
    return NULL;
}

/* The maps a detail samples (BTBH_GPU_WORLD_DETAIL_MAP_* bits); 0 for none, which draws as without the detail. */
static inline uint32_t btbh_gpu_world_detail_maps(const BTBHGPUSceneWorldDetail *detail) {
    if (detail == NULL) {
        return 0u;
    }
    uint32_t maps = 0u;
    if (detail->albedo_texture != NULL) {
        maps |= BTBH_GPU_WORLD_DETAIL_MAP_ALBEDO;
    }
    if (detail->normal_texture != NULL) {
        maps |= BTBH_GPU_WORLD_DETAIL_MAP_NORMAL;
    }
    if (detail->orm_texture != NULL) {
        maps |= BTBH_GPU_WORLD_DETAIL_MAP_ORM;
        if (((uint32_t)detail->flags & BTBH_GPU_SCENE_WORLD_DETAIL_ALBEDO_IN_ORM_BLUE) != 0u) {
            maps |= BTBH_GPU_WORLD_DETAIL_MAP_ALBEDO_IN_ORM_BLUE;
        }
    }
    return maps;
}

/* Whether a batch draws with its world detail: it has one with a texture, and samples a material (a refracting
 * batch doesn't). Only such a batch leaves the fast unlit pipeline, sets the uniforms and needs 0.6.0's shaders. */
static inline bool btbh_gpu_world_detail_active(const BTBHGPUSceneGeometryBatch *batch) {
    return batch != NULL &&
        batch->surface_variant != BTBH_GPU_SCENE_SURFACE_REFRACT &&
        btbh_gpu_world_detail_maps(batch->world_detail) != 0u;
}

/* cos(level_degrees), 0 meaning the default. */
static inline float btbh_gpu_world_detail_cos_level(float level_degrees) {
    const float degrees = level_degrees > 0.0f ? level_degrees : BTBH_GPU_WORLD_DETAIL_DEFAULT_LEVEL_DEGREES;
    return (float)cos((double)degrees * 3.14159265358979323846 / 180.0);
}

/* RFC 0009: whether a batch draws with its world-detail table: it has one whose map_layers is set, and samples a
 * material (a refracting batch doesn't). Only such a batch leaves the fast unlit pipeline, pushes the table's block
 * and needs 0.8.0's shaders. The scene refuses a batch that has this and an active world_detail too. */
static inline bool btbh_gpu_world_detail_table_active(const BTBHGPUSceneGeometryBatch *batch) {
    return batch != NULL &&
        batch->surface_variant != BTBH_GPU_SCENE_SURFACE_REFRACT &&
        batch->world_detail_table != NULL &&
        batch->world_detail_table->map_layers != NULL;
}

/* The batch extension's world-detail block (production_model.frag.hlsl, BatchExtension): tiles 1 / level_tile
 * (u, v) and 1 / wall_tile (u, v); controls the albedo, normal and roughness scales (0 means 1) and cos(level
 * angle); mode x the maps, y the seed component, zw 0. All zero for a batch that doesn't draw a detail. The detail
 * must have passed btbh_gpu_world_detail_problem. A batch that draws a table (RFC 0009) has mode x
 * BTBH_GPU_WORLD_DETAIL_MAP_TABLE and y the table's seed component, the rest 0: its entries go in the table's own
 * block (btbh_gpu_world_detail_table_pack). */
static inline void btbh_gpu_world_detail_pack(
    const BTBHGPUSceneGeometryBatch *batch,
    float tiles[4],
    float controls[4],
    uint32_t mode[4]
) {
    for (int index = 0; index < 4; ++index) {
        tiles[index] = 0.0f;
        controls[index] = 0.0f;
        mode[index] = 0u;
    }
    if (btbh_gpu_world_detail_table_active(batch)) {
        mode[0] = BTBH_GPU_WORLD_DETAIL_MAP_TABLE;
        mode[1] = (uint32_t)batch->world_detail_table->seed_component;
        return;
    }
    if (!btbh_gpu_world_detail_active(batch)) {
        return;
    }
    const BTBHGPUSceneWorldDetail *detail = batch->world_detail;
    tiles[0] = 1.0f / detail->level_tile[0];
    tiles[1] = 1.0f / detail->level_tile[1];
    tiles[2] = 1.0f / detail->wall_tile[0];
    tiles[3] = 1.0f / detail->wall_tile[1];
    controls[0] = detail->albedo_scale != 0.0f ? detail->albedo_scale : 1.0f;
    controls[1] = detail->normal_scale != 0.0f ? detail->normal_scale : 1.0f;
    controls[2] = detail->roughness_scale != 0.0f ? detail->roughness_scale : 1.0f;
    controls[3] = btbh_gpu_world_detail_cos_level(detail->level_degrees);
    mode[0] = btbh_gpu_world_detail_maps(detail);
    mode[1] = (uint32_t)detail->seed_component;
}

/* ---------------------------------------------------------------------------------- RFC 0009: the table */

/* The scene fragment shaders' third uniform buffer (production_model.frag.hlsl, WorldDetailTable), 400 bytes,
 * pushed only for a batch that draws a table. Per entry: tiles 1 / level_tile (u, v), 1 / wall_tile (u, v);
 * controls the albedo, normal and roughness scales (0 means 1) and cos(level angle); layers x the normal's layer,
 * y the ORM's (BTBH_GPU_SCENE_WORLD_DETAIL_NO_LAYER: absent), zw 0. count x the entry count, yzw 0. The entries
 * past the count are all zero. */
typedef struct BTBHGPUWorldDetailTableUniforms {
    float tiles[BTBH_GPU_SCENE_WORLD_DETAIL_TABLE_MAX_ENTRIES][4];
    float controls[BTBH_GPU_SCENE_WORLD_DETAIL_TABLE_MAX_ENTRIES][4];
    uint32_t layers[BTBH_GPU_SCENE_WORLD_DETAIL_TABLE_MAX_ENTRIES][4];
    uint32_t count[4];
} BTBHGPUWorldDetailTableUniforms;

/* What is wrong with one entry, given the array's layer count, or NULL. */
static inline const char *btbh_gpu_world_detail_table_entry_problem(
    const BTBHGPUSceneWorldDetailTableEntry *entry,
    uint32_t layer_count
) {
    for (int axis = 0; axis < 2; ++axis) {
        if (!isfinite(entry->level_tile[axis]) || !(entry->level_tile[axis] > 0.0f)) {
            return "level_tile must be finite and above 0";
        }
        if (!isfinite(entry->wall_tile[axis]) || !(entry->wall_tile[axis] > 0.0f)) {
            return "wall_tile must be finite and above 0";
        }
    }
    if (!isfinite(entry->albedo_scale) || !isfinite(entry->normal_scale) || !isfinite(entry->roughness_scale)) {
        return "albedo_scale, normal_scale and roughness_scale must be finite";
    }
    if (!isfinite(entry->level_degrees) || entry->level_degrees < 0.0f || entry->level_degrees >= 90.0f) {
        return "level_degrees must lie in [0, 90)";
    }
    if (entry->normal_layer != BTBH_GPU_SCENE_WORLD_DETAIL_NO_LAYER && (uint32_t)entry->normal_layer >= layer_count) {
        return "normal_layer must be 255 (none) or below map_layers' layer count";
    }
    if (entry->orm_layer != BTBH_GPU_SCENE_WORLD_DETAIL_NO_LAYER && (uint32_t)entry->orm_layer >= layer_count) {
        return "orm_layer must be 255 (none) or below map_layers' layer count";
    }
    if (((uint32_t)entry->flags & ~BTBH_GPU_WORLD_DETAIL_TABLE_ENTRY_KNOWN_FLAGS) != 0u) {
        return "flags has a bit this release doesn't know";
    }
    if (entry->reserved != 0u) {
        return "reserved must be 0";
    }
    return NULL;
}

/* What is wrong with a table's own fields and entries, or NULL (its array texture's owner and kind are the scene's
 * to check; layer_count is the array's). With a fault in an entry, *entry_index names it (UINT32_MAX otherwise).
 * A table without map_layers draws as none: only its seed, flags and reserved bytes are checked then. */
static inline const char *btbh_gpu_world_detail_table_problem(
    const BTBHGPUSceneWorldDetailTable *table,
    uint32_t layer_count,
    uint32_t *entry_index
) {
    *entry_index = UINT32_MAX;
    if (table->seed_component > 4u) {
        return "seed_component must be 0 (none) or 1..4 (custom x..w)";
    }
    if (((uint32_t)table->flags & ~BTBH_GPU_WORLD_DETAIL_TABLE_KNOWN_FLAGS) != 0u) {
        return "flags has a bit this release doesn't know";
    }
    if (table->reserved[0] != 0u || table->reserved[1] != 0u) {
        return "reserved must be 0";
    }
    if (table->map_layers == NULL) {
        return NULL;
    }
    if (table->entries == NULL) {
        return "entries is NULL";
    }
    if (table->entry_count == 0u || table->entry_count > BTBH_GPU_SCENE_WORLD_DETAIL_TABLE_MAX_ENTRIES) {
        return "entry_count must be 1..8";
    }
    for (uint32_t index = 0u; index < table->entry_count; ++index) {
        const char *problem = btbh_gpu_world_detail_table_entry_problem(&table->entries[index], layer_count);
        if (problem != NULL) {
            *entry_index = index;
            return problem;
        }
    }
    return NULL;
}

/* The table's block (BTBHGPUWorldDetailTableUniforms). The table must have passed
 * btbh_gpu_world_detail_table_problem and have map_layers. Each entry's tiles and controls as
 * btbh_gpu_world_detail_pack packs a detail's, so one entry and a detail with its floats agree to the bit. */
static inline void btbh_gpu_world_detail_table_pack(
    const BTBHGPUSceneWorldDetailTable *table,
    BTBHGPUWorldDetailTableUniforms *uniforms
) {
    memset(uniforms, 0, sizeof(*uniforms));
    for (uint32_t index = 0u; index < table->entry_count; ++index) {
        const BTBHGPUSceneWorldDetailTableEntry *entry = &table->entries[index];
        uniforms->tiles[index][0] = 1.0f / entry->level_tile[0];
        uniforms->tiles[index][1] = 1.0f / entry->level_tile[1];
        uniforms->tiles[index][2] = 1.0f / entry->wall_tile[0];
        uniforms->tiles[index][3] = 1.0f / entry->wall_tile[1];
        uniforms->controls[index][0] = entry->albedo_scale != 0.0f ? entry->albedo_scale : 1.0f;
        uniforms->controls[index][1] = entry->normal_scale != 0.0f ? entry->normal_scale : 1.0f;
        uniforms->controls[index][2] = entry->roughness_scale != 0.0f ? entry->roughness_scale : 1.0f;
        uniforms->controls[index][3] = btbh_gpu_world_detail_cos_level(entry->level_degrees);
        uniforms->layers[index][0] = (uint32_t)entry->normal_layer;
        uniforms->layers[index][1] = (uint32_t)entry->orm_layer;
    }
    uniforms->count[0] = table->entry_count;
}

/* -------------------------------------------------- the copy of production_world_detail.hlsli (keep in step) */

typedef struct BTBHGPUWorldDetailFrame {
    /* The face normal N, turned to the vertex normal's side (or the camera's). */
    float normal[3];
    /* T, the image's right; B = cross(N, T), toward the image's top. T x B = N. */
    float tangent[3];
    float bitangent[3];
    /* u and v in repeats, before the flip to the image's rows and the seed's offset. */
    float uv[2];
    /* Projected straight down (|N.y| >= cos level) rather than along the face. */
    bool level;
} BTBHGPUWorldDetailFrame;

static inline float btbh_gpu_world_detail_dot(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static inline void btbh_gpu_world_detail_cross(const float a[3], const float b[3], float out[3]) {
    const float x = a[1] * b[2] - a[2] * b[1];
    const float y = a[2] * b[0] - a[0] * b[2];
    const float z = a[0] * b[1] - a[1] * b[0];
    out[0] = x;
    out[1] = y;
    out[2] = z;
}

/* value scaled to length 1, or fallback where it is (nearly) zero. */
static inline void btbh_gpu_world_detail_normalize(const float value[3], const float fallback[3], float out[3]) {
    const float length_squared = btbh_gpu_world_detail_dot(value, value);
    if (length_squared > 1.0e-30f) {
        const float inverse = 1.0f / sqrtf(length_squared);
        out[0] = value[0] * inverse;
        out[1] = value[1] * inverse;
        out[2] = value[2] * inverse;
    } else {
        out[0] = fallback[0];
        out[1] = fallback[1];
        out[2] = fallback[2];
    }
}

/* The projection and its frame at a flat world position (before world_curvature bends it).
 * - face: the face's normal, either way round (the shader takes cross(ddx(P), ddy(P))).
 * - vertex_normal: the interpolated vertex normal, which says which side is the face's outside.
 * - to_camera: from the position to the camera, for a face whose vertex normal lies in its plane.
 * - cos_level: cos(level_degrees); inverse_tiles: 1 / level_tile (u, v), 1 / wall_tile (u, v).
 * Level faces (|N.y| >= cos_level): u = x / level_tile[0], v = -z / level_tile[1] (north), or +z on a face that
 * looks down; T is +x made orthogonal to N. Other faces: T = normalize(cross(up, N)), B = cross(N, T),
 * u = dot(P, T) / wall_tile[0], v = dot(P, B) / wall_tile[1] (up the face). */
static inline void btbh_gpu_world_detail_frame(
    const float position[3],
    const float face[3],
    const float vertex_normal[3],
    const float to_camera[3],
    float cos_level,
    const float inverse_tiles[4],
    BTBHGPUWorldDetailFrame *frame
) {
    static const float up[3] = { 0.0f, 1.0f, 0.0f };
    static const float zero[3] = { 0.0f, 0.0f, 0.0f };
    static const float east[3] = { 1.0f, 0.0f, 0.0f };
    float side_normal[3];
    btbh_gpu_world_detail_normalize(vertex_normal, zero, side_normal);
    float normal[3];
    btbh_gpu_world_detail_normalize(face, btbh_gpu_world_detail_dot(side_normal, side_normal) > 0.0f ? side_normal : up,
        normal);
    const float facing = btbh_gpu_world_detail_dot(normal, side_normal);
    const float side = fabsf(facing) < BTBH_GPU_WORLD_DETAIL_CAMERA_SIDE
        ? btbh_gpu_world_detail_dot(normal, to_camera)
        : facing;
    if (side < 0.0f) {
        normal[0] = -normal[0];
        normal[1] = -normal[1];
        normal[2] = -normal[2];
    }
    float tangent[3];
    frame->level = fabsf(normal[1]) >= cos_level;
    if (frame->level) {
        const float along[3] = {
            1.0f - normal[0] * normal[0],
            -normal[1] * normal[0],
            -normal[2] * normal[0]
        };
        btbh_gpu_world_detail_normalize(along, east, tangent);
    } else {
        const float across[3] = { normal[2], 0.0f, -normal[0] };
        btbh_gpu_world_detail_normalize(across, east, tangent);
    }
    float bitangent[3];
    btbh_gpu_world_detail_cross(normal, tangent, bitangent);
    if (frame->level) {
        frame->uv[0] = position[0] * inverse_tiles[0];
        frame->uv[1] = (normal[1] >= 0.0f ? -position[2] : position[2]) * inverse_tiles[1];
    } else {
        frame->uv[0] = btbh_gpu_world_detail_dot(position, tangent) * inverse_tiles[2];
        frame->uv[1] = btbh_gpu_world_detail_dot(position, bitangent) * inverse_tiles[3];
    }
    for (int axis = 0; axis < 3; ++axis) {
        frame->normal[axis] = normal[axis];
        frame->tangent[axis] = tangent[axis];
        frame->bitangent[axis] = bitangent[axis];
    }
}

/* PCG (Jarzynski and Olano, "Hash Functions for GPU Rendering", 2020). */
static inline uint32_t btbh_gpu_world_detail_hash(uint32_t value) {
    const uint32_t state = value * 747796405u + 2891336453u;
    const uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

/* The seed's offset in repeats along u and v, each in [0, 1): custom[component - 1] rounded to a whole number
 * (to even on a tie, as HLSL's round), hashed, its two 16-bit halves. (0, 0) for component 0. */
static inline void btbh_gpu_world_detail_offset(const float custom[4], uint32_t component, float offset[2]) {
    offset[0] = 0.0f;
    offset[1] = 0.0f;
    if (component == 0u || component > 4u) {
        return;
    }
    float seed = rintf(custom[component - 1u]);
    if (!(seed == seed)) {
        seed = 0.0f;  /* NaN: unspecified on the GPU; 0 here */
    }
    seed = seed < -BTBH_GPU_WORLD_DETAIL_SEED_LIMIT ? -BTBH_GPU_WORLD_DETAIL_SEED_LIMIT : seed;
    seed = seed > BTBH_GPU_WORLD_DETAIL_SEED_LIMIT ? BTBH_GPU_WORLD_DETAIL_SEED_LIMIT : seed;
    const uint32_t hash = btbh_gpu_world_detail_hash((uint32_t)(int32_t)seed);
    offset[0] = (float)(hash & 0xffffu) * (1.0f / 65536.0f);
    offset[1] = (float)(hash >> 16u) * (1.0f / 65536.0f);
}

/* The texture coordinate: (u, -v) plus the offset. The image's top lies toward +v (an OpenGL normal map's +Y). */
static inline void btbh_gpu_world_detail_coordinate(
    const BTBHGPUWorldDetailFrame *frame,
    const float offset[2],
    float coordinate[2]
) {
    coordinate[0] = frame->uv[0] + offset[0];
    coordinate[1] = -frame->uv[1] + offset[1];
}

/* The surface gradient a tangent-space normal adds (normal_xy: the map's xy decoded and times normal_scale): the
 * shading normal becomes normalize(normal - gradient), as the procedural detail's gradient is added. */
static inline void btbh_gpu_world_detail_gradient(
    const BTBHGPUWorldDetailFrame *frame,
    const float normal_xy[2],
    float gradient[3]
) {
    float squared = normal_xy[0] * normal_xy[0] + normal_xy[1] * normal_xy[1];
    squared = squared < 0.0f ? 0.0f : (squared > 1.0f ? 1.0f : squared);
    float z = sqrtf(1.0f - squared);
    z = z > 1.0e-3f ? z : 1.0e-3f;
    for (int axis = 0; axis < 3; ++axis) {
        gradient[axis] = -(normal_xy[0] * frame->tangent[axis] + normal_xy[1] * frame->bitangent[axis]) / z;
    }
}

/* RFC 0009: a vertex's table index from the first float of its joint slot (production_model_detail_table.vert):
 * rounded to the nearest whole number (to even on a tie, as HLSL's round), then 0 below 0, 255 above 255, and 255
 * (none) for NaN. */
static inline uint32_t btbh_gpu_world_detail_table_index(float joint) {
    const float rounded = rintf(joint);
    if (rounded >= 0.0f) {
        return rounded < 255.0f ? (uint32_t)rounded : 255u;
    }
    return rounded < 0.0f ? 0u : 255u;
}

/* The entry whose block the fragment shader reads for an index: min(index, 7). Past the count it is read and
 * thrown away (btbh_gpu_world_detail_table_applies), never branched on. */
static inline uint32_t btbh_gpu_world_detail_table_slot(uint32_t index) {
    return index < BTBH_GPU_SCENE_WORLD_DETAIL_TABLE_MAX_ENTRIES - 1u
        ? index
        : BTBH_GPU_SCENE_WORLD_DETAIL_TABLE_MAX_ENTRIES - 1u;
}

/* Whether a map of the entry an index picks changes the pixel: the index is below the count and the map's layer
 * isn't BTBH_GPU_SCENE_WORLD_DETAIL_NO_LAYER. Otherwise the shader multiplies by 1 and adds no tilt. */
static inline bool btbh_gpu_world_detail_table_applies(uint32_t index, uint32_t count, uint32_t layer) {
    return index < count && layer != BTBH_GPU_SCENE_WORLD_DETAIL_NO_LAYER;
}

#endif
