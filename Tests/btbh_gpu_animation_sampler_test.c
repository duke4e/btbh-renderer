/* The decoded animation sampler (since 0.16.0) against v0.15.0's, which read every key time and value through cgltf's
 * accessor reader, bit for bit.
 *
 * The glTFs: the fixtures with clips (blended-animation, root-after-children-skin, skinned-quad, and animation-samplers,
 * which has every sampler the renderer reads: linear, step and cubic spline; translations, slerped rotations and scales;
 * normalised and plain integer outputs; key times shared, repeated and single; an output narrower than its path, one
 * without a buffer view, two channels on one target, a channel without a target node; a skin without inverse binds, one
 * whose can't be read and one with fewer than its joints (0.16.0 fails it; 0.15.0 read past the accessor); clips that
 * can't be read: a sparse output, one wider than its path, key times that aren't scalars), then any paths on the
 * command line (a game's models, run by hand).
 *
 * For each, at many times a clip (every key time and the floats either side of it, the midpoints, before the first and
 * after the last key, far outside, -0 and +0, and pseudo-random times over the range):
 * - each sampled channel's value against v0.15.0's (both written over the same sentinel, so a partial write shows), and
 *   the same failures: a clip v0.15.0 can't read at one time it can't read at any;
 * - each clip's palette for each skin, and blends of two and three clips through a pose, against v0.15.0's.
 * Then the palettes kept across presents (btbh_gpu_bone_palette_cache): keys, hits, eviction by age and by budget
 * checked against a model of it, a pose's key. Prints each glTF's decoded bytes. */
#undef NDEBUG

#include "btbh_gpu_internal.h"
#include "cgltf.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ v0.15.0's sampler (the reference) */
/* Copied from src/gpu/btbh_gpu_model.c at v0.15.0, renamed (btbh_gpu_animation_ -> reference_) and reading a parsed
   cgltf_data of its own. */

typedef struct ReferenceLibrary {
    cgltf_data *data;
} ReferenceLibrary;

static void reference_slerp_shortest(
    const float lower[4],
    const float upper[4],
    float amount,
    float output[4]
);

static bool reference_read_value(
    const cgltf_animation_sampler *sampler,
    cgltf_size key_index,
    cgltf_size value_index,
    cgltf_size component_count,
    float *value
) {
    if (
        sampler == NULL ||
        sampler->output == NULL ||
        value == NULL
    ) {
        return false;
    }
    const cgltf_size sample_index =
        sampler->interpolation == cgltf_interpolation_type_cubic_spline
            ? key_index * 3u + value_index
            : key_index;
    return cgltf_accessor_read_float(
        sampler->output,
        sample_index,
        value,
        component_count
    );
}

static bool reference_sample_value(
    const cgltf_animation_sampler *sampler,
    float time,
    cgltf_size component_count,
    bool quaternion,
    float *value
) {
    if (
        sampler == NULL ||
        sampler->input == NULL ||
        sampler->output == NULL ||
        sampler->input->count == 0u ||
        component_count == 0u ||
        component_count > 4u ||
        value == NULL
    ) {
        return false;
    }
    const cgltf_size key_count = sampler->input->count;
    float first_time = 0.0f;
    float last_time = 0.0f;
    if (
        !cgltf_accessor_read_float(
            sampler->input,
            0u,
            &first_time,
            1u
        ) ||
        !cgltf_accessor_read_float(
            sampler->input,
            key_count - 1u,
            &last_time,
            1u
        )
    ) {
        return false;
    }
    if (key_count == 1u || time <= first_time) {
        return reference_read_value(
            sampler,
            0u,
            1u,
            component_count,
            value
        );
    }
    if (time >= last_time) {
        return reference_read_value(
            sampler,
            key_count - 1u,
            1u,
            component_count,
            value
        );
    }

    cgltf_size lower = 0u;
    cgltf_size upper = key_count - 1u;
    while (upper - lower > 1u) {
        const cgltf_size middle = lower + (upper - lower) / 2u;
        float middle_time = 0.0f;
        if (!cgltf_accessor_read_float(
                sampler->input,
                middle,
                &middle_time,
                1u
            )) {
            return false;
        }
        if (time < middle_time) {
            upper = middle;
        } else {
            lower = middle;
        }
    }
    float lower_time = 0.0f;
    float upper_time = 0.0f;
    float lower_value[4] = {0};
    float upper_value[4] = {0};
    if (
        !cgltf_accessor_read_float(
            sampler->input,
            lower,
            &lower_time,
            1u
        ) ||
        !cgltf_accessor_read_float(
            sampler->input,
            upper,
            &upper_time,
            1u
        ) ||
        !reference_read_value(
            sampler,
            lower,
            1u,
            component_count,
            lower_value
        )
    ) {
        return false;
    }
    if (
        sampler->interpolation == cgltf_interpolation_type_step ||
        upper_time <= lower_time
    ) {
        (void)memcpy(
            value,
            lower_value,
            component_count * sizeof(float)
        );
        return true;
    }
    if (!reference_read_value(
            sampler,
            upper,
            1u,
            component_count,
            upper_value
        )) {
        return false;
    }
    const float interval = upper_time - lower_time;
    const float amount = (time - lower_time) / interval;
    if (
        sampler->interpolation ==
        cgltf_interpolation_type_cubic_spline
    ) {
        float lower_tangent[4] = {0};
        float upper_tangent[4] = {0};
        if (
            !reference_read_value(
                sampler,
                lower,
                2u,
                component_count,
                lower_tangent
            ) ||
            !reference_read_value(
                sampler,
                upper,
                0u,
                component_count,
                upper_tangent
            )
        ) {
            return false;
        }
        const float amount2 = amount * amount;
        const float amount3 = amount2 * amount;
        const float h00 = 2.0f * amount3 - 3.0f * amount2 + 1.0f;
        const float h10 = amount3 - 2.0f * amount2 + amount;
        const float h01 = -2.0f * amount3 + 3.0f * amount2;
        const float h11 = amount3 - amount2;
        for (cgltf_size component = 0u;
             component < component_count;
             component += 1u) {
            value[component] =
                h00 * lower_value[component] +
                h10 * interval * lower_tangent[component] +
                h01 * upper_value[component] +
                h11 * interval * upper_tangent[component];
        }
    } else if (quaternion && component_count == 4u) {
        reference_slerp_shortest(
            lower_value,
            upper_value,
            amount,
            value
        );
    } else {
        for (cgltf_size component = 0u;
             component < component_count;
             component += 1u) {
            value[component] =
                lower_value[component] +
                (upper_value[component] - lower_value[component]) *
                    amount;
        }
    }
    return true;
}

