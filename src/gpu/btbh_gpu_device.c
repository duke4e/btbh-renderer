#include "btbh_gpu_internal.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void btbh_gpu_set_report_error(
    BTBHGPUDeviceReport *report,
    BTBHGPUResult result,
    BTBHGPUErrorStage stage,
    const char *message
) {
    report->result = result;
    report->error_stage = stage;
    (void) snprintf(
        report->message,
        sizeof(report->message),
        "%s",
        message != NULL ? message : ""
    );
}

static bool btbh_gpu_set_device_properties(
    SDL_PropertiesID properties,
    const BTBHGPUDeviceDescriptor *descriptor
) {
    const char *driver = btbh_gpu_backend_driver_name(descriptor->backend);
    if (
        !SDL_SetStringProperty(
            properties,
            SDL_PROP_GPU_DEVICE_CREATE_NAME_STRING,
            driver
        ) ||
        !SDL_SetBooleanProperty(
            properties,
            SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN,
            descriptor->debug_mode != 0
        ) ||
        !SDL_SetBooleanProperty(
            properties,
            SDL_PROP_GPU_DEVICE_CREATE_VERBOSE_BOOLEAN,
            descriptor->debug_mode != 0
        )
    ) {
        return false;
    }

    switch (descriptor->backend) {
        case BTBH_GPU_BACKEND_VULKAN:
            return
                SDL_SetBooleanProperty(
                    properties,
                    SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN,
                    true
                ) &&
                SDL_SetBooleanProperty(
                    properties,
                    SDL_PROP_GPU_DEVICE_CREATE_VULKAN_REQUIRE_HARDWARE_ACCELERATION_BOOLEAN,
                    true
                );
        case BTBH_GPU_BACKEND_METAL:
            return SDL_SetBooleanProperty(
                properties,
                SDL_PROP_GPU_DEVICE_CREATE_SHADERS_METALLIB_BOOLEAN,
                true
            );
        case BTBH_GPU_BACKEND_INVALID:
        default:
            return false;
    }
}

static bool btbh_gpu_supports_color(
    SDL_GPUDevice *device,
    SDL_GPUTextureFormat format
) {
    return SDL_GPUTextureSupportsFormat(
        device,
        format,
        SDL_GPU_TEXTURETYPE_2D,
        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
            SDL_GPU_TEXTUREUSAGE_SAMPLER
    );
}

static bool btbh_gpu_supports_depth_type(
    SDL_GPUDevice *device,
    SDL_GPUTextureType type
) {
    const SDL_GPUTextureUsageFlags usage =
        SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET |
        SDL_GPU_TEXTUREUSAGE_SAMPLER;
    return
        SDL_GPUTextureSupportsFormat(
            device,
            SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
            type,
            usage
        ) ||
        SDL_GPUTextureSupportsFormat(
            device,
            SDL_GPU_TEXTUREFORMAT_D24_UNORM,
            type,
            usage
        );
}

static BTBHGPUFeatureFlags btbh_gpu_query_features(SDL_GPUDevice *device) {
    BTBHGPUFeatureFlags features = 0;

    if (btbh_gpu_supports_color(
        device,
        SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB
    )) {
        features |= BTBH_GPU_FEATURE_GBUFFER_RGBA8_SRGB;
    }
    if (btbh_gpu_supports_color(
        device,
        SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM
    )) {
        features |= BTBH_GPU_FEATURE_GBUFFER_RGBA8_UNORM;
    }
    if (btbh_gpu_supports_color(
        device,
        SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT
    )) {
        features |= BTBH_GPU_FEATURE_GBUFFER_RGBA16_FLOAT;
    }
    if (btbh_gpu_supports_depth_type(device, SDL_GPU_TEXTURETYPE_2D)) {
        features |= BTBH_GPU_FEATURE_SAMPLEABLE_DEPTH;
    }
    if (btbh_gpu_supports_depth_type(device, SDL_GPU_TEXTURETYPE_2D_ARRAY)) {
        features |= BTBH_GPU_FEATURE_SHADOW_2D_ARRAY;
    }
    if (btbh_gpu_supports_depth_type(device, SDL_GPU_TEXTURETYPE_CUBE_ARRAY)) {
        features |= BTBH_GPU_FEATURE_SHADOW_CUBE_ARRAY;
    }
    if (SDL_GPUTextureSupportsFormat(
        device,
        SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
        SDL_GPU_TEXTURETYPE_2D,
        SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE |
            SDL_GPU_TEXTUREUSAGE_SAMPLER
    )) {
        features |= BTBH_GPU_FEATURE_STORAGE_RGBA16_FLOAT;
    }

    features |= BTBH_GPU_FEATURE_COMPUTE;
    if (
        (features & BTBH_GPU_FEATURE_GBUFFER_RGBA8_UNORM) != 0
    ) {
        features |= BTBH_GPU_FEATURE_TEXTURE_READBACK;
    }
    return features;
}

BTBHGPUDevice *btbh_gpu_device_create(
    const BTBHGPUDeviceDescriptor *descriptor,
    BTBHGPUDeviceReport *report
) {
    if (descriptor == NULL || report == NULL) {
        return NULL;
    }

    memset(report, 0, sizeof(*report));
    if (btbh_gpu_backend_driver_name(descriptor->backend) == NULL) {
        btbh_gpu_set_report_error(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            BTBH_GPU_ERROR_STAGE_DEVICE_PROPERTIES,
            "Invalid GPU backend request"
        );
        return NULL;
    }

    BTBHGPUDevice *device = calloc(1, sizeof(*device));
    if (device == NULL) {
        btbh_gpu_set_report_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            BTBH_GPU_ERROR_STAGE_DEVICE_CREATION,
            "Could not allocate the GPU device context"
        );
        return NULL;
    }

    device->owns_video_subsystem =
        (SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) == 0;
    if (device->owns_video_subsystem && !SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        btbh_gpu_set_report_error(
            report,
            BTBH_GPU_PLATFORM_FAILURE,
            BTBH_GPU_ERROR_STAGE_PLATFORM_INITIALIZATION,
            SDL_GetError()
        );
        free(device);
        return NULL;
    }

    SDL_PropertiesID properties = SDL_CreateProperties();
    if (
        properties == 0 ||
        !btbh_gpu_set_device_properties(properties, descriptor)
    ) {
        btbh_gpu_set_report_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            BTBH_GPU_ERROR_STAGE_DEVICE_PROPERTIES,
            SDL_GetError()
        );
        if (properties != 0) {
            SDL_DestroyProperties(properties);
        }
        btbh_gpu_device_destroy(device);
        return NULL;
    }

    device->gpu = SDL_CreateGPUDeviceWithProperties(properties);
    SDL_DestroyProperties(properties);
    if (device->gpu == NULL) {
        btbh_gpu_set_report_error(
            report,
            BTBH_GPU_DEVICE_FAILURE,
            BTBH_GPU_ERROR_STAGE_DEVICE_CREATION,
            SDL_GetError()
        );
        btbh_gpu_device_destroy(device);
        return NULL;
    }

    const char *driver = SDL_GetGPUDeviceDriver(device->gpu);
    (void) snprintf(
        report->capabilities.driver_name,
        sizeof(report->capabilities.driver_name),
        "%s",
        driver != NULL ? driver : ""
    );
    report->capabilities.hardware_accelerated = 1;
    report->capabilities.features = btbh_gpu_query_features(device->gpu);

    report->result = btbh_gpu_validate_capabilities(
        descriptor->backend,
        descriptor->required_features,
        &report->capabilities,
        &report->validation
    );
    if (report->result != BTBH_GPU_OK) {
        btbh_gpu_set_report_error(
            report,
            report->result,
            report->result == BTBH_GPU_BACKEND_MISMATCH
                ? BTBH_GPU_ERROR_STAGE_DRIVER_VALIDATION
                : BTBH_GPU_ERROR_STAGE_CAPABILITY_VALIDATION,
            report->result == BTBH_GPU_BACKEND_MISMATCH
                ? "SDL_GPU selected an unexpected backend"
                : "SDL_GPU device is missing required Phase 2 capabilities"
        );
        btbh_gpu_device_destroy(device);
        return NULL;
    }

    report->error_stage = BTBH_GPU_ERROR_STAGE_NONE;
    return device;
}

