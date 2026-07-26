#include "btbh_gpu_phase2_runtime.h"

#include "btbh_gpu/btbh_gpu_resources.h"
#include "btbh_gpu_internal.h"
#include "btbh_gpu_spike_scene.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BTBH_PHASE2_COMPUTE_WIDTH 64u
#define BTBH_PHASE2_COMPUTE_HEIGHT 64u
#define BTBH_PHASE2_SHADOW_SIZE 256u
#define BTBH_PHASE2_SHADER_COUNT 9u

typedef enum BTBHPhase2ShaderIndex {
    BTBH_PHASE2_SHADER_SHADOW_VERTEX = 0,
    BTBH_PHASE2_SHADER_SHADOW_FRAGMENT = 1,
    BTBH_PHASE2_SHADER_GBUFFER_VERTEX = 2,
    BTBH_PHASE2_SHADER_GBUFFER_FRAGMENT = 3,
    BTBH_PHASE2_SHADER_PATTERN_COMPUTE = 4,
    BTBH_PHASE2_SHADER_INTERVAL_COMPUTE = 5,
    BTBH_PHASE2_SHADER_LIGHTING_VERTEX = 6,
    BTBH_PHASE2_SHADER_LIGHTING_FRAGMENT = 7,
    BTBH_PHASE2_SHADER_TRANSPARENT_FRAGMENT = 8
} BTBHPhase2ShaderIndex;

typedef struct BTBHPhase2ShaderBytes {
    unsigned char *data;
    size_t size;
} BTBHPhase2ShaderBytes;

typedef struct BTBHPhase2ReleaseContext {
    SDL_GPUDevice *device;
    uint32_t released_count;
} BTBHPhase2ReleaseContext;

