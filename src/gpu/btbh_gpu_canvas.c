#include "btbh_gpu_internal.h"
#include "btbh_gpu_hdr.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#define BTBH_GPU_CANVAS_VERTEX_STRIDE 32u
#define BTBH_GPU_CANVAS_PIPELINE_COUNT 4u
#define BTBH_GPU_CANVAS_PATH_CAPACITY 1024u

struct BTBHGPUCanvasRuntime {
    SDL_GPUGraphicsPipeline
        *solid_pipelines[BTBH_GPU_CANVAS_PIPELINE_COUNT];
    SDL_GPUGraphicsPipeline
        *textured_pipelines[BTBH_GPU_CANVAS_PIPELINE_COUNT];
    /* For batches whose texture has one channel (a font atlas baked with
       BTBH_GPU_FONT_FLAG_R8_ATLAS): white x coverage x the vertex colour, with
       production_canvas_coverage.frag. Made the first time a batch needs them. */
    SDL_GPUGraphicsPipeline
        *coverage_pipelines[BTBH_GPU_CANVAS_PIPELINE_COUNT];
    /* The format the pipelines above draw into: the window's target format (the SDR swapchain's). */
    SDL_GPUTextureFormat format;
    /* RFC 0017 (since 0.17.0): the same pipelines drawing into RGBA16F (an HDR frame's screen passes, a frame-format
       render target), made the first time a pass needs them; float_ready once the solid and textured ones exist. */
    SDL_GPUGraphicsPipeline
        *float_solid_pipelines[BTBH_GPU_CANVAS_PIPELINE_COUNT];
    SDL_GPUGraphicsPipeline
        *float_textured_pipelines[BTBH_GPU_CANVAS_PIPELINE_COUNT];
    SDL_GPUGraphicsPipeline
        *float_coverage_pipelines[BTBH_GPU_CANVAS_PIPELINE_COUNT];
    bool float_ready;
    /* The shader paths the runtime was made with, for the RGBA16F pipelines. */
    char vertex_shader_path[BTBH_GPU_CANVAS_PATH_CAPACITY];
    char fragment_shader_path[BTBH_GPU_CANVAS_PATH_CAPACITY];
    char textured_fragment_shader_path[BTBH_GPU_CANVAS_PATH_CAPACITY];
    BTBHGPUShaderFormat shader_format;
    SDL_GPUBuffer *vertex_buffer;
    SDL_GPUTransferBuffer *upload_transfer;
    uint32_t vertex_capacity;
    uint32_t upload_capacity;
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
    const uint64_t load_started_ticks = btbh_gpu_lazy_creation_clock();
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
    SDL_GPUShader *shader = btbh_gpu_create_shader_from(device, &info, path, load_started_ticks, NULL);
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

static bool btbh_gpu_canvas_create_pipelines(
    BTBHGPUWindow *window,
    SDL_GPUShader *vertex_shader,
    SDL_GPUShader *fragment_shader,
    SDL_GPUTextureFormat swapchain_format,
    SDL_GPUGraphicsPipeline *pipelines[BTBH_GPU_CANVAS_PIPELINE_COUNT]
);

static bool btbh_gpu_canvas_ensure_float_pipelines(
    BTBHGPUWindow *window,
    bool coverage,
    BTBHGPUCanvasReport *report
);

static bool btbh_gpu_canvas_create_runtime(
    BTBHGPUWindow *window,
    const char *vertex_shader_path,
    const char *fragment_shader_path,
    const char *textured_fragment_shader_path,
    BTBHGPUShaderFormat project_format,
    SDL_GPUTextureFormat swapchain_format,
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
    const bool created =
        btbh_gpu_canvas_create_pipelines(
            window,
            vertex_shader,
            fragment_shader,
            swapchain_format,
            window->canvas_runtime->solid_pipelines
        ) &&
        btbh_gpu_canvas_create_pipelines(
            window,
            vertex_shader,
            textured_fragment_shader,
            swapchain_format,
            window->canvas_runtime->textured_pipelines
        );
    SDL_ReleaseGPUShader(device, textured_fragment_shader);
    SDL_ReleaseGPUShader(device, fragment_shader);
    SDL_ReleaseGPUShader(device, vertex_shader);
    if (!created) {
        btbh_gpu_canvas_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        btbh_gpu_window_release_canvas(window);
        return false;
    }
    BTBHGPUCanvasRuntime *runtime = window->canvas_runtime;
    runtime->format = swapchain_format;
    runtime->shader_format = project_format;
    (void)snprintf(runtime->vertex_shader_path, sizeof(runtime->vertex_shader_path), "%s", vertex_shader_path);
    (void)snprintf(runtime->fragment_shader_path, sizeof(runtime->fragment_shader_path), "%s", fragment_shader_path);
    (void)snprintf(
        runtime->textured_fragment_shader_path,
        sizeof(runtime->textured_fragment_shader_path),
        "%s",
        textured_fragment_shader_path
    );
    return true;
}

/* The path of filename in the directory of reference (no directory: filename alone); false if it
   doesn't fit in output. */
static bool btbh_gpu_canvas_sibling_path(
    const char *reference,
    const char *filename,
    char output[BTBH_GPU_CANVAS_PATH_CAPACITY]
) {
    const char *slash = strrchr(reference, '/');
    const char *backslash = strrchr(reference, '\\');
    const char *separator = slash;
    if (backslash != NULL && (separator == NULL || backslash > separator)) {
        separator = backslash;
    }
    const size_t directory_length =
        separator == NULL ? 0u : (size_t)(separator - reference + 1);
    const size_t filename_length = strlen(filename);
    if (directory_length + filename_length + 1u > BTBH_GPU_CANVAS_PATH_CAPACITY) {
        return false;
    }
    if (directory_length > 0u) {
        (void)memcpy(output, reference, directory_length);
    }
    (void)memcpy(output + directory_length, filename, filename_length + 1u);
    return true;
}

/* A batch whose texture has one channel (R8: a font atlas baked with BTBH_GPU_FONT_FLAG_R8_ATLAS)
   is drawn as coverage. */
static bool btbh_gpu_canvas_texture_is_coverage(const BTBHGPUTexture *texture) {
    return texture != NULL && texture->format == SDL_GPU_TEXTUREFORMAT_R8_UNORM;
}

/* Makes the coverage pipelines, once: production_canvas_coverage.frag from the directory of the
   textured fragment shader, with the canvas's vertex shader. */
static bool btbh_gpu_canvas_ensure_coverage_pipelines(
    BTBHGPUWindow *window,
    const char *vertex_shader_path,
    const char *textured_fragment_shader_path,
    BTBHGPUShaderFormat project_format,
    SDL_GPUTextureFormat swapchain_format,
    BTBHGPUCanvasReport *report
) {
    BTBHGPUCanvasRuntime *runtime = window->canvas_runtime;
    if (runtime->coverage_pipelines[0] != NULL) {
        return true;
    }
    const bool metal = project_format == BTBH_GPU_SHADER_FORMAT_METALLIB;
    char path[BTBH_GPU_CANVAS_PATH_CAPACITY];
    if (!btbh_gpu_canvas_sibling_path(
            textured_fragment_shader_path,
            metal
                ? "production_canvas_coverage.frag.metallib"
                : "production_canvas_coverage.frag.spv",
            path
        )) {
        btbh_gpu_canvas_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            "The canvas coverage shader's path is too long"
        );
        return false;
    }
    SDL_GPUDevice *device = window->device->gpu;
    const SDL_GPUShaderFormat format =
        btbh_gpu_canvas_shader_format(project_format);
    const char *entrypoint = metal ? "main0" : "main";
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
    SDL_GPUShader *coverage_shader = btbh_gpu_canvas_load_shader(
        device,
        path,
        entrypoint,
        format,
        SDL_GPU_SHADERSTAGE_FRAGMENT,
        1u,
        0u,
        report
    );
    if (coverage_shader == NULL) {
        SDL_ReleaseGPUShader(device, vertex_shader);
        if (report->result == BTBH_GPU_INVALID_ARGUMENT) {
            (void)snprintf(
                report->message,
                sizeof(report->message),
                "A one-channel texture (an R8 font atlas) needs %s",
                path
            );
        }
        return false;
    }
    const bool created = btbh_gpu_canvas_create_pipelines(
        window,
        vertex_shader,
        coverage_shader,
        swapchain_format,
        runtime->coverage_pipelines
    );
    SDL_ReleaseGPUShader(device, coverage_shader);
    SDL_ReleaseGPUShader(device, vertex_shader);
    if (!created) {
        btbh_gpu_canvas_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return false;
    }
    return true;
}


/* RFC 0017: one blend set of pipelines from these shader paths drawing into RGBA16F. */
static bool btbh_gpu_canvas_make_float_set(
    BTBHGPUWindow *window,
    const char *fragment_shader_path,
    uint32_t sampler_count,
    SDL_GPUGraphicsPipeline *pipelines[BTBH_GPU_CANVAS_PIPELINE_COUNT],
    BTBHGPUCanvasReport *report
) {
    BTBHGPUCanvasRuntime *runtime = window->canvas_runtime;
    SDL_GPUDevice *gpu = window->device->gpu;
    const SDL_GPUShaderFormat format = btbh_gpu_canvas_shader_format(runtime->shader_format);
    const char *entrypoint = runtime->shader_format == BTBH_GPU_SHADER_FORMAT_METALLIB ? "main0" : "main";
    SDL_GPUShader *vertex_shader = btbh_gpu_canvas_load_shader(
        gpu, runtime->vertex_shader_path, entrypoint, format, SDL_GPU_SHADERSTAGE_VERTEX, 0u, 1u, report
    );
    if (vertex_shader == NULL) {
        return false;
    }
    SDL_GPUShader *fragment_shader = btbh_gpu_canvas_load_shader(
        gpu, fragment_shader_path, entrypoint, format, SDL_GPU_SHADERSTAGE_FRAGMENT, sampler_count, 0u, report
    );
    if (fragment_shader == NULL) {
        SDL_ReleaseGPUShader(gpu, vertex_shader);
        return false;
    }
    const bool created = btbh_gpu_canvas_create_pipelines(
        window,
        vertex_shader,
        fragment_shader,
        SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
        pipelines
    );
    SDL_ReleaseGPUShader(gpu, fragment_shader);
    SDL_ReleaseGPUShader(gpu, vertex_shader);
    if (!created) {
        btbh_gpu_canvas_error(report, BTBH_GPU_DEVICE_FAILURE, SDL_GetError());
    }
    return created;
}

/* RFC 0017: the canvas's pipelines drawing into RGBA16F (an HDR frame's screen passes, a frame-format render target),
   made once from the shader paths the runtime was made with; the coverage ones when `coverage`. */