void btbh_gpu_device_shutdown(BTBHGPUDevice *device) {
    if (device == NULL || device->shutdown) {
        return;
    }
    device->shutdown = true;
    if (device->gpu != NULL) {
        btbh_gpu_staging_release_device(device->gpu);
        SDL_DestroyGPUDevice(device->gpu);
        device->gpu = NULL;
    }
    if (device->owns_video_subsystem) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        device->owns_video_subsystem = false;
    }
}

void btbh_gpu_device_destroy(BTBHGPUDevice *device) {
    if (device == NULL) {
        return;
    }
    btbh_gpu_device_shutdown(device);
    free(device);
}

static SDL_SpinLock btbh_gpu_creation_lock;
static BTBHGPUCreationStats btbh_gpu_creation_totals;

static void btbh_gpu_count_creation(
    uint64_t *counter,
    uint64_t started_ticks
) {
    const uint64_t elapsed = SDL_GetTicksNS() - started_ticks;
    SDL_LockSpinlock(&btbh_gpu_creation_lock);
    *counter += 1u;
    btbh_gpu_creation_totals.nanoseconds += elapsed;
    SDL_UnlockSpinlock(&btbh_gpu_creation_lock);
}

/* ---- The lazy creation log (since 0.11.0, btbh_gpu_window_lazy_creation_report) ---- */

struct BTBHGPULazyCreationLog {
    bool enabled;
    uint32_t start;
    uint32_t count;
    uint64_t recorded;
    /* Each timed call's usual time (a running average), in milliseconds. */
    double usual_call_milliseconds[BTBH_GPU_LAZY_CALL_COUNT];
    BTBHGPULazyCreationEntry entries[BTBH_GPU_LAZY_CREATION_WINDOW_CAPACITY];
};

/* The shaders the recording scope made, by handle, so that its pipelines are named by their shader files (the
   renderer loads a pipeline's shaders just before it and releases them just after). */
#define BTBH_GPU_LAZY_SHADER_NAMES 32u
typedef struct BTBHGPULazyShaderName {
    const SDL_GPUShader *shader;
    char name[64];
} BTBHGPULazyShaderName;

static int btbh_gpu_lazy_environment = -1;
static SDL_SpinLock btbh_gpu_lazy_lock;
/* The recording scope's window (NULL while none records: then the wrappers do nothing more), thread and call. */
static void *btbh_gpu_lazy_window;
static SDL_ThreadID btbh_gpu_lazy_thread;
static uint8_t btbh_gpu_lazy_call;
/* Set while the recording present runs a step it shares with the prewarm (btbh_gpu_lazy_creation_prewarm_step). */
static bool btbh_gpu_lazy_in_prewarm_step;
static uint64_t btbh_gpu_lazy_call_threshold_nanoseconds = 1000000u;
static BTBHGPULazyShaderName btbh_gpu_lazy_shader_names[BTBH_GPU_LAZY_SHADER_NAMES];
static uint32_t btbh_gpu_lazy_shader_name_next;

static bool btbh_gpu_lazy_environment_enabled(void) {
    if (btbh_gpu_lazy_environment < 0) {
        const char *value = SDL_getenv("BTBH_GPU_LOG_LAZY_CREATION");
        btbh_gpu_lazy_environment = value != NULL && strcmp(value, "1") == 0 ? 1 : 0;
    }
    return btbh_gpu_lazy_environment == 1;
}

static BTBHGPULazyCreationLog *btbh_gpu_lazy_log(BTBHGPUWindow *window) {
    if (window->lazy_creation == NULL) {
        window->lazy_creation = calloc(1u, sizeof(*window->lazy_creation));
    }
    return window->lazy_creation;
}

void btbh_gpu_window_set_lazy_creation_log_enabled(
    BTBHGPUWindow *window,
    uint8_t enabled
) {
    if (window == NULL) {
        return;
    }
    if (enabled == 0u) {
        if (window->lazy_creation != NULL) {
            window->lazy_creation->enabled = false;
        }
        return;
    }
    BTBHGPULazyCreationLog *log = btbh_gpu_lazy_log(window);
    if (log != NULL) {
        log->enabled = true;
    }
}

BTBHGPUResult btbh_gpu_window_lazy_creation_report(
    BTBHGPUWindow *window,
    BTBHGPULazyCreationEntry *entries,
    uint32_t capacity,
    uint32_t *count
) {
    if (count != NULL) {
        *count = 0u;
    }
    if (window == NULL || count == NULL || (entries == NULL && capacity > 0u)) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    BTBHGPULazyCreationLog *log = window->lazy_creation;
    if (log == NULL) {
        return BTBH_GPU_OK;
    }
    SDL_LockSpinlock(&btbh_gpu_lazy_lock);
    const uint32_t copied = SDL_min(capacity, log->count);
    for (uint32_t index = 0u; index < copied; ++index) {
        entries[index] = log->entries[(log->start + index) % BTBH_GPU_LAZY_CREATION_WINDOW_CAPACITY];
    }
    log->start = (log->start + copied) % BTBH_GPU_LAZY_CREATION_WINDOW_CAPACITY;
    log->count -= copied;
    SDL_UnlockSpinlock(&btbh_gpu_lazy_lock);
    *count = copied;
    return BTBH_GPU_OK;
}

void btbh_gpu_lazy_creation_release(BTBHGPUWindow *window) {
    if (window != NULL) {
        free(window->lazy_creation);
        window->lazy_creation = NULL;
    }
}

void btbh_gpu_lazy_creation_set_call_threshold(uint64_t nanoseconds) {
    btbh_gpu_lazy_call_threshold_nanoseconds = nanoseconds;
}

void btbh_gpu_lazy_creation_begin(
    BTBHGPUWindow *window,
    uint8_t call,
    BTBHGPULazyCreationScope *scope
) {
    scope->window = NULL;
    if (window == NULL ||
        !((window->lazy_creation != NULL && window->lazy_creation->enabled) ||
          btbh_gpu_lazy_environment_enabled()) ||
        btbh_gpu_lazy_log(window) == NULL) {
        return;
    }
    const SDL_ThreadID thread = SDL_GetCurrentThreadID();
    SDL_LockSpinlock(&btbh_gpu_lazy_lock);
    void *recording = SDL_GetAtomicPointer(&btbh_gpu_lazy_window);
    /* One thread records at a time: another thread's present meanwhile isn't recorded. */
    if (recording == NULL || btbh_gpu_lazy_thread == thread) {
        scope->window = window;
        scope->previous_window = recording;
        scope->previous_thread = btbh_gpu_lazy_thread;
        scope->previous_call = btbh_gpu_lazy_call;
        scope->previous_in_prewarm_step = btbh_gpu_lazy_in_prewarm_step;
        btbh_gpu_lazy_thread = thread;
        btbh_gpu_lazy_call = call;
        btbh_gpu_lazy_in_prewarm_step = false;
        SDL_SetAtomicPointer(&btbh_gpu_lazy_window, window);
    }
    SDL_UnlockSpinlock(&btbh_gpu_lazy_lock);
}

void btbh_gpu_lazy_creation_end(const BTBHGPULazyCreationScope *scope) {
    if (scope->window == NULL) {
        return;
    }
    SDL_LockSpinlock(&btbh_gpu_lazy_lock);
    btbh_gpu_lazy_thread = scope->previous_thread;
    btbh_gpu_lazy_call = scope->previous_call;
    btbh_gpu_lazy_in_prewarm_step = scope->previous_in_prewarm_step;
    SDL_SetAtomicPointer(&btbh_gpu_lazy_window, scope->previous_window);
    if (scope->previous_window == NULL) {
        SDL_zeroa(btbh_gpu_lazy_shader_names);
        btbh_gpu_lazy_shader_name_next = 0u;
    }
    SDL_UnlockSpinlock(&btbh_gpu_lazy_lock);
}

