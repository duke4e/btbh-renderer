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
    // Whole 255ths that add up to exactly 255: each slot gets its share rounded down, and the 255ths
    // left over go one each to the slots with the largest remainders (the lower slot on a tie).
    // Rounding each slot on its own let multi-bone vertices add up to 254..257, and a sum off 1 moves
    // the vertex in proportion to its distance from the world origin.
    uint32_t quantized[4];
    uint32_t remainders[4];
    uint32_t total = 0u;
    for (uint32_t slot = 0u; slot < 4u; slot += 1u) {
        const uint32_t scaled = (uint32_t) weights[slot] * 255u;
        quantized[slot] = scaled / sum;
        remainders[slot] = scaled % sum;
        total += quantized[slot];
    }
    // Each slot loses less than one 255th, so fewer than four are left; the remainders add up to
    // exactly that many times `sum`, which gives every one of them a slot with a remainder.
    for (uint32_t left = 255u - total; left > 0u; left -= 1u) {
        uint32_t largest = 0u;
        for (uint32_t slot = 1u; slot < 4u; slot += 1u) {
            if (remainders[slot] > remainders[largest]) {
                largest = slot;
            }
        }
        quantized[largest] += 1u;
        remainders[largest] = 0u;
    }
    for (uint32_t slot = 0u; slot < 4u; slot += 1u) {
        weights[slot] = (float) quantized[slot] / 255.0f;
    }
}

/* Since 0.16.0 a library decodes, at load, what sampling a skin reads (btbh_gpu_animation_library_decode): each
   channel's key times and values as flat floats, read once through cgltf_accessor_read_float (normalised integers
   convert as they always did), each skin's joints and inverse bind matrices, and the nodes' rest transforms. Sampling
   reads those floats with the comparisons and the arithmetic it ran on cgltf's reader, in the same order, so a pose's
   palette is the same to the bit. */

/* One translation, rotation or scale channel with a target node, as its sampler decoded. */
typedef struct BTBHGPUAnimationTrack {
    /* The output's elements, component_count floats each (a cubic spline's: in-tangent, value, out-tangent a key),
       zero past the `written` floats its reader filled. */
    const float *values;
    size_t node;
    /* Its key times: the clip's time slot (BTBHGPUAnimationClip.time_sets). */
    uint32_t time_slot;
    /* 3, or 4 for a rotation. */
    uint8_t component_count;
    /* The floats a read of one element wrote: component_count, or fewer for an output narrower than its path (the
       rest of the target keeps what it held). */
    uint8_t written;
    /* BTBH_GPU_ANIMATION_LINEAR, _STEP or _CUBIC_SPLINE. */
    uint8_t interpolation;
    /* BTBH_GPU_ANIMATION_PATH_*. */
    uint8_t path;
} BTBHGPUAnimationTrack;

/* One set of key times, shared by every channel whose times are the same floats. */
typedef struct BTBHGPUAnimationKeyTimes {
    float *times;
    size_t count;
    uint64_t hash;
} BTBHGPUAnimationKeyTimes;

/* One output accessor decoded at a component count, shared by the channels that read it so. */
typedef struct BTBHGPUAnimationValues {
    const cgltf_accessor *accessor;
    float *values;
    uint32_t component_count;
    bool readable;
} BTBHGPUAnimationValues;

typedef struct BTBHGPUAnimationClip {
    /* Its sampled channels, in the file's order (a later channel on the same target wins, as it did). */
    BTBHGPUAnimationTrack *tracks;
    uint32_t track_count;
    /* The key times its tracks read: library key_times indices, one a slot. */
    uint32_t *time_sets;
    uint32_t time_slot_count;
    /* False when sampling it fails whatever the time (an unreadable accessor, a channel without keys), as reading it
       through cgltf did: every such failure is the accessor's, never a key's. */
    bool valid;
} BTBHGPUAnimationClip;

typedef struct BTBHGPUAnimationSkin {
    size_t *joint_nodes;
    /* 16 floats a joint, NULL when the skin has none (each joint's is the identity). */
    float *inverse_binds;
    /* False when a joint isn't a node, or its inverse bind matrices can't be read or are fewer than its joints:
       sampling it fails. */
    bool valid;
} BTBHGPUAnimationSkin;

struct BTBHGPUAnimationLibrary {
    cgltf_data *data;
    /* Unique among libraries and blended poses for the process's life (btbh_gpu_animation_next_serial): the key a
       palette is kept under across presents, which a later library at the same address can't take over. */
    uint64_t serial;
    BTBHGPUAnimationClip *clips;
    BTBHGPUAnimationKeyTimes *key_times;
    uint32_t key_time_count;
    BTBHGPUAnimationValues *values;
    uint32_t value_count;
    BTBHGPUAnimationSkin *skins;
    /* The nodes' rest transforms (each node's, or the identity's parts): 3, 4 and 3 floats a node. */
    float *rest_translations;
    float *rest_rotations;
    float *rest_scales;
    /* The most time slots of any clip: the segments a sample resolves. */
    uint32_t max_time_slots;
    /* What the decoded arrays hold, in bytes (btbh_gpu_animation_library_decoded_bytes). */
    size_t decoded_bytes;
};

