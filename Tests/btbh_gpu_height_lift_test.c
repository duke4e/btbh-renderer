/* RFC 0012 (0.11.0): the height lift's C copy (src/gpu/btbh_gpu_height_lift.h), without a GPU.
 * - Half floats: exact values, rounding to nearest even (ties, subnormals, overflow), every half back and forth.
 * - The heightfield's slopes: central differences inside, one-sided at the edges; its texels.
 * - The bilinear read: texel centres, clamped edges.
 * - The lift and the tilt: a plane's lift and normal exactly, a vertical face's normal unchanged, tangents and normals
 *   orthogonal after the tilt, KEEP_NORMALS.
 * - A v2-style lattice (x0 + i * step) mapped onto texel centres: the lift at a lattice point is its sample; along a
 *   lattice line the lift is linear between the line's samples (no crack between pieces meeting there).
 * - The 32-byte block, the sort's height, and the refusals' reasons. */
#undef NDEBUG

#include "btbh_gpu_internal.h"
#include "btbh_gpu_height_lift.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t checks;

#define CHECK(condition)                                                              \
    do {                                                                              \
        checks += 1u;                                                                 \
        if (!(condition)) {                                                           \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            abort();                                                                  \
        }                                                                             \
    } while (0)

static bool near_value(float a, float b, float tolerance) {
    return fabsf(a - b) <= tolerance;
}

static uint32_t random_state = 12345u;
static float random_unit(void) {
    random_state = random_state * 1664525u + 1013904223u;
    return (float)(random_state >> 8u) / 16777216.0f;
}

/* ---- half floats ---- */

static void check_halves(void) {
    CHECK(btbh_gpu_height_lift_half(0.0f) == 0x0000u);
    CHECK(btbh_gpu_height_lift_half(-0.0f) == 0x8000u);
    CHECK(btbh_gpu_height_lift_half(1.0f) == 0x3c00u);
    CHECK(btbh_gpu_height_lift_half(-2.0f) == 0xc000u);
    CHECK(btbh_gpu_height_lift_half(0.5f) == 0x3800u);
    CHECK(btbh_gpu_height_lift_half(65504.0f) == 0x7bffu);
    CHECK(btbh_gpu_height_lift_half(65520.0f) == 0x7c00u);       /* the tie past the largest half: infinity */
    CHECK(btbh_gpu_height_lift_half(65519.0f) == 0x7bffu);
    CHECK(btbh_gpu_height_lift_half(1.0e9f) == 0x7c00u);
    CHECK(btbh_gpu_height_lift_half(INFINITY) == 0x7c00u);
    CHECK((btbh_gpu_height_lift_half(NAN) & 0x7c00u) == 0x7c00u && (btbh_gpu_height_lift_half(NAN) & 0x3ffu) != 0u);
    /* 1 + 2^-11 lies halfway between 1 and 1 + 2^-10: to even (1). 1 + 3 * 2^-11: to even (1 + 2^-9). */
    CHECK(btbh_gpu_height_lift_half(1.0f + 0.00048828125f) == 0x3c00u);
    CHECK(btbh_gpu_height_lift_half(1.0f + 3.0f * 0.00048828125f) == 0x3c02u);
    CHECK(btbh_gpu_height_lift_half(1.0f + 0.0005f) == 0x3c01u);
    /* 15.4, the top of v2's heights: within 8..16 a half steps by 1/128. */
    CHECK(near_value(btbh_gpu_height_lift_from_half(btbh_gpu_height_lift_half(15.4f)), 15.4f, 1.0f / 256.0f));
    CHECK(near_value(btbh_gpu_height_lift_from_half(btbh_gpu_height_lift_half(-0.34f)), -0.34f, 1.0f / 4096.0f));
    /* Subnormals: 2^-24 is the smallest; half of it ties to 0; 1.5 of it ties to 2. */
    CHECK(btbh_gpu_height_lift_half(5.9604644775390625e-8f) == 0x0001u);
    CHECK(btbh_gpu_height_lift_half(2.98023223876953125e-8f) == 0x0000u);
    CHECK(btbh_gpu_height_lift_half(8.94069671630859375e-8f) == 0x0002u);
    CHECK(btbh_gpu_height_lift_half(6.103515625e-5f) == 0x0400u);   /* the smallest normal */
    CHECK(btbh_gpu_height_lift_half(6.0975551605224609375e-5f) == 0x03ffu);
    CHECK(btbh_gpu_height_lift_half(1.0e-10f) == 0x0000u);
    /* Every finite half, back and forth. */
    for (uint32_t half = 0u; half < 0x10000u; ++half) {
        if ((half & 0x7c00u) == 0x7c00u) {
            continue;
        }
        const float value = btbh_gpu_height_lift_from_half((uint16_t)half);
        CHECK(btbh_gpu_height_lift_half(value) == (uint16_t)half);
    }
    printf("halves: exact values, ties to even, subnormals, overflow, and every finite half back and forth\n");
}