static void btbh_phase2_error(
    BTBHGPUPhase2Report *report,
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

static bool btbh_phase2_track_resource(
    BTBHGPUResourceRegistry *registry,
    BTBHGPUResourceKind kind,
    uintptr_t payload,
    BTBHGPUResourceHandle handles[30],
    BTBHGPUResourceKind kinds[30],
    uint32_t *handle_count,
    BTBHGPUPhase2Report *report
) {
    BTBHGPUResourceHandle handle = BTBH_GPU_INVALID_RESOURCE_HANDLE;
    uintptr_t verified_payload = 0u;
    if (registry == NULL ||
        payload == 0u ||
        handles == NULL ||
        kinds == NULL ||
        handle_count == NULL ||
        *handle_count >= 30u ||
        btbh_gpu_resource_insert(
            registry,
            kind,
            payload,
            &handle
        ) != BTBH_GPU_RESOURCE_OK ||
        btbh_gpu_resource_lookup(
            registry,
            handle,
            kind,
            &verified_payload
        ) != BTBH_GPU_RESOURCE_OK ||
        verified_payload != payload) {
        btbh_phase2_error(
            report,
            BTBH_GPU_PLATFORM_FAILURE,
            "Could not register a Phase 2 GPU resource"
        );
        return false;
    }
    handles[*handle_count] = handle;
    kinds[*handle_count] = kind;
    *handle_count += 1u;
    return true;
}

static void btbh_phase2_release_resource(
    BTBHGPUResourceKind kind,
    uintptr_t payload,
    void *context
) {
    BTBHPhase2ReleaseContext *release_context =
        (BTBHPhase2ReleaseContext *)context;
    if (release_context == NULL ||
        release_context->device == NULL ||
        payload == 0u) {
        return;
    }
    switch (kind) {
        case BTBH_GPU_RESOURCE_BUFFER:
            SDL_ReleaseGPUBuffer(
                release_context->device,
                (SDL_GPUBuffer *)payload
            );
            break;
        case BTBH_GPU_RESOURCE_TEXTURE:
            SDL_ReleaseGPUTexture(
                release_context->device,
                (SDL_GPUTexture *)payload
            );
            break;
        case BTBH_GPU_RESOURCE_SAMPLER:
            SDL_ReleaseGPUSampler(
                release_context->device,
                (SDL_GPUSampler *)payload
            );
            break;
        case BTBH_GPU_RESOURCE_SHADER:
            SDL_ReleaseGPUShader(
                release_context->device,
                (SDL_GPUShader *)payload
            );
            break;
        case BTBH_GPU_RESOURCE_GRAPHICS_PIPELINE:
            SDL_ReleaseGPUGraphicsPipeline(
                release_context->device,
                (SDL_GPUGraphicsPipeline *)payload
            );
            break;
        case BTBH_GPU_RESOURCE_COMPUTE_PIPELINE:
            SDL_ReleaseGPUComputePipeline(
                release_context->device,
                (SDL_GPUComputePipeline *)payload
            );
            break;
        case BTBH_GPU_RESOURCE_TRANSFER_BUFFER:
            SDL_ReleaseGPUTransferBuffer(
                release_context->device,
                (SDL_GPUTransferBuffer *)payload
            );
            break;
        case BTBH_GPU_RESOURCE_INVALID:
        default:
            return;
    }
    release_context->released_count += 1u;
}

static SDL_GPUShaderFormat btbh_phase2_shader_format(
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

static bool btbh_phase2_read_shader(
    const char *path,
    BTBHPhase2ShaderBytes *bytes,
    BTBHGPUPhase2Report *report
) {
    FILE *file = fopen(path, "rb");
    long size;

    if (file == NULL ||
        fseek(file, 0, SEEK_END) != 0 ||
        (size = ftell(file)) <= 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        if (file != NULL) {
            fclose(file);
        }
        btbh_phase2_error(report, BTBH_GPU_INVALID_ARGUMENT, path);
        return false;
    }
    bytes->data = (unsigned char *)malloc((size_t)size);
    if (bytes->data == NULL) {
        fclose(file);
        btbh_phase2_error(
            report,
            BTBH_GPU_PLATFORM_FAILURE,
            "Could not allocate Phase 2 shader bytes"
        );
        return false;
    }
    bytes->size = (size_t)size;
    if (fread(bytes->data, 1u, bytes->size, file) != bytes->size) {
        fclose(file);
        free(bytes->data);
        bytes->data = NULL;
        bytes->size = 0u;
        btbh_phase2_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            "Could not read a Phase 2 shader artifact"
        );
        return false;
    }
    fclose(file);
    return true;
}

static SDL_GPUShader *btbh_phase2_create_shader(
    SDL_GPUDevice *device,
    const BTBHPhase2ShaderBytes *bytes,
    const char *entrypoint,
    SDL_GPUShaderFormat format,
    SDL_GPUShaderStage stage,
    uint32_t samplers,
    uint32_t storage_textures,
    uint32_t storage_buffers,
    uint32_t uniform_buffers
) {
    SDL_GPUShaderCreateInfo info;
    SDL_zero(info);
    info.code = bytes->data;
    info.code_size = bytes->size;
    info.entrypoint = entrypoint;
    info.format = format;
    info.stage = stage;
    info.num_samplers = samplers;
    info.num_storage_textures = storage_textures;
    info.num_storage_buffers = storage_buffers;
    info.num_uniform_buffers = uniform_buffers;
    return SDL_CreateGPUShader(device, &info);
}

static SDL_GPUComputePipeline *btbh_phase2_create_compute_pipeline(
    SDL_GPUDevice *device,
    const BTBHPhase2ShaderBytes *bytes,
    const char *entrypoint,
    SDL_GPUShaderFormat format,
    uint32_t readonly_buffers
) {
    SDL_GPUComputePipelineCreateInfo info;
    SDL_zero(info);
    info.code = bytes->data;
    info.code_size = bytes->size;
    info.entrypoint = entrypoint;
    info.format = format;
    info.num_readonly_storage_buffers = readonly_buffers;
    info.num_readwrite_storage_textures = 1u;
    info.threadcount_x = 8u;
    info.threadcount_y = 8u;
    info.threadcount_z = 1u;
    return SDL_CreateGPUComputePipeline(device, &info);
}

static SDL_GPUTexture *btbh_phase2_create_texture(
    SDL_GPUDevice *device,
    SDL_GPUTextureFormat format,
    SDL_GPUTextureUsageFlags usage,
    uint32_t width,
    uint32_t height
) {
    SDL_GPUTextureCreateInfo info;
    SDL_zero(info);
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = format;
    info.usage = usage;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1u;
    info.num_levels = 1u;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    return SDL_CreateGPUTexture(device, &info);
}

static SDL_GPUBuffer *btbh_phase2_create_buffer(
    SDL_GPUDevice *device,
    SDL_GPUBufferUsageFlags usage,
    uint32_t size
) {
    SDL_GPUBufferCreateInfo info;
    SDL_zero(info);
    info.usage = usage;
    info.size = size;
    return SDL_CreateGPUBuffer(device, &info);
}

static SDL_GPUTransferBuffer *btbh_phase2_create_transfer(
    SDL_GPUDevice *device,
    SDL_GPUTransferBufferUsage usage,
    uint32_t size
) {
    SDL_GPUTransferBufferCreateInfo info;
    SDL_zero(info);
    info.usage = usage;
    info.size = size;
    return SDL_CreateGPUTransferBuffer(device, &info);
}

static SDL_GPUGraphicsPipeline *btbh_phase2_create_shadow_pipeline(
    SDL_GPUDevice *device,
    SDL_GPUShader *vertex_shader,
    SDL_GPUShader *fragment_shader
) {
    SDL_GPUGraphicsPipelineCreateInfo info;
    SDL_zero(info);
    info.vertex_shader = vertex_shader;
    info.fragment_shader = fragment_shader;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    info.depth_stencil_state.enable_depth_test = true;
    info.depth_stencil_state.enable_depth_write = true;
    info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    info.target_info.has_depth_stencil_target = true;
    return SDL_CreateGPUGraphicsPipeline(device, &info);
}

static SDL_GPUGraphicsPipeline *btbh_phase2_create_gbuffer_pipeline(
    SDL_GPUDevice *device,
    SDL_GPUShader *vertex_shader,
    SDL_GPUShader *fragment_shader
) {
    SDL_GPUColorTargetDescription targets[3];
    SDL_GPUGraphicsPipelineCreateInfo info;
    SDL_zeroa(targets);
    targets[0].format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;
    targets[1].format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    targets[2].format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    SDL_zero(info);
    info.vertex_shader = vertex_shader;
    info.fragment_shader = fragment_shader;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    info.depth_stencil_state.enable_depth_test = true;
    info.depth_stencil_state.enable_depth_write = true;
    info.target_info.color_target_descriptions = targets;
    info.target_info.num_color_targets = 3u;
    info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    info.target_info.has_depth_stencil_target = true;
    return SDL_CreateGPUGraphicsPipeline(device, &info);
}

static SDL_GPUGraphicsPipeline *btbh_phase2_create_color_pipeline(
    SDL_GPUDevice *device,
    SDL_GPUShader *vertex_shader,
    SDL_GPUShader *fragment_shader,
    bool transparent
) {
    SDL_GPUColorTargetDescription target;
    SDL_GPUGraphicsPipelineCreateInfo info;
    SDL_zero(target);
    target.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    if (transparent) {
        target.blend_state.enable_blend = true;
        target.blend_state.color_write_mask = 0x0fu;
        target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        target.blend_state.src_color_blendfactor =
            SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        target.blend_state.dst_color_blendfactor =
            SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        target.blend_state.src_alpha_blendfactor =
            SDL_GPU_BLENDFACTOR_ZERO;
        target.blend_state.dst_alpha_blendfactor =
            SDL_GPU_BLENDFACTOR_ONE;
    }
    SDL_zero(info);
    info.vertex_shader = vertex_shader;
    info.fragment_shader = fragment_shader;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.target_info.color_target_descriptions = &target;
    info.target_info.num_color_targets = 1u;
    return SDL_CreateGPUGraphicsPipeline(device, &info);
}

static float btbh_phase2_half_to_float(uint16_t value)
{
    const uint16_t sign = (uint16_t)(value >> 15u);
    const uint16_t exponent = (uint16_t)((value >> 10u) & 0x1fu);
    const uint16_t mantissa = (uint16_t)(value & 0x03ffu);
    float result;
    if (exponent == 0u) {
        result = mantissa == 0u ? 0.0f : ldexpf((float)mantissa, -24);
    } else if (exponent == 31u) {
        result = mantissa == 0u ? INFINITY : NAN;
    } else {
        result = ldexpf(
            1.0f + ((float)mantissa / 1024.0f),
            (int)exponent - 15
        );
    }
    return sign != 0u ? -result : result;
}

static bool btbh_phase2_save_capture(
    const char *path,
    void *rgba,
    uint32_t width,
    uint32_t height,
    BTBHGPUPhase2Report *report
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
        btbh_phase2_error(report, BTBH_GPU_PLATFORM_FAILURE, SDL_GetError());
        return false;
    }
    saved = SDL_SaveBMP(surface, path);
    SDL_DestroySurface(surface);
    if (!saved) {
        btbh_phase2_error(report, BTBH_GPU_PLATFORM_FAILURE, SDL_GetError());
    }
    return saved;
}