struct BTBHGPUAnimationPose {
    const BTBHGPUAnimationLibrary *library;
    uint32_t *animation_indices;
    float *animation_times;
    float *animation_weights;
    uint32_t sample_count;
    /* A blend's (more than one sample) key for the palettes kept across presents: a new serial whenever its samples
       change. A pose of one sample is kept under its library's, clip's and time's key instead. */
    uint64_t serial;
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

/* In place and lexical (the file system is not asked): '\' becomes '/', empty and "." segments
   go, and "name/.." pairs cancel. The root ("/", "//", "X:/") stays and nothing climbs above it.
   Two spellings of one file can still differ (letter case, links): they then load twice, as
   without sharing. */
static void btbh_gpu_model_normalize_texture_path(char *path) {
    for (char *cursor = path; *cursor != '\0'; cursor += 1) {
        if (*cursor == '\\') {
            *cursor = '/';
        }
    }
    size_t root = 0u;
    if (path[0] == '/') {
        root = path[1] == '/' ? 2u : 1u;
    } else if (
        ((path[0] >= 'A' && path[0] <= 'Z') ||
            (path[0] >= 'a' && path[0] <= 'z')) &&
        path[1] == ':' &&
        path[2] == '/'
    ) {
        root = 3u;
    }
    size_t read = root;
    size_t write = root;
    while (path[read] != '\0') {
        size_t end = read;
        while (path[end] != '\0' && path[end] != '/') {
            end += 1u;
        }
        const size_t length = end - read;
        const bool current = length == 1u && path[read] == '.';
        const bool parent =
            length == 2u && path[read] == '.' && path[read + 1u] == '.';
        bool append = length > 0u && !current;
        if (parent && write > root) {
            size_t start = write;
            while (start > root && path[start - 1u] != '/') {
                start -= 1u;
            }
            const bool previous_is_parent =
                write - start == 2u &&
                path[start] == '.' &&
                path[start + 1u] == '.';
            if (!previous_is_parent) {
                write = start > root ? start - 1u : root;
                append = false;
            }
        } else if (parent && root > 0u) {
            append = false;
        }
        if (append) {
            if (write > root) {
                path[write] = '/';
                write += 1u;
            }
            (void)memmove(path + write, path + read, length);
            write += length;
        }
        read = path[end] == '/' ? end + 1u : end;
    }
    path[write] = '\0';
}

/* The share key of a resolved external texture path: a normalised copy; NULL when the copy cannot
   be allocated (the texture is then loaded unshared). */
static char *btbh_gpu_model_texture_share_key(const char *texture_path) {
    const size_t length = strlen(texture_path);
    char *key = malloc(length + 1u);
    if (key == NULL) {
        return NULL;
    }
    (void)memcpy(key, texture_path, length + 1u);
    btbh_gpu_model_normalize_texture_path(key);
    return key;
}

static bool btbh_gpu_model_texture_share_path_equal(
    const char *left,
    const char *right
) {
    if (left == NULL || right == NULL) {
        return left == right;
    }
    return strcmp(left, right) == 0;
}

static BTBHGPUModelTextureShare *btbh_gpu_model_texture_share_find(
    const BTBHGPUWindow *window,
    const char *path,
    const char *second_path,
    SDL_GPUTextureFormat format,
    bool composed_orm
) {
    for (
        BTBHGPUModelTextureShare *share = window->model_texture_share_head;
        share != NULL;
        share = share->next
    ) {
        if (
            share->format == format &&
            share->composed_orm == composed_orm &&
            btbh_gpu_model_texture_share_path_equal(share->path, path) &&
            btbh_gpu_model_texture_share_path_equal(
                share->second_path,
                second_path
            )
        ) {
            return share;
        }
    }
    return NULL;
}

/* Hands `texture` (just loaded for one material slot) to a new share. Takes the paths either way;
   when the share cannot be allocated the texture stays the model's own, as without sharing. */
static void btbh_gpu_model_texture_share_add(
    BTBHGPUWindow *window,
    BTBHGPUTexture *texture,
    char *path,
    char *second_path,
    SDL_GPUTextureFormat format,
    bool composed_orm
) {
    BTBHGPUModelTextureShare *share = calloc(1, sizeof(*share));
    if (share == NULL) {
        free(path);
        free(second_path);
        return;
    }
    share->owner = window;
    share->texture = texture;
    share->path = path;
    share->second_path = second_path;
    share->format = format;
    share->composed_orm = composed_orm;
    share->reference_count = 1u;
    share->next = window->model_texture_share_head;
    if (window->model_texture_share_head != NULL) {
        window->model_texture_share_head->previous = share;
    }
    window->model_texture_share_head = share;
    texture->share = share;
}

static void btbh_gpu_model_texture_share_free(
    BTBHGPUModelTextureShare *share
) {
    BTBHGPUWindow *owner = share->owner;
    if (owner != NULL) {
        if (share->previous != NULL) {
            share->previous->next = share->next;
        } else if (owner->model_texture_share_head == share) {
            owner->model_texture_share_head = share->next;
        }
        if (share->next != NULL) {
            share->next->previous = share->previous;
        }
    }
    if (share->texture != NULL) {
        share->texture->share = NULL;
        btbh_gpu_texture_destroy(share->texture);
    }
    free(share->path);
    free(share->second_path);
    free(share);
}

/* What a model's destroy does with each of its material textures: a shared one loses this slot's
   count (and is freed with the last), any other is destroyed as before. */
static void btbh_gpu_model_texture_release(BTBHGPUTexture *texture) {
    BTBHGPUModelTextureShare *share = texture->share;
    if (share == NULL) {
        btbh_gpu_texture_destroy(texture);
        return;
    }
    if (share->reference_count > 1u) {
        share->reference_count -= 1u;
        return;
    }
    btbh_gpu_model_texture_share_free(share);
}

/* The share key of one ORM source: true with *key NULL for no image, true with a key for an
   external image; false for anything else (an embedded or data-URI image, a texture without an
   image, no memory), which loads the ORM texture unshared. */
static bool btbh_gpu_model_orm_share_key(
    const cgltf_texture_view *view,
    const char *model_path,
    char **key
) {
    *key = NULL;
    if (view == NULL || view->texture == NULL) {
        return true;
    }
    const cgltf_image *image = view->texture->image;
    if (image == NULL || image->buffer_view != NULL) {
        return false;
    }
    char *texture_path = btbh_gpu_model_resolve_texture_path(
        model_path,
        image->uri
    );
    if (texture_path == NULL) {
        return false;
    }
    btbh_gpu_model_normalize_texture_path(texture_path);
    *key = texture_path;
    return true;
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
    bool share,
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
    /* Shared by the pair of external images it is composed from: the composition depends on
       nothing else. An embedded image keeps the texture the model's own. */
    char *occlusion_key = NULL;
    char *metallic_roughness_key = NULL;
    const bool shared =
        share &&
        btbh_gpu_model_orm_share_key(
            occlusion_view,
            model_path,
            &occlusion_key
        ) &&
        btbh_gpu_model_orm_share_key(
            metallic_roughness_view,
            model_path,
            &metallic_roughness_key
        );
    if (shared) {
        BTBHGPUModelTextureShare *existing =
            btbh_gpu_model_texture_share_find(
                window,
                occlusion_key,
                metallic_roughness_key,
                SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                true
            );
        if (existing != NULL) {
            existing->reference_count += 1u;
            *texture = existing->texture;
            free(metallic_roughness_key);
            free(occlusion_key);
            return true;
        }
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
    if (success && shared) {
        btbh_gpu_model_texture_share_add(
            window,
            *texture,
            occlusion_key,
            metallic_roughness_key,
            SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
            true
        );
    } else {
        free(metallic_roughness_key);
        free(occlusion_key);
    }
    return success;
}

static bool btbh_gpu_model_load_texture(
    BTBHGPUWindow *window,
    const cgltf_texture_view *view,
    const char *model_path,
    SDL_GPUTextureFormat format,
    bool share,
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
        /* Shared by its normalised path and format: an sRGB albedo or emission texture never
           shares with a linear normal one of the same image. */
        char *share_key = share
            ? btbh_gpu_model_texture_share_key(texture_path)
            : NULL;
        if (share_key != NULL) {
            BTBHGPUModelTextureShare *existing =
                btbh_gpu_model_texture_share_find(
                    window,
                    share_key,
                    NULL,
                    format,
                    false
                );
            if (existing != NULL) {
                existing->reference_count += 1u;
                *texture = existing->texture;
                free(share_key);
                free(texture_path);
                return true;
            }
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
            if (share_key != NULL) {
                btbh_gpu_model_texture_share_add(
                    window,
                    *texture,
                    share_key,
                    NULL,
                    format,
                    false
                );
            }
            return true;
        }
        free(share_key);
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
    bool share_external_textures,
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
                    share_external_textures,
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
                share_external_textures,
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
                    share_external_textures,
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
                share_external_textures,
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

bool btbh_gpu_geometry_copy(
    SDL_GPUDevice *device,
    SDL_GPUBuffer *buffer,
    uint32_t offset,
    const void *bytes,
    uint32_t size
) {
    uint32_t staging_offset = 0u;
    SDL_GPUTransferBuffer *transfer = btbh_gpu_staging_acquire(device, size, &staging_offset);
    if (transfer == NULL) {
        return false;
    }
    void *mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
    if (mapped == NULL) {
        btbh_gpu_staging_abandon(device, transfer);
        return false;
    }
    memcpy((uint8_t *)mapped + staging_offset, bytes, size);
    SDL_UnmapGPUTransferBuffer(device, transfer);
    SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass *copy =
        command != NULL ? SDL_BeginGPUCopyPass(command) : NULL;
    if (copy == NULL) {
        if (command != NULL) {
            (void) SDL_CancelGPUCommandBuffer(command);
        }
        btbh_gpu_staging_abandon(device, transfer);
        return false;
    }
    SDL_GPUTransferBufferLocation source = {0};
    source.transfer_buffer = transfer;
    source.offset = staging_offset;
    SDL_GPUBufferRegion destination = {0};
    destination.buffer = buffer;
    destination.offset = offset;
    destination.size = size;
    SDL_UploadToGPUBuffer(copy, &source, &destination, false);
    SDL_EndGPUCopyPass(copy);
    return btbh_gpu_staging_submit(device, command, transfer);
}

/* Copies `size` bytes into the arena's open block (a new one when they do not fit), at an offset that is a multiple
   of `alignment`, and returns the block, or NULL. The models' primitives and the shared custom meshes
   (btbh_gpu_custom_mesh.c) take their ranges here. */
BTBHGPUGeometryBlock *btbh_gpu_geometry_arena_upload(
    SDL_GPUDevice *device,
    BTBHGPUGeometryArena *arena,
    SDL_GPUBufferUsageFlags usage,
    uint32_t block_bytes,
    uint32_t alignment,
    const void *bytes,
    uint32_t size,
    uint32_t *offset
) {
    if (device == NULL || arena == NULL || bytes == NULL || size == 0u || alignment == 0u) {
        return NULL;
    }
    BTBHGPUGeometryBlock *block = NULL;
    uint32_t start = 0u;
    if (arena->block_count > 0u) {
        BTBHGPUGeometryBlock *open = arena->blocks[arena->block_count - 1u];
        const uint64_t aligned =
            ((uint64_t) open->used + alignment - 1u) / alignment * alignment;
        if (open->buffer != NULL && aligned + size <= open->capacity) {
            block = open;
            start = (uint32_t) aligned;
        }
    }
    bool created = false;
    if (block == NULL) {
        if (arena->block_count == arena->block_capacity) {
            const uint32_t capacity =
                arena->block_capacity == 0u ? 8u : arena->block_capacity * 2u;
            BTBHGPUGeometryBlock **blocks = realloc(
                arena->blocks,
                capacity * sizeof(*blocks)
            );
            if (blocks == NULL) {
                return NULL;
            }
            arena->blocks = blocks;
            arena->block_capacity = capacity;
        }
        block = calloc(1, sizeof(*block));
        if (block == NULL) {
            return NULL;
        }
        SDL_GPUBufferCreateInfo buffer_info = {0};
        buffer_info.usage = usage;
        buffer_info.size = size > block_bytes ? size : block_bytes;
        block->buffer = SDL_CreateGPUBuffer(device, &buffer_info);
        if (block->buffer == NULL) {
            free(block);
            return NULL;
        }
        block->capacity = buffer_info.size;
        arena->blocks[arena->block_count] = block;
        arena->block_count += 1u;
        created = true;
    }
    if (!btbh_gpu_geometry_copy(device, block->buffer, start, bytes, size)) {
        if (created && block->live == 0u) {
            SDL_ReleaseGPUBuffer(device, block->buffer);
            block->buffer = NULL;
        }
        return NULL;
    }
    block->used = start + size;
    block->live += 1u;
    *offset = start;
    return block;
}

void btbh_gpu_geometry_block_release(
    BTBHGPUGeometryBlock *block,
    SDL_GPUDevice *device
) {
    if (block == NULL || block->live == 0u) {
        return;
    }
    block->live -= 1u;
    if (block->live == 0u && block->buffer != NULL) {
        if (device != NULL) {
            SDL_ReleaseGPUBuffer(device, block->buffer);
        }
        block->buffer = NULL;
    }
}

static bool btbh_gpu_model_upload_vertices(
    BTBHGPUModel *model,
    BTBHGPUModelPrimitive *destination,
    const void *vertices,
    uint32_t vertex_size
) {
    uint32_t offset = 0u;
    destination->vertex_block = btbh_gpu_geometry_arena_upload(
        model->owner->device->gpu,
        &model->owner->vertex_arena,
        SDL_GPU_BUFFERUSAGE_VERTEX,
        BTBH_GPU_GEOMETRY_VERTEX_BLOCK_BYTES,
        (uint32_t) sizeof(BTBHGPUModelVertex),
        vertices,
        vertex_size,
        &offset
    );
    if (destination->vertex_block == NULL) {
        return false;
    }
    destination->vertex_buffer = destination->vertex_block->buffer;
    destination->vertex_base =
        (int32_t) (offset / (uint32_t) sizeof(BTBHGPUModelVertex));
    return true;
}

static bool btbh_gpu_model_upload_indices(
    BTBHGPUModel *model,
    BTBHGPUModelPrimitive *destination,
    const uint32_t *indices,
    uint32_t index_size
) {
    uint32_t offset = 0u;
    destination->index_block = btbh_gpu_geometry_arena_upload(
        model->owner->device->gpu,
        &model->owner->index_arena,
        SDL_GPU_BUFFERUSAGE_INDEX,
        BTBH_GPU_GEOMETRY_INDEX_BLOCK_BYTES,
        (uint32_t) sizeof(uint32_t),
        indices,
        index_size,
        &offset
    );
    if (destination->index_block == NULL) {
        return false;
    }
    destination->index_buffer = destination->index_block->buffer;
    destination->first_index = offset / (uint32_t) sizeof(uint32_t);
    return true;
}

static void btbh_gpu_geometry_arena_free(
    BTBHGPUGeometryArena *arena,
    SDL_GPUDevice *device
) {
    for (uint32_t index = 0u; index < arena->block_count; index += 1u) {
        BTBHGPUGeometryBlock *block = arena->blocks[index];
        if (block->buffer != NULL && device != NULL) {
            SDL_ReleaseGPUBuffer(device, block->buffer);
        }
        free(block);
    }
    free(arena->blocks);
    arena->blocks = NULL;
    arena->block_count = 0u;
    arena->block_capacity = 0u;
}

void btbh_gpu_window_release_geometry_arenas(BTBHGPUWindow *window) {
    if (window == NULL) {
        return;
    }
    SDL_GPUDevice *device =
        window->device != NULL && !window->device->shutdown
            ? window->device->gpu
            : NULL;
    btbh_gpu_geometry_arena_free(&window->vertex_arena, device);
    btbh_gpu_geometry_arena_free(&window->index_arena, device);
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
                btbh_gpu_model_texture_release(material->albedo_texture);
                material->albedo_texture = NULL;
            }
            if (material->emission_texture != NULL) {
                btbh_gpu_model_texture_release(material->emission_texture);
                material->emission_texture = NULL;
            }
            if (material->normal_texture != NULL) {
                btbh_gpu_model_texture_release(material->normal_texture);
                material->normal_texture = NULL;
            }
            if (material->orm_texture != NULL) {
                btbh_gpu_model_texture_release(material->orm_texture);
                material->orm_texture = NULL;
            }
        }
    }
    if (model->primitives == NULL) {
        return;
    }
    for (uint32_t index = 0; index < model->primitive_count; index += 1u) {
        BTBHGPUModelPrimitive *primitive = &model->primitives[index];
        btbh_gpu_geometry_block_release(primitive->index_block, device);
        btbh_gpu_geometry_block_release(primitive->vertex_block, device);
        primitive->index_block = NULL;
        primitive->vertex_block = NULL;
        primitive->index_buffer = NULL;
        primitive->vertex_buffer = NULL;
        free(primitive->positions);
        free(primitive->indices);
        primitive->positions = NULL;
        primitive->indices = NULL;
    }
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