static bool btbh_gpu_canvas_ensure_float_pipelines(
    BTBHGPUWindow *window,
    bool coverage,
    BTBHGPUCanvasReport *report
) {
    BTBHGPUCanvasRuntime *runtime = window->canvas_runtime;
    if (runtime == NULL) {
        return false;
    }
    if (!runtime->float_ready) {
        if (!btbh_gpu_canvas_make_float_set(
                window, runtime->fragment_shader_path, 0u, runtime->float_solid_pipelines, report
            )) {
            return false;
        }
        if (!btbh_gpu_canvas_make_float_set(
                window, runtime->textured_fragment_shader_path, 1u, runtime->float_textured_pipelines, report
            )) {
            return false;
        }
        runtime->float_ready = true;
    }
    if (coverage && runtime->float_coverage_pipelines[0] == NULL) {
        char path[BTBH_GPU_CANVAS_PATH_CAPACITY];
        const bool metal = runtime->shader_format == BTBH_GPU_SHADER_FORMAT_METALLIB;
        if (!btbh_gpu_canvas_sibling_path(
                runtime->textured_fragment_shader_path,
                metal ? "production_canvas_coverage.frag.metallib" : "production_canvas_coverage.frag.spv",
                path
            )) {
            btbh_gpu_canvas_error(report, BTBH_GPU_INVALID_ARGUMENT, "The canvas coverage shader's path is too long");
            return false;
        }
        if (!btbh_gpu_canvas_make_float_set(window, path, 1u, runtime->float_coverage_pipelines, report)) {
            return false;
        }
    }
    return true;
}

/* The canvas's pipeline for each blend mode from these shaders, drawing into the swapchain's format; false, with none
   kept, if one of them can't be made. */
static bool btbh_gpu_canvas_create_pipelines(
    BTBHGPUWindow *window,
    SDL_GPUShader *vertex_shader,
    SDL_GPUShader *fragment_shader,
    SDL_GPUTextureFormat swapchain_format,
    SDL_GPUGraphicsPipeline *pipelines[BTBH_GPU_CANVAS_PIPELINE_COUNT]
) {
    SDL_GPUDevice *device = window->device->gpu;
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
    color_target.format = swapchain_format;
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
        pipelines[index] = btbh_gpu_create_graphics_pipeline(device, &info);
        if (pipelines[index] == NULL) {
            for (uint32_t made = 0u; made < index; ++made) {
                SDL_ReleaseGPUGraphicsPipeline(device, pipelines[made]);
                pipelines[made] = NULL;
            }
            return false;
        }
    }
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
             (batch->texture->owner != window ||
              btbh_gpu_texture_is_array(batch->texture) ||
              btbh_gpu_texture_is_heightfield(batch->texture))) ||
            (batch->scissor_enabled != 0u &&
             (batch->scissor_width <= 0 ||
              batch->scissor_height <= 0))
        ) {
            return false;
        }
    }
    return true;
}

static bool btbh_gpu_canvas_validate_passes(
    const BTBHGPUWindow *window,
    const BTBHGPUFrameContext *frame_context,
    const BTBHGPUCanvasPass *passes,
    uint32_t pass_count,
    uint32_t batch_count
) {
    uint32_t expected_first_batch = 0u;
    for (uint32_t index = 0u; index < pass_count; ++index) {
        const BTBHGPUCanvasPass *pass = &passes[index];
        if (
            pass->first_batch != expected_first_batch ||
            pass->batch_count > batch_count - expected_first_batch ||
            (pass->target != NULL &&
             (pass->target->owner != window ||
              btbh_gpu_texture_is_array(pass->target) ||
              btbh_gpu_texture_is_heightfield(pass->target) ||
              (pass->target->format != frame_context->target_format &&
               pass->target->format != SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT)))
        ) {
            return false;
        }
        expected_first_batch += pass->batch_count;
    }
    return expected_first_batch == batch_count;
}

static int btbh_gpu_canvas_scale_screen_edge(
    int64_t coordinate,
    uint32_t logical_extent,
    uint32_t drawable_extent,
    bool round_up
) {
    if (coordinate <= 0 || logical_extent == 0u) {
        return 0;
    }
    if ((uint64_t)coordinate >= logical_extent) {
        return (int)drawable_extent;
    }
    const uint64_t numerator =
        (uint64_t)coordinate * drawable_extent;
    const uint64_t scaled = round_up
        ? (numerator + logical_extent - 1u) / logical_extent
        : numerator / logical_extent;
    return (int)scaled;
}

static SDL_Rect btbh_gpu_canvas_scale_screen_scissor(
    const BTBHGPUCanvasBatch *batch,
    uint32_t logical_width,
    uint32_t logical_height,
    uint32_t drawable_width,
    uint32_t drawable_height
) {
    const int64_t right =
        (int64_t)batch->scissor_x + batch->scissor_width;
    const int64_t bottom =
        (int64_t)batch->scissor_y + batch->scissor_height;
    SDL_Rect result;
    result.x = btbh_gpu_canvas_scale_screen_edge(
        batch->scissor_x,
        logical_width,
        drawable_width,
        false
    );
    result.y = btbh_gpu_canvas_scale_screen_edge(
        batch->scissor_y,
        logical_height,
        drawable_height,
        false
    );
    const int scaled_right = btbh_gpu_canvas_scale_screen_edge(
        right,
        logical_width,
        drawable_width,
        true
    );
    const int scaled_bottom = btbh_gpu_canvas_scale_screen_edge(
        bottom,
        logical_height,
        drawable_height,
        true
    );
    result.w = scaled_right - result.x;
    result.h = scaled_bottom - result.y;
    return result;
}

static void btbh_gpu_window_poll_radiance_cascades_telemetry(
    BTBHGPUWindow *window
) {
    if (window == NULL ||
        window->device == NULL ||
        window->device->gpu == NULL ||
        window->frame_telemetry_enabled ||
        !window->radiance_cascades_telemetry_enabled) {
        return;
    }
    const uint64_t observed_ticks = SDL_GetTicksNS();
    uint32_t retained_count = 0u;
    for (uint32_t index = 0u;
         index < window->frame_telemetry_pending_count;
         ++index) {
        BTBHGPUFrameTelemetryPending pending =
            window->frame_telemetry_pending[index];
        if (SDL_QueryGPUFence(window->device->gpu, pending.fence)) {
            const double completion_milliseconds =
                (double)(observed_ticks - pending.submit_started_ticks) /
                1000000.0;
            btbh_gpu_window_scene_record_frame_completion(
                window,
                completion_milliseconds
            );
            SDL_ReleaseGPUFence(window->device->gpu, pending.fence);
        } else {
            window->frame_telemetry_pending[retained_count] = pending;
            retained_count += 1u;
        }
    }
    window->frame_telemetry_pending_count = retained_count;
}

/* Hands a frame telemetry fence back to SDL once the GPU has signalled it, waiting for it first when it hasn't (since
   0.11.0). SDL gives a released fence to the next submission at once (vkResetFences on Vulkan), so a fence released
   while its frame still runs can read signalled before that next submission has run. A presented frame's fence is
   also held by its swapchain until SDL has waited for it, but one that presented nothing (a hidden or minimized
   window) is held only here. */
static void btbh_gpu_window_release_telemetry_fence(SDL_GPUDevice *device, SDL_GPUFence *fence) {
    if (!SDL_QueryGPUFence(device, fence)) {
        (void)SDL_WaitForGPUFences(device, true, &fence, 1u);
    }
    SDL_ReleaseGPUFence(device, fence);
}