/* ---- the heightfield ---- */

static void check_slopes(void) {
    /* 4 x 3, h = i * i + 10 j. */
    float heights[12];
    for (uint32_t j = 0u; j < 3u; ++j) {
        for (uint32_t i = 0u; i < 4u; ++i) {
            heights[j * 4u + i] = (float)(i * i) + 10.0f * (float)j;
        }
    }
    /* Along u: one-sided at i = 0 (1 - 0) and i = 3 (9 - 4); central inside ((4 - 0) / 2, (9 - 1) / 2). */
    CHECK(btbh_gpu_height_lift_slope(heights, 4u, 3u, 0u, 1u, 0u) == 1.0f);
    CHECK(btbh_gpu_height_lift_slope(heights, 4u, 3u, 1u, 1u, 0u) == 2.0f);
    CHECK(btbh_gpu_height_lift_slope(heights, 4u, 3u, 2u, 1u, 0u) == 4.0f);
    CHECK(btbh_gpu_height_lift_slope(heights, 4u, 3u, 3u, 1u, 0u) == 5.0f);
    /* Along v: 10 everywhere (one-sided and central alike). */
    for (uint32_t j = 0u; j < 3u; ++j) {
        for (uint32_t i = 0u; i < 4u; ++i) {
            CHECK(btbh_gpu_height_lift_slope(heights, 4u, 3u, i, j, 1u) == 10.0f);
        }
    }
    /* A 2 x 2 field: one-sided everywhere. */
    const float tiny[4] = { 1.0f, 3.0f, 2.0f, 7.0f };
    CHECK(btbh_gpu_height_lift_slope(tiny, 2u, 2u, 0u, 0u, 0u) == 2.0f);
    CHECK(btbh_gpu_height_lift_slope(tiny, 2u, 2u, 1u, 1u, 0u) == 5.0f);
    CHECK(btbh_gpu_height_lift_slope(tiny, 2u, 2u, 1u, 0u, 1u) == 4.0f);
    uint16_t texels[16];
    btbh_gpu_height_lift_texels(tiny, 2u, 2u, texels);
    CHECK(texels[0] == btbh_gpu_height_lift_half(1.0f) && texels[1] == btbh_gpu_height_lift_half(2.0f) &&
        texels[2] == btbh_gpu_height_lift_half(1.0f) && texels[3] == 0x3c00u);
    CHECK(texels[12] == btbh_gpu_height_lift_half(7.0f) && texels[13] == btbh_gpu_height_lift_half(5.0f) &&
        texels[14] == btbh_gpu_height_lift_half(4.0f));
    printf("slopes: central differences inside, one-sided at the edges; the texels\n");
}