static void btbh_phase2_identity(float matrix[16])
{
    (void)memset(matrix, 0, sizeof(float) * 16u);
    matrix[0] = 1.0f;
    matrix[5] = 1.0f;
    matrix[10] = 1.0f;
    matrix[15] = 1.0f;
}

static uint64_t btbh_phase2_checksum(
    const uint8_t *bytes,
    size_t count
) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t index = 0u; index < count; ++index) {
        hash ^= (uint64_t)bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

int btbh_gpu_run_phase2(
    const BTBHGPUPhase2Config *config,
    BTBHGPUPhase2Report *report
) {
    BTBHPhase2ShaderBytes shader_bytes[BTBH_PHASE2_SHADER_COUNT] = { 0 };
    const char *shader_paths[BTBH_PHASE2_SHADER_COUNT];
    SDL_GPUShader *shaders[7] = { 0 };
    BTBHGPUDevice *device = NULL;
    BTBHGPUDeviceReport device_report;
    SDL_GPUComputePipeline *pattern_pipeline = NULL;
    SDL_GPUComputePipeline *interval_pipeline = NULL;
    SDL_GPUGraphicsPipeline *shadow_pipeline = NULL;
    SDL_GPUGraphicsPipeline *gbuffer_pipeline = NULL;
    SDL_GPUGraphicsPipeline *lighting_pipeline = NULL;
    SDL_GPUGraphicsPipeline *transparent_pipeline = NULL;
    SDL_GPUTexture *directional_shadow = NULL;
    SDL_GPUTexture *spot_shadow = NULL;
    SDL_GPUTexture *albedo = NULL;
    SDL_GPUTexture *normal_material = NULL;
    SDL_GPUTexture *emission = NULL;
    SDL_GPUTexture *depth = NULL;
    SDL_GPUTexture *pattern_output = NULL;
    SDL_GPUTexture *interval_output = NULL;
    SDL_GPUTexture *final_color = NULL;
    SDL_GPUBuffer *instance_buffer = NULL;
    SDL_GPUBuffer *bone_buffer = NULL;
    SDL_GPUBuffer *interval_buffer = NULL;
    SDL_GPUSampler *sampler = NULL;
    SDL_GPUSampler *shadow_sampler = NULL;
    SDL_GPUTransferBuffer *upload = NULL;
    SDL_GPUTransferBuffer *final_download = NULL;
    SDL_GPUTransferBuffer *interval_download = NULL;
    SDL_GPUCommandBuffer *command_buffer = NULL;
    SDL_GPUFence *fence = NULL;
    BTBHGPUResourceRegistry *resource_registry = NULL;
    BTBHGPUResourceHandle resource_handles[30] = { 0 };
    BTBHGPUResourceKind resource_kinds[30] = { 0 };
    BTBHPhase2ReleaseContext release_context = { 0 };
    uint32_t resource_handle_count = 0u;
    bool resources_registered = false;
    void *final_map = NULL;
    void *interval_map = NULL;
    BTBHGPUSceneFixture fixture;
    const BTBHGPUSceneContract *scene_contract =
        btbh_gpu_spike_scene_contract();
    uint32_t executed_passes = 0u;
    int result = 1;

    if (config == NULL || report == NULL) {
        return 1;
    }
    (void)memset(report, 0, sizeof(*report));
    shader_paths[0] = config->shadow_vertex_path;
    shader_paths[1] = config->shadow_fragment_path;
    shader_paths[2] = config->gbuffer_vertex_path;
    shader_paths[3] = config->gbuffer_fragment_path;
    shader_paths[4] = config->pattern_compute_path;
    shader_paths[5] = config->interval_compute_path;
    shader_paths[6] = config->lighting_vertex_path;
    shader_paths[7] = config->lighting_fragment_path;
    shader_paths[8] = config->transparent_fragment_path;
    if (config->shader_entrypoint == NULL ||
        config->width == 0u ||
        config->height == 0u ||
        ((uint64_t)config->width * config->height * 4u) > UINT32_MAX ||
        btbh_phase2_shader_format(config->shader_format) ==
            SDL_GPU_SHADERFORMAT_INVALID) {
        btbh_phase2_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            "Invalid Phase 2 runtime configuration"
        );
        goto cleanup;
    }
    if (scene_contract == NULL ||
        scene_contract->instance_count != BTBH_GPU_SCENE_INSTANCE_COUNT ||
        scene_contract->bone_count != BTBH_GPU_SCENE_BONE_COUNT ||
        btbh_gpu_spike_scene_build_fixture(&fixture) != BTBH_GPU_SCENE_OK) {
        btbh_phase2_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            "Invalid canonical Phase 2 scene fixture"
        );
        goto cleanup;
    }
    for (uint32_t index = 0u;
         index < BTBH_PHASE2_SHADER_COUNT;
         ++index) {
        if (shader_paths[index] == NULL ||
            !btbh_phase2_read_shader(
                shader_paths[index],
                &shader_bytes[index],
                report
            )) {
            goto cleanup;
        }
    }

    const BTBHGPUDeviceDescriptor descriptor = {
        config->backend,
        BTBH_GPU_PHASE2_REQUIRED_FEATURES,
        1u,
        { 0 }
    };
    device = btbh_gpu_device_create(&descriptor, &device_report);
    if (device == NULL) {
        btbh_phase2_error(report, device_report.result, device_report.message);
        goto cleanup;
    }
    (void)snprintf(
        report->driver_name,
        sizeof(report->driver_name),
        "%s",
        device_report.capabilities.driver_name
    );
    const SDL_GPUShaderFormat format =
        btbh_phase2_shader_format(config->shader_format);
    shaders[0] = btbh_phase2_create_shader(
        device->gpu,
        &shader_bytes[BTBH_PHASE2_SHADER_SHADOW_VERTEX],
        config->shader_entrypoint,
        format,
        SDL_GPU_SHADERSTAGE_VERTEX,
        0u,
        0u,
        0u,
        1u
    );
    shaders[1] = btbh_phase2_create_shader(
        device->gpu,
        &shader_bytes[BTBH_PHASE2_SHADER_SHADOW_FRAGMENT],
        config->shader_entrypoint,
        format,
        SDL_GPU_SHADERSTAGE_FRAGMENT,
        0u,
        0u,
        0u,
        0u
    );
    shaders[2] = btbh_phase2_create_shader(
        device->gpu,
        &shader_bytes[BTBH_PHASE2_SHADER_GBUFFER_VERTEX],
        config->shader_entrypoint,
        format,
        SDL_GPU_SHADERSTAGE_VERTEX,
        0u,
        0u,
        2u,
        1u
    );
    shaders[3] = btbh_phase2_create_shader(
        device->gpu,
        &shader_bytes[BTBH_PHASE2_SHADER_GBUFFER_FRAGMENT],
        config->shader_entrypoint,
        format,
        SDL_GPU_SHADERSTAGE_FRAGMENT,
        0u,
        0u,
        0u,
        0u
    );
    shaders[4] = btbh_phase2_create_shader(
        device->gpu,
        &shader_bytes[BTBH_PHASE2_SHADER_LIGHTING_VERTEX],
        config->shader_entrypoint,
        format,
        SDL_GPU_SHADERSTAGE_VERTEX,
        0u,
        0u,
        0u,
        0u
    );
    shaders[5] = btbh_phase2_create_shader(
        device->gpu,
        &shader_bytes[BTBH_PHASE2_SHADER_LIGHTING_FRAGMENT],
        config->shader_entrypoint,
        format,
        SDL_GPU_SHADERSTAGE_FRAGMENT,
        6u,
        0u,
        0u,
        0u
    );
    shaders[6] = btbh_phase2_create_shader(
        device->gpu,
        &shader_bytes[BTBH_PHASE2_SHADER_TRANSPARENT_FRAGMENT],
        config->shader_entrypoint,
        format,
        SDL_GPU_SHADERSTAGE_FRAGMENT,
        0u,
        0u,
        0u,
        0u
    );
    pattern_pipeline = btbh_phase2_create_compute_pipeline(
        device->gpu,
        &shader_bytes[BTBH_PHASE2_SHADER_PATTERN_COMPUTE],
        config->shader_entrypoint,
        format,
        0u
    );
    interval_pipeline = btbh_phase2_create_compute_pipeline(
        device->gpu,
        &shader_bytes[BTBH_PHASE2_SHADER_INTERVAL_COMPUTE],
        config->shader_entrypoint,
        format,
        1u
    );
    for (uint32_t index = 0u; index < 7u; ++index) {
        if (shaders[index] == NULL) {
            btbh_phase2_error(report, BTBH_GPU_DEVICE_FAILURE, SDL_GetError());
            goto cleanup;
        }
    }
    if (pattern_pipeline == NULL || interval_pipeline == NULL) {
        btbh_phase2_error(report, BTBH_GPU_DEVICE_FAILURE, SDL_GetError());
        goto cleanup;
    }

    shadow_pipeline = btbh_phase2_create_shadow_pipeline(
        device->gpu,
        shaders[0],
        shaders[1]
    );
    gbuffer_pipeline = btbh_phase2_create_gbuffer_pipeline(
        device->gpu,
        shaders[2],
        shaders[3]
    );
    lighting_pipeline = btbh_phase2_create_color_pipeline(
        device->gpu,
        shaders[4],
        shaders[5],
        false
    );
    transparent_pipeline = btbh_phase2_create_color_pipeline(
        device->gpu,
        shaders[4],
        shaders[6],
        true
    );
    if (shadow_pipeline == NULL ||
        gbuffer_pipeline == NULL ||
        lighting_pipeline == NULL ||
        transparent_pipeline == NULL) {
        btbh_phase2_error(report, BTBH_GPU_DEVICE_FAILURE, SDL_GetError());
        goto cleanup;
    }
    const SDL_GPUTextureUsageFlags sampled_color_usage =
        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    directional_shadow = btbh_phase2_create_texture(
        device->gpu,
        SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
        SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET |
            SDL_GPU_TEXTUREUSAGE_SAMPLER,
        BTBH_PHASE2_SHADOW_SIZE,
        BTBH_PHASE2_SHADOW_SIZE
    );
    spot_shadow = btbh_phase2_create_texture(
        device->gpu,
        SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
        SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET |
            SDL_GPU_TEXTUREUSAGE_SAMPLER,
        BTBH_PHASE2_SHADOW_SIZE,
        BTBH_PHASE2_SHADOW_SIZE
    );
    albedo = btbh_phase2_create_texture(
        device->gpu,
        SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB,
        sampled_color_usage,
        config->width,
        config->height
    );
    normal_material = btbh_phase2_create_texture(
        device->gpu,
        SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        sampled_color_usage,
        config->width,
        config->height
    );
    emission = btbh_phase2_create_texture(
        device->gpu,
        SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
        sampled_color_usage,
        config->width,
        config->height
    );
    depth = btbh_phase2_create_texture(
        device->gpu,
        SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
        SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET |
            SDL_GPU_TEXTUREUSAGE_SAMPLER,
        config->width,
        config->height
    );
    pattern_output = btbh_phase2_create_texture(
        device->gpu,
        SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
        SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE |
            SDL_GPU_TEXTUREUSAGE_SAMPLER,
        BTBH_PHASE2_COMPUTE_WIDTH,
        BTBH_PHASE2_COMPUTE_HEIGHT
    );
    interval_output = btbh_phase2_create_texture(
        device->gpu,
        SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
        SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE |
            SDL_GPU_TEXTUREUSAGE_SAMPLER,
        BTBH_PHASE2_COMPUTE_WIDTH,
        BTBH_PHASE2_COMPUTE_HEIGHT
    );
    final_color = btbh_phase2_create_texture(
        device->gpu,
        SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
            SDL_GPU_TEXTUREUSAGE_SAMPLER,
        config->width,
        config->height
    );
    if (directional_shadow == NULL ||
        spot_shadow == NULL ||
        albedo == NULL ||
        normal_material == NULL ||
        emission == NULL ||
        depth == NULL ||
        pattern_output == NULL ||
        interval_output == NULL ||
        final_color == NULL) {
        btbh_phase2_error(report, BTBH_GPU_DEVICE_FAILURE, SDL_GetError());
        goto cleanup;
    }

    const uint32_t instance_size = (uint32_t)sizeof(fixture.instances);
    const uint32_t bone_size = (uint32_t)sizeof(fixture.bones);
    const uint32_t interval_size = (uint32_t)sizeof(fixture.intervals);
    const uint32_t upload_size =
        instance_size + bone_size + interval_size;
    instance_buffer = btbh_phase2_create_buffer(
        device->gpu,
        SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ,
        instance_size
    );
    bone_buffer = btbh_phase2_create_buffer(
        device->gpu,
        SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ,
        bone_size
    );
    interval_buffer = btbh_phase2_create_buffer(
        device->gpu,
        SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,
        interval_size
    );
    upload = btbh_phase2_create_transfer(
        device->gpu,
        SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        upload_size
    );
    final_download = btbh_phase2_create_transfer(
        device->gpu,
        SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
        config->width * config->height * 4u
    );
    interval_download = btbh_phase2_create_transfer(
        device->gpu,
        SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
        BTBH_PHASE2_COMPUTE_WIDTH * BTBH_PHASE2_COMPUTE_HEIGHT * 8u
    );
    SDL_GPUSamplerCreateInfo sampler_info;
    SDL_zero(sampler_info);
    sampler_info.min_filter = SDL_GPU_FILTER_LINEAR;
    sampler_info.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampler_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler = SDL_CreateGPUSampler(device->gpu, &sampler_info);
    sampler_info.min_filter = SDL_GPU_FILTER_NEAREST;
    sampler_info.mag_filter = SDL_GPU_FILTER_NEAREST;
    shadow_sampler = SDL_CreateGPUSampler(device->gpu, &sampler_info);
    if (instance_buffer == NULL ||
        bone_buffer == NULL ||
        interval_buffer == NULL ||
        upload == NULL ||
        final_download == NULL ||
        interval_download == NULL ||
        sampler == NULL ||
        shadow_sampler == NULL) {
        btbh_phase2_error(report, BTBH_GPU_DEVICE_FAILURE, SDL_GetError());
        goto cleanup;
    }

    resource_registry = btbh_gpu_resource_registry_create(30u, 30u);
    if (resource_registry == NULL) {
        btbh_phase2_error(
            report,
            BTBH_GPU_PLATFORM_FAILURE,
            "Could not create the Phase 2 resource registry"
        );
        goto cleanup;
    }