bool btbh_gpu_window_finish_pending_frame(
    BTBHGPUWindow *window,
    bool submit
) {
    if (window == NULL || window->pending_command == NULL) {
        return false;
    }
    SDL_GPUCommandBuffer *command = window->pending_command;
    const bool acquired_swapchain =
        window->pending_swapchain != NULL;
    /* A frame that holds queued uploads (or a write recorded after its draws) is submitted even when it is dropped,
       so that they land (since 0.11.0; a host that never queues drops such frames as before). */
    const bool holds_uploads = btbh_gpu_upload_queue_finish_pending(window);
    const bool must_submit = submit || acquired_swapchain || holds_uploads;
    /* (A scene frame that prints no cost-log line here leaves no split or pass state to the next frame's.) */
    const bool scene_frame = window->pending_scene_ready;
    bool cost_emitted = false;
    window->pending_command = NULL;
    window->pending_swapchain = NULL;
    window->pending_scene_ready = false;
    window->pending_drawable_width = 0u;
    window->pending_drawable_height = 0u;
    window->presented_frame_readback_requested = false;
    if (window->radiance_cascades_telemetry_enabled) {
        btbh_gpu_window_poll_radiance_cascades_telemetry(window);
    } else if (!window->frame_telemetry_enabled &&
               window->frame_telemetry_pending_count > 0u) {
        btbh_gpu_window_release_frame_telemetry(window);
    }
    const uint64_t lazy_submit_started_ticks = must_submit ? btbh_gpu_lazy_creation_clock() : 0u;
    bool result;
    if (must_submit && btbh_gpu_cost_log_enabled()) {
        btbh_gpu_cost_wait_warm();
        const uint64_t submit_started_ticks = SDL_GetTicksNS();
        SDL_GPUFence *fence =
            SDL_SubmitGPUCommandBufferAndAcquireFence(command);
        result = fence != NULL;
        if (fence != NULL) {
            (void)SDL_WaitForGPUFences(
                window->device->gpu,
                true,
                &fence,
                1u
            );
            btbh_gpu_cost_emit_frame(
                (double)(SDL_GetTicksNS() - submit_started_ticks) /
                1000000.0
            );
            cost_emitted = true;
            SDL_ReleaseGPUFence(window->device->gpu, fence);
            btbh_gpu_cost_warm(window->device->gpu);
        }
    } else if (submit &&
        (window->frame_telemetry_enabled ||
         window->radiance_cascades_telemetry_enabled)) {
        const uint64_t submit_started_ticks = SDL_GetTicksNS();
        SDL_GPUFence *fence =
            SDL_SubmitGPUCommandBufferAndAcquireFence(command);
        const uint64_t submit_finished_ticks = SDL_GetTicksNS();
        result = fence != NULL;
        if (fence != NULL) {
            if (window->frame_telemetry_pending_count == BTBH_GPU_FRAME_TELEMETRY_CAPACITY) {
                /* Full (no poll for that many frames): the oldest sample makes room, its fence long signalled. The
                   new one's fence stays held until it signals (since 0.11.0; before, it was released at once and
                   the newest sample dropped). */
                btbh_gpu_window_release_telemetry_fence(window->device->gpu, window->frame_telemetry_pending[0].fence);
                window->frame_telemetry_pending_count -= 1u;
                SDL_memmove(
                    window->frame_telemetry_pending,
                    window->frame_telemetry_pending + 1,
                    window->frame_telemetry_pending_count * sizeof(window->frame_telemetry_pending[0])
                );
            }
            const uint32_t index =
                window->frame_telemetry_pending_count;
            BTBHGPUFrameTelemetryPending *pending =
                &window->frame_telemetry_pending[index];
            window->next_frame_telemetry_serial += 1u;
            pending->fence = fence;
            pending->frame_serial =
                window->next_frame_telemetry_serial;
            pending->submit_started_ticks =
                submit_started_ticks;
            pending->cpu_submit_milliseconds =
                (double)(
                    submit_finished_ticks -
                    submit_started_ticks
                ) / 1000000.0;
            window->frame_telemetry_pending_count += 1u;
        }
    } else {
        result = must_submit
            ? SDL_SubmitGPUCommandBuffer(command)
            : SDL_CancelGPUCommandBuffer(command);
    }
    btbh_gpu_lazy_creation_call_timed(BTBH_GPU_LAZY_CALL_FRAME_SUBMIT, lazy_submit_started_ticks);
    if (scene_frame && !cost_emitted) {
        btbh_gpu_cost_abandon_frame();
    }
    /* A frame of the window's, submitted or dropped: a new use of its kept palettes (since 0.16.0); nothing when it
       keeps none. */
    if (window->bone_palette_cache != NULL) {
        (void)btbh_gpu_bone_palette_cache_begin(&window->bone_palette_cache, true, false);
    }
    if (must_submit && result) {
        window->submitted_frame_count += 1u;
        if (acquired_swapchain) {
            /* (A submit that presents: SDL takes back what finished command buffers held, btbh_gpu_scene.c's
               prewarm.) */
            btbh_gpu_note_command_buffers_settled();
        }
    }
    btbh_gpu_window_scene_submission_finished(
        window,
        must_submit && result
    );
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

void btbh_gpu_window_set_frame_telemetry_enabled(
    BTBHGPUWindow *window,
    uint8_t enabled
) {
    if (window == NULL) {
        return;
    }
    const bool should_enable = enabled != 0u;
    if (window->frame_telemetry_enabled == should_enable) {
        return;
    }
    btbh_gpu_window_release_frame_telemetry(window);
    window->frame_telemetry_enabled = should_enable;
    window->next_frame_telemetry_serial = 0u;
}

uint32_t btbh_gpu_window_poll_frame_telemetry(
    BTBHGPUWindow *window,
    BTBHGPUFrameTelemetrySample *samples,
    uint32_t capacity
) {
    if (
        window == NULL ||
        window->device == NULL ||
        window->device->gpu == NULL ||
        !window->frame_telemetry_enabled ||
        samples == NULL ||
        capacity == 0u
    ) {
        return 0u;
    }
    const uint64_t observed_ticks = SDL_GetTicksNS();
    uint32_t output_count = 0u;
    uint32_t retained_count = 0u;
    for (
        uint32_t index = 0u;
        index < window->frame_telemetry_pending_count;
        ++index
    ) {
        BTBHGPUFrameTelemetryPending pending =
            window->frame_telemetry_pending[index];
        if (
            output_count < capacity &&
            SDL_QueryGPUFence(
                window->device->gpu,
                pending.fence
            )
        ) {
            BTBHGPUFrameTelemetrySample *sample =
                &samples[output_count];
            sample->frame_serial = pending.frame_serial;
            sample->cpu_submit_milliseconds =
                pending.cpu_submit_milliseconds;
            sample->fence_completion_milliseconds =
                (double)(
                    observed_ticks -
                    pending.submit_started_ticks
                ) / 1000000.0;
            btbh_gpu_window_scene_record_frame_completion(
                window,
                sample->fence_completion_milliseconds
            );
            output_count += 1u;
            SDL_ReleaseGPUFence(
                window->device->gpu,
                pending.fence
            );
        } else {
            window->frame_telemetry_pending[retained_count] =
                pending;
            retained_count += 1u;
        }
    }
    window->frame_telemetry_pending_count = retained_count;
    return output_count;
}

uint32_t btbh_gpu_window_finish_frame_telemetry(
    BTBHGPUWindow *window,
    BTBHGPUFrameTelemetrySample *samples,
    uint32_t capacity
) {
    if (
        window == NULL ||
        window->device == NULL ||
        window->device->gpu == NULL ||
        !window->frame_telemetry_enabled ||
        samples == NULL ||
        capacity == 0u ||
        window->frame_telemetry_pending_count == 0u
    ) {
        return 0u;
    }
    SDL_GPUFence *fences[BTBH_GPU_FRAME_TELEMETRY_CAPACITY];
    for (
        uint32_t index = 0u;
        index < window->frame_telemetry_pending_count;
        ++index
    ) {
        fences[index] =
            window->frame_telemetry_pending[index].fence;
    }
    /* 32 at a time (since 0.11.0): asked to wait for all 256 at once, SDL 3.4 on Vulkan (an RTX 3070 Ti, driver
       621.14) returned at once with the newest still running. */
    for (
        uint32_t start = 0u;
        start < window->frame_telemetry_pending_count;
        start += 32u
    ) {
        (void)SDL_WaitForGPUFences(
            window->device->gpu,
            true,
            &fences[start],
            SDL_min(32u, window->frame_telemetry_pending_count - start)
        );
    }
    return btbh_gpu_window_poll_frame_telemetry(
        window,
        samples,
        capacity
    );
}

void btbh_gpu_window_release_frame_telemetry(
    BTBHGPUWindow *window
) {
    if (window == NULL) {
        return;
    }
    SDL_GPUDevice *device =
        window->device != NULL &&
        !window->device->shutdown
            ? window->device->gpu
            : NULL;
    if (device != NULL) {
        /* (A frame still running is waited for: its fence must not go back to SDL before it signals.) */
        for (
            uint32_t index = 0u;
            index < window->frame_telemetry_pending_count;
            ++index
        ) {
            btbh_gpu_window_release_telemetry_fence(
                device,
                window->frame_telemetry_pending[index].fence
            );
        }
    }
    window->frame_telemetry_pending_count = 0u;
}

void btbh_gpu_window_request_presented_frame_readback(
    BTBHGPUWindow *window
) {
    if (
        window != NULL &&
        window->window != NULL &&
        window->device != NULL &&
        window->device->gpu != NULL &&
        window->claimed
    ) {
        window->presented_frame_readback_requested = true;
    }
}

void btbh_gpu_window_frame_context_fill(
    const BTBHGPUWindow *window,
    BTBHGPUFrameContext *frame_context
) {
    SDL_zerop(frame_context);
    if (
        window == NULL ||
        window->window == NULL ||
        window->device == NULL ||
        window->device->gpu == NULL ||
        !window->claimed
    ) {
        return;
    }
    int drawable_width = 0;
    int drawable_height = 0;
    if (SDL_GetWindowSizeInPixels(window->window, &drawable_width, &drawable_height) &&
        drawable_width > 0 &&
        drawable_height > 0) {
        frame_context->drawable_width = (uint32_t)drawable_width;
        frame_context->drawable_height = (uint32_t)drawable_height;
    }
    int logical_width = 0;
    int logical_height = 0;
    if (SDL_GetWindowSize(window->window, &logical_width, &logical_height) &&
        logical_width > 0 &&
        logical_height > 0) {
        frame_context->logical_width = (uint32_t)logical_width;
        frame_context->logical_height = (uint32_t)logical_height;
    }
    frame_context->swapchain_format = SDL_GetGPUSwapchainTextureFormat(window->device->gpu, window->window);
    /* RFC 0017: in SDR the three formats are equal (the target format is the SDR swapchain's, kept from the claim). */
    frame_context->target_format = window->sdr_format != SDL_GPU_TEXTUREFORMAT_INVALID
        ? window->sdr_format
        : frame_context->swapchain_format;
    frame_context->frame_format = window->hdr_active
        ? SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT
        : frame_context->swapchain_format;
    frame_context->hdr_active = window->hdr_active;
    frame_context->hdr_debug_heat = window->hdr_active && window->hdr_debug_heat;
    frame_context->hdr_highlight_ratio = window->hdr_active ? window->hdr_state.highlight_ratio : 1.0f;
    frame_context->hdr_knee_override = window->hdr_knee_override;
    frame_context->hdr_shoulder_scale =
        window->hdr_shoulder_scale > 0.0f ? window->hdr_shoulder_scale : BTBH_HDR_SHOULDER_SCALE;
}

/* The texture the frame's screen passes draw into: the swapchain image, or with a readback requested the presented
   frame (at the image's size, in the frame's swapchain format), blitted into the image before the submit. */
SDL_GPUTexture *btbh_gpu_window_present_target(
    BTBHGPUWindow *window,
    const BTBHGPUFrameContext *frame_context
) {
    if (
        window == NULL ||
        window->window == NULL ||
        window->device == NULL ||
        window->device->gpu == NULL ||
        window->pending_command == NULL ||
        window->pending_swapchain == NULL ||
        window->pending_drawable_width == 0u ||
        window->pending_drawable_height == 0u
    ) {
        return NULL;
    }
    if (!window->presented_frame_readback_requested && !window->hdr_active) {
        return window->pending_swapchain;
    }
    /* A read-back frame in the swapchain's format, or (RFC 0017) the HDR frame in RGBA16F. */
    const SDL_GPUTextureFormat format = frame_context->frame_format;
    if (
        window->presented_frame != NULL &&
        (
            window->presented_frame->width !=
                window->pending_drawable_width ||
            window->presented_frame->height !=
                window->pending_drawable_height ||
            window->presented_frame->format != format
        )
    ) {
        btbh_gpu_texture_destroy(window->presented_frame);
        window->presented_frame = NULL;
    }
    if (window->presented_frame == NULL) {
        BTBHGPUTextureReport report;
        window->presented_frame = btbh_gpu_window_texture_create_empty(
            window,
            window->pending_drawable_width,
            window->pending_drawable_height,
            format,
            SDL_GPU_TEXTUREUSAGE_SAMPLER |
                SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
            BTBH_GPU_TEXTURE_FILTER_LINEAR,
            BTBH_GPU_TEXTURE_WRAP_CLAMP,
            &report
        );
        if (window->presented_frame == NULL) {
            return NULL;
        }
    }
    return window->presented_frame->texture;
}

static bool btbh_gpu_window_record_hdr_frame(BTBHGPUWindow *window);

bool btbh_gpu_window_record_presented_frame(
    BTBHGPUWindow *window
) {
    if (
        window == NULL ||
        window->pending_command == NULL ||
        window->pending_swapchain == NULL ||
        window->presented_frame == NULL ||
        window->presented_frame->texture == NULL
    ) {
        return false;
    }
    if (window->hdr_active) {
        /* RFC 0017: the HDR frame is encoded into the image. */
        return btbh_gpu_window_record_hdr_frame(window);
    }
    SDL_GPUBlitInfo blit;
    SDL_zero(blit);
    blit.source.texture = window->presented_frame->texture;
    blit.source.w = window->pending_drawable_width;
    blit.source.h = window->pending_drawable_height;
    blit.destination.texture = window->pending_swapchain;
    blit.destination.w = window->pending_drawable_width;
    blit.destination.h = window->pending_drawable_height;
    blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
    blit.filter = SDL_GPU_FILTER_NEAREST;
    SDL_InsertGPUDebugLabel(
        window->pending_command,
        "BTBH Presented Frame Composite"
    );
    SDL_BlitGPUTexture(window->pending_command, &blit);
    return true;
}


/* HDR output (RFC 0017, since 0.17.0): the GPU side of the window's HDR frame. */

static SDL_GPUTextureFormat btbh_gpu_hdr_output_format(const BTBHGPUWindow *window) {
    return window->hdr_composition == BTBH_GPU_SWAPCHAIN_HDR_EXTENDED_LINEAR
        ? SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT
        : SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM;
}

SDL_GPUGraphicsPipeline *btbh_gpu_window_hdr_create_pass_pipeline(
    BTBHGPUWindow *window,
    const char *fragment_name,
    SDL_GPUTextureFormat target_format,
    char *message,
    size_t message_capacity
) {
    if (window == NULL || window->device == NULL || window->device->gpu == NULL ||
        window->shader_reference_path[0] == '\0') {
        (void)snprintf(message, message_capacity, "%s", "HDR output needs a present's shader paths first");
        return NULL;
    }
    const bool metal = window->shader_reference_format == BTBH_GPU_SHADER_FORMAT_METALLIB;
    const char *extension = metal ? "metallib" : "spv";
    char filename[128];
    char vertex_path[BTBH_GPU_CANVAS_PATH_CAPACITY];
    char fragment_path[BTBH_GPU_CANVAS_PATH_CAPACITY];
    (void)snprintf(filename, sizeof(filename), "production_post.vert.%s", extension);
    if (!btbh_gpu_canvas_sibling_path(window->shader_reference_path, filename, vertex_path)) {
        (void)snprintf(message, message_capacity, "%s", "The HDR pass's shader path is too long");
        return NULL;
    }
    (void)snprintf(filename, sizeof(filename), "%s.%s", fragment_name, extension);
    if (!btbh_gpu_canvas_sibling_path(window->shader_reference_path, filename, fragment_path)) {
        (void)snprintf(message, message_capacity, "%s", "The HDR pass's shader path is too long");
        return NULL;
    }
    SDL_GPUDevice *gpu = window->device->gpu;
    const SDL_GPUShaderFormat format = btbh_gpu_canvas_shader_format(window->shader_reference_format);
    const char *entrypoint = metal ? "main0" : "main";
    BTBHGPUCanvasReport report;
    SDL_zero(report);
    SDL_GPUShader *vertex_shader = btbh_gpu_canvas_load_shader(
        gpu, vertex_path, entrypoint, format, SDL_GPU_SHADERSTAGE_VERTEX, 0u, 0u, &report
    );
    SDL_GPUShader *fragment_shader = vertex_shader == NULL
        ? NULL
        : btbh_gpu_canvas_load_shader(
            gpu, fragment_path, entrypoint, format, SDL_GPU_SHADERSTAGE_FRAGMENT, 1u, 1u, &report
        );
    SDL_GPUGraphicsPipeline *pipeline = NULL;
    if (fragment_shader != NULL) {
        SDL_GPUColorTargetDescription color_target;
        SDL_GPUGraphicsPipelineCreateInfo info;
        SDL_zero(color_target);
        SDL_zero(info);
        color_target.format = target_format;
        color_target.blend_state.color_write_mask = 0x0fu;
        info.vertex_shader = vertex_shader;
        info.fragment_shader = fragment_shader;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.target_info.color_target_descriptions = &color_target;
        info.target_info.num_color_targets = 1u;
        pipeline = btbh_gpu_create_graphics_pipeline(gpu, &info);
        if (pipeline == NULL) {
            (void)snprintf(report.message, sizeof(report.message), "%s", SDL_GetError());
        }
    }
    if (fragment_shader != NULL) {
        SDL_ReleaseGPUShader(gpu, fragment_shader);
    }
    if (vertex_shader != NULL) {
        SDL_ReleaseGPUShader(gpu, vertex_shader);
    }
    if (pipeline == NULL) {
        (void)snprintf(message, message_capacity, "%s", report.message);
    }
    return pipeline;
}

typedef struct BTBHGPUHDREncodeUniforms {
    float paper_white_nits;
    float highlight_ratio;
    float linear_scale;
    float dither_scale;
    uint32_t encode_mode;
    float unused[3];
} BTBHGPUHDREncodeUniforms;

bool btbh_gpu_window_hdr_record_encode(
    BTBHGPUWindow *window,
    SDL_GPUCommandBuffer *command,
    SDL_GPUTexture *frame,
    SDL_GPUTexture *destination,
    SDL_GPUTextureFormat destination_format,
    uint32_t width,
    uint32_t height
) {
    const uint32_t index = destination_format == SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT ? 1u : 0u;
    if (window->hdr_encode_pipelines[index] == NULL) {
        char message[256];
        window->hdr_encode_pipelines[index] = btbh_gpu_window_hdr_create_pass_pipeline(
            window,
            "production_hdr_encode.frag",
            destination_format,
            message,
            sizeof(message)
        );
        if (window->hdr_encode_pipelines[index] == NULL) {
            SDL_SetError("%s", message);
            return false;
        }
    }
    if (window->presented_frame == NULL || window->presented_frame->sampler == NULL) {
        return false;
    }
    BTBHGPUHDREncodeUniforms uniforms;
    SDL_zero(uniforms);
    uniforms.paper_white_nits = window->hdr_state.paper_white_nits;
    uniforms.highlight_ratio = window->hdr_state.highlight_ratio > 1.0f ? window->hdr_state.highlight_ratio : 1.0f;
    uniforms.linear_scale = window->hdr_state.relative_luminance != 0u
        ? 1.0f
        : window->hdr_state.paper_white_nits / BTBH_HDR_SCRGB_NITS;
    const bool hdr10 = window->hdr_composition == BTBH_GPU_SWAPCHAIN_HDR10_ST2084;
    uniforms.dither_scale = hdr10 && window->hdr_dither ? 1.0f / 1023.0f : 0.0f;
    uniforms.encode_mode = hdr10 ? 0u : 1u;
    SDL_GPUColorTargetInfo color_target;
    SDL_zero(color_target);
    color_target.texture = destination;
    color_target.load_op = SDL_GPU_LOADOP_DONT_CARE;
    color_target.store_op = SDL_GPU_STOREOP_STORE;
    SDL_InsertGPUDebugLabel(command, "BTBH HDR Encode");
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, &color_target, 1u, NULL);
    if (pass == NULL) {
        return false;
    }
    SDL_Rect scissor;
    scissor.x = 0;
    scissor.y = 0;
    scissor.w = (int)width;
    scissor.h = (int)height;
    SDL_SetGPUScissor(pass, &scissor);
    SDL_PushGPUFragmentUniformData(command, 0u, &uniforms, (uint32_t)sizeof(uniforms));
    SDL_BindGPUGraphicsPipeline(pass, window->hdr_encode_pipelines[index]);
    SDL_GPUTextureSamplerBinding sampler;
    SDL_zero(sampler);
    sampler.texture = frame;
    sampler.sampler = window->presented_frame->sampler;
    SDL_BindGPUFragmentSamplers(pass, 0u, &sampler, 1u);
    SDL_DrawGPUPrimitives(pass, 3u, 1u, 0u, 0u);
    SDL_EndGPURenderPass(pass);
    return true;
}