/* The window this thread's scope records (and its call, with BTBH_GPU_LAZY_CALL_IN_PREWARM_STEP set inside a step
   the prewarm shares), or NULL: one atomic load while nothing records. */
#define BTBH_GPU_LAZY_CALL_IN_PREWARM_STEP 0x80u
static BTBHGPUWindow *btbh_gpu_lazy_recording(uint8_t *call) {
    if (SDL_GetAtomicPointer(&btbh_gpu_lazy_window) == NULL) {
        return NULL;
    }
    const SDL_ThreadID thread = SDL_GetCurrentThreadID();
    SDL_LockSpinlock(&btbh_gpu_lazy_lock);
    BTBHGPUWindow *window =
        btbh_gpu_lazy_thread == thread ? (BTBHGPUWindow *)SDL_GetAtomicPointer(&btbh_gpu_lazy_window) : NULL;
    *call = (uint8_t)(btbh_gpu_lazy_call | (btbh_gpu_lazy_in_prewarm_step ? BTBH_GPU_LAZY_CALL_IN_PREWARM_STEP : 0u));
    SDL_UnlockSpinlock(&btbh_gpu_lazy_lock);
    return window;
}

void btbh_gpu_lazy_creation_prewarm_step(bool entering) {
    if (SDL_GetAtomicPointer(&btbh_gpu_lazy_window) == NULL) {
        return;
    }
    const SDL_ThreadID thread = SDL_GetCurrentThreadID();
    SDL_LockSpinlock(&btbh_gpu_lazy_lock);
    if (btbh_gpu_lazy_thread == thread) {
        btbh_gpu_lazy_in_prewarm_step = entering;
    }
    SDL_UnlockSpinlock(&btbh_gpu_lazy_lock);
}

static const char *const btbh_gpu_lazy_kind_names[] = {
    "?", "shader", "graphics_pipeline", "compute_pipeline", "sampler", "texture", "buffer", "transfer_buffer",
    "slow_call"
};
static const char *const btbh_gpu_lazy_call_names[] = { "?", "scene", "target", "canvas" };

static void btbh_gpu_lazy_record(
    BTBHGPUWindow *window,
    uint8_t call,
    BTBHGPULazyCreationKind kind,
    uint64_t nanoseconds,
    const char *name
) {
    BTBHGPULazyCreationLog *log = window->lazy_creation;
    BTBHGPULazyCreationEntry entry;
    SDL_zero(entry);
    entry.frame_index = window->submitted_frame_count;
    entry.microseconds = (uint32_t)SDL_min(nanoseconds / 1000u, (uint64_t)UINT32_MAX);
    entry.kind = (uint8_t)kind;
    /* What the present made in the steps it shares with btbh_gpu_window_scene_prewarm (or its render-to-target
       form), the prewarm makes. The rest (the buffers that grow with a frame's contents, the canvas, the slow calls)
       nothing makes ahead. */
    entry.prewarm_covers = (call & BTBH_GPU_LAZY_CALL_IN_PREWARM_STEP) != 0u ? 1u : 0u;
    call = (uint8_t)(call & ~BTBH_GPU_LAZY_CALL_IN_PREWARM_STEP);
    entry.call = call;
    (void)snprintf(entry.name, sizeof(entry.name), "%s", name);
    SDL_LockSpinlock(&btbh_gpu_lazy_lock);
    entry.sequence = log->recorded++;
    const uint32_t slot = (log->start + log->count) % BTBH_GPU_LAZY_CREATION_WINDOW_CAPACITY;
    if (log->count == BTBH_GPU_LAZY_CREATION_WINDOW_CAPACITY) {
        log->start = (log->start + 1u) % BTBH_GPU_LAZY_CREATION_WINDOW_CAPACITY;
    } else {
        log->count += 1u;
    }
    log->entries[slot] = entry;
    SDL_UnlockSpinlock(&btbh_gpu_lazy_lock);
    if (btbh_gpu_lazy_environment_enabled()) {
        SDL_Log(
            "BTBH_GPU_LAZY frame=%llu call=%s kind=%s ms=%.3f prewarm=%u %s",
            (unsigned long long)entry.frame_index,
            btbh_gpu_lazy_call_names[call <= 3u ? call : 0u],
            btbh_gpu_lazy_kind_names[kind],
            (double)nanoseconds / 1000000.0,
            (unsigned)entry.prewarm_covers,
            entry.name
        );
    }
}

static const char *btbh_gpu_lazy_function(const char *function) {
    if (function == NULL) {
        return "?";
    }
    return strncmp(function, "btbh_gpu_", 9u) == 0 ? function + 9 : function;
}

static const char *btbh_gpu_lazy_file_name(const char *path) {
    if (path == NULL) {
        return "?";
    }
    const char *name = path;
    for (const char *at = path; *at != '\0'; ++at) {
        if (*at == '/' || *at == '\\') {
            name = at + 1;
        }
    }
    return name;
}

static const char *btbh_gpu_lazy_format(SDL_GPUTextureFormat format, char *scratch, size_t capacity) {
    switch (format) {
        case SDL_GPU_TEXTUREFORMAT_R8_UNORM: return "r8";
        case SDL_GPU_TEXTUREFORMAT_R8G8_UNORM: return "rg8";
        case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM: return "rgba8";
        case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB: return "rgba8_srgb";
        case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM: return "bgra8";
        case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB: return "bgra8_srgb";
        case SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM: return "rgb10a2";
        case SDL_GPU_TEXTUREFORMAT_R11G11B10_UFLOAT: return "rg11b10f";
        case SDL_GPU_TEXTUREFORMAT_R16_UNORM: return "r16";
        case SDL_GPU_TEXTUREFORMAT_R16G16_UNORM: return "rg16";
        case SDL_GPU_TEXTUREFORMAT_R16G16B16A16_UNORM: return "rgba16";
        case SDL_GPU_TEXTUREFORMAT_R16_FLOAT: return "r16f";
        case SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT: return "rg16f";
        case SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT: return "rgba16f";
        case SDL_GPU_TEXTUREFORMAT_R32_FLOAT: return "r32f";
        case SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT: return "rg32f";
        case SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT: return "rgba32f";
        case SDL_GPU_TEXTUREFORMAT_R8_UINT: return "r8u";
        case SDL_GPU_TEXTUREFORMAT_R32_UINT: return "r32u";
        case SDL_GPU_TEXTUREFORMAT_D16_UNORM: return "d16";
        case SDL_GPU_TEXTUREFORMAT_D24_UNORM: return "d24";
        case SDL_GPU_TEXTUREFORMAT_D32_FLOAT: return "d32f";
        case SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT: return "d24s8";
        case SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT: return "d32fs8";
        default:
            (void)snprintf(scratch, capacity, "format%d", (int)format);
            return scratch;
    }
}

static const char *btbh_gpu_lazy_compare(SDL_GPUCompareOp op) {
    static const char *const names[] = { "?", "never", "lt", "eq", "le", "gt", "ne", "ge", "always" };
    return (uint32_t)op < SDL_arraysize(names) ? names[op] : "?";
}

static const char *btbh_gpu_lazy_stencil_op(SDL_GPUStencilOp op) {
    static const char *const names[] = { "?", "keep", "zero", "replace", "incc", "decc", "invert", "incw", "decw" };
    return (uint32_t)op < SDL_arraysize(names) ? names[op] : "?";
}

static const char *btbh_gpu_lazy_blend_factor(SDL_GPUBlendFactor factor) {
    static const char *const names[] = {
        "?", "0", "1", "sc", "1-sc", "dc", "1-dc", "sa", "1-sa", "da", "1-da", "k", "1-k", "sat"
    };
    return (uint32_t)factor < SDL_arraysize(names) ? names[factor] : "?";
}

