#include "btbh_gpu_internal.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BTBH_GPU_MODEL_VERTEX_STRIDE 96u
#define BTBH_GPU_MODEL_NORMAL_OFFSET 12u
#define BTBH_GPU_MODEL_TANGENT_OFFSET 32u
#define BTBH_GPU_MODEL_COLOR_OFFSET 48u
#define BTBH_GPU_MODEL_JOINTS_OFFSET 64u
#define BTBH_GPU_MODEL_WEIGHTS_OFFSET 80u
#define BTBH_GPU_SCENE_INSTANCE_FLOATS 24u
#define BTBH_GPU_DIRECTIONAL_SHADOW_MAP_SIZE 2048u

typedef struct BTBHGPUSceneUniforms {
    float view_projection[16];
    float camera_right[4];
    float camera_up[4];
    float camera_position[4];
    float material_color[4];
    float ambient_color_energy[4];
    float directional_direction_energy[4];
    float directional_color[4];
    float secondary_directional_direction_energy[4];
    float secondary_directional_color[4];
    uint32_t bone_offset;
    uint32_t billboard_mode;
    uint32_t unlit_enabled;
    uint32_t padding;
    float emission_color_energy[4];
} BTBHGPUSceneUniforms;

typedef struct BTBHGPUSceneFragmentUniforms {
    float camera_position[4];
    float ambient_color_energy[4];
    float directional_direction_energy[4];
    float directional_color[4];
    float secondary_directional_direction_energy[4];
    float secondary_directional_color[4];
    float material_factors[4];
    float emission_color_energy[4];
    uint32_t unlit_enabled;
    uint32_t padding[3];
    float irradiance_sh[9][4];
    float ground_bottom_energy[4];
    float ground_horizon_curve[4];
    float sky_top_energy[4];
    float sky_horizon_curve[4];
    float sun_color_energy[4];
    float sun_direction_size[4];
    float procedural_controls[4];
    float directional_shadow_view_projection[16];
    float secondary_directional_shadow_view_projection[16];
    float directional_shadow_controls[4];
    float secondary_directional_shadow_controls[4];
} BTBHGPUSceneFragmentUniforms;

typedef struct BTBHGPUShadowUniforms {
    float light_view_projection[16];
    float camera_right[4];
    float camera_up[4];
    uint32_t bone_offset;
    uint32_t billboard_mode;
    uint32_t padding[2];
} BTBHGPUShadowUniforms;

struct BTBHGPUSceneRuntime {
    SDL_GPUGraphicsPipeline *model_pipelines[
        BTBH_GPU_SCENE_BLEND_COUNT
    ][BTBH_GPU_SCENE_DEPTH_COUNT];
    SDL_GPUGraphicsPipeline *animated_model_pipelines[
        BTBH_GPU_SCENE_BLEND_COUNT
    ][BTBH_GPU_SCENE_DEPTH_COUNT];
    SDL_GPUGraphicsPipeline *unshadowed_model_pipelines[
        BTBH_GPU_SCENE_BLEND_COUNT
    ][BTBH_GPU_SCENE_DEPTH_COUNT];
    SDL_GPUGraphicsPipeline *unshadowed_animated_model_pipelines[
        BTBH_GPU_SCENE_BLEND_COUNT
    ][BTBH_GPU_SCENE_DEPTH_COUNT];
    SDL_GPUGraphicsPipeline *shadow_pipeline;
    SDL_GPUGraphicsPipeline *animated_shadow_pipeline;
    BTBHGPUTexture *fallback_texture;
    BTBHGPUTexture *normal_fallback_texture;
    SDL_GPUTexture *depth_texture;
    SDL_GPUTexture *directional_shadow_textures[2];
    SDL_GPUSampler *directional_shadow_sampler;
    SDL_GPUBuffer *instance_buffer;
    SDL_GPUBuffer *bone_buffer;
    SDL_GPUTransferBuffer *upload_transfer;
    uint32_t depth_width;
    uint32_t depth_height;
    uint32_t instance_capacity;
    uint32_t bone_capacity;
    uint32_t upload_capacity;
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
    uint32_t sampler_count,
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
    info.num_samplers = sampler_count;
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

static void btbh_gpu_scene_blend_state(
    BTBHGPUSceneBlendMode mode,
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
        case BTBH_GPU_SCENE_BLEND_ADDITIVE:
            state->src_color_blendfactor =
                SDL_GPU_BLENDFACTOR_SRC_ALPHA;
            state->dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            break;
        case BTBH_GPU_SCENE_BLEND_MULTIPLIED:
            state->src_color_blendfactor =
                SDL_GPU_BLENDFACTOR_DST_COLOR;
            state->dst_color_blendfactor =
                SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            break;
        case BTBH_GPU_SCENE_BLEND_PREMULTIPLIED_ALPHA:
            state->src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            state->dst_color_blendfactor =
                SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            break;
        case BTBH_GPU_SCENE_BLEND_ALPHA:
        default:
            state->src_color_blendfactor =
                SDL_GPU_BLENDFACTOR_SRC_ALPHA;
            state->dst_color_blendfactor =
                SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            break;
    }
}

static SDL_GPUCompareOp btbh_gpu_scene_depth_compare(
    BTBHGPUSceneDepthCompare compare
) {
    switch (compare) {
        case BTBH_GPU_SCENE_DEPTH_ALWAYS:
            return SDL_GPU_COMPAREOP_ALWAYS;
        case BTBH_GPU_SCENE_DEPTH_LESS_EQUAL:
            return SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
        case BTBH_GPU_SCENE_DEPTH_GREATER:
            return SDL_GPU_COMPAREOP_GREATER;
        case BTBH_GPU_SCENE_DEPTH_NOT_EQUAL:
            return SDL_GPU_COMPAREOP_NOT_EQUAL;
        case BTBH_GPU_SCENE_DEPTH_EQUAL:
            return SDL_GPU_COMPAREOP_EQUAL;
        case BTBH_GPU_SCENE_DEPTH_GREATER_EQUAL:
            return SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
        case BTBH_GPU_SCENE_DEPTH_NEVER:
            return SDL_GPU_COMPAREOP_NEVER;
        case BTBH_GPU_SCENE_DEPTH_LESS:
        default:
            return SDL_GPU_COMPAREOP_LESS;
    }
}

