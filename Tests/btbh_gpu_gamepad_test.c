#include "btbh_gpu/btbh_gpu.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "gamepad test line %d: %s (%s)\n", __LINE__, #condition, SDL_GetError()); \
    exit(1); \
} } while (0)

static SDL_JoystickID attach(void) {
    SDL_VirtualJoystickDesc descriptor;
    SDL_INIT_INTERFACE(&descriptor);
    descriptor.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    descriptor.vendor_id = 0x1209;
    descriptor.product_id = 0xb7b0;
    descriptor.naxes = SDL_GAMEPAD_AXIS_COUNT;
    descriptor.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    descriptor.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1u;
    descriptor.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1u;
    descriptor.name = "BTBH virtual gamepad regression";
    const SDL_JoystickID identifier = SDL_AttachVirtualJoystick(&descriptor);
    CHECK(identifier != 0);
    CHECK(SDL_IsGamepad(identifier));
    return identifier;
}

static BTBHGPUGamepadSnapshot poll(BTBHGPUWindow *window) {
    BTBHGPUPlatformSnapshot platform = {0};
    BTBHGPUGamepadSnapshot gamepad = {0};
    CHECK(btbh_gpu_window_poll_events(window, &platform) == BTBH_GPU_OK);
    CHECK(btbh_gpu_window_gamepad_snapshot(window, &gamepad) == BTBH_GPU_OK);
    return gamepad;
}

int main(void) {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, "0x1209/0xb7b0");
    CHECK(SDL_InitSubSystem(SDL_INIT_GAMEPAD));
    const SDL_JoystickID first = attach();
    SDL_Joystick *joystick = SDL_OpenJoystick(first);
    CHECK(joystick != NULL);
    BTBHGPUWindowDescriptor descriptor = {0};
    descriptor.title = "Gamepad test";
    descriptor.width = 64;
    descriptor.height = 64;
    descriptor.hidden = 1;
    BTBHGPUWindowReport report = {0};
    BTBHGPUWindow *window = btbh_gpu_window_create(&descriptor, &report);
    CHECK(window != NULL);
    CHECK(poll(window).instance_id == first);

    const SDL_GamepadButton buttons[] = {
        SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
        SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
        SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
        SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK,
        SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_GUIDE,
        SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
        SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT
    };
    for (uint32_t index = 0; index < 15; index += 1) {
        CHECK(SDL_SetJoystickVirtualButton(joystick, buttons[index], true));
        const BTBHGPUGamepadSnapshot pressed = poll(window);
        CHECK(pressed.buttons_down == (1u << index));
        CHECK(pressed.buttons_pressed == (1u << index));
        CHECK(poll(window).buttons_pressed == 0);
        CHECK(SDL_SetJoystickVirtualButton(joystick, buttons[index], false));
        CHECK(poll(window).buttons_down == 0);
    }
    CHECK(SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFTX, -32768));
    CHECK(SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHTY, 32767));
    CHECK(SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767));
    const BTBHGPUGamepadSnapshot axes = poll(window);
    CHECK(axes.left_x == -1.0f);
    CHECK(axes.right_y == 1.0f);
    CHECK(axes.left_trigger == 1.0f);

    // Preserve a complete tap occurring between frame polls.
    SDL_Event event = {0};
    event.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    event.gbutton.which = first;
    event.gbutton.button = SDL_GAMEPAD_BUTTON_START;
    CHECK(SDL_PushEvent(&event));
    event.type = SDL_EVENT_GAMEPAD_BUTTON_UP;
    CHECK(SDL_PushEvent(&event));
    const BTBHGPUGamepadSnapshot tap = poll(window);
    CHECK(tap.buttons_down == 0);
    CHECK(tap.buttons_pressed == (1u << 8));

    const SDL_JoystickID second = attach();
    CHECK(poll(window).instance_id == first);
    SDL_CloseJoystick(joystick);
    CHECK(SDL_DetachVirtualJoystick(first));
    CHECK(poll(window).instance_id == second);
    CHECK(SDL_DetachVirtualJoystick(second));
    const BTBHGPUGamepadSnapshot disconnected = poll(window);
    CHECK(disconnected.instance_id == 0);
    CHECK(disconnected.buttons_down == 0);
    CHECK(disconnected.buttons_pressed == 0);
    CHECK(disconnected.left_x == 0);
    const SDL_JoystickID reconnected = attach();
    CHECK(poll(window).instance_id == reconnected);
    btbh_gpu_window_destroy(window);
    // The window must release only its own subsystem reference.
    CHECK(SDL_WasInit(SDL_INIT_GAMEPAD) != 0);
    CHECK(SDL_DetachVirtualJoystick(reconnected));
    SDL_Quit();
    puts("BTBH_GPU_GAMEPAD_TEST_OK buttons=15 axes=normalized hotplug=ok tap=ok ownership=ok");
    return 0;
}
