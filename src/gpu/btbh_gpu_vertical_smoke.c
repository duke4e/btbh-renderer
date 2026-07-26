#include "btbh_gpu_vertical_smoke.h"

#include "btbh_gpu_internal.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BTBH_COMPUTE_WIDTH 64u
#define BTBH_COMPUTE_HEIGHT 64u

typedef struct BTBHShaderBytes {
    unsigned char *data;
    size_t size;
} BTBHShaderBytes;

static void btbh_smoke_error(
    BTBHGPUVerticalSmokeReport *report,
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

static bool btbh_read_shader(
    const char *path,
    BTBHShaderBytes *shader,
    BTBHGPUVerticalSmokeReport *report
) {
    FILE *file = fopen(path, "rb");
    long size;

    if (file == NULL) {
        btbh_smoke_error(report, BTBH_GPU_INVALID_ARGUMENT, path);
        return false;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) <= 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        btbh_smoke_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            "Could not measure a shader artifact"
        );
        return false;
    }

    shader->data = (unsigned char *)malloc((size_t)size);
    if (shader->data == NULL) {
        fclose(file);
        btbh_smoke_error(
            report,
            BTBH_GPU_PLATFORM_FAILURE,
            "Could not allocate shader bytes"
        );
        return false;
    }
    shader->size = (size_t)size;
    if (fread(shader->data, 1, shader->size, file) != shader->size) {
        fclose(file);
        free(shader->data);
        shader->data = NULL;
        shader->size = 0;
        btbh_smoke_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            "Could not read a shader artifact"
        );
        return false;
    }
    fclose(file);
    return true;
}

