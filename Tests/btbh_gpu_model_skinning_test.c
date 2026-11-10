#include "btbh_gpu_internal.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>

static void assert_near(float actual, float expected) {
    assert(fabsf(actual - expected) < 0.00001f);
}

// The packed weights as whole 255ths (each must be one), added up.
static uint32_t whole_255ths(const float weights[4]) {
    uint32_t total = 0u;
    for (uint32_t slot = 0u; slot < 4u; slot += 1u) {
        const float scaled = weights[slot] * 255.0f;
        const float whole = floorf(scaled + 0.5f);
        assert(fabsf(scaled - whole) < 0.001f);
        total += (uint32_t) whole;
    }
    return total;
}

// What v0.2.0 made of byte weights: each slot rounded on its own, so the sum could miss 255.
static uint32_t rounded_each_sum(const uint32_t bytes[4]) {
    const uint32_t sum = bytes[0] + bytes[1] + bytes[2] + bytes[3];
    uint32_t total = 0u;
    for (uint32_t slot = 0u; slot < 4u; slot += 1u) {
        total += (bytes[slot] * 255u + (sum >> 1u)) / sum;
    }
    return total;
}

// Normalises byte weights and checks that they add up to exactly 255, each within one 255th of its
// exact share (rounded down, or up by one).
static void assert_normalized_bytes(const uint32_t bytes[4]) {
    float joints[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    float weights[4];
    uint32_t sum = 0u;
    for (uint32_t slot = 0u; slot < 4u; slot += 1u) {
        weights[slot] = (float) bytes[slot];
        sum += bytes[slot];
    }
    btbh_gpu_model_normalize_bone_weights(joints, weights);
    assert(whole_255ths(weights) == 255u);
    for (uint32_t slot = 0u; slot < 4u; slot += 1u) {
        const uint32_t whole = (uint32_t) floorf(weights[slot] * 255.0f + 0.5f);
        const uint32_t floor_share = bytes[slot] * 255u / sum;
        assert(whole == floor_share || whole == floor_share + 1u);
        assert(bytes[slot] != 0u || whole == 0u);
    }
}

int main(void) {
    float joints[4] = {0};
    float weights[4] = {0};

    assert(btbh_gpu_model_assign_bone_weight(
        joints, weights, 4u, 0.10f
    ));
    assert(btbh_gpu_model_assign_bone_weight(
        joints, weights, 7u, 0.40f
    ));
    assert(btbh_gpu_model_assign_bone_weight(
        joints, weights, 9u, 0.20f
    ));
    assert(btbh_gpu_model_assign_bone_weight(
        joints, weights, 11u, 0.30f
    ));
    assert(btbh_gpu_model_assign_bone_weight(
        joints, weights, 13u, 0.50f
    ));
    assert(!btbh_gpu_model_assign_bone_weight(
        joints, weights, 15u, 0.05f
    ));

    btbh_gpu_model_normalize_bone_weights(joints, weights);

    assert_near(joints[0], 11.0f);
    assert_near(joints[1], 13.0f);
    assert_near(joints[2], 7.0f);
    assert_near(joints[3], 9.0f);
    assert_near(weights[0], 55.0f / 255.0f);
    assert_near(weights[1], 91.0f / 255.0f);
    assert_near(weights[2], 73.0f / 255.0f);
    assert_near(weights[3], 36.0f / 255.0f);

    float fallback_joints[4] = {8.0f, 6.0f, 4.0f, 2.0f};
    float fallback_weights[4] = {0};
    btbh_gpu_model_normalize_bone_weights(
        fallback_joints,
        fallback_weights
    );
    assert_near(fallback_joints[0], 0.0f);
    assert_near(fallback_weights[0], 1.0f);
    assert_near(fallback_weights[1], 0.0f);
    assert_near(fallback_weights[2], 0.0f);
    assert_near(fallback_weights[3], 0.0f);

    float gltf_joints[4] = {11.0f, 4.0f, 9.0f, 7.0f};
    float gltf_weights[4] = {0.30f, 0.10f, 0.20f, 0.40f};
    btbh_gpu_model_pack_gltf_bone_weights(
        gltf_joints,
        gltf_weights
    );
    assert_near(gltf_joints[0], 11.0f);
    assert_near(gltf_joints[1], 4.0f);
    assert_near(gltf_joints[2], 7.0f);
    assert_near(gltf_joints[3], 9.0f);
    // 77 + 26 + 102 + 51 = 256 before; the shortest remainder (joint 7's) now gives up the extra 255th.
    assert_near(gltf_weights[0], 77.0f / 255.0f);
    assert_near(gltf_weights[1], 26.0f / 255.0f);
    assert_near(gltf_weights[2], 101.0f / 255.0f);
    assert_near(gltf_weights[3], 51.0f / 255.0f);
    assert(whole_255ths(gltf_weights) == 255u);

    // Byte weights whose slots, rounded each on their own, added up to 254, 256 and 257.
    {
        static const uint32_t cases[3][4] = {
            {1u, 1u, 1u, 200u},
            {1u, 1u, 1u, 1u},
            {1u, 1u, 1u, 3u},
        };
        static const uint32_t old_sums[3] = {254u, 256u, 257u};
        static const float expected[3][4] = {
            {2.0f, 1.0f, 1.0f, 251.0f},
            {64.0f, 64.0f, 64.0f, 63.0f},
            {43.0f, 43.0f, 42.0f, 127.0f},
        };
        for (uint32_t index = 0u; index < 3u; index += 1u) {
            assert(rounded_each_sum(cases[index]) == old_sums[index]);
            float case_joints[4] = {1.0f, 2.0f, 3.0f, 4.0f};
            float case_weights[4];
            for (uint32_t slot = 0u; slot < 4u; slot += 1u) {
                case_weights[slot] = (float) cases[index][slot];
            }
            btbh_gpu_model_normalize_bone_weights(case_joints, case_weights);
            for (uint32_t slot = 0u; slot < 4u; slot += 1u) {
                assert_near(case_weights[slot], expected[index][slot] / 255.0f);
            }
            assert(whole_255ths(case_weights) == 255u);
        }
    }

    // glTF weights that add up to 1 but packed to 256 and 254 255ths: four quarters, and 0.02, 0.02,
    // 0.48, 0.48.
    {
        float quarter_joints[4] = {3.0f, 0.0f, 2.0f, 1.0f};
        float quarter_weights[4] = {0.25f, 0.25f, 0.25f, 0.25f};
        btbh_gpu_model_pack_gltf_bone_weights(quarter_joints, quarter_weights);
        assert(whole_255ths(quarter_weights) == 255u);

        float split_joints[4] = {5.0f, 6.0f, 7.0f, 8.0f};
        float split_weights[4] = {0.02f, 0.02f, 0.48f, 0.48f};
        btbh_gpu_model_pack_gltf_bone_weights(split_joints, split_weights);
        assert(whole_255ths(split_weights) == 255u);
    }

    // A sweep over two-, three- and four-bone byte weights: every one adds up to exactly 255, while the
    // old per-slot rounding missed it both ways.
    {
        uint32_t old_low = 255u;
        uint32_t old_high = 255u;
        for (uint32_t a = 1u; a <= 255u; a += 1u) {
            for (uint32_t b = 1u; b <= 255u; b += 1u) {
                const uint32_t pair[4] = {a, b, 0u, 0u};
                assert_normalized_bytes(pair);
            }
        }
        for (uint32_t a = 1u; a <= 255u; a += 3u) {
            for (uint32_t b = 1u; b <= 255u; b += 3u) {
                for (uint32_t c = 1u; c <= 255u; c += 3u) {
                    const uint32_t triple[4] = {0u, a, b, c};
                    assert_normalized_bytes(triple);
                }
            }
        }
        for (uint32_t a = 1u; a <= 255u; a += 9u) {
            for (uint32_t b = 1u; b <= 255u; b += 9u) {
                for (uint32_t c = 1u; c <= 255u; c += 9u) {
                    for (uint32_t d = 1u; d <= 255u; d += 9u) {
                        const uint32_t quad[4] = {a, b, c, d};
                        assert_normalized_bytes(quad);
                        const uint32_t old_sum = rounded_each_sum(quad);
                        old_low = old_sum < old_low ? old_sum : old_low;
                        old_high = old_sum > old_high ? old_sum : old_high;
                    }
                }
            }
        }
        assert(old_low < 255u && old_high > 255u);
    }

    // glTF weights in hundredths that add up to 1, through the whole packing: always 255 255ths.
    for (uint32_t a = 1u; a < 100u; a += 1u) {
        for (uint32_t b = 1u; a + b < 100u; b += 1u) {
            for (uint32_t c = 1u; a + b + c < 100u; c += 1u) {
                const uint32_t d = 100u - a - b - c;
                float sweep_joints[4] = {9.0f, 3.0f, 6.0f, 1.0f};
                float sweep_weights[4] = {
                    (float) a / 100.0f,
                    (float) b / 100.0f,
                    (float) c / 100.0f,
                    (float) d / 100.0f,
                };
                btbh_gpu_model_pack_gltf_bone_weights(sweep_joints, sweep_weights);
                assert(whole_255ths(sweep_weights) == 255u);
            }
        }
    }

    // A single bone stays whole.
    {
        float single_joints[4] = {12.0f, 0.0f, 0.0f, 0.0f};
        float single_weights[4] = {1.0f, 0.0f, 0.0f, 0.0f};
        btbh_gpu_model_pack_gltf_bone_weights(single_joints, single_weights);
        assert(whole_255ths(single_weights) == 255u);
        float whole_weight = 0.0f;
        for (uint32_t slot = 0u; slot < 4u; slot += 1u) {
            if (single_joints[slot] == 12.0f) {
                whole_weight = single_weights[slot];
            }
        }
        assert(whole_weight == 1.0f);
    }

    return 0;
}
