/* Destructible terrain from SDF edits (RFC 0007).
 *
 * The field is the base (a heightfield or the host's function; in a frozen chunk, its frozen bricks) with every
 * edit applied, in the order the edits were added, where the point lies within that edit's reach (its shape's box
 * grown by its smoothing and SDF_REACH_MARGIN_CELLS cells). Samples sit on a lattice of cells over the bounds;
 * bricks hold 8^3 of them (an int8 distance clamped to +-(sqrt(3)/2) cells, a material and a "differs from the
 * base" bit) and exist only where an edit reaches. Chunks of chunk_cells^3 cells keep the edits that reach them,
 * their bricks and their mesh. Surface nets mesh the cells near differing samples into patches; the dig mask (one
 * texel per cell column) marks where a patch has taken over from the host's ground. */

#include "btbh_gpu_sdf_terrain_internal.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SDF_BRICK 8
#define SDF_BRICK_SAMPLES 512
#define SDF_FROZEN_EDGE 9
#define SDF_FROZEN_SAMPLES 729
#define SDF_DIFFER_WORDS (SDF_BRICK_SAMPLES / 64)
/* How far an edit's reach extends past its shape's box and smoothing. The mesh reads samples up to three cells
 * from a differing one, and a sample differs at most a clamp distance outside the shape's box and smoothing, so
 * every sample a mesh reads lies within some reach (and in an evaluated brick). */
#define SDF_REACH_MARGIN_CELLS 5.0f
/* Patch vertices in cells whose corners all equal the base (the apron) sink this far under the field, so the
 * host's ground, which still draws there, wins the depth test instead of fighting the patch. */
#define SDF_APRON_SINK_CELLS 0.05f
/* The host's ground draws where it lies more than this far under the patch's top surface. */
#define SDF_MASK_THRESHOLD_CELLS 0.1f
#define SDF_MASK_SENTINEL 0x7BFFu
#define SDF_MASK_SENTINEL_VALUE 65504.0f
#define SDF_GRADIENT_STEP_CELLS 0.1f
#define SDF_DEFAULT_CHUNK_CELLS 64u
#define SDF_DEFAULT_MAX_EDITS 4096u
#define SDF_MAX_LATTICE_CELLS 16384
#define SDF_TAG_LIMIT 64u
#define SDF_NO_VERTEX UINT32_MAX
#define SDF_GPU_VERTEX_FLOATS 24u

enum {
    SDF_EDIT_FREE = 0,
    SDF_EDIT_LIVE = 1,
    SDF_EDIT_FROZEN = 2
};

enum {
    SDF_COLUMN_NONE = 0,       /* no surface in this chunk's part of the column: look further down */
    SDF_COLUMN_PATCH = 1,      /* the top surface here is the patch's: the mask holds its height */
    SDF_COLUMN_BASE = 2        /* the top surface here (or above) is the base's: the host's ground stays */
};

typedef struct SdfEdit {
    uint32_t id;
    uint8_t state;
    uint8_t shape;
    uint8_t operation;
    uint8_t material;
    float position[3];
    float inverse_rotation[9];
    float size[3];
    float corner_radius;
    float smoothing;
    float material_band;
    float lipschitz;
    float reach_min[3];
    float reach_max[3];
    uint32_t chunk_refs;
    BTBHGPUSdfEdit source;
} SdfEdit;

typedef struct SdfBrick {
    int8_t distance[SDF_BRICK_SAMPLES];
    uint8_t material[SDF_BRICK_SAMPLES];
    uint64_t differs[SDF_DIFFER_WORDS];
    uint8_t active;
    struct SdfBrick *next_free;
} SdfBrick;

typedef struct SdfFrozenBrick {
    int8_t distance[SDF_FROZEN_SAMPLES];
    uint8_t material[SDF_FROZEN_SAMPLES];
    uint64_t differs[SDF_DIFFER_WORDS];
} SdfFrozenBrick;

typedef struct SdfMesh {
    float *positions;
    float *normals;
    uint8_t *materials;
    uint32_t *indices;
    uint32_t vertex_count;
    uint32_t index_count;
    uint64_t hash;
    float bounds_min[3];
    float bounds_max[3];
    uint8_t *column_kind;
    float *column_height;
    uint64_t bytes;
} SdfMesh;

typedef struct SdfChunk {
    uint32_t *edits;
    uint32_t edit_count;
    uint32_t edit_capacity;
    uintptr_t *bricks;
    uint64_t *stale_bits;
    uintptr_t *frozen;
    uint32_t stale_count;
    uint8_t is_frozen;
    uint8_t needs_mesh;
    uint8_t in_dirty_list;
    uint8_t in_busy_list;
    uint8_t has_pending;
    uint8_t gpu_dirty;
    uint8_t component_mark;
    uint8_t reserved;
    SdfMesh *mesh;
    SdfMesh *pending;
    BTBHGPUCustomMesh *gpu_mesh;
    uint64_t gpu_bytes;
    uint64_t revision;
} SdfChunk;

typedef struct SdfU32List {
    uint32_t *items;
    uint32_t count;
    uint32_t capacity;
} SdfU32List;

struct BTBHGPUSdfTerrain {
    BTBHGPUSdfTerrainDesc desc;
    float *heights;
    float lattice_min[3];
    float lattice_max[3];
    float cell;
    float clamp_distance;
    int32_t cells[3];
    int32_t chunk_cells;
    int32_t bricks_per_chunk;
    int32_t chunks[3];
    uint32_t chunk_total;
    uint32_t slots_per_chunk;
    SdfChunk *chunk_array;
    uint32_t max_edits;

    SdfEdit *edits;
    uint32_t edit_capacity;
    SdfU32List free_edit_slots;
    SdfU32List order;
    uint32_t next_id;
    uint32_t live_count;
    uint32_t frozen_total;

    SdfBrick *free_bricks;
    SdfBrick **slabs;
    uint32_t slab_count;
    uint32_t slab_capacity;
    uint32_t brick_total;
    uint32_t frozen_brick_total;

    uint64_t *stale;
    uint32_t stale_head;
    uint32_t stale_tail;
    uint32_t stale_capacity;
    uint32_t stale_total;
    SdfU32List dirty;
    SdfU32List busy;

    float (*changes)[4];
    uint32_t change_head;
    uint32_t change_count;
    uint32_t change_capacity;

    uint16_t **mask_tiles;
    uint32_t *mask_tile_counts;
    uint8_t *mask_tile_dirty;
    uint64_t mask_texel_total;
    uint32_t mask_tile_total;

    BTBHGPUWindow *window;
    BTBHGPUSdfTerrain *window_next;
    BTBHGPUSdfTerrain *window_previous;
    SDL_GPUTexture *mask_texture;
    SDL_GPUSampler *mask_sampler;
    uint8_t mask_upload_all;

    double brick_microseconds;
    double chunk_microseconds;
    uint32_t last_bricks_evaluated;
    uint32_t last_chunks_meshed;
    SdfU32List gpu_dirty;
    uint64_t mesh_cpu_bytes;
    uint64_t mesh_gpu_bytes;
    float *gpu_vertex_scratch;
    uint32_t gpu_vertex_scratch_capacity;
    struct SdfBrickEval *brick_work;
    uint32_t brick_work_capacity;
};

/* ------------------------------------------------------------------------------------------------ helpers */

static float sdf_minf(float a, float b) { return a < b ? a : b; }
static float sdf_maxf(float a, float b) { return a > b ? a : b; }
static float sdf_clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int32_t sdf_mini(int32_t a, int32_t b) { return a < b ? a : b; }
static int32_t sdf_maxi(int32_t a, int32_t b) { return a > b ? a : b; }

static bool sdf_finite(float v) { return isfinite(v) != 0; }

static uint64_t sdf_now_ticks(void) { return SDL_GetTicksNS(); }

static void sdf_report_message(BTBHGPUSdfTerrainReport *report, BTBHGPUResult result, const char *message) {
    if (report == NULL) {
        return;
    }
    report->result = result;
    (void)snprintf(report->message, sizeof(report->message), "%s", message != NULL ? message : "");
}

static bool sdf_list_push(SdfU32List *list, uint32_t value) {
    if (list->count == list->capacity) {
        const uint32_t capacity = list->capacity < 16u ? 16u : list->capacity * 2u;
        uint32_t *items = (uint32_t *)realloc(list->items, sizeof(uint32_t) * capacity);
        if (items == NULL) {
            return false;
        }
        list->items = items;
        list->capacity = capacity;
    }
    list->items[list->count++] = value;
    return true;
}

static void sdf_list_free(SdfU32List *list) {
    free(list->items);
    list->items = NULL;
    list->count = 0u;
    list->capacity = 0u;
}