static bool btbh_gpu_scene_create_model_pipeline_set(
    BTBHGPUWindow *window,
    const char *vertex_shader_path,
    const char *fragment_shader_path,
    BTBHGPUShaderFormat project_format,
    bool animated,
    uint32_t fragment_sampler_count,
    SDL_GPUGraphicsPipeline *pipelines[
        BTBH_GPU_SCENE_BLEND_COUNT
    ][BTBH_GPU_SCENE_DEPTH_COUNT],
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
        animated ? 2u : 1u,
        0u,
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
        fragment_sampler_count,
        1u,
        report
    );
    if (fragment_shader == NULL) {
        SDL_ReleaseGPUShader(device, vertex_shader);
        return false;
    }

    SDL_GPUColorTargetDescription color_target;
    SDL_GPUVertexBufferDescription vertex_buffer;
    SDL_GPUVertexAttribute attributes[7];
    SDL_GPUGraphicsPipelineCreateInfo info;
    SDL_zero(color_target);
    SDL_zero(vertex_buffer);
    SDL_zeroa(attributes);
    SDL_zero(info);

    color_target.format = SDL_GetGPUSwapchainTextureFormat(
        device,
        window->window
    );
    color_target.blend_state.color_write_mask = 0x0fu;
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
    attributes[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    attributes[2].offset = 24u;
    attributes[3].location = 3u;
    attributes[3].buffer_slot = 0u;
    attributes[3].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    attributes[3].offset = BTBH_GPU_MODEL_NORMAL_OFFSET;
    attributes[4].location = 4u;
    attributes[4].buffer_slot = 0u;
    attributes[4].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    attributes[4].offset = animated
        ? BTBH_GPU_MODEL_JOINTS_OFFSET
        : BTBH_GPU_MODEL_TANGENT_OFFSET;
    if (animated) {
        attributes[5].location = 5u;
        attributes[5].buffer_slot = 0u;
        attributes[5].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
        attributes[5].offset = BTBH_GPU_MODEL_WEIGHTS_OFFSET;
        attributes[6].location = 6u;
        attributes[6].buffer_slot = 0u;
        attributes[6].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
        attributes[6].offset = BTBH_GPU_MODEL_TANGENT_OFFSET;
    }

    info.vertex_shader = vertex_shader;
    info.fragment_shader = fragment_shader;
    info.vertex_input_state.vertex_buffer_descriptions = &vertex_buffer;
    info.vertex_input_state.num_vertex_buffers = 1u;
    info.vertex_input_state.vertex_attributes = attributes;
    info.vertex_input_state.num_vertex_attributes = animated ? 7u : 5u;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.rasterizer_state.front_face =
        SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.depth_stencil_state.enable_depth_test = true;
    info.depth_stencil_state.enable_depth_write = true;
    info.target_info.color_target_descriptions = &color_target;
    info.target_info.num_color_targets = 1u;
    info.target_info.depth_stencil_format =
        SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    info.target_info.has_depth_stencil_target = true;

    for (uint32_t mode = 0u;
         mode < BTBH_GPU_SCENE_BLEND_COUNT;
         ++mode) {
        btbh_gpu_scene_blend_state(
            (BTBHGPUSceneBlendMode)mode,
            &color_target.blend_state
        );
        color_target.blend_state.color_write_mask = 0x0fu;
        for (uint32_t depth = 0u;
             depth < BTBH_GPU_SCENE_DEPTH_COUNT;
             ++depth) {
            info.depth_stencil_state.compare_op =
                btbh_gpu_scene_depth_compare(
                    (BTBHGPUSceneDepthCompare)depth
                );
            pipelines[mode][depth] =
                SDL_CreateGPUGraphicsPipeline(device, &info);
            if (pipelines[mode][depth] == NULL) {
                SDL_ReleaseGPUShader(device, fragment_shader);
                SDL_ReleaseGPUShader(device, vertex_shader);
                btbh_gpu_scene_error(
                    report,
                    BTBH_GPU_DEVICE_FAILURE,
                    SDL_GetError()
                );
                return false;
            }
        }
    }
    SDL_ReleaseGPUShader(device, fragment_shader);
    SDL_ReleaseGPUShader(device, vertex_shader);
    return true;
}

