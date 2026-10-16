#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "btbh_gpu_internal.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#if defined(BTBH_GPU_HAS_ASSIMP)
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wignored-attributes"
#endif
#include <assimp/cimport.h>
#include <assimp/GltfMaterial.h>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#endif
#define CGLTF_IMPLEMENTATION
#include "cgltf.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

typedef struct BTBHGPUModelVertex {
    float position[3];
    float normal[3];
    float texture_coordinate[2];
    float tangent[4];
    float color[4];
    float joints[4];
    float weights[4];
} BTBHGPUModelVertex;

typedef struct BTBHGPUFloatAttribute {
    float *values;
    uint32_t component_count;
} BTBHGPUFloatAttribute;

bool btbh_gpu_model_assign_bone_weight(
    float joints[4],
    float weights[4],
    uint32_t bone_index,
    float influence
) {
    if (
        joints == NULL ||
        weights == NULL ||
        bone_index > UINT8_MAX ||
        !isfinite(influence) ||
        influence <= 0.0f
    ) {
        return false;
    }
    const float scaled = influence * 255.0f + 0.5f;
    const uint32_t quantized = scaled >= 255.0f
        ? 255u
        : (uint32_t) scaled;
    if (quantized == 0u) {
        return false;
    }

    int empty_slot = -1;
    uint32_t minimum_slot = 0u;
    float minimum_weight = weights[0];
    for (uint32_t slot = 1u; slot < 4u; slot += 1u) {
        if (weights[slot] == 0.0f && empty_slot < 0) {
            empty_slot = (int) slot;
        }
        if (weights[slot] < minimum_weight) {
            minimum_weight = weights[slot];
            minimum_slot = slot;
        }
    }
    const uint32_t destination = empty_slot >= 0
        ? (uint32_t) empty_slot
        : minimum_slot;
    if (empty_slot < 0 && (float) quantized <= minimum_weight) {
        return false;
    }
    joints[destination] = (float) bone_index;
    weights[destination] = (float) quantized;
    return true;
}

void btbh_gpu_model_pack_gltf_bone_weights(
    float joints[4],
    float weights[4]
) {
    if (joints == NULL || weights == NULL) {
        return;
    }
    float sorted_joints[4];
    float sorted_weights[4];
    memcpy(sorted_joints, joints, sizeof(sorted_joints));
    memcpy(sorted_weights, weights, sizeof(sorted_weights));
    for (uint32_t index = 1u; index < 4u; index += 1u) {
        const float joint = sorted_joints[index];
        const float weight = sorted_weights[index];
        uint32_t destination = index;
        while (
            destination > 0u &&
            sorted_joints[destination - 1u] > joint
        ) {
            sorted_joints[destination] =
                sorted_joints[destination - 1u];
            sorted_weights[destination] =
                sorted_weights[destination - 1u];
            destination -= 1u;
        }
        sorted_joints[destination] = joint;
        sorted_weights[destination] = weight;
    }

    memset(joints, 0, sizeof(float) * 4u);
    memset(weights, 0, sizeof(float) * 4u);
    for (uint32_t slot = 0u; slot < 4u; slot += 1u) {
        const float joint = sorted_joints[slot];
        if (
            !isfinite(joint) ||
            joint < 0.0f ||
            joint > (float) UINT8_MAX ||
            floorf(joint) != joint
        ) {
            continue;
        }
        (void) btbh_gpu_model_assign_bone_weight(
            joints,
            weights,
            (uint32_t) joint,
            sorted_weights[slot]
        );
    }
    btbh_gpu_model_normalize_bone_weights(joints, weights);
}

void btbh_gpu_model_normalize_bone_weights(
    float joints[4],
    float weights[4]
) {
    if (joints == NULL || weights == NULL) {
        return;
    }
    const uint32_t sum =
        (uint32_t) weights[0] +
        (uint32_t) weights[1] +
        (uint32_t) weights[2] +
        (uint32_t) weights[3];
    if (sum == 0u) {
        memset(joints, 0, sizeof(float) * 4u);
        memset(weights, 0, sizeof(float) * 4u);
        weights[0] = 1.0f;
        return;
    }
    const uint32_t half = sum >> 1u;
    for (uint32_t slot = 0u; slot < 4u; slot += 1u) {
        const uint32_t quantized =
            ((uint32_t) weights[slot] * 255u + half) / sum;
        weights[slot] = (float) quantized / 255.0f;
    }
}

struct BTBHGPUAnimationLibrary {
    cgltf_data *data;
};

struct BTBHGPUAnimationPose {
    const BTBHGPUAnimationLibrary *library;
    uint32_t *animation_indices;
    float *animation_times;
    float *animation_weights;
    uint32_t sample_count;
};

static void btbh_gpu_model_error(
    BTBHGPUModelReport *report,
    const char *message
) {
    report->result = BTBH_GPU_DEVICE_FAILURE;
    (void) snprintf(
        report->message,
        sizeof(report->message),
        "%s",
        message != NULL ? message : ""
    );
}

static cgltf_result btbh_gpu_model_file_read(
    const cgltf_memory_options *memory_options,
    const cgltf_file_options *file_options,
    const char *path,
    cgltf_size *size,
    void **data
) {
    (void) file_options;
    SDL_IOStream *stream = SDL_IOFromFile(path, "rb");
    if (stream == NULL) {
        return cgltf_result_file_not_found;
    }
    const Sint64 length = SDL_GetIOSize(stream);
    if (
        length <= 0 ||
        (uint64_t) length > (uint64_t) SIZE_MAX
    ) {
        SDL_CloseIO(stream);
        return cgltf_result_io_error;
    }
    void *bytes = memory_options->alloc_func != NULL
        ? memory_options->alloc_func(
            memory_options->user_data,
            (cgltf_size) length
        )
        : malloc((size_t) length);
    if (bytes == NULL) {
        SDL_CloseIO(stream);
        return cgltf_result_out_of_memory;
    }
    const size_t read_count = SDL_ReadIO(
        stream,
        bytes,
        (size_t) length
    );
    SDL_CloseIO(stream);
    if (read_count != (size_t) length) {
        if (memory_options->free_func != NULL) {
            memory_options->free_func(
                memory_options->user_data,
                bytes
            );
        } else {
            free(bytes);
        }
        return cgltf_result_io_error;
    }
    *size = (cgltf_size) length;
    *data = bytes;
    return cgltf_result_success;
}

static void btbh_gpu_model_file_release(
    const cgltf_memory_options *memory_options,
    const cgltf_file_options *file_options,
    void *data
) {
    (void) file_options;
    if (memory_options->free_func != NULL) {
        memory_options->free_func(memory_options->user_data, data);
    } else {
        free(data);
    }
}

static uint8_t btbh_gpu_model_color_channel(float value) {
    const float clamped = fmaxf(0.0f, fminf(1.0f, value));
    return (uint8_t)lroundf(clamped * 255.0f);
}

static void btbh_gpu_model_material_default(
    BTBHGPUModelMaterialInfo *material
) {
    (void)memset(material, 0, sizeof(*material));
    material->albedo_red = 255u;
    material->albedo_green = 255u;
    material->albedo_blue = 255u;
    material->albedo_alpha = 255u;
    material->emission_red = 255u;
    material->emission_green = 255u;
    material->emission_blue = 255u;
    material->emission_alpha = 255u;
    material->normal_scale = 1.0f;
    material->occlusion = 1.0f;
    material->roughness = 1.0f;
    material->metalness = 0.0f;
    material->alpha_cutoff = 0.01f;
}

static char *btbh_gpu_model_resolve_texture_path(
    const char *model_path,
    const char *uri
) {
    if (
        model_path == NULL ||
        uri == NULL ||
        uri[0] == '\0' ||
        strncmp(uri, "data:", 5u) == 0
    ) {
        return NULL;
    }
    const char *forward_slash = strrchr(model_path, '/');
    const char *backward_slash = strrchr(model_path, '\\');
    const char *slash = forward_slash;
    if (
        slash == NULL ||
        (backward_slash != NULL && backward_slash > slash)
    ) {
        slash = backward_slash;
    }
    const size_t prefix_length = slash != NULL
        ? (size_t)(slash - model_path) + 1u
        : 0u;
    const size_t uri_length = strlen(uri);
    if (
        uri_length == SIZE_MAX ||
        prefix_length > SIZE_MAX - uri_length - 1u
    ) {
        return NULL;
    }
    char *resolved = malloc(prefix_length + uri_length + 1u);
    if (resolved == NULL) {
        return NULL;
    }
    if (prefix_length > 0u) {
        (void)memcpy(resolved, model_path, prefix_length);
    }
    (void)memcpy(
        resolved + prefix_length,
        uri,
        uri_length + 1u
    );
    (void)cgltf_decode_uri(resolved + prefix_length);
    return resolved;
}