static void check_bilinear(void) {
    /* 2 x 2: 0 1 / 2 3. Texel centres at u = 0.25 and 0.75. */
    const float texels[4] = { 0.0f, 1.0f, 2.0f, 3.0f };
    float out = 0.0f;
    btbh_gpu_height_lift_bilinear(texels, 1u, 2u, 2u, 0.25f, 0.25f, &out);
    CHECK(out == 0.0f);
    btbh_gpu_height_lift_bilinear(texels, 1u, 2u, 2u, 0.75f, 0.75f, &out);
    CHECK(out == 3.0f);
    btbh_gpu_height_lift_bilinear(texels, 1u, 2u, 2u, 0.5f, 0.5f, &out);
    CHECK(out == 1.5f);
    /* Clamped past the edges and up to the outer half texel. */
    btbh_gpu_height_lift_bilinear(texels, 1u, 2u, 2u, -3.0f, 0.25f, &out);
    CHECK(out == 0.0f);
    btbh_gpu_height_lift_bilinear(texels, 1u, 2u, 2u, 0.1f, 0.75f, &out);
    CHECK(out == 2.0f);
    btbh_gpu_height_lift_bilinear(texels, 1u, 2u, 2u, 7.0f, 7.0f, &out);
    CHECK(out == 3.0f);
    btbh_gpu_height_lift_bilinear(texels, 1u, 2u, 2u, 0.5f, -1.0f, &out);
    CHECK(out == 0.5f);
    printf("bilinear: texel centres, clamped edges\n");
}

/* ---- the lift ---- */

/* The halves of a field's texels as floats (3 a texel), as the GPU reads them. */
static float *texel_floats(const float *heights, uint32_t width, uint32_t height) {
    uint16_t *halves = malloc((size_t)width * height * 4u * sizeof(uint16_t));
    float *floats = malloc((size_t)width * height * 3u * sizeof(float));
    assert(halves != NULL && floats != NULL);
    btbh_gpu_height_lift_texels(heights, width, height, halves);
    for (size_t index = 0u; index < (size_t)width * height; ++index) {
        for (uint32_t c = 0u; c < 3u; ++c) {
            floats[index * 3u + c] = btbh_gpu_height_lift_from_half(halves[index * 4u + c]);
        }
    }
    free(halves);
    return floats;
}

/* A v2-style lattice: samples at (x0 + i * step, z0 + j * step), the texel centres on them. */
static BTBHGPUSceneHeightLift lattice_lift(BTBHGPUTexture *field, float x0, float z0, float step) {
    BTBHGPUSceneHeightLift lift;
    memset(&lift, 0, sizeof(lift));
    lift.heightfield = field;
    lift.world_to_uv[0] = 1.0f / ((float)field->width * step);
    lift.world_to_uv[1] = 1.0f / ((float)field->height * step);
    lift.world_to_uv[2] = 0.5f / (float)field->width - x0 * lift.world_to_uv[0];
    lift.world_to_uv[3] = 0.5f / (float)field->height - z0 * lift.world_to_uv[1];
    return lift;
}

