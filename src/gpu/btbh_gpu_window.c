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

static const SDL_GamepadButton btbh_gpu_gamepad_buttons[] = {
    SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
    SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
    SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
    SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK,
    SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_GUIDE,
    SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
    SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT
};

_Static_assert(sizeof(BTBHGPUGamepadSnapshot) == 36, "Gamepad snapshot ABI");

static void btbh_gpu_close_gamepad(BTBHGPUWindow *window) {
    if (window->gamepad != NULL) {
        SDL_CloseGamepad(window->gamepad);
        window->gamepad = NULL;
    }
    memset(&window->gamepad_snapshot, 0, sizeof(window->gamepad_snapshot));
}

static void btbh_gpu_discover_gamepad(BTBHGPUWindow *window) {
    if (window->gamepad != NULL) { return; }
    int count = 0;
    SDL_JoystickID *identifiers = SDL_GetGamepads(&count);
    for (int index = 0; index < count; index += 1) {
        window->gamepad = SDL_OpenGamepad(identifiers[index]);
        if (window->gamepad != NULL) {
            window->gamepad_snapshot.instance_id = identifiers[index];
            break;
        }
    }
    SDL_free(identifiers);
}

static float btbh_gpu_gamepad_axis(SDL_Gamepad *gamepad, SDL_GamepadAxis axis) {
    const Sint16 value = SDL_GetGamepadAxis(gamepad, axis);
    return value >= 0 ? (float) value / 32767.0f : (float) value / 32768.0f;
}

