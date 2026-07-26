#include "btbh_gpu_internal.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct BTBHGPUWindow {
    SDL_Window *window;
    BTBHGPUDevice *device;
    BTBHGPUTextureFormat swapchain_format;
    BTBHGPUPresentMode present_mode;
    BTBHGPUSwapchainComposition composition;
    bool owns_video_subsystem;
    bool claimed;
};

static void btbh_gpu_set_window_error(
    BTBHGPUWindowReport *report,
    BTBHGPUErrorStage stage,
    const char *message
) {
    report->result = BTBH_GPU_WINDOW_FAILURE;
    report->error_stage = stage;
    (void) snprintf(
        report->message,
        sizeof(report->message),
        "%s",
        message != NULL ? message : ""
    );
}

static bool btbh_gpu_valid_window_size(uint32_t width, uint32_t height) {
    return
        width > 0 &&
        height > 0 &&
        width <= (uint32_t) INT_MAX &&
        height <= (uint32_t) INT_MAX;
}

static void btbh_gpu_write_window_state(
    const BTBHGPUWindow *window,
    BTBHGPUWindowReport *report
) {
    report->claimed = window->claimed ? 1u : 0u;
    report->swapchain_format = window->swapchain_format;
    report->present_mode = window->present_mode;
    report->composition = window->composition;
}

static bool btbh_gpu_refresh_window_sizes(
    BTBHGPUWindow *window,
    BTBHGPUWindowReport *report
) {
    int logical_width = 0;
    int logical_height = 0;
    int drawable_width = 0;
    int drawable_height = 0;
    if (
        !SDL_GetWindowSize(
            window->window,
            &logical_width,
            &logical_height
        ) ||
        !SDL_GetWindowSizeInPixels(
            window->window,
            &drawable_width,
            &drawable_height
        ) ||
        logical_width <= 0 ||
        logical_height <= 0 ||
        drawable_width <= 0 ||
        drawable_height <= 0
    ) {
        return false;
    }
    report->logical_width = (uint32_t) logical_width;
    report->logical_height = (uint32_t) logical_height;
    report->drawable_width = (uint32_t) drawable_width;
    report->drawable_height = (uint32_t) drawable_height;
    const SDL_WindowFlags flags = SDL_GetWindowFlags(window->window);
    report->minimized =
        (flags & SDL_WINDOW_MINIMIZED) != 0 ? 1u : 0u;
    report->hidden = (flags & SDL_WINDOW_HIDDEN) != 0 ? 1u : 0u;
    btbh_gpu_write_window_state(window, report);
    return true;
}

static BTBHGPUTextureFormat btbh_gpu_texture_format(
    SDL_GPUTextureFormat format
) {
    switch (format) {
        case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM:
            return BTBH_GPU_TEXTURE_FORMAT_RGBA8_UNORM;
        case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB:
            return BTBH_GPU_TEXTURE_FORMAT_RGBA8_SRGB;
        case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM:
            return BTBH_GPU_TEXTURE_FORMAT_BGRA8_UNORM;
        case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB:
            return BTBH_GPU_TEXTURE_FORMAT_BGRA8_SRGB;
        case SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM:
            return BTBH_GPU_TEXTURE_FORMAT_RGB10A2_UNORM;
        case SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT:
            return BTBH_GPU_TEXTURE_FORMAT_RGBA16_FLOAT;
        default:
            return BTBH_GPU_TEXTURE_FORMAT_INVALID;
    }
}

static SDL_GPUPresentMode btbh_gpu_choose_present_mode(
    BTBHGPUDevice *device,
    BTBHGPUWindow *window,
    BTBHGPUPresentMode *project_mode
) {
    if (SDL_WindowSupportsGPUPresentMode(
        device->gpu,
        window->window,
        SDL_GPU_PRESENTMODE_IMMEDIATE
    )) {
        *project_mode = BTBH_GPU_PRESENT_MODE_IMMEDIATE;
        return SDL_GPU_PRESENTMODE_IMMEDIATE;
    }
    if (SDL_WindowSupportsGPUPresentMode(
        device->gpu,
        window->window,
        SDL_GPU_PRESENTMODE_MAILBOX
    )) {
        *project_mode = BTBH_GPU_PRESENT_MODE_MAILBOX;
        return SDL_GPU_PRESENTMODE_MAILBOX;
    }
    *project_mode = BTBH_GPU_PRESENT_MODE_VSYNC;
    return SDL_GPU_PRESENTMODE_VSYNC;
}

