#include "btbh_gpu_fsr3.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Thread-group widths of the passes (their [numthreads]). */
#define BTBH_GPU_FSR3_TILE 8u
#define BTBH_GPU_FSR3_SPD_TILE 64u
#define BTBH_GPU_FSR3_SPD_STORED_LEVELS 6u
#define BTBH_GPU_FSR3_MINIMUM_RENDER_SIZE 64u

typedef struct BTBHGPUFSR3SPDConstants {
    uint32_t mips;
    uint32_t work_group_count;
    uint32_t work_group_offset[2];
    uint32_t render_size[2];
    uint32_t padding[2];
} BTBHGPUFSR3SPDConstants;

typedef struct BTBHGPUFSR3ClearConstants {
    uint32_t value_extent[4];
} BTBHGPUFSR3ClearConstants;

typedef struct BTBHGPUFSR3MotionViewConstants {
    float output_metrics[4];
    float motion_metrics[4];
} BTBHGPUFSR3MotionViewConstants;

typedef struct BTBHGPUFSR3PipelineSpec {
    const char *name;
    uint32_t samplers;
    uint32_t readwrite_textures;
    uint32_t uniform_buffers;
    uint32_t threads_x;
    uint32_t threads_y;
} BTBHGPUFSR3PipelineSpec;

/* Bindings as numbered in shaders/gpu/production_fsr3_*.comp.hlsl. */
static const BTBHGPUFSR3PipelineSpec btbh_gpu_fsr3_pipeline_specs[
    BTBH_GPU_FSR3_PIPELINE_COUNT
] = {
    {"production_fsr3_prepare_inputs.comp", 3u, 5u, 1u, 8u, 8u},
    /* Only mip 5 of the pyramid: SDL GPU binds at most 8 read-write textures per compute pass. */
    {"production_fsr3_luma_pyramid.comp", 2u, 4u, 2u, 256u, 1u},
    {"production_fsr3_shading_change_pyramid.comp", 4u, 7u, 2u, 256u, 1u},
    {"production_fsr3_shading_change.comp", 1u, 1u, 1u, 8u, 8u},
    {"production_fsr3_prepare_reactivity.comp", 9u, 3u, 1u, 8u, 8u},
    {"production_fsr3_luma_instability.comp", 7u, 2u, 1u, 8u, 8u},
    {"production_fsr3_accumulate.comp", 8u, 3u, 1u, 8u, 8u},
    {"production_fsr3_debug_view.comp", 5u, 1u, 1u, 8u, 8u},
    {"production_fsr3_clear.comp", 0u, 1u, 1u, 8u, 8u},
    {"production_fsr3_motion_view.comp", 4u, 1u, 1u, 8u, 8u},
};

static void btbh_gpu_fsr3_message(
    char *message,
    size_t capacity,
    const char *prefix,
    const char *detail
) {
    if (message == NULL || capacity == 0u) {
        return;
    }
    (void)snprintf(
        message,
        capacity,
        "%s%s%s",
        prefix,
        detail != NULL && detail[0] != '\0' ? ": " : "",
        detail != NULL ? detail : ""
    );
}

static bool btbh_gpu_fsr3_sibling_path(
    const char *reference,
    const char *filename,
    char *path,
    size_t capacity
) {
    const char *slash = strrchr(reference, '/');
    const char *backslash = strrchr(reference, '\\');
    const char *separator = slash;
    if (backslash != NULL && (separator == NULL || backslash > separator)) {
        separator = backslash;
    }
    const size_t directory_length = separator != NULL
        ? (size_t)(separator - reference + 1)
        : 0u;
    const size_t filename_length = strlen(filename);
    if (directory_length + filename_length + 1u > capacity) {
        return false;
    }
    (void)memcpy(path, reference, directory_length);
    (void)memcpy(path + directory_length, filename, filename_length + 1u);
    return true;
}

static SDL_GPUComputePipeline *btbh_gpu_fsr3_create_pipeline(
    SDL_GPUDevice *device,
    const char *shader_reference_path,
    BTBHGPUShaderFormat shader_format,
    const BTBHGPUFSR3PipelineSpec *spec,
    char *message,
    size_t message_capacity
) {
    const bool metallib = shader_format == BTBH_GPU_SHADER_FORMAT_METALLIB;
    char filename[128];
    (void)snprintf(
        filename,
        sizeof(filename),
        "%s.%s",
        spec->name,
        metallib ? "metallib" : "spv"
    );
    char path[1024];
    if (!btbh_gpu_fsr3_sibling_path(
            shader_reference_path,
            filename,
            path,
            sizeof(path)
        )) {
        btbh_gpu_fsr3_message(message, message_capacity, "FSR3 shader path is too long", filename);
        return NULL;
    }
    size_t size = 0u;
    void *bytes = SDL_LoadFile(path, &size);
    if (bytes == NULL || size == 0u) {
        SDL_free(bytes);
        btbh_gpu_fsr3_message(message, message_capacity, "FSR3 shader is missing", path);
        return NULL;
    }
    SDL_GPUComputePipelineCreateInfo info;
    SDL_zero(info);
    info.code = bytes;
    info.code_size = size;
    info.entrypoint = metallib ? "main0" : "main";
    info.format = metallib
        ? SDL_GPU_SHADERFORMAT_METALLIB
        : SDL_GPU_SHADERFORMAT_SPIRV;
    info.num_samplers = spec->samplers;
    info.num_readwrite_storage_textures = spec->readwrite_textures;
    info.num_uniform_buffers = spec->uniform_buffers;
    info.threadcount_x = spec->threads_x;
    info.threadcount_y = spec->threads_y;
    info.threadcount_z = 1u;
    SDL_GPUComputePipeline *pipeline = SDL_CreateGPUComputePipeline(device, &info);
    SDL_free(bytes);
    if (pipeline == NULL) {
        btbh_gpu_fsr3_message(message, message_capacity, spec->name, SDL_GetError());
    }
    return pipeline;
}

static void btbh_gpu_fsr3_release_texture(
    SDL_GPUDevice *device,
    SDL_GPUTexture **texture
) {
    if (*texture != NULL) {
        SDL_ReleaseGPUTexture(device, *texture);
        *texture = NULL;
    }
}