static void btbh_gpu_sample_gamepad(BTBHGPUWindow *window) {
    if (window->gamepad == NULL) { return; }
    BTBHGPUGamepadSnapshot *state = &window->gamepad_snapshot;
    uint32_t down = 0;
    for (uint32_t index = 0; index < 15; index += 1) {
        if (SDL_GetGamepadButton(window->gamepad, btbh_gpu_gamepad_buttons[index])) {
            down |= 1u << index;
        }
    }
    state->buttons_pressed |= down & ~state->buttons_down;
    state->buttons_down = down;
    state->left_x = btbh_gpu_gamepad_axis(window->gamepad, SDL_GAMEPAD_AXIS_LEFTX);
    state->left_y = btbh_gpu_gamepad_axis(window->gamepad, SDL_GAMEPAD_AXIS_LEFTY);
    state->right_x = btbh_gpu_gamepad_axis(window->gamepad, SDL_GAMEPAD_AXIS_RIGHTX);
    state->right_y = btbh_gpu_gamepad_axis(window->gamepad, SDL_GAMEPAD_AXIS_RIGHTY);
    state->left_trigger = SDL_max(0.0f, btbh_gpu_gamepad_axis(window->gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
    state->right_trigger = SDL_max(0.0f, btbh_gpu_gamepad_axis(window->gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
}

static bool btbh_gpu_valid_window_size(uint32_t width, uint32_t height) {
    return
        width > 0 &&
        height > 0 &&
        width <= (uint32_t) INT_MAX &&
        height <= (uint32_t) INT_MAX;
}

typedef struct BTBHGPUFileDialogContext {
    BTBHGPUFileDialogCallback callback;
    void *userdata;
    SDL_PropertiesID properties;
    SDL_DialogFileFilter *filters;
    char **filter_names;
    char **filter_patterns;
    uint32_t filter_count;
} BTBHGPUFileDialogContext;

static void btbh_gpu_set_file_dialog_report(
    BTBHGPUFileDialogReport *report,
    BTBHGPUResult result,
    const char *message
) {
    if (report == NULL) {
        return;
    }
    memset(report, 0, sizeof(*report));
    report->result = result;
    (void) snprintf(
        report->message,
        sizeof(report->message),
        "%s",
        message != NULL ? message : ""
    );
}

static void btbh_gpu_free_file_dialog_context(
    BTBHGPUFileDialogContext *context
) {
    if (context == NULL) {
        return;
    }
    if (context->properties != 0) {
        SDL_DestroyProperties(context->properties);
    }
    for (uint32_t index = 0; index < context->filter_count; index += 1) {
        if (context->filter_names != NULL) {
            SDL_free(context->filter_names[index]);
        }
        if (context->filter_patterns != NULL) {
            SDL_free(context->filter_patterns[index]);
        }
    }
    free(context->filter_names);
    free(context->filter_patterns);
    free(context->filters);
    free(context);
}

static void SDLCALL btbh_gpu_file_dialog_callback(
    void *userdata,
    const char *const *paths,
    int selected_filter
) {
    (void) selected_filter;
    BTBHGPUFileDialogContext *context = userdata;
    if (context == NULL) {
        return;
    }
    const char *error_message = paths == NULL ? SDL_GetError() : NULL;
    context->callback(context->userdata, paths, error_message);
    btbh_gpu_free_file_dialog_context(context);
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

BTBHGPUPresentMode btbh_gpu_resolve_present_mode(
    int requested,
    bool supports_immediate,
    bool supports_mailbox
) {
    switch (requested) {
        case BTBH_GPU_PRESENT_MODE_INVALID:
            /* The default: the fastest mode the window supports (the only choice before 0.3.0). */
            if (supports_immediate) {
                return BTBH_GPU_PRESENT_MODE_IMMEDIATE;
            }
            return supports_mailbox
                ? BTBH_GPU_PRESENT_MODE_MAILBOX
                : BTBH_GPU_PRESENT_MODE_VSYNC;
        case BTBH_GPU_PRESENT_MODE_VSYNC:
            return BTBH_GPU_PRESENT_MODE_VSYNC;
        case BTBH_GPU_PRESENT_MODE_IMMEDIATE:
            return supports_immediate
                ? BTBH_GPU_PRESENT_MODE_IMMEDIATE
                : BTBH_GPU_PRESENT_MODE_VSYNC;
        case BTBH_GPU_PRESENT_MODE_MAILBOX:
            return supports_mailbox
                ? BTBH_GPU_PRESENT_MODE_MAILBOX
                : BTBH_GPU_PRESENT_MODE_VSYNC;
        default:
            return BTBH_GPU_PRESENT_MODE_INVALID;
    }
}

static SDL_GPUPresentMode btbh_gpu_sdl_present_mode(BTBHGPUPresentMode mode) {
    switch (mode) {
        case BTBH_GPU_PRESENT_MODE_IMMEDIATE:
            return SDL_GPU_PRESENTMODE_IMMEDIATE;
        case BTBH_GPU_PRESENT_MODE_MAILBOX:
            return SDL_GPU_PRESENTMODE_MAILBOX;
        default:
            return SDL_GPU_PRESENTMODE_VSYNC;
    }
}

/* The mode a claimed window takes for the request (a valid BTBHGPUPresentMode); SDL guarantees VSYNC. */
static BTBHGPUPresentMode btbh_gpu_choose_present_mode(
    BTBHGPUDevice *device,
    BTBHGPUWindow *window,
    BTBHGPUPresentMode requested
) {
    const bool supports_immediate = SDL_WindowSupportsGPUPresentMode(
        device->gpu,
        window->window,
        SDL_GPU_PRESENTMODE_IMMEDIATE
    );
    const bool supports_mailbox = SDL_WindowSupportsGPUPresentMode(
        device->gpu,
        window->window,
        SDL_GPU_PRESENTMODE_MAILBOX
    );
    return btbh_gpu_resolve_present_mode(
        (int) requested,
        supports_immediate,
        supports_mailbox
    );
}

static uint32_t btbh_gpu_default_allowed_frames_in_flight(void) {
#if defined(__APPLE__)
    return 2u;
#else
    return 3u;
#endif
}

uint32_t btbh_gpu_parse_allowed_frames_in_flight(const char *value) {
    const uint32_t platform_default =
        btbh_gpu_default_allowed_frames_in_flight();
    if (value == NULL || value[0] == '\0') {
        return platform_default;
    }

    char *end = NULL;
    const unsigned long parsed = strtoul(value, &end, 10);
    if (
        end == value ||
        end == NULL ||
        end[0] != '\0' ||
        parsed < 1ul ||
        parsed > 3ul
    ) {
        return platform_default;
    }
    return (uint32_t) parsed;
}

static uint32_t btbh_gpu_allowed_frames_in_flight(void) {
    return btbh_gpu_parse_allowed_frames_in_flight(
        SDL_getenv("BTBH_GPU_ALLOWED_FRAMES_IN_FLIGHT")
    );
}

/* Fits the width:height aspect into `percent` of the primary display. Display bounds are
   physical pixels on Windows (SDL is per-monitor DPI aware there), so the DPI scale never
   shrinks the window; the usable area keeps the title bar and the taskbar clear. The
   requested size stays when the display cannot be queried. */
static void btbh_gpu_fit_window_to_display(
    uint8_t percent,
    uint32_t *width,
    uint32_t *height
) {
    const SDL_DisplayID display = SDL_GetPrimaryDisplay();
    SDL_Rect bounds;
    SDL_Rect usable;
    if (display == 0 ||
        !SDL_GetDisplayBounds(display, &bounds) ||
        bounds.w <= 0 ||
        bounds.h <= 0) {
        return;
    }
    if (!SDL_GetDisplayUsableBounds(display, &usable) ||
        usable.w <= 0 ||
        usable.h <= 0) {
        usable = bounds;
    }
    const double fraction = (percent > 100u ? 100u : percent) / 100.0;
    double max_width = bounds.w * fraction;
    double max_height = bounds.h * fraction;
    if (max_width > usable.w * 0.98) {
        max_width = usable.w * 0.98;
    }
    if (max_height > usable.h * 0.92) {
        max_height = usable.h * 0.92;
    }
    const double aspect = (double) *width / (double) *height;
    double fitted_width = max_width;
    double fitted_height = fitted_width / aspect;
    if (fitted_height > max_height) {
        fitted_height = max_height;
        fitted_width = fitted_height * aspect;
    }
    if (fitted_width < 320.0 || fitted_height < 180.0) {
        return;
    }
    *width = (uint32_t) (fitted_width + 0.5);
    *height = (uint32_t) (fitted_height + 0.5);
}

/* Alt+Enter: borderless desktop fullscreen (a NULL mode), so there is no display mode
   switch and the scene follows the display's native resolution through the drawable size. */
static void btbh_gpu_window_toggle_fullscreen(BTBHGPUWindow *window) {
    const bool fullscreen =
        (SDL_GetWindowFlags(window->window) & SDL_WINDOW_FULLSCREEN) != 0;
    if (!fullscreen) {
        (void) SDL_SetWindowFullscreenMode(window->window, NULL);
    }
    if (SDL_SetWindowFullscreen(window->window, !fullscreen)) {
        (void) SDL_SyncWindow(window->window);
    }
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
        !btbh_gpu_valid_window_size(descriptor->width, descriptor->height) ||
        btbh_gpu_resolve_present_mode(descriptor->present_mode, false, false) ==
            BTBH_GPU_PRESENT_MODE_INVALID
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

    // SDL subsystems are reference counted. Hold our own gamepad reference so
    // shutdown cannot disable another window's or library's controller input.
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        btbh_gpu_set_window_error(report, BTBH_GPU_ERROR_STAGE_PLATFORM_INITIALIZATION, SDL_GetError());
        btbh_gpu_window_destroy(window);
        return NULL;
    }
    window->owns_gamepad_subsystem = true;
    btbh_gpu_discover_gamepad(window);

    SDL_WindowFlags flags = 0;
    if (descriptor->hidden != 0) {
        flags |= SDL_WINDOW_HIDDEN;
    }
    if (descriptor->high_pixel_density != 0) {
        flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
    }
    if (descriptor->resizable != 0) {
        flags |= SDL_WINDOW_RESIZABLE;
    }
    uint32_t width = descriptor->width;
    uint32_t height = descriptor->height;
    if (descriptor->display_percent != 0) {
        btbh_gpu_fit_window_to_display(
            descriptor->display_percent,
            &width,
            &height
        );
    }
    window->fullscreen_toggle = descriptor->fullscreen_toggle != 0;
    window->requested_present_mode = (BTBHGPUPresentMode) descriptor->present_mode;
    /* Centred on the primary display, the display the size was fitted to. SDL centres an
       undefined position itself on Windows and macOS but leaves it to the window manager
       on X11, which put the window in a corner; Wayland compositors place it themselves. */
    const SDL_PropertiesID window_properties = SDL_CreateProperties();
    if (window_properties == 0) {
        btbh_gpu_set_window_error(
            report,
            BTBH_GPU_ERROR_STAGE_WINDOW_CREATION,
            SDL_GetError()
        );
        btbh_gpu_window_destroy(window);
        return NULL;
    }
    (void) SDL_SetStringProperty(window_properties, SDL_PROP_WINDOW_CREATE_TITLE_STRING, descriptor->title);
    (void) SDL_SetNumberProperty(window_properties, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED);
    (void) SDL_SetNumberProperty(window_properties, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED);
    (void) SDL_SetNumberProperty(window_properties, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, (Sint64) width);
    (void) SDL_SetNumberProperty(window_properties, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, (Sint64) height);
    (void) SDL_SetNumberProperty(window_properties, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, (Sint64) flags);
    window->window = SDL_CreateWindowWithProperties(window_properties);
    SDL_DestroyProperties(window_properties);
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
    const BTBHGPUPresentMode project_present_mode =
        btbh_gpu_choose_present_mode(
            device,
            window,
            window->requested_present_mode
        );
    const SDL_GPUPresentMode present_mode =
        btbh_gpu_sdl_present_mode(project_present_mode);
    const uint32_t allowed_frames_in_flight =
        btbh_gpu_allowed_frames_in_flight();
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
        ) ||
        !SDL_SetGPUAllowedFramesInFlight(
            device->gpu,
            allowed_frames_in_flight
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

#if defined(__APPLE__)
    /* Cocoa clears its pending-operation marker before the asynchronous
       deminiaturize notification updates SDL_WINDOW_MINIMIZED. Keep the
       explicit lifecycle call synchronous at our API boundary without
       adding any work to the per-frame path. */
    const Uint64 deadline = SDL_GetTicksNS() + SDL_MS_TO_NS(2500);
    for (;;) {
        SDL_PumpEvents();
        if (!btbh_gpu_refresh_window_sizes(window, report)) {
            btbh_gpu_set_window_error(
                report,
                BTBH_GPU_ERROR_STAGE_WINDOW_RESIZE,
                SDL_GetError()
            );
            return report->result;
        }
        if ((report->minimized != 0) == minimized) {
            break;
        }
        if (SDL_GetTicksNS() >= deadline) {
            btbh_gpu_set_window_error(
                report,
                BTBH_GPU_ERROR_STAGE_WINDOW_RESIZE,
                "Window minimize/restore state did not converge"
            );
            return report->result;
        }
        SDL_Delay(10);
    }
#else
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
#endif
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

BTBHGPUResult btbh_gpu_window_set_present_mode(
    BTBHGPUWindow *window,
    BTBHGPUPresentMode present_mode,
    BTBHGPUWindowReport *report
) {
    if (
        window == NULL ||
        window->window == NULL ||
        report == NULL ||
        btbh_gpu_resolve_present_mode((int) present_mode, false, false) ==
            BTBH_GPU_PRESENT_MODE_INVALID ||
        (window->claimed &&
            (window->device == NULL || window->device->gpu == NULL))
    ) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }

    memset(report, 0, sizeof(*report));
    /* A pending frame holds an acquired swapchain image (or a scene waiting for one), and SDL
       rebuilds the swapchain when its present mode changes. */
    if (window->pending_command != NULL || window->pending_scene_ready) {
        (void) btbh_gpu_refresh_window_sizes(window, report);
        report->result = BTBH_GPU_INVALID_ARGUMENT;
        report->error_stage = BTBH_GPU_ERROR_STAGE_SWAPCHAIN_CONFIGURATION;
        (void) snprintf(
            report->message,
            sizeof(report->message),
            "%s",
            "A frame is pending: switch the present mode between frames"
        );
        return report->result;
    }

    if (window->claimed) {
        const BTBHGPUPresentMode taken = btbh_gpu_choose_present_mode(
            window->device,
            window,
            present_mode
        );
        if (
            taken != window->present_mode &&
            !SDL_SetGPUSwapchainParameters(
                window->device->gpu,
                window->window,
                SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                btbh_gpu_sdl_present_mode(taken)
            )
        ) {
            btbh_gpu_set_window_error(
                report,
                BTBH_GPU_ERROR_STAGE_SWAPCHAIN_CONFIGURATION,
                SDL_GetError()
            );
            /* SDL may have taken the new mode before rebuilding the swapchain failed: go back to
               the mode in use, so the report's present_mode stays true. */
            (void) SDL_SetGPUSwapchainParameters(
                window->device->gpu,
                window->window,
                SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                btbh_gpu_sdl_present_mode(window->present_mode)
            );
            btbh_gpu_write_window_state(window, report);
            return report->result;
        }
        window->present_mode = taken;
    }
    window->requested_present_mode = present_mode;

    if (!btbh_gpu_refresh_window_sizes(window, report)) {
        btbh_gpu_set_window_error(
            report,
            BTBH_GPU_ERROR_STAGE_SWAPCHAIN_CONFIGURATION,
            SDL_GetError()
        );
        btbh_gpu_write_window_state(window, report);
        return report->result;
    }
    report->result = BTBH_GPU_OK;
    return report->result;
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

    const bool had_scene = window->pending_scene_ready;
    if (window->pending_command == NULL) {
        window->pending_command =
            SDL_AcquireGPUCommandBuffer(window->device->gpu);
    }
    if (window->pending_command == NULL) {
        btbh_gpu_set_window_error(
            report,
            BTBH_GPU_ERROR_STAGE_SWAPCHAIN_CONFIGURATION,
            SDL_GetError()
        );
        return report->result;
    }

    if (!SDL_WaitAndAcquireGPUSwapchainTexture(
        window->pending_command,
        window->window,
        &window->pending_swapchain,
        &window->pending_drawable_width,
        &window->pending_drawable_height
    )) {
        (void)btbh_gpu_window_finish_pending_frame(window, false);
        btbh_gpu_set_window_error(
            report,
            BTBH_GPU_ERROR_STAGE_SWAPCHAIN_CONFIGURATION,
            SDL_GetError()
        );
        return report->result;
    }
    if (window->pending_swapchain == NULL) {
        // A minimized or hidden window has no swapchain image. Submit the frame's offscreen
        // work and present nothing; only a requested readback needs the image.
        if (window->presented_frame_readback_requested) {
            (void)btbh_gpu_window_finish_pending_frame(window, false);
            btbh_gpu_set_window_error(
                report,
                BTBH_GPU_ERROR_STAGE_SWAPCHAIN_CONFIGURATION,
                "SDL_GPU swapchain is unavailable"
            );
            return report->result;
        }
        if (!btbh_gpu_window_finish_pending_frame(window, true) ||
            !btbh_gpu_refresh_window_sizes(window, report)) {
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
    SDL_GPUTexture *present_target =
        btbh_gpu_window_present_target(window);
    if (present_target == NULL) {
        (void)btbh_gpu_window_finish_pending_frame(window, false);
        btbh_gpu_set_window_error(
            report,
            BTBH_GPU_ERROR_STAGE_SWAPCHAIN_CONFIGURATION,
            SDL_GetError()
        );
        return report->result;
    }

    if (had_scene &&
        !btbh_gpu_window_blit_pending_scene(
            window,
            window->pending_command,
            present_target,
            window->pending_drawable_width,
            window->pending_drawable_height
        )) {
        (void)btbh_gpu_window_finish_pending_frame(window, false);
        btbh_gpu_set_window_error(
            report,
            BTBH_GPU_ERROR_STAGE_SWAPCHAIN_CONFIGURATION,
            "Could not composite submitted GPU scene"
        );
        return report->result;
    }
    if (!had_scene) {
        SDL_GPUColorTargetInfo color_target = {0};
        color_target.texture = present_target;
        color_target.clear_color.r = (float) red / 255.0f;
        color_target.clear_color.g = (float) green / 255.0f;
        color_target.clear_color.b = (float) blue / 255.0f;
        color_target.clear_color.a = (float) alpha / 255.0f;
        color_target.load_op = SDL_GPU_LOADOP_CLEAR;
        color_target.store_op = SDL_GPU_STOREOP_STORE;

        SDL_GPURenderPass *render_pass = SDL_BeginGPURenderPass(
            window->pending_command,
            &color_target,
            1,
            NULL
        );
        if (render_pass == NULL) {
            (void)btbh_gpu_window_finish_pending_frame(window, false);
            btbh_gpu_set_window_error(
                report,
                BTBH_GPU_ERROR_STAGE_SWAPCHAIN_CONFIGURATION,
                SDL_GetError()
            );
            return report->result;
        }
        SDL_EndGPURenderPass(render_pass);
    }

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

void btbh_gpu_delay_precise(uint64_t nanoseconds) {
    const Uint64 spin_tail = 500000u;
    const Uint64 started = SDL_GetTicksNS();
    const Uint64 deadline = started + (Uint64) nanoseconds;
    if (nanoseconds > spin_tail) {
        SDL_DelayNS((Uint64) nanoseconds - spin_tail);
    }
    while (SDL_GetTicksNS() < deadline) {
        SDL_CPUPauseInstruction();
    }
}

BTBHGPUResult btbh_gpu_window_poll_events(
    BTBHGPUWindow *window,
    BTBHGPUPlatformSnapshot *snapshot
) {
    if (window == NULL || window->window == NULL || snapshot == NULL) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }

    window->gamepad_snapshot.buttons_pressed = 0;
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
        if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
            btbh_gpu_discover_gamepad(window);
        } else if (event.type == SDL_EVENT_GAMEPAD_REMOVED &&
                   event.gdevice.which == window->gamepad_snapshot.instance_id) {
            btbh_gpu_close_gamepad(window);
            btbh_gpu_discover_gamepad(window);
        } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
                   event.gbutton.which == window->gamepad_snapshot.instance_id) {
            for (uint32_t index = 0; index < 15; index += 1) {
                if (event.gbutton.button == btbh_gpu_gamepad_buttons[index]) {
                    window->gamepad_snapshot.buttons_pressed |= 1u << index;
                    break;
                }
            }
        } else if (event.type == SDL_EVENT_QUIT) {
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
            const bool fullscreen_toggle =
                window->fullscreen_toggle &&
                (event.key.mod & SDL_KMOD_ALT) != 0 &&
                (event.key.scancode == SDL_SCANCODE_RETURN ||
                 event.key.scancode == SDL_SCANCODE_KP_ENTER);
            if (fullscreen_toggle) {
                btbh_gpu_window_toggle_fullscreen(window);
            } else {
                window->key_pressed[event.key.scancode] = true;
            }
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

    btbh_gpu_sample_gamepad(window);
    btbh_gpu_platform_snapshot(window, snapshot, 0.0f);
    return BTBH_GPU_OK;
}

BTBHGPUResult btbh_gpu_window_gamepad_snapshot(
    const BTBHGPUWindow *window,
    BTBHGPUGamepadSnapshot *snapshot
) {
    if (window == NULL || snapshot == NULL) { return BTBH_GPU_INVALID_ARGUMENT; }
    *snapshot = window->gamepad_snapshot;
    return BTBH_GPU_OK;
}

BTBHGPUResult btbh_gpu_window_gamepad_rumble(
    BTBHGPUWindow *window,
    uint16_t low_frequency,
    uint16_t high_frequency,
    uint32_t duration_ms
) {
    if (window == NULL) { return BTBH_GPU_INVALID_ARGUMENT; }
    /* SDL reports a pad without motors as an error: that is no failure here. */
    if (window->gamepad != NULL) {
        (void)SDL_RumbleGamepad(window->gamepad, low_frequency, high_frequency, duration_ms);
    }
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

BTBHGPUResult btbh_gpu_window_show_file_dialog(
    BTBHGPUWindow *window,
    const BTBHGPUFileDialogDescriptor *descriptor,
    BTBHGPUFileDialogCallback callback,
    void *userdata,
    BTBHGPUFileDialogReport *report
) {
    if (report == NULL) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    btbh_gpu_set_file_dialog_report(report, BTBH_GPU_OK, "");
    if (
        window == NULL ||
        window->window == NULL ||
        descriptor == NULL ||
        callback == NULL ||
        descriptor->title == NULL ||
        (
            descriptor->kind != BTBH_GPU_FILE_DIALOG_OPEN &&
            descriptor->kind != BTBH_GPU_FILE_DIALOG_SAVE
        ) ||
        descriptor->filter_count > (uint32_t) INT_MAX ||
        (
            descriptor->filter_count > 0 &&
            (
                descriptor->filter_names == NULL ||
                descriptor->filter_patterns == NULL
            )
        ) ||
        (
            descriptor->kind == BTBH_GPU_FILE_DIALOG_SAVE &&
            descriptor->allows_multiple_selection != 0
        )
    ) {
        btbh_gpu_set_file_dialog_report(
            report,
            BTBH_GPU_INVALID_ARGUMENT,
            "Invalid GPU file-dialog descriptor"
        );
        return report->result;
    }

    BTBHGPUFileDialogContext *context = calloc(1, sizeof(*context));
    if (context == NULL) {
        btbh_gpu_set_file_dialog_report(
            report,
            BTBH_GPU_PLATFORM_FAILURE,
            "Could not allocate GPU file-dialog callback context"
        );
        return report->result;
    }
    context->callback = callback;
    context->userdata = userdata;
    context->filter_count = descriptor->filter_count;

    if (context->filter_count > 0) {
        context->filters = calloc(
            context->filter_count,
            sizeof(*context->filters)
        );
        context->filter_names = calloc(
            context->filter_count,
            sizeof(*context->filter_names)
        );
        context->filter_patterns = calloc(
            context->filter_count,
            sizeof(*context->filter_patterns)
        );
        if (
            context->filters == NULL ||
            context->filter_names == NULL ||
            context->filter_patterns == NULL
        ) {
            btbh_gpu_free_file_dialog_context(context);
            btbh_gpu_set_file_dialog_report(
                report,
                BTBH_GPU_PLATFORM_FAILURE,
                "Could not allocate GPU file-dialog filters"
            );
            return report->result;
        }
        for (
            uint32_t index = 0;
            index < context->filter_count;
            index += 1
        ) {
            const char *name = descriptor->filter_names[index];
            const char *pattern = descriptor->filter_patterns[index];
            if (name == NULL || pattern == NULL) {
                btbh_gpu_free_file_dialog_context(context);
                btbh_gpu_set_file_dialog_report(
                    report,
                    BTBH_GPU_INVALID_ARGUMENT,
                    "Invalid GPU file-dialog filter"
                );
                return report->result;
            }
            context->filter_names[index] = SDL_strdup(name);
            context->filter_patterns[index] = SDL_strdup(pattern);
            if (
                context->filter_names[index] == NULL ||
                context->filter_patterns[index] == NULL
            ) {
                btbh_gpu_free_file_dialog_context(context);
                btbh_gpu_set_file_dialog_report(
                    report,
                    BTBH_GPU_PLATFORM_FAILURE,
                    "Could not copy GPU file-dialog filters"
                );
                return report->result;
            }
            context->filters[index].name =
                context->filter_names[index];
            context->filters[index].pattern =
                context->filter_patterns[index];
        }
    }

    context->properties = SDL_CreateProperties();
    if (context->properties == 0) {
        btbh_gpu_free_file_dialog_context(context);
        btbh_gpu_set_file_dialog_report(
            report,
            BTBH_GPU_PLATFORM_FAILURE,
            "SDL file-dialog property creation failed"
        );
        return report->result;
    }
    if (
        !SDL_SetStringProperty(
            context->properties,
            SDL_PROP_FILE_DIALOG_TITLE_STRING,
            descriptor->title
        ) ||
        !SDL_SetNumberProperty(
            context->properties,
            SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER,
            (Sint64) context->filter_count
        ) ||
        !SDL_SetBooleanProperty(
            context->properties,
            SDL_PROP_FILE_DIALOG_MANY_BOOLEAN,
            descriptor->allows_multiple_selection != 0
        ) ||
        (
            context->filter_count > 0 &&
            !SDL_SetPointerProperty(
                context->properties,
                SDL_PROP_FILE_DIALOG_FILTERS_POINTER,
                context->filters
            )
        ) ||
        (
            descriptor->location != NULL &&
            !SDL_SetStringProperty(
                context->properties,
                SDL_PROP_FILE_DIALOG_LOCATION_STRING,
                descriptor->location
            )
        )
    ) {
        btbh_gpu_free_file_dialog_context(context);
        btbh_gpu_set_file_dialog_report(
            report,
            BTBH_GPU_PLATFORM_FAILURE,
            "SDL file-dialog property setup failed"
        );
        return report->result;
    }

    const SDL_FileDialogType type =
        descriptor->kind == BTBH_GPU_FILE_DIALOG_SAVE
            ? SDL_FILEDIALOG_SAVEFILE
            : SDL_FILEDIALOG_OPENFILE;
    SDL_ShowFileDialogWithProperties(
        type,
        btbh_gpu_file_dialog_callback,
        context,
        context->properties
    );
    return BTBH_GPU_OK;
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

struct SDL_Window *btbh_gpu_window_sdl_window(BTBHGPUWindow *window) {
    return window != NULL ? window->window : NULL;
}

void btbh_gpu_window_destroy(BTBHGPUWindow *window) {
    if (window == NULL) {
        return;
    }
    if (window->pending_command != NULL) {
        (void)btbh_gpu_window_finish_pending_frame(window, false);
    }
    btbh_gpu_window_release_frame_telemetry(window);
    btbh_gpu_window_release_canvas(window);
    btbh_gpu_window_release_aside_scene(window);
    btbh_gpu_window_release_scene(window);
    btbh_gpu_window_release_custom_meshes(window);
    btbh_gpu_window_release_instance_buffers(window);
    btbh_gpu_window_release_models(window);
    btbh_gpu_window_release_geometry_arenas(window);
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
    btbh_gpu_close_gamepad(window);
    if (window->owns_gamepad_subsystem) {
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        window->owns_gamepad_subsystem = false;
    }
    if (window->owns_video_subsystem) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        window->owns_video_subsystem = false;
    }
    free(window);
}