/* BTBH_GPU_HDR_SIMULATE: the encode's target in place of the swapchain image, at the frame's size. */
static SDL_GPUTexture *btbh_gpu_hdr_virtual_swapchain(BTBHGPUWindow *window, uint32_t width, uint32_t height) {
    const SDL_GPUTextureFormat format = btbh_gpu_hdr_output_format(window);
    if (window->hdr_virtual_swapchain != NULL &&
        (window->hdr_virtual_swapchain->width != width ||
         window->hdr_virtual_swapchain->height != height ||
         window->hdr_virtual_swapchain->format != format)) {
        btbh_gpu_texture_destroy(window->hdr_virtual_swapchain);
        window->hdr_virtual_swapchain = NULL;
    }
    if (window->hdr_virtual_swapchain == NULL) {
        BTBHGPUTextureReport report;
        window->hdr_virtual_swapchain = btbh_gpu_window_texture_create_empty(
            window,
            width,
            height,
            format,
            SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
            BTBH_GPU_TEXTURE_FILTER_NEAREST,
            BTBH_GPU_TEXTURE_WRAP_CLAMP,
            &report
        );
    }
    return window->hdr_virtual_swapchain != NULL ? window->hdr_virtual_swapchain->texture : NULL;
}

/* The HDR frame into the swapchain image: the encode (with BTBH_GPU_HDR_SIMULATE into the simulated swapchain, and the
   frame blitted to the SDR image, clamped). Without the encode's shaders (no present has given its paths yet) the frame
   is blitted as it is. */
static bool btbh_gpu_window_record_hdr_frame(BTBHGPUWindow *window) {
    const uint32_t width = window->pending_drawable_width;
    const uint32_t height = window->pending_drawable_height;
    SDL_GPUTexture *frame = window->presented_frame->texture;
    SDL_GPUTexture *destination = window->pending_swapchain;
    if (window->hdr_simulate != 0u) {
        destination = btbh_gpu_hdr_virtual_swapchain(window, width, height);
        if (destination == NULL) {
            return false;
        }
    }
    const bool encoded = btbh_gpu_window_hdr_record_encode(
        window,
        window->pending_command,
        frame,
        destination,
        btbh_gpu_hdr_output_format(window),
        width,
        height
    );
    if (encoded && window->hdr_simulate == 0u) {
        return true;
    }
    if (!encoded && window->hdr_simulate != 0u) {
        return false;
    }
    SDL_GPUBlitInfo blit;
    SDL_zero(blit);
    blit.source.texture = frame;
    blit.source.w = width;
    blit.source.h = height;
    blit.destination.texture = window->pending_swapchain;
    blit.destination.w = width;
    blit.destination.h = height;
    blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
    blit.filter = SDL_GPU_FILTER_NEAREST;
    SDL_InsertGPUDebugLabel(window->pending_command, "BTBH HDR Frame To SDR");
    SDL_BlitGPUTexture(window->pending_command, &blit);
    return true;
}