BTBHGPUWindow *btbh_gpu_window_create(
    const BTBHGPUWindowDescriptor *descriptor,
    BTBHGPUWindowReport *report
) {
    if (descriptor == NULL || report == NULL) {
        return NULL;
    }
    memset(report, 0, sizeof(*report));
    if (
        descriptor->title == NULL ||
        !btbh_gpu_valid_window_size(descriptor->width, descriptor->height)
    ) {
        btbh_gpu_set_window_error(
            report,
            BTBH_GPU_ERROR_STAGE_WINDOW_CREATION,
            "Invalid GPU window descriptor"
        );
        return NULL;
    }

    BTBHGPUWindow *window = calloc(1, sizeof(*window));
    if (window == NULL) {
        btbh_gpu_set_window_error(
            report,
            BTBH_GPU_ERROR_STAGE_WINDOW_CREATION,
            "Could not allocate the GPU window context"
        );
        return NULL;
    }

    window->owns_video_subsystem =
        (SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) == 0;
    if (
        window->owns_video_subsystem &&
        !SDL_InitSubSystem(SDL_INIT_VIDEO)
    ) {
        btbh_gpu_set_window_error(
            report,
            BTBH_GPU_ERROR_STAGE_PLATFORM_INITIALIZATION,
            SDL_GetError()
        );
        free(window);
        return NULL;
    }

    SDL_WindowFlags flags = 0;
    if (descriptor->hidden != 0) {
        flags |= SDL_WINDOW_HIDDEN;
    }
    if (descriptor->high_pixel_density != 0) {
        flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
    }
    window->window = SDL_CreateWindow(
        descriptor->title,
        (int) descriptor->width,
        (int) descriptor->height,
        flags
    );
    if (window->window == NULL) {
        btbh_gpu_set_window_error(
            report,
            BTBH_GPU_ERROR_STAGE_WINDOW_CREATION,
            SDL_GetError()
        );
        btbh_gpu_window_destroy(window);
        return NULL;
    }
    if (!btbh_gpu_refresh_window_sizes(window, report)) {
        btbh_gpu_set_window_error(
            report,
            BTBH_GPU_ERROR_STAGE_WINDOW_CREATION,
            SDL_GetError()
        );
        btbh_gpu_window_destroy(window);
        return NULL;
    }

    report->result = BTBH_GPU_OK;
    return window;
}