bool btbh_gpu_model_compose_orm_rgba8(
    const uint8_t *occlusion_pixels,
    uint32_t occlusion_width,
    uint32_t occlusion_height,
    const uint8_t *metallic_roughness_pixels,
    uint32_t metallic_roughness_width,
    uint32_t metallic_roughness_height,
    uint8_t **composed_pixels,
    uint32_t *composed_width,
    uint32_t *composed_height
) {
    if (
        composed_pixels == NULL ||
        composed_width == NULL ||
        composed_height == NULL
    ) {
        return false;
    }
    *composed_pixels = NULL;
    *composed_width = 0u;
    *composed_height = 0u;
    const bool has_occlusion =
        occlusion_pixels != NULL &&
        occlusion_width > 0u &&
        occlusion_height > 0u;
    const bool has_metallic_roughness =
        metallic_roughness_pixels != NULL &&
        metallic_roughness_width > 0u &&
        metallic_roughness_height > 0u;
    if (
        has_occlusion != (
            occlusion_pixels != NULL ||
            occlusion_width > 0u ||
            occlusion_height > 0u
        ) ||
        has_metallic_roughness != (
            metallic_roughness_pixels != NULL ||
            metallic_roughness_width > 0u ||
            metallic_roughness_height > 0u
        ) ||
        (!has_occlusion && !has_metallic_roughness)
    ) {
        return false;
    }
    const uint32_t width =
        occlusion_width > metallic_roughness_width
            ? occlusion_width
            : metallic_roughness_width;
    const uint32_t height =
        occlusion_height > metallic_roughness_height
            ? occlusion_height
            : metallic_roughness_height;
    const uint64_t byte_count =
        (uint64_t)width * (uint64_t)height * UINT64_C(4);
    if (byte_count > SIZE_MAX) {
        return false;
    }
    uint8_t *output = malloc((size_t)byte_count);
    if (output == NULL) {
        return false;
    }
    for (uint32_t y = 0u; y < height; y += 1u) {
        const uint32_t occlusion_y = has_occlusion
            ? (uint32_t)(
                (uint64_t)y * occlusion_height / height
            )
            : 0u;
        const uint32_t metallic_roughness_y =
            has_metallic_roughness
                ? (uint32_t)(
                    (uint64_t)y * metallic_roughness_height /
                    height
                )
                : 0u;
        for (uint32_t x = 0u; x < width; x += 1u) {
            const uint32_t occlusion_x = has_occlusion
                ? (uint32_t)(
                    (uint64_t)x * occlusion_width / width
                )
                : 0u;
            const uint32_t metallic_roughness_x =
                has_metallic_roughness
                    ? (uint32_t)(
                        (uint64_t)x * metallic_roughness_width /
                        width
                    )
                    : 0u;
            const size_t destination =
                ((size_t)y * width + x) * 4u;
            const size_t occlusion_source =
                ((size_t)occlusion_y * occlusion_width +
                    occlusion_x) * 4u;
            const size_t metallic_roughness_source =
                ((size_t)metallic_roughness_y *
                    metallic_roughness_width +
                    metallic_roughness_x) * 4u;
            output[destination] = has_occlusion
                ? occlusion_pixels[occlusion_source]
                : 255u;
            output[destination + 1u] = has_metallic_roughness
                ? metallic_roughness_pixels[
                    metallic_roughness_source + 1u
                ]
                : 255u;
            output[destination + 2u] = has_metallic_roughness
                ? metallic_roughness_pixels[
                    metallic_roughness_source + 2u
                ]
                : 255u;
            output[destination + 3u] = 255u;
        }
    }
    *composed_pixels = output;
    *composed_width = width;
    *composed_height = height;
    return true;
}

static bool btbh_gpu_model_decode_texture(
    const cgltf_texture_view *view,
    const char *model_path,
    uint8_t **pixels,
    uint32_t *width,
    uint32_t *height,
    BTBHGPUModelReport *model_report
) {
    *pixels = NULL;
    *width = 0u;
    *height = 0u;
    if (view == NULL || view->texture == NULL) {
        return true;
    }
    const cgltf_image *image = view->texture->image;
    if (image == NULL) {
        btbh_gpu_model_error(
            model_report,
            "glTF texture does not reference an image"
        );
        return false;
    }
    const uint8_t *encoded = NULL;
    size_t encoded_size = 0u;
    void *external_bytes = NULL;
    if (image->buffer_view != NULL) {
        const cgltf_buffer_view *buffer_view = image->buffer_view;
        if (
            buffer_view->buffer == NULL ||
            buffer_view->buffer->data == NULL ||
            buffer_view->offset > buffer_view->buffer->size ||
            buffer_view->size >
                buffer_view->buffer->size - buffer_view->offset ||
            buffer_view->size == 0u ||
            buffer_view->size > (cgltf_size)INT_MAX
        ) {
            btbh_gpu_model_error(
                model_report,
                "glTF embedded texture buffer view is invalid"
            );
            return false;
        }
        encoded =
            (const uint8_t *)buffer_view->buffer->data +
            buffer_view->offset;
        encoded_size = (size_t)buffer_view->size;
    } else {
        char *texture_path = btbh_gpu_model_resolve_texture_path(
            model_path,
            image->uri
        );
        if (texture_path == NULL) {
            btbh_gpu_model_error(
                model_report,
                image->uri != NULL &&
                        strncmp(image->uri, "data:", 5u) == 0
                    ? "GPU glTF import does not support data-URI textures"
                    : "Could not resolve external glTF texture path"
            );
            return false;
        }
        cgltf_memory_options memory_options = {0};
        cgltf_file_options file_options = {0};
        cgltf_size external_size = 0u;
        const cgltf_result read_result = btbh_gpu_model_file_read(
            &memory_options,
            &file_options,
            texture_path,
            &external_size,
            &external_bytes
        );
        free(texture_path);
        if (
            read_result != cgltf_result_success ||
            external_size == 0u ||
            external_size > (cgltf_size)INT_MAX
        ) {
            free(external_bytes);
            btbh_gpu_model_error(
                model_report,
                "Could not read external glTF texture"
            );
            return false;
        }
        encoded = external_bytes;
        encoded_size = (size_t)external_size;
    }
    const bool decoded = btbh_gpu_texture_decode_memory_rgba8(
        encoded,
        encoded_size,
        pixels,
        width,
        height
    );
    free(external_bytes);
    if (!decoded) {
        btbh_gpu_model_error(
            model_report,
            "Could not decode glTF texture for ORM composition"
        );
        return false;
    }
    return true;
}

static bool btbh_gpu_model_load_orm_texture(
    BTBHGPUWindow *window,
    const cgltf_texture_view *occlusion_view,
    const cgltf_texture_view *metallic_roughness_view,
    const char *model_path,
    BTBHGPUTexture **texture,
    BTBHGPUModelReport *model_report
) {
    *texture = NULL;
    if (
        (occlusion_view == NULL ||
            occlusion_view->texture == NULL) &&
        (metallic_roughness_view == NULL ||
            metallic_roughness_view->texture == NULL)
    ) {
        return true;
    }
    uint8_t *occlusion = NULL;
    uint32_t occlusion_width = 0u;
    uint32_t occlusion_height = 0u;
    uint8_t *metallic_roughness = NULL;
    uint32_t metallic_roughness_width = 0u;
    uint32_t metallic_roughness_height = 0u;
    uint8_t *composed = NULL;
    uint32_t composed_width = 0u;
    uint32_t composed_height = 0u;
    bool success = btbh_gpu_model_decode_texture(
        occlusion_view,
        model_path,
        &occlusion,
        &occlusion_width,
        &occlusion_height,
        model_report
    );
    if (success) {
        success = btbh_gpu_model_decode_texture(
            metallic_roughness_view,
            model_path,
            &metallic_roughness,
            &metallic_roughness_width,
            &metallic_roughness_height,
            model_report
        );
    }
    if (success) {
        success = btbh_gpu_model_compose_orm_rgba8(
            occlusion,
            occlusion_width,
            occlusion_height,
            metallic_roughness,
            metallic_roughness_width,
            metallic_roughness_height,
            &composed,
            &composed_width,
            &composed_height
        );
        if (!success) {
            btbh_gpu_model_error(
                model_report,
                "Could not compose glTF ORM texture"
            );
        }
    }
    if (success) {
        BTBHGPUTextureReport texture_report = {0};
        *texture = btbh_gpu_window_texture_create_rgba8_mipmapped(
            window,
            composed,
            composed_width,
            composed_height,
            BTBH_GPU_TEXTURE_FILTER_TRILINEAR,
            BTBH_GPU_TEXTURE_WRAP_REPEAT,
            &texture_report
        );
        success = *texture != NULL;
        if (!success) {
            btbh_gpu_model_error(
                model_report,
                texture_report.message[0] != '\0'
                    ? texture_report.message
                    : "Could not upload composed glTF ORM texture"
            );
        }
    }
    free(composed);
    free(metallic_roughness);
    free(occlusion);
    return success;
}

static bool btbh_gpu_model_load_texture(
    BTBHGPUWindow *window,
    const cgltf_texture_view *view,
    const char *model_path,
    SDL_GPUTextureFormat format,
    BTBHGPUTexture **texture,
    BTBHGPUModelReport *model_report
) {
    *texture = NULL;
    if (view == NULL || view->texture == NULL) {
        return true;
    }
    if (view->texture->image == NULL) {
        btbh_gpu_model_error(
            model_report,
            "glTF texture does not reference an image"
        );
        return false;
    }
    const cgltf_image *image = view->texture->image;
    if (image->buffer_view == NULL) {
        char *texture_path = btbh_gpu_model_resolve_texture_path(
            model_path,
            image->uri
        );
        if (texture_path == NULL) {
            btbh_gpu_model_error(
                model_report,
                image->uri != NULL &&
                        strncmp(image->uri, "data:", 5u) == 0
                    ? "GPU glTF import does not support data-URI textures"
                    : "Could not resolve external glTF texture path"
            );
            return false;
        }
        BTBHGPUTextureReport texture_report = {0};
        *texture = btbh_gpu_window_texture_load_format(
            window,
            texture_path,
            1u,
            BTBH_GPU_TEXTURE_FILTER_TRILINEAR,
            BTBH_GPU_TEXTURE_WRAP_REPEAT,
            format,
            &texture_report
        );
        free(texture_path);
        if (*texture != NULL) {
            return true;
        }
        btbh_gpu_model_error(
            model_report,
            texture_report.message[0] != '\0'
                ? texture_report.message
                : "Could not decode or upload external glTF texture"
        );
        return false;
    }
    const cgltf_buffer_view *buffer_view =
        image->buffer_view;
    if (
        buffer_view->buffer == NULL ||
        buffer_view->buffer->data == NULL ||
        buffer_view->offset > buffer_view->buffer->size ||
        buffer_view->size >
            buffer_view->buffer->size - buffer_view->offset
    ) {
        btbh_gpu_model_error(
            model_report,
            "glTF embedded texture buffer view is invalid"
        );
        return false;
    }
    const uint8_t *encoded =
        (const uint8_t *)buffer_view->buffer->data +
        buffer_view->offset;
    if (
        buffer_view->size == 0u ||
        buffer_view->size > (cgltf_size)INT_MAX
    ) {
        btbh_gpu_model_error(
            model_report,
            "glTF embedded texture buffer view size is invalid"
        );
        return false;
    }
    BTBHGPUTextureReport texture_report = {0};
    *texture = btbh_gpu_window_texture_load_memory_format(
        window,
        encoded,
        (size_t)buffer_view->size,
        1u,
        BTBH_GPU_TEXTURE_FILTER_TRILINEAR,
        BTBH_GPU_TEXTURE_WRAP_REPEAT,
        format,
        &texture_report
    );
    if (*texture != NULL) {
        return true;
    }
    btbh_gpu_model_error(
        model_report,
        texture_report.message[0] != '\0'
            ? texture_report.message
            : "Could not decode or upload embedded glTF texture"
    );
    return false;
}

