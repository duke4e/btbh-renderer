#ifndef BTBH_GPU_H
#define BTBH_GPU_H

#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

/* The version of this API (semver, COORDINATION.md); release tags are v<major>.<minor>.<patch>. */
#define BTBH_GPU_VERSION_MAJOR 0
#define BTBH_GPU_VERSION_MINOR 15
#define BTBH_GPU_VERSION_PATCH 0

#define BTBH_GPU_DRIVER_NAME_CAPACITY 32u
/* Lamps (street lamps, car headlights) a scene draw weighs per pixel (BTBHGPUSceneLighting.lamp_*). */
#define BTBH_GPU_SCENE_MAX_LAMPS 40u

typedef enum BTBHGPUBackendRequest {
    BTBH_GPU_BACKEND_INVALID = 0,
    BTBH_GPU_BACKEND_VULKAN = 1,
    BTBH_GPU_BACKEND_METAL = 2
} BTBHGPUBackendRequest;

typedef enum BTBHGPUShaderFormat {
    BTBH_GPU_SHADER_FORMAT_INVALID = 0,
    BTBH_GPU_SHADER_FORMAT_SPIRV = 1,
    BTBH_GPU_SHADER_FORMAT_METALLIB = 2
} BTBHGPUShaderFormat;

typedef enum BTBHGPUResult {
    BTBH_GPU_OK = 0,
    BTBH_GPU_INVALID_ARGUMENT = 1,
    BTBH_GPU_BACKEND_MISMATCH = 2,
    BTBH_GPU_SOFTWARE_DEVICE = 3,
    BTBH_GPU_MISSING_CAPABILITY = 4,
    BTBH_GPU_PLATFORM_FAILURE = 5,
    BTBH_GPU_DEVICE_FAILURE = 6,
    BTBH_GPU_WINDOW_FAILURE = 7
} BTBHGPUResult;

typedef enum BTBHGPUErrorStage {
    BTBH_GPU_ERROR_STAGE_NONE = 0,
    BTBH_GPU_ERROR_STAGE_PLATFORM_INITIALIZATION = 1,
    BTBH_GPU_ERROR_STAGE_DEVICE_PROPERTIES = 2,
    BTBH_GPU_ERROR_STAGE_DEVICE_CREATION = 3,
    BTBH_GPU_ERROR_STAGE_DRIVER_VALIDATION = 4,
    BTBH_GPU_ERROR_STAGE_CAPABILITY_VALIDATION = 5,
    BTBH_GPU_ERROR_STAGE_WINDOW_CREATION = 6,
    BTBH_GPU_ERROR_STAGE_WINDOW_CLAIM = 7,
    BTBH_GPU_ERROR_STAGE_SWAPCHAIN_CONFIGURATION = 8,
    BTBH_GPU_ERROR_STAGE_WINDOW_RESIZE = 9
} BTBHGPUErrorStage;

typedef uint64_t BTBHGPUFeatureFlags;

#define BTBH_GPU_FEATURE_GBUFFER_RGBA8_SRGB      (UINT64_C(1) << 0u)
#define BTBH_GPU_FEATURE_GBUFFER_RGBA8_UNORM     (UINT64_C(1) << 1u)
#define BTBH_GPU_FEATURE_GBUFFER_RGBA16_FLOAT    (UINT64_C(1) << 2u)
#define BTBH_GPU_FEATURE_SAMPLEABLE_DEPTH        (UINT64_C(1) << 3u)
#define BTBH_GPU_FEATURE_SHADOW_2D_ARRAY         (UINT64_C(1) << 4u)
#define BTBH_GPU_FEATURE_SHADOW_CUBE_ARRAY       (UINT64_C(1) << 5u)
#define BTBH_GPU_FEATURE_STORAGE_RGBA16_FLOAT    (UINT64_C(1) << 6u)
#define BTBH_GPU_FEATURE_COMPUTE                 (UINT64_C(1) << 7u)
#define BTBH_GPU_FEATURE_TEXTURE_READBACK        (UINT64_C(1) << 8u)
#define BTBH_GPU_REQUIRED_FEATURES ( \
    BTBH_GPU_FEATURE_GBUFFER_RGBA8_SRGB | \
    BTBH_GPU_FEATURE_GBUFFER_RGBA8_UNORM | \
    BTBH_GPU_FEATURE_GBUFFER_RGBA16_FLOAT | \
    BTBH_GPU_FEATURE_SAMPLEABLE_DEPTH | \
    BTBH_GPU_FEATURE_SHADOW_2D_ARRAY | \
    BTBH_GPU_FEATURE_SHADOW_CUBE_ARRAY | \
    BTBH_GPU_FEATURE_STORAGE_RGBA16_FLOAT | \
    BTBH_GPU_FEATURE_COMPUTE | \
    BTBH_GPU_FEATURE_TEXTURE_READBACK \
)
/* Deprecated: the name before the GPU spike was removed. Kept for hosts that still use it. */
#define BTBH_GPU_PHASE2_REQUIRED_FEATURES BTBH_GPU_REQUIRED_FEATURES

typedef struct BTBHGPUCapabilitySnapshot {
    char driver_name[BTBH_GPU_DRIVER_NAME_CAPACITY];
    uint8_t hardware_accelerated;
    uint8_t reserved[7];
    BTBHGPUFeatureFlags features;
} BTBHGPUCapabilitySnapshot;

typedef struct BTBHGPUValidationReport {
    BTBHGPUResult result;
    char expected_driver[BTBH_GPU_DRIVER_NAME_CAPACITY];
    char actual_driver[BTBH_GPU_DRIVER_NAME_CAPACITY];
    BTBHGPUFeatureFlags missing_features;
} BTBHGPUValidationReport;

typedef struct BTBHGPUDeviceDescriptor {
    BTBHGPUBackendRequest backend;
    BTBHGPUFeatureFlags required_features;
    uint8_t debug_mode;
    uint8_t reserved[7];
} BTBHGPUDeviceDescriptor;

typedef struct BTBHGPUDeviceReport {
    BTBHGPUResult result;
    BTBHGPUErrorStage error_stage;
    BTBHGPUCapabilitySnapshot capabilities;
    BTBHGPUValidationReport validation;
    char message[256];
} BTBHGPUDeviceReport;

typedef struct BTBHGPUDevice BTBHGPUDevice;

typedef enum BTBHGPUTextureFormat {
    BTBH_GPU_TEXTURE_FORMAT_INVALID = 0,
    BTBH_GPU_TEXTURE_FORMAT_RGBA8_UNORM = 1,
    BTBH_GPU_TEXTURE_FORMAT_RGBA8_SRGB = 2,
    BTBH_GPU_TEXTURE_FORMAT_BGRA8_UNORM = 3,
    BTBH_GPU_TEXTURE_FORMAT_BGRA8_SRGB = 4,
    BTBH_GPU_TEXTURE_FORMAT_RGB10A2_UNORM = 5,
    BTBH_GPU_TEXTURE_FORMAT_RGBA16_FLOAT = 6
} BTBHGPUTextureFormat;

typedef enum BTBHGPUPresentMode {
    BTBH_GPU_PRESENT_MODE_INVALID = 0,
    BTBH_GPU_PRESENT_MODE_VSYNC = 1,
    BTBH_GPU_PRESENT_MODE_IMMEDIATE = 2,
    BTBH_GPU_PRESENT_MODE_MAILBOX = 3
} BTBHGPUPresentMode;

typedef enum BTBHGPUSwapchainComposition {
    BTBH_GPU_SWAPCHAIN_INVALID = 0,
    BTBH_GPU_SWAPCHAIN_SDR = 1
} BTBHGPUSwapchainComposition;

typedef struct BTBHGPUWindowDescriptor {
    const char *title;
    uint32_t width;
    uint32_t height;
    uint8_t hidden;
    uint8_t high_pixel_density;
    /* Non-zero lets the user resize and maximize the window. */
    uint8_t resizable;
    /* Non-zero makes Alt+Enter switch between the window and borderless desktop
       fullscreen; that key press never reaches the key state. */
    uint8_t fullscreen_toggle;
    /* 1...100 sizes the window to that percentage of the primary display (keeping the
       width:height aspect and fitting the usable area); 0 uses width x height exactly. */
    uint8_t display_percent;
    /* The present mode the claim asks for, a BTBHGPUPresentMode (since 0.3.0).
       0 (BTBH_GPU_PRESENT_MODE_INVALID) takes the first of IMMEDIATE, MAILBOX and VSYNC that the
       window supports, as before. VSYNC, IMMEDIATE or MAILBOX asks for that mode, and takes VSYNC,
       which every window supports, where the window lacks it. Any other value fails the create.
       BTBHGPUWindowReport.present_mode says what was taken; btbh_gpu_window_set_present_mode
       switches it later. */
    uint8_t present_mode;
    uint8_t reserved[2];
} BTBHGPUWindowDescriptor;

/* Defined when BTBHGPUWindowDescriptor.present_mode and btbh_gpu_window_set_present_mode exist
   (since 0.3.0). */
#define BTBH_GPU_WINDOW_HAS_PRESENT_MODE 1

typedef struct BTBHGPUWindowReport {
    BTBHGPUResult result;
    BTBHGPUErrorStage error_stage;
    uint32_t logical_width;
    uint32_t logical_height;
    uint32_t drawable_width;
    uint32_t drawable_height;
    BTBHGPUTextureFormat swapchain_format;
    BTBHGPUPresentMode present_mode;
    BTBHGPUSwapchainComposition composition;
    uint8_t claimed;
    uint8_t minimized;
    uint8_t hidden;
    uint8_t reserved[5];
    char message[256];
} BTBHGPUWindowReport;

typedef struct BTBHGPUPlatformSnapshot {
    double elapsed_seconds;
    float delta_seconds;
    float mouse_x;
    float mouse_y;
    float mouse_delta_x;
    float mouse_delta_y;
    float mouse_wheel_x;
    float mouse_wheel_y;
    int32_t refresh_rate;
    int32_t frames_per_second;
    uint8_t should_close;
    uint8_t focused;
    uint8_t cursor_visible;
    uint8_t reserved[5];
} BTBHGPUPlatformSnapshot;

/* Button bits follow the public GamepadButton order: south, east, west,
 * north, left/right shoulder, left/right stick, start, back, guide,
 * dpad up/down/left/right. Axis values are normalized to [-1, 1]; triggers
 * use [0, 1]. instance_id == 0 means disconnected. */
typedef struct BTBHGPUGamepadSnapshot {
    uint32_t instance_id;
    uint32_t buttons_down;
    uint32_t buttons_pressed;
    float left_x;
    float left_y;
    float right_x;
    float right_y;
    float left_trigger;
    float right_trigger;
} BTBHGPUGamepadSnapshot;

/* The window's gamepad roster (since 0.15.0), for local co-op: the window opens every gamepad that
   connects, up to BTBH_GPU_GAMEPAD_CAPACITY, each in a slot.
   - A pad takes the lowest free slot, in the order SDL reports them connected (pads already
     connected when the window is made, in SDL's order, then each new one as it arrives).
   - A slot keeps its pad while it is connected. A pad that leaves empties its slot and moves no
     other pad; the next pad to connect takes the lowest free slot, so a player whose pad drops and
     comes back alone gets the slot back. A slot is a seat, not an identity: to give a returning pad
     its own player when several pads come and go, compare BTBHGPUGamepadInfo (serial, guid, vendor
     and product).
   - btbh_gpu_window_gamepad_count is the number of slots up to the highest occupied one, so a
     loop over 0..count visits every pad; empty slots in between read as disconnected.
   - With every slot taken, a further pad waits unopened and takes the first slot that frees.
   - btbh_gpu_window_gamepad_snapshot and btbh_gpu_window_gamepad_rumble keep their meaning: the
     lowest occupied slot's pad, which falls over to the next pad when it leaves (and back to a pad
     that connects into a lower slot). Use the _at calls to hold one pad per player.
   - Each slot's buttons_pressed has its own edges: the button-down events of that pad between two
     btbh_gpu_window_poll_events calls, and the presses its sampling sees. */
#define BTBH_GPU_GAMEPAD_CAPACITY 8u

/* BTBHGPUGamepadInfo.type: the kind of pad, for button glyphs (SDL 3's SDL_GamepadType, in its
   order). STANDARD is a pad SDL knows by mapping only; UNKNOWN one it can't tell. */
typedef enum BTBHGPUGamepadType {
    BTBH_GPU_GAMEPAD_TYPE_UNKNOWN = 0,
    BTBH_GPU_GAMEPAD_TYPE_STANDARD = 1,
    BTBH_GPU_GAMEPAD_TYPE_XBOX360 = 2,
    BTBH_GPU_GAMEPAD_TYPE_XBOXONE = 3,
    BTBH_GPU_GAMEPAD_TYPE_PS3 = 4,
    BTBH_GPU_GAMEPAD_TYPE_PS4 = 5,
    BTBH_GPU_GAMEPAD_TYPE_PS5 = 6,
    BTBH_GPU_GAMEPAD_TYPE_SWITCH_PRO = 7,
    BTBH_GPU_GAMEPAD_TYPE_JOYCON_LEFT = 8,
    BTBH_GPU_GAMEPAD_TYPE_JOYCON_RIGHT = 9,
    BTBH_GPU_GAMEPAD_TYPE_JOYCON_PAIR = 10,
    BTBH_GPU_GAMEPAD_TYPE_GAMECUBE = 11
} BTBHGPUGamepadType;

/* BTBHGPUGamepadInfo.connection. */
typedef enum BTBHGPUGamepadConnection {
    BTBH_GPU_GAMEPAD_CONNECTION_UNKNOWN = 0,
    BTBH_GPU_GAMEPAD_CONNECTION_WIRED = 1,
    BTBH_GPU_GAMEPAD_CONNECTION_WIRELESS = 2
} BTBHGPUGamepadConnection;

/* BTBHGPUGamepadInfo.power: the pad's battery, as SDL reports it at the call. */
typedef enum BTBHGPUGamepadPower {
    BTBH_GPU_GAMEPAD_POWER_UNKNOWN = 0,
    BTBH_GPU_GAMEPAD_POWER_ON_BATTERY = 1,
    BTBH_GPU_GAMEPAD_POWER_NO_BATTERY = 2,
    BTBH_GPU_GAMEPAD_POWER_CHARGING = 3,
    BTBH_GPU_GAMEPAD_POWER_CHARGED = 4
} BTBHGPUGamepadPower;

/* BTBHGPUGamepadInfo.flags. */
enum {
    /* The pad has rumble motors: btbh_gpu_window_gamepad_rumble_at moves them. */
    BTBH_GPU_GAMEPAD_INFO_RUMBLE = 1u << 0
};

/* One roster slot's pad (btbh_gpu_window_gamepad_info_at). An empty slot reads as all zeros
   (instance_id 0). Strings are UTF-8, NUL-terminated and cut at a character boundary to fit; an
   empty string means SDL has none. The trailing reserved bytes are zero; a later version may fill
   them, so the struct never grows. 256 bytes. */
typedef struct BTBHGPUGamepadInfo {
    /* SDL's joystick instance id, as BTBHGPUGamepadSnapshot.instance_id: never 0 for a pad, and never
       reused for another pad while the program runs. */
    uint32_t instance_id;
    /* A BTBHGPUGamepadType. */
    uint32_t type;
    /* The USB (or Bluetooth) ids, 0 when unknown. */
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t product_version;
    uint16_t firmware_version;
    /* SDL's GUID for the pad (the bytes SDL_GUIDToString prints as 32 hex digits): the same for every
       pad of one model on one driver, so with serial it tells pads apart across reconnections. */
    uint8_t guid[16];
    /* BTBH_GPU_GAMEPAD_INFO_* bits. */
    uint32_t flags;
    /* A BTBHGPUGamepadConnection. */
    uint8_t connection;
    /* A BTBHGPUGamepadPower. */
    uint8_t power;
    /* 0 to 100, or -1 when SDL can't tell. */
    int8_t battery_percent;
    uint8_t reserved0;
    /* The pad's name as SDL gives it ("Xbox Series X Controller", "PS5 Controller"). */
    char name[128];
    /* The pad's serial number, where its driver reports one (many don't: then empty). */
    char serial[64];
    uint8_t reserved[24];
} BTBHGPUGamepadInfo;

/* Defined when BTBH_GPU_GAMEPAD_CAPACITY, BTBHGPUGamepadInfo and the roster calls
   (btbh_gpu_window_gamepad_count, _snapshot_at, _info_at, _rumble_at) exist (since 0.15.0). */
#define BTBH_GPU_WINDOW_HAS_GAMEPAD_ROSTER 1

typedef enum BTBHGPUFileDialogKind {
    BTBH_GPU_FILE_DIALOG_OPEN = 1,
    BTBH_GPU_FILE_DIALOG_SAVE = 2
} BTBHGPUFileDialogKind;

typedef struct BTBHGPUFileDialogDescriptor {
    BTBHGPUFileDialogKind kind;
    const char *title;
    const char *const *filter_names;
    const char *const *filter_patterns;
    uint32_t filter_count;
    uint8_t allows_multiple_selection;
    uint8_t reserved[3];
    const char *location;
} BTBHGPUFileDialogDescriptor;

typedef struct BTBHGPUFileDialogReport {
    BTBHGPUResult result;
    char message[256];
} BTBHGPUFileDialogReport;

typedef void (*BTBHGPUFileDialogCallback)(
    void *userdata,
    const char *const *paths,
    const char *error_message
);

typedef struct BTBHGPUWindow BTBHGPUWindow;
typedef struct BTBHGPUTexture BTBHGPUTexture;
typedef struct BTBHGPUFont BTBHGPUFont;
typedef struct BTBHGPUModel BTBHGPUModel;
typedef struct BTBHGPUInstanceBuffer BTBHGPUInstanceBuffer;
typedef struct BTBHGPUCustomMesh BTBHGPUCustomMesh;
typedef struct BTBHGPUAnimationLibrary BTBHGPUAnimationLibrary;
typedef struct BTBHGPUAnimationPose BTBHGPUAnimationPose;
/* Destructible terrain from SDF edits (btbh_gpu_sdf_terrain.h, since 0.5.0). */
typedef struct BTBHGPUSdfTerrain BTBHGPUSdfTerrain;

typedef enum BTBHGPUTextureFilter {
    BTBH_GPU_TEXTURE_FILTER_NEAREST = 0,
    BTBH_GPU_TEXTURE_FILTER_LINEAR = 1,
    BTBH_GPU_TEXTURE_FILTER_TRILINEAR = 2
} BTBHGPUTextureFilter;

typedef enum BTBHGPUTextureWrap {
    BTBH_GPU_TEXTURE_WRAP_CLAMP = 0,
    BTBH_GPU_TEXTURE_WRAP_REPEAT = 1,
    BTBH_GPU_TEXTURE_WRAP_MIRROR = 2
} BTBHGPUTextureWrap;

typedef enum BTBHGPUTextureColorSpace {
    BTBH_GPU_TEXTURE_COLOR_SPACE_LINEAR = 0,
    BTBH_GPU_TEXTURE_COLOR_SPACE_SRGB = 1
} BTBHGPUTextureColorSpace;

typedef struct BTBHGPUTextureReport {
    BTBHGPUResult result;
    uint32_t width;
    uint32_t height;
    uint32_t mip_count;
    char message[256];
} BTBHGPUTextureReport;

typedef struct BTBHGPUFontReport {
    BTBHGPUResult result;
    uint32_t base_size;
    uint32_t glyph_count;
    uint32_t atlas_width;
    uint32_t atlas_height;
    float ascent;
    float descent;
    float line_gap;
    char message[256];
} BTBHGPUFontReport;

typedef struct BTBHGPUFontGlyph {
    uint32_t codepoint;
    float advance;
    float offset_x;
    float offset_y;
    float width;
    float height;
    float u0;
    float v0;
    float u1;
    float v1;
} BTBHGPUFontGlyph;

/* BTBHGPUFontDesc.flags (since 0.5.0). Zero is the 0.4.0 bake, byte for byte. */
enum {
    /* A one-channel atlas: R8 coverage instead of RGBA8 (white, coverage in alpha), a quarter of the
       memory at the same width and height; the glyphs, metrics and kerning are the same. The canvas
       draws a batch whose texture is a one-channel texture as white x coverage x the vertex colour,
       which is what it draws from the RGBA8 atlas (within a level of rounding, should a GPU filter the
       two formats differently). For that it loads
       production_canvas_coverage.frag (.spv or .metallib, as the other shaders) from the directory of
       btbh_gpu_window_canvas_present's textured_fragment_shader_path, the first time a batch needs it:
       ship it with the other canvas shaders, or that present fails with BTBH_GPU_INVALID_ARGUMENT and
       the path in its report. Anywhere but the canvas (a scene material, a readback) the atlas is an
       R8 texture, and btbh_gpu_texture_update_rgba8 refuses it. */
    BTBH_GPU_FONT_FLAG_R8_ATLAS = 1u << 0,
    /* Puts the x-height on a whole pixel, a cheap stand-in for vertical hinting: glyphs are rasterised
       with a vertical scale of scale x round(x) / x, x being the x-height in pixels at pixel_size, so
       the top of flat lowercase ('x', 'z', 'u', stems) sits on a pixel edge on a whole-pixel baseline
       instead of leaving a grey row. The horizontal scale, advances and kerning don't change, so
       spacing stays even. btbh_gpu_font_metrics reports the scale used: x_height is round(x) (a whole
       number, at least 1), and ascent, descent, line_gap and cap_height are scaled by round(x) / x like
       the glyphs, so ascent - descent is pixel_size x round(x) / x (the rounding moves it by up to half
       a pixel of x-height: about 10% at 10-14 px); pixel_size itself stays as asked. Only the
       x-height's top edge goes on the grid: a horizontal stroke elsewhere or of a fractional thickness
       (the bar of 'e', the cross of 't', the underside of the bar of 'z') still straddles a pixel edge,
       round tops ('o', 'n') still overshoot a little, and capitals and ascenders aren't snapped. It
       helps least with oversample_y above 1, which blurs vertically anyway. A font without an 'x'
       bakes as without the flag. */
    BTBH_GPU_FONT_FLAG_SNAP_X_HEIGHT = 1u << 1,
    /* As BTBH_GPU_FONT_FLAG_SNAP_X_HEIGHT, but the x-height is rounded down: floor(x) for round(x), so
       the face never grows taller than asked (a line that has to fit a box keeps fitting it) and loses
       up to a pixel of x-height instead (up to about 25% at 10-14 px). Where floor(x) would be 0 (an
       x-height under one pixel) the font bakes as without the flag. Set one of the two snap flags, not
       both: both fail the load with BTBH_GPU_INVALID_ARGUMENT. */
    BTBH_GPU_FONT_FLAG_SNAP_X_HEIGHT_DOWN = 1u << 2
};

/* Defined when BTBHGPUFontDesc.flags, BTBH_GPU_FONT_FLAG_R8_ATLAS (and the canvas's coverage shader),
   BTBH_GPU_FONT_FLAG_SNAP_X_HEIGHT and BTBH_GPU_FONT_FLAG_SNAP_X_HEIGHT_DOWN exist (since 0.5.0). */
#define BTBH_GPU_HAS_FONT_FLAGS 1

/* How btbh_gpu_window_font_load_ex bakes a font (RFC 0004, option A: readable small text). Bake one
   font per size it is drawn at, in output pixels (size x UI scale), and draw it at that size: text
   scaled at draw time blurs, whatever the filter. A zeroed descriptor is invalid (pixel_size 0). */
typedef struct BTBHGPUFontDesc {
    /* The size it will be drawn at, in output pixels: the line from the font's ascent to its
       descent, as btbh_gpu_window_font_load's base_size. 1 to 1024; it may be fractional. */
    float pixel_size;
    /* Horizontal oversampling: 0 or 1 is none, 2 to 8 rasterises each glyph that many times wider
       and box-filters it (stb_truetype's prefilter), so a glyph drawn at a pen position on a
       1/oversample_x pixel grid samples whole texels and stays as sharp as at a whole pixel. 3 is
       recommended for UI text. The atlas is that much wider. */
    uint8_t oversample_x;
    /* Vertical oversampling, the same way (0 or 1: none). It rarely helps: draw baselines on whole
       pixels instead. */
    uint8_t oversample_y;
    /* 1: advances (and kerning) rounded to whole pixels, so a pen that starts on a whole pixel stays
       on one (crisp, uneven spacing). 0: fractional advances and kerning (even spacing; place each
       glyph on the 1/oversample_x grid). Other values are invalid. */
    uint8_t snap_advances;
    /* BTBH_GPU_FONT_FLAG_* (0: none, as in 0.4.0); other bits are invalid. */
    uint8_t flags;
    /* Coverage contrast, applied once to the atlas while baking: coverage' = coverage^(1 /
       coverage_gamma). 0 means 1 (as rasterised); 0.25 to 4 otherwise. Values above 1 (1.2-1.4)
       thicken light-on-dark small text, which blending in gamma space thins out; below 1 thins it. */
    float coverage_gamma;
} BTBHGPUFontDesc;

/* A font's vertical metrics in pixels at its size: ascent above the baseline (positive), descent
   below it (negative), the font's line gap, and the size it was baked at. Line height is
   ascent - descent + line_gap. x_height and cap_height are the tops of 'x' and 'H' above the
   baseline, unrounded, from their outlines (0 if the font lacks the glyph): a size that puts
   x_height on a whole pixel keeps the top of lowercase sharp. */
typedef struct BTBHGPUFontMetrics {
    float ascent;
    float descent;
    float line_gap;
    float pixel_size;
    float x_height;
    float cap_height;
} BTBHGPUFontMetrics;