static bool btbh_gpu_scene_create_pipeline(
    BTBHGPUWindow *window,
    const char *vertex_shader_path,
    const char *animated_vertex_shader_path,
    const char *shadow_vertex_shader_path,
    const char *animated_shadow_vertex_shader_path,
    const char *shadow_fragment_shader_path,
    const char *fragment_shader_path,
    const char *unshadowed_fragment_shader_path,
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
    if (!btbh_gpu_scene_create_model_pipeline_set(
            window,
            vertex_shader_path,
            fragment_shader_path,
            project_format,
            false,
            6u,
            window->scene_runtime->model_pipelines,
            report
        ) ||
        !btbh_gpu_scene_create_model_pipeline_set(
            window,
            animated_vertex_shader_path,
            fragment_shader_path,
            project_format,
            true,
            6u,
            window->scene_runtime->animated_model_pipelines,
            report
        ) ||
        !btbh_gpu_scene_create_model_pipeline_set(
            window,
            vertex_shader_path,
            unshadowed_fragment_shader_path,
            project_format,
            false,
            4u,
            window->scene_runtime->unshadowed_model_pipelines,
            report
        ) ||
        !btbh_gpu_scene_create_model_pipeline_set(
            window,
            animated_vertex_shader_path,
            unshadowed_fragment_shader_path,
            project_format,
            true,
            4u,
            window->scene_runtime
                ->unshadowed_animated_model_pipelines,
            report
        )) {
        return false;
    }
    SDL_GPUShader *shadow_vertex_shader = btbh_gpu_scene_load_shader(
        device,
        shadow_vertex_shader_path,
        entrypoint,
        format,
        SDL_GPU_SHADERSTAGE_VERTEX,
        1u,
        0u,
        1u,
        report
    );
    if (shadow_vertex_shader == NULL) {
        return false;
    }
    SDL_GPUShader *animated_shadow_vertex_shader =
        btbh_gpu_scene_load_shader(
            device,
            animated_shadow_vertex_shader_path,
            entrypoint,
            format,
            SDL_GPU_SHADERSTAGE_VERTEX,
            2u,
            0u,
            1u,
            report
        );
    if (animated_shadow_vertex_shader == NULL) {
        SDL_ReleaseGPUShader(device, shadow_vertex_shader);
        return false;
    }
    SDL_GPUShader *shadow_fragment_shader =
        btbh_gpu_scene_load_shader(
            device,
            shadow_fragment_shader_path,
            entrypoint,
            format,
            SDL_GPU_SHADERSTAGE_FRAGMENT,
            0u,
            0u,
            0u,
            report
        );
    if (shadow_fragment_shader == NULL) {
        SDL_ReleaseGPUShader(
            device,
            animated_shadow_vertex_shader
        );
        SDL_ReleaseGPUShader(device, shadow_vertex_shader);
        return false;
    }

    SDL_GPUVertexBufferDescription vertex_buffer;
    SDL_GPUVertexAttribute shadow_attributes[3];
    SDL_GPUGraphicsPipelineCreateInfo info;
    SDL_zero(vertex_buffer);
    SDL_zeroa(shadow_attributes);
    SDL_zero(info);
    vertex_buffer.slot = 0u;
    vertex_buffer.pitch = BTBH_GPU_MODEL_VERTEX_STRIDE;
    vertex_buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    vertex_buffer.instance_step_rate = 0u;
    shadow_attributes[0].location = 0u;
    shadow_attributes[0].buffer_slot = 0u;
    shadow_attributes[0].format =
        SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    shadow_attributes[0].offset = 0u;
    shadow_attributes[1].location = 4u;
    shadow_attributes[1].buffer_slot = 0u;
    shadow_attributes[1].format =
        SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    shadow_attributes[1].offset = BTBH_GPU_MODEL_JOINTS_OFFSET;
    shadow_attributes[2].location = 5u;
    shadow_attributes[2].buffer_slot = 0u;
    shadow_attributes[2].format =
        SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    shadow_attributes[2].offset = BTBH_GPU_MODEL_WEIGHTS_OFFSET;
    info.vertex_shader = shadow_vertex_shader;
    info.fragment_shader = shadow_fragment_shader;
    info.vertex_input_state.vertex_buffer_descriptions = &vertex_buffer;
    info.vertex_input_state.num_vertex_buffers = 1u;
    info.vertex_input_state.vertex_attributes = shadow_attributes;
    info.vertex_input_state.num_vertex_attributes = 1u;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.rasterizer_state.front_face =
        SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
    info.depth_stencil_state.enable_depth_test = true;
    info.depth_stencil_state.enable_depth_write = true;
    info.target_info.num_color_targets = 0u;
    info.target_info.depth_stencil_format =
        SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    info.target_info.has_depth_stencil_target = true;
    window->scene_runtime->shadow_pipeline =
        SDL_CreateGPUGraphicsPipeline(device, &info);
    info.vertex_shader = animated_shadow_vertex_shader;
    info.vertex_input_state.num_vertex_attributes = 3u;
    window->scene_runtime->animated_shadow_pipeline =
        SDL_CreateGPUGraphicsPipeline(device, &info);
    SDL_ReleaseGPUShader(device, animated_shadow_vertex_shader);
    SDL_ReleaseGPUShader(device, shadow_vertex_shader);
    SDL_ReleaseGPUShader(device, shadow_fragment_shader);
    if (window->scene_runtime->shadow_pipeline == NULL ||
        window->scene_runtime->animated_shadow_pipeline == NULL) {
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
    const char *shadow_vertex_shader_path,
    const char *animated_shadow_vertex_shader_path,
    const char *shadow_fragment_shader_path,
    const char *fragment_shader_path,
    const char *unshadowed_fragment_shader_path,
    BTBHGPUShaderFormat shader_format,
    BTBHGPUSceneReport *report
) {
    if (window->scene_runtime != NULL) {
        return true;
    }
    SDL_GPUDevice *device = window->device->gpu;
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
            shadow_vertex_shader_path,
            animated_shadow_vertex_shader_path,
            shadow_fragment_shader_path,
            fragment_shader_path,
            unshadowed_fragment_shader_path,
            shader_format,
            report
        )) {
        btbh_gpu_window_release_scene(window);
        return false;
    }
    const uint8_t white_pixel[4] = {255u, 255u, 255u, 255u};
    BTBHGPUTextureReport texture_report;
    window->scene_runtime->fallback_texture =
        btbh_gpu_window_texture_create_rgba8(
            window,
            white_pixel,
            1u,
            1u,
            BTBH_GPU_TEXTURE_FILTER_NEAREST,
            BTBH_GPU_TEXTURE_WRAP_CLAMP,
            &texture_report
        );
    if (window->scene_runtime->fallback_texture == NULL) {
        btbh_gpu_scene_error(
            report,
            texture_report.result,
            texture_report.message
        );
        btbh_gpu_window_release_scene(window);
        return false;
    }
    const uint8_t normal_pixel[4] = {128u, 128u, 255u, 255u};
    window->scene_runtime->normal_fallback_texture =
        btbh_gpu_window_texture_create_rgba8(
            window,
            normal_pixel,
            1u,
            1u,
            BTBH_GPU_TEXTURE_FILTER_NEAREST,
            BTBH_GPU_TEXTURE_WRAP_CLAMP,
            &texture_report
        );
    if (window->scene_runtime->normal_fallback_texture == NULL) {
        btbh_gpu_scene_error(
            report,
            texture_report.result,
            texture_report.message
        );
        btbh_gpu_window_release_scene(window);
        return false;
    }
    SDL_GPUTextureCreateInfo shadow_info;
    SDL_zero(shadow_info);
    shadow_info.type = SDL_GPU_TEXTURETYPE_2D;
    shadow_info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    shadow_info.usage =
        SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET |
        SDL_GPU_TEXTUREUSAGE_SAMPLER;
    shadow_info.width = BTBH_GPU_DIRECTIONAL_SHADOW_MAP_SIZE;
    shadow_info.height = BTBH_GPU_DIRECTIONAL_SHADOW_MAP_SIZE;
    shadow_info.layer_count_or_depth = 1u;
    shadow_info.num_levels = 1u;
    shadow_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    for (uint32_t index = 0u; index < 2u; ++index) {
        window->scene_runtime->directional_shadow_textures[index] =
            SDL_CreateGPUTexture(device, &shadow_info);
        if (window->scene_runtime
                ->directional_shadow_textures[index] == NULL) {
            btbh_gpu_scene_error(
                report,
                BTBH_GPU_DEVICE_FAILURE,
                SDL_GetError()
            );
            btbh_gpu_window_release_scene(window);
            return false;
        }
    }
    SDL_GPUSamplerCreateInfo sampler_info;
    SDL_zero(sampler_info);
    sampler_info.min_filter = SDL_GPU_FILTER_LINEAR;
    sampler_info.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampler_info.mipmap_mode =
        SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler_info.address_mode_u =
        SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_v =
        SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_w =
        SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    window->scene_runtime->directional_shadow_sampler =
        SDL_CreateGPUSampler(device, &sampler_info);
    if (window->scene_runtime->directional_shadow_sampler == NULL) {
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
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

static uint32_t btbh_gpu_scene_buffer_capacity(
    uint32_t required
) {
    uint32_t capacity = 256u;
    while (capacity < required && capacity <= UINT32_MAX / 2u) {
        capacity *= 2u;
    }
    return capacity >= required ? capacity : required;
}

static bool btbh_gpu_scene_ensure_upload_buffers(
    BTBHGPUWindow *window,
    uint32_t instance_bytes,
    uint32_t bone_bytes,
    uint32_t transfer_bytes
) {
    BTBHGPUSceneRuntime *runtime = window->scene_runtime;
    SDL_GPUDevice *device = window->device->gpu;
    SDL_GPUBufferCreateInfo buffer_info;
    SDL_GPUTransferBufferCreateInfo transfer_info;
    SDL_zero(buffer_info);
    SDL_zero(transfer_info);
    buffer_info.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    if (
        instance_bytes > 0u &&
        (runtime->instance_buffer == NULL ||
         runtime->instance_capacity < instance_bytes)
    ) {
        if (runtime->instance_buffer != NULL) {
            SDL_ReleaseGPUBuffer(device, runtime->instance_buffer);
        }
        runtime->instance_capacity =
            btbh_gpu_scene_buffer_capacity(instance_bytes);
        buffer_info.size = runtime->instance_capacity;
        runtime->instance_buffer =
            SDL_CreateGPUBuffer(device, &buffer_info);
        if (runtime->instance_buffer == NULL) {
            runtime->instance_capacity = 0u;
            return false;
        }
    }
    if (
        bone_bytes > 0u &&
        (
            runtime->bone_buffer == NULL ||
            runtime->bone_capacity < bone_bytes
        )
    ) {
        if (runtime->bone_buffer != NULL) {
            SDL_ReleaseGPUBuffer(device, runtime->bone_buffer);
        }
        runtime->bone_capacity =
            btbh_gpu_scene_buffer_capacity(bone_bytes);
        buffer_info.size = runtime->bone_capacity;
        runtime->bone_buffer = SDL_CreateGPUBuffer(
            device,
            &buffer_info
        );
        if (runtime->bone_buffer == NULL) {
            runtime->bone_capacity = 0u;
            return false;
        }
    }
    if (
        transfer_bytes > 0u &&
        (runtime->upload_transfer == NULL ||
         runtime->upload_capacity < transfer_bytes)
    ) {
        if (runtime->upload_transfer != NULL) {
            SDL_ReleaseGPUTransferBuffer(
                device,
                runtime->upload_transfer
            );
        }
        runtime->upload_capacity =
            btbh_gpu_scene_buffer_capacity(transfer_bytes);
        transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transfer_info.size = runtime->upload_capacity;
        runtime->upload_transfer =
            SDL_CreateGPUTransferBuffer(device, &transfer_info);
        if (runtime->upload_transfer == NULL) {
            runtime->upload_capacity = 0u;
            return false;
        }
    }
    return true;
}

static bool btbh_gpu_scene_validate_batches(
    BTBHGPUWindow *window,
    uint32_t transient_instance_count,
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
        const bool valid_material =
            batch->blend_mode >= BTBH_GPU_SCENE_BLEND_ALPHA &&
            batch->blend_mode < BTBH_GPU_SCENE_BLEND_COUNT &&
            batch->billboard_mode >=
                BTBH_GPU_SCENE_BILLBOARD_DISABLED &&
            batch->billboard_mode <= BTBH_GPU_SCENE_BILLBOARD_Y_AXIS &&
            batch->depth_compare >= BTBH_GPU_SCENE_DEPTH_ALWAYS &&
            batch->depth_compare < BTBH_GPU_SCENE_DEPTH_COUNT &&
            batch->unlit_enabled <= 1u &&
            isfinite(batch->emission_energy) &&
            isfinite(batch->normal_scale) &&
            isfinite(batch->occlusion) &&
            isfinite(batch->roughness) &&
            isfinite(batch->metalness) &&
            (batch->albedo_texture == NULL ||
             (batch->albedo_texture->owner == window &&
              batch->albedo_texture->texture != NULL &&
              batch->albedo_texture->sampler != NULL)) &&
            (batch->emission_texture == NULL ||
             (batch->emission_texture->owner == window &&
              batch->emission_texture->texture != NULL &&
              batch->emission_texture->sampler != NULL)) &&
            (batch->normal_texture == NULL ||
             (batch->normal_texture->owner == window &&
              batch->normal_texture->texture != NULL &&
              batch->normal_texture->sampler != NULL)) &&
            (batch->orm_texture == NULL ||
             (batch->orm_texture->owner == window &&
              batch->orm_texture->texture != NULL &&
              batch->orm_texture->sampler != NULL));
        const bool valid_material_slot =
            (batch->kind == BTBH_GPU_SCENE_GEOMETRY_MODEL &&
                batch->model != NULL &&
                (batch->material_slot < 0 ||
                 (uint32_t)batch->material_slot <
                    batch->model->material_count)) ||
            (batch->kind == BTBH_GPU_SCENE_GEOMETRY_CUSTOM_MESH &&
                batch->material_slot < 0);
        const bool uses_persistent_instances =
            batch->instance_buffer != NULL;
        const bool valid_instance_buffer =
            !uses_persistent_instances ||
            (batch->instance_buffer->owner == window &&
             batch->instance_buffer->buffer != NULL &&
             batch->instance_buffer->stride ==
                BTBH_GPU_SCENE_INSTANCE_FLOATS * sizeof(float));
        const uint32_t available_instance_count =
            uses_persistent_instances && valid_instance_buffer
                ? batch->instance_buffer->uploaded_prefix_byte_count /
                    batch->instance_buffer->stride
                : transient_instance_count;
        if (!valid_geometry ||
            !valid_animation ||
            !valid_material ||
            !valid_material_slot ||
            !valid_instance_buffer ||
            batch->instance_count == 0u ||
            batch->first_instance > available_instance_count ||
            batch->instance_count >
                available_instance_count - batch->first_instance) {
            return false;
        }
    }
    return true;
}

