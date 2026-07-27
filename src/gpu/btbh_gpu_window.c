#include "btbh_gpu_internal.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    window->cursor_visible = true;
    window->last_frame_ticks = SDL_GetTicksNS();
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

    if (window->pending_command != NULL) {
        const bool recorded =
            !window->presented_frame_readback_requested ||
            btbh_gpu_window_record_presented_frame(window);
        const bool submitted = btbh_gpu_window_finish_pending_frame(
            window,
            recorded
        );
        if (!recorded || !submitted) {
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
    uint32_t drawable_width = 0u;
    uint32_t drawable_height = 0u;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(
        command_buffer,
        window->window,
        &swapchain_texture,
        &drawable_width,
        &drawable_height
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

    window->pending_command = command_buffer;
    window->pending_swapchain = swapchain_texture;
    window->pending_drawable_width = drawable_width;
    window->pending_drawable_height = drawable_height;
    const bool recorded =
        !window->presented_frame_readback_requested ||
        btbh_gpu_window_record_presented_frame(window);
    const bool submitted = btbh_gpu_window_finish_pending_frame(
        window,
        recorded
    );
    if (!recorded || !submitted) {
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

static void btbh_gpu_platform_snapshot(
    BTBHGPUWindow *window,
    BTBHGPUPlatformSnapshot *snapshot,
    float delta_seconds
) {
    float mouse_x = 0.0f;
    float mouse_y = 0.0f;
    (void) SDL_GetMouseState(&mouse_x, &mouse_y);

    int refresh_rate = 0;
    const SDL_DisplayID display = SDL_GetDisplayForWindow(window->window);
    const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(display);
    if (mode != NULL) {
        refresh_rate = (int) mode->refresh_rate;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->elapsed_seconds = btbh_gpu_monotonic_elapsed_seconds();
    snapshot->delta_seconds = delta_seconds;
    snapshot->mouse_x = mouse_x;
    snapshot->mouse_y = mouse_y;
    snapshot->mouse_delta_x = window->mouse_delta_x;
    snapshot->mouse_delta_y = window->mouse_delta_y;
    snapshot->mouse_wheel_x = window->mouse_wheel_x;
    snapshot->mouse_wheel_y = window->mouse_wheel_y;
    snapshot->refresh_rate = refresh_rate;
    snapshot->frames_per_second = delta_seconds > 0.0f
        ? (int32_t) (1.0f / delta_seconds)
        : 0;
    snapshot->should_close = window->should_close ? 1u : 0u;
    snapshot->focused =
        (SDL_GetWindowFlags(window->window) & SDL_WINDOW_INPUT_FOCUS) != 0
            ? 1u
            : 0u;
    snapshot->cursor_visible = window->cursor_visible ? 1u : 0u;
}

double btbh_gpu_monotonic_elapsed_seconds(void) {
    return (double) SDL_GetTicksNS() / 1000000000.0;
}

BTBHGPUResult btbh_gpu_window_poll_events(
    BTBHGPUWindow *window,
    BTBHGPUPlatformSnapshot *snapshot
) {
    if (window == NULL || window->window == NULL || snapshot == NULL) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }

    window->mouse_delta_x = 0.0f;
    window->mouse_delta_y = 0.0f;
    window->mouse_wheel_x = 0.0f;
    window->mouse_wheel_y = 0.0f;
    memset(window->key_pressed, 0, sizeof(window->key_pressed));
    memset(window->mouse_pressed, 0, sizeof(window->mouse_pressed));
    memset(window->mouse_released, 0, sizeof(window->mouse_released));
    const SDL_WindowID window_id = SDL_GetWindowID(window->window);
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) {
            window->should_close = true;
        } else if (
            event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
            event.window.windowID == window_id
        ) {
            window->should_close = true;
        } else if (
            event.type == SDL_EVENT_MOUSE_MOTION &&
            event.motion.windowID == window_id
        ) {
            window->mouse_delta_x += event.motion.xrel;
            window->mouse_delta_y += event.motion.yrel;
        } else if (
            event.type == SDL_EVENT_MOUSE_WHEEL &&
            event.wheel.windowID == window_id
        ) {
            const float direction =
                event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED
                    ? -1.0f
                    : 1.0f;
            window->mouse_wheel_x += event.wheel.x * direction;
            window->mouse_wheel_y += event.wheel.y * direction;
        } else if (
            event.type == SDL_EVENT_KEY_DOWN &&
            event.key.windowID == window_id &&
            !event.key.repeat &&
            event.key.scancode < SDL_SCANCODE_COUNT
        ) {
            window->key_pressed[event.key.scancode] = true;
        } else if (
            event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
            event.button.windowID == window_id &&
            event.button.button < 4
        ) {
            window->mouse_pressed[event.button.button] = true;
        } else if (
            event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
            event.button.windowID == window_id &&
            event.button.button < 4
        ) {
            window->mouse_released[event.button.button] = true;
        }
    }

    btbh_gpu_platform_snapshot(window, snapshot, 0.0f);
    return BTBH_GPU_OK;
}

BTBHGPUResult btbh_gpu_window_begin_frame(
    BTBHGPUWindow *window,
    BTBHGPUWindowReport *report,
    BTBHGPUPlatformSnapshot *snapshot
) {
    if (
        window == NULL ||
        window->window == NULL ||
        report == NULL ||
        snapshot == NULL
    ) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    if (!btbh_gpu_refresh_window_sizes(window, report)) {
        btbh_gpu_set_window_error(
            report,
            BTBH_GPU_ERROR_STAGE_WINDOW_RESIZE,
            SDL_GetError()
        );
        return report->result;
    }

    const uint64_t ticks = SDL_GetTicksNS();
    const float delta_seconds = window->last_frame_ticks == 0
        ? 0.0f
        : (float) (
            (double) (ticks - window->last_frame_ticks) /
            1000000000.0
        );
    window->last_frame_ticks = ticks;
    btbh_gpu_platform_snapshot(window, snapshot, delta_seconds);
    report->result = BTBH_GPU_OK;
    return BTBH_GPU_OK;
}

BTBHGPUResult btbh_gpu_window_set_mouse_position(
    BTBHGPUWindow *window,
    float x,
    float y
) {
    if (window == NULL || window->window == NULL) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    SDL_WarpMouseInWindow(window->window, x, y);
    return BTBH_GPU_OK;
}

BTBHGPUResult btbh_gpu_window_set_cursor_visible(
    BTBHGPUWindow *window,
    uint8_t visible
) {
    if (window == NULL || window->window == NULL) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    const bool succeeded = visible != 0
        ? SDL_ShowCursor()
        : SDL_HideCursor();
    if (!succeeded) {
        return BTBH_GPU_WINDOW_FAILURE;
    }
    window->cursor_visible = visible != 0;
    return BTBH_GPU_OK;
}

BTBHGPUResult btbh_gpu_window_set_clipboard_text(
    BTBHGPUWindow *window,
    const char *text
) {
    if (
        window == NULL ||
        window->window == NULL ||
        text == NULL
    ) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    return SDL_SetClipboardText(text)
        ? BTBH_GPU_OK
        : BTBH_GPU_WINDOW_FAILURE;
}

static SDL_Scancode btbh_gpu_project_key_scancode(uint16_t key) {
    static const SDL_Scancode scancodes[] = {
        SDL_SCANCODE_UP,
        SDL_SCANCODE_DOWN,
        SDL_SCANCODE_LEFT,
        SDL_SCANCODE_RIGHT,
        SDL_SCANCODE_RETURN,
        SDL_SCANCODE_KP_ENTER,
        SDL_SCANCODE_ESCAPE,
        SDL_SCANCODE_TAB,
        SDL_SCANCODE_SPACE,
        SDL_SCANCODE_BACKSPACE,
        SDL_SCANCODE_PAUSE,
        SDL_SCANCODE_LCTRL,
        SDL_SCANCODE_RCTRL,
        SDL_SCANCODE_LSHIFT,
        SDL_SCANCODE_RSHIFT,
        SDL_SCANCODE_LGUI,
        SDL_SCANCODE_RGUI,
        SDL_SCANCODE_F1,
        SDL_SCANCODE_F2,
        SDL_SCANCODE_F3,
        SDL_SCANCODE_F4,
        SDL_SCANCODE_F5,
        SDL_SCANCODE_F6,
        SDL_SCANCODE_F7,
        SDL_SCANCODE_F8,
        SDL_SCANCODE_F9,
        SDL_SCANCODE_F10,
        SDL_SCANCODE_F11,
        SDL_SCANCODE_F12,
        SDL_SCANCODE_KP_0,
        SDL_SCANCODE_KP_1,
        SDL_SCANCODE_KP_2,
        SDL_SCANCODE_KP_3,
        SDL_SCANCODE_KP_PLUS,
        SDL_SCANCODE_KP_MINUS,
        SDL_SCANCODE_1,
        SDL_SCANCODE_2,
        SDL_SCANCODE_3,
        SDL_SCANCODE_4,
        SDL_SCANCODE_5,
        SDL_SCANCODE_6,
        SDL_SCANCODE_7,
        SDL_SCANCODE_8,
        SDL_SCANCODE_A,
        SDL_SCANCODE_B,
        SDL_SCANCODE_C,
        SDL_SCANCODE_D,
        SDL_SCANCODE_E,
        SDL_SCANCODE_G,
        SDL_SCANCODE_H,
        SDL_SCANCODE_I,
        SDL_SCANCODE_J,
        SDL_SCANCODE_K,
        SDL_SCANCODE_M,
        SDL_SCANCODE_N,
        SDL_SCANCODE_O,
        SDL_SCANCODE_P,
        SDL_SCANCODE_Q,
        SDL_SCANCODE_R,
        SDL_SCANCODE_S,
        SDL_SCANCODE_U,
        SDL_SCANCODE_V,
        SDL_SCANCODE_W,
        SDL_SCANCODE_X,
        SDL_SCANCODE_Y,
        SDL_SCANCODE_Z,
        SDL_SCANCODE_APOSTROPHE,
        SDL_SCANCODE_SEMICOLON,
        SDL_SCANCODE_LEFTBRACKET,
        SDL_SCANCODE_RIGHTBRACKET,
        SDL_SCANCODE_MINUS,
        SDL_SCANCODE_EQUALS,
        SDL_SCANCODE_COMMA,
        SDL_SCANCODE_PERIOD
    };
    const size_t count = sizeof(scancodes) / sizeof(scancodes[0]);
    return key < count ? scancodes[key] : SDL_SCANCODE_UNKNOWN;
}

static uint8_t btbh_gpu_project_mouse_button(uint8_t button) {
    switch (button) {
        case 0:
            return SDL_BUTTON_LEFT;
        case 1:
            return SDL_BUTTON_RIGHT;
        case 2:
            return SDL_BUTTON_MIDDLE;
        default:
            return 0;
    }
}

uint8_t btbh_gpu_window_key_down(
    BTBHGPUWindow *window,
    uint16_t project_key
) {
    if (window == NULL || window->window == NULL) {
        return 0;
    }
    const SDL_Scancode scancode =
        btbh_gpu_project_key_scancode(project_key);
    int count = 0;
    const bool *state = SDL_GetKeyboardState(&count);
    return
        state != NULL &&
        scancode > SDL_SCANCODE_UNKNOWN &&
        (int) scancode < count &&
        state[scancode]
            ? 1u
            : 0u;
}

uint8_t btbh_gpu_window_key_pressed(
    BTBHGPUWindow *window,
    uint16_t project_key
) {
    if (window == NULL || window->window == NULL) {
        return 0;
    }
    const SDL_Scancode scancode =
        btbh_gpu_project_key_scancode(project_key);
    return
        scancode > SDL_SCANCODE_UNKNOWN &&
        scancode < SDL_SCANCODE_COUNT &&
        window->key_pressed[scancode]
            ? 1u
            : 0u;
}

uint8_t btbh_gpu_window_mouse_button_down(
    BTBHGPUWindow *window,
    uint8_t project_button
) {
    if (window == NULL || window->window == NULL) {
        return 0;
    }
    const uint8_t button = btbh_gpu_project_mouse_button(project_button);
    const SDL_MouseButtonFlags state = SDL_GetMouseState(NULL, NULL);
    return
        button != 0 &&
        (state & SDL_BUTTON_MASK(button)) != 0
            ? 1u
            : 0u;
}

uint8_t btbh_gpu_window_mouse_button_pressed(
    BTBHGPUWindow *window,
    uint8_t project_button
) {
    if (window == NULL || window->window == NULL) {
        return 0;
    }
    const uint8_t button = btbh_gpu_project_mouse_button(project_button);
    return button < 4 && window->mouse_pressed[button] ? 1u : 0u;
}

uint8_t btbh_gpu_window_mouse_button_released(
    BTBHGPUWindow *window,
    uint8_t project_button
) {
    if (window == NULL || window->window == NULL) {
        return 0;
    }
    const uint8_t button = btbh_gpu_project_mouse_button(project_button);
    return button < 4 && window->mouse_released[button] ? 1u : 0u;
}

void btbh_gpu_window_destroy(BTBHGPUWindow *window) {
    if (window == NULL) {
        return;
    }
    if (window->pending_command != NULL) {
        (void)btbh_gpu_window_finish_pending_frame(window, false);
    }
    btbh_gpu_window_release_canvas(window);
    btbh_gpu_window_release_scene(window);
    btbh_gpu_window_release_custom_meshes(window);
    btbh_gpu_window_release_instance_buffers(window);
    btbh_gpu_window_release_models(window);
    btbh_gpu_window_release_fonts(window);
    btbh_gpu_window_release_textures(window);
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