static void check_plane(void) {
    /* h = 0.25 i - 0.5 j + 2 over a 9 x 7 lattice from (-3, 5) with step 0.5: dh/dx = 0.5, dh/dz = -1. */
    enum { W = 9, H = 7 };
    float heights[W * H];
    for (uint32_t j = 0u; j < H; ++j) {
        for (uint32_t i = 0u; i < W; ++i) {
            heights[j * W + i] = 0.25f * (float)i - 0.5f * (float)j + 2.0f;
        }
    }
    float *texels = texel_floats(heights, W, H);
    BTBHGPUTexture field;
    memset(&field, 0, sizeof(field));
    field.width = W;
    field.height = H;
    field.heights = heights;
    BTBHGPUSceneHeightLift lift = lattice_lift(&field, -3.0f, 5.0f, 0.5f);
    BTBHGPUHeightLiftUniforms uniforms;
    btbh_gpu_height_lift_pack(&lift, W, H, &uniforms);
    CHECK(uniforms.scale[0] == 1.0f && uniforms.scale[1] == 0.0f);
    for (uint32_t sample = 0u; sample < 200u; ++sample) {
        /* Inside the lattice: (x0, x0 + 8 step) by (z0, z0 + 6 step). */
        const float x = -3.0f + 4.0f * random_unit();
        const float z = 5.0f + 3.0f * random_unit();
        float lifted[3];
        btbh_gpu_height_lift_at(&uniforms, texels, W, H, x, z, lifted);
        const float expected = 0.5f * (x + 3.0f) - (z - 5.0f) + 2.0f;
        CHECK(near_value(lifted[0], expected, 1.0e-5f));
        CHECK(near_value(lifted[1], 0.5f, 1.0e-6f));
        CHECK(near_value(lifted[2], -1.0f, 1.0e-6f));
        /* The CPU heights give the same height (the sort's). */
        CHECK(near_value(btbh_gpu_height_lift_height(&lift, heights, W, H, x, z), expected, 1.0e-5f));
        /* An upward normal tilts to the plane's: (-dh/dx, 1, -dh/dz). A tangent along x climbs dh/dx. */
        float position[3] = { x, 0.25f, z };
        float normal[3] = { 0.0f, 1.0f, 0.0f };
        float tangent[3] = { 1.0f, 0.0f, 0.0f };
        btbh_gpu_height_lift_apply(lifted, position, normal, tangent);
        CHECK(position[0] == x && position[2] == z && near_value(position[1], 0.25f + expected, 1.0e-5f));
        CHECK(near_value(normal[0], -0.5f, 1.0e-6f) && normal[1] == 1.0f && near_value(normal[2], 1.0f, 1.0e-6f));
        CHECK(tangent[0] == 1.0f && near_value(tangent[1], 0.5f, 1.0e-6f) && tangent[2] == 0.0f);
        /* A vertical face keeps its normal exactly. */
        float wall[3] = { 0.6f, 0.0f, -0.8f };
        btbh_gpu_height_lift_apply(lifted, position, wall, NULL);
        CHECK(wall[0] == 0.6f && wall[1] == 0.0f && wall[2] == -0.8f);
    }
    /* height_scale 2 and offset -1: twice the slope. KEEP_NORMALS: no tilt (the block's slope factors are 0). */
    lift.height_scale = 2.0f;
    lift.height_offset = -1.0f;
    btbh_gpu_height_lift_pack(&lift, W, H, &uniforms);
    float lifted[3];
    btbh_gpu_height_lift_at(&uniforms, texels, W, H, -1.0f, 6.0f, lifted);
    CHECK(near_value(lifted[0], 2.0f * (0.5f * 2.0f - 1.0f + 2.0f) - 1.0f, 1.0e-5f));
    CHECK(near_value(lifted[1], 1.0f, 1.0e-6f) && near_value(lifted[2], -2.0f, 1.0e-6f));
    lift.flags = BTBH_GPU_SCENE_HEIGHT_LIFT_KEEP_NORMALS;
    btbh_gpu_height_lift_pack(&lift, W, H, &uniforms);
    CHECK(uniforms.scale[2] == 0.0f && uniforms.scale[3] == 0.0f);
    btbh_gpu_height_lift_at(&uniforms, texels, W, H, -1.0f, 6.0f, lifted);
    float kept[3] = { 0.3f, 0.9f, -0.1f };
    float kept_tangent[3] = { 0.9f, -0.3f, 0.2f };
    float position[3] = { -1.0f, 0.0f, 6.0f };
    btbh_gpu_height_lift_apply(lifted, position, kept, kept_tangent);
    CHECK(kept[0] == 0.3f && kept[1] == 0.9f && kept[2] == -0.1f);
    CHECK(kept_tangent[0] == 0.9f && kept_tangent[1] == -0.3f && kept_tangent[2] == 0.2f);
    CHECK(near_value(position[1], 3.0f, 1.0e-5f));
    free(texels);
    printf("plane: the lift and its normal exactly, a vertical face's normal unchanged, scale and offset, KEEP_NORMALS\n");
}

