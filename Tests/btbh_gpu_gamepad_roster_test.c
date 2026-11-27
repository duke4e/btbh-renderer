/* The window's gamepad roster (since 0.15.0), without a GPU or a display: SDL's virtual gamepads on the dummy
   video driver, as btbh_gpu_gamepad_test.c. Real pads are kept out by SDL's ignore hint. It checks the count, the
   slots and each pad's identity; a disconnect that leaves a hole and a connect that fills the lowest free slot; the
   single-pad calls following the lowest occupied slot; per-pad button edges and axes; rumble reaching only its slot's
   pad (and nothing on an empty slot); a full roster, whose waiting pad takes the first slot that frees; and the
   argument checks. BTBHGPUGamepadInfo's layout is asserted below. */
#include "btbh_gpu/btbh_gpu.h"
#include <SDL3/SDL.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(BTBH_GPU_WINDOW_HAS_GAMEPAD_ROSTER == 1, "the roster's feature macro");
_Static_assert(BTBH_GPU_GAMEPAD_CAPACITY == 8u, "eight slots");
_Static_assert(sizeof(BTBHGPUGamepadInfo) == 256u, "BTBHGPUGamepadInfo is 256 bytes");
_Static_assert(_Alignof(BTBHGPUGamepadInfo) == 4u, "BTBHGPUGamepadInfo is aligned to 4");
_Static_assert(offsetof(BTBHGPUGamepadInfo, instance_id) == 0u, "instance_id");
_Static_assert(offsetof(BTBHGPUGamepadInfo, type) == 4u, "type");
_Static_assert(offsetof(BTBHGPUGamepadInfo, vendor_id) == 8u, "vendor_id");
_Static_assert(offsetof(BTBHGPUGamepadInfo, product_id) == 10u, "product_id");
_Static_assert(offsetof(BTBHGPUGamepadInfo, product_version) == 12u, "product_version");
_Static_assert(offsetof(BTBHGPUGamepadInfo, firmware_version) == 14u, "firmware_version");
_Static_assert(offsetof(BTBHGPUGamepadInfo, guid) == 16u, "guid");
_Static_assert(offsetof(BTBHGPUGamepadInfo, flags) == 32u, "flags");
_Static_assert(offsetof(BTBHGPUGamepadInfo, connection) == 36u, "connection");
_Static_assert(offsetof(BTBHGPUGamepadInfo, power) == 37u, "power");
_Static_assert(offsetof(BTBHGPUGamepadInfo, battery_percent) == 38u, "battery_percent");
_Static_assert(offsetof(BTBHGPUGamepadInfo, reserved0) == 39u, "reserved0");
_Static_assert(offsetof(BTBHGPUGamepadInfo, name) == 40u, "name");
_Static_assert(offsetof(BTBHGPUGamepadInfo, serial) == 168u, "serial");
_Static_assert(offsetof(BTBHGPUGamepadInfo, reserved) == 232u, "reserved");
_Static_assert(sizeof(((BTBHGPUGamepadInfo *) 0)->guid) == 16u, "a 16-byte GUID");
_Static_assert(sizeof(((BTBHGPUGamepadInfo *) 0)->name) == 128u, "a 128-byte name");
_Static_assert(sizeof(((BTBHGPUGamepadInfo *) 0)->serial) == 64u, "a 64-byte serial");
_Static_assert(sizeof(((BTBHGPUGamepadInfo *) 0)->reserved) == 24u, "24 reserved bytes");
_Static_assert(sizeof(BTBHGPUGamepadSnapshot) == 36u, "the snapshot keeps its 36 bytes");
_Static_assert(BTBH_GPU_GAMEPAD_CONNECTION_WIRELESS == 2 && BTBH_GPU_GAMEPAD_POWER_CHARGED == 4, "the small enums");
_Static_assert(BTBH_GPU_GAMEPAD_INFO_RUMBLE == 1u, "the rumble flag is bit 0");

static int checks = 0;