static bool btbh_gpu_model_load_gltf_materials(
    BTBHGPUModel *model,
    const cgltf_data *data,
    const char *model_path,
    bool load_material_textures,
    BTBHGPUModelReport *report
) {
    model->materials = calloc(
        model->material_count,
        sizeof(*model->materials)
    );
    if (model->materials == NULL) {
        btbh_gpu_model_error(
            report,
            "Could not allocate GPU model materials"
        );
        return false;
    }
    for (
        uint32_t index = 0u;
        index < model->material_count;
        index += 1u
    ) {
        btbh_gpu_model_material_default(&model->materials[index]);
    }
    for (
        cgltf_size source_index = 0u;
        source_index < data->materials_count;
        source_index += 1u
    ) {
        const cgltf_material *source =
            &data->materials[source_index];
        BTBHGPUModelMaterialInfo *destination =
            &model->materials[source_index + 1u];
        destination->roughness = source
            ->pbr_metallic_roughness.roughness_factor;
        destination->metalness = source
            ->pbr_metallic_roughness.metallic_factor;
        if (source->occlusion_texture.texture != NULL) {
            destination->occlusion =
                source->occlusion_texture.scale;
        }
        if (source->has_pbr_metallic_roughness) {
            const cgltf_float *factor = source
                ->pbr_metallic_roughness.base_color_factor;
            destination->albedo_red =
                btbh_gpu_model_color_channel(factor[0]);
            destination->albedo_green =
                btbh_gpu_model_color_channel(factor[1]);
            destination->albedo_blue =
                btbh_gpu_model_color_channel(factor[2]);
            destination->albedo_alpha =
                btbh_gpu_model_color_channel(factor[3]);
            if (load_material_textures && !btbh_gpu_model_load_texture(
                    model->owner,
                    &source->pbr_metallic_roughness.base_color_texture,
                    model_path,
                    SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB,
                    &destination->albedo_texture,
                    report
                )) {
                return false;
            }
        }
        if (load_material_textures && !btbh_gpu_model_load_orm_texture(
                model->owner,
                &source->occlusion_texture,
                &source->pbr_metallic_roughness
                    .metallic_roughness_texture,
                model_path,
                &destination->orm_texture,
                report
            )) {
            return false;
        }
        if (load_material_textures && !btbh_gpu_model_load_texture(
                    model->owner,
                    &source->normal_texture,
                    model_path,
                    SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                    &destination->normal_texture,
                    report
                )) {
            return false;
        }
        if (source->normal_texture.texture != NULL) {
            destination->normal_scale = source->normal_texture.scale;
        }
        destination->emission_red =
            btbh_gpu_model_color_channel(source->emissive_factor[0]);
        destination->emission_green =
            btbh_gpu_model_color_channel(source->emissive_factor[1]);
        destination->emission_blue =
            btbh_gpu_model_color_channel(source->emissive_factor[2]);
        destination->emission_energy =
            source->has_emissive_strength
                ? source->emissive_strength.emissive_strength
                : 1.0f;
        if (load_material_textures && !btbh_gpu_model_load_texture(
                model->owner,
                &source->emissive_texture,
                model_path,
                SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB,
                &destination->emission_texture,
                report
            )) {
            return false;
        }
        destination->unlit_enabled = source->unlit ? 1u : 0u;
        destination->double_sided_enabled =
            source->double_sided ? 1u : 0u;
        if (source->alpha_mode == cgltf_alpha_mode_mask) {
            destination->alpha_cutoff = source->alpha_cutoff;
        } else if (source->alpha_mode == cgltf_alpha_mode_blend) {
            destination->transparency_enabled = 1u;
        }
    }
    return true;
}

static void btbh_gpu_model_unlink(BTBHGPUModel *model) {
    BTBHGPUWindow *owner = model->owner;
    if (owner == NULL) {
        return;
    }
    if (model->previous != NULL) {
        model->previous->next = model->next;
    } else if (owner->model_head == model) {
        owner->model_head = model->next;
    }
    if (model->next != NULL) {
        model->next->previous = model->previous;
    }
    model->owner = NULL;
    model->previous = NULL;
    model->next = NULL;
}

static void btbh_gpu_model_release_handles(
    BTBHGPUModel *model,
    SDL_GPUDevice *device
) {
    if (model->materials != NULL) {
        for (
            uint32_t index = 0u;
            index < model->material_count;
            index += 1u
        ) {
            BTBHGPUModelMaterialInfo *material =
                &model->materials[index];
            if (material->albedo_texture != NULL) {
                btbh_gpu_texture_destroy(material->albedo_texture);
                material->albedo_texture = NULL;
            }
            if (material->emission_texture != NULL) {
                btbh_gpu_texture_destroy(material->emission_texture);
                material->emission_texture = NULL;
            }
            if (material->normal_texture != NULL) {
                btbh_gpu_texture_destroy(material->normal_texture);
                material->normal_texture = NULL;
            }
            if (material->orm_texture != NULL) {
                btbh_gpu_texture_destroy(material->orm_texture);
                material->orm_texture = NULL;
            }
        }
    }
    if (model->primitives == NULL) {
        return;
    }
    for (uint32_t index = 0; index < model->primitive_count; index += 1u) {
        BTBHGPUModelPrimitive *primitive = &model->primitives[index];
        if (device != NULL) {
            if (primitive->index_buffer != NULL) {
                SDL_ReleaseGPUBuffer(device, primitive->index_buffer);
            }
            if (primitive->vertex_buffer != NULL) {
                SDL_ReleaseGPUBuffer(device, primitive->vertex_buffer);
            }
        }
        primitive->index_buffer = NULL;
        primitive->vertex_buffer = NULL;
        free(primitive->positions);
        free(primitive->indices);
        primitive->positions = NULL;
        primitive->indices = NULL;
    }
}

static SDL_GPUBuffer *btbh_gpu_model_upload_buffer(
    SDL_GPUDevice *device,
    SDL_GPUBufferUsageFlags usage,
    const void *bytes,
    uint32_t size
) {
    SDL_GPUBufferCreateInfo buffer_info = {0};
    buffer_info.usage = usage;
    buffer_info.size = size;
    SDL_GPUBuffer *buffer = SDL_CreateGPUBuffer(device, &buffer_info);

    SDL_GPUTransferBufferCreateInfo transfer_info = {0};
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size = size;
    SDL_GPUTransferBuffer *transfer =
        SDL_CreateGPUTransferBuffer(device, &transfer_info);
    if (buffer == NULL || transfer == NULL) {
        if (transfer != NULL) {
            SDL_ReleaseGPUTransferBuffer(device, transfer);
        }
        if (buffer != NULL) {
            SDL_ReleaseGPUBuffer(device, buffer);
        }
        return NULL;
    }

    void *mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
    if (mapped == NULL) {
        SDL_ReleaseGPUTransferBuffer(device, transfer);
        SDL_ReleaseGPUBuffer(device, buffer);
        return NULL;
    }
    memcpy(mapped, bytes, size);
    SDL_UnmapGPUTransferBuffer(device, transfer);

    SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass *copy =
        command != NULL ? SDL_BeginGPUCopyPass(command) : NULL;
    if (copy == NULL) {
        if (command != NULL) {
            (void) SDL_CancelGPUCommandBuffer(command);
        }
        SDL_ReleaseGPUTransferBuffer(device, transfer);
        SDL_ReleaseGPUBuffer(device, buffer);
        return NULL;
    }

    SDL_GPUTransferBufferLocation source = {0};
    source.transfer_buffer = transfer;
    SDL_GPUBufferRegion destination = {0};
    destination.buffer = buffer;
    destination.size = size;
    SDL_UploadToGPUBuffer(copy, &source, &destination, false);
    SDL_EndGPUCopyPass(copy);
    const bool submitted = SDL_SubmitGPUCommandBuffer(command);
    SDL_ReleaseGPUTransferBuffer(device, transfer);
    if (!submitted) {
        SDL_ReleaseGPUBuffer(device, buffer);
        return NULL;
    }
    return buffer;
}

static const cgltf_accessor *btbh_gpu_model_attribute(
    const cgltf_primitive *primitive,
    cgltf_attribute_type type,
    cgltf_int set_index
) {
    for (
        cgltf_size index = 0;
        index < primitive->attributes_count;
        index += 1
    ) {
        const cgltf_attribute *attribute = &primitive->attributes[index];
        if (attribute->type == type && attribute->index == set_index) {
            return attribute->data;
        }
    }
    return NULL;
}

static bool btbh_gpu_model_unpack_attribute(
    const cgltf_accessor *accessor,
    uint32_t vertex_count,
    BTBHGPUFloatAttribute *attribute
) {
    memset(attribute, 0, sizeof(*attribute));
    if (accessor == NULL) {
        return true;
    }
    const cgltf_size component_count =
        cgltf_num_components(accessor->type);
    if (
        accessor->count != vertex_count ||
        component_count == 0 ||
        component_count > 4 ||
        accessor->count > SIZE_MAX / component_count ||
        accessor->count * component_count >
            SIZE_MAX / sizeof(float)
    ) {
        return false;
    }
    const cgltf_size float_count =
        accessor->count * component_count;
    attribute->values = malloc(float_count * sizeof(float));
    if (attribute->values == NULL) {
        return false;
    }
    if (
        cgltf_accessor_unpack_floats(
            accessor,
            attribute->values,
            float_count
        ) != float_count
    ) {
        free(attribute->values);
        memset(attribute, 0, sizeof(*attribute));
        return false;
    }
    attribute->component_count = (uint32_t) component_count;
    return true;
}