#define BTBH_PHASE2_TRACK(kind, pointer) \
    btbh_phase2_track_resource( \
        resource_registry, \
        (kind), \
        (uintptr_t)(pointer), \
        resource_handles, \
        resource_kinds, \
        &resource_handle_count, \
        report \
    )
    if (!BTBH_PHASE2_TRACK(BTBH_GPU_RESOURCE_TRANSFER_BUFFER, upload) ||
        !BTBH_PHASE2_TRACK(
            BTBH_GPU_RESOURCE_TRANSFER_BUFFER,
            final_download
        ) ||
        !BTBH_PHASE2_TRACK(
            BTBH_GPU_RESOURCE_TRANSFER_BUFFER,
            interval_download
        ) ||
        !BTBH_PHASE2_TRACK(BTBH_GPU_RESOURCE_SAMPLER, sampler) ||
        !BTBH_PHASE2_TRACK(BTBH_GPU_RESOURCE_SAMPLER, shadow_sampler) ||
        !BTBH_PHASE2_TRACK(BTBH_GPU_RESOURCE_BUFFER, instance_buffer) ||
        !BTBH_PHASE2_TRACK(BTBH_GPU_RESOURCE_BUFFER, bone_buffer) ||
        !BTBH_PHASE2_TRACK(BTBH_GPU_RESOURCE_BUFFER, interval_buffer) ||
        !BTBH_PHASE2_TRACK(BTBH_GPU_RESOURCE_TEXTURE, final_color) ||
        !BTBH_PHASE2_TRACK(BTBH_GPU_RESOURCE_TEXTURE, interval_output) ||
        !BTBH_PHASE2_TRACK(BTBH_GPU_RESOURCE_TEXTURE, pattern_output) ||
        !BTBH_PHASE2_TRACK(BTBH_GPU_RESOURCE_TEXTURE, depth) ||
        !BTBH_PHASE2_TRACK(BTBH_GPU_RESOURCE_TEXTURE, emission) ||
        !BTBH_PHASE2_TRACK(BTBH_GPU_RESOURCE_TEXTURE, normal_material) ||
        !BTBH_PHASE2_TRACK(BTBH_GPU_RESOURCE_TEXTURE, albedo) ||
        !BTBH_PHASE2_TRACK(BTBH_GPU_RESOURCE_TEXTURE, spot_shadow) ||
        !BTBH_PHASE2_TRACK(BTBH_GPU_RESOURCE_TEXTURE, directional_shadow) ||
        !BTBH_PHASE2_TRACK(
            BTBH_GPU_RESOURCE_GRAPHICS_PIPELINE,
            transparent_pipeline
        ) ||
        !BTBH_PHASE2_TRACK(
            BTBH_GPU_RESOURCE_GRAPHICS_PIPELINE,
            lighting_pipeline
        ) ||
        !BTBH_PHASE2_TRACK(
            BTBH_GPU_RESOURCE_GRAPHICS_PIPELINE,
            gbuffer_pipeline
        ) ||
        !BTBH_PHASE2_TRACK(
            BTBH_GPU_RESOURCE_GRAPHICS_PIPELINE,
            shadow_pipeline
        ) ||
        !BTBH_PHASE2_TRACK(
            BTBH_GPU_RESOURCE_COMPUTE_PIPELINE,
            interval_pipeline
        ) ||
        !BTBH_PHASE2_TRACK(
            BTBH_GPU_RESOURCE_COMPUTE_PIPELINE,
            pattern_pipeline
        )) {
        goto cleanup;
    }
    for (uint32_t index = 0u; index < 7u; ++index) {
        if (!BTBH_PHASE2_TRACK(BTBH_GPU_RESOURCE_SHADER, shaders[index])) {
            goto cleanup;
        }
    }