#define CHECK(condition) do { checks += 1; if (!(condition)) { \
    fprintf(stderr, "gamepad roster test line %d: %s (%s)\n", __LINE__, #condition, SDL_GetError()); \
    exit(1); \
} } while (0)

/* BTBHGPUGamepadType follows SDL 3's SDL_GamepadType, value for value (checked in main: MSVC warns on a comparison of
   two enum types even in a static assertion). */
static const struct { uint32_t ours; SDL_GamepadType sdl; } gamepad_types[] = {
    {BTBH_GPU_GAMEPAD_TYPE_UNKNOWN, SDL_GAMEPAD_TYPE_UNKNOWN},
    {BTBH_GPU_GAMEPAD_TYPE_STANDARD, SDL_GAMEPAD_TYPE_STANDARD},
    {BTBH_GPU_GAMEPAD_TYPE_XBOX360, SDL_GAMEPAD_TYPE_XBOX360},
    {BTBH_GPU_GAMEPAD_TYPE_XBOXONE, SDL_GAMEPAD_TYPE_XBOXONE},
    {BTBH_GPU_GAMEPAD_TYPE_PS3, SDL_GAMEPAD_TYPE_PS3},
    {BTBH_GPU_GAMEPAD_TYPE_PS4, SDL_GAMEPAD_TYPE_PS4},
    {BTBH_GPU_GAMEPAD_TYPE_PS5, SDL_GAMEPAD_TYPE_PS5},
    {BTBH_GPU_GAMEPAD_TYPE_SWITCH_PRO, SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO},
    {BTBH_GPU_GAMEPAD_TYPE_JOYCON_LEFT, SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT},
    {BTBH_GPU_GAMEPAD_TYPE_JOYCON_RIGHT, SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT},
    {BTBH_GPU_GAMEPAD_TYPE_JOYCON_PAIR, SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR},
    {BTBH_GPU_GAMEPAD_TYPE_GAMECUBE, SDL_GAMEPAD_TYPE_GAMECUBE}
};
_Static_assert(sizeof(gamepad_types) / sizeof(gamepad_types[0]) == (size_t) SDL_GAMEPAD_TYPE_COUNT,
    "every SDL gamepad type has a BTBHGPUGamepadType");

#define VENDOR 0x1209u
#define FIRST_PRODUCT 0xb7c0u
#define PAD_COUNT 16u

typedef struct VirtualPad {
    SDL_JoystickID id;
    SDL_Joystick *joystick; /* the test's own handle, to move its buttons and axes */
    uint16_t product;
    int rumble_calls;
    uint16_t rumble_low;
    uint16_t rumble_high;
} VirtualPad;

static VirtualPad pads[PAD_COUNT];

static bool SDLCALL rumble(void *userdata, Uint16 low, Uint16 high) {
    VirtualPad *pad = (VirtualPad *) userdata;
    pad->rumble_calls += 1;
    pad->rumble_low = low;
    pad->rumble_high = high;
    return true;
}

/* Attaches pads[which], product FIRST_PRODUCT + which, with motors unless told otherwise. */
static VirtualPad *attach(uint32_t which, bool motors) {
    VirtualPad *pad = &pads[which];
    memset(pad, 0, sizeof(*pad));
    pad->product = (uint16_t) (FIRST_PRODUCT + which);
    char name[64];
    (void) snprintf(name, sizeof(name), "BTBH roster pad %c", (char) ('A' + which));
    SDL_VirtualJoystickDesc descriptor;
    SDL_INIT_INTERFACE(&descriptor);
    descriptor.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    descriptor.vendor_id = VENDOR;
    descriptor.product_id = pad->product;
    descriptor.naxes = SDL_GAMEPAD_AXIS_COUNT;
    descriptor.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    descriptor.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1u;
    descriptor.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1u;
    descriptor.name = name;
    descriptor.userdata = pad;
    descriptor.Rumble = motors ? rumble : NULL;
    pad->id = SDL_AttachVirtualJoystick(&descriptor);
    CHECK(pad->id != 0);
    CHECK(SDL_IsGamepad(pad->id));
    pad->joystick = SDL_OpenJoystick(pad->id);
    CHECK(pad->joystick != NULL);
    return pad;
}