static const char *btbh_gpu_lazy_blend_op(SDL_GPUBlendOp op) {
    static const char *const names[] = { "?", "add", "sub", "rsub", "min", "max" };
    return (uint32_t)op < SDL_arraysize(names) ? names[op] : "?";
}

/* Appends to a name, cut at its capacity. */
static void btbh_gpu_lazy_append(
    char *name,
    size_t capacity,
    size_t *length,
    SDL_PRINTF_FORMAT_STRING const char *format,
    ...
) SDL_PRINTF_VARARG_FUNC(4);
static void btbh_gpu_lazy_append(char *name, size_t capacity, size_t *length, const char *format, ...) {
    if (*length + 1u >= capacity) {
        return;
    }
    va_list arguments;
    va_start(arguments, format);
    const int written = vsnprintf(name + *length, capacity - *length, format, arguments);
    va_end(arguments);
    if (written > 0) {
        *length = SDL_min(*length + (size_t)written, capacity - 1u);
    }
}

/* (Called with btbh_gpu_lazy_lock held.) */
static const char *btbh_gpu_lazy_shader_name(const SDL_GPUShader *shader) {
    if (shader == NULL) {
        return "-";
    }
    for (uint32_t index = 0u; index < BTBH_GPU_LAZY_SHADER_NAMES; ++index) {
        if (btbh_gpu_lazy_shader_names[index].shader == shader) {
            return btbh_gpu_lazy_shader_names[index].name;
        }
    }
    return "?";
}

SDL_GPUShader *btbh_gpu_create_shader_from(
    SDL_GPUDevice *device,
    const SDL_GPUShaderCreateInfo *info,
    const char *path,
    uint64_t load_started_ticks,
    const char *function
) {
    const uint64_t started_ticks = SDL_GetTicksNS();
    SDL_GPUShader *shader = SDL_CreateGPUShader(device, info);
    btbh_gpu_count_creation(&btbh_gpu_creation_totals.shaders, started_ticks);
    uint8_t call = 0u;
    BTBHGPUWindow *window = btbh_gpu_lazy_recording(&call);
    if (window != NULL && shader != NULL) {
        const uint64_t finished_ticks = SDL_GetTicksNS();
        SDL_LockSpinlock(&btbh_gpu_lazy_lock);
        BTBHGPULazyShaderName *slot =
            &btbh_gpu_lazy_shader_names[btbh_gpu_lazy_shader_name_next++ % BTBH_GPU_LAZY_SHADER_NAMES];
        slot->shader = shader;
        (void)snprintf(slot->name, sizeof(slot->name), "%s", btbh_gpu_lazy_file_name(path));
        SDL_UnlockSpinlock(&btbh_gpu_lazy_lock);
        char name[BTBH_GPU_LAZY_CREATION_NAME_CAPACITY];
        if (function != NULL) {
            (void)snprintf(
                name,
                sizeof(name),
                "%s (%s)",
                btbh_gpu_lazy_file_name(path),
                btbh_gpu_lazy_function(function)
            );
        } else {
            (void)snprintf(name, sizeof(name), "%s", btbh_gpu_lazy_file_name(path));
        }
        btbh_gpu_lazy_record(
            window,
            call,
            BTBH_GPU_LAZY_CREATION_SHADER,
            finished_ticks - (load_started_ticks != 0u ? load_started_ticks : started_ticks),
            name
        );
    }
    return shader;
}

SDL_GPUGraphicsPipeline *btbh_gpu_create_graphics_pipeline_at(
    SDL_GPUDevice *device,
    const SDL_GPUGraphicsPipelineCreateInfo *info,
    const char *function
) {
    const uint64_t started_ticks = SDL_GetTicksNS();
    SDL_GPUGraphicsPipeline *pipeline = SDL_CreateGPUGraphicsPipeline(device, info);
    btbh_gpu_count_creation(&btbh_gpu_creation_totals.graphics_pipelines, started_ticks);
    uint8_t call = 0u;
    BTBHGPUWindow *window = btbh_gpu_lazy_recording(&call);
    if (window == NULL || pipeline == NULL) {
        return pipeline;
    }
    const uint64_t nanoseconds = SDL_GetTicksNS() - started_ticks;
    /* The pipeline's key: shaders, colour targets and their blending, depth-stencil, depth test, stencil, culling. */
    char name[BTBH_GPU_LAZY_CREATION_NAME_CAPACITY];
    char scratch[16];
    size_t length = 0u;
    name[0] = '\0';
    SDL_LockSpinlock(&btbh_gpu_lazy_lock);
    btbh_gpu_lazy_append(
        name,
        sizeof(name),
        &length,
        "vs=%s fs=%s",
        btbh_gpu_lazy_shader_name(info->vertex_shader),
        btbh_gpu_lazy_shader_name(info->fragment_shader)
    );
    SDL_UnlockSpinlock(&btbh_gpu_lazy_lock);
    const SDL_GPUGraphicsPipelineTargetInfo *targets = &info->target_info;
    if (targets->num_color_targets > 0u) {
        btbh_gpu_lazy_append(name, sizeof(name), &length, " colors=");
    }
    for (uint32_t index = 0u; index < targets->num_color_targets; ++index) {
        const SDL_GPUColorTargetDescription *target = &targets->color_target_descriptions[index];
        const SDL_GPUColorTargetBlendState *blend = &target->blend_state;
        btbh_gpu_lazy_append(
            name,
            sizeof(name),
            &length,
            "%s%s",
            index > 0u ? "," : "",
            btbh_gpu_lazy_format(target->format, scratch, sizeof(scratch))
        );
        if (blend->enable_blend) {
            btbh_gpu_lazy_append(
                name,
                sizeof(name),
                &length,
                "[%s,%s,%s",
                btbh_gpu_lazy_blend_factor(blend->src_color_blendfactor),
                btbh_gpu_lazy_blend_factor(blend->dst_color_blendfactor),
                btbh_gpu_lazy_blend_op(blend->color_blend_op)
            );
            if (blend->src_alpha_blendfactor != blend->src_color_blendfactor ||
                blend->dst_alpha_blendfactor != blend->dst_color_blendfactor ||
                blend->alpha_blend_op != blend->color_blend_op) {
                btbh_gpu_lazy_append(
                    name,
                    sizeof(name),
                    &length,
                    "/%s,%s,%s",
                    btbh_gpu_lazy_blend_factor(blend->src_alpha_blendfactor),
                    btbh_gpu_lazy_blend_factor(blend->dst_alpha_blendfactor),
                    btbh_gpu_lazy_blend_op(blend->alpha_blend_op)
                );
            }
            btbh_gpu_lazy_append(name, sizeof(name), &length, "]");
        }
        if (blend->enable_color_write_mask && blend->color_write_mask != 0x0fu) {
            btbh_gpu_lazy_append(name, sizeof(name), &length, "m%x", (unsigned)blend->color_write_mask);
        }
    }
    if (targets->has_depth_stencil_target) {
        btbh_gpu_lazy_append(
            name,
            sizeof(name),
            &length,
            " ds=%s",
            btbh_gpu_lazy_format(targets->depth_stencil_format, scratch, sizeof(scratch))
        );
    }
    const SDL_GPUDepthStencilState *depth = &info->depth_stencil_state;
    if (depth->enable_depth_test) {
        btbh_gpu_lazy_append(
            name,
            sizeof(name),
            &length,
            " depth=%s%s",
            btbh_gpu_lazy_compare(depth->compare_op),
            depth->enable_depth_write ? "+w" : ""
        );
    } else if (depth->enable_depth_write) {
        btbh_gpu_lazy_append(name, sizeof(name), &length, " depth=w");
    }
    if (depth->enable_stencil_test) {
        btbh_gpu_lazy_append(
            name,
            sizeof(name),
            &length,
            " stencil=%s/%s r%02x w%02x",
            btbh_gpu_lazy_compare(depth->front_stencil_state.compare_op),
            btbh_gpu_lazy_stencil_op(depth->front_stencil_state.pass_op),
            (unsigned)depth->compare_mask,
            (unsigned)depth->write_mask
        );
    }
    static const char *const culls[] = { "none", "front", "back" };
    btbh_gpu_lazy_append(
        name,
        sizeof(name),
        &length,
        " cull=%s%s%s%s",
        (uint32_t)info->rasterizer_state.cull_mode < 3u ? culls[info->rasterizer_state.cull_mode] : "?",
        info->rasterizer_state.front_face == SDL_GPU_FRONTFACE_CLOCKWISE ? " cw" : "",
        info->rasterizer_state.enable_depth_bias ? " bias" : "",
        info->primitive_type != SDL_GPU_PRIMITIVETYPE_TRIANGLELIST ? " not-trianglelist" : ""
    );
    btbh_gpu_lazy_append(name, sizeof(name), &length, " (%s)", btbh_gpu_lazy_function(function));
    btbh_gpu_lazy_record(window, call, BTBH_GPU_LAZY_CREATION_GRAPHICS_PIPELINE, nanoseconds, name);
    return pipeline;
}