static void reference_normalize_quaternion(float rotation[4]) {
    const float length_squared =
        rotation[0] * rotation[0] +
        rotation[1] * rotation[1] +
        rotation[2] * rotation[2] +
        rotation[3] * rotation[3];
    if (length_squared <= 0.0f || !isfinite(length_squared)) {
        rotation[0] = 0.0f;
        rotation[1] = 0.0f;
        rotation[2] = 0.0f;
        rotation[3] = 1.0f;
        return;
    }
    const float inverse_length = 1.0f / sqrtf(length_squared);
    for (uint32_t component = 0u; component < 4u; component += 1u) {
        rotation[component] *= inverse_length;
    }
}

static void reference_matrix_srt_quaternion(
    const float scale[3],
    const float rotation[4],
    const float translation[3],
    float output[16]
) {
    const float length = sqrtf(
        rotation[0] * rotation[0] +
        rotation[1] * rotation[1] +
        rotation[2] * rotation[2] +
        rotation[3] * rotation[3]
    );
    if (length < 0.000001f) {
        memset(output, 0, sizeof(float) * 16u);
        output[0] = scale[0];
        output[5] = scale[1];
        output[10] = scale[2];
        output[12] = translation[0];
        output[13] = translation[1];
        output[14] = translation[2];
        output[15] = 1.0f;
        return;
    }

    const float inverse_length = 1.0f / length;
    const float qx = rotation[0] * inverse_length;
    const float qy = rotation[1] * inverse_length;
    const float qz = rotation[2] * inverse_length;
    const float qw = rotation[3] * inverse_length;
    const float qx2 = qx * qx;
    const float qy2 = qy * qy;
    const float qz2 = qz * qz;
    const float qxqy = qx * qy;
    const float qxqz = qx * qz;
    const float qxqw = qx * qw;
    const float qyqz = qy * qz;
    const float qyqw = qy * qw;
    const float qzqw = qz * qw;
    const float r00 = 1.0f - 2.0f * (qy2 + qz2);
    const float r01 = 2.0f * (qxqy - qzqw);
    const float r02 = 2.0f * (qxqz + qyqw);
    const float r10 = 2.0f * (qxqy + qzqw);
    const float r11 = 1.0f - 2.0f * (qx2 + qz2);
    const float r12 = 2.0f * (qyqz - qxqw);
    const float r20 = 2.0f * (qxqz - qyqw);
    const float r21 = 2.0f * (qyqz + qxqw);
    const float r22 = 1.0f - 2.0f * (qx2 + qy2);

    output[0] = r00 * scale[0];
    output[1] = r10 * scale[0];
    output[2] = r20 * scale[0];
    output[3] = 0.0f;
    output[4] = r01 * scale[1];
    output[5] = r11 * scale[1];
    output[6] = r21 * scale[1];
    output[7] = 0.0f;
    output[8] = r02 * scale[2];
    output[9] = r12 * scale[2];
    output[10] = r22 * scale[2];
    output[11] = 0.0f;
    output[12] = translation[0];
    output[13] = translation[1];
    output[14] = translation[2];
    output[15] = 1.0f;
}

static void reference_slerp_shortest(
    const float lower[4],
    const float upper[4],
    float amount,
    float output[4]
) {
    float start[4];
    float end[4];
    (void)memcpy(start, lower, sizeof(start));
    (void)memcpy(end, upper, sizeof(end));

    float dot =
        start[0] * end[0] +
        start[1] * end[1] +
        start[2] * end[2] +
        start[3] * end[3];
    if (dot < 0.0f) {
        dot = -dot;
        for (uint32_t component = 0u; component < 4u; ++component) {
            end[component] = -end[component];
        }
    }
    if (fabsf(dot) >= 1.0f) {
        (void)memcpy(output, start, sizeof(start));
        return;
    }
    if (dot > 0.95f) {
        for (uint32_t component = 0u; component < 4u; ++component) {
            output[component] =
                start[component] +
                amount * (end[component] - start[component]);
        }
        reference_normalize_quaternion(output);
        return;
    }

    const float angle = acosf(dot);
    const float sine = sqrtf(1.0f - dot * dot);
    if (fabsf(sine) < 0.000001f) {
        for (uint32_t component = 0u; component < 4u; ++component) {
            output[component] =
                start[component] * 0.5f + end[component] * 0.5f;
        }
        return;
    }
    const float start_weight = sinf((1.0f - amount) * angle) / sine;
    const float end_weight = sinf(amount * angle) / sine;
    for (uint32_t component = 0u; component < 4u; ++component) {
        output[component] =
            start[component] * start_weight +
            end[component] * end_weight;
    }
}

static void reference_multiply_matrix(
    const float left[16],
    const float right[16],
    float output[16]
) {
    float result[16];
    for (uint32_t column = 0u; column < 4u; column += 1u) {
        for (uint32_t row = 0u; row < 4u; row += 1u) {
            result[column * 4u + row] =
                left[row] * right[column * 4u] +
                left[4u + row] * right[column * 4u + 1u] +
                left[8u + row] * right[column * 4u + 2u] +
                left[12u + row] * right[column * 4u + 3u];
        }
    }
    (void)memcpy(output, result, sizeof(result));
}

static bool reference_world_matrix(
    const cgltf_data *data,
    const float *local_matrices,
    float *world_matrices,
    uint8_t *states,
    cgltf_size node_index
) {
    if (states[node_index] == 2u) {
        return true;
    }
    if (states[node_index] == 1u) {
        return false;
    }
    states[node_index] = 1u;
    const cgltf_node *node = &data->nodes[node_index];
    const float *local = &local_matrices[node_index * 16u];
    float *world = &world_matrices[node_index * 16u];
    if (node->parent == NULL) {
        (void)memcpy(world, local, sizeof(float) * 16u);
    } else {
        const ptrdiff_t parent_index = node->parent - data->nodes;
        if (
            parent_index < 0 ||
            (cgltf_size)parent_index >= data->nodes_count ||
            !reference_world_matrix(
                data,
                local_matrices,
                world_matrices,
                states,
                (cgltf_size)parent_index
            )
        ) {
            return false;
        }
        reference_multiply_matrix(
            &world_matrices[(cgltf_size)parent_index * 16u],
            local,
            world
        );
    }
    states[node_index] = 2u;
    return true;
}