static void btbh_gpu_fsr3_release_render_resources(
    SDL_GPUDevice *device,
    BTBHGPUFSR3Runtime *runtime
) {
    btbh_gpu_fsr3_release_texture(device, &runtime->dilated_depth);
    btbh_gpu_fsr3_release_texture(device, &runtime->dilated_motion_vectors);
    btbh_gpu_fsr3_release_texture(device, &runtime->reconstructed_previous_depth);
    btbh_gpu_fsr3_release_texture(device, &runtime->intermediate);
    for (uint32_t index = 0u; index < 2u; ++index) {
        btbh_gpu_fsr3_release_texture(device, &runtime->luma[index]);
        btbh_gpu_fsr3_release_texture(device, &runtime->accumulation[index]);
        btbh_gpu_fsr3_release_texture(device, &runtime->luma_history[index]);
    }
    btbh_gpu_fsr3_release_texture(device, &runtime->dilated_reactive_masks);
    btbh_gpu_fsr3_release_texture(device, &runtime->shading_change);
    btbh_gpu_fsr3_release_texture(device, &runtime->spd_mips);
    btbh_gpu_fsr3_release_texture(device, &runtime->farthest_depth_mip1);
    runtime->render_capacity_width = 0u;
    runtime->render_capacity_height = 0u;
    runtime->spd_levels = 0u;
}

static void btbh_gpu_fsr3_release_output_resources(
    SDL_GPUDevice *device,
    BTBHGPUFSR3Runtime *runtime
) {
    for (uint32_t index = 0u; index < 2u; ++index) {
        btbh_gpu_fsr3_release_texture(device, &runtime->internal_upscaled[index]);
    }
    btbh_gpu_fsr3_release_texture(device, &runtime->new_locks);
    btbh_gpu_fsr3_release_texture(device, &runtime->output);
    runtime->output_width = 0u;
    runtime->output_height = 0u;
}

static SDL_GPUTexture *btbh_gpu_fsr3_create_texture(
    SDL_GPUDevice *device,
    SDL_GPUTextureFormat format,
    uint32_t width,
    uint32_t height,
    uint32_t levels,
    SDL_GPUTextureUsageFlags usage
) {
    SDL_GPUTextureCreateInfo info;
    SDL_zero(info);
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = format;
    info.usage = usage;
    info.width = width > 0u ? width : 1u;
    info.height = height > 0u ? height : 1u;
    info.layer_count_or_depth = 1u;
    info.num_levels = levels;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    return SDL_CreateGPUTexture(device, &info);
}

/* Every format and use the passes need, so an unsupported device keeps FSR 1 instead of failing a
   frame. Read-write formats past Vulkan's required set (R16F, RG16F, R8) need the device's extended
   storage formats, which AMD's own Vulkan backend needs too. */
static bool btbh_gpu_fsr3_formats_supported(SDL_GPUDevice *device) {
    const SDL_GPUTextureUsageFlags storage =
        SDL_GPU_TEXTUREUSAGE_SAMPLER |
        SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
    const SDL_GPUTextureUsageFlags simultaneous =
        SDL_GPU_TEXTUREUSAGE_SAMPLER |
        SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE;
    const struct {
        SDL_GPUTextureFormat format;
        SDL_GPUTextureUsageFlags usage;
    } checks[] = {
        {SDL_GPU_TEXTUREFORMAT_R32_FLOAT, storage},
        {SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT, storage | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET},
        {SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT, simultaneous},
        {SDL_GPU_TEXTUREFORMAT_R32_UINT, simultaneous},
        {SDL_GPU_TEXTUREFORMAT_R16_FLOAT, storage | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET},
        {SDL_GPU_TEXTUREFORMAT_R8_UNORM, storage | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET},
        {SDL_GPU_TEXTUREFORMAT_R8_UNORM, simultaneous},
        {SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, storage},
        {SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, storage},
        {SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT, simultaneous | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET},
        {SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT, SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET},
    };
    for (size_t index = 0u; index < sizeof(checks) / sizeof(checks[0]); ++index) {
        if (!SDL_GPUTextureSupportsFormat(
                device,
                checks[index].format,
                SDL_GPU_TEXTURETYPE_2D,
                checks[index].usage
            )) {
            return false;
        }
    }
    return true;
}

static uint32_t btbh_gpu_fsr3_level_count(uint32_t width, uint32_t height) {
    uint32_t levels = 1u;
    uint32_t size = width > height ? width : height;
    while (size > 1u) {
        size >>= 1u;
        levels += 1u;
    }
    return levels;
}

