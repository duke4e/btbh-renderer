/* The opaque pass's front-to-back order (BTBHGPUScenePost.opaque_sort, btbh_gpu_scene_opaque_order) and the
 * batch flags' layout, without a GPU: which batches sort, which keep their place, and the depth they sort by.
 * Also which procedural detail parameters the present accepts (btbh_gpu_scene_detail_parameters_valid): the
 * patterns' names, and RFC 0009's pattern 7 with x, y and z at 0. And the layouts of RFC 0009 phase 2's
 * world-detail table, its entry and the batch it is appended to. And RFC 0011's run key
 * (btbh_gpu_scene_batch_continues_run): every byte of the batch flipped in turn, which fields break a run and which
 * don't, and the shared-mesh flag's constants. */
#undef NDEBUG

#include "btbh_gpu_internal.h"

#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The released layouts (0.4.0, 64-bit): the new fields took reserved bytes. */
#if UINTPTR_MAX == UINT64_MAX
/* 0.4.0 was 256 bytes; RFC 0007 appends dug_by after the reserved bytes. */
_Static_assert(offsetof(BTBHGPUSceneGeometryBatch, dug_by) == 256u, "dug_by is appended after 0.4.0's 256 bytes");
_Static_assert(offsetof(BTBHGPUSceneGeometryBatch, ssao_mode) == 252u, "ssao_mode keeps its place");
_Static_assert(offsetof(BTBHGPUSceneGeometryBatch, flags) == 253u, "flags is the old ssao_reserved[0]");
_Static_assert(offsetof(BTBHGPUSceneGeometryBatch, ssao_reserved) == 254u, "the rest stays reserved");
/* RFC 0008 appends world_detail after 0.5.0's 264 bytes; dug_by keeps its place. */
_Static_assert(offsetof(BTBHGPUSceneGeometryBatch, world_detail) == 264u, "world_detail is appended at 264");
/* RFC 0009 phase 2 (0.8.0) appends world_detail_table after 0.6.0's and 0.7.0's 272 bytes. */
_Static_assert(offsetof(BTBHGPUSceneGeometryBatch, world_detail_table) == 272u,
    "world_detail_table is appended at 272");
_Static_assert(sizeof(BTBHGPUSceneGeometryBatch) == 280u, "the batch grows from 272 to 280 bytes");
_Static_assert(sizeof(BTBHGPUSceneWorldDetailTable) == 24u, "BTBHGPUSceneWorldDetailTable is 24 bytes");
_Static_assert(offsetof(BTBHGPUSceneWorldDetailTable, map_layers) == 0u, "map_layers");
_Static_assert(offsetof(BTBHGPUSceneWorldDetailTable, entries) == 8u, "entries");
_Static_assert(offsetof(BTBHGPUSceneWorldDetailTable, entry_count) == 16u, "entry_count");
_Static_assert(offsetof(BTBHGPUSceneWorldDetailTable, seed_component) == 20u, "seed_component");
_Static_assert(offsetof(BTBHGPUSceneWorldDetailTable, flags) == 21u, "flags");
_Static_assert(offsetof(BTBHGPUSceneWorldDetailTable, reserved) == 22u, "reserved");
#endif
/* On any target: the entry is 36 bytes of floats and bytes, aligned to 4, and its eight floats are
   BTBHGPUSceneWorldDetail's, at that struct's offsets minus its three pointers (24 bytes on 64-bit). */
_Static_assert(sizeof(BTBHGPUSceneWorldDetailTableEntry) == 36u, "BTBHGPUSceneWorldDetailTableEntry is 36 bytes");
_Static_assert(_Alignof(BTBHGPUSceneWorldDetailTableEntry) == 4u, "the entry is aligned to 4");
#define BTBH_ENTRY_AT_DETAIL(field) \
    (offsetof(BTBHGPUSceneWorldDetailTableEntry, field) == \
        offsetof(BTBHGPUSceneWorldDetail, field) - offsetof(BTBHGPUSceneWorldDetail, level_tile))
_Static_assert(BTBH_ENTRY_AT_DETAIL(level_tile) && offsetof(BTBHGPUSceneWorldDetailTableEntry, level_tile) == 0u,
    "level_tile");
_Static_assert(BTBH_ENTRY_AT_DETAIL(wall_tile) && offsetof(BTBHGPUSceneWorldDetailTableEntry, wall_tile) == 8u,
    "wall_tile");
_Static_assert(BTBH_ENTRY_AT_DETAIL(albedo_scale) && offsetof(BTBHGPUSceneWorldDetailTableEntry, albedo_scale) == 16u,
    "albedo_scale");
_Static_assert(BTBH_ENTRY_AT_DETAIL(normal_scale) && offsetof(BTBHGPUSceneWorldDetailTableEntry, normal_scale) == 20u,
    "normal_scale");
_Static_assert(BTBH_ENTRY_AT_DETAIL(roughness_scale) &&
    offsetof(BTBHGPUSceneWorldDetailTableEntry, roughness_scale) == 24u, "roughness_scale");