static bool btbh_gpu_scene_batch_uses_geometry(
    const BTBHGPUSceneGeometryBatch *batch,
    uint32_t geometry_index
) {
    return
        batch->kind != BTBH_GPU_SCENE_GEOMETRY_MODEL ||
        batch->material_slot < 0 ||
        batch->model->primitives[geometry_index].material_slot ==
            batch->material_slot;
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
        for (uint32_t primitive_index = 0u;
             primitive_index < batch_geometry_count;
             primitive_index += 1u) {
            if (!btbh_gpu_scene_batch_uses_geometry(
                    batch,
                    primitive_index
                )) {
                continue;
            }
            if (total_geometry_count == UINT32_MAX) {
                return false;
            }
            total_geometry_count += 1u;
            if (batch->animation_library == NULL) {
                continue;
            }
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
            if (!btbh_gpu_scene_batch_uses_geometry(
                    batch,
                    primitive_index
                )) {
                continue;
            }
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

static bool btbh_gpu_scene_lighting_valid(
    const BTBHGPUSceneLighting *lighting
) {
    if (lighting == NULL ||
        !isfinite(lighting->ambient_red) ||
        !isfinite(lighting->ambient_green) ||
        !isfinite(lighting->ambient_blue) ||
        !isfinite(lighting->ambient_energy) ||
        lighting->ambient_energy < 0.0f ||
        !isfinite(lighting->directional_x) ||
        !isfinite(lighting->directional_y) ||
        !isfinite(lighting->directional_z) ||
        !isfinite(lighting->directional_energy) ||
        lighting->directional_energy < 0.0f ||
        !isfinite(lighting->directional_red) ||
        !isfinite(lighting->directional_green) ||
        !isfinite(lighting->directional_blue) ||
        !isfinite(lighting->secondary_directional_x) ||
        !isfinite(lighting->secondary_directional_y) ||
        !isfinite(lighting->secondary_directional_z) ||
        !isfinite(lighting->secondary_directional_energy) ||
        lighting->secondary_directional_energy < 0.0f ||
        !isfinite(lighting->secondary_directional_red) ||
        !isfinite(lighting->secondary_directional_green) ||
        !isfinite(lighting->secondary_directional_blue) ||
        lighting->ibl_enabled > 1u ||
        lighting->directional_shadow_enabled > 1u ||
        !isfinite(lighting->directional_shadow_softness) ||
        lighting->directional_shadow_softness < 0.0f ||
        lighting->secondary_directional_shadow_enabled > 1u ||
        !isfinite(lighting->secondary_directional_shadow_softness) ||
        lighting->secondary_directional_shadow_softness < 0.0f) {
        return false;
    }
    for (size_t index = 0u; index < 16u; index += 1u) {
        if (!isfinite(
                lighting->directional_shadow_view_projection[index]
            ) ||
            !isfinite(
                lighting
                    ->secondary_directional_shadow_view_projection[index]
            )) {
            return false;
        }
    }
    for (size_t index = 0u; index < 27u; index += 1u) {
        if (!isfinite(lighting->irradiance_sh[index])) {
            return false;
        }
    }
    const float procedural_values[] = {
        lighting->ground_bottom_red,
        lighting->ground_bottom_green,
        lighting->ground_bottom_blue,
        lighting->ground_energy,
        lighting->ground_horizon_red,
        lighting->ground_horizon_green,
        lighting->ground_horizon_blue,
        lighting->ground_horizon_curve,
        lighting->sky_top_red,
        lighting->sky_top_green,
        lighting->sky_top_blue,
        lighting->sky_energy,
        lighting->sky_horizon_red,
        lighting->sky_horizon_green,
        lighting->sky_horizon_blue,
        lighting->sky_horizon_curve,
        lighting->sun_red,
        lighting->sun_green,
        lighting->sun_blue,
        lighting->sun_energy,
        lighting->sun_direction_x,
        lighting->sun_direction_y,
        lighting->sun_direction_z,
        lighting->sun_size_radians,
        lighting->sun_curve
    };
    for (size_t index = 0u;
         index < sizeof(procedural_values) / sizeof(procedural_values[0]);
         index += 1u) {
        if (!isfinite(procedural_values[index])) {
            return false;
        }
    }
    return true;
}

static bool btbh_gpu_scene_render_directional_shadow(
    SDL_GPUCommandBuffer *command,
    BTBHGPUSceneRuntime *runtime,
    SDL_GPUTexture *shadow_texture,
    const float light_view_projection[16],
    const float camera_right[3],
    const float camera_up[3],
    SDL_GPUBuffer *transient_instance_buffer,
    SDL_GPUBuffer *bone_buffer,
    const uint32_t *bone_offsets,
    const BTBHGPUSceneGeometryBatch *batches,
    uint32_t batch_count
) {
    SDL_GPUDepthStencilTargetInfo depth_target;
    SDL_zero(depth_target);
    depth_target.texture = shadow_texture;
    depth_target.clear_depth = 1.0f;
    depth_target.load_op = SDL_GPU_LOADOP_CLEAR;
    depth_target.store_op = SDL_GPU_STOREOP_STORE;
    depth_target.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth_target.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth_target.cycle = true;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(
        command,
        NULL,
        0u,
        &depth_target
    );
    if (pass == NULL) {
        return false;
    }

    uint32_t geometry_cursor = 0u;
    for (uint32_t batch_index = 0u;
         batch_index < batch_count;
         ++batch_index) {
        const BTBHGPUSceneGeometryBatch *batch = &batches[batch_index];
        SDL_GPUBuffer *batch_instance_buffer =
            batch->instance_buffer != NULL
                ? batch->instance_buffer->buffer
                : transient_instance_buffer;
        const uint32_t geometry_count =
            batch->kind == BTBH_GPU_SCENE_GEOMETRY_MODEL
                ? batch->model->primitive_count
                : 1u;
        for (uint32_t geometry_index = 0u;
             geometry_index < geometry_count;
             ++geometry_index) {
            if (!btbh_gpu_scene_batch_uses_geometry(
                    batch,
                    geometry_index
                )) {
                continue;
            }
            const uint32_t bone_offset =
                bone_offsets[geometry_cursor];
            geometry_cursor += 1u;
            if (batch->transparency_enabled != 0u) {
                continue;
            }
            const bool animated = bone_offset != UINT32_MAX;
            BTBHGPUShadowUniforms uniforms;
            (void)memset(&uniforms, 0, sizeof(uniforms));
            (void)memcpy(
                uniforms.light_view_projection,
                light_view_projection,
                sizeof(uniforms.light_view_projection)
            );
            (void)memcpy(
                uniforms.camera_right,
                camera_right,
                sizeof(float) * 3u
            );
            (void)memcpy(
                uniforms.camera_up,
                camera_up,
                sizeof(float) * 3u
            );
            uniforms.bone_offset = animated ? bone_offset : 0u;
            uniforms.billboard_mode =
                (uint32_t)batch->billboard_mode;
            SDL_PushGPUVertexUniformData(
                command,
                0u,
                &uniforms,
                sizeof(uniforms)
            );
            if (animated) {
                SDL_BindGPUGraphicsPipeline(
                    pass,
                    runtime->animated_shadow_pipeline
                );
                SDL_GPUBuffer *storage_buffers[2] = {
                    batch_instance_buffer,
                    bone_buffer
                };
                SDL_BindGPUVertexStorageBuffers(
                    pass,
                    0u,
                    storage_buffers,
                    2u
                );
            } else {
                SDL_BindGPUGraphicsPipeline(
                    pass,
                    runtime->shadow_pipeline
                );
                SDL_BindGPUVertexStorageBuffers(
                    pass,
                    0u,
                    &batch_instance_buffer,
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
        }
    }
    SDL_EndGPURenderPass(pass);
    return true;
}

BTBHGPUResult btbh_gpu_window_scene_present_instances(
    BTBHGPUWindow *window,
    const char *vertex_shader_path,
    const char *animated_vertex_shader_path,
    const char *shadow_vertex_shader_path,
    const char *animated_shadow_vertex_shader_path,
    const char *shadow_fragment_shader_path,
    const char *fragment_shader_path,
    const char *unshadowed_fragment_shader_path,
    BTBHGPUShaderFormat shader_format,
    const float view_projection[16],
    const float camera_right[3],
    const float camera_up[3],
    const float camera_position[3],
    const BTBHGPUSceneLighting *lighting,
    const float *instances,
    uint32_t transient_instance_count,
    uint32_t logical_instance_count,
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
        shadow_vertex_shader_path == NULL ||
        animated_shadow_vertex_shader_path == NULL ||
        shadow_fragment_shader_path == NULL ||
        fragment_shader_path == NULL ||
        unshadowed_fragment_shader_path == NULL ||
        view_projection == NULL ||
        camera_right == NULL ||
        camera_up == NULL ||
        camera_position == NULL ||
        !isfinite(camera_position[0]) ||
        !isfinite(camera_position[1]) ||
        !isfinite(camera_position[2]) ||
        !btbh_gpu_scene_lighting_valid(lighting) ||
        (instances == NULL && transient_instance_count > 0u) ||
        logical_instance_count == 0u ||
        batches == NULL ||
        batch_count == 0u ||
        btbh_gpu_scene_shader_format(shader_format) ==
            SDL_GPU_SHADERFORMAT_INVALID ||
        (uint64_t)transient_instance_count *
            BTBH_GPU_SCENE_INSTANCE_FLOATS * sizeof(float) > UINT32_MAX ||
        !btbh_gpu_scene_validate_batches(
            window,
            transient_instance_count,
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
            shadow_vertex_shader_path,
            animated_shadow_vertex_shader_path,
            shadow_fragment_shader_path,
            fragment_shader_path,
            unshadowed_fragment_shader_path,
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
        transient_instance_count *
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
    if (!btbh_gpu_scene_ensure_upload_buffers(
            window,
            instance_bytes,
            bone_bytes,
            transfer_bytes
        )) {
        free(bone_matrices);
        free(bone_offsets);
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return report->result;
    }
    BTBHGPUSceneRuntime *runtime = window->scene_runtime;
    SDL_GPUBuffer *transient_instance_buffer = instance_bytes > 0u
        ? runtime->instance_buffer
        : NULL;
    SDL_GPUBuffer *bone_buffer = bone_bytes > 0u
        ? runtime->bone_buffer
        : NULL;
    SDL_GPUTransferBuffer *transfer = runtime->upload_transfer;
    if (transfer_bytes > 0u) {
        void *mapped = SDL_MapGPUTransferBuffer(device, transfer, true);
        if (mapped == NULL) {
            free(bone_matrices);
            free(bone_offsets);
            btbh_gpu_scene_error(
                report,
                BTBH_GPU_DEVICE_FAILURE,
                SDL_GetError()
            );
            return report->result;
        }
        if (instance_bytes > 0u) {
            (void)memcpy(mapped, instances, instance_bytes);
        }
        if (bone_bytes > 0u) {
            (void)memcpy(
                (uint8_t *)mapped + instance_bytes,
                bone_matrices,
                bone_bytes
            );
        }
        SDL_UnmapGPUTransferBuffer(device, transfer);
    }
    free(bone_matrices);
    bone_matrices = NULL;

    SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(device);
    if (command == NULL) {
        free(bone_offsets);
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return report->result;
    }
    if (transfer_bytes > 0u) {
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command);
        if (copy == NULL) {
            (void)SDL_CancelGPUCommandBuffer(command);
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
        if (transient_instance_buffer != NULL) {
            destination.buffer = transient_instance_buffer;
            destination.size = instance_bytes;
            SDL_UploadToGPUBuffer(copy, &source, &destination, true);
        }
        if (bone_buffer != NULL) {
            source.offset = instance_bytes;
            destination.buffer = bone_buffer;
            destination.size = bone_bytes;
            SDL_UploadToGPUBuffer(copy, &source, &destination, true);
        }
        SDL_EndGPUCopyPass(copy);
    }

    const uint32_t shadow_enabled[2] = {
        lighting->directional_shadow_enabled,
        lighting->secondary_directional_shadow_enabled
    };
    const bool shadows_active =
        shadow_enabled[0] != 0u || shadow_enabled[1] != 0u;
    const float *shadow_matrices[2] = {
        lighting->directional_shadow_view_projection,
        lighting->secondary_directional_shadow_view_projection
    };
    for (uint32_t shadow_index = 0u;
         shadow_index < 2u;
         ++shadow_index) {
        if (shadow_enabled[shadow_index] == 0u) {
            continue;
        }
        if (!btbh_gpu_scene_render_directional_shadow(
                command,
                runtime,
                runtime->directional_shadow_textures[shadow_index],
                shadow_matrices[shadow_index],
                camera_right,
                camera_up,
                transient_instance_buffer,
                bone_buffer,
                bone_offsets,
                batches,
                batch_count
            )) {
            (void)SDL_CancelGPUCommandBuffer(command);
            free(bone_offsets);
            btbh_gpu_scene_error(
                report,
                BTBH_GPU_DEVICE_FAILURE,
                SDL_GetError()
            );
            return report->result;
        }
    }

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
        free(bone_offsets);
        btbh_gpu_scene_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return report->result;
    }
    uint32_t geometry_cursor = 0u;
    uint32_t primitive_draw_count = 0u;
    for (uint32_t batch_index = 0u;
         batch_index < batch_count;
         ++batch_index) {
        const BTBHGPUSceneGeometryBatch *batch = &batches[batch_index];
        SDL_GPUBuffer *batch_instance_buffer =
            batch->instance_buffer != NULL
                ? batch->instance_buffer->buffer
                : transient_instance_buffer;
        const uint32_t geometry_count =
            batch->kind == BTBH_GPU_SCENE_GEOMETRY_MODEL
                ? batch->model->primitive_count
                : 1u;
        for (uint32_t geometry_index = 0u;
             geometry_index < geometry_count;
            ++geometry_index) {
            if (!btbh_gpu_scene_batch_uses_geometry(
                    batch,
                    geometry_index
                )) {
                continue;
            }
            const uint32_t bone_offset =
                bone_offsets[geometry_cursor];
            geometry_cursor += 1u;
            const bool animated = bone_offset != UINT32_MAX;
            BTBHGPUSceneUniforms uniforms;
            (void)memset(&uniforms, 0, sizeof(uniforms));
            (void)memcpy(
                uniforms.view_projection,
                view_projection,
                sizeof(uniforms.view_projection)
            );
            (void)memcpy(
                uniforms.camera_right,
                camera_right,
                sizeof(float) * 3u
            );
            (void)memcpy(
                uniforms.camera_up,
                camera_up,
                sizeof(float) * 3u
            );
            (void)memcpy(
                uniforms.camera_position,
                camera_position,
                sizeof(float) * 3u
            );
            uniforms.material_color[0] =
                (float)batch->albedo_red / 255.0f;
            uniforms.material_color[1] =
                (float)batch->albedo_green / 255.0f;
            uniforms.material_color[2] =
                (float)batch->albedo_blue / 255.0f;
            uniforms.material_color[3] =
                (float)batch->albedo_alpha / 255.0f;
            uniforms.ambient_color_energy[0] =
                lighting->ambient_red;
            uniforms.ambient_color_energy[1] =
                lighting->ambient_green;
            uniforms.ambient_color_energy[2] =
                lighting->ambient_blue;
            uniforms.ambient_color_energy[3] =
                lighting->ambient_energy;
            uniforms.directional_direction_energy[0] =
                lighting->directional_x;
            uniforms.directional_direction_energy[1] =
                lighting->directional_y;
            uniforms.directional_direction_energy[2] =
                lighting->directional_z;
            uniforms.directional_direction_energy[3] =
                lighting->directional_energy;
            uniforms.directional_color[0] =
                lighting->directional_red;
            uniforms.directional_color[1] =
                lighting->directional_green;
            uniforms.directional_color[2] =
                lighting->directional_blue;
            uniforms.directional_color[3] = 1.0f;
            uniforms.secondary_directional_direction_energy[0] =
                lighting->secondary_directional_x;
            uniforms.secondary_directional_direction_energy[1] =
                lighting->secondary_directional_y;
            uniforms.secondary_directional_direction_energy[2] =
                lighting->secondary_directional_z;
            uniforms.secondary_directional_direction_energy[3] =
                lighting->secondary_directional_energy;
            uniforms.secondary_directional_color[0] =
                lighting->secondary_directional_red;
            uniforms.secondary_directional_color[1] =
                lighting->secondary_directional_green;
            uniforms.secondary_directional_color[2] =
                lighting->secondary_directional_blue;
            uniforms.secondary_directional_color[3] = 1.0f;
            uniforms.bone_offset = animated ? bone_offset : 0u;
            uniforms.billboard_mode =
                (uint32_t)batch->billboard_mode;
            uniforms.unlit_enabled =
                (uint32_t)batch->unlit_enabled;
            uniforms.emission_color_energy[0] =
                (float)batch->emission_red / 255.0f;
            uniforms.emission_color_energy[1] =
                (float)batch->emission_green / 255.0f;
            uniforms.emission_color_energy[2] =
                (float)batch->emission_blue / 255.0f;
            uniforms.emission_color_energy[3] =
                batch->unlit_enabled != 0u
                    ? 0.0f
                    : batch->emission_energy;
            SDL_PushGPUVertexUniformData(
                command,
                0u,
                &uniforms,
                sizeof(uniforms)
            );
            BTBHGPUSceneFragmentUniforms fragment_uniforms;
            (void)memset(
                &fragment_uniforms,
                0,
                sizeof(fragment_uniforms)
            );
            (void)memcpy(
                fragment_uniforms.camera_position,
                camera_position,
                sizeof(float) * 3u
            );
            (void)memcpy(
                fragment_uniforms.ambient_color_energy,
                uniforms.ambient_color_energy,
                sizeof(fragment_uniforms.ambient_color_energy)
            );
            (void)memcpy(
                fragment_uniforms.directional_direction_energy,
                uniforms.directional_direction_energy,
                sizeof(fragment_uniforms.directional_direction_energy)
            );
            (void)memcpy(
                fragment_uniforms.directional_color,
                uniforms.directional_color,
                sizeof(fragment_uniforms.directional_color)
            );
            (void)memcpy(
                fragment_uniforms
                    .secondary_directional_direction_energy,
                uniforms.secondary_directional_direction_energy,
                sizeof(
                    fragment_uniforms
                        .secondary_directional_direction_energy
                )
            );
            (void)memcpy(
                fragment_uniforms.secondary_directional_color,
                uniforms.secondary_directional_color,
                sizeof(fragment_uniforms.secondary_directional_color)
            );
            fragment_uniforms.material_factors[0] =
                batch->normal_scale;
            fragment_uniforms.material_factors[1] =
                batch->occlusion;
            fragment_uniforms.material_factors[2] =
                batch->roughness;
            fragment_uniforms.material_factors[3] =
                batch->metalness;
            (void)memcpy(
                fragment_uniforms.emission_color_energy,
                uniforms.emission_color_energy,
                sizeof(fragment_uniforms.emission_color_energy)
            );
            fragment_uniforms.unlit_enabled =
                uniforms.unlit_enabled;
            for (size_t coefficient = 0u;
                 coefficient < 9u;
                 coefficient += 1u) {
                for (size_t channel = 0u;
                     channel < 3u;
                     channel += 1u) {
                    fragment_uniforms
                        .irradiance_sh[coefficient][channel] =
                        lighting->irradiance_sh[
                            coefficient * 3u + channel
                        ];
                }
            }
            fragment_uniforms.ground_bottom_energy[0] =
                lighting->ground_bottom_red;
            fragment_uniforms.ground_bottom_energy[1] =
                lighting->ground_bottom_green;
            fragment_uniforms.ground_bottom_energy[2] =
                lighting->ground_bottom_blue;
            fragment_uniforms.ground_bottom_energy[3] =
                lighting->ground_energy;
            fragment_uniforms.ground_horizon_curve[0] =
                lighting->ground_horizon_red;
            fragment_uniforms.ground_horizon_curve[1] =
                lighting->ground_horizon_green;
            fragment_uniforms.ground_horizon_curve[2] =
                lighting->ground_horizon_blue;
            fragment_uniforms.ground_horizon_curve[3] =
                lighting->ground_horizon_curve;
            fragment_uniforms.sky_top_energy[0] =
                lighting->sky_top_red;
            fragment_uniforms.sky_top_energy[1] =
                lighting->sky_top_green;
            fragment_uniforms.sky_top_energy[2] =
                lighting->sky_top_blue;
            fragment_uniforms.sky_top_energy[3] =
                lighting->sky_energy;
            fragment_uniforms.sky_horizon_curve[0] =
                lighting->sky_horizon_red;
            fragment_uniforms.sky_horizon_curve[1] =
                lighting->sky_horizon_green;
            fragment_uniforms.sky_horizon_curve[2] =
                lighting->sky_horizon_blue;
            fragment_uniforms.sky_horizon_curve[3] =
                lighting->sky_horizon_curve;
            fragment_uniforms.sun_color_energy[0] =
                lighting->sun_red;
            fragment_uniforms.sun_color_energy[1] =
                lighting->sun_green;
            fragment_uniforms.sun_color_energy[2] =
                lighting->sun_blue;
            fragment_uniforms.sun_color_energy[3] =
                lighting->sun_energy;
            fragment_uniforms.sun_direction_size[0] =
                lighting->sun_direction_x;
            fragment_uniforms.sun_direction_size[1] =
                lighting->sun_direction_y;
            fragment_uniforms.sun_direction_size[2] =
                lighting->sun_direction_z;
            fragment_uniforms.sun_direction_size[3] =
                lighting->sun_size_radians;
            fragment_uniforms.procedural_controls[0] =
                lighting->sun_curve;
            fragment_uniforms.procedural_controls[1] =
                (float)lighting->ibl_enabled;
            (void)memcpy(
                fragment_uniforms
                    .directional_shadow_view_projection,
                lighting->directional_shadow_view_projection,
                sizeof(
                    fragment_uniforms
                        .directional_shadow_view_projection
                )
            );
            (void)memcpy(
                fragment_uniforms
                    .secondary_directional_shadow_view_projection,
                lighting
                    ->secondary_directional_shadow_view_projection,
                sizeof(
                    fragment_uniforms
                        .secondary_directional_shadow_view_projection
                )
            );
            fragment_uniforms.directional_shadow_controls[0] =
                (float)lighting->directional_shadow_enabled;
            fragment_uniforms.directional_shadow_controls[1] =
                lighting->directional_shadow_softness;
            fragment_uniforms
                .secondary_directional_shadow_controls[0] =
                (float)lighting
                    ->secondary_directional_shadow_enabled;
            fragment_uniforms
                .secondary_directional_shadow_controls[1] =
                lighting->secondary_directional_shadow_softness;
            SDL_PushGPUFragmentUniformData(
                command,
                0u,
                &fragment_uniforms,
                sizeof(fragment_uniforms)
            );
            if (animated) {
                SDL_BindGPUGraphicsPipeline(
                    pass,
                    shadows_active
                        ? window->scene_runtime
                            ->animated_model_pipelines[
                                batch->blend_mode
                            ][batch->depth_compare]
                        : window->scene_runtime
                            ->unshadowed_animated_model_pipelines[
                                batch->blend_mode
                            ][batch->depth_compare]
                );
                SDL_GPUBuffer *storage_buffers[2] = {
                    batch_instance_buffer,
                    bone_buffer
                };
                SDL_BindGPUVertexStorageBuffers(
                    pass,
                    0u,
                    storage_buffers,
                    2u
                );
            } else {
                SDL_BindGPUGraphicsPipeline(
                    pass,
                    shadows_active
                        ? window->scene_runtime->model_pipelines[
                            batch->blend_mode
                        ][batch->depth_compare]
                        : window->scene_runtime
                            ->unshadowed_model_pipelines[
                                batch->blend_mode
                            ][batch->depth_compare]
                );
                SDL_BindGPUVertexStorageBuffers(
                    pass,
                    0u,
                    &batch_instance_buffer,
                    1u
                );
            }
            BTBHGPUTexture *texture = batch->albedo_texture != NULL
                ? batch->albedo_texture
                : window->scene_runtime->fallback_texture;
            BTBHGPUTexture *emission_texture =
                batch->emission_texture != NULL
                    ? batch->emission_texture
                    : window->scene_runtime->fallback_texture;
            BTBHGPUTexture *normal_texture =
                batch->normal_texture != NULL
                    ? batch->normal_texture
                    : window->scene_runtime->normal_fallback_texture;
            BTBHGPUTexture *orm_texture =
                batch->orm_texture != NULL
                    ? batch->orm_texture
                    : window->scene_runtime->fallback_texture;
            SDL_GPUTextureSamplerBinding samplers[6];
            SDL_zeroa(samplers);
            samplers[0].texture = texture->texture;
            samplers[0].sampler = texture->sampler;
            samplers[1].texture = emission_texture->texture;
            samplers[1].sampler = emission_texture->sampler;
            samplers[2].texture = normal_texture->texture;
            samplers[2].sampler = normal_texture->sampler;
            samplers[3].texture = orm_texture->texture;
            samplers[3].sampler = orm_texture->sampler;
            samplers[4].texture =
                runtime->directional_shadow_textures[0];
            samplers[4].sampler =
                runtime->directional_shadow_sampler;
            samplers[5].texture =
                runtime->directional_shadow_textures[1];
            samplers[5].sampler =
                runtime->directional_shadow_sampler;
            SDL_BindGPUFragmentSamplers(
                pass,
                0u,
                samplers,
                shadows_active ? 6u : 4u
            );
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
    if (geometry_cursor != prepared_geometry_count) {
        (void)SDL_CancelGPUCommandBuffer(command);
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
    window->pending_drawable_width = drawable_width;
    window->pending_drawable_height = drawable_height;
    free(bone_offsets);

    report->result = BTBH_GPU_OK;
    report->drawable_width = drawable_width;
    report->drawable_height = drawable_height;
    report->batch_count = batch_count;
    report->primitive_draw_count = primitive_draw_count;
    report->prepared_geometry_count = prepared_geometry_count;
    uint32_t persistent_batch_count = 0u;
    for (uint32_t index = 0u; index < batch_count; ++index) {
        if (batches[index].instance_buffer != NULL) {
            persistent_batch_count += 1u;
        }
    }
    report->instance_count = logical_instance_count;
    report->transient_instance_count = transient_instance_count;
    report->persistent_batch_count = persistent_batch_count;
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
        if (window->scene_runtime->upload_transfer != NULL) {
            SDL_ReleaseGPUTransferBuffer(
                device,
                window->scene_runtime->upload_transfer
            );
        }
        if (window->scene_runtime->bone_buffer != NULL) {
            SDL_ReleaseGPUBuffer(
                device,
                window->scene_runtime->bone_buffer
            );
        }
        if (window->scene_runtime->instance_buffer != NULL) {
            SDL_ReleaseGPUBuffer(
                device,
                window->scene_runtime->instance_buffer
            );
        }
        if (window->scene_runtime->depth_texture != NULL) {
            SDL_ReleaseGPUTexture(
                device,
                window->scene_runtime->depth_texture
            );
        }
        if (
            window->scene_runtime->directional_shadow_sampler != NULL
        ) {
            SDL_ReleaseGPUSampler(
                device,
                window->scene_runtime->directional_shadow_sampler
            );
        }
        for (uint32_t index = 0u; index < 2u; ++index) {
            if (
                window->scene_runtime
                    ->directional_shadow_textures[index] != NULL
            ) {
                SDL_ReleaseGPUTexture(
                    device,
                    window->scene_runtime
                        ->directional_shadow_textures[index]
                );
            }
        }
        if (window->scene_runtime->shadow_pipeline != NULL) {
            SDL_ReleaseGPUGraphicsPipeline(
                device,
                window->scene_runtime->shadow_pipeline
            );
        }
        if (
            window->scene_runtime->animated_shadow_pipeline != NULL
        ) {
            SDL_ReleaseGPUGraphicsPipeline(
                device,
                window->scene_runtime->animated_shadow_pipeline
            );
        }
        for (uint32_t mode = 0u;
             mode < BTBH_GPU_SCENE_BLEND_COUNT;
             mode += 1u) {
            for (uint32_t depth = 0u;
                 depth < BTBH_GPU_SCENE_DEPTH_COUNT;
                 depth += 1u) {
                if (
                    window->scene_runtime
                        ->model_pipelines[mode][depth] != NULL
                ) {
                    SDL_ReleaseGPUGraphicsPipeline(
                        device,
                        window->scene_runtime
                            ->model_pipelines[mode][depth]
                    );
                }
                if (
                    window->scene_runtime
                        ->animated_model_pipelines[mode][depth] != NULL
                ) {
                    SDL_ReleaseGPUGraphicsPipeline(
                        device,
                        window->scene_runtime
                            ->animated_model_pipelines[mode][depth]
                    );
                }
                if (
                    window->scene_runtime
                        ->unshadowed_model_pipelines[mode][depth] != NULL
                ) {
                    SDL_ReleaseGPUGraphicsPipeline(
                        device,
                        window->scene_runtime
                            ->unshadowed_model_pipelines[mode][depth]
                    );
                }
                if (
                    window->scene_runtime
                        ->unshadowed_animated_model_pipelines[
                            mode
                        ][depth] != NULL
                ) {
                    SDL_ReleaseGPUGraphicsPipeline(
                        device,
                        window->scene_runtime
                            ->unshadowed_animated_model_pipelines[
                                mode
                            ][depth]
                    );
                }
            }
        }
    }
    if (window->scene_runtime->fallback_texture != NULL) {
        btbh_gpu_texture_destroy(
            window->scene_runtime->fallback_texture
        );
    }
    if (window->scene_runtime->normal_fallback_texture != NULL) {
        btbh_gpu_texture_destroy(
            window->scene_runtime->normal_fallback_texture
        );
    }
    free(window->scene_runtime);
    window->scene_runtime = NULL;
}