void btbh_gpu_window_hdr_prewarm(BTBHGPUWindow *window) {
    if (window == NULL || !window->hdr_active || window->device == NULL || window->device->gpu == NULL) {
        return;
    }
    int drawable_width = 0;
    int drawable_height = 0;
    /* (Not the textures while a frame is pending: its commands may draw into the frame texture.) */
    const bool frame_pending = window->pending_command != NULL || window->pending_scene_ready;
    if (!frame_pending &&
        SDL_GetWindowSizeInPixels(window->window, &drawable_width, &drawable_height) &&
        drawable_width > 0 && drawable_height > 0) {
        if (window->presented_frame != NULL &&
            (window->presented_frame->width != (uint32_t)drawable_width ||
             window->presented_frame->height != (uint32_t)drawable_height ||
             window->presented_frame->format != SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT)) {
            btbh_gpu_texture_destroy(window->presented_frame);
            window->presented_frame = NULL;
        }
        if (window->presented_frame == NULL) {
            BTBHGPUTextureReport report;
            window->presented_frame = btbh_gpu_window_texture_create_empty(
                window,
                (uint32_t)drawable_width,
                (uint32_t)drawable_height,
                SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
                SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
                BTBH_GPU_TEXTURE_FILTER_LINEAR,
                BTBH_GPU_TEXTURE_WRAP_CLAMP,
                &report
            );
        }
        if (window->hdr_simulate != 0u) {
            (void)btbh_gpu_hdr_virtual_swapchain(window, (uint32_t)drawable_width, (uint32_t)drawable_height);
        }
    }
    const SDL_GPUTextureFormat output_format = btbh_gpu_hdr_output_format(window);
    const uint32_t index = output_format == SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT ? 1u : 0u;
    if (window->hdr_encode_pipelines[index] == NULL) {
        char message[256];
        window->hdr_encode_pipelines[index] = btbh_gpu_window_hdr_create_pass_pipeline(
            window,
            "production_hdr_encode.frag",
            output_format,
            message,
            sizeof(message)
        );
    }
    /* The fold of an HDR capture into an SDR target (btbh_gpu_window_copy_pending_scene_texture). */
    if (window->hdr_fold_pipeline == NULL) {
        char message[256];
        window->hdr_fold_pipeline = btbh_gpu_window_hdr_create_pass_pipeline(
            window,
            "production_hdr_fold.frag",
            window->sdr_format,
            message,
            sizeof(message)
        );
    }
    if (window->canvas_runtime != NULL) {
        BTBHGPUCanvasReport report;
        SDL_zero(report);
        (void)btbh_gpu_canvas_ensure_float_pipelines(window, false, &report);
    }
    SDL_ClearError();
}

/* Halves to RGBA8 (an HDR frame's RGBA8 read-back: clamped and rounded), or RGBA8/BGRA8 widened to halves. */
static void btbh_gpu_hdr_convert_row(
    const uint8_t *source,
    SDL_GPUTextureFormat format,
    uint32_t width,
    uint8_t *rgba8,
    uint16_t *rgba16f
) {
    if (format == SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT) {
        const uint16_t *halves = (const uint16_t *)source;
        for (uint32_t x = 0u; x < width * 4u; ++x) {
            if (rgba8 != NULL) {
                rgba8[x] = btbh_hdr_clamp_to_unorm8(btbh_hdr_half_to_float(halves[x]));
            } else {
                rgba16f[x] = halves[x];
            }
        }
        return;
    }
    const bool bgra =
        format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM ||
        format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB;
    for (uint32_t x = 0u; x < width; ++x) {
        const uint8_t r = source[x * 4u + (bgra ? 2u : 0u)];
        const uint8_t g = source[x * 4u + 1u];
        const uint8_t b = source[x * 4u + (bgra ? 0u : 2u)];
        const uint8_t a = source[x * 4u + 3u];
        rgba16f[x * 4u] = btbh_hdr_float_to_half((float)r / 255.0f);
        rgba16f[x * 4u + 1u] = btbh_hdr_float_to_half((float)g / 255.0f);
        rgba16f[x * 4u + 2u] = btbh_hdr_float_to_half((float)b / 255.0f);
        rgba16f[x * 4u + 3u] = btbh_hdr_float_to_half((float)a / 255.0f);
    }
}

/* Downloads `texture` (width x height, `bytes_per_pixel`) into a mapped transfer buffer, rows `row_pitch` apart: the
   caller reads it, then unmaps and releases it. NULL with SDL's error on failure. */
static SDL_GPUTransferBuffer *btbh_gpu_hdr_download(
    SDL_GPUDevice *gpu,
    SDL_GPUTexture *texture,
    uint32_t width,
    uint32_t height,
    uint32_t bytes_per_pixel,
    uint64_t *row_pitch,
    const uint8_t **mapped
) {
    const uint64_t row_bytes = (uint64_t)width * bytes_per_pixel;
    *row_pitch = (row_bytes + UINT64_C(255)) & ~UINT64_C(255);
    const uint64_t transfer_bytes = *row_pitch * (uint64_t)height;
    if (transfer_bytes > UINT32_MAX) {
        SDL_SetError("The frame is too large to read back");
        return NULL;
    }
    SDL_GPUTransferBufferCreateInfo transfer_info;
    SDL_zero(transfer_info);
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    transfer_info.size = (uint32_t)transfer_bytes;
    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(gpu, &transfer_info);
    SDL_GPUCommandBuffer *command = transfer != NULL ? SDL_AcquireGPUCommandBuffer(gpu) : NULL;
    SDL_GPUCopyPass *copy = command != NULL ? SDL_BeginGPUCopyPass(command) : NULL;
    if (copy == NULL) {
        if (command != NULL) {
            (void)SDL_CancelGPUCommandBuffer(command);
        }
        if (transfer != NULL) {
            SDL_ReleaseGPUTransferBuffer(gpu, transfer);
        }
        return NULL;
    }
    SDL_GPUTextureRegion source;
    SDL_GPUTextureTransferInfo destination;
    SDL_zero(source);
    SDL_zero(destination);
    source.texture = texture;
    source.w = width;
    source.h = height;
    source.d = 1u;
    destination.transfer_buffer = transfer;
    destination.pixels_per_row = (uint32_t)(*row_pitch / bytes_per_pixel);
    destination.rows_per_layer = height;
    SDL_DownloadFromGPUTexture(copy, &source, &destination);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
    if (fence == NULL || !SDL_WaitForGPUFences(gpu, true, &fence, 1u)) {
        if (fence != NULL) {
            SDL_ReleaseGPUFence(gpu, fence);
        }
        SDL_ReleaseGPUTransferBuffer(gpu, transfer);
        return NULL;
    }
    SDL_ReleaseGPUFence(gpu, fence);
    *mapped = (const uint8_t *)SDL_MapGPUTransferBuffer(gpu, transfer, false);
    if (*mapped == NULL) {
        SDL_ReleaseGPUTransferBuffer(gpu, transfer);
        return NULL;
    }
    return transfer;
}

BTBHGPUResult btbh_gpu_window_readback_presented_frame_rgba16f(
    BTBHGPUWindow *window,
    uint16_t *rgba16f,
    uint32_t byte_capacity,
    BTBHGPUCanvasReport *report
) {
    if (report == NULL) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    (void)memset(report, 0, sizeof(*report));
    if (window == NULL || window->device == NULL || window->device->gpu == NULL ||
        window->presented_frame == NULL || rgba16f == NULL) {
        btbh_gpu_canvas_error(report, BTBH_GPU_INVALID_ARGUMENT, "No presented GPU frame is available");
        return report->result;
    }
    BTBHGPUTexture *frame = window->presented_frame;
    const uint64_t output_bytes = (uint64_t)frame->width * (uint64_t)frame->height * UINT64_C(8);
    if (output_bytes > byte_capacity) {
        btbh_gpu_canvas_error(report, BTBH_GPU_INVALID_ARGUMENT, "Presented GPU frame readback buffer is too small");
        return report->result;
    }
    const uint32_t bytes_per_pixel = frame->format == SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT ? 8u : 4u;
    uint64_t row_pitch = 0u;
    const uint8_t *mapped = NULL;
    SDL_GPUTransferBuffer *transfer = btbh_gpu_hdr_download(
        window->device->gpu, frame->texture, frame->width, frame->height, bytes_per_pixel, &row_pitch, &mapped
    );
    if (transfer == NULL) {
        btbh_gpu_canvas_error(report, BTBH_GPU_DEVICE_FAILURE, SDL_GetError());
        return report->result;
    }
    for (uint32_t y = 0u; y < frame->height; ++y) {
        btbh_gpu_hdr_convert_row(
            mapped + (size_t)y * (size_t)row_pitch,
            frame->format,
            frame->width,
            NULL,
            rgba16f + (size_t)y * (size_t)frame->width * 4u
        );
    }
    SDL_UnmapGPUTransferBuffer(window->device->gpu, transfer);
    SDL_ReleaseGPUTransferBuffer(window->device->gpu, transfer);
    report->result = BTBH_GPU_OK;
    report->drawable_width = frame->width;
    report->drawable_height = frame->height;
    report->vertex_count = (uint32_t)output_bytes;
    return report->result;
}

BTBHGPUResult btbh_gpu_window_readback_hdr_output(
    BTBHGPUWindow *window,
    void *pixels,
    uint32_t byte_capacity,
    uint32_t *width,
    uint32_t *height,
    uint32_t *bytes_per_pixel
) {
    if (window == NULL || pixels == NULL || width == NULL || height == NULL || bytes_per_pixel == NULL ||
        window->device == NULL || window->device->gpu == NULL || window->hdr_virtual_swapchain == NULL) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    BTBHGPUTexture *output = window->hdr_virtual_swapchain;
    const uint32_t pixel_bytes = output->format == SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT ? 8u : 4u;
    const uint64_t output_bytes = (uint64_t)output->width * (uint64_t)output->height * pixel_bytes;
    if (output_bytes > byte_capacity) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    uint64_t row_pitch = 0u;
    const uint8_t *mapped = NULL;
    SDL_GPUTransferBuffer *transfer = btbh_gpu_hdr_download(
        window->device->gpu, output->texture, output->width, output->height, pixel_bytes, &row_pitch, &mapped
    );
    if (transfer == NULL) {
        return BTBH_GPU_DEVICE_FAILURE;
    }
    for (uint32_t y = 0u; y < output->height; ++y) {
        (void)memcpy(
            (uint8_t *)pixels + (size_t)y * (size_t)output->width * pixel_bytes,
            mapped + (size_t)y * (size_t)row_pitch,
            (size_t)output->width * pixel_bytes
        );
    }
    SDL_UnmapGPUTransferBuffer(window->device->gpu, transfer);
    SDL_ReleaseGPUTransferBuffer(window->device->gpu, transfer);
    *width = output->width;
    *height = output->height;
    *bytes_per_pixel = pixel_bytes;
    return BTBH_GPU_OK;
}