static void btbh_gpu_model_copy_attribute(
    const BTBHGPUFloatAttribute *attribute,
    uint32_t index,
    float *destination,
    uint32_t destination_count
) {
    if (attribute->values == NULL) {
        return;
    }
    const uint32_t count =
        attribute->component_count < destination_count
            ? attribute->component_count
            : destination_count;
    memcpy(
        destination,
        attribute->values +
            (size_t) index * attribute->component_count,
        (size_t) count * sizeof(float)
    );
}

static void btbh_gpu_model_release_attribute(
    BTBHGPUFloatAttribute *attribute
) {
    free(attribute->values);
    memset(attribute, 0, sizeof(*attribute));
}

static void btbh_gpu_model_transform_position(
    float position[3],
    const float transform[16]
) {
    const float x = position[0];
    const float y = position[1];
    const float z = position[2];
    position[0] =
        transform[0] * x +
        transform[4] * y +
        transform[8] * z +
        transform[12];
    position[1] =
        transform[1] * x +
        transform[5] * y +
        transform[9] * z +
        transform[13];
    position[2] =
        transform[2] * x +
        transform[6] * y +
        transform[10] * z +
        transform[14];
}

static void btbh_gpu_model_normal_matrix(
    const float transform[16],
    float normal_matrix[9]
) {
    const float a = transform[0];
    const float b = transform[4];
    const float c = transform[8];
    const float d = transform[1];
    const float e = transform[5];
    const float f = transform[9];
    const float g = transform[2];
    const float h = transform[6];
    const float i = transform[10];
    const float determinant =
        a * (e * i - f * h) -
        b * (d * i - f * g) +
        c * (d * h - e * g);
    const float inverse_determinant =
        fabsf(determinant) > FLT_EPSILON
            ? 1.0f / determinant
            : 1.0f;
    normal_matrix[0] = (e * i - f * h) * inverse_determinant;
    normal_matrix[1] = (f * g - d * i) * inverse_determinant;
    normal_matrix[2] = (d * h - e * g) * inverse_determinant;
    normal_matrix[3] = (c * h - b * i) * inverse_determinant;
    normal_matrix[4] = (a * i - c * g) * inverse_determinant;
    normal_matrix[5] = (b * g - a * h) * inverse_determinant;
    normal_matrix[6] = (b * f - c * e) * inverse_determinant;
    normal_matrix[7] = (c * d - a * f) * inverse_determinant;
    normal_matrix[8] = (a * e - b * d) * inverse_determinant;
}

static void btbh_gpu_model_transform_direction(
    float direction[3],
    const float normal_matrix[9]
) {
    const float x = direction[0];
    const float y = direction[1];
    const float z = direction[2];
    direction[0] =
        normal_matrix[0] * x +
        normal_matrix[1] * y +
        normal_matrix[2] * z;
    direction[1] =
        normal_matrix[3] * x +
        normal_matrix[4] * y +
        normal_matrix[5] * z;
    direction[2] =
        normal_matrix[6] * x +
        normal_matrix[7] * y +
        normal_matrix[8] * z;
    const float length = sqrtf(
        direction[0] * direction[0] +
        direction[1] * direction[1] +
        direction[2] * direction[2]
    );
    if (length > FLT_EPSILON) {
        direction[0] /= length;
        direction[1] /= length;
        direction[2] /= length;
    }
}

static void btbh_gpu_model_transform_tangent(
    float tangent[4],
    const float transform[16]
) {
    const float x = tangent[0];
    const float y = tangent[1];
    const float z = tangent[2];
    tangent[0] =
        transform[0] * x +
        transform[4] * y +
        transform[8] * z;
    tangent[1] =
        transform[1] * x +
        transform[5] * y +
        transform[9] * z;
    tangent[2] =
        transform[2] * x +
        transform[6] * y +
        transform[10] * z;
    const float length = sqrtf(
        tangent[0] * tangent[0] +
        tangent[1] * tangent[1] +
        tangent[2] * tangent[2]
    );
    if (length > FLT_EPSILON) {
        tangent[0] /= length;
        tangent[1] /= length;
        tangent[2] /= length;
    }
    const float determinant =
        transform[0] * (
            transform[5] * transform[10] -
            transform[9] * transform[6]
        ) -
        transform[4] * (
            transform[1] * transform[10] -
            transform[9] * transform[2]
        ) +
        transform[8] * (
            transform[1] * transform[6] -
            transform[5] * transform[2]
        );
    if (determinant < 0.0f) {
        tangent[3] = -tangent[3];
    }
}

static void btbh_gpu_model_include_position(
    BTBHGPUModel *model,
    const float position[3]
) {
    for (uint32_t axis = 0; axis < 3u; axis += 1u) {
        if (position[axis] < model->minimum[axis]) {
            model->minimum[axis] = position[axis];
        }
        if (position[axis] > model->maximum[axis]) {
            model->maximum[axis] = position[axis];
        }
    }
}

static bool btbh_gpu_model_decode_vertices(
    BTBHGPUModel *model,
    const cgltf_primitive *primitive,
    const float world_transform[16],
    BTBHGPUModelPrimitive *destination
) {
    const cgltf_accessor *positions = btbh_gpu_model_attribute(
        primitive,
        cgltf_attribute_type_position,
        0
    );
    if (
        positions == NULL ||
        positions->count == 0 ||
        positions->count > UINT32_MAX ||
        positions->count >
            (cgltf_size) (UINT32_MAX / sizeof(BTBHGPUModelVertex))
    ) {
        return false;
    }

    const uint32_t vertex_count = (uint32_t) positions->count;
    const uint32_t vertex_size =
        vertex_count * (uint32_t) sizeof(BTBHGPUModelVertex);
    BTBHGPUModelVertex *vertices = calloc(
        vertex_count,
        sizeof(*vertices)
    );
    if (vertices == NULL) {
        return false;
    }

    const cgltf_accessor *accessors[7] = {
        positions,
        btbh_gpu_model_attribute(
            primitive,
            cgltf_attribute_type_normal,
            0
        ),
        btbh_gpu_model_attribute(
            primitive,
            cgltf_attribute_type_texcoord,
            0
        ),
        btbh_gpu_model_attribute(
            primitive,
            cgltf_attribute_type_tangent,
            0
        ),
        btbh_gpu_model_attribute(
            primitive,
            cgltf_attribute_type_color,
            0
        ),
        btbh_gpu_model_attribute(
            primitive,
            cgltf_attribute_type_joints,
            0
        ),
        btbh_gpu_model_attribute(
            primitive,
            cgltf_attribute_type_weights,
            0
        )
    };
    BTBHGPUFloatAttribute attributes[7] = {0};
    bool valid = true;
    for (uint32_t index = 0; valid && index < 7u; index += 1u) {
        valid = btbh_gpu_model_unpack_attribute(
            accessors[index],
            vertex_count,
            &attributes[index]
        );
    }
    if (!valid || attributes[0].component_count != 3u) {
        for (uint32_t index = 0; index < 7u; index += 1u) {
            btbh_gpu_model_release_attribute(&attributes[index]);
        }
        free(vertices);
        return false;
    }

    float normal_matrix[9];
    btbh_gpu_model_normal_matrix(world_transform, normal_matrix);
    for (uint32_t index = 0; index < vertex_count; index += 1u) {
        BTBHGPUModelVertex *vertex = &vertices[index];
        vertex->normal[1] = 1.0f;
        vertex->tangent[0] = 1.0f;
        vertex->tangent[3] = 1.0f;
        vertex->color[0] = 1.0f;
        vertex->color[1] = 1.0f;
        vertex->color[2] = 1.0f;
        vertex->color[3] = 1.0f;
        vertex->weights[0] = 1.0f;
        btbh_gpu_model_copy_attribute(
            &attributes[0], index, vertex->position, 3
        );
        btbh_gpu_model_copy_attribute(
            &attributes[1], index, vertex->normal, 3
        );
        btbh_gpu_model_copy_attribute(
            &attributes[2], index, vertex->texture_coordinate, 2
        );
        btbh_gpu_model_copy_attribute(
            &attributes[3], index, vertex->tangent, 4
        );
        btbh_gpu_model_copy_attribute(
            &attributes[4], index, vertex->color, 4
        );
        btbh_gpu_model_copy_attribute(
            &attributes[5], index, vertex->joints, 4
        );
        btbh_gpu_model_copy_attribute(
            &attributes[6], index, vertex->weights, 4
        );
        btbh_gpu_model_pack_gltf_bone_weights(
            vertex->joints,
            vertex->weights
        );
        btbh_gpu_model_transform_position(
            vertex->position,
            world_transform
        );
        btbh_gpu_model_transform_direction(
            vertex->normal,
            normal_matrix
        );
        btbh_gpu_model_transform_tangent(
            vertex->tangent,
            world_transform
        );
        btbh_gpu_model_include_position(model, vertex->position);
    }
    for (uint32_t index = 0; index < 7u; index += 1u) {
        btbh_gpu_model_release_attribute(&attributes[index]);
    }

    destination->vertex_buffer = btbh_gpu_model_upload_buffer(
        model->owner->device->gpu,
        SDL_GPU_BUFFERUSAGE_VERTEX,
        vertices,
        vertex_size
    );
    if (destination->vertex_buffer == NULL) {
        free(vertices);
        return false;
    }
    destination->positions = malloc(
        vertex_count * 3u * (uint32_t) sizeof(float)
    );
    if (destination->positions == NULL) {
        free(vertices);
        return false;
    }
    for (uint32_t index = 0u; index < vertex_count; index += 1u) {
        memcpy(
            &destination->positions[index * 3u],
            vertices[index].position,
            3u * sizeof(float)
        );
    }
    free(vertices);
    destination->vertex_count = vertex_count;
    return true;
}

