#include "btbh_gpu_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BTBH_GPU_CANVAS_VERTEX_STRIDE 32u
#define BTBH_GPU_CANVAS_PIPELINE_COUNT 4u

struct BTBHGPUCanvasRuntime {
    SDL_GPUGraphicsPipeline
        *solid_pipelines[BTBH_GPU_CANVAS_PIPELINE_COUNT];
    SDL_GPUGraphicsPipeline
        *textured_pipelines[BTBH_GPU_CANVAS_PIPELINE_COUNT];
};

static void btbh_gpu_canvas_error(
    BTBHGPUCanvasReport *report,
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

static SDL_GPUShaderFormat btbh_gpu_canvas_shader_format(
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

static SDL_GPUShader *btbh_gpu_canvas_load_shader(
    SDL_GPUDevice *device,
    const char *path,
    const char *entrypoint,
    SDL_GPUShaderFormat format,
    SDL_GPUShaderStage stage,
    uint32_t sampler_count,
    uint32_t uniform_buffer_count,
    BTBHGPUCanvasReport *report
) {
    size_t byte_count = 0u;
    void *bytes = SDL_LoadFile(path, &byte_count);
    if (bytes == NULL || byte_count == 0u) {
        SDL_free(bytes);
        btbh_gpu_canvas_error(
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
    info.num_samplers = sampler_count;
    info.num_uniform_buffers = uniform_buffer_count;
    SDL_GPUShader *shader = SDL_CreateGPUShader(device, &info);
    SDL_free(bytes);
    if (shader == NULL) {
        btbh_gpu_canvas_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
    }
    return shader;
}

static void btbh_gpu_canvas_blend_state(
    BTBHGPUCanvasBlendMode mode,
    SDL_GPUColorTargetBlendState *state
) {
    SDL_zero(*state);
    state->enable_blend = true;
    state->color_blend_op = SDL_GPU_BLENDOP_ADD;
    state->alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    state->src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    state->dst_alpha_blendfactor =
        SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    switch (mode) {
        case BTBH_GPU_CANVAS_BLEND_ADDITIVE:
            state->src_color_blendfactor =
                SDL_GPU_BLENDFACTOR_SRC_ALPHA;
            state->dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            break;
        case BTBH_GPU_CANVAS_BLEND_MULTIPLIED:
            state->src_color_blendfactor =
                SDL_GPU_BLENDFACTOR_DST_COLOR;
            state->dst_color_blendfactor =
                SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            break;
        case BTBH_GPU_CANVAS_BLEND_PREMULTIPLIED_ALPHA:
            state->src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            state->dst_color_blendfactor =
                SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            break;
        case BTBH_GPU_CANVAS_BLEND_ALPHA:
        default:
            state->src_color_blendfactor =
                SDL_GPU_BLENDFACTOR_SRC_ALPHA;
            state->dst_color_blendfactor =
                SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            break;
    }
}

static bool btbh_gpu_canvas_create_runtime(
    BTBHGPUWindow *window,
    const char *vertex_shader_path,
    const char *fragment_shader_path,
    const char *textured_fragment_shader_path,
    BTBHGPUShaderFormat project_format,
    BTBHGPUCanvasReport *report
) {
    SDL_GPUDevice *device = window->device->gpu;
    const SDL_GPUShaderFormat format =
        btbh_gpu_canvas_shader_format(project_format);
    const char *entrypoint =
        project_format == BTBH_GPU_SHADER_FORMAT_METALLIB
            ? "main0"
            : "main";
    SDL_GPUShader *vertex_shader = btbh_gpu_canvas_load_shader(
        device,
        vertex_shader_path,
        entrypoint,
        format,
        SDL_GPU_SHADERSTAGE_VERTEX,
        0u,
        1u,
        report
    );
    if (vertex_shader == NULL) {
        return false;
    }
    SDL_GPUShader *fragment_shader = btbh_gpu_canvas_load_shader(
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
    SDL_GPUShader *textured_fragment_shader =
        btbh_gpu_canvas_load_shader(
            device,
            textured_fragment_shader_path,
            entrypoint,
            format,
            SDL_GPU_SHADERSTAGE_FRAGMENT,
            1u,
            0u,
            report
        );
    if (textured_fragment_shader == NULL) {
        SDL_ReleaseGPUShader(device, fragment_shader);
        SDL_ReleaseGPUShader(device, vertex_shader);
        return false;
    }
    window->canvas_runtime = (BTBHGPUCanvasRuntime *)calloc(
        1u,
        sizeof(*window->canvas_runtime)
    );
    if (window->canvas_runtime == NULL) {
        SDL_ReleaseGPUShader(device, textured_fragment_shader);
        SDL_ReleaseGPUShader(device, fragment_shader);
        SDL_ReleaseGPUShader(device, vertex_shader);
        btbh_gpu_canvas_error(
            report,
            BTBH_GPU_PLATFORM_FAILURE,
            "Could not allocate the GPU canvas runtime"
        );
        return false;
    }

    SDL_GPUVertexBufferDescription vertex_buffer;
    SDL_GPUVertexAttribute attributes[3];
    SDL_GPUColorTargetDescription color_target;
    SDL_GPUGraphicsPipelineCreateInfo info;
    SDL_zero(vertex_buffer);
    SDL_zeroa(attributes);
    SDL_zero(color_target);
    SDL_zero(info);
    vertex_buffer.slot = 0u;
    vertex_buffer.pitch = BTBH_GPU_CANVAS_VERTEX_STRIDE;
    vertex_buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    attributes[0].location = 0u;
    attributes[0].buffer_slot = 0u;
    attributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    attributes[0].offset = 0u;
    attributes[1].location = 1u;
    attributes[1].buffer_slot = 0u;
    attributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    attributes[1].offset = 8u;
    attributes[2].location = 2u;
    attributes[2].buffer_slot = 0u;
    attributes[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    attributes[2].offset = 16u;
    color_target.format = SDL_GetGPUSwapchainTextureFormat(
        device,
        window->window
    );
    info.vertex_shader = vertex_shader;
    info.fragment_shader = fragment_shader;
    info.vertex_input_state.vertex_buffer_descriptions = &vertex_buffer;
    info.vertex_input_state.num_vertex_buffers = 1u;
    info.vertex_input_state.vertex_attributes = attributes;
    info.vertex_input_state.num_vertex_attributes = 3u;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.rasterizer_state.front_face =
        SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.target_info.color_target_descriptions = &color_target;
    info.target_info.num_color_targets = 1u;
    for (uint32_t index = 0u;
         index < BTBH_GPU_CANVAS_PIPELINE_COUNT;
         ++index) {
        btbh_gpu_canvas_blend_state(
            (BTBHGPUCanvasBlendMode)index,
            &color_target.blend_state
        );
        window->canvas_runtime->solid_pipelines[index] =
            SDL_CreateGPUGraphicsPipeline(device, &info);
        info.fragment_shader = textured_fragment_shader;
        window->canvas_runtime->textured_pipelines[index] =
            SDL_CreateGPUGraphicsPipeline(device, &info);
        info.fragment_shader = fragment_shader;
        if (
            window->canvas_runtime->solid_pipelines[index] == NULL ||
            window->canvas_runtime->textured_pipelines[index] == NULL
        ) {
            SDL_ReleaseGPUShader(
                device,
                textured_fragment_shader
            );
            SDL_ReleaseGPUShader(device, fragment_shader);
            SDL_ReleaseGPUShader(device, vertex_shader);
            btbh_gpu_canvas_error(
                report,
                BTBH_GPU_DEVICE_FAILURE,
                SDL_GetError()
            );
            btbh_gpu_window_release_canvas(window);
            return false;
        }
    }
    SDL_ReleaseGPUShader(device, textured_fragment_shader);
    SDL_ReleaseGPUShader(device, fragment_shader);
    SDL_ReleaseGPUShader(device, vertex_shader);
    return true;
}

static bool btbh_gpu_canvas_validate_batches(
    const BTBHGPUWindow *window,
    const BTBHGPUCanvasBatch *batches,
    uint32_t batch_count,
    uint32_t vertex_count
) {
    for (uint32_t index = 0u; index < batch_count; ++index) {
        const BTBHGPUCanvasBatch *batch = &batches[index];
        if (
            batch->vertex_count == 0u ||
            batch->first_vertex > vertex_count ||
            batch->vertex_count >
                vertex_count - batch->first_vertex ||
            batch->blend_mode < BTBH_GPU_CANVAS_BLEND_ALPHA ||
            batch->blend_mode >
                BTBH_GPU_CANVAS_BLEND_PREMULTIPLIED_ALPHA ||
            (batch->texture != NULL &&
             batch->texture->owner != window) ||
            (batch->scissor_enabled != 0u &&
             (batch->scissor_width <= 0 ||
              batch->scissor_height <= 0))
        ) {
            return false;
        }
    }
    return true;
}

bool btbh_gpu_window_finish_pending_frame(
    BTBHGPUWindow *window,
    bool submit
) {
    if (window == NULL || window->pending_command == NULL) {
        return false;
    }
    SDL_GPUCommandBuffer *command = window->pending_command;
    window->pending_command = NULL;
    window->pending_swapchain = NULL;
    window->pending_drawable_width = 0u;
    window->pending_drawable_height = 0u;
    const bool result = submit
        ? SDL_SubmitGPUCommandBuffer(command)
        : SDL_CancelGPUCommandBuffer(command);
    SDL_GPUDevice *device =
        window->device != NULL && !window->device->shutdown
            ? window->device->gpu
            : NULL;
    if (device != NULL) {
        if (window->pending_scene_transfer != NULL) {
            SDL_ReleaseGPUTransferBuffer(
                device,
                window->pending_scene_transfer
            );
        }
        if (window->pending_bone_buffer != NULL) {
            SDL_ReleaseGPUBuffer(
                device,
                window->pending_bone_buffer
            );
        }
        if (window->pending_instance_buffer != NULL) {
            SDL_ReleaseGPUBuffer(
                device,
                window->pending_instance_buffer
            );
        }
    }
    window->pending_scene_transfer = NULL;
    window->pending_bone_buffer = NULL;
    window->pending_instance_buffer = NULL;
    return result;
}

BTBHGPUResult btbh_gpu_window_canvas_present(
    BTBHGPUWindow *window,
    const char *vertex_shader_path,
    const char *fragment_shader_path,
    const char *textured_fragment_shader_path,
    BTBHGPUShaderFormat shader_format,
    const BTBHGPUCanvasVertex *vertices,
    uint32_t vertex_count,
    const BTBHGPUCanvasBatch *batches,
    uint32_t batch_count,
    uint8_t clear_red,
    uint8_t clear_green,
    uint8_t clear_blue,
    uint8_t clear_alpha,
    BTBHGPUCanvasReport *report
) {
    if (report == NULL) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    (void)memset(report, 0, sizeof(*report));
    if (
        window == NULL ||
        window->window == NULL ||
        window->device == NULL ||
        window->device->gpu == NULL ||
        !window->claimed ||
        vertex_shader_path == NULL ||
        fragment_shader_path == NULL ||
        textured_fragment_shader_path == NULL ||
        btbh_gpu_canvas_shader_format(shader_format) ==
            SDL_GPU_SHADERFORMAT_INVALID ||
        vertices == NULL ||
        vertex_count == 0u ||
        batches == NULL ||
        batch_count == 0u ||
        (uint64_t)vertex_count * sizeof(*vertices) > UINT32_MAX ||
        !btbh_gpu_canvas_validate_batches(
            window,
            batches,
            batch_count,
            vertex_count
        )
    ) {
        btbh_gpu_canvas_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            "Invalid GPU canvas submission"
        );
        return report->result;
    }
    if (
        window->canvas_runtime == NULL &&
        !btbh_gpu_canvas_create_runtime(
            window,
            vertex_shader_path,
            fragment_shader_path,
            textured_fragment_shader_path,
            shader_format,
            report
        )
    ) {
        return report->result;
    }

    SDL_GPUDevice *device = window->device->gpu;
    const bool had_scene = window->pending_command != NULL;
    if (!had_scene) {
        window->pending_command = SDL_AcquireGPUCommandBuffer(device);
        if (window->pending_command == NULL ||
            !SDL_WaitAndAcquireGPUSwapchainTexture(
                window->pending_command,
                window->window,
                &window->pending_swapchain,
                &window->pending_drawable_width,
                &window->pending_drawable_height
            ) ||
            window->pending_swapchain == NULL) {
            if (window->pending_command != NULL) {
                (void)btbh_gpu_window_finish_pending_frame(
                    window,
                    false
                );
            }
            btbh_gpu_canvas_error(
                report,
                BTBH_GPU_WINDOW_FAILURE,
                SDL_GetError()
            );
            return report->result;
        }
    }

    const uint32_t byte_count =
        vertex_count * (uint32_t)sizeof(*vertices);
    SDL_GPUBufferCreateInfo buffer_info;
    SDL_GPUTransferBufferCreateInfo transfer_info;
    SDL_zero(buffer_info);
    SDL_zero(transfer_info);
    buffer_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    buffer_info.size = byte_count;
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size = byte_count;
    SDL_GPUBuffer *vertex_buffer = SDL_CreateGPUBuffer(
        device,
        &buffer_info
    );
    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(
        device,
        &transfer_info
    );
    if (vertex_buffer == NULL || transfer == NULL) {
        if (transfer != NULL) {
            SDL_ReleaseGPUTransferBuffer(device, transfer);
        }
        if (vertex_buffer != NULL) {
            SDL_ReleaseGPUBuffer(device, vertex_buffer);
        }
        (void)btbh_gpu_window_finish_pending_frame(window, false);
        btbh_gpu_canvas_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return report->result;
    }
    void *mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
    if (mapped == NULL) {
        SDL_ReleaseGPUTransferBuffer(device, transfer);
        SDL_ReleaseGPUBuffer(device, vertex_buffer);
        (void)btbh_gpu_window_finish_pending_frame(window, false);
        btbh_gpu_canvas_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return report->result;
    }
    (void)memcpy(mapped, vertices, byte_count);
    SDL_UnmapGPUTransferBuffer(device, transfer);

    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(
        window->pending_command
    );
    if (copy == NULL) {
        SDL_ReleaseGPUTransferBuffer(device, transfer);
        SDL_ReleaseGPUBuffer(device, vertex_buffer);
        (void)btbh_gpu_window_finish_pending_frame(window, false);
        btbh_gpu_canvas_error(
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
    destination.buffer = vertex_buffer;
    destination.size = byte_count;
    SDL_UploadToGPUBuffer(copy, &source, &destination, false);
    SDL_EndGPUCopyPass(copy);

    SDL_GPUColorTargetInfo color_target;
    SDL_zero(color_target);
    color_target.texture = window->pending_swapchain;
    color_target.load_op = had_scene
        ? SDL_GPU_LOADOP_LOAD
        : SDL_GPU_LOADOP_CLEAR;
    color_target.store_op = SDL_GPU_STOREOP_STORE;
    color_target.clear_color.r = (float)clear_red / 255.0f;
    color_target.clear_color.g = (float)clear_green / 255.0f;
    color_target.clear_color.b = (float)clear_blue / 255.0f;
    color_target.clear_color.a = (float)clear_alpha / 255.0f;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(
        window->pending_command,
        &color_target,
        1u,
        NULL
    );
    if (pass == NULL) {
        SDL_ReleaseGPUTransferBuffer(device, transfer);
        SDL_ReleaseGPUBuffer(device, vertex_buffer);
        (void)btbh_gpu_window_finish_pending_frame(window, false);
        btbh_gpu_canvas_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return report->result;
    }
    const float uniforms[4] = {
        (float)window->pending_drawable_width,
        (float)window->pending_drawable_height,
        0.0f,
        0.0f
    };
    SDL_PushGPUVertexUniformData(
        window->pending_command,
        0u,
        uniforms,
        sizeof(uniforms)
    );
    SDL_GPUBufferBinding binding;
    SDL_zero(binding);
    binding.buffer = vertex_buffer;
    SDL_BindGPUVertexBuffers(pass, 0u, &binding, 1u);
    for (uint32_t index = 0u; index < batch_count; ++index) {
        const BTBHGPUCanvasBatch *batch = &batches[index];
        if (batch->texture != NULL) {
            SDL_BindGPUGraphicsPipeline(
                pass,
                window->canvas_runtime
                    ->textured_pipelines[batch->blend_mode]
            );
            SDL_GPUTextureSamplerBinding sampler;
            SDL_zero(sampler);
            sampler.texture = batch->texture->texture;
            sampler.sampler = batch->texture->sampler;
            SDL_BindGPUFragmentSamplers(
                pass,
                0u,
                &sampler,
                1u
            );
        } else {
            SDL_BindGPUGraphicsPipeline(
                pass,
                window->canvas_runtime
                    ->solid_pipelines[batch->blend_mode]
            );
        }
        SDL_Rect scissor;
        if (batch->scissor_enabled != 0u) {
            scissor.x = batch->scissor_x;
            scissor.y = batch->scissor_y;
            scissor.w = batch->scissor_width;
            scissor.h = batch->scissor_height;
        } else {
            scissor.x = 0;
            scissor.y = 0;
            scissor.w = (int)window->pending_drawable_width;
            scissor.h = (int)window->pending_drawable_height;
        }
        SDL_SetGPUScissor(pass, &scissor);
        SDL_DrawGPUPrimitives(
            pass,
            batch->vertex_count,
            1u,
            batch->first_vertex,
            0u
        );
    }
    SDL_EndGPURenderPass(pass);
    const uint32_t drawable_width =
        window->pending_drawable_width;
    const uint32_t drawable_height =
        window->pending_drawable_height;
    const bool submitted = btbh_gpu_window_finish_pending_frame(
        window,
        true
    );
    SDL_ReleaseGPUTransferBuffer(device, transfer);
    SDL_ReleaseGPUBuffer(device, vertex_buffer);
    if (!submitted) {
        btbh_gpu_canvas_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return report->result;
    }
    report->result = BTBH_GPU_OK;
    report->drawable_width = drawable_width;
    report->drawable_height = drawable_height;
    report->batch_count = batch_count;
    report->vertex_count = vertex_count;
    report->message[0] = '\0';
    return report->result;
}

void btbh_gpu_window_release_canvas(BTBHGPUWindow *window) {
    if (window == NULL || window->canvas_runtime == NULL) {
        return;
    }
    SDL_GPUDevice *device =
        window->device != NULL && !window->device->shutdown
            ? window->device->gpu
            : NULL;
    if (device != NULL) {
        for (uint32_t index = 0u;
             index < BTBH_GPU_CANVAS_PIPELINE_COUNT;
             ++index) {
            if (
                window->canvas_runtime->solid_pipelines[index] != NULL
            ) {
                SDL_ReleaseGPUGraphicsPipeline(
                    device,
                    window->canvas_runtime->solid_pipelines[index]
                );
            }
            if (
                window->canvas_runtime
                    ->textured_pipelines[index] != NULL
            ) {
                SDL_ReleaseGPUGraphicsPipeline(
                    device,
                    window->canvas_runtime
                        ->textured_pipelines[index]
                );
            }
        }
    }
    free(window->canvas_runtime);
    window->canvas_runtime = NULL;
}