static bool btbh_gpu_fsr3_ensure_render_resources(
    SDL_GPUDevice *device,
    BTBHGPUFSR3Runtime *runtime,
    uint32_t render_width,
    uint32_t render_height
) {
    if (runtime->dilated_depth != NULL &&
        render_width <= runtime->render_capacity_width &&
        render_height <= runtime->render_capacity_height) {
        return true;
    }
    btbh_gpu_fsr3_release_render_resources(device, runtime);
    const uint32_t width = render_width;
    const uint32_t height = render_height;
    const uint32_t half_width = (width + 1u) / 2u;
    const uint32_t half_height = (height + 1u) / 2u;
    const SDL_GPUTextureUsageFlags storage =
        SDL_GPU_TEXTUREUSAGE_SAMPLER |
        SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
    const SDL_GPUTextureUsageFlags simultaneous =
        SDL_GPU_TEXTUREUSAGE_SAMPLER |
        SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE;
    const SDL_GPUTextureUsageFlags clearable =
        storage | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    runtime->spd_levels = btbh_gpu_fsr3_level_count(half_width, half_height);
    runtime->dilated_depth = btbh_gpu_fsr3_create_texture(
        device, SDL_GPU_TEXTUREFORMAT_R32_FLOAT, width, height, 1u, storage
    );
    runtime->dilated_motion_vectors = btbh_gpu_fsr3_create_texture(
        device, SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT, width, height, 1u, storage
    );
    runtime->reconstructed_previous_depth = btbh_gpu_fsr3_create_texture(
        device, SDL_GPU_TEXTUREFORMAT_R32_UINT, width, height, 1u, simultaneous
    );
    runtime->intermediate = btbh_gpu_fsr3_create_texture(
        device, SDL_GPU_TEXTUREFORMAT_R16_FLOAT, width, height, 1u, storage
    );
    for (uint32_t index = 0u; index < 2u; ++index) {
        runtime->luma[index] = btbh_gpu_fsr3_create_texture(
            device, SDL_GPU_TEXTUREFORMAT_R16_FLOAT, width, height, 1u, clearable
        );
        runtime->accumulation[index] = btbh_gpu_fsr3_create_texture(
            device, SDL_GPU_TEXTUREFORMAT_R8_UNORM, width, height, 1u, clearable
        );
        runtime->luma_history[index] = btbh_gpu_fsr3_create_texture(
            device,
            SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
            width,
            height,
            1u,
            clearable
        );
    }
    runtime->dilated_reactive_masks = btbh_gpu_fsr3_create_texture(
        device, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, width, height, 1u, storage
    );
    runtime->shading_change = btbh_gpu_fsr3_create_texture(
        device, SDL_GPU_TEXTUREFORMAT_R8_UNORM, half_width, half_height, 1u, storage
    );
    runtime->spd_mips = btbh_gpu_fsr3_create_texture(
        device,
        SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT,
        half_width,
        half_height,
        runtime->spd_levels,
        simultaneous | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET
    );
    runtime->farthest_depth_mip1 = btbh_gpu_fsr3_create_texture(
        device, SDL_GPU_TEXTUREFORMAT_R16_FLOAT, half_width, half_height, 1u, storage
    );
    bool created =
        runtime->dilated_depth != NULL &&
        runtime->dilated_motion_vectors != NULL &&
        runtime->reconstructed_previous_depth != NULL &&
        runtime->intermediate != NULL &&
        runtime->dilated_reactive_masks != NULL &&
        runtime->shading_change != NULL &&
        runtime->spd_mips != NULL &&
        runtime->farthest_depth_mip1 != NULL;
    for (uint32_t index = 0u; index < 2u; ++index) {
        created = created &&
            runtime->luma[index] != NULL &&
            runtime->accumulation[index] != NULL &&
            runtime->luma_history[index] != NULL;
    }
    if (!created) {
        btbh_gpu_fsr3_release_render_resources(device, runtime);
        return false;
    }
    runtime->render_capacity_width = width;
    runtime->render_capacity_height = height;
    runtime->resources_need_clear = true;
    runtime->history_valid = false;
    return true;
}

static bool btbh_gpu_fsr3_ensure_output_resources(
    SDL_GPUDevice *device,
    BTBHGPUFSR3Runtime *runtime,
    uint32_t output_width,
    uint32_t output_height
) {
    if (runtime->output != NULL &&
        runtime->output_width == output_width &&
        runtime->output_height == output_height) {
        return true;
    }
    btbh_gpu_fsr3_release_output_resources(device, runtime);
    const SDL_GPUTextureUsageFlags storage =
        SDL_GPU_TEXTUREUSAGE_SAMPLER |
        SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
    for (uint32_t index = 0u; index < 2u; ++index) {
        runtime->internal_upscaled[index] = btbh_gpu_fsr3_create_texture(
            device,
            SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
            output_width,
            output_height,
            1u,
            storage
        );
    }
    runtime->new_locks = btbh_gpu_fsr3_create_texture(
        device,
        SDL_GPU_TEXTUREFORMAT_R8_UNORM,
        output_width,
        output_height,
        1u,
        SDL_GPU_TEXTUREUSAGE_SAMPLER |
            SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE |
            SDL_GPU_TEXTUREUSAGE_COLOR_TARGET
    );
    runtime->output = btbh_gpu_fsr3_create_texture(
        device,
        SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
        output_width,
        output_height,
        1u,
        storage
    );
    if (runtime->internal_upscaled[0] == NULL ||
        runtime->internal_upscaled[1] == NULL ||
        runtime->new_locks == NULL ||
        runtime->output == NULL) {
        btbh_gpu_fsr3_release_output_resources(device, runtime);
        return false;
    }
    runtime->output_width = output_width;
    runtime->output_height = output_height;
    runtime->resources_need_clear = true;
    runtime->history_valid = false;
    return true;
}

static bool btbh_gpu_fsr3_ensure_fixed_resources(
    SDL_GPUDevice *device,
    BTBHGPUFSR3Runtime *runtime
) {
    if (runtime->point_sampler == NULL || runtime->linear_sampler == NULL) {
        SDL_GPUSamplerCreateInfo sampler;
        SDL_zero(sampler);
        sampler.min_filter = SDL_GPU_FILTER_NEAREST;
        sampler.mag_filter = SDL_GPU_FILTER_NEAREST;
        sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        sampler.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        sampler.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        sampler.max_lod = 1000.0f;
        if (runtime->point_sampler == NULL) {
            runtime->point_sampler = SDL_CreateGPUSampler(device, &sampler);
        }
        sampler.min_filter = SDL_GPU_FILTER_LINEAR;
        sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
        sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
        if (runtime->linear_sampler == NULL) {
            runtime->linear_sampler = SDL_CreateGPUSampler(device, &sampler);
        }
    }
    if (runtime->spd_atomic == NULL) {
        runtime->spd_atomic = btbh_gpu_fsr3_create_texture(
            device,
            SDL_GPU_TEXTUREFORMAT_R32_UINT,
            1u,
            1u,
            1u,
            SDL_GPU_TEXTUREUSAGE_SAMPLER |
                SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE
        );
        runtime->resources_need_clear = true;
    }
    if (runtime->frame_info == NULL) {
        runtime->frame_info = btbh_gpu_fsr3_create_texture(
            device,
            SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT,
            1u,
            1u,
            1u,
            SDL_GPU_TEXTUREUSAGE_SAMPLER |
                SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE |
                SDL_GPU_TEXTUREUSAGE_COLOR_TARGET
        );
        runtime->resources_need_clear = true;
    }
    if (runtime->default_exposure == NULL) {
        runtime->default_exposure = btbh_gpu_fsr3_create_texture(
            device,
            SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT,
            1u,
            1u,
            1u,
            SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET
        );
        runtime->resources_need_clear = true;
    }
    if (runtime->default_transparency == NULL) {
        runtime->default_transparency = btbh_gpu_fsr3_create_texture(
            device,
            SDL_GPU_TEXTUREFORMAT_R8_UNORM,
            1u,
            1u,
            1u,
            SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET
        );
        runtime->resources_need_clear = true;
    }
    return runtime->point_sampler != NULL &&
        runtime->linear_sampler != NULL &&
        runtime->spd_atomic != NULL &&
        runtime->frame_info != NULL &&
        runtime->default_exposure != NULL &&
        runtime->default_transparency != NULL;
}