#undef BTBH_PHASE2_TRACK
    resources_registered = true;
    report->tracked_resource_count = resource_handle_count;
    void *upload_map = SDL_MapGPUTransferBuffer(
        device->gpu,
        upload,
        false
    );
    if (upload_map == NULL) {
        btbh_phase2_error(report, BTBH_GPU_DEVICE_FAILURE, SDL_GetError());
        goto cleanup;
    }
    (void)memcpy(upload_map, fixture.instances, instance_size);
    (void)memcpy(
        (uint8_t *)upload_map + instance_size,
        fixture.bones,
        bone_size
    );
    (void)memcpy(
        (uint8_t *)upload_map + instance_size + bone_size,
        fixture.intervals,
        interval_size
    );
    SDL_UnmapGPUTransferBuffer(device->gpu, upload);

    command_buffer = SDL_AcquireGPUCommandBuffer(device->gpu);
    if (command_buffer == NULL) {
        btbh_phase2_error(report, BTBH_GPU_DEVICE_FAILURE, SDL_GetError());
        goto cleanup;
    }

    SDL_GPUCopyPass *copy_pass = SDL_BeginGPUCopyPass(command_buffer);
    if (copy_pass == NULL) {
        btbh_phase2_error(report, BTBH_GPU_DEVICE_FAILURE, SDL_GetError());
        goto cleanup;
    }
    SDL_GPUTransferBufferLocation upload_location;
    SDL_GPUBufferRegion upload_region;
    SDL_zero(upload_location);
    SDL_zero(upload_region);
    upload_location.transfer_buffer = upload;
    upload_region.buffer = instance_buffer;
    upload_region.size = instance_size;
    SDL_UploadToGPUBuffer(
        copy_pass,
        &upload_location,
        &upload_region,
        false
    );
    upload_location.offset = instance_size;
    upload_region.buffer = bone_buffer;
    upload_region.size = bone_size;
    SDL_UploadToGPUBuffer(
        copy_pass,
        &upload_location,
        &upload_region,
        false
    );
    upload_location.offset = instance_size + bone_size;
    upload_region.buffer = interval_buffer;
    upload_region.size = interval_size;
    SDL_UploadToGPUBuffer(
        copy_pass,
        &upload_location,
        &upload_region,
        false
    );
    SDL_EndGPUCopyPass(copy_pass);
    executed_passes += 1u;
    float identity[16];
    btbh_phase2_identity(identity);
    float shadow_matrices[2][16];
    btbh_phase2_identity(shadow_matrices[0]);
    btbh_phase2_identity(shadow_matrices[1]);
    shadow_matrices[1][0] = 0.60f;
    shadow_matrices[1][5] = 0.60f;
    SDL_GPUTexture *shadow_textures[2] = {
        directional_shadow,
        spot_shadow
    };
    for (uint32_t index = 0u; index < 2u; ++index) {
        SDL_GPUDepthStencilTargetInfo shadow_target;
        SDL_zero(shadow_target);
        shadow_target.texture = shadow_textures[index];
        shadow_target.clear_depth = 1.0f;
        shadow_target.load_op = SDL_GPU_LOADOP_CLEAR;
        shadow_target.store_op = SDL_GPU_STOREOP_STORE;
        shadow_target.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        shadow_target.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
        SDL_PushGPUVertexUniformData(
            command_buffer,
            0u,
            shadow_matrices[index],
            sizeof(shadow_matrices[index])
        );
        SDL_GPURenderPass *shadow_pass = SDL_BeginGPURenderPass(
            command_buffer,
            NULL,
            0u,
            &shadow_target
        );
        if (shadow_pass == NULL) {
            btbh_phase2_error(
                report,
                BTBH_GPU_DEVICE_FAILURE,
                SDL_GetError()
            );
            goto cleanup;
        }
        SDL_BindGPUGraphicsPipeline(shadow_pass, shadow_pipeline);
        SDL_DrawGPUPrimitives(shadow_pass, 3u, 1u, 0u, 0u);
        SDL_EndGPURenderPass(shadow_pass);
        executed_passes += 1u;
    }

    SDL_GPUColorTargetInfo gbuffer_targets[3];
    SDL_GPUDepthStencilTargetInfo depth_target;
    SDL_zeroa(gbuffer_targets);
    gbuffer_targets[0].texture = albedo;
    gbuffer_targets[1].texture = normal_material;
    gbuffer_targets[2].texture = emission;
    for (uint32_t index = 0u; index < 3u; ++index) {
        gbuffer_targets[index].load_op = SDL_GPU_LOADOP_CLEAR;
        gbuffer_targets[index].store_op = SDL_GPU_STOREOP_STORE;
        gbuffer_targets[index].clear_color.a = 1.0f;
    }
    SDL_zero(depth_target);
    depth_target.texture = depth;
    depth_target.clear_depth = 1.0f;
    depth_target.load_op = SDL_GPU_LOADOP_CLEAR;
    depth_target.store_op = SDL_GPU_STOREOP_STORE;
    depth_target.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth_target.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPUBuffer *vertex_storage[2] = {
        instance_buffer,
        bone_buffer
    };
    const uint32_t draw_instance_counts[3] = {
        1u,
        scene_contract->instance_count - 2u,
        1u
    };
    const uint32_t first_instances[3] = {
        0u,
        1u,
        scene_contract->instance_count - 1u
    };
    for (uint32_t draw = 0u; draw < 3u; ++draw) {
        SDL_PushGPUVertexUniformData(
            command_buffer,
            0u,
            identity,
            sizeof(identity)
        );
        SDL_GPURenderPass *gbuffer_pass = SDL_BeginGPURenderPass(
            command_buffer,
            gbuffer_targets,
            3u,
            &depth_target
        );
        if (gbuffer_pass == NULL) {
            btbh_phase2_error(
                report,
                BTBH_GPU_DEVICE_FAILURE,
                SDL_GetError()
            );
            goto cleanup;
        }
        SDL_BindGPUGraphicsPipeline(gbuffer_pass, gbuffer_pipeline);
        SDL_BindGPUVertexStorageBuffers(
            gbuffer_pass,
            0u,
            vertex_storage,
            2u
        );
        SDL_DrawGPUPrimitives(
            gbuffer_pass,
            3u,
            draw_instance_counts[draw],
            0u,
            first_instances[draw]
        );
        SDL_EndGPURenderPass(gbuffer_pass);
        executed_passes += 1u;
        for (uint32_t index = 0u; index < 3u; ++index) {
            gbuffer_targets[index].load_op = SDL_GPU_LOADOP_LOAD;
        }
        depth_target.load_op = SDL_GPU_LOADOP_LOAD;
    }

    SDL_GPUStorageTextureReadWriteBinding pattern_binding;
    SDL_zero(pattern_binding);
    pattern_binding.texture = pattern_output;
    SDL_GPUComputePass *pattern_pass = SDL_BeginGPUComputePass(
        command_buffer,
        &pattern_binding,
        1u,
        NULL,
        0u
    );
    if (pattern_pass == NULL) {
        btbh_phase2_error(report, BTBH_GPU_DEVICE_FAILURE, SDL_GetError());
        goto cleanup;
    }
    SDL_BindGPUComputePipeline(pattern_pass, pattern_pipeline);
    SDL_DispatchGPUCompute(pattern_pass, 8u, 8u, 1u);
    SDL_EndGPUComputePass(pattern_pass);
    executed_passes += 1u;

    SDL_GPUStorageTextureReadWriteBinding interval_binding;
    SDL_zero(interval_binding);
    interval_binding.texture = interval_output;
    SDL_GPUComputePass *interval_pass = SDL_BeginGPUComputePass(
        command_buffer,
        &interval_binding,
        1u,
        NULL,
        0u
    );
    if (interval_pass == NULL) {
        btbh_phase2_error(report, BTBH_GPU_DEVICE_FAILURE, SDL_GetError());
        goto cleanup;
    }
    SDL_BindGPUComputePipeline(interval_pass, interval_pipeline);
    SDL_BindGPUComputeStorageBuffers(
        interval_pass,
        0u,
        &interval_buffer,
        1u
    );
    SDL_DispatchGPUCompute(interval_pass, 8u, 8u, 1u);
    SDL_EndGPUComputePass(interval_pass);
    executed_passes += 1u;

    SDL_GPUColorTargetInfo final_target;
    SDL_zero(final_target);
    final_target.texture = final_color;
    final_target.load_op = SDL_GPU_LOADOP_CLEAR;
    final_target.store_op = SDL_GPU_STOREOP_STORE;
    final_target.clear_color.a = 1.0f;
    SDL_GPURenderPass *lighting_pass = SDL_BeginGPURenderPass(
        command_buffer,
        &final_target,
        1u,
        NULL
    );
    if (lighting_pass == NULL) {
        btbh_phase2_error(report, BTBH_GPU_DEVICE_FAILURE, SDL_GetError());
        goto cleanup;
    }
    SDL_GPUTextureSamplerBinding lighting_bindings[6];
    SDL_zeroa(lighting_bindings);
    lighting_bindings[0].texture = albedo;
    lighting_bindings[1].texture = normal_material;
    lighting_bindings[2].texture = emission;
    lighting_bindings[3].texture = pattern_output;
    lighting_bindings[4].texture = directional_shadow;
    lighting_bindings[5].texture = spot_shadow;
    for (uint32_t index = 0u; index < 4u; ++index) {
        lighting_bindings[index].sampler = sampler;
    }
    lighting_bindings[4].sampler = shadow_sampler;
    lighting_bindings[5].sampler = shadow_sampler;
    SDL_BindGPUGraphicsPipeline(lighting_pass, lighting_pipeline);
    SDL_BindGPUFragmentSamplers(
        lighting_pass,
        0u,
        lighting_bindings,
        6u
    );
    SDL_DrawGPUPrimitives(lighting_pass, 3u, 1u, 0u, 0u);
    SDL_EndGPURenderPass(lighting_pass);
    executed_passes += 1u;

    final_target.load_op = SDL_GPU_LOADOP_LOAD;
    SDL_GPURenderPass *transparent_pass = SDL_BeginGPURenderPass(
        command_buffer,
        &final_target,
        1u,
        NULL
    );
    if (transparent_pass == NULL) {
        btbh_phase2_error(report, BTBH_GPU_DEVICE_FAILURE, SDL_GetError());
        goto cleanup;
    }
    SDL_BindGPUGraphicsPipeline(transparent_pass, transparent_pipeline);
    SDL_DrawGPUPrimitives(transparent_pass, 3u, 1u, 0u, 0u);
    SDL_EndGPURenderPass(transparent_pass);
    executed_passes += 1u;

    SDL_GPUTextureRegion final_region;
    SDL_GPUTextureTransferInfo final_transfer;
    SDL_GPUTextureRegion interval_region;
    SDL_GPUTextureTransferInfo interval_transfer;
    SDL_zero(final_region);
    SDL_zero(final_transfer);
    SDL_zero(interval_region);
    SDL_zero(interval_transfer);
    final_region.texture = final_color;
    final_region.w = config->width;
    final_region.h = config->height;
    final_region.d = 1u;
    final_transfer.transfer_buffer = final_download;
    interval_region.texture = interval_output;
    interval_region.w = BTBH_PHASE2_COMPUTE_WIDTH;
    interval_region.h = BTBH_PHASE2_COMPUTE_HEIGHT;
    interval_region.d = 1u;
    interval_transfer.transfer_buffer = interval_download;
    copy_pass = SDL_BeginGPUCopyPass(command_buffer);
    if (copy_pass == NULL) {
        btbh_phase2_error(report, BTBH_GPU_DEVICE_FAILURE, SDL_GetError());
        goto cleanup;
    }
    SDL_DownloadFromGPUTexture(copy_pass, &final_region, &final_transfer);
    SDL_DownloadFromGPUTexture(
        copy_pass,
        &interval_region,
        &interval_transfer
    );
    SDL_EndGPUCopyPass(copy_pass);
    executed_passes += 1u;

    const uint64_t submit_started = SDL_GetTicksNS();
    fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command_buffer);
    report->cpu_submit_nanoseconds = SDL_GetTicksNS() - submit_started;
    command_buffer = NULL;
    if (fence == NULL) {
        btbh_phase2_error(report, BTBH_GPU_DEVICE_FAILURE, SDL_GetError());
        goto cleanup;
    }
    const uint64_t wait_started = SDL_GetTicksNS();
    const bool wait_succeeded =
        SDL_WaitForGPUFences(device->gpu, true, &fence, 1u);
    report->gpu_wait_nanoseconds = SDL_GetTicksNS() - wait_started;
    if (!wait_succeeded) {
        btbh_phase2_error(report, BTBH_GPU_DEVICE_FAILURE, SDL_GetError());
        goto cleanup;
    }
    final_map = SDL_MapGPUTransferBuffer(
        device->gpu,
        final_download,
        false
    );
    interval_map = SDL_MapGPUTransferBuffer(
        device->gpu,
        interval_download,
        false
    );
    if (final_map == NULL || interval_map == NULL) {
        btbh_phase2_error(report, BTBH_GPU_DEVICE_FAILURE, SDL_GetError());
        goto cleanup;
    }
    const size_t center_index =
        ((size_t)(config->height / 2u) * config->width +
         config->width / 2u) * 4u;
    (void)memcpy(
        report->center_rgba,
        (const uint8_t *)final_map + center_index,
        4u
    );
    const size_t directional_shadow_index =
        ((((size_t)config->height * 7u) / 10u) * config->width +
         config->width / 2u) * 4u;
    (void)memcpy(
        report->directional_shadow_rgba,
        (const uint8_t *)final_map + directional_shadow_index,
        4u
    );
    const size_t skinned_index =
        ((((size_t)config->height * 11u) / 12u) * config->width +
         ((size_t)config->width * 25u) / 32u) * 4u;
    (void)memcpy(
        report->skinned_rgba,
        (const uint8_t *)final_map + skinned_index,
        4u
    );
    report->final_image_checksum = btbh_phase2_checksum(
        (const uint8_t *)final_map,
        (size_t)config->width * config->height * 4u
    );
    const size_t interval_center =
        ((size_t)(BTBH_PHASE2_COMPUTE_HEIGHT / 2u) *
         BTBH_PHASE2_COMPUTE_WIDTH +
         BTBH_PHASE2_COMPUTE_WIDTH / 2u) * 4u;
    const uint16_t *interval_half = (const uint16_t *)interval_map;
    for (uint32_t channel = 0u; channel < 4u; ++channel) {
        report->interval_center_rgba[channel] = btbh_phase2_half_to_float(
            interval_half[interval_center + channel]
        );
    }
    if (config->capture_path != NULL &&
        !btbh_phase2_save_capture(
            config->capture_path,
            final_map,
            config->width,
            config->height,
            report
        )) {
        goto cleanup;
    }
    if (executed_passes != scene_contract->pass_count) {
        btbh_phase2_error(
            report,
            BTBH_GPU_PLATFORM_FAILURE,
            "Canonical Phase 2 pass count was not executed"
        );
        goto cleanup;
    }
    report->result = BTBH_GPU_OK;
    report->width = config->width;
    report->height = config->height;
    report->executed_pass_count = executed_passes;
    report->scene_signature =
        btbh_gpu_spike_scene_fixture_signature(&fixture);
    report->message[0] = '\0';
    result = 0;