static bool reference_sample_local_transforms(
    const ReferenceLibrary *library,
    uint32_t animation_index,
    float time,
    float *translations,
    float *rotations,
    float *scales,
    uint8_t *animated_nodes
) {
    if (
        library == NULL ||
        library->data == NULL ||
        !isfinite(time) ||
        animation_index >= library->data->animations_count ||
        translations == NULL ||
        rotations == NULL ||
        scales == NULL ||
        animated_nodes == NULL
    ) {
        return false;
    }
    const cgltf_data *data = library->data;
    (void)memset(
        translations,
        0,
        data->nodes_count * 3u * sizeof(float)
    );
    (void)memset(
        rotations,
        0,
        data->nodes_count * 4u * sizeof(float)
    );
    (void)memset(
        scales,
        0,
        data->nodes_count * 3u * sizeof(float)
    );
    (void)memset(
        animated_nodes,
        0,
        data->nodes_count * sizeof(uint8_t)
    );

    for (cgltf_size index = 0u; index < data->nodes_count; index += 1u) {
        const cgltf_node *node = &data->nodes[index];
        float *translation = &translations[index * 3u];
        float *rotation = &rotations[index * 4u];
        float *scale = &scales[index * 3u];
        if (node->has_translation) {
            (void)memcpy(translation, node->translation, sizeof(float) * 3u);
        }
        if (node->has_rotation) {
            (void)memcpy(rotation, node->rotation, sizeof(float) * 4u);
        } else {
            rotation[3] = 1.0f;
        }
        if (node->has_scale) {
            (void)memcpy(scale, node->scale, sizeof(float) * 3u);
        } else {
            scale[0] = 1.0f;
            scale[1] = 1.0f;
            scale[2] = 1.0f;
        }
    }

    const cgltf_animation *animation = &data->animations[animation_index];
    bool valid = true;
    for (cgltf_size index = 0u;
         valid && index < animation->channels_count;
         index += 1u) {
        const cgltf_animation_channel *channel =
            &animation->channels[index];
        if (channel->target_node == NULL) {
            continue;
        }
        const ptrdiff_t node_index = channel->target_node - data->nodes;
        if (
            node_index < 0 ||
            (cgltf_size)node_index >= data->nodes_count
        ) {
            valid = false;
            break;
        }
        float *target = NULL;
        cgltf_size component_count = 0u;
        switch (channel->target_path) {
            case cgltf_animation_path_type_translation:
                target = &translations[(cgltf_size)node_index * 3u];
                component_count = 3u;
                break;
            case cgltf_animation_path_type_rotation:
                target = &rotations[(cgltf_size)node_index * 4u];
                component_count = 4u;
                break;
            case cgltf_animation_path_type_scale:
                target = &scales[(cgltf_size)node_index * 3u];
                component_count = 3u;
                break;
            case cgltf_animation_path_type_weights:
            case cgltf_animation_path_type_invalid:
            case cgltf_animation_path_type_max_enum:
            default:
                continue;
        }
        valid = reference_sample_value(
            channel->sampler,
            time,
            component_count,
            channel->target_path ==
                cgltf_animation_path_type_rotation,
            target
        );
        animated_nodes[(cgltf_size)node_index] = 1u;
        if (
            valid &&
            channel->target_path == cgltf_animation_path_type_rotation
        ) {
            reference_normalize_quaternion(target);
        }
    }
    return valid;
}

static bool reference_build_skin_matrices(
    const ReferenceLibrary *library,
    uint32_t skin_index,
    const float *translations,
    const float *rotations,
    const float *scales,
    const uint8_t *animated_nodes,
    float *matrices,
    uint32_t matrix_capacity
) {
    if (
        library == NULL ||
        library->data == NULL ||
        skin_index >= library->data->skins_count ||
        translations == NULL ||
        rotations == NULL ||
        scales == NULL ||
        animated_nodes == NULL ||
        matrices == NULL
    ) {
        return false;
    }
    const cgltf_data *data = library->data;
    const cgltf_skin *skin = &data->skins[skin_index];
    if (
        skin->joints_count == 0u ||
        skin->joints_count > matrix_capacity ||
        data->nodes_count == 0u ||
        data->nodes_count > SIZE_MAX / (sizeof(float) * 16u)
    ) {
        return false;
    }
    float *local_matrices = calloc(data->nodes_count * 16u, sizeof(float));
    float *world_matrices = calloc(data->nodes_count * 16u, sizeof(float));
    uint8_t *states = calloc(data->nodes_count, sizeof(uint8_t));
    if (
        local_matrices == NULL ||
        world_matrices == NULL ||
        states == NULL
    ) {
        free(states);
        free(world_matrices);
        free(local_matrices);
        return false;
    }
    for (cgltf_size index = 0u; index < data->nodes_count; index += 1u) {
        const cgltf_node *node = &data->nodes[index];
        float *local = &local_matrices[index * 16u];
        if (node->has_matrix && animated_nodes[index] == 0u) {
            (void)memcpy(local, node->matrix, sizeof(float) * 16u);
        } else {
            reference_matrix_srt_quaternion(
                &scales[index * 3u],
                &rotations[index * 4u],
                &translations[index * 3u],
                local
            );
        }
    }
    // Every local transform is set before any world transform is composed:
    // a parent may come after its children in the node list (Blender exports
    // put the armature last), and composing early multiplied by a zero matrix.
    bool valid = true;
    for (cgltf_size index = 0u;
         valid && index < data->nodes_count;
         index += 1u) {
        valid = reference_world_matrix(
            data,
            local_matrices,
            world_matrices,
            states,
            index
        );
    }

    static const float identity[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };
    for (cgltf_size joint_index = 0u;
         valid && joint_index < skin->joints_count;
         joint_index += 1u) {
        const ptrdiff_t node_index = skin->joints[joint_index] - data->nodes;
        float inverse_bind[16];
        if (
            node_index < 0 ||
            (cgltf_size)node_index >= data->nodes_count
        ) {
            valid = false;
            break;
        }
        if (skin->inverse_bind_matrices != NULL) {
            valid = cgltf_accessor_read_float(
                skin->inverse_bind_matrices,
                joint_index,
                inverse_bind,
                16u
            );
        } else {
            (void)memcpy(inverse_bind, identity, sizeof(identity));
        }
        if (valid) {
            reference_multiply_matrix(
                &world_matrices[(cgltf_size)node_index * 16u],
                inverse_bind,
                &matrices[joint_index * 16u]
            );
        }
    }
    free(states);
    free(world_matrices);
    free(local_matrices);
    return valid;
}