bool btbh_gpu_fsr3_prepare(
    SDL_GPUDevice *device,
    BTBHGPUFSR3Runtime *runtime,
    const char *shader_reference_path,
    BTBHGPUShaderFormat shader_format,
    uint32_t render_width,
    uint32_t render_height,
    uint32_t output_width,
    uint32_t output_height,
    char *message,
    size_t message_capacity
) {
    if (message != NULL && message_capacity > 0u) {
        message[0] = '\0';
    }
    if (device == NULL || runtime == NULL || shader_reference_path == NULL) {
        btbh_gpu_fsr3_message(message, message_capacity, "FSR3 has no device", NULL);
        return false;
    }
    if (runtime->unavailable) {
        btbh_gpu_fsr3_message(message, message_capacity, "FSR3 is unavailable on this device", NULL);
        return false;
    }
    if (render_width < BTBH_GPU_FSR3_MINIMUM_RENDER_SIZE ||
        render_height < BTBH_GPU_FSR3_MINIMUM_RENDER_SIZE ||
        output_width < render_width ||
        output_height < render_height) {
        btbh_gpu_fsr3_message(message, message_capacity, "FSR3 needs a render size of at least 64 pixels, no larger than the output", NULL);
        return false;
    }
    if (runtime->pipelines[0] == NULL) {
        if (!btbh_gpu_fsr3_formats_supported(device)) {
            runtime->unavailable = true;
            btbh_gpu_fsr3_message(message, message_capacity, "FSR3 storage formats are unsupported on this device", NULL);
            return false;
        }
        for (uint32_t index = 0u; index < BTBH_GPU_FSR3_PIPELINE_COUNT; ++index) {
            runtime->pipelines[index] = btbh_gpu_fsr3_create_pipeline(
                device,
                shader_reference_path,
                shader_format,
                &btbh_gpu_fsr3_pipeline_specs[index],
                message,
                message_capacity
            );
            if (runtime->pipelines[index] == NULL) {
                for (uint32_t created = 0u; created < index; ++created) {
                    SDL_ReleaseGPUComputePipeline(device, runtime->pipelines[created]);
                    runtime->pipelines[created] = NULL;
                }
                runtime->unavailable = true;
                return false;
            }
        }
    }
    if (!btbh_gpu_fsr3_ensure_fixed_resources(device, runtime) ||
        !btbh_gpu_fsr3_ensure_render_resources(device, runtime, render_width, render_height) ||
        !btbh_gpu_fsr3_ensure_output_resources(device, runtime, output_width, output_height)) {
        btbh_gpu_fsr3_message(message, message_capacity, "FSR3 resources could not be created", SDL_GetError());
        return false;
    }
    return true;
}

int32_t btbh_gpu_fsr3_jitter_phase_count(
    uint32_t render_width,
    uint32_t output_width
) {
    if (render_width == 0u) {
        return 8;
    }
    const float ratio = (float)output_width / (float)render_width;
    const int32_t count = (int32_t)(8.0f * ratio * ratio);
    return count > 1 ? count : 1;
}

static float btbh_gpu_fsr3_halton(int32_t index, int32_t base) {
    float f = 1.0f;
    float result = 0.0f;
    for (int32_t current = index; current > 0;) {
        f /= (float)base;
        result += f * (float)(current % base);
        current = (int32_t)floorf((float)current / (float)base);
    }
    return result;
}

void btbh_gpu_fsr3_jitter_offset(
    int32_t index,
    int32_t phase_count,
    float *jitter_x,
    float *jitter_y
) {
    const int32_t count = phase_count > 0 ? phase_count : 1;
    const int32_t phase = (index % count + count) % count + 1;
    *jitter_x = btbh_gpu_fsr3_halton(phase, 2) - 0.5f;
    *jitter_y = btbh_gpu_fsr3_halton(phase, 3) - 0.5f;
}

void btbh_gpu_fsr3_next_jitter(
    BTBHGPUFSR3Runtime *runtime,
    uint32_t render_width,
    uint32_t output_width,
    float *jitter_x,
    float *jitter_y
) {
    const int32_t phase_count =
        btbh_gpu_fsr3_jitter_phase_count(render_width, output_width);
    btbh_gpu_fsr3_jitter_offset(
        (int32_t)(runtime->jitter_index % 1048576u),
        phase_count,
        &runtime->jitter_x,
        &runtime->jitter_y
    );
    runtime->jitter_index += 1u;
    *jitter_x = runtime->jitter_x;
    *jitter_y = runtime->jitter_y;
}

void btbh_gpu_fsr3_invalidate_history(BTBHGPUFSR3Runtime *runtime) {
    if (runtime != NULL) {
        runtime->history_valid = false;
    }
}

static bool btbh_gpu_fsr3_clear_color(
    SDL_GPUCommandBuffer *command,
    SDL_GPUTexture *texture,
    uint32_t mip_level,
    float red,
    float green,
    float blue,
    float alpha
) {
    SDL_GPUColorTargetInfo target;
    SDL_zero(target);
    target.texture = texture;
    target.mip_level = mip_level;
    target.clear_color.r = red;
    target.clear_color.g = green;
    target.clear_color.b = blue;
    target.clear_color.a = alpha;
    target.load_op = SDL_GPU_LOADOP_CLEAR;
    target.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, &target, 1u, NULL);
    if (pass == NULL) {
        return false;
    }
    SDL_EndGPURenderPass(pass);
    return true;
}

