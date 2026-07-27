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

typedef struct BTBHGPUModelPrimitive {
    SDL_GPUBuffer *vertex_buffer;
    SDL_GPUBuffer *index_buffer;
    uint32_t vertex_count;
    uint32_t index_count;
    int32_t material_slot;
} BTBHGPUModelPrimitive;

struct BTBHGPUModel {
    BTBHGPUWindow *owner;
    BTBHGPUModel *previous;
    BTBHGPUModel *next;
    BTBHGPUModelPrimitive *primitives;
    uint32_t primitive_count;
    uint32_t material_count;
    float minimum[3];
    float maximum[3];
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
    free(vertices);
    if (destination->vertex_buffer == NULL) {
        return false;
    }
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
    free(indices);
    if (!valid) {
        return false;
    }
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

BTBHGPUModel *btbh_gpu_window_model_load(
    BTBHGPUWindow *window,
    const char *path,
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
        btbh_gpu_model_error(report, "Could not parse glTF model");
        return NULL;
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
            destination->material_slot = primitive->material != NULL
                ? (int32_t) (
                    primitive->material - data->materials + 1
                )
                : 0;
            valid =
                btbh_gpu_model_decode_vertices(
                    model,
                    primitive,
                    world_transform,
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