_Static_assert(BTBH_ENTRY_AT_DETAIL(level_degrees) &&
    offsetof(BTBHGPUSceneWorldDetailTableEntry, level_degrees) == 28u, "level_degrees");
#undef BTBH_ENTRY_AT_DETAIL
_Static_assert(offsetof(BTBHGPUSceneWorldDetailTableEntry, normal_layer) == 32u, "normal_layer");
_Static_assert(offsetof(BTBHGPUSceneWorldDetailTableEntry, orm_layer) == 33u, "orm_layer");
_Static_assert(offsetof(BTBHGPUSceneWorldDetailTableEntry, flags) == 34u, "flags");
_Static_assert(offsetof(BTBHGPUSceneWorldDetailTableEntry, reserved) == 35u, "reserved");
/* The table's pointer follows world_detail; its constants and the vertex's index float. */
_Static_assert(
    offsetof(BTBHGPUSceneGeometryBatch, world_detail_table) ==
        offsetof(BTBHGPUSceneGeometryBatch, world_detail) + sizeof(void *),
    "world_detail_table follows world_detail"
);
_Static_assert(BTBH_GPU_SCENE_WORLD_DETAIL_TABLE_MAX_ENTRIES == 8u, "eight entries (RFC 0009, decision 1)");
_Static_assert(BTBH_GPU_SCENE_WORLD_DETAIL_NO_LAYER == 255u, "255 is none (decision 5)");
_Static_assert(BTBH_GPU_CUSTOM_MESH_VERTEX_FLOATS == 24u, "the vertex is 24 floats");
_Static_assert(BTBH_GPU_CUSTOM_MESH_DETAIL_INDEX_FLOAT * sizeof(float) == 64u, "the index is joints.x, byte 64");
_Static_assert(BTBH_GPU_TEXTURE_ARRAY_MAX_LAYERS == 256u, "Vulkan's guaranteed maxImageArrayLayers");
#if !defined(BTBH_GPU_SCENE_HAS_WORLD_DETAIL_TABLE) || !defined(BTBH_GPU_HAS_TEXTURE_ARRAYS)
#error "BTBH_GPU_SCENE_HAS_WORLD_DETAIL_TABLE and BTBH_GPU_HAS_TEXTURE_ARRAYS are defined with the table"
#endif
#if UINTPTR_MAX == UINT64_MAX
_Static_assert(sizeof(BTBHGPUSceneWorldDetail) == 64u, "BTBHGPUSceneWorldDetail is 64 bytes");
_Static_assert(offsetof(BTBHGPUSceneWorldDetail, albedo_texture) == 0u, "albedo_texture");
_Static_assert(offsetof(BTBHGPUSceneWorldDetail, normal_texture) == 8u, "normal_texture");
_Static_assert(offsetof(BTBHGPUSceneWorldDetail, orm_texture) == 16u, "orm_texture");
_Static_assert(offsetof(BTBHGPUSceneWorldDetail, level_tile) == 24u, "level_tile");
_Static_assert(offsetof(BTBHGPUSceneWorldDetail, wall_tile) == 32u, "wall_tile");
_Static_assert(offsetof(BTBHGPUSceneWorldDetail, albedo_scale) == 40u, "albedo_scale");
_Static_assert(offsetof(BTBHGPUSceneWorldDetail, normal_scale) == 44u, "normal_scale");
_Static_assert(offsetof(BTBHGPUSceneWorldDetail, roughness_scale) == 48u, "roughness_scale");
_Static_assert(offsetof(BTBHGPUSceneWorldDetail, level_degrees) == 52u, "level_degrees");
_Static_assert(offsetof(BTBHGPUSceneWorldDetail, seed_component) == 56u, "seed_component");
_Static_assert(offsetof(BTBHGPUSceneWorldDetail, flags) == 57u, "flags");
_Static_assert(offsetof(BTBHGPUSceneWorldDetail, reserved) == 58u, "reserved");
#endif
/* On any target: the pointer follows dug_by, and the packed flag is bit 0. */
_Static_assert(
    offsetof(BTBHGPUSceneGeometryBatch, world_detail) ==
        offsetof(BTBHGPUSceneGeometryBatch, dug_by) + sizeof(void *),
    "world_detail follows dug_by"
);
_Static_assert(BTBH_GPU_SCENE_WORLD_DETAIL_ALBEDO_IN_ORM_BLUE == 1u, "the packed layout is bit 0");
#if !defined(BTBH_GPU_SCENE_HAS_WORLD_DETAIL)
#error "BTBH_GPU_SCENE_HAS_WORLD_DETAIL is defined with the world detail"
#endif
/* RFC 0009 phase 1: the patterns' names, and pattern 7, with no struct changed. */
_Static_assert(BTBH_GPU_SCENE_DETAIL_NONE == 0 && BTBH_GPU_SCENE_DETAIL_GROUND == 1 &&
    BTBH_GPU_SCENE_DETAIL_GRASS == 2 && BTBH_GPU_SCENE_DETAIL_FACADE == 3 && BTBH_GPU_SCENE_DETAIL_PAVING == 4 &&
    BTBH_GPU_SCENE_DETAIL_GLASS == 5 && BTBH_GPU_SCENE_DETAIL_LAMP_LENS == 6, "the patterns keep their numbers");