static bool btbh_gpu_fsr3_clear_uint(
    SDL_GPUCommandBuffer *command,
    BTBHGPUFSR3Runtime *runtime,
    SDL_GPUTexture *texture,
    uint32_t value,
    uint32_t width,
    uint32_t height
) {
    SDL_GPUStorageTextureReadWriteBinding binding;
    SDL_zero(binding);
    binding.texture = texture;
    SDL_GPUComputePass *pass = SDL_BeginGPUComputePass(command, &binding, 1u, NULL, 0u);
    if (pass == NULL) {
        return false;
    }
    BTBHGPUFSR3ClearConstants constants;
    constants.value_extent[0] = value;
    constants.value_extent[1] = width;
    constants.value_extent[2] = height;
    constants.value_extent[3] = 0u;
    SDL_BindGPUComputePipeline(pass, runtime->pipelines[BTBH_GPU_FSR3_PIPELINE_CLEAR]);
    SDL_PushGPUComputeUniformData(command, 0u, &constants, (uint32_t)sizeof(constants));
    SDL_DispatchGPUCompute(
        pass,
        (width + BTBH_GPU_FSR3_TILE - 1u) / BTBH_GPU_FSR3_TILE,
        (height + BTBH_GPU_FSR3_TILE - 1u) / BTBH_GPU_FSR3_TILE,
        1u
    );
    SDL_EndGPUComputePass(pass);
    return true;
}

/* One pass: read-only textures (each with its sampler), read-write textures (texture, mip). */
typedef struct BTBHGPUFSR3Storage {
    SDL_GPUTexture *texture;
    uint32_t mip_level;
} BTBHGPUFSR3Storage;

static bool btbh_gpu_fsr3_dispatch(
    SDL_GPUCommandBuffer *command,
    BTBHGPUFSR3Runtime *runtime,
    BTBHGPUFSR3Pipeline pipeline,
    const char *label,
    SDL_GPUTexture *const *textures,
    const bool *point_sampled,
    uint32_t texture_count,
    const BTBHGPUFSR3Storage *storage,
    uint32_t storage_count,
    uint32_t groups_x,
    uint32_t groups_y
) {
    SDL_GPUStorageTextureReadWriteBinding storage_bindings[12];
    SDL_GPUTextureSamplerBinding sampler_bindings[12];
    if (texture_count > 12u || storage_count > 12u ||
        texture_count != btbh_gpu_fsr3_pipeline_specs[pipeline].samplers ||
        storage_count != btbh_gpu_fsr3_pipeline_specs[pipeline].readwrite_textures) {
        SDL_SetError("FSR3 %s binding count mismatch", label);
        return false;
    }
    SDL_zeroa(storage_bindings);
    SDL_zeroa(sampler_bindings);
    for (uint32_t index = 0u; index < storage_count; ++index) {
        storage_bindings[index].texture = storage[index].texture;
        storage_bindings[index].mip_level = storage[index].mip_level;
    }
    for (uint32_t index = 0u; index < texture_count; ++index) {
        sampler_bindings[index].texture = textures[index];
        sampler_bindings[index].sampler =
            point_sampled != NULL && point_sampled[index]
                ? runtime->point_sampler
                : runtime->linear_sampler;
    }
    SDL_InsertGPUDebugLabel(command, label);
    SDL_GPUComputePass *pass = SDL_BeginGPUComputePass(
        command,
        storage_bindings,
        storage_count,
        NULL,
        0u
    );
    if (pass == NULL) {
        return false;
    }
    SDL_BindGPUComputePipeline(pass, runtime->pipelines[pipeline]);
    if (texture_count > 0u) {
        SDL_BindGPUComputeSamplers(pass, 0u, sampler_bindings, texture_count);
    }
    SDL_DispatchGPUCompute(
        pass,
        groups_x > 0u ? groups_x : 1u,
        groups_y > 0u ? groups_y : 1u,
        1u
    );
    SDL_EndGPUComputePass(pass);
    return true;
}

static uint32_t btbh_gpu_fsr3_groups(uint32_t size, uint32_t tile) {
    return (size + tile - 1u) / tile;
}