static bool reference_library_sample_skin_blended(
    const ReferenceLibrary *library,
    const uint32_t *animation_indices,
    const float *animation_times,
    const float *animation_weights,
    uint32_t sample_count,
    uint32_t skin_index,
    float *matrices,
    uint32_t matrix_capacity
) {
    if (
        library == NULL ||
        library->data == NULL ||
        animation_indices == NULL ||
        animation_times == NULL ||
        animation_weights == NULL ||
        sample_count == 0u ||
        skin_index >= library->data->skins_count ||
        matrices == NULL
    ) {
        return false;
    }
    const cgltf_data *data = library->data;
    if (
        data->nodes_count == 0u ||
        data->nodes_count > SIZE_MAX / (sizeof(float) * 16u)
    ) {
        return false;
    }
    float weight_sum = 0.0f;
    for (uint32_t sample = 0u; sample < sample_count; sample += 1u) {
        if (
            animation_indices[sample] >= data->animations_count ||
            !isfinite(animation_times[sample]) ||
            !isfinite(animation_weights[sample]) ||
            animation_weights[sample] < 0.0f
        ) {
            return false;
        }
        weight_sum += animation_weights[sample];
    }
    if (!isfinite(weight_sum) || weight_sum <= 0.0f) {
        return false;
    }

    float *translations = calloc(data->nodes_count * 3u, sizeof(float));
    float *rotations = calloc(data->nodes_count * 4u, sizeof(float));
    float *scales = calloc(data->nodes_count * 3u, sizeof(float));
    float *sample_translations =
        calloc(data->nodes_count * 3u, sizeof(float));
    float *sample_rotations =
        calloc(data->nodes_count * 4u, sizeof(float));
    float *sample_scales =
        calloc(data->nodes_count * 3u, sizeof(float));
    float *rotation_weight_sums =
        calloc(data->nodes_count, sizeof(float));
    uint8_t *animated_nodes =
        calloc(data->nodes_count, sizeof(uint8_t));
    uint8_t *sample_animated_nodes =
        calloc(data->nodes_count, sizeof(uint8_t));
    if (
        translations == NULL ||
        rotations == NULL ||
        scales == NULL ||
        sample_translations == NULL ||
        sample_rotations == NULL ||
        sample_scales == NULL ||
        rotation_weight_sums == NULL ||
        animated_nodes == NULL ||
        sample_animated_nodes == NULL
    ) {
        free(sample_animated_nodes);
        free(animated_nodes);
        free(rotation_weight_sums);
        free(sample_scales);
        free(sample_rotations);
        free(sample_translations);
        free(scales);
        free(rotations);
        free(translations);
        return false;
    }

    bool valid = true;
    for (uint32_t sample = 0u;
         valid && sample < sample_count;
         sample += 1u) {
        valid = reference_sample_local_transforms(
            library,
            animation_indices[sample],
            animation_times[sample],
            sample_translations,
            sample_rotations,
            sample_scales,
            sample_animated_nodes
        );
        const float weight = animation_weights[sample] / weight_sum;
        for (cgltf_size node = 0u;
             valid && node < data->nodes_count;
             node += 1u) {
            for (uint32_t component = 0u; component < 3u; component += 1u) {
                translations[node * 3u + component] +=
                    sample_translations[node * 3u + component] * weight;
                scales[node * 3u + component] +=
                    sample_scales[node * 3u + component] * weight;
            }
            if (weight > 0.0f) {
                float *rotation = &rotations[node * 4u];
                const float *sample_rotation =
                    &sample_rotations[node * 4u];
                const float previous_weight =
                    rotation_weight_sums[node];
                if (previous_weight <= 0.0f) {
                    (void)memcpy(
                        rotation,
                        sample_rotation,
                        sizeof(float) * 4u
                    );
                } else {
                    reference_slerp_shortest(
                        rotation,
                        sample_rotation,
                        weight / (previous_weight + weight),
                        rotation
                    );
                }
                rotation_weight_sums[node] = previous_weight + weight;
            }
            animated_nodes[node] |= sample_animated_nodes[node];
        }
    }
    if (valid) {
        valid = reference_build_skin_matrices(
            library,
            skin_index,
            translations,
            rotations,
            scales,
            animated_nodes,
            matrices,
            matrix_capacity
        );
    }

    free(sample_animated_nodes);
    free(animated_nodes);
    free(rotation_weight_sums);
    free(sample_scales);
    free(sample_rotations);
    free(sample_translations);
    free(scales);
    free(rotations);
    free(translations);
    return valid;
}

/* ------------------------------------------------------------------ The comparisons */

/* A float pattern no sampler writes: the values a partial write leaves show as it. */
static const uint32_t sentinel_bits = 0x7fc0beefu;

static void fill_sentinel(float *values, size_t count) {
    for (size_t index = 0u; index < count; ++index) {
        (void)memcpy(&values[index], &sentinel_bits, sizeof(float));
    }
}

typedef struct TimeList {
    float *times;
    size_t count;
    size_t capacity;
} TimeList;

static void push_time(TimeList *list, float time) {
    if (list->count == list->capacity) {
        list->capacity = list->capacity == 0u ? 256u : list->capacity * 2u;
        list->times = realloc(list->times, list->capacity * sizeof(float));
        assert(list->times != NULL);
    }
    list->times[list->count++] = time;
}

static uint32_t random_state = 0x2545f491u;

static float random_unit(void) {
    random_state = random_state * 1664525u + 1013904223u;
    return (float)(random_state >> 8) / 16777216.0f;
}