_Static_assert(BTBH_GPU_SCENE_DETAIL_FACADE_WEATHER == 7, "the facade's weather is pattern 7");
#if !defined(BTBH_GPU_SCENE_HAS_FACADE_WEATHER)
#error "BTBH_GPU_SCENE_HAS_FACADE_WEATHER is defined with pattern 7"
#endif
#if UINTPTR_MAX == UINT64_MAX
_Static_assert(sizeof(BTBHGPUScenePost) == 432u, "BTBHGPUScenePost keeps its size");
_Static_assert(offsetof(BTBHGPUScenePost, ssao_debug_view) == 425u, "ssao_debug_view keeps its place");
_Static_assert(offsetof(BTBHGPUScenePost, opaque_sort) == 426u, "opaque_sort is the old ssao_reserved[0]");
_Static_assert(offsetof(BTBHGPUScenePost, flags) == 427u, "flags is the old ssao_reserved[0]");
#endif
/* 0.9.0: a batch flag and the post flags, in bytes that were there. */
_Static_assert(BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW == 1 && BTBH_GPU_SCENE_BATCH_KEEP_ORDER == 2 &&
    BTBH_GPU_SCENE_BATCH_NO_RC_SOURCE == 4, "the batch flags keep their bits");
_Static_assert(BTBH_GPU_SCENE_POST_ADDITIVE_CASCADES_KEEP_HBAO == 1 && BTBH_GPU_SCENE_POST_SSR_SURFACE_REFLECTIVITY == 2,
    "the post flags' bits");
_Static_assert(sizeof(((BTBHGPUScenePost *)0)->flags) == 1u, "the post flags are a byte");
#if !defined(BTBH_GPU_SCENE_HAS_BATCH_NO_RC_SOURCE) || !defined(BTBH_GPU_SCENE_HAS_ADDITIVE_CASCADES_KEEP_HBAO) ||     !defined(BTBH_GPU_SCENE_HAS_SSR_SURFACE_REFLECTIVITY) || !defined(BTBH_GPU_HAS_SCENE_RENDER_TO_TARGET_PREWARM)
#error "0.9.0's HAS_ macros are defined with their features"
#endif

/* RFC 0011 (0.10.0): the shared-mesh flag and its macro. */
_Static_assert(BTBH_GPU_CUSTOM_MESH_SHARED_BUFFERS == 1u, "the shared-buffers flag is bit 0");
#if !defined(BTBH_GPU_HAS_SHARED_MESH_BUFFERS)
#error "BTBH_GPU_HAS_SHARED_MESH_BUFFERS is defined with btbh_gpu_window_custom_mesh_create_ex"
#endif

/* 0.10.0 (Be The Bullet Hell v2's findings and its kaiju shadow): a post flag, a batch flag and the spot occluder's
   flags, in bits and bytes that were there. Batch bits 3 and 4 stay free for RFC 0013's draft. */
_Static_assert(BTBH_GPU_SCENE_POST_SSR_FACING_HITS == 4, "SSR_FACING_HITS is post bit 2");
_Static_assert(BTBH_GPU_SCENE_BATCH_RECEIVES_NO_SPOT_SHADOW == 32, "RECEIVES_NO_SPOT_SHADOW is batch bit 5");
_Static_assert(BTBH_GPU_SCENE_SPOT_OCCLUDER_SUN_ONLY == 1 && BTBH_GPU_SCENE_SPOT_OCCLUDER_ORTHOGRAPHIC == 2,
    "the spot occluder's flags' bits");
_Static_assert(sizeof(BTBHGPUSceneLighting) == 3340u, "BTBHGPUSceneLighting keeps its size");
_Static_assert(offsetof(BTBHGPUSceneLighting, directional_shadow_filter) == 3336u, "the shadow filter keeps its place");
_Static_assert(offsetof(BTBHGPUSceneLighting, spot_occluder_flags) == 3338u,
    "spot_occluder_flags is the old directional_shadow_reserved[1]");
#if !defined(BTBH_GPU_SCENE_HAS_SSR_FACING_HITS) || !defined(BTBH_GPU_SCENE_HAS_BATCH_RECEIVES_NO_SPOT_SHADOW) || \
    !defined(BTBH_GPU_SCENE_HAS_SPOT_OCCLUDER_FLAGS)
#error "0.10.0's HAS_ macros are defined with their features"
#endif

#define MAX_INSTANCES 32u

static float instances[MAX_INSTANCES][24];
static uint32_t instance_count;

/* An instance at (x, y, z), scaled by s; returns its index. */
static uint32_t add_instance(float x, float y, float z, float s) {
    assert(instance_count < MAX_INSTANCES);
    float *record = instances[instance_count];
    memset(record, 0, 24u * sizeof(float));
    record[0] = s;
    record[5] = s;
    record[10] = s;
    record[12] = x;
    record[13] = y;
    record[14] = z;
    record[15] = 1.0f;
    return instance_count++;
}