static bool btbh_gpu_model_decode_indices(
    BTBHGPUModel *model,
    const cgltf_primitive *primitive,
    BTBHGPUModelPrimitive *destination
) {
    if (primitive->indices == NULL) {
        return true;
    }
    if (
        primitive->indices->count == 0 ||
        primitive->indices->count > UINT32_MAX ||
        primitive->indices->count >
            (cgltf_size) (UINT32_MAX / sizeof(uint32_t))
    ) {
        return false;
    }
    const uint32_t index_count = (uint32_t) primitive->indices->count;
    const uint32_t index_size =
        index_count * (uint32_t) sizeof(uint32_t);
    uint32_t *indices = malloc(index_size);
    if (indices == NULL) {
        return false;
    }
    const cgltf_accessor *accessor = primitive->indices;
    bool valid = true;
    if (accessor->buffer_view == NULL) {
        memset(indices, 0, index_size);
    } else {
        cgltf_accessor base_accessor = *accessor;
        base_accessor.is_sparse = 0;
        valid = cgltf_accessor_unpack_indices(
            &base_accessor,
            indices,
            sizeof(uint32_t),
            index_count
        ) == index_count;
    }
    if (valid && accessor->is_sparse) {
        const cgltf_accessor_sparse *sparse = &accessor->sparse;
        const uint8_t *sparse_indices =
            cgltf_buffer_view_data(sparse->indices_buffer_view);
        const uint8_t *sparse_values =
            cgltf_buffer_view_data(sparse->values_buffer_view);
        const cgltf_size sparse_index_stride =
            cgltf_component_size(sparse->indices_component_type);
        const cgltf_size sparse_value_stride =
            cgltf_component_size(accessor->component_type);
        valid =
            sparse_indices != NULL &&
            sparse_values != NULL &&
            sparse_index_stride > 0 &&
            sparse_value_stride > 0;
        if (valid) {
            sparse_indices += sparse->indices_byte_offset;
            sparse_values += sparse->values_byte_offset;
        }
        for (
            cgltf_size sparse_index = 0;
            valid && sparse_index < sparse->count;
            sparse_index += 1
        ) {
            const cgltf_size destination_index =
                cgltf_component_read_index(
                    sparse_indices,
                    sparse->indices_component_type
                );
            const cgltf_size value = cgltf_component_read_index(
                sparse_values,
                accessor->component_type
            );
            if (
                destination_index >= index_count ||
                value > UINT32_MAX
            ) {
                valid = false;
                break;
            }
            indices[destination_index] = (uint32_t) value;
            sparse_indices += sparse_index_stride;
            sparse_values += sparse_value_stride;
        }
    }
    for (
        uint32_t index = 0;
        valid && index < index_count;
        index += 1u
    ) {
        valid = indices[index] < destination->vertex_count;
    }
    if (valid) {
        destination->index_buffer = btbh_gpu_model_upload_buffer(
            model->owner->device->gpu,
            SDL_GPU_BUFFERUSAGE_INDEX,
            indices,
            index_size
        );
        valid = destination->index_buffer != NULL;
    }
    if (!valid) {
        free(indices);
        return false;
    }
    destination->indices = indices;
    destination->index_count = index_count;
    return true;
}

static uint32_t btbh_gpu_model_primitive_count(
    const cgltf_data *data
) {
    uint32_t count = 0;
    for (
        cgltf_size node_index = 0;
        node_index < data->nodes_count;
        node_index += 1
    ) {
        const cgltf_mesh *mesh = data->nodes[node_index].mesh;
        if (mesh == NULL) {
            continue;
        }
        for (
            cgltf_size primitive_index = 0;
            primitive_index < mesh->primitives_count;
            primitive_index += 1
        ) {
            const cgltf_primitive *primitive =
                &mesh->primitives[primitive_index];
            if (
                primitive->type == cgltf_primitive_type_triangles &&
                btbh_gpu_model_attribute(
                    primitive,
                    cgltf_attribute_type_position,
                    0
                ) != NULL &&
                count < UINT32_MAX
            ) {
                count += 1u;
            }
        }
    }
    return count;
}

#if defined(BTBH_GPU_HAS_ASSIMP)
static uint8_t btbh_gpu_model_assimp_color_channel(float value) {
    const float clamped = fmaxf(0.0f, fminf(1.0f, value));
    return (uint8_t)(clamped * 255.0f);
}

static void btbh_gpu_model_load_assimp_material(
    BTBHGPUModelMaterialInfo *destination,
    const struct aiMaterial *source
) {
    struct aiColor4D color;
    if (
        aiGetMaterialColor(
            source,
            AI_MATKEY_BASE_COLOR,
            &color
        ) == AI_SUCCESS ||
        aiGetMaterialColor(
            source,
            AI_MATKEY_COLOR_DIFFUSE,
            &color
        ) == AI_SUCCESS
    ) {
        destination->albedo_red =
            btbh_gpu_model_assimp_color_channel(color.r);
        destination->albedo_green =
            btbh_gpu_model_assimp_color_channel(color.g);
        destination->albedo_blue =
            btbh_gpu_model_assimp_color_channel(color.b);
        destination->albedo_alpha =
            btbh_gpu_model_assimp_color_channel(color.a);
    }

    float opacity = 1.0f;
    if (
        destination->albedo_alpha == 255u &&
        aiGetMaterialFloat(
            source,
            AI_MATKEY_OPACITY,
            &opacity
        ) == AI_SUCCESS
    ) {
        destination->albedo_alpha =
            btbh_gpu_model_assimp_color_channel(opacity);
    }

    bool has_emission = false;
    if (
        aiGetMaterialColor(
            source,
            AI_MATKEY_COLOR_EMISSIVE,
            &color
        ) == AI_SUCCESS
    ) {
        destination->emission_red =
            btbh_gpu_model_assimp_color_channel(color.r);
        destination->emission_green =
            btbh_gpu_model_assimp_color_channel(color.g);
        destination->emission_blue =
            btbh_gpu_model_assimp_color_channel(color.b);
        has_emission =
            destination->emission_red != 0u ||
            destination->emission_green != 0u ||
            destination->emission_blue != 0u;
    }
    float emission_energy = 0.0f;
    if (
        aiGetMaterialFloat(
            source,
            AI_MATKEY_EMISSIVE_INTENSITY,
            &emission_energy
        ) == AI_SUCCESS
    ) {
        destination->emission_energy = emission_energy;
    } else if (has_emission) {
        destination->emission_energy = 1.0f;
    }

    float roughness = 0.0f;
    if (
        aiGetMaterialFloat(
            source,
            AI_MATKEY_ROUGHNESS_FACTOR,
            &roughness
        ) == AI_SUCCESS
    ) {
        destination->roughness = roughness;
    } else {
        float shininess = 0.0f;
        if (
            aiGetMaterialFloat(
                source,
                AI_MATKEY_SHININESS,
                &shininess
            ) == AI_SUCCESS &&
            shininess > 0.0f
        ) {
            const float normalized =
                fminf(shininess / 1000.0f, 1.0f);
            float derived = 1.0f - sqrtf(normalized);
            float strength = 1.0f;
            if (
                aiGetMaterialFloat(
                    source,
                    AI_MATKEY_SHININESS_STRENGTH,
                    &strength
                ) == AI_SUCCESS
            ) {
                strength = fminf(fmaxf(strength, 0.0f), 1.0f);
                derived +=
                    (1.0f - derived) * (1.0f - strength);
            }
            destination->roughness =
                fminf(fmaxf(derived, 0.0f), 1.0f);
        }
    }

    float metalness = 0.0f;
    if (
        aiGetMaterialFloat(
            source,
            AI_MATKEY_METALLIC_FACTOR,
            &metalness
        ) == AI_SUCCESS
    ) {
        destination->metalness = metalness;
    }
    float normal_scale = 0.0f;
    if (
        aiGetMaterialFloat(
            source,
            AI_MATKEY_BUMPSCALING,
            &normal_scale
        ) == AI_SUCCESS
    ) {
        destination->normal_scale = normal_scale;
    }
    int shading_mode = 0;
    if (
        aiGetMaterialInteger(
            source,
            AI_MATKEY_SHADING_MODEL,
            &shading_mode
        ) == AI_SUCCESS
    ) {
        destination->unlit_enabled =
            shading_mode == aiShadingMode_Unlit ? 1u : 0u;
    }
}

static uint32_t btbh_gpu_model_assimp_primitive_count(
    const struct aiScene *scene
) {
    uint32_t count = 0;
    for (
        unsigned int index = 0;
        index < scene->mNumMeshes;
        index += 1u
    ) {
        const struct aiMesh *mesh = scene->mMeshes[index];
        if (
            mesh != NULL &&
            mesh->mVertices != NULL &&
            mesh->mNumVertices > 0u &&
            mesh->mNumFaces > 0u &&
            (mesh->mPrimitiveTypes & aiPrimitiveType_TRIANGLE) != 0u &&
            count < UINT32_MAX
        ) {
            count += 1u;
        }
    }
    return count;
}