static void check_orthogonal(void) {
    /* A bumpy field; normals and tangents orthogonal before the tilt stay orthogonal after it. */
    enum { W = 17, H = 13 };
    float heights[W * H];
    for (uint32_t j = 0u; j < H; ++j) {
        for (uint32_t i = 0u; i < W; ++i) {
            heights[j * W + i] = 2.0f * sinf(0.7f * (float)i) * cosf(0.45f * (float)j) + 0.1f * (float)i;
        }
    }
    float *texels = texel_floats(heights, W, H);
    BTBHGPUTexture field;
    memset(&field, 0, sizeof(field));
    field.width = W;
    field.height = H;
    field.heights = heights;
    const BTBHGPUSceneHeightLift lift = lattice_lift(&field, 0.0f, 0.0f, 1.0f);
    BTBHGPUHeightLiftUniforms uniforms;
    btbh_gpu_height_lift_pack(&lift, W, H, &uniforms);
    float worst = 0.0f;
    for (uint32_t sample = 0u; sample < 500u; ++sample) {
        float normal[3] = { random_unit() - 0.5f, random_unit() - 0.5f, random_unit() - 0.5f };
        float other[3] = { random_unit() - 0.5f, random_unit() - 0.5f, random_unit() - 0.5f };
        /* tangent = other minus its part along the normal */
        const float nn = normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2];
        const float on = other[0] * normal[0] + other[1] * normal[1] + other[2] * normal[2];
        float tangent[3] = {
            other[0] - normal[0] * on / nn, other[1] - normal[1] * on / nn, other[2] - normal[2] * on / nn,
        };
        float lifted[3];
        btbh_gpu_height_lift_at(&uniforms, texels, W, H, 16.0f * random_unit(), 12.0f * random_unit(), lifted);
        float position[3] = { 0.0f, 0.0f, 0.0f };
        btbh_gpu_height_lift_apply(lifted, position, normal, tangent);
        const float dot = normal[0] * tangent[0] + normal[1] * tangent[1] + normal[2] * tangent[2];
        const float scale = sqrtf((normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]) *
            (tangent[0] * tangent[0] + tangent[1] * tangent[1] + tangent[2] * tangent[2]));
        worst = fmaxf(worst, fabsf(dot) / scale);
    }
    CHECK(worst < 1.0e-5f);
    free(texels);
    printf("orthogonal: tangents and normals stay orthogonal through the tilt (worst cosine %.2g)\n", (double)worst);
}

static void check_lattice(void) {
    /* v2's layout in small: samples on cell corners from -3 with step 1, 12 x 10, random heights in -0.34..15.4. */
    enum { W = 12, H = 10 };
    float heights[W * H];
    for (uint32_t index = 0u; index < W * H; ++index) {
        heights[index] = -0.34f + 15.74f * random_unit();
    }
    float *texels = texel_floats(heights, W, H);
    BTBHGPUTexture field;
    memset(&field, 0, sizeof(field));
    field.width = W;
    field.height = H;
    field.heights = heights;
    const BTBHGPUSceneHeightLift lift = lattice_lift(&field, -3.0f, -3.0f, 1.0f);
    BTBHGPUHeightLiftUniforms uniforms;
    btbh_gpu_height_lift_pack(&lift, W, H, &uniforms);
    /* At a lattice point the lift is its sample (as a half: within 1/256 of v2's heights). */
    for (uint32_t j = 0u; j < H; ++j) {
        for (uint32_t i = 0u; i < W; ++i) {
            float lifted[3];
            btbh_gpu_height_lift_at(&uniforms, texels, W, H, -3.0f + (float)i, -3.0f + (float)j, lifted);
            CHECK(near_value(lifted[0], heights[j * W + i], 1.0f / 256.0f + 1.0e-5f));
            CHECK(near_value(lifted[0], texels[(j * W + i) * 3u], 1.0e-4f));
            CHECK(near_value(btbh_gpu_height_lift_height(&lift, heights, W, H, -3.0f + (float)i, -3.0f + (float)j),
                heights[j * W + i], 1.0e-4f));
        }
    }
    /* Along the lattice line x = x0 + i (and z = z0 + j) the lift is linear between the line's two samples: what lies
       beside the line doesn't enter, so two pieces sharing an edge on it get the same heights. */
    for (uint32_t sample = 0u; sample < 300u; ++sample) {
        const uint32_t i = (uint32_t)(random_unit() * (float)W) % W;
        const uint32_t j = (uint32_t)(random_unit() * (float)(H - 1u)) % (H - 1u);
        const float t = random_unit();
        float lifted[3];
        btbh_gpu_height_lift_at(&uniforms, texels, W, H, -3.0f + (float)i, -3.0f + (float)j + t, lifted);
        const float a = texels[(j * W + i) * 3u];
        const float b = texels[((j + 1u) * W + i) * 3u];
        CHECK(near_value(lifted[0], a + (b - a) * t, 2.0e-4f));
        const uint32_t row = (uint32_t)(random_unit() * (float)H) % H;
        const uint32_t column = (uint32_t)(random_unit() * (float)(W - 1u)) % (W - 1u);
        btbh_gpu_height_lift_at(&uniforms, texels, W, H, -3.0f + (float)column + t, -3.0f + (float)row, lifted);
        const float c = texels[(row * W + column) * 3u];
        const float d = texels[(row * W + column + 1u) * 3u];
        CHECK(near_value(lifted[0], c + (d - c) * t, 2.0e-4f));
    }
    free(texels);
    printf("lattice: a v2-style lattice's samples on the texel centres; linear along its lines\n");
}