/* Defined when BTBHGPUFontDesc, btbh_gpu_window_font_load_ex, btbh_gpu_font_metrics and
   btbh_gpu_font_kerning exist. */
#define BTBH_GPU_HAS_FONT_LOAD_EX 1

/* One kerning pair from btbh_gpu_font_kerning_pairs: advance is btbh_gpu_font_kerning(font, left,
   right), never 0. */
typedef struct BTBHGPUFontKerningPair {
    uint32_t left;
    uint32_t right;
    float advance;
} BTBHGPUFontKerningPair;

/* Defined when BTBHGPUFontKerningPair and btbh_gpu_font_kerning_pairs exist (since 0.5.0). */
#define BTBH_GPU_HAS_FONT_KERNING_PAIRS 1

typedef enum BTBHGPUCanvasBlendMode {
    BTBH_GPU_CANVAS_BLEND_ALPHA = 0,
    BTBH_GPU_CANVAS_BLEND_ADDITIVE = 1,
    BTBH_GPU_CANVAS_BLEND_MULTIPLIED = 2,
    BTBH_GPU_CANVAS_BLEND_PREMULTIPLIED_ALPHA = 3
} BTBHGPUCanvasBlendMode;

typedef struct BTBHGPUCanvasVertex {
    float x;
    float y;
    float u;
    float v;
    float red;
    float green;
    float blue;
    float alpha;
} BTBHGPUCanvasVertex;

typedef struct BTBHGPUCanvasBatch {
    uint32_t first_vertex;
    uint32_t vertex_count;
    BTBHGPUTexture *texture;
    BTBHGPUCanvasBlendMode blend_mode;
    int32_t scissor_x;
    int32_t scissor_y;
    int32_t scissor_width;
    int32_t scissor_height;
    uint8_t scissor_enabled;
    uint8_t reserved[3];
} BTBHGPUCanvasBatch;

typedef struct BTBHGPUCanvasPass {
    uint32_t first_batch;
    uint32_t batch_count;
    BTBHGPUTexture *target;
    uint8_t clear_enabled;
    uint8_t clear_red;
    uint8_t clear_green;
    uint8_t clear_blue;
    uint8_t clear_alpha;
    uint8_t reserved[3];
} BTBHGPUCanvasPass;

typedef struct BTBHGPUCanvasReport {
    BTBHGPUResult result;
    uint32_t drawable_width;
    uint32_t drawable_height;
    uint32_t batch_count;
    uint32_t vertex_count;
    char message[256];
} BTBHGPUCanvasReport;

typedef struct BTBHGPUModelReport {
    BTBHGPUResult result;
    float minimum_x;
    float minimum_y;
    float minimum_z;
    float maximum_x;
    float maximum_y;
    float maximum_z;
    uint32_t mesh_count;
    uint32_t material_count;
    char message[256];
} BTBHGPUModelReport;

enum {
    BTBH_GPU_MODEL_IMPORT_IGNORE_MATERIAL_TEXTURES = 1u << 2,
    /* Models that load an external image (a glTF image with a uri, resolved against the model's
       directory) share one texture of it, in the window's cache, keyed by the image's normalised
       path and the texture's format: an sRGB albedo or emission texture never shares with a linear
       normal or ORM one. A composed ORM texture is shared by its pair of external images. Embedded
       (buffer-view) images stay each model's own. The key is lexical ('\' and '/' alike, "." and
       "name/.." folded); spellings that differ otherwise (letter case, links) load twice. Without
       the flag every model loads its own textures, as before. IGNORE_MATERIAL_TEXTURES wins over it.
       (since 0.4.0) */
    BTBH_GPU_MODEL_IMPORT_SHARE_EXTERNAL_TEXTURES = 1u << 3
};

/* Defined when BTBH_GPU_MODEL_IMPORT_SHARE_EXTERNAL_TEXTURES exists (since 0.4.0). */
#define BTBH_GPU_MODEL_HAS_SHARED_EXTERNAL_TEXTURES 1

/* An imported material (btbh_gpu_model_material_info). Its textures belong to the model: the
   model's destroy, or the window's, frees them, and they live as long as the model does.
   A texture shared by BTBH_GPU_MODEL_IMPORT_SHARE_EXTERNAL_TEXTURES belongs to the window's cache:
   every material that holds it keeps it alive, the last model destroyed frees it, the window's
   destroy frees what is left, and btbh_gpu_texture_destroy on it does nothing. Any other model
   texture is the model's alone, as before: the host must not destroy it (the model would free it
   again). */
typedef struct BTBHGPUModelMaterialInfo {
    BTBHGPUTexture *albedo_texture;
    uint8_t albedo_red;
    uint8_t albedo_green;
    uint8_t albedo_blue;
    uint8_t albedo_alpha;
    BTBHGPUTexture *normal_texture;
    BTBHGPUTexture *orm_texture;
    float normal_scale;
    float occlusion;
    float roughness;
    float metalness;
    BTBHGPUTexture *emission_texture;
    uint8_t emission_red;
    uint8_t emission_green;
    uint8_t emission_blue;
    uint8_t emission_alpha;
    float emission_energy;
    float alpha_cutoff;
    uint8_t unlit_enabled;
    uint8_t double_sided_enabled;
    uint8_t transparency_enabled;
    uint8_t reserved;
} BTBHGPUModelMaterialInfo;

typedef struct BTBHGPUModelMeshInfo {
    uint32_t vertex_count;
    uint32_t index_count;
} BTBHGPUModelMeshInfo;

typedef struct BTBHGPUInstanceBufferReport {
    BTBHGPUResult result;
    uint32_t capacity;
    uint32_t stride;
    uint32_t byte_capacity;
    char message[256];
} BTBHGPUInstanceBufferReport;

typedef struct BTBHGPUCustomMeshReport {
    BTBHGPUResult result;
    uint32_t vertex_count;
    uint32_t index_count;
    uint32_t vertex_byte_count;
    char message[256];
} BTBHGPUCustomMeshReport;

typedef struct BTBHGPUAnimationLibraryReport {
    BTBHGPUResult result;
    uint32_t animation_count;
    char message[256];
} BTBHGPUAnimationLibraryReport;

typedef enum BTBHGPUSceneGeometryKind {
    BTBH_GPU_SCENE_GEOMETRY_INVALID = 0,
    BTBH_GPU_SCENE_GEOMETRY_MODEL = 1,
    BTBH_GPU_SCENE_GEOMETRY_CUSTOM_MESH = 2
} BTBHGPUSceneGeometryKind;

typedef enum BTBHGPUSceneBlendMode {
    BTBH_GPU_SCENE_BLEND_ALPHA = 0,
    BTBH_GPU_SCENE_BLEND_ADDITIVE = 1,
    BTBH_GPU_SCENE_BLEND_MULTIPLIED = 2,
    BTBH_GPU_SCENE_BLEND_PREMULTIPLIED_ALPHA = 3,
    BTBH_GPU_SCENE_BLEND_COUNT = 4
} BTBHGPUSceneBlendMode;

typedef enum BTBHGPUSceneSurfaceVariant {
    BTBH_GPU_SCENE_SURFACE_NONE = 0,
    BTBH_GPU_SCENE_SURFACE_GLITTER_STATIC = 1,
    BTBH_GPU_SCENE_SURFACE_WATER = 2,
    BTBH_GPU_SCENE_SURFACE_DISSOLVE = 3,
    BTBH_GPU_SCENE_SURFACE_SHATTER = 4,
    BTBH_GPU_SCENE_SURFACE_GLITTER = 5,
    /* A see-through billboard that fades over surface_parameters0.x render units where it meets the opaque scene
     * behind it, instead of cutting a straight edge through it (in the transparent pass; elsewhere, or when the
     * scene's distance can't be drawn, it draws as NONE). Its other parameters are unused. */
    BTBH_GPU_SCENE_SURFACE_SOFT_PARTICLE = 6,
    /* A self-lit rim in a tint (parameters0: rgb tint, w rim strength; parameters1.x body glow). */
    BTBH_GPU_SCENE_SURFACE_RIM = 7,
    // The primary light's highlights on moving waves where the ORM texture's blue channel says,
    // added to the surface's own colours (also unlit ones). surface_parameters0: wave scale,
    // strength, sharpness, tilt.
    BTBH_GPU_SCENE_SURFACE_GLINTS = 8,
    // Procedural water where the ORM texture's blue channel says, instead of the texture (also
    // unlit surfaces): surface_parameters0 x = wave scale, y = highlight strength, z = sharpness;
    // surface_parameters1 rgb = the water's colour (linear), w = how much of the texture's detail
    // shows in it.
    BTBH_GPU_SCENE_SURFACE_PIXEL_WATER = 9,
    /* Screen-space refraction: drawn after every other batch, depth-tested without writing depth, it shows
     * the scene as it was before the first refracting batch, seen through a flowing displacement. Needs
     * transparency_enabled = 1 and BTBH_GPU_SCENE_BLEND_ALPHA; casts no shadow. The displacement scales with
     * the colour's alpha (vertex x instance x material: 0 shows the scene undisplaced) and the refracted
     * colour is multiplied by its rgb. surface_time animates the flow.
     * surface_parameters0: x strength (largest displacement in pixels at a 1080-pixel-high render target,
     *   scaled with the render height), y dispersion 0..1 (red and blue part along the bend), z flow scale
     *   (noise cells per world unit), w flow speed (cells per second).
     * surface_parameters1: x view coupling 0..1 (0 the pattern is fixed in the world; 1 it shifts with the
     *   view direction, so it changes as the camera moves and holds still with it), y line strength (thin
     *   chromatic lines where the bend is steepest), z normal push (the vertex normal's direction on screen
     *   times this, in pixels at 1080: a ring with outward normals makes a shockwave), w reserved (0). */
    BTBH_GPU_SCENE_SURFACE_REFRACT = 10,
    /* A see-through surface lit per pixel, for smoke, dust and steam (RFC 0013, since 0.11.0). Drawn in the
     * transparent pass with production_particle_lit.frag (its _2d_shadows or _unshadowed sibling by the frame's
     * shadows), found beside the host's fragment shader: a present without it fails, naming the file. Diffuse only.
     * Its light, each at the scale a lit surface takes it: the sky's ambient (the SH irradiance at its normal with
     * IBL, else the flat ambient); the sun, its shadow through four plain comparison taps whatever
     * directional_shadow_filter says, and the clouds; the fill and its shadow; the spot (or omni) light and its
     * shadow; the street lamps and the point lights, each skipped beyond its reach; with the radiance cascades on
     * (additive or not), their light at its world x and z from four directions; its emission, as a lit batch's.
     * A light's share is albedo * (wrap + transmission) * light, where wrap = saturate((n.l + w) / (1 + w)) and
     * the transmission (light from behind, through it) = x * exp(-thickness * z) * 4 pi * HG(y, the angle the
     * light turns to reach the eye), HG being Henyey-Greenstein's phase; only the sun and the point lights give a
     * transmission, the fill, the spot, the lamps and the cascades the wrap alone. Then fog at its own depth:
     * written so that the HDR composite's fog at the opaque depth behind it leaves the puff fogged at its own depth
     * over the background fogged at its own (exact while the fog's opacity there stays below 1).
     * Its alpha: the albedo's times the vertex's, the instance's and the material's, times the fades.
     * Its normal: normal_texture, linear, on the albedo's texture coordinate: rg a tangent-space normal (OpenGL +Y,
     * z rebuilt) whose tilt normal_scale scales (0 flat), b the thickness 0..1. The frame is the vertex's: a
     * billboard's lies in the camera's plane, so a quad in local x and y with normal +z and tangent +x (w +1) takes
     * the instance's x and y axes. It is turned toward the side seen. Without normal_texture it is flat, 1 thick.
     * surface_parameters0: x the soft fade (render units over which it fades where it meets the opaque scene behind
     *   it, as SOFT_PARTICLE's; 0 none); y, z the near fade (gone nearer the camera than y render units, whole from
     *   y + z on; z 0 none); w the wrap (0 Lambert, 1 lit all the way round to the back).
     * surface_parameters1: x the transmission (0 none); y the asymmetry, clamped to -0.99..0.99 (above 0 the light
     *   goes on forward, so a puff glows looking toward the light); z the optical depth at thickness 1 (taken as 0
     *   when below); w the density it shadows with in the particle transmittance map, a scale of its optical depth
     *   there (BTBH_GPU_SCENE_BATCH_PARTICLE_SHADOW, since 0.14.0; 0 taken as 1; reserved and refused unless 0 in
     *   0.11.0 to 0.13.0).
     * With BTBHGPUSceneLighting.particle_shadow 1 or 2 and a particle transmittance map drawn this frame (since
     * 0.14.0), the sun's share is multiplied by the map's transmittance at its flat world position, read after the
     * shadow maps (t9 in production_particle_lit.frag, t8 in _2d_shadows; the unshadowed variant never reads it).
     * Needs transparency_enabled 1 with BTBH_GPU_SCENE_BLEND_ALPHA (straight alpha), unlit_enabled 0, a static
     * geometry (animation_library and animation_pose NULL), dug_by NULL, no world_detail with a texture, no
     * world_detail_table with map_layers, no height_lift with a heightfield, detail_parameters 0 and
     * surface_parameters1[3] 0 or more: otherwise the present fails with BTBH_GPU_INVALID_ARGUMENT, naming the batch.
     * It casts no shadow (but into the particle transmittance map with BTBH_GPU_SCENE_BATCH_PARTICLE_SHADOW), writes
     * no motion and is never a source of the radiance cascades' light, with an emission or without (as if it had
     * BTBH_GPU_SCENE_BATCH_NO_RC_SOURCE). */
    BTBH_GPU_SCENE_SURFACE_LIT_PARTICLE = 11,
    BTBH_GPU_SCENE_SURFACE_COUNT = 12
} BTBHGPUSceneSurfaceVariant;

/* Defined when BTBH_GPU_SCENE_SURFACE_LIT_PARTICLE exists (RFC 0013, since 0.11.0). */
#define BTBH_GPU_SCENE_HAS_LIT_PARTICLE 1

/* How a batch takes part in the horizon-based ambient occlusion (BTBHGPUScenePost.ssao_enabled 2; the
 * occlusion of ssao_enabled 1 takes every batch fully). */
typedef enum BTBHGPUSceneSSAOMode {
    BTBH_GPU_SCENE_SSAO_FULL = 0,          /* darkened by what stands round it, and darkens what is round it */
    BTBH_GPU_SCENE_SSAO_RECEIVE_ONLY = 1,  /* darkened, but darkens nothing: flat layers drawn over other surfaces,
                                            * which would darken what lies round their edges */
    BTBH_GPU_SCENE_SSAO_CAST_ONLY = 2,     /* darkens what is round it, but isn't darkened */
    BTBH_GPU_SCENE_SSAO_NONE = 3,          /* neither: painted sprites, what lies far off */
    BTBH_GPU_SCENE_SSAO_MODE_COUNT = 4
} BTBHGPUSceneSSAOMode;

/* Defined when the renderer draws BTBH_GPU_SCENE_SURFACE_REFRACT (every device that runs the scene). */
#define BTBH_GPU_SCENE_HAS_REFRACTION 1
/* Defined when btbh_gpu_window_scene_render_to_target exists (a scene into a render target, apart
   from the window's frame and its temporal history), with BTBHGPUSceneGeometryBatch.reactive_mode
   and BTBHGPUSceneReport.upscale_mode (since 0.1.1). */
#define BTBH_GPU_SCENE_HAS_RENDER_TO_TARGET 1

/* What a transparent batch writes into FSR 3's reactive mask (BTBHGPUSceneGeometryBatch.reactive_mode),
   where the upscaler keeps less of its history. */
typedef enum BTBHGPUSceneReactiveMode {
    /* Its alpha times BTBHGPUScenePost.temporal_reactive_scale (as before). */
    BTBH_GPU_SCENE_REACTIVE_SCALED = 0,
    /* Nothing: the layer is accumulated like the opaque scene (ground detail drawn over its base). */
    BTBH_GPU_SCENE_REACTIVE_NONE = 1,
    BTBH_GPU_SCENE_REACTIVE_COUNT = 2
} BTBHGPUSceneReactiveMode;

typedef enum BTBHGPUSceneBillboardMode {
    BTBH_GPU_SCENE_BILLBOARD_DISABLED = 0,
    BTBH_GPU_SCENE_BILLBOARD_FRONT = 1,
    BTBH_GPU_SCENE_BILLBOARD_Y_AXIS = 2
} BTBHGPUSceneBillboardMode;

typedef enum BTBHGPUSceneDepthCompare {
    BTBH_GPU_SCENE_DEPTH_ALWAYS = 0,
    BTBH_GPU_SCENE_DEPTH_LESS_EQUAL = 1,
    BTBH_GPU_SCENE_DEPTH_GREATER = 2,
    BTBH_GPU_SCENE_DEPTH_NOT_EQUAL = 3,
    BTBH_GPU_SCENE_DEPTH_LESS = 4,
    BTBH_GPU_SCENE_DEPTH_EQUAL = 5,
    BTBH_GPU_SCENE_DEPTH_GREATER_EQUAL = 6,
    BTBH_GPU_SCENE_DEPTH_NEVER = 7,
    BTBH_GPU_SCENE_DEPTH_COUNT = 8
} BTBHGPUSceneDepthCompare;

typedef enum BTBHGPUSceneCullMode {
    BTBH_GPU_SCENE_CULL_NONE = 0,
    BTBH_GPU_SCENE_CULL_FRONT = 1,
    BTBH_GPU_SCENE_CULL_BACK = 2,
    BTBH_GPU_SCENE_CULL_COUNT = 3
} BTBHGPUSceneCullMode;

typedef enum BTBHGPUSceneStencilOperation {
    BTBH_GPU_SCENE_STENCIL_KEEP = 0,
    BTBH_GPU_SCENE_STENCIL_ZERO = 1,
    BTBH_GPU_SCENE_STENCIL_REPLACE = 2,
    BTBH_GPU_SCENE_STENCIL_INCREMENT_CLAMP = 3,
    BTBH_GPU_SCENE_STENCIL_DECREMENT_CLAMP = 4,
    BTBH_GPU_SCENE_STENCIL_OPERATION_COUNT = 5
} BTBHGPUSceneStencilOperation;

typedef enum BTBHGPUSceneUpscaleMode {
    BTBH_GPU_SCENE_UPSCALE_LINEAR = 0,
    BTBH_GPU_SCENE_UPSCALE_LANCZOS = 1,
    /* AMD FidelityFX Super Resolution 1: EASU upscale, then RCAS sharpening at the output size. */
    BTBH_GPU_SCENE_UPSCALE_FSR1 = 2,
    /* AMD FidelityFX Super Resolution 3.1 (temporal, no frame generation): the scene renders with a
     * sub-pixel jitter, motion vectors and a reactive mask, and the upscaler accumulates the frames at
     * the output size, then RCAS sharpens. It replaces the anti-aliasing. A device without its storage
     * formats falls back to FSR 1. */
    BTBH_GPU_SCENE_UPSCALE_FSR3 = 3
} BTBHGPUSceneUpscaleMode;

typedef enum BTBHGPUSceneTonemapMode {
    BTBH_GPU_SCENE_TONEMAP_REINHARD = 0,
    BTBH_GPU_SCENE_TONEMAP_PBR_NEUTRAL = 1,
    BTBH_GPU_SCENE_TONEMAP_ACES = 2,
    BTBH_GPU_SCENE_TONEMAP_COUNT = 3
} BTBHGPUSceneTonemapMode;

typedef enum BTBHGPUSceneRadianceCascadesTier {
    BTBH_GPU_SCENE_RC_OFF = 0,
    BTBH_GPU_SCENE_RC_LOW = 1,
    BTBH_GPU_SCENE_RC_HIGH = 2,
    BTBH_GPU_SCENE_RC_AUTO = 3
} BTBHGPUSceneRadianceCascadesTier;

typedef enum BTBHGPUSceneRadianceCascadesBackend {
    BTBH_GPU_SCENE_RC_BACKEND_HOLOGRAPHIC = 0
} BTBHGPUSceneRadianceCascadesBackend;

/* BTBHGPUSceneRadianceCascades.debug_view: the scene shows one of the cascades' buffers instead of the frame. A debug
 * view shows the cascades alone: the frame's HBAO, SSAO, reflections, bloom, fog and vignette are off, and since 0.10.0
 * the transparent and refracting batches aren't drawn over it either (before, they were, so that Be The Bullet Hell
 * v2's additive night overlays read as the cascades' light in the views). The tone mapping and grading still apply.
 * Frames without a debug view are unchanged. */
typedef enum BTBHGPUSceneRadianceCascadesDebugView {
    BTBH_GPU_SCENE_RC_DEBUG_NONE = 0,
    BTBH_GPU_SCENE_RC_DEBUG_CASCADE_0 = 1,
    BTBH_GPU_SCENE_RC_DEBUG_CASCADE_1 = 2,
    BTBH_GPU_SCENE_RC_DEBUG_CASCADE_2 = 3,
    BTBH_GPU_SCENE_RC_DEBUG_CASCADE_3 = 4,
    BTBH_GPU_SCENE_RC_DEBUG_HIT_DISTANCE = 5,
    BTBH_GPU_SCENE_RC_DEBUG_CONFIDENCE = 6,
    BTBH_GPU_SCENE_RC_DEBUG_FINAL_INDIRECT = 7,
    BTBH_GPU_SCENE_RC_DEBUG_FALLBACK_REASON = 8,
    BTBH_GPU_SCENE_RC_DEBUG_DIRECTIONAL_SOURCE = 9,
    BTBH_GPU_SCENE_RC_DEBUG_DIRECT_LIGHT = 10
} BTBHGPUSceneRadianceCascadesDebugView;

typedef enum BTBHGPUSceneRadianceCascadesFallbackReason {
    BTBH_GPU_SCENE_RC_FALLBACK_NONE = 0,
    BTBH_GPU_SCENE_RC_FALLBACK_DISABLED = 1,
    BTBH_GPU_SCENE_RC_FALLBACK_CAPABILITY_UNAVAILABLE = 2,
    BTBH_GPU_SCENE_RC_FALLBACK_SHADER_UNAVAILABLE = 3,
    BTBH_GPU_SCENE_RC_FALLBACK_PIPELINE_CREATION_FAILED = 4,
    BTBH_GPU_SCENE_RC_FALLBACK_RESOURCE_ALLOCATION_FAILED = 5,
    BTBH_GPU_SCENE_RC_FALLBACK_DISPATCH_FAILED = 6,
    BTBH_GPU_SCENE_RC_FALLBACK_INVALID_CONFIGURATION = 7,
    /* Since 0.16.0 (RFC 0014): a frame of two scene views (btbh_gpu_window_scene_present_views), or of one clipped to a
     * part of the frame, draws without the cascades. Their history starts afresh on the next frame that runs them. */
    BTBH_GPU_SCENE_RC_FALLBACK_VIEWS = 8
} BTBHGPUSceneRadianceCascadesFallbackReason;

#define BTBH_GPU_RC_MAX_OCCLUSION_CASCADES 3u

typedef enum BTBHGPUSceneRadianceEmitterShape {
    BTBH_GPU_SCENE_RADIANCE_EMITTER_DISK = 0,
    BTBH_GPU_SCENE_RADIANCE_EMITTER_RECTANGLE = 1,
    BTBH_GPU_SCENE_RADIANCE_EMITTER_SEGMENT = 2
} BTBHGPUSceneRadianceEmitterShape;

typedef enum BTBHGPUSceneRadianceEmitterClass {
    BTBH_GPU_SCENE_RADIANCE_EMITTER_CLASS_HERO = 0,
    BTBH_GPU_SCENE_RADIANCE_EMITTER_CLASS_WORLD = 1,
    BTBH_GPU_SCENE_RADIANCE_EMITTER_CLASS_PICKUP = 2,
    BTBH_GPU_SCENE_RADIANCE_EMITTER_CLASS_TRANSIENT = 3
} BTBHGPUSceneRadianceEmitterClass;

typedef struct BTBHGPUSceneRadianceEmitter {
    uint64_t stable_id;
    /* The field is drawn from above: the source's footprint lies round position_x and position_z. position_y is the
     * direct light's height (direct_energy), and with BTBHGPUSceneRadianceCascades.source_tops 1 the base of the
     * source's top (top_above). */
    float position_x;
    float position_y;
    float position_z;
    float radius;
    /* A segment's width (shape SEGMENT; kept between 0.16 and 2 times the radius) and the direct light's range. It
     * is no vertical extent: the field's sources have none, unless source_tops gives them a top (top_above). */
    float height;
    float red;
    float green;
    float blue;
    float energy;
    float direct_energy;
    float direction_x;
    float direction_y;
    float direction_z;
    float inner_cutoff;
    float outer_cutoff;
    BTBHGPUSceneRadianceEmitterShape shape;
    BTBHGPUSceneRadianceEmitterClass light_class;
    /* With BTBHGPUSceneRadianceCascades.source_tops 1: the source's top stands this far above position_y (world
     * units; finite and at least 0; 0 puts the top at position_y). A ray piece that starts higher than the top
     * passes over the source, so a large value makes the source a column that lights every height, as without
     * tops. A spot (inner_cutoff and outer_cutoff set) has its top over its footprint, at least 1.5 field texels
     * round it. Ignored, and not checked, with source_tops 0. (RFC 0010, since 0.10.0, in the four bytes that were
     * reserved.) */
    float top_above;
} BTBHGPUSceneRadianceEmitter;

typedef struct BTBHGPUSceneRadianceCascadesOcclusionCascade {
    float origin_world_x;
    float origin_world_z;
    float cell_size;
    uint32_t width;
    uint32_t height;
    uint64_t revision;
    const float *samples;
} BTBHGPUSceneRadianceCascadesOcclusionCascade;

