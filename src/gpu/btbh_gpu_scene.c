#include "btbh_gpu_internal.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BTBH_GPU_MODEL_VERTEX_STRIDE 96u
#define BTBH_GPU_MODEL_COLOR_OFFSET 48u
#define BTBH_GPU_MODEL_JOINTS_OFFSET 64u
#define BTBH_GPU_MODEL_WEIGHTS_OFFSET 80u
#define BTBH_GPU_SCENE_INSTANCE_FLOATS 20u

struct BTBHGPUSceneRuntime {
    SDL_GPUGraphicsPipeline *model_pipeline;
    SDL_GPUGraphicsPipeline *animated_model_pipeline;
    SDL_GPUTexture *depth_texture;
    uint32_t depth_width;
    uint32_t depth_height;
};

static void btbh_gpu_scene_error(
    BTBHGPUSceneReport *report,
    BTBHGPUResult result,
    const char *message
) {
    report->result = result;
    (void)snprintf(
        report->message,
        sizeof(report->message),
        "%s",
        message != NULL ? message : ""
    );
}

static SDL_GPUShaderFormat btbh_gpu_scene_shader_format(
    BTBHGPUShaderFormat format
) {
    switch (format) {
        case BTBH_GPU_SHADER_FORMAT_SPIRV:
            return SDL_GPU_SHADERFORMAT_SPIRV;
        case BTBH_GPU_SHADER_FORMAT_METALLIB:
            return SDL_GPU_SHADERFORMAT_METALLIB;
        case BTBH_GPU_SHADER_FORMAT_INVALID:
        default:
            return SDL_GPU_SHADERFORMAT_INVALID;
    }
}

static SDL_GPUShader *btbh_gpu_scene_load_shader(
    SDL_GPUDevice *device,
    const char *path,
    const char *entrypoint,
    SDL_GPUShaderFormat format,
    SDL_GPUShaderStage stage,
    uint32_t storage_buffer_count,
    uint32_t uniform_buffer_count,
    BTBHGPUSceneReport *report
) {
    size_t byte_count = 0u;
    void *bytes = SDL_LoadFile(path, &byte_count);
    if (bytes == NULL || byte_count == 0u) {
        SDL_free(bytes);
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            path
        );
        return NULL;
    }
    SDL_GPUShaderCreateInfo info;
    SDL_zero(info);
    info.code = bytes;
    info.code_size = byte_count;
    info.entrypoint = entrypoint;
    info.format = format;
    info.stage = stage;
    info.num_storage_buffers = storage_buffer_count;
    info.num_uniform_buffers = uniform_buffer_count;
    SDL_GPUShader *shader = SDL_CreateGPUShader(device, &info);
    SDL_free(bytes);
    if (shader == NULL) {
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
    }
    return shader;
}