static bool btbh_gpu_model_decode_assimp_vertices(
    BTBHGPUModel *model,
    const struct aiMesh *mesh,
    BTBHGPUModelPrimitive *destination
) {
    if (
        mesh->mNumVertices == 0u ||
        mesh->mNumVertices >
            UINT32_MAX / (uint32_t) sizeof(BTBHGPUModelVertex)
    ) {
        return false;
    }
    const uint32_t vertex_count = (uint32_t) mesh->mNumVertices;
    const uint32_t vertex_size =
        vertex_count * (uint32_t) sizeof(BTBHGPUModelVertex);
    BTBHGPUModelVertex *vertices = calloc(
        vertex_count,
        sizeof(*vertices)
    );
    if (vertices == NULL) {
        return false;
    }

    for (uint32_t index = 0; index < vertex_count; index += 1u) {
        BTBHGPUModelVertex *vertex = &vertices[index];
        const struct aiVector3D *position = &mesh->mVertices[index];
        vertex->position[0] = position->x;
        vertex->position[1] = position->y;
        vertex->position[2] = position->z;
        vertex->normal[1] = 1.0f;
        vertex->tangent[0] = 1.0f;
        vertex->tangent[3] = 1.0f;
        vertex->color[0] = 1.0f;
        vertex->color[1] = 1.0f;
        vertex->color[2] = 1.0f;
        vertex->color[3] = 1.0f;

        if (mesh->mNormals != NULL) {
            vertex->normal[0] = mesh->mNormals[index].x;
            vertex->normal[1] = mesh->mNormals[index].y;
            vertex->normal[2] = mesh->mNormals[index].z;
        }
        if (mesh->mTextureCoords[0] != NULL) {
            vertex->texture_coordinate[0] =
                mesh->mTextureCoords[0][index].x;
            vertex->texture_coordinate[1] =
                mesh->mTextureCoords[0][index].y;
        }
        if (mesh->mTangents != NULL) {
            vertex->tangent[0] = mesh->mTangents[index].x;
            vertex->tangent[1] = mesh->mTangents[index].y;
            vertex->tangent[2] = mesh->mTangents[index].z;
            if (
                mesh->mNormals != NULL &&
                mesh->mBitangents != NULL
            ) {
                const float cross_x =
                    vertex->normal[1] * vertex->tangent[2] -
                    vertex->normal[2] * vertex->tangent[1];
                const float cross_y =
                    vertex->normal[2] * vertex->tangent[0] -
                    vertex->normal[0] * vertex->tangent[2];
                const float cross_z =
                    vertex->normal[0] * vertex->tangent[1] -
                    vertex->normal[1] * vertex->tangent[0];
                const struct aiVector3D *bitangent =
                    &mesh->mBitangents[index];
                const float handedness =
                    cross_x * bitangent->x +
                    cross_y * bitangent->y +
                    cross_z * bitangent->z;
                vertex->tangent[3] =
                    handedness < 0.0f ? -1.0f : 1.0f;
            }
        }
        if (mesh->mColors[0] != NULL) {
            vertex->color[0] = mesh->mColors[0][index].r;
            vertex->color[1] = mesh->mColors[0][index].g;
            vertex->color[2] = mesh->mColors[0][index].b;
            vertex->color[3] = mesh->mColors[0][index].a;
        }
        btbh_gpu_model_include_position(model, vertex->position);
    }
    if (mesh->mNumBones > UINT8_MAX + 1u) {
        free(vertices);
        return false;
    }
    for (uint32_t bone_index = 0u;
         bone_index < mesh->mNumBones;
         bone_index += 1u) {
        const struct aiBone *bone = mesh->mBones[bone_index];
        if (bone == NULL) {
            continue;
        }
        for (uint32_t weight_index = 0u;
             weight_index < bone->mNumWeights;
             weight_index += 1u) {
            const struct aiVertexWeight *influence =
                &bone->mWeights[weight_index];
            if (influence->mVertexId >= vertex_count) {
                continue;
            }
            (void) btbh_gpu_model_assign_bone_weight(
                vertices[influence->mVertexId].joints,
                vertices[influence->mVertexId].weights,
                bone_index,
                influence->mWeight
            );
        }
    }
    for (uint32_t index = 0u; index < vertex_count; index += 1u) {
        btbh_gpu_model_normalize_bone_weights(
            vertices[index].joints,
            vertices[index].weights
        );
    }

    destination->vertex_buffer = btbh_gpu_model_upload_buffer(
        model->owner->device->gpu,
        SDL_GPU_BUFFERUSAGE_VERTEX,
        vertices,
        vertex_size
    );
    if (destination->vertex_buffer == NULL) {
        free(vertices);
        return false;
    }
    destination->positions = malloc(
        vertex_count * 3u * (uint32_t) sizeof(float)
    );
    if (destination->positions == NULL) {
        free(vertices);
        return false;
    }
    for (uint32_t index = 0u; index < vertex_count; index += 1u) {
        memcpy(
            &destination->positions[index * 3u],
            vertices[index].position,
            3u * sizeof(float)
        );
    }
    free(vertices);
    destination->vertex_count = vertex_count;
    return true;
}

static bool btbh_gpu_model_decode_assimp_indices(
    BTBHGPUModel *model,
    const struct aiMesh *mesh,
    BTBHGPUModelPrimitive *destination
) {
    if (
        mesh->mNumFaces == 0u ||
        mesh->mNumFaces > UINT32_MAX / 3u ||
        mesh->mNumFaces * 3u >
            UINT32_MAX / (uint32_t) sizeof(uint32_t)
    ) {
        return false;
    }
    const uint32_t index_count = (uint32_t) mesh->mNumFaces * 3u;
    const uint32_t index_size =
        index_count * (uint32_t) sizeof(uint32_t);
    uint32_t *indices = malloc(index_size);
    if (indices == NULL) {
        return false;
    }
    bool valid = true;
    for (
        uint32_t face_index = 0;
        valid && face_index < (uint32_t) mesh->mNumFaces;
        face_index += 1u
    ) {
        const struct aiFace *face = &mesh->mFaces[face_index];
        valid =
            face->mNumIndices == 3u &&
            face->mIndices != NULL;
        for (
            uint32_t corner = 0;
            valid && corner < 3u;
            corner += 1u
        ) {
            const unsigned int source_index = face->mIndices[corner];
            valid = source_index < destination->vertex_count;
            if (valid) {
                const uint32_t destination_corner =
                    corner == 1u ? 2u : (corner == 2u ? 1u : 0u);
                indices[face_index * 3u + destination_corner] =
                    (uint32_t) source_index;
            }
        }
    }
    if (valid) {
        destination->index_buffer = btbh_gpu_model_upload_buffer(
            model->owner->device->gpu,
            SDL_GPU_BUFFERUSAGE_INDEX,
            indices,
            index_size
        );
        valid = destination->index_buffer != NULL;
    }
    if (!valid) {
        free(indices);
        return false;
    }
    destination->indices = indices;
    destination->index_count = index_count;
    return true;
}

void btbh_gpu_model_set_debug_name(BTBHGPUModel *model, const char *path) {
    if (model == NULL || path == NULL) {
        return;
    }
    const char *start = path;
    int separators = 0;
    for (const char *cursor = path + strlen(path); cursor > path; --cursor) {
        if (cursor[-1] == '/' || cursor[-1] == '\\') {
            separators += 1;
            if (separators == 2) {
                start = cursor;
                break;
            }
        }
    }
    (void)snprintf(model->debug_name, sizeof(model->debug_name), "%s", start);
}

static BTBHGPUModel *btbh_gpu_window_model_load_assimp(
    BTBHGPUWindow *window,
    const char *path,
    BTBHGPUModelReport *report
) {
    const unsigned int import_flags =
        aiProcess_Triangulate |
        aiProcess_JoinIdenticalVertices |
        aiProcess_GenUVCoords |
        aiProcess_FlipUVs |
        aiProcess_RemoveRedundantMaterials |
        aiProcess_GenSmoothNormals |
        aiProcess_CalcTangentSpace |
        aiProcess_PreTransformVertices |
        aiProcess_SortByPType |
        aiProcess_ImproveCacheLocality;
    const struct aiScene *scene = aiImportFile(path, import_flags);
    if (scene == NULL) {
        btbh_gpu_model_error(report, aiGetErrorString());
        return NULL;
    }

    const uint32_t primitive_count =
        btbh_gpu_model_assimp_primitive_count(scene);
    if (primitive_count == 0u) {
        aiReleaseImport(scene);
        btbh_gpu_model_error(
            report,
            "Assimp model contains no triangle meshes"
        );
        return NULL;
    }

    BTBHGPUModel *model = calloc(1, sizeof(*model));
    if (model == NULL) {
        aiReleaseImport(scene);
        btbh_gpu_model_error(report, "Could not allocate GPU model resource");
        return NULL;
    }
    btbh_gpu_model_set_debug_name(model, path);
    model->primitives = calloc(
        primitive_count,
        sizeof(*model->primitives)
    );
    if (model->primitives == NULL) {
        aiReleaseImport(scene);
        free(model);
        btbh_gpu_model_error(report, "Could not allocate GPU model meshes");
        return NULL;
    }
    model->owner = window;
    model->next = window->model_head;
    if (window->model_head != NULL) {
        window->model_head->previous = model;
    }
    window->model_head = model;
    model->primitive_count = primitive_count;
    model->material_count = scene->mNumMaterials > 0u
        ? (uint32_t) scene->mNumMaterials
        : 1u;
    model->materials = calloc(
        model->material_count,
        sizeof(*model->materials)
    );
    if (model->materials == NULL) {
        aiReleaseImport(scene);
        btbh_gpu_model_error(
            report,
            "Could not allocate GPU model materials"
        );
        btbh_gpu_model_destroy(model);
        return NULL;
    }
    for (
        uint32_t material_index = 0u;
        material_index < model->material_count;
        material_index += 1u
    ) {
        btbh_gpu_model_material_default(
            &model->materials[material_index]
        );
        if (
            material_index < scene->mNumMaterials &&
            scene->mMaterials[material_index] != NULL
        ) {
            btbh_gpu_model_load_assimp_material(
                &model->materials[material_index],
                scene->mMaterials[material_index]
            );
        }
    }
    for (uint32_t axis = 0; axis < 3u; axis += 1u) {
        model->minimum[axis] = FLT_MAX;
        model->maximum[axis] = -FLT_MAX;
    }

    uint32_t output_index = 0u;
    bool valid = true;
    for (
        unsigned int mesh_index = 0;
        valid && mesh_index < scene->mNumMeshes;
        mesh_index += 1u
    ) {
        const struct aiMesh *mesh = scene->mMeshes[mesh_index];
        if (
            mesh == NULL ||
            mesh->mVertices == NULL ||
        mesh->mNumVertices == 0u ||
        mesh->mNumFaces == 0u ||
        mesh->mFaces == NULL ||
        (mesh->mPrimitiveTypes & aiPrimitiveType_TRIANGLE) == 0u
        ) {
            continue;
        }
        BTBHGPUModelPrimitive *destination =
            &model->primitives[output_index];
        destination->skin_index = -1;
        destination->material_slot =
            mesh->mMaterialIndex < model->material_count
                ? (int32_t) mesh->mMaterialIndex
                : 0;
        valid =
            btbh_gpu_model_decode_assimp_vertices(
                model,
                mesh,
                destination
            ) &&
            btbh_gpu_model_decode_assimp_indices(
                model,
                mesh,
                destination
            );
        output_index += 1u;
    }
    aiReleaseImport(scene);

    if (!valid || output_index != primitive_count) {
        btbh_gpu_model_error(
            report,
            "Could not upload Assimp model meshes"
        );
        btbh_gpu_model_destroy(model);
        return NULL;
    }

    report->result = BTBH_GPU_OK;
    report->minimum_x = model->minimum[0];
    report->minimum_y = model->minimum[1];
    report->minimum_z = model->minimum[2];
    report->maximum_x = model->maximum[0];
    report->maximum_y = model->maximum[1];
    report->maximum_z = model->maximum[2];
    report->mesh_count = model->primitive_count;
    report->material_count = model->material_count;
    return model;
}
#endif