typedef struct BTBHGPUSceneRadianceCascades {
    BTBHGPUSceneRadianceCascadesTier tier;
    BTBHGPUSceneRadianceCascadesBackend backend;
    BTBHGPUSceneRadianceCascadesDebugView debug_view;
    float auto_frame_budget_milliseconds;
    float indirect_intensity;
    uint32_t emitter_count;
    int32_t hero_omni_emitter_index;
    uint32_t occlusion_cascade_count;
    uint8_t allow_auto_transition;
    uint8_t invalidate;
    /* Only add light: no sky or sun at the field's edges, and every surface takes the bounce light
     * of what glows on top of the scene's own lighting (rather than being lit again by the
     * cascades). */
    uint8_t additive;
    /* 0: as before: every source lights every ray that crosses it, at any height. 1: sources have tops (RFC 0010,
     * since 0.10.0, in the byte that was reserved[0]).
     * - A material source's top (a batch the cascades take light from: unlit and no rc_receiver, or with an
     *   emission) is the highest world height of its glowing fragments in each field texel. An emitter's is
     *   position_y + top_above.
     * - A ray piece passes over a source whose top lies lower than the height the piece starts from, less a
     *   twentieth of a field texel (cell_size / 20): the top of the occluder it starts in
     *   (occlusion_cascades[0].samples), or 0 outside one. It takes none of the source's light then and isn't
     *   dimmed by it. Occluders stop rays as before.
     * - The cascades share ray pieces between receivers, and each piece starts from its own height, so the test
     *   shields only what lies past the pieces that start on a roof. A band along a roof's edge still takes a street
     *   glow's light, and a glow farther out reaches further in. Measured (RFC 0010's implementation notes): a lamp
     *   head half a cell out from a two-storey facade leaves 16 % of its light in the roof's interior on Be The
     *   Bullet Hell v2's High field (0 % on its Low); one a cell out leaves 78 % (75 %).
     * - Steep faces (a normal less than 45 degrees from level) look the field up half a texel out from their wall,
     *   on the street side, so a facade keeps the street's glow up its whole height, as without tops.
     * - The cascades then read the field where its texels lie. With 0 they read it half a texel toward +x and +z, as
     *   before, which sets the two directions along each axis a texel apart; with tops that made the shield depend
     *   on which way a street runs. So 1 also moves the field's light by half a texel toward -x and -z.
     * - Needs this release's radiance-cascade shaders (production_hrc_trace.comp and its siblings): with older ones
     *   the present fails with BTBH_GPU_INVALID_ARGUMENT, naming the trace shader (SPIR-V; Metal libraries aren't
     *   checked).
     * Other values, and with 1 an emitter's top_above that isn't finite or is below 0, are an invalid
     * configuration, as the struct's other bad values are: the cascades are off for that present
     * (BTBH_GPU_SCENE_RC_FALLBACK_INVALID_CONFIGURATION in the report's status) and the frame draws without them.
     * Turning it on or off changes the field's light; the temporal history follows over a few frames, or at once
     * with invalidate. */
    uint8_t source_tops;
    const BTBHGPUSceneRadianceEmitter *emitters;
    BTBHGPUSceneRadianceCascadesOcclusionCascade
        occlusion_cascades[BTBH_GPU_RC_MAX_OCCLUSION_CASCADES];
} BTBHGPUSceneRadianceCascades;

/* Defined when BTBHGPUSceneRadianceCascades.source_tops and BTBHGPUSceneRadianceEmitter.top_above exist (since
   0.10.0, RFC 0010). */
#define BTBH_GPU_SCENE_HAS_RC_SOURCE_TOPS 1

typedef struct BTBHGPUSceneRadianceCascadesPassTelemetry {
    double cpu_prepare_milliseconds;
    double cpu_pipeline_prepare_milliseconds;
    double cpu_resource_prepare_milliseconds;
    double cpu_trace_encode_milliseconds;
    double cpu_merge_up_encode_milliseconds;
    double cpu_merge_down_encode_milliseconds;
    double cpu_reproject_encode_milliseconds;
    double cpu_finish_encode_milliseconds;
    double cpu_blur_encode_milliseconds;
    double cpu_resolve_encode_milliseconds;
    double cpu_reconstruct_encode_milliseconds;
    double cpu_compose_encode_milliseconds;
    uint32_t trace_dispatch_count;
    uint32_t merge_up_dispatch_count;
    uint32_t merge_down_dispatch_count;
    uint32_t reproject_dispatch_count;
    uint32_t finish_dispatch_count;
    uint32_t blur_dispatch_count;
    uint32_t resolve_dispatch_count;
    uint32_t reconstruct_dispatch_count;
    uint32_t compose_draw_count;
    uint32_t temporal_history_valid;
    uint32_t temporal_phase;
    uint32_t fresh_quadrant_count;
    uint32_t reserved;
} BTBHGPUSceneRadianceCascadesPassTelemetry;

typedef struct BTBHGPUSceneRadianceCascadesStatus {
    BTBHGPUSceneRadianceCascadesTier requested_tier;
    BTBHGPUSceneRadianceCascadesTier active_tier;
    BTBHGPUSceneRadianceCascadesTier pending_tier;
    BTBHGPUSceneRadianceCascadesBackend requested_backend;
    BTBHGPUSceneRadianceCascadesBackend active_backend;
    BTBHGPUSceneRadianceCascadesFallbackReason fallback_reason;
    uint32_t internal_width;
    uint32_t internal_height;
    uint32_t generation;
    uint32_t sample_count;
    double cpu_encode_milliseconds;
    BTBHGPUSceneRadianceCascadesPassTelemetry pass_telemetry;
} BTBHGPUSceneRadianceCascadesStatus;

/* A second texture set for a batch (BTBHGPUSceneGeometryBatch.world_detail, RFC 0008, since 0.6.0), projected
 * from the batch's flat world position (before world_curvature bends it) instead of read through the mesh's
 * TEXCOORD_0. It multiplies the albedo, adds its normal's tilt to the first set's, and scales the occlusion and
 * the roughness; metalness and emission are untouched. The host owns it; the renderer reads it during the present
 * call only, so one detail can serve any number of batches. A batch with world_detail NULL, or one whose three
 * textures are NULL, draws exactly as before; so does a refracting batch (BTBH_GPU_SCENE_SURFACE_REFRACT), which
 * samples no material.
 *
 * The projection, by the face's own normal N (from the position's screen derivatives, turned to the side of the
 * vertex normal; to the camera's where the vertex normal lies in the face's plane):
 * - Level faces (N within level_degrees of straight up or down): u = x / level_tile[0], v = -z / level_tile[1]
 *   (north), seen from above; a face that looks down takes v = +z (it is seen from below). The tangent is +x made
 *   orthogonal to N.
 * - Other faces: T = normalize(cross(up, N)) (to the right seen from the front), B = cross(N, T) (up the face),
 *   u = dot(P, T) / wall_tile[0], v = dot(P, B) / wall_tile[1]: the height on vertical walls, the distance up
 *   the slope on pitched faces (not stretched). Where a pitched face meets a wall, v jumps.
 * - The frame is (T, cross(N, T), N). The image's top lies toward +v (as an OpenGL normal map's +Y): upright on
 *   walls, up the slope on pitched faces, to the north on level faces. The texture coordinate is (u, -v) plus the
 *   seed's offset.
 * Load the textures with BTBH_GPU_TEXTURE_WRAP_REPEAT. Static geometry only: on anything that moves (skinned
 * units, billboards, wind, a dissolve's drift) the pattern slides. */
typedef struct BTBHGPUSceneWorldDetail {
    /* Its rgb times albedo_scale multiplies the albedo; alpha unused. NULL: no albedo change (or the albedo in
     * the ORM's blue, BTBH_GPU_SCENE_WORLD_DETAIL_ALBEDO_IN_ORM_BLUE). */
    BTBHGPUTexture *albedo_texture;
    /* Tangent space in the frame above, OpenGL +Y, linear: xy are read and z is rebuilt (two channels are
     * enough). Its tilt times normal_scale is added to the first set's (as surface gradients). NULL: no tilt. */
    BTBHGPUTexture *normal_texture;
    /* Linear. r multiplies the occlusion; g times roughness_scale multiplies the roughness; b unused, or the
     * grey albedo multiplier with BTBH_GPU_SCENE_WORLD_DETAIL_ALBEDO_IN_ORM_BLUE. NULL: neither changes. */
    BTBHGPUTexture *orm_texture;
    /* World units one repeat of the textures covers, along u and v; each must be finite and above 0. */
    float level_tile[2];
    float wall_tile[2];
    float albedo_scale;        /* 0 means 1. Be The Bullet Hell v2: 1.25 (a linear mean of 0.8 to 1) */
    float normal_scale;        /* 0 means 1 */
    float roughness_scale;     /* 0 means 1. Be The Bullet Hell v2: 1.25 (its g of 0.8 keeps the roughness) */
    float level_degrees;       /* 0 means 18; below 90 */
    /* Which of the instance's four custom floats seeds an offset of the pattern: 0 none (every instance shares
     * it, anchored at the world's origin), 1..4 x..w. The seed is rounded to a whole number (exact up to 2^24);
     * different whole numbers give unrelated offsets in [0, 1) of a repeat along u and v. Give every instance of
     * a building the same seed (its lot's index). The surface variants that read custom floats (DISSOLVE x y z,
     * SHATTER y z w, GLITTER x) leave the others free. */
    uint8_t seed_component;
    uint8_t flags;             /* BTBH_GPU_SCENE_WORLD_DETAIL_* bits; 0 */
    uint8_t reserved[2];       /* 0 */
} BTBHGPUSceneWorldDetail;

/* BTBHGPUSceneWorldDetail.flags (since 0.6.0). */
enum {
    /* The grey albedo multiplier is the ORM texture's blue channel, linear like the rest of it (times
     * albedo_scale, as albedo_texture's rgb would be): the set is two textures and two samples. albedo_texture
     * must be NULL and orm_texture set. Be The Bullet Hell v2's layout. */
    BTBH_GPU_SCENE_WORLD_DETAIL_ALBEDO_IN_ORM_BLUE = 1u << 0
};

/* The most entries a world-detail table holds (BTBHGPUSceneWorldDetailTable.entry_count, since 0.8.0). */
#define BTBH_GPU_SCENE_WORLD_DETAIL_TABLE_MAX_ENTRIES 8u
/* An entry's normal_layer or orm_layer: that map is absent. Also the vertex index that means "no detail". */
#define BTBH_GPU_SCENE_WORLD_DETAIL_NO_LAYER 255u

/* One entry of a world-detail table (RFC 0009, since 0.8.0): the eight floats of BTBHGPUSceneWorldDetail (same
 * names, order and meaning, same zero values; they are that struct's bytes 24..55, so a host can copy them across),
 * then the layers of the table's array it samples. 36 bytes. */
typedef struct BTBHGPUSceneWorldDetailTableEntry {
    float level_tile[2];       /* finite and above 0 */
    float wall_tile[2];        /* finite and above 0 */
    float albedo_scale;        /* 0 means 1 */
    float normal_scale;        /* 0 means 1 */
    float roughness_scale;     /* 0 means 1 */
    float level_degrees;       /* 0 means 18; below 90 */
    /* Layers of BTBHGPUSceneWorldDetailTable.map_layers: the normal (tangent space as BTBHGPUSceneWorldDetail's,
     * OpenGL +Y, xy read and z rebuilt) and the ORM (r occlusion, g roughness factor times roughness_scale, b the
     * grey albedo multiplier times albedo_scale, linear: BTBH_GPU_SCENE_WORLD_DETAIL_ALBEDO_IN_ORM_BLUE's layout).
     * BTBH_GPU_SCENE_WORLD_DETAIL_NO_LAYER: that map is absent. A zeroed entry samples layer 0 for both. */
    uint8_t normal_layer;
    uint8_t orm_layer;
    uint8_t flags;             /* 0 */
    uint8_t reserved;          /* 0 */
} BTBHGPUSceneWorldDetailTableEntry;

/* Several world details in one batch, chosen per vertex (BTBHGPUSceneGeometryBatch.world_detail_table, RFC 0009,
 * since 0.8.0). Each vertex's index is the first float of its joint slot (BTBH_GPU_CUSTOM_MESH_DETAIL_INDEX_FLOAT),
 * rounded to the nearest whole number and clamped to 0..255 (NaN is 255): the triangle takes entries[index],
 * projected and blended as a BTBHGPUSceneWorldDetail with BTBH_GPU_SCENE_WORLD_DETAIL_ALBEDO_IN_ORM_BLUE (packed
 * only: no coloured albedo). An index at or past entry_count (BTBH_GPU_SCENE_WORLD_DETAIL_NO_LAYER, 255, by convention)
 * leaves the triangle without a detail. All three vertices of a triangle must carry the same index (which vertex
 * gives a flat value differs between graphics APIs). Meant for custom meshes and unskinned models whose joint slot
 * the host writes: a skinned batch takes entry 0 everywhere (its joints are bones), and so does a model loaded
 * without joints. Not for an SDF terrain's chunks, whose joint slots hold layer weights. A static batch with a table
 * draws with production_model_detail_table.vert from the directory of the static vertex shader the present is
 * given (a present without that file fails, naming it); the others with production_model.vert, which doesn't read
 * the index. The host owns the table and its entries; the renderer reads them during the present call only, so
 * one table can serve any number of batches. 24 bytes on 64-bit. */
typedef struct BTBHGPUSceneWorldDetailTable {
    /* A 2D array texture (btbh_gpu_window_texture_array_*), linear, made with BTBH_GPU_TEXTURE_WRAP_REPEAT, that
     * holds every entry's maps. NULL: the batch draws as without a table. */
    BTBHGPUTexture *map_layers;
    const BTBHGPUSceneWorldDetailTableEntry *entries;
    uint32_t entry_count;      /* 1..BTBH_GPU_SCENE_WORLD_DETAIL_TABLE_MAX_ENTRIES */
    uint8_t seed_component;    /* as BTBHGPUSceneWorldDetail.seed_component, for every entry */
    uint8_t flags;             /* 0 */
    uint8_t reserved[2];       /* 0 */
} BTBHGPUSceneWorldDetailTable;

/* A static batch drawn on a heightfield (BTBHGPUSceneGeometryBatch.height_lift, RFC 0012, since 0.11.0). After its
 * instance's matrix and the wind, every vertex rises by height_scale * h(u, v) + height_offset, h read bilinearly
 * from the heightfield (btbh_gpu_window_heightfield_create) at u = x * world_to_uv[0] + world_to_uv[2] and
 * v = z * world_to_uv[1] + world_to_uv[3], x and z the vertex's flat world position. Its normal and tangent tilt with
 * the heightfield's slope there, unless BTBH_GPU_SCENE_HEIGHT_LIFT_KEEP_NORMALS. The fragment shaders see the lifted
 * world position (lighting, the world detail, the procedural detail's height terms, the fog). FSR 3's previous
 * position is lifted at its own xz: a static instance on a fixed field gets no motion from the lift. The host owns
 * it; the renderer reads it during the present call only, so one lift can serve any number of batches. 40 bytes on
 * 64-bit. A lift with heightfield NULL draws as no lift (only its flags and reserved are checked then). */
typedef struct BTBHGPUSceneHeightLift {
    BTBHGPUTexture *heightfield;     /* btbh_gpu_window_heightfield_create, of the present's window */
    float world_to_uv[4];            /* u scale, v scale, u offset, v offset; finite, the scales not 0 */
    float height_scale;              /* world units per unit of h; finite; 0 means 1 */
    float height_offset;             /* world units added; finite */
    uint32_t flags;                  /* BTBH_GPU_SCENE_HEIGHT_LIFT_* bits */
    uint32_t reserved;               /* 0 */
} BTBHGPUSceneHeightLift;

/* BTBHGPUSceneHeightLift.flags (since 0.11.0). */
enum {
    /* The normals and tangents stay as the instance's matrix gives them: a lifted batch that keeps its shading (an
     * unlit overlay, a decal). Positions lift as without it. */
    BTBH_GPU_SCENE_HEIGHT_LIFT_KEEP_NORMALS = 1u << 0
};

/* The widest and tallest heightfield (btbh_gpu_window_heightfield_create, since 0.11.0); the smallest is 2 x 2. */
#define BTBH_GPU_HEIGHTFIELD_MAX_SIZE 4096u

typedef struct BTBHGPUSceneGeometryBatch {
    BTBHGPUSceneGeometryKind kind;
    BTBHGPUModel *model;
    BTBHGPUCustomMesh *custom_mesh;
    BTBHGPUAnimationLibrary *animation_library;
    BTBHGPUAnimationPose *animation_pose;
    uint32_t animation_index;
    float animation_time;
    int32_t material_slot;
    BTBHGPUInstanceBuffer *instance_buffer;
    uint32_t first_instance;
    uint32_t instance_count;
    BTBHGPUTexture *albedo_texture;
    uint8_t albedo_red;
    uint8_t albedo_green;
    uint8_t albedo_blue;
    uint8_t albedo_alpha;
    float uv_scale_x;
    float uv_scale_y;
    float uv_offset_x;
    float uv_offset_y;
    BTBHGPUTexture *normal_texture;
    BTBHGPUTexture *orm_texture;
    float normal_scale;
    float occlusion;
    float roughness;
    float metalness;
    BTBHGPUTexture *emission_texture;
    uint8_t emission_red;
    uint8_t emission_green;
    uint8_t emission_blue;
    uint8_t emission_alpha;
    float emission_energy;
    BTBHGPUSceneBlendMode blend_mode;
    BTBHGPUSceneBillboardMode billboard_mode;
    BTBHGPUSceneDepthCompare depth_compare;
    float alpha_cutoff;
    BTBHGPUSceneDepthCompare stencil_compare;
    BTBHGPUSceneCullMode cull_mode;
    BTBHGPUSceneStencilOperation stencil_fail;
    BTBHGPUSceneStencilOperation stencil_depth_fail;
    BTBHGPUSceneStencilOperation stencil_pass;
    uint8_t stencil_reference;
    uint8_t stencil_mask;
    uint8_t transparency_enabled;
    uint8_t unlit_enabled;  // 1: unlit; 2: unlit, darkened in the primary directional shadow; 3: as 2, and with
                            // the soft sun shadow filter through four plain comparison taps (no soft penumbra:
                            // see-through layers drawn many times)
    uint8_t spot_shadow_visible;
    /* An unlit surface that takes the radiance cascades' bounce light (added to its colour) and is no
     * source of it (unlit surfaces are, unless this is set). A lit batch that should be no source sets
     * BTBH_GPU_SCENE_BATCH_NO_RC_SOURCE instead. */
    uint8_t rc_receiver;
    /* BTBHGPUSceneReactiveMode: how a transparent batch marks FSR 3's reactive mask; 0 as before. */
    uint8_t reactive_mode;
    BTBHGPUSceneSurfaceVariant surface_variant;
    float surface_time;
    float surface_parameters0[4];
    float surface_parameters1[4];
    /* Procedural surface detail: x albedo variation, y bump height (render units), z noise cells per render
     * unit, w the pattern (BTBH_GPU_SCENE_DETAIL_*). Patterns 1..6 need z above 0; pattern 7 needs x, y and z at
     * 0. A zeroed batch has none. */
    float detail_parameters[4];
    /* How it takes part in the horizon-based ambient occlusion (ssao_enabled 2): BTBHGPUSceneSSAOMode (0:
     * fully). Procedural pixel water is never darkened by it where it is water. */
    uint8_t ssao_mode;
    /* BTBH_GPU_SCENE_BATCH_* bits (since 0.5.0, in a byte that was ssao_reserved[0]). 0 draws as before; a bit the renderer doesn't know fails the present with
     * BTBH_GPU_INVALID_ARGUMENT. */
    uint8_t flags;
    /* BTBH_GPU_SCENE_BATCH_FLAGS2_* bits, the batch flags' second byte (since 0.12.0: byte 254 on 64-bit, right after
     * flags, which was ssao_reserved[0], so the struct keeps its 288 bytes; it was in the run key already, and still
     * is). Its bits are its own: they don't go in flags. 0 draws as before; a bit the renderer doesn't know fails the
     * present with BTBH_GPU_INVALID_ARGUMENT (0.11.0 and older ignore this byte). */
    uint8_t flags2;
    uint8_t ssao_reserved[1];
    /* NULL: drawn as before. Otherwise the batch is ground that this terrain may dig through
     * (btbh_gpu_sdf_terrain.h, since 0.5.0): where the terrain's patch has taken over from the base (a crater,
     * a mound's foot, paint), its fragments that don't lie more than a tenth of a cell under the patch's top
     * surface at their flat world XZ are discarded, in the colour, depth, shadow and radiance-cascade passes.
     * Until the terrain has a dig mask (it is attached to this window and a patch exists), the batch draws
     * exactly as with NULL. */
    const BTBHGPUSdfTerrain *dug_by;
    /* NULL: drawn as before. Otherwise the batch's second texture set, projected from the world position
     * (BTBHGPUSceneWorldDetail, RFC 0008, since 0.6.0). Several batches may point at one. A detail that isn't
     * valid fails the present with BTBH_GPU_INVALID_ARGUMENT, naming the batch. A batch whose detail has a
     * texture leaves the fast unlit pipeline, and needs the scene shaders from 0.6.0: with SPIR-V shaders that
     * don't sample it, the present fails with the shader's path. */
    const BTBHGPUSceneWorldDetail *world_detail;
    /* NULL: drawn as before. Otherwise world details chosen per vertex from a table over one array texture
     * (BTBHGPUSceneWorldDetailTable, RFC 0009, since 0.8.0). Several batches may point at one. A table that isn't
     * valid fails the present with BTBH_GPU_INVALID_ARGUMENT, naming the batch; so does a batch with both a table
     * whose map_layers is set and a world_detail with a texture (a world_detail without textures may stay). A
     * refracting batch ignores it, as it does world_detail. A batch whose table has map_layers leaves the fast
     * unlit pipeline, and needs the scene shaders from 0.8.0: with SPIR-V shaders that don't sample it, the
     * present fails with the shader's path. */
    const BTBHGPUSceneWorldDetailTable *world_detail_table;
    /* NULL: drawn as before. Otherwise the batch's vertices are lifted by a heightfield (BTBHGPUSceneHeightLift,
     * RFC 0012, since 0.11.0), in every pass that draws it: opaque, transparent, unlit (the fast unlit pipeline too),
     * additive, soft particles and refraction. Several batches may point at one. A lift that isn't valid fails the
     * present with BTBH_GPU_INVALID_ARGUMENT, naming the batch; so does, with its heightfield set, a batch that is
     * animated (animation_library or animation_pose set), a billboard, one with dug_by, or one that would cast a
     * shadow: in 0.11.0 a lifted batch casts none, so it must be transparent (transparency_enabled), or set
     * BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW (or have its directional_shadow_visibility byte 0, and its byte 0 in every
     * scene view's shadow_visibility: BTBHGPUSceneView, since 0.16.0) and leave spot_shadow_visible 0. The radiance
     * cascades' material sources take it unlifted (they project straight down).
     * A lifted batch draws with production_model_lift.vert (production_model_detail_table_lift.vert with a
     * world-detail table) from the directory of the static vertex shader the present is given; a present without
     * that file fails, naming it. 288 bytes on 64-bit with it (280 in 0.8.0 to 0.10.0). */
    const BTBHGPUSceneHeightLift *height_lift;
} BTBHGPUSceneGeometryBatch;