static bool btbh_gpu_scene_create_pipeline(
    BTBHGPUWindow *window,
    const char *vertex_shader_path,
    const char *animated_vertex_shader_path,
    const char *fragment_shader_path,
    BTBHGPUShaderFormat project_format,
    BTBHGPUSceneReport *report
) {
    SDL_GPUDevice *device = window->device->gpu;
    const SDL_GPUShaderFormat format =
        btbh_gpu_scene_shader_format(project_format);
    const char *entrypoint =
        project_format == BTBH_GPU_SHADER_FORMAT_METALLIB
            ? "main0"
            : "main";
    SDL_GPUShader *vertex_shader = btbh_gpu_scene_load_shader(
        device,
        vertex_shader_path,
        entrypoint,
        format,
        SDL_GPU_SHADERSTAGE_VERTEX,
        1u,
        1u,
        report
    );
    if (vertex_shader == NULL) {
        return false;
    }
    SDL_GPUShader *fragment_shader = btbh_gpu_scene_load_shader(
        device,
        fragment_shader_path,
        entrypoint,
        format,
        SDL_GPU_SHADERSTAGE_FRAGMENT,
        0u,
        0u,
        report
    );
    if (fragment_shader == NULL) {
        SDL_ReleaseGPUShader(device, vertex_shader);
        return false;
    }

    SDL_GPUColorTargetDescription color_target;
    SDL_GPUVertexBufferDescription vertex_buffer;
    SDL_GPUVertexAttribute attributes[4];
    SDL_GPUGraphicsPipelineCreateInfo info;
    SDL_zero(color_target);
    SDL_zero(vertex_buffer);
    SDL_zeroa(attributes);
    SDL_zero(info);

    color_target.format = SDL_GetGPUSwapchainTextureFormat(
        device,
        window->window
    );
    color_target.blend_state.enable_blend = true;
    color_target.blend_state.color_write_mask = 0x0fu;
    color_target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    color_target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    color_target.blend_state.src_color_blendfactor =
        SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    color_target.blend_state.dst_color_blendfactor =
        SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    color_target.blend_state.src_alpha_blendfactor =
        SDL_GPU_BLENDFACTOR_ONE;
    color_target.blend_state.dst_alpha_blendfactor =
        SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;

    vertex_buffer.slot = 0u;
    vertex_buffer.pitch = BTBH_GPU_MODEL_VERTEX_STRIDE;
    vertex_buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    vertex_buffer.instance_step_rate = 0u;
    attributes[0].location = 0u;
    attributes[0].buffer_slot = 0u;
    attributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    attributes[0].offset = 0u;
    attributes[1].location = 1u;
    attributes[1].buffer_slot = 0u;
    attributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    attributes[1].offset = BTBH_GPU_MODEL_COLOR_OFFSET;
    attributes[2].location = 2u;
    attributes[2].buffer_slot = 0u;
    attributes[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    attributes[2].offset = BTBH_GPU_MODEL_JOINTS_OFFSET;
    attributes[3].location = 3u;
    attributes[3].buffer_slot = 0u;
    attributes[3].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    attributes[3].offset = BTBH_GPU_MODEL_WEIGHTS_OFFSET;

    info.vertex_shader = vertex_shader;
    info.fragment_shader = fragment_shader;
    info.vertex_input_state.vertex_buffer_descriptions = &vertex_buffer;
    info.vertex_input_state.num_vertex_buffers = 1u;
    info.vertex_input_state.vertex_attributes = attributes;
    info.vertex_input_state.num_vertex_attributes = 2u;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    info.depth_stencil_state.enable_depth_test = true;
    info.depth_stencil_state.enable_depth_write = true;
    info.target_info.color_target_descriptions = &color_target;
    info.target_info.num_color_targets = 1u;
    info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    info.target_info.has_depth_stencil_target = true;

    window->scene_runtime->model_pipeline =
        SDL_CreateGPUGraphicsPipeline(device, &info);
    SDL_ReleaseGPUShader(device, vertex_shader);
    if (window->scene_runtime->model_pipeline == NULL) {
        SDL_ReleaseGPUShader(device, fragment_shader);
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return false;
    }
    SDL_GPUShader *animated_vertex_shader = btbh_gpu_scene_load_shader(
        device,
        animated_vertex_shader_path,
        entrypoint,
        format,
        SDL_GPU_SHADERSTAGE_VERTEX,
        2u,
        1u,
        report
    );
    if (animated_vertex_shader == NULL) {
        SDL_ReleaseGPUShader(device, fragment_shader);
        return false;
    }
    info.vertex_shader = animated_vertex_shader;
    info.vertex_input_state.num_vertex_attributes = 4u;
    window->scene_runtime->animated_model_pipeline =
        SDL_CreateGPUGraphicsPipeline(device, &info);
    SDL_ReleaseGPUShader(device, animated_vertex_shader);
    SDL_ReleaseGPUShader(device, fragment_shader);
    if (window->scene_runtime->animated_model_pipeline == NULL) {
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return false;
    }
    return true;
}

static bool btbh_gpu_scene_ensure_runtime(
    BTBHGPUWindow *window,
    const char *vertex_shader_path,
    const char *animated_vertex_shader_path,
    const char *fragment_shader_path,
    BTBHGPUShaderFormat shader_format,
    BTBHGPUSceneReport *report
) {
    if (window->scene_runtime != NULL) {
        return true;
    }
    window->scene_runtime = (BTBHGPUSceneRuntime *)calloc(
        1u,
        sizeof(BTBHGPUSceneRuntime)
    );
    if (window->scene_runtime == NULL) {
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_PLATFORM_FAILURE,
            "Could not allocate the GPU scene runtime"
        );
        return false;
    }
    if (!btbh_gpu_scene_create_pipeline(
            window,
            vertex_shader_path,
            animated_vertex_shader_path,
            fragment_shader_path,
            shader_format,
            report
        )) {
        btbh_gpu_window_release_scene(window);
        return false;
    }
    return true;
}