static uint64_t sdf_hash_bytes(uint64_t hash, const void *bytes, size_t count) {
    const uint8_t *data = (const uint8_t *)bytes;
    for (size_t index = 0u; index < count; ++index) {
        hash ^= data[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static float sdf_half_to_float(uint16_t half) {
    const uint32_t sign = (uint32_t)(half >> 15u) & 1u;
    const uint32_t exponent = (uint32_t)(half >> 10u) & 31u;
    const uint32_t mantissa = (uint32_t)half & 1023u;
    float value;
    if (exponent == 0u) {
        value = ldexpf((float)mantissa, -24);
    } else if (exponent == 31u) {
        value = mantissa != 0u ? NAN : INFINITY;
    } else {
        value = ldexpf((float)(mantissa | 1024u), (int)exponent - 25);
    }
    return sign != 0u ? -value : value;
}

/* The largest half not above value (finite, within +-65504). */
static uint16_t sdf_half_round_down(float value) {
    if (!(value == value) || value >= SDF_MASK_SENTINEL_VALUE) {
        return (uint16_t)SDF_MASK_SENTINEL;
    }
    if (value <= -SDF_MASK_SENTINEL_VALUE) {
        return (uint16_t)0xFBFFu;
    }
    const bool negative = value < 0.0f;
    const float magnitude = negative ? -value : value;
    uint16_t bits;
    if (magnitude < ldexpf(1.0f, -14)) {
        bits = (uint16_t)floorf(magnitude * 16777216.0f);
    } else {
        int exponent2 = 0;
        const float fraction = frexpf(magnitude, &exponent2);   /* [0.5, 1) */
        const int exponent = exponent2 - 1 + 15;
        const uint32_t mantissa = (uint32_t)floorf((fraction * 2.0f - 1.0f) * 1024.0f);
        bits = (uint16_t)(((uint32_t)exponent << 10u) | (mantissa & 1023u));
    }
    if (negative) {
        /* Truncation rounded the magnitude down, so the value up: one step further from zero. */
        if (sdf_half_to_float(bits) != magnitude) {
            bits = (uint16_t)(bits + 1u);
        }
        bits = (uint16_t)(bits | 0x8000u);
    }
    return bits;
}

/* ------------------------------------------------------------------------------------------------ shapes */

static void sdf_rotation_matrix(const float quaternion[4], float matrix[9]) {
    float x = quaternion[0];
    float y = quaternion[1];
    float z = quaternion[2];
    float w = quaternion[3];
    const float length = sqrtf(x * x + y * y + z * z + w * w);
    if (length <= 0.0f) {
        x = 0.0f;
        y = 0.0f;
        z = 0.0f;
        w = 1.0f;
    } else {
        x /= length;
        y /= length;
        z /= length;
        w /= length;
    }
    /* Local -> world, row-major. */
    matrix[0] = 1.0f - 2.0f * (y * y + z * z);
    matrix[1] = 2.0f * (x * y - z * w);
    matrix[2] = 2.0f * (x * z + y * w);
    matrix[3] = 2.0f * (x * y + z * w);
    matrix[4] = 1.0f - 2.0f * (x * x + z * z);
    matrix[5] = 2.0f * (y * z - x * w);
    matrix[6] = 2.0f * (x * z - y * w);
    matrix[7] = 2.0f * (y * z + x * w);
    matrix[8] = 1.0f - 2.0f * (x * x + y * y);
}

static float sdf_length3(float x, float y, float z) { return sqrtf(x * x + y * y + z * z); }
static float sdf_length2(float x, float y) { return sqrtf(x * x + y * y); }

static float sdf_shape_local(const SdfEdit *edit, float x, float y, float z) {
    switch (edit->shape) {
    case BTBH_GPU_SDF_SHAPE_SPHERE:
        return sdf_length3(x, y, z) - edit->size[0];
    case BTBH_GPU_SDF_SHAPE_ELLIPSOID: {
        const float k0 = sdf_length3(x / edit->size[0], y / edit->size[1], z / edit->size[2]);
        const float k1 = sdf_length3(
            x / (edit->size[0] * edit->size[0]),
            y / (edit->size[1] * edit->size[1]),
            z / (edit->size[2] * edit->size[2])
        );
        if (k1 < 1.0e-20f) {
            return -sdf_minf(edit->size[0], sdf_minf(edit->size[1], edit->size[2]));
        }
        return k0 * (k0 - 1.0f) / k1;
    }
    case BTBH_GPU_SDF_SHAPE_BOX: {
        const float r = edit->corner_radius;
        const float qx = fabsf(x) - (edit->size[0] - r);
        const float qy = fabsf(y) - (edit->size[1] - r);
        const float qz = fabsf(z) - (edit->size[2] - r);
        const float outside = sdf_length3(sdf_maxf(qx, 0.0f), sdf_maxf(qy, 0.0f), sdf_maxf(qz, 0.0f));
        const float inside = sdf_minf(sdf_maxf(qx, sdf_maxf(qy, qz)), 0.0f);
        return outside + inside - r;
    }
    case BTBH_GPU_SDF_SHAPE_CAPSULE: {
        const float half_length = edit->size[1];
        const float cy = y - sdf_clampf(y, -half_length, half_length);
        return sdf_length3(x, cy, z) - edit->size[0];
    }
    case BTBH_GPU_SDF_SHAPE_CYLINDER: {
        const float rb = edit->corner_radius;
        const float dx = sdf_length2(x, z) - edit->size[0] + rb;
        const float dy = fabsf(y) - edit->size[1] + rb;
        return sdf_minf(sdf_maxf(dx, dy), 0.0f) +
            sdf_length2(sdf_maxf(dx, 0.0f), sdf_maxf(dy, 0.0f)) - rb;
    }
    case BTBH_GPU_SDF_SHAPE_TORUS:
    default: {
        const float qx = sdf_length2(x, z) - edit->size[0];
        return sdf_length2(qx, y) - edit->size[1];
    }
    }
}

static float sdf_shape_distance(const SdfEdit *edit, float x, float y, float z) {
    const float px = x - edit->position[0];
    const float py = y - edit->position[1];
    const float pz = z - edit->position[2];
    const float *m = edit->inverse_rotation;
    return sdf_shape_local(
        edit,
        m[0] * px + m[1] * py + m[2] * pz,
        m[3] * px + m[4] * py + m[5] * pz,
        m[6] * px + m[7] * py + m[8] * pz
    );
}

static float sdf_smin(float a, float b, float k) {
    const float lower = a < b ? a : b;
    if (k <= 0.0f) {
        return lower;
    }
    float h = k - fabsf(a - b);
    if (h <= 0.0f) {
        return lower;
    }
    h /= k;
    return lower - h * h * k * 0.25f;
}

static float sdf_smax(float a, float b, float k) { return -sdf_smin(-a, -b, k); }

static void sdf_apply_edit(const SdfEdit *edit, float s, float *distance, uint8_t *material) {
    const float d = *distance;
    switch (edit->operation) {
    case BTBH_GPU_SDF_OPERATION_ADD: {
        const float next = sdf_smin(d, s, edit->smoothing);
        if (s < d) {
            *material = edit->material;
        }
        *distance = next;
        break;
    }
    case BTBH_GPU_SDF_OPERATION_SUBTRACT: {
        const float next = sdf_smax(d, -s, edit->smoothing);
        if (edit->material_band > 0.0f && -s > d && next >= -edit->material_band) {
            *material = edit->material;
        }
        *distance = next;
        break;
    }
    case BTBH_GPU_SDF_OPERATION_INTERSECT: {
        const float next = sdf_smax(d, s, edit->smoothing);
        if (s > d) {
            *material = edit->material;
        }
        *distance = next;
        break;
    }
    case BTBH_GPU_SDF_OPERATION_PAINT:
    default:
        if (s <= 0.0f && (edit->material_band <= 0.0f || d >= -edit->material_band)) {
            *material = edit->material;
        }
        break;
    }
}

static bool sdf_point_in_reach(const SdfEdit *edit, float x, float y, float z) {
    return x >= edit->reach_min[0] && x <= edit->reach_max[0] &&
        y >= edit->reach_min[1] && y <= edit->reach_max[1] &&
        z >= edit->reach_min[2] && z <= edit->reach_max[2];
}

/* ------------------------------------------------------------------------------------------------ lattice */

static uint32_t sdf_chunk_index(const BTBHGPUSdfTerrain *t, int32_t cx, int32_t cy, int32_t cz) {
    return ((uint32_t)cz * (uint32_t)t->chunks[1] + (uint32_t)cy) * (uint32_t)t->chunks[0] + (uint32_t)cx;
}

static void sdf_chunk_coords(const BTBHGPUSdfTerrain *t, uint32_t index, int32_t coords[3]) {
    coords[0] = (int32_t)(index % (uint32_t)t->chunks[0]);
    coords[1] = (int32_t)((index / (uint32_t)t->chunks[0]) % (uint32_t)t->chunks[1]);
    coords[2] = (int32_t)(index / ((uint32_t)t->chunks[0] * (uint32_t)t->chunks[1]));
}

/* The chunk coordinate of a world coordinate along an axis, clamped to the lattice: monotone in value. */
static int32_t sdf_chunk_coord(const BTBHGPUSdfTerrain *t, int axis, float value) {
    float cell = floorf((value - t->lattice_min[axis]) / t->cell);
    if (!(cell >= 0.0f)) {
        cell = 0.0f;
    }
    if (cell > (float)t->cells[axis]) {
        cell = (float)t->cells[axis];
    }
    int32_t chunk = (int32_t)cell / t->chunk_cells;
    if (chunk >= t->chunks[axis]) {
        chunk = t->chunks[axis] - 1;
    }
    return chunk;
}

static bool sdf_point_inside_lattice(const BTBHGPUSdfTerrain *t, float x, float y, float z) {
    return x >= t->lattice_min[0] && x <= t->lattice_max[0] &&
        y >= t->lattice_min[1] && y <= t->lattice_max[1] &&
        z >= t->lattice_min[2] && z <= t->lattice_max[2];
}

static uint32_t sdf_point_chunk(const BTBHGPUSdfTerrain *t, float x, float y, float z) {
    if (!sdf_point_inside_lattice(t, x, y, z)) {
        return UINT32_MAX;
    }
    return sdf_chunk_index(t, sdf_chunk_coord(t, 0, x), sdf_chunk_coord(t, 1, y), sdf_chunk_coord(t, 2, z));
}

static uint32_t sdf_slot_index(const BTBHGPUSdfTerrain *t, int32_t bx, int32_t by, int32_t bz) {
    const int32_t b = t->bricks_per_chunk;
    return (uint32_t)((bz * b + by) * b + bx);
}

static float sdf_lattice_coord(const BTBHGPUSdfTerrain *t, int axis, int32_t sample) {
    return t->lattice_min[axis] + (float)sample * t->cell;
}

static bool sdf_is_tag(uintptr_t slot) { return slot != 0u && slot < SDF_TAG_LIMIT; }
static uintptr_t sdf_make_tag(bool inside, uint8_t material) {
    return (uintptr_t)(1u + (inside ? 1u : 0u) + 2u * (uint32_t)material);
}
static bool sdf_tag_inside(uintptr_t tag) { return ((tag - 1u) & 1u) != 0u; }
static uint8_t sdf_tag_material(uintptr_t tag) { return (uint8_t)((tag - 1u) >> 1u); }

static int8_t sdf_quantize(const BTBHGPUSdfTerrain *t, float distance) {
    const float scaled = sdf_clampf(distance / t->clamp_distance, -1.0f, 1.0f) * 127.0f;
    return (int8_t)floorf(scaled + 0.5f);
}

static float sdf_dequantize(const BTBHGPUSdfTerrain *t, int8_t q) {
    return (float)q * t->clamp_distance / 127.0f;
}

/* Reads one sample of the live bricks by its global lattice index; false where no brick slot holds it (the base,
 * unknown here). */
static bool sdf_read_live_sample(
    const BTBHGPUSdfTerrain *t,
    int32_t gx,
    int32_t gy,
    int32_t gz,
    int8_t *q,
    uint8_t *material,
    uint8_t *differs
) {
    if (gx < 0 || gy < 0 || gz < 0 || gx > t->cells[0] || gy > t->cells[1] || gz > t->cells[2]) {
        return false;
    }
    const int32_t n = t->chunk_cells;
    const SdfChunk *chunk = &t->chunk_array[sdf_chunk_index(t, gx / n, gy / n, gz / n)];
    if (chunk->bricks == NULL) {
        return false;
    }
    const int32_t lx = gx % n;
    const int32_t ly = gy % n;
    const int32_t lz = gz % n;
    const uintptr_t slot = chunk->bricks[sdf_slot_index(t, lx / SDF_BRICK, ly / SDF_BRICK, lz / SDF_BRICK)];
    if (slot == 0u) {
        return false;
    }
    if (sdf_is_tag(slot)) {
        *q = sdf_tag_inside(slot) ? (int8_t)-127 : (int8_t)127;
        *material = sdf_tag_material(slot);
        *differs = 0u;
        return true;
    }
    const SdfBrick *brick = (const SdfBrick *)slot;
    const uint32_t s = (uint32_t)(((lz % SDF_BRICK) * SDF_BRICK + (ly % SDF_BRICK)) * SDF_BRICK + (lx % SDF_BRICK));
    *q = brick->distance[s];
    *material = brick->material[s];
    *differs = (uint8_t)((brick->differs[s >> 6u] >> (s & 63u)) & 1u);
    return true;
}

/* ------------------------------------------------------------------------------------------------ base */

static void sdf_heightfield_at(const BTBHGPUSdfTerrain *t, float x, float z, float *height, float *gx, float *gz) {
    const uint32_t width = t->desc.base_width;
    const uint32_t depth = t->desc.base_depth;
    const float base_cell = t->desc.base_cell;
    float u = (x - t->desc.bounds_min[0]) / base_cell;
    float v = (z - t->desc.bounds_min[2]) / base_cell;
    u = sdf_clampf(u, 0.0f, (float)width);
    v = sdf_clampf(v, 0.0f, (float)depth);
    uint32_t i = (uint32_t)u;
    uint32_t k = (uint32_t)v;
    if (i >= width) {
        i = width - 1u;
    }
    if (k >= depth) {
        k = depth - 1u;
    }
    const float fu = u - (float)i;
    const float fv = v - (float)k;
    const uint32_t stride = width + 1u;
    const float h00 = t->heights[k * stride + i];
    const float h10 = t->heights[k * stride + i + 1u];
    const float h01 = t->heights[(k + 1u) * stride + i];
    const float h11 = t->heights[(k + 1u) * stride + i + 1u];
    const float h0 = h00 + (h10 - h00) * fu;
    const float h1 = h01 + (h11 - h01) * fu;
    *height = h0 + (h1 - h0) * fv;
    if (gx != NULL) {
        *gx = ((h10 - h00) * (1.0f - fv) + (h11 - h01) * fv) / base_cell;
    }
    if (gz != NULL) {
        *gz = (h1 - h0) / base_cell;
    }
}

static uint8_t sdf_heightfield_material(const BTBHGPUSdfTerrain *t, float depth_under_surface) {
    return depth_under_surface <= t->desc.base_subsurface_depth
        ? t->desc.base_surface_material
        : t->desc.base_subsurface_material;
}

/* The original base (never the frozen one) at count points. */
static void sdf_original_base(
    const BTBHGPUSdfTerrain *t,
    const float *points,
    uint32_t count,
    float *distances,
    uint8_t *materials
) {
    if (count == 0u) {
        return;
    }
    if (t->heights != NULL) {
        for (uint32_t index = 0u; index < count; ++index) {
            float height = 0.0f;
            float gx = 0.0f;
            float gz = 0.0f;
            const float *p = &points[index * 3u];
            sdf_heightfield_at(t, p[0], p[2], &height, &gx, &gz);
            distances[index] = (p[1] - height) / sqrtf(1.0f + gx * gx + gz * gz);
            materials[index] = sdf_heightfield_material(t, height - p[1]);
        }
        return;
    }
    t->desc.base_function(t->desc.base_user, points, count, distances, materials);
    for (uint32_t index = 0u; index < count; ++index) {
        if (!sdf_finite(distances[index])) {
            distances[index] = distances[index] < 0.0f ? -FLT_MAX : FLT_MAX;
        }
        if (materials[index] > 7u) {
            materials[index] = 7u;
        }
    }
}

static float sdf_original_base_height(const BTBHGPUSdfTerrain *t, float x, float z, float from_y);

/* A frozen chunk's base at a point inside it: trilinear in its frozen brick, or the original base where it has
 * none (returns false then). */
static bool sdf_frozen_base_at(
    const BTBHGPUSdfTerrain *t,
    const SdfChunk *chunk,
    const int32_t chunk_coords[3],
    float x,
    float y,
    float z,
    float *distance,
    uint8_t *material
) {
    const float p[3] = { x, y, z };
    int32_t local_cell[3];
    float fraction[3];
    for (int axis = 0; axis < 3; ++axis) {
        const float coordinate = (p[axis] - t->lattice_min[axis]) / t->cell;
        int32_t cell = (int32_t)floorf(coordinate);
        const int32_t c0 = chunk_coords[axis] * t->chunk_cells;
        cell = sdf_maxi(c0, sdf_mini(cell, c0 + t->chunk_cells - 1));
        fraction[axis] = sdf_clampf(coordinate - (float)cell, 0.0f, 1.0f);
        local_cell[axis] = cell - c0;
    }
    const uintptr_t slot = chunk->frozen[sdf_slot_index(
        t,
        local_cell[0] / SDF_BRICK,
        local_cell[1] / SDF_BRICK,
        local_cell[2] / SDF_BRICK
    )];
    if (slot == 0u) {
        return false;
    }
    if (sdf_is_tag(slot)) {
        *distance = sdf_tag_inside(slot) ? -t->clamp_distance : t->clamp_distance;
        *material = sdf_tag_material(slot);
        return true;
    }
    const SdfFrozenBrick *brick = (const SdfFrozenBrick *)slot;
    const int32_t bx = local_cell[0] % SDF_BRICK;
    const int32_t by = local_cell[1] % SDF_BRICK;
    const int32_t bz = local_cell[2] % SDF_BRICK;
    float value = 0.0f;
    for (int corner = 0; corner < 8; ++corner) {
        const int32_t ox = corner & 1;
        const int32_t oy = (corner >> 1) & 1;
        const int32_t oz = (corner >> 2) & 1;
        const float weight =
            (ox != 0 ? fraction[0] : 1.0f - fraction[0]) *
            (oy != 0 ? fraction[1] : 1.0f - fraction[1]) *
            (oz != 0 ? fraction[2] : 1.0f - fraction[2]);
        const int32_t s = ((bz + oz) * SDF_FROZEN_EDGE + (by + oy)) * SDF_FROZEN_EDGE + (bx + ox);
        value += weight * sdf_dequantize(t, brick->distance[s]);
    }
    const int32_t nx = bx + (fraction[0] >= 0.5f ? 1 : 0);
    const int32_t ny = by + (fraction[1] >= 0.5f ? 1 : 0);
    const int32_t nz = bz + (fraction[2] >= 0.5f ? 1 : 0);
    *distance = value;
    *material = brick->material[(nz * SDF_FROZEN_EDGE + ny) * SDF_FROZEN_EDGE + nx];
    return true;
}

/* The field at count arbitrary points (queries, mesh vertices). */
static void sdf_field_points(
    const BTBHGPUSdfTerrain *t,
    const float *points,
    uint32_t count,
    float *distances,
    uint8_t *materials
) {
    sdf_original_base(t, points, count, distances, materials);
    for (uint32_t index = 0u; index < count; ++index) {
        const float *p = &points[index * 3u];
        const uint32_t chunk_index = sdf_point_chunk(t, p[0], p[1], p[2]);
        if (chunk_index == UINT32_MAX) {
            continue;
        }
        const SdfChunk *chunk = &t->chunk_array[chunk_index];
        if (chunk->is_frozen) {
            int32_t coords[3];
            sdf_chunk_coords(t, chunk_index, coords);
            float frozen_distance = 0.0f;
            uint8_t frozen_material = 0u;
            if (sdf_frozen_base_at(t, chunk, coords, p[0], p[1], p[2], &frozen_distance, &frozen_material)) {
                distances[index] = frozen_distance;
                materials[index] = frozen_material;
            }
        }
        float d = distances[index];
        uint8_t m = materials[index];
        for (uint32_t e = 0u; e < chunk->edit_count; ++e) {
            const SdfEdit *edit = &t->edits[chunk->edits[e]];
            if (sdf_point_in_reach(edit, p[0], p[1], p[2])) {
                sdf_apply_edit(edit, sdf_shape_distance(edit, p[0], p[1], p[2]), &d, &m);
            }
        }
        distances[index] = d;
        materials[index] = m;
    }
}

static float sdf_field_at(const BTBHGPUSdfTerrain *t, float x, float y, float z) {
    const float point[3] = { x, y, z };
    float distance = 0.0f;
    uint8_t material = 0u;
    sdf_field_points(t, point, 1u, &distance, &material);
    return distance;
}

/* ------------------------------------------------------------------------------------------------ bricks */

static SdfBrick *sdf_brick_allocate(BTBHGPUSdfTerrain *t) {
    if (t->free_bricks == NULL) {
        const uint32_t per_slab = 64u;
        SdfBrick *slab = (SdfBrick *)calloc(per_slab, sizeof(SdfBrick));
        if (slab == NULL) {
            return NULL;
        }
        if (t->slab_count == t->slab_capacity) {
            const uint32_t capacity = t->slab_capacity < 8u ? 8u : t->slab_capacity * 2u;
            SdfBrick **slabs = (SdfBrick **)realloc(t->slabs, sizeof(SdfBrick *) * capacity);
            if (slabs == NULL) {
                free(slab);
                return NULL;
            }
            t->slabs = slabs;
            t->slab_capacity = capacity;
        }
        t->slabs[t->slab_count++] = slab;
        for (uint32_t index = per_slab; index > 0u; --index) {
            slab[index - 1u].next_free = t->free_bricks;
            t->free_bricks = &slab[index - 1u];
        }
    }
    SdfBrick *brick = t->free_bricks;
    t->free_bricks = brick->next_free;
    brick->next_free = NULL;
    t->brick_total += 1u;
    return brick;
}

static void sdf_brick_release(BTBHGPUSdfTerrain *t, SdfBrick *brick) {
    brick->next_free = t->free_bricks;
    t->free_bricks = brick;
    t->brick_total -= 1u;
}

static void sdf_slot_release(BTBHGPUSdfTerrain *t, uintptr_t slot) {
    if (slot != 0u && !sdf_is_tag(slot)) {
        sdf_brick_release(t, (SdfBrick *)slot);
    }
}

static void sdf_frozen_slot_release(BTBHGPUSdfTerrain *t, uintptr_t slot) {
    if (slot != 0u && !sdf_is_tag(slot)) {
        free((void *)slot);
        t->frozen_brick_total -= 1u;
    }
}

static bool sdf_chunk_ensure_bricks(BTBHGPUSdfTerrain *t, SdfChunk *chunk) {
    if (chunk->bricks != NULL) {
        return true;
    }
    chunk->bricks = (uintptr_t *)calloc(t->slots_per_chunk, sizeof(uintptr_t));
    chunk->stale_bits = (uint64_t *)calloc((t->slots_per_chunk + 63u) / 64u, sizeof(uint64_t));
    if (chunk->bricks == NULL || chunk->stale_bits == NULL) {
        free(chunk->bricks);
        free(chunk->stale_bits);
        chunk->bricks = NULL;
        chunk->stale_bits = NULL;
        return false;
    }
    return true;
}

/* Bricks that hold no sample at all any more (and no frozen data): their arrays go too. */
static void sdf_chunk_trim(BTBHGPUSdfTerrain *t, SdfChunk *chunk) {
    if (chunk->bricks == NULL || chunk->stale_count != 0u || chunk->frozen != NULL) {
        return;
    }
    for (uint32_t slot = 0u; slot < t->slots_per_chunk; ++slot) {
        if (chunk->bricks[slot] != 0u) {
            return;
        }
    }
    free(chunk->bricks);
    free(chunk->stale_bits);
    chunk->bricks = NULL;
    chunk->stale_bits = NULL;
}

static void sdf_busy_add(BTBHGPUSdfTerrain *t, uint32_t chunk_index) {
    SdfChunk *chunk = &t->chunk_array[chunk_index];
    if (!chunk->in_busy_list && sdf_list_push(&t->busy, chunk_index)) {
        chunk->in_busy_list = 1u;
    }
}

static void sdf_mark_needs_mesh(BTBHGPUSdfTerrain *t, uint32_t chunk_index) {
    SdfChunk *chunk = &t->chunk_array[chunk_index];
    chunk->needs_mesh = 1u;
    if (!chunk->in_dirty_list && sdf_list_push(&t->dirty, chunk_index)) {
        chunk->in_dirty_list = 1u;
    }
    sdf_busy_add(t, chunk_index);
}

static bool sdf_stale_push(BTBHGPUSdfTerrain *t, uint64_t entry) {
    if (t->stale_tail == t->stale_capacity) {
        if (t->stale_head > 0u) {
            memmove(t->stale, t->stale + t->stale_head, sizeof(uint64_t) * (t->stale_tail - t->stale_head));
            t->stale_tail -= t->stale_head;
            t->stale_head = 0u;
        }
        if (t->stale_tail == t->stale_capacity) {
            const uint32_t capacity = t->stale_capacity < 256u ? 256u : t->stale_capacity * 2u;
            uint64_t *stale = (uint64_t *)realloc(t->stale, sizeof(uint64_t) * capacity);
            if (stale == NULL) {
                return false;
            }
            t->stale = stale;
            t->stale_capacity = capacity;
        }
    }
    t->stale[t->stale_tail++] = entry;
    return true;
}

static void sdf_mark_brick_stale(BTBHGPUSdfTerrain *t, uint32_t chunk_index, uint32_t slot) {
    SdfChunk *chunk = &t->chunk_array[chunk_index];
    if (!sdf_chunk_ensure_bricks(t, chunk)) {
        return;
    }
    uint64_t *word = &chunk->stale_bits[slot >> 6u];
    const uint64_t bit = UINT64_C(1) << (slot & 63u);
    if ((*word & bit) != 0u) {
        return;
    }
    if (!sdf_stale_push(t, ((uint64_t)chunk_index << 32u) | (uint64_t)slot)) {
        return;
    }
    *word |= bit;
    chunk->stale_count += 1u;
    t->stale_total += 1u;
    sdf_busy_add(t, chunk_index);
}

/* Every brick whose samples may lie within [lo, hi] (world) goes stale. */
static void sdf_mark_box_stale(BTBHGPUSdfTerrain *t, const float lo[3], const float hi[3]) {
    int32_t brick_lo[3];
    int32_t brick_hi[3];
    for (int axis = 0; axis < 3; ++axis) {
        if (hi[axis] < t->lattice_min[axis] || lo[axis] > t->lattice_max[axis]) {
            return;
        }
        int32_t s0 = (int32_t)floorf((lo[axis] - t->lattice_min[axis]) / t->cell) - 1;
        int32_t s1 = (int32_t)ceilf((hi[axis] - t->lattice_min[axis]) / t->cell) + 1;
        s0 = sdf_maxi(0, s0);
        s1 = sdf_mini(t->cells[axis], s1);
        if (s1 < s0) {
            return;
        }
        brick_lo[axis] = s0 / SDF_BRICK;
        brick_hi[axis] = s1 / SDF_BRICK;
    }
    const int32_t b = t->bricks_per_chunk;
    for (int32_t gz = brick_lo[2]; gz <= brick_hi[2]; ++gz) {
        for (int32_t gy = brick_lo[1]; gy <= brick_hi[1]; ++gy) {
            for (int32_t gx = brick_lo[0]; gx <= brick_hi[0]; ++gx) {
                const uint32_t chunk_index = sdf_chunk_index(t, gx / b, gy / b, gz / b);
                sdf_mark_brick_stale(t, chunk_index, sdf_slot_index(t, gx % b, gy % b, gz % b));
            }
        }
    }
}

/* The far test: an interval of the field over the brick (from its centre, the shapes' Lipschitz bounds and the
 * monotone operations). True when every sample is clamped to the same sign as the base, which then needs no
 * evaluation (a tag stands for the brick). */
typedef struct SdfBrickEval {
    uint32_t chunk_index;
    uint32_t slot;
    uintptr_t result;     /* 0: the base (no brick), a tag, or 1u << 6 for `brick` */
    SdfBrick brick;
} SdfBrickEval;

#define SDF_RESULT_BRICK ((uintptr_t)SDF_TAG_LIMIT)

static bool sdf_base_interval(
    const BTBHGPUSdfTerrain *t,
    const float lo[3],
    const float hi[3],
    const float centre[3],
    float half_diagonal,
    float *base_lo,
    float *base_hi
) {
    if (t->heights != NULL) {
        const float base_cell = t->desc.base_cell;
        const int32_t width = (int32_t)t->desc.base_width;
        const int32_t depth = (int32_t)t->desc.base_depth;
        int32_t i0 = (int32_t)floorf((lo[0] - t->desc.bounds_min[0]) / base_cell);
        int32_t i1 = (int32_t)ceilf((hi[0] - t->desc.bounds_min[0]) / base_cell);
        int32_t k0 = (int32_t)floorf((lo[2] - t->desc.bounds_min[2]) / base_cell);
        int32_t k1 = (int32_t)ceilf((hi[2] - t->desc.bounds_min[2]) / base_cell);
        i0 = sdf_maxi(0, sdf_mini(i0, width));
        i1 = sdf_maxi(0, sdf_mini(i1, width));
        k0 = sdf_maxi(0, sdf_mini(k0, depth));
        k1 = sdf_maxi(0, sdf_mini(k1, depth));
        if (i1 == i0) {
            if (i1 < width) {
                i1 += 1;
            } else {
                i0 -= 1;
            }
        }
        if (k1 == k0) {
            if (k1 < depth) {
                k1 += 1;
            } else {
                k0 -= 1;
            }
        }
        const uint32_t stride = (uint32_t)width + 1u;
        float h_min = FLT_MAX;
        float h_max = -FLT_MAX;
        float slope = 0.0f;
        for (int32_t k = k0; k <= k1; ++k) {
            for (int32_t i = i0; i <= i1; ++i) {
                const float h = t->heights[(uint32_t)k * stride + (uint32_t)i];
                h_min = sdf_minf(h_min, h);
                h_max = sdf_maxf(h_max, h);
                if (i < i1) {
                    slope = sdf_maxf(slope, fabsf(t->heights[(uint32_t)k * stride + (uint32_t)i + 1u] - h));
                }
                if (k < k1) {
                    slope = sdf_maxf(slope, fabsf(t->heights[(uint32_t)(k + 1) * stride + (uint32_t)i] - h));
                }
            }
        }
        slope /= base_cell;
        const float normaliser = sqrtf(1.0f + 2.0f * slope * slope);
        if (lo[1] - h_max > 0.0f) {
            *base_lo = (lo[1] - h_max) / normaliser;
            *base_hi = hi[1] - h_min;
            return true;
        }
        if (hi[1] - h_min < 0.0f) {
            *base_lo = lo[1] - h_max;
            *base_hi = (hi[1] - h_min) / normaliser;
            return true;
        }
        return false;
    }
    float distance = 0.0f;
    uint8_t material = 0u;
    sdf_original_base(t, centre, 1u, &distance, &material);
    *base_lo = distance - 1.25f * half_diagonal;
    *base_hi = distance + 1.25f * half_diagonal;
    return true;
}

static bool sdf_brick_is_far(
    const BTBHGPUSdfTerrain *t,
    const uint32_t *edits,
    uint32_t edit_count,
    const uint8_t *fully_inside,
    const float lo[3],
    const float hi[3],
    bool *inside
) {
    const float centre[3] = {
        (lo[0] + hi[0]) * 0.5f,
        (lo[1] + hi[1]) * 0.5f,
        (lo[2] + hi[2]) * 0.5f
    };
    const float half_diagonal = 0.5f * sdf_length3(hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]);
    float base_lo = 0.0f;
    float base_hi = 0.0f;
    if (!sdf_base_interval(t, lo, hi, centre, half_diagonal, &base_lo, &base_hi)) {
        return false;
    }
    const float clamp = t->clamp_distance;
    const bool base_outside = base_lo >= clamp;
    const bool base_inside = base_hi <= -clamp;
    if (!base_outside && !base_inside) {
        return false;
    }
    float field_lo = base_lo;
    float field_hi = base_hi;
    for (uint32_t index = 0u; index < edit_count; ++index) {
        const SdfEdit *edit = &t->edits[edits[index]];
        if (edit->operation == BTBH_GPU_SDF_OPERATION_PAINT) {
            continue;
        }
        const float s = sdf_shape_distance(edit, centre[0], centre[1], centre[2]);
        const float spread = 1.25f * edit->lipschitz * half_diagonal;
        const float s_lo = s - spread;
        const float s_hi = s + spread;
        const float k = edit->smoothing;
        float next_lo;
        float next_hi;
        if (edit->operation == BTBH_GPU_SDF_OPERATION_ADD) {
            next_lo = sdf_smin(field_lo, s_lo, k);
            next_hi = sdf_smin(field_hi, s_hi, k);
        } else if (edit->operation == BTBH_GPU_SDF_OPERATION_SUBTRACT) {
            next_lo = sdf_smax(field_lo, -s_hi, k);
            next_hi = sdf_smax(field_hi, -s_lo, k);
        } else {
            next_lo = sdf_smax(field_lo, s_lo, k);
            next_hi = sdf_smax(field_hi, s_hi, k);
        }
        if (fully_inside[index]) {
            field_lo = next_lo;
            field_hi = next_hi;
        } else {
            field_lo = sdf_minf(field_lo, next_lo);
            field_hi = sdf_maxf(field_hi, next_hi);
        }
    }
    if (base_outside && field_lo >= clamp) {
        *inside = false;
        return true;
    }
    if (base_inside && field_hi <= -clamp) {
        *inside = true;
        return true;
    }
    return false;
}

static void sdf_brick_origin(
    const BTBHGPUSdfTerrain *t,
    uint32_t chunk_index,
    uint32_t slot,
    int32_t origin[3]
) {
    int32_t coords[3];
    sdf_chunk_coords(t, chunk_index, coords);
    const int32_t b = t->bricks_per_chunk;
    const int32_t local[3] = {
        (int32_t)slot % b,
        ((int32_t)slot / b) % b,
        (int32_t)slot / (b * b)
    };
    for (int axis = 0; axis < 3; ++axis) {
        origin[axis] = coords[axis] * t->chunk_cells + local[axis] * SDF_BRICK;
    }
}

/* Evaluates one brick into work->brick (or a tag, or the base). Touches nothing but `work` and the base
 * function, so it can run on any thread. */
static void sdf_brick_evaluate(const BTBHGPUSdfTerrain *t, SdfBrickEval *work) {
    const SdfChunk *chunk = &t->chunk_array[work->chunk_index];
    int32_t origin[3];
    sdf_brick_origin(t, work->chunk_index, work->slot, origin);
    float lo[3];
    float hi[3];
    for (int axis = 0; axis < 3; ++axis) {
        lo[axis] = sdf_lattice_coord(t, axis, origin[axis]);
        hi[axis] = sdf_lattice_coord(t, axis, origin[axis] + SDF_BRICK - 1);
    }

    uint32_t local_edits[256];
    uint8_t local_inside[256];
    uint32_t *edits = local_edits;
    uint8_t *inside_flags = local_inside;
    if (chunk->edit_count > 256u) {
        edits = (uint32_t *)malloc(sizeof(uint32_t) * chunk->edit_count);
        inside_flags = (uint8_t *)malloc(chunk->edit_count);
        if (edits == NULL || inside_flags == NULL) {
            free(edits);
            free(inside_flags);
            edits = local_edits;
            inside_flags = local_inside;
        }
    }
    const uint32_t edit_limit = edits == local_edits ? sdf_mini((int32_t)chunk->edit_count, 256) : chunk->edit_count;
    uint32_t edit_count = 0u;
    for (uint32_t index = 0u; index < edit_limit; ++index) {
        const SdfEdit *edit = &t->edits[chunk->edits[index]];
        bool touches = true;
        bool covers = true;
        for (int axis = 0; axis < 3; ++axis) {
            if (edit->reach_max[axis] < lo[axis] || edit->reach_min[axis] > hi[axis]) {
                touches = false;
            }
            if (edit->reach_min[axis] > lo[axis] || edit->reach_max[axis] < hi[axis]) {
                covers = false;
            }
        }
        if (touches) {
            edits[edit_count] = chunk->edits[index];
            inside_flags[edit_count] = covers ? 1u : 0u;
            edit_count += 1u;
        }
    }

    const uintptr_t frozen_slot = chunk->is_frozen && chunk->frozen != NULL ? chunk->frozen[work->slot] : 0u;
    if (edit_count == 0u && frozen_slot == 0u) {
        work->result = 0u;
        goto done;
    }
    if (frozen_slot == 0u) {
        bool inside = false;
        if (sdf_brick_is_far(t, edits, edit_count, inside_flags, lo, hi, &inside)) {
            uint8_t material = 0u;
            if (inside) {
                const float centre[3] = {
                    (lo[0] + hi[0]) * 0.5f,
                    (lo[1] + hi[1]) * 0.5f,
                    (lo[2] + hi[2]) * 0.5f
                };
                float d = 0.0f;
                sdf_original_base(t, centre, 1u, &d, &material);
                for (uint32_t index = 0u; index < edit_count; ++index) {
                    const SdfEdit *edit = &t->edits[edits[index]];
                    if (sdf_point_in_reach(edit, centre[0], centre[1], centre[2])) {
                        sdf_apply_edit(edit, sdf_shape_distance(edit, centre[0], centre[1], centre[2]), &d, &material);
                    }
                }
            }
            work->result = sdf_make_tag(inside, material);
            goto done;
        }
    }
    {
        float points[SDF_BRICK_SAMPLES * 3];
        float distances[SDF_BRICK_SAMPLES];
        uint8_t materials[SDF_BRICK_SAMPLES];
        int8_t base_q[SDF_BRICK_SAMPLES];
        uint8_t base_m[SDF_BRICK_SAMPLES];
        uint32_t s = 0u;
        for (int32_t z = 0; z < SDF_BRICK; ++z) {
            for (int32_t y = 0; y < SDF_BRICK; ++y) {
                for (int32_t x = 0; x < SDF_BRICK; ++x) {
                    points[s * 3u + 0u] = sdf_lattice_coord(t, 0, origin[0] + x);
                    points[s * 3u + 1u] = sdf_lattice_coord(t, 1, origin[1] + y);
                    points[s * 3u + 2u] = sdf_lattice_coord(t, 2, origin[2] + z);
                    s += 1u;
                }
            }
        }
        const SdfFrozenBrick *frozen = NULL;
        if (frozen_slot != 0u && !sdf_is_tag(frozen_slot)) {
            frozen = (const SdfFrozenBrick *)frozen_slot;
            s = 0u;
            for (int32_t z = 0; z < SDF_BRICK; ++z) {
                for (int32_t y = 0; y < SDF_BRICK; ++y) {
                    for (int32_t x = 0; x < SDF_BRICK; ++x) {
                        const int32_t f = (z * SDF_FROZEN_EDGE + y) * SDF_FROZEN_EDGE + x;
                        distances[s] = sdf_dequantize(t, frozen->distance[f]);
                        materials[s] = frozen->material[f];
                        s += 1u;
                    }
                }
            }
        } else if (frozen_slot != 0u) {
            for (s = 0u; s < SDF_BRICK_SAMPLES; ++s) {
                distances[s] = sdf_tag_inside(frozen_slot) ? -t->clamp_distance : t->clamp_distance;
                materials[s] = sdf_tag_material(frozen_slot);
            }
        } else {
            sdf_original_base(t, points, SDF_BRICK_SAMPLES, distances, materials);
        }
        for (s = 0u; s < SDF_BRICK_SAMPLES; ++s) {
            base_q[s] = sdf_quantize(t, distances[s]);
            base_m[s] = materials[s];
        }
        for (uint32_t index = 0u; index < edit_count; ++index) {
            const SdfEdit *edit = &t->edits[edits[index]];
            const bool covers = inside_flags[index] != 0u;
            for (s = 0u; s < SDF_BRICK_SAMPLES; ++s) {
                const float *p = &points[s * 3u];
                if (covers || sdf_point_in_reach(edit, p[0], p[1], p[2])) {
                    sdf_apply_edit(edit, sdf_shape_distance(edit, p[0], p[1], p[2]), &distances[s], &materials[s]);
                }
            }
        }
        SdfBrick *brick = &work->brick;
        memset(brick->differs, 0, sizeof(brick->differs));
        brick->active = 0u;
        for (s = 0u; s < SDF_BRICK_SAMPLES; ++s) {
            const int8_t q = sdf_quantize(t, distances[s]);
            brick->distance[s] = q;
            brick->material[s] = materials[s];
            bool differs = q != base_q[s] || (q > -127 && q < 127 && materials[s] != base_m[s]);
            if (frozen != NULL && ((frozen->differs[s >> 6u] >> (s & 63u)) & 1u) != 0u) {
                differs = true;
            }
            if (differs) {
                brick->differs[s >> 6u] |= UINT64_C(1) << (s & 63u);
                brick->active = 1u;
            }
        }
        work->result = SDF_RESULT_BRICK;
    }
done:
    if (edits != local_edits) {
        free(edits);
        free(inside_flags);
    }
}

static bool sdf_brick_equal(const SdfBrick *a, const SdfBrick *b) {
    return memcmp(a->distance, b->distance, sizeof(a->distance)) == 0 &&
        memcmp(a->material, b->material, sizeof(a->material)) == 0 &&
        memcmp(a->differs, b->differs, sizeof(a->differs)) == 0;
}

/* Every chunk whose mesh reads the brick's samples (its own, and its neighbours' for a brick on the border). */
static void sdf_mark_brick_readers(BTBHGPUSdfTerrain *t, uint32_t chunk_index, uint32_t slot) {
    int32_t coords[3];
    sdf_chunk_coords(t, chunk_index, coords);
    const int32_t b = t->bricks_per_chunk;
    const int32_t local[3] = {
        (int32_t)slot % b,
        ((int32_t)slot / b) % b,
        (int32_t)slot / (b * b)
    };
    int32_t lo[3];
    int32_t hi[3];
    for (int axis = 0; axis < 3; ++axis) {
        lo[axis] = local[axis] == 0 ? sdf_maxi(coords[axis] - 1, 0) : coords[axis];
        hi[axis] = local[axis] == b - 1 ? sdf_mini(coords[axis] + 1, t->chunks[axis] - 1) : coords[axis];
    }
    for (int32_t z = lo[2]; z <= hi[2]; ++z) {
        for (int32_t y = lo[1]; y <= hi[1]; ++y) {
            for (int32_t x = lo[0]; x <= hi[0]; ++x) {
                sdf_mark_needs_mesh(t, sdf_chunk_index(t, x, y, z));
            }
        }
    }
}

/* Takes a brick's evaluation into the chunk (calling thread). */
static void sdf_brick_commit(BTBHGPUSdfTerrain *t, SdfBrickEval *work) {
    SdfChunk *chunk = &t->chunk_array[work->chunk_index];
    const uintptr_t old = chunk->bricks[work->slot];
    bool changed;
    if (work->result == SDF_RESULT_BRICK) {
        if (old != 0u && !sdf_is_tag(old)) {
            SdfBrick *brick = (SdfBrick *)old;
            changed = !sdf_brick_equal(brick, &work->brick);
            if (changed) {
                memcpy(brick->distance, work->brick.distance, sizeof(brick->distance));
                memcpy(brick->material, work->brick.material, sizeof(brick->material));
                memcpy(brick->differs, work->brick.differs, sizeof(brick->differs));
                brick->active = work->brick.active;
            }
        } else {
            SdfBrick *brick = sdf_brick_allocate(t);
            if (brick == NULL) {
                /* Out of memory: stays stale (and queued) for the next update. */
                if (sdf_stale_push(t, ((uint64_t)work->chunk_index << 32u) | (uint64_t)work->slot)) {
                    chunk->stale_bits[work->slot >> 6u] |= UINT64_C(1) << (work->slot & 63u);
                }
                return;
            }
            memcpy(brick->distance, work->brick.distance, sizeof(brick->distance));
            memcpy(brick->material, work->brick.material, sizeof(brick->material));
            memcpy(brick->differs, work->brick.differs, sizeof(brick->differs));
            brick->active = work->brick.active;
            chunk->bricks[work->slot] = (uintptr_t)brick;
            changed = true;
        }
    } else {
        changed = old != work->result;
        sdf_slot_release(t, old);
        chunk->bricks[work->slot] = work->result;
    }
    /* The stale bit was cleared when the brick was taken (so a second queue entry for it is skipped). */
    chunk->stale_count -= 1u;
    t->stale_total -= 1u;
    if (changed) {
        sdf_mark_brick_readers(t, work->chunk_index, work->slot);
    }
}

/* ------------------------------------------------------------------------------------------------ edits */

static bool sdf_edit_validate(const BTBHGPUSdfEdit *edit) {
    if (edit == NULL ||
        (uint32_t)edit->shape > (uint32_t)BTBH_GPU_SDF_SHAPE_TORUS ||
        (uint32_t)edit->operation > (uint32_t)BTBH_GPU_SDF_OPERATION_PAINT ||
        edit->material > 7u) {
        return false;
    }
    for (int index = 0; index < 3; ++index) {
        if (!sdf_finite(edit->position[index]) || !sdf_finite(edit->size[index])) {
            return false;
        }
    }
    float rotation_length = 0.0f;
    for (int index = 0; index < 4; ++index) {
        if (!sdf_finite(edit->rotation[index])) {
            return false;
        }
        rotation_length += edit->rotation[index] * edit->rotation[index];
    }
    const bool identity = edit->rotation[0] == 0.0f && edit->rotation[1] == 0.0f &&
        edit->rotation[2] == 0.0f && edit->rotation[3] == 0.0f;
    if (!identity && !(rotation_length > 1.0e-12f)) {
        return false;
    }
    if (!sdf_finite(edit->corner_radius) || edit->corner_radius < 0.0f ||
        !sdf_finite(edit->smoothing) || edit->smoothing < 0.0f ||
        !sdf_finite(edit->material_band) || edit->material_band < 0.0f) {
        return false;
    }
    const float *size = edit->size;
    switch (edit->shape) {
    case BTBH_GPU_SDF_SHAPE_SPHERE:
        return size[0] > 0.0f;
    case BTBH_GPU_SDF_SHAPE_ELLIPSOID:
    case BTBH_GPU_SDF_SHAPE_BOX:
        return size[0] > 0.0f && size[1] > 0.0f && size[2] > 0.0f;
    case BTBH_GPU_SDF_SHAPE_CAPSULE:
        return size[0] > 0.0f && size[1] >= 0.0f;
    case BTBH_GPU_SDF_SHAPE_CYLINDER:
    case BTBH_GPU_SDF_SHAPE_TORUS:
    default:
        return size[0] > 0.0f && size[1] > 0.0f;
    }
}

static void sdf_edit_prepare(const BTBHGPUSdfTerrain *t, const BTBHGPUSdfEdit *source, SdfEdit *edit) {
    edit->source = *source;
    edit->shape = (uint8_t)source->shape;
    edit->operation = (uint8_t)source->operation;
    edit->material = source->material;
    memcpy(edit->position, source->position, sizeof(edit->position));
    memcpy(edit->size, source->size, sizeof(edit->size));
    edit->smoothing = source->smoothing;
    edit->material_band = source->material_band;
    float rotation[9];
    sdf_rotation_matrix(source->rotation, rotation);
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            edit->inverse_rotation[row * 3 + column] = rotation[column * 3 + row];
        }
    }
    float extent[3];
    edit->lipschitz = 1.0f;
    edit->corner_radius = 0.0f;
    switch (source->shape) {
    case BTBH_GPU_SDF_SHAPE_SPHERE:
        extent[0] = extent[1] = extent[2] = source->size[0];
        break;
    case BTBH_GPU_SDF_SHAPE_ELLIPSOID: {
        extent[0] = source->size[0];
        extent[1] = source->size[1];
        extent[2] = source->size[2];
        const float largest = sdf_maxf(extent[0], sdf_maxf(extent[1], extent[2]));
        const float smallest = sdf_minf(extent[0], sdf_minf(extent[1], extent[2]));
        edit->lipschitz = largest / smallest;
        break;
    }
    case BTBH_GPU_SDF_SHAPE_BOX:
        extent[0] = source->size[0];
        extent[1] = source->size[1];
        extent[2] = source->size[2];
        edit->corner_radius = sdf_minf(
            source->corner_radius,
            sdf_minf(extent[0], sdf_minf(extent[1], extent[2]))
        );
        break;
    case BTBH_GPU_SDF_SHAPE_CAPSULE:
        extent[0] = source->size[0];
        extent[1] = source->size[1] + source->size[0];
        extent[2] = source->size[0];
        break;
    case BTBH_GPU_SDF_SHAPE_CYLINDER:
        extent[0] = source->size[0];
        extent[1] = source->size[1];
        extent[2] = source->size[0];
        edit->corner_radius = sdf_minf(source->corner_radius, sdf_minf(source->size[0], source->size[1]));
        break;
    case BTBH_GPU_SDF_SHAPE_TORUS:
    default:
        extent[0] = source->size[0] + source->size[1];
        extent[1] = source->size[1];
        extent[2] = source->size[0] + source->size[1];
        break;
    }
    const float grow = source->smoothing + SDF_REACH_MARGIN_CELLS * t->cell;
    for (int axis = 0; axis < 3; ++axis) {
        /* The local box turned into the world: |R| times the extents. */
        const float half =
            fabsf(rotation[axis * 3 + 0]) * extent[0] +
            fabsf(rotation[axis * 3 + 1]) * extent[1] +
            fabsf(rotation[axis * 3 + 2]) * extent[2];
        edit->reach_min[axis] = source->position[axis] - half - grow;
        edit->reach_max[axis] = source->position[axis] + half + grow;
    }
}

