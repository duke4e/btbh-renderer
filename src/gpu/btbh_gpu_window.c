#include "btbh_gpu_internal.h"
#include "btbh_gpu_hdr.h"
#include "btbh_gpu_sdf_terrain_internal.h"

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
_Static_assert(sizeof(BTBHGPUGamepadInfo) == 256, "Gamepad info ABI");
_Static_assert(sizeof(((BTBHGPUGamepadInfo *) 0)->guid) == sizeof(((SDL_GUID *) 0)->data), "Gamepad GUID");

/* The gamepad roster (since 0.15.0): window->gamepads[slot] is the pad open in that slot (NULL when it
   is empty) and window->gamepad_snapshots[slot] its state, whose instance_id is the pad's (0 when
   empty). Every pad SDL reports takes the lowest free slot and keeps it until it leaves. */

static void btbh_gpu_close_gamepad_slot(BTBHGPUWindow *window, uint32_t slot) {
    if (window->gamepads[slot] != NULL) {
        SDL_CloseGamepad(window->gamepads[slot]);
        window->gamepads[slot] = NULL;
    }
    memset(&window->gamepad_snapshots[slot], 0, sizeof(window->gamepad_snapshots[slot]));
}

static void btbh_gpu_close_gamepads(BTBHGPUWindow *window) {
    for (uint32_t slot = 0; slot < BTBH_GPU_GAMEPAD_CAPACITY; slot += 1) {
        btbh_gpu_close_gamepad_slot(window, slot);
    }
}

/* The slot holding pad identifier, or -1. */
static int btbh_gpu_gamepad_slot_of(const BTBHGPUWindow *window, SDL_JoystickID identifier) {
    if (identifier == 0) { return -1; }
    for (uint32_t slot = 0; slot < BTBH_GPU_GAMEPAD_CAPACITY; slot += 1) {
        if (window->gamepads[slot] != NULL && window->gamepad_snapshots[slot].instance_id == identifier) {
            return (int) slot;
        }
    }
    return -1;
}

static int btbh_gpu_gamepad_free_slot(const BTBHGPUWindow *window) {
    for (uint32_t slot = 0; slot < BTBH_GPU_GAMEPAD_CAPACITY; slot += 1) {
        if (window->gamepads[slot] == NULL) { return (int) slot; }
    }
    return -1;
}

/* The lowest occupied slot, the single-pad calls' pad, or -1. */
static int btbh_gpu_first_gamepad_slot(const BTBHGPUWindow *window) {
    for (uint32_t slot = 0; slot < BTBH_GPU_GAMEPAD_CAPACITY; slot += 1) {
        if (window->gamepads[slot] != NULL) { return (int) slot; }
    }
    return -1;
}

/* Opens pad identifier into the lowest free slot, unless the roster holds it already or is full (it
   waits then: btbh_gpu_discover_gamepads opens it when a slot frees). */
static void btbh_gpu_open_gamepad(BTBHGPUWindow *window, SDL_JoystickID identifier) {
    if (identifier == 0 || btbh_gpu_gamepad_slot_of(window, identifier) >= 0) { return; }
    const int slot = btbh_gpu_gamepad_free_slot(window);
    if (slot < 0) { return; }
    SDL_Gamepad *gamepad = SDL_OpenGamepad(identifier);
    if (gamepad == NULL) { return; }
    window->gamepads[slot] = gamepad;
    memset(&window->gamepad_snapshots[slot], 0, sizeof(window->gamepad_snapshots[slot]));
    window->gamepad_snapshots[slot].instance_id = identifier;
}

/* Opens every connected pad the roster lacks, in SDL's order, while a slot is free: the pads present
   when the window is made, and a waiting pad when a slot frees. */
static void btbh_gpu_discover_gamepads(BTBHGPUWindow *window) {
    int count = 0;
    SDL_JoystickID *identifiers = SDL_GetGamepads(&count);
    if (identifiers == NULL) { return; }
    for (int index = 0; index < count && btbh_gpu_gamepad_free_slot(window) >= 0; index += 1) {
        btbh_gpu_open_gamepad(window, identifiers[index]);
    }
    SDL_free(identifiers);
}

static float btbh_gpu_gamepad_axis(SDL_Gamepad *gamepad, SDL_GamepadAxis axis) {
    const Sint16 value = SDL_GetGamepadAxis(gamepad, axis);
    return value >= 0 ? (float) value / 32767.0f : (float) value / 32768.0f;
}