static uint32_t btbh_gpu_canvas_buffer_capacity(
    uint32_t required
) {
    uint32_t capacity = 256u;
    while (capacity < required && capacity <= UINT32_MAX / 2u) {
        capacity *= 2u;
    }
    return capacity >= required ? capacity : required;
}

static bool btbh_gpu_canvas_ensure_upload_buffers(
    BTBHGPUWindow *window,
    uint32_t byte_count
) {
    if (byte_count == 0u) {
        return true;
    }
    BTBHGPUCanvasRuntime *runtime = window->canvas_runtime;
    SDL_GPUDevice *device = window->device->gpu;
    SDL_GPUBufferCreateInfo buffer_info;
    SDL_GPUTransferBufferCreateInfo transfer_info;
    SDL_zero(buffer_info);
    SDL_zero(transfer_info);
    if (
        runtime->vertex_buffer == NULL ||
        runtime->vertex_capacity < byte_count
    ) {
        if (runtime->vertex_buffer != NULL) {
            SDL_ReleaseGPUBuffer(device, runtime->vertex_buffer);
        }
        runtime->vertex_capacity =
            btbh_gpu_canvas_buffer_capacity(byte_count);
        buffer_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        buffer_info.size = runtime->vertex_capacity;
        runtime->vertex_buffer =
            SDL_CreateGPUBuffer(device, &buffer_info);
        if (runtime->vertex_buffer == NULL) {
            runtime->vertex_capacity = 0u;
            return false;
        }
    }
    if (
        runtime->upload_transfer == NULL ||
        runtime->upload_capacity < byte_count
    ) {
        if (runtime->upload_transfer != NULL) {
            SDL_ReleaseGPUTransferBuffer(
                device,
                runtime->upload_transfer
            );
        }
        runtime->upload_capacity =
            btbh_gpu_canvas_buffer_capacity(byte_count);
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

BTBHGPUResult btbh_gpu_window_readback_presented_frame(
    BTBHGPUWindow *window,
    uint8_t *rgba8,
    uint32_t byte_capacity,
    BTBHGPUCanvasReport *report
) {
    if (report == NULL) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    (void)memset(report, 0, sizeof(*report));
    if (
        window == NULL ||
        window->device == NULL ||
        window->device->gpu == NULL ||
        window->presented_frame == NULL ||
        rgba8 == NULL
    ) {
        btbh_gpu_canvas_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            "No presented GPU frame is available"
        );
        return report->result;
    }
    BTBHGPUTexture *frame = window->presented_frame;
    if (frame->format == SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT) {
        /* RFC 0017: an HDR frame, clamped to 0-1 and rounded. */
        const uint64_t hdr_output_bytes = (uint64_t)frame->width * (uint64_t)frame->height * UINT64_C(4);
        if (hdr_output_bytes > byte_capacity) {
            btbh_gpu_canvas_error(
                report,
                BTBH_GPU_INVALID_ARGUMENT,
                "Presented GPU frame readback buffer is too small"
            );
            return report->result;
        }
        uint64_t hdr_row_pitch = 0u;
        const uint8_t *hdr_mapped = NULL;
        SDL_GPUTransferBuffer *hdr_transfer = btbh_gpu_hdr_download(
            window->device->gpu, frame->texture, frame->width, frame->height, 8u, &hdr_row_pitch, &hdr_mapped
        );
        if (hdr_transfer == NULL) {
            btbh_gpu_canvas_error(report, BTBH_GPU_DEVICE_FAILURE, SDL_GetError());
            return report->result;
        }
        for (uint32_t y = 0u; y < frame->height; ++y) {
            btbh_gpu_hdr_convert_row(
                hdr_mapped + (size_t)y * (size_t)hdr_row_pitch,
                frame->format,
                frame->width,
                rgba8 + (size_t)y * (size_t)frame->width * 4u,
                NULL
            );
        }
        SDL_UnmapGPUTransferBuffer(window->device->gpu, hdr_transfer);
        SDL_ReleaseGPUTransferBuffer(window->device->gpu, hdr_transfer);
        report->result = BTBH_GPU_OK;
        report->drawable_width = frame->width;
        report->drawable_height = frame->height;
        report->vertex_count = (uint32_t)hdr_output_bytes;
        report->message[0] = '\0';
        return report->result;
    }
    const uint64_t row_bytes = (uint64_t)frame->width * UINT64_C(4);
    const uint64_t output_bytes =
        row_bytes * (uint64_t)frame->height;
    const uint64_t row_pitch = (row_bytes + UINT64_C(255)) &
        ~UINT64_C(255);
    const uint64_t transfer_bytes =
        row_pitch * (uint64_t)frame->height;
    if (
        output_bytes > byte_capacity ||
        transfer_bytes > UINT32_MAX ||
        row_pitch / UINT64_C(4) > UINT32_MAX
    ) {
        btbh_gpu_canvas_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            "Presented GPU frame readback buffer is too small"
        );
        return report->result;
    }

    SDL_GPUDevice *device = window->device->gpu;
    SDL_GPUTransferBufferCreateInfo transfer_info;
    SDL_zero(transfer_info);
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    transfer_info.size = (uint32_t)transfer_bytes;
    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(
        device,
        &transfer_info
    );
    SDL_GPUCommandBuffer *command =
        SDL_AcquireGPUCommandBuffer(device);
    if (transfer == NULL || command == NULL) {
        if (command != NULL) {
            (void)SDL_CancelGPUCommandBuffer(command);
        }
        if (transfer != NULL) {
            SDL_ReleaseGPUTransferBuffer(device, transfer);
        }
        btbh_gpu_canvas_error(
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
        btbh_gpu_canvas_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return report->result;
    }
    SDL_GPUTextureRegion source;
    SDL_GPUTextureTransferInfo destination;
    SDL_zero(source);
    SDL_zero(destination);
    source.texture = frame->texture;
    source.w = frame->width;
    source.h = frame->height;
    source.d = 1u;
    destination.transfer_buffer = transfer;
    destination.pixels_per_row = (uint32_t)(row_pitch / UINT64_C(4));
    destination.rows_per_layer = frame->height;
    SDL_DownloadFromGPUTexture(copy, &source, &destination);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence *fence =
        SDL_SubmitGPUCommandBufferAndAcquireFence(command);
    if (
        fence == NULL ||
        !SDL_WaitForGPUFences(device, true, &fence, 1u)
    ) {
        if (fence != NULL) {
            SDL_ReleaseGPUFence(device, fence);
        }
        SDL_ReleaseGPUTransferBuffer(device, transfer);
        btbh_gpu_canvas_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return report->result;
    }
    SDL_ReleaseGPUFence(device, fence);
    const uint8_t *mapped = (const uint8_t *)
        SDL_MapGPUTransferBuffer(device, transfer, false);
    if (mapped == NULL) {
        SDL_ReleaseGPUTransferBuffer(device, transfer);
        btbh_gpu_canvas_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return report->result;
    }
    const bool bgra =
        frame->format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM ||
        frame->format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB;
    for (uint32_t y = 0u; y < frame->height; ++y) {
        const uint8_t *source_row =
            mapped + (size_t)y * (size_t)row_pitch;
        uint8_t *destination_row =
            rgba8 + (size_t)y * (size_t)row_bytes;
        if (!bgra) {
            (void)memcpy(
                destination_row,
                source_row,
                (size_t)row_bytes
            );
            continue;
        }
        for (uint32_t x = 0u; x < frame->width; ++x) {
            destination_row[x * 4u] = source_row[x * 4u + 2u];
            destination_row[x * 4u + 1u] =
                source_row[x * 4u + 1u];
            destination_row[x * 4u + 2u] = source_row[x * 4u];
            destination_row[x * 4u + 3u] =
                source_row[x * 4u + 3u];
        }
    }
    SDL_UnmapGPUTransferBuffer(device, transfer);
    SDL_ReleaseGPUTransferBuffer(device, transfer);
    report->result = BTBH_GPU_OK;
    report->drawable_width = frame->width;
    report->drawable_height = frame->height;
    report->vertex_count = (uint32_t)output_bytes;
    report->message[0] = '\0';
    return report->result;
}

BTBHGPUResult btbh_gpu_write_png_rgba8(
    const char *path,
    uint32_t width,
    uint32_t height,
    const uint8_t *rgba8
) {
    if (
        path == NULL ||
        path[0] == '\0' ||
        width == 0u ||
        height == 0u ||
        width > (uint32_t)INT32_MAX ||
        height > (uint32_t)INT32_MAX ||
        width > (uint32_t)(INT32_MAX / 4) ||
        rgba8 == NULL
    ) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    return stbi_write_png(
        path,
        (int)width,
        (int)height,
        4,
        rgba8,
        (int)(width * 4u)
    ) != 0
        ? BTBH_GPU_OK
        : BTBH_GPU_PLATFORM_FAILURE;
}

static BTBHGPUResult btbh_gpu_window_canvas_present_internal(
    BTBHGPUWindow *window,
    const BTBHGPUFrameContext *frame_context,
    const char *vertex_shader_path,
    const char *fragment_shader_path,
    const char *textured_fragment_shader_path,
    BTBHGPUShaderFormat shader_format,
    const BTBHGPUCanvasVertex *vertices,
    uint32_t vertex_count,
    const BTBHGPUCanvasBatch *batches,
    uint32_t batch_count,
    const BTBHGPUCanvasPass *passes,
    uint32_t pass_count,
    BTBHGPUCanvasReport *report
);
static bool btbh_gpu_window_canvas_acquire_frame(
    BTBHGPUWindow *window,
    const BTBHGPUFrameContext *frame_context,
    SDL_GPUTexture **present_target,
    BTBHGPUCanvasReport *report
);
static bool btbh_gpu_window_canvas_draw(
    BTBHGPUWindow *window,
    SDL_GPUCommandBuffer *command,
    const BTBHGPUFrameContext *frame_context,
    SDL_GPUTexture *screen_target,
    uint32_t screen_width,
    uint32_t screen_height,
    bool had_scene,
    const BTBHGPUCanvasVertex *vertices,
    uint32_t vertex_count,
    const BTBHGPUCanvasBatch *batches,
    const BTBHGPUCanvasPass *passes,
    uint32_t pass_count,
    const char **failure
);
static BTBHGPUResult btbh_gpu_window_canvas_submit_frame(
    BTBHGPUWindow *window,
    uint32_t batch_count,
    uint32_t vertex_count,
    BTBHGPUCanvasReport *report
);

/* The canvas present inside the lazy creation log's scope, with what it reads from the window (RFC 0015 phase 1a: the
   frame context, read here). Its body, btbh_gpu_window_canvas_present_internal, follows at once, then its halves (the
   Swift source probes read the present from here to btbh_gpu_window_release_canvas). */
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
    const BTBHGPUCanvasPass *passes,
    uint32_t pass_count,
    BTBHGPUCanvasReport *report
) {
    /* RFC 0017: a canvas present with no scene before it starts the frame (an AUTO window takes an HDR change). */
    btbh_gpu_window_hdr_frame_start(window);
    BTBHGPUFrameContext frame_context;
    btbh_gpu_window_frame_context_fill(window, &frame_context);
    BTBHGPULazyCreationScope lazy_scope;
    btbh_gpu_lazy_creation_begin(window, BTBH_GPU_LAZY_CREATION_CALL_CANVAS_PRESENT, &lazy_scope);
    const BTBHGPUResult result = btbh_gpu_window_canvas_present_internal(
        window,
        &frame_context,
        vertex_shader_path,
        fragment_shader_path,
        textured_fragment_shader_path,
        shader_format,
        vertices,
        vertex_count,
        batches,
        batch_count,
        passes,
        pass_count,
        report
    );
    btbh_gpu_lazy_creation_end(&lazy_scope);
    return result;
}