static bool sdf_edit_chunk_range(const BTBHGPUSdfTerrain *t, const SdfEdit *edit, int32_t lo[3], int32_t hi[3]) {
    for (int axis = 0; axis < 3; ++axis) {
        if (edit->reach_max[axis] < t->lattice_min[axis] || edit->reach_min[axis] > t->lattice_max[axis]) {
            return false;
        }
        lo[axis] = sdf_chunk_coord(t, axis, edit->reach_min[axis]);
        hi[axis] = sdf_chunk_coord(t, axis, edit->reach_max[axis]);
    }
    return true;
}

static uint32_t sdf_order_find(const BTBHGPUSdfTerrain *t, uint32_t id) {
    uint32_t lo = 0u;
    uint32_t hi = t->order.count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2u;
        const uint32_t mid_id = t->edits[t->order.items[mid]].id;
        if (mid_id < id) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static uint32_t sdf_live_slot(const BTBHGPUSdfTerrain *t, uint32_t id) {
    if (id == 0u) {
        return UINT32_MAX;
    }
    const uint32_t position = sdf_order_find(t, id);
    if (position >= t->order.count) {
        return UINT32_MAX;
    }
    const uint32_t slot = t->order.items[position];
    if (t->edits[slot].id != id || t->edits[slot].state != SDF_EDIT_LIVE) {
        return UINT32_MAX;
    }
    return slot;
}

static bool sdf_chunk_insert_edit(BTBHGPUSdfTerrain *t, SdfChunk *chunk, uint32_t slot) {
    const uint32_t id = t->edits[slot].id;
    uint32_t lo = 0u;
    uint32_t hi = chunk->edit_count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2u;
        if (t->edits[chunk->edits[mid]].id < id) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    if (lo < chunk->edit_count && chunk->edits[lo] == slot) {
        return true;
    }
    if (chunk->edit_count == chunk->edit_capacity) {
        const uint32_t capacity = chunk->edit_capacity < 4u ? 4u : chunk->edit_capacity * 2u;
        uint32_t *edits = (uint32_t *)realloc(chunk->edits, sizeof(uint32_t) * capacity);
        if (edits == NULL) {
            return false;
        }
        chunk->edits = edits;
        chunk->edit_capacity = capacity;
    }
    memmove(&chunk->edits[lo + 1u], &chunk->edits[lo], sizeof(uint32_t) * (chunk->edit_count - lo));
    chunk->edits[lo] = slot;
    chunk->edit_count += 1u;
    t->edits[slot].chunk_refs += 1u;
    return true;
}

static void sdf_edit_release(BTBHGPUSdfTerrain *t, uint32_t slot) {
    SdfEdit *edit = &t->edits[slot];
    const uint32_t position = sdf_order_find(t, edit->id);
    if (position < t->order.count && t->order.items[position] == slot) {
        memmove(
            &t->order.items[position],
            &t->order.items[position + 1u],
            sizeof(uint32_t) * (t->order.count - position - 1u)
        );
        t->order.count -= 1u;
    }
    edit->state = SDF_EDIT_FREE;
    edit->id = 0u;
    (void)sdf_list_push(&t->free_edit_slots, slot);
}

static bool sdf_chunk_remove_edit(BTBHGPUSdfTerrain *t, SdfChunk *chunk, uint32_t slot) {
    for (uint32_t index = 0u; index < chunk->edit_count; ++index) {
        if (chunk->edits[index] == slot) {
            memmove(
                &chunk->edits[index],
                &chunk->edits[index + 1u],
                sizeof(uint32_t) * (chunk->edit_count - index - 1u)
            );
            chunk->edit_count -= 1u;
            t->edits[slot].chunk_refs -= 1u;
            return true;
        }
    }
    return false;
}

static bool sdf_edit_link(BTBHGPUSdfTerrain *t, uint32_t slot) {
    int32_t lo[3];
    int32_t hi[3];
    if (!sdf_edit_chunk_range(t, &t->edits[slot], lo, hi)) {
        return true;
    }
    for (int32_t z = lo[2]; z <= hi[2]; ++z) {
        for (int32_t y = lo[1]; y <= hi[1]; ++y) {
            for (int32_t x = lo[0]; x <= hi[0]; ++x) {
                if (!sdf_chunk_insert_edit(t, &t->chunk_array[sdf_chunk_index(t, x, y, z)], slot)) {
                    return false;
                }
            }
        }
    }
    return true;
}

static void sdf_edit_unlink(BTBHGPUSdfTerrain *t, uint32_t slot, const SdfEdit *as_it_was) {
    int32_t lo[3];
    int32_t hi[3];
    if (!sdf_edit_chunk_range(t, as_it_was, lo, hi)) {
        return;
    }
    for (int32_t z = lo[2]; z <= hi[2]; ++z) {
        for (int32_t y = lo[1]; y <= hi[1]; ++y) {
            for (int32_t x = lo[0]; x <= hi[0]; ++x) {
                (void)sdf_chunk_remove_edit(t, &t->chunk_array[sdf_chunk_index(t, x, y, z)], slot);
            }
        }
    }
}

static uint32_t sdf_edit_slot_allocate(BTBHGPUSdfTerrain *t) {
    if (t->free_edit_slots.count > 0u) {
        return t->free_edit_slots.items[--t->free_edit_slots.count];
    }
    if (t->edit_capacity == UINT32_MAX / 2u) {
        return UINT32_MAX;
    }
    const uint32_t capacity = t->edit_capacity < 64u ? 64u : t->edit_capacity * 2u;
    SdfEdit *edits = (SdfEdit *)realloc(t->edits, sizeof(SdfEdit) * capacity);
    if (edits == NULL) {
        return UINT32_MAX;
    }
    memset(&edits[t->edit_capacity], 0, sizeof(SdfEdit) * (capacity - t->edit_capacity));
    t->edits = edits;
    for (uint32_t slot = capacity; slot > t->edit_capacity + 1u; --slot) {
        (void)sdf_list_push(&t->free_edit_slots, slot - 1u);
    }
    const uint32_t slot = t->edit_capacity;
    t->edit_capacity = capacity;
    return slot;
}

/* ------------------------------------------------------------------------------------------------ freezing */

static void sdf_evaluate_stale_in_chunk(BTBHGPUSdfTerrain *t, uint32_t chunk_index) {
    SdfChunk *chunk = &t->chunk_array[chunk_index];
    if (chunk->stale_count == 0u || chunk->bricks == NULL) {
        return;
    }
    SdfBrickEval *work = (SdfBrickEval *)malloc(sizeof(SdfBrickEval));
    if (work == NULL) {
        return;
    }
    for (uint32_t slot = 0u; slot < t->slots_per_chunk && chunk->stale_count > 0u; ++slot) {
        if (((chunk->stale_bits[slot >> 6u] >> (slot & 63u)) & 1u) == 0u) {
            continue;
        }
        chunk->stale_bits[slot >> 6u] &= ~(UINT64_C(1) << (slot & 63u));
        work->chunk_index = chunk_index;
        work->slot = slot;
        work->result = 0u;
        sdf_brick_evaluate(t, work);
        sdf_brick_commit(t, work);
    }
    free(work);
    /* The queue may still list these bricks; their stale bits are clear, so the queue skips them. */
}

static void sdf_freeze_chunk(BTBHGPUSdfTerrain *t, uint32_t chunk_index) {
    int32_t coords[3];
    sdf_chunk_coords(t, chunk_index, coords);
    /* The frozen bricks hold one more layer of samples on the high side: bring the neighbours up to date. */
    for (int32_t dz = 0; dz <= 1; ++dz) {
        for (int32_t dy = 0; dy <= 1; ++dy) {
            for (int32_t dx = 0; dx <= 1; ++dx) {
                const int32_t x = coords[0] + dx;
                const int32_t y = coords[1] + dy;
                const int32_t z = coords[2] + dz;
                if (x < t->chunks[0] && y < t->chunks[1] && z < t->chunks[2]) {
                    sdf_evaluate_stale_in_chunk(t, sdf_chunk_index(t, x, y, z));
                }
            }
        }
    }
    SdfChunk *chunk = &t->chunk_array[chunk_index];
    uintptr_t *frozen = (uintptr_t *)calloc(t->slots_per_chunk, sizeof(uintptr_t));
    if (frozen == NULL) {
        return;
    }
    if (chunk->bricks != NULL) {
        float points[SDF_FROZEN_SAMPLES * 3];
        float distances[SDF_FROZEN_SAMPLES];
        uint8_t materials[SDF_FROZEN_SAMPLES];
        uint32_t missing[SDF_FROZEN_SAMPLES];
        for (uint32_t slot = 0u; slot < t->slots_per_chunk; ++slot) {
            const uintptr_t live = chunk->bricks[slot];
            if (live == 0u) {
                continue;
            }
            if (sdf_is_tag(live)) {
                frozen[slot] = live;
                continue;
            }
            SdfFrozenBrick *brick = (SdfFrozenBrick *)calloc(1u, sizeof(SdfFrozenBrick));
            if (brick == NULL) {
                continue;
            }
            t->frozen_brick_total += 1u;
            memcpy(brick->differs, ((const SdfBrick *)live)->differs, sizeof(brick->differs));
            int32_t origin[3];
            sdf_brick_origin(t, chunk_index, slot, origin);
            uint32_t missing_count = 0u;
            for (int32_t z = 0; z < SDF_FROZEN_EDGE; ++z) {
                for (int32_t y = 0; y < SDF_FROZEN_EDGE; ++y) {
                    for (int32_t x = 0; x < SDF_FROZEN_EDGE; ++x) {
                        const uint32_t f = (uint32_t)((z * SDF_FROZEN_EDGE + y) * SDF_FROZEN_EDGE + x);
                        const int32_t gx = sdf_mini(origin[0] + x, t->cells[0]);
                        const int32_t gy = sdf_mini(origin[1] + y, t->cells[1]);
                        const int32_t gz = sdf_mini(origin[2] + z, t->cells[2]);
                        int8_t q = 0;
                        uint8_t m = 0u;
                        uint8_t d = 0u;
                        if (sdf_read_live_sample(t, gx, gy, gz, &q, &m, &d)) {
                            brick->distance[f] = q;
                            brick->material[f] = m;
                        } else {
                            points[missing_count * 3u + 0u] = sdf_lattice_coord(t, 0, gx);
                            points[missing_count * 3u + 1u] = sdf_lattice_coord(t, 1, gy);
                            points[missing_count * 3u + 2u] = sdf_lattice_coord(t, 2, gz);
                            missing[missing_count++] = f;
                        }
                    }
                }
            }
            sdf_original_base(t, points, missing_count, distances, materials);
            for (uint32_t index = 0u; index < missing_count; ++index) {
                brick->distance[missing[index]] = sdf_quantize(t, distances[index]);
                brick->material[missing[index]] = materials[index];
            }
            frozen[slot] = (uintptr_t)brick;
        }
    }
    if (chunk->frozen != NULL) {
        for (uint32_t slot = 0u; slot < t->slots_per_chunk; ++slot) {
            sdf_frozen_slot_release(t, chunk->frozen[slot]);
        }
        free(chunk->frozen);
    }
    chunk->frozen = frozen;
    chunk->is_frozen = 1u;
    for (uint32_t index = 0u; index < chunk->edit_count; ++index) {
        const uint32_t slot = chunk->edits[index];
        SdfEdit *edit = &t->edits[slot];
        if (edit->state == SDF_EDIT_LIVE) {
            edit->state = SDF_EDIT_FROZEN;
            t->live_count -= 1u;
            t->frozen_total += 1u;
        }
        edit->chunk_refs -= 1u;
        if (edit->chunk_refs == 0u) {
            sdf_edit_release(t, slot);
        }
    }
    chunk->edit_count = 0u;
}

static void sdf_freeze_oldest(BTBHGPUSdfTerrain *t) {
    uint32_t oldest = UINT32_MAX;
    for (uint32_t index = 0u; index < t->order.count; ++index) {
        const uint32_t slot = t->order.items[index];
        if (t->edits[slot].state == SDF_EDIT_LIVE) {
            oldest = slot;
            break;
        }
    }
    if (oldest == UINT32_MAX) {
        return;
    }
    int32_t lo[3];
    int32_t hi[3];
    if (sdf_edit_chunk_range(t, &t->edits[oldest], lo, hi)) {
        for (int32_t z = lo[2]; z <= hi[2]; ++z) {
            for (int32_t y = lo[1]; y <= hi[1]; ++y) {
                for (int32_t x = lo[0]; x <= hi[0]; ++x) {
                    sdf_freeze_chunk(t, sdf_chunk_index(t, x, y, z));
                }
            }
        }
    }
    /* The freezes released it unless it reached no chunk. */
    if (t->edits[oldest].state == SDF_EDIT_LIVE) {
        t->edits[oldest].state = SDF_EDIT_FROZEN;
        t->live_count -= 1u;
        t->frozen_total += 1u;
        if (t->edits[oldest].chunk_refs == 0u) {
            sdf_edit_release(t, oldest);
        }
    }
}

/* ------------------------------------------------------------------------------------------------ meshing */

static void sdf_mesh_free(SdfMesh *mesh) {
    if (mesh == NULL) {
        return;
    }
    free(mesh->positions);
    free(mesh->normals);
    free(mesh->materials);
    free(mesh->indices);
    free(mesh->column_kind);
    free(mesh->column_height);
    free(mesh);
}

typedef struct SdfVertexBuild {
    float position[3];
    float normal[3];
    uint8_t material;
    uint8_t apron;
    uint32_t cell[3];
} SdfVertexBuild;

static const int32_t sdf_corner_offsets[8][3] = {
    { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 1, 1, 0 },
    { 0, 0, 1 }, { 1, 0, 1 }, { 0, 1, 1 }, { 1, 1, 1 }
};

static const uint8_t sdf_cell_edges[12][2] = {
    { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 },
    { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 },
    { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 }
};

static bool sdf_append_triangle(uint32_t **indices, uint32_t *count, uint32_t *capacity, uint32_t a, uint32_t b, uint32_t c) {
    if (*count + 3u > *capacity) {
        const uint32_t next = *capacity < 192u ? 192u : *capacity * 2u;
        uint32_t *grown = (uint32_t *)realloc(*indices, sizeof(uint32_t) * next);
        if (grown == NULL) {
            return false;
        }
        *indices = grown;
        *capacity = next;
    }
    (*indices)[(*count)++] = a;
    (*indices)[(*count)++] = b;
    (*indices)[(*count)++] = c;
    return true;
}

static float sdf_distance_squared(const float a[3], const float b[3]) {
    const float x = a[0] - b[0];
    const float y = a[1] - b[1];
    const float z = a[2] - b[2];
    return x * x + y * y + z * z;
}

static uint8_t sdf_majority(uint8_t a, uint8_t b, uint8_t c) {
    if (a == b || a == c) {
        return a;
    }
    if (b == c) {
        return b;
    }
    const uint8_t lower = a < b ? a : b;
    return lower < c ? lower : c;
}

/* Builds a chunk's mesh from the live bricks (any thread). NULL when the chunk has no patch. */
static SdfMesh *sdf_mesh_chunk(const BTBHGPUSdfTerrain *t, uint32_t chunk_index) {
    int32_t coords[3];
    sdf_chunk_coords(t, chunk_index, coords);
    const int32_t n = t->chunk_cells;
    int32_t c0[3];
    int32_t region_lo[3];
    int32_t region_hi[3];
    for (int axis = 0; axis < 3; ++axis) {
        c0[axis] = coords[axis] * n;
        region_lo[axis] = sdf_maxi(c0[axis] - 2, 0);
        region_hi[axis] = sdf_mini(c0[axis] + n + 1, t->cells[axis]);
        if (region_hi[axis] - region_lo[axis] < 1) {
            return NULL;
        }
    }
    /* The active bricks the region reads, in global brick coordinates. */
    int32_t active_lo[3] = { INT32_MAX, INT32_MAX, INT32_MAX };
    int32_t active_hi[3] = { INT32_MIN, INT32_MIN, INT32_MIN };
    const int32_t b = t->bricks_per_chunk;
    for (int32_t gz = region_lo[2] / SDF_BRICK; gz <= region_hi[2] / SDF_BRICK; ++gz) {
        for (int32_t gy = region_lo[1] / SDF_BRICK; gy <= region_hi[1] / SDF_BRICK; ++gy) {
            for (int32_t gx = region_lo[0] / SDF_BRICK; gx <= region_hi[0] / SDF_BRICK; ++gx) {
                const SdfChunk *owner = &t->chunk_array[sdf_chunk_index(t, gx / b, gy / b, gz / b)];
                if (owner->bricks == NULL) {
                    continue;
                }
                const uintptr_t slot = owner->bricks[sdf_slot_index(t, gx % b, gy % b, gz % b)];
                if (slot == 0u || sdf_is_tag(slot) || !((const SdfBrick *)slot)->active) {
                    continue;
                }
                const int32_t g[3] = { gx, gy, gz };
                for (int axis = 0; axis < 3; ++axis) {
                    active_lo[axis] = sdf_mini(active_lo[axis], g[axis]);
                    active_hi[axis] = sdf_maxi(active_hi[axis], g[axis]);
                }
            }
        }
    }
    if (active_lo[0] == INT32_MAX) {
        return NULL;
    }
    int32_t box_lo[3];
    int32_t dims[3];
    for (int axis = 0; axis < 3; ++axis) {
        /* Cells of the mesh set lie within three samples of a differing one; a fourth keeps the set's border. */
        box_lo[axis] = sdf_maxi(region_lo[axis], active_lo[axis] * SDF_BRICK - 4);
        const int32_t box_hi = sdf_mini(region_hi[axis], active_hi[axis] * SDF_BRICK + SDF_BRICK - 1 + 4);
        dims[axis] = box_hi - box_lo[axis] + 1;
        if (dims[axis] < 2) {
            return NULL;
        }
    }
    const size_t sample_count = (size_t)dims[0] * (size_t)dims[1] * (size_t)dims[2];
    const int32_t cdims[3] = { dims[0] - 1, dims[1] - 1, dims[2] - 1 };
    const size_t cell_count = (size_t)cdims[0] * (size_t)cdims[1] * (size_t)cdims[2];
    int8_t *q = (int8_t *)malloc(sample_count);
    uint8_t *mat = (uint8_t *)malloc(sample_count);
    uint8_t *diff = (uint8_t *)malloc(sample_count);
    uint8_t *cell_sign = (uint8_t *)malloc(cell_count);
    uint8_t *cell_diff = (uint8_t *)malloc(cell_count);
    uint8_t *cell_set = (uint8_t *)malloc(cell_count);
    uint8_t *scratch = (uint8_t *)malloc(cell_count);
    uint32_t *cell_vertex = (uint32_t *)malloc(cell_count * sizeof(uint32_t));
    SdfVertexBuild *vertices = NULL;
    float *eval_points = NULL;
    float *eval_distances = NULL;
    uint8_t *eval_materials = NULL;
    uint32_t *indices = NULL;
    uint32_t index_count = 0u;
    uint32_t index_capacity = 0u;
    SdfMesh *mesh = NULL;
    uint32_t vertex_count = 0u;
    uint32_t vertex_capacity = 0u;
    if (q == NULL || mat == NULL || diff == NULL || cell_sign == NULL || cell_diff == NULL ||
        cell_set == NULL || scratch == NULL || cell_vertex == NULL) {
        goto cleanup;
    }
#define SDF_SAMPLE(x, y, z) (((size_t)(z) * (size_t)dims[1] + (size_t)(y)) * (size_t)dims[0] + (size_t)(x))
#define SDF_CELL(x, y, z) (((size_t)(z) * (size_t)cdims[1] + (size_t)(y)) * (size_t)cdims[0] + (size_t)(x))
    /* -128 (never quantized): no brick holds the sample, so it is unknown and no cell that has it as a corner is a
     * surface. The mesh set never reaches such samples (SDF_REACH_MARGIN_CELLS). Filled a brick at a time. */
    memset(q, 0x80, sample_count);
    memset(mat, 0, sample_count);
    memset(diff, 0, sample_count);
    {
        const int32_t box_hi[3] = { box_lo[0] + dims[0] - 1, box_lo[1] + dims[1] - 1, box_lo[2] + dims[2] - 1 };
        for (int32_t gz = box_lo[2] / SDF_BRICK; gz <= box_hi[2] / SDF_BRICK; ++gz) {
            for (int32_t gy = box_lo[1] / SDF_BRICK; gy <= box_hi[1] / SDF_BRICK; ++gy) {
                for (int32_t gx = box_lo[0] / SDF_BRICK; gx <= box_hi[0] / SDF_BRICK; ++gx) {
                    const SdfChunk *owner = &t->chunk_array[sdf_chunk_index(t, gx / b, gy / b, gz / b)];
                    if (owner->bricks == NULL) {
                        continue;
                    }
                    const uintptr_t slot = owner->bricks[sdf_slot_index(t, gx % b, gy % b, gz % b)];
                    if (slot == 0u) {
                        continue;
                    }
                    const int32_t g[3] = { gx, gy, gz };
                    int32_t lo[3];
                    int32_t hi[3];
                    for (int axis = 0; axis < 3; ++axis) {
                        lo[axis] = sdf_maxi(g[axis] * SDF_BRICK, box_lo[axis]);
                        hi[axis] = sdf_mini(g[axis] * SDF_BRICK + SDF_BRICK - 1, box_hi[axis]);
                        hi[axis] = sdf_mini(hi[axis], t->cells[axis]);
                    }
                    if (hi[0] < lo[0] || hi[1] < lo[1] || hi[2] < lo[2]) {
                        continue;
                    }
                    const size_t run = (size_t)(hi[0] - lo[0] + 1);
                    if (sdf_is_tag(slot)) {
                        const int8_t value = sdf_tag_inside(slot) ? (int8_t)-127 : (int8_t)127;
                        const uint8_t material = sdf_tag_material(slot);
                        for (int32_t z = lo[2]; z <= hi[2]; ++z) {
                            for (int32_t y = lo[1]; y <= hi[1]; ++y) {
                                const size_t at = SDF_SAMPLE(lo[0] - box_lo[0], y - box_lo[1], z - box_lo[2]);
                                memset(&q[at], (unsigned char)value, run);
                                memset(&mat[at], material, run);
                            }
                        }
                        continue;
                    }
                    const SdfBrick *brick = (const SdfBrick *)slot;
                    for (int32_t z = lo[2]; z <= hi[2]; ++z) {
                        for (int32_t y = lo[1]; y <= hi[1]; ++y) {
                            const size_t at = SDF_SAMPLE(lo[0] - box_lo[0], y - box_lo[1], z - box_lo[2]);
                            const uint32_t from = (uint32_t)(((z - gz * SDF_BRICK) * SDF_BRICK + (y - gy * SDF_BRICK)) *
                                SDF_BRICK + (lo[0] - gx * SDF_BRICK));
                            memcpy(&q[at], &brick->distance[from], run);
                            memcpy(&mat[at], &brick->material[from], run);
                            for (size_t k = 0u; k < run; ++k) {
                                const uint32_t bit = from + (uint32_t)k;
                                diff[at + k] = (uint8_t)((brick->differs[bit >> 6u] >> (bit & 63u)) & 1u);
                            }
                        }
                    }
                }
            }
        }
    }
    size_t corner_step[8];
    for (int corner = 0; corner < 8; ++corner) {
        corner_step[corner] = SDF_SAMPLE(
            sdf_corner_offsets[corner][0],
            sdf_corner_offsets[corner][1],
            sdf_corner_offsets[corner][2]
        );
    }
    for (int32_t z = 0; z < cdims[2]; ++z) {
        for (int32_t y = 0; y < cdims[1]; ++y) {
            for (int32_t x = 0; x < cdims[0]; ++x) {
                uint8_t sign = 0u;
                uint8_t differs = 0u;
                bool unknown = false;
                const size_t base_sample = SDF_SAMPLE(x, y, z);
                for (int corner = 0; corner < 8; ++corner) {
                    const size_t s = base_sample + corner_step[corner];
                    if (q[s] == -128) {
                        unknown = true;
                    } else if (q[s] < 0) {
                        sign = (uint8_t)(sign | (1u << corner));
                    }
                    differs = (uint8_t)(differs | diff[s]);
                }
                const size_t c = SDF_CELL(x, y, z);
                cell_sign[c] = unknown ? 0u : sign;
                cell_diff[c] = differs;
            }
        }
    }
    /* The mesh set: cells within one cell of a cell with a differing corner (a separable dilation). */
    memcpy(cell_set, cell_diff, cell_count);
    for (int axis = 0; axis < 3; ++axis) {
        memcpy(scratch, cell_set, cell_count);
        for (int32_t z = 0; z < cdims[2]; ++z) {
            for (int32_t y = 0; y < cdims[1]; ++y) {
                for (int32_t x = 0; x < cdims[0]; ++x) {
                    const int32_t p[3] = { x, y, z };
                    uint8_t value = scratch[SDF_CELL(x, y, z)];
                    if (p[axis] > 0) {
                        int32_t o[3] = { x, y, z };
                        o[axis] -= 1;
                        value = (uint8_t)(value | scratch[SDF_CELL(o[0], o[1], o[2])]);
                    }
                    if (p[axis] < cdims[axis] - 1) {
                        int32_t o[3] = { x, y, z };
                        o[axis] += 1;
                        value = (uint8_t)(value | scratch[SDF_CELL(o[0], o[1], o[2])]);
                    }
                    cell_set[SDF_CELL(x, y, z)] = value;
                }
            }
        }
    }
    for (size_t c = 0u; c < cell_count; ++c) {
        cell_vertex[c] = SDF_NO_VERTEX;
    }
    /* Vertices: cells [c0 - 1, c0 + n) in the set with a sign change. */
    int32_t emit_lo[3];
    int32_t emit_hi[3];
    for (int axis = 0; axis < 3; ++axis) {
        emit_lo[axis] = sdf_maxi(sdf_maxi(c0[axis] - 1, 0), box_lo[axis]) - box_lo[axis];
        emit_hi[axis] = sdf_mini(
            sdf_mini(c0[axis] + n - 1, t->cells[axis] - 1),
            box_lo[axis] + cdims[axis] - 1
        ) - box_lo[axis];
    }
    for (int32_t z = emit_lo[2]; z <= emit_hi[2]; ++z) {
        for (int32_t y = emit_lo[1]; y <= emit_hi[1]; ++y) {
            for (int32_t x = emit_lo[0]; x <= emit_hi[0]; ++x) {
                const size_t c = SDF_CELL(x, y, z);
                const uint8_t sign = cell_sign[c];
                if (!cell_set[c] || sign == 0u || sign == 255u) {
                    continue;
                }
                if (vertex_count == vertex_capacity) {
                    const uint32_t capacity = vertex_capacity < 256u ? 256u : vertex_capacity * 2u;
                    SdfVertexBuild *grown = (SdfVertexBuild *)realloc(vertices, sizeof(SdfVertexBuild) * capacity);
                    if (grown == NULL) {
                        goto cleanup;
                    }
                    vertices = grown;
                    vertex_capacity = capacity;
                }
                SdfVertexBuild *vertex = &vertices[vertex_count];
                float corner_position[8][3];
                int8_t corner_q[8];
                for (int corner = 0; corner < 8; ++corner) {
                    const int32_t sx = x + sdf_corner_offsets[corner][0];
                    const int32_t sy = y + sdf_corner_offsets[corner][1];
                    const int32_t sz = z + sdf_corner_offsets[corner][2];
                    corner_q[corner] = q[SDF_SAMPLE(sx, sy, sz)];
                    corner_position[corner][0] = sdf_lattice_coord(t, 0, box_lo[0] + sx);
                    corner_position[corner][1] = sdf_lattice_coord(t, 1, box_lo[1] + sy);
                    corner_position[corner][2] = sdf_lattice_coord(t, 2, box_lo[2] + sz);
                }
                float sum[3] = { 0.0f, 0.0f, 0.0f };
                uint32_t crossings = 0u;
                for (int edge = 0; edge < 12; ++edge) {
                    const int a = sdf_cell_edges[edge][0];
                    const int e = sdf_cell_edges[edge][1];
                    if ((corner_q[a] < 0) == (corner_q[e] < 0)) {
                        continue;
                    }
                    const float qa = (float)corner_q[a];
                    const float qe = (float)corner_q[e];
                    const float fraction = qa / (qa - qe);
                    for (int axis = 0; axis < 3; ++axis) {
                        sum[axis] += corner_position[a][axis] +
                            fraction * (corner_position[e][axis] - corner_position[a][axis]);
                    }
                    crossings += 1u;
                }
                for (int axis = 0; axis < 3; ++axis) {
                    vertex->position[axis] = sum[axis] / (float)crossings;
                }
                int best = -1;
                for (int corner = 0; corner < 8; ++corner) {
                    if (corner_q[corner] < 0 && (best < 0 || corner_q[corner] > corner_q[best])) {
                        best = corner;
                    }
                }
                vertex->material = mat[SDF_SAMPLE(
                    x + sdf_corner_offsets[best][0],
                    y + sdf_corner_offsets[best][1],
                    z + sdf_corner_offsets[best][2]
                )];
                vertex->apron = cell_diff[c] ? 0u : 1u;
                vertex->cell[0] = (uint32_t)x;
                vertex->cell[1] = (uint32_t)y;
                vertex->cell[2] = (uint32_t)z;
                cell_vertex[c] = vertex_count;
                vertex_count += 1u;
            }
        }
    }
    if (vertex_count == 0u) {
        goto columns;
    }
    /* One step along the field's gradient (tetrahedral differences of the exact field). */
    {
        static const float tetra[4][3] = {
            { 1.0f, -1.0f, -1.0f }, { -1.0f, -1.0f, 1.0f }, { -1.0f, 1.0f, -1.0f }, { 1.0f, 1.0f, 1.0f }
        };
        const float h = SDF_GRADIENT_STEP_CELLS * t->cell;
        eval_points = (float *)malloc(sizeof(float) * 12u * vertex_count);
        eval_distances = (float *)malloc(sizeof(float) * 4u * vertex_count);
        eval_materials = (uint8_t *)malloc(4u * vertex_count);
        if (eval_points == NULL || eval_distances == NULL || eval_materials == NULL) {
            goto cleanup;
        }
        for (uint32_t v = 0u; v < vertex_count; ++v) {
            for (int k = 0; k < 4; ++k) {
                for (int axis = 0; axis < 3; ++axis) {
                    eval_points[(v * 4u + (uint32_t)k) * 3u + (uint32_t)axis] =
                        vertices[v].position[axis] + h * tetra[k][axis];
                }
            }
        }
        sdf_field_points(t, eval_points, 4u * vertex_count, eval_distances, eval_materials);
        for (uint32_t v = 0u; v < vertex_count; ++v) {
            SdfVertexBuild *vertex = &vertices[v];
            float gradient[3] = { 0.0f, 0.0f, 0.0f };
            float value = 0.0f;
            for (int k = 0; k < 4; ++k) {
                const float f = eval_distances[v * 4u + (uint32_t)k];
                value += f;
                for (int axis = 0; axis < 3; ++axis) {
                    gradient[axis] += tetra[k][axis] * f;
                }
            }
            value *= 0.25f;
            for (int axis = 0; axis < 3; ++axis) {
                gradient[axis] /= 4.0f * h;
            }
            const float length_squared =
                gradient[0] * gradient[0] + gradient[1] * gradient[1] + gradient[2] * gradient[2];
            if (length_squared > 1.0e-12f && sdf_finite(length_squared) && sdf_finite(value)) {
                const float length = sqrtf(length_squared);
                for (int axis = 0; axis < 3; ++axis) {
                    const float cell_lo = sdf_lattice_coord(t, axis, box_lo[axis] + (int32_t)vertex->cell[axis]);
                    const float cell_hi = sdf_lattice_coord(t, axis, box_lo[axis] + (int32_t)vertex->cell[axis] + 1);
                    vertex->position[axis] = sdf_clampf(
                        vertex->position[axis] - value * gradient[axis] / length_squared,
                        cell_lo,
                        cell_hi
                    );
                    vertex->normal[axis] = gradient[axis] / length;
                }
            } else {
                vertex->normal[0] = 0.0f;
                vertex->normal[1] = 1.0f;
                vertex->normal[2] = 0.0f;
            }
            if (vertex->apron) {
                vertex->position[1] -= SDF_APRON_SINK_CELLS * t->cell;
            }
        }
    }
    /* Quads: each sign-changing edge that starts at a sample this chunk owns, between four cells with vertices.
     * The edge starts at the low corner of the last of those cells, so only cells with a vertex are visited. */
    for (uint32_t v = 0u; v < vertex_count; ++v) {
        const int32_t x = (int32_t)vertices[v].cell[0];
        const int32_t y = (int32_t)vertices[v].cell[1];
        const int32_t z = (int32_t)vertices[v].cell[2];
        const int32_t s[3] = { x, y, z };
        bool owned = true;
        for (int axis = 0; axis < 3; ++axis) {
            const int32_t g = box_lo[axis] + s[axis];
            if (g < c0[axis] || g >= c0[axis] + n) {
                owned = false;
            }
        }
        if (!owned) {
            continue;
        }
        const bool inside = q[SDF_SAMPLE(x, y, z)] < 0;
        for (int axis = 0; axis < 3; ++axis) {
            const int u = (axis + 1) % 3;
            const int w = (axis + 2) % 3;
            if (s[u] < 1 || s[w] < 1) {
                continue;
            }
            int32_t end[3] = { x, y, z };
            end[axis] += 1;
            if (end[axis] >= dims[axis] || box_lo[axis] + end[axis] > t->cells[axis]) {
                continue;
            }
            if ((q[SDF_SAMPLE(end[0], end[1], end[2])] < 0) == inside) {
                continue;
            }
            static const int32_t around[4][2] = { { -1, -1 }, { 0, -1 }, { 0, 0 }, { -1, 0 } };
            uint32_t quad[4];
            bool complete = true;
            for (int corner = 0; corner < 4; ++corner) {
                int32_t cell[3] = { x, y, z };
                cell[u] += around[corner][0];
                cell[w] += around[corner][1];
                quad[corner] = cell_vertex[SDF_CELL(cell[0], cell[1], cell[2])];
                if (quad[corner] == SDF_NO_VERTEX) {
                    complete = false;
                    break;
                }
            }
            if (!complete) {
                continue;
            }
            if (!inside) {
                const uint32_t swap = quad[1];
                quad[1] = quad[3];
                quad[3] = swap;
            }
            const float d02 = sdf_distance_squared(vertices[quad[0]].position, vertices[quad[2]].position);
            const float d13 = sdf_distance_squared(vertices[quad[1]].position, vertices[quad[3]].position);
            bool ok;
            if (d02 <= d13) {
                ok = sdf_append_triangle(&indices, &index_count, &index_capacity, quad[0], quad[1], quad[2]) &&
                    sdf_append_triangle(&indices, &index_count, &index_capacity, quad[0], quad[2], quad[3]);
            } else {
                ok = sdf_append_triangle(&indices, &index_count, &index_capacity, quad[0], quad[1], quad[3]) &&
                    sdf_append_triangle(&indices, &index_count, &index_capacity, quad[1], quad[2], quad[3]);
            }
            if (!ok) {
                goto cleanup;
            }
        }
    }
columns:
    mesh = (SdfMesh *)calloc(1u, sizeof(SdfMesh));
    if (mesh == NULL) {
        goto cleanup;
    }
    if (index_count > 0u) {
        const bool faceted = t->desc.normals == BTBH_GPU_SDF_NORMALS_FACETED;
        uint32_t *remap = (uint32_t *)malloc(sizeof(uint32_t) * vertex_count);
        if (remap == NULL) {
            sdf_mesh_free(mesh);
            mesh = NULL;
            goto cleanup;
        }
        uint32_t used = 0u;
        if (faceted) {
            used = index_count;
        } else {
            for (uint32_t v = 0u; v < vertex_count; ++v) {
                remap[v] = SDF_NO_VERTEX;
            }
            for (uint32_t i = 0u; i < index_count; ++i) {
                if (remap[indices[i]] == SDF_NO_VERTEX) {
                    remap[indices[i]] = used++;
                }
            }
        }
        mesh->positions = (float *)malloc(sizeof(float) * 3u * used);
        mesh->normals = (float *)malloc(sizeof(float) * 3u * used);
        mesh->materials = (uint8_t *)malloc(used);
        mesh->indices = (uint32_t *)malloc(sizeof(uint32_t) * index_count);
        if (mesh->positions == NULL || mesh->normals == NULL || mesh->materials == NULL || mesh->indices == NULL) {
            free(remap);
            sdf_mesh_free(mesh);
            mesh = NULL;
            goto cleanup;
        }
        mesh->vertex_count = used;
        mesh->index_count = index_count;
        if (faceted) {
            for (uint32_t tri = 0u; tri < index_count / 3u; ++tri) {
                const SdfVertexBuild *a = &vertices[indices[tri * 3u + 0u]];
                const SdfVertexBuild *bv = &vertices[indices[tri * 3u + 1u]];
                const SdfVertexBuild *c = &vertices[indices[tri * 3u + 2u]];
                const float e1[3] = {
                    bv->position[0] - a->position[0], bv->position[1] - a->position[1], bv->position[2] - a->position[2]
                };
                const float e2[3] = {
                    c->position[0] - a->position[0], c->position[1] - a->position[1], c->position[2] - a->position[2]
                };
                float normal[3] = {
                    e1[1] * e2[2] - e1[2] * e2[1],
                    e1[2] * e2[0] - e1[0] * e2[2],
                    e1[0] * e2[1] - e1[1] * e2[0]
                };
                const float length = sdf_length3(normal[0], normal[1], normal[2]);
                if (length > 1.0e-20f) {
                    normal[0] /= length;
                    normal[1] /= length;
                    normal[2] /= length;
                } else {
                    normal[0] = a->normal[0];
                    normal[1] = a->normal[1];
                    normal[2] = a->normal[2];
                }
                const uint8_t material = sdf_majority(a->material, bv->material, c->material);
                const SdfVertexBuild *corners[3] = { a, bv, c };
                for (uint32_t k = 0u; k < 3u; ++k) {
                    const uint32_t out = tri * 3u + k;
                    memcpy(&mesh->positions[out * 3u], corners[k]->position, sizeof(float) * 3u);
                    memcpy(&mesh->normals[out * 3u], normal, sizeof(float) * 3u);
                    mesh->materials[out] = material;
                    mesh->indices[out] = out;
                }
            }
        } else {
            for (uint32_t v = 0u; v < vertex_count; ++v) {
                if (remap[v] == SDF_NO_VERTEX) {
                    continue;
                }
                memcpy(&mesh->positions[remap[v] * 3u], vertices[v].position, sizeof(float) * 3u);
                memcpy(&mesh->normals[remap[v] * 3u], vertices[v].normal, sizeof(float) * 3u);
                mesh->materials[remap[v]] = vertices[v].material;
            }
            for (uint32_t i = 0u; i < index_count; ++i) {
                mesh->indices[i] = remap[indices[i]];
            }
        }
        free(remap);
        for (int axis = 0; axis < 3; ++axis) {
            mesh->bounds_min[axis] = FLT_MAX;
            mesh->bounds_max[axis] = -FLT_MAX;
        }
        for (uint32_t v = 0u; v < mesh->vertex_count; ++v) {
            for (int axis = 0; axis < 3; ++axis) {
                mesh->bounds_min[axis] = sdf_minf(mesh->bounds_min[axis], mesh->positions[v * 3u + (uint32_t)axis]);
                mesh->bounds_max[axis] = sdf_maxf(mesh->bounds_max[axis], mesh->positions[v * 3u + (uint32_t)axis]);
            }
        }
    }
    /* The mask's view of this chunk: each own cell column's top surface here, if the patch owns it. */
    {
        bool any = false;
        int32_t column_lo[2];
        int32_t column_hi[2];
        const int column_axes[2] = { 0, 2 };
        for (int k = 0; k < 2; ++k) {
            const int axis = column_axes[k];
            column_lo[k] = sdf_maxi(c0[axis], box_lo[axis]);
            column_hi[k] = sdf_mini(sdf_mini(c0[axis] + n - 1, t->cells[axis] - 1), box_lo[axis] + cdims[axis] - 1);
        }
        const int32_t top = sdf_mini(sdf_mini(c0[1] + n - 1, t->cells[1] - 1), box_lo[1] + cdims[1] - 1) - box_lo[1];
        const int32_t bottom = sdf_maxi(c0[1], box_lo[1]) - box_lo[1];
        if (top >= bottom && column_hi[0] >= column_lo[0] && column_hi[1] >= column_lo[1]) {
            for (int32_t gz = column_lo[1]; gz <= column_hi[1]; ++gz) {
                for (int32_t gx = column_lo[0]; gx <= column_hi[0]; ++gx) {
                    const int32_t x = gx - box_lo[0];
                    const int32_t z = gz - box_lo[2];
                    uint8_t kind = SDF_COLUMN_NONE;
                    float height = 0.0f;
                    if (cell_sign[SDF_CELL(x, top, z)] == 255u) {
                        kind = SDF_COLUMN_BASE;
                    } else {
                        for (int32_t y = top; y >= bottom; --y) {
                            const size_t c = SDF_CELL(x, y, z);
                            if (cell_sign[c] == 0u || cell_sign[c] == 255u) {
                                continue;
                            }
                            if (!cell_diff[c]) {
                                kind = SDF_COLUMN_BASE;
                                break;
                            }
                            kind = SDF_COLUMN_PATCH;
                            const float bottom_y = sdf_lattice_coord(t, 1, box_lo[1] + y);
                            height = FLT_MAX;
                            for (int corner = 0; corner < 4; ++corner) {
                                const int32_t ox = corner & 1;
                                const int32_t oz = corner >> 1;
                                const int8_t lo_q = q[SDF_SAMPLE(x + ox, y, z + oz)];
                                const int8_t hi_q = q[SDF_SAMPLE(x + ox, y + 1, z + oz)];
                                if ((lo_q < 0) != (hi_q < 0)) {
                                    const float fraction = (float)lo_q / ((float)lo_q - (float)hi_q);
                                    height = sdf_minf(height, bottom_y + fraction * t->cell);
                                }
                            }
                            if (height == FLT_MAX) {
                                height = bottom_y;
                            }
                            break;
                        }
                    }
                    if (kind == SDF_COLUMN_NONE) {
                        continue;
                    }
                    if (!any) {
                        const size_t columns = (size_t)n * (size_t)n;
                        mesh->column_kind = (uint8_t *)calloc(columns, 1u);
                        mesh->column_height = (float *)calloc(columns, sizeof(float));
                        if (mesh->column_kind == NULL || mesh->column_height == NULL) {
                            sdf_mesh_free(mesh);
                            mesh = NULL;
                            goto cleanup;
                        }
                        any = true;
                    }
                    const size_t column = (size_t)(gz - c0[2]) * (size_t)n + (size_t)(gx - c0[0]);
                    mesh->column_kind[column] = kind;
                    mesh->column_height[column] = height;
                }
            }
        }
    }
    if (mesh->index_count == 0u && mesh->column_kind == NULL) {
        sdf_mesh_free(mesh);
        mesh = NULL;
        goto cleanup;
    }
    {
        uint64_t hash = UINT64_C(1469598103934665603);
        hash = sdf_hash_bytes(hash, &mesh->vertex_count, sizeof(mesh->vertex_count));
        hash = sdf_hash_bytes(hash, &mesh->index_count, sizeof(mesh->index_count));
        if (mesh->vertex_count > 0u) {
            hash = sdf_hash_bytes(hash, mesh->positions, sizeof(float) * 3u * mesh->vertex_count);
            hash = sdf_hash_bytes(hash, mesh->normals, sizeof(float) * 3u * mesh->vertex_count);
            hash = sdf_hash_bytes(hash, mesh->materials, mesh->vertex_count);
            hash = sdf_hash_bytes(hash, mesh->indices, sizeof(uint32_t) * mesh->index_count);
        }
        if (mesh->column_kind != NULL) {
            const size_t columns = (size_t)n * (size_t)n;
            hash = sdf_hash_bytes(hash, mesh->column_kind, columns);
            hash = sdf_hash_bytes(hash, mesh->column_height, columns * sizeof(float));
        }
        mesh->hash = hash;
        mesh->bytes = (uint64_t)mesh->vertex_count * (sizeof(float) * 6u + 1u) +
            (uint64_t)mesh->index_count * sizeof(uint32_t) +
            (mesh->column_kind != NULL ? (uint64_t)n * (uint64_t)n * (1u + sizeof(float)) : 0u) +
            sizeof(SdfMesh);
    }
#undef SDF_SAMPLE
#undef SDF_CELL
cleanup:
    free(q);
    free(mat);
    free(diff);
    free(cell_sign);
    free(cell_diff);
    free(cell_set);
    free(scratch);
    free(cell_vertex);
    free(vertices);
    free(eval_points);
    free(eval_distances);
    free(eval_materials);
    free(indices);
    return mesh;
}

/* ------------------------------------------------------------------------------------------------ switching */

static void sdf_push_change(BTBHGPUSdfTerrain *t, const float rect[4]) {
    if (t->change_head + t->change_count == t->change_capacity) {
        if (t->change_head > 0u) {
            memmove(t->changes, t->changes + t->change_head, sizeof(float) * 4u * t->change_count);
            t->change_head = 0u;
        } else {
            const uint32_t capacity = t->change_capacity < 32u ? 32u : t->change_capacity * 2u;
            float (*changes)[4] = (float (*)[4])realloc(t->changes, sizeof(float) * 4u * capacity);
            if (changes == NULL) {
                return;
            }
            t->changes = changes;
            t->change_capacity = capacity;
        }
    }
    memcpy(t->changes[t->change_head + t->change_count], rect, sizeof(float) * 4u);
    t->change_count += 1u;
}

static void sdf_chunk_xz_box(const BTBHGPUSdfTerrain *t, uint32_t chunk_index, float rect[4]) {
    int32_t coords[3];
    sdf_chunk_coords(t, chunk_index, coords);
    rect[0] = sdf_lattice_coord(t, 0, coords[0] * t->chunk_cells);
    rect[1] = sdf_lattice_coord(t, 2, coords[2] * t->chunk_cells);
    rect[2] = sdf_lattice_coord(t, 0, sdf_mini((coords[0] + 1) * t->chunk_cells, t->cells[0]));
    rect[3] = sdf_lattice_coord(t, 2, sdf_mini((coords[2] + 1) * t->chunk_cells, t->cells[2]));
}

static void sdf_switch_chunk(BTBHGPUSdfTerrain *t, uint32_t chunk_index, uint8_t *column_dirty) {
    SdfChunk *chunk = &t->chunk_array[chunk_index];
    if (!chunk->has_pending) {
        return;
    }
    SdfMesh *next = chunk->pending;
    SdfMesh *old = chunk->mesh;
    chunk->pending = NULL;
    chunk->has_pending = 0u;
    if ((old == NULL) == (next == NULL) && (old == NULL || old->hash == next->hash)) {
        sdf_mesh_free(next);
        return;
    }
    float rect[4] = { FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX };
    const SdfMesh *meshes[2] = { old, next };
    for (int index = 0; index < 2; ++index) {
        if (meshes[index] != NULL && meshes[index]->vertex_count > 0u) {
            rect[0] = sdf_minf(rect[0], meshes[index]->bounds_min[0]);
            rect[1] = sdf_minf(rect[1], meshes[index]->bounds_min[2]);
            rect[2] = sdf_maxf(rect[2], meshes[index]->bounds_max[0]);
            rect[3] = sdf_maxf(rect[3], meshes[index]->bounds_max[2]);
        }
    }
    if (rect[0] > rect[2]) {
        sdf_chunk_xz_box(t, chunk_index, rect);
    }
    sdf_push_change(t, rect);
    t->mesh_cpu_bytes -= old != NULL ? old->bytes : 0u;
    t->mesh_cpu_bytes += next != NULL ? next->bytes : 0u;
    chunk->mesh = next;
    sdf_mesh_free(old);
    chunk->revision += 1u;
    if (!chunk->gpu_dirty && sdf_list_push(&t->gpu_dirty, chunk_index)) {
        chunk->gpu_dirty = 1u;
    }
    int32_t coords[3];
    sdf_chunk_coords(t, chunk_index, coords);
    column_dirty[(uint32_t)coords[2] * (uint32_t)t->chunks[0] + (uint32_t)coords[0]] = 1u;
}

static void sdf_mask_tile_update(BTBHGPUSdfTerrain *t, int32_t cx, int32_t cz, uint16_t *values) {
    const int32_t n = t->chunk_cells;
    const uint32_t tile_index = (uint32_t)cz * (uint32_t)t->chunks[0] + (uint32_t)cx;
    const int32_t x0 = cx * n;
    const int32_t z0 = cz * n;
    const int32_t width = sdf_mini(n, t->cells[0] - x0);
    const int32_t depth = sdf_mini(n, t->cells[2] - z0);
    if (width <= 0 || depth <= 0) {
        return;
    }
    uint32_t count = 0u;
    for (int32_t k = 0; k < n; ++k) {
        for (int32_t i = 0; i < n; ++i) {
            uint16_t value = (uint16_t)SDF_MASK_SENTINEL;
            if (i < width && k < depth) {
                for (int32_t cy = t->chunks[1] - 1; cy >= 0; --cy) {
                    const SdfMesh *mesh = t->chunk_array[sdf_chunk_index(t, cx, cy, cz)].mesh;
                    if (mesh == NULL || mesh->column_kind == NULL) {
                        continue;
                    }
                    const size_t column = (size_t)k * (size_t)n + (size_t)i;
                    const uint8_t kind = mesh->column_kind[column];
                    if (kind == SDF_COLUMN_NONE) {
                        continue;
                    }
                    if (kind == SDF_COLUMN_PATCH) {
                        value = sdf_half_round_down((mesh->column_height[column] - t->lattice_min[1]) / t->cell);
                        if (value == SDF_MASK_SENTINEL) {
                            value = (uint16_t)(SDF_MASK_SENTINEL - 1u);
                        }
                    }
                    break;
                }
            }
            values[(size_t)k * (size_t)n + (size_t)i] = value;
            if (value != SDF_MASK_SENTINEL) {
                count += 1u;
            }
        }
    }
    uint16_t *tile = t->mask_tiles[tile_index];
    if (count == 0u) {
        if (tile != NULL) {
            free(tile);
            t->mask_tiles[tile_index] = NULL;
            t->mask_tile_total -= 1u;
            t->mask_texel_total -= t->mask_tile_counts[tile_index];
            t->mask_tile_counts[tile_index] = 0u;
            t->mask_tile_dirty[tile_index] = 1u;
        }
        return;
    }
    const size_t bytes = (size_t)n * (size_t)n * sizeof(uint16_t);
    if (tile == NULL) {
        tile = (uint16_t *)malloc(bytes);
        if (tile == NULL) {
            return;
        }
        t->mask_tiles[tile_index] = tile;
        t->mask_tile_total += 1u;
        memcpy(tile, values, bytes);
        t->mask_tile_dirty[tile_index] = 1u;
    } else if (memcmp(tile, values, bytes) != 0) {
        memcpy(tile, values, bytes);
        t->mask_tile_dirty[tile_index] = 1u;
    }
    t->mask_texel_total -= t->mask_tile_counts[tile_index];
    t->mask_tile_counts[tile_index] = count;
    t->mask_texel_total += count;
}

/* Switches every group of touching busy chunks that waits for nothing (no stale brick, no mesh to build). */
static void sdf_switch_ready(BTBHGPUSdfTerrain *t) {
    if (t->busy.count == 0u) {
        return;
    }
    const uint32_t columns = (uint32_t)t->chunks[0] * (uint32_t)t->chunks[2];
    uint8_t *column_dirty = (uint8_t *)calloc(columns, 1u);
    if (column_dirty == NULL) {
        return;
    }
    for (uint32_t index = 0u; index < t->busy.count; ++index) {
        t->chunk_array[t->busy.items[index]].component_mark = 0u;
    }
    SdfU32List component = { NULL, 0u, 0u };
    for (uint32_t index = 0u; index < t->busy.count; ++index) {
        const uint32_t start = t->busy.items[index];
        SdfChunk *first = &t->chunk_array[start];
        if (!first->in_busy_list || first->component_mark) {
            continue;
        }
        component.count = 0u;
        first->component_mark = 1u;
        if (!sdf_list_push(&component, start)) {
            break;
        }
        bool ready = true;
        for (uint32_t head = 0u; head < component.count; ++head) {
            const uint32_t current = component.items[head];
            const SdfChunk *chunk = &t->chunk_array[current];
            if (chunk->stale_count != 0u || chunk->needs_mesh) {
                ready = false;
            }
            int32_t coords[3];
            sdf_chunk_coords(t, current, coords);
            for (int32_t dz = -1; dz <= 1; ++dz) {
                for (int32_t dy = -1; dy <= 1; ++dy) {
                    for (int32_t dx = -1; dx <= 1; ++dx) {
                        const int32_t x = coords[0] + dx;
                        const int32_t y = coords[1] + dy;
                        const int32_t z = coords[2] + dz;
                        if (x < 0 || y < 0 || z < 0 || x >= t->chunks[0] || y >= t->chunks[1] || z >= t->chunks[2]) {
                            continue;
                        }
                        const uint32_t neighbour = sdf_chunk_index(t, x, y, z);
                        SdfChunk *other = &t->chunk_array[neighbour];
                        if (other->in_busy_list && !other->component_mark) {
                            other->component_mark = 1u;
                            (void)sdf_list_push(&component, neighbour);
                        }
                    }
                }
            }
        }
        if (!ready) {
            continue;
        }
        for (uint32_t member = 0u; member < component.count; ++member) {
            sdf_switch_chunk(t, component.items[member], column_dirty);
            t->chunk_array[component.items[member]].in_busy_list = 0u;
        }
    }
    sdf_list_free(&component);
    uint32_t kept = 0u;
    for (uint32_t index = 0u; index < t->busy.count; ++index) {
        const uint32_t chunk_index = t->busy.items[index];
        if (t->chunk_array[chunk_index].in_busy_list) {
            t->busy.items[kept++] = chunk_index;
        } else {
            sdf_chunk_trim(t, &t->chunk_array[chunk_index]);
        }
    }
    t->busy.count = kept;
    uint16_t *values = (uint16_t *)malloc((size_t)t->chunk_cells * (size_t)t->chunk_cells * sizeof(uint16_t));
    if (values != NULL) {
        for (uint32_t column = 0u; column < columns; ++column) {
            if (column_dirty[column]) {
                sdf_mask_tile_update(
                    t,
                    (int32_t)(column % (uint32_t)t->chunks[0]),
                    (int32_t)(column / (uint32_t)t->chunks[0]),
                    values
                );
            }
        }
        free(values);
    }
    free(column_dirty);
}

/* ------------------------------------------------------------------------------------------------ jobs */

typedef struct SdfBrickBatch {
    const BTBHGPUSdfTerrain *terrain;
    SdfBrickEval *work;
} SdfBrickBatch;

static void sdf_brick_job(void *user, uint32_t index) {
    SdfBrickBatch *batch = (SdfBrickBatch *)user;
    sdf_brick_evaluate(batch->terrain, &batch->work[index]);
}

typedef struct SdfMeshBatch {
    const BTBHGPUSdfTerrain *terrain;
    const uint32_t *chunks;
    SdfMesh **results;
} SdfMeshBatch;

static void sdf_mesh_job(void *user, uint32_t index) {
    SdfMeshBatch *batch = (SdfMeshBatch *)user;
    batch->results[index] = sdf_mesh_chunk(batch->terrain, batch->chunks[index]);
}

static void sdf_run(BTBHGPUSdfTerrain *t, uint32_t count, void (*job)(void *, uint32_t), void *user) {
    if (count == 0u) {
        return;
    }
    if (t->desc.run_jobs != NULL && count > 1u) {
        t->desc.run_jobs(t->desc.jobs_user, count, job, user);
        return;
    }
    for (uint32_t index = 0u; index < count; ++index) {
        job(user, index);
    }
}

static bool sdf_chunk_meshable(const BTBHGPUSdfTerrain *t, uint32_t chunk_index) {
    int32_t coords[3];
    sdf_chunk_coords(t, chunk_index, coords);
    for (int32_t dz = -1; dz <= 1; ++dz) {
        for (int32_t dy = -1; dy <= 1; ++dy) {
            for (int32_t dx = -1; dx <= 1; ++dx) {
                const int32_t x = coords[0] + dx;
                const int32_t y = coords[1] + dy;
                const int32_t z = coords[2] + dz;
                if (x < 0 || y < 0 || z < 0 || x >= t->chunks[0] || y >= t->chunks[1] || z >= t->chunks[2]) {
                    continue;
                }
                if (t->chunk_array[sdf_chunk_index(t, x, y, z)].stale_count != 0u) {
                    return false;
                }
            }
        }
    }
    return true;
}

static uint32_t sdf_batch_size(double remaining_microseconds, double per_item, uint32_t limit) {
    if (remaining_microseconds < 0.0) {
        return limit;
    }
    double count = remaining_microseconds / (per_item > 1.0 ? per_item : 1.0);
    if (count < 1.0) {
        count = 1.0;
    }
    return count > (double)limit ? limit : (uint32_t)count;
}

/* Meshes up to `limit` chunks whose bricks (and their neighbours') are all fresh. Returns how many. */
static uint32_t sdf_mesh_batch(BTBHGPUSdfTerrain *t, uint32_t limit) {
    uint32_t chunks[16];
    SdfMesh *results[16];
    uint32_t count = 0u;
    if (limit > 16u) {
        limit = 16u;
    }
    for (uint32_t index = 0u; index < t->dirty.count && count < limit; ++index) {
        const uint32_t chunk_index = t->dirty.items[index];
        const SdfChunk *chunk = &t->chunk_array[chunk_index];
        if (chunk->in_dirty_list && chunk->needs_mesh && sdf_chunk_meshable(t, chunk_index)) {
            chunks[count++] = chunk_index;
        }
    }
    if (count == 0u) {
        return 0u;
    }
    SdfMeshBatch batch = { t, chunks, results };
    sdf_run(t, count, sdf_mesh_job, &batch);
    for (uint32_t index = 0u; index < count; ++index) {
        SdfChunk *chunk = &t->chunk_array[chunks[index]];
        sdf_mesh_free(chunk->pending);
        chunk->pending = results[index];
        chunk->has_pending = 1u;
        chunk->needs_mesh = 0u;
        chunk->in_dirty_list = 0u;
    }
    uint32_t kept = 0u;
    for (uint32_t index = 0u; index < t->dirty.count; ++index) {
        if (t->chunk_array[t->dirty.items[index]].in_dirty_list) {
            t->dirty.items[kept++] = t->dirty.items[index];
        }
    }
    t->dirty.count = kept;
    return count;
}

/* Evaluates up to `limit` stale bricks, oldest first. Returns how many. */
static uint32_t sdf_brick_batch(BTBHGPUSdfTerrain *t, uint32_t limit) {
    if (t->brick_work_capacity < limit) {
        SdfBrickEval *work = (SdfBrickEval *)realloc(t->brick_work, sizeof(SdfBrickEval) * limit);
        if (work == NULL) {
            limit = t->brick_work_capacity;
        } else {
            t->brick_work = work;
            t->brick_work_capacity = limit;
        }
    }
    if (limit == 0u) {
        return 0u;
    }
    uint32_t count = 0u;
    while (count < limit && t->stale_head < t->stale_tail) {
        const uint64_t entry = t->stale[t->stale_head++];
        const uint32_t chunk_index = (uint32_t)(entry >> 32u);
        const uint32_t slot = (uint32_t)(entry & 0xFFFFFFFFu);
        SdfChunk *chunk = &t->chunk_array[chunk_index];
        if (chunk->stale_bits == NULL || ((chunk->stale_bits[slot >> 6u] >> (slot & 63u)) & 1u) == 0u) {
            continue;
        }
        chunk->stale_bits[slot >> 6u] &= ~(UINT64_C(1) << (slot & 63u));
        SdfBrickEval *work = &t->brick_work[count++];
        work->chunk_index = chunk_index;
        work->slot = slot;
        work->result = 0u;
    }
    if (t->stale_head == t->stale_tail) {
        t->stale_head = 0u;
        t->stale_tail = 0u;
    }
    SdfBrickBatch batch = { t, t->brick_work };
    sdf_run(t, count, sdf_brick_job, &batch);
    for (uint32_t index = 0u; index < count; ++index) {
        sdf_brick_commit(t, &t->brick_work[index]);
    }
    return count;
}

/* ------------------------------------------------------------------------------------------------ GPU side */

static void sdf_gpu_vertices(const BTBHGPUSdfTerrain *t, const SdfMesh *mesh, float *out) {
    for (uint32_t v = 0u; v < mesh->vertex_count; ++v) {
        float *vertex = &out[v * SDF_GPU_VERTEX_FLOATS];
        const float *p = &mesh->positions[v * 3u];
        const float *n = &mesh->normals[v * 3u];
        const uint8_t material = mesh->materials[v];
        memset(vertex, 0, sizeof(float) * SDF_GPU_VERTEX_FLOATS);
        vertex[0] = p[0];
        vertex[1] = p[1];
        vertex[2] = p[2];
        vertex[3] = n[0];
        vertex[4] = n[1];
        vertex[5] = n[2];
        vertex[6] = p[0];
        vertex[7] = p[2];
        float tangent[3] = { 1.0f, 0.0f, 0.0f };
        if (fabsf(n[2]) < 0.999f) {
            /* cross((0, 0, 1), n), as the vertex shader's fallback */
            const float tx = -n[1];
            const float ty = n[0];
            const float length = sdf_length2(tx, ty);
            if (length > 1.0e-6f) {
                tangent[0] = tx / length;
                tangent[1] = ty / length;
                tangent[2] = 0.0f;
            }
        }
        vertex[8] = tangent[0];
        vertex[9] = tangent[1];
        vertex[10] = tangent[2];
        vertex[11] = 1.0f;
        const float *color = t->desc.material_colors[material];
        if (color[0] == 0.0f && color[1] == 0.0f && color[2] == 0.0f && color[3] == 0.0f) {
            vertex[12] = 1.0f;
            vertex[13] = 1.0f;
            vertex[14] = 1.0f;
            vertex[15] = 1.0f;
        } else {
            vertex[12] = color[0];
            vertex[13] = color[1];
            vertex[14] = color[2];
            vertex[15] = color[3];
        }
        /* Bytes 64..95: the weights of layers 0..7 (RFC 0001's vertex weights, phase 4). */
        vertex[16u + material] = 1.0f;
    }
}

static void sdf_gpu_release_chunk(BTBHGPUSdfTerrain *t, SdfChunk *chunk) {
    if (chunk->gpu_mesh != NULL) {
        btbh_gpu_custom_mesh_destroy(chunk->gpu_mesh);
        chunk->gpu_mesh = NULL;
    }
    t->mesh_gpu_bytes -= chunk->gpu_bytes;
    chunk->gpu_bytes = 0u;
}

static uint64_t sdf_mask_texture_bytes(const BTBHGPUSdfTerrain *t) {
    return t->mask_texture != NULL ? (uint64_t)t->cells[0] * (uint64_t)t->cells[2] * 2u : 0u;
}

static void sdf_gpu_release_mask(BTBHGPUSdfTerrain *t) {
    if (t->window == NULL || t->window->device == NULL || t->window->device->shutdown) {
        t->mask_texture = NULL;
        t->mask_sampler = NULL;
        return;
    }
    SDL_GPUDevice *device = t->window->device->gpu;
    if (t->mask_texture != NULL) {
        SDL_ReleaseGPUTexture(device, t->mask_texture);
        t->mask_texture = NULL;
    }
    if (t->mask_sampler != NULL) {
        SDL_ReleaseGPUSampler(device, t->mask_sampler);
        t->mask_sampler = NULL;
    }
}

static bool sdf_gpu_create_mask(BTBHGPUSdfTerrain *t) {
    SDL_GPUDevice *device = t->window->device->gpu;
    SDL_GPUTextureCreateInfo info;
    SDL_zero(info);
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R16_FLOAT;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    info.width = (Uint32)t->cells[0];
    info.height = (Uint32)t->cells[2];
    info.layer_count_or_depth = 1u;
    info.num_levels = 1u;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    t->mask_texture = SDL_CreateGPUTexture(device, &info);
    if (t->mask_texture == NULL) {
        return false;
    }
    SDL_SetGPUTextureName(device, t->mask_texture, "BTBH SDF terrain dig mask");
    if (t->mask_sampler == NULL) {
        SDL_GPUSamplerCreateInfo sampler;
        SDL_zero(sampler);
        sampler.min_filter = SDL_GPU_FILTER_NEAREST;
        sampler.mag_filter = SDL_GPU_FILTER_NEAREST;
        sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        sampler.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        sampler.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        t->mask_sampler = SDL_CreateGPUSampler(device, &sampler);
        if (t->mask_sampler == NULL) {
            SDL_ReleaseGPUTexture(device, t->mask_texture);
            t->mask_texture = NULL;
            return false;
        }
    }
    /* Everything starts as "the host's ground stays". */
    SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(device);
    if (command == NULL) {
        return false;
    }
    SDL_GPUColorTargetInfo target;
    SDL_zero(target);
    target.texture = t->mask_texture;
    target.load_op = SDL_GPU_LOADOP_CLEAR;
    target.store_op = SDL_GPU_STOREOP_STORE;
    target.clear_color.r = SDF_MASK_SENTINEL_VALUE;
    target.clear_color.g = 0.0f;
    target.clear_color.b = 0.0f;
    target.clear_color.a = 1.0f;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, &target, 1u, NULL);
    if (pass != NULL) {
        SDL_EndGPURenderPass(pass);
    }
    return SDL_SubmitGPUCommandBuffer(command);
}