BTBHGPUModel *btbh_gpu_window_model_load_ex(
    BTBHGPUWindow *window,
    const char *path,
    uint32_t import_flags,
    BTBHGPUModelReport *report
) {
    if (
        window == NULL ||
        window->device == NULL ||
        window->device->gpu == NULL ||
        window->device->shutdown ||
        !window->claimed ||
        path == NULL ||
        report == NULL
    ) {
        return NULL;
    }
    memset(report, 0, sizeof(*report));

    cgltf_options options = {0};
    options.file.read = btbh_gpu_model_file_read;
    options.file.release = btbh_gpu_model_file_release;
    cgltf_data *data = NULL;
    cgltf_result result = cgltf_parse_file(&options, path, &data);
    if (result == cgltf_result_success) {
        result = cgltf_load_buffers(&options, data, path);
    }
    if (result == cgltf_result_success) {
        result = cgltf_validate(data);
    }
    if (result != cgltf_result_success || data == NULL) {
        cgltf_free(data);
#if defined(BTBH_GPU_HAS_ASSIMP)
        return btbh_gpu_window_model_load_assimp(
            window,
            path,
            report
        );
#else
        btbh_gpu_model_error(report, "Could not parse glTF model");
        return NULL;
#endif
    }

    if (data->materials_count >= UINT32_MAX) {
        cgltf_free(data);
        btbh_gpu_model_error(report, "glTF material count exceeds GPU limits");
        return NULL;
    }
    const uint32_t primitive_count =
        btbh_gpu_model_primitive_count(data);
    if (primitive_count == 0u) {
        cgltf_free(data);
        btbh_gpu_model_error(
            report,
            "glTF model contains no instantiated triangle meshes"
        );
        return NULL;
    }

    BTBHGPUModel *model = calloc(1, sizeof(*model));
    if (model == NULL) {
        cgltf_free(data);
        btbh_gpu_model_error(report, "Could not allocate GPU model resource");
        return NULL;
    }
    btbh_gpu_model_set_debug_name(model, path);
    model->primitives = calloc(
        primitive_count,
        sizeof(*model->primitives)
    );
    if (model->primitives == NULL) {
        cgltf_free(data);
        free(model);
        btbh_gpu_model_error(report, "Could not allocate GPU model meshes");
        return NULL;
    }
    model->owner = window;
    model->next = window->model_head;
    if (window->model_head != NULL) {
        window->model_head->previous = model;
    }
    window->model_head = model;
    model->primitive_count = primitive_count;
    model->material_count = (uint32_t) data->materials_count + 1u;
    if (!btbh_gpu_model_load_gltf_materials(
            model,
            data,
            path,
            (import_flags
                & BTBH_GPU_MODEL_IMPORT_IGNORE_MATERIAL_TEXTURES) == 0u,
            report
        )) {
        cgltf_free(data);
        btbh_gpu_model_destroy(model);
        return NULL;
    }
    for (uint32_t axis = 0; axis < 3u; axis += 1u) {
        model->minimum[axis] = FLT_MAX;
        model->maximum[axis] = -FLT_MAX;
    }

    uint32_t output_index = 0u;
    bool valid = true;
    for (
        cgltf_size node_index = 0;
        valid && node_index < data->nodes_count;
        node_index += 1
    ) {
        const cgltf_node *node = &data->nodes[node_index];
        const cgltf_mesh *mesh = node->mesh;
        if (mesh == NULL) {
            continue;
        }
        float world_transform[16];
        cgltf_node_transform_world(node, world_transform);
        static const float identity_transform[16] = {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f
        };
        const float *vertex_transform = node->skin != NULL
            ? identity_transform
            : world_transform;
        for (
            cgltf_size primitive_index = 0;
            valid && primitive_index < mesh->primitives_count;
            primitive_index += 1
        ) {
            const cgltf_primitive *primitive =
                &mesh->primitives[primitive_index];
            if (
                primitive->type != cgltf_primitive_type_triangles ||
                btbh_gpu_model_attribute(
                    primitive,
                    cgltf_attribute_type_position,
                    0
                ) == NULL
            ) {
                continue;
            }
            BTBHGPUModelPrimitive *destination =
                &model->primitives[output_index];
            destination->skin_index = node->skin != NULL
                ? (int32_t)(node->skin - data->skins)
                : -1;
            destination->material_slot = primitive->material != NULL
                ? (int32_t) (
                    primitive->material - data->materials + 1
                )
                : 0;
            valid =
                btbh_gpu_model_decode_vertices(
                    model,
                    primitive,
                    vertex_transform,
                    destination
                ) &&
                btbh_gpu_model_decode_indices(
                    model,
                    primitive,
                    destination
                );
            output_index += 1u;
        }
    }
    cgltf_free(data);

    if (!valid || output_index != primitive_count) {
        btbh_gpu_model_error(report, SDL_GetError());
        btbh_gpu_model_destroy(model);
        return NULL;
    }

    report->result = BTBH_GPU_OK;
    report->minimum_x = model->minimum[0];
    report->minimum_y = model->minimum[1];
    report->minimum_z = model->minimum[2];
    report->maximum_x = model->maximum[0];
    report->maximum_y = model->maximum[1];
    report->maximum_z = model->maximum[2];
    report->mesh_count = model->primitive_count;
    report->material_count = model->material_count;
    return model;
}

BTBHGPUModel *btbh_gpu_window_model_load(
    BTBHGPUWindow *window,
    const char *path,
    BTBHGPUModelReport *report
) {
    return btbh_gpu_window_model_load_ex(window, path, 0u, report);
}

int32_t btbh_gpu_model_material_slot(
    const BTBHGPUModel *model,
    uint32_t mesh_index
) {
    if (
        model == NULL ||
        model->primitives == NULL ||
        mesh_index >= model->primitive_count
    ) {
        return -1;
    }
    return model->primitives[mesh_index].material_slot;
}

BTBHGPUResult btbh_gpu_model_material_info(
    const BTBHGPUModel *model,
    uint32_t material_index,
    BTBHGPUModelMaterialInfo *info
) {
    if (
        model == NULL ||
        model->materials == NULL ||
        material_index >= model->material_count ||
        info == NULL
    ) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    *info = model->materials[material_index];
    return BTBH_GPU_OK;
}

BTBHGPUResult btbh_gpu_model_mesh_info(
    const BTBHGPUModel *model,
    uint32_t mesh_index,
    BTBHGPUModelMeshInfo *info
) {
    if (
        model == NULL ||
        model->primitives == NULL ||
        mesh_index >= model->primitive_count ||
        info == NULL
    ) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    const BTBHGPUModelPrimitive *primitive =
        &model->primitives[mesh_index];
    info->vertex_count = primitive->vertex_count;
    info->index_count = primitive->index_count;
    return BTBH_GPU_OK;
}

BTBHGPUResult btbh_gpu_model_mesh_copy_positions(
    const BTBHGPUModel *model,
    uint32_t mesh_index,
    float *positions,
    uint32_t float_capacity
) {
    if (
        model == NULL ||
        model->primitives == NULL ||
        mesh_index >= model->primitive_count
    ) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    const BTBHGPUModelPrimitive *primitive =
        &model->primitives[mesh_index];
    if (
        primitive->positions == NULL ||
        primitive->vertex_count > UINT32_MAX / 3u ||
        positions == NULL ||
        float_capacity < primitive->vertex_count * 3u
    ) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    memcpy(
        positions,
        primitive->positions,
        primitive->vertex_count * 3u * sizeof(float)
    );
    return BTBH_GPU_OK;
}

BTBHGPUResult btbh_gpu_model_mesh_copy_indices(
    const BTBHGPUModel *model,
    uint32_t mesh_index,
    uint32_t *indices,
    uint32_t index_capacity
) {
    if (
        model == NULL ||
        model->primitives == NULL ||
        mesh_index >= model->primitive_count
    ) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    const BTBHGPUModelPrimitive *primitive =
        &model->primitives[mesh_index];
    if (primitive->index_count == 0u) {
        return index_capacity == 0u
            ? BTBH_GPU_OK
            : BTBH_GPU_INVALID_ARGUMENT;
    }
    if (
        primitive->indices == NULL ||
        indices == NULL ||
        index_capacity < primitive->index_count
    ) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    memcpy(
        indices,
        primitive->indices,
        primitive->index_count * sizeof(uint32_t)
    );
    return BTBH_GPU_OK;
}

void btbh_gpu_model_destroy(BTBHGPUModel *model) {
    if (model == NULL) {
        return;
    }
    SDL_GPUDevice *device = NULL;
    if (
        model->owner != NULL &&
        model->owner->device != NULL &&
        !model->owner->device->shutdown
    ) {
        device = model->owner->device->gpu;
    }
    btbh_gpu_model_release_handles(model, device);
    btbh_gpu_model_unlink(model);
    free(model->primitives);
    free(model->materials);
    free(model);
}