/* BTBHGPUSceneGeometryBatch.flags (since 0.5.0). */
enum {
    /* The batch is drawn, and shadowed, as usual, but casts into neither directional shadow map (the primary
     * one, the sun's, nor the secondary one): for decals, ground overlays and small props whose shadow isn't
     * worth the shadow pass. Every scene call takes it; with the _with_visibility calls a batch casts only
     * where its directional_shadow_visibility byte lets it too (that array did this before, through those
     * calls only). The spot (or omni) light's shadow is left to spot_shadow_visible, as before: 0 casts
     * none there, so a batch that should cast no shadow at all sets this and leaves spot_shadow_visible 0.
     * A transparent batch casts no shadow either way. Where a shadow map is kept over several frames (an
     * update period above 1), a change of the flag shows at the map's next update. */
    BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW = 1u << 0,
    /* With BTBHGPUScenePost.opaque_sort FRONT_TO_BACK (or FRONT_TO_BACK_LESS_EQUAL): the batch keeps its
     * place in the opaque pass and no other batch is moved across it, for an order that matters in a way the
     * renderer can't see (a coplanar overlay drawn over what it lies on with depth LESS, or with LESS_EQUAL
     * under FRONT_TO_BACK_LESS_EQUAL; the ground deliberately left after the units, so that the units before
     * it are sorted among themselves and it still draws last). Without the sort it changes nothing. */
    BTBH_GPU_SCENE_BATCH_KEEP_ORDER = 1u << 1,
    /* With the radiance cascades on, the batch is no source of their light (since 0.9.0, asked for by Be The
     * Bullet Hell v2): it isn't drawn into their source field, so its emission (and, unlit, its colour) lights
     * nothing round it through them. Everything else stays: it is drawn, glows on screen with its own emission,
     * is lit, takes the cascades' bounce light (lit, or unlit with rc_receiver) and casts its shadows as before.
     * For small glows that add up to an even wash (a city's lit windows and signs at night) while the game's own
     * emitters and glowing batches keep lighting the streets. The cascades' occlusion is the host's
     * (BTBHGPUSceneRadianceCascades.occlusion_cascades), which no batch draws into, so the flag leaves it alone:
     * the batch blocks the cascades' light where that occlusion says so, with or without it. Without the
     * cascades, or on a batch that doesn't glow, it changes nothing. */
    BTBH_GPU_SCENE_BATCH_NO_RC_SOURCE = 1u << 2,
    /* The instance's four custom floats are a rectangle of its textures (RFC 0013, since 0.11.0, asked for by Be The
     * Bullet Hell v2): the texture coordinate, after uv_scale and uv_offset, is multiplied by custom x and y and
     * offset by z and w, so one batch draws every cell of an atlas, a cell per instance. Every static vertex shader
     * takes it (production_model.vert and the table's and the lift's), so it works in every pass that draws the
     * batch: opaque or transparent, lit, unlit (the fast unlit shader too), soft and lit particles, lifted, and the
     * radiance cascades' source pass. Shadows sample no texture and don't change. Static batches only, and not
     * with the surface variants that read the custom floats (DISSOLVE, SHATTER, GLITTER) nor a world detail or table
     * seeded by one (seed_component not 0), nor with BTBH_GPU_SCENE_BATCH_INSTANCE_LIGHT_FACTOR: the present fails
     * with BTBH_GPU_INVALID_ARGUMENT, naming the batch. It needs this release's static vertex shaders: with SPIR-V
     * ones that don't read it, the present fails with the shader's path. */
    BTBH_GPU_SCENE_BATCH_INSTANCE_UV_RECT = 1u << 3,
    /* The batch shadows the sun's light in the particle transmittance map (RFC 0013 phase 2, since 0.14.0, asked for
     * by Be The Bullet Hell v2), on frames where BTBHGPUSceneLighting.particle_shadow is set and the sun's shadow is
     * on (directional_shadow_enabled); otherwise the flag does nothing. Lit particles then darken where other puffs
     * stand between them and the sun (a smoke column's lee side), and with particle_shadow 2 so does the ground.
     * - The map is drawn in sun space, with the sun map's matrix (the one the receivers read, kept over frames as the
     *   sun map is), every frame, after the shadow maps. A billboard faces the sun there (FRONT: its quad in the
     *   matrix's x and y; Y_AXIS: turned round y toward the sun), so a puff shadows with its whole disc; other
     *   batches are drawn as they stand. The camera's fades (soft and near) don't apply.
     * - A fragment's alpha (the albedo's, the vertex's, the instance's and the material's, with the UV rect when the
     *   batch has it) becomes optical depth -ln(1 - min(alpha, 0.999)), times surface_parameters1[3] for a
     *   LIT_PARTICLE (its density, 0 meaning 1); a fragment at alpha 1/255 or less, or below alpha_cutoff, adds
     *   nothing. Each texel keeps one slab: the sum of the optical depths, their mean depth along the sun and the
     *   nearest depth. A receiver before the slab takes all the light, one past it exp(-sum), one inside it the
     *   share of the sum between the front and its depth (the coverage taken as even from the front to twice the
     *   mean). Puffs stacked along a ray count as one column.
     * - The batch casts no other shadow (transparent batches cast none), and every pass draws it as before.
     * - It takes its place in the batch's directional_shadow_visibility byte with the _with_visibility calls (0: it
     *   isn't drawn into the map); BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW doesn't keep it out.
     * - Only on a static (animation_library and animation_pose NULL), unlifted (no height_lift with a heightfield),
     *   undug (dug_by NULL) batch drawn in the transparent pass (transparency_enabled, or a blend other than ALPHA) of
     *   surface variant NONE, SOFT_PARTICLE or LIT_PARTICLE: the present fails with BTBH_GPU_INVALID_ARGUMENT, naming
     *   the batch, on any other. The map's shaders are production_particle_transmittance.vert and .frag, beside the
     *   host's static vertex shader (production_model.vert); a present that draws the map without them fails, naming
     *   the file. */
    BTBH_GPU_SCENE_BATCH_PARTICLE_SHADOW = 1u << 4,
    /* The batch receives no spot (or omni) shadow (since 0.10.0, asked
     * for by Be The Bullet Hell v2: its kaiju, the only thing in its spot map, was darkened by its own map): the
     * spot light lights it unshadowed, and spot_occluder_shadow_strength takes none of its light. What it casts is
     * left to spot_shadow_visible, as before; the directional shadows are as before. Needs this release's scene
     * shaders (production_model*.frag); older ones ignore it. */
    BTBH_GPU_SCENE_BATCH_RECEIVES_NO_SPOT_SHADOW = 1u << 5,
    /* The instance's custom x is its light (since 0.11.0, asked for by Be The Bullet Hell v2 for its district
     * blackouts): clamped to 0..1 (1 powered, 0 dark; a NaN is 0), it multiplies on that instance alone
     * - the material's emission (emission_texture times the emission colour times emission_energy),
     * - the glass detail's share of panes lit at night (pattern 5's detail_parameters[1]),
     * - the lamp lens detail's glow (pattern 6),
     * in every pass that draws them (the fast unlit shader too) and in the radiance cascades' source field. Nothing
     * else changes: the albedo (an unlit batch's colour, and its share of the cascades' sources), the glass's panes,
     * tint and reflection, the shading, the surface variants' own glows. At 1 the instance draws byte for byte as
     * without the flag; at 0 as the same batch with emission_energy 0, and detail_parameters[1] 0 on glass or
     * detail_parameters[3] 0 on a lamp lens. Between them the emission and the lens's glow scale with it and the
     * glass keeps that share of its lit panes, each at its full light: as it falls they go out one by one. Read per
     * pixel and taken to the nearest 1/4096 (the value interpolated across a triangle may come a rounding off), so 0,
     * 1 and every k/4096 are exact. Skinned batches take it too. Not with INSTANCE_UV_RECT, DISSOLVE or GLITTER (they read custom x), a world detail or
     * table seeded by custom x (seed_component 1; 2 to 4 are free), a lit particle (its shader doesn't take it) nor a
     * refracting batch (it draws no material): the present fails with BTBH_GPU_INVALID_ARGUMENT, naming the batch. It
     * reaches the shaders as a bit of a uniform word and adds no pipeline. Needs this release's scene shaders
     * (production_model*.frag, production_model_unlit.frag and production_rc_material_source.frag); older ones ignore
     * it, and the instance draws powered. */
    BTBH_GPU_SCENE_BATCH_INSTANCE_LIGHT_FACTOR = 1u << 6
};

/* Defined when BTBH_GPU_SCENE_BATCH_RECEIVES_NO_SPOT_SHADOW exists (since 0.10.0). */
#define BTBH_GPU_SCENE_HAS_BATCH_RECEIVES_NO_SPOT_SHADOW 1
/* Defined when BTBH_GPU_SCENE_BATCH_INSTANCE_UV_RECT exists (RFC 0013, since 0.11.0). */
#define BTBH_GPU_SCENE_HAS_INSTANCE_UV_RECT 1
/* Defined when BTBH_GPU_SCENE_BATCH_PARTICLE_SHADOW, BTBHGPUSceneLighting.particle_shadow and the
   BTBH_GPU_SCENE_PARTICLE_SHADOW_* values exist (RFC 0013 phase 2, since 0.14.0). */
#define BTBH_GPU_SCENE_HAS_PARTICLE_SHADOW 1
/* Defined when BTBH_GPU_SCENE_BATCH_INSTANCE_LIGHT_FACTOR exists (since 0.11.0). */
#define BTBH_GPU_SCENE_HAS_INSTANCE_LIGHT_FACTOR 1

/* BTBHGPUSceneGeometryBatch.flags2, the batch flags' second byte (since 0.12.0). */
enum {
    /* The batch keeps the radiance cascades' look under BTBH_GPU_SCENE_POST_ADDITIVE_CASCADES_KEEP_HBAO (since 0.12.0,
     * asked for by Be The Bullet Hell v2 for its weapon beams and glow sources, while its light overlays draw as
     * without the cascades). That flag draws a transparent unlit batch under additive cascades with the fast unlit
     * shader, as without the cascades, which adds no point light; with this bit the batch draws as it does under the
     * cascades without that flag: with the cascades' scene shader (production_model_rc*.frag), which adds the point
     * lights (eased by point_light_ceiling), the dark scene's darkening, spot and glow (unlit_darkness), and unlit 2
     * and 3's sun shadow. Byte for byte as under the cascades without KEEP_HBAO, over whatever lies behind it.
     * - It matters only for a batch the fast unlit shader would draw (unlit, drawn in the transparent pass, with no
     *   surface variant, no world detail texture and no table with map_layers), under additive cascades with
     *   KEEP_HBAO, outside the cascades' debug views. Everywhere else it changes nothing: without the cascades (the
     *   fast unlit shader, as before), under cascades that aren't additive or without KEEP_HBAO (the scene shader
     *   already), and on opaque or lit batches.
     * - The batch takes the pipeline it takes under the cascades without KEEP_HBAO: no new one, and
     *   btbh_gpu_window_scene_prewarm makes it for a batch with the bit. Needs no new shader. */
    BTBH_GPU_SCENE_BATCH_FLAGS2_KEEP_CASCADES_LOOK = 1u << 0
};

/* Defined when BTBHGPUSceneGeometryBatch.flags2 and BTBH_GPU_SCENE_BATCH_FLAGS2_KEEP_CASCADES_LOOK exist (since
   0.12.0). */
#define BTBH_GPU_SCENE_HAS_BATCH_KEEP_CASCADES_LOOK 1

/* BTBHGPUSceneGeometryBatch.detail_parameters[3], the procedural surface detail's pattern (rounded to the nearest
 * whole number). Values 0..6 are as before; their names and value 7 are new in 0.7.0. */
enum {
    BTBH_GPU_SCENE_DETAIL_NONE = 0,
    BTBH_GPU_SCENE_DETAIL_GROUND = 1,      /* grain, stains, wet puddles, rain rings */
    BTBH_GPU_SCENE_DETAIL_GRASS = 2,       /* grain, stains, a warm-to-cool drift */
    BTBH_GPU_SCENE_DETAIL_FACADE = 3,      /* triplanar grain, stains, street darkening, wet walls */
    BTBH_GPU_SCENE_DETAIL_PAVING = 4,      /* slabs, joints, grain, wet puddles, rain rings */
    BTBH_GPU_SCENE_DETAIL_GLASS = 5,       /* panes; y the share lit at night, z panes per render unit */
    BTBH_GPU_SCENE_DETAIL_LAMP_LENS = 6,   /* its colour shines with BTBHGPUSceneLighting.window_glow */
    /* Pattern 3's weather without its grain (RFC 0009): every face darkens toward the street (0.8 at y = 0 to 1
     * at 0.25 render units, the flat world height) and, with BTBHGPUSceneLighting.surface_wetness, steep faces
     * turn darker and semi-gloss. No noise, no stains, no bump. x, y and z mean nothing yet and must be 0
     * (detail_parameters = { 0, 0, 0, 7 }); with any of them set the present fails with
     * BTBH_GPU_INVALID_ARGUMENT. It draws byte for byte as pattern 3 with x and y at 0 (whatever its z), without
     * the noise's cost. Be The Bullet Hell v2's facades where the world detail replaces the grain, and on its Low
     * and Deck presets. */
    BTBH_GPU_SCENE_DETAIL_FACADE_WEATHER = 7
};

/* Defined when BTBHGPUSceneGeometryBatch.flags and BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW exist (since 0.5.0). */
#define BTBH_GPU_SCENE_HAS_BATCH_FLAGS 1

/* Defined when BTBH_GPU_SCENE_BATCH_NO_RC_SOURCE exists (since 0.9.0). */
#define BTBH_GPU_SCENE_HAS_BATCH_NO_RC_SOURCE 1

/* Defined when BTBHGPUSceneGeometryBatch.world_detail, BTBHGPUSceneWorldDetail and
   BTBH_GPU_SCENE_WORLD_DETAIL_ALBEDO_IN_ORM_BLUE exist (since 0.6.0). */
#define BTBH_GPU_SCENE_HAS_WORLD_DETAIL 1

/* Defined when BTBH_GPU_SCENE_DETAIL_FACADE_WEATHER (pattern 7) and the BTBH_GPU_SCENE_DETAIL_* names exist (since
   0.7.0). */
#define BTBH_GPU_SCENE_HAS_FACADE_WEATHER 1

/* Defined when BTBHGPUSceneGeometryBatch.world_detail_table, BTBHGPUSceneWorldDetailTable,
   BTBHGPUSceneWorldDetailTableEntry and the BTBH_GPU_CUSTOM_MESH_* vertex macros exist (since 0.8.0). */
#define BTBH_GPU_SCENE_HAS_WORLD_DETAIL_TABLE 1
/* Defined when btbh_gpu_window_heightfield_create, BTBHGPUSceneHeightLift and BTBHGPUSceneGeometryBatch.height_lift
   exist (since 0.11.0). */
#define BTBH_GPU_SCENE_HAS_HEIGHT_LIFT 1

#define BTBH_GPU_SCENE_POINT_LIGHTS 32

/* How the primary directional (sun) shadow is filtered (BTBHGPUSceneLighting.directional_shadow_filter). */
typedef enum BTBHGPUSceneShadowFilter {
    /* A tent over the map's texels, one texel wider than directional_shadow_softness (up to three). */
    BTBH_GPU_SCENE_SHADOW_FILTER_TENT = 0,
    /* Percentage-closer soft shadows: a blocker search finds how far above a point its blockers stand,
     * the penumbra widens by 0.035 world units per unit of that (times directional_shadow_softness, at
     * least 0.75 texel) and is filtered with 4 to 32 comparison taps against the receiver's own plane. */
    BTBH_GPU_SCENE_SHADOW_FILTER_SOFT = 1,
    BTBH_GPU_SCENE_SHADOW_FILTER_COUNT = 2
} BTBHGPUSceneShadowFilter;

typedef struct BTBHGPUSceneLighting {
    float ambient_red;
    float ambient_green;
    float ambient_blue;
    float ambient_energy;
    float directional_x;
    float directional_y;
    float directional_z;
    float directional_energy;
    float directional_red;
    float directional_green;
    float directional_blue;
    float secondary_directional_energy;
    float secondary_directional_x;
    float secondary_directional_y;
    float secondary_directional_z;
    float reserved;
    float secondary_directional_red;
    float secondary_directional_green;
    float secondary_directional_blue;
    float secondary_reserved;
    float spot_position_x;
    float spot_position_y;
    float spot_position_z;
    float spot_range;
    float spot_direction_x;
    float spot_direction_y;
    float spot_direction_z;
    float spot_energy;
    float spot_red;
    float spot_green;
    float spot_blue;
    float spot_falloff;
    float spot_inner_cutoff;
    float spot_outer_cutoff;
    /* The share of the sun, fill and sky light the spot's shadow also takes where its occluder lies
       within spot_occluder_shadow_reach of the light (0 = the classic spot shadow). The reach is read through a
       perspective spot matrix (a near plane of 0.05, spot_range far), as the distance from the light: the darkening
       eases out from 0.4 to 1 times it. spot_occluder_flags (since 0.10.0) can take only the sun's light instead,
       and read the reach through an orthographic matrix. */
    float spot_occluder_shadow_strength;
    float spot_occluder_shadow_reach;
    float irradiance_sh[27];
    uint32_t ibl_enabled;
    float ground_bottom_red;
    float ground_bottom_green;
    float ground_bottom_blue;
    float ground_energy;
    float ground_horizon_red;
    float ground_horizon_green;
    float ground_horizon_blue;
    float ground_horizon_curve;
    float sky_top_red;
    float sky_top_green;
    float sky_top_blue;
    float sky_energy;
    float sky_horizon_red;
    float sky_horizon_green;
    float sky_horizon_blue;
    float sky_horizon_curve;
    float sun_red;
    float sun_green;
    float sun_blue;
    float sun_energy;
    float sun_direction_x;
    float sun_direction_y;
    float sun_direction_z;
    float sun_size_radians;
    float sun_curve;
    /* 0 dry ... 1 soaked: the ground and paving surface details (patterns 1 and 4) turn glossy and a
       shade darker, in puddles more than between them (the game's city at night); the facade's (patterns 3 and
       7) steep faces turn a shade darker and semi-gloss. */
    float surface_wetness;
    /* The night's lit windows (the glass surface detail, pattern 5): their light's energy, 0 by day. */
    float window_glow;
    float ibl_reserved[1];
    float directional_shadow_view_projection[16];
    float secondary_directional_shadow_view_projection[16];
    float spot_shadow_view_projection[16];
    float omni_shadow_view_projection[6][16];
    uint32_t directional_shadow_enabled;
    float directional_shadow_softness;
    uint32_t secondary_directional_shadow_enabled;
    float secondary_directional_shadow_softness;
    uint32_t spot_shadow_enabled;
    float spot_shadow_softness;
    uint32_t spot_shadow_is_omni;
    float spot_shadow_near;
    /* Street lamps: up to BTBH_GPU_SCENE_MAX_LAMPS small point lights every lit pixel weighs through the scene's
       BRDF with its own normal, so wet, bumpy ground glints under them (the game's city at night).
       lamp_position_reach: xyz the lamp, w the distance its light ends at (render units); lamp_color_energy:
       linear rgb and the energy; within lamp_core_radius a lamp's light stays near full. */
    uint32_t lamp_count;
    float lamp_core_radius;
    float lamp_position_reach[BTBH_GPU_SCENE_MAX_LAMPS][4];
    float lamp_color_energy[BTBH_GPU_SCENE_MAX_LAMPS][4];
    /* Cloud shadows on the sun (production_model.frag.hlsl btbh_cloud_light): the share of the sun a cloud's
       shadow takes (0 none), the sky's share under clouds, their edges' width, noise cells per render unit, the
       layer's height (render units) and its drift so far (x, z render units). */
    float cloud_strength;
    float cloud_coverage;
    float cloud_softness;
    float cloud_frequency;
    float cloud_height;
    float cloud_offset[2];
    /* 0 dry ... 1 a downpour: rings where drops land on wet level ground (production_model.frag.hlsl
       btbh_rain_ripples). */
    float rain;
    // Small point lights without shadows (explosions' flashes): per light x, y, z, radius, red,
    // green, blue, energy. They light lit surfaces (diffuse) and brighten opaque unlit ones.
    // Transparent unlit batches take them only under the radiance cascades, unless they are additive
    // and BTBH_GPU_SCENE_POST_ADDITIVE_CASCADES_KEEP_HBAO is set. Without the cascades, or with that
    // flag under additive ones, those batches draw with the fast unlit shader, which adds no point light
    // (but those with BTBH_GPU_SCENE_BATCH_FLAGS2_KEEP_CASCADES_LOOK, since 0.12.0, which take them there).
    uint32_t point_light_count;
    float point_lights[BTBH_GPU_SCENE_POINT_LIGHTS][8];
    /* A dark scene: unlit surfaces keep 1 - unlit_darkness of their colour (0: all of it, as
     * usual), the spot light lights them (with its shadow), and their pixels brighter than
     * unlit_glow_threshold (luminance 0-1) glow on regardless. */
    float unlit_darkness;
    float unlit_glow_threshold;
    /* The point lights' ceiling (luminance, finite; 0: none, as before): the light they add to a surface
     * eases out towards the room the surface has left under it (at least a fifth of it), so dark
     * ground still flashes bright while pale ground under many flashes doesn't burn white. */
    float point_light_ceiling;
    /* How the primary directional (sun) shadow is filtered: BTBHGPUSceneShadowFilter (0: the tent
     * as before). */
    uint8_t directional_shadow_filter;
    /* The particle transmittance map (RFC 0013 phase 2, since 0.14.0, byte 3,337 on 64-bit, which was
     * directional_shadow_reserved[0], so the struct keeps its 3,340 bytes): BTBH_GPU_SCENE_PARTICLE_SHADOW_* values.
     * Bits 0-1 who reads it: 0 nobody, as before (the map isn't drawn); 1 the lit particles (the sun's share of their
     * light, BTBH_GPU_SCENE_SURFACE_LIT_PARTICLE); 2 those and the opaque surfaces the sun shadows (lit, and unlit
     * modes 2 and 3: the visibility of the sun's shadow, times the map's). Bit 2 (4) the map at a quarter of the sun
     * map's size each way instead of half (BTBH_GPU_SCENE_PARTICLE_SHADOW_QUARTER; nothing with bits 0-1 at 0). The
     * map is drawn only on frames whose sun shadow is on and that have a batch with
     * BTBH_GPU_SCENE_BATCH_PARTICLE_SHADOW: one RGBA16F texture, 8 MiB at half under the default 2048 sun map (2 at a
     * quarter), made with its pipeline by the first frame that draws it (or by btbh_gpu_window_scene_prewarm and the
     * render target's prewarm with the frame's settings), and made anew when its size changes. Bits 0-1 at 3, or a
     * bit above 2, fail the present with BTBH_GPU_INVALID_ARGUMENT. The readers need this
     * release's shaders: with SPIR-V ones that don't read the map (production_particle_lit*.frag, and for 2 the scene
     * fragment shader the frame draws with), a present that draws it fails with the shader's path. */
    uint8_t particle_shadow;
    /* BTBH_GPU_SCENE_SPOT_OCCLUDER_* bits (since 0.10.0, byte 3,338, which was directional_shadow_reserved[1], so
     * the struct keeps its 3,340 bytes): how spot_occluder_shadow_strength darkens. 0 as before; a bit the renderer
     * doesn't know fails the present with BTBH_GPU_INVALID_ARGUMENT. */
    uint8_t spot_occluder_flags;
    uint8_t spot_occluder_reserved[1];
} BTBHGPUSceneLighting;

/* BTBHGPUSceneLighting.spot_occluder_flags (since 0.10.0, asked for by Be The Bullet Hell v2, which draws only its
 * kaiju into the spot map, fitted along the sun round its body with an orthographic matrix, for a crisp kaiju
 * shadow). They need this release's scene shaders (production_model*.frag); older ones ignore them. */
enum {
    /* The occluder's darkening takes only the primary directional light (the sun, with its own shadow), not the
     * sun, fill (the secondary directional light) and sky (the ambient light) together: the spot map then acts as
     * a sharper sun shadow of what it holds, and that can leave the sun's map (taking the fill and sky too, a shadow
     * at dusk came out brown). The spot light's own shadow is as before. Radiance cascades that aren't additive
     * light the surface with their own field, which carries the sun and the sky together, so their light is still
     * darkened as before (else the field would fill the shadow in); additive ones add bounce light that the
     * occluder takes either way. */
    BTBH_GPU_SCENE_SPOT_OCCLUDER_SUN_ONLY = 1u << 0,
    /* The spot matrix (spot_shadow_view_projection) is orthographic: a directional light's map. Its depth is read
     * as linear from its near plane, so spot_occluder_shadow_reach is the occluder's distance from that plane, in
     * world units along the light (the darkening eases out from 0.4 to 1 times it; a reach of at least 2.5 times the
     * map's depth range, far less near, never eases). The spot shadow's slope bias takes the light's direction
     * (spot_direction) instead of the direction to spot_position. Without it such a matrix's depth is read as perspective, and only a very large
     * reach kept the easing off. Ignored for an omni shadow. */
    BTBH_GPU_SCENE_SPOT_OCCLUDER_ORTHOGRAPHIC = 1u << 1
};

/* Defined when BTBHGPUSceneLighting.spot_occluder_flags and its two bits exist (since 0.10.0). */
#define BTBH_GPU_SCENE_HAS_SPOT_OCCLUDER_FLAGS 1

/* BTBHGPUSceneLighting.particle_shadow (RFC 0013 phase 2, since 0.14.0, asked for by Be The Bullet Hell v2): who reads
 * the particle transmittance map that batches with BTBH_GPU_SCENE_BATCH_PARTICLE_SHADOW draw, and its size. */
enum {
    /* Nobody: the map isn't drawn (0, as before). */
    BTBH_GPU_SCENE_PARTICLE_SHADOW_OFF = 0,
    /* The lit particles: their sun's share times the map's transmittance (a smoke column's lee side darkens). */
    BTBH_GPU_SCENE_PARTICLE_SHADOW_PARTICLES = 1,
    /* The lit particles and the opaque surfaces the sun shadows (the column's shadow on the street). It takes the scene
       fragment shader's last free texture slot (production_model.frag and its _2d_shadows and _rc siblings). */
    BTBH_GPU_SCENE_PARTICLE_SHADOW_GROUND = 2,
    /* Bits 0-1: one of the three above. */
    BTBH_GPU_SCENE_PARTICLE_SHADOW_READERS = 3,
    /* Or'd in: the map at a quarter of the sun map's size each way (a sixteenth of its texels) instead of half (a
       quarter of them). Half is the default; a quarter suits a 1024 sun map (Be The Bullet Hell v2's Deck). */
    BTBH_GPU_SCENE_PARTICLE_SHADOW_QUARTER = 4
};