SDL_GPUComputePipeline *btbh_gpu_create_compute_pipeline_from(
    SDL_GPUDevice *device,
    const SDL_GPUComputePipelineCreateInfo *info,
    const char *path,
    uint64_t load_started_ticks,
    const char *function
) {
    const uint64_t started_ticks = SDL_GetTicksNS();
    SDL_GPUComputePipeline *pipeline = SDL_CreateGPUComputePipeline(device, info);
    btbh_gpu_count_creation(&btbh_gpu_creation_totals.compute_pipelines, started_ticks);
    uint8_t call = 0u;
    BTBHGPUWindow *window = btbh_gpu_lazy_recording(&call);
    if (window != NULL && pipeline != NULL) {
        const uint64_t finished_ticks = SDL_GetTicksNS();
        char name[BTBH_GPU_LAZY_CREATION_NAME_CAPACITY];
        (void)snprintf(
            name,
            sizeof(name),
            "cs=%s threads=%ux%ux%u%s%s%s",
            btbh_gpu_lazy_file_name(path),
            (unsigned)info->threadcount_x,
            (unsigned)info->threadcount_y,
            (unsigned)info->threadcount_z,
            function != NULL ? " (" : "",
            function != NULL ? btbh_gpu_lazy_function(function) : "",
            function != NULL ? ")" : ""
        );
        btbh_gpu_lazy_record(
            window,
            call,
            BTBH_GPU_LAZY_CREATION_COMPUTE_PIPELINE,
            finished_ticks - (load_started_ticks != 0u ? load_started_ticks : started_ticks),
            name
        );
    }
    return pipeline;
}

uint64_t btbh_gpu_lazy_creation_clock(void) {
    uint8_t call = 0u;
    return btbh_gpu_lazy_recording(&call) != NULL ? SDL_GetTicksNS() : 0u;
}

SDL_GPUTexture *btbh_gpu_create_texture_at(
    SDL_GPUDevice *device,
    const SDL_GPUTextureCreateInfo *info,
    const char *function
) {
    const uint64_t started_ticks = btbh_gpu_lazy_creation_clock();
    SDL_GPUTexture *texture = (SDL_CreateGPUTexture)(device, info);
    if (started_ticks == 0u || texture == NULL) {
        return texture;
    }
    uint8_t call = 0u;
    BTBHGPUWindow *window = btbh_gpu_lazy_recording(&call);
    if (window != NULL) {
        const uint64_t nanoseconds = SDL_GetTicksNS() - started_ticks;
        static const char *const types[] = { "2d", "2d_array", "3d", "cube", "cube_array" };
        char scratch[16];
        char name[BTBH_GPU_LAZY_CREATION_NAME_CAPACITY];
        (void)snprintf(
            name,
            sizeof(name),
            "%ux%ux%u %s %s levels=%u%s%s%s%s%s (%s)",
            (unsigned)info->width,
            (unsigned)info->height,
            (unsigned)info->layer_count_or_depth,
            (uint32_t)info->type < 5u ? types[info->type] : "?",
            btbh_gpu_lazy_format(info->format, scratch, sizeof(scratch)),
            (unsigned)info->num_levels,
            (info->usage & SDL_GPU_TEXTUREUSAGE_SAMPLER) != 0u ? " sampled" : "",
            (info->usage & SDL_GPU_TEXTUREUSAGE_COLOR_TARGET) != 0u ? " color_target" : "",
            (info->usage & SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET) != 0u ? " depth_target" : "",
            (info->usage & (SDL_GPU_TEXTUREUSAGE_GRAPHICS_STORAGE_READ |
                            SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ)) != 0u
                ? " storage_read"
                : "",
            (info->usage & (SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE |
                            SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE)) != 0u
                ? " storage_write"
                : "",
            btbh_gpu_lazy_function(function)
        );
        btbh_gpu_lazy_record(window, call, BTBH_GPU_LAZY_CREATION_TEXTURE, nanoseconds, name);
    }
    return texture;
}

SDL_GPUSampler *btbh_gpu_create_sampler_at(
    SDL_GPUDevice *device,
    const SDL_GPUSamplerCreateInfo *info,
    const char *function
) {
    const uint64_t started_ticks = btbh_gpu_lazy_creation_clock();
    SDL_GPUSampler *sampler = (SDL_CreateGPUSampler)(device, info);
    if (started_ticks == 0u || sampler == NULL) {
        return sampler;
    }
    uint8_t call = 0u;
    BTBHGPUWindow *window = btbh_gpu_lazy_recording(&call);
    if (window != NULL) {
        const uint64_t nanoseconds = SDL_GetTicksNS() - started_ticks;
        static const char *const filters[] = { "nearest", "linear" };
        static const char *const addresses[] = { "repeat", "mirror", "clamp" };
        char name[BTBH_GPU_LAZY_CREATION_NAME_CAPACITY];
        (void)snprintf(
            name,
            sizeof(name),
            "min=%s mag=%s mip=%s address=%s%s%s (%s)",
            (uint32_t)info->min_filter < 2u ? filters[info->min_filter] : "?",
            (uint32_t)info->mag_filter < 2u ? filters[info->mag_filter] : "?",
            (uint32_t)info->mipmap_mode < 2u ? filters[info->mipmap_mode] : "?",
            (uint32_t)info->address_mode_u < 3u ? addresses[info->address_mode_u] : "?",
            info->enable_anisotropy ? " anisotropic" : "",
            info->enable_compare ? " compare" : "",
            btbh_gpu_lazy_function(function)
        );
        btbh_gpu_lazy_record(window, call, BTBH_GPU_LAZY_CREATION_SAMPLER, nanoseconds, name);
    }
    return sampler;
}

SDL_GPUBuffer *btbh_gpu_create_buffer_at(
    SDL_GPUDevice *device,
    const SDL_GPUBufferCreateInfo *info,
    const char *function
) {
    const uint64_t started_ticks = btbh_gpu_lazy_creation_clock();
    SDL_GPUBuffer *buffer = (SDL_CreateGPUBuffer)(device, info);
    if (started_ticks == 0u || buffer == NULL) {
        return buffer;
    }
    uint8_t call = 0u;
    BTBHGPUWindow *window = btbh_gpu_lazy_recording(&call);
    if (window != NULL) {
        const uint64_t nanoseconds = SDL_GetTicksNS() - started_ticks;
        char name[BTBH_GPU_LAZY_CREATION_NAME_CAPACITY];
        (void)snprintf(
            name,
            sizeof(name),
            "%u bytes%s%s%s%s (%s)",
            (unsigned)info->size,
            (info->usage & SDL_GPU_BUFFERUSAGE_VERTEX) != 0u ? " vertex" : "",
            (info->usage & SDL_GPU_BUFFERUSAGE_INDEX) != 0u ? " index" : "",
            (info->usage & (SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ)) != 0u
                ? " storage_read"
                : "",
            (info->usage & SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE) != 0u ? " storage_write" : "",
            btbh_gpu_lazy_function(function)
        );
        btbh_gpu_lazy_record(window, call, BTBH_GPU_LAZY_CREATION_BUFFER, nanoseconds, name);
    }
    return buffer;
}