BTBHGPUResult btbh_gpu_window_claim(
    BTBHGPUDevice *device,
    BTBHGPUWindow *window,
    BTBHGPUWindowReport *report
) {
    if (
        device == NULL ||
        device->gpu == NULL ||
        device->shutdown ||
        window == NULL ||
        window->window == NULL ||
        report == NULL ||
        window->claimed
    ) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }

    memset(report, 0, sizeof(*report));
    if (!SDL_ClaimWindowForGPUDevice(device->gpu, window->window)) {
        btbh_gpu_set_window_error(
            report,
            BTBH_GPU_ERROR_STAGE_WINDOW_CLAIM,
            SDL_GetError()
        );
        return report->result;
    }

    window->device = device;
    window->claimed = true;
    BTBHGPUPresentMode project_present_mode =
        BTBH_GPU_PRESENT_MODE_INVALID;
    const SDL_GPUPresentMode present_mode = btbh_gpu_choose_present_mode(
        device,
        window,
        &project_present_mode
    );
    if (
        !SDL_WindowSupportsGPUSwapchainComposition(
            device->gpu,
            window->window,
            SDL_GPU_SWAPCHAINCOMPOSITION_SDR
        ) ||
        !SDL_SetGPUSwapchainParameters(
            device->gpu,
            window->window,
            SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
            present_mode
        )
    ) {
        btbh_gpu_set_window_error(
            report,
            BTBH_GPU_ERROR_STAGE_SWAPCHAIN_CONFIGURATION,
            SDL_GetError()
        );
        SDL_ReleaseWindowFromGPUDevice(device->gpu, window->window);
        window->device = NULL;
        window->claimed = false;
        return report->result;
    }

    const BTBHGPUTextureFormat swapchain_format = btbh_gpu_texture_format(
        SDL_GetGPUSwapchainTextureFormat(device->gpu, window->window)
    );
    if (
        swapchain_format == BTBH_GPU_TEXTURE_FORMAT_INVALID ||
        !btbh_gpu_refresh_window_sizes(window, report)
    ) {
        btbh_gpu_set_window_error(
            report,
            BTBH_GPU_ERROR_STAGE_SWAPCHAIN_CONFIGURATION,
            swapchain_format == BTBH_GPU_TEXTURE_FORMAT_INVALID
                ? "SDL_GPU selected an unsupported swapchain format"
                : SDL_GetError()
        );
        SDL_ReleaseWindowFromGPUDevice(device->gpu, window->window);
        window->device = NULL;
        window->claimed = false;
        return report->result;
    }

    window->swapchain_format = swapchain_format;
    window->present_mode = project_present_mode;
    window->composition = BTBH_GPU_SWAPCHAIN_SDR;
    btbh_gpu_write_window_state(window, report);
    report->result = BTBH_GPU_OK;
    return report->result;
}

BTBHGPUResult btbh_gpu_window_resize(
    BTBHGPUWindow *window,
    uint32_t width,
    uint32_t height,
    BTBHGPUWindowReport *report
) {
    if (
        window == NULL ||
        window->window == NULL ||
        report == NULL ||
        !btbh_gpu_valid_window_size(width, height)
    ) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }

    if (
        !SDL_SetWindowSize(window->window, (int) width, (int) height) ||
        !SDL_SyncWindow(window->window)
    ) {
        btbh_gpu_set_window_error(
            report,
            BTBH_GPU_ERROR_STAGE_WINDOW_RESIZE,
            SDL_GetError()
        );
        return report->result;
    }
    SDL_PumpEvents();
    if (!btbh_gpu_refresh_window_sizes(window, report)) {
        btbh_gpu_set_window_error(
            report,
            BTBH_GPU_ERROR_STAGE_WINDOW_RESIZE,
            SDL_GetError()
        );
        return report->result;
    }

    report->result = BTBH_GPU_OK;
    report->error_stage = BTBH_GPU_ERROR_STAGE_NONE;
    report->message[0] = '\0';
    return report->result;
}

static BTBHGPUResult btbh_gpu_window_set_minimized(
    BTBHGPUWindow *window,
    bool minimized,
    BTBHGPUWindowReport *report
) {
    if (window == NULL || window->window == NULL || report == NULL) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }

    const bool changed = minimized
        ? SDL_MinimizeWindow(window->window)
        : SDL_RestoreWindow(window->window);
    if (!changed || !SDL_SyncWindow(window->window)) {
        btbh_gpu_set_window_error(
            report,
            BTBH_GPU_ERROR_STAGE_WINDOW_RESIZE,
            SDL_GetError()
        );
        return report->result;
    }
    SDL_PumpEvents();
    if (!btbh_gpu_refresh_window_sizes(window, report) ||
        (report->minimized != 0) != minimized) {
        btbh_gpu_set_window_error(
            report,
            BTBH_GPU_ERROR_STAGE_WINDOW_RESIZE,
            "Window minimize/restore state did not converge"
        );
        return report->result;
    }
    report->result = BTBH_GPU_OK;
    report->error_stage = BTBH_GPU_ERROR_STAGE_NONE;
    report->message[0] = '\0';
    return report->result;
}

BTBHGPUResult btbh_gpu_window_minimize(
    BTBHGPUWindow *window,
    BTBHGPUWindowReport *report
) {
    return btbh_gpu_window_set_minimized(window, true, report);
}

BTBHGPUResult btbh_gpu_window_restore(
    BTBHGPUWindow *window,
    BTBHGPUWindowReport *report
) {
    return btbh_gpu_window_set_minimized(window, false, report);
}