cleanup:
    if (interval_map != NULL && device != NULL) {
        SDL_UnmapGPUTransferBuffer(device->gpu, interval_download);
    }
    if (final_map != NULL && device != NULL) {
        SDL_UnmapGPUTransferBuffer(device->gpu, final_download);
    }
    if (fence != NULL && device != NULL) {
        SDL_ReleaseGPUFence(device->gpu, fence);
    }
    if (command_buffer != NULL) {
        (void)SDL_CancelGPUCommandBuffer(command_buffer);
    }
    if (resources_registered && resource_registry != NULL && device != NULL) {
        bool retirement_ok = true;
        release_context.device = device->gpu;
        for (uint32_t index = 0u;
             index < resource_handle_count;
             ++index) {
            if (btbh_gpu_resource_retire(
                    resource_registry,
                    resource_handles[index],
                    resource_kinds[index],
                    1u
                ) != BTBH_GPU_RESOURCE_OK) {
                retirement_ok = false;
                break;
            }
        }
        if (retirement_ok) {
            report->deferred_release_count = btbh_gpu_resource_collect(
                resource_registry,
                1u,
                btbh_phase2_release_resource,
                &release_context
            );
            retirement_ok =
                report->deferred_release_count == resource_handle_count &&
                release_context.released_count == resource_handle_count;
        }
        if (!retirement_ok) {
            btbh_phase2_error(
                report,
                BTBH_GPU_PLATFORM_FAILURE,
                "Could not retire Phase 2 GPU resources"
            );
            result = 1;
        }
    }
    if (device != NULL && !resources_registered) {
        if (shadow_sampler != NULL) {
            SDL_ReleaseGPUSampler(device->gpu, shadow_sampler);
        }
        if (interval_download != NULL) {
            SDL_ReleaseGPUTransferBuffer(device->gpu, interval_download);
        }
        if (final_download != NULL) {
            SDL_ReleaseGPUTransferBuffer(device->gpu, final_download);
        }
        if (upload != NULL) {
            SDL_ReleaseGPUTransferBuffer(device->gpu, upload);
        }
        if (sampler != NULL) {
            SDL_ReleaseGPUSampler(device->gpu, sampler);
        }
        if (interval_buffer != NULL) {
            SDL_ReleaseGPUBuffer(device->gpu, interval_buffer);
        }
        if (bone_buffer != NULL) {
            SDL_ReleaseGPUBuffer(device->gpu, bone_buffer);
        }
        if (instance_buffer != NULL) {
            SDL_ReleaseGPUBuffer(device->gpu, instance_buffer);
        }
        SDL_GPUTexture *textures[] = {
            final_color,
            interval_output,
            pattern_output,
            depth,
            emission,
            normal_material,
            albedo,
            spot_shadow,
            directional_shadow
        };
        for (uint32_t index = 0u;
             index < sizeof(textures) / sizeof(textures[0]);
             ++index) {
            if (textures[index] != NULL) {
                SDL_ReleaseGPUTexture(device->gpu, textures[index]);
            }
        }
        SDL_GPUGraphicsPipeline *graphics_pipelines[] = {
            transparent_pipeline,
            lighting_pipeline,
            gbuffer_pipeline,
            shadow_pipeline
        };
        for (uint32_t index = 0u;
             index < sizeof(graphics_pipelines) /
                sizeof(graphics_pipelines[0]);
             ++index) {
            if (graphics_pipelines[index] != NULL) {
                SDL_ReleaseGPUGraphicsPipeline(
                    device->gpu,
                    graphics_pipelines[index]
                );
            }
        }
        if (interval_pipeline != NULL) {
            SDL_ReleaseGPUComputePipeline(device->gpu, interval_pipeline);
        }
        if (pattern_pipeline != NULL) {
            SDL_ReleaseGPUComputePipeline(device->gpu, pattern_pipeline);
        }
        for (uint32_t index = 0u; index < 7u; ++index) {
            if (shaders[index] != NULL) {
                SDL_ReleaseGPUShader(device->gpu, shaders[index]);
            }
        }
    }
    if (resource_registry != NULL) {
        btbh_gpu_resource_registry_destroy(
            resource_registry,
            resources_registered ? btbh_phase2_release_resource : NULL,
            resources_registered ? &release_context : NULL
        );
        resource_registry = NULL;
    }
    btbh_gpu_device_destroy(device);
    for (uint32_t index = 0u;
         index < BTBH_PHASE2_SHADER_COUNT;
         ++index) {
        free(shader_bytes[index].data);
    }
    return result;
}