static void detach(VirtualPad *pad) {
    SDL_CloseJoystick(pad->joystick);
    pad->joystick = NULL;
    CHECK(SDL_DetachVirtualJoystick(pad->id));
}

static void poll(BTBHGPUWindow *window) {
    BTBHGPUPlatformSnapshot platform = {0};
    CHECK(btbh_gpu_window_poll_events(window, &platform) == BTBH_GPU_OK);
}

static BTBHGPUGamepadSnapshot at(const BTBHGPUWindow *window, uint32_t index) {
    BTBHGPUGamepadSnapshot snapshot;
    memset(&snapshot, 0xA5, sizeof(snapshot));
    CHECK(btbh_gpu_window_gamepad_snapshot_at(window, index, &snapshot) == BTBH_GPU_OK);
    return snapshot;
}

static BTBHGPUGamepadSnapshot single(const BTBHGPUWindow *window) {
    BTBHGPUGamepadSnapshot snapshot;
    memset(&snapshot, 0xA5, sizeof(snapshot));
    CHECK(btbh_gpu_window_gamepad_snapshot(window, &snapshot) == BTBH_GPU_OK);
    return snapshot;
}

static BTBHGPUGamepadInfo info_at(const BTBHGPUWindow *window, uint32_t index) {
    BTBHGPUGamepadInfo info;
    memset(&info, 0xA5, sizeof(info));
    CHECK(btbh_gpu_window_gamepad_info_at(window, index, &info) == BTBH_GPU_OK);
    return info;
}

static bool all_zero(const void *bytes, size_t size) {
    const unsigned char *byte = (const unsigned char *) bytes;
    for (size_t index = 0; index < size; index += 1) {
        if (byte[index] != 0) { return false; }
    }
    return true;
}

/* The roster is expected[0..CAPACITY) (0: empty) with count slots: every slot's snapshot and info carry that id, an
   empty one reads as zeros, and the single-pad call reads the lowest occupied slot. */
static void expect_roster(const BTBHGPUWindow *window, uint32_t count, const SDL_JoystickID *expected) {
    CHECK(btbh_gpu_window_gamepad_count(window) == count);
    SDL_JoystickID first = 0;
    for (uint32_t slot = 0; slot < BTBH_GPU_GAMEPAD_CAPACITY; slot += 1) {
        const BTBHGPUGamepadSnapshot snapshot = at(window, slot);
        const BTBHGPUGamepadInfo info = info_at(window, slot);
        CHECK(snapshot.instance_id == expected[slot]);
        CHECK(info.instance_id == expected[slot]);
        if (expected[slot] == 0) {
            CHECK(all_zero(&snapshot, sizeof(snapshot)));
            CHECK(all_zero(&info, sizeof(info)));
            CHECK(slot >= count || slot + 1 < count);
        } else if (first == 0) {
            first = expected[slot];
        }
    }
    const BTBHGPUGamepadSnapshot lowest = single(window);
    CHECK(lowest.instance_id == first);
    if (first == 0) { CHECK(all_zero(&lowest, sizeof(lowest))); }
}

/* expect_roster with slots 0-2 given and the rest empty. */
static void expect_slots(
    const BTBHGPUWindow *window,
    uint32_t count,
    SDL_JoystickID slot0,
    SDL_JoystickID slot1,
    SDL_JoystickID slot2
) {
    SDL_JoystickID expected[BTBH_GPU_GAMEPAD_CAPACITY];
    memset(expected, 0, sizeof(expected));
    expected[0] = slot0;
    expected[1] = slot1;
    expected[2] = slot2;
    expect_roster(window, count, expected);
}