/* The times a clip is sampled at: around every key of every channel it reads, outside its range, and random ones. */
static TimeList clip_times(const cgltf_animation *animation) {
    TimeList list = {0};
    float first = INFINITY;
    float last = -INFINITY;
    for (cgltf_size index = 0u; index < animation->samplers_count; ++index) {
        const cgltf_accessor *input = animation->samplers[index].input;
        float previous = 0.0f;
        for (cgltf_size key = 0u; key < input->count; ++key) {
            float time = 0.0f;
            if (!cgltf_accessor_read_float(input, key, &time, 1u)) {
                break;
            }
            push_time(&list, time);
            push_time(&list, nextafterf(time, -INFINITY));
            push_time(&list, nextafterf(time, INFINITY));
            if (key > 0u) {
                push_time(&list, 0.5f * (previous + time));
                push_time(&list, previous + 0.25f * (time - previous));
            }
            previous = time;
            first = fminf(first, time);
            last = fmaxf(last, time);
        }
    }
    const float outside[] = {-1000.0f, -1.0f, -0.0f, 0.0f, 1.0e-7f, 1.0f, 1000.0f, 3.0e38f, -3.0e38f};
    for (size_t index = 0u; index < sizeof(outside) / sizeof(outside[0]); ++index) {
        push_time(&list, outside[index]);
    }
    if (isfinite(first) && isfinite(last)) {
        push_time(&list, first - 0.5f);
        push_time(&list, last + 0.5f);
        for (int index = 0; index < 64; ++index) {
            push_time(&list, first - 0.25f + (last - first + 0.5f) * random_unit());
        }
    }
    return list;
}

static cgltf_data *load_reference(const char *path) {
    cgltf_options options;
    memset(&options, 0, sizeof(options));
    cgltf_data *data = NULL;
    cgltf_result result = cgltf_parse_file(&options, path, &data);
    if (result == cgltf_result_success) {
        result = cgltf_load_buffers(&options, data, path);
    }
    if (result == cgltf_result_success) {
        result = cgltf_validate(data);
    }
    assert(result == cgltf_result_success);
    return data;
}

/* Fewer inverse binds than joints: 0.15.0 read past the accessor (undefined; never compared), 0.16.0 fails the skin. */
static bool short_inverse_binds(const cgltf_skin *skin) {
    return skin->inverse_bind_matrices != NULL && skin->inverse_bind_matrices->count < skin->joints_count;
}

typedef struct Counts {
    uint64_t channel_samples;
    uint64_t channel_failures;
    uint64_t palettes;
    uint64_t palette_failures;
    uint64_t blends;
} Counts;

/* Every sampled channel of clip `animation_index` at every time: v0.15.0's value and the decoded one. */
static void compare_channels(
    const BTBHGPUAnimationLibrary *library,
    const cgltf_data *data,
    uint32_t animation_index,
    const TimeList *times,
    Counts *counts
) {
    const cgltf_animation *animation = &data->animations[animation_index];
    /* v0.15.0 fails a clip when any channel fails; whether one does never depends on the time. */
    bool clip_fails = false;
    for (cgltf_size channel_index = 0u; channel_index < animation->channels_count; ++channel_index) {
        const cgltf_animation_channel *channel = &animation->channels[channel_index];
        cgltf_size component_count = 0u;
        switch (channel->target_path) {
            case cgltf_animation_path_type_translation:
            case cgltf_animation_path_type_scale:
                component_count = 3u;
                break;
            case cgltf_animation_path_type_rotation:
                component_count = 4u;
                break;
            default:
                break;
        }
        if (channel->target_node == NULL || component_count == 0u) {
            float value[4];
            assert(!btbh_gpu_animation_library_sample_channel(library, animation_index, (uint32_t)channel_index,
                0.0f, value));
            continue;
        }
        size_t failures = 0u;
        for (size_t index = 0u; index < times->count; ++index) {
            float value[4];
            fill_sentinel(value, 4u);
            failures += reference_sample_value(channel->sampler, times->times[index], component_count,
                channel->target_path == cgltf_animation_path_type_rotation, value) ? 0u : 1u;
        }
        assert(failures == 0u || failures == times->count);
        clip_fails |= failures > 0u;
    }
    for (cgltf_size channel_index = 0u; channel_index < animation->channels_count; ++channel_index) {
        const cgltf_animation_channel *channel = &animation->channels[channel_index];
        const bool rotation = channel->target_path == cgltf_animation_path_type_rotation;
        const cgltf_size component_count = rotation ? 4u : 3u;
        if (channel->target_node == NULL ||
            (channel->target_path != cgltf_animation_path_type_translation && !rotation &&
             channel->target_path != cgltf_animation_path_type_scale)) {
            continue;
        }
        for (size_t index = 0u; index < times->count; ++index) {
            const float time = times->times[index];
            float expected[4];
            float actual[4];
            fill_sentinel(expected, 4u);
            fill_sentinel(actual, 4u);
            const bool expected_ok = reference_sample_value(channel->sampler, time, component_count, rotation,
                expected);
            const bool actual_ok = btbh_gpu_animation_library_sample_channel(library, animation_index,
                (uint32_t)channel_index, time, actual);
            counts->channel_samples += 1u;
            if (clip_fails) {
                assert(!actual_ok);
                counts->channel_failures += 1u;
                continue;
            }
            assert(expected_ok && actual_ok);
            if (memcmp(expected, actual, sizeof(expected)) != 0) {
                fprintf(stderr, "clip %u channel %u at %.9g: expected %.9g %.9g %.9g %.9g, got %.9g %.9g %.9g %.9g\n",
                    animation_index, (unsigned)channel_index, (double)time, (double)expected[0],
                    (double)expected[1], (double)expected[2], (double)expected[3], (double)actual[0],
                    (double)actual[1], (double)actual[2], (double)actual[3]);
            }
            assert(memcmp(expected, actual, sizeof(expected)) == 0);
        }
    }
}