/* The order the opaque pass draws its batches in (BTBHGPUScenePost.opaque_sort, since 0.5.0). */
typedef enum BTBHGPUSceneOpaqueSort {
    /* The order given (as before 0.5.0). */
    BTBH_GPU_SCENE_OPAQUE_SORT_SUBMITTED = 0,
    /* Nearest first, so the depth test throws hidden pixels away before they are shaded. The key is the
     * view depth (clip w for a perspective view-projection, clip z for an orthographic one) of the batch's
     * centre: the mean, over its instances, of the instance transform applied to the centre of its geometry's
     * bounds (a model's whole bounds, whatever its material_slot; a skinned model's in its bind pose; a custom
     * mesh's vertices). Equal depths keep the order given. One key stands for the whole batch, so it says
     * little about an instanced batch spread over the view (trees across a map, the ground as tiles): its mean
     * lands mid-scene however near its nearest instances are. Such batches gain little from the sort; split
     * them by region, or pin them with KEEP_ORDER where they should draw first or last.
     * It moves only a batch whose picture can't depend on what was drawn before it: drawn in the opaque pass
     * (transparency_enabled 0, BTBH_GPU_SCENE_BLEND_ALPHA, not REFRACT), visible, depth_compare
     * BTBH_GPU_SCENE_DEPTH_LESS, the stencil off (reference 0, compare ALWAYS, KEEP, KEEP, REPLACE), transient
     * instances (instance_buffer NULL), bounds known, without BTBH_GPU_SCENE_BATCH_KEEP_ORDER. Every other
     * opaque batch keeps its place and nothing is moved across it, so the sort runs within each run of
     * movable batches between them: other depth compares (LESS_EQUAL overlays, ALWAYS), stencil writers and
     * testers, persistent instance buffers (their positions live on the GPU), a custom mesh with a position
     * that isn't finite, KEEP_ORDER.
     * Transparent and refracting batches draw after the opaque ones in the order given, as before, and so do
     * the shadow maps and the radiance cascades' sources.
     * The picture stays the same, except where two opaque surfaces have exactly the same depth (copies of one
     * surface): with LESS the first drawn shows, and the sort may change which is first; pin such a batch
     * with KEEP_ORDER or draw it with LESS_EQUAL. The pass may bind pipelines more often (the batches are no
     * longer grouped as given). Costs the CPU one pass over the movable batches' instances and a sort of the
     * batches; memory for it is kept with the window.
     * A host that draws everything with LESS_EQUAL (as Tyrian3D's batches do) sees nothing move here: see
     * FRONT_TO_BACK_LESS_EQUAL. */
    BTBH_GPU_SCENE_OPAQUE_SORT_FRONT_TO_BACK = 1,
    /* As FRONT_TO_BACK, and it also moves batches drawn with BTBH_GPU_SCENE_DEPTH_LESS_EQUAL (the same
     * conditions otherwise), LESS and LESS_EQUAL batches sorting together. With LESS_EQUAL the last drawn of
     * two surfaces at exactly the same depth shows, so an overlay drawn coplanar over what it lies on (a decal,
     * a road on the ground, a selection ring) must keep its place: give it BTBH_GPU_SCENE_BATCH_KEEP_ORDER,
     * after what it covers, or the sort may draw it first and hide it. */
    BTBH_GPU_SCENE_OPAQUE_SORT_FRONT_TO_BACK_LESS_EQUAL = 2,
    BTBH_GPU_SCENE_OPAQUE_SORT_COUNT = 3
} BTBHGPUSceneOpaqueSort;

/* Defined when BTBHGPUScenePost.opaque_sort (FRONT_TO_BACK and FRONT_TO_BACK_LESS_EQUAL) and
   BTBH_GPU_SCENE_BATCH_KEEP_ORDER exist (since 0.5.0). */
#define BTBH_GPU_SCENE_HAS_OPAQUE_SORT 1

typedef struct BTBHGPUScenePost {
    BTBHGPUTexture *lut_texture;
    float tonemap_white;
    float exposure;
    float contrast;
    float saturation;
    float cas_sharpness;
    float focus_point_x;
    float focus_point_y;
    float focus_direction_x;
    float focus_direction_y;
    float focus_half_width;
    float maximum_radius_pixels;
    float blur_curve;
    float highlight_threshold;
    float highlight_gain;
    uint8_t lut_enabled;
    uint8_t cas_enabled;
    uint8_t tilt_shift_enabled;
    uint8_t ssao_enabled;
    uint8_t tonemap_mode;
    uint8_t anti_aliasing;
    uint8_t upscale_mode;
    /* 1 flips the final scene image left-right before it is composited. */
    uint8_t mirror_horizontal;
    BTBHGPUSceneRadianceCascades radiance_cascades;
    /* HDR composite before grading. Fog blends toward linear fog_color by
     * 1 - exp(-((view distance - start) * density)^2), capped at the maximum
     * opacity; the cleared background counts as infinitely far. */
    float fog_color[3];
    float fog_density;
    float fog_start_distance;
    float fog_maximum_opacity;
    /* Edge fade: within fog_edge_fade_distance of the world XZ rectangle
     * (min x, min z, max x, max z) the fog rises to full, so the end of the
     * world dissolves into the fog colour. Surfaces nearer than
     * fog_edge_clear_distance (view depth) keep a crisp edge; the fade ramps
     * in over the next fog_edge_fade_distance. */
    float fog_edge_bounds[4];
    float fog_edge_fade_distance;
    float fog_edge_clear_distance;
    /* Soft-threshold bloom added to the HDR image before grading; the
     * threshold is in exposed units (scene colour times exposure). */
    float bloom_intensity;
    float bloom_threshold;
    float bloom_soft_knee;
    float bloom_scatter;
    /* Round vignette that darkens the HDR image toward the corners. */
    float vignette_intensity;
    float vignette_smoothness;
    uint8_t fog_enabled;
    uint8_t bloom_enabled;
    uint8_t vignette_enabled;
    uint8_t fog_edge_enabled;
    /* Temporal upscaling (BTBH_GPU_SCENE_UPSCALE_FSR3). Per transient instance its world translation
     * since the previous frame and the seconds its vertex-animated surface (dissolve, shatter) advanced
     * since then (four floats: x, y, z, seconds; the count must equal the transient instance count),
     * or NULL when nothing moves on its own. The camera's near and far planes and vertical
     * field of view (radians), the seconds since the previous frame, metres per world unit (0 for 1),
     * and the reactive mask's scale for transparent effects. temporal_reset starts a new history
     * (a camera cut). */
    const float *temporal_instance_motions;
    uint32_t temporal_instance_motion_count;
    float temporal_camera_near;
    float temporal_camera_far;
    float temporal_vertical_fov;
    float temporal_frame_seconds;
    float temporal_meters_per_unit;
    float temporal_reactive_scale;
    uint8_t temporal_reset;
    uint8_t temporal_reserved[3];
    /* Render-only world curvature: x the signed curvature (1 / radius in world units; positive bends
     * the ground down around the centre like a small planet, negative compresses distance like a
     * hyperbolic plane, 0 draws the world flat), y and z the world x and z it bends around. Gameplay,
     * lighting and shadows stay flat; only where the scene's vertices are drawn moves. */
    float world_curvature[4];
    /* Screen-space reflections, added in the SSAO resolve (production_ssao_resolve.frag.hlsl, which then runs
     * without ambient occlusion too): their intensity (0 off; it multiplies the Fresnel reflectance), the
     * roughest surface that reflects, how far a reflected ray looks and the thickness a hit may lie behind a
     * surface (render units). Without the radiance cascades they reflect nothing unless flags has
     * BTBH_GPU_SCENE_POST_SSR_SURFACE_REFLECTIVITY: the scene's targets there mark every surface unlit, which they
     * skip (as since they came; kept so for the hosts that run them so). */
    float ssr_intensity;
    float ssr_max_roughness;
    float ssr_max_distance;
    float ssr_thickness;
    /* Ambient occlusion (ssao_enabled 1): the radius occluders count within (render units), its strength,
     * and with the cascades the share of the indirect light it occludes and the share of that the direct light
     * keeps (both 0...1; not under additive cascades with BTBH_GPU_SCENE_POST_ADDITIVE_CASCADES_KEEP_HBAO, since
     * 0.12.0, where it darkens the ambient light as without the cascades). A zero radius leaves all four at their
     * defaults (1, 0.4, 0.75, 0.2). */
    float ao_radius;
    float ao_intensity;
    float ao_contact_strength;
    float ao_direct_share;
    /* Horizon-based ambient occlusion (ssao_enabled 2, from Tyrian3D), worked out after the opaque surfaces are
     * drawn and laid over them before the transparent ones, at half the render resolution (horizons in 4
     * directions of 4 steps, turned by a 4x4 pattern that a depth-aware pass evens out; a depth-aware upsample).
     * ssao_radius: how far round a point it looks for what hides it (world units; 0: 1, at most a tenth of the
     * view's height on the screen); the visibility is exp(-ssao_intensity * occlusion) ^ ssao_power (0: 1 each);
     * ssao_bias: how far over a surface's plane a point must stand to hide it (world units; 0: 0.03). Each
     * batch's ssao_mode says how it takes part. */
    float ssao_radius;
    float ssao_intensity;
    float ssao_power;
    float ssao_bias;
    /* 1: it darkens all the light of the opaque surfaces but their emission, lit or unlit (contact shading);
     * 0: the ambient light of lit surfaces, or with the radiance cascades on, all their light but the emission,
     * three quarters as much (but with BTBH_GPU_SCENE_POST_ADDITIVE_CASCADES_KEEP_HBAO under additive ones). */
    uint8_t ssao_contact;
    /* 1: the scene shows the occlusion alone, in grey (for tuning). */
    uint8_t ssao_debug_view;
    /* The order the opaque pass draws its batches in: a BTBHGPUSceneOpaqueSort (since 0.5.0, in a byte that
     * was ssao_reserved[0], so the struct keeps its size). 0 draws them in the order given, as before; a value
     * past the enum fails the present with BTBH_GPU_INVALID_ARGUMENT. */
    uint8_t opaque_sort;
    /* BTBH_GPU_SCENE_POST_* bits (since 0.9.0, in the byte that was ssao_reserved[0], so the struct keeps its
     * size). 0 renders as before; a bit the renderer doesn't know fails the present with
     * BTBH_GPU_INVALID_ARGUMENT. */
    uint8_t flags;
} BTBHGPUScenePost;

/* BTBHGPUScenePost.flags (since 0.9.0). Each needs the scene shaders of the release that brought it
 * (production_model*.frag, production_ssao_resolve.frag, production_hrc_resolve.comp), and nothing detects older ones:
 * with 0.8.0's SSR_SURFACE_REFLECTIVITY changes nothing and KEEP_HBAO darkens by the albedo (don't set it with them);
 * with 0.9.0's SSR_FACING_HITS changes nothing; with 0.11.0's (production_model_rc*.frag) KEEP_HBAO with contact AO
 * keeps the ambient light instead of the emission from its darkening (don't set it with them). */
enum {
    /* The ambient occlusion under additive radiance cascades (BTBHGPUSceneRadianceCascades.additive) darkens what it
     * darkens without the cascades: the horizon-based one (ssao_enabled 2, ssao_contact 0) and, since 0.12.0, the SSAO
     * (ssao_enabled 1) the ambient light of lit surfaces (diffuse, and specular as its occlusion goes); since 0.12.0,
     * contact AO (ssao_enabled 2, ssao_contact 1) all the opaque surfaces' light but their emission. Without it,
     * cascades of any kind make the HBAO and the SSAO darken three quarters of all their light but the emission (the
     * SSAO as ao_contact_strength and ao_direct_share say), which suits cascades that replace the scene's ambient light
     * and darkens additive ones, whose surfaces keep it (Be The Bullet Hell v2, who asked for it: with nothing glowing,
     * its night stage lost 1.4 sRGB levels on average once the cascades were on); and contact AO keeps the street
     * lamps' light (the cascades' scene shader carries it in the emission target) and the colour of opaque unlit
     * surfaces (v2's lit surfaces in a street lamp's pool came out up to 94 levels brighter than without the cascades).
     * - With nothing glowing, a frame is then the frame without the cascades, to the byte (SSR off). Under the omni
     *   shadow a pixel may differ by a level now and then: the cascades' variant of the scene shader rounds that light
     *   apart from the plain one. (With no ambient occlusion a frame with nothing glowing is that frame without the
     *   flag too, but for the transparent pass: nothing darkens the scene.)
     * - The transparent pass draws as without the cascades too (since 0.10.0), whatever the ambient occlusion (since
     *   0.11.0: the HBAO, ssao_contact 1, the SSAO of ssao_enabled 1, or none; 0.10.0 needed the HBAO): a transparent
     *   unlit batch takes the fast unlit shader, which adds no point light (nor the dark scene's darkening and spot,
     *   nor unlit 2 and 3's sun shadow). Under the cascades without the flag, and in 0.9.0 with it, such a batch draws
     *   with the cascades' scene shader, which does add them: v2's additive night overlays by its neon point lights
     *   (reach 2.6, energy 2.1) came out up to 200 sRGB levels brighter than without the cascades. This changes the
     *   frames drawn with the flag wherever a point light (or the dark scene, or unlit 2 or 3) meets a transparent
     *   unlit batch. A batch with BTBH_GPU_SCENE_BATCH_FLAGS2_KEEP_CASCADES_LOOK (since 0.12.0: v2's weapon beams and
     *   glows) keeps the cascades' scene shader, and draws as without the flag.
     * - The bounce light the cascades add isn't darkened (it isn't the ambient light), as their emitters' isn't. With
     *   contact AO (since 0.12.0) the HBAO is laid over the opaque scene before the cascades add it; without the flag,
     *   as in 0.11.0 with it, contact AO comes after them and darkens it with the rest.
     * - How: the opaque pass writes the scene's own ambient light to the targets the cascades' resolve reads its
     *   materials from (as without the cascades), and the albedo the bounce light lands on to the emission target,
     *   which the additive resolve doesn't otherwise read; with contact AO, which reads no ambient light, the emission
     *   it keeps to the ambient target. The bounce light is the same but at the edges of metal surfaces (a level or
     *   two), where the resolve's coarser samples blend albedo times (1 - metalness) instead of each apart.
     * - Screen-space reflections then weigh each surface by its reflectivity's luminance, as with
     *   BTBH_GPU_SCENE_POST_SSR_SURFACE_REFLECTIVITY (the cascades' targets kept its colour); with no ambient occlusion
     *   they read the cascades' targets, as without the flag.
     * - Ignored, rendering as without it: without the cascades or with them not additive, and in the cascades' debug
     *   views (and the HBAO's debug view, ssao_debug_view, shows its occlusion as without it). With no ambient
     *   occlusion (ssao_enabled 0) the flag changes the transparent pass alone. Everything but the transparent pass
     *   (the ambient light, the bounce light's albedo, the reflections' reflectivity) applies with the HBAO in either
     *   mode and with the SSAO since 0.12.0; 0.9.0 to 0.11.0 applied it with ssao_enabled 2 and ssao_contact 0 only. So
     *   0.12.0 changes the frames drawn with the flag under additive cascades with contact AO or the SSAO. */
    BTBH_GPU_SCENE_POST_ADDITIVE_CASCADES_KEEP_HBAO = 1u << 0,
    /* Screen-space reflections without the radiance cascades weigh each surface by its own reflectivity, as with
     * the cascades: its F0 (0.04 for a dielectric, 0.12 for glass, detail pattern 5, its albedo for a metal,
     * between them as metalness goes). Without it they reflect nothing there (see ssr_intensity). Asked for by
     * Be The Bullet Hell v2.
     * - The reflectivity is the F0's luminance, one channel: a dielectric, glass or a grey metal reflects as with
     *   the cascades, to the byte; a coloured metal reflects as bright, untinted.
     * - With the cascades on it changes nothing (they carry the F0 already); nor without SSR. */
    BTBH_GPU_SCENE_POST_SSR_SURFACE_REFLECTIVITY = 1u << 1,
    /* Screen-space reflections take only hits a ray can make (since 0.10.0, asked for by Be The Bullet Hell v2: under
     * a steep camera in the rain, every actor left a noisy column of itself smeared down the wet street below it).
     * The depth buffer holds only the surfaces in front, so a ray that passes behind or beneath a thin object (a jet,
     * a kaiju's arm or tail) within ssr_thickness of it was taken for a hit on its visible face.
     * - A hit on a surface that faces along the ray (its normal less than 90 degrees from the ray's direction: the
     *   top of anything seen from above, for a ray that climbs from the street) can't be one: the ray passed behind
     *   or beneath that surface, and marches on to what lies past it.
     * - The hits left fade as the surface turns edge on to the ray (its normal within about 9 degrees of square to
     *   it) and as the ray passes farther behind the surface (from a quarter of ssr_thickness to all of it).
     * - Walls and the faces of objects turned toward the camera keep their reflections: a ray reflected from level
     *   ground keeps the camera's heading, so it meets them from the front.
     * - With or without the radiance cascades, and with BTBH_GPU_SCENE_POST_SSR_SURFACE_REFLECTIVITY or without; it
     *   changes nothing without SSR. Needs this release's production_ssao_resolve.frag; older ones ignore it. */
    BTBH_GPU_SCENE_POST_SSR_FACING_HITS = 1u << 2
};

/* Defined when BTBHGPUScenePost.flags and BTBH_GPU_SCENE_POST_ADDITIVE_CASCADES_KEEP_HBAO exist (since 0.9.0). */
#define BTBH_GPU_SCENE_HAS_ADDITIVE_CASCADES_KEEP_HBAO 1
/* Defined when BTBHGPUScenePost.flags and BTBH_GPU_SCENE_POST_SSR_SURFACE_REFLECTIVITY exist (since 0.9.0). */
#define BTBH_GPU_SCENE_HAS_SSR_SURFACE_REFLECTIVITY 1
/* Defined when BTBH_GPU_SCENE_POST_SSR_FACING_HITS exists (since 0.10.0). */
#define BTBH_GPU_SCENE_HAS_SSR_FACING_HITS 1

typedef struct BTBHGPUSceneReport {
    BTBHGPUResult result;
    uint32_t drawable_width;
    uint32_t drawable_height;
    uint32_t batch_count;
    uint32_t primitive_draw_count;
    uint32_t prepared_geometry_count;
    uint32_t instance_count;
    uint32_t transient_instance_count;
    uint32_t persistent_batch_count;
    double cpu_prepare_milliseconds;
    double cpu_upload_milliseconds;
    double cpu_shadow_encode_milliseconds;
    double cpu_swapchain_acquire_milliseconds;
    double cpu_target_ensure_milliseconds;
    double cpu_geometry_encode_milliseconds;
    double cpu_post_encode_milliseconds;
    BTBHGPUSceneRadianceCascadesStatus radiance_cascades;
    char message[256];
    /* The upscaling the scene ran (BTBHGPUSceneUpscaleMode): post->upscale_mode, except
       BTBH_GPU_SCENE_UPSCALE_FSR1 where FSR 3 was asked for and is unavailable (the device lacks
       its formats or shaders, an encode failed, or the scene was rendered to a target), and
       BTBH_GPU_SCENE_UPSCALE_LINEAR for a scene rendered to a target at its size or bigger. 0 when
       the present failed. */
    uint8_t upscale_mode;
    uint8_t upscale_reserved[3];
} BTBHGPUSceneReport;

typedef struct BTBHGPUFrameTelemetrySample {
    uint64_t frame_serial;
    double cpu_submit_milliseconds;
    double fence_completion_milliseconds;
} BTBHGPUFrameTelemetrySample;

const char *btbh_gpu_backend_driver_name(BTBHGPUBackendRequest backend);
BTBHGPUShaderFormat btbh_gpu_backend_shader_format(BTBHGPUBackendRequest backend);
BTBHGPUResult btbh_gpu_validate_capabilities(
    BTBHGPUBackendRequest backend,
    BTBHGPUFeatureFlags required_features,
    const BTBHGPUCapabilitySnapshot *snapshot,
    BTBHGPUValidationReport *report
);
BTBHGPUDevice *btbh_gpu_device_create(
    const BTBHGPUDeviceDescriptor *descriptor,
    BTBHGPUDeviceReport *report
);
void btbh_gpu_device_shutdown(BTBHGPUDevice *device);
void btbh_gpu_device_destroy(BTBHGPUDevice *device);
/* Upload staging (since 0.11.0, asked for by Be The Bullet Hell v2): keeps up to `bytes` of upload buffers for reuse.
   Every upload copies its data into a transfer buffer (host-visible memory) first. By default (a budget of 0) each
   upload makes its own and releases it, and SDL frees that memory at the next frame that presents: after a level's
   load, the first frame frees every one the load made (SDL 3.4 on Vulkan: about 0.1 ms each, more for big ones; 17 to
   28 ms for a load of 140 uploads on an RTX 3070 Ti). With a budget, uploads take ranges of a few pooled transfer
   buffers instead (at most 16, 4 MiB or a power of two that holds the upload), and a pooled buffer starts again once
   the GPU has finished every upload that read it (checked with fences, never waited for), so a load makes a few
   buffers and frees none. Pooled: textures (made, loaded, updated, arrays, heightfields), the shared geometry buffers
   (models' primitives and BTBH_GPU_CUSTOM_MESH_SHARED_BUFFERS meshes), custom meshes with buffers of their own (made,
   updated, updated in part) and instance buffer uploads (plain and BTBH_GPU_INSTANCE_BUFFER_UPLOAD_NO_CYCLE). An
   upload recorded into a frame waiting for its canvas (a NO_CYCLE upload or a plain mesh update meanwhile) still
   makes its own; queued uploads are copied from the scene's own transfer buffer, as without a budget. An upload
   bigger than the budget, or one while every pooled buffer is still being read, makes its own too. The pool's memory
   stays allocated until the budget is lowered (idle buffers beyond it are released) or the device shuts down.
   Uploads and frames are the same to the byte with or without it. One device at a time: another device's call, a
   NULL or shut-down device return BTBH_GPU_INVALID_ARGUMENT.
   Since 0.13.0 a device made with BTBH_GPU_BACKEND_VULKAN starts with a budget of
   BTBH_GPU_DEFAULT_UPLOAD_STAGING_BUDGET (32 MiB), when no other device has the pool; a Metal device starts with none.
   The environment variable BTBH_GPU_UPLOAD_STAGING_BUDGET=<bytes>, read when a device is made, sets the budget it starts
   with instead (0: none, 0.12.0's behaviour), and this call changes it at any time. Why (Be The Bullet Hell v1's exit
   stall): SDL 3.4 frees a released upload buffer's memory only in a submit that presents, or in SDL_DestroyGPUDevice
   (about 0.1 ms each on an RTX 3070 Ti), so every upload since the last presented frame (a level streamed in, then
   quit; a minimized window) waited for the device's destroy. 1,500 such uploads made the exit 246 ms instead of
   about 70; with the budget, 77 ms. */
BTBHGPUResult btbh_gpu_device_set_upload_staging_budget(
    BTBHGPUDevice *device,
    uint32_t bytes
);
/* Defined when btbh_gpu_device_set_upload_staging_budget exists (since 0.11.0). */
#define BTBH_GPU_HAS_UPLOAD_STAGING_BUDGET 1
/* The upload staging budget a Vulkan device starts with (since 0.13.0). */
#define BTBH_GPU_DEFAULT_UPLOAD_STAGING_BUDGET (32u * 1024u * 1024u)
double btbh_gpu_monotonic_elapsed_seconds(void);
BTBHGPUWindow *btbh_gpu_window_create(
    const BTBHGPUWindowDescriptor *descriptor,
    BTBHGPUWindowReport *report
);
BTBHGPUResult btbh_gpu_window_claim(
    BTBHGPUDevice *device,
    BTBHGPUWindow *window,
    BTBHGPUWindowReport *report
);
BTBHGPUResult btbh_gpu_window_resize(
    BTBHGPUWindow *window,
    uint32_t width,
    uint32_t height,
    BTBHGPUWindowReport *report
);
BTBHGPUResult btbh_gpu_window_minimize(
    BTBHGPUWindow *window,
    BTBHGPUWindowReport *report
);
BTBHGPUResult btbh_gpu_window_restore(
    BTBHGPUWindow *window,
    BTBHGPUWindowReport *report
);
/* Switches the window's present mode without recreating the window, e.g. from a Graphics menu's
   V-Sync row (since 0.3.0). present_mode is taken as BTBHGPUWindowDescriptor.present_mode is:
   0 for the default choice (the fastest mode the window supports), else that mode, or VSYNC where
   the window lacks it. The swapchain keeps its SDR composition. report->present_mode says what
   was taken, from the next frame on. Switching may wait for the GPU to finish the frames in
   flight; a request that resolves to the mode in use changes nothing. Call it between frames:
   while a frame is pending (a scene presented, its canvas not yet) it changes nothing and returns
   BTBH_GPU_INVALID_ARGUMENT. Before the claim it sets the mode the claim asks for. An unknown
   mode returns BTBH_GPU_INVALID_ARGUMENT without touching the report. */
BTBHGPUResult btbh_gpu_window_set_present_mode(
    BTBHGPUWindow *window,
    BTBHGPUPresentMode present_mode,
    BTBHGPUWindowReport *report
);
BTBHGPUResult btbh_gpu_window_present_clear(
    BTBHGPUWindow *window,
    uint8_t red,
    uint8_t green,
    uint8_t blue,
    uint8_t alpha,
    BTBHGPUWindowReport *report
);
BTBHGPUResult btbh_gpu_window_poll_events(
    BTBHGPUWindow *window,
    BTBHGPUPlatformSnapshot *snapshot
);
/* The single-pad view: the snapshot of the roster's lowest occupied slot (see
 * BTBH_GPU_GAMEPAD_CAPACITY), all zeros without a pad. It falls over to the next pad when that one
 * leaves; btbh_gpu_window_gamepad_snapshot_at holds one pad per player. */
BTBHGPUResult btbh_gpu_window_gamepad_snapshot(
    const BTBHGPUWindow *window,
    BTBHGPUGamepadSnapshot *snapshot
);
/* Rumbles the lowest occupied slot's pad (the one btbh_gpu_window_gamepad_snapshot reads): its low-
 * and high-frequency motors (0..65535) for duration_ms; a later call replaces the effect and zero
 * stops it. Without a pad, or on one without motors, it does nothing and still succeeds. */