static void check_identity(const BTBHGPUWindow *window, uint32_t slot, const VirtualPad *pad, bool motors) {
    const BTBHGPUGamepadInfo info = info_at(window, slot);
    CHECK(info.instance_id == pad->id);
    CHECK(info.vendor_id == VENDOR);
    CHECK(info.product_id == pad->product);
    CHECK(info.product_id == SDL_GetGamepadProductForID(pad->id));
    CHECK(info.product_version == SDL_GetGamepadProductVersionForID(pad->id));
    const SDL_GUID guid = SDL_GetGamepadGUIDForID(pad->id);
    CHECK(memcmp(info.guid, guid.data, sizeof(info.guid)) == 0);
    CHECK(!all_zero(info.guid, sizeof(info.guid)));
    CHECK(info.type == (uint32_t) SDL_GetGamepadTypeForID(pad->id));
    const char *name = SDL_GetGamepadNameForID(pad->id);
    CHECK(name != NULL && strcmp(info.name, name) == 0);
    CHECK(info.serial[0] == '\0'); /* a virtual pad has none */
    CHECK(((info.flags & BTBH_GPU_GAMEPAD_INFO_RUMBLE) != 0) == motors);
    CHECK((info.flags & ~(uint32_t) BTBH_GPU_GAMEPAD_INFO_RUMBLE) == 0);
    CHECK(info.connection <= BTBH_GPU_GAMEPAD_CONNECTION_WIRELESS);
    CHECK(info.power <= BTBH_GPU_GAMEPAD_POWER_CHARGED);
    CHECK(info.battery_percent >= -1 && info.battery_percent <= 100);
    CHECK(info.reserved0 == 0 && all_zero(info.reserved, sizeof(info.reserved)));
    CHECK(info.name[sizeof(info.name) - 1] == '\0' && info.serial[sizeof(info.serial) - 1] == '\0');
}