static void btbh_gpu_fsr3_update_constants(
    BTBHGPUFSR3Runtime *runtime,
    const BTBHGPUFSR3Inputs *inputs,
    bool reset
) {
    BTBHGPUFSR3Constants *constants = &runtime->constants;
    constants->previous_frame_jitter_offset[0] = constants->jitter_offset[0];
    constants->previous_frame_jitter_offset[1] = constants->jitter_offset[1];
    constants->jitter_offset[0] = runtime->jitter_x;
    constants->jitter_offset[1] = runtime->jitter_y;
    constants->previous_frame_render_size[0] = constants->render_size[0];
    constants->previous_frame_render_size[1] = constants->render_size[1];
    constants->render_size[0] = (int32_t)inputs->render_width;
    constants->render_size[1] = (int32_t)inputs->render_height;
    constants->max_render_size[0] = (int32_t)runtime->render_capacity_width;
    constants->max_render_size[1] = (int32_t)runtime->render_capacity_height;
    if (reset) {
        constants->previous_frame_jitter_offset[0] = runtime->jitter_x;
        constants->previous_frame_jitter_offset[1] = runtime->jitter_y;
        constants->previous_frame_render_size[0] = constants->render_size[0];
        constants->previous_frame_render_size[1] = constants->render_size[1];
    }

    /* tanHalfFOV is horizontal: tan(atan(tan(vertical / 2) * aspect)). */
    const float aspect = (float)inputs->render_width / (float)inputs->render_height;
    const float tan_half_vertical = tanf(0.5f * inputs->vertical_fov_radians);
    constants->tan_half_fov = tan_half_vertical * aspect;
    constants->view_space_to_meters_factor =
        inputs->view_space_to_meters > 0.0f ? inputs->view_space_to_meters : 1.0f;
    /* Standard (not reversed), finite depth: device depth d = far / (far - near) -
       near far / ((far - near) view depth), so view depth = e / (d - c) with these factors. */
    const float near_plane = fminf(inputs->camera_near, inputs->camera_far);
    const float far_plane = fmaxf(inputs->camera_near, inputs->camera_far);
    const float q = far_plane / (near_plane - far_plane);
    constants->device_to_view_depth[0] = -q;
    constants->device_to_view_depth[1] = q * near_plane;
    constants->device_to_view_depth[2] = tan_half_vertical * aspect;
    constants->device_to_view_depth[3] = tan_half_vertical;

    constants->previous_frame_upscale_size[0] = constants->upscale_size[0];
    constants->previous_frame_upscale_size[1] = constants->upscale_size[1];
    constants->upscale_size[0] = (int32_t)inputs->output_width;
    constants->upscale_size[1] = (int32_t)inputs->output_height;
    constants->max_upscale_size[0] = (int32_t)inputs->output_width;
    constants->max_upscale_size[1] = (int32_t)inputs->output_height;
    if (reset) {
        constants->previous_frame_upscale_size[0] = constants->upscale_size[0];
        constants->previous_frame_upscale_size[1] = constants->upscale_size[1];
    }
    constants->downscale_factor[0] =
        (float)constants->render_size[0] / (float)constants->upscale_size[0];
    constants->downscale_factor[1] =
        (float)constants->render_size[1] / (float)constants->upscale_size[1];
    constants->delta_pre_exposure = 1.0f;
    /* The scene's motion vectors are already in UV units at the render resolution. */
    constants->motion_vector_scale[0] = 1.0f;
    constants->motion_vector_scale[1] = 1.0f;
    constants->motion_vector_jitter_cancellation[0] = 0.0f;
    constants->motion_vector_jitter_cancellation[1] = 0.0f;

    const int32_t phase_count = btbh_gpu_fsr3_jitter_phase_count(
        inputs->render_width,
        inputs->output_width
    );
    if (reset || constants->jitter_phase_count == 0.0f) {
        constants->jitter_phase_count = (float)phase_count;
    } else if ((float)phase_count > constants->jitter_phase_count) {
        constants->jitter_phase_count += 1.0f;
    } else if ((float)phase_count < constants->jitter_phase_count) {
        constants->jitter_phase_count -= 1.0f;
    }
    constants->delta_time = fmaxf(0.0f, fminf(1.0f, inputs->frame_seconds));
    constants->frame_index = reset ? 0.0f : constants->frame_index + 1.0f;
    constants->velocity_factor = 1.0f;
    constants->reactiveness_scale = 1.0f;
    constants->shading_change_scale = 1.0f;
    constants->accumulation_added_per_frame = 1.0f / 3.0f;
    constants->min_disocclusion_accumulation = -1.0f / 3.0f;
}

static bool btbh_gpu_fsr3_clear_history(
    SDL_GPUCommandBuffer *command,
    BTBHGPUFSR3Runtime *runtime,
    bool all_resources
) {
    bool cleared = true;
    for (uint32_t index = 0u; index < 2u && cleared; ++index) {
        cleared =
            btbh_gpu_fsr3_clear_color(command, runtime->accumulation[index], 0u, 0.0f, 0.0f, 0.0f, 0.0f) &&
            (!all_resources ||
             (btbh_gpu_fsr3_clear_color(command, runtime->luma[index], 0u, 0.0f, 0.0f, 0.0f, 0.0f) &&
              btbh_gpu_fsr3_clear_color(command, runtime->luma_history[index], 0u, 0.0f, 0.0f, 0.0f, 0.0f)));
    }
    for (uint32_t level = 0u; level < runtime->spd_levels && cleared; ++level) {
        cleared = btbh_gpu_fsr3_clear_color(command, runtime->spd_mips, level, 0.0f, 0.0f, 0.0f, 0.0f);
    }
    /* AMD's reset value: exposure -1, log luma 1, so the exposure average starts over. */
    cleared = cleared &&
        btbh_gpu_fsr3_clear_color(command, runtime->frame_info, 0u, -1.0f, 1.0f, 0.0f, 0.0f);
    if (all_resources) {
        cleared = cleared &&
            btbh_gpu_fsr3_clear_color(command, runtime->new_locks, 0u, 0.0f, 0.0f, 0.0f, 0.0f) &&
            btbh_gpu_fsr3_clear_color(command, runtime->default_exposure, 0u, 0.0f, 0.0f, 0.0f, 0.0f) &&
            btbh_gpu_fsr3_clear_color(command, runtime->default_transparency, 0u, 0.0f, 0.0f, 0.0f, 0.0f);
    }
    return cleared;
}