BTBHGPUResult btbh_gpu_window_gamepad_rumble(
    BTBHGPUWindow *window,
    uint16_t low_frequency,
    uint16_t high_frequency,
    uint32_t duration_ms
);
/* The number of roster slots up to the highest occupied one, 0 to BTBH_GPU_GAMEPAD_CAPACITY (0
 * without a pad, or for a NULL window). Since 0.15.0; it changes only in
 * btbh_gpu_window_poll_events (and when the window is made). */
uint32_t btbh_gpu_window_gamepad_count(const BTBHGPUWindow *window);
/* Slot index's snapshot as of the last btbh_gpu_window_poll_events, as
 * btbh_gpu_window_gamepad_snapshot gives the lowest slot's. An empty slot reads as all zeros
 * (instance_id 0) and returns BTBH_GPU_OK. An index at or past BTBH_GPU_GAMEPAD_CAPACITY, a NULL
 * window or a NULL out returns BTBH_GPU_INVALID_ARGUMENT, with out (when not NULL) zeroed. Since
 * 0.15.0. */
BTBHGPUResult btbh_gpu_window_gamepad_snapshot_at(
    const BTBHGPUWindow *window,
    uint32_t index,
    BTBHGPUGamepadSnapshot *out
);
/* Slot index's pad: its identity (name, serial, GUID, vendor and product), kind, connection and
 * battery, read from SDL at the call. An empty slot reads as all zeros and returns BTBH_GPU_OK; the
 * arguments are checked as btbh_gpu_window_gamepad_snapshot_at's. Since 0.15.0. */
BTBHGPUResult btbh_gpu_window_gamepad_info_at(
    const BTBHGPUWindow *window,
    uint32_t index,
    BTBHGPUGamepadInfo *out
);
/* Rumbles slot index's pad as btbh_gpu_window_gamepad_rumble does the lowest one's. An empty slot,
 * or a pad without motors, does nothing and returns BTBH_GPU_OK; an index at or past
 * BTBH_GPU_GAMEPAD_CAPACITY or a NULL window returns BTBH_GPU_INVALID_ARGUMENT. Since 0.15.0. */
BTBHGPUResult btbh_gpu_window_gamepad_rumble_at(
    BTBHGPUWindow *window,
    uint32_t index,
    uint16_t low_frequency,
    uint16_t high_frequency,
    uint32_t duration_ms
);
BTBHGPUResult btbh_gpu_window_begin_frame(
    BTBHGPUWindow *window,
    BTBHGPUWindowReport *report,
    BTBHGPUPlatformSnapshot *snapshot
);
void btbh_gpu_window_set_frame_telemetry_enabled(
    BTBHGPUWindow *window,
    uint8_t enabled
);
uint32_t btbh_gpu_window_poll_frame_telemetry(
    BTBHGPUWindow *window,
    BTBHGPUFrameTelemetrySample *samples,
    uint32_t capacity
);
uint32_t btbh_gpu_window_finish_frame_telemetry(
    BTBHGPUWindow *window,
    BTBHGPUFrameTelemetrySample *samples,
    uint32_t capacity
);
BTBHGPUResult btbh_gpu_window_set_mouse_position(
    BTBHGPUWindow *window,
    float x,
    float y
);
BTBHGPUResult btbh_gpu_window_set_cursor_visible(
    BTBHGPUWindow *window,
    uint8_t visible
);
BTBHGPUResult btbh_gpu_window_set_clipboard_text(
    BTBHGPUWindow *window,
    const char *text
);
BTBHGPUResult btbh_gpu_window_show_file_dialog(
    BTBHGPUWindow *window,
    const BTBHGPUFileDialogDescriptor *descriptor,
    BTBHGPUFileDialogCallback callback,
    void *userdata,
    BTBHGPUFileDialogReport *report
);
uint8_t btbh_gpu_window_key_down(
    BTBHGPUWindow *window,
    uint16_t project_key
);
uint8_t btbh_gpu_window_key_pressed(
    BTBHGPUWindow *window,
    uint16_t project_key
);
uint8_t btbh_gpu_window_mouse_button_down(
    BTBHGPUWindow *window,
    uint8_t project_button
);
uint8_t btbh_gpu_window_mouse_button_pressed(
    BTBHGPUWindow *window,
    uint8_t project_button
);
uint8_t btbh_gpu_window_mouse_button_released(
    BTBHGPUWindow *window,
    uint8_t project_button
);
BTBHGPUTexture *btbh_gpu_window_texture_load(
    BTBHGPUWindow *window,
    const char *path,
    uint8_t generate_mipmaps,
    BTBHGPUTextureFilter filter,
    BTBHGPUTextureWrap wrap,
    BTBHGPUTextureReport *report
);
BTBHGPUTexture *btbh_gpu_window_texture_load_ex(
    BTBHGPUWindow *window,
    const char *path,
    uint8_t generate_mipmaps,
    BTBHGPUTextureFilter filter,
    BTBHGPUTextureWrap wrap,
    BTBHGPUTextureColorSpace color_space,
    BTBHGPUTextureReport *report
);
BTBHGPUTexture *btbh_gpu_window_render_target_create(
    BTBHGPUWindow *window,
    uint32_t width,
    uint32_t height,
    BTBHGPUTextureFilter filter,
    BTBHGPUTextureReport *report
);
void btbh_gpu_texture_destroy(BTBHGPUTexture *texture);
BTBHGPUFont *btbh_gpu_window_font_load(
    BTBHGPUWindow *window,
    const char *path,
    uint32_t base_size,
    const uint32_t *glyphs,
    uint32_t glyph_count,
    BTBHGPUFontReport *report
);
BTBHGPUResult btbh_gpu_font_glyph(
    const BTBHGPUFont *font,
    uint32_t codepoint,
    BTBHGPUFontGlyph *glyph
);
BTBHGPUTexture *btbh_gpu_font_atlas(BTBHGPUFont *font);
void btbh_gpu_font_destroy(BTBHGPUFont *font);
/* A font baked for readable small text (BTBHGPUFontDesc): oversampled and prefiltered with
   stb_truetype's packer, its atlas always sampled linearly. The glyphs from btbh_gpu_font_glyph have
   fractional offsets and advances (whole-pixel advances with snap_advances), and UVs that undo the
   oversampling: draw each glyph's quad at (pen + offset_x, baseline + offset_y), width x height
   output pixels, with no scaling. For sharp text put the baseline and each run's start on whole
   pixels, and each glyph's pen position on the 1/oversample_x pixel grid (whole pixels without
   oversampling); add btbh_gpu_font_kerning between glyphs. The canvas shader is the same.
   Codepoints the font lacks get a glyph with nothing in it (no advance), as with
   btbh_gpu_window_font_load. An invalid desc or argument returns NULL with the report's result
   BTBH_GPU_INVALID_ARGUMENT and the reason; report must not be NULL. report->base_size is
   pixel_size rounded to whole pixels; its ascent, descent and line_gap are
   btbh_gpu_font_metrics'. The atlas is RGBA8 (white, coverage in alpha), or R8 coverage with
   BTBH_GPU_FONT_FLAG_R8_ATLAS; report->atlas_width and atlas_height are its size either way. */
BTBHGPUFont *btbh_gpu_window_font_load_ex(
    BTBHGPUWindow *window,
    const char *path,
    const BTBHGPUFontDesc *desc,
    const uint32_t *glyphs,
    uint32_t glyph_count,
    BTBHGPUFontReport *report
);
/* Vertical metrics in pixels at the font's size. For a font from btbh_gpu_window_font_load the
   ascent, descent and line gap are what its report said (the ascent rounded down, a line gap of 2);
   x_height and cap_height are unrounded either way. With BTBH_GPU_FONT_FLAG_SNAP_X_HEIGHT (or _DOWN)
   they are at the vertical scale the glyphs were rasterised with: x_height a whole number, the rest
   scaled to match (see the flag); pixel_size is still the size asked for. */
BTBHGPUResult btbh_gpu_font_metrics(
    const BTBHGPUFont *font,
    BTBHGPUFontMetrics *metrics
);
/* How much further (negative: closer) the pen moves between left and right at the font's size, in
   pixels, from the font's kerning (GPOS pair adjustments or its kern table): 0 if none, or if either
   codepoint isn't one the font was baked with or the font lacks it. Whole pixels with snap_advances.
   Always 0 for a font from btbh_gpu_window_font_load. */
float btbh_gpu_font_kerning(
    const BTBHGPUFont *font,
    uint32_t left,
    uint32_t right
);
/* Every nonzero kerning pair of the font in one call, sorted by left, then right: the pairs
   btbh_gpu_font_kerning answers with something other than 0, with the same advances, between the
   codepoints the font was baked with and has. Copies the first min(count, capacity) of them into pairs
   and returns count, the number the font has, whatever capacity is: call it with NULL and 0 to size
   the array, then again to fill it. With pairs NULL nothing is copied. 0 for a font from
   btbh_gpu_window_font_load, a font without kerning, or a NULL font. It still answers after the
   font's window is destroyed. */
uint32_t btbh_gpu_font_kerning_pairs(
    const BTBHGPUFont *font,
    BTBHGPUFontKerningPair *pairs,
    uint32_t capacity
);
/* A batch with a texture is drawn as the texture x the vertex colour with
   textured_fragment_shader_path. A batch whose texture has one channel (a font atlas baked with
   BTBH_GPU_FONT_FLAG_R8_ATLAS) is drawn as white x its coverage x the vertex colour with
   production_canvas_coverage.frag from the same directory, loaded the first time a batch needs it (since
   0.5.0); if it can't be loaded the present fails with BTBH_GPU_INVALID_ARGUMENT and the path in the
   report, and draws nothing, as any other failed present. */
BTBHGPUResult btbh_gpu_window_canvas_present(
    BTBHGPUWindow *window,
    const char *vertex_shader_path,
    const char *fragment_shader_path,
    const char *textured_fragment_shader_path,
    BTBHGPUShaderFormat shader_format,
    const BTBHGPUCanvasVertex *vertices,
    uint32_t vertex_count,
    const BTBHGPUCanvasBatch *batches,
    uint32_t batch_count,
    const BTBHGPUCanvasPass *passes,
    uint32_t pass_count,
    BTBHGPUCanvasReport *report
);
BTBHGPUTexture *btbh_gpu_window_capture_pending_scene_texture(
    BTBHGPUWindow *window,
    BTBHGPUTextureReport *report
);
/* Copies the finished scene into a capture of the same size and format and releases the frame,
   so another scene can render before the canvas presents. */
BTBHGPUResult btbh_gpu_window_copy_pending_scene_texture(
    BTBHGPUWindow *window,
    BTBHGPUTexture *destination,
    BTBHGPUTextureReport *report
);
void btbh_gpu_window_request_presented_frame_readback(
    BTBHGPUWindow *window
);
BTBHGPUResult btbh_gpu_window_readback_presented_frame(
    BTBHGPUWindow *window,
    uint8_t *rgba8,
    uint32_t byte_capacity,
    BTBHGPUCanvasReport *report
);
BTBHGPUResult btbh_gpu_write_png_rgba8(
    const char *path,
    uint32_t width,
    uint32_t height,
    const uint8_t *rgba8
);
BTBHGPUModel *btbh_gpu_window_model_load(
    BTBHGPUWindow *window,
    const char *path,
    BTBHGPUModelReport *report
);
BTBHGPUModel *btbh_gpu_window_model_load_ex(
    BTBHGPUWindow *window,
    const char *path,
    uint32_t import_flags,
    BTBHGPUModelReport *report
);
int32_t btbh_gpu_model_material_slot(
    const BTBHGPUModel *model,
    uint32_t mesh_index
);
BTBHGPUResult btbh_gpu_model_material_info(
    const BTBHGPUModel *model,
    uint32_t material_index,
    BTBHGPUModelMaterialInfo *info
);
BTBHGPUResult btbh_gpu_model_mesh_info(
    const BTBHGPUModel *model,
    uint32_t mesh_index,
    BTBHGPUModelMeshInfo *info
);
BTBHGPUResult btbh_gpu_model_mesh_copy_positions(
    const BTBHGPUModel *model,
    uint32_t mesh_index,
    float *positions,
    uint32_t float_capacity
);
BTBHGPUResult btbh_gpu_model_mesh_copy_indices(
    const BTBHGPUModel *model,
    uint32_t mesh_index,
    uint32_t *indices,
    uint32_t index_capacity
);
void btbh_gpu_model_destroy(BTBHGPUModel *model);
BTBHGPUInstanceBuffer *btbh_gpu_window_instance_buffer_create(
    BTBHGPUWindow *window,
    uint32_t capacity,
    uint32_t stride,
    BTBHGPUInstanceBufferReport *report
);
BTBHGPUResult btbh_gpu_instance_buffer_upload(
    BTBHGPUInstanceBuffer *buffer,
    uint32_t byte_offset,
    const void *bytes,
    uint32_t byte_count,
    BTBHGPUInstanceBufferReport *report
);
/* btbh_gpu_instance_buffer_upload_ex's flags (since 0.11.0). Without any, the upload is btbh_gpu_instance_buffer_upload:
 * a command buffer acquired and submitted per call (about 0.17 ms whatever its size) that cycles the buffer, so a frame
 * still drawing it keeps its data, but everything outside the uploaded range is undefined after it; a host that
 * changes part of a buffer has to upload all of it. */
enum {
    /* The upload is queued into the window's next scene and written in place. The bytes are copied at the call into
     * the window's upload queue (the one BTBH_GPU_CUSTOM_MESH_UPDATE_QUEUED uses), and the uploaded prefix (the
     * instances a batch may draw) grows at once. The next btbh_gpu_window_scene_present_instances(_with_visibility)
     * or btbh_gpu_window_scene_render_to_target of the buffer's window copies every queued upload into the GPU at the
     * start of its own command buffer, in one transfer and one copy pass with the scene's instances and the queued
     * mesh updates, before it draws anything: so the new data shows in the next scene that draws, whichever of the two
     * comes first, and in every one after it. The buffer isn't cycled: everything outside the range keeps its data.
     * Frames submitted before that scene, and a scene presented but not yet submitted by the canvas, draw the old data
     * (on the GPU's one queue the copy runs after them). Rules:
     * - Uploads of one buffer land in the order they were made, queued or not: where a later upload overlaps a queued
     *   one that no scene has copied yet, the later one's bytes land (a queued one it covers entirely is dropped),
     *   several queued uploads of one frame included. A BTBH_GPU_INSTANCE_BUFFER_UPLOAD_NO_CYCLE upload made after a
     *   queued one that a scene presented but not yet submitted by the canvas has copied goes into that frame after
     *   its draws, so it lands after it. A plain upload (no flags) cycles the buffer as always: after it only its own
     *   range is defined, a queued range that frame copied included.
     * - btbh_gpu_instance_buffer_destroy drops the buffer's queued uploads; one already copied into a frame that isn't
     *   submitted yet still runs (SDL frees the buffers after it).
     * - A scene that fails before it is submitted leaves the queue as it was, for the next one. A frame that copied
     *   queued uploads is submitted even when it is dropped (the canvas refuses its submission, or the next scene is
     *   presented without one), so that they land. A window closed with uploads queued drops them.
     * - The window's queue holds at most BTBH_GPU_CUSTOM_MESH_QUEUE_MAX_BYTES at a time, queued mesh updates and
     *   instance uploads together (replaced ranges count until the next scene): past that the call fails with
     *   BTBH_GPU_INVALID_ARGUMENT and queues nothing.
     * A window that queues nothing draws as before. */
    BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED = 1u << 0,
    /* The upload is submitted at once, as without flags, but written in place: everything outside the range keeps its
     * data. Frames submitted before it draw the old data (the GPU runs the copy after them, waiting if they still read
     * the buffer). While a scene presented but not yet submitted by the canvas waits, the upload goes into that frame's
     * command buffer after its draws instead of a submission of its own: that frame draws the old data, every one after
     * it the new. Overlapping queued uploads not yet copied take its bytes, as above. With
     * BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED it changes nothing (a queued upload never cycles). */
    BTBH_GPU_INSTANCE_BUFFER_UPLOAD_NO_CYCLE = 1u << 1
};
/* btbh_gpu_instance_buffer_upload with BTBH_GPU_INSTANCE_BUFFER_UPLOAD_* flags; 0 is btbh_gpu_instance_buffer_upload.
 * An unknown bit fails with BTBH_GPU_INVALID_ARGUMENT. The range and the report are btbh_gpu_instance_buffer_upload's. */
BTBHGPUResult btbh_gpu_instance_buffer_upload_ex(
    BTBHGPUInstanceBuffer *buffer,
    uint32_t byte_offset,
    const void *bytes,
    uint32_t byte_count,
    uint32_t flags,
    BTBHGPUInstanceBufferReport *report
);
/* Defined when btbh_gpu_instance_buffer_upload_ex, BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED and
 * BTBH_GPU_INSTANCE_BUFFER_UPLOAD_NO_CYCLE exist (since 0.11.0). */
#define BTBH_GPU_HAS_QUEUED_INSTANCE_UPLOADS 1
void btbh_gpu_instance_buffer_destroy(BTBHGPUInstanceBuffer *buffer);
uint8_t btbh_gpu_pack_transform_stream_instances(
    float *destination,
    const void *positions,
    uint32_t position_stride,
    const void *rotations,
    uint32_t rotation_stride,
    const void *scales,
    uint32_t scale_stride,
    const void *colors,
    uint32_t color_stride,
    const void *customs,
    uint32_t custom_stride,
    uint32_t source_start,
    uint32_t count
);
/* The custom-mesh vertex (a loaded model's vertex too): BTBH_GPU_CUSTOM_MESH_VERTEX_FLOATS floats, 96 bytes,
 * position 0..2, normal 3..5, uv 6..7, tangent 8..11 (w the handedness), colour 12..15, joints 16..19, weights
 * 20..23. A static draw reads the joints' first float as its world-detail table index (since 0.8.0, RFC 0009:
 * BTBHGPUSceneWorldDetailTable) and nothing else of the joints and weights; a skinned draw reads them as bones. */
#define BTBH_GPU_CUSTOM_MESH_VERTEX_FLOATS 24u
#define BTBH_GPU_CUSTOM_MESH_DETAIL_INDEX_FLOAT 16u
BTBHGPUCustomMesh *btbh_gpu_window_custom_mesh_create(
    BTBHGPUWindow *window,
    const void *vertices,
    uint32_t vertex_byte_count,
    uint32_t vertex_count,
    const uint32_t *indices,
    uint32_t index_count,
    BTBHGPUCustomMeshReport *report
);
/* btbh_gpu_window_custom_mesh_create_ex's flags (since 0.10.0, RFC 0011). */
enum {
    /* The mesh's vertices and indices live in the window's shared geometry buffers, beside loaded models'
     * primitives, instead of two buffers of its own. Batches drawing such meshes one after another then bind no
     * vertex or index buffer between them, and a run of batches that differ only in their mesh and instance range
     * (btbh_gpu_window_scene_present_instances) costs little more than its draw calls. For meshes made once and
     * kept until the window closes: a level's or a game's kit, landmarks, set pieces. Rules:
     * - btbh_gpu_custom_mesh_update writes in place and must fit the mesh's ranges: at most the vertex count and
     *   the index count the mesh was created with (a mesh created without indices takes none). A bigger update
     *   fails with BTBH_GPU_INVALID_ARGUMENT and leaves the mesh as it was. An update shows in every frame
     *   submitted after it, a scene already presented but not yet submitted by the canvas included (a mesh with
     *   buffers of its own keeps the old data for that one).
     * - btbh_gpu_custom_mesh_destroy leaves a hole that is never reused. A shared buffer is freed when every mesh
     *   and model in it is destroyed, or when the window closes.
     * Meshes that change size or are remade often (terrain chunks, per-stage meshes) should keep buffers of their
     * own (no flag): their holes would keep the shared buffers alive. */
    BTBH_GPU_CUSTOM_MESH_SHARED_BUFFERS = 1u << 0
};
/* btbh_gpu_window_custom_mesh_create with BTBH_GPU_CUSTOM_MESH_* flags; 0 is btbh_gpu_window_custom_mesh_create.
 * An unknown bit fails with BTBH_GPU_INVALID_ARGUMENT. The report is filled as by
 * btbh_gpu_window_custom_mesh_create. */
BTBHGPUCustomMesh *btbh_gpu_window_custom_mesh_create_ex(
    BTBHGPUWindow *window,
    const void *vertices,
    uint32_t vertex_byte_count,
    uint32_t vertex_count,
    const uint32_t *indices,
    uint32_t index_count,
    uint32_t flags,
    BTBHGPUCustomMeshReport *report
);
/* Defined when btbh_gpu_window_custom_mesh_create_ex and BTBH_GPU_CUSTOM_MESH_SHARED_BUFFERS exist (since 0.10.0). */
#define BTBH_GPU_HAS_SHARED_MESH_BUFFERS 1
BTBHGPUResult btbh_gpu_custom_mesh_update(
    BTBHGPUCustomMesh *mesh,
    const void *vertices,
    uint32_t vertex_byte_count,
    const uint32_t *indices,
    uint32_t index_count,
    BTBHGPUCustomMeshReport *report
);
/* btbh_gpu_custom_mesh_update_ex's flags (since 0.11.0). */
enum {
    /* The update is queued into the window's next scene instead of submitted on its own (btbh_gpu_custom_mesh_update
     * acquires and submits a command buffer per call, about 0.17 ms whatever its size). The data is copied at the
     * call, as without the flag, into a staging area of the window that grows as needed; the mesh's counts and
     * bounds change at once. The next btbh_gpu_window_scene_present_instances(_with_visibility) or
     * btbh_gpu_window_scene_render_to_target of the mesh's window copies every queued update into the GPU at the
     * start of its own command buffer, in one transfer and one copy pass with the scene's instances, before it draws
     * anything: so the new data shows in the next scene that draws, whichever of the two comes first, and in every
     * one after it. Rules:
     * - A later update of the same mesh, queued or not, replaces the queued ones that no scene has copied yet (a
     *   partial one, btbh_gpu_custom_mesh_update_range, only where it overlaps them). A plain update made after a
     *   queued one always lands after it: on a mesh in the shared buffers whose queued update a scene presented but
     *   not yet submitted by the canvas has copied, the plain update is recorded into that frame's command buffer
     *   after its draws, which keep the queued data.
     * - btbh_gpu_custom_mesh_destroy drops the mesh's queued update; one already copied into a frame that isn't
     *   submitted yet still runs (SDL frees the buffers after it).
     * - A mesh in the shared buffers (BTBH_GPU_CUSTOM_MESH_SHARED_BUFFERS): as without the flag, the update must fit
     *   the mesh's ranges and is written in place; a mesh with buffers of its own grows them at the call when the
     *   update needs more, and its frames in flight keep their data (SDL's cycling).
     * - A scene that fails before it is submitted leaves the queue as it was, for the next one. A frame that copied
     *   queued updates is submitted even when it is dropped (the canvas refuses its submission, or the next scene is
     *   presented without one), so that they land. A window closed with updates queued drops them.
     * - The queue holds at most BTBH_GPU_CUSTOM_MESH_QUEUE_MAX_BYTES of data at a time, vertices and indices and
     *   queued instance buffer uploads (BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED) together (replaced updates count until
     *   the next scene): past that the call fails with BTBH_GPU_INVALID_ARGUMENT and queues nothing. One update's
     *   limits are btbh_gpu_custom_mesh_update's.
     * A window that queues nothing draws as before. */
    BTBH_GPU_CUSTOM_MESH_UPDATE_QUEUED = 1u << 0
};
#define BTBH_GPU_CUSTOM_MESH_QUEUE_MAX_BYTES (256u * 1024u * 1024u)
/* btbh_gpu_custom_mesh_update with BTBH_GPU_CUSTOM_MESH_UPDATE_* flags; 0 is btbh_gpu_custom_mesh_update. An unknown
 * bit fails with BTBH_GPU_INVALID_ARGUMENT. The report is filled as by btbh_gpu_custom_mesh_update. */
BTBHGPUResult btbh_gpu_custom_mesh_update_ex(
    BTBHGPUCustomMesh *mesh,
    const void *vertices,
    uint32_t vertex_byte_count,
    const uint32_t *indices,
    uint32_t index_count,
    uint32_t flags,
    BTBHGPUCustomMeshReport *report
);
/* Defined when btbh_gpu_custom_mesh_update_ex and BTBH_GPU_CUSTOM_MESH_UPDATE_QUEUED exist (since 0.11.0). */
#define BTBH_GPU_HAS_QUEUED_MESH_UPDATES 1
/* A partial update (since 0.11.0): vertex_byte_count / 96 vertices written from vertex first_vertex, and index_count
 * indices from index first_index, in place (either part may be empty, not both; an empty part's pointer may be NULL).
 * The rest of the mesh keeps its data and its counts don't change: the range must lie within them (first_vertex plus
 * the vertices at most the vertex count, first_index + index_count at most the index count), and every index must be
 * below the vertex count. For meshes with buffers of their own and in the shared buffers alike. The mesh's bounds (the
 * opaque sort's, BTBH_GPU_SCENE_OPAQUE_SORT_FRONT_TO_BACK) grow to cover the new vertices and never shrink: the old
 * vertices aren't kept to measure again (a whole update measures afresh); a position that isn't finite makes them
 * unknown, as on create. flags: 0 or BTBH_GPU_CUSTOM_MESH_UPDATE_QUEUED.
 * - 0: submitted at once without cycling (a mesh with buffers of its own too). Frames submitted before it draw the
 *   old data (the GPU runs the copy after them). While a scene presented but not yet submitted by the canvas waits, it
 *   goes into that frame's command buffer after its draws instead: that frame draws the old data, every one after it
 *   the new; a queued update that frame copied lands first.
 * - BTBH_GPU_CUSTOM_MESH_UPDATE_QUEUED: copied at the call and written by the window's next scene, as a queued whole
 *   update, never cycling. Updates of one mesh land in the order they were made: where a partial update overlaps
 *   queued ones not yet copied (whole or partial, queued or not), its data lands; a whole update replaces every
 *   queued partial one before it.
 * The report holds the mesh's counts. Refused with BTBH_GPU_INVALID_ARGUMENT, the mesh as it was: an unknown flag, no
 * data, a vertex byte count not a multiple of 96, a range past the counts, an index past the vertices, a full queue
 * (BTBH_GPU_CUSTOM_MESH_QUEUE_MAX_BYTES). */