static bool btbh_gpu_scene_ensure_depth(
    BTBHGPUWindow *window,
    uint32_t width,
    uint32_t height,
    BTBHGPUSceneReport *report
) {
    BTBHGPUSceneRuntime *runtime = window->scene_runtime;
    SDL_GPUDevice *device = window->device->gpu;
    if (runtime->depth_texture != NULL &&
        runtime->depth_width == width &&
        runtime->depth_height == height) {
        return true;
    }
    if (runtime->depth_texture != NULL) {
        SDL_ReleaseGPUTexture(device, runtime->depth_texture);
        runtime->depth_texture = NULL;
    }
    SDL_GPUTextureCreateInfo info;
    SDL_zero(info);
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1u;
    info.num_levels = 1u;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    runtime->depth_texture = SDL_CreateGPUTexture(device, &info);
    if (runtime->depth_texture == NULL) {
        runtime->depth_width = 0u;
        runtime->depth_height = 0u;
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return false;
    }
    runtime->depth_width = width;
    runtime->depth_height = height;
    return true;
}

static bool btbh_gpu_scene_validate_batches(
    BTBHGPUWindow *window,
    uint32_t total_instance_count,
    const BTBHGPUSceneGeometryBatch *batches,
    uint32_t batch_count
) {
    for (uint32_t index = 0u; index < batch_count; ++index) {
        const BTBHGPUSceneGeometryBatch *batch = &batches[index];
        const bool valid_geometry =
            (batch->kind == BTBH_GPU_SCENE_GEOMETRY_MODEL &&
                batch->model != NULL &&
                batch->model->owner == window &&
                batch->model->primitives != NULL) ||
            (batch->kind == BTBH_GPU_SCENE_GEOMETRY_CUSTOM_MESH &&
                batch->custom_mesh != NULL &&
                batch->custom_mesh->owner == window &&
                batch->custom_mesh->vertex_buffer != NULL);
        const bool valid_animation =
            batch->animation_library == NULL ||
            (batch->kind == BTBH_GPU_SCENE_GEOMETRY_MODEL &&
                isfinite(batch->animation_time));
        if (!valid_geometry ||
            !valid_animation ||
            batch->instance_count == 0u ||
            batch->first_instance > total_instance_count ||
            batch->instance_count >
                total_instance_count - batch->first_instance) {
            return false;
        }
    }
    return true;
}