bool btbh_gpu_fsr3_encode(
    SDL_GPUCommandBuffer *command,
    BTBHGPUFSR3Runtime *runtime,
    const BTBHGPUFSR3Inputs *inputs
) {
    if (command == NULL || runtime == NULL || inputs == NULL ||
        runtime->pipelines[0] == NULL || runtime->output == NULL ||
        inputs->color == NULL || inputs->depth == NULL ||
        inputs->motion_vectors == NULL || inputs->reactive == NULL ||
        inputs->render_width > runtime->render_capacity_width ||
        inputs->render_height > runtime->render_capacity_height ||
        inputs->output_width != runtime->output_width ||
        inputs->output_height != runtime->output_height) {
        SDL_SetError("FSR3 inputs do not match its resources");
        return false;
    }
    const bool first_frame = runtime->resources_need_clear;
    const bool reset = inputs->reset || !runtime->history_valid || first_frame;
    runtime->resources_need_clear = false;
    runtime->history_valid = true;

    const bool odd = (runtime->resource_frame_index & 1u) != 0u;
    const uint32_t read_index = odd ? 1u : 0u;
    const uint32_t write_index = odd ? 0u : 1u;
    SDL_GPUTexture *accumulation_read = runtime->accumulation[read_index];
    SDL_GPUTexture *accumulation_write = runtime->accumulation[write_index];
    SDL_GPUTexture *upscaled_read = runtime->internal_upscaled[read_index];
    SDL_GPUTexture *upscaled_write = runtime->internal_upscaled[write_index];
    SDL_GPUTexture *luma_history_read = runtime->luma_history[read_index];
    SDL_GPUTexture *luma_history_write = runtime->luma_history[write_index];
    SDL_GPUTexture *current_luma = runtime->luma[read_index];
    SDL_GPUTexture *previous_luma = runtime->luma[write_index];

    btbh_gpu_fsr3_update_constants(runtime, inputs, reset);

    SDL_InsertGPUDebugLabel(command, "BTBH FSR3");
    bool ok = true;
    if (reset) {
        ok = btbh_gpu_fsr3_clear_history(command, runtime, first_frame);
    }
    /* The reconstructed previous depth keeps each texel's nearest depth with an atomic minimum:
       it starts at the far plane (the bits of 1.0) every frame. */
    ok = ok &&
        btbh_gpu_fsr3_clear_uint(
            command,
            runtime,
            runtime->reconstructed_previous_depth,
            0x3F800000u,
            runtime->render_capacity_width,
            runtime->render_capacity_height
        ) &&
        btbh_gpu_fsr3_clear_uint(command, runtime, runtime->spd_atomic, 0u, 1u, 1u);
    if (!ok) {
        return false;
    }

    BTBHGPUFSR3SPDConstants spd;
    SDL_zero(spd);
    const uint32_t spd_groups_x =
        (inputs->render_width - 1u) / BTBH_GPU_FSR3_SPD_TILE + 1u;
    const uint32_t spd_groups_y =
        (inputs->render_height - 1u) / BTBH_GPU_FSR3_SPD_TILE + 1u;
    const uint32_t largest = inputs->render_width > inputs->render_height
        ? inputs->render_width
        : inputs->render_height;
    uint32_t mips = (uint32_t)floorf(log2f((float)largest));
    if (mips > 12u) {
        mips = 12u;
    }
    spd.mips = mips;
    spd.work_group_count = spd_groups_x * spd_groups_y;
    spd.render_size[0] = inputs->render_width;
    spd.render_size[1] = inputs->render_height;

    SDL_PushGPUComputeUniformData(command, 0u, &runtime->constants, (uint32_t)sizeof(runtime->constants));
    SDL_PushGPUComputeUniformData(command, 1u, &spd, (uint32_t)sizeof(spd));

    const uint32_t source_x = btbh_gpu_fsr3_groups(inputs->render_width, BTBH_GPU_FSR3_TILE);
    const uint32_t source_y = btbh_gpu_fsr3_groups(inputs->render_height, BTBH_GPU_FSR3_TILE);
    const uint32_t output_x = btbh_gpu_fsr3_groups(inputs->output_width, BTBH_GPU_FSR3_TILE);
    const uint32_t output_y = btbh_gpu_fsr3_groups(inputs->output_height, BTBH_GPU_FSR3_TILE);
    const uint32_t shading_x = btbh_gpu_fsr3_groups(
        (uint32_t)((float)inputs->render_width * 0.5f),
        BTBH_GPU_FSR3_TILE
    );
    const uint32_t shading_y = btbh_gpu_fsr3_groups(
        (uint32_t)((float)inputs->render_height * 0.5f),
        BTBH_GPU_FSR3_TILE
    );

    {
        SDL_GPUTexture *textures[] = {inputs->motion_vectors, inputs->depth, inputs->color};
        const bool point[] = {true, true, true};
        const BTBHGPUFSR3Storage storage[] = {
            {runtime->dilated_motion_vectors, 0u},
            {runtime->dilated_depth, 0u},
            {runtime->reconstructed_previous_depth, 0u},
            {runtime->intermediate, 0u},
            {current_luma, 0u},
        };
        ok = btbh_gpu_fsr3_dispatch(command, runtime, BTBH_GPU_FSR3_PIPELINE_PREPARE_INPUTS,
            "BTBH FSR3 Prepare Inputs", textures, point, 3u, storage, 5u, source_x, source_y);
    }
    if (ok) {
        SDL_GPUTexture *textures[] = {current_luma, runtime->intermediate};
        const BTBHGPUFSR3Storage storage[] = {
            {runtime->spd_atomic, 0u},
            {runtime->frame_info, 0u},
            {runtime->spd_mips, 5u},
            {runtime->farthest_depth_mip1, 0u},
        };
        ok = btbh_gpu_fsr3_dispatch(command, runtime, BTBH_GPU_FSR3_PIPELINE_LUMA_PYRAMID,
            "BTBH FSR3 Luma Pyramid", textures, NULL, 2u, storage, 4u, spd_groups_x, spd_groups_y);
    }
    if (ok) {
        SDL_GPUTexture *textures[] = {
            current_luma, previous_luma, runtime->dilated_motion_vectors, runtime->default_exposure
        };
        const BTBHGPUFSR3Storage storage[] = {
            {runtime->spd_atomic, 0u},
            {runtime->spd_mips, 0u},
            {runtime->spd_mips, 1u},
            {runtime->spd_mips, 2u},
            {runtime->spd_mips, 3u},
            {runtime->spd_mips, 4u},
            {runtime->spd_mips, 5u},
        };
        ok = btbh_gpu_fsr3_dispatch(command, runtime, BTBH_GPU_FSR3_PIPELINE_SHADING_CHANGE_PYRAMID,
            "BTBH FSR3 Shading Change Pyramid", textures, NULL, 4u, storage, 7u, spd_groups_x, spd_groups_y);
    }
    if (ok) {
        SDL_GPUTexture *textures[] = {runtime->spd_mips};
        const BTBHGPUFSR3Storage storage[] = {{runtime->shading_change, 0u}};
        ok = btbh_gpu_fsr3_dispatch(command, runtime, BTBH_GPU_FSR3_PIPELINE_SHADING_CHANGE,
            "BTBH FSR3 Shading Change", textures, NULL, 1u, storage, 1u, shading_x, shading_y);
    }
    if (ok) {
        SDL_GPUTexture *textures[] = {
            runtime->reconstructed_previous_depth,
            runtime->dilated_motion_vectors,
            runtime->dilated_depth,
            inputs->reactive,
            runtime->default_transparency,
            accumulation_read,
            runtime->shading_change,
            current_luma,
            runtime->default_exposure,
        };
        const bool point[] = {true, false, false, false, false, false, false, false, true};
        const BTBHGPUFSR3Storage storage[] = {
            {runtime->dilated_reactive_masks, 0u},
            {runtime->new_locks, 0u},
            {accumulation_write, 0u},
        };
        ok = btbh_gpu_fsr3_dispatch(command, runtime, BTBH_GPU_FSR3_PIPELINE_PREPARE_REACTIVITY,
            "BTBH FSR3 Prepare Reactivity", textures, point, 9u, storage, 3u, source_x, source_y);
    }
    if (ok) {
        SDL_GPUTexture *textures[] = {
            runtime->default_exposure,
            runtime->dilated_reactive_masks,
            runtime->dilated_motion_vectors,
            runtime->frame_info,
            luma_history_read,
            runtime->farthest_depth_mip1,
            current_luma,
        };
        const bool point[] = {true, false, false, true, false, false, false};
        const BTBHGPUFSR3Storage storage[] = {
            {luma_history_write, 0u},
            {runtime->intermediate, 0u},
        };
        ok = btbh_gpu_fsr3_dispatch(command, runtime, BTBH_GPU_FSR3_PIPELINE_LUMA_INSTABILITY,
            "BTBH FSR3 Luma Instability", textures, point, 7u, storage, 2u, source_x, source_y);
    }
    if (ok) {
        SDL_GPUTexture *textures[] = {
            runtime->default_exposure,
            runtime->dilated_reactive_masks,
            runtime->dilated_motion_vectors,
            upscaled_read,
            runtime->farthest_depth_mip1,
            current_luma,
            runtime->intermediate,
            inputs->color,
        };
        const bool point[] = {true, false, false, false, false, false, false, true};
        const BTBHGPUFSR3Storage storage[] = {
            {upscaled_write, 0u},
            {runtime->output, 0u},
            {runtime->new_locks, 0u},
        };
        ok = btbh_gpu_fsr3_dispatch(command, runtime, BTBH_GPU_FSR3_PIPELINE_ACCUMULATE,
            "BTBH FSR3 Accumulate", textures, point, 8u, storage, 3u, output_x, output_y);
    }
    if (ok && inputs->debug_view == BTBH_GPU_FSR3_DEBUG_UPSCALER) {
        SDL_GPUTexture *textures[] = {
            runtime->dilated_reactive_masks,
            runtime->dilated_motion_vectors,
            runtime->dilated_depth,
            upscaled_read,
            runtime->default_exposure,
        };
        const BTBHGPUFSR3Storage storage[] = {{runtime->output, 0u}};
        ok = btbh_gpu_fsr3_dispatch(command, runtime, BTBH_GPU_FSR3_PIPELINE_DEBUG_VIEW,
            "BTBH FSR3 Debug View", textures, NULL, 5u, storage, 1u, output_x, output_y);
    }
    if (ok &&
        (inputs->debug_view == BTBH_GPU_FSR3_DEBUG_MOTION_VECTORS ||
         (inputs->debug_view == BTBH_GPU_FSR3_DEBUG_MOTION_ERROR && inputs->previous_color != NULL))) {
        BTBHGPUFSR3MotionViewConstants view;
        view.output_metrics[0] = 1.0f / (float)inputs->output_width;
        view.output_metrics[1] = 1.0f / (float)inputs->output_height;
        view.output_metrics[2] = (float)inputs->output_width;
        view.output_metrics[3] = (float)inputs->output_height;
        view.motion_metrics[0] = (float)inputs->render_width;
        view.motion_metrics[1] = (float)inputs->render_height;
        view.motion_metrics[2] =
            inputs->debug_motion_pixels > 0.0f ? inputs->debug_motion_pixels : 8.0f;
        view.motion_metrics[3] =
            inputs->debug_view == BTBH_GPU_FSR3_DEBUG_MOTION_ERROR ? 1.0f : 0.0f;
        SDL_PushGPUComputeUniformData(command, 0u, &view, (uint32_t)sizeof(view));
        SDL_GPUTexture *textures[] = {
            inputs->color,
            inputs->motion_vectors,
            inputs->reactive,
            inputs->previous_color != NULL ? inputs->previous_color : inputs->color,
        };
        const bool point[] = {false, true, true, false};
        const BTBHGPUFSR3Storage storage[] = {{runtime->output, 0u}};
        ok = btbh_gpu_fsr3_dispatch(command, runtime, BTBH_GPU_FSR3_PIPELINE_MOTION_VIEW,
            "BTBH FSR3 Motion View", textures, point, 4u, storage, 1u, output_x, output_y);
    }
    runtime->resource_frame_index = (runtime->resource_frame_index + 1u) % 16u;
    if (!ok) {
        runtime->history_valid = false;
    }
    return ok;
}