static BTBHGPUModel model;
static BTBHGPUCustomMesh mesh;

/* An opaque, sortable model batch (depth LESS, no stencil) over `count` instances from `first`. */
static BTBHGPUSceneGeometryBatch opaque(uint32_t first, uint32_t count) {
    BTBHGPUSceneGeometryBatch batch;
    memset(&batch, 0, sizeof(batch));
    batch.kind = BTBH_GPU_SCENE_GEOMETRY_MODEL;
    batch.model = &model;
    batch.material_slot = -1;
    batch.first_instance = first;
    batch.instance_count = count;
    batch.depth_compare = BTBH_GPU_SCENE_DEPTH_LESS;
    batch.stencil_pass = BTBH_GPU_SCENE_STENCIL_REPLACE;
    return batch;
}

/* A camera at the origin looking down -z (right-handed, depth 0..1): clip w is the distance in front. */
static void perspective(float view_projection[16]) {
    const float near_plane = 0.1f;
    const float far_plane = 100.0f;
    const float r = far_plane / (near_plane - far_plane);
    const float matrix[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, r, -1.0f,
        0.0f, 0.0f, r * near_plane, 0.0f,
    };
    memcpy(view_projection, matrix, sizeof(matrix));
}

/* The same view, orthographic: clip w is 1, clip z grows with the distance. */
static void orthographic(float view_projection[16]) {
    const float range = 1.0f / (0.1f - 100.0f);
    const float matrix[16] = {
        0.1f, 0.0f, 0.0f, 0.0f,
        0.0f, 0.1f, 0.0f, 0.0f,
        0.0f, 0.0f, range, 0.0f,
        0.0f, 0.0f, range * 0.1f, 1.0f,
    };
    memcpy(view_projection, matrix, sizeof(matrix));
}

/* FRONT_TO_BACK_LESS_EQUAL for order_of: LESS_EQUAL batches move too. */
static bool move_less_equal;

static uint32_t order_of(
    const BTBHGPUSceneGeometryBatch *batches,
    const uint8_t *visibility,
    uint32_t count,
    const float view_projection[16],
    uint32_t *order
) {
    uint64_t scratch[64];
    assert(count * 2u <= 64u);
    for (uint32_t index = 0u; index < count; ++index) {
        order[index] = UINT32_MAX;
    }
    return btbh_gpu_scene_opaque_order(
        batches,
        visibility,
        count,
        &instances[0][0],
        view_projection,
        move_less_equal,
        order,
        scratch
    );
}

static void assert_order(const uint32_t *order, uint32_t count, const uint32_t *expected, uint32_t expected_count) {
    if (count != expected_count || memcmp(order, expected, sizeof(uint32_t) * count) != 0) {
        fprintf(stderr, "order:");
        for (uint32_t index = 0u; index < count; ++index) {
            fprintf(stderr, " %u", order[index]);
        }
        fprintf(stderr, " (expected");
        for (uint32_t index = 0u; index < expected_count; ++index) {
            fprintf(stderr, " %u", expected[index]);
        }
        fprintf(stderr, ")\n");
    }
    assert(count == expected_count);
    assert(memcmp(order, expected, sizeof(uint32_t) * count) == 0);
}

/* ---- RFC 0011: the run key ---- */

/* Every field of the batch in order, and whether a batch that differs from the one before it only there continues
   its run: its geometry (kind, model, custom_mesh, material_slot) and its instance range (first_instance,
   instance_count) may differ; nothing else may. */
typedef struct RunField {
    const char *name;
    size_t offset;
    size_t size;
    bool continues;
} RunField;