BTBHGPUResult btbh_gpu_custom_mesh_update_range(
    BTBHGPUCustomMesh *mesh,
    uint32_t first_vertex,
    const void *vertices,
    uint32_t vertex_byte_count,
    uint32_t first_index,
    const uint32_t *indices,
    uint32_t index_count,
    uint32_t flags,
    BTBHGPUCustomMeshReport *report
);
/* Defined when btbh_gpu_custom_mesh_update_range exists (since 0.11.0). */
#define BTBH_GPU_HAS_PARTIAL_MESH_UPDATES 1
void btbh_gpu_custom_mesh_destroy(BTBHGPUCustomMesh *mesh);
/* Same-material runs (since 0.10.0, RFC 0011). In every pass that draws batches (opaque, transparent, refraction, the
 * shadow maps, the radiance cascades' sources), a batch equal to the one the pass drew before it in every field but
 * kind, model, custom_mesh, material_slot, first_instance and instance_count (byte for byte, a float by its bits)
 * binds and pushes nothing but its vertex and index buffers before it draws; so do a model's second and later
 * primitives. A skinned geometry never starts or continues a run. What is drawn doesn't change. A host gains most by
 * submitting each instance buffer's batches grouped, outermost first:
 * 1. by static vertex shader: the batches whose world-detail table has map_layers together, the others together. Each
 *    switch between the two changes the pipeline, and SDL then writes all of its descriptor sets again. A coplanar
 *    overlay still has to follow what it lies on: put the overlays after the batches under them, grouped the same way;
 * 2. by material (every field but the six), mesh after mesh;
 * and by making the meshes drawn so with BTBH_GPU_CUSTOM_MESH_SHARED_BUFFERS, so that neighbours share their vertex
 * and index buffers. The instance buffer is part of the material: a run never spans two. With
 * BTBHGPUScenePost.opaque_sort, the opaque pass draws the batches it may move in its own order.
 * The environment variable BTBH_GPU_SCENE_RUNS, read once a present, is for measuring and debugging them: 0 turns runs
 * off (every batch binds and pushes its whole state); verify (since 0.13.0) keeps them on and checks every draw that
 * continues a run: it builds the uniforms, samplers, storage buffers, pipeline and stencil reference the batch would
 * push and bind without the run and compares them with what the pass holds. Each difference is logged with SDL_Log
 * (the first 64 a window finds), as a line starting "BTBH_GPU_SCENE_RUNS=verify: mismatch" that names the pass, the
 * batch and the field, and a window destroyed after it checked says how many draws it checked and how many
 * mismatches it found. The frames are those with runs on; verify costs the CPU work runs save, unset nothing. */
BTBHGPUResult btbh_gpu_window_scene_present_instances(
    BTBHGPUWindow *window,
    const char *vertex_shader_path,
    const char *animated_vertex_shader_path,
    const char *fast_shadow_vertex_shader_path,
    const char *fast_shadow_fragment_shader_path,
    const char *shadow_vertex_shader_path,
    const char *animated_shadow_vertex_shader_path,
    const char *shadow_fragment_shader_path,
    const char *fragment_shader_path,
    const char *unshadowed_fragment_shader_path,
    const char *post_vertex_shader_path,
    const char *post_fragment_shader_path,
    BTBHGPUShaderFormat shader_format,
    uint32_t render_width,
    uint32_t render_height,
    const float view_projection[16],
    const float camera_right[3],
    const float camera_up[3],
    const float camera_position[3],
    const BTBHGPUSceneLighting *lighting,
    const BTBHGPUScenePost *post,
    const float *instances,
    uint32_t transient_instance_count,
    uint32_t logical_instance_count,
    const BTBHGPUSceneGeometryBatch *batches,
    uint32_t batch_count,
    uint8_t clear_red,
    uint8_t clear_green,
    uint8_t clear_blue,
    uint8_t clear_alpha,
    BTBHGPUSceneReport *report
);
// As btbh_gpu_window_scene_present_instances, with per-batch visibility: scene_visibility
// (drawn in the scene) and directional_shadow_visibility (cast into the directional shadow
// maps), one byte (0 or 1) per batch; NULL means every batch. A batch with
// BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW casts into neither directional map, whatever its byte says.
BTBHGPUResult btbh_gpu_window_scene_present_instances_with_visibility(
    BTBHGPUWindow *window,
    const char *vertex_shader_path,
    const char *animated_vertex_shader_path,
    const char *fast_shadow_vertex_shader_path,
    const char *fast_shadow_fragment_shader_path,
    const char *shadow_vertex_shader_path,
    const char *animated_shadow_vertex_shader_path,
    const char *shadow_fragment_shader_path,
    const char *fragment_shader_path,
    const char *unshadowed_fragment_shader_path,
    const char *post_vertex_shader_path,
    const char *post_fragment_shader_path,
    BTBHGPUShaderFormat shader_format,
    uint32_t render_width,
    uint32_t render_height,
    const float view_projection[16],
    const float camera_right[3],
    const float camera_up[3],
    const float camera_position[3],
    const BTBHGPUSceneLighting *lighting,
    const BTBHGPUScenePost *post,
    const float *instances,
    uint32_t transient_instance_count,
    uint32_t logical_instance_count,
    const BTBHGPUSceneGeometryBatch *batches,
    const uint8_t *scene_visibility,
    const uint8_t *directional_shadow_visibility,
    uint32_t batch_count,
    uint8_t clear_red,
    uint8_t clear_green,
    uint8_t clear_blue,
    uint8_t clear_alpha,
    BTBHGPUSceneReport *report
);
/* Makes at load what the window's scene presents would make the first time they draw (since 0.7.0, asked for by
   Be The Bullet Hell v2). Without it, the first frame of a scene makes every pipeline, shader and target it needs,
   and a later frame stalls where it first draws a new kind of batch or turns a setting on. Call it with the shader
   paths, render size, lighting, post settings and batches the frames will present: those frames then make nothing.
   What it makes:
   - each batch's model pipelines for these settings: with the sun's shadow, the omni spot shadow or none; with the
     radiance cascades or not; with FSR 3's and the SSAO/SSR outputs or not; skinned or not; per blend, depth
     compare, cull mode and stencil use; the fast unlit shader where a batch takes it;
   - the shadow maps and their pipelines; the radiance cascades' and FSR 3's pipelines and resources; SMAA's or
     FXAA's. A device without one falls back as the present would;
   - the scene's targets at render_width x render_height and the window's drawable size; the post chain's pipelines
     (upscaling, SSAO resolve, HBAO, HDR composite and bloom, LUT, CAS, tilt-shift, the final pass); the distance
     pass and the refraction copy where a batch needs them.
   It runs the present's own steps, so the two make the same things. It draws nothing and leaves every frame's
   state alone (FSR 3's history and jitter, the retained shadow maps, the radiance cascades' history): the frames
   are the same to the byte with or without it. What exists is kept, so a second call with the same arguments makes
   nothing.
   - Settings change what a frame needs: call it once for each set the game presents (each quality preset), and
     again with the batches a level adds.
   - A batch is taken as the present takes it, but its instances aren't read: any first_instance and
     instance_count, an empty instance_buffer. Every batch counts, as with a scene_visibility of NULL.
   - The window's frames only, at its drawable size now: after a resize, or at another render size, the next
     present makes its targets anew (the pipelines stay). btbh_gpu_window_scene_render_to_target keeps its own
     targets and pipelines.
   - The first frame or two after it still cost a few milliseconds more than the next ones: the GPU API's first
     use of the new pipelines' render passes, descriptor pools and uniform buffers, which only drawing makes.
   - After a level's load, the first frame that presents also frees every upload buffer the load made (in its submit,
     outside the scene report's split): btbh_gpu_device_set_upload_staging_budget keeps them for reuse instead (and a
     Vulkan device has a budget from the start since 0.13.0).
   - Since 0.13.0, on Vulkan, when the renderer submitted command buffers since the last frame that presented (a
     level's uploads, renders into targets), it waits for the GPU once at its end, so that SDL takes back the
     descriptor-set caches those command buffers held: SDL 3.4 hands each command buffer one and takes them back only
     in a submit that presents or a wait, and without it the first frame after a load drew with an empty cache, making
     15 descriptor pools in its draws (about 0.5 ms; 24 ms once when the driver stalled). The wait lasts as long as the
     GPU still runs what was submitted; with nothing submitted since the last presented frame it doesn't happen.
   - The lazy creation log (btbh_gpu_window_set_lazy_creation_log_enabled, below) names what a frame made that a
     prewarm would have, and what nothing makes ahead.
   - Nothing is kept between runs: SDL 3.4 makes its Vulkan pipelines without a pipeline cache. The driver's own
     shader cache is what makes a later run's creations faster.
   Call it between frames. While a scene waits for its canvas it returns BTBH_GPU_INVALID_ARGUMENT and leaves the
   frame as it was. Arguments the present would refuse, NULL paths, lighting, post or batches, batch_count 0 or a
   size of 0 return BTBH_GPU_INVALID_ARGUMENT. A shader that can't be loaded, or a pipeline the device refuses,
   returns the present's error with the path or SDL's message; what was made before it stays. On success the
   report holds drawable_width/height, batch_count, prepared_geometry_count and upscale_mode (what the frames will
   run: FSR 1 where FSR 3 is unavailable); its timings stay 0. */
BTBHGPUResult btbh_gpu_window_scene_prewarm(
    BTBHGPUWindow *window,
    const char *vertex_shader_path,
    const char *animated_vertex_shader_path,
    const char *fast_shadow_vertex_shader_path,
    const char *fast_shadow_fragment_shader_path,
    const char *shadow_vertex_shader_path,
    const char *animated_shadow_vertex_shader_path,
    const char *shadow_fragment_shader_path,
    const char *fragment_shader_path,
    const char *unshadowed_fragment_shader_path,
    const char *post_vertex_shader_path,
    const char *post_fragment_shader_path,
    BTBHGPUShaderFormat shader_format,
    uint32_t render_width,
    uint32_t render_height,
    const BTBHGPUSceneLighting *lighting,
    const BTBHGPUScenePost *post,
    const BTBHGPUSceneGeometryBatch *batches,
    uint32_t batch_count,
    BTBHGPUSceneReport *report
);
/* Defined when btbh_gpu_window_scene_prewarm exists (since 0.7.0). */
#define BTBH_GPU_HAS_SCENE_PREWARM 1
// The primary directional shadow map's size in texels (256-8192 each way; by default 2048 x 2048), for
// a light-space region that isn't square. Taken at the next scene presented (the map is made anew).
uint8_t btbh_gpu_window_scene_set_directional_shadow_size(
    BTBHGPUWindow *window,
    uint32_t width,
    uint32_t height
);
/* How often the scene redraws each shadow map (public since 0.15.0; the call is older, and the Swift layer's scene
   uses it): every directional_frames frames for the primary directional (sun) map,
   secondary_directional_frames for the secondary directional map, and spot_frames for the spot or omni map. Each is
   1 to 60; 1, the default of a window that never calls this, redraws the map every frame. Returns 1, or 0 and
   changes nothing for a NULL window or a period outside 1-60. Taken from the next scene presented.
   - Between redraws a map is kept, and the scene reads it with the matrix it was drawn with, so its shadows stay
     right for what didn't move and lag a moving caster (or a moved light matrix) by up to period - 1 frames. The
     particle transmittance map (RFC 0013) is redrawn every frame, with the kept sun matrix.
   - The maps take turns: the n-th map a frame uses (in the order primary, secondary, spot; 0 for the first) is
     redrawn on the frames whose count, modulo its period, is n modulo its period. At periods 2, 2 and 1 the two
     directional maps alternate and the spot map is redrawn every frame. (Since 0.16.0, a frame of scene views counts
     a second view's own primary map right after the primary, and its own spot map right after the spot, each with
     that kind's period: btbh_gpu_window_scene_present_views.)
   - A map is redrawn whatever its period on the first frame that uses it after one that didn't, when its size
     changes, when the spot map switches between spot and omni, and after a frame whose submit failed.
   - Renders into a target: btbh_gpu_window_scene_render_to_target redraws every map each call. A target view of
     its own (btbh_gpu_window_scene_render_to_target_view, view_id 1 and up) follows these periods with its own frame
     count. */
uint8_t btbh_gpu_window_scene_set_shadow_update_periods(
    BTBHGPUWindow *window,
    uint32_t directional_frames,
    uint32_t secondary_directional_frames,
    uint32_t spot_frames
);
/* Defined when btbh_gpu_window_scene_set_shadow_update_periods is declared here (since 0.15.0; before, it was in the
   renderer's internal package header only, and a host declared it itself). */
#define BTBH_GPU_SCENE_HAS_SHADOW_UPDATE_PERIODS 1
/* Renders a scene into `target` (a render target of this window, btbh_gpu_window_render_target_create)
   apart from the window's frame, for a picture inside it (a unit's portrait). Arguments as
   btbh_gpu_window_scene_present_instances_with_visibility; the scene renders at render_width x
   render_height and is brought to the target's size: enlarged by post->upscale_mode (Lanczos or FSR 1)
   when it is smaller, else scaled bilinearly without sharpening (render it at twice the target's size
   to supersample it: it is halved exactly).
   - It is never temporal and leaves the window's temporal state alone: FSR 3 runs as FSR 1, the
     temporal_* fields of `post` are ignored, and the window's FSR 3 history, previous view-projection
     and bone palettes, retained shadow maps and radiance-cascade history are neither read nor written.
     The next frame's scene reprojects as if this one had not been drawn. Each call starts afresh
     itself (its shadow maps redrawn whatever the shadow update periods, no history of its own).
   - It has its own scene targets (at its render size), shadow maps and pipelines, made by the first
     call and kept with the window; models, meshes, textures and instance buffers are the window's.
   - Call it when no scene is waiting for its canvas: before the frame's scene present, or after its
     canvas present. It submits its own work; the canvas (or a later scene) can sample `target` in
     the same frame.
   report->drawable_width/height are the target's size. Fails with BTBH_GPU_INVALID_ARGUMENT, leaving
   the window as it was, for a texture that is not this window's render target or while a scene waits
   for its canvas. */
BTBHGPUResult btbh_gpu_window_scene_render_to_target(
    BTBHGPUWindow *window,
    BTBHGPUTexture *target,
    const char *vertex_shader_path,
    const char *animated_vertex_shader_path,
    const char *fast_shadow_vertex_shader_path,
    const char *fast_shadow_fragment_shader_path,
    const char *shadow_vertex_shader_path,
    const char *animated_shadow_vertex_shader_path,
    const char *shadow_fragment_shader_path,
    const char *fragment_shader_path,
    const char *unshadowed_fragment_shader_path,
    const char *post_vertex_shader_path,
    const char *post_fragment_shader_path,
    BTBHGPUShaderFormat shader_format,
    uint32_t render_width,
    uint32_t render_height,
    const float view_projection[16],
    const float camera_right[3],
    const float camera_up[3],
    const float camera_position[3],
    const BTBHGPUSceneLighting *lighting,
    const BTBHGPUScenePost *post,
    const float *instances,
    uint32_t transient_instance_count,
    uint32_t logical_instance_count,
    const BTBHGPUSceneGeometryBatch *batches,
    const uint8_t *scene_visibility,
    const uint8_t *directional_shadow_visibility,
    uint32_t batch_count,
    uint8_t clear_red,
    uint8_t clear_green,
    uint8_t clear_blue,
    uint8_t clear_alpha,
    BTBHGPUSceneReport *report
);
/* btbh_gpu_window_scene_prewarm for btbh_gpu_window_scene_render_to_target into `target` (since 0.9.0, asked for by
   Be The Bullet Hell v2, whose first render to a target in a session, a card's live preview, stalled for 54-56 ms
   while it made that call's runtime). Makes what those renders would make the first time: their own scene runtime,
   each batch's model pipelines for these settings, their shadow maps and the cascades' and SMAA's or FXAA's
   resources, their scene targets at render_width x render_height and their post chain at the target's size. Call
   it with the target and the arguments the renders will take: they then make no shader, pipeline or target.
   - It runs btbh_gpu_window_scene_prewarm's steps with what btbh_gpu_window_scene_render_to_target does to the
     settings first (FSR 3 runs as FSR 1; a render at the target's size or bigger is scaled bilinearly), in that
     call's runtime, so the three can't drift apart. It draws nothing: the window's frames and their runtime are
     left alone, and a render to a target is the same to the byte with or without it.
   - The renders to every target of a window share one runtime: prewarm with the render size and the target size
     they will use. A render at another size makes its targets anew (the pipelines stay).
   - A second call with the same arguments makes nothing. Batches, settings and their limits are as for
     btbh_gpu_window_scene_prewarm: call it once per set of settings.
   Refusals: btbh_gpu_window_scene_prewarm's, and a target btbh_gpu_window_scene_render_to_target refuses (NULL,
   another window's, not a render target, an array), with BTBH_GPU_INVALID_ARGUMENT. On success the report holds
   drawable_width/height (the target's size), batch_count, prepared_geometry_count and upscale_mode (what the
   renders will run); its timings stay 0. */
BTBHGPUResult btbh_gpu_window_scene_prewarm_render_to_target(
    BTBHGPUWindow *window,
    BTBHGPUTexture *target,
    const char *vertex_shader_path,
    const char *animated_vertex_shader_path,
    const char *fast_shadow_vertex_shader_path,
    const char *fast_shadow_fragment_shader_path,
    const char *shadow_vertex_shader_path,
    const char *animated_shadow_vertex_shader_path,
    const char *shadow_fragment_shader_path,
    const char *fragment_shader_path,
    const char *unshadowed_fragment_shader_path,
    const char *post_vertex_shader_path,
    const char *post_fragment_shader_path,
    BTBHGPUShaderFormat shader_format,
    uint32_t render_width,
    uint32_t render_height,
    const BTBHGPUSceneLighting *lighting,
    const BTBHGPUScenePost *post,
    const BTBHGPUSceneGeometryBatch *batches,
    uint32_t batch_count,
    BTBHGPUSceneReport *report
);
/* Defined when btbh_gpu_window_scene_prewarm_render_to_target exists (since 0.9.0). */
#define BTBH_GPU_HAS_SCENE_RENDER_TO_TARGET_PREWARM 1

/* Target views (since 0.15.0, asked for by Be The Bullet Hell v2 for its local co-op split screen, which draws the
   second player's view into a target every frame): btbh_gpu_window_scene_render_to_target with a view, which can keep
   a runtime of its own between calls and shade only a rect of the target.
   - view_id 0: the runtime btbh_gpu_window_scene_render_to_target uses, every call afresh (its portraits).
   - view_id 1 to BTBH_GPU_SCENE_TARGET_VIEW_CAPACITY: a runtime of the view's own, made by its first call (or its
     prewarm) and kept until btbh_gpu_window_scene_release_target_view or the window's destroy. It keeps its
     retained shadow maps and its own shadow frame count, so it follows the window's shadow update periods
     (btbh_gpu_window_scene_set_shadow_update_periods) as the window's frames do, and its radiance cascades keep their
     history (the field is reprojected from the view's last call), as the window's do from frame to frame. It is
     still never temporal: FSR 3 runs as FSR 1, as for every render to a target. A failed submit drops what it kept,
     as a failed frame does for the window's. It never reads or writes the window's runtime or another view's.
   - scissor_width and scissor_height both nonzero: only the rect (scissor_x, scissor_y, scissor_width,
     scissor_height), in the target's pixels (after mirror_horizontal's mirror, where it is set), is shaded. It is
     mapped to the render size (scaled, rounded outward)
     and every pass that writes the scene's targets or the post chain's is limited to it: the opaque, transparent and
     refraction passes, the SSAO and HBAO with their denoise and resolve (with the contact AO and the reflections),
     the distance pass, the radiance cascades' compose, bloom, the HDR composite, LUT, CAS, tilt-shift, the output
     pass, SMAA or FXAA, and FSR 1's EASU and RCAS or Lanczos at the target's size. The shadow maps are never limited,
     nor are the cascades' passes in their field; their per-pixel compute passes run whole, but stop at once where the
     depth outside the rect is the far plane's.
   - Outside the rect the target holds the clear colour (clear_red / 255 and so on, as a clear of the target stores
     it: an 8-bit UNORM target holds these bytes, an sRGB one their sRGB encoding, so clear_red 128 reads back about
     188). The passes that read their neighbours read cleared values there, so a band
     inside the rect's border differs from a render of the whole target: a seam of a few pixels where the
     anti-aliasing, CAS and FSR 1 read the clear (in the renderer's smoke 3 pixels at the render size, 19 target
     pixels when FSR 1 enlarges a half-size render), and a softer band as wide as the SSAO's, HBAO's and bloom's
     reach on screen (at 4K in the renderer's bench: 80 pixels for the HBAO, 300 for bloom, more than 8 of 255 steps
     within 57 and 152). Reflections that leave the rect find nothing there. To keep the band out of what is shown,
     grow the rect beyond it.
   Either size zero: the whole target (x and y are then ignored). reserved: zero. */
#define BTBH_GPU_SCENE_TARGET_VIEW_CAPACITY 8u
typedef struct BTBHGPUSceneTargetView {
    uint32_t view_id;
    uint32_t scissor_x;
    uint32_t scissor_y;
    uint32_t scissor_width;
    uint32_t scissor_height;
    uint32_t reserved[3];
} BTBHGPUSceneTargetView;

/* btbh_gpu_window_scene_render_to_target into `target` with `view` (above). A NULL view is exactly
   btbh_gpu_window_scene_render_to_target, and so is view_id 0 with no rect. Arguments, timing (between frames, while
   no scene waits for its canvas) and refusals are render_to_target's, and with BTBH_GPU_INVALID_ARGUMENT, leaving the
   window as it was: a view_id over BTBH_GPU_SCENE_TARGET_VIEW_CAPACITY, a reserved field that isn't zero, and a rect
   that doesn't fit in the target. */
BTBHGPUResult btbh_gpu_window_scene_render_to_target_view(
    BTBHGPUWindow *window,
    BTBHGPUTexture *target,
    const char *vertex_shader_path,
    const char *animated_vertex_shader_path,
    const char *fast_shadow_vertex_shader_path,
    const char *fast_shadow_fragment_shader_path,
    const char *shadow_vertex_shader_path,
    const char *animated_shadow_vertex_shader_path,
    const char *shadow_fragment_shader_path,
    const char *fragment_shader_path,
    const char *unshadowed_fragment_shader_path,
    const char *post_vertex_shader_path,
    const char *post_fragment_shader_path,
    BTBHGPUShaderFormat shader_format,
    uint32_t render_width,
    uint32_t render_height,
    const float view_projection[16],
    const float camera_right[3],
    const float camera_up[3],
    const float camera_position[3],
    const BTBHGPUSceneLighting *lighting,
    const BTBHGPUScenePost *post,
    const float *instances,
    uint32_t transient_instance_count,
    uint32_t logical_instance_count,
    const BTBHGPUSceneGeometryBatch *batches,
    const uint8_t *scene_visibility,
    const uint8_t *directional_shadow_visibility,
    uint32_t batch_count,
    uint8_t clear_red,
    uint8_t clear_green,
    uint8_t clear_blue,
    uint8_t clear_alpha,
    const BTBHGPUSceneTargetView *view,
    BTBHGPUSceneReport *report
);
/* btbh_gpu_window_scene_prewarm_render_to_target for renders with `view`: in the view's runtime (view_id 1 and up:
   made here if it doesn't exist yet), and with what a rect needs besides. A NULL view, or view_id 0 without a rect, is
   exactly btbh_gpu_window_scene_prewarm_render_to_target; view_id 0 with a rect also makes the rect's 1x1 fill
   texture (in the target's format) in render_to_target's runtime. Refusals: its own, and those of
   btbh_gpu_window_scene_render_to_target_view for the view. */
BTBHGPUResult btbh_gpu_window_scene_prewarm_render_to_target_view(
    BTBHGPUWindow *window,
    BTBHGPUTexture *target,
    const char *vertex_shader_path,
    const char *animated_vertex_shader_path,
    const char *fast_shadow_vertex_shader_path,
    const char *fast_shadow_fragment_shader_path,
    const char *shadow_vertex_shader_path,
    const char *animated_shadow_vertex_shader_path,
    const char *shadow_fragment_shader_path,
    const char *fragment_shader_path,
    const char *unshadowed_fragment_shader_path,
    const char *post_vertex_shader_path,
    const char *post_fragment_shader_path,
    BTBHGPUShaderFormat shader_format,
    uint32_t render_width,
    uint32_t render_height,
    const BTBHGPUSceneLighting *lighting,
    const BTBHGPUScenePost *post,
    const BTBHGPUSceneGeometryBatch *batches,
    uint32_t batch_count,
    const BTBHGPUSceneTargetView *view,
    BTBHGPUSceneReport *report
);
/* Frees the runtime a target view keeps (its scene targets, shadow maps, pipelines and history); its next render makes
   it again, afresh. view_id 0 frees the runtime btbh_gpu_window_scene_render_to_target uses. A view without a runtime
   is left as it is. The window's frame, a scene waiting for its canvas included, isn't touched. A NULL window or a
   view_id over BTBH_GPU_SCENE_TARGET_VIEW_CAPACITY returns BTBH_GPU_INVALID_ARGUMENT. */