/* RFC 0015 phase 1a (since 0.16.1): the canvas present in two halves, so that phase 1b can record the draw on a thread
   of its own while the window's thread keeps the swapchain. The draw half (btbh_gpu_window_canvas_draw) records the
   pending scene's blit, the vertices' upload and the passes into a command buffer and a target. The present half
   acquires the command buffer and the swapchain image before it (btbh_gpu_window_canvas_acquire_frame), and blits a
   read-back frame into the image and submits after it (btbh_gpu_window_canvas_submit_frame). Unthreaded the present
   runs them in that order, the swapchain first as before, and the screen passes draw into the image (or the read-back
   frame): the same frame, the same GPU work. */
static BTBHGPUResult btbh_gpu_window_canvas_present_internal(
    BTBHGPUWindow *window,
    const BTBHGPUFrameContext *frame_context,
    const char *vertex_shader_path,
    const char *fragment_shader_path,
    const char *textured_fragment_shader_path,
    BTBHGPUShaderFormat shader_format,
    const BTBHGPUCanvasVertex *vertices,
    uint32_t vertex_count,
    const BTBHGPUCanvasBatch *batches,
    uint32_t batch_count,
    const BTBHGPUCanvasPass *passes,
    uint32_t pass_count,
    BTBHGPUCanvasReport *report
) {
    if (report == NULL) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    (void)memset(report, 0, sizeof(*report));
    if (window != NULL && fragment_shader_path != NULL) {
        /* RFC 0017: the HDR passes' shaders are siblings of the canvas's. */
        btbh_gpu_window_hdr_remember_shaders(window, fragment_shader_path, shader_format);
    }
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
        (vertex_count != 0u && vertices == NULL) ||
        (batch_count != 0u && batches == NULL) ||
        passes == NULL ||
        pass_count == 0u ||
        (uint64_t)vertex_count * sizeof(*vertices) > UINT32_MAX ||
        !btbh_gpu_canvas_validate_batches(
            window,
            batches,
            batch_count,
            vertex_count
        ) ||
        !btbh_gpu_canvas_validate_passes(
            window,
            frame_context,
            passes,
            pass_count,
            batch_count
        )
    ) {
        if (window != NULL && window->pending_command != NULL) {
            (void)btbh_gpu_window_finish_pending_frame(window, false);
        }
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
            frame_context->target_format,
            report
        )
    ) {
        if (window->pending_command != NULL) {
            (void)btbh_gpu_window_finish_pending_frame(window, false);
        }
        return report->result;
    }
    bool draws_coverage = false;
    for (uint32_t index = 0u; index < batch_count && !draws_coverage; ++index) {
        draws_coverage = btbh_gpu_canvas_texture_is_coverage(batches[index].texture);
    }
    if (
        draws_coverage &&
        !btbh_gpu_canvas_ensure_coverage_pipelines(
            window,
            vertex_shader_path,
            textured_fragment_shader_path,
            shader_format,
            window->canvas_runtime->format,
            report
        )
    ) {
        if (window->pending_command != NULL) {
            (void)btbh_gpu_window_finish_pending_frame(window, false);
        }
        return report->result;
    }
    /* RFC 0017: an HDR frame's screen passes, and passes into frame-format render targets, draw into RGBA16F. */
    bool draws_float = frame_context->hdr_active;
    for (uint32_t index = 0u; index < pass_count && !draws_float; ++index) {
        draws_float = passes[index].target != NULL &&
            passes[index].target->format == SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    }
    if (
        draws_float &&
        window->canvas_runtime->format != SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT &&
        !btbh_gpu_canvas_ensure_float_pipelines(window, draws_coverage, report)
    ) {
        if (window->pending_command != NULL) {
            (void)btbh_gpu_window_finish_pending_frame(window, false);
        }
        return report->result;
    }

    const bool had_scene = window->pending_scene_ready;
    SDL_GPUTexture *present_target = NULL;
    if (!btbh_gpu_window_canvas_acquire_frame(window, frame_context, &present_target, report)) {
        return report->result;
    }
    const char *draw_failure = NULL;
    if (!btbh_gpu_window_canvas_draw(
            window,
            window->pending_command,
            frame_context,
            present_target,
            window->pending_drawable_width,
            window->pending_drawable_height,
            had_scene,
            vertices,
            vertex_count,
            batches,
            passes,
            pass_count,
            &draw_failure
        )) {
        /* (SDL's error is read once the frame is dropped, as before the split.) */
        (void)btbh_gpu_window_finish_pending_frame(window, false);
        btbh_gpu_canvas_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            draw_failure != NULL ? draw_failure : SDL_GetError()
        );
        return report->result;
    }
    return btbh_gpu_window_canvas_submit_frame(window, batch_count, vertex_count, report);
}

/* The present half, before the draw: the frame's command buffer (the pending scene's, else a new one), the swapchain
   image, and the target the screen passes draw into (btbh_gpu_window_present_target: the image, or the read-back
   frame; NULL for a window with no image, minimized or hidden, whose offscreen work is still submitted). False, the
   frame dropped and the report set, when the command buffer, the image or the read-back frame is missing. */
static bool btbh_gpu_window_canvas_acquire_frame(
    BTBHGPUWindow *window,
    const BTBHGPUFrameContext *frame_context,
    SDL_GPUTexture **present_target,
    BTBHGPUCanvasReport *report
) {
    SDL_GPUDevice *device = window->device->gpu;
    if (window->pending_command == NULL) {
        const uint64_t command_started_ticks = btbh_gpu_lazy_creation_clock();
        window->pending_command = SDL_AcquireGPUCommandBuffer(device);
        btbh_gpu_lazy_creation_call_timed(BTBH_GPU_LAZY_CALL_CANVAS_COMMAND_BUFFER, command_started_ticks);
    }
    const uint64_t acquire_started_ticks = btbh_gpu_lazy_creation_clock();
    const bool swapchain_acquired =
        window->pending_command != NULL &&
        SDL_WaitAndAcquireGPUSwapchainTexture(
            window->pending_command,
            window->window,
            &window->pending_swapchain,
            &window->pending_drawable_width,
            &window->pending_drawable_height
        );
    btbh_gpu_lazy_creation_call_timed(BTBH_GPU_LAZY_CALL_SWAPCHAIN_ACQUIRE, acquire_started_ticks);
    if (!swapchain_acquired) {
        char acquire_error[256];
        (void)snprintf(acquire_error, sizeof(acquire_error), "%s", SDL_GetError());
        if (window->pending_command != NULL) {
            (void)btbh_gpu_window_finish_pending_frame(
                window,
                false
            );
        }
        window->pending_scene_ready = false;
        /* RFC 0017: the swapchain may have lost its HDR composition; the window is SDR from the next frame. */
        (void)btbh_gpu_window_hdr_acquire_failed(window);
        btbh_gpu_canvas_error(
            report,
            BTBH_GPU_WINDOW_FAILURE,
            acquire_error
        );
        return false;
    }
    // A minimized or hidden window has no swapchain image. That is not an error: the
    // frame's offscreen work (scene and canvas target passes) still runs and is
    // submitted, and only the screen passes are skipped.
    const bool presentable = window->pending_swapchain != NULL;
    if (!presentable && window->presented_frame_readback_requested) {
        (void)btbh_gpu_window_finish_pending_frame(window, false);
        btbh_gpu_canvas_error(
            report,
            BTBH_GPU_WINDOW_FAILURE,
            "The window has no swapchain image to read back"
        );
        return false;
    }
    *present_target = presentable
        ? btbh_gpu_window_present_target(window, frame_context)
        : NULL;
    if (presentable && *present_target == NULL) {
        (void)btbh_gpu_window_finish_pending_frame(window, false);
        btbh_gpu_canvas_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return false;
    }
    return true;
}

/* The draw half: the pending scene's blit (had_scene), the vertices' upload and the passes, recorded into `command`. The
   screen passes draw into screen_target (screen_width x screen_height), projected in the frame's logical size, and are
   skipped when it is NULL (a window with no image); target passes draw into their targets. It queries neither the window
   nor its swapchain, and submits nothing. False when a step failed, with *failure the reason, or NULL for SDL's error
   (which the caller reads once it has dropped the frame). */