static bool sdf_mask_tile_needs_upload(const BTBHGPUSdfTerrain *t, uint32_t tile) {
    return t->mask_tile_dirty[tile] || (t->mask_upload_all && t->mask_tiles[tile] != NULL);
}

static void sdf_gpu_upload_mask(BTBHGPUSdfTerrain *t) {
    SDL_GPUDevice *device = t->window->device->gpu;
    const uint32_t tiles = (uint32_t)t->chunks[0] * (uint32_t)t->chunks[2];
    const int32_t n = t->chunk_cells;
    uint64_t bytes = 0u;
    for (uint32_t tile = 0u; tile < tiles; ++tile) {
        if (sdf_mask_tile_needs_upload(t, tile)) {
            const int32_t x0 = (int32_t)(tile % (uint32_t)t->chunks[0]) * n;
            const int32_t z0 = (int32_t)(tile / (uint32_t)t->chunks[0]) * n;
            const int32_t width = sdf_mini(n, t->cells[0] - x0);
            const int32_t depth = sdf_mini(n, t->cells[2] - z0);
            if (width > 0 && depth > 0) {
                bytes += (uint64_t)width * (uint64_t)depth * 2u;
            }
        }
    }
    if (bytes == 0u) {
        t->mask_upload_all = 0u;
        memset(t->mask_tile_dirty, 0, tiles);
        return;
    }
    SDL_GPUTransferBufferCreateInfo transfer_info;
    SDL_zero(transfer_info);
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size = (Uint32)bytes;
    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(device, &transfer_info);
    if (transfer == NULL) {
        return;
    }
    uint8_t *mapped = (uint8_t *)SDL_MapGPUTransferBuffer(device, transfer, false);
    SDL_GPUCommandBuffer *command = mapped != NULL ? SDL_AcquireGPUCommandBuffer(device) : NULL;
    SDL_GPUCopyPass *copy = command != NULL ? SDL_BeginGPUCopyPass(command) : NULL;
    if (copy == NULL) {
        if (mapped != NULL) {
            SDL_UnmapGPUTransferBuffer(device, transfer);
        }
        if (command != NULL) {
            (void)SDL_CancelGPUCommandBuffer(command);
        }
        SDL_ReleaseGPUTransferBuffer(device, transfer);
        return;
    }
    uint64_t offset = 0u;
    for (uint32_t tile = 0u; tile < tiles; ++tile) {
        if (!sdf_mask_tile_needs_upload(t, tile)) {
            continue;
        }
        const int32_t x0 = (int32_t)(tile % (uint32_t)t->chunks[0]) * n;
        const int32_t z0 = (int32_t)(tile / (uint32_t)t->chunks[0]) * n;
        const int32_t width = sdf_mini(n, t->cells[0] - x0);
        const int32_t depth = sdf_mini(n, t->cells[2] - z0);
        if (width <= 0 || depth <= 0) {
            continue;
        }
        uint16_t *destination = (uint16_t *)(mapped + offset);
        for (int32_t k = 0; k < depth; ++k) {
            for (int32_t i = 0; i < width; ++i) {
                destination[(size_t)k * (size_t)width + (size_t)i] = t->mask_tiles[tile] != NULL
                    ? t->mask_tiles[tile][(size_t)k * (size_t)n + (size_t)i]
                    : (uint16_t)SDF_MASK_SENTINEL;
            }
        }
        SDL_GPUTextureTransferInfo source;
        SDL_zero(source);
        source.transfer_buffer = transfer;
        source.offset = (Uint32)offset;
        source.pixels_per_row = (Uint32)width;
        source.rows_per_layer = (Uint32)depth;
        SDL_GPUTextureRegion region;
        SDL_zero(region);
        region.texture = t->mask_texture;
        region.x = (Uint32)x0;
        region.y = (Uint32)z0;
        region.w = (Uint32)width;
        region.h = (Uint32)depth;
        region.d = 1u;
        SDL_UploadToGPUTexture(copy, &source, &region, false);
        offset += (uint64_t)width * (uint64_t)depth * 2u;
    }
    SDL_UnmapGPUTransferBuffer(device, transfer);
    SDL_EndGPUCopyPass(copy);
    (void)SDL_SubmitGPUCommandBuffer(command);
    SDL_ReleaseGPUTransferBuffer(device, transfer);
    t->mask_upload_all = 0u;
    memset(t->mask_tile_dirty, 0, tiles);
}