BTBHGPUResult btbh_gpu_window_scene_release_target_view(
    BTBHGPUWindow *window,
    uint32_t view_id
);
/* Defined when BTBHGPUSceneTargetView and its three functions exist (since 0.15.0). */
#define BTBH_GPU_SCENE_HAS_TARGET_VIEWS 1

/* Scene views (since 0.16.0, RFC 0014, asked for by Be The Bullet Hell v2 for its local co-op split screen, which drew
   its second player's view into a target and laid it over the frame): one present that draws up to
   BTBH_GPU_SCENE_MAX_VIEWS cameras of the same batches and instances into the window's frame, each clipped to its half
   of it. The preparation, the upload and the post chain run once; each view draws its own geometry and its own
   screen-space passes. */
#define BTBH_GPU_SCENE_MAX_VIEWS 2u

/* BTBHGPUSceneView.flags. */
enum {
    /* A cut in this view alone (a player put elsewhere). Reserved for FSR 3 over two views (RFC 0014 phase 2): in this
     * release a frame of two views never runs FSR 3, so the flag changes nothing; it is accepted. */
    BTBH_GPU_SCENE_VIEW_TEMPORAL_RESET = 1u << 0
};

/* One view of a scene presented by btbh_gpu_window_scene_present_views: a camera and the part of the frame it covers.
   144 bytes on 64-bit: view_projection 0, right 64, up 76, position 88, clip 100, visibility 112, shadow_visibility 120,
   lighting 128, flags 136, reserved 140. A zeroed view with a valid camera is the whole frame, every batch the call lets
   in and the call's lighting. */
typedef struct BTBHGPUSceneView {
    /* The camera, as btbh_gpu_window_scene_present_instances' view_projection, camera_right, camera_up and
       camera_position. */
    float view_projection[16];
    float right[3];
    float up[3];
    float position[3];
    /* The view's half of the frame, in the window's drawable pixels (x right, y down from the top-left corner, before
       post->mirror_horizontal mirrors the frame): a pixel centre (x, y) is the view's where
       clip[0] * x + clip[1] * y + clip[2] >= 0. All zero: the whole frame (one view only); clip[0] and clip[1] 0 with
       clip[2] not is refused. With two views the halves should cover the frame once (the second view's clip the
       first's negated): each is widened by a quarter of a render pixel, so that no pixel is drawn by neither. A pixel
       whose centre lies within a quarter pixel of the line is drawn by both, the band by the line: none on a line
       along pixel edges, a column or row on one through pixel centres, about one pixel a row (or column) on a
       diagonal. There the nearer opaque surface shows, and the screen-space passes that write a pixel take the second
       view's; but both views' transparent and additive surfaces blend into it, and the HBAO's occlusion (which
       darkens by blending) can darken it twice. The screen-space passes' reads leave the band out: each view's
       samples come from pixels and texels it alone drew. */
    float clip[3];
    /* Per batch, 0 or 1: drawn in this view (the call's scene_visibility must let it in too). NULL: every batch the
       call's scene_visibility lets in. */
    const uint8_t *visibility;
    /* Per batch, 0 or 1: cast into this view's own sun and spot maps (below). NULL: the call's
       directional_shadow_visibility for the sun map, every batch with spot_shadow_visible for the spot map. A map two
       views share takes the call's (directional_shadow_visibility, and spot_shadow_visible alone). A batch with
       BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW still casts into no sun map. */
    const uint8_t *shadow_visibility;
    /* NULL: the call's lighting. Otherwise it is read only for:
       - directional_shadow_view_projection: the sun map's matrix. A view whose matrix equals view 0's byte for byte
         reads view 0's map; else it has a map of its own (with the sun map's size, and its own particle transmittance
         map), drawn from its own camera and shadow_visibility;
       - the spot light: spot_position_*, spot_range, spot_direction_*, spot_energy, spot_red, _green, _blue,
         spot_falloff, spot_inner_cutoff, spot_outer_cutoff, spot_occluder_shadow_strength, spot_occluder_shadow_reach,
         spot_occluder_flags, spot_shadow_softness, spot_shadow_near and spot_shadow_view_projection, whose map is shared
         or the view's own by the same rule. Whether the spot casts a shadow, and whether it is omni, are the call's
         (spot_shadow_enabled, spot_shadow_is_omni). An omni map is always the call's: drawn from the call's
         omni_shadow_view_projection and spot position, and shared by the views;
       - lamp_count, lamp_core_radius, lamp_position_reach and lamp_color_energy;
       - point_light_count and point_lights.
       Every other field is the call's (the sky, the sun's direction and colour, the fill, the clouds, the fog, the
       shadows' settings): the view's isn't read. It must still be a valid lighting. */
    const BTBHGPUSceneLighting *lighting;
    uint32_t flags;      /* BTBH_GPU_SCENE_VIEW_* bits; 0 as before */
    uint32_t reserved;   /* 0 */
} BTBHGPUSceneView;

/* What one view drew (btbh_gpu_window_scene_present_views' view_reports). 32 bytes. */
typedef struct BTBHGPUSceneViewReport {
    /* Its draws in the opaque, transparent and refraction passes. */
    uint32_t primitive_draw_count;
    /* The instances of the batches it drew (each batch's instance_count, once a batch, after the host's culling and
       visibility). */
    uint32_t instance_count;
    /* The view whose sun map it read: itself, or view 0 when its matrix is view 0's. UINT32_MAX without the sun's
       shadow. */
    uint32_t sun_map;
    /* The same for the spot (or omni) map: 0 for an omni map. UINT32_MAX without the spot's shadow. */
    uint32_t spot_map;
    /* Its opaque order's sort (BTBHGPUScenePost.opaque_sort; 0 without one) and its draws' encoding, on the CPU. */
    double cpu_opaque_order_milliseconds;
    double cpu_geometry_encode_milliseconds;
} BTBHGPUSceneViewReport;

/* Presents one frame of up to BTBH_GPU_SCENE_MAX_VIEWS views of the same batches and instances, each clipped to its half
   of the frame. Arguments as btbh_gpu_window_scene_present_instances_with_visibility, with the camera replaced by views
   (view_count of them); the frame then waits for its canvas as a present's does. One view whose clip is zero and whose
   visibility, shadow_visibility and lighting are NULL draws that call's frame with the view's camera, byte for byte.
   - Once for the frame: the preparation, the upload, the shadow maps the views share, bloom, the LUT, CAS, tilt-shift
     and the output pass, SMAA or FXAA, the mirror and the upscaling. Bloom reaches a little across the line between
     the views (up to 1/64 of the frame at its widest level), and the anti-aliasing and FSR 1 a few pixels.
   - Per view, clipped to its half (each geometry pass and screen-space pass holds every view in turn): the opaque
     sort, the opaque, transparent and refraction passes, and the passes that read the camera, each keeping its
     samples to its own half: the SSAO or HBAO with their denoise and resolve (taps across the line, or in the band
     by it that both views draw, count for nothing),
     the reflections (a ray stops at the line, as at the screen's edge, and fades near it), the distance pass and the
     HDR composite (the distance fog and the world-edge fade at each view's depth; the vignette stays centred on the
     frame). A refracting surface near the line may sample the other view's pixels as far as it displaces them.
   - Shadow maps: the secondary (fill) map is the call's, shared. A sun or spot map is shared by views whose matrices
     are equal, else each view has its own. Maps take turns under btbh_gpu_window_scene_set_shadow_update_periods: a
     view's own sun map counts right after view 0's in the order the periods use (primary, the views' own primary,
     secondary, spot, the views' own spot), so at a sun period of 2 the two sun maps are redrawn on alternate frames.
     Billboard casters face view 0's camera in a shared map and their own view's in an own map. A second view's own
     maps cost 16 MiB (a 2048 sun map), 4 MiB (the spot map) and its transmittance map.
   - With two views (or one whose clip isn't zero) the frame runs no radiance cascades (report.radiance_cascades:
     active_tier BTBH_GPU_SCENE_RC_OFF, fallback_reason BTBH_GPU_SCENE_RC_FALLBACK_VIEWS) and never FSR 3: FSR 1 runs
     instead (report.upscale_mode says BTBH_GPU_SCENE_UPSCALE_FSR1); the next frame that runs the cascades or FSR 3
     starts a new history (a refused present, or a prewarm, leaves the histories as they were).
   - Needs this release's scene vertex shaders (production_model.vert and its siblings, production_animated_model.vert:
     they write the clip distance) and production_post_view.vert beside the host's post vertex shader. With two views
     (or a clip), SPIR-V vertex shaders that don't write a clip distance are refused with BTBH_GPU_INVALID_ARGUMENT
     naming the path, as is a missing production_post_view.vert; Metal libraries aren't checked. One view with a zero
     clip draws exactly as the present does and needs neither.
   - report: the whole frame's, as the present's: its primitive_draw_count is every view's. view_reports, when not
     NULL, takes view_count entries: what each view drew.
   - Refused with BTBH_GPU_INVALID_ARGUMENT, naming the view, the window left as it was: views NULL, view_count 0 or
     above BTBH_GPU_SCENE_MAX_VIEWS; a view's view_projection, right, up or position not finite, or its view_projection
     not invertible; its clip not finite, or clip[0] and clip[1] both 0 with two views, or with clip[2] not 0 (one
     view); a visibility or shadow_visibility byte above 1; an unknown flag, or reserved not 0; a lighting the present
     would refuse; a lifted batch (height_lift with a heightfield) that would cast into a view's own sun map by its
     shadow_visibility byte, as by directional_shadow_visibility's; and every refusal of
     btbh_gpu_window_scene_present_instances_with_visibility.
   - The window's frame only: a picture in a target stays one view (btbh_gpu_window_scene_render_to_target_view). */
BTBHGPUResult btbh_gpu_window_scene_present_views(
    BTBHGPUWindow *window,
    const char *vertex_shader_path,
    const char *animated_vertex_shader_path,
    const char *fast_shadow_vertex_shader_path,
    const char *fast_shadow_fragment_shader_path,
    const char *shadow_vertex_shader_path,
    const char *animated_shadow_vertex_shader_path,
    const char *shadow_fragment_shader_path,
    const char *fragment_shader_path,
    const char *unshadowed_fragment_shader_path,
    const char *post_vertex_shader_path,
    const char *post_fragment_shader_path,
    BTBHGPUShaderFormat shader_format,
    uint32_t render_width,
    uint32_t render_height,
    const BTBHGPUSceneView *views,
    uint32_t view_count,
    const BTBHGPUSceneLighting *lighting,
    const BTBHGPUScenePost *post,
    const float *instances,
    uint32_t transient_instance_count,
    uint32_t logical_instance_count,
    const BTBHGPUSceneGeometryBatch *batches,
    const uint8_t *scene_visibility,
    const uint8_t *directional_shadow_visibility,
    uint32_t batch_count,
    uint8_t clear_red,
    uint8_t clear_green,
    uint8_t clear_blue,
    uint8_t clear_alpha,
    BTBHGPUSceneViewReport *view_reports,
    BTBHGPUSceneReport *report
);

/* btbh_gpu_window_scene_prewarm for btbh_gpu_window_scene_present_views with these views: its steps with the settings a
   frame of these views runs (no cascades and FSR 1 for two views), and besides, for two views (or a clip), the clipped
   screen-space passes' pipelines (production_post_view.vert with the SSAO's, HBAO's, distance and composite fragment
   shaders, those the settings use) and the views' own shadow maps and transmittance maps, as the present decides them
   from the views' lighting (the matrices are read only to tell which maps the views share). One view with a zero clip
   is exactly btbh_gpu_window_scene_prewarm. It draws nothing and leaves the frames' state alone (the radiance
   cascades' and FSR 3's histories, the retained shadow maps). Refusals: btbh_gpu_window_scene_prewarm's, and the views'
   as btbh_gpu_window_scene_present_views refuses them (their visibility isn't read). */
BTBHGPUResult btbh_gpu_window_scene_prewarm_views(
    BTBHGPUWindow *window,
    const char *vertex_shader_path,
    const char *animated_vertex_shader_path,
    const char *fast_shadow_vertex_shader_path,
    const char *fast_shadow_fragment_shader_path,
    const char *shadow_vertex_shader_path,
    const char *animated_shadow_vertex_shader_path,
    const char *shadow_fragment_shader_path,
    const char *fragment_shader_path,
    const char *unshadowed_fragment_shader_path,
    const char *post_vertex_shader_path,
    const char *post_fragment_shader_path,
    BTBHGPUShaderFormat shader_format,
    uint32_t render_width,
    uint32_t render_height,
    const BTBHGPUSceneView *views,
    uint32_t view_count,
    const BTBHGPUSceneLighting *lighting,
    const BTBHGPUScenePost *post,
    const BTBHGPUSceneGeometryBatch *batches,
    uint32_t batch_count,
    BTBHGPUSceneReport *report
);
/* Defined when btbh_gpu_window_scene_present_views, btbh_gpu_window_scene_prewarm_views and their structs exist (since
   0.16.0). */
#define BTBH_GPU_SCENE_HAS_VIEWS 1

/* The lazy creation log (since 0.11.0, asked for by Be The Bullet Hell v2): what a window's scene presents, renders to
   a target and canvas presents made because nothing had made it before, with how long each took, and the SDL calls
   in them that took 1 ms or more over their usual time. It names what a prewarm missed. Off by default: turn it on
   per window with btbh_gpu_window_set_lazy_creation_log_enabled, or for every window with the environment variable
   BTBH_GPU_LOG_LAZY_CREATION=1, which also prints each entry with SDL_Log as one line starting "BTBH_GPU_LAZY".
   Off, nothing is timed or kept, and on or off the frames are the same to the byte.
   - Recorded: shaders (the file read and the module), graphics and compute pipelines, samplers, textures (the scene's
     targets, the shadow maps, lookup tables, FSR 3's and the radiance cascades' resources), buffers and transfer
     buffers made or grown, each with its CPU time; and the slow calls: the scene's and the canvas's command buffer
     and upload buffer, the swapchain image, the frame's submit and a render to a target's submit.
   - prewarm_covers 1: btbh_gpu_window_scene_prewarm (for a render to a target, its _render_to_target form) called
     with the frame's shader paths, settings, batches and render size, at the window's drawable size, makes it. Its
     shaders, pipelines, samplers and textures in a scene present or a render to a target. A texture made again
     after a window resize or at another render size counts too: only a prewarm after the change would have made it.
   - prewarm_covers 0: no prewarm makes it. The buffers that grow with a frame's instances, bones, motions, canvas
     vertices and the cascades' emitters and occlusion; everything the canvas makes (its pipelines are made by the
     first canvas present, those for font atlases by the first batch that draws one); the slow calls. SDL's own
     lazy work (its descriptor pools and uniform buffers, its deferred releases and memory clean-up on a presenting
     submit) shows only as a slow call around it.
   - Only the thread that presents is recorded; work on other threads (loading) isn't. */
typedef enum BTBHGPULazyCreationKind {
    BTBH_GPU_LAZY_CREATION_SHADER = 1,
    BTBH_GPU_LAZY_CREATION_GRAPHICS_PIPELINE = 2,
    BTBH_GPU_LAZY_CREATION_COMPUTE_PIPELINE = 3,
    BTBH_GPU_LAZY_CREATION_SAMPLER = 4,
    BTBH_GPU_LAZY_CREATION_TEXTURE = 5,
    BTBH_GPU_LAZY_CREATION_BUFFER = 6,
    BTBH_GPU_LAZY_CREATION_TRANSFER_BUFFER = 7,
    BTBH_GPU_LAZY_CREATION_SLOW_CALL = 8
} BTBHGPULazyCreationKind;

/* The call an entry happened in. */
typedef enum BTBHGPULazyCreationCall {
    BTBH_GPU_LAZY_CREATION_CALL_SCENE_PRESENT = 1,
    BTBH_GPU_LAZY_CREATION_CALL_RENDER_TO_TARGET = 2,
    BTBH_GPU_LAZY_CREATION_CALL_CANVAS_PRESENT = 3
} BTBHGPULazyCreationCall;

#define BTBH_GPU_LAZY_CREATION_NAME_CAPACITY 232u
/* How many entries a window keeps between two reports: when more are recorded, the oldest are dropped (a gap in
   sequence shows where). */
#define BTBH_GPU_LAZY_CREATION_WINDOW_CAPACITY 128u

typedef struct BTBHGPULazyCreationEntry {
    /* The frames the window had submitted before this one (its canvas presents): a scene present, a render to a
       target and the canvas present of one frame share it. */
    uint64_t frame_index;
    /* The entry's number among the window's, from 0 when its log was first turned on. */
    uint64_t sequence;
    /* The creation's CPU time (a shader's includes reading its file), or the slow call's. */
    uint32_t microseconds;
    uint8_t kind;           /* BTBHGPULazyCreationKind */
    uint8_t call;           /* BTBHGPULazyCreationCall */
    uint8_t prewarm_covers; /* 1 when a prewarm makes it (see above) */
    uint8_t reserved;
    /* What was made, NUL-terminated: a pipeline by its key (its vertex and fragment shader files, colour targets'
       formats and blending, depth-stencil format, depth test, stencil, cull mode), a shader by its file, a texture
       by its size, format and use, a buffer by its size and use, a slow call by its SDL function and its usual
       time; then where in the renderer. Cut at the capacity. */
    char name[BTBH_GPU_LAZY_CREATION_NAME_CAPACITY];
} BTBHGPULazyCreationEntry;

/* Turns the window's lazy creation log on (enabled 1) or off (0); turned off, it keeps what it recorded until the
   next report. BTBH_GPU_LOG_LAZY_CREATION=1 records every window whatever this says. A NULL window does nothing. */
void btbh_gpu_window_set_lazy_creation_log_enabled(
    BTBHGPUWindow *window,
    uint8_t enabled
);
/* Copies the oldest entries the window recorded since the last report, at most `capacity`, into `entries`, and
   forgets them; *count is how many it copied (0 when there are none, or the log was never on). Call it again while
   *count equals capacity to take the rest. NULL window or count, or NULL entries with a capacity, return
   BTBH_GPU_INVALID_ARGUMENT. */
BTBHGPUResult btbh_gpu_window_lazy_creation_report(
    BTBHGPUWindow *window,
    BTBHGPULazyCreationEntry *entries,
    uint32_t capacity,
    uint32_t *count
);
/* Defined when the lazy creation log exists (since 0.11.0). */
#define BTBH_GPU_HAS_LAZY_CREATION_LOG 1
BTBHGPUAnimationLibrary *btbh_gpu_animation_library_load(
    const char *path,
    BTBHGPUAnimationLibraryReport *report
);
float btbh_gpu_animation_library_duration(
    const BTBHGPUAnimationLibrary *library,
    uint32_t animation_index
);
BTBHGPUAnimationPose *btbh_gpu_animation_pose_create(
    const BTBHGPUAnimationLibrary *library
);
uint8_t btbh_gpu_animation_pose_set_samples(
    BTBHGPUAnimationPose *pose,
    const uint32_t *animation_indices,
    const float *animation_times,
    const float *animation_weights,
    uint32_t sample_count
);
void btbh_gpu_animation_pose_destroy(
    BTBHGPUAnimationPose *pose
);
void btbh_gpu_animation_library_destroy(
    BTBHGPUAnimationLibrary *library
);
void btbh_gpu_window_destroy(BTBHGPUWindow *window);

/* Raw-pixel textures. Pixels are tightly packed RGBA8, row 0 at the top.
 * generate_mipmaps: 0 none, 1 the full chain, n > 1 at most n levels. */
BTBHGPUTexture *btbh_gpu_window_texture_create_rgba8_ex(
    BTBHGPUWindow *window,
    const uint8_t *pixels,
    uint32_t width,
    uint32_t height,
    uint8_t generate_mipmaps,
    BTBHGPUTextureFilter filter,
    BTBHGPUTextureWrap wrap,
    BTBHGPUTextureColorSpace color_space,
    BTBHGPUTextureReport *report
);
/* A 2D array texture (RFC 0001's functions, since 0.8.0): layer_count layers of one size and colour space, each
 * with its own mips, sampled through the slots that take an array (BTBHGPUSceneWorldDetailTable.map_layers). Every
 * other texture slot (a batch's four, a world detail's three, the canvas's texture and target, the post LUT, the
 * capture destination, the render-to-target target) refuses one with BTBH_GPU_INVALID_ARGUMENT, and so does
 * btbh_gpu_texture_update_rgba8. btbh_gpu_texture_destroy frees it. The report's width, height and mip_count
 * describe one layer.
 * - layer_pixels: layer_count pointers, each to width * height * 4 bytes (tightly packed RGBA8, row 0 at the top).
 * - layer_count: 1..BTBH_GPU_TEXTURE_ARRAY_MAX_LAYERS; generate_mipmaps as btbh_gpu_window_texture_create_rgba8_ex
 *   (every layer gets its own chain).
 * - A NULL or out-of-range argument, a NULL layer pointer, or more than 4 GiB to upload: NULL with
 *   BTBH_GPU_INVALID_ARGUMENT and the reason in the report (when there is a report). A device that can't sample a
 *   2D array in the format: BTBH_GPU_MISSING_CAPABILITY. */
#define BTBH_GPU_TEXTURE_ARRAY_MAX_LAYERS 256u
BTBHGPUTexture *btbh_gpu_window_texture_array_create_rgba8_ex(
    BTBHGPUWindow *window,
    const uint8_t *const *layer_pixels,
    uint32_t width,
    uint32_t height,
    uint32_t layer_count,
    uint8_t generate_mipmaps,
    BTBHGPUTextureFilter filter,
    BTBHGPUTextureWrap wrap,
    BTBHGPUTextureColorSpace color_space,
    BTBHGPUTextureReport *report
);
/* The same from image files (PNG, JPG, ...), one per layer, in order, decoded to RGBA8. Every image must have the
 * first one's size: otherwise NULL with BTBH_GPU_INVALID_ARGUMENT, naming the path. A file that can't be read or
 * decoded fails as with btbh_gpu_window_texture_load_ex (BTBH_GPU_DEVICE_FAILURE), and the message names the
 * path. */
BTBHGPUTexture *btbh_gpu_window_texture_array_load_ex(
    BTBHGPUWindow *window,
    const char *const *layer_paths,
    uint32_t layer_count,
    uint8_t generate_mipmaps,
    BTBHGPUTextureFilter filter,
    BTBHGPUTextureWrap wrap,
    BTBHGPUTextureColorSpace color_space,
    BTBHGPUTextureReport *report
);
/* Defined when the two functions above and BTBH_GPU_TEXTURE_ARRAY_MAX_LAYERS exist (since 0.8.0). */
#define BTBH_GPU_HAS_TEXTURE_ARRAYS 1
/* A heightfield for BTBHGPUSceneHeightLift (RFC 0012, since 0.11.0): width x height samples, row by row along u
 * (heights[j * width + i]), rows along v; sample (i, j) sits at the texel centre ((i + 0.5) / width,
 * (j + 0.5) / height), so a host whose lattice is (x0 + i * step, z0 + j * step) maps it with
 * world_to_uv = { 1 / (width * step), 1 / (height * step), 0.5 / width - x0 / (width * step),
 * 0.5 / height - z0 / (height * step) }. Kept on the GPU as RGBA16F: the height and its slope along u and v (central
 * differences in heights per sample, one-sided at the edges), filtered bilinearly and clamped at the edges, one mip.
 * The renderer keeps the heights on the CPU too (the opaque sort lifts a lifted batch's depth with them). Heights
 * are stored as half floats: within 8 to 16 units they are rounded to 1/128, so give values within a few hundred
 * units, or normalise them and use height_scale. Only BTBHGPUSceneHeightLift.heightfield takes one: every other
 * texture slot refuses it with BTBH_GPU_INVALID_ARGUMENT (as an array texture), and so does
 * btbh_gpu_texture_update_rgba8. btbh_gpu_texture_destroy frees it. width and height: 2..BTBH_GPU_HEIGHTFIELD_MAX_SIZE.
 * A NULL or out-of-range argument, or a height that isn't finite or lies past +-32768 (halves hold the heights and
 * their differences; the message names the sample): NULL with BTBH_GPU_INVALID_ARGUMENT; a device that can't sample
 * RGBA16F: BTBH_GPU_MISSING_CAPABILITY. report must not be NULL; its width and height are the field's, its mip_count
 * 1. */
BTBHGPUTexture *btbh_gpu_window_heightfield_create(
    BTBHGPUWindow *window,
    const float *heights,
    uint32_t width,
    uint32_t height,
    BTBHGPUTextureReport *report
);
/* Replaces a region of a raw-pixel texture. Submits its own upload, so call it
 * before the scene or canvas present that samples the texture. A one-channel font atlas
 * (BTBH_GPU_FONT_FLAG_R8_ATLAS) isn't RGBA, an array texture has layers and a heightfield holds halves:
 * BTBH_GPU_INVALID_ARGUMENT. */
BTBHGPUResult btbh_gpu_texture_update_rgba8(
    BTBHGPUTexture *texture,
    const uint8_t *pixels,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height
);
/* The SDL_Window behind a BTBH window, for hosts that drive SDL themselves. */
struct SDL_Window *btbh_gpu_window_sdl_window(BTBHGPUWindow *window);

#if defined(__cplusplus)
}
#endif

#include "btbh_gpu_sdf_terrain.h"

#endif