SDL_GPUTransferBuffer *btbh_gpu_create_transfer_buffer_at(
    SDL_GPUDevice *device,
    const SDL_GPUTransferBufferCreateInfo *info,
    const char *function
) {
    const uint64_t started_ticks = btbh_gpu_lazy_creation_clock();
    SDL_GPUTransferBuffer *buffer = (SDL_CreateGPUTransferBuffer)(device, info);
    if (started_ticks == 0u || buffer == NULL) {
        return buffer;
    }
    uint8_t call = 0u;
    BTBHGPUWindow *window = btbh_gpu_lazy_recording(&call);
    if (window != NULL) {
        const uint64_t nanoseconds = SDL_GetTicksNS() - started_ticks;
        char name[BTBH_GPU_LAZY_CREATION_NAME_CAPACITY];
        (void)snprintf(
            name,
            sizeof(name),
            "%u bytes %s (%s)",
            (unsigned)info->size,
            info->usage == SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD ? "download" : "upload",
            btbh_gpu_lazy_function(function)
        );
        btbh_gpu_lazy_record(window, call, BTBH_GPU_LAZY_CREATION_TRANSFER_BUFFER, nanoseconds, name);
    }
    return buffer;
}

void btbh_gpu_lazy_creation_call_timed(BTBHGPULazyTimedCall timed_call, uint64_t started_ticks) {
    if (started_ticks == 0u || (uint32_t)timed_call >= (uint32_t)BTBH_GPU_LAZY_CALL_COUNT) {
        return;
    }
    const uint64_t nanoseconds = SDL_GetTicksNS() - started_ticks;
    uint8_t call = 0u;
    BTBHGPUWindow *window = btbh_gpu_lazy_recording(&call);
    if (window == NULL) {
        return;
    }
    static const char *const names[BTBH_GPU_LAZY_CALL_COUNT] = {
        "SDL_AcquireGPUCommandBuffer (scene)",
        "SDL_MapGPUTransferBuffer (scene upload)",
        "SDL_SubmitGPUCommandBuffer (render to target)",
        "SDL_AcquireGPUCommandBuffer (canvas)",
        "SDL_WaitAndAcquireGPUSwapchainTexture",
        "SDL_MapGPUTransferBuffer (canvas upload)",
        "SDL_SubmitGPUCommandBuffer (frame)",
    };
    double *usual = &window->lazy_creation->usual_call_milliseconds[timed_call];
    const double usual_before = *usual;
    const double milliseconds = (double)nanoseconds / 1000000.0;
    /* The usual time follows the calls slowly: a stall stands out, a steady wait (vsync's) doesn't. */
    *usual = usual_before <= 0.0 ? milliseconds : usual_before + 0.1 * (milliseconds - usual_before);
    if (btbh_gpu_lazy_call_threshold_nanoseconds != 0u &&
        (double)nanoseconds < usual_before * 1000000.0 + (double)btbh_gpu_lazy_call_threshold_nanoseconds) {
        return;
    }
    char name[BTBH_GPU_LAZY_CREATION_NAME_CAPACITY];
    (void)snprintf(name, sizeof(name), "%s, usually %.3f ms", names[timed_call], usual_before);
    btbh_gpu_lazy_record(window, call, BTBH_GPU_LAZY_CREATION_SLOW_CALL, nanoseconds, name);
}

/* ---- Upload staging (since 0.11.0, btbh_gpu_device_set_upload_staging_budget) ---- */

/* The pool's transfer buffers ("chunks"): uploads take consecutive ranges of the open chunk; a chunk starts again from
   0 once no range is lent out and the GPU has finished every upload that read it. The uploads' fences are kept in
   submission order and released only once signalled: SDL hands a released fence to the next submission, which must
   not happen while the GPU may still signal it. One queue runs everything, so a signalled fence means every earlier
   submission has finished too. */
#define BTBH_GPU_STAGING_CHUNKS 16u
#define BTBH_GPU_STAGING_CHUNK_BYTES (4u * 1024u * 1024u)
/* A range's start (D3D12 places texture data at multiples of 512). */
#define BTBH_GPU_STAGING_ALIGNMENT 512u

typedef struct BTBHGPUStagingChunk {
    SDL_GPUTransferBuffer *transfer;  /* NULL: an empty slot */
    uint32_t size;
    uint32_t cursor;
    uint32_t lent;                    /* ranges acquired and not yet submitted or abandoned */
    uint64_t last_serial;             /* the last submission that read it (0: none) */
} BTBHGPUStagingChunk;

typedef struct BTBHGPUStagingFence {
    SDL_GPUFence *fence;
    uint64_t serial;
} BTBHGPUStagingFence;

/* One pool, for the device that set a budget (the renderer runs one). */
static SDL_SpinLock btbh_gpu_staging_lock;
static SDL_GPUDevice *btbh_gpu_staging_device;
static uint32_t btbh_gpu_staging_budget;
static uint32_t btbh_gpu_staging_pooled_bytes;
static uint32_t btbh_gpu_staging_open = BTBH_GPU_STAGING_CHUNKS;
static BTBHGPUStagingChunk btbh_gpu_staging_chunks[BTBH_GPU_STAGING_CHUNKS];
/* The submissions' fences not yet seen signalled, oldest first; the serials of the last submission and of the last
   one known finished. */
static BTBHGPUStagingFence *btbh_gpu_staging_fences;
static uint32_t btbh_gpu_staging_fence_count;
static uint32_t btbh_gpu_staging_fence_capacity;
static uint64_t btbh_gpu_staging_submitted_serial;
static uint64_t btbh_gpu_staging_finished_serial;

static SDL_GPUTransferBuffer *btbh_gpu_staging_create(SDL_GPUDevice *device, uint32_t size) {
    SDL_GPUTransferBufferCreateInfo info;
    SDL_zero(info);
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    info.size = size;
    return SDL_CreateGPUTransferBuffer(device, &info);
}

/* Releases the fences that have signalled, oldest first. (Lock held.) */
static void btbh_gpu_staging_poll(void) {
    uint32_t finished = 0u;
    while (finished < btbh_gpu_staging_fence_count &&
           SDL_QueryGPUFence(btbh_gpu_staging_device, btbh_gpu_staging_fences[finished].fence)) {
        SDL_ReleaseGPUFence(btbh_gpu_staging_device, btbh_gpu_staging_fences[finished].fence);
        btbh_gpu_staging_finished_serial = btbh_gpu_staging_fences[finished].serial;
        finished += 1u;
    }
    if (finished > 0u) {
        btbh_gpu_staging_fence_count -= finished;
        SDL_memmove(
            btbh_gpu_staging_fences,
            btbh_gpu_staging_fences + finished,
            btbh_gpu_staging_fence_count * sizeof(*btbh_gpu_staging_fences)
        );
    }
}

/* Whether chunk `index` can start again from 0: nothing lent, every upload that read it finished. (Lock held,
   polled.) */
static bool btbh_gpu_staging_idle(uint32_t index) {
    const BTBHGPUStagingChunk *chunk = &btbh_gpu_staging_chunks[index];
    return chunk->transfer != NULL && chunk->lent == 0u && chunk->last_serial <= btbh_gpu_staging_finished_serial;
}