/* A blend of `count` clips through a pose against v0.15.0's, for every skin. */
static void compare_blend(
    const BTBHGPUAnimationLibrary *library,
    ReferenceLibrary *reference,
    const uint32_t *indices,
    const float *sample_times,
    const float *weights,
    uint32_t count,
    float *expected,
    float *actual,
    Counts *counts
) {
    BTBHGPUAnimationPose *pose = btbh_gpu_animation_pose_create(library);
    assert(pose != NULL);
    const bool set = btbh_gpu_animation_pose_set_samples(pose, indices, sample_times, weights, count) != 0u;
    for (cgltf_size skin = 0u; skin < reference->data->skins_count; ++skin) {
        const uint32_t joint_count = (uint32_t)reference->data->skins[skin].joints_count;
        const size_t floats = (size_t)joint_count * 16u;
        fill_sentinel(expected, floats);
        fill_sentinel(actual, floats);
        const bool actual_ok = set && btbh_gpu_animation_pose_sample_skin(pose, (uint32_t)skin, actual, joint_count);
        if (short_inverse_binds(&reference->data->skins[skin])) {
            assert(!actual_ok);
            counts->blends += 1u;
            continue;
        }
        const bool expected_ok = reference_library_sample_skin_blended(reference, indices, sample_times, weights,
            count, (uint32_t)skin, expected, joint_count);
        assert(expected_ok == actual_ok);
        if (expected_ok) {
            assert(memcmp(expected, actual, floats * sizeof(float)) == 0);
        }
        counts->blends += 1u;
    }
    btbh_gpu_animation_pose_destroy(pose);
}

static void compare_file(const char *path, bool must_have_clips) {
    BTBHGPUAnimationLibraryReport report;
    memset(&report, 0, sizeof(report));
    BTBHGPUAnimationLibrary *library = btbh_gpu_animation_library_load(path, &report);
    if (library == NULL) {
        assert(!must_have_clips);
        printf("%s: no clips (%s)\n", path, report.message);
        return;
    }
    ReferenceLibrary reference = {load_reference(path)};
    const cgltf_data *data = reference.data;
    assert(report.animation_count == data->animations_count);
    Counts counts;
    memset(&counts, 0, sizeof(counts));

    size_t most_joints = 1u;
    for (cgltf_size skin = 0u; skin < data->skins_count; ++skin) {
        if (data->skins[skin].joints_count > most_joints) {
            most_joints = data->skins[skin].joints_count;
        }
    }
    float *expected = malloc(most_joints * 16u * sizeof(float));
    float *actual = malloc(most_joints * 16u * sizeof(float));
    assert(expected != NULL && actual != NULL);
    TimeList *all_times = calloc(data->animations_count, sizeof(TimeList));
    assert(all_times != NULL);

    for (uint32_t animation = 0u; animation < (uint32_t)data->animations_count; ++animation) {
        all_times[animation] = clip_times(&data->animations[animation]);
        const TimeList *times = &all_times[animation];
        compare_channels(library, data, animation, times, &counts);
        const float one = 1.0f;
        for (cgltf_size skin = 0u; skin < data->skins_count; ++skin) {
            const uint32_t joint_count = btbh_gpu_animation_library_skin_joint_count(library, (uint32_t)skin);
            assert(joint_count == data->skins[skin].joints_count);
            const size_t floats = (size_t)joint_count * 16u;
            for (size_t index = 0u; index < times->count; ++index) {
                const float time = times->times[index];
                fill_sentinel(expected, floats);
                fill_sentinel(actual, floats);
                const bool actual_ok = btbh_gpu_animation_library_sample_skin(library, animation, time,
                    (uint32_t)skin, actual, joint_count);
                if (short_inverse_binds(&data->skins[skin])) {
                    assert(!actual_ok);
                    counts.palettes += 1u;
                    counts.palette_failures += 1u;
                    continue;
                }
                const bool expected_ok = reference_library_sample_skin_blended(&reference, &animation, &time, &one,
                    1u, (uint32_t)skin, expected, joint_count);
                assert(expected_ok == actual_ok);
                counts.palettes += 1u;
                if (!expected_ok) {
                    counts.palette_failures += 1u;
                    continue;
                }
                if (memcmp(expected, actual, floats * sizeof(float)) != 0) {
                    fprintf(stderr, "clip %u skin %u at %.9g: the palette differs\n", animation, (unsigned)skin,
                        (double)time);
                }
                assert(memcmp(expected, actual, floats * sizeof(float)) == 0);
            }
            /* Too small a palette fails in both. */
            if (joint_count > 1u) {
                const float time = times->count > 0u ? times->times[0] : 0.0f;
                assert(!reference_library_sample_skin_blended(&reference, &animation, &time, &one, 1u,
                    (uint32_t)skin, expected, joint_count - 1u));
                assert(!btbh_gpu_animation_library_sample_skin(library, animation, time, (uint32_t)skin, actual,
                    joint_count - 1u));
            }
        }
    }

    /* Blends: each clip with the next (and the one after), at random times of theirs, under a few weightings. */
    const uint32_t clip_count = (uint32_t)data->animations_count;
    const float weightings[][3] = {
        {0.5f, 0.5f, 0.0f}, {0.3f, 0.7f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 2.0f, 0.0f}, {0.2f, 0.3f, 0.5f},
        {3.0f, 1.0f, 0.25f},
    };
    for (uint32_t first = 0u; first < clip_count; ++first) {
        for (size_t weighting = 0u; weighting < sizeof(weightings) / sizeof(weightings[0]); ++weighting) {
            for (int round = 0; round < 8; ++round) {
                uint32_t indices[3];
                float sample_times[3];
                for (uint32_t sample = 0u; sample < 3u; ++sample) {
                    indices[sample] = (first + sample) % clip_count;
                    const TimeList *times = &all_times[indices[sample]];
                    sample_times[sample] = times->count > 0u
                        ? times->times[(size_t)(random_unit() * (float)times->count) % times->count]
                        : 0.0f;
                }
                const uint32_t count = weightings[weighting][2] > 0.0f ? 3u : 2u;
                compare_blend(library, &reference, indices, sample_times, weightings[weighting], count, expected,
                    actual, &counts);
            }
        }
    }

    printf("%s: %u clips, %u skins, %llu channel samples (%llu of clips that can't be read), %llu palettes (%llu "
        "failing in both), %llu blends: all the same to the bit; decoded %zu bytes\n",
        path, clip_count, (unsigned)data->skins_count, (unsigned long long)counts.channel_samples,
        (unsigned long long)counts.channel_failures, (unsigned long long)counts.palettes,
        (unsigned long long)counts.palette_failures, (unsigned long long)counts.blends,
        btbh_gpu_animation_library_decoded_bytes(library));
    for (uint32_t animation = 0u; animation < clip_count; ++animation) {
        free(all_times[animation].times);
    }
    free(all_times);
    free(actual);
    free(expected);
    cgltf_free(reference.data);
    btbh_gpu_animation_library_destroy(library);
}