/* ---- the checks ---- */

static void check_problems(void) {
    BTBHGPUTexture field;
    memset(&field, 0, sizeof(field));
    BTBHGPUSceneHeightLift lift;
    memset(&lift, 0, sizeof(lift));
    /* Without a heightfield only the flags and the reserved word count. */
    CHECK(btbh_gpu_height_lift_problem(&lift) == NULL);
    lift.world_to_uv[0] = NAN;
    CHECK(btbh_gpu_height_lift_problem(&lift) == NULL);
    lift.flags = 2u;
    CHECK(btbh_gpu_height_lift_problem(&lift) != NULL && strstr(btbh_gpu_height_lift_problem(&lift), "flags"));
    lift.flags = 0u;
    lift.reserved = 1u;
    CHECK(btbh_gpu_height_lift_problem(&lift) != NULL && strstr(btbh_gpu_height_lift_problem(&lift), "reserved"));
    lift.reserved = 0u;
    lift.heightfield = &field;
    CHECK(strstr(btbh_gpu_height_lift_problem(&lift), "finite") != NULL);
    lift.world_to_uv[0] = 0.0f;
    lift.world_to_uv[1] = 0.1f;
    CHECK(strstr(btbh_gpu_height_lift_problem(&lift), "scales") != NULL);
    lift.world_to_uv[0] = 0.1f;
    lift.world_to_uv[1] = 0.0f;
    CHECK(strstr(btbh_gpu_height_lift_problem(&lift), "scales") != NULL);
    lift.world_to_uv[1] = -0.1f;
    CHECK(btbh_gpu_height_lift_problem(&lift) == NULL);
    lift.world_to_uv[3] = INFINITY;
    CHECK(strstr(btbh_gpu_height_lift_problem(&lift), "finite") != NULL);
    lift.world_to_uv[3] = 0.0f;
    lift.height_scale = NAN;
    CHECK(strstr(btbh_gpu_height_lift_problem(&lift), "height_scale") != NULL);
    lift.height_scale = 0.0f;
    lift.height_offset = -INFINITY;
    CHECK(strstr(btbh_gpu_height_lift_problem(&lift), "height_offset") != NULL);
    lift.height_offset = 0.0f;
    lift.flags = BTBH_GPU_SCENE_HEIGHT_LIFT_KEEP_NORMALS;
    CHECK(btbh_gpu_height_lift_problem(&lift) == NULL);

    /* A lifted batch: static, no billboard, not dug, casting no shadow. */
    BTBHGPUSceneGeometryBatch batch;
    memset(&batch, 0, sizeof(batch));
    batch.height_lift = &lift;
    CHECK(btbh_gpu_height_lift_active(&batch));
    batch.flags = BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW;
    CHECK(btbh_gpu_height_lift_batch_problem(&batch, 1u) == NULL);
    batch.spot_shadow_visible = 1u;
    CHECK(strstr(btbh_gpu_height_lift_batch_problem(&batch, 1u), "shadow") != NULL);
    batch.spot_shadow_visible = 0u;
    batch.flags = 0u;
    CHECK(strstr(btbh_gpu_height_lift_batch_problem(&batch, 1u), "shadow") != NULL);
    CHECK(btbh_gpu_height_lift_batch_problem(&batch, 0u) == NULL);   /* its visibility byte keeps it out */
    batch.transparency_enabled = 1u;
    batch.spot_shadow_visible = 1u;
    CHECK(btbh_gpu_height_lift_batch_problem(&batch, 1u) == NULL);   /* transparent: casts none */
    batch.animation_library = (BTBHGPUAnimationLibrary *)(uintptr_t)0x10u;
    CHECK(strstr(btbh_gpu_height_lift_batch_problem(&batch, 1u), "static") != NULL);
    batch.animation_library = NULL;
    batch.animation_pose = (BTBHGPUAnimationPose *)(uintptr_t)0x10u;
    CHECK(strstr(btbh_gpu_height_lift_batch_problem(&batch, 1u), "static") != NULL);
    batch.animation_pose = NULL;
    batch.billboard_mode = BTBH_GPU_SCENE_BILLBOARD_Y_AXIS;
    CHECK(strstr(btbh_gpu_height_lift_batch_problem(&batch, 1u), "billboard") != NULL);
    batch.billboard_mode = BTBH_GPU_SCENE_BILLBOARD_DISABLED;
    batch.dug_by = (const BTBHGPUSdfTerrain *)(uintptr_t)0x10u;
    CHECK(strstr(btbh_gpu_height_lift_batch_problem(&batch, 1u), "dug") != NULL);
    batch.dug_by = NULL;
    CHECK(btbh_gpu_height_lift_batch_problem(&batch, 1u) == NULL);
    lift.heightfield = NULL;
    CHECK(!btbh_gpu_height_lift_active(&batch));
    batch.height_lift = NULL;
    CHECK(!btbh_gpu_height_lift_active(&batch));

    /* The block: height_scale 0 means 1; the slope factors are scale x world_to_uv x the field's size. */
    memset(&lift, 0, sizeof(lift));
    lift.heightfield = &field;
    lift.world_to_uv[0] = 0.25f;
    lift.world_to_uv[1] = -0.125f;
    lift.world_to_uv[2] = 0.5f;
    lift.world_to_uv[3] = 0.75f;
    lift.height_offset = 3.0f;
    BTBHGPUHeightLiftUniforms uniforms;
    _Static_assert(sizeof(BTBHGPUHeightLiftUniforms) == 32u, "the lift's block is 32 bytes");
    btbh_gpu_height_lift_pack(&lift, 64u, 32u, &uniforms);
    CHECK(memcmp(uniforms.uv, lift.world_to_uv, sizeof(uniforms.uv)) == 0);
    CHECK(uniforms.scale[0] == 1.0f && uniforms.scale[1] == 3.0f);
    CHECK(uniforms.scale[2] == 16.0f && uniforms.scale[3] == -4.0f);
    lift.height_scale = 0.5f;
    btbh_gpu_height_lift_pack(&lift, 64u, 32u, &uniforms);
    CHECK(uniforms.scale[0] == 0.5f && uniforms.scale[2] == 8.0f && uniforms.scale[3] == -2.0f);
    printf("checks: the lift's refusals, a lifted batch's, and the block\n");
}

int main(void) {
    check_halves();
    check_slopes();
    check_bilinear();
    check_plane();
    check_orthogonal();
    check_lattice();
    check_problems();
    printf("btbh_gpu_height_lift_test: %u checks passed\n", checks);
    return 0;
}