/* Drops chunk `index` from the pool: SDL frees its buffer once the GPU is done with it. (Lock held.) */
static void btbh_gpu_staging_drop(uint32_t index) {
    BTBHGPUStagingChunk *chunk = &btbh_gpu_staging_chunks[index];
    SDL_ReleaseGPUTransferBuffer(btbh_gpu_staging_device, chunk->transfer);
    btbh_gpu_staging_pooled_bytes -= chunk->size;
    SDL_zerop(chunk);
    if (btbh_gpu_staging_open == index) {
        btbh_gpu_staging_open = BTBH_GPU_STAGING_CHUNKS;
    }
}

/* Drops chunks nothing is lent from, largest first, until the pool holds at most `target` bytes. (Lock held.) */
static void btbh_gpu_staging_trim(uint32_t target) {
    while (btbh_gpu_staging_pooled_bytes > target) {
        uint32_t largest = BTBH_GPU_STAGING_CHUNKS;
        for (uint32_t index = 0u; index < BTBH_GPU_STAGING_CHUNKS; ++index) {
            const BTBHGPUStagingChunk *chunk = &btbh_gpu_staging_chunks[index];
            if (chunk->transfer != NULL && chunk->lent == 0u &&
                (largest == BTBH_GPU_STAGING_CHUNKS || chunk->size > btbh_gpu_staging_chunks[largest].size)) {
                largest = index;
            }
        }
        if (largest == BTBH_GPU_STAGING_CHUNKS) {
            return;
        }
        btbh_gpu_staging_drop(largest);
    }
}