static bool btbh_gpu_scene_prepare_bones(
    const BTBHGPUSceneGeometryBatch *batches,
    uint32_t batch_count,
    uint32_t **bone_offsets,
    uint32_t *geometry_count,
    float **bone_matrices,
    uint32_t *bone_count
) {
    uint32_t total_geometry_count = 0u;
    uint32_t total_bone_count = 0u;
    for (uint32_t batch_index = 0u;
         batch_index < batch_count;
         batch_index += 1u) {
        const BTBHGPUSceneGeometryBatch *batch = &batches[batch_index];
        const uint32_t batch_geometry_count =
            batch->kind == BTBH_GPU_SCENE_GEOMETRY_MODEL
                ? batch->model->primitive_count
                : 1u;
        if (
            batch_geometry_count > UINT32_MAX - total_geometry_count
        ) {
            return false;
        }
        total_geometry_count += batch_geometry_count;
        if (batch->animation_library == NULL) {
            continue;
        }
        for (uint32_t primitive_index = 0u;
             primitive_index < batch_geometry_count;
             primitive_index += 1u) {
            const int32_t skin_index =
                batch->model->primitives[primitive_index].skin_index;
            if (skin_index < 0) {
                continue;
            }
            const uint32_t joint_count =
                btbh_gpu_animation_library_skin_joint_count(
                    batch->animation_library,
                    (uint32_t)skin_index
                );
            if (
                joint_count == 0u ||
                joint_count > UINT32_MAX - total_bone_count
            ) {
                return false;
            }
            total_bone_count += joint_count;
        }
    }
    if (
        total_geometry_count == 0u ||
        (size_t)total_geometry_count >
            SIZE_MAX / sizeof(uint32_t) ||
        (size_t)total_bone_count >
            SIZE_MAX / (sizeof(float) * 16u)
    ) {
        return false;
    }
    uint32_t *offsets = malloc(
        (size_t)total_geometry_count * sizeof(*offsets)
    );
    float *matrices = total_bone_count > 0u
        ? malloc(
            (size_t)total_bone_count *
            sizeof(float) *
            16u
        )
        : NULL;
    if (offsets == NULL || (total_bone_count > 0u && matrices == NULL)) {
        free(matrices);
        free(offsets);
        return false;
    }

    uint32_t geometry_index = 0u;
    uint32_t bone_index = 0u;
    bool valid = true;
    for (uint32_t batch_index = 0u;
         valid && batch_index < batch_count;
         batch_index += 1u) {
        const BTBHGPUSceneGeometryBatch *batch = &batches[batch_index];
        const uint32_t batch_geometry_count =
            batch->kind == BTBH_GPU_SCENE_GEOMETRY_MODEL
                ? batch->model->primitive_count
                : 1u;
        for (uint32_t primitive_index = 0u;
             valid && primitive_index < batch_geometry_count;
             primitive_index += 1u) {
            offsets[geometry_index] = UINT32_MAX;
            if (batch->animation_library != NULL) {
                const int32_t skin_index =
                    batch->model->primitives[primitive_index].skin_index;
                if (skin_index >= 0) {
                    const uint32_t joint_count =
                        btbh_gpu_animation_library_skin_joint_count(
                            batch->animation_library,
                            (uint32_t)skin_index
                        );
                    offsets[geometry_index] = bone_index;
                    valid = btbh_gpu_animation_library_sample_skin(
                        batch->animation_library,
                        batch->animation_index,
                        batch->animation_time,
                        (uint32_t)skin_index,
                        &matrices[(size_t)bone_index * 16u],
                        joint_count
                    );
                    bone_index += joint_count;
                }
            }
            geometry_index += 1u;
        }
    }
    if (!valid) {
        free(matrices);
        free(offsets);
        return false;
    }
    *bone_offsets = offsets;
    *geometry_count = total_geometry_count;
    *bone_matrices = matrices;
    *bone_count = total_bone_count;
    return true;
}