BTBHGPUResult btbh_gpu_window_present_clear(
    BTBHGPUWindow *window,
    uint8_t red,
    uint8_t green,
    uint8_t blue,
    uint8_t alpha,
    BTBHGPUWindowReport *report
) {
    if (
        window == NULL ||
        window->window == NULL ||
        window->device == NULL ||
        window->device->gpu == NULL ||
        !window->claimed ||
        report == NULL
    ) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }

    SDL_GPUCommandBuffer *command_buffer =
        SDL_AcquireGPUCommandBuffer(window->device->gpu);
    if (command_buffer == NULL) {
        btbh_gpu_set_window_error(
            report,
            BTBH_GPU_ERROR_STAGE_SWAPCHAIN_CONFIGURATION,
            SDL_GetError()
        );
        return report->result;
    }

    SDL_GPUTexture *swapchain_texture = NULL;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(
        command_buffer,
        window->window,
        &swapchain_texture,
        NULL,
        NULL
    )) {
        btbh_gpu_set_window_error(
            report,
            BTBH_GPU_ERROR_STAGE_SWAPCHAIN_CONFIGURATION,
            SDL_GetError()
        );
        (void) SDL_CancelGPUCommandBuffer(command_buffer);
        return report->result;
    }
    if (swapchain_texture == NULL) {
        (void) SDL_CancelGPUCommandBuffer(command_buffer);
        btbh_gpu_set_window_error(
            report,
            BTBH_GPU_ERROR_STAGE_SWAPCHAIN_CONFIGURATION,
            "SDL_GPU swapchain is unavailable"
        );
        return report->result;
    }

    SDL_GPUColorTargetInfo color_target = {0};
    color_target.texture = swapchain_texture;
    color_target.clear_color.r = (float) red / 255.0f;
    color_target.clear_color.g = (float) green / 255.0f;
    color_target.clear_color.b = (float) blue / 255.0f;
    color_target.clear_color.a = (float) alpha / 255.0f;
    color_target.load_op = SDL_GPU_LOADOP_CLEAR;
    color_target.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass *render_pass = SDL_BeginGPURenderPass(
        command_buffer,
        &color_target,
        1,
        NULL
    );
    if (render_pass == NULL) {
        btbh_gpu_set_window_error(
            report,
            BTBH_GPU_ERROR_STAGE_SWAPCHAIN_CONFIGURATION,
            SDL_GetError()
        );
        (void) SDL_SubmitGPUCommandBuffer(command_buffer);
        return report->result;
    }
    SDL_EndGPURenderPass(render_pass);

    if (!SDL_SubmitGPUCommandBuffer(command_buffer)) {
        btbh_gpu_set_window_error(
            report,
            BTBH_GPU_ERROR_STAGE_SWAPCHAIN_CONFIGURATION,
            SDL_GetError()
        );
        return report->result;
    }

    if (!btbh_gpu_refresh_window_sizes(window, report)) {
        btbh_gpu_set_window_error(
            report,
            BTBH_GPU_ERROR_STAGE_SWAPCHAIN_CONFIGURATION,
            SDL_GetError()
        );
        return report->result;
    }

    report->result = BTBH_GPU_OK;
    report->error_stage = BTBH_GPU_ERROR_STAGE_NONE;
    report->message[0] = '\0';
    return report->result;
}

void btbh_gpu_window_destroy(BTBHGPUWindow *window) {
    if (window == NULL) {
        return;
    }
    if (
        window->claimed &&
        window->device != NULL &&
        window->device->gpu != NULL
    ) {
        SDL_ReleaseWindowFromGPUDevice(
            window->device->gpu,
            window->window
        );
    }
    if (
        window->owns_video_subsystem &&
        window->device != NULL &&
        !window->device->shutdown
    ) {
        window->device->owns_video_subsystem = true;
        window->owns_video_subsystem = false;
    }
    if (window->window != NULL) {
        SDL_DestroyWindow(window->window);
        window->window = NULL;
    }
    if (window->owns_video_subsystem) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        window->owns_video_subsystem = false;
    }
    free(window);
}