void btbh_gpu_window_release_models(BTBHGPUWindow *window) {
    if (window == NULL) {
        return;
    }
    SDL_GPUDevice *device =
        window->device != NULL && !window->device->shutdown
            ? window->device->gpu
            : NULL;
    BTBHGPUModel *model = window->model_head;
    window->model_head = NULL;
    while (model != NULL) {
        BTBHGPUModel *next = model->next;
        btbh_gpu_model_release_handles(model, device);
        model->owner = NULL;
        model->previous = NULL;
        model->next = NULL;
        model = next;
    }
}

BTBHGPUAnimationLibrary *btbh_gpu_animation_library_load(
    const char *path,
    BTBHGPUAnimationLibraryReport *report
) {
    if (path == NULL || report == NULL) {
        return NULL;
    }
    memset(report, 0, sizeof(*report));
    cgltf_options options = {0};
    options.file.read = btbh_gpu_model_file_read;
    options.file.release = btbh_gpu_model_file_release;
    cgltf_data *data = NULL;
    cgltf_result result = cgltf_parse_file(&options, path, &data);
    if (result == cgltf_result_success) {
        result = cgltf_load_buffers(&options, data, path);
    }
    if (result == cgltf_result_success) {
        result = cgltf_validate(data);
    }
    if (
        result != cgltf_result_success ||
        data == NULL ||
        data->animations_count == 0 ||
        data->animations_count > UINT32_MAX
    ) {
        cgltf_free(data);
        report->result = BTBH_GPU_DEVICE_FAILURE;
        (void) snprintf(
            report->message,
            sizeof(report->message),
            "%s",
            "Could not load glTF animation clips"
        );
        return NULL;
    }
    BTBHGPUAnimationLibrary *library = calloc(1, sizeof(*library));
    if (library == NULL) {
        cgltf_free(data);
        report->result = BTBH_GPU_DEVICE_FAILURE;
        (void) snprintf(
            report->message,
            sizeof(report->message),
            "%s",
            "Could not allocate animation library"
        );
        return NULL;
    }
    library->data = data;
    report->result = BTBH_GPU_OK;
    report->animation_count = (uint32_t) data->animations_count;
    return library;
}

float btbh_gpu_animation_library_duration(
    const BTBHGPUAnimationLibrary *library,
    uint32_t animation_index
) {
    if (
        library == NULL ||
        library->data == NULL ||
        animation_index >= library->data->animations_count
    ) {
        return 0.0f;
    }
    const cgltf_animation *animation =
        &library->data->animations[animation_index];
    float duration = 0.0f;
    for (cgltf_size index = 0; index < animation->samplers_count; ++index) {
        const cgltf_accessor *input = animation->samplers[index].input;
        if (input == NULL || input->count == 0) {
            continue;
        }
        float final_time = 0.0f;
        if (input->has_max) {
            final_time = input->max[0];
        } else if (!cgltf_accessor_read_float(
            input,
            input->count - 1,
            &final_time,
            1
        )) {
            if (input->count > SIZE_MAX / sizeof(cgltf_float)) {
                continue;
            }
            cgltf_float *times = malloc(input->count * sizeof(*times));
            if (times == NULL) {
                continue;
            }
            const cgltf_size unpacked = cgltf_accessor_unpack_floats(
                input,
                times,
                input->count
            );
            if (unpacked == input->count) {
                final_time = times[input->count - 1];
            }
            free(times);
        }
        if (isfinite(final_time) && final_time > duration) {
            duration = final_time;
        }
    }
    return duration;
}

static bool btbh_gpu_animation_read_value(
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

static bool btbh_gpu_animation_sample_value(
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
        return btbh_gpu_animation_read_value(
            sampler,
            0u,
            1u,
            component_count,
            value
        );
    }
    if (time >= last_time) {
        return btbh_gpu_animation_read_value(
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
        !btbh_gpu_animation_read_value(
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
    if (!btbh_gpu_animation_read_value(
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
            !btbh_gpu_animation_read_value(
                sampler,
                lower,
                2u,
                component_count,
                lower_tangent
            ) ||
            !btbh_gpu_animation_read_value(
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
        btbh_gpu_animation_slerp_shortest(
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

static void btbh_gpu_animation_normalize_quaternion(float rotation[4]) {
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

static void btbh_gpu_animation_matrix_srt_quaternion(
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

void btbh_gpu_animation_slerp_shortest(
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
        btbh_gpu_animation_normalize_quaternion(output);
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

static void btbh_gpu_animation_multiply_matrix(
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

static bool btbh_gpu_animation_world_matrix(
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
            !btbh_gpu_animation_world_matrix(
                data,
                local_matrices,
                world_matrices,
                states,
                (cgltf_size)parent_index
            )
        ) {
            return false;
        }
        btbh_gpu_animation_multiply_matrix(
            &world_matrices[(cgltf_size)parent_index * 16u],
            local,
            world
        );
    }
    states[node_index] = 2u;
    return true;
}

uint32_t btbh_gpu_animation_library_skin_joint_count(
    const BTBHGPUAnimationLibrary *library,
    uint32_t skin_index
) {
    if (
        library == NULL ||
        library->data == NULL ||
        skin_index >= library->data->skins_count ||
        library->data->skins[skin_index].joints_count > UINT32_MAX
    ) {
        return 0u;
    }
    return (uint32_t)library->data->skins[skin_index].joints_count;
}

static bool btbh_gpu_animation_sample_local_transforms(
    const BTBHGPUAnimationLibrary *library,
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
        valid = btbh_gpu_animation_sample_value(
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
            btbh_gpu_animation_normalize_quaternion(target);
        }
    }
    return valid;
}

static bool btbh_gpu_animation_build_skin_matrices(
    const BTBHGPUAnimationLibrary *library,
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
            btbh_gpu_animation_matrix_srt_quaternion(
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
        valid = btbh_gpu_animation_world_matrix(
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
            btbh_gpu_animation_multiply_matrix(
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

static bool btbh_gpu_animation_library_sample_skin_blended(
    const BTBHGPUAnimationLibrary *library,
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
        valid = btbh_gpu_animation_sample_local_transforms(
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
                    btbh_gpu_animation_slerp_shortest(
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
        valid = btbh_gpu_animation_build_skin_matrices(
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

bool btbh_gpu_animation_library_sample_skin(
    const BTBHGPUAnimationLibrary *library,
    uint32_t animation_index,
    float time,
    uint32_t skin_index,
    float *matrices,
    uint32_t matrix_capacity
) {
    const float weight = 1.0f;
    return btbh_gpu_animation_library_sample_skin_blended(
        library,
        &animation_index,
        &time,
        &weight,
        1u,
        skin_index,
        matrices,
        matrix_capacity
    );
}

BTBHGPUAnimationPose *btbh_gpu_animation_pose_create(
    const BTBHGPUAnimationLibrary *library
) {
    if (library == NULL || library->data == NULL) {
        return NULL;
    }
    BTBHGPUAnimationPose *pose = calloc(1, sizeof(*pose));
    if (pose != NULL) {
        pose->library = library;
    }
    return pose;
}

uint8_t btbh_gpu_animation_pose_set_samples(
    BTBHGPUAnimationPose *pose,
    const uint32_t *animation_indices,
    const float *animation_times,
    const float *animation_weights,
    uint32_t sample_count
) {
    if (
        pose == NULL ||
        pose->library == NULL ||
        animation_indices == NULL ||
        animation_times == NULL ||
        animation_weights == NULL ||
        sample_count == 0u ||
        (size_t)sample_count > SIZE_MAX / sizeof(float)
    ) {
        return 0u;
    }
    uint32_t *indices = malloc(
        (size_t)sample_count * sizeof(*indices)
    );
    float *times = malloc((size_t)sample_count * sizeof(*times));
    float *weights = malloc((size_t)sample_count * sizeof(*weights));
    if (indices == NULL || times == NULL || weights == NULL) {
        free(weights);
        free(times);
        free(indices);
        return 0u;
    }
    float weight_sum = 0.0f;
    bool valid = true;
    for (uint32_t index = 0u; index < sample_count; index += 1u) {
        if (
            animation_indices[index] >=
                pose->library->data->animations_count ||
            !isfinite(animation_times[index]) ||
            !isfinite(animation_weights[index]) ||
            animation_weights[index] < 0.0f
        ) {
            valid = false;
            break;
        }
        indices[index] = animation_indices[index];
        times[index] = animation_times[index];
        weights[index] = animation_weights[index];
        weight_sum += animation_weights[index];
    }
    if (!valid || !isfinite(weight_sum) || weight_sum <= 0.0f) {
        free(weights);
        free(times);
        free(indices);
        return 0u;
    }
    free(pose->animation_weights);
    free(pose->animation_times);
    free(pose->animation_indices);
    pose->animation_indices = indices;
    pose->animation_times = times;
    pose->animation_weights = weights;
    pose->sample_count = sample_count;
    return 1u;
}

bool btbh_gpu_animation_pose_sample_skin(
    const BTBHGPUAnimationPose *pose,
    uint32_t skin_index,
    float *matrices,
    uint32_t matrix_capacity
) {
    return pose != NULL &&
        btbh_gpu_animation_library_sample_skin_blended(
            pose->library,
            pose->animation_indices,
            pose->animation_times,
            pose->animation_weights,
            pose->sample_count,
            skin_index,
            matrices,
            matrix_capacity
        );
}

uint32_t btbh_gpu_animation_pose_skin_joint_count(
    const BTBHGPUAnimationPose *pose,
    uint32_t skin_index
) {
    return pose != NULL
        ? btbh_gpu_animation_library_skin_joint_count(
            pose->library,
            skin_index
        )
        : 0u;
}

void btbh_gpu_animation_pose_destroy(
    BTBHGPUAnimationPose *pose
) {
    if (pose == NULL) {
        return;
    }
    free(pose->animation_weights);
    free(pose->animation_times);
    free(pose->animation_indices);
    free(pose);
}

void btbh_gpu_animation_library_destroy(
    BTBHGPUAnimationLibrary *library
) {
    if (library == NULL) {
        return;
    }
    cgltf_free(library->data);
    free(library);
}
