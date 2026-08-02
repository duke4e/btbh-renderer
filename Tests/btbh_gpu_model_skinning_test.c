#include "btbh_gpu_internal.h"

#include <assert.h>
#include <math.h>

static void assert_near(float actual, float expected) {
    assert(fabsf(actual - expected) < 0.00001f);
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
    assert_near(gltf_weights[0], 77.0f / 255.0f);
    assert_near(gltf_weights[1], 26.0f / 255.0f);
    assert_near(gltf_weights[2], 102.0f / 255.0f);
    assert_near(gltf_weights[3], 51.0f / 255.0f);

    return 0;
}