int main(void) {
    char hint[PAD_COUNT * 16];
    size_t used = 0;
    for (uint32_t which = 0; which < PAD_COUNT; which += 1) {
        used += (size_t) snprintf(hint + used, sizeof(hint) - used, "%s0x%04x/0x%04x",
            which == 0 ? "" : ",", VENDOR, FIRST_PRODUCT + which);
    }
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, hint);
    CHECK(SDL_InitSubSystem(SDL_INIT_GAMEPAD));
    for (size_t index = 0; index < sizeof(gamepad_types) / sizeof(gamepad_types[0]); index += 1) {
        CHECK(gamepad_types[index].ours == index && (uint32_t) gamepad_types[index].sdl == index);
    }

    /* Arguments, before any window. */
    BTBHGPUGamepadSnapshot snapshot;
    BTBHGPUGamepadInfo info;
    CHECK(btbh_gpu_window_gamepad_count(NULL) == 0);
    memset(&snapshot, 0xA5, sizeof(snapshot));
    CHECK(btbh_gpu_window_gamepad_snapshot_at(NULL, 0, &snapshot) == BTBH_GPU_INVALID_ARGUMENT);
    CHECK(all_zero(&snapshot, sizeof(snapshot)));
    CHECK(btbh_gpu_window_gamepad_info_at(NULL, 0, &info) == BTBH_GPU_INVALID_ARGUMENT);
    CHECK(btbh_gpu_window_gamepad_rumble_at(NULL, 0, 1, 1, 1) == BTBH_GPU_INVALID_ARGUMENT);

    /* Two pads connected before the window: it opens both, in SDL's order. */
    VirtualPad *a = attach(0, true);
    VirtualPad *b = attach(1, true);
    BTBHGPUWindowDescriptor descriptor = {0};
    descriptor.title = "Gamepad roster test";
    descriptor.width = 64;
    descriptor.height = 64;
    descriptor.hidden = 1;
    BTBHGPUWindowReport report = {0};
    BTBHGPUWindow *window = btbh_gpu_window_create(&descriptor, &report);
    CHECK(window != NULL);
    CHECK(btbh_gpu_window_gamepad_count(window) == 2);
    poll(window);
    expect_slots(window, 2, a->id, b->id, 0);

    /* Arguments with a window. */
    memset(&snapshot, 0xA5, sizeof(snapshot));
    CHECK(btbh_gpu_window_gamepad_snapshot_at(window, BTBH_GPU_GAMEPAD_CAPACITY, &snapshot) ==
        BTBH_GPU_INVALID_ARGUMENT);
    CHECK(all_zero(&snapshot, sizeof(snapshot)));
    CHECK(btbh_gpu_window_gamepad_snapshot_at(window, 0xFFFFFFFFu, &snapshot) == BTBH_GPU_INVALID_ARGUMENT);
    memset(&info, 0xA5, sizeof(info));
    CHECK(btbh_gpu_window_gamepad_info_at(window, BTBH_GPU_GAMEPAD_CAPACITY, &info) == BTBH_GPU_INVALID_ARGUMENT);
    CHECK(all_zero(&info, sizeof(info)));
    CHECK(btbh_gpu_window_gamepad_snapshot_at(window, 0, NULL) == BTBH_GPU_INVALID_ARGUMENT);
    CHECK(btbh_gpu_window_gamepad_info_at(window, 0, NULL) == BTBH_GPU_INVALID_ARGUMENT);
    CHECK(btbh_gpu_window_gamepad_rumble_at(window, BTBH_GPU_GAMEPAD_CAPACITY, 1, 1, 1) ==
        BTBH_GPU_INVALID_ARGUMENT);

    /* A third pad hot-plugs into slot 2; identities. */
    VirtualPad *c = attach(2, true);
    CHECK(btbh_gpu_window_gamepad_count(window) == 2); /* the roster changes only in a poll */
    poll(window);
    expect_slots(window, 3, a->id, b->id, c->id);
    check_identity(window, 0, a, true);
    check_identity(window, 1, b, true);
    check_identity(window, 2, c, true);
    {
        const BTBHGPUGamepadInfo first = info_at(window, 0);
        const BTBHGPUGamepadInfo second = info_at(window, 1);
        CHECK(memcmp(first.guid, second.guid, sizeof(first.guid)) != 0);
        printf("slot 0: \"%s\" %04x:%04x type %u connection %u power %u battery %d\n", first.name,
            (unsigned) first.vendor_id, (unsigned) first.product_id, (unsigned) first.type,
            (unsigned) first.connection, (unsigned) first.power, (int) first.battery_percent);
    }

    /* Per-pad button edges: a press on B is B's alone. */
    CHECK(SDL_SetJoystickVirtualButton(b->joystick, SDL_GAMEPAD_BUTTON_SOUTH, true));
    poll(window);
    CHECK(at(window, 1).buttons_down == 1u && at(window, 1).buttons_pressed == 1u);
    CHECK(at(window, 0).buttons_down == 0 && at(window, 0).buttons_pressed == 0);
    CHECK(at(window, 2).buttons_down == 0 && at(window, 2).buttons_pressed == 0);
    CHECK(single(window).buttons_pressed == 0);
    poll(window);
    CHECK(at(window, 1).buttons_down == 1u && at(window, 1).buttons_pressed == 0);
    CHECK(SDL_SetJoystickVirtualButton(b->joystick, SDL_GAMEPAD_BUTTON_SOUTH, false));
    poll(window);
    CHECK(at(window, 1).buttons_down == 0 && at(window, 1).buttons_pressed == 0);
    /* A press held on two pads at once: each sees its own edge once. */
    CHECK(SDL_SetJoystickVirtualButton(a->joystick, SDL_GAMEPAD_BUTTON_NORTH, true));
    CHECK(SDL_SetJoystickVirtualButton(c->joystick, SDL_GAMEPAD_BUTTON_EAST, true));
    poll(window);
    CHECK(at(window, 0).buttons_pressed == (1u << 3) && at(window, 0).buttons_down == (1u << 3));
    CHECK(at(window, 2).buttons_pressed == (1u << 1) && at(window, 2).buttons_down == (1u << 1));
    CHECK(at(window, 1).buttons_pressed == 0);
    CHECK(single(window).buttons_pressed == (1u << 3));
    CHECK(SDL_SetJoystickVirtualButton(a->joystick, SDL_GAMEPAD_BUTTON_NORTH, false));
    CHECK(SDL_SetJoystickVirtualButton(c->joystick, SDL_GAMEPAD_BUTTON_EAST, false));
    poll(window);
    /* A complete tap on C between two polls (down and up events only) is C's press. */
    SDL_Event event;
    memset(&event, 0, sizeof(event));
    event.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    event.gbutton.which = c->id;
    event.gbutton.button = SDL_GAMEPAD_BUTTON_START;
    CHECK(SDL_PushEvent(&event));
    event.type = SDL_EVENT_GAMEPAD_BUTTON_UP;
    CHECK(SDL_PushEvent(&event));
    poll(window);
    CHECK(at(window, 2).buttons_pressed == (1u << 8) && at(window, 2).buttons_down == 0);
    CHECK(at(window, 0).buttons_pressed == 0 && at(window, 1).buttons_pressed == 0);
    /* Axes per pad. */
    CHECK(SDL_SetJoystickVirtualAxis(c->joystick, SDL_GAMEPAD_AXIS_LEFTX, -32768));
    CHECK(SDL_SetJoystickVirtualAxis(b->joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767));
    poll(window);
    CHECK(at(window, 2).left_x == -1.0f && at(window, 2).right_trigger == 0.0f);
    CHECK(at(window, 1).right_trigger == 1.0f && at(window, 1).left_x == 0.0f);
    CHECK(at(window, 0).left_x == 0.0f && at(window, 0).right_trigger == 0.0f);
    CHECK(SDL_SetJoystickVirtualAxis(c->joystick, SDL_GAMEPAD_AXIS_LEFTX, 0));
    CHECK(SDL_SetJoystickVirtualAxis(b->joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0));
    poll(window);

    /* Rumble reaches its slot's pad only; the single-pad call rumbles slot 0. */
    CHECK(btbh_gpu_window_gamepad_rumble_at(window, 1, 1000, 2000, 5000) == BTBH_GPU_OK);
    CHECK(b->rumble_low == 1000 && b->rumble_high == 2000 && b->rumble_calls >= 1);
    CHECK(a->rumble_calls == 0 && c->rumble_calls == 0);
    CHECK(btbh_gpu_window_gamepad_rumble(window, 3000, 4000, 5000) == BTBH_GPU_OK);
    CHECK(a->rumble_low == 3000 && a->rumble_high == 4000);
    CHECK(b->rumble_low == 1000 && c->rumble_calls == 0);
    /* An empty slot: success, and no pad moves. */
    const int calls_before = a->rumble_calls + b->rumble_calls + c->rumble_calls;
    CHECK(btbh_gpu_window_gamepad_rumble_at(window, 5, 5000, 6000, 5000) == BTBH_GPU_OK);
    CHECK(btbh_gpu_window_gamepad_rumble_at(window, BTBH_GPU_GAMEPAD_CAPACITY - 1, 5000, 6000, 5000) == BTBH_GPU_OK);
    CHECK(a->rumble_calls + b->rumble_calls + c->rumble_calls == calls_before);
    CHECK(a->rumble_low == 3000 && b->rumble_low == 1000 && c->rumble_calls == 0);
    CHECK(btbh_gpu_window_gamepad_rumble_at(window, 1, 0, 0, 0) == BTBH_GPU_OK);
    CHECK(btbh_gpu_window_gamepad_rumble(window, 0, 0, 0) == BTBH_GPU_OK);
    CHECK(a->rumble_low == 0 && b->rumble_low == 0);

    /* B leaves: its slot stays empty and moves nobody. */
    detach(b);
    poll(window);
    expect_slots(window, 3, a->id, 0, c->id);
    CHECK(btbh_gpu_window_gamepad_rumble_at(window, 1, 7000, 7000, 5000) == BTBH_GPU_OK);
    CHECK(a->rumble_low == 0 && c->rumble_calls == 0);

    /* A leaves: the single-pad calls fall over to C, the lowest occupied slot. */
    detach(a);
    poll(window);
    expect_slots(window, 3, 0, 0, c->id);
    CHECK(btbh_gpu_window_gamepad_rumble(window, 8000, 9000, 5000) == BTBH_GPU_OK);
    CHECK(c->rumble_low == 8000 && c->rumble_high == 9000);
    CHECK(btbh_gpu_window_gamepad_rumble(window, 0, 0, 0) == BTBH_GPU_OK);
    CHECK(SDL_SetJoystickVirtualButton(c->joystick, SDL_GAMEPAD_BUTTON_WEST, true));
    poll(window);
    CHECK(single(window).instance_id == c->id && single(window).buttons_pressed == (1u << 2));
    CHECK(SDL_SetJoystickVirtualButton(c->joystick, SDL_GAMEPAD_BUTTON_WEST, false));
    poll(window);

    /* A pad connecting takes the lowest free slot, 0, and the single-pad calls with it; the next one slot 1. A pad
       without motors reports no rumble, and rumbling it succeeds. */
    VirtualPad *d = attach(3, true);
    poll(window);
    expect_slots(window, 3, d->id, 0, c->id);
    VirtualPad *e = attach(4, false);
    poll(window);
    expect_slots(window, 3, d->id, e->id, c->id);
    check_identity(window, 0, d, true);
    check_identity(window, 1, e, false);
    CHECK(btbh_gpu_window_gamepad_rumble_at(window, 1, 1234, 4321, 5000) == BTBH_GPU_OK);
    CHECK(d->rumble_calls == 0 && c->rumble_low == 0);

    /* C leaves from the top: the count shrinks to the highest occupied slot. */
    detach(c);
    poll(window);
    expect_slots(window, 2, d->id, e->id, 0);

    /* A full roster: slots 2-7 fill, a ninth pad waits unopened, and takes the first slot that frees. */
    SDL_JoystickID full[BTBH_GPU_GAMEPAD_CAPACITY];
    memset(full, 0, sizeof(full));
    full[0] = d->id;
    full[1] = e->id;
    for (uint32_t slot = 2; slot < BTBH_GPU_GAMEPAD_CAPACITY; slot += 1) {
        full[slot] = attach(5 + slot - 2, true)->id; /* pads 5-10 */
        poll(window);
    }
    expect_roster(window, BTBH_GPU_GAMEPAD_CAPACITY, full);
    VirtualPad *waiting = attach(11, true);
    poll(window);
    expect_roster(window, BTBH_GPU_GAMEPAD_CAPACITY, full);
    detach(&pads[6]); /* slot 3 */
    poll(window);
    full[3] = waiting->id;
    expect_roster(window, BTBH_GPU_GAMEPAD_CAPACITY, full);
    check_identity(window, 3, waiting, true);

    /* Everyone leaves. */
    detach(d);
    detach(e);
    for (uint32_t which = 5; which <= 11; which += 1) {
        if (which != 6) { detach(&pads[which]); }
    }
    poll(window);
    expect_slots(window, 0, 0, 0, 0);
    CHECK(btbh_gpu_window_gamepad_rumble(window, 1, 1, 1) == BTBH_GPU_OK);
    CHECK(btbh_gpu_window_gamepad_rumble_at(window, 0, 1, 1, 1) == BTBH_GPU_OK);

    /* A pad after an empty roster: slot 0 again. */
    VirtualPad *f = attach(12, true);
    poll(window);
    expect_slots(window, 1, f->id, 0, 0);
    btbh_gpu_window_destroy(window);
    /* The window releases only its own subsystem reference. */
    CHECK(SDL_WasInit(SDL_INIT_GAMEPAD) != 0);
    detach(f);
    SDL_Quit();
    printf("BTBH_GPU_GAMEPAD_ROSTER_TEST_OK checks=%d slots=%u hotplug=ok identity=ok edges=ok rumble=ok full=ok\n",
        checks, BTBH_GPU_GAMEPAD_CAPACITY);
    return 0;
}