/* Uploads the chunks whose mesh switched and the mask tiles that changed (calling thread). */
static void sdf_gpu_sync(BTBHGPUSdfTerrain *t) {
    if (t->window == NULL || t->window->device == NULL || t->window->device->shutdown) {
        return;
    }
    for (uint32_t index = 0u; index < t->gpu_dirty.count; ++index) {
        SdfChunk *chunk = &t->chunk_array[t->gpu_dirty.items[index]];
        chunk->gpu_dirty = 0u;
        const SdfMesh *mesh = chunk->mesh;
        if (mesh == NULL || mesh->index_count == 0u) {
            sdf_gpu_release_chunk(t, chunk);
            continue;
        }
        const uint32_t floats = mesh->vertex_count * SDF_GPU_VERTEX_FLOATS;
        if (t->gpu_vertex_scratch_capacity < floats) {
            float *scratch = (float *)realloc(t->gpu_vertex_scratch, sizeof(float) * floats);
            if (scratch == NULL) {
                continue;
            }
            t->gpu_vertex_scratch = scratch;
            t->gpu_vertex_scratch_capacity = floats;
        }
        sdf_gpu_vertices(t, mesh, t->gpu_vertex_scratch);
        /* Faceted meshes are triangle lists already: no index buffer on the GPU. */
        const bool faceted = t->desc.normals == BTBH_GPU_SDF_NORMALS_FACETED;
        const uint32_t *indices = faceted ? NULL : mesh->indices;
        const uint32_t index_count = faceted ? 0u : mesh->index_count;
        BTBHGPUCustomMeshReport report;
        if (chunk->gpu_mesh != NULL) {
            if (btbh_gpu_custom_mesh_update(
                    chunk->gpu_mesh,
                    t->gpu_vertex_scratch,
                    floats * (uint32_t)sizeof(float),
                    indices,
                    index_count,
                    &report
                ) != BTBH_GPU_OK) {
                sdf_gpu_release_chunk(t, chunk);
            }
        } else {
            chunk->gpu_mesh = btbh_gpu_window_custom_mesh_create(
                t->window,
                t->gpu_vertex_scratch,
                floats * (uint32_t)sizeof(float),
                mesh->vertex_count,
                indices,
                index_count,
                &report
            );
        }
        t->mesh_gpu_bytes -= chunk->gpu_bytes;
        chunk->gpu_bytes = chunk->gpu_mesh != NULL
            ? (uint64_t)chunk->gpu_mesh->vertex_buffer_capacity + (uint64_t)chunk->gpu_mesh->index_buffer_capacity
            : 0u;
        t->mesh_gpu_bytes += chunk->gpu_bytes;
    }
    t->gpu_dirty.count = 0u;
    if (t->mask_texel_total > 0u) {
        if (t->mask_texture == NULL) {
            if (!sdf_gpu_create_mask(t)) {
                return;
            }
            t->mask_upload_all = 1u;
        }
        sdf_gpu_upload_mask(t);
    } else if (t->mask_texture != NULL) {
        sdf_gpu_release_mask(t);
    }
}