static bool btbh_gpu_window_canvas_draw(
    BTBHGPUWindow *window,
    SDL_GPUCommandBuffer *command,
    const BTBHGPUFrameContext *frame_context,
    SDL_GPUTexture *screen_target,
    uint32_t screen_width,
    uint32_t screen_height,
    bool had_scene,
    const BTBHGPUCanvasVertex *vertices,
    uint32_t vertex_count,
    const BTBHGPUCanvasBatch *batches,
    const BTBHGPUCanvasPass *passes,
    uint32_t pass_count,
    const char **failure
) {
    *failure = NULL;
    const bool presentable = screen_target != NULL;
    if (presentable && had_scene &&
        !btbh_gpu_window_blit_pending_scene(
            window,
            command,
            screen_target,
            screen_width,
            screen_height
        )) {
        *failure = "Could not composite submitted GPU scene";
        return false;
    }

    /* The window's logical size (the frame context's), else the image's. */
    const bool logical_size_available = frame_context->logical_width != 0u;
    const uint32_t logical_width = logical_size_available
        ? frame_context->logical_width
        : screen_width;
    const uint32_t logical_height = logical_size_available
        ? frame_context->logical_height
        : screen_height;

    SDL_GPUDevice *device = window->device->gpu;
    const uint32_t byte_count =
        vertex_count * (uint32_t)sizeof(*vertices);
    if (!btbh_gpu_canvas_ensure_upload_buffers(window, byte_count)) {
        return false;
    }
    BTBHGPUCanvasRuntime *runtime = window->canvas_runtime;
    SDL_GPUBuffer *vertex_buffer = runtime->vertex_buffer;
    SDL_GPUTransferBuffer *transfer = runtime->upload_transfer;
    if (byte_count != 0u) {
        const uint64_t map_started_ticks = btbh_gpu_lazy_creation_clock();
        void *mapped = SDL_MapGPUTransferBuffer(
            device,
            transfer,
            true
        );
        btbh_gpu_lazy_creation_call_timed(BTBH_GPU_LAZY_CALL_CANVAS_UPLOAD_MAP, map_started_ticks);
        if (mapped == NULL) {
            return false;
        }
        (void)memcpy(mapped, vertices, byte_count);
        SDL_UnmapGPUTransferBuffer(device, transfer);
        SDL_InsertGPUDebugLabel(
            command,
            "BTBH Canvas Upload"
        );
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(
            command
        );
        if (copy == NULL) {
            return false;
        }
        SDL_GPUTransferBufferLocation source;
        SDL_GPUBufferRegion destination;
        SDL_zero(source);
        SDL_zero(destination);
        source.transfer_buffer = transfer;
        destination.buffer = vertex_buffer;
        destination.size = byte_count;
        SDL_UploadToGPUBuffer(copy, &source, &destination, true);
        SDL_EndGPUCopyPass(copy);
    }

    bool screen_touched = had_scene;
    for (uint32_t pass_index = 0u;
         pass_index < pass_count;
         ++pass_index) {
        const BTBHGPUCanvasPass *canvas_pass = &passes[pass_index];
        const bool is_screen = canvas_pass->target == NULL;
        if (is_screen && !presentable) {
            continue;
        }
        SDL_GPUTexture *target_texture = is_screen
            ? screen_target
            : canvas_pass->target->texture;
        const uint32_t target_width = is_screen
            ? screen_width
            : canvas_pass->target->width;
        const uint32_t target_height = is_screen
            ? screen_height
            : canvas_pass->target->height;
        const uint32_t projection_width =
            is_screen ? logical_width : target_width;
        const uint32_t projection_height =
            is_screen ? logical_height : target_height;
        SDL_GPUColorTargetInfo color_target;
        SDL_zero(color_target);
        color_target.texture = target_texture;
        color_target.load_op =
            canvas_pass->clear_enabled != 0u ||
            (is_screen && !screen_touched)
                ? SDL_GPU_LOADOP_CLEAR
                : SDL_GPU_LOADOP_LOAD;
        color_target.store_op = SDL_GPU_STOREOP_STORE;
        color_target.clear_color.r =
            (float)canvas_pass->clear_red / 255.0f;
        color_target.clear_color.g =
            (float)canvas_pass->clear_green / 255.0f;
        color_target.clear_color.b =
            (float)canvas_pass->clear_blue / 255.0f;
        color_target.clear_color.a =
            (float)canvas_pass->clear_alpha / 255.0f;
        SDL_InsertGPUDebugLabel(
            command,
            is_screen
                ? "BTBH Canvas Screen Pass"
                : "BTBH Canvas Target Pass"
        );
        SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(
            command,
            &color_target,
            1u,
            NULL
        );
        if (pass == NULL) {
            return false;
        }
        const float uniforms[4] = {
            (float)projection_width,
            (float)projection_height,
            0.0f,
            0.0f
        };
        SDL_PushGPUVertexUniformData(
            command,
            0u,
            uniforms,
            sizeof(uniforms)
        );
        if (canvas_pass->batch_count != 0u) {
            SDL_GPUBufferBinding binding;
            SDL_zero(binding);
            binding.buffer = vertex_buffer;
            SDL_BindGPUVertexBuffers(pass, 0u, &binding, 1u);
        }
        const uint32_t last_batch =
            canvas_pass->first_batch + canvas_pass->batch_count;
        /* RFC 0017: a pass into RGBA16F (an HDR frame's screen pass, a frame-format target) takes the RGBA16F set. */
        const SDL_GPUTextureFormat pass_format = is_screen
            ? frame_context->frame_format
            : canvas_pass->target->format;
        const bool float_pass =
            pass_format == SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT &&
            window->canvas_runtime->format != SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
        SDL_GPUGraphicsPipeline *const *solid_pipelines = float_pass
            ? window->canvas_runtime->float_solid_pipelines
            : window->canvas_runtime->solid_pipelines;
        SDL_GPUGraphicsPipeline *const *textured_pipelines = float_pass
            ? window->canvas_runtime->float_textured_pipelines
            : window->canvas_runtime->textured_pipelines;
        SDL_GPUGraphicsPipeline *const *coverage_pipelines = float_pass
            ? window->canvas_runtime->float_coverage_pipelines
            : window->canvas_runtime->coverage_pipelines;
        SDL_GPUGraphicsPipeline *bound_pipeline = NULL;
        SDL_Rect bound_scissor;
        SDL_zero(bound_scissor);
        bool has_bound_scissor = false;
        for (uint32_t index = canvas_pass->first_batch;
             index < last_batch;
             ++index) {
            const BTBHGPUCanvasBatch *batch = &batches[index];
            SDL_GPUGraphicsPipeline *pipeline =
                batch->texture == NULL
                    ? solid_pipelines[batch->blend_mode]
                    : btbh_gpu_canvas_texture_is_coverage(batch->texture)
                    ? coverage_pipelines[batch->blend_mode]
                    : textured_pipelines[batch->blend_mode];
            if (pipeline != bound_pipeline) {
                SDL_BindGPUGraphicsPipeline(pass, pipeline);
                bound_pipeline = pipeline;
            }
            if (batch->texture != NULL) {
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
            }
            SDL_Rect scissor;
            if (batch->scissor_enabled != 0u) {
                if (is_screen) {
                    scissor = btbh_gpu_canvas_scale_screen_scissor(
                        batch,
                        logical_width,
                        logical_height,
                        target_width,
                        target_height
                    );
                } else {
                    scissor.x = batch->scissor_x;
                    scissor.y = batch->scissor_y;
                    scissor.w = batch->scissor_width;
                    scissor.h = batch->scissor_height;
                }
            } else {
                scissor.x = 0;
                scissor.y = 0;
                scissor.w = (int)target_width;
                scissor.h = (int)target_height;
            }
            if (scissor.w <= 0 || scissor.h <= 0) {
                continue;
            }
            if (!has_bound_scissor ||
                scissor.x != bound_scissor.x ||
                scissor.y != bound_scissor.y ||
                scissor.w != bound_scissor.w ||
                scissor.h != bound_scissor.h) {
                SDL_SetGPUScissor(pass, &scissor);
                bound_scissor = scissor;
                has_bound_scissor = true;
            }
            SDL_DrawGPUPrimitives(
                pass,
                batch->vertex_count,
                1u,
                batch->first_vertex,
                0u
            );
        }
        SDL_EndGPURenderPass(pass);
        if (is_screen) {
            screen_touched = true;
        }
    }
    if (presentable && !screen_touched) {
        SDL_InsertGPUDebugLabel(
            command,
            "BTBH Canvas Clear Screen"
        );
        SDL_GPUColorTargetInfo screen_clear;
        SDL_zero(screen_clear);
        screen_clear.texture = screen_target;
        screen_clear.load_op = SDL_GPU_LOADOP_CLEAR;
        screen_clear.store_op = SDL_GPU_STOREOP_STORE;
        screen_clear.clear_color.a = 1.0f;
        SDL_GPURenderPass *screen_pass = SDL_BeginGPURenderPass(
            command,
            &screen_clear,
            1u,
            NULL
        );
        if (screen_pass == NULL) {
            return false;
        }
        SDL_EndGPURenderPass(screen_pass);
    }
    return true;
}

/* The present half, after the draw: a read-back frame blitted into the swapchain image
   (btbh_gpu_window_record_presented_frame), then the frame submitted, which presents the image it holds, and the
   report. */
static BTBHGPUResult btbh_gpu_window_canvas_submit_frame(
    BTBHGPUWindow *window,
    uint32_t batch_count,
    uint32_t vertex_count,
    BTBHGPUCanvasReport *report
) {
    const uint32_t drawable_width =
        window->pending_drawable_width;
    const uint32_t drawable_height =
        window->pending_drawable_height;
    if (
        (window->presented_frame_readback_requested || window->hdr_active) &&
        window->pending_swapchain != NULL &&
        !btbh_gpu_window_record_presented_frame(window)
    ) {
        (void)btbh_gpu_window_finish_pending_frame(window, false);
        btbh_gpu_canvas_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            SDL_GetError()
        );
        return report->result;
    }
    const bool submitted = btbh_gpu_window_finish_pending_frame(
        window,
        true
    );
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
        for (uint32_t index = 0u; index < BTBH_GPU_CANVAS_PIPELINE_COUNT; ++index) {
            SDL_GPUGraphicsPipeline *const float_pipelines[3] = {
                window->canvas_runtime->float_solid_pipelines[index],
                window->canvas_runtime->float_textured_pipelines[index],
                window->canvas_runtime->float_coverage_pipelines[index]
            };
            for (uint32_t set = 0u; set < 3u; ++set) {
                if (float_pipelines[set] != NULL) {
                    SDL_ReleaseGPUGraphicsPipeline(device, float_pipelines[set]);
                }
            }
        }
        if (window->canvas_runtime->upload_transfer != NULL) {
            SDL_ReleaseGPUTransferBuffer(
                device,
                window->canvas_runtime->upload_transfer
            );
        }
        if (window->canvas_runtime->vertex_buffer != NULL) {
            SDL_ReleaseGPUBuffer(
                device,
                window->canvas_runtime->vertex_buffer
            );
        }
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
            if (
                window->canvas_runtime
                    ->coverage_pipelines[index] != NULL
            ) {
                SDL_ReleaseGPUGraphicsPipeline(
                    device,
                    window->canvas_runtime
                        ->coverage_pipelines[index]
                );
            }
        }
    }
    free(window->canvas_runtime);
    window->canvas_runtime = NULL;
}