static SDL_GPUShaderFormat btbh_sdl_shader_format(
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

static SDL_GPUShader *btbh_create_graphics_shader(
    SDL_GPUDevice *device,
    const BTBHShaderBytes *bytes,
    const char *entrypoint,
    SDL_GPUShaderFormat format,
    SDL_GPUShaderStage stage
) {
    SDL_GPUShaderCreateInfo create_info;
    SDL_zero(create_info);
    create_info.code = bytes->data;
    create_info.code_size = bytes->size;
    create_info.entrypoint = entrypoint;
    create_info.format = format;
    create_info.stage = stage;
    return SDL_CreateGPUShader(device, &create_info);
}

static SDL_GPUComputePipeline *btbh_create_compute_pipeline(
    SDL_GPUDevice *device,
    const BTBHShaderBytes *bytes,
    const char *entrypoint,
    SDL_GPUShaderFormat format
) {
    SDL_GPUComputePipelineCreateInfo create_info;
    SDL_zero(create_info);
    create_info.code = bytes->data;
    create_info.code_size = bytes->size;
    create_info.entrypoint = entrypoint;
    create_info.format = format;
    create_info.num_readwrite_storage_textures = 1;
    create_info.threadcount_x = 8;
    create_info.threadcount_y = 8;
    create_info.threadcount_z = 1;
    return SDL_CreateGPUComputePipeline(device, &create_info);
}

static SDL_GPUTexture *btbh_create_texture(
    SDL_GPUDevice *device,
    SDL_GPUTextureFormat format,
    SDL_GPUTextureUsageFlags usage,
    uint32_t width,
    uint32_t height
) {
    SDL_GPUTextureCreateInfo create_info;
    SDL_zero(create_info);
    create_info.type = SDL_GPU_TEXTURETYPE_2D;
    create_info.format = format;
    create_info.usage = usage;
    create_info.width = width;
    create_info.height = height;
    create_info.layer_count_or_depth = 1;
    create_info.num_levels = 1;
    create_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    return SDL_CreateGPUTexture(device, &create_info);
}

static SDL_GPUTransferBuffer *btbh_create_download_buffer(
    SDL_GPUDevice *device,
    uint32_t size
) {
    SDL_GPUTransferBufferCreateInfo create_info;
    SDL_zero(create_info);
    create_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    create_info.size = size;
    return SDL_CreateGPUTransferBuffer(device, &create_info);
}

static float btbh_half_to_float(uint16_t value)
{
    const uint16_t sign = (uint16_t)(value >> 15u);
    const uint16_t exponent = (uint16_t)((value >> 10u) & 0x1fu);
    const uint16_t mantissa = (uint16_t)(value & 0x03ffu);
    float result;

    if (exponent == 0) {
        result = mantissa == 0 ? 0.0f : ldexpf((float)mantissa, -24);
    } else if (exponent == 31) {
        result = mantissa == 0 ? INFINITY : NAN;
    } else {
        result = ldexpf(1.0f + ((float)mantissa / 1024.0f), exponent - 15);
    }
    return sign != 0 ? -result : result;
}

static bool btbh_save_capture(
    const char *path,
    void *rgba,
    uint32_t width,
    uint32_t height,
    BTBHGPUVerticalSmokeReport *report
) {
    SDL_Surface *surface = SDL_CreateSurfaceFrom(
        (int)width,
        (int)height,
        SDL_PIXELFORMAT_RGBA32,
        rgba,
        (int)(width * 4u)
    );
    bool saved;

    if (surface == NULL) {
        btbh_smoke_error(report, BTBH_GPU_PLATFORM_FAILURE, SDL_GetError());
        return false;
    }
    saved = SDL_SaveBMP(surface, path);
    SDL_DestroySurface(surface);
    if (!saved) {
        btbh_smoke_error(report, BTBH_GPU_PLATFORM_FAILURE, SDL_GetError());
    }
    return saved;
}

int btbh_gpu_run_vertical_smoke(
    const BTBHGPUVerticalSmokeConfig *config,
    BTBHGPUVerticalSmokeReport *report
) {
    BTBHShaderBytes vertex_bytes = { 0 };
    BTBHShaderBytes fragment_bytes = { 0 };
    BTBHShaderBytes compute_bytes = { 0 };
    BTBHGPUDevice *device = NULL;
    BTBHGPUDeviceReport device_report;
    SDL_GPUShader *vertex_shader = NULL;
    SDL_GPUShader *fragment_shader = NULL;
    SDL_GPUComputePipeline *compute_pipeline = NULL;
    SDL_GPUGraphicsPipeline *graphics_pipeline = NULL;
    SDL_GPUTexture *albedo = NULL;
    SDL_GPUTexture *normal_roughness = NULL;
    SDL_GPUTexture *emissive_hdr = NULL;
    SDL_GPUTexture *depth = NULL;
    SDL_GPUTexture *compute_output = NULL;
    SDL_GPUTransferBuffer *albedo_download = NULL;
    SDL_GPUTransferBuffer *compute_download = NULL;
    SDL_GPUCommandBuffer *command_buffer = NULL;
    SDL_GPUFence *fence = NULL;
    void *albedo_map = NULL;
    void *compute_map = NULL;
    int result = 1;

    if (config == NULL || report == NULL) {
        return 1;
    }
    memset(report, 0, sizeof(*report));
    if (config->vertex_shader_path == NULL ||
        config->fragment_shader_path == NULL ||
        config->compute_shader_path == NULL ||
        config->shader_entrypoint == NULL ||
        config->capture_path == NULL ||
        config->width == 0 ||
        config->height == 0 ||
        btbh_sdl_shader_format(config->shader_format) ==
            SDL_GPU_SHADERFORMAT_INVALID) {
        btbh_smoke_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            "Invalid vertical smoke configuration"
        );
        goto cleanup;
    }

    if (!btbh_read_shader(
            config->vertex_shader_path,
            &vertex_bytes,
            report
        ) ||
        !btbh_read_shader(
            config->fragment_shader_path,
            &fragment_bytes,
            report
        ) ||
        !btbh_read_shader(
            config->compute_shader_path,
            &compute_bytes,
            report
        )) {
        goto cleanup;
    }

    const BTBHGPUDeviceDescriptor descriptor = {
        config->backend,
        BTBH_GPU_PHASE2_REQUIRED_FEATURES,
        1,
        { 0 }
    };
    device = btbh_gpu_device_create(&descriptor, &device_report);
    if (device == NULL) {
        btbh_smoke_error(report, device_report.result, device_report.message);
        goto cleanup;
    }
    (void)snprintf(
        report->driver_name,
        sizeof(report->driver_name),
        "%s",
        device_report.capabilities.driver_name
    );

    const SDL_GPUShaderFormat shader_format =
        btbh_sdl_shader_format(config->shader_format);
    vertex_shader = btbh_create_graphics_shader(
        device->gpu,
        &vertex_bytes,
        config->shader_entrypoint,
        shader_format,
        SDL_GPU_SHADERSTAGE_VERTEX
    );
    fragment_shader = btbh_create_graphics_shader(
        device->gpu,
        &fragment_bytes,
        config->shader_entrypoint,
        shader_format,
        SDL_GPU_SHADERSTAGE_FRAGMENT
    );
    compute_pipeline = btbh_create_compute_pipeline(
        device->gpu,
        &compute_bytes,
        config->shader_entrypoint,
        shader_format
    );
    if (vertex_shader == NULL ||
        fragment_shader == NULL ||
        compute_pipeline == NULL) {
        btbh_smoke_error(report, BTBH_GPU_DEVICE_FAILURE, SDL_GetError());
        goto cleanup;
    }

    SDL_GPUColorTargetDescription color_targets[3];
    SDL_zeroa(color_targets);
    color_targets[0].format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    color_targets[1].format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    color_targets[2].format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;

    SDL_GPUGraphicsPipelineCreateInfo pipeline_info;
    SDL_zero(pipeline_info);
    pipeline_info.vertex_shader = vertex_shader;
    pipeline_info.fragment_shader = fragment_shader;
    pipeline_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pipeline_info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pipeline_info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pipeline_info.rasterizer_state.front_face =
        SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    pipeline_info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    pipeline_info.depth_stencil_state.compare_op =
        SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    pipeline_info.depth_stencil_state.enable_depth_test = true;
    pipeline_info.depth_stencil_state.enable_depth_write = true;
    pipeline_info.target_info.color_target_descriptions = color_targets;
    pipeline_info.target_info.num_color_targets = 3;
    pipeline_info.target_info.depth_stencil_format =
        SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    pipeline_info.target_info.has_depth_stencil_target = true;
    graphics_pipeline = SDL_CreateGPUGraphicsPipeline(
        device->gpu,
        &pipeline_info
    );
    if (graphics_pipeline == NULL) {
        btbh_smoke_error(report, BTBH_GPU_DEVICE_FAILURE, SDL_GetError());
        goto cleanup;
    }

    const SDL_GPUTextureUsageFlags color_usage =
        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
        SDL_GPU_TEXTUREUSAGE_SAMPLER;
    albedo = btbh_create_texture(
        device->gpu,
        SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        color_usage,
        config->width,
        config->height
    );
    normal_roughness = btbh_create_texture(
        device->gpu,
        SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        color_usage,
        config->width,
        config->height
    );
    emissive_hdr = btbh_create_texture(
        device->gpu,
        SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
        color_usage,
        config->width,
        config->height
    );
    depth = btbh_create_texture(
        device->gpu,
        SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
        SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET |
            SDL_GPU_TEXTUREUSAGE_SAMPLER,
        config->width,
        config->height
    );
    compute_output = btbh_create_texture(
        device->gpu,
        SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
        SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE |
            SDL_GPU_TEXTUREUSAGE_SAMPLER,
        BTBH_COMPUTE_WIDTH,
        BTBH_COMPUTE_HEIGHT
    );
    if (albedo == NULL ||
        normal_roughness == NULL ||
        emissive_hdr == NULL ||
        depth == NULL ||
        compute_output == NULL) {
        btbh_smoke_error(report, BTBH_GPU_DEVICE_FAILURE, SDL_GetError());
        goto cleanup;
    }

    const uint32_t albedo_size = config->width * config->height * 4u;
    const uint32_t compute_size =
        BTBH_COMPUTE_WIDTH * BTBH_COMPUTE_HEIGHT * 8u;
    albedo_download = btbh_create_download_buffer(
        device->gpu,
        albedo_size
    );
    compute_download = btbh_create_download_buffer(
        device->gpu,
        compute_size
    );
    if (albedo_download == NULL || compute_download == NULL) {
        btbh_smoke_error(report, BTBH_GPU_DEVICE_FAILURE, SDL_GetError());
        goto cleanup;
    }

    command_buffer = SDL_AcquireGPUCommandBuffer(device->gpu);
    if (command_buffer == NULL) {
        btbh_smoke_error(report, BTBH_GPU_DEVICE_FAILURE, SDL_GetError());
        goto cleanup;
    }

    SDL_GPUStorageTextureReadWriteBinding compute_binding;
    SDL_zero(compute_binding);
    compute_binding.texture = compute_output;
    SDL_GPUComputePass *compute_pass = SDL_BeginGPUComputePass(
        command_buffer,
        &compute_binding,
        1,
        NULL,
        0
    );
    if (compute_pass == NULL) {
        btbh_smoke_error(report, BTBH_GPU_DEVICE_FAILURE, SDL_GetError());
        goto cleanup;
    }
    SDL_BindGPUComputePipeline(compute_pass, compute_pipeline);
    SDL_DispatchGPUCompute(
        compute_pass,
        BTBH_COMPUTE_WIDTH / 8u,
        BTBH_COMPUTE_HEIGHT / 8u,
        1
    );
    SDL_EndGPUComputePass(compute_pass);

    SDL_GPUColorTargetInfo render_targets[3];
    SDL_zeroa(render_targets);
    render_targets[0].texture = albedo;
    render_targets[0].clear_color.r = 0.01f;
    render_targets[0].clear_color.g = 0.02f;
    render_targets[0].clear_color.b = 0.04f;
    render_targets[0].clear_color.a = 1.0f;
    render_targets[1].texture = normal_roughness;
    render_targets[1].clear_color.a = 1.0f;
    render_targets[2].texture = emissive_hdr;
    render_targets[2].clear_color.a = 1.0f;
    for (uint32_t index = 0; index < 3; ++index) {
        render_targets[index].load_op = SDL_GPU_LOADOP_CLEAR;
        render_targets[index].store_op = SDL_GPU_STOREOP_STORE;
    }

    SDL_GPUDepthStencilTargetInfo depth_target;
    SDL_zero(depth_target);
    depth_target.texture = depth;
    depth_target.clear_depth = 1.0f;
    depth_target.load_op = SDL_GPU_LOADOP_CLEAR;
    depth_target.store_op = SDL_GPU_STOREOP_STORE;
    depth_target.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth_target.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;

    SDL_GPURenderPass *render_pass = SDL_BeginGPURenderPass(
        command_buffer,
        render_targets,
        3,
        &depth_target
    );
    if (render_pass == NULL) {
        btbh_smoke_error(report, BTBH_GPU_DEVICE_FAILURE, SDL_GetError());
        goto cleanup;
    }
    SDL_BindGPUGraphicsPipeline(render_pass, graphics_pipeline);
    SDL_DrawGPUPrimitives(render_pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(render_pass);

    SDL_GPUTextureRegion albedo_region;
    SDL_zero(albedo_region);
    albedo_region.texture = albedo;
    albedo_region.w = config->width;
    albedo_region.h = config->height;
    albedo_region.d = 1;
    SDL_GPUTextureTransferInfo albedo_transfer;
    SDL_zero(albedo_transfer);
    albedo_transfer.transfer_buffer = albedo_download;

    SDL_GPUTextureRegion compute_region;
    SDL_zero(compute_region);
    compute_region.texture = compute_output;
    compute_region.w = BTBH_COMPUTE_WIDTH;
    compute_region.h = BTBH_COMPUTE_HEIGHT;
    compute_region.d = 1;
    SDL_GPUTextureTransferInfo compute_transfer;
    SDL_zero(compute_transfer);
    compute_transfer.transfer_buffer = compute_download;

    SDL_GPUCopyPass *copy_pass = SDL_BeginGPUCopyPass(command_buffer);
    if (copy_pass == NULL) {
        btbh_smoke_error(report, BTBH_GPU_DEVICE_FAILURE, SDL_GetError());
        goto cleanup;
    }
    SDL_DownloadFromGPUTexture(
        copy_pass,
        &albedo_region,
        &albedo_transfer
    );
    SDL_DownloadFromGPUTexture(
        copy_pass,
        &compute_region,
        &compute_transfer
    );
    SDL_EndGPUCopyPass(copy_pass);

    fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command_buffer);
    command_buffer = NULL;
    if (fence == NULL ||
        !SDL_WaitForGPUFences(device->gpu, true, &fence, 1)) {
        btbh_smoke_error(report, BTBH_GPU_DEVICE_FAILURE, SDL_GetError());
        goto cleanup;
    }

    albedo_map = SDL_MapGPUTransferBuffer(
        device->gpu,
        albedo_download,
        false
    );
    compute_map = SDL_MapGPUTransferBuffer(
        device->gpu,
        compute_download,
        false
    );
    if (albedo_map == NULL || compute_map == NULL) {
        btbh_smoke_error(report, BTBH_GPU_DEVICE_FAILURE, SDL_GetError());
        goto cleanup;
    }

    const size_t center_index =
        ((size_t)(config->height / 2u) * config->width +
            (config->width / 2u)) * 4u;
    memcpy(report->center_rgba, (uint8_t *)albedo_map + center_index, 4);

    const size_t compute_center_index =
        ((size_t)(BTBH_COMPUTE_HEIGHT / 2u) * BTBH_COMPUTE_WIDTH +
            (BTBH_COMPUTE_WIDTH / 2u)) * 4u;
    const uint16_t *compute_half = (const uint16_t *)compute_map;
    for (size_t channel = 0; channel < 4; ++channel) {
        report->compute_center_rgba[channel] = btbh_half_to_float(
            compute_half[compute_center_index + channel]
        );
    }

    if (!btbh_save_capture(
            config->capture_path,
            albedo_map,
            config->width,
            config->height,
            report
        )) {
        goto cleanup;
    }

    report->result = BTBH_GPU_OK;
    report->width = config->width;
    report->height = config->height;
    report->message[0] = '\0';
    result = 0;

cleanup:
    if (compute_map != NULL && device != NULL) {
        SDL_UnmapGPUTransferBuffer(device->gpu, compute_download);
    }
    if (albedo_map != NULL && device != NULL) {
        SDL_UnmapGPUTransferBuffer(device->gpu, albedo_download);
    }
    if (fence != NULL && device != NULL) {
        SDL_ReleaseGPUFence(device->gpu, fence);
    }
    if (command_buffer != NULL) {
        (void)SDL_CancelGPUCommandBuffer(command_buffer);
    }
    if (device != NULL) {
        if (compute_download != NULL) {
            SDL_ReleaseGPUTransferBuffer(device->gpu, compute_download);
        }
        if (albedo_download != NULL) {
            SDL_ReleaseGPUTransferBuffer(device->gpu, albedo_download);
        }
        if (compute_output != NULL) {
            SDL_ReleaseGPUTexture(device->gpu, compute_output);
        }
        if (depth != NULL) {
            SDL_ReleaseGPUTexture(device->gpu, depth);
        }
        if (emissive_hdr != NULL) {
            SDL_ReleaseGPUTexture(device->gpu, emissive_hdr);
        }
        if (normal_roughness != NULL) {
            SDL_ReleaseGPUTexture(device->gpu, normal_roughness);
        }
        if (albedo != NULL) {
            SDL_ReleaseGPUTexture(device->gpu, albedo);
        }
        if (graphics_pipeline != NULL) {
            SDL_ReleaseGPUGraphicsPipeline(
                device->gpu,
                graphics_pipeline
            );
        }
        if (compute_pipeline != NULL) {
            SDL_ReleaseGPUComputePipeline(device->gpu, compute_pipeline);
        }
        if (fragment_shader != NULL) {
            SDL_ReleaseGPUShader(device->gpu, fragment_shader);
        }
        if (vertex_shader != NULL) {
            SDL_ReleaseGPUShader(device->gpu, vertex_shader);
        }
    }
    btbh_gpu_device_destroy(device);
    free(compute_bytes.data);
    free(fragment_bytes.data);
    free(vertex_bytes.data);
    return result;
}