/* ------------------------------------------------------------------------------------------------ queries */

static bool sdf_column_edited(const BTBHGPUSdfTerrain *t, float x, float z) {
    if (x < t->lattice_min[0] || x > t->lattice_max[0] || z < t->lattice_min[2] || z > t->lattice_max[2]) {
        return false;
    }
    const int32_t cx = sdf_chunk_coord(t, 0, x);
    const int32_t cz = sdf_chunk_coord(t, 2, z);
    for (int32_t cy = 0; cy < t->chunks[1]; ++cy) {
        const SdfChunk *chunk = &t->chunk_array[sdf_chunk_index(t, cx, cy, cz)];
        if (chunk->is_frozen) {
            return true;
        }
        for (uint32_t index = 0u; index < chunk->edit_count; ++index) {
            const SdfEdit *edit = &t->edits[chunk->edits[index]];
            if (x >= edit->reach_min[0] && x <= edit->reach_max[0] &&
                z >= edit->reach_min[2] && z <= edit->reach_max[2]) {
                return true;
            }
        }
    }
    return false;
}

static float sdf_column_value(const BTBHGPUSdfTerrain *t, float x, float y, float z, bool base_only) {
    const float point[3] = { x, y, z };
    float distance = 0.0f;
    uint8_t material = 0u;
    if (base_only) {
        sdf_original_base(t, point, 1u, &distance, &material);
    } else {
        sdf_field_points(t, point, 1u, &distance, &material);
    }
    return distance;
}