/* ------------------------------------------------------------------ The kept palettes */

static BTBHGPUBonePaletteKey test_key(uint64_t serial, uint32_t time_bits, uint32_t joint_count) {
    BTBHGPUBonePaletteKey key;
    memset(&key, 0, sizeof(key));
    key.serial = serial;
    key.animation_index = (uint32_t)(serial % 7u);
    key.time_bits = time_bits;
    key.skin_index = 0u;
    key.joint_count = joint_count;
    return key;
}

static void fill_palette(float *matrices, uint32_t joint_count, float seed) {
    for (uint32_t index = 0u; index < joint_count * 16u; ++index) {
        matrices[index] = seed + (float)index;
    }
}

static uint32_t kept_palettes(const BTBHGPUBonePaletteCache *cache) {
    uint32_t count = 0u;
    btbh_gpu_bone_palette_cache_read(cache, &count, NULL, NULL);
    return count;
}

static void test_palette_cache(void) {
    BTBHGPUBonePaletteCache *slot = NULL;
    BTBHGPUBonePaletteCache *cache = btbh_gpu_bone_palette_cache_begin(&slot);
    assert(cache != NULL && cache == slot);
    float palette[4u * 16u];
    float other[4u * 16u];
    fill_palette(palette, 4u, 1.0f);
    const BTBHGPUBonePaletteKey a = test_key(11u, 0x3f000000u, 4u);
    assert(btbh_gpu_bone_palette_cache_find(cache, &a) == NULL);
    btbh_gpu_bone_palette_cache_store(cache, &a, palette);
    const float *found = btbh_gpu_bone_palette_cache_find(cache, &a);
    assert(found != NULL && memcmp(found, palette, sizeof(palette)) == 0);
    /* Every field of the key counts. */
    BTBHGPUBonePaletteKey b = a;
    b.time_bits += 1u;
    assert(btbh_gpu_bone_palette_cache_find(cache, &b) == NULL);
    b = a;
    b.serial += 1u;
    assert(btbh_gpu_bone_palette_cache_find(cache, &b) == NULL);
    b = a;
    b.animation_index += 1u;
    assert(btbh_gpu_bone_palette_cache_find(cache, &b) == NULL);
    b = a;
    b.skin_index = 1u;
    assert(btbh_gpu_bone_palette_cache_find(cache, &b) == NULL);
    b = a;
    b.joint_count = 3u;
    assert(btbh_gpu_bone_palette_cache_find(cache, &b) == NULL);
    /* A second store under the key keeps the first copy. */
    fill_palette(other, 4u, 100.0f);
    btbh_gpu_bone_palette_cache_store(cache, &a, other);
    assert(memcmp(btbh_gpu_bone_palette_cache_find(cache, &a), palette, sizeof(palette)) == 0);
    assert(kept_palettes(cache) == 1u);

    /* Unused for BTBH_GPU_BONE_PALETTE_MAX_AGE uses it stays; one more and it goes. */
    for (uint32_t use = 0u; use < BTBH_GPU_BONE_PALETTE_MAX_AGE; ++use) {
        assert(btbh_gpu_bone_palette_cache_begin(&slot) == cache);
        assert(kept_palettes(cache) == 1u);
    }
    (void)btbh_gpu_bone_palette_cache_begin(&slot);
    assert(kept_palettes(cache) == 0u);
    assert(btbh_gpu_bone_palette_cache_find(cache, &a) == NULL);

    /* The budget: a use keeps what fits; the next use's palettes push out the oldest. */
    const uint32_t big = 1000u;
    float *big_palette = malloc((size_t)big * 16u * sizeof(float));
    assert(big_palette != NULL);
    fill_palette(big_palette, big, 7.0f);
    (void)btbh_gpu_bone_palette_cache_begin(&slot);
    const uint32_t fit = BTBH_GPU_BONE_PALETTE_BUDGET / big;
    for (uint32_t index = 0u; index <= fit; ++index) {
        const BTBHGPUBonePaletteKey key = test_key(1000u + index, 0u, big);
        btbh_gpu_bone_palette_cache_store(cache, &key, big_palette);
    }
    size_t matrices = 0u;
    btbh_gpu_bone_palette_cache_read(cache, NULL, &matrices, NULL);
    assert(kept_palettes(cache) == fit && matrices == (size_t)fit * big);
    const BTBHGPUBonePaletteKey last = test_key(1000u + fit, 0u, big);
    assert(btbh_gpu_bone_palette_cache_find(cache, &last) == NULL);
    (void)btbh_gpu_bone_palette_cache_begin(&slot);
    const BTBHGPUBonePaletteKey newer = test_key(5000u, 0u, big);
    btbh_gpu_bone_palette_cache_store(cache, &newer, big_palette);
    assert(btbh_gpu_bone_palette_cache_find(cache, &newer) != NULL);
    btbh_gpu_bone_palette_cache_read(cache, NULL, &matrices, NULL);
    assert(matrices <= BTBH_GPU_BONE_PALETTE_BUDGET);
    /* Larger than the budget: never kept. */
    float *huge = calloc((size_t)(BTBH_GPU_BONE_PALETTE_BUDGET + 1u) * 16u, sizeof(float));
    assert(huge != NULL);
    const BTBHGPUBonePaletteKey huge_key = test_key(6000u, 0u, BTBH_GPU_BONE_PALETTE_BUDGET + 1u);
    btbh_gpu_bone_palette_cache_store(cache, &huge_key, huge);
    assert(btbh_gpu_bone_palette_cache_find(cache, &huge_key) == NULL);
    free(huge);
    free(big_palette);

    /* A model of the cache over many uses: random palettes found or stored each use, the rest left alone; whatever
       was used within BTBH_GPU_BONE_PALETTE_MAX_AGE uses is found with its matrices, nothing older is. */
    enum { KEYS = 700, USES = 300 };
    static uint64_t last_use[KEYS];
    memset(last_use, 0, sizeof(last_use));
    BTBHGPUBonePaletteCache *model_slot = NULL;
    uint64_t use = 0u;
    float matrices_of_key[2u * 16u];
    for (int round = 0; round < USES; ++round) {
        BTBHGPUBonePaletteCache *model = btbh_gpu_bone_palette_cache_begin(&model_slot);
        assert(model != NULL);
        use += 1u;
        uint32_t expected_kept = 0u;
        for (uint32_t key_index = 0u; key_index < KEYS; ++key_index) {
            if (last_use[key_index] != 0u && last_use[key_index] + BTBH_GPU_BONE_PALETTE_MAX_AGE < use) {
                last_use[key_index] = 0u;
            }
        }
        const uint32_t touched = 20u + (uint32_t)(random_unit() * 120.0f);
        for (uint32_t touch = 0u; touch < touched; ++touch) {
            const uint32_t key_index = (uint32_t)(random_unit() * (float)KEYS) % KEYS;
            const BTBHGPUBonePaletteKey key = test_key(70000u + key_index, key_index * 3u, 2u);
            fill_palette(matrices_of_key, 2u, (float)key_index);
            const float *kept = btbh_gpu_bone_palette_cache_find(model, &key);
            assert((kept != NULL) == (last_use[key_index] != 0u));
            if (kept != NULL) {
                assert(memcmp(kept, matrices_of_key, sizeof(matrices_of_key)) == 0);
            } else {
                btbh_gpu_bone_palette_cache_store(model, &key, matrices_of_key);
            }
            last_use[key_index] = use;
        }
        for (uint32_t key_index = 0u; key_index < KEYS; ++key_index) {
            expected_kept += last_use[key_index] != 0u ? 1u : 0u;
        }
        assert(kept_palettes(model) == expected_kept);
    }
    size_t bytes = 0u;
    btbh_gpu_bone_palette_cache_read(model_slot, NULL, NULL, &bytes);
    assert(bytes > 0u);
    btbh_gpu_bone_palette_cache_destroy(model_slot);
    btbh_gpu_bone_palette_cache_destroy(slot);
    btbh_gpu_bone_palette_cache_destroy(NULL);
}

