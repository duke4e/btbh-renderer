/* The opaque pass's front-to-back order (BTBHGPUScenePost.opaque_sort, btbh_gpu_scene_opaque_order) and the
 * batch flags' layout, without a GPU: which batches sort, which keep their place, and the depth they sort by. */
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
_Static_assert(sizeof(BTBHGPUSceneGeometryBatch) == 256u, "BTBHGPUSceneGeometryBatch keeps its size");
_Static_assert(offsetof(BTBHGPUSceneGeometryBatch, ssao_mode) == 252u, "ssao_mode keeps its place");
_Static_assert(offsetof(BTBHGPUSceneGeometryBatch, flags) == 253u, "flags is the old ssao_reserved[0]");
_Static_assert(offsetof(BTBHGPUSceneGeometryBatch, ssao_reserved) == 254u, "the rest stays reserved");
_Static_assert(sizeof(BTBHGPUScenePost) == 432u, "BTBHGPUScenePost keeps its size");
_Static_assert(offsetof(BTBHGPUScenePost, ssao_debug_view) == 425u, "ssao_debug_view keeps its place");
_Static_assert(offsetof(BTBHGPUScenePost, opaque_sort) == 426u, "opaque_sort is the old ssao_reserved[0]");
_Static_assert(offsetof(BTBHGPUScenePost, ssao_reserved) == 427u, "the rest stays reserved");
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
            batches, NULL, MANY, &instances[0][0], view_projection, many_order, scratch);
        assert(count == MANY);
        for (uint32_t index = 1u; index < MANY; ++index) {
            const uint32_t previous = many_order[index - 1u];
            const uint32_t current = many_order[index];
            const float previous_z = instances[batches[previous].first_instance][14];
            const float current_z = instances[batches[current].first_instance][14];
            assert(previous_z > current_z || (previous_z == current_z && previous < current));
        }
    }

    printf("scene opaque order test passed\n");
    return 0;
}