/* The first crossing from outside to inside going down from from_y. Inside the lattice a step never exceeds
 * most of the reach margin, so it can't jump over a shape whose reach the point lies outside of. */
static float sdf_march_down(const BTBHGPUSdfTerrain *t, float x, float z, float from_y, bool base_only) {
    const float min_step = 0.05f * t->cell;
    const float max_step = 0.8f * SDF_REACH_MARGIN_CELLS * t->cell;
    const float floor_y = t->lattice_min[1] - sdf_maxf(64.0f * t->cell, t->lattice_max[1] - t->lattice_min[1]);
    float y = from_y;
    float f = sdf_column_value(t, x, y, z, base_only);
    for (uint32_t iteration = 0u; iteration < 8192u; ++iteration) {
        float step = sdf_maxf(fabsf(f) * 0.9f, min_step);
        const bool in_lattice_y = y <= t->lattice_max[1] && y >= t->lattice_min[1];
        if (!base_only && in_lattice_y) {
            step = sdf_minf(step, max_step);
        }
        float next_y = y - step;
        if (!base_only && y > t->lattice_max[1] && next_y < t->lattice_max[1]) {
            next_y = t->lattice_max[1];
        }
        const float next_f = sdf_column_value(t, x, next_y, z, base_only);
        if (f >= 0.0f && next_f < 0.0f) {
            /* False position (Illinois): exact at once where the field is linear (flat ground). */
            float outside_y = y;
            float outside_f = f;
            float inside_y = next_y;
            float inside_f = next_f;
            int side = 0;
            for (int refine = 0; refine < 40; ++refine) {
                if (outside_f == 0.0f) {
                    return outside_y;
                }
                float middle = outside_y - outside_f * (outside_y - inside_y) / (outside_f - inside_f);
                if (!(middle <= outside_y && middle >= inside_y)) {
                    middle = 0.5f * (outside_y + inside_y);
                }
                if (middle == outside_y || middle == inside_y) {
                    break;
                }
                const float value = sdf_column_value(t, x, middle, z, base_only);
                if (value < 0.0f) {
                    inside_y = middle;
                    inside_f = value;
                    if (side == -1) {
                        outside_f *= 0.5f;
                    }
                    side = -1;
                } else {
                    outside_y = middle;
                    outside_f = value;
                    if (side == 1) {
                        inside_f *= 0.5f;
                    }
                    side = 1;
                }
            }
            return outside_f == 0.0f ? outside_y : 0.5f * (outside_y + inside_y);
        }
        y = next_y;
        f = next_f;
        if (y < floor_y) {
            break;
        }
    }
    return -INFINITY;
}

static float sdf_original_base_height(const BTBHGPUSdfTerrain *t, float x, float z, float from_y) {
    if (t->heights != NULL) {
        float height = 0.0f;
        sdf_heightfield_at(t, x, z, &height, NULL, NULL);
        return height <= from_y ? height : -INFINITY;
    }
    return sdf_march_down(t, x, z, from_y, true);
}

/* ------------------------------------------------------------------------------------------------ API */

static void sdf_fill_report(const BTBHGPUSdfTerrain *t, BTBHGPUSdfTerrainReport *report) {
    if (report == NULL) {
        return;
    }
    memset(report, 0, sizeof(*report));
    report->result = BTBH_GPU_OK;
    report->bricks_evaluated = t->last_bricks_evaluated;
    report->chunks_meshed = t->last_chunks_meshed;
    report->pending_bricks = t->stale_total;
    report->brick_count = t->brick_total;
    report->edit_count = t->live_count;
    report->frozen_edits = t->frozen_total;
    report->mesh_bytes = t->mesh_cpu_bytes + t->mesh_gpu_bytes;
    report->mask_bytes = (uint64_t)t->mask_tile_total * (uint64_t)t->chunk_cells * (uint64_t)t->chunk_cells * 2u +
        sdf_mask_texture_bytes(t);
}

static bool sdf_desc_valid(const BTBHGPUSdfTerrainDesc *desc, const char **why) {
    for (int axis = 0; axis < 3; ++axis) {
        if (!sdf_finite(desc->bounds_min[axis]) || !sdf_finite(desc->bounds_max[axis]) ||
            !(desc->bounds_max[axis] > desc->bounds_min[axis])) {
            *why = "SDF terrain bounds must be finite with max > min";
            return false;
        }
    }
    if (!sdf_finite(desc->cell_size) || desc->cell_size < 0.1f || desc->cell_size > 4.0f) {
        *why = "SDF terrain cell_size must be 0.1..4";
        return false;
    }
    if (desc->chunk_cells != 0u && (desc->chunk_cells % 8u != 0u || desc->chunk_cells > 256u)) {
        *why = "SDF terrain chunk_cells must be a multiple of 8 up to 256 (0 for 64)";
        return false;
    }
    if (desc->max_edits > (1u << 20u)) {
        *why = "SDF terrain max_edits is at most 1048576";
        return false;
    }
    if (desc->base_surface_material > 7u || desc->base_subsurface_material > 7u ||
        desc->normals > (uint8_t)BTBH_GPU_SDF_NORMALS_FACETED ||
        desc->edit_cap > (uint8_t)BTBH_GPU_SDF_EDIT_CAP_REJECT ||
        !sdf_finite(desc->base_subsurface_depth)) {
        *why = "SDF terrain materials are 0..7; normals and edit_cap must be known values";
        return false;
    }
    if (desc->base_heights != NULL) {
        if (desc->base_width == 0u || desc->base_depth == 0u || desc->base_width > 65536u ||
            desc->base_depth > 65536u || !sdf_finite(desc->base_cell) || !(desc->base_cell > 0.0f)) {
            *why = "SDF terrain heightfield needs base_width, base_depth >= 1 and base_cell > 0";
            return false;
        }
        const size_t count = ((size_t)desc->base_width + 1u) * ((size_t)desc->base_depth + 1u);
        for (size_t index = 0u; index < count; ++index) {
            if (!sdf_finite(desc->base_heights[index])) {
                *why = "SDF terrain base heights must be finite";
                return false;
            }
        }
    } else if (desc->base_function == NULL) {
        *why = "SDF terrain needs base_heights or base_function";
        return false;
    }
    for (int material = 0; material < 8; ++material) {
        for (int channel = 0; channel < 4; ++channel) {
            if (!sdf_finite(desc->material_colors[material][channel])) {
                *why = "SDF terrain material colours must be finite";
                return false;
            }
        }
    }
    for (int axis = 0; axis < 3; ++axis) {
        const float cells = ceilf((desc->bounds_max[axis] - desc->bounds_min[axis]) / desc->cell_size);
        if (!(cells <= (float)SDF_MAX_LATTICE_CELLS)) {
            *why = "SDF terrain bounds span more than 16384 cells along an axis";
            return false;
        }
    }
    return true;
}

BTBHGPUSdfTerrain *btbh_gpu_sdf_terrain_create(const BTBHGPUSdfTerrainDesc *desc, BTBHGPUSdfTerrainReport *report) {
    if (report != NULL) {
        memset(report, 0, sizeof(*report));
    }
    const char *why = "SDF terrain desc is NULL";
    if (desc == NULL || !sdf_desc_valid(desc, &why)) {
        sdf_report_message(report, BTBH_GPU_INVALID_ARGUMENT, why);
        return NULL;
    }
    BTBHGPUSdfTerrain *t = (BTBHGPUSdfTerrain *)calloc(1u, sizeof(BTBHGPUSdfTerrain));
    if (t == NULL) {
        sdf_report_message(report, BTBH_GPU_PLATFORM_FAILURE, "Could not allocate the SDF terrain");
        return NULL;
    }
    t->desc = *desc;
    t->desc.base_heights = NULL;
    t->cell = desc->cell_size;
    t->clamp_distance = 0.86602540f * t->cell;
    t->chunk_cells = desc->chunk_cells != 0u ? (int32_t)desc->chunk_cells : (int32_t)SDF_DEFAULT_CHUNK_CELLS;
    t->bricks_per_chunk = t->chunk_cells / SDF_BRICK;
    t->slots_per_chunk = (uint32_t)(t->bricks_per_chunk * t->bricks_per_chunk * t->bricks_per_chunk);
    t->max_edits = desc->max_edits != 0u ? desc->max_edits : SDF_DEFAULT_MAX_EDITS;
    uint64_t chunk_total = 1u;
    for (int axis = 0; axis < 3; ++axis) {
        t->lattice_min[axis] = desc->bounds_min[axis];
        t->cells[axis] = sdf_maxi(1, (int32_t)ceilf((desc->bounds_max[axis] - desc->bounds_min[axis]) / t->cell));
        t->lattice_max[axis] = sdf_lattice_coord(t, axis, t->cells[axis]);
        t->chunks[axis] = t->cells[axis] / t->chunk_cells + 1;
        chunk_total *= (uint64_t)t->chunks[axis];
    }
    t->chunk_total = (uint32_t)chunk_total;
    t->chunk_array = (SdfChunk *)calloc(t->chunk_total, sizeof(SdfChunk));
    const uint32_t tiles = (uint32_t)t->chunks[0] * (uint32_t)t->chunks[2];
    t->mask_tiles = (uint16_t **)calloc(tiles, sizeof(uint16_t *));
    t->mask_tile_counts = (uint32_t *)calloc(tiles, sizeof(uint32_t));
    t->mask_tile_dirty = (uint8_t *)calloc(tiles, 1u);
    bool ok = t->chunk_array != NULL && t->mask_tiles != NULL && t->mask_tile_counts != NULL &&
        t->mask_tile_dirty != NULL;
    if (ok && desc->base_heights != NULL) {
        const size_t count = ((size_t)desc->base_width + 1u) * ((size_t)desc->base_depth + 1u);
        t->heights = (float *)malloc(sizeof(float) * count);
        ok = t->heights != NULL;
        if (ok) {
            memcpy(t->heights, desc->base_heights, sizeof(float) * count);
            t->desc.base_function = NULL;
        }
    }
    if (!ok) {
        btbh_gpu_sdf_terrain_destroy(t);
        sdf_report_message(report, BTBH_GPU_PLATFORM_FAILURE, "Could not allocate the SDF terrain");
        return NULL;
    }
    t->next_id = 1u;
    t->brick_microseconds = 50.0;
    t->chunk_microseconds = 500.0;
    sdf_fill_report(t, report);
    return t;
}

void btbh_gpu_sdf_terrain_detach(BTBHGPUSdfTerrain *terrain) {
    if (terrain == NULL || terrain->window == NULL) {
        return;
    }
    BTBHGPUSdfTerrain *t = terrain;
    for (uint32_t index = 0u; index < t->chunk_total; ++index) {
        SdfChunk *chunk = &t->chunk_array[index];
        sdf_gpu_release_chunk(t, chunk);
        chunk->gpu_dirty = 0u;
    }
    t->gpu_dirty.count = 0u;
    t->mesh_gpu_bytes = 0u;
    sdf_gpu_release_mask(t);
    BTBHGPUWindow *window = t->window;
    if (t->window_previous != NULL) {
        t->window_previous->window_next = t->window_next;
    } else if (window->sdf_terrain_head == t) {
        window->sdf_terrain_head = t->window_next;
    }
    if (t->window_next != NULL) {
        t->window_next->window_previous = t->window_previous;
    }
    t->window_next = NULL;
    t->window_previous = NULL;
    t->window = NULL;
}

void btbh_gpu_window_detach_sdf_terrains(BTBHGPUWindow *window) {
    if (window == NULL) {
        return;
    }
    while (window->sdf_terrain_head != NULL) {
        btbh_gpu_sdf_terrain_detach(window->sdf_terrain_head);
    }
}