    if (!btbh_gpu_model_upload_vertices(
            model,
            destination,
            vertices,
            vertex_size
        )) {
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
        valid = btbh_gpu_model_upload_indices(
            model,
            destination,
            indices,
            index_size
        );
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

    if (!btbh_gpu_model_upload_vertices(
            model,
            destination,
            vertices,
            vertex_size
        )) {
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
        valid = btbh_gpu_model_upload_indices(
            model,
            destination,
            indices,
            index_size
        );
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
            (import_flags
                & BTBH_GPU_MODEL_IMPORT_SHARE_EXTERNAL_TEXTURES) != 0u,
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
    /* The models' releases freed every shared texture; whatever a share still holds goes now,
       before the window's textures. */
    while (window->model_texture_share_head != NULL) {
        btbh_gpu_model_texture_share_free(window->model_texture_share_head);
    }
}

/* ------------------------------------------------------------ Decoded animation data (since 0.16.0) */

enum {
    BTBH_GPU_ANIMATION_PATH_TRANSLATION = 0,
    BTBH_GPU_ANIMATION_PATH_ROTATION = 1,
    BTBH_GPU_ANIMATION_PATH_SCALE = 2
};

/* A sampler's interpolation as sampling reads it: anything but step and cubic spline is linear. */
enum {
    BTBH_GPU_ANIMATION_LINEAR = 0,
    BTBH_GPU_ANIMATION_STEP = 1,
    BTBH_GPU_ANIMATION_CUBIC_SPLINE = 2
};

/* The accessor maps' "not decoded yet" and "can't be read". */
#define BTBH_GPU_ANIMATION_NOT_DECODED UINT32_MAX
#define BTBH_GPU_ANIMATION_UNREADABLE (UINT32_MAX - 1u)

static SDL_SpinLock btbh_gpu_animation_serial_lock;
static uint64_t btbh_gpu_animation_serial_last;

/* A serial no library or blended pose took before in this process (never 0). */
static uint64_t btbh_gpu_animation_next_serial(void) {
    SDL_LockSpinlock(&btbh_gpu_animation_serial_lock);
    const uint64_t serial = ++btbh_gpu_animation_serial_last;
    SDL_UnlockSpinlock(&btbh_gpu_animation_serial_lock);
    return serial;
}

static void btbh_gpu_animation_library_free_decoded(
    BTBHGPUAnimationLibrary *library
) {
    if (library->clips != NULL) {
        for (cgltf_size index = 0u;
             index < library->data->animations_count;
             index += 1u) {
            free(library->clips[index].tracks);
            free(library->clips[index].time_sets);
        }
    }
    free(library->clips);
    library->clips = NULL;
    for (uint32_t index = 0u; index < library->key_time_count; index += 1u) {
        free(library->key_times[index].times);
    }
    free(library->key_times);
    library->key_times = NULL;
    library->key_time_count = 0u;
    for (uint32_t index = 0u; index < library->value_count; index += 1u) {
        free(library->values[index].values);
    }
    free(library->values);
    library->values = NULL;
    library->value_count = 0u;
    if (library->skins != NULL) {
        for (cgltf_size index = 0u;
             index < library->data->skins_count;
             index += 1u) {
            free(library->skins[index].joint_nodes);
            free(library->skins[index].inverse_binds);
        }
    }
    free(library->skins);
    library->skins = NULL;
    free(library->rest_translations);
    free(library->rest_rotations);
    free(library->rest_scales);
    library->rest_translations = NULL;
    library->rest_rotations = NULL;
    library->rest_scales = NULL;
}

/* Grows `*items` (of `item_size` bytes each) to hold one more than `count`. */
static bool btbh_gpu_animation_reserve(
    void **items,
    uint32_t count,
    uint32_t *capacity,
    size_t item_size
) {
    if (count < *capacity) {
        return true;
    }
    if (*capacity > UINT32_MAX / 2u) {
        return false;
    }
    const uint32_t grown = *capacity == 0u ? 16u : *capacity * 2u;
    if ((size_t)grown > SIZE_MAX / item_size) {
        return false;
    }
    void *items_grown = realloc(*items, (size_t)grown * item_size);
    if (items_grown == NULL) {
        return false;
    }
    *items = items_grown;
    *capacity = grown;
    return true;
}

/* `input`'s key times (it has keys): *index is their library key_times entry, BTBH_GPU_ANIMATION_UNREADABLE when they
   can't be read (cgltf_accessor_read_float fails for the accessor, never for one element: then for every key). A
   channel whose times are the same floats as another's shares its entry, so a sample finds where a time falls once
   for both. False when memory runs out. */
static bool btbh_gpu_animation_decode_key_times(
    BTBHGPUAnimationLibrary *library,
    const cgltf_accessor *input,
    uint32_t *capacity,
    uint32_t *index
) {
    const size_t count = input->count;
    if (count > SIZE_MAX / sizeof(float)) {
        return false;
    }
    float *times = malloc(count * sizeof(float));
    if (times == NULL) {
        return false;
    }
    uint64_t hash = 14695981039346656037ull;
    for (size_t key = 0u; key < count; key += 1u) {
        if (!cgltf_accessor_read_float(input, key, &times[key], 1u)) {
            free(times);
            *index = BTBH_GPU_ANIMATION_UNREADABLE;
            return true;
        }
        uint32_t bits = 0u;
        (void)memcpy(&bits, &times[key], sizeof(bits));
        hash = (hash ^ bits) * 1099511628211ull;
    }
    for (uint32_t existing = 0u;
         existing < library->key_time_count;
         existing += 1u) {
        const BTBHGPUAnimationKeyTimes *other = &library->key_times[existing];
        if (other->hash == hash &&
            other->count == count &&
            memcmp(other->times, times, count * sizeof(float)) == 0) {
            free(times);
            *index = existing;
            return true;
        }
    }
    if (library->key_time_count >= BTBH_GPU_ANIMATION_UNREADABLE ||
        !btbh_gpu_animation_reserve(
            (void **)&library->key_times,
            library->key_time_count,
            capacity,
            sizeof(*library->key_times)
        )) {
        free(times);
        return false;
    }
    BTBHGPUAnimationKeyTimes *entry =
        &library->key_times[library->key_time_count];
    entry->times = times;
    entry->count = count;
    entry->hash = hash;
    library->decoded_bytes += count * sizeof(float);
    *index = library->key_time_count;
    library->key_time_count += 1u;
    return true;
}

/* `output`'s elements read `component_count` floats at a time, as sampling read them, into a library values entry
   (*index); the floats a read doesn't write stay zero. False when memory runs out. */
static bool btbh_gpu_animation_decode_values(
    BTBHGPUAnimationLibrary *library,
    const cgltf_accessor *output,
    uint32_t component_count,
    uint32_t *capacity,
    uint32_t *index
) {
    const size_t count = output->count;
    if (count == 0u ||
        count > SIZE_MAX / (sizeof(float) * component_count) ||
        library->value_count >= BTBH_GPU_ANIMATION_UNREADABLE ||
        !btbh_gpu_animation_reserve(
            (void **)&library->values,
            library->value_count,
            capacity,
            sizeof(*library->values)
        )) {
        return false;
    }
    float *values = calloc(count * component_count, sizeof(float));
    if (values == NULL) {
        return false;
    }
    bool readable = true;
    for (size_t element = 0u; element < count; element += 1u) {
        if (!cgltf_accessor_read_float(
                output,
                element,
                &values[element * component_count],
                component_count
            )) {
            readable = false;
            break;
        }
    }
    if (!readable) {
        free(values);
        values = NULL;
    } else {
        library->decoded_bytes += count * component_count * sizeof(float);
    }
    BTBHGPUAnimationValues *entry = &library->values[library->value_count];
    entry->accessor = output;
    entry->values = values;
    entry->component_count = component_count;
    entry->readable = readable;
    *index = library->value_count;
    library->value_count += 1u;
    return true;
}

/* One clip's tracks (`clip` zeroed). A channel that sampling skips (no target node, a path other than translation,
   rotation and scale) has none. `times_by_accessor` and `values_by_accessor` (two a accessor: three and four
   components) map the accessors decoded so far. False when memory runs out. */
static bool btbh_gpu_animation_decode_clip(
    BTBHGPUAnimationLibrary *library,
    const cgltf_animation *animation,
    BTBHGPUAnimationClip *clip,
    uint32_t *times_by_accessor,
    uint32_t *values_by_accessor,
    uint32_t *key_time_capacity,
    uint32_t *value_capacity
) {
    const cgltf_data *data = library->data;
    clip->valid = true;
    if (animation->channels_count == 0u) {
        return true;
    }
    if (animation->channels_count > UINT32_MAX ||
        animation->channels_count > SIZE_MAX / sizeof(*clip->tracks)) {
        return false;
    }
    clip->tracks = malloc(animation->channels_count * sizeof(*clip->tracks));
    clip->time_sets = malloc(animation->channels_count * sizeof(*clip->time_sets));
    if (clip->tracks == NULL || clip->time_sets == NULL) {
        return false;
    }
    library->decoded_bytes += animation->channels_count *
        (sizeof(*clip->tracks) + sizeof(*clip->time_sets));
    for (cgltf_size channel_index = 0u;
         channel_index < animation->channels_count;
         channel_index += 1u) {
        const cgltf_animation_channel *channel =
            &animation->channels[channel_index];
        if (channel->target_node == NULL) {
            continue;
        }
        const ptrdiff_t node_index = channel->target_node - data->nodes;
        if (node_index < 0 || (cgltf_size)node_index >= data->nodes_count) {
            clip->valid = false;
            return true;
        }
        uint8_t path = 0u;
        uint32_t component_count = 0u;
        switch (channel->target_path) {
            case cgltf_animation_path_type_translation:
                path = BTBH_GPU_ANIMATION_PATH_TRANSLATION;
                component_count = 3u;
                break;
            case cgltf_animation_path_type_rotation:
                path = BTBH_GPU_ANIMATION_PATH_ROTATION;
                component_count = 4u;
                break;
            case cgltf_animation_path_type_scale:
                path = BTBH_GPU_ANIMATION_PATH_SCALE;
                component_count = 3u;
                break;
            case cgltf_animation_path_type_weights:
            case cgltf_animation_path_type_invalid:
            case cgltf_animation_path_type_max_enum:
            default:
                continue;
        }
        const cgltf_animation_sampler *sampler = channel->sampler;
        if (sampler == NULL ||
            sampler->input == NULL ||
            sampler->output == NULL ||
            sampler->input->count == 0u) {
            clip->valid = false;
            return true;
        }
        const uint8_t interpolation =
            sampler->interpolation == cgltf_interpolation_type_step
                ? BTBH_GPU_ANIMATION_STEP
                : sampler->interpolation ==
                        cgltf_interpolation_type_cubic_spline
                    ? BTBH_GPU_ANIMATION_CUBIC_SPLINE
                    : BTBH_GPU_ANIMATION_LINEAR;
        /* Every read stays inside the output: one element a key, three for a cubic spline (cgltf_validate checks it,
           so this only guards the arrays). */
        const size_t elements_per_key =
            interpolation == BTBH_GPU_ANIMATION_CUBIC_SPLINE ? 3u : 1u;
        if (sampler->input->count > SIZE_MAX / elements_per_key ||
            sampler->output->count <
                sampler->input->count * elements_per_key) {
            clip->valid = false;
            return true;
        }

        const size_t input_index = (size_t)(sampler->input - data->accessors);
        uint32_t times = times_by_accessor[input_index];
        if (times == BTBH_GPU_ANIMATION_NOT_DECODED) {
            if (!btbh_gpu_animation_decode_key_times(
                    library,
                    sampler->input,
                    key_time_capacity,
                    &times
                )) {
                return false;
            }
            times_by_accessor[input_index] = times;
        }
        const size_t output_index = (size_t)(sampler->output - data->accessors);
        uint32_t *values_slot =
            &values_by_accessor[output_index * 2u + (component_count == 4u ? 1u : 0u)];
        if (*values_slot == BTBH_GPU_ANIMATION_NOT_DECODED &&
            !btbh_gpu_animation_decode_values(
                library,
                sampler->output,
                component_count,
                value_capacity,
                values_slot
            )) {
            return false;
        }
        if (times == BTBH_GPU_ANIMATION_UNREADABLE ||
            !library->values[*values_slot].readable) {
            clip->valid = false;
            return true;
        }

        uint32_t time_slot = 0u;
        while (time_slot < clip->time_slot_count &&
               clip->time_sets[time_slot] != times) {
            time_slot += 1u;
        }
        if (time_slot == clip->time_slot_count) {
            clip->time_sets[time_slot] = times;
            clip->time_slot_count += 1u;
        }
        BTBHGPUAnimationTrack *track = &clip->tracks[clip->track_count];
        track->values = library->values[*values_slot].values;
        track->node = (size_t)node_index;
        track->time_slot = time_slot;
        track->component_count = (uint8_t)component_count;
        /* An accessor without a buffer view reads as zeros, all component_count of them; otherwise a read writes the
           accessor's own components (fewer than the path's for a narrower output; never more, or it fails). */
        track->written = sampler->output->buffer_view == NULL
            ? (uint8_t)component_count
            : (uint8_t)cgltf_num_components(sampler->output->type);
        track->interpolation = interpolation;
        track->path = path;
        clip->track_count += 1u;
    }
    return true;
}

/* Decodes what sampling reads (`library->data` loaded and validated). Only running out of memory fails: an accessor
   that can't be read marks its clip or skin, whose samples fail as they did. */
static bool btbh_gpu_animation_library_decode(BTBHGPUAnimationLibrary *library) {
    const cgltf_data *data = library->data;
    const size_t node_count = data->nodes_count;
    if (node_count > SIZE_MAX / (sizeof(float) * 4u) ||
        data->accessors_count > SIZE_MAX / (sizeof(uint32_t) * 3u) ||
        data->animations_count > SIZE_MAX / sizeof(*library->clips) ||
        data->skins_count > SIZE_MAX / sizeof(*library->skins)) {
        return false;
    }
    if (node_count > 0u) {
        library->rest_translations = malloc(node_count * 3u * sizeof(float));
        library->rest_rotations = malloc(node_count * 4u * sizeof(float));
        library->rest_scales = malloc(node_count * 3u * sizeof(float));
        if (library->rest_translations == NULL ||
            library->rest_rotations == NULL ||
            library->rest_scales == NULL) {
            return false;
        }
        library->decoded_bytes += node_count * 10u * sizeof(float);
    }
    for (size_t index = 0u; index < node_count; index += 1u) {
        const cgltf_node *node = &data->nodes[index];
        float *translation = &library->rest_translations[index * 3u];
        float *rotation = &library->rest_rotations[index * 4u];
        float *scale = &library->rest_scales[index * 3u];
        (void)memset(translation, 0, sizeof(float) * 3u);
        (void)memset(rotation, 0, sizeof(float) * 4u);
        (void)memset(scale, 0, sizeof(float) * 3u);
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

    if (data->skins_count > 0u) {
        library->skins = calloc(data->skins_count, sizeof(*library->skins));
        if (library->skins == NULL) {
            return false;
        }
        library->decoded_bytes += data->skins_count * sizeof(*library->skins);
    }
    for (cgltf_size skin_index = 0u;
         skin_index < data->skins_count;
         skin_index += 1u) {
        const cgltf_skin *skin = &data->skins[skin_index];
        BTBHGPUAnimationSkin *decoded = &library->skins[skin_index];
        decoded->valid = true;
        const size_t joint_count = skin->joints_count;
        if (joint_count == 0u) {
            continue;
        }
        if (joint_count > SIZE_MAX / (sizeof(float) * 16u)) {
            return false;
        }
        decoded->joint_nodes = malloc(joint_count * sizeof(size_t));
        if (decoded->joint_nodes == NULL) {
            return false;
        }
        library->decoded_bytes += joint_count * sizeof(size_t);
        for (size_t joint = 0u; decoded->valid && joint < joint_count; joint += 1u) {
            const ptrdiff_t node_index = skin->joints[joint] - data->nodes;
            if (node_index < 0 || (cgltf_size)node_index >= node_count) {
                decoded->valid = false;
            } else {
                decoded->joint_nodes[joint] = (size_t)node_index;
            }
        }
        /* Fewer inverse binds than joints (cgltf_validate doesn't check skins): the skin can't be read, rather than
           read past its accessor (0.15.0 read past it when it sampled the skin). */
        if (decoded->valid &&
            skin->inverse_bind_matrices != NULL &&
            skin->inverse_bind_matrices->count < joint_count) {
            decoded->valid = false;
        }
        if (decoded->valid && skin->inverse_bind_matrices != NULL) {
            decoded->inverse_binds = calloc(joint_count * 16u, sizeof(float));
            if (decoded->inverse_binds == NULL) {
                return false;
            }
            library->decoded_bytes += joint_count * 16u * sizeof(float);
            for (size_t joint = 0u; decoded->valid && joint < joint_count; joint += 1u) {
                decoded->valid = cgltf_accessor_read_float(
                    skin->inverse_bind_matrices,
                    joint,
                    &decoded->inverse_binds[joint * 16u],
                    16u
                );
            }
        }
    }

    library->clips = calloc(data->animations_count, sizeof(*library->clips));
    if (library->clips == NULL) {
        return false;
    }
    library->decoded_bytes += data->animations_count * sizeof(*library->clips);
    uint32_t *accessor_maps = NULL;
    if (data->accessors_count > 0u) {
        accessor_maps = malloc(data->accessors_count * 3u * sizeof(uint32_t));
        if (accessor_maps == NULL) {
            return false;
        }
        for (size_t index = 0u; index < data->accessors_count * 3u; index += 1u) {
            accessor_maps[index] = BTBH_GPU_ANIMATION_NOT_DECODED;
        }
    }
    uint32_t key_time_capacity = 0u;
    uint32_t value_capacity = 0u;
    bool decoded = true;
    for (cgltf_size animation_index = 0u;
         decoded && animation_index < data->animations_count;
         animation_index += 1u) {
        BTBHGPUAnimationClip *clip = &library->clips[animation_index];
        decoded = btbh_gpu_animation_decode_clip(
            library,
            &data->animations[animation_index],
            clip,
            accessor_maps,
            accessor_maps + data->accessors_count,
            &key_time_capacity,
            &value_capacity
        );
        if (clip->time_slot_count > library->max_time_slots) {
            library->max_time_slots = clip->time_slot_count;
        }
    }
    free(accessor_maps);
    library->decoded_bytes += (size_t)key_time_capacity * sizeof(*library->key_times) +
        (size_t)value_capacity * sizeof(*library->values);
    return decoded;
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
    library->serial = btbh_gpu_animation_next_serial();
    if (!btbh_gpu_animation_library_decode(library)) {
        btbh_gpu_animation_library_free_decoded(library);
        cgltf_free(data);
        free(library);
        report->result = BTBH_GPU_DEVICE_FAILURE;
        (void) snprintf(
            report->message,
            sizeof(report->message),
            "%s",
            "Could not allocate animation library"
        );
        return NULL;
    }
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

/* Where a time falls among a set of key times (btbh_gpu_animation_find_segment). */
enum {
    /* At or before the first key, or from the last on: that key's value as it is. */
    BTBH_GPU_ANIMATION_SEGMENT_KEY = 0,
    /* Between two keys whose upper doesn't come after its lower: the lower key's value. */
    BTBH_GPU_ANIMATION_SEGMENT_HOLD = 1,
    /* Between two keys: interpolated over `interval` at `amount`. */
    BTBH_GPU_ANIMATION_SEGMENT_BETWEEN = 2
};

typedef struct BTBHGPUAnimationSegment {
    size_t lower;
    size_t upper;
    float interval;
    float amount;
    uint32_t kind;
} BTBHGPUAnimationSegment;

/* Where `time` falls among `keys`, found with the comparisons sampling ran on cgltf's reader: the first and last keys,
   then a binary search for the last key at or before the time. It depends on the times alone, so the channels that
   share them share it. */
static void btbh_gpu_animation_find_segment(
    const BTBHGPUAnimationKeyTimes *keys,
    float time,
    BTBHGPUAnimationSegment *segment
) {
    const float *times = keys->times;
    const size_t key_count = keys->count;
    const float first_time = times[0];
    const float last_time = times[key_count - 1u];
    segment->kind = BTBH_GPU_ANIMATION_SEGMENT_KEY;
    segment->interval = 0.0f;
    segment->amount = 0.0f;
    if (key_count == 1u || time <= first_time) {
        segment->lower = 0u;
        segment->upper = 0u;
        return;
    }
    if (time >= last_time) {
        segment->lower = key_count - 1u;
        segment->upper = key_count - 1u;
        return;
    }
    size_t lower = 0u;
    size_t upper = key_count - 1u;
    while (upper - lower > 1u) {
        const size_t middle = lower + (upper - lower) / 2u;
        if (time < times[middle]) {
            upper = middle;
        } else {
            lower = middle;
        }
    }
    const float lower_time = times[lower];
    const float upper_time = times[upper];
    segment->lower = lower;
    segment->upper = upper;
    if (upper_time <= lower_time) {
        segment->kind = BTBH_GPU_ANIMATION_SEGMENT_HOLD;
        return;
    }
    const float interval = upper_time - lower_time;
    const float amount = (time - lower_time) / interval;
    segment->kind = BTBH_GPU_ANIMATION_SEGMENT_BETWEEN;
    segment->interval = interval;
    segment->amount = amount;
}

/* A track's value at `segment` into `value`, as sampling computed it from cgltf's reader: a key's value writes the
   floats its read wrote (the rest of `value` keeps what it held); a step or a hold the lower key's components; a cubic
   spline the Hermite curve through the lower key's value and out-tangent and the upper's value and in-tangent; a
   rotation the shortest slerp; anything else the linear blend. */
static void btbh_gpu_animation_track_sample(
    const BTBHGPUAnimationTrack *track,
    const BTBHGPUAnimationSegment *segment,
    float *value
) {
    const size_t component_count = track->component_count;
    const bool cubic =
        track->interpolation == BTBH_GPU_ANIMATION_CUBIC_SPLINE;
    /* A key's value: element key (key * 3 + 1 among a cubic spline's in-tangent, value and out-tangent). */
    const size_t elements_per_key = cubic ? 3u : 1u;
    const size_t value_element = cubic ? 1u : 0u;
    const float *lower_value = &track->values[
        (segment->lower * elements_per_key + value_element) * component_count
    ];
    if (segment->kind == BTBH_GPU_ANIMATION_SEGMENT_KEY) {
        (void)memcpy(value, lower_value, track->written * sizeof(float));
        return;
    }
    if (track->interpolation == BTBH_GPU_ANIMATION_STEP ||
        segment->kind == BTBH_GPU_ANIMATION_SEGMENT_HOLD) {
        (void)memcpy(value, lower_value, component_count * sizeof(float));
        return;
    }
    const float *upper_value = &track->values[
        (segment->upper * elements_per_key + value_element) * component_count
    ];
    const float interval = segment->interval;
    const float amount = segment->amount;
    if (cubic) {
        const float *lower_tangent =
            &track->values[(segment->lower * 3u + 2u) * component_count];
        const float *upper_tangent =
            &track->values[(segment->upper * 3u) * component_count];
        const float amount2 = amount * amount;
        const float amount3 = amount2 * amount;
        const float h00 = 2.0f * amount3 - 3.0f * amount2 + 1.0f;
        const float h10 = amount3 - 2.0f * amount2 + amount;
        const float h01 = -2.0f * amount3 + 3.0f * amount2;
        const float h11 = amount3 - amount2;
        for (size_t component = 0u;
             component < component_count;
             component += 1u) {
            value[component] =
                h00 * lower_value[component] +
                h10 * interval * lower_tangent[component] +
                h01 * upper_value[component] +
                h11 * interval * upper_tangent[component];
        }
    } else if (
        track->path == BTBH_GPU_ANIMATION_PATH_ROTATION &&
        component_count == 4u
    ) {
        btbh_gpu_animation_slerp_shortest(
            lower_value,
            upper_value,
            amount,
            value
        );
    } else {
        for (size_t component = 0u;
             component < component_count;
             component += 1u) {
            value[component] =
                lower_value[component] +
                (upper_value[component] - lower_value[component]) *
                    amount;
        }
    }
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

/* The clip's local transforms at `time` into translations, rotations and scales (3, 4 and 3 floats a node), starting
   from the nodes' rest transforms; animated_nodes marks the nodes a channel set. `segments` has a place for each of
   the clip's time slots. */
static bool btbh_gpu_animation_sample_local_transforms(
    const BTBHGPUAnimationLibrary *library,
    uint32_t animation_index,
    float time,
    float *translations,
    float *rotations,
    float *scales,
    uint8_t *animated_nodes,
    BTBHGPUAnimationSegment *segments
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
    const size_t node_count = library->data->nodes_count;
    if (node_count > 0u) {
        (void)memcpy(
            translations,
            library->rest_translations,
            node_count * 3u * sizeof(float)
        );
        (void)memcpy(
            rotations,
            library->rest_rotations,
            node_count * 4u * sizeof(float)
        );
        (void)memcpy(
            scales,
            library->rest_scales,
            node_count * 3u * sizeof(float)
        );
        (void)memset(animated_nodes, 0, node_count * sizeof(uint8_t));
    }

    const BTBHGPUAnimationClip *clip = &library->clips[animation_index];
    if (!clip->valid) {
        return false;
    }
    for (uint32_t slot = 0u; slot < clip->time_slot_count; slot += 1u) {
        btbh_gpu_animation_find_segment(
            &library->key_times[clip->time_sets[slot]],
            time,
            &segments[slot]
        );
    }
    for (uint32_t index = 0u; index < clip->track_count; index += 1u) {
        const BTBHGPUAnimationTrack *track = &clip->tracks[index];
        float *target =
            track->path == BTBH_GPU_ANIMATION_PATH_ROTATION
                ? &rotations[track->node * 4u]
                : track->path == BTBH_GPU_ANIMATION_PATH_TRANSLATION
                    ? &translations[track->node * 3u]
                    : &scales[track->node * 3u];
        btbh_gpu_animation_track_sample(
            track,
            &segments[track->time_slot],
            target
        );
        animated_nodes[track->node] = 1u;
        if (track->path == BTBH_GPU_ANIMATION_PATH_ROTATION) {
            btbh_gpu_animation_normalize_quaternion(target);
        }
    }
    return true;
}

/* The skin's palette from the blended local transforms: every node's local and world matrix (local_matrices,
   world_matrices and states have room for every node), then each joint's world matrix times its inverse bind. */
static bool btbh_gpu_animation_build_skin_matrices(
    const BTBHGPUAnimationLibrary *library,
    uint32_t skin_index,
    const float *translations,
    const float *rotations,
    const float *scales,
    const uint8_t *animated_nodes,
    float *local_matrices,
    float *world_matrices,
    uint8_t *states,
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
    (void)memset(states, 0, data->nodes_count * sizeof(uint8_t));
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
    const BTBHGPUAnimationSkin *decoded = &library->skins[skin_index];
    valid = valid && decoded->valid;
    for (cgltf_size joint_index = 0u;
         valid && joint_index < skin->joints_count;
         joint_index += 1u) {
        const size_t node_index = decoded->joint_nodes[joint_index];
        const float *inverse_bind = decoded->inverse_binds != NULL
            ? &decoded->inverse_binds[joint_index * 16u]
            : identity;
        btbh_gpu_animation_multiply_matrix(
            &world_matrices[node_index * 16u],
            inverse_bind,
            &matrices[joint_index * 16u]
        );
    }
    return valid;
}

/* The floats a blended sample's scratch holds a node: the blend's translation, rotation and scale and its rotation
   weight, one sample's translation, rotation and scale, and the local and world matrices. */
#define BTBH_GPU_ANIMATION_SCRATCH_FLOATS_PER_NODE (3u + 4u + 3u + 1u + 3u + 4u + 3u + 16u + 16u)

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

    /* One block: the clip's segments, then the floats, then three bytes a node (the blend's animated nodes, one
       sample's, the world matrices' states). */
    const size_t node_count = data->nodes_count;
    const size_t node_bytes =
        BTBH_GPU_ANIMATION_SCRATCH_FLOATS_PER_NODE * sizeof(float) + 3u;
    const size_t segment_bytes =
        (size_t)library->max_time_slots * sizeof(BTBHGPUAnimationSegment);
    if (node_count > (SIZE_MAX - segment_bytes) / node_bytes) {
        return false;
    }
    uint8_t *scratch = malloc(segment_bytes + node_count * node_bytes);
    if (scratch == NULL) {
        return false;
    }
    BTBHGPUAnimationSegment *segments = (BTBHGPUAnimationSegment *)scratch;
    float *translations = (float *)(scratch + segment_bytes);
    float *rotations = translations + node_count * 3u;
    float *scales = rotations + node_count * 4u;
    float *rotation_weight_sums = scales + node_count * 3u;
    float *sample_translations = rotation_weight_sums + node_count;
    float *sample_rotations = sample_translations + node_count * 3u;
    float *sample_scales = sample_rotations + node_count * 4u;
    float *local_matrices = sample_scales + node_count * 3u;
    float *world_matrices = local_matrices + node_count * 16u;
    uint8_t *animated_nodes = (uint8_t *)(world_matrices + node_count * 16u);
    uint8_t *sample_animated_nodes = animated_nodes + node_count;
    uint8_t *states = sample_animated_nodes + node_count;
    /* The blend's sums start at zero: translations, rotations, scales and the rotation weights. */
    (void)memset(translations, 0, node_count * 11u * sizeof(float));
    (void)memset(animated_nodes, 0, node_count * sizeof(uint8_t));

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
            sample_animated_nodes,
            segments
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
            local_matrices,
            world_matrices,
            states,
            matrices,
            matrix_capacity
        );
    }
    free(scratch);
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
    /* The samples' bits, as before: the palettes kept under the pose's serial still hold. */
    const bool unchanged =
        pose->serial != 0u &&
        pose->sample_count == sample_count &&
        memcmp(pose->animation_indices, indices, (size_t)sample_count * sizeof(*indices)) == 0 &&
        memcmp(pose->animation_times, times, (size_t)sample_count * sizeof(*times)) == 0 &&
        memcmp(pose->animation_weights, weights, (size_t)sample_count * sizeof(*weights)) == 0;
    if (!unchanged) {
        pose->serial = btbh_gpu_animation_next_serial();
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
    btbh_gpu_animation_library_free_decoded(library);
    cgltf_free(library->data);
    free(library);
}

size_t btbh_gpu_animation_library_decoded_bytes(
    const BTBHGPUAnimationLibrary *library
) {
    return library != NULL ? library->decoded_bytes : 0u;
}

bool btbh_gpu_animation_library_sample_channel(
    const BTBHGPUAnimationLibrary *library,
    uint32_t animation_index,
    uint32_t channel_index,
    float time,
    float value[4]
) {
    if (library == NULL ||
        library->data == NULL ||
        animation_index >= library->data->animations_count ||
        !isfinite(time) ||
        value == NULL) {
        return false;
    }
    const cgltf_animation *animation =
        &library->data->animations[animation_index];
    const BTBHGPUAnimationClip *clip = &library->clips[animation_index];
    if (channel_index >= animation->channels_count || !clip->valid) {
        return false;
    }
    /* Its track: the sampled channels before it are the tracks before it. */
    uint32_t track_index = 0u;
    for (uint32_t index = 0u; index <= channel_index; index += 1u) {
        const cgltf_animation_channel *channel = &animation->channels[index];
        const bool sampled =
            channel->target_node != NULL &&
            (channel->target_path == cgltf_animation_path_type_translation ||
             channel->target_path == cgltf_animation_path_type_rotation ||
             channel->target_path == cgltf_animation_path_type_scale);
        if (index == channel_index) {
            if (!sampled) {
                return false;
            }
            break;
        }
        track_index += sampled ? 1u : 0u;
    }
    if (track_index >= clip->track_count) {
        return false;
    }
    const BTBHGPUAnimationTrack *track = &clip->tracks[track_index];
    BTBHGPUAnimationSegment segment;
    btbh_gpu_animation_find_segment(
        &library->key_times[clip->time_sets[track->time_slot]],
        time,
        &segment
    );
    btbh_gpu_animation_track_sample(track, &segment, value);
    return true;
}

/* ------------------------------------------------- Palettes kept across presents (since 0.16.0) */

static uint32_t btbh_gpu_animation_float_bits(float value) {
    uint32_t bits = 0u;
    (void)memcpy(&bits, &value, sizeof(bits));
    return bits;
}

bool btbh_gpu_bone_palette_key(
    const BTBHGPUAnimationLibrary *library,
    const BTBHGPUAnimationPose *pose,
    uint32_t animation_index,
    float animation_time,
    uint32_t skin_index,
    uint32_t joint_count,
    BTBHGPUBonePaletteKey *key
) {
    if (key == NULL) {
        return false;
    }
    (void)memset(key, 0, sizeof(*key));
    if (pose != NULL) {
        if (pose->library == NULL || pose->sample_count == 0u) {
            return false;
        }
        if (pose->sample_count == 1u) {
            /* One sample's weight over itself is 1: the palette is the library's at that clip and time. */
            key->serial = pose->library->serial;
            key->animation_index = pose->animation_indices[0];
            key->time_bits = btbh_gpu_animation_float_bits(pose->animation_times[0]);
        } else {
            key->serial = pose->serial;
            key->animation_index = UINT32_MAX;
        }
    } else {
        if (library == NULL) {
            return false;
        }
        key->serial = library->serial;
        key->animation_index = animation_index;
        key->time_bits = btbh_gpu_animation_float_bits(animation_time);
    }
    key->skin_index = skin_index;
    key->joint_count = joint_count;
    return true;
}

typedef struct BTBHGPUBonePaletteEntry {
    BTBHGPUBonePaletteKey key;
    /* key.joint_count matrices (a block is always its palette's size). */
    float *matrices;
    uint32_t hash;
    /* The cache's use that last stored or found it; 0 for an empty slot. */
    uint64_t last_use;
} BTBHGPUBonePaletteEntry;

/* An evicted palette's block, kept for the next palette of its size (a skin always comes back at its joint count). */
typedef struct BTBHGPUBonePaletteSpare {
    float *matrices;
    uint32_t joint_count;
} BTBHGPUBonePaletteSpare;

/* The most spare blocks kept, and how far down the spares a store looks for one of its size. */
#define BTBH_GPU_BONE_PALETTE_SPARE_LIMIT 256u
#define BTBH_GPU_BONE_PALETTE_SPARE_SEARCH 32u
/* The spares a use keeps however few palettes the last one stored. */
#define BTBH_GPU_BONE_PALETTE_SPARE_MINIMUM 16u

struct BTBHGPUBonePaletteCache {
    /* Open addressing with linear probing, a power of two of slots (0 before the first palette), at most half full. */
    BTBHGPUBonePaletteEntry *slots;
    uint32_t slot_count;
    uint32_t entry_count;
    /* The uses so far (btbh_gpu_bone_palette_cache_begin). */
    uint64_t use;
    /* The use in which a palette found no room beside that use's own: nothing more is kept until the next use. */
    uint64_t full_use;
    /* The matrices every block holds, the entries' and the spares': never more than BTBH_GPU_BONE_PALETTE_BUDGET. */
    size_t held_matrices;
    /* The matrices the entries' palettes hold. */
    size_t matrix_count;
    /* The palettes stored in this use and in the last: the spares the next use keeps (its likely turnover). */
    uint32_t stored_this_use;
    uint32_t stored_last_use;
    BTBHGPUBonePaletteSpare *spares;
    uint32_t spare_count;
    uint32_t spare_capacity;
};

static uint32_t btbh_gpu_bone_palette_hash(const BTBHGPUBonePaletteKey *key) {
    uint64_t hash = key->serial * 0x9e3779b97f4a7c15ull;
    hash ^= ((uint64_t)key->animation_index << 32u) | key->time_bits;
    hash *= 0xbf58476d1ce4e5b9ull;
    hash ^= ((uint64_t)key->skin_index << 32u) | key->joint_count;
    hash *= 0x94d049bb133111ebull;
    hash ^= hash >> 31u;
    return (uint32_t)hash ^ (uint32_t)(hash >> 32u);
}

static bool btbh_gpu_bone_palette_key_equal(
    const BTBHGPUBonePaletteKey *a,
    const BTBHGPUBonePaletteKey *b
) {
    return a->serial == b->serial &&
        a->animation_index == b->animation_index &&
        a->time_bits == b->time_bits &&
        a->skin_index == b->skin_index &&
        a->joint_count == b->joint_count;
}

/* Frees spare `index` (the top one takes its place). */
static void btbh_gpu_bone_palette_cache_free_spare(
    BTBHGPUBonePaletteCache *cache,
    uint32_t index
) {
    free(cache->spares[index].matrices);
    cache->held_matrices -= cache->spares[index].joint_count;
    cache->spare_count -= 1u;
    cache->spares[index] = cache->spares[cache->spare_count];
}

/* Keeps an evicted palette's block as a spare, or frees it past BTBH_GPU_BONE_PALETTE_SPARE_LIMIT. */
static void btbh_gpu_bone_palette_cache_keep_spare(
    BTBHGPUBonePaletteCache *cache,
    float *matrices,
    uint32_t joint_count
) {
    if (cache->spare_count == cache->spare_capacity &&
        cache->spare_capacity < BTBH_GPU_BONE_PALETTE_SPARE_LIMIT) {
        const uint32_t capacity = cache->spare_capacity == 0u ? 16u : cache->spare_capacity * 2u;
        BTBHGPUBonePaletteSpare *spares = realloc(cache->spares, capacity * sizeof(*spares));
        if (spares != NULL) {
            cache->spares = spares;
            cache->spare_capacity = capacity;
        }
    }
    if (cache->spare_count < cache->spare_capacity) {
        cache->spares[cache->spare_count].matrices = matrices;
        cache->spares[cache->spare_count].joint_count = joint_count;
        cache->spare_count += 1u;
    } else {
        free(matrices);
        cache->held_matrices -= joint_count;
    }
}

/* A spare block of exactly `joint_count` matrices from the top BTBH_GPU_BONE_PALETTE_SPARE_SEARCH spares, or NULL. */
static float *btbh_gpu_bone_palette_cache_take_spare(
    BTBHGPUBonePaletteCache *cache,
    uint32_t joint_count
) {
    const uint32_t lowest = cache->spare_count > BTBH_GPU_BONE_PALETTE_SPARE_SEARCH
        ? cache->spare_count - BTBH_GPU_BONE_PALETTE_SPARE_SEARCH
        : 0u;
    for (uint32_t index = cache->spare_count; index > lowest; index -= 1u) {
        BTBHGPUBonePaletteSpare *spare = &cache->spares[index - 1u];
        if (spare->joint_count == joint_count) {
            float *matrices = spare->matrices;
            cache->spare_count -= 1u;
            *spare = cache->spares[cache->spare_count];
            return matrices;
        }
    }
    return NULL;
}

/* Empties slot `index`, keeping its block as a spare, and shifts the entries probed past it back (no tombstones). */
static void btbh_gpu_bone_palette_cache_remove(
    BTBHGPUBonePaletteCache *cache,
    uint32_t index
) {
    BTBHGPUBonePaletteEntry *entry = &cache->slots[index];
    cache->matrix_count -= entry->key.joint_count;
    cache->entry_count -= 1u;
    btbh_gpu_bone_palette_cache_keep_spare(cache, entry->matrices, entry->key.joint_count);
    const uint32_t mask = cache->slot_count - 1u;
    uint32_t hole = index;
    uint32_t next = (hole + 1u) & mask;
    while (cache->slots[next].last_use != 0u) {
        const uint32_t home = cache->slots[next].hash & mask;
        /* It may fill the hole when the hole lies between its home slot and where it sits. */
        if (((next - home) & mask) >= ((next - hole) & mask)) {
            cache->slots[hole] = cache->slots[next];
            hole = next;
        }
        next = (next + 1u) & mask;
    }
    (void)memset(&cache->slots[hole], 0, sizeof(cache->slots[hole]));
}

/* Drops every palette last used before `oldest_kept_use`. */
static void btbh_gpu_bone_palette_cache_evict_before(
    BTBHGPUBonePaletteCache *cache,
    uint64_t oldest_kept_use
) {
    uint32_t index = 0u;
    while (cache->entry_count > 0u && index < cache->slot_count) {
        const BTBHGPUBonePaletteEntry *entry = &cache->slots[index];
        if (entry->last_use != 0u && entry->last_use < oldest_kept_use) {
            /* An entry shifted back into this slot is looked at next. */
            btbh_gpu_bone_palette_cache_remove(cache, index);
        } else {
            index += 1u;
        }
    }
}

BTBHGPUBonePaletteCache *btbh_gpu_bone_palette_cache_begin(
    BTBHGPUBonePaletteCache **slot
) {
    if (slot == NULL) {
        return NULL;
    }
    if (*slot == NULL) {
        *slot = calloc(1, sizeof(**slot));
        if (*slot == NULL) {
            return NULL;
        }
    }
    BTBHGPUBonePaletteCache *cache = *slot;
    cache->use += 1u;
    if (cache->use > BTBH_GPU_BONE_PALETTE_MAX_AGE) {
        btbh_gpu_bone_palette_cache_evict_before(
            cache,
            cache->use - BTBH_GPU_BONE_PALETTE_MAX_AGE
        );
    }
    /* The spares this use is likely to take: as many as the last use stored. */
    cache->stored_last_use = cache->stored_this_use;
    cache->stored_this_use = 0u;
    const uint32_t keep = cache->stored_last_use > BTBH_GPU_BONE_PALETTE_SPARE_MINIMUM
        ? cache->stored_last_use
        : BTBH_GPU_BONE_PALETTE_SPARE_MINIMUM;
    while (cache->spare_count > keep) {
        btbh_gpu_bone_palette_cache_free_spare(cache, 0u);
    }
    return cache;
}

const float *btbh_gpu_bone_palette_cache_find(
    BTBHGPUBonePaletteCache *cache,
    const BTBHGPUBonePaletteKey *key
) {
    if (cache == NULL || key == NULL || cache->entry_count == 0u) {
        return NULL;
    }
    const uint32_t hash = btbh_gpu_bone_palette_hash(key);
    const uint32_t mask = cache->slot_count - 1u;
    for (uint32_t index = hash & mask;
         cache->slots[index].last_use != 0u;
         index = (index + 1u) & mask) {
        BTBHGPUBonePaletteEntry *entry = &cache->slots[index];
        if (entry->hash == hash &&
            btbh_gpu_bone_palette_key_equal(&entry->key, key)) {
            entry->last_use = cache->use;
            return entry->matrices;
        }
    }
    return NULL;
}

/* Doubles the slots (at least 64), placing every entry anew. */
static bool btbh_gpu_bone_palette_cache_grow(BTBHGPUBonePaletteCache *cache) {
    const uint32_t slot_count = cache->slot_count == 0u ? 64u : cache->slot_count * 2u;
    if (slot_count == 0u || (size_t)slot_count > SIZE_MAX / sizeof(BTBHGPUBonePaletteEntry)) {
        return false;
    }
    BTBHGPUBonePaletteEntry *slots = calloc(slot_count, sizeof(*slots));
    if (slots == NULL) {
        return false;
    }
    const uint32_t mask = slot_count - 1u;
    for (uint32_t index = 0u; index < cache->slot_count; index += 1u) {
        const BTBHGPUBonePaletteEntry *entry = &cache->slots[index];
        if (entry->last_use == 0u) {
            continue;
        }
        uint32_t target = entry->hash & mask;
        while (slots[target].last_use != 0u) {
            target = (target + 1u) & mask;
        }
        slots[target] = *entry;
    }
    free(cache->slots);
    cache->slots = slots;
    cache->slot_count = slot_count;
    return true;
}

void btbh_gpu_bone_palette_cache_store(
    BTBHGPUBonePaletteCache *cache,
    const BTBHGPUBonePaletteKey *key,
    const float *matrices
) {
    if (cache == NULL ||
        key == NULL ||
        matrices == NULL ||
        key->joint_count == 0u ||
        key->joint_count > BTBH_GPU_BONE_PALETTE_BUDGET ||
        cache->full_use == cache->use ||
        btbh_gpu_bone_palette_cache_find(cache, key) != NULL) {
        return;
    }
    if (((size_t)cache->entry_count + 1u) * 2u > cache->slot_count &&
        !btbh_gpu_bone_palette_cache_grow(cache)) {
        return;
    }
    const uint32_t joint_count = key->joint_count;
    /* A spare of its size, else a new block within the budget: the spares go first, then the palettes unused the
       longest (their blocks become spares, which may fit). This use's palettes stay: when only they are left, this
       one isn't kept, and neither is any other until the next use. */
    float *block = btbh_gpu_bone_palette_cache_take_spare(cache, joint_count);
    while (block == NULL &&
           cache->held_matrices + joint_count > BTBH_GPU_BONE_PALETTE_BUDGET) {
        if (cache->spare_count > 0u) {
            btbh_gpu_bone_palette_cache_free_spare(cache, 0u);
            continue;
        }
        uint64_t oldest = UINT64_MAX;
        for (uint32_t index = 0u; index < cache->slot_count; index += 1u) {
            const uint64_t last_use = cache->slots[index].last_use;
            if (last_use != 0u && last_use < oldest) {
                oldest = last_use;
            }
        }
        if (oldest >= cache->use) {
            cache->full_use = cache->use;
            return;
        }
        btbh_gpu_bone_palette_cache_evict_before(cache, oldest + 1u);
        block = btbh_gpu_bone_palette_cache_take_spare(cache, joint_count);
    }
    if (block == NULL) {
        block = malloc((size_t)joint_count * 16u * sizeof(float));
        if (block == NULL) {
            return;
        }
        cache->held_matrices += joint_count;
    }
    (void)memcpy(block, matrices, (size_t)joint_count * 16u * sizeof(float));
    const uint32_t hash = btbh_gpu_bone_palette_hash(key);
    const uint32_t mask = cache->slot_count - 1u;
    uint32_t index = hash & mask;
    while (cache->slots[index].last_use != 0u) {
        index = (index + 1u) & mask;
    }
    BTBHGPUBonePaletteEntry *entry = &cache->slots[index];
    entry->key = *key;
    entry->matrices = block;
    entry->hash = hash;
    entry->last_use = cache->use;
    cache->entry_count += 1u;
    cache->matrix_count += joint_count;
    cache->stored_this_use += 1u;
}

void btbh_gpu_bone_palette_cache_read(
    const BTBHGPUBonePaletteCache *cache,
    uint32_t *palette_count,
    size_t *matrix_count,
    size_t *block_matrix_count,
    size_t *bytes
) {
    uint32_t palettes = 0u;
    size_t matrices = 0u;
    size_t blocks = 0u;
    size_t total = 0u;
    if (cache != NULL) {
        palettes = cache->entry_count;
        matrices = cache->matrix_count;
        blocks = cache->held_matrices;
        total = sizeof(*cache) +
            (size_t)cache->slot_count * sizeof(*cache->slots) +
            (size_t)cache->spare_capacity * sizeof(*cache->spares) +
            cache->held_matrices * 16u * sizeof(float);
    }
    if (palette_count != NULL) {
        *palette_count = palettes;
    }
    if (matrix_count != NULL) {
        *matrix_count = matrices;
    }
    if (block_matrix_count != NULL) {
        *block_matrix_count = blocks;
    }
    if (bytes != NULL) {
        *bytes = total;
    }
}

void btbh_gpu_bone_palette_cache_destroy(BTBHGPUBonePaletteCache *cache) {
    if (cache == NULL) {
        return;
    }
    for (uint32_t index = 0u; index < cache->slot_count; index += 1u) {
        if (cache->slots[index].last_use != 0u) {
            free(cache->slots[index].matrices);
        }
    }
    for (uint32_t spare = 0u; spare < cache->spare_count; spare += 1u) {
        free(cache->spares[spare].matrices);
    }
    free(cache->spares);
    free(cache->slots);
    free(cache);
}
