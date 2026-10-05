#include "btbh_gpu_internal.h"

#include <assert.h>
#include <math.h>
#include <stdlib.h>

static void assert_near(float actual, float expected) {
    assert(fabsf(actual - expected) < 0.00001f);
}

int main(void) {
    const float identity[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const float antipodal_identity[4] = {
        0.0f,
        0.0f,
        0.0f,
        -1.0f
    };
    float output[4];

    btbh_gpu_animation_slerp_shortest(
        identity,
        antipodal_identity,
        0.5f,
        output
    );
    assert_near(output[0], 0.0f);
    assert_near(output[1], 0.0f);
    assert_near(output[2], 0.0f);
    assert_near(output[3], 1.0f);

    const float quarter_turn_y[4] = {
        0.0f,
        0.7071067812f,
        0.0f,
        0.7071067812f
    };
    btbh_gpu_animation_slerp_shortest(
        identity,
        quarter_turn_y,
        0.5f,
        output
    );
    assert_near(output[0], 0.0f);
    assert_near(output[1], 0.3826834324f);
    assert_near(output[2], 0.0f);
    assert_near(output[3], 0.9238795325f);

    const float nearby_rotation_y[4] = {
        0.0f,
        0.28f,
        0.0f,
        0.96f
    };
    btbh_gpu_animation_slerp_shortest(
        identity,
        nearby_rotation_y,
        0.25f,
        output
    );
    const float legacy_nlerp_length = sqrtf(
        0.07f * 0.07f + 0.99f * 0.99f
    );
    assert_near(output[0], 0.0f);
    assert_near(output[1], 0.07f / legacy_nlerp_length);
    assert_near(output[2], 0.0f);
    assert_near(output[3], 0.99f / legacy_nlerp_length);

    BTBHGPUAnimationLibraryReport report = {0};
    BTBHGPUAnimationLibrary *library =
        btbh_gpu_animation_library_load(
            BTBH_GPU_ANIMATION_FIXTURE_PATH,
            &report
        );
    assert(library != NULL);
    const uint32_t joint_count =
        btbh_gpu_animation_library_skin_joint_count(library, 0u);
    assert(joint_count > 0u);
    float *first = calloc((size_t)joint_count * 16u, sizeof(float));
    float *last = calloc((size_t)joint_count * 16u, sizeof(float));
    float *blended = calloc((size_t)joint_count * 16u, sizeof(float));
    assert(first != NULL);
    assert(last != NULL);
    assert(blended != NULL);
    const float first_time = 0.25f;
    const float last_time = 0.75f;
    assert(btbh_gpu_animation_library_sample_skin(
        library,
        0u,
        first_time,
        0u,
        first,
        joint_count
    ));
    assert(btbh_gpu_animation_library_sample_skin(
        library,
        1u,
        last_time,
        0u,
        last,
        joint_count
    ));
    BTBHGPUAnimationPose *pose =
        btbh_gpu_animation_pose_create(library);
    assert(pose != NULL);
    const uint32_t indices[2] = {0u, 1u};
    const float times[2] = {first_time, last_time};
    const float weights[2] = {0.5f, 0.5f};
    assert(btbh_gpu_animation_pose_set_samples(
        pose,
        indices,
        times,
        weights,
        2u
    ));
    assert(btbh_gpu_animation_pose_sample_skin(
        pose,
        0u,
        blended,
        joint_count
    ));
    bool differs_from_first = false;
    bool differs_from_last = false;
    for (uint32_t index = 0u; index < joint_count * 16u; index += 1u) {
        assert(isfinite(blended[index]));
        differs_from_first |= fabsf(blended[index] - first[index]) > 0.00001f;
        differs_from_last |= fabsf(blended[index] - last[index]) > 0.00001f;
    }
    assert(differs_from_first);
    assert(differs_from_last);
    assert_near(blended[12], 0.25f);
    assert_near(blended[13], 1.5f);
    assert_near(blended[14], 0.0f);
    btbh_gpu_animation_pose_destroy(pose);
    free(blended);
    free(last);
    free(first);
    btbh_gpu_animation_library_destroy(library);

    // The skeleton root comes after its joint in the node list, as Blender
    // exports write it: the joint still sits under the root's transform.
    BTBHGPUAnimationLibrary *root_last =
        btbh_gpu_animation_library_load(
            BTBH_GPU_ROOT_AFTER_CHILDREN_FIXTURE_PATH,
            &report
        );
    assert(root_last != NULL);
    assert(btbh_gpu_animation_library_skin_joint_count(root_last, 0u) == 1u);
    float joint[16] = {0};
    assert(btbh_gpu_animation_library_sample_skin(
        root_last,
        0u,
        0.5f,
        0u,
        joint,
        1u
    ));
    const float expected[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        3.0f, 1.0f, 0.0f, 1.0f
    };
    for (uint32_t index = 0u; index < 16u; index += 1u) {
        assert_near(joint[index], expected[index]);
    }
    btbh_gpu_animation_library_destroy(root_last);
    return 0;
}