#define RUN_FIELD(field, continues) \
    { #field, offsetof(BTBHGPUSceneGeometryBatch, field), sizeof(((BTBHGPUSceneGeometryBatch *)0)->field), continues }
static const RunField run_fields[] = {
    RUN_FIELD(kind, true),
    RUN_FIELD(model, true),
    RUN_FIELD(custom_mesh, true),
    RUN_FIELD(animation_library, false),
    RUN_FIELD(animation_pose, false),
    RUN_FIELD(animation_index, false),
    RUN_FIELD(animation_time, false),
    RUN_FIELD(material_slot, true),
    RUN_FIELD(instance_buffer, false),
    RUN_FIELD(first_instance, true),
    RUN_FIELD(instance_count, true),
    RUN_FIELD(albedo_texture, false),
    RUN_FIELD(albedo_red, false),
    RUN_FIELD(albedo_green, false),
    RUN_FIELD(albedo_blue, false),
    RUN_FIELD(albedo_alpha, false),
    RUN_FIELD(uv_scale_x, false),
    RUN_FIELD(uv_scale_y, false),
    RUN_FIELD(uv_offset_x, false),
    RUN_FIELD(uv_offset_y, false),
    RUN_FIELD(normal_texture, false),
    RUN_FIELD(orm_texture, false),
    RUN_FIELD(normal_scale, false),
    RUN_FIELD(occlusion, false),
    RUN_FIELD(roughness, false),
    RUN_FIELD(metalness, false),
    RUN_FIELD(emission_texture, false),
    RUN_FIELD(emission_red, false),
    RUN_FIELD(emission_green, false),
    RUN_FIELD(emission_blue, false),
    RUN_FIELD(emission_alpha, false),
    RUN_FIELD(emission_energy, false),
    RUN_FIELD(blend_mode, false),
    RUN_FIELD(billboard_mode, false),
    RUN_FIELD(depth_compare, false),
    RUN_FIELD(alpha_cutoff, false),
    RUN_FIELD(stencil_compare, false),
    RUN_FIELD(cull_mode, false),
    RUN_FIELD(stencil_fail, false),
    RUN_FIELD(stencil_depth_fail, false),
    RUN_FIELD(stencil_pass, false),
    RUN_FIELD(stencil_reference, false),
    RUN_FIELD(stencil_mask, false),
    RUN_FIELD(transparency_enabled, false),
    RUN_FIELD(unlit_enabled, false),
    RUN_FIELD(spot_shadow_visible, false),
    RUN_FIELD(rc_receiver, false),
    RUN_FIELD(reactive_mode, false),
    RUN_FIELD(surface_variant, false),
    RUN_FIELD(surface_time, false),
    RUN_FIELD(surface_parameters0, false),
    RUN_FIELD(surface_parameters1, false),
    RUN_FIELD(detail_parameters, false),
    RUN_FIELD(ssao_mode, false),
    RUN_FIELD(flags, false),
    RUN_FIELD(ssao_reserved, false),
    RUN_FIELD(dug_by, false),
    RUN_FIELD(world_detail, false),
    RUN_FIELD(world_detail_table, false),
};
#undef RUN_FIELD
#define RUN_FIELD_COUNT (sizeof(run_fields) / sizeof(run_fields[0]))

/* A batch with something in every field (the pointers are never followed). */
static BTBHGPUSceneGeometryBatch run_batch(void) {
    BTBHGPUSceneGeometryBatch batch;
    memset(&batch, 0, sizeof(batch));
    uint8_t *bytes = (uint8_t *)&batch;
    for (size_t field = 0u; field < RUN_FIELD_COUNT; ++field) {
        for (size_t byte = 0u; byte < run_fields[field].size; ++byte) {
            bytes[run_fields[field].offset + byte] = (uint8_t)(17u + 31u * field + 7u * byte);
        }
    }
    return batch;
}

static void check_run_key(void) {
    /* The fields cover the batch, in order, with only padding (less than a pointer) between them and none after. */
    size_t covered = 0u;
    size_t padding = 0u;
    size_t end = 0u;
    for (size_t field = 0u; field < RUN_FIELD_COUNT; ++field) {
        assert(run_fields[field].offset >= end);
        assert(run_fields[field].offset - end < sizeof(void *));
        padding += run_fields[field].offset - end;
        covered += run_fields[field].size;
        end = run_fields[field].offset + run_fields[field].size;
    }
    assert(end == sizeof(BTBHGPUSceneGeometryBatch));
    assert(covered + padding == sizeof(BTBHGPUSceneGeometryBatch));

    const BTBHGPUSceneGeometryBatch base = run_batch();
    assert(btbh_gpu_scene_batch_continues_run(&base, &base));
    BTBHGPUSceneGeometryBatch copy = base;
    assert(btbh_gpu_scene_batch_continues_run(&base, &copy));

    /* Every byte flipped in turn, either way round. */
    uint32_t breaking = 0u;
    uint32_t keeping = 0u;
    for (size_t byte = 0u; byte < sizeof(BTBHGPUSceneGeometryBatch); ++byte) {
        bool expected = true;   /* padding */
        const char *name = "padding";
        for (size_t field = 0u; field < RUN_FIELD_COUNT; ++field) {
            if (byte >= run_fields[field].offset && byte < run_fields[field].offset + run_fields[field].size) {
                expected = run_fields[field].continues;
                name = run_fields[field].name;
            }
        }
        BTBHGPUSceneGeometryBatch flipped = base;
        ((uint8_t *)&flipped)[byte] ^= 0xa5u;
        const bool forward = btbh_gpu_scene_batch_continues_run(&base, &flipped);
        const bool backward = btbh_gpu_scene_batch_continues_run(&flipped, &base);
        if (forward != expected || backward != expected) {
            fprintf(stderr, "run key: byte %u (%s): %s, expected %s\n", (unsigned)byte, name,
                forward ? "continues" : "breaks", expected ? "continues" : "breaks");
        }
        assert(forward == expected && backward == expected);
        if (expected) {
            keeping += 1u;
        } else {
            breaking += 1u;
        }
    }

    /* All six at once still continue; any one other field with them breaks. */
    BTBHGPUSceneGeometryBatch moved = base;
    moved.kind = BTBH_GPU_SCENE_GEOMETRY_MODEL;
    moved.model = (BTBHGPUModel *)(uintptr_t)0x1234u;
    moved.custom_mesh = NULL;
    moved.material_slot = 3;
    moved.first_instance = 900u;
    moved.instance_count = 77u;
    assert(btbh_gpu_scene_batch_continues_run(&base, &moved));
    moved.stencil_reference ^= 1u;
    assert(!btbh_gpu_scene_batch_continues_run(&base, &moved));

    /* Floats compare by their bits: -0 and +0 differ (conservatively), the same NaN is the same. */
    BTBHGPUSceneGeometryBatch zero = base;
    BTBHGPUSceneGeometryBatch negative_zero = base;
    zero.roughness = 0.0f;
    negative_zero.roughness = -0.0f;
    assert(!btbh_gpu_scene_batch_continues_run(&zero, &negative_zero));
    zero.roughness = NAN;
    negative_zero.roughness = zero.roughness;
    assert(btbh_gpu_scene_batch_continues_run(&zero, &negative_zero));
    printf("run key: %u fields over %u bytes (%u of padding); %u bytes break a run, %u keep it (the six fields "
        "and padding)\n", (unsigned)RUN_FIELD_COUNT, (unsigned)sizeof(BTBHGPUSceneGeometryBatch), (unsigned)padding,
        breaking, keeping);
}

int main(void) {
    memset(&model, 0, sizeof(model));
    model.minimum[0] = -1.0f;
    model.minimum[1] = -1.0f;
    model.minimum[2] = -1.0f;
    model.maximum[0] = 1.0f;
    model.maximum[1] = 1.0f;
    model.maximum[2] = 1.0f;
    memset(&mesh, 0, sizeof(mesh));

    float view_projection[16];
    perspective(view_projection);
    uint32_t order[16];

    /* Far to near, as given: drawn near to far. Off to the side doesn't matter, only the depth. */
    const uint32_t far = add_instance(0.0f, 0.0f, -30.0f, 1.0f);
    const uint32_t middle = add_instance(25.0f, 0.0f, -20.0f, 1.0f);
    const uint32_t near = add_instance(0.0f, -3.0f, -10.0f, 1.0f);
    {
        const BTBHGPUSceneGeometryBatch batches[3] = {
            opaque(far, 1u), opaque(middle, 1u), opaque(near, 1u),
        };
        const uint32_t expected[3] = {2u, 1u, 0u};
        assert_order(order, order_of(batches, NULL, 3u, view_projection, order), expected, 3u);
    }

    /* Equal depths keep the order given (stable), behind and ahead of others. */
    const uint32_t near_twin = add_instance(-4.0f, 2.0f, -10.0f, 1.0f);
    {
        const BTBHGPUSceneGeometryBatch batches[4] = {
            opaque(far, 1u), opaque(near_twin, 1u), opaque(middle, 1u), opaque(near, 1u),
        };
        const uint32_t expected[4] = {1u, 3u, 2u, 0u};
        assert_order(order, order_of(batches, NULL, 4u, view_projection, order), expected, 4u);
    }

    /* A batch sorts by its instances' mean depth: 10 and 30 make 20, between 15 and 25 (its nearest
       instance would sort it first, its farthest last). */
    {
        const uint32_t pair = add_instance(0.0f, 0.0f, -30.0f, 2.0f);
        const uint32_t pair_near = add_instance(0.0f, 0.0f, -10.0f, 0.5f);
        assert(pair_near == pair + 1u);
        const uint32_t at15 = add_instance(0.0f, 0.0f, -15.0f, 1.0f);
        const uint32_t at25 = add_instance(0.0f, 0.0f, -25.0f, 1.0f);
        const BTBHGPUSceneGeometryBatch batches[3] = {
            opaque(at25, 1u), opaque(pair, 2u), opaque(at15, 1u),
        };
        const uint32_t expected[3] = {2u, 1u, 0u};
        assert_order(order, order_of(batches, NULL, 3u, view_projection, order), expected, 3u);
    }

    /* The geometry's bounds count, not just the instance's origin: a model centred 25 units deeper. */
    {
        BTBHGPUModel deep = model;
        deep.minimum[2] = -26.0f;
        deep.maximum[2] = -24.0f;
        BTBHGPUSceneGeometryBatch batches[2] = {opaque(near, 1u), opaque(middle, 1u)};
        batches[0].model = &deep; /* centre at z = -35 */
        const uint32_t expected[2] = {1u, 0u};
        assert_order(order, order_of(batches, NULL, 2u, view_projection, order), expected, 2u);
    }

    /* A custom mesh sorts by its vertices' bounds; without valid bounds it keeps its place. */
    {
        mesh.minimum[0] = -1.0f;
        mesh.minimum[1] = -1.0f;
        mesh.minimum[2] = -1.0f;
        mesh.maximum[0] = 1.0f;
        mesh.maximum[1] = 1.0f;
        mesh.maximum[2] = 1.0f;
        mesh.bounds_valid = true;
        BTBHGPUSceneGeometryBatch batches[3] = {opaque(far, 1u), opaque(middle, 1u), opaque(near, 1u)};
        batches[0].kind = BTBH_GPU_SCENE_GEOMETRY_CUSTOM_MESH;
        batches[0].model = NULL;
        batches[0].custom_mesh = &mesh;
        const uint32_t sorted[3] = {2u, 1u, 0u};
        assert_order(order, order_of(batches, NULL, 3u, view_projection, order), sorted, 3u);
        mesh.bounds_valid = false;
        const uint32_t kept[3] = {0u, 2u, 1u};
        assert_order(order, order_of(batches, NULL, 3u, view_projection, order), kept, 3u);
    }

    /* Transparent, refracting and hidden batches aren't in the opaque pass at all; they don't split runs. */
    {
        BTBHGPUSceneGeometryBatch batches[6] = {
            opaque(far, 1u), opaque(middle, 1u), opaque(near, 1u),
            opaque(near, 1u), opaque(near, 1u), opaque(near, 1u),
        };
        batches[1].transparency_enabled = 1u;
        batches[3].blend_mode = BTBH_GPU_SCENE_BLEND_ADDITIVE;
        batches[4].transparency_enabled = 1u;
        batches[4].surface_variant = BTBH_GPU_SCENE_SURFACE_REFRACT;
        const uint8_t visibility[6] = {1u, 1u, 1u, 1u, 1u, 0u};
        const uint32_t expected[2] = {2u, 0u};
        assert_order(order, order_of(batches, visibility, 6u, view_projection, order), expected, 2u);
    }

    /* Batches whose picture depends on what was drawn before keep their place, and nothing crosses them:
       depth compares other than LESS, stencil writes and tests, KEEP_ORDER, persistent instances. */
    {
        BTBHGPUInstanceBuffer persistent;
        memset(&persistent, 0, sizeof(persistent));
        for (uint32_t pin = 0u; pin < 8u; ++pin) {
            /* far, near, [pinned far], far, near: each run sorts on its own. */
            BTBHGPUSceneGeometryBatch batches[5] = {
                opaque(far, 1u), opaque(near, 1u), opaque(far, 1u), opaque(far, 1u), opaque(near, 1u),
            };
            BTBHGPUSceneGeometryBatch *pinned = &batches[2];
            switch (pin) {
                case 0u: pinned->depth_compare = BTBH_GPU_SCENE_DEPTH_ALWAYS; break;
                case 1u: pinned->depth_compare = BTBH_GPU_SCENE_DEPTH_LESS_EQUAL; break;
                case 2u: pinned->depth_compare = BTBH_GPU_SCENE_DEPTH_GREATER; break;
                case 3u: pinned->stencil_reference = 1u; break; /* writes its reference */
                case 4u:
                    pinned->stencil_reference = 1u;
                    pinned->stencil_mask = 0xffu;
                    pinned->stencil_compare = BTBH_GPU_SCENE_DEPTH_NOT_EQUAL;
                    pinned->stencil_pass = BTBH_GPU_SCENE_STENCIL_KEEP;
                    break;
                case 5u: pinned->flags = BTBH_GPU_SCENE_BATCH_KEEP_ORDER; break;
                case 6u: pinned->instance_buffer = &persistent; pinned->first_instance = 0u; break;
                default: pinned->flags = BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW; break; /* sorts */
            }
            const uint32_t kept[5] = {1u, 0u, 2u, 4u, 3u};
            const uint32_t sorted[5] = {1u, 4u, 0u, 2u, 3u};
            assert_order(
                order,
                order_of(batches, NULL, 5u, view_projection, order),
                pin < 7u ? kept : sorted,
                5u
            );
        }
    }

    /* FRONT_TO_BACK_LESS_EQUAL: LESS_EQUAL batches sort too, together with LESS ones; KEEP_ORDER and the
       other compares still keep their place. */
    {
        /* far, [LESS_EQUAL near], [LESS_EQUAL far], far, near */
        BTBHGPUSceneGeometryBatch batches[5] = {
            opaque(far, 1u), opaque(near, 1u), opaque(far, 1u), opaque(far, 1u), opaque(near, 1u),
        };
        batches[1].depth_compare = BTBH_GPU_SCENE_DEPTH_LESS_EQUAL;
        batches[2].depth_compare = BTBH_GPU_SCENE_DEPTH_LESS_EQUAL;
        const uint32_t kept[5] = {0u, 1u, 2u, 4u, 3u};
        assert_order(order, order_of(batches, NULL, 5u, view_projection, order), kept, 5u);
        move_less_equal = true;
        const uint32_t sorted[5] = {1u, 4u, 0u, 2u, 3u};
        assert_order(order, order_of(batches, NULL, 5u, view_projection, order), sorted, 5u);
        batches[2].flags = BTBH_GPU_SCENE_BATCH_KEEP_ORDER;
        const uint32_t pinned[5] = {1u, 0u, 2u, 4u, 3u};
        assert_order(order, order_of(batches, NULL, 5u, view_projection, order), pinned, 5u);
        batches[2].flags = 0u;
        batches[2].depth_compare = BTBH_GPU_SCENE_DEPTH_GREATER;
        assert_order(order, order_of(batches, NULL, 5u, view_projection, order), pinned, 5u);
        move_less_equal = false;
    }

    /* Behind the camera (negative depth) comes first; an orthographic view sorts by clip z. */
    {
        const uint32_t behind = add_instance(0.0f, 0.0f, 5.0f, 1.0f);
        const BTBHGPUSceneGeometryBatch batches[4] = {
            opaque(far, 1u), opaque(near, 1u), opaque(behind, 1u), opaque(middle, 1u),
        };
        const uint32_t expected[4] = {2u, 1u, 3u, 0u};
        assert_order(order, order_of(batches, NULL, 4u, view_projection, order), expected, 4u);
        float ortho[16];
        orthographic(ortho);
        assert_order(order, order_of(batches, NULL, 4u, ortho, order), expected, 4u);
    }

    /* Many batches (the merge sort past its insertion runs): ascending, ties in the order given. */
    {
        enum { MANY = 40 };
        BTBHGPUSceneGeometryBatch batches[MANY];
        const uint32_t firsts[3] = {far, middle, near};
        for (uint32_t index = 0u; index < MANY; ++index) {
            batches[index] = opaque(firsts[(index * 7u) % 3u], 1u);
        }
        uint32_t many_order[MANY];
        uint64_t scratch[2 * MANY];
        const uint32_t count = btbh_gpu_scene_opaque_order(
            batches, NULL, MANY, &instances[0][0], view_projection, false, many_order, scratch);
        assert(count == MANY);
        for (uint32_t index = 1u; index < MANY; ++index) {
            const uint32_t previous = many_order[index - 1u];
            const uint32_t current = many_order[index];
            const float previous_z = instances[batches[previous].first_instance][14];
            const float current_z = instances[batches[current].first_instance][14];
            assert(previous_z > current_z || (previous_z == current_z && previous < current));
        }
    }

    /* The procedural detail's parameters: a pattern rounds to the nearest whole number, as the shader rounds it. */
    {
        typedef struct DetailCase {
            float parameters[4];
            bool accepted;
            const char *what;
        } DetailCase;
        const DetailCase cases[] = {
            { { 0.0f, 0.0f, 0.0f, 0.0f }, true, "a zeroed batch" },
            { { 0.3f, 0.02f, 0.0f, 0.0f }, true, "none ignores x, y and z" },
            { { 0.0f, 0.0f, 0.0f, 0.49f }, true, "0.49 is none" },
            { { 0.2f, 0.02f, 3.0f, 1.0f }, true, "the ground" },
            { { 0.0f, 0.0f, 6.0f, 3.0f }, true, "a facade with x = y = 0 (0.6.0's preview of pattern 7)" },
            { { 0.07f, 0.0f, 6.0f, 3.0f }, true, "a facade" },
            { { 0.0f, 0.0f, 0.0f, 3.0f }, false, "a facade with z 0" },
            { { 0.0f, 0.0f, 1.0f, 6.0f }, true, "a lamp's lens" },
            { { 0.0f, 0.0f, 0.0f, 6.0f }, false, "a lamp's lens with z 0" },
            { { 0.0f, 0.0f, 1.0f, 6.49f }, true, "6.49 is a lamp's lens" },
            { { 0.0f, 0.0f, 0.0f, 7.0f }, true, "pattern 7" },
            { { -0.0f, -0.0f, -0.0f, 7.0f }, true, "pattern 7 with negative zeros" },
            { { 0.0f, 0.0f, 0.0f, 6.5f }, true, "6.5 is pattern 7" },
            { { 0.0f, 0.0f, 1.0f, 6.5f }, false, "6.5 with z set" },
            { { 0.0f, 0.0f, 6.0f, 7.0f }, false, "pattern 7 with z 6" },
            { { 0.07f, 0.0f, 0.0f, 7.0f }, false, "pattern 7 with x set" },
            { { 0.0f, 0.01f, 0.0f, 7.0f }, false, "pattern 7 with y set" },
            { { 0.0f, 0.0f, -1.0f, 7.0f }, false, "pattern 7 with a negative z" },
            { { 0.0f, 0.0f, 0.0f, 7.25f }, false, "past 7" },
            { { 0.0f, 0.0f, 1.0f, 8.0f }, false, "pattern 8" },
            { { 0.0f, 0.0f, 1.0f, -0.25f }, false, "a negative pattern" },
            { { 0.0f, 0.0f, 0.0f, NAN }, false, "a NaN pattern" },
            { { NAN, 0.0f, 0.0f, 7.0f }, false, "pattern 7 with a NaN x" },
            { { 0.2f, 0.02f, INFINITY, 1.0f }, false, "an infinite z" },
        };
        for (size_t index = 0u; index < sizeof(cases) / sizeof(cases[0]); ++index) {
            const bool accepted = btbh_gpu_scene_detail_parameters_valid(cases[index].parameters);
            if (accepted != cases[index].accepted) {
                fprintf(stderr, "detail parameters: %s %s\n", cases[index].what,
                    accepted ? "accepted" : "refused");
            }
            assert(accepted == cases[index].accepted);
        }
        printf("detail parameters: %u cases (pattern 7 only with x, y and z at 0; nothing past 7)\n",
            (unsigned)(sizeof(cases) / sizeof(cases[0])));
    }

    check_run_key();

    printf("scene opaque order test passed\n");
    return 0;
}