BTBHGPUResult btbh_gpu_window_scene_present_instances(
    BTBHGPUWindow *window,
    const char *vertex_shader_path,
    const char *animated_vertex_shader_path,
    const char *fragment_shader_path,
    BTBHGPUShaderFormat shader_format,
    const float view_projection[16],
    const float *instances,
    uint32_t total_instance_count,
    const BTBHGPUSceneGeometryBatch *batches,
    uint32_t batch_count,
    uint8_t clear_red,
    uint8_t clear_green,
    uint8_t clear_blue,
    uint8_t clear_alpha,
    BTBHGPUSceneReport *report
) {
    if (report == NULL) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    (void)memset(report, 0, sizeof(*report));
    if (window != NULL && window->pending_command != NULL) {
        (void)btbh_gpu_window_finish_pending_frame(window, false);
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            "GPU scene frame was not finalized by Canvas2D"
        );
        return report->result;
    }
    if (window == NULL ||
        window->window == NULL ||
        window->device == NULL ||
        window->device->gpu == NULL ||
        !window->claimed ||
        vertex_shader_path == NULL ||
        animated_vertex_shader_path == NULL ||
        fragment_shader_path == NULL ||
        view_projection == NULL ||
        instances == NULL ||
        total_instance_count == 0u ||
        batches == NULL ||
        batch_count == 0u ||
        btbh_gpu_scene_shader_format(shader_format) ==
            SDL_GPU_SHADERFORMAT_INVALID ||
        (uint64_t)total_instance_count *
            BTBH_GPU_SCENE_INSTANCE_FLOATS * sizeof(float) > UINT32_MAX ||
        !btbh_gpu_scene_validate_batches(
            window,
            total_instance_count,
            batches,
            batch_count
        )) {
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            "Invalid GPU model instance scene submission"
        );
        return report->result;
    }
    if (!btbh_gpu_scene_ensure_runtime(
            window,
            vertex_shader_path,
            animated_vertex_shader_path,
            fragment_shader_path,
            shader_format,
            report
        )) {
        return report->result;
    }

    uint32_t *bone_offsets = NULL;
    uint32_t prepared_geometry_count = 0u;
    float *bone_matrices = NULL;
    uint32_t bone_count = 0u;
    if (
        !btbh_gpu_scene_prepare_bones(
            batches,
            batch_count,
            &bone_offsets,
            &prepared_geometry_count,
            &bone_matrices,
            &bone_count
        ) ||
        bone_count > UINT32_MAX / (sizeof(float) * 16u)
    ) {
        free(bone_matrices);
        free(bone_offsets);
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            "Could not prepare GPU animation pose"
        );
        return report->result;
    }

    SDL_GPUDevice *device = window->device->gpu;
    const uint32_t instance_bytes =
        total_instance_count *
        BTBH_GPU_SCENE_INSTANCE_FLOATS *
        (uint32_t)sizeof(float);
    const uint32_t bone_bytes =
        bone_count * (uint32_t)(sizeof(float) * 16u);
    if (bone_bytes > UINT32_MAX - instance_bytes) {
        free(bone_matrices);
        free(bone_offsets);
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            "GPU scene upload exceeds buffer limits"
        );
        return report->result;
    }
    const uint32_t transfer_bytes = instance_bytes + bone_bytes;
    SDL_GPUBufferCreateInfo buffer_info;
    SDL_GPUTransferBufferCreateInfo transfer_info;
    SDL_zero(buffer_info);
    SDL_zero(transfer_info);
    buffer_info.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    buffer_info.size = instance_bytes;
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size = instance_bytes;
    SDL_GPUBuffer *instance_buffer = SDL_CreateGPUBuffer(
        device,
        &buffer_info
    );
    SDL_GPUBuffer *bone_buffer = NULL;
    if (bone_bytes > 0u) {
        buffer_info.size = bone_bytes;
        bone_buffer = SDL_CreateGPUBuffer(device, &buffer_info);
    }
    transfer_info.size = transfer_bytes;
    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(
        device,
        &transfer_info
    );
    if (
        instance_buffer == NULL ||
        (bone_bytes > 0u && bone_buffer == NULL) ||
        transfer == NULL
    ) {
        if (transfer != NULL) {
            SDL_ReleaseGPUTransferBuffer(device, transfer);
        }
        if (bone_buffer != NULL) {
            SDL_ReleaseGPUBuffer(device, bone_buffer);
        }
        if (instance_buffer != NULL) {
            SDL_ReleaseGPUBuffer(device, instance_buffer);
        }
        free(bone_matrices);
        free(bone_offsets);
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return report->result;
    }
    void *mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
    if (mapped == NULL) {
        SDL_ReleaseGPUTransferBuffer(device, transfer);
        if (bone_buffer != NULL) {
            SDL_ReleaseGPUBuffer(device, bone_buffer);
        }
        SDL_ReleaseGPUBuffer(device, instance_buffer);
        free(bone_matrices);
        free(bone_offsets);
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return report->result;
    }
    (void)memcpy(mapped, instances, instance_bytes);
    if (bone_bytes > 0u) {
        (void)memcpy(
            (uint8_t *)mapped + instance_bytes,
            bone_matrices,
            bone_bytes
        );
    }
    SDL_UnmapGPUTransferBuffer(device, transfer);
    free(bone_matrices);
    bone_matrices = NULL;

    SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(device);
    if (command == NULL) {
        SDL_ReleaseGPUTransferBuffer(device, transfer);
        if (bone_buffer != NULL) {
            SDL_ReleaseGPUBuffer(device, bone_buffer);
        }
        SDL_ReleaseGPUBuffer(device, instance_buffer);
        free(bone_offsets);
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return report->result;
    }
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command);
    if (copy == NULL) {
        (void)SDL_CancelGPUCommandBuffer(command);
        SDL_ReleaseGPUTransferBuffer(device, transfer);
        if (bone_buffer != NULL) {
            SDL_ReleaseGPUBuffer(device, bone_buffer);
        }
        SDL_ReleaseGPUBuffer(device, instance_buffer);
        free(bone_offsets);
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return report->result;
    }
    SDL_GPUTransferBufferLocation source;
    SDL_GPUBufferRegion destination;
    SDL_zero(source);
    SDL_zero(destination);
    source.transfer_buffer = transfer;
    destination.buffer = instance_buffer;
    destination.size = instance_bytes;
    SDL_UploadToGPUBuffer(copy, &source, &destination, false);
    if (bone_buffer != NULL) {
        source.offset = instance_bytes;
        destination.buffer = bone_buffer;
        destination.size = bone_bytes;
        SDL_UploadToGPUBuffer(copy, &source, &destination, false);
    }
    SDL_EndGPUCopyPass(copy);

    SDL_GPUTexture *swapchain = NULL;
    uint32_t drawable_width = 0u;
    uint32_t drawable_height = 0u;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(
            command,
            window->window,
            &swapchain,
            &drawable_width,
            &drawable_height
        ) ||
        swapchain == NULL ||
        drawable_width == 0u ||
        drawable_height == 0u ||
        !btbh_gpu_scene_ensure_depth(
            window,
            drawable_width,
            drawable_height,
            report
        )) {
        if (report->result == BTBH_GPU_OK) {
            btbh_gpu_scene_error(
                report,
                BTBH_GPU_WINDOW_FAILURE,
                SDL_GetError()
            );
        }
        (void)SDL_CancelGPUCommandBuffer(command);
        SDL_ReleaseGPUTransferBuffer(device, transfer);
        if (bone_buffer != NULL) {
            SDL_ReleaseGPUBuffer(device, bone_buffer);
        }
        SDL_ReleaseGPUBuffer(device, instance_buffer);
        free(bone_offsets);
        return report->result;
    }

    SDL_GPUColorTargetInfo color_target;
    SDL_GPUDepthStencilTargetInfo depth_target;
    SDL_zero(color_target);
    SDL_zero(depth_target);
    color_target.texture = swapchain;
    color_target.clear_color.r = (float)clear_red / 255.0f;
    color_target.clear_color.g = (float)clear_green / 255.0f;
    color_target.clear_color.b = (float)clear_blue / 255.0f;
    color_target.clear_color.a = (float)clear_alpha / 255.0f;
    color_target.load_op = SDL_GPU_LOADOP_CLEAR;
    color_target.store_op = SDL_GPU_STOREOP_STORE;
    depth_target.texture = window->scene_runtime->depth_texture;
    depth_target.clear_depth = 1.0f;
    depth_target.load_op = SDL_GPU_LOADOP_CLEAR;
    depth_target.store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth_target.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth_target.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth_target.cycle = true;

    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(
        command,
        &color_target,
        1u,
        &depth_target
    );
    if (pass == NULL) {
        (void)SDL_CancelGPUCommandBuffer(command);
        SDL_ReleaseGPUTransferBuffer(device, transfer);
        if (bone_buffer != NULL) {
            SDL_ReleaseGPUBuffer(device, bone_buffer);
        }
        SDL_ReleaseGPUBuffer(device, instance_buffer);
        free(bone_offsets);
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return report->result;
    }
    uint32_t primitive_draw_count = 0u;
    for (uint32_t batch_index = 0u;
         batch_index < batch_count;
         ++batch_index) {
        const BTBHGPUSceneGeometryBatch *batch = &batches[batch_index];
        const uint32_t geometry_count =
            batch->kind == BTBH_GPU_SCENE_GEOMETRY_MODEL
                ? batch->model->primitive_count
                : 1u;
        for (uint32_t geometry_index = 0u;
             geometry_index < geometry_count;
             ++geometry_index) {
            const uint32_t bone_offset =
                bone_offsets[primitive_draw_count];
            const bool animated = bone_offset != UINT32_MAX;
            if (animated) {
                struct {
                    float view_projection[16];
                    uint32_t bone_offset;
                    uint32_t padding[3];
                } uniforms;
                (void)memset(&uniforms, 0, sizeof(uniforms));
                (void)memcpy(
                    uniforms.view_projection,
                    view_projection,
                    sizeof(uniforms.view_projection)
                );
                uniforms.bone_offset = bone_offset;
                SDL_PushGPUVertexUniformData(
                    command,
                    0u,
                    &uniforms,
                    sizeof(uniforms)
                );
                SDL_BindGPUGraphicsPipeline(
                    pass,
                    window->scene_runtime->animated_model_pipeline
                );
                SDL_GPUBuffer *storage_buffers[2] = {
                    instance_buffer,
                    bone_buffer
                };
                SDL_BindGPUVertexStorageBuffers(
                    pass,
                    0u,
                    storage_buffers,
                    2u
                );
            } else {
                SDL_PushGPUVertexUniformData(
                    command,
                    0u,
                    view_projection,
                    sizeof(float) * 16u
                );
                SDL_BindGPUGraphicsPipeline(
                    pass,
                    window->scene_runtime->model_pipeline
                );
                SDL_BindGPUVertexStorageBuffers(
                    pass,
                    0u,
                    &instance_buffer,
                    1u
                );
            }
            SDL_GPUBuffer *vertex_buffer = NULL;
            SDL_GPUBuffer *index_buffer = NULL;
            uint32_t vertex_count = 0u;
            uint32_t index_count = 0u;
            if (batch->kind == BTBH_GPU_SCENE_GEOMETRY_MODEL) {
                const BTBHGPUModelPrimitive *primitive =
                    &batch->model->primitives[geometry_index];
                vertex_buffer = primitive->vertex_buffer;
                index_buffer = primitive->index_buffer;
                vertex_count = primitive->vertex_count;
                index_count = primitive->index_count;
            } else {
                vertex_buffer = batch->custom_mesh->vertex_buffer;
                index_buffer = batch->custom_mesh->index_buffer;
                vertex_count = batch->custom_mesh->vertex_count;
                index_count = batch->custom_mesh->index_count;
            }
            SDL_GPUBufferBinding vertex_binding;
            SDL_zero(vertex_binding);
            vertex_binding.buffer = vertex_buffer;
            SDL_BindGPUVertexBuffers(
                pass,
                0u,
                &vertex_binding,
                1u
            );
            if (index_buffer != NULL && index_count > 0u) {
                SDL_GPUBufferBinding index_binding;
                SDL_zero(index_binding);
                index_binding.buffer = index_buffer;
                SDL_BindGPUIndexBuffer(
                    pass,
                    &index_binding,
                    SDL_GPU_INDEXELEMENTSIZE_32BIT
                );
                SDL_DrawGPUIndexedPrimitives(
                    pass,
                    index_count,
                    batch->instance_count,
                    0u,
                    0,
                    batch->first_instance
                );
            } else {
                SDL_DrawGPUPrimitives(
                    pass,
                    vertex_count,
                    batch->instance_count,
                    0u,
                    batch->first_instance
                );
            }
            primitive_draw_count += 1u;
        }
    }
    SDL_EndGPURenderPass(pass);
    if (primitive_draw_count != prepared_geometry_count) {
        (void)SDL_CancelGPUCommandBuffer(command);
        SDL_ReleaseGPUTransferBuffer(device, transfer);
        if (bone_buffer != NULL) {
            SDL_ReleaseGPUBuffer(device, bone_buffer);
        }
        SDL_ReleaseGPUBuffer(device, instance_buffer);
        free(bone_offsets);
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            "GPU scene geometry preparation changed during submission"
        );
        return report->result;
    }
    window->pending_command = command;
    window->pending_swapchain = swapchain;
    window->pending_scene_transfer = transfer;
    window->pending_instance_buffer = instance_buffer;
    window->pending_bone_buffer = bone_buffer;
    window->pending_drawable_width = drawable_width;
    window->pending_drawable_height = drawable_height;
    free(bone_offsets);

    report->result = BTBH_GPU_OK;
    report->drawable_width = drawable_width;
    report->drawable_height = drawable_height;
    report->batch_count = batch_count;
    report->primitive_draw_count = primitive_draw_count;
    report->instance_count = total_instance_count;
    report->message[0] = '\0';
    return report->result;
}

void btbh_gpu_window_release_scene(BTBHGPUWindow *window) {
    if (window == NULL || window->scene_runtime == NULL) {
        return;
    }
    SDL_GPUDevice *device =
        window->device != NULL && !window->device->shutdown
            ? window->device->gpu
            : NULL;
    if (device != NULL) {
        if (window->scene_runtime->depth_texture != NULL) {
            SDL_ReleaseGPUTexture(
                device,
                window->scene_runtime->depth_texture
            );
        }
        if (window->scene_runtime->model_pipeline != NULL) {
            SDL_ReleaseGPUGraphicsPipeline(
                device,
                window->scene_runtime->model_pipeline
            );
        }
        if (window->scene_runtime->animated_model_pipeline != NULL) {
            SDL_ReleaseGPUGraphicsPipeline(
                device,
                window->scene_runtime->animated_model_pipeline
            );
        }
    }
    free(window->scene_runtime);
    window->scene_runtime = NULL;
}