static void btbh_gpu_sample_gamepad(SDL_Gamepad *gamepad, BTBHGPUGamepadSnapshot *state) {
    uint32_t down = 0;
    for (uint32_t index = 0; index < 15; index += 1) {
        if (SDL_GetGamepadButton(gamepad, btbh_gpu_gamepad_buttons[index])) {
            down |= 1u << index;
        }
    }
    state->buttons_pressed |= down & ~state->buttons_down;
    state->buttons_down = down;
    state->left_x = btbh_gpu_gamepad_axis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
    state->left_y = btbh_gpu_gamepad_axis(gamepad, SDL_GAMEPAD_AXIS_LEFTY);
    state->right_x = btbh_gpu_gamepad_axis(gamepad, SDL_GAMEPAD_AXIS_RIGHTX);
    state->right_y = btbh_gpu_gamepad_axis(gamepad, SDL_GAMEPAD_AXIS_RIGHTY);
    state->left_trigger = SDL_max(0.0f, btbh_gpu_gamepad_axis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
    state->right_trigger = SDL_max(0.0f, btbh_gpu_gamepad_axis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
}

static void btbh_gpu_sample_gamepads(BTBHGPUWindow *window) {
    for (uint32_t slot = 0; slot < BTBH_GPU_GAMEPAD_CAPACITY; slot += 1) {
        if (window->gamepads[slot] != NULL) {
            btbh_gpu_sample_gamepad(window->gamepads[slot], &window->gamepad_snapshots[slot]);
        }
    }
}

static uint32_t btbh_gpu_gamepad_type(SDL_GamepadType type) {
    switch (type) {
    case SDL_GAMEPAD_TYPE_STANDARD: return BTBH_GPU_GAMEPAD_TYPE_STANDARD;
    case SDL_GAMEPAD_TYPE_XBOX360: return BTBH_GPU_GAMEPAD_TYPE_XBOX360;
    case SDL_GAMEPAD_TYPE_XBOXONE: return BTBH_GPU_GAMEPAD_TYPE_XBOXONE;
    case SDL_GAMEPAD_TYPE_PS3: return BTBH_GPU_GAMEPAD_TYPE_PS3;
    case SDL_GAMEPAD_TYPE_PS4: return BTBH_GPU_GAMEPAD_TYPE_PS4;
    case SDL_GAMEPAD_TYPE_PS5: return BTBH_GPU_GAMEPAD_TYPE_PS5;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: return BTBH_GPU_GAMEPAD_TYPE_SWITCH_PRO;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT: return BTBH_GPU_GAMEPAD_TYPE_JOYCON_LEFT;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: return BTBH_GPU_GAMEPAD_TYPE_JOYCON_RIGHT;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return BTBH_GPU_GAMEPAD_TYPE_JOYCON_PAIR;
    case SDL_GAMEPAD_TYPE_GAMECUBE: return BTBH_GPU_GAMEPAD_TYPE_GAMECUBE;
    default: return BTBH_GPU_GAMEPAD_TYPE_UNKNOWN;
    }
}

/* Copies a UTF-8 string into a zeroed char array of capacity bytes, cut at a character boundary. */
static void btbh_gpu_copy_utf8(char *destination, size_t capacity, const char *source) {
    if (source == NULL || capacity == 0) { return; }
    size_t length = strlen(source);
    if (length >= capacity) {
        length = capacity - 1;
        while (length > 0 && ((unsigned char) source[length] & 0xC0u) == 0x80u) {
            length -= 1;
        }
    }
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static void btbh_gpu_gamepad_info(SDL_Gamepad *gamepad, SDL_JoystickID identifier, BTBHGPUGamepadInfo *info) {
    info->instance_id = identifier;
    info->type = btbh_gpu_gamepad_type(SDL_GetGamepadType(gamepad));
    info->vendor_id = SDL_GetGamepadVendor(gamepad);
    info->product_id = SDL_GetGamepadProduct(gamepad);
    info->product_version = SDL_GetGamepadProductVersion(gamepad);
    info->firmware_version = SDL_GetGamepadFirmwareVersion(gamepad);
    const SDL_GUID guid = SDL_GetJoystickGUID(SDL_GetGamepadJoystick(gamepad));
    memcpy(info->guid, guid.data, sizeof(info->guid));
    const SDL_PropertiesID properties = SDL_GetGamepadProperties(gamepad);
    if (properties != 0 && SDL_GetBooleanProperty(properties, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false)) {
        info->flags |= BTBH_GPU_GAMEPAD_INFO_RUMBLE;
    }
    switch (SDL_GetGamepadConnectionState(gamepad)) {
    case SDL_JOYSTICK_CONNECTION_WIRED: info->connection = BTBH_GPU_GAMEPAD_CONNECTION_WIRED; break;
    case SDL_JOYSTICK_CONNECTION_WIRELESS: info->connection = BTBH_GPU_GAMEPAD_CONNECTION_WIRELESS; break;
    default: info->connection = BTBH_GPU_GAMEPAD_CONNECTION_UNKNOWN; break;
    }
    int percent = -1;
    switch (SDL_GetGamepadPowerInfo(gamepad, &percent)) {
    case SDL_POWERSTATE_ON_BATTERY: info->power = BTBH_GPU_GAMEPAD_POWER_ON_BATTERY; break;
    case SDL_POWERSTATE_NO_BATTERY: info->power = BTBH_GPU_GAMEPAD_POWER_NO_BATTERY; break;
    case SDL_POWERSTATE_CHARGING: info->power = BTBH_GPU_GAMEPAD_POWER_CHARGING; break;
    case SDL_POWERSTATE_CHARGED: info->power = BTBH_GPU_GAMEPAD_POWER_CHARGED; break;
    default: info->power = BTBH_GPU_GAMEPAD_POWER_UNKNOWN; break;
    }
    info->battery_percent = percent >= 0 && percent <= 100 ? (int8_t) percent : (int8_t) -1;
    btbh_gpu_copy_utf8(info->name, sizeof(info->name), SDL_GetGamepadName(gamepad));
    btbh_gpu_copy_utf8(info->serial, sizeof(info->serial), SDL_GetGamepadSerial(gamepad));
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
    btbh_gpu_discover_gamepads(window);

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

/* HDR output (RFC 0017, since 0.17.0). The window's HDR state: what SDL reports, which composition is in use, the
   switch, the debug switches and the log line. The GPU side (the frame texture, the encode, the canvas's RGBA16F
   pipelines) is in btbh_gpu_canvas.c, the scene's HDR post tail in btbh_gpu_scene.c. */

static const char *btbh_gpu_hdr_composition_name(BTBHGPUSwapchainComposition composition) {
    switch (composition) {
        case BTBH_GPU_SWAPCHAIN_SDR: return "SDR";
        case BTBH_GPU_SWAPCHAIN_HDR10_ST2084: return "HDR10_ST2084";
        case BTBH_GPU_SWAPCHAIN_HDR_EXTENDED_LINEAR: return "HDR_EXTENDED_LINEAR";
        default: return "INVALID";
    }
}

static const char *btbh_gpu_hdr_format_name(BTBHGPUTextureFormat format) {
    switch (format) {
        case BTBH_GPU_TEXTURE_FORMAT_RGBA8_UNORM: return "RGBA8_UNORM";
        case BTBH_GPU_TEXTURE_FORMAT_RGBA8_SRGB: return "RGBA8_SRGB";
        case BTBH_GPU_TEXTURE_FORMAT_BGRA8_UNORM: return "BGRA8_UNORM";
        case BTBH_GPU_TEXTURE_FORMAT_BGRA8_SRGB: return "BGRA8_SRGB";
        case BTBH_GPU_TEXTURE_FORMAT_RGB10A2_UNORM: return "RGB10A2_UNORM";
        case BTBH_GPU_TEXTURE_FORMAT_RGBA16_FLOAT: return "RGBA16_FLOAT";
        default: return "INVALID";
    }
}

static const char *btbh_gpu_hdr_mode_name(uint8_t mode) {
    switch (mode) {
        case BTBH_GPU_HDR_MODE_OFF: return "off";
        case BTBH_GPU_HDR_MODE_AUTO: return "auto";
        case BTBH_GPU_HDR_MODE_FORCE: return "force";
        default: return "?";
    }
}

static const char *btbh_gpu_hdr_fallback_name(uint8_t fallback) {
    switch (fallback) {
        case BTBH_GPU_HDR_FALLBACK_NONE: return "none";
        case BTBH_GPU_HDR_FALLBACK_UNSUPPORTED: return "unsupported";
        case BTBH_GPU_HDR_FALLBACK_OS_SDR: return "os_sdr";
        case BTBH_GPU_HDR_FALLBACK_SWITCH_FAILED: return "switch_failed";
        case BTBH_GPU_HDR_FALLBACK_LOST: return "lost";
        case BTBH_GPU_HDR_FALLBACK_PENDING: return "pending";
        default: return "?";
    }
}

/* SDL's colour space for a composition, by its own tables (SDL_gpu_vulkan.c:258-263, SDL_gpu_metal.m:4596-4602). */
static const char *btbh_gpu_hdr_colorspace_name(BTBHGPUSwapchainComposition composition) {
#if defined(__APPLE__)
    switch (composition) {
        case BTBH_GPU_SWAPCHAIN_HDR10_ST2084: return "ITUR_2100_PQ";
        case BTBH_GPU_SWAPCHAIN_HDR_EXTENDED_LINEAR: return "EXTENDED_LINEAR_SRGB";
        default: return "SRGB";
    }
#else
    switch (composition) {
        case BTBH_GPU_SWAPCHAIN_HDR10_ST2084: return "HDR10_ST2084";
        case BTBH_GPU_SWAPCHAIN_HDR_EXTENDED_LINEAR: return "EXTENDED_SRGB_LINEAR";
        default: return "SRGB_NONLINEAR";
    }
#endif
}

static SDL_GPUSwapchainComposition btbh_gpu_hdr_sdl_composition(BTBHGPUSwapchainComposition composition) {
    switch (composition) {
        case BTBH_GPU_SWAPCHAIN_HDR10_ST2084: return SDL_GPU_SWAPCHAINCOMPOSITION_HDR10_ST2084;
        case BTBH_GPU_SWAPCHAIN_HDR_EXTENDED_LINEAR: return SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR;
        default: return SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
    }
}

/* Wine/Proton: ntdll.dll exports wine_get_version there (found through SDL's own loader). */
static bool btbh_gpu_hdr_detect_wine(void) {
#if defined(_WIN32)
    BTBHGPUHDRSavedError saved;
    btbh_gpu_hdr_save_error(&saved);
    SDL_SharedObject *ntdll = SDL_LoadObject("ntdll.dll");
    if (ntdll == NULL) {
        btbh_gpu_hdr_restore_error(&saved);
        return false;
    }
    const bool wine = SDL_LoadFunction(ntdll, "wine_get_version") != NULL;
    SDL_UnloadObject(ntdll);
    btbh_gpu_hdr_restore_error(&saved);
    return wine;
#else
    return false;
#endif
}

static bool btbh_gpu_hdr_env_is(const char *name, const char *value) {
    const char *set = SDL_getenv(name);
    return set != NULL && SDL_strcasecmp(set, value) == 0;
}

/* The debug switches, once at the claim (RFC 0017, "Debug switches"). */
static void btbh_gpu_hdr_read_switches(BTBHGPUWindow *window) {
    window->hdr_log = btbh_gpu_hdr_env_is("BTBH_GPU_HDR_LOG", "1");
    window->hdr_dither = !btbh_gpu_hdr_env_is("BTBH_GPU_HDR_DITHER", "0");
    window->hdr_debug_heat = btbh_gpu_hdr_env_is("BTBH_GPU_HDR_DEBUG", "heat");
    window->hdr_simulate = 0u;
    if (btbh_gpu_hdr_env_is("BTBH_GPU_HDR_SIMULATE", "hdr10")) {
        window->hdr_simulate = (uint8_t)BTBH_GPU_SWAPCHAIN_HDR10_ST2084;
    } else if (btbh_gpu_hdr_env_is("BTBH_GPU_HDR_SIMULATE", "scrgb")) {
        window->hdr_simulate = (uint8_t)BTBH_GPU_SWAPCHAIN_HDR_EXTENDED_LINEAR;
    }
    window->hdr_override_mode = -1;
    window->hdr_override_composition = 0u;
    if (btbh_gpu_hdr_env_is("BTBH_GPU_HDR", "off")) {
        window->hdr_override_mode = (int8_t)BTBH_GPU_HDR_MODE_OFF;
    } else if (btbh_gpu_hdr_env_is("BTBH_GPU_HDR", "auto")) {
        window->hdr_override_mode = (int8_t)BTBH_GPU_HDR_MODE_AUTO;
    } else if (btbh_gpu_hdr_env_is("BTBH_GPU_HDR", "force")) {
        window->hdr_override_mode = (int8_t)BTBH_GPU_HDR_MODE_FORCE;
    } else if (btbh_gpu_hdr_env_is("BTBH_GPU_HDR", "hdr10")) {
        window->hdr_override_mode = (int8_t)BTBH_GPU_HDR_MODE_FORCE;
        window->hdr_override_composition = (uint8_t)BTBH_GPU_SWAPCHAIN_HDR10_ST2084;
    } else if (btbh_gpu_hdr_env_is("BTBH_GPU_HDR", "scrgb")) {
        window->hdr_override_mode = (int8_t)BTBH_GPU_HDR_MODE_FORCE;
        window->hdr_override_composition = (uint8_t)BTBH_GPU_SWAPCHAIN_HDR_EXTENDED_LINEAR;
    }
    window->hdr_override_paper_white = 0.0f;
    window->hdr_override_peak = 0.0f;
    const char *nits = SDL_getenv("BTBH_GPU_HDR_NITS");
    if (nits != NULL) {
        float paper = 0.0f;
        float peak = 0.0f;
        if (SDL_sscanf(nits, "%f,%f", &paper, &peak) == 2 && paper > 0.0f && peak > 0.0f) {
            window->hdr_override_paper_white = paper;
            window->hdr_override_peak = peak;
        }
    }
    window->hdr_knee_override = 0.0f;
    const char *knee = SDL_getenv("BTBH_GPU_HDR_KNEE");
    if (knee != NULL) {
        const float value = (float)SDL_atof(knee);
        window->hdr_knee_override = value > 0.0f && value < 1.0f ? value : 0.0f;
    }
    window->hdr_shoulder_scale = BTBH_HDR_SHOULDER_SCALE;
    const char *shoulder = SDL_getenv("BTBH_GPU_HDR_SHOULDER");
    if (shoulder != NULL) {
        const float value = (float)SDL_atof(shoulder);
        window->hdr_shoulder_scale = value > 0.0f ? value : BTBH_HDR_SHOULDER_SCALE;
    }
    window->under_wine = btbh_gpu_hdr_detect_wine();
}

static uint8_t btbh_gpu_hdr_effective_mode(const BTBHGPUWindow *window) {
    return window->hdr_override_mode >= 0 ? (uint8_t)window->hdr_override_mode : window->hdr_settings.mode;
}

static uint8_t btbh_gpu_hdr_effective_composition(const BTBHGPUWindow *window) {
    return window->hdr_override_mode >= 0 ? window->hdr_override_composition : window->hdr_settings.composition;
}

/* What SDL reports for the window now: the compositions, the properties, the display; with BTBH_GPU_HDR_SIMULATE a
   display that is HDR (203-nit SDR white, 1000-nit peak) offering the simulated composition alone. */
static void btbh_gpu_hdr_query(BTBHGPUWindow *window, BTBHGPUWindowHDRState *state) {
    /* (SDL's queries may set an error: the host's is kept.) */
    BTBHGPUHDRSavedError saved;
    btbh_gpu_hdr_save_error(&saved);
    SDL_PropertiesID properties = SDL_GetWindowProperties(window->window);
    state->os_hdr_enabled = SDL_GetBooleanProperty(properties, SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN, false) ? 1u : 0u;
    state->sdr_white_level = SDL_GetFloatProperty(properties, SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT, 1.0f);
    state->hdr_headroom = SDL_GetFloatProperty(properties, SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT, 1.0f);
    state->display_id = (uint32_t)SDL_GetDisplayForWindow(window->window);
    state->under_wine = window->under_wine ? 1u : 0u;
#if defined(__APPLE__)
    state->relative_luminance = 1u;
#else
    state->relative_luminance = 0u;
#endif
    for (uint32_t index = 0u; index < 4u; ++index) {
        state->supported[index] = SDL_WindowSupportsGPUSwapchainComposition(
            window->device->gpu,
            window->window,
            (SDL_GPUSwapchainComposition)index
        ) ? 1u : 0u;
    }
    btbh_gpu_hdr_restore_error(&saved);
    if (window->hdr_simulate != 0u) {
        /* (A test may make the simulated display report SDR: hdr_simulate_os_sdr.) */
        state->os_hdr_enabled = window->hdr_simulate_os_sdr ? 0u : 1u;
        state->sdr_white_level = BTBH_GPU_HDR_DEFAULT_PAPER_WHITE_NITS / BTBH_HDR_SCRGB_NITS;
        state->hdr_headroom = window->hdr_simulate_os_sdr
            ? 1.0f
            : BTBH_GPU_HDR_DEFAULT_PEAK_NITS / BTBH_GPU_HDR_DEFAULT_PAPER_WHITE_NITS;
        state->supported[SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR] =
            window->hdr_simulate == (uint8_t)BTBH_GPU_SWAPCHAIN_HDR_EXTENDED_LINEAR ? 1u : 0u;
        state->supported[SDL_GPU_SWAPCHAINCOMPOSITION_HDR10_ST2084] =
            window->hdr_simulate == (uint8_t)BTBH_GPU_SWAPCHAIN_HDR10_ST2084 ? 1u : 0u;
    }
    if (state->relative_luminance != 0u) {
        state->display_sdr_white_nits = 0.0f;
        state->display_peak_nits = 0.0f;
    } else {
        state->display_sdr_white_nits = state->sdr_white_level * BTBH_HDR_SCRGB_NITS;
        state->display_peak_nits = state->hdr_headroom > 1.0f
            ? state->hdr_headroom * state->display_sdr_white_nits
            : 0.0f;
    }
}

static bool btbh_gpu_hdr_composition_supported(
    const BTBHGPUWindowHDRState *state,
    BTBHGPUSwapchainComposition composition
) {
    return state->supported[btbh_gpu_hdr_sdl_composition(composition)] != 0u;
}

/* The HDR composition to use when HDR is wanted: the one named, else HDR10 then extended linear (extended linear
   first on Apple); SDR when none is offered. */
static BTBHGPUSwapchainComposition btbh_gpu_hdr_pick(
    const BTBHGPUWindowHDRState *state,
    uint8_t requested
) {
    if (requested != 0u) {
        return btbh_gpu_hdr_composition_supported(state, (BTBHGPUSwapchainComposition)requested)
            ? (BTBHGPUSwapchainComposition)requested
            : BTBH_GPU_SWAPCHAIN_SDR;
    }
#if defined(__APPLE__)
    const BTBHGPUSwapchainComposition order[2] = {
        BTBH_GPU_SWAPCHAIN_HDR_EXTENDED_LINEAR, BTBH_GPU_SWAPCHAIN_HDR10_ST2084
    };
#else
    const BTBHGPUSwapchainComposition order[2] = {
        BTBH_GPU_SWAPCHAIN_HDR10_ST2084, BTBH_GPU_SWAPCHAIN_HDR_EXTENDED_LINEAR
    };
#endif
    for (uint32_t index = 0u; index < 2u; ++index) {
        if (btbh_gpu_hdr_composition_supported(state, order[index])) {
            return order[index];
        }
    }
    return BTBH_GPU_SWAPCHAIN_SDR;
}

/* Whether HDR can be shown (BTBHGPUWindowHDRState.available) and, for the mode, the composition to be in. */
static BTBHGPUSwapchainComposition btbh_gpu_hdr_target(
    const BTBHGPUWindow *window,
    BTBHGPUWindowHDRState *state
) {
    const uint8_t requested = btbh_gpu_hdr_effective_composition(window);
    const BTBHGPUSwapchainComposition offered = btbh_gpu_hdr_pick(state, requested);
    bool platform = state->os_hdr_enabled != 0u;
    if (window->under_wine && window->hdr_simulate == 0u) {
        platform = true;
    }
#if defined(__APPLE__)
    platform = state->hdr_headroom > 1.0f;
#endif
    state->available = offered != BTBH_GPU_SWAPCHAIN_SDR && platform ? 1u : 0u;
    const uint8_t mode = btbh_gpu_hdr_effective_mode(window);
    if (mode == BTBH_GPU_HDR_MODE_FORCE) {
        return offered;
    }
    if (mode == BTBH_GPU_HDR_MODE_AUTO && state->available != 0u) {
        return offered;
    }
    return BTBH_GPU_SWAPCHAIN_SDR;
}

/* The nits in use: the settings' (or BTBH_GPU_HDR_NITS), their defaults and clamps; P the peak over paper white. */
static void btbh_gpu_hdr_resolve_nits(const BTBHGPUWindow *window, BTBHGPUWindowHDRState *state) {
    float paper = window->hdr_override_paper_white > 0.0f
        ? window->hdr_override_paper_white
        : window->hdr_settings.paper_white_nits;
    if (!(paper > 0.0f)) {
        paper = BTBH_GPU_HDR_DEFAULT_PAPER_WHITE_NITS;
    }
    paper = paper < 80.0f ? 80.0f : (paper > 1000.0f ? 1000.0f : paper);
    float peak = window->hdr_override_peak > 0.0f ? window->hdr_override_peak : window->hdr_settings.peak_nits;
    if (!(peak > 0.0f)) {
        peak = state->display_peak_nits > paper ? state->display_peak_nits : BTBH_GPU_HDR_DEFAULT_PEAK_NITS;
    }
    peak = peak < paper ? paper : (peak > 10000.0f ? 10000.0f : peak);
    state->paper_white_nits = paper;
    state->peak_nits = peak;
    float ratio = peak / paper;
    if (state->relative_luminance != 0u && state->hdr_headroom > 1.0f && ratio > state->hdr_headroom) {
        ratio = state->hdr_headroom;
    }
    state->highlight_ratio = window->hdr_active ? (ratio > 1.0f ? ratio : 1.0f) : 1.0f;
}

/* The whole state: SDL's answers (queried when `query`), the composition in use, the fallback. */
static void btbh_gpu_hdr_fill(BTBHGPUWindow *window, bool query) {
    BTBHGPUWindowHDRState *state = &window->hdr_state;
    if (query) {
        btbh_gpu_hdr_query(window, state);
    }
    const BTBHGPUSwapchainComposition target = btbh_gpu_hdr_target(window, state);
    state->mode = btbh_gpu_hdr_effective_mode(window);
    state->active = window->hdr_active ? 1u : 0u;
    state->composition = window->hdr_active ? window->hdr_composition : BTBH_GPU_SWAPCHAIN_SDR;
    state->swapchain_format = window->hdr_active && window->hdr_simulate != 0u
        ? (window->hdr_composition == BTBH_GPU_SWAPCHAIN_HDR10_ST2084
            ? BTBH_GPU_TEXTURE_FORMAT_RGB10A2_UNORM
            : BTBH_GPU_TEXTURE_FORMAT_RGBA16_FLOAT)
        : window->swapchain_format;
    state->target_format = btbh_gpu_texture_format(window->sdr_format);
    state->frame_format = window->hdr_active ? BTBH_GPU_TEXTURE_FORMAT_RGBA16_FLOAT : window->swapchain_format;
    state->metadata_set = 0u;
    btbh_gpu_hdr_resolve_nits(window, state);
    state->switch_count = window->hdr_switch_count;
    state->last_switch_milliseconds = window->hdr_last_switch_milliseconds;
    uint8_t fallback = BTBH_GPU_HDR_FALLBACK_NONE;
    if (state->mode != BTBH_GPU_HDR_MODE_OFF && !window->hdr_active) {
        if (window->hdr_sticky_fallback != BTBH_GPU_HDR_FALLBACK_NONE) {
            fallback = window->hdr_sticky_fallback;
        } else if (target != BTBH_GPU_SWAPCHAIN_SDR) {
            fallback = BTBH_GPU_HDR_FALLBACK_PENDING;
        } else if (btbh_gpu_hdr_pick(state, btbh_gpu_hdr_effective_composition(window)) == BTBH_GPU_SWAPCHAIN_SDR) {
            fallback = BTBH_GPU_HDR_FALLBACK_UNSUPPORTED;
        } else {
            fallback = BTBH_GPU_HDR_FALLBACK_OS_SDR;
        }
    } else if (state->mode == BTBH_GPU_HDR_MODE_AUTO && window->hdr_active && target == BTBH_GPU_SWAPCHAIN_SDR) {
        fallback = BTBH_GPU_HDR_FALLBACK_PENDING;
    }
    state->fallback = fallback;
    /* (Zeroed first: two fills that report the same compare equal byte for byte, btbh_gpu_hdr_refill.) */
    SDL_zeroa(state->message);
    (void)snprintf(state->message, sizeof(state->message), "%s", window->hdr_message);
    if (fallback == BTBH_GPU_HDR_FALLBACK_NONE) {
        state->message[0] = '\0';
    } else if (state->message[0] == '\0') {
        const char *why =
            fallback == BTBH_GPU_HDR_FALLBACK_UNSUPPORTED
                ? "The swapchain offers no HDR composition here"
            : fallback == BTBH_GPU_HDR_FALLBACK_OS_SDR
                ? "The display is SDR (the OS reports HDR off)"
            : fallback == BTBH_GPU_HDR_FALLBACK_PENDING
                ? "The change is taken at the next frame's start"
                : "";
        (void)snprintf(state->message, sizeof(state->message), "%s", why);
    }
}

uint32_t btbh_gpu_window_hdr_describe(
    BTBHGPUWindow *window,
    char *buffer,
    uint32_t capacity
) {
    if (window == NULL || buffer == NULL || capacity == 0u) {
        return 0u;
    }
    const BTBHGPUWindowHDRState *state = &window->hdr_state;
    char supported[96];
    supported[0] = '\0';
    static const char *const names[4] = { "SDR", "SDR_LINEAR", "HDR_EXTENDED_LINEAR", "HDR10_ST2084" };
    for (uint32_t index = 0u; index < 4u; ++index) {
        if (state->supported[index] != 0u) {
            if (supported[0] != '\0') {
                (void)SDL_strlcat(supported, ",", sizeof(supported));
            }
            (void)SDL_strlcat(supported, names[index], sizeof(supported));
        }
    }
    /* (SDL may set an error for a display that is gone: the host's is kept.) */
    BTBHGPUHDRSavedError saved;
    btbh_gpu_hdr_save_error(&saved);
    const char *display_name = window->window != NULL ? SDL_GetDisplayName((SDL_DisplayID)state->display_id) : NULL;
    const char *driver = window->device != NULL && window->device->gpu != NULL
        ? SDL_GetGPUDeviceDriver(window->device->gpu)
        : NULL;
    btbh_gpu_hdr_restore_error(&saved);
    char reason[160];
    reason[0] = '\0';
    if (state->fallback != BTBH_GPU_HDR_FALLBACK_NONE && state->message[0] != '\0') {
        (void)snprintf(reason, sizeof(reason), " reason=\"%s\"", state->message);
    }
    const int written = snprintf(
        buffer,
        capacity,
        "BTBH_GPU_HDR event=%s serial=%u gpu=%s wine=%u simulate=%s display=%u '%s' os_hdr=%u sdr_white=%.3f "
        "(%.0f nits) headroom=%.3f peak_reported=%.0f supported=%s available=%u mode=%s composition=%s swapchain=%s "
        "colorspace=%s frame=%s target=%s paper=%.0f peak=%.0f P=%.2f metadata=%u%s fallback=%s%s switch_ms=%.1f",
        window->hdr_event[0] != '\0' ? window->hdr_event : "state",
        (unsigned)state->serial,
        driver != NULL ? driver : "none",
        (unsigned)state->under_wine,
        window->hdr_simulate == (uint8_t)BTBH_GPU_SWAPCHAIN_HDR10_ST2084
            ? "hdr10"
            : (window->hdr_simulate != 0u ? "scrgb" : "off"),
        (unsigned)state->display_id,
        display_name != NULL ? display_name : "?",
        (unsigned)state->os_hdr_enabled,
        (double)state->sdr_white_level,
        (double)(state->sdr_white_level * BTBH_HDR_SCRGB_NITS),
        (double)state->hdr_headroom,
        (double)state->display_peak_nits,
        supported[0] != '\0' ? supported : "none",
        (unsigned)state->available,
        btbh_gpu_hdr_mode_name(state->mode),
        btbh_gpu_hdr_composition_name(state->composition),
        btbh_gpu_hdr_format_name(state->swapchain_format),
        btbh_gpu_hdr_colorspace_name(state->composition),
        btbh_gpu_hdr_format_name(state->frame_format),
        btbh_gpu_hdr_format_name(state->target_format),
        (double)state->paper_white_nits,
        (double)state->peak_nits,
        (double)state->highlight_ratio,
        (unsigned)state->metadata_set,
        state->metadata_set != 0u ? "" : " (SDL 3.4.2 has no call)",
        btbh_gpu_hdr_fallback_name(state->fallback),
        reason,
        state->last_switch_milliseconds
    );
    if (written < 0) {
        buffer[0] = '\0';
        return 0u;
    }
    return (uint32_t)written < capacity ? (uint32_t)written : capacity - 1u;
}

/* A new state (its serial; the snapshot's flag when `changed`) and, with BTBH_GPU_HDR_LOG=1, its line. A fall back to
   SDR the host didn't ask for (events lost and encode) is logged always. */
static void btbh_gpu_hdr_announce(BTBHGPUWindow *window, const char *event, bool changed) {
    (void)snprintf(window->hdr_event, sizeof(window->hdr_event), "%s", event);
    window->hdr_serial += 1u;
    window->hdr_state.serial = window->hdr_serial;
    if (changed) {
        window->hdr_changed = true;
    }
    const bool fell_back = SDL_strcmp(event, "lost") == 0 || SDL_strcmp(event, "encode") == 0;
    if (window->hdr_log || fell_back) {
        char line[BTBH_GPU_HDR_DESCRIBE_CAPACITY];
        (void)btbh_gpu_window_hdr_describe(window, line, sizeof(line));
        SDL_Log("%s", line);
    }
}

/* The fallback's words when the encode pass can't be made. */
static void btbh_gpu_hdr_encode_unavailable_message(BTBHGPUWindow *window) {
    (void)snprintf(
        window->hdr_message,
        sizeof(window->hdr_message),
        "The HDR encode is unavailable (%s)",
        window->hdr_encode_error[0] != '\0' ? window->hdr_encode_error : "its pipeline could not be made"
    );
}

/* Puts the window in `target` (SDR or an HDR composition): SDL's swapchain (none with BTBH_GPU_HDR_SIMULATE, whose
   swapchain stays SDR), the window's fields, the HDR frame's resources. False, the window in SDR and the fallback
   SWITCH_FAILED, when SDL refused or the encode pass can't be made. Inside a present's scope the lazy creation log
   names SDL's wait and what the switch makes. */
static bool btbh_gpu_hdr_switch(BTBHGPUWindow *window, BTBHGPUSwapchainComposition target) {
    const BTBHGPUSwapchainComposition current =
        window->hdr_active ? window->hdr_composition : BTBH_GPU_SWAPCHAIN_SDR;
    if (target == current) {
        return true;
    }
    const uint64_t started = SDL_GetTicksNS();
    bool switched = true;
    window->hdr_acquire_failures = 0u;
    if (window->hdr_simulate == 0u) {
        const uint64_t switch_started_ticks = btbh_gpu_lazy_creation_clock();
        switched = SDL_SetGPUSwapchainParameters(
            window->device->gpu,
            window->window,
            btbh_gpu_hdr_sdl_composition(target),
            btbh_gpu_sdl_present_mode(window->present_mode)
        );
        btbh_gpu_lazy_creation_call_timed(BTBH_GPU_LAZY_CALL_HDR_SWITCH, switch_started_ticks);
        if (!switched) {
            (void)snprintf(window->hdr_message, sizeof(window->hdr_message), "%s", SDL_GetError());
            /* SDL may have taken the composition before rebuilding the swapchain failed: back to SDR. */
            (void)SDL_SetGPUSwapchainParameters(
                window->device->gpu,
                window->window,
                SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                btbh_gpu_sdl_present_mode(window->present_mode)
            );
            target = BTBH_GPU_SWAPCHAIN_SDR;
        }
        const BTBHGPUTextureFormat format = btbh_gpu_texture_format(
            SDL_GetGPUSwapchainTextureFormat(window->device->gpu, window->window)
        );
        if (format != BTBH_GPU_TEXTURE_FORMAT_INVALID) {
            window->swapchain_format = format;
        }
        window->composition = target == BTBH_GPU_SWAPCHAIN_SDR ? BTBH_GPU_SWAPCHAIN_SDR : target;
    }
    window->hdr_active = target != BTBH_GPU_SWAPCHAIN_SDR;
    window->hdr_composition = window->hdr_active ? target : BTBH_GPU_SWAPCHAIN_SDR;
    if (switched) {
        window->hdr_sticky_fallback = BTBH_GPU_HDR_FALLBACK_NONE;
        window->hdr_message[0] = '\0';
    } else {
        window->hdr_sticky_fallback = BTBH_GPU_HDR_FALLBACK_SWITCH_FAILED;
    }
    if (window->hdr_active) {
        btbh_gpu_window_hdr_prewarm(window);
    } else {
        /* The HDR frame and the simulated swapchain are made again when HDR comes back. */
        if (window->presented_frame != NULL &&
            window->presented_frame->format == SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT) {
            btbh_gpu_texture_destroy(window->presented_frame);
            window->presented_frame = NULL;
        }
        if (window->hdr_virtual_swapchain != NULL) {
            btbh_gpu_texture_destroy(window->hdr_virtual_swapchain);
            window->hdr_virtual_swapchain = NULL;
        }
    }
    window->hdr_switch_count += 1u;
    window->hdr_last_switch_milliseconds = (double)(SDL_GetTicksNS() - started) / 1000000.0;
    if (window->hdr_active && window->hdr_encode_failed) {
        /* The prewarm could not make the encode from the shaders a present gave: never unencoded values into an HDR
           swapchain, so SDR. */
        (void)btbh_gpu_hdr_switch(window, BTBH_GPU_SWAPCHAIN_SDR);
        window->hdr_sticky_fallback = BTBH_GPU_HDR_FALLBACK_SWITCH_FAILED;
        btbh_gpu_hdr_encode_unavailable_message(window);
        return false;
    }
    return switched;
}

/* The frame's HDR numbers (BTBHGPUFrameContext.hdr_*), from the state in use; at a frame's start and at the claim. */
static void btbh_gpu_hdr_latch_frame(BTBHGPUWindow *window) {
    const BTBHGPUWindowHDRState *state = &window->hdr_state;
    window->hdr_frame_ratio = window->hdr_active && state->highlight_ratio > 1.0f ? state->highlight_ratio : 1.0f;
    window->hdr_frame_paper_white = state->paper_white_nits > 0.0f
        ? state->paper_white_nits
        : BTBH_GPU_HDR_DEFAULT_PAPER_WHITE_NITS;
    window->hdr_frame_linear_scale = state->relative_luminance != 0u
        ? 1.0f
        : window->hdr_frame_paper_white / BTBH_HDR_SCRGB_NITS;
}

/* Fills the state again (asking SDL when `query`); true when what it reports changed. The serial is kept: the caller
   announces a change. */
static bool btbh_gpu_hdr_refill(BTBHGPUWindow *window, bool query) {
    const BTBHGPUWindowHDRState before = window->hdr_state;
    btbh_gpu_hdr_fill(window, query);
    window->hdr_state.serial = before.serial;
    return memcmp(&before, &window->hdr_state, sizeof(before)) != 0;
}

/* At the claim: the SDR format kept, the switches read, the state filled; a mode forced by BTBH_GPU_HDR taken. */
static void btbh_gpu_hdr_claimed(BTBHGPUWindow *window) {
    window->sdr_format = SDL_GetGPUSwapchainTextureFormat(window->device->gpu, window->window);
    SDL_zero(window->hdr_settings);
    window->hdr_active = false;
    window->hdr_composition = BTBH_GPU_SWAPCHAIN_SDR;
    window->hdr_dirty = false;
    window->hdr_sticky_fallback = BTBH_GPU_HDR_FALLBACK_NONE;
    window->hdr_message[0] = '\0';
    window->hdr_pending_event[0] = '\0';
    window->hdr_encode_failed = false;
    window->hdr_encode_error[0] = '\0';
    window->hdr_acquire_failures = 0u;
    window->hdr_last_check_milliseconds = SDL_GetTicks();
    btbh_gpu_hdr_read_switches(window);
    btbh_gpu_hdr_fill(window, true);
    if (btbh_gpu_hdr_effective_mode(window) != BTBH_GPU_HDR_MODE_OFF) {
        (void)btbh_gpu_hdr_switch(window, btbh_gpu_hdr_target(window, &window->hdr_state));
        btbh_gpu_hdr_fill(window, true);
    }
    btbh_gpu_hdr_latch_frame(window);
    btbh_gpu_hdr_announce(window, "claim", false);
}

BTBHGPUResult btbh_gpu_window_hdr_state(
    BTBHGPUWindow *window,
    BTBHGPUWindowHDRState *state
) {
    if (window == NULL || state == NULL || window->window == NULL || !window->claimed ||
        window->device == NULL || window->device->gpu == NULL) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    if (btbh_gpu_hdr_refill(window, true)) {
        /* What it reports changed with no event the renderer saw (a list SDL changed silently, say): a new serial, and
           the line. */
        btbh_gpu_hdr_announce(window, "state", false);
    }
    *state = window->hdr_state;
    return BTBH_GPU_OK;
}

static bool btbh_gpu_hdr_finite_nits(float value) {
    return value >= 0.0f && value <= 1.0e30f;
}

BTBHGPUResult btbh_gpu_window_set_hdr(
    BTBHGPUWindow *window,
    const BTBHGPUWindowHDRSettings *settings,
    BTBHGPUWindowHDRState *state
) {
    if (window == NULL || settings == NULL || state == NULL) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    if (window->window == NULL || !window->claimed || window->device == NULL || window->device->gpu == NULL) {
        SDL_zerop(state);
        (void)snprintf(state->message, sizeof(state->message), "%s", "The window is not claimed");
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    bool reserved_clear = settings->flags == 0u;
    for (uint32_t index = 0u; index < 5u; ++index) {
        reserved_clear = reserved_clear && settings->reserved[index] == 0u;
    }
    const char *refusal = NULL;
    if (settings->mode > BTBH_GPU_HDR_MODE_FORCE) {
        refusal = "Unknown HDR mode";
    } else if (settings->composition != 0u &&
               settings->composition != (uint8_t)BTBH_GPU_SWAPCHAIN_HDR10_ST2084 &&
               settings->composition != (uint8_t)BTBH_GPU_SWAPCHAIN_HDR_EXTENDED_LINEAR) {
        refusal = "The composition must be 0, HDR10_ST2084 or HDR_EXTENDED_LINEAR";
    } else if (!reserved_clear) {
        refusal = "flags and reserved words must be 0";
    } else if (!btbh_gpu_hdr_finite_nits(settings->paper_white_nits) ||
               !btbh_gpu_hdr_finite_nits(settings->peak_nits)) {
        refusal = "Nits must be finite and not below 0";
    } else if ((settings->mode != window->hdr_settings.mode ||
                settings->composition != window->hdr_settings.composition) &&
               (window->pending_command != NULL || window->pending_scene_ready)) {
        refusal = "A frame is pending: switch HDR between frames";
    }
    if (refusal != NULL) {
        *state = window->hdr_state;
        (void)snprintf(state->message, sizeof(state->message), "%s", refusal);
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    const bool switch_asked =
        settings->mode != window->hdr_settings.mode || settings->composition != window->hdr_settings.composition;
    const bool nits_changed =
        settings->paper_white_nits != window->hdr_settings.paper_white_nits ||
        settings->peak_nits != window->hdr_settings.peak_nits;
    window->hdr_settings = *settings;
    bool encode_fell_back = false;
    if (switch_asked) {
        /* A new mode: an old fallback (lost, a failed switch or encode) no longer explains anything; a failed encode is
           tried once more. */
        window->hdr_sticky_fallback = BTBH_GPU_HDR_FALLBACK_NONE;
        window->hdr_message[0] = '\0';
        window->hdr_encode_failed = false;
        window->hdr_encode_error[0] = '\0';
        btbh_gpu_hdr_fill(window, true);
        (void)btbh_gpu_hdr_switch(window, btbh_gpu_hdr_target(window, &window->hdr_state));
        encode_fell_back = window->hdr_encode_failed && !window->hdr_active;
    }
    btbh_gpu_hdr_fill(window, switch_asked);
    if (switch_asked || nits_changed) {
        /* hdr_changed for a mode or composition change; a change of nits takes a new serial only (the host made it,
           and it takes effect from the next frame). */
        btbh_gpu_hdr_announce(window, encode_fell_back ? "encode" : "set", switch_asked);
    }
    *state = window->hdr_state;
    return BTBH_GPU_OK;
}

/* The re-check at a frame's start (btbh_gpu_window_hdr_frame_start): after an SDL event, or (AUTO) every frame while a
   change is pending and once a second otherwise, SDL is asked again and an AUTO window switches when it should. A
   silent re-check doesn't undo a fallback (lost, a failed switch or encode); an event retries. */
static void btbh_gpu_hdr_recheck(BTBHGPUWindow *window) {
    const uint8_t mode = btbh_gpu_hdr_effective_mode(window);
    const bool event = window->hdr_dirty;
    if (!event) {
        if (mode != BTBH_GPU_HDR_MODE_AUTO) {
            return;
        }
        const uint64_t now = SDL_GetTicks();
        if (window->hdr_state.fallback != BTBH_GPU_HDR_FALLBACK_PENDING &&
            now - window->hdr_last_check_milliseconds < 1000u) {
            return;
        }
    }
    window->hdr_dirty = false;
    window->hdr_last_check_milliseconds = SDL_GetTicks();
    char pending_event[16];
    (void)snprintf(
        pending_event,
        sizeof(pending_event),
        "%s",
        window->hdr_pending_event[0] != '\0' ? window->hdr_pending_event : "os"
    );
    window->hdr_pending_event[0] = '\0';
    const bool changed = btbh_gpu_hdr_refill(window, true);
    const BTBHGPUSwapchainComposition current =
        window->hdr_active ? window->hdr_composition : BTBH_GPU_SWAPCHAIN_SDR;
    const BTBHGPUSwapchainComposition target = btbh_gpu_hdr_target(window, &window->hdr_state);
    if (mode == BTBH_GPU_HDR_MODE_AUTO && target != current &&
        (event || window->hdr_sticky_fallback == BTBH_GPU_HDR_FALLBACK_NONE)) {
        if (event) {
            window->hdr_sticky_fallback = BTBH_GPU_HDR_FALLBACK_NONE;
            window->hdr_message[0] = '\0';
        }
        const bool switched = btbh_gpu_hdr_switch(window, target);
        btbh_gpu_hdr_fill(window, false);
        btbh_gpu_hdr_announce(
            window,
            !switched && window->hdr_encode_failed ? "encode" : "switch",
            true
        );
        return;
    }
    if (event) {
        /* (The event itself was in the poll's snapshot already.) */
        btbh_gpu_hdr_announce(window, pending_event, false);
    } else if (changed) {
        /* What SDL reports changed with no event (Wine's lists): for the host's next poll too. */
        btbh_gpu_hdr_announce(window, "recheck", true);
    }
}

void btbh_gpu_window_hdr_frame_start(BTBHGPUWindow *window) {
    if (window == NULL || !window->claimed || window->device == NULL || window->device->gpu == NULL ||
        window->pending_command != NULL || window->pending_scene_ready) {
        return;
    }
    if (window->hdr_encode_failed && window->hdr_active) {
        /* The last frame could not encode (btbh_gpu_window_hdr_record_encode): SDR from this frame, until a set_hdr
           switch tries again. */
        (void)btbh_gpu_hdr_switch(window, BTBH_GPU_SWAPCHAIN_SDR);
        window->hdr_sticky_fallback = BTBH_GPU_HDR_FALLBACK_SWITCH_FAILED;
        btbh_gpu_hdr_encode_unavailable_message(window);
        btbh_gpu_hdr_fill(window, true);
        btbh_gpu_hdr_announce(window, "encode", true);
    } else {
        btbh_gpu_hdr_recheck(window);
    }
    /* The frame's HDR numbers, for its gain, its encode and its fold alike. */
    btbh_gpu_hdr_latch_frame(window);
}

bool btbh_gpu_window_hdr_acquire_failed(BTBHGPUWindow *window) {
    if (window == NULL || !window->hdr_active || window->hdr_simulate != 0u || !window->claimed ||
        window->device == NULL || window->device->gpu == NULL) {
        return false;
    }
    BTBHGPUHDRSavedError saved;
    btbh_gpu_hdr_save_error(&saved);
    window->hdr_acquire_failures += 1u;
    const bool listed = SDL_WindowSupportsGPUSwapchainComposition(
        window->device->gpu,
        window->window,
        btbh_gpu_hdr_sdl_composition(window->hdr_composition)
    );
    if (listed && window->hdr_acquire_failures < BTBH_GPU_HDR_ACQUIRE_FAILURE_LIMIT) {
        btbh_gpu_hdr_restore_error(&saved);
        return false;
    }
    const uint32_t failures = window->hdr_acquire_failures;
    (void)btbh_gpu_hdr_switch(window, BTBH_GPU_SWAPCHAIN_SDR);
    window->hdr_sticky_fallback = BTBH_GPU_HDR_FALLBACK_LOST;
    if (listed) {
        /* gamescope may keep listing a composition it can't present: don't let every present fail. */
        (void)snprintf(
            window->hdr_message,
            sizeof(window->hdr_message),
            "%u swapchain acquires failed in a row in HDR (%s)",
            (unsigned)failures,
            saved.text[0] != '\0' ? saved.text : "no error"
        );
    } else {
        (void)snprintf(
            window->hdr_message,
            sizeof(window->hdr_message),
            "%s",
            "The swapchain lost its HDR composition (the display or the OS changed)"
        );
    }
    btbh_gpu_hdr_fill(window, true);
    btbh_gpu_hdr_announce(window, "lost", true);
    btbh_gpu_hdr_restore_error(&saved);
    return true;
}

void btbh_gpu_window_hdr_remember_shaders(BTBHGPUWindow *window, const char *path, BTBHGPUShaderFormat format) {
    if (window == NULL || path == NULL) {
        return;
    }
    const size_t length = strlen(path);
    if (length == 0u || length >= sizeof(window->shader_reference_path)) {
        return;
    }
    if (window->shader_reference_format == format && strcmp(window->shader_reference_path, path) == 0) {
        return;
    }
    (void)memcpy(window->shader_reference_path, path, length + 1u);
    window->shader_reference_format = format;
}

void btbh_gpu_window_release_hdr(BTBHGPUWindow *window) {
    if (window == NULL || window->device == NULL || window->device->gpu == NULL) {
        return;
    }
    for (uint32_t index = 0u; index < 2u; ++index) {
        if (window->hdr_encode_pipelines[index] != NULL) {
            SDL_ReleaseGPUGraphicsPipeline(window->device->gpu, window->hdr_encode_pipelines[index]);
            window->hdr_encode_pipelines[index] = NULL;
        }
    }
    if (window->hdr_fold_pipeline != NULL) {
        SDL_ReleaseGPUGraphicsPipeline(window->device->gpu, window->hdr_fold_pipeline);
        window->hdr_fold_pipeline = NULL;
    }
    /* The renderer's own textures (the window's texture list would only orphan them): the simulated swapchain, and the
       frame texture (HDR's, or a read-back frame's). */
    if (window->hdr_virtual_swapchain != NULL) {
        btbh_gpu_texture_destroy(window->hdr_virtual_swapchain);
        window->hdr_virtual_swapchain = NULL;
    }
    if (window->presented_frame != NULL) {
        btbh_gpu_texture_destroy(window->presented_frame);
        window->presented_frame = NULL;
    }
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
    /* RFC 0017: the SDR format kept, HDR's debug switches read, its state filled (a mode BTBH_GPU_HDR forces taken). */
    btbh_gpu_hdr_claimed(window);
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
                btbh_gpu_hdr_sdl_composition(window->composition),
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
                btbh_gpu_hdr_sdl_composition(window->composition),
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

    /* RFC 0017: a frame's start, where an AUTO window takes an HDR change. */
    btbh_gpu_window_hdr_frame_start(window);
    /* What the frame reads from the window (RFC 0015 phase 1a): here, a read-back frame's format. */
    BTBHGPUFrameContext frame_context;
    btbh_gpu_window_frame_context_fill(window, &frame_context);
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
        /* RFC 0017: the swapchain may have lost its HDR composition; the window is SDR from the next frame. */
        (void)btbh_gpu_window_hdr_acquire_failed(window);
        return report->result;
    }
    window->hdr_acquire_failures = 0u;
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
        btbh_gpu_window_present_target(window, &frame_context);
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

    /* A read-back frame, or (RFC 0017) the HDR frame, goes to the swapchain image. */
    const bool recorded =
        (!window->presented_frame_readback_requested && !window->hdr_active) ||
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
    snapshot->hdr_changed = window->hdr_changed ? 1u : 0u;
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

    for (uint32_t slot = 0; slot < BTBH_GPU_GAMEPAD_CAPACITY; slot += 1) {
        window->gamepad_snapshots[slot].buttons_pressed = 0;
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
        if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
            btbh_gpu_open_gamepad(window, event.gdevice.which);
        } else if (event.type == SDL_EVENT_GAMEPAD_REMOVED) {
            const int slot = btbh_gpu_gamepad_slot_of(window, event.gdevice.which);
            if (slot >= 0) {
                btbh_gpu_close_gamepad_slot(window, (uint32_t) slot);
                btbh_gpu_discover_gamepads(window);
            }
        } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
            const int slot = btbh_gpu_gamepad_slot_of(window, event.gbutton.which);
            for (uint32_t index = 0; slot >= 0 && index < 15; index += 1) {
                if (event.gbutton.button == btbh_gpu_gamepad_buttons[index]) {
                    window->gamepad_snapshots[slot].buttons_pressed |= 1u << index;
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
        } else if (
            (event.type == SDL_EVENT_WINDOW_HDR_STATE_CHANGED ||
             event.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED) &&
            event.window.windowID == window_id &&
            window->claimed
        ) {
            /* RFC 0017: re-checked at the next frame's start (an AUTO window switches there); no GPU work here. */
            window->hdr_dirty = true;
            window->hdr_changed = true;
            (void)snprintf(
                window->hdr_pending_event,
                sizeof(window->hdr_pending_event),
                "%s",
                event.type == SDL_EVENT_WINDOW_HDR_STATE_CHANGED ? "os" : "display"
            );
        }
    }

    btbh_gpu_sample_gamepads(window);
    btbh_gpu_platform_snapshot(window, snapshot, 0.0f);
    window->hdr_changed = false;
    return BTBH_GPU_OK;
}

BTBHGPUResult btbh_gpu_window_gamepad_snapshot(
    const BTBHGPUWindow *window,
    BTBHGPUGamepadSnapshot *snapshot
) {
    if (window == NULL || snapshot == NULL) { return BTBH_GPU_INVALID_ARGUMENT; }
    const int slot = btbh_gpu_first_gamepad_slot(window);
    if (slot < 0) {
        memset(snapshot, 0, sizeof(*snapshot));
    } else {
        *snapshot = window->gamepad_snapshots[slot];
    }
    return BTBH_GPU_OK;
}

BTBHGPUResult btbh_gpu_window_gamepad_rumble(
    BTBHGPUWindow *window,
    uint16_t low_frequency,
    uint16_t high_frequency,
    uint32_t duration_ms
) {
    if (window == NULL) { return BTBH_GPU_INVALID_ARGUMENT; }
    const int slot = btbh_gpu_first_gamepad_slot(window);
    /* SDL reports a pad without motors as an error: that is no failure here. */
    if (slot >= 0) {
        (void)SDL_RumbleGamepad(window->gamepads[slot], low_frequency, high_frequency, duration_ms);
    }
    return BTBH_GPU_OK;
}

uint32_t btbh_gpu_window_gamepad_count(const BTBHGPUWindow *window) {
    if (window == NULL) { return 0; }
    for (uint32_t count = BTBH_GPU_GAMEPAD_CAPACITY; count > 0; count -= 1) {
        if (window->gamepads[count - 1] != NULL) { return count; }
    }
    return 0;
}

BTBHGPUResult btbh_gpu_window_gamepad_snapshot_at(
    const BTBHGPUWindow *window,
    uint32_t index,
    BTBHGPUGamepadSnapshot *out
) {
    if (out != NULL) { memset(out, 0, sizeof(*out)); }
    if (window == NULL || out == NULL || index >= BTBH_GPU_GAMEPAD_CAPACITY) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    if (window->gamepads[index] != NULL) { *out = window->gamepad_snapshots[index]; }
    return BTBH_GPU_OK;
}

BTBHGPUResult btbh_gpu_window_gamepad_info_at(
    const BTBHGPUWindow *window,
    uint32_t index,
    BTBHGPUGamepadInfo *out
) {
    if (out != NULL) { memset(out, 0, sizeof(*out)); }
    if (window == NULL || out == NULL || index >= BTBH_GPU_GAMEPAD_CAPACITY) {
        return BTBH_GPU_INVALID_ARGUMENT;
    }
    if (window->gamepads[index] != NULL) {
        btbh_gpu_gamepad_info(window->gamepads[index], window->gamepad_snapshots[index].instance_id, out);
    }
    return BTBH_GPU_OK;
}

BTBHGPUResult btbh_gpu_window_gamepad_rumble_at(
    BTBHGPUWindow *window,
    uint32_t index,
    uint16_t low_frequency,
    uint16_t high_frequency,
    uint32_t duration_ms
) {
    if (window == NULL || index >= BTBH_GPU_GAMEPAD_CAPACITY) { return BTBH_GPU_INVALID_ARGUMENT; }
    /* As btbh_gpu_window_gamepad_rumble: a pad without motors is no failure, an empty slot does nothing. */
    if (window->gamepads[index] != NULL) {
        (void)SDL_RumbleGamepad(window->gamepads[index], low_frequency, high_frequency, duration_ms);
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
    if (window->scene_run_verify.checked_draw_count > 0u || window->scene_run_verify.mismatch_count > 0u) {
        /* BTBH_GPU_SCENE_RUNS=verify (since 0.13.0): the window's totals. */
        SDL_Log(
            "BTBH_GPU_SCENE_RUNS=verify: the window checked %llu run draws and found %llu mismatches",
            (unsigned long long)window->scene_run_verify.checked_draw_count,
            (unsigned long long)window->scene_run_verify.mismatch_count
        );
    }
    btbh_gpu_window_release_hdr(window);
    btbh_gpu_window_release_canvas(window);
    btbh_gpu_window_release_aside_scene(window);
    btbh_gpu_window_release_scene(window);
    btbh_gpu_window_detach_sdf_terrains(window);
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
    btbh_gpu_close_gamepads(window);
    if (window->owns_gamepad_subsystem) {
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        window->owns_gamepad_subsystem = false;
    }
    if (window->owns_video_subsystem) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        window->owns_video_subsystem = false;
    }
    btbh_gpu_lazy_creation_release(window);
    btbh_gpu_bone_palette_cache_destroy(window->bone_palette_cache);
    free(window);
}