/* A pose's key: one sample takes the library's key at its clip and time; a blend a serial of its own, which stays
   while its samples are set again to the same bits and changes when they change. */
static void test_pose_keys(const char *path) {
    BTBHGPUAnimationLibraryReport report;
    memset(&report, 0, sizeof(report));
    BTBHGPUAnimationLibrary *library = btbh_gpu_animation_library_load(path, &report);
    assert(library != NULL && report.animation_count >= 2u);
    BTBHGPUAnimationLibrary *other = btbh_gpu_animation_library_load(path, &report);
    assert(other != NULL);
    BTBHGPUBonePaletteKey library_key;
    BTBHGPUBonePaletteKey other_key;
    BTBHGPUBonePaletteKey pose_key;
    assert(btbh_gpu_bone_palette_key(library, NULL, 1u, 0.25f, 0u, 5u, &library_key));
    assert(btbh_gpu_bone_palette_key(other, NULL, 1u, 0.25f, 0u, 5u, &other_key));
    /* Two loads of one file are two libraries. */
    assert(library_key.serial != other_key.serial);
    BTBHGPUAnimationPose *pose = btbh_gpu_animation_pose_create(library);
    assert(pose != NULL);
    /* No samples yet: no key. */
    assert(!btbh_gpu_bone_palette_key(NULL, pose, 0u, 0.0f, 0u, 5u, &pose_key));
    const uint32_t clip = 1u;
    const float time = 0.25f;
    const float weight = 3.0f;
    assert(btbh_gpu_animation_pose_set_samples(pose, &clip, &time, &weight, 1u));
    assert(btbh_gpu_bone_palette_key(other, pose, 0u, 9.0f, 0u, 5u, &pose_key));
    assert(memcmp(&pose_key, &library_key, sizeof(pose_key)) == 0);
    const uint32_t clips[2] = {0u, 1u};
    float times[2] = {0.1f, 0.6f};
    const float weights[2] = {0.25f, 0.75f};
    assert(btbh_gpu_animation_pose_set_samples(pose, clips, times, weights, 2u));
    BTBHGPUBonePaletteKey blend_key;
    assert(btbh_gpu_bone_palette_key(NULL, pose, 0u, 0.0f, 0u, 5u, &blend_key));
    assert(blend_key.animation_index == UINT32_MAX);
    assert(blend_key.serial != library_key.serial && blend_key.serial != other_key.serial);
    assert(btbh_gpu_animation_pose_set_samples(pose, clips, times, weights, 2u));
    BTBHGPUBonePaletteKey again;
    assert(btbh_gpu_bone_palette_key(NULL, pose, 0u, 0.0f, 0u, 5u, &again));
    assert(memcmp(&again, &blend_key, sizeof(again)) == 0);
    times[1] = nextafterf(times[1], 1.0f);
    assert(btbh_gpu_animation_pose_set_samples(pose, clips, times, weights, 2u));
    assert(btbh_gpu_bone_palette_key(NULL, pose, 0u, 0.0f, 0u, 5u, &again));
    assert(again.serial != blend_key.serial);
    /* A refused set keeps the samples and the key. */
    const float negative[2] = {-1.0f, 1.0f};
    BTBHGPUBonePaletteKey before = again;
    assert(!btbh_gpu_animation_pose_set_samples(pose, clips, times, negative, 2u));
    assert(btbh_gpu_bone_palette_key(NULL, pose, 0u, 0.0f, 0u, 5u, &again));
    assert(memcmp(&again, &before, sizeof(again)) == 0);
    btbh_gpu_animation_pose_destroy(pose);
    btbh_gpu_animation_library_destroy(other);
    btbh_gpu_animation_library_destroy(library);
}

int main(int argc, char **argv) {
    static const char *const fixtures[] = {
        "blended-animation.gltf",
        "root-after-children-skin.gltf",
        "skinned-quad.gltf",
        "animation-samplers.gltf",
    };
    char path[1024];
    for (size_t index = 0u; index < sizeof(fixtures) / sizeof(fixtures[0]); ++index) {
        (void)snprintf(path, sizeof(path), "%s/%s", BTBH_GPU_FIXTURE_DIR, fixtures[index]);
        compare_file(path, true);
    }
    for (int index = 1; index < argc; ++index) {
        compare_file(argv[index], false);
    }
    test_palette_cache();
    (void)snprintf(path, sizeof(path), "%s/%s", BTBH_GPU_FIXTURE_DIR, "animation-samplers.gltf");
    test_pose_keys(path);
    printf("btbh_gpu_animation_sampler: ok\n");
    return 0;
}