BTBHGPUResult btbh_gpu_window_sdf_terrain_attach(
    BTBHGPUWindow *window,
    BTBHGPUSdfTerrain *terrain,
    BTBHGPUSdfTerrainReport *report
) {
    if (report != NULL) {
        memset(report, 0, sizeof(*report));
    }
    if (window == NULL || terrain == NULL || window->device == NULL || window->device->gpu == NULL ||
        window->device->shutdown || !window->claimed) {
        sdf_report_message(report, BTBH_GPU_INVALID_ARGUMENT, "SDF terrain attach needs a terrain and a claimed window");
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    if (terrain->window != window) {
        btbh_gpu_sdf_terrain_detach(terrain);
        terrain->window = window;
        terrain->window_next = window->sdf_terrain_head;
        if (window->sdf_terrain_head != NULL) {
            window->sdf_terrain_head->window_previous = terrain;
        }
        window->sdf_terrain_head = terrain;
        for (uint32_t index = 0u; index < terrain->chunk_total; ++index) {
            SdfChunk *chunk = &terrain->chunk_array[index];
            if (chunk->mesh != NULL && !chunk->gpu_dirty && sdf_list_push(&terrain->gpu_dirty, index)) {
                chunk->gpu_dirty = 1u;
            }
        }
        terrain->mask_upload_all = 1u;
    }
    sdf_gpu_sync(terrain);
    sdf_fill_report(terrain, report);
    return BTBH_GPU_OK;
}

void btbh_gpu_sdf_terrain_destroy(BTBHGPUSdfTerrain *terrain) {
    if (terrain == NULL) {
        return;
    }
    BTBHGPUSdfTerrain *t = terrain;
    btbh_gpu_sdf_terrain_detach(t);
    if (t->chunk_array != NULL) {
        for (uint32_t index = 0u; index < t->chunk_total; ++index) {
            SdfChunk *chunk = &t->chunk_array[index];
            free(chunk->edits);
            if (chunk->bricks != NULL) {
                for (uint32_t slot = 0u; slot < t->slots_per_chunk; ++slot) {
                    sdf_slot_release(t, chunk->bricks[slot]);
                }
            }
            if (chunk->frozen != NULL) {
                for (uint32_t slot = 0u; slot < t->slots_per_chunk; ++slot) {
                    sdf_frozen_slot_release(t, chunk->frozen[slot]);
                }
            }
            free(chunk->bricks);
            free(chunk->stale_bits);
            free(chunk->frozen);
            sdf_mesh_free(chunk->mesh);
            sdf_mesh_free(chunk->pending);
        }
        free(t->chunk_array);
    }
    for (uint32_t slab = 0u; slab < t->slab_count; ++slab) {
        free(t->slabs[slab]);
    }
    free(t->slabs);
    if (t->mask_tiles != NULL) {
        const uint32_t tiles = (uint32_t)t->chunks[0] * (uint32_t)t->chunks[2];
        for (uint32_t tile = 0u; tile < tiles; ++tile) {
            free(t->mask_tiles[tile]);
        }
    }
    free(t->mask_tiles);
    free(t->mask_tile_counts);
    free(t->mask_tile_dirty);
    free(t->heights);
    free(t->edits);
    sdf_list_free(&t->free_edit_slots);
    sdf_list_free(&t->order);
    sdf_list_free(&t->dirty);
    sdf_list_free(&t->busy);
    sdf_list_free(&t->gpu_dirty);
    free(t->stale);
    free(t->changes);
    free(t->gpu_vertex_scratch);
    free(t->brick_work);
    free(t);
}

uint32_t btbh_gpu_sdf_terrain_add_edit(BTBHGPUSdfTerrain *terrain, const BTBHGPUSdfEdit *edit) {
    if (terrain == NULL || !sdf_edit_validate(edit)) {
        return 0u;
    }
    BTBHGPUSdfTerrain *t = terrain;
    if (t->next_id == UINT32_MAX) {
        return 0u;
    }
    if (t->live_count >= t->max_edits) {
        if (t->desc.edit_cap == BTBH_GPU_SDF_EDIT_CAP_REJECT) {
            return 0u;
        }
        while (t->live_count >= t->max_edits) {
            const uint32_t before = t->live_count;
            sdf_freeze_oldest(t);
            if (t->live_count == before) {
                return 0u;
            }
        }
    }
    const uint32_t slot = sdf_edit_slot_allocate(t);
    if (slot == UINT32_MAX) {
        return 0u;
    }
    SdfEdit *record = &t->edits[slot];
    memset(record, 0, sizeof(*record));
    sdf_edit_prepare(t, edit, record);
    record->id = t->next_id;
    record->state = SDF_EDIT_LIVE;
    if (!sdf_list_push(&t->order, slot)) {
        record->state = SDF_EDIT_FREE;
        record->id = 0u;
        (void)sdf_list_push(&t->free_edit_slots, slot);
        return 0u;
    }
    t->next_id += 1u;
    t->live_count += 1u;
    if (!sdf_edit_link(t, slot)) {
        const SdfEdit copy = *record;
        sdf_edit_unlink(t, slot, &copy);
        t->live_count -= 1u;
        sdf_edit_release(t, slot);
        return 0u;
    }
    sdf_mark_box_stale(t, record->reach_min, record->reach_max);
    return record->id;
}

BTBHGPUResult btbh_gpu_sdf_terrain_set_edit(BTBHGPUSdfTerrain *terrain, uint32_t id, const BTBHGPUSdfEdit *edit) {
    if (terrain == NULL || !sdf_edit_validate(edit)) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    BTBHGPUSdfTerrain *t = terrain;
    const uint32_t slot = sdf_live_slot(t, id);
    if (slot == UINT32_MAX) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    const SdfEdit old = t->edits[slot];
    sdf_edit_unlink(t, slot, &old);
    SdfEdit *record = &t->edits[slot];
    sdf_edit_prepare(t, edit, record);
    record->id = old.id;
    record->state = SDF_EDIT_LIVE;
    record->chunk_refs = 0u;
    if (!sdf_edit_link(t, slot)) {
        const SdfEdit copy = *record;
        sdf_edit_unlink(t, slot, &copy);
        sdf_mark_box_stale(t, old.reach_min, old.reach_max);
        t->live_count -= 1u;
        sdf_edit_release(t, slot);
        return BTBH_GPU_PLATFORM_FAILURE;
    }
    sdf_mark_box_stale(t, old.reach_min, old.reach_max);
    sdf_mark_box_stale(t, record->reach_min, record->reach_max);
    return BTBH_GPU_OK;
}

BTBHGPUResult btbh_gpu_sdf_terrain_remove_edit(BTBHGPUSdfTerrain *terrain, uint32_t id) {
    if (terrain == NULL) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    BTBHGPUSdfTerrain *t = terrain;
    const uint32_t slot = sdf_live_slot(t, id);
    if (slot == UINT32_MAX) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    const SdfEdit old = t->edits[slot];
    sdf_edit_unlink(t, slot, &old);
    t->live_count -= 1u;
    sdf_edit_release(t, slot);
    sdf_mark_box_stale(t, old.reach_min, old.reach_max);
    return BTBH_GPU_OK;
}

BTBHGPUResult btbh_gpu_sdf_terrain_update(
    BTBHGPUSdfTerrain *terrain,
    uint32_t budget_microseconds,
    BTBHGPUSdfTerrainReport *report
) {
    if (terrain == NULL) {
        if (report != NULL) {
            memset(report, 0, sizeof(*report));
        }
        sdf_report_message(report, BTBH_GPU_INVALID_ARGUMENT, "SDF terrain is NULL");
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    BTBHGPUSdfTerrain *t = terrain;
    const uint64_t started = sdf_now_ticks();
    uint32_t bricks = 0u;
    uint32_t chunks = 0u;
    for (;;) {
        const double elapsed = (double)(sdf_now_ticks() - started) / 1000.0;
        const double remaining = budget_microseconds != 0u ? (double)budget_microseconds - elapsed : -1.0;
        if (budget_microseconds != 0u && remaining <= 0.0) {
            break;
        }
        const uint64_t batch_started = sdf_now_ticks();
        uint32_t done = sdf_mesh_batch(t, sdf_batch_size(remaining, t->chunk_microseconds, 8u));
        if (done > 0u) {
            chunks += done;
            const double each = (double)(sdf_now_ticks() - batch_started) / 1000.0 / (double)done;
            t->chunk_microseconds = 0.75 * t->chunk_microseconds + 0.25 * each;
            continue;
        }
        done = sdf_brick_batch(t, sdf_batch_size(remaining, t->brick_microseconds, 64u));
        if (done == 0u) {
            break;
        }
        bricks += done;
        const double each = (double)(sdf_now_ticks() - batch_started) / 1000.0 / (double)done;
        t->brick_microseconds = 0.75 * t->brick_microseconds + 0.25 * each;
    }
    sdf_switch_ready(t);
    sdf_gpu_sync(t);
    t->last_bricks_evaluated = bricks;
    t->last_chunks_meshed = chunks;
    sdf_fill_report(t, report);
    return BTBH_GPU_OK;
}

float btbh_gpu_sdf_terrain_distance(const BTBHGPUSdfTerrain *terrain, const float point[3]) {
    if (terrain == NULL || point == NULL) {
        return INFINITY;
    }
    return sdf_field_at(terrain, point[0], point[1], point[2]);
}

float btbh_gpu_sdf_terrain_height(const BTBHGPUSdfTerrain *terrain, float x, float z, float from_y) {
    if (terrain == NULL || !sdf_finite(x) || !sdf_finite(z) || !sdf_finite(from_y)) {
        return -INFINITY;
    }
    if (!sdf_column_edited(terrain, x, z)) {
        return sdf_original_base_height(terrain, x, z, from_y);
    }
    return sdf_march_down(terrain, x, z, from_y, false);
}

uint8_t btbh_gpu_sdf_terrain_raycast(
    const BTBHGPUSdfTerrain *terrain,
    const float origin[3],
    const float direction[3],
    float max_distance,
    float hit[3]
) {
    if (terrain == NULL || origin == NULL || direction == NULL || !(max_distance > 0.0f) ||
        !sdf_finite(max_distance)) {
        return 0u;
    }
    const BTBHGPUSdfTerrain *t = terrain;
    const float length = sdf_length3(direction[0], direction[1], direction[2]);
    if (!(length > 0.0f) || !sdf_finite(length)) {
        return 0u;
    }
    const float d[3] = { direction[0] / length, direction[1] / length, direction[2] / length };
    const float min_step = 0.05f * t->cell;
    const float max_step = 0.8f * SDF_REACH_MARGIN_CELLS * t->cell;
    float travelled = 0.0f;
    float f = sdf_field_at(t, origin[0], origin[1], origin[2]);
    if (f < 0.0f) {
        if (hit != NULL) {
            memcpy(hit, origin, sizeof(float) * 3u);
        }
        return 1u;
    }
    /* Where the ray is inside the lattice (the box edits live in): steps there stay within the reach margin. */
    float enter = 0.0f;
    float leave = FLT_MAX;
    for (int axis = 0; axis < 3; ++axis) {
        if (fabsf(d[axis]) < 1.0e-12f) {
            if (origin[axis] < t->lattice_min[axis] || origin[axis] > t->lattice_max[axis]) {
                enter = FLT_MAX;
            }
            continue;
        }
        float a = (t->lattice_min[axis] - origin[axis]) / d[axis];
        float b = (t->lattice_max[axis] - origin[axis]) / d[axis];
        if (a > b) {
            const float swap = a;
            a = b;
            b = swap;
        }
        enter = sdf_maxf(enter, a);
        leave = sdf_minf(leave, b);
    }
    const bool crosses_lattice = enter <= leave;
    for (uint32_t iteration = 0u; iteration < 16384u; ++iteration) {
        float step = sdf_maxf(f * 0.8f, min_step);
        const bool inside_lattice = crosses_lattice && travelled >= enter && travelled <= leave;
        if (inside_lattice) {
            step = sdf_minf(step, max_step);
        } else if (crosses_lattice && travelled < enter && travelled + step > enter) {
            step = sdf_maxf(enter - travelled, min_step);
        }
        float next = travelled + step;
        if (next > max_distance) {
            next = max_distance;
        }
        const float next_f = sdf_field_at(t, origin[0] + d[0] * next, origin[1] + d[1] * next, origin[2] + d[2] * next);
        if (next_f < 0.0f) {
            float outside = travelled;
            float inside = next;
            for (int bisect = 0; bisect < 24; ++bisect) {
                const float middle = 0.5f * (outside + inside);
                if (sdf_field_at(t, origin[0] + d[0] * middle, origin[1] + d[1] * middle, origin[2] + d[2] * middle) <
                    0.0f) {
                    inside = middle;
                } else {
                    outside = middle;
                }
            }
            const float at = 0.5f * (outside + inside);
            if (hit != NULL) {
                hit[0] = origin[0] + d[0] * at;
                hit[1] = origin[1] + d[1] * at;
                hit[2] = origin[2] + d[2] * at;
            }
            return 1u;
        }
        if (next >= max_distance) {
            return 0u;
        }
        travelled = next;
        f = next_f;
    }
    return 0u;
}

BTBHGPUResult btbh_gpu_sdf_terrain_sample_height_deltas(
    const BTBHGPUSdfTerrain *terrain,
    float origin_x,
    float origin_z,
    float cell,
    uint32_t width,
    uint32_t depth,
    float *out
) {
    if (terrain == NULL || out == NULL || width == 0u || depth == 0u || !(cell > 0.0f) || !sdf_finite(cell) ||
        !sdf_finite(origin_x) || !sdf_finite(origin_z)) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    const BTBHGPUSdfTerrain *t = terrain;
    const float from_y = t->lattice_max[1] + t->cell;
    for (uint32_t k = 0u; k < depth; ++k) {
        for (uint32_t i = 0u; i < width; ++i) {
            const float x = origin_x + ((float)i + 0.5f) * cell;
            const float z = origin_z + ((float)k + 0.5f) * cell;
            float delta = 0.0f;
            if (sdf_column_edited(t, x, z)) {
                float top = sdf_march_down(t, x, z, from_y, false);
                float base = sdf_original_base_height(t, x, z, from_y);
                if (top == -INFINITY) {
                    top = t->lattice_min[1];
                }
                if (base == -INFINITY) {
                    base = t->lattice_min[1];
                }
                delta = top - base;
            }
            out[(size_t)k * width + i] = delta;
        }
    }
    return BTBH_GPU_OK;
}

uint32_t btbh_gpu_sdf_terrain_take_changes(BTBHGPUSdfTerrain *terrain, float (*rects)[4], uint32_t capacity) {
    if (terrain == NULL || rects == NULL) {
        return 0u;
    }
    uint32_t count = 0u;
    while (count < capacity && terrain->change_count > 0u) {
        memcpy(rects[count], terrain->changes[terrain->change_head], sizeof(float) * 4u);
        terrain->change_head += 1u;
        terrain->change_count -= 1u;
        count += 1u;
    }
    if (terrain->change_count == 0u) {
        terrain->change_head = 0u;
    }
    return count;
}

uint32_t btbh_gpu_sdf_terrain_chunk_count(const BTBHGPUSdfTerrain *terrain) {
    return terrain != NULL ? terrain->chunk_total : 0u;
}

uint8_t btbh_gpu_sdf_terrain_chunk(const BTBHGPUSdfTerrain *terrain, uint32_t index, BTBHGPUSdfTerrainChunk *chunk) {
    if (terrain == NULL || chunk == NULL || index >= terrain->chunk_total) {
        return 0u;
    }
    const SdfChunk *source = &terrain->chunk_array[index];
    int32_t coords[3];
    sdf_chunk_coords(terrain, index, coords);
    memset(chunk, 0, sizeof(*chunk));
    chunk->mesh = source->gpu_mesh;
    for (int axis = 0; axis < 3; ++axis) {
        chunk->bounds_min[axis] = sdf_lattice_coord(terrain, axis, coords[axis] * terrain->chunk_cells);
        chunk->bounds_max[axis] = sdf_lattice_coord(
            terrain,
            axis,
            sdf_mini((coords[axis] + 1) * terrain->chunk_cells, terrain->cells[axis])
        );
    }
    if (source->mesh != NULL && source->mesh->vertex_count > 0u) {
        /* A patch also has vertices one cell under the chunk's low faces (the seams it shares). */
        for (int axis = 0; axis < 3; ++axis) {
            chunk->bounds_min[axis] = sdf_minf(chunk->bounds_min[axis], source->mesh->bounds_min[axis]);
            chunk->bounds_max[axis] = sdf_maxf(chunk->bounds_max[axis], source->mesh->bounds_max[axis]);
        }
    }
    chunk->revision = source->revision;
    return 1u;
}

BTBHGPUResult btbh_gpu_sdf_terrain_chunk_geometry(
    const BTBHGPUSdfTerrain *terrain,
    uint32_t index,
    const float **positions,
    uint32_t *vertex_count,
    const uint32_t **indices,
    uint32_t *index_count
) {
    if (terrain == NULL || index >= terrain->chunk_total || positions == NULL || vertex_count == NULL ||
        indices == NULL || index_count == NULL) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    const SdfMesh *mesh = terrain->chunk_array[index].mesh;
    if (mesh == NULL || mesh->index_count == 0u) {
        *positions = NULL;
        *vertex_count = 0u;
        *indices = NULL;
        *index_count = 0u;
        return BTBH_GPU_OK;
    }
    *positions = mesh->positions;
    *vertex_count = mesh->vertex_count;
    *indices = mesh->indices;
    *index_count = mesh->index_count;
    return BTBH_GPU_OK;
}

/* ------------------------------------------------------------------------------------------------ internal */

bool btbh_gpu_sdf_terrain_mask_binding(
    const BTBHGPUSdfTerrain *terrain,
    const BTBHGPUWindow *window,
    BTBHGPUSdfTerrainMaskBinding *binding
) {
    if (terrain == NULL || window == NULL || terrain->window != window || terrain->mask_texture == NULL ||
        terrain->mask_sampler == NULL) {
        return false;
    }
    binding->texture = terrain->mask_texture;
    binding->sampler = terrain->mask_sampler;
    binding->origin[0] = terrain->lattice_min[0];
    binding->origin[1] = terrain->lattice_min[2];
    binding->origin[2] = terrain->lattice_min[1];
    binding->origin[3] = terrain->cell;
    binding->scale[0] = 1.0f / ((float)terrain->cells[0] * terrain->cell);
    binding->scale[1] = 1.0f / ((float)terrain->cells[2] * terrain->cell);
    binding->scale[2] = 1.0f / terrain->cell;
    binding->scale[3] = SDF_MASK_THRESHOLD_CELLS;
    return true;
}

float btbh_gpu_sdf_terrain_mask_texel(const BTBHGPUSdfTerrain *terrain, uint32_t x, uint32_t z) {
    if (terrain == NULL || x >= (uint32_t)terrain->cells[0] || z >= (uint32_t)terrain->cells[2]) {
        return SDF_MASK_SENTINEL_VALUE;
    }
    const uint32_t n = (uint32_t)terrain->chunk_cells;
    const uint16_t *tile = terrain->mask_tiles[(z / n) * (uint32_t)terrain->chunks[0] + x / n];
    if (tile == NULL) {
        return SDF_MASK_SENTINEL_VALUE;
    }
    return sdf_half_to_float(tile[(size_t)(z % n) * n + (x % n)]);
}

uint64_t btbh_gpu_sdf_terrain_mask_texel_count(const BTBHGPUSdfTerrain *terrain) {
    return terrain != NULL ? terrain->mask_texel_total : 0u;
}

uint64_t btbh_gpu_sdf_terrain_mesh_hash(const BTBHGPUSdfTerrain *terrain) {
    uint64_t hash = UINT64_C(1469598103934665603);
    if (terrain == NULL) {
        return hash;
    }
    for (uint32_t index = 0u; index < terrain->chunk_total; ++index) {
        const SdfMesh *mesh = terrain->chunk_array[index].mesh;
        const uint64_t chunk_hash = mesh != NULL ? mesh->hash : 0u;
        hash = sdf_hash_bytes(hash, &index, sizeof(index));
        hash = sdf_hash_bytes(hash, &chunk_hash, sizeof(chunk_hash));
    }
    return hash;
}

const uint8_t *btbh_gpu_sdf_terrain_chunk_materials(const BTBHGPUSdfTerrain *terrain, uint32_t index) {
    if (terrain == NULL || index >= terrain->chunk_total || terrain->chunk_array[index].mesh == NULL) {
        return NULL;
    }
    return terrain->chunk_array[index].mesh->materials;
}

const float *btbh_gpu_sdf_terrain_chunk_normals(const BTBHGPUSdfTerrain *terrain, uint32_t index) {
    if (terrain == NULL || index >= terrain->chunk_total || terrain->chunk_array[index].mesh == NULL) {
        return NULL;
    }
    return terrain->chunk_array[index].mesh->normals;
}

bool btbh_gpu_sdf_terrain_sample(
    const BTBHGPUSdfTerrain *terrain,
    int32_t x,
    int32_t y,
    int32_t z,
    int8_t *distance,
    uint8_t *material
) {
    uint8_t differs = 0u;
    return terrain != NULL && sdf_read_live_sample(terrain, x, y, z, distance, material, &differs);
}