void btbh_gpu_fsr3_release(
    SDL_GPUDevice *device,
    BTBHGPUFSR3Runtime *runtime
) {
    if (device == NULL || runtime == NULL) {
        return;
    }
    for (uint32_t index = 0u; index < BTBH_GPU_FSR3_PIPELINE_COUNT; ++index) {
        if (runtime->pipelines[index] != NULL) {
            SDL_ReleaseGPUComputePipeline(device, runtime->pipelines[index]);
            runtime->pipelines[index] = NULL;
        }
    }
    btbh_gpu_fsr3_release_render_resources(device, runtime);
    btbh_gpu_fsr3_release_output_resources(device, runtime);
    btbh_gpu_fsr3_release_texture(device, &runtime->spd_atomic);
    btbh_gpu_fsr3_release_texture(device, &runtime->frame_info);
    btbh_gpu_fsr3_release_texture(device, &runtime->default_exposure);
    btbh_gpu_fsr3_release_texture(device, &runtime->default_transparency);
    if (runtime->point_sampler != NULL) {
        SDL_ReleaseGPUSampler(device, runtime->point_sampler);
        runtime->point_sampler = NULL;
    }
    if (runtime->linear_sampler != NULL) {
        SDL_ReleaseGPUSampler(device, runtime->linear_sampler);
        runtime->linear_sampler = NULL;
    }
}