BTBHGPUResult btbh_gpu_device_set_upload_staging_budget(
    BTBHGPUDevice *device,
    uint32_t bytes
) {
    if (device == NULL || device->gpu == NULL || device->shutdown) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    SDL_LockSpinlock(&btbh_gpu_staging_lock);
    if (btbh_gpu_staging_device != NULL && btbh_gpu_staging_device != device->gpu) {
        /* Another device has the pool. */
        SDL_UnlockSpinlock(&btbh_gpu_staging_lock);
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    btbh_gpu_staging_device = device->gpu;
    btbh_gpu_staging_budget = bytes;
    btbh_gpu_staging_trim(bytes);
    SDL_UnlockSpinlock(&btbh_gpu_staging_lock);
    return BTBH_GPU_OK;
}

void btbh_gpu_staging_release_device(SDL_GPUDevice *device) {
    SDL_LockSpinlock(&btbh_gpu_staging_lock);
    const bool pooled = device != NULL && btbh_gpu_staging_device == device;
    SDL_UnlockSpinlock(&btbh_gpu_staging_lock);
    if (!pooled) {
        return;
    }
    /* (The device goes: what it still runs finishes first, so that every fence is released signalled.) */
    (void)SDL_WaitForGPUIdle(device);
    SDL_LockSpinlock(&btbh_gpu_staging_lock);
    for (uint32_t index = 0u; index < BTBH_GPU_STAGING_CHUNKS; ++index) {
        if (btbh_gpu_staging_chunks[index].transfer != NULL) {
            btbh_gpu_staging_drop(index);
        }
    }
    for (uint32_t index = 0u; index < btbh_gpu_staging_fence_count; ++index) {
        SDL_ReleaseGPUFence(device, btbh_gpu_staging_fences[index].fence);
    }
    SDL_free(btbh_gpu_staging_fences);
    btbh_gpu_staging_fences = NULL;
    btbh_gpu_staging_fence_count = 0u;
    btbh_gpu_staging_fence_capacity = 0u;
    btbh_gpu_staging_device = NULL;
    btbh_gpu_staging_budget = 0u;
    btbh_gpu_staging_pooled_bytes = 0u;
    btbh_gpu_staging_open = BTBH_GPU_STAGING_CHUNKS;
    SDL_UnlockSpinlock(&btbh_gpu_staging_lock);
}

/* The chunk that is `transfer` with a range lent out, or BTBH_GPU_STAGING_CHUNKS. (Lock held.) */
static uint32_t btbh_gpu_staging_lending_chunk(SDL_GPUDevice *device, const SDL_GPUTransferBuffer *transfer) {
    if (device == NULL || device != btbh_gpu_staging_device || transfer == NULL) {
        return BTBH_GPU_STAGING_CHUNKS;
    }
    for (uint32_t index = 0u; index < BTBH_GPU_STAGING_CHUNKS; ++index) {
        if (btbh_gpu_staging_chunks[index].transfer == transfer && btbh_gpu_staging_chunks[index].lent > 0u) {
            return index;
        }
    }
    return BTBH_GPU_STAGING_CHUNKS;
}

/* Lends `size` bytes at the chunk's cursor. (Lock held.) */
static SDL_GPUTransferBuffer *btbh_gpu_staging_lend(uint32_t index, uint32_t size, uint32_t *offset) {
    BTBHGPUStagingChunk *chunk = &btbh_gpu_staging_chunks[index];
    *offset = chunk->cursor;
    const uint64_t next = ((uint64_t)chunk->cursor + size + BTBH_GPU_STAGING_ALIGNMENT - 1u) &
        ~(uint64_t)(BTBH_GPU_STAGING_ALIGNMENT - 1u);
    chunk->cursor = next < chunk->size ? (uint32_t)next : chunk->size;
    chunk->lent += 1u;
    btbh_gpu_staging_open = index;
    return chunk->transfer;
}

SDL_GPUTransferBuffer *btbh_gpu_staging_acquire(SDL_GPUDevice *device, uint32_t size, uint32_t *offset) {
    *offset = 0u;
    SDL_LockSpinlock(&btbh_gpu_staging_lock);
    if (device == NULL || device != btbh_gpu_staging_device || btbh_gpu_staging_budget == 0u || size == 0u ||
        size > btbh_gpu_staging_budget) {
        SDL_UnlockSpinlock(&btbh_gpu_staging_lock);
        return btbh_gpu_staging_create(device, size);
    }
    /* The open chunk, while the range fits after its cursor: its earlier ranges may still be read, not these bytes. */
    const uint32_t open = btbh_gpu_staging_open;
    if (open < BTBH_GPU_STAGING_CHUNKS &&
        btbh_gpu_staging_chunks[open].size - btbh_gpu_staging_chunks[open].cursor >= size) {
        SDL_GPUTransferBuffer *transfer = btbh_gpu_staging_lend(open, size, offset);
        SDL_UnlockSpinlock(&btbh_gpu_staging_lock);
        return transfer;
    }
    /* Else the smallest chunk big enough that the GPU is done with, from its start. */
    btbh_gpu_staging_poll();
    uint32_t best = BTBH_GPU_STAGING_CHUNKS;
    for (uint32_t index = 0u; index < BTBH_GPU_STAGING_CHUNKS; ++index) {
        if (btbh_gpu_staging_idle(index) && btbh_gpu_staging_chunks[index].size >= size &&
            (best == BTBH_GPU_STAGING_CHUNKS ||
             btbh_gpu_staging_chunks[index].size < btbh_gpu_staging_chunks[best].size)) {
            best = index;
        }
    }
    if (best != BTBH_GPU_STAGING_CHUNKS) {
        btbh_gpu_staging_chunks[best].cursor = 0u;
        SDL_GPUTransferBuffer *transfer = btbh_gpu_staging_lend(best, size, offset);
        SDL_UnlockSpinlock(&btbh_gpu_staging_lock);
        return transfer;
    }
    /* Else a new chunk in the budget (idle ones too small make room): the default size, or a power of two that holds
       the range. */
    uint32_t chunk_size = BTBH_GPU_STAGING_CHUNK_BYTES;
    while (chunk_size < size && chunk_size <= UINT32_MAX / 2u) {
        chunk_size *= 2u;
    }
    if (chunk_size > btbh_gpu_staging_budget) {
        chunk_size = size;
    }
    if (btbh_gpu_staging_pooled_bytes + chunk_size > btbh_gpu_staging_budget) {
        btbh_gpu_staging_trim(btbh_gpu_staging_budget - chunk_size);
    }
    uint32_t empty = BTBH_GPU_STAGING_CHUNKS;
    for (uint32_t index = 0u; index < BTBH_GPU_STAGING_CHUNKS && empty == BTBH_GPU_STAGING_CHUNKS; ++index) {
        if (btbh_gpu_staging_chunks[index].transfer == NULL) {
            empty = index;
        }
    }
    if (empty == BTBH_GPU_STAGING_CHUNKS || btbh_gpu_staging_pooled_bytes + chunk_size > btbh_gpu_staging_budget) {
        /* The pool is busy: an upload buffer of its own, as without a budget. */
        SDL_UnlockSpinlock(&btbh_gpu_staging_lock);
        return btbh_gpu_staging_create(device, size);
    }
    SDL_GPUTransferBuffer *transfer = btbh_gpu_staging_create(device, chunk_size);
    if (transfer != NULL) {
        BTBHGPUStagingChunk *chunk = &btbh_gpu_staging_chunks[empty];
        SDL_zerop(chunk);
        chunk->transfer = transfer;
        chunk->size = chunk_size;
        btbh_gpu_staging_pooled_bytes += chunk_size;
        transfer = btbh_gpu_staging_lend(empty, size, offset);
    }
    SDL_UnlockSpinlock(&btbh_gpu_staging_lock);
    return transfer;
}

SDL_GPUTransferBuffer *btbh_gpu_staging_acquire_into(
    SDL_GPUDevice *device,
    SDL_GPUCommandBuffer *into,
    uint32_t size,
    uint32_t *offset
) {
    if (into == NULL) {
        return btbh_gpu_staging_acquire(device, size, offset);
    }
    /* Recorded into a command buffer someone else submits (a frame waiting for the canvas): the pool would never see
       that submission's fence, so the upload has a buffer of its own, released once recorded (SDL frees it once the
       frame has run). */
    *offset = 0u;
    return btbh_gpu_staging_create(device, size);
}

bool btbh_gpu_staging_submit_into(
    SDL_GPUDevice *device,
    SDL_GPUCommandBuffer *command,
    SDL_GPUCommandBuffer *into,
    SDL_GPUTransferBuffer *const *transfers,
    uint32_t transfer_count
) {
    if (into != NULL) {
        for (uint32_t index = 0u; index < transfer_count; ++index) {
            if (transfers[index] != NULL) {
                SDL_ReleaseGPUTransferBuffer(device, transfers[index]);
            }
        }
        return true;
    }
    /* Which of the transfer buffers (at most 4) are pooled ranges: a chunk once for each range it lent. */
    uint32_t chunks[4];
    uint32_t pooled = 0u;
    transfer_count = SDL_min(transfer_count, (uint32_t)SDL_arraysize(chunks));
    SDL_LockSpinlock(&btbh_gpu_staging_lock);
    for (uint32_t index = 0u; index < transfer_count; ++index) {
        chunks[index] = btbh_gpu_staging_lending_chunk(device, transfers[index]);
        pooled += chunks[index] != BTBH_GPU_STAGING_CHUNKS ? 1u : 0u;
    }
    SDL_UnlockSpinlock(&btbh_gpu_staging_lock);
    if (pooled == 0u) {
        const bool submitted = SDL_SubmitGPUCommandBuffer(command);
        for (uint32_t index = 0u; index < transfer_count; ++index) {
            if (transfers[index] != NULL) {
                SDL_ReleaseGPUTransferBuffer(device, transfers[index]);
            }
        }
        return submitted;
    }
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
    for (uint32_t index = 0u; index < transfer_count; ++index) {
        if (transfers[index] != NULL && chunks[index] == BTBH_GPU_STAGING_CHUNKS) {
            SDL_ReleaseGPUTransferBuffer(device, transfers[index]);
        }
    }
    SDL_LockSpinlock(&btbh_gpu_staging_lock);
    for (uint32_t index = 0u; index < transfer_count; ++index) {
        if (chunks[index] != BTBH_GPU_STAGING_CHUNKS) {
            btbh_gpu_staging_chunks[chunks[index]].lent -= 1u;
        }
    }
    if (fence == NULL) {
        /* (A failed submit: its chunks aren't trusted again.) */
        for (uint32_t index = 0u; index < transfer_count; ++index) {
            const uint32_t chunk_index = chunks[index];
            if (chunk_index == BTBH_GPU_STAGING_CHUNKS || btbh_gpu_staging_chunks[chunk_index].transfer == NULL) {
                continue;
            }
            BTBHGPUStagingChunk *chunk = &btbh_gpu_staging_chunks[chunk_index];
            if (chunk->lent == 0u) {
                btbh_gpu_staging_drop(chunk_index);
            } else {
                chunk->cursor = chunk->size;
                chunk->last_serial = UINT64_MAX;
            }
        }
        SDL_UnlockSpinlock(&btbh_gpu_staging_lock);
        return false;
    }
    if (btbh_gpu_staging_fence_count == btbh_gpu_staging_fence_capacity) {
        const uint32_t capacity = btbh_gpu_staging_fence_capacity == 0u ? 64u : btbh_gpu_staging_fence_capacity * 2u;
        BTBHGPUStagingFence *fences =
            SDL_realloc(btbh_gpu_staging_fences, capacity * sizeof(*btbh_gpu_staging_fences));
        if (fences == NULL) {
            /* (Out of memory: this upload is waited for instead.) */
            SDL_UnlockSpinlock(&btbh_gpu_staging_lock);
            (void)SDL_WaitForGPUFences(device, true, &fence, 1u);
            SDL_ReleaseGPUFence(device, fence);
            return true;
        }
        btbh_gpu_staging_fences = fences;
        btbh_gpu_staging_fence_capacity = capacity;
    }
    btbh_gpu_staging_submitted_serial += 1u;
    btbh_gpu_staging_fences[btbh_gpu_staging_fence_count].fence = fence;
    btbh_gpu_staging_fences[btbh_gpu_staging_fence_count].serial = btbh_gpu_staging_submitted_serial;
    btbh_gpu_staging_fence_count += 1u;
    for (uint32_t index = 0u; index < transfer_count; ++index) {
        if (chunks[index] != BTBH_GPU_STAGING_CHUNKS) {
            btbh_gpu_staging_chunks[chunks[index]].last_serial = btbh_gpu_staging_submitted_serial;
        }
    }
    btbh_gpu_staging_poll();
    /* (A budget lowered meanwhile.) */
    btbh_gpu_staging_trim(btbh_gpu_staging_budget);
    SDL_UnlockSpinlock(&btbh_gpu_staging_lock);
    return true;
}

bool btbh_gpu_staging_submit(
    SDL_GPUDevice *device,
    SDL_GPUCommandBuffer *command,
    SDL_GPUTransferBuffer *transfer
) {
    return btbh_gpu_staging_submit_into(device, command, NULL, &transfer, 1u);
}

void btbh_gpu_staging_abandon(SDL_GPUDevice *device, SDL_GPUTransferBuffer *transfer) {
    if (transfer == NULL) {
        return;
    }
    SDL_LockSpinlock(&btbh_gpu_staging_lock);
    const uint32_t index = btbh_gpu_staging_lending_chunk(device, transfer);
    if (index != BTBH_GPU_STAGING_CHUNKS) {
        /* (Its range stays used until the chunk starts again.) */
        btbh_gpu_staging_chunks[index].lent -= 1u;
        btbh_gpu_staging_trim(btbh_gpu_staging_budget);
    }
    SDL_UnlockSpinlock(&btbh_gpu_staging_lock);
    if (index == BTBH_GPU_STAGING_CHUNKS) {
        SDL_ReleaseGPUTransferBuffer(device, transfer);
    }
}

void btbh_gpu_creation_stats(BTBHGPUCreationStats *stats) {
    if (stats == NULL) {
        return;
    }
    SDL_LockSpinlock(&btbh_gpu_creation_lock);
    *stats = btbh_gpu_creation_totals;
    SDL_UnlockSpinlock(&btbh_gpu_creation_lock);
}
